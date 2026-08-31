/* peload -- CLI front end: inspect a Windows VST2 plugin and render it to WAV,
 * natively, without Wine. See pehost.h for the hosting API.
 *
 * Everything below `main` is one stage of a run, in the order a run does them.
 * They were all inline in `main` until it reached three hundred lines and you
 * could no longer see that a run is: parse, maybe detect, open, describe,
 * patch, dump, render, capture. */
#define _GNU_SOURCE
#include "pehost.h"
#include "patch.h"
#include "win32host.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define RATE 48000

/* Everything the command line can say. */
/* Win32 mouse messages, the form pehost_editor_mouse takes. Only the three a
 * click is made of are needed here. */
#define WM_MOUSEMOVE   0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP   0x0202
#define MAX_CLICKS     8

typedef struct {
    const char *path;             /* the plugin, or a bank that names one */
    const char *wav;              /* --render */
    const char *shot;             /* --editor */
    const char *patch_in;         /* --patch, or a .json given positionally */
    const char *patch_out;        /* --save-patch */
    const char *pick;             /* --pick */
    const char *as_name;          /* --as */
    int         detect;           /* --detect */
    int         dump;             /* --params */
    int         list_patches;
    int         list_programs;
    int         secs, note, prog;
    /* The host runs at 256; matching it here is what reproduces a plugin that
     * only misbehaves under the block size it will actually be given. */
    int         block;
    /* --click X,Y, repeatable. See the click loop in the editor capture. */
    struct { int x, y; } click[MAX_CLICKS];
    int         nclick;
} opts;

/* ------------------------------------------------------------- file output */

/* Dump the editor's pixels as a PPM: enough to confirm the Win32 layer really
 * produced an image, without needing a window system. */
static int write_ppm(const char *path, const unsigned int *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int y, x;
    if (!f) { perror(path); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned int p = px[(size_t)y * w + x];
            unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8),
                                     (unsigned char)p };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return 1;
}

static int write_wav(const char *path, const float *inter, int frames, int sr)
{
    unsigned char h[44];
    uint32_t bytes = (uint32_t)frames * 4u;
    int16_t *pcm;
    FILE *f;
    int i;

    if (!(pcm = malloc((size_t)frames * 2 * sizeof *pcm))) return 0;
    for (i = 0; i < frames * 2; i++) {
        double v = (double)inter[i] * 32767.0;
        pcm[i] = (int16_t)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
    }
    memcpy(h, "RIFF", 4);
    { uint32_t v = 36 + bytes; memcpy(h + 4, &v, 4); }
    memcpy(h + 8, "WAVEfmt ", 8);
    { uint32_t v = 16;     memcpy(h + 16, &v, 4); }
    { uint16_t v = 1;      memcpy(h + 20, &v, 2); }
    { uint16_t v = 2;      memcpy(h + 22, &v, 2); }
    { uint32_t v = sr;     memcpy(h + 24, &v, 4); }
    { uint32_t v = sr * 4; memcpy(h + 28, &v, 4); }
    { uint16_t v = 4;      memcpy(h + 32, &v, 2); }
    { uint16_t v = 16;     memcpy(h + 34, &v, 2); }
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    if (!(f = fopen(path, "wb"))) { perror(path); free(pcm); return 0; }
    fwrite(h, 1, sizeof h, f);
    fwrite(pcm, 2, (size_t)frames * 2, f);
    fclose(f); free(pcm);
    return 1;
}

/* ------------------------------------------------------------ the command */

static void usage(void)
{
    fprintf(stderr,
        "usage: peload <plugin.dll|bank.json> [--params] [--render out.wav]\n"
        "              [--secs N] [--note N] [--program N]\n"
        "              [--patch bank.json]  apply a patch before rendering\n"
        "              [--pick name|N]      which patch, when the file holds several\n"
        "              [--list-patches]     name the patches in it\n"
        "              [--list-programs]    name the plugin's own programs\n"
        "              [--detect]           say what platform/loader the file needs, then stop\n"
        "              [--as KIND]          force a loader instead of auto-detecting\n"
        "                                   (auto, win-vst2-64, win-vst2-32, win-vst3,\n"
        "                                    linux-vst3, linux-vst2, mac-vst2, mac-vst3,\n"
        "                                    mac-au, classic-mac)\n"
        "              [--save-patch out.json]  write the current state\n"
        "              [--editor out.ppm]   open the GUI and capture it\n"
       "              [--click X,Y]        click there first (repeatable)\n"
        "              [--block N]          frames per block (default 512)\n"
        "\n"
        "A bank names the plugin it was written for, so `peload bank.json`\n"
        "opens that plugin with the patch already applied.\n");
}

