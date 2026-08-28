/* Four-operator FM voice engine for FB-02 (Yamaha FB-01) programs.
 *
 * Scope, same as dw_synth.h: the parameter values and their meanings are taken
 * from the plugin, but the DSP is a reimplementation from the documented
 * 4-operator FM architecture. FB-02's own code has not been reverse
 * engineered, so this will not match it sample for sample. Points where a
 * mapping is a judgement call are marked APPROX in fm_synth.c. */
#ifndef FM_SYNTH_H
#define FM_SYNTH_H

#include "fb02.h"

#define FM_MAX_VOICES 16

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double phase, inc;
    double out, prev;          /* last output, for modulating the next operator */
    int    stage;              /* 0 idle, 1 attack, 2 decay1, 3 decay2, 4 release */
    double level;              /* 0..1 */
    double inc_a;              /* linear attack increment */
    double c_d1, c_d2, c_rr;   /* exponential coefficients */
    double sustain;            /* 0..1 */
    double gain;               /* from Total Level, plus velocity */
} fm_op;

typedef struct {
    int      active, note, held;
    double   velocity;
    fm_op    op[FB02_OPS];
    double   fb1, fb2;         /* feedback history on operator 1 */
    unsigned age;
} fm_voice;

typedef struct {
    double       samplerate;
    fb02_program prog;
    int          have_prog;
    fm_voice     voice[FM_MAX_VOICES];
    int          nvoices;
    unsigned     counter;
    double       lfo_phase, lfo_inc;
    double       gain;
} fm_synth;

int  fm_synth_init(fm_synth *s, double samplerate);
void fm_synth_set_program(fm_synth *s, const fb02_program *p);
void fm_synth_note_on(fm_synth *s, int note, int velocity);
void fm_synth_note_off(fm_synth *s, int note);
void fm_synth_all_off(fm_synth *s);
void fm_synth_render(fm_synth *s, double *out, int frames);  /* interleaved stereo */
int  fm_synth_busy(const fm_synth *s);

/* Which operators reach the output for a given algorithm, as a bitmask. */
unsigned fm_algorithm_carriers(int alg);

#ifdef __cplusplus
}
#endif

#endif /* FM_SYNTH_H */
