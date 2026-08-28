#include "juno.h"
#include "dw_dsp.h"     /* dw_filter (the 4-pole ladder) and dw_limit */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- chorus -------------------------------------------------------------
 * The Juno's chorus is a pair of BBD lines swept by a triangle. Mode I is a
 * slow shallow sweep, mode II a faster deeper one; the two channels are swept
 * in antiphase, which is where the width comes from. */

#define CH_MAX 2048

typedef struct {
    float  bufL[CH_MAX], bufR[CH_MAX];
    int    w;
    double phase, inc, depth, base, sr;
    int    on;
} juno_chorus;

static void chorus_set(juno_chorus *c, juno_chorus_mode m, double sr)
{
    c->sr = sr;
    c->on = (m != JUNO_CH_OFF);
    /* APPROX: rates and depths chosen to sit in the usual BBD range. */
    if (m == JUNO_CH_I)  { c->inc = 0.513 / sr; c->depth = 0.0013 * sr; }
    else                 { c->inc = 0.863 / sr; c->depth = 0.0021 * sr; }
    c->base = 0.0032 * sr;
    if (c->base + c->depth > CH_MAX - 4) c->base = CH_MAX - 4 - c->depth;
}

static double tap(const float *b, int w, double d)
{
    double p = (double)w - d;
    int i0, i1;
    double f;
    while (p < 0.0) p += CH_MAX;
    i0 = (int)p;
    f  = p - i0;
    i1 = (i0 + 1) & (CH_MAX - 1);
    i0 &= (CH_MAX - 1);
    return b[i0] + f * (b[i1] - b[i0]);
}

static void chorus_run(juno_chorus *c, double in, double *l, double *r)
{
    double tri, dl, dr;

    if (!c->on) { *l = *r = in; return; }

    c->bufL[c->w] = (float)in;
    c->bufR[c->w] = (float)in;

    /* triangle, -1..1 */
    tri = c->phase < 0.5 ? (4.0 * c->phase - 1.0) : (3.0 - 4.0 * c->phase);
    c->phase += c->inc;
    if (c->phase >= 1.0) c->phase -= 1.0;

    dl = c->base + c->depth * tri;
    dr = c->base - c->depth * tri;        /* antiphase: the stereo spread */

    *l = 0.7 * in + 0.7 * tap(c->bufL, c->w, dl);
    *r = 0.7 * in + 0.7 * tap(c->bufR, c->w, dr);

    c->w = (c->w + 1) & (CH_MAX - 1);
}

/* ---- envelope ----------------------------------------------------------- */

typedef struct {
    int    stage;          /* 0 idle, 1 A, 2 D, 3 S, 4 R */
    double level, inc_a, c_d, c_r, sus;
} juno_eg;

/* APPROX: the panel's 0..1 sweeps map onto these ranges. */
static double eg_time(double v, double lo, double hi)
{
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return lo * pow(hi / lo, v);
}

static double coef(double sec, double sr)
{
    double n = sec * sr;
    if (n < 1.0) n = 1.0;
    return exp(-6.9 / n);
}

/* ---- voice -------------------------------------------------------------- */

typedef struct {
    int       active, note, held;
    unsigned  age;
    double    vel;
    double    phase, inc;      /* DCO, 0..1 per cycle */
    double    subphase;
    juno_eg   eg;
    dw_filter lpf;
    double    hp_z;            /* one-pole highpass state */
} juno_voice;

struct juno_synth {
    double      sr;
    juno_patch  p;
    juno_voice  v[JUNO_MAX_VOICES];
    int         nv;
    unsigned    counter;
    double      lfo_phase, lfo_inc, lfo_env;
    juno_chorus ch;
    double      pw;            /* current pulse width */
};

/* PolyBLEP: removes the worst of the aliasing from the hard edges without a
 * wavetable, which suits the Juno's single oscillator. */