/* Fills `o`. Returns 0, or an exit status if the command line is unusable. */
static int parse_args(int argc, char **argv, opts *o)
{
    int i;

    memset(o, 0, sizeof *o);
    o->secs = 3;
    o->note = 60;
    o->block = 512;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--render") && i + 1 < argc)        o->wav   = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc)     o->secs  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--note") && i + 1 < argc)     o->note  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--program") && i + 1 < argc)  o->prog  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--params"))                   o->dump  = 1;
        else if (!strcmp(argv[i], "--editor") && i + 1 < argc)   o->shot  = argv[++i];
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            int cx, cy;
            if (sscanf(argv[++i], "%d,%d", &cx, &cy) != 2) {
                fprintf(stderr, "--click wants X,Y\n"); return 2;
            }
            if (o->nclick < MAX_CLICKS) {
                o->click[o->nclick].x = cx; o->click[o->nclick].y = cy; o->nclick++;
            }
        }
        else if (!strcmp(argv[i], "--block") && i + 1 < argc)    o->block = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--patch") && i + 1 < argc)    o->patch_in  = argv[++i];
        else if (!strcmp(argv[i], "--save-patch") && i + 1 < argc)
                                                                 o->patch_out = argv[++i];
        else if (!strcmp(argv[i], "--pick") && i + 1 < argc)     o->pick = argv[++i];
        else if (!strcmp(argv[i], "--list-patches"))             o->list_patches  = 1;
        else if (!strcmp(argv[i], "--list-programs"))            o->list_programs = 1;
        else if (!strcmp(argv[i], "--detect"))                   o->detect = 1;
        else if (!strcmp(argv[i], "--as") && i + 1 < argc)       o->as_name = argv[++i];
        else if (argv[i][0] != '-')                              o->path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    /* The positional argument may be the bank rather than the plugin. A bank
     * records the plugin it was written for, so naming one is enough to open
     * both -- which is the whole point of "pluginPath". */
    if (o->path && !o->patch_in) {
        const char *dot = strrchr(o->path, '.');
        if (dot && !strcasecmp(dot, ".json")) {
            o->patch_in = o->path;
            o->path     = NULL;
        }
    }
    if (o->block < 16 || o->block > 8192) o->block = 512;
    return 0;
}

/* Report what the file is and which loader it needs, then stop. A downloader's
 * "what did I just get" query, and a way to see why a plugin is being refused
 * before trying to render it. */
static int cmd_detect(const char *path)
{
    pehost_info info;

    pehost_classify(path, &info);
    printf("%s\n", path);
    printf("  kind      %s (%s)\n", pehost_kind_name(info.kind), info.label);
    printf("  os        %s\n", info.os[0]     ? info.os     : "-");
    printf("  arch      %s\n", info.arch[0]   ? info.arch   : "-");
    printf("  format    %s\n", info.format[0] ? info.format : "-");
    if (strcmp(info.binary, path))
        printf("  binary    %s\n", info.binary);
    printf("  loadable  %s\n", info.loadable ? "yes" : "no");
    if (!info.loadable && info.why[0])
        printf("  reason    %s\n", info.why);
    return info.loadable ? 0 : 1;
}

/* Read the bank named by --patch, honour --list-patches and --pick, and let it
 * supply the plugin path when none was given. Returns 0, or an exit status.
 * `*out` is left NULL when there is no bank to read. */
