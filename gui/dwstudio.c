/* dwstudio -- pick an instrument, pick a patch, play it.  GTK4, plain C.
 *
 * A front-end over the same C engine the command-line tools use. Instruments
 * and their banks are read straight out of the plugin binaries at startup via
 * pe_walk_resources(), so nothing has to be extracted to disk first. Only
 * instruments an engine can voice are listed; --all shows the rest for
 * browsing.
 *
 * Threading. The audio thread owns the engines and never takes a lock: note
 * and program events reach it through a single-producer/single-consumer ring
 * buffer written by the GTK thread. That matters more than it sounds -- the
 * obvious design, a mutex around the render call, makes the UI thread block
 * for the length of an audio block on every keypress and invites priority
 * inversion. Only instrument/bank switching, which is rare and reallocates,
 * parks the audio thread properly.
 */

#include "bank.h"
#include "dw_synth.h"
#include "dw_wavetable.h"
#include "fb02.h"
#include "fm_synth.h"
#include "drumkit.h"
#include "juno.h"
#include "pe.h"
#include "wavedst.h"

#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>

#include "pehost.h"
#include "plugview.h"
#include "../peload/version.h"
#include <spa/param/audio/format-utils.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SR       48000
/* One PipeWire quantum (1024 frames at 48 kHz = 21.3 ms). The audio thread
 * cannot get realtime priority here -- `ulimit -r` is 0 and the user is not in
 * an audio group -- so a 256-frame, 5.3 ms deadline at normal priority gets
 * preempted by the GUI or another application and underruns, which is heard as
 * hiss and crackle. Matching the quantum and asking for a deeper buffer trades
 * latency for not dropping samples. */
#define PERIOD_MAX 4096
/* Block size and buffer depth decide the latency you feel when playing, and
 * both are runtime-settable because the right value depends on the machine.
 * Without realtime scheduling (`ulimit -r` is 0 here) a small block gets
 * preempted and underruns as hiss; a large one is solid but laggy. Tune with
 * DW_PERIOD (frames) and DW_LATENCY (ms). Joining the audio group and getting
 * rtprio is the real fix -- then small blocks stop underrunning. */
static int g_period     = 256;      /* 5.3 ms -- safe on PipeWire's RT thread */
static int g_latency_us = 50000;    /* 50 ms   */
#define MAX_INST    64
#define MAX_BANKS   16
#define EVQ        512          /* power of two */

/* ------------------------------------------------------------- instruments */

typedef enum { ENG_NONE = 0, ENG_DW, ENG_FM, ENG_JUNO, ENG_DRUM } eng_kind;

typedef struct {
    char           type[32];
    unsigned char *raw;
    size_t         len;
    char           path[512];   /* drum kits only: the directory to load */
} bank_res;

typedef struct {
    char           name[64];
    char           path[512];
    bank_res       banks[MAX_BANKS];
    int            nbanks;
    unsigned char *wavedst;
    uint32_t       wavedst_len;
    eng_kind       eng;
} instrument;

static instrument g_inst[MAX_INST];
static int        g_ninst;

/* The Juno-6 has no plugin and no stored patches -- it is a synthetic entry
 * whose single "bank" is the hand-written set in juno.c. */
/* Any directory containing WAVs becomes a kit. Scanned from the sample
 * collections rather than a fixed list, so dropping a new folder in is enough. */
static void add_drumkits(void)
{
    static const char *roots[] = {
        "/storage01/synth_stuff/drums", "/storage01/synth_stuff/furnace", NULL };
    instrument *in;
    int r;

    if (g_ninst >= MAX_INST) return;
    in = &g_inst[g_ninst];
    memset(in, 0, sizeof *in);
    snprintf(in->name, sizeof in->name, "Drum Kits");
    in->eng = ENG_DRUM;

    for (r = 0; roots[r] && in->nbanks < MAX_BANKS; r++) {
        GDir *d = g_dir_open(roots[r], 0, NULL);
        const char *nm;
        if (!d) continue;
        /* the root itself may hold WAVs */
        {
            GDir *t = g_dir_open(roots[r], 0, NULL);
            const char *f; int has = 0;
            while (t && (f = g_dir_read_name(t)))
                if (g_str_has_suffix(f, ".wav") || g_str_has_suffix(f, ".WAV")) { has = 1; break; }
            if (t) g_dir_close(t);
            if (has && in->nbanks < MAX_BANKS) {
                snprintf(in->banks[in->nbanks].path, 512, "%s", roots[r]);
                snprintf(in->banks[in->nbanks].type, 32, "%s", strrchr(roots[r], '/') + 1);
                in->nbanks++;
            }
        }
        while ((nm = g_dir_read_name(d)) && in->nbanks < MAX_BANKS) {
            char sub[512];
            GDir *t;
            const char *f;
            int has = 0;
            snprintf(sub, sizeof sub, "%s/%s", roots[r], nm);
            if (!(t = g_dir_open(sub, 0, NULL))) continue;
            while ((f = g_dir_read_name(t)))
                if (g_str_has_suffix(f, ".wav") || g_str_has_suffix(f, ".WAV")) { has = 1; break; }
            g_dir_close(t);
            if (!has) {           /* one level deeper: kits are often nested */
                GDir *t2 = g_dir_open(sub, 0, NULL);
                const char *n2;
                while (t2 && (n2 = g_dir_read_name(t2)) && in->nbanks < MAX_BANKS) {
                    char s2[512]; GDir *t3; const char *f3; int h2 = 0;
                    snprintf(s2, sizeof s2, "%s/%s", sub, n2);
                    if (!(t3 = g_dir_open(s2, 0, NULL))) continue;
                    while ((f3 = g_dir_read_name(t3)))
                        if (g_str_has_suffix(f3, ".wav") || g_str_has_suffix(f3, ".WAV")) { h2 = 1; break; }
                    g_dir_close(t3);
                    if (h2) {
                        snprintf(in->banks[in->nbanks].path, 512, "%s", s2);
                        snprintf(in->banks[in->nbanks].type, 32, "%.31s", n2);
                        in->nbanks++;
                    }
                }
                if (t2) g_dir_close(t2);
                continue;
            }
            snprintf(in->banks[in->nbanks].path, 512, "%s", sub);
            snprintf(in->banks[in->nbanks].type, 32, "%.31s", nm);
            in->nbanks++;
        }
        g_dir_close(d);
    }
    if (in->nbanks) g_ninst++;
}

static void add_juno(void)
{
    instrument *in;
    if (g_ninst >= MAX_INST) return;
    in = &g_inst[g_ninst++];
    memset(in, 0, sizeof *in);
    snprintf(in->name, sizeof in->name, "Juno-6");
    snprintf(in->banks[0].type, sizeof in->banks[0].type, "Built-in");
    in->nbanks = 1;
    in->eng = ENG_JUNO;
}
static gboolean   g_show_all;
static int        g_cycle_ms;   /* --cycle: walk every plug-in editor unattended */

/* Which MIDI channel this instance answers to: -1 is omni, 0-15 a single
 * channel. A tracker sends every channel down one port, so an omni engine
 * plays them all with whatever patch is selected -- one track per instance
 * needs this filter. */
static _Atomic int g_midi_ch = -1;

static int grab_cb(const char *type, const char *name, int type_id,
                   const unsigned char *data, uint32_t size, void *ud)
{
    instrument *in = ud;
    (void)type_id;
    if ((!strcmp(name, "PROGINIT") || !strcmp(name, "PROGDATA")) && in->nbanks < MAX_BANKS) {
        bank_res *b = &in->banks[in->nbanks];
        if (!(b->raw = malloc(size))) return 0;
        memcpy(b->raw, data, size);
        b->len = size;
        snprintf(b->type, sizeof b->type, "%s", type);
        in->nbanks++;
    } else if (!strcmp(type, "DSTDATA") && !strcmp(name, "WAVEDST")) {
        if ((in->wavedst = malloc(size))) {
            memcpy(in->wavedst, data, size);
            in->wavedst_len = size;
        }
    }
    return 0;
}

static int load_instrument(const char *path, instrument *in)
{
    pe_image img;
    const char *slash;
    int i;

    memset(in, 0, sizeof *in);
    if (pe_open(&img, path)) return 0;
    snprintf(in->path, sizeof in->path, "%s", path);
    slash = strrchr(path, '/');
    snprintf(in->name, sizeof in->name, "%s", slash ? slash + 1 : path);
    { char *d = strstr(in->name, ".dll"); if (d) *d = '\0'; }

    pe_walk_resources(&img, grab_cb, in);
    pe_close(&img);

    for (i = 0; i < in->nbanks && in->eng == ENG_NONE; i++) {
        bank bk;
        if (bank_parse(&bk, in->banks[i].raw, in->banks[i].len)) continue;
        if (in->wavedst && bk.nparam == DWP_COUNT)                     in->eng = ENG_DW;
        else if (bk.fx_id == 0x66623032u && bk.body_bytes == FB02_BODY) in->eng = ENG_FM;
        bank_free(&bk);
    }
    return in->nbanks > 0;
}

/* ------------------------------------------------------------- event queue */

enum { EV_ON = 1, EV_OFF, EV_ALLOFF, EV_PROG, EV_PARAM };

typedef struct { unsigned char type, a, b; float v; } ev_t;

static ev_t             g_evq[EVQ];
static _Atomic unsigned g_ev_head, g_ev_tail;      /* head: producer, tail: consumer */

static void ev_push_v(unsigned char type, unsigned char a, unsigned char b, float v)
{
    unsigned h = atomic_load_explicit(&g_ev_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&g_ev_tail, memory_order_acquire);
    if (((h + 1) & (EVQ - 1)) == (t & (EVQ - 1))) return;   /* full: drop */
    g_evq[h & (EVQ - 1)] = (ev_t){ type, a, b, v };
    atomic_store_explicit(&g_ev_head, h + 1, memory_order_release);
}

static void ev_push(unsigned char type, unsigned char a, unsigned char b)
{
    ev_push_v(type, a, b, 0.0f);
}

/* ------------------------------------------------------------------ engine */

typedef struct {
    eng_kind      kind;
    wavedst       wd;
    dw_wavetable  wt;
    dw_synth      dw;
    fm_synth      fm;
    juno_synth   *ju;
    drumkit      *dk;
    int           ready;

    /* Programs are decoded up front so a program change is just an index. */
    double       (*dwparam)[BANK_MAXPARAM];
    fb02_program  *fmprog;
    int            nprog;

    snd_pcm_t     *pcm;
    pthread_t      thread;
    _Atomic int    running;
    _Atomic int    parked;      /* audio thread has stopped touching the engine */
    _Atomic int    park_req;
    double         gain;
} engine;

static unsigned long g_xruns;   /* ALSA underruns, reported on exit */
static engine g_eng;

/* Drain queued events and render one block. Shared by both backends -- the
 * PipeWire path calls this from PipeWire's realtime thread, the ALSA path from
 * our own normal-priority one. */
/* Scratch for the plug-in path: pehost works in interleaved float, the engines
 * here in interleaved double. Sized once, at PERIOD_MAX, so the audio thread
 * never allocates. */
static float g_plug_buf[PERIOD_MAX * 2];

/* ---- audio input ---------------------------------------------------------
 *
 * The Qt window has taken a microphone or a line in since it learned to; this
 * one passed NULL to the plug-in unconditionally, so every effect in the
 * corpus -- and the corpus is mostly effects -- rendered silence and looked
 * broken. A vocoder had nothing to vocode.
 *
 * One ring between the two PipeWire callbacks, which run on different threads:
 * capture writes, playback reads. Single producer, single consumer, power-of-
 * two so the wrap is a mask -- the same shape the note queue below uses, and
 * for the same reason: neither side may take a lock. */
#define CAP_RING  16384                        /* frames, not samples */
static float           g_cap_ring[CAP_RING * 2];
static _Atomic unsigned g_cap_head, g_cap_tail;
static _Atomic int      g_cap_peak_milli;      /* for the level meter */
static _Atomic int      g_cap_gain_milli = 1000;   /* 1.000 */
static float           g_cap_buf[PERIOD_MAX * 2];
static struct pw_stream *g_cap_stream;
/* The PipeWire source to record from; empty means whatever the system calls
 * default. Changed from the Inputs menu while running, which is what
 * capture_open is for. */
static char             g_cap_target[128];
static char             g_cap_lat[32];      /* node latency, as the output uses */
static void            *g_cap_ud;           /* the engine the callback is given */
/* How much has arrived. A device that is connected and silent and one that is
 * not connected there at all look identical until something counts frames. */
static _Atomic unsigned long g_cap_frames;
static int capture_open(const char *target);

static void cap_write(const float *src, int frames)
{
    unsigned h = atomic_load_explicit(&g_cap_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&g_cap_tail, memory_order_acquire);
    int i;
    for (i = 0; i < frames; i++) {
        if (h - t >= CAP_RING) break;          /* full: drop, never block */
        g_cap_ring[(h & (CAP_RING - 1)) * 2]     = src[i * 2];
        g_cap_ring[(h & (CAP_RING - 1)) * 2 + 1] = src[i * 2 + 1];
        h++;
    }
    atomic_store_explicit(&g_cap_head, h, memory_order_release);
}

/* Read `frames`, applying the input gain. Short reads are zero-filled: the
 * plug-in is asked for a whole block whatever the input managed to deliver. */
/* What feeds an effect's input, and the note gates the "keys" choice plays.
 *
 * A synth ignores the input; an effect renders silence without one and reads
 * as broken. The four choices are pestudio's, because the question is the same
 * one and the answer should not depend on which window is open: silence, a
 * sawtooth per held key, white noise, or the microphone. */
enum { SRC_SILENCE = 0, SRC_NOTES = 1, SRC_NOISE = 2, SRC_INPUT = 3 };
static _Atomic int g_src = SRC_SILENCE;
static _Atomic unsigned char g_gate[128];

static int cap_read(float *dst, int frames)
{
    unsigned h = atomic_load_explicit(&g_cap_head, memory_order_acquire);
    unsigned t = atomic_load_explicit(&g_cap_tail, memory_order_relaxed);
    float g = atomic_load_explicit(&g_cap_gain_milli, memory_order_relaxed) / 1000.0f;
    int i, got = 0;
    for (i = 0; i < frames; i++) {
        if (t == h) break;
        {   /* Clamped, because the gain goes to 400%: a plug-in written for
             * line level is entitled to a signal that stays inside it. */
            float l = g_cap_ring[(t & (CAP_RING - 1)) * 2]     * g;
            float r = g_cap_ring[(t & (CAP_RING - 1)) * 2 + 1] * g;
            dst[i * 2]     = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
            dst[i * 2 + 1] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
        }
        t++; got++;
    }
    atomic_store_explicit(&g_cap_tail, t, memory_order_release);
    if (got < frames)
        memset(dst + (size_t)got * 2, 0, (size_t)(frames - got) * 2 * sizeof *dst);
    return got;
}

/* The scale is a percentage; the ring applies it in thousandths so the audio
 * thread reads one atomic int rather than a float it might see half of. */
static void on_mic_gain(GtkRange *r, gpointer u)
{
    (void)u;
    atomic_store_explicit(&g_cap_gain_milli,
                          (int)(gtk_range_get_value(r) * 10.0), memory_order_relaxed);
}

static void on_capture_process(void *ud)
{
    struct pw_buffer *b;
    struct spa_buffer *sb;
    const float *src;
    int n, i;
    (void)ud;

    if (!g_cap_stream || !(b = pw_stream_dequeue_buffer(g_cap_stream))) return;
    sb = b->buffer;
    src = sb->datas[0].data;
    if (src && sb->datas[0].chunk) {
        n = (int)(sb->datas[0].chunk->size / (sizeof(float) * 2));
        if (n > PERIOD_MAX) n = PERIOD_MAX;
        if (n > 0) {
            float pk = 0.0f;
            for (i = 0; i < n * 2; i++) {
                float a = src[i] < 0 ? -src[i] : src[i];
                if (a > pk) pk = a;
            }
            { int cur = (int)(pk * 1000.0f);
              if (cur > atomic_load_explicit(&g_cap_peak_milli, memory_order_relaxed))
                  atomic_store_explicit(&g_cap_peak_milli, cur, memory_order_relaxed); }
            cap_write(src, n);
            atomic_fetch_add_explicit(&g_cap_frames, (unsigned long)n,
                                      memory_order_relaxed);
        }
    }
    pw_stream_queue_buffer(g_cap_stream, b);
}

/* One block of input for the plug-in. Audio thread.
 *
 * The saw is what an effect can be judged on: it has harmonics and an envelope,
 * where a sine tells you very little about a compressor. Scaled so a fistful of
 * keys does not clip the plug-in's input before it has had a chance to act.
 * Identical to pestudio's fillInput, down to the 5 ms attack and 120 ms
 * release, so the same plug-in fed the same way sounds the same in both. */
