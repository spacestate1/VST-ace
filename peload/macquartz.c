/* CoreGraphics for the Mach-O loader.
 *
 * The bitmap context is real, because that is where the plugin's GUI is going to
 * end up: the Windows side already renders these same plugins by handing them a
 * pixel buffer and blitting it, and CGBitmapContext is the equivalent handle
 * here. Images, colours and data providers are real for the same reason -- they
 * carry the pixels around.
 *
 * The display and cursor calls are not: there is no display to hide a cursor on,
 * so they report a plausible screen and do nothing. Those are the ones where
 * doing nothing is correct rather than merely convenient.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "macshim.h"
#include "png_in.h"

typedef struct { double x, y; } CGPoint;
typedef struct { double width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;

/* Shared header so retain/release can be uniform. */
enum { Q_CONTEXT = 1, Q_IMAGE, Q_COLOR, Q_PROVIDER, Q_FONT, Q_COLORSPACE,
       Q_SOURCE, Q_GRADIENT };
typedef struct qobj { uint32_t magic, kind; long refs; struct qobj *reg_next; } qobj;
#define QMAGIC 0x51475458u          /* 'QGTX' */

/* ---- which pointers are ours ------------------------------------------
 *
 * Same reasoning as the CoreFoundation and Objective-C registries: "is this one
 * of mine?" is asked about pointers a plugin supplies, and reading a magic word
 * out of one that is not a pointer is a fault inside a question. It also gives
 * CFRelease something to dispatch on -- CGImageSourceRef has no typed release,
 * so a plugin frees one with CFRelease, and without this every image an editor
 * loaded stayed in memory for the life of the process. */
#define QREG_BUCKETS 1024
static qobj *g_qreg[QREG_BUCKETS];
static pthread_mutex_t g_qreg_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned qreg_bucket(const void *p)
{ return (unsigned)((((uintptr_t)p) >> 4) * 2654435761u) % QREG_BUCKETS; }

static int qreg_has(const void *p)
{
    qobj *o; int found = 0;
    if (!p) return 0;
    pthread_mutex_lock(&g_qreg_lock);
    for (o = g_qreg[qreg_bucket(p)]; o; o = o->reg_next)
        if (o == p) { found = 1; break; }
    pthread_mutex_unlock(&g_qreg_lock);
    return found;
}

static void *q_new(size_t sz, int kind)
{
    qobj *o = calloc(1, sz);
    unsigned b;
    if (!o) return NULL;
    o->magic = QMAGIC;
    o->kind = (uint32_t)kind;
    o->refs = 1;
    b = qreg_bucket(o);
    pthread_mutex_lock(&g_qreg_lock);
    o->reg_next = g_qreg[b];
    g_qreg[b] = o;
    pthread_mutex_unlock(&g_qreg_lock);
    return o;
}

/* Unregister and free. Every path that frees one of these objects goes through
 * here, or the registry would keep pointers into freed memory -- and then hand
 * one back to the next caller that asked about the same address. */
static void q_free(void *p)
{
    qobj *o = p, **pp;
    unsigned b;
    if (!o) return;
    b = qreg_bucket(o);
    pthread_mutex_lock(&g_qreg_lock);
    for (pp = &g_qreg[b]; *pp; pp = &(*pp)->reg_next)
        if (*pp == o) { *pp = o->reg_next; break; }
    pthread_mutex_unlock(&g_qreg_lock);
    o->magic = 0;
    free(o);
}

static qobj *q_of(const void *p, int kind)
{
    qobj *o = (qobj *)p;
    if (!qreg_has(p)) return NULL;
    return (o->magic == QMAGIC && (int)o->kind == kind) ? o : NULL;
}

/* What kind of object this is, or 0 -- CFRelease has to know before it can
 * release one. */
static int q_kind(const void *p)
{
    const qobj *o = p;
    return qreg_has(p) ? (int)o->kind : 0;
}

typedef struct {
    qobj o;
    uint8_t *px;                    /* caller's buffer, or ours */
    int owned;
    size_t w, h, stride, bpc, bpp;
    uint32_t bitmapinfo;
} qimage;

/* The graphics state a plugin actually leans on.
 *
 * This context had none: the transform and clip calls accepted their arguments
 * and returned, which is fine while the only thing drawing is a plugin that
 * composites into a bitmap of its own at the identity. A VSTGUI editor does not
 * work that way -- it clips to a control's rectangle, translates the origin to
 * that control, flips the y axis, and then draws the whole filmstrip -- so
 * ignoring the state meant every bitmap landed on top of every other one at the
 * canvas origin. The first editor to get this far drew three vertical bands of
 * stripes.
 *
 * Only translation and scaling are honoured, which is all VSTGUI and iPlug use;
 * a rotated CTM is kept in the matrix and drawn as its bounding box, because
 * refusing outright would lose more than it saved. */
typedef struct { double a, b, c, d, tx, ty; } qmat;
typedef struct { double x0, y0, x1, y1; } qclip;

typedef struct {
    qmat   ctm;
    qclip  clip;
    double fill[4], stroke[4], alpha;
    double line_width;
} qgstate;

/* A flattened path: subpaths of device-space points. Built by the CGContext
 * path calls and painted by geom_fill / geom_stroke -- see the path section. */
typedef struct qpt_s { double x, y; } qpt;
typedef struct qgeom_s {
    qpt           *pt;
    long          *sub;         /* first point index of each subpath */
    unsigned char *closed;
    long           np, npcap;
    long           ns, nscap;
} qgeom;
static void geom_free(qgeom *g);

typedef struct {
    qobj o;
    uint8_t *px;
    int owned;
    size_t w, h, stride;
    uint32_t bitmapinfo;
    double shadow[4];
    qgstate g;
    qgstate stack[32];
    int      depth;
    /* The path being built by CGContextMoveToPoint and friends, flattened to
     * device-space points as it goes. See the path section below. */
    qgeom    path;
    double   cur_x, cur_y;
    int      have_cur;
} qcontext;

static void qmat_identity(qmat *m)
{ m->a = 1; m->b = 0; m->c = 0; m->d = 1; m->tx = 0; m->ty = 0; }
static void qmat_apply(const qmat *m, double x, double y, double *ox, double *oy)
{ *ox = m->a * x + m->c * y + m->tx; *oy = m->b * x + m->d * y + m->ty; }
static void qmat_concat(qmat *m, const qmat *n)
{
    qmat r;
    r.a  = n->a * m->a + n->b * m->c;
    r.b  = n->a * m->b + n->b * m->d;
    r.c  = n->c * m->a + n->d * m->c;
    r.d  = n->c * m->b + n->d * m->d;
    r.tx = n->tx * m->a + n->ty * m->c + m->tx;
    r.ty = n->tx * m->b + n->ty * m->d + m->ty;
    *m = r;
}

typedef struct { qobj o; double c[4]; } qcolor;
typedef struct { qobj o; uint8_t *b; size_t n; int owned; } qprovider;
typedef struct { qobj o; qprovider *data; } qfont;

/* --------------------------------------------------------------- contexts */

/* The plugin hands us its own buffer, which is the point: whatever it draws is
 * then ours to present, exactly as the Win32 layer does. */
/* The context a Core Graphics editor draws into. See cg_bitmap_context_create. */
static qcontext *g_editor_ctx;

int macquartz_editor_pixels(const unsigned int **px, int *w, int *h)
{
    qcontext *c = g_editor_ctx;
    if (!c || !c->px || !c->w || !c->h) return 0;
    /* Only a tightly packed 32-bit context can be handed over as-is; anything
     * else would need converting and no plugin here produces one. */
    if (c->stride != c->w * 4) return 0;
    if (px) *px = (const unsigned int *)c->px;
    if (w)  *w  = (int)c->w;
    if (h)  *h  = (int)c->h;
    return 1;
}

/* ---- a context of the host's own -------------------------------------
 *
 * An editor that draws through drawRect: does not make a bitmap; AppKit hands
 * it one already focused on the view. That is this. Everything else in this
 * host is handed pixels by the plugin -- the Metal path renders into a layer,
 * and an older Core Graphics editor makes its own bitmap context and
 * composites into it -- so this is the one case where the host has to supply
 * the surface, and it is what makes a VSTGUI editor visible at all.
 *
 * Sticky, once made: a plugin's own scratch contexts must not displace the
 * framebuffer just for being larger. */
static qcontext *g_host_ctx;

void *macquartz_editor_context(int w, int h)
{
    qcontext *c;
    if (w <= 0 || h <= 0) return NULL;
    if (g_host_ctx && (int)g_host_ctx->w == w && (int)g_host_ctx->h == h)
        return g_host_ctx;
    if (!(c = q_new(sizeof *c, Q_CONTEXT))) return NULL;
    c->w = (size_t)w; c->h = (size_t)h; c->stride = (size_t)w * 4;
    qmat_identity(&c->g.ctm);
    c->g.clip.x0 = 0; c->g.clip.y0 = 0;
    c->g.clip.x1 = (double)w; c->g.clip.y1 = (double)h;
    c->g.alpha = 1.0;
    c->g.line_width = 1.0;
    if (!(c->px = calloc(1, c->stride * c->h))) { q_free(c); return NULL; }
    c->owned = 1;
    if (getenv("MACQZ_VERBOSE"))
        fprintf(stderr, "  [qz] host editor context %dx%d\n", w, h);
    g_host_ctx = c;
    g_editor_ctx = c;
    return c;
}

/* Put the editor's context into the state AppKit hands drawRect:.
 *
 * A *flipped* view draws with y counting down from the top, and AppKit gets
 * that by concatenating a flip onto the context before it calls drawRect: --
 * the bitmap underneath is still an ordinary Core Graphics one with its origin
 * at the bottom left. Handing the plugin an unflipped context instead does not
 * fail visibly: the editor draws, every control lands at `height - y`, and the
 * picture looks plausible until you click it and the control you hit is the one
 * mirrored opposite the pointer. Automaton invalidates its row of mode buttons
 * at y=293 and they were appearing at y=192.
 *
 * The rest of the state is reset for the same reason it is reset by AppKit: a
 * plugin that leaves a translate or a clip behind at the end of one drawRect:
 * would otherwise start the next one inside it, and the drift compounds. */
void macquartz_begin_draw(void *ctx, int flipped, double height)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    if (!c) return;
    c->depth = 0;
    if (flipped && height > 0.0) {
        c->g.ctm.a = 1; c->g.ctm.b = 0; c->g.ctm.c = 0;
        c->g.ctm.d = -1; c->g.ctm.tx = 0; c->g.ctm.ty = height;
    } else {
        qmat_identity(&c->g.ctm);
    }
    c->g.clip.x0 = 0; c->g.clip.y0 = 0;
    c->g.clip.x1 = (double)c->w; c->g.clip.y1 = (double)c->h;
    c->g.alpha = 1.0;
    c->g.line_width = 1.0;
}

