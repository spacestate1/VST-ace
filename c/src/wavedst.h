/* WAVEDST: FB-7999's DW waveform ROM.
 *
 * The resource is a flat array of little-endian float32 harmonic amplitudes,
 * 32 waveforms x 319 harmonics. It is not sampled PCM -- each value is the
 * amplitude of one harmonic, and its sign carries the 0/pi phase flip. */
#ifndef FB_WAVEDST_H
#define FB_WAVEDST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     nwaves;
    int     nharm;
    double *amp;     /* nwaves * nharm, row-major */
} wavedst;

/* `nwaves` of 0 selects the geometry automatically (32 for the stock ROM). */
int  wavedst_load(wavedst *wd, const unsigned char *data, size_t size, int nwaves);
void wavedst_free(wavedst *wd);

static inline const double *wavedst_row(const wavedst *wd, int i)
{
    return wd->amp + (size_t)i * wd->nharm;
}

/* Additive resynthesis in sine phase:
 *     x[t] = sum_k a_k * sin(2*pi*k*t/frame)
 * which is what numpy's irfft() produces when bin k holds -1j*a_k, up to a
 * constant gain that the peak normalisation in wav_write_mono16 removes.
 * Harmonics at or above frame/2 are dropped (band-limiting). */
void wavedst_synth(const wavedst *wd, int wave, double *out, int frame);

/* Structural probe -- the C equivalent of analyze_wavedst.py + the geometry
 * test in parse_wavedst.py. Prints to stdout. */
void wavedst_probe(const unsigned char *data, size_t size);

/* Per-waveform spectrum report; JSON matching out/wavedst_spectra.json when
 * `json_path` is non-NULL. */
int  wavedst_report(const wavedst *wd, const char *json_path);

/* Cross-correlates the reconstruction of `wave` against an ideal shape
 * ("sawtooth", "square" or "sine") over every circular shift, returning the
 * peak |r| in 0..1. */
double wavedst_match(const wavedst *wd, int wave, const char *ideal, int frame);

#ifdef __cplusplus
}
#endif

#endif /* FB_WAVEDST_H */
