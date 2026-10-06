/* Must precede every include: opendir/strdup are POSIX, and the project
 * compiles with -std=c99, which otherwise hides them. */
#define _POSIX_C_SOURCE 200809L

#include "drumkit.h"
#include "dw_dsp.h"     /* dw_limit() -- the same master limiter as the synths */
#include "wav.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, strncasecmp */
#include <unistd.h>

/* A pad cut off -- by its choke group, or by being hit again -- fades over
 * this long rather than stopping dead, which clicks. */
#define DK_FADE_S 0.004

typedef struct {
    char    name[DK_NAME_MAX];   /* the file name without .wav, for showing */
    char   *file;                /* the file name as kit.txt names it */
    float  *pcm;                 /* interleaved stereo */
    size_t  frames;
    double  step;        /* source frames per output frame, from the rate ratio */
    int     note, choke;
    double  gain_db, gain;
} dk_sample;

typedef struct {
    int    active, slot;
    int    group;        /* hits in one group cut each other: a pad, or a track */
    double pos;
    double rate;         /* 1: as recorded */
    double gain;
    double fade;         /* 1 while playing; falling to 0 once cut off */
    int    cut;
} dk_voice;

struct drumkit {
    dk_sample sample[DK_MAX_SAMPLES];
    int       n;
    int       slot_of[128];      /* note -> sample, or -1 */
    int       mapped;
    dk_voice  voice[DK_MAX_VOICES];
    double    sr, gain, fade_step;
    unsigned  next;
};

static const char *const k_names[12] = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"
};

/* "C-2", "C#2" (C-4 is 60, as the tracker has it), or a MIDI number. Notes
 * under C-0 name the negative octave outright: "C--1" is MIDI 0. */
static int parse_note(const char *s)
{
    int i;
    if (isdigit((unsigned char)s[0])) {
        char *end;
        long v = strtol(s, &end, 10);
        return *end || v < 0 || v > 127 ? -1 : (int)v;
    }
    for (i = 0; i < 12; i++)
        if (!strncasecmp(s, k_names[i], 2)) {
            char *end;
            long oct, n;
            if (s[2] != '-' && !isdigit((unsigned char)s[2])) return -1;
            oct = strtol(s + 2, &end, 10);
            if (end == s + 2 || *end) return -1;
            n = (oct + 1) * 12 + i;
            return n >= 0 && n <= 127 ? (int)n : -1;
        }
    return -1;
}

static void note_name(int note, char *buf)
{
    snprintf(buf, 5, "%s%d", k_names[note % 12], note / 12 - 1);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* A pad's file: relative to the set's folder, or a path of its own -- what
 * the editor's Add gives for a WAV from somewhere else. */
static void pad_path(const char *dir, const char *file, char *out, size_t n)
{
    if (file[0] == '/') snprintf(out, n, "%s", file);
    else                snprintf(out, n, "%s/%s", dir, file);
}

/* One sample into the next slot. Returns 0 when it loaded. */
static int add_sample(drumkit *k, const char *dir, const dk_pad *p)
{
    char path[1024];
    float *pcm = NULL;
    size_t fr = 0;
    int sr = 0;
    dk_sample *s;
    const char *base;

    if (k->n >= DK_MAX_SAMPLES || p->note < 0 || p->note > 127 || k->slot_of[p->note] >= 0)
        return -1;
    pad_path(dir, p->file, path, sizeof path);
    if (wav_read_stereo(path, &pcm, &fr, &sr) || !fr) { free(pcm); return -1; }

    s = &k->sample[k->n];
    memset(s, 0, sizeof *s);
    if (!(s->file = strdup(p->file))) { free(pcm); return -1; }
    base = strrchr(p->file, '/');
    snprintf(s->name, sizeof s->name, "%s", base ? base + 1 : p->file);
    { char *dot = strrchr(s->name, '.'); if (dot) *dot = '\0'; }
    s->pcm = pcm;
    s->frames = fr;
    /* Kits are usually 44.1 k and the engine runs at 48 k; without this
     * ratio every hit would play a semitone-and-a-bit sharp. */
    s->step = (double)sr / k->sr;
    s->note = p->note;
    s->choke = p->choke;
    s->gain_db = p->gain_db;
    s->gain = pow(10.0, p->gain_db / 20.0);
    k->slot_of[p->note] = k->n++;
    return 0;
}

static void map_add(dk_map *m, int note, const char *file, double gain_db, int choke)
{
    dk_pad *p;
    if (m->n >= DK_MAX_SAMPLES) return;
    p = &m->pad[m->n++];
    p->note = note;
    snprintf(p->file, sizeof p->file, "%s", file);
    p->gain_db = gain_db;
    p->choke = choke;
}

/* Every .wav in the directory, sorted by name, on consecutive notes. */
static void map_dir(dk_map *m, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **names = NULL;
    int cap = 0, cnt = 0, i;

    if (!d) return;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5) continue;
        if (strcasecmp(e->d_name + l - 4, ".wav")) continue;
        if (cnt == cap) {
            char **g = realloc(names, (size_t)(cap ? cap * 2 : 32) * sizeof *g);
            if (!g) break;
            names = g; cap = cap ? cap * 2 : 32;
        }
        names[cnt] = strdup(e->d_name);
        if (!names[cnt]) break;
        cnt++;
    }
    closedir(d);
    if (cnt) qsort(names, (size_t)cnt, sizeof *names, by_name);

    {   /* From C-4, where a tracker's keys start; a set too big for the
         * notes above that starts as low as it must to fit. */
        int fit = cnt < DK_MAX_SAMPLES ? cnt : DK_MAX_SAMPLES;
        int base = fit <= 128 - DK_BASE_NOTE ? DK_BASE_NOTE : 128 - fit;
        for (i = 0; i < cnt; i++) {
            int note = base + m->n;
            if (note <= 127) map_add(m, note, names[i], 0.0, 0);
            free(names[i]);
        }
    }
    free(names);
}

