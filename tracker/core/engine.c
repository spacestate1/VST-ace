/* The engine: ALSA sequencer ports, a queue, and a thread that keeps a short
 * stretch of the song scheduled ahead of the queue.
 *
 * Every event goes out timestamped on the queue rather than at the moment a
 * thread gets round to it, so the kernel does the timing: a window busy
 * drawing, or this process being descheduled for a few milliseconds, moves
 * nothing. The thread only has to stay ahead -- it wakes every few
 * milliseconds and tops the schedule up to LOOKAHEAD_S in front of the queue.
 * The window is short so that an edit just ahead of the play position is
 * still heard.
 *
 * Stuck notes are the failure that matters, so every note this program starts
 * is remembered -- per track, per channel -- until a release has been sent
 * for it directly, outside the queue. Stop, routing a track elsewhere, panic
 * and closing all send those, whatever was scheduled and then thrown away.
 *
 * A track can play a sample set instead of a window: a folder of WAVs, each
 * on a note, as its kit.txt says or from C-4 up. Its hits go on the same
 * queue clock as everything else, into a list the audio thread reads: each
 * period it works out when every pending hit falls -- the queue's tick turned
 * into wall time, plus the output's own latency -- and starts it at that
 * sample. So samples keep time with the MIDI tracks to within the queue
 * timer's millisecond, with nothing in between. */
#include "trk.h"
#include "drumkit.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TRK_PPQ       960            /* divisible by every lpb trk_lpb_ok allows, and by 24 */
#define CLOCK_TICKS   (TRK_PPQ / 24) /* MIDI clock is 24 per quarter note */
#define LOOKAHEAD_S   0.12
#define START_S       0.01           /* play to first row */
#define POS_RING      512
#define OUTPUT_POOL   4000           /* kernel cells for events scheduled ahead */
#define KIT_RATE      48000
#define SEV_MAX       1024           /* kit hits scheduled and not yet played */
#define KIT_RESCAN_S  5.0            /* how stale the list of kits may get */

/* Something for the audio thread to do, at a queue tick or as soon as it can
 * (a preview, a panic): start a sample on a track, fade out what a track is
 * playing, or silence everything. */
enum { SEV_PLAY, SEV_AUDITION, SEV_FADE, SEV_SILENCE };
typedef struct { unsigned tick; int asap, op, kit, note, vel; } sample_ev;

typedef struct { char name[TRK_PATH_LEN]; drumkit *dk; } kit_slot;

/* One row as scheduled: when, where, and what each track was holding just
 * before it -- so the schedule can be wound back to any row still ahead of the
 * queue and played again from there with the right notes to release. */
typedef struct {
    unsigned tick;
    int      order, pattern, row;
    int      held[TRK_TRACKS], held_ch[TRK_TRACKS];
} pos_mark;

struct trk_engine {
    snd_seq_t      *seq;
    int             client;
    char            name[64];
    int             port[TRK_TRACKS];
    int             clock_port;
    int             queue;

    pthread_mutex_t lock;            /* the song, the play state and the seq handle */
    pthread_t       thread;
    int             quit;
    trk_song        song;

    /* Routing as last made. */
    int             routed[TRK_TRACKS];
    snd_seq_addr_t  dest[TRK_TRACKS];
    snd_seq_addr_t  clock_dest[TRK_TRACKS];
    int             nclock;

    /* Playback. */
    int             playing, mode;
    int             order, pattern, row;  /* the next row to schedule */
    unsigned        next_tick, next_clock;
    unsigned        clock_base;           /* tick of the first clock */
    int             held[TRK_TRACKS];     /* note the schedule has sounding, or -1 */
    int             held_ch[TRK_TRACKS];
    pos_mark        pos[POS_RING];
    unsigned        npos;

    int             prev_note[TRK_TRACKS], prev_ch[TRK_TRACKS];

    /* Every note started and not yet released directly: [track][channel]
     * as a 128-bit set. Cleared only by release_track. */
    unsigned char   touched[TRK_TRACKS][16][16];
    unsigned short  chans_used[TRK_TRACKS];

    /* The sample set. Changed under both locks, lock then smx; the audio
     * thread takes smx alone, so it never waits on the scheduler. */
    pthread_mutex_t smx;
    kit_slot        kit[TRK_TRACKS];      /* one per distinct set the tracks play */
    int             nkit;
    int             track_kit[TRK_TRACKS];/* index into kit, or -1 */
    char            stale[TRK_PATH_LEN];  /* a set to read again: it was edited */
    drumkit        *aud;                  /* one pad, being listened to in an editor */
    char            added[TRK_TRACKS][TRK_PATH_LEN]; /* sets loaded from elsewhere */
    int             nadded;
    sample_ev       sev[SEV_MAX];
    int             nsev;
    drumkit_place   places[DK_MAX_KITS];  /* every set there is, as last scanned */
    int             nplaces;
    double          scanned_at;

    /* Their output. */
    snd_pcm_t      *pcm;
    pthread_t       athread;
    int             audio_on, aquit, aperiod;
    double          alat;                 /* seconds from a hit's time to its sound */
    double          bpm_now;              /* the queue's tempo, for the audio thread */
    char            audio_msg[160];
};

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int is_sampled(const trk_track *k) { return k->samples[0] != 0; }

/* Work for the audio thread. Dropped when the list is full: a missed hit, or
 * a release missed -- and every sample ends by itself, so nothing is left
 * sounding for good. */
static void push_sample(trk_engine *e, unsigned tick, int asap, int op, int kit,
                        int note, int vel)
{
    pthread_mutex_lock(&e->smx);
    if (e->nsev < SEV_MAX) {
        sample_ev *v = &e->sev[e->nsev++];
        v->tick = tick; v->asap = asap; v->op = op; v->kit = kit;
        v->note = note; v->vel = vel;
    }
    pthread_mutex_unlock(&e->smx);
}

/* Pending hits after `tick` gone -- or all of them. */
static void drop_samples(trk_engine *e, int all, unsigned tick)
{
    int i, j;
    pthread_mutex_lock(&e->smx);
    for (i = j = 0; i < e->nsev; i++)
        if (!all && (e->sev[i].asap || e->sev[i].tick <= tick)) e->sev[j++] = e->sev[i];
    e->nsev = j;
    pthread_mutex_unlock(&e->smx);
}

/* ------------------------------------------------------------ events out */

static void ev_base(trk_engine *e, snd_seq_event_t *ev, int port)
{
    (void)e;
    snd_seq_ev_clear(ev);
    snd_seq_ev_set_source(ev, (unsigned char)port);
    snd_seq_ev_set_subs(ev);
}