static void *cg_bitmap_context_create(void *data, size_t w, size_t h,
                                      size_t bpc, size_t stride,
                                      void *space, uint32_t info)
{
    qcontext *c = q_new(sizeof *c, Q_CONTEXT);
    (void)space;
    if (!c) return NULL;
    if (!stride) stride = w * 4;
    c->w = w; c->h = h; c->stride = stride; c->bitmapinfo = info;
    qmat_identity(&c->g.ctm);
    c->g.clip.x0 = 0; c->g.clip.y0 = 0;
    c->g.clip.x1 = (double)w; c->g.clip.y1 = (double)h;
    c->g.alpha = 1.0;
    c->g.line_width = 1.0;
    /* Remember the biggest one: an editor that draws through Core Graphics rather
     * than Metal makes exactly one bitmap context the size of its window and
     * composites everything into it, so that context *is* the framebuffer. Any
     * others a plugin makes are scratch and smaller. */
    /* Never over the host's own -- see macquartz_editor_context. */
    if (!g_host_ctx && (!g_editor_ctx || (size_t)w * h > g_editor_ctx->w * g_editor_ctx->h))
        g_editor_ctx = c;
    if (getenv("MACQZ_VERBOSE"))
        fprintf(stderr, "  [qz] CGBitmapContextCreate %zux%zu stride=%zu data=%p\n",
                w, h, stride, data);
    if (data) {
        c->px = data;
    } else {
        c->px = calloc(1, stride * (h ? h : 1));
        c->owned = 1;
        if (!c->px) { q_free(c); return NULL; }
    }
    (void)bpc;
    return c;
}

static void *cg_bitmap_context_create_image(void *ctx)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qimage *im;
    if (!c) return NULL;
    if (!(im = q_new(sizeof *im, Q_IMAGE))) return NULL;
    im->w = c->w; im->h = c->h; im->stride = c->stride;
    im->bpc = 8; im->bpp = 32; im->bitmapinfo = c->bitmapinfo;
    /* A snapshot, not a view: the context keeps being drawn into. */
    im->px = malloc(c->stride * (c->h ? c->h : 1));
    if (im->px) {
        memcpy(im->px, c->px, c->stride * (c->h ? c->h : 1));
        im->owned = 1;
    }
    return im;
}

static void cg_context_release(void *p)
{
    qcontext *c = (qcontext *)q_of(p, Q_CONTEXT);
    if (!c || --c->o.refs > 0) return;
    if (c->owned) free(c->px);
    geom_free(&c->path);
    if (c == g_host_ctx) g_host_ctx = NULL;
    if (c == g_editor_ctx) g_editor_ctx = NULL;
    q_free(c);
}

/* Nearest-neighbour blit into the destination rect. Enough to composite an
 * image a plugin has prepared; a real resampler is a later concern. */
/* ---- the graphics state, as calls ------------------------------------ */

static void cg_ctm_translate(void *ctx, double tx, double ty)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qmat t;
    if (!c) return;
    t.a = 1; t.b = 0; t.c = 0; t.d = 1; t.tx = tx; t.ty = ty;
    qmat_concat(&c->g.ctm, &t);
}
static void cg_ctm_scale(void *ctx, double sx, double sy)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qmat t;
    if (!c) return;
    t.a = sx; t.b = 0; t.c = 0; t.d = sy; t.tx = 0; t.ty = 0;
    qmat_concat(&c->g.ctm, &t);
}
static void cg_ctm_concat(void *ctx, qmat m)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    if (c) qmat_concat(&c->g.ctm, &m);
}

/* A rect through the CTM, as the axis-aligned box that contains it. */
static void cg_map_rect(const qcontext *c, CGRect r, qclip *out)
{
    double x0, y0, x1, y1, ax, ay, bx, by;
    qmat_apply(&c->g.ctm, r.origin.x, r.origin.y, &ax, &ay);
    qmat_apply(&c->g.ctm, r.origin.x + r.size.width, r.origin.y + r.size.height,
               &bx, &by);
    x0 = ax < bx ? ax : bx; x1 = ax < bx ? bx : ax;
    y0 = ay < by ? ay : by; y1 = ay < by ? by : ay;
    out->x0 = x0; out->y0 = y0; out->x1 = x1; out->y1 = y1;
}

static void cg_clip_to_rect(void *ctx, CGRect r)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qclip q;
    if (!c) return;
    cg_map_rect(c, r, &q);
    if (q.x0 > c->g.clip.x0) c->g.clip.x0 = q.x0;
    if (q.y0 > c->g.clip.y0) c->g.clip.y0 = q.y0;
    if (q.x1 < c->g.clip.x1) c->g.clip.x1 = q.x1;
    if (q.y1 < c->g.clip.y1) c->g.clip.y1 = q.y1;
}

/* Save and restore have to stay paired even past the depth this keeps state
 * for: counting beyond the array and only restoring inside it means a plugin
 * that nests deeper than thirty-two loses the extra levels rather than having
 * its restores land on the wrong ones. */
#define QGSTACK ((int)(sizeof ((qcontext *)0)->stack / sizeof ((qcontext *)0)->stack[0]))

static void cg_state_save(void *ctx)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    if (!c) return;
    if (c->depth < QGSTACK) c->stack[c->depth] = c->g;
    c->depth++;
}
static void cg_state_restore(void *ctx)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    if (!c || c->depth <= 0) return;
    c->depth--;
    if (c->depth < QGSTACK) c->g = c->stack[c->depth];
}
static void cg_set_alpha(void *ctx, double a)
{ qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT); if (c) c->g.alpha = a; }
static void cg_set_rgb_fill(void *ctx, double r, double g, double b, double a)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    if (!c) return;
    c->g.fill[0] = r; c->g.fill[1] = g; c->g.fill[2] = b; c->g.fill[3] = a;
}
static void cg_set_fill_color_obj(void *ctx, void *col)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qcolor *k = (qcolor *)q_of(col, Q_COLOR);
    if (!c || !k) return;
    memcpy(c->g.fill, k->c, sizeof c->g.fill);
}

/* One pixel, blended. The buffer is BGRX and the colour is 0..1 RGBA. */
static void cg_blend(qcontext *c, long x, long y, const double *rgba, double alpha)
{
    uint8_t *p;
    double a = rgba[3] * alpha;
    if (x < 0 || y < 0 || (size_t)x >= c->w || (size_t)y >= c->h) return;
    if (a <= 0.0) return;
    if (a > 1.0) a = 1.0;
    p = c->px + (size_t)y * c->stride + (size_t)x * 4;
    p[0] = (uint8_t)(p[0] * (1 - a) + rgba[2] * 255.0 * a);
    p[1] = (uint8_t)(p[1] * (1 - a) + rgba[1] * 255.0 * a);
    p[2] = (uint8_t)(p[2] * (1 - a) + rgba[0] * 255.0 * a);
    p[3] = 255;
}

/* Device rows count from the top; CoreGraphics user space counts from the
 * bottom. Everything below goes through here so the two are never confused. */
static long cg_device_row(const qcontext *c, double user_y)
{ return (long)((double)c->h - user_y); }

static void cg_fill_rect(void *ctx, CGRect r)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qclip q;
    long x, y, x0, x1, y0, y1;
    if (!c || !c->px) return;
    cg_map_rect(c, r, &q);
    if (q.x0 < c->g.clip.x0) q.x0 = c->g.clip.x0;
    if (q.y0 < c->g.clip.y0) q.y0 = c->g.clip.y0;
    if (q.x1 > c->g.clip.x1) q.x1 = c->g.clip.x1;
    if (q.y1 > c->g.clip.y1) q.y1 = c->g.clip.y1;
    x0 = (long)q.x0; x1 = (long)q.x1;
    y0 = cg_device_row(c, q.y1); y1 = cg_device_row(c, q.y0);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            cg_blend(c, x, y, c->g.fill, c->g.alpha);
}

static void cg_context_draw_image(void *ctx, CGRect r, void *img)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qimage *im = (qimage *)q_of(img, Q_IMAGE);
    qclip q, box;
    long dx0, dy0, dx1, dy1, x, y;

    if (getenv("MACQZ_VERBOSE"))
        fprintf(stderr, "  [qz] CGContextDrawImage ctx=%p(%zux%zu) img=%p(%zux%zu) "
                        "at %.0f,%.0f %.0fx%.0f\n",
                (void *)c, c ? c->w : 0, c ? c->h : 0, (void *)im,
                im ? im->w : 0, im ? im->h : 0,
                r.origin.x, r.origin.y, r.size.width, r.size.height);
    if (!c || !im || !c->px || !im->px || !im->w || !im->h) return;

    /* The rect is in user space, so it goes through the CTM -- a VSTGUI editor
     * draws every bitmap at the origin and moves the origin instead. Then the
     * clip, then the canvas. */
    cg_map_rect(c, r, &q);
    box = q;
    if (box.x0 < c->g.clip.x0) box.x0 = c->g.clip.x0;
    if (box.y0 < c->g.clip.y0) box.y0 = c->g.clip.y0;
    if (box.x1 > c->g.clip.x1) box.x1 = c->g.clip.x1;
    if (box.y1 > c->g.clip.y1) box.y1 = c->g.clip.y1;
    if (box.x1 <= box.x0 || box.y1 <= box.y0) return;
    if (q.x1 - q.x0 <= 0.0 || q.y1 - q.y0 <= 0.0) return;

    dx0 = (long)box.x0; dx1 = (long)(box.x1 + 0.5);
    dy0 = cg_device_row(c, box.y1); dy1 = cg_device_row(c, box.y0);
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > (long)c->w) dx1 = (long)c->w;
    if (dy1 > (long)c->h) dy1 = (long)c->h;

    for (y = dy0; y < dy1; y++) {
        /* Back to user space, then to a fraction down the image: a CGImage's
         * row 0 is its top, and its top sits at the rect's maximum y. */
        double uy = (double)c->h - (double)y;
        long sy = (long)(((q.y1 - uy) / (q.y1 - q.y0)) * (double)im->h);
        const uint8_t *srow;
        uint8_t *drow = c->px + (size_t)y * c->stride;
        if (sy < 0) sy = 0;
        if ((size_t)sy >= im->h) sy = (long)im->h - 1;
        srow = im->px + (size_t)sy * im->stride;
        for (x = dx0; x < dx1; x++) {
            long sx = (long)((((double)x + 0.5 - q.x0) / (q.x1 - q.x0)) * (double)im->w);
            const uint8_t *sp;
            unsigned a;
            if (sx < 0) sx = 0;
            if ((size_t)sx >= im->w) sx = (long)im->w - 1;
            sp = srow + sx * 4;
            /* Composited, not copied: a control's bitmap is drawn over the
             * background and the transparent parts have to stay transparent.
             * Copying is what made an editor a stack of opaque rectangles. */
            a = sp[3];
            if (c->g.alpha < 1.0) a = (unsigned)(a * c->g.alpha);
            if (!a) continue;
            if (a == 255) { memcpy(drow + x * 4, sp, 4); continue; }
            {   uint8_t *dp = drow + x * 4;
                dp[0] = (uint8_t)((dp[0] * (255 - a) + sp[0] * a) / 255);
                dp[1] = (uint8_t)((dp[1] * (255 - a) + sp[1] * a) / 255);
                dp[2] = (uint8_t)((dp[2] * (255 - a) + sp[2] * a) / 255);
                dp[3] = 255; }
        }
    }
}

static void cg_context_set_shadow(void *ctx, CGSize off, double blur, void *color)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    (void)color;
    if (!c) return;
    c->shadow[0] = off.width; c->shadow[1] = off.height; c->shadow[2] = blur;
}

/* ----------------------------------------------------------------- images */

