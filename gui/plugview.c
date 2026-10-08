/* See plugview.h. */

/* realpath, readlink: _GNU_SOURCE rather than _POSIX_C_SOURCE, which is
 * what the rest of the tree uses and what dwstudio.c relies on implicitly. */
#define _GNU_SOURCE

#include "plugview.h"

#include "pehost.h"
#include "vstdirs.h"
#include "patch.h"
#include "vst3.h"

/* gdk_x11_display_get_xdisplay and gdk_x11_surface_get_xid are marked
 * deprecated in current GTK, with no replacement that gives an X11 id -- which
 * is the one thing a native plug-in editor needs. Silenced here rather than
 * file-wide so a real deprecation still shows up. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <gdk/x11/gdkx.h>
#pragma GCC diagnostic pop
#include <glib-unix.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <time.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PLUGINS 1024
#define MAX_ROOTS   24

/* macOS hosting, on in both windows so they browse the same set -- see the
 * longer note at the top of qtgui/main.cpp for what "on" is worth measured
 * against the corpus. -DPLUGVIEW_MAC=0 takes .vst and .component bundles back
 * out of the browser; the loaders stay compiled in either way, and `va peload`
 * on the command line has always reached them. */
#ifndef PLUGVIEW_MAC
#define PLUGVIEW_MAC 1
#endif

/* Audio Units, separately from the rest of the macOS support and off -- every
 * .component here is the same plug-in as the .vst beside it. The loader stays
 * compiled in: naming one on the command line still works, and
 * -DPLUGVIEW_AU=1 puts them back in the scan. */
#ifndef PLUGVIEW_AU
#define PLUGVIEW_AU 0
#endif

/* Classic Mac OS, separately: a .vstclassic loads, renders and draws its own
 * editor through the CFM/PEF loader and the PowerPC interpreter.
 * -DPLUGVIEW_CLASSIC=0 drops it. */
#ifndef PLUGVIEW_CLASSIC
#define PLUGVIEW_CLASSIC 1
#endif

/* One entry in the browser. `kind` is what pehost decided the file is, kept so
 * the list can say "VST3" or "32-bit bridge" next to the name -- which is the
 * difference between "this will not load" and "this loads through a helper". */
typedef struct {
    char path[1024];
    char name[128];
    char kind[128];           /* what it is, or why it cannot be loaded */
    int  loadable;
    /* Data the plug-in needs and has not got -- a firmware ROM, the artwork its
     * editor draws with. It loads and opens an editor either way, so without
     * this the browser reported a clean load for a synth that cannot make a
     * sound. See pehost_data_check. */
    char warn[160];
    int  repairable;          /* the missing data exists and can be linked in */
    /* What pehost_classify made of it, kept so the two selectors can sift the
     * list without sniffing anything again. `os` is the authority the OS
     * selector sorts and filters on -- a .vst3 is a Windows plug-in or a Linux
     * one depending on the binary inside it, which no extension reveals. */
    char os[16];              /* "windows" | "linux" | "macos" | "classic" */
    char fmt[8];              /* "VST2" | "VST3" | "AU" */
    int  kindv;               /* pehost_kind, ordering formats within a platform */
    vstdirs_id id;            /* which file it is, for spotting it found twice */
} entry;

/* One entry in the platform dropdown: a corpus directory and what to call it. */
typedef struct { char label[64]; char path[1024]; } proot;

#define MAX_WATCH 128

/* One run-loop registration a native editor has made -- an X11 descriptor or
 * a timer. See the longer note where they are dispatched, below. */
typedef struct {
    void    *handler;
    int      fd;              /* -1 for a timer */
    GSource *src;
} watch;

/* One plug-in pane, whole: what it browses, what it has loaded, its editor.
 * plugview_new makes one per plug-in window; every function below takes it as
 * its first parameter. Nothing in this file is shared between instances. */
struct plugview {
    GtkWidget *root;
    /* What to browse, in the two terms a plug-in actually has: the format it
     * is and the platform it was built for. These replace a dropdown that
     * named directories -- one entry per corpus, plus every folder the user
     * had ever added -- which made the tree layout the user's problem and
     * offered "VST2" once per directory holding one. Every root is scanned
     * now, once, and these two sift the result. pestudio has the same pair,
     * in the same order, for the same reason. */
    GtkWidget *filterrows[2]; /* the Type: and OS: rows, packed into the pane */
    GtkWidget *typedd;        /* All types | VST2 | VST3 | AU */
    GtkWidget *osdd;          /* All platforms | Windows | Linux | macOS */
    GtkStringList *typemodel;
    GtkStringList *osmodel;
    char   types[8][8];       /* the formats this scan actually found */
    int    ntypes;
    char   oses[8][16];       /* and the platforms, in display order */
    int    noses;
    proot  roots[MAX_ROOTS];
    int    nroot;
    GtkWidget *dirlabel;
    GtkWidget *list;          /* plug-ins */
    GtkWidget *search;        /* keyword search over the plug-in list */
    GtkWidget *proglist;      /* the loaded plug-in's programs */
    GtkWidget *paramlist;
    /* What is open, so a saved patch can name the plug-in it came from and be
     * opened later without naming it again. */
    char       loaded_path[1024];
    GtkWidget *paramsw;
    GtkWidget *editor;        /* GtkDrawingArea fed by pehost_editor_pixels */
    GtkWidget *editorsw;
    GtkWidget *editorpage;    /* the zoom bar and editorsw together */
    GtkWidget *stack;         /* Parameters | Editor */
    GtkWidget *header;
    GtkWidget *status;

    entry  plug[MAX_PLUGINS];
    int    nplug;
    /* Which of them the two selectors let through, as indices into plug[].
     * The list box shows these and nothing else, so a row index is not a
     * plug[] index any more -- everything that starts from a clicked row goes
     * through here. */
    int    vis[MAX_PLUGINS];
    int    nvis;
    char   dir[1024];         /* where the selection came from -- a display */

    /* The loaded plug-in. Written only from the GTK thread, and only while the
     * audio callback is parked -- see load(). The audio callback reads it
     * through plugview_active()/plugview_render(). */
    pehost *host;
    _Atomic int live;         /* host != NULL, readable from the audio thread */

    void  (*park)(void);
    void  (*unpark)(void);
    double rate;
    int    block;

    /* The X11 editor: a window of its own, and the run-loop registrations the
     * plug-in makes while it is up. */
    /* A bare X11 window, not a GtkWindow. GTK owns the drawing of every
     * surface it creates and would repaint over the plug-in continuously --
     * which is exactly what a GtkWindow did: Cardinal brought up its GL
     * context on the id it was given and GTK cleared it every frame. Qt can
     * lend a native widget that never paints; GTK cannot, so this window is
     * made with Xlib and GTK never learns about it. */
    /* The plug-in's editor lives in the Editor page, not in a window of its
     * own: an X11 child of this window's toplevel, moved and sized to sit over
     * the pane's editor widget. GTK4 hands out an X11 id only for a toplevel,
     * so the child has to be made with Xlib and placed by hand -- but it is
     * placed inside our window, which is where a plug-in editor belongs and
     * where pestudio puts its own. */
    /* Two windows, not one. `ed_xwin` is the whole editor at its natural size
     * -- that is what the plug-in is given and what it lays itself out in --
     * and `ed_clip` is a window the size of the visible pane that it is a child
     * of. X clips a window to its parent, so an editor bigger than the pane is
     * cut off at the pane's edge instead of painted over the plug-in list, the
     * menu bar and the status line, and scrolling is `ed_xwin` moving to a
     * negative offset inside `ed_clip`. Handing the plug-in a window that was
     * only as big as the pane would clip it just as well, but there would be no
     * way to reach the rest of it: the window the plug-in draws into is its
     * own, and the host cannot move it. */
    Window     ed_clip;
    Window     ed_xwin;
    unsigned long ed_xid;
    pehost    *ed_attached;   /* what is currently embedded, or NULL */
    GdkRectangle ed_clip_at, ed_plug_at;   /* where they were last put */
    int        ed_shown;      /* ed_clip is mapped */
    /* How big the plug-in says its editor is, which is not the same as how big
     * the pane is and must not be confused with it -- see editor_bounds. */
    int        ed_nat_w, ed_nat_h;
    /* The size the plug-in actually drew at, before the zoom. ed_nat_* is that
     * multiplied by ed_zoom, and is what the pane and the plug-in's window are
     * sized from; this is what the multiplication starts from, so that zooming
     * twice does not compound. */
    int        ed_base_w, ed_base_h;
    double     ed_zoom;
    /* An automatic fit that has not managed to measure the pane yet, and
     * whether the fit is still in charge. See zoom_fit_idle. */
    int        ed_fit_pending;
    int        ed_fit_auto;
    guint      ed_fit_idle;     /* the queued zoom_fit_idle, removed on shutdown */
    int        ed_fit_vw, ed_fit_vh;   /* pane size the last fit measured */
    GtkWidget *zoom_out, *zoom_in, *zoom_fit, *zoom_one, *zoom_lbl, *zoom_note;
    int        dead_reported;   /* said once, not once per tick */

    int        ed_native;     /* the loaded plug-in wants an X11 window */
    int        ready;         /* the pane is built; page changes are real now */

    /* Editor state. `ed_open` is the one that matters: pehost_editor_pump and
     * pehost_editor_pixels are only meaningful after pehost_editor_open has
     * succeeded, and calling them on a plug-in that has no editor -- or whose
     * editor was refused -- crashes inside guest code. */
    guint  tick;
    guint  meter;
    int    ed_open;
    int    ed_w, ed_h;
    int    buttons;           /* MK_* mask, tracked across GTK's separate events */
    int    loading;           /* suppress parameter callbacks while repopulating */

    /* Output level, written by the audio thread and read by the GTK one.
     * Scaled to an int because that is what can be stored atomically without a
     * lock, and a meter does not need more resolution than a thousandth. */
    _Atomic int peak_milli;
    char        loaded_msg[512];   /* the status line the level is appended to */

    /* A --dir, or the folder a plug-in was opened out of. Remembered across
     * the roots_discover(pv) calls that reset the list, which happen after this
     * is set: plugview_scan runs before the pane exists and roots_discover
     * runs when it is built, so without this the folder the user actually
     * asked for was the one folder dropped from the scan. */
    char   session_dir[1024];

    /* The run-loop registrations the loaded plug-in's native editor has made.
     * The hook table they arrive through is pehost's and is process-global --
     * see the note in plugview.h. */
    watch  watches[MAX_WATCH];

    /* Every entry into a plug-in from the GTK thread raises this, and anything
     * that could be re-entered checks it. The same guard pestudio carries, for
     * the same reason: a plug-in's own modal drag loop pumps the host's event
     * queue, so a click on the plug-in list can be delivered while that
     * plug-in is still running -- and loading another one there frees what is
     * currently executing. */
    int    in_plugin;

    /* Which computer keys the piano claims, as the window answers it. See
     * plugview_set_note_key. */
    int  (*note_key)(guint keyval);
    /* What the window wants re-sending after every load -- see
     * plugview_set_load_hook. */
    void (*load_hook)(struct plugview *pv);

    /* MIDI from an in-process sequencer, wall-clock stamped -- see
     * plugview_inject_midi. The delivery thread produces, the audio thread
     * drains at the top of plugview_render_io. */
#define PV_INJQ 1024
    struct { double wall; unsigned char st, d1, d2; } inj[PV_INJQ];
    _Atomic unsigned inj_head, inj_tail;
    _Atomic unsigned long inj_in, inj_placed;

    int    meter_shown;   /* the level the meter last drew, for its decay */

    /* --cycle: where the unattended walk over the whole list has got to. */
    int    cycle_ms, cycle_at, cycle_report;
    int    unpacking;          /* an installer is being unpacked, off the main loop */
    guint  cycle_src;          /* the --cycle timeout, so shutdown can remove it */

    /* Settings > Plug-in Folders, while the dialog is open. */
    struct {
        GtkWidget *win;
        GtkWidget *list;          /* the folders, grouped */
        GtkWidget *rm;
        GtkWidget *asdd;          /* which group Add puts the next one in */
        char       sel[1024];     /* the selected folder, or "" */
    } folders;
};

/* --------------------------------------------------------- platform roots */

