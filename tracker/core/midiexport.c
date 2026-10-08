/* The song as MIDI events, and as a standard MIDI file. The walk here is the
 * engine's schedule_row in another form: the same one-note-per-track rule,
 * the same default velocity, the same clamp -- so what Reaper (or any other
 * program) is given is what the tracker sounds. */

#include "trk.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int lpb_of(const trk_song *s) { return trk_lpb_ok(s->lpb) ? s->lpb : 4; }

unsigned trk_song_ticks(const trk_song *s)
{
    unsigned t = 0;
    int o;
    for (o = 0; o < s->norder; o++) {
        int p = s->order[o];
        if (p < 0 || p >= TRK_PATTERNS) p = 0;
        t += (unsigned)s->pattern[p].rows * (TRK_MIDI_PPQ / lpb_of(s));
    }
    return t;
}

static int mev_cmp(const void *a, const void *b)
{
    const trk_mev *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->is_cc != y->is_cc ? (x->is_cc ? -1 : 1) : 0;   /* a controller first */
}

int trk_song_events(const trk_song *s, int t, trk_mev **out, int *n)
{
    const trk_track *k;
    trk_mev *ev = NULL;
    size_t cap = 0, cnt = 0;
    int o, r, held = -1;        /* index of the note still sounding */
    unsigned tick = 0;
    const unsigned row = TRK_MIDI_PPQ / lpb_of(s);
    const int sampled = t >= 0 && t < TRK_TRACKS && s->track[t].samples[0];

    *out = NULL;
    *n = 0;
    if (t < 0 || t >= TRK_TRACKS) return -1;
    k = &s->track[t];

#define PUSH(...) do {                                                      \
        if (cnt == cap) {                                                   \
            trk_mev *g;                                                     \
            cap = cap ? cap * 2 : 256;                                      \
            g = realloc(ev, cap * sizeof *ev);                              \
            if (!g) { free(ev); return -1; }                                \
            ev = g;                                                         \
        }                                                                   \
        ev[cnt++] = (trk_mev){ __VA_ARGS__ };                               \
    } while (0)

    for (o = 0; o < s->norder; o++) {
        int p = s->order[o];
        if (p < 0 || p >= TRK_PATTERNS) p = 0;
        for (r = 0; r < s->pattern[p].rows; r++, tick += row) {
            const trk_cell *c = &s->pattern[p].cell[r][t];
            if (!sampled && c->cc != TRK_EMPTY && c->val != TRK_EMPTY)
                PUSH(1, tick, tick, k->channel & 15, c->cc, c->val);
            if (c->note == TRK_EMPTY) continue;
            if (!sampled && held >= 0) { ev[held].end = tick; held = -1; }
            if (c->note <= 127 && trk_track_velocity(k, c->vel == TRK_EMPTY ? 0 : c->vel) > 0) {
                const int vel = trk_track_velocity(k, c->vel == TRK_EMPTY ? 0 : c->vel);
                PUSH(0, tick, sampled ? tick + row : 0, k->channel & 15, c->note, vel);
                if (!sampled) held = (int)cnt - 1;
            }
        }
    }
    if (held >= 0) ev[held].end = tick;
#undef PUSH
    if (cnt) qsort(ev, cnt, sizeof *ev, mev_cmp);
    *out = ev;
    *n = (int)cnt;
    return 0;
}

/* ------------------------------------------------------------- the file */

typedef struct { unsigned char *p; size_t n, cap; int bad; } buf;

