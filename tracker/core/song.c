/* The song: what it holds, how it is written down, and how a cell reads. */
#include "trk.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define TRK_PPQ 960                  /* engine.c's queue resolution */

static const char *const k_names[12] = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"
};

void trk_song_init(trk_song *s)
{
    int t, p;

    memset(s, 0, sizeof *s);
    s->bpm = 120.0;
    s->lpb = 4;
    s->volume = 100;
    for (t = 0; t < TRK_TRACKS; t++) {
        snprintf(s->track[t].name, sizeof s->track[t].name, "Track %d", t + 1);
        s->track[t].channel  = 0;
        s->track[t].velocity = 100;
        s->track[t].octave   = 4;
    }
    for (p = 0; p < TRK_PATTERNS; p++) {
        s->pattern[p].rows = 64;
        memset(s->pattern[p].cell, TRK_EMPTY, sizeof s->pattern[p].cell);
    }
    s->order[0] = 0;
    s->norder   = 1;
}

int trk_lpb_ok(int lpb)
{
    return lpb >= 1 && lpb <= 16 && TRK_PPQ % lpb == 0;
}

int trk_pattern_used(const trk_song *s, int p)
{
    int r, t;
    if (p < 0 || p >= TRK_PATTERNS) return 0;
    for (r = 0; r < s->pattern[p].rows; r++)
        for (t = 0; t < TRK_TRACKS; t++) {
            const trk_cell *c = &s->pattern[p].cell[r][t];
            if (c->note != TRK_EMPTY || c->vel != TRK_EMPTY ||
                c->cc != TRK_EMPTY || c->val != TRK_EMPTY) return 1;
        }
    return 0;
}

/* Middle C, MIDI 60, is C-4. Notes below C-0 (MIDI 12) cannot be written
 * with one octave digit and are not entered; a file naming one is refused. */
const char *trk_part_label(const trk_song *s, int p, char *buf)
{
    if (p >= 0 && p < TRK_PATTERNS && s->pattern[p].name[0])
        snprintf(buf, TRK_NAME_LEN + 8, "%s", s->pattern[p].name);
    else
        snprintf(buf, TRK_NAME_LEN + 8, "Part %d", p);
    return buf;
}

const char *trk_note_name(int note, char *buf)
{
    if (note == TRK_NOTE_OFF)          { strcpy(buf, "==="); return buf; }
    if (note < 12 || note > 127)       { strcpy(buf, "..."); return buf; }
    snprintf(buf, 4, "%s%d", k_names[note % 12], note / 12 - 1);
    return buf;
}

static int parse_note(const char *s)
{
    int i;
    if (!strcmp(s, "===")) return TRK_NOTE_OFF;
    if (!strcmp(s, "...")) return TRK_EMPTY;
    if (strlen(s) != 3 || !isdigit((unsigned char)s[2])) return -1;
    for (i = 0; i < 12; i++)
        if (!strncasecmp(s, k_names[i], 2)) {
            int n = (s[2] - '0' + 1) * 12 + i;
            return n <= 127 ? n : -1;
        }
    return -1;
}

static void hex2(int v, char *out)
{
    static const char hx[] = "0123456789ABCDEF";
    if (v == TRK_EMPTY) { out[0] = out[1] = '.'; return; }
    out[0] = hx[(v >> 4) & 15];
    out[1] = hx[v & 15];
}

/* Two hex digits, or ".." for empty. -1 when it is neither, or above 7F. */
static int parse_hex2(const char *s)
{
    char *end;
    long  v;
    if (!strcmp(s, "..")) return TRK_EMPTY;
    if (strlen(s) != 2) return -1;
    v = strtol(s, &end, 16);
    if (*end || v < 0 || v > 127) return -1;
    return (int)v;
}

const char *trk_cell_text(const trk_cell *c, char *buf)
{
    char n[4];
    trk_note_name(c->note, n);
    memcpy(buf, n, 3);
    buf[3] = ' ';
    hex2(c->vel, buf + TRK_COL_VEL);
    buf[6] = ' ';
    hex2(c->cc,  buf + TRK_COL_CC);
    buf[9] = ' ';
    hex2(c->val, buf + TRK_COL_VAL);
    buf[TRK_CELL_CHARS] = 0;
    return buf;
}

/* ------------------------------------------------------------- the file --
 *
 * Plain text, one fact per line, so a song can be read, diffed and fixed by
 * hand. Only cells with something in them are written. Tracks are numbered
 * from 1 as the window shows them; patterns from 0, as trackers do.
 *
 *   tracker 1
 *   bpm 120
 *   lpb 4
 *   volume 80                        master, in percent, when not 100
 *   track 1 channel 1 velocity 100 mute 0
 *   track 1 name Bass
 *   track 1 client pestudio 2
 *   track 1 port pestudio in
 *   track 1 sink this window: FB-7999  an in-process synth, as the shell named it
 *   track 1 octave 3                 the note keys' octave on it, when not 4
 *   track 2 samples drum-singles     a sample set, which the tracker plays
 *   order 0 0 1 2
 *   pattern 0 rows 64
 *   pattern 0 name Verse             parts are named, or shown by number
 *   cell 0 12 1 C-4 64 .. ..        pattern row track note vel cc val
 */