static int is_dir(const char *p)
{ struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

/* The plug-in corpora this tree carries, one per platform and format.
 *
 * Found by walking up from the executable, the same way pestudio's root
 * selector does and for the same reason: the window should open on whatever
 * the checkout actually has rather than on a path compiled in. Only the ones
 * that exist are listed, so a tree without the macOS downloads simply does not
 * offer them. */
static void roots_discover(plugview *pv)
{
    static const struct { const char *label, *rel; } cand[] = {
        { "Windows VST2 64-bit", "windows/VST2-64" },
        { "Windows VST3",        "windows/VST3"    },
        { "Windows VST2 32-bit", "windows/VST2-32" },
        { "Linux native",        "linux/extracted" },
#if PLUGVIEW_CLASSIC
        { "Mac OS 9 (Classic)",  "macos/classic"   },
#endif
#if PLUGVIEW_MAC
        { "macOS VST2",          "macos/VST2"      },
        { "macOS VST3",          "macos/VST3"      },
        { "macOS Audio Units",   "macos/AU"        },
#endif
    };
    char    exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char   *slash;
    int     up, i, j;

    pv->nroot = 0;
    if (n <= 0) return;   /* roots_add_standard still applies */
    exe[n] = 0;

    for (up = 0; up < 6; up++) {
        if (!(slash = strrchr(exe, '/'))) break;
        *slash = 0;
        if (!exe[0]) break;
        for (i = 0; i < (int)(sizeof cand / sizeof cand[0]); i++) {
            char path[1024];
            int  dup = 0;
            snprintf(path, sizeof path, "%s/%s", exe, cand[i].rel);
            if (!is_dir(path)) continue;
            for (j = 0; j < pv->nroot; j++) if (!strcmp(pv->roots[j].path, path)) dup = 1;
            if (dup || pv->nroot >= MAX_ROOTS) continue;
            snprintf(pv->roots[pv->nroot].label, sizeof pv->roots[0].label, "%s", cand[i].label);
            snprintf(pv->roots[pv->nroot].path,  sizeof pv->roots[0].path,  "%s", path);
            pv->nroot++;
        }
    }
}

/* Add a directory that is not one of the built-in corpora -- a Downloads
 * folder, a system VST path -- and say where it came from. Returns its index,
 * or the existing one if it is already listed. */
static int roots_add(plugview *pv, const char *path, const char *label)
{
    /* PATH_MAX, not a size of our choosing: realpath() is specified to write up
     * to that much, and glibc's fortified form checks the buffer against it
     * before resolving anything -- so a smaller one aborts the process on the
     * first call, whatever the path turns out to be. */
    char lbl[64], real[PATH_MAX];
    int  i;

    if (realpath(path, real)) path = real;
    for (i = 0; i < pv->nroot; i++) if (!strcmp(pv->roots[i].path, path)) return i;
    if (pv->nroot >= MAX_ROOTS) return -1;
    snprintf(lbl, sizeof lbl, "%s", label);
    snprintf(pv->roots[pv->nroot].label, sizeof pv->roots[0].label, "%s", lbl);
    snprintf(pv->roots[pv->nroot].path,  sizeof pv->roots[0].path,  "%s", path);
    return pv->nroot++;
}

/* Where a Linux system actually keeps plug-ins.
 *
 * The corpora above are found by walking up from the executable, which is
 * exactly right in a checkout and finds nothing at all once this is installed:
 * from /usr/lib/vst-ace the walk reaches /windows/VST2-64, which no machine
 * has. Opening the browser from the desktop menu therefore showed an empty
 * list and, in dwstudio's case, printed "no such directory" at a path the user
 * had never mentioned. These are the conventional locations, plus whatever
 * VST_PATH and VST3_PATH name, and only the ones that exist are offered. */
static void roots_add_standard(plugview *pv)
{
    static const struct { const char *label, *fmt; int home; } cand[] = {
        { "VST2 (yours)",  "%s/.vst",                        1 },
        { "VST3 (yours)",  "%s/.vst3",                       1 },
        { "VST2 (system)", "/usr/lib/vst",                   0 },
        { "VST3 (system)", "/usr/lib/vst3",                  0 },
        { "VST2 (local)",  "/usr/local/lib/vst",             0 },
        { "VST3 (local)",  "/usr/local/lib/vst3",            0 },
        { "VST2 (system)", "/usr/lib/x86_64-linux-gnu/vst",  0 },
        { "VST3 (system)", "/usr/lib/x86_64-linux-gnu/vst3", 0 },
    };
    const char *home = getenv("HOME");
    int i;

    for (i = 0; i < (int)(sizeof cand / sizeof cand[0]); i++) {
        char path[1024];
        if (cand[i].home) {
            if (!home || !*home) continue;
            snprintf(path, sizeof path, cand[i].fmt, home);
        } else {
            snprintf(path, sizeof path, "%s", cand[i].fmt);
        }
        if (is_dir(path)) roots_add(pv, path, cand[i].label);
    }

    /* VST_PATH and VST3_PATH are colon-separated, like PATH. */
    {
        static const char *const vars[] = { "VST_PATH", "VST3_PATH", NULL };
        int v;
        for (v = 0; vars[v]; v++) {
            const char *e = getenv(vars[v]), *p;
            if (!e || !*e) continue;
            for (p = e; *p; ) {
                const char *sep = strchr(p, ':');
                size_t len = sep ? (size_t)(sep - p) : strlen(p);
                char path[1024];
                if (len && len < sizeof path) {
                    memcpy(path, p, len);
                    path[len] = 0;
                    if (is_dir(path)) roots_add(pv, path, vars[v]);
                }
                if (!sep) break;
                p = sep + 1;
            }
        }
    }
}


/* The selector must never be empty: it names the directory being browsed, and
 * a blank one on a machine with no corpora and no system VST folders left the
 * window showing plug-ins from a directory it would not admit to. The default
 * falls back to the home folder, so the selector has to be able to say so. */
static void roots_add_fallback(plugview *pv)
{
    const char *home;
    if (pv->nroot > 0) return;
    home = getenv("HOME");
    if (home && *home && is_dir(home)) roots_add(pv, home, "Home folder");
}

/* The folders the user set, from the file pestudio writes too -- one list per
 * machine, not one per window. See vstdirs.h. Added after the discovered
 * corpora so a folder that is already one of them is deduped away by
 * roots_add rather than scanned twice. */
static void roots_add_user(plugview *pv)
{
    vstdir dirs[VSTDIRS_MAX];
    int    n = vstdirs_load(dirs, VSTDIRS_MAX), i;

    /* Labelled with the platform the user filed it under, which is what the
     * settings list shows. It does not decide what is in it: every plug-in is
     * sniffed on the way into the list. */
    for (i = 0; i < n; i++)
        if (is_dir(dirs[i].path))
            roots_add(pv, dirs[i].path, vstdirs_os_label(dirs[i].os));
}

/* The directory to open on, for a caller that was given none. The checkout's
 * own corpus first so a developer's window opens where it always did, then the
 * system locations, and the home directory only if nothing else exists --
 * which at least browses somewhere real. */
const char *plugview_default_dir(plugview *pv)
{
    static char out[1024];
    const char *home;

    roots_discover(pv);
    roots_add_standard(pv);
    roots_add_fallback(pv);
    if (pv->nroot > 0) {
        snprintf(out, sizeof out, "%s", pv->roots[0].path);
        return out;
    }
    home = getenv("HOME");
    snprintf(out, sizeof out, "%s", (home && *home) ? home : ".");
    return out;
}

/* ------------------------------------------------------------- the browser */

/* Everything that has something to say says it here.
 *
 * The level meter rewrites this label ten times a second, so a message written
 * straight to the widget survived for one tick and vanished -- which is how a
 * refused plug-in looked like nothing happening at all. The text is kept, and
 * the meter appends to it. */
static void plug_status(plugview *pv, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(pv->loaded_msg, sizeof pv->loaded_msg, fmt, ap);
    va_end(ap);
    if (pv->status) gtk_label_set_text(GTK_LABEL(pv->status), pv->loaded_msg);
}

/* Platform first, then format within it, then name.
 *
 * Grouping by platform is what makes one list of every folder readable -- the
 * Windows builds together, the native ones together -- and it is the order the
 * OS selector narrows to when one platform is picked out of it. Identical to
 * pestudio's, so the two windows list a corpus the same way round. */
static int os_rank(const char *os)
{
    if (!strcmp(os, "windows")) return 0;
    if (!strcmp(os, "linux"))   return 1;
    if (!strcmp(os, "macos"))   return 2;
    if (!strcmp(os, "classic")) return 3;
    return 4;                 /* nothing placed it -- last, with its reason */
}

/* "windows" -> "Windows". The label a person reads, from the tag pehost
 * writes. */
static const char *os_label(const char *os)
{
    if (!strcmp(os, "windows")) return "Windows";
    if (!strcmp(os, "linux"))   return "Linux";
    if (!strcmp(os, "macos"))   return "macOS";
    if (!strcmp(os, "classic")) return "Mac OS 9";
    return "Unrecognised";
}

/* A format for the type selector. Empty when pehost could not place the file,
 * which still has to be selectable -- it is in the list with its reason. */
static const char *fmt_of(const entry *e)
{ return e->fmt[0] ? e->fmt : "Unrecognised"; }

static int entry_cmp(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    int c = g_ascii_strcasecmp(x->name, y->name);

    /* Alphabetical by name, whatever the platform or format: the two selectors
     * narrow the list, and a plug-in added later lands where its name belongs. */
    return c ? c : g_ascii_strcasecmp(x->path, y->path);
}

/* Is this entry a plug-in worth listing, and if so what shape?
 *
 * The tests are the ones pestudio uses, and they are not interchangeable: a
 * VST3 is a bundle directory *or* a bare file, a macOS plug-in is always a
 * directory, a Classic one is a plain file with no extension worth trusting,
 * and a native Linux VST2 is a bare .so -- which means an ELF check alone
 * would match every support library in the tree. Only the export settles that
 * last one, and the .lv2 skip keeps us from dlopen'ing an LV2 bundle's inner
 * library just to find out it is not a candidate. */
static int is_candidate(const char *path, const char *name, int isdir)
{
    size_t l = strlen(name);

    if (l > 5 && !strcasecmp(name + l - 5, ".vst3")) return 1;      /* file or dir */
    if (isdir) {
#if PLUGVIEW_MAC
        if (l > 4  && !strcasecmp(name + l - 4,  ".vst"))       return 1;
#endif
#if PLUGVIEW_AU
        if (l > 10 && !strcasecmp(name + l - 10, ".component")) return 1;
#endif
        return 0;
    }
    /* The export, not the extension. A .dll is only a Windows binary; an
     * installer's setup.dll satisfied this test and was listed beside the
     * synthesisers, then failed when picked. Same reasoning as the .so case
     * below, which has always checked. */
    if (l > 4 && !strcasecmp(name + l - 4, ".dll")) return pehost_is_windows_vst(path);
    if (l > 3 && !strcasecmp(name + l - 3, ".so"))
        return !strstr(path, ".lv2/") && pehost_is_native_vst2(path);
#if PLUGVIEW_CLASSIC
    /* Only files nothing else claimed, and only ones the size a plug-in of this
     * era can be -- the header test opens the file, and a Classic plug-in has no
     * extension convention worth trusting, so without a bound this would sniff
     * every stray file in a scanned tree. Nothing from 2002 is over 32 MB. */
    {   struct stat st;
        if (stat(path, &st) || st.st_size <= 128 || st.st_size >= 32 * 1024 * 1024)
            return 0;
    }
    return pehost_is_classic_mac(path);
#else
    return 0;
#endif
}

/* Walk the tree, not just the top of it.
 *
 * A flat listing found the Windows corpora, where every plug-in is a file in
 * one directory, and found nothing at all under linux/extracted, where each
 * one arrives as its own unpacked release with the plug-in several levels
 * down. */
/* One plug-in found on disk: skipped when it is hidden or already listed,
 * otherwise classified and appended to plug[]. Shared by the folder walk and
 * by the plug-ins added by file for this session. */
static void add_candidate(plugview *pv, const char *path, const char *nm)
{
    entry *e;
    pehost_info info;

    if (vstdirs_is_hidden(path)) return;       /* taken off the list: File > Plug-ins */
    /* A candidate by shape still has to be one this host can run --
     * a 32-bit build without the helper, a PowerPC Mach-O. Listed
     * either way, with the reason when it cannot: a plug-in that is
     * simply absent from the list looks like one the scan failed to
     * find, and "why is it not there" is a worse question than "why
     * will it not load". pestudio does the same. */
    {   /* Folders overlap -- a system VST directory can sit inside a
         * corpus, the user can add one that is already scanned, a
         * VST_PATH folder can be symlinks into one, and ~/.vst can hold
         * a copy of what a corpus has. The same plug-in is listed once,
         * wherever it was found first. */
        vstdirs_id id;
        int k, seen = 0;
        vstdirs_identify(path, &id);
        for (k = 0; k < pv->nplug; k++)
            if (!strcmp(pv->plug[k].path, path) ||
                vstdirs_same_plugin(path, &id, pv->plug[k].path, &pv->plug[k].id))
                { seen = 1; break; }
        if (seen) return;
        pv->plug[pv->nplug].id = id;
    }
    e = &pv->plug[pv->nplug++];
    snprintf(e->path, sizeof e->path, "%s", path);
    snprintf(e->name, sizeof e->name, "%s", nm);
    /* One verdict, not two. pehost_classify already reports whether
     * this build can run the file and why not, and it knows things
     * pehost_can_load does not -- that a macOS VST3 bundle is a VST3
     * that is simply not hosted yet, rather than "not a PE, ELF or
     * Mach-O image". Asking both sniffed every candidate twice and then
     * showed the less informed of the two answers. */
    pehost_classify(path, &info);
    e->loadable = info.loadable;
    /* The platform and format the two selectors sort and sift on. From
     * the binary, every time -- which is why a .vst3 can come out
     * Windows here and Linux three rows down. */
    snprintf(e->os,  sizeof e->os,  "%s", info.os);
    snprintf(e->fmt, sizeof e->fmt, "%s", info.format);
    e->kindv = (int)info.kind;
    {   /* Directory reads only -- nothing is loaded to find this out. */
        pehost_data_need dn;
        if (pehost_data_check(path, &dn)) {
            snprintf(e->warn, sizeof e->warn, "%s", dn.need);
            e->repairable = dn.repairable;
        }
    }
    snprintf(e->kind, sizeof e->kind, "%s",
             info.loadable  ? pehost_kind_label(info.kind)
             : info.why[0]  ? info.why
                            : "unsupported");
}

/* Plug-ins added by file for this run only (Add plug-in, "this session only"):
 * one list for every tab, so each tab's scan finds them. */
static char g_session_files[64][1024];
static int  g_nsession_files;

/* Walk one folder, appending what it holds to plug[]. Split out of
 * plugview_scan because there are several folders now and each is walked the
 * same way -- and because the caller, not this, decides when the list starts
 * over. */
static void scan_tree(plugview *pv, const char *dir)
{
    char   queue[512][1024];
    int    head = 0, tail = 0, visited = 0;
    char   real[PATH_MAX];        /* realpath()'s size, not ours -- see above */

    if (!dir || !*dir) return;

    /* Resolved, not as given. dwstudio builds its default from the executable
     * path and hands over something like ".../gui/build/../../../windows/
     * VST2-64" -- the same directory the dropdown discovered, but not the same
     * string, so it was listed twice and neither entry matched the other. */
    if (realpath(dir, real)) snprintf(queue[tail++], sizeof queue[0], "%s", real);
    else                     snprintf(queue[tail++], sizeof queue[0], "%s", dir);

    while (head < tail && pv->nplug < MAX_PLUGINS && visited < 512) {
        char        base[1024];
        GDir       *d;
        const char *nm;

        snprintf(base, sizeof base, "%s", queue[head++]);
        visited++;
        if (!(d = g_dir_open(base, 0, NULL))) {
            if (head == 1) fprintf(stderr, "plugview: no such directory: %s\n", base);
            continue;
        }
        while ((nm = g_dir_read_name(d)) && pv->nplug < MAX_PLUGINS) {
            char  path[1024];
            int   isdir;

            if (nm[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s", base, nm);
            isdir = is_dir(path);

            if (!is_candidate(path, nm, isdir)) {
                if (isdir && tail < (int)(sizeof queue / sizeof queue[0]))
                    snprintf(queue[tail++], sizeof queue[0], "%s", path);
                continue;
            }
            add_candidate(pv, path, nm);
        }
        g_dir_close(d);
    }
}

/* Rebuild the two selectors from what the scan actually found.
 *
 * Offered once each and only when present, so a machine with no Linux plug-ins
 * is not asked to choose between platforms it has none of, and a format spread
 * over three folders is still one entry -- which is the whole complaint about
 * the dropdown this replaced. */
static void rebuild_filters(plugview *pv)
{
    int i, j;

    pv->ntypes = pv->noses = 0;
    for (i = 0; i < pv->nplug; i++) {
        const char *f = fmt_of(&pv->plug[i]);
        for (j = 0; j < pv->ntypes; j++) if (!strcmp(pv->types[j], f)) break;
        if (j == pv->ntypes && pv->ntypes < (int)(sizeof pv->types / sizeof pv->types[0]))
            snprintf(pv->types[pv->ntypes++], sizeof pv->types[0], "%s", f);
        for (j = 0; j < pv->noses; j++) if (!strcmp(pv->oses[j], pv->plug[i].os)) break;
        if (j == pv->noses && pv->noses < (int)(sizeof pv->oses / sizeof pv->oses[0]))
            snprintf(pv->oses[pv->noses++], sizeof pv->oses[0], "%s", pv->plug[i].os);
    }
    /* plug[] is already in platform order, so the platforms come out in it
     * too; the formats are sorted by name, there being no better order. */
    for (i = 0; i < pv->ntypes; i++)
        for (j = i + 1; j < pv->ntypes; j++)
            if (g_ascii_strcasecmp(pv->types[i], pv->types[j]) > 0) {
                char t[sizeof pv->types[0]];
                memcpy(t, pv->types[i], sizeof t);
                memcpy(pv->types[i], pv->types[j], sizeof t);
                memcpy(pv->types[j], t, sizeof t);
            }

    if (!pv->typedd) return;
    /* Rebuilding a model the dropdown is watching moves its selection; both go
     * back to "All", which is the honest answer after a rescan anyway -- what
     * was selected may no longer be among the choices. */
    pv->loading = 1;
    while (g_list_model_get_n_items(G_LIST_MODEL(pv->typemodel)) > 1)
        gtk_string_list_remove(pv->typemodel, 1);
    for (i = 0; i < pv->ntypes; i++) gtk_string_list_append(pv->typemodel, pv->types[i]);
    while (g_list_model_get_n_items(G_LIST_MODEL(pv->osmodel)) > 1)
        gtk_string_list_remove(pv->osmodel, 1);
    for (i = 0; i < pv->noses; i++) gtk_string_list_append(pv->osmodel, os_label(pv->oses[i]));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(pv->typedd), 0);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(pv->osdd), 0);
    pv->loading = 0;
}

/* Every folder the browser walks: the corpora found by walking up from the
 * binary, the system VST directories, the folders the user set in Settings,
 * and anything named on the command line. The list is held whole and sifted by
 * fill_browser, so changing format or platform costs nothing -- the expensive
 * part is sniffing files, and it happens once. */
void plugview_scan(plugview *pv, const char *dir)
{
    int i;

    /* A folder handed in is somewhere to scan rather than somewhere to browse:
     * it joins the roots, and what it holds turns up in the one list. */
    if (dir && *dir) {
        snprintf(pv->session_dir, sizeof pv->session_dir, "%s", dir);
        roots_add(pv, dir, "given folder");
    }
    if (pv->nroot == 0) { roots_discover(pv); roots_add_standard(pv); roots_add_user(pv); }
    /* Called before the pane exists -- dwstudio hands over its --dir or its
     * default from main(). Remembering the folder is all that is wanted here:
     * plugview_new rebuilds the root list and scans, so walking every folder
     * now would only be to throw the result away and walk them again. */
    if (!pv->list) return;

    pv->nplug = 0;
    for (i = 0; i < pv->nroot; i++) scan_tree(pv, pv->roots[i].path);
    for (i = 0; i < g_nsession_files && pv->nplug < MAX_PLUGINS; i++) {
        const char *sl = strrchr(g_session_files[i], '/');
        if (g_file_test(g_session_files[i], G_FILE_TEST_EXISTS))
            add_candidate(pv, g_session_files[i], sl ? sl + 1 : g_session_files[i]);
    }
    qsort(pv->plug, (size_t)pv->nplug, sizeof pv->plug[0], entry_cmp);
    rebuild_filters(pv);
    /* Said out loud, the way pestudio says it, so the two windows can be
     * compared on the same corpus without reading either one's status bar. */
    fprintf(stderr, "plugview: scanned %d folder(s) -> %d plug-in(s)\n",
            pv->nroot, pv->nplug);
}

static void append_row(plugview *pv, const entry *e);

static void clear_list(GtkWidget *lb)
{
    GtkWidget *row;
    while ((row = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), 0))))
        gtk_list_box_remove(GTK_LIST_BOX(lb), row);
}

/* What the selectors are set to, or NULL for "all of them". Index 0 is the
 * "All ..." row in both, so anything else indexes the arrays the scan built. */
static const char *want_type(plugview *pv)
{
    guint i;
    if (!pv->typedd) return NULL;
    i = gtk_drop_down_get_selected(GTK_DROP_DOWN(pv->typedd));
    if (i == GTK_INVALID_LIST_POSITION || i == 0 || (int)i > pv->ntypes) return NULL;
    return pv->types[i - 1];
}

static const char *want_os(plugview *pv)
{
    guint i;
    if (!pv->osdd) return NULL;
    i = gtk_drop_down_get_selected(GTK_DROP_DOWN(pv->osdd));
    if (i == GTK_INVALID_LIST_POSITION || i == 0 || (int)i > pv->noses) return NULL;
    return pv->oses[i - 1];
}

static void fill_browser(plugview *pv)
{
    const char *wt = want_type(pv), *wo = want_os(pv);
    const char *needle = pv->search ? gtk_editable_get_text(GTK_EDITABLE(pv->search)) : "";
    char *ln = g_utf8_strdown(needle, -1);
    int i;

    if (!pv->list) { g_free(ln); return; }
    clear_list(pv->list);
    pv->nvis = 0;
    for (i = 0; i < pv->nplug; i++) {
        const entry *e = &pv->plug[i];
        if (wt && strcmp(fmt_of(e), wt)) continue;
        if (wo && strcmp(e->os, wo)) continue;
        if (*ln) {
            char *n1 = g_utf8_strdown(e->name, -1), *n2 = g_utf8_strdown(e->path, -1);
            int hit = strstr(n1, ln) || strstr(n2, ln);
            g_free(n1); g_free(n2);
            if (!hit) continue;
        }
        pv->vis[pv->nvis++] = i;
        append_row(pv, e);
    }

    if (*ln) {
        plug_status(pv, "%d of %d plug-in(s) match \"%s\"", pv->nvis, pv->nplug, needle);
        g_free(ln);
        return;          /* searching is not choosing: nothing loads while typing */
    }
    g_free(ln);
    if (!wt && !wo) plug_status(pv, "%d plug-in(s) in %d folder(s)", pv->nplug, pv->nroot);
    else            plug_status(pv, "%d of %d plug-in(s) -- %s%s%s", pv->nvis, pv->nplug,
                                wo ? os_label(wo) : "every platform",
                                wt ? ", " : "", wt ? wt : "");

    /* Load the first one, the way pestudio does. Not for the convenience: a
     * GtkListBox picks a row for itself when it first takes focus, so without
     * saying which, the window came up having loaded whichever row that
     * happened to be -- different one each run. */
    if (pv->nvis > 0)
        gtk_list_box_select_row(GTK_LIST_BOX(pv->list),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv->list), 0));
}

/* Both selectors sift the list already in hand -- no folder is walked again,
 * so switching format or platform is instant however long the scan took. */
static void on_search_changed(GtkSearchEntry *e, gpointer ud)
{
    plugview *pv = ud;
    (void)e;
    if (pv->loading) return;
    fill_browser(pv);
}

static void on_filter_changed(GObject *dd, GParamSpec *ps, gpointer ud)
{
    plugview *pv = ud;
    (void)dd; (void)ps;
    if (pv->loading) return;
    fill_browser(pv);
}

/* --------------------------------------------------- native (X11) editor */

/* A native Linux plug-in draws its own GUI into an X11 window the host gives
 * it, and expects the host to run its event loop: it registers X11 descriptors
 * and timers with us rather than spinning its own, and a JUCE editor sits
 * blank without them. GLib sources are what those registrations become here --
 * the same job QSocketNotifier and QTimer do in pestudio.
 *
 * Three things about the bookkeeping, each of which pestudio learned the hard
 * way and this repeats rather than rediscovers:
 *
 *   - One handler can own several descriptors. IRunLoop keys a registration by
 *     handler and says nothing about the descriptor being unique, and a plug-in
 *     opening a menu uses that: it takes a second X11 connection for the menu
 *     and registers the same handler on it. Keyed by handler alone, the first
 *     watch is replaced but stays alive and unreachable, still calling a
 *     handler the plug-in has since freed.
 *   - Re-registering a descriptor already watched under the same handler must
 *     retire the old watch, or every event arrives twice.
 *   - A plug-in unregisters from inside the callback being dispatched --
 *     dismissing a popup is exactly that. g_source_destroy is safe there; the
 *     GSource itself is freed when our own reference goes, not on the spot. */

static void watch_retire(watch *w)
{
    if (!w->src) return;
    g_source_destroy(w->src);
    g_source_unref(w->src);
    w->src = NULL;
    w->handler = NULL;
}

static watch *watch_slot(plugview *pv)
{
    int i;
    for (i = 0; i < MAX_WATCH; i++) if (!pv->watches[i].src) return &pv->watches[i];
    return NULL;
}

static void watches_clear(plugview *pv)
{
    int i;
    for (i = 0; i < MAX_WATCH; i++) watch_retire(&pv->watches[i]);
}

static gboolean on_watch_fd(gint fd, GIOCondition cond, gpointer ud)
{
    (void)cond;
    v3_runloop_fd(ud, fd);
    return G_SOURCE_CONTINUE;
}

static gboolean on_watch_timer(gpointer ud)
{
    v3_runloop_timer(ud);
    return G_SOURCE_CONTINUE;
}

static void hook_add_fd(void *ud, void *handler, int fd)
{
    plugview *pv = ud;
    watch    *w;
    int       i;
    for (i = 0; i < MAX_WATCH; i++)          /* same fd, same handler: replace */
        if (pv->watches[i].src && pv->watches[i].handler == handler && pv->watches[i].fd == fd)
            watch_retire(&pv->watches[i]);
    if (!(w = watch_slot(pv))) {
        fprintf(stderr, "plugview: too many editor watches; ignoring fd %d\n", fd);
        return;
    }
    w->handler = handler;
    w->fd      = fd;
    w->src     = g_unix_fd_source_new(fd, G_IO_IN);
    g_source_set_callback(w->src, (GSourceFunc)(void *)on_watch_fd, handler, NULL);
    g_source_attach(w->src, NULL);
}

static void hook_del_fd(void *ud, void *handler)
{
    plugview *pv = ud;
    int       i;
    /* unregisterEventHandler names only the handler, so every descriptor
     * registered under it goes. */
    for (i = 0; i < MAX_WATCH; i++)
        if (pv->watches[i].src && pv->watches[i].handler == handler && pv->watches[i].fd >= 0)
            watch_retire(&pv->watches[i]);
}

