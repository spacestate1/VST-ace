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
 * timer's millisecond, with nothing in between.
 *
 * A track can also play an in-process sink -- a synth this same program
 * hosts -- instead of an ALSA window. Those events take the same route the
 * sample hits do: queued against the tick clock by the scheduling thread,
 * then a delivery thread turns tick into wall time exactly as the queue's
 * timestamps mean it and hands each sink a block at a time, every event with
 * its sample offset in the block. The sink's host places the events in its
 * own rendered block from there, which is the accuracy the kernel's queue
 * gives an ALSA window, without the trip through it. */
#include "trk.h"
#include "drumkit.h"

#include <alsa/asoundlib.h>
#ifdef TRK_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef TRK_HAVE_PIPEWIRE
#define TRK_DEFAULT_LABEL "System default (PipeWire)"
#else
#define TRK_DEFAULT_LABEL "System default"
#endif
#define TRK_PPQ       960            /* divisible by every lpb trk_lpb_ok allows, and by 24 */
#define CLOCK_TICKS   (TRK_PPQ / 24) /* MIDI clock is 24 per quarter note */
#define LOOKAHEAD_S   0.12
#define START_S       0.01           /* play to first row */
#define POS_RING      512
#define OUTPUT_POOL   4000           /* kernel cells for events scheduled ahead */
#define KIT_RATE      48000
#define SEV_MAX       1024           /* kit hits scheduled and not yet played */
#define KIT_RESCAN_S  5.0            /* how stale the list of kits may get */
#define TRK_SINKS     8              /* in-process destinations registered at once */
#define SINK_Q        2048           /* events scheduled for one sink and not yet delivered */
void trk_undo_push(trk_engine *e);            /* below: the editor's snapshot, before an edit */
struct trk_engine;
static void audio_conf_load(struct trk_engine *e);
#define SINK_PERIOD   128            /* frames per delivered block: 2.7 ms at 48 kHz */

/* An event queued for an in-process sink, against the same tick clock the
 * ALSA events are scheduled on. */
typedef struct { unsigned tick; int asap; uint8_t st, d1, d2; } sink_qev;

typedef struct {
    int         used;
    char        name[TRK_DEST_LEN];
    trk_sink_fn fn;
    void       *ud;
    sink_qev    q[SINK_Q];
    int         nq;
    trk_gain_fn gain_cb;            /* the output gain, when one track plays this sink alone */
    double      gain_sent;
} trk_sink;

/* Something for the audio thread to do, at a queue tick or as soon as it can
 * (a preview, a panic): start a sample on a track, fade out what one track or
 * everything is playing, or silence everything. */
enum { SEV_PLAY, SEV_AUDITION, SEV_FADE, SEV_FADE_TRACK, SEV_SILENCE, SEV_CLICK };
typedef struct { unsigned tick; int asap, op, kit, note, vel, track; } sample_ev;

typedef struct { char name[TRK_PATH_LEN]; drumkit *dk; } kit_slot;

/* One row as scheduled: when, where, and what each track was holding just
 * before it -- so the schedule can be wound back to any row still ahead of the
 * queue and played again from there with the right notes to release. */
typedef struct {
    unsigned tick;
    int      order, pattern, row;
    int      held[TRK_TRACKS], held_ch[TRK_TRACKS];
} pos_mark;

/* One thing played, as it was played: kept for the take's own MIDI file. */
typedef struct { unsigned tick; unsigned char track, kind, chan, a, b; } take_ev;
enum { TAKE_ON, TAKE_OFF, TAKE_CC };
#define TAKE_MAX  (1u << 20)                     /* events kept: ~16 MB, an hour of dense playing */

struct trk_engine {
    snd_seq_t      *seq;
    int             client;
    char            name[64];
    int             port[TRK_TRACKS];
    int             clock_port;
    int             in_port;         /* "Record In": MIDI in, stamped with the queue's tick */
    int             queue;

    pthread_mutex_t lock;            /* the song, the play state and the seq handle */
    pthread_t       thread;
    int             quit;
    trk_song        song;

    /* Undo: a whole-song snapshot taken just before each edit, the last
     * TRK_UNDO_MAX kept, the oldest dropped over that. The snapshots are
     * the song's own ~800 KB apiece, so the cap is a memory cap too.
     * Under lock, as the song is. */
    struct snap   *undo[TRK_UNDO_MAX];
    int             nundo;
    struct snap   *redo[TRK_UNDO_MAX];   /* what undo took off, newest last */
    int             nredo;

    /* Recording. All under lock unless said. */
    trk_rec_opts    ropt;
    int             recording;            /* a take is running, count-in included */
    unsigned        rec_first_tick;       /* the first row's tick; before it, the count-in */
    unsigned        rec_lead;             /* the count-in's length in ticks, 0 for none */
    int             rec_track;            /* where the MIDI input goes: the cursor's */
    int             rec_note[TRK_TRACKS]; /* recorded on and not yet released, or -1 */
    int             vol_sent[TRK_TRACKS]; /* channel volume (CC 7) last sent for the track: (value + 1) | channel << 8, 0 for none */
    int             rec_pat[TRK_TRACKS], rec_row[TRK_TRACKS];   /* ... and where it went */
    take_ev        *take;
    size_t          ntake, captake;
    double          take_bpm;
    pthread_t       ithread;              /* reads the MIDI input */
    int             ion, ipipe[2];
    char            in_name[TRK_DEST_LEN];
    /* The metronome's one voice, rendered by the audio thread; under smx. */
    int             click_left, click_total;
    double          click_ph, click_freq, click_amp;

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

    /* The level meters: a note's velocity as it sounds, falling away. */
    float           meter[TRK_TRACKS];
    unsigned        meter_seen;           /* pos marks already counted */
    double          meter_time;           /* when trk_levels last ran */

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
    /* Totals of what the audio thread has applied, for trktest: hits
     * started, control ops done. Written under smx, as they are read. */
    unsigned long   sev_played, sev_controlled;
    drumkit_place   places[DK_MAX_KITS];  /* every set there is, as last scanned */
    int             nplaces;
    double          scanned_at;
    pthread_mutex_t kmx;                  /* places[], the scan clock and the failure memory below */
    double          forced_at;            /* when the list was last re-read for a name it lacked */
    int             kits_stale;           /* a set was added or reloaded: the next miss may rescan at once */
    struct { char name[TRK_PATH_LEN]; double at; } bad[4];   /* sets that would not load, and when */
    int             nbad;
    _Atomic double  audio_fail_at;        /* when opening the output last failed */

    /* Their output. */
    snd_pcm_t      *pcm;
    pthread_t       athread;
    int             audio_on, aperiod;
    int             use_pw;               /* the output is a PipeWire stream, not the ALSA thread */
    int             buf_level;            /* 0..TRK_BUFFER_LEVELS-1: the buffer-size setting */
#ifdef TRK_HAVE_PIPEWIRE
    struct pw_thread_loop *pwloop;
    struct pw_stream      *pwstream;
    double                 pw_last, pw_blk, pw_said;   /* the stream callback's own, for dropouts */
    _Atomic int            pw_up;        /* the stream is running: its loss is then news */
    _Atomic int            pw_dead;      /* ... and it was lost: the output wants reopening */
#endif
    pthread_mutex_t amx;                  /* opening and closing the output, one at a time */
    pthread_mutex_t rmx;                  /* the recovery thread's handle */
    pthread_t       rthread;
    int             rstarted, rrunning;   /* the recovery thread was made / is still going (under rmx) */
    _Atomic int     closing;              /* the engine is going: nothing new starts */
    pthread_mutex_t tapmx;                /* the output tap: set under it, called under it */
    trk_tap_fn      tap;
    void           *tap_ud;
    _Atomic int     tap_on;
    /* What one block of sample audio is rendered with, shared by both outputs. */
    double         *a_mix, *a_tmp;
    void           *a_due;               /* due_ev[SEV_MAX] */
    void           *a_lim;               /* limiter */
    _Atomic int     aquit;                /* stop flags: read by threads that do not hold the lock */
    double          alat;                 /* seconds from a hit's time to its sound */
    _Atomic double  bpm_now;              /* the queue's tempo, for the audio thread */
    _Atomic int     vol_live;             /* song.volume as of the last unlock: the audio thread's copy */
    char            audio_msg[160];
    _Atomic unsigned xruns;               /* periods the device ran dry on: each is a click */
    char            pcm_name[TRK_DEST_LEN];   /* the chosen output, "" for the default */

    /* In-process sinks. Changed under both locks, lock then dmx; the delivery
     * thread takes dmx alone, so it never waits on the scheduler -- the same
     * discipline as the sample set's smx. */
    pthread_mutex_t dmx;
    trk_sink        sink[TRK_SINKS];
    int             track_sink[TRK_TRACKS]; /* the sink a track plays, or -1: its window */
    pthread_t       dthread;
    int             delivery_on;
    _Atomic int     dquit;
};

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int is_sampled(const trk_track *k) { return k->samples[0] != 0; }

/* Work for the audio thread. A hit is dropped when the list is full: a
 * missed hit, and every sample ends by itself, so nothing is left sounding
 * for good. A control op never is -- hits stop one short of the top, and a
 * control op finding even that slot taken evicts the oldest entry: a SILENCE
 * lost to a full list would leave a panic ringing. */