static size_t cg_image_width(void *p)
{ qimage *i = (qimage *)q_of(p, Q_IMAGE); return i ? i->w : 0; }
static size_t cg_image_height(void *p)
{ qimage *i = (qimage *)q_of(p, Q_IMAGE); return i ? i->h : 0; }
static size_t cg_image_bpc(void *p)
{ qimage *i = (qimage *)q_of(p, Q_IMAGE); return i ? i->bpc : 8; }
static uint32_t cg_image_bitmapinfo(void *p)
{ qimage *i = (qimage *)q_of(p, Q_IMAGE); return i ? i->bitmapinfo : 0; }
static void *g_colorspace_srgb;
static void *cg_image_colorspace(void *p) { (void)p; return &g_colorspace_srgb; }
static void cg_image_release(void *p)
{
    qimage *i = (qimage *)q_of(p, Q_IMAGE);
    if (!i || --i->o.refs > 0) return;
    if (i->owned) free(i->px);
    q_free(i);
}

/* ---------------------------------------------------------------- colours */

static void *cg_color_create_rgb(double r, double g, double b, double a)
{
    qcolor *c = q_new(sizeof *c, Q_COLOR);
    if (!c) return NULL;
    c->c[0] = r; c->c[1] = g; c->c[2] = b; c->c[3] = a;
    return c;
}
static void cg_color_release(void *p)
{
    qcolor *c = (qcolor *)q_of(p, Q_COLOR);
    if (c && --c->o.refs <= 0) q_free(c);
}

/* --------------------------------------------------------- data providers */

static void *cg_provider_with_data(void *info, const void *data, size_t size,
                                   void *release)
{
    qprovider *p = q_new(sizeof *p, Q_PROVIDER);
    (void)info; (void)release;
    if (!p) return NULL;
    /* Copy: the caller is entitled to free its buffer once the provider exists. */
    p->b = malloc(size ? size : 1);
    if (!p->b) { q_free(p); return NULL; }
    if (data && size) memcpy(p->b, data, size);
    p->n = size;
    p->owned = 1;
    return p;
}

/* A URL here is one of our CFString-alikes carrying a path (see macfound.c). */
static void *cg_provider_with_url(void *url)
{
    char path[4096];
    qprovider *p;
    FILE *f;
    long n;
    if (!url || !macshim_cf_string_get(url, path, sizeof path)) return NULL;
    if (!(f = fopen(path, "rb"))) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    if (!(p = q_new(sizeof *p, Q_PROVIDER))) { fclose(f); return NULL; }
    p->b = malloc((size_t)n ? (size_t)n : 1);
    if (!p->b) { q_free(p); fclose(f); return NULL; }
    p->n = fread(p->b, 1, (size_t)n, f);
    p->owned = 1;
    fclose(f);
    return p;
}

/* CGDataProviderCopyData hands back a CFData. */
static void *cg_provider_copy_data(void *p)
{
    qprovider *q = (qprovider *)q_of(p, Q_PROVIDER);
    if (!q) return NULL;
    return macshim_cf_data(q->b, q->n);
}
static void cg_provider_release(void *p)
{
    qprovider *q = (qprovider *)q_of(p, Q_PROVIDER);
    if (!q || --q->o.refs > 0) return;
    if (q->owned) free(q->b);
    q_free(q);
}

/* A CGFont keeps its provider so the font bytes can reach a text backend when
 * one exists -- the same bytes FreeType takes on the Windows side. */
static void *cg_font_with_provider(void *provider)
{
    qfont *f = q_new(sizeof *f, Q_FONT);
    if (!f) return NULL;
    f->data = (qprovider *)q_of(provider, Q_PROVIDER);
    if (f->data) f->data->o.refs++;
    return f;
}

const void *macquartz_font_bytes(void *font, size_t *len)
{
    qfont *f = (qfont *)q_of(font, Q_FONT);
    if (len) *len = 0;
    if (!f || !f->data) return NULL;
    if (len) *len = f->data->n;
    return f->data->b;
}

/* ---------------------------------------------------------------- display */

/* No display. A plausible size beats zero, which a plugin may divide by. */
static uint32_t cg_main_display(void) { return 1; }
/* The same screen height NSScreen reports and [NSEvent mouseLocation] is
 * measured against. A plugin reads the cursor position through Cocoa, converts
 * it with this, and compares the result to a window coordinate -- so a number
 * invented here rather than shared would put the pointer a screen away from
 * itself. */
static size_t cg_display_pixels_high(uint32_t d)
{ (void)d; return (size_t)macns_screen_height(); }
static int  cg_display_hide_cursor(uint32_t d) { (void)d; return 0; }
static int  cg_display_show_cursor(uint32_t d) { (void)d; return 0; }
/* Warping the cursor is not decoration either: a plugin dragging a locked
 * control moves it back to where the gesture started and expects to read that
 * position back. The point is in screen coordinates counted from the top, which
 * is what the host keeps. */
static int  cg_display_move_cursor(uint32_t d, CGPoint p)
{ (void)d; macns_set_pointer(p.x, p.y); return 0; }
static int  cg_assoc_mouse(int connected) { (void)connected; return 0; }

const macshim_entry macshim_quartz[] = {
    { "_CGBitmapContextCreate",      cg_bitmap_context_create },
    { "_CGBitmapContextCreateImage", cg_bitmap_context_create_image },
    { "_CGContextRelease",           cg_context_release },
    { "_CGContextDrawImage",         cg_context_draw_image },
    { "_CGContextSetShadowWithColor", cg_context_set_shadow },
    { "_CGImageGetWidth",            cg_image_width },
    { "_CGImageGetHeight",           cg_image_height },
    { "_CGImageGetBitsPerComponent", cg_image_bpc },
    { "_CGImageGetBitmapInfo",       cg_image_bitmapinfo },
    { "_CGImageGetColorSpace",       cg_image_colorspace },
    { "_CGImageRelease",             cg_image_release },
    { "_CGColorCreateGenericRGB",    cg_color_create_rgb },
    { "_CGColorRelease",             cg_color_release },
    { "_CGDataProviderCreateWithData", cg_provider_with_data },
    { "_CGDataProviderCreateWithURL",  cg_provider_with_url },
    { "_CGDataProviderCopyData",       cg_provider_copy_data },
    { "_CGDataProviderRelease",        cg_provider_release },
    { "_CGFontCreateWithDataProvider", cg_font_with_provider },
    { "_CGMainDisplayID",              cg_main_display },
    { "_CGDisplayPixelsHigh",          cg_display_pixels_high },
    { "_CGDisplayHideCursor",          cg_display_hide_cursor },
    { "_CGDisplayShowCursor",          cg_display_show_cursor },
    { "_CGDisplayMoveCursorToPoint",   cg_display_move_cursor },
    { "_CGAssociateMouseAndMouseCursorPosition", cg_assoc_mouse },
    { NULL, NULL }
};

/* ------------------------------------------------------------ ImageIO
 *
 * A VSTGUI editor is a stack of PNGs -- background, knob filmstrips, button
 * states -- and it loads every one of them through CGImageSourceCreateWithURL.
 * Nothing else in this host needed image *decoding* before: the Metal path is
 * handed pixels by the plugin, and the Windows side has its own DIB reader. So
 * this is where the corpus's 551 PNGs come in, through the decoder in
 * png_in.h.
 *
 * A source here is just the decoded image. Real ImageIO defers the work until
 * an index is asked for, and can hold several; a PNG holds one and a plugin
 * asks for it immediately, so deferring would buy nothing but a state machine. */
typedef struct { qobj o; qimage *img; } qsource;

static void *cg_source_with_url(void *url, void *opts)
{
    char path[4096];
    qsource *src;
    qimage *im;
    uint32_t *px;
    int w = 0, h = 0;
    long n = 0;
    uint8_t *buf;
    FILE *f;

    (void)opts;
    if (!url || !macshim_cf_string_get(url, path, sizeof path)) return NULL;
    if (!(f = fopen(path, "rb"))) {
        if (getenv("MACQZ_VERBOSE"))
            fprintf(stderr, "  [qz] CGImageSourceCreateWithURL(%s) -> no such file\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    /* A bound, so a file that is not an image cannot ask for an arbitrary
     * allocation before the decoder has looked at a single byte. The largest
     * artwork in this corpus is under a megabyte. */
    if (n <= 0 || n > (long)(64u << 20) || !(buf = malloc((size_t)n))) {
        fclose(f); return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);

    px = image_decode(buf, (size_t)n, &w, &h);
    free(buf);
    if (getenv("MACQZ_VERBOSE"))
        fprintf(stderr, "  [qz] CGImageSourceCreateWithURL(%s) -> %dx%d\n",
                path, w, h);
    if (!px) return NULL;
    if (!(im = q_new(sizeof *im, Q_IMAGE))) { free(px); return NULL; }
    im->px = (uint8_t *)px; im->owned = 1;
    im->w = (size_t)w; im->h = (size_t)h; im->stride = (size_t)w * 4;
    im->bpc = 8; im->bpp = 32;
    if (!(src = q_new(sizeof *src, Q_SOURCE))) { free(px); q_free(im); return NULL; }
    src->img = im;
    return src;
}

/* CGImageCreateWithMaskingColors(image, components): a copy with one colour
 * range knocked out. VSTGUI uses it for BMP artwork, which carries no alpha of
 * its own -- the transparent parts are a colour the plugin names here. Without
 * it a control's background was drawn as an opaque rectangle over whatever it
 * was meant to sit on. `components` is min,max per channel in the image's own
 * order, which for everything here is 8-bit RGB. */
static void *cg_image_masking_colors(void *img, const double *comp)
{
    qimage *src = (qimage *)q_of(img, Q_IMAGE), *out;
    size_t n, i;
    uint8_t *px;
    if (!src || !src->px || !comp) return NULL;
    if (!(out = q_new(sizeof *out, Q_IMAGE))) return NULL;
    *out = *src;
    out->o.refs = 1;
    n = src->stride * src->h;
    if (!(px = malloc(n))) { free(out); return NULL; }
    memcpy(px, src->px, n);
    out->px = px;
    out->owned = 1;
    for (i = 0; i + 3 < n; i += 4) {
        /* The buffer is BGRA; the components are given red first. */
        double r = px[i + 2], g = px[i + 1], b = px[i];
        if (r >= comp[0] && r <= comp[1] && g >= comp[2] && g <= comp[3] &&
            b >= comp[4] && b <= comp[5])
            px[i + 3] = 0;
    }
    return out;
}

static void *cg_source_image_at(void *p, size_t index, void *opts)
{
    qsource *s = (qsource *)q_of(p, Q_SOURCE);
    (void)opts;
    if (!s || index != 0) return NULL;
    /* Retained rather than copied: the caller releases the image and the source
     * separately, and both point at the same pixels. */
    s->img->o.refs++;
    return s->img;
}

static size_t cg_source_count(void *p)
{ return q_of(p, Q_SOURCE) ? 1u : 0u; }

/* The two properties a plugin actually reads, as a dictionary keyed by the
 * constants below. */
static void *g_k_prop_width[2], *g_k_prop_height[2], *g_k_prop_alpha[2];
static void *g_k_source_cache[2];

static void *cg_source_properties(void *p, size_t index, void *opts)
{
    qsource *s = (qsource *)q_of(p, Q_SOURCE);
    void *d;
    (void)opts;
    if (!s || index != 0) return NULL;
    if (!(d = macshim_cf_dict_create_mutable_pub(4))) return NULL;
    macshim_cf_dict_set_pub(d, g_k_prop_width,  macshim_cf_number_int((long)s->img->w));
    macshim_cf_dict_set_pub(d, g_k_prop_height, macshim_cf_number_int((long)s->img->h));
    macshim_cf_dict_set_pub(d, g_k_prop_alpha,  macshim_cf_number_int(1));
    return d;
}

const macshim_entry macshim_imageio[] = {
    { "_CGImageSourceCreateWithURL",        cg_source_with_url },
    { "_CGImageSourceCreateWithData",       NULL },   /* filled below */
    { "_CGImageSourceCreateImageAtIndex",   cg_source_image_at },
    { "_CGImageSourceGetCount",             cg_source_count },
    { "_CGImageSourceCopyPropertiesAtIndex", cg_source_properties },
    { "_kCGImagePropertyPixelWidth",        g_k_prop_width },
    { "_kCGImagePropertyPixelHeight",       g_k_prop_height },
    { "_kCGImagePropertyHasAlpha",          g_k_prop_alpha },
    { "_kCGImageSourceShouldCache",         g_k_source_cache },
    { NULL, NULL }
};

/* ------------------------------------------- the older CoreGraphics surface */

/* An older plugin draws its GUI with CoreGraphics directly rather than through
 * Skia, so it imports the whole drawing API. For the audio path none of that has
 * to draw -- it has to not crash. The calls that return something a plugin will
 * *use* are implemented; the ones that only mark the canvas accept and return.
 *
 * Kept separate from the block above so the distinction stays visible: those are
 * load-bearing, these are placeholders until a GUI needs them.
 */
static size_t cg_bmctx_width(void *ctx)
{ qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT); return c ? c->w : 0; }
static size_t cg_bmctx_height(void *ctx)
{ qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT); return c ? c->h : 0; }
static void *cg_bmctx_data(void *ctx)
{ qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT); return c ? c->px : NULL; }
static size_t cg_bmctx_stride(void *ctx)
{ qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT); return c ? c->stride : 0; }
static size_t cg_bmctx_bpp(void *ctx) { (void)ctx; return 32; }
static size_t cg_bmctx_bpc(void *ctx) { (void)ctx; return 8; }
/* kCGImageAlphaPremultipliedFirst: BGRA, which is what everything here uses. */
static uint32_t cg_bmctx_alpha(void *ctx) { (void)ctx; return 2; }

