/* System V VST2 as a library, for pehost to dispatch to.
 *
 * Serves two loaders through one driver. A macOS VST2 is a Mach-O bundle loaded
 * by machoload.c; a native Linux VST2 is an ordinary ELF shared object loaded by
 * dlopen. Everything after the entry point is identical, because VST2 on both is
 * plain System V with the same AEffect layout -- so the host callback, the event
 * queue, the render path and the editor are shared rather than duplicated.
 *
 * The macvst_ prefix predates the Linux side; it is kept because renaming forty
 * call sites buys nothing.
 *
 * The CLI in macvst.c proved the path; this is the same sequence behind the
 * interface pehost needs, plus the lock-free event queue that lets the GUI
 * thread send notes while the audio thread renders.
 *
 * No ABI layer: macOS x86-64 is System V, the same convention the host uses, so
 * this runs in-process. That is the whole reason the 32-bit Windows plugins need
 * a helper process and these do not.
 */
#define _GNU_SOURCE
#define VST2_SYSV 1
#include <dlfcn.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machoload.h"
#include "macshim.h"
#include "macvsthost.h"
#include "vst2.h"

#define EVQ 512

typedef struct { unsigned char status, d1, d2, pad; } ev_t;

struct macvst {
    macho   *img;         /* Mach-O bundle, or NULL for a native ELF */
    void    *dl;          /* dlopen handle, for a native ELF */
    int      editor_open;
    AEffect *fx;
    double   sr;
    int      bs;
    int      nin, nout;
    float  **in, **out;
    int      cap;
    char     name[80], vendor[80];

    ev_t              q[EVQ];
    _Atomic unsigned  head, tail;

    /* Built once at open, so the audio thread never calls the allocator.
     * pehost's Windows VST2 path has always done this; this driver -- which is
     * what every native Linux VST2 goes through -- was calloc'ing and freeing
     * twice per block whenever a note was pending. malloc can block on the
     * arena lock, and a block that misses its deadline is a dropout. */
    VstEvents        *evblk;
    int               evblk_n;
    VstMidiEvent      evmidi[EVQ];

    /* The folder the plugin was loaded from, for audioMasterGetDirectory. Only
     * the native loader fills this; a Mach-O bundle answers from its own. */
    char              dir[1024];

    /* The host-owned NSView an editor is grafted into. See macvst_editor_open. */
    void             *parent;

    /* The transport handed back from audioMasterGetTime. It has to outlive the
     * callback, because the plugin keeps the pointer. */
    VstTimeInfo       time;
    double            play_pos;
};

static char g_err[256];
const char *macvst_last_error(void) { return g_err; }