/* As kit.txt says. Returns -1 when there is no kit.txt to read. */
static int map_file(dk_map *m, const char *dir)
{
    char path[1024], line[1024];
    FILE *f;
    int ln = 0, i;

    snprintf(path, sizeof path, "%s/%s", dir, DK_MAP_FILE);
    if (!(f = fopen(path, "r"))) return -1;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *w, *file = NULL;
        size_t l = strlen(line);
        int note = -1, choke = 0, bad = 0, dup = 0;
        double gain_db = 0.0;

        ln++;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;

        /* pad <note> [gain <dB>] [choke <group>] file <rest of line> */
        w = strtok(p, " \t");
        if (!w || strcmp(w, "pad")) bad = 1;
        if (!bad && (!(w = strtok(NULL, " \t")) || (note = parse_note(w)) < 0)) bad = 1;
        while (!bad && (w = strtok(NULL, " \t"))) {
            char *v, *end;
            if (!strcmp(w, "file")) {
                char *e;
                /* The rest of the line, spaces and all: strtok ended "file"
                 * by writing a NUL over the blank after it -- unless it ended
                 * at the end of the line, and there is no name. */
                if (w + strlen(w) >= line + l) { bad = 1; break; }
                file = w + strlen(w) + 1;
                while (*file == ' ' || *file == '\t') file++;
                e = file + strlen(file);
                while (e > file && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
                if (!*file) bad = 1;
                break;
            }
            if (!(v = strtok(NULL, " \t"))) { bad = 1; break; }
            if (!strcmp(w, "gain")) {
                gain_db = strtod(v, &end);
                if (*end && strcasecmp(end, "dB")) bad = 1;
                if (gain_db < -60.0 || gain_db > 24.0) bad = 1;
            } else if (!strcmp(w, "choke")) {
                long c = strtol(v, &end, 10);
                if (*end || c < 0 || c > 99) bad = 1;
                choke = (int)c;
            } else {
                bad = 1;
            }
        }
        if (bad || !file) {
            fprintf(stderr, "%s:%d: not a pad line -- skipped\n", path, ln);
            continue;
        }
        for (i = 0; i < m->n; i++) if (m->pad[i].note == note) dup = 1;
        if (dup) {
            fprintf(stderr, "%s:%d: a second pad on one note -- skipped\n", path, ln);
            continue;
        }
        map_add(m, note, file, gain_db, choke);
    }
    fclose(f);
    return 0;
}

int drumkit_map_read(dk_map *m, const char *dir)
{
    memset(m, 0, sizeof *m);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    /* A kit.txt that names not one pad -- empty, or every line rejected --
     * stands for nothing, so the folder scan runs as if it were not there. */
    if (map_file(m, dir) == 0 && m->n) m->mapped = 1;
    else map_dir(m, dir);
    return m->n;
}