static void hook_add_timer(void *ud, void *handler, unsigned long long ms)
{
    plugview *pv = ud;
    watch    *w;
    if (!(w = watch_slot(pv))) return;
    w->handler = handler;
    w->fd      = -1;
    w->src     = g_timeout_source_new((guint)(ms ? ms : 16));
    g_source_set_callback(w->src, on_watch_timer, handler, NULL);
    g_source_attach(w->src, NULL);
}

static void hook_del_timer(void *ud, void *handler)
{
    plugview *pv = ud;
    int       i;
    for (i = 0; i < MAX_WATCH; i++)
        if (pv->watches[i].src && pv->watches[i].handler == handler && pv->watches[i].fd < 0)
            watch_retire(&pv->watches[i]);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static Display *ed_display(void)
{
    GdkDisplay *d = gdk_display_get_default();
    return GDK_IS_X11_DISPLAY(d) ? gdk_x11_display_get_xdisplay(d) : NULL;
}

/* The zoom, which is defined with the rest of the editor input further down but
 * is reached from the run-loop resize hook and from the attach path above it. */
static void     zoom_apply(plugview *pv);
static void     zoom_update_ui(plugview *pv);
static gboolean zoom_fit_idle(gpointer u);
static void     zoom_fit(plugview *pv, int only_shrink);
static void     zoom_fit_poll(plugview *pv);

static void hook_resize(void *ud, int w, int h)
{
    plugview *pv = ud;
    Display  *dpy = ed_display();
    (void)dpy;
    if (w > 0 && h > 0) {
        /* The plug-in's own idea of its natural size, which replaces whatever
         * it opened at -- otherwise a later zoom would scale from a size the
         * plug-in has moved on from. The zoom is then re-applied on top. */
        pv->ed_base_w = w; pv->ed_base_h = h;
        pv->ed_w = w; pv->ed_h = h;
        pv->ed_nat_w = w; pv->ed_nat_h = h;
        gtk_widget_set_size_request(pv->editor, w, h);   /* the pane follows */
        zoom_apply(pv);
    }
}

static int native_editor_open(plugview *pv, pehost *h, const char *title);


/* Where the editor goes, in the toplevel's coordinates.
 *
 * `clip` is the part of the pane actually on screen -- the viewport, so it stops
 * short of the scrollbars -- and `plug` is the whole editor, positioned relative
 * to `clip`. Scrolling shows up here as a negative `plug` origin, which is
 * exactly what the clip window needs to be told.
 *
 * Fails when the two do not overlap at all, which is what a pane dragged shut
 * or scrolled out of view looks like; the caller hides the editor rather than
 * leaving it wherever it last was. */
static int editor_bounds(plugview *pv, GdkRectangle *clip, GdkRectangle *plug)
{
    GtkWidget       *root = GTK_WIDGET(gtk_widget_get_root(pv->editor));
    /* The viewport GtkScrolledWindow wrapped the drawing area in. Its
     * allocation is the visible rectangle; the drawing area's is the whole
     * editor, which is larger as soon as the size request below exceeds it. */
    GtkWidget       *port = gtk_widget_get_parent(pv->editor);
    graphene_rect_t  full, vis;
    int              x0, y0, x1, y1;

    if (!root || !port) return 0;
    if (!gtk_widget_compute_bounds(pv->editor, root, &full)) return 0;
    if (!gtk_widget_compute_bounds(port, root, &vis))      return 0;

    /* The plug-in's own size is a floor on the editor rectangle, not just
     * whatever the drawing area has been allocated.
     *
     * The allocation catches up one layout pass after the size request that
     * causes it, and native_editor_open runs in between -- so the plug-in was
     * handed a window the size of the pane, laid itself out to fit that, and
     * never grew out of it. Every editor came out exactly pane-sized and
     * nothing ever needed scrolling. Taking the larger of the two makes the
     * window the plug-in gets independent of when GTK gets round to the
     * layout. */
    if ((int)full.size.width  < pv->ed_nat_w) full.size.width  = (float)pv->ed_nat_w;
    if ((int)full.size.height < pv->ed_nat_h) full.size.height = (float)pv->ed_nat_h;

    x0 = (int)(full.origin.x > vis.origin.x ? full.origin.x : vis.origin.x);
    y0 = (int)(full.origin.y > vis.origin.y ? full.origin.y : vis.origin.y);
    x1 = (int)(full.origin.x + full.size.width < vis.origin.x + vis.size.width
               ? full.origin.x + full.size.width : vis.origin.x + vis.size.width);
    y1 = (int)(full.origin.y + full.size.height < vis.origin.y + vis.size.height
               ? full.origin.y + full.size.height : vis.origin.y + vis.size.height);
    if (x1 <= x0 || y1 <= y0) return 0;

    clip->x = x0; clip->y = y0; clip->width = x1 - x0; clip->height = y1 - y0;
    plug->x = (int)full.origin.x - x0;          /* <= 0 once scrolled */
    plug->y = (int)full.origin.y - y0;
    plug->width  = (int)full.size.width;
    plug->height = (int)full.size.height;
    return plug->width > 0 && plug->height > 0;
}

/* Keep the plug-in's window over the visible pane as the layout moves under it.
 *
 * Called from the resize signal and from the pump, because the three things
 * that move it do not share a signal: resizing the window emits one, dragging
 * the pane divider emits one, and scrolling emits none that reports the new
 * geometry after layout. Polling at the pump's rate covers all three, and the
 * comparison against the last placement means the X server only hears about it
 * when something really moved.
 *
 * Leaving the Editor page hides the window rather than closing the editor.
 * A foreign X child is not part of GTK's hierarchy, so nothing unmaps it when
 * the stack switches pages and the plug-in's editor stayed painted over the
 * parameter list. Unmapping our own clip window costs the plug-in nothing --
 * it is never told, and does not go through the attach/detach cycle that
 * Cardinal comes apart in. */
static void native_editor_place(plugview *pv)
{
    Display     *dpy = ed_display();
    GdkRectangle clip, plug;
    int          show;

    if (!dpy || !pv->ed_clip || !pv->ed_xwin) return;

    show = GTK_IS_WIDGET(pv->editor) && gtk_widget_get_mapped(pv->editor) &&
           editor_bounds(pv, &clip, &plug);
    if (!show) {
        if (pv->ed_shown) { XUnmapWindow(dpy, pv->ed_clip); pv->ed_shown = 0; XFlush(dpy); }
        return;
    }
    if (pv->ed_shown &&
        !memcmp(&clip, &pv->ed_clip_at, sizeof clip) &&
        !memcmp(&plug, &pv->ed_plug_at, sizeof plug))
        return;

    XMoveResizeWindow(dpy, pv->ed_clip, clip.x, clip.y,
                      (unsigned)clip.width, (unsigned)clip.height);
    XMoveResizeWindow(dpy, pv->ed_xwin, plug.x, plug.y,
                      (unsigned)plug.width, (unsigned)plug.height);
    if (!pv->ed_shown) { XMapWindow(dpy, pv->ed_clip); pv->ed_shown = 1; }
    pv->ed_clip_at = clip;
    pv->ed_plug_at = plug;
    XFlush(dpy);
}

/* The pane changing shape is both "put the editor back over it" and, the first
 * time, "there is somewhere to put it now".
 *
 * Switching to the Editor page does not lay it out synchronously, so the page
 * change arrives before the pane has any size and native_editor_open has
 * nowhere to place a window -- which read as every plug-in failing to attach.
 * The layout pass that follows is what actually makes it possible. */
static void on_editor_resize(GtkDrawingArea *a, int w, int h, gpointer u)
{
    plugview *pv = u;
    (void)a; (void)w; (void)h;
    if (pv->ed_attached) { native_editor_place(pv); return; }
    if (pv->ready && pv->host && pv->ed_native && pv->stack) {
        const char *page = gtk_stack_get_visible_child_name(GTK_STACK(pv->stack));
        if (page && !strcmp(page, "editor"))
            native_editor_open(pv, pv->host, pehost_name(pv->host));
    }
}

/* Let go of the native editor. Detach first, so unregisters the plug-in makes
 * on the way out are still routed somewhere. */
/* Detach, but keep the window.
 *
 * One window for the life of the pane, reused by every plug-in in turn -- what
 * pestudio does, and every "embedded as a child of window 0x..." line it
 * prints names the same one. Destroying it per plug-in tore the parent out
 * from under an editor that was still shutting down: JE8086 crashed inside
 * juce::OpenGLContext::CachedImage::stop(), down in the NVIDIA driver, on the
 * way out. The window costs nothing to keep and the plug-in that owns it has
 * already let go by the time the next one attaches. */
static void native_editor_close(plugview *pv)
{
    if (pv->ed_attached) { pehost_editor_detach(pv->ed_attached); pv->ed_attached = NULL; }
    /* Forget the last placement with it: the next plug-in's editor is a
     * different size, and a cached rectangle that still matches would let
     * native_editor_place decide there was nothing to do. */
    memset(&pv->ed_clip_at, 0, sizeof pv->ed_clip_at);
    memset(&pv->ed_plug_at, 0, sizeof pv->ed_plug_at);
    watches_clear(pv);
}

/* The window really does go, at shutdown.
 *
 * By then the GTK toplevel has usually destroyed its own X window already, and
 * X destroys a dead window's children along with it -- so the id held here
 * names nothing and XDestroyWindow answers BadWindow, which Xlib prints and
 * then exits on. Asking first does not help: every question about the window is
 * itself a request that would fault the same way. So the error is caught for
 * the length of the call rather than predicted. */
static int ed_swallow_x_error(Display *d, XErrorEvent *e)
{ (void)d; (void)e; return 0; }

static void native_editor_destroy(plugview *pv)
{
    Display *dpy = ed_display();

    native_editor_close(pv);
    if (dpy && (pv->ed_xwin || pv->ed_clip)) {
        int (*prev)(Display *, XErrorEvent *);
        XSync(dpy, False);                  /* let earlier errors land first */
        prev = XSetErrorHandler(ed_swallow_x_error);
        /* The child first, then its parent. Destroying the clip window would
         * take the other with it, but naming both keeps this readable and
         * costs one request. */
        if (pv->ed_xwin) XDestroyWindow(dpy, pv->ed_xwin);
        if (pv->ed_clip) XDestroyWindow(dpy, pv->ed_clip);
        XSync(dpy, False);                  /* and ours inside the handler */
        XSetErrorHandler(prev);
    }
    pv->ed_xwin  = 0;
    pv->ed_clip  = 0;
    pv->ed_xid   = 0;
    pv->ed_shown = 0;
}

/* One X11 window, made by hand.
 *
 * A plain 24-bit window, not an inherited-visual one. XCreateSimpleWindow takes
 * the parent's visual and a GTK toplevel is 32-bit ARGB; Qt's editor widget is
 * 24-bit and some plug-ins are happier with that. Kept even though the worst
 * GLX failure turned out to live elsewhere (GTK's GL renderer claiming the
 * hierarchy -- see GSK_RENDERER in dwstudio.c): matching what pestudio hands
 * over means one variable fewer when a plug-in misbehaves. */
static Window ed_make_window(Display *dpy, Window parent, const GdkRectangle *r)
{
    int                  screen = DefaultScreen(dpy);
    XSetWindowAttributes attrs;

    attrs.background_pixel = BlackPixel(dpy, screen);
    attrs.border_pixel     = 0;
    attrs.colormap         = DefaultColormap(dpy, screen);
    return XCreateWindow(dpy, parent, r->x, r->y,
                         (unsigned)r->width, (unsigned)r->height, 0,
                         DefaultDepth(dpy, screen), InputOutput,
                         DefaultVisual(dpy, screen),
                         CWBackPixel | CWBorderPixel | CWColormap, &attrs);
}

/* Give the plug-in a window inside the Editor page and hand it the id. */
static int native_editor_open(plugview *pv, pehost *h, const char *title)
{
    Display     *dpy = ed_display();
    GdkSurface  *surf;
    Window       parent;
    GdkRectangle clip, plug;

    native_editor_close(pv);
    if (!dpy) {
        plug_status(pv, "%s: a native editor needs the X11 backend -- "
                    "run with GDK_BACKEND=x11", title);
        return -1;
    }
    if (!editor_bounds(pv, &clip, &plug)) {
        /* The pane has not been laid out yet; the page change that brings it
         * into view will come back through here. */
        return -1;
    }
    surf = gtk_native_get_surface(gtk_widget_get_native(pv->editor));
    if (!surf || !GDK_IS_X11_SURFACE(surf)) {
        plug_status(pv, "%s: no X11 window to embed into", title);
        return -1;
    }
    parent = gdk_x11_surface_get_xid(GDK_X11_SURFACE(surf));

    /* Both windows live for the life of the pane and are reused by every
     * plug-in in turn, which is what pestudio does. Destroying them per plug-in
     * tore the parent out from under an editor that was still shutting down:
     * JE8086 crashed inside juce::OpenGLContext::CachedImage::stop(), down in
     * the NVIDIA driver, on the way out. They cost nothing to keep and the
     * plug-in that owns one has let go by the time the next attaches. */
    if (!pv->ed_clip && !(pv->ed_clip = ed_make_window(dpy, parent, &clip))) {
        plug_status(pv, "%s: could not create an editor window", title);
        return -1;
    }
    if (!pv->ed_xwin && !(pv->ed_xwin = ed_make_window(dpy, pv->ed_clip, &plug))) {
        plug_status(pv, "%s: could not create an editor window", title);
        return -1;
    }
    XMoveResizeWindow(dpy, pv->ed_clip, clip.x, clip.y,
                      (unsigned)clip.width, (unsigned)clip.height);
    XMoveResizeWindow(dpy, pv->ed_xwin, plug.x, plug.y,
                      (unsigned)plug.width, (unsigned)plug.height);
    XMapWindow(dpy, pv->ed_xwin);
    XMapWindow(dpy, pv->ed_clip);
    pv->ed_shown   = 1;
    pv->ed_clip_at = clip;
    pv->ed_plug_at = plug;

    /* Wait for it to be on screen before handing it over.
     *
     * Mapping is a request, not a fact. Attaching in the gap gives the plug-in
     * a parent that is not yet viewable and the child it creates inherits
     * that -- it builds its window, never maps it, and the pane stays blank. */
    {
        XWindowAttributes a;
        int spins;
        for (spins = 0; spins < 200; spins++) {
            XSync(dpy, False);
            if (XGetWindowAttributes(dpy, pv->ed_xwin, &a) && a.map_state == IsViewable)
                break;
            { struct timespec ts = { 0, 5000000 }; nanosleep(&ts, NULL); }
        }
    }
    pv->ed_xid = (unsigned long)pv->ed_xwin;

    if (pehost_editor_attach(h, pv->ed_xid) != 0) {
        plug_status(pv, "%s: the plug-in refused to embed into window 0x%lx",
                    title, pv->ed_xid);
        return -1;
    }
    pv->ed_attached = h;
    /* Zoomable from here, not from load(): until the plug-in has a window there
     * is nothing to ask to resize. Fit on the next turn of the main loop, once
     * the pane has been laid out at this editor's size. */
    zoom_update_ui(pv);
    if (!pv->ed_fit_idle) pv->ed_fit_idle = g_idle_add(zoom_fit_idle, pv);
    /* Said out loud, the way pestudio says it. Both rectangles, because which
     * one is bigger is the whole question when an editor does not fit: the
     * plug-in gets the first, and the second is how much of it you can see. */
    fprintf(stderr, "plugview: embedded as a child of window 0x%lx "
                    "(%dx%d editor, %dx%d visible)\n",
            pv->ed_xid, plug.width, plug.height, clip.width, clip.height);
    XFlush(dpy);

    /* No pehost_editor_resized() here, deferred or otherwise: it crashes
     * Cardinal inside its own framework, which asserts "pData->view !=
     * nullptr" and then dereferences it anyway. pestudio does not send one
     * either -- its editor widget is already the right size before it
     * attaches, so Qt has no resize left to deliver. */
    pv->ed_w = plug.width;
    pv->ed_h = plug.height;
    return 0;
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* ------------------------------------------------------------- the editor */

/* WM_* codes, so the plug-in sees the messages it was written against. */
enum { WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
       WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
       WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020A };
enum { MK_LBUTTON = 0x0001, MK_RBUTTON = 0x0002, MK_MBUTTON = 0x0010 };

static void editor_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    const unsigned int *px = NULL;
    int pw = 0, ph = 0;
    cairo_surface_t *surf;
    plugview *pv = ud;

    (void)area;
    if (!pv->host || !pv->ed_open ||
        !pehost_editor_pixels(pv->host, &px, &pw, &ph) ||
        !px || pw <= 0 || ph <= 0) {
        /* Nothing drawn yet. Paint the background rather than leaving whatever
         * was on the surface before. */
        cairo_set_source_rgb(cr, 0.12, 0.12, 0.13);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);
        return;
    }

    /* pehost hands back 0x00RRGGBB, which is exactly CAIRO_FORMAT_RGB24 in
     * native byte order -- so the buffer is wrapped, not converted. */
    surf = cairo_image_surface_create_for_data((unsigned char *)px,
                                              CAIRO_FORMAT_RGB24, pw, ph, pw * 4);
    /* The zoom lives here and nowhere near the plug-in: it goes on drawing at
     * its own size into its own buffer, and only this last step -- putting that
     * buffer on the screen -- knows about it. Input is mapped back the other
     * way in ed_mouse. */
    if (pv->ed_zoom != 1.0) cairo_scale(cr, pv->ed_zoom, pv->ed_zoom);
    cairo_set_source_surface(cr, surf, 0, 0);
    /* Smoothed on the way down, sharp on the way up. Dropping every other pixel
     * of a knob leaves it ragged; an editor enlarged is bitmaps and text at a
     * fixed size, and blurring those is worse than seeing the pixels. */
    cairo_pattern_set_filter(cairo_get_source(cr),
                             pv->ed_zoom < 1.0 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
}

/* The editor is not driven by GTK: the plug-in repaints when it feels like it,
 * so the buffer has to be pumped and the widget told to redraw. 30 ms is what
 * pestudio settled on -- fast enough that a dragged knob tracks the mouse. */
static gboolean editor_tick(gpointer ud)
{
    plugview *pv = ud;
    if (pv->in_plugin) return G_SOURCE_CONTINUE;

    /* A plug-in whose helper has died stops repainting and goes silent, and
     * both look exactly like a plug-in that is working and idle. The Qt window
     * has said so, and then restarted it, for a while; this one noticed
     * nothing at all -- so a helper that died left a window that appeared to be
     * playing and was not.
     *
     * Restarting is the first thing to try, because the host can: it knows the
     * file and it has kept the program and the parameter values. Three
     * attempts, because a plug-in that faults on something it will meet again
     * faults the same way every time, and restarting for ever is worse than
     * saying so. */
    if (pv->host && !pehost_alive(pv->host) && !pv->dead_reported) {
        if (pehost_restarts(pv->host) < 3 && pehost_recover(pv->host)) {
            pv->ed_open = 0;                 /* reopened below, as after a load */
            plug_status(pv, "this plug-in stopped responding and was restarted "
                        "(attempt %d) — its settings were put back",
                        pehost_restarts(pv->host));
            if (pehost_editor_kind(pv->host) == PEHOST_EDITOR_PIXELS &&
                pehost_editor_open(pv->host) == 0)
                pv->ed_open = 1;
        } else {
            pv->dead_reported = 1;
            plug_status(pv, "this plug-in stopped responding and would not restart "
                        "— reload it to try again");
        }
        return G_SOURCE_CONTINUE;
    }

    /* A native editor is an X11 window of its own: nothing to pump and no
     * pixels to fetch, but it has to be kept over the visible part of the pane
     * as that scrolls, resizes, and comes and goes with the page. Scrolling in
     * particular emits no signal that reports the new geometry after layout,
     * so this is where it is noticed. */
    if (pv->ed_attached) {
        native_editor_place(pv);
        zoom_fit_poll(pv);
        return G_SOURCE_CONTINUE;
    }
    if (!pv->host || !pv->ed_open) return G_SOURCE_CONTINUE;
    if (!GTK_IS_WIDGET(pv->editor) || !gtk_widget_get_mapped(pv->editor))
        return G_SOURCE_CONTINUE;
    pv->in_plugin++;
    pehost_editor_pump(pv->host);
    pv->in_plugin--;
    gtk_widget_queue_draw(pv->editor);
    zoom_fit_poll(pv);
    return G_SOURCE_CONTINUE;
}

/* Reads the peak the audio thread left behind and shows it, decaying so a note
 * that has stopped stops reading. Also the answer to "is this thing actually
 * making sound", which otherwise needs a recording to establish. */
static gboolean meter_tick(gpointer ud)
{
    plugview *pv = ud;
    char txt[640];
    int  pk;
    if (!pv->host || !pv->status || !pv->loaded_msg[0]) return G_SOURCE_CONTINUE;
    pk = atomic_exchange_explicit(&pv->peak_milli, 0, memory_order_relaxed);
    if (pk < pv->meter_shown) pk = pv->meter_shown - 40 > 0 ? pv->meter_shown - 40 : 0;
    pv->meter_shown = pk;                                   /* ease down */
    snprintf(txt, sizeof txt, "%s   ·   out %.3f %s", pv->loaded_msg, pk / 1000.0,
             pk > 0 ? "\xe2\x96\xa0" : "");
    gtk_label_set_text(GTK_LABEL(pv->status), txt);
    return G_SOURCE_CONTINUE;
}

/* Every mouse message goes through here, which is what makes the zoom safe to
 * add: the plug-in is told where the click landed in its own picture, not where
 * it landed on screen. Getting this wrong does not look like a bug in the zoom
 * -- it looks like the plug-in's knobs have stopped working. */
static void ed_mouse(plugview *pv, int x, int y, int msg, int wheel)
{
    if (!pv->host || !pv->ed_open) return;
    if (pv->ed_zoom != 1.0) { x = (int)(x / pv->ed_zoom); y = (int)(y / pv->ed_zoom); }
    pv->in_plugin++;
    pehost_editor_mouse(pv->host, x, y, msg, pv->buttons, wheel);
    pv->in_plugin--;
}

/* ---------------------------------------------------------------- zoom */

/* Plug-in editors are drawn at whatever size the plug-in chose, and several in
 * this corpus are larger than the window can be -- one is 1480x660 against a
 * pane that is 836x406 with the browser open. Scrolling a synth you are trying
 * to play is not much of an answer, so the picture is scaled to the room there
 * is instead.
 *
 * The two editor kinds get there by different routes. A pixel editor is scaled
 * on the way to the screen and the plug-in never learns of it. A native editor
 * is an X11 window the plug-in paints itself -- there is no image on this side
 * to scale, and X has no scaled child window either -- so the only thing that
 * can be done is to hand it a different size and let it lay itself out again,
 * which a resizable VST3 does by scaling its whole interface. One that says it
 * cannot resize is left alone and the bar says why. */
#define ZOOM_MIN 0.25
#define ZOOM_MAX 4.00

static int zoom_can(plugview *pv)
{
    if (!pv->host || pv->ed_base_w <= 0) return 0;
    if (pv->ed_native) return pv->ed_attached && pehost_editor_can_resize(pv->host);
    return pv->ed_open;
}

static void zoom_update_ui(plugview *pv)
{
    char txt[32];
    int  on = zoom_can(pv);

    /* Six pointers are touched below and the guard used to check one of them,
     * so a call arriving after the pane went away wrote to five stale widgets
     * before anyone noticed. They are cleared together in plugview_shutdown, so
     * one test covers the lot -- but each is checked anyway, because this runs
     * from an idle callback and being inert is the only safe thing for it to be
     * when it arrives late. */
    if (!pv->zoom_lbl || !pv->zoom_out || !pv->zoom_in ||
        !pv->zoom_fit || !pv->zoom_one || !pv->zoom_note) return;
    snprintf(txt, sizeof txt, "%d%%", (int)(pv->ed_zoom * 100.0 + 0.5));
    gtk_label_set_text(GTK_LABEL(pv->zoom_lbl), txt);
    gtk_widget_set_sensitive(pv->zoom_lbl, on);
    gtk_widget_set_sensitive(pv->zoom_out, on && pv->ed_zoom > ZOOM_MIN);
    gtk_widget_set_sensitive(pv->zoom_in,  on && pv->ed_zoom < ZOOM_MAX);
    gtk_widget_set_sensitive(pv->zoom_fit, on);
    gtk_widget_set_sensitive(pv->zoom_one, on && pv->ed_zoom != 1.0);
    /* Say why, when they are dead. A disabled button with no reason beside it
     * reads as something broken rather than something that cannot be done to
     * this particular plug-in. */
    gtk_label_set_text(GTK_LABEL(pv->zoom_note),
        on      ? (pv->ed_native ? "the plug-in redraws itself at this size" : "")
        : !pv->host || pv->ed_base_w <= 0 ? "no editor open"
        : pv->ed_native ? "this plug-in draws its own window and will not resize it"
                      : "");
}

static void zoom_apply(plugview *pv)
{
    int sw, sh;

    if (pv->ed_base_w <= 0 || pv->ed_base_h <= 0) { zoom_update_ui(pv); return; }
    if (!zoom_can(pv)) {                  /* shown at the size the plug-in drew */
        pv->ed_zoom = 1.0;
        zoom_update_ui(pv);
        return;
    }
    sw = (int)(pv->ed_base_w * pv->ed_zoom + 0.5);
    sh = (int)(pv->ed_base_h * pv->ed_zoom + 0.5);
    pv->ed_w = sw; pv->ed_h = sh;
    if (pv->ed_native) {
        /* The window the plug-in was given changes size, and editor_bounds
         * reads ed_nat_* to decide how big that is -- so it is the scaled size
         * that goes there, not the one the plug-in first asked for. */
        pv->ed_nat_w = sw; pv->ed_nat_h = sh;
        gtk_widget_set_size_request(pv->editor, sw, sh);
        native_editor_place(pv);
        pv->in_plugin++;
        pehost_editor_resized(pv->host, sw, sh);
        pv->in_plugin--;
    } else {
        gtk_widget_set_size_request(pv->editor, sw, sh);
        gtk_widget_queue_draw(pv->editor);
    }
    zoom_update_ui(pv);
}

static void zoom_set(plugview *pv, double z)
{
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    pv->ed_zoom = z;
    zoom_apply(pv);
}

/* One notch. Geometric rather than a fixed number of percent: stepping down
 * from 100 in tenths takes ten presses to halve the picture and then crawls,
 * where a constant ratio feels the same at every size. */
static void zoom_step(plugview *pv, int dir)
{
    if (!dir) return;
    pv->ed_fit_auto = 0;
    zoom_set(pv, pv->ed_zoom * (dir > 0 ? 1.25 : 1.0 / 1.25));
}

/* Scale the editor to the space there is.
 *
 * `only_shrink` is what the automatic fit on opening an editor uses: one that
 * already fits is left at the size the plug-in drew it, because enlarging is
 * not an improvement -- the artwork is bitmaps and text at a fixed size, and
 * stretching it only makes it soft. Pressing Fit is an explicit request and
 * will enlarge. */
static void zoom_fit(plugview *pv, int only_shrink)
{
    GtkWidget *port;
    double     z, zy;
    int        vw, vh;

    if (!zoom_can(pv) || pv->ed_base_w <= 0 || pv->ed_base_h <= 0) return;
    if (!pv->editor || !(port = gtk_widget_get_parent(pv->editor))) return;
    vw = gtk_widget_get_width(port);
    vh = gtk_widget_get_height(port);
    if (vw < 16 || vh < 16) return;        /* not laid out yet -- try again */
    z  = (double)vw / pv->ed_base_w;
    zy = (double)vh / pv->ed_base_h;
    if (zy < z) z = zy;
    if (only_shrink && z >= 1.0) z = 1.0;
    zoom_set(pv, z);
    pv->ed_fit_pending = 0;                  /* measured something real */
    pv->ed_fit_vw = vw; pv->ed_fit_vh = vh;
}

/* Keep the editor fitted while nobody has asked for a particular zoom: re-fit
 * when the pane changes size, and keep trying while it has not been laid out
 * yet. Touching the zoom controls ends it -- a chosen zoom is a decision. */
static void zoom_fit_poll(plugview *pv)
{
    GtkWidget *port;
    if (!pv->ed_fit_auto && !pv->ed_fit_pending) return;
    if (!pv->editor || !(port = gtk_widget_get_parent(pv->editor))) return;
    if (!pv->ed_fit_pending &&
        gtk_widget_get_width(port)  == pv->ed_fit_vw &&
        gtk_widget_get_height(port) == pv->ed_fit_vh) return;
    zoom_fit(pv, 1);
}

/* The automatic fit runs one main-loop turn after the editor opens: the pane
 * has not been laid out at this editor's size yet, and measuring it now returns
 * the last plug-in's.
 *
 * One turn is not always enough, and when it was not the fit simply did not
 * happen -- zoom_fit returns without a word if the pane has no size, and
 * nothing asked again. An editor larger than the pane then stayed at 100% and
 * you got the top-left corner of it and a pair of scrollbars, which is what
 * WhispAir and Tricent did: the two largest editors here are the two most
 * likely to be opened before the pane has been laid out at their size. The
 * flag keeps the request alive and editor_tick retries it until the pane can
 * actually be measured. */
static gboolean zoom_fit_idle(gpointer u)
{ plugview *pv = u; pv->ed_fit_idle = 0; pv->ed_fit_pending = 1; pv->ed_fit_auto = 1;
  pv->ed_fit_vw = pv->ed_fit_vh = -1; zoom_fit(pv, 1); return G_SOURCE_REMOVE; }

static void on_zoom_out(GtkButton *b, gpointer u) { plugview *pv = u; (void)b; zoom_step(pv, -1); }
static void on_zoom_in (GtkButton *b, gpointer u) { plugview *pv = u; (void)b; zoom_step(pv, +1); }
static void on_zoom_fit(GtkButton *b, gpointer u)
{ plugview *pv = u; (void)b; pv->ed_fit_auto = 1; zoom_fit(pv, 0); }
static void on_zoom_one(GtkButton *b, gpointer u)
{ plugview *pv = u; (void)b; pv->ed_fit_auto = 0; zoom_set(pv, 1.0); }

static void on_ed_pressed(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    plugview *pv = ud;
    int msg;

    gtk_widget_grab_focus(pv->editor);
    if (button == 3)      { pv->buttons |= MK_RBUTTON; msg = WM_RBUTTONDOWN; }
    else if (button == 2) { pv->buttons |= MK_MBUTTON; msg = WM_MBUTTONDOWN; }
    else                  { pv->buttons |= MK_LBUTTON;
                            msg = (n >= 2) ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN; }
    ed_mouse(pv, (int)x, (int)y, msg, 0);
}

static void on_ed_released(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    plugview *pv = ud;
    int msg;

    (void)n;
    if (button == 3)      { pv->buttons &= ~MK_RBUTTON; msg = WM_RBUTTONUP; }
    else if (button == 2) { pv->buttons &= ~MK_MBUTTON; msg = WM_MBUTTONUP; }
    else                  { pv->buttons &= ~MK_LBUTTON; msg = WM_LBUTTONUP; }
    ed_mouse(pv, (int)x, (int)y, msg, 0);
}

static void on_ed_motion(GtkEventControllerMotion *m, double x, double y, gpointer ud)
{ plugview *pv = ud; (void)m; ed_mouse(pv, (int)x, (int)y, WM_MOUSEMOVE, 0); }

static gboolean on_ed_scroll(GtkEventControllerScroll *s, double dx, double dy, gpointer ud)
{
    GdkModifierType st;
    plugview *pv = ud;

    (void)dx;
    /* Ctrl and the wheel is the zoom everywhere else, and the plug-in is not
     * expecting it -- a bare wheel still belongs to whatever control is under
     * the pointer, which is the only way to work some editors. */
    st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(s));
    if (st & GDK_CONTROL_MASK) {
        if (dy != 0.0) zoom_step(pv, dy < 0.0 ? 1 : -1);
        return TRUE;
    }
    /* GTK counts notches, Windows counts 120ths of one; the plug-in expects
     * the latter. Sign flipped: scrolling down is a negative delta there. */
    ed_mouse(pv, 0, 0, WM_MOUSEWHEEL, (int)(-dy));
    return TRUE;
}

