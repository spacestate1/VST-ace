/* Live playback for the i386 loader: PipeWire out, ALSA sequencer MIDI in.
 *
 * Included by pe32.c and only compiled when the 32-bit PipeWire and ALSA
 * libraries are present (PELOAD32_AUDIO). Kept separate because none of it is
 * loader business -- pe32.c stays about mapping and calling a PE image.
 *
 * The one thing here that is loader business: PipeWire's realtime thread is not
 * the thread that loaded the plugin, and guest code reads its TEB through %fs.
 * That thread therefore has to install its own TEB before the first
 * processReplacing, or the plugin's TLS lookups read whatever %fs happened to
 * hold. See play_teb_once().
 */
#ifndef PELOAD_PLAY32_H
#define PELOAD_PLAY32_H

#include <stdatomic.h>
#include <poll.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <alsa/asoundlib.h>

#define PLAY_SR       48000
#define PLAY_PERIOD   512
#define PLAY_MAXEV    256

/* --------------------------------------------------- MIDI -> audio handoff */

/* Single-producer (ALSA thread) single-consumer (RT thread) ring. Plain
 * relaxed/acquire-release is enough: only the indices are shared. */
typedef struct { unsigned char d[4]; } play_msg;

typedef struct {
    play_msg          q[PLAY_MAXEV];
    _Atomic unsigned  head, tail;
} play_ring;

static int play_push(play_ring *r, const unsigned char *m, int n)
{
    unsigned h = atomic_load_explicit(&r->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&r->tail, memory_order_acquire);
    int i;
    if (h - t >= PLAY_MAXEV) return 0;                 /* full: drop */
    for (i = 0; i < 4; i++) r->q[h % PLAY_MAXEV].d[i] = i < n ? m[i] : 0;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 1;
}
static int play_pop(play_ring *r, play_msg *out)
{
    unsigned t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (t == atomic_load_explicit(&r->head, memory_order_acquire)) return 0;
    *out = r->q[t % PLAY_MAXEV];
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 1;
}

/* ------------------------------------------------------------------ engine */

typedef struct {
    AEffect32  *fx;
    int         nin, nout;
    float     **ins, **outs;
    float       gain;
    play_ring   midi;

    struct pw_thread_loop *loop;
    struct pw_stream      *stream;

    /* Running output peak, so "is it actually making sound?" is answerable
     * without capturing the graph. Written by the RT thread, read by the main
     * thread -- relaxed atomics, because it is a level meter. */
    _Atomic uint32_t peak_bits;
    _Atomic uint64_t blocks;

    snd_seq_t  *seq;
    int         seq_port;
    pthread_t   seq_thread;
    volatile int seq_stop;

    /* One VstEvents block, allocated once and reused every callback: the
     * pointer array must be contiguous with the header, so it cannot be a
     * separate member. See VSTEVENTS32_BYTES. */
    void          *evbuf;
    VstMidiEvent32 evm[PLAY_MAXEV];
} play_engine;

/* The engine play_start brought up, so the level can be reported. */
static play_engine *g_play_engine;

/* The RT thread is not the loading thread, and guest TLS reads go through %fs. */
static void play_teb_once(void)
{
    static __thread int done;
    if (!done) { done = 1; if (teb_install()) fprintf(stderr, "play: no TEB on RT thread\n"); }
}

static void play_feed_midi(play_engine *e)
{
    play_msg m;
    int n = 0;

    while (n < PLAY_MAXEV && play_pop(&e->midi, &m)) {
        VstMidiEvent32 *ev = &e->evm[n];
        memset(ev, 0, sizeof *ev);
        ev->type = 1;                       /* kVstMidiType */
        ev->byteSize = sizeof *ev;
        ev->midiData[0] = (char)m.d[0];
        ev->midiData[1] = (char)m.d[1];
        ev->midiData[2] = (char)m.d[2];
        vstevents32_array(e->evbuf)[n] = ev;
        n++;
    }
    if (!n) return;
    {
        VstEvents32 *ve = e->evbuf;
        ve->numEvents = n;
        ve->reserved  = 0;
        e->fx->dispatcher(e->fx, effProcessEvents, 0, 0, ve, 0.0f);
    }
}

