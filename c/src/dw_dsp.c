#include "dw_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- envelope --------------------------------------------------------- */

/* Defaults for everything the reversing did not pin down. Musically sensible
 * approximations, not measurements of the original.
 *
 * cutoff_min started at 20 Hz, which put the low stored cutoff values below the
 * fundamental of the note being played -- SYNTH BASS 1 stores Cutoff 9, which
 * mapped to 52 Hz against a 65 Hz note, so the filter removed almost the entire
 * tone and all that was left was the envelope's brief opening spike. That reads
 * as a chirp rather than a bass. Starting at 100 Hz keeps low settings dark
 * without erasing the note. */
dw_tuning dw_tune = {
    /* eg_min, eg_max        */ 0.002, 20.0,
    /* vcf_eg_octaves        */ 6.0,
    /* cutoff_min, octaves   */ 100.0, 7.3,
    /* mg_min, mg_max        */ 0.1, 30.0,
    /* delay_min/_max/_mix   */ 0.008, 0.21, 0.45,
    /* gain                  */ 5.5
};

static void env_d(const char *name, double *dst)
{
    const char *s = getenv(name);
    char       *end;
    double      v;
    if (!s || !*s) return;
    v = strtod(s, &end);
    if (end != s && v > 0.0) *dst = v;
}

void dw_tuning_from_env(void)
{
    env_d("DW_EG_MIN",         &dw_tune.eg_min);
    env_d("DW_EG_MAX",         &dw_tune.eg_max);
    env_d("DW_VCF_EG_OCTAVES", &dw_tune.vcf_eg_octaves);
    env_d("DW_CUTOFF_MIN",     &dw_tune.cutoff_min);
    env_d("DW_CUTOFF_OCTAVES", &dw_tune.cutoff_octaves);
    env_d("DW_MG_MIN",         &dw_tune.mg_min);
    env_d("DW_MG_MAX",         &dw_tune.mg_max);
    env_d("DW_DELAY_MIN",      &dw_tune.delay_min);
    env_d("DW_DELAY_MAX",      &dw_tune.delay_max);
    env_d("DW_DELAY_MIX",      &dw_tune.delay_mix);
    env_d("DW_GAIN",           &dw_tune.gain);
}

void dw_tuning_print(void)
{
    printf("tuning: EG %.4f..%.1fs  VCF EG %.2f oct  cutoff %.0f Hz +%.2f oct"
           "  MG %.2f..%.1f Hz\n",
           dw_tune.eg_min, dw_tune.eg_max, dw_tune.vcf_eg_octaves,
           dw_tune.cutoff_min, dw_tune.cutoff_octaves,
           dw_tune.mg_min, dw_tune.mg_max);
}

double dw_eg_time(double v031)
{
    double x = v031 / 31.0;
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    return dw_tune.eg_min * pow(dw_tune.eg_max / dw_tune.eg_min, x);
}

/* Per-sample coefficient for an exponential approach that lands within about
 * 0.1% of the target after `seconds`. */
static double approach_coef(double seconds, double samplerate)
{
    double n = seconds * samplerate;
    if (n < 1.0) n = 1.0;
    return exp(-6.9 / n);
}

void dw_eg_init(dw_eg *e, double samplerate)
{
    memset(e, 0, sizeof *e);
    e->samplerate = samplerate;
    e->stage = DW_EG_IDLE;
}

void dw_eg_config(dw_eg *e, const dw_eg_cfg *cfg)
{
    e->cfg = *cfg;
}

void dw_eg_gate_on(dw_eg *e)
{
    double n = e->cfg.attack * e->samplerate;
    e->stage = DW_EG_ATTACK;
    e->inc   = (n < 1.0) ? 1.0 : 1.0 / n;
}

void dw_eg_gate_off(dw_eg *e)
{
    /* A release of literally zero cuts the waveform mid-cycle and clicks.
     * Plenty of factory patches store Release = 0, so floor it at a few
     * milliseconds -- short enough to still read as an instant stop. */
    double r = e->cfg.release < 0.006 ? 0.006 : e->cfg.release;

    if (e->stage == DW_EG_IDLE) return;
    e->stage  = DW_EG_RELEASE;
    e->target = 0.0;
    e->coef   = approach_coef(r, e->samplerate);
}

double dw_eg_process(dw_eg *e)
{
    switch (e->stage) {
    case DW_EG_IDLE:
        return 0.0;

    case DW_EG_ATTACK:
        e->level += e->inc;
        if (e->level >= 1.0) {
            e->level  = 1.0;
            e->stage  = DW_EG_DECAY;
            e->target = e->cfg.breakpoint;
            e->coef   = approach_coef(e->cfg.decay, e->samplerate);
        }
        break;

    case DW_EG_DECAY:
        e->level = e->target + (e->level - e->target) * e->coef;
        if (fabs(e->level - e->target) < 1e-4) {
            e->level  = e->target;
            e->stage  = DW_EG_SLOPE;
            e->target = e->cfg.sustain;
            e->coef   = approach_coef(e->cfg.slope, e->samplerate);
        }
        break;

    case DW_EG_SLOPE:
        e->level = e->target + (e->level - e->target) * e->coef;
        if (fabs(e->level - e->target) < 1e-4) {
            e->level = e->target;
            e->stage = DW_EG_SUSTAIN;
        }
        break;

    case DW_EG_SUSTAIN:
        e->level = e->cfg.sustain;
        break;

    case DW_EG_RELEASE:
        e->level *= e->coef;
        if (e->level < 1e-5) { e->level = 0.0; e->stage = DW_EG_IDLE; }
        break;
    }
    return e->level;
}

