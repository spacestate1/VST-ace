#include "fb02.h"

#include <string.h>

#define PAYLOAD_AT 72

static const char *const GLOBALS[17] = {
    "Algorithm", "Transpose", "Pitch Bend Range", "Portamento", "Feedback",
    "Mode", "PMD Controller", "Output Left", "Output Right", "LFO Enable",
    "LFO Waveform", "LFO Speed", "LFO Sync", "LFO AM Depth",
    "LFO AM Sensitivity", "LFO PM Depth", "LFO PM Sensitivity"
};

static const char *const OPNAMES[17] = {
    "Enable", "Level", "Velocity", "Boost", "Frequency", "Inharmonic",
    "Detune", "Keyb. Scaling Type", "Level Adjust", "Keyb. Scaling Depth",
    "Keyb. Scaling Rate", "Attack", "Attack Velocity", "Decay 1", "Decay 2",
    "Sustain", "Release"
};

const char *fb02_global_name(int i) { return (i >= 0 && i < 17) ? GLOBALS[i] : 0; }
const char *fb02_op_name(int i)     { return (i >= 0 && i < 17) ? OPNAMES[i] : 0; }

int fb02_decode(fb02_program *p, const unsigned char *body, int body_len)
{
    const unsigned char *g;
    int i;

    if (body_len < FB02_BODY) return -1;
    memset(p, 0, sizeof *p);

    g = body + PAYLOAD_AT + 1;   /* skip the 0x55 marker */

    p->algorithm = g[0];
    /* Transpose is signed: 244 means -12, an octave down. */
    p->transpose = g[1] > 127 ? (int)g[1] - 256 : g[1];
    p->pb_range      = g[2];
    p->portamento    = g[3];
    p->feedback      = g[4];
    p->mode          = g[5];
    p->pmd_ctrl      = g[6];
    p->out_l         = g[7];
    p->out_r         = g[8];
    p->lfo_enable    = g[9];
    p->lfo_wave      = g[10];
    p->lfo_speed     = g[11];
    p->lfo_sync      = g[12];
    p->lfo_am_depth  = g[13];
    p->lfo_am_sens   = g[14];
    p->lfo_pm_depth  = g[15];
    p->lfo_pm_sens   = g[16];

    for (i = 0; i < FB02_OPS; i++) {
        const unsigned char *o = body + PAYLOAD_AT + 18 + i * FB02_OPSTRIDE;
        fb02_op *d = &p->op[i];
        d->enable       = o[0];
        d->level        = o[1];
        d->velocity     = o[2];
        d->boost        = o[3];
        d->frequency    = o[4];
        d->inharmonic   = o[5];
        d->detune       = o[6];
        d->ks_type      = o[7];
        d->level_adjust = o[8];
        d->ks_depth     = o[9];
        d->ks_rate      = o[10];
        d->attack       = o[11];
        d->attack_vel   = o[12];
        d->decay1       = o[13];
        d->decay2       = o[14];
        d->sustain      = o[15];
        d->release      = o[16];
        d->wave         = 0;     /* see the header: sine, for every factory program */
    }
    return 0;
}
