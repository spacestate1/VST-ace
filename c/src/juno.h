/* Roland Juno-6 voice engine.
 *
 * Unlike the other two engines here, nothing about this one is extracted: the
 * Juno-6 has no patch memory, so there is no preset data in existence to
 * recover. The architecture is the documented one -- a single DCO with saw,
 * pulse and sub, a non-resonant HPF ahead of a 4-pole LPF, one ADSR shared by
 * filter and amplifier, one LFO, and the BBD chorus -- and the patches in
 * juno_factory() are written by hand, not Roland's.
 *
 * Signal path per voice:
 *   DCO (saw + pulse + sub + noise) -> HPF -> LPF -> VCA
 * then a shared stereo chorus across the whole voice mix. */
#ifndef JUNO_H
#define JUNO_H

#define JUNO_MAX_VOICES 8
#define JUNO_NAME_MAX   24

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JUNO_CH_OFF = 0, JUNO_CH_I, JUNO_CH_II } juno_chorus_mode;

typedef struct {
    char   name[JUNO_NAME_MAX];

    int    range;         /* 0 = 16', 1 = 8', 2 = 4'          */
    double dco_lfo;       /* LFO -> pitch, 0..1               */
    double pwm;           /* pulse width modulation depth     */
    int    pwm_manual;    /* 1 = manual width, 0 = LFO        */
    int    saw, pulse;    /* waveform switches                */
    double sub;           /* sub-oscillator level 0..1        */
    double noise;         /* noise level 0..1                 */

    int    hpf;           /* 0..3; 0 boosts the bottom end    */

    double cutoff, res;   /* 0..1                             */
    double vcf_env;       /* envelope -> cutoff, 0..1         */
    int    vcf_env_neg;   /* envelope polarity                */
    double vcf_lfo;       /* LFO -> cutoff, 0..1              */
    double vcf_key;       /* keyboard follow, 0..1            */

    int    vca_gate;      /* 1 = gate, 0 = envelope           */
    double volume;

    double a, d, s, r;    /* 0..1                             */

    double lfo_rate;      /* 0..1                             */
    double lfo_delay;     /* 0..1                             */

    juno_chorus_mode chorus;
} juno_patch;

typedef struct juno_synth juno_synth;

/* Every panel control, addressable at run time so the GUI can edit a patch
 * live rather than only selecting from the built-in list. */
enum {
    JP_RANGE = 0, JP_DCO_LFO, JP_PWM, JP_PWM_MANUAL, JP_SAW, JP_PULSE,
    JP_SUB, JP_NOISE, JP_HPF, JP_CUTOFF, JP_RES, JP_VCF_ENV, JP_VCF_ENV_NEG,
    JP_VCF_LFO, JP_VCF_KEY, JP_VCA_GATE, JP_VOLUME, JP_A, JP_D, JP_S, JP_R,
    JP_LFO_RATE, JP_LFO_DELAY, JP_CHORUS, JP_COUNT
};

const char *juno_param_name(int id);
/* Continuous parameters take 0..1; switches take their integer value. */
void   juno_set_param(juno_synth *s, int id, double v);
double juno_get_param(const juno_synth *s, int id);
int    juno_param_max(int id);   /* 0 for continuous, else the top integer */

juno_synth *juno_create(double samplerate);
void        juno_destroy(juno_synth *s);

void juno_set_patch(juno_synth *s, const juno_patch *p);
void juno_note_on(juno_synth *s, int note, int velocity);
void juno_note_off(juno_synth *s, int note);
void juno_all_off(juno_synth *s);
void juno_render(juno_synth *s, double *out, int frames);   /* interleaved stereo */

/* A small set of hand-written patches, so there is something to play. */
int               juno_factory_count(void);
const juno_patch *juno_factory(int i);

#ifdef __cplusplus
}
#endif

#endif /* JUNO_H */
