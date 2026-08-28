/* Render a macOS Audio Unit to a WAV file. The end-to-end check for the Mach-O
 * loader and the framework shims: if this produces the right audio, everything
 * underneath it is working. */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "macau.h"

/* A plugin that never returns from Open or Render gives no clue where it is.
 * Sample the stack on an alarm and report each frame as an image offset. */
static macau *g_watch;
static void on_alarm(int sig)
{
    void *fr[32];
    int n = backtrace(fr, 32), i;
    char b[128];
    (void)sig;
    fprintf(stderr, "\n*** still running after the deadline; stack:\n");
    for (i = 0; i < n; i++) {
        macau_describe(g_watch, fr[i], b, sizeof b);
        fprintf(stderr, "    #%-2d %s\n", i, b);
    }
    fflush(stderr);
    _exit(3);
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int write_wav(const char *path, const float *inter, int frames, int sr)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) { perror(path); return -1; }
    fwrite("RIFF", 1, 4, f); put32(f, (uint32_t)(36 + frames * 4));
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
    put16(f, 1); put16(f, 2); put32(f, (uint32_t)sr);
    put32(f, (uint32_t)(sr * 4)); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (uint32_t)(frames * 4));
    for (i = 0; i < frames * 2; i++) {
        double v = inter[i];
        int16_t s = (int16_t)(v > 1.0 ? 32767 : v < -1.0 ? -32768 : v * 32767.0);
        put16(f, (uint16_t)s);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    /* Unbuffered: a plugin that faults takes the process down before an
     * exit flush, and the lost lines are exactly the ones that say how far
     * it got. */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = NULL, *wav = NULL;
    int i, secs = 2, sr = 44100, bs = 512, want_params = 0;
    int note = 60;
    macau *a;
    float *inl, *inr, *outl, *outr, *inter;
    int total, done = 0;
    double t = 0.0, peak = 0.0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--render") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--note") && i + 1 < argc) note = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) sr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--params")) want_params = 1;
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: macau <Plugin.component> [--render out.wav]\n"
                        "               [--secs N] [--rate HZ] [--params] [--note N]\n");
        return 2;
    }

    signal(SIGALRM, on_alarm);
    alarm(10);

    if (!(a = macau_open(path, (double)sr, bs))) {
        fprintf(stderr, "%s\n", macau_last_error());
        return 1;
    }
    g_watch = a;
    if (macau_configure(a)) {
        fprintf(stderr, "%s\n", macau_last_error());
        macau_close(a);
        return 1;
    }
    printf("%s\n  %s, %d Hz, %d-frame blocks\n", path,
           macau_is_effect(a) ? "effect (pulls input)" : "generator", sr, bs);

    {   int total = macau_param_count(a);
        uint32_t *ids = total > 0 ? calloc((size_t)total, sizeof *ids) : NULL;
        int n = ids ? macau_num_params(a, ids, total) : 0;
        printf("  %d parameter(s)\n", n);
        if (want_params) {
            for (i = 0; i < n; i++) {
                char nm[64];
                float lo, hi, def;
                if (macau_param_info(a, ids[i], nm, sizeof nm, &lo, &hi, &def) == 0)
                    printf("    %-3u %-28s %g .. %g  (default %g, now %g)\n",
                           ids[i], nm, lo, hi, def, macau_get_param(a, ids[i]));
            }
        }
        free(ids);
    }

    total = secs * sr;
    inl  = calloc((size_t)bs, sizeof *inl);
    inr  = calloc((size_t)bs, sizeof *inr);
    outl = calloc((size_t)bs, sizeof *outl);
    outr = calloc((size_t)bs, sizeof *outr);
    inter = calloc((size_t)total * 2, sizeof *inter);
    if (!inl || !inr || !outl || !outr || !inter) return 1;

    /* An instrument only makes sound if something plays it. */
    if (macau_has_midi(a) && !macau_is_effect(a)) {
        macau_midi(a, 0x90, note, 100);
        printf("  note on %d\n", note);
    } else if (!macau_is_effect(a)) {
        printf("  no MusicDevice MIDI entry point -- nothing can play it\n");
    }

    while (done < total) {
        int n = total - done < bs ? total - done : bs, j;
        /* A tone plus a click train: enough spectral content for a spectral
         * effect to act on, and obvious in the output if it passes through. */
        for (j = 0; j < n; j++) {
            double ph = 2.0 * M_PI * 220.0 * (done + j) / sr;
            float v = (float)(0.4 * sin(ph));
            if ((done + j) % (sr / 2) < 64) v += 0.3f;
            inl[j] = inr[j] = v;
        }
        if (macau_render(a, inl, inr, outl, outr, n, &t)) {
            fprintf(stderr, "  %s\n", macau_last_error());
            break;
        }
        for (j = 0; j < n; j++) {
            inter[(done + j) * 2]     = outl[j];
            inter[(done + j) * 2 + 1] = outr[j];
            if (fabs(outl[j]) > peak) peak = fabs(outl[j]);
            if (fabs(outr[j]) > peak) peak = fabs(outr[j]);
        }
        done += n;
    }

    printf("  rendered %d frames, peak %.4f%s\n", done, peak,
           peak < 1e-9 ? "   !! silence" : "");
    if (wav && done && write_wav(wav, inter, done, sr) == 0)
        printf("  wrote %s\n", wav);

    macau_close(a);
    return peak > 1e-9 ? 0 : 1;
}
