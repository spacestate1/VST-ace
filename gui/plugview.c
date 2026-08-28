/* See plugview.h. */

/* realpath, readlink: _GNU_SOURCE rather than _POSIX_C_SOURCE, which is
 * what the rest of the tree uses and what dwstudio.c relies on implicitly. */
#define _GNU_SOURCE

#include "plugview.h"

#include "pehost.h"
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

/* macOS hosting is switched off in the UI, exactly as it is in pestudio -- see
 * the note at the top of qtgui/main.cpp. Nothing is removed: pehost still
 * loads a .vst, .component or Classic plug-in, and `dw peload` still opens one
 * from the command line. This only stops the window offering them, so the two
 * windows browse the same set. Build with -DPLUGVIEW_MAC=1 to put it back. */
#ifndef PLUGVIEW_MAC
#define PLUGVIEW_MAC 0
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
} entry;

/* One entry in the platform dropdown: a corpus directory and what to call it. */
typedef struct { char label[64]; char path[1024]; } proot;

static struct {
    GtkWidget *root;
    GtkWidget *rootdd;        /* which platform's plug-ins to browse */
    GtkStringList *rootmodel;
    proot  roots[MAX_ROOTS];
    int    nroot;
    GtkWidget *dirlabel;
    GtkWidget *list;          /* plug-ins */
    GtkWidget *proglist;      /* the loaded plug-in's programs */
    GtkWidget *paramlist;
    GtkWidget *paramsw;
    GtkWidget *editor;        /* GtkDrawingArea fed by pehost_editor_pixels */
    GtkWidget *editorsw;
    GtkWidget *editorpage;    /* the zoom bar and editorsw together */
    GtkWidget *stack;         /* Parameters | Editor */
    GtkWidget *header;
    GtkWidget *status;

    entry  plug[MAX_PLUGINS];
    int    nplug;
    char   dir[1024];

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
     * P.editor. GTK4 hands out an X11 id only for a toplevel, so the child has
     * to be made with Xlib and placed by hand -- but it is placed inside our
     * window, which is where a plug-in editor belongs and where pestudio puts
     * its own. */
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
    GtkWidget *zoom_out, *zoom_in, *zoom_fit, *zoom_one, *zoom_lbl, *zoom_note;

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
} P;

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
static void roots_discover(void)
{
    static const struct { const char *label, *rel; } cand[] = {
        { "Windows VST2 64-bit", "windows/VST2-64" },
        { "Windows VST3",        "windows/VST3"    },
        { "Windows VST2 32-bit", "windows/VST2-32" },
        { "Linux native",        "linux/extracted" },
#if PLUGVIEW_MAC
        { "macOS VST2",          "macos/VST2"      },
        { "macOS Audio Units",   "macos/AU"        },
        { "Mac OS 9 (Classic)",  "macos/classic"   },
#endif
    };
    char    exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char   *slash;
    int     up, i, j;

    P.nroot = 0;
    if (n <= 0) return;
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
            for (j = 0; j < P.nroot; j++) if (!strcmp(P.roots[j].path, path)) dup = 1;
            if (dup || P.nroot >= MAX_ROOTS) continue;
            snprintf(P.roots[P.nroot].label, sizeof P.roots[0].label, "%s", cand[i].label);
            snprintf(P.roots[P.nroot].path,  sizeof P.roots[0].path,  "%s", path);
            P.nroot++;
        }
    }
}

/* Add a directory that is not one of the built-in corpora -- a Downloads
 * folder, a system VST path -- and say where it came from. Returns its index,
 * or the existing one if it is already listed. */
static int roots_add(const char *path, const char *label)
{
    char lbl[64], real[1024];
    int  i;

    if (realpath(path, real)) path = real;
    for (i = 0; i < P.nroot; i++) if (!strcmp(P.roots[i].path, path)) return i;
    if (P.nroot >= MAX_ROOTS) return -1;
    snprintf(lbl, sizeof lbl, "%s", label);
    snprintf(P.roots[P.nroot].label, sizeof P.roots[0].label, "%s", lbl);
    snprintf(P.roots[P.nroot].path,  sizeof P.roots[0].path,  "%s", path);
    if (P.rootmodel) gtk_string_list_append(P.rootmodel, lbl);
    return P.nroot++;
}

/* ------------------------------------------------------------- the browser */

/* Everything that has something to say says it here.
 *
 * The level meter rewrites this label ten times a second, so a message written
 * straight to the widget survived for one tick and vanished -- which is how a
 * refused plug-in looked like nothing happening at all. The text is kept, and
 * the meter appends to it. */
static void plug_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(P.loaded_msg, sizeof P.loaded_msg, fmt, ap);
    va_end(ap);
    if (P.status) gtk_label_set_text(GTK_LABEL(P.status), P.loaded_msg);
}

static int entry_cmp(const void *a, const void *b)
{ return g_ascii_strcasecmp(((const entry *)a)->name, ((const entry *)b)->name); }

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
        if (l > 10 && !strcasecmp(name + l - 10, ".component")) return 1;
#endif
        return 0;
    }
    if (l > 4 && !strcasecmp(name + l - 4, ".dll")) return 1;
    if (l > 3 && !strcasecmp(name + l - 3, ".so"))
        return !strstr(path, ".lv2/") && pehost_is_native_vst2(path);
#if PLUGVIEW_MAC
    /* Skipped otherwise, which also spares every unrecognised file in the tree
     * from being opened and sniffed. */
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
void plugview_scan(const char *dir)
{
    char   queue[512][1024];
    int    head = 0, tail = 0, visited = 0;
    char   real[1024];

    if (!dir || !*dir) return;

    /* Resolved, not as given. dwstudio builds its default from the executable
     * path and hands over something like ".../gui/build/../../../windows/
     * VST2-64" -- the same directory the dropdown discovered, but not the same
     * string, so it was listed twice and neither entry matched the other. */
    if (realpath(dir, real)) snprintf(P.dir, sizeof P.dir, "%s", real);
    else                     snprintf(P.dir, sizeof P.dir, "%s", dir);
    P.nplug = 0;

    snprintf(queue[tail++], sizeof queue[0], "%s", P.dir);

    while (head < tail && P.nplug < MAX_PLUGINS && visited < 512) {
        char        base[1024];
        GDir       *d;
        const char *nm;

        snprintf(base, sizeof base, "%s", queue[head++]);
        visited++;
        if (!(d = g_dir_open(base, 0, NULL))) {
            if (head == 1) fprintf(stderr, "plugview: no such directory: %s\n", base);
            continue;
        }
        while ((nm = g_dir_read_name(d)) && P.nplug < MAX_PLUGINS) {
            char  path[1024];
            int   isdir;
            entry *e;
            pehost_info info;

            if (nm[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s", base, nm);
            isdir = is_dir(path);

            if (!is_candidate(path, nm, isdir)) {
                if (isdir && tail < (int)(sizeof queue / sizeof queue[0]))
                    snprintf(queue[tail++], sizeof queue[0], "%s", path);
                continue;
            }
            /* A candidate by shape still has to be one this host can run --
             * a 32-bit build without the helper, a PowerPC Mach-O. Listed
             * either way, with the reason when it cannot: a plug-in that is
             * simply absent from the list looks like one the scan failed to
             * find, and "why is it not there" is a worse question than "why
             * will it not load". pestudio does the same. */
            e = &P.plug[P.nplug++];
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
        g_dir_close(d);
    }
    qsort(P.plug, (size_t)P.nplug, sizeof P.plug[0], entry_cmp);
    /* Said out loud, the way pestudio says it, so the two windows can be
     * compared on the same folder without reading either one's status bar. */
    fprintf(stderr, "plugview: scanned %s -> %d plug-in(s)\n", P.dir, P.nplug);
}

static void append_row(const entry *e);

static void clear_list(GtkWidget *lb)
{
    GtkWidget *row;
    while ((row = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), 0))))
        gtk_list_box_remove(GTK_LIST_BOX(lb), row);
}