/* The host callback. Only the queries a plugin asks during setup matter here. */
static intptr_t host_cb(AEffect *fx, int32_t op, int32_t idx, intptr_t val,
                        void *ptr, float opt)
{
    macvst *h = fx ? (macvst *)fx->user : NULL;
    (void)idx; (void)val; (void)opt;
    switch (op) {
    case 1:  return 2400;                       /* audioMasterVersion       */
    case 16: return (intptr_t)(h ? h->sr : 48000);
    case 17: return h ? h->bs : 512;
    case 23: return 1;                          /* GetCurrentProcessLevel   */
    case 32: return 1;                          /* GetAutomationState       */
    case 13: return 1;                          /* SizeWindow               */
    case 33: case 34:
        if (ptr) snprintf(ptr, 64, "peload");
        return 1;
    case 37: {
        /* audioMasterCanDo. `ptr` names the thing being asked about.
         *
         * The one that matters on macOS is Cockos' "hasCockosViewAsConfig".
         * Answering it with 0xbeef0000 is how a host says that the pointer it
         * passes to effEditOpen is an NSView*, and not a Carbon WindowRef.
         * iPlug1-era plugins read silence as "Carbon" and go off into
         * HIToolbox, which is not implemented here and never will be -- so they
         * built no view, drew nothing, and reported an empty rect. That is
         * every Audio Damage editor in this corpus.
         *
         * The rest are the ordinary VST2 host capabilities. Answering them is
         * not decoration: a plugin that asks whether it may send MIDI and is
         * told nothing will not send any. */
        const char *what = ptr;
        if (!what) return 0;
        if (!strcmp(what, "hasCockosViewAsConfig")) return (intptr_t)0xbeef0000;
        if (!strcmp(what, "sendVstEvents")      || !strcmp(what, "sendVstMidiEvent") ||
            !strcmp(what, "sendVstTimeInfo")    || !strcmp(what, "receiveVstEvents") ||
            !strcmp(what, "receiveVstMidiEvent")|| !strcmp(what, "receiveVstTimeInfo") ||
            !strcmp(what, "sizeWindow")         || !strcmp(what, "supplyIdle") ||
            !strcmp(what, "startStopProcess")   || !strcmp(what, "acceptIOChanges") ||
            !strcmp(what, "supportShell"))
            return 1;
        return 0;
    }
    case 7:
        /* audioMasterGetTime. Filled fresh each ask, from the position the
         * render loop maintains. */
        if (!h) return 0;
        vst_time_set(&h->time, h->play_pos, h->sr);
        return (intptr_t)&h->time;
    case 41:
        /* audioMasterGetDirectory: the plugin's own folder, as a pointer the
         * plugin reads and does not own. Without it a plugin falls back to a
         * path relative to the working directory -- which is how three of these
         * came to open "<Name>.vst/Contents/Resources/..." and get nothing, and
         * therefore render silence with no error anywhere.
         *
         * A native ELF has no bundle, and macho_bundle_path(NULL) answers with
         * the empty string -- which is worse than not answering, because "" is
         * a non-NULL pointer a plugin will happily join a filename onto and go
         * looking at the root of the filesystem. Its own directory is what it
         * should get, and 0 -- "not handled" -- when even that is unknown. */
        if (!h) return 0;
        if (h->img) return (intptr_t)macho_bundle_path(h->img);
        return h->dir[0] ? (intptr_t)h->dir : 0;
    default: return 0;
    }
}

static void free_bufs(macvst *h)
{
    int i;
    if (h->in)  for (i = 0; i < h->nin; i++)  { free(h->in[i]);  h->in[i] = NULL; }
    if (h->out) for (i = 0; i < h->nout; i++) { free(h->out[i]); h->out[i] = NULL; }
    h->cap = 0;
}

static int alloc_bufs(macvst *h, int frames)
{
    int i;
    if (frames <= h->cap) return 0;
    free_bufs(h);
    for (i = 0; i < h->nin; i++)
        if (!(h->in[i] = calloc((size_t)frames, sizeof **h->in))) return -1;
    for (i = 0; i < h->nout; i++)
        if (!(h->out[i] = calloc((size_t)frames, sizeof **h->out))) return -1;
    h->cap = frames;
    return 0;
}

typedef AEffect *(*vst_entry)(intptr_t (*)(AEffect *, int32_t, int32_t,
                                           intptr_t, void *, float));