drumkit *drumkit_load_map(const dk_map *m, double samplerate)
{
    drumkit *k;
    int i;

    if (!(k = calloc(1, sizeof *k))) return NULL;
    k->sr = samplerate;
    k->gain = 1.0;
    k->fade_step = 1.0 / (DK_FADE_S * samplerate);
    k->mapped = m->mapped;
    for (i = 0; i < 128; i++) k->slot_of[i] = -1;
    for (i = 0; i < m->n; i++)
        if (add_sample(k, m->dir, &m->pad[i]) && m->mapped)
            fprintf(stderr, "%s/%s: pad %d (%s) did not load -- skipped\n",
                    m->dir, DK_MAP_FILE, i + 1, m->pad[i].file);
    if (!k->n) { free(k); return NULL; }
    return k;
}

drumkit *drumkit_load(const char *dir, double samplerate)
{
    dk_map *m = malloc(sizeof *m);
    drumkit *k = NULL;
    if (!m) return NULL;
    if (drumkit_map_read(m, dir) > 0) k = drumkit_load_map(m, samplerate);
    free(m);
    return k;
}

void drumkit_free(drumkit *k)
{
    int i;
    if (!k) return;
    for (i = 0; i < k->n; i++) { free(k->sample[i].pcm); free(k->sample[i].file); }
    free(k);
}

int drumkit_count(const drumkit *k) { return k ? k->n : 0; }
int drumkit_mapped(const drumkit *k) { return k ? k->mapped : 0; }

const char *drumkit_sample_name(const drumkit *k, int i)
{
    return (k && i >= 0 && i < k->n) ? k->sample[i].name : 0;
}

int drumkit_note_of(const drumkit *k, int i)
{
    return (k && i >= 0 && i < k->n) ? k->sample[i].note : -1;
}

int drumkit_choke_of(const drumkit *k, int i)
{
    return (k && i >= 0 && i < k->n) ? k->sample[i].choke : 0;
}

double drumkit_gain_db_of(const drumkit *k, int i)
{
    return (k && i >= 0 && i < k->n) ? k->sample[i].gain_db : 0.0;
}

static int by_note(const void *a, const void *b)
{
    return ((const dk_pad *)a)->note - ((const dk_pad *)b)->note;
}

/* The map as kit.txt, to an open file. */
static int map_print(const dk_map *m, FILE *f)
{
    char nn[5];
    const char *base = strrchr(m->dir, '/');
    dk_pad sorted[DK_MAX_SAMPLES];
    int i;

    fprintf(f, "# %s: which note plays which sample.\n"
               "#\n"
               "#   pad <note> [gain <dB>] [choke <group>] file <name.wav>\n"
               "#\n"
               "# Notes as the tracker writes them: C-4 is MIDI 60, and under\n"
               "# C-0 the octave goes negative, C--1 to B--1. Pads in one\n"
               "# choke group cut each other off, as a closed hi-hat stops an open one.\n"
               "# A file is named from this folder, or by a path of its own.\n\n",
            base && base[1] ? base + 1 : m->dir);
    /* In note order, which is the order the pads are played in. */
    memcpy(sorted, m->pad, sizeof sorted[0] * (size_t)m->n);
    qsort(sorted, (size_t)m->n, sizeof sorted[0], by_note);
    for (i = 0; i < m->n; i++) {
        const dk_pad *p = &sorted[i];
        note_name(p->note, nn);
        fprintf(f, "pad %s", nn);
        if (p->gain_db != 0.0) fprintf(f, " gain %g", p->gain_db);
        if (p->choke) fprintf(f, " choke %d", p->choke);
        fprintf(f, " file %s\n", p->file);
    }
    return fflush(f) || ferror(f) ? -1 : 0;
}

const char *drumkit_map_check(const dk_map *m)
{
    static char why[160];
    int i, j;
    for (i = 0; i < m->n; i++) {
        char nn[5];
        if (m->pad[i].note < 0 || m->pad[i].note > 127) {
            snprintf(why, sizeof why, "pad %d has no note", i + 1);
            return why;
        }
        if (!m->pad[i].file[0]) {
            snprintf(why, sizeof why, "pad %d has no file", i + 1);
            return why;
        }
        for (j = 0; j < i; j++)
            if (m->pad[j].note == m->pad[i].note) {
                note_name(m->pad[i].note, nn);
                snprintf(why, sizeof why, "two pads on %s", nn);
                return why;
            }
    }
    return NULL;
}

