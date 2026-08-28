#include "fm_synth.h"
#include "dw_dsp.h"   /* dw_limit(): the same master limiter the DW engine uses */

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The eight 4-operator algorithms, in FB-02's operator order.
 *
 * Note the direction: the carrier is OP1 and the chain runs *down* from OP4,
 * the reverse of how the algorithms are usually drawn. The factory bank makes
 * that unambiguous -- it contains two-operator patches ("2OP Gum", "Zwicker")
 * with OP3 and OP4 disabled, which can only sound if OP1 is the carrier.
 * Checked across all 336 programs: with this order every program has at least
 * one enabled carrier; with the conventional order, 7 would be silent.
 *
 * modmask[i] is the set of operators feeding operator i; carriers is the set
 * reaching the output. Feedback is on OP4, the head of the chain. */
static const struct { unsigned char modmask[4], carriers; } ALG[8] = {
    /* 0: 4->3->2->1                */ { { 1u<<1, 1u<<2, 1u<<3, 0 }, 1u<<0 },
    /* 1: (4+3)->2->1               */ { { 1u<<1, (1u<<2)|(1u<<3), 0, 0 }, 1u<<0 },
    /* 2: 4->1, 3->2->1             */ { { (1u<<1)|(1u<<3), 1u<<2, 0, 0 }, 1u<<0 },
    /* 3: 4->3->1, 2->1             */ { { (1u<<1)|(1u<<2), 0, 1u<<3, 0 }, 1u<<0 },
    /* 4: 4->3, 2->1                */ { { 1u<<1, 0, 1u<<3, 0 }, (1u<<0)|(1u<<2) },
    /* 5: 4->3, 4->2, 4->1          */ { { 1u<<3, 1u<<3, 1u<<3, 0 }, (1u<<0)|(1u<<1)|(1u<<2) },
    /* 6: 4->3, 2, 1                */ { { 0, 0, 1u<<3, 0 }, (1u<<0)|(1u<<1)|(1u<<2) },
    /* 7: all parallel              */ { { 0, 0, 0, 0 }, 0xf },
};

#define FM_FEEDBACK_OP 3   /* OP4: the head of every chain above */

unsigned fm_algorithm_carriers(int alg)
{
    if (alg < 0 || alg > 7) alg = 0;
    return ALG[alg].carriers;
}

/* Envelope RATES, not times: Yamaha's AR/D1R/D2R/RR count upward for faster,
 * the opposite of the DW-8000's envelope parameters, which are durations. Pass
 * the stored value straight in -- subtracting it from the maximum first
 * inverts the mapping a second time and turns every instant attack into a
 * fifteen-second fade. APPROX: only the endpoint times are a judgement call. */
#define FM_T_FAST 0.0015
#define FM_T_SLOW 15.0

static double rate_time(double r, double rmax)
{
    double x = r / rmax;
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    return FM_T_SLOW * pow(FM_T_FAST / FM_T_SLOW, x);
}

static double approach(double seconds, double sr)
{
    double n = seconds * sr;
    if (n < 1.0) n = 1.0;
    return exp(-6.9 / n);
}

/* Total Level is an attenuation: 0 is loudest. Confirmed against the factory
 * bank -- 825 enabled modulators have a median Level of 0, which only makes
 * sense as "wide open"; read as amplitude it would mean the entire bank is
 * unmodulated sine. APPROX: 0.75 dB per step, the usual Yamaha figure. */
static double tl_gain(int tl)
{
    if (tl < 0) tl = 0;
    if (tl > 127) tl = 127;
    return pow(10.0, -(double)tl * 0.75 / 20.0);
}

/* Sustain level, 0 loudest .. 15 silent, 3 dB per step. */
static double sl_level(int sl)
{
    if (sl >= 15) return 0.0;
    if (sl < 0) sl = 0;
    return pow(10.0, -(double)sl * 3.0 / 20.0);
}

/* APPROX: DT1-style fine detune in cents, and DT2-style coarse ratios. */
static const double DETUNE_CENTS[8] = { 0, 3.4, 6.8, 10.2, 0, -3.4, -6.8, -10.2 };
static const double INHARMONIC[4]   = { 1.0, 1.4142135, 1.5874010, 1.7320508 };

/* The TX81Z waveform set. Every factory program uses wave 0, so only the sine
 * matters for the stock banks; the rest are FB-02's own addition. APPROX. */
