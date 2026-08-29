/* dw -- one program for the whole tree.
 *
 * This replaces dw.sh. The shell version had grown into a launcher that
 * located the plugin, extracted its resources if they were missing, ran make,
 * rendered a preset to a temporary WAV and then went hunting for a pw-play or
 * an aplay to hand it to. Most of that was work to make up for not being a
 * program: the DW-8000 engine is right here, so `list`, `play`, `demo`,
 * `render` and the live keyboard are calls, not subprocesses, and the WAV and
 * its external player are gone with them.
 *
 * What is still spawned is what genuinely has to be a separate process: the Qt
 * host, the GTK front end, the CLI plug-in host, and the i386 loader -- which
 * cannot even share this one, because a process cannot execute both widths.
 *
 * Everything the shell version knew how to find, this finds the same way, with
 * one difference worth stating: the wavetable and the preset banks are read
 * straight out of the plugin's PE resources, so there is no extract-to-disk
 * step and nothing to go stale. An already-extracted resource tree still works
 * if the DLL itself is not around. */

#define _POSIX_C_SOURCE 200809L

#include "bank.h"
#include "dw_synth.h"
#include "dw_wavetable.h"
#include "rom.h"
#include "wav.h"
#include "wavedst.h"

#ifdef DW_HAVE_ALSA
#include "dwplay.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#define SR 48000

/* ------------------------------------------------------------ the tree ---- */

/* An installed copy has no source tree to find: the package build puts the
 * helper programs in one directory and the data in another, and defines these
 * to say where. A source build leaves them empty, and every installed path
 * below is then unreachable -- `dw` behaves exactly as it always has. */
#ifndef DW_PKGLIBDIR
#define DW_PKGLIBDIR ""
#endif
#ifndef DW_PKGDATADIR
#define DW_PKGDATADIR ""
#endif

/* Where everything lives, worked out once at startup. */
static char g_re[PATH_MAX];      /* .../vst/re, or the installed data dir  */
static char g_vst[PATH_MAX];     /* .../vst -- the plug-in corpus above it */
static int  g_installed;         /* found at DW_PKGLIBDIR, not in a tree   */