int drumkit_map_save(const dk_map *m)
{
    char path[1024], tmp[1100];
    FILE *f;

    if (drumkit_map_check(m)) { errno = EINVAL; return -1; }
    snprintf(path, sizeof path, "%s/%s", m->dir, DK_MAP_FILE);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return -1;
    if (map_print(m, f)) { fclose(f); unlink(tmp); return -1; }
    if (fclose(f)) { unlink(tmp); return -1; }
    /* Renamed into place: a failed save leaves the old map, not half a new one. */
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

int drumkit_note_parse(const char *s) { return parse_note(s); }
void drumkit_note_name(int note, char *buf) { note_name(note, buf); }

int drumkit_write_map(const drumkit *k, const char *dir)
{
    char path[1024];
    dk_map *m;
    FILE *f;
    int fd, i, rc;

    if (!k) { errno = EINVAL; return -1; }
    if (!(m = calloc(1, sizeof *m))) return -1;
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    for (i = 0; i < k->n; i++)
        map_add(m, k->sample[i].note, k->sample[i].file, k->sample[i].gain_db, k->sample[i].choke);
    snprintf(path, sizeof path, "%s/%s", dir, DK_MAP_FILE);
    /* O_EXCL: a kit.txt already there may be someone's hand-made map. */
    if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) { free(m); return -1; }
    /* The open has made the file, so every failure from here unlinks it: a
     * failed write leaves nothing that would block the next try with EEXIST
     * or load as an empty map. */
    if (!(f = fdopen(fd, "w"))) { close(fd); unlink(path); free(m); return -1; }
    rc = map_print(m, f);
    free(m);
    if (fclose(f)) rc = -1;
    if (rc) { unlink(path); return -1; }
    return 0;
}

void drumkit_set_gain(drumkit *k, double g) { if (k) k->gain = g; }

/* ------------------------------------------------------- finding kits -- */

static int has_wav(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int has = 0;
    if (!d) return 0;
    while (!has && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l >= 5 && !strcasecmp(e->d_name + l - 4, ".wav")) has = 1;
    }
    closedir(d);
    return has;
}

static int by_place(const void *a, const void *b)
{
    return strcasecmp(((const drumkit_place *)a)->name, ((const drumkit_place *)b)->name);
}

static int add_place(drumkit_place *out, int n, int max, const char *path, const char *name)
{
    int i;
    if (n >= max) return n;
    for (i = 0; i < n; i++) if (!strcmp(out[i].path, path)) return n;   /* named twice */
    snprintf(out[n].path, sizeof out[n].path, "%s", path);
    snprintf(out[n].name, sizeof out[n].name, "%s", name);
    return n + 1;
}

/* The kits in one root, appended from out[n]. */
static int find_in(const char *root, drumkit_place *out, int n, int max)
{
    DIR *d;
    struct dirent *e;
    int first = n;
    const char *base = strrchr(root, '/');

    if (has_wav(root)) n = add_place(out, n, max, root, base && base[1] ? base + 1 : root);
    if (!(d = opendir(root))) return n;
    while ((e = readdir(d)) && n < max) {
        char sub[512];
        DIR *d2;
        struct dirent *e2;
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
        if (has_wav(sub)) { n = add_place(out, n, max, sub, e->d_name); continue; }
        if (!(d2 = opendir(sub))) continue;
        while ((e2 = readdir(d2)) && n < max) {
            char s2[512];
            if (e2->d_name[0] == '.') continue;
            snprintf(s2, sizeof s2, "%s/%s", sub, e2->d_name);
            if (has_wav(s2)) {
                /* Named with the pack it is in: two packs that both have a
                 * "Kick" folder are otherwise two identical menu entries. */
                char nm[sizeof out->name];
                snprintf(nm, sizeof nm, "%s / %s", e->d_name, e2->d_name);
                n = add_place(out, n, max, s2, nm);
            }
        }
        closedir(d2);
    }
    closedir(d);
    qsort(out + first, (size_t)(n - first), sizeof *out, by_place);
    return n;
}