static void fill_input(float *dst, int n)
{
    static float  env[128];
    static double phase[128];
    static unsigned rng = 22222u;
    const double sr = SR;
    int src = atomic_load_explicit(&g_src, memory_order_relaxed);
    int i, note;

    memset(dst, 0, (size_t)n * 2 * sizeof *dst);
    if (src == SRC_INPUT) { cap_read(dst, n); return; }   /* gain applied there */
    if (src == SRC_NOISE) {
        for (i = 0; i < n; i++) {
            float v;
            rng = rng * 1664525u + 1013904223u;
            v = (float)((int32_t)rng >> 8) / 8388608.0f * 0.25f;
            dst[2 * i] = dst[2 * i + 1] = v;
        }
        return;
    }
    if (src != SRC_NOTES) return;
    for (note = 0; note < 128; note++) {
        float target = (float)atomic_load_explicit(&g_gate[note],
                                                   memory_order_relaxed) / 127.0f;
        const float up = 1.0f - expf(-1.0f / (0.005f * (float)sr));
        const float dn = 1.0f - expf(-1.0f / (0.120f * (float)sr));
        double step;
        if (target <= 0.0f && env[note] <= 1e-5f) { env[note] = 0.0f; continue; }
        step = 440.0 * pow(2.0, (note - 69) / 12.0) / sr;
        for (i = 0; i < n; i++) {
            float e = env[note], v;
            e += ((target > 0.0f ? target : 0.0f) - e) * (target > e ? up : dn);
            env[note] = e;
            phase[note] += step;
            if (phase[note] >= 1.0) phase[note] -= 1.0;
            v = (float)(2.0 * phase[note] - 1.0) * e * 0.18f;
            dst[2 * i]     += v;
            dst[2 * i + 1] += v;
        }
    }
}

static void render_block(engine *e, double *buf, int frames)
{
    unsigned h, t;

    /* A loaded plug-in is the instrument. Its own event queue carries the
     * notes -- they were handed to pehost on the GTK thread -- so this path
     * does not drain ours, and the engine below is left idle rather than
     * playing underneath it. */
    if (plugview_active()) {
        int i, n = frames > PERIOD_MAX ? PERIOD_MAX : frames;
        /* Whatever the effect input is set to -- an effect with nothing fed to
         * it renders silence, which is the one setting that hands over NULL
         * rather than a buffer of zeros. */
        const float *in = NULL;
        if (atomic_load_explicit(&g_src, memory_order_relaxed) != SRC_SILENCE) {
            fill_input(g_cap_buf, n);
            in = g_cap_buf;
        }
        if (plugview_render_io(in, g_plug_buf, n)) {
            for (i = 0; i < n * 2; i++) buf[i] = g_plug_buf[i];
            if (n < frames) memset(buf + (size_t)n * 2, 0,
                                   (size_t)(frames - n) * 2 * sizeof *buf);
            /* Our own queue still has to be drained, or a switch back to an
             * engine replays every note that arrived while the plug-in had
             * the keyboard. */
            atomic_store_explicit(&g_ev_tail,
                atomic_load_explicit(&g_ev_head, memory_order_acquire),
                memory_order_relaxed);
            return;
        }
    }

    h = atomic_load_explicit(&g_ev_head, memory_order_acquire);
    t = atomic_load_explicit(&g_ev_tail, memory_order_relaxed);

    for (; t != h; t++) {
        ev_t ev = g_evq[t & (EVQ - 1)];
        switch (ev.type) {
        case EV_ON:
            if (e->kind == ENG_DW) dw_synth_note_on(&e->dw, ev.a, ev.b);
            else if (e->kind == ENG_FM) fm_synth_note_on(&e->fm, ev.a, ev.b);
            else if (e->kind == ENG_JUNO && e->ju) juno_note_on(e->ju, ev.a, ev.b);
            else if (e->kind == ENG_DRUM && e->dk) drumkit_note_on(e->dk, ev.a, ev.b);
            break;
        case EV_OFF:
            if (e->kind == ENG_DW) dw_synth_note_off(&e->dw, ev.a);
            else if (e->kind == ENG_FM) fm_synth_note_off(&e->fm, ev.a);
            else if (e->kind == ENG_JUNO && e->ju) juno_note_off(e->ju, ev.a);
            else if (e->kind == ENG_DRUM && e->dk) drumkit_note_off(e->dk, ev.a);
            break;
        case EV_ALLOFF:
            if (e->kind == ENG_DW) dw_synth_all_off(&e->dw);
            else if (e->kind == ENG_FM) fm_synth_all_off(&e->fm);
            else if (e->kind == ENG_JUNO && e->ju) juno_all_off(e->ju);
            else if (e->kind == ENG_DRUM && e->dk) drumkit_all_off(e->dk);
            break;
        case EV_PROG: {
            int idx = ev.a | (ev.b << 8);
            if (idx < 0 || idx >= e->nprog) break;
            if (e->kind == ENG_DW && e->dwparam)
                dw_synth_set_program(&e->dw, e->dwparam[idx]);
            else if (e->kind == ENG_FM && e->fmprog)
                fm_synth_set_program(&e->fm, &e->fmprog[idx]);
            else if (e->kind == ENG_JUNO && e->ju) {
                const juno_patch *jp = juno_factory(idx);
                if (jp) juno_set_patch(e->ju, jp);
            }
            break;
        }
        case EV_PARAM:
            if (e->kind == ENG_JUNO && e->ju) juno_set_param(e->ju, ev.a, ev.v);
            break;
        default: break;
        }
    }
    atomic_store_explicit(&g_ev_tail, t, memory_order_release);

    if (e->ready && e->kind == ENG_DW)           dw_synth_render(&e->dw, buf, frames);
    else if (e->ready && e->kind == ENG_FM)      fm_synth_render(&e->fm, buf, frames);
    else if (e->ready && e->kind == ENG_JUNO && e->ju) juno_render(e->ju, buf, frames);
    else if (e->ready && e->kind == ENG_DRUM && e->dk) drumkit_render(e->dk, buf, frames);
    else memset(buf, 0, (size_t)frames * 2 * sizeof *buf);
}

/* Whether this thread has had its TEB installed. Per-thread by definition, and
 * both backends set it: MSVC-generated plug-in code reaches TLS and its
 * security cookie through a TEB on %gs, so without one the first plug-in call
 * from an audio thread reads a NULL TLS pointer. */
static __thread int g_teb_ready;

static void *audio_thread(void *ud)
{
    engine *e = ud;
    double *buf = malloc((size_t)PERIOD_MAX * 2 * sizeof *buf);
    short  *pcm = malloc((size_t)PERIOD_MAX * 2 * sizeof *pcm);
    int i;

    if (!buf || !pcm) return NULL;
    if (!g_teb_ready) { pehost_thread_init(); g_teb_ready = 1; }

    while (atomic_load_explicit(&e->running, memory_order_relaxed)) {
        long n;

        if (atomic_load_explicit(&e->park_req, memory_order_acquire)) {
            struct timespec ts = { 0, 2000000 };
            atomic_store_explicit(&e->parked, 1, memory_order_release);
            nanosleep(&ts, NULL);
            continue;
        }
        atomic_store_explicit(&e->parked, 0, memory_order_release);

        render_block(e, buf, g_period);

        for (i = 0; i < g_period * 2; i++) {
            double v = buf[i] * e->gain * 32767.0;
            pcm[i] = (short)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
        }

        n = snd_pcm_writei(e->pcm, pcm, g_period);
        if (n < 0) {
            g_xruns++;
            if (snd_pcm_recover(e->pcm, (int)n, 1) < 0) snd_pcm_prepare(e->pcm);
        }
    }
    free(buf);
    free(pcm);
    return NULL;
}

/* ---- PipeWire backend --------------------------------------------------
 *
 * The reason this exists: PipeWire runs stream callbacks on its own data-loop,
 * which RTKit has already granted realtime priority (RR 20) even though this
 * user has `ulimit -r` 0 and no audio group. Rendering there gets realtime
 * scheduling for free, so a small quantum stops underrunning -- low latency
 * and no hiss, instead of trading one for the other as the ALSA path must. */

static struct pw_thread_loop *g_pw_loop;
static struct pw_stream      *g_pw_stream;
static double                *g_pw_buf;

static void pw_on_process(void *ud)
{
    engine *e = ud;
    struct pw_buffer *b;
    struct spa_buffer *sb;
    float *dst;
    int n, i;

    if (!(b = pw_stream_dequeue_buffer(g_pw_stream))) return;
    sb = b->buffer;
    if (!(dst = sb->datas[0].data)) { pw_stream_queue_buffer(g_pw_stream, b); return; }

    n = (int)(sb->datas[0].maxsize / (sizeof(float) * 2));
    if (b->requested && (int)b->requested < n) n = (int)b->requested;
    if (n > PERIOD_MAX) n = PERIOD_MAX;

    if (!g_teb_ready) { pehost_thread_init(); g_teb_ready = 1; }

    if (atomic_load_explicit(&e->park_req, memory_order_acquire)) {
        atomic_store_explicit(&e->parked, 1, memory_order_release);
        memset(dst, 0, (size_t)n * 2 * sizeof *dst);
    } else {
        atomic_store_explicit(&e->parked, 0, memory_order_release);
        render_block(e, g_pw_buf, n);
        for (i = 0; i < n * 2; i++) {
            double v = g_pw_buf[i] * e->gain;
            dst[i] = (float)(v > 1.0 ? 1.0 : (v < -1.0 ? -1.0 : v));
        }
    }

    sb->datas[0].chunk->offset = 0;
    sb->datas[0].chunk->stride = sizeof(float) * 2;
    sb->datas[0].chunk->size   = (uint32_t)(n * 2 * sizeof(float));
    pw_stream_queue_buffer(g_pw_stream, b);
}

static const struct pw_stream_events g_pw_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = pw_on_process,
};

/* The capture devices the machine has, as PipeWire reports them now.
 *
 * Asked at startup and from Rescan rather than watched: plugging a USB
 * interface in is exactly when the list is wrong, and holding a registry
 * listener open to keep it right would mean a second connection to the graph
 * for the whole session. Same scan pestudio does, and the same node names, so
 * a device picked in one window can be named in the other. */
typedef struct { char node[128], label[160]; } indev;
static indev g_indev[32];
static int   g_nindev;

typedef struct {
    struct pw_main_loop *loop;
    int sync;
} devscan;

static void dev_global(void *data, uint32_t id, uint32_t perm, const char *type,
                       uint32_t ver, const struct spa_dict *props)
{
    const char *cls, *name, *desc;
    (void)data; (void)id; (void)perm; (void)ver;
    if (!type || strcmp(type, PW_TYPE_INTERFACE_Node) || !props) return;
    cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!cls || strcmp(cls, "Audio/Source")) return;
    name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!name || g_nindev >= (int)(sizeof g_indev / sizeof g_indev[0])) return;
    desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    snprintf(g_indev[g_nindev].node, sizeof g_indev[0].node, "%s", name);
    snprintf(g_indev[g_nindev].label, sizeof g_indev[0].label, "%s",
             desc && *desc ? desc : name);
    g_nindev++;
}

static void dev_core_done(void *data, uint32_t id, int seq)
{
    devscan *d = data;
    if (id == PW_ID_CORE && seq == d->sync) pw_main_loop_quit(d->loop);
}

static void scan_input_devices(void)
{
    static const struct pw_registry_events rev = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global  = dev_global,
    };
    static const struct pw_core_events cev = {
        .version = PW_VERSION_CORE_EVENTS,
        .done    = dev_core_done,
    };
    devscan scan;
    struct pw_context  *ctx = NULL;
    struct pw_core     *core = NULL;
    struct pw_registry *reg = NULL;
    struct spa_hook rl, cl;

    spa_zero(rl); spa_zero(cl); spa_zero(scan);
    g_nindev = 0;
    scan.loop = pw_main_loop_new(NULL);
    if (!scan.loop) return;
    ctx = pw_context_new(pw_main_loop_get_loop(scan.loop), NULL, 0);
    if (ctx) core = pw_context_connect(ctx, NULL, 0);
    if (core) {
        reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (reg) {
            pw_registry_add_listener(reg, &rl, &rev, &scan);
            pw_core_add_listener(core, &cl, &cev, &scan);
            /* Ask the server to say when it has finished replaying the globals
             * it already has; without it the loop waits forever for a device
             * that is not going to appear. */
            scan.sync = pw_core_sync(core, PW_ID_CORE, 0);
            pw_main_loop_run(scan.loop);
        }
    }
    if (reg)  pw_proxy_destroy((struct pw_proxy *)reg);
    if (core) pw_core_disconnect(core);
    if (ctx)  pw_context_destroy(ctx);
    pw_main_loop_destroy(scan.loop);
}

/* Point the capture stream at a source, or at the system default when `target`
 * is empty. Called once when the audio starts and again whenever a device is
 * picked from the Inputs menu, which is why the old stream is torn down here
 * rather than at shutdown: PipeWire will not retarget a connected stream.
 *
 * The thread loop is locked around it because the capture callback runs on
 * that loop, and destroying a stream underneath its own process callback is
 * the crash this lock exists to prevent. Locking before the loop is started is
 * harmless -- it is an ordinary mutex until then. */
static int capture_open(const char *target)
{
    static const struct pw_stream_events cev = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = on_capture_process,
    };
    struct pw_properties *cp;
    uint8_t cpod[1024];
    struct spa_pod_builder cb = SPA_POD_BUILDER_INIT(cpod, sizeof cpod);
    const struct spa_pod *cparams[1];
    struct spa_audio_info_raw ci;

    if (!g_pw_loop) return 0;
    if (target != g_cap_target)
        snprintf(g_cap_target, sizeof g_cap_target, "%s", target ? target : "");

    pw_thread_loop_lock(g_pw_loop);
    if (g_cap_stream) { pw_stream_destroy(g_cap_stream); g_cap_stream = NULL; }
    cp = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                           PW_KEY_MEDIA_CATEGORY, "Capture",
                           PW_KEY_MEDIA_ROLE, "Production",
                           PW_KEY_NODE_LATENCY, g_cap_lat[0] ? g_cap_lat : "256/48000",
                           NULL);
    if (g_cap_target[0])
        pw_properties_set(cp, PW_KEY_TARGET_OBJECT, g_cap_target);
    g_cap_stream = pw_stream_new_simple(pw_thread_loop_get_loop(g_pw_loop),
                                        "dwstudio input", cp, &cev, g_cap_ud);
    if (g_cap_stream) {
        spa_zero(ci);
        ci.format = SPA_AUDIO_FORMAT_F32;
        ci.rate = SR;
        ci.channels = 2;
        ci.position[0] = SPA_AUDIO_CHANNEL_FL;
        ci.position[1] = SPA_AUDIO_CHANNEL_FR;
        cparams[0] = spa_format_audio_raw_build(&cb, SPA_PARAM_EnumFormat, &ci);
        if (pw_stream_connect(g_cap_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                              PW_STREAM_FLAG_RT_PROCESS, cparams, 1) < 0) {
            pw_stream_destroy(g_cap_stream);
            g_cap_stream = NULL;
        }
    }
    pw_thread_loop_unlock(g_pw_loop);
    /* A new device starts a new count, so "nothing received yet" means this
     * device and not the one before it. */
    atomic_store_explicit(&g_cap_frames, 0, memory_order_relaxed);
    return g_cap_stream != NULL;
}

/* Tear the stream down. Stopping the loop thread first is what makes the rest
 * safe: once it returns, no further pw_on_process can be in flight, so nothing
 * is rendering out of the engine while we destroy the stream. Also used on the
 * partial-setup paths below -- leaving g_pw_stream set after a failed connect
 * would make engine_park() wait on a callback that never runs. */
static void engine_stop_pipewire(void)
{
    if (g_pw_loop)   pw_thread_loop_stop(g_pw_loop);
    if (g_cap_stream) { pw_stream_destroy(g_cap_stream); g_cap_stream = NULL; }
    if (g_pw_stream) { pw_stream_destroy(g_pw_stream); g_pw_stream = NULL; }
    if (g_pw_loop)   { pw_thread_loop_destroy(g_pw_loop); g_pw_loop = NULL; }
    free(g_pw_buf); g_pw_buf = NULL;
}

