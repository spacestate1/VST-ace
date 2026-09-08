/* Play a plug-in live: audio out to PipeWire, MIDI in from the ALSA sequencer.
 *
 * The 32-bit loader has had this since it was written (play32.h) and the
 * 64-bit one never did, so the one arrangement people actually want -- a synth
 * running headless with a tracker or a sequencer driving it -- worked only for
 * 32-bit plug-ins. Everything here is what pehost.h already exposes, so it is
 * the same few calls whichever loader is underneath.
 *
 * Two halves, and the second is the point:
 *
 *   audio   one stereo PipeWire playback stream. `target` names a sink to
 *           connect to; without one PipeWire sends it wherever it sends music,
 *           which is right for listening and wrong for recording into
 *           something else.
 *
 *   MIDI    a writable, subscribable ALSA port, so anything on the machine can
 *           connect *to* this and play it. That is the direction that matters:
 *           `--midi` connecting outward to a keyboard is convenient, but a
 *           tracker wants to find a synth and drive it.
 *
 * The reader runs on its own thread for the reason midiio.cpp sets out at
 * length: an event's timestamp should be taken when it arrives.
 */
#ifndef PELOAD_LIVE_H
#define PELOAD_LIVE_H

#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "pehost.h"

typedef struct {
    pehost      *h;
    int          block;
    float       *buf;
    snd_seq_t   *seq;
    int          in_port;
    pthread_t    reader;
    volatile int stop;
} live_state;

static volatile sig_atomic_t g_live_quit;
static void live_on_signal(int s) { (void)s; g_live_quit = 1; }

/* Everything the sequencer hands us, turned back into the three bytes a plug-in
 * expects. The system-realtime messages are passed through as well, because a
 * plug-in's arpeggiator and its tempo delay are driven by the clock rather than
 * by the notes. */
static void live_dispatch(live_state *st, snd_seq_event_t *ev)
{
    int ch = ev->data.note.channel & 0x0f;
    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
        pehost_midi(st->h, (ev->data.note.velocity ? 0x90 : 0x80) | ch,
                    ev->data.note.note, ev->data.note.velocity);
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        pehost_midi(st->h, 0x80 | ch, ev->data.note.note, 0);
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        pehost_midi(st->h, 0xB0 | ch, ev->data.control.param & 0x7f,
                    ev->data.control.value & 0x7f);
        break;
    case SND_SEQ_EVENT_PGMCHANGE:
        pehost_midi(st->h, 0xC0 | ch, ev->data.control.value & 0x7f, 0);
        break;
    case SND_SEQ_EVENT_CHANPRESS:
        pehost_midi(st->h, 0xD0 | ch, ev->data.control.value & 0x7f, 0);
        break;
    case SND_SEQ_EVENT_KEYPRESS:
        pehost_midi(st->h, 0xA0 | ch, ev->data.note.note, ev->data.note.velocity);
        break;
    case SND_SEQ_EVENT_PITCHBEND: {
        int v = ev->data.control.value + 8192;      /* back to 0..16383 */
        if (v < 0) v = 0;
        if (v > 16383) v = 16383;
        pehost_midi(st->h, 0xE0 | ch, v & 0x7f, (v >> 7) & 0x7f);
        break;
    }
    case SND_SEQ_EVENT_SONGPOS: {
        int v = ev->data.control.value & 0x3FFF;
        pehost_midi(st->h, 0xF2, v & 0x7f, (v >> 7) & 0x7f);
        break;
    }
    case SND_SEQ_EVENT_CLOCK:    pehost_midi(st->h, 0xF8, 0, 0); break;
    case SND_SEQ_EVENT_START:    pehost_midi(st->h, 0xFA, 0, 0); break;
    case SND_SEQ_EVENT_CONTINUE: pehost_midi(st->h, 0xFB, 0, 0); break;
    case SND_SEQ_EVENT_STOP:     pehost_midi(st->h, 0xFC, 0, 0); break;
    default: break;
    }
}

static void *live_reader(void *ud)
{
    live_state *st = ud;
    pehost_thread_init();
    while (!st->stop) {
        int n = snd_seq_poll_descriptors_count(st->seq, POLLIN);
        struct pollfd pfd[8];
        snd_seq_event_t *ev = NULL;
        if (n <= 0 || n > 8) { struct timespec t = { 0, 5000000 }; nanosleep(&t, NULL); continue; }
        snd_seq_poll_descriptors(st->seq, pfd, (unsigned)n, POLLIN);
        /* A timeout rather than an indefinite wait, so stopping does not hang. */
        if (poll(pfd, (nfds_t)n, 50) <= 0) continue;
        while (snd_seq_event_input_pending(st->seq, 1) > 0) {
            if (snd_seq_event_input(st->seq, &ev) < 0 || !ev) break;
            live_dispatch(st, ev);
        }
    }
    return NULL;
}

/* One block: render, then interleave into whatever PipeWire handed us. The
 * plug-in is asked for exactly the frames the graph wants, so nothing here has
 * a buffer of its own to keep in step. */
static struct pw_stream *g_live_stream;

