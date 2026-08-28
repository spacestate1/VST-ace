#include "dw_synth.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Enum tables, read out of the binary's InitEnum call sites. */
static const double octave_semis[3]   = { -12.0, 0.0, 12.0 };        /* 16' 8' 4'   */
static const double interval_semis[5] = { 0.0, -3.0, 4.0, 5.0, 7.0 };/* 1 -3 3 4 5  */
static const double kbd_track[4]      = { 0.0, 0.25, 0.5, 1.0 };     /* 0 1/4 1/2 1 */

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Normalise a stored value to 0..1 using the parameter's real maximum, which
 * is not 31 for all of them -- Detune is 0..6, the delay's Feedback and Level
 * are 0..15, Delay Time is 0..7, After Touch is 0..3, the EG Velocities are
 * 0..7. The ranges come from the InitDouble call sites in FUN_18052f240. */
static double norm(double v, double max)
{
    return clampd(v, 0.0, max) / max;
}

static int idx_of(double v, int n)
{
    int i = (int)(v + 0.5);
    return i < 0 ? 0 : (i >= n ? n - 1 : i);
}

static double note_hz(double semitones_from_a4)
{
    return 440.0 * pow(2.0, semitones_from_a4 / 12.0);
}

int dw_synth_init(dw_synth *s, const dw_wavetable *wt, double samplerate)
{
    int i;

    memset(s, 0, sizeof *s);
    s->wt         = wt;
    s->samplerate = samplerate;
    s->nvoices    = 8;
    s->rng        = 22222u;

    for (i = 0; i < DW_MAX_VOICES; i++) {
        dw_eg_init(&s->voice[i].eg_vcf, samplerate);
        dw_eg_init(&s->voice[i].eg_vca, samplerate);
        dw_filter_init(&s->voice[i].filt, samplerate);
    }
    dw_mg_init(&s->mg, samplerate);
    if (dw_delay_init(&s->delay_l, samplerate, 1.2)) return -1;
    if (dw_delay_init(&s->delay_r, samplerate, 1.2)) { dw_delay_free(&s->delay_l); return -1; }
    return 0;
}

void dw_synth_free(dw_synth *s)
{
    dw_delay_free(&s->delay_l);
    dw_delay_free(&s->delay_r);
}

