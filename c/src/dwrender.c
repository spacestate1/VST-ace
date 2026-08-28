/* dwrender -- render FB-7999 factory presets to WAV with a DW-8000 engine.
 *
 * The wavetables and the 69 parameter values per program are the plugin's own,
 * extracted from its resources. The synthesis around them is a
 * reimplementation from the documented DW-8000 architecture -- see the note at
 * the top of dw_synth.h. This renders offline, so it needs no audio device and
 * its output is byte-stable and diffable. */

#include "bank.h"
#include "dw_synth.h"
#include "dw_wavetable.h"
#include "rom.h"
#include "wav.h"
#include "wavedst.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_NOTES 16

/* ---- program lookup ---- */

static void render_one(dw_synth *s, const bank_program *g,
                       const int *notes, int nnotes, int vel,
                       double gate, double total, int sr, const char *path)
{
    size_t  frames = (size_t)(total * sr);
    size_t  gate_f = (size_t)(gate  * sr);
    double *buf;
    int     i;

    if (!(buf = calloc(frames * 2, sizeof *buf))) return;

    dw_synth_set_program(s, g->param);
    dw_synth_all_off(s);
    for (i = 0; i < nnotes; i++) dw_synth_note_on(s, notes[i], vel);

    dw_synth_render(s, buf, (int)gate_f);
    for (i = 0; i < nnotes; i++) dw_synth_note_off(s, notes[i]);
    dw_synth_render(s, buf + gate_f * 2, (int)(frames - gate_f));

    if (!wav_write_stereo16(path, buf, frames, sr)) {
        double peak = 0.0;
        size_t k;
        for (k = 0; k < frames * 2; k++)
            if (buf[k] > peak) peak = buf[k]; else if (-buf[k] > peak) peak = -buf[k];
        printf("  %-20s peak %5.1f dBFS  -> %s\n", g->name,
               peak > 0.0 ? 20.0 * log10(peak) : -99.9, path);
    }
    free(buf);
}

static void usage(void)
{
    fprintf(stderr,
      "dwrender -- render FB-7999 presets through a DW-8000 engine\n\n"
      "usage: dwrender -w <WAVEDST|plugin.dll> [-b <PROGINIT>] [options] <out.wav>\n\n"
      "  -w <path>   wavetable ROM, or a plugin binary to pull it from (required)\n"
      "  -b <path>   preset bank (PROGINIT); without it the engine's defaults are used\n"
      "  -p <sel>    program: index or name substring (default 0)\n"
      "  -l          list the bank's programs and exit\n"
      "  -a <dir>    render every program in the bank into <dir>\n"
      "  -n <note>   MIDI note, repeatable for a chord (default 60)\n"
      "  -v <vel>    velocity 1..127 (default 100)\n"
      "  -g <sec>    how long the key is held (default 1.5)\n"
      "  -t <sec>    total render length (default gate + 2.0)\n"
      "  -r <hz>     samplerate (default 48000)\n");
}

