/* DW-8000 voice engine.
 *
 * Reads a program straight out of a decoded FB-7999 preset bank -- the 69
 * doubles, in the recovered parameter order -- and renders audio.
 *
 * Scope: the waveforms and the parameter values are exact, taken from the
 * plugin's own resources. The DSP around them is a reimplementation from the
 * documented DW-8000 architecture, not a port of FB-7999's code, which has not
 * been reverse engineered. It will not match the plugin sample for sample.
 * Points where a mapping is an approximation are marked APPROX in dw_synth.c. */
#ifndef DW_SYNTH_H
#define DW_SYNTH_H

#include "dw_dsp.h"
#include "dw_wavetable.h"

#define DW_MAX_VOICES 16

#ifdef __cplusplus
extern "C" {
#endif

/* Parameter indices, recovered from FUN_18052f240. Mirrors bank_param_name(). */
enum {
    DWP_OSC1_OCTAVE = 0,  DWP_OSC1_WAVEFORM,    DWP_OSC1_LEVEL,
    DWP_AUTOBEND_SELECT,  DWP_AUTOBEND_MODE,    DWP_AUTOBEND_TIME,
    DWP_AUTOBEND_INTENSITY, DWP_OSC2_OCTAVE,    DWP_OSC2_WAVEFORM,
    DWP_OSC2_LEVEL,       DWP_OSC2_INTERVAL,    DWP_OSC2_DETUNE,
    DWP_NOISE_LEVEL,      DWP_MODE,             DWP_EDIT_PARAMETER,
    DWP_VCF_CUTOFF,       DWP_VCF_RESONANCE,    DWP_VCF_KBD_TRACK,
    DWP_VCF_EG_POLARITY,  DWP_VCF_EG_INTENSITY, DWP_VCF_EG_ATTACK,
    DWP_VCF_EG_DECAY,     DWP_VCF_EG_BREAKPOINT,DWP_VCF_EG_SLOPE,
    DWP_VCF_EG_SUSTAIN,   DWP_VCF_EG_RELEASE,   DWP_VCF_EG_VELOCITY,
    DWP_VCA_EG_ATTACK,    DWP_VCA_EG_DECAY,     DWP_VCA_EG_BREAKPOINT,
    DWP_VCA_EG_SLOPE,     DWP_VCA_EG_SUSTAIN,   DWP_VCA_EG_RELEASE,
    DWP_VCA_EG_VELOCITY,  DWP_MG_WAVEFORM,      DWP_MG_FREQUENCY,
    DWP_MG_DELAY,         DWP_MG_OSC,           DWP_MG_VCF,
    DWP_BEND_OSC,         DWP_BEND_VCF,         DWP_DELAY_TIME,
    DWP_DELAY_FACTOR,     DWP_DELAY_FEEDBACK,   DWP_DELAY_MOD_FREQ,
    DWP_DELAY_MOD_INT,    DWP_DELAY_LEVEL,      DWP_PORTAMENTO,
    DWP_AT_OSC_MG,        DWP_AT_VCF,           DWP_AT_VCA,
    DWP_MW_OSC_MG,        DWP_MW_VCF_MG,        DWP_PSEUDO_STEREO,
    DWP_VOICES,           DWP_VOLUME,           DWP_TUNE,
    DWP_DW_MODE,          DWP_WAVETABLE_SET,    DWP_VCF_MG_MOD_SOURCE,
    DWP_COUNT = 69
};

/* Mode (p13) */
enum { DW_MODE_POLY1 = 0, DW_MODE_POLY2, DW_MODE_UNISON1, DW_MODE_UNISON2 };

typedef struct {
    int    active, note, held;
    double velocity;
    double phase1, phase2;
    double pitch, pitch_target;   /* semitones, for portamento */
    double bend_level;            /* auto bend, semitones */
    double bend_coef;
    double detune_cents;          /* unison spread */
    dw_eg     eg_vcf, eg_vca;
    dw_filter filt;
    unsigned  age;
} dw_voice;

typedef struct {
    const dw_wavetable *wt;
    double   samplerate;
    double   param[DWP_COUNT];

    dw_voice voice[DW_MAX_VOICES];
    int      nvoices;
    unsigned counter;

    dw_mg    mg;
    dw_delay delay_l, delay_r;

    /* derived per program */
    int      wave1, wave2;
    double   osc1_gain, osc2_gain, noise_gain;
    double   osc2_offset_semi, osc2_detune_cents;
    double   cutoff_hz, resonance, kbd_track;
    double   vcf_eg_amount, vcf_eg_sign, vcf_eg_vel, vca_eg_vel;
    double   mg_osc_cents, mg_vcf_amount;
    double   bend_semis, bend_time, bend_dir;
    int      bend_osc1, bend_osc2;
    double   porta_time, volume, tune_cents;
    double   voice_scale;   /* master headroom, set from the voice count */
    int      mode;
    unsigned rng;
} dw_synth;

int  dw_synth_init(dw_synth *s, const dw_wavetable *wt, double samplerate);
void dw_synth_free(dw_synth *s);

/* `param` is the 69-value array from a decoded bank program. */
void dw_synth_set_program(dw_synth *s, const double *param);

void dw_synth_note_on(dw_synth *s, int note, int velocity);
void dw_synth_note_off(dw_synth *s, int note);
void dw_synth_all_off(dw_synth *s);

/* Writes `frames` interleaved stereo sample pairs. */
void dw_synth_render(dw_synth *s, double *out, int frames);

/* True while any voice is still sounding. */
int  dw_synth_busy(const dw_synth *s);

#ifdef __cplusplus
}
#endif

#endif /* DW_SYNTH_H */