void dw_synth_set_program(dw_synth *s, const double *param)
{
    const double *p = param;
    int    set, i;
    double mgf, dtime;

    memcpy(s->param, param, sizeof s->param);

    /* Waveform selectors are 1..16; Wavetable Set picks which bank of 16. */
    set = idx_of(p[DWP_WAVETABLE_SET], 2);
    s->wave1 = (int)clampd(p[DWP_OSC1_WAVEFORM] - 1.0, 0, 15) + set * 16;
    s->wave2 = (int)clampd(p[DWP_OSC2_WAVEFORM] - 1.0, 0, 15) + set * 16;
    if (s->wave1 >= s->wt->nwaves) s->wave1 -= 16;
    if (s->wave2 >= s->wt->nwaves) s->wave2 -= 16;

    s->osc1_gain  = norm(p[DWP_OSC1_LEVEL], 31.0);
    s->osc2_gain  = norm(p[DWP_OSC2_LEVEL], 31.0);
    s->noise_gain = norm(p[DWP_NOISE_LEVEL], 31.0);

    s->osc2_offset_semi = octave_semis[idx_of(p[DWP_OSC2_OCTAVE], 3)]
                        - octave_semis[idx_of(p[DWP_OSC1_OCTAVE], 3)]
                        + interval_semis[idx_of(p[DWP_OSC2_INTERVAL], 5)];
    /* Detune is 0..6, centred at its default of 2 -- so 2 is "in tune" and the
     * range spans roughly a quartertone either side. APPROX: the cents per
     * step is a guess; only the range is from the binary. */
    s->osc2_detune_cents = (clampd(p[DWP_OSC2_DETUNE], 0, 6) - 2.0) * 8.0;

    /* APPROX: cutoff 0..63 spread exponentially over the audible range. */
    s->cutoff_hz  = dw_tune.cutoff_min
                  * pow(2.0, norm(p[DWP_VCF_CUTOFF], 63.0) * dw_tune.cutoff_octaves);
    s->resonance  = norm(p[DWP_VCF_RESONANCE], 31.0);
    s->kbd_track  = kbd_track[idx_of(p[DWP_VCF_KBD_TRACK], 4)];

    s->vcf_eg_amount = norm(p[DWP_VCF_EG_INTENSITY], 31.0);
    s->vcf_eg_sign   = idx_of(p[DWP_VCF_EG_POLARITY], 2) ? -1.0 : 1.0;
    s->vcf_eg_vel    = norm(p[DWP_VCF_EG_VELOCITY], 7.0);
    s->vca_eg_vel    = norm(p[DWP_VCA_EG_VELOCITY], 7.0);

    /* APPROX: MG rate range. */
    mgf = dw_tune.mg_min * pow(dw_tune.mg_max / dw_tune.mg_min,
                               norm(p[DWP_MG_FREQUENCY], 31.0));
    dw_mg_set(&s->mg, (dw_mg_wave)idx_of(p[DWP_MG_WAVEFORM], 4), mgf,
              norm(p[DWP_MG_DELAY], 31.0) * 5.0);
    s->mg_osc_cents  = norm(p[DWP_MG_OSC], 31.0) * 100.0;
    s->mg_vcf_amount = norm(p[DWP_MG_VCF], 31.0);

    /* Auto bend: a pitch offset at note-on that decays back to zero. */
    {
        int sel = idx_of(p[DWP_AUTOBEND_SELECT], 4);
        s->bend_osc1 = (sel == 1 || sel == 3);
        s->bend_osc2 = (sel == 2 || sel == 3);
        s->bend_dir  = idx_of(p[DWP_AUTOBEND_MODE], 2) ? -1.0 : 1.0;
        s->bend_semis = norm(p[DWP_AUTOBEND_INTENSITY], 31.0) * 12.0;
        s->bend_time  = dw_eg_time(clampd(p[DWP_AUTOBEND_TIME], 0, 31));
    }

    s->porta_time = (p[DWP_PORTAMENTO] > 0.0)
                  ? dw_eg_time(clampd(p[DWP_PORTAMENTO], 0, 31)) : 0.0;
    s->volume     = clampd(p[DWP_VOLUME], 0.0, 1.0);
    /* Tune is bipolar and centred on zero: every factory program stores 0,
     * which must mean "in tune". APPROX: cents per step. */
    s->tune_cents = clampd(p[DWP_TUNE], -50.0, 50.0) * 2.0;
    s->mode       = idx_of(p[DWP_MODE], 4);

    /* Voices is an offset, not a count: the binary registers it as -4..+3 with
     * a default of 0, and every factory program stores 0. */
    s->nvoices = 8 + (int)clampd(p[DWP_VOICES], -4.0, 3.0);
    if (s->nvoices < 1) s->nvoices = 1;
    if (s->nvoices > DW_MAX_VOICES) s->nvoices = DW_MAX_VOICES;
    if (s->mode == DW_MODE_UNISON1 || s->mode == DW_MODE_UNISON2)
        s->nvoices = (s->nvoices < 4) ? 4 : s->nvoices;

    /* Voices at different pitches sum incoherently, so amplitude grows about
     * as sqrt(n). Reserving the full sqrt(nvoices) leaves single notes around
     * 9 dB quieter than they need to be, since most playing is a handful of
     * notes rather than full polyphony -- so reserve most of it and let the
     * master soft-clip absorb the rare dense chord. */
    s->voice_scale = dw_tune.gain * 0.4 / sqrt((double)s->nvoices);

    /* APPROX: the delay is FB-7999's own addition, not a DW-8000 control, and
     * its scaling has not been reverse engineered -- only the ranges below are
     * from the binary (Time 0..7, Factor 0..15, Feedback 0..15, Level 0..15). */
    dtime = dw_tune.delay_min
          + norm(p[DWP_DELAY_TIME], 7.0) * (dw_tune.delay_max - dw_tune.delay_min);
    dtime *= 1.0 + norm(p[DWP_DELAY_FACTOR], 15.0) * 3.0;
    for (i = 0; i < 2; i++) {
        dw_delay *d = i ? &s->delay_r : &s->delay_l;
        double    t = dtime * (i && p[DWP_PSEUDO_STEREO] > 0.0 ? 1.18 : 1.0);
        dw_delay_set(d, t,
                     norm(p[DWP_DELAY_FEEDBACK], 15.0) * 0.85,
                     norm(p[DWP_DELAY_LEVEL], 15.0) * dw_tune.delay_mix,
                     0.05 + norm(p[DWP_DELAY_MOD_FREQ], 31.0) * 6.0,
                     norm(p[DWP_DELAY_MOD_INT], 31.0) * 0.004);
    }
}

