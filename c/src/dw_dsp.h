/* DSP primitives for the DW-8000 voice: the six-stage envelope, the resonant
 * lowpass, the MG, and the modulated delay.
 *
 * These are written from the documented DW-8000 architecture, NOT recovered
 * from FB-7999's code -- no attempt has been made to match its filter topology
 * or envelope curves sample for sample. Only the waveforms and the parameter
 * values are exact. See FINDINGS.md section 6. */
#ifndef DW_DSP_H
#define DW_DSP_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- six-stage envelope ------------------------------------------------
 * ATTACK(time) -> DECAY(time) -> BREAK POINT(level) -> SLOPE(time)
 *              -> SUSTAIN(level) -> RELEASE(time)
 *
 * Break Point and Sustain are levels; Attack, Decay, Slope and Release are
 * times. Decay runs from the peak down to the break point, then Slope runs
 * from the break point to the sustain level -- that second ramp is what the
 * DW's envelope has and a plain ADSR does not. */

typedef enum {
    DW_EG_IDLE = 0, DW_EG_ATTACK, DW_EG_DECAY,
    DW_EG_SLOPE, DW_EG_SUSTAIN, DW_EG_RELEASE
} dw_eg_stage;

typedef struct {
    double attack, decay, slope, release;   /* seconds */
    double breakpoint, sustain;             /* 0..1    */
} dw_eg_cfg;

typedef struct {
    dw_eg_cfg   cfg;
    dw_eg_stage stage;
    double      level;
    double      samplerate;
    double      inc;      /* attack is linear    */
    double      coef;     /* other stages decay exponentially */
    double      target;
} dw_eg;

void   dw_eg_init(dw_eg *e, double samplerate);
void   dw_eg_config(dw_eg *e, const dw_eg_cfg *cfg);
void   dw_eg_gate_on(dw_eg *e);
void   dw_eg_gate_off(dw_eg *e);
double dw_eg_process(dw_eg *e);
static inline int dw_eg_active(const dw_eg *e) { return e->stage != DW_EG_IDLE; }

/* Maps a stored 0..31 parameter to seconds, exponentially. */
double dw_eg_time(double v031);

/* ---- tunables ----------------------------------------------------------
 * The scalings marked APPROX in dw_synth.c: how a stored 0..31 or 0..63 step
 * becomes seconds, hertz or octaves. These are the parts that were NOT
 * recovered from the plugin -- only the parameter values and ranges were --
 * so they are the dials that decide how close this sounds to the original.
 *
 * dw_tuning_from_env() lets them be changed without a rebuild, which is the
 * point: they are meant to be experimented with.
 *
 *   DW_EG_MIN / DW_EG_MAX        envelope stage times at step 0 and 31
 *   DW_VCF_EG_OCTAVES            how far a full VCF envelope opens the filter
 *   DW_CUTOFF_MIN / _OCTAVES     cutoff at step 0, and the span to step 63
 *   DW_MG_MIN / DW_MG_MAX        MG rate at step 0 and 31
 *   DW_DELAY_MIN / _MAX          delay time at step 0 and 7
 *   DW_DELAY_MIX                 scale on the wet level (1.0 = fully wet at
 *                                Delay Level 15)
 *   DW_GAIN                      master output gain, linear (1.0 = unity)
 */
typedef struct {
    double eg_min, eg_max;
    double vcf_eg_octaves;
    double cutoff_min, cutoff_octaves;
    double mg_min, mg_max;
    double delay_min, delay_max, delay_mix;
    double gain;
} dw_tuning;

extern dw_tuning dw_tune;

void dw_tuning_from_env(void);
void dw_tuning_print(void);

/* Saturation for the filter's feedback path, where a bit of non-linearity is
 * wanted. Note it starts bending well below full scale -- fine inside a ladder,
 * wrong for a master bus. */
static inline double dw_softclip(double x)
{
    if (x < -3.0) return -1.0;
    if (x >  3.0) return  1.0;
    return x * (27.0 + x * x) / (27.0 + 9.0 * x * x);
}

/* Master limiter: exactly linear below the knee, smoothly bounded above it, so
 * ordinary playing is untouched and only a dense chord gets compressed.
 * dw_softclip() was doing this job and colouring everything on the way. */
#define DW_LIMIT_KNEE 0.70

static inline double dw_limit(double x)
{
    double a = x < 0.0 ? -x : x;
    double over;
    if (a <= DW_LIMIT_KNEE) return x;
    over = (a - DW_LIMIT_KNEE) / (1.0 - DW_LIMIT_KNEE);
    over = over / (1.0 + over);            /* -> 1 as the input grows */
    a = DW_LIMIT_KNEE + (1.0 - DW_LIMIT_KNEE) * over;
    return x < 0.0 ? -a : a;
}

/* ---- resonant lowpass --------------------------------------------------
 * Four cascaded one-pole sections with a feedback path -- the usual ladder
 * approximation. Resonance 0..1 reaches self-oscillation near the top. */

typedef struct {
    double s[4];
    double g, res, comp, samplerate;
} dw_filter;

void   dw_filter_init(dw_filter *f, double samplerate);
void   dw_filter_set(dw_filter *f, double cutoff_hz, double res01);
double dw_filter_process(dw_filter *f, double in);

/* ---- MG (the LFO) ---------------------------------------------------- */

typedef enum { DW_MG_TRI = 0, DW_MG_SAW, DW_MG_RAMP, DW_MG_RECT } dw_mg_wave;

typedef struct {
    double     phase, inc, samplerate;
    double     delay, elapsed;   /* fade-in, seconds */
    dw_mg_wave wave;
} dw_mg;

void   dw_mg_init(dw_mg *m, double samplerate);
void   dw_mg_set(dw_mg *m, dw_mg_wave w, double hz, double delay_sec);
void   dw_mg_retrigger(dw_mg *m);
double dw_mg_process(dw_mg *m);   /* -1..1, scaled by the delay fade-in */

/* ---- modulated delay ------------------------------------------------- */

typedef struct {
    float  *buf;
    int     size, write;
    double  samplerate;
    double  delay_samples, feedback, level;
    double  mod_depth, mod_phase, mod_inc;
} dw_delay;

int    dw_delay_init(dw_delay *d, double samplerate, double max_seconds);
void   dw_delay_free(dw_delay *d);
void   dw_delay_set(dw_delay *d, double time_sec, double feedback,
                    double level, double mod_hz, double mod_depth_sec);
double dw_delay_process(dw_delay *d, double in);

#ifdef __cplusplus
}
#endif

#endif /* DW_DSP_H */