static int open_bank(opts *o, patch_bank **out, int *patch_ix)
{
    patch_bank *bank;
    char        err[256];
    int         i;

    *out = NULL;
    *patch_ix = 0;
    if (!o->patch_in) return 0;

    if (!(bank = patch_bank_read(o->patch_in, err, sizeof err))) {
        fprintf(stderr, "patch: %s\n", err);
        return 1;
    }
    *out = bank;

    if (o->list_patches) {
        printf("%d patch(es) in %s", patch_bank_count(bank), o->patch_in);
        if (*patch_bank_plugin_name(bank))
            printf("  [%s]", patch_bank_plugin_name(bank));
        printf("\n");
        for (i = 0; i < patch_bank_count(bank); i++)
            printf("  %2d  %s\n", i, patch_bank_patch_name(bank, i));
    }

    /* --pick takes a name or an index, because a bank is read by people and
     * driven by scripts, and those want different handles on the same thing.
     * Resolved before the plugin below: in a bank spanning machines each patch
     * names its own, so the pick decides which one to open. */
    if (o->pick) {
        int ix = -1;
        for (i = 0; i < patch_bank_count(bank); i++)
            if (!strcasecmp(o->pick, patch_bank_patch_name(bank, i))) { ix = i; break; }
        if (ix < 0 && isdigit((unsigned char)o->pick[0])) {
            ix = atoi(o->pick);
            if (ix >= patch_bank_count(bank)) ix = -1;
        }
        if (ix < 0) {
            fprintf(stderr, "no patch called \"%s\" in %s "
                            "(--list-patches to see them)\n", o->pick, o->patch_in);
            return 2;
        }
        *patch_ix = ix;
    }

    if (!o->path) {
        o->path = patch_bank_patch_plugin_path(bank, *patch_ix);
        if (!*o->path) {
            fprintf(stderr, "%s names no \"pluginPath\" -- give the plugin "
                            "as well\n", o->patch_in);
            return 2;
        }
    }
    return 0;
}

static void describe(pehost *h, const opts *o)
{
    int i;

    printf("\n%s -- %s\n", pehost_name(h), pehost_vendor(h));
    printf("  uniqueID 0x%08x  %s  in %d  out %d  programs %d  params %d\n",
           pehost_unique_id(h), pehost_is_synth(h) ? "synth" : "effect",
           pehost_num_inputs(h), pehost_num_outputs(h),
           pehost_num_programs(h), pehost_num_params(h));

    /* Every program's name, from one load. The alternative is --program N in a
     * loop, which reloads the plugin sixty-four times to read sixty-four
     * strings -- and is how you end up waiting ten minutes to find out whether
     * a machine has a harp in it. */
    if (o->list_programs) {
        int np = pehost_num_programs(h);
        printf("  %d program(s):\n", np);
        for (i = 0; i < np; i++) {
            char pn[64] = {0};
            pehost_program_name(h, i, pn, sizeof pn);
            printf("  %3d  %s\n", i, pn);
        }
    }

    pehost_set_program(h, o->prog);
    {
        char pn[64];
        pehost_program_name(h, o->prog, pn, sizeof pn);
        printf("  program %d: \"%s\"\n", o->prog, pn);
    }
}

/* After the program, never before it: selecting a program makes the plugin
 * overwrite every parameter with that program's values. */
static int apply_patch(pehost *h, patch_bank *bank, int patch_ix, int block)
{
    char err[256];
    int  applied = 0, missed = 0;

    if (patch_bank_apply(bank, patch_ix, h, err, sizeof err, &applied, &missed) != 0) {
        fprintf(stderr, "patch: %s\n", err);
        return 1;
    }
    /* One block through the plugin before anything is read back. Parameter
     * writes are queued for the audio thread, and on the 32-bit bridge the
     * helper applies them only as it renders -- so without this the dump below
     * and --save-patch would both report the state from before the patch. This
     * process is single-threaded, which is what makes rendering a block here
     * safe to do. */
    {
        float *scratch = calloc((size_t)block * 2, sizeof *scratch);
        if (scratch) { pehost_render(h, scratch, block); free(scratch); }
    }
    printf("  patch \"%s\": %d parameter(s) set",
           patch_bank_patch_name(bank, patch_ix), applied);
    if (missed) printf(", %d key(s) matched nothing", missed);
    printf("\n");
    if (err[0]) fprintf(stderr, "  warning: %s\n", err);
    return 0;
}