/* A name goes into the file on a line of its own: a line break in it would end
 * the line there and turn the rest into one the loader cannot read -- and the
 * whole song with it. Names come from outside too (an ALSA client's name, a
 * folder's), not only from what is typed. */
static const char *one_line(const char *in, char *out, size_t n)
{
    size_t i;
    for (i = 0; in[i] && i + 1 < n; i++) out[i] = (in[i] == '\n' || in[i] == '\r') ? ' ' : in[i];
    out[i] = 0;
    return out;
}

int trk_song_save(const trk_song *s, const char *path, char *err, size_t errn)
{
    char  tmp[4096];
    FILE *f;
    int   t, p, r, i;
    char  ln[TRK_PATH_LEN + 1];

    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) {
        snprintf(err, errn, "%s: %s", tmp, strerror(errno));
        return -1;
    }
    fprintf(f, "tracker 1\nbpm %.10g\nlpb %d\n", s->bpm, s->lpb);
    if (s->volume != 100) fprintf(f, "volume %d\n", s->volume);
    for (t = 0; t < TRK_TRACKS; t++) {
        const trk_track *k = &s->track[t];
        fprintf(f, "track %d channel %d velocity %d mute %d\n",
                t + 1, k->channel + 1, k->velocity, k->mute ? 1 : 0);
        fprintf(f, "track %d name %s\n", t + 1, one_line(k->name, ln, sizeof ln));
        if (k->octave != 4) fprintf(f, "track %d octave %d\n", t + 1, k->octave);
        if (k->samples[0]) fprintf(f, "track %d samples %s\n", t + 1, one_line(k->samples, ln, sizeof ln));
        if (k->client[0]) fprintf(f, "track %d client %s\n", t + 1, one_line(k->client, ln, sizeof ln));
        if (k->port[0])   fprintf(f, "track %d port %s\n", t + 1, one_line(k->port, ln, sizeof ln));
        if (k->sink[0])   fprintf(f, "track %d sink %s\n", t + 1, one_line(k->sink, ln, sizeof ln));
    }
    fprintf(f, "order");
    for (i = 0; i < s->norder; i++) fprintf(f, " %d", s->order[i]);
    fprintf(f, "\n");
    for (p = 0; p < TRK_PATTERNS; p++) {
        const trk_pattern *pt = &s->pattern[p];
        /* A pattern is written if anything plays it or anything is in it; a
         * blank one at the default length says nothing worth keeping. */
        int listed = 0;
        for (i = 0; i < s->norder; i++) if (s->order[i] == p) listed = 1;
        if (!listed && !trk_pattern_used(s, p) && pt->rows == 64 && !pt->name[0]) continue;
        fprintf(f, "pattern %d rows %d\n", p, pt->rows);
        if (pt->name[0]) fprintf(f, "pattern %d name %s\n", p, one_line(pt->name, ln, sizeof ln));
        for (r = 0; r < pt->rows; r++)
            for (t = 0; t < TRK_TRACKS; t++) {
                const trk_cell *c = &pt->cell[r][t];
                char n[4], v[3] = "..", cc[3] = "..", val[3] = "..";
                if (c->note == TRK_EMPTY && c->vel == TRK_EMPTY &&
                    c->cc == TRK_EMPTY && c->val == TRK_EMPTY) continue;
                trk_note_name(c->note, n);
                hex2(c->vel, v); hex2(c->cc, cc); hex2(c->val, val);
                fprintf(f, "cell %d %d %d %s %s %s %s\n", p, r, t + 1, n, v, cc, val);
            }
    }
    if (fflush(f) || ferror(f)) {
        snprintf(err, errn, "%s: %s", tmp, strerror(errno));
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    /* Renamed into place, so a failed save leaves the old song rather than
     * half of the new one. */
    if (rename(tmp, path)) {
        snprintf(err, errn, "%s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* The rest of a line after `n` words, for the values that may hold spaces --
 * a track name, an ALSA client name. */
static const char *after_words(const char *line, int n)
{
    const char *p = line;
    while (n-- > 0) {
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int trk_song_load(trk_song *out, const char *path, char *err, size_t errn)
{
    /* Parsed into a scratch song and copied over only when the whole file
     * read cleanly: a song half-replaced by a bad file is worse than either. */
    trk_song *s = malloc(sizeof *s);
    char      line[1024];
    FILE     *f;
    int       ln = 0, ok = 0;

    if (!s) { snprintf(err, errn, "out of memory"); return -1; }
    if (!(f = fopen(path, "r"))) {
        snprintf(err, errn, "%s: %s", path, strerror(errno));
        free(s);
        return -1;
    }
    trk_song_init(s);
    s->norder = 0;
    while (fgets(line, sizeof line, f)) {
        char  w[5][32];
        int   n, a, b, c;
        size_t l = strlen(line);
        ln++;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!line[0] || line[0] == '#') continue;
        n = sscanf(line, "%31s %31s %31s %31s %31s", w[0], w[1], w[2], w[3], w[4]);
        if (n < 1) continue;

        if (!strcmp(w[0], "tracker")) {
            if (n < 2 || atoi(w[1]) != 1) goto bad;
            ok = 1;
        } else if (!strcmp(w[0], "bpm") && n >= 2) {
            s->bpm = atof(w[1]);
            if (!(s->bpm >= 20.0 && s->bpm <= 999.0)) goto bad;      /* NaN is neither */
        } else if (!strcmp(w[0], "volume") && n >= 2) {
            s->volume = atoi(w[1]);
            if (s->volume < 0 || s->volume > 150) goto bad;
        } else if (!strcmp(w[0], "lpb") && n >= 2) {
            s->lpb = atoi(w[1]);
            if (!trk_lpb_ok(s->lpb)) goto bad;
        } else if (!strcmp(w[0], "track") && n >= 3) {
            trk_track *k;
            a = atoi(w[1]);
            a = a > INT_MIN ? a - 1 : -1;               /* no overflow on a hostile number */
            if (a < 0 || a >= TRK_TRACKS) goto bad;
            k = &s->track[a];
            if (!strcmp(w[2], "name"))
                snprintf(k->name, sizeof k->name, "%s", after_words(line, 3));
            else if (!strcmp(w[2], "client"))
                snprintf(k->client, sizeof k->client, "%s", after_words(line, 3));
            else if (!strcmp(w[2], "port"))
                snprintf(k->port, sizeof k->port, "%s", after_words(line, 3));
            else if (!strcmp(w[2], "sink"))
                snprintf(k->sink, sizeof k->sink, "%s", after_words(line, 3));
            else if (!strcmp(w[2], "octave")) {
                int o = n >= 4 ? atoi(w[3]) : -1;
                if (n < 4 || o < 0 || o > 9) goto bad;
                k->octave = o;
            } else if (!strcmp(w[2], "samples"))
                snprintf(k->samples, sizeof k->samples, "%s", after_words(line, 3));
            else {
                int ch = 1, vel = 100, mute = 0;
                if (sscanf(line, "track %*d channel %d velocity %d mute %d",
                           &ch, &vel, &mute) != 3) goto bad;
                if (ch < 1 || ch > 16 || vel < 1 || vel > 127) goto bad;
                k->channel = ch - 1; k->velocity = vel; k->mute = mute != 0;
            }
        } else if (!strcmp(w[0], "order")) {
            const char *p = after_words(line, 1);
            while (*p && s->norder < TRK_ORDER_MAX) {
                char *end;
                long  v = strtol(p, &end, 10);
                if (end == p) goto bad;
                if (v < 0 || v >= TRK_PATTERNS) goto bad;
                s->order[s->norder++] = (int)v;
                p = end;
                while (*p == ' ' || *p == '\t') p++;
            }
        } else if (!strcmp(w[0], "pattern") && n >= 3 && !strcmp(w[2], "name")) {
            a = atoi(w[1]);
            if (a < 0 || a >= TRK_PATTERNS) goto bad;
            snprintf(s->pattern[a].name, sizeof s->pattern[a].name, "%s", after_words(line, 3));
        } else if (!strcmp(w[0], "pattern") && n >= 4) {
            a = atoi(w[1]); b = atoi(w[3]);
            if (a < 0 || a >= TRK_PATTERNS || b < 1 || b > TRK_ROWS_MAX) goto bad;
            s->pattern[a].rows = b;
        } else if (!strcmp(w[0], "cell")) {
            char nt[8], v[8], cc[8], val[8];
            trk_cell *cl;
            int note, iv, icc, ival;
            if (sscanf(line, "cell %d %d %d %7s %7s %7s %7s",
                       &a, &b, &c, nt, v, cc, val) != 7) goto bad;
            c -= 1;
            if (a < 0 || a >= TRK_PATTERNS || b < 0 || b >= TRK_ROWS_MAX ||
                c < 0 || c >= TRK_TRACKS) goto bad;
            note = parse_note(nt); iv = parse_hex2(v);
            icc = parse_hex2(cc);  ival = parse_hex2(val);
            if (note < 0 || iv < 0 || icc < 0 || ival < 0) goto bad;
            cl = &s->pattern[a].cell[b][c];
            cl->note = (uint8_t)note; cl->vel = (uint8_t)iv;
            cl->cc = (uint8_t)icc;    cl->val = (uint8_t)ival;
        } else {
            goto bad;
        }
    }
    fclose(f);
    if (!ok) {
        snprintf(err, errn, "%s: not a tracker song", path);
        free(s);
        return -1;
    }
    if (s->norder == 0) { s->order[0] = 0; s->norder = 1; }
    memcpy(out, s, sizeof *s);
    free(s);
    return 0;

bad:
    snprintf(err, errn, "%s:%d: cannot read \"%s\"", path, ln, line);
    fclose(f);
    free(s);
    return -1;
}