static int engine_start_pipewire(engine *e)
{
    const struct spa_pod *params[1];
    uint8_t pod[1024];
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    char lat[64];
    struct spa_audio_info_raw info;

    if (!(g_pw_buf = malloc((size_t)PERIOD_MAX * 2 * sizeof *g_pw_buf))) return -1;
    if (!(g_pw_loop = pw_thread_loop_new("dwstudio", NULL))) { engine_stop_pipewire(); return -1; }

    snprintf(lat, sizeof lat, "%d/%d", g_period, SR);
    g_pw_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_pw_loop), "dwstudio",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Music",
                          PW_KEY_NODE_LATENCY, lat,
                          NULL),
        &g_pw_events, e);
    if (!g_pw_stream) { engine_stop_pipewire(); return -1; }

    spa_zero(info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = SR;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    params[0] = spa_format_audio_raw_build(&bb, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(g_pw_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
        engine_stop_pipewire();
        return -1;
    }

    /* The input, opened beside the output. Optional in every sense: a machine
     * with no microphone, or a user who does not want one, gets a host that
     * behaves exactly as it did before -- the plug-in is simply fed nothing. */
    snprintf(g_cap_lat, sizeof g_cap_lat, "%s", lat);
    g_cap_ud = e;
    capture_open(g_cap_target);

    if (pw_thread_loop_start(g_pw_loop) < 0) { engine_stop_pipewire(); return -1; }
    return 0;
}

/* Stop the audio callback touching engine state so the GTK thread can free and
 * rebuild it. Both backends acknowledge through `parked`; which one is live
 * decides whether there is anyone to wait for. Testing e->pcm alone used to
 * skip the wait entirely under PipeWire, which freed the engine out from under
 * a rendering realtime thread. */
static void engine_park(engine *e)
{
    int spins;

    if (!e->pcm && !g_pw_stream) return;    /* no audio running: nothing to park */

    /* Clear the acknowledgement before asking for it, or a stale `parked` left
     * over from the previous park is mistaken for this one. select_instrument()
     * parks, unparks, then parks again well inside a single quantum, so the
     * callback need not have run in between to reset it. */
    atomic_store_explicit(&e->parked, 0, memory_order_relaxed);
    atomic_store_explicit(&e->park_req, 1, memory_order_release);

    /* Bounded wait: a suspended PipeWire node or a wedged device never calls
     * back, and spinning forever here would freeze the UI. A quantum is 5.3 ms
     * at the default period, so half a second means it simply is not running. */
    for (spins = 0; spins < 1000; spins++) {
        if (atomic_load_explicit(&e->parked, memory_order_acquire)) return;
        { struct timespec ts = { 0, 500000 }; nanosleep(&ts, NULL); }
    }
    fprintf(stderr, "audio: park timed out -- callback not running?\n");
}

static void engine_unpark(engine *e)
{
    atomic_store_explicit(&e->park_req, 0, memory_order_release);
}

/* plugview loads and closes plug-ins on the GTK thread and needs the audio
 * callback stopped while it does; it has no engine pointer, so it gets these. */
static void plug_park(void)   { engine_park(&g_eng); }
static void plug_unpark(void) { engine_unpark(&g_eng); }

static void engine_release(engine *e)
{
    if (e->kind == ENG_DW && e->ready) {
        dw_synth_free(&e->dw);
        dw_wavetable_free(&e->wt);
        wavedst_free(&e->wd);
    } else if (e->kind == ENG_JUNO && e->ju) {
        juno_destroy(e->ju);
        e->ju = NULL;
    } else if (e->kind == ENG_DRUM && e->dk) {
        drumkit_free(e->dk);
        e->dk = NULL;
    }
    free(e->dwparam); e->dwparam = NULL;
    free(e->fmprog);  e->fmprog  = NULL;
    e->nprog = 0;
    e->ready = 0;
    e->kind  = ENG_NONE;
}

static const char *g_backend = "none";
/* "auto" (PipeWire, falling back to ALSA), "pipewire" or "alsa".
 * Set by --backend or DW_BACKEND. */
static const char *g_backend_want = "auto";

static int engine_start_audio(engine *e)
{
    if (e->pcm || g_pw_stream) return 0;

    if (strcmp(g_backend_want, "auto") && strcmp(g_backend_want, "pipewire") &&
        strcmp(g_backend_want, "alsa")) {
        fprintf(stderr, "audio: unknown backend '%s' (want auto, pipewire or alsa)"
                        " -- using auto\n", g_backend_want);
        g_backend_want = "auto";
    }

    /* PipeWire first: its callback runs on an RTKit-granted realtime thread,
     * which is the whole point. Fall back to ALSA if that fails. */
    if (strcmp(g_backend_want, "alsa")) {
        if (!engine_start_pipewire(e)) {
            g_backend = "pipewire (realtime)";
            fprintf(stderr, "audio: pipewire, %d-frame quantum (%.1f ms), realtime\n",
                    g_period, 1000.0 * g_period / SR);
            return 0;
        }
        if (!strcmp(g_backend_want, "pipewire")) {
            fprintf(stderr, "audio: pipewire requested but unavailable\n");
            return -1;
        }
        fprintf(stderr, "audio: pipewire unavailable, falling back to ALSA\n");
    }
    g_backend = "alsa";
    if (snd_pcm_open(&e->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        e->pcm = NULL;
        return -1;
    }
    if (snd_pcm_set_params(e->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           2, SR, 1, (unsigned)g_latency_us) < 0)
        return -1;
    fprintf(stderr, "audio: alsa, %d-frame blocks (%.1f ms), %d ms buffer\n",
            g_period, 1000.0 * g_period / SR, g_latency_us / 1000);
    atomic_store_explicit(&e->running, 1, memory_order_release);
    pthread_create(&e->thread, NULL, audio_thread, e);
    return 0;
}

/* ---------------------------------------------------------------- UI state */

typedef struct {
    GtkWidget    *win, *instdd, *bankdd, *list, *info, *piano, *status, *vol;
    GtkStringList *instmodel, *bankmodel;
    bank          cur;
    int           cur_inst, cur_bank;
    int           held[128];
    char          hint[128];
    char          midi[160];

    snd_seq_t    *seq;
    GtkWidget    *mic_level;       /* what the input is doing, after gain */
    int           seqport;         /* what a sequencer plays into */
    int           seqout;          /* ...and what this can play out of */
    int           thru;            /* echo what arrives straight back out */
    int           autoout;         /* subscribe the out port to hardware too */
    int           clock_seen;      /* a sequencer's clock has arrived */
    int           tempo_echo;      /* the box is following the clock, not typing */
    int           chan_echo;       /* the menu and the drop-down agreeing */
    char          port[64];        /* "128:0 (dwstudio in)", for a tracker */
    char          srcs[512];       /* what is connected, in... */
    char          snks[512];       /* ...and out */
    GtkWidget    *midi_conn;       /* those two, on screen */
    GtkWidget    *topbar, *midibar;/* the rows above the pane, shared by both */
    GtkWidget    *chan_dd, *tempo_sb, *tempo_state, *srcdd;
    GMenu        *midi_menu;       /* Inputs > MIDI input, refilled on scan */
    GMenu        *audio_menu;      /* Inputs > Audio input, the device list */
    GMenu        *audio_state;     /* ...and the line under it saying if it is live */
    char          audio_said[32];  /* what that line says now */

    /* Juno control panel: one widget per parameter, shown in place of the
     * read-only text when the Juno engine is selected. */
    GtkWidget    *panel, *infosw, *panelsw;
    /* The two halves: `mode` switches between them, `plug` is plugview's pane.
     * `outer` holds the switcher, the stack, and everything both halves share. */
    GtkWidget    *mode, *plug, *outer;

    /* The pitch wheel, left of the keys as on a hardware synth. 14-bit MIDI,
     * 8192 at rest. */
    GtkWidget    *wheel, *kbrow;
    int           kb_h;
    int           bend, bend_drag;
    double        bend_grab_y;
    int           bend_grab_v;
    GtkWidget    *ctl[JP_COUNT];
    int           loading;      /* suppress callbacks while repopulating */
} ui_t;

static ui_t U;

/* Walk the chunk for a program's raw body -- FB-02 bodies stay opaque. */
static const unsigned char *raw_body(int row)
{
    const bank_res *b;
    size_t off = 160 + 8;
    int i;
    if (U.cur_inst < 0 || U.cur_bank < 0) return NULL;
    b = &g_inst[U.cur_inst].banks[U.cur_bank];
    for (i = 0; i < row; i++) {
        uint32_t nl;
        memcpy(&nl, b->raw + off, 4);
        off += 4 + nl + (size_t)U.cur.body_bytes;
    }
    {
        uint32_t nl;
        memcpy(&nl, b->raw + off, 4);
        return b->raw + off + 4 + nl;
    }
}

/* The line along the bottom: what is playing, what is connected, and which
 * audio backend got it. The plug-in's own name and size are in the pane's
 * status line, so this one answers the questions the pane cannot -- and since
 * the engines stopped being a page, saying which of them would sound is only
 * worth the room when one of them actually would. */
static void set_status(void)
{
    char msg[512];
    const instrument *in = (U.cur_inst >= 0) ? &g_inst[U.cur_inst] : NULL;

    if (!GTK_IS_LABEL(U.status)) return;
    if (plugview_active() || !in)
        snprintf(msg, sizeof msg, "plug-in host%s%s", U.midi, U.hint);
    else
        snprintf(msg, sizeof msg, "nothing loaded — the %s would sound%s%s",
                 in->eng == ENG_DW ? "DW-8000 engine"
                                   : in->eng == ENG_FM ? "4-op FM engine"
                                   : in->eng == ENG_JUNO ? "Juno-6 engine"
                                   : in->eng == ENG_DRUM ? "drum kit"
                                                       : "browser (no engine)",
                 U.midi, U.hint);
    {   /* append the live audio backend so it is visible, not just on stderr */
        size_t l = strlen(msg);
        snprintf(msg + l, sizeof msg - l, "   [%s]", g_backend);
    }
    gtk_label_set_text(GTK_LABEL(U.status), msg);
}

/* ------------------------------------------------------------------- piano */

/* A key keeps its size; a wider window shows more of the keyboard rather than
 * the same keys stretched flatter. That is what a keyboard is -- an octave is
 * a hand span whatever the room is like -- and stretched keys stop matching the
 * fingers that play them. */
#define KEY_W   24.0        /* white key width, in pixels, fixed */
#define KEY_LO  36          /* C2, the leftmost key */
#define KEY_TOP 127         /* the top of MIDI; a wide window reaches for it */
#define KEY_H   ((int)(KEY_W * 4.5))

/* The highest note that fits across `w` pixels at that key width. Ends on a
 * white key, the way a keyboard does.
 *
 * Once every note MIDI has is on screen there is nothing left to add, so the
 * keys stop and the keyboard is centred in what is left rather than sitting
 * against the left edge with a gap beside it. */
static int is_white(int n);

static int key_hi_for_width(int w)
{
    int whites = (int)((double)w / KEY_W);
    int n, seen = 0, hi = KEY_LO;

    if (whites < 1) whites = 1;
    for (n = KEY_LO; n <= KEY_TOP; n++) {
        if (!is_white(n)) continue;
        seen++;
        hi = n;
        if (seen >= whites) break;
    }
    return hi;
}

static int is_white(int n)
{
    static const int w[12] = { 1,0,1,0,1,1,0,1,0,1,0,1 };
    return w[n % 12];
}

/* Where the leftmost key starts: centred once the whole of MIDI is on screen
 * and there is width to spare, hard left otherwise. */
static double piano_x0(int w, int hi)
{
    int n, whites = 0;
    double used;

    for (n = KEY_LO; n <= hi; n++) if (is_white(n)) whites++;
    used = whites * KEY_W;
    return used < w ? (w - used) / 2.0 : 0.0;
}

/* "C4" for middle C, the same numbering and the same real sharp sign the Qt
 * window uses -- at the size a black key allows, "#" reads as a smudge. */
static const char *note_name(int n, int with_octave, char *buf, size_t bufn)
{
    static const char *nm[12] = { "C", "C\u266f", "D", "D\u266f", "E", "F",
                                  "F\u266f", "G", "G\u266f", "A", "A\u266f", "B" };
    if (with_octave) snprintf(buf, bufn, "%s%d", nm[n % 12], n / 12 - 1);
    else             snprintf(buf, bufn, "%s", nm[n % 12]);
    return buf;
}

/* Centred on the key, sitting on its bottom edge. */
static void piano_label(cairo_t *cr, const char *text, double cx, double bottom,
                        double size)
{
    cairo_text_extents_t te;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, text, &te);
    cairo_move_to(cr, cx - (te.width / 2 + te.x_bearing), bottom);
    cairo_show_text(cr, text);
}

static void piano_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u)
{
    int n, i = 0, hi = key_hi_for_width(w);
    double kw = KEY_W, x0 = piano_x0(w, hi);
    (void)a; (void)u;

    /* Whatever is left over past the last whole key stays background rather
     * than being shared out among the keys. */
    cairo_set_source_rgb(cr, 0.09, 0.09, 0.11);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    for (n = KEY_LO; n <= hi; n++) {
        if (!is_white(n)) continue;
        if (U.held[n]) cairo_set_source_rgb(cr, 0.47, 0.67, 1.0);
        else           cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, x0 + i * kw, 0, kw - 1, h);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        /* Every key named, not just the Cs -- and the octave dropped when the
         * key is too narrow to hold it, because a truncated "C" is still the
         * note where a truncated "C4" is a lie about which one. */
        {
            char buf[16];
            double fs = kw * 0.30;
            cairo_text_extents_t te;
            if (fs < 6) fs = 6;
            if (fs > 10) fs = 10;
            cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, fs);
            note_name(n, 1, buf, sizeof buf);
            cairo_text_extents(cr, buf, &te);
            if (te.width > kw - 5) note_name(n, 0, buf, sizeof buf);
            if (n % 12 == 0) cairo_set_source_rgb(cr, 0.27, 0.27, 0.31);
            else             cairo_set_source_rgb(cr, 0.51, 0.51, 0.55);
            piano_label(cr, buf, x0 + i * kw + (kw - 1) / 2.0, h - 4.0, fs);
        }
        i++;
    }
    i = 0;
    for (n = KEY_LO; n <= hi; n++) {
        if (!is_white(n)) continue;
        if (n + 1 <= hi && !is_white(n + 1)) {
            if (U.held[n + 1]) cairo_set_source_rgb(cr, 0.24, 0.43, 0.82);
            else               cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
            cairo_rectangle(cr, x0 + i * kw + kw * 0.68, 0, kw * 0.62, h * 0.62);
            cairo_fill(cr);
            /* The accidental only: at half a white key wide there is no room
             * for the octave, and the white key beside it already says it. */
            {
                char buf[16];
                double bw = kw * 0.62, fs = bw * 0.46;
                if (fs < 5.5) fs = 5.5;
                if (fs > 9) fs = 9;
                cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
                piano_label(cr, note_name(n + 1, 0, buf, sizeof buf),
                            x0 + i * kw + kw * 0.68 + bw / 2.0, h * 0.62 - 4.0, fs);
            }
        }
        i++;
    }
}

/* ------------------------------------------------------------- pitch wheel */

/* The sprung wheel a synth keyboard puts to the left of its keys, the same one
 * pestudio carries and for the same reasons.
 *
 * Sprung is the whole of it: a bend left off centre detunes everything played
 * afterwards and nothing downstream can tell the user stopped meaning it, so it
 * returns to centre the moment it is released and says so.
 *
 * The value stays in MIDI's 14-bit form rather than being converted to
 * semitones, because how far the wheel reaches is the plug-in's business --
 * bend range is one of its parameters, and converting here would mean guessing
 * it.
 *
 * It drives a loaded plug-in. The engines in this tree have no MIDI bend input
 * -- the DW-8000's "bend" is its per-note auto-bend, and the FM engine fixes
 * every operator's increment at note-on -- so on the Engines page the wheel is
 * insensitive rather than silently doing nothing. */
#define BEND_CENTRE 8192
#define BEND_MAX    16383

static void midi_send(int status, int d1, int d2);

/* A bend made here: the plug-in hears it, the port carries it, the wheel draws
 * it. One that arrived over MIDI takes bend_show instead -- it has already
 * reached the plug-in as a raw message, and putting it back out of the port it
 * came in on is how two devices end up bending each other. */
static void bend_send(void)
{
    if (plugview_active()) plugview_bend(U.bend);
    midi_send(0xE0, U.bend & 0x7f, (U.bend >> 7) & 0x7f);
    gtk_widget_queue_draw(U.wheel);
}

static void bend_show(void) { gtk_widget_queue_draw(U.wheel); }

static void bend_recentre(void)
{
    U.bend_drag = 0;
    U.bend = BEND_CENTRE;
    bend_send();
}

static void wheel_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u)
{
    const double label = 12.0;
    double bx = 6.0, by = 4.0, bw = w - 12.0, bh = h - 4.0 - label;
    double off, spacing = 7.0, roll, y, my, cy;
    cairo_pattern_t *lg;

    (void)a; (void)u;
    cairo_set_source_rgb(cr, 0.09, 0.09, 0.11);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    if (bw < 6.0 || bh < 8.0) return;

    off = (double)(U.bend - BEND_CENTRE) / BEND_CENTRE;      /* -1 .. +1 */

    /* A cylinder seen edge on: dark at the rims, lit across the middle. */
    lg = cairo_pattern_create_linear(bx, 0, bx + bw, 0);
    cairo_pattern_add_color_stop_rgb(lg, 0.00, 0.07, 0.07, 0.09);
    cairo_pattern_add_color_stop_rgb(lg, 0.35, 0.29, 0.29, 0.33);
    cairo_pattern_add_color_stop_rgb(lg, 0.50, 0.38, 0.38, 0.43);
    cairo_pattern_add_color_stop_rgb(lg, 0.65, 0.29, 0.29, 0.33);
    cairo_pattern_add_color_stop_rgb(lg, 1.00, 0.07, 0.07, 0.09);
    cairo_set_source(cr, lg);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_fill(cr);
    cairo_pattern_destroy(lg);

    cairo_save(cr);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_clip(cr);

    /* Ridges roll with the value -- that is what makes the travel legible at a
     * glance, where a bare marker line would read as a slider. Two and a half
     * ridges of roll, not three: a whole number puts full deflection back in
     * phase with centre, and the wheel would look untouched exactly where it is
     * furthest from rest. */
    roll = -off * spacing * 2.5;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_set_line_width(cr, 1.0);
    for (y = fmod(roll, spacing) - spacing; y < bh + spacing; y += spacing) {
        double yy = by + y;
        if (yy < by || yy > by + bh) continue;
        cairo_move_to(cr, bx, yy + 0.5);
        cairo_line_to(cr, bx + bw, yy + 0.5);
        cairo_stroke(cr);
    }

    /* The grip, in the colour a held key uses once it is off centre. */
    my = by + bh / 2.0 - off * (bh / 2.0 - 4.0);
    if (U.bend == BEND_CENTRE) cairo_set_source_rgb(cr, 0.59, 0.59, 0.63);
    else                       cairo_set_source_rgb(cr, 0.47, 0.67, 1.0);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, bx + 1, my);
    cairo_line_to(cr, bx + bw - 1, my);
    cairo_stroke(cr);
    cairo_restore(cr);

    /* Detent marks on the frame: where centre is, whatever the wheel says. */
    cy = by + bh / 2.0;
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.39);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 1, cy + 0.5);       cairo_line_to(cr, 5, cy + 0.5);
    cairo_move_to(cr, w - 5, cy + 0.5);   cairo_line_to(cr, w - 1, cy + 0.5);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.51, 0.51, 0.55);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 8.0);
    {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, "PITCH", &ext);
        cairo_move_to(cr, (w - ext.width) / 2.0 - ext.x_bearing, h - 3.0);
        cairo_show_text(cr, "PITCH");
    }
}