static void at_tick(trk_engine *e, snd_seq_event_t *ev, unsigned tick)
{
    snd_seq_ev_schedule_tick(ev, e->queue, 0, tick);
    snd_seq_event_output(e->seq, ev);
}

static void now(trk_engine *e, snd_seq_event_t *ev)
{
    snd_seq_ev_set_direct(ev);
    snd_seq_event_output(e->seq, ev);
}

static void mark(trk_engine *e, int t, int ch, int note)
{
    e->touched[t][ch][note >> 3] |= (unsigned char)(1u << (note & 7));
    e->chans_used[t] |= (unsigned short)(1u << ch);
}

/* Release, directly, everything track t may have sounding: a note-off for
 * every note it started, then CC 123 and 120 on each channel it used. Extra
 * note-offs cost nothing; a missing one is a note that plays until someone
 * finds the window it is in. */
static void release_track(trk_engine *e, int t)
{
    snd_seq_event_t ev;
    int ch, n;

    for (ch = 0; ch < 16; ch++) {
        if (!(e->chans_used[t] & (1u << ch))) continue;
        for (n = 0; n < 128; n++) {
            if (!(e->touched[t][ch][n >> 3] & (1u << (n & 7)))) continue;
            ev_base(e, &ev, e->port[t]);
            snd_seq_ev_set_noteoff(&ev, ch, n, 0);
            now(e, &ev);
        }
        ev_base(e, &ev, e->port[t]);
        snd_seq_ev_set_controller(&ev, ch, 123, 0);
        now(e, &ev);
        ev_base(e, &ev, e->port[t]);
        snd_seq_ev_set_controller(&ev, ch, 120, 0);
        now(e, &ev);
    }
    memset(e->touched[t], 0, sizeof e->touched[t]);
    e->chans_used[t] = 0;
    e->held[t] = -1;
    e->prev_note[t] = -1;
    snd_seq_drain_output(e->seq);
}

/* ------------------------------------------------------------- the queue */

static unsigned queue_tick(trk_engine *e)
{
    snd_seq_queue_status_t *st;
    snd_seq_queue_status_alloca(&st);
    if (snd_seq_get_queue_status(e->seq, e->queue, st) < 0) return 0;
    return (unsigned)snd_seq_queue_status_get_tick_time(st);
}

static void set_tempo(trk_engine *e, double bpm)
{
    unsigned us = (unsigned)(60000000.0 / (bpm > 0 ? bpm : 120.0));
    e->bpm_now = bpm > 0 ? bpm : 120.0;
    snd_seq_change_queue_tempo(e->seq, e->queue, us, NULL);
    snd_seq_drain_output(e->seq);
}

/* Everything scheduled and not yet delivered, gone -- from the kernel's
 * queue and from our own output buffer. What it would have released is
 * covered by release_track, which is why this is never done without it. */
static void unschedule(trk_engine *e)
{
    snd_seq_remove_events_t *rm;
    snd_seq_drop_output(e->seq);
    snd_seq_remove_events_alloca(&rm);
    /* Output alone matches every event this client has queued. */
    snd_seq_remove_events_set_condition(rm, SND_SEQ_REMOVE_OUTPUT);
    snd_seq_remove_events_set_queue(rm, e->queue);
    snd_seq_remove_events(e->seq, rm);
}

/* ------------------------------------------------------------ scheduling */

static int row_pattern(trk_engine *e)
{
    if (e->mode == TRK_PLAY_SONG) {
        int o = e->order;
        if (o < 0 || o >= e->song.norder) o = 0;
        return e->song.order[o];
    }
    return e->pattern;
}

static void schedule_row(trk_engine *e)
{
    trk_song *s = &e->song;
    int p = row_pattern(e), t;
    trk_pattern *pt;
    snd_seq_event_t ev;

    if (p < 0 || p >= TRK_PATTERNS) p = 0;
    pt = &s->pattern[p];
    if (e->row >= pt->rows) e->row = 0;      /* shortened under us */

    {
        pos_mark *m = &e->pos[e->npos++ % POS_RING];
        m->tick = e->next_tick;
        m->order = e->mode == TRK_PLAY_SONG ? e->order : -1;
        m->pattern = p;
        m->row = e->row;
        memcpy(m->held, e->held, sizeof m->held);
        memcpy(m->held_ch, e->held_ch, sizeof m->held_ch);
    }

    for (t = 0; t < TRK_TRACKS; t++) {
        const trk_cell *c = &pt->cell[e->row][t];
        const trk_track *k = &s->track[t];
        int ch = k->channel & 15;

        /* A sample set: the note picks the pad, and a hit plays out -- a
         * drum has no note to end, and a controller has nowhere to go. */
        if (is_sampled(k)) {
            if (!k->mute && c->note <= 127 && e->track_kit[t] >= 0) {
                int vel = c->vel != TRK_EMPTY ? c->vel : k->velocity;
                push_sample(e, e->next_tick, 0, SEV_PLAY, e->track_kit[t], c->note,
                            vel < 1 ? 1 : vel > 127 ? 127 : vel);
            }
            continue;
        }

        if (k->mute) {
            /* Muting releases what was playing rather than leaving it to
             * ring on under a track that says it is silent. */
            if (e->held[t] >= 0) {
                ev_base(e, &ev, e->port[t]);
                snd_seq_ev_set_noteoff(&ev, e->held_ch[t], e->held[t], 0);
                at_tick(e, &ev, e->next_tick);
                e->held[t] = -1;
            }
            continue;
        }
        if (c->cc != TRK_EMPTY && c->val != TRK_EMPTY) {
            ev_base(e, &ev, e->port[t]);
            snd_seq_ev_set_controller(&ev, ch, c->cc, c->val);
            at_tick(e, &ev, e->next_tick);
        }
        if (c->note == TRK_EMPTY) continue;
        /* One note per track, as in a tracker: anything new, or a note-off,
         * ends what the track was holding -- on the channel it started on,
         * which is not necessarily the track's channel now. */
        if (e->held[t] >= 0) {
            ev_base(e, &ev, e->port[t]);
            snd_seq_ev_set_noteoff(&ev, e->held_ch[t], e->held[t], 0);
            at_tick(e, &ev, e->next_tick);
            e->held[t] = -1;
        }
        if (c->note <= 127) {
            int vel = c->vel != TRK_EMPTY ? c->vel : k->velocity;
            if (vel < 1) vel = 1;
            if (vel > 127) vel = 127;
            ev_base(e, &ev, e->port[t]);
            snd_seq_ev_set_noteon(&ev, ch, c->note, vel);
            at_tick(e, &ev, e->next_tick);
            e->held[t] = c->note;
            e->held_ch[t] = ch;
            mark(e, t, ch, c->note);
        }
    }

    e->next_tick += TRK_PPQ / (trk_lpb_ok(s->lpb) ? s->lpb : 4);
    if (++e->row >= pt->rows) {
        e->row = 0;
        if (e->mode == TRK_PLAY_SONG && ++e->order >= s->norder) e->order = 0;
    }
}