static void fill_browser(void)
{
    int i;

    if (!P.list) return;
    clear_list(P.list);
    for (i = 0; i < P.nplug; i++) append_row(&P.plug[i]);
    plug_status("%d plug-in(s) under %s", P.nplug, P.dir);

    /* Load the first one, the way pestudio does. Not for the convenience: a
     * GtkListBox picks a row for itself when it first takes focus, so without
     * saying which, the window came up having loaded whichever row that
     * happened to be -- different one each run. */
    if (P.nplug > 0)
        gtk_list_box_select_row(GTK_LIST_BOX(P.list),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(P.list), 0));
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
#define MAX_WATCH 128

typedef struct {
    void    *handler;
    int      fd;              /* -1 for a timer */
    GSource *src;
} watch;

static watch g_watch[MAX_WATCH];

static void watch_retire(watch *w)
{
    if (!w->src) return;
    g_source_destroy(w->src);
    g_source_unref(w->src);
    w->src = NULL;
    w->handler = NULL;
}

static watch *watch_slot(void)
{
    int i;
    for (i = 0; i < MAX_WATCH; i++) if (!g_watch[i].src) return &g_watch[i];
    return NULL;
}

static void watches_clear(void)
{
    int i;
    for (i = 0; i < MAX_WATCH; i++) watch_retire(&g_watch[i]);
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
    watch *w;
    int    i;

    (void)ud;
    for (i = 0; i < MAX_WATCH; i++)          /* same fd, same handler: replace */
        if (g_watch[i].src && g_watch[i].handler == handler && g_watch[i].fd == fd)
            watch_retire(&g_watch[i]);
    if (!(w = watch_slot())) {
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
    int i;
    (void)ud;
    /* unregisterEventHandler names only the handler, so every descriptor
     * registered under it goes. */
    for (i = 0; i < MAX_WATCH; i++)
        if (g_watch[i].src && g_watch[i].handler == handler && g_watch[i].fd >= 0)
            watch_retire(&g_watch[i]);
}

static void hook_add_timer(void *ud, void *handler, unsigned long long ms)
{
    watch *w;
    (void)ud;
    if (!(w = watch_slot())) return;
    w->handler = handler;
    w->fd      = -1;
    w->src     = g_timeout_source_new((guint)(ms ? ms : 16));
    g_source_set_callback(w->src, on_watch_timer, handler, NULL);
    g_source_attach(w->src, NULL);
}

static void hook_del_timer(void *ud, void *handler)
{
    int i;
    (void)ud;
    for (i = 0; i < MAX_WATCH; i++)
        if (g_watch[i].src && g_watch[i].handler == handler && g_watch[i].fd < 0)
            watch_retire(&g_watch[i]);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static Display *ed_display(void)
{
    GdkDisplay *d = gdk_display_get_default();
    return GDK_IS_X11_DISPLAY(d) ? gdk_x11_display_get_xdisplay(d) : NULL;
}

/* The zoom, which is defined with the rest of the editor input further down but
 * is reached from the run-loop resize hook and from the attach path above it. */
static void     zoom_apply(void);
static void     zoom_update_ui(void);
static gboolean zoom_fit_idle(gpointer u);

static void hook_resize(void *ud, int w, int h)
{
    Display *dpy = ed_display();
    (void)ud;
    (void)dpy;
    if (w > 0 && h > 0) {
        /* The plug-in's own idea of its natural size, which replaces whatever
         * it opened at -- otherwise a later zoom would scale from a size the
         * plug-in has moved on from. The zoom is then re-applied on top. */
        P.ed_base_w = w; P.ed_base_h = h;
        P.ed_w = w; P.ed_h = h;
        P.ed_nat_w = w; P.ed_nat_h = h;
        gtk_widget_set_size_request(P.editor, w, h);   /* the pane follows */
        zoom_apply();
    }
}

static int native_editor_open(pehost *h, const char *title);


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
static int editor_bounds(GdkRectangle *clip, GdkRectangle *plug)
{
    GtkWidget       *root = GTK_WIDGET(gtk_widget_get_root(P.editor));
    /* The viewport GtkScrolledWindow wrapped the drawing area in. Its
     * allocation is the visible rectangle; the drawing area's is the whole
     * editor, which is larger as soon as the size request below exceeds it. */
    GtkWidget       *port = gtk_widget_get_parent(P.editor);
    graphene_rect_t  full, vis;
    int              x0, y0, x1, y1;

    if (!root || !port) return 0;
    if (!gtk_widget_compute_bounds(P.editor, root, &full)) return 0;
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
    if ((int)full.size.width  < P.ed_nat_w) full.size.width  = (float)P.ed_nat_w;
    if ((int)full.size.height < P.ed_nat_h) full.size.height = (float)P.ed_nat_h;

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
static void native_editor_place(void)
{
    Display     *dpy = ed_display();
    GdkRectangle clip, plug;
    int          show;

    if (!dpy || !P.ed_clip || !P.ed_xwin) return;

    show = GTK_IS_WIDGET(P.editor) && gtk_widget_get_mapped(P.editor) &&
           editor_bounds(&clip, &plug);
    if (!show) {
        if (P.ed_shown) { XUnmapWindow(dpy, P.ed_clip); P.ed_shown = 0; XFlush(dpy); }
        return;
    }
    if (P.ed_shown &&
        !memcmp(&clip, &P.ed_clip_at, sizeof clip) &&
        !memcmp(&plug, &P.ed_plug_at, sizeof plug))
        return;

    XMoveResizeWindow(dpy, P.ed_clip, clip.x, clip.y,
                      (unsigned)clip.width, (unsigned)clip.height);
    XMoveResizeWindow(dpy, P.ed_xwin, plug.x, plug.y,
                      (unsigned)plug.width, (unsigned)plug.height);
    if (!P.ed_shown) { XMapWindow(dpy, P.ed_clip); P.ed_shown = 1; }
    P.ed_clip_at = clip;
    P.ed_plug_at = plug;
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
    (void)a; (void)w; (void)h; (void)u;
    if (P.ed_attached) { native_editor_place(); return; }
    if (P.ready && P.host && P.ed_native && P.stack) {
        const char *page = gtk_stack_get_visible_child_name(GTK_STACK(P.stack));
        if (page && !strcmp(page, "editor"))
            native_editor_open(P.host, pehost_name(P.host));
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
static void native_editor_close(void)
{
    if (P.ed_attached) { pehost_editor_detach(P.ed_attached); P.ed_attached = NULL; }
    /* Forget the last placement with it: the next plug-in's editor is a
     * different size, and a cached rectangle that still matches would let
     * native_editor_place decide there was nothing to do. */
    memset(&P.ed_clip_at, 0, sizeof P.ed_clip_at);
    memset(&P.ed_plug_at, 0, sizeof P.ed_plug_at);
    watches_clear();
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

static void native_editor_destroy(void)
{
    Display *dpy = ed_display();

    native_editor_close();
    if (dpy && (P.ed_xwin || P.ed_clip)) {
        int (*prev)(Display *, XErrorEvent *);
        XSync(dpy, False);                  /* let earlier errors land first */
        prev = XSetErrorHandler(ed_swallow_x_error);
        /* The child first, then its parent. Destroying the clip window would
         * take the other with it, but naming both keeps this readable and
         * costs one request. */
        if (P.ed_xwin) XDestroyWindow(dpy, P.ed_xwin);
        if (P.ed_clip) XDestroyWindow(dpy, P.ed_clip);
        XSync(dpy, False);                  /* and ours inside the handler */
        XSetErrorHandler(prev);
    }
    P.ed_xwin  = 0;
    P.ed_clip  = 0;
    P.ed_xid   = 0;
    P.ed_shown = 0;
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
static int native_editor_open(pehost *h, const char *title)
{
    Display     *dpy = ed_display();
    GdkSurface  *surf;
    Window       parent;
    GdkRectangle clip, plug;

    native_editor_close();
    if (!dpy) {
        plug_status("%s: a native editor needs the X11 backend -- "
                    "run with GDK_BACKEND=x11", title);
        return -1;
    }
    if (!editor_bounds(&clip, &plug)) {
        /* The pane has not been laid out yet; the page change that brings it
         * into view will come back through here. */
        return -1;
    }
    surf = gtk_native_get_surface(gtk_widget_get_native(P.editor));
    if (!surf || !GDK_IS_X11_SURFACE(surf)) {
        plug_status("%s: no X11 window to embed into", title);
        return -1;
    }
    parent = gdk_x11_surface_get_xid(GDK_X11_SURFACE(surf));

    /* Both windows live for the life of the pane and are reused by every
     * plug-in in turn, which is what pestudio does. Destroying them per plug-in
     * tore the parent out from under an editor that was still shutting down:
     * JE8086 crashed inside juce::OpenGLContext::CachedImage::stop(), down in
     * the NVIDIA driver, on the way out. They cost nothing to keep and the
     * plug-in that owns one has let go by the time the next attaches. */
    if (!P.ed_clip && !(P.ed_clip = ed_make_window(dpy, parent, &clip))) {
        plug_status("%s: could not create an editor window", title);
        return -1;
    }
    if (!P.ed_xwin && !(P.ed_xwin = ed_make_window(dpy, P.ed_clip, &plug))) {
        plug_status("%s: could not create an editor window", title);
        return -1;
    }
    XMoveResizeWindow(dpy, P.ed_clip, clip.x, clip.y,
                      (unsigned)clip.width, (unsigned)clip.height);
    XMoveResizeWindow(dpy, P.ed_xwin, plug.x, plug.y,
                      (unsigned)plug.width, (unsigned)plug.height);
    XMapWindow(dpy, P.ed_xwin);
    XMapWindow(dpy, P.ed_clip);
    P.ed_shown   = 1;
    P.ed_clip_at = clip;
    P.ed_plug_at = plug;

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
            if (XGetWindowAttributes(dpy, P.ed_xwin, &a) && a.map_state == IsViewable)
                break;
            { struct timespec ts = { 0, 5000000 }; nanosleep(&ts, NULL); }
        }
    }
    P.ed_xid = (unsigned long)P.ed_xwin;

    if (pehost_editor_attach(h, P.ed_xid) != 0) {
        plug_status("%s: the plug-in refused to embed into window 0x%lx",
                    title, P.ed_xid);
        return -1;
    }
    P.ed_attached = h;
    /* Zoomable from here, not from load(): until the plug-in has a window there
     * is nothing to ask to resize. Fit on the next turn of the main loop, once
     * the pane has been laid out at this editor's size. */
    zoom_update_ui();
    g_idle_add(zoom_fit_idle, NULL);
    /* Said out loud, the way pestudio says it. Both rectangles, because which
     * one is bigger is the whole question when an editor does not fit: the
     * plug-in gets the first, and the second is how much of it you can see. */
    fprintf(stderr, "plugview: embedded as a child of window 0x%lx "
                    "(%dx%d editor, %dx%d visible)\n",
            P.ed_xid, plug.width, plug.height, clip.width, clip.height);
    XFlush(dpy);

    /* No pehost_editor_resized() here, deferred or otherwise: it crashes
     * Cardinal inside its own framework, which asserts "pData->view !=
     * nullptr" and then dereferences it anyway. pestudio does not send one
     * either -- its editor widget is already the right size before it
     * attaches, so Qt has no resize left to deliver. */
    P.ed_w = plug.width;
    P.ed_h = plug.height;
    return 0;
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* ------------------------------------------------------------- the editor */

/* Every entry into a plug-in from the GTK thread raises this, and anything
 * that could be re-entered checks it.
 *
 * The same guard pestudio carries, for the same reason: a plug-in's own modal
 * drag loop pumps the host's event queue, so a click on the plug-in list can
 * be delivered while that plug-in is still running -- and loading another one
 * there frees what is currently executing. */
static int in_plugin;

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

    (void)area; (void)ud;
    if (!P.host || !P.ed_open ||
        !pehost_editor_pixels(P.host, &px, &pw, &ph) ||
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
    if (P.ed_zoom != 1.0) cairo_scale(cr, P.ed_zoom, P.ed_zoom);
    cairo_set_source_surface(cr, surf, 0, 0);
    /* Smoothed on the way down, sharp on the way up. Dropping every other pixel
     * of a knob leaves it ragged; an editor enlarged is bitmaps and text at a
     * fixed size, and blurring those is worse than seeing the pixels. */
    cairo_pattern_set_filter(cairo_get_source(cr),
                             P.ed_zoom < 1.0 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
}

/* The editor is not driven by GTK: the plug-in repaints when it feels like it,
 * so the buffer has to be pumped and the widget told to redraw. 30 ms is what
 * pestudio settled on -- fast enough that a dragged knob tracks the mouse. */
static gboolean editor_tick(gpointer ud)
{
    (void)ud;
    if (in_plugin) return G_SOURCE_CONTINUE;
    /* A native editor is an X11 window of its own: nothing to pump and no
     * pixels to fetch, but it has to be kept over the visible part of the pane
     * as that scrolls, resizes, and comes and goes with the page. Scrolling in
     * particular emits no signal that reports the new geometry after layout,
     * so this is where it is noticed. */
    if (P.ed_attached) { native_editor_place(); return G_SOURCE_CONTINUE; }
    if (!P.host || !P.ed_open) return G_SOURCE_CONTINUE;
    if (!GTK_IS_WIDGET(P.editor) || !gtk_widget_get_mapped(P.editor))
        return G_SOURCE_CONTINUE;
    in_plugin++;
    pehost_editor_pump(P.host);
    in_plugin--;
    gtk_widget_queue_draw(P.editor);
    return G_SOURCE_CONTINUE;
}

/* Reads the peak the audio thread left behind and shows it, decaying so a note
 * that has stopped stops reading. Also the answer to "is this thing actually
 * making sound", which otherwise needs a recording to establish. */
static gboolean meter_tick(gpointer ud)
{
    static int shown;
    char txt[640];
    int  pk;

    (void)ud;
    if (!P.host || !P.status || !P.loaded_msg[0]) return G_SOURCE_CONTINUE;
    pk = atomic_exchange_explicit(&P.peak_milli, 0, memory_order_relaxed);
    if (pk < shown) pk = shown - 40 > 0 ? shown - 40 : 0;   /* ease down */
    shown = pk;
    snprintf(txt, sizeof txt, "%s   ·   out %.3f %s", P.loaded_msg, pk / 1000.0,
             pk > 0 ? "\xe2\x96\xa0" : "");
    gtk_label_set_text(GTK_LABEL(P.status), txt);
    return G_SOURCE_CONTINUE;
}

/* Every mouse message goes through here, which is what makes the zoom safe to
 * add: the plug-in is told where the click landed in its own picture, not where
 * it landed on screen. Getting this wrong does not look like a bug in the zoom
 * -- it looks like the plug-in's knobs have stopped working. */
static void ed_mouse(int x, int y, int msg, int wheel)
{
    if (!P.host || !P.ed_open) return;
    if (P.ed_zoom != 1.0) { x = (int)(x / P.ed_zoom); y = (int)(y / P.ed_zoom); }
    in_plugin++;
    pehost_editor_mouse(P.host, x, y, msg, P.buttons, wheel);
    in_plugin--;
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

static int zoom_can(void)
{
    if (!P.host || P.ed_base_w <= 0) return 0;
    if (P.ed_native) return P.ed_attached && pehost_editor_can_resize(P.host);
    return P.ed_open;
}

static void zoom_update_ui(void)
{
    char txt[32];
    int  on = zoom_can();

    if (!P.zoom_lbl) return;
    snprintf(txt, sizeof txt, "%d%%", (int)(P.ed_zoom * 100.0 + 0.5));
    gtk_label_set_text(GTK_LABEL(P.zoom_lbl), txt);
    gtk_widget_set_sensitive(P.zoom_lbl, on);
    gtk_widget_set_sensitive(P.zoom_out, on && P.ed_zoom > ZOOM_MIN);
    gtk_widget_set_sensitive(P.zoom_in,  on && P.ed_zoom < ZOOM_MAX);
    gtk_widget_set_sensitive(P.zoom_fit, on);
    gtk_widget_set_sensitive(P.zoom_one, on && P.ed_zoom != 1.0);
    /* Say why, when they are dead. A disabled button with no reason beside it
     * reads as something broken rather than something that cannot be done to
     * this particular plug-in. */
    gtk_label_set_text(GTK_LABEL(P.zoom_note),
        on      ? (P.ed_native ? "the plug-in redraws itself at this size" : "")
        : !P.host || P.ed_base_w <= 0 ? "no editor open"
        : P.ed_native ? "this plug-in draws its own window and will not resize it"
                      : "");
}

static void zoom_apply(void)
{
    int sw, sh;

    if (P.ed_base_w <= 0 || P.ed_base_h <= 0) { zoom_update_ui(); return; }
    if (!zoom_can()) {                  /* shown at the size the plug-in drew */
        P.ed_zoom = 1.0;
        zoom_update_ui();
        return;
    }
    sw = (int)(P.ed_base_w * P.ed_zoom + 0.5);
    sh = (int)(P.ed_base_h * P.ed_zoom + 0.5);
    P.ed_w = sw; P.ed_h = sh;
    if (P.ed_native) {
        /* The window the plug-in was given changes size, and editor_bounds
         * reads ed_nat_* to decide how big that is -- so it is the scaled size
         * that goes there, not the one the plug-in first asked for. */
        P.ed_nat_w = sw; P.ed_nat_h = sh;
        gtk_widget_set_size_request(P.editor, sw, sh);
        native_editor_place();
        in_plugin++;
        pehost_editor_resized(P.host, sw, sh);
        in_plugin--;
    } else {
        gtk_widget_set_size_request(P.editor, sw, sh);
        gtk_widget_queue_draw(P.editor);
    }
    zoom_update_ui();
}

static void zoom_set(double z)
{
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    P.ed_zoom = z;
    zoom_apply();
}

/* One notch. Geometric rather than a fixed number of percent: stepping down
 * from 100 in tenths takes ten presses to halve the picture and then crawls,
 * where a constant ratio feels the same at every size. */
static void zoom_step(int dir)
{
    if (dir) zoom_set(P.ed_zoom * (dir > 0 ? 1.25 : 1.0 / 1.25));
}

/* Scale the editor to the space there is.
 *
 * `only_shrink` is what the automatic fit on opening an editor uses: one that
 * already fits is left at the size the plug-in drew it, because enlarging is
 * not an improvement -- the artwork is bitmaps and text at a fixed size, and
 * stretching it only makes it soft. Pressing Fit is an explicit request and
 * will enlarge. */
static void zoom_fit(int only_shrink)
{
    GtkWidget *port;
    double     z, zy;
    int        vw, vh;

    if (!zoom_can() || P.ed_base_w <= 0 || P.ed_base_h <= 0) return;
    if (!P.editor || !(port = gtk_widget_get_parent(P.editor))) return;
    vw = gtk_widget_get_width(port);
    vh = gtk_widget_get_height(port);
    if (vw < 16 || vh < 16) return;        /* not laid out yet */
    z  = (double)vw / P.ed_base_w;
    zy = (double)vh / P.ed_base_h;
    if (zy < z) z = zy;
    if (only_shrink && z >= 1.0) z = 1.0;
    zoom_set(z);
}

/* The automatic fit runs one main-loop turn after the editor opens: the pane
 * has not been laid out at this editor's size yet, and measuring it now returns
 * the last plug-in's. */
static gboolean zoom_fit_idle(gpointer u)
{ (void)u; zoom_fit(1); return G_SOURCE_REMOVE; }

static void on_zoom_out(GtkButton *b, gpointer u) { (void)b; (void)u; zoom_step(-1); }
static void on_zoom_in (GtkButton *b, gpointer u) { (void)b; (void)u; zoom_step(+1); }
static void on_zoom_fit(GtkButton *b, gpointer u) { (void)b; (void)u; zoom_fit(0); }
static void on_zoom_one(GtkButton *b, gpointer u) { (void)b; (void)u; zoom_set(1.0); }

static void on_ed_pressed(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    int msg;

    (void)ud;
    gtk_widget_grab_focus(P.editor);
    if (button == 3)      { P.buttons |= MK_RBUTTON; msg = WM_RBUTTONDOWN; }
    else if (button == 2) { P.buttons |= MK_MBUTTON; msg = WM_MBUTTONDOWN; }
    else                  { P.buttons |= MK_LBUTTON;
                            msg = (n >= 2) ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN; }
    ed_mouse((int)x, (int)y, msg, 0);
}

static void on_ed_released(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    int msg;

    (void)n; (void)ud;
    if (button == 3)      { P.buttons &= ~MK_RBUTTON; msg = WM_RBUTTONUP; }
    else if (button == 2) { P.buttons &= ~MK_MBUTTON; msg = WM_MBUTTONUP; }
    else                  { P.buttons &= ~MK_LBUTTON; msg = WM_LBUTTONUP; }
    ed_mouse((int)x, (int)y, msg, 0);
}

static void on_ed_motion(GtkEventControllerMotion *m, double x, double y, gpointer ud)
{ (void)m; (void)ud; ed_mouse((int)x, (int)y, WM_MOUSEMOVE, 0); }

static gboolean on_ed_scroll(GtkEventControllerScroll *s, double dx, double dy, gpointer ud)
{
    GdkModifierType st;

    (void)dx; (void)ud;
    /* Ctrl and the wheel is the zoom everywhere else, and the plug-in is not
     * expecting it -- a bare wheel still belongs to whatever control is under
     * the pointer, which is the only way to work some editors. */
    st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(s));
    if (st & GDK_CONTROL_MASK) {
        if (dy != 0.0) zoom_step(dy < 0.0 ? 1 : -1);
        return TRUE;
    }
    /* GTK counts notches, Windows counts 120ths of one; the plug-in expects
     * the latter. Sign flipped: scrolling down is a negative delta there. */
    ed_mouse(0, 0, WM_MOUSEWHEEL, (int)(-dy));
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

static gboolean on_ed_key_down(GtkEventControllerKey *c, guint kv, guint code,
                               GdkModifierType st, gpointer ud)
{
    guint32 ch;
    (void)c; (void)code; (void)st; (void)ud;
    if (!P.host || !P.ed_open) return FALSE;
    ch = gdk_keyval_to_unicode(kv);
    in_plugin++;
    pehost_editor_key(P.host, gdk_to_vk(kv), 1, (int)ch);
    in_plugin--;
    return TRUE;
}

static void on_ed_key_up(GtkEventControllerKey *c, guint kv, guint code,
                         GdkModifierType st, gpointer ud)
{
    (void)c; (void)code; (void)st; (void)ud;
    if (!P.host || !P.ed_open) return;
    in_plugin++;
    pehost_editor_key(P.host, gdk_to_vk(kv), 0, 0);
    in_plugin--;
}

/* -------------------------------------------------------------- parameters */

typedef struct { int index; GtkWidget *value; } prow;

/* g_free has the wrong shape for a GClosureNotify, and casting it there is a
 * warning the compiler is right to give. */
static void prow_free(gpointer p, GClosure *c) { (void)c; g_free(p); }

static void on_param_changed(GtkRange *r, gpointer ud)
{
    prow *pr = ud;
    char  ds[64], lb[64], txt[160];

    if (P.loading || !P.host || in_plugin) return;
    in_plugin++;
    pehost_set_param(P.host, pr->index, (float)gtk_range_get_value(r));
    pehost_param_display(P.host, pr->index, ds, sizeof ds);
    pehost_param_label(P.host, pr->index, lb, sizeof lb);
    in_plugin--;
    snprintf(txt, sizeof txt, "%s %s", ds, lb);
    gtk_label_set_text(GTK_LABEL(pr->value), txt);
}

static void fill_params(void)
{
    int n, i;

    clear_list(P.paramlist);
    if (!P.host) return;

    P.loading = 1;
    n = pehost_num_params(P.host);
    for (i = 0; i < n; i++) {
        char nm[64], ds[64], lb[64], txt[160];
        GtkWidget *row, *name, *scale, *value;
        prow *pr;

        pehost_param_name(P.host, i, nm, sizeof nm);
        pehost_param_display(P.host, i, ds, sizeof ds);
        pehost_param_label(P.host, i, lb, sizeof lb);

        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);

        name = gtk_label_new(nm[0] ? nm : "-");
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_widget_set_size_request(name, 190, -1);

        scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_range_set_value(GTK_RANGE(scale), pehost_get_param(P.host, i));
        gtk_widget_set_hexpand(scale, TRUE);

        snprintf(txt, sizeof txt, "%s %s", ds, lb);
        value = gtk_label_new(txt);
        gtk_label_set_xalign(GTK_LABEL(value), 1.0f);
        gtk_widget_set_size_request(value, 130, -1);

        pr = g_new0(prow, 1);
        pr->index = i;
        pr->value = value;
        g_signal_connect_data(scale, "value-changed", G_CALLBACK(on_param_changed),
                              pr, prow_free, 0);

        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), scale);
        gtk_box_append(GTK_BOX(row), value);
        gtk_list_box_append(GTK_LIST_BOX(P.paramlist), row);
    }
    P.loading = 0;
}

static void fill_programs(void)
{
    int n, i;

    clear_list(P.proglist);
    if (!P.host) return;

    P.loading = 1;
    n = pehost_num_programs(P.host);
    for (i = 0; i < n; i++) {
        char pn[64] = { 0 }, lbl[96];
        GtkWidget *l;
        pehost_program_name(P.host, i, pn, sizeof pn);
        snprintf(lbl, sizeof lbl, "%3d  %s", i, pn[0] ? pn : "-");
        l = gtk_label_new(lbl);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_widget_set_margin_start(l, 4);
        gtk_list_box_append(GTK_LIST_BOX(P.proglist), l);
    }
    if (n > 0)
        gtk_list_box_select_row(GTK_LIST_BOX(P.proglist),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(P.proglist),
                                          pehost_get_program(P.host)));
    P.loading = 0;
}

/* ------------------------------------------------------------------ loading */

static void set_header(void)
{
    char txt[512];

    if (!P.host) {
        gtk_label_set_text(GTK_LABEL(P.header), "no plug-in loaded");
        return;
    }
    snprintf(txt, sizeof txt,
             "%s — %s\n%s   in %d / out %d   programs %d   params %d",
             pehost_name(P.host), pehost_vendor(P.host),
             pehost_is_synth(P.host) ? "synth" : "effect",
             pehost_num_inputs(P.host), pehost_num_outputs(P.host),
             pehost_num_programs(P.host), pehost_num_params(P.host));
    gtk_label_set_text(GTK_LABEL(P.header), txt);
}

static void unload_locked(void)
{
    if (!P.host) return;
    /* The editor first: it holds a pointer to this plug-in, and the run-loop
     * watches hold pointers into it. Closing the plug-in with either still
     * live is a callback into freed memory. */
    native_editor_close();
    /* The pane goes back to following the window. A size request left behind by
     * a large editor would keep the scrollbars up for the next plug-in loaded,
     * whatever size that one turns out to be. */
    if (GTK_IS_WIDGET(P.editor)) gtk_widget_set_size_request(P.editor, -1, -1);
    P.ed_nat_w = P.ed_nat_h = 0;
    P.ed_base_w = P.ed_base_h = 0;
    P.ed_zoom = 1.0;
    zoom_update_ui();
    atomic_store_explicit(&P.live, 0, memory_order_release);
    P.ed_open = 0;
    P.ed_native = 0;
    pehost_close(P.host);
    P.host = NULL;
}

static void load(const entry *e)
{
    char msg[1024];

    /* Refuse rather than free a plug-in that is mid-call. */
    if (in_plugin) return;
    in_plugin++;

    /* Park first, always. The callback may be inside pehost_render_io on the
     * plug-in we are about to close, and closing it under a realtime thread is
     * a crash while playing rather than a tidy failure. */
    if (P.park) P.park();
    unload_locked();
    P.host = pehost_open_as(e->path, PEHOST_KIND_AUTO, P.rate, P.block);
    if (P.host) atomic_store_explicit(&P.live, 1, memory_order_release);
    if (P.unpark) P.unpark();

    if (!P.host) {
        plug_status("%s: %s", e->name, pehost_last_error());
        set_header();
        clear_list(P.proglist);
        clear_list(P.paramlist);
        in_plugin--;
        return;
    }

    set_header();
    fill_programs();
    fill_params();

    {
        int kind = pehost_editor_kind(P.host);
        int w = 0, h = 0;
        if (kind == PEHOST_EDITOR_PIXELS && pehost_editor_open(P.host) == 0) {
            P.ed_open = 1;
            pehost_editor_size(P.host, &w, &h);
            if (w > 0 && h > 0) {
                P.ed_w = w; P.ed_h = h;
                P.ed_base_w = w; P.ed_base_h = h;
                gtk_widget_set_size_request(P.editor, w, h);
                zoom_update_ui();
                /* Fit it if it does not already fit, one turn later -- the pane
                 * has not been laid out at this editor's size yet. */
                g_idle_add(zoom_fit_idle, NULL);
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
            P.ed_native = 1;
            pehost_editor_size(P.host, &w, &h);
            if (w > 0 && h > 0) {
                P.ed_w = w; P.ed_h = h;
                P.ed_nat_w = w; P.ed_nat_h = h;
                P.ed_base_w = w; P.ed_base_h = h;
                gtk_widget_set_size_request(P.editor, w, h);
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
        plug_status("%s", msg);
    }
    in_plugin--;

    /* Already looking at the Editor page: bring this plug-in's up, because the
     * page is not changing and nothing else will.
     *
     * Losing this line is what made 54 of 55 plug-ins show nothing -- the
     * first one attached from the page change and every one after it silently
     * did not. */
    if (P.ed_native && P.stack && !P.ed_attached) {
        const char *page = gtk_stack_get_visible_child_name(GTK_STACK(P.stack));
        if (page && !strcmp(page, "editor"))
            native_editor_open(P.host, pehost_name(P.host));
    }
}

/* Switching platform rescans and shows what that corpus holds. Loading the
 * first of them is fill_browser's doing, and is the same thing that happens at
 * startup -- picking "Linux native" should show a Linux plug-in, not an empty
 * pane waiting to be clicked. */
static void on_root_changed(GObject *dd, GParamSpec *ps, gpointer ud)
{
    guint i;

    (void)ps; (void)ud;
    if (P.loading) return;
    i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i == GTK_INVALID_LIST_POSITION || (int)i >= P.nroot) return;
    plugview_scan(P.roots[i].path);
    gtk_label_set_text(GTK_LABEL(P.dirlabel), P.roots[i].path);
    fill_browser();
}

/* Point the dropdown at `dir`, listing it as its own entry when it is not one
 * of the corpora. */
static void root_select_path(const char *dir, const char *label)
{
    int i;

    if (!dir || !dir[0] || !P.rootdd) return;
    i = roots_add(dir, label);
    if (i < 0) return;
    P.loading = 1;                       /* not a user choice: do not rescan */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(P.rootdd), (guint)i);
    P.loading = 0;
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

    (void)ps; (void)ud;
    /* GtkStack emits this while the pane is still being assembled -- adding
     * pages and attaching a switcher both move the visible child. Opening a
     * plug-in editor from inside plugview_new is not what the user asked for,
     * and it happened before anything was on screen. */
    if (!P.ready || !P.host || !P.ed_native || P.ed_attached) return;
    if (page && !strcmp(page, "editor"))
        native_editor_open(P.host, pehost_name(P.host));
}

static void on_plug_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
    int i;
    (void)lb; (void)ud;
    if (!row) return;
    i = gtk_list_box_row_get_index(row);
    if (i < 0 || i >= P.nplug) return;
    if (!P.plug[i].loadable) {
        plug_status("%s: %s", P.plug[i].name, P.plug[i].kind);
        return;
    }
    load(&P.plug[i]);
}

static void on_prog_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
    (void)lb; (void)ud;
    if (P.loading || !P.host || !row || in_plugin) return;
    in_plugin++;
    pehost_set_program(P.host, gtk_list_box_row_get_index(row));
    in_plugin--;
    fill_params();          /* a program change rewrites every parameter */
}

static void append_row(const entry *e)
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
    gtk_list_box_append(GTK_LIST_BOX(P.list), l);
}

/* Load one plug-in by path, wherever it came from. Already-listed ones are
 * selected rather than added twice. */
static void open_path(const char *path)
{
    pehost_info info;
    const char *base;
    entry      *e;
    char        why[160] = "";
    int         i;

    if (!pehost_can_load(path, why, (int)sizeof why)) {
        plug_status("%s: %s", path,
                    why[0] ? why : "not a plug-in this host can load");
        return;
    }
    for (i = 0; i < P.nplug; i++)
        if (!strcmp(P.plug[i].path, path)) {
            gtk_list_box_select_row(GTK_LIST_BOX(P.list),
                gtk_list_box_get_row_at_index(GTK_LIST_BOX(P.list), i));
            return;
        }
    if (P.nplug >= MAX_PLUGINS) return;

    base = strrchr(path, '/');
    e = &P.plug[P.nplug];
    snprintf(e->path, sizeof e->path, "%s", path);
    snprintf(e->name, sizeof e->name, "%s", base ? base + 1 : path);
    pehost_classify(path, &info);
    snprintf(e->kind, sizeof e->kind, "%s", pehost_kind_label(info.kind));
    e->loadable = 1;                          /* pehost_can_load said so above */
    append_row(e);
    P.nplug++;

    /* Selecting it is what loads it -- one path in, not two. */
    gtk_list_box_select_row(GTK_LIST_BOX(P.list),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(P.list), P.nplug - 1));
}

static void on_vst_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *err = NULL;
    GFile  *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
    char   *path;

    (void)ud;
    g_clear_error(&err);
    g_object_unref(src);                         /* the ref taken in open_vst */
    if (!f) return;                              /* cancelled */
    if ((path = g_file_get_path(f))) { open_path(path); g_free(path); }
    g_object_unref(f);
}

