/* Metal, on the CPU.
 *
 * Eighteen of the nineteen macOS plugins here draw their editor through
 * NanoVG's Metal backend, so on macOS a GUI *means* Metal -- none of them has an
 * OpenGL path compiled in. Emulating a GPU would be hopeless; what makes this
 * tractable is that the shaders are not arbitrary. nanovg_mtl ships exactly one
 * vertex shader and one fragment shader, and their behaviour is fixed by
 * nanovg's own source. So the precompiled Metal bitcode the plugin hands to
 * newLibraryWithData: is ignored, the pipeline it builds is recognised by the
 * state it sets, and the draw calls are rasterized here in C.
 *
 * The vertex shader is what makes it cheap. It is
 *
 *     position = (2*pos.x/viewSize.x - 1, 1 - 2*pos.y/viewSize.y, 0, 1)
 *     fpos     = pos      (passed through, in pixels)
 *     ftcoord  = tcoord   (passed through)
 *
 * -- a pure viewport transform. Since the viewport always equals viewSize, a
 * vertex's pixel coordinate *is* its position attribute, so triangles are
 * rasterized directly in attribute space with no matrix work at all.
 *
 * What still has to be real: the stencil buffer (nanovg fills concave paths by
 * counting winding into stencil, then covering), premultiplied-alpha blending,
 * and the fragment shader's gradient/image/scissor maths.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macobjc.h"
#include "macshim.h"

typedef void *id;
typedef const char *SEL;

/* ------------------------------------------------------------ Metal enums */

enum { PF_INVALID = 0, PF_A8 = 1, PF_R8 = 10, PF_RGBA8 = 70, PF_BGRA8 = 80,
       PF_STENCIL8 = 253 };

enum { BF_ZERO = 0, BF_ONE = 1, BF_SRC_COLOR = 2, BF_1M_SRC_COLOR = 3,
       BF_SRC_ALPHA = 4, BF_1M_SRC_ALPHA = 5, BF_DST_COLOR = 6,
       BF_1M_DST_COLOR = 7, BF_DST_ALPHA = 8, BF_1M_DST_ALPHA = 9 };

enum { CMP_NEVER = 0, CMP_LESS = 1, CMP_EQUAL = 2, CMP_LEQUAL = 3,
       CMP_GREATER = 4, CMP_NOTEQUAL = 5, CMP_GEQUAL = 6, CMP_ALWAYS = 7 };

enum { SOP_KEEP = 0, SOP_ZERO = 1, SOP_REPLACE = 2, SOP_INCR_CLAMP = 3,
       SOP_DECR_CLAMP = 4, SOP_INVERT = 5, SOP_INCR_WRAP = 6, SOP_DECR_WRAP = 7 };

enum { PRIM_POINT = 0, PRIM_LINE = 1, PRIM_LINESTRIP = 2, PRIM_TRI = 3,
       PRIM_TRISTRIP = 4 };

enum { LOAD_DONTCARE = 0, LOAD_LOAD = 1, LOAD_CLEAR = 2 };

/* nanovg's shader selector, from nanovg_mtl's MNVGshaderType. */
enum { SH_FILLGRAD = 0, SH_FILLIMG = 1, SH_IMG = 2 };

typedef struct { double x, y, w, h, znear, zfar; } MTLViewport;
typedef struct { double r, g, b, a; }               MTLClearColor;
typedef struct { unsigned long w, h, d; }           MTLSize;
typedef struct { unsigned long x, y, z; }           MTLOrigin;
typedef struct { MTLOrigin origin; MTLSize size; }  MTLRegion;
typedef struct { double w, h; }                     CGSize;

/* --------------------------------------------------------- object model */

#define OBJ void *isa; long refs

typedef struct { OBJ; } mtl_obj;

typedef struct { OBJ; void *data; unsigned long len; } mtl_buffer;

typedef struct {
    OBJ;
    int w, h, fmt, bpp;
    uint8_t *px;                      /* tightly packed, w*h*bpp */
} mtl_texture;

typedef struct { OBJ; char name[64]; } mtl_named;

/* A pipeline: nanovg only ever varies blend state and whether a fragment shader
 * runs at all. The stencil-only pipeline is the one built with a nil fragment
 * function, and recognising it is how a stencil pass is told from a colour one. */
typedef struct {
    OBJ;
    int has_frag, blend, write_mask;
    int src_rgb, dst_rgb, src_a, dst_a;
    int pixfmt;
} mtl_pipeline;

typedef struct {
    OBJ;
    int cmp, sfail, dpfail, dppass;
    unsigned rmask, wmask;
} mtl_stencildesc;

typedef struct {
    OBJ;
    mtl_stencildesc *front, *back;
    int depth_cmp;
} mtl_dsstate;

typedef struct { OBJ; int min_f, mag_f, s_addr, t_addr; } mtl_sampler;

typedef struct {
    OBJ;
    mtl_texture *tex;
    int load, store, pixfmt;
    MTLClearColor clear;
    unsigned clear_stencil;
    /* pipeline-attachment fields, when this stands in for one of those */
    int blend, src_rgb, dst_rgb, src_a, dst_a;
    int write_mask;                   /* -1 until the plugin sets one */
} mtl_attach;

typedef struct { OBJ; mtl_attach *a[4]; } mtl_attacharr;

typedef struct {
    OBJ;
    mtl_attacharr *color;
    mtl_attach    *stencil;
} mtl_rpdesc;

typedef struct {
    OBJ;
    void *vfn, *ffn, *vdesc;
    mtl_attacharr *color;
    int stencil_pixfmt;
} mtl_pipedesc;

typedef struct { OBJ; int fmt, w, h, mip, usage, storage; } mtl_texdesc;

typedef struct {
    OBJ;
    void *handlers[8];
    int   nh;
    int   committed;
} mtl_cmdbuf;

typedef struct {
    OBJ;
    mtl_texture  *color, *sten;
    mtl_pipeline *pipe;
    mtl_dsstate  *ds;
    unsigned      sref;
    double        vx, vy, vw, vh;
    mtl_buffer   *vbuf[4]; unsigned long voff[4];
    mtl_buffer   *fbuf[4]; unsigned long foff[4];
    mtl_texture  *ftex[4];
    mtl_sampler  *fsamp;
    float         vsw, vsh;        /* the shader's viewSize, in points */
} mtl_encoder;

typedef struct { OBJ; mtl_texture *tex; } ca_drawable;

typedef struct {
    OBJ;
    void        *device;
    mtl_texture *back;
    int          w, h, pixfmt, opaque;
    double       scale;
} ca_layer;

/* Every one of these lives in an instance allocated by the objc runtime, which
 * gives a stand-in class 512 bytes of payload. Cheap to assert, and the failure
 * mode without it is a silent overwrite of the next heap block. */
_Static_assert(sizeof(mtl_encoder) <= 512 + 16, "encoder outgrows the instance");
_Static_assert(sizeof(ca_layer)    <= 512 + 16, "layer outgrows the instance");

/* nanovg's per-draw uniform block. Metal lays matrix_float3x3 out as three
 * float4 columns, which is why the matrices are 48 bytes and not 36. */
typedef struct {
    float scissor_mat[12];
    float paint_mat[12];
    float inner[4];
    float outer[4];
    float scissor_ext[2];
    float scissor_scale[2];
    float extent[2];
    float radius, feather;
    float stroke_mult, stroke_thr;
    int   tex_type;
    int   type;
} nvg_uniforms;

_Static_assert(sizeof(nvg_uniforms) == 176, "MNVGfragUniforms layout changed");

/* ---------------------------------------------------------------- helpers */

static int verbose(void)
{ static int v = -1;
  if (v < 0) { const char *e = getenv("MACMETAL_VERBOSE"); v = e && *e != '0'; }
  return v; }

static id new_of(const char *cls)
{
    void *k = macobjc_define_class(cls);
    id (*alloc)(id, SEL);
    if (!k) return NULL;
    alloc = (id (*)(id, SEL))macobjc_lookup(k, "alloc");
    return alloc ? alloc(k, "alloc") : NULL;
}

static int bytes_per_pixel(int fmt)
{
    switch (fmt) {
    case PF_A8: case PF_R8: case PF_STENCIL8: return 1;
    case PF_RGBA8: case PF_BGRA8:             return 4;
    default:                                  return 4;
    }
}

/* Where R, G and B sit within a 4-byte pixel of this format. Getting this from
 * the format rather than assuming BGRA matters because nanovg renders into an
 * RGBA8 offscreen texture and then samples it: writing BGRA into a surface that
 * is read back as RGBA swaps red and blue across the whole editor. */
static void chan_order(int fmt, int *r, int *g, int *b, int *a)
{
    if (fmt == PF_BGRA8) { *b = 0; *g = 1; *r = 2; *a = 3; }
    else                 { *r = 0; *g = 1; *b = 2; *a = 3; }
}

/* ---- what the editor allocated -----------------------------------------
 *
 * A texture or a buffer is an Objective-C object, and objects here are retired
 * rather than freed -- see macobjc.c for why. Their *payloads* are not objects
 * though: they are plain allocations, and a font atlas plus a drawable plus the
 * vertex and uniform buffers come to megabytes per editor. Nothing freed them,
 * so opening one plugin after another grew the heap by about six megabytes a
 * time -- a hundred plug-ins into a browsing session, half a gigabyte.
 *
 * They are recorded here and released by macmetal_reset, which the host already
 * calls when a plugin closes and which already did exactly this for the layer's
 * back buffer. The objects themselves stay retired; it is the pixels that
 * mattered. */
#define MAX_PAYLOAD 4096
static void   *g_payload[MAX_PAYLOAD];
static int     g_npayload;

static void *payload_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) return NULL;
    if (g_npayload < MAX_PAYLOAD) g_payload[g_npayload++] = p;
    return p;
}
static void payload_forget(void *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < g_npayload; i++)
        if (g_payload[i] == p) { g_payload[i] = g_payload[--g_npayload]; return; }
}
static void payload_free_all(void)
{
    int i;
    for (i = 0; i < g_npayload; i++) free(g_payload[i]);
    g_npayload = 0;
}

