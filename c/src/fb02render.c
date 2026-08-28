/* fb02render -- render FB-02 factory presets through the 4-operator FM engine.
 *
 * The counterpart to dwrender: offline, no audio device, byte-stable output.
 * Parameter values are the plugin's own; the FM synthesis around them is a
 * reimplementation -- see the note at the top of fm_synth.h. */

#include "bank.h"
#include "fb02.h"
#include "fm_synth.h"
#include "rom.h"
#include "wav.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_NOTES 16
#define SR_DEFAULT 48000

/* The bank decoder leaves FB-02 bodies opaque, so walk the chunk again to get
 * the raw body for a given program. */
static const unsigned char *body_of(const unsigned char *raw, const bank *b, int idx)
{
    size_t off = 160 + 8;
    int i;
    for (i = 0; i < idx; i++) {
        uint32_t nl = (uint32_t)raw[off] | ((uint32_t)raw[off+1] << 8) |
                      ((uint32_t)raw[off+2] << 16) | ((uint32_t)raw[off+3] << 24);
        off += 4 + nl + (size_t)b->body_bytes;
    }
    {
        uint32_t nl = (uint32_t)raw[off] | ((uint32_t)raw[off+1] << 8) |
                      ((uint32_t)raw[off+2] << 16) | ((uint32_t)raw[off+3] << 24);
        return raw + off + 4 + nl;
    }
}

static int find_program(const bank *b, const char *sel)
{
    char *end;
    long  n = strtol(sel, &end, 10);
    int   i;
    if (*sel && !*end) return (n >= 0 && n < b->count) ? (int)n : -1;
    for (i = 0; i < b->count; i++) {
        const char *h = b->prog[i].name, *p;
        for (p = h; *p; p++) {
            const char *a = p, *q = sel;
            while (*q && *a && ((*a | 32) == (*q | 32))) { a++; q++; }
            if (!*q) return i;
        }
    }
    return -1;
}

static void usage(void)
{
    fprintf(stderr,
      "fb02render -- render FB-02 presets through a 4-operator FM engine\n\n"
      "usage: fb02render -b <PROGINIT> [options] <out.wav>\n\n"
      "  -b <path>   preset bank (required)\n"
      "  -p <sel>    program: index or name substring (default 0)\n"
      "  -l          list the bank's programs and exit\n"
      "  -a <dir>    render every program into <dir>\n"
      "  -n <note>   MIDI note, repeatable (default 60)\n"
      "  -v <vel>    velocity 1..127 (default 100)\n"
      "  -g <sec>    key held (default 1.5)\n"
      "  -t <sec>    total length (default gate + 2)\n"
      "  -i          print the decoded voice and exit\n");
}

int main(int argc, char **argv)
{
    const char *bpath = NULL, *sel = "0", *outdir = NULL, *out = NULL;
    int notes[MAX_NOTES], nnotes = 0, vel = 100, list = 0, info = 0, i;
    double gate = 1.5, total = -1.0;
    unsigned char *raw = NULL;
    size_t rawn = 0;
    bank bk;
    fm_synth syn;

    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-b") && i + 1 < argc) bpath = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) sel = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-l")) list = 1;
        else if (!strcmp(argv[i], "-i")) info = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            if (nnotes < MAX_NOTES) notes[nnotes++] = atoi(argv[++i]); else i++;
        }
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) vel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gate = atof(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) total = atof(argv[++i]);
        else if (argv[i][0] != '-' && !out) out = argv[i];
        else { usage(); return 2; }
    }
    if (!bpath || (!out && !list && !outdir && !info)) { usage(); return 2; }
    if (!nnotes) notes[nnotes++] = 60;
    if (total < 0.0) total = gate + 2.0;

    if (!(raw = rom_slurp(bpath, &rawn))) return 1;
    if (bank_parse(&bk, raw, rawn)) { free(raw); return 1; }

    if (list) {
        for (i = 0; i < bk.count; i++) printf("%3d  %s\n", i, bk.prog[i].name);
        bank_free(&bk); free(raw); return 0;
    }

    fm_synth_init(&syn, SR_DEFAULT);

    if (info) {
        int n = find_program(&bk, sel), k;
        fb02_program p;
        if (n < 0) { fprintf(stderr, "no program '%s'\n", sel); return 1; }
        fb02_decode(&p, body_of(raw, &bk, n), bk.body_bytes);
        printf("program %d: %s\n", n, bk.prog[n].name);
        printf("  algorithm %d  feedback %d  transpose %d  carriers 0x%x\n",
               p.algorithm, p.feedback, p.transpose, fm_algorithm_carriers(p.algorithm));
        for (k = 0; k < FB02_OPS; k++)
            printf("  OP%d en=%d lvl=%3d frq=%2d det=%d inh=%d "
                   "atk=%2d d1=%2d d2=%2d sus=%2d rel=%2d\n",
                   k + 1, p.op[k].enable, p.op[k].level, p.op[k].frequency,
                   p.op[k].detune, p.op[k].inharmonic, p.op[k].attack,
                   p.op[k].decay1, p.op[k].decay2, p.op[k].sustain, p.op[k].release);
        bank_free(&bk); free(raw); return 0;
    }

    {
        size_t frames = (size_t)(total * SR_DEFAULT);
        size_t gate_f = (size_t)(gate * SR_DEFAULT);
        double *buf = calloc(frames * 2, sizeof *buf);
        int lo = 0, hi = bk.count;

        if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }
        if (!outdir) {
            int n = find_program(&bk, sel);
            if (n < 0) { fprintf(stderr, "no program '%s'\n", sel); return 1; }
            lo = n; hi = n + 1;
        } else if (mkdir(outdir, 0755) && errno != EEXIST) { perror(outdir); return 1; }

        for (i = lo; i < hi; i++) {
            fb02_program p;
            char path[1024], safe[BANK_NAME_MAX], *q;
            double peak = 0.0;
            size_t k;

            fb02_decode(&p, body_of(raw, &bk, i), bk.body_bytes);
            fm_synth_set_program(&syn, &p);
            fm_synth_all_off(&syn);
            memset(buf, 0, frames * 2 * sizeof *buf);

            { int j; for (j = 0; j < nnotes; j++) fm_synth_note_on(&syn, notes[j], vel); }
            fm_synth_render(&syn, buf, (int)gate_f);
            { int j; for (j = 0; j < nnotes; j++) fm_synth_note_off(&syn, notes[j]); }
            fm_synth_render(&syn, buf + gate_f * 2, (int)(frames - gate_f));

            if (outdir) {
                snprintf(safe, sizeof safe, "%s", bk.prog[i].name);
                for (q = safe; *q; q++) if (*q == '/' || *q == ' ') *q = '_';
                snprintf(path, sizeof path, "%s/%03d_%s.wav", outdir, i, safe);
            } else {
                snprintf(path, sizeof path, "%s", out);
            }
            for (k = 0; k < frames * 2; k++)
                if (fabs(buf[k]) > peak) peak = fabs(buf[k]);
            if (!wav_write_stereo16(path, buf, frames, SR_DEFAULT))
                printf("  %-12s alg %d  peak %6.1f dBFS  -> %s\n", bk.prog[i].name,
                       p.algorithm, peak > 0 ? 20.0 * log10(peak) : -99.9, path);
        }
        free(buf);
    }

    bank_free(&bk);
    free(raw);
    return 0;
}