/* GDK keyvals to Windows virtual keys, for the ones an editor reacts to. */
static int gdk_to_vk(guint kv)
{
    switch (kv) {
    case GDK_KEY_BackSpace: return 0x08;
    case GDK_KEY_Tab:       return 0x09;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return 0x0D;
    case GDK_KEY_Escape:    return 0x1B;
    case GDK_KEY_space:     return 0x20;
    case GDK_KEY_End:       return 0x23;
    case GDK_KEY_Home:      return 0x24;
    case GDK_KEY_Left:      return 0x25;
    case GDK_KEY_Up:        return 0x26;
    case GDK_KEY_Right:     return 0x27;
    case GDK_KEY_Down:      return 0x28;
    case GDK_KEY_Delete:    return 0x2E;
    default:
        if (kv >= GDK_KEY_0 && kv <= GDK_KEY_9) return (int)(kv - GDK_KEY_0) + 0x30;
        if (kv >= GDK_KEY_a && kv <= GDK_KEY_z) return (int)(kv - GDK_KEY_a) + 0x41;
        if (kv >= GDK_KEY_A && kv <= GDK_KEY_Z) return (int)(kv - GDK_KEY_A) + 0x41;
        return 0;
    }
}

void plugview_set_note_key(plugview *pv, int (*claims)(guint keyval))
{ pv->note_key = claims; }

/* Keys over the editor go to the plug-in -- and a note key goes to the piano
 * as well.
 *
 * Turning a knob in a plug-in's editor moves focus to it, and a controller
 * that answered TRUE here stopped the key ever reaching the window, which is
 * where the note keys live: z, x, c, v went dead the moment you touched a
 * control, and stayed dead until the on-screen keyboard was clicked to take
 * focus back. Tweak a sound and then play it is the ordinary way round to do
 * things, so that is the case to keep working -- the same trade pestudio's
 * event filter makes, and made for the same reason.
 *
 * The plug-in still sees the letter: it is forwarded before the note test, and
 * the test only declines to swallow it. The cost is that typing into a text
 * field inside an editor plays a note alongside -- a field inside a foreign
 * window is not something this side can detect, Cardinal has a whole
 * text-editor module -- which is the rarer half of the trade, and audible
 * rather than destructive. Settings > Enter Key is the way in when a plug-in
 * really does want twenty-five characters typed at it.
 *
 * Releases need nothing here. GTK's key-released signal carries no "handled"
 * answer back, and a release is delivered to every controller between the
 * focused widget and the one that took the press -- so a note started at the
 * window is released even though focus has since moved into the editor, which
 * is the case that matters: a note is held precisely so the hand is free to
 * reach into the editor, and the key-up then arrives over the editor. (A press
 * this handler does swallow keeps its release too, which is right, because no
 * note was started for it.) */
static gboolean on_ed_key_down(GtkEventControllerKey *c, guint kv, guint code,
                               GdkModifierType st, gpointer ud)
{
    guint32 ch;
    plugview *pv = ud;
    (void)c; (void)code; (void)st;
    if (!pv->host || !pv->ed_open) return FALSE;
    ch = gdk_keyval_to_unicode(kv);
    pv->in_plugin++;
    pehost_editor_key(pv->host, gdk_to_vk(kv), 1, (int)ch);
    pv->in_plugin--;
    if (pv->note_key && pv->note_key(kv)) return FALSE;    /* on to the piano */
    return TRUE;
}

static void on_ed_key_up(GtkEventControllerKey *c, guint kv, guint code,
                         GdkModifierType st, gpointer ud)
{
    plugview *pv = ud;
    (void)c; (void)code; (void)st;
    if (!pv->host || !pv->ed_open) return;
    pv->in_plugin++;
    pehost_editor_key(pv->host, gdk_to_vk(kv), 0, 0);
    pv->in_plugin--;
}

/* -------------------------------------------------------------- parameters */

typedef struct { plugview *pv; int index; GtkWidget *value; } prow;

/* g_free has the wrong shape for a GClosureNotify, and casting it there is a
 * warning the compiler is right to give. */
static void prow_free(gpointer p, GClosure *c) { (void)c; g_free(p); }

static void on_param_changed(GtkRange *r, gpointer ud)
{
    prow *pr = ud;
    plugview *pv = pr->pv;
    char  ds[64], lb[64], txt[160];

    if (pv->loading || !pv->host || pv->in_plugin) return;
    pv->in_plugin++;
    pehost_set_param(pv->host, pr->index, (float)gtk_range_get_value(r));
    pehost_param_display(pv->host, pr->index, ds, sizeof ds);
    pehost_param_label(pv->host, pr->index, lb, sizeof lb);
    pv->in_plugin--;
    snprintf(txt, sizeof txt, "%s %s", ds, lb);
    gtk_label_set_text(GTK_LABEL(pr->value), txt);
}

static void fill_params(plugview *pv)
{
    int n, i;

    clear_list(pv->paramlist);
    if (!pv->host) return;

    pv->loading = 1;
    n = pehost_num_params(pv->host);
    for (i = 0; i < n; i++) {
        char nm[64], ds[64], lb[64], txt[160];
        GtkWidget *row, *name, *scale, *value;
        prow *pr;

        pehost_param_name(pv->host, i, nm, sizeof nm);
        pehost_param_display(pv->host, i, ds, sizeof ds);
        pehost_param_label(pv->host, i, lb, sizeof lb);

        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);

        name = gtk_label_new(nm[0] ? nm : "-");
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_widget_set_size_request(name, 190, -1);

        scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_range_set_value(GTK_RANGE(scale), pehost_get_param(pv->host, i));
        gtk_widget_set_hexpand(scale, TRUE);

        snprintf(txt, sizeof txt, "%s %s", ds, lb);
        value = gtk_label_new(txt);
        gtk_label_set_xalign(GTK_LABEL(value), 1.0f);
        gtk_widget_set_size_request(value, 130, -1);

        pr = g_new0(prow, 1);
        pr->pv = pv;
        pr->index = i;
        pr->value = value;
        g_signal_connect_data(scale, "value-changed", G_CALLBACK(on_param_changed),
                              pr, prow_free, 0);

        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), scale);
        gtk_box_append(GTK_BOX(row), value);
        gtk_list_box_append(GTK_LIST_BOX(pv->paramlist), row);
    }
    pv->loading = 0;
}

static void fill_programs(plugview *pv)
{
    int n, i;

    clear_list(pv->proglist);
    if (!pv->host) return;

    pv->loading = 1;
    n = pehost_num_programs(pv->host);
    for (i = 0; i < n; i++) {
        char pn[64] = { 0 }, lbl[96];
        GtkWidget *l;
        pehost_program_name(pv->host, i, pn, sizeof pn);
        snprintf(lbl, sizeof lbl, "%3d  %s", i, pn[0] ? pn : "-");
        l = gtk_label_new(lbl);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_widget_set_margin_start(l, 4);
        gtk_list_box_append(GTK_LIST_BOX(pv->proglist), l);
    }
    if (n > 0)
        gtk_list_box_select_row(GTK_LIST_BOX(pv->proglist),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv->proglist),
                                          pehost_get_program(pv->host)));
    pv->loading = 0;
}

/* ------------------------------------------------------------------ loading */

