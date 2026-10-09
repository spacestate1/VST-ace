/* studiogtk -- the session shell in GTK: one window, a tab per synth plug-in,
 * one tab for the tracker. The GTK counterpart of the Qt studio
 * (session/qt/main.cpp), built the same way: a synth tab is a plugview
 * instance out of gui/'s gtkhost (what dwstudio is one of), the tracker tab
 * is tracker/gtk's trackerview, and this file is only what both ask a frame
 * for -- a menu bar, a status line, tab bookkeeping, the audio they all mix
 * into, and a say over which tab's piano answers the computer keyboard.
 *
 * And the routing, as in the Qt shell: every synth tab is registered with
 * the tracker's engine as an in-process MIDI sink (trk_add_sink), named by
 * its plug-in, so a track can play a tab directly -- the track's destination
 * list shows it as "this window: <name>" beside the ALSA windows. Delivery
 * is sample-accurate: the engine's delivery thread hands each tab's pane
 * wall-clock-stamped blocks (sink_deliver), and the pane re-places every
 * event into its own rendered block on the sample (plugview_inject_midi,
 * drained at the top of plugview_render_io). A tab closing unregisters its
 * sink first -- trk_remove_sink waits out any delivery in flight -- so the
 * tracker never calls into a dead pane.
 *
 * Threading, as dwstudio: the audio thread never takes a lock; the tabs it
 * mixes are changed only while it is parked.
 *
 * Two more things a frame owes several tabs rather than one, as in the Qt
 * shell. A plug-in whose helper died for good -- its pane restarted it three
 * times and gave up -- is marked on its tab by a once-a-second watch, and
 * Synth > Reload Plug-in is the way back; the recoverable deaths never reach
 * here, the pane restarts those itself and says so on its status line. And
 * the whole session -- the open tabs with their plug-ins and sounds, the
 * song, which tracks play which tab -- is one JSON file
 * (session/sessfile.h), written from File > Save Session and read back by
 * File > Open Session or --session. */

#include "plugview.h"
#include "trackerview.h"
#include "trk.h"
#include "pehost.h"
#include "audioout.h"
#include "vstdirs.h"
#include "sessfile.h"
#include "recentfiles.h"
#include "mixbus.h"
#include "reaperlink.h"

#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <math.h>

/* The output stage: a NaN from a plug-in becomes silence rather than a burst,
 * and past 0.95 the level rolls into the rail instead of hitting it -- the
 * same in value and slope, never over 1. */
static inline float out_soft(double v)
{
    double a;
    if (v != v) return 0.0f;
    a = v < 0 ? -v : v;
    if (a > 0.95) {
        a = 0.95 + 0.05 * tanh((a - 0.95) * 20.0);
        v = v < 0 ? -a : a;
    }
    return (float)v;
}


#define SR         48000
/* One PipeWire quantum, as dwstudio: the audio thread cannot get realtime
 * priority here (`ulimit -r` is 0), so the callback runs on PipeWire's own
 * RTKit-granted realtime thread, and a small period is safe there. DW_PERIOD
 * (frames) and DW_LATENCY (ms) tune it, DW_BACKEND / --backend picks. */
#define PERIOD_MAX 4096
#define MAXTABS    16

static int g_period     = 256;
static int g_latency_us = 50000;

/* ------------------------------------------------------------- the tabs */

typedef struct {
    int           used;
    plugview     *pv;
    GtkWidget    *pane;        /* the notebook page: plugview_pane(pv) */
    GtkWidget    *tablabel;    /* its name on the tab */
    int           sink_id;     /* the tracker's handle for it, or -1 */
    char          sink_name[TRK_DEST_LEN];
    unsigned char held[128];   /* computer keys down, released on switch away */
    _Atomic unsigned long callbacks;   /* blocks rendered, for the smoke drive */
    int           dead_marked; /* the tab already says (stopped) */
    unsigned long last_calls;  /* callback count a second ago, for the tooltip */
    GtkWidget    *kbd;         /* the on-screen keyboard under the pane */
    _Atomic unsigned char play[128];   /* notes the tracker has sounding here: set by its delivery thread */
    _Atomic unsigned play_gen;         /* bumped on every change, so the GUI thread knows to redraw */
    unsigned      play_seen;
    _Atomic float tgain;               /* the tracker's fader for a track alone on this synth */
    GtkWidget    *wheel;       /* the pitch wheel left of it */
    GtkWidget    *kbrow;       /* wheel and keys: what View > On-screen keyboard shows */
    int           bend, bend_drag, bend_grab_v;   /* 14-bit, 8192 at rest */
    /* Connect > REAPER: this synth's own MIDI port and, while REAPER is on, its
     * own audio stream, so REAPER can take it on a track of its own. */
    int           mport;       /* its ALSA sequencer port, or -1 */
    struct pw_stream *dstream; /* its own output stream, or NULL: then it is in the shared mix */
    int           dmix;        /* its place on the studio mix bus while it has a stream */
    _Atomic int   direct;      /* the stream carries it: the shared mix leaves it out */
    float         dbuf[PERIOD_MAX * 2];
} synctab;

static synctab      g_tabs[MAXTABS];
static trk_engine  *g_trk;
static trk_view    *g_tracker;
static GtkWidget   *g_tracker_page;
/* The menu bar follows the tab in front -- see sync_menus. */
static GMenu       *g_bar, *g_file_m, *g_synth_m, *g_samples_m, *g_view_m, *g_help_m, *g_help_trk;
static GMenu       *g_connect_m;       /* Connect > REAPER, shown while a synth is loaded */
static int          g_connect_shown;
static void sync_menus(GtkWidget *front);
static synctab     *g_front;           /* the synth tab the keys play, if one is */

static GtkWidget   *g_win, *g_notebook, *g_stack, *g_hint, *g_status;
static GSimpleAction *g_new_tracker_act;
static int          g_routed;          /* --route: the tracker drives the proof */
static int          g_smoke_ms;
static char         g_session_path[4096];   /* the file Save Session writes */
static char         g_save_on_exit[4096];   /* --save-session */

static void status(const char *msg)
{
    if (GTK_IS_LABEL(g_status)) gtk_label_set_text(GTK_LABEL(g_status), msg);
}

/* ------------------------------------------------------- the loading bar
 *
 * Loading a plug-in blocks the main thread (a Windows plug-in can take many
 * seconds), so the window used to sit unpainted and read as frozen. A small
 * modal window with a pulsing bar is put up around the load. The bar only
 * moves when the main context is run, so the pehost input pump -- called from
 * inside the load -- is pointed at it for the duration. The periodic timers
 * stand down while g_loading is set: they must not touch a tab that is half
 * loaded. */
static int         g_loading;
static GtkWidget  *g_busy_win, *g_busy_bar;
static gint64      g_busy_last;

static void busy_flush(void)
{
    int n;
    for (n = 0; n < 20 && g_main_context_iteration(NULL, FALSE); n++) ;
}

static void busy_pump(void *ud)
{
    gint64 now = g_get_monotonic_time();
    (void)ud;
    if (!g_busy_bar || now - g_busy_last < 40000) return;
    g_busy_last = now;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(g_busy_bar));
    busy_flush();
}

static int load_busy(plugview *pv, const char *path)
{
    const char *base = strrchr(path, '/');
    char text[1200];
    GtkWidget *box, *lab;
    int ok;

    if (g_loading || !GTK_IS_WINDOW(g_win)) return plugview_load_path(pv, path);
    snprintf(text, sizeof text, "Loading %s ...", base ? base + 1 : path);
    status(text);
    g_busy_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(g_busy_win), "Loading");
    gtk_window_set_transient_for(GTK_WINDOW(g_busy_win), GTK_WINDOW(g_win));
    gtk_window_set_modal(GTK_WINDOW(g_busy_win), TRUE);
    gtk_window_set_deletable(GTK_WINDOW(g_busy_win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(g_busy_win), FALSE);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 20); gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 16);   gtk_widget_set_margin_bottom(box, 16);
    lab = gtk_label_new(text);
    g_busy_bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(g_busy_bar, 320, -1);
    gtk_box_append(GTK_BOX(box), lab);
    gtk_box_append(GTK_BOX(box), g_busy_bar);
    gtk_window_set_child(GTK_WINDOW(g_busy_win), box);
    gtk_window_present(GTK_WINDOW(g_busy_win));
    g_loading = 1;
    pehost_set_input_pump(busy_pump, NULL);
    g_busy_last = 0;
    busy_pump(NULL);

    ok = plugview_load_path(pv, path);

    pehost_set_input_pump(NULL, NULL);
    g_loading = 0;
    gtk_window_destroy(GTK_WINDOW(g_busy_win));
    g_busy_win = g_busy_bar = NULL;
    busy_flush();
    return ok;
}

/* ------------------------------------------------------------- the audio
 *
 * One stream, every tab mixed into it. dwstudio's arrangement, minus its
 * engines: the plug-ins do their own event handling through pehost's queues,
 * so the callback only renders and sums. */

static ao *g_ao;               /* the JACK or ALSA backend, when that is what runs */
static _Atomic int g_parked, g_park_req;
static _Atomic int g_direct_busy;     /* tabs' own streams inside a render right now */
static struct pw_thread_loop *g_pw_loop;
static struct pw_stream      *g_pw_stream;
static double    *g_pw_buf;
static const char *g_backend = "none";
static char        g_backend_buf[200];       /* g_backend, when it is a description */
static const char *g_backend_want = "auto";
static int         g_backend_cli;            /* --backend was given: it wins over the saved choice */
static char        g_audio_note[300];        /* what the last start or switch said */
static double      g_ao_buf[PERIOD_MAX * 2]; /* the realtime scratch for the ao backends */
static __thread int g_teb_ready;

/* Scratch for one tab's block, mixed into the output at once. Sized once so
 * the audio thread never allocates. */
static float g_plug_buf[PERIOD_MAX * 2];

/* The studio mix bus (File > Record studio mix): the synth tabs' mix is one
 * source, the tracker's sample tracks another. */
static int g_mix_synth = -1, g_mix_trk = -1;

/* What an effect plug-in processes: the system's capture input, when "Effect
 * input" is on. PipeWire's capture callback writes stereo frames into a ring
 * and render_block takes one block's worth out of it. Without it an effect is
 * handed silence, and an effect given silence produces silence. */
#define CAP_FRAMES 16384                      /* a power of two */
static float            g_cap_ring[CAP_FRAMES * 2];
static _Atomic unsigned g_cap_head, g_cap_tail;
static _Atomic int      g_in_on;
static float            g_in_buf[PERIOD_MAX * 2];

/* One block of input into g_in_buf: what has arrived, silence for what has not.
 * A backlog is dropped down to a block and a half -- taking one block a read and
 * leaving the rest would let the input run further and further behind. */
static void input_block(int n)
{
    unsigned head = atomic_load_explicit(&g_cap_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&g_cap_tail, memory_order_relaxed);
    unsigned avail = head - tail, want = (unsigned)n, i;
    if (avail > want * 3) { tail = head - want * 3 / 2; avail = head - tail; }
    if (avail > want) avail = want;
    for (i = 0; i < avail; i++) {
        g_in_buf[2 * i]     = g_cap_ring[2 * ((tail + i) & (CAP_FRAMES - 1))];
        g_in_buf[2 * i + 1] = g_cap_ring[2 * ((tail + i) & (CAP_FRAMES - 1)) + 1];
    }
    if (avail < want) memset(g_in_buf + 2 * avail, 0, (size_t)(want - avail) * 2 * sizeof(float));
    atomic_store_explicit(&g_cap_tail, tail + avail, memory_order_release);
}

static void render_block(double *buf, int frames)
{
    int i, t, n = frames > PERIOD_MAX ? PERIOD_MAX : frames;
    const float *in = NULL;

    memset(buf, 0, (size_t)n * 2 * sizeof *buf);
    if (atomic_load_explicit(&g_in_on, memory_order_acquire)) { input_block(n); in = g_in_buf; }
    for (t = 0; t < MAXTABS; t++) {
        if (!g_tabs[t].used || atomic_load_explicit(&g_tabs[t].direct, memory_order_acquire) ||
            !plugview_active(g_tabs[t].pv)) continue;
        if (plugview_render_io(g_tabs[t].pv, in, g_plug_buf, n)) {
            const float tg = atomic_load_explicit(&g_tabs[t].tgain, memory_order_relaxed);
            for (i = 0; i < n * 2; i++) buf[i] += g_plug_buf[i] * tg;
            atomic_fetch_add_explicit(&g_tabs[t].callbacks, 1, memory_order_relaxed);
        }
    }
    if (n < frames)
        memset(buf + (size_t)n * 2, 0, (size_t)(frames - n) * 2 * sizeof *buf);
}

/* The ao backends' render function: the same block the PipeWire callback
 * renders, parked the same way, into the float buffer they hand over. */
static void ao_render_cb(void *ud, float *out, int frames)
{
    int i;
    (void)ud;
    if (frames > PERIOD_MAX) frames = PERIOD_MAX;
    if (!g_teb_ready) { pehost_thread_init(); g_teb_ready = 1; }
    if (atomic_load_explicit(&g_park_req, memory_order_acquire)) {
        atomic_store_explicit(&g_parked, 1, memory_order_release);
        memset(out, 0, (size_t)frames * 2 * sizeof *out);
        mixbus_feed_f(g_mix_synth, out, frames);        /* silence, so a recording keeps its length */
        return;
    }
    atomic_store_explicit(&g_parked, 0, memory_order_release);
    render_block(g_ao_buf, frames);
    for (i = 0; i < frames * 2; i++) out[i] = out_soft(g_ao_buf[i]);
    mixbus_feed_f(g_mix_synth, out, frames);
}

static void pw_on_process(void *ud)
{
    struct pw_buffer *b;
    struct spa_buffer *sb;
    float *dst;
    int n, i;
    (void)ud;

    if (!(b = pw_stream_dequeue_buffer(g_pw_stream))) return;
    sb = b->buffer;
    if (!(dst = sb->datas[0].data)) { pw_stream_queue_buffer(g_pw_stream, b); return; }

    n = (int)(sb->datas[0].maxsize / (sizeof(float) * 2));
    if (b->requested && (int)b->requested < n) n = (int)b->requested;
    if (n > PERIOD_MAX) n = PERIOD_MAX;

    if (!g_teb_ready) { pehost_thread_init(); g_teb_ready = 1; }

    if (atomic_load_explicit(&g_park_req, memory_order_acquire)) {
        atomic_store_explicit(&g_parked, 1, memory_order_release);
        memset(dst, 0, (size_t)n * 2 * sizeof *dst);
    } else {
        atomic_store_explicit(&g_parked, 0, memory_order_release);
        render_block(g_pw_buf, n);
        for (i = 0; i < n * 2; i++) {
            double v = g_pw_buf[i];
            dst[i] = out_soft(v);
        }
    }

    mixbus_feed_f(g_mix_synth, dst, n);                  /* what leaves, parked (silence) or not */
    sb->datas[0].chunk->offset = 0;
    sb->datas[0].chunk->stride = sizeof(float) * 2;
    sb->datas[0].chunk->size   = (uint32_t)(n * 2 * sizeof(float));
    pw_stream_queue_buffer(g_pw_stream, b);
}

static const struct pw_stream_events g_pw_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = pw_on_process,
};

static void engine_stop_pipewire(void)
{
    if (g_pw_loop)   pw_thread_loop_stop(g_pw_loop);
    if (g_pw_stream) { pw_stream_destroy(g_pw_stream); g_pw_stream = NULL; }
    if (g_pw_loop)   { pw_thread_loop_destroy(g_pw_loop); g_pw_loop = NULL; }
    free(g_pw_buf); g_pw_buf = NULL;
}

static int engine_start_pipewire(void)
{
    const struct spa_pod *params[1];
    uint8_t pod[1024];
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    char lat[64];
    struct spa_audio_info_raw info;

    if (!(g_pw_buf = malloc((size_t)PERIOD_MAX * 2 * sizeof *g_pw_buf))) return -1;
    if (!(g_pw_loop = pw_thread_loop_new("studiogtk", NULL))) { engine_stop_pipewire(); return -1; }

    snprintf(lat, sizeof lat, "%d/%d", g_period, SR);
    g_pw_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_pw_loop), "studiogtk",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Music",
                          PW_KEY_NODE_LATENCY, lat,
                          NULL),
        &g_pw_events, NULL);
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
    if (pw_thread_loop_start(g_pw_loop) < 0) { engine_stop_pipewire(); return -1; }
    return 0;
}

/* One backend, started. 0, or -1 with the reason in err. */
static int start_backend(int backend, const char *device, char *err, size_t errn)
{
    err[0] = 0;
    if (backend == AO_PIPEWIRE) {
        if (engine_start_pipewire()) { snprintf(err, errn, "PipeWire is not available"); return -1; }
        snprintf(g_backend_buf, sizeof g_backend_buf, "PipeWire, %d-frame quantum (%.1f ms), realtime",
                 g_period, 1000.0 * g_period / SR);
        g_backend = g_backend_buf;
        return 0;
    }
    if (backend == AO_JACK || backend == AO_ALSA) {
        g_ao = ao_open(backend, device, "studiogtk", SR, g_period, ao_render_cb, NULL, err, errn);
        if (!g_ao) return -1;
        snprintf(g_backend_buf, sizeof g_backend_buf, "%s", ao_describe(g_ao));
        g_backend = g_backend_buf;
        return 0;
    }
    snprintf(err, errn, "not a backend");
    return -1;
}

/* Let go of whichever backend runs. After this nothing calls render_block. */
static void engine_stop_audio(void)
{
    ao_close(g_ao);
    g_ao = NULL;
    engine_stop_pipewire();
    g_backend = "none";
}

/* Start what `c` asks for. A named backend that will not start falls back --
 * PipeWire, then ALSA's default -- and says so in g_audio_note, so the sound
 * never simply goes missing because a setting could not be honoured. Automatic
 * is PipeWire first (its callback runs on an RTKit-granted realtime thread),
 * then ALSA. Returns 0 when what runs is what was asked for. */
static int engine_start_with(const ao_choice *c)
{
    char err[200];
    int rc = 0;

    g_audio_note[0] = 0;
    if (c->backend == AO_AUTO) {
        if (start_backend(AO_PIPEWIRE, "", err, sizeof err)) {
            fprintf(stderr, "audio: pipewire unavailable, falling back to ALSA\n");
            if (start_backend(AO_ALSA, c->device, err, sizeof err)) {
                snprintf(g_audio_note, sizeof g_audio_note, "no audio output: %s", err);
                return -1;
            }
        }
    } else if (start_backend(c->backend, c->device, err, sizeof err)) {
        char why[200];
        snprintf(why, sizeof why, "%s", err);
        rc = -1;
        if (!start_backend(AO_PIPEWIRE, "", err, sizeof err) ||
            !start_backend(AO_ALSA, "", err, sizeof err)) {
            snprintf(g_audio_note, sizeof g_audio_note, "%s: %s -- using %s",
                     ao_backend_name(c->backend), why, g_backend);
        } else {
            snprintf(g_audio_note, sizeof g_audio_note, "%s: %s -- and nothing else would start",
                     ao_backend_name(c->backend), why);
        }
        fprintf(stderr, "audio: %s\n", g_audio_note);
        return rc;
    }
    snprintf(g_audio_note, sizeof g_audio_note, "audio: %s", g_backend);
    fprintf(stderr, "%s\n", g_audio_note);
    return rc;
}

static int engine_start_audio(void)
{
    ao_choice c;
    int forced = ao_backend_from_name(g_backend_want);

    if (g_ao || g_pw_stream) return 0;
    ao_choice_load(&c);
    if (g_backend_cli && forced >= 0) c.backend = forced;   /* --backend on the command line wins */
    else if (g_backend_cli) {
        fprintf(stderr, "audio: unknown backend '%s' (want auto, pipewire, jack or alsa) -- using the saved choice\n",
                g_backend_want);
    }
    return engine_start_with(&c);
}

/* Stop the audio callback touching the tabs so the GTK thread can add, load
 * into or free them. Both backends acknowledge through `parked`; which one is
 * live decides whether there is anyone to wait for. */
static void engine_park(void)
{
    int spins;

    if (!g_ao && !g_pw_stream) return;       /* no audio running: nothing to park */

    /* Clear the acknowledgement before asking for it, or a stale `parked` left
     * over from the previous park is mistaken for this one. */
    atomic_store_explicit(&g_parked, 0, memory_order_relaxed);
    atomic_store_explicit(&g_park_req, 1, memory_order_release);

    /* Bounded wait: a suspended PipeWire node or a wedged device never calls
     * back, and spinning forever here would freeze the UI. */
    for (spins = 0; spins < 1000; spins++) {
        if (atomic_load_explicit(&g_parked, memory_order_acquire)) break;
        { struct timespec ts = { 0, 500000 }; nanosleep(&ts, NULL); }
    }
    if (spins == 1000) fprintf(stderr, "audio: park timed out -- callback not running?\n");
    /* The tabs' own streams (Connect > REAPER) take the request too: one that
     * began before it is waited for, one that begins after sees it and plays
     * silence. */
    for (spins = 0; spins < 1000 && atomic_load(&g_direct_busy) > 0; spins++) {
        struct timespec ts = { 0, 500000 };
        nanosleep(&ts, NULL);
    }
}

