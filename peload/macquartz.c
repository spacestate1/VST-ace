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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macshim.h"

typedef struct { double x, y; } CGPoint;
typedef struct { double width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;

/* Shared header so retain/release can be uniform. */
enum { Q_CONTEXT = 1, Q_IMAGE, Q_COLOR, Q_PROVIDER, Q_FONT, Q_COLORSPACE };
typedef struct { uint32_t magic, kind; long refs; } qobj;
#define QMAGIC 0x51475458u          /* 'QGTX' */

static void *q_new(size_t sz, int kind)
{
    qobj *o = calloc(1, sz);
    if (!o) return NULL;
    o->magic = QMAGIC;
    o->kind = (uint32_t)kind;
    o->refs = 1;
    return o;
}
static qobj *q_of(const void *p, int kind)
{
    qobj *o = (qobj *)p;
    return (o && o->magic == QMAGIC && (int)o->kind == kind) ? o : NULL;
}

typedef struct {
    qobj o;
    uint8_t *px;                    /* caller's buffer, or ours */
    int owned;
    size_t w, h, stride, bpc, bpp;
    uint32_t bitmapinfo;
} qimage;

typedef struct {
    qobj o;
    uint8_t *px;
    int owned;
    size_t w, h, stride;
    uint32_t bitmapinfo;
    double shadow[4];
} qcontext;

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

void macquartz_reset_editor(void) { g_editor_ctx = NULL; }

static void *cg_bitmap_context_create(void *data, size_t w, size_t h,
                                      size_t bpc, size_t stride,
                                      void *space, uint32_t info)
{
    qcontext *c = q_new(sizeof *c, Q_CONTEXT);
    (void)space;
    if (!c) return NULL;
    if (!stride) stride = w * 4;
    c->w = w; c->h = h; c->stride = stride; c->bitmapinfo = info;
    /* Remember the biggest one: an editor that draws through Core Graphics rather
     * than Metal makes exactly one bitmap context the size of its window and
     * composites everything into it, so that context *is* the framebuffer. Any
     * others a plugin makes are scratch and smaller. */
    if (!g_editor_ctx || (size_t)w * h > g_editor_ctx->w * g_editor_ctx->h)
        g_editor_ctx = c;
    if (getenv("MACQZ_VERBOSE"))
        fprintf(stderr, "  [qz] CGBitmapContextCreate %zux%zu stride=%zu data=%p\n",
                w, h, stride, data);
    if (data) {
        c->px = data;
    } else {
        c->px = calloc(1, stride * (h ? h : 1));
        c->owned = 1;
        if (!c->px) { free(c); return NULL; }
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
    free(c);
}

/* Nearest-neighbour blit into the destination rect. Enough to composite an
 * image a plugin has prepared; a real resampler is a later concern. */
static void cg_context_draw_image(void *ctx, CGRect r, void *img)
{
    qcontext *c = (qcontext *)q_of(ctx, Q_CONTEXT);
    qimage *im = (qimage *)q_of(img, Q_IMAGE);
    long dx0, dy0, dx1, dy1, x, y;

    if (getenv("MACQZ_VERBOSE"))
        fprintf(stderr, "  [qz] CGContextDrawImage ctx=%p(%zux%zu) img=%p(%zux%zu) "
                        "at %.0f,%.0f %.0fx%.0f\n",
                (void *)c, c ? c->w : 0, c ? c->h : 0, (void *)im,
                im ? im->w : 0, im ? im->h : 0,
                r.origin.x, r.origin.y, r.size.width, r.size.height);
    if (!c || !im || !c->px || !im->px || !im->w || !im->h) return;
    dx0 = (long)r.origin.x; dy0 = (long)r.origin.y;
    dx1 = dx0 + (long)r.size.width; dy1 = dy0 + (long)r.size.height;
    if (dx0 < 0) dx0 = 0; if (dy0 < 0) dy0 = 0;
    if (dx1 > (long)c->w) dx1 = (long)c->w;
    if (dy1 > (long)c->h) dy1 = (long)c->h;

    for (y = dy0; y < dy1; y++) {
        long sy = (dy1 - dy0) ? (y - dy0) * (long)im->h / (dy1 - dy0) : 0;
        /* CoreGraphics has the origin at the bottom left; a bitmap row 0 is the
         * top, so the source row is flipped. */
        const uint8_t *srow = im->px + (size_t)(im->h - 1 - (size_t)sy) * im->stride;
        uint8_t *drow = c->px + (size_t)y * c->stride;
        for (x = dx0; x < dx1; x++) {
            long sx = (dx1 - dx0) ? (x - dx0) * (long)im->w / (dx1 - dx0) : 0;
            memcpy(drow + x * 4, srow + sx * 4, 4);
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
    free(i);
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
    if (c && --c->o.refs <= 0) free(c);
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
    if (!p->b) { free(p); return NULL; }
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
    if (!p->b) { free(p); fclose(f); return NULL; }
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
    free(q);
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
static size_t cg_display_pixels_high(uint32_t d) { (void)d; return 1080; }
static int  cg_display_hide_cursor(uint32_t d) { (void)d; return 0; }
static int  cg_display_show_cursor(uint32_t d) { (void)d; return 0; }
static int  cg_display_move_cursor(uint32_t d, CGPoint p) { (void)d; (void)p; return 0; }
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

/* Graphics state stack. Depth only -- nothing here transforms yet, but a plugin
 * that saves and restores must see balanced calls rather than a failure. */
static int g_gstate_depth;
static void cg_gstate_push(void *ctx) { (void)ctx; g_gstate_depth++; }
static void cg_gstate_pop(void *ctx)
{ (void)ctx; if (g_gstate_depth > 0) g_gstate_depth--; }

/* The drawing calls. Each accepts its arguments and returns; a path or a fill
 * that goes nowhere is correct for a headless render. */
static void cg_noop(void) { }
static void *cg_noop_ptr(void) { return NULL; }
static double cg_noop_double(void) { return 0.0; }

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
const macshim_entry macshim_quartz2[] = {
    { "_CFAttributedStringCreate", cg_noop_ptr },
    { "_CFUserNotificationDisplayAlert", cg_noop },
    { "_CGContextAddCurveToPoint", cg_noop },
    { "_CGContextAddLineToPoint", cg_noop },
    { "_CGContextBeginPath", cg_noop },
    { "_CGContextClearRect", cg_noop },
    { "_CGContextClipToRect", cg_noop },
    { "_CGContextDrawPath", cg_noop },
    { "_CGContextFillEllipseInRect", cg_noop },
    { "_CGContextFillRect", cg_noop },
    { "_CGContextMoveToPoint", cg_noop },
    { "_CGContextScaleCTM", cg_noop },
    { "_CGContextSetFillColorWithColor", cg_noop },
    { "_CGContextSetInterpolationQuality", cg_noop },
    { "_CGContextSetLineWidth", cg_noop },
    { "_CGContextSetRGBStrokeColor", cg_noop },
    { "_CGContextSetShouldAntialias", cg_noop },
    { "_CGContextSetStrokeColorWithColor", cg_noop },
    { "_CGContextSetTextMatrix", cg_noop },
    { "_CGContextSetTextPosition", cg_noop },
    { "_CGContextStrokeEllipseInRect", cg_noop },
    { "_CGContextStrokePath", cg_noop },
    { "_CGContextStrokeRectWithWidth", cg_noop },
    { "_CGContextTranslateCTM", cg_noop },
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
    { "_CGContextSaveGState",      cg_gstate_push },
    { "_CGContextRestoreGState",   cg_gstate_pop },
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
enum { Q_PATH = 16, Q_CTFONT, Q_CTLINE, Q_FRAMESETTER, Q_CTFRAME };

typedef struct { qobj o; CGRect *r; long n, cap; } qpath;
typedef struct { qobj o; double size; char name[64]; } qctfont;
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
static void cg_path_release(void *path)
{
    qpath *p = (qpath *)q_of(path, Q_PATH);
    if (!p || --p->o.refs > 0) return;
    free(p->r); free(p);
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
    { "_CTFontCreateWithName",  ct_font_create_with_name },
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
