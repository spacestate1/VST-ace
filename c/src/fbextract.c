/* fbextract -- pull everything interesting out of an FB-7999 plugin binary.
 *
 * A single self-contained C program covering what the Python scripts in
 * ../scripts did:
 *
 *   resources   walk the PE .rsrc tree and write it to disk   (was: 7z x)
 *   probe       structural analysis of WAVEDST                (was: analyze_wavedst.py)
 *   spectra     per-waveform harmonic report + JSON           (was: parse_wavedst.py)
 *   waves       additive resynthesis to WAV + verification    (was: render_waves.py)
 *   bank        decode a PROGINIT preset bank                 (new)
 *   all         resources + spectra + waves + every bank
 *
 * Works on both fb799964.dll (VST2) and fb7999.vst3 -- their resources are
 * byte-identical.  No dependencies beyond libc and libm. */

#include "bank.h"
#include "pe.h"
#include "wav.h"
#include "wavedst.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FRAME 2048   /* samples per single cycle; Serum/Surge convention */

static int mkdir_p(const char *path)
{
    char   tmp[1024];
    size_t len = strlen(path), i;

    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    if (len && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (i = 1; i <= len; i++) {
        if (tmp[i] == '/' || i == len) {
            char save = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) && errno != EEXIST) { perror(tmp); return -1; }
            tmp[i] = save;
        }
    }
    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (n && fwrite(data, 1, n, f) != n) { perror(path); fclose(f); return -1; }
    fclose(f);
    return 0;
}

static unsigned char *read_file(const char *path, size_t *out)
{
    FILE          *f = fopen(path, "rb");
    unsigned char *buf;
    long           n;

    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    if (!(buf = malloc((size_t)n ? (size_t)n : 1))) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror(path); fclose(f); free(buf); return NULL; }
    fclose(f);
    *out = (size_t)n;
    return buf;
}

/* ---- resource extraction ---- */

typedef struct {
    const char    *outdir;
    int            count;
    unsigned char *wavedst;      /* borrowed pointer into the mapped image */
    uint32_t       wavedst_size;
} rsrc_ctx;

/* iPlug2's .rc quotes the image and font resource names, and MSVC stores the
 * quotes as part of the name -- the PNG entries really are named "BACK.PNG"
 * with the quote characters included. Strip a matching pair so the extracted
 * tree has usable filenames (this is what 7z displays too). */