static void engine_unpark(void)
{
    atomic_store_explicit(&g_park_req, 0, memory_order_release);
}

/* ------------------------------------------------------------- the keys */

/* Tracker layout, same as dwstudio's. GTK gives real key-release events, so
 * holding a key sustains. */
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


/* ----------------------------------------------------- on-screen keyboard */

/* A row of keys under each synth tab, the way dwstudio has one: keys keep
 * their size and a wider window shows more of them. Clicking plays the tab's
 * plug-in; keys held from the computer keyboard light up too. View >
 * On-screen keyboard hides or shows it in every tab. */
#define KB_W   24.0
#define KB_LO  36
#define KB_TOP 127
#define KB_H   108                    /* 4.5 white keys, as pestudio's */
#define BEND_CENTRE 8192
#define BEND_MAX    16383

static int g_kbd_on = 1;               /* View > On-screen keyboard */

static int kb_white(int n)
{
    static const int w[12] = { 1,0,1,0,1,1,0,1,0,1,0,1 };
    return w[n % 12];
}

static int kb_hi_for_width(int w)
{
    int whites = (int)((double)w / KB_W), n, seen = 0, hi = KB_LO;
    if (whites < 1) whites = 1;
    for (n = KB_LO; n <= KB_TOP; n++) {
        if (!kb_white(n)) continue;
        seen++;
        hi = n;
        if (seen >= whites) break;
    }
    return hi;
}

/* The keys are centred in the room there is: whatever is left over past the
 * last whole key is shared between the two sides, as pestudio's piano does. */
static double kb_x0(int w, int hi)
{
    int n, whites = 0;
    double used;
    for (n = KB_LO; n <= hi; n++) if (kb_white(n)) whites++;
    used = whites * KB_W;
    return used < w ? (w - used) / 2.0 : 0.0;
}

static int kb_note_at(double x, double y, int w, int h)
{
    int n, i = 0, hi = kb_hi_for_width(w);
    x -= kb_x0(w, hi);
    if (x < 0) return -1;
    for (n = KB_LO; n <= hi; n++) {                      /* black keys sit on top */
        if (!kb_white(n)) continue;
        if (n + 1 <= hi && !kb_white(n + 1) && y < h * 0.62) {
            double bx = i * KB_W + KB_W * 0.68;
            if (x >= bx && x < bx + KB_W * 0.62) return n + 1;
        }
        i++;
    }
    i = (int)(x / KB_W);
    for (n = KB_LO; n <= hi; n++)
        if (kb_white(n) && i-- == 0) return n;
    return -1;
}

static synctab *tab_of_kbd(GtkWidget *w)
{
    int t;
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_tabs[t].kbd == w) return &g_tabs[t];
    return NULL;
}

static const char *kb_name(int n, int with_octave, char *buf, size_t bufn)
{
    static const char *nm[12] = { "C", "C♯", "D", "D♯", "E", "F",
                                  "F♯", "G", "G♯", "A", "A♯", "B" };
    if (with_octave) snprintf(buf, bufn, "%s%d", nm[n % 12], n / 12 - 1);
    else             snprintf(buf, bufn, "%s", nm[n % 12]);
    return buf;
}

/* Centred on the key, sitting on `bottom`. */
/* One layout for every key label, made once and moved onto each draw's cairo
 * context: a keyboard has fifty-odd labels and is redrawn at every key, and
 * building a layout and a font description for each was most of the draw. */
static PangoLayout *g_kb_layout;
static double       g_kb_size;

static void kb_label_free(void)
{
    if (g_kb_layout) { g_object_unref(g_kb_layout); g_kb_layout = NULL; }
}

static void kb_label(cairo_t *cr, const char *text, double cx, double bottom, double size)
{
    PangoLayout *l;
    int tw, th;
    if (!g_kb_layout) { g_kb_layout = pango_cairo_create_layout(cr); g_kb_size = 0; }
    l = g_kb_layout;
    pango_cairo_update_layout(cr, l);
    /* Pango rather than cairo's toy text API, which has no font fallback and
     * draws the sharp sign as a missing-glyph box. */
    if (size != g_kb_size) {
        PangoFontDescription *fd = pango_font_description_from_string("sans");
        pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
        pango_layout_set_font_description(l, fd);
        pango_font_description_free(fd);
        g_kb_size = size;
    }
    pango_layout_set_text(l, text, -1);
    pango_layout_get_pixel_size(l, &tw, &th);
    /* `bottom` is the text's baseline in the old code; the layout's own
     * baseline is what it is aligned to now. */
    cairo_move_to(cr, cx - tw / 2.0, bottom - pango_layout_get_baseline(l) / (double)PANGO_SCALE);
    pango_cairo_show_layout(cr, l);
    (void)th;
}

/* Drawn as pestudio and dwstudio draw it: every white key named, the octave
 * dropped when the key is too narrow to hold it, black keys with the
 * accidental only. */
static void kb_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u)
{
    synctab *tab = tab_of_kbd(GTK_WIDGET(a));
    int n, i = 0, hi = kb_hi_for_width(w);
    const double x0 = kb_x0(w, hi);
    (void)u;
    cairo_set_source_rgb(cr, 0.094, 0.094, 0.110);
    cairo_paint(cr);
    for (n = KB_LO; n <= hi; n++) {                      /* white keys */
        char buf[16];
        double fs = KB_W * 0.30;
        cairo_text_extents_t te;
        if (!kb_white(n)) continue;
        if (tab && (tab->held[n] || atomic_load_explicit(&tab->play[n], memory_order_relaxed)))
                                 cairo_set_source_rgb(cr, 0.47, 0.67, 1.0);
        else                     cairo_set_source_rgb(cr, 0.933, 0.933, 0.941);
        cairo_rectangle(cr, x0 + i * KB_W, 0, KB_W - 1, h);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.235, 0.235, 0.259);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        if (fs < 6) fs = 6;
        if (fs > 10) fs = 10;
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs);
        kb_name(n, 1, buf, sizeof buf);
        cairo_text_extents(cr, buf, &te);
        if (te.width > KB_W - 5) kb_name(n, 0, buf, sizeof buf);
        if (n % 12 == 0) cairo_set_source_rgb(cr, 0.27, 0.27, 0.31);
        else             cairo_set_source_rgb(cr, 0.51, 0.51, 0.55);
        kb_label(cr, buf, x0 + i * KB_W + (KB_W - 1) / 2.0, h - 4.0, fs);
        i++;
    }
    i = 0;
    for (n = KB_LO; n <= hi; n++) {                      /* black keys on top */
        if (!kb_white(n)) continue;
        if (n + 1 <= hi && !kb_white(n + 1)) {
            char buf[16];
            double bw = KB_W * 0.62, fs = bw * 0.46;
            if (tab && (tab->held[n + 1] || atomic_load_explicit(&tab->play[n + 1], memory_order_relaxed)))
                                         cairo_set_source_rgb(cr, 0.275, 0.471, 0.824);
            else                         cairo_set_source_rgb(cr, 0.078, 0.078, 0.094);
            cairo_rectangle(cr, x0 + i * KB_W + KB_W * 0.68, 0, bw, h * 0.62);
            cairo_fill(cr);
            if (fs < 5.5) fs = 5.5;
            if (fs > 9) fs = 9;
            cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
            kb_label(cr, kb_name(n + 1, 0, buf, sizeof buf),
                     x0 + i * KB_W + KB_W * 0.68 + bw / 2.0, h * 0.62 - 4.0, fs);
        }
        i++;
    }
}

static void kb_pressed(GtkGestureClick *g, int np, double x, double y, gpointer u)
{
    synctab *tab = u;
    int n = kb_note_at(x, y, gtk_widget_get_width(tab->kbd), gtk_widget_get_height(tab->kbd));
    (void)g; (void)np;
    if (n < 0 || tab->held[n]) return;
    tab->held[n] = 1;
    plugview_note_on(tab->pv, n, 100);
    gtk_widget_queue_draw(tab->kbd);
}

static void kb_released(GtkGestureClick *g, int np, double x, double y, gpointer u)
{
    synctab *tab = u;
    int n;
    (void)g; (void)np; (void)x; (void)y;
    /* The pointer leaves the key it pressed as often as not: every note this
     * click could have started is released, computer keys included. */
    for (n = 0; n < 128; n++)
        if (tab->held[n]) { tab->held[n] = 0; plugview_note_off(tab->pv, n); }
    gtk_widget_queue_draw(tab->kbd);
}

/* The sprung pitch wheel left of the keys, as pestudio and dwstudio have it.
 * Sprung is the whole of it: a bend left off centre detunes everything played
 * after, so it returns to centre when released and says so. The value stays in
 * MIDI's 14-bit form because bend range is the plug-in's parameter. */
static synctab *tab_of_wheel(GtkWidget *w)
{
    int t;
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_tabs[t].wheel == w) return &g_tabs[t];
    return NULL;
}

static void wheel_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u)
{
    synctab *tab = tab_of_wheel(GTK_WIDGET(a));
    const double label = 12.0;
    double bx = 6.0, by = 4.0, bw = w - 12.0, bh = h - 4.0 - label;
    double off, spacing = 7.0, roll, y, my, cy;
    cairo_pattern_t *lg;
    int bend = tab ? tab->bend : BEND_CENTRE;
    (void)u;
    cairo_set_source_rgb(cr, 0.094, 0.094, 0.110);
    cairo_paint(cr);
    if (bw < 6.0 || bh < 8.0) return;
    off = (double)(bend - BEND_CENTRE) / BEND_CENTRE;          /* -1 .. +1 */

    lg = cairo_pattern_create_linear(bx, 0, bx + bw, 0);       /* a cylinder seen edge on */
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
    /* Ridges roll with the value; two and a half of roll, not three, so full
     * deflection is not back in phase with centre. */
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
    my = by + bh / 2.0 - off * (bh / 2.0 - 4.0);               /* the grip */
    if (bend == BEND_CENTRE) cairo_set_source_rgb(cr, 0.59, 0.59, 0.63);
    else                     cairo_set_source_rgb(cr, 0.47, 0.67, 1.0);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, bx + 1, my);
    cairo_line_to(cr, bx + bw - 1, my);
    cairo_stroke(cr);
    cairo_restore(cr);

    cy = by + bh / 2.0;                                        /* detent marks: where centre is */
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.39);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 1, cy + 0.5);       cairo_line_to(cr, 5, cy + 0.5);
    cairo_move_to(cr, w - 5, cy + 0.5);   cairo_line_to(cr, w - 1, cy + 0.5);
    cairo_stroke(cr);

    {
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 0.51, 0.51, 0.55);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8.0);
        cairo_text_extents(cr, "PITCH", &ext);
        cairo_move_to(cr, (w - ext.width) / 2.0 - ext.x_bearing, h - 3.0);
        cairo_show_text(cr, "PITCH");
    }
}

static void bend_send(synctab *tab)
{
    plugview_bend(tab->pv, tab->bend);
    gtk_widget_queue_draw(tab->wheel);
}

static void bend_recentre(synctab *tab)
{
    tab->bend_drag = 0;
    tab->bend = BEND_CENTRE;
    bend_send(tab);
}

static void wheel_drag_begin(GtkGestureDrag *g, double x, double y, gpointer u)
{ synctab *tab = u; (void)g; (void)x; (void)y; tab->bend_drag = 1; tab->bend_grab_v = tab->bend; }

static void wheel_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer u)
{
    synctab *tab = u;
    double travel = gtk_widget_get_height(tab->wheel) / 2.0 - 6.0;
    int nv;
    (void)g; (void)ox;
    if (!tab->bend_drag) return;
    if (travel < 8.0) travel = 8.0;
    /* Relative to where the wheel was taken hold of, so the way to a small
     * bend is not a large one. */
    nv = tab->bend_grab_v + (int)(-oy / travel * BEND_CENTRE);   /* up is sharp */
    if (nv < 0) nv = 0;
    if (nv > BEND_MAX) nv = BEND_MAX;
    if (nv == tab->bend) return;
    tab->bend = nv;
    bend_send(tab);
}

static void wheel_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer u)
{ (void)g; (void)ox; (void)oy; bend_recentre(u); }

/* The pane with its keyboard under it: the notebook page for a synth tab. The
 * wheel sits left of the keys, in the same row so the two are one height. */
static GtkWidget *kb_wrap(synctab *tab, GtkWidget *pane)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkGesture *click = gtk_gesture_click_new(), *drag = gtk_gesture_drag_new();
    gtk_widget_set_vexpand(pane, TRUE);
    gtk_box_append(GTK_BOX(box), pane);

    tab->bend = BEND_CENTRE;
    tab->wheel = gtk_drawing_area_new();
    gtk_widget_set_size_request(tab->wheel, 40, -1);
    gtk_widget_set_tooltip_text(tab->wheel, "Pitch wheel — drag up or down; springs back to centre");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(tab->wheel), wheel_draw, NULL, NULL);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(wheel_drag_begin),  tab);
    g_signal_connect(drag, "drag-update", G_CALLBACK(wheel_drag_update), tab);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(wheel_drag_end),    tab);
    gtk_widget_add_controller(tab->wheel, GTK_EVENT_CONTROLLER(drag));

    tab->kbd = gtk_drawing_area_new();
    gtk_widget_set_hexpand(tab->kbd, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(tab->kbd), kb_draw, NULL, NULL);
    g_signal_connect(click, "pressed", G_CALLBACK(kb_pressed), tab);
    g_signal_connect(click, "released", G_CALLBACK(kb_released), tab);
    gtk_widget_add_controller(tab->kbd, GTK_EVENT_CONTROLLER(click));

    tab->kbrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(tab->kbrow), tab->wheel);
    gtk_box_append(GTK_BOX(tab->kbrow), tab->kbd);
    gtk_widget_set_size_request(tab->kbrow, -1, KB_H);
    gtk_widget_set_visible(tab->kbrow, g_kbd_on != 0);
    gtk_box_append(GTK_BOX(box), tab->kbrow);
    return box;
}

static synctab *tab_of_page(GtkWidget *page)
{
    int t;
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_tabs[t].pane == page) return &g_tabs[t];
    return NULL;
}

/* Only the tab in front answers the computer keyboard. The controller is on
 * the window, so it sees what the focused widget let past -- a tracker's
 * grid claims its own keys first, and when the tracker is the front tab this
 * is NULL and no synth answers at all. */
static synctab *current_synth_tab(void)
{
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    GtkWidget *page = cur >= 0 ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), cur)
                               : NULL;
    return page ? tab_of_page(page) : NULL;
}

/* ----------------------------------------------------- record studio mix --
 *
 * File > Record studio mix: one WAV of everything the studio plays -- every synth
 * tab, mixed as it goes to the speakers, and the tracker's sample tracks -- from
 * the mix bus (mixbus.c). The synth tabs' own audio is one source and the
 * tracker's another; neither knows of the other, which is why there is a bus.
 * Stopping offers to move the file out of renders/, as the Qt shell does. */
static GSimpleAction *g_mix_act;
static GtkWidget     *g_mix_label;
static guint          g_mix_timer;
static char           g_mix_dir[4096];

static gboolean mix_tick(gpointer u)
{
    double secs;
    char buf[64];
    (void)u;
    if (g_loading) return G_SOURCE_CONTINUE;
    if (!mixbus_active()) { g_mix_timer = 0; return G_SOURCE_REMOVE; }
    secs = (double)mixbus_frames() / 48000.0;
    snprintf(buf, sizeof buf, "● mix  %02d:%02d.%d", (int)secs / 60, (int)secs % 60, (int)(secs * 10) % 10);
    if (g_mix_label) gtk_label_set_text(GTK_LABEL(g_mix_label), buf);
    return G_SOURCE_CONTINUE;
}

static int mix_begin(const char *path)
{
    char msg[4300];
    if (mixbus_start(path, 48000)) {
        snprintf(msg, sizeof msg, "could not open %s for writing", path);
        status(msg);
        return -1;
    }
    if (g_mix_act) g_simple_action_set_state(g_mix_act, g_variant_new_boolean(TRUE));
    if (!g_mix_timer) g_mix_timer = g_timeout_add(200, mix_tick, NULL);
    snprintf(msg, sizeof msg, "recording the studio mix to %s", path);
    status(msg);
    fprintf(stderr, "studiogtk: %s\n", msg);
    return 0;
}

static void mix_saved(GObject *src, GAsyncResult *res, gpointer u)
{
    char *file = u, msg[8300];
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *target = f ? g_file_get_path(f) : NULL;
    if (target) {
        char with_ext[4200], *dir;
        snprintf(with_ext, sizeof with_ext, "%s%s", target,
                 g_str_has_suffix(target, ".wav") || g_str_has_suffix(target, ".WAV") ? "" : ".wav");
        if (strcmp(with_ext, file)) {
            g_remove(with_ext);
            if (g_rename(file, with_ext) == 0) {
                dir = g_path_get_dirname(with_ext);
                snprintf(g_mix_dir, sizeof g_mix_dir, "%s", dir);
                g_free(dir);
                snprintf(msg, sizeof msg, "moved the studio mix to %s", with_ext);
            } else {
                snprintf(msg, sizeof msg, "could not write %s -- left it in %s", with_ext, file);
            }
            status(msg);
        }
    }
    g_free(target);
    if (f) g_object_unref(f);
    g_free(file);
}

/* Ends the take. `ask`: offer to move it -- not on quit, where it stays in renders/. */
static void mix_finish(int ask)
{
    char file[4096], msg[4400];
    double secs;
    uint64_t dropped;
    if (mixbus_stop(file, sizeof file)) return;
    secs = (double)mixbus_frames() / 48000.0;
    dropped = mixbus_dropped();
    if (g_mix_act) g_simple_action_set_state(g_mix_act, g_variant_new_boolean(FALSE));
    if (g_mix_label) gtk_label_set_text(GTK_LABEL(g_mix_label), "");
    snprintf(msg, sizeof msg, "wrote %s -- %d.%d s", file, (int)secs, (int)(secs * 10) % 10);
    if (dropped)
        snprintf(msg + strlen(msg), sizeof msg - strlen(msg), "  (%llu frames dropped -- the disk could not keep up)",
                 (unsigned long long)dropped);
    if (mixbus_failed())
        snprintf(msg + strlen(msg), sizeof msg - strlen(msg), "  (a write failed -- the disk is full? the file is incomplete)");
    status(msg);
    fprintf(stderr, "studiogtk: %s\n", msg);
    if (ask && g_win) {
        GtkFileDialog *d = gtk_file_dialog_new();
        char *base = g_path_get_basename(file), *abs = g_canonicalize_filename(file, NULL), *dir;
        gtk_file_dialog_set_title(d, "Save the studio mix as");
        dir = g_path_get_dirname(abs);
        {
            char *start = g_build_filename(g_mix_dir[0] ? g_mix_dir : dir, base, NULL);
            GFile *gf = g_file_new_for_path(start);
            gtk_file_dialog_set_initial_file(d, gf);
            g_object_unref(gf);
            g_free(start);
        }
        gtk_file_dialog_save(d, GTK_WINDOW(g_win), NULL, mix_saved, g_strdup(abs));
        g_object_unref(d);
        g_free(base); g_free(abs); g_free(dir);
    }
}

static void act_record_mix(GSimpleAction *a, GVariant *v, gpointer u)
{
    (void)a; (void)u;
    if (g_variant_get_boolean(v)) {
        char path[256];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        g_mkdir_with_parents("renders", 0755);
        strftime(path, sizeof path, "renders/mix-%Y%m%d-%H%M%S.wav", &tmv);
        mix_begin(path);              /* sets the state itself, and leaves it off on a failure */
    } else {
        mix_finish(1);
    }
}

/* ------------------------------------------------------- effect input --
 *
 * A PipeWire capture stream of its own, on a thread loop of its own, so it
 * works whichever backend the output uses. Off until asked for (Synth menu >
 * Effect input), so that nothing opens the microphone unprompted. */
static struct pw_thread_loop *g_cap_loop;
static struct pw_stream      *g_cap_stream;

static void cap_on_process(void *ud)
{
    struct pw_buffer *b;
    struct spa_buffer *sb;
    (void)ud;
    if (!(b = pw_stream_dequeue_buffer(g_cap_stream))) return;
    sb = b->buffer;
    if (sb->datas[0].data && sb->datas[0].chunk) {
        const float *src = (const float *)((const char *)sb->datas[0].data + sb->datas[0].chunk->offset);
        unsigned n = sb->datas[0].chunk->size / (sizeof(float) * 2), i;
        unsigned head = atomic_load_explicit(&g_cap_head, memory_order_relaxed);
        unsigned tail = atomic_load_explicit(&g_cap_tail, memory_order_acquire);
        if (n > CAP_FRAMES - (head - tail)) n = CAP_FRAMES - (head - tail);    /* full: drop the rest */
        for (i = 0; i < n; i++) {
            g_cap_ring[2 * ((head + i) & (CAP_FRAMES - 1))]     = src[2 * i];
            g_cap_ring[2 * ((head + i) & (CAP_FRAMES - 1)) + 1] = src[2 * i + 1];
        }
        atomic_store_explicit(&g_cap_head, head + n, memory_order_release);
    }
    pw_stream_queue_buffer(g_cap_stream, b);
}

static const struct pw_stream_events g_cap_events = { PW_VERSION_STREAM_EVENTS, .process = cap_on_process };

