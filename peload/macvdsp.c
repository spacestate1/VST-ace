/* Accelerate/vDSP for the Mach-O loader.
 *
 * Only what the corpus actually imports, which is a small and well-defined slice
 * of vDSP: one real FFT, the split-complex packing helpers, and a set of vector
 * kernels. Written straightforwardly rather than vectorised -- correctness
 * first, and a plugin doing FFT work is dominated by the transform anyway.
 *
 * The two conventions that matter, because getting either wrong sounds like a
 * broken plugin rather than an error:
 *
 *  - Split complex. vDSP keeps real and imaginary parts in separate arrays
 *    (DSPSplitComplex), not interleaved. vDSP_ctoz packs an interleaved buffer
 *    into that form and vDSP_ztoc unpacks it. For a real-to-complex transform of
 *    n points, the n/2 split-complex elements hold n/2+1 logical bins, with the
 *    two purely-real ones folded together: realp[0] is DC and imagp[0] is
 *    Nyquist.
 *
 *  - Scaling. vDSP_fft_zrip forward produces twice the textbook DFT, and the
 *    inverse leaves a factor of n behind; Apple documents the caller as applying
 *    1/(2n) on the round trip. Both are reproduced here, because plugins are
 *    written against that behaviour and compensate for it themselves.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "macshim.h"

typedef struct { float *realp, *imagp; } DSPSplitComplex;
typedef struct { double *realp, *imagp; } DSPDoubleSplitComplex;
typedef unsigned long vDSP_Length;
typedef long          vDSP_Stride;

#define FFT_FORWARD  1
#define FFT_INVERSE (-1)

/* ------------------------------------------------------------------- setup */

/* A setup holds the twiddle factors and a bit-reversal table for every size up
 * to the one requested, which is what vDSP promises: one setup serves any
 * log2n at or below the maximum. */
typedef struct {
    unsigned long  maxlog2;
    float         *cos_tab, *sin_tab;      /* half-turn twiddles, n/2 entries */
    unsigned long  n;
} fftsetup;

static void *vd_create_fftsetup(vDSP_Length log2n, int radix)
{
    fftsetup *s = calloc(1, sizeof *s);
    unsigned long n, i;
    (void)radix;
    if (!s) return NULL;
    if (log2n > 24) log2n = 24;
    s->maxlog2 = log2n;
    n = 1ul << log2n;
    s->n = n;
    s->cos_tab = malloc((n / 2 + 1) * sizeof *s->cos_tab);
    s->sin_tab = malloc((n / 2 + 1) * sizeof *s->sin_tab);
    if (!s->cos_tab || !s->sin_tab) {
        free(s->cos_tab); free(s->sin_tab); free(s);
        return NULL;
    }
    for (i = 0; i <= n / 2; i++) {
        double a = -2.0 * M_PI * (double)i / (double)n;
        s->cos_tab[i] = (float)cos(a);
        s->sin_tab[i] = (float)sin(a);
    }
    return s;
}

static void vd_destroy_fftsetup(void *p)
{
    fftsetup *s = p;
    if (!s) return;
    free(s->cos_tab); free(s->sin_tab); free(s);
}

/* ------------------------------------------------------- complex FFT core */

/* In-place radix-2 decimation-in-time on split arrays. `sign` is -1 forward
 * (matching e^{-i2πkn/N}) and +1 inverse; no scaling is applied here. */
