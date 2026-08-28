#include "dw_wavetable.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int dw_wavetable_build(dw_wavetable *wt, const wavedst *wd, double samplerate)
{
    double *sine;
    int     w, m, k, t;
    size_t  stride = (size_t)DW_FRAME + 1;

    memset(wt, 0, sizeof *wt);
    if (!wd->nwaves || !wd->nharm || samplerate <= 0.0) return -1;

    wt->nwaves     = wd->nwaves;
    wt->frame      = DW_FRAME;
    wt->mips       = DW_MIPS;
    wt->samplerate = samplerate;

    wt->tab = malloc((size_t)wt->nwaves * wt->mips * stride * sizeof *wt->tab);
    wt->harmonics = malloc((size_t)wt->nwaves * wt->mips * sizeof *wt->harmonics);
    sine = malloc(DW_FRAME * sizeof *sine);
    if (!wt->tab || !wt->harmonics || !sine) {
        free(sine); dw_wavetable_free(wt); return -1;
    }

    /* sin(2*pi*k*t/N) for integer k,t is exactly sine[(k*t) mod N], so the
     * whole table set is built from lookups with no loss of accuracy and no
     * call to sin() in the inner loop. */
    for (t = 0; t < DW_FRAME; t++)
        sine[t] = sin(2.0 * M_PI * (double)t / (double)DW_FRAME);

    for (w = 0; w < wt->nwaves; w++) {
        const double *amp = wavedst_row(wd, w);

        for (m = 0; m < wt->mips; m++) {
            /* Mip m serves fundamentals up to DW_BASE_HZ * 2^(m+1). Keep only
             * the harmonics that stay below Nyquist at that top frequency. */
            double top   = DW_BASE_HZ * pow(2.0, m + 1);
            int    limit = (int)floor((samplerate * 0.5) / top);
            float *dst   = wt->tab + ((size_t)w * wt->mips + m) * stride;
            double peak  = 0.0;

            if (limit > wd->nharm)    limit = wd->nharm;
            if (limit > DW_FRAME / 2) limit = DW_FRAME / 2;
            if (limit < 1)            limit = 1;
            wt->harmonics[w * wt->mips + m] = limit;

            for (t = 0; t < DW_FRAME; t++) dst[t] = 0.0f;
            for (k = 1; k <= limit; k++) {
                double ak = amp[k - 1];
                int    ph = 0;
                if (ak == 0.0) continue;
                for (t = 0; t < DW_FRAME; t++) {
                    dst[t] += (float)(ak * sine[ph]);
                    ph += k;
                    if (ph >= DW_FRAME) ph -= DW_FRAME;
                }
            }

            /* Normalise per waveform-and-mip so switching mips mid-glide does
             * not step the level, and so all 32 waveforms play at a
             * comparable loudness. */
            for (t = 0; t < DW_FRAME; t++)
                if (fabs(dst[t]) > peak) peak = fabs(dst[t]);
            if (peak > 0.0) {
                float g = (float)(1.0 / peak);
                for (t = 0; t < DW_FRAME; t++) dst[t] *= g;
            }
            dst[DW_FRAME] = dst[0];
        }
    }

    free(sine);
    return 0;
}

void dw_wavetable_free(dw_wavetable *wt)
{
    free(wt->tab);
    free(wt->harmonics);
    memset(wt, 0, sizeof *wt);
}

int dw_wavetable_mip(const dw_wavetable *wt, double hz)
{
    int m;
    if (hz <= DW_BASE_HZ) return 0;
    m = (int)floor(log(hz / DW_BASE_HZ) / log(2.0));
    if (m < 0) m = 0;
    if (m >= wt->mips) m = wt->mips - 1;
    return m;
}

double dw_wavetable_read(const dw_wavetable *wt, int wave, int mip, double phase)
{
    const float *tab;
    double       x, frac;
    int          i;

    if (wave < 0) wave = 0;
    if (wave >= wt->nwaves) wave = wt->nwaves - 1;

    tab  = wt->tab + ((size_t)wave * wt->mips + mip) * ((size_t)wt->frame + 1);
    x    = phase * (double)wt->frame;
    i    = (int)x;
    frac = x - (double)i;
    if (i < 0) i = 0;
    if (i >= wt->frame) i = wt->frame - 1;

    return tab[i] + frac * (tab[i + 1] - tab[i]);
}
