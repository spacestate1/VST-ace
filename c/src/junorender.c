/* junorender -- render the Juno-6 engine's patches to WAV.
 *
 * Same shape as dwrender and fb02render, and the same purpose: an offline,
 * device-free way to check the engine actually produces what it should. */

#include "juno.h"
#include "wav.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_NOTES 16
#define SR 48000

static void usage(void)
{
    fprintf(stderr,
      "junorender -- render Juno-6 patches\n\n"
      "usage: junorender [options] <out.wav>\n\n"
      "  -p <sel>   patch: index or name substring (default 0)\n"
      "  -l         list patches and exit\n"
      "  -a <dir>   render every patch into <dir>\n"
      "  -n <note>  MIDI note, repeatable (default 60)\n"
      "  -v <vel>   velocity 1..127 (default 100)\n"
      "  -g <sec>   key held (default 1.5)\n"
      "  -t <sec>   total length (default gate + 2)\n");
}

static int find_patch(const char *sel)
{
    char *end;
    long n = strtol(sel, &end, 10);
    int i;
    if (*sel && !*end) return (n >= 0 && n < juno_factory_count()) ? (int)n : -1;
    for (i = 0; i < juno_factory_count(); i++) {
        const char *h = juno_factory(i)->name, *p;
        for (p = h; *p; p++) {
            const char *a = p, *q = sel;
            while (*q && *a && ((*a | 32) == (*q | 32))) { a++; q++; }
            if (!*q) return i;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *sel = "0", *outdir = NULL, *out = NULL;
    int notes[MAX_NOTES], nnotes = 0, vel = 100, list = 0, i;
    double gate = 1.5, total = -1.0;
    juno_synth *s;

    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-p") && i + 1 < argc) sel = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-l")) list = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            if (nnotes < MAX_NOTES) notes[nnotes++] = atoi(argv[++i]); else i++;
        }
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) vel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gate = atof(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) total = atof(argv[++i]);
        else if (argv[i][0] != '-' && !out) out = argv[i];
        else { usage(); return 2; }
    }
    if (list) {
        for (i = 0; i < juno_factory_count(); i++)
            printf("%3d  %s\n", i, juno_factory(i)->name);
        return 0;
    }
    if (!out && !outdir) { usage(); return 2; }
    if (!nnotes) notes[nnotes++] = 60;
    if (total < 0.0) total = gate + 2.0;

    if (!(s = juno_create(SR))) return 1;

    {
        size_t frames = (size_t)(total * SR), gate_f = (size_t)(gate * SR), k;
        double *buf = calloc(frames * 2, sizeof *buf);
        int lo = 0, hi = juno_factory_count();

        if (!buf) return 1;
        if (!outdir) {
            int n = find_patch(sel);
            if (n < 0) { fprintf(stderr, "no patch '%s'\n", sel); return 1; }
            lo = n; hi = n + 1;
        } else if (mkdir(outdir, 0755) && errno != EEXIST) { perror(outdir); return 1; }

        for (i = lo; i < hi; i++) {
            const juno_patch *p = juno_factory(i);
            char path[1024], safe[JUNO_NAME_MAX], *q;
            double peak = 0.0;
            int j;

            juno_set_patch(s, p);
            juno_all_off(s);
            memset(buf, 0, frames * 2 * sizeof *buf);

            for (j = 0; j < nnotes; j++) juno_note_on(s, notes[j], vel);
            juno_render(s, buf, (int)gate_f);
            for (j = 0; j < nnotes; j++) juno_note_off(s, notes[j]);
            juno_render(s, buf + gate_f * 2, (int)(frames - gate_f));

            if (outdir) {
                snprintf(safe, sizeof safe, "%s", p->name);
                for (q = safe; *q; q++) if (*q == '/' || *q == ' ') *q = '_';
                snprintf(path, sizeof path, "%s/%02d_%s.wav", outdir, i, safe);
            } else {
                snprintf(path, sizeof path, "%s", out);
            }
            for (k = 0; k < frames * 2; k++)
                if (fabs(buf[k]) > peak) peak = fabs(buf[k]);
            if (!wav_write_stereo16(path, buf, frames, SR))
                printf("  %-14s peak %6.1f dBFS  -> %s\n", p->name,
                       peak > 0 ? 20.0 * log10(peak) : -99.9, path);
        }
        free(buf);
    }
    juno_destroy(s);
    return 0;
}
