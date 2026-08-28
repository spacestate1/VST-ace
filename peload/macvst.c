/* Host a macOS VST2 bundle. See vst2.h for why this needs no ABI layer: macOS
 * x86-64 is System V, the same convention the host itself uses, so a function
 * pointer out of the AEffect is directly callable. */
#define _GNU_SOURCE
#define VST2_SYSV 1
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "machoload.h"
#include "macobjc.h"
#include "macshim.h"
#include "png_out.h"
#include "vst2.h"

/* Guest faults report as an image offset, because a raw pointer says nothing
 * about which plugin code got there. Frames outside the image are resolved
 * through dladdr, so a crash inside one of our own shims names the shim. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    static volatile int nested;
    void *fr[40];
    int n, i;
    char b[160];
    (void)uc;
    /* backtrace() itself can fault when the stack cannot be unwound through
     * plugin code; without this the handler re-enters and buries the report. */
    if (nested++) { fprintf(stderr, "*** fault while reporting; stopping\n"); _exit(139); }
    macho_describe(NULL, si->si_addr, b, sizeof b);
    fprintf(stderr, "\n*** %s, fault addr %s\n",
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
            sig == SIGFPE ? "SIGFPE (a divide by zero)" : "SIGILL", b);
    fflush(stderr);
    n = backtrace(fr, 40);
    for (i = 0; i < n; i++) {
        macho_describe(NULL, fr[i], b, sizeof b);
        fprintf(stderr, "    #%-2d %s\n", i, b);
    }
    fflush(stderr);
    _exit(139);
}

/* Same deadline reporter the AU host uses: a plugin that never returns gives no
 * other clue where it is. */
static void on_alarm(int sig)
{
    void *fr[40];
    int n = backtrace(fr, 40), i;
    char b[160];
    (void)sig;
    fprintf(stderr, "\n*** still running after the deadline; stack:\n");
    for (i = 0; i < n; i++) {
        macho_describe(NULL, fr[i], b, sizeof b);
        fprintf(stderr, "    #%-2d %s\n", i, b);
    }
    fflush(stderr);
    _exit(3);
}

static macho *g_img;
static double g_sr = 44100.0;
static int    g_bs = 512;