static void effect_input_stop(void)
{
    if (g_cap_loop) fprintf(stderr, "studiogtk: effect input off (%u frames captured)\n",
                            atomic_load_explicit(&g_cap_head, memory_order_relaxed));
    atomic_store_explicit(&g_in_on, 0, memory_order_release);
    if (g_cap_loop)   pw_thread_loop_stop(g_cap_loop);
    if (g_cap_stream) { pw_stream_destroy(g_cap_stream); g_cap_stream = NULL; }
    if (g_cap_loop)   { pw_thread_loop_destroy(g_cap_loop); g_cap_loop = NULL; }
}

static int effect_input_start(void)
{
    const struct spa_pod *params[1];
    uint8_t pod[1024];
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    struct spa_audio_info_raw info;
    char lat[64];

    if (g_cap_loop) return 0;
    atomic_store(&g_cap_head, 0);
    atomic_store(&g_cap_tail, 0);
    if (!(g_cap_loop = pw_thread_loop_new("studiogtk input", NULL))) return -1;
    snprintf(lat, sizeof lat, "%d/%d", g_period, SR);
    g_cap_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_cap_loop), "studiogtk input",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_LATENCY, lat, NULL),
        &g_cap_events, NULL);
    if (!g_cap_stream) { effect_input_stop(); return -1; }
    spa_zero(info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = SR;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    params[0] = spa_format_audio_raw_build(&bb, SPA_PARAM_EnumFormat, &info);
    if (pw_stream_connect(g_cap_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0 ||
        pw_thread_loop_start(g_cap_loop) < 0) {
        effect_input_stop();
        return -1;
    }
    atomic_store_explicit(&g_in_on, 1, memory_order_release);
    return 0;
}

static void act_effect_input(GSimpleAction *a, GVariant *v, gpointer u)
{
    (void)u;
    if (g_variant_get_boolean(v)) {
        if (effect_input_start()) {
            status("could not open the audio input -- effects stay silent");
            g_simple_action_set_state(a, g_variant_new_boolean(FALSE));
            return;
        }
        status("effect input on: effects now process the system's audio input");
    } else {
        effect_input_stop();
        status("effect input off");
    }
    g_simple_action_set_state(a, v);
}

/* ---------------------------------------------------------- MIDI input --
 *
 * One ALSA-sequencer port, "studiogtk:in", for a keyboard or a sequencer to be
 * connected to (aconnect, qjackctl, a DAW). What arrives plays the synth tab in
 * front, the same one the computer keyboard plays. A thread of its own reads
 * the port and hands each event to the pane's lock-free queue
 * (plugview_inject_midi), which places it in the next block it renders -- so
 * nothing here touches GTK or waits on the audio.
 *
 * g_midi_mx guards the front tab against the one thing that can go wrong: the
 * tab being closed while an event is on its way to it. */
static pthread_mutex_t g_midi_mx = PTHREAD_MUTEX_INITIALIZER;
static snd_seq_t      *g_mseq;
static pthread_t       g_mthread;
static int             g_mpipe[2] = { -1, -1 };
static int             g_mon;

static int             g_mbase = -1;      /* the "in" port: plays whichever tab is in front */

/* To the tab whose own port `port` is, else -- the "in" port -- the tab in front. */
static void midi_deliver(int port, int st, int d1, int d2)
{
    struct timespec ts;
    double wall;
    synctab *to = NULL;
    int t;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wall = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    pthread_mutex_lock(&g_midi_mx);
    for (t = 0; t < MAXTABS; t++) if (g_tabs[t].used && g_tabs[t].mport == port && port >= 0) to = &g_tabs[t];
    if (!to && port == g_mbase) to = g_front;
    if (to && to->used && to->pv) plugview_inject_midi(to->pv, wall, st, d1, d2);
    pthread_mutex_unlock(&g_midi_mx);
}

/* A port for each synth tab, so something outside -- REAPER -- can play one synth
 * and not whichever is in front. Named "synth NN" by the tab's slot. */
static void midi_tab_port_open(synctab *tab)
{
    char name[16];
    if (!g_mseq) return;
    snprintf(name, sizeof name, "synth %02d", (int)(tab - g_tabs) + 1);
    tab->mport = snd_seq_create_simple_port(g_mseq, name,
                     SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (tab->mport < 0) tab->mport = -1;
}

static void midi_tab_port_close(synctab *tab)
{
    int port;
    pthread_mutex_lock(&g_midi_mx);        /* no event is on its way to the tab by this port after this */
    port = tab->mport;
    tab->mport = -1;
    pthread_mutex_unlock(&g_midi_mx);
    if (g_mseq && port >= 0) snd_seq_delete_simple_port(g_mseq, port);
}

static void *midi_main(void *u)
{
    struct pollfd pfd[8];
    int n;
    (void)u;
    n = snd_seq_poll_descriptors(g_mseq, pfd, 7, POLLIN);
    pfd[n].fd = g_mpipe[0];
    pfd[n].events = POLLIN;
    for (;;) {
        snd_seq_event_t *ev;
        if (poll(pfd, (nfds_t)n + 1, -1) < 0) continue;
        if (pfd[n].revents & POLLIN) break;
        while (snd_seq_event_input(g_mseq, &ev) >= 0) {
            const int ch = ev->data.note.channel & 15;
            switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                if (ev->data.note.velocity) midi_deliver(ev->dest.port, 0x90 | ch, ev->data.note.note, ev->data.note.velocity);
                else                        midi_deliver(ev->dest.port, 0x80 | ch, ev->data.note.note, 0);
                break;
            case SND_SEQ_EVENT_NOTEOFF:   midi_deliver(ev->dest.port, 0x80 | ch, ev->data.note.note, ev->data.note.velocity); break;
            case SND_SEQ_EVENT_KEYPRESS:  midi_deliver(ev->dest.port, 0xA0 | ch, ev->data.note.note, ev->data.note.velocity); break;
            case SND_SEQ_EVENT_CONTROLLER:
                midi_deliver(ev->dest.port, 0xB0 | (ev->data.control.channel & 15), ev->data.control.param & 127,
                             ev->data.control.value & 127);
                break;
            case SND_SEQ_EVENT_PGMCHANGE:
                midi_deliver(ev->dest.port, 0xC0 | (ev->data.control.channel & 15), ev->data.control.value & 127, 0);
                break;
            case SND_SEQ_EVENT_CHANPRESS:
                midi_deliver(ev->dest.port, 0xD0 | (ev->data.control.channel & 15), ev->data.control.value & 127, 0);
                break;
            case SND_SEQ_EVENT_PITCHBEND: {
                int v = ev->data.control.value + 8192;
                if (v < 0) v = 0;
                if (v > 16383) v = 16383;
                midi_deliver(ev->dest.port, 0xE0 | (ev->data.control.channel & 15), v & 127, (v >> 7) & 127);
                break;
            }
            default: break;
            }
        }
    }
    return NULL;
}

static void midi_start(void)
{
    int port;
    if (g_mon) return;
    if (snd_seq_open(&g_mseq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        g_mseq = NULL;
        fprintf(stderr, "studiogtk: no MIDI input (the ALSA sequencer would not open)\n");
        return;
    }
    snd_seq_set_client_name(g_mseq, "studiogtk");
    port = g_mbase = snd_seq_create_simple_port(g_mseq, "in",
               SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
               SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0 || pipe(g_mpipe) != 0 ||
        pthread_create(&g_mthread, NULL, midi_main, NULL) != 0) {
        fprintf(stderr, "studiogtk: no MIDI input (could not set the port up)\n");
        if (g_mpipe[0] >= 0) { close(g_mpipe[0]); close(g_mpipe[1]); g_mpipe[0] = g_mpipe[1] = -1; }
        snd_seq_close(g_mseq);
        g_mseq = NULL;
        return;
    }
    g_mon = 1;
}

static void midi_stop(void)
{
    char b = 1;
    if (!g_mon) return;
    if (write(g_mpipe[1], &b, 1) < 0) { /* the thread is stopped by the close below too */ }
    pthread_join(g_mthread, NULL);
    close(g_mpipe[0]); close(g_mpipe[1]);
    g_mpipe[0] = g_mpipe[1] = -1;
    snd_seq_close(g_mseq);
    g_mseq = NULL;
    g_mon = 0;
}

/* ----------------------------------------------------------- Connect: REAPER --
 *
 * With REAPER on, each loaded synth gets a stream of its own in the PipeWire
 * graph (the shared mix leaves it out) and its own MIDI port, and reaperlink.c
 * links them to REAPER's inputs and outputs as REAPER appears: synth k to
 * REAPER's audio inputs 2k+1 and 2k+2 and the k-th of REAPER's MIDI outputs. The
 * synth tabs' order is the order of the tracks to set up in REAPER. */
static rl                   *g_rl;
static GSimpleAction        *g_reaper_act;
static struct pw_thread_loop *g_dl_loop;           /* the tabs' own streams run on this one */
static char                  g_connect_sig[1024];
static char                  g_connect_info[300];

static void direct_process(void *ud)
{
    synctab *tab = ud;
    struct pw_buffer *b;
    struct spa_buffer *sb;
    float *dst;
    int n, i;

    if (!tab->dstream || !(b = pw_stream_dequeue_buffer(tab->dstream))) return;
    sb = b->buffer;
    if (!(dst = sb->datas[0].data)) { pw_stream_queue_buffer(tab->dstream, b); return; }
    n = (int)(sb->datas[0].maxsize / (sizeof(float) * 2));
    if (b->requested && (int)b->requested < n) n = (int)b->requested;
    if (n > PERIOD_MAX) n = PERIOD_MAX;
    if (!g_teb_ready) { pehost_thread_init(); g_teb_ready = 1; }

    atomic_fetch_add(&g_direct_busy, 1);                 /* before looking at the park request, which is set before it is read */
    if (atomic_load(&g_park_req) || !tab->used || !plugview_active(tab->pv) ||
        !plugview_render_io(tab->pv, NULL, tab->dbuf, n)) {
        memset(dst, 0, (size_t)n * 2 * sizeof *dst);
    } else {
        const float tg = atomic_load_explicit(&tab->tgain, memory_order_relaxed);
        for (i = 0; i < n * 2; i++) dst[i] = out_soft((double)(tab->dbuf[i] * tg));
        atomic_fetch_add_explicit(&tab->callbacks, 1, memory_order_relaxed);
    }
    atomic_fetch_sub(&g_direct_busy, 1);
    mixbus_feed_f(tab->dmix, dst, n);                    /* a recording of the mix still has it */
    sb->datas[0].chunk->offset = 0;
    sb->datas[0].chunk->stride = sizeof(float) * 2;
    sb->datas[0].chunk->size = (uint32_t)(n * 2 * sizeof(float));
    pw_stream_queue_buffer(tab->dstream, b);
}

static const struct pw_stream_events g_direct_events = { PW_VERSION_STREAM_EVENTS, .process = direct_process };

static void direct_node_name(const synctab *tab, char *out, size_t n)
{ snprintf(out, n, "studiogtk synth %02d", (int)(tab - g_tabs) + 1); }

static void tab_direct_stop(synctab *tab)
{
    atomic_store_explicit(&tab->direct, 0, memory_order_release);     /* the shared mix takes it back first */
    if (tab->dstream && g_dl_loop) {
        pw_thread_loop_lock(g_dl_loop);
        pw_stream_destroy(tab->dstream);                              /* returns once its callback is out */
        tab->dstream = NULL;
        pw_thread_loop_unlock(g_dl_loop);
    }
    if (tab->dmix >= 0) { mixbus_release(tab->dmix); tab->dmix = -1; }
}

static int tab_direct_start(synctab *tab)
{
    const struct spa_pod *params[1];
    uint8_t pod[1024];
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    struct spa_audio_info_raw info;
    char lat[64], name[48];

    if (tab->dstream) return 0;
    if (!g_dl_loop) {
        if (!(g_dl_loop = pw_thread_loop_new("studiogtk synths", NULL))) return -1;
        if (pw_thread_loop_start(g_dl_loop) < 0) { pw_thread_loop_destroy(g_dl_loop); g_dl_loop = NULL; return -1; }
    }
    direct_node_name(tab, name, sizeof name);
    snprintf(lat, sizeof lat, "%d/%d", g_period, SR);
    pw_thread_loop_lock(g_dl_loop);
    tab->dstream = pw_stream_new_simple(pw_thread_loop_get_loop(g_dl_loop), name,
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_LATENCY, lat, NULL),
        &g_direct_events, tab);
    if (tab->dstream) {
        spa_zero(info);
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = SR;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        params[0] = spa_format_audio_raw_build(&bb, SPA_PARAM_EnumFormat, &info);
        if (pw_stream_connect(tab->dstream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                              PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
            pw_stream_destroy(tab->dstream);
            tab->dstream = NULL;
        }
    }
    pw_thread_loop_unlock(g_dl_loop);
    if (!tab->dstream) return -1;
    tab->dmix = mixbus_register();
    atomic_store_explicit(&tab->direct, 1, memory_order_release);     /* now the shared mix leaves it out */
    return 0;
}

static const char *connect_pref_path(char *buf, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(buf, n, "%s/vst-ace/connect", x);
    else if (h && *h) snprintf(buf, n, "%s/.config/vst-ace/connect", h);
    else return NULL;
    return buf;
}

static int connect_pref_load(void)
{
    char path[1100], line[64];
    FILE *f;
    int on = 0;
    if (!connect_pref_path(path, sizeof path) || !(f = fopen(path, "r"))) return 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "reaper 1", 8)) on = 1;
    fclose(f);
    return on;
}

static void connect_pref_save(int on)
{
    char path[1100], tmp[1120], dir[1100], *slash;
    FILE *f;
    if (!connect_pref_path(path, sizeof path)) return;
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/'))) { *slash = 0; g_mkdir_with_parents(dir, 0700); }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return;
    fprintf(f, "reaper %d\n", on ? 1 : 0);
    if (fclose(f) != 0 || rename(tmp, path) != 0) g_unlink(tmp);
}

/* The info line under the REAPER item, rebuilt when its text changes. */
static void connect_rebuild_menu(void)
{
    GMenuItem *info;
    if (!g_connect_m) return;
    g_menu_remove_all(g_connect_m);
    g_menu_append(g_connect_m, "REAPER", "win.connect-reaper");
    info = g_menu_item_new(g_connect_info, "win.no-connect-info");      /* no such action: shown greyed */
    g_menu_append_item(g_connect_m, info);
    g_object_unref(info);
}

/* Once a second, and when the switch moves: which synths are loaded, in tab
 * order; their own streams while REAPER is on; the linker told when any of that
 * changed; the menu shown while there is a synth to connect. */
static void connect_update(void)
{
    rl_synth rs[MAXTABS];
    synctab *order[MAXTABS];
    char sig[1024] = "", info[300] = "REAPER: off";
    int n = 0, i, on = 0;
    int pages = g_notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) : 0;

    if (g_reaper_act) {
        GVariant *st = g_action_get_state(G_ACTION(g_reaper_act));
        on = st && g_variant_get_boolean(st);
        if (st) g_variant_unref(st);
    }

    for (i = 0; i < pages; i++) {
        synctab *tab = tab_of_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i));
        if (tab && tab->used && plugview_loaded_path(tab->pv)[0] && n < MAXTABS) order[n++] = tab;
    }
    for (i = 0; i < MAXTABS; i++) {            /* every tab: its own stream exactly while REAPER is on and it is loaded */
        synctab *tab = &g_tabs[i];
        int loaded = 0, k;
        if (!tab->used) continue;
        for (k = 0; k < n; k++) if (order[k] == tab) loaded = 1;
        if (on && loaded && !tab->dstream) tab_direct_start(tab);
        else if ((!on || !loaded) && tab->dstream) tab_direct_stop(tab);
    }
    for (i = 0; i < n; i++) {
        char node[48];
        memset(&rs[i], 0, sizeof rs[i]);
        direct_node_name(order[i], node, sizeof node);
        snprintf(rs[i].node, sizeof rs[i].node, "%s", node);
        snprintf(rs[i].alsa_client, sizeof rs[i].alsa_client, "studiogtk");
        snprintf(rs[i].midi_port, sizeof rs[i].midi_port, "synth %02d", (int)(order[i] - g_tabs) + 1);
        snprintf(sig + strlen(sig), sizeof sig - strlen(sig), "|%s", node);
    }
    if (g_rl) {
        char sg[1100];
        snprintf(sg, sizeof sg, "%s%s", on ? "on" : "off", sig);
        if (strcmp(sg, g_connect_sig)) {
            snprintf(g_connect_sig, sizeof g_connect_sig, "%s", sg);
            rl_set(g_rl, rs, n, on);
        }
        rl_status(g_rl, info, sizeof info);
    }
    if (strcmp(info, g_connect_info)) {
        snprintf(g_connect_info, sizeof g_connect_info, "%s", info);
        connect_rebuild_menu();
    }
    if ((n > 0) != g_connect_shown) {
        int cur = g_notebook ? gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook)) : -1;
        g_connect_shown = n > 0;
        sync_menus(cur >= 0 ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), cur) : NULL);
    }
}

static void act_connect_reaper(GSimpleAction *a, GVariant *v, gpointer u)
{
    int on = g_variant_get_boolean(v);
    (void)u;
    if (on && !g_rl && !(g_rl = rl_open())) {
        status("could not connect to PipeWire -- REAPER links need it");
        return;                                  /* the check stays off */
    }
    g_simple_action_set_state(a, v);
    connect_pref_save(on);
    g_connect_sig[0] = 0;                        /* push the new state at the next look */
    connect_update();
}

/* At the end: the tabs' streams and the links go before the tabs are freed. */
static void connect_shutdown(void)
{
    int i;
    if (g_rl) { rl_close(g_rl); g_rl = NULL; }
    for (i = 0; i < MAXTABS; i++) if (g_tabs[i].used && g_tabs[i].dstream) tab_direct_stop(&g_tabs[i]);
    if (g_dl_loop) { pw_thread_loop_stop(g_dl_loop); pw_thread_loop_destroy(g_dl_loop); g_dl_loop = NULL; }
}

static gboolean on_key(GtkEventControllerKey *c, guint kv, guint kc,
                       GdkModifierType st, gpointer u)
{
    synctab *tab = current_synth_tab();
    int n;
    (void)c; (void)kc; (void)u;
    if (!tab) return FALSE;
    n = key_note(kv);
    if (n < 0) return FALSE;
    /* A press carrying Ctrl, Alt or Meta is a command, whether or not anything
     * here claims it: Ctrl+C, Ctrl+V, Ctrl+X all land on note keys, and
     * playing a note at the copy shortcut is not something to do in front of
     * an audience. Presses only -- a release is delivered whatever is held
     * with it, because reaching for a modifier while a note is down must not
     * be what strands that note on. */
    if (st & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK | GDK_SUPER_MASK))
        return FALSE;
    if (!tab->held[n]) {
        tab->held[n] = 1;
        plugview_note_on(tab->pv, n, 100);
        if (tab->kbd) gtk_widget_queue_draw(tab->kbd);
    }
    return TRUE;
}

static void on_key_up(GtkEventControllerKey *c, guint kv, guint kc,
                      GdkModifierType st, gpointer u)
{
    synctab *tab = current_synth_tab();
    int n;
    (void)c; (void)kc; (void)st; (void)u;
    if (!tab) return;
    n = key_note(kv);
    if (n >= 0 && tab->held[n]) {
        tab->held[n] = 0;
        plugview_note_off(tab->pv, n);
        if (tab->kbd) gtk_widget_queue_draw(tab->kbd);
    }
}

/* Everything a tab has sounding from the computer keyboard, up. Called when
 * a tab is switched away from -- the release of a key still down goes to the
 * tab now in front, so without this the note would sound for good -- and
 * when the window goes inactive, from where no key-up will ever arrive. */
static void release_tab(synctab *tab)
{
    int n;
    if (!tab) return;
    for (n = 0; n < 128; n++)
        if (tab->held[n]) { tab->held[n] = 0; plugview_note_off(tab->pv, n); }
    if (tab->kbd) gtk_widget_queue_draw(tab->kbd);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer u)
{
    (void)nb; (void)num; (void)u;
    release_tab(g_front);
    pthread_mutex_lock(&g_midi_mx);
    g_front = tab_of_page(page);
    pthread_mutex_unlock(&g_midi_mx);
    sync_menus(page);       /* the signal comes before the notebook's own current page moves */
}

static void on_win_active(GObject *o, GParamSpec *ps, gpointer u)
{
    (void)ps; (void)u;
    if (!gtk_window_is_active(GTK_WINDOW(o))) release_tab(g_front);
}

/* ------------------------------------------------------------- the sinks
 *
 * Every synth tab is a destination the tracker can play directly, named by
 * its plug-in. Registered as soon as the engine exists -- the engine is
 * opened with the first tracker tab, so tabs from before that are registered
 * when it arrives. */

/* The tracker's delivery into a synth tab: the engine's delivery thread
 * calls this with one block of events; the pane re-places them into its own
 * rendered block by wall-clock time. Never blocks, never calls back into the
 * tracker -- the delivery lock is held while this runs. */
static void sink_gain(void *ud, double g)
{
    synctab *tab = ud;
    atomic_store_explicit(&tab->tgain, (float)g, memory_order_relaxed);
}