static double blep(double t, double dt)
{
    if (t < dt)        { t /= dt;       return t + t - t * t - 1.0; }
    if (t > 1.0 - dt)  { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

static double note_hz(double n) { return 440.0 * pow(2.0, (n - 69.0) / 12.0); }

juno_synth *juno_create(double sr)
{
    juno_synth *s = calloc(1, sizeof *s);
    int i;
    if (!s) return NULL;
    s->sr = sr;
    s->nv = 6;                       /* the Juno-6 is six-voice */
    for (i = 0; i < JUNO_MAX_VOICES; i++) dw_filter_init(&s->v[i].lpf, sr);
    chorus_set(&s->ch, JUNO_CH_OFF, sr);
    return s;
}

void juno_destroy(juno_synth *s) { free(s); }

void juno_set_patch(juno_synth *s, const juno_patch *p)
{
    s->p = *p;
    /* APPROX: 0.05..25 Hz covers the panel sweep. */
    s->lfo_inc = (0.05 * pow(500.0, p->lfo_rate)) / s->sr;
    chorus_set(&s->ch, p->chorus, s->sr);
}

static void voice_on(juno_synth *s, juno_voice *v, int note, int vel)
{
    const juno_patch *p = &s->p;
    v->active = v->held = 1;
    v->note = note;
    v->vel  = vel / 127.0;
    v->age  = ++s->counter;
    v->phase = v->subphase = 0.0;
    v->hp_z = 0.0;
    memset(v->lpf.s, 0, sizeof v->lpf.s);

    v->eg.stage = 1;
    v->eg.level = 0.0;
    {
        double n = eg_time(p->a, 0.002, 6.0) * s->sr;
        v->eg.inc_a = (n < 1.0) ? 1.0 : 1.0 / n;
    }
    v->eg.c_d = coef(eg_time(p->d, 0.005, 12.0), s->sr);
    v->eg.c_r = coef(eg_time(p->r, 0.005, 12.0), s->sr);
    v->eg.sus = p->s;
}

static juno_voice *pick(juno_synth *s)
{
    int i, best = 0;
    unsigned oldest = ~0u;
    for (i = 0; i < s->nv; i++) if (!s->v[i].active) return &s->v[i];
    for (i = 0; i < s->nv; i++)
        if (s->v[i].age < oldest) { oldest = s->v[i].age; best = i; }
    return &s->v[best];
}

void juno_note_on(juno_synth *s, int note, int vel)
{
    voice_on(s, pick(s), note, vel);
    if (s->p.lfo_delay > 0.0) s->lfo_env = 0.0;
}

void juno_note_off(juno_synth *s, int note)
{
    int i;
    for (i = 0; i < JUNO_MAX_VOICES; i++)
        if (s->v[i].active && s->v[i].held && s->v[i].note == note) {
            s->v[i].held = 0;
            s->v[i].eg.stage = 4;
        }
}

void juno_all_off(juno_synth *s)
{
    int i;
    for (i = 0; i < JUNO_MAX_VOICES; i++) {
        s->v[i].active = s->v[i].held = 0;
        s->v[i].eg.stage = 0;
        s->v[i].eg.level = 0.0;
    }
}

static double eg_run(juno_eg *e)
{
    switch (e->stage) {
    case 1:
        e->level += e->inc_a;
        if (e->level >= 1.0) { e->level = 1.0; e->stage = 2; }
        break;
    case 2:
        e->level = e->sus + (e->level - e->sus) * e->c_d;
        if (fabs(e->level - e->sus) < 1e-4) { e->level = e->sus; e->stage = 3; }
        break;
    case 3:
        e->level = e->sus;
        break;
    case 4:
        e->level *= e->c_r;
        if (e->level < 1e-5) { e->level = 0.0; e->stage = 0; }
        break;
    default:
        return 0.0;
    }
    return e->level;
}

void juno_render(juno_synth *s, double *out, int frames)
{
    const juno_patch *p = &s->p;
    static const double RANGE[3] = { -12.0, 0.0, 12.0 };
    /* HPF position 0 is the Juno's bass boost; 1..3 lift progressively. */
    static const double HPF_HZ[4] = { 0.0, 120.0, 320.0, 800.0 };
    int n, i;

    for (n = 0; n < frames; n++) {
        double lfo, mix = 0.0, l, r;

        lfo = s->lfo_phase < 0.5 ? (4.0 * s->lfo_phase - 1.0)
                                 : (3.0 - 4.0 * s->lfo_phase);
        s->lfo_phase += s->lfo_inc;
        if (s->lfo_phase >= 1.0) s->lfo_phase -= 1.0;

        /* LFO delay: fade the LFO in after a note starts. */
        if (p->lfo_delay > 0.0) {
            double t = eg_time(p->lfo_delay, 0.01, 3.0);
            s->lfo_env += 1.0 / (t * s->sr);
            if (s->lfo_env > 1.0) s->lfo_env = 1.0;
        } else {
            s->lfo_env = 1.0;
        }
        lfo *= s->lfo_env;

        /* Pulse width: either the manual setting or swept by the LFO. */
        s->pw = p->pwm_manual ? (0.5 - 0.45 * p->pwm)
                              : (0.5 - 0.45 * p->pwm * (0.5 + 0.5 * lfo));
        if (s->pw < 0.05) s->pw = 0.05;
        if (s->pw > 0.95) s->pw = 0.95;

        for (i = 0; i < s->nv; i++) {
            juno_voice *v = &s->v[i];
            double env, o = 0.0, dt, t, hz, cut, amp;

            if (!v->active) continue;
            env = eg_run(&v->eg);
            if (!v->eg.stage && env <= 0.0) { v->active = 0; continue; }

            hz = note_hz((double)v->note + RANGE[p->range & 3]
                         + lfo * p->dco_lfo * 0.5);
            v->inc = hz / s->sr;
            dt = v->inc;
            t  = v->phase;

            if (p->saw)   o += -(2.0 * t - 1.0) + blep(t, dt);
            if (p->pulse) {
                double sq = (t < s->pw) ? 1.0 : -1.0;
                sq += blep(t, dt);
                sq -= blep(fmod(t + 1.0 - s->pw, 1.0), dt);
                o += sq * 0.8;
            }
            if (p->sub > 0.0) {
                double sub = (v->subphase < 0.5) ? 1.0 : -1.0;
                sub += blep(v->subphase, dt * 0.5);
                sub -= blep(fmod(v->subphase + 0.5, 1.0), dt * 0.5);
                o += sub * p->sub * 0.9;
                v->subphase += dt * 0.5;
                if (v->subphase >= 1.0) v->subphase -= 1.0;
            }
            if (p->noise > 0.0) {
                /* cheap white noise, per voice */
                static uint32_t rng = 2463534242u;
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                o += ((double)(int32_t)rng / 2147483648.0) * p->noise * 0.7;
            }

            v->phase += dt;
            if (v->phase >= 1.0) v->phase -= 1.0;

            /* non-resonant highpass ahead of the filter */
            if (p->hpf > 0) {
                double g = 1.0 - exp(-2.0 * M_PI * HPF_HZ[p->hpf & 3] / s->sr);
                v->hp_z += g * (o - v->hp_z);
                o -= v->hp_z;
            } else {
                o *= 1.35;                       /* position 0 boosts the bass */
            }

            /* Cutoff range taken from TAL-U-NO-62's decompiled pitch/filter
             * routine (FUN_1800033f0), which carries 5000 as its ceiling
             * alongside 440 and 17.3123 = 12/ln(2) for exponential pitch. */
            cut = 20.0 * pow(5000.0 / 20.0, p->cutoff);
            cut *= pow(2.0, (p->vcf_env_neg ? -1.0 : 1.0) * p->vcf_env * env * 5.0);
            cut *= pow(2.0, lfo * p->vcf_lfo * 2.0);
            cut *= pow(2.0, p->vcf_key * ((double)v->note - 60.0) / 12.0);
            dw_filter_set(&v->lpf, cut, p->res);
            o = dw_filter_process(&v->lpf, o);

            amp = p->vca_gate ? (v->eg.stage ? 1.0 : 0.0) : env;
            mix += o * amp * (0.35 + 0.65 * v->vel);
        }

        mix *= 0.75 * p->volume;
        chorus_run(&s->ch, mix, &l, &r);
        out[2 * n]     = dw_limit(l);
        out[2 * n + 1] = dw_limit(r);
    }
}

/* ---- hand-written patches ----------------------------------------------
 * The Juno-6 stores nothing, so these are mine, not Roland's. They aim at the
 * sounds the panel is famous for rather than at any particular factory sheet. */

static const juno_patch FACTORY[] = {
 /* name          rng lfo  pwm man saw pul sub  noi hpf cut  res  env neg lfo  key gate vol   a     d     s     r    lfoR lfoD chorus */
 { "Juno Brass",   1, 0.00,0.35,0, 1, 1, 0.30,0.00, 0, 0.42,0.28,0.55,0, 0.00,0.35,0, 0.85, 0.06, 0.45, 0.62, 0.22, 0.35,0.15, JUNO_CH_I  },
 { "Strings",      1, 0.06,0.60,0, 1, 1, 0.20,0.00, 1, 0.34,0.16,0.30,0, 0.05,0.30,0, 0.80, 0.30, 0.60, 0.85, 0.45, 0.28,0.35, JUNO_CH_II },
 { "Fat Bass",     0, 0.00,0.20,1, 1, 1, 0.75,0.00, 0, 0.24,0.42,0.55,0, 0.00,0.25,0, 0.90, 0.00, 0.30, 0.15, 0.12, 0.30,0.00, JUNO_CH_OFF},
 { "Sync Lead",    2, 0.10,0.30,1, 1, 1, 0.15,0.00, 1, 0.55,0.55,0.40,0, 0.10,0.55,0, 0.85, 0.02, 0.35, 0.70, 0.18, 0.45,0.20, JUNO_CH_I  },
 { "Soft Pad",     1, 0.05,0.75,0, 1, 1, 0.25,0.00, 1, 0.28,0.12,0.28,0, 0.08,0.30,0, 0.78, 0.45, 0.70, 0.90, 0.60, 0.22,0.40, JUNO_CH_II },
 { "Pluck",        1, 0.00,0.25,1, 1, 0, 0.35,0.00, 1, 0.38,0.50,0.70,0, 0.00,0.40,0, 0.88, 0.00, 0.22, 0.00, 0.16, 0.30,0.00, JUNO_CH_I  },
 { "Organ",        1, 0.00,0.50,1, 0, 1, 0.60,0.00, 2, 0.62,0.05,0.00,0, 0.00,0.20,1, 0.82, 0.00, 0.50, 1.00, 0.08, 0.30,0.00, JUNO_CH_II },
 { "Sweep Pad",    1, 0.04,0.70,0, 1, 1, 0.20,0.00, 1, 0.20,0.62,0.85,0, 0.06,0.25,0, 0.78, 0.55, 0.85, 0.75, 0.65, 0.15,0.30, JUNO_CH_II },
 { "Clav",         1, 0.00,0.15,1, 0, 1, 0.30,0.00, 2, 0.45,0.45,0.60,0, 0.00,0.50,0, 0.86, 0.00, 0.20, 0.05, 0.12, 0.30,0.00, JUNO_CH_OFF},
 { "Noise Sweep",  1, 0.00,0.50,1, 0, 0, 0.00,0.85, 0, 0.30,0.55,0.90,0, 0.00,0.00,0, 0.75, 0.35, 0.70, 0.40, 0.55, 0.12,0.25, JUNO_CH_II },
 { "Sub Bass",     0, 0.00,0.50,1, 0, 1, 0.95,0.00, 0, 0.18,0.20,0.30,0, 0.00,0.15,0, 0.92, 0.01, 0.40, 0.55, 0.15, 0.30,0.00, JUNO_CH_OFF},
 { "Vibes",        2, 0.08,0.40,1, 0, 1, 0.25,0.00, 2, 0.50,0.30,0.55,0, 0.00,0.45,0, 0.84, 0.00, 0.35, 0.10, 0.30, 0.55,0.10, JUNO_CH_I  },
};

/* ---- run-time parameter access ------------------------------------------ */

static const struct { const char *name; int max; } PINFO[JP_COUNT] = {
    { "DCO Range",      2 }, { "DCO LFO",        0 }, { "PWM",            0 },
    { "PWM Manual",     1 }, { "Saw",            1 }, { "Pulse",          1 },
    { "Sub Level",      0 }, { "Noise",          0 }, { "HPF",            3 },
    { "VCF Cutoff",     0 }, { "VCF Resonance",  0 }, { "VCF Env",        0 },
    { "VCF Env Neg",    1 }, { "VCF LFO",        0 }, { "VCF Key Follow", 0 },
    { "VCA Gate",       1 }, { "Volume",         0 }, { "Attack",         0 },
    { "Decay",          0 }, { "Sustain",        0 }, { "Release",        0 },
    { "LFO Rate",       0 }, { "LFO Delay",      0 }, { "Chorus",         2 },
};

const char *juno_param_name(int id)
{
    return (id >= 0 && id < JP_COUNT) ? PINFO[id].name : 0;
}

int juno_param_max(int id)
{
    return (id >= 0 && id < JP_COUNT) ? PINFO[id].max : 0;
}

double juno_get_param(const juno_synth *s, int id)
{
    const juno_patch *p = &s->p;
    switch (id) {
    case JP_RANGE:      return p->range;
    case JP_DCO_LFO:    return p->dco_lfo;
    case JP_PWM:        return p->pwm;
    case JP_PWM_MANUAL: return p->pwm_manual;
    case JP_SAW:        return p->saw;
    case JP_PULSE:      return p->pulse;
    case JP_SUB:        return p->sub;
    case JP_NOISE:      return p->noise;
    case JP_HPF:        return p->hpf;
    case JP_CUTOFF:     return p->cutoff;
    case JP_RES:        return p->res;
    case JP_VCF_ENV:    return p->vcf_env;
    case JP_VCF_ENV_NEG:return p->vcf_env_neg;
    case JP_VCF_LFO:    return p->vcf_lfo;
    case JP_VCF_KEY:    return p->vcf_key;
    case JP_VCA_GATE:   return p->vca_gate;
    case JP_VOLUME:     return p->volume;
    case JP_A:          return p->a;
    case JP_D:          return p->d;
    case JP_S:          return p->s;
    case JP_R:          return p->r;
    case JP_LFO_RATE:   return p->lfo_rate;
    case JP_LFO_DELAY:  return p->lfo_delay;
    case JP_CHORUS:     return p->chorus;
    default:            return 0.0;
    }
}

void juno_set_param(juno_synth *s, int id, double v)
{
    juno_patch *p = &s->p;
    int iv = (int)(v + 0.5);
    switch (id) {
    case JP_RANGE:      p->range = iv < 0 ? 0 : (iv > 2 ? 2 : iv); break;
    case JP_DCO_LFO:    p->dco_lfo = v;    break;
    case JP_PWM:        p->pwm = v;        break;
    case JP_PWM_MANUAL: p->pwm_manual = !!iv; break;
    case JP_SAW:        p->saw = !!iv;     break;
    case JP_PULSE:      p->pulse = !!iv;   break;
    case JP_SUB:        p->sub = v;        break;
    case JP_NOISE:      p->noise = v;      break;
    case JP_HPF:        p->hpf = iv < 0 ? 0 : (iv > 3 ? 3 : iv); break;
    case JP_CUTOFF:     p->cutoff = v;     break;
    case JP_RES:        p->res = v;        break;
    case JP_VCF_ENV:    p->vcf_env = v;    break;
    case JP_VCF_ENV_NEG:p->vcf_env_neg = !!iv; break;
    case JP_VCF_LFO:    p->vcf_lfo = v;    break;
    case JP_VCF_KEY:    p->vcf_key = v;    break;
    case JP_VCA_GATE:   p->vca_gate = !!iv; break;
    case JP_VOLUME:     p->volume = v;     break;
    case JP_A:          p->a = v;          break;
    case JP_D:          p->d = v;          break;
    case JP_S:          p->s = v;          break;
    case JP_R:          p->r = v;          break;
    case JP_LFO_RATE:   p->lfo_rate = v;
        s->lfo_inc = (0.05 * pow(500.0, v)) / s->sr; break;
    case JP_LFO_DELAY:  p->lfo_delay = v;  break;
    case JP_CHORUS:     p->chorus = (juno_chorus_mode)(iv < 0 ? 0 : (iv > 2 ? 2 : iv));
        chorus_set(&s->ch, p->chorus, s->sr); break;
    default: break;
    }
}

int juno_factory_count(void) { return (int)(sizeof FACTORY / sizeof *FACTORY); }

const juno_patch *juno_factory(int i)
{
    if (i < 0 || i >= juno_factory_count()) return NULL;
    return &FACTORY[i];
}
