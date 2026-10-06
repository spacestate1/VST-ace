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
 * mixes are changed only while it is parked. */

#include "plugview.h"
#include "trackerview.h"
#include "trk.h"
#include "pehost.h"

#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <gtk/gtk.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
} synctab;

static synctab      g_tabs[MAXTABS];
static trk_engine  *g_trk;
static trk_view    *g_tracker;
static GtkWidget   *g_tracker_page;
static synctab     *g_front;           /* the synth tab the keys play, if one is */

static GtkWidget   *g_win, *g_notebook, *g_stack, *g_hint, *g_status;
static GSimpleAction *g_new_tracker_act;
static int          g_routed;          /* --route: the tracker drives the proof */
static int          g_smoke_ms;

static void status(const char *msg)
{
    if (GTK_IS_LABEL(g_status)) gtk_label_set_text(GTK_LABEL(g_status), msg);
}

/* ------------------------------------------------------------- the audio
 *
 * One stream, every tab mixed into it. dwstudio's arrangement, minus its
 * engines: the plug-ins do their own event handling through pehost's queues,
 * so the callback only renders and sums. */

static snd_pcm_t *g_pcm;
static pthread_t  g_thread;
static _Atomic int g_running, g_parked, g_park_req;
static struct pw_thread_loop *g_pw_loop;
static struct pw_stream      *g_pw_stream;
static double    *g_pw_buf;
static const char *g_backend = "none";
static const char *g_backend_want = "auto";
static unsigned long g_xruns;
static __thread int g_teb_ready;

/* Scratch for one tab's block, mixed into the output at once. Sized once so
 * the audio thread never allocates. */
static float g_plug_buf[PERIOD_MAX * 2];

static void render_block(double *buf, int frames)
{
    int i, t, n = frames > PERIOD_MAX ? PERIOD_MAX : frames;

    memset(buf, 0, (size_t)n * 2 * sizeof *buf);
    for (t = 0; t < MAXTABS; t++) {
        if (!g_tabs[t].used || !plugview_active(g_tabs[t].pv)) continue;
        if (plugview_render(g_tabs[t].pv, g_plug_buf, n)) {
            for (i = 0; i < n * 2; i++) buf[i] += g_plug_buf[i];
            atomic_fetch_add_explicit(&g_tabs[t].callbacks, 1, memory_order_relaxed);
        }
    }
    if (n < frames)
        memset(buf + (size_t)n * 2, 0, (size_t)(frames - n) * 2 * sizeof *buf);
}

static void *audio_thread(void *ud)
{
    double *buf = malloc((size_t)PERIOD_MAX * 2 * sizeof *buf);
    short  *pcm = malloc((size_t)PERIOD_MAX * 2 * sizeof *pcm);
    int i;
    (void)ud;

    if (!buf || !pcm) return NULL;
    if (!g_teb_ready) { pehost_thread_init(); g_teb_ready = 1; }

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        long n;

        if (atomic_load_explicit(&g_park_req, memory_order_acquire)) {
            struct timespec ts = { 0, 2000000 };
            atomic_store_explicit(&g_parked, 1, memory_order_release);
            nanosleep(&ts, NULL);
            continue;
        }
        atomic_store_explicit(&g_parked, 0, memory_order_release);

        render_block(buf, g_period);

        for (i = 0; i < g_period * 2; i++) {
            double v = buf[i] * 32767.0;
            pcm[i] = (short)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
        }

        n = snd_pcm_writei(g_pcm, pcm, g_period);
        if (n < 0) {
            g_xruns++;
            if (snd_pcm_recover(g_pcm, (int)n, 1) < 0) snd_pcm_prepare(g_pcm);
        }
    }
    free(buf);
    free(pcm);
    return NULL;
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