static void fft_split(float *re, float *im, unsigned long n, int sign)
{
    unsigned long i, j, m, mmax, step;

    /* bit reversal */
    for (i = 1, j = 0; i < n; i++) {
        unsigned long bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (mmax = 1; mmax < n; mmax <<= 1) {
        step = mmax << 1;
        for (m = 0; m < mmax; m++) {
            double a = (double)sign * M_PI * (double)m / (double)mmax;
            float wr = (float)cos(a), wi = (float)sin(a);
            for (i = m; i < n; i += step) {
                unsigned long k = i + mmax;
                float tr = wr * re[k] - wi * im[k];
                float ti = wr * im[k] + wi * re[k];
                re[k] = re[i] - tr; im[k] = im[i] - ti;
                re[i] += tr;        im[i] += ti;
            }
        }
    }
}

/* vDSP_fft_zrip: real-to-complex (forward) or complex-to-real (inverse), with
 * the DC/Nyquist folding and the factor-of-2 scaling described at the top. */
static void vd_fft_zrip(void *setup, DSPSplitComplex *io, vDSP_Stride stride,
                        vDSP_Length log2n, int direction)
{
    unsigned long n = 1ul << log2n;       /* logical real length */
    unsigned long h = n / 2;              /* split-complex elements */
    float *re, *im;
    unsigned long k;

    (void)setup;
    if (!io || !io->realp || !io->imagp || stride != 1 || h == 0) return;
    re = malloc(h * sizeof *re);
    im = malloc(h * sizeof *im);
    if (!re || !im) { free(re); free(im); return; }

    if (direction == FFT_FORWARD) {
        /* The caller packed even samples in realp and odd in imagp (that is what
         * vDSP_ctoz does), so an h-point complex FFT of that sequence yields the
         * n-point real spectrum after one untangling pass. */
        memcpy(re, io->realp, h * sizeof *re);
        memcpy(im, io->imagp, h * sizeof *im);
        fft_split(re, im, h, -1);

        {   /* untangle: Z[k] -> X[k] for k in 0..h-1, folding DC and Nyquist */
            float dc  = re[0] + im[0];
            float nyq = re[0] - im[0];
            for (k = 1; k <= h / 2; k++) {
                unsigned long kc = h - k;
                float ar = re[k],  ai = im[k];
                float br = re[kc], bi = -im[kc];
                float sr = 0.5f * (ar + br), si = 0.5f * (ai + bi);
                float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
                /* multiply the difference by -i*e^{-i pi k / h} */
                double a = -M_PI * (double)k / (double)h;
                float wr = (float)cos(a), wi = (float)sin(a);
                float tr = dr * wr - di * wi;
                float ti = dr * wi + di * wr;
                float xr = sr + ti,  xi = si - tr;
                float yr = sr - ti,  yi = -(si + tr);
                io->realp[k]  = 2.0f * xr;  io->imagp[k]  = 2.0f * xi;
                if (kc != k) { io->realp[kc] = 2.0f * yr; io->imagp[kc] = 2.0f * yi; }
            }
            io->realp[0] = 2.0f * dc;
            io->imagp[0] = 2.0f * nyq;
        }
    } else {
        /* Inverse. The forward direction uses the half-length trick, but
         * inverting that in place means untangling E and O with four sign
         * conventions to get right at once, and getting one wrong produces a
         * mirrored spectrum that still round-trips magnitudes -- so it survives
         * casual testing. Reconstructing the full n-point conjugate-symmetric
         * spectrum and transforming that is unambiguous, at the cost of one
         * n-point transform instead of n/2. The FFT is not the expensive part of
         * a spectral plugin's frame.
         *
         * Scaling: the caller is documented to apply 1/(2n), so the output here
         * is 2n*x -- an unnormalised inverse transform (which carries a factor
         * of n) times two. */
        float *fr = calloc(n, sizeof *fr), *fi = calloc(n, sizeof *fi);
        if (!fr || !fi) { free(fr); free(fi); free(re); free(im); return; }

        fr[0] = 0.5f * io->realp[0];              /* DC   */
        fr[h] = 0.5f * io->imagp[0];              /* Nyquist */
        for (k = 1; k < h; k++) {
            float xr = 0.5f * io->realp[k], xi = 0.5f * io->imagp[k];
            fr[k]     = xr;  fi[k]     =  xi;
            fr[n - k] = xr;  fi[n - k] = -xi;     /* conjugate mirror */
        }
        fft_split(fr, fi, n, +1);
        for (k = 0; k < h; k++) {
            io->realp[k] = 2.0f * fr[2 * k];
            io->imagp[k] = 2.0f * fr[2 * k + 1];
        }
        free(fr); free(fi);
    }
    free(re); free(im);
}

/* ------------------------------------------------- split-complex packing */

/* Interleaved -> split. vDSP counts `size` in split-complex elements, and the
 * input stride is in floats, so the classic call packs 2n floats into n slots. */
static void vd_ctoz(const float *c, vDSP_Stride cs, DSPSplitComplex *z,
                    vDSP_Stride zs, vDSP_Length size)
{
    vDSP_Length i;
    if (!c || !z) return;
    for (i = 0; i < size; i++) {
        z->realp[i * zs] = c[i * cs];
        z->imagp[i * zs] = c[i * cs + 1];
    }
}

static void vd_ztoc(const DSPSplitComplex *z, vDSP_Stride zs, float *c,
                    vDSP_Stride cs, vDSP_Length size)
{
    vDSP_Length i;
    if (!c || !z) return;
    for (i = 0; i < size; i++) {
        c[i * cs]     = z->realp[i * zs];
        c[i * cs + 1] = z->imagp[i * zs];
    }
}

/* ------------------------------------------------------- vector kernels */

#define A(i) a[(i) * as]
#define B(i) b[(i) * bs]
#define C(i) c[(i) * cs]
#define D(i) d[(i) * ds]

static void vd_vadd(const float *a, vDSP_Stride as, const float *b, vDSP_Stride bs,
                    float *c, vDSP_Stride cs, vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = A(i) + B(i); }

static void vd_vsub(const float *a, vDSP_Stride as, const float *b, vDSP_Stride bs,
                    float *c, vDSP_Stride cs, vDSP_Length n)
/* vDSP_vsub computes b - a, not a - b. */
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = B(i) - A(i); }