static void play_on_process(void *ud)
{
    play_engine *e = ud;
    struct pw_buffer *b;
    struct spa_buffer *sb;
    float *dst;
    int n, i;

    play_teb_once();

    if (!(b = pw_stream_dequeue_buffer(e->stream))) return;
    sb = b->buffer;
    if (!(dst = sb->datas[0].data)) { pw_stream_queue_buffer(e->stream, b); return; }

    n = (int)(sb->datas[0].maxsize / (sizeof(float) * 2));
    if (b->requested && (int)b->requested < n) n = (int)b->requested;
    if (n > PLAY_PERIOD) n = PLAY_PERIOD;

    play_feed_midi(e);

    for (i = 0; i < e->nin; i++)  memset(e->ins[i],  0, (size_t)n * sizeof **e->ins);
    for (i = 0; i < e->nout; i++) memset(e->outs[i], 0, (size_t)n * sizeof **e->outs);
    e->fx->processReplacing(e->fx, e->nin ? e->ins : NULL, e->outs, n);

    for (i = 0; i < n; i++) {
        float l = e->outs[0][i] * e->gain;
        float r = (e->nout >= 2 ? e->outs[1][i] : e->outs[0][i]) * e->gain;
        dst[i * 2]     = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        dst[i * 2 + 1] = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
    }

    {   /* track the peak of what we just emitted */
        float pk = 0.0f;
        uint32_t bits;
        for (i = 0; i < n * 2; i++) { float a = dst[i] < 0 ? -dst[i] : dst[i]; if (a > pk) pk = a; }
        memcpy(&bits, &pk, 4);
        if (pk > 0.0f) {
            uint32_t old = atomic_load_explicit(&e->peak_bits, memory_order_relaxed);
            float oldf;
            memcpy(&oldf, &old, 4);
            if (pk > oldf) atomic_store_explicit(&e->peak_bits, bits, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&e->blocks, 1, memory_order_relaxed);
    }

    sb->datas[0].chunk->offset = 0;
    sb->datas[0].chunk->stride = sizeof(float) * 2;
    sb->datas[0].chunk->size   = (uint32_t)(n * 2 * sizeof(float));
    pw_stream_queue_buffer(e->stream, b);
}

static const struct pw_stream_events g_play_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = play_on_process,
};

static void play_stop(play_engine *e)
{
    if (e->seq) {
        e->seq_stop = 1;
        pthread_join(e->seq_thread, NULL);
        snd_seq_close(e->seq);
        e->seq = NULL;
    }
    /* Stop the loop before destroying the stream: once it returns, no callback
     * is in flight, so nothing is inside the plugin while we tear it down. */
    if (e->loop)   pw_thread_loop_stop(e->loop);
    if (e->stream) { pw_stream_destroy(e->stream); e->stream = NULL; }
    if (e->loop)   { pw_thread_loop_destroy(e->loop); e->loop = NULL; }
}

/* ----------------------------------------------------------- ALSA MIDI in */

