/* The engine against two ALSA receivers, no window.
 *
 * Two clients stand in for two vst-ace windows. Tracks 1 and 2 play the
 * first (on channels 1 and 2), track 3 plays the second and holds a note it
 * never releases. Checked:
 *   - notes arrive on the beat, measured on arrival;
 *   - the clock arrives once per destination, not once per track;
 *   - start and stop arrive;
 *   - after stop, every note that was started has been released -- the
 *     held one included;
 *   - preview and panic leave nothing sounding;
 *   - panic, or moving a track to another window, while playing leaves
 *     nothing sounding -- the notes already queued included;
 *   - a track can play a sample set: sets are offered, one that is there
 *     loads and one that is not is reported, a sample track sends nothing
 *     over MIDI (the audio goes to ALSA's null device here), and a set
 *     edited and saved as kit.txt is what the track then plays;
 *   - a tempo change while playing keeps the sample and MIDI tracks
 *     together, on the new grid;
 *   - panic with the pending sample list full still silences, and leaves
 *     no scheduled hit pending;
 *   - muting a sample track fades what it is sounding and drops its queued
 *     hits, and unmuting resumes it;
 *   - a track routed to a sink saves the sink's name: the pick is mirrored
 *     into the song, survives a save and load, a hand-written sink line
 *     parses, and routing back or removing the sink clears it;
 *   - a song saved and loaded is the same song.
 * Exit status is the number of failed checks. */
#include "trk.h"
#include "wav.h"
#include "drumkit.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* engine.c's instrumentation for this test -- not in trk.h, the windows
 * have no use for it. */
void trk_sample_stats(trk_engine *e, int *pending, int *played, int *controlled);

#define MAXEV 4096

typedef struct { double t; int rx, type, ch, note, vel, param, value; } rec;

static rec      g_ev[MAXEV];
static int      g_nev;
static volatile int g_stop;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static int      g_fail;

/* What the in-process sink was handed: one block's events, each with the
 * block's wall-clock start and its sample offset within it. */
#define MAXSEV 8192
typedef struct { double wall; unsigned frame; int st, d1, d2; } sink_rec;
static sink_rec g_sev[MAXSEV];
static int      g_snev;
static pthread_mutex_t g_smx = PTHREAD_MUTEX_INITIALIZER;

static void sink_cb(void *ud, double wall, const trk_sink_ev *evs, int n)
{
    int i;
    (void)ud;
    pthread_mutex_lock(&g_smx);
    for (i = 0; i < n && g_snev < MAXSEV; i++) {
        g_sev[g_snev].wall = wall;
        g_sev[g_snev].frame = evs[i].frame;
        g_sev[g_snev].st = evs[i].status;
        g_sev[g_snev].d1 = evs[i].d1;
        g_sev[g_snev].d2 = evs[i].d2;
        g_snev++;
    }
    pthread_mutex_unlock(&g_smx);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct { snd_seq_t *seq; int idx; } rx_t;

static void *reader(void *ud)
{
    rx_t *r = ud;
    while (!g_stop) {
        snd_seq_event_t *ev;
        int got = 0;
        while (snd_seq_event_input(r->seq, &ev) >= 0) {
            pthread_mutex_lock(&g_mx);
            if (g_nev < MAXEV) {
                rec *x = &g_ev[g_nev++];
                memset(x, 0, sizeof *x);
                x->t = now_s();
                x->rx = r->idx;
                x->type = ev->type;
                if (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF) {
                    x->ch = ev->data.note.channel;
                    x->note = ev->data.note.note;
                    x->vel = ev->data.note.velocity;
                } else if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
                    x->ch = ev->data.control.channel;
                    x->param = (int)ev->data.control.param;
                    x->value = ev->data.control.value;
                }
            }
            pthread_mutex_unlock(&g_mx);
            got = 1;
        }
        if (!got) { struct timespec ts = { 0, 200000 }; nanosleep(&ts, NULL); }
    }
    return NULL;
}

static snd_seq_t *make_rx(const char *name)
{
    snd_seq_t *s;
    if (snd_seq_open(&s, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) return NULL;
    snd_seq_set_client_name(s, name);
    snd_seq_create_simple_port(s, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                               SND_SEQ_PORT_TYPE_MIDI_GENERIC);
    return s;
}

static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static int is_on(const rec *x)  { return x->type == SND_SEQ_EVENT_NOTEON && x->vel > 0; }
static int is_off(const rec *x) { return x->type == SND_SEQ_EVENT_NOTEOFF ||
                                         (x->type == SND_SEQ_EVENT_NOTEON && x->vel == 0); }

/* Every note started at a receiver has a release after its last start. */
static int all_released(int from, int to)
{
    int rx, ch, n, i, bad = 0;
    for (rx = 0; rx < 2; rx++)
        for (ch = 0; ch < 16; ch++)
            for (n = 0; n < 128; n++) {
                int sounding = 0;
                for (i = from; i < to; i++) {
                    const rec *x = &g_ev[i];
                    if (x->rx != rx || x->ch != ch || x->note != n) continue;
                    if (is_on(x)) sounding = 1;
                    else if (is_off(x)) sounding = 0;
                }
                if (sounding) {
                    printf("        still sounding: receiver %d channel %d note %d\n",
                           rx, ch + 1, n);
                    bad++;
                }
            }
    return bad == 0;
}

/* ------------------------------------------------------------ recording */

/* Wait until the song is on row r, then `extra_ms` further into it. */
static int wait_row(trk_engine *e, int r, int extra_ms)
{
    int i, o, p, row;
    for (i = 0; i < 4000; i++) {
        trk_position(e, &o, &p, &row);
        if (row == r) {
            struct timespec ts = { 0, extra_ms * 1000000L };
            nanosleep(&ts, NULL);
            return 1;
        }
        { struct timespec ts = { 0, 1000000L }; nanosleep(&ts, NULL); }
    }
    return 0;
}

static int find_client(snd_seq_t *s, const char *name)
{
    snd_seq_client_info_t *ci;
    snd_seq_client_info_alloca(&ci);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(s, ci) >= 0)
        if (!strcmp(snd_seq_client_info_get_name(ci), name)) return snd_seq_client_info_get_client(ci);
    return -1;
}

static int find_port(snd_seq_t *s, int client, const char *name)
{
    snd_seq_port_info_t *pi;
    snd_seq_port_info_alloca(&pi);
    snd_seq_port_info_set_client(pi, client);
    snd_seq_port_info_set_port(pi, -1);
    while (snd_seq_query_next_port(s, pi) >= 0)
        if (!strcmp(snd_seq_port_info_get_name(pi), name)) return snd_seq_port_info_get_port(pi);
    return -1;
}