static void sink_deliver(void *ud, double wall, const trk_sink_ev *evs, int n)
{
    synctab *tab = ud;
    int i;
    for (i = 0; i < n; i++) {
        const int st = evs[i].status & 0xF0, note = evs[i].d1 & 0x7f;
        plugview_inject_midi(tab->pv, wall + (double)evs[i].frame / TRK_SINK_RATE,
                             evs[i].status, evs[i].d1, evs[i].d2);
        /* The keys of the tab it plays on follow the notes. This is the
         * tracker's delivery thread, so only the atomics change here; the GUI
         * thread redraws when it sees the generation move (kb_poll). */
        if (st == 0x90 && evs[i].d2 > 0)  atomic_store_explicit(&tab->play[note], 1, memory_order_relaxed);
        else if (st == 0x80 || st == 0x90) atomic_store_explicit(&tab->play[note], 0, memory_order_relaxed);
        else if (st == 0xB0 && (evs[i].d1 == 123 || evs[i].d1 == 120)) {
            int k;
            for (k = 0; k < 128; k++) atomic_store_explicit(&tab->play[k], 0, memory_order_relaxed);
        } else continue;
        atomic_fetch_add_explicit(&tab->play_gen, 1, memory_order_release);
    }
}

/* Redraw a tab's keyboard when the tracker has moved a key on it. */
static guint g_kb_poll_id;          /* the poll runs only while a tracker is there to play keys */

static gboolean kb_poll(gpointer u)
{
    int t;
    (void)u;
    if (g_loading) return G_SOURCE_CONTINUE;
    for (t = 0; t < MAXTABS; t++) {
        synctab *tab = &g_tabs[t];
        unsigned g;
        if (!tab->used || !tab->kbd) continue;
        g = atomic_load_explicit(&tab->play_gen, memory_order_acquire);
        if (g != tab->play_seen) { tab->play_seen = g; gtk_widget_queue_draw(tab->kbd); }
    }
    return G_SOURCE_CONTINUE;
}

static void unique_sink_name(const char *base, const synctab *exclude,
                             char *out, size_t n)
{
    char b[TRK_DEST_LEN];
    int k;

    snprintf(b, sizeof b, "%s", base && *base ? base : "synth");
    snprintf(out, n, "%s", b);
    for (k = 2; ; k++) {
        int t, taken = 0;
        for (t = 0; t < MAXTABS; t++)
            if (g_tabs[t].used && &g_tabs[t] != exclude &&
                g_tabs[t].sink_id >= 0 && !strcmp(g_tabs[t].sink_name, out)) {
                taken = 1;
                break;
            }
        if (!taken) return;
        snprintf(out, n, "%s %d", b, k);
    }
}

static void ensure_sink(synctab *tab)
{
    char name[TRK_DEST_LEN];
    int id;

    if (!g_trk || !tab || tab->sink_id >= 0) return;
    unique_sink_name(plugview_loaded_name(tab->pv), tab, name, sizeof name);
    id = trk_add_sink(g_trk, name, &sink_deliver, tab);
    if (id < 0) return;
    tab->sink_id = id;
    trk_sink_set_gain_cb(g_trk, id, &sink_gain);
    trk_sink_name(g_trk, id, tab->sink_name, sizeof tab->sink_name);
}

/* The plug-in loaded, unloaded, or failed to: the tab and the tracker's
 * destination list follow the name. */
static void on_tab_loaded(plugview *pv)
{
    int t;
    synctab *tab = NULL;
    char name[TRK_DEST_LEN];

    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_tabs[t].pv == pv) { tab = &g_tabs[t]; break; }
    if (!tab) return;
    {
        const char *loaded = plugview_loaded_name(pv);
        gtk_label_set_text(GTK_LABEL(tab->tablabel), *loaded ? loaded : "synth");
    }
    if (tab->sink_id < 0) return;
    unique_sink_name(plugview_loaded_name(pv), tab, name, sizeof name);
    if (!strcmp(name, tab->sink_name)) return;
    if (g_trk) {
        trk_sink_rename(g_trk, tab->sink_id, name);
        trk_sink_name(g_trk, tab->sink_id, tab->sink_name, sizeof tab->sink_name);
    } else {
        snprintf(tab->sink_name, sizeof tab->sink_name, "%s", name);
    }
}

/* What the tracker view asks its shell: the destinations, and a pick. */
static int sink_names(void *ud, const char **out, int max)
{
    int t, n = 0;
    (void)ud;
    for (t = 0; t < MAXTABS && n < max; t++)
        if (g_tabs[t].used && g_tabs[t].sink_id >= 0)
            out[n++] = g_tabs[t].sink_name;
    return n;
}

static void sink_picked(void *ud, int track, const char *name)
{
    int t;
    (void)ud;
    if (!g_trk) return;
    if (!name) { trk_route_sink(g_trk, track, -1); return; }
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_tabs[t].sink_id >= 0 &&
            !strcmp(g_tabs[t].sink_name, name)) {
            trk_route_sink(g_trk, track, g_tabs[t].sink_id);
            return;
        }
}

static const trk_view_sinks g_sink_api = { sink_names, sink_picked };

/* ------------------------------------------------------------- the tabs */

static void update_canvas(void)
{
    gtk_stack_set_visible_child(GTK_STACK(g_stack),
        gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) ? g_notebook : g_hint);
    {
        GtkNotebook *nb = GTK_NOTEBOOK(g_notebook);
        int cur = gtk_notebook_get_current_page(nb);
        sync_menus(cur >= 0 ? gtk_notebook_get_nth_page(nb, cur) : NULL);
    }
}

static void close_tab_page(GtkWidget *page);

static void on_tab_close(GtkButton *b, gpointer u)
{
    (void)b;
    close_tab_page(u);
}

/* A tab's label: its name and a close button, as the Qt shell's closable
 * tabs carry. */
static GtkWidget *tab_title_widget(const char *title, GtkWidget *page,
                                   GtkWidget **label_out)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lbl = gtk_label_new(title);
    GtkWidget *btn = gtk_button_new_with_label("✕");
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_widget_set_tooltip_text(btn, "Close this tab");
    gtk_widget_add_css_class(btn, "flat");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_close), page);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), btn);
    if (label_out) *label_out = lbl;
    return box;
}

static synctab *add_synth_tab(void)
{
    int t;
    synctab *tab = NULL;
    GtkWidget *title;

    for (t = 0; t < MAXTABS; t++)
        if (!g_tabs[t].used) { tab = &g_tabs[t]; break; }
    if (!tab) { status("no tab slots left"); return NULL; }

    /* Parked while the slot and the pane change under the audio callback;
     * plugview's own loads park it again inside. */
    engine_park();
    memset(tab, 0, sizeof *tab);
    atomic_store(&tab->tgain, 1.0f);
    tab->used = 1;
    tab->sink_id = -1;
    tab->mport = -1;
    tab->dmix = -1;
    midi_tab_port_open(tab);
    tab->pv = plugview_new(engine_park, engine_unpark, SR, g_period);
    plugview_scan(tab->pv, NULL);
    tab->pane = kb_wrap(tab, plugview_pane(tab->pv));
    engine_unpark();

    plugview_set_note_key(tab->pv, key_note);
    plugview_set_load_hook(tab->pv, on_tab_loaded);

    title = tab_title_widget("synth", tab->pane, &tab->tablabel);
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), tab->pane, title);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook),
        gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), tab->pane));
    ensure_sink(tab);
    update_canvas();
    return tab;
}

static void close_synth_tab(synctab *tab)
{
    /* Unregister first: the tracker's delivery thread may be calling into the
     * pane right now, and trk_remove_sink waits that out -- after it returns,
     * nothing touches the pane again, and tracks routed here fall back to
     * their ALSA windows. */
    if (g_trk && tab->sink_id >= 0) trk_remove_sink(g_trk, tab->sink_id);
    tab->sink_id = -1;
    tab_direct_stop(tab);               /* its own stream, if REAPER has it, before the plug-in goes */
    midi_tab_port_close(tab);
    pthread_mutex_lock(&g_midi_mx);       /* no MIDI event is on its way to this tab after this */
    if (g_front == tab) g_front = NULL;
    pthread_mutex_unlock(&g_midi_mx);
    /* plugview_shutdown closes the plug-in the callback may be rendering out
     * of, so the audio is parked first. Removing the page destroys the pane's
     * widgets afterwards, as the pane expects. */
    engine_park();
    plugview_shutdown(tab->pv);
    gtk_notebook_remove_page(GTK_NOTEBOOK(g_notebook),
        gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), tab->pane));
    plugview_free(tab->pv);
    tab->used = 0;
    engine_unpark();
    update_canvas();
}

/* ------------------------------------------------------------- the tracker */

static void reopen_song_synths(void);
static void remember(const char *kind, const char *path);       /* Open Recent's list */

/* The view's song-opened hook: its Open button and the shell's open song
 * paths alike land here, and every one of them reopens the song's synths. */
static void restore_song_sounds(const char *song);
static void save_song_sounds(const char *song);

static void song_opened_cb(void *ud)
{
    (void)ud;
    reopen_song_synths();
    if (g_tracker && trk_view_path(g_tracker)[0]) {
        restore_song_sounds(trk_view_path(g_tracker));
        remember("song", trk_view_path(g_tracker));      /* the tracker's own Open button too */
    }
}

static void song_saved_cb(const char *path, void *ud)
{
    (void)ud;
    save_song_sounds(path);
    remember("song", path);
}

/* The tracker's sample tracks on the studio mix bus. */
static void tracker_tap(void *ud, const double *x, int frames)
{
    mixbus_feed_d((int)(intptr_t)ud, x, frames);
}

static int open_tracker_tab(void)
{
    char err[256];
    int t;

    if (g_tracker) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook),
            gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), g_tracker_page));
        return 1;
    }
    /* One engine per process, opened on first use: trk_open claims an ALSA
     * sequencer client, and a blank canvas should not be holding one. */
    if (!g_trk && !(g_trk = trk_open(err, sizeof err))) {
        char msg[300];
        snprintf(msg, sizeof msg, "the tracker could not start: %s", err);
        status(msg);
        return 0;
    }
    if (!g_kb_poll_id) g_kb_poll_id = g_timeout_add(40, kb_poll, NULL);   /* keys the tracker is playing */
    if (g_mix_trk < 0) g_mix_trk = mixbus_register();
    trk_set_tap(g_trk, tracker_tap, (void *)(intptr_t)g_mix_trk);
    g_tracker = trk_view_new(g_trk);
    trk_view_set_embedded(g_tracker, 1);      /* the menu bar carries its commands */
    g_tracker_page = trk_view_widget(g_tracker);
    trk_view_set_sinks(g_tracker, &g_sink_api, NULL);
    trk_view_set_song_opened(g_tracker, song_opened_cb, NULL);
    trk_view_set_song_saved(g_tracker, song_saved_cb, NULL);
    trk_view_reset(g_tracker);
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), g_tracker_page,
                             tab_title_widget("tracker", g_tracker_page, NULL));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook),
        gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), g_tracker_page));
    /* The engine did not exist when earlier synth tabs opened; register their
     * destinations now. */
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used) ensure_sink(&g_tabs[t]);
    /* One tracker per process -- the engine behind it is single-instance, so
     * a second tab would only be two faces of one song. */
    if (g_new_tracker_act) g_simple_action_set_enabled(g_new_tracker_act, FALSE);
    update_canvas();
    return 1;
}

/* The song is saved or discarded; the tab can really come down. */
static void tracker_close_ok(void *ud)
{
    int t;
    (void)ud;
    trk_stop(g_trk);                    /* done with the engine: playback stops */
    gtk_notebook_remove_page(GTK_NOTEBOOK(g_notebook),
        gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), g_tracker_page));
    trk_view_free(g_tracker);
    g_tracker = NULL;
    g_tracker_page = NULL;
    if (g_kb_poll_id) { g_source_remove(g_kb_poll_id); g_kb_poll_id = 0; }
    trk_set_tap(g_trk, NULL, NULL);     /* returns once no call is in flight */
    mixbus_release(g_mix_trk);
    g_mix_trk = -1;
    trk_close(g_trk);
    g_trk = NULL;
    for (t = 0; t < MAXTABS; t++)
        g_tabs[t].sink_id = -1;         /* the destinations died with the engine */
    if (g_new_tracker_act) g_simple_action_set_enabled(g_new_tracker_act, TRUE);
    update_canvas();
}

static void close_tracker_tab(void)
{
    /* The same unsaved-changes flow the standalone's window close runs; the
     * answer calls tracker_close_ok, cancel is silence. */
    trk_view_confirm_close(g_tracker, tracker_close_ok, NULL);
}

static void close_tab_page(GtkWidget *page)
{
    synctab *tab;
    if (!page) return;
    if (page == g_tracker_page) { close_tracker_tab(); return; }
    tab = tab_of_page(page);
    if (tab) close_synth_tab(tab);
}

/* ------------------------------------------------- a song's own synths
 *
 * A track routed to a synth tab saves the tab's name with the song ("track N
 * sink <name>"), and loading the song puts the name back into the track --
 * but the tab it named is long gone, so without more the name only sits
 * there and the track keeps its ALSA window. This is the more: each distinct
 * name the song carries is found among the open tabs or, failing that,
 * resolved to a plug-in path out of the same folders the browser scans,
 * loaded in a tab of its own, and routed to. What a tab made this way plays
 * is the plug-in's default program: the sound the track was mixed with lives
 * in the session file, which the song knows nothing of. */