static mtl_texture *tex_make(int fmt, int w, int h)
{
    mtl_texture *t = (mtl_texture *)new_of("MTLTexture");
    if (!t) return NULL;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    t->fmt = fmt; t->w = w; t->h = h;
    t->bpp = bytes_per_pixel(fmt);
    t->px = payload_alloc((size_t)w * (size_t)h * (size_t)t->bpp);
    return t;
}

/* -------------------------------------------------------- the framebuffer */

/* The layer's back texture is the editor's framebuffer: the host reads it
 * straight out, so presentDrawable has nothing to copy. */
static ca_layer *g_layer;

int macmetal_pixels(const unsigned int **px, int *w, int *h)
{
    static unsigned int *conv;
    static size_t convn;
    mtl_texture *t = g_layer ? g_layer->back : NULL;

    if (!t || !t->px || t->bpp != 4) return 0;
    if (w) *w = t->w;
    if (h) *h = t->h;
    if (!px) return 1;

    if (t->fmt == PF_BGRA8) {           /* already what the host reads */
        *px = (const unsigned int *)t->px;
        return 1;
    }
    /* Everything downstream -- the X11 blit, the PNG writer, the bridge -- reads
     * 32-bit BGRX, so a layer that chose RGBA8 gets converted once per frame. */
    { size_t n = (size_t)t->w * t->h, i;
      if (n > convn) { free(conv); conv = malloc(n * 4); convn = conv ? n : 0; }
      if (!conv) return 0;
      for (i = 0; i < n; i++) {
          const uint8_t *p = t->px + i * 4;
          conv[i] = ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16)
                  | ((unsigned)p[1] << 8) | p[2];
      }
      *px = conv; }
    return 1;
}

int macmetal_active(void) { return g_layer && g_layer->back; }

/* Forget the current layer. Called when a plugin closes: the layer object lives
 * in that plugin's world, and keeping it would hand the next plugin a
 * framebuffer sized for the last one. */
void macmetal_reset(void)
{
    if (g_layer && g_layer->back) {
        payload_forget(g_layer->back->px);
        free(g_layer->back->px);
        g_layer->back->px = NULL;
        g_layer->back = NULL;
    }
    g_layer = NULL;
    /* And everything else the editor's textures and buffers were holding. */
    payload_free_all();
}

void macmetal_set_size(int w, int h)
{
    if (!g_layer) return;
    if (g_layer->back && g_layer->back->w == w && g_layer->back->h == h) return;
    g_layer->w = w; g_layer->h = h;
    if (g_layer->back) { free(g_layer->back->px); g_layer->back->px = NULL; }
    g_layer->back = tex_make(g_layer->pixfmt ? g_layer->pixfmt : PF_BGRA8, w, h);
    if (verbose()) fprintf(stderr, "  [mtl] host-sized drawable %dx%d fmt=%d\n",
                           w, h, g_layer->pixfmt);
}

/* ------------------------------------------------------- the rasterizer */

/* Blend factor applied to one channel. src/dst are premultiplied 0..1. */
static float blend_factor(int f, float sc, float sa, float dc, float da)
{
    switch (f) {
    case BF_ZERO:          return 0.0f;
    case BF_ONE:           return 1.0f;
    case BF_SRC_COLOR:     return sc;
    case BF_1M_SRC_COLOR:  return 1.0f - sc;
    case BF_SRC_ALPHA:     return sa;
    case BF_1M_SRC_ALPHA:  return 1.0f - sa;
    case BF_DST_COLOR:     return dc;
    case BF_1M_DST_COLOR:  return 1.0f - dc;
    case BF_DST_ALPHA:     return da;
    case BF_1M_DST_ALPHA:  return 1.0f - da;
    default:               return 1.0f;
    }
}

static int stencil_pass(int cmp, unsigned ref, unsigned val, unsigned mask)
{
    unsigned r = ref & mask, v = val & mask;
    switch (cmp) {
    case CMP_NEVER:    return 0;
    case CMP_LESS:     return r <  v;
    case CMP_EQUAL:    return r == v;
    case CMP_LEQUAL:   return r <= v;
    case CMP_GREATER:  return r >  v;
    case CMP_NOTEQUAL: return r != v;
    case CMP_GEQUAL:   return r >= v;
    default:           return 1;
    }
}

static unsigned stencil_op(int op, unsigned val, unsigned ref)
{
    switch (op) {
    case SOP_ZERO:       return 0;
    case SOP_REPLACE:    return ref;
    case SOP_INCR_CLAMP: return val < 255 ? val + 1 : 255;
    case SOP_DECR_CLAMP: return val > 0 ? val - 1 : 0;
    case SOP_INVERT:     return ~val & 0xff;
    case SOP_INCR_WRAP:  return (val + 1) & 0xff;
    case SOP_DECR_WRAP:  return (val - 1) & 0xff;
    default:             return val;
    }
}

static float clampf(float v, float lo, float hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

/* nanovg's scissor mask: the paint's own clip rectangle, with a one-pixel
 * feather at the edges. */
static float scissor_mask(const nvg_uniforms *u, float px, float py)
{
    /* A float3x3 in Metal is three float4 columns, so column j starts at 4*j
     * and the row is the element within it. */
    float x = u->scissor_mat[0] * px + u->scissor_mat[4] * py + u->scissor_mat[8];
    float y = u->scissor_mat[1] * px + u->scissor_mat[5] * py + u->scissor_mat[9];
    float sx = fabsf(x) - u->scissor_ext[0];
    float sy = fabsf(y) - u->scissor_ext[1];
    sx = 0.5f - sx * u->scissor_scale[0];
    sy = 0.5f - sy * u->scissor_scale[1];
    return clampf(sx, 0.0f, 1.0f) * clampf(sy, 0.0f, 1.0f);
}

static float stroke_mask(const nvg_uniforms *u, float tu, float tv)
{
    float a = (1.0f - fabsf(tu * 2.0f - 1.0f)) * u->stroke_mult;
    if (a > 1.0f) a = 1.0f;
    return a * (tv < 1.0f ? tv : 1.0f);
}

/* Signed distance to a rounded rectangle, as nanovg defines it. */
static float sd_roundrect(float px, float py, float ex, float ey, float rad)
{
    float dx = fabsf(px) - (ex - rad);
    float dy = fabsf(py) - (ey - rad);
    float mx = dx > dy ? dx : dy;
    float ox = dx > 0.0f ? dx : 0.0f;
    float oy = dy > 0.0f ? dy : 0.0f;
    return (mx < 0.0f ? mx : 0.0f) + sqrtf(ox * ox + oy * oy) - rad;
}

/* Sample a texture with the semantics nanovg's shader expects: the result is
 * premultiplied RGBA in 0..1. */
static void sample_tex(const mtl_texture *t, const mtl_sampler *s,
                       float u, float v, int tex_type, float out[4])
{
    int x, y;
    const uint8_t *p;

    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!t || !t->px) return;

    /* Clamp rather than repeat: every nanovg image paint and the font atlas are
     * addressed inside their extent, and repeating would smear edges. */
    (void)s;
    x = (int)floorf(u * (float)t->w);
    y = (int)floorf(v * (float)t->h);
    if (x < 0) x = 0;
    if (x >= t->w) x = t->w - 1;
    if (y < 0) y = 0;
    if (y >= t->h) y = t->h - 1;
    p = t->px + ((size_t)y * t->w + x) * t->bpp;

    if (t->bpp == 1) {
        /* An alpha-only image: the font atlas. */
        out[0] = out[1] = out[2] = out[3] = p[0] / 255.0f;
        return;
    }
    { int ri, gi, bi, ai;
      chan_order(t->fmt, &ri, &gi, &bi, &ai);
      out[0] = p[ri] / 255.0f; out[1] = p[gi] / 255.0f;
      out[2] = p[bi] / 255.0f; out[3] = p[ai] / 255.0f; }
    if (tex_type == 1) {               /* straight alpha -> premultiply */
        out[0] *= out[3]; out[1] *= out[3]; out[2] *= out[3];
    } else if (tex_type == 2) {        /* single channel */
        out[1] = out[2] = out[3] = out[0];
    }
}

/* nanovg's fragment shader. Returns 0 to discard. */
static int frag_shader(const nvg_uniforms *u, const mtl_texture *tex,
                       const mtl_sampler *samp,
                       float fx, float fy, float tu, float tv, float out[4])
{
    float scissor = scissor_mask(u, fx, fy);
    float stroke, c[4];
    int i;

    if (scissor <= 0.0f) return 0;

    if (u->type == SH_FILLIMG) {
        /* An image paint: the paint matrix maps pixel space into the image's
         * extent, and the divide turns that into normalised coordinates. */
        float px = u->paint_mat[0] * fx + u->paint_mat[4] * fy + u->paint_mat[8];
        float py = u->paint_mat[1] * fx + u->paint_mat[5] * fy + u->paint_mat[9];
        float ex = u->extent[0] != 0.0f ? u->extent[0] : 1.0f;
        float ey = u->extent[1] != 0.0f ? u->extent[1] : 1.0f;
        stroke = stroke_mask(u, tu, tv);
        if (stroke < u->stroke_thr) return 0;
        sample_tex(tex, samp, px / ex, py / ey, u->tex_type, c);
        for (i = 0; i < 4; i++) c[i] *= u->inner[i];
        for (i = 0; i < 4; i++) out[i] = c[i] * stroke * scissor;
        return 1;
    }
    if (u->type == SH_IMG) {
        /* Text and blitted images address the texture through the vertex uv. */
        stroke = stroke_mask(u, tu, tv);
        if (stroke < u->stroke_thr) return 0;
        sample_tex(tex, samp, tu, tv, u->tex_type, c);
        for (i = 0; i < 4; i++) c[i] *= u->inner[i];
        for (i = 0; i < 4; i++) out[i] = c[i] * scissor;
        return 1;
    }

    /* A gradient (or a flat colour, which nanovg expresses as a gradient whose
     * two stops are equal). */
    {
        float px = u->paint_mat[0] * fx + u->paint_mat[4] * fy + u->paint_mat[8];
        float py = u->paint_mat[1] * fx + u->paint_mat[5] * fy + u->paint_mat[9];
        float d, f = u->feather != 0.0f ? u->feather : 1e-6f;
        stroke = stroke_mask(u, tu, tv);
        if (stroke < u->stroke_thr) return 0;
        d = sd_roundrect(px, py, u->extent[0], u->extent[1], u->radius);
        d = clampf((d + f * 0.5f) / f, 0.0f, 1.0f);
        for (i = 0; i < 4; i++)
            c[i] = u->inner[i] + (u->outer[i] - u->inner[i]) * d;
        for (i = 0; i < 4; i++) out[i] = c[i] * stroke * scissor;
        return 1;
    }
}