void plugview_open_vst(GtkWindow *parent)
{
    GtkFileDialog *d = gtk_file_dialog_new();

    gtk_file_dialog_set_title(d, "Open VST");
    gtk_file_dialog_open(d, parent, NULL, on_vst_chosen, NULL);
}

static void on_dir_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    (void)ud;
    if (!f) return;
    {
        char *path = g_file_get_path(f);
        if (path) {
            const char *base = strrchr(path, '/');
            char lbl[64];
            snprintf(lbl, sizeof lbl, "%s  [added]", base && base[1] ? base + 1 : path);
            plugview_scan(path);
            gtk_label_set_text(GTK_LABEL(P.dirlabel), path);
            root_select_path(path, lbl);
            fill_browser();
            g_free(path);
        }
    }
    g_object_unref(f);
}

void plugview_load_folder(GtkWindow *parent)
{
    GtkFileDialog *d = gtk_file_dialog_new();

    gtk_file_dialog_set_title(d, "Load plug-in folder");
    gtk_file_dialog_select_folder(d, parent, NULL, on_dir_chosen, NULL);
    g_object_unref(d);
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
void plugview_install_missing_data(void)
{
    int i, files = 0, plugins = 0, failed = 0;
    char lasterr[160] = "";

    for (i = 0; i < P.nplug; i++) {
        pehost_data_need dn;
        char err[160];
        int  n;

        if (!P.plug[i].repairable) continue;
        /* Re-checked rather than trusting what the scan recorded: the folders
         * may have moved, and this is the call that is about to write. */
        if (!pehost_data_check(P.plug[i].path, &dn) || !dn.repairable) continue;
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
        plug_status("nothing to install -- no scanned plug-in is missing data "
                    "that this machine has a copy of");
        return;
    }
    if (!plugins) {
        plug_status("could not install: %s", lasterr[0] ? lasterr : "unknown error");
        return;
    }
    /* Reload, because these are read when the editor is built: a plug-in that
     * is already open went looking before the folders existed. */
    plug_status("linked %d folder(s) for %d plug-in(s)%s -- reload one to see it",
                files, plugins, failed ? ", some failed" : "");
    plugview_scan(P.dir);
    fill_browser();
}

/* ------------------------------------------------------------ the audio API */

int plugview_active(void)
{ return atomic_load_explicit(&P.live, memory_order_acquire); }

int plugview_render(float *out, int frames)
{
    float pk = 0.0f;
    int   i, cur;

    if (!atomic_load_explicit(&P.live, memory_order_acquire) || !P.host) return 0;
    /* NULL input: a synth ignores it, and an effect correctly renders silence
     * rather than being fed a tone it was never sent. */
    pehost_render_io(P.host, NULL, out, frames);

    /* Peak for the meter. Kept here rather than in the GTK thread because this
     * is the only place the samples exist, and "it loaded and the editor drew"
     * is not the same claim as "it is audible". */
    for (i = 0; i < frames * 2; i++) {
        float a = out[i] < 0.0f ? -out[i] : out[i];
        if (a > pk) pk = a;
    }
    cur = (int)(pk * 1000.0f);
    if (cur > atomic_load_explicit(&P.peak_milli, memory_order_relaxed))
        atomic_store_explicit(&P.peak_milli, cur, memory_order_relaxed);
    return 1;
}

void plugview_note_on(int note, int vel)  { if (P.host) pehost_note_on(P.host, note, vel); }
void plugview_note_off(int note)          { if (P.host) pehost_note_off(P.host, note); }
void plugview_all_notes_off(void)         { if (P.host) pehost_all_notes_off(P.host); }

void plugview_bend(int value14)
{
    if (!P.host) return;
    if (value14 < 0) value14 = 0;
    if (value14 > 16383) value14 = 16383;
    in_plugin++;
    pehost_midi(P.host, 0xE0, value14 & 0x7F, (value14 >> 7) & 0x7F);
    in_plugin--;
}

void plugview_program(int idx)
{
    if (!P.host) return;
    gtk_list_box_select_row(GTK_LIST_BOX(P.proglist),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(P.proglist), idx));
}