static void set_header(plugview *pv)
{
    char txt[512];

    /* Inert once the pane has gone, the way plug_status and zoom_update_ui
     * are: this runs from load and unload paths, and one of those is reached
     * during teardown. */
    if (!pv->header) return;
    if (!pv->host) {
        gtk_label_set_text(GTK_LABEL(pv->header), "no plug-in loaded");
        return;
    }
    snprintf(txt, sizeof txt,
             "%s — %s\n%s   in %d / out %d   programs %d   params %d",
             pehost_name(pv->host), pehost_vendor(pv->host),
             pehost_is_synth(pv->host) ? "synth" : "effect",
             pehost_num_inputs(pv->host), pehost_num_outputs(pv->host),
             pehost_num_programs(pv->host), pehost_num_params(pv->host));
    gtk_label_set_text(GTK_LABEL(pv->header), txt);
}

static void unload_locked(plugview *pv)
{
    if (!pv->host) return;
    /* The editor first: it holds a pointer to this plug-in, and the run-loop
     * watches hold pointers into it. Closing the plug-in with either still
     * live is a callback into freed memory. */
    native_editor_close(pv);
    /* The pane goes back to following the window. A size request left behind by
     * a large editor would keep the scrollbars up for the next plug-in loaded,
     * whatever size that one turns out to be. */
    if (GTK_IS_WIDGET(pv->editor)) gtk_widget_set_size_request(pv->editor, -1, -1);
    pv->ed_nat_w = pv->ed_nat_h = 0;
    pv->ed_base_w = pv->ed_base_h = 0;
    pv->ed_zoom = 1.0;
    pv->ed_fit_pending = 0;          /* whatever was still to be fitted is gone */
    pv->ed_fit_auto = 0;
    zoom_update_ui(pv);
    atomic_store_explicit(&pv->live, 0, memory_order_release);
    pv->ed_open = 0;
    pv->ed_native = 0;
    pehost_close(pv->host);
    pv->host = NULL;
}

void plugview_set_load_hook(plugview *pv, void (*fn)(plugview *pv)) { pv->load_hook = fn; }

static void load(plugview *pv, const entry *e)
{
    char msg[1024];

    /* Refuse rather than free a plug-in that is mid-call. */
    if (pv->in_plugin) return;
    pv->in_plugin++;

    /* Park first, always. The callback may be inside pehost_render_io on the
     * plug-in we are about to close, and closing it under a realtime thread is
     * a crash while playing rather than a tidy failure. */
    if (pv->park) pv->park();
    unload_locked(pv);
    pv->host = pehost_open_as(e->path, PEHOST_KIND_AUTO, pv->rate, pv->block);
    snprintf(pv->loaded_path, sizeof pv->loaded_path, "%s", pv->host ? e->path : "");
    if (pv->host) atomic_store_explicit(&pv->live, 1, memory_order_release);
    if (pv->unpark) pv->unpark();

    if (!pv->host) {
        plug_status(pv, "%s: %s", e->name, pehost_last_error());
        set_header(pv);
        clear_list(pv->proglist);
        clear_list(pv->paramlist);
        pv->in_plugin--;
        return;
    }

    set_header(pv);
    fill_programs(pv);
    fill_params(pv);

    {
        int kind = pehost_editor_kind(pv->host);
        int w = 0, h = 0;
        pv->dead_reported = 0;              /* a fresh plug-in gets a fresh verdict */
        if (kind == PEHOST_EDITOR_PIXELS && pehost_editor_open(pv->host) == 0) {
            pv->ed_open = 1;
            pehost_editor_size(pv->host, &w, &h);
            if (w > 0 && h > 0) {
                pv->ed_w = w; pv->ed_h = h;
                pv->ed_base_w = w; pv->ed_base_h = h;
                gtk_widget_set_size_request(pv->editor, w, h);
                zoom_update_ui(pv);
                /* Fit it if it does not already fit, one turn later -- the pane
                 * has not been laid out at this editor's size yet. */
                if (!pv->ed_fit_idle) pv->ed_fit_idle = g_idle_add(zoom_fit_idle, pv);
            }
            snprintf(msg, sizeof msg, "%s loaded — editor %dx%d", e->name, w, h);
        } else if (kind == PEHOST_EDITOR_X11) {
            /* A native Linux plug-in draws into a window we give it, in the
             * Editor page. Opened when that page is looked at.
             *
             * Its natural size becomes the drawing area's size request, which
             * is what puts scrollbars on the pane when the editor is bigger
             * than the window. Without it the pane was only ever as big as the
             * window, the plug-in was handed that, and an editor that did not
             * fit was simply cut off with no way to reach the rest of it. */
            pv->ed_native = 1;
            pehost_editor_size(pv->host, &w, &h);
            if (w > 0 && h > 0) {
                pv->ed_w = w; pv->ed_h = h;
                pv->ed_nat_w = w; pv->ed_nat_h = h;
                pv->ed_base_w = w; pv->ed_base_h = h;
                gtk_widget_set_size_request(pv->editor, w, h);
                snprintf(msg, sizeof msg,
                         "%s loaded — editor %dx%d, on the Editor page",
                         e->name, w, h);
            } else {
                snprintf(msg, sizeof msg, "%s loaded — see the Editor page", e->name);
            }
        } else {
            snprintf(msg, sizeof msg, "%s loaded — no editor", e->name);
        }
        if (e->warn[0])
            snprintf(msg + strlen(msg), sizeof msg - strlen(msg),
                     "   \xc2\xb7   %s", e->warn);
        /* And whether anyone has already made a sound with it.
         *
         * Saving a patch and never seeing it again is the failure this stops:
         * the plug-in says on load that its patches exist and where to get at
         * them. pestudio puts them straight in its Patches list; this window
         * has no list to put them in, so it says the count and names the menu
         * item that opens them. */
        {
            char hits[16][1024];
            int  n = patch_find_for(pv->host, e->path, hits, 16);
            if (n > 0)
                snprintf(msg + strlen(msg), sizeof msg - strlen(msg),
                         "   \xc2\xb7   %d patch file(s) saved for it "
                         "— File > Open Patch", n);
        }
        plug_status(pv, "%s", msg);
    }
    pv->in_plugin--;

    /* Already looking at the Editor page: bring this plug-in's up, because the
     * page is not changing and nothing else will.
     *
     * Losing this line is what made 54 of 55 plug-ins show nothing -- the
     * first one attached from the page change and every one after it silently
     * did not. */
    if (pv->ed_native && pv->stack && !pv->ed_attached) {
        const char *page = gtk_stack_get_visible_child_name(GTK_STACK(pv->stack));
        if (page && !strcmp(page, "editor"))
            native_editor_open(pv, pv->host, pehost_name(pv->host));
    }

    /* Last, so the window is told about a plug-in that is finished loading and
     * not one that is halfway in. */
    if (pv->load_hook) pv->load_hook(pv);
}

/* Rescan every folder and show the result. What File > Load Folder and the
 * settings dialog call once they have changed the set of folders. */
static void rescan_all(plugview *pv)
{
    plugview_scan(pv, NULL);
    fill_browser(pv);
}

/* Looking at the Editor page is what opens a native editor. Leaving the page
 * does *not* close it again.
 *
 * One attach and one detach per plug-in, which is all pestudio ever does and
 * all these are tested against. Opening and closing per tab switch put
 * Cardinal through repeated attach/detach cycles and it came apart inside its
 * own framework -- "assertion failure: pData->view != nullptr", then a
 * segfault. The window stays until the plug-in is unloaded. */
static void on_page_changed(GObject *stack, GParamSpec *ps, gpointer ud)
{
    const char *page = gtk_stack_get_visible_child_name(GTK_STACK(stack));

    plugview *pv = ud;
    (void)ps;
    /* GtkStack emits this while the pane is still being assembled -- adding
     * pages and attaching a switcher both move the visible child. Opening a
     * plug-in editor from inside plugview_new is not what the user asked for,
     * and it happened before anything was on screen. */
    if (!pv->ready || !pv->host || !pv->ed_native || pv->ed_attached) return;
    if (page && !strcmp(page, "editor"))
        native_editor_open(pv, pv->host, pehost_name(pv->host));
}

static void on_plug_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
    plugview *pv = ud;
    int i;
    (void)lb;
    if (!row) return;
    /* A row index is an index into the *visible* rows, which the selectors
     * decide -- not into plug[]. Reading plug[] with it directly loaded a
     * different plug-in from the one clicked as soon as anything was
     * filtered. */
    i = gtk_list_box_row_get_index(row);
    if (i < 0 || i >= pv->nvis) return;
    i = pv->vis[i];
    /* Which folder it came out of, now that the list spans all of them: two
     * builds of one plug-in under different roots are otherwise one name
     * twice. */
    if (pv->dirlabel) {
        char dir[1024], *slash;
        snprintf(dir, sizeof dir, "%s", pv->plug[i].path);
        if ((slash = strrchr(dir, '/'))) *slash = 0;
        snprintf(pv->dir, sizeof pv->dir, "%s", dir);
        gtk_label_set_text(GTK_LABEL(pv->dirlabel), dir);
    }
    if (!pv->plug[i].loadable) {
        plug_status(pv, "%s: %s", pv->plug[i].name, pv->plug[i].kind);
        return;
    }
    load(pv, &pv->plug[i]);
}

static void on_prog_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
    plugview *pv = ud;
    (void)lb;
    if (pv->loading || !pv->host || !row || pv->in_plugin) return;
    pv->in_plugin++;
    pehost_set_program(pv->host, gtk_list_box_row_get_index(row));
    pv->in_plugin--;
    fill_params(pv);          /* a program change rewrites every parameter */
}

static void append_row(plugview *pv, const entry *e)
{
    char       lbl[288];
    GtkWidget *l;

    /* The marker rather than the whole sentence: a row has to stay readable in
     * a 300px list, and the reason is spelled out in the status line when the
     * plug-in is actually selected. */
    snprintf(lbl, sizeof lbl, "%s   [%s]%s", e->name, e->kind,
             e->warn[0] ? "   \xe2\x9a\xa0 needs data" : "");
    l = gtk_label_new(lbl);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_margin_start(l, 4);
    gtk_widget_set_margin_end(l, 4);
    gtk_list_box_append(GTK_LIST_BOX(pv->list), l);
}

/* Load one plug-in by path, wherever it came from. Already-listed ones are
 * selected rather than added twice. */
static void open_path(plugview *pv, const char *path)
{
    pehost_info info;
    const char *base;
    entry      *e;
    char        why[160] = "";
    int         i;

    if (!pehost_can_load(path, why, (int)sizeof why)) {
        plug_status(pv, "%s: %s", path,
                    why[0] ? why : "not a plug-in this host can load");
        return;
    }
    /* Anything opened by hand has to end up visible, so the selectors go back
     * to "All" first: opening a Linux plug-in while the list is filtered to
     * Windows would otherwise add it and select nothing. */
    if (pv->typedd && (gtk_drop_down_get_selected(GTK_DROP_DOWN(pv->typedd)) ||
                     gtk_drop_down_get_selected(GTK_DROP_DOWN(pv->osdd)))) {
        pv->loading = 1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(pv->typedd), 0);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(pv->osdd), 0);
        pv->loading = 0;
        fill_browser(pv);
    }
    for (i = 0; i < pv->nvis; i++)
        if (!strcmp(pv->plug[pv->vis[i]].path, path)) {
            GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv->list), i);
            /* Selecting the row that is already selected emits nothing, so
             * opening the plug-in already open -- a shell's Reload, after its
             * helper died for good -- loads it directly. */
            if (gtk_list_box_get_selected_row(GTK_LIST_BOX(pv->list)) == row)
                load(pv, &pv->plug[pv->vis[i]]);
            else
                gtk_list_box_select_row(GTK_LIST_BOX(pv->list), row);
            return;
        }
    if (pv->nplug >= MAX_PLUGINS) return;

    base = strrchr(path, '/');
    e = &pv->plug[pv->nplug];
    snprintf(e->path, sizeof e->path, "%s", path);
    snprintf(e->name, sizeof e->name, "%s", base ? base + 1 : path);
    pehost_classify(path, &info);
    snprintf(e->kind, sizeof e->kind, "%s", pehost_kind_label(info.kind));
    snprintf(e->os,   sizeof e->os,   "%s", info.os);
    snprintf(e->fmt,  sizeof e->fmt,  "%s", info.format);
    e->kindv = (int)info.kind;
    e->loadable = 1;                          /* pehost_can_load said so above */
    append_row(pv, e);
    pv->vis[pv->nvis++] = pv->nplug;
    pv->nplug++;

    /* Selecting it is what loads it -- one path in, not two. */
    gtk_list_box_select_row(GTK_LIST_BOX(pv->list),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv->list), pv->nvis - 1));
}

/* Opening an installer rather than a plug-in.
 *
 * The same thing the Qt window does, and for the same reason: half of what
 * people have is a setup.exe, an .msi or a macOS .pkg, and none of it needs to
 * be *run* -- the payload comes out without a Windows or a Mac to install
 * into. tools/vst_install.py does the unpacking; this finds it the way the
 * plug-in corpora are found, upwards from the binary. */
static int installer_ext(const char *path)
{
    static const char *ext[] = { ".exe", ".msi", ".zip", ".7z", ".dmg", ".pkg" };
    size_t i, n = strlen(path);
    for (i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        size_t e = strlen(ext[i]);
        if (n > e && !g_ascii_strcasecmp(path + n - e, ext[i])) return 1;
    }
    return 0;
}

/* Whether something found by walking up from the binary is a place to run
 * things from: owned by whoever is running this, or by root, and not writable
 * by everyone. The walk reaches /tmp from an AppImage's mount point, and what
 * is under /tmp is anybody's -- a planted "tools/" there would otherwise be
 * taken for ours. */
static int trusted_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (st.st_uid != geteuid() && st.st_uid != 0) return 0;
    return !(st.st_mode & S_IWOTH);
}

/* The installer's unpacker. An installed copy -- ../lib/vst-ace or
 * ../share/vst-ace beside the binary -- is looked for first and is taken as
 * it is. The development tree is found by walking up, and only what the user
 * or root owns is taken from there. */
static char *installer_tool(void)
{
    static const char *installed[] = { "../lib/vst-ace/vst_install.py",
                                       "../share/vst-ace/vst_install.py" };
    static const char *tree[] = { "tools/vst_install.py", "../tools/vst_install.py",
                                  "../lib/vst-ace/vst_install.py",
                                  "../share/vst-ace/vst_install.py" };
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    int up;
    size_t i;
    if (n <= 0) return NULL;
    exe[n] = 0;
    {   /* beside the binary, as installed */
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            for (i = 0; i < sizeof installed / sizeof installed[0]; i++) {
                char *cand = g_strdup_printf("%s/%s", exe, installed[i]);
                if (g_file_test(cand, G_FILE_TEST_IS_REGULAR)) return cand;
                g_free(cand);
            }
            *slash = '/';
        }
    }
    for (up = 0; up < 6; up++) {
        char *slash = strrchr(exe, '/');
        if (!slash) break;
        *slash = 0;
        for (i = 0; i < sizeof tree / sizeof tree[0]; i++) {
            char *cand = g_strdup_printf("%s/%s", exe, tree[i]);
            char *dir = g_path_get_dirname(cand);
            int ok = g_file_test(cand, G_FILE_TEST_IS_REGULAR) &&
                     trusted_path(cand) && trusted_path(dir);
            g_free(dir);
            if (ok) return cand;
            g_free(cand);
        }
    }
    return NULL;
}

/* Unpack an installer without holding the GTK main loop: the tool can run for
 * minutes on a big one, and a synchronous wait froze the whole window --
 * the keyboard, the meter, the editor -- until it finished. The tool runs as a
 * GSubprocess and the answer comes back to unpack_done on the main loop. */
typedef struct {
    plugview *pv;
    char     *dest;
    GSubprocess *proc;
} unpack_job;

/* The plug-in the tool's output names first, or NULL. The tool prints one
 * indented path per plug-in, under a heading. */
static char *unpack_first_plugin(const char *stdout_s, const char *dest)
{
    char *out = NULL;
    if (stdout_s) {
        char **lines = g_strsplit(stdout_s, "\n", -1);
        int i, in_list = 0;
        for (i = 0; lines[i] && !out; i++) {
            if (g_str_has_prefix(lines[i], "installed ")) { in_list = 1; continue; }
            if (!in_list || !g_str_has_prefix(lines[i], "    ")) { in_list = 0; continue; }
            { char *rel = g_strstrip(g_strdup(lines[i]));
              char *full = g_strdup_printf("%s/%s", dest, rel);
              if (g_file_test(full, G_FILE_TEST_EXISTS)) out = full; else g_free(full);
              g_free(rel); }
        }
        g_strfreev(lines);
    }
    return out;
}

static void unpack_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    unpack_job *job = ud;
    plugview   *pv = job->pv;
    char *stdout_s = NULL, *got;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &stdout_s, NULL, NULL);
    pv->unpacking = 0;
    /* plugview_shutdown clears the widgets: a window closed mid-unpack gets no
     * plug-in opened into it. */
    if (pv->list) {
        got = unpack_first_plugin(stdout_s, job->dest);
        if (got) { open_path(pv, got); g_free(got); }
        else plug_status(pv, "nothing came out of it");
    }
    g_free(stdout_s);
    g_free(job->dest);
    g_object_unref(job->proc);
    g_free(job);
}

static void unpack_installer(plugview *pv, const char *path)
{
    char *tool = installer_tool(), *dest;
    char *argv[6];
    unpack_job *job;
    GSubprocess *proc;

    if (!tool) {
        plug_status(pv, "the installer unpacker (tools/vst_install.py) is not "
                        "beside this program");
        return;
    }
    if (pv->unpacking) {
        plug_status(pv, "an installer is already being unpacked");
        g_free(tool);
        return;
    }
    dest = g_strdup_printf("%s/.vst", g_get_home_dir());
    argv[0] = (char *)"python3"; argv[1] = tool; argv[2] = (char *)path;
    argv[3] = (char *)"--dest";  argv[4] = dest; argv[5] = NULL;
    proc = g_subprocess_newv((const gchar * const *)argv,
                             G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL);
    g_free(tool);
    if (!proc) {
        plug_status(pv, "python3 is needed to unpack an installer");
        g_free(dest);
        return;
    }
    job = g_new0(unpack_job, 1);
    job->pv = pv; job->dest = dest; job->proc = proc;
    pv->unpacking = 1;
    plug_status(pv, "unpacking the installer...");
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, unpack_done, job);
}

/* ---- keeping a plug-in: its folder made one of the folders searched ---- */

/* The folders a system keeps plug-ins in already -- the ones nothing needs
 * adding for. */
static int in_standard_plugin_dir(const char *path)
{
    const char *home = g_get_home_dir();
    char *dirs[16];
    int n = 0, i, hit = 0;
    char want[PATH_MAX];
    const char *envs[] = { "VST_PATH", "VST3_PATH" };
    if (!realpath(path, want)) return 0;
    dirs[n++] = g_strdup_printf("%s/.vst", home);
    dirs[n++] = g_strdup_printf("%s/.vst3", home);
    dirs[n++] = g_strdup("/usr/lib/vst");
    dirs[n++] = g_strdup("/usr/lib/vst3");
    dirs[n++] = g_strdup("/usr/local/lib/vst");
    dirs[n++] = g_strdup("/usr/local/lib/vst3");
    for (i = 0; i < 2; i++) {
        const char *e = g_getenv(envs[i]);
        char **parts = e ? g_strsplit(e, ":", 0) : NULL;
        int k;
        for (k = 0; parts && parts[k] && n < 16; k++) if (*parts[k]) dirs[n++] = g_strdup(parts[k]);
        g_strfreev(parts);
    }
    for (i = 0; i < n; i++) {
        char root[PATH_MAX];
        size_t l;
        if (!hit && realpath(dirs[i], root)) {
            l = strlen(root);
            if (!strncmp(want, root, l) && (want[l] == '/' || l == 1)) hit = 1;
        }
        g_free(dirs[i]);
    }
    return hit;
}