static void *play_seq_thread(void *ud)
{
    play_engine *e = ud;
    struct pollfd pfd[8];
    int nfd = snd_seq_poll_descriptors_count(e->seq, POLLIN);

    if (nfd < 1 || nfd > (int)(sizeof pfd / sizeof *pfd)) return NULL;
    snd_seq_poll_descriptors(e->seq, pfd, (unsigned)nfd, POLLIN);

    while (!e->seq_stop) {
        if (poll(pfd, (unsigned)nfd, 100) < 0) continue;
        /* Drain on pending, not on the fd: one wakeup can carry several events
         * and reading only one leaves the rest queued until the next note. */
        while (snd_seq_event_input_pending(e->seq, 1) > 0) {
            snd_seq_event_t *ev = NULL;
            unsigned char m[3];
            if (snd_seq_event_input(e->seq, &ev) < 0 || !ev) break;
            switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                m[0] = 0x90 | (ev->data.note.channel & 0x0f);
                m[1] = ev->data.note.note;
                m[2] = ev->data.note.velocity;
                play_push(&e->midi, m, 3);
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                m[0] = 0x80 | (ev->data.note.channel & 0x0f);
                m[1] = ev->data.note.note;
                m[2] = ev->data.note.velocity;
                play_push(&e->midi, m, 3);
                break;
            case SND_SEQ_EVENT_CONTROLLER:
                m[0] = 0xb0 | (ev->data.control.channel & 0x0f);
                m[1] = (unsigned char)ev->data.control.param;
                m[2] = (unsigned char)ev->data.control.value;
                play_push(&e->midi, m, 3);
                break;
            case SND_SEQ_EVENT_PITCHBEND: {
                int v = ev->data.control.value + 8192;
                m[0] = 0xe0 | (ev->data.control.channel & 0x0f);
                m[1] = (unsigned char)(v & 0x7f);
                m[2] = (unsigned char)((v >> 7) & 0x7f);
                play_push(&e->midi, m, 3);
                break;
            }
            case SND_SEQ_EVENT_PGMCHANGE:
                m[0] = 0xc0 | (ev->data.control.channel & 0x0f);
                m[1] = (unsigned char)ev->data.control.value;
                m[2] = 0;
                play_push(&e->midi, m, 3);
                break;
            default: break;
            }
        }
    }
    return NULL;
}