typedef struct { float x, y, u, v; } vtx;

/* Rasterizer cost, so "the software renderer is slow" can be a number rather
 * than a feeling. Counted here because only this layer knows how much of a frame
 * was actually shaded. */
static unsigned long g_tris, g_shaded;
static double        g_raster_ms;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

void macmetal_stats(unsigned long *tris, unsigned long *shaded, double *ms)
{
    if (tris)   *tris   = g_tris;
    if (shaded) *shaded = g_shaded;
    if (ms)     *ms     = g_raster_ms;
    g_tris = g_shaded = 0;
    g_raster_ms = 0.0;
}

/* One triangle, with the whole pipeline applied per pixel.
 *
 * The per-pixel work is in raster_rows so that a big triangle can be split by
 * scanline across cores -- see draw_bands below. Everything the inner loop
 * needs that does not vary from row to row is gathered here first. */
typedef struct {
    mtl_encoder     *e;
    const nvg_uniforms *u;
    mtl_texture     *ct, *st;
    mtl_pipeline    *pipe;
    mtl_stencildesc *sd;
    vtx    a, b, c;
    float  area, sx, sy, ox, oy;
    int    x0, x1;
} tri_ctx;

static void raster_rows(const tri_ctx *t, int y0, int y1, unsigned long *shaded);

typedef struct { unsigned long tris, shaded; } raster_stats;

static void raster_tri(mtl_encoder *e, const nvg_uniforms *u,
                       const vtx *a, const vtx *b, const vtx *c,
                       int by0, int by1, raster_stats *rs)
{
    mtl_texture *ct = e->color, *st = e->sten;
    mtl_pipeline *pipe = e->pipe;
    mtl_dsstate *ds = e->ds;
    mtl_stencildesc *sd = ds ? ds->front : NULL;
    float minx, maxx, miny, maxy, area;
    int x0, x1, y0, y1;
    int clipx0, clipy0, clipx1, clipy1;
    vtx A, B, C;
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;

    if (!ct || !ct->px) return;
    /* Counted once per triangle, by whichever band starts at the top -- the
     * others are the same triangle seen again. */
    if (by0 == 0) rs->tris++;

    /* nanovg works in points and hands the shader a viewSize to divide by, while
     * the viewport is in pixels -- the difference is the backing scale factor.
     * Composing the shader's NDC step with the viewport transform collapses to a
     * scale and offset, which is applied here so everything below is in pixels.
     *
     *   ndc.x = 2*pos.x/vsw - 1        x_px = vx + (ndc.x+1)/2 * vw
     *   ndc.y = 1 - 2*pos.y/vsh        y_px = vy + (1-ndc.y)/2 * vh
     */
    if (e->vsw > 0.0f && e->vw > 0.0) { sx = (float)e->vw / e->vsw; ox = (float)e->vx; }
    if (e->vsh > 0.0f && e->vh > 0.0) { sy = (float)e->vh / e->vsh; oy = (float)e->vy; }
    A = *a; B = *b; C = *c;
    A.x = A.x * sx + ox; A.y = A.y * sy + oy;
    B.x = B.x * sx + ox; B.y = B.y * sy + oy;
    C.x = C.x * sx + ox; C.y = C.y * sy + oy;
    a = &A; b = &B; c = &C;

    area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0.0f) return;

    minx = fminf(a->x, fminf(b->x, c->x)); maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    miny = fminf(a->y, fminf(b->y, c->y)); maxy = fmaxf(a->y, fmaxf(b->y, c->y));

    /* The viewport clips, and so does the target. */
    clipx0 = (int)floorf((float)e->vx);       clipy0 = (int)floorf((float)e->vy);
    clipx1 = (int)ceilf((float)(e->vx + e->vw)); clipy1 = (int)ceilf((float)(e->vy + e->vh));
    if (e->vw <= 0.0) { clipx0 = 0; clipx1 = ct->w; }
    if (e->vh <= 0.0) { clipy0 = 0; clipy1 = ct->h; }
    if (clipx0 < 0) clipx0 = 0;
    if (clipx1 > ct->w) clipx1 = ct->w;
    if (clipy0 < 0) clipy0 = 0;
    if (clipy1 > ct->h) clipy1 = ct->h;

    x0 = (int)floorf(minx); x1 = (int)ceilf(maxx);
    y0 = (int)floorf(miny); y1 = (int)ceilf(maxy);
    if (x0 < clipx0) x0 = clipx0;
    if (x1 > clipx1) x1 = clipx1;
    if (y0 < clipy0) y0 = clipy0;
    if (y1 > clipy1) y1 = clipy1;

    /* This band's rows only. */
    if (y0 < by0) y0 = by0;
    if (y1 > by1) y1 = by1;
    if (y1 <= y0) return;

    { tri_ctx t;
      t.e = e; t.u = u; t.ct = ct; t.st = st; t.pipe = pipe; t.sd = sd;
      t.a = *a; t.b = *b; t.c = *c;
      t.area = area; t.sx = sx; t.sy = sy; t.ox = ox; t.oy = oy;
      t.x0 = x0; t.x1 = x1;
      raster_rows(&t, y0, y1, &rs->shaded); }
}