static void *g_colorspace_devrgb;
static void *cg_colorspace_rgb(void) { return &g_colorspace_devrgb; }

/* The CoreText attribute keys, as real strings. Built once, before a plugin can
 * read them: the loader binds the address of each of these, and the plugin
 * dereferences it. */
static const void *g_ct_font_attr;
static const void *g_ct_fg_attr;

static void __attribute__((constructor)) ct_keys_init(void)
{
    g_ct_font_attr = macshim_cf_string("CTFontAttributeName");
    g_ct_fg_attr   = macshim_cf_string("CTForegroundColorAttributeName");
}

static void *cg_color_create_comps(void *space, const double *c)
{
    (void)space;
    return c ? cg_color_create_rgb(c[0], c[1], c[2], c[3]) : NULL;
}

/* A CGImage built over caller-supplied pixels via a data provider. */
static void *cg_image_create(size_t w, size_t h, size_t bpc, size_t bpp,
                             size_t stride, void *space, uint32_t info,
                             void *provider, const double *decode,
                             int interpolate, int intent)
{
    qimage *im = q_new(sizeof *im, Q_IMAGE);
    size_t len = 0;
    const void *px;
    (void)space; (void)decode; (void)interpolate; (void)intent;
    if (!im) return NULL;
    im->w = w; im->h = h; im->bpc = bpc; im->bpp = bpp;
    im->stride = stride ? stride : w * 4;
    im->bitmapinfo = info;
    px = macquartz_provider_bytes(provider, &len);
    if (px && len) {
        im->px = malloc(len);
        if (im->px) { memcpy(im->px, px, len); im->owned = 1; }
    }
    return im;
}


/* The drawing calls. Each accepts its arguments and returns; a path or a fill
 * that goes nowhere is correct for a headless render. */
static void cg_noop(void) { }
static void *cg_noop_ptr(void) { return NULL; }
static double cg_noop_double(void) { return 0.0; }

/* ---- what a VSTGUI editor asks for beyond the above --------------------
 *
 * These are the calls the twelve VSTGUI editors here reach that nothing had
 * needed before. Most are marks on a canvas that this host does not rasterize,
 * and accepting them is the whole job -- the artwork is PNG blitted through
 * CGContextDrawImage, which is real, and a fill that goes nowhere costs a
 * gradient behind a bitmap.
 *
 * The ones that had to be real are the ones whose *result* a plugin uses.
 * CGImageRetain returning nil rather than the image is the difference between
 * an editor and a fault, and it is not obvious from the name: it looks like
 * ceremony, and it is the object the caller goes on to draw. */
static void *cg_image_retain(void *img)
{
    qobj *o = (qobj *)q_of(img, Q_IMAGE);
    if (o) o->refs++;
    return img;
}
static void *g_colorspace_named;
static void *cg_colorspace_named(void *name) { (void)name; return &g_colorspace_named; }
static void  cg_colorspace_release(void *cs) { (void)cs; }

/* A colour space name is a CFString the plugin reads through the loader's
 * binding of the symbol, so it has to exist before anything asks. */
static const void *g_k_cs_generic_rgb;
static const void *g_k_cs_srgb;
static void __attribute__((constructor)) cs_names_init(void)
{
    g_k_cs_generic_rgb = macshim_cf_string("kCGColorSpaceGenericRGB");
    g_k_cs_srgb        = macshim_cf_string("kCGColorSpaceSRGB");
}

/* Rect accessors. Trivial, and a plugin that lays its interface out with them
 * gets nothing but zeroes without them. */
static double cg_rect_width(CGRect r)  { return r.size.width; }
static double cg_rect_height(CGRect r) { return r.size.height; }
static double cg_rect_minx(CGRect r)   { return r.origin.x; }
static double cg_rect_miny(CGRect r)   { return r.origin.y; }
static double cg_rect_maxx(CGRect r)   { return r.origin.x + r.size.width; }
static double cg_rect_maxy(CGRect r)   { return r.origin.y + r.size.height; }
static double cg_rect_midx(CGRect r)   { return r.origin.x + r.size.width / 2.0; }
static double cg_rect_midy(CGRect r)   { return r.origin.y + r.size.height / 2.0; }

/* An affine transform is returned by value -- six doubles, so in memory rather
 * than registers, and a plugin that keeps one has to get a whole one back. */
typedef struct { double a, b, c, d, tx, ty; } CGAffine;
static CGAffine cg_affine_identity_v(void)
{ CGAffine t = { 1, 0, 0, 1, 0, 0 }; return t; }
static CGAffine cg_affine_translate_make(double tx, double ty)
{ CGAffine t = { 1, 0, 0, 1, tx, ty }; return t; }
static CGAffine cg_affine_scale_make(double sx, double sy)
{ CGAffine t = { sx, 0, 0, sy, 0, 0 }; return t; }
static CGAffine cg_affine_scale(CGAffine t, double sx, double sy)
{ t.a *= sx; t.b *= sx; t.c *= sy; t.d *= sy; return t; }
static CGAffine cg_affine_translate(CGAffine t, double tx, double ty)
{ t.tx += t.a * tx + t.c * ty; t.ty += t.b * tx + t.d * ty; return t; }

/* A gradient is kept only so that releasing one is balanced. */
static void *cg_gradient_create(void *space, void *colors, const double *locs)
{ (void)space; (void)colors; (void)locs; return q_new(sizeof(qobj), Q_GRADIENT); }

const void *macquartz_provider_bytes(void *provider, size_t *len)
{
    qprovider *q = (qprovider *)q_of(provider, Q_PROVIDER);
    if (len) *len = 0;
    if (!q) return NULL;
    if (len) *len = (size_t)q->n;
    return q->b;
}

/* The rest, from the measured import list: drawing, text layout, ColorSync and
 * Carbon theming. Generated from what the corpus actually asks for rather than
 * from the framework headers, so the list stays honest about coverage. */
/* ------------------------------------------------------------------ paths
 *
 * Every one of these was a no-op, which is a quiet way to lose a control.
 * An editor's fixed furniture is bitmaps -- panels, labels, the knob body --
 * and those drew correctly, so the editors looked finished. What is drawn with
 * a path is the part that *moves*: the pointer on a knob, the needle on a
 * meter, the curve in an envelope, the highlight on the selected step. With
 * the path calls accepting and returning, Tattoo's knobs were featureless
 * discs that never changed, and dragging one moved the sound while the picture
 * sat still.
 *
 * Points are flattened and transformed to device space as they are added,
 * which is what Core Graphics does -- a point belongs to the CTM in force when
 * it was added, not the one in force when the path is painted.
 */

#define QP_CURVE_STEPS 16       /* per cubic; a UI curve is a few tens of pixels */
#define QP_ARC_STEPS   32       /* per full turn */
#define QP_AA          4        /* sub-scanlines per pixel row */

static void geom_free(qgeom *g)
{ free(g->pt); free(g->sub); free(g->closed); memset(g, 0, sizeof *g); }

static void geom_clear(qgeom *g) { g->np = 0; g->ns = 0; }

static int geom_reserve(qgeom *g, long extra)
{
    if (g->np + extra > g->npcap) {
        long cap = g->npcap ? g->npcap * 2 : 64;
        qpt *n;
        while (cap < g->np + extra) cap *= 2;
        if (!(n = realloc(g->pt, (size_t)cap * sizeof *n))) return 0;
        g->pt = n; g->npcap = cap;
    }
    return 1;
}

static int geom_new_sub(qgeom *g)
{
    if (g->ns + 1 > g->nscap) {
        long cap = g->nscap ? g->nscap * 2 : 16;
        long *n; unsigned char *m;
        if (!(n = realloc(g->sub, (size_t)cap * sizeof *n))) return 0;
        g->sub = n;
        if (!(m = realloc(g->closed, (size_t)cap))) return 0;
        g->closed = m;
        g->nscap = cap;
    }
    g->sub[g->ns] = g->np;
    g->closed[g->ns] = 0;
    g->ns++;
    return 1;
}

static void geom_add(qgeom *g, double x, double y)
{
    if (!g->ns && !geom_new_sub(g)) return;
    if (!geom_reserve(g, 1)) return;
    /* Drop a point that repeats the last: a zero-length edge contributes
     * nothing to a fill and makes a degenerate quad in a stroke. */
    if (g->np > g->sub[g->ns - 1] &&
        g->pt[g->np - 1].x == x && g->pt[g->np - 1].y == y) return;
    g->pt[g->np].x = x; g->pt[g->np].y = y; g->np++;
}

static long geom_sub_end(const qgeom *g, long i)
{ return i + 1 < g->ns ? g->sub[i + 1] : g->np; }

/* ---- building in device space ---- */

static void cg_user_to_dev(const qcontext *c, double x, double y,
                           double *ox, double *oy)
{
    double mx, my;
    qmat_apply(&c->g.ctm, x, y, &mx, &my);
    *ox = mx;
    *oy = (double)c->h - my;
}