/* ---- filter ----------------------------------------------------------- */

void dw_filter_init(dw_filter *f, double samplerate)
{
    memset(f, 0, sizeof *f);
    f->samplerate = samplerate;
    dw_filter_set(f, 1000.0, 0.0);
}

void dw_filter_set(dw_filter *f, double cutoff_hz, double res01)
{
    double nyq = f->samplerate * 0.5;
    if (cutoff_hz < 10.0)        cutoff_hz = 10.0;
    if (cutoff_hz > nyq * 0.98)  cutoff_hz = nyq * 0.98;
    if (res01 < 0.0) res01 = 0.0;
    if (res01 > 1.0) res01 = 1.0;

    f->g   = 1.0 - exp(-2.0 * M_PI * cutoff_hz / f->samplerate);
    /* Just under 4.0 keeps the ladder stable at full resonance. */
    f->res = res01 * 3.94;
    /* A ladder peaks hard around cutoff as resonance comes up; without this
     * the loudest factory patches clip on their own. */
    f->comp = 1.0 / (1.0 + 2.2 * res01);
}

double dw_filter_process(dw_filter *f, double in)
{
    double x = in * f->comp - f->res * f->s[3];

    x = dw_softclip(x);
    f->s[0] += f->g * (x        - f->s[0]);
    f->s[1] += f->g * (f->s[0]  - f->s[1]);
    f->s[2] += f->g * (f->s[1]  - f->s[2]);
    f->s[3] += f->g * (f->s[2]  - f->s[3]);
    return f->s[3];
}

/* ---- MG --------------------------------------------------------------- */

void dw_mg_init(dw_mg *m, double samplerate)
{
    memset(m, 0, sizeof *m);
    m->samplerate = samplerate;
}

void dw_mg_set(dw_mg *m, dw_mg_wave w, double hz, double delay_sec)
{
    m->wave  = w;
    m->inc   = hz / m->samplerate;
    m->delay = delay_sec;
}

void dw_mg_retrigger(dw_mg *m)
{
    m->phase   = 0.0;
    m->elapsed = 0.0;
}

double dw_mg_process(dw_mg *m)
{
    double p = m->phase, v, fade = 1.0;

    m->phase += m->inc;
    if (m->phase >= 1.0) m->phase -= 1.0;

    switch (m->wave) {
    case DW_MG_TRI:  v = (p < 0.5) ? (4.0 * p - 1.0) : (3.0 - 4.0 * p); break;
    case DW_MG_SAW:  v = 2.0 * p - 1.0;                                 break;
    case DW_MG_RAMP: v = 1.0 - 2.0 * p;                                 break;
    default:         v = (p < 0.5) ? 1.0 : -1.0;                        break;
    }

    if (m->delay > 0.0) {
        if (m->elapsed < m->delay) {
            fade = m->elapsed / m->delay;
            m->elapsed += 1.0 / m->samplerate;
        }
    }
    return v * fade;
}

/* ---- delay ------------------------------------------------------------ */

int dw_delay_init(dw_delay *d, double samplerate, double max_seconds)
{
    memset(d, 0, sizeof *d);
    d->samplerate = samplerate;
    d->size = (int)(samplerate * max_seconds) + 4;
    if (!(d->buf = calloc((size_t)d->size, sizeof *d->buf))) return -1;
    return 0;
}

void dw_delay_free(dw_delay *d)
{
    free(d->buf);
    memset(d, 0, sizeof *d);
}

void dw_delay_set(dw_delay *d, double time_sec, double feedback,
                  double level, double mod_hz, double mod_depth_sec)
{
    d->delay_samples = time_sec * d->samplerate;
    if (d->delay_samples < 1.0) d->delay_samples = 1.0;
    if (d->delay_samples > d->size - 4) d->delay_samples = d->size - 4;
    d->feedback  = feedback;
    d->level     = level;
    d->mod_inc   = mod_hz / d->samplerate;
    d->mod_depth = mod_depth_sec * d->samplerate;
}

double dw_delay_process(dw_delay *d, double in)
{
    double mod, tap, frac, out;
    int    i0, i1;

    mod = sin(2.0 * M_PI * d->mod_phase) * d->mod_depth;
    d->mod_phase += d->mod_inc;
    if (d->mod_phase >= 1.0) d->mod_phase -= 1.0;

    tap = (double)d->write - (d->delay_samples + mod);
    while (tap < 0.0)              tap += d->size;
    while (tap >= (double)d->size) tap -= d->size;

    i0   = (int)tap;
    frac = tap - (double)i0;
    i1   = (i0 + 1 == d->size) ? 0 : i0 + 1;
    out  = d->buf[i0] + frac * (d->buf[i1] - d->buf[i0]);

    d->buf[d->write] = (float)(in + out * d->feedback);
    if (++d->write == d->size) d->write = 0;

    return out * d->level;
}