static void vd_vmul(const float *a, vDSP_Stride as, const float *b, vDSP_Stride bs,
                    float *c, vDSP_Stride cs, vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = A(i) * B(i); }

static void vd_vdiv(const float *a, vDSP_Stride as, const float *b, vDSP_Stride bs,
                    float *c, vDSP_Stride cs, vDSP_Length n)
/* likewise b / a */
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = A(i) != 0.0f ? B(i) / A(i) : 0.0f; }

static void vd_vsmul(const float *a, vDSP_Stride as, const float *s,
                     float *c, vDSP_Stride cs, vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = A(i) * *s; }

static void vd_vsma(const float *a, vDSP_Stride as, const float *s,
                    const float *b, vDSP_Stride bs, float *c, vDSP_Stride cs,
                    vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = A(i) * *s + B(i); }

static void vd_vma(const float *a, vDSP_Stride as, const float *b, vDSP_Stride bs,
                   const float *c, vDSP_Stride cs, float *d, vDSP_Stride ds,
                   vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) D(i) = A(i) * B(i) + C(i); }

static void vd_vneg(const float *a, vDSP_Stride as, float *c, vDSP_Stride cs,
                    vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = -A(i); }

static void vd_vabs(const float *a, vDSP_Stride as, float *c, vDSP_Stride cs,
                    vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = fabsf(A(i)); }

static void vd_vramp(const float *start, const float *step, float *c,
                     vDSP_Stride cs, vDSP_Length n)
{ vDSP_Length i; float v = *start; for (i = 0; i < n; i++) { C(i) = v; v += *step; } }

static void vd_vgen(const float *a, const float *b, float *c, vDSP_Stride cs,
                    vDSP_Length n)
{
    vDSP_Length i;
    if (n < 2) { if (n) C(0) = *a; return; }
    for (i = 0; i < n; i++)
        C(i) = *a + (*b - *a) * (float)i / (float)(n - 1);
}

static void vd_vthres(const float *a, vDSP_Stride as, const float *t,
                      float *c, vDSP_Stride cs, vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = A(i) < *t ? *t : A(i); }

static void vd_vdist(const float *a, vDSP_Stride as, const float *b, vDSP_Stride bs,
                     float *c, vDSP_Stride cs, vDSP_Length n)
{ vDSP_Length i; for (i = 0; i < n; i++) C(i) = sqrtf(A(i) * A(i) + B(i) * B(i)); }

static void vd_maxmgv(const float *a, vDSP_Stride as, float *out, vDSP_Length n)
{
    vDSP_Length i; float m = 0.0f;
    for (i = 0; i < n; i++) { float v = fabsf(A(i)); if (v > m) m = v; }
    if (out) *out = m;
}

static void vd_meanv(const float *a, vDSP_Stride as, float *out, vDSP_Length n)
{
    vDSP_Length i; double s = 0.0;
    for (i = 0; i < n; i++) s += A(i);
    if (out) *out = n ? (float)(s / (double)n) : 0.0f;
}

/* dB conversion. `flag` 1 means the input is amplitude (20*log10), 0 power
 * (10*log10) -- the opposite way round is a classic mistake. */