int main(void)
{
    char err[256], path[] = "/tmp/trktest-XXXXXX";
    trk_engine *e;
    trk_song *s, *back;
    snd_seq_t *ra, *rb;
    rx_t a, b;
    pthread_t ta, tb;
    int i, n, mark, missing;
    double t0;

    /* A kit of two clicks, found through VA_KITS, played into nothing. */
    char kitroot[] = "/tmp/trkkit-XXXXXX", kitdir[256], wavp[300];
    if (!mkdtemp(kitroot)) { perror("mkdtemp"); return 1; }
    snprintf(kitdir, sizeof kitdir, "%s/testkit", kitroot);
    mkdir(kitdir, 0755);
    {
        static double click[2 * 2400];
        for (i = 0; i < 2400; i++) click[2 * i] = click[2 * i + 1] = 0.5 * exp(-i / 300.0);
        snprintf(wavp, sizeof wavp, "%s/a.wav", kitdir); wav_write_stereo16(wavp, click, 2400, 48000);
        snprintf(wavp, sizeof wavp, "%s/b.wav", kitdir); wav_write_stereo16(wavp, click, 2400, 48000);
    }
    setenv("VA_KITS", kitroot, 1);
    setenv("TRK_PCM", "null", 1);

    ra = make_rx("trkrx A");
    rb = make_rx("trkrx B");
    if (!ra || !rb) { fprintf(stderr, "no ALSA sequencer\n"); return 1; }
    a.seq = ra; a.idx = 0; b.seq = rb; b.idx = 1;
    pthread_create(&ta, NULL, reader, &a);
    pthread_create(&tb, NULL, reader, &b);

    if (!(e = trk_open(err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    printf("engine: %s\n", trk_client_name(e));

    trk_lock(e);
    s = trk_song_of(e);
    s->bpm = 120;
    s->lpb = 4;                                  /* a row is 125 ms */
    s->pattern[0].rows = 16;
    for (i = 0; i < 3; i++) {
        snprintf(s->track[i].client, sizeof s->track[i].client, "%s", i < 2 ? "trkrx A" : "trkrx B");
        snprintf(s->track[i].port, sizeof s->track[i].port, "in");
    }
    s->track[1].channel = 1;
    for (i = 0; i < 16; i += 4) s->pattern[0].cell[i][0].note = 60;   /* every beat */
    s->pattern[0].cell[0][0].cc = 74;
    s->pattern[0].cell[0][0].val = 0x40;
    for (i = 2; i < 16; i += 4) {
        s->pattern[0].cell[i][1].note = 67;
        s->pattern[0].cell[i][1].vel = 0x50;
        s->pattern[0].cell[i + 1][1].note = TRK_NOTE_OFF;
    }
    s->pattern[0].cell[0][2].note = 62;          /* held, never released */
    trk_unlock(e);

    printf("routing\n");
    {   /* What dwstudio's and pestudio's scans do: subscribe to every
         * readable, subscribable port. The tracker's must refuse them, or
         * every window would play every track. */
        snd_seq_client_info_t *ci;
        snd_seq_port_info_t *pi;
        int refused = 1, seen = 0;
        snd_seq_client_info_alloca(&ci);
        snd_seq_port_info_alloca(&pi);
        snd_seq_client_info_set_client(ci, -1);
        while (snd_seq_query_next_client(ra, ci) >= 0) {
            int cl = snd_seq_client_info_get_client(ci);
            if (strcmp(snd_seq_client_info_get_name(ci), trk_client_name(e))) continue;
            snd_seq_port_info_set_client(pi, cl);
            snd_seq_port_info_set_port(pi, -1);
            while (snd_seq_query_next_port(ra, pi) >= 0) {
                unsigned cap = snd_seq_port_info_get_capability(pi);
                seen++;
                if (cap & SND_SEQ_PORT_CAP_SUBS_READ) refused = 0;
                if (snd_seq_connect_from(ra, 0, cl, snd_seq_port_info_get_port(pi)) == 0) refused = 0;
            }
        }
        /* Eight tracks, the clock, and Record In -- which is the one port meant to be
         * written to, and is not readable at all. */
        check(seen == TRK_TRACKS + 2 && refused,
              "another program cannot subscribe to the tracker's ports");
    }
    missing = trk_route(e);
    check(missing == 0, "every track's destination found");
    check(trk_routed(e, 0) && trk_routed(e, 1) && trk_routed(e, 2), "tracks 1-3 connected");
    {
        char list[4096];
        int nd = trk_list_dests(e, list, sizeof list);
        check(nd >= 2 && strstr(list, "trkrx A\tin") && strstr(list, "trkrx B\tin"),
              "both receivers offered as destinations");
    }

    printf("playing pattern 0 for 2.2 s at 120 bpm\n");
    t0 = now_s();
    trk_play(e, TRK_PLAY_PATTERN, 0, 0);
    {
        int saw_row = 0;
        while (now_s() - t0 < 2.2) {
            int o, p, r;
            usleep(20000);
            trk_position(e, &o, &p, &r);
            if (p == 0 && r >= 4) saw_row = 1;
        }
        check(saw_row, "position follows playback");
    }
    pthread_mutex_lock(&g_mx); mark = g_nev; pthread_mutex_unlock(&g_mx);
    trk_stop(e);
    usleep(200000);

    pthread_mutex_lock(&g_mx);
    n = g_nev;
    {
        double on[64]; int non = 0, clk[2] = {0, 0}, start[2] = {0, 0}, stop[2] = {0, 0};
        int cc74 = 0, cc123 = 0, t2on = 0, t2off = 0;
        double first = 0, worst = 0;
        for (i = 0; i < mark; i++) {
            const rec *x = &g_ev[i];
            if (x->rx == 0 && x->ch == 0 && x->note == 60 && is_on(x) && non < 64) on[non++] = x->t;
            if (x->type == SND_SEQ_EVENT_CLOCK) clk[x->rx]++;
            if (x->type == SND_SEQ_EVENT_START) start[x->rx]++;
            if (x->type == SND_SEQ_EVENT_CONTROLLER && x->param == 74 && x->value == 0x40) cc74++;
            if (x->rx == 0 && x->ch == 1 && x->note == 67 && is_on(x)) { t2on++; if (x->vel != 0x50) t2on = -999; }
            if (x->rx == 0 && x->ch == 1 && x->note == 67 && is_off(x)) t2off++;
        }
        for (i = mark; i < n; i++) {
            const rec *x = &g_ev[i];
            if (x->type == SND_SEQ_EVENT_STOP) stop[x->rx]++;
            if (x->type == SND_SEQ_EVENT_CONTROLLER && x->param == 123) cc123++;
        }
        if (non) first = on[0] - t0;
        for (i = 1; i < non; i++) {
            double d = fabs((on[i] - on[i - 1]) - 0.5);
            if (d > worst) worst = d;
        }
        printf("  beats on track 1: %d, first after %.1f ms, worst spacing error %.2f ms\n",
               non, first * 1000, worst * 1000);
        check(non >= 4 && non <= 6, "track 1 played every beat");
        check(worst < 0.003, "beat spacing within 3 ms of 500 ms");
        check(first < 0.05, "first note within 50 ms of play");
        check(cc74 >= 1, "controller column sent");
        check(t2on >= 3 && t2off >= 3, "track 2: own channel, own velocity, note-offs");
        printf("  clocks in 2.2 s: A %d, B %d (expect about %d each)\n", clk[0], clk[1], (int)(2.2 * 48));
        check(clk[0] > 90 && clk[0] < 120, "clock once to A, though two tracks play it");
        check(clk[1] > 90 && clk[1] < 120, "clock to B");
        check(start[0] == 1 && start[1] == 1, "start to each destination once");
        check(stop[0] == 1 && stop[1] == 1, "stop to each destination once");
        check(cc123 >= 3, "all-notes-off on each channel used");
        check(all_released(0, n), "nothing left sounding after stop (the held note included)");
    }
    pthread_mutex_unlock(&g_mx);

    printf("preview and panic\n");
    pthread_mutex_lock(&g_mx); mark = g_nev; pthread_mutex_unlock(&g_mx);
    trk_preview(e, 2, 64, 90);
    trk_preview(e, 2, 65, 90);                   /* replaces 64 */
    usleep(50000);
    trk_panic(e);
    usleep(100000);
    pthread_mutex_lock(&g_mx);
    n = g_nev;
    check(all_released(mark, n), "previews released by the next preview and by panic");
    pthread_mutex_unlock(&g_mx);

    /* Pattern 1: track 1 climbs a note a beat, so a note that outlives its
     * release is a different note from the one that should replace it. Row 4
     * plays 510 ms in; at 450 ms it is on the queue and not yet heard -- the
     * moment a panic or a re-route has to take it back rather than chase it. */
    trk_lock(e);
    s->pattern[1].rows = 16;
    for (i = 0; i < 16; i += 4) s->pattern[1].cell[i][0].note = (uint8_t)(70 + i / 4);
    trk_unlock(e);

    printf("panic while playing\n");
    pthread_mutex_lock(&g_mx); mark = g_nev; pthread_mutex_unlock(&g_mx);
    trk_play(e, TRK_PLAY_PATTERN, 1, 0);
    usleep(450000);
    trk_panic(e);
    usleep(750000);                              /* past row 8 */
    trk_stop(e);
    usleep(150000);
    pthread_mutex_lock(&g_mx);
    n = g_nev;
    check(all_released(mark, n), "a note queued at the panic is still released by the next");
    pthread_mutex_unlock(&g_mx);

    printf("re-routing a track while playing\n");
    pthread_mutex_lock(&g_mx); mark = g_nev; pthread_mutex_unlock(&g_mx);
    trk_play(e, TRK_PLAY_PATTERN, 1, 0);
    usleep(450000);
    trk_lock(e);
    snprintf(s->track[0].client, sizeof s->track[0].client, "trkrx B");
    trk_unlock(e);
    trk_route(e);
    {
        int moved_at;
        pthread_mutex_lock(&g_mx); moved_at = g_nev; pthread_mutex_unlock(&g_mx);
        usleep(750000);
        trk_stop(e);
        usleep(150000);
        pthread_mutex_lock(&g_mx);
        n = g_nev;
        {
            int late_a = 0, at_b = 0;
            for (i = moved_at; i < n; i++) {
                const rec *x = &g_ev[i];
                if (x->rx == 0 && x->ch == 0 && is_on(x)) late_a++;
                if (x->rx == 1 && x->ch == 0 && is_on(x)) at_b++;
            }
            check(late_a == 0, "nothing reaches the old window after the re-route");
            check(at_b >= 2, "the track plays on at the new window");
        }
        check(all_released(mark, n), "nothing left sounding at either window");
        pthread_mutex_unlock(&g_mx);
    }
    trk_lock(e);
    snprintf(s->track[0].client, sizeof s->track[0].client, "trkrx A");
    trk_unlock(e);
    trk_route(e);

    printf("in-process sinks\n");
    {
        int id, mark_s, mark_r, non;
        char nm[TRK_DEST_LEN];

        id = trk_add_sink(e, "test sink", sink_cb, NULL);
        check(id >= 0, "a sink registers");
        check(id >= 0 && trk_sink_name(e, id, nm, sizeof nm) == 0 && !strcmp(nm, "test sink"),
              "the sink keeps its name");
        check(trk_sink_of(e, 0) == -1, "tracks start on their windows");
        trk_route_sink(e, 0, id);
        trk_route_sink(e, 1, id);
        check(trk_sink_of(e, 0) == id && trk_routed(e, 0), "track 1 routed to the sink");
        check(trk_route(e) == 0, "a sink-routed track is not a missing window");
        trk_lock(e);
        check(!strcmp(s->track[0].sink, "test sink") && !strcmp(s->track[1].sink, "test sink"),
              "the pick is mirrored into the song, to be saved");
        trk_unlock(e);

        pthread_mutex_lock(&g_mx); mark_r = g_nev; pthread_mutex_unlock(&g_mx);
        pthread_mutex_lock(&g_smx); mark_s = g_snev; pthread_mutex_unlock(&g_smx);
        t0 = now_s();
        trk_play(e, TRK_PLAY_PATTERN, 0, 0);
        usleep(1600000);
        trk_stop(e);
        usleep(300000);
        pthread_mutex_lock(&g_smx);
        {
            int cc74 = 0, clk = 0, starts = 0, stops = 0, spps = 0, disordered = 0;
            int sounding[16][128];
            double on[16], prev = 0, first = -1, worst = 0;
            non = 0;
            memset(sounding, 0, sizeof sounding);
            for (i = mark_s; i < g_snev; i++) {
                const sink_rec *x = &g_sev[i];
                const double tev = x->wall + x->frame / 48000.0;
                const int kind = x->st & 0xF0, ch = x->st & 15;
                if (i > mark_s && tev < prev - 0.0001) disordered++;
                prev = tev;
                if (x->st == 0xF8) clk++;
                if (x->st == 0xFA) starts++;
                if (x->st == 0xFC) stops++;
                if (x->st == 0xF2) spps++;
                if (kind == 0xB0 && x->d1 == 74 && x->d2 == 0x40) cc74++;
                if (kind == 0xB0 && (x->d1 == 123 || x->d1 == 120)) memset(sounding[ch], 0, sizeof sounding[ch]);
                if (kind == 0x90 && x->d2 > 0) {
                    sounding[ch][x->d1] = 1;
                    if (ch == 0 && x->d1 == 60 && non < 16) {
                        if (first < 0) first = tev - t0;
                        else {
                            double d = fabs(tev - on[non - 1] - 0.5);
                            if (d > worst) worst = d;
                        }
                        on[non++] = tev;
                    }
                }
                if (kind == 0x80 || (kind == 0x90 && x->d2 == 0)) sounding[ch][x->d1] = 0;
            }
            {
                int held = 0;
                for (i = 0; i < 16 * 128; i++) held += sounding[i / 128][i % 128];
                printf("  sink: %d note-ons, first after %.1f ms, worst spacing error %.2f ms, "
                       "%d clocks, %d held at stop\n", non, first * 1000, worst * 1000, clk, held);
                check(non >= 3 && non <= 5, "track 1's beats reach the sink");
                check(first >= 0 && first < 0.1, "the first note within 100 ms");
                check(worst < 0.003, "beat spacing at the sink within 3 ms");
                check(cc74 >= 1, "the controller column reaches the sink");
                check(clk > 60 && clk < 90, "clock once to the sink, though two tracks play it");
                check(starts == 1 && spps == 1 && stops == 1, "start, song position and stop each once");
                check(!disordered, "events arrive in time order");
                check(held == 0, "stop releases everything at the sink");
            }
        }
        pthread_mutex_unlock(&g_smx);
        pthread_mutex_lock(&g_mx);
        {
            int a_notes = 0, b_notes = 0;
            for (i = mark_r; i < g_nev; i++) {
                const rec *x = &g_ev[i];
                if (x->rx == 0 && (x->type == SND_SEQ_EVENT_NOTEON || x->type == SND_SEQ_EVENT_NOTEOFF ||
                                   x->type == SND_SEQ_EVENT_CONTROLLER)) a_notes++;
                if (x->rx == 1 && is_on(x)) b_notes++;
            }
            check(a_notes == 0, "nothing of the sink-routed tracks reaches their old windows");
            check(b_notes >= 1, "a track left on its window still plays it");
        }
        pthread_mutex_unlock(&g_mx);

        pthread_mutex_lock(&g_mx); mark_r = g_nev; pthread_mutex_unlock(&g_mx);
        pthread_mutex_lock(&g_smx); mark_s = g_snev; pthread_mutex_unlock(&g_smx);
        trk_preview(e, 0, 64, 90);
        usleep(60000);
        trk_preview_off(e, 0);
        usleep(150000);
        pthread_mutex_lock(&g_smx);
        non = 0;
        {
            int off = 0;
            for (i = mark_s; i < g_snev; i++) {
                const sink_rec *x = &g_sev[i];
                if (x->d1 != 64) continue;
                if ((x->st & 0xF0) == 0x90 && x->d2 > 0) non++;
                if ((x->st & 0xF0) == 0x80 || ((x->st & 0xF0) == 0x90 && x->d2 == 0)) off++;
            }
            check(non == 1 && off == 1, "a preview plays at the sink");
        }
        pthread_mutex_unlock(&g_smx);
        pthread_mutex_lock(&g_mx);
        {
            int at_a = 0;
            for (i = mark_r; i < g_nev; i++)
                if (g_ev[i].rx == 0 && g_ev[i].note == 64) at_a++;
            check(at_a == 0, "and not at the window it left");
        }
        pthread_mutex_unlock(&g_mx);

        trk_route_sink(e, 0, -1);
        trk_route_sink(e, 1, -1);
        check(trk_sink_of(e, 0) == -1, "routed back to the window");
        trk_lock(e);
        check(!s->track[0].sink[0] && !s->track[1].sink[0], "routing back clears the saved name");
        trk_unlock(e);
        pthread_mutex_lock(&g_mx); mark_r = g_nev; pthread_mutex_unlock(&g_mx);
        pthread_mutex_lock(&g_smx); mark_s = g_snev; pthread_mutex_unlock(&g_smx);
        trk_play(e, TRK_PLAY_PATTERN, 0, 0);
        usleep(700000);
        trk_stop(e);
        usleep(250000);
        pthread_mutex_lock(&g_mx);
        non = 0;
        for (i = mark_r; i < g_nev; i++)
            if (g_ev[i].rx == 0 && g_ev[i].ch == 0 && g_ev[i].note == 60 && is_on(&g_ev[i])) non++;
        check(non >= 1, "the window plays the track again");
        pthread_mutex_unlock(&g_mx);
        pthread_mutex_lock(&g_smx);
        non = 0;
        for (i = mark_s; i < g_snev; i++)
            if ((g_sev[i].st & 0xF0) == 0x90 && g_sev[i].d2 > 0) non++;
        check(non == 0, "and the sink hears nothing more");
        pthread_mutex_unlock(&g_smx);

        trk_route_sink(e, 0, id);
        trk_play(e, TRK_PLAY_PATTERN, 0, 0);
        usleep(400000);
        pthread_mutex_lock(&g_mx); mark_r = g_nev; pthread_mutex_unlock(&g_mx);
        pthread_mutex_lock(&g_smx); mark_s = g_snev; pthread_mutex_unlock(&g_smx);
        trk_remove_sink(e, id);
        check(trk_sink_of(e, 0) == -1, "removing the sink returns the track to its window");
        check(trk_sink_name(e, id, nm, sizeof nm) == -1, "the sink is gone");
        trk_lock(e);
        check(!s->track[0].sink[0], "removing the sink clears the saved name too");
        trk_unlock(e);
        usleep(700000);
        trk_stop(e);
        usleep(250000);
        pthread_mutex_lock(&g_smx);
        non = 0;
        for (i = mark_s; i < g_snev; i++) {
            const sink_rec *x = &g_sev[i];
            if ((x->st & 0xF0) == 0x80 ||
                ((x->st & 0xF0) == 0xB0 && (x->d1 == 123 || x->d1 == 120))) non++;
        }
        check(non > 0, "what was sounding is released to the sink at removal");
        pthread_mutex_unlock(&g_smx);
        pthread_mutex_lock(&g_mx);
        non = 0;
        for (i = mark_r; i < g_nev; i++)
            if (g_ev[i].rx == 0 && g_ev[i].ch == 0 && g_ev[i].note == 60 && is_on(&g_ev[i])) non++;
        check(non >= 1, "the window takes over mid-song");
        pthread_mutex_unlock(&g_mx);
        check(trk_route(e) == 0, "routing is whole again afterwards");
    }

    printf("sink names in the song file\n");
    {
        char  spath[] = "/tmp/trktest-sink-XXXXXX";
        trk_song *rt = malloc(sizeof *rt);
        int   sid, fd = mkstemp(spath);
        if (fd >= 0) close(fd);
        sid = trk_add_sink(e, "lead synth", sink_cb, NULL);
        trk_route_sink(e, 2, sid);
        trk_lock(e);
        n = trk_song_save(s, spath, err, sizeof err);
        trk_unlock(e);
        check(n == 0, "saved with a track on a sink");
        if (n) printf("        %s\n", err);
        n = trk_song_load(rt, spath, err, sizeof err);
        check(n == 0, "loaded back");
        if (n) printf("        %s\n", err);
        check(n == 0 && !strcmp(rt->track[2].sink, "lead synth"),
              "the sink name survives the round trip");
        check(n == 0 && rt->track[1].sink[0] == 0, "a track on its window saves no sink");
        trk_route_sink(e, 2, -1);
        trk_remove_sink(e, sid);
        free(rt);
        unlink(spath);
    }
    {
        char hpath[] = "/tmp/trktest-hand-XXXXXX";
        int  fd = mkstemp(hpath);
        FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
        trk_song *hw = malloc(sizeof *hw);
        check(f != NULL, "a hand-written song file to parse");
        if (f) {
            fputs("tracker 1\nbpm 120\nlpb 4\n"
                  "track 2 sink this window: FB-7999 bass\n"
                  "order 0\n", f);
            fclose(f);
            n = trk_song_load(hw, hpath, err, sizeof err);
            check(n == 0 && !strcmp(hw->track[1].sink, "this window: FB-7999 bass"),
                  "a sink line with spaces in the name parses");
            if (n) printf("        %s\n", err);
        }
        free(hw);
        unlink(hpath);
    }

    printf("restart while playing, then close while playing\n");
    pthread_mutex_lock(&g_mx); mark = g_nev; pthread_mutex_unlock(&g_mx);
    trk_play(e, TRK_PLAY_PATTERN, 0, 0);
    usleep(300000);
    trk_play(e, TRK_PLAY_PATTERN, 0, 8);         /* from the middle */
    usleep(300000);

    printf("sample sets\n");
    {
        char list[16384];
        int before, mid_count = 0;
        dk_map *m = calloc(1, sizeof *m);
        trk_list_sample_sets(e, list, sizeof list);
        check(strstr(list, "testkit\t") != NULL, "the sample set is offered");
        trk_lock(e);
        snprintf(s->track[3].samples, sizeof s->track[3].samples, "testkit");
        snprintf(s->track[4].samples, sizeof s->track[4].samples, "no such set");
        for (i = 0; i < 16; i += 2) s->pattern[1].cell[i][3].note = (uint8_t)(60 + (i / 2) % 2);
        trk_unlock(e);
        missing = trk_route(e);
        check(trk_routed(e, 3), "a track's set loads");
        check(!trk_routed(e, 4) && missing == 1, "one that is not there is reported missing");
        check(!strncmp(trk_audio_status(e), "samples: null", 13), "samples play out of TRK_PCM");
        printf("        %s\n", trk_audio_status(e));

        pthread_mutex_lock(&g_mx); before = g_nev; pthread_mutex_unlock(&g_mx);
        trk_play(e, TRK_PLAY_PATTERN, 1, 0);
        usleep(600000);
        trk_panic(e);
        trk_stop(e);
        usleep(100000);
        pthread_mutex_lock(&g_mx);
        for (i = before; i < g_nev; i++)
            if ((g_ev[i].note == 60 || g_ev[i].note == 61) && is_on(&g_ev[i])) mid_count++;
        pthread_mutex_unlock(&g_mx);
        check(mid_count == 0, "a sample track sends nothing over MIDI");
        {
            char what[256];
            check(trk_sample_at(e, 3, 60, what, sizeof what) == 1 && !strcmp(what, "a"),
                  "a set without a kit.txt starts at C-4");
            check(trk_sample_at(e, 3, 62, what, sizeof what) == 0 &&
                  strstr(what, "C-4 to C#4") != NULL, "a note past the last sample says where they are");
            check(trk_sample_at(e, 0, 60, what, sizeof what) == -1, "a window track has no samples");
        }

        /* What the editors do: read the set, change it, save it, reload. */
        check(drumkit_map_read(m, kitdir) == 2 && !m->mapped, "a set without a kit.txt reads by name");
        m->pad[1].note = m->pad[0].note;
        check(drumkit_map_check(m) != NULL, "two pads on one note are refused");
        m->pad[1].note = 70;
        m->pad[1].gain_db = -3;
        m->pad[1].choke = 1;
        check(drumkit_map_save(m) == 0, "the set saves as kit.txt");
        trk_reload_sample_set(e, "testkit");
        check(trk_routed(e, 3), "the track plays the saved set");
        memset(m, 0, sizeof *m);
        check(drumkit_map_read(m, kitdir) == 2 && m->mapped && m->pad[1].note == 70 &&
              m->pad[1].choke == 1 && m->pad[1].gain_db == -3, "and reads back as saved");
        free(m);

        trk_add_sample_set(e, kitdir);
        trk_list_sample_sets(e, list, sizeof list);
        check(strstr(list, kitdir) != NULL, "a set loaded from a folder is offered");
        check(trk_audition(e, kitdir, "a.wav", 0, 100) == 0, "a pad plays on its own (Play)");
        trk_lock(e);
        memset(s->track[4].samples, 0, sizeof s->track[4].samples);
        trk_unlock(e);
        trk_route(e);
    }

    printf("tempo change while playing\n");
    /* Pattern 2: a MIDI beat on track 1 and a sample hit on track 4 on every
     * row, so the two must land together -- before and after a tempo change. */
    trk_lock(e);
    s->pattern[2].rows = 8;
    for (i = 0; i < 8; i++) {
        s->pattern[2].cell[i][0].note = 64;
        s->pattern[2].cell[i][3].note = 60;
    }
    trk_unlock(e);
    pthread_mutex_lock(&g_mx); mark = g_nev; pthread_mutex_unlock(&g_mx);
    {
        int played0, played1, non = 0, late = 0;
        double tc, on[64], worst = 0;
        trk_sample_stats(e, NULL, &played0, NULL);
        trk_play(e, TRK_PLAY_PATTERN, 2, 0);
        usleep(400000);                          /* three rows at 120 bpm */
        tc = now_s();
        trk_set_bpm(e, 240);                     /* rows halve to 62.5 ms */
        usleep(900000);
        trk_stop(e);
        usleep(150000);
        trk_sample_stats(e, NULL, &played1, NULL);
        pthread_mutex_lock(&g_mx);
        n = g_nev;
        for (i = mark; i < n; i++) {
            const rec *x = &g_ev[i];
            if (x->rx == 0 && x->ch == 0 && x->note == 64 && is_on(x) && non < 64) on[non++] = x->t;
        }
        check(non >= 12 && non <= 24, "rows keep coming across the tempo change");
        for (i = 1; i < non; i++) {
            double d;
            if (on[i - 1] < tc + 0.15) continue; /* the change itself shortens one */
            d = fabs((on[i] - on[i - 1]) - 0.0625);
            if (d > worst) worst = d;
            late++;
        }
        check(late >= 6 && worst < 0.003, "the beat grid follows the new tempo");
        /* One hit started per row, as one note-on per row: the two queues
         * stay together through the change (the output's own latency lets a
         * hit trail its note-on by up to a row). */
        check(abs(played1 - played0 - non) <= 2, "sample and MIDI tracks stay together");
        check(all_released(mark, n), "nothing left sounding after a tempo change");
        pthread_mutex_unlock(&g_mx);
    }
    trk_set_bpm(e, 120);

    printf("panic with the sample list full\n");
    {
        int pending = 0, tries, c0, c1;
        trk_sample_stats(e, NULL, NULL, &c0);
        /* Preview hits land asap -- pushed faster than the audio thread
         * drains them, the list fills to the top. */
        for (tries = 0; tries < 5 && pending <= 100; tries++) {
            for (i = 0; i < 4000; i++) trk_preview(e, 3, 60, 100);
            trk_sample_stats(e, &pending, NULL, NULL);
        }
        check(pending > 100, "the pending list fills");
        trk_panic(e);
        usleep(300000);
        trk_sample_stats(e, &pending, NULL, &c1);
        check(c1 > c0, "the silence lands though the list was full");
        check(pending == 0, "panic leaves no scheduled hit pending");
    }

    printf("muting a sample track\n");
    pthread_mutex_lock(&g_mx); mark = g_nev; pthread_mutex_unlock(&g_mx);
    {
        int p0, p1, p2, c0, c1;
        /* Fast, so the lookahead holds many queued hits: at 999 bpm and 16
         * rows a beat a row is 15 ms and about eight sit pending. */
        trk_lock(e);
        s->lpb = 16;
        trk_unlock(e);
        trk_set_bpm(e, 999);
        trk_play(e, TRK_PLAY_PATTERN, 2, 0);
        usleep(300000);
        /* Read after the play's own fade has long been applied, so only the
         * mute's can count below. */
        trk_sample_stats(e, NULL, &p0, &c0);
        trk_lock(e);
        s->track[3].mute = 1;
        trk_unlock(e);
        usleep(120000);
        trk_sample_stats(e, NULL, &p1, NULL);
        /* A row or so already due may land; the rest of the queue may not. */
        check(p1 - p0 <= 3, "the track's queued hits are dropped at the mute");
        usleep(300000);
        trk_sample_stats(e, NULL, &p2, &c1);
        check(c1 > c0, "muting fades what the track is sounding");
        check(p2 == p1, "no hit lands while the track is muted");
        trk_lock(e);
        s->track[3].mute = 0;
        trk_unlock(e);
        usleep(300000);
        trk_sample_stats(e, NULL, &p1, NULL);
        check(p1 > p2, "unmuted, the track's hits land again");
        trk_stop(e);
        usleep(150000);
        pthread_mutex_lock(&g_mx);
        check(all_released(mark, g_nev), "nothing left sounding after the muting");
        pthread_mutex_unlock(&g_mx);
        trk_lock(e);
        s->lpb = 4;
        trk_unlock(e);
        trk_set_bpm(e, 120);
    }

    printf("edit mode\n");
    {
        trk_editor ed;
        trk_cell before;
        int row;
        char sheet[4096];
        trk_editor_init(&ed);
        ed.pattern = 2; ed.track = 0; ed.row = 3; ed.field = TRK_F_NOTE;
        trk_key(e, &ed, TRK_K_EDIT);
        check(!ed.edit, "` turns edit mode off");
        trk_lock(e); before = s->pattern[2].cell[3][0]; trk_unlock(e);
        row = ed.row;
        trk_key(e, &ed, 'z');
        trk_key_release(e, &ed, 'z');
        trk_key(e, &ed, '.');
        ed.field = TRK_F_VEL;
        trk_key(e, &ed, '7');
        trk_lock(e);
        check(!memcmp(&before, &s->pattern[2].cell[3][0], sizeof before) && ed.row == row,
              "with it off, keys change nothing and the cursor stays");
        trk_unlock(e);
        trk_key(e, &ed, TRK_K_EDIT);
        ed.field = TRK_F_NOTE;
        trk_key(e, &ed, 'z');
        trk_key_release(e, &ed, 'z');
        trk_lock(e);
        check(ed.edit && s->pattern[2].cell[3][0].note == 60 && ed.row == row + 1,
              "back on, a note key writes and advances");
        s->pattern[2].cell[3][0] = before;
        trk_unlock(e);
        trk_preview_off(e, 0);
        /* Each track its own octave. */
        trk_lock(e); s->track[1].octave = 2; trk_unlock(e);
        ed.track = 1; ed.row = 3; ed.field = TRK_F_NOTE;
        trk_key(e, &ed, 'z');
        trk_key_release(e, &ed, 'z');
        trk_lock(e);
        check(s->pattern[2].cell[3][1].note == 36, "a track at octave 2 types C-2");
        s->pattern[2].cell[3][1] = before;
        trk_unlock(e);
        trk_key(e, &ed, TRK_K_OCT_UP);
        trk_key(e, &ed, TRK_K_TAB);           /* to track 3, still at 4 */
        check(s->track[1].octave == 3 && s->track[0].octave == 4 && s->track[2].octave == 4 &&
              ed.octave == 4, "] changes only the cursor's track, and Tab shows the next one's");
        trk_preview_off(e, 1);
        check(trk_cheat_sheet(e, 3, 4, 20, sheet, sizeof sheet) == 2 && !strncmp(sheet, "z C-4 a\n", 8),
              "a sample track's cheat sheet lists its samples and keys");
        check(trk_cheat_sheet(e, 0, 4, 30, sheet, sizeof sheet) == 6 && strstr(sheet, "C-4..B-4"),
              "a window track's lists the note keys");
    }

    printf("parts\n");
    {
        int at, p2, saved_norder = s->norder, saved_order[TRK_ORDER_MAX];
        char lbl[TRK_NAME_LEN + 8];
        trk_pattern saved_p0;
        memcpy(saved_order, s->order, sizeof saved_order);
        trk_lock(e);
        saved_p0 = s->pattern[0];
        s->norder = 1; s->order[0] = 0;
        snprintf(s->pattern[0].name, sizeof s->pattern[0].name, "Verse");
        trk_unlock(e);
        check(!strcmp(trk_part_label(s, 0, lbl), "Verse") && !strcmp(trk_part_label(s, 7, lbl), "Part 7"),
              "a part shows its name, or its number");
        p2 = trk_pattern_new(e, 0);
        check(p2 > 0 && !strcmp(s->pattern[p2].name, "Verse 2") &&
              s->pattern[p2].cell[0][0].note == s->pattern[0].cell[0][0].note, "Copy makes a named copy");
        at = trk_order_insert(e, 0, p2);
        check(at == 1 && s->norder == 2 && s->order[1] == p2, "a part goes in after the one picked");
        at = trk_order_insert(e, 1, 0);
        check(at == 2 && s->order[2] == 0, "Again plays a part twice");
        at = trk_order_move(e, 2, -2);
        check(at == 0 && s->order[0] == 0 && s->order[1] == 0 && s->order[2] == p2, "parts move");
        check(trk_order_move(e, 0, -1) == -1, "not past the start");
        at = trk_order_remove(e, 2);
        check(at == 1 && s->norder == 2, "a part comes out of the order");
        trk_order_remove(e, 0);
        check(trk_order_remove(e, 0) == -1 && s->norder == 1, "the last part stays");
        trk_lock(e);
        s->pattern[0] = saved_p0;
        memset(&s->pattern[p2], 0, sizeof s->pattern[p2]);
        s->pattern[p2].rows = 64;
        memset(s->pattern[p2].cell, TRK_EMPTY, sizeof s->pattern[p2].cell);
        memcpy(s->order, saved_order, sizeof saved_order);
        s->norder = saved_norder;
        snprintf(s->pattern[1].name, sizeof s->pattern[1].name, "Chorus");
        trk_unlock(e);
    }

    printf("selecting, copy and paste\n");
    {
        trk_editor ed;
        trk_pattern keep;
        int r, ok = 1, rows, tracks;
        trk_editor_init(&ed);
        ed.pattern = 3;
        trk_lock(e);
        keep = s->pattern[3];
        s->pattern[3].rows = 16;
        for (r = 0; r < 4; r++) { s->pattern[3].cell[r][0].note = (uint8_t)(48 + r); s->pattern[3].cell[r][1].note = (uint8_t)(60 + r); }
        trk_unlock(e);
        ed.row = 0; ed.track = 0;
        trk_key(e, &ed, TRK_K_SEL_DOWN); trk_key(e, &ed, TRK_K_SEL_DOWN); trk_key(e, &ed, TRK_K_SEL_DOWN);
        trk_key(e, &ed, TRK_K_SEL_RIGHT);
        check(trk_selected(&ed, 0, 0) && trk_selected(&ed, 3, 1) && !trk_selected(&ed, 4, 0) &&
              !trk_selected(&ed, 0, 2), "Shift and the arrows select a block");
        trk_key(e, &ed, TRK_K_COPY);
        check(trk_clipboard(&rows, &tracks) == 8 && rows == 4 && tracks == 2, "copy takes the block");
        trk_key(e, &ed, TRK_K_DOWN);
        check(!ed.sel, "a plain move ends the selection");
        ed.row = 8; ed.track = 2;
        trk_key(e, &ed, TRK_K_PASTE);
        trk_lock(e);
        for (r = 0; r < 4; r++)
            ok &= s->pattern[3].cell[8 + r][2].note == 48 + r && s->pattern[3].cell[8 + r][3].note == 60 + r;
        trk_unlock(e);
        check(ok && ed.sel && ed.row == 8, "paste puts it down at the cursor, selected");
        ed.row = 14; ed.track = 7;
        trk_select_none(&ed);
        check(trk_paste(e, &ed) == 2, "and cuts it off at the pattern's edges");
        trk_select(&ed, 8, 2, 11, 3);
        trk_key(e, &ed, TRK_K_DELETE);
        trk_lock(e);
        check(s->pattern[3].cell[9][3].note == TRK_EMPTY, "Delete clears the selection");
        trk_unlock(e);
        trk_select(&ed, 0, 0, 3, 0);
        ed.edit = 0;
        trk_key(e, &ed, TRK_K_CUT);
        trk_lock(e);
        check(s->pattern[3].cell[0][0].note == 48, "with edit off, cut changes nothing");
        trk_unlock(e);
        ed.edit = 1;
        trk_key(e, &ed, TRK_K_CUT);
        trk_lock(e);
        check(s->pattern[3].cell[0][0].note == TRK_EMPTY && trk_clipboard(NULL, NULL) == 4,
              "with it on, cut empties the block and keeps it");
        s->pattern[3] = keep;
        trk_unlock(e);
    }

    printf("save and load\n");
    {
        int fd = mkstemp(path);
        if (fd >= 0) close(fd);
        back = malloc(sizeof *back);
        trk_lock(e);
        snprintf(s->track[0].name, sizeof s->track[0].name, "Bass line");
        n = trk_song_save(s, path, err, sizeof err);
        trk_unlock(e);
        check(n == 0, "saved");
        if (n) printf("        %s\n", err);
        n = trk_song_load(back, path, err, sizeof err);
        check(n == 0, "loaded");
        if (n) printf("        %s\n", err);
        trk_lock(e);
        check(n == 0 && !memcmp(back, s, sizeof *s), "the same song after a round trip");
        trk_unlock(e);
        free(back);
        unlink(path);
    }

    printf("octave moves the notes\n");
    {
        trk_editor ed;
        trk_song *snap = malloc(sizeof *snap);
        int i, r, pf, want, moved;
        trk_editor_init(&ed);
        trk_lock(e);
        *snap = *s;
        for (pf = 0; pf < TRK_PATTERNS; pf++) {
            int listed = 0;
            for (i = 0; i < s->norder; i++) if (s->order[i] == pf) listed = 1;
            if (!listed) break;
        }
        check(pf < TRK_PATTERNS, "a pattern no part lists, to move notes in");
        /* Track 2 is at octave 3 from the edit checks above. */
        s->pattern[0].cell[0][1].note = 60;
        s->pattern[0].cell[1][1].note = TRK_NOTE_OFF;
        s->pattern[0].cell[2][1].note = TRK_EMPTY;   /* a controller-only row */
        s->pattern[0].cell[2][1].cc = 7;
        s->pattern[0].cell[2][1].val = 100;
        s->pattern[0].cell[3][1].note = 120;         /* past G-9 when raised */
        s->pattern[0].cell[4][1].note = 13;          /* under C-0 when lowered */
        s->pattern[0].cell[0][0].note = 60;          /* another track's note */
        s->pattern[0].cell[0][2].note = 60;          /* a twin, for the direct call */
        s->pattern[pf].cell[0][1].note = 60;
        trk_unlock(e);
        ed.track = 1;
        trk_key(e, &ed, TRK_K_OCT_UP);
        trk_lock(e);
        check(s->track[1].octave == 4 &&
              s->pattern[0].cell[0][1].note == 72 && s->pattern[pf].cell[0][1].note == 72,
              "] moves the track's notes +12, in a partless pattern too");
        check(s->pattern[0].cell[3][1].note == 127, "a note past G-9 clamps at it");
        check(s->pattern[0].cell[1][1].note == TRK_NOTE_OFF &&
              s->pattern[0].cell[2][1].note == TRK_EMPTY &&
              s->pattern[0].cell[2][1].cc == 7 && s->pattern[0].cell[2][1].val == 100,
              "note-off, empty and controller-only cells stay");
        check(s->pattern[0].cell[0][0].note == 60 && s->pattern[0].cell[0][2].note == 60,
              "the other tracks keep their notes");
        trk_unlock(e);
        trk_key(e, &ed, TRK_K_OCT_DOWN);
        trk_lock(e);
        check(s->track[1].octave == 3 &&
              s->pattern[0].cell[0][1].note == 60 && s->pattern[pf].cell[0][1].note == 60,
              "[ moves them back");
        trk_unlock(e);
        trk_key(e, &ed, TRK_K_OCT_DOWN);
        trk_lock(e);
        check(s->pattern[0].cell[4][1].note == 12, "a note under C-0 clamps at it");
        trk_unlock(e);
        /* The call a window's octave box makes: the same shift the keys
         * gave track 2's twin, and it says how many notes moved. */
        want = 0;
        trk_lock(e);
        for (i = 0; i < TRK_PATTERNS; i++)
            for (r = 0; r < s->pattern[i].rows; r++)
                if (s->pattern[i].cell[r][2].note <= 127) want++;
        trk_unlock(e);
        moved = trk_track_set_octave(e, 2, 5);
        trk_lock(e);
        check(moved == want && s->track[2].octave == 5 &&
              s->pattern[0].cell[0][2].note == 72,
              "trk_track_set_octave moves them the same and returns the count");
        trk_unlock(e);
        check(trk_track_set_octave(e, 2, 5) == 0, "no change, nothing moves");
        check(trk_track_set_octave(e, TRK_TRACKS, 5) == -1, "a bad track is refused");
        trk_lock(e);
        *s = *snap;
        trk_unlock(e);
        free(snap);
    }

    printf("undo\n");
    {
        trk_editor ed;
        trk_song *snap = malloc(sizeof *snap);
        int i, n0, first;
        trk_editor_init(&ed);
        trk_undo_clear(e);
        trk_lock(e);
        *snap = *s;
        /* Earlier checks left notes in pattern 0; entry only snapshots a
         * cell it actually changes, so start from an empty one. */
        memset(s->pattern[0].cell, TRK_EMPTY, sizeof s->pattern[0].cell);
        trk_unlock(e);

        /* Entry: two notes, taken back one at a time, then nothing left. */
        ed.pattern = 0; ed.row = 0; ed.track = 0; ed.field = TRK_F_NOTE;
        trk_key(e, &ed, 'z'); trk_key_release(e, &ed, 'z');
        trk_key(e, &ed, 'x'); trk_key_release(e, &ed, 'x');
        trk_lock(e);
        check(s->pattern[0].cell[0][0].note == 60 && s->pattern[0].cell[1][0].note == 62,
              "two notes typed");
        trk_unlock(e);
        check(trk_key(e, &ed, TRK_K_UNDO) == 1, "ctrl+z answers a redraw");
        trk_lock(e);
        check(s->pattern[0].cell[1][0].note == TRK_EMPTY && s->pattern[0].cell[0][0].note == 60,
              "the last note is taken back");
        trk_unlock(e);
        check(trk_undo(e) == 1, "trk_undo too");
        trk_lock(e);
        check(s->pattern[0].cell[0][0].note == TRK_EMPTY, "and the first");
        trk_unlock(e);
        check(trk_undo(e) == 0 && trk_key(e, &ed, TRK_K_UNDO) == 0,
              "an empty history says so");

        /* One undo step per hex value, not per digit. */
        ed.row = 2; ed.field = TRK_F_VEL;
        trk_key(e, &ed, '4');
        trk_key(e, &ed, '0');
        trk_lock(e);
        check(s->pattern[0].cell[2][0].vel == 0x40, "hex 4 0 into velocity");
        trk_unlock(e);
        check(trk_undo(e) == 1, "the value undone in one step");
        trk_lock(e);
        check(s->pattern[0].cell[2][0].vel == TRK_EMPTY, "both digits of it");
        trk_unlock(e);

        /* Insert and backspace. */
        ed.row = 4; ed.field = TRK_F_NOTE;
        trk_key(e, &ed, 'z'); trk_key_release(e, &ed, 'z');   /* row 4, advances to 5 */
        ed.row = 4;
        trk_key(e, &ed, TRK_K_INSERT);
        trk_lock(e);
        check(s->pattern[0].cell[4][0].note == TRK_EMPTY && s->pattern[0].cell[5][0].note == 60,
              "insert pushed the note down");
        trk_unlock(e);
        trk_undo(e);
        trk_lock(e);
        check(s->pattern[0].cell[4][0].note == 60, "insert undone");
        trk_unlock(e);
        ed.row = 4;
        trk_key(e, &ed, TRK_K_DELETE);
        trk_lock(e);
        check(s->pattern[0].cell[4][0].note == TRK_EMPTY, "delete cleared it");
        trk_unlock(e);
        trk_undo(e);
        trk_lock(e);
        check(s->pattern[0].cell[4][0].note == 60, "delete undone");
        trk_unlock(e);
        ed.row = 4;
        trk_key(e, &ed, TRK_K_DELETE);               /* gone again, for what follows */

        /* An octave move, notes and octave number together. */
        ed.track = 1;                                /* at octave 3 from above */
        trk_lock(e);
        s->pattern[0].cell[0][1].note = 60;
        trk_unlock(e);
        trk_key(e, &ed, TRK_K_OCT_UP);
        trk_lock(e);
        check(s->track[1].octave == 4 && s->pattern[0].cell[0][1].note == 72,
              "octave up moved the notes");
        trk_unlock(e);
        trk_undo(e);
        trk_lock(e);
        check(s->track[1].octave == 3 && s->pattern[0].cell[0][1].note == 60,
              "undone, notes and octave number together");
        trk_unlock(e);

        /* Cut and paste. */
        ed.track = 0;
        trk_lock(e);
        s->pattern[0].cell[0][0].note = 60;
        s->pattern[0].cell[1][0].note = 62;
        trk_unlock(e);
        trk_select(&ed, 0, 0, 1, 0);
        trk_cut(e, &ed);
        trk_lock(e);
        check(s->pattern[0].cell[0][0].note == TRK_EMPTY, "cut emptied the block");
        trk_unlock(e);
        trk_undo(e);
        trk_lock(e);
        check(s->pattern[0].cell[0][0].note == 60 && s->pattern[0].cell[1][0].note == 62,
              "cut undone");
        trk_unlock(e);

        /* The parts list. */
        trk_lock(e);
        n0 = s->norder; first = s->order[0];
        trk_unlock(e);
        trk_order_insert(e, -1, 7);
        trk_lock(e);
        check(s->norder == n0 + 1 && s->order[0] == 7, "a part inserted at the front");
        trk_unlock(e);
        trk_undo(e);
        trk_lock(e);
        check(s->norder == n0 && s->order[0] == first, "the insert undone");
        trk_unlock(e);

        /* The ring holds TRK_UNDO_MAX: more edits than that drop the
         * oldest, and draining it ends exactly there. */
        trk_undo_clear(e);
        trk_lock(e);
        s->pattern[0].rows = 64;
        s->track[0].octave = 4;
        memset(s->pattern[0].cell, TRK_EMPTY, sizeof s->pattern[0].cell);
        trk_unlock(e);
        ed.track = 0; ed.row = 0; ed.field = TRK_F_NOTE; ed.step = 1; ed.digit = 0;
        for (i = 0; i < TRK_UNDO_MAX + 5; i++) {
            trk_key(e, &ed, 'z');
            trk_key_release(e, &ed, 'z');
        }
        for (i = 0; i < TRK_UNDO_MAX; i++)
            if (trk_undo(e) != 1) break;
        check(i == TRK_UNDO_MAX, "fifty edits kept, fifty undos");
        check(trk_undo(e) == 0, "the fifty-first is gone");
        trk_lock(e);
        check(s->pattern[0].cell[4][0].note == 60 && s->pattern[0].cell[5][0].note == TRK_EMPTY,
              "the oldest five edits fell off the ring");
        trk_unlock(e);

        /* Clearing the history: a loaded song starts with none. */
        trk_undo_clear(e);
        check(trk_undo(e) == 0, "a cleared history stays empty");

        /* Routing is not undone: the song's sink name follows the track's
         * live routing, whichever way the snapshot read. */
        {
            int sid = trk_add_sink(e, "undo sink", sink_cb, NULL);
            trk_key(e, &ed, TRK_K_EDIT);
            trk_key(e, &ed, 'z');                    /* a snapshot, sink-less */
            trk_route_sink(e, 0, sid);
            trk_undo(e);
            check(trk_sink_of(e, 0) == sid && !strcmp(s->track[0].sink, "undo sink"),
                  "undo past a route keeps the song's sink true");
            trk_route_sink(e, 0, -1);
            check(s->track[0].sink[0] == 0, "routed back, no sink saved");
            trk_remove_sink(e, sid);
        }

        trk_lock(e);
        *s = *snap;
        trk_unlock(e);
        free(snap);
    }

    printf("recording\n");
    {
        trk_editor ed;
        trk_song *keep = malloc(sizeof *keep);
        trk_rec_opts o;
        snd_seq_t *tx = NULL;
        int tport = -1, tclient = -1, sent_ok = 0, got;
        trk_editor_init(&ed);
        trk_stop(e);
        trk_undo_clear(e);
        trk_lock(e);
        *keep = *s;
        memset(s->pattern[0].cell, TRK_EMPTY, sizeof s->pattern[0].cell);
        s->pattern[0].rows = 16;
        s->lpb = 4;
        s->bpm = 120;                                 /* a row is 125 ms */
        s->norder = 1; s->order[0] = 0;
        { int t; for (t = 0; t < TRK_TRACKS; t++) { s->track[t].mute = 0; s->track[t].samples[0] = 0; s->track[t].velocity = 100; } }
        trk_unlock(e);
        trk_set_bpm(e, 120);
        trk_record_defaults(&o);
        o.count_in = 0; o.metronome = 0; o.monitor = 0;
        trk_record_set(e, &o);

        check(trk_recording(e) == 0 && trk_take_events(e) == 0, "not recording to begin with");
        trk_record_note(e, 0, 60, 100, 1);
        check(s->pattern[0].cell[0][0].note == TRK_EMPTY, "keys are ignored when not recording");

        /* A take, from the computer keyboard's entry point. */
        trk_record_arm(e, 0);
        trk_record_start(e, TRK_PLAY_PATTERN, 0, 0);
        check(trk_recording(e) == 1, "recording, no count-in");
        wait_row(e, 2, 20);   trk_record_note(e, 0, 60, -1, 1);       /* early in row 2 */
        wait_row(e, 3, 20);   trk_record_note(e, 0, 60, -1, 0);       /* let go early in row 3 */
        wait_row(e, 5, 90);   trk_record_note(e, 0, 62, 80, 1);       /* late in row 5: row 6 */
        wait_row(e, 7, 20);   trk_record_note(e, 0, 62, 80, 0);

        /* The same from a MIDI source: another client writing to Record In. */
        if (snd_seq_open(&tx, "default", SND_SEQ_OPEN_OUTPUT, 0) >= 0) {
            snd_seq_set_client_name(tx, "trktest source");
            tport = snd_seq_create_simple_port(tx, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                               SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
            tclient = find_client(tx, trk_client_name(e));
            if (tclient >= 0 && tport >= 0) {
                int ip = find_port(tx, tclient, "Record In");
                sent_ok = ip >= 0 && snd_seq_connect_to(tx, tport, tclient, ip) >= 0;
            }
        }
        check(sent_ok, "the tracker has a Record In port to connect to");
        if (sent_ok) {
            snd_seq_event_t ev;
            wait_row(e, 9, 20);
            snd_seq_ev_clear(&ev); snd_seq_ev_set_source(&ev, tport); snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev); snd_seq_ev_set_noteon(&ev, 0, 64, 90);
            snd_seq_event_output_direct(tx, &ev);
            wait_row(e, 10, 20);
            snd_seq_ev_clear(&ev); snd_seq_ev_set_source(&ev, tport); snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev); snd_seq_ev_set_noteoff(&ev, 0, 64, 0);
            snd_seq_event_output_direct(tx, &ev);
            wait_row(e, 12, 20);
            snd_seq_ev_clear(&ev); snd_seq_ev_set_source(&ev, tport); snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev); snd_seq_ev_set_controller(&ev, 0, 7, 99);
            snd_seq_event_output_direct(tx, &ev);
            /* Another client's bytes are passed through as they are: a note past 127
             * must not reach the pattern or the take. */
            wait_row(e, 13, 20);
            snd_seq_ev_clear(&ev); snd_seq_ev_set_source(&ev, tport); snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev); snd_seq_ev_set_noteon(&ev, 0, 200, 90);
            snd_seq_event_output_direct(tx, &ev);
            snd_seq_ev_set_noteon(&ev, 0, 255, 90);
            snd_seq_event_output_direct(tx, &ev);
            snd_seq_ev_set_noteoff(&ev, 0, 254, 0);
            snd_seq_event_output_direct(tx, &ev);
        }
        wait_row(e, 14, 0);
        trk_stop(e);
        usleep(30000);

        check(s->pattern[0].cell[2][0].note == 60 && s->pattern[0].cell[2][0].vel == TRK_EMPTY,
              "a key early in a row lands on that row, at the track's own velocity");
        check(s->pattern[0].cell[3][0].note == TRK_NOTE_OFF, "its release writes === on the row it falls on");
        check(s->pattern[0].cell[6][0].note == 62 && s->pattern[0].cell[6][0].vel == 80,
              "a key late in a row rounds up to the next, with its velocity");
        check(s->pattern[0].cell[5][0].note == TRK_EMPTY, "and the row it was struck in stays empty");
        check(s->pattern[0].cell[7][0].note == TRK_NOTE_OFF, "that release too");
        if (sent_ok) {
            check(s->pattern[0].cell[9][0].note == 64 && s->pattern[0].cell[9][0].vel == 90,
                  "a MIDI note is placed by the tick the sequencer stamped it with");
            check(s->pattern[0].cell[10][0].note == TRK_NOTE_OFF, "and its note-off");
            check(s->pattern[0].cell[12][0].cc == 7 && s->pattern[0].cell[12][0].val == 99,
                  "a MIDI controller goes into the cc columns");
        }
        { int r, bad = 0;
          for (r = 0; r < 16; r++) {
              const int n = s->pattern[0].cell[r][0].note;
              if (n > 127 && n != TRK_NOTE_OFF && n != TRK_EMPTY) bad = 1;
          }
          check(!bad, "a MIDI note past 127 is dropped, not written into the pattern"); }
        got = trk_take_events(e);
        check(got >= 4, "the take kept every event as played");
        {
            char err[200] = "", path[256];
            unsigned char *f; long len; FILE *fp; int i, ons = 0, tempo_ok = 0;
            snprintf(path, sizeof path, "/tmp/trktest-take-%d.mid", (int)getpid());
            check(trk_take_export_midi(e, path, err, sizeof err) == 0, "the take is written as a MIDI file");
            fp = fopen(path, "rb");
            f = fp ? malloc(65536) : NULL;
            len = f ? (long)fread(f, 1, 65536, fp) : 0;
            if (fp) fclose(fp);
            unlink(path);
            check(len > 22 && !memcmp(f, "MThd", 4) && f[9] == 1, "an SMF, type 1");
            for (i = 0; i + 2 < len; i++) {
                if (f[i] == 0x90 && (f[i + 1] == 60 || f[i + 1] == 62 || f[i + 1] == 64)) ons++;
                if (f[i] == 0xFF && f[i + 1] == 0x51 && f[i + 2] == 3 && f[i + 3] == 0x07 && f[i + 4] == 0xA1) tempo_ok = 1;
            }
            check(ons >= 3, "the notes are in it");
            check(tempo_ok, "at the song's tempo (120 BPM)");
            free(f);
        }
        check(trk_undo(e) == 1 && s->pattern[0].cell[2][0].note == TRK_EMPTY && s->pattern[0].cell[6][0].note == TRK_EMPTY,
              "one undo takes the whole take back out");

        /* Quantizing to the row sounding keeps a late note where it is. */
        o.quantize = TRK_REC_ROW; trk_record_set(e, &o);
        trk_record_start(e, TRK_PLAY_PATTERN, 0, 0);
        wait_row(e, 5, 90);   trk_record_note(e, 0, 65, -1, 1);
        trk_stop(e);
        check(s->pattern[0].cell[5][0].note == 65 && s->pattern[0].cell[6][0].note == TRK_EMPTY,
              "quantize to the row sounding: a late note stays on its row");
        trk_undo(e);

        /* A count-in: the take starts after it, and keys before it are nobody's. */
        o.quantize = TRK_REC_NEAREST; o.count_in = 1; trk_record_set(e, &o);
        trk_set_bpm(e, 240);                          /* a bar is 1 s */
        trk_record_start(e, TRK_PLAY_PATTERN, 0, 0);
        check(trk_recording(e) == 2, "counting in");
        usleep(200000);
        trk_record_note(e, 0, 67, -1, 1);
        usleep(900000);
        check(trk_recording(e) == 1, "recording once the count-in is over");
        trk_stop(e);
        check(trk_recording(e) == 0, "stopping ends the take");
        { int r, any = 0; for (r = 0; r < 16; r++) if (s->pattern[0].cell[r][0].note != TRK_EMPTY) any = 1;
          check(!any, "a key struck during the count-in was not written"); }
        trk_undo(e);

        /* Options out of range are clamped. */
        o.count_in = 99; o.offset_ms = -999; trk_record_set(e, &o);
        trk_record_get(e, &o);
        check(o.count_in == 4 && o.offset_ms == -200, "options are clamped");

        if (tx) snd_seq_close(tx);
        trk_set_bpm(e, 120);
        trk_undo_clear(e);
        trk_lock(e);
        *s = *keep;
        trk_unlock(e);
        free(keep);
    }

    printf("robustness\n");
    {
        trk_song *a = malloc(sizeof *a), *b = malloc(sizeof *b);
        char err[300] = "", path[256], tmp[300];
        FILE *fp;
        trk_editor ed;
        trk_editor_init(&ed);
        trk_stop(e);
        trk_undo_clear(e);
        trk_song_init(a);
        snprintf(a->track[0].name, sizeof a->track[0].name, "two\nlines");
        snprintf(a->track[1].client, sizeof a->track[1].client, "a client\rwith a break");
        a->bpm = 123.456789;
        snprintf(path, sizeof path, "/tmp/trktest-robust-%d.trk", (int)getpid());
        check(trk_song_save(a, path, err, sizeof err) == 0, "a song with line breaks in its names saves");
        check(trk_song_load(b, path, err, sizeof err) == 0, "and loads back, not as an unreadable file");
        check(!strcmp(b->track[0].name, "two lines") && !strcmp(b->track[1].client, "a client with a break"),
              "the breaks became spaces");
        check(b->bpm == 123.456789, "the tempo keeps its digits");
        fp = fopen(path, "w");
        fprintf(fp, "tracker 1\nbpm nan\nlpb 4\n");
        fclose(fp);
        check(trk_song_load(b, path, err, sizeof err) != 0, "a tempo of nan is refused");
        unlink(path); snprintf(tmp, sizeof tmp, "%s.new", path); unlink(tmp);

        trk_set_bpm(e, 130);
        trk_set_bpm(e, NAN);
        check(trk_song_of(e)->bpm == 130, "set_bpm(nan) leaves the tempo as it was");

        /* Undo is for edits: the tempo the queue runs at stays the live one. */
        trk_lock(e);
        memset(s->pattern[0].cell, TRK_EMPTY, sizeof s->pattern[0].cell);
        s->pattern[0].cell[0][0].note = 60;
        trk_unlock(e);
        ed.edit = 1; ed.row = 0; ed.track = 0; ed.pattern = 0;
        trk_set_bpm(e, 140);
        trk_key(e, &ed, TRK_K_TRANSPOSE_UP);              /* a snapshot, at 140 */
        trk_set_bpm(e, 100);
        check(trk_undo(e) == 1 && s->pattern[0].cell[0][0].note == 60, "the edit is undone");
        check(trk_song_of(e)->bpm == 100, "and the tempo is still the live one, not the snapshot's");
        trk_undo_clear(e);

        /* A song's path that cannot be written leaves nothing half-written behind. */
        a->bpm = 120;
        snprintf(path, sizeof path, "/tmp/trktest-nodir-%d/x.mid", (int)getpid());
        check(trk_song_export_midi(a, path, err, sizeof err) != 0, "an export to a folder that is not there fails");
        {
            int id = trk_add_sink(e, "unroute sink", sink_cb, NULL);
            trk_route_sink(e, 3, id);
            check(trk_sink_of(e, 3) == id, "a track is routed to a sink");
            trk_unroute_sinks(e);
            check(trk_sink_of(e, 3) == -1 && s->track[3].sink[0] == 0,
                  "before another song comes in, every track is back on its window");
            trk_remove_sink(e, id);
        }
        trk_set_bpm(e, 120);
        free(a); free(b);
    }

    printf("tracks\n");
    {
        trk_song *keep = malloc(sizeof *keep), *a = malloc(sizeof *a), *b = malloc(sizeof *b);
        char err[300] = "", path[256], tmp[300];
        int i, id;
        trk_stop(e);
        trk_undo_clear(e);
        trk_lock(e);
        *keep = *s;
        trk_song_init(s);
        trk_unlock(e);
        check(s->ntracks == 8, "a new song has eight tracks");

        /* Notes follow their tracks. */
        s->pattern[0].cell[0][0].note = 60;
        s->pattern[0].cell[0][2].note = 62;
        snprintf(s->track[2].name, sizeof s->track[2].name, "Lead");
        check(trk_track_insert(e, 1) == 0 && s->ntracks == 9, "a track is added");
        check(s->pattern[0].cell[0][0].note == 60 && s->pattern[0].cell[0][1].note == TRK_EMPTY &&
              s->pattern[0].cell[0][3].note == 62, "the new track is empty and the ones after it moved right");
        check(!strcmp(s->track[3].name, "Lead") && !strcmp(s->track[1].name, "Track 2"), "with their names");
        check(trk_track_used(e, 3) && !trk_track_used(e, 1), "a track says whether it holds anything");
        check(trk_undo(e) == 1 && s->ntracks == 8 && s->pattern[0].cell[0][2].note == 62, "undone");
        check(trk_redo(e) == 1 && s->ntracks == 9, "redone");
        check(trk_track_remove(e, 0) == 0 && s->ntracks == 8, "a track is taken away");
        check(s->pattern[0].cell[0][2].note == 62 && !strcmp(s->track[2].name, "Lead") &&
              s->pattern[0].cell[0][0].note == TRK_EMPTY, "its notes go with it; the rest move left");
        check(trk_undo(e) == 1 && s->pattern[0].cell[0][0].note == 60 && s->ntracks == 9, "and it comes back with undo");
        trk_undo_clear(e);

        /* The ends. */
        for (i = s->ntracks; i < TRK_TRACKS; i++) trk_track_insert(e, i);
        check(s->ntracks == TRK_TRACKS && trk_track_insert(e, 0) == -1, "no more than the maximum");
        for (i = 0; i < TRK_TRACKS - 1; i++) trk_track_remove(e, 0);
        check(s->ntracks == 1 && trk_track_remove(e, 0) == -1, "never fewer than one");
        trk_lock(e); trk_song_init(s); trk_unlock(e);

        /* A synth route moves with its track. */
        id = trk_add_sink(e, "track-move sink", sink_cb, NULL);
        trk_route_sink(e, 3, id);
        trk_track_insert(e, 0);
        check(trk_sink_of(e, 4) == id && trk_sink_of(e, 3) == -1 && !strcmp(s->track[4].sink, "track-move sink") && !s->track[3].sink[0],
              "a track routed to a synth keeps it when tracks are added before it");
        trk_track_remove(e, 0);
        check(trk_sink_of(e, 3) == id, "and when they are taken away");
        trk_route_sink(e, 3, -1);
        trk_remove_sink(e, id);

        /* The song file: a count other than eight is written, and read back. */
        *a = *s;
        a->ntracks = 12;
        snprintf(a->track[10].name, sizeof a->track[10].name, "Eleven");
        a->pattern[0].cell[3][10].note = 64;
        snprintf(path, sizeof path, "/tmp/trktest-tracks-%d.trk", (int)getpid());
        check(trk_song_save(a, path, err, sizeof err) == 0 && trk_song_load(b, path, err, sizeof err) == 0 &&
              b->ntracks == 12 && !strcmp(b->track[10].name, "Eleven") && b->pattern[0].cell[3][10].note == 64,
              "a song of twelve tracks saves and loads");
        a->ntracks = 8;
        a->pattern[0].cell[3][10].note = TRK_EMPTY;
        trk_song_save(a, path, err, sizeof err);
        { FILE *fp = fopen(path, "r"); char line[200]; int has = 0; while (fp && fgets(line, sizeof line, fp)) if (!strncmp(line, "tracks", 6)) has = 1; if (fp) fclose(fp);
          check(!has, "a song of eight tracks has no tracks line, so older builds still read it"); }
        {   /* A file that uses a track past its count has that many tracks. */
            FILE *fp = fopen(path, "w");
            fprintf(fp, "tracker 1\nbpm 120\nlpb 4\npattern 0 rows 4\ncell 0 1 12 C-4 .. .. ..\norder 0\n");
            fclose(fp);
            check(trk_song_load(b, path, err, sizeof err) == 0 && b->ntracks == 12, "a file that uses track 12 has twelve tracks");
            fp = fopen(path, "w");
            fprintf(fp, "tracker 1\ntracks 99\n");
            fclose(fp);
            check(trk_song_load(b, path, err, sizeof err) != 0, "a count past the maximum is refused");
        }
        unlink(path); snprintf(tmp, sizeof tmp, "%s.new", path); unlink(tmp);

        /* The cursor never rests on a track that is not there. */
        {
            trk_editor ed;
            trk_editor_init(&ed);
            ed.track = 7; ed.edit = 1;
            trk_lock(e); trk_song_init(s); trk_unlock(e);
            trk_track_remove(e, 7);
            trk_key(e, &ed, TRK_K_DOWN);
            check(ed.track == 6, "a cursor on a removed track moves to the last one");
            trk_key(e, &ed, TRK_K_TAB);
            check(ed.track == 0, "Tab wraps at the song's track count, not the maximum");
        }

        trk_undo_clear(e);
        trk_lock(e); *s = *keep; trk_unlock(e);
        free(keep); free(a); free(b);
    }

    printf("furnace keys\n");
    {
        trk_editor ed;
        trk_song *keep = malloc(sizeof *keep);
        int t;
        trk_editor_init(&ed);
        trk_undo_clear(e);
        trk_lock(e);
        *keep = *s;
        memset(s->pattern[0].cell, TRK_EMPTY, sizeof s->pattern[0].cell);
        s->pattern[0].rows = 16;
        for (t = 0; t < TRK_TRACKS; t++) { s->track[t].mute = 0; s->track[t].samples[0] = 0; }
        s->pattern[0].cell[0][0].note = 60;
        s->pattern[0].cell[1][0].note = 64; s->pattern[0].cell[1][0].vel = 0x40;
        s->pattern[0].cell[2][0].note = TRK_NOTE_OFF;
        trk_unlock(e);
        ed.edit = 1; ed.pattern = 0; ed.row = 0; ed.track = 0;

        /* Redo puts back what undo took off; a new edit forgets it. */
        trk_key(e, &ed, TRK_K_TRANSPOSE_UP);
        check(s->pattern[0].cell[0][0].note == 61 && s->pattern[0].cell[1][0].note == 64,
              "transpose moves the cell under the cursor a semitone");
        trk_key(e, &ed, TRK_K_TRANSPOSE_OCT_DOWN);
        check(s->pattern[0].cell[0][0].note == 49, "and an octave down");
        trk_key(e, &ed, TRK_K_SEL_DOWN); trk_key(e, &ed, TRK_K_SEL_DOWN);
        trk_key(e, &ed, TRK_K_TRANSPOSE_UP);
        check(s->pattern[0].cell[0][0].note == 50 && s->pattern[0].cell[1][0].note == 65 &&
              s->pattern[0].cell[2][0].note == TRK_NOTE_OFF, "the selection moves; a note-off stays");
        check(trk_key(e, &ed, TRK_K_UNDO) == 1 && s->pattern[0].cell[1][0].note == 64, "undone");
        check(trk_key(e, &ed, TRK_K_REDO) == 1 && s->pattern[0].cell[1][0].note == 65, "redone");
        check(trk_key(e, &ed, TRK_K_REDO) == 0, "nothing more to redo");
        trk_key(e, &ed, TRK_K_UNDO);
        ed.sel = 0; ed.row = 0;
        trk_key(e, &ed, TRK_K_TRANSPOSE_DOWN);                  /* a new branch */
        check(trk_redo(e) == 0, "a new edit clears the redo");
        {
            const int before = s->pattern[0].cell[0][0].note;
            ed.edit = 0;
            trk_key(e, &ed, TRK_K_TRANSPOSE_UP);
            check(s->pattern[0].cell[0][0].note == before, "edit off: transpose does nothing");
        }
        ed.edit = 1;
        s->pattern[0].cell[0][0].note = 126;
        trk_key(e, &ed, TRK_K_TRANSPOSE_OCT_UP);
        check(s->pattern[0].cell[0][0].note == 127, "transpose stops at G-9");

        /* Mute, solo, unmute. */
        ed.track = 2;
        trk_key(e, &ed, TRK_K_MUTE_TRACK);
        check(s->track[2].mute && !s->track[1].mute, "mute the cursor's track");
        trk_key(e, &ed, TRK_K_MUTE_TRACK);
        check(!s->track[2].mute, "and back");
        trk_key(e, &ed, TRK_K_SOLO_TRACK);
        for (t = 0; t < s->ntracks; t++) if (s->track[t].mute != (t != 2)) break;
        check(t == s->ntracks, "solo mutes every other track");
        trk_key(e, &ed, TRK_K_SOLO_TRACK);
        for (t = 0; t < s->ntracks; t++) if (s->track[t].mute) break;
        check(t == s->ntracks, "soloing the one alone brings everyone back");
        trk_key(e, &ed, TRK_K_MUTE_TRACK); trk_key(e, &ed, TRK_K_UNMUTE_ALL);
        check(!s->track[2].mute, "unmute all");

        /* Edit step from the keypad keys. */
        ed.step = 1;
        trk_key(e, &ed, TRK_K_STEP_UP);   check(ed.step == 2, "step up");
        trk_key(e, &ed, TRK_K_STEP_DOWN); trk_key(e, &ed, TRK_K_STEP_DOWN); trk_key(e, &ed, TRK_K_STEP_DOWN);
        check(ed.step == 0, "step down stops at 0");

        /* Paste mix lays down only what the clipboard has. */
        trk_lock(e);
        memset(s->pattern[0].cell, TRK_EMPTY, sizeof s->pattern[0].cell);
        s->pattern[0].cell[0][0].note = 60; s->pattern[0].cell[0][0].vel = 0x50;
        s->pattern[0].cell[4][0].vel = 0x20;                     /* a velocity alone, no note */
        trk_unlock(e);
        ed.sel = 0; ed.row = 4; ed.track = 0;
        trk_select(&ed, 4, 0, 4, 0);
        trk_key(e, &ed, TRK_K_COPY);                              /* just that velocity */
        ed.sel = 0; ed.row = 0; ed.track = 0;
        trk_key(e, &ed, TRK_K_PASTE_MIX);
        check(s->pattern[0].cell[0][0].note == 60 && s->pattern[0].cell[0][0].vel == 0x20,
              "paste mix: the note stays, the clipboard's velocity goes over");
        trk_key(e, &ed, TRK_K_UNDO);
        trk_key(e, &ed, TRK_K_PASTE);
        check(s->pattern[0].cell[0][0].note == TRK_EMPTY, "plain paste replaces the whole cell");

        /* Selecting a page at a time. */
        ed.sel = 0; ed.row = 0; ed.track = 0;
        trk_key(e, &ed, TRK_K_SEL_PGDN);
        {
            int r0, t0, r1, t1;
            check(trk_selection(&ed, &r0, &t0, &r1, &t1) && r0 == 0 && r1 == 15,
                  "shift+page down selects a page of rows (to the pattern's end)");
        }

        trk_undo_clear(e);
        trk_lock(e);
        *s = *keep;
        trk_unlock(e);
        free(keep);
    }

    printf("midi export\n");
    {
        trk_song *m = malloc(sizeof *m);
        trk_mev *ev;
        int n, i, ons = 0, offs = 0;
        unsigned char *f;
        long len;
        char err[200] = "", path[256];
        FILE *fp;
        trk_song_init(m);
        m->bpm = 120; m->lpb = 4;
        memset(m->pattern[0].cell, TRK_EMPTY, sizeof m->pattern[0].cell);
        m->pattern[0].rows = 8;
        m->track[0].velocity = 90; m->track[0].channel = 2;
        m->pattern[0].cell[0][0].note = 60;                     /* default velocity */
        m->pattern[0].cell[2][0].note = 64; m->pattern[0].cell[2][0].vel = 0x40;   /* ends the first */
        m->pattern[0].cell[3][0].cc = 7;    m->pattern[0].cell[3][0].val = 100;
        m->pattern[0].cell[4][0].note = TRK_NOTE_OFF;
        m->pattern[0].cell[6][0].note = 67;                     /* held to the end */
        m->pattern[0].cell[1][2].note = 36;                     /* a muted track */
        m->track[2].mute = 1;
        m->norder = 1; m->order[0] = 0;
        check(trk_song_ticks(m) == 8u * (TRK_MIDI_PPQ / 4), "song length is rows * row ticks");
        check(trk_song_events(m, 0, &ev, &n) == 0 && n == 4, "track 1: three notes and a controller");
        if (n == 4) {
            const unsigned row = TRK_MIDI_PPQ / 4;
            check(ev[0].start == 0 && ev[0].end == 2 * row && ev[0].a == 60 && ev[0].b == 90 && ev[0].chan == 2,
                  "a note ends where the next begins, default velocity, its channel");
            check(ev[1].start == 2 * row && ev[1].end == 4 * row && ev[1].b == 0x40,
                  "the next note ends at the note-off");
            check(ev[2].is_cc && ev[2].start == 3 * row && ev[2].a == 7 && ev[2].b == 100, "the controller");
            check(ev[3].start == 6 * row && ev[3].end == 8 * row, "a held note ends with the song");
        }
        free(ev);
        m->track[1].samples[0] = 's'; m->track[1].samples[1] = 0;
        m->pattern[0].cell[1][1].note = 36; m->pattern[0].cell[2][1].note = 38;
        check(trk_song_events(m, 1, &ev, &n) == 0 && n == 2 &&
              ev[0].end - ev[0].start == TRK_MIDI_PPQ / 4 && ev[1].start == 2 * (TRK_MIDI_PPQ / 4),
              "a sample-set hit plays out for one row");
        free(ev);
        check(trk_song_events(m, TRK_TRACKS, &ev, &n) == -1, "a bad track is refused");

        snprintf(path, sizeof path, "/tmp/trktest-%d.mid", (int)getpid());
        check(trk_song_export_midi(m, path, err, sizeof err) == 0, "the file is written");
        fp = fopen(path, "rb");
        f = fp ? malloc(65536) : NULL;
        len = f ? (long)fread(f, 1, 65536, fp) : 0;
        if (fp) fclose(fp);
        unlink(path);
        check(len > 22 && !memcmp(f, "MThd", 4) && f[9] == 1, "an SMF, type 1");
        /* tempo track, track 1, the sample track: the muted one is left out */
        check(len > 22 && f[10] == 0 && f[11] == 3, "a tempo track and two playing tracks");
        check(len > 22 && f[12] == (TRK_MIDI_PPQ >> 8) && f[13] == (TRK_MIDI_PPQ & 255), "the resolution");
        for (i = 0; i + 2 < len; i++) {
            if (f[i] == 0x92 && f[i + 1] == 60 && f[i + 2] == 90) ons++;
            if (f[i] == 0x82 && f[i + 1] == 60) offs++;
        }
        check(ons == 1 && offs == 1, "the first note's on and off are in the track");
        check(len > 22 && f[len - 3] == 0xFF && f[len - 2] == 0x2F && f[len - 1] == 0, "ends with end-of-track");
        free(f);
        free(m);
    }

    trk_close(e);                                /* while playing */
    usleep(150000);
    pthread_mutex_lock(&g_mx);
    check(all_released(mark, g_nev), "closing while playing releases everything");
    pthread_mutex_unlock(&g_mx);

    snprintf(wavp, sizeof wavp, "%s/kit.txt", kitdir); unlink(wavp);
    snprintf(wavp, sizeof wavp, "%s/a.wav", kitdir); unlink(wavp);
    snprintf(wavp, sizeof wavp, "%s/b.wav", kitdir); unlink(wavp);
    rmdir(kitdir);
    rmdir(kitroot);

    g_stop = 1;
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    snd_seq_close(ra);
    snd_seq_close(rb);
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail;
}