/* Everything past the entry point, shared by both loaders. */
static macvst *vst_start(macvst *h, vst_entry entry)
{
    int nchan;

    if (!entry) {
        snprintf(g_err, sizeof g_err, "no VSTPluginMain export");
        macvst_close(h); return NULL;
    }

    if (!(h->fx = entry(host_cb)) || h->fx->magic != 0x56737450) {
        snprintf(g_err, sizeof g_err, "VSTPluginMain gave no valid AEffect");
        macvst_close(h); return NULL;
    }
    /* The callback needs to find us, and `user` is the field the SDK reserves
     * for exactly that. */
    h->fx->user = h;

    h->nin  = h->fx->numInputs  < 0 ? 0 : h->fx->numInputs;
    h->nout = h->fx->numOutputs < 1 ? 1 : h->fx->numOutputs;
    if (h->nin > 256 || h->nout > 256) {
        snprintf(g_err, sizeof g_err, "implausible channel count (in %d out %d)",
                 h->nin, h->nout);
        macvst_close(h); return NULL;
    }
    nchan = h->nin > h->nout ? h->nin : h->nout;
    h->in  = calloc((size_t)nchan + 1, sizeof *h->in);
    h->out = calloc((size_t)nchan + 1, sizeof *h->out);
    if (!h->in || !h->out || alloc_bufs(h, h->bs)) {
        snprintf(g_err, sizeof g_err, "buffer allocation failed");
        macvst_close(h); return NULL;
    }

    {   /* Sized to the queue, which is the most a single drain can produce. */
        size_t maxev = sizeof h->evmidi / sizeof h->evmidi[0];
        size_t bytes = offsetof(VstEvents, events) + maxev * sizeof(void *);
        h->evblk   = calloc(1, bytes);
        h->evblk_n = h->evblk ? (int)maxev : 0;
    }

    h->fx->dispatcher(h->fx, effOpen, 0, 0, NULL, 0.0f);
    h->fx->dispatcher(h->fx, effSetSampleRate, 0, 0, NULL, (float)h->sr);
    h->fx->dispatcher(h->fx, effSetBlockSize, 0, h->bs, NULL, 0.0f);
    h->fx->dispatcher(h->fx, effMainsChanged, 0, 1, NULL, 0.0f);
    h->fx->dispatcher(h->fx, effGetEffectName, 0, 0, h->name, 0.0f);
    h->fx->dispatcher(h->fx, effGetVendorString, 0, 0, h->vendor, 0.0f);
    return h;
}

static macvst *vst_alloc(double samplerate, int blocksize)
{
    macvst *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    g_err[0] = 0;
    h->sr = samplerate > 0 ? samplerate : 48000.0;
    h->bs = blocksize > 0 ? blocksize : 512;
    return h;
}

/* A macOS VST2: a Mach-O bundle. */
macvst *macvst_open(const char *path, double samplerate, int blocksize)
{
    macvst *h = vst_alloc(samplerate, blocksize);
    vst_entry entry;

    if (!h) return NULL;
    if (!(h->img = macho_open(path))) {
        snprintf(g_err, sizeof g_err, "%s", macho_last_error());
        free(h); return NULL;
    }
    macho_run_init(h->img);
    if (!(entry = (vst_entry)macho_symbol(h->img, "VSTPluginMain")))
        entry = (vst_entry)macho_symbol(h->img, "main_macho");
    return vst_start(h, entry);
}

/* A native Linux VST2: an ordinary ELF shared object. No loader of our own is
 * needed -- the dynamic linker does it, and the ABI is already System V. The
 * only wrinkle is the entry point's name: the 2.4 SDK renamed `main` to
 * VSTPluginMain, and plugins of that vintage export whichever they were built
 * against (some export both, with `main` aliased). */
macvst *macvst_open_native(const char *path, double samplerate, int blocksize)
{
    macvst *h = vst_alloc(samplerate, blocksize);
    vst_entry entry;

    if (!h) return NULL;
    if (!(h->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL))) {
        snprintf(g_err, sizeof g_err, "%s", dlerror());
        free(h); return NULL;
    }
    {   /* Everything up to the last slash, which is what the plugin is told
         * when it asks where it lives. Resolved first, because a plugin joins
         * relative paths onto this and a symlinked .so would send it to the
         * link's folder rather than the release it belongs to. */
        /* PATH_MAX because realpath() writes up to that much, and glibc's
         * fortified form rejects a smaller buffer outright rather than on a
         * long path -- the same trap plugview.c hit. */
        char        real[PATH_MAX];
        const char *use = realpath(path, real) ? real : path;
        const char *slash = strrchr(use, '/');
        if (slash && slash != use && (size_t)(slash - use) < sizeof h->dir)
            snprintf(h->dir, (size_t)(slash - use) + 1, "%s", use);
    }
    if (!(entry = (vst_entry)dlsym(h->dl, "VSTPluginMain")))
        if (!(entry = (vst_entry)dlsym(h->dl, "main")))
            entry = (vst_entry)dlsym(h->dl, "main_plugin");
    return vst_start(h, entry);
}