/* The pixels of one triangle, for the rows [y0, y1). */
static void raster_rows(const tri_ctx *t, int y0, int y1, unsigned long *shaded)
{
    mtl_encoder *e = t->e;
    const nvg_uniforms *u = t->u;
    mtl_texture *ct = t->ct, *st = t->st;
    mtl_pipeline *pipe = t->pipe;
    mtl_stencildesc *sd = t->sd;
    const vtx *a = &t->a, *b = &t->b, *c = &t->c;
    const float area = t->area, sx = t->sx, sy = t->sy, ox = t->ox, oy = t->oy;
    const int x0 = t->x0, x1 = t->x1;
    unsigned long sh = 0;
    int x, y;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float pxc = (float)x + 0.5f, pyc = (float)y + 0.5f;
            float w0, w1, w2, tu, tv, src[4];
            size_t off;
            uint8_t *dp;
            unsigned sval = 0;
            int passed;

            /* Barycentric coverage. The sign of `area` absorbs winding, so both
             * orientations rasterize -- nanovg does not cull. */
            w0 = ((b->x - a->x) * (pyc - a->y) - (pxc - a->x) * (b->y - a->y)) / area;
            w1 = ((pxc - a->x) * (c->y - a->y) - (c->x - a->x) * (pyc - a->y)) / area;
            w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            /* Stencil test, then the ops -- which run whether or not colour is
             * written, because that is how nanovg counts winding. */
            if (st && st->px && sd) {
                sval = st->px[(size_t)y * st->w + x];
                passed = stencil_pass(sd->cmp, e->sref, sval, sd->rmask);
                if (sd->wmask) {
                    unsigned nv = stencil_op(passed ? sd->dppass : sd->sfail,
                                             sval, e->sref);
                    st->px[(size_t)y * st->w + x] =
                        (uint8_t)((sval & ~sd->wmask) | (nv & sd->wmask));
                }
                if (!passed) continue;
            }

            /* The stencil-only pipeline has no fragment shader: it exists purely
             * to accumulate winding, so nothing is written to colour. */
            if (!pipe || !pipe->has_frag) continue;

            tu = w2 * a->u + w0 * b->u + w1 * c->u;
            tv = w2 * a->v + w0 * b->v + w1 * c->v;
            /* The shader sees fpos in points -- scissor and paint matrices were
             * built in that space -- so hand back the un-scaled position. */
            if (!frag_shader(u, e->ftex[0], e->fsamp,
                             (pxc - ox) / sx, (pyc - oy) / sy, tu, tv, src))
                continue;

            off = ((size_t)y * ct->w + x) * (size_t)ct->bpp;
            dp = ct->px + off;
            sh++;
            if (ct->bpp == 4) {
                int ri, gi, bi, ai;
                float d[4], o[4];
                int i;
                chan_order(ct->fmt, &ri, &gi, &bi, &ai);
                d[0] = dp[ri] / 255.0f; d[1] = dp[gi] / 255.0f;
                d[2] = dp[bi] / 255.0f; d[3] = dp[ai] / 255.0f;
                if (pipe->blend) {
                    for (i = 0; i < 3; i++)
                        o[i] = src[i] * blend_factor(pipe->src_rgb, src[i], src[3], d[i], d[3])
                             + d[i]   * blend_factor(pipe->dst_rgb, src[i], src[3], d[i], d[3]);
                    o[3] = src[3] * blend_factor(pipe->src_a, src[3], src[3], d[3], d[3])
                         + d[3]   * blend_factor(pipe->dst_a, src[3], src[3], d[3], d[3]);
                } else {
                    for (i = 0; i < 4; i++) o[i] = src[i];
                }
                dp[ri] = (uint8_t)(clampf(o[0], 0.0f, 1.0f) * 255.0f + 0.5f);
                dp[gi] = (uint8_t)(clampf(o[1], 0.0f, 1.0f) * 255.0f + 0.5f);
                dp[bi] = (uint8_t)(clampf(o[2], 0.0f, 1.0f) * 255.0f + 0.5f);
                dp[ai] = (uint8_t)(clampf(o[3], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    *shaded += sh;
}


static const nvg_uniforms *cur_uniforms(mtl_encoder *e)
{
    static const nvg_uniforms zero;
    mtl_buffer *b = e->fbuf[0];
    if (!b || !b->data) return &zero;
    if (e->foff[0] + sizeof(nvg_uniforms) > b->len) return &zero;
    return (const nvg_uniforms *)((uint8_t *)b->data + e->foff[0]);
}


/* ---- one draw call, banded ---------------------------------------------
 *
 * A call is a stream of triangles the plug-in issued in order. Give each band a
 * horizontal strip of the target and let every band walk the whole stream,
 * drawing only the rows it owns: a pixel is written by exactly one band, and
 * within that band the triangles are applied in the order they arrived. The
 * blending, the stencil read-modify-write and the overdraw are therefore
 * unchanged, and the image is the one the single-threaded loop produced -- which
 * is compared byte for byte across the corpus rather than assumed.
 *
 * Banding the call rather than the triangle is what makes it work for FB-7999,
 * whose frame is twelve thousand small triangles: no one of them is worth a
 * handoff, and together they are most of a second per twenty frames. */
typedef struct {
    mtl_encoder        *e;
    const nvg_uniforms *u;
    int                 prim;
    const vtx          *v;
    unsigned long       n;        /* vertices, for the non-indexed forms */
    const void         *ix;       /* NULL unless indexed */
    int                 itype;    /* 0 = uint16, else uint32 */
    unsigned long       count;    /* indices */
    mtl_buffer         *vb;
    unsigned long       voff;
} draw_job;

static unsigned long job_ntri(const draw_job *j)
{
    if (j->ix) return j->count / 3;
    if (j->prim == PRIM_TRISTRIP) return j->n >= 2 ? j->n - 2 : 0;
    return j->n / 3;
}

/* The k'th triangle, or 0 when the buffers do not hold it. */
static int job_tri(const draw_job *j, unsigned long k,
                   const vtx **a, const vtx **b, const vtx **c)
{
    if (j->ix) {
        unsigned long i = k * 3, ia, ib, ic;
        if (j->itype == 0) {
            const uint16_t *x = (const uint16_t *)j->ix;
            ia = x[i]; ib = x[i + 1]; ic = x[i + 2];
        } else {
            const uint32_t *x = (const uint32_t *)j->ix;
            ia = x[i]; ib = x[i + 1]; ic = x[i + 2];
        }
        if (j->voff + (ia + 1) * sizeof(vtx) > j->vb->len) return 0;
        if (j->voff + (ib + 1) * sizeof(vtx) > j->vb->len) return 0;
        if (j->voff + (ic + 1) * sizeof(vtx) > j->vb->len) return 0;
        *a = &j->v[ia]; *b = &j->v[ib]; *c = &j->v[ic];
        return 1;
    }
    if (j->prim == PRIM_TRISTRIP) {
        /* Alternating winding, as a strip has. raster_tri does not cull, so
         * this only matters for interpolation -- keep it faithful anyway. */
        if (k & 1) { *a = &j->v[k + 1]; *b = &j->v[k]; *c = &j->v[k + 2]; }
        else       { *a = &j->v[k];     *b = &j->v[k + 1]; *c = &j->v[k + 2]; }
        return 1;
    }
    *a = &j->v[k * 3]; *b = &j->v[k * 3 + 1]; *c = &j->v[k * 3 + 2];
    return 1;
}

/* Roughly how many pixels this call will touch: the triangles' bounding boxes,
 * which overestimates by about half and is the same overestimate every time.
 * Stops counting once the answer is past the threshold, so a big call pays for
 * a few triangles rather than all of them. */
static long job_work(const draw_job *j, long enough)
{
    unsigned long k, n = job_ntri(j);
    long total = 0;
    float sx = 1.0f, sy = 1.0f;
    const mtl_encoder *e = j->e;
    if (e->vsw > 0.0f && e->vw > 0.0) sx = (float)e->vw / e->vsw;
    if (e->vsh > 0.0f && e->vh > 0.0) sy = (float)e->vh / e->vsh;
    for (k = 0; k < n; k++) {
        const vtx *a, *b, *c;
        float x0, x1, y0, y1;
        if (!job_tri(j, k, &a, &b, &c)) continue;
        x0 = fminf(a->x, fminf(b->x, c->x)); x1 = fmaxf(a->x, fmaxf(b->x, c->x));
        y0 = fminf(a->y, fminf(b->y, c->y)); y1 = fmaxf(a->y, fmaxf(b->y, c->y));
        total += (long)((x1 - x0) * sx * (y1 - y0) * sy);
        if (total >= enough) return total;
    }
    return total;
}

static void job_rows(const draw_job *j, int by0, int by1, raster_stats *rs)
{
    unsigned long k, n = job_ntri(j);
    for (k = 0; k < n; k++) {
        const vtx *a, *b, *c;
        if (!job_tri(j, k, &a, &b, &c)) continue;
        raster_tri(j->e, j->u, a, b, c, by0, by1, rs);
    }
}

/* What a call has to be worth before it is split. The handoff is two condition
 * variables and a dozen wakeups -- tens of microseconds -- so it pays only for
 * a call that will shade a substantial part of a frame. Judging that by the
 * size of the target instead was a fifty-millisecond mistake: FB-7999 issues
 * twenty-three hundred draw calls a frame, eight triangles each, and banding
 * every one of them cost twice what drawing them did. */
#define RASTER_MIN_ROWS   64
#define RASTER_MIN_WORK   150000    /* pixels, summed over the call's triangles */
#define RASTER_MAX_BANDS  12

static int             g_nband;                  /* 0 until the pool is up */
static pthread_t       g_rthread[RASTER_MAX_BANDS - 1];
static pthread_mutex_t g_rlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_rgo   = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_rdone = PTHREAD_COND_INITIALIZER;
static const draw_job *g_rjob;
static int             g_rgen, g_rleft, g_ry0, g_ry1;
static raster_stats    g_rstats[RASTER_MAX_BANDS];

static void band_range(int y0, int y1, int band, int *a, int *b)
{
    long span = y1 - y0;
    *a = y0 + (int)(span * band / g_nband);
    *b = y0 + (int)(span * (band + 1) / g_nband);
}

static void *raster_worker(void *arg)
{
    int id = (int)(long)arg, seen = 0;
    for (;;) {
        int a, b;
        pthread_mutex_lock(&g_rlock);
        while (g_rgen == seen) pthread_cond_wait(&g_rgo, &g_rlock);
        seen = g_rgen;
        pthread_mutex_unlock(&g_rlock);
        band_range(g_ry0, g_ry1, id, &a, &b);
        if (b > a) job_rows(g_rjob, a, b, &g_rstats[id]);
        pthread_mutex_lock(&g_rlock);
        if (--g_rleft == 0) pthread_cond_signal(&g_rdone);
        pthread_mutex_unlock(&g_rlock);
    }
    return NULL;                                  /* not reached */
}

/* Brought up on the first call worth splitting, so a host that only ever shows
 * Core Graphics editors never creates a thread. */
static void raster_pool_start(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int i, n;
    if (g_nband) return;
    n = ncpu > 1 ? (int)ncpu : 1;
    if (n > RASTER_MAX_BANDS) n = RASTER_MAX_BANDS;
    g_nband = n;
    for (i = 1; i < n; i++)
        if (pthread_create(&g_rthread[i - 1], NULL, raster_worker,
                           (void *)(long)i) != 0) {
            g_nband = i;          /* however many started is however many bands */
            break;
        }
}

static void draw_bands(const draw_job *j)
{
    mtl_texture *ct = j->e ? j->e->color : NULL;
    raster_stats rs;
    int h, i, a, b;

    if (!ct || !ct->px) return;
    h = ct->h;
    rs.tris = rs.shaded = 0;

    if (h < RASTER_MIN_ROWS || job_ntri(j) < 2 ||
        job_work(j, RASTER_MIN_WORK) < RASTER_MIN_WORK) {
        job_rows(j, 0, h, &rs);
        g_tris += rs.tris; g_shaded += rs.shaded;
        return;
    }
    raster_pool_start();
    if (g_nband < 2) {
        job_rows(j, 0, h, &rs);
        g_tris += rs.tris; g_shaded += rs.shaded;
        return;
    }

    pthread_mutex_lock(&g_rlock);
    g_rjob = j; g_ry0 = 0; g_ry1 = h;
    for (i = 0; i < g_nband; i++) g_rstats[i].tris = g_rstats[i].shaded = 0;
    g_rleft = g_nband - 1;
    g_rgen++;
    pthread_cond_broadcast(&g_rgo);
    pthread_mutex_unlock(&g_rlock);

    band_range(0, h, 0, &a, &b);
    if (b > a) job_rows(j, a, b, &g_rstats[0]);

    pthread_mutex_lock(&g_rlock);
    while (g_rleft > 0) pthread_cond_wait(&g_rdone, &g_rlock);
    pthread_mutex_unlock(&g_rlock);

    for (i = 0; i < g_nband; i++)
        { g_tris += g_rstats[i].tris; g_shaded += g_rstats[i].shaded; }
}

static void draw_range(mtl_encoder *e, int prim, const vtx *v, unsigned long n)
{
    draw_job j;
    double t0 = now_ms();

    if (prim != PRIM_TRI && prim != PRIM_TRISTRIP) return;   /* nanovg emits neither */
    memset(&j, 0, sizeof j);
    j.e = e; j.u = cur_uniforms(e); j.prim = prim; j.v = v; j.n = n;
    draw_bands(&j);
    g_raster_ms += now_ms() - t0;
}


/* ------------------------------------------------------------ MTLDevice */

static id dev_new_queue(id self, SEL sel)
{ (void)self; (void)sel; return new_of("MTLCommandQueue"); }

static id dev_new_buffer(id self, SEL sel, unsigned long len, unsigned long opts)
{
    mtl_buffer *b = (mtl_buffer *)new_of("MTLBuffer");
    (void)self; (void)sel; (void)opts;
    if (!b) return NULL;
    b->len = len;
    b->data = payload_alloc(len);
    return b;
}

static id dev_new_buffer_bytes(id self, SEL sel, const void *p,
                               unsigned long len, unsigned long opts)
{
    mtl_buffer *b = (mtl_buffer *)dev_new_buffer(self, sel, len, opts);
    if (b && b->data && p) memcpy(b->data, p, len);
    return b;
}

static id dev_new_texture(id self, SEL sel, id descp)
{
    mtl_texdesc *d = descp;
    (void)self; (void)sel;
    if (!d) return NULL;
    if (verbose())
        fprintf(stderr, "  [mtl] texture %dx%d fmt=%d\n", d->w, d->h, d->fmt);
    return tex_make(d->fmt, d->w, d->h);
}

/* The bitcode is ignored -- see the file comment. Returning a library object is
 * enough, because the only thing asked of it is a named function. */
static id dev_new_library(id self, SEL sel, id data, id *err)
{
    (void)sel; (void)data;
    if (verbose()) fprintf(stderr, "  [mtl] newLibraryWithData on %p\n", (void *)self);
    if (err) *err = NULL;
    return new_of("MTLLibrary");
}

static id dev_new_default_library(id self, SEL sel)
{ (void)self; (void)sel; return new_of("MTLLibrary"); }

static id lib_new_function(id self, SEL sel, id name)
{
    mtl_named *f = (mtl_named *)new_of("MTLFunction");
    const char *s = macns_utf8(name);
    (void)self; (void)sel;
    if (f) snprintf(f->name, sizeof f->name, "%s", s ? s : "");
    if (f && verbose()) fprintf(stderr, "  [mtl] function %s\n", f->name);
    return f;
}

static id dev_new_pipeline(id self, SEL sel, id descp, id *err)
{
    mtl_pipedesc *d = descp;
    mtl_pipeline *p = (mtl_pipeline *)new_of("MTLRenderPipelineState");
    mtl_attach *ca = (d && d->color) ? d->color->a[0] : NULL;
    (void)self; (void)sel;
    if (err) *err = NULL;
    if (!p) return NULL;
    /* A nil fragment function is nanovg's stencil-only pipeline. */
    p->has_frag = d && d->ffn != NULL;
    /* -1 keeps "never set" distinct from "set to zero". */
    p->write_mask = ca ? ca->write_mask : -1;
    if (p->write_mask == 0) p->has_frag = 0;
    if (ca) {
        p->blend = ca->blend;
        p->src_rgb = ca->src_rgb; p->dst_rgb = ca->dst_rgb;
        p->src_a = ca->src_a;     p->dst_a = ca->dst_a;
        p->pixfmt = ca->pixfmt;
    }
    if (verbose())
        fprintf(stderr, "  [mtl] pipeline frag=%d blend=%d src=%d dst=%d fmt=%d\n",
                p->has_frag, p->blend, p->src_rgb, p->dst_rgb, p->pixfmt);
    return p;
}

static id dev_new_dsstate(id self, SEL sel, id descp)
{
    mtl_dsstate *src = descp;
    mtl_dsstate *s = (mtl_dsstate *)new_of("MTLDepthStencilState");
    (void)self; (void)sel;
    if (!s) return NULL;
    if (src) { s->front = src->front; s->back = src->back; s->depth_cmp = src->depth_cmp; }
    return s;
}

static id dev_new_sampler(id self, SEL sel, id descp)
{
    mtl_sampler *src = descp;
    mtl_sampler *s = (mtl_sampler *)new_of("MTLSamplerState");
    (void)self; (void)sel;
    if (s && src) {
        void *isa = s->isa;
        long refs = s->refs;
        *s = *src;
        s->isa = isa; s->refs = refs;      /* the copy must stay our class */
    }
    return s;
}

static id dev_name(id self, SEL sel)
{ (void)self; (void)sel; return macshim_cf_string("peload software Metal"); }

static signed char dev_false(id self, SEL sel) { (void)self; (void)sel; return 0; }
static signed char dev_true(id self, SEL sel)  { (void)self; (void)sel; return 1; }

/* --------------------------------------------------------- command buffer */

static id queue_cmdbuf(id self, SEL sel)
{ (void)self; (void)sel; return new_of("MTLCommandBuffer"); }

typedef struct { void *isa; int flags, reserved; void (*invoke)(void *, ...); } block_lit;

static void cb_add_handler(id self, SEL sel, void *blk)
{
    mtl_cmdbuf *c = self;
    (void)sel;
    if (c && blk && c->nh < (int)(sizeof c->handlers / sizeof *c->handlers))
        c->handlers[c->nh++] = blk;
}

static void cb_run_handlers(mtl_cmdbuf *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < c->nh; i++) {
        block_lit *b = c->handlers[i];
        if (b && b->invoke) ((void (*)(void *, void *))b->invoke)(b, c);
    }
    c->nh = 0;
}

static void cb_commit(id self, SEL sel)
{ mtl_cmdbuf *c = self; (void)sel; if (c) { c->committed = 1; cb_run_handlers(c); } }

static void cb_wait(id self, SEL sel)
{ mtl_cmdbuf *c = self; (void)sel; cb_run_handlers(c); }

static void cb_present(id self, SEL sel, id drawable)
{ (void)self; (void)sel; (void)drawable; /* the layer's texture is the target */ }

static id cb_render_encoder(id self, SEL sel, id descp)
{
    mtl_rpdesc *d = descp;
    mtl_encoder *e = (mtl_encoder *)new_of("MTLRenderCommandEncoder");
    mtl_attach *ca = (d && d->color) ? d->color->a[0] : NULL;
    (void)self; (void)sel;
    if (!e) return NULL;

    if (ca) {
        e->color = ca->tex;
        if (ca->load == LOAD_CLEAR && ca->tex && ca->tex->px) {
            /* Clear in the target's own byte order. */
            size_t i, n = (size_t)ca->tex->w * ca->tex->h;
            uint8_t c[4];
            int ri, gi, bi, ai;
            chan_order(ca->tex->fmt, &ri, &gi, &bi, &ai);
            c[ri] = (uint8_t)(clampf((float)ca->clear.r, 0, 1) * 255.0f + 0.5f);
            c[gi] = (uint8_t)(clampf((float)ca->clear.g, 0, 1) * 255.0f + 0.5f);
            c[bi] = (uint8_t)(clampf((float)ca->clear.b, 0, 1) * 255.0f + 0.5f);
            c[ai] = (uint8_t)(clampf((float)ca->clear.a, 0, 1) * 255.0f + 0.5f);
            if (ca->tex->bpp == 4)
                for (i = 0; i < n; i++) memcpy(ca->tex->px + i * 4, c, 4);
            else
                memset(ca->tex->px, c[3], n * (size_t)ca->tex->bpp);
        }
    }
    if (d && d->stencil) {
        e->sten = d->stencil->tex;
        if (d->stencil->load == LOAD_CLEAR && e->sten && e->sten->px)
            memset(e->sten->px, (int)(d->stencil->clear_stencil & 0xff),
                   (size_t)e->sten->w * e->sten->h * (size_t)e->sten->bpp);
    }
    return e;
}

static id cb_blit_encoder(id self, SEL sel)
{ (void)self; (void)sel; return new_of("MTLBlitCommandEncoder"); }

/* -------------------------------------------------------- render encoder */

static void enc_set_pipeline(id self, SEL sel, id p)
{ mtl_encoder *e = self; (void)sel; if (e) e->pipe = p; }

static void enc_set_ds(id self, SEL sel, id p)
{ mtl_encoder *e = self; (void)sel; if (e) e->ds = p; }

static void enc_set_sref(id self, SEL sel, unsigned r)
{ mtl_encoder *e = self; (void)sel; if (e) e->sref = r; }

static void enc_set_viewport(id self, SEL sel, MTLViewport v)
{
    mtl_encoder *e = self;
    (void)sel;
    if (!e) return;
    e->vx = v.x; e->vy = v.y; e->vw = v.w; e->vh = v.h;
}

/* nanovg declares its viewSize as vector_uint2, but other builds of the same
 * shader use float2. Sniff rather than assume: a plausible pixel size as two
 * integers is unambiguous, and anything else is read as floats. */
static void note_viewsize(mtl_encoder *e, const void *p, unsigned long len)
{
    const uint32_t *u = p;
    const float *f = p;
    if (!e || !p || len < 8) return;
    if (u[0] >= 1 && u[0] <= 65535 && u[1] >= 1 && u[1] <= 65535) {
        e->vsw = (float)u[0]; e->vsh = (float)u[1];
    } else if (f[0] >= 1.0f && f[0] <= 65535.0f && f[1] >= 1.0f && f[1] <= 65535.0f) {
        e->vsw = f[0]; e->vsh = f[1];
    }
}

static void enc_set_vbuf(id self, SEL sel, id buf, unsigned long off, unsigned long idx)
{
    mtl_encoder *e = self;
    mtl_buffer *b = buf;
    (void)sel;
    if (!e || idx >= 4) return;
    e->vbuf[idx] = buf; e->voff[idx] = off;
    if (idx == 1 && b && b->data && off + 8 <= b->len)
        note_viewsize(e, (uint8_t *)b->data + off, 8);
}

static void enc_set_fbuf(id self, SEL sel, id buf, unsigned long off, unsigned long idx)
{
    mtl_encoder *e = self;
    (void)sel;
    if (e && idx < 4) { e->fbuf[idx] = buf; e->foff[idx] = off; }
}

static void enc_set_fbuf_off(id self, SEL sel, unsigned long off, unsigned long idx)
{ mtl_encoder *e = self; (void)sel; if (e && idx < 4) e->foff[idx] = off; }

static void enc_set_vbuf_off(id self, SEL sel, unsigned long off, unsigned long idx)
{ mtl_encoder *e = self; (void)sel; if (e && idx < 4) e->voff[idx] = off; }

static void enc_set_ftex(id self, SEL sel, id t, unsigned long idx)
{ mtl_encoder *e = self; (void)sel; if (e && idx < 4) e->ftex[idx] = t; }

static void enc_set_fsamp(id self, SEL sel, id s, unsigned long idx)
{ mtl_encoder *e = self; (void)sel; if (e && idx == 0) e->fsamp = s; }

static void enc_set_vbytes(id self, SEL sel, const void *bytes,
                           unsigned long len, unsigned long idx)
{
    mtl_encoder *e = self;
    (void)sel;
    if (idx == 1) note_viewsize(e, bytes, len);
}

static void enc_end(id self, SEL sel) { (void)self; (void)sel; }

static void enc_draw(id self, SEL sel, unsigned long prim,
                     unsigned long start, unsigned long count)
{
    mtl_encoder *e = self;
    mtl_buffer *vb;
    (void)sel;
    if (!e || !(vb = e->vbuf[0]) || !vb->data) return;
    if (e->voff[0] + (start + count) * sizeof(vtx) > vb->len) return;
    draw_range(e, (int)prim, (const vtx *)((uint8_t *)vb->data + e->voff[0]) + start,
               count);
}

static void enc_draw_indexed(id self, SEL sel, unsigned long prim,
                             unsigned long count, unsigned long itype,
                             id ibufp, unsigned long ioff)
{
    mtl_encoder *e = self;
    mtl_buffer *vb, *ib = ibufp;
    draw_job j;
    unsigned long isz, room;
    double t0 = now_ms();
    (void)sel;

    if (!e || !(vb = e->vbuf[0]) || !vb->data || !ib || !ib->data) return;
    if (prim != PRIM_TRI) return;

    /* Trim to what the index buffer actually holds, once, rather than checking
     * inside the loop -- the loop now runs on several threads. */
    isz = itype == 0 ? 2 : 4;
    if (ioff >= ib->len) return;
    room = (ib->len - ioff) / isz;
    if (count > room) count = room;

    memset(&j, 0, sizeof j);
    j.e = e; j.u = cur_uniforms(e); j.prim = PRIM_TRI;
    j.v = (const vtx *)((uint8_t *)vb->data + e->voff[0]);
    j.ix = (const uint8_t *)ib->data + ioff;
    j.itype = itype == 0 ? 0 : 1;
    j.count = count;
    j.vb = vb;
    j.voff = e->voff[0];
    draw_bands(&j);
    g_raster_ms += now_ms() - t0;
}

/* ---------------------------------------------------------- blit encoder */

static void blit_copy_buf_to_tex(id self, SEL sel, id bufp, unsigned long soff,
                                 unsigned long sbpr, unsigned long sbpi,
                                 MTLSize ssize, id texp, unsigned long slice,
                                 unsigned long level, MTLOrigin dorigin)
{
    mtl_buffer *b = bufp;
    mtl_texture *t = texp;
    unsigned long row;
    (void)self; (void)sel; (void)sbpi; (void)slice; (void)level;

    if (!b || !b->data || !t || !t->px) return;
    if (verbose() && ssize.h > 100) {
        const uint8_t *q = (const uint8_t *)b->data + soff;
        fprintf(stderr, "  [mtl] blit %lux%lu -> tex %dx%d fmt=%d "
                        "first px %02x %02x %02x %02x\n",
                ssize.w, ssize.h, t->w, t->h, t->fmt, q[0], q[1], q[2], q[3]);
    }
    for (row = 0; row < ssize.h; row++) {
        unsigned long dy = dorigin.y + row;
        const uint8_t *src;
        uint8_t *dst;
        unsigned long n;
        if (dy >= (unsigned long)t->h) break;
        src = (const uint8_t *)b->data + soff + row * sbpr;
        if (soff + row * sbpr + ssize.w * (unsigned long)t->bpp > b->len) break;
        dst = t->px + ((size_t)dy * t->w + dorigin.x) * (size_t)t->bpp;
        n = ssize.w;
        if (dorigin.x + n > (unsigned long)t->w) n = (unsigned long)t->w - dorigin.x;
        memcpy(dst, src, n * (size_t)t->bpp);
    }
}

static void blit_noop_tex(id self, SEL sel, id t) { (void)self; (void)sel; (void)t; }

static void tex_replace_region(id self, SEL sel, MTLRegion r, unsigned long level,
                               const void *bytes, unsigned long bpr)
{
    mtl_texture *t = self;
    unsigned long row;
    (void)sel; (void)level;
    if (!t || !t->px || !bytes) return;
    if (verbose()) {
        const uint8_t *q = bytes;
        fprintf(stderr, "  [mtl] replaceRegion %lux%lu -> tex %dx%d fmt=%d "
                        "first px %02x %02x %02x %02x\n",
                r.size.w, r.size.h, t->w, t->h, t->fmt, q[0], q[1], q[2], q[3]);
    }
    for (row = 0; row < r.size.h; row++) {
        unsigned long dy = r.origin.y + row, n = r.size.w;
        if (dy >= (unsigned long)t->h) break;
        if (r.origin.x + n > (unsigned long)t->w) n = (unsigned long)t->w - r.origin.x;
        memcpy(t->px + ((size_t)dy * t->w + r.origin.x) * (size_t)t->bpp,
               (const uint8_t *)bytes + row * bpr, n * (size_t)t->bpp);
    }
}

/* ------------------------------------------------------------ descriptors */

static id texdesc_2d(id self, SEL sel, unsigned long fmt, unsigned long w,
                     unsigned long h, signed char mip)
{
    mtl_texdesc *d = (mtl_texdesc *)new_of("MTLTextureDescriptor");
    (void)self; (void)sel;
    if (!d) return NULL;
    d->fmt = (int)fmt; d->w = (int)w; d->h = (int)h; d->mip = mip;
    return d;
}

static void td_set_fmt(id self, SEL sel, unsigned long v)
{ mtl_texdesc *d = self; (void)sel; if (d) d->fmt = (int)v; }
static void td_set_w(id self, SEL sel, unsigned long v)
{ mtl_texdesc *d = self; (void)sel; if (d) d->w = (int)v; }
static void td_set_h(id self, SEL sel, unsigned long v)
{ mtl_texdesc *d = self; (void)sel; if (d) d->h = (int)v; }
static void td_set_usage(id self, SEL sel, unsigned long v)
{ mtl_texdesc *d = self; (void)sel; if (d) d->usage = (int)v; }
static void td_set_storage(id self, SEL sel, unsigned long v)
{ mtl_texdesc *d = self; (void)sel; if (d) d->storage = (int)v; }
/* nanovg reads this back to decide between replaceRegion and a staging blit.
 * Shared storage is the truth here: the "GPU" reads the same malloc. */
static unsigned long td_storage(id self, SEL sel)
{ mtl_texdesc *d = self; (void)sel; return d ? (unsigned long)d->storage : 0; }

static mtl_attach *new_attach(void)
{
    mtl_attach *a = (mtl_attach *)new_of("MTLAttachment");
    if (a) a->write_mask = -1;
    return a;
}

static id rpdesc_new(id self, SEL sel)
{
    mtl_rpdesc *d = (mtl_rpdesc *)new_of("MTLRenderPassDescriptor");
    (void)self; (void)sel;
    if (!d) return NULL;
    d->color = (mtl_attacharr *)new_of("MTLAttachmentArray");
    if (d->color) d->color->a[0] = new_attach();
    d->stencil = new_attach();
    return d;
}

static id pipedesc_new(id self, SEL sel)
{
    mtl_pipedesc *d = (mtl_pipedesc *)new_of("MTLRenderPipelineDescriptor");
    (void)self; (void)sel;
    if (!d) return NULL;
    d->color = (mtl_attacharr *)new_of("MTLAttachmentArray");
    if (d->color) d->color->a[0] = new_attach();
    return d;
}

/* Built on demand, because a descriptor may arrive through alloc/init rather
 * than the class method that would have populated it. */
static mtl_attacharr *need_arr(mtl_attacharr **slot)
{
    if (!*slot) {
        *slot = (mtl_attacharr *)new_of("MTLAttachmentArray");
        if (*slot) (*slot)->a[0] = new_attach();
    }
    return *slot;
}

static id rp_color_attachments(id self, SEL sel)
{ mtl_rpdesc *d = self; (void)sel; return d ? need_arr(&d->color) : NULL; }
static id rp_stencil_attachment(id self, SEL sel)
{
    mtl_rpdesc *d = self;
    (void)sel;
    if (!d) return NULL;
    if (!d->stencil) d->stencil = new_attach();
    return d->stencil;
}
static id pd_color_attachments(id self, SEL sel)
{ mtl_pipedesc *d = self; (void)sel; return d ? need_arr(&d->color) : NULL; }

static id arr_subscript(id self, SEL sel, unsigned long i)
{
    mtl_attacharr *a = self;
    (void)sel;
    if (!a || i >= 4) return NULL;
    if (!a->a[i]) a->a[i] = new_attach();
    return a->a[i];
}

static void at_set_tex(id self, SEL sel, id t)
{ mtl_attach *a = self; (void)sel; if (a) a->tex = t; }
static id at_get_tex(id self, SEL sel)
{ mtl_attach *a = self; (void)sel; return a ? a->tex : NULL; }
static void at_set_load(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->load = (int)v; }
static void at_set_store(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->store = (int)v; }
static void at_set_clear_stencil(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->clear_stencil = (unsigned)v; }
static void at_set_clear_color(id self, SEL sel, MTLClearColor c)
{ mtl_attach *a = self; (void)sel; if (a) a->clear = c; }
static void at_set_pixfmt(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->pixfmt = (int)v; }
static void at_set_blend(id self, SEL sel, signed char v)
{ mtl_attach *a = self; (void)sel; if (a) a->blend = v ? 1 : 0; }
static void at_set_src_rgb(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->src_rgb = (int)v; }
static void at_set_dst_rgb(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->dst_rgb = (int)v; }
static void at_set_src_a(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->src_a = (int)v; }
static void at_set_dst_a(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->dst_a = (int)v; }
/* Zero means write no colour channels -- a second way to say "stencil only". */
static void at_set_write_mask(id self, SEL sel, unsigned long v)
{ mtl_attach *a = self; (void)sel; if (a) a->write_mask = (int)v; }

static void pd_set_vfn(id self, SEL sel, id f)
{ mtl_pipedesc *d = self; (void)sel; if (d) d->vfn = f; }
static void pd_set_ffn(id self, SEL sel, id f)
{ mtl_pipedesc *d = self; (void)sel; if (d) d->ffn = f; }
static void pd_set_vdesc(id self, SEL sel, id v)
{ mtl_pipedesc *d = self; (void)sel; if (d) d->vdesc = v; }
static void pd_set_stencil_fmt(id self, SEL sel, unsigned long v)
{ mtl_pipedesc *d = self; (void)sel; if (d) d->stencil_pixfmt = (int)v; }

static void ds_set_front(id self, SEL sel, id s)
{ mtl_dsstate *d = self; (void)sel; if (d) d->front = s; }
static void ds_set_back(id self, SEL sel, id s)
{ mtl_dsstate *d = self; (void)sel; if (d) d->back = s; }
static void ds_set_depth_cmp(id self, SEL sel, unsigned long v)
{ mtl_dsstate *d = self; (void)sel; if (d) d->depth_cmp = (int)v; }

/* A freshly allocated stencil descriptor must read as Metal's defaults: compare
 * always, keep on every path, and both masks open. calloc gives compare=never,
 * which would silently reject every fragment. */
static id sdesc_new(id self, SEL sel)
{
    mtl_stencildesc *s = (mtl_stencildesc *)new_of("MTLStencilDescriptor");
    (void)self; (void)sel;
    if (!s) return NULL;
    s->cmp = CMP_ALWAYS;
    s->sfail = s->dpfail = s->dppass = SOP_KEEP;
    s->rmask = s->wmask = 0xff;
    return s;
}

static id sdesc_init(id self, SEL sel)
{
    mtl_stencildesc *s = self;
    (void)sel;
    if (s) {
        s->cmp = CMP_ALWAYS;
        s->sfail = s->dpfail = s->dppass = SOP_KEEP;
        s->rmask = s->wmask = 0xff;
    }
    return self;
}

static void sd_set_cmp(id self, SEL sel, unsigned long v)
{ mtl_stencildesc *s = self; (void)sel; if (s) s->cmp = (int)v; }
static void sd_set_sfail(id self, SEL sel, unsigned long v)
{ mtl_stencildesc *s = self; (void)sel; if (s) s->sfail = (int)v; }
static void sd_set_dpfail(id self, SEL sel, unsigned long v)
{ mtl_stencildesc *s = self; (void)sel; if (s) s->dpfail = (int)v; }
static void sd_set_dppass(id self, SEL sel, unsigned long v)
{ mtl_stencildesc *s = self; (void)sel; if (s) s->dppass = (int)v; }
static void sd_set_rmask(id self, SEL sel, unsigned long v)
{ mtl_stencildesc *s = self; (void)sel; if (s) s->rmask = (unsigned)v; }
static void sd_set_wmask(id self, SEL sel, unsigned long v)
{ mtl_stencildesc *s = self; (void)sel; if (s) s->wmask = (unsigned)v; }

static id samp_new(id self, SEL sel)
{
    mtl_sampler *s = (mtl_sampler *)new_of("MTLSamplerDescriptor");
    (void)self; (void)sel;
    return s;
}
static void samp_set_int(id self, SEL sel, unsigned long v)
{ (void)self; (void)sel; (void)v; }

/* The vertex descriptor is accepted and ignored: nanovg's layout is fixed at
 * float2 position, float2 texcoord, stride 16, which is what `vtx` is. */
static id vdesc_new(id self, SEL sel)
{ (void)self; (void)sel; return new_of("MTLVertexDescriptor"); }
static id vdesc_arr(id self, SEL sel)
{ (void)self; (void)sel; return new_of("MTLVertexArray"); }
static id vdesc_sub(id self, SEL sel, unsigned long i)
{ (void)self; (void)sel; (void)i; return new_of("MTLVertexItem"); }
static void vitem_set_int(id self, SEL sel, unsigned long v)
{ (void)self; (void)sel; (void)v; }

/* ---------------------------------------------------------- buffer/texture */

static void *buf_contents(id self, SEL sel)
{ mtl_buffer *b = self; (void)sel; return b ? b->data : NULL; }
static unsigned long buf_length(id self, SEL sel)
{ mtl_buffer *b = self; (void)sel; return b ? b->len : 0; }
static void buf_did_modify(id self, SEL sel, MTLRegion r)
{ (void)self; (void)sel; (void)r; }

static unsigned long tex_width(id self, SEL sel)
{ mtl_texture *t = self; (void)sel; return t ? (unsigned long)t->w : 0; }
static unsigned long tex_height(id self, SEL sel)
{ mtl_texture *t = self; (void)sel; return t ? (unsigned long)t->h : 0; }
static unsigned long tex_pixfmt(id self, SEL sel)
{ mtl_texture *t = self; (void)sel; return t ? (unsigned long)t->fmt : 0; }

/* ------------------------------------------------------------ CAMetalLayer */

static id layer_new(id self, SEL sel)
{
    ca_layer *l = (ca_layer *)new_of("CAMetalLayer");
    (void)self; (void)sel;
    if (!l) return NULL;
    l->pixfmt = PF_BGRA8;
    l->scale = 1.0;
    /* One layer at a time: an editor has exactly one, and the host needs to
     * find its framebuffer without being handed a pointer. */
    g_layer = l;
    return l;
}

static id layer_device(id self, SEL sel)
{
    ca_layer *l = self;
    (void)sel;
    if (verbose()) fprintf(stderr, "  [mtl] layer %p device -> %p\n",
                           (void *)l, l ? l->device : NULL);
    return l ? l->device : NULL;
}
static void layer_set_device(id self, SEL sel, id d)
{
    ca_layer *l = self;
    (void)sel;
    if (verbose()) fprintf(stderr, "  [mtl] layer %p setDevice %p\n", (void *)l, d);
    if (l) l->device = d;
}
static void layer_set_pixfmt(id self, SEL sel, unsigned long v)
{ ca_layer *l = self; (void)sel; if (l) l->pixfmt = (int)v; }
static void layer_set_flag(id self, SEL sel, signed char v)
{ ca_layer *l = self; (void)sel; if (l) l->opaque = v ? 1 : 0; }
static double layer_scale(id self, SEL sel)
{ ca_layer *l = self; (void)sel; return l && l->scale > 0.0 ? l->scale : 1.0; }
static void layer_set_scale(id self, SEL sel, double v)
{ ca_layer *l = self; (void)sel; if (l && v > 0.0) l->scale = v; }
static void layer_void(id self, SEL sel) { (void)self; (void)sel; }

static void layer_set_drawable_size(id self, SEL sel, CGSize s)
{
    ca_layer *l = self;
    int w = (int)(s.w + 0.5), h = (int)(s.h + 0.5);
    (void)sel;
    if (!l) return;
    if (w < 1 || h < 1) return;
    if (l->back && l->back->w == w && l->back->h == h) return;
    if (l->back) { free(l->back->px); l->back->px = NULL; }
    l->w = w; l->h = h;
    l->back = tex_make(l->pixfmt, w, h);
    if (verbose()) fprintf(stderr, "  [mtl] drawable %dx%d fmt=%d\n", w, h, l->pixfmt);
}

static CGSize layer_drawable_size(id self, SEL sel)
{
    ca_layer *l = self;
    CGSize s;
    (void)sel;
    s.w = l ? (double)l->w : 0.0;
    s.h = l ? (double)l->h : 0.0;
    return s;
}

static id layer_next_drawable(id self, SEL sel)
{
    ca_layer *l = self;
    ca_drawable *d;
    (void)sel;
    if (!l) return NULL;
    if (!l->back) {
        /* A layer asked for a drawable before anyone set a size. Refusing would
         * abort the frame, so give it the size the editor was opened at. */
        if (l->w < 1 || l->h < 1) return NULL;
        l->back = tex_make(l->pixfmt, l->w, l->h);
    }
    d = (ca_drawable *)new_of("CAMetalDrawable");
    if (d) d->tex = l->back;
    return d;
}

static id drawable_texture(id self, SEL sel)
{ ca_drawable *d = self; (void)sel; return d ? d->tex : NULL; }
static void drawable_present(id self, SEL sel) { (void)self; (void)sel; }

/* ------------------------------------------------------------- the device */

static void *g_device;

static void *mtl_create_system_default_device(void)
{
    if (!g_device) g_device = new_of("MTLDevice");
    return g_device;
}

static void *mtl_copy_all_devices(void)
{
    const void *v[1];
    v[0] = mtl_create_system_default_device();
    return macshim_cf_array(v, 1);
}

/* nanovg_mtl reads this for its buffer allocations. Storage mode shared, so the
 * CPU pointer it gets from `contents` is the one the "GPU" reads. */
static unsigned long kMetalBufferOptions = 0;

const macshim_entry macshim_metal[] = {
    { "_MTLCreateSystemDefaultDevice", (void *)mtl_create_system_default_device },
    { "_MTLCopyAllDevices",           (void *)mtl_copy_all_devices },
    { "_kMetalBufferOptions",         (void *)&kMetalBufferOptions },
    { NULL, NULL }
};

/* ------------------------------------------------------------------ install */

typedef struct { const char *cls, *sel; void *imp; } entry;

static const entry g_table[] = {
    /* device */
    { "MTLDevice", "newCommandQueue",                    dev_new_queue },
    { "MTLDevice", "newCommandQueueWithMaxCommandBufferCount:", dev_new_queue },
    { "MTLDevice", "newBufferWithLength:options:",        dev_new_buffer },
    { "MTLDevice", "newBufferWithBytes:length:options:",  dev_new_buffer_bytes },
    { "MTLDevice", "newTextureWithDescriptor:",           dev_new_texture },
    { "MTLDevice", "newLibraryWithData:error:",           dev_new_library },
    { "MTLDevice", "newLibraryWithSource:options:error:", dev_new_library },
    { "MTLDevice", "newDefaultLibrary",                   dev_new_default_library },
    { "MTLDevice", "newRenderPipelineStateWithDescriptor:error:", dev_new_pipeline },
    { "MTLDevice", "newDepthStencilStateWithDescriptor:", dev_new_dsstate },
    { "MTLDevice", "newSamplerStateWithDescriptor:",      dev_new_sampler },
    { "MTLDevice", "name",                                dev_name },
    { "MTLDevice", "isLowPower",                          dev_false },
    { "MTLDevice", "isHeadless",                          dev_false },
    { "MTLDevice", "supportsTextureSampleCount:",         dev_true },

    { "MTLLibrary", "newFunctionWithName:",               lib_new_function },

    /* queue and command buffer */
    { "MTLCommandQueue", "commandBuffer",                 queue_cmdbuf },
    { "MTLCommandBuffer", "renderCommandEncoderWithDescriptor:", cb_render_encoder },
    { "MTLCommandBuffer", "blitCommandEncoder",           cb_blit_encoder },
    { "MTLCommandBuffer", "addCompletedHandler:",         cb_add_handler },
    { "MTLCommandBuffer", "commit",                       cb_commit },
    { "MTLCommandBuffer", "waitUntilCompleted",           cb_wait },
    { "MTLCommandBuffer", "waitUntilScheduled",           cb_wait },
    { "MTLCommandBuffer", "presentDrawable:",             cb_present },
    { "MTLCommandBuffer", "enqueue",                      layer_void },

    /* render command encoder */
    { "MTLRenderCommandEncoder", "setRenderPipelineState:", enc_set_pipeline },
    { "MTLRenderCommandEncoder", "setDepthStencilState:",   enc_set_ds },
    { "MTLRenderCommandEncoder", "setStencilReferenceValue:", enc_set_sref },
    { "MTLRenderCommandEncoder", "setViewport:",            enc_set_viewport },
    { "MTLRenderCommandEncoder", "setVertexBuffer:offset:atIndex:", enc_set_vbuf },
    { "MTLRenderCommandEncoder", "setVertexBufferOffset:atIndex:",  enc_set_vbuf_off },
    { "MTLRenderCommandEncoder", "setVertexBytes:length:atIndex:",  enc_set_vbytes },
    { "MTLRenderCommandEncoder", "setFragmentBuffer:offset:atIndex:", enc_set_fbuf },
    { "MTLRenderCommandEncoder", "setFragmentBufferOffset:atIndex:",  enc_set_fbuf_off },
    { "MTLRenderCommandEncoder", "setFragmentTexture:atIndex:",     enc_set_ftex },
    { "MTLRenderCommandEncoder", "setFragmentSamplerState:atIndex:", enc_set_fsamp },
    { "MTLRenderCommandEncoder", "setCullMode:",            samp_set_int },
    { "MTLRenderCommandEncoder", "setFrontFacingWinding:",  samp_set_int },
    { "MTLRenderCommandEncoder", "drawPrimitives:vertexStart:vertexCount:", enc_draw },
    { "MTLRenderCommandEncoder",
      "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
      enc_draw_indexed },
    { "MTLRenderCommandEncoder", "endEncoding",             enc_end },

    /* blit encoder */
    { "MTLBlitCommandEncoder",
      "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:"
      "sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
      blit_copy_buf_to_tex },
    { "MTLBlitCommandEncoder", "generateMipmapsForTexture:", blit_noop_tex },
    /* Managed-storage sync: our textures are plain memory, already coherent. */
    { "MTLBlitCommandEncoder", "synchronizeResource:",        blit_noop_tex },
    { "MTLBlitCommandEncoder", "endEncoding",               enc_end },

    /* buffers and textures */
    { "MTLBuffer", "contents",                             buf_contents },
    { "MTLBuffer", "length",                               buf_length },
    { "MTLBuffer", "didModifyRange:",                      buf_did_modify },
    { "MTLTexture", "width",                               tex_width },
    { "MTLTexture", "height",                              tex_height },
    { "MTLTexture", "pixelFormat",                         tex_pixfmt },
    { "MTLTexture", "replaceRegion:mipmapLevel:withBytes:bytesPerRow:",
                                                           tex_replace_region },

    /* descriptors */
    { "MTLTextureDescriptor",
      "+texture2DDescriptorWithPixelFormat:width:height:mipmapped:", texdesc_2d },
    { "MTLTextureDescriptor", "setPixelFormat:", td_set_fmt },
    { "MTLTextureDescriptor", "setWidth:",     td_set_w },
    { "MTLTextureDescriptor", "setHeight:",    td_set_h },
    { "MTLTextureDescriptor", "setUsage:",     td_set_usage },
    { "MTLTextureDescriptor", "setStorageMode:", td_set_storage },
    { "MTLTextureDescriptor", "storageMode",     td_storage },

    { "MTLRenderPassDescriptor", "+renderPassDescriptor", rpdesc_new },
    { "MTLRenderPassDescriptor", "+new",                  rpdesc_new },
    { "MTLRenderPassDescriptor", "colorAttachments",      rp_color_attachments },
    { "MTLRenderPassDescriptor", "stencilAttachment",     rp_stencil_attachment },

    { "MTLRenderPipelineDescriptor", "+new",              pipedesc_new },
    { "MTLRenderPipelineDescriptor", "+renderPipelineDescriptor", pipedesc_new },
    { "MTLRenderPipelineDescriptor", "colorAttachments",  pd_color_attachments },
    { "MTLRenderPipelineDescriptor", "setVertexFunction:",   pd_set_vfn },
    { "MTLRenderPipelineDescriptor", "setFragmentFunction:", pd_set_ffn },
    { "MTLRenderPipelineDescriptor", "setVertexDescriptor:", pd_set_vdesc },
    { "MTLRenderPipelineDescriptor", "setStencilAttachmentPixelFormat:",
                                                          pd_set_stencil_fmt },

    { "MTLAttachmentArray", "objectAtIndexedSubscript:",  arr_subscript },

    { "MTLAttachment", "setTexture:",          at_set_tex },
    { "MTLAttachment", "texture",              at_get_tex },
    { "MTLAttachment", "setLoadAction:",       at_set_load },
    { "MTLAttachment", "setStoreAction:",      at_set_store },
    { "MTLAttachment", "setClearStencil:",     at_set_clear_stencil },
    { "MTLAttachment", "setClearColor:",       at_set_clear_color },
    { "MTLAttachment", "setPixelFormat:",      at_set_pixfmt },
    { "MTLAttachment", "setBlendingEnabled:",  at_set_blend },
    { "MTLAttachment", "setSourceRGBBlendFactor:",      at_set_src_rgb },
    { "MTLAttachment", "setDestinationRGBBlendFactor:", at_set_dst_rgb },
    { "MTLAttachment", "setSourceAlphaBlendFactor:",      at_set_src_a },
    { "MTLAttachment", "setDestinationAlphaBlendFactor:", at_set_dst_a },
    { "MTLAttachment", "setWriteMask:",           at_set_write_mask },
    { "MTLAttachment", "setRgbBlendOperation:",   samp_set_int },
    { "MTLAttachment", "setAlphaBlendOperation:", samp_set_int },

    { "MTLDepthStencilDescriptor", "setFrontFaceStencil:", ds_set_front },
    { "MTLDepthStencilDescriptor", "setBackFaceStencil:",  ds_set_back },
    { "MTLDepthStencilDescriptor", "setDepthCompareFunction:", ds_set_depth_cmp },
    { "MTLDepthStencilDescriptor", "setDepthWriteEnabled:",    at_set_blend },

    { "MTLStencilDescriptor", "+new",                          sdesc_new },
    { "MTLStencilDescriptor", "init",                          sdesc_init },
    { "MTLStencilDescriptor", "setStencilCompareFunction:",     sd_set_cmp },
    { "MTLStencilDescriptor", "setStencilFailureOperation:",    sd_set_sfail },
    { "MTLStencilDescriptor", "setDepthFailureOperation:",      sd_set_dpfail },
    { "MTLStencilDescriptor", "setDepthStencilPassOperation:",  sd_set_dppass },
    { "MTLStencilDescriptor", "setReadMask:",                   sd_set_rmask },
    { "MTLStencilDescriptor", "setWriteMask:",                  sd_set_wmask },

    { "MTLSamplerDescriptor", "+new",              samp_new },
    { "MTLSamplerDescriptor", "setMinFilter:",     samp_set_int },
    { "MTLSamplerDescriptor", "setMagFilter:",     samp_set_int },
    { "MTLSamplerDescriptor", "setMipFilter:",     samp_set_int },
    { "MTLSamplerDescriptor", "setSAddressMode:",  samp_set_int },
    { "MTLSamplerDescriptor", "setTAddressMode:",  samp_set_int },

    { "MTLVertexDescriptor", "+vertexDescriptor",  vdesc_new },
    { "MTLVertexDescriptor", "+new",               vdesc_new },
    { "MTLVertexDescriptor", "attributes",         vdesc_arr },
    { "MTLVertexDescriptor", "layouts",            vdesc_arr },
    { "MTLVertexArray", "objectAtIndexedSubscript:", vdesc_sub },
    { "MTLVertexItem", "setFormat:",       vitem_set_int },
    { "MTLVertexItem", "setOffset:",       vitem_set_int },
    { "MTLVertexItem", "setBufferIndex:",  vitem_set_int },
    { "MTLVertexItem", "setStride:",       vitem_set_int },
    { "MTLVertexItem", "setStepFunction:", vitem_set_int },

    /* the layer and its drawable */
    { "CAMetalLayer", "+layer",              layer_new },
    { "CAMetalLayer", "+new",                layer_new },
    { "CAMetalLayer", "device",              layer_device },
    { "CAMetalLayer", "setDevice:",          layer_set_device },
    { "CAMetalLayer", "setPixelFormat:",     layer_set_pixfmt },
    { "CAMetalLayer", "pixelFormat",         tex_pixfmt },
    { "CAMetalLayer", "setOpaque:",          layer_set_flag },
    { "CAMetalLayer", "setFramebufferOnly:", layer_set_flag },
    { "CAMetalLayer", "setPresentsWithTransaction:", layer_set_flag },
    { "CAMetalLayer", "presentsWithTransaction",     dev_false },
    { "CAMetalLayer", "setDrawableSize:",    layer_set_drawable_size },
    { "CAMetalLayer", "drawableSize",        layer_drawable_size },
    { "CAMetalLayer", "contentsScale",       layer_scale },
    { "CAMetalLayer", "setContentsScale:",   layer_set_scale },
    { "CAMetalLayer", "nextDrawable",        layer_next_drawable },
    { "CAMetalLayer", "setNeedsDisplay",     layer_void },
    { "CAMetalLayer", "setMaximumDrawableCount:", samp_set_int },

    { "CAMetalDrawable", "texture",          drawable_texture },
    { "CAMetalDrawable", "present",          drawable_present },

    { NULL, NULL, NULL }
};

void macmetal_install(void)
{
    int i;
    static const char *const classes[] = {
        "MTLDevice", "MTLCommandQueue", "MTLCommandBuffer",
        "MTLRenderCommandEncoder", "MTLBlitCommandEncoder", "MTLBuffer",
        "MTLTexture", "MTLLibrary", "MTLFunction", "MTLRenderPipelineState",
        "MTLDepthStencilState", "MTLSamplerState", "MTLAttachment",
        "MTLAttachmentArray", "MTLVertexArray", "MTLVertexItem",
        "CAMetalDrawable", "MTLTextureDescriptor", "MTLRenderPassDescriptor",
        "MTLRenderPipelineDescriptor", "MTLDepthStencilDescriptor",
        "MTLStencilDescriptor", "MTLSamplerDescriptor", "MTLVertexDescriptor",
        "CAMetalLayer", NULL
    };
    for (i = 0; classes[i]; i++) macobjc_define_class(classes[i]);
    for (i = 0; g_table[i].cls; i++)
        macobjc_add_method(g_table[i].cls, g_table[i].sel, g_table[i].imp);
}