static double fm_wave(int w, double ph)
{
    double s = sin(2.0 * M_PI * ph);
    double a;
    switch (w) {
    case 0:  return s;
    case 1:  return s > 0.0 ? s : 0.0;
    case 2:  return fabs(s);
    case 3:  a = fmod(ph, 0.5); return a < 0.25 ? fabs(s) : 0.0;
    case 4:  return ph < 0.5 ? sin(4.0 * M_PI * ph) : 0.0;
    case 5:  return ph < 0.5 ? fabs(sin(4.0 * M_PI * ph)) : 0.0;
    case 6:  return s >= 0.0 ? 1.0 : -1.0;
    default: return ph < 0.5 ? 1.0 : 0.0;
    }
}

static double note_hz(int note)
{
    return 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);
}

int fm_synth_init(fm_synth *s, double samplerate)
{
    memset(s, 0, sizeof *s);
    s->samplerate = samplerate;
    s->nvoices    = 8;
    s->gain       = 0.6;   /* Calibrated after the envelope-rate fix: with correct (mostly instant)
                            * attacks the bank is far hotter than it was with the rates
                            * inverted, so this is much lower than it once needed to be. */
    return 0;
}

void fm_synth_set_program(fm_synth *s, const fb02_program *p)
{
    s->prog = *p;
    s->have_prog = 1;
    /* APPROX: LFO speed 0..255 mapped over a musical range. */
    s->lfo_inc = (0.05 + (double)p->lfo_speed / 255.0 * 12.0) / s->samplerate;
}

static void voice_start(fm_synth *s, fm_voice *v, int note, int vel)
{
    const fb02_program *p = &s->prog;
    double base = note_hz(note + p->transpose);
    int    i;

    v->active = 1;
    v->held   = 1;
    v->note   = note;
    v->velocity = vel / 127.0;
    v->age = ++s->counter;
    v->fb1 = v->fb2 = 0.0;

    for (i = 0; i < FB02_OPS; i++) {
        const fb02_op *o = &p->op[i];
        fm_op *e = &v->op[i];
        double mul = (o->frequency == 0) ? 0.5 : (double)o->frequency;
        double f;

        f = base * mul * INHARMONIC[o->inharmonic & 3];
        f *= pow(2.0, DETUNE_CENTS[o->detune & 7] / 1200.0);

        e->phase = 0.0;
        e->inc   = f / s->samplerate;
        e->out = e->prev = 0.0;

        /* Velocity scales the operator's output; how much is per-operator. */
        {
            double vsens = (double)(o->velocity & 7) / 7.0;
            double vscale = 1.0 - vsens * (1.0 - v->velocity);
            e->gain = tl_gain(o->level) * vscale;
            if (!o->enable) e->gain = 0.0;
        }

        e->sustain = sl_level(o->sustain);
        {
            double n = rate_time(o->attack, 31.0) * s->samplerate;
            e->inc_a = (n < 1.0) ? 1.0 : 1.0 / n;
        }
        e->c_d1 = approach(rate_time(o->decay1, 31.0), s->samplerate);
        e->c_d2 = approach(rate_time(o->decay2, 31.0), s->samplerate);
        e->c_rr = approach(rate_time(o->release, 15.0), s->samplerate);
        e->stage = 1;
        e->level = 0.0;
    }
}

static fm_voice *pick(fm_synth *s)
{
    int i, best = 0;
    unsigned oldest = ~0u;
    for (i = 0; i < s->nvoices; i++)
        if (!s->voice[i].active) return &s->voice[i];
    for (i = 0; i < s->nvoices; i++)
        if (s->voice[i].age < oldest) { oldest = s->voice[i].age; best = i; }
    return &s->voice[best];
}

void fm_synth_note_on(fm_synth *s, int note, int velocity)
{
    if (!s->have_prog) return;
    voice_start(s, pick(s), note, velocity);
    if (!s->prog.lfo_sync) return;
    s->lfo_phase = 0.0;
}

void fm_synth_note_off(fm_synth *s, int note)
{
    int i, k;
    for (i = 0; i < FM_MAX_VOICES; i++) {
        fm_voice *v = &s->voice[i];
        if (!v->active || !v->held || v->note != note) continue;
        v->held = 0;
        for (k = 0; k < FB02_OPS; k++)
            if (v->op[k].stage) v->op[k].stage = 4;
    }
}