static void dump_params(pehost *h, int all)
{
    int n = pehost_num_params(h);
    int lim = all ? n : (n < 12 ? n : 12);
    int i;

    printf("\nparameters:\n");
    for (i = 0; i < lim; i++) {
        char nm[64], ds[64], lb[64];
        pehost_param_name(h, i, nm, sizeof nm);
        pehost_param_display(h, i, ds, sizeof ds);
        pehost_param_label(h, i, lb, sizeof lb);
        printf("  %3d %-24s %-10s %-8s (raw %.4f)\n", i, nm, ds, lb,
               pehost_get_param(h, i));
    }
    if (lim < n) printf("  ... %d more (--params for all)\n", n - lim);
}

/* What an effect gets fed. Impulses alone excite delays, filters and phasers,
 * but a spectral processor needs something continuous -- fed only impulses,
 * every one of the MNSpectral units reports silence and looks broken. So: a
 * tone plus an impulse train, which covers both. */
static void fill_test_signal(float *src, int frames, int at)
{
    int j;
    for (j = 0; j < frames; j++) {
        double ph = 2.0 * M_PI * 220.0 * (at + j) / (double)RATE;
        float  v  = (float)(0.4 * sin(ph));
        if ((at + j) % 12000 == 0) v += 0.5f;
        src[2 * j] = src[2 * j + 1] = v;
    }
}

static int render_to_wav(pehost *h, const opts *o)
{
    int     total = RATE * o->secs, bs = 512, done = 0, i;
    float  *inter = malloc((size_t)total * 2 * sizeof *inter);
    float  *src   = NULL;
    double  peak  = 0.0;
    int     fx_in = pehost_num_inputs(h);

    if (!inter) return 1;
    if (fx_in > 0) {
        /* An effect has nothing to process unless we feed it. */
        if (!(src = calloc((size_t)bs * 2, sizeof *src))) { free(inter); return 1; }
        printf("(effect: feeding a tone and impulse train to %d input(s))\n", fx_in);
    }

    /* --note -1 renders without sending any note, which separates a plugin's
     * DSP from its MIDI handling when one of them misbehaves. */
    if (o->note >= 0) pehost_note_on(h, o->note, 100);
    while (done < total) {
        int n = (total - done < bs) ? total - done : bs;
        if (o->note >= 0 && done <= total * 2 / 3 && done + n > total * 2 / 3)
            pehost_note_off(h, o->note);
        if (src) fill_test_signal(src, n, done);
        pehost_render_io(h, src, inter + (size_t)done * 2, n);
        done += n;
    }
    free(src);

    for (i = 0; i < total * 2; i++)
        if (fabs(inter[i]) > peak) peak = fabs(inter[i]);
    printf("\nrendered %d frames, peak %.4f%s\n", total, peak,
           peak < 1e-9 ? "  !! silence" : "");
    if (write_wav(o->wav, inter, total, RATE)) printf("wrote %s\n", o->wav);
    free(inter);
    return 0;
}

