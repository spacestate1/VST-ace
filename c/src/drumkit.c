/* Must precede every include: opendir/strdup are POSIX, and the project
 * compiles with -std=c99, which otherwise hides them. */
#define _POSIX_C_SOURCE 200809L

#include "drumkit.h"
#include "dw_dsp.h"     /* dw_limit() -- the same master limiter as the synths */
#include "wav.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

typedef struct {
    char    name[DK_NAME_MAX];
    float  *pcm;
    size_t  frames;
    double  step;        /* source frames per output frame, from the rate ratio */
} dk_sample;

typedef struct {
    int    active, slot;
    double pos;
    double gain;
} dk_voice;

struct drumkit {
    dk_sample sample[DK_MAX_SAMPLES];
    int       n;
    dk_voice  voice[DK_MAX_VOICES];
    double    sr, gain;
    unsigned  next;
};

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

drumkit *drumkit_load(const char *dir, double samplerate)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **names = NULL;
    int cap = 0, cnt = 0, i;
    drumkit *k;

    if (!d) return NULL;
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
    if (!cnt) { free(names); return NULL; }

    qsort(names, (size_t)cnt, sizeof *names, by_name);

    if (!(k = calloc(1, sizeof *k))) {
        for (i = 0; i < cnt; i++) free(names[i]);
        free(names); return NULL;
    }
    k->sr = samplerate;
    k->gain = 1.0;

    for (i = 0; i < cnt && k->n < DK_MAX_SAMPLES; i++) {
        char path[1024];
        float *pcm = NULL;
        size_t fr = 0;
        int sr = 0;
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (!wav_read_mono(path, &pcm, &fr, &sr) && fr) {
            dk_sample *s = &k->sample[k->n++];
            snprintf(s->name, sizeof s->name, "%s", names[i]);
            { char *dot = strrchr(s->name, '.'); if (dot) *dot = '\0'; }
            s->pcm = pcm;
            s->frames = fr;
            /* Kits are usually 44.1 k and the engine runs at 48 k; without
             * this ratio every hit would play a semitone-and-a-bit sharp. */
            s->step = (double)sr / samplerate;
        } else {
            free(pcm);
        }
    }
    for (i = 0; i < cnt; i++) free(names[i]);
    free(names);

    if (!k->n) { free(k); return NULL; }
    return k;
}

void drumkit_free(drumkit *k)
{
    int i;
    if (!k) return;
    for (i = 0; i < k->n; i++) free(k->sample[i].pcm);
    free(k);
}

int drumkit_count(const drumkit *k) { return k ? k->n : 0; }

const char *drumkit_sample_name(const drumkit *k, int i)
{
    return (k && i >= 0 && i < k->n) ? k->sample[i].name : 0;
}

int drumkit_note_of(const drumkit *k, int i) { (void)k; return DK_BASE_NOTE + i; }

void drumkit_set_gain(drumkit *k, double g) { if (k) k->gain = g; }

void drumkit_note_on(drumkit *k, int note, int velocity)
{
    int slot = note - DK_BASE_NOTE, i, pick = -1;
    if (!k || slot < 0 || slot >= k->n) return;

    /* Retrigger the same pad rather than layering it onto itself -- two copies
     * of one hit an instant apart flams and doubles the level. */
    for (i = 0; i < DK_MAX_VOICES; i++)
        if (k->voice[i].active && k->voice[i].slot == slot) { pick = i; break; }
    if (pick < 0)
        for (i = 0; i < DK_MAX_VOICES; i++)
            if (!k->voice[i].active) { pick = i; break; }
    if (pick < 0) pick = (int)(k->next++ % DK_MAX_VOICES);

    k->voice[pick].active = 1;
    k->voice[pick].slot   = slot;
    k->voice[pick].pos    = 0.0;
    k->voice[pick].gain   = 0.25 + 0.75 * (velocity / 127.0);
}

void drumkit_note_off(drumkit *k, int note)
{
    (void)k; (void)note;   /* one-shots: a hit always runs to its end */
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
        double mix = 0.0;
        for (i = 0; i < DK_MAX_VOICES; i++) {
            dk_voice  *v = &k->voice[i];
            dk_sample *s;
            size_t     i0;
            double     f, a, b;

            if (!v->active) continue;
            s = &k->sample[v->slot];
            i0 = (size_t)v->pos;
            if (i0 + 1 >= s->frames) { v->active = 0; continue; }

            f = v->pos - (double)i0;
            a = s->pcm[i0];
            b = s->pcm[i0 + 1];
            mix += (a + f * (b - a)) * v->gain;

            v->pos += s->step;
        }
        mix *= k->gain;
        out[2 * n]     = dw_limit(mix);
        out[2 * n + 1] = dw_limit(mix);
    }
}