/* Schedule everything up to LOOKAHEAD_S in front of the queue. */
static void top_up(trk_engine *e)
{
    unsigned cur = queue_tick(e);
    double bpm = e->song.bpm > 0 ? e->song.bpm : 120.0;
    unsigned horizon = cur + (unsigned)(LOOKAHEAD_S * bpm / 60.0 * TRK_PPQ);
    int guard = 0;
    while (e->next_tick <= horizon && guard++ < 64) schedule_row(e);
    while (e->next_clock <= horizon) {
        snd_seq_event_t ev;
        ev_base(e, &ev, e->clock_port);
        ev.type = SND_SEQ_EVENT_CLOCK;
        at_tick(e, &ev, e->next_clock);
        e->next_clock += CLOCK_TICKS;
    }
    snd_seq_drain_output(e->seq);
}

static void *sched_thread(void *ud)
{
    trk_engine *e = ud;
    for (;;) {
        struct timespec ts = { 0, 4000000 };
        pthread_mutex_lock(&e->lock);
        if (e->quit) { pthread_mutex_unlock(&e->lock); break; }
        if (e->playing) top_up(e);
        pthread_mutex_unlock(&e->lock);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* Take back everything scheduled that has not played yet, and set the
 * schedule to carry on from the first row still ahead of the queue, holding
 * what was held before that row. Whatever is released next is then released
 * against what is really sounding: without this, note-ons already on the
 * queue would arrive after a release meant to end them -- and, once a track
 * has been routed elsewhere, at the new window -- with nothing left that
 * knows to end them. The caller tops the schedule up again afterwards.
 *
 * The queue's position is read before the events are removed, so a row
 * played in between is played again rather than lost: a note sounding twice
 * is harmless, a lost row may be the release of one. */
static void rewind_locked(trk_engine *e)
{
    unsigned cur = queue_tick(e), i, n, back = 0;

    unschedule(e);
    drop_samples(e, 0, cur);
    n = e->npos < POS_RING ? e->npos : POS_RING;
    for (i = 1; i <= n; i++) {
        if (e->pos[(e->npos - i) % POS_RING].tick <= cur) break;
        back = i;
    }
    if (back) {
        const pos_mark *m = &e->pos[(e->npos - back) % POS_RING];
        e->next_tick = m->tick;
        e->row = m->row;
        if (e->mode == TRK_PLAY_SONG) e->order = m->order;
        memcpy(e->held, m->held, sizeof e->held);
        memcpy(e->held_ch, m->held_ch, sizeof e->held_ch);
        e->npos -= back;
    }
    /* The first clock not yet delivered. */
    if (cur < e->clock_base) e->next_clock = e->clock_base;
    else e->next_clock = e->clock_base + ((cur - e->clock_base) / CLOCK_TICKS + 1) * CLOCK_TICKS;
}

/* ----------------------------------------------------------------- kits */

typedef struct { int off; sample_ev v; } due_ev;

/* As the audio thread does it. Under smx. */
static void apply_due(trk_engine *e, const sample_ev *v)
{
    int i;
    switch (v->op) {
    case SEV_PLAY:
        if (v->kit >= 0 && v->kit < e->nkit) drumkit_note_on(e->kit[v->kit].dk, v->note, v->vel);
        break;
    case SEV_AUDITION:
        drumkit_note_on(e->aud, v->note, v->vel);
        break;
    case SEV_FADE:
        for (i = 0; i < e->nkit; i++) drumkit_fade_all(e->kit[i].dk);
        drumkit_fade_all(e->aud);
        break;
    default:
        for (i = 0; i < e->nkit; i++) drumkit_all_off(e->kit[i].dk);
        drumkit_all_off(e->aud);
        break;
    }
}

/* One period at a time: which pending hits fall inside it, and where; then
 * every kit rendered, split at those points so each hit starts on its own
 * sample. A hit's time is its tick turned into wall time, plus alat -- the
 * output's buffer -- so that one scheduled just ahead of the queue is never
 * already late; the period's own time is now plus what the device still
 * holds. Late anyway (an underrun), it plays at the period's start. */
static void *audio_main(void *ud)
{
    trk_engine *e = ud;
    const int P = e->aperiod;
    double *mix = calloc((size_t)P * 2, sizeof *mix);
    double *tmp = calloc((size_t)P * 2, sizeof *tmp);
    short  *out = calloc((size_t)P * 2, sizeof *out);
    due_ev *due = calloc(SEV_MAX, sizeof *due);

    while (mix && tmp && out && due && !e->aquit) {
        snd_pcm_sframes_t delay = 0;
        double now, pwall, spt;
        unsigned cur;
        int i, j, nd = 0, a, k;
        snd_pcm_sframes_t w;

        if (snd_pcm_delay(e->pcm, &delay) < 0 || delay < 0) delay = 0;
        now = mono_now();
        pwall = now + (double)delay / KIT_RATE;
        cur = queue_tick(e);
        spt = 60.0 / ((e->bpm_now > 0 ? e->bpm_now : 120.0) * TRK_PPQ);

        pthread_mutex_lock(&e->smx);
        for (i = j = 0; i < e->nsev; i++) {
            const sample_ev *v = &e->sev[i];
            int off = 0;
            if (!v->asap) {
                double wall = now + (double)(int)(v->tick - cur) * spt + e->alat;
                double f = floor((wall - pwall) * KIT_RATE);
                off = f < 0 ? 0 : f > P ? P : (int)f;
            }
            if (off < P) {
                /* In time order, a hit stays behind the ones before it. */
                int at = nd;
                while (at > 0 && due[at - 1].off > off) { due[at] = due[at - 1]; at--; }
                due[at].off = off; due[at].v = *v;
                nd++;
            } else {
                e->sev[j++] = *v;
            }
        }
        e->nsev = j;

        memset(mix, 0, (size_t)P * 2 * sizeof *mix);
        for (a = 0, k = 0; a < P; ) {
            int b, n;
            while (k < nd && due[k].off <= a) { apply_due(e, &due[k].v); k++; }
            b = k < nd ? due[k].off : P;
            for (i = 0; b > a && i <= e->nkit; i++) {
                drumkit *dk = i < e->nkit ? e->kit[i].dk : e->aud;
                if (!dk) continue;
                drumkit_render(dk, tmp, b - a);
                for (n = 0; n < (b - a) * 2; n++) mix[a * 2 + n] += tmp[n];
            }
            a = b;
        }
        pthread_mutex_unlock(&e->smx);

        {   /* The master volume: read without the lock, as one int is. */
            const int vol = e->song.volume;
            const double g = (vol < 0 ? 0 : vol > 150 ? 150 : vol) / 100.0;
            for (i = 0; i < P * 2; i++) mix[i] *= g;
        }
        for (i = 0; i < P * 2; i++) {
            double v = mix[i] * 32767.0;
            out[i] = (short)(v > 32767.0 ? 32767 : v < -32768.0 ? -32768 : v);
        }
        w = snd_pcm_writei(e->pcm, out, (snd_pcm_uframes_t)P);
        if (w < 0 && snd_pcm_recover(e->pcm, (int)w, 1) < 0) {
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);                /* broken device: do not spin */
        }
        /* A device that takes a period at once -- ALSA's null, say -- is
         * kept to the period's own length here. Unpaced, this loop would
         * spin on smx, and everything else that wants the samples would
         * wait on it for good. */
        {
            double took = mono_now() - now, want = (double)P / KIT_RATE;
            if (took < want / 4) {
                struct timespec ts = { 0, (long)((want - took) * 1e9) };
                nanosleep(&ts, NULL);
            }
        }
    }
    free(mix); free(tmp); free(out); free(due);
    return NULL;
}

/* The output, opened the first time a track wants a kit. TRK_PCM names the
 * ALSA device, "default" when unset. */
static int audio_open(trk_engine *e)
{
    const char *dev = getenv("TRK_PCM");
    snd_pcm_uframes_t buf = 0, per = 0;
    int r;

    if (e->audio_on) return 0;
    if (!dev || !*dev) dev = "default";
    if ((r = snd_pcm_open(&e->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        snprintf(e->audio_msg, sizeof e->audio_msg, "no audio output (%s): %s", dev, snd_strerror(r));
        e->pcm = NULL;
        return -1;
    }
    /* 30 ms: short enough to play along to, long enough not to drop out at
     * ordinary priority. */
    if ((r = snd_pcm_set_params(e->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                2, KIT_RATE, 1, 30000)) < 0) {
        snprintf(e->audio_msg, sizeof e->audio_msg, "audio output (%s) refused 48 kHz stereo: %s",
                 dev, snd_strerror(r));
        snd_pcm_close(e->pcm);
        e->pcm = NULL;
        return -1;
    }
    snd_pcm_get_params(e->pcm, &buf, &per);
    e->aperiod = per >= 64 && per <= 4096 ? (int)per : 256;
    e->alat = (double)(buf + per) / KIT_RATE;
    e->aquit = 0;
    if (pthread_create(&e->athread, NULL, audio_main, e)) {
        snprintf(e->audio_msg, sizeof e->audio_msg, "could not start the audio thread");
        snd_pcm_close(e->pcm);
        e->pcm = NULL;
        return -1;
    }
    e->audio_on = 1;
    snprintf(e->audio_msg, sizeof e->audio_msg, "samples: %s, %.0f ms", dev, e->alat * 1000);
    return 0;
}

static void scan_kits(trk_engine *e, int force)
{
    if (!force && e->nplaces && mono_now() - e->scanned_at < KIT_RESCAN_S) return;
    e->nplaces = drumkit_find(e->places, DK_MAX_KITS);
    e->scanned_at = mono_now();
}

/* A sample set's folder from what a song calls it: the name the track's
 * list gives it, the last part of that ("Clap" for "The Cave Drumkit V3 /
 * Clap"), or a path -- what Load Sample Set gives. */
static const char *kit_path(trk_engine *e, const char *name)
{
    int pass, i;
    if (name[0] == '/') return name;
    for (pass = 0; pass < 2; pass++) {
        scan_kits(e, pass);                    /* the second time, a fresh look */
        for (i = 0; i < e->nplaces; i++) if (!strcmp(e->places[i].name, name)) return e->places[i].path;
        for (i = 0; i < e->nplaces; i++) {
            const char *last = strstr(e->places[i].name, " / ");
            if (last && !strcmp(last + 3, name)) return e->places[i].path;
        }
    }
    return NULL;
}

/* Every set the tracks name, loaded; the ones nothing names any more, freed;
 * one just edited, read again. The WAVs are read outside every lock -- that
 * takes a while, and playback goes on meanwhile. Called only from trk_route,
 * so only one thread at a time changes e->kit. Returns how many sample tracks
 * name a set that is not there. */
static int load_samples(trk_engine *e)
{
    char want[TRK_TRACKS][TRK_PATH_LEN], stale[TRK_PATH_LEN];
    kit_slot fresh[TRK_TRACKS], next[TRK_TRACKS], drop[2 * TRK_TRACKS];
    int nfresh = 0, nnext = 0, ndrop = 0, any = 0, missing = 0, t, i, j;
    int map[TRK_TRACKS];

    pthread_mutex_lock(&e->lock);
    for (t = 0; t < TRK_TRACKS; t++) {
        snprintf(want[t], sizeof want[t], "%s", e->song.track[t].samples);
        if (want[t][0]) any = 1;
    }
    snprintf(stale, sizeof stale, "%s", e->stale);
    e->stale[0] = 0;
    pthread_mutex_unlock(&e->lock);

    for (t = 0; t < TRK_TRACKS; t++) {
        const char *path;
        int have = 0;
        if (!want[t][0]) continue;
        if (strcmp(want[t], stale))
            for (i = 0; i < e->nkit; i++) if (!strcmp(e->kit[i].name, want[t])) have = 1;
        for (i = 0; i < nfresh; i++) if (!strcmp(fresh[i].name, want[t])) have = 1;
        if (have || !(path = kit_path(e, want[t]))) continue;
        snprintf(fresh[nfresh].name, sizeof fresh[nfresh].name, "%s", want[t]);
        fresh[nfresh].dk = drumkit_load(path, KIT_RATE);
        if (fresh[nfresh].dk) nfresh++;
    }
    if (any) audio_open(e);

    pthread_mutex_lock(&e->lock);
    pthread_mutex_lock(&e->smx);
    for (t = 0; t < TRK_TRACKS; t++) {
        int at = -1;
        e->track_kit[t] = -1;
        if (!want[t][0]) continue;
        for (i = 0; i < nnext; i++) if (!strcmp(next[i].name, want[t])) at = i;
        /* A set read again replaces the one loaded; otherwise what is loaded
         * stays, with whatever it has sounding. */
        for (i = 0; i < nfresh && at < 0; i++)
            if (fresh[i].dk && !strcmp(fresh[i].name, want[t])) {
                next[nnext] = fresh[i]; fresh[i].dk = NULL; at = nnext++;
            }
        for (i = 0; i < e->nkit && at < 0; i++)
            if (e->kit[i].dk && !strcmp(e->kit[i].name, want[t])) {
                next[nnext] = e->kit[i]; e->kit[i].dk = NULL; at = nnext++;
            }
        if (at >= 0 && e->audio_on) e->track_kit[t] = at;
        else missing++;
    }
    /* Pending hits follow their set to its new place, or go with it. */
    for (i = 0; i < e->nkit; i++) {
        map[i] = -1;
        if (!e->kit[i].dk)                        /* moved to next, not replaced */
            for (j = 0; j < nnext; j++) if (!strcmp(next[j].name, e->kit[i].name)) map[i] = j;
        if (e->kit[i].dk) drop[ndrop++] = e->kit[i];
    }
    for (i = j = 0; i < e->nsev; i++) {
        sample_ev v = e->sev[i];
        if (v.op == SEV_PLAY) {
            if (v.kit < 0 || v.kit >= e->nkit || map[v.kit] < 0) continue;
            v.kit = map[v.kit];
        }
        e->sev[j++] = v;
    }
    e->nsev = j;
    for (i = 0; i < nfresh; i++) if (fresh[i].dk) drop[ndrop++] = fresh[i];
    memcpy(e->kit, next, sizeof next[0] * (size_t)nnext);
    e->nkit = nnext;
    pthread_mutex_unlock(&e->smx);
    pthread_mutex_unlock(&e->lock);

    for (i = 0; i < ndrop; i++) drumkit_free(drop[i].dk);
    return missing;
}

/* --------------------------------------------------------------- routing */

/* A destination by name: the client called `client`, and its port called
 * `port` -- or, with no port named, its first one that can be played. */
static int resolve(trk_engine *e, const char *client, const char *port,
                   snd_seq_addr_t *out)
{
    const unsigned need = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t   *pi;

    if (!client || !*client) return -1;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(e->seq, ci) >= 0) {
        int cl = snd_seq_client_info_get_client(ci);
        if (cl == e->client) continue;
        if (strcmp(snd_seq_client_info_get_name(ci), client)) continue;
        snd_seq_port_info_set_client(pi, cl);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(e->seq, pi) >= 0) {
            if ((snd_seq_port_info_get_capability(pi) & need) != need) continue;
            if (port && *port && strcmp(snd_seq_port_info_get_name(pi), port)) continue;
            out->client = (unsigned char)cl;
            out->port = (unsigned char)snd_seq_port_info_get_port(pi);
            return 0;
        }
    }
    return -1;
}

static int same_addr(snd_seq_addr_t a, snd_seq_addr_t b)
{ return a.client == b.client && a.port == b.port; }

int trk_route(trk_engine *e)
{
    snd_seq_addr_t want[TRK_TRACKS], clk[TRK_TRACKS];
    int ok[TRK_TRACKS], t, i, missing, nclk = 0, moved = 0;

    missing = load_samples(e);
    pthread_mutex_lock(&e->lock);
    for (t = 0; t < TRK_TRACKS; t++) {
        const trk_track *k = &e->song.track[t];
        /* A sample track plays no window: whatever window it played before
         * is released and let go below, as for any track that moves. */
        ok[t] = !is_sampled(k) && resolve(e, k->client, k->port, &want[t]) == 0;
        if (k->client[0] && !is_sampled(k) && !ok[t]) missing++;
        if (e->routed[t] && (!ok[t] || !same_addr(want[t], e->dest[t]))) moved++;
    }
    /* A track's queued notes go wherever its port is connected when their
     * time comes. Taken back first, they cannot reach the new window. */
    if (moved && e->playing) rewind_locked(e);

    for (t = 0; t < TRK_TRACKS; t++) {
        if (e->routed[t] && (!ok[t] || !same_addr(want[t], e->dest[t]))) {
            /* Going somewhere else: what it left sounding there is released
             * while the connection still exists to carry the note-offs. */
            release_track(e, t);
            snd_seq_disconnect_to(e->seq, e->port[t], e->dest[t].client, e->dest[t].port);
            e->routed[t] = 0;
        }
        if (ok[t]) {
            /* Connected already answers EBUSY, which is still connected. A
             * window that closed and reopened under the same number needs
             * this as well, since the kernel dropped the old subscription. */
            int r = snd_seq_connect_to(e->seq, e->port[t], want[t].client, want[t].port);
            e->routed[t] = (r == 0 || r == -EBUSY);
            e->dest[t] = want[t];
        }
        if (ok[t]) {
            int dup = 0;
            for (i = 0; i < nclk; i++) if (same_addr(clk[i], want[t])) dup = 1;
            if (!dup) clk[nclk++] = want[t];
        }
    }

    /* The clock goes once to each destination, however many tracks play it
     * -- two tracks on one window would otherwise run its tempo double. */
    for (i = 0; i < e->nclock; i++) {
        int keep = 0, j;
        for (j = 0; j < nclk; j++) if (same_addr(e->clock_dest[i], clk[j])) keep = 1;
        if (!keep) snd_seq_disconnect_to(e->seq, e->clock_port,
                                         e->clock_dest[i].client, e->clock_dest[i].port);
    }
    for (i = 0; i < nclk; i++) {
        snd_seq_connect_to(e->seq, e->clock_port, clk[i].client, clk[i].port);
        e->clock_dest[i] = clk[i];
    }
    e->nclock = nclk;
    if (moved && e->playing) top_up(e);
    pthread_mutex_unlock(&e->lock);
    return missing;
}

int trk_routed(trk_engine *e, int t)
{
    int r;
    if (t < 0 || t >= TRK_TRACKS) return 0;
    pthread_mutex_lock(&e->lock);
    r = is_sampled(&e->song.track[t]) ? e->track_kit[t] >= 0 : e->routed[t];
    pthread_mutex_unlock(&e->lock);
    return r;
}

int trk_list_dests(trk_engine *e, char *buf, size_t n)
{
    const unsigned need = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t   *pi;
    size_t used = 0;
    int    count = 0;

    if (n) buf[0] = 0;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    pthread_mutex_lock(&e->lock);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(e->seq, ci) >= 0) {
        int cl = snd_seq_client_info_get_client(ci);
        if (cl == e->client || cl == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pi, cl);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(e->seq, pi) >= 0) {
            int w;
            if ((snd_seq_port_info_get_capability(pi) & need) != need) continue;
            if (!(snd_seq_port_info_get_type(pi) & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
            w = snprintf(buf + used, n - used, "%s\t%s\n",
                         snd_seq_client_info_get_name(ci),
                         snd_seq_port_info_get_name(pi));
            if (w < 0 || (size_t)w >= n - used) { buf[used] = 0; goto done; }
            used += (size_t)w;
            count++;
        }
    }
done:
    pthread_mutex_unlock(&e->lock);
    return count;
}

int trk_list_sample_sets(trk_engine *e, char *buf, size_t n)
{
    size_t used = 0;
    int i, count = 0;
    if (n) buf[0] = 0;
    pthread_mutex_lock(&e->lock);
    scan_kits(e, 0);
    /* The ones loaded from elsewhere first: they were asked for by name. */
    for (i = 0; i < e->nadded + e->nplaces; i++) {
        const char *name = i < e->nadded ? e->added[i] : e->places[i - e->nadded].name;
        const char *path = i < e->nadded ? e->added[i] : e->places[i - e->nadded].path;
        int w = snprintf(buf + used, n - used, "%s\t%s\n", name, path);
        if (w < 0 || (size_t)w >= n - used) { buf[used] = 0; break; }
        used += (size_t)w;
        count++;
    }
    pthread_mutex_unlock(&e->lock);
    return count;
}

void trk_add_sample_set(trk_engine *e, const char *path)
{
    int i;
    pthread_mutex_lock(&e->lock);
    for (i = 0; i < e->nadded; i++) if (!strcmp(e->added[i], path)) break;
    if (i == e->nadded) {
        if (e->nadded == TRK_TRACKS) {            /* the oldest makes room */
            memmove(e->added[0], e->added[1], sizeof e->added[0] * (TRK_TRACKS - 1));
            e->nadded--;
        }
        snprintf(e->added[e->nadded++], sizeof e->added[0], "%s", path);
    }
    pthread_mutex_unlock(&e->lock);
}

int trk_sample_set_dir(trk_engine *e, const char *name, char *out, size_t n)
{
    const char *p;
    pthread_mutex_lock(&e->lock);
    p = kit_path(e, name);
    if (p) snprintf(out, n, "%s", p);
    pthread_mutex_unlock(&e->lock);
    return p ? 0 : -1;
}

int trk_sample_at(trk_engine *e, int t, int note, char *name, size_t n)
{
    int r = -1, k, slot, i, lo = 128, hi = -1;
    if (n) name[0] = 0;
    if (t < 0 || t >= TRK_TRACKS) return -1;
    pthread_mutex_lock(&e->lock);
    pthread_mutex_lock(&e->smx);
    if ((k = e->track_kit[t]) >= 0) {
        const drumkit *dk = e->kit[k].dk;
        if ((slot = drumkit_slot_at(dk, note)) >= 0) {
            snprintf(name, n, "%s", drumkit_sample_name(dk, slot));
            r = 1;
        } else {
            char a[5] = "", b[5] = "";
            const char *set = strrchr(e->kit[k].name, '/');
            for (i = 0; i < drumkit_count(dk); i++) {
                int nt = drumkit_note_of(dk, i);
                if (nt < lo) lo = nt;
                if (nt > hi) hi = nt;
            }
            if (hi >= 0) { drumkit_note_name(lo, a); drumkit_note_name(hi, b); }
            snprintf(name, n, "%s runs %s to %s", set ? set + 1 : e->kit[k].name, a, b);
            r = 0;
        }
    }
    pthread_mutex_unlock(&e->smx);
    pthread_mutex_unlock(&e->lock);
    return r;
}

int trk_sample_mask(trk_engine *e, int t, unsigned char mask[16])
{
    int k, note, r = 0;
    if (t < 0 || t >= TRK_TRACKS) return 0;
    pthread_mutex_lock(&e->lock);
    pthread_mutex_lock(&e->smx);
    if ((k = e->track_kit[t]) >= 0) {
        memset(mask, 0, 16);
        for (note = 0; note < 128; note++)
            if (drumkit_slot_at(e->kit[k].dk, note) >= 0) mask[note >> 3] |= (unsigned char)(1u << (note & 7));
        r = 1;
    }
    pthread_mutex_unlock(&e->smx);
    pthread_mutex_unlock(&e->lock);
    return r;
}

int trk_list_pads(trk_engine *e, int t, char *buf, size_t n)
{
    size_t used = 0;
    int k, note, count = 0;
    if (n) buf[0] = 0;
    if (t < 0 || t >= TRK_TRACKS) return 0;
    pthread_mutex_lock(&e->lock);
    pthread_mutex_lock(&e->smx);
    if ((k = e->track_kit[t]) >= 0)
        for (note = 0; note < 128; note++) {
            int slot = drumkit_slot_at(e->kit[k].dk, note), w;
            if (slot < 0) continue;
            w = snprintf(buf + used, n - used, "%d\t%s\n", note,
                         drumkit_sample_name(e->kit[k].dk, slot));
            if (w < 0 || (size_t)w >= n - used) { buf[used] = 0; break; }
            used += (size_t)w;
            count++;
        }
    pthread_mutex_unlock(&e->smx);
    pthread_mutex_unlock(&e->lock);
    return count;
}

void trk_reload_sample_set(trk_engine *e, const char *name)
{
    pthread_mutex_lock(&e->lock);
    snprintf(e->stale, sizeof e->stale, "%s", name);
    pthread_mutex_unlock(&e->lock);
    trk_route(e);
}

int trk_audition(trk_engine *e, const char *dir, const char *file, double gain_db, int vel)
{
    dk_map *m = calloc(1, sizeof *m);
    drumkit *k = NULL, *old;
    if (!m) return -1;
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    m->n = 1;
    m->pad[0].note = 60;
    snprintf(m->pad[0].file, sizeof m->pad[0].file, "%s", file);
    m->pad[0].gain_db = gain_db;
    k = drumkit_load_map(m, KIT_RATE);
    free(m);
    if (!k || audio_open(e)) { drumkit_free(k); return -1; }
    pthread_mutex_lock(&e->smx);
    old = e->aud;
    e->aud = k;
    if (e->nsev < SEV_MAX) {
        sample_ev *v = &e->sev[e->nsev++];
        memset(v, 0, sizeof *v);
        v->asap = 1; v->op = SEV_AUDITION; v->note = 60; v->vel = vel;
    }
    pthread_mutex_unlock(&e->smx);
    drumkit_free(old);
    return 0;
}

/* ------------------------------------------------------------- transport *//* ------------------------------------------------------------- transport */

static void stop_locked(trk_engine *e, int send_stop)
{
    int t;
    if (e->playing) {
        e->playing = 0;
        unschedule(e);
        snd_seq_stop_queue(e->seq, e->queue, NULL);
        snd_seq_drain_output(e->seq);
    }
    if (send_stop) {
        snd_seq_event_t ev;
        ev_base(e, &ev, e->clock_port);
        ev.type = SND_SEQ_EVENT_STOP;
        now(e, &ev);
    }
    for (t = 0; t < TRK_TRACKS; t++) release_track(e, t);
    /* Hits still to come go, and what is sounding fades, as a held MIDI
     * note is released: a long sample should stop when the song does. */
    drop_samples(e, 1, 0);
    push_sample(e, 0, 1, SEV_FADE, -1, 0, 0);
    e->npos = 0;
}

void trk_play(trk_engine *e, int mode, int order, int row)
{
    snd_seq_event_t ev;
    int beats_rows = 0, i, sixteenths;

    pthread_mutex_lock(&e->lock);
    stop_locked(e, 0);
    e->mode = mode == TRK_PLAY_SONG ? TRK_PLAY_SONG : TRK_PLAY_PATTERN;
    if (e->mode == TRK_PLAY_SONG) {
        if (order < 0 || order >= e->song.norder) order = 0;
        e->order = order;
        e->pattern = e->song.order[order];
        for (i = 0; i < order; i++) beats_rows += e->song.pattern[e->song.order[i]].rows;
    } else {
        if (order < 0 || order >= TRK_PATTERNS) order = 0;
        e->order = 0;
        e->pattern = order;
    }
    if (row < 0 || row >= e->song.pattern[e->pattern].rows) row = 0;
    e->row = row;
    beats_rows += row;

    /* A queue restarted from tick zero, at the song's tempo. */
    set_tempo(e, e->song.bpm);
    snd_seq_start_queue(e->seq, e->queue, NULL);
    snd_seq_drain_output(e->seq);
    /* The first row a few milliseconds in, and scheduled here rather than
     * left to the thread: it may be most of a sleep away, and a row whose
     * time has passed goes out late -- the first beat out of step with
     * every one after it. */
    e->next_tick = (unsigned)(START_S * (e->song.bpm > 0 ? e->song.bpm : 120.0)
                              / 60.0 * TRK_PPQ);
    e->next_clock = e->clock_base = e->next_tick;

    /* Where the song is, then go -- so a synced arpeggiator starts on the
     * beat this does rather than wherever its own counter had got to. */
    sixteenths = beats_rows * 4 / (trk_lpb_ok(e->song.lpb) ? e->song.lpb : 4);
    ev_base(e, &ev, e->clock_port);
    ev.type = SND_SEQ_EVENT_SONGPOS;
    ev.data.control.value = sixteenths & 0x3FFF;
    now(e, &ev);
    ev_base(e, &ev, e->clock_port);
    ev.type = row == 0 && beats_rows == 0 ? SND_SEQ_EVENT_START : SND_SEQ_EVENT_CONTINUE;
    now(e, &ev);
    snd_seq_drain_output(e->seq);
    e->playing = 1;
    top_up(e);
    pthread_mutex_unlock(&e->lock);
}

void trk_stop(trk_engine *e)
{
    pthread_mutex_lock(&e->lock);
    stop_locked(e, 1);
    pthread_mutex_unlock(&e->lock);
}

int trk_playing(trk_engine *e)
{
    int p;
    pthread_mutex_lock(&e->lock);
    p = e->playing;
    pthread_mutex_unlock(&e->lock);
    return p;
}

void trk_position(trk_engine *e, int *order, int *pattern, int *row)
{
    int o = -1, p = -1, r = -1;
    pthread_mutex_lock(&e->lock);
    if (e->playing && e->npos) {
        unsigned cur = queue_tick(e), i, n = e->npos < POS_RING ? e->npos : POS_RING;
        /* The latest row whose time has come. Scheduled ones still ahead of
         * the queue are not playing yet, whatever has been sent. */
        for (i = 1; i <= n; i++) {
            const pos_mark *m = &e->pos[(e->npos - i) % POS_RING];
            if (m->tick <= cur) { o = m->order; p = m->pattern; r = m->row; break; }
        }
    }
    pthread_mutex_unlock(&e->lock);
    if (order) *order = o;
    if (pattern) *pattern = p;
    if (row) *row = r;
}

void trk_set_bpm(trk_engine *e, double bpm)
{
    if (bpm < 20.0) bpm = 20.0;
    if (bpm > 999.0) bpm = 999.0;
    pthread_mutex_lock(&e->lock);
    e->song.bpm = bpm;
    set_tempo(e, bpm);
    pthread_mutex_unlock(&e->lock);
}

/* --------------------------------------------------------------- preview */

static void preview_off_locked(trk_engine *e, int t)
{
    snd_seq_event_t ev;
    if (e->prev_note[t] < 0) return;
    ev_base(e, &ev, e->port[t]);
    snd_seq_ev_set_noteoff(&ev, e->prev_ch[t], e->prev_note[t], 0);
    now(e, &ev);
    snd_seq_drain_output(e->seq);
    e->prev_note[t] = -1;
}

void trk_preview(trk_engine *e, int t, int note, int vel)
{
    snd_seq_event_t ev;
    int ch;
    if (t < 0 || t >= TRK_TRACKS || note < 0 || note > 127) return;
    pthread_mutex_lock(&e->lock);
    if (is_sampled(&e->song.track[t])) {
        /* Played out rather than held: a preview of a drum should be the
         * whole hit, however quickly the key comes back up. */
        if (e->track_kit[t] >= 0)
            push_sample(e, 0, 1, SEV_PLAY, e->track_kit[t], note,
                        vel >= 1 && vel <= 127 ? vel : e->song.track[t].velocity);
        pthread_mutex_unlock(&e->lock);
        return;
    }
    preview_off_locked(e, t);
    ch = e->song.track[t].channel & 15;
    if (vel < 1 || vel > 127) vel = e->song.track[t].velocity;
    ev_base(e, &ev, e->port[t]);
    snd_seq_ev_set_noteon(&ev, ch, note, vel);
    now(e, &ev);
    snd_seq_drain_output(e->seq);
    e->prev_note[t] = note;
    e->prev_ch[t] = ch;
    mark(e, t, ch, note);
    pthread_mutex_unlock(&e->lock);
}

void trk_preview_off(trk_engine *e, int t)
{
    if (t < 0 || t >= TRK_TRACKS) return;
    pthread_mutex_lock(&e->lock);
    preview_off_locked(e, t);
    pthread_mutex_unlock(&e->lock);
}

void trk_panic(trk_engine *e)
{
    int t;
    pthread_mutex_lock(&e->lock);
    /* Playing on: what is queued is taken back first, or it would sound
     * after the releases below and be held by nothing. */
    if (e->playing) rewind_locked(e);
    push_sample(e, 0, 1, SEV_SILENCE, -1, 0, 0);         /* every sample, now */
    for (t = 0; t < TRK_TRACKS; t++) {
        release_track(e, t);
        /* And every channel the track is set to now, even one it never
         * played: a note somebody else started on it is still a stuck note. */
        {
            snd_seq_event_t ev;
            int ch = e->song.track[t].channel & 15;
            ev_base(e, &ev, e->port[t]);
            snd_seq_ev_set_controller(&ev, ch, 123, 0);
            now(e, &ev);
        }
    }
    snd_seq_drain_output(e->seq);
    if (e->playing) top_up(e);
    pthread_mutex_unlock(&e->lock);
}

/* ------------------------------------------------------------ life cycle */

trk_engine *trk_open(char *err, size_t errn)
{
    trk_engine *e = calloc(1, sizeof *e);
    snd_seq_queue_tempo_t *qt;
    int t;

    if (!e) { snprintf(err, errn, "out of memory"); return NULL; }
    /* Blocking: with the output pool full, a write waits rather than failing,
     * and a failed write can be the note-off. */
    if (snd_seq_open(&e->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        snprintf(err, errn, "the ALSA sequencer is not available (is snd-seq loaded?)");
        free(e);
        return NULL;
    }
    e->client = snd_seq_client_id(e->seq);
    snd_seq_set_client_pool_output(e->seq, OUTPUT_POOL);

    /* "tracker", or "tracker 2" when one is open already: two clients with
     * one name are two identical entries in every other program's list. */
    {
        snd_seq_client_info_t *ci;
        int n;
        snd_seq_client_info_alloca(&ci);
        snprintf(e->name, sizeof e->name, "tracker");
        for (n = 2; n < 32; n++) {
            int taken = 0;
            snd_seq_client_info_set_client(ci, -1);
            while (snd_seq_query_next_client(e->seq, ci) >= 0)
                if (snd_seq_client_info_get_client(ci) != e->client &&
                    !strcmp(snd_seq_client_info_get_name(ci), e->name)) { taken = 1; break; }
            if (!taken) break;
            snprintf(e->name, sizeof e->name, "tracker %d", n);
        }
        snd_seq_set_client_name(e->seq, e->name);
    }

    /* Readable but not subscribable by anyone else. dwstudio and pestudio
     * connect themselves to every port that is both, so a subscribable
     * track would be heard by every window -- each instrument playing all
     * eight tracks. The tracker makes its own connections, which as the
     * ports' owner it may do without SUBS_READ; nothing else can. */
    for (t = 0; t < TRK_TRACKS; t++) {
        char pn[32];
        snprintf(pn, sizeof pn, "Track %d", t + 1);
        e->port[t] = snd_seq_create_simple_port(e->seq, pn, SND_SEQ_PORT_CAP_READ,
                         SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        e->held[t] = e->prev_note[t] = -1;
    }
    e->clock_port = snd_seq_create_simple_port(e->seq, "Clock", SND_SEQ_PORT_CAP_READ,
                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    e->queue = snd_seq_alloc_named_queue(e->seq, "tracker");
    if (e->queue < 0) {
        snprintf(err, errn, "could not allocate a sequencer queue");
        snd_seq_close(e->seq);
        free(e);
        return NULL;
    }
    /* The queue's default timer is the system one, which steps at the
     * kernel's HZ -- 4 ms at 250 -- and every note lands on one of those
     * steps. A beat a few milliseconds out is audible against a drum
     * machine; the high-resolution timer is not out at all. Left on the
     * default where the kernel has no hrtimer. */
    {
        snd_seq_queue_timer_t *tm;
        snd_timer_id_t *id;
        snd_seq_queue_timer_alloca(&tm);
        snd_timer_id_alloca(&id);
        snd_timer_id_set_class(id, SND_TIMER_CLASS_GLOBAL);
        snd_timer_id_set_sclass(id, SND_TIMER_SCLASS_NONE);
        snd_timer_id_set_card(id, -1);
        snd_timer_id_set_device(id, SND_TIMER_GLOBAL_HRTIMER);
        snd_timer_id_set_subdevice(id, 0);
        if (snd_seq_get_queue_timer(e->seq, e->queue, tm) >= 0) {
            snd_seq_queue_timer_set_type(tm, SND_SEQ_TIMER_ALSA);
            snd_seq_queue_timer_set_id(tm, id);
            snd_seq_queue_timer_set_resolution(tm, 1000000);   /* 1 ms ticks */
            if (snd_seq_set_queue_timer(e->seq, e->queue, tm) < 0)
                fprintf(stderr, "tracker: no high-resolution timer; "
                                "timing will step at the kernel's HZ\n");
        }
    }
    snd_seq_queue_tempo_alloca(&qt);
    snd_seq_queue_tempo_set_tempo(qt, 500000);
    snd_seq_queue_tempo_set_ppq(qt, TRK_PPQ);
    snd_seq_set_queue_tempo(e->seq, e->queue, qt);

    trk_song_init(&e->song);
    pthread_mutex_init(&e->lock, NULL);
    pthread_mutex_init(&e->smx, NULL);
    for (t = 0; t < TRK_TRACKS; t++) e->track_kit[t] = -1;
    e->bpm_now = 120.0;
    if (pthread_create(&e->thread, NULL, sched_thread, e)) {
        snprintf(err, errn, "could not start the scheduling thread");
        snd_seq_close(e->seq);
        free(e);
        return NULL;
    }
    return e;
}

void trk_close(trk_engine *e)
{
    int i;
    if (!e) return;
    pthread_mutex_lock(&e->lock);
    stop_locked(e, e->playing);
    e->quit = 1;
    pthread_mutex_unlock(&e->lock);
    pthread_join(e->thread, NULL);
    if (e->audio_on) {
        e->aquit = 1;
        pthread_join(e->athread, NULL);
        snd_pcm_drop(e->pcm);
        snd_pcm_close(e->pcm);
    }
    for (i = 0; i < e->nkit; i++) drumkit_free(e->kit[i].dk);
    drumkit_free(e->aud);
    pthread_mutex_destroy(&e->smx);
    snd_seq_free_queue(e->seq, e->queue);
    snd_seq_close(e->seq);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

trk_song *trk_song_of(trk_engine *e) { return &e->song; }
void trk_lock(trk_engine *e)   { pthread_mutex_lock(&e->lock); }
void trk_unlock(trk_engine *e) { pthread_mutex_unlock(&e->lock); }
const char *trk_client_name(trk_engine *e) { return e->name; }
const char *trk_audio_status(trk_engine *e) { return e->audio_msg; }