static void unquote(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    if (n >= 2 && in[0] == '"' && in[n - 1] == '"') { in++; n -= 2; }
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

static int on_resource(const char *type, const char *name, int type_id,
                       const unsigned char *data, uint32_t size, void *ud)
{
    rsrc_ctx *ctx = ud;
    char      dir[1024], path[1152];
    char      t[256], n[256];

    (void)type_id;
    unquote(type, t, sizeof t);
    unquote(name, n, sizeof n);

    snprintf(dir,  sizeof dir,  "%s/%s", ctx->outdir, t);
    snprintf(path, sizeof path, "%s/%s", dir, n);
    if (mkdir_p(dir)) return -1;

    if (write_file(path, data, size)) return -1;
    printf("  %-14s %-22s %8u bytes\n", t, n, size);
    ctx->count++;

    if (!strcmp(t, "DSTDATA") && !strcmp(n, "WAVEDST")) {
        ctx->wavedst      = (unsigned char *)data;
        ctx->wavedst_size = size;
    }
    return 0;
}

/* ---- waveform rendering ---- */

static int render_waves(const wavedst *wd, const char *outdir)
{
    double *cyc, *all;
    int     w, ok = 0;
    size_t  total = (size_t)wd->nwaves * FRAME;

    if (mkdir_p(outdir)) return -1;
    if (!(cyc = malloc(FRAME * sizeof *cyc))) return -1;
    if (!(all = malloc(total * sizeof *all))) { free(cyc); return -1; }

    for (w = 0; w < wd->nwaves; w++) {
        char   path[1024];
        double rms = 0.0;
        int    t, nz = 0;

        wavedst_synth(wd, w, cyc, FRAME);
        for (t = 0; t < FRAME; t++) {
            rms += cyc[t] * cyc[t];
            all[(size_t)w * FRAME + t] = cyc[t];
        }
        rms = sqrt(rms / (double)FRAME);
        for (t = 0; t < wd->nharm; t++)
            if (fabs(wavedst_row(wd, w)[t]) > 1e-6) nz++;

        snprintf(path, sizeof path, "%s/dw_wave_%02d.wav", outdir, w);
        if (wav_write_mono16(path, cyc, FRAME, 44100)) break;
        printf("  wave %2d: %3d harmonics, rms=%.4f -> dw_wave_%02d.wav\n", w, nz, rms, w);
        ok++;
    }

    if (ok == wd->nwaves) {
        char path[1024];
        snprintf(path, sizeof path, "%s/dw_wavetable.wav", outdir);
        if (!wav_write_mono16(path, all, total, 44100))
            printf("\nwrote %d single cycles + dw_wavetable.wav (%zu samples, "
                   "%d frames of %d)\n", ok, total, wd->nwaves, FRAME);
    }

    free(cyc);
    free(all);
    return ok == wd->nwaves ? 0 : -1;
}

static void verify_waves(const wavedst *wd)
{
    static const struct { int wave; const char *ideal; } checks[] = {
        { 0, "sawtooth" }, { 25, "square" }, { 27, "sine" }
    };
    int i;

    printf("\n--- shape verification (best |correlation| over circular shift) ---\n");
    for (i = 0; i < (int)(sizeof checks / sizeof *checks); i++) {
        double r;
        if (checks[i].wave >= wd->nwaves) continue;
        r = wavedst_match(wd, checks[i].wave, checks[i].ideal, FRAME);
        printf("  wave %2d vs ideal %-8s : r = %.4f   %s\n",
               checks[i].wave, checks[i].ideal, r, r > 0.97 ? "MATCH" : "no match");
    }
}

/* ---- subcommands ---- */

static int cmd_resources(const char *binary, const char *outdir)
{
    pe_image img;
    rsrc_ctx ctx;
    int      rc;

    if (pe_open(&img, binary)) return 1;
    printf("%s: PE%s, %zu bytes, .rsrc rva=0x%x size=%u\n\n",
           binary, img.is64 ? "32+" : "32", img.size, img.rsrc_rva, img.rsrc_size);

    memset(&ctx, 0, sizeof ctx);
    ctx.outdir = outdir;
    rc = pe_walk_resources(&img, on_resource, &ctx);
    printf("\n%d resources -> %s\n", ctx.count, outdir);

    pe_close(&img);
    return rc ? 1 : 0;
}

static int load_wavedst_from(const char *path, wavedst *wd,
                             unsigned char **owned, size_t *owned_size)
{
    unsigned char *buf;
    size_t         n;

    if (!(buf = read_file(path, &n))) return -1;
    if (wavedst_load(wd, buf, n, 0)) {
        fprintf(stderr, "%s: could not determine WAVEDST geometry\n", path);
        free(buf);
        return -1;
    }
    *owned = buf;
    *owned_size = n;
    return 0;
}

static int cmd_probe(const char *path)
{
    unsigned char *buf;
    size_t         n;

    if (!(buf = read_file(path, &n))) return 1;
    printf("file: %s\n", path);
    wavedst_probe(buf, n);
    free(buf);
    return 0;
}

static int cmd_spectra(const char *path, const char *json)
{
    wavedst        wd;
    unsigned char *buf;
    size_t         n;

    if (load_wavedst_from(path, &wd, &buf, &n)) return 1;
    printf("%d waves x %d harmonics\n\n", wd.nwaves, wd.nharm);
    wavedst_report(&wd, json);
    if (json) printf("\n-> %s\n", json);
    wavedst_free(&wd);
    free(buf);
    return 0;
}

static int cmd_waves(const char *path, const char *outdir)
{
    wavedst        wd;
    unsigned char *buf;
    size_t         n;
    int            rc;

    if (load_wavedst_from(path, &wd, &buf, &n)) return 1;
    printf("%d waves x %d harmonics\n", wd.nwaves, wd.nharm);
    rc = render_waves(&wd, outdir);
    verify_waves(&wd);
    wavedst_free(&wd);
    free(buf);
    return rc ? 1 : 0;
}

static int cmd_bank(const char *path, const char *names_path,
                    const char *csv, int verbose)
{
    unsigned char *buf;
    size_t         n;
    bank           b;
    bank_names     names;
    int            have_names = 0, rc = 0;

    if (!(buf = read_file(path, &n))) return 1;
    if (bank_parse(&b, buf, n)) { free(buf); return 1; }

    if (names_path && bank_names_load(&names, names_path) > 0) have_names = 1;

    printf("file: %s (%zu bytes)\n", path, n);
    bank_print(&b, have_names ? &names : NULL, verbose);
    if (csv && !bank_write_csv(&b, have_names ? &names : NULL, csv))
        printf("\n-> %s\n", csv);

    if (have_names) bank_names_free(&names);
    bank_free(&b);
    free(buf);
    return rc;
}

static int cmd_all(const char *binary, const char *outdir)
{
    pe_image img;
    rsrc_ctx ctx;
    wavedst  wd;
    char     path[1024], sub[1152];
    int      rc = 0;
    static const char *banks[] = { "BANK_A", "BANK_B", "PROG6000" };
    int      i;

    if (pe_open(&img, binary)) return 1;
    printf("=== resources ===\n%s: PE%s, %zu bytes\n\n",
           binary, img.is64 ? "32+" : "32", img.size);

    memset(&ctx, 0, sizeof ctx);
    snprintf(path, sizeof path, "%s/resources", outdir);
    ctx.outdir = path;
    if (pe_walk_resources(&img, on_resource, &ctx)) { pe_close(&img); return 1; }
    printf("\n%d resources -> %s\n", ctx.count, path);

    if (!ctx.wavedst) {
        fprintf(stderr, "no DSTDATA/WAVEDST resource found\n");
        pe_close(&img);
        return 1;
    }

    printf("\n=== WAVEDST structure ===\n");
    wavedst_probe(ctx.wavedst, ctx.wavedst_size);

    if (wavedst_load(&wd, ctx.wavedst, ctx.wavedst_size, 0)) {
        fprintf(stderr, "could not determine WAVEDST geometry\n");
        pe_close(&img);
        return 1;
    }

    printf("\n=== spectra: %d waves x %d harmonics ===\n", wd.nwaves, wd.nharm);
    snprintf(sub, sizeof sub, "%s/wavedst_spectra.json", outdir);
    wavedst_report(&wd, sub);

    printf("\n=== waves ===\n");
    snprintf(sub, sizeof sub, "%s/waves", outdir);
    rc |= render_waves(&wd, sub);
    verify_waves(&wd);

    for (i = 0; i < 3; i++) {
        unsigned char *buf;
        size_t         n;
        bank           b;

        snprintf(sub, sizeof sub, "%s/%s/PROGINIT", path, banks[i]);
        if (!(buf = read_file(sub, &n))) continue;
        printf("\n=== bank %s ===\n", banks[i]);
        if (!bank_parse(&b, buf, n)) {
            char csv[1152];
            bank_print(&b, NULL, 0);
            snprintf(csv, sizeof csv, "%s/%s.csv", outdir, banks[i]);
            if (!bank_write_csv(&b, NULL, csv)) printf("\n-> %s\n", csv);
            bank_free(&b);
        }
        free(buf);
    }

    wavedst_free(&wd);
    pe_close(&img);
    return rc ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr,
        "fbextract -- FB-7999 resource / wavetable / preset extractor\n\n"
        "usage:\n"
        "  fbextract all       <plugin.dll|.vst3> <outdir>\n"
        "  fbextract resources <plugin.dll|.vst3> <outdir>\n"
        "  fbextract probe     <WAVEDST>\n"
        "  fbextract spectra   <WAVEDST> [out.json]\n"
        "  fbextract waves     <WAVEDST> <outdir>\n"
        "  fbextract bank      <PROGINIT> [-n names.txt] [-c out.csv] [-v]\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }

    if (!strcmp(argv[1], "all") && argc == 4)
        return cmd_all(argv[2], argv[3]);

    if (!strcmp(argv[1], "resources") && argc == 4)
        return cmd_resources(argv[2], argv[3]);

    if (!strcmp(argv[1], "probe") && argc == 3)
        return cmd_probe(argv[2]);

    if (!strcmp(argv[1], "spectra") && (argc == 3 || argc == 4))
        return cmd_spectra(argv[2], argc == 4 ? argv[3] : NULL);

    if (!strcmp(argv[1], "waves") && argc == 4)
        return cmd_waves(argv[2], argv[3]);

    if (!strcmp(argv[1], "bank") && argc >= 3) {
        const char *names = NULL, *csv = NULL;
        int         verbose = 0, i;
        for (i = 3; i < argc; i++) {
            if      (!strcmp(argv[i], "-n") && i + 1 < argc) names = argv[++i];
            else if (!strcmp(argv[i], "-c") && i + 1 < argc) csv   = argv[++i];
            else if (!strcmp(argv[i], "-v"))                 verbose = 1;
            else { usage(); return 2; }
        }
        return cmd_bank(argv[2], names, csv, verbose);
    }

    usage();
    return 2;
}