/* A drag gesture, not a click plus a motion controller: a click gesture claims
 * the event sequence, so the motion controller never saw the pointer move and
 * the wheel stayed at centre however far it was dragged. */
static void on_wheel_drag_begin(GtkGestureDrag *g, double x, double y, gpointer u)
{ (void)g; (void)x; (void)y; (void)u; U.bend_drag = 1; U.bend_grab_v = U.bend; }

static void on_wheel_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer u)
{
    double travel;
    int nv;

    (void)g; (void)ox; (void)u;
    if (!U.bend_drag) return;
    /* Half the height reaches full bend either way, so the travel on screen is
     * the travel of the thing being imitated. Relative to where the wheel was
     * taken hold of, not absolute to the cursor: an absolute mapping snaps to
     * full bend when it is grabbed near an end, and the way to a small bend
     * should not be a large one. */
    travel = gtk_widget_get_height(U.wheel) / 2.0 - 6.0;
    if (travel < 8.0) travel = 8.0;
    nv = U.bend_grab_v + (int)(-oy / travel * BEND_CENTRE);   /* up is sharp */
    if (nv < 0) nv = 0;
    if (nv > BEND_MAX) nv = BEND_MAX;
    if (nv == U.bend) return;                                 /* no repeats */
    U.bend = nv;
    bend_send();
}

static void on_wheel_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer u)
{ (void)g; (void)ox; (void)oy; (void)u; bend_recentre(); }

static int note_at(double x, double y, int w, int h)
{
    int n, i = 0, hi = key_hi_for_width(w);
    double kw = KEY_W, x0 = piano_x0(w, hi);

    x -= x0;
    if (x < 0) return -1;

    for (n = KEY_LO; n <= hi; n++) {               /* black keys sit on top */
        if (!is_white(n)) continue;
        if (n + 1 <= hi && !is_white(n + 1) && y < h * 0.62) {
            double bx = i * kw + kw * 0.68;
            if (x >= bx && x < bx + kw * 0.62) return n + 1;
        }
        i++;
    }
    i = (int)(x / kw);
    for (n = KEY_LO; n <= hi; n++)
        if (is_white(n) && i-- == 0) return n;
    return -1;
}

/* Out of "dwstudio out", to whatever subscribed to it -- a tracker, a DAW, a
 * hardware synth. Everything played here goes out as well as being heard,
 * which is what makes this a MIDI instrument rather than a window that happens
 * to make a noise.
 *
 * No lock, unlike pestudio's: every caller is on the GTK thread, because the
 * MIDI poll is a GTK timeout rather than pestudio's reader thread. */
static void midi_send(int status, int d1, int d2)
{
    snd_seq_event_t ev;
    int ch = status & 0x0f;

    if (!U.seq || U.seqout < 0) return;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, (unsigned char)U.seqout);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    switch (status & 0xf0) {
    case 0x80: snd_seq_ev_set_noteoff(&ev, ch, d1, d2); break;
    case 0x90:
        if (d2 > 0) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
        else        snd_seq_ev_set_noteoff(&ev, ch, d1, 0);
        break;
    case 0xA0: snd_seq_ev_set_keypress(&ev, ch, d1, d2); break;
    case 0xB0: snd_seq_ev_set_controller(&ev, ch, d1, d2); break;
    case 0xC0: snd_seq_ev_set_pgmchange(&ev, ch, d1); break;
    case 0xD0: snd_seq_ev_set_chanpress(&ev, ch, d1); break;
    /* ALSA's bend is signed around zero; MIDI's is two 7-bit halves biased at
     * 8192, which is the form everything here keeps it in. */
    case 0xE0: snd_seq_ev_set_pitchbend(&ev, ch, ((d2 << 7) | d1) - BEND_CENTRE); break;
    default: return;
    }
    snd_seq_event_output_direct(U.seq, &ev);
}

/* The keyboard plays whatever is loaded. pehost keeps its own lock-free queue
 * for exactly this, so a note goes straight to the plug-in rather than through
 * ours -- and both paths stay single-producer, since dwstudio's MIDI poll is a
 * GTK timeout on this same thread.
 *
 * `local` is where the note came from. A note played here -- on the on-screen
 * keys, or on the computer keyboard -- is sounded, drawn and sent out of the
 * MIDI port. A note that arrived over MIDI has already reached the plug-in as
 * the raw message it came in as, so all it wants is drawing; sounding it again
 * would play it twice, and sending it back out is what `thru` is for. */
static void note_start(int n, int vel, int local)
{
    if (n < 0 || n > 127 || U.held[n]) return;
    U.held[n] = 1;
    atomic_store_explicit(&g_gate[n], (unsigned char)(vel > 127 ? 127 : vel),
                          memory_order_relaxed);
    if (plugview_active()) { if (local) plugview_note_on(n, vel); }
    else                   ev_push(EV_ON, (unsigned char)n, (unsigned char)vel);
    if (local) midi_send(0x90, n, vel);
    gtk_widget_queue_draw(U.piano);
}

static void note_stop(int n, int local)
{
    if (n < 0 || n > 127 || !U.held[n]) return;
    U.held[n] = 0;
    atomic_store_explicit(&g_gate[n], 0, memory_order_relaxed);
    if (plugview_active()) { if (local) plugview_note_off(n); }
    else                   ev_push(EV_OFF, (unsigned char)n, 0);
    if (local) midi_send(0x80, n, 0);
    gtk_widget_queue_draw(U.piano);
}

static void note_on(int n, int vel) { note_start(n, vel, 1); }
static void note_off(int n)         { note_stop(n, 1); }

static void on_press(GtkGestureClick *g, int np, double x, double y, gpointer u)
{
    (void)g; (void)np; (void)u;
    note_on(note_at(x, y, gtk_widget_get_width(U.piano),
                    gtk_widget_get_height(U.piano)), 100);
}

/* Everything down, up. Reached from a mouse release -- the pointer leaves the
 * key it pressed as often as not -- and from the window going inactive, where
 * it matters more: from there no key-up will ever arrive, so a note held while
 * the desktop or a plug-in's own window took focus would sound for good. */
static void release_all(void)
{
    int n;
    for (n = 0; n < 128; n++) note_off(n);
}

static void on_release(GtkGestureClick *g, int np, double x, double y, gpointer u)
{
    (void)g; (void)np; (void)u; (void)x; (void)y;
    release_all();
}

static void on_win_active(GObject *o, GParamSpec *ps, gpointer u)
{
    (void)ps; (void)u;
    if (!gtk_window_is_active(GTK_WINDOW(o))) release_all();
}

/* Tracker layout, same as dwplay's. GTK gives real key-release events, so
 * there is no gate timer here -- holding a key sustains. */
static int key_note(guint kv)
{
    static const char *lo = "zsxdcvgbhnjm";
    static const char *hi = "q2w3er5t6y7u";
    const char *p;
    if (kv < 128) {
        char c = (char)(kv | 32);
        if ((p = strchr(lo, c))) return 48 + (int)(p - lo);
        if ((p = strchr(hi, c))) return 60 + (int)(p - hi);
    }
    return -1;
}

static gboolean on_key(GtkEventControllerKey *c, guint kv, guint kc,
                       GdkModifierType st, gpointer u)
{
    int n = key_note(kv);
    (void)c; (void)kc; (void)u;
    if (n < 0) return FALSE;
    /* A press carrying Ctrl, Alt or Meta is a command, whether or not anything
     * here claims it: Ctrl+C, Ctrl+V, Ctrl+X and Ctrl+B all land on note keys,
     * and playing a note at the copy shortcut is not something to do in front
     * of an audience. Presses only -- a release is delivered whatever is held
     * with it, because reaching for a modifier while a note is down must not be
     * what strands that note on. */
    if (st & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK | GDK_SUPER_MASK))
        return FALSE;
    note_on(n, 100);
    return TRUE;
}

static void on_key_up(GtkEventControllerKey *c, guint kv, guint kc,
                      GdkModifierType st, gpointer u)
{
    int n = key_note(kv);
    (void)c; (void)kc; (void)st; (void)u;
    if (n >= 0) note_off(n);
}

/* --------------------------------------------------------------- selection */

static void show_params(int row);

static void select_bank(int bi);

static void on_patch(GtkListBox *b, GtkListBoxRow *row, gpointer u)
{
    int i;
    (void)b; (void)u;
    if (!row) return;
    i = gtk_list_box_row_get_index(row);
    if (i < 0 || i >= U.cur.count) return;
    ev_push(EV_PROG, (unsigned char)(i & 0xff), (unsigned char)(i >> 8));
    show_params(i);
}

static void decode_all_programs(void)
{
    engine *e = &g_eng;
    int i;

    free(e->dwparam); e->dwparam = NULL;
    free(e->fmprog);  e->fmprog  = NULL;
    e->nprog = 0;

    if (e->kind == ENG_DW && U.cur.nparam == DWP_COUNT) {
        e->dwparam = malloc((size_t)U.cur.count * sizeof *e->dwparam);
        if (!e->dwparam) return;
        for (i = 0; i < U.cur.count; i++)
            memcpy(e->dwparam[i], U.cur.prog[i].param, sizeof e->dwparam[i]);
        e->nprog = U.cur.count;
    } else if (e->kind == ENG_FM) {
        e->fmprog = malloc((size_t)U.cur.count * sizeof *e->fmprog);
        if (!e->fmprog) return;
        for (i = 0; i < U.cur.count; i++) {
            const unsigned char *b = raw_body(i);
            if (!b || fb02_decode(&e->fmprog[i], b, U.cur.body_bytes))
                memset(&e->fmprog[i], 0, sizeof e->fmprog[i]);
        }
        e->nprog = U.cur.count;
    }
}

static void select_instrument(int idx)
{
    instrument *in;
    int i;

    if (idx < 0 || idx >= g_ninst) return;
    U.cur_inst = idx;
    in = &g_inst[idx];

    engine_park(&g_eng);
    engine_release(&g_eng);

    if (in->eng == ENG_DRUM) {
        g_eng.kind = ENG_DRUM;      /* the kit itself loads in select_bank */
        g_eng.ready = 1;
    } else if (in->eng == ENG_JUNO) {
        if ((g_eng.ju = juno_create(SR))) {
            juno_set_patch(g_eng.ju, juno_factory(0));
            g_eng.kind = ENG_JUNO;
            g_eng.ready = 1;
        }
    } else if (in->eng == ENG_DW && in->wavedst) {
        if (!wavedst_load(&g_eng.wd, in->wavedst, in->wavedst_len, 0) &&
            !dw_wavetable_build(&g_eng.wt, &g_eng.wd, SR) &&
            !dw_synth_init(&g_eng.dw, &g_eng.wt, SR)) {
            g_eng.kind = ENG_DW;
            g_eng.ready = 1;
        }
    } else if (in->eng == ENG_FM) {
        fm_synth_init(&g_eng.fm, SR);
        g_eng.kind = ENG_FM;
        g_eng.ready = 1;
    }
    engine_unpark(&g_eng);

    gtk_string_list_splice(U.bankmodel, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(U.bankmodel)), NULL);
    for (i = 0; i < in->nbanks; i++)
        gtk_string_list_append(U.bankmodel, in->banks[i].type);
    if (in->nbanks) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U.bankdd), 0);
        select_bank(0);
    }
}

static void select_bank(int bi)
{
    instrument *in;
    GtkWidget *child;
    int i;

    if (U.cur_inst < 0) return;
    in = &g_inst[U.cur_inst];
    if (bi < 0 || bi >= in->nbanks) return;
    U.cur_bank = bi;

    while ((child = gtk_widget_get_first_child(U.list)))
        gtk_list_box_remove(GTK_LIST_BOX(U.list), child);

    if (U.cur.prog) bank_free(&U.cur);
    memset(&U.cur, 0, sizeof U.cur);

    if (in->eng == ENG_DRUM) {
        engine_park(&g_eng);
        if (g_eng.dk) { drumkit_free(g_eng.dk); g_eng.dk = NULL; }
        g_eng.dk = drumkit_load(in->banks[bi].path, SR);
        engine_unpark(&g_eng);
        if (g_eng.dk) {
            for (i = 0; i < drumkit_count(g_eng.dk); i++) {
                char line[160];
                GtkWidget *lbl;
                snprintf(line, sizeof line, "%3d  %-28s  note %d",
                         i, drumkit_sample_name(g_eng.dk, i), drumkit_note_of(g_eng.dk, i));
                lbl = gtk_label_new(line);
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
                gtk_widget_set_margin_start(lbl, 6);
                gtk_list_box_append(GTK_LIST_BOX(U.list), lbl);
            }
            U.cur.count = drumkit_count(g_eng.dk);
        } else {
            U.cur.count = 0;
        }
        set_status();
        return;
    }

    if (in->eng == ENG_JUNO) {
        for (i = 0; i < juno_factory_count(); i++) {
            char line[128];
            GtkWidget *lbl;
            snprintf(line, sizeof line, "%3d  %s", i, juno_factory(i)->name);
            lbl = gtk_label_new(line);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_widget_set_margin_start(lbl, 6);
            gtk_widget_set_margin_end(lbl, 6);
            gtk_list_box_append(GTK_LIST_BOX(U.list), lbl);
        }
        U.cur.count = juno_factory_count();
        g_eng.nprog = U.cur.count;
        set_status();
        gtk_list_box_select_row(GTK_LIST_BOX(U.list),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(U.list), 0));
        return;
    }

    if (bank_parse(&U.cur, in->banks[bi].raw, in->banks[bi].len)) {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(U.info)),
            "This bank's programs are variable-length; no fixed layout was detected.", -1);
        set_status();
        return;
    }

    for (i = 0; i < U.cur.count; i++) {
        char line[128];
        GtkWidget *lbl;
        snprintf(line, sizeof line, "%3d  %s", i, U.cur.prog[i].name);
        lbl = gtk_label_new(line);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_widget_set_margin_start(lbl, 6);
        gtk_widget_set_margin_end(lbl, 6);
        gtk_list_box_append(GTK_LIST_BOX(U.list), lbl);
    }

    engine_park(&g_eng);
    decode_all_programs();
    engine_unpark(&g_eng);

    set_status();
    if (U.cur.count)
        gtk_list_box_select_row(GTK_LIST_BOX(U.list),
                                gtk_list_box_get_row_at_index(GTK_LIST_BOX(U.list), 0));
}

static void on_inst_changed(GtkDropDown *d, GParamSpec *p, gpointer u)
{
    (void)p; (void)u;
    select_instrument((int)gtk_drop_down_get_selected(d));
}

static void on_bank_changed(GtkDropDown *d, GParamSpec *p, gpointer u)
{
    (void)p; (void)u;
    select_bank((int)gtk_drop_down_get_selected(d));
}

/* What feeds an effect's input. Same four in the same order as pestudio's, so
 * "set it to input" means the same thing in either window. */
static void on_src_changed(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    (void)ps; (void)u;
    atomic_store_explicit(&g_src, (int)gtk_drop_down_get_selected(d),
                          memory_order_relaxed);
}

static void src_select(int src)
{
    atomic_store_explicit(&g_src, src, memory_order_relaxed);
    if (GTK_IS_WIDGET(U.srcdd))
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U.srcdd), (guint)src);
}

static void on_chan_changed(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    int ch = (int)gtk_drop_down_get_selected(d) - 1;   /* 0 = Omni */
    (void)ps; (void)u;
    atomic_store_explicit(&g_midi_ch, ch, memory_order_relaxed);
    ev_push(EV_ALLOFF, 0, 0);
    /* The same setting is a radio item under Inputs, and a menu still showing
     * the old channel is worse than no menu at all. The guard is for the trip
     * back: the action's handler sets this drop-down too. */
    if (!U.chan_echo && U.win) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(U.win), "midi-channel");
        if (a) {
            U.chan_echo = 1;
            g_action_change_state(a, g_variant_new_int32(ch));
            U.chan_echo = 0;
        }
    }
}

/* Typed, not followed: echoing back a tempo that came from the clock would
 * fight the sync, so poll_transport sets the box with U.tempo_echo raised and
 * this does nothing while it is. */
static void on_tempo(GtkSpinButton *sb, gpointer u)
{
    (void)u;
    if (U.tempo_echo) return;
    plugview_set_tempo(gtk_spin_button_get_value(sb));
}