void fm_synth_all_off(fm_synth *s)
{
    int i, k;
    for (i = 0; i < FM_MAX_VOICES; i++) {
        s->voice[i].active = s->voice[i].held = 0;
        for (k = 0; k < FB02_OPS; k++) {
            s->voice[i].op[k].stage = 0;
            s->voice[i].op[k].level = 0.0;
        }
    }
}

int fm_synth_busy(const fm_synth *s)
{
    int i;
    for (i = 0; i < FM_MAX_VOICES; i++) if (s->voice[i].active) return 1;
    return 0;
}

static double eg_step(fm_op *e)
{
    switch (e->stage) {
    case 1:
        e->level += e->inc_a;
        if (e->level >= 1.0) { e->level = 1.0; e->stage = 2; }
        break;
    case 2:
        e->level = e->sustain + (e->level - e->sustain) * e->c_d1;
        if (fabs(e->level - e->sustain) < 1e-4) { e->level = e->sustain; e->stage = 3; }
        break;
    case 3:
        e->level *= e->c_d2;
        if (e->level < 1e-5) { e->level = 0.0; e->stage = 0; }
        break;
    case 4:
        e->level *= e->c_rr;
        if (e->level < 1e-5) { e->level = 0.0; e->stage = 0; }
        break;
    default:
        return 0.0;
    }
    return e->level;
}

void fm_synth_render(fm_synth *s, double *out, int frames)
{
    const fb02_program *p = &s->prog;
    const unsigned carriers = fm_algorithm_carriers(p->algorithm);
    /* APPROX: feedback 0..7 into a modulation index. */
    const double fbamt = p->feedback ? pow(2.0, (double)p->feedback - 7.0) * 2.0 : 0.0;
    int n, i, k;

    for (n = 0; n < frames; n++) {
        double mix = 0.0;
        double lfo = 0.0, pm = 0.0, am = 1.0;

        if (p->lfo_enable) {
            lfo = sin(2.0 * M_PI * s->lfo_phase);
            s->lfo_phase += s->lfo_inc;
            if (s->lfo_phase >= 1.0) s->lfo_phase -= 1.0;
            pm = lfo * ((double)p->lfo_pm_depth / 127.0)
                     * ((double)p->lfo_pm_sens / 7.0) * 0.02;
            am = 1.0 - (0.5 + 0.5 * lfo) * ((double)p->lfo_am_depth / 127.0)
                                         * ((double)p->lfo_am_sens / 3.0);
        }

        for (i = 0; i < s->nvoices; i++) {
            fm_voice *v = &s->voice[i];
            double vout = 0.0;
            int live = 0;

            if (!v->active) continue;

            for (k = 0; k < FB02_OPS; k++) {
                fm_op *e = &v->op[k];
                double mod = 0.0, env, ph;
                unsigned m = ALG[p->algorithm & 7].modmask[k];

                if (!e->stage && e->level <= 0.0) { e->out = 0.0; continue; }
                live = 1;

                /* Modulators are read from the previous sample, as the
                 * hardware's pipeline does. */
                if (m & 1u) mod += v->op[0].prev;
                if (m & 2u) mod += v->op[1].prev;
                if (m & 4u) mod += v->op[2].prev;
                if (m & 8u) mod += v->op[3].prev;
                if (k == FM_FEEDBACK_OP && fbamt > 0.0)
                    mod += (v->fb1 + v->fb2) * 0.5 * fbamt;

                env = eg_step(e);
                ph  = e->phase + mod + pm;
                ph -= floor(ph);
                e->out = fm_wave(p->op[k].wave, ph) * env * e->gain;

                e->phase += e->inc;
                if (e->phase >= 1.0) e->phase -= floor(e->phase);

                if (carriers & (1u << k)) vout += e->out;
            }

            v->fb2 = v->fb1;
            v->fb1 = v->op[FM_FEEDBACK_OP].out;
            for (k = 0; k < FB02_OPS; k++) v->op[k].prev = v->op[k].out;

            if (!live) { v->active = 0; continue; }
            mix += vout;
        }

        mix *= s->gain * am * 0.5;
        out[2 * n]     = dw_limit(mix);
        out[2 * n + 1] = dw_limit(mix);
    }
}