void macvst_close(macvst *h)
{
    if (!h) return;
    if (h->fx) {
        if (h->editor_open) h->fx->dispatcher(h->fx, effEditClose, 0, 0, NULL, 0.0f);
        h->fx->dispatcher(h->fx, effMainsChanged, 0, 0, NULL, 0.0f);
        h->fx->dispatcher(h->fx, effClose, 0, 0, NULL, 0.0f);
    }
    /* After the plugin has had its say -- effClose may still touch its view --
     * and before the image goes away, since anything left pointing into it
     * becomes a dangling call on the next plugin's first pump. */
    macns_reset_gui();
    macmetal_reset();
    macquartz_reset_editor();
    free_bufs(h);
    free(h->in); free(h->out);
    free(h->evblk);
    if (h->img) macho_close(h->img);
    /* Left loaded deliberately: a JUCE or u-he plugin registers atexit handlers
     * and static state that dlclose runs teardown on, and several crash doing it.
     * Leaking one mapping per load is the cheaper trade, and matches how the
     * Mach-O side treats its images. */
    free(h);
}

const char *macvst_name(const macvst *h)   { return h ? h->name : ""; }
const char *macvst_vendor(const macvst *h) { return h ? h->vendor : ""; }
int macvst_num_programs(const macvst *h) { return h && h->fx ? h->fx->numPrograms : 0; }
int macvst_num_params(const macvst *h)   { return h && h->fx ? h->fx->numParams : 0; }
int macvst_num_inputs(const macvst *h)   { return h ? h->nin : 0; }
int macvst_num_outputs(const macvst *h)  { return h ? h->nout : 0; }
int macvst_is_synth(const macvst *h)
{ return h && h->fx ? ((h->fx->flags & 0x100) != 0) : 0; }
int macvst_unique_id(const macvst *h)    { return h && h->fx ? h->fx->uniqueID : 0; }

static void ask(macvst *h, int op, int idx, char *buf, int n)
{
    char tmp[128] = { 0 };
    if (n > 0) buf[0] = 0;
    if (!h || !h->fx) return;
    h->fx->dispatcher(h->fx, op, idx, 0, tmp, 0.0f);
    snprintf(buf, (size_t)n, "%s", tmp);
}
void macvst_param_name(macvst *h, int i, char *b, int n)
{ ask(h, effGetParamName, i, b, n); }
void macvst_param_label(macvst *h, int i, char *b, int n)
{ ask(h, effGetParamLabel, i, b, n); }
void macvst_param_display(macvst *h, int i, char *b, int n)
{ ask(h, effGetParamDisplay, i, b, n); }
void macvst_program_name(macvst *h, int i, char *b, int n)
{
    if (n > 0) b[0] = 0;
    if (!h || !h->fx) return;
    /* The buffer, not the return value, says whether it answered -- JUCE returns
     * 1 without writing anything. Left empty when there is no name; the caller
     * decides what to show, so there is one policy rather than two. */
    { char tmp[128] = { 0 };
      h->fx->dispatcher(h->fx, effGetProgramNameIndexed, i, -1, tmp, 0.0f);
      tmp[sizeof tmp - 1] = 0;
      if (!tmp[0]) h->fx->dispatcher(h->fx, effGetProgramName, 0, 0, tmp, 0.0f);
      tmp[sizeof tmp - 1] = 0;
      snprintf(b, (size_t)n, "%s", tmp); }
}
void macvst_set_program(macvst *h, int i)
{ if (h && h->fx) h->fx->dispatcher(h->fx, effSetProgram, 0, i, NULL, 0.0f); }
int macvst_get_program(macvst *h)
{ return h && h->fx ? (int)h->fx->dispatcher(h->fx, effGetProgram, 0, 0, NULL, 0.0f) : 0; }