static intptr_t host_cb(AEffect *fx, int32_t op, int32_t idx, intptr_t val,
                        void *ptr, float opt)
{
    (void)fx; (void)idx; (void)val; (void)opt;
    switch (op) {
    case 1:  return 2400;                       /* audioMasterVersion       */
    case 16: return (intptr_t)g_sr;             /* GetSampleRate            */
    case 17: return g_bs;                       /* GetBlockSize             */
    case 23: return 1;                          /* GetCurrentProcessLevel   */
    case 33:                                    /* GetVendorString          */
    case 34:                                    /* GetProductString         */
        if (ptr) snprintf(ptr, 64, "peload");
        return 1;
    case 41:
        /* audioMasterGetDirectory: the plugin's own folder. Without it a plugin
         * falls back to a path relative to the working directory. */
        return (intptr_t)macho_bundle_path(g_img);
    case 32: return 1;                          /* GetAutomationState       */
    case 13: return 1;                          /* SizeWindow               */
    default: return 0;
    }
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

static void show_missed(const char *cls, const char *sel, unsigned long n, void *ud)
{
    int *i = ud;
    if ((*i)++ < 40) printf("    %-28s -%-44s %lu\n", cls ? cls : "?", sel ? sel : "?", n);
}

int main(int argc, char **argv)
{
    /* Unbuffered: a plugin that faults takes the process down before an
     * exit flush, and the lost lines are exactly the ones that say how far
     * it got. */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = NULL, *wav = NULL;
    int i, secs = 2, note = 60, dump = 0, editor = 0;
    const char *dump_png = NULL;
    macho *img;
    AEffect *(*entry)(intptr_t (*)(AEffect *, int32_t, int32_t, intptr_t, void *, float));
    AEffect *fx;
    float **ins, **outs, *inter;
    int nin, nout, nch, total, done = 0;
    double peak = 0.0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--render") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--note") && i + 1 < argc) note = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--params")) dump = 1;
        else if (!strcmp(argv[i], "--editor")) editor = 1;
        else if (!strcmp(argv[i], "--dump-editor") && i + 1 < argc)
        { editor = 1; dump_png = argv[++i]; }
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: macvst <Plugin.vst> [--render out.wav] [--secs N]\n"
                        "               [--note N] [--params] [--editor]\n"
                        "               [--dump-editor out.png]\n");
        return 2;
    }

    signal(SIGALRM, on_alarm);
    alarm(15);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
    }

    if (!(img = macho_open(path))) { fprintf(stderr, "%s\n", macho_last_error()); return 1; }
    g_img = img;
    macho_run_init(img);

    entry = (AEffect *(*)(intptr_t (*)(AEffect *, int32_t, int32_t, intptr_t, void *, float)))
            macho_symbol(img, "VSTPluginMain");
    if (!entry) entry = (AEffect *(*)(intptr_t (*)(AEffect *, int32_t, int32_t, intptr_t, void *, float)))
                        macho_symbol(img, "main_macho");
    if (!entry) { fprintf(stderr, "no VSTPluginMain export\n"); return 1; }

    fx = entry(host_cb);
    if (!fx || fx->magic != 0x56737450) {
        fprintf(stderr, "VSTPluginMain gave no valid AEffect (%p)\n", (void *)fx);
        return 1;
    }

    fx->dispatcher(fx, effOpen, 0, 0, NULL, 0.0f);
    fx->dispatcher(fx, effSetSampleRate, 0, 0, NULL, (float)g_sr);
    fx->dispatcher(fx, effSetBlockSize, 0, g_bs, NULL, 0.0f);
    fx->dispatcher(fx, effMainsChanged, 0, 1, NULL, 0.0f);

    { char nm[80] = { 0 }, vn[80] = { 0 };
      fx->dispatcher(fx, effGetEffectName, 0, 0, nm, 0.0f);
      fx->dispatcher(fx, effGetVendorString, 0, 0, vn, 0.0f);
      printf("%s\n  %s -- %s\n", path, nm, vn); }
    printf("  uniqueID 0x%08x  %s  in %d  out %d  programs %d  params %d\n",
           fx->uniqueID, (fx->flags & 0x100) ? "synth" : "effect",
           fx->numInputs, fx->numOutputs, fx->numPrograms, fx->numParams);

    if (dump) {
        for (i = 0; i < fx->numParams && i < 400; i++) {
            char n2[64] = { 0 }, d[64] = { 0 };
            fx->dispatcher(fx, effGetParamName, i, 0, n2, 0.0f);
            fx->dispatcher(fx, effGetParamDisplay, i, 0, d, 0.0f);
            printf("    %3d %-24s %s\n", i, n2, d);
        }
    }

    nin = fx->numInputs < 0 ? 0 : fx->numInputs;
    nout = fx->numOutputs < 1 ? 1 : fx->numOutputs;
    if (nin > 256 || nout > 256) { fprintf(stderr, "implausible channel count\n"); return 1; }
    nch = nin > nout ? nin : nout;
    ins = calloc((size_t)nch + 1, sizeof *ins);
    outs = calloc((size_t)nch + 1, sizeof *outs);
    for (i = 0; i < nin; i++)  ins[i]  = calloc((size_t)g_bs, sizeof **ins);
    for (i = 0; i < nout; i++) outs[i] = calloc((size_t)g_bs, sizeof **outs);
    total = secs * (int)g_sr;
    inter = calloc((size_t)total * 2, sizeof *inter);
    if (!ins || !outs || !inter) return 1;

    {   /* one note on, released two thirds through */
        struct { VstEvents ev; VstMidiEvent m; } pkt;
        memset(&pkt, 0, sizeof pkt);
        pkt.ev.numEvents = 1;
        pkt.ev.events[0] = &pkt.m;
        pkt.m.type = 1; pkt.m.byteSize = 24;
        pkt.m.midiData[0] = (char)0x90;
        pkt.m.midiData[1] = (char)note;
        pkt.m.midiData[2] = 100;
        fx->dispatcher(fx, effProcessEvents, 0, 0, &pkt.ev, 0.0f);

        while (done < total) {
            int n = total - done < g_bs ? total - done : g_bs, j;
            if (done <= total * 2 / 3 && done + n > total * 2 / 3) {
                pkt.m.midiData[0] = (char)0x80;
                pkt.m.midiData[2] = 0;
                fx->dispatcher(fx, effProcessEvents, 0, 0, &pkt.ev, 0.0f);
            }
            for (j = 0; j < nin; j++) {
                int k;
                for (k = 0; k < n; k++)
                    ins[j][k] = ((done + k) % 12000 == 0) ? 0.5f : 0.0f;
            }
            for (j = 0; j < nout; j++) memset(outs[j], 0, (size_t)g_bs * sizeof **outs);
            fx->processReplacing(fx, nin ? ins : NULL, outs, n);
            for (j = 0; j < n; j++) {
                inter[(done + j) * 2]     = outs[0][j];
                inter[(done + j) * 2 + 1] = nout >= 2 ? outs[1][j] : outs[0][j];
            }
            done += n;
        }
    }
    for (i = 0; i < total * 2; i++) if (fabs(inter[i]) > peak) peak = fabs(inter[i]);
    printf("  rendered %d frames, peak %.4f%s\n", total, peak,
           peak < 1e-9 ? "   !! silence" : "");
    if (wav && write_wav(wav, inter, total, (int)g_sr) == 0) printf("  wrote %s\n", wav);

    if (editor) {
        /* Ask for the editor. On macOS the plugin builds an NSView, so this is
         * where the Objective-C surface gets exercised -- the point is the list
         * of selectors it sends, not a window. */
        struct { int16_t top, left, bottom, right; } *r = NULL;
        int ew = 0, eh = 0;
        printf("  editor: flags say %s\n", (fx->flags & 1) ? "yes" : "no");
        fx->dispatcher(fx, effEditGetRect, 0, 0, &r, 0.0f);
        if (r) {
            ew = r->right - r->left; eh = r->bottom - r->top;
            printf("  editor rect: %dx%d\n", ew, eh);
        }
        fx->dispatcher(fx, effEditOpen, 0, 0, NULL, 0.0f);
        /* The plugin sizes its own layer once it knows the scale, but seed it
         * from the rect so a first frame has somewhere to go. */
        if (ew > 0 && eh > 0) macmetal_set_size(ew, eh);
        /* The first paint draws everything; after that an iPlug2 editor only
         * redraws what changed, so the two are worth reporting apart. */
        { unsigned long t1, s1; double m1;
          fx->dispatcher(fx, effEditIdle, 0, 0, NULL, 0.0f);
          macns_fire_timers();
          macns_draw_dirty();
          macmetal_stats(&t1, &s1, &m1);
          printf("  first paint: %lu triangles, %lu pixels shaded, %.1f ms\n",
                 t1, s1, m1); }
        for (i = 1; i < 20; i++) {
            fx->dispatcher(fx, effEditIdle, 0, 0, NULL, 0.0f);
            macns_fire_timers();
            macns_draw_dirty();
        }
        { const unsigned int *px; int pw, ph;
          /* Metal first, then the Core Graphics backing store an older editor
           * draws into. */
          if (macmetal_pixels(&px, &pw, &ph) ||
              macquartz_editor_pixels(&px, &pw, &ph)) {
              unsigned long j, nonzero = 0;
              for (j = 0; j < (unsigned long)pw * ph; j++)
                  if ((px[j] & 0x00ffffff) != 0) nonzero++;
              printf("  editor framebuffer %dx%d, %lu/%lu pixels painted\n",
                     pw, ph, nonzero, (unsigned long)pw * ph);
              { unsigned long tris, shaded; double rms;
                macmetal_stats(&tris, &shaded, &rms);
                printf("  next %d frames: %lu triangles, %lu pixels shaded, "
                       "%.1f ms (%.2f ms/frame)\n",
                       i - 1, tris, shaded, rms, rms / (i > 1 ? i - 1 : 1)); }
              if (dump_png && png_write_bgrx(dump_png, px, pw, ph) == 0)
                  printf("  wrote %s\n", dump_png);
          } else {
              printf("  editor produced no framebuffer\n");
          } }
        fx->dispatcher(fx, effEditClose, 0, 0, NULL, 0.0f);
    }

    { int n = macobjc_missed_count(), shown = 0;
      if (n) {
          printf("  %d selector(s) sent with no implementation:\n", n);
          macobjc_each_missed(show_missed, &shown);
          if (n > 40) printf("    ... %d more\n", n - 40);
      } }

    fx->dispatcher(fx, effMainsChanged, 0, 0, NULL, 0.0f);
    fx->dispatcher(fx, effClose, 0, 0, NULL, 0.0f);
    return peak > 1e-9 ? 0 : 1;
}