/* "Don't ask again", and the plug-ins already declined, kept beside the
 * folder list so they outlive the session. */
static char *offers_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "vst-ace", "keep-offers", NULL);
}

static int offer_declined(const char *path)
{
    char *f = offers_file(), *txt = NULL;
    int declined = 0;
    if (g_file_get_contents(f, &txt, NULL, NULL)) {
        char **lines = g_strsplit(txt, "\n", 0);
        int i;
        for (i = 0; lines[i]; i++)
            if (!strcmp(lines[i], "*") || !strcmp(lines[i], path)) declined = 1;
        g_strfreev(lines);
        g_free(txt);
    }
    g_free(f);
    return declined;
}

static void offer_remember(const char *what)
{
    char *f = offers_file(), *dir = g_path_get_dirname(f);
    FILE *fp;
    g_mkdir_with_parents(dir, 0700);
    if ((fp = fopen(f, "a"))) { fprintf(fp, "%s\n", what); fclose(fp); }
    g_free(dir);
    g_free(f);
}

/* The folder a plug-in sits in: its own directory, for a file; the one
 * holding it, for a bundle. */
static char *folder_of(const char *path)
{
    char *d = g_path_get_dirname(path);
    return d;
}

static void keep_folder(plugview *pv, const char *path)
{
    char *dir = folder_of(path);
    int r = vstdirs_add(VSTDIRS_ANY, dir);
    if (r < 0) plug_status(pv, "could not save the folder list to %s", vstdirs_file());
    else {
        plug_status(pv, r ? "%s is now one of your plug-in folders" : "%s is already one of your plug-in folders", dir);
        if (r) rescan_all(pv);
    }
    g_free(dir);
}

typedef struct { plugview *pv; char *path; } keep_ask;

static void keep_answered(GObject *src, GAsyncResult *res, gpointer u)
{
    keep_ask *k = u;
    int b = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    if (b == 2) keep_folder(k->pv, k->path);              /* Keep its folder */
    else if (b == 1) offer_remember("*");                  /* Don't ask again */
    else offer_remember(k->path);                          /* Just open it: not this one again */
    g_object_unref(src);
    g_free(k->path);
    g_free(k);
}

/* A plug-in opened by hand from somewhere that is not a folder of plug-ins:
 * offer to keep its folder, so it is listed every session. */
static void offer_keep(plugview *pv, GtkWindow *parent, const char *path)
{
    static const char *buttons[] = { "Just open it", "Don't ask again", "Keep its folder", NULL };
    GtkAlertDialog *d;
    keep_ask *k;
    char *dir, *msg;
    if (!pv->host || strcmp(pv->loaded_path, path)) return;       /* it did not load */
    if (vstdirs_contains(path) || in_standard_plugin_dir(path) || offer_declined(path)) return;
    dir = folder_of(path);
    msg = g_strdup_printf("%s is not in any of your plug-in folders", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    d = gtk_alert_dialog_new("%s", msg);
    {
        char *detail = g_strdup_printf("Keep %s as one of your plug-in folders, and every plug-in in it is "
                                       "listed each time you start. Plug-in Folders changes the list later.", dir);
        gtk_alert_dialog_set_detail(d, detail);
        g_free(detail);
    }
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 2);
    k = g_new0(keep_ask, 1);
    k->pv = pv;
    k->path = g_strdup(path);
    gtk_alert_dialog_choose(d, parent, NULL, keep_answered, k);
    g_free(msg);
    g_free(dir);
}

static void on_vst_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *err = NULL;
    GFile  *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
    char   *path;
    plugview *pv = ud;

    g_clear_error(&err);
    g_object_unref(src);                         /* the ref taken in open_vst */
    if (!f) return;                              /* cancelled */
    if ((path = g_file_get_path(f))) {
        if (installer_ext(path)) unpack_installer(pv, path);
        else {
            open_path(pv, path);
            {
                GtkRoot *rt = pv->root ? gtk_widget_get_root(pv->root) : NULL;
                offer_keep(pv, rt && GTK_IS_WINDOW(rt) ? GTK_WINDOW(rt) : NULL, path);
            }
        }
        g_free(path);
    }
    g_object_unref(f);
}

void plugview_open_vst(plugview *pv, GtkWindow *parent)
{
    GtkFileDialog *d = gtk_file_dialog_new();

    gtk_file_dialog_set_title(d, "Open VST");
    gtk_file_dialog_open(d, parent, NULL, on_vst_chosen, pv);
}


/* -------------------------------------------------------- Add plug-in ---- */

/* Plugins > Add plug-in: one file (or bundle) the user picks, either for this
 * session only or copied where plug-ins of its kind belong. What kind it is
 * comes from the binary, not the name, and the kind decides the folder: a
 * folder the user already set up for that platform if there is one, else a
 * place of this program's own under the data directory. Native Linux plug-ins
 * go to the standard ~/.vst and ~/.vst3. */
typedef struct {
    plugview *pv;
    char path[1024], os[16], fmt[8], arch[16], target[1024];
} addctx;

static void install_dir_for(const char *os, const char *fmt, const char *arch,
                            char *out, size_t n)
{
    vstdir dirs[VSTDIRS_MAX];
    char first[1024] = "", byfmt[1024] = "", lfmt[16];
    int i, nd;
    const char *data;
    char *lc;

    if (!strcmp(os, "linux")) {
        snprintf(out, n, "%s/%s", g_get_home_dir(), !strcmp(fmt, "VST3") ? ".vst3" : ".vst");
        return;
    }
    nd = vstdirs_load(dirs, VSTDIRS_MAX);
    snprintf(lfmt, sizeof lfmt, "%s", fmt);
    lc = g_ascii_strdown(lfmt, -1);
    for (i = 0; i < nd; i++) {
        char *lp;
        if (strcmp(os, dirs[i].os) || !g_file_test(dirs[i].path, G_FILE_TEST_IS_DIR)) continue;
        if (!first[0]) snprintf(first, sizeof first, "%s", dirs[i].path);
        lp = g_ascii_strdown(dirs[i].path, -1);
        if (!byfmt[0] && *lc && strstr(lp, lc)) snprintf(byfmt, sizeof byfmt, "%s", dirs[i].path);
        g_free(lp);
    }
    g_free(lc);
    if (byfmt[0]) { snprintf(out, n, "%s", byfmt); return; }
    if (first[0]) { snprintf(out, n, "%s", first); return; }
    data = g_get_user_data_dir();
    snprintf(out, n, "%s/vst-ace/plugins/%s/%s%s%s", data, os, fmt, arch[0] ? "-" : "", arch);
}

/* Copy a file or a whole folder, recursively. 0 on success. */
static int copy_tree(const char *src, const char *dst)
{
    if (g_file_test(src, G_FILE_TEST_IS_DIR)) {
        GDir *d = g_dir_open(src, 0, NULL);
        const char *nm;
        int rc = 0;
        if (!d || g_mkdir_with_parents(dst, 0755) != 0) { if (d) g_dir_close(d); return -1; }
        while (!rc && (nm = g_dir_read_name(d))) {
            char *s2 = g_build_filename(src, nm, NULL), *d2 = g_build_filename(dst, nm, NULL);
            rc = copy_tree(s2, d2);
            g_free(s2); g_free(d2);
        }
        g_dir_close(d);
        return rc;
    } else {
        GFile *a = g_file_new_for_path(src), *b = g_file_new_for_path(dst);
        int rc = g_file_copy(a, b, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL) ? 0 : -1;
        g_object_unref(a); g_object_unref(b);
        return rc;
    }
}

static void (*g_list_changed)(void);
void plugview_set_list_changed(void (*cb)(void)) { g_list_changed = cb; }

static void on_add_answer(GObject *src, GAsyncResult *res, gpointer ud)
{
    addctx *c = ud;
    plugview *pv = c->pv;
    int pick = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    const char *nm = strrchr(c->path, '/') ? strrchr(c->path, '/') + 1 : c->path;

    if (pick == 1) {                                   /* this session only */
        int i;
        for (i = 0; i < g_nsession_files; i++) if (!strcmp(g_session_files[i], c->path)) break;
        if (i == g_nsession_files && g_nsession_files < 64)
            snprintf(g_session_files[g_nsession_files++], sizeof g_session_files[0], "%s", c->path);
        rescan_all(pv);
        plug_status(pv, "%s added for this session", nm);
    } else if (pick == 2) {                            /* install */
        char *srcp = g_strdup(c->path), *dest;
        if (!g_file_test(c->path, G_FILE_TEST_IS_DIR)) {
            /* A lone plug-in sitting among other files loads its data from
             * beside itself: the whole folder goes. */
            char *par = g_path_get_dirname(c->path);
            GDir *d = g_dir_open(par, 0, NULL);
            const char *e;
            int plugins = 0, others = 0;
            while (d && (e = g_dir_read_name(d))) {
                char *ep = g_build_filename(par, e, NULL);
                if (is_candidate(ep, e, is_dir(ep))) plugins++; else others++;
                g_free(ep);
            }
            if (d) g_dir_close(d);
            if (plugins == 1 && others > 0) { g_free(srcp); srcp = par; par = NULL; }
            g_free(par);
        }
        {
            char *base = g_path_get_basename(srcp);
            dest = g_build_filename(c->target, base, NULL);
            g_free(base);
        }
        if (copy_tree(srcp, dest) != 0) {
            plug_status(pv, "could not install %s into %s", nm, c->target);
        } else {
            vstdirs_add(c->os, c->target);
            rescan_all(pv);
            plug_status(pv, "%s installed into %s", nm, c->target);
        }
        g_free(srcp); g_free(dest);
    }
    if (g_list_changed) g_list_changed();
    g_object_unref(src);
    g_free(c);
}

static void on_add_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    plugview *pv = ud;
    GFile *f = g_object_get_data(G_OBJECT(src), "bundle")
             ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL)
             : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    g_object_unref(src);
    if (!f) return;
    if ((path = g_file_get_path(f))) {
        pehost_info info;
        pehost_classify(path, &info);
        if (info.kind == PEHOST_KIND_UNKNOWN) {
            plug_status(pv, "%s is not a plug-in this host recognises",
                        strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
        } else {
            addctx *c = g_new0(addctx, 1);
            GtkAlertDialog *d;
            const char *buttons[] = { "Cancel", "This session only", "Install", NULL };
            char *msg, *detail;
            GtkRoot *rt = pv->root ? gtk_widget_get_root(pv->root) : NULL;
            c->pv = pv;
            snprintf(c->path, sizeof c->path, "%s", path);
            snprintf(c->os, sizeof c->os, "%s", info.os);
            snprintf(c->fmt, sizeof c->fmt, "%s", info.format);
            snprintf(c->arch, sizeof c->arch, "%s", info.arch);
            install_dir_for(c->os, c->fmt, c->arch, c->target, sizeof c->target);
            msg = g_strdup_printf("Add %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
            detail = g_strdup_printf("%s %s%s%s%s\n\nUse it for this session only, or install it "
                                     "where plug-ins of this kind are kept:\n%s%s%s",
                                     os_label(c->os), c->fmt, c->arch[0] ? " (" : "", c->arch,
                                     c->arch[0] ? ")" : "", c->target,
                                     info.loadable ? "" : "\n\nThis build may not be able to run it: ",
                                     info.loadable ? "" : (info.why[0] ? info.why : "unsupported"));
            d = gtk_alert_dialog_new("%s", msg);
            gtk_alert_dialog_set_detail(d, detail);
            gtk_alert_dialog_set_buttons(d, buttons);
            gtk_alert_dialog_set_cancel_button(d, 0);
            gtk_alert_dialog_set_default_button(d, 1);
            gtk_alert_dialog_choose(d, rt && GTK_IS_WINDOW(rt) ? GTK_WINDOW(rt) : NULL,
                                    NULL, on_add_answer, c);
            g_free(msg); g_free(detail);
        }
        g_free(path);
    }
    g_object_unref(f);
}

void plugview_add_plugin(plugview *pv, GtkWindow *parent, int bundle)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, bundle ? "Add plug-in bundle (.vst3, .vst, .component)"
                                        : "Add plug-in");
    if (bundle) {
        g_object_set_data(G_OBJECT(d), "bundle", GINT_TO_POINTER(1));
        gtk_file_dialog_select_folder(d, parent, NULL, on_add_chosen, pv);
    } else {
        gtk_file_dialog_open(d, parent, NULL, on_add_chosen, pv);
    }
}

/* ------------------------------------------------------------- patches ---- */

/* A plug-in's own programs are its factory presets and are read-only; this is
 * where a sound somebody made goes. The file is the same JSON the command line
 * reads and writes -- parameter names and values -- so a patch saved here opens
 * with `va peload <patch.json>`, and a bank written by hand opens here.
 *
 * pestudio shows every patch in a bank as a list; this window applies the first
 * and says so. A single saved patch is a bank of one, so the common case is the
 * same in both. */
static void on_patch_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    char err[256] = "";
    plugview *pv = ud;
    if (!f) return;
    if ((path = g_file_get_path(f)) != NULL) {
        char with_ext[1100];
        size_t n = strlen(path);
        if (n < 5 || g_ascii_strcasecmp(path + n - 5, ".json"))
            snprintf(with_ext, sizeof with_ext, "%s.json", path);
        else
            snprintf(with_ext, sizeof with_ext, "%s", path);
        if (!pv->host)
            plug_status(pv, "load a plug-in first");
        else if (patch_save(pv->host, with_ext,
                            pv->loaded_path[0] ? pv->loaded_path : NULL,
                            err, sizeof err) != 0)
            plug_status(pv, "save failed: %s", err);
        else
            plug_status(pv, "saved %s", strrchr(with_ext, '/')
                                        ? strrchr(with_ext, '/') + 1 : with_ext);
        g_free(path);
    }
    g_object_unref(f);
}

/* Both dialogs start in the directory patch_find_for searches, so a patch
 * saved without thinking about where it goes is found again without thinking
 * about where it went. Anywhere else still works -- a patch beside its plug-in
 * is found too -- it just is not the default. */
static void patch_dialog_start_here(GtkFileDialog *d)
{
    const char *dir = patch_user_dir();
    GFile *f;
    if (!dir || !*dir) return;
    f = g_file_new_for_path(dir);
    gtk_file_dialog_set_initial_folder(d, f);
    g_object_unref(f);
}

void plugview_save_patch(plugview *pv, GtkWindow *parent)
{
    GtkFileDialog *d;
    char suggest[160] = "patch.json";

    if (!pv->host) { plug_status(pv, "load a plug-in first"); return; }
    patch_user_dir_ensure();              /* saving is the ask; looking is not */
    if (pv->loaded_path[0]) {
        const char *base = strrchr(pv->loaded_path, '/');
        const char *dot;
        base = base ? base + 1 : pv->loaded_path;
        snprintf(suggest, sizeof suggest, "%s", base);
        if ((dot = strrchr(suggest, '.')) != NULL) *(char *)dot = 0;
        g_strlcat(suggest, ".json", sizeof suggest);
    }
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Save patch");
    gtk_file_dialog_set_initial_name(d, suggest);
    patch_dialog_start_here(d);
    gtk_file_dialog_save(d, parent, NULL, on_patch_save_chosen, pv);
    g_object_unref(d);
}

static void on_patch_open_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    plugview *pv = ud;
    if (!f) return;
    if ((path = g_file_get_path(f)) != NULL) {
        char err[256] = "";
        int applied = 0, missed = 0;
        if (!pv->host) {
            plug_status(pv, "load a plug-in first");
        } else if (patch_load(pv->host, path, err, sizeof err, &applied, &missed) != 0) {
            plug_status(pv, "open failed: %s", err);
        } else {
            /* The values changed under the list, so it has to be rebuilt --
             * otherwise the patch is audible and invisible. */
            fill_params(pv);
            if (missed)
                plug_status(pv, "%d parameter(s) set, %d matched nothing%s%s",
                            applied, missed, err[0] ? " -- " : "", err);
            else
                plug_status(pv, "%d parameter(s) set%s%s", applied,
                            err[0] ? " -- " : "", err);
        }
        g_free(path);
    }
    g_object_unref(f);
}

void plugview_load_patch(plugview *pv, GtkWindow *parent)
{
    GtkFileDialog *d;

    if (!pv->host) { plug_status(pv, "load a plug-in first"); return; }
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Open patch");
    patch_dialog_start_here(d);
    gtk_file_dialog_open(d, parent, NULL, on_patch_open_chosen, pv);
    g_object_unref(d);
}

static void on_dir_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    plugview *pv = ud;
    if (!f) return;
    {
        char *path = g_file_get_path(f);
        if (path) {
            /* Remembered, not just browsed: a folder chosen here is one the
             * user means to keep, and it is the same list Settings edits and
             * pestudio reads. Untagged -- File > Load Folder does not ask
             * which platform, and Settings is where that is chosen. */
            if (vstdirs_add(VSTDIRS_ANY, path) < 0)
                plug_status(pv, "could not save the folder list to %s", vstdirs_file());
            roots_add(pv, path, "user folder");
            rescan_all(pv);
            g_free(path);
        }
    }
    g_object_unref(f);
}

void plugview_load_folder(plugview *pv, GtkWindow *parent)
{
    GtkFileDialog *d = gtk_file_dialog_new();

    gtk_file_dialog_set_title(d, "Load plug-in folder");
    gtk_file_dialog_select_folder(d, parent, NULL, on_dir_chosen, pv);
    g_object_unref(d);
}


/* ------------------------------------------------- settings: plug-in folders */

/* Where the plug-ins are, as a list the user owns rather than a folder they
 * happen to be browsing.
 *
 * File > Load Folder adds one in passing; this is the list itself, so a folder
 * can be taken off the scan as well as put on it, and so there is somewhere to
 * look to find out why a plug-in is or is not being found. It is the same file
 * pestudio reads and writes -- one answer per machine to "where are my
 * plug-ins", not one per window. See vstdirs.h.
 *
 * Folders are grouped by the platform they hold, so a Windows corpus, a macOS
 * one and a Linux one are separate entries. That tag is what the folder is
 * *for*; it does not decide what is in it, because every plug-in is identified
 * from its own binary when it is scanned. */
static const char *const g_dir_groups[] = {
    VSTDIRS_WINDOWS, VSTDIRS_LINUX, VSTDIRS_MACOS, VSTDIRS_CLASSIC, VSTDIRS_ANY
};

static void folders_refill(plugview *pv);

static void on_folder_row(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
    plugview *pv = ud;
    const char *p;
    (void)lb;
    pv->folders.sel[0] = 0;
    if (row && (p = g_object_get_data(G_OBJECT(row), "path")))
        snprintf(pv->folders.sel, sizeof pv->folders.sel, "%s", p);
    if (pv->folders.rm) gtk_widget_set_sensitive(pv->folders.rm, pv->folders.sel[0] != 0);
}

static void folders_refill(plugview *pv)
{
    vstdir dirs[VSTDIRS_MAX];
    int    n, i, added = 0;
    size_t g;

    if (!pv->folders.list) return;
    clear_list(pv->folders.list);
    pv->folders.sel[0] = 0;
    if (pv->folders.rm) gtk_widget_set_sensitive(pv->folders.rm, FALSE);

    n = vstdirs_load(dirs, VSTDIRS_MAX);
    /* In the order the browser lists plug-ins, so the Windows folders read
     * together and the native ones together. */
    for (g = 0; g < sizeof g_dir_groups / sizeof g_dir_groups[0]; g++)
        for (i = 0; i < n; i++) {
            char       text[1200];
            GtkWidget *l;
            GtkListBoxRow *row;

            if (strcmp(dirs[i].os, g_dir_groups[g])) continue;
            snprintf(text, sizeof text, "%-12s  %s%s",
                     vstdirs_os_label(dirs[i].os), dirs[i].path,
                     is_dir(dirs[i].path) ? "" : "   -- missing");
            l = gtk_label_new(text);
            gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
            gtk_widget_set_margin_start(l, 4);
            gtk_widget_set_margin_end(l, 4);
            gtk_list_box_append(GTK_LIST_BOX(pv->folders.list), l);
            /* The path lives on the row, not in a parallel array: rows are
             * built group by group, so a row's position is not an index into
             * anything the caller still has. */
            row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv->folders.list), added++);
            if (row) g_object_set_data_full(G_OBJECT(row), "path",
                                            g_strdup(dirs[i].path), g_free);
        }
}