float macvst_get_param(macvst *h, int i)
{ return (h && h->fx && i >= 0 && i < h->fx->numParams) ? h->fx->getParameter(h->fx, i) : 0.0f; }
void macvst_set_param(macvst *h, int i, float v)
{ if (h && h->fx && i >= 0 && i < h->fx->numParams) h->fx->setParameter(h->fx, i, v); }

/* Single-producer (GUI) single-consumer (audio) ring, drained at the top of
 * every render -- the same contract pehost's own queue keeps. */
void macvst_midi(macvst *h, int status, int d1, int d2)
{
    unsigned hd, tl;
    if (!h) return;
    hd = atomic_load_explicit(&h->head, memory_order_relaxed);
    tl = atomic_load_explicit(&h->tail, memory_order_acquire);
    if (hd - tl >= EVQ) return;
    h->q[hd % EVQ].status = (unsigned char)status;
    h->q[hd % EVQ].d1 = (unsigned char)(d1 & 0x7f);
    h->q[hd % EVQ].d2 = (unsigned char)(d2 & 0x7f);
    atomic_store_explicit(&h->head, hd + 1, memory_order_release);
}

void macvst_render_io(macvst *h, const float *src, float *inter, int frames)
{
    unsigned hd, tl;
    int nev = 0, i, k;

    if (!h || !h->fx || frames <= 0) {
        if (inter) memset(inter, 0, (size_t)frames * 2 * sizeof *inter);
        return;
    }
    if (alloc_bufs(h, frames)) {
        memset(inter, 0, (size_t)frames * 2 * sizeof *inter);
        return;
    }

    /* Drain the queue into one VstEvents block. The pointer array must be
     * contiguous with the header, so it is built as one allocation. */
    tl = atomic_load_explicit(&h->tail, memory_order_relaxed);
    hd = atomic_load_explicit(&h->head, memory_order_acquire);
    if (tl != hd) {
        int n = (int)(hd - tl);
        if (n > h->evblk_n) n = h->evblk_n;   /* the ring cannot hold more */
        if (h->evblk) {
            void **arr = (void **)((char *)h->evblk + offsetof(VstEvents, events));
            for (i = 0; i < n; i++, tl++) {
                ev_t e = h->q[tl % EVQ];
                memset(&h->evmidi[i], 0, sizeof h->evmidi[i]);
                h->evmidi[i].type = 1;
                h->evmidi[i].byteSize = 24;
                h->evmidi[i].midiData[0] = (char)e.status;
                h->evmidi[i].midiData[1] = (char)e.d1;
                h->evmidi[i].midiData[2] = (char)e.d2;
                arr[i] = &h->evmidi[i];
                nev++;
            }
            h->evblk->numEvents = nev;
            h->evblk->reserved  = 0;
            h->fx->dispatcher(h->fx, effProcessEvents, 0, 0, h->evblk, 0.0f);
        }
        atomic_store_explicit(&h->tail, hd, memory_order_release);
    }

    for (k = 0; k < h->nin; k++) {
        if (src) {
            int c = k < 2 ? k : k % 2;
            for (i = 0; i < frames; i++) h->in[k][i] = src[2 * i + c];
        } else {
            memset(h->in[k], 0, (size_t)frames * sizeof **h->in);
        }
    }
    for (k = 0; k < h->nout; k++) memset(h->out[k], 0, (size_t)frames * sizeof **h->out);

    h->fx->processReplacing(h->fx, h->nin ? h->in : NULL, h->out, frames);
    /* Advance the transport the plugin reads through audioMasterGetTime. A clock
     * that never moves is as good as no clock for anything tempo-synced. */
    h->play_pos += frames;

    for (i = 0; i < frames; i++) {
        inter[2 * i]     = h->out[0][i];
        inter[2 * i + 1] = h->nout >= 2 ? h->out[1][i] : h->out[0][i];
    }
}

