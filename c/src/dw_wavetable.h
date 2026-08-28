/* Band-limited mipmapped oscillator tables built from the WAVEDST ROM.
 *
 * WAVEDST stores 32 waveforms as 319 harmonic amplitudes each. Playing those
 * back at an arbitrary pitch needs the harmonic series truncated below Nyquist
 * for the note being played, so we pre-render one table per octave and pick
 * the right one at note time. */
#ifndef DW_WAVETABLE_H
#define DW_WAVETABLE_H

#include "wavedst.h"

#define DW_FRAME  2048    /* samples per single cycle */
#define DW_MIPS     12    /* octaves covered, starting at DW_BASE_HZ */
#define DW_BASE_HZ  8.1757989156437073   /* MIDI note 0 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     nwaves;
    int     frame;
    int     mips;
    double  samplerate;
    float  *tab;        /* nwaves * mips * (frame+1); the extra sample is a
                         * wrapped copy of tab[0] so interpolation needs no
                         * branch at the end of the cycle */
    int    *harmonics;  /* nwaves * mips, for reporting */
} dw_wavetable;

int  dw_wavetable_build(dw_wavetable *wt, const wavedst *wd, double samplerate);
void dw_wavetable_free(dw_wavetable *wt);

/* Which mip to read for a given fundamental. */
int  dw_wavetable_mip(const dw_wavetable *wt, double hz);

/* `phase` is in cycles, 0..1. Linear interpolation between table samples. */
double dw_wavetable_read(const dw_wavetable *wt, int wave, int mip, double phase);

#ifdef __cplusplus
}
#endif

#endif /* DW_WAVETABLE_H */
