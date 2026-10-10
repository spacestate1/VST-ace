/* OpenGL for Windows plug-ins: opengl32's WGL, and gdi32's pixel formats and
 * SwapBuffers, on the host's own GL through EGL.
 *
 * An editor built on OpenGL -- JUCE's OpenGLContext, which is how Helm draws
 * everything it shows -- has no GDI path to fall back to. With opengl32
 * answering from generic stubs it got a context handle that was nothing,
 * drew into it, and showed a black window.
 *
 * Each window a plug-in renders to gets an off-screen EGL pbuffer of the
 * window's size, so its framebuffer 0 is a real one and nothing about the
 * plug-in's drawing needs translating. SwapBuffers reads the frame back and
 * hands it to the window's surface, where it is composited like any GDI
 * paint. The display is Mesa's surfaceless platform where there is one --
 * which needs no X server or compositor, so it works the same in the bridge
 * helper and headless -- then an EGL device, then the default display.
 *
 * Every GL call goes through gl_thunks.h, a wrapper per entry point that
 * moves the arguments from the Windows convention to the host's.
 *
 * libEGL is opened at run time. Without it, or without a display that gives
 * a context, wglCreateContext fails the way it does on a machine with no GL
 * driver, and opengl32 stays among the DLLs LoadLibrary reports absent. */
#ifndef PELOAD_GL_SHIM_H
#define PELOAD_GL_SHIM_H

#include <dlfcn.h>
#include "gl_thunks.h"

#define W32_HGLRC_BASE 0x00770000u
#define WGL_MAX_CTX    32

/* The EGL this needs, spelled out so the build needs no EGL headers. */
typedef void *wgl_egl_dpy, *wgl_egl_cfg, *wgl_egl_ctx, *wgl_egl_surf;
#define EGL_NONE_                      0x3038
#define EGL_SURFACE_TYPE_              0x3033
#define EGL_PBUFFER_BIT_               0x0001
#define EGL_RENDERABLE_TYPE_           0x3040
#define EGL_OPENGL_BIT_                0x0008
#define EGL_RED_SIZE_                  0x3024
#define EGL_GREEN_SIZE_                0x3023
#define EGL_BLUE_SIZE_                 0x3022
#define EGL_ALPHA_SIZE_                0x3021
#define EGL_DEPTH_SIZE_                0x3025
#define EGL_STENCIL_SIZE_              0x3026
#define EGL_WIDTH_                     0x3057
#define EGL_HEIGHT_                    0x3056
#define EGL_OPENGL_API_                0x30A2
#define EGL_CONTEXT_MAJOR_VERSION_     0x3098
#define EGL_CONTEXT_MINOR_VERSION_     0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK_ 0x30FD
#define EGL_CONTEXT_OPENGL_DEBUG_      0x31B0
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_ 0x31B1
#define EGL_PLATFORM_SURFACELESS_MESA_ 0x31DD
#define EGL_PLATFORM_DEVICE_EXT_       0x313F

static struct {
    int tried, ok;
    void *lib;
    wgl_egl_dpy dpy;
    wgl_egl_cfg cfg;
    void *(*GetProcAddress)(const char *);
    void *(*GetDisplay)(void *);
    void *(*GetPlatformDisplay)(uint32_t, void *, const intptr_t *);
    void *(*GetPlatformDisplayEXT)(uint32_t, void *, const int32_t *);
    uint32_t (*QueryDevicesEXT)(int32_t, void **, int32_t *);
    uint32_t (*Initialize)(void *, int32_t *, int32_t *);
    uint32_t (*BindAPI)(uint32_t);
    uint32_t (*ChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *);
    void *(*CreateContext)(void *, void *, void *, const int32_t *);
    uint32_t (*DestroyContext)(void *, void *);
    void *(*CreatePbufferSurface)(void *, void *, const int32_t *);
    uint32_t (*DestroySurface)(void *, void *);
    uint32_t (*MakeCurrent)(void *, void *, void *, void *);
    int32_t (*GetError)(void);
} E;

static pthread_mutex_t g_wgl_lock = PTHREAD_MUTEX_INITIALIZER;

/* A rendering context. `owner` is the thread it is current on, if any:
 * wglShareLists can only rebuild a context nobody is using. */
typedef struct {
    int used;
    wgl_egl_ctx ctx;
    int major, minor, profile, flags;
    int share;                     /* index of the context it shares with, or 0 */
    int current;                   /* current on some thread */
} wgl_ctx;

/* A window's off-screen framebuffer, and the last frame read back from it,
 * waiting for the UI thread to copy it into the window's surface. */