int main(int argc, char **argv)
{
    const char *wpath = NULL, *bpath = NULL, *sel = "0", *outdir = NULL, *out = NULL;
    int    notes[MAX_NOTES], nnotes = 0, vel = 100, sr = 48000, list = 0, i;
    double gate = 1.5, total = -1.0;

    unsigned char *wraw = NULL;
    size_t         wsize = 0;
    wavedst        wd;
    dw_wavetable   wt;
    dw_synth       syn;
    bank           bk;
    int            have_bank = 0, rc = 1;

    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-w") && i + 1 < argc) wpath  = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) bpath  = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) sel    = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-l"))                 list   = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            if (nnotes < MAX_NOTES) notes[nnotes++] = atoi(argv[++i]); else i++;
        }
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) vel   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gate  = atof(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) total = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) sr    = atoi(argv[++i]);
        else if (argv[i][0] != '-' && !out)              out   = argv[i];
        else { usage(); return 2; }
    }

    if (!wpath || (!out && !list && !outdir)) { usage(); return 2; }
    dw_tuning_from_env();
    if (!nnotes) notes[nnotes++] = 60;
    if (total < 0.0) total = gate + 2.0;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;

    if (bpath) {
        unsigned char *braw;
        size_t         bn;
        int            bad;
        if (!(braw = rom_slurp(bpath, &bn))) return 1;
        bad = rom_bank_parse(&bk, braw, bn, bpath);
        free(braw);
        if (bad) return 1;
        have_bank = 1;
    }

    if (list) {
        if (!have_bank) { fprintf(stderr, "-l needs -b\n"); return 2; }
        for (i = 0; i < bk.count; i++) printf("%3d  %s\n", i, bk.prog[i].name);
        bank_free(&bk);
        return 0;
    }

    if (!(wraw = rom_wavedst(wpath, &wsize))) goto done;
    if (wavedst_load(&wd, wraw, wsize, 0)) {
        fprintf(stderr, "%s: could not determine WAVEDST geometry\n", wpath);
        goto done;
    }
    printf("wavetable: %d waves x %d harmonics\n", wd.nwaves, wd.nharm);

    if (dw_wavetable_build(&wt, &wd, (double)sr)) {
        fprintf(stderr, "could not build mip tables\n");
        wavedst_free(&wd);
        goto done;
    }
    printf("mip tables: %d waves x %d octaves, top mip keeps %d harmonics\n\n",
           wt.nwaves, wt.mips, wt.harmonics[wt.mips - 1]);

    if (dw_synth_init(&syn, &wt, (double)sr)) { fprintf(stderr, "synth init failed\n"); goto cleanup; }

    if (outdir) {
        char path[1024];
        if (!have_bank) { fprintf(stderr, "-a needs -b\n"); goto cleanup2; }
        if (mkdir(outdir, 0755) && errno != EEXIST) { perror(outdir); goto cleanup2; }
        for (i = 0; i < bk.count; i++) {
            char safe[BANK_NAME_MAX], *q;
            snprintf(safe, sizeof safe, "%s", bk.prog[i].name);
            for (q = safe; *q; q++) if (*q == '/' || *q == ' ') *q = '_';
            snprintf(path, sizeof path, "%s/%02d_%s.wav", outdir, i, safe);
            render_one(&syn, &bk.prog[i], notes, nnotes, vel, gate, total, sr, path);
        }
        printf("\n%d programs -> %s\n", bk.count, outdir);
    } else {
        bank_program def;
        const bank_program *g;

        if (have_bank) {
            int n = bank_find(&bk, sel);
            if (n < 0) { fprintf(stderr, "no program matching '%s'\n", sel); goto cleanup2; }
            g = &bk.prog[n];
            printf("program %d: %s\n", n, g->name);
        } else {
            memset(&def, 0, sizeof def);
            snprintf(def.name, sizeof def.name, "default");
            def.param[DWP_OSC1_WAVEFORM] = 1;  def.param[DWP_OSC1_LEVEL]   = 31;
            def.param[DWP_OSC2_WAVEFORM] = 1;  def.param[DWP_OSC2_LEVEL]   = 24;
            def.param[DWP_OSC1_OCTAVE]   = 1;  def.param[DWP_OSC2_OCTAVE]  = 1;
            def.param[DWP_OSC2_DETUNE]   = 3;
            def.param[DWP_VCF_CUTOFF]    = 45; def.param[DWP_VCF_RESONANCE] = 8;
            def.param[DWP_VCF_EG_INTENSITY] = 16;
            def.param[DWP_VCF_EG_DECAY]  = 14; def.param[DWP_VCF_EG_BREAKPOINT] = 20;
            def.param[DWP_VCF_EG_SUSTAIN]= 12; def.param[DWP_VCF_EG_RELEASE] = 10;
            def.param[DWP_VCA_EG_DECAY]  = 18; def.param[DWP_VCA_EG_BREAKPOINT] = 24;
            def.param[DWP_VCA_EG_SUSTAIN]= 22; def.param[DWP_VCA_EG_RELEASE] = 10;
            def.param[DWP_VOLUME]        = 0.5;
            g = &def;
            printf("no bank given; using built-in default patch\n");
        }
        render_one(&syn, g, notes, nnotes, vel, gate, total, sr, out);
    }
    rc = 0;

cleanup2:
    dw_synth_free(&syn);
cleanup:
    dw_wavetable_free(&wt);
    wavedst_free(&wd);
done:
    free(wraw);
    if (have_bank) bank_free(&bk);
    return rc;
}