static int is_dir(const char *p)
{ struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

static int is_file(const char *p)
{ struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

/* The plug-in corpus, for an installed copy.
 *
 * In a source tree it is simply the directory holding `re`, which is where the
 * windows/, linux/ and macos/ folders sit. A package has no such tree, and the
 * corpus is not something a package could ship in any case -- the plug-ins are
 * their authors' own. So it is named by VST_ROOT, or looked for at ~/vst,
 * which is the same layout one directory further down. Nothing here is fatal:
 * an unset corpus only means bare plug-in names do not expand and `dw peload`
 * with no arguments has nothing to list. */
static void corpus_root(char *out, size_t n)
{
    const char *env = getenv("VST_ROOT");
    const char *home;
    char        p[PATH_MAX];

    out[0] = 0;
    if (env && *env && is_dir(env)) { snprintf(out, n, "%s", env); return; }
    if ((home = getenv("HOME")) && *home) {
        snprintf(p, sizeof p, "%s/vst", home);
        if (is_dir(p)) snprintf(out, n, "%s", p);
    }
}

/* Find the tree from the executable rather than the working directory -- the
 * point of a single binary is that it runs from anywhere, including a copy on
 * $PATH. Walking up looking for peload/pehost.c finds `re` whether this was
 * started as re/dw or as re/c/build/dw, and does not mistake a build directory
 * for the top of the tree the way a bare `c/` test would. */
static int locate_tree(void)
{
    char  exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;
    int   up;

    if (n <= 0) { fprintf(stderr, "dw: cannot read /proc/self/exe\n"); return -1; }
    exe[n] = 0;

    for (up = 0; up < 8; up++) {
        char probe[PATH_MAX];
        if (!(slash = strrchr(exe, '/'))) break;
        *slash = 0;                              /* strip a component */
        if (!exe[0]) break;
        snprintf(probe, sizeof probe, "%s/peload/pehost.c", exe);
        if (is_file(probe)) {
            snprintf(g_re, sizeof g_re, "%s", exe);
            if ((slash = strrchr(exe, '/')) && slash != exe) {
                *slash = 0;
                snprintf(g_vst, sizeof g_vst, "%s", exe);
            } else {
                snprintf(g_vst, sizeof g_vst, "%s/..", g_re);
            }
            return 0;
        }
    }
    /* No source tree above us. The other possibility is an installed copy,
     * where the helpers sit together in one directory and there is nothing to
     * build -- so this is a check for the helpers, not for sources. */
    if (DW_PKGLIBDIR[0]) {
        char probe[PATH_MAX];
        snprintf(probe, sizeof probe, "%s/peload", DW_PKGLIBDIR);
        if (is_file(probe)) {
            g_installed = 1;
            snprintf(g_re, sizeof g_re, "%s", DW_PKGDATADIR);
            corpus_root(g_vst, sizeof g_vst);
            return 0;
        }
    }

    fprintf(stderr, "dw: cannot find the tree from %s -- expected a peload/ "
                    "directory above it\n", exe);
    return -1;
}

/* ------------------------------------------------------------- the ROM ---- */

/* The wavetable and the banks, however this machine has them.
 *
 * The plugin binary carries both, so it is the first choice and the only one
 * that needs nothing prepared. An extracted `out/resources/.rsrc` tree is the
 * fallback, for a checkout that has the resources but not the DLL. */
typedef struct {
    char plugin[PATH_MAX];       /* the DLL, or empty */
    char rsrc[PATH_MAX];         /* extracted resource tree, or empty */
} romsrc;

static void rom_find(romsrc *r)
{
    const char *env = getenv("FB7999");

    r->plugin[0] = r->rsrc[0] = 0;

    if (env && *env && is_file(env))
        snprintf(r->plugin, sizeof r->plugin, "%s", env);
    else {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/windows/VST2-64/fb799964.dll", g_vst);
        if (is_file(p)) snprintf(r->plugin, sizeof r->plugin, "%s", p);
    }

    {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/out/resources/.rsrc", g_re);
        if (is_dir(p)) snprintf(r->rsrc, sizeof r->rsrc, "%s", p);
    }
}

/* One named resource, from whichever source has it. */
static unsigned char *rom_get(const romsrc *r, const char *type,
                              const char *name, size_t *size, char *from, size_t fromn)
{
    unsigned char *b;

    if (r->plugin[0] && (b = rom_resource(r->plugin, type, name, size))) {
        if (from) snprintf(from, fromn, "%s in %s", name, r->plugin);
        return b;
    }
    if (r->rsrc[0]) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s/%s", r->rsrc, type, name);
        if (is_file(p)) {
            if (from) snprintf(from, fromn, "%s", p);
            return rom_slurp(p, size);
        }
    }
    return NULL;
}

static void rom_complain(const romsrc *r, const char *type, const char *name)
{
    fprintf(stderr, "dw: cannot find %s/%s.\n", type, name);
    if (!r->plugin[0])
        fprintf(stderr, "    No plugin at %s/windows/VST2-64/fb799964.dll "
                        "(set FB7999 to point at one).\n", g_vst);
    if (!r->rsrc[0])
        fprintf(stderr, "    No extracted resources at %s/out/resources/.rsrc "
                        "either.\n", g_re);
}

/* BANK=A|B|6000 picks which of the three the plugin carries. */
static const char *bank_resource_type(void)
{
    const char *b = getenv("BANK");

    if (!b || !*b) return "BANK_A";
    if (!strcasecmp(b, "A")) return "BANK_A";
    if (!strcasecmp(b, "B")) return "BANK_B";
    if (!strcmp(b, "6000") || !strcasecmp(b, "DW6000")) return "PROG6000";
    fprintf(stderr, "dw: BANK must be A, B or 6000 (got '%s')\n", b);
    exit(2);
}

/* ------------------------------------------------------ engine assembly ---- */

/* Everything needed to make a sound, loaded once per command. */
typedef struct {
    unsigned char *wraw;
    size_t         wsize;
    unsigned char *braw;
    size_t         bsize;
    char           bank_from[PATH_MAX];
    wavedst        wd;
    dw_wavetable   wt;
    dw_synth       syn;
    bank           bk;
    int            have_bank, have_engine;
} engine;

/* Fetch both blobs and decode the bank. Cheap: no tables are built here, which
 * is why `list` and the live keyboard can stop at this point. */
static int engine_load(engine *e)
{
    romsrc      r;
    const char *type = bank_resource_type();

    memset(e, 0, sizeof *e);
    rom_find(&r);

    if (!(e->braw = rom_get(&r, type, "PROGINIT", &e->bsize,
                            e->bank_from, sizeof e->bank_from))) {
        rom_complain(&r, type, "PROGINIT");
        return -1;
    }
    if (rom_bank_parse(&e->bk, e->braw, e->bsize, e->bank_from)) return -1;
    e->have_bank = 1;

    if (!(e->wraw = rom_get(&r, "DSTDATA", "WAVEDST", &e->wsize, NULL, 0))) {
        rom_complain(&r, "DSTDATA", "WAVEDST");
        return -1;
    }
    return 0;
}

/* Build the mip tables and the synth. This is where the startup time goes, so
 * only the commands that make a sound pay for it. */
static int engine_build(engine *e)
{
    if (wavedst_load(&e->wd, e->wraw, e->wsize, 0)) {
        fprintf(stderr, "dw: could not determine WAVEDST geometry\n");
        return -1;
    }
    if (dw_wavetable_build(&e->wt, &e->wd, SR)) {
        fprintf(stderr, "dw: could not build the mip tables\n");
        return -1;
    }
    if (dw_synth_init(&e->syn, &e->wt, SR)) {
        fprintf(stderr, "dw: synth init failed\n");
        return -1;
    }
    e->have_engine = 1;
    return 0;
}

/* Load and build, for the commands that always need both. */
static int engine_open(engine *e)
{ return engine_load(e) ? -1 : engine_build(e); }

static void engine_close(engine *e)
{
    if (e->have_engine) {
        dw_synth_free(&e->syn);
        dw_wavetable_free(&e->wt);
        wavedst_free(&e->wd);
    }
    if (e->have_bank) bank_free(&e->bk);
    free(e->wraw);
    free(e->braw);
}

/* Render one program into a fresh interleaved stereo buffer, held for `gate`
 * seconds and left to tail out for the rest of `total`. Caller frees. */
static double *render_program(engine *e, const bank_program *prog, const int *notes,
                              int nnotes, double gate, double total, size_t *frames_out)
{
    size_t  frames = (size_t)(total * SR);
    size_t  gate_f = (size_t)(gate  * SR);
    double *buf;
    int     i;

    if (gate_f > frames) gate_f = frames;
    if (!(buf = calloc(frames * 2, sizeof *buf))) return NULL;

    dw_synth_set_program(&e->syn, prog->param);
    dw_synth_all_off(&e->syn);
    for (i = 0; i < nnotes; i++) dw_synth_note_on(&e->syn, notes[i], 100);
    dw_synth_render(&e->syn, buf, (int)gate_f);
    for (i = 0; i < nnotes; i++) dw_synth_note_off(&e->syn, notes[i]);
    dw_synth_render(&e->syn, buf + gate_f * 2, (int)(frames - gate_f));

    *frames_out = frames;
    return buf;
}

/* NOTE, GATE and LEN, as the shell version took them. */
static double env_double(const char *name, double dflt)
{
    const char *s = getenv(name);
    char *end;
    double v;
    if (!s || !*s) return dflt;
    v = strtod(s, &end);
    return (end != s && v > 0.0) ? v : dflt;
}

static int env_note(void)
{
    const char *s = getenv("NOTE");
    int n = s && *s ? atoi(s) : 60;
    return (n >= 0 && n <= 127) ? n : 60;
}

/* --------------------------------------------------- spawning the others ---- */

/* Run a program and wait. Returns its exit status, or -1 if it never started. */
static int run(char *const argv[])
{
    pid_t pid = fork();
    int   status;

    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "dw: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Same, with the child's output discarded. For probes -- `pkg-config --exists`
 * says what it has to say in its exit status, and a missing pkg-config should
 * not print anything at all. */
static int run_quiet(char *const argv[])
{
    pid_t pid = fork();
    int   status;

    if (pid < 0) return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); close(null); }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* cmake configure-and-build for one of the cmake projects in the tree.
 *
 * Built every time rather than only when the binary is missing, which is what
 * the shell version settled on and for the reason it gives: an incremental
 * build costs nothing when nothing changed, whereas skipping it after a source
 * edit silently runs the previous binary, and that looks exactly like the edit
 * not having worked. The output is shown for the same reason -- a minute of
 * silence is indistinguishable from a hang. */
static int cmake_build(const char *srcdir, const char *target)
{
    char cache[PATH_MAX], builddir[PATH_MAX];

    snprintf(builddir, sizeof builddir, "%s/build", srcdir);
    snprintf(cache, sizeof cache, "%s/CMakeCache.txt", builddir);

    if (!is_file(cache)) {
        char *conf[] = { "cmake", "-S", (char *)srcdir, "-B", builddir,
                         "-DCMAKE_BUILD_TYPE=Release", NULL };
        if (run(conf) != 0) return -1;
    }
    {
        char *build[] = { "cmake", "--build", builddir, "--parallel",
                          "--target", (char *)target, NULL };
        if (run(build) != 0) return -1;
    }
    return 0;
}

/* Hand off to one of the plug-in hosts, having built it first. An extra
 * flag/value pair goes in front of the user's own arguments, which is how the
 * GTK front end gets its --backend. */
static int exec_tool(const char *srcdir, const char *target, int argc,
                     char **argv, const char *extra_flag, const char *extra_val)
{
    char   path[PATH_MAX];
    char **av;
    int    n = 0, i;

    if (g_installed) {
        /* Nothing to build: the package did that. A missing program here is a
         * broken install rather than a missing dependency, so say so
         * differently -- the advice below would send someone looking at cmake
         * output that does not exist. */
        snprintf(path, sizeof path, "%s/%s", DW_PKGLIBDIR, target);
        if (!is_file(path)) {
            fprintf(stderr, "dw: %s is missing from %s -- this is an installed "
                            "copy and it should be there\n", target, DW_PKGLIBDIR);
            return 1;
        }
    } else {
        if (cmake_build(srcdir, target)) {
            fprintf(stderr, "dw: %s failed to build\n", target);
            return 1;
        }
        snprintf(path, sizeof path, "%s/build/%s", srcdir, target);
        if (!is_file(path)) {
            fprintf(stderr, "dw: %s was not built -- see the cmake output above "
                            "for the dependency it is missing\n", target);
            return 1;
        }
    }

    if (!(av = calloc((size_t)argc + 4, sizeof *av))) return 1;
    av[n++] = path;
    if (extra_flag && extra_val) { av[n++] = (char *)extra_flag; av[n++] = (char *)extra_val; }
    for (i = 0; i < argc; i++) av[n++] = argv[i];
    av[n] = NULL;

    execv(path, av);
    fprintf(stderr, "dw: cannot run %s: %s\n", path, strerror(errno));
    return 1;
}

/* A bare plugin name means the one in the corpus, since that is what `dw
 * peload` with no arguments lists. Anything with a slash, or that exists as
 * given, is left alone. */
static void expand_plugin_names(int argc, char **argv, const char *subdir)
{
    int i;
    for (i = 0; i < argc; i++) {
        char cand[PATH_MAX];
        if (argv[i][0] == '-' || strchr(argv[i], '/') || is_file(argv[i]) || is_dir(argv[i]))
            continue;
        snprintf(cand, sizeof cand, "%s/%s/%s", g_vst, subdir, argv[i]);
        if (!is_file(cand) && !is_dir(cand))
            snprintf(cand, sizeof cand, "%s/%s/%s.dll", g_vst, subdir, argv[i]);
        /* Never freed, and does not need to be: the next thing this process
         * does is exec. */
        if (is_file(cand) || is_dir(cand)) argv[i] = strdup(cand);
    }
}

static void list_corpus(const char *subdir)
{
    char           dir[PATH_MAX];
    DIR           *d;
    struct dirent *ent;

    snprintf(dir, sizeof dir, "%s/%s", g_vst, subdir);
    if (!(d = opendir(dir))) return;
    printf("plugins in %s:\n", dir);
    while ((ent = readdir(d)))
        if (ent->d_name[0] != '.') printf("  %s\n", ent->d_name);
    closedir(d);
}

/* ------------------------------------------------------- which window ---- */

/* There are two windows, and they are different programs on different
 * toolkits: pestudio is Qt6 and hosts real plug-ins, dwstudio is GTK4 and
 * drives the engines in this tree. Naming one is always allowed. When none is
 * named, the question is usually settled by which one this machine can
 * actually run -- plenty of boxes have one toolkit and not the other. */
typedef enum { GUI_NONE = 0, GUI_QT, GUI_GTK } guikind;

/* Case-insensitive substring, so the desktop test does not need _GNU_SOURCE
 * for strcasestr. */
static int has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

static int pkg_exists(const char *module)
{
    char *av[] = { "pkg-config", "--exists", NULL, NULL };
    av[2] = (char *)module;
    return run_quiet(av) == 0;
}

/* Built already, or buildable. The binary settles it when it is there;
 * otherwise ask whether the toolkit is installed, since `dw` builds before it
 * launches anyway and a missing Qt or GTK is what would stop it. */
static int have_gui(guikind k)
{
    char p[PATH_MAX];

    /* Installed, the binary is the whole answer: it was either built into the
     * package or it was not, and asking pkg-config about a -dev package that a
     * user's machine has no reason to carry would only ever say no. */
    if (g_installed) {
        snprintf(p, sizeof p, "%s/%s", DW_PKGLIBDIR,
                 k == GUI_QT ? "pestudio" : "dwstudio");
        return is_file(p);
    }

    if (k == GUI_QT) {
        snprintf(p, sizeof p, "%s/peload/build/pestudio", g_re);
        return is_file(p) || pkg_exists("Qt6Widgets");
    }
    snprintf(p, sizeof p, "%s/gui/build/dwstudio", g_re);
    return is_file(p) || pkg_exists("gtk4");
}

/* What the desktop itself is built on. Only used to break a tie. */
static guikind desktop_toolkit(void)
{
    const char *d   = getenv("XDG_CURRENT_DESKTOP");
    const char *kde = getenv("KDE_FULL_SESSION");

    /* Set-but-empty is not set: some login scripts export these blank, and
     * getenv answers "" rather than NULL for them. */
    if (kde && *kde) return GUI_QT;
    if (!d || !*d) return GUI_NONE;
    if (has_ci(d, "KDE") || has_ci(d, "plasma") || has_ci(d, "LXQt")) return GUI_QT;
    if (has_ci(d, "GNOME") || has_ci(d, "XFCE") || has_ci(d, "Cinnamon") ||
        has_ci(d, "MATE")  || has_ci(d, "Budgie") || has_ci(d, "Pantheon"))
        return GUI_GTK;
    return GUI_NONE;
}

/* DW_GUI, then availability, then the desktop. `why` gets a short phrase for
 * the line printed on the way out, because a program that picks for you should
 * say what it picked and on what grounds. */
static guikind pick_gui(const char **why)
{
    const char *want = getenv("DW_GUI");
    guikind     desk;
    int         qt, gtk;

    if (want && *want) {
        if (has_ci(want, "qt") || !strcasecmp(want, "pe") || !strcasecmp(want, "pestudio")) {
            *why = "DW_GUI"; return GUI_QT;
        }
        if (has_ci(want, "gtk") || !strcasecmp(want, "gui") || !strcasecmp(want, "dwstudio")) {
            *why = "DW_GUI"; return GUI_GTK;
        }
        fprintf(stderr, "dw: DW_GUI should be qt or gtk (got '%s') -- detecting instead\n", want);
    }

    qt  = have_gui(GUI_QT);
    gtk = have_gui(GUI_GTK);
    if (qt && !gtk) { *why = "the only toolkit installed"; return GUI_QT;  }
    if (gtk && !qt) { *why = "the only toolkit installed"; return GUI_GTK; }
    if (!qt && !gtk) return GUI_NONE;

    /* Both work. Match the session: a Plasma desktop is already Qt and a GNOME
     * one is already GTK, so the matching window is the one that will look and
     * behave like everything else on screen. */
    if ((desk = desktop_toolkit()) != GUI_NONE) { *why = "this desktop"; return desk; }

    *why = "both available";
    return GUI_QT;                 /* the plug-in host is what people come for */
}

/* ------------------------------------------------------------ commands ---- */

static int cmd_list(void)
{
    engine e;
    int    i, rc;

    if ((rc = engine_load(&e))) { engine_close(&e); return 1; }
    for (i = 0; i < e.bk.count; i++) printf("%3d  %s\n", i, e.bk.prog[i].name);
    engine_close(&e);
    return 0;
}

static int cmd_render(const char *outdir)
{
    engine e;
    int    i, note = env_note(), rc = 1;
    double gate = env_double("GATE", 1.5), len = env_double("LEN", 3.5);

    if (engine_open(&e)) goto done;
    if (mkdir(outdir, 0755) && errno != EEXIST) { perror(outdir); goto done; }

    for (i = 0; i < e.bk.count; i++) {
        char    path[PATH_MAX], safe[BANK_NAME_MAX], *q;
        double *buf;
        size_t  frames;

        snprintf(safe, sizeof safe, "%s", e.bk.prog[i].name);
        for (q = safe; *q; q++) if (*q == '/' || *q == ' ') *q = '_';
        snprintf(path, sizeof path, "%s/%02d_%s.wav", outdir, i, safe);

        if (!(buf = render_program(&e, &e.bk.prog[i], &note, 1, gate, len, &frames))) continue;
        if (!wav_write_stereo16(path, buf, frames, SR))
            printf("  %-20s -> %s\n", e.bk.prog[i].name, path);
        free(buf);
    }
    printf("\n%d programs -> %s\n", e.bk.count, outdir);
    rc = 0;
done:
    engine_close(&e);
    return rc;
}

#ifdef DW_HAVE_ALSA

/* One preset, straight to the speakers. */
static int cmd_play(const char *sel)
{
    engine  e;
    int     n, note = env_note(), rc = 1;
    double  gate = env_double("GATE", 1.5), len = env_double("LEN", 3.5);
    double *buf;
    size_t  frames;

    if (engine_open(&e)) goto done;
    if ((n = bank_find(&e.bk, sel)) < 0) {
        fprintf(stderr, "dw: no program matching '%s'\n", sel);
        goto done;
    }
    printf("program %d: %s\n", n, e.bk.prog[n].name);
    if (!(buf = render_program(&e, &e.bk.prog[n], &note, 1, gate, len, &frames))) goto done;
    rc = dwplay_pcm(buf, frames, SR) ? 1 : 0;
    free(buf);
done:
    engine_close(&e);
    return rc;
}

/* A spread of the bank: pads, keys, bass, lead, percussion, effects. */
static int cmd_demo(void)
{
    static const struct { const char *sel; int note[4]; } tour[] = {
        { "STRINGS-1",        { 48, 55, 60, 64 } },
        { "CHORUSED E.PIANO", { 52, 55, 59, 64 } },
        { "BRASS DELAY",      { 48, 52, 55, -1 } },
        { "SLAP BASS",        { 36, -1, -1, -1 } },
        { "MODULAR LEAD",     { 67, -1, -1, -1 } },
        { "BELL SE 1",        { 72, 79, -1, -1 } },
        { "CX-3",             { 48, 55, 60, -1 } },
        { "DISINTEGRATOR",    { 45, -1, -1, -1 } },
    };
    engine e;
    size_t k;
    int    rc = 1;

    if (engine_open(&e)) goto done;
    printf("bank %s -- press Ctrl-C to stop\n", getenv("BANK") ? getenv("BANK") : "A");

    for (k = 0; k < sizeof tour / sizeof tour[0]; k++) {
        int     notes[4], nnotes = 0, i, n;
        double *buf;
        size_t  frames;

        if ((n = bank_find(&e.bk, tour[k].sel)) < 0) continue;
        for (i = 0; i < 4; i++)
            if (tour[k].note[i] >= 0) notes[nnotes++] = tour[k].note[i];

        printf("  %s\n", e.bk.prog[n].name);
        fflush(stdout);
        if (!(buf = render_program(&e, &e.bk.prog[n], notes, nnotes, 2.0, 4.0, &frames))) continue;
        dwplay_pcm(buf, frames, SR);
        free(buf);
    }
    printf("done.\n");
    rc = 0;
done:
    engine_close(&e);
    return rc;
}

/* The live keyboard: MIDI in, computer keyboard, or both. */
static int cmd_live(const char *sel)
{
    engine      e;
    dwplay_opts o;
    int         rc = 1;

    /* Only the blobs: dwplay_run builds its own tables from them. */
    if (engine_load(&e)) goto done;
    memset(&o, 0, sizeof o);
    if (sel && *sel) {
        int n = bank_find(&e.bk, sel);
        if (n < 0) { fprintf(stderr, "dw: no program matching '%s'\n", sel); goto done; }
        o.program = n;
    }
    o.wavedst       = e.wraw;
    o.wavedst_size  = e.wsize;
    o.proginit      = e.braw;
    o.proginit_size = e.bsize;
    o.proginit_name = e.bank_from;
    rc = dwplay_run(&o);
done:
    engine_close(&e);
    return rc;
}

#else   /* no ALSA at build time */

static int no_audio(const char *what)
{
    fprintf(stderr, "dw: `%s` needs audio, and this build has none -- ALSA was "
                    "missing when it was compiled.\n"
                    "    Install alsa-lib and rebuild (`dw build`, or make -C c).\n", what);
    return 1;
}
static int cmd_play(const char *sel) { (void)sel; return no_audio("play"); }
static int cmd_demo(void)            { return no_audio("demo"); }
static int cmd_live(const char *sel) { (void)sel; return no_audio("live"); }

#endif

static void usage(void)
{
    printf(
"dw -- the DW-8000 engine and the native plug-in hosts, in one program.\n"
"\n"
"  dw                      open a window -- pestudio if this box has Qt6,\n"
"                          dwstudio if it has GTK4, the one matching the\n"
"                          desktop if it has both\n"
"  dw --qt / dw --gtk      force one or the other (so does DW_GUI=qt|gtk)\n"
"\n"
"  dw live                 play live (MIDI controller and/or computer keyboard)\n"
"  dw play <preset>        play one preset, by index or part of its name\n"
"  dw demo                 a short tour of the factory presets\n"
"  dw list                 list the current bank's programs\n"
"  dw render <dir>         render the whole bank to <dir>\n"
"  dw keys                 the note map, and what this terminal does with a\n"
"                          held key\n"
"\n"
"  dw gui                  GTK4 window: instruments, patches, drums, Juno panel\n"
"  dw pe [dir|bank.json]   Qt6 window: load and play real plug-ins natively,\n"
"                          no Wine -- Windows VST2 and VST3 at both widths,\n"
"                          native Linux VST3, and each plug-in's own GUI\n"
"  dw peload <plug>        the same hosts from the command line: --params,\n"
"                          --render out.wav, --patch/--pick, --detect, --as\n"
"  dw peload32 <plug>      the i386 loader, for 32-bit Windows builds:\n"
"                          --render, --play, --editor\n"
"  dw build                rebuild everything, both windows included\n"
"\n"
"Bank: BANK=A (default), B, or 6000.  Note and timing: NOTE, GATE, LEN.\n"
"The wavetable and banks are read out of the plug-in itself; set FB7999 to\n"
"point at a copy somewhere other than vst/windows/VST2-64/fb799964.dll.\n");
}

int main(int argc, char **argv)
{
    const char *cmd;
    char        dir[PATH_MAX];

    if (locate_tree()) return 1;

    /* Everything after the subcommand belongs to the subcommand. */
    if (argc > 1) { cmd = argv[1]; argv += 2; argc -= 2; }
    else          { cmd = NULL;    argv += 1; argc  = 0; }

    /* --qt and --gtk name a window instead of a subcommand, so the choice can
     * be forced without having to remember which program is which toolkit. */
    if (cmd && (!strcmp(cmd, "--qt")  || !strcmp(cmd, "-qt")))  cmd = "pe";
    if (cmd && (!strcmp(cmd, "--gtk") || !strcmp(cmd, "-gtk"))) cmd = "gui";

    /* Nothing named: open a window, and work out which. */
    if (!cmd) {
        const char *why = "";
        switch (pick_gui(&why)) {
        case GUI_QT:  cmd = "pe";  break;
        case GUI_GTK: cmd = "gui"; break;
        default:
            fprintf(stderr,
                "dw: no GUI available -- pestudio needs Qt6, dwstudio needs GTK4,\n"
                "    and neither is installed. The command line still works:\n"
                "    `dw list`, `dw play <preset>`, `dw render <dir>`, `dw live`.\n");
            return 1;
        }
        fprintf(stderr, "dw: opening %s (%s; --qt / --gtk to choose, `dw help` "
                        "for the rest)\n",
                !strcmp(cmd, "pe") ? "pestudio" : "dwstudio", why);
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        usage();
        return 0;
    }
    if (!strcmp(cmd, "live"))   return cmd_live(argc > 0 ? argv[0] : NULL);
    if (!strcmp(cmd, "play"))   return cmd_play(argc > 0 ? argv[0] : "0");
    if (!strcmp(cmd, "demo"))   return cmd_demo();
    if (!strcmp(cmd, "list"))   return cmd_list();
    if (!strcmp(cmd, "render")) {
        if (argc < 1) { fprintf(stderr, "usage: dw render <dir>\n"); return 2; }
        return cmd_render(argv[0]);
    }
    if (!strcmp(cmd, "keys")) {
#ifdef DW_HAVE_ALSA
        return dwplay_keyboard_info();
#else
        return no_audio("keys");
#endif
    }

    if (!strcmp(cmd, "gui")) {
        const char *backend = getenv("BACKEND");
        snprintf(dir, sizeof dir, "%s/gui", g_re);
        /* A native Linux plug-in's editor is an X11 window, and GTK hands out
         * an X11 id only on the X11 backend -- under Wayland there is nothing
         * to give the plug-in and its editor cannot open at all. Same reason
         * the Qt window is asked for xcb below. Override GDK_BACKEND if you
         * would rather have a native Wayland window and no native editors. */
        setenv("GDK_BACKEND", "x11", 0);   /* 0: only if unset */
        return exec_tool(dir, "dwstudio", argc, argv,
                         backend && *backend ? "--backend" : NULL, backend);
    }
    if (!strcmp(cmd, "pe") || !strcmp(cmd, "pestudio")) {
        snprintf(dir, sizeof dir, "%s/peload", g_re);
        /* Plug-in editors embed through an X11 window id, so Qt has to be on
         * xcb; under Wayland that means XWayland. Override QT_QPA_PLATFORM if
         * you only want the parameter list and prefer a native window. */
        setenv("QT_QPA_PLATFORM", "xcb", 0);   /* 0: only if unset */
        return exec_tool(dir, "pestudio", argc, argv, NULL, NULL);
    }
    if (!strcmp(cmd, "peload") || !strcmp(cmd, "peload32")) {
        int is32 = !strcmp(cmd, "peload32");
        const char *sub = is32 ? "windows/VST2-32" : "windows/VST2-64";
        snprintf(dir, sizeof dir, "%s/peload", g_re);
        if (argc == 0) {
            printf("usage: dw %s <plugin> [options] -- run `dw %s --help` for the "
                   "full list\n", cmd, cmd);
            list_corpus(sub);
            return 0;
        }
        expand_plugin_names(argc, argv, sub);
        return exec_tool(dir, cmd, argc, argv, NULL, NULL);
    }
    if (!strcmp(cmd, "build")) {
        char cdir[PATH_MAX], gdir[PATH_MAX];
        char *mk[] = { "make", "-C", cdir, "--no-print-directory", NULL };
        int   rc, failed = 0;

        if (g_installed) {
            fprintf(stderr, "dw: this is an installed copy -- there are no "
                            "sources here to build. Build from a checkout of "
                            "the tree instead.\n");
            return 1;
        }

        snprintf(cdir, sizeof cdir, "%s/c", g_re);
        if ((rc = run(mk))) return rc;

        /* Both windows, not just the Qt one. Whichever toolkit is missing
         * fails its own build and is reported; the other still gets built,
         * which is the point of doing them separately. */
        snprintf(dir,  sizeof dir,  "%s/peload", g_re);
        snprintf(gdir, sizeof gdir, "%s/gui", g_re);
        if (cmake_build(dir, "all"))       { fprintf(stderr, "dw: peload failed to build\n");   failed = 1; }
        if (cmake_build(gdir, "dwstudio")) { fprintf(stderr, "dw: dwstudio failed to build\n"); failed = 1; }
        return failed;
    }

    fprintf(stderr, "dw: unknown command '%s'\n\n", cmd);
    usage();
    return 2;
}