typedef struct {
    wgl_egl_surf surf;
    int sw, sh;                    /* the pbuffer's size */
    uint32_t *frame;               /* the last swapped frame, top row first */
    int fw, fh, fresh;
    int pixfmt;
} wgl_win;

static wgl_ctx g_wglctx[WGL_MAX_CTX];
static wgl_win g_wglwin[W32_MAX_WND];
static wgl_egl_surf g_wgl_tiny;    /* for a context made current on a memory DC */
static int g_wgl_interval = 1;
static __thread int g_wgl_cur;     /* this thread's context, or 0 */
static __thread void *g_wgl_curdc;
static __thread int g_wgl_curwnd;
static __thread double g_wgl_last_swap;

static void *wgl_sym(const char *n)
{
    void *p = dlsym(E.lib, n);
    if (!p && E.GetProcAddress) p = E.GetProcAddress(n);
    return p;
}

static int wgl_choose(void)
{
    /* RGBA8, depth and stencil, which is what every plug-in asks for; then
     * less, for a driver that cannot do all of it in a pbuffer. */
    static const int32_t want[][17] = {
        { EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_, EGL_RENDERABLE_TYPE_, EGL_OPENGL_BIT_,
          EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8, EGL_ALPHA_SIZE_, 8,
          EGL_DEPTH_SIZE_, 24, EGL_STENCIL_SIZE_, 8, EGL_NONE_ },
        { EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_, EGL_RENDERABLE_TYPE_, EGL_OPENGL_BIT_,
          EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8,
          EGL_DEPTH_SIZE_, 16, EGL_NONE_ },
    };
    size_t k;
    for (k = 0; k < sizeof want / sizeof *want; k++) {
        int32_t n = 0;
        if (E.ChooseConfig(E.dpy, want[k], &E.cfg, 1, &n) && n > 0) return 1;
    }
    return 0;
}

static int wgl_try_display(void *dpy)
{
    int32_t ma, mi;
    if (!dpy || !E.Initialize(dpy, &ma, &mi)) return 0;
    E.dpy = dpy;
    if (!E.BindAPI(EGL_OPENGL_API_) || !wgl_choose()) { E.dpy = NULL; return 0; }
    return 1;
}

static int wgl_init(void)
{
    size_t i;
    pthread_mutex_lock(&g_wgl_lock);
    if (E.tried) { pthread_mutex_unlock(&g_wgl_lock); return E.ok; }
    E.tried = 1;
    E.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!E.lib) {
        fprintf(stderr, "  [gl] no libEGL.so.1: OpenGL editors will not draw\n");
        pthread_mutex_unlock(&g_wgl_lock);
        return 0;
    }
    E.GetProcAddress = (void *(*)(const char *))dlsym(E.lib, "eglGetProcAddress");
    *(void **)&E.GetDisplay           = wgl_sym("eglGetDisplay");
    *(void **)&E.GetPlatformDisplay   = wgl_sym("eglGetPlatformDisplay");
    *(void **)&E.GetPlatformDisplayEXT = wgl_sym("eglGetPlatformDisplayEXT");
    *(void **)&E.QueryDevicesEXT      = wgl_sym("eglQueryDevicesEXT");
    *(void **)&E.Initialize           = wgl_sym("eglInitialize");
    *(void **)&E.BindAPI              = wgl_sym("eglBindAPI");
    *(void **)&E.ChooseConfig         = wgl_sym("eglChooseConfig");
    *(void **)&E.CreateContext        = wgl_sym("eglCreateContext");
    *(void **)&E.DestroyContext       = wgl_sym("eglDestroyContext");
    *(void **)&E.CreatePbufferSurface = wgl_sym("eglCreatePbufferSurface");
    *(void **)&E.DestroySurface       = wgl_sym("eglDestroySurface");
    *(void **)&E.MakeCurrent          = wgl_sym("eglMakeCurrent");
    *(void **)&E.GetError             = wgl_sym("eglGetError");
    if (!E.GetProcAddress || !E.Initialize || !E.BindAPI || !E.ChooseConfig ||
        !E.CreateContext || !E.CreatePbufferSurface || !E.MakeCurrent) {
        fprintf(stderr, "  [gl] libEGL is missing core entry points\n");
        pthread_mutex_unlock(&g_wgl_lock);
        return 0;
    }
    if (E.GetPlatformDisplayEXT &&
        wgl_try_display(E.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA_, NULL, NULL))) {
    } else if (E.GetPlatformDisplay &&
               wgl_try_display(E.GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA_, NULL, NULL))) {
    } else {
        void *dev[4];
        int32_t nd = 0, k, done = 0;
        if (E.QueryDevicesEXT && E.GetPlatformDisplayEXT && E.QueryDevicesEXT(4, dev, &nd))
            for (k = 0; k < nd && !done; k++)
                done = wgl_try_display(E.GetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT_, dev[k], NULL));
        if (!done && E.GetDisplay) done = wgl_try_display(E.GetDisplay(NULL));
        if (!done) {
            fprintf(stderr, "  [gl] no EGL display gives an OpenGL pbuffer config\n");
            pthread_mutex_unlock(&g_wgl_lock);
            return 0;
        }
    }
    for (i = 0; i < GLT_N; i++) glt_fn[i] = E.GetProcAddress(glt_tab[i].name);
    {
        static const int32_t pa[] = { EGL_WIDTH_, 1, EGL_HEIGHT_, 1, EGL_NONE_ };
        g_wgl_tiny = E.CreatePbufferSurface(E.dpy, E.cfg, pa);
    }
    E.ok = 1;
    pthread_mutex_unlock(&g_wgl_lock);
    return 1;
}