/* How much the CTM scales a length, for line widths and flattening. */
static double cg_ctm_scale_of(const qcontext *c)
{
    double det = fabs(c->g.ctm.a * c->g.ctm.d - c->g.ctm.b * c->g.ctm.c);
    double s = sqrt(det);
    return s > 1e-9 ? s : 1.0;
}

static void path_move(qcontext *c, double x, double y)
{
    double dx, dy;
    cg_user_to_dev(c, x, y, &dx, &dy);
    if (!geom_new_sub(&c->path)) return;
    geom_add(&c->path, dx, dy);
    c->cur_x = x; c->cur_y = y; c->have_cur = 1;
}
static void path_line(qcontext *c, double x, double y)
{
    double dx, dy;
    if (!c->have_cur) { path_move(c, x, y); return; }
    cg_user_to_dev(c, x, y, &dx, &dy);
    geom_add(&c->path, dx, dy);
    c->cur_x = x; c->cur_y = y;
}
static void path_curve(qcontext *c, double c1x, double c1y,
                       double c2x, double c2y, double x, double y)
{
    double x0 = c->cur_x, y0 = c->cur_y;
    int i;
    if (!c->have_cur) { path_move(c, c1x, c1y); x0 = c1x; y0 = c1y; }
    for (i = 1; i <= QP_CURVE_STEPS; i++) {
        double t = (double)i / QP_CURVE_STEPS, u = 1.0 - t;
        double bx = u*u*u*x0 + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*x;
        double by = u*u*u*y0 + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*y;
        double dx, dy;
        cg_user_to_dev(c, bx, by, &dx, &dy);
        geom_add(&c->path, dx, dy);
    }
    c->cur_x = x; c->cur_y = y; c->have_cur = 1;
}
static void path_arc(qcontext *c, double cx, double cy, double r,
                     double a0, double a1, int clockwise)
{
    int n, i;
    double span;
    if (r <= 0.0) return;
    /* Core Graphics sweeps the short way round in the direction asked for;
     * normalise the span so the sign says which way and the size says how far. */
    span = a1 - a0;
    if (clockwise) { while (span > 0.0) span -= 2.0 * M_PI; }
    else           { while (span < 0.0) span += 2.0 * M_PI; }
    if (fabs(span) > 2.0 * M_PI) span = span > 0 ? 2.0 * M_PI : -2.0 * M_PI;
    n = (int)(QP_ARC_STEPS * fabs(span) / (2.0 * M_PI)) + 2;
    /* With no current point the arc opens a subpath of its own; with one, it
     * continues from there, which is the line Core Graphics draws to the arc's
     * first point. Appending to a subpath that has been closed would reopen it. */
    if (!c->have_cur && !geom_new_sub(&c->path)) return;
    for (i = 0; i <= n; i++) {
        double t = a0 + span * (double)i / n;
        double dx, dy;
        cg_user_to_dev(c, cx + r * cos(t), cy + r * sin(t), &dx, &dy);
        geom_add(&c->path, dx, dy);
    }
    c->cur_x = cx + r * cos(a0 + span); c->cur_y = cy + r * sin(a0 + span);
    c->have_cur = 1;
}
static void path_rect(qcontext *c, CGRect r)
{
    path_move(c, r.origin.x, r.origin.y);
    path_line(c, r.origin.x + r.size.width, r.origin.y);
    path_line(c, r.origin.x + r.size.width, r.origin.y + r.size.height);
    path_line(c, r.origin.x, r.origin.y + r.size.height);
    if (c->path.ns) c->path.closed[c->path.ns - 1] = 1;
    c->have_cur = 0;
}
static void path_ellipse(qcontext *c, CGRect r)
{
    double cx = r.origin.x + r.size.width / 2.0;
    double cy = r.origin.y + r.size.height / 2.0;
    double rx = r.size.width / 2.0, ry = r.size.height / 2.0;
    int i;
    if (rx <= 0.0 || ry <= 0.0) return;
    if (!geom_new_sub(&c->path)) return;
    for (i = 0; i < QP_ARC_STEPS; i++) {
        double t = 2.0 * M_PI * i / QP_ARC_STEPS, dx, dy;
        cg_user_to_dev(c, cx + rx * cos(t), cy + ry * sin(t), &dx, &dy);
        geom_add(&c->path, dx, dy);
    }
    c->path.closed[c->path.ns - 1] = 1;
    c->have_cur = 0;
}

/* ---- filling ------------------------------------------------------------
 *
 * A scanline fill with four sub-scanlines a row and exact horizontal coverage,
 * which is enough anti-aliasing that a knob's pointer does not crawl as it
 * turns. Coverage is accumulated for one row at a time and then blended, so a
 * pixel the path crosses twice in one row is still painted once. */

typedef struct { double x0, y0, x1, y1; } qedge;

static double *g_cov;
static long    g_covcap;
static qedge  *g_edge;
static long    g_edgecap;
static double *g_xs;
static int    *g_dir;
static long    g_xscap;

static int cov_room(long n)
{
    if (n > g_covcap) {
        double *p = realloc(g_cov, (size_t)n * sizeof *p);
        if (!p) return 0;
        g_cov = p; g_covcap = n;
    }
    return 1;
}
static int edge_room(long n)
{
    if (n > g_edgecap) {
        qedge *p = realloc(g_edge, (size_t)n * sizeof *p);
        if (!p) return 0;
        g_edge = p; g_edgecap = n;
    }
    if (n > g_xscap) {
        double *x = realloc(g_xs, (size_t)n * sizeof *x);
        int *d = realloc(g_dir, (size_t)n * sizeof *d);
        if (!x || !d) { free(x); free(d); return 0; }
        g_xs = x; g_dir = d; g_xscap = n;
    }
    return 1;
}

/* Add a span's coverage to the row, with partial pixels at both ends. */
static void cov_span(double *cov, long w, double xa, double xb, double weight)
{
    long i, ia, ib;
    if (xb <= xa) return;
    if (xa < 0.0) xa = 0.0;
    if (xb > (double)w) xb = (double)w;
    if (xb <= xa) return;
    ia = (long)floor(xa); ib = (long)floor(xb);
    if (ia == ib) { cov[ia] += (xb - xa) * weight; return; }
    cov[ia] += ((double)(ia + 1) - xa) * weight;
    for (i = ia + 1; i < ib; i++) cov[i] += weight;
    if (ib < w) cov[ib] += (xb - (double)ib) * weight;
}

/* One filled path. `rgba` and `alpha` are the paint; `eo` picks the fill rule. */
static void geom_fill(qcontext *c, const qgeom *g, const double *rgba,
                      double alpha, int eo)
{
    long i, s, ne = 0, y, cx0, cx1, cy0, cy1, w;
    double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;

    if (!c || !c->px || !g->np || !g->ns) return;

    for (i = 0; i < g->np; i++) {
        if (g->pt[i].x < minx) minx = g->pt[i].x;
        if (g->pt[i].x > maxx) maxx = g->pt[i].x;
        if (g->pt[i].y < miny) miny = g->pt[i].y;
        if (g->pt[i].y > maxy) maxy = g->pt[i].y;
    }
    /* The clip is kept in the space the CTM maps into, the same one cg_fill_rect
     * uses; rows count the other way. */
    cx0 = (long)floor(c->g.clip.x0); cx1 = (long)ceil(c->g.clip.x1);
    cy0 = cg_device_row(c, c->g.clip.y1); cy1 = cg_device_row(c, c->g.clip.y0);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > (long)c->w) cx1 = (long)c->w;
    if (cy1 > (long)c->h) cy1 = (long)c->h;
    if ((long)floor(minx) > cx0) cx0 = (long)floor(minx);
    if ((long)ceil(maxx)  < cx1) cx1 = (long)ceil(maxx);
    if ((long)floor(miny) > cy0) cy0 = (long)floor(miny);
    if ((long)ceil(maxy)  < cy1) cy1 = (long)ceil(maxy);
    if (cx1 <= cx0 || cy1 <= cy0) return;

    /* Edges, every subpath closed -- a fill always closes. */
    for (s = 0; s < g->ns; s++) {
        long a = g->sub[s], b = geom_sub_end(g, s);
        if (b - a < 2) continue;
        if (!edge_room(ne + (b - a) + 1)) return;
        for (i = a; i + 1 < b; i++) {
            g_edge[ne].x0 = g->pt[i].x;     g_edge[ne].y0 = g->pt[i].y;
            g_edge[ne].x1 = g->pt[i + 1].x; g_edge[ne].y1 = g->pt[i + 1].y;
            ne++;
        }
        if (g->pt[b - 1].x != g->pt[a].x || g->pt[b - 1].y != g->pt[a].y) {
            g_edge[ne].x0 = g->pt[b - 1].x; g_edge[ne].y0 = g->pt[b - 1].y;
            g_edge[ne].x1 = g->pt[a].x;     g_edge[ne].y1 = g->pt[a].y;
            ne++;
        }
    }
    if (!ne) return;

    w = cx1 - cx0;
    if (!cov_room(w)) return;

    for (y = cy0; y < cy1; y++) {
        long k, n;
        int  sub;
        memset(g_cov, 0, (size_t)w * sizeof *g_cov);
        for (sub = 0; sub < QP_AA; sub++) {
            double sy = (double)y + ((double)sub + 0.5) / QP_AA;
            n = 0;
            for (k = 0; k < ne; k++) {
                double y0 = g_edge[k].y0, y1 = g_edge[k].y1;
                if (y0 == y1) continue;
                if ((sy >= y0 && sy < y1) || (sy >= y1 && sy < y0)) {
                    double t = (sy - y0) / (y1 - y0);
                    g_xs[n]  = g_edge[k].x0 + t * (g_edge[k].x1 - g_edge[k].x0);
                    g_dir[n] = y1 > y0 ? 1 : -1;
                    n++;
                }
            }
            if (n < 2) continue;
            /* Insertion sort: a scanline crosses a UI path a handful of times. */
            for (k = 1; k < n; k++) {
                double xv = g_xs[k]; int dv = g_dir[k]; long j = k - 1;
                while (j >= 0 && g_xs[j] > xv) { g_xs[j+1] = g_xs[j]; g_dir[j+1] = g_dir[j]; j--; }
                g_xs[j+1] = xv; g_dir[j+1] = dv;
            }
            { int wind = 0;
              for (k = 0; k + 1 <= n - 1; k++) {
                  int inside;
                  wind += eo ? 1 : g_dir[k];
                  inside = eo ? ((k & 1) == 0) : (wind != 0);
                  if (inside)
                      cov_span(g_cov, w, g_xs[k] - (double)cx0,
                               g_xs[k+1] - (double)cx0, 1.0 / QP_AA);
              } }
        }
        for (k = 0; k < w; k++) {
            double a = g_cov[k];
            if (a <= 0.0009) continue;
            if (a > 1.0) a = 1.0;
            cg_blend(c, cx0 + k, y, rgba, alpha * a);
        }
    }
}

/* ---- stroking -----------------------------------------------------------
 *
 * Each segment becomes a quad and each joint a small polygon, all filled with
 * the nonzero rule so the overlaps union rather than cancel. That is coarser
 * than real joins and caps, and at the one-to-two pixel widths an editor uses
 * the difference does not survive being rasterised. */