static void on_vol(GtkRange *r, gpointer u)
{
    (void)u;
    g_eng.gain = gtk_range_get_value(r) / 100.0;
}

/* ------------------------------------------------- Juno control panel ---- */

static void on_ctl_scale(GtkRange *r, gpointer id)
{
    if (U.loading) return;
    ev_push_v(EV_PARAM, (unsigned char)GPOINTER_TO_INT(id), 0,
              (float)gtk_range_get_value(r));
}

static void on_ctl_switch(GtkCheckButton *b, gpointer id)
{
    if (U.loading) return;
    ev_push_v(EV_PARAM, (unsigned char)GPOINTER_TO_INT(id), 0,
              gtk_check_button_get_active(b) ? 1.0f : 0.0f);
}

static void on_ctl_drop(GtkDropDown *d, GParamSpec *ps, gpointer id)
{
    (void)ps;
    if (U.loading) return;
    ev_push_v(EV_PARAM, (unsigned char)GPOINTER_TO_INT(id), 0,
              (float)gtk_drop_down_get_selected(d));
}

static const char *const RANGE_LBL[] = { "16'", "8'", "4'", NULL };
static const char *const HPF_LBL[]   = { "0", "1", "2", "3", NULL };
static const char *const CHOR_LBL[]  = { "Off", "I", "II", NULL };

static void build_panel(void)
{
    GtkWidget *grid = gtk_grid_new();
    int id, row = 0;

    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 6);

    for (id = 0; id < JP_COUNT; id++) {
        GtkWidget *lbl = gtk_label_new(juno_param_name(id));
        GtkWidget *w;
        int max = juno_param_max(id);

        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_widget_set_size_request(lbl, 118, -1);

        if (max == 0) {                      /* continuous 0..1 */
            w = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
            gtk_scale_set_draw_value(GTK_SCALE(w), TRUE);
            gtk_scale_set_value_pos(GTK_SCALE(w), GTK_POS_RIGHT);
            gtk_widget_set_hexpand(w, TRUE);
            g_signal_connect(w, "value-changed",
                             G_CALLBACK(on_ctl_scale), GINT_TO_POINTER(id));
        } else if (max == 1) {               /* on/off */
            w = gtk_check_button_new();
            g_signal_connect(w, "toggled",
                             G_CALLBACK(on_ctl_switch), GINT_TO_POINTER(id));
        } else {                             /* a few discrete positions */
            const char *const *lab = (id == JP_RANGE)  ? RANGE_LBL
                                   : (id == JP_HPF)    ? HPF_LBL
                                   : (id == JP_CHORUS) ? CHOR_LBL : HPF_LBL;
            w = gtk_drop_down_new_from_strings(lab);
            g_signal_connect(w, "notify::selected",
                             G_CALLBACK(on_ctl_drop), GINT_TO_POINTER(id));
        }
        U.ctl[id] = w;
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), w,   1, row, 1, 1);
        row++;
    }
    U.panel = grid;
}

/* Push a patch into the widgets without re-emitting change events. */
static void panel_load(const juno_patch *p)
{
    int id;
    U.loading = 1;
    for (id = 0; id < JP_COUNT; id++) {
        double v;
        switch (id) {
        case JP_RANGE: v = p->range; break;      case JP_DCO_LFO: v = p->dco_lfo; break;
        case JP_PWM: v = p->pwm; break;          case JP_PWM_MANUAL: v = p->pwm_manual; break;
        case JP_SAW: v = p->saw; break;          case JP_PULSE: v = p->pulse; break;
        case JP_SUB: v = p->sub; break;          case JP_NOISE: v = p->noise; break;
        case JP_HPF: v = p->hpf; break;          case JP_CUTOFF: v = p->cutoff; break;
        case JP_RES: v = p->res; break;          case JP_VCF_ENV: v = p->vcf_env; break;
        case JP_VCF_ENV_NEG: v = p->vcf_env_neg; break;
        case JP_VCF_LFO: v = p->vcf_lfo; break;  case JP_VCF_KEY: v = p->vcf_key; break;
        case JP_VCA_GATE: v = p->vca_gate; break;case JP_VOLUME: v = p->volume; break;
        case JP_A: v = p->a; break;              case JP_D: v = p->d; break;
        case JP_S: v = p->s; break;              case JP_R: v = p->r; break;
        case JP_LFO_RATE: v = p->lfo_rate; break;case JP_LFO_DELAY: v = p->lfo_delay; break;
        case JP_CHORUS: v = p->chorus; break;    default: v = 0.0; break;
        }
        if (juno_param_max(id) == 0)
            gtk_range_set_value(GTK_RANGE(U.ctl[id]), v);
        else if (juno_param_max(id) == 1)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(U.ctl[id]), v > 0.5);
        else
            gtk_drop_down_set_selected(GTK_DROP_DOWN(U.ctl[id]), (guint)(v + 0.5));
    }
    U.loading = 0;
}

/* ------------------------------------------------------------------- info */

static void show_params(int row)
{
    GString *s = g_string_new(NULL);
    int i;

    if (g_inst[U.cur_inst].eng == ENG_JUNO) {
        const juno_patch *p = juno_factory(row);
        if (p) panel_load(p);
        static const char *RNG[3] = { "16'", "8'", "4'" };
        static const char *CH[3]  = { "off", "I", "II" };
        if (p) {
            g_string_append_printf(s,
                "Juno-6 patch  \"%s\"\n"
                "(written by hand -- the Juno-6 has no patch memory,\n"
                " so there is no factory data to recover)\n\n"
                "DCO   range %s   saw %s   pulse %s\n"
                "      sub %.2f   noise %.2f   PWM %.2f (%s)   LFO %.2f\n\n"
                "HPF   %d\n\n"
                "VCF   cutoff %.2f   resonance %.2f\n"
                "      env %.2f (%s)   LFO %.2f   key follow %.2f\n\n"
                "ENV   A %.2f   D %.2f   S %.2f   R %.2f\n"
                "VCA   %s   volume %.2f\n\n"
                "LFO   rate %.2f   delay %.2f\n"
                "CHORUS %s\n",
                p->name, RNG[p->range % 3], p->saw ? "on" : "off",
                p->pulse ? "on" : "off", p->sub, p->noise, p->pwm,
                p->pwm_manual ? "manual" : "LFO", p->dco_lfo, p->hpf,
                p->cutoff, p->res, p->vcf_env, p->vcf_env_neg ? "neg" : "pos",
                p->vcf_lfo, p->vcf_key, p->a, p->d, p->s, p->r,
                p->vca_gate ? "gate" : "envelope", p->volume,
                p->lfo_rate, p->lfo_delay, CH[p->chorus % 3]);
        }
    } else if (g_inst[U.cur_inst].eng == ENG_FM) {
        const unsigned char *b = raw_body(row);
        fb02_program p;
        g_string_append(s, "FB-02 4-operator FM voice\n\n");
        if (b && !fb02_decode(&p, b, U.cur.body_bytes)) {
            const int gv[17] = { p.algorithm, p.transpose, p.pb_range, p.portamento,
                                 p.feedback, p.mode, p.pmd_ctrl, p.out_l, p.out_r,
                                 p.lfo_enable, p.lfo_wave, p.lfo_speed, p.lfo_sync,
                                 p.lfo_am_depth, p.lfo_am_sens, p.lfo_pm_depth,
                                 p.lfo_pm_sens };
            for (i = 0; i < 17; i++)
                g_string_append_printf(s, "%-22s %d\n", fb02_global_name(i), gv[i]);
            for (i = 0; i < FB02_OPS; i++) {
                const fb02_op *o = &p.op[i];
                const int ov[17] = { o->enable, o->level, o->velocity, o->boost,
                                     o->frequency, o->inharmonic, o->detune, o->ks_type,
                                     o->level_adjust, o->ks_depth, o->ks_rate, o->attack,
                                     o->attack_vel, o->decay1, o->decay2, o->sustain,
                                     o->release };
                int k;
                g_string_append_printf(s, "\nOP%d\n", i + 1);
                for (k = 0; k < 17; k++)
                    g_string_append_printf(s, "  %-20s %d\n", fb02_op_name(k), ov[k]);
            }
        }
    } else if (U.cur.nparam == DWP_COUNT) {
        g_string_append(s, "DW-8000 program\n\n");
        for (i = 0; i < U.cur.nparam; i++) {
            const char *n = bank_param_name(i);
            if (!n || !strcmp(n, "reserved")) continue;
            g_string_append_printf(s, "%-22s %g\n", n, U.cur.prog[row].param[i]);
        }
    } else {
        g_string_append_printf(s,
            "Body is %d bytes and does not decode as parameters.\n"
            "Patch names are readable; the layout is plugin-specific.", U.cur.body_bytes);
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(U.info)), s->str, -1);
    g_string_free(s, TRUE);

    {
        int juno = (g_inst[U.cur_inst].eng == ENG_JUNO);
        gtk_widget_set_visible(U.panelsw, juno);
        gtk_widget_set_visible(U.infosw, !juno);
    }
}

/* -------------------------------------------------------------------- MIDI */

static gboolean poll_mic_level(gpointer u)
{
    int pk;
    (void)u;
    if (!GTK_IS_WIDGET(U.mic_level)) return G_SOURCE_CONTINUE;
    pk = atomic_exchange_explicit(&g_cap_peak_milli, 0, memory_order_relaxed);
    {   /* Fall back gently rather than snapping to zero between transients. */
        static double shown;
        double v = pk / 1000.0;
        if (v > shown) shown = v; else shown = shown * 0.7;
        if (shown > 1.0) shown = 1.0;
        gtk_level_bar_set_value(GTK_LEVEL_BAR(U.mic_level), shown);
    }
    return G_SOURCE_CONTINUE;
}

/* What is connected, in and out, said in the window rather than only in the
 * status line. "no MIDI in" and a list of one hardware port look nothing alike
 * when a tracker will not play, and which of the two it is decides where to
 * look next. */
static void midi_conn_update(void)
{
    char txt[1100];

    /* The port this window is, before what is plugged into it: connecting a
     * tracker means picking this client out of its list, and the name it is
     * listed under is the one thing the window alone can say. */
    snprintf(txt, sizeof txt, "%s   in: %s   out: %s",
             U.port[0] ? U.port : "no MIDI",
             U.srcs[0] ? U.srcs : "nothing connected",
             U.snks[0] ? U.snks : (U.autoout ? "nothing connected"
                                             : "on request only"));
    if (GTK_IS_LABEL(U.midi_conn)) {
        char *line = g_strdup(txt), *nl;
        /* One line in the row, the whole list on hover: the row is next to the
         * tempo and cannot grow, and a port list is exactly the thing you want
         * in full when something is missing from it. */
        for (nl = line; (nl = strchr(nl, '\n')) != NULL; ) *nl = ' ';
        gtk_label_set_text(GTK_LABEL(U.midi_conn), line);
        gtk_widget_set_tooltip_text(U.midi_conn, txt);
        g_free(line);
    }
    if (U.midi_menu) {
        g_menu_remove_all(U.midi_menu);
        if (!U.srcs[0]) {
            /* An item with no action: a list, not a chooser. Everything that
             * can send is subscribed already, so there is nothing to pick. */
            g_menu_append(U.midi_menu, "nothing connected", NULL);
        } else {
            char *copy = g_strdup(U.srcs), *tok, *save = NULL;
            for (tok = strtok_r(copy, "\n", &save); tok;
                 tok = strtok_r(NULL, "\n", &save))
                g_menu_append(U.midi_menu, tok, NULL);
            g_free(copy);
        }
    }
}

/* Subscribe to everything that can send, and -- when asked -- to everything
 * that can receive. Returns how many connections were new, so a keyboard
 * plugged in after startup can be picked up without a restart and the window
 * can say whether that did anything.
 *
 * Another dwstudio is skipped on purpose. Subscribing to everything that sends
 * is right for keyboards and trackers, but two of these are both senders and
 * both receivers: the second to scan grabs the first's output, the first grabs
 * it back, and with thru on the pair is a closed loop. Chaining two of them is
 * a reasonable thing to want and a terrible thing to do by accident -- aconnect
 * still does it on purpose. */
static int midi_rescan(void)
{
    const unsigned need_read  = SND_SEQ_PORT_CAP_READ  | SND_SEQ_PORT_CAP_SUBS_READ;
    const unsigned need_write = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    snd_seq_client_info_t *ci = NULL;
    snd_seq_port_info_t   *pi = NULL;
    int added = 0;

    if (!U.seq) return 0;
    if (snd_seq_client_info_malloc(&ci) < 0) return 0;
    if (snd_seq_port_info_malloc(&pi) < 0) { snd_seq_client_info_free(ci); return 0; }

    U.srcs[0] = U.snks[0] = '\0';
    U.midi[0] = '\0';
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(U.seq, ci) >= 0) {
        int cl = snd_seq_client_info_get_client(ci);
        const char *cname = snd_seq_client_info_get_name(ci);
        if (cl == SND_SEQ_CLIENT_SYSTEM || cl == snd_seq_client_id(U.seq)) continue;
        if (cname && !strncmp(cname, "dwstudio", 8)) continue;
        snd_seq_port_info_set_client(pi, cl);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(U.seq, pi) >= 0) {
            unsigned cap  = snd_seq_port_info_get_capability(pi);
            unsigned type = snd_seq_port_info_get_type(pi);
            int      pn   = snd_seq_port_info_get_port(pi);
            const char *pname = snd_seq_port_info_get_name(pi);
            if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
            if ((cap & need_read) == need_read) {
                size_t l;
                /* Already subscribed answers EBUSY, which is still connected --
                 * it just is not new. */
                if (snd_seq_connect_from(U.seq, U.seqport, cl, pn) == 0) added++;
                l = strlen(U.srcs);
                snprintf(U.srcs + l, sizeof U.srcs - l, "%s%s:%s",
                         l ? "\n" : "", cname ? cname : "?", pname ? pname : "?");
                l = strlen(U.midi);
                snprintf(U.midi + l, sizeof U.midi - l, "%s%s",
                         l ? ", " : "  MIDI: ", cname ? cname : "?");
            }
            if (U.autoout && (cap & need_write) == need_write) {
                size_t l;
                if (snd_seq_connect_to(U.seq, U.seqout, cl, pn) == 0) added++;
                l = strlen(U.snks);
                snprintf(U.snks + l, sizeof U.snks - l, "%s%s:%s",
                         l ? "\n" : "", cname ? cname : "?", pname ? pname : "?");
            }
        }
    }
    snd_seq_port_info_free(pi);
    snd_seq_client_info_free(ci);
    if (!U.midi[0]) snprintf(U.midi, sizeof U.midi, "  (no MIDI in)");
    midi_conn_update();
    return added;
}

static gboolean poll_midi(gpointer u)
{
    snd_seq_event_t *ev;
    (void)u;
    if (!U.seq) return G_SOURCE_REMOVE;
    while (snd_seq_event_input(U.seq, &ev) >= 0) {
        int status = -1, d1 = 0, d2 = 0, ch, voice;

        /* Every message is turned back into the three bytes it was on the
         * wire. That is what the plug-in wants -- a wheel, a pedal, aftertouch
         * and a sequencer's clock are all raw MIDI and nothing else, and a poll
         * that only understood notes dropped the lot -- and it is what the out
         * port needs for thru. */
        voice = ev->type == SND_SEQ_EVENT_NOTEON  || ev->type == SND_SEQ_EVENT_NOTEOFF ||
                ev->type == SND_SEQ_EVENT_CONTROLLER || ev->type == SND_SEQ_EVENT_PGMCHANGE ||
                ev->type == SND_SEQ_EVENT_PITCHBEND  || ev->type == SND_SEQ_EVENT_CHANPRESS ||
                ev->type == SND_SEQ_EVENT_KEYPRESS;
        {   /* channel filter, applied to the voice messages only, so clock
             * still arrives when a single channel is picked out */
            int want = atomic_load_explicit(&g_midi_ch, memory_order_relaxed);
            if (want >= 0 && voice && ev->data.note.channel != want) continue;
        }
        ch = voice ? ev->data.note.channel : 0;

        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            status = 0x90 | ch; d1 = ev->data.note.note; d2 = ev->data.note.velocity;
            if (d2 > 0) note_start(d1, d2, 0); else note_stop(d1, 0);
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            status = 0x80 | ch; d1 = ev->data.note.note;
            note_stop(d1, 0);
            break;
        case SND_SEQ_EVENT_PITCHBEND: {
            /* ALSA hands it back signed around zero; MIDI's own form is
             * unsigned around 8192, which is what the plug-in wants and what
             * the wheel draws. */
            int v = ev->data.control.value + BEND_CENTRE;
            if (v < 0) v = 0;
            if (v > BEND_MAX) v = BEND_MAX;
            U.bend = v;
            status = 0xE0 | ch; d1 = v & 0x7f; d2 = (v >> 7) & 0x7f;
            bend_show();
            break;
        }
        case SND_SEQ_EVENT_PGMCHANGE:
            status = 0xC0 | ch; d1 = ev->data.control.value & 0x7f;
            if (!plugview_active() && U.cur.count) {
                int i = d1 % U.cur.count;
                gtk_list_box_select_row(GTK_LIST_BOX(U.list),
                    gtk_list_box_get_row_at_index(GTK_LIST_BOX(U.list), i));
            }
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            status = 0xB0 | ch;
            d1 = ev->data.control.param & 0x7f;
            d2 = ev->data.control.value & 0x7f;
            if (d1 == 123) { int n; for (n = 0; n < 128; n++) note_stop(n, 0);
                             if (!plugview_active()) ev_push(EV_ALLOFF, 0, 0); }
            break;
        case SND_SEQ_EVENT_CHANPRESS:
            status = 0xD0 | ch; d1 = ev->data.control.value & 0x7f;
            break;
        case SND_SEQ_EVENT_KEYPRESS:
            status = 0xA0 | ch; d1 = ev->data.note.note; d2 = ev->data.note.velocity;
            break;
        /* System realtime. No channel, which is why the filter above is for
         * voice messages only, and this is how a sequencer says what its tempo
         * is and whether the song is rolling -- without them every tempo-synced
         * arpeggiator and delay runs at the host's default instead. */
        case SND_SEQ_EVENT_SONGPOS: {
            int v = ev->data.control.value & 0x3FFF;    /* sixteenths */
            status = 0xF2; d1 = v & 0x7f; d2 = (v >> 7) & 0x7f;
            break;
        }
        case SND_SEQ_EVENT_CLOCK:    status = 0xF8; U.clock_seen = 1; break;
        case SND_SEQ_EVENT_START:    status = 0xFA; break;
        case SND_SEQ_EVENT_CONTINUE: status = 0xFB; break;
        case SND_SEQ_EVENT_STOP:     status = 0xFC; break;
        default: break;
        }

        if (status < 0) continue;
        plugview_midi(status, d1, d2);
        /* Realtime is deliberately not echoed: thru exists to pass playing
         * through to other gear, and re-sending a clock we were given is how
         * two devices end up driving each other. */
        if (U.thru && status < 0xF0) midi_send(status, d1, d2);
    }
    return G_SOURCE_CONTINUE;
}