static int is_dir(const char *p)
{ struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

/* Where a name is looked up: the same folders plugview_scan walks, found the
 * same way -- the corpora beside the binary, the system VST directories, and
 * the folders the user set. plugview keeps the scan's list per pane and
 * private to plugview.c, so the lookup walks the same places itself. Nothing
 * is sniffed: the shape of a name (a .dll, a .vst3 bundle) is filter enough
 * when what follows is an exact name match, and a path that will not load is
 * caught by the load. */
static int reopen_add_root(char roots[][1024], int n, int max, const char *path)
{
    /* PATH_MAX, not a size of our choosing, as in plugview: glibc's fortified
     * realpath checks the buffer against it before resolving anything. */
    char real[PATH_MAX];
    int  i;

    if (realpath(path, real)) path = real;
    for (i = 0; i < n; i++) if (!strcmp(roots[i], path)) return n;
    if (n >= max) return n;
    snprintf(roots[n], 1024, "%s", path);
    return n + 1;
}

static int reopen_roots(char roots[][1024], int max)
{
    /* The built-in corpora, as plugview's roots_discover: found by walking up
     * from the executable. */
    static const char *const corp[] = {
        "windows/VST2-64", "windows/VST3",  "windows/VST2-32",
        "linux/extracted", "macos/classic", "macos/VST2",
        "macos/VST3",      "macos/AU",
    };
    /* The conventional system locations, as roots_add_standard. */
    static const struct { const char *fmt; int home; } std[] = {
        { "%s/.vst", 1 }, { "%s/.vst3", 1 },
        { "/usr/lib/vst", 0 }, { "/usr/lib/vst3", 0 },
        { "/usr/local/lib/vst", 0 }, { "/usr/local/lib/vst3", 0 },
        { "/usr/lib/x86_64-linux-gnu/vst", 0 },
        { "/usr/lib/x86_64-linux-gnu/vst3", 0 },
    };
    char    exe[1024];
    ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
    int     n = 0, i;

    if (r > 0) {
        int up;
        exe[r] = 0;
        for (up = 0; up < 6; up++) {
            char *slash = strrchr(exe, '/');
            if (!slash) break;
            *slash = 0;
            if (!exe[0]) break;
            for (i = 0; i < (int)(sizeof corp / sizeof corp[0]); i++) {
                char path[1024];
                snprintf(path, sizeof path, "%s/%s", exe, corp[i]);
                if (is_dir(path)) n = reopen_add_root(roots, n, max, path);
            }
        }
    }
    for (i = 0; i < (int)(sizeof std / sizeof std[0]); i++) {
        char path[1024];
        if (std[i].home) {
            const char *home = getenv("HOME");
            if (!home || !*home) continue;
            snprintf(path, sizeof path, std[i].fmt, home);
        } else {
            snprintf(path, sizeof path, "%s", std[i].fmt);
        }
        if (is_dir(path)) n = reopen_add_root(roots, n, max, path);
    }
    /* VST_PATH and VST3_PATH are colon-separated, like PATH. */
    {
        static const char *const vars[] = { "VST_PATH", "VST3_PATH", NULL };
        int v;
        for (v = 0; vars[v]; v++) {
            const char *e = getenv(vars[v]), *p;
            if (!e || !*e) continue;
            for (p = e; *p; ) {
                const char *sep = strchr(p, ':');
                size_t len = sep ? (size_t)(sep - p) : strlen(p);
                char path[1024];
                if (len && len < sizeof path) {
                    memcpy(path, p, len);
                    path[len] = 0;
                    if (is_dir(path)) n = reopen_add_root(roots, n, max, path);
                }
                if (!sep) break;
                p = sep + 1;
            }
        }
    }
    /* The folders the user set, from the file both windows share -- see
     * vstdirs.h. */
    {
        vstdir dirs[VSTDIRS_MAX];
        int nd = vstdirs_load(dirs, VSTDIRS_MAX);
        for (i = 0; i < nd; i++)
            if (is_dir(dirs[i].path)) n = reopen_add_root(roots, n, max, dirs[i].path);
    }
    return n;
}

/* The shapes plugview's scan lists, judged by name alone -- the content
 * checks it also runs are for building a list to browse; here the name match
 * below decides, and a false positive is caught by the load it leads to.
 * Classic plug-ins carry no extension convention, so they are not matched
 * this way at all: one of those is a "not found", not a guess. */
static int plug_shape(const char *path, const char *nm, int isdir)
{
    size_t l = strlen(nm);

    if (l > 5 && !g_ascii_strcasecmp(nm + l - 5, ".vst3")) return 1;
    if (isdir)
        return (l > 4  && !g_ascii_strcasecmp(nm + l - 4,  ".vst")) ||
               (l > 10 && !g_ascii_strcasecmp(nm + l - 10, ".component"));
    if (l > 4 && !g_ascii_strcasecmp(nm + l - 4, ".dll")) return 1;
    if (l > 3 && !g_ascii_strcasecmp(nm + l - 3, ".so")) return !strstr(path, ".lv2/");
    return 0;
}

/* Whether a scanned file is the plug-in a saved name asks for, the strict
 * stages: the scan lists a plug-in under its file name while the song saved
 * the name it reports, and for everything that names its file after itself
 * the two differ only by the extension -- "Surge XT.vst3" is "Surge XT" --
 * so the stem is compared beside the whole name, exactly first and then
 * without case. The looser stages live in plug_near; nothing here places is
 * guessed at, it falls through to them. */
static int plug_named(const char *nm, const char *want, int ci)
{
    char stem[1024];
    char *dot;

    snprintf(stem, sizeof stem, "%s", nm);
    if ((dot = strrchr(stem, '.')) && dot != stem) *dot = 0;
    if (ci)
        return !g_ascii_strcasecmp(nm, want) || !g_ascii_strcasecmp(stem, want);
    return !strcmp(nm, want) || !strcmp(stem, want);
}

/* The name with its punctuation and case off, for the looser stages:
 * "FB-7999" and "fb799964" have to meet. */
static void bare_name(const char *s, char *out, size_t n)
{
    size_t w = 0;

    for (; *s && w + 1 < n; s++)
        if (g_ascii_isalnum(*s)) out[w++] = (char)g_ascii_tolower(*s);
    out[w] = 0;
}

/* What one walk collects while looking for an exact match, in the order the
 * stages are preferred: the first case-insensitive hit; the first
 * punctuation-stripped one ("FB-7999" is fb7999.vst3); the shortest stem
 * that is the name plus a tag of trailing digits ("blooo" is blooo64.dll);
 * and the shortest that is the name behind a vendor's tag up front ("FM8" is
 * NI FM8.dll). The shortest spelling wins as the least decorated -- the
 * plug-in itself rather than its sibling editions. The same stages in the
 * same order as the Qt shell's resolvePlugin, so a song resolves to the same
 * plug-in in either window. */
typedef struct {
    const char *want;                /* the base name being resolved */
    char        wbare[TRK_DEST_LEN]; /* it, punctuation-stripped */
    char        ci[1024];            /* each stage's hit path, "" while none */
    char        bare[1024];
    char        tag[1024];
    int         tag_len;
    char        pre[1024];
    int         pre_len;
} plug_match;

/* One candidate against the looser stages. */
static void plug_near(plug_match *m, const char *path, const char *nm)
{
    char stem[1024], sbare[1024];
    char *dot;
    int  hl, wl, i;

    if (!m->ci[0] && plug_named(nm, m->want, 1))
        snprintf(m->ci, sizeof m->ci, "%s", path);
    wl = (int)strlen(m->wbare);
    if (!wl) return;
    snprintf(stem, sizeof stem, "%s", nm);
    if ((dot = strrchr(stem, '.')) && dot != stem) *dot = 0;
    bare_name(stem, sbare, sizeof sbare);
    hl = (int)strlen(sbare);
    if (!m->bare[0] && !strcmp(sbare, m->wbare))
        snprintf(m->bare, sizeof m->bare, "%s", path);
    /* The name plus a tag of trailing digits: blooo64.dll is "Blooo". */
    if (hl > wl && !strncmp(sbare, m->wbare, (size_t)wl)) {
        for (i = wl; i < hl && g_ascii_isdigit(sbare[i]); i++) ;
        if (i == hl && (!m->tag[0] || hl < m->tag_len)) {
            snprintf(m->tag, sizeof m->tag, "%s", path);
            m->tag_len = hl;
        }
    }
    /* The name behind a vendor's tag up front: NI FM8.dll is "FM8". Names
     * shorter than three letters are excepted -- too much ends in one. */
    if (wl >= 3 && hl > wl && !strcmp(sbare + hl - wl, m->wbare) &&
        (!m->pre[0] || hl < m->pre_len)) {
        snprintf(m->pre, sizeof m->pre, "%s", path);
        m->pre_len = hl;
    }
}

/* One folder, walked as plugview's scan_tree walks it -- the tree, not just
 * the top, because a native plug-in arrives as its own unpacked release with
 * the bundle several levels down. The first exact match anywhere wins at
 * once; everything else a candidate matches is collected in `m` for the
 * stages to sort out when the whole walk is done. */
static int find_in_tree(const char *dir, plug_match *m, char *out, size_t outn)
{
    char queue[512][1024];
    int  head = 0, tail = 0, visited = 0;

    if (!dir || !*dir) return 0;
    snprintf(queue[tail++], sizeof queue[0], "%s", dir);
    while (head < tail && visited < 512) {
        char        base[1024];
        GDir       *d;
        const char *nm;

        snprintf(base, sizeof base, "%s", queue[head++]);
        visited++;
        if (!(d = g_dir_open(base, 0, NULL))) continue;
        while ((nm = g_dir_read_name(d))) {
            char path[1024];
            int  isdir;

            if (nm[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s", base, nm);
            isdir = is_dir(path);
            if (!plug_shape(path, nm, isdir)) {
                if (isdir && tail < (int)(sizeof queue / sizeof queue[0]))
                    snprintf(queue[tail++], sizeof queue[0], "%s", path);
                continue;
            }
            if (plug_named(nm, m->want, 0)) {
                snprintf(out, outn, "%s", path);
                g_dir_close(d);
                return 1;
            }
            plug_near(m, path, nm);
        }
        g_dir_close(d);
    }
    return 0;
}

/* The name a track saved, to the path of the plug-in that reported it. Every
 * folder the scan would walk is tried, and the stages answer in order:
 * exact, case-insensitive, punctuation-stripped, a tag of trailing digits,
 * a vendor's tag up front. */
static int find_plugin_path(const char *want, char *out, size_t outn)
{
    char roots[32][1024];
    plug_match m;
    int  nroot, i;

    memset(&m, 0, sizeof m);
    m.want = want;
    bare_name(want, m.wbare, sizeof m.wbare);
    nroot = reopen_roots(roots, (int)(sizeof roots / sizeof roots[0]));
    for (i = 0; i < nroot; i++)
        if (find_in_tree(roots[i], &m, out, outn))
            return 1;
    snprintf(out, outn, "%s",
             m.ci[0]   ? m.ci   :
             m.bare[0] ? m.bare :
             m.tag[0]  ? m.tag  : m.pre);
    return out[0] != 0;
}

/* "Blooo 2" back to "Blooo": the suffix unique_sink_name put on when two
 * tabs reported one name. Only a trailing space and digits, and only when
 * something came before them. */
static void sink_base_name(const char *name, char *out, size_t n)
{
    size_t l = strlen(name), end = l;

    snprintf(out, n, "%s", name);
    while (l > 0 && g_ascii_isdigit(name[l - 1])) l--;
    if (l < end && l > 1 && name[l - 1] == ' ') out[l - 1] = 0;
}

/* The song is loaded and its tracks name the synths they were playing. Bring
 * those synths back: one a tab still has by exactly that name is reused, the
 * rest are resolved, loaded in a tab each, and every track that named one is
 * routed to it. A name nothing resolves keeps the track's ALSA fallback,
 * untouched. */
static void reopen_song_synths(void)
{
    char names[TRK_TRACKS][TRK_DEST_LEN];  /* the distinct sink names, first-seen */
    int  tname[TRK_TRACKS];                /* each track's index into names, or -1 */
    int  ids[TRK_TRACKS];                  /* the sink each name resolved to, or -1 */
    char missing[TRK_TRACKS * TRK_DEST_LEN] = "";
    char msg[4200];
    int  nnames = 0, reopened = 0, t, i;

    if (!g_trk) return;
    trk_lock(g_trk);
    for (t = 0; t < TRK_TRACKS; t++) {
        const char *sn = trk_song_of(g_trk)->track[t].sink;
        tname[t] = -1;
        if (!*sn) continue;
        for (i = 0; i < nnames; i++) if (!strcmp(names[i], sn)) break;
        if (i == nnames) snprintf(names[nnames++], sizeof names[0], "%s", sn);
        tname[t] = i;
    }
    trk_unlock(g_trk);
    if (!nnames) return;      /* nothing sink-routed: nothing to do or to say */

    for (i = 0; i < nnames; i++) {
        char base[TRK_DEST_LEN], path[1024];
        synctab *tab = NULL;

        ids[i] = -1;
        /* A tab already playing under exactly this name is the one the song
         * meant -- no second instance of it is opened. Case is given the
         * benefit of the doubt before a second one is, as the Qt shell's
         * resolver does with its tab titles. */
        for (t = 0; t < MAXTABS; t++)
            if (g_tabs[t].used && g_tabs[t].sink_id >= 0 &&
                !strcmp(g_tabs[t].sink_name, names[i])) {
                ids[i] = g_tabs[t].sink_id;
                break;
            }
        for (t = 0; ids[i] < 0 && t < MAXTABS; t++)
            if (g_tabs[t].used && g_tabs[t].sink_id >= 0 &&
                !g_ascii_strcasecmp(g_tabs[t].sink_name, names[i])) {
                ids[i] = g_tabs[t].sink_id;
                break;
            }
        if (ids[i] < 0) {
            sink_base_name(names[i], base, sizeof base);
            if (find_plugin_path(base, path, sizeof path) &&
                (tab = add_synth_tab()) &&
                load_busy(tab->pv, path)) {
                /* The load named the tab's sink after the plug-in
                 * (ensure_sink via add_synth_tab, renamed by on_tab_loaded):
                 * the song's full name when the uniquifier came out the same,
                 * else the base name. The new tab answers first, or a wrong
                 * resolution would capture an older tab's sink; a tab that
                 * matches neither way resolved to the wrong plug-in and does
                 * not stay. */
                if (!strcmp(tab->sink_name, names[i]) || !strcmp(tab->sink_name, base)) {
                    ids[i] = tab->sink_id;
                } else {
                    for (t = 0; t < MAXTABS; t++)
                        if (g_tabs[t].used && g_tabs[t].sink_id >= 0 &&
                            (!strcmp(g_tabs[t].sink_name, names[i]) ||
                             !strcmp(g_tabs[t].sink_name, base))) {
                            ids[i] = g_tabs[t].sink_id;
                            break;
                        }
                }
            }
            if (ids[i] < 0) {
                if (tab) close_synth_tab(tab);
                snprintf(missing + strlen(missing), sizeof missing - strlen(missing),
                         "%s%s", missing[0] ? ", " : "", names[i]);
            }
        }
    }

    /* Only now, with every tab that is coming made: each track that named a
     * synth is routed to its sink. The names without an id keep what the
     * track had. */
    for (t = 0; t < TRK_TRACKS; t++)
        if (tname[t] >= 0 && ids[tname[t]] >= 0)
            trk_route_sink(g_trk, t, ids[tname[t]]);

    /* The tabs opened one in front of the last; the song that asked for them
     * is what was opened, so it is what should be showing. */
    if (g_tracker_page)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook),
            gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), g_tracker_page));

    for (i = 0; i < nnames; i++)
        if (ids[i] >= 0) reopened++;
    snprintf(msg, sizeof msg,
             "reopened %d synth(s) from the song", reopened);
    if (missing[0])
        snprintf(msg + strlen(msg), sizeof msg - strlen(msg),
                 " -- not found: %s", missing);
    status(msg);
}

/* ----------------------------------------------------------------- recent --
 *
 * File > Open Recent: the songs and sessions last opened or saved, from the list
 * the Qt shell keeps too (recentfiles.c). The submenu is rebuilt whenever the
 * list changes. */
static GMenu *g_recent_m;

static void recent_refresh(void)
{
    recent_item *it = g_new(recent_item, RECENT_SHOWN);
    int n, i;
    if (!g_recent_m) { g_free(it); return; }
    g_menu_remove_all(g_recent_m);
    n = recent_list(it, RECENT_SHOWN);
    for (i = 0; i < n; i++) {
        GMenuItem *mi;
        char *base = g_path_get_basename(it[i].path), *dir = g_path_get_dirname(it[i].path);
        char *label = g_strdup_printf("%s%s  —  %s", base, strcmp(it[i].kind, "session") ? "" : " (session)", dir);
        char *target = g_strdup_printf("%s\t%s", it[i].kind, it[i].path);
        mi = g_menu_item_new(label, NULL);
        g_menu_item_set_action_and_target_value(mi, "win.open-recent", g_variant_new_string(target));
        g_menu_append_item(g_recent_m, mi);
        g_object_unref(mi);
        g_free(base); g_free(dir); g_free(label); g_free(target);
    }
    if (n) {
        GMenu *clr = g_menu_new();
        g_menu_append(clr, "Clear Recent", "win.clear-recent");
        g_menu_append_section(g_recent_m, NULL, G_MENU_MODEL(clr));
        g_object_unref(clr);
    } else {
        GMenuItem *none = g_menu_item_new("(nothing yet)", "win.no-recent");
        g_menu_append_item(g_recent_m, none);
        g_object_unref(none);
    }
    g_free(it);
}

static void remember(const char *kind, const char *path)
{
    recent_add(kind, path);
    recent_refresh();
}

static int open_song_path(const char *path)
{
    if (!open_tracker_tab()) return 0;
    if (trk_view_open(g_tracker, path)) {
        char msg[300];
        snprintf(msg, sizeof msg, "could not open %s", path);
        status(msg);
        return 0;
    }
    remember("song", path);
    /* The tracks may name synth tabs they were playing; trk_view_open's
     * song_opened hook (song_opened_cb) brings those back. */
    return 1;
}

/* --route <track>: play the track into the first synth tab, in-process, and
 * start the song. A four-note figure goes into the pattern first so the
 * scripted proof does not depend on the song's contents. */
static int route_track(int track)
{
    int t, i, id = -1;

    if (!g_trk || track < 0 || track >= TRK_TRACKS) return 0;
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_tabs[t].sink_id >= 0) { id = g_tabs[t].sink_id; break; }
    if (id < 0) return 0;
    trk_lock(g_trk);
    {
        trk_song *s = trk_song_of(g_trk);
        if (s->pattern[0].rows < 16) s->pattern[0].rows = 16;
        for (i = 0; i < 4; i++) {
            s->pattern[0].cell[i * 4][track].note = (unsigned char)(60 + i * 4);
            s->pattern[0].cell[i * 4][track].vel = 100;
        }
    }
    trk_unlock(g_trk);
    if (g_tracker) trk_view_mark_clean(g_tracker); /* the figure is the drive's */
    trk_route_sink(g_trk, track, id);
    if (trk_sink_of(g_trk, track) < 0) return 0;
    g_routed = 1;
    trk_play(g_trk, TRK_PLAY_SONG, 0, 0);
    return 1;
}

/* ------------------------------------------------------------- the session
 *
 * The whole session as one JSON file: which synth tabs with which plug-ins
 * and their sounds, the song, which tracks play which tab. Shared with the
 * Qt shell through sessfile.c, so a session saved in one opens in the other. */

static int write_session(const char *path)
{
    sess_file s;
    char err[512], msg[4200];
    int synth_sink[MAXTABS];   /* each recorded synth's tracker destination */
    int t, i;

    memset(&s, 0, sizeof s);
    for (t = 0; t < MAXTABS; t++) {
        sess_synth *sy;
        if (!g_tabs[t].used || !plugview_loaded_path(g_tabs[t].pv)[0]) continue;
        if (!(sy = realloc(s.synths, (size_t)(s.nsynths + 1) * sizeof *sy))) break;
        s.synths = sy;
        synth_sink[s.nsynths] = g_tabs[t].sink_id;
        sy = &s.synths[s.nsynths++];
        /* Absolute, whatever --synth was given as: the file has to mean the
         * same thing from any working directory. */
        sy->plugin = g_canonicalize_filename(plugview_loaded_path(g_tabs[t].pv), NULL);
        sy->patch = plugview_capture_patch(g_tabs[t].pv);
    }
    s.song = g_tracker && trk_view_path(g_tracker)[0]
           ? g_canonicalize_filename(trk_view_path(g_tracker), NULL) : g_strdup("");
    if (g_trk) {
        for (i = 0; i < TRK_TRACKS; i++) {
            char nm[TRK_DEST_LEN] = "";
            sess_route *r;
            int id = trk_sink_of(g_trk, i);
            if (id < 0) continue;
            trk_sink_name(g_trk, id, nm, sizeof nm);
            if (!(r = realloc(s.routes, (size_t)(s.nroutes + 1) * sizeof *r))) break;
            s.routes = r;
            r = &s.routes[s.nroutes++];
            r->track = i;
            r->synth = -1;
            for (t = 0; t < s.nsynths; t++)
                if (synth_sink[t] == id) { r->synth = t; break; }
            r->sink = g_strdup(nm);
        }
    }

    i = sess_write(path, &s, err, sizeof err);
    for (t = 0; t < s.nsynths; t++) { g_free(s.synths[t].plugin); free(s.synths[t].patch); }
    for (t = 0; t < s.nroutes; t++) g_free(s.routes[t].sink);
    free(s.synths);
    free(s.routes);
    g_free(s.song);
    if (i) {
        snprintf(msg, sizeof msg, "could not save the session: %s", err);
        status(msg);
        return 0;
    }
    snprintf(g_session_path, sizeof g_session_path, "%s", path);
    remember("session", path);
    snprintf(msg, sizeof msg, "session saved to %s", path);
    status(msg);
    fprintf(stderr, "session: saved %s\n", path);
    fflush(stderr);
    return 1;
}

/* The sounds a song's synths were on, beside it as <song>.sounds in the
 * session file's own format -- the Qt shell reads and writes the same file.
 * One synth entry (plug-in and patch) for each tab a track plays, and a route
 * naming the track and the tab's sink name. */
static void save_song_sounds(const char *song)
{
    sess_file s;
    char err[512], *out;
    int synth_sink[MAXTABS], t, i;

    if (!g_trk) return;
    memset(&s, 0, sizeof s);
    for (i = 0; i < TRK_TRACKS; i++) {
        char nm[TRK_DEST_LEN] = "";
        sess_route *r;
        int id = trk_sink_of(g_trk, i), at = -1;
        synctab *tab = NULL;
        if (id < 0) continue;
        for (t = 0; t < MAXTABS; t++)
            if (g_tabs[t].used && g_tabs[t].sink_id == id) { tab = &g_tabs[t]; break; }
        if (!tab || !plugview_loaded_path(tab->pv)[0]) continue;
        for (t = 0; t < s.nsynths; t++) if (synth_sink[t] == id) { at = t; break; }
        if (at < 0) {
            sess_synth *sy = realloc(s.synths, (size_t)(s.nsynths + 1) * sizeof *sy);
            if (!sy) break;
            s.synths = sy;
            synth_sink[s.nsynths] = id;
            sy = &s.synths[at = s.nsynths++];
            sy->plugin = g_canonicalize_filename(plugview_loaded_path(tab->pv), NULL);
            sy->patch = plugview_capture_patch(tab->pv);
        }
        trk_sink_name(g_trk, id, nm, sizeof nm);
        if (!(r = realloc(s.routes, (size_t)(s.nroutes + 1) * sizeof *r))) break;
        s.routes = r;
        r = &s.routes[s.nroutes++];
        r->track = i;
        r->synth = at;
        r->sink = g_strdup(nm);
    }
    s.song = g_canonicalize_filename(song, NULL);
    out = g_strconcat(song, ".sounds", NULL);
    if (!s.nsynths) {
        g_unlink(out);                       /* nothing to remember: no stale file */
    } else if (sess_write(out, &s, err, sizeof err)) {
        char msg[700];
        snprintf(msg, sizeof msg, "song saved, but not the synths' sounds: %s", err);
        status(msg);
    }
    g_free(out);
    for (t = 0; t < s.nsynths; t++) { g_free(s.synths[t].plugin); free(s.synths[t].patch); }
    for (t = 0; t < s.nroutes; t++) g_free(s.routes[t].sink);
    free(s.synths);
    free(s.routes);
    g_free(s.song);
}

/* The other half: each synth the song's routes name gets its saved patch,
 * matched by sink name among the tabs there are now. */
static void restore_song_sounds(const char *song)
{
    char err[512], msg[700], bad[512] = "";
    char *in = g_strconcat(song, ".sounds", NULL);
    sess_file *s = sess_read(in, err, sizeof err);
    unsigned done = 0;
    int i, t;

    g_free(in);
    if (!s) return;                          /* no sounds saved with it */
    for (i = 0; i < s->nroutes; i++) {
        const sess_route *r = &s->routes[i];
        if (r->synth < 0 || r->synth >= s->nsynths || r->synth >= 32 ||
            (done & (1u << r->synth)) || !s->synths[r->synth].patch) continue;
        for (t = 0; t < MAXTABS; t++) {
            if (!g_tabs[t].used || g_tabs[t].sink_id < 0 ||
                strcmp(g_tabs[t].sink_name, r->sink ? r->sink : "")) continue;
            /* A tab of that name that now holds a different plug-in would be
             * handed another instrument's parameters: say so, and leave it. */
            {
                const char *want = s->synths[r->synth].plugin;
                char *a = g_path_get_basename(want ? want : ""),
                     *b = g_path_get_basename(plugview_loaded_path(g_tabs[t].pv));
                const int same = !want || !*want || !g_ascii_strcasecmp(a, b);
                g_free(a); g_free(b);
                if (!same) {
                    snprintf(bad + strlen(bad), sizeof bad - strlen(bad), "%s%s (a different plug-in)",
                             bad[0] ? ", " : "", g_tabs[t].sink_name);
                    done |= 1u << r->synth;
                    break;
                }
            }
            if (plugview_apply_patch(g_tabs[t].pv, s->synths[r->synth].patch))
                snprintf(bad + strlen(bad), sizeof bad - strlen(bad), "%s%s",
                         bad[0] ? ", " : "", g_tabs[t].sink_name);
            done |= 1u << r->synth;
            break;
        }
    }
    sess_free(s);
    if (bad[0]) {
        snprintf(msg, sizeof msg, "song's sounds not restored: %s", bad);
        status(msg);
    }
}

/* The song question settled -- saved or discarded, never cancelled -- so the
 * write can go ahead. trk_view_ensure_saved's callback. */
static char g_pending_session[4096];   /* the write waiting on that answer */

static void session_write_confirmed(void *ud)
{
    (void)ud;
    write_session(g_pending_session);
}

/* The write, after the unsaved-song question if there is one. The song is
 * recorded by its path, which a dirty song may not have yet -- Save and
 * Discard both let the write go ahead, Cancel stops it. */
static void save_session_checked(const char *path)
{
    if (g_tracker && trk_view_dirty(g_tracker)) {
        /* Held here rather than handed over as ud: a Cancel never calls
         * back, and there would be nothing to free it. */
        snprintf(g_pending_session, sizeof g_pending_session, "%s", path);
        trk_view_ensure_saved(g_tracker, session_write_confirmed, NULL);
        return;
    }
    write_session(path);
}

/* Every tab, any song question already answered: the synths, then the
 * tracker -- whose close takes the engine and every destination with it. */
static void close_all_tabs_now(void)
{
    int t;
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used) close_synth_tab(&g_tabs[t]);
    if (g_tracker) tracker_close_ok(NULL);
}

/* A question that has to be answered before the next line runs: the dialog
 * is asked and the main loop kept turning until it is. 1 for the second
 * button, 0 for Cancel. */
typedef struct { GMainLoop *loop; int answer; } sync_answer;

static void sync_answered(GObject *src, GAsyncResult *res, gpointer u)
{
    sync_answer *a = u;
    a->answer = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    g_main_loop_quit(a->loop);
}

static int confirm_sync(const char *message, const char *detail, const char *yes)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", message);
    const char *buttons[] = { "Cancel", yes, NULL };
    sync_answer a = { g_main_loop_new(NULL, FALSE), -1 };
    gtk_alert_dialog_set_detail(d, detail);
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 0);
    gtk_alert_dialog_choose(d, GTK_WINDOW(g_win), NULL, sync_answered, &a);
    g_main_loop_run(a.loop);
    g_main_loop_unref(a.loop);
    g_object_unref(d);
    return a.answer == 1;
}

/* Restore, in place of whatever was open: the tabs with their plug-ins and
 * sounds, then the song, then -- last, because they name the tabs -- the
 * routings. Anything that will not come back is said about and skipped, not
 * fatal. Takes `s` and frees it. */