static int play_open_midi(play_engine *e, const char *connect_to)
{
    if (snd_seq_open(&e->seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        fprintf(stderr, "play: no ALSA sequencer; running without MIDI in\n");
        e->seq = NULL;
        return -1;
    }
    snd_seq_set_client_name(e->seq, "peload32");
    e->seq_port = snd_seq_create_simple_port(e->seq, "in",
                    SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                    SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (e->seq_port < 0) { snd_seq_close(e->seq); e->seq = NULL; return -1; }

    if (connect_to) {
        snd_seq_addr_t a;
        if (snd_seq_parse_address(e->seq, &a, connect_to) == 0 &&
            snd_seq_connect_from(e->seq, e->seq_port, a.client, a.port) == 0)
            printf("  midi: connected to %s\n", connect_to);
        else
            fprintf(stderr, "  midi: cannot connect to \"%s\"\n", connect_to);
    } else {
        /* Subscribe to every hardware keyboard we can find, so a USB keyboard
         * just works without the user hunting for its port number. */
        snd_seq_client_info_t *ci;
        snd_seq_port_info_t   *pi;
        int found = 0;
        snd_seq_client_info_alloca(&ci);
        snd_seq_port_info_alloca(&pi);
        snd_seq_client_info_set_client(ci, -1);
        while (snd_seq_query_next_client(e->seq, ci) >= 0) {
            int cl = snd_seq_client_info_get_client(ci);
            if (cl == snd_seq_client_id(e->seq)) continue;
            snd_seq_port_info_set_client(pi, cl);
            snd_seq_port_info_set_port(pi, -1);
            while (snd_seq_query_next_port(e->seq, pi) >= 0) {
                unsigned caps = snd_seq_port_info_get_capability(pi);
                if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                        != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                    continue;
                if (snd_seq_connect_from(e->seq, e->seq_port, cl,
                                         snd_seq_port_info_get_port(pi)) == 0) {
                    printf("  midi: %d:%d  %s\n", cl,
                           snd_seq_port_info_get_port(pi),
                           snd_seq_client_info_get_name(ci));
                    found++;
                }
            }
        }
        if (!found) printf("  midi: no keyboards found; connect one to \"peload32\"\n");
    }

    e->seq_stop = 0;
    if (pthread_create(&e->seq_thread, NULL, play_seq_thread, e) != 0) {
        snd_seq_close(e->seq); e->seq = NULL; return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ driver */

/* Bring the stream up and return, leaving audio running on PipeWire's thread.
 * play_run() is this plus a park loop, for when there is no GUI to run. */
static int play_start(AEffect32 *fx, int argc, char **argv, const char *midi_port)
{
    static play_engine e;
    const struct spa_pod *params[1];
    uint8_t pod[1024];
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    struct spa_audio_info_raw info;
    char lat[64];
    int i, nchan;

    memset(&e, 0, sizeof e);
    e.fx   = fx;
    e.gain = 0.7f;
    e.nin  = fx->numInputs;
    e.nout = fx->numOutputs < 1 ? 1 : fx->numOutputs;
    if (e.nin < 0 || e.nin > 256 || e.nout > 256) {
        fprintf(stderr, "implausible channel count\n");
        return 1;
    }
    nchan  = e.nin > e.nout ? e.nin : e.nout;
    e.ins  = calloc((size_t)nchan + 1, sizeof *e.ins);
    e.outs = calloc((size_t)nchan + 1, sizeof *e.outs);
    if (!e.ins || !e.outs) return 1;
    for (i = 0; i < e.nin; i++)
        if (!(e.ins[i] = calloc(PLAY_PERIOD, sizeof **e.ins))) return 1;
    for (i = 0; i < e.nout; i++)
        if (!(e.outs[i] = calloc(PLAY_PERIOD, sizeof **e.outs))) return 1;
    if (!(e.evbuf = calloc(1, VSTEVENTS32_BYTES(PLAY_MAXEV)))) return 1;

    pw_init(&argc, &argv);

    if (!(e.loop = pw_thread_loop_new("peload32", NULL))) {
        fprintf(stderr, "play: no PipeWire loop\n");
        return 1;
    }
    snprintf(lat, sizeof lat, "%d/%d", PLAY_PERIOD, PLAY_SR);
    e.stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(e.loop), "peload32",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Music",
                          PW_KEY_NODE_LATENCY, lat,
                          NULL),
        &g_play_events, &e);
    if (!e.stream) { play_stop(&e); fprintf(stderr, "play: no stream\n"); return 1; }

    spa_zero(info);
    info.format   = SPA_AUDIO_FORMAT_F32;
    info.rate     = PLAY_SR;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    params[0] = spa_format_audio_raw_build(&bb, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(e.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
        play_stop(&e); fprintf(stderr, "play: cannot connect stream\n"); return 1;
    }
    if (pw_thread_loop_start(e.loop) < 0) {
        play_stop(&e); fprintf(stderr, "play: cannot start loop\n"); return 1;
    }

    g_play_engine = &e;
    play_open_midi(&e, midi_port);

    printf("\nplaying -- %d in, %d out at %d Hz, %d-frame period.\n",
           e.nin, e.nout, PLAY_SR, PLAY_PERIOD);
    fflush(stdout);
    return 0;
}

void play_level(double *peak, unsigned long long *blocks)
{
    if (peak)   *peak = 0.0;
    if (blocks) *blocks = 0;
    if (!g_play_engine) return;
    {
        uint32_t bits = atomic_load_explicit(&g_play_engine->peak_bits,
                                             memory_order_relaxed);
        float f;
        memcpy(&f, &bits, 4);
        if (peak) *peak = f;
    }
    if (blocks) *blocks = atomic_load_explicit(&g_play_engine->blocks,
                                               memory_order_relaxed);
}

static int play_run(AEffect32 *fx, int argc, char **argv, const char *midi_port)
{
    if (play_start(fx, argc, argv, midi_port)) return 1;
    printf("Ctrl-C to stop.  (level is printed every 2 s)\n");
    fflush(stdout);
    for (;;) {
        struct timespec ts = { 2, 0 };
        double pk; unsigned long long blk;
        nanosleep(&ts, NULL);
        play_level(&pk, &blk);
        printf("  %llu blocks rendered, peak so far %.4f\n", blk, pk);
        fflush(stdout);
    }
}

#endif /* PELOAD_PLAY32_H */