/* What on_folder_chosen needs from on_folder_add: the instance, and which
 * platform group the folder was filed under. GTK hands a callback one user
 * pointer, so the two travel together. */
typedef struct { plugview *pv; char *os; } folder_pick;

static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    folder_pick *pick = ud;
    plugview    *pv = pick->pv;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    const char *os = pick->os;
    char       *path;

    g_object_unref(src);
    if (f) {
        if ((path = g_file_get_path(f))) {
            if (vstdirs_add(os, path) < 0)
                plug_status(pv, "could not save the folder list to %s", vstdirs_file());
            folders_refill(pv);
            /* The scan set changed, so the browser behind the dialog is out of
             * date the moment this returns. */
            roots_discover(pv); roots_add_standard(pv); roots_add_user(pv);
            roots_add_fallback(pv);
            if (pv->session_dir[0]) roots_add(pv, pv->session_dir, "given folder");
            rescan_all(pv);
            g_free(path);
        }
        g_object_unref(f);
    }
    g_free(pick->os);
    g_free(pick);
}

static void on_folder_add(GtkButton *b, gpointer ud)
{
    plugview      *pv = ud;
    GtkFileDialog *d = gtk_file_dialog_new();
    guint          i = gtk_drop_down_get_selected(GTK_DROP_DOWN(pv->folders.asdd));
    const char    *os = i < sizeof g_dir_groups / sizeof g_dir_groups[0]
                            ? g_dir_groups[i] : VSTDIRS_ANY;
    char           title[64];
    folder_pick   *pick = g_new(folder_pick, 1);

    (void)b;
    pick->pv = pv;
    pick->os = g_strdup(os);
    snprintf(title, sizeof title, "Add %s plug-in folder", vstdirs_os_label(os));
    gtk_file_dialog_set_title(d, title);
    gtk_file_dialog_select_folder(d, GTK_WINDOW(pv->folders.win), NULL,
                                  on_folder_chosen, pick);
}

static void on_folder_remove(GtkButton *b, gpointer ud)
{
    plugview *pv = ud;
    (void)b;
    if (!pv->folders.sel[0]) return;
    vstdirs_remove(pv->folders.sel);
    folders_refill(pv);
    roots_discover(pv); roots_add_standard(pv); roots_add_user(pv); roots_add_fallback(pv);
    if (pv->session_dir[0]) roots_add(pv, pv->session_dir, "given folder");
    rescan_all(pv);
}

static void on_folder_close(GtkButton *b, gpointer ud)
{
    plugview *pv = ud;
    (void)b;
    if (pv->folders.win) gtk_window_destroy(GTK_WINDOW(pv->folders.win));
}

static void on_folders_gone(GtkWidget *w, gpointer ud)
{
    plugview *pv = ud;
    (void)w;
    pv->folders.win = NULL; pv->folders.list = NULL; pv->folders.rm = NULL; pv->folders.asdd = NULL;
}

/* Send text to whatever the editor has focused, as if it were typed.
 *
 * Each character goes down, as a character, and up, because a control that
 * reads only one of the three is common; Return follows, since a field that
 * commits on Enter commits on nothing else. The editor is pumped between keys
 * so a plug-in that repaints per keystroke gets the chance to. */
static void plugview_type(plugview *pv, const char *text)
{
    const unsigned char *t;
    if (!pv->host || !pv->ed_open || !text) return;
    for (t = (const unsigned char *)text; *t; t++) {
        int ch = *t, vk;
        if (ch < 32 || ch > 126) continue;
        vk = (ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch;
        pehost_editor_key(pv->host, vk, 1, ch);
        pehost_editor_pump(pv->host);
        pehost_editor_key(pv->host, vk, 0, ch);
        pehost_editor_pump(pv->host);
    }
    pehost_editor_key(pv->host, 0x0D, 1, 13);          /* VK_RETURN */
    pehost_editor_pump(pv->host);
    pehost_editor_key(pv->host, 0x0D, 0, 0);
    pehost_editor_pump(pv->host);
}

static void key_status(plugview *pv, const char *msg)
{
    if (pv->status) gtk_label_set_text(GTK_LABEL(pv->status), msg);
}

/* Both the Enter key on the field and the button land here; the window to
 * close is hung off the entry so one callback serves both signals. */
static void on_key_entry_done(GtkWidget *entry, gpointer ud)
{
    plugview   *pv = g_object_get_data(G_OBJECT(entry), "pv");
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    GtkWidget *win = ud ? GTK_WIDGET(ud)
                        : GTK_WIDGET(g_object_get_data(G_OBJECT(entry), "win"));
    char msg[128];
    int n = text ? (int)strlen(text) : 0;
    if (n > 0) {
        plugview_type(pv, text);
        snprintf(msg, sizeof msg, "sent %d character(s) to the editor", n);
        key_status(pv, msg);
    }
    if (win) gtk_window_destroy(GTK_WINDOW(win));
}

static void on_key_send_clicked(GtkButton *b, gpointer entry)
{ (void)b; on_key_entry_done(GTK_WIDGET(entry), NULL); }

void plugview_enter_key(plugview *pv, GtkWindow *parent)
{
    GtkWidget *win, *box, *lbl, *entry, *send;

    if (!pv->host || !pv->ed_open) {
        key_status(pv, "open a plug-in and its editor first");
        return;
    }
    win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Enter key or serial");
    gtk_window_set_default_size(GTK_WINDOW(win), 460, -1);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 10);    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);  gtk_widget_set_margin_end(box, 10);

    lbl = gtk_label_new("Click the plug-in's own field first, then type the key "
                        "here.\nIt is sent to the editor a character at a time, "
                        "then Return.");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_box_append(GTK_BOX(box), lbl);

    entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    g_signal_connect(entry, "activate", G_CALLBACK(on_key_entry_done), win);
    gtk_box_append(GTK_BOX(box), entry);

    g_object_set_data(G_OBJECT(entry), "win", win);
    g_object_set_data(G_OBJECT(entry), "pv", pv);
    send = gtk_button_new_with_label("Send to editor");
    g_signal_connect(send, "clicked", G_CALLBACK(on_key_send_clicked), entry);
    gtk_box_append(GTK_BOX(box), send);

    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
}

/* Synth > Keep This Plug-in's Folder: the same as the offer, asked for. */
void plugview_keep_folder(plugview *pv)
{
    if (!pv->host || !pv->loaded_path[0]) { plug_status(pv, "load a plug-in first"); return; }
    keep_folder(pv, pv->loaded_path);
}

/* What the browser is listing, for a shell's plug-in manager. */
int plugview_available_count(plugview *pv) { return pv->nplug; }

void plugview_available(plugview *pv, int i, const char **path, const char **name,
                        const char **kind, int *loadable)
{
    if (i < 0 || i >= pv->nplug) return;
    if (path) *path = pv->plug[i].path;
    if (name) *name = pv->plug[i].name;
    if (kind) *kind = pv->plug[i].kind;
    if (loadable) *loadable = pv->plug[i].loadable;
}

void plugview_rescan(plugview *pv) { rescan_all(pv); }

void plugview_edit_folders(plugview *pv, GtkWindow *parent)
{
    GtkWidget     *box, *sw, *btns, *lbl, *close;
    GtkStringList *asmodel;
    size_t         g;
    int            i;

    if (pv->folders.win) { gtk_window_present(GTK_WINDOW(pv->folders.win)); return; }

    pv->folders.win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(pv->folders.win), "Plug-in folders");
    gtk_window_set_default_size(GTK_WINDOW(pv->folders.win), 660, 440);
    gtk_window_set_transient_for(GTK_WINDOW(pv->folders.win), parent);
    gtk_window_set_modal(GTK_WINDOW(pv->folders.win), TRUE);
    g_signal_connect(pv->folders.win, "destroy", G_CALLBACK(on_folders_gone), pv);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 10);    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);  gtk_widget_set_margin_end(box, 10);

    lbl = gtk_label_new("Folders searched for plug-ins. Kept between sessions "
                        "and shared with the Qt window.");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_box_append(GTK_BOX(box), lbl);

    pv->folders.list = gtk_list_box_new();
    g_signal_connect(pv->folders.list, "row-selected", G_CALLBACK(on_folder_row), pv);
    sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), pv->folders.list);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);

    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(btns), gtk_label_new("Add as:"));
    asmodel = gtk_string_list_new(NULL);
    for (g = 0; g < sizeof g_dir_groups / sizeof g_dir_groups[0]; g++)
        gtk_string_list_append(asmodel, vstdirs_os_label(g_dir_groups[g]));
    pv->folders.asdd = gtk_drop_down_new(G_LIST_MODEL(asmodel), NULL);
    gtk_box_append(GTK_BOX(btns), pv->folders.asdd);
    {
        GtkWidget *add = gtk_button_new_with_label("Add Folder…");
        g_signal_connect(add, "clicked", G_CALLBACK(on_folder_add), pv);
        gtk_box_append(GTK_BOX(btns), add);
    }
    pv->folders.rm = gtk_button_new_with_label("Remove");
    gtk_widget_set_sensitive(pv->folders.rm, FALSE);
    g_signal_connect(pv->folders.rm, "clicked", G_CALLBACK(on_folder_remove), pv);
    gtk_box_append(GTK_BOX(btns), pv->folders.rm);
    gtk_box_append(GTK_BOX(box), btns);

    /* Said out loud, because a settings page that groups things by platform
     * looks like it decides the platform, and this one does not. */
    lbl = gtk_label_new("The platform says what a folder is for. Every plug-in is "
                        "still identified by its own binary when it is scanned, so "
                        "a Windows plug-in in the Linux folder is listed as Windows.");
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_add_css_class(lbl, "dim-label");
    gtk_box_append(GTK_BOX(box), lbl);

    /* What is scanned without being asked for. Read-only on purpose: these
     * come from where the binary sits and from the system VST directories, so
     * removing one here would only be undone by the next launch. */
    {
        GtkWidget *bl = gtk_list_box_new();
        GtkWidget *bsw = gtk_scrolled_window_new();
        vstdir     ud[VSTDIRS_MAX];
        int        un = vstdirs_load(ud, VSTDIRS_MAX), k, shown = 0;

        for (i = 0; i < pv->nroot; i++) {
            char line[1200];
            int  mine = 0;
            for (k = 0; k < un; k++)
                if (!strcmp(ud[k].path, pv->roots[i].path)) { mine = 1; break; }
            if (mine) continue;
            snprintf(line, sizeof line, "%s   (%s)", pv->roots[i].path, pv->roots[i].label);
            gtk_list_box_append(GTK_LIST_BOX(bl), gtk_label_new(line));
            shown++;
        }
        if (shown) {
            lbl = gtk_label_new("Also searched, found automatically:");
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_widget_add_css_class(lbl, "dim-label");
            gtk_box_append(GTK_BOX(box), lbl);
            gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(bsw), bl);
            gtk_widget_set_size_request(bsw, -1, 96);
            gtk_box_append(GTK_BOX(box), bsw);
        }
    }

    {
        char where[1200];
        snprintf(where, sizeof where, "Stored in %s", vstdirs_file());
        lbl = gtk_label_new(where);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_box_append(GTK_BOX(box), lbl);
    }

    close = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    g_signal_connect(close, "clicked", G_CALLBACK(on_folder_close), pv);
    gtk_box_append(GTK_BOX(box), close);

    gtk_window_set_child(GTK_WINDOW(pv->folders.win), box);
    folders_refill(pv);
    gtk_window_present(GTK_WINDOW(pv->folders.win));
}

/* ---------------------------------------------------- installing plug-in data */

/* Link in what the scanned plug-ins are missing and this machine already has.
 *
 * Only the repairable half: a u-he release carries the Images and Fonts its
 * installer would have copied into ~/.u-he/<Product>/, so a plug-in unpacked
 * rather than installed can be pointed at its own artwork. The firmware a Virus
 * or Waldorf emulation wants is not in any download and cannot be conjured --
 * those are reported and left alone.
 *
 * Deliberately a menu command and not something load() does on its own: it
 * writes outside the tree, into the user's home, and that is a decision to be
 * taken rather than a side effect of clicking a plug-in in a list. */
void plugview_install_missing_data(plugview *pv)
{
    int i, files = 0, plugins = 0, failed = 0;
    char lasterr[160] = "";

    for (i = 0; i < pv->nplug; i++) {
        pehost_data_need dn;
        char err[160];
        int  n;

        if (!pv->plug[i].repairable) continue;
        /* Re-checked rather than trusting what the scan recorded: the folders
         * may have moved, and this is the call that is about to write. */
        if (!pehost_data_check(pv->plug[i].path, &dn) || !dn.repairable) continue;
        if ((n = pehost_data_repair(&dn, err, (int)sizeof err)) > 0) {
            files += n;
            plugins++;
            fprintf(stderr, "plugview: %s -- linked %d item(s) into %s\n",
                    dn.product, n, dn.where);
        } else {
            failed++;
            snprintf(lasterr, sizeof lasterr, "%s", err);
        }
    }

    if (!plugins && !failed) {
        plug_status(pv, "nothing to install -- no scanned plug-in is missing data "
                    "that this machine has a copy of");
        return;
    }
    if (!plugins) {
        plug_status(pv, "could not install: %s", lasterr[0] ? lasterr : "unknown error");
        return;
    }
    /* Reload, because these are read when the editor is built: a plug-in that
     * is already open went looking before the folders existed. */
    plug_status(pv, "linked %d folder(s) for %d plug-in(s)%s -- reload one to see it",
                files, plugins, failed ? ", some failed" : "");
    rescan_all(pv);
}

/* ------------------------------------------------------------ the audio API */

/* The window's accelerators reach the pane through these. A list that has
 * focus is a list the arrow keys walk, which is the whole of picking a plug-in
 * without a mouse -- row-selected loads it as if it had been clicked. */
/* The row, not the list.
 *
 * A GtkListBox is not focusable itself -- its rows are -- so grabbing focus on
 * the box quietly does nothing and the arrow keys go on being handled by
 * whatever had focus before, which looks exactly like the shortcut not being
 * bound. Focus the selected row, or the first one when nothing is selected. */
static void focus_row(GtkWidget *box)
{
    GtkListBoxRow *r;
    if (!GTK_IS_LIST_BOX(box)) return;
    r = gtk_list_box_get_selected_row(GTK_LIST_BOX(box));
    if (!r) r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(box), 0);
    if (r) gtk_widget_grab_focus(GTK_WIDGET(r));
}

void plugview_focus_list(plugview *pv)     { focus_row(pv->list); }
void plugview_focus_programs(plugview *pv) { focus_row(pv->proglist); }

void plugview_toggle_editor(plugview *pv)
{
    const char *page;
    if (!GTK_IS_WIDGET(pv->stack)) return;
    page = gtk_stack_get_visible_child_name(GTK_STACK(pv->stack));
    gtk_stack_set_visible_child_name(GTK_STACK(pv->stack),
        page && !strcmp(page, "editor") ? "params" : "editor");
}

int plugview_active(plugview *pv)
{ return atomic_load_explicit(&pv->live, memory_order_acquire); }

int plugview_render_io(plugview *pv, const float *in, float *out, int frames)
{
    float pk = 0.0f;
    int   i, cur;

    /* Injected MIDI goes in first, so a note due at this block's very start
     * still makes the block. Drained whether or not a plug-in is loaded:
     * with none there is nothing to play to, so due events are discarded
     * rather than saved up to burst in when one loads. */
    {
        struct timespec ts;
        double bwall;
        unsigned t, h;

        /* When this block starts, on the clock the injected MIDI is stamped
         * with. Measured here rather than derived from PipeWire's time: what
         * matters is agreement with the sequencer's CLOCK_MONOTONIC. */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        bwall = ts.tv_sec + ts.tv_nsec / 1e9;
        t = atomic_load_explicit(&pv->inj_tail, memory_order_relaxed);
        h = atomic_load_explicit(&pv->inj_head, memory_order_acquire);
        while (t != h) {
            const double f = (pv->inj[t % PV_INJQ].wall - bwall) * pv->rate;
            if (f >= frames) break;              /* a later block's */
            if (pv->host) {
                pehost_midi_at(pv->host, pv->inj[t % PV_INJQ].st,
                               pv->inj[t % PV_INJQ].d1, pv->inj[t % PV_INJQ].d2,
                               f < 0 ? 0 : (int)f);
                atomic_fetch_add_explicit(&pv->inj_placed, 1, memory_order_relaxed);
            }
            t++;
        }
        atomic_store_explicit(&pv->inj_tail, t, memory_order_release);
    }

    if (!atomic_load_explicit(&pv->live, memory_order_acquire) || !pv->host) return 0;
    /* `in` is the captured signal, interleaved stereo, or NULL when there is
     * no input open. A synth ignores it either way; an effect with nothing to
     * process renders silence, which is why this window used to be silent for
     * every effect in the corpus -- it passed NULL unconditionally. */
    pehost_render_io(pv->host, in, out, frames);

    /* Peak for the meter. Kept here rather than in the GTK thread because this
     * is the only place the samples exist, and "it loaded and the editor drew"
     * is not the same claim as "it is audible". */
    for (i = 0; i < frames * 2; i++) {
        float a = out[i] < 0.0f ? -out[i] : out[i];
        if (a > pk) pk = a;
    }
    cur = (int)(pk * 1000.0f);
    if (cur > atomic_load_explicit(&pv->peak_milli, memory_order_relaxed))
        atomic_store_explicit(&pv->peak_milli, cur, memory_order_relaxed);
    return 1;
}

/* The old spelling, for callers with nothing to feed it. */
int plugview_render(plugview *pv, float *out, int frames)
{ return plugview_render_io(pv, NULL, out, frames); }

int plugview_load_path(plugview *pv, const char *path)
{
    open_path(pv, path);
    /* Loaded means *this* plug-in is: a pane that already had one keeps it
     * when the path will not load, and "there is a host" would read that as
     * success -- a session naming a missing plug-in came back as whichever
     * one the new tab had opened on. */
    return pv->host != NULL && !strcmp(pv->loaded_path, path);
}

const char *plugview_loaded_name(plugview *pv)
{ return pv->host ? pehost_name(pv->host) : ""; }

const char *plugview_loaded_path(plugview *pv) { return pv->loaded_path; }

int plugview_dead(plugview *pv) { return pv->dead_reported; }

char *plugview_capture_patch(plugview *pv)
{
    char err[128];
    if (!pv->host) return NULL;
    return patch_capture(pv->host, pv->loaded_path[0] ? pv->loaded_path : NULL,
                         err, sizeof err);
}

int plugview_apply_patch(plugview *pv, const char *text)
{
    char err[256];
    if (!pv->host || !text) return -1;
    if (patch_apply_text(pv->host, text, err, sizeof err, NULL, NULL)) return -1;
    /* As the pane's own Open Patch: the values changed under the list, so it
     * is rebuilt -- otherwise the sound is audible and invisible. */
    fill_params(pv);
    return 0;
}

double plugview_peak(plugview *pv)
{ return atomic_load_explicit(&pv->peak_milli, memory_order_relaxed) / 1000.0; }

void plugview_peak_reset(plugview *pv)
{ atomic_store_explicit(&pv->peak_milli, 0, memory_order_relaxed); }