/* ------------------------------------------------------------------ editor */

/* Every macOS editor in this corpus draws through NanoVG on Metal, which
 * macmetal.c implements in software -- so an "editor" here is a framebuffer the
 * plugin has rendered into, exactly like the Windows side's GDI surface. What
 * differs is what drives a frame: Windows editors animate off effEditIdle, while
 * an iPlug2 editor on macOS installs a timer on the run loop. There is no run
 * loop, so pumping fires the timers by hand. */
typedef struct { int16_t top, left, bottom, right; } ed_rect;

int macvst_editor_kind(macvst *h)
{
    /* Bit 0 of AEffect::flags is effFlagsHasEditor. */
    return (h && h->fx && (h->fx->flags & 1)) ? 2 : 0;
}

void macvst_editor_size(macvst *h, int *w, int *hh)
{
    ed_rect *r = NULL;
    if (w) *w = 0;
    if (hh) *hh = 0;
    if (!h || !h->fx) return;
    h->fx->dispatcher(h->fx, effEditGetRect, 0, 0, &r, 0.0f);
    if (!r) return;
    if (w)  *w  = r->right - r->left;
    if (hh) *hh = r->bottom - r->top;
}

int macvst_editor_open(macvst *h)
{
    int w = 0, hh = 0, k;
    if (!h || !h->fx || !macvst_editor_kind(h)) return -1;
    if (h->editor_open) return 0;
    macvst_editor_size(h, &w, &hh);
    /* A parent view, rather than NULL.
     *
     * An iPlug2 editor ignores what it is given and builds its own view either
     * way, which is why NULL served for as long as the corpus was iPlug2. A
     * VSTGUI editor does not: it grafts its frame into the view it is handed
     * and, given nothing, decides it is being hosted through Carbon instead --
     * a framework that is not here and is not coming. The runtime can mint an
     * NSView, so it does. */
    if (!h->parent) h->parent = macns_make_view(w, hh);
    h->fx->dispatcher(h->fx, effEditOpen, 0, 0, h->parent, 0.0f);
    h->fx->dispatcher(h->fx, effEditTop, 0, 0, NULL, 0.0f);
    h->editor_open = 1;

    /* Ask again now the editor exists, and keep asking, the way the Windows
     * host already does.
     *
     * The size above was taken before effEditOpen, and a plugin that sizes
     * itself from artwork it has not loaded yet answers that with an empty
     * rect. Taking it as final meant no drawable was ever seeded -- so the
     * plugin painted into nothing, no pixels came back, and the editor was
     * reported as refused. All five Audio Damage editors failed exactly there. */
    for (k = 0; k < 30 && (w <= 0 || hh <= 0); k++) {
        macvst_editor_pump(h);
        macvst_editor_size(h, &w, &hh);
    }
    /* Seed the drawable so the first frame has somewhere to land; the plugin
     * resizes it itself once it knows the backing scale. */
    if (w > 0 && hh > 0) macmetal_set_size(w, hh);

    /* Either backend counts: Metal for the iPlug2 editors, a Core Graphics
     * bitmap context for the older ones. Gating on Metal alone reported the
     * editor as refused for a plugin that had in fact just drawn a full frame.
     *
     * Give it several frames rather than one: an editor that loads its artwork
     * on the first idle has not necessarily painted by the time that idle
     * returns. */
    for (k = 0; k < 8; k++) {
        const unsigned int *px; int pw = 0, ph = 0;
        macvst_editor_pump(h);
        if (macmetal_pixels(&px, &pw, &ph) || macquartz_editor_pixels(&px, &pw, &ph))
            return 0;
    }
    /* Close it properly on the way out -- leaving the plugin's editor open
     * after reporting failure left it drawing for a host that had given up. */
    h->fx->dispatcher(h->fx, effEditClose, 0, 0, NULL, 0.0f);
    h->editor_open = 0;
    return -1;
}