static void geom_stroke(qcontext *c, const qgeom *g, double w,
                        const double *rgba, double alpha)
{
    qgeom out;
    long s, i, k;
    double hw = w * 0.5;

    if (hw < 0.35) hw = 0.35;      /* a hairline still has to land on a pixel */
    memset(&out, 0, sizeof out);

    for (s = 0; s < g->ns; s++) {
        long a = g->sub[s], b = geom_sub_end(g, s);
        long last = b - 1;
        if (b - a < 2) {
            continue;
        }
        for (i = a; i < last || (g->closed[s] && i == last); i++) {
            long j = (i == last) ? a : i + 1;
            double dx = g->pt[j].x - g->pt[i].x;
            double dy = g->pt[j].y - g->pt[i].y;
            double len = sqrt(dx * dx + dy * dy), nx, ny;
            if (len < 1e-9) continue;
            nx = -dy / len * hw; ny = dx / len * hw;
            if (!geom_new_sub(&out)) { geom_free(&out); return; }
            geom_add(&out, g->pt[i].x + nx, g->pt[i].y + ny);
            geom_add(&out, g->pt[j].x + nx, g->pt[j].y + ny);
            geom_add(&out, g->pt[j].x - nx, g->pt[j].y - ny);
            geom_add(&out, g->pt[i].x - nx, g->pt[i].y - ny);
            out.closed[out.ns - 1] = 1;
            if (i == last) break;
        }
        /* Joints, so a corner is not a notch. Only worth it once the line is
         * wide enough for the notch to be visible. */
        if (hw > 0.75)
            for (k = a; k < b; k++) {
                int t;
                if (!geom_new_sub(&out)) { geom_free(&out); return; }
                for (t = 0; t < 8; t++) {
                    double ang = 2.0 * M_PI * t / 8.0;
                    geom_add(&out, g->pt[k].x + hw * cos(ang),
                                   g->pt[k].y + hw * sin(ang));
                }
                out.closed[out.ns - 1] = 1;
            }
    }
    geom_fill(c, &out, rgba, alpha, 0);
    geom_free(&out);
}

/* ---- the calls a plugin makes ---- */

static qcontext *ctx_of(void *p) { return (qcontext *)q_of(p, Q_CONTEXT); }

static void cg_begin_path(void *ctx)
{ qcontext *c = ctx_of(ctx); if (c) { geom_clear(&c->path); c->have_cur = 0; } }
static void cg_move_to(void *ctx, double x, double y)
{ qcontext *c = ctx_of(ctx); if (c) path_move(c, x, y); }
static void cg_line_to(void *ctx, double x, double y)
{ qcontext *c = ctx_of(ctx); if (c) path_line(c, x, y); }
static void cg_curve_to(void *ctx, double c1x, double c1y, double c2x, double c2y,
                        double x, double y)
{ qcontext *c = ctx_of(ctx); if (c) path_curve(c, c1x, c1y, c2x, c2y, x, y); }
static void cg_add_arc(void *ctx, double x, double y, double r,
                       double a0, double a1, int cw)
{ qcontext *c = ctx_of(ctx); if (c) path_arc(c, x, y, r, a0, a1, cw); }
/* The tangent-arc form. Approximated by the corner it rounds, which is what it
 * degenerates to as the radius goes to zero and is within a pixel of the real
 * curve at the radii a rounded panel uses. */
static void cg_add_arc_to(void *ctx, double x1, double y1, double x2, double y2,
                          double r)
{ qcontext *c = ctx_of(ctx); (void)r; if (c) { path_line(c, x1, y1); path_line(c, x2, y2); } }
static void cg_add_rect(void *ctx, CGRect r)
{ qcontext *c = ctx_of(ctx); if (c) path_rect(c, r); }
static void cg_close_path(void *ctx)
{
    qcontext *c = ctx_of(ctx);
    if (c && c->path.ns) { c->path.closed[c->path.ns - 1] = 1; c->have_cur = 0; }
}
static void cg_add_ellipse(void *ctx, CGRect r)
{ qcontext *c = ctx_of(ctx); if (c) path_ellipse(c, r); }

/* Defined after the CGPath object below, which it needs. */
static void cg_add_path(void *ctx, void *path);

static void cg_fill_path_rule(void *ctx, int eo)
{
    qcontext *c = ctx_of(ctx);
    if (!c) return;
    geom_fill(c, &c->path, c->g.fill, c->g.alpha, eo);
    geom_clear(&c->path); c->have_cur = 0;
}
static void cg_fill_path(void *ctx)   { cg_fill_path_rule(ctx, 0); }
static void cg_eofill_path(void *ctx) { cg_fill_path_rule(ctx, 1); }
static void cg_stroke_path(void *ctx)
{
    qcontext *c = ctx_of(ctx);
    if (!c) return;
    geom_stroke(c, &c->path, c->g.line_width * cg_ctm_scale_of(c),
                c->g.stroke, c->g.alpha);
    geom_clear(&c->path); c->have_cur = 0;
}
/* kCGPathFill 0, EOFill 1, Stroke 2, FillStroke 3, EOFillStroke 4 */
static void cg_draw_path(void *ctx, int mode)
{
    qcontext *c = ctx_of(ctx);
    if (!c) return;
    if (mode == 0 || mode == 3) geom_fill(c, &c->path, c->g.fill, c->g.alpha, 0);
    if (mode == 1 || mode == 4) geom_fill(c, &c->path, c->g.fill, c->g.alpha, 1);
    if (mode >= 2)
        geom_stroke(c, &c->path, c->g.line_width * cg_ctm_scale_of(c),
                    c->g.stroke, c->g.alpha);
    geom_clear(&c->path); c->have_cur = 0;
}
/* Clipping to a path. The shape is reduced to its bounding box, which is the
 * same approximation the rest of the clip handling makes; the important part is
 * that the path is consumed, or the next fill would paint the clip shape. */
static void cg_clip_path(void *ctx)
{
    qcontext *c = ctx_of(ctx);
    long i;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    if (!c) return;
    if (!c->path.np) { geom_clear(&c->path); c->have_cur = 0; return; }
    for (i = 0; i < c->path.np; i++) {
        double ux = c->path.pt[i].x, uy = (double)c->h - c->path.pt[i].y;
        if (ux < x0) x0 = ux;
        if (ux > x1) x1 = ux;
        if (uy < y0) y0 = uy;
        if (uy > y1) y1 = uy;
    }
    if (x0 > c->g.clip.x0) c->g.clip.x0 = x0;
    if (y0 > c->g.clip.y0) c->g.clip.y0 = y0;
    if (x1 < c->g.clip.x1) c->g.clip.x1 = x1;
    if (y1 < c->g.clip.y1) c->g.clip.y1 = y1;
    geom_clear(&c->path); c->have_cur = 0;
}

static void cg_fill_ellipse(void *ctx, CGRect r)
{
    qcontext *c = ctx_of(ctx);
    if (!c) return;
    geom_clear(&c->path); c->have_cur = 0;
    path_ellipse(c, r);
    cg_fill_path_rule(ctx, 0);
}
static void cg_stroke_ellipse(void *ctx, CGRect r)
{
    qcontext *c = ctx_of(ctx);
    if (!c) return;
    geom_clear(&c->path); c->have_cur = 0;
    path_ellipse(c, r);
    cg_stroke_path(ctx);
}
static void cg_stroke_rect_w(void *ctx, CGRect r, double w)
{
    qcontext *c = ctx_of(ctx);
    if (!c) return;
    geom_clear(&c->path); c->have_cur = 0;
    path_rect(c, r);
    geom_stroke(c, &c->path, w * cg_ctm_scale_of(c), c->g.stroke, c->g.alpha);
    geom_clear(&c->path); c->have_cur = 0;
}
static void cg_stroke_rect(void *ctx, CGRect r)
{ qcontext *c = ctx_of(ctx); if (c) cg_stroke_rect_w(ctx, r, c->g.line_width); }
static void cg_stroke_segments(void *ctx, const CGPoint *pts, size_t n)
{
    qcontext *c = ctx_of(ctx);
    size_t i;
    if (!c || !pts) return;
    geom_clear(&c->path); c->have_cur = 0;
    for (i = 0; i + 1 < n; i += 2) {
        path_move(c, pts[i].x, pts[i].y);
        path_line(c, pts[i + 1].x, pts[i + 1].y);
    }
    cg_stroke_path(ctx);
}
static void cg_set_line_width(void *ctx, double w)
{ qcontext *c = ctx_of(ctx); if (c) c->g.line_width = w > 0.0 ? w : 0.0; }
static void cg_set_rgb_stroke(void *ctx, double r, double g, double b, double a)
{
    qcontext *c = ctx_of(ctx);
    if (!c) return;
    c->g.stroke[0] = r; c->g.stroke[1] = g; c->g.stroke[2] = b; c->g.stroke[3] = a;
}
static void cg_set_stroke_color_obj(void *ctx, void *col)
{
    qcontext *c = ctx_of(ctx);
    qcolor *k = (qcolor *)q_of(col, Q_COLOR);
    if (!c || !k) return;
    memcpy(c->g.stroke, k->c, sizeof c->g.stroke);
}
/* Rotation, which a knob's pointer is drawn with. */
static void cg_ctm_rotate(void *ctx, double rad)
{
    qcontext *c = ctx_of(ctx);
    qmat r;
    if (!c) return;
    r.a = cos(rad); r.b = sin(rad); r.c = -sin(rad); r.d = cos(rad);
    r.tx = 0; r.ty = 0;
    qmat_concat(&c->g.ctm, &r);
}
static void cg_clear_rect(void *ctx, CGRect r)
{
    qcontext *c = ctx_of(ctx);
    qclip q;
    long y, x0, x1, y0, y1;
    if (!c || !c->px) return;
    cg_map_rect(c, r, &q);
    if (q.x0 < c->g.clip.x0) q.x0 = c->g.clip.x0;
    if (q.y0 < c->g.clip.y0) q.y0 = c->g.clip.y0;
    if (q.x1 > c->g.clip.x1) q.x1 = c->g.clip.x1;
    if (q.y1 > c->g.clip.y1) q.y1 = c->g.clip.y1;
    x0 = (long)q.x0; x1 = (long)q.x1;
    y0 = cg_device_row(c, q.y1); y1 = cg_device_row(c, q.y0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (long)c->w) x1 = (long)c->w;
    if (y1 > (long)c->h) y1 = (long)c->h;
    for (y = y0; y < y1; y++)
        if (x1 > x0)
            memset(c->px + (size_t)y * c->stride + (size_t)x0 * 4, 0,
                   (size_t)(x1 - x0) * 4);
}