/* Defined with the rest of the Inputs menu, below; the transport poll is the
 * one timer that runs often enough to notice a plug-in loading and an input
 * going quiet. */
static void apply_input_mask(void);
static void audio_state_update(void);

/* Follow the transport rather than assume it. Once a sequencer's clock is
 * driving the tempo the box shows what it is doing instead of what somebody
 * typed earlier -- two numbers disagreeing about the tempo is worse than one
 * that is merely read-only. */
static gboolean poll_transport(gpointer u)
{
    static int was_active = -1;
    double bpm;
    const char *state;
    (void)u;
    if (!GTK_IS_WIDGET(U.tempo_sb)) return G_SOURCE_CONTINUE;
    /* The bottom line says whether a plug-in is what would sound, and nothing
     * tells it when that changes -- loading happens in the pane, and a load
     * that failed changes it back. Cheaper to notice here than to thread a
     * callback out of plugview for one label. */
    if (plugview_active() != was_active) {
        was_active = plugview_active();
        set_status();
    }
    audio_state_update();
    if (!plugview_active()) return G_SOURCE_CONTINUE;
    bpm = plugview_tempo();
    if (bpm >= 20.0 &&
        fabs(bpm - gtk_spin_button_get_value(GTK_SPIN_BUTTON(U.tempo_sb))) > 0.05) {
        U.tempo_echo = 1;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.tempo_sb), bpm);
        U.tempo_echo = 0;
    }
    state = U.clock_seen ? (plugview_playing() ? "following clock" : "clock, stopped")
                         : (plugview_playing() ? "internal" : "stopped");
    if (GTK_IS_LABEL(U.tempo_state))
        gtk_label_set_text(GTK_LABEL(U.tempo_state), state);
    return G_SOURCE_CONTINUE;
}

static void setup_midi(void)
{
    U.midi[0] = '\0';
    /* DUPLEX rather than INPUT: one client owning both directions is what makes
     * this look like an ordinary MIDI device to everything else, and it is the
     * only way an out port can exist at all. The Qt window has done this from
     * the start; this one could receive and never send. */
    if (snd_seq_open(&U.seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        U.seq = NULL;
        snprintf(U.midi, sizeof U.midi, "  (no MIDI)");
        return;
    }
    /* A second instance must not be called "dwstudio" as well. ALSA allows
     * duplicate client names, so two of them give a tracker two entries called
     * "dwstudio:dwstudio in" with nothing to tell them apart, and picking the
     * wrong one looks exactly like MIDI not working. Number the later ones,
     * the way pestudio does. */
    {
        char name[32] = "dwstudio";
        snd_seq_client_info_t *ci = NULL;
        if (snd_seq_client_info_malloc(&ci) >= 0) {
            int n;
            for (n = 2; n < 32; n++) {
                int taken = 0;
                snd_seq_client_info_set_client(ci, -1);
                while (snd_seq_query_next_client(U.seq, ci) >= 0) {
                    const char *o = snd_seq_client_info_get_name(ci);
                    if (snd_seq_client_info_get_client(ci) != snd_seq_client_id(U.seq)
                        && o && !strcmp(o, name)) { taken = 1; break; }
                }
                if (!taken) break;
                snprintf(name, sizeof name, "dwstudio %d", n);
            }
            snd_seq_client_info_free(ci);
        }
        snd_seq_set_client_name(U.seq, name);
    }
    U.seqport = snd_seq_create_simple_port(U.seq, "dwstudio in",
                    SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                    SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTHESIZER);
    U.seqout = snd_seq_create_simple_port(U.seq, "dwstudio out",
                    SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                    SND_SEQ_PORT_TYPE_MIDI_GENERIC);
    {   /* Said the way aconnect and every tracker's device list say it. */
        snd_seq_client_info_t *ci = NULL;
        const char *nm = "dwstudio";
        if (snd_seq_client_info_malloc(&ci) >= 0 &&
            snd_seq_get_client_info(U.seq, ci) >= 0)
            nm = snd_seq_client_info_get_name(ci);
        snprintf(U.port, sizeof U.port, "%d:%d (%s in)",
                 snd_seq_client_id(U.seq), U.seqport, nm);
        if (ci) snd_seq_client_info_free(ci);
    }

    midi_rescan();

    g_timeout_add(1, poll_midi, NULL);   /* 1 ms: MIDI jitter is nearly free */
    /* The input meter. Thirty a second is what the eye wants from a level and
     * far less than the audio thread produces, so the peak is taken and reset
     * rather than sampled -- a transient between two reads still shows. */
    g_timeout_add(33, poll_mic_level, NULL);
    /* The transport is read rather than watched: a clock arrives 24 times a
     * quarter note and the number on screen only has to be right, not early. */
    g_timeout_add(200, poll_transport, NULL);
}

/* -------------------------------------------------------------------- main */

/* ---------------------------------------------------------------- File menu */

/* The same two commands pestudio carries, under the same names.
 *
 * They used to be a "Folder…" button inside the plug-in pane, which meant the
 * way into a plug-in depended on which window you had open and which page you
 * were looking at. A menu bar sits above the stack, so both are reachable from
 * either half. */
static void act_open_vst(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    /* Whatever is opened is a plug-in, so show the half that hosts it. */
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    plugview_open_vst(GTK_WINDOW(U.win));
}

static void act_load_folder(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    plugview_load_folder(GTK_WINDOW(U.win));
}

/* Settings > Plug-in Folders. Where the plug-ins are, set once and kept --
 * and kept where pestudio reads it too, so the two windows search the same
 * places. Load Folder above adds one in passing; this is the list itself. */
static void act_plugin_folders(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    plugview_edit_folders(GTK_WINDOW(U.win));
}

/* A plug-in's own programs are its factory presets and are read-only, so a
 * sound somebody made has nowhere to live without these. The file is the JSON
 * the command line already reads and writes, and pestudio reads and writes the
 * same one, so a patch travels between all three. */
static void act_save_patch(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    plugview_save_patch(GTK_WINDOW(U.win));
}

static void act_open_patch(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    plugview_load_patch(GTK_WINDOW(U.win));
}

/* Some plug-ins do nothing until something has been typed into them: a
 * registration panel over the plug-in's own interface, and no sound until its
 * serial number has been entered into it. Typing already reaches an editor --
 * the editor widget forwards every key -- so what this adds is somewhere to
 * paste a key rather than typing it blind into a skinned field. */
static void act_enter_key(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    plugview_enter_key(GTK_WINDOW(U.win));
}

/* Plug-ins that need data they have not got are marked in the list and spelled
 * out in the status line; this is what does something about the ones that can
 * be. Under File because it acts on the whole scanned folder rather than on
 * whichever plug-in happens to be selected. */
static void act_install_data(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    plugview_install_missing_data();
}

/* --------------------------------------------------------------- Inputs menu */

/* The audio device list, as a menu of radio items. "system default" is first
 * and is what an empty target means; the rest are what the last scan found.
 * pestudio's Audio input menu is the same list in the same order, built from
 * the same node names, so a device chosen in one window can be named in the
 * other. */
static void audio_menu_rebuild(void)
{
    GMenuItem *it;
    int i;

    if (!U.audio_menu) return;
    g_menu_remove_all(U.audio_menu);
    it = g_menu_item_new("system default", NULL);
    g_menu_item_set_action_and_target_value(it, "win.audio-input",
                                            g_variant_new_string(""));
    g_menu_append_item(U.audio_menu, it);
    g_object_unref(it);
    for (i = 0; i < g_nindev; i++) {
        it = g_menu_item_new(g_indev[i].label, NULL);
        g_menu_item_set_action_and_target_value(it, "win.audio-input",
                                                g_variant_new_string(g_indev[i].node));
        g_menu_append_item(U.audio_menu, it);
        g_object_unref(it);
    }
}

/* Whether anything is actually arriving, under the device that was picked. A
 * device that is connected and silent and one that is not connected at all
 * look identical until something counts frames, and "the microphone does
 * nothing" is the same complaint either way. */
static void audio_state_update(void)
{
    static unsigned long seen;
    unsigned long now = atomic_load_explicit(&g_cap_frames, memory_order_relaxed);
    const char *state = !g_cap_stream ? "no input stream"
                      : now == 0      ? "open, nothing received yet"
                      : now != seen   ? "receiving audio"
                                      : "open, idle";
    seen = now;
    if (!U.audio_state || !strcmp(state, U.audio_said)) return;
    snprintf(U.audio_said, sizeof U.audio_said, "%s", state);
    g_menu_remove_all(U.audio_state);
    g_menu_append(U.audio_state, state, NULL);   /* no action: a line, not a choice */
}

static void on_audio_input(GSimpleAction *a, GVariant *v, gpointer ud)
{
    const char *node = g_variant_get_string(v, NULL);
    (void)ud;
    if (!capture_open(node)) {
        snprintf(U.hint, sizeof U.hint, "   could not open that input device");
        set_status();
        return;
    }
    g_simple_action_set_state(a, v);
    /* Choosing an input is the whole of what "turn the microphone on" means to
     * anyone doing it. Leaving the effect source elsewhere afterwards makes the
     * choice do nothing audible, and the only sign is a meter that never moves
     * -- so route it here and say so, rather than making it two steps that look
     * like one. pestudio does the same on the same click. */
    src_select(SRC_INPUT);
    snprintf(U.hint, sizeof U.hint, "   input: %s — effect input switched to it",
             node[0] ? node : "system default");
    set_status();
    audio_state_update();
}

/* The input-channel mask, which is a vocoder question: a vocoder has a
 * modulator and a carrier bus and wants the microphone on one of them, and
 * sending it to both puts the raw voice in the output beside the analysis.
 * Re-applied on every load, because the mask lives on the plug-in handle and a
 * fresh plug-in starts without one. */
static void apply_input_mask(void)
{
    GAction *a;
    if (!U.win) return;
    a = g_action_map_lookup_action(G_ACTION_MAP(U.win), "mic-raw");
    if (!a) return;
    {
        GVariant *st = g_action_get_state(a);
        plugview_set_input_mask(g_variant_get_boolean(st) ? 0x3u : 0u);
        g_variant_unref(st);
    }
}

/* Everything the window has to re-send after a load. Both of these live on the
 * plug-in handle rather than in the pane, so a fresh plug-in starts without
 * them and a load onto one of the same shape would not be noticed by anything
 * watching the pane. */
static void on_plugin_loaded(void)
{
    apply_input_mask();
    /* Ask what the plug-in has rather than whether it calls itself a synth --
     * Full Bucket's vocoder is a synth with two inputs, and asking the wrong
     * question leaves it on silence with nothing to vocode. The microphone is
     * never overridden: choosing a device and then loading the plug-in you
     * meant to use it with is the ordinary order to do things in, and
     * reverting to the keys on every load is indistinguishable from the
     * microphone not working. pestudio decides it the same way. */
    if (atomic_load_explicit(&g_src, memory_order_relaxed) != SRC_INPUT)
        src_select(plugview_num_inputs() > 0 ? SRC_NOTES : SRC_SILENCE);
    set_status();
}

static void on_mic_raw(GSimpleAction *a, GVariant *v, gpointer ud)
{
    (void)ud;
    g_simple_action_set_state(a, v);
    apply_input_mask();
}

/* Both halves of "what is this machine listening to", rescanned together --
 * plugging something in is exactly when both lists are wrong, and pestudio's
 * Rescan does the same two things. */
static void act_rescan(GSimpleAction *a, GVariant *p, gpointer ud)
{
    int n, i, kept = 0;
    (void)a; (void)p; (void)ud;
    scan_input_devices();
    audio_menu_rebuild();
    if (g_cap_target[0]) {
        for (i = 0; i < g_nindev; i++)
            if (!strcmp(g_indev[i].node, g_cap_target)) { kept = 1; break; }
        /* The device that was chosen has gone. Say so rather than quietly
         * listening to something else. */
        if (!kept) {
            capture_open("");
            snprintf(U.hint, sizeof U.hint,
                     "   that input device is gone — back to the system default");
        }
    }
    n = midi_rescan();
    if (kept || !g_cap_target[0])
        snprintf(U.hint, sizeof U.hint, n ? "   MIDI: %d new connection(s)"
                                          : "   MIDI: no new sources", n);
    audio_state_update();
    set_status();
}

/* ---------------------------------------------------------------- MIDI menu */

/* Everything a tracker or a DAW at the other end of the port can ask of this
 * window, in the place people look for it. The same settings sit in the row
 * under the menu bar; pestudio carries both too, for the same reason -- one is
 * where you look while playing, the other is where you look when it is not
 * working. */
static void on_midi_thru(GSimpleAction *a, GVariant *v, gpointer ud)
{
    (void)ud;
    U.thru = g_variant_get_boolean(v);
    g_simple_action_set_state(a, v);
}

static void on_midi_out_auto(GSimpleAction *a, GVariant *v, gpointer ud)
{
    (void)ud;
    U.autoout = g_variant_get_boolean(v);
    g_simple_action_set_state(a, v);
    /* Turning it on is a request to connect now, not next time something is
     * plugged in. Turning it off leaves the subscriptions that exist: dropping
     * them would cut a synth off mid-note, and `aconnect -d` is the tool for
     * unmaking a connection somebody wanted. */
    if (U.autoout) midi_rescan();
    else           midi_conn_update();
}

static void on_midi_channel(GSimpleAction *a, GVariant *v, gpointer ud)
{
    int ch = g_variant_get_int32(v);
    (void)ud;
    atomic_store_explicit(&g_midi_ch, ch, memory_order_relaxed);
    g_simple_action_set_state(a, v);
    if (GTK_IS_WIDGET(U.chan_dd) && !U.chan_echo) {
        U.chan_echo = 1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U.chan_dd), (guint)(ch + 1));
        U.chan_echo = 0;
    }
}

/* All notes off, and everything that goes with it.
 *
 * Not only the plug-in: a note stuck on is stuck wherever it is sounding, so
 * the engines, the drawn keys and the port all hear about it. The wheel comes
 * back to centre with them, because a bend the plug-in still thinks is applied
 * survives every note being cut and puts the next thing played in the wrong
 * key -- which is harder to recognise as the cause than a held note is. */
static void act_panic(GSimpleAction *a, GVariant *p, gpointer ud)
{
    int n;
    (void)a; (void)p; (void)ud;
    for (n = 0; n < 128; n++) note_stop(n, 1);
    if (plugview_active()) plugview_all_notes_off();
    ev_push(EV_ALLOFF, 0, 0);
    midi_send(0xB0, 123, 0);
    bend_recentre();
}

/* ------------------------------------------------------------- the keyboard */

/* Reaching the window without the mouse.
 *
 * A list that has focus is walked with the arrow keys and loads what it lands
 * on, so these four plus the file commands are the whole window: find a
 * plug-in, pick a program, look at its editor, and get back to the keys. */
static void act_focus_plugins(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; (void)ud; plugview_focus_list(); }

static void act_focus_programs(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; (void)ud; plugview_focus_programs(); }

static void act_toggle_editor(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; (void)ud; plugview_toggle_editor(); }

/* Back to playing. Focus in a list means the letters are keyboard navigation
 * before they are notes, and Escape is where a hand goes to get out of
 * something -- so it is what puts the keys back under the fingers. */
static void act_focus_keys(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    if (GTK_IS_WIDGET(U.piano)) gtk_widget_grab_focus(U.piano);
}