void macvst_editor_close(macvst *h)
{
    if (!h || !h->fx || !h->editor_open) return;
    h->fx->dispatcher(h->fx, effEditClose, 0, 0, NULL, 0.0f);
    h->editor_open = 0;
}

/* A native Linux VST2 versus a macOS one. Both share this driver -- same System V
 * ABI, same AEffect -- but their editors could not be more different: the macOS
 * one builds an NSView and renders into a layer this side owns, while a Linux one
 * is an X11 client that needs a window of ours to become a child of. */
int macvst_is_native(macvst *h) { return h && h->dl && !h->img; }

/* Graft a native Linux editor into an X11 window.
 *
 * effEditOpen's `ptr` is the parent window: an NSView on macOS, an X11 Window id
 * on Linux, an HWND on Windows. Passing NULL is legitimate on macOS -- the plugin
 * makes its own view and we read its layer -- but on Linux it tells a toolkit
 * there is no parent, so it creates a top-level window of its own. That is what
 * put Dexed's editor in a separate window instead of inside the host's. */
int macvst_editor_attach(macvst *h, unsigned long xid)
{
    if (!h || !h->fx || !macvst_editor_kind(h)) return -1;
    if (h->editor_open) return 0;
    /* Asked before opening as well as after: a plugin may size its window from
     * the rect it reports, and some only answer once the editor exists. */
    { int w = 0, hh = 0; macvst_editor_size(h, &w, &hh); }
    if (!h->fx->dispatcher(h->fx, effEditOpen, 0, 0, (void *)(uintptr_t)xid, 0.0f)
        && !(h->fx->flags & 1))
        return -1;
    h->fx->dispatcher(h->fx, effEditTop, 0, 0, NULL, 0.0f);
    h->editor_open = 1;
    return 0;
}

void macvst_editor_pump(macvst *h)
{
    if (!h || !h->fx || !h->editor_open) return;
    h->fx->dispatcher(h->fx, effEditIdle, 0, 0, NULL, 0.0f);
    /* The Cocoa shim's timers and redraw belong to a Mach-O plugin. A native
     * Linux editor runs its own event handling against the real X server, and
     * only wants the idle call. */
    if (macvst_is_native(h)) return;
    macns_fire_timers();
    macns_draw_dirty();
}

int macvst_editor_pixels(macvst *h, const unsigned int **px, int *w, int *hh)
{
    if (!h || !h->editor_open) return 0;
    if (macmetal_pixels(px, w, hh)) return 1;
    /* No Metal layer: an older editor draws through Core Graphics, and its
     * bitmap context is the framebuffer. */
    return macquartz_editor_pixels(px, w, hh);
}

/* Mouse and keys reach the editor as Cocoa events would. The plugin's view is
 * one of its own classes, so these are messages to it. */
/* Post the event; do not paint.
 *
 * Painting here meant one full repaint per mouse event, and a mouse reports
 * far faster than an editor can be drawn -- a pointer moving across Qyooo's
 * editor asked for a 31 ms frame every few milliseconds, so the queue backed
 * up and the dial arrived in lurches. The Win32 path never did this: it posts
 * WM_MOUSEMOVE and lets the pump turn the invalid region into a WM_PAINT, so
 * however many events arrive between frames, one frame is drawn. This is that,
 * for Cocoa: the event marks the view dirty and macvst_editor_pump draws it,
 * once, when the host asks for the next frame. */
void macvst_editor_mouse(macvst *h, int x, int y, int msg, int buttons, int wheel)
{
    if (!h || !h->editor_open) return;
    macns_post_mouse(x, y, msg, buttons, wheel);
}

void macvst_editor_key(macvst *h, int vk, int down, int ch)
{
    if (!h || !h->editor_open) return;
    macns_post_key(vk, down, ch);
}