static void restore_session(sess_file *s, const char *path)
{
    char msg[4200], trouble[2048] = "";
    synctab *made[MAXTABS];      /* the tab each saved synth came back as */
    int i, routed = 0;

    /* A session names the plug-ins to load, and loading one runs it: a file
     * somebody sent is not to choose what runs. The ones in folders set up
     * for this program go ahead; any other is asked about first. */
    if (!g_getenv("STUDIO_TRUST_SESSIONS")) {
        GString *list = g_string_new(NULL);
        int unknown = 0;
        for (i = 0; i < s->nsynths && i < MAXTABS; i++) {
            const char *pl = s->synths[i].plugin;
            if (pl && *pl && !vstdirs_contains(pl)) {
                g_string_append_printf(list, "%s\n", pl);
                unknown++;
            }
        }
        if (unknown) {
            char head[160];
            snprintf(head, sizeof head, "This session loads %d plug-in%s from outside the folders set up for studiogtk",
                     unknown, unknown > 1 ? "s" : "");
            g_string_append(list, "\nLoading a plug-in runs it.");
            if (!confirm_sync(head, list->str, unknown > 1 ? "Load them" : "Load it")) {
                g_string_free(list, TRUE);
                sess_free(s);
                status("session not opened -- its plug-ins were not approved");
                return;
            }
        }
        g_string_free(list, TRUE);
    }

    close_all_tabs_now();
    g_session_path[0] = 0;
    for (i = 0; i < MAXTABS; i++) made[i] = NULL;
    for (i = 0; i < s->nsynths && i < MAXTABS; i++) {
        synctab *tab;
        if (!s->synths[i].plugin || !*s->synths[i].plugin) continue;
        if (!(tab = add_synth_tab())) break;
        if (!load_busy(tab->pv, s->synths[i].plugin)) {
            const char *base = strrchr(s->synths[i].plugin, '/');
            /* No empty tab left standing for it: the message says what did
             * not come back, and an empty tab would be saved over the session
             * as one that never had anything in it. */
            close_synth_tab(tab);
            snprintf(trouble + strlen(trouble), sizeof trouble - strlen(trouble),
                     "%s%s", trouble[0] ? ", " : "", base ? base + 1 : s->synths[i].plugin);
            continue;
        }
        made[i] = tab;
        if (s->synths[i].patch && plugview_apply_patch(tab->pv, s->synths[i].patch)) {
            const char *base = strrchr(s->synths[i].plugin, '/');
            snprintf(trouble + strlen(trouble), sizeof trouble - strlen(trouble),
                     "%s%s (its sound)", trouble[0] ? ", " : "",
                     base ? base + 1 : s->synths[i].plugin);
        }
    }
    if (s->song && *s->song) {
        if (!open_song_path(s->song)) {
            snprintf(trouble + strlen(trouble), sizeof trouble - strlen(trouble),
                     "%s%s", trouble[0] ? ", " : "", s->song);
        }
    } else if (s->nroutes) {
        open_tracker_tab();              /* the routes need somewhere to point */
    }
    /* Last, now that the tabs exist to be found: by the saved synth's index,
     * mapped to the tab this restore made for it, so a tab that was already
     * open -- or a second instance of the same plug-in -- cannot capture the
     * route. A file without indices falls back to the name, among this
     * restore's tabs only. A route whose tab failed to load is dropped. */
    for (i = 0; i < s->nroutes; i++) {
        const sess_route *r = &s->routes[i];
        synctab *to = NULL;
        int j;
        if (g_trk && r->track >= 0 && r->track < TRK_TRACKS) {
            if (r->synth >= 0 && r->synth < s->nsynths && r->synth < MAXTABS)
                to = made[r->synth];
            else if (r->synth < 0 && r->sink)
                for (j = 0; j < s->nsynths && j < MAXTABS; j++)
                    if (made[j] && !strcmp(made[j]->sink_name, r->sink)) {
                        to = made[j];
                        break;
                    }
        }
        if (!to || to->sink_id < 0) {
            snprintf(trouble + strlen(trouble), sizeof trouble - strlen(trouble),
                     "%strack %d's route to %s", trouble[0] ? ", " : "", r->track + 1,
                     r->sink && *r->sink ? r->sink : "a synth");
            continue;
        }
        trk_route_sink(g_trk, r->track, to->sink_id);
        routed++;
    }
    snprintf(g_session_path, sizeof g_session_path, "%s", path);
    snprintf(msg, sizeof msg, "session restored from %s%s%s", path,
             trouble[0] ? "; could not bring back: " : "", trouble);
    status(msg);
    fprintf(stderr, "session: restored %s -- %d tab(s), %d route(s)%s\n",
            path, s->nsynths, routed, trouble[0] ? " (with losses)" : "");
    fflush(stderr);
    sess_free(s);
}

/* A restore waiting on the unsaved-song question. Held here rather than
 * handed over as ud: a Cancel never calls back, and there would be nothing to
 * free it -- the next restore frees it instead. */
static sess_file *g_pending_restore;
static char       g_pending_restore_path[4096];

static gboolean restore_pending(gpointer u)
{
    sess_file *s = g_pending_restore;
    (void)u;
    if (g_loading) return G_SOURCE_CONTINUE;
    g_pending_restore = NULL;
    if (s) restore_session(s, g_pending_restore_path);
    return G_SOURCE_REMOVE;
}

/* trk_view_ensure_saved's callback. The restore frees the tracker view, and
 * this runs inside that view's own dialog handling -- so it goes on from the
 * main loop, once the view is out of its call stack. */
static void restore_confirmed(void *ud)
{
    (void)ud;
    g_idle_add(restore_pending, NULL);
}

/* Open a session: read first, so a file that will not parse costs nothing;
 * then, when the song has unsaved changes, the tracker's Save/Discard/Cancel
 * -- Cancel leaves everything as it was -- and only then the restore. */
static int open_session_path(const char *path)
{
    sess_file *s;
    char err[512], msg[700];

    if (!(s = sess_read(path, err, sizeof err))) {
        snprintf(msg, sizeof msg, "could not open the session: %s", err);
        status(msg);
        fprintf(stderr, "session: %s\n", msg);
        fflush(stderr);
        return 0;
    }
    remember("session", path);
    if (g_tracker && trk_view_dirty(g_tracker)) {
        sess_free(g_pending_restore);
        g_pending_restore = s;
        snprintf(g_pending_restore_path, sizeof g_pending_restore_path, "%s", path);
        trk_view_ensure_saved(g_tracker, restore_confirmed, NULL);
        return 1;
    }
    restore_session(s, path);
    return 1;
}

/* Once a second: the tab-face bookkeeping nothing else owns. A tab whose
 * plug-in's helper died for good is marked on its tab (the recoverable
 * deaths are the pane's own business -- it restarts them and says so on its
 * status line), and every tab's tooltip says what its audio is doing, from
 * the callback counter against the ~187.5 blocks a second a 256-frame
 * quantum at 48 kHz should be making. */
static void engine_restart_note(const ao_choice *c);

static gboolean watch_tabs(gpointer u)
{
    int t;
    static int gone;
    (void)u;
    if (g_loading) return G_SOURCE_CONTINUE;
    connect_update();
    /* A JACK server that went away, or a device that cannot be recovered: three
     * looks in a row (an underrun the thread recovers from is not this), then
     * the audio is brought up somewhere else rather than left silent. */
    if (g_ao && !ao_alive(g_ao)) {
        if (++gone >= 3) {
            ao_choice c = { AO_AUTO, "" };
            gone = 0;
            engine_restart_note(&c);
        }
    } else {
        gone = 0;
    }
    for (t = 0; t < MAXTABS; t++) {
        synctab *tab = &g_tabs[t];
        unsigned long calls, rate;
        char tip[160];
        int dead;
        if (!tab->used) continue;
        dead = plugview_dead(tab->pv);
        if (dead && !tab->dead_marked) {
            char lbl[TRK_DEST_LEN + 16], msg[TRK_DEST_LEN + 128];
            tab->dead_marked = 1;
            snprintf(lbl, sizeof lbl, "%s (stopped)", plugview_loaded_name(tab->pv));
            gtk_label_set_text(GTK_LABEL(tab->tablabel), lbl);
            snprintf(msg, sizeof msg,
                     "the plug-in in tab \"%s\" stopped responding and would "
                     "not restart -- Synth > Reload Plug-in tries again",
                     plugview_loaded_name(tab->pv));
            status(msg);
        } else if (!dead && tab->dead_marked) {
            const char *loaded = plugview_loaded_name(tab->pv);
            tab->dead_marked = 0;
            gtk_label_set_text(GTK_LABEL(tab->tablabel), *loaded ? loaded : "synth");
        }
        calls = atomic_load_explicit(&tab->callbacks, memory_order_relaxed);
        rate = calls - tab->last_calls;
        tab->last_calls = calls;
        snprintf(tip, sizeof tip, "audio: %lu blocks/s (%d%%)%s", rate,
                 (int)(rate * 100.0 / 187.5 + 0.5),
                 dead ? " -- plug-in stopped" : "");
        gtk_widget_set_tooltip_text(tab->tablabel, tip);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------- the menus */

static void act_new_synth(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a; (void)p; (void)u; add_synth_tab(); }

static void act_new_tracker(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a; (void)p; (void)u; open_tracker_tab(); }

static void act_close_tab(GSimpleAction *a, GVariant *p, gpointer u)
{
    int cur;
    (void)a; (void)p; (void)u;
    cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    if (cur >= 0)
        close_tab_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), cur));
}

static void on_song_opened(GObject *src, GAsyncResult *res, gpointer u)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    (void)u;
    if (!f) return;
    if ((path = g_file_get_path(f))) {
        open_song_path(path);
        g_free(path);
    }
    g_object_unref(f);
}

static void act_open_recent(GSimpleAction *a, GVariant *p, gpointer u)
{
    const char *v = p ? g_variant_get_string(p, NULL) : NULL;
    const char *tab = v ? strchr(v, '\t') : NULL;
    (void)a; (void)u;
    if (!tab) return;
    if (!strncmp(v, "session\t", 8)) open_session_path(tab + 1);
    else open_song_path(tab + 1);
}

static void act_clear_recent(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p; (void)u;
    recent_clear();
    recent_refresh();
}

static void act_open_song(GSimpleAction *a, GVariant *p, gpointer u)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *ft = gtk_file_filter_new();
    GListStore *fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)a; (void)p; (void)u;
    gtk_file_dialog_set_title(d, "Open song");
    gtk_file_filter_set_name(ft, "Tracker songs");
    gtk_file_filter_add_pattern(ft, "*.trk");
    g_list_store_append(fs, ft);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    g_object_unref(ft);
    g_object_unref(fs);
    gtk_file_dialog_open(d, GTK_WINDOW(g_win), NULL, on_song_opened, NULL);
    g_object_unref(d);
}

/* A new song is worth opening the tracker tab for, exactly as Open song
 * does -- the tab comes up (or comes to the front) and the view's own New
 * flow runs, unsaved-changes question included. */
static void act_new_song(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p; (void)u;
    if (open_tracker_tab()) trk_view_new_song(g_tracker);
}

/* Saving, unlike New, does not create the tab: with no tracker there is no
 * song to save, so the menu answers the way the Synth menu answers when the
 * front tab is not a synth. */
static trk_view *tracker_or_status(void)
{
    if (!g_tracker) status("there is no tracker tab");
    return g_tracker;
}

static void act_save_song(GSimpleAction *a, GVariant *p, gpointer u)
{
    trk_view *v;
    (void)a; (void)p; (void)u;
    if ((v = tracker_or_status())) trk_view_save(v);
}

static void act_save_song_as(GSimpleAction *a, GVariant *p, gpointer u)
{
    trk_view *v;
    (void)a; (void)p; (void)u;
    if ((v = tracker_or_status())) trk_view_save_as(v);
}

static void act_export_midi(GSimpleAction *a, GVariant *p, gpointer u)
{
    trk_view *v;
    (void)a; (void)p; (void)u;
    if ((v = tracker_or_status())) trk_view_export_midi(v);
}

static void act_export_take(GSimpleAction *a, GVariant *p, gpointer u)
{ trk_view *v; (void)a; (void)p; (void)u; if ((v = tracker_or_status())) trk_view_export_take(v); }
static void act_trk_load_samples(GSimpleAction *a, GVariant *p, gpointer u)
{ trk_view *v; (void)a; (void)p; (void)u; if ((v = tracker_or_status())) trk_view_load_samples(v); }
static void act_trk_edit_samples(GSimpleAction *a, GVariant *p, gpointer u)
{ trk_view *v; (void)a; (void)p; (void)u; if ((v = tracker_or_status())) trk_view_edit_samples(v); }
static void act_trk_keys(GSimpleAction *a, GVariant *p, gpointer u)
{ trk_view *v; (void)a; (void)p; (void)u; if ((v = tracker_or_status())) trk_view_show_keys(v); }
static void act_trk_audio(GSimpleAction *a, GVariant *p, gpointer u)
{ trk_view *v; (void)a; (void)p; (void)u; if ((v = tracker_or_status())) trk_view_audio_output(v); }
static void act_trk_columns(GSimpleAction *a, GVariant *p, gpointer u)
{ trk_view *v; (void)a; (void)p; (void)u; if ((v = tracker_or_status())) trk_view_show_columns(v); }
/* View > On-screen keyboard: every synth tab's keys, on by default. */
static void act_view_keyboard(GSimpleAction *a, GVariant *v, gpointer u)
{
    int t;
    (void)u;
    g_simple_action_set_state(a, v);
    g_kbd_on = g_variant_get_boolean(v);
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_tabs[t].kbrow) gtk_widget_set_visible(g_tabs[t].kbrow, g_kbd_on != 0);
}

/* View > Level meters: a stateful toggle, on by default. */
static void act_trk_meters(GSimpleAction *a, GVariant *v, gpointer u)
{
    (void)u;
    g_simple_action_set_state(a, v);
    if (g_tracker) trk_view_set_meters(g_tracker, g_variant_get_boolean(v));
}

static void act_trk_pitch(GSimpleAction *a, GVariant *v, gpointer u)
{
    (void)u;
    g_simple_action_set_state(a, v);
    if (g_tracker) trk_view_set_pitch_colors(g_tracker, g_variant_get_boolean(v));
}

static void act_trk_cheat(GSimpleAction *a, GVariant *p, gpointer u)
{ trk_view *v; (void)a; (void)p; (void)u; if ((v = tracker_or_status())) trk_view_show_cheat(v); }

/* One menu bar for every tab: File always; Synth only with a synth tab in
 * front, Samples only with the tracker in front; Help always, with the
 * tracker's Keys and Cheat Sheet ahead of About when the tracker is. The
 * bar's model is edited in place and the widget follows it. */
static void sync_menus(GtkWidget *front)
{
    int is_tracker = front && front == g_tracker_page;
    int is_synth = front && !is_tracker;
    if (!g_bar) return;
    g_menu_remove_all(g_bar);
    g_menu_append_submenu(g_bar, "File", G_MENU_MODEL(g_file_m));
    if (is_synth)   g_menu_append_submenu(g_bar, "Synth", G_MENU_MODEL(g_synth_m));
    if (is_tracker) g_menu_append_submenu(g_bar, "Samples", G_MENU_MODEL(g_samples_m));
    g_menu_remove_all(g_view_m);
    if (is_synth)   g_menu_append(g_view_m, "On-screen keyboard", "win.view-keyboard");
    if (is_tracker) g_menu_append(g_view_m, "Level meters", "win.tracker-meters");
    if (is_tracker) g_menu_append(g_view_m, "Color notes by pitch", "win.tracker-pitch");
    if (is_synth || is_tracker) g_menu_append_submenu(g_bar, "View", G_MENU_MODEL(g_view_m));
    g_menu_remove_all(g_help_m);
    if (is_tracker) g_menu_append_section(g_help_m, NULL, G_MENU_MODEL(g_help_trk));
    g_menu_append(g_help_m, "About studiogtk", "win.about");
    if (g_connect_shown && g_connect_m) g_menu_append_submenu(g_bar, "Connect", G_MENU_MODEL(g_connect_m));
    g_menu_append_submenu(g_bar, "Help", G_MENU_MODEL(g_help_m));
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a; (void)p; (void)u; gtk_window_close(GTK_WINDOW(g_win)); }

static void on_session_opened(GObject *src, GAsyncResult *res, gpointer u)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    (void)u;
    if (!f) return;
    if ((path = g_file_get_path(f))) {
        open_session_path(path);
        g_free(path);
    }
    g_object_unref(f);
}

static void act_open_session(GSimpleAction *a, GVariant *p, gpointer u)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *ft = gtk_file_filter_new();
    GListStore *fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)a; (void)p; (void)u;
    gtk_file_dialog_set_title(d, "Open session");
    gtk_file_filter_set_name(ft, "vst-ace sessions");
    gtk_file_filter_add_pattern(ft, "*.vstace");
    g_list_store_append(fs, ft);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    g_object_unref(ft);
    g_object_unref(fs);
    gtk_file_dialog_open(d, GTK_WINDOW(g_win), NULL, on_session_opened, NULL);
    g_object_unref(d);
}

static void on_session_save_chosen(GObject *src, GAsyncResult *res, gpointer u)
{
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    (void)u;
    if (!f) return;
    if ((path = g_file_get_path(f))) {
        char with_ext[4096];
        snprintf(with_ext, sizeof with_ext, "%s%s", path,
                 strstr(path, ".vstace") ? "" : ".vstace");
        save_session_checked(with_ext);
        g_free(path);
    }
    g_object_unref(f);
}

static void act_save_session_as(GSimpleAction *a, GVariant *p, gpointer u)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    const char *base;
    (void)a; (void)p; (void)u;
    gtk_file_dialog_set_title(d, "Save session");
    gtk_file_dialog_set_initial_name(d,
        g_session_path[0] ? ((base = strrchr(g_session_path, '/')) ? base + 1
                                                                     : g_session_path)
                          : "session.vstace");
    gtk_file_dialog_save(d, GTK_WINDOW(g_win), NULL, on_session_save_chosen, NULL);
    g_object_unref(d);
}

static void act_save_session(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p; (void)u;
    if (g_session_path[0]) save_session_checked(g_session_path);
    else act_save_session_as(a, p, u);
}

/* The Synth menu: the pane's own commands, acting on the tab in front. One
 * shared menu set rather than a set per tab -- GTK's popover menus rebuild
 * per window, not per tab, and what they act on is always the tab you are
 * looking at. */
static synctab *synth_or_status(void)
{
    synctab *tab = current_synth_tab();
    if (!tab) status("the front tab is not a synth");
    return tab;
}

static void act_open_vst(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_open_vst(t->pv, GTK_WINDOW(g_win)); }

static void act_load_folder(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_load_folder(t->pv, GTK_WINDOW(g_win)); }

static void act_save_patch(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_save_patch(t->pv, GTK_WINDOW(g_win)); }

static void act_open_patch(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_load_patch(t->pv, GTK_WINDOW(g_win)); }

static void act_plugin_folders(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_edit_folders(t->pv, GTK_WINDOW(g_win)); }

/* File > Plug-in folders: the same dialog, from whichever tab is in front --
 * and with none open, from a new synth tab, which is what the dialog's
 * rescans land in. A setting is not to be out of reach until a synth is. */
static void act_plugin_folders_any(GSimpleAction *a, GVariant *p, gpointer u)
{
    synctab *t = g_front;
    int i;
    (void)a; (void)p; (void)u;
    for (i = 0; !t && i < MAXTABS; i++) if (g_tabs[i].used) t = &g_tabs[i];
    if (!t) t = add_synth_tab();
    if (t) plugview_edit_folders(t->pv, GTK_WINDOW(g_win));
}

/* ------------------------------------------------------- plug-in manager --
 *
 * File > Plug-ins: every plug-in the scan found, in one list -- loaded into a
 * new tab, unloaded (the tab that holds it is closed), or taken off the list.
 * Taking one off does not delete its file: the path goes in the hidden list
 * and the scans skip it, until it is put back. */

typedef struct {
    GtkWidget *win, *list, *search, *loadb, *unloadb, *removeb, *show_removed, *note;
    int        removed_view;                 /* the list shows what was taken off */
} plugmgr;

static plugmgr *g_pm;

/* The pane whose scan the list reads: any synth tab will do, they all scan the
 * same folders. */
static synctab *any_synth_tab(int make)
{
    int i;
    for (i = 0; i < MAXTABS; i++) if (g_tabs[i].used) return &g_tabs[i];
    return make ? add_synth_tab() : NULL;
}

static synctab *tab_holding(const char *path)
{
    int i;
    for (i = 0; i < MAXTABS; i++)
        if (g_tabs[i].used && !strcmp(plugview_loaded_path(g_tabs[i].pv), path)) return &g_tabs[i];
    return NULL;
}

static const char *pm_selected_path(void)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_pm->list));
    return row ? g_object_get_data(G_OBJECT(row), "path") : NULL;
}

static void pm_sync_buttons(void)
{
    const char *p = pm_selected_path();
    int loaded = p && tab_holding(p);
    gtk_widget_set_sensitive(g_pm->loadb, p && !g_pm->removed_view);
    gtk_widget_set_sensitive(g_pm->unloadb, loaded && !g_pm->removed_view);
    gtk_widget_set_sensitive(g_pm->removeb, p != NULL);
    gtk_button_set_label(GTK_BUTTON(g_pm->removeb), g_pm->removed_view ? "Put back" : "Remove from list");
}