static void vd_vdbcon(const float *a, vDSP_Stride as, const float *zero,
                      float *c, vDSP_Stride cs, vDSP_Length n, unsigned flag)
{
    vDSP_Length i;
    float k = flag ? 20.0f : 10.0f;
    float z = (zero && *zero != 0.0f) ? *zero : 1.0f;
    for (i = 0; i < n; i++) {
        float v = fabsf(A(i)) / z;
        C(i) = v > 1e-30f ? k * log10f(v) : k * -30.0f;
    }
}

/* Linear interpolation lookup: c[i] = table[floor(b[i])] blended by the
 * fraction. `m` is the output count, `tn` the table length. */
static void vd_vlint(const float *table, const float *b, vDSP_Stride bs,
                     float *c, vDSP_Stride cs, vDSP_Length m, vDSP_Length tn)
{
    vDSP_Length i;
    for (i = 0; i < m; i++) {
        float x = B(i);
        long k = (long)x;
        float f = x - (float)k;
        if (k < 0) { k = 0; f = 0.0f; }
        if ((vDSP_Length)k >= tn - 1) { k = (long)tn - 1; f = 0.0f; }
        C(i) = table[k] + f * (table[k + 1 < (long)tn ? k + 1 : k] - table[k]);
    }
}

static void vd_hann_window(float *w, vDSP_Length n, int flag)
{
    vDSP_Length i;
    /* flag bit 0 selects the denormalised ("half") form used for overlap-add. */
    double den = (flag & 1) ? (double)n : (double)(n - 1);
    if (den <= 0.0) den = 1.0;
    for (i = 0; i < n; i++)
        w[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * (double)i / den)));
}

/* Sort an index vector by the values it points at. flag 1 ascending, -1
 * descending. Insertion sort: the corpus calls this on spectra of a few
 * thousand bins, not on large arrays. */
static void vd_vsorti(const float *a, vDSP_Length *idx, void *tmp,
                      vDSP_Length n, int flag)
{
    vDSP_Length i, j;
    (void)tmp;
    for (i = 1; i < n; i++) {
        vDSP_Length key = idx[i];
        float kv = a[key];
        j = i;
        while (j > 0 && ((flag >= 0) ? (a[idx[j - 1]] > kv) : (a[idx[j - 1]] < kv))) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }
}

/* rectangular -> polar and back, on interleaved pairs */
static void vd_polar(const float *a, vDSP_Stride as, float *c, vDSP_Stride cs,
                     vDSP_Length n)
{
    vDSP_Length i;
    for (i = 0; i < n; i++) {
        float x = a[i * as], y = a[i * as + 1];
        c[i * cs]     = sqrtf(x * x + y * y);
        c[i * cs + 1] = atan2f(y, x);
    }
}

/* ------------------------------------------------- split-complex kernels */

static void vd_zvmul(const DSPSplitComplex *a, vDSP_Stride as,
                     const DSPSplitComplex *b, vDSP_Stride bs,
                     const DSPSplitComplex *c, vDSP_Stride cs,
                     vDSP_Length n, int conj)
{
    vDSP_Length i;
    for (i = 0; i < n; i++) {
        float ar = a->realp[i * as], ai = a->imagp[i * as];
        float br = b->realp[i * bs], bi = b->imagp[i * bs];
        if (conj < 0) bi = -bi;
        c->realp[i * cs] = ar * br - ai * bi;
        c->imagp[i * cs] = ar * bi + ai * br;
    }
}

static void vd_zvadd(const DSPSplitComplex *a, vDSP_Stride as,
                     const DSPSplitComplex *b, vDSP_Stride bs,
                     const DSPSplitComplex *c, vDSP_Stride cs, vDSP_Length n)
{
    vDSP_Length i;
    for (i = 0; i < n; i++) {
        c->realp[i * cs] = a->realp[i * as] + b->realp[i * bs];
        c->imagp[i * cs] = a->imagp[i * as] + b->imagp[i * bs];
    }
}

static void vd_zvabs(const DSPSplitComplex *a, vDSP_Stride as,
                     float *c, vDSP_Stride cs, vDSP_Length n)
{
    vDSP_Length i;
    for (i = 0; i < n; i++) {
        float r = a->realp[i * as], m = a->imagp[i * as];
        c[i * cs] = sqrtf(r * r + m * m);
    }
}