static void voice_config_egs(dw_synth *s, dw_voice *v)
{
    const double *p = s->param;
    dw_eg_cfg c;

    c.attack     = dw_eg_time(p[DWP_VCF_EG_ATTACK]);
    c.decay      = dw_eg_time(p[DWP_VCF_EG_DECAY]);
    c.slope      = dw_eg_time(p[DWP_VCF_EG_SLOPE]);
    c.release    = dw_eg_time(p[DWP_VCF_EG_RELEASE]);
    c.breakpoint = clampd(p[DWP_VCF_EG_BREAKPOINT], 0, 31) / 31.0;
    c.sustain    = clampd(p[DWP_VCF_EG_SUSTAIN], 0, 31) / 31.0;
    dw_eg_config(&v->eg_vcf, &c);

    c.attack     = dw_eg_time(p[DWP_VCA_EG_ATTACK]);
    c.decay      = dw_eg_time(p[DWP_VCA_EG_DECAY]);
    c.slope      = dw_eg_time(p[DWP_VCA_EG_SLOPE]);
    c.release    = dw_eg_time(p[DWP_VCA_EG_RELEASE]);
    c.breakpoint = clampd(p[DWP_VCA_EG_BREAKPOINT], 0, 31) / 31.0;
    c.sustain    = clampd(p[DWP_VCA_EG_SUSTAIN], 0, 31) / 31.0;
    dw_eg_config(&v->eg_vca, &c);
}

static void voice_start(dw_synth *s, dw_voice *v, int note, int vel,
                        double detune_cents, double from_pitch)
{
    int was_sounding = v->active;

    v->active   = 1;
    v->held     = 1;
    v->note     = note;
    v->velocity = vel / 127.0;
    v->age      = ++s->counter;
    v->detune_cents = detune_cents;

    v->pitch_target = (double)note - 69.0;
    v->pitch        = (s->porta_time > 0.0) ? from_pitch : v->pitch_target;

    /* Restarting the oscillators from zero phase and wiping the filter is
     * right for a voice that was silent, but on a voice that is still
     * sounding -- a stolen voice, or a unison stack being retriggered -- it
     * steps the waveform mid-cycle and clicks. Over a fast passage those
     * clicks are what turn a melody into a stutter, so keep the state. */
    if (!was_sounding) {
        v->phase1 = 0.0;
        v->phase2 = 0.0;
        memset(v->filt.s, 0, sizeof v->filt.s);
    }

    v->bend_level = s->bend_semis * s->bend_dir;
    v->bend_coef  = exp(-6.9 / (s->bend_time * s->samplerate < 1.0
                                ? 1.0 : s->bend_time * s->samplerate));

    voice_config_egs(s, v);
    dw_eg_gate_on(&v->eg_vcf);
    dw_eg_gate_on(&v->eg_vca);
}

static dw_voice *pick_voice(dw_synth *s)
{
    int i, best = 0;
    unsigned oldest = ~0u;

    for (i = 0; i < s->nvoices; i++)
        if (!s->voice[i].active) return &s->voice[i];
    /* All busy: steal whichever has been sounding longest. */
    for (i = 0; i < s->nvoices; i++)
        if (s->voice[i].age < oldest) { oldest = s->voice[i].age; best = i; }
    return &s->voice[best];
}

void dw_synth_note_on(dw_synth *s, int note, int velocity)
{
    double from = (double)note - 69.0;
    int    i;

    /* Portamento glides from whatever is currently sounding. */
    for (i = 0; i < s->nvoices; i++)
        if (s->voice[i].active) { from = s->voice[i].pitch; break; }

    if (s->mode == DW_MODE_UNISON1 || s->mode == DW_MODE_UNISON2) {
        int n = s->nvoices;
        /* Retrigger the stack in place. Killing it first (dw_synth_all_off)
         * zeroes the envelopes instantly, so every note in a passage started
         * with a hard cut -- which is most of what made this sound stuttery. */
        for (i = 0; i < n; i++) {
            /* Spread the stack symmetrically around the nominal pitch. */
            double d = (n > 1) ? ((double)i / (double)(n - 1) - 0.5) * 14.0 : 0.0;
            voice_start(s, &s->voice[i], note, velocity, d, from);
        }
        dw_mg_retrigger(&s->mg);
        return;
    }

    voice_start(s, pick_voice(s), note, velocity, 0.0, from);
    dw_mg_retrigger(&s->mg);
}

void dw_synth_note_off(dw_synth *s, int note)
{
    int i;
    for (i = 0; i < DW_MAX_VOICES; i++) {
        dw_voice *v = &s->voice[i];
        if (v->active && v->held && v->note == note) {
            v->held = 0;
            dw_eg_gate_off(&v->eg_vcf);
            dw_eg_gate_off(&v->eg_vca);
        }
    }
}