static wgl_ctx *wgl_ctxget(const void *h)
{
    int i = w32_i(W32_HGLRC_BASE, h);
    return (i > 0 && i < WGL_MAX_CTX && g_wglctx[i].used) ? &g_wglctx[i] : NULL;
}

static wgl_egl_ctx wgl_egl_create(int major, int minor, int profile, int flags, wgl_egl_ctx share)
{
    int32_t a[16];
    int n = 0;
    wgl_egl_ctx c;
    if (major > 0) {
        a[n++] = EGL_CONTEXT_MAJOR_VERSION_; a[n++] = major;
        a[n++] = EGL_CONTEXT_MINOR_VERSION_; a[n++] = minor;
    }
    /* WGL's profile bits are EGL's: core 1, compatibility 2. Unasked for, a
     * context is compatibility, as wglCreateContext's is on Windows. */
    a[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_; a[n++] = profile ? profile : 2;
    if (flags & 1) { a[n++] = EGL_CONTEXT_OPENGL_DEBUG_; a[n++] = 1; }
    if (flags & 2) { a[n++] = EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_; a[n++] = 1; }
    a[n] = EGL_NONE_;
    E.BindAPI(EGL_OPENGL_API_);
    c = E.CreateContext(E.dpy, E.cfg, share, a);
    if (!c && major > 0 && profile != 1) {
        /* A version the driver will not name exactly: any compatibility
         * context is at least as new as what a plug-in asks for. */
        int32_t b[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK_, 2, EGL_NONE_ };
        c = E.CreateContext(E.dpy, E.cfg, share, b);
    }
    return c;
}

static void *wgl_create(int major, int minor, int profile, int flags, void *share)
{
    int i;
    wgl_ctx *s = share ? wgl_ctxget(share) : NULL;
    if (!wgl_init()) return NULL;
    pthread_mutex_lock(&g_wgl_lock);
    for (i = 1; i < WGL_MAX_CTX; i++) if (!g_wglctx[i].used) break;
    if (i >= WGL_MAX_CTX) { pthread_mutex_unlock(&g_wgl_lock); return NULL; }
    g_wglctx[i].ctx = wgl_egl_create(major, minor, profile, flags, s ? s->ctx : NULL);
    if (!g_wglctx[i].ctx) {
        pthread_mutex_unlock(&g_wgl_lock);
        fprintf(stderr, "  [gl] eglCreateContext %d.%d failed (0x%x)\n", major, minor,
                E.GetError ? (unsigned)E.GetError() : 0u);
        return NULL;
    }
    g_wglctx[i].used = 1;
    g_wglctx[i].major = major; g_wglctx[i].minor = minor;
    g_wglctx[i].profile = profile; g_wglctx[i].flags = flags;
    g_wglctx[i].share = s ? (int)(s - g_wglctx) : 0;
    g_wglctx[i].current = 0;
    pthread_mutex_unlock(&g_wgl_lock);
    PLOG("  [gl] context #%d %d.%d profile %d%s\n", i, major, minor, profile, s ? " (shared)" : "");
    return w32_h(W32_HGLRC_BASE, i);
}

static MS void *st_wglCreateContext(void *hdc)
{ (void)hdc; return wgl_create(0, 0, 0, 0, NULL); }
static MS void *st_wglCreateLayerContext(void *hdc, int32_t layer)
{ (void)layer; return st_wglCreateContext(hdc); }

static MS int32_t st_wglDeleteContext(void *h)
{
    wgl_ctx *c = wgl_ctxget(h);
    if (!c) return 0;
    if (g_wgl_cur == (int)(c - g_wglctx)) {
        E.MakeCurrent(E.dpy, NULL, NULL, NULL);
        g_wgl_cur = 0; g_wgl_curdc = NULL; g_wgl_curwnd = 0;
    }
    pthread_mutex_lock(&g_wgl_lock);
    if (E.DestroyContext) E.DestroyContext(E.dpy, c->ctx);   /* deferred while current elsewhere */
    memset(c, 0, sizeof *c);
    pthread_mutex_unlock(&g_wgl_lock);
    return 1;
}

/* EGL fixes a context's sharing when it is created; WGL lets a plug-in ask
 * afterwards, as long as the second context has no objects yet. That is the
 * moment JUCE asks, so the second context is simply made again, sharing. */
static MS int32_t st_wglShareLists(void *a, void *b)
{
    wgl_ctx *s = wgl_ctxget(a), *d = wgl_ctxget(b);
    wgl_egl_ctx n;
    if (!s || !d || s == d) return 0;
    pthread_mutex_lock(&g_wgl_lock);
    if (d->current) { pthread_mutex_unlock(&g_wgl_lock); return 0; }
    n = wgl_egl_create(d->major, d->minor, d->profile, d->flags, s->ctx);
    if (!n) { pthread_mutex_unlock(&g_wgl_lock); return 0; }
    if (E.DestroyContext) E.DestroyContext(E.dpy, d->ctx);
    d->ctx = n;
    d->share = (int)(s - g_wglctx);
    pthread_mutex_unlock(&g_wgl_lock);
    return 1;
}

static MS int32_t st_wglCopyContext(void *a, void *b, uint32_t mask)
{ (void)a; (void)b; (void)mask; return 0; }

/* The pbuffer for a window, at the window's present size. A window that has
 * been resized gets a new one; the old one goes when nothing is using it. */
static wgl_egl_surf wgl_surface_for(int wnd)
{
    wgl_win *g;
    int w, h;
    if (wnd <= 0 || wnd >= W32_MAX_WND || !W.wnd[wnd].used) return g_wgl_tiny;
    g = &g_wglwin[wnd];
    w = W.wnd[wnd].w > 0 ? W.wnd[wnd].w : 1;
    h = W.wnd[wnd].h > 0 ? W.wnd[wnd].h : 1;
    if (g->surf && g->sw == w && g->sh == h) return g->surf;
    {
        int32_t pa[] = { EGL_WIDTH_, w, EGL_HEIGHT_, h, EGL_NONE_ };
        wgl_egl_surf s = E.CreatePbufferSurface(E.dpy, E.cfg, pa);
        if (!s) return g->surf ? g->surf : g_wgl_tiny;
        if (g->surf && E.DestroySurface) E.DestroySurface(E.dpy, g->surf);
        g->surf = s; g->sw = w; g->sh = h;
        PLOG("  [gl] window #%d framebuffer %dx%d\n", wnd, w, h);
    }
    return g->surf;
}

static MS int32_t st_wglMakeCurrent(void *hdc, void *h)
{
    wgl_ctx *c;
    w32_dc *d;
    wgl_egl_surf s;
    int wnd;
    if (!E.ok) return h ? 0 : 1;
    if (!h) {
        if (g_wgl_cur) {
            pthread_mutex_lock(&g_wgl_lock);
            g_wglctx[g_wgl_cur].current = 0;
            pthread_mutex_unlock(&g_wgl_lock);
        }
        E.MakeCurrent(E.dpy, NULL, NULL, NULL);
        g_wgl_cur = 0; g_wgl_curdc = NULL; g_wgl_curwnd = 0;
        return 1;
    }
    c = wgl_ctxget(h);
    d = w32_dcget(hdc);
    if (!c) return 0;
    wnd = d ? d->wnd : 0;
    pthread_mutex_lock(&g_wgl_lock);
    s = wgl_surface_for(wnd);
    E.BindAPI(EGL_OPENGL_API_);
    if (!E.MakeCurrent(E.dpy, s, s, c->ctx)) {
        pthread_mutex_unlock(&g_wgl_lock);
        PLOG("  [gl] eglMakeCurrent failed (0x%x)\n", E.GetError ? (unsigned)E.GetError() : 0u);
        return 0;
    }
    if (g_wgl_cur && g_wgl_cur != (int)(c - g_wglctx)) g_wglctx[g_wgl_cur].current = 0;
    c->current = 1;
    pthread_mutex_unlock(&g_wgl_lock);
    g_wgl_cur = (int)(c - g_wglctx);
    g_wgl_curdc = hdc;
    g_wgl_curwnd = wnd;
    return 1;
}
static MS int32_t st_wglMakeContextCurrentARB(void *draw, void *read, void *h)
{ (void)read; return st_wglMakeCurrent(draw, h); }

static MS void *st_wglGetCurrentContext(void)
{ return g_wgl_cur ? w32_h(W32_HGLRC_BASE, g_wgl_cur) : NULL; }
static MS void *st_wglGetCurrentDC(void) { return g_wgl_cur ? g_wgl_curdc : NULL; }

/* GL constants SwapBuffers needs to read the frame back without disturbing
 * the plug-in's own state. */
#define WGL_GL_READ_FRAMEBUFFER          0x8CA8
#define WGL_GL_READ_FRAMEBUFFER_BINDING  0x8CAA
#define WGL_GL_PIXEL_PACK_BUFFER         0x88EB
#define WGL_GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define WGL_GL_PACK_ALIGNMENT            0x0D05
#define WGL_GL_PACK_ROW_LENGTH           0x0D02
#define WGL_GL_PACK_SKIP_ROWS            0x0D03
#define WGL_GL_PACK_SKIP_PIXELS          0x0D04
#define WGL_GL_READ_BUFFER               0x0C02
#define WGL_GL_BACK                      0x0405
#define WGL_GL_BGRA                      0x80E1
#define WGL_GL_UNSIGNED_BYTE             0x1401

static void *wgl_gl(const char *n)
{
    /* glt_tab is in strcmp order: the generator sorts it. */
    int lo = 0, hi = GLT_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(n, glt_tab[mid].name);
        if (!c) return glt_fn[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
}

/* The frame the plug-in just finished, read out of its pbuffer. The copy into
 * the window's surface happens on the UI thread (wgl_flush_frames, from
 * w32_pump): JUCE swaps from its own render thread, and the surface belongs
 * to the thread that resizes and composites it. */
static void wgl_read_frame(int wnd)
{
    static void (*GetIntegerv)(uint32_t, int32_t *);
    static void (*BindFramebuffer)(uint32_t, uint32_t);
    static void (*BindBuffer)(uint32_t, uint32_t);
    static void (*PixelStorei)(uint32_t, int32_t);
    static void (*ReadBuffer)(uint32_t);
    static void (*ReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);
    int32_t fb = 0, pbo = 0, al = 4, rl = 0, sr = 0, sp = 0, rb = WGL_GL_BACK;
    wgl_win *g = &g_wglwin[wnd];
    int w = g->sw, h = g->sh, y;
    uint32_t *tmp, *dst;

    if (!ReadPixels) {
        *(void **)&GetIntegerv     = wgl_gl("glGetIntegerv");
        *(void **)&BindFramebuffer = wgl_gl("glBindFramebuffer");
        *(void **)&BindBuffer      = wgl_gl("glBindBuffer");
        *(void **)&PixelStorei     = wgl_gl("glPixelStorei");
        *(void **)&ReadBuffer      = wgl_gl("glReadBuffer");
        *(void **)&ReadPixels      = wgl_gl("glReadPixels");
    }
    if (!ReadPixels || !GetIntegerv || w <= 0 || h <= 0) return;
    tmp = malloc((size_t)w * h * 4);
    if (!tmp) return;
    GetIntegerv(WGL_GL_READ_FRAMEBUFFER_BINDING, &fb);
    GetIntegerv(WGL_GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    GetIntegerv(WGL_GL_PACK_ALIGNMENT, &al);
    GetIntegerv(WGL_GL_PACK_ROW_LENGTH, &rl);
    GetIntegerv(WGL_GL_PACK_SKIP_ROWS, &sr);
    GetIntegerv(WGL_GL_PACK_SKIP_PIXELS, &sp);
    GetIntegerv(WGL_GL_READ_BUFFER, &rb);
    if (BindFramebuffer) BindFramebuffer(WGL_GL_READ_FRAMEBUFFER, 0);
    if (BindBuffer) BindBuffer(WGL_GL_PIXEL_PACK_BUFFER, 0);
    PixelStorei(WGL_GL_PACK_ALIGNMENT, 4);
    PixelStorei(WGL_GL_PACK_ROW_LENGTH, 0);
    PixelStorei(WGL_GL_PACK_SKIP_ROWS, 0);
    PixelStorei(WGL_GL_PACK_SKIP_PIXELS, 0);
    if (ReadBuffer) ReadBuffer(WGL_GL_BACK);
    ReadPixels(0, 0, w, h, WGL_GL_BGRA, WGL_GL_UNSIGNED_BYTE, tmp);
    if (ReadBuffer) ReadBuffer((uint32_t)rb);
    PixelStorei(WGL_GL_PACK_ALIGNMENT, al);
    PixelStorei(WGL_GL_PACK_ROW_LENGTH, rl);
    PixelStorei(WGL_GL_PACK_SKIP_ROWS, sr);
    PixelStorei(WGL_GL_PACK_SKIP_PIXELS, sp);
    if (BindBuffer) BindBuffer(WGL_GL_PIXEL_PACK_BUFFER, (uint32_t)pbo);
    if (BindFramebuffer) BindFramebuffer(WGL_GL_READ_FRAMEBUFFER, (uint32_t)fb);

    pthread_mutex_lock(&g_wgl_lock);
    if (g->fw != w || g->fh != h || !g->frame) {
        free(g->frame);
        g->frame = malloc((size_t)w * h * 4);
        g->fw = g->frame ? w : 0;
        g->fh = g->frame ? h : 0;
    }
    dst = g->frame;
    if (dst)                       /* GL's rows run bottom up, a DIB's top down */
        for (y = 0; y < h; y++) {
            const uint32_t *s = tmp + (size_t)(h - 1 - y) * w;
            uint32_t *o = dst + (size_t)y * w;
            int x;
            for (x = 0; x < w; x++) o[x] = s[x] & 0x00FFFFFFu;
        }
    g->fresh = dst != NULL;
    pthread_mutex_unlock(&g_wgl_lock);
    free(tmp);
}

/* Called from w32_pump on the UI thread. */
static void wgl_flush_frames(void)
{
    int i;
    if (!E.ok) return;
    pthread_mutex_lock(&g_wgl_lock);
    for (i = 1; i < W32_MAX_WND; i++) {
        wgl_win *g = &g_wglwin[i];
        w32_surf *s;
        int y, cw, ch;
        if (!g->fresh || !g->frame) continue;
        g->fresh = 0;
        if (!W.wnd[i].used) continue;
        s = &W.wnd[i].surf;
        w32_surf_size(s, W.wnd[i].w, W.wnd[i].h);
        if (!s->px) continue;
        cw = g->fw < s->w ? g->fw : s->w;
        ch = g->fh < s->h ? g->fh : s->h;
        for (y = 0; y < ch; y++)
            memcpy(s->px + (size_t)y * s->w, g->frame + (size_t)y * g->fw, (size_t)cw * 4);
    }
    pthread_mutex_unlock(&g_wgl_lock);
}

/* With a swap interval of one, Windows holds SwapBuffers until the next
 * vertical blank. A render thread that loops on it relies on that to run at
 * the display's rate; without it, it ran flat out and took a core. */
static void wgl_pace(void)
{
    struct timespec t;
    double now, next;
    if (g_wgl_interval <= 0) return;
    clock_gettime(CLOCK_MONOTONIC, &t);
    now = (double)t.tv_sec + t.tv_nsec / 1e9;
    next = g_wgl_last_swap + g_wgl_interval / 60.0;
    if (next > now && next - now < 0.5) {
        double d = next - now;
        struct timespec s;
        s.tv_sec = (time_t)d;
        s.tv_nsec = (long)((d - (double)s.tv_sec) * 1e9);
        nanosleep(&s, NULL);
        g_wgl_last_swap = next;
    } else {
        g_wgl_last_swap = now;
    }
}

static MS int32_t st_SwapBuffers(void *hdc)
{
    w32_dc *d = w32_dcget(hdc);
    int wnd = d ? d->wnd : 0;
    if (!E.ok || !g_wgl_cur) return 0;
    if (wnd > 0 && wnd == g_wgl_curwnd && g_wglwin[wnd].surf) {
        static void (*Flush)(void);
        if (!Flush) *(void **)&Flush = wgl_gl("glFlush");
        wgl_read_frame(wnd);
        if (Flush) Flush();
    }
    wgl_pace();
    return 1;
}
static MS int32_t st_wglSwapBuffers(void *hdc) { return st_SwapBuffers(hdc); }
static MS int32_t st_wglSwapLayerBuffers(void *hdc, uint32_t planes)
{ (void)planes; return st_SwapBuffers(hdc); }

static MS int32_t st_wglSwapIntervalEXT(int32_t n) { g_wgl_interval = n < 0 ? 0 : n; return 1; }
static MS int32_t st_wglGetSwapIntervalEXT(void) { return g_wgl_interval; }

/* ---- pixel formats ------------------------------------------------------- */

/* There is one: 32-bit RGBA, double buffered, 24-bit depth and 8-bit stencil,
 * drawn by a full driver. It is what the pbuffer config is, near enough, and
 * it is what every plug-in asks for. */
static void wgl_describe(uint8_t *p)
{
    memset(p, 0, 40);
    *(uint16_t *)(p + 0) = 40;
    *(uint16_t *)(p + 2) = 1;
    *(uint32_t *)(p + 4) = 0x4 | 0x20 | 0x1;     /* DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLEBUFFER */
    p[8] = 0;                                     /* PFD_TYPE_RGBA */
    p[9] = 32;
    p[10] = 8; p[11] = 16; p[12] = 8; p[13] = 8; p[14] = 8; p[15] = 0;
    p[16] = 8; p[17] = 24;
    p[23] = 24; p[24] = 8;
}

static MS int32_t st_ChoosePixelFormat(void *hdc, const void *pfd)
{ (void)hdc; (void)pfd; return wgl_init() ? 1 : 0; }

static MS int32_t st_DescribePixelFormat(void *hdc, int32_t fmt, uint32_t n, void *pfd)
{
    (void)hdc;
    if (!wgl_init()) return 0;
    if (pfd && n >= 40 && fmt == 1) wgl_describe(pfd);
    return 1;                                     /* the number of formats */
}
static MS int32_t st_SetPixelFormat(void *hdc, int32_t fmt, const void *pfd)
{
    w32_dc *d = w32_dcget(hdc);
    (void)pfd;
    if (fmt != 1 || !wgl_init()) return 0;
    if (d && d->wnd > 0 && d->wnd < W32_MAX_WND) g_wglwin[d->wnd].pixfmt = fmt;
    return 1;
}
static MS int32_t st_GetPixelFormat(void *hdc)
{
    w32_dc *d = w32_dcget(hdc);
    return (d && d->wnd > 0 && d->wnd < W32_MAX_WND) ? g_wglwin[d->wnd].pixfmt : 0;
}
static MS int32_t st_wglChoosePixelFormat(void *hdc, const void *pfd) { return st_ChoosePixelFormat(hdc, pfd); }
static MS int32_t st_wglDescribePixelFormat(void *hdc, int32_t f, uint32_t n, void *p)
{ return st_DescribePixelFormat(hdc, f, n, p); }
static MS int32_t st_wglSetPixelFormat(void *hdc, int32_t f, const void *p) { return st_SetPixelFormat(hdc, f, p); }
static MS int32_t st_wglGetPixelFormat(void *hdc) { return st_GetPixelFormat(hdc); }

/* WGL_ARB_pixel_format. Every request is answered with the one format; the
 * attributes report what it is. */
static int32_t wgl_format_attrib(int32_t a)
{
    switch (a) {
    case 0x2000: return 1;            /* NUMBER_PIXEL_FORMATS */
    case 0x2001: case 0x2010: case 0x2011: return 1;   /* DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLE_BUFFER */
    case 0x2003: return 0x2027;       /* ACCELERATION: FULL */
    case 0x2013: return 0x202B;       /* PIXEL_TYPE: RGBA */
    case 0x2014: return 32;           /* COLOR_BITS */
    case 0x2015: case 0x2017: case 0x2019: case 0x201B: return 8;
    case 0x2016: return 16;           /* RED_SHIFT */
    case 0x2018: return 8;            /* GREEN_SHIFT */
    case 0x201C: return 24;           /* ALPHA_SHIFT */
    case 0x2022: return 24;           /* DEPTH_BITS */
    case 0x2023: return 8;            /* STENCIL_BITS */
    case 0x2007: return 0x2028;       /* SWAP_METHOD: EXCHANGE */
    default:     return 0;            /* stereo, accumulation, samples ... */
    }
}
static MS int32_t st_wglChoosePixelFormatARB(void *hdc, const int32_t *ia, const float *fa,
                                             uint32_t max, int32_t *fmts, uint32_t *n)
{
    (void)hdc; (void)ia; (void)fa;
    if (!wgl_init()) { if (n) *n = 0; return 0; }
    if (fmts && max > 0) fmts[0] = 1;
    if (n) *n = max > 0 ? 1 : 0;
    return 1;
}
static MS int32_t st_wglGetPixelFormatAttribivARB(void *hdc, int32_t fmt, int32_t layer,
                                                  uint32_t n, const int32_t *at, int32_t *v)
{
    uint32_t i;
    (void)hdc; (void)layer;
    if (!at || !v || (fmt != 1 && !(n == 1 && at[0] == 0x2000))) return 0;
    for (i = 0; i < n; i++) v[i] = wgl_format_attrib(at[i]);
    return 1;
}
static MS int32_t st_wglGetPixelFormatAttribfvARB(void *hdc, int32_t fmt, int32_t layer,
                                                  uint32_t n, const int32_t *at, float *v)
{
    uint32_t i;
    (void)hdc; (void)layer;
    if (!at || !v || fmt != 1) return 0;
    for (i = 0; i < n; i++) v[i] = (float)wgl_format_attrib(at[i]);
    return 1;
}

/* WGL_ARB_create_context: the version and profile a modern renderer needs. */
static MS void *st_wglCreateContextAttribsARB(void *hdc, void *share, const int32_t *a)
{
    int major = 0, minor = 0, profile = 0, flags = 0;
    (void)hdc;
    for (; a && a[0]; a += 2) {
        switch (a[0]) {
        case 0x2091: major = a[1]; break;       /* MAJOR_VERSION */
        case 0x2092: minor = a[1]; break;       /* MINOR_VERSION */
        case 0x2094: flags = a[1]; break;       /* FLAGS */
        case 0x9126: profile = a[1]; break;     /* PROFILE_MASK */
        default: break;
        }
    }
    return wgl_create(major, minor, profile, flags, share);
}

static const char g_wgl_exts[] =
    "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_pixel_format "
    "WGL_ARB_create_context WGL_ARB_create_context_profile WGL_EXT_swap_control "
    "WGL_ARB_make_current_read";
static MS const char *st_wglGetExtensionsStringARB(void *hdc) { (void)hdc; return g_wgl_exts; }
static MS const char *st_wglGetExtensionsStringEXT(void) { return g_wgl_exts; }

static MS void *st_wglGetProcAddress(const char *name)
{
    static const struct { const char *n; void *f; } wgl[] = {
        { "wglCreateContextAttribsARB",  (void *)st_wglCreateContextAttribsARB },
        { "wglChoosePixelFormatARB",     (void *)st_wglChoosePixelFormatARB },
        { "wglGetPixelFormatAttribivARB", (void *)st_wglGetPixelFormatAttribivARB },
        { "wglGetPixelFormatAttribfvARB", (void *)st_wglGetPixelFormatAttribfvARB },
        { "wglGetExtensionsStringARB",   (void *)st_wglGetExtensionsStringARB },
        { "wglGetExtensionsStringEXT",   (void *)st_wglGetExtensionsStringEXT },
        { "wglSwapIntervalEXT",          (void *)st_wglSwapIntervalEXT },
        { "wglGetSwapIntervalEXT",       (void *)st_wglGetSwapIntervalEXT },
        { "wglMakeContextCurrentARB",    (void *)st_wglMakeContextCurrentARB },
        { "wglGetCurrentReadDCARB",      (void *)st_wglGetCurrentDC },
    };
    size_t i;
    int lo, hi;
    if (!name || (uintptr_t)name < 0x10000) return NULL;
    for (i = 0; i < sizeof wgl / sizeof *wgl; i++)
        if (!strcmp(name, wgl[i].n)) return wgl[i].f;
    /* Windows answers only with a context current, and only for what the
     * driver has: a NULL is how a plug-in finds a feature missing. */
    if (!g_wgl_cur) { PLOG("  [gl] wglGetProcAddress(%s) with no context\n", name); return NULL; }
    lo = 0; hi = GLT_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, glt_tab[mid].name);
        if (!c) return glt_fn[mid] ? glt_tab[mid].fn : NULL;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    PLOG("  [gl] wglGetProcAddress(%s): not a GL entry point\n", name);
    return NULL;
}

static MS int32_t st_wglUseFontBitmapsA(void *hdc, uint32_t first, uint32_t n, uint32_t base)
{ (void)hdc; (void)first; (void)n; (void)base; return 0; }
static MS int32_t st_wglUseFontBitmapsW(void *hdc, uint32_t first, uint32_t n, uint32_t base)
{ (void)hdc; (void)first; (void)n; (void)base; return 0; }

/* Whether OpenGL is really here, for LoadLibrary("opengl32"): a plug-in that
 * probes for it and finds it switches to it, so it is only admitted when a
 * context can actually be made. */
static int wgl_available(void) { return wgl_init(); }


#endif /* PELOAD_GL_SHIM_H */