static void put(buf *b, const void *d, size_t n)
{
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        unsigned char *g;
        while (c < b->n + n) c *= 2;
        g = realloc(b->p, c);
        if (!g) { b->bad = 1; return; }
        b->p = g; b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(buf *b, int v) { unsigned char c = (unsigned char)v; put(b, &c, 1); }
static void put16(buf *b, int v) { put8(b, v >> 8); put8(b, v); }
static void put32(buf *b, unsigned v) { put16(b, (int)(v >> 16)); put16(b, (int)(v & 0xFFFF)); }
static void putvar(buf *b, unsigned v)
{
    unsigned char t[5];
    int i = 0;
    t[i++] = v & 0x7F;
    while ((v >>= 7)) t[i++] = (unsigned char)(0x80 | (v & 0x7F));
    while (i--) put8(b, t[i]);
}
static void meta(buf *b, unsigned delta, int type, const void *d, size_t n)
{
    putvar(b, delta);
    put8(b, 0xFF); put8(b, type); putvar(b, (unsigned)n);
    if (n) put(b, d, n);
}
static void chunk(buf *f, const buf *trk)
{
    put(f, "MTrk", 4);
    put32(f, (unsigned)trk->n);
    put(f, trk->p, trk->n);
}

typedef struct { unsigned tick; int order; unsigned char b[3]; int len; } sev;

static int sev_cmp(const void *a, const void *b)
{
    const sev *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->order - y->order;
}

int trk_midi_write(const char *path, double bpm, unsigned end, int ntracks,
                   const char *const *names, trk_mev *const *evs, const int *counts,
                   char *err, size_t errn)
{
    buf f = {0}, t = {0};
    int written = 1, i, rc = -1;
    unsigned tempo;
    FILE *fp;
    size_t header_at;

    /* Header: patched with the track count once it is known. */
    put(&f, "MThd", 4); put32(&f, 6); put16(&f, 1); header_at = f.n; put16(&f, 0);
    put16(&f, TRK_MIDI_PPQ);

    tempo = (unsigned)(60000000.0 / (bpm > 0 ? bpm : 120.0) + 0.5);
    meta(&t, 0, 0x03, "tempo", 5);
    { unsigned char d[3] = { (unsigned char)(tempo >> 16), (unsigned char)(tempo >> 8), (unsigned char)tempo };
      meta(&t, 0, 0x51, d, 3); }
    { unsigned char d[4] = { 4, 2, 24, 8 }; meta(&t, 0, 0x58, d, 4); }
    meta(&t, 0, 0x2F, NULL, 0);
    chunk(&f, &t);

    for (i = 0; i < ntracks; i++) {
        const trk_mev *ev = evs[i];
        int n = counts[i], j, ns = 0;
        sev *list;
        unsigned last = 0;
        char nm[TRK_NAME_LEN + 8];

        if (n <= 0) continue;
        list = malloc(sizeof *list * (size_t)n * 2);
        if (!list) { snprintf(err, errn, "out of memory"); goto done; }
        for (j = 0; j < n; j++) {
            if (ev[j].is_cc) {
                list[ns++] = (sev){ ev[j].start, 1, { (unsigned char)(0xB0 | ev[j].chan), (unsigned char)ev[j].a, (unsigned char)ev[j].b }, 3 };
            } else {
                /* An off sorts before an on at the same tick, as the engine sends it. */
                list[ns++] = (sev){ ev[j].start, 2, { (unsigned char)(0x90 | ev[j].chan), (unsigned char)ev[j].a, (unsigned char)ev[j].b }, 3 };
                list[ns++] = (sev){ ev[j].end, 0, { (unsigned char)(0x80 | ev[j].chan), (unsigned char)ev[j].a, 0 }, 3 };
            }
        }
        qsort(list, (size_t)ns, sizeof *list, sev_cmp);
        t.n = 0;
        if (names && names[i] && names[i][0]) snprintf(nm, sizeof nm, "%s", names[i]);
        else snprintf(nm, sizeof nm, "Track %d", i + 1);
        meta(&t, 0, 0x03, nm, strlen(nm));
        for (j = 0; j < ns; j++) {
            putvar(&t, list[j].tick - last);
            last = list[j].tick;
            put(&t, list[j].b, (size_t)list[j].len);
        }
        meta(&t, end > last ? end - last : 0, 0x2F, NULL, 0);
        chunk(&f, &t);
        written++;
        free(list);
    }

    f.p[header_at] = (unsigned char)(written >> 8);
    f.p[header_at + 1] = (unsigned char)written;
    if (f.bad || t.bad) { snprintf(err, errn, "out of memory"); goto done; }
    /* To a file beside it, then renamed over it: a disk that fills halfway
     * leaves the file that was there, not a truncated one. */
    {
        char tmp[1100];
        int fd;
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        unlink(tmp);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        fp = fd >= 0 ? fdopen(fd, "wb") : NULL;
        if (!fp) {
            if (fd >= 0) close(fd);
            snprintf(err, errn, "cannot write %s", path);
            goto done;
        }
        {
            const int wrote = fwrite(f.p, 1, f.n, fp) == f.n;
            const int closed = fclose(fp) == 0;           /* always closed, whatever the write did */
            if (!wrote || !closed || rename(tmp, path) != 0) {
                unlink(tmp);
                snprintf(err, errn, "write to %s failed", path);
                goto done;
            }
        }
    }
    rc = 0;
done:
    free(f.p);
    free(t.p);
    return rc;
}

int trk_song_export_midi(const trk_song *s, const char *path, char *err, size_t errn)
{
    trk_mev *ev[TRK_TRACKS] = { 0 };
    int n[TRK_TRACKS] = { 0 }, i, rc = -1;
    const char *names[TRK_TRACKS];
    for (i = 0; i < TRK_TRACKS; i++) {
        names[i] = s->track[i].name;
        if (s->track[i].mute) continue;
        if (trk_song_events(s, i, &ev[i], &n[i]) < 0) { snprintf(err, errn, "out of memory"); goto done; }
    }
    rc = trk_midi_write(path, s->bpm, trk_song_ticks(s), TRK_TRACKS, names,
                        (trk_mev *const *)ev, n, err, errn);
done:
    for (i = 0; i < TRK_TRACKS; i++) free(ev[i]);
    return rc;
}