const macshim_entry macshim_quartz2[] = {
    { "_CFAttributedStringCreate", cg_noop_ptr },
    { "_CFUserNotificationDisplayAlert", cg_noop },
    { "_CGContextAddCurveToPoint", cg_curve_to },
    { "_CGContextAddLineToPoint", cg_line_to },
    /* The rest of what a VSTGUI editor asks for. Marks on the canvas accept and
     * return; anything whose result is used is above. */
    { "_CGImageRetain",              cg_image_retain },
    { "_CGImageCreateWithMaskingColors", cg_image_masking_colors },
    { "_CGColorSpaceCreateWithName", cg_colorspace_named },
    { "_CGColorSpaceRelease",        cg_colorspace_release },
    { "_kCGColorSpaceGenericRGB",    &g_k_cs_generic_rgb },
    { "_kCGColorSpaceSRGB",          &g_k_cs_srgb },
    { "_CGRectGetWidth",             cg_rect_width },
    { "_CGRectGetHeight",            cg_rect_height },
    { "_CGRectGetMinX",              cg_rect_minx },
    { "_CGRectGetMinY",              cg_rect_miny },
    { "_CGRectGetMaxX",              cg_rect_maxx },
    { "_CGRectGetMaxY",              cg_rect_maxy },
    { "_CGRectGetMidX",              cg_rect_midx },
    { "_CGRectGetMidY",              cg_rect_midy },
    
    { "_CGAffineTransformIdentity",  cg_affine_identity_v },
    { "_CGAffineTransformMakeTranslation", cg_affine_translate_make },
    { "_CGAffineTransformMakeScale", cg_affine_scale_make },
    { "_CGAffineTransformScale",     cg_affine_scale },
    { "_CGAffineTransformTranslate", cg_affine_translate },
    { "_CGGradientCreateWithColors", cg_gradient_create },
    { "_CGGradientRelease",          cg_noop },
    { "_CGBitmapContextGetData",     cg_bmctx_data },
    { "_CGBitmapContextGetBytesPerRow", cg_bmctx_stride },
    { "_CGBitmapContextGetBitsPerPixel", cg_bmctx_bpp },
    { "_CGBitmapContextGetBitsPerComponent", cg_bmctx_bpc },
    { "_CGBitmapContextGetAlphaInfo", cg_bmctx_alpha },
    { "_CGContextAddArc", cg_add_arc },
    { "_CGContextAddArcToPoint", cg_add_arc_to },
    { "_CGContextAddPath", cg_add_path },
    { "_CGContextAddRect", cg_add_rect },
    { "_CGContextClip", cg_clip_path },
    { "_CGContextEOClip", cg_clip_path },
    { "_CGContextEOFillPath", cg_eofill_path },
    { "_CGContextClosePath", cg_close_path },
    { "_CGContextFillPath", cg_fill_path },
    { "_CGContextConcatCTM", cg_ctm_concat },
    { "_CGContextRotateCTM", cg_ctm_rotate },
    { "_CGContextDrawLinearGradient", cg_noop },
    { "_CGContextDrawRadialGradient", cg_noop },
    { "_CGContextSetAlpha", cg_set_alpha },
    { "_CGContextSetBlendMode", cg_noop },
    { "_CGContextSetFillColorSpace", cg_noop },
    { "_CGContextSetStrokeColorSpace", cg_noop },
    { "_CGContextSetFillColor", cg_noop },
    { "_CGContextSetStrokeColor", cg_noop },
    { "_CGContextSetRGBFillColor", cg_set_rgb_fill },
    { "_CGContextSetLineCap", cg_noop },
    { "_CGContextSetLineJoin", cg_noop },
    { "_CGContextSetLineDash", cg_noop },
    { "_CGContextSetMiterLimit", cg_noop },
    { "_CGContextSetFlatness", cg_noop },
    { "_CGContextSetShouldSmoothFonts", cg_noop },
    { "_CGContextSetAllowsAntialiasing", cg_noop },
    { "_CGContextSetPatternPhase", cg_noop },
    { "_CGContextStrokeLineSegments", cg_stroke_segments },
    { "_CGContextStrokeRect", cg_stroke_rect },
    { "_CGContextSynchronize", cg_noop },
    { "_CGContextFlush", cg_noop },
    { "_CGContextBeginPath", cg_begin_path },
    { "_CGContextClearRect", cg_clear_rect },
    { "_CGContextClipToRect", cg_clip_to_rect },
    { "_CGContextDrawPath", cg_draw_path },
    { "_CGContextAddEllipseInRect", cg_add_ellipse },
    { "_CGContextFillEllipseInRect", cg_fill_ellipse },
    { "_CGContextFillRect", cg_fill_rect },
    { "_CGContextMoveToPoint", cg_move_to },
    { "_CGContextScaleCTM", cg_ctm_scale },
    { "_CGContextSetFillColorWithColor", cg_set_fill_color_obj },
    { "_CGContextSetInterpolationQuality", cg_noop },
    { "_CGContextSetLineWidth", cg_set_line_width },
    { "_CGContextSetRGBStrokeColor", cg_set_rgb_stroke },
    { "_CGContextSetShouldAntialias", cg_noop },
    { "_CGContextSetStrokeColorWithColor", cg_set_stroke_color_obj },
    { "_CGContextSetTextMatrix", cg_noop },
    { "_CGContextSetTextPosition", cg_noop },
    { "_CGContextStrokeEllipseInRect", cg_stroke_ellipse },
    { "_CGContextStrokePath", cg_stroke_path },
    { "_CGContextStrokeRectWithWidth", cg_stroke_rect_w },
    { "_CGContextTranslateCTM", cg_ctm_translate },
    { "_CMCloseProfile", cg_noop },
    { "_CMGetSystemProfile", cg_noop },
    { "_CTLineDraw", cg_noop },
    { "entry", cg_noop },
    { "_HIThemeSetFill", cg_noop },
    /* Data, not functions. Registering these as a no-op handed the plugin a code
     * address where a CFStringRef belongs, and it then used that as a dictionary
     * key -- which is how drawing text ended up dereferencing nothing. The values
     * only have to be consistent with the CoreText here, since both ends are
     * ours. */
    { "_kCTFontAttributeName",            &g_ct_font_attr },
    { "_kCTForegroundColorAttributeName", &g_ct_fg_attr },
    { "_NSDeviceRGBColorSpace", cg_noop },
    { "_NSFoundationVersionNumber", cg_noop },
    { "_NSZeroRect", cg_noop },
    { "__objc_empty_vtable", cg_noop },
    { "unimplemented", cg_noop },

    { "_CGBitmapContextGetWidth",  cg_bmctx_width },
    { "_CGBitmapContextGetHeight", cg_bmctx_height },
    { "_CGColorSpaceCreateDeviceRGB", cg_colorspace_rgb },
    { "_CGColorSpaceCreateWithPlatformColorSpace", cg_colorspace_rgb },
    { "_CGColorCreate",            cg_color_create_comps },
    { "_CGImageCreate",            cg_image_create },
    { "_CGContextSaveGState",      cg_state_save },
    { "_CGContextRestoreGState",   cg_state_restore },
    { NULL, NULL }
};

/* ------------------------------------------------- paths, fonts, text lines */

/* These were placeholders returning NULL, which is worse than useless: a plugin
 * that asks for a path or a text line stores what it gets and then uses it, so
 * NULL faults inside the plugin rather than failing at the call. They are real
 * objects now -- they carry enough state to be queried and released. Nothing
 * here rasterises; the metrics are derived from the font size, which is what a
 * layout needs to reserve space and position controls.
 */
enum { Q_PATH = 16, Q_CTFONT, Q_CTLINE, Q_FRAMESETTER, Q_CTFRAME, Q_CTDESC };

typedef struct { qobj o; CGRect *r; long n, cap; } qpath;
typedef struct { qobj o; double size; char name[64]; } qctfont;
typedef struct { qobj o; double size; char name[64]; } qctdesc;
typedef struct { qobj o; char *text; long len; double size; } qctline;

static void *cg_path_create_mutable(void)
{
    qpath *p = q_new(sizeof *p, Q_PATH);
    if (!p) return NULL;
    p->cap = 8;
    p->r = calloc((size_t)p->cap, sizeof *p->r);
    return p;
}
static void cg_path_add_rect(void *path, const void *m, CGRect r)
{
    qpath *p = (qpath *)q_of(path, Q_PATH);
    (void)m;
    if (!p) return;
    if (p->n == p->cap) {
        long nc = p->cap * 2;
        CGRect *nr = realloc(p->r, (size_t)nc * sizeof *nr);
        if (!nr) return;
        p->r = nr; p->cap = nc;
    }
    p->r[p->n++] = r;
}
/* The rest of the path API, on the same rect list: a point is a degenerate
 * rectangle, and a curve or an arc is the box that contains it. That is enough
 * for the two questions a plugin asks a path -- where it has got to, and how
 * big it is -- without a rasterizer to answer any others. */
static void cg_path_note(qpath *p, double x, double y, double w, double h)
{
    CGRect r;
    r.origin.x = x; r.origin.y = y; r.size.width = w; r.size.height = h;
    cg_path_add_rect(p, NULL, r);
}
static void cg_path_move_to(void *path, const void *m, double x, double y)
{ qpath *p = (qpath *)q_of(path, Q_PATH); (void)m; if (p) cg_path_note(p, x, y, 0, 0); }
static void cg_path_line_to(void *path, const void *m, double x, double y)
{ qpath *p = (qpath *)q_of(path, Q_PATH); (void)m; if (p) cg_path_note(p, x, y, 0, 0); }
static void cg_path_curve_to(void *path, const void *m, double c1x, double c1y,
                             double c2x, double c2y, double x, double y)
{
    qpath *p = (qpath *)q_of(path, Q_PATH);
    (void)m;
    if (!p) return;
    cg_path_note(p, c1x, c1y, 0, 0);
    cg_path_note(p, c2x, c2y, 0, 0);
    cg_path_note(p, x, y, 0, 0);
}
static void cg_path_arc_to(void *path, const void *m, double x, double y, double r,
                           double a0, double a1, int cw)
{
    qpath *p = (qpath *)q_of(path, Q_PATH);
    (void)m; (void)a0; (void)a1; (void)cw;
    if (p) cg_path_note(p, x - r, y - r, 2 * r, 2 * r);
}
static void cg_path_ellipse_in(void *path, const void *m, CGRect r)
{ cg_path_add_rect(path, m, r); }
static void cg_path_close_sub(void *path) { (void)path; }
static CGRect cg_path_bounding(void *path)
{
    qpath *p = (qpath *)q_of(path, Q_PATH);
    CGRect out;
    long i;
    memset(&out, 0, sizeof out);
    if (!p || !p->n) return out;
    {   double x0 = p->r[0].origin.x, y0 = p->r[0].origin.y;
        double x1 = x0 + p->r[0].size.width, y1 = y0 + p->r[0].size.height;
        for (i = 1; i < p->n; i++) {
            const CGRect *r = &p->r[i];
            if (r->origin.x < x0) x0 = r->origin.x;
            if (r->origin.y < y0) y0 = r->origin.y;
            if (r->origin.x + r->size.width  > x1) x1 = r->origin.x + r->size.width;
            if (r->origin.y + r->size.height > y1) y1 = r->origin.y + r->size.height;
        }
        out.origin.x = x0; out.origin.y = y0;
        out.size.width = x1 - x0; out.size.height = y1 - y0; }
    return out;
}
static CGPoint cg_path_current_point(void *path)
{
    qpath *p = (qpath *)q_of(path, Q_PATH);
    CGPoint z;
    memset(&z, 0, sizeof z);
    if (!p || !p->n) return z;
    return p->r[p->n - 1].origin;
}
static unsigned char cg_path_is_empty(void *path)
{ qpath *p = (qpath *)q_of(path, Q_PATH); return (unsigned char)(!p || !p->n); }

