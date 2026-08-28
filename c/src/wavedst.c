#include "wavedst.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float rdf32(const unsigned char *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float    f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* Score a candidate geometry: in a table of harmonic amplitudes the
 * fundamental dominates, so each block should peak at its own first entry. */
static int geometry_score(const double *v, size_t n, int nwaves)
{
    size_t len = n / (size_t)nwaves;
    int    hits = 0, w;

    for (w = 0; w < nwaves; w++) {
        const double *blk = v + (size_t)w * len;
        double        best = 0.0;
        size_t        i;
        for (i = 0; i < len; i++)
            if (fabs(blk[i]) > best) best = fabs(blk[i]);
        if (len && fabs(blk[0]) == best) hits++;
    }
    return hits;
}

int wavedst_load(wavedst *wd, const unsigned char *data, size_t size, int nwaves)
{
    size_t  n = size / 4, i;
    double *v;

    memset(wd, 0, sizeof *wd);
    if (n == 0) return -1;

    if (!(v = malloc(n * sizeof *v))) return -1;
    for (i = 0; i < n; i++) v[i] = (double)rdf32(data + i * 4);

    if (nwaves <= 0) {
        static const int cand[] = { 8, 16, 22, 29, 32, 44, 58, 64, 116, 128 };
        int best = 0, best_hits = -1, k;
        for (k = 0; k < (int)(sizeof cand / sizeof *cand); k++) {
            int c = cand[k], hits;
            if (n % (size_t)c) continue;
            hits = geometry_score(v, n, c);
            if (hits > best_hits || (hits == best_hits && c > best)) {
                best_hits = hits; best = c;
            }
        }
        if (best <= 0) { free(v); return -1; }
        nwaves = best;
    }
    if (n % (size_t)nwaves) { free(v); return -1; }

    wd->nwaves = nwaves;
    wd->nharm  = (int)(n / (size_t)nwaves);
    wd->amp    = v;
    return 0;
}

void wavedst_free(wavedst *wd)
{
    free(wd->amp);
    memset(wd, 0, sizeof *wd);
}

void wavedst_synth(const wavedst *wd, int wave, double *out, int frame)
{
    const double *a = wavedst_row(wd, wave);
    int           usable = wd->nharm < frame / 2 ? wd->nharm : frame / 2;
    int           k, t;

    for (t = 0; t < frame; t++) out[t] = 0.0;

    for (k = 1; k <= usable; k++) {
        double ak = a[k - 1];
        double w;
        if (ak == 0.0) continue;
        w = 2.0 * M_PI * (double)k / (double)frame;
        for (t = 0; t < frame; t++) out[t] += ak * sin(w * (double)t);
    }
}

void wavedst_probe(const unsigned char *data, size_t size)
{
    size_t  n = size / 4, i;
    int     k, nz = 0;
    double  lo = 1e300, hi = -1e300, peak_over = 0;
    double *v;

    printf("size: %zu bytes = %zu float32\n", size, n);

    printf("divisors <= 4096:");
    for (k = 1; k <= 4096; k++)
        if (size % (size_t)k == 0) printf(" %d", k);
    printf("\n");

    if (!(v = malloc(n * sizeof *v))) return;
    for (i = 0; i < n; i++) {
        v[i] = (double)rdf32(data + i * 4);
        if (v[i] != 0.0) nz++;
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
        if (fabs(v[i]) > 0.05) peak_over++;
    }
    printf("as float32: min=%.6f max=%.6f  nonzero=%d/%zu  |v|>0.05: %.0f\n",
           lo, hi, nz, n, peak_over);
    printf("first 12: ");
    for (i = 0; i < 12 && i < n; i++) printf("%.5f ", v[i]);
    printf("\n\n--- geometry test (does each block peak at its own first entry?) ---\n");
    {
        static const int cand[] = { 8, 16, 22, 29, 32, 44, 58, 64, 116, 128 };
        int c;
        for (c = 0; c < (int)(sizeof cand / sizeof *cand); c++) {
            int nw = cand[c];
            if (n % (size_t)nw) continue;
            printf("  %3d waves x %5zu harmonics -> %d/%d blocks\n",
                   nw, n / (size_t)nw, geometry_score(v, n, nw), nw);
        }
    }
    free(v);
}

int wavedst_report(const wavedst *wd, const char *json_path)
{
    FILE *j = NULL;
    int   w;

    if (json_path && !(j = fopen(json_path, "w"))) { perror(json_path); return -1; }
    if (j) fprintf(j, "[\n");

    for (w = 0; w < wd->nwaves; w++) {
        const double *blk = wavedst_row(wd, w);
        int           present = 0, highest = 0, i;
        double        peak = 0.0;

        for (i = 0; i < wd->nharm; i++) {
            if (fabs(blk[i]) > 1e-6) { present++; highest = i + 1; }
            if (fabs(blk[i]) > peak) peak = fabs(blk[i]);
        }
        printf("wave %2d: %4d nonzero harmonics, highest=%4d, peak=%.4f, h1..h6=",
               w, present, highest, peak);
        for (i = 0; i < 6 && i < wd->nharm; i++) printf(" %.4f", blk[i]);
        printf("\n");

        if (j) {
            fprintf(j, " {\"index\": %d, \"harmonics_present\": %d, "
                       "\"highest_harmonic\": %d, \"peak\": %g, \"first8\": [",
                    w, present, highest, peak);
            for (i = 0; i < 8 && i < wd->nharm; i++)
                fprintf(j, "%s%g", i ? ", " : "", blk[i]);
            fprintf(j, "]}%s\n", w + 1 < wd->nwaves ? "," : "");
        }
    }
    if (j) { fprintf(j, "]\n"); fclose(j); }
    return 0;
}

/* Zero-mean, unit-L2 in place. */
static void normalise(double *x, int n)
{
    double mean = 0.0, nrm = 0.0;
    int    i;
    for (i = 0; i < n; i++) mean += x[i];
    mean /= (double)n;
    for (i = 0; i < n; i++) { x[i] -= mean; nrm += x[i] * x[i]; }
    nrm = sqrt(nrm);
    if (nrm == 0.0) nrm = 1.0;
    for (i = 0; i < n; i++) x[i] /= nrm;
}

double wavedst_match(const wavedst *wd, int wave, const char *ideal, int frame)
{
    double *a, *b;
    double  best = 0.0;
    int     t, s;

    if (!(a = malloc((size_t)frame * sizeof *a))) return -1.0;
    if (!(b = malloc((size_t)frame * sizeof *b))) { free(a); return -1.0; }

    wavedst_synth(wd, wave, a, frame);

    for (t = 0; t < frame; t++) {
        double u = (double)t / (double)frame;
        if      (!strcmp(ideal, "sawtooth")) b[t] = 2.0 * (u - 0.5);
        else if (!strcmp(ideal, "square")) {
            /* numpy's sign(): exactly zero maps to zero, not to +1. The
             * distinction costs 0.0003 of correlation at t = 0. */
            double s = sin(2.0 * M_PI * u);
            b[t] = (s > 0.0) - (s < 0.0);
        }
        else                                 b[t] = sin(2.0 * M_PI * u);
    }

    normalise(a, frame);
    normalise(b, frame);

    /* Phase-invariant match: best |correlation| over every circular shift. */
    for (s = 0; s < frame; s++) {
        double acc = 0.0;
        int    u = s;
        for (t = 0; t < frame; t++) {
            acc += a[u] * b[t];
            if (++u == frame) u = 0;
        }
        if (fabs(acc) > best) best = fabs(acc);
    }

    free(a);
    free(b);
    return best;
}