/* c = a * s + b, with s a real scalar */
static void vd_zvsma(const DSPSplitComplex *a, vDSP_Stride as,
                     const DSPSplitComplex *s,
                     const DSPSplitComplex *b, vDSP_Stride bs,
                     const DSPSplitComplex *c, vDSP_Stride cs, vDSP_Length n)
{
    vDSP_Length i;
    float sr = s->realp[0], si = s->imagp[0];
    for (i = 0; i < n; i++) {
        float ar = a->realp[i * as], ai = a->imagp[i * as];
        c->realp[i * cs] = ar * sr - ai * si + b->realp[i * bs];
        c->imagp[i * cs] = ar * si + ai * sr + b->imagp[i * bs];
    }
}

/* c = a * real-scalar */
static void vd_zvzsml(const DSPSplitComplex *a, vDSP_Stride as,
                      const DSPSplitComplex *s,
                      const DSPSplitComplex *c, vDSP_Stride cs, vDSP_Length n)
{
    vDSP_Length i;
    float sr = s->realp[0], si = s->imagp[0];
    for (i = 0; i < n; i++) {
        float ar = a->realp[i * as], ai = a->imagp[i * as];
        c->realp[i * cs] = ar * sr - ai * si;
        c->imagp[i * cs] = ar * si + ai * sr;
    }
}

/* ------------------------------------------------------------- vForce */

static void vv_sinf(float *out, const float *in, const int *n)
{ int i; for (i = 0; i < *n; i++) out[i] = sinf(in[i]); }
static void vv_cosf(float *out, const float *in, const int *n)
{ int i; for (i = 0; i < *n; i++) out[i] = cosf(in[i]); }

/* ------------------------------------------------------------------ table */

const macshim_entry macshim_vdsp[] = {
    { "_vDSP_create_fftsetup",  vd_create_fftsetup },
    { "_vDSP_destroy_fftsetup", vd_destroy_fftsetup },
    { "_vDSP_fft_zrip",         vd_fft_zrip },
    { "_vDSP_ctoz",             vd_ctoz },
    { "_vDSP_ztoc",             vd_ztoc },
    { "_vDSP_vadd",             vd_vadd },
    { "_vDSP_vsub",             vd_vsub },
    { "_vDSP_vmul",             vd_vmul },
    { "_vDSP_vdiv",             vd_vdiv },
    { "_vDSP_vsmul",            vd_vsmul },
    { "_vDSP_vsma",             vd_vsma },
    { "_vDSP_vma",              vd_vma },
    { "_vDSP_vneg",             vd_vneg },
    { "_vDSP_vabs",             vd_vabs },
    { "_vDSP_vramp",            vd_vramp },
    { "_vDSP_vgen",             vd_vgen },
    { "_vDSP_vthres",           vd_vthres },
    { "_vDSP_vdist",            vd_vdist },
    { "_vDSP_maxmgv",           vd_maxmgv },
    { "_vDSP_meanv",            vd_meanv },
    { "_vDSP_vdbcon",           vd_vdbcon },
    { "_vDSP_vlint",            vd_vlint },
    { "_vDSP_hann_window",      vd_hann_window },
    { "_vDSP_vsorti",           vd_vsorti },
    { "_vDSP_polar",            vd_polar },
    { "_vDSP_zvmul",            vd_zvmul },
    { "_vDSP_zvadd",            vd_zvadd },
    { "_vDSP_zvabs",            vd_zvabs },
    { "_vDSP_zvsma",            vd_zvsma },
    { "_vDSP_zvzsml",           vd_zvzsml },
    { "_vvsinf",                vv_sinf },
    { "_vvcosf",                vv_cosf },
    { NULL, NULL }
};

/* Exposed for the self-test in tools/, which checks the transform against a
 * reference rather than trusting it. */
void *macshim_fftsetup(unsigned long log2n) { return vd_create_fftsetup(log2n, 0); }
void  macshim_fft_zrip(void *setup, float *realp, float *imagp,
                       unsigned long log2n, int dir)
{
    DSPSplitComplex z; z.realp = realp; z.imagp = imagp;
    vd_fft_zrip(setup, &z, 1, log2n, dir);
}