/* A CGPath object carries only its bounding box here, so adding one to a
 * context contributes that rectangle -- the shape is lost but the area is not,
 * which keeps a clip honest and a fill in the right place. */
static void cg_add_path(void *ctx, void *path)
{
    qcontext *c = ctx_of(ctx);
    qpath *p = (qpath *)q_of(path, Q_PATH);
    long i;
    if (!c || !p) return;
    for (i = 0; i < p->n; i++) path_rect(c, p->r[i]);
}


static void cg_path_release(void *path)
{
    qpath *p = (qpath *)q_of(path, Q_PATH);
    if (!p || --p->o.refs > 0) return;
    free(p->r); q_free(p);
}

/* Releasing what CoreFoundation is asked to release.
 *
 * A CGImageSourceRef has no typed release -- a plugin frees one with CFRelease,
 * which knew nothing about these objects, so every image an editor decoded
 * stayed for the life of the process. Handled by kind, and only for pointers
 * the registry recognises, so CFRelease of something else is unaffected. */
int macquartz_cf_release(void *p)
{
    switch (q_kind(p)) {
    case Q_SOURCE: {
        qsource *s = (qsource *)q_of(p, Q_SOURCE);
        if (!s || --s->o.refs > 0) return 1;
        cg_image_release(s->img);
        q_free(s);
        return 1; }
    case Q_IMAGE:    cg_image_release(p);    return 1;
    case Q_CONTEXT:  cg_context_release(p);  return 1;
    case Q_PROVIDER: cg_provider_release(p); return 1;
    case Q_COLOR:    cg_color_release(p);    return 1;
    case Q_PATH:     cg_path_release(p);     return 1;
    default:         return 0;
    }
}
int macquartz_cf_retain(void *p)
{
    qobj *o;
    if (!q_kind(p)) return 0;
    o = (qobj *)p;
    o->refs++;
    return 1;
}

/* Everything this shim allocated for the editor that is closing.
 *
 * Freed, not merely forgotten. A VSTGUI editor is several hundred decoded
 * images and the framebuffer they are composited into -- megabytes per plugin
 * -- and a plugin does not reliably release them: CGImageSourceRef has no typed
 * release, and an editor being torn down has better things to do than balance
 * its retains. The editor is gone by the time this is called, so nothing can
 * still be pointing at them.
 *
 * The registry is walked rather than a separate list kept, because it already
 * holds exactly the set in question. */
void macquartz_reset_editor(void)
{
    int i;
    g_editor_ctx = NULL;
    g_host_ctx = NULL;
    pthread_mutex_lock(&g_qreg_lock);
    for (i = 0; i < QREG_BUCKETS; i++) {
        qobj *o = g_qreg[i], *next;
        for (; o; o = next) {
            next = o->reg_next;
            switch (o->kind) {
            case Q_CONTEXT:  { qcontext *c = (qcontext *)o;
                               if (c->owned)
                                   free(c->px);
                               geom_free(&c->path);
                               break; }
            case Q_IMAGE:    { qimage *m = (qimage *)o;
                               if (m->owned)
                                   free(m->px);
                               break; }
            case Q_PROVIDER: { qprovider *q = (qprovider *)o;
                               if (q->owned)
                                   free(q->b);
                               break; }
            case Q_PATH:     free(((qpath *)o)->r); break;
            case Q_SOURCE:   break;   /* its image is freed as an image above */
            default:         break;
            }
            o->magic = 0;
            free(o);
        }
        g_qreg[i] = NULL;
    }
    pthread_mutex_unlock(&g_qreg_lock);
}

/* A font is its family name and its point size. Ascent/descent/leading follow
 * the proportions of a typical text face, which is what a caller reserving line
 * height actually depends on. */
static void *ct_font_create_with_name(void *name, double size, const void *matrix)
{
    qctfont *f = q_new(sizeof *f, Q_CTFONT);
    (void)matrix;
    if (!f) return NULL;
    f->size = size > 0.0 ? size : 12.0;
    if (!name || !macshim_cf_string_get(name, f->name, sizeof f->name))
        snprintf(f->name, sizeof f->name, "Helvetica");
    return f;
}
static double ct_font_ascent(void *font)
{ qctfont *f = (qctfont *)q_of(font, Q_CTFONT); return f ? f->size * 0.80 : 0.0; }
static double ct_font_descent(void *font)
{ qctfont *f = (qctfont *)q_of(font, Q_CTFONT); return f ? f->size * 0.20 : 0.0; }
static double ct_font_leading(void *font)
{ qctfont *f = (qctfont *)q_of(font, Q_CTFONT); return f ? f->size * 0.12 : 0.0; }
static void *ct_font_copy_name(void *font)
{
    qctfont *f = (qctfont *)q_of(font, Q_CTFONT);
    return macshim_cf_string(f ? f->name : "Helvetica");
}

/* ---- font descriptors --------------------------------------------------
 *
 * These returned NULL, and NULL is the one answer a caller cannot check its way
 * out of here: iPlug2 builds a font, asks it for its descriptor, keeps what it
 * gets, and reads a size out of it when a control wants text entry. With
 * nothing to keep, clicking the program name field on MPS read a double from
 * address 8 and took the host down -- a click, on a control, crashing the
 * process.
 *
 * They carry a name and a size, which is what a layout asks them for. Nothing
 * here shapes text; the point is that a plug-in that asks for a descriptor gets
 * an object rather than a hole. */
static void *ct_desc_make(const char *name, double size)
{
    qctdesc *d = q_new(sizeof *d, Q_CTDESC);
    if (!d) return NULL;
    d->size = size > 0.0 ? size : 12.0;
    snprintf(d->name, sizeof d->name, "%s", name && *name ? name : "Helvetica");
    return d;
}
/* CGFont carries no name here -- it is the font's bytes and nothing else -- so
 * the face is left generic and the size, which is what gets read back, is
 * right. */
static void *ct_font_with_graphics_font(void *cgfont, double size,
                                        const void *matrix, void *attrs)
{
    qctfont *f = q_new(sizeof *f, Q_CTFONT);
    (void)cgfont; (void)matrix; (void)attrs;
    if (!f) return NULL;
    f->size = size > 0.0 ? size : 12.0;
    snprintf(f->name, sizeof f->name, "Helvetica");
    return f;
}
static void *ct_font_copy_descriptor(void *font)
{
    qctfont *f = (qctfont *)q_of(font, Q_CTFONT);
    return ct_desc_make(f ? f->name : NULL, f ? f->size : 0.0);
}
/* The attributes are not read: nothing here shapes text, so the face named in
 * them would not change a glyph. What matters is that the caller is handed a
 * descriptor it can keep. */
static void *ct_desc_with_attributes(void *attrs)
{ (void)attrs; return ct_desc_make(NULL, 0.0); }
static void *ct_desc_copy_attribute(void *desc, void *key)
{
    qctdesc *d = (qctdesc *)q_of(desc, Q_CTDESC);
    (void)key;
    return macshim_cf_string(d ? d->name : "Helvetica");
}
static void *ct_descs_matching(void *desc, void *set)
{
    const void *one[1];
    (void)set;
    if (!q_of(desc, Q_CTDESC)) return NULL;
    one[0] = desc;
    ((qobj *)desc)->refs++;
    return macshim_cf_array(one, 1);
}

/* A line, a framesetter and a frame all carry the same thing here: the text and
 * the size it will be measured at. */
static void *line_make(void *attrstr, double size)
{
    qctline *l = q_new(sizeof *l, Q_CTLINE);
    char buf[1024];
    if (!l) return NULL;
    l->size = size > 0.0 ? size : 12.0;
    if (attrstr && macshim_cf_string_get(attrstr, buf, sizeof buf)) {
        l->len = (long)strlen(buf);
        l->text = malloc((size_t)l->len + 1);
        if (l->text) memcpy(l->text, buf, (size_t)l->len + 1);
    }
    return l;
}
static void *ct_line_create(void *attrstr) { return line_make(attrstr, 12.0); }
static void *ct_framesetter_create(void *attrstr) { return line_make(attrstr, 12.0); }
static void *ct_framesetter_create_frame(void *fs, void *range, void *path, void *attrs)
{ (void)range; (void)path; (void)attrs; return fs ? (void *)macshim_lookup_retain(fs) : NULL; }
static void *ct_frame_get_lines(void *frame)
{
    /* One line, which is what a single-line label produces -- and every use of
     * a framesetter in this corpus is a label. */
    const void *v[1];
    v[0] = frame;
    return macshim_cf_array(v, frame ? 1 : 0);
}

/* Width in points, with the vertical metrics as out-parameters. An average
 * advance of half the point size is close enough for a proportional face to lay
 * out without overlapping. */
static double ct_line_bounds(void *line, double *ascent, double *descent,
                             double *leading)
{
    qctline *l = (qctline *)q_of(line, Q_CTLINE);
    double size = l ? l->size : 12.0;
    if (ascent)  *ascent  = size * 0.80;
    if (descent) *descent = size * 0.20;
    if (leading) *leading = size * 0.12;
    return l ? (double)l->len * size * 0.5 : 0.0;
}

/* No device transform is in play, so device space is user space. */
static CGSize cg_convert_size(void *ctx, CGSize s) { (void)ctx; return s; }

const macshim_entry macshim_quartz3[] = {
    { "_CGPathCreateMutable",   cg_path_create_mutable },
    { "_CGPathAddRect",         cg_path_add_rect },
    { "_CGPathRelease",         cg_path_release },
    { "_CGPathMoveToPoint",     cg_path_move_to },
    { "_CGPathAddLineToPoint",  cg_path_line_to },
    { "_CGPathAddCurveToPoint", cg_path_curve_to },
    { "_CGPathAddArc",          cg_path_arc_to },
    { "_CGPathAddEllipseInRect", cg_path_ellipse_in },
    { "_CGPathCloseSubpath",    cg_path_close_sub },
    { "_CGPathGetBoundingBox",  cg_path_bounding },
    { "_CGPathGetCurrentPoint", cg_path_current_point },
    { "_CGPathIsEmpty",         cg_path_is_empty },
    { "_CTFontCreateWithName",  ct_font_create_with_name },
    { "_CTFontCreateWithGraphicsFont", ct_font_with_graphics_font },
    { "_CTFontCopyFontDescriptor",     ct_font_copy_descriptor },
    { "_CTFontDescriptorCreateWithAttributes", ct_desc_with_attributes },
    { "_CTFontDescriptorCopyAttribute",        ct_desc_copy_attribute },
    { "_CTFontDescriptorCreateMatchingFontDescriptors", ct_descs_matching },
    { "_CTFontGetAscent",       ct_font_ascent },
    { "_CTFontGetDescent",      ct_font_descent },
    { "_CTFontGetLeading",      ct_font_leading },
    { "_CTFontCopyDisplayName", ct_font_copy_name },
    { "_CTFontCopyPostScriptName", ct_font_copy_name },
    { "_CTLineCreateWithAttributedString", ct_line_create },
    { "_CTFramesetterCreateWithAttributedString", ct_framesetter_create },
    { "_CTFramesetterCreateFrame", ct_framesetter_create_frame },
    { "_CTFrameGetLines",       ct_frame_get_lines },
    { "_CTLineGetTypographicBounds", ct_line_bounds },
    { "_CGContextConvertSizeToDeviceSpace", cg_convert_size },
    { NULL, NULL }
};