/* ------------------------------------------------------------------ --cycle */

static int g_cycle_ms, g_cycle_at, g_cycle_report = -1;

/* One line per plug-in, in the same shape pestudio prints. */
static void cycle_report(int i)
{
    fprintf(stderr, "plugview: cycle %d/%d %s -- %s\n", i + 1, P.nplug,
            P.plug[i].name,
            !P.plug[i].loadable ? "not loadable"
            : !P.host           ? "load failed"
            : P.ed_attached     ? "editor attached"
            : P.ed_native       ? "editor did not attach"
            : P.ed_open         ? "editor (pixels)"
                                : "no editor");
}

static gboolean cycle_step(gpointer u)
{
    (void)u;
    /* Report the previous plug-in first, not in the step that loaded it. A
     * native editor attaches when the Editor page gets its first layout, which
     * is after the page switch returns -- printing in the same step called
     * every first plug-in "did not attach" while its editor came up a moment
     * later. One interval later the attach has either happened or it has not,
     * and the line also confirms the last plug-in survived a full interval
     * alongside the new one. */
    if (g_cycle_report >= 0) {
        cycle_report(g_cycle_report);
        g_cycle_report = -1;
    }
    if (g_cycle_at >= P.nplug) {
        fprintf(stderr, "plugview: cycle finished (%d plug-in(s))\n", P.nplug);
        return G_SOURCE_REMOVE;
    }
    {
        int i = g_cycle_at++;
        gtk_list_box_select_row(GTK_LIST_BOX(P.list),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(P.list), i));
        gtk_stack_set_visible_child_name(GTK_STACK(P.stack), "editor");
        g_cycle_report = i;
    }
    return G_SOURCE_CONTINUE;
}