static void live_on_process(void *ud)
{
    live_state *st = ud;
    struct pw_buffer *b;
    struct spa_buffer *sb;
    float *dst;
    uint32_t want, i;

    /* PipeWire's realtime thread is a thread the plug-in has never seen, and
     * pehost.h is explicit that every one of them installs a TEB before
     * reaching plug-in code -- MSVC-generated code reads it through %gs on
     * entry. Without this the first render faults inside the plug-in with a
     * stack address, which looks like the plug-in's bug and is not. Once per
     * thread, so the cost is a thread-local test per block. */
    static __thread int teb_ready;
    if (!teb_ready) { pehost_thread_init(); teb_ready = 1; }

    if (!(b = pw_stream_dequeue_buffer(g_live_stream))) return;
    sb = b->buffer;
    if (!(dst = sb->datas[0].data)) { pw_stream_queue_buffer(g_live_stream, b); return; }

    want = sb->datas[0].maxsize / (2 * sizeof *dst);
    if (b->requested && b->requested < want) want = (uint32_t)b->requested;
    if (want > (uint32_t)st->block) want = (uint32_t)st->block;

    pehost_render_io(st->h, NULL, st->buf, (int)want);
    for (i = 0; i < want * 2; i++) dst[i] = st->buf[i];

    sb->datas[0].chunk->offset = 0;
    sb->datas[0].chunk->stride = (int32_t)(2 * sizeof *dst);
    sb->datas[0].chunk->size = want * 2 * (uint32_t)sizeof *dst;
    pw_stream_queue_buffer(g_live_stream, b);
}

/* Play until interrupted. Returns 0 when it ran, non-zero when it could not
 * start -- and says which half failed, because "live mode did not work" with a
 * sound server and a sequencer behind it is two very different problems. */
static int live_run(pehost *h, double rate, int block,
                    const char *midi_from, const char *sink)
{
    static const struct pw_stream_events ev = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = live_on_process,
    };
    live_state st;
    struct pw_thread_loop *loop;
    struct pw_properties *props;
    const struct spa_pod *params[1];
    uint8_t pod[1024];
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    struct spa_audio_info_raw info;
    char lat[64], me[64];
    int srate = (int)(rate > 0 ? rate : 48000);

    memset(&st, 0, sizeof st);
    st.h = h;
    st.block = block > 0 ? block : 512;
    if (!(st.buf = calloc((size_t)st.block * 2, sizeof *st.buf))) return 1;

    /* MIDI first: a port that exists before any sound comes out is one a
     * sequencer can already be connected to. */
    if (snd_seq_open(&st.seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        fprintf(stderr, "live: the ALSA sequencer is not available\n");
        free(st.buf);
        return 1;
    }
    snd_seq_set_client_name(st.seq, "peload");
    st.in_port = snd_seq_create_simple_port(st.seq, "peload in",
                     SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTHESIZER);
    if (st.in_port < 0) {
        fprintf(stderr, "live: could not create a MIDI port\n");
        snd_seq_close(st.seq); free(st.buf);
        return 1;
    }
    snprintf(me, sizeof me, "%d:%d", snd_seq_client_id(st.seq), st.in_port);
    /* Connecting outward is the convenience; being connectable is the feature. */
    if (midi_from && *midi_from) {
        snd_seq_addr_t src;
        if (snd_seq_parse_address(st.seq, &src, midi_from) == 0) {
            snd_seq_port_subscribe_t *sub;
            snd_seq_addr_t dst = { (unsigned char)snd_seq_client_id(st.seq),
                                   (unsigned char)st.in_port };
            snd_seq_port_subscribe_alloca(&sub);
            snd_seq_port_subscribe_set_sender(sub, &src);
            snd_seq_port_subscribe_set_dest(sub, &dst);
            if (snd_seq_subscribe_port(st.seq, sub) < 0)
                fprintf(stderr, "live: could not connect from %s\n", midi_from);
        } else {
            fprintf(stderr, "live: no such MIDI port: %s\n", midi_from);
        }
    }

    pw_init(NULL, NULL);
    if (!(loop = pw_thread_loop_new("peload", NULL))) {
        fprintf(stderr, "live: could not start PipeWire\n");
        snd_seq_close(st.seq); free(st.buf);
        return 1;
    }
    snprintf(lat, sizeof lat, "%d/%d", st.block, srate);
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                              PW_KEY_MEDIA_CATEGORY, "Playback",
                              PW_KEY_MEDIA_ROLE, "Music",
                              PW_KEY_NODE_LATENCY, lat, NULL);
    /* Naming a sink is what makes this useful for recording into something
     * else: without it the graph sends this where it sends music. */
    if (sink && *sink) pw_properties_set(props, PW_KEY_TARGET_OBJECT, sink);

    g_live_stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                         "peload", props, &ev, &st);
    if (!g_live_stream) {
        fprintf(stderr, "live: could not create the audio stream\n");
        pw_thread_loop_destroy(loop);
        snd_seq_close(st.seq); free(st.buf);
        return 1;
    }
    spa_zero(info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = (uint32_t)srate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    params[0] = spa_format_audio_raw_build(&bb, SPA_PARAM_EnumFormat, &info);
    if (pw_stream_connect(g_live_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
        fprintf(stderr, "live: could not connect the audio stream\n");
        pw_stream_destroy(g_live_stream);
        pw_thread_loop_destroy(loop);
        snd_seq_close(st.seq); free(st.buf);
        return 1;
    }

    pthread_create(&st.reader, NULL, live_reader, &st);
    pw_thread_loop_start(loop);

    printf("playing %s\n"
           "  MIDI in   %s   (\"peload\":\"peload in\")\n"
           "  audio out %s\n"
           "  connect a sequencer to it and play; ctrl-c to stop\n",
           pehost_name(h), me, sink && *sink ? sink : "the default sink");
    fflush(stdout);

    signal(SIGINT, live_on_signal);
    signal(SIGTERM, live_on_signal);
    while (!g_live_quit) { struct timespec t = { 0, 100000000 }; nanosleep(&t, NULL); }

    st.stop = 1;
    pthread_join(st.reader, NULL);
    pw_thread_loop_stop(loop);
    pw_stream_destroy(g_live_stream);
    pw_thread_loop_destroy(loop);
    snd_seq_close(st.seq);
    free(st.buf);
    printf("\nstopped\n");
    return 0;
}

#endif /* PELOAD_LIVE_H */