static void vol_nudge(int by)
{
    double v;
    if (!GTK_IS_RANGE(U.vol)) return;
    v = gtk_range_get_value(GTK_RANGE(U.vol)) + by;
    gtk_range_set_value(GTK_RANGE(U.vol), v < 0 ? 0 : v > 150 ? 150 : v);
}

static void act_vol_up(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; (void)ud; vol_nudge(5); }

static void act_vol_down(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; (void)ud; vol_nudge(-5); }

static void act_quit(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; (void)ud; gtk_window_close(GTK_WINDOW(U.win)); }


/* Which build this is. The usual way it gets asked is somebody reporting
 * behaviour from a copy neither of us can identify, so the commit is in here
 * too when the build came from a checkout. */
static void act_about(GSimpleAction *a, GVariant *p, gpointer u)
{
    GtkAlertDialog *d;
    char body[512];
    (void)a; (void)p; (void)u;

    snprintf(body, sizeof body,
             "Built %s%s%s\n\n"
             "Runs Windows, macOS and Linux audio plug-ins as native code on "
             "Linux \xe2\x80\x94 a PE loader with a Win32 subsystem under it, "
             "a Mach-O loader with an Objective-C runtime, and a CFM/PEF "
             "interpreter. Not emulation, and not Wine.\n\n"
             "This window is dwstudio (GTK4).",
             VSTACE_BUILD_DATE,
             VSTACE_GIT[0] ? "\nCommit " : "",
             VSTACE_GIT[0] ? VSTACE_GIT : "");

    /* GtkAlertDialog rather than GtkMessageDialog, which GTK 4.10 deprecated
     * and which warns on every build here; the file dialogs beside this one
     * are already on the newer async API. */
    d = gtk_alert_dialog_new("vst-ace %s", VSTACE_VERSION);
    gtk_alert_dialog_set_detail(d, body);
    gtk_alert_dialog_show(d, GTK_WINDOW(U.win));
    g_object_unref(d);
}

static GtkWidget *build_menubar(GtkApplication *app)
{
    static const GActionEntry entries[] = {
        { "open-vst",    act_open_vst,    NULL, NULL, NULL, {0} },
        { "load-folder", act_load_folder, NULL, NULL, NULL, {0} },
        { "install-data", act_install_data, NULL, NULL, NULL, {0} },
        { "save-patch",  act_save_patch,  NULL, NULL, NULL, {0} },
        { "open-patch",  act_open_patch,  NULL, NULL, NULL, {0} },
        { "plugin-folders", act_plugin_folders, NULL, NULL, NULL, {0} },
        { "enter-key",   act_enter_key,   NULL, NULL, NULL, {0} },
        { "rescan",      act_rescan,      NULL, NULL, NULL, {0} },
        { "panic",       act_panic,       NULL, NULL, NULL, {0} },
        { "focus-plugins",  act_focus_plugins,  NULL, NULL, NULL, {0} },
        { "focus-programs", act_focus_programs, NULL, NULL, NULL, {0} },
        { "toggle-editor",  act_toggle_editor,  NULL, NULL, NULL, {0} },
        { "focus-keys",     act_focus_keys,     NULL, NULL, NULL, {0} },
        { "vol-up",         act_vol_up,         NULL, NULL, NULL, {0} },
        { "vol-down",       act_vol_down,       NULL, NULL, NULL, {0} },
        /* Stateful, so the menu draws the check itself and the state is the
         * one place the answer lives. A boolean entry with no activate handler
         * toggles on its own and reports through change_state. */
        { "midi-thru",     NULL, NULL,  "false", on_midi_thru,     {0} },
        { "midi-out-auto", NULL, NULL,  "false", on_midi_out_auto, {0} },
        { "midi-channel",  NULL, "i",   "-1",    on_midi_channel,  {0} },
        { "audio-input",   NULL, "s",   "''",    on_audio_input,   {0} },
        /* Checked to begin with, like pestudio's: a vocoder fed on both buses
         * is the case that sounds wrong, and it is the commoner one. */
        { "mic-raw",       NULL, NULL,  "true",  on_mic_raw,       {0} },
        { "quit",        act_quit,        NULL, NULL, NULL, {0} },
        { "about",       act_about,       NULL, NULL, NULL, {0} },
    };
    GMenu *bar      = g_menu_new();
    GMenu *file     = g_menu_new();
    GMenu *sect     = g_menu_new();
    GMenu *settings = g_menu_new();
    GMenu *view     = g_menu_new();
    GMenu *inputs   = g_menu_new();
    GMenu *midiin   = g_menu_new();
    GMenu *midisrcs = g_menu_new();
    GMenu *audioin  = g_menu_new();
    GMenu *audiodevs = g_menu_new();
    GMenu *audiostate = g_menu_new();
    GMenu *chan     = g_menu_new();
    GMenu *midisect = g_menu_new();
    GMenu *about    = g_menu_new();
    GtkWidget *w;
    int i;

    g_action_map_add_action_entries(G_ACTION_MAP(U.win), entries,
                                    G_N_ELEMENTS(entries), NULL);
    gtk_application_set_accels_for_action(app, "win.open-vst",
                                          (const char *[]){ "<Control>o", NULL });
    gtk_application_set_accels_for_action(app, "win.load-folder",
                                          (const char *[]){ "<Control>l", NULL });
    gtk_application_set_accels_for_action(app, "win.save-patch",
                                          (const char *[]){ "<Control>s", NULL });
    gtk_application_set_accels_for_action(app, "win.open-patch",
                                          (const char *[]){ "<Control>p", NULL });
    gtk_application_set_accels_for_action(app, "win.quit",
                                          (const char *[]){ "<Control>q", NULL });
    /* Everything on this menu has a key, so the window can be driven without
     * reaching for the mouse -- which is the difference between changing a
     * MIDI setting mid-take and stopping to hunt through a menu for it. GTK
     * prints them beside the items, so the menu is also where they are
     * learned.
     *
     * All of them carry Ctrl. The note keys are plain letters, and a bare
     * shortcut would be a letter that no longer plays -- z, x, c and v are
     * the bottom octave, not commands. */
    gtk_application_set_accels_for_action(app, "win.rescan",
                                          (const char *[]){ "<Control>r", "F5", NULL });
    gtk_application_set_accels_for_action(app, "win.midi-thru",
                                          (const char *[]){ "<Control>t", NULL });
    gtk_application_set_accels_for_action(app, "win.midi-out-auto",
                                          (const char *[]){ "<Control>h", NULL });
    /* Panic is the one that gets wanted in a hurry, so it is also the one that
     * must not need aim: a note stuck on in front of an audience is fixed with
     * one hand while the other is still on the keys. */
    gtk_application_set_accels_for_action(app, "win.panic",
                                          (const char *[]){ "<Control>period",
                                                            "<Control>Escape", NULL });
    gtk_application_set_accels_for_action(app, "win.plugin-folders",
                                          (const char *[]){ "<Control>d", NULL });
    gtk_application_set_accels_for_action(app, "win.enter-key",
                                          (const char *[]){ "<Control>k", NULL });
    gtk_application_set_accels_for_action(app, "win.install-data",
                                          (const char *[]){ "<Control>i", NULL });
    gtk_application_set_accels_for_action(app, "win.focus-plugins",
                                          (const char *[]){ "<Control>f", NULL });
    gtk_application_set_accels_for_action(app, "win.focus-programs",
                                          (const char *[]){ "<Control>g", NULL });
    gtk_application_set_accels_for_action(app, "win.toggle-editor",
                                          (const char *[]){ "<Control>e", NULL });
    gtk_application_set_accels_for_action(app, "win.focus-keys",
                                          (const char *[]){ "Escape", NULL });
    gtk_application_set_accels_for_action(app, "win.vol-up",
                                          (const char *[]){ "<Control>Up", NULL });
    gtk_application_set_accels_for_action(app, "win.vol-down",
                                          (const char *[]){ "<Control>Down", NULL });

    g_menu_append(file, "Open VST…",    "win.open-vst");
    g_menu_append(file, "Load Folder…", "win.load-folder");
    g_menu_append(file, "Install Missing Plug-in Data", "win.install-data");
    g_menu_append(file, "Save Patch…", "win.save-patch");
    g_menu_append(file, "Open Patch…", "win.open-patch");
    g_menu_append(sect, "Quit",         "win.quit");
    g_menu_append_section(file, NULL, G_MENU_MODEL(sect));
    g_menu_append_submenu(bar, "File", G_MENU_MODEL(file));

    /* Between File and About, matching pestudio. */
    g_menu_append(view, "Plug-in List", "win.focus-plugins");
    g_menu_append(view, "Programs", "win.focus-programs");
    g_menu_append(view, "Parameters / Editor", "win.toggle-editor");
    g_menu_append(view, "Back to the Keys", "win.focus-keys");
    g_menu_append_submenu(bar, "Go", G_MENU_MODEL(view));

    g_menu_append(settings, "Plug-in Folders…", "win.plugin-folders");
    g_menu_append(settings, "Enter Key / Serial…", "win.enter-key");
    g_menu_append_submenu(bar, "Settings", G_MENU_MODEL(settings));

    /* Inputs: what the machine is listening to, and what it plays out to.
     * pestudio's menu of the same name holds the same things in the same
     * order, so the question is answered the same way in either window.
     *
     * The list of sources is rebuilt by every scan rather than being filled in
     * once here -- U.midi_menu is that section, and midi_conn_update owns it.
     * The reference on it is deliberately kept for as long as the window
     * lives, because that is how long it goes on being refilled. */
    U.midi_menu   = midisrcs;
    U.audio_menu  = audiodevs;
    U.audio_state = audiostate;
    g_menu_append_section(midiin, NULL, G_MENU_MODEL(midisrcs));
    g_menu_append_section(audioin, NULL, G_MENU_MODEL(audiodevs));
    g_menu_append_section(audioin, NULL, G_MENU_MODEL(audiostate));
    g_menu_append(audioin, "Mute raw (first two channels only)", "win.mic-raw");
    g_menu_append_submenu(inputs, "Audio input", G_MENU_MODEL(audioin));
    for (i = -1; i < 16; i++) {
        char label[16], action[32];
        if (i < 0) snprintf(label, sizeof label, "All channels");
        else       snprintf(label, sizeof label, "Channel %d", i + 1);
        snprintf(action, sizeof action, "win.midi-channel(%d)", i);
        g_menu_append(chan, label, action);
    }
    g_menu_append_submenu(midisect, "Channel", G_MENU_MODEL(chan));
    g_menu_append(midisect, "Thru (in → out)", "win.midi-thru");
    g_menu_append(midisect, "Connect out to hardware", "win.midi-out-auto");
    g_menu_append_section(midiin, NULL, G_MENU_MODEL(midisect));
    g_menu_append_submenu(inputs, "MIDI input", G_MENU_MODEL(midiin));
    g_menu_append(inputs, "Rescan devices", "win.rescan");
    g_menu_append(inputs, "All Notes Off", "win.panic");
    g_menu_append_submenu(bar, "Inputs", G_MENU_MODEL(inputs));

    /* Next to File, matching pestudio, so the same question is answered the
     * same way in whichever window is open. */
    g_menu_append(about, "About vst-ace", "win.about");
    g_menu_append_submenu(bar, "About", G_MENU_MODEL(about));

    w = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    g_object_unref(about); g_object_unref(settings); g_object_unref(view);
    /* midisrcs, audiodevs and audiostate are not unreffed with the rest: each
     * is refilled for as long as the window lives -- see U.midi_menu above. */
    g_object_unref(chan); g_object_unref(midisect);
    g_object_unref(midiin); g_object_unref(audioin); g_object_unref(inputs);
    g_object_unref(sect); g_object_unref(file); g_object_unref(bar);
    return w;
}

static void scan(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *nm;
    int skipped = 0, i, j;

    if (!d) { fprintf(stderr, "no such directory: %s\n", dir); return; }
    while ((nm = g_dir_read_name(d)) && g_ninst < MAX_INST) {
        char path[512];
        instrument in;
        if (!g_str_has_suffix(nm, ".dll")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, nm);
        if (!load_instrument(path, &in)) continue;
        if (in.eng == ENG_NONE && !g_show_all) { skipped++; continue; }
        g_inst[g_ninst++] = in;
    }
    g_dir_close(d);

    /* playable first, then by name */
    for (i = 0; i < g_ninst; i++)
        for (j = i + 1; j < g_ninst; j++) {
            int swap = (g_inst[j].eng != ENG_NONE && g_inst[i].eng == ENG_NONE) ||
                       ((g_inst[i].eng == ENG_NONE) == (g_inst[j].eng == ENG_NONE) &&
                        strcmp(g_inst[j].name, g_inst[i].name) < 0);
            if (swap) { instrument t = g_inst[i]; g_inst[i] = g_inst[j]; g_inst[j] = t; }
        }
    if (skipped)
        snprintf(U.hint, sizeof U.hint,
                 "  (%d more without an engine; --all to browse them)", skipped);
}

/* The window going away is what stops the plug-in half.
 *
 * It has to happen here rather than after g_application_run returns: plugview
 * drives its editor pump and level meter from GTK timeouts that hold widget
 * pointers, and those keep firing while the toplevel finalises its children.
 * By the time the main loop returns they have already run against freed
 * widgets. `destroy` is emitted before any of that, so it is the last moment
 * the pointers are still good.
 *
 * The audio callback is parked first because closing a plug-in it may be
 * rendering out of is a crash rather than a message, and left parked -- both
 * backends answer a park with silence, which is what a closing window should
 * be doing anyway. */
static void on_win_destroy(GtkWidget *w, gpointer u)
{
    (void)w; (void)u;
    engine_park(&g_eng);
    plugview_shutdown();
}