static void pm_refill(void)
{
    synctab *t = any_synth_tab(0);
    const char *needle = gtk_editable_get_text(GTK_EDITABLE(g_pm->search));
    GtkWidget *c;
    int i, shown = 0, total = 0;
    char note[160];

    while ((c = gtk_widget_get_first_child(g_pm->list))) gtk_list_box_remove(GTK_LIST_BOX(g_pm->list), c);

    if (g_pm->removed_view) {
        static char hid[512][VSTDIRS_PATHLEN];
        int n = vstdirs_hidden_list(hid, 512);
        for (i = 0; i < n; i++) {
            const char *base = strrchr(hid[i], '/');
            char *label;
            GtkWidget *row, *l;
            total++;
            if (*needle && !g_strrstr(hid[i], needle)) continue;
            label = g_strdup_printf("%s   (%s)", base ? base + 1 : hid[i], hid[i]);
            l = gtk_label_new(label);
            gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
            row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
            g_object_set_data_full(G_OBJECT(row), "path", g_strdup(hid[i]), g_free);
            gtk_list_box_append(GTK_LIST_BOX(g_pm->list), row);
            g_free(label);
            shown++;
        }
        snprintf(note, sizeof note, "%d of %d plug-ins taken off the list", shown, total);
    } else if (t) {
        int n = plugview_available_count(t->pv);
        for (i = 0; i < n; i++) {
            const char *path = NULL, *name = NULL, *kind = NULL;
            int loadable = 0;
            char *label, *esc;
            GtkWidget *row, *l;
            plugview_available(t->pv, i, &path, &name, &kind, &loadable);
            total++;
            if (*needle) {   /* one letter: names starting with it; more: a keyword */
                char *ln = g_utf8_strdown(needle, -1), *ln1 = g_utf8_strdown(name, -1), *ln2 = g_utf8_strdown(path, -1);
                int hit = g_utf8_strlen(ln, -1) == 1 ? g_str_has_prefix(ln1, ln)
                                                     : (strstr(ln1, ln) || strstr(ln2, ln));
                g_free(ln); g_free(ln1); g_free(ln2);
                if (!hit) continue;
            }
            esc = g_markup_escape_text(name, -1);
            label = g_strdup_printf("%s%s%s%s%s", tab_holding(path) ? "<b>● " : "", esc,
                                    tab_holding(path) ? "</b>   loaded" : "",
                                    kind && *kind ? "   <small>" : "", "");
            if (kind && *kind) {
                char *k = g_markup_escape_text(kind, -1), *tmp = g_strdup_printf("%s%s</small>", label, k);
                g_free(label); g_free(k); label = tmp;
            }
            l = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(l), label);
            gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
            if (!loadable) gtk_widget_set_opacity(l, 0.55);
            row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
            gtk_widget_set_tooltip_text(row, path);
            g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
            gtk_list_box_append(GTK_LIST_BOX(g_pm->list), row);
            g_free(label); g_free(esc);
            shown++;
        }
        snprintf(note, sizeof note, "%d of %d plug-ins available", shown, total);
    } else {
        snprintf(note, sizeof note, "no list yet -- open a synth tab to scan");
    }
    gtk_label_set_text(GTK_LABEL(g_pm->note), note);
    pm_sync_buttons();
}

static void pm_on_search(GtkEditable *e, gpointer u) { (void)e; (void)u; if (g_pm) pm_refill(); }
static void pm_on_select(GtkListBox *b, GtkListBoxRow *r, gpointer u) { (void)b; (void)r; (void)u; if (g_pm) pm_sync_buttons(); }

static void pm_on_load(GtkButton *b, gpointer u)
{
    const char *p = pm_selected_path();
    synctab *tab;
    (void)b; (void)u;
    if (!p) return;
    if ((tab = tab_holding(p))) {                         /* already open: bring it forward */
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook),
            gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), tab->pane));
        return;
    }
    if (!(tab = add_synth_tab())) return;
    if (!load_busy(tab->pv, p)) {
        close_synth_tab(tab);
        status("that plug-in could not be loaded");
    }
    pm_refill();
}

static void pm_on_unload(GtkButton *b, gpointer u)
{
    const char *p = pm_selected_path();
    synctab *tab;
    (void)b; (void)u;
    if (p && (tab = tab_holding(p))) close_synth_tab(tab);
    pm_refill();
}

static void pm_rescan_all(void)
{
    int i;
    for (i = 0; i < MAXTABS; i++) if (g_tabs[i].used) plugview_rescan(g_tabs[i].pv);
}

static void pm_on_remove(GtkButton *b, gpointer u)
{
    char *p = g_strdup(pm_selected_path());
    (void)b; (void)u;
    if (!p) return;
    if (g_pm->removed_view) vstdirs_unhide(p);
    else vstdirs_hide(p);
    pm_rescan_all();
    pm_refill();
    g_free(p);
}

static void pm_list_changed(void)
{
    pm_rescan_all();
    if (g_pm) pm_refill();
}

static void pm_on_add(GtkButton *b, gpointer u)
{
    synctab *t = synth_or_status();
    (void)b;
    if (t) plugview_add_plugin(t->pv, GTK_WINDOW(g_pm->win), GPOINTER_TO_INT(u));
}

static void pm_on_rescan(GtkButton *b, gpointer u) { (void)b; (void)u; pm_rescan_all(); pm_refill(); }

static void pm_on_toggle(GtkCheckButton *c, gpointer u)
{
    (void)u;
    g_pm->removed_view = gtk_check_button_get_active(c);
    pm_refill();
}

static void pm_gone(GtkWidget *w, gpointer u) { (void)w; (void)u; g_free(g_pm); g_pm = NULL; }

static void act_plugin_manager(GSimpleAction *a, GVariant *p, gpointer u)
{
    GtkWidget *box, *sw, *row, *close;
    (void)a; (void)p; (void)u;
    if (g_pm) { gtk_window_present(GTK_WINDOW(g_pm->win)); return; }
    if (!any_synth_tab(1)) return;                        /* the scan lives in a synth pane */
    g_pm = g_new0(plugmgr, 1);
    g_pm->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(g_pm->win), "Plug-ins");
    gtk_window_set_transient_for(GTK_WINDOW(g_pm->win), GTK_WINDOW(g_win));
    gtk_window_set_default_size(GTK_WINDOW(g_pm->win), 640, 520);
    g_signal_connect(g_pm->win, "destroy", G_CALLBACK(pm_gone), NULL);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);   gtk_widget_set_margin_bottom(box, 12);
    g_pm->search = gtk_search_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(g_pm->search), "");
    g_signal_connect(g_pm->search, "search-changed", G_CALLBACK(pm_on_search), NULL);
    gtk_box_append(GTK_BOX(box), g_pm->search);

    g_pm->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_pm->list), GTK_SELECTION_SINGLE);
    g_signal_connect(g_pm->list, "row-selected", G_CALLBACK(pm_on_select), NULL);
    sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), g_pm->list);
    gtk_box_append(GTK_BOX(box), sw);

    g_pm->note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_pm->note), 0.0f);
    gtk_widget_add_css_class(g_pm->note, "dim-label");
    gtk_box_append(GTK_BOX(box), g_pm->note);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g_pm->loadb = gtk_button_new_with_label("Load");
    g_pm->unloadb = gtk_button_new_with_label("Unload");
    g_pm->removeb = gtk_button_new_with_label("Remove from list");
    gtk_widget_set_tooltip_text(g_pm->unloadb, "Closes the tab that has this plug-in open");
    gtk_widget_set_tooltip_text(g_pm->removeb, "Takes it off the list. The file stays where it is; "
                                               "Show removed lets you put it back");
    g_signal_connect(g_pm->loadb, "clicked", G_CALLBACK(pm_on_load), NULL);
    g_signal_connect(g_pm->unloadb, "clicked", G_CALLBACK(pm_on_unload), NULL);
    g_signal_connect(g_pm->removeb, "clicked", G_CALLBACK(pm_on_remove), NULL);
    gtk_box_append(GTK_BOX(row), g_pm->loadb);
    gtk_box_append(GTK_BOX(row), g_pm->unloadb);
    gtk_box_append(GTK_BOX(row), g_pm->removeb);
    g_pm->show_removed = gtk_check_button_new_with_label("Show removed");
    g_signal_connect(g_pm->show_removed, "toggled", G_CALLBACK(pm_on_toggle), NULL);
    gtk_box_append(GTK_BOX(row), g_pm->show_removed);
    {
        GtkWidget *add = gtk_button_new_with_label("Add plug-in…");
        GtkWidget *addb = gtk_button_new_with_label("Add bundle…");
        gtk_widget_set_tooltip_text(add, "Pick a plug-in file: use it for this session only, or "
                                         "install it into the folder for its kind");
        gtk_widget_set_tooltip_text(addb, "The same for a .vst3 / .vst / .component bundle folder");
        g_signal_connect(add, "clicked", G_CALLBACK(pm_on_add), GINT_TO_POINTER(0));
        g_signal_connect(addb, "clicked", G_CALLBACK(pm_on_add), GINT_TO_POINTER(1));
        gtk_box_append(GTK_BOX(row), add);
        gtk_box_append(GTK_BOX(row), addb);
        plugview_set_list_changed(pm_list_changed);
    }
    {
        GtkWidget *rescan = gtk_button_new_with_label("Rescan");
        GtkWidget *spacer = gtk_label_new("");
        g_signal_connect(rescan, "clicked", G_CALLBACK(pm_on_rescan), NULL);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(row), spacer);
        gtk_box_append(GTK_BOX(row), rescan);
    }
    close = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), g_pm->win);
    gtk_box_append(GTK_BOX(row), close);
    gtk_box_append(GTK_BOX(box), row);

    gtk_window_set_child(GTK_WINDOW(g_pm->win), box);
    pm_refill();
    gtk_window_present(GTK_WINDOW(g_pm->win));
}

/* ---------------------------------------------------------- audio settings --
 *
 * File > Audio: PipeWire, JACK or ALSA for the synths, and which ALSA device.
 * Applying stops the running backend and starts the chosen one -- with the tabs
 * left as they are, silent for the moment between -- and a backend that will not
 * start is said so about and replaced, not left as silence. */

static void engine_restart_note(const ao_choice *c)
{
    engine_stop_audio();
    engine_start_with(c);
    status(g_audio_note);
}

typedef struct {
    GtkWidget *win, *radio[4], *dev, *now, *note;
    char names[24][128], labels[24][96];
    int ndev;
} audiodlg;

static audiodlg *g_ad;

static void ad_show_now(void)
{
    char t[400];
    snprintf(t, sizeof t, "Now: %s", g_backend);
    gtk_label_set_text(GTK_LABEL(g_ad->now), t);
    gtk_label_set_text(GTK_LABEL(g_ad->note), g_audio_note);
}

static void ad_apply(GtkButton *b, gpointer u)
{
    ao_choice c;
    int i, sel = AO_AUTO;
    (void)b; (void)u;
    for (i = 0; i < 4; i++)
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(g_ad->radio[i]))) sel = i;
    c.backend = sel;
    c.device[0] = 0;
    if (g_ad->ndev) {
        guint d = gtk_drop_down_get_selected(GTK_DROP_DOWN(g_ad->dev));
        if (d < (guint)g_ad->ndev && strcmp(g_ad->names[d], "default"))
            snprintf(c.device, sizeof c.device, "%s", g_ad->names[d]);
    }
    ao_choice_save(&c);
    engine_restart_note(&c);
    ad_show_now();
}

static void ad_gone(GtkWidget *w, gpointer u) { (void)w; (void)u; g_free(g_ad); g_ad = NULL; }

static void act_audio(GSimpleAction *a, GVariant *p, gpointer u)
{
    GtkWidget *box, *row, *apply, *close, *intro;
    static const char *const labels[4] = { "Automatic (PipeWire, else ALSA)", "PipeWire", "JACK", "ALSA (direct)" };
    ao_choice cur;
    char why[160];
    int i;
    (void)a; (void)p; (void)u;
    if (g_ad) { gtk_window_present(GTK_WINDOW(g_ad->win)); return; }
    g_ad = g_new0(audiodlg, 1);
    g_ad->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(g_ad->win), "Audio");
    gtk_window_set_transient_for(GTK_WINDOW(g_ad->win), GTK_WINDOW(g_win));
    g_signal_connect(g_ad->win, "destroy", G_CALLBACK(ad_gone), NULL);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 14); gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 12);   gtk_widget_set_margin_bottom(box, 12);
    intro = gtk_label_new("Where the synths play. The choice is kept, and applied now.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
    gtk_box_append(GTK_BOX(box), intro);

    ao_choice_load(&cur);
    for (i = 0; i < 4; i++) {
        g_ad->radio[i] = gtk_check_button_new_with_label(labels[i]);
        if (i) gtk_check_button_set_group(GTK_CHECK_BUTTON(g_ad->radio[i]), GTK_CHECK_BUTTON(g_ad->radio[0]));
        if (i == cur.backend) gtk_check_button_set_active(GTK_CHECK_BUTTON(g_ad->radio[i]), TRUE);
        gtk_box_append(GTK_BOX(box), g_ad->radio[i]);
    }
    if (!ao_jack_available(why, sizeof why)) {
        char tip[200];
        snprintf(tip, sizeof tip, "JACK is not available: %s", why);
        gtk_widget_set_tooltip_text(g_ad->radio[AO_JACK], tip);
        gtk_check_button_set_label(GTK_CHECK_BUTTON(g_ad->radio[AO_JACK]), "JACK (not available right now)");
    }
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row), gtk_label_new("ALSA device:"));
    {
        const char *items[26];
        int sel = 0;
        g_ad->ndev = ao_alsa_devices(g_ad->names, g_ad->labels, 24);
        for (i = 0; i < g_ad->ndev; i++) {
            items[i] = g_ad->labels[i];
            if (cur.device[0] ? !strcmp(cur.device, g_ad->names[i]) : !strcmp(g_ad->names[i], "default")) sel = i;
        }
        items[g_ad->ndev] = NULL;
        g_ad->dev = gtk_drop_down_new_from_strings(items);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ad->dev), (guint)sel);
    }
    gtk_widget_set_hexpand(g_ad->dev, TRUE);
    gtk_box_append(GTK_BOX(row), g_ad->dev);
    gtk_box_append(GTK_BOX(box), row);
    g_ad->now = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_ad->now), 0.0f);
    gtk_box_append(GTK_BOX(box), g_ad->now);
    g_ad->note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_ad->note), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(g_ad->note), TRUE);
    gtk_widget_add_css_class(g_ad->note, "dim-label");
    gtk_box_append(GTK_BOX(box), g_ad->note);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    apply = gtk_button_new_with_label("Apply");
    close = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    g_signal_connect(apply, "clicked", G_CALLBACK(ad_apply), NULL);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), g_ad->win);
    gtk_box_append(GTK_BOX(row), apply);
    gtk_box_append(GTK_BOX(row), close);
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(GTK_WINDOW(g_ad->win), box);
    ad_show_now();
    gtk_window_present(GTK_WINDOW(g_ad->win));
}

static void act_keep_folder(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_keep_folder(t->pv); }

static void act_enter_key(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_enter_key(t->pv, GTK_WINDOW(g_win)); }

static void act_toggle_editor(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_toggle_editor(t->pv); }

/* Synth > Reload Plug-in: the way back for a tab whose plug-in stopped and
 * would not restart. It is the ordinary load path, so a live plug-in is just
 * loaded again. */
static void act_reload_plugin(GSimpleAction *a, GVariant *p, gpointer u)
{
    synctab *t;
    char path[1024], msg[1100], *sound;
    (void)a; (void)p; (void)u;
    if (!(t = synth_or_status())) return;
    /* Copied out: the load rewrites the pane's own loaded_path. */
    snprintf(path, sizeof path, "%s", plugview_loaded_path(t->pv));
    if (!path[0]) { status("nothing is loaded in this tab to reload"); return; }
    /* The sound comes along: captured first -- from a dead helper that is the
     * bridge's record of what was last set, which is what a restart would
     * have put back -- and applied to the fresh instance. */
    sound = plugview_capture_patch(t->pv);
    snprintf(msg, sizeof msg, "reloading %s ...", path);
    status(msg);
    if (!load_busy(t->pv, path)) {
        snprintf(msg, sizeof msg, "could not reload %s", path);
        status(msg);
    } else if (sound && plugview_apply_patch(t->pv, sound)) {
        snprintf(msg, sizeof msg, "reloaded %s, but not its sound", path);
        status(msg);
    }
    free(sound);
}

static void act_panic(GSimpleAction *a, GVariant *p, gpointer u)
{
    synctab *t = current_synth_tab();
    (void)a; (void)p; (void)u;
    if (t) {
        release_tab(t);
        plugview_release_all(t->pv);
    }
    /* The tracker's tracks too, if one is open: a stuck note is stuck
     * wherever it is sounding. */
    if (g_trk) trk_panic(g_trk);
}

static void act_about(GSimpleAction *a, GVariant *p, gpointer u)
{
    GtkAlertDialog *d;
    char body[640];
    (void)a; (void)p; (void)u;

    snprintf(body, sizeof body,
             "Built %s%s%s\n\n"
             "A session window: each tab hosts one plug-in natively \xe2\x80\x94 "
             "Windows, macOS or Linux, no Wine, no emulation \xe2\x80\x94 and "
             "one tab is the pattern tracker that plays them, over ALSA MIDI "
             "or directly in this process.\n\n"
             "This window is studiogtk (GTK4).",
             VSTACE_BUILD_DATE,
             VSTACE_GIT[0] ? "\nCommit " : "",
             VSTACE_GIT[0] ? VSTACE_GIT : "");
    d = gtk_alert_dialog_new("vst-ace %s", VSTACE_VERSION);
    gtk_alert_dialog_set_detail(d, body);
    gtk_alert_dialog_show(d, GTK_WINDOW(g_win));
    g_object_unref(d);
}

