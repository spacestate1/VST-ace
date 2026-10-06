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
        check(seen == TRK_TRACKS + 1 && refused,
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
        check(trk_cheat_sheet(e, 0, 4, 30, sheet, sizeof sheet) == 5 && strstr(sheet, "C-4..B-4"),
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