static void activate(GtkApplication *app, gpointer ud)
{
    GtkWidget *box, *top, *paned, *sw1, *sw2, *frame, *kbox;
    GtkEventController *kc;
    GtkGesture *click;
    int i;
    (void)ud;

    U.bend = BEND_CENTRE;
    U.win = gtk_application_window_new(app);
    g_signal_connect(U.win, "destroy", G_CALLBACK(on_win_destroy), NULL);
    gtk_window_set_title(GTK_WINDOW(U.win), "dwstudio");
    gtk_window_set_default_size(GTK_WINDOW(U.win), 960, 640);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);   gtk_widget_set_margin_bottom(box, 8);

    /* One window, hosting plug-ins.
     *
     * This began as a front end for the engines in c/src and grew a plug-in
     * host beside them, with a switcher to pick which half you were looking
     * at. Hosting is what it is for, so the switcher is gone and the plug-in
     * pane is the window rather than half of it. The engines are still what
     * sounds when nothing is loaded -- they cost nothing to keep and a window
     * that makes no noise until a plug-in is picked is harder to tell from a
     * broken one -- they are simply not something to be chosen any more.
     *
     * The rows above the pane are what both halves always shared: the input,
     * the output level, and the MIDI ports. They live out here rather than on
     * a page so they stay visible whatever the pane is showing. */
    {
        GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        U.outer  = outer;
        U.topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        U.midibar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        U.mode = gtk_stack_new();
        U.plug = plugview_new(plug_park, plug_unpark, SR, g_period);
        gtk_stack_add_titled(GTK_STACK(U.mode), box, "engines", "Engines");
        gtk_stack_add_titled(GTK_STACK(U.mode), U.plug, "plugins", "Plug-ins");
        gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
        gtk_widget_set_vexpand(U.mode, TRUE);

        gtk_widget_set_margin_start(outer, 8); gtk_widget_set_margin_end(outer, 8);
        gtk_widget_set_margin_top(outer, 4);   gtk_widget_set_margin_bottom(outer, 8);
        gtk_box_append(GTK_BOX(outer), build_menubar(app));
        gtk_box_append(GTK_BOX(outer), U.topbar);
        gtk_box_append(GTK_BOX(outer), U.midibar);
        gtk_box_append(GTK_BOX(outer), U.mode);
        gtk_window_set_child(GTK_WINDOW(U.win), outer);
        gtk_widget_set_margin_start(box, 0); gtk_widget_set_margin_end(box, 0);
        gtk_widget_set_margin_top(box, 0);   gtk_widget_set_margin_bottom(box, 0);
    }

    top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    U.instmodel = gtk_string_list_new(NULL);
    U.bankmodel = gtk_string_list_new(NULL);
    for (i = 0; i < g_ninst; i++) {
        char lbl[96];
        snprintf(lbl, sizeof lbl, "%s%s", g_inst[i].name,
                 g_inst[i].eng == ENG_DW ? "   (DW-8000)"
                   : g_inst[i].eng == ENG_FM ? "   (4-op FM)"
                   : g_inst[i].eng == ENG_JUNO ? "   (Juno-6)"
                   : g_inst[i].eng == ENG_DRUM ? "   (samples)" : "");
        gtk_string_list_append(U.instmodel, lbl);
    }
    U.instdd = gtk_drop_down_new(G_LIST_MODEL(U.instmodel), NULL);
    U.bankdd = gtk_drop_down_new(G_LIST_MODEL(U.bankmodel), NULL);
    gtk_widget_set_hexpand(U.instdd, TRUE);
    gtk_widget_set_hexpand(U.bankdd, TRUE);
    gtk_box_append(GTK_BOX(top), gtk_label_new("Instrument"));
    gtk_box_append(GTK_BOX(top), U.instdd);
    gtk_box_append(GTK_BOX(top), gtk_label_new("Bank"));
    gtk_box_append(GTK_BOX(top), U.bankdd);
    gtk_box_append(GTK_BOX(box), top);

    /* The microphone. An effect plug-in is only audible if something is fed to
     * it, so this is not a nicety: without an input, every effect in the corpus
     * renders silence and reads as broken.
     *
     * Mic gain and a level beside it, because the two are only useful together
     * -- a gain with no meter is a guess, and the meter is what says whether
     * the input is the reason nothing is coming out. */
    {
        GtkWidget *g;
        gtk_box_append(GTK_BOX(U.topbar), gtk_label_new("Mic"));
        U.mic_level = gtk_level_bar_new_for_interval(0.0, 1.0);
        gtk_widget_set_size_request(U.mic_level, 90, -1);
        gtk_widget_set_tooltip_text(U.mic_level,
            "the audio input after the mic gain — what the plug-in receives");
        gtk_box_append(GTK_BOX(U.topbar), U.mic_level);
        g = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 400, 1);
        gtk_range_set_value(GTK_RANGE(g), 100);
        gtk_widget_set_size_request(g, 90, -1);
        gtk_scale_set_draw_value(GTK_SCALE(g), FALSE);
        gtk_widget_set_tooltip_text(g, "mic gain (100% is unity)");
        g_signal_connect(g, "value-changed", G_CALLBACK(on_mic_gain), NULL);
        gtk_box_append(GTK_BOX(U.topbar), g);
        {   /* silence / keys / noise / input, in pestudio's order so the
             * index means the same thing in both windows. */
            static const char *srcs[] = { "silence", "keys", "noise", "input", NULL };
            gtk_box_append(GTK_BOX(U.topbar), gtk_label_new("Effect in"));
            U.srcdd = gtk_drop_down_new_from_strings(srcs);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(U.srcdd), SRC_SILENCE);
            gtk_widget_set_tooltip_text(U.srcdd,
                "what to feed an effect's input — a synth ignores it.\n"
                "keys plays a sawtooth per held note; input is the microphone");
            g_signal_connect(U.srcdd, "notify::selected",
                             G_CALLBACK(on_src_changed), NULL);
            gtk_box_append(GTK_BOX(U.topbar), U.srcdd);
        }
        g = gtk_check_button_new_with_label("Mute raw");
        gtk_widget_set_tooltip_text(g,
            "feed the input only to the plug-in's first two channels — what a "
            "vocoder wants, so the raw voice is not in the output beside it");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(g), "win.mic-raw");
        gtk_box_append(GTK_BOX(U.topbar), g);
    }

    /* The MIDI row: what a tracker at the other end of the port needs from
     * this window, and what this window needs to say back. The same settings
     * are under Inputs on the menu bar -- this row is where you look while
     * playing, the menu is where you look when it is not working. */
    {
        static const char *chl[] = { "Omni","1","2","3","4","5","6","7","8",
                                     "9","10","11","12","13","14","15","16", NULL };
        GtkWidget *b;
        U.chan_dd = gtk_drop_down_new_from_strings(chl);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U.chan_dd), 0);
        gtk_widget_set_tooltip_text(U.chan_dd,
            "which MIDI channel to listen on; Omni is all of them");
        g_signal_connect(U.chan_dd, "notify::selected",
                         G_CALLBACK(on_chan_changed), NULL);
        gtk_box_append(GTK_BOX(U.midibar), gtk_label_new("MIDI Ch"));
        gtk_box_append(GTK_BOX(U.midibar), U.chan_dd);

        /* Somewhere to say the tempo when nothing is sending clock. An
         * arpeggiator or a synced delay has to be told by someone, and if no
         * sequencer is driving this is the only way to say it. When clock is
         * arriving the box follows it rather than fighting it. */
        gtk_box_append(GTK_BOX(U.midibar), gtk_label_new("Tempo"));
        U.tempo_sb = gtk_spin_button_new_with_range(20.0, 999.0, 0.25);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(U.tempo_sb), 2);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.tempo_sb), 120.0);
        gtk_widget_set_tooltip_text(U.tempo_sb,
            "the tempo the plug-in is told, in BPM; a sequencer's clock "
            "overrides it");
        g_signal_connect(U.tempo_sb, "value-changed", G_CALLBACK(on_tempo), NULL);
        gtk_box_append(GTK_BOX(U.midibar), U.tempo_sb);
        U.tempo_state = gtk_label_new("stopped");
        gtk_widget_add_css_class(U.tempo_state, "dim-label");
        gtk_box_append(GTK_BOX(U.midibar), U.tempo_state);

        /* The same two settings as the menu's, bound to the same actions, so
         * either place shows what the other did. pestudio carries them as
         * check boxes in its MIDI panel; this is that panel's row. */
        b = gtk_check_button_new_with_label("Thru");
        gtk_widget_set_tooltip_text(b,
            "echo everything that arrives straight back out of the MIDI port");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), "win.midi-thru");
        gtk_box_append(GTK_BOX(U.midibar), b);
        b = gtk_check_button_new_with_label("Out → HW");
        gtk_widget_set_tooltip_text(b,
            "connect this window's MIDI out to every hardware input found");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), "win.midi-out-auto");
        gtk_box_append(GTK_BOX(U.midibar), b);

        b = gtk_button_new_with_label("All Notes Off");
        gtk_widget_set_tooltip_text(b,
            "cut every note, here and downstream, and recentre the wheel");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), "win.panic");
        gtk_box_append(GTK_BOX(U.midibar), b);
        b = gtk_button_new_with_label("Rescan");
        gtk_widget_set_tooltip_text(b,
            "look for MIDI devices connected since this window opened");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), "win.rescan");
        gtk_box_append(GTK_BOX(U.midibar), b);

        U.midi_conn = gtk_label_new("not open");
        gtk_label_set_ellipsize(GTK_LABEL(U.midi_conn), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(U.midi_conn), 0.0);
        gtk_widget_set_hexpand(U.midi_conn, TRUE);
        gtk_widget_add_css_class(U.midi_conn, "dim-label");
        gtk_box_append(GTK_BOX(U.midibar), U.midi_conn);
    }

    U.list = gtk_list_box_new();
    sw1 = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw1), U.list);
    U.info = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(U.info), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(U.info), TRUE);
    sw2 = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw2), U.info);
    U.infosw = sw2;

    build_panel();
    U.panelsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(U.panelsw), U.panel);

    {
        GtkWidget *rightbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_vexpand(sw2, TRUE);
        gtk_widget_set_vexpand(U.panelsw, TRUE);
        gtk_box_append(GTK_BOX(rightbox), sw2);
        gtk_box_append(GTK_BOX(rightbox), U.panelsw);
        gtk_widget_set_visible(U.panelsw, FALSE);
        paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_start_child(GTK_PANED(paned), sw1);
        gtk_paned_set_end_child(GTK_PANED(paned), rightbox);
    }
    gtk_paned_set_position(GTK_PANED(paned), 400);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(box), paned);

    /* Below the stack, not inside it: the keyboard plays whichever half is
     * showing, and a plug-in you cannot play is not much of a plug-in host.
     *
     * The wheel sits to its left, where a hardware synth puts it, and the two
     * share a row so they always end up the same height. */
    U.piano = gtk_drawing_area_new();
    gtk_widget_set_hexpand(U.piano, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(U.piano), piano_draw, NULL, NULL);

    U.wheel = gtk_drawing_area_new();
    gtk_widget_set_size_request(U.wheel, 40, -1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(U.wheel), wheel_draw, NULL, NULL);
    gtk_widget_set_tooltip_text(U.wheel,
        "Pitch wheel — drag up or down; springs back to centre.\n"
        "Bends a loaded plug-in; the engines have no MIDI bend input.");
    {
        GtkGesture *wg = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(wg), GDK_BUTTON_PRIMARY);
        g_signal_connect(wg, "drag-begin",  G_CALLBACK(on_wheel_drag_begin),  NULL);
        g_signal_connect(wg, "drag-update", G_CALLBACK(on_wheel_drag_update), NULL);
        g_signal_connect(wg, "drag-end",    G_CALLBACK(on_wheel_drag_end),    NULL);
        gtk_widget_add_controller(U.wheel, GTK_EVENT_CONTROLLER(wg));
    }

    kbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(kbox), U.wheel);
    gtk_box_append(GTK_BOX(kbox), U.piano);
    U.kbrow = kbox;
    gtk_widget_set_size_request(kbox, -1, KEY_H);
    gtk_widget_set_vexpand(kbox, FALSE);

    /* Master volume, directly over the keys.
     *
     * It is the control reached for while playing -- the hand is already down
     * here -- and it does not need the width the row above was giving it: a
     * short bar is enough for a level, and the room is worth more to the port
     * list beside it. */
    {
        GtkWidget *vrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *kb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *lbl = gtk_label_new("Volume");
        U.vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 150, 1);
        gtk_range_set_value(GTK_RANGE(U.vol), 100);
        gtk_widget_set_size_request(U.vol, 160, -1);
        gtk_scale_set_draw_value(GTK_SCALE(U.vol), FALSE);
        gtk_widget_set_tooltip_text(U.vol, "master volume (100% is unity)");
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_widget_set_halign(vrow, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(vrow), lbl);
        gtk_box_append(GTK_BOX(vrow), U.vol);
        gtk_box_append(GTK_BOX(kb), vrow);
        gtk_box_append(GTK_BOX(kb), kbox);
        frame = gtk_frame_new("Keyboard  —  click, or zsxdcvgbhnjm / q2w3er5t6y7u");
        gtk_frame_set_child(GTK_FRAME(frame), kb);
    }
    gtk_box_append(GTK_BOX(U.outer), frame);

    U.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(U.status), 0.0);
    gtk_box_append(GTK_BOX(U.outer), U.status);

    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed",  G_CALLBACK(on_press), NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_release), NULL);
    gtk_widget_add_controller(U.piano, GTK_EVENT_CONTROLLER(click));

    kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed",  G_CALLBACK(on_key), NULL);
    g_signal_connect(kc, "key-released", G_CALLBACK(on_key_up), NULL);
    gtk_widget_add_controller(U.win, kc);
    /* The controller above is on the window, so it only sees what the focused
     * widget let past. A plug-in's editor takes focus when one of its knobs is
     * turned, so it has to be told which keys to let past -- this is that map.
     * See plugview_set_note_key. */
    plugview_set_note_key(key_note);
    plugview_set_load_hook(on_plugin_loaded);
    g_signal_connect(U.win, "notify::is-active", G_CALLBACK(on_win_active), NULL);

    g_signal_connect(U.instdd, "notify::selected", G_CALLBACK(on_inst_changed), NULL);
    g_signal_connect(U.bankdd, "notify::selected", G_CALLBACK(on_bank_changed), NULL);
    g_signal_connect(U.list, "row-selected", G_CALLBACK(on_patch), NULL);
    g_signal_connect(U.vol, "value-changed", G_CALLBACK(on_vol), NULL);

    setup_midi();
    /* After the audio is up, so there is a capture stream to point at, and so
     * the first Inputs menu opened is already the machine's real device list. */
    scan_input_devices();
    audio_menu_rebuild();
    audio_state_update();
    set_status();          /* now that there is something to say about MIDI */
    engine_start_audio(&g_eng);
    if (g_ninst) select_instrument(0);

    /* Opens on the plug-ins: that is what this window is usually wanted for
     * now, and the engines are a page away rather than the front door.
     *
     * Set here rather than where the stack is built, because selecting the
     * first instrument above shows and hides widgets on the engines page and
     * GtkStack takes that as a reason to show it. Last word wins, so this has
     * to have it. */
    gtk_stack_set_visible_child_name(GTK_STACK(U.mode), "plugins");
    if (g_cycle_ms > 0) plugview_start_cycle(g_cycle_ms);

    gtk_window_present(GTK_WINDOW(U.win));
}

int main(int argc, char **argv)
{
    GtkApplication *app;
    char dir[512];
    const char *base = NULL;
    int i, status;

    /* Software rendering for our own widgets, on purpose.
     *
     * The Plug-ins half hosts editors as foreign X11 child windows, and those
     * draw with GLX. The ngl renderer binds GTK's own GL to the toplevel's
     * whole window hierarchy, and the X server then refuses a plug-in's GLX
     * MakeCurrent on any descendant with BadAccess -- every GL editor either
     * fell back to an offscreen GLES context (Cardinal painted black) or died
     * inside its own toolkit, and after enough attach/detach cycles GTK's GL
     * came down too (SIGSEGV in libnvidia-eglcore out of gsk_renderer_render).
     * Cairo never touches GL, so the hierarchy stays free for the plug-ins.
     * Set in the environment already? The user's choice wins. */
    g_setenv("GSK_RENDERER", "cairo", FALSE);

    /* Ask for the X11 backend in a Wayland session, the same way pestudio asks
     * for xcb and for exactly the same reason: a native plug-in's editor embeds
     * through an X11 window id, and GDK hands one out only on the X11 backend.
     * Under Wayland there is nothing to give the plug-in and every native
     * editor is refused -- plugview says so, but only in the status line of a
     * window you have to open first.
     *
     * `dw gui` already sets this before exec'ing. Doing it here as well is what
     * makes running the binary directly -- which is what anyone debugging does
     * -- behave the same as launching it through dw. XWayland is present on any
     * Wayland desktop that can run these plug-ins at all. Set GDK_BACKEND
     * yourself to override. */
    if (!getenv("GDK_BACKEND")) {
        const char *sess = getenv("XDG_SESSION_TYPE");
        if ((sess && !strcmp(sess, "wayland")) || getenv("WAYLAND_DISPLAY")) {
            if (getenv("DISPLAY")) {
                g_setenv("GDK_BACKEND", "x11", TRUE);
                fprintf(stderr, "dwstudio: Wayland session -- using the x11 "
                                "backend so plug-in editors can embed\n");
            } else {
                fprintf(stderr, "dwstudio: Wayland session with no DISPLAY; "
                                "native plug-in editors need XWayland and "
                                "will be refused\n");
            }
        }
    }

    /* Host plug-ins out-of-process by default, which is what pestudio does and
     * for the same reason: this window is a browser, it loads a plug-in on
     * every click, and in-process a bad one ends the session. `NI Massive`
     * calls ExitProcess(1) while loading, and the Win32 stub for that is
     * exit(), so selecting it took dwstudio down with it -- no message, no
     * chance to pick something else. Behind the helper it costs a subprocess
     * and the load is reported as failed.
     *
     * Native Linux plug-ins are kept in this process by pehost regardless of
     * this setting: their editors are X11 windows embedded into ours and the
     * bridge carries pixels, not window ids. */
    if (!getenv("PEHOST_ISOLATE")) {
        pehost_set_isolation(1);
        fprintf(stderr, "dwstudio: hosting plug-ins out-of-process "
                        "(PEHOST_ISOLATE=0 to disable)\n");
    }

    dir[0] = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) g_show_all = TRUE;
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) base = argv[++i];
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) g_backend_want = argv[++i];
        else if (!strcmp(argv[i], "--cycle") && i + 1 < argc) g_cycle_ms = atoi(argv[++i]);
    }
    /* Without --dir, ask plugview where to open. It knows the checkout's own
     * corpora and the system's VST directories, and it only ever names one
     * that exists -- where this used to build a path out of the executable's
     * location unconditionally and scan it whether or not it was there. From
     * an installed copy that path was /windows/VST2-64, so the window opened
     * on nothing and said so about a directory nobody had asked for. */
    if (base) snprintf(dir, sizeof dir, "%s", base);
    else      snprintf(dir, sizeof dir, "%s", plugview_default_dir());

    pw_init(&argc, &argv);
    {   /* DW_PERIOD / DW_LATENCY: trade latency against underruns */
        const char *e;
        if ((e = getenv("DW_PERIOD"))) {
            int v = atoi(e);
            if (v >= 64 && v <= PERIOD_MAX) g_period = v;
        }
        if ((e = getenv("DW_LATENCY"))) {
            int v = atoi(e);
            if (v >= 5 && v <= 500) g_latency_us = v * 1000;
        }
        if ((e = getenv("DW_BACKEND"))) g_backend_want = e;
    }
    g_eng.gain = 1.0;
    scan(dir);
    plugview_scan(dir);
    add_juno();
    add_drumkits();

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            for (i = 0; i < g_ninst; i++)
                printf("%-14s %s  %d bank(s)  %s\n", g_inst[i].name,
                       g_inst[i].wavedst ? "wavetable" : "         ",
                       g_inst[i].nbanks,
                       g_inst[i].eng == ENG_DW ? "[DW-8000]"
                         : g_inst[i].eng == ENG_FM ? "[4-op FM]"
                         : g_inst[i].eng == ENG_JUNO ? "[Juno-6]"
                         : g_inst[i].eng == ENG_DRUM ? "[samples]" : "");
            printf("\n%d instruments\n", g_ninst);
            return 0;
        }
    }

    app = gtk_application_new("de.fullbucket.dwstudio", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);

    /* Silence both backends before anything else goes away: PipeWire's data
     * loop is still calling render_block() at this point, and letting it run
     * into process teardown renders out of freed engine state. */
    atomic_store_explicit(&g_eng.running, 0, memory_order_release);
    if (g_eng.pcm) { pthread_join(g_eng.thread, NULL); snd_pcm_close(g_eng.pcm); }
    engine_stop_pipewire();
    pw_deinit();
    return status;
}