static GtkWidget *build_menubar(GtkApplication *app)
{
    static const GActionEntry entries[] = {
        { "new-synth",   act_new_synth,   NULL, NULL, NULL, {0} },
        { "new-tracker", act_new_tracker, NULL, NULL, NULL, {0} },
        { "new-song",    act_new_song,    NULL, NULL, NULL, {0} },
        { "open-song",   act_open_song,   NULL, NULL, NULL, {0} },
        { "save-song",   act_save_song,   NULL, NULL, NULL, {0} },
        { "save-song-as", act_save_song_as, NULL, NULL, NULL, {0} },
        { "export-midi",  act_export_midi,  NULL, NULL, NULL, {0} },
        { "export-take",  act_export_take,  NULL, NULL, NULL, {0} },
        { "open-session",    act_open_session,    NULL, NULL, NULL, {0} },
        { "open-recent",     act_open_recent,     "s",  NULL, NULL, {0} },
        { "clear-recent",    act_clear_recent,    NULL, NULL, NULL, {0} },
        { "save-session",    act_save_session,    NULL, NULL, NULL, {0} },
        { "save-session-as", act_save_session_as, NULL, NULL, NULL, {0} },
        { "reload-plugin",   act_reload_plugin,   NULL, NULL, NULL, {0} },
        { "close-tab",   act_close_tab,   NULL, NULL, NULL, {0} },
        { "quit",        act_quit,        NULL, NULL, NULL, {0} },
        { "open-vst",    act_open_vst,    NULL, NULL, NULL, {0} },
        { "load-folder", act_load_folder, NULL, NULL, NULL, {0} },
        { "save-patch",  act_save_patch,  NULL, NULL, NULL, {0} },
        { "open-patch",  act_open_patch,  NULL, NULL, NULL, {0} },
        { "plugin-folders", act_plugin_folders, NULL, NULL, NULL, {0} },
        { "plugin-folders-any", act_plugin_folders_any, NULL, NULL, NULL, {0} },
        { "keep-folder", act_keep_folder, NULL, NULL, NULL, {0} },
        { "plugin-manager", act_plugin_manager, NULL, NULL, NULL, {0} },
        { "audio-settings", act_audio, NULL, NULL, NULL, {0} },
        { "effect-input", NULL, NULL, "false", act_effect_input, {0} },
        { "record-mix", NULL, NULL, "false", act_record_mix, {0} },
        { "connect-reaper", NULL, NULL, "false", act_connect_reaper, {0} },
        { "enter-key",   act_enter_key,   NULL, NULL, NULL, {0} },
        { "toggle-editor", act_toggle_editor, NULL, NULL, NULL, {0} },
        { "panic",       act_panic,       NULL, NULL, NULL, {0} },
        { "about",       act_about,       NULL, NULL, NULL, {0} },
        { "tracker-load-samples", act_trk_load_samples, NULL, NULL, NULL, {0} },
        { "tracker-edit-samples", act_trk_edit_samples, NULL, NULL, NULL, {0} },
        { "tracker-keys",         act_trk_keys,         NULL, NULL, NULL, {0} },
        { "tracker-columns",      act_trk_columns,      NULL, NULL, NULL, {0} },
        { "tracker-audio",        act_trk_audio,        NULL, NULL, NULL, {0} },
        { "tracker-cheat",        act_trk_cheat,        NULL, NULL, NULL, {0} },
        { "tracker-meters",       NULL, NULL, "true", act_trk_meters, {0} },
        { "tracker-pitch",        NULL, NULL, "true", act_trk_pitch,  {0} },
        { "view-keyboard",        NULL, NULL, "true", act_view_keyboard, {0} },
    };
    GMenu *bar   = g_menu_new();
    GMenu *file  = g_menu_new();
    GMenu *sect  = g_menu_new();
    GMenu *sess  = g_menu_new();
    GMenu *synth = g_menu_new();
    GMenu *s2    = g_menu_new();
    GMenu *help  = g_menu_new();
    GtkWidget *w;

    g_action_map_add_action_entries(G_ACTION_MAP(g_win), entries,
                                    G_N_ELEMENTS(entries), NULL);
    g_new_tracker_act = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(g_win), "new-tracker"));
    g_reaper_act = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(g_win), "connect-reaper"));
    g_connect_m = g_menu_new();
    snprintf(g_connect_info, sizeof g_connect_info, "REAPER: off");
    connect_rebuild_menu();
    /* Everything on these menus carries Ctrl: the note keys are plain
     * letters, and a bare shortcut would be a letter that no longer plays. */
    gtk_application_set_accels_for_action(app, "win.new-synth",
                                          (const char *[]){ "<Control>n", NULL });
    gtk_application_set_accels_for_action(app, "win.new-tracker",
                                          (const char *[]){ "<Control>t", NULL });
    gtk_application_set_accels_for_action(app, "win.close-tab",
                                          (const char *[]){ "<Control>w", NULL });
    gtk_application_set_accels_for_action(app, "win.quit",
                                          (const char *[]){ "<Control>q", NULL });
    gtk_application_set_accels_for_action(app, "win.open-vst",
                                          (const char *[]){ "<Control>o", NULL });
    gtk_application_set_accels_for_action(app, "win.load-folder",
                                          (const char *[]){ "<Control>l", NULL });
    gtk_application_set_accels_for_action(app, "win.save-patch",
                                          (const char *[]){ "<Control>s", NULL });
    gtk_application_set_accels_for_action(app, "win.open-patch",
                                          (const char *[]){ "<Control>p", NULL });
    /* Save song wanted Ctrl+S, but that is Save Patch's: an application
     * accel names one action, so the song takes the shifted chord. */
    gtk_application_set_accels_for_action(app, "win.save-song",
                                          (const char *[]){ "<Control><Shift>s", NULL });
    gtk_application_set_accels_for_action(app, "win.plugin-folders",
                                          (const char *[]){ "<Control>d", NULL });
    gtk_application_set_accels_for_action(app, "win.enter-key",
                                          (const char *[]){ "<Control>k", NULL });
    gtk_application_set_accels_for_action(app, "win.toggle-editor",
                                          (const char *[]){ "<Control>e", NULL });
    gtk_application_set_accels_for_action(app, "win.panic",
                                          (const char *[]){ "<Control>period",
                                                            "<Control>Escape", NULL });

    g_menu_append(file, "New synth…",  "win.new-synth");
    g_menu_append(file, "New tracker", "win.new-tracker");
    g_menu_append(file, "New song",    "win.new-song");
    g_menu_append(file, "Open song…",  "win.open-song");
    g_recent_m = g_menu_new();
    g_menu_append_submenu(file, "Open Recent", G_MENU_MODEL(g_recent_m));
    recent_refresh();
    g_menu_append(file, "Save song",     "win.save-song");
    g_menu_append(file, "Save song as…", "win.save-song-as");
    g_menu_append(file, "Export song as MIDI…", "win.export-midi");
    g_menu_append(file, "Export recorded take as MIDI…", "win.export-take");
    g_menu_append(sess, "Open session…",    "win.open-session");
    g_menu_append(sess, "Save session",     "win.save-session");
    g_menu_append(sess, "Save session as…", "win.save-session-as");
    g_menu_append_section(file, NULL, G_MENU_MODEL(sess));
    g_menu_append(sect, "Plug-ins…", "win.plugin-manager");
    g_menu_append(sect, "Audio…", "win.audio-settings");
    g_menu_append(sect, "Effect input (microphone)", "win.effect-input");
    g_menu_append(sect, "Record studio mix", "win.record-mix");
    g_menu_append(sect, "Plug-in folders…", "win.plugin-folders-any");
    g_menu_append(sect, "Close tab",   "win.close-tab");
    g_menu_append(sect, "Quit",        "win.quit");
    g_menu_append_section(file, NULL, G_MENU_MODEL(sect));
    g_file_m = file;

    g_menu_append(synth, "Open VST…",   "win.open-vst");
    g_menu_append(synth, "Reload Plug-in", "win.reload-plugin");
    g_menu_append(synth, "Load Folder…", "win.load-folder");
    g_menu_append(synth, "Save Patch…", "win.save-patch");
    g_menu_append(synth, "Open Patch…", "win.open-patch");
    g_menu_append(s2, "Plug-in Folders…", "win.plugin-folders");
    g_menu_append(s2, "Keep This Plug-in's Folder", "win.keep-folder");
    g_menu_append(s2, "Enter Key / Serial…", "win.enter-key");
    g_menu_append_section(synth, NULL, G_MENU_MODEL(s2));
    g_menu_append(synth, "Parameters / Editor", "win.toggle-editor");
    g_menu_append(synth, "All Notes Off", "win.panic");
    g_synth_m = synth;

    g_samples_m = g_menu_new();
    g_menu_append(g_samples_m, "Load Sample Set…", "win.tracker-load-samples");
    g_menu_append(g_samples_m, "Edit Sample Set…", "win.tracker-edit-samples");
    g_menu_append(g_samples_m, "Audio Output…", "win.tracker-audio");
    g_view_m = g_menu_new();             /* filled by sync_menus: it depends on the tab */
    g_help_trk = g_menu_new();
    g_menu_append(g_help_trk, "Keys", "win.tracker-keys");
    g_menu_append(g_help_trk, "Columns", "win.tracker-columns");
    g_menu_append(g_help_trk, "Cheat Sheet", "win.tracker-cheat");
    g_help_m = help;
    g_bar = bar;
    sync_menus(NULL);                    /* no tab yet: File and Help */

    w = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    g_object_unref(s2);
    g_object_unref(sect); g_object_unref(sess);   /* the rest are kept: sync_menus re-adds them */
    return w;
}

/* ------------------------------------------------------------- the smoke
 *
 * Scripted exercise for a machine with no XTEST, in the idiom of the Qt
 * shell's --smoke. Every second the next tab is brought to the front (so
 * the keys-live arbitration runs), a note is played into every loaded synth
 * (so its peak meter has something to say), and each tab's callback count
 * and peak are printed. Three seconds before the end the first synth tab is
 * closed through the ordinary close path, and at the end the window closes.
 * With --route the note injection is skipped -- the tracker is playing the
 * routed tab by then, and its peak has to be the tracker's alone to prove
 * anything. */

static int g_smoke_step, g_smoke_note = 60;

static gboolean smoke_tick(gpointer u)
{
    GtkNotebook *nb = GTK_NOTEBOOK(g_notebook);
    int i, np = gtk_notebook_get_n_pages(nb);
    (void)u;

    if (np) gtk_notebook_set_current_page(nb, g_smoke_step % np);
    for (i = 0; i < np; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(nb, i);
        synctab *tab = tab_of_page(page);
        if (!tab) {
            fprintf(stderr, "smoke: tab %d (tracker)%s\n", i,
                    g_trk && trk_playing(g_trk) ? " playing" : "");
            continue;
        }
        if (!g_routed && plugview_active(tab->pv)) {
            plugview_note_off(tab->pv, g_smoke_note);
            g_smoke_note = 60 + (g_smoke_step + i) % 12;
            plugview_note_on(tab->pv, g_smoke_note, 100);
        }
        {
            unsigned long inj = 0, placed = 0;
            plugview_inject_stats(tab->pv, &inj, &placed);
            fprintf(stderr, "smoke: tab %d \"%s\" callbacks=%lu peak=%.3f "
                            "keysLive=%d injected=%lu placed=%lu%s\n",
                    i, tab->sink_name,
                    atomic_load_explicit(&tab->callbacks, memory_order_relaxed),
                    plugview_peak(tab->pv), tab == current_synth_tab(),
                    inj, placed, plugview_active(tab->pv) ? "" : " (no plug-in)");
            plugview_peak_reset(tab->pv);
        }
    }
    g_smoke_step++;
    fflush(stderr);
    return G_SOURCE_CONTINUE;
}

static gboolean smoke_close_one(gpointer u)
{
    int i, np = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    (void)u;
    for (i = 0; i < np; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i);
        if (tab_of_page(page)) {
            fprintf(stderr, "smoke: closing tab %d\n", i);
            fflush(stderr);
            close_tab_page(page);
            break;
        }
    }
    return G_SOURCE_REMOVE;
}

static gboolean smoke_quit(gpointer u)
{
    (void)u;
    gtk_window_close(GTK_WINDOW(g_win));
    return G_SOURCE_REMOVE;
}

static void start_smoke_drive(int ms)
{
    g_smoke_step = 0;
    g_timeout_add(1000, smoke_tick, NULL);
    g_timeout_add(ms > 4000 ? (guint)ms - 3000 : 1000, smoke_close_one, NULL);
    g_timeout_add((guint)ms, smoke_quit, NULL);
}

/* ------------------------------------------------------------- the window */

/* Set once the song question has been answered for this close, so the
 * close it leads to is not asked again: after Discard the song is still
 * dirty, and close-request would otherwise put the same question for ever. */
static int g_win_close_ok;

/* The close goes ahead: --save-session writes now, with the song question
 * (if there was one) already answered, so it is not asked a second time. */
static void win_close_proceed(void)
{
    g_win_close_ok = 1;
    if (g_save_on_exit[0]) write_session(g_save_on_exit);
}

static void win_close_confirmed(void *ud)
{
    (void)ud;
    win_close_proceed();
    gtk_window_close(GTK_WINDOW(g_win));
}

static gboolean on_win_close(GtkWindow *w, gpointer u)
{
    (void)w; (void)u;
    if (g_win_close_ok) return FALSE;
    /* The tracker's song may have unsaved changes; closing the window asks
     * exactly as closing its tab does, and the answer closes the window. */
    if (g_tracker && !g_smoke_ms && trk_view_dirty(g_tracker)) {
        trk_view_confirm_close(g_tracker, win_close_confirmed, NULL);
        return TRUE;
    }
    win_close_proceed();
    return FALSE;
}

static void on_win_destroy(GtkWidget *w, gpointer u)
{
    int t;
    (void)w; (void)u;
    /* The tabs die with the window rather than through close_tab_page, so
     * their tracker destinations are unregistered here: after the last
     * trk_remove_sink returns the delivery thread is provably out of every
     * pane, whatever order the widgets die in. */
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used && g_trk && g_tabs[t].sink_id >= 0) {
            trk_remove_sink(g_trk, g_tabs[t].sink_id);
            g_tabs[t].sink_id = -1;
        }
    /* Parked, and left parked: closing a plug-in the callback may be
     * rendering out of is a crash rather than a message, and silence is what
     * a closing window should be making anyway. */
    engine_park();
    for (t = 0; t < MAXTABS; t++)
        if (g_tabs[t].used) plugview_shutdown(g_tabs[t].pv);
}

/* The session named on the command line, once the window is up. Set from
 * main() before activate runs. */
static char   g_want_synths[MAXTABS][1024];
static int    g_nwant_synths;
static int    g_want_tracker;
static char   g_want_song[4096];
static char   g_want_record[4096];             /* --record-mix: start a take with the window */
static int    g_want_play;                     /* --play: start the song when it is open */
static char   g_want_session[4096];
static int    g_want_route = -1;
static int    g_quit_after;

static void activate(GtkApplication *app, gpointer ud)
{
    GtkWidget *outer;
    GtkEventController *kc;
    int i;
    (void)ud;

    g_win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_win), "studiogtk -- vst-ace session");
    gtk_window_set_default_size(GTK_WINDOW(g_win), 1280, 800);
    g_signal_connect(g_win, "close-request", G_CALLBACK(on_win_close), NULL);
    g_signal_connect(g_win, "destroy", G_CALLBACK(on_win_destroy), NULL);

    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(outer, 8); gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 4);   gtk_widget_set_margin_bottom(outer, 8);
    gtk_box_append(GTK_BOX(outer), build_menubar(app));

    /* No tabs at start: the canvas opens blank, with the way in said on it.
     * The stack is the hint page until the first tab exists. */
    g_stack = gtk_stack_new();
    g_hint = gtk_label_new(
        "Nothing is open.\n\n"
        "File > New synth… opens a plug-in host in a tab, one tab per "
        "plug-in.\nFile > New tracker opens the pattern sequencer; File > "
        "Open song… loads a song into it.");
    gtk_widget_add_css_class(g_hint, "dim-label");
    gtk_stack_add_child(GTK_STACK(g_stack), g_hint);

    g_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(g_notebook), TRUE);
    g_signal_connect(g_notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);
    gtk_stack_add_child(GTK_STACK(g_stack), g_notebook);
    gtk_widget_set_vexpand(g_stack, TRUE);
    gtk_box_append(GTK_BOX(outer), g_stack);

    g_status = gtk_label_new("open a synth or the tracker from the File menu");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.0);
    gtk_widget_set_hexpand(g_status, TRUE);
    g_mix_label = gtk_label_new("");
    gtk_widget_add_css_class(g_mix_label, "error");         /* red: a take is running */
    {
        GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_append(GTK_BOX(bar), g_status);
        gtk_box_append(GTK_BOX(bar), g_mix_label);
        gtk_box_append(GTK_BOX(outer), bar);
    }
    gtk_window_set_child(GTK_WINDOW(g_win), outer);

    /* The computer keyboard plays the front tab's piano; see on_key. The
     * controller is on the window, so a plug-in's editor has to be told
     * which keys to let past -- plugview_set_note_key, per tab. */
    kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed",  G_CALLBACK(on_key), NULL);
    g_signal_connect(kc, "key-released", G_CALLBACK(on_key_up), NULL);
    gtk_widget_add_controller(g_win, kc);
    g_signal_connect(g_win, "notify::is-active", G_CALLBACK(on_win_active), NULL);

    g_mix_synth = mixbus_register();
    engine_start_audio();
    midi_start();
    if (connect_pref_load() && !g_rl && (g_rl = rl_open()))       /* Connect > REAPER, as it was left */
        g_simple_action_set_state(g_reaper_act, g_variant_new_boolean(TRUE));
    if (getenv("STUDIOGTK_EFFECT_INPUT") && effect_input_start() == 0)   /* scripted: the menu item, from the environment */
        fprintf(stderr, "studiogtk: effect input on\n");
    update_canvas();
    g_timeout_add(1000, watch_tabs, NULL);
    /* (the keys the tracker is playing are polled from open_tracker_tab on) */

    gtk_window_present(GTK_WINDOW(g_win));      /* up before the loads, which block */
    busy_flush();

    if (g_want_session[0]) open_session_path(g_want_session);

    for (i = 0; i < g_nwant_synths; i++) {
        synctab *tab = add_synth_tab();
        if (tab && !load_busy(tab->pv, g_want_synths[i])) {
            char msg[1100];
            snprintf(msg, sizeof msg, "could not load %s", g_want_synths[i]);
            status(msg);
        }
    }
    if (g_want_tracker) open_tracker_tab();
    if (g_want_song[0]) open_song_path(g_want_song);
    if (g_want_record[0]) mix_begin(g_want_record);
    if (g_want_play && g_trk) trk_play(g_trk, TRK_PLAY_SONG, 0, 0);
    if (g_want_route > 0 && !route_track(g_want_route - 1))
        fprintf(stderr, "studiogtk: --route %d failed (no synth tab, or no tracker?)\n",
                g_want_route);
    if (g_smoke_ms > 0)
        start_smoke_drive(g_smoke_ms);
    else if (g_quit_after > 0)
        g_timeout_add((guint)g_quit_after, smoke_quit, NULL);

    gtk_window_present(GTK_WINDOW(g_win));
}

int main(int argc, char **argv)
{
    GtkApplication *app;
    int i, status_rc;

    /* Software rendering for our own widgets, and the X11 backend in a
     * Wayland session -- the same coercion dwstudio does, for the same
     * reason: native plug-in editors are X11 child windows of this window,
     * drawn with GLX, and GTK's GL renderer on the same hierarchy is what
     * kills them. Set GSK_RENDERER / GDK_BACKEND yourself to override. */
    g_setenv("GSK_RENDERER", "cairo", FALSE);
    if (!getenv("GDK_BACKEND")) {
        const char *sess = getenv("XDG_SESSION_TYPE");
        if ((sess && !strcmp(sess, "wayland")) || getenv("WAYLAND_DISPLAY")) {
            if (getenv("DISPLAY")) {
                g_setenv("GDK_BACKEND", "x11", TRUE);
                fprintf(stderr, "studiogtk: Wayland session -- using the x11 "
                                "backend so plug-in editors can embed\n");
            } else {
                fprintf(stderr, "studiogtk: Wayland session with no DISPLAY; "
                                "native plug-in editors need XWayland and "
                                "will be refused\n");
            }
        }
    }

    /* Host plug-ins out-of-process by default, as dwstudio and pestudio: a
     * plug-in that faults costs a helper subprocess rather than the whole
     * session. */
    if (!getenv("PEHOST_ISOLATE")) {
        pehost_set_isolation(1);
        fprintf(stderr, "studiogtk: hosting plug-ins out-of-process "
                        "(PEHOST_ISOLATE=0 to disable)\n");
    }

    /* A session can be named rather than clicked together:
     *
     *   studiogtk --synth blooo64.dll --synth "Surge XT.vst3" --tracker
     *   studiogtk --song song.trk
     *   studiogtk --session take-five.vstace   a saved session, whole
     *   studiogtk --save-session out.vstace ...   write the session on the clean exit
     *   studiogtk --route 2 --synth ... --tracker   track 2 plays the first synth tab
     *   studiogtk --smoke 15000 --synth ...   scripted exercise, then quit
     *   studiogtk --quit-after 15000 ...      just quit then
     */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--synth") && i + 1 < argc) {
            if (g_nwant_synths < MAXTABS)
                snprintf(g_want_synths[g_nwant_synths++], 1024, "%s", argv[++i]);
            else
                i++;
        } else if (!strcmp(argv[i], "--tracker")) {
            g_want_tracker = 1;
        } else if (!strcmp(argv[i], "--song") && i + 1 < argc) {
            snprintf(g_want_song, sizeof g_want_song, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--session") && i + 1 < argc) {
            snprintf(g_want_session, sizeof g_want_session, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--save-session") && i + 1 < argc) {
            snprintf(g_save_on_exit, sizeof g_save_on_exit, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--route") && i + 1 < argc) {
            g_want_route = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--smoke") && i + 1 < argc) {
            g_smoke_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc) {
            g_quit_after = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--record-mix") && i + 1 < argc) {
            snprintf(g_want_record, sizeof g_want_record, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--play")) {
            g_want_play = 1;
        } else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            g_backend_want = argv[++i];
            g_backend_cli = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("studiogtk [--synth <plug-in>]... [--tracker] [--song <file.trk>]\n"
                   "          [--session <file.vstace>] [--save-session <file.vstace>]\n"
                   "          [--route <track>] [--smoke <ms>] [--quit-after <ms>]\n"
                   "          [--backend auto|pipewire|jack|alsa]\n"
                   "          [--record-mix <file.wav>] [--play]\n\n"
                   "The session window, in GTK: a tab per synth plug-in, one for "
                   "the tracker.\nWith no arguments it opens on a blank canvas.\n"
                   "--session restores a session saved with File > Save session; "
                   "--save-session\nwrites one on the clean exit.\n"
                   "--route plays the numbered track into the first synth tab, "
                   "in-process,\nand starts the song -- the scripted proof of the "
                   "direct routing.\n");
            return 0;
        } else {
            fprintf(stderr, "studiogtk: unknown argument %s -- try --help\n", argv[i]);
            return 2;
        }
    }
    pw_init(&argc, &argv);
    {   /* DW_PERIOD / DW_LATENCY / DW_BACKEND, as dwstudio */
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

    app = gtk_application_new("de.fullbucket.studiogtk", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    {
        char *one[] = { argv[0], NULL };
        status_rc = g_application_run(G_APPLICATION(app), 1, one);
    }
    g_object_unref(app);

    /* Silence both backends before anything else goes away: PipeWire's data
     * loop is still calling render_block() at this point, and letting it run
     * into process teardown renders out of freed tab state. */
    if (mixbus_active()) {          /* a take still running when the window closes is finished, not lost */
        char f[4096];
        mixbus_stop(f, sizeof f);
        fprintf(stderr, "studiogtk: wrote %s\n", f);
    }
    connect_shutdown();
    kb_label_free();
    midi_stop();
    effect_input_stop();
    engine_stop_audio();
    pw_deinit();
    if (g_tracker) trk_view_free(g_tracker);  /* its widget died with the window */
    if (g_trk) trk_close(g_trk);
    for (i = 0; i < MAXTABS; i++)
        if (g_tabs[i].used) plugview_free(g_tabs[i].pv);
    return status_rc;
}