void dw_synth_all_off(dw_synth *s)
{
    int i;
    for (i = 0; i < DW_MAX_VOICES; i++) {
        s->voice[i].active = 0;
        s->voice[i].held   = 0;
        s->voice[i].eg_vcf.stage = DW_EG_IDLE;
        s->voice[i].eg_vca.stage = DW_EG_IDLE;
        s->voice[i].eg_vcf.level = 0.0;
        s->voice[i].eg_vca.level = 0.0;
    }
}

int dw_synth_busy(const dw_synth *s)
{
    int i;
    for (i = 0; i < DW_MAX_VOICES; i++) if (s->voice[i].active) return 1;
    return 0;
}

static double noise(dw_synth *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (double)(int32_t)s->rng * (1.0 / 2147483648.0);
}

void dw_synth_render(dw_synth *s, double *out, int frames)
{
    const double *p = s->param;
    double porta_coef = (s->porta_time > 0.0)
        ? exp(-6.9 / (s->porta_time * s->samplerate)) : 0.0;
    int n, i;

    for (n = 0; n < frames; n++) {
        double mg  = dw_mg_process(&s->mg);
        double mix = 0.0;
        double l, r;

        for (i = 0; i < s->nvoices; i++) {
            dw_voice *v = &s->voice[i];
            double e_vca, e_vcf, base, f1, f2, o, cut, vel_vca, vel_vcf;

            if (!v->active) continue;

            e_vca = dw_eg_process(&v->eg_vca);
            e_vcf = dw_eg_process(&v->eg_vcf);
            if (!dw_eg_active(&v->eg_vca)) { v->active = 0; continue; }

            if (porta_coef > 0.0)
                v->pitch = v->pitch_target + (v->pitch - v->pitch_target) * porta_coef;

            v->bend_level *= v->bend_coef;

            /* Oscillator 1 */
            base = v->pitch
                 + octave_semis[idx_of(p[DWP_OSC1_OCTAVE], 3)]
                 + (s->tune_cents + v->detune_cents + mg * s->mg_osc_cents) / 100.0
                 + (s->bend_osc1 ? v->bend_level : 0.0);
            f1 = note_hz(base);

            /* Oscillator 2 */
            base = v->pitch
                 + octave_semis[idx_of(p[DWP_OSC1_OCTAVE], 3)] + s->osc2_offset_semi
                 + (s->tune_cents + v->detune_cents + s->osc2_detune_cents
                    + mg * s->mg_osc_cents) / 100.0
                 + (s->bend_osc2 ? v->bend_level : 0.0);
            f2 = note_hz(base);

            o = dw_wavetable_read(s->wt, s->wave1, dw_wavetable_mip(s->wt, f1),
                                  v->phase1) * s->osc1_gain
              + dw_wavetable_read(s->wt, s->wave2, dw_wavetable_mip(s->wt, f2),
                                  v->phase2) * s->osc2_gain
              + noise(s) * s->noise_gain;

            v->phase1 += f1 / s->samplerate;
            v->phase2 += f2 / s->samplerate;
            if (v->phase1 >= 1.0) v->phase1 -= floor(v->phase1);
            if (v->phase2 >= 1.0) v->phase2 -= floor(v->phase2);

            /* Filter: base cutoff, keyboard tracking, envelope, MG. */
            vel_vcf = 1.0 - s->vcf_eg_vel * (1.0 - v->velocity);
            cut  = s->cutoff_hz;
            cut *= pow(2.0, s->kbd_track * (v->pitch / 12.0));
            cut *= pow(2.0, s->vcf_eg_sign * s->vcf_eg_amount * e_vcf * vel_vcf
                            * dw_tune.vcf_eg_octaves);
            cut *= pow(2.0, mg * s->mg_vcf_amount);
            dw_filter_set(&v->filt, cut, s->resonance);

            o = dw_filter_process(&v->filt, o);

            vel_vca = 1.0 - s->vca_eg_vel * (1.0 - v->velocity);
            mix += o * e_vca * vel_vca;
        }

        /* Headroom scales with the voice count, so holding a chord is not
         * dramatically louder than one note; what is left over is soft-clipped
         * rather than hard-clipped, which is what the hardware's output stage
         * would do anyway. */
        mix *= s->voice_scale * s->volume;

        l = mix + dw_delay_process(&s->delay_l, mix);
        r = mix + dw_delay_process(&s->delay_r, mix);

        out[2 * n]     = dw_limit(l);
        out[2 * n + 1] = dw_limit(r);
    }
}