static void push_sample(trk_engine *e, unsigned tick, int asap, int op, int kit,
                        int note, int vel, int track)
{
    int control = op != SEV_PLAY && op != SEV_AUDITION;
    pthread_mutex_lock(&e->smx);
    if (e->nsev >= (control ? SEV_MAX : SEV_MAX - 1)) {
        if (!control) { pthread_mutex_unlock(&e->smx); return; }
        if (e->nsev == SEV_MAX) {
            /* An older control op holds the kept slot -- hits stop short of
             * it. A stale silence superseded by a fresh one is harmless. */
            memmove(e->sev, e->sev + 1, sizeof e->sev[0] * (SEV_MAX - 1));
            e->nsev--;
        }
    }
    {
        sample_ev *v = &e->sev[e->nsev++];
        v->tick = tick; v->asap = asap; v->op = op; v->kit = kit;
        v->note = note; v->vel = vel; v->track = track;
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

/* One track's pending hits gone -- a muted track's queued hits would
 * otherwise land after its fade. */
static void drop_track_samples(trk_engine *e, int t)
{
    int i, j;
    pthread_mutex_lock(&e->smx);
    for (i = j = 0; i < e->nsev; i++)
        if (e->sev[i].op != SEV_PLAY || e->sev[i].track != t) e->sev[j++] = e->sev[i];
    e->nsev = j;
    pthread_mutex_unlock(&e->smx);
}

/* ------------------------------------------------------------ sinks */

/* An event for an in-process destination, scheduled like the track's ALSA
 * events. The queue discipline is the sample list's: a note-on or a clock
 * tick may be dropped from a full queue, but anything that ends or moves
 * sound never is -- a lost note-off is the stuck note this ordering exists to
 * prevent; finding even the last slot taken, a control event evicts the
 * oldest entry. */
static void sink_push(trk_engine *e, int s, unsigned tick, int asap, int st, int d1, int d2)
{
    trk_sink *k = &e->sink[s];
    int control = !(st == 0xF8 || ((st & 0xF0) == 0x90 && d2 > 0));
    pthread_mutex_lock(&e->dmx);
    if (!k->used) { pthread_mutex_unlock(&e->dmx); return; }
    if (k->nq >= (control ? SINK_Q : SINK_Q - 1)) {
        if (!control) { pthread_mutex_unlock(&e->dmx); return; }
        if (k->nq == SINK_Q) {
            memmove(k->q, k->q + 1, sizeof k->q[0] * (SINK_Q - 1));
            k->nq--;
        }
    }
    k->q[k->nq].tick = tick; k->q[k->nq].asap = asap;
    k->q[k->nq].st = (uint8_t)st; k->q[k->nq].d1 = (uint8_t)d1; k->q[k->nq].d2 = (uint8_t)d2;
    k->nq++;
    pthread_mutex_unlock(&e->dmx);
}

/* Pending sink events after `tick` gone -- or all of them. As drop_samples is
 * for the kits: called wherever scheduled events are taken back. */
static void drop_sinks(trk_engine *e, int all, unsigned tick)
{
    int s, i, j;
    pthread_mutex_lock(&e->dmx);
    for (s = 0; s < TRK_SINKS; s++) {
        trk_sink *k = &e->sink[s];
        for (i = j = 0; i < k->nq; i++)
            if (!all && (k->q[i].asap || k->q[i].tick <= tick)) k->q[j++] = k->q[i];
        k->nq = j;
    }
    pthread_mutex_unlock(&e->dmx);
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

/* One event for a track: to its in-process sink when it is routed to one, to
 * its ALSA window otherwise. The sink gets the raw message against the same
 * tick the ALSA event would have carried, so both clocks agree. */
static void track_ev(trk_engine *e, int t, unsigned tick, int asap, int st, int d1, int d2)
{
    snd_seq_event_t ev;
    if (e->track_sink[t] >= 0) {
        sink_push(e, e->track_sink[t], tick, asap, st, d1, d2);
        return;
    }
    ev_base(e, &ev, e->port[t]);
    switch (st & 0xF0) {
    case 0x80: snd_seq_ev_set_noteoff(&ev, st & 15, d1, d2); break;
    case 0x90: snd_seq_ev_set_noteon(&ev, st & 15, d1, d2); break;
    default:   snd_seq_ev_set_controller(&ev, st & 15, d1, d2); break;
    }
    if (asap) now(e, &ev);
    else at_tick(e, &ev, tick);
}

static int track_on_gain(trk_engine *e, int t);
static int track_rel_volume(trk_engine *e, int t);

/* A track's volume as the synth's own channel volume (CC 7), sent ahead of a
 * note when it is not what the synth was last told. Scaling the velocity alone
 * does nothing on a synth that is not velocity-sensitive -- its amp's velocity
 * amount at zero is common -- so the fader also moves the channel volume. A
 * track left at 100 that never moved the fader sends nothing: the synth keeps
 * its own level. */
static void track_volume(trk_engine *e, int t, unsigned tick, int asap)
{
    const int vol = e->song.track[t].volume, ch = e->song.track[t].channel & 15;
    const int want = vol >= 100 ? 127 : (vol * 127 + 50) / 100;
    const int mark = (want + 1) | (ch << 8);
    if (e->vol_sent[t] == mark) return;
    if (!e->vol_sent[t] && vol >= 100) return;       /* never touched: leave the synth's level */
    track_ev(e, t, tick, asap, 0xB0 | ch, 7, want);
    e->vol_sent[t] = mark;
}

/* Clock, transport and song position to every sink a track plays, once each
 * -- as the clock port carries them once to each window. */
static void clock_to_sinks(trk_engine *e, unsigned tick, int asap, int st, int d1, int d2)
{
    int s, t, used;
    for (s = 0; s < TRK_SINKS; s++) {
        if (!e->sink[s].used) continue;
        used = 0;
        for (t = 0; t < TRK_TRACKS; t++)
            if (e->track_sink[t] == s && !is_sampled(&e->song.track[t])) used = 1;
        if (used) sink_push(e, s, tick, asap, st, d1, d2);
    }
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
    int ch, n;

    for (ch = 0; ch < 16; ch++) {
        if (!(e->chans_used[t] & (1u << ch))) continue;
        for (n = 0; n < 128; n++) {
            if (!(e->touched[t][ch][n >> 3] & (1u << (n & 7)))) continue;
            track_ev(e, t, 0, 1, 0x80 | ch, n, 0);
        }
        track_ev(e, t, 0, 1, 0xB0 | ch, 123, 0);
        track_ev(e, t, 0, 1, 0xB0 | ch, 120, 0);
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

    /* The click, on each beat while a take runs: the bar's first beat higher. */
    if (e->recording && e->ropt.metronome) {
        const int lpb = trk_lpb_ok(s->lpb) ? s->lpb : 4;
        if (e->row % lpb == 0)
            push_sample(e, e->next_tick, 0, SEV_CLICK, -1, e->row % (lpb * 4) == 0, 0, -1);
    }

    for (t = 0; t < TRK_TRACKS; t++) {
        const trk_cell *c = &pt->cell[e->row][t];
        const trk_track *k = &s->track[t];
        int ch = k->channel & 15;

        /* A sample set: the note picks the pad, and a hit plays out -- a
         * drum has no note to end, and a controller has nowhere to go. */
        if (is_sampled(k)) {
            if (k->mute) {
                /* As the MIDI branch below releases a held note: a hit
                 * already sounding would ring out under a track that says
                 * it is silent, and hits queued ahead of the queue would
                 * still land after the fade. */
                if (e->held[t] >= 0 && e->track_kit[t] >= 0) {
                    push_sample(e, e->next_tick, 0, SEV_FADE_TRACK, e->track_kit[t],
                                0, 0, t);
                    e->held[t] = -1;
                }
                drop_track_samples(e, t);
                continue;
            }
            if (c->note <= 127 && e->track_kit[t] >= 0 && trk_track_velocity(k, c->vel == TRK_EMPTY ? 0 : c->vel) > 0) {
                const int vel = trk_track_velocity(k, c->vel == TRK_EMPTY ? 0 : c->vel);
                push_sample(e, e->next_tick, 0, SEV_PLAY, e->track_kit[t], c->note, vel, t);
                e->held[t] = c->note;   /* may be sounding: what a mute fades */
            }
            continue;
        }

        if (k->mute) {
            /* Muting releases what was playing rather than leaving it to
             * ring on under a track that says it is silent. */
            if (e->held[t] >= 0) {
                track_ev(e, t, e->next_tick, 0, 0x80 | e->held_ch[t], e->held[t], 0);
                e->held[t] = -1;
            }
            continue;
        }
        if (c->cc != TRK_EMPTY && c->val != TRK_EMPTY)
            track_ev(e, t, e->next_tick, 0, 0xB0 | ch, c->cc, c->val);
        if (c->note == TRK_EMPTY) continue;
        /* One note per track, as in a tracker: anything new, or a note-off,
         * ends what the track was holding -- on the channel it started on,
         * which is not necessarily the track's channel now. */
        if (e->held[t] >= 0) {
            track_ev(e, t, e->next_tick, 0, 0x80 | e->held_ch[t], e->held[t], 0);
            e->held[t] = -1;
        }
        if (c->note <= 127 && trk_track_velocity(k, c->vel == TRK_EMPTY ? 0 : c->vel) > 0) {
            /* A track alone on an in-process synth is faded by that synth's
             * output gain instead, so its velocities stay the song's. */
            const int on_gain = track_on_gain(e, t);
            trk_track kk = *k;
            int vel;
            kk.volume = track_rel_volume(e, t);
            vel = trk_track_velocity(&kk, c->vel == TRK_EMPTY ? 0 : c->vel);
            if (!on_gain) track_volume(e, t, e->next_tick, 0);
            track_ev(e, t, e->next_tick, 0, 0x90 | ch, c->note, vel);
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
        clock_to_sinks(e, e->next_clock, 0, 0xF8, 0, 0);
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
    drop_sinks(e, 0, cur);
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
        if (v->kit >= 0 && v->kit < e->nkit && v->track >= 0) {
            drumkit *dk = e->kit[v->kit].dk;
            int slot = drumkit_slot_at(dk, v->note);
            /* Grouped by track, not by pad: the track's next hit fades its
             * last, and a mute can end one track's voices -- by group --
             * while the other tracks on the same set play on. */
            if (slot >= 0) {
                drumkit_play(dk, slot, 1.0, v->vel, v->track);
                e->sev_played++;
            }
        }
        break;
    case SEV_AUDITION:
        drumkit_note_on(e->aud, v->note, v->vel);
        e->sev_played++;
        break;
    case SEV_FADE:
        for (i = 0; i < e->nkit; i++) drumkit_fade_all(e->kit[i].dk);
        drumkit_fade_all(e->aud);
        e->sev_controlled++;
        break;
    case SEV_FADE_TRACK:
        if (v->kit >= 0 && v->kit < e->nkit) drumkit_release(e->kit[v->kit].dk, v->track);
        e->sev_controlled++;
        break;
    case SEV_CLICK:
        /* A short sine burst, higher on the bar's first beat. */
        e->click_total = e->click_left = KIT_RATE * 25 / 1000;
        e->click_ph = 0.0;
        e->click_freq = v->note ? 1600.0 : 1000.0;
        e->click_amp = v->note ? 0.45 : 0.3;
        break;
    default:
        for (i = 0; i < e->nkit; i++) drumkit_all_off(e->kit[i].dk);
        drumkit_all_off(e->aud);
        e->sev_controlled++;
        break;
    }
}


/* ------------------------------------------------------------ the limiter --
 *
 * The sample tracks' mix can pass full scale -- stacked hits peaking near 1
 * sum to well over it. The kits used to squash that with a per-sample soft
 * limiter, which bends the waveform of every loud hit and measured about 3.5%
 * distortion on a drum pattern: a crunch on exactly the loudest transients.
 * This one never touches the waveform. It looks LIM_L frames ahead, works out
 * the gain that keeps the peak under the ceiling, spreads that gain change over
 * the lookahead so it is a smooth dip rather than a step, and lets it recover
 * slowly. Each frame is delayed by LIM_L (about a millisecond) so the dip is in
 * place before the peak arrives.
 *
 * Why the result cannot pass the ceiling: the frame leaving now has target t.
 * Every gain-reduction value averaged below was the minimum over a window that
 * includes that frame, so each is <= t, and so is their average. */
#define LIM_L       48                  /* lookahead, frames */
#define LIM_N       (LIM_L + 1)
#define LIM_CEIL    0.97
#define LIM_REL     0.00045             /* recovery per frame: ~45 ms to come back up */

typedef struct {
    double dx[LIM_N][2];                /* the audio, delayed by LIM_L frames */
    double tg[LIM_N];                   /* the gain each frame needs on its own */
    double sm[LIM_N];                   /* the windowed minimum, for the average */
    double smsum, gf;
    int    w;
} limiter;

static void lim_init(limiter *m)
{
    int i;
    memset(m, 0, sizeof *m);
    for (i = 0; i < LIM_N; i++) { m->tg[i] = 1.0; m->sm[i] = 1.0; }
    m->smsum = LIM_N;
    m->gf = 1.0;
}

static void lim_process(limiter *m, double *buf, int frames)
{
    int i, k;
    for (i = 0; i < frames; i++) {
        const double l = buf[2 * i], r = buf[2 * i + 1];
        double pk = l < 0 ? -l : l, pr = r < 0 ? -r : r, gmin = 1.0, avg;
        const int w = m->w, oldest = (w + 1) % LIM_N;
        if (pr > pk) pk = pr;
        if (pk != pk) { buf[2 * i] = buf[2 * i + 1] = 0.0; m->dx[w][0] = m->dx[w][1] = 0.0; pk = 0.0; }
        m->dx[w][0] = l == l ? l : 0.0;
        m->dx[w][1] = r == r ? r : 0.0;
        m->tg[w] = pk > LIM_CEIL ? LIM_CEIL / pk : 1.0;
        for (k = 0; k < LIM_N; k++) if (m->tg[k] < gmin) gmin = m->tg[k];
        m->smsum += gmin - m->sm[w];
        m->sm[w] = gmin;
        avg = m->smsum / LIM_N;
        m->gf = m->gf + (1.0 - m->gf) * LIM_REL;       /* recovering... */
        if (avg < m->gf) m->gf = avg;                  /* ...never above what the peak needs */
        buf[2 * i]     = m->dx[oldest][0] * m->gf;
        buf[2 * i + 1] = m->dx[oldest][1] * m->gf;
        m->w = oldest;
    }
}

#define AUD_MAX 8192                    /* the most frames one block may be */

/* The room one block of sample audio is made in, kept from open to close so
 * neither output allocates while it runs. */
static int audio_state_alloc(trk_engine *e)
{
    e->a_mix = calloc((size_t)AUD_MAX * 2, sizeof(double));
    e->a_tmp = calloc((size_t)AUD_MAX * 2, sizeof(double));
    e->a_due = calloc(SEV_MAX, sizeof(due_ev));
    e->a_lim = calloc(1, sizeof(limiter));
    if (!e->a_mix || !e->a_tmp || !e->a_due || !e->a_lim) {
        free(e->a_mix); free(e->a_tmp); free(e->a_due); free(e->a_lim);
        e->a_mix = e->a_tmp = e->a_due = e->a_lim = NULL;
        return -1;
    }
    lim_init(e->a_lim);
    return 0;
}

static void audio_state_free(trk_engine *e)
{
    free(e->a_mix); free(e->a_tmp); free(e->a_due); free(e->a_lim);
    e->a_mix = e->a_tmp = e->a_due = e->a_lim = NULL;
}

/* One block of P frames, into e->a_mix (interleaved stereo, limited): which
 * pending hits fall inside it, and where; then every kit rendered, split at
 * those points so each hit starts on its own sample. A hit's time is its tick
 * turned into wall time, plus alat -- the output's latency -- so that one
 * scheduled just ahead of the queue is never already late; the block's own
 * time is now plus `delay_s`, what the device still holds ahead of it. Late
 * anyway (an underrun), a hit plays at the block's start. Used by both
 * outputs: the ALSA thread and the PipeWire stream's callback. */
static void audio_render(trk_engine *e, int P, double delay_s)
{
    double *mix = e->a_mix, *tmp = e->a_tmp;
    due_ev *due = e->a_due;
    double now, pwall, spt;
    unsigned cur;
    int i, j, nd = 0, a, k;

    if (P > AUD_MAX) P = AUD_MAX;
    now = mono_now();
    pwall = now + delay_s;
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
        for (n = a; n < b && e->click_left > 0; n++, e->click_left--) {
            const double env = (double)e->click_left / (double)e->click_total;
            const double v = e->click_amp * env * sin(e->click_ph);
            e->click_ph += 6.283185307179586 * e->click_freq / KIT_RATE;
            mix[n * 2] += v;
            mix[n * 2 + 1] += v;
        }
        a = b;
    }
    pthread_mutex_unlock(&e->smx);

    {   /* The master volume, from the copy published at unlock. */
        const int vol = atomic_load_explicit(&e->vol_live, memory_order_relaxed);
        const double g = (vol < 0 ? 0 : vol > 150 ? 150 : vol) / 100.0;
        for (i = 0; i < P * 2; i++) mix[i] *= g;
    }
    lim_process(e->a_lim, mix, P);
    /* What a recorder of the whole studio takes: the mix as it leaves, limited. */
    if (atomic_load_explicit(&e->tap_on, memory_order_acquire)) {
        pthread_mutex_lock(&e->tapmx);
        if (e->tap) e->tap(e->tap_ud, mix, P);
        pthread_mutex_unlock(&e->tapmx);
    }
}

void trk_set_tap(trk_engine *e, trk_tap_fn fn, void *ud)
{
    pthread_mutex_lock(&e->tapmx);            /* waits out a call in flight */
    e->tap = fn;
    e->tap_ud = ud;
    atomic_store_explicit(&e->tap_on, fn != NULL, memory_order_release);
    pthread_mutex_unlock(&e->tapmx);
}

/* A dropout of the sample output, either kind: counted, and said with the time,
 * so a regular stall (something that runs once a second) shows as such. The
 * first few are all said, then one a second. Each one is an audible click. */
static void xrun_note(trk_engine *e, double *said, const char *why)
{
    const unsigned n = atomic_fetch_add_explicit(&e->xruns, 1, memory_order_relaxed) + 1;
    const double t = mono_now();
    /* A write to stderr from the audio thread can itself stall it, so: the first
     * few, then one in ten seconds. The count is what the windows show. */
    if (n <= 3 || t - *said >= 10.0) {
        fprintf(stderr, "trk: sample output dropout #%u at %.3f s (%s)\n", n, t, why);
        *said = t;
    }
}

/* The ALSA output: a thread of its own, one period at a time. */
static void *audio_main(void *ud)
{
    trk_engine *e = ud;
    const int P = e->aperiod;
    short *out = calloc((size_t)P * 2, sizeof *out);
    double xr_said = -10.0;
    int slow = 0, rt;
    {   /* A busy machine must not starve this thread into a dropout. */
        char how[160];
        rt = trk_audio_thread_realtime(10, how, sizeof how);
        fprintf(stderr, "trk: sample audio thread: %s\n", how);
    }

    while (out && !e->aquit) {
        snd_pcm_sframes_t delay = 0, w;
        const double t0 = mono_now();
        int i;

        if (snd_pcm_delay(e->pcm, &delay) < 0 || delay < 0) delay = 0;
        audio_render(e, P, (double)delay / KIT_RATE);
        /* A render that takes longer than the audio it makes, block after
         * block, would hold a realtime thread on the CPU and starve the
         * machine: give the priority back (the sound is late, not gone). */
        if (rt) {
            if (mono_now() - t0 > (double)P / KIT_RATE) slow++; else slow = 0;
            if (slow >= 50) {
                trk_audio_thread_normal();
                rt = 0;
                fprintf(stderr, "trk: sample audio thread: rendering is slower than real time, back to ordinary priority\n");
            }
        }
        for (i = 0; i < P * 2; i++) {
            double v = e->a_mix[i] * 32767.0;
            out[i] = (short)(v > 32767.0 ? 32767 : v < -32768.0 ? -32768 : v);
        }
        w = snd_pcm_writei(e->pcm, out, (snd_pcm_uframes_t)P);
        if (w < 0) xrun_note(e, &xr_said, snd_strerror((int)w));
        if (w < 0 && snd_pcm_recover(e->pcm, (int)w, 1) < 0) {
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);                /* broken device: do not spin */
        }
        /* A device that takes a period at once -- ALSA's null, say -- is
         * kept to the period's own length here. Unpaced, this loop would
         * spin on smx, and everything else that wants the samples would
         * wait on it for good. */
        {
            double took = mono_now() - t0, want = (double)P / KIT_RATE;
            if (took < want / 4) {
                struct timespec ts = { 0, (long)((want - took) * 1e9) };
                nanosleep(&ts, NULL);
            }
        }
    }
    free(out);
    return NULL;
}

/* The sinks' clock, mirroring the sample thread: one block at a time, every
 * queued event's tick turned into wall time against the queue, and each sink
 * called with the events that fall in the block and their sample offsets in
 * it. No output latency is added: the wall time is when the event should
 * reach the destination -- what the ALSA queue's timestamp means as well --
 * and the destination places it in its own block from there, as it does what
 * ALSA delivers. The callback runs under dmx, so trk_remove_sink taking that
 * lock has waited out any call in flight when it returns. */
static void *delivery_main(void *ud)
{
    trk_engine *e = ud;
    const double period = (double)SINK_PERIOD / TRK_SINK_RATE;
    trk_sink_ev *block = calloc(SINK_Q, sizeof *block);

    while (block && !e->dquit) {
        double now = mono_now(), spt;
        unsigned cur = queue_tick(e);
        int s;

        spt = 60.0 / ((e->bpm_now > 0 ? e->bpm_now : 120.0) * TRK_PPQ);
        pthread_mutex_lock(&e->dmx);
        for (s = 0; s < TRK_SINKS; s++) {
            trk_sink *k = &e->sink[s];
            int i, j, n = 0;
            if (!k->used || !k->fn) continue;
            for (i = j = 0; i < k->nq; i++) {
                const sink_qev *v = &k->q[i];
                double wall = now, f = 0;
                long frame = 0;
                if (!v->asap) {
                    wall = now + (double)(int)(v->tick - cur) * spt;
                    f = floor((wall - now) * TRK_SINK_RATE);
                    frame = f < 0 ? 0 : (long)f;
                }
                if (frame < SINK_PERIOD) {
                    /* In time order, an event stays behind the ones before it. */
                    int at = n;
                    while (at > 0 && block[at - 1].frame > frame) { block[at] = block[at - 1]; at--; }
                    block[at].frame = (uint32_t)frame;
                    block[at].status = v->st;
                    block[at].d1 = v->d1;
                    block[at].d2 = v->d2;
                    n++;
                } else {
                    k->q[j++] = *v;
                }
            }
            k->nq = j;
            if (n) k->fn(k->ud, now, block, n);
        }
        pthread_mutex_unlock(&e->dmx);
        /* Paced like the sample thread's unpaced branch. */
        {
            double took = mono_now() - now;
            if (took < period) {
                struct timespec ts = { 0, (long)((period - took) * 1e9) };
                nanosleep(&ts, NULL);
            }
        }
    }
    free(block);
    return NULL;
}

/* The buffer-size setting: how much room the output has before it runs dry.
 * Bigger is safer on a busy machine and costs latency, which only a note typed
 * to be heard (a preview) feels -- a song playing is scheduled ahead and lines
 * up whatever the size. ALSA is asked for milliseconds, PipeWire for a block
 * size in frames. */
static const int k_alsa_us[TRK_BUFFER_LEVELS]   = { 20000, 30000, 60000, 100000 };
static const int k_pw_frames[TRK_BUFFER_LEVELS] = { 256, 512, 1024, 2048 };
static const char *const k_buf_label[TRK_BUFFER_LEVELS] = {
    "Low latency", "Balanced", "Safe", "Extra safe"
};

const char *trk_audio_buffer_label(int level)
{ return level >= 0 && level < TRK_BUFFER_LEVELS ? k_buf_label[level] : ""; }

#ifdef TRK_HAVE_PIPEWIRE
/* The PipeWire output: a stream whose process callback runs on PipeWire's own
 * realtime thread, with the priority PipeWire itself obtained. That removes the
 * ALSA layer and the thread of ours, and puts the drums on the same clock as
 * the synth tabs' streams instead of a separate one. */
static void pw_process(void *ud)
{
    trk_engine *e = ud;
    struct pw_buffer *b;
    struct spa_buffer *sb;
    struct pw_time t;
    float *dst;
    double delay_s;
    int n, i;

    /* PipeWire does not hand a stream its xruns; a callback that comes much later
     * than the block before it was long is the same thing to the ear. A gap of
     * a second or more is the stream having been paused, not a dropout. */
    {
        const double now = mono_now(), gap = now - e->pw_last, blk = e->pw_blk;
        if (e->pw_last > 0 && blk > 0 && gap > 2.5 * blk && gap < 1.0) xrun_note(e, &e->pw_said, "late callback");
        e->pw_last = now;
    }
    if (!(b = pw_stream_dequeue_buffer(e->pwstream))) return;
    sb = b->buffer;
    if (!(dst = sb->datas[0].data)) { pw_stream_queue_buffer(e->pwstream, b); return; }
    n = (int)(sb->datas[0].maxsize / (sizeof(float) * 2));
    if (b->requested && (int)b->requested < n) n = (int)b->requested;
    if (n > AUD_MAX) n = AUD_MAX;

    e->pw_blk = (double)n / KIT_RATE;
    delay_s = (double)n / KIT_RATE;                 /* the next cycle, if the graph says nothing more */
    if (pw_stream_get_time_n(e->pwstream, &t, sizeof t) == 0 && t.rate.denom > 0 && t.delay > 0)
        delay_s = (double)t.delay * (double)t.rate.num / (double)t.rate.denom;
    if (!e->a_mix || e->aquit) {
        memset(dst, 0, (size_t)n * 2 * sizeof *dst);
    } else {
        audio_render(e, n, delay_s);
        for (i = 0; i < n * 2; i++) {
            const double v = e->a_mix[i];
            dst[i] = (float)(v > 1.0 ? 1.0 : v < -1.0 ? -1.0 : v);
        }
    }
    sb->datas[0].chunk->offset = 0;
    sb->datas[0].chunk->stride = sizeof(float) * 2;
    sb->datas[0].chunk->size = (uint32_t)(n * 2 * sizeof(float));
    pw_stream_queue_buffer(e->pwstream, b);
}

static void pw_recover_schedule(trk_engine *e);

/* The PipeWire server going away (a restart, a crash, the user's session
 * ending) leaves the stream in error or unconnected, and nothing plays any
 * more. Said here, on PipeWire's thread, which must not tear itself down: it
 * only flags the loss and wakes the thread that reopens the output. */
static void pw_state(void *ud, enum pw_stream_state old, enum pw_stream_state st, const char *error)
{
    trk_engine *e = ud;
    (void)old;
    if (!atomic_load_explicit(&e->pw_up, memory_order_acquire)) return;
    if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED) {
        fprintf(stderr, "trk: PipeWire sample stream lost (%s)\n", error ? error : "server gone");
        atomic_store(&e->pw_dead, 1);
        pw_recover_schedule(e);
    }
}

static const struct pw_stream_events k_pw_events = {
    PW_VERSION_STREAM_EVENTS, .process = pw_process, .state_changed = pw_state };

static void pw_stop(trk_engine *e)
{
    atomic_store_explicit(&e->pw_up, 0, memory_order_release);   /* the teardown's own state changes are not a loss */
    if (e->pwloop)   pw_thread_loop_stop(e->pwloop);
    if (e->pwstream) { pw_stream_destroy(e->pwstream); e->pwstream = NULL; }
    if (e->pwloop)   { pw_thread_loop_destroy(e->pwloop); e->pwloop = NULL; }
}

static int pw_start(trk_engine *e, char *why, size_t whyn)
{
    static int inited;
    const struct spa_pod *params[1];
    uint8_t pod[1024];
    struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    struct spa_audio_info_raw info;
    char lat[64];
    const int frames = k_pw_frames[e->buf_level];
    int tries;

    if (!inited) { pw_init(NULL, NULL); inited = 1; }
    if (!(e->pwloop = pw_thread_loop_new("vst-ace tracker", NULL))) {
        snprintf(why, whyn, "PipeWire is not available");
        return -1;
    }
    snprintf(lat, sizeof lat, "%d/%d", frames, KIT_RATE);
    e->pwstream = pw_stream_new_simple(
        pw_thread_loop_get_loop(e->pwloop), "tracker samples",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_LATENCY, lat,
                          PW_KEY_APP_NAME, "vst-ace tracker", NULL),
        &k_pw_events, e);
    if (!e->pwstream) { snprintf(why, whyn, "no PipeWire server"); pw_stop(e); return -1; }

    spa_zero(info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = KIT_RATE;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    params[0] = spa_format_audio_raw_build(&bb, SPA_PARAM_EnumFormat, &info);
    if (pw_stream_connect(e->pwstream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
        snprintf(why, whyn, "PipeWire refused the stream");
        pw_stop(e);
        return -1;
    }
    if (pw_thread_loop_start(e->pwloop) < 0) { snprintf(why, whyn, "PipeWire loop would not start"); pw_stop(e); return -1; }

    /* Connected, or not: wait a moment for the stream to leave "connecting". */
    for (tries = 0; tries < 100; tries++) {
        struct timespec ts = { 0, 10000000 };
        enum pw_stream_state st;
        pw_thread_loop_lock(e->pwloop);
        st = pw_stream_get_state(e->pwstream, NULL);
        pw_thread_loop_unlock(e->pwloop);
        if (st == PW_STREAM_STATE_ERROR) { snprintf(why, whyn, "PipeWire stream error"); pw_stop(e); return -1; }
        if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING) {
            atomic_store(&e->pw_dead, 0);
            atomic_store_explicit(&e->pw_up, 1, memory_order_release);
            return 0;
        }
        nanosleep(&ts, NULL);
    }
    snprintf(why, whyn, "PipeWire did not answer");
    pw_stop(e);
    return -1;
}
#endif

static void audio_close_l(trk_engine *e)
{
    if (!e->audio_on) return;
    e->aquit = 1;
#ifdef TRK_HAVE_PIPEWIRE
    if (e->use_pw) pw_stop(e);
    else
#endif
    {
        pthread_join(e->athread, NULL);
        snd_pcm_drop(e->pcm);
        snd_pcm_close(e->pcm);
        e->pcm = NULL;
    }
    audio_state_free(e);
    e->use_pw = 0;
    e->audio_on = 0;
#ifdef TRK_HAVE_PIPEWIRE
    atomic_store(&e->pw_dead, 0);
#endif
    e->aquit = 0;
}

/* The output, opened the first time a track wants a kit. The device is the
 * one chosen in the Audio output window (kept in a file beside the folder
 * lists), else TRK_PCM, else the system's default -- which is PipeWire's own
 * stream where PipeWire is there (and it falls back to ALSA's "default" where
 * it is not). Any named ALSA device, and TRK_PCM, are opened directly through
 * ALSA as before. */
static int audio_open_l(trk_engine *e)
{
    const char *dev = e->pcm_name[0] ? e->pcm_name : getenv("TRK_PCM");
    snd_pcm_uframes_t buf = 0, per = 0;
    int r;
    char pwwhy[96] = "";

    if (e->audio_on) return 0;
    if (audio_state_alloc(e)) {
        snprintf(e->audio_msg, sizeof e->audio_msg, "out of memory opening the audio output");
        return -1;
    }
    e->aquit = 0;
#ifdef TRK_HAVE_PIPEWIRE
    if (!dev || !*dev) {
        if (pw_start(e, pwwhy, sizeof pwwhy) == 0) {
            e->use_pw = 1;
            e->pw_last = 0; e->pw_blk = 0; e->pw_said = -10.0;
            e->aperiod = k_pw_frames[e->buf_level];
            e->alat = 2.0 * e->aperiod / KIT_RATE;
            e->audio_on = 1;
            snprintf(e->audio_msg, sizeof e->audio_msg, "samples: PipeWire, %d frames (%.0f ms), %s",
                     e->aperiod, 1000.0 * e->aperiod / KIT_RATE, k_buf_label[e->buf_level]);
            return 0;
        }
        /* No PipeWire: the ALSA default, as it always was. */
    }
#endif
    if (!dev || !*dev) dev = "default";
    if ((r = snd_pcm_open(&e->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        snprintf(e->audio_msg, sizeof e->audio_msg, "no audio output (%s): %s%s%s", dev, snd_strerror(r),
                 pwwhy[0] ? "; PipeWire: " : "", pwwhy);
        e->pcm = NULL;
        audio_state_free(e);
        return -1;
    }
    /* 30 ms by default: short enough to play along to, long enough not to
     * drop out when the machine is busy. The buffer-size setting moves it. */
    if ((r = snd_pcm_set_params(e->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                2, KIT_RATE, 1, k_alsa_us[e->buf_level])) < 0) {
        snprintf(e->audio_msg, sizeof e->audio_msg, "audio output (%s) refused 48 kHz stereo: %s%s%s",
                 dev, snd_strerror(r), pwwhy[0] ? "; PipeWire: " : "", pwwhy);
        snd_pcm_close(e->pcm);
        e->pcm = NULL;
        audio_state_free(e);
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
        audio_state_free(e);
        return -1;
    }
    e->audio_on = 1;
    snprintf(e->audio_msg, sizeof e->audio_msg, "samples: %s, %.0f ms%s%s", dev, e->alat * 1000,
             pwwhy[0] ? " -- " : "", pwwhy);
    return 0;
}

/* The list of sample-set folders on disk. Read at most every KIT_RESCAN_S; the
 * scan is a walk of the folders, and it is not done under the engine's lock.
 * kmx guards the list, and is never held while taking the engine's lock. */
static void scan_kits(trk_engine *e, int force)
{
    pthread_mutex_lock(&e->kmx);
    if (force || !e->nplaces || mono_now() - e->scanned_at >= KIT_RESCAN_S) {
        e->nplaces = drumkit_find(e->places, DK_MAX_KITS);
        e->scanned_at = mono_now();
    }
    pthread_mutex_unlock(&e->kmx);
}

/* A set that would not load is not tried again at every pass of the 2 s route
 * timer -- that is a disk read and a decode each time for a file that is still
 * bad -- but after KIT_RESCAN_S, or when the user reloads it. */
static int kit_known_bad(trk_engine *e, const char *name)
{
    int i, r = 0;
    pthread_mutex_lock(&e->kmx);
    for (i = 0; i < e->nbad; i++)
        if (!strcmp(e->bad[i].name, name) && mono_now() - e->bad[i].at < KIT_RESCAN_S) r = 1;
    pthread_mutex_unlock(&e->kmx);
    return r;
}

static void kit_mark_bad(trk_engine *e, const char *name)
{
    int i;
    pthread_mutex_lock(&e->kmx);
    for (i = 0; i < e->nbad; i++) if (!strcmp(e->bad[i].name, name)) break;
    if (i == e->nbad) i = e->nbad < 4 ? e->nbad++ : 0;
    snprintf(e->bad[i].name, sizeof e->bad[i].name, "%s", name);
    e->bad[i].at = mono_now();
    pthread_mutex_unlock(&e->kmx);
}

static void kit_forget_bad(trk_engine *e, const char *name)
{
    int i;
    pthread_mutex_lock(&e->kmx);
    e->kits_stale = 1;
    for (i = 0; i < e->nbad; i++) if (!strcmp(e->bad[i].name, name)) e->bad[i].at = -1e9;
    pthread_mutex_unlock(&e->kmx);
}

/* A sample set's folder from what a song calls it: the name the track's
 * list gives it, the last part of that ("Clap" for "The Cave Drumkit V3 /
 * Clap"), or a path -- what Load Sample Set gives. */
static int kit_lookup(trk_engine *e, const char *name, char *out, size_t n)
{
    int i, r = -1;
    pthread_mutex_lock(&e->kmx);
    for (i = 0; i < e->nplaces && r < 0; i++)
        if (!strcmp(e->places[i].name, name)) { snprintf(out, n, "%s", e->places[i].path); r = 0; }
    for (i = 0; i < e->nplaces && r < 0; i++) {
        const char *last = strstr(e->places[i].name, " / ");
        if (last && !strcmp(last + 3, name)) { snprintf(out, n, "%s", e->places[i].path); r = 0; }
    }
    pthread_mutex_unlock(&e->kmx);
    return r;
}

/* A name that is not in the list gets a fresh look at the disk -- a folder may
 * have been put there since -- but not more than once in KIT_RESCAN_S, or the
 * route timer would walk every kit folder every pass for a set that is simply
 * missing. A set the user just added or reloaded may rescan at once. */
static int kit_path(trk_engine *e, const char *name, char *out, size_t n)
{
    int again;
    if (name[0] == '/') { snprintf(out, n, "%s", name); return 0; }
    scan_kits(e, 0);
    if (kit_lookup(e, name, out, n) == 0) return 0;
    pthread_mutex_lock(&e->kmx);
    again = e->kits_stale || mono_now() - e->forced_at >= KIT_RESCAN_S;
    if (again) { e->kits_stale = 0; e->forced_at = mono_now(); }
    pthread_mutex_unlock(&e->kmx);
    if (!again) return -1;
    scan_kits(e, 1);
    return kit_lookup(e, name, out, n);
}

/* Opening and closing the output are one at a time: several paths ask for it
 * (loading a set, a take's click, an audition, the settings window), and two
 * at once would build the buffers twice under a running audio thread. The
 * _l forms are for a caller that already holds amx and does several steps. */
static void audio_close(trk_engine *e)
{
    pthread_mutex_lock(&e->amx);
    audio_close_l(e);
    pthread_mutex_unlock(&e->amx);
}

static int audio_open(trk_engine *e)
{
    int r;
    pthread_mutex_lock(&e->amx);
    r = audio_open_l(e);
    pthread_mutex_unlock(&e->amx);
    e->audio_fail_at = r ? mono_now() : 0;
    return r;
}

#ifdef TRK_HAVE_PIPEWIRE
/* Reopens the output after the PipeWire stream was lost: a moment's wait for
 * the server to be back, then close and open again -- which is PipeWire if it
 * is there, ALSA if it is not -- and again, a few times, if that fails. */
static void *recover_main(void *ud)
{
    trk_engine *e = ud;
    int attempt, gave_up = 0;
    for (attempt = 0; attempt < 20 && !atomic_load(&e->closing); attempt++) {
        struct timespec ts = { 0, 0 };
        int r = -1;
        const int ms = 300 * (attempt + 1) > 3000 ? 3000 : 300 * (attempt + 1);
        ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        if (atomic_load(&e->closing)) break;
        pthread_mutex_lock(&e->amx);
        if (e->audio_on && !atomic_load(&e->pw_dead)) { pthread_mutex_unlock(&e->amx); break; }   /* sorted already */
        if (!e->audio_on && attempt == 0) { pthread_mutex_unlock(&e->amx); break; }               /* closed on purpose */
        audio_close_l(e);
        r = audio_open_l(e);
        pthread_mutex_unlock(&e->amx);
        fprintf(stderr, "trk: sample output reopened after losing PipeWire: %s\n", e->audio_msg);
        if (r == 0) { gave_up = 0; break; }
        gave_up = 1;
    }
    if (gave_up && !atomic_load(&e->closing)) {
        /* Said where the user looks: the Audio output window and the status line. */
        pthread_mutex_lock(&e->amx);
        snprintf(e->audio_msg, sizeof e->audio_msg,
                 "sample output lost and it would not come back -- choose it again in Audio output");
        pthread_mutex_unlock(&e->amx);
        fprintf(stderr, "trk: %s\n", e->audio_msg);
    }
    pthread_mutex_lock(&e->rmx);
    e->rrunning = 0;
    pthread_mutex_unlock(&e->rmx);
    return NULL;
}

static void pw_recover_schedule(trk_engine *e)
{
    pthread_mutex_lock(&e->rmx);
    if (!atomic_load(&e->closing) && !e->rrunning) {
        if (e->rstarted) pthread_join(e->rthread, NULL);       /* the last one has finished */
        e->rrunning = pthread_create(&e->rthread, NULL, recover_main, e) == 0;
        e->rstarted = e->rrunning;
    }
    pthread_mutex_unlock(&e->rmx);
}
#endif

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
        char path[TRK_PATH_LEN];
        int have = 0;
        if (!want[t][0]) continue;
        if (strcmp(want[t], stale))
            for (i = 0; i < e->nkit; i++) if (!strcmp(e->kit[i].name, want[t])) have = 1;
        for (i = 0; i < nfresh; i++) if (!strcmp(fresh[i].name, want[t])) have = 1;
        if (have || kit_known_bad(e, want[t]) || kit_path(e, want[t], path, sizeof path)) continue;
        snprintf(fresh[nfresh].name, sizeof fresh[nfresh].name, "%s", want[t]);
        fresh[nfresh].dk = drumkit_load(path, KIT_RATE);
        drumkit_set_raw(fresh[nfresh].dk, 1);          /* the mix is limited in audio_main */
        if (fresh[nfresh].dk) nfresh++;
        else kit_mark_bad(e, want[t]);
    }
    /* Not at every pass of the route timer when it has just failed: opening a
     * device that is not there is a PipeWire attempt and an ALSA one. */
    if (any && !(e->audio_fail_at > 0 && mono_now() - e->audio_fail_at < KIT_RESCAN_S)) audio_open(e);

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

/* A sample track's note keys sit on its set: octave = the one holding the
 * set's lowest pad. Whatever the song file or an earlier set left in the
 * field is overwritten, and the UIs keep the control locked, so the pads
 * stay under the keys. Called with e->lock held. */
static void fit_sample_octaves(trk_engine *e)
{
    int t;
    pthread_mutex_lock(&e->smx);
    for (t = 0; t < TRK_TRACKS; t++) {
        int k = e->track_kit[t], n, oct;
        if (k < 0 || !is_sampled(&e->song.track[t])) continue;
        for (n = 0; n < 128; n++) if (drumkit_slot_at(e->kit[k].dk, n) >= 0) break;
        if (n == 128) continue;
        oct = n / 12 - 1;
        e->song.track[t].octave = oct < 0 ? 0 : oct > 9 ? 9 : oct;
    }
    pthread_mutex_unlock(&e->smx);
}

int trk_route(trk_engine *e)
{
    snd_seq_addr_t want[TRK_TRACKS], clk[TRK_TRACKS];
    int ok[TRK_TRACKS], t, i, missing, nclk = 0, moved = 0;

    missing = load_samples(e);
    pthread_mutex_lock(&e->lock);
    for (t = 0; t < TRK_TRACKS; t++) {
        const trk_track *k = &e->song.track[t];
        /* A sample track plays no window, and a sink-routed track plays no
         * window either: whatever window it played before is released and let
         * go below, as for any track that moves. */
        ok[t] = !is_sampled(k) && e->track_sink[t] < 0 &&
                resolve(e, k->client, k->port, &want[t]) == 0;
        if (k->client[0] && !is_sampled(k) && e->track_sink[t] < 0 && !ok[t]) missing++;
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
            e->vol_sent[t] = 0;          /* the next synth has not been told the level */
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
    fit_sample_octaves(e);
    if (moved && e->playing) top_up(e);
    pthread_mutex_unlock(&e->lock);
    return missing;
}

int trk_routed(trk_engine *e, int t)
{
    int r;
    if (t < 0 || t >= TRK_TRACKS) return 0;
    pthread_mutex_lock(&e->lock);
    r = is_sampled(&e->song.track[t]) ? e->track_kit[t] >= 0
                                      : e->track_sink[t] >= 0 ? 1 : e->routed[t];
    pthread_mutex_unlock(&e->lock);
    return r;
}

/* ------------------------------------------------------------------ sinks */

/* The loudest fader among the tracks that play sink `s`, 0..100, or -1 when no
 * track plays it. */
static int sink_max_volume(trk_engine *e, int s)
{
    int t, best = -1;
    for (t = 0; t < e->song.ntracks && t < TRK_TRACKS; t++)
        if (e->track_sink[t] == s && e->song.track[t].volume > best) best = e->song.track[t].volume;
    return best;
}

/* Hand each sink its gain: the loudest fader among the tracks playing it. One
 * track alone is that synth's fader exactly; several share one synth output, so
 * the output follows the loudest and each track is made quieter than that by
 * velocity (see track_rel_volume). Called with the engine's lock held. */
static void sink_gains_locked(trk_engine *e)
{
    int s;
    for (s = 0; s < TRK_SINKS; s++) {
        int m;
        double g = 1.0;
        if (!e->sink[s].used || !e->sink[s].gain_cb) continue;
        m = sink_max_volume(e, s);
        if (m >= 0) g = m / 100.0;
        if (g != e->sink[s].gain_sent) {
            e->sink[s].gain_sent = g;
            e->sink[s].gain_cb(e->sink[s].ud, g);
        }
    }
}

void trk_sink_set_gain_cb(trk_engine *e, int id, trk_gain_fn cb)
{
    if (!e || id < 0 || id >= TRK_SINKS) return;
    pthread_mutex_lock(&e->lock);
    if (e->sink[id].used) {
        e->sink[id].gain_cb = cb;
        e->sink[id].gain_sent = -1.0;              /* so the first refresh always sends */
        sink_gains_locked(e);
    }
    pthread_mutex_unlock(&e->lock);
}

void trk_sink_gains_refresh(trk_engine *e)
{
    if (!e) return;
    pthread_mutex_lock(&e->lock);
    sink_gains_locked(e);
    pthread_mutex_unlock(&e->lock);
}

/* A track on a sink that has a gain: its fader is carried by that gain, and what
 * is left for the notes is its volume relative to the loudest track on the same
 * synth (100 for the loudest, or the only one). Any other track keeps its own
 * volume as the velocity scale. */
static int track_on_gain(trk_engine *e, int t)
{
    const int s = e->track_sink[t];
    return s >= 0 && e->sink[s].gain_cb;
}

static int track_rel_volume(trk_engine *e, int t)
{
    const int v = e->song.track[t].volume;
    int m;
    if (!track_on_gain(e, t) || v <= 0) return v;
    m = sink_max_volume(e, e->track_sink[t]);
    return m > 0 ? v * 100 / m : v;
}

int trk_add_sink(trk_engine *e, const char *name, trk_sink_fn fn, void *ud)
{
    int s;
    if (!e || !fn) return -1;
    pthread_mutex_lock(&e->dmx);
    for (s = 0; s < TRK_SINKS; s++) if (!e->sink[s].used) break;
    if (s == TRK_SINKS) { pthread_mutex_unlock(&e->dmx); return -1; }
    snprintf(e->sink[s].name, sizeof e->sink[s].name, "%s", name ? name : "sink");
    e->sink[s].fn = fn;
    e->sink[s].ud = ud;
    e->sink[s].nq = 0;
    e->sink[s].gain_cb = NULL;
    e->sink[s].gain_sent = 1.0;
    e->sink[s].used = 1;
    /* The delivery thread starts with the first sink. */
    if (!e->delivery_on) {
        e->dquit = 0;
        if (pthread_create(&e->dthread, NULL, delivery_main, e)) {
            e->sink[s].used = 0;
            pthread_mutex_unlock(&e->dmx);
            return -1;
        }
        e->delivery_on = 1;
    }
    pthread_mutex_unlock(&e->dmx);
    return s;
}

void trk_remove_sink(trk_engine *e, int id)
{
    int t, i;
    if (!e || id < 0 || id >= TRK_SINKS) return;
    pthread_mutex_lock(&e->lock);
    if (!e->sink[id].used) { pthread_mutex_unlock(&e->lock); return; }
    /* Tracks playing it go back to their windows: what they have sounding is
     * released first, while the sink is still there to hear it, and what was
     * scheduled for it is taken back so nothing lands after the move. The
     * sink name saved in the song goes with the routing. */
    if (e->playing) rewind_locked(e);
    for (t = 0; t < TRK_TRACKS; t++)
        if (e->track_sink[t] == id) {
            release_track(e, t);
            e->track_sink[t] = -1;
            e->vol_sent[t] = 0;
            memset(e->song.track[t].sink, 0, sizeof e->song.track[t].sink);
        }
    /* The releases just queued are only releases once they are delivered:
     * wait out the delivery thread's next passes before the slot is let go.
     * Asap events go out on the first pass, so this is milliseconds. */
    for (i = 0; i < 50; i++) {
        struct timespec ts = { 0, 2000000 };
        int empty;
        pthread_mutex_lock(&e->dmx);
        empty = e->sink[id].nq == 0;
        pthread_mutex_unlock(&e->dmx);
        if (empty) break;
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&e->dmx);
    e->sink[id].used = 0;
    e->sink[id].fn = NULL;
    e->sink[id].ud = NULL;
    e->sink[id].nq = 0;
    pthread_mutex_unlock(&e->dmx);
    if (e->playing) top_up(e);
    pthread_mutex_unlock(&e->lock);
    /* dmx taken and released above: the delivery thread was either out of the
     * callback already or is now, and with used cleared it never enters it
     * again. The tracks' windows reconnect here rather than waiting for the
     * caller's next trk_route. */
    trk_route(e);
}

void trk_sink_rename(trk_engine *e, int id, const char *name)
{
    int t;
    if (!e || id < 0 || id >= TRK_SINKS || !name) return;
    pthread_mutex_lock(&e->lock);
    pthread_mutex_lock(&e->dmx);
    if (e->sink[id].used) {
        char nm[TRK_DEST_LEN];           /* bounced: the sink and the song are both e's */
        snprintf(e->sink[id].name, sizeof e->sink[id].name, "%s", name);
        snprintf(nm, sizeof nm, "%s", e->sink[id].name);
        /* Tracks playing it saved the old name: keep what a song saves
         * naming the sink they actually play. Cleared whole first, as in
         * trk_route_sink. */
        for (t = 0; t < TRK_TRACKS; t++)
            if (e->track_sink[t] == id) {
                memset(e->song.track[t].sink, 0, sizeof e->song.track[t].sink);
                strcpy(e->song.track[t].sink, nm);
            }
    }
    pthread_mutex_unlock(&e->dmx);
    pthread_mutex_unlock(&e->lock);
}

void trk_route_sink(trk_engine *e, int t, int id)
{
    if (!e || t < 0 || t >= TRK_TRACKS || id >= TRK_SINKS) return;
    pthread_mutex_lock(&e->lock);
    if (id >= 0 && !e->sink[id].used) { pthread_mutex_unlock(&e->lock); return; }
    if (e->track_sink[t] != id) {
        /* As a window re-route: what was scheduled is taken back, what is
         * sounding is released to the destination being left, and the
         * schedule carries on from the first row still ahead. */
        if (e->playing) rewind_locked(e);
        release_track(e, t);
        if (id >= 0 && e->routed[t]) {
            snd_seq_disconnect_to(e->seq, e->port[t], e->dest[t].client, e->dest[t].port);
            e->routed[t] = 0;
        }
        e->track_sink[t] = id;
        e->vol_sent[t] = 0;              /* a different synth: tell it the level afresh */
        /* The pick is part of the song now -- saved with the track, so the
         * shell opens the same synth again when the song is opened. Routing
         * back clears it. Cleared whole, so a reloaded song matches the live
         * one byte for byte, as the front-ends' dirty check compares it. */
        pthread_mutex_lock(&e->dmx);
        memset(e->song.track[t].sink, 0, sizeof e->song.track[t].sink);
        if (id >= 0) {
            char nm[TRK_DEST_LEN];       /* bounced: the sink and the song are both e's */
            snprintf(nm, sizeof nm, "%s", e->sink[id].name);
            strcpy(e->song.track[t].sink, nm);
        }
        pthread_mutex_unlock(&e->dmx);
        if (e->playing) top_up(e);
    }
    sink_gains_locked(e);
    pthread_mutex_unlock(&e->lock);
    if (id < 0) trk_route(e);            /* back to the window: reconnect it */
}

/* A song is about to take the place of the one in the engine: the tracks
 * routed to in-process synths go back to their windows first, so the routing
 * the engine holds and the sink names the new song carries agree. A shell
 * routes them to synths again from those names. */
void trk_unroute_sinks(trk_engine *e)
{
    int t;
    for (t = 0; t < TRK_TRACKS; t++)
        if (trk_sink_of(e, t) >= 0) trk_route_sink(e, t, -1);
}

int trk_sink_of(trk_engine *e, int t)
{
    int r;
    if (!e || t < 0 || t >= TRK_TRACKS) return -1;
    pthread_mutex_lock(&e->lock);
    r = e->track_sink[t];
    pthread_mutex_unlock(&e->lock);
    return r;
}

int trk_sink_name(trk_engine *e, int id, char *buf, size_t n)
{
    int ok;
    if (!e || id < 0 || id >= TRK_SINKS) return -1;
    if (n) buf[0] = 0;
    pthread_mutex_lock(&e->dmx);
    ok = e->sink[id].used;
    if (ok && n) snprintf(buf, n, "%s", e->sink[id].name);
    pthread_mutex_unlock(&e->dmx);
    return ok ? 0 : -1;
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
    /* No engine lock: the queries are ioctls that read the kernel's client and
     * port tables, they touch none of the handle's buffers, and e->seq and
     * e->client do not change after open. Held, this walk -- a kernel round
     * trip per client and port -- stalled the note path every 2 s. */
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
    return count;
}

int trk_list_sample_sets(trk_engine *e, char *buf, size_t n)
{
    size_t used = 0;
    int i, count = 0;
    if (n) buf[0] = 0;
    scan_kits(e, 0);                    /* the folders are walked before the engine's lock is taken */
    pthread_mutex_lock(&e->lock);
    pthread_mutex_lock(&e->kmx);
    /* The ones loaded from elsewhere first: they were asked for by name. */
    for (i = 0; i < e->nadded + e->nplaces; i++) {
        const char *name = i < e->nadded ? e->added[i] : e->places[i - e->nadded].name;
        const char *path = i < e->nadded ? e->added[i] : e->places[i - e->nadded].path;
        int w = snprintf(buf + used, n - used, "%s\t%s\n", name, path);
        if (w < 0 || (size_t)w >= n - used) { buf[used] = 0; break; }
        used += (size_t)w;
        count++;
    }
    pthread_mutex_unlock(&e->kmx);
    pthread_mutex_unlock(&e->lock);
    return count;
}

void trk_add_sample_set(trk_engine *e, const char *path)
{
    int i;
    kit_forget_bad(e, path);
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
    char p[TRK_PATH_LEN];
    if (kit_path(e, name, p, sizeof p)) return -1;      /* a disk walk at worst: not under the engine's lock */
    snprintf(out, n, "%s", p);
    return 0;
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
    kit_forget_bad(e, name);
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
    drumkit_set_raw(k, 1);                          /* the mix is limited in audio_main */
    free(m);
    if (!k || audio_open(e)) { drumkit_free(k); return -1; }
    pthread_mutex_lock(&e->smx);
    old = e->aud;
    e->aud = k;
    pthread_mutex_unlock(&e->smx);
    push_sample(e, 0, 1, SEV_AUDITION, -1, 60, vel, -1);
    drumkit_free(old);
    return 0;
}

/* ------------------------------------------------------------- transport *//* ------------------------------------------------------------- transport */

static void stop_locked(trk_engine *e, int send_stop)
{
    int t;
    e->recording = 0;                   /* a take ends with the playing; its events stay to be saved */
    if (e->playing) {
        e->playing = 0;
        unschedule(e);
        drop_sinks(e, 1, 0);
        snd_seq_stop_queue(e->seq, e->queue, NULL);
        snd_seq_drain_output(e->seq);
    }
    if (send_stop) {
        snd_seq_event_t ev;
        ev_base(e, &ev, e->clock_port);
        ev.type = SND_SEQ_EVENT_STOP;
        now(e, &ev);
        clock_to_sinks(e, 0, 1, 0xFC, 0, 0);
    }
    for (t = 0; t < TRK_TRACKS; t++) release_track(e, t);
    /* Hits still to come go, and what is sounding fades, as a held MIDI
     * note is released: a long sample should stop when the song does. */
    drop_samples(e, 1, 0);
    push_sample(e, 0, 1, SEV_FADE, -1, 0, 0, -1);
    e->npos = 0;
}

/* Playing from a row, the first row `lead` ticks after the usual start: a
 * count-in's worth of quiet in front of it. The caller holds the lock. */
static void play_locked(trk_engine *e, int mode, int order, int row, unsigned lead)
{
    snd_seq_event_t ev;
    int beats_rows = 0, i, sixteenths;

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
                              / 60.0 * TRK_PPQ) + lead;
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
    clock_to_sinks(e, 0, 1, 0xF2, sixteenths & 0x7F, (sixteenths >> 7) & 0x7F);
    clock_to_sinks(e, 0, 1, row == 0 && beats_rows == 0 ? 0xFA : 0xFB, 0, 0);
    snd_seq_drain_output(e->seq);
    e->playing = 1;
    top_up(e);
}

void trk_play(trk_engine *e, int mode, int order, int row)
{
    pthread_mutex_lock(&e->lock);
    memset(e->vol_sent, 0, sizeof e->vol_sent);      /* a fresh run tells each synth its level again */
    play_locked(e, mode, order, row, 0);
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

static void position_locked(trk_engine *e, int *order, int *pattern, int *row)
{
    int o = -1, p = -1, r = -1;
    if (e->playing && e->npos) {
        unsigned cur = queue_tick(e), i, n = e->npos < POS_RING ? e->npos : POS_RING;
        /* The latest row whose time has come. Scheduled ones still ahead of
         * the queue are not playing yet, whatever has been sent. */
        for (i = 1; i <= n; i++) {
            const pos_mark *m = &e->pos[(e->npos - i) % POS_RING];
            if (m->tick <= cur) { o = m->order; p = m->pattern; r = m->row; break; }
        }
    }
    if (order) *order = o;
    if (pattern) *pattern = p;
    if (row) *row = r;
}

void trk_position(trk_engine *e, int *order, int *pattern, int *row)
{
    pthread_mutex_lock(&e->lock);
    position_locked(e, order, pattern, row);
    pthread_mutex_unlock(&e->lock);
}

static void levels_locked(trk_engine *e, float out[TRK_TRACKS])
{
    struct timespec ts;
    double now, dt;
    int t;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = ts.tv_sec + ts.tv_nsec * 1e-9;
    dt = e->meter_time > 0 ? now - e->meter_time : 0;
    if (dt < 0 || dt > 1) dt = 0.033;
    e->meter_time = now;
    for (t = 0; t < TRK_TRACKS; t++) e->meter[t] *= (float)exp(-dt / 0.55);
    if (e->playing && e->npos) {
        unsigned cur = queue_tick(e), i;
        if (e->meter_seen > e->npos || e->npos - e->meter_seen > POS_RING) e->meter_seen = e->npos;
        for (i = e->meter_seen; i < e->npos; i++) {
            const pos_mark *m = &e->pos[i % POS_RING];
            if (m->tick > cur) break;                /* not sounding yet */
            for (t = 0; t < e->song.ntracks; t++) {
                const trk_track *k = &e->song.track[t];
                const trk_cell *c = &e->song.pattern[m->pattern].cell[m->row][t];
                if (k->mute || c->note > 127) continue;
                {
                    int vel = c->vel != TRK_EMPTY ? c->vel : k->velocity;
                    float v = (vel < 1 ? 1 : vel > 127 ? 127 : vel) / 127.0f;
                    if (v > e->meter[t]) e->meter[t] = v;
                }
            }
            e->meter_seen = i + 1;
        }
    } else {
        e->meter_seen = e->npos;
    }
    for (t = 0; t < TRK_TRACKS; t++) out[t] = e->meter[t];
}

void trk_levels(trk_engine *e, float out[TRK_TRACKS])
{
    pthread_mutex_lock(&e->lock);
    levels_locked(e, out);
    pthread_mutex_unlock(&e->lock);
}

void trk_set_bpm(trk_engine *e, double bpm)
{
    if (bpm != bpm) return;                 /* NaN is no tempo: the one there stays */
    if (bpm < 20.0) bpm = 20.0;
    if (bpm > 999.0) bpm = 999.0;
    pthread_mutex_lock(&e->lock);
    e->song.bpm = bpm;
    set_tempo(e, bpm);
    /* What is queued was laid down against the old tempo: the MIDI events
     * carry absolute ticks and the kernel rescales them, but next_tick and
     * the sample hits pending in sev were worked out from the old one --
     * left as they are, the sample and MIDI tracks part by up to the
     * lookahead on every change. Take the schedule back and lay it down
     * again, as a re-route does. */
    if (e->playing) {
        rewind_locked(e);
        top_up(e);
    }
    pthread_mutex_unlock(&e->lock);
}

/* --------------------------------------------------------------- preview */

static void preview_off_locked(trk_engine *e, int t)
{
    if (e->prev_note[t] < 0) return;
    track_ev(e, t, 0, 1, 0x80 | e->prev_ch[t], e->prev_note[t], 0);
    snd_seq_drain_output(e->seq);
    e->prev_note[t] = -1;
}

void trk_preview(trk_engine *e, int t, int note, int vel)
{
    int ch;
    if (t < 0 || t >= TRK_TRACKS || note < 0 || note > 127) return;
    pthread_mutex_lock(&e->lock);
    {   /* the track's fader applies to what is typed too: through the synth's gain
         * when the track is alone on it, else as velocity */
        trk_track kk = e->song.track[t];
        kk.volume = track_rel_volume(e, t);
        vel = trk_track_velocity(&kk, vel);
    }
    if (vel < 1) { pthread_mutex_unlock(&e->lock); return; }
    e->meter[t] = vel / 127.0f;
    if (is_sampled(&e->song.track[t])) {
        /* Played out rather than held: a preview of a drum should be the
         * whole hit, however quickly the key comes back up. */
        if (e->track_kit[t] >= 0)
            push_sample(e, 0, 1, SEV_PLAY, e->track_kit[t], note, vel, t);
        pthread_mutex_unlock(&e->lock);
        return;
    }
    preview_off_locked(e, t);
    ch = e->song.track[t].channel & 15;
    if (!track_on_gain(e, t)) track_volume(e, t, 0, 1);
    track_ev(e, t, 0, 1, 0x90 | ch, note, vel);
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


/* ------------------------------------------------------------- recording */

static unsigned row_ticks(trk_engine *e)
{
    return (unsigned)(TRK_PPQ / (trk_lpb_ok(e->song.lpb) ? e->song.lpb : 4));
}

/* The row after (o, p, r) as the schedule would take it. */
static void next_row_of(trk_engine *e, int o, int p, int r, int *no, int *np, int *nr)
{
    const trk_song *s = &e->song;
    *no = o; *np = p; *nr = r + 1;
    if (*nr >= s->pattern[p].rows) {
        *nr = 0;
        if (e->mode == TRK_PLAY_SONG) {
            *no = o + 1 >= s->norder ? 0 : o + 1;
            *np = s->order[*no];
        }
    }
}

/* Where a tick falls: the row scheduled at or before it, and, when the
 * quantizing is to the nearest and the tick is in its second half, the row
 * after. A tick just ahead of the take's first row -- a note struck a hair
 * early on the downbeat -- takes that row; one further ahead (the count-in)
 * is nobody's. Returns 0 when the tick has no row. */
static int rec_place(trk_engine *e, unsigned tick, int *po, int *pp, int *pr)
{
    const unsigned rt = row_ticks(e);
    unsigned i, n = e->npos < POS_RING ? e->npos : POS_RING;
    const pos_mark *m = NULL;
    int o, p, r;

    for (i = 1; i <= n; i++) {
        const pos_mark *c = &e->pos[(e->npos - i) % POS_RING];
        if (c->tick <= tick) { m = c; break; }
    }
    if (!m) {
        /* Ahead of every row we know: only the first row of this run, and
         * only a little ahead of it, and only if rounding is allowed. */
        if (n == 0 || e->npos > POS_RING) return 0;
        m = &e->pos[(e->npos - n) % POS_RING];
        if (e->ropt.quantize != TRK_REC_NEAREST || m->tick - tick > rt / 2) return 0;
        *po = m->order; *pp = m->pattern; *pr = m->row;
        return 1;
    }
    o = m->order; p = m->pattern; r = m->row;
    if (e->ropt.quantize == TRK_REC_NEAREST && (tick - m->tick) * 2 >= rt)
        next_row_of(e, o, p, r, &o, &p, &r);
    if (p < 0 || p >= TRK_PATTERNS) return 0;
    if (r >= e->song.pattern[p].rows) r = e->song.pattern[p].rows - 1;
    *po = o; *pp = p; *pr = r;
    return 1;
}

static void take_add(trk_engine *e, unsigned tick, int track, int kind, int chan, int a, int b)
{
    take_ev *v;
    if (e->ntake >= TAKE_MAX) return;                 /* a flood from another client stops here */
    if (e->ntake == e->captake) {
        size_t c = e->captake ? e->captake * 2 : 1024;
        take_ev *g = realloc(e->take, c * sizeof *g);
        if (!g) return;                               /* a full take is dropped, never the key */
        e->take = g; e->captake = c;
    }
    v = &e->take[e->ntake++];
    v->tick = tick > e->rec_first_tick ? tick - e->rec_first_tick : 0;
    v->track = (unsigned char)track; v->kind = (unsigned char)kind;
    v->chan = (unsigned char)chan; v->a = (unsigned char)a; v->b = (unsigned char)b;
}

/* The keyboard's offset as ticks: positive places a note earlier. */
static long offset_ticks(trk_engine *e)
{
    double bpm = e->bpm_now > 0 ? e->bpm_now : 120.0;
    return (long)(e->ropt.offset_ms * bpm / 60000.0 * TRK_PPQ);
}

/* The note-on, note-off and controller of a take, each at its tick. Lock held. */
static void record_on(trk_engine *e, unsigned tick, int t, int note, int vel)
{
    int o, p, r;
    trk_cell *c;
    if (!e->recording || !e->playing || t < 0 || t >= TRK_TRACKS || note < 0 || note > 127) return;
    if (!rec_place(e, tick, &o, &p, &r)) return;
    take_add(e, tick, t, TAKE_ON, e->song.track[t].channel & 15, note, vel >= 1 && vel <= 127 ? vel : e->song.track[t].velocity);
    c = &e->song.pattern[p].cell[r][t];
    c->note = (uint8_t)note;
    c->vel = vel >= 1 && vel <= 127 ? (uint8_t)vel : TRK_EMPTY;
    e->rec_note[t] = note; e->rec_pat[t] = p; e->rec_row[t] = r;
}

/* Release. The === goes to the row the release falls on -- or the next, if
 * that is the note's own row: a note is at least a row long. Never over a
 * cell with something in it. */
static void record_off(trk_engine *e, unsigned tick, int t, int note)
{
    int o, p, r;
    if (!e->recording || !e->playing || t < 0 || t >= TRK_TRACKS || note < 0 || note > 127) return;
    if (e->rec_note[t] != note) return;
    e->rec_note[t] = -1;
    take_add(e, tick, t, TAKE_OFF, e->song.track[t].channel & 15, note, 0);
    if (!e->ropt.note_off || !rec_place(e, tick, &o, &p, &r)) return;
    if (p == e->rec_pat[t] && r == e->rec_row[t]) next_row_of(e, o, p, r, &o, &p, &r);
    if (e->song.pattern[p].cell[r][t].note == TRK_EMPTY) {
        e->song.pattern[p].cell[r][t].note = TRK_NOTE_OFF;
        e->song.pattern[p].cell[r][t].vel = TRK_EMPTY;
    }
}

static void record_cc(trk_engine *e, unsigned tick, int t, int cc, int val)
{
    int o, p, r;
    if (!e->recording || !e->playing || t < 0 || t >= TRK_TRACKS || cc < 0 || cc > 127 || val < 0 || val > 127) return;
    if (!rec_place(e, tick, &o, &p, &r)) return;
    take_add(e, tick, t, TAKE_CC, e->song.track[t].channel & 15, cc, val);
    e->song.pattern[p].cell[r][t].cc = (uint8_t)cc;
    e->song.pattern[p].cell[r][t].val = (uint8_t)val;
}

/* The editor's, with the lock held (a key's). */
int trk_recording_locked(trk_engine *e) { return e->recording && e->playing; }

void trk_record_note_locked(trk_engine *e, int track, int note, int vel, int on)
{
    long t = (long)queue_tick(e) - offset_ticks(e);
    unsigned tick = t > 0 ? (unsigned)t : 0;
    if (on) record_on(e, tick, track, note, vel);
    else record_off(e, tick, track, note);
}

/* A "stop here" typed while recording: === on the row now playing. */
void trk_record_stopnote_locked(trk_engine *e, int track)
{
    int o, p, r;
    unsigned tick = queue_tick(e);
    if (track < 0 || track >= TRK_TRACKS) return;
    if (!e->recording || !e->playing || !rec_place(e, tick, &o, &p, &r)) return;
    e->song.pattern[p].cell[r][track].note = TRK_NOTE_OFF;
    e->song.pattern[p].cell[r][track].vel = TRK_EMPTY;
    e->rec_note[track] = -1;
}

void trk_record_note(trk_engine *e, int track, int note, int vel, int on)
{
    pthread_mutex_lock(&e->lock);
    trk_record_note_locked(e, track, note, vel, on);
    pthread_mutex_unlock(&e->lock);
}

void trk_record_defaults(trk_rec_opts *o)
{
    o->count_in = 1;
    o->metronome = 1;
    o->quantize = TRK_REC_NEAREST;
    o->note_off = 1;
    o->monitor = 1;
    o->offset_ms = 0;
}

static void clamp_opts(trk_rec_opts *o)
{
    if (o->count_in < 0) o->count_in = 0;
    if (o->count_in > 4) o->count_in = 4;
    o->metronome = o->metronome != 0;
    o->quantize = o->quantize == TRK_REC_ROW ? TRK_REC_ROW : TRK_REC_NEAREST;
    o->note_off = o->note_off != 0;
    o->monitor = o->monitor != 0;
    if (o->offset_ms < -200) o->offset_ms = -200;
    if (o->offset_ms > 200) o->offset_ms = 200;
}

void trk_record_set(trk_engine *e, const trk_rec_opts *o)
{
    trk_rec_opts c = *o;
    clamp_opts(&c);
    pthread_mutex_lock(&e->lock);
    e->ropt = c;
    pthread_mutex_unlock(&e->lock);
}

void trk_record_get(trk_engine *e, trk_rec_opts *o)
{
    pthread_mutex_lock(&e->lock);
    *o = e->ropt;
    pthread_mutex_unlock(&e->lock);
}

void trk_record_arm(trk_engine *e, int track)
{
    if (track < 0 || track >= TRK_TRACKS) return;
    pthread_mutex_lock(&e->lock);
    e->rec_track = track;
    pthread_mutex_unlock(&e->lock);
}

static int recording_locked(trk_engine *e)
{
    return e->recording && e->playing ? (e->rec_lead && queue_tick(e) < e->rec_first_tick ? 2 : 1) : 0;
}

int trk_recording(trk_engine *e)
{
    int r;
    pthread_mutex_lock(&e->lock);
    r = recording_locked(e);
    pthread_mutex_unlock(&e->lock);
    return r;
}

/* What a window's playback timer wants, from one take of the lock instead of
 * three, thirty times a second. `quiet` says nothing is playing or recording
 * and every meter has died away: the timer can slow down until that changes. */
void trk_poll(trk_engine *e, trk_poll_state *st)
{
    int t;
    pthread_mutex_lock(&e->lock);
    position_locked(e, &st->order, &st->pattern, &st->row);
    levels_locked(e, st->level);
    st->recording = recording_locked(e);
    st->playing = e->playing;
    pthread_mutex_unlock(&e->lock);
    st->xruns = trk_audio_xruns(e);
    st->quiet = !st->playing && !st->recording;
    for (t = 0; t < TRK_TRACKS && st->quiet; t++) if (st->level[t] > 0.004f) st->quiet = 0;
}

void trk_record_start(trk_engine *e, int mode, int order, int row)
{
    int t;
    unsigned lead, beat0;
    /* The click needs the audio output; asked for before the lock, as the
     * sample sets ask. A machine without one still records, silently. */
    if (e->ropt.metronome) audio_open(e);
    pthread_mutex_lock(&e->lock);
    lead = (unsigned)e->ropt.count_in * 4u * TRK_PPQ;
    play_locked(e, mode, order, row, lead);
    e->rec_first_tick = e->clock_base;          /* play_locked left the first row's tick here */
    e->rec_lead = lead;
    e->take_bpm = e->song.bpm;
    e->ntake = 0;
    for (t = 0; t < TRK_TRACKS; t++) e->rec_note[t] = -1;
    trk_undo_push(e);                           /* one undo takes the whole take back out */
    e->recording = 1;
    /* The count-in's clicks: one a beat, ahead of the first row. */
    beat0 = e->rec_first_tick - lead;
    if (e->ropt.metronome) {
        for (t = 0; t < e->ropt.count_in * 4; t++)
            push_sample(e, beat0 + (unsigned)t * TRK_PPQ, 0, SEV_CLICK, -1, t % 4 == 0, 0, -1);
        /* The rows already laid down when recording went on -- the first and
         * the few the lookahead took with it -- were scheduled without their
         * clicks: the ones on a beat get theirs here. */
        {
            const int lpb = trk_lpb_ok(e->song.lpb) ? e->song.lpb : 4;
            unsigned k;
            for (k = 0; k < e->npos && k < POS_RING; k++) {
                const pos_mark *m = &e->pos[k];
                if (m->row % lpb == 0)
                    push_sample(e, m->tick, 0, SEV_CLICK, -1, m->row % (lpb * 4) == 0, 0, -1);
            }
        }
    }
    pthread_mutex_unlock(&e->lock);
}

/* ------------------------------------------------------------ the input */

/* One event off the input port. A MIDI event carries the queue's tick the
 * kernel stamped it with; one without (the queue was not running) is
 * stamped now. */
static void input_event(trk_engine *e, const snd_seq_event_t *ev)
{
    int on = 0, off = 0, cc = 0, note = 0, vel = 0, ccn = 0, ccv = 0, t, monitor;
    unsigned tick;

    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
        note = ev->data.note.note; vel = ev->data.note.velocity;
        /* The kernel passes another client's bytes through as they are: a
         * note is 0..127 only if this checks it. */
        if (note > 127 || vel > 127) return;
        if (vel > 0) on = 1; else off = 1;
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        note = ev->data.note.note; off = 1;
        if (note > 127) return;
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        ccn = ev->data.control.param; ccv = ev->data.control.value; cc = 1;
        if (ccn < 0 || ccn > 127 || ccv < 0 || ccv > 127) return;
        break;
    default:
        return;
    }
    pthread_mutex_lock(&e->lock);
    if ((ev->flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_TICK &&
        (ev->flags & SND_SEQ_TIME_MODE_MASK) == SND_SEQ_TIME_MODE_ABS)
        tick = ev->time.tick;
    else
        tick = queue_tick(e);
    t = e->rec_track;
    monitor = e->ropt.monitor;
    if (on) record_on(e, tick, t, note, vel);
    else if (off) record_off(e, tick, t, note);
    else if (cc) record_cc(e, tick, t, ccn, ccv);
    pthread_mutex_unlock(&e->lock);
    /* Heard as it is played, on the cursor's track. */
    if (monitor && on) trk_preview(e, t, note, vel);
    else if (monitor && off) trk_preview_off(e, t);
}

static void *input_main(void *ud)
{
    trk_engine *e = ud;
    int n = snd_seq_poll_descriptors_count(e->seq, POLLIN);
    struct pollfd *fds = calloc((size_t)n + 1, sizeof *fds);
    if (!fds) return NULL;
    snd_seq_poll_descriptors(e->seq, fds, (unsigned)n, POLLIN);
    fds[n].fd = e->ipipe[0];
    fds[n].events = POLLIN;
    for (;;) {
        int i, any = 0;
        if (poll(fds, (nfds_t)n + 1, -1) < 0) continue;
        if (fds[n].revents & POLLIN) break;
        for (i = 0; i < n; i++) {
            /* The sequencer going away reports an error and never a readable
             * fd again: looping back to poll would spin on it at full speed.
             * Recording from MIDI ends here; the keyboard goes on. */
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "tracker: the MIDI input is gone -- recording from MIDI has stopped\n");
                free(fds);
                return NULL;
            }
            if (fds[i].revents & POLLIN) any = 1;
        }
        if (!any) continue;
        while (snd_seq_event_input_pending(e->seq, 1) > 0) {
            snd_seq_event_t *ev = NULL;
            if (snd_seq_event_input(e->seq, &ev) < 0 || !ev) break;
            input_event(e, ev);
        }
    }
    free(fds);
    return NULL;
}

int trk_input_list(trk_engine *e, char names[][TRK_DEST_LEN], int max)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    const unsigned need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
    int n = 0;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (n < max && snd_seq_query_next_client(e->seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        if (c == e->client || c == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (n < max && snd_seq_query_next_port(e->seq, pi) >= 0) {
            if ((snd_seq_port_info_get_capability(pi) & need) != need) continue;
            if (snd_seq_port_info_get_capability(pi) & SND_SEQ_PORT_CAP_NO_EXPORT) continue;
            snprintf(names[n++], TRK_DEST_LEN, "%s: %s", snd_seq_client_info_get_name(ci),
                     snd_seq_port_info_get_name(pi));
        }
    }
    return n;
}

int trk_input_connect(trk_engine *e, const char *name)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    const unsigned need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
    int r = -1;

    pthread_mutex_lock(&e->lock);
    /* Whatever was connected lets go first. */
    {
        snd_seq_query_subscribe_t *q;
        snd_seq_addr_t me = { (unsigned char)e->client, (unsigned char)e->in_port };
        snd_seq_query_subscribe_alloca(&q);
        snd_seq_query_subscribe_set_root(q, &me);
        snd_seq_query_subscribe_set_type(q, SND_SEQ_QUERY_SUBS_WRITE);
        int guard;
        snd_seq_query_subscribe_set_index(q, 0);
        for (guard = 0; guard < 64 && snd_seq_query_port_subscribers(e->seq, q) >= 0; guard++) {
            const snd_seq_addr_t *from = snd_seq_query_subscribe_get_addr(q);
            snd_seq_disconnect_from(e->seq, e->in_port, from->client, from->port);
            snd_seq_query_subscribe_set_index(q, 0);
        }
    }
    e->in_name[0] = 0;
    if (!name || !*name) { pthread_mutex_unlock(&e->lock); return 0; }

    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (r < 0 && snd_seq_query_next_client(e->seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        if (c == e->client) continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(e->seq, pi) >= 0) {
            char full[TRK_DEST_LEN];
            if ((snd_seq_port_info_get_capability(pi) & need) != need) continue;
            snprintf(full, sizeof full, "%s: %s", snd_seq_client_info_get_name(ci), snd_seq_port_info_get_name(pi));
            if (strcmp(full, name)) continue;
            if (snd_seq_connect_from(e->seq, e->in_port, c, snd_seq_port_info_get_port(pi)) >= 0) {
                snprintf(e->in_name, sizeof e->in_name, "%s", name);
                r = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&e->lock);
    return r;
}

const char *trk_input_connected(trk_engine *e) { return e->in_name; }

/* ------------------------------------------------------------- the take */

int trk_take_events(trk_engine *e)
{
    int n;
    pthread_mutex_lock(&e->lock);
    n = (int)e->ntake;
    pthread_mutex_unlock(&e->lock);
    return n;
}

int trk_take_export_midi(trk_engine *e, const char *path, char *err, size_t errn)
{
    trk_mev *ev[TRK_TRACKS] = { 0 };
    int n[TRK_TRACKS] = { 0 }, t, rc = -1;
    const char *names[TRK_TRACKS];
    take_ev *copy = NULL;
    size_t nt, i;
    double bpm;
    unsigned end = 0;
    char nm[TRK_TRACKS][TRK_NAME_LEN];

    pthread_mutex_lock(&e->lock);
    nt = e->ntake;
    bpm = e->take_bpm > 0 ? e->take_bpm : 120.0;
    if (nt && (copy = malloc(nt * sizeof *copy))) memcpy(copy, e->take, nt * sizeof *copy);
    for (t = 0; t < TRK_TRACKS; t++) snprintf(nm[t], sizeof nm[t], "%s", e->song.track[t].name);
    pthread_mutex_unlock(&e->lock);
    if (!nt) { snprintf(err, errn, "nothing has been recorded"); return -1; }
    if (!copy) { snprintf(err, errn, "out of memory"); return -1; }

    for (t = 0; t < TRK_TRACKS; t++) {
        int open_at[128], k, cnt = 0;
        trk_mev *list = NULL;
        size_t cap = 0;
        for (k = 0; k < 128; k++) open_at[k] = -1;
        for (i = 0; i < nt; i++) {
            const take_ev *v = &copy[i];
            if (v->track != t || v->a > 127) continue;
            if (v->tick > end) end = v->tick;
            if (cnt + 1 > (int)cap) {
                trk_mev *g;
                cap = cap ? cap * 2 : 128;
                if (!(g = realloc(list, cap * sizeof *g))) { free(list); snprintf(err, errn, "out of memory"); goto done; }
                list = g;
            }
            if (v->kind == TAKE_ON) {
                if (open_at[v->a] >= 0) list[open_at[v->a]].end = v->tick;       /* retriggered */
                list[cnt] = (trk_mev){ 0, v->tick, v->tick, v->chan, v->a, v->b };
                open_at[v->a] = cnt++;
            } else if (v->kind == TAKE_OFF) {
                if (open_at[v->a] >= 0) { list[open_at[v->a]].end = v->tick; open_at[v->a] = -1; }
            } else {
                list[cnt++] = (trk_mev){ 1, v->tick, v->tick, v->chan, v->a, v->b };
            }
        }
        /* A key still down at the end of the take is held to its last tick. */
        for (k = 0; k < 128; k++) if (open_at[k] >= 0) list[open_at[k]].end = end > list[open_at[k]].start ? end : list[open_at[k]].start + 1;
        ev[t] = list; n[t] = cnt;
        names[t] = nm[t];
    }
    rc = trk_midi_write(path, bpm, end + TRK_PPQ / 4, TRK_TRACKS, names, (trk_mev *const *)ev, n, err, errn);
done:
    for (t = 0; t < TRK_TRACKS; t++) free(ev[t]);
    free(copy);
    return rc;
}

void trk_panic(trk_engine *e)
{
    int t;
    pthread_mutex_lock(&e->lock);
    /* Playing on: what is queued is taken back first, or it would sound
     * after the releases below and be held by nothing. */
    if (e->playing) rewind_locked(e);
    /* Pending sample hits go too, the due ones included -- rewind keeps
     * those, but panic means nothing scheduled survives, however close to
     * its time it was: "Panic cuts it dead." */
    drop_samples(e, 1, 0);
    drop_sinks(e, 1, 0);
    push_sample(e, 0, 1, SEV_SILENCE, -1, 0, 0, -1);     /* every sample, now */
    for (t = 0; t < TRK_TRACKS; t++) {
        release_track(e, t);
        /* And every channel the track is set to now, even one it never
         * played: a note somebody else started on it is still a stuck note. */
        track_ev(e, t, 0, 1, 0xB0 | (e->song.track[t].channel & 15), 123, 0);
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
    if (snd_seq_open(&e->seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
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
        if (e->port[t] < 0) {
            snprintf(err, errn, "could not create a sequencer port (%s)", snd_strerror(e->port[t]));
            snd_seq_close(e->seq);
            free(e);
            return NULL;
        }
    }
    e->clock_port = snd_seq_create_simple_port(e->seq, "Clock", SND_SEQ_PORT_CAP_READ,
                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (e->clock_port < 0) {
        snprintf(err, errn, "could not create the clock port (%s)", snd_strerror(e->clock_port));
        snd_seq_close(e->seq);
        free(e);
        return NULL;
    }

    e->queue = snd_seq_alloc_named_queue(e->seq, "tracker");
    if (e->queue < 0) {
        snprintf(err, errn, "could not allocate a sequencer queue");
        snd_seq_close(e->seq);
        free(e);
        return NULL;
    }
    /* The MIDI input: writable and subscribable by anything, and every event
     * that arrives is stamped with this queue's tick by the kernel -- the
     * time it came in by the clock the song plays on, whatever the input
     * thread's own scheduling does. */
    e->in_port = snd_seq_create_simple_port(e->seq, "Record In",
                     SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (e->in_port >= 0) {
        snd_seq_port_info_t *pi;
        snd_seq_port_info_alloca(&pi);
        if (snd_seq_get_port_info(e->seq, e->in_port, pi) >= 0) {
            snd_seq_port_info_set_timestamping(pi, 1);
            snd_seq_port_info_set_timestamp_real(pi, 0);
            snd_seq_port_info_set_timestamp_queue(pi, e->queue);
            snd_seq_set_port_info(e->seq, e->in_port, pi);
        }
    }
    trk_record_defaults(&e->ropt);
    e->rec_first_tick = 0;
    audio_conf_load(e);                           /* the output chosen last time */
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
    atomic_store(&e->vol_live, e->song.volume);
    pthread_mutex_init(&e->lock, NULL);
    {   /* The sample audio runs at realtime priority and shares this lock with
         * threads that do not: priority inheritance lets a low-priority holder
         * finish at the audio thread's priority instead of making it wait. */
        pthread_mutexattr_t ma;
        pthread_mutexattr_init(&ma);
        pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&e->smx, &ma);
        pthread_mutexattr_destroy(&ma);
    }
    e->buf_level = 1;
    pthread_mutex_init(&e->amx, NULL);
    pthread_mutex_init(&e->kmx, NULL);
    {   /* the audio thread takes this: priority-inheriting, as smx is */
        pthread_mutexattr_t ta;
        pthread_mutexattr_init(&ta);
        pthread_mutexattr_setprotocol(&ta, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&e->tapmx, &ta);
        pthread_mutexattr_destroy(&ta);
    }
    pthread_mutex_init(&e->rmx, NULL);
    pthread_mutex_init(&e->dmx, NULL);
    for (t = 0; t < TRK_TRACKS; t++) e->track_kit[t] = -1;
    for (t = 0; t < TRK_TRACKS; t++) e->track_sink[t] = -1;
    e->bpm_now = 120.0;
    if (pthread_create(&e->thread, NULL, sched_thread, e)) {
        snprintf(err, errn, "could not start the scheduling thread");
        snd_seq_close(e->seq);
        free(e);
        return NULL;
    }
    /* The input thread: without it the tracker records from the keyboard
     * only, which is not an error. */
    if (e->in_port >= 0 && pipe(e->ipipe) == 0) {
        if (pthread_create(&e->ithread, NULL, input_main, e) == 0) e->ion = 1;
        else { close(e->ipipe[0]); close(e->ipipe[1]); }
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
    if (e->ion) {
        char b = 1;
        if (write(e->ipipe[1], &b, 1) < 0) { /* the thread polls on; closing the seq below ends it */ }
        pthread_join(e->ithread, NULL);
        close(e->ipipe[0]); close(e->ipipe[1]);
    }
    free(e->take);
    if (e->delivery_on) {
        /* As the ALSA side drains at stop: a few passes for the delivery
         * thread to hand the sinks their stop and releases before it goes. */
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&e->dmx);
        e->dquit = 1;
        pthread_mutex_unlock(&e->dmx);
        pthread_join(e->dthread, NULL);
    }
    /* A synth the tracker's fader was holding down must not stay quiet once the
     * tracker is gone: every sink with a gain goes back to full. The shells
     * close the engine while their tabs still exist, so `ud` is good here. */
    pthread_mutex_lock(&e->lock);
    for (i = 0; i < TRK_SINKS; i++)
        if (e->sink[i].used && e->sink[i].gain_cb) {
            e->sink[i].gain_cb(e->sink[i].ud, 1.0);
            e->sink[i].gain_sent = 1.0;
        }
    pthread_mutex_unlock(&e->lock);
    atomic_store(&e->closing, 1);
    pthread_mutex_lock(&e->rmx);
    if (e->rstarted) { pthread_mutex_unlock(&e->rmx); pthread_join(e->rthread, NULL); pthread_mutex_lock(&e->rmx); e->rstarted = 0; }
    pthread_mutex_unlock(&e->rmx);
    audio_close(e);
    for (i = 0; i < e->nkit; i++) drumkit_free(e->kit[i].dk);
    drumkit_free(e->aud);
    while (e->nundo) free(e->undo[--e->nundo]);
    while (e->nredo) free(e->redo[--e->nredo]);
    pthread_mutex_destroy(&e->smx);
    pthread_mutex_destroy(&e->dmx);
    pthread_mutex_destroy(&e->amx);
    pthread_mutex_destroy(&e->kmx);
    pthread_mutex_destroy(&e->tapmx);
    pthread_mutex_destroy(&e->rmx);
    snd_seq_free_queue(e->seq, e->queue);
    snd_seq_close(e->seq);
    pthread_mutex_destroy(&e->lock);
    free(e);
}


/* ------------------------------------------------------- audio output -- */

static const char *audio_conf_path(char *buf, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(buf, n, "%s/vst-ace/audio-output", x);
    else if (h && *h) snprintf(buf, n, "%s/.config/vst-ace/audio-output", h);
    else return NULL;
    return buf;
}

static void audio_conf_load(trk_engine *e)
{
    char path[1024], line[TRK_DEST_LEN + 8];
    FILE *f;
    if (!audio_conf_path(path, sizeof path) || !(f = fopen(path, "r"))) return;
    if (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        snprintf(e->pcm_name, sizeof e->pcm_name, "%s", line);
    }
    if (fgets(line, sizeof line, f) && !strncmp(line, "buffer ", 7)) {
        const int lv = atoi(line + 7);
        if (lv >= 0 && lv < TRK_BUFFER_LEVELS) e->buf_level = lv;
    }
    fclose(f);
}

static void audio_conf_save(trk_engine *e)
{
    char path[1024], dir[1024], *slash, tmp[1100];
    FILE *f;
    if (!audio_conf_path(path, sizeof path)) return;
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/'))) {
        char *q;
        *slash = 0;
        for (q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0700); *q = '/'; }
        mkdir(dir, 0700);
    }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return;
    fprintf(f, "%s\n", e->pcm_name);
    fprintf(f, "buffer %d\n", e->buf_level);
    if (fclose(f) != 0 || rename(tmp, path) != 0) unlink(tmp);
}

/* The playback devices ALSA offers that are worth choosing between: the
 * system's default, the PipeWire and JACK plug-ins (present when
 * pipewire-alsa and the JACK plug-in are installed), PulseAudio, and the
 * hardware cards through plughw, which takes any rate and format. */
int trk_audio_devices(char names[][TRK_DEST_LEN], char labels[][96], int max)
{
    void **hints = NULL, **h;
    int n = 0, pass;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return 0;
    for (pass = 0; pass < 2; pass++)
        for (h = hints; *h && n < max; h++) {
            char *name = snd_device_name_get_hint(*h, "NAME"), *desc = snd_device_name_get_hint(*h, "DESC"),
                 *io = snd_device_name_get_hint(*h, "IOID");
            int special, hw, dup = 0, i;
            if (!name || (io && !strcmp(io, "Input"))) goto next;
            special = !strcmp(name, "default") || !strcmp(name, "pipewire") || !strcmp(name, "jack") ||
                      !strcmp(name, "pulse") || !strcmp(name, "sysdefault");
            hw = !strncmp(name, "plughw:", 7);
            if ((pass == 0) != special || (pass == 1 && !hw)) goto next;
            for (i = 0; i < n; i++) if (!strcmp(names[i], name)) dup = 1;
            if (dup) goto next;
            snprintf(names[n], TRK_DEST_LEN, "%s", name);
            if (!strcmp(name, "default"))        snprintf(labels[n], 96, "%s", TRK_DEFAULT_LABEL);
            else if (!strcmp(name, "sysdefault")) snprintf(labels[n], 96, "ALSA default (sysdefault)");
            else if (!strcmp(name, "pipewire"))  snprintf(labels[n], 96, "PipeWire");
            else if (!strcmp(name, "jack"))      snprintf(labels[n], 96, "JACK");
            else if (!strcmp(name, "pulse"))     snprintf(labels[n], 96, "PulseAudio");
            else {
                char *nl = desc ? strchr(desc, '\n') : NULL;
                if (nl) *nl = 0;
                snprintf(labels[n], 96, "ALSA: %s", desc ? desc : name);
            }
            n++;
        next:
            free(name); free(desc); free(io);
        }
    snd_device_name_free_hint(hints);
    {   /* The usual choices first, in the order a person looks for them. */
        static const char *const order[] = { "default", "pipewire", "jack", "pulse", "sysdefault" };
        int i, j, k = 0;
        for (j = 0; j < 5; j++)
            for (i = k; i < n; i++)
                if (!strcmp(names[i], order[j])) {
                    char tn[TRK_DEST_LEN], tl[96];
                    memcpy(tn, names[i], sizeof tn); memcpy(tl, labels[i], sizeof tl);
                    memmove(names[k + 1], names[k], (size_t)(i - k) * TRK_DEST_LEN);
                    memmove(labels[k + 1], labels[k], (size_t)(i - k) * 96);
                    memcpy(names[k], tn, sizeof tn); memcpy(labels[k], tl, sizeof tl);
                    k++;
                    break;
                }
    }
    return n;
}

const char *trk_audio_device(trk_engine *e) { return e->pcm_name; }

int trk_audio_buffer(trk_engine *e) { return e->buf_level; }

/* The buffer-size setting, changed live: the output is let go of and opened
 * again at the new size (the sample sets stay loaded). Kept between runs. */
int trk_audio_set_buffer(trk_engine *e, int level)
{
    int was_on;
    if (level < 0 || level >= TRK_BUFFER_LEVELS) return -1;
    if (level == e->buf_level) return 0;
    pthread_mutex_lock(&e->amx);
    was_on = e->audio_on;
    audio_close_l(e);
    e->buf_level = level;
    if (was_on) audio_open_l(e);
    pthread_mutex_unlock(&e->amx);
    audio_conf_save(e);
    return 0;
}

/* Switch the sample output. "" is the default (TRK_PCM, else the system's).
 * Switched live: the old device is let go of and the new one opened; if it will
 * not open, the default is put back and the reason is in trk_audio_status.
 * Returns 0, or -1 when the chosen device would not open. */
int trk_audio_set_device(trk_engine *e, const char *name)
{
    int was_on, r = 0;
    if (!name) name = "";
    pthread_mutex_lock(&e->amx);
    was_on = e->audio_on;
    audio_close_l(e);
    snprintf(e->pcm_name, sizeof e->pcm_name, "%s", name);
    if (was_on && audio_open_l(e) != 0) {
        char why[sizeof e->audio_msg];
        snprintf(why, sizeof why, "%s", e->audio_msg);
        e->pcm_name[0] = 0;
        audio_open_l(e);
        snprintf(e->audio_msg, sizeof e->audio_msg, "%s -- back on the default", why);
        r = -1;
    }
    pthread_mutex_unlock(&e->amx);
    audio_conf_save(e);
    return r;
}

trk_song *trk_song_of(trk_engine *e) { return &e->song; }
void trk_lock(trk_engine *e)   { pthread_mutex_lock(&e->lock); }
/* Everything that edits the song holds the lock, and the volume is the one
 * field the audio thread wants without it: so it is published as the lock is
 * let go, and read from here, rather than read raw off the song. */
void trk_unlock(trk_engine *e)
{
    atomic_store_explicit(&e->vol_live, e->song.volume, memory_order_relaxed);
    pthread_mutex_unlock(&e->lock);
}

/* Routing is runtime state, which a snapshot does not hold: the sink names
 * in a restored song are put back to what the tracks are playing now, as
 * trk_route_sink writes them. */
static void sync_sink_names(trk_engine *e)
{
    int t;
    pthread_mutex_lock(&e->dmx);
    for (t = 0; t < TRK_TRACKS; t++) {
        memset(e->song.track[t].sink, 0, sizeof e->song.track[t].sink);
        if (e->track_sink[t] >= 0) {
            char nm[TRK_DEST_LEN];
            snprintf(nm, sizeof nm, "%s", e->sink[e->track_sink[t]].name);
            strcpy(e->song.track[t].sink, nm);
        }
    }
    pthread_mutex_unlock(&e->dmx);
}

/* ---- undo: snapshots ----
 *
 * A snapshot is everything of the song that comes before its patterns, and
 * then only the patterns that hold something: a pattern that is blank and of
 * the default length is the same as no pattern, so it is not kept. Of the
 * hundred patterns a song has room for, a few are usually used -- a snapshot
 * is tens of kilobytes where the whole song is over a megabyte. */
struct snap {
    unsigned char hdr[offsetof(trk_song, pattern)];
    int           npat;
    struct { int idx; trk_pattern p; } pat[];
};

static int pattern_blank(const trk_pattern *p)
{
    static trk_pattern blank;
    static int made;
    if (!made) {
        memset(&blank, 0, sizeof blank);
        blank.rows = 64;
        memset(blank.cell, TRK_EMPTY, sizeof blank.cell);
        made = 1;
    }
    return memcmp(p, &blank, sizeof blank) == 0;
}

static struct snap *snap_make(const trk_song *s)
{
    struct snap *k;
    int p, n = 0;
    for (p = 0; p < TRK_PATTERNS; p++) if (!pattern_blank(&s->pattern[p])) n++;
    k = malloc(sizeof *k + (size_t)n * sizeof k->pat[0]);
    if (!k) return NULL;
    memcpy(k->hdr, s, sizeof k->hdr);
    k->npat = 0;
    for (p = 0; p < TRK_PATTERNS; p++)
        if (!pattern_blank(&s->pattern[p])) {
            k->pat[k->npat].idx = p;
            k->pat[k->npat].p = s->pattern[p];
            k->npat++;
        }
    return k;
}

/* A snapshot back into the song -- except what is live rather than edited:
 * the tempo (the queue is running at it), the rows per beat and the volume.
 * An undo of a cell edit that also took the tempo back to what it was then
 * would show one tempo while the queue played another. */
static void snap_restore(trk_engine *e, const struct snap *k)
{
    trk_song *s = &e->song;
    const double bpm = s->bpm;
    const int lpb = s->lpb, volume = s->volume;
    int p;
    for (p = 0; p < TRK_PATTERNS; p++)
        if (!pattern_blank(&s->pattern[p])) {
            memset(&s->pattern[p], 0, sizeof s->pattern[p]);
            s->pattern[p].rows = 64;
            memset(s->pattern[p].cell, TRK_EMPTY, sizeof s->pattern[p].cell);
        }
    memcpy(s, k->hdr, sizeof k->hdr);
    for (p = 0; p < k->npat; p++) s->pattern[k->pat[p].idx] = k->pat[p].p;
    s->bpm = bpm;
    s->lpb = lpb;
    s->volume = volume;
}

static void redo_clear(trk_engine *e)
{
    while (e->nredo) free(e->redo[--e->nredo]);
}

/* A snapshot of the song as it is onto one of the two stacks. A full stack
 * gives up its oldest. A malloc failure drops the entry, never the edit. */
static void stack_push(struct snap **stack, int *n, const trk_song *s)
{
    struct snap *k;
    if (*n == TRK_UNDO_MAX) {
        free(stack[0]);
        memmove(stack, stack + 1, sizeof stack[0] * (TRK_UNDO_MAX - 1));
        (*n)--;
    }
    if (!(k = snap_make(s))) return;
    stack[(*n)++] = k;
}

/* The editor's edit points call this just before they change the song,
 * under the lock. A new edit is a new branch: what could be redone is gone.
 * Not in trk.h -- only the core snapshots. */
void trk_undo_push(trk_engine *e)
{
    redo_clear(e);
    stack_push(e->undo, &e->nundo, &e->song);
}

/* The caller holds the lock -- trk_key's undo case does already. */
int trk_undo_locked(trk_engine *e)
{
    struct snap *k;
    if (!e->nundo) return 0;
    k = e->undo[--e->nundo];
    stack_push(e->redo, &e->nredo, &e->song);       /* the song as it is, to redo to */
    snap_restore(e, k);
    free(k);
    sync_sink_names(e);
    return 1;
}

int trk_redo_locked(trk_engine *e)
{
    struct snap *k;
    if (!e->nredo) return 0;
    k = e->redo[--e->nredo];
    stack_push(e->undo, &e->nundo, &e->song);       /* not trk_undo_push: that would clear the redo */
    snap_restore(e, k);
    free(k);
    sync_sink_names(e);
    return 1;
}

/* Whether any pattern has something on track t. */
int trk_track_used(trk_engine *e, int t)
{
    int p, r, used = 0;
    if (t < 0 || t >= TRK_TRACKS) return 0;
    pthread_mutex_lock(&e->lock);
    for (p = 0; p < TRK_PATTERNS && !used; p++)
        for (r = 0; r < e->song.pattern[p].rows; r++) {
            const trk_cell *c = &e->song.pattern[p].cell[r][t];
            if (c->note != TRK_EMPTY || c->vel != TRK_EMPTY || c->cc != TRK_EMPTY || c->val != TRK_EMPTY) { used = 1; break; }
        }
    pthread_mutex_unlock(&e->lock);
    return used;
}

/* Tracks inserted and removed: the song's tracks and every pattern's cells
 * move together and the routing follows. Playing goes on; what was sounding
 * is released first, so nothing is left held by a track that is no longer
 * where it was. */
static void move_tracks_locked(trk_engine *e, int at, int dir)
{
    trk_song *s = &e->song;
    int p, r, t;
    unsigned i, n;
    /* Playback carries on: what was scheduled ahead is taken back (it was laid
     * down against the tracks as they were), what is sounding is let go of, and
     * the schedule resumes from the first row not yet played. The history the
     * schedule rewinds to names the held notes by track number -- it is
     * cleared, since the numbers have moved. */
    if (e->playing) rewind_locked(e);
    for (t = 0; t < TRK_TRACKS; t++) release_track(e, t);
    n = e->npos < POS_RING ? e->npos : POS_RING;
    for (i = 0; i < n; i++)
        for (t = 0; t < TRK_TRACKS; t++) e->pos[(e->npos - 1 - i) % POS_RING].held[t] = -1;
    for (p = 0; p < TRK_PATTERNS; p++)
        for (r = 0; r < TRK_ROWS_MAX; r++) {
            trk_cell *row = s->pattern[p].cell[r];
            if (dir > 0) {
                memmove(&row[at + 1], &row[at], sizeof row[0] * (size_t)(TRK_TRACKS - 1 - at));
                memset(&row[at], TRK_EMPTY, sizeof row[0]);
            } else {
                memmove(&row[at], &row[at + 1], sizeof row[0] * (size_t)(TRK_TRACKS - 1 - at));
                memset(&row[TRK_TRACKS - 1], TRK_EMPTY, sizeof row[0]);
            }
        }
    if (dir > 0) {
        memmove(&s->track[at + 1], &s->track[at], sizeof s->track[0] * (size_t)(TRK_TRACKS - 1 - at));
        memmove(&e->track_sink[at + 1], &e->track_sink[at], sizeof e->track_sink[0] * (size_t)(TRK_TRACKS - 1 - at));
        memset(&s->track[at], 0, sizeof s->track[at]);
        {   /* "Track N" for the lowest N no track has -- not the position, which
             * would name an added track after one that is already there. */
            int num, t2, used;
            for (num = 1; num <= TRK_TRACKS + 1; num++) {
                char want[32];
                snprintf(want, sizeof want, "Track %d", num);
                used = 0;
                for (t2 = 0; t2 < TRK_TRACKS; t2++) if (t2 != at && !strcmp(s->track[t2].name, want)) used = 1;
                if (!used) break;
            }
            snprintf(s->track[at].name, sizeof s->track[at].name, "Track %d", num);
        }
        s->track[at].velocity = 100;
        s->track[at].volume = 100;
        s->track[at].octave = 4;
        e->track_sink[at] = -1;
        s->ntracks++;
    } else {
        memmove(&s->track[at], &s->track[at + 1], sizeof s->track[0] * (size_t)(TRK_TRACKS - 1 - at));
        memmove(&e->track_sink[at], &e->track_sink[at + 1], sizeof e->track_sink[0] * (size_t)(TRK_TRACKS - 1 - at));
        memset(&s->track[TRK_TRACKS - 1], 0, sizeof s->track[0]);
        snprintf(s->track[TRK_TRACKS - 1].name, sizeof s->track[0].name, "Track %d", TRK_TRACKS);
        s->track[TRK_TRACKS - 1].velocity = 100;
        s->track[TRK_TRACKS - 1].volume = 100;
        s->track[TRK_TRACKS - 1].octave = 4;
        e->track_sink[TRK_TRACKS - 1] = -1;
        s->ntracks--;
    }
    /* The room past the last track is as a new song leaves it, so a song
     * that has had tracks added and taken away is the song a file of it
     * would load back as. */
    for (t = s->ntracks; t < TRK_TRACKS; t++) {
        memset(&s->track[t], 0, sizeof s->track[t]);
        snprintf(s->track[t].name, sizeof s->track[t].name, "Track %d", t + 1);
        s->track[t].velocity = 100;
        s->track[t].volume = 100;
        s->track[t].octave = 4;
        e->track_sink[t] = -1;
    }
    sync_sink_names(e);
    sink_gains_locked(e);
    memset(e->vol_sent, 0, sizeof e->vol_sent);      /* tracks changed places */
    for (t = 0; t < TRK_TRACKS; t++) e->rec_note[t] = -1;
    if (e->playing) top_up(e);
}

int trk_track_insert(trk_engine *e, int at)
{
    int rc = -1;
    pthread_mutex_lock(&e->lock);
    if (at < 0) at = 0;
    if (at > e->song.ntracks) at = e->song.ntracks;
    if (e->song.ntracks < TRK_TRACKS) {
        trk_undo_push(e);
        move_tracks_locked(e, at, +1);
        rc = 0;
    }
    pthread_mutex_unlock(&e->lock);
    if (rc == 0) trk_route(e);                /* the windows and sample sets follow their tracks */
    return rc;
}

int trk_track_remove(trk_engine *e, int at)
{
    int rc = -1;
    pthread_mutex_lock(&e->lock);
    if (at >= 0 && at < e->song.ntracks && e->song.ntracks > 1) {
        trk_undo_push(e);
        move_tracks_locked(e, at, -1);
        rc = 0;
    }
    pthread_mutex_unlock(&e->lock);
    if (rc == 0) trk_route(e);
    return rc;
}

int trk_undo(trk_engine *e)
{
    int r;
    trk_lock(e);
    r = trk_undo_locked(e);
    trk_unlock(e);
    return r;
}

int trk_redo(trk_engine *e)
{
    int r;
    trk_lock(e);
    r = trk_redo_locked(e);
    trk_unlock(e);
    return r;
}

/* Forget the history -- a song just started or loaded has none. */
void trk_undo_clear(trk_engine *e)
{
    trk_lock(e);
    while (e->nundo) free(e->undo[--e->nundo]);
    redo_clear(e);
    trk_unlock(e);
}
const char *trk_client_name(trk_engine *e) { return e->name; }
const char *trk_audio_status(trk_engine *e) { return e->audio_msg; }
unsigned trk_audio_xruns(trk_engine *e) { return atomic_load_explicit(&e->xruns, memory_order_relaxed); }

/* Instrumentation for trktest, which has no other window onto the sample
 * queue: how much of it is pending, and the audio thread's running totals.
 * Not in trk.h -- the windows have no use for it. */
void trk_sample_stats(trk_engine *e, int *pending, int *played, int *controlled)
{
    pthread_mutex_lock(&e->smx);
    if (pending)    *pending = e->nsev;
    if (played)     *played = (int)e->sev_played;
    if (controlled) *controlled = (int)e->sev_controlled;
    pthread_mutex_unlock(&e->smx);
}