int drumkit_find(drumkit_place *out, int max)
{
    static const char *const roots[] = {
        "/storage01/synth_stuff/drums", "/storage01/synth_stuff/furnace", NULL };
    const char *env = getenv("VA_KITS");
    int n = 0, r;

    if (env && *env) {
        char buf[4096], *tok, *save = NULL;
        snprintf(buf, sizeof buf, "%s", env);
        for (tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
            if (*tok) n = find_in(tok, out, n, max);
    }
    for (r = 0; roots[r]; r++) n = find_in(roots[r], out, n, max);
    return n;
}

/* Pads played by note are grouped by pad; drumkit_play's groups are the
 * caller's, from 0 up, and kept clear of these. */
#define DK_PAD_GROUP(slot) (-1 - (slot))

void drumkit_note_on(drumkit *k, int note, int velocity)
{
    int slot;
    if (!k || note < 0 || note > 127 || (slot = k->slot_of[note]) < 0) return;
    drumkit_play(k, slot, 1.0, velocity, DK_PAD_GROUP(slot));
}

void drumkit_play(drumkit *k, int slot, double rate, int velocity, int group)
{
    int i, pick = -1, choke;
    if (!k || slot < 0 || slot >= k->n || !(rate > 0.0)) return;
    choke = k->sample[slot].choke;

    /* The same group hit again, and the pads in this one's choke group, fade
     * out under the new hit: one note at a time per pad or track, so a fast
     * roll neither flams nor doubles in level, and an open hat ends at the
     * closed one. */
    for (i = 0; i < DK_MAX_VOICES; i++) {
        dk_voice *v = &k->voice[i];
        if (!v->active || v->cut) continue;
        if (v->group == group || (choke && k->sample[v->slot].choke == choke)) v->cut = 1;
    }
    for (i = 0; i < DK_MAX_VOICES; i++)
        if (!k->voice[i].active) { pick = i; break; }
    if (pick < 0) pick = (int)(k->next++ % DK_MAX_VOICES);

    k->voice[pick].active = 1;
    k->voice[pick].slot   = slot;
    k->voice[pick].group  = group;
    k->voice[pick].rate   = rate;
    k->voice[pick].pos    = 0.0;
    k->voice[pick].gain   = (0.25 + 0.75 * (velocity / 127.0)) * k->sample[slot].gain;
    k->voice[pick].fade   = 1.0;
    k->voice[pick].cut    = 0;
}

void drumkit_note_off(drumkit *k, int note)
{
    (void)k; (void)note;   /* one-shots: a hit always runs to its end */
}

void drumkit_release(drumkit *k, int group)
{
    int i;
    if (!k) return;
    for (i = 0; i < DK_MAX_VOICES; i++)
        if (k->voice[i].active && k->voice[i].group == group) k->voice[i].cut = 1;
}

int drumkit_slot_at(const drumkit *k, int note)
{
    return k && note >= 0 && note <= 127 ? k->slot_of[note] : -1;
}

int drumkit_find_sample(const drumkit *k, const char *name)
{
    int i;
    if (!k || !name || !*name) return -1;
    for (i = 0; i < k->n; i++) if (!strcmp(k->sample[i].name, name)) return i;
    return -1;
}

void drumkit_fade_all(drumkit *k)
{
    int i;
    if (!k) return;
    for (i = 0; i < DK_MAX_VOICES; i++) if (k->voice[i].active) k->voice[i].cut = 1;
}

void drumkit_all_off(drumkit *k)
{
    int i;
    if (!k) return;
    for (i = 0; i < DK_MAX_VOICES; i++) k->voice[i].active = 0;
}

void drumkit_render(drumkit *k, double *out, int frames)
{
    int n, i;

    if (!k) {
        memset(out, 0, (size_t)frames * 2 * sizeof *out);
        return;
    }

    for (n = 0; n < frames; n++) {
        double l = 0.0, r = 0.0;
        for (i = 0; i < DK_MAX_VOICES; i++) {
            dk_voice  *v = &k->voice[i];
            dk_sample *s;
            size_t     i0;
            double     f, g;
            const float *a;

            if (!v->active) continue;
            s = &k->sample[v->slot];
            i0 = (size_t)v->pos;
            if (i0 + 1 >= s->frames) { v->active = 0; continue; }
            if (v->cut && (v->fade -= k->fade_step) <= 0.0) { v->active = 0; continue; }

            f = v->pos - (double)i0;
            g = v->gain * v->fade;
            a = s->pcm + 2 * i0;
            l += (a[0] + f * (a[2] - a[0])) * g;
            r += (a[1] + f * (a[3] - a[1])) * g;

            v->pos += s->step * v->rate;
        }
        out[2 * n]     = dw_limit(l * k->gain);
        out[2 * n + 1] = dw_limit(r * k->gain);
    }
}