static int engine_start_audio(void)
{
    if (g_pcm || g_pw_stream) return 0;

    if (strcmp(g_backend_want, "auto") && strcmp(g_backend_want, "pipewire") &&
        strcmp(g_backend_want, "alsa")) {
        fprintf(stderr, "audio: unknown backend '%s' (want auto, pipewire or alsa)"
                        " -- using auto\n", g_backend_want);
        g_backend_want = "auto";
    }

    /* PipeWire first: its callback runs on an RTKit-granted realtime thread,
     * which is the whole point. Fall back to ALSA if that fails. */
    if (strcmp(g_backend_want, "alsa")) {
        if (!engine_start_pipewire()) {
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
    if (snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        g_pcm = NULL;
        return -1;
    }
    if (snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           2, SR, 1, (unsigned)g_latency_us) < 0)
        return -1;
    fprintf(stderr, "audio: alsa, %d-frame blocks (%.1f ms), %d ms buffer\n",
            g_period, 1000.0 * g_period / SR, g_latency_us / 1000);
    atomic_store_explicit(&g_running, 1, memory_order_release);
    pthread_create(&g_thread, NULL, audio_thread, NULL);
    return 0;
}

/* Stop the audio callback touching the tabs so the GTK thread can add, load
 * into or free them. Both backends acknowledge through `parked`; which one is
 * live decides whether there is anyone to wait for. */
static void engine_park(void)
{
    int spins;

    if (!g_pcm && !g_pw_stream) return;      /* no audio running: nothing to park */

    /* Clear the acknowledgement before asking for it, or a stale `parked` left
     * over from the previous park is mistaken for this one. */
    atomic_store_explicit(&g_parked, 0, memory_order_relaxed);
    atomic_store_explicit(&g_park_req, 1, memory_order_release);

    /* Bounded wait: a suspended PipeWire node or a wedged device never calls
     * back, and spinning forever here would freeze the UI. */
    for (spins = 0; spins < 1000; spins++) {
        if (atomic_load_explicit(&g_parked, memory_order_acquire)) return;
        { struct timespec ts = { 0, 500000 }; nanosleep(&ts, NULL); }
    }
    fprintf(stderr, "audio: park timed out -- callback not running?\n");
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
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer u)
{
    (void)nb; (void)num; (void)u;
    release_tab(g_front);
    g_front = tab_of_page(page);
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
static void sink_deliver(void *ud, double wall, const trk_sink_ev *evs, int n)
{
    plugview *pv = ud;
    int i;
    for (i = 0; i < n; i++)
        plugview_inject_midi(pv, wall + (double)evs[i].frame / TRK_SINK_RATE,
                             evs[i].status, evs[i].d1, evs[i].d2);
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
    id = trk_add_sink(g_trk, name, &sink_deliver, tab->pv);
    if (id < 0) return;
    tab->sink_id = id;
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
    tab->used = 1;
    tab->sink_id = -1;
    tab->pv = plugview_new(engine_park, engine_unpark, SR, g_period);
    plugview_scan(tab->pv, NULL);
    tab->pane = plugview_pane(tab->pv);
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
    if (g_front == tab) g_front = NULL;
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
    g_tracker = trk_view_new(g_trk);
    g_tracker_page = trk_view_widget(g_tracker);
    trk_view_set_sinks(g_tracker, &g_sink_api, NULL);
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

static int open_song_path(const char *path)
{
    if (!open_tracker_tab()) return 0;
    if (trk_view_open(g_tracker, path)) {
        char msg[300];
        snprintf(msg, sizeof msg, "could not open %s", path);
        status(msg);
        return 0;
    }
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

static void act_quit(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a; (void)p; (void)u; gtk_window_close(GTK_WINDOW(g_win)); }

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

static void act_enter_key(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_enter_key(t->pv, GTK_WINDOW(g_win)); }

static void act_toggle_editor(GSimpleAction *a, GVariant *p, gpointer u)
{ synctab *t; (void)a; (void)p; (void)u;
  if ((t = synth_or_status())) plugview_toggle_editor(t->pv); }

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
        { "open-song",   act_open_song,   NULL, NULL, NULL, {0} },
        { "close-tab",   act_close_tab,   NULL, NULL, NULL, {0} },
        { "quit",        act_quit,        NULL, NULL, NULL, {0} },
        { "open-vst",    act_open_vst,    NULL, NULL, NULL, {0} },
        { "load-folder", act_load_folder, NULL, NULL, NULL, {0} },
        { "save-patch",  act_save_patch,  NULL, NULL, NULL, {0} },
        { "open-patch",  act_open_patch,  NULL, NULL, NULL, {0} },
        { "plugin-folders", act_plugin_folders, NULL, NULL, NULL, {0} },
        { "enter-key",   act_enter_key,   NULL, NULL, NULL, {0} },
        { "toggle-editor", act_toggle_editor, NULL, NULL, NULL, {0} },
        { "panic",       act_panic,       NULL, NULL, NULL, {0} },
        { "about",       act_about,       NULL, NULL, NULL, {0} },
    };
    GMenu *bar   = g_menu_new();
    GMenu *file  = g_menu_new();
    GMenu *sect  = g_menu_new();
    GMenu *synth = g_menu_new();
    GMenu *s2    = g_menu_new();
    GMenu *help  = g_menu_new();
    GtkWidget *w;

    g_action_map_add_action_entries(G_ACTION_MAP(g_win), entries,
                                    G_N_ELEMENTS(entries), NULL);
    g_new_tracker_act = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(g_win), "new-tracker"));
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
    g_menu_append(file, "Open song…",  "win.open-song");
    g_menu_append(sect, "Close tab",   "win.close-tab");
    g_menu_append(sect, "Quit",        "win.quit");
    g_menu_append_section(file, NULL, G_MENU_MODEL(sect));
    g_menu_append_submenu(bar, "File", G_MENU_MODEL(file));

    g_menu_append(synth, "Open VST…",   "win.open-vst");
    g_menu_append(synth, "Load Folder…", "win.load-folder");
    g_menu_append(synth, "Save Patch…", "win.save-patch");
    g_menu_append(synth, "Open Patch…", "win.open-patch");
    g_menu_append(s2, "Plug-in Folders…", "win.plugin-folders");
    g_menu_append(s2, "Enter Key / Serial…", "win.enter-key");
    g_menu_append_section(synth, NULL, G_MENU_MODEL(s2));
    g_menu_append(synth, "Parameters / Editor", "win.toggle-editor");
    g_menu_append(synth, "All Notes Off", "win.panic");
    g_menu_append_submenu(bar, "Synth", G_MENU_MODEL(synth));

    g_menu_append(help, "About studiogtk", "win.about");
    g_menu_append_submenu(bar, "Help", G_MENU_MODEL(help));

    w = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    g_object_unref(help); g_object_unref(s2); g_object_unref(synth);
    g_object_unref(sect); g_object_unref(file); g_object_unref(bar);
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

static gboolean on_win_close(GtkWindow *w, gpointer u)
{
    (void)w; (void)u;
    /* The tracker's song may have unsaved changes; closing the window asks
     * exactly as closing its tab does, and the answer closes the window. */
    if (g_tracker && !g_smoke_ms && trk_view_dirty(g_tracker)) {
        trk_view_confirm_close(g_tracker, (void (*)(void *))gtk_window_close, g_win);
        return TRUE;
    }
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
    gtk_box_append(GTK_BOX(outer), g_status);
    gtk_window_set_child(GTK_WINDOW(g_win), outer);

    /* The computer keyboard plays the front tab's piano; see on_key. The
     * controller is on the window, so a plug-in's editor has to be told
     * which keys to let past -- plugview_set_note_key, per tab. */
    kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed",  G_CALLBACK(on_key), NULL);
    g_signal_connect(kc, "key-released", G_CALLBACK(on_key_up), NULL);
    gtk_widget_add_controller(g_win, kc);
    g_signal_connect(g_win, "notify::is-active", G_CALLBACK(on_win_active), NULL);

    engine_start_audio();
    update_canvas();

    for (i = 0; i < g_nwant_synths; i++) {
        synctab *tab = add_synth_tab();
        if (tab && !plugview_load_path(tab->pv, g_want_synths[i])) {
            char msg[1100];
            snprintf(msg, sizeof msg, "could not load %s", g_want_synths[i]);
            status(msg);
        }
    }
    if (g_want_tracker) open_tracker_tab();
    if (g_want_song[0]) open_song_path(g_want_song);
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
        } else if (!strcmp(argv[i], "--route") && i + 1 < argc) {
            g_want_route = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--smoke") && i + 1 < argc) {
            g_smoke_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc) {
            g_quit_after = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            g_backend_want = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("studiogtk [--synth <plug-in>]... [--tracker] [--song <file.trk>]\n"
                   "          [--route <track>] [--smoke <ms>] [--quit-after <ms>]\n"
                   "          [--backend auto|pipewire|alsa]\n\n"
                   "The session window, in GTK: a tab per synth plug-in, one for "
                   "the tracker.\nWith no arguments it opens on a blank canvas.\n"
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
    atomic_store_explicit(&g_running, 0, memory_order_release);
    if (g_pcm) { pthread_join(g_thread, NULL); snd_pcm_close(g_pcm); }
    engine_stop_pipewire();
    pw_deinit();
    if (g_tracker) trk_view_free(g_tracker);  /* its widget died with the window */
    if (g_trk) trk_close(g_trk);
    for (i = 0; i < MAXTABS; i++)
        if (g_tabs[i].used) plugview_free(g_tabs[i].pv);
    if (g_xruns) fprintf(stderr, "audio: %lu ALSA underrun(s)\n", g_xruns);
    return status_rc;
}