void plugview_start_cycle(int ms)
{
    g_cycle_ms = ms > 0 ? ms : 1500;
    g_cycle_at = 0;
    g_cycle_report = -1;
    g_timeout_add(g_cycle_ms, cycle_step, NULL);
}

void plugview_shutdown(void)
{
    /* The timeouts go first, before anything they read is torn down.
     *
     * Both hold widget pointers and both keep firing while the toplevel
     * finalises its children, so the meter's gtk_label_set_text ran against a
     * freed label -- ten times a second, for as long as the teardown took.
     * That is the `GTK_IS_LABEL (self)' assertion printed on the way out of
     * every session. Clearing the pointers as well means a source that somehow
     * outlives this call is inert rather than merely unlikely to run. */
    if (P.tick)  { g_source_remove(P.tick);  P.tick  = 0; }
    if (P.meter) { g_source_remove(P.meter); P.meter = 0; }
    unload_locked();
    native_editor_destroy();
    P.status = NULL;
    P.editor = NULL;
}

/* ------------------------------------------------------------------ the pane */

GtkWidget *plugview_new(void (*park)(void), void (*unpark)(void),
                        double samplerate, int blocksize)
{
    GtkWidget *left, *right, *paned, *top, *sw, *progsw, *sws;
    GtkWidget *sidebar;
    GtkEventController *ctl;
    GtkGesture *click;
    int i;

    P.park   = park;
    P.unpark = unpark;
    P.ed_zoom = 1.0;          /* a static struct starts at 0, which is no picture */
    P.rate   = samplerate > 0 ? samplerate : 48000.0;
    P.block  = blocksize > 0 ? blocksize : 512;

    /* ---- left: directory, plug-ins, programs ---- */
    /* Which platform's plug-ins to browse. The tree carries a corpus per
     * platform and format, and this is how you get between them -- the same
     * selector pestudio puts at the top of its own list. */
    roots_discover();
    P.rootmodel = gtk_string_list_new(NULL);
    for (i = 0; i < P.nroot; i++) gtk_string_list_append(P.rootmodel, P.roots[i].label);
    P.rootdd = gtk_drop_down_new(G_LIST_MODEL(P.rootmodel), NULL);
    gtk_widget_set_hexpand(P.rootdd, TRUE);

    /* The folder being shown. Choosing a different one is File > Load Folder,
     * not a button here. */
    top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    P.dirlabel = gtk_label_new(P.dir[0] ? P.dir : "(no folder)");
    gtk_label_set_ellipsize(GTK_LABEL(P.dirlabel), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(P.dirlabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(P.dirlabel), 0.0f);
    gtk_box_append(GTK_BOX(top), P.dirlabel);

    P.list = gtk_list_box_new();
    g_signal_connect(P.list, "row-selected", G_CALLBACK(on_plug_selected), NULL);
    sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), P.list);
    gtk_widget_set_vexpand(sw, TRUE);

    P.proglist = gtk_list_box_new();
    g_signal_connect(P.proglist, "row-selected", G_CALLBACK(on_prog_selected), NULL);
    progsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(progsw), P.proglist);
    gtk_widget_set_size_request(progsw, -1, 170);

    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(left), P.rootdd);
    gtk_box_append(GTK_BOX(left), top);
    gtk_box_append(GTK_BOX(left), gtk_label_new("Plug-ins"));
    gtk_box_append(GTK_BOX(left), sw);
    gtk_box_append(GTK_BOX(left), gtk_label_new("Programs"));
    gtk_box_append(GTK_BOX(left), progsw);
    gtk_widget_set_size_request(left, 300, -1);

    /* ---- right: header, then Parameters | Editor ---- */
    P.header = gtk_label_new("no plug-in loaded");
    gtk_label_set_xalign(GTK_LABEL(P.header), 0.0f);

    P.paramlist = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(P.paramlist), GTK_SELECTION_NONE);
    P.paramsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(P.paramsw), P.paramlist);
    gtk_widget_set_vexpand(P.paramsw, TRUE);

    P.editor = gtk_drawing_area_new();
    gtk_widget_set_focusable(P.editor, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(P.editor), editor_draw, NULL, NULL);
    /* A foreign X window does not move with GTK's layout, so it is put back
     * over the pane whenever the pane changes shape. */
    g_signal_connect(P.editor, "resize", G_CALLBACK(on_editor_resize), NULL);

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);   /* any button */
    g_signal_connect(click, "pressed",  G_CALLBACK(on_ed_pressed),  NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_ed_released), NULL);
    gtk_widget_add_controller(P.editor, GTK_EVENT_CONTROLLER(click));

    ctl = gtk_event_controller_motion_new();
    g_signal_connect(ctl, "motion", G_CALLBACK(on_ed_motion), NULL);
    gtk_widget_add_controller(P.editor, ctl);

    ctl = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(ctl, "scroll", G_CALLBACK(on_ed_scroll), NULL);
    gtk_widget_add_controller(P.editor, ctl);

    ctl = gtk_event_controller_key_new();
    g_signal_connect(ctl, "key-pressed",  G_CALLBACK(on_ed_key_down), NULL);
    g_signal_connect(ctl, "key-released", G_CALLBACK(on_ed_key_up),   NULL);
    gtk_widget_add_controller(P.editor, ctl);

    P.editorsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(P.editorsw), P.editor);
    gtk_widget_set_vexpand(P.editorsw, TRUE);
    /* Scrollbars that take their own strip, not GTK's overlay ones.
     *
     * An overlay scrollbar is drawn on top of the viewport, and the viewport is
     * exactly what the plug-in's X window covers -- a foreign child sits above
     * everything GTK paints, so the scrollbars would be invisible under it and
     * unclickable through it. Given their own space they sit outside the clip
     * window, stay visible, and can be dragged, which is the only way to pan a
     * large editor: the wheel over the editor itself belongs to the plug-in,
     * which uses it for its own controls. */
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(P.editorsw), FALSE);

    /* The zoom bar, above the viewport rather than floating over it: a foreign
     * X11 child sits on top of everything GTK paints, so anything overlaid on
     * the editor would be invisible and unclickable exactly when it is a native
     * editor that needs it -- the same reason the scrollbars were given their
     * own strip. */
    {
        GtkWidget *zb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        P.zoom_out = gtk_button_new_with_label("\xe2\x88\x92");
        P.zoom_in  = gtk_button_new_with_label("+");
        P.zoom_fit = gtk_button_new_with_label("Fit");
        P.zoom_one = gtk_button_new_with_label("1:1");
        P.zoom_lbl = gtk_label_new("100%");
        P.zoom_note = gtk_label_new("");
        gtk_widget_set_size_request(P.zoom_lbl, 48, -1);
        gtk_label_set_xalign(GTK_LABEL(P.zoom_note), 0.0f);
        gtk_widget_set_hexpand(P.zoom_note, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(P.zoom_note), PANGO_ELLIPSIZE_END);
        gtk_widget_set_tooltip_text(P.zoom_out, "Zoom out  (Ctrl+wheel over the editor)");
        gtk_widget_set_tooltip_text(P.zoom_in,  "Zoom in  (Ctrl+wheel over the editor)");
        gtk_widget_set_tooltip_text(P.zoom_fit, "Scale the editor to fit the space there is");
        gtk_widget_set_tooltip_text(P.zoom_one, "Back to the size the plug-in drew");
        /* Not focus stops. Tab is how you get out of the plug-in list, and four
         * more places for the keyboard to end up is four more ways to be typing
         * at something that is not the synth. */
        gtk_widget_set_focus_on_click(P.zoom_out, FALSE);
        gtk_widget_set_focus_on_click(P.zoom_in,  FALSE);
        gtk_widget_set_focus_on_click(P.zoom_fit, FALSE);
        gtk_widget_set_focus_on_click(P.zoom_one, FALSE);
        g_signal_connect(P.zoom_out, "clicked", G_CALLBACK(on_zoom_out), NULL);
        g_signal_connect(P.zoom_in,  "clicked", G_CALLBACK(on_zoom_in),  NULL);
        g_signal_connect(P.zoom_fit, "clicked", G_CALLBACK(on_zoom_fit), NULL);
        g_signal_connect(P.zoom_one, "clicked", G_CALLBACK(on_zoom_one), NULL);

        gtk_box_append(GTK_BOX(zb), gtk_label_new("Zoom"));
        gtk_box_append(GTK_BOX(zb), P.zoom_out);
        gtk_box_append(GTK_BOX(zb), P.zoom_lbl);
        gtk_box_append(GTK_BOX(zb), P.zoom_in);
        gtk_box_append(GTK_BOX(zb), P.zoom_fit);
        gtk_box_append(GTK_BOX(zb), P.zoom_one);
        gtk_box_append(GTK_BOX(zb), P.zoom_note);
        gtk_box_append(GTK_BOX(page), zb);
        gtk_box_append(GTK_BOX(page), P.editorsw);
        P.editorpage = page;
    }

    P.stack = gtk_stack_new();
    gtk_stack_add_titled(GTK_STACK(P.stack), P.paramsw,  "params", "Parameters");
    gtk_stack_add_titled(GTK_STACK(P.stack), P.editorpage, "editor", "Editor");
    g_signal_connect(P.stack, "notify::visible-child", G_CALLBACK(on_page_changed), NULL);
    sws = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(sws), GTK_STACK(P.stack));
    gtk_widget_set_halign(sws, GTK_ALIGN_START);

    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(right), P.header);
    gtk_box_append(GTK_BOX(right), sws);
    gtk_box_append(GTK_BOX(right), P.stack);

    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_position(GTK_PANED(paned), 320);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);

    P.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(P.status), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(P.status), PANGO_ELLIPSIZE_MIDDLE);

    sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(sidebar), paned);
    gtk_box_append(GTK_BOX(sidebar), P.status);

    P.root = sidebar;
    /* Whatever plugview_scan was told to look at before the pane existed --
     * dwstudio's --dir, or its default -- becomes the selected entry. */
    root_select_path(P.dir, "given folder");
    g_signal_connect(P.rootdd, "notify::selected", G_CALLBACK(on_root_changed), NULL);
    fill_browser();
    {   /* Where a native editor's descriptors and timers end up. Installed
         * once: only one editor is open at a time. */
        static const v3_runloop_hooks hooks = {
            NULL, hook_add_fd, hook_del_fd, hook_add_timer, hook_del_timer, hook_resize
        };
        v3_set_runloop_hooks(&hooks);
    }
    zoom_update_ui();
    P.tick  = g_timeout_add(30, editor_tick, NULL);
    P.meter = g_timeout_add(100, meter_tick, NULL);
    P.ready = 1;
    return P.root;
}