void plugview_inject_midi(plugview *pv, double wall, int status, int d1, int d2)
{
    unsigned h = atomic_load_explicit(&pv->inj_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&pv->inj_tail, memory_order_acquire);
    if (h - t >= PV_INJQ) return;   /* full: dropped -- the caller counts its own */
    pv->inj[h % PV_INJQ].wall = wall;
    pv->inj[h % PV_INJQ].st = (unsigned char)status;
    pv->inj[h % PV_INJQ].d1 = (unsigned char)d1;
    pv->inj[h % PV_INJQ].d2 = (unsigned char)d2;
    atomic_store_explicit(&pv->inj_head, h + 1, memory_order_release);
    atomic_fetch_add_explicit(&pv->inj_in, 1, memory_order_relaxed);
}

void plugview_inject_stats(plugview *pv, unsigned long *injected, unsigned long *placed)
{
    if (injected) *injected = atomic_load_explicit(&pv->inj_in, memory_order_relaxed);
    if (placed)   *placed   = atomic_load_explicit(&pv->inj_placed, memory_order_relaxed);
}

/* Which input channels the fed signal reaches. A vocoder has a modulator and a
 * carrier bus and wants the microphone on one of them; sending it to both puts
 * the raw voice in the output beside the analysis. 0 means every channel. */
void plugview_set_input_mask(plugview *pv, unsigned mask)
{ if (pv->host) pehost_set_input_mask(pv->host, mask); }

int plugview_num_inputs(plugview *pv)
{ return pv->host ? pehost_num_inputs(pv->host) : 0; }

/* Raw MIDI, straight through. pv->in_plugin is not raised around it the way
 * plugview_bend does: pehost_midi is a queue write the audio thread drains, so
 * there is no call into the plug-in here to be re-entered. */
void plugview_midi(plugview *pv, int status, int d1, int d2)
{ if (pv->host) pehost_midi(pv->host, status, d1, d2); }

/* 4/4 because nothing upstream of this knows any better: a time signature
 * arrives with a sequencer's song position, not with its clock, and a plug-in
 * that cares reads the tempo. */
void   plugview_set_tempo(plugview *pv, double bpm) { if (pv->host) pehost_set_tempo(pv->host, bpm, 4, 4); }
double plugview_tempo(plugview *pv)   { return pv->host ? pehost_tempo(pv->host) : 0.0; }
int    plugview_playing(plugview *pv) { return pv->host ? pehost_playing(pv->host) : 0; }

void plugview_note_on(plugview *pv, int note, int vel)  { if (pv->host) pehost_note_on(pv->host, note, vel); }
void plugview_note_off(plugview *pv, int note)          { if (pv->host) pehost_note_off(pv->host, note); }
void plugview_all_notes_off(plugview *pv)         { if (pv->host) pehost_all_notes_off(pv->host); }
void plugview_release_all(plugview *pv)           { if (pv->host) pehost_release_all(pv->host); }

void plugview_bend(plugview *pv, int value14)
{
    if (!pv->host) return;
    if (value14 < 0) value14 = 0;
    if (value14 > 16383) value14 = 16383;
    pv->in_plugin++;
    pehost_midi(pv->host, 0xE0, value14 & 0x7F, (value14 >> 7) & 0x7F);
    pv->in_plugin--;
}

void plugview_program(plugview *pv, int idx)
{
    if (!pv->host) return;
    gtk_list_box_select_row(GTK_LIST_BOX(pv->proglist),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv->proglist), idx));
}

/* ------------------------------------------------------------------ --cycle */

/* One line per plug-in, in the same shape pestudio prints. */
static void cycle_report(plugview *pv, int i)
{
    fprintf(stderr, "plugview: cycle %d/%d %s -- %s\n", i + 1, pv->nplug,
            pv->plug[i].name,
            !pv->plug[i].loadable ? "not loadable"
            : !pv->host           ? "load failed"
            : pv->ed_attached     ? "editor attached"
            : pv->ed_native       ? "editor did not attach"
            : pv->ed_open         ? "editor (pixels)"
                                : "no editor");
}

static gboolean cycle_step(gpointer u)
{
    plugview *pv = u;
    /* Report the previous plug-in first, not in the step that loaded it. A
     * native editor attaches when the Editor page gets its first layout, which
     * is after the page switch returns -- printing in the same step called
     * every first plug-in "did not attach" while its editor came up a moment
     * later. One interval later the attach has either happened or it has not,
     * and the line also confirms the last plug-in survived a full interval
     * alongside the new one. */
    if (pv->cycle_report >= 0) {
        cycle_report(pv, pv->cycle_report);
        pv->cycle_report = -1;
    }
    if (pv->cycle_at >= pv->nplug) {
        fprintf(stderr, "plugview: cycle finished (%d plug-in(s))\n", pv->nplug);
        pv->cycle_src = 0;
        return G_SOURCE_REMOVE;
    }
    {
        int i = pv->cycle_at++;
        gtk_list_box_select_row(GTK_LIST_BOX(pv->list),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv->list), i));
        gtk_stack_set_visible_child_name(GTK_STACK(pv->stack), "editor");
        pv->cycle_report = i;
    }
    return G_SOURCE_CONTINUE;
}

void plugview_start_cycle(plugview *pv, int ms)
{
    pv->cycle_ms = ms > 0 ? ms : 1500;
    pv->cycle_at = 0;
    pv->cycle_report = -1;
    if (pv->cycle_src) g_source_remove(pv->cycle_src);
    pv->cycle_src = g_timeout_add(pv->cycle_ms, cycle_step, pv);
}

void plugview_shutdown(plugview *pv)
{
    /* The timeouts go first, before anything they read is torn down.
     *
     * Both hold widget pointers and both keep firing while the toplevel
     * finalises its children, so the meter's gtk_label_set_text ran against a
     * freed label -- ten times a second, for as long as the teardown took.
     * That is the `GTK_IS_LABEL (self)' assertion printed on the way out of
     * every session. Clearing the pointers as well means a source that somehow
     * outlives this call is inert rather than merely unlikely to run. */
    if (pv->tick)  { g_source_remove(pv->tick);  pv->tick  = 0; }
    if (pv->meter) { g_source_remove(pv->meter); pv->meter = 0; }
    /* --cycle's step selects rows and switches the stack, both of which are
     * pointers cleared below. */
    if (pv->cycle_src) { g_source_remove(pv->cycle_src); pv->cycle_src = 0; }
    /* And the editor fit a load queued for the next turn of the loop: a pane
     * closed straight after a load -- a session's tab whose plug-in did not
     * come back -- was freed before it ran, and it ran anyway. */
    if (pv->ed_fit_idle) { g_source_remove(pv->ed_fit_idle); pv->ed_fit_idle = 0; }
    /* Every widget pointer, and before the unload rather than after it.
     *
     * Clearing `status` and `editor` fixed the meter and left the zoom bar,
     * which is its own six pointers and its own updater. Clearing all of them
     * afterwards still left the same seven assertions, because the order is
     * the whole of it: GTK emits `destroy` on the toplevel from its dispose,
     * by which point the children are already gone, so this function runs with
     * dead widgets and live pointers -- and unload_locked calls
     * zoom_update_ui, which writes to six of them and is the backtrace under
     * every one of those `GTK_IS_LABEL (self)' and `GTK_IS_WIDGET (widget)'
     * lines. Nothing below this point may touch a widget, so nothing below it
     * has a pointer to touch. A pointer that is not cleared here is one that
     * outlives what it points at. */
    pv->dirlabel = pv->list = pv->proglist = pv->paramlist = NULL;
    pv->paramsw = pv->editorsw = pv->editorpage = pv->stack = pv->header = NULL;
    pv->status = NULL;
    pv->editor = NULL;
    pv->zoom_out = pv->zoom_in = pv->zoom_fit = NULL;
    pv->zoom_one = pv->zoom_lbl = pv->zoom_note = NULL;
    unload_locked(pv);
    native_editor_destroy(pv);
}

/* ------------------------------------------------------------------ the pane */

plugview *plugview_new(void (*park)(void), void (*unpark)(void),
                       double samplerate, int blocksize)
{
    plugview *pv = g_new0(plugview, 1);

    pv->park   = park;
    pv->unpark = unpark;
    pv->ed_zoom = 1.0;          /* a fresh instance starts at 0, which is no picture */
    pv->rate   = samplerate > 0 ? samplerate : 48000.0;
    pv->block  = blocksize > 0 ? blocksize : 512;
    return pv;
}

void plugview_free(plugview *pv)
{
    if (!pv) return;            /* plugview_shutdown has already run */
    g_free(pv);
}

GtkWidget *plugview_pane(plugview *pv)
{
    GtkWidget *left, *right, *paned, *top, *sw, *progsw, *sws;
    GtkWidget *sidebar;
    GtkEventController *ctl;
    GtkGesture *click;

    /* ---- left: what to browse, plug-ins, programs ---- */
    /* Every folder is walked, once, and the two selectors below sift the
     * result. The dropdown here used to name folders -- one per corpus, plus
     * every one the user had added -- which made the tree layout the user's
     * problem and offered the same format once per folder holding it. */
    roots_discover(pv);
    /* The system's own VST directories, and the folders the user set in
     * Settings. Without the first an installed copy has nothing to scan, the
     * walk-up above having found no checkout to walk. */
    roots_add_standard(pv);
    roots_add_user(pv);
    /* roots_discover(pv) above cleared the list, so the fallback has to be
     * re-applied here or nothing is scanned at all on a machine that has
     * neither a checkout nor a system VST directory. */
    roots_add_fallback(pv);
    /* Whatever plugview_scan was told to look at before the pane existed --
     * dwstudio's --dir, or its default -- survives that reset here. */
    if (pv->session_dir[0]) roots_add(pv, pv->session_dir, "given folder");

    /* Format, then platform: the two terms a plug-in actually has, and the
     * same pair in the same order as pestudio's. Filled by rebuild_filters(pv)
     * from what the scan found, so neither offers a choice with nothing
     * behind it. */
    pv->typemodel = gtk_string_list_new(NULL);
    gtk_string_list_append(pv->typemodel, "All types");
    pv->typedd = gtk_drop_down_new(G_LIST_MODEL(pv->typemodel), NULL);
    gtk_widget_set_hexpand(pv->typedd, TRUE);
    pv->osmodel = gtk_string_list_new(NULL);
    gtk_string_list_append(pv->osmodel, "All platforms");
    pv->osdd = gtk_drop_down_new(G_LIST_MODEL(pv->osmodel), NULL);
    gtk_widget_set_hexpand(pv->osdd, TRUE);
    {
        GtkWidget *tr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *orow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(tr), gtk_label_new("Type:"));
        gtk_box_append(GTK_BOX(tr), pv->typedd);
        gtk_box_append(GTK_BOX(orow), gtk_label_new("OS:"));
        gtk_box_append(GTK_BOX(orow), pv->osdd);
        pv->filterrows[0] = tr;
        pv->filterrows[1] = orow;
    }

    /* Where the plug-in under the cursor came from. A display now, not the
     * thing being browsed: with every folder scanned at once there is no one
     * directory to point at, and the useful one is the selection's. */
    top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    pv->dirlabel = gtk_label_new(pv->dir[0] ? pv->dir : "(no folder)");
    gtk_label_set_ellipsize(GTK_LABEL(pv->dirlabel), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(pv->dirlabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(pv->dirlabel), 0.0f);
    gtk_box_append(GTK_BOX(top), pv->dirlabel);

    pv->list = gtk_list_box_new();
    g_signal_connect(pv->list, "row-selected", G_CALLBACK(on_plug_selected), pv);
    sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), pv->list);
    gtk_widget_set_vexpand(sw, TRUE);

    pv->proglist = gtk_list_box_new();
    g_signal_connect(pv->proglist, "row-selected", G_CALLBACK(on_prog_selected), pv);
    progsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(progsw), pv->proglist);
    gtk_widget_set_size_request(progsw, -1, 260);

    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(left), pv->filterrows[0]);
    gtk_box_append(GTK_BOX(left), pv->filterrows[1]);
    gtk_box_append(GTK_BOX(left), top);
    gtk_box_append(GTK_BOX(left), gtk_label_new("Plug-ins"));
    pv->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(pv->search), "Search plug-ins");
    g_signal_connect(pv->search, "search-changed", G_CALLBACK(on_search_changed), pv);
    gtk_box_append(GTK_BOX(left), pv->search);
    gtk_box_append(GTK_BOX(left), sw);
    gtk_box_append(GTK_BOX(left), gtk_label_new("Programs"));
    gtk_box_append(GTK_BOX(left), progsw);
    gtk_widget_set_size_request(left, 420, -1);

    /* ---- right: header, then Parameters | Editor ---- */
    pv->header = gtk_label_new("no plug-in loaded");
    gtk_label_set_xalign(GTK_LABEL(pv->header), 0.0f);

    pv->paramlist = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(pv->paramlist), GTK_SELECTION_NONE);
    pv->paramsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pv->paramsw), pv->paramlist);
    gtk_widget_set_vexpand(pv->paramsw, TRUE);

    pv->editor = gtk_drawing_area_new();
    gtk_widget_set_focusable(pv->editor, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pv->editor), editor_draw, pv, NULL);
    /* A foreign X window does not move with GTK's layout, so it is put back
     * over the pane whenever the pane changes shape. */
    g_signal_connect(pv->editor, "resize", G_CALLBACK(on_editor_resize), pv);

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);   /* any button */
    g_signal_connect(click, "pressed",  G_CALLBACK(on_ed_pressed),  pv);
    g_signal_connect(click, "released", G_CALLBACK(on_ed_released), pv);
    gtk_widget_add_controller(pv->editor, GTK_EVENT_CONTROLLER(click));

    ctl = gtk_event_controller_motion_new();
    g_signal_connect(ctl, "motion", G_CALLBACK(on_ed_motion), pv);
    gtk_widget_add_controller(pv->editor, ctl);

    ctl = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(ctl, "scroll", G_CALLBACK(on_ed_scroll), pv);
    gtk_widget_add_controller(pv->editor, ctl);

    ctl = gtk_event_controller_key_new();
    g_signal_connect(ctl, "key-pressed",  G_CALLBACK(on_ed_key_down), pv);
    g_signal_connect(ctl, "key-released", G_CALLBACK(on_ed_key_up),   pv);
    gtk_widget_add_controller(pv->editor, ctl);

    pv->editorsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pv->editorsw), pv->editor);
    gtk_widget_set_vexpand(pv->editorsw, TRUE);
    /* Scrollbars that take their own strip, not GTK's overlay ones.
     *
     * An overlay scrollbar is drawn on top of the viewport, and the viewport is
     * exactly what the plug-in's X window covers -- a foreign child sits above
     * everything GTK paints, so the scrollbars would be invisible under it and
     * unclickable through it. Given their own space they sit outside the clip
     * window, stay visible, and can be dragged, which is the only way to pan a
     * large editor: the wheel over the editor itself belongs to the plug-in,
     * which uses it for its own controls. */
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(pv->editorsw), FALSE);

    /* The zoom bar, above the viewport rather than floating over it: a foreign
     * X11 child sits on top of everything GTK paints, so anything overlaid on
     * the editor would be invisible and unclickable exactly when it is a native
     * editor that needs it -- the same reason the scrollbars were given their
     * own strip. */
    {
        GtkWidget *zb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        pv->zoom_out = gtk_button_new_with_label("\xe2\x88\x92");
        pv->zoom_in  = gtk_button_new_with_label("+");
        pv->zoom_fit = gtk_button_new_with_label("Fit");
        pv->zoom_one = gtk_button_new_with_label("1:1");
        pv->zoom_lbl = gtk_label_new("100%");
        pv->zoom_note = gtk_label_new("");
        gtk_widget_set_size_request(pv->zoom_lbl, 48, -1);
        gtk_label_set_xalign(GTK_LABEL(pv->zoom_note), 0.0f);
        gtk_widget_set_hexpand(pv->zoom_note, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(pv->zoom_note), PANGO_ELLIPSIZE_END);
        gtk_widget_set_tooltip_text(pv->zoom_out, "Zoom out  (Ctrl+wheel over the editor)");
        gtk_widget_set_tooltip_text(pv->zoom_in,  "Zoom in  (Ctrl+wheel over the editor)");
        gtk_widget_set_tooltip_text(pv->zoom_fit, "Scale the editor to fit the space there is");
        gtk_widget_set_tooltip_text(pv->zoom_one, "Back to the size the plug-in drew");
        /* Not focus stops. Tab is how you get out of the plug-in list, and four
         * more places for the keyboard to end up is four more ways to be typing
         * at something that is not the synth. */
        gtk_widget_set_focus_on_click(pv->zoom_out, FALSE);
        gtk_widget_set_focus_on_click(pv->zoom_in,  FALSE);
        gtk_widget_set_focus_on_click(pv->zoom_fit, FALSE);
        gtk_widget_set_focus_on_click(pv->zoom_one, FALSE);
        g_signal_connect(pv->zoom_out, "clicked", G_CALLBACK(on_zoom_out), pv);
        g_signal_connect(pv->zoom_in,  "clicked", G_CALLBACK(on_zoom_in),  pv);
        g_signal_connect(pv->zoom_fit, "clicked", G_CALLBACK(on_zoom_fit), pv);
        g_signal_connect(pv->zoom_one, "clicked", G_CALLBACK(on_zoom_one), pv);

        gtk_box_append(GTK_BOX(zb), gtk_label_new("Zoom"));
        gtk_box_append(GTK_BOX(zb), pv->zoom_out);
        gtk_box_append(GTK_BOX(zb), pv->zoom_lbl);
        gtk_box_append(GTK_BOX(zb), pv->zoom_in);
        gtk_box_append(GTK_BOX(zb), pv->zoom_fit);
        gtk_box_append(GTK_BOX(zb), pv->zoom_one);
        gtk_box_append(GTK_BOX(zb), pv->zoom_note);
        gtk_box_append(GTK_BOX(page), zb);
        gtk_box_append(GTK_BOX(page), pv->editorsw);
        pv->editorpage = page;
    }

    pv->stack = gtk_stack_new();
    gtk_stack_add_titled(GTK_STACK(pv->stack), pv->paramsw,  "params", "Parameters");
    gtk_stack_add_titled(GTK_STACK(pv->stack), pv->editorpage, "editor", "Editor");
    g_signal_connect(pv->stack, "notify::visible-child", G_CALLBACK(on_page_changed), pv);
    sws = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(sws), GTK_STACK(pv->stack));
    gtk_widget_set_halign(sws, GTK_ALIGN_START);

    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(right), pv->header);
    gtk_box_append(GTK_BOX(right), sws);
    gtk_box_append(GTK_BOX(right), pv->stack);

    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_position(GTK_PANED(paned), 520);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);

    pv->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(pv->status), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(pv->status), PANGO_ELLIPSIZE_MIDDLE);

    sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(sidebar), paned);
    gtk_box_append(GTK_BOX(sidebar), pv->status);

    pv->root = sidebar;
    /* The roots were rebuilt above, so what was scanned before the pane
     * existed is out of date -- scan again over the full set, then show it. */
    plugview_scan(pv, NULL);
    g_signal_connect(pv->typedd, "notify::selected", G_CALLBACK(on_filter_changed), pv);
    g_signal_connect(pv->osdd,   "notify::selected", G_CALLBACK(on_filter_changed), pv);
    fill_browser(pv);
    {   /* Where a native editor's descriptors and timers end up. The table
         * itself is pehost's and process-global, so with more than one pane
         * the last one built is the one editors register with -- see
         * plugview.h. */
        v3_runloop_hooks hooks = {
            pv, hook_add_fd, hook_del_fd, hook_add_timer, hook_del_timer, hook_resize
        };
        v3_set_runloop_hooks(&hooks);
    }
    zoom_update_ui(pv);
    pv->tick  = g_timeout_add(30, editor_tick, pv);
    pv->meter = g_timeout_add(100, meter_tick, pv);
    pv->ready = 1;
    return pv->root;
}