static void capture_editor(pehost *h, const char *shot, const opts *o)
{
    int kind = pehost_editor_kind(h);
    int w = 0, ht = 0;

    /* Which backend produced the pixels is worth saying: they are very
     * different paths, and "it drew something" is the interesting part. */
    printf("\neditor: %s\n",
           kind == PEHOST_EDITOR_PIXELS
               ? (pehost_is_macos(h) ? "pixel buffer (software Metal)"
                                     : "pixel buffer (Win32 layer)")
           : kind == PEHOST_EDITOR_X11 ? "X11 embed (needs a window)"
                                       : "none");
    if (kind != PEHOST_EDITOR_PIXELS) return;

    pehost_editor_size(h, &w, &ht);
    printf("  reported size %dx%d\n", w, ht);
    if (pehost_editor_open(h) != 0) { printf("  effEditOpen failed\n"); return; }

    {
        const unsigned int *px = NULL;
        int pw = 0, ph = 0, frame, ci;

        /* Let it settle: editors commonly need a few idle cycles before the
         * first full repaint lands. */
        for (frame = 0; frame < 40; frame++) {
            pehost_editor_pump(h);
            usleep(16000);
        }
        /* Then any clicks asked for. An editor whose controls only appear once
         * a tab is selected, or a page turned, cannot be photographed without
         * being touched first -- and doing it here means the picture can be
         * checked without a display or a person. Each click is a press, a
         * short hold and a release, with idles throughout, because that is
         * what a plug-in watching for a drag expects to see. */
        for (ci = 0; ci < o->nclick; ci++) {
            pehost_editor_mouse(h, o->click[ci].x, o->click[ci].y, WM_MOUSEMOVE, 0, 0);
            pehost_editor_pump(h); usleep(16000);
            pehost_editor_mouse(h, o->click[ci].x, o->click[ci].y, WM_LBUTTONDOWN, 1, 0);
            for (frame = 0; frame < 4; frame++) { pehost_editor_pump(h); usleep(16000); }
            pehost_editor_mouse(h, o->click[ci].x, o->click[ci].y, WM_LBUTTONUP, 0, 0);
            for (frame = 0; frame < 30; frame++) { pehost_editor_pump(h); usleep(16000); }
            printf("  clicked %d,%d\n", o->click[ci].x, o->click[ci].y);
        }
        if (pehost_editor_pixels(h, &px, &pw, &ph) && px && pw > 0 && ph > 0) {
            long nonzero = 0;
            int  k;
            for (k = 0; k < pw * ph; k++) if (px[k] & 0x00FFFFFF) nonzero++;
            printf("  captured %dx%d, %ld/%d pixels non-black (%.1f%%)\n",
                   pw, ph, nonzero, pw * ph, 100.0 * nonzero / (pw * ph));
            if (write_ppm(shot, px, pw, ph)) printf("  wrote %s\n", shot);
        } else printf("  no pixels produced\n");
        w32_stats();
    }
}

int main(int argc, char **argv)
{
    opts        o;
    patch_bank *bank = NULL;
    int         patch_ix = 0, rc;
    pehost_kind force = PEHOST_KIND_AUTO;
    pehost     *h;

    if ((rc = parse_args(argc, argv, &o))) return rc;
    if ((rc = open_bank(&o, &bank, &patch_ix))) return rc;
    if (!o.path) { usage(); return 2; }
    if (o.detect) return cmd_detect(o.path);

    /* --as forces a loader. An unknown name is an error rather than a silent
     * fall-back to auto, so a typo does not quietly load under the wrong guess. */
    if (o.as_name) {
        force = pehost_kind_from_name(o.as_name);
        if (force == PEHOST_KIND_AUTO && strcasecmp(o.as_name, "auto")) {
            fprintf(stderr, "unknown --as loader '%s'\n", o.as_name);
            return 2;
        }
    }

    if (!(h = pehost_open_as(o.path, force, (double)RATE, o.block))) {
        fprintf(stderr, "load failed: %s\n", pehost_last_error());
        return 1;
    }

    describe(h, &o);
    if (bank && apply_patch(h, bank, patch_ix, o.block)) { pehost_close(h); return 1; }
    dump_params(h, o.dump);

    /* Written from the state the dump above just described, so the file and the
     * listing always agree -- including when --patch put us here. */
    if (o.patch_out) {
        char err[256];
        if (patch_save(h, o.patch_out, o.path, err, sizeof err) != 0)
            fprintf(stderr, "save-patch: %s\n", err);
        else
            printf("\nwrote %s (%d parameters)\n", o.patch_out, pehost_num_params(h));
    }

    if (o.wav && render_to_wav(h, &o)) { pehost_close(h); return 1; }
    if (o.shot) capture_editor(h, o.shot, &o);

    {
        int impl, stub, called;
        pehost_import_stats(&impl, &stub, &called);
        printf("\nimports: %d implemented, %d stubbed, %d stubs reached\n",
               impl, stub, called);
    }
    pehost_close(h);

    /* Leave without running the plugin's global destructors. Foreign plugin
     * code registers its own atexit/DSO teardown, and some of it faults --
     * Cardinal does -- long after the render is safely written. Exiting this
     * way keeps the status code meaningful instead of reporting a crash for
     * work that completed. */
    fflush(NULL);
    _exit(0);
}
