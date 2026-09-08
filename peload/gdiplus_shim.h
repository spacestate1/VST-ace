/* GDI+, enough of it for a plug-in to draw its own interface.
 *
 * A VSTGUI- or SynthEdit-era plug-in that is not using Direct2D draws
 * everything through GDI+: paths, gradients, text, and bitmaps blitted with a
 * colour matrix. Ninety-six entry points in the corpus here, and none of them
 * existed -- worse, GDI+ spells success as 0, which is what an unimplemented
 * stub returns, so a plug-in was told every call had worked and then faulted
 * on the handle it was never given.
 *
 * What backs it is already in this tree. A DC's pixels are a w32_surf, a flat
 * BGRA buffer (win32gui.h); glyphs come from the same FreeType face the
 * DirectWrite shim measures with; and PNG and BMP decode through png_in.h. So
 * this file is the mapping between GDI+'s object model and those, plus the one
 * piece genuinely missing: a scanline rasteriser for filled and stroked paths.
 *
 * The rasteriser flattens curves to line segments and fills by scanline with
 * either winding rule, sampling four times vertically so edges are not jagged.
 * That is a deliberate stopping point: it is what makes a knob and a panel look
 * right, and it is not a general-purpose 2D library.
 *
 * Every entry point returns GpStatus, where 0 is Ok. Coordinates are float.
 * Colours are ARGB, which is the byte order a w32_surf already stores.
 */
#ifndef PELOAD_GDIPLUS_SHIM_H
#define PELOAD_GDIPLUS_SHIM_H

#define GP_OK            0
#define GP_GENERIC       1
#define GP_INVALIDARG    2
#define GP_OUTOFMEMORY   3

/* ---- objects ------------------------------------------------------------
 *
 * GDI+ handles are opaque pointers, so each object carries a tag: a plug-in
 * that hands back the wrong kind is caught here rather than corrupting the
 * one it is mistaken for. */
enum {
    GPO_GRAPHICS = 0x47500001, GPO_PATH, GPO_PEN, GPO_BRUSH,
    GPO_FONT, GPO_FAMILY, GPO_MATRIX, GPO_IMAGE, GPO_IMAGEATTR, GPO_STRINGFORMAT,
    GPO_REGION, GPO_CACHED
};

typedef struct { float m[6]; } gp_matrix;          /* a b c d e f, as GDI+ */

typedef struct {
    int      tag;
    float   *pt;                    /* x,y pairs */
    uint8_t *ty;                    /* 0 start, 1 line, 0x80 closes the figure */
    int      n, cap;
    int      fillmode;              /* 0 alternate (even-odd), 1 winding */
    int      open;                  /* a figure is in progress */
} gp_path;

typedef struct { int tag; uint32_t argb; float width; int join, cap; } gp_pen;

enum { GPB_SOLID = 0, GPB_LINEAR, GPB_PATHGRAD };
typedef struct {
    int      tag, kind;
    uint32_t argb;                  /* solid, and the centre of a gradient */
    uint32_t argb2;                 /* the far end                          */
    float    x0, y0, x1, y1;        /* linear: the axis                     */
} gp_brush;

/* A region, as far as this layer needs one: a rectangle, or "everything".
 *
 * GDI+ regions are arbitrary areas -- unions and differences of paths -- and
 * modelling that properly means a scanline set. What a plug-in editor does with
 * one is narrower: it asks the graphics for its current clip, saves it, clips
 * to a control's rectangle, draws, and puts the old one back. That is
 * rectangles throughout, and it is also exactly what the clip on gp_graphics
 * already is, so a region here is the same four numbers plus a flag for the
 * unbounded case a fresh region starts in.
 *
 * A combine that cannot be expressed as one rectangle (a union of two
 * disjoint ones, an exclusion) widens to the bounding box, which clips less
 * than GDI+ would. Drawing slightly outside a clip is visible only where a
 * plug-in relies on the clip to erase; drawing nothing at all, which is what
 * these returning "not implemented" did, is visible everywhere. */
typedef struct { int tag; int infinite; float x0, y0, x1, y1; } gp_region;

typedef struct { int tag; char name[64]; } gp_family;
typedef struct { int tag; float size; int style; gp_family *fam; } gp_font;
typedef struct { int tag; int w, h; uint32_t *px; int owns; } gp_image;
typedef struct { int tag; float m[5][5]; int has_matrix; } gp_imageattr;

typedef struct {
    int        tag;
    w32_surf  *surf;
    gp_image  *img;                 /* when drawing into a bitmap instead */
    gp_matrix  xf;
    int        has_clip;
    float      cx0, cy0, cx1, cy1;
    int        smoothing;
} gp_graphics;

static void *gp_new(size_t n, int tag)
{
    int *p = calloc(1, n);
    if (p) *p = tag;
    return p;
}
static void *gp_check(void *o, int tag)
{ return (o && *(int *)o == tag) ? o : NULL; }

/* ---- the surface a graphics object writes to --------------------------- */

static uint32_t *gp_pixels(gp_graphics *g, int *w, int *h)
{
    if (!g) return NULL;
    if (g->img) { *w = g->img->w; *h = g->img->h; return g->img->px; }
    if (g->surf) { *w = g->surf->w; *h = g->surf->h; return g->surf->px; }
    return NULL;
}

/* ---- colour ------------------------------------------------------------- */

static void gp_blend(uint32_t *dst, uint32_t argb, int cov)
{
    uint32_t a = ((argb >> 24) & 0xFF) * (uint32_t)cov / 255u;
    uint32_t sr, sg, sb, dr, dg, db;
    if (!a) return;
    if (a == 255) { *dst = argb | 0xFF000000u; return; }
    sr = (argb >> 16) & 0xFF; sg = (argb >> 8) & 0xFF; sb = argb & 0xFF;
    dr = (*dst >> 16) & 0xFF; dg = (*dst >> 8) & 0xFF; db = *dst & 0xFF;
    dr = (sr * a + dr * (255 - a)) / 255;
    dg = (sg * a + dg * (255 - a)) / 255;
    db = (sb * a + db * (255 - a)) / 255;
    *dst = 0xFF000000u | (dr << 16) | (dg << 8) | db;
}

/* ---- the transform ------------------------------------------------------ */

static void gp_ident(gp_matrix *m)
{ m->m[0] = 1; m->m[1] = 0; m->m[2] = 0; m->m[3] = 1; m->m[4] = 0; m->m[5] = 0; }

static void gp_apply(const gp_matrix *m, float x, float y, float *ox, float *oy)
{
    *ox = m->m[0] * x + m->m[2] * y + m->m[4];
    *oy = m->m[1] * x + m->m[3] * y + m->m[5];
}

/* ---- paths -------------------------------------------------------------- */

static int gp_path_room(gp_path *p, int extra)
{
    int need = p->n + extra;
    if (need <= p->cap) return 1;
    {
        int cap = p->cap ? p->cap * 2 : 64;
        float *pt;
        uint8_t *ty;
        while (cap < need) cap *= 2;
        if (!(pt = realloc(p->pt, (size_t)cap * 2 * sizeof *pt))) return 0;
        p->pt = pt;
        if (!(ty = realloc(p->ty, (size_t)cap))) return 0;
        p->ty = ty;
        p->cap = cap;
    }
    return 1;
}
static void gp_pt(gp_path *p, float x, float y, uint8_t t)
{
    if (!gp_path_room(p, 1)) return;
    p->pt[p->n * 2] = x; p->pt[p->n * 2 + 1] = y;
    p->ty[p->n] = t;
    p->n++;
}
static void gp_moveto(gp_path *p, float x, float y) { gp_pt(p, x, y, 0); p->open = 1; }
static void gp_lineto(gp_path *p, float x, float y)
{
    if (!p->n) { gp_moveto(p, x, y); return; }
    gp_pt(p, x, y, 1);
}
/* Curves are flattened rather than rasterised directly: a cubic at this size
 * is indistinguishable from sixteen segments, and the fill below only has to
 * understand straight edges. */
static void gp_bezier(gp_path *p, float x1, float y1, float cx1, float cy1,
                      float cx2, float cy2, float x2, float y2)
{
    int i;
    for (i = 1; i <= 16; i++) {
        float t = (float)i / 16.0f, u = 1.0f - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        gp_lineto(p, a * x1 + b * cx1 + c * cx2 + d * x2,
                     a * y1 + b * cy1 + c * cy2 + d * y2);
    }
}
static void gp_arc(gp_path *p, float x, float y, float w, float h,
                   float start, float sweep)
{
    float cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
    int i, steps = (int)(fabsf(sweep) / 6.0f) + 4;
    for (i = 0; i <= steps; i++) {
        float a = (start + sweep * (float)i / (float)steps) * 3.14159265f / 180.0f;
        float px = cx + rx * cosf(a), py = cy + ry * sinf(a);
        if (i == 0 && !p->open) gp_moveto(p, px, py); else gp_lineto(p, px, py);
    }
}

/* ---- the rasteriser ----------------------------------------------------- */

#define GP_SS 4                       /* vertical samples per pixel */

/* Fill the path's transformed outline. `shade` supplies the colour per pixel,
 * which is what lets one routine serve solid fills and both gradients. */
typedef uint32_t (*gp_shader)(void *ctx, int x, int y);

static void gp_fill_shaded(gp_graphics *g, gp_path *p, gp_shader shade, void *ctx)
{
    int W, H, i, y, sub, x0, x1;
    uint32_t *px = gp_pixels(g, &W, &H);
    float *xs = NULL, minY = 1e30f, maxY = -1e30f, minX = 1e30f, maxX = -1e30f;
    uint8_t *cov;
    int *wind;

    if (!px || !p || p->n < 2) return;
    if (!(cov = calloc((size_t)W, 1))) return;
    if (!(wind = calloc((size_t)W + 1, sizeof *wind))) { free(cov); return; }
    if (!(xs = malloc((size_t)p->n * 2 * sizeof *xs))) { free(cov); free(wind); return; }

    /* Transform once, then work in device space. */
    for (i = 0; i < p->n; i++) {
        gp_apply(&g->xf, p->pt[i * 2], p->pt[i * 2 + 1], &xs[i * 2], &xs[i * 2 + 1]);
        if (xs[i * 2 + 1] < minY) minY = xs[i * 2 + 1];
        if (xs[i * 2 + 1] > maxY) maxY = xs[i * 2 + 1];
        if (xs[i * 2] < minX) minX = xs[i * 2];
        if (xs[i * 2] > maxX) maxX = xs[i * 2];
    }
    if (minY < 0) minY = 0;
    if (maxY > (float)H) maxY = (float)H;

    /* The columns this shape can touch, so the per-scanline work is the width
     * of the shape rather than the width of the surface. A knob twenty pixels
     * across on a four-hundred-pixel panel was clearing and walking the whole
     * four hundred, four times, for every one of its rows.
     *
     * Exactly the same pixels come out. A crossing is a linear interpolation
     * between two of the path's own points, so it cannot fall outside their x
     * range; left of the first crossing the winding count is still zero, which
     * is "outside" under either fill rule; and right of the last one it is zero
     * again because every figure is closed, so the crossings cancel. The two
     * clamps are where that reasoning would fail -- a crossing off the left edge
     * lands on column 0, and one off the right lands on column W -- so a shape
     * that runs past either edge widens the range back to it. */
    x0 = (minX < 0.0f) ? 0 : (int)minX - 1;
    x1 = (maxX >= (float)W) ? W - 1 : (int)maxX + 1;
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (x1 < x0) { free(xs); free(cov); free(wind); return; }

    for (y = (int)minY; y < (int)maxY + 1 && y < H; y++) {
        if (y < 0) continue;
        memset(cov + x0, 0, (size_t)(x1 - x0 + 1));
        for (sub = 0; sub < GP_SS; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) / GP_SS;
            int start = 0, x;
            memset(wind + x0, 0, (size_t)(x1 - x0 + 2) * sizeof *wind);
            /* Walk each figure's edges, wrapping the last point to the first:
             * GDI+ closes a figure for filling whether or not it was closed
             * explicitly. */
            for (i = 0; i < p->n; i++) {
                int j;
                if (p->ty[i] == 0) start = i;
                j = (i + 1 < p->n && p->ty[i + 1] != 0) ? i + 1 : start;
                {
                    float ax = xs[i * 2], ay = xs[i * 2 + 1];
                    float bx = xs[j * 2], by = xs[j * 2 + 1];
                    int dir = 1;
                    if (ay == by) continue;
                    if (ay > by) { float t; t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; dir = -1; }
                    if (sy < ay || sy >= by) continue;
                    {
                        float ix = ax + (bx - ax) * (sy - ay) / (by - ay);
                        /* Truncation, not floorf. They differ only for a
                         * negative result, and every negative result is clamped
                         * to column 0 on the next line either way -- so the
                         * clamped answer is identical and the libm call goes,
                         * which was an eighth of the time spent drawing. */
                        int xi = (int)(ix + 0.5f);
                        if (xi < 0) xi = 0;
                        if (xi > W) xi = W;
                        wind[xi] += dir;
                    }
                }
            }
            {   /* Accumulate the crossings into spans. */
                int acc = 0, inside;
                for (x = x0; x <= x1; x++) {
                    acc += wind[x];
                    inside = p->fillmode ? (acc != 0) : (acc & 1);
                    if (inside && cov[x] < 255)
                        cov[x] = (uint8_t)(cov[x] + 255 / GP_SS);
                }
            }
        }
        for (i = x0; i <= x1; i++) {
            if (!cov[i]) continue;
            if (g->has_clip &&
                ((float)i < g->cx0 || (float)i >= g->cx1 ||
                 (float)y < g->cy0 || (float)y >= g->cy1)) continue;
            gp_blend(&px[(size_t)y * W + i], shade(ctx, i, y), cov[i]);
        }
    }
    free(xs); free(cov); free(wind);
}

static uint32_t gp_shade_solid(void *ctx, int x, int y)
{ (void)x; (void)y; return *(uint32_t *)ctx; }

static uint32_t gp_shade_brush(void *ctx, int x, int y)
{
    gp_brush *b = ctx;
    float t = 0.5f;
    uint32_t c0 = b->argb, c1 = b->argb2;
    if (b->kind == GPB_SOLID) return b->argb;
    if (b->kind == GPB_LINEAR) {
        float dx = b->x1 - b->x0, dy = b->y1 - b->y0;
        float len2 = dx * dx + dy * dy;
        t = len2 > 0 ? (((float)x - b->x0) * dx + ((float)y - b->y0) * dy) / len2 : 0.0f;
    }
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    {
        uint32_t out = 0;
        int s;
        for (s = 0; s < 4; s++) {
            uint32_t a = (c0 >> (s * 8)) & 0xFF, bb = (c1 >> (s * 8)) & 0xFF;
            out |= (uint32_t)((float)a + ((float)bb - (float)a) * t) << (s * 8);
        }
        return out;
    }
}

static void gp_fill_path_brush(gp_graphics *g, gp_path *p, gp_brush *b)
{
    if (!b) return;
    if (b->kind == GPB_SOLID) gp_fill_shaded(g, p, gp_shade_solid, &b->argb);
    else                      gp_fill_shaded(g, p, gp_shade_brush, b);
}

/* A stroke is drawn as a filled quad per segment. Joins are not mitred: at the
 * widths a plug-in uses for a panel outline or a knob pointer the difference
 * is invisible, and a proper join needs the whole outline offset. */
static void gp_stroke(gp_graphics *g, gp_path *p, gp_pen *pen)
{
    int i, start = 0;
    float hw;
    if (!p || !pen || p->n < 2) return;
    hw = pen->width > 0 ? pen->width / 2 : 0.5f;
    if (hw < 0.5f) hw = 0.5f;
    for (i = 0; i < p->n; i++) {
        int j;
        gp_path q;
        float ax, ay, bx, by, dx, dy, len, nx, ny;
        if (p->ty[i] == 0) { start = i; }
        j = i + 1;
        if (j >= p->n || p->ty[j] == 0) {
            if (!(p->ty[i] & 0x80)) continue;      /* an open figure ends here */
            j = start;
        }
        ax = p->pt[i * 2]; ay = p->pt[i * 2 + 1];
        bx = p->pt[j * 2]; by = p->pt[j * 2 + 1];
        dx = bx - ax; dy = by - ay;
        len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f) continue;
        nx = -dy / len * hw; ny = dx / len * hw;
        memset(&q, 0, sizeof q);
        q.fillmode = 1;
        gp_moveto(&q, ax + nx, ay + ny);
        gp_lineto(&q, bx + nx, by + ny);
        gp_lineto(&q, bx - nx, by - ny);
        gp_lineto(&q, ax - nx, ay - ny);
        gp_fill_shaded(g, &q, gp_shade_solid, &pen->argb);
        free(q.pt); free(q.ty);
    }
}



/* ---- the flat API ------------------------------------------------------- */

/* Startup. The token is what Shutdown is handed back; anything non-zero will
 * do, since there is no global state to tear down. */
static MS int32_t st_GdiplusStartup(void *token, const void *in, void *out)
{
    (void)in;
    if (token) *(uintptr_t *)token = 1;
    /* GdiplusStartupOutput is two callbacks for the "suppress background
     * thread" mode. Nothing here runs one, so leaving them null is correct --
     * a caller that asked for that mode checks them before calling. */
    if (out) memset(out, 0, 2 * sizeof(void *));
    return GP_OK;
}
static MS void st_GdiplusShutdown(uintptr_t token) { (void)token; }

/* ---- paths -------------------------------------------------------------- */

static MS int32_t st_GdipCreatePath(int32_t mode, void **out)
{
    gp_path *p;
    if (!out) return GP_INVALIDARG;
    if (!(p = gp_new(sizeof *p, GPO_PATH))) return GP_OUTOFMEMORY;
    p->fillmode = mode;
    *out = p;
    return GP_OK;
}
static MS int32_t st_GdipClonePath(void *path, void **out)
{
    gp_path *p = gp_check(path, GPO_PATH), *q;
    if (!p || !out) return GP_INVALIDARG;
    if (!(q = gp_new(sizeof *q, GPO_PATH))) return GP_OUTOFMEMORY;
    q->fillmode = p->fillmode;
    if (p->n && gp_path_room(q, p->n)) {
        memcpy(q->pt, p->pt, (size_t)p->n * 2 * sizeof *q->pt);
        memcpy(q->ty, p->ty, (size_t)p->n);
        q->n = p->n;
    }
    *out = q;
    return GP_OK;
}
static MS int32_t st_GdipDeletePath(void *path)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p) return GP_INVALIDARG;
    free(p->pt); free(p->ty); free(p);
    return GP_OK;
}
static MS int32_t st_GdipStartPathFigure(void *path)
{ gp_path *p = gp_check(path, GPO_PATH); if (p) p->open = 0; return p ? GP_OK : GP_INVALIDARG; }
static MS int32_t st_GdipClosePathFigure(void *path)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p) return GP_INVALIDARG;
    if (p->n) p->ty[p->n - 1] |= 0x80;
    p->open = 0;
    return GP_OK;
}
static MS int32_t st_GdipGetPathLastPoint(void *path, float *pt)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p || !pt || !p->n) return GP_INVALIDARG;
    pt[0] = p->pt[(p->n - 1) * 2];
    pt[1] = p->pt[(p->n - 1) * 2 + 1];
    return GP_OK;
}
static MS int32_t st_GdipAddPathLine(void *path, float x1, float y1, float x2, float y2)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p) return GP_INVALIDARG;
    if (!p->open) gp_moveto(p, x1, y1); else gp_lineto(p, x1, y1);
    gp_lineto(p, x2, y2);
    return GP_OK;
}
static MS int32_t st_GdipAddPathArc(void *path, float x, float y, float w, float h,
                                    float start, float sweep)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p) return GP_INVALIDARG;
    gp_arc(p, x, y, w, h, start, sweep);
    return GP_OK;
}
static MS int32_t st_GdipAddPathBezier(void *path, float x1, float y1, float x2, float y2,
                                       float x3, float y3, float x4, float y4)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p) return GP_INVALIDARG;
    if (!p->open) gp_moveto(p, x1, y1);
    gp_bezier(p, x1, y1, x2, y2, x3, y3, x4, y4);
    return GP_OK;
}
static MS int32_t st_GdipAddPathRectangle(void *path, float x, float y, float w, float h)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p) return GP_INVALIDARG;
    gp_moveto(p, x, y);
    gp_lineto(p, x + w, y);
    gp_lineto(p, x + w, y + h);
    gp_lineto(p, x, y + h);
    if (p->n) p->ty[p->n - 1] |= 0x80;
    p->open = 0;
    return GP_OK;
}
static MS int32_t st_GdipAddPathEllipse(void *path, float x, float y, float w, float h)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p) return GP_INVALIDARG;
    p->open = 0;
    gp_arc(p, x, y, w, h, 0, 360);
    if (p->n) p->ty[p->n - 1] |= 0x80;
    p->open = 0;
    return GP_OK;
}
static MS int32_t st_GdipSetPathFillMode(void *path, int32_t mode)
{ gp_path *p = gp_check(path, GPO_PATH); if (p) p->fillmode = mode; return p ? GP_OK : GP_INVALIDARG; }

static MS int32_t st_GdipGetPathWorldBounds(void *path, float *rc, void *m, void *pen)
{
    gp_path *p = gp_check(path, GPO_PATH);
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    int i;
    (void)m; (void)pen;
    if (!p || !rc) return GP_INVALIDARG;
    for (i = 0; i < p->n; i++) {
        float x = p->pt[i * 2], y = p->pt[i * 2 + 1];
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
    }
    if (!p->n) { x0 = y0 = x1 = y1 = 0; }
    rc[0] = x0; rc[1] = y0; rc[2] = x1 - x0; rc[3] = y1 - y0;
    return GP_OK;
}
static MS int32_t st_GdipTransformPath(void *path, void *matrix)
{
    gp_path *p = gp_check(path, GPO_PATH);
    gp_matrix *m = gp_check(matrix, GPO_MATRIX) ? (gp_matrix *)((int *)matrix + 1) : NULL;
    int i;
    if (!p) return GP_INVALIDARG;
    if (!m) return GP_OK;
    for (i = 0; i < p->n; i++) {
        float ox, oy;
        gp_apply(m, p->pt[i * 2], p->pt[i * 2 + 1], &ox, &oy);
        p->pt[i * 2] = ox; p->pt[i * 2 + 1] = oy;
    }
    return GP_OK;
}
/* Point-in-path, by the same winding rule the fill uses. A plug-in hit-tests
 * its own controls with this, so a wrong answer is a dial that cannot be
 * grabbed rather than something visible. */
static MS int32_t st_GdipIsVisiblePathPoint(void *path, float x, float y,
                                            void *graphics, int32_t *out)
{
    gp_path *p = gp_check(path, GPO_PATH);
    int i, start = 0, acc = 0;
    (void)graphics;
    if (!p || !out) return GP_INVALIDARG;
    for (i = 0; i < p->n; i++) {
        int j;
        float ax, ay, bx, by;
        if (p->ty[i] == 0) start = i;
        j = (i + 1 < p->n && p->ty[i + 1] != 0) ? i + 1 : start;
        ax = p->pt[i * 2]; ay = p->pt[i * 2 + 1];
        bx = p->pt[j * 2]; by = p->pt[j * 2 + 1];
        if ((ay <= y && by > y) || (by <= y && ay > y)) {
            float ix = ax + (bx - ax) * (y - ay) / (by - ay);
            if (ix > x) acc += (by > ay) ? 1 : -1;
        }
    }
    *out = p->fillmode ? (acc != 0) : (acc & 1);
    return GP_OK;
}

/* ---- graphics ----------------------------------------------------------- */

static gp_graphics *gp_graphics_new(w32_surf *surf, gp_image *img)
{
    gp_graphics *g = gp_new(sizeof *g, GPO_GRAPHICS);
    if (!g) return NULL;
    g->surf = surf; g->img = img;
    gp_ident(&g->xf);
    return g;
}
static MS int32_t st_GdipCreateFromHDC(void *hdc, void **out)
{
    w32_dc *d = w32_dcget(hdc);
    static w32_surf tmp;
    w32_surf *s;
    if (!out) return GP_INVALIDARG;
    if (!(s = w32_target_in(d, &tmp))) return GP_GENERIC;
    *out = gp_graphics_new(s, NULL);
    return *out ? GP_OK : GP_OUTOFMEMORY;
}
static MS int32_t st_GdipCreateFromHWND(void *hwnd, void **out)
{
    w32_wnd *w = w32_wget(hwnd);
    if (!out) return GP_INVALIDARG;
    if (!w) return GP_GENERIC;
    *out = gp_graphics_new(&w->surf, NULL);
    return *out ? GP_OK : GP_OUTOFMEMORY;
}
static MS int32_t st_GdipGetImageGraphicsContext(void *image, void **out)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im || !out) return GP_INVALIDARG;
    *out = gp_graphics_new(NULL, im);
    return *out ? GP_OK : GP_OUTOFMEMORY;
}
static MS int32_t st_GdipDeleteGraphics(void *g)
{ if (!gp_check(g, GPO_GRAPHICS)) return GP_INVALIDARG; free(g); return GP_OK; }

static MS int32_t st_GdipSetClipRect(void *graphics, float x, float y, float w, float h,
                                     int32_t combine)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    float x0, y0, x1, y1;
    (void)combine;
    if (!g) return GP_INVALIDARG;
    gp_apply(&g->xf, x, y, &x0, &y0);
    gp_apply(&g->xf, x + w, y + h, &x1, &y1);
    g->cx0 = x0; g->cy0 = y0; g->cx1 = x1; g->cy1 = y1;
    g->has_clip = 1;
    return GP_OK;
}
/* The integer form of the same call. A plug-in that clips with it and gets
 * GDI+'s "not implemented" back gives up on the whole draw -- OrilRiver painted
 * nothing at all and reached exactly one stub, this one. */
static MS int32_t st_GdipSetClipRectI(void *graphics, int32_t x, int32_t y,
                                      int32_t w, int32_t h, int32_t combine)
{ return st_GdipSetClipRect(graphics, (float)x, (float)y, (float)w, (float)h, combine); }

/* Save and restore, which is how a GDI+ caller brackets a change to the
 * transform or the clip. There is no depth limit in GDI+; a small stack covers
 * what a plug-in's paint routine nests and reports failure rather than
 * silently restoring the wrong state. */
#define GP_STATE_MAX 32
typedef struct { gp_matrix xf; int has_clip; float cx0, cy0, cx1, cy1; int smoothing; } gp_state;
static gp_state g_gp_states[GP_STATE_MAX];
static int      g_gp_nstates;

static MS int32_t st_GdipSaveGraphics(void *graphics, uint32_t *state)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_state *st;
    if (!g || !state) return GP_INVALIDARG;
    /* Written before anything can fail. Gdiplus::Graphics::Save() hands its
     * caller the GraphicsState whatever the status says, so a failure that
     * left it alone would return whatever was on the stack -- and a garbage
     * value that happens to land in 1..nstates truncates the stack and
     * restores some other state. Zero is the one value Restore refuses. */
    *state = 0;
    if (g_gp_nstates >= GP_STATE_MAX) return GP_INVALIDARG;
    st = &g_gp_states[g_gp_nstates];
    st->xf = g->xf;
    st->has_clip = g->has_clip;
    st->cx0 = g->cx0; st->cy0 = g->cy0; st->cx1 = g->cx1; st->cy1 = g->cy1;
    st->smoothing = g->smoothing;
    *state = (uint32_t)(++g_gp_nstates);
    return GP_OK;
}
static MS int32_t st_GdipRestoreGraphics(void *graphics, uint32_t state)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_state *st;
    if (!g || state == 0 || state > (uint32_t)g_gp_nstates) return GP_INVALIDARG;
    g_gp_nstates = (int)state - 1;
    st = &g_gp_states[g_gp_nstates];
    g->xf = st->xf;
    g->has_clip = st->has_clip;
    g->cx0 = st->cx0; g->cy0 = st->cy0; g->cx1 = st->cx1; g->cy1 = st->cy1;
    g->smoothing = st->smoothing;
    return GP_OK;
}

static MS int32_t st_GdipSetPageUnit(void *g, int32_t unit) { (void)g; (void)unit; return GP_OK; }
static MS int32_t st_GdipGetDpiY(void *g, float *dpi)
{ (void)g; if (dpi) *dpi = 96.0f; return GP_OK; }
static MS int32_t st_GdipSetSmoothingMode(void *graphics, int32_t m)
{ gp_graphics *g = gp_check(graphics, GPO_GRAPHICS); if (g) g->smoothing = m; return GP_OK; }
static MS int32_t st_GdipSetInterpolationMode(void *g, int32_t m) { (void)g; (void)m; return GP_OK; }
static MS int32_t st_GdipSetTextRenderingHint(void *g, int32_t m) { (void)g; (void)m; return GP_OK; }
static MS int32_t st_GdipSetPixelOffsetMode(void *g, int32_t m) { (void)g; (void)m; return GP_OK; }

static MS int32_t st_GdipGetWorldTransform(void *graphics, void *matrix)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    if (!g || !gp_check(matrix, GPO_MATRIX)) return GP_INVALIDARG;
    memcpy((int *)matrix + 1, &g->xf, sizeof g->xf);
    return GP_OK;
}
static MS int32_t st_GdipSetWorldTransform(void *graphics, void *matrix)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    if (!g || !gp_check(matrix, GPO_MATRIX)) return GP_INVALIDARG;
    memcpy(&g->xf, (int *)matrix + 1, sizeof g->xf);
    return GP_OK;
}
static MS int32_t st_GdipTranslateWorldTransform(void *graphics, float dx, float dy,
                                                 int32_t order)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    (void)order;
    if (!g) return GP_INVALIDARG;
    g->xf.m[4] += g->xf.m[0] * dx + g->xf.m[2] * dy;
    g->xf.m[5] += g->xf.m[1] * dx + g->xf.m[3] * dy;
    return GP_OK;
}

/* ---- drawing ------------------------------------------------------------ */

static MS int32_t st_GdipFillPath(void *graphics, void *brush, void *path)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_brush *b = gp_check(brush, GPO_BRUSH);
    gp_path *p = gp_check(path, GPO_PATH);
    if (!g || !b || !p) return GP_INVALIDARG;
    gp_fill_path_brush(g, p, b);
    return GP_OK;
}
static MS int32_t st_GdipDrawPath(void *graphics, void *pen, void *path)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_pen *pn = gp_check(pen, GPO_PEN);
    gp_path *p = gp_check(path, GPO_PATH);
    if (!g || !pn || !p) return GP_INVALIDARG;
    gp_stroke(g, p, pn);
    return GP_OK;
}
/* The rectangle, ellipse, polygon and line calls all build a temporary path
 * and go through the same two routines. */
static int32_t gp_shape(void *graphics, void *style, int is_pen, int kind,
                        float x, float y, float w, float h,
                        const float *pts, int npts)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_path p;
    int i;
    if (!g || !style) return GP_INVALIDARG;
    memset(&p, 0, sizeof p);
    p.fillmode = 1;
    if (kind == 0) {
        gp_moveto(&p, x, y); gp_lineto(&p, x + w, y);
        gp_lineto(&p, x + w, y + h); gp_lineto(&p, x, y + h);
        if (p.n) p.ty[p.n - 1] |= 0x80;
    } else if (kind == 1) {
        gp_arc(&p, x, y, w, h, 0, 360);
        if (p.n) p.ty[p.n - 1] |= 0x80;
    } else if (kind == 2) {
        for (i = 0; i < npts; i++) {
            if (i == 0) gp_moveto(&p, pts[0], pts[1]);
            else        gp_lineto(&p, pts[i * 2], pts[i * 2 + 1]);
        }
        if (p.n) p.ty[p.n - 1] |= 0x80;
    } else {
        gp_moveto(&p, x, y); gp_lineto(&p, w, h);           /* a line: x,y -> w,h */
    }
    if (is_pen) gp_stroke(g, &p, (gp_pen *)style);
    else        gp_fill_path_brush(g, &p, (gp_brush *)style);
    free(p.pt); free(p.ty);
    return GP_OK;
}
static MS int32_t st_GdipFillRectangle(void *g, void *b, float x, float y, float w, float h)
{ return gp_shape(g, gp_check(b, GPO_BRUSH), 0, 0, x, y, w, h, NULL, 0); }
static MS int32_t st_GdipDrawRectangle(void *g, void *p, float x, float y, float w, float h)
{ return gp_shape(g, gp_check(p, GPO_PEN), 1, 0, x, y, w, h, NULL, 0); }
static MS int32_t st_GdipFillEllipse(void *g, void *b, float x, float y, float w, float h)
{ return gp_shape(g, gp_check(b, GPO_BRUSH), 0, 1, x, y, w, h, NULL, 0); }
static MS int32_t st_GdipDrawEllipse(void *g, void *p, float x, float y, float w, float h)
{ return gp_shape(g, gp_check(p, GPO_PEN), 1, 1, x, y, w, h, NULL, 0); }
static MS int32_t st_GdipFillPolygon(void *g, void *b, const float *pts, int32_t n, int32_t mode)
{ (void)mode; return gp_shape(g, gp_check(b, GPO_BRUSH), 0, 2, 0, 0, 0, 0, pts, n); }
static MS int32_t st_GdipDrawPolygon(void *g, void *p, const float *pts, int32_t n)
{ return gp_shape(g, gp_check(p, GPO_PEN), 1, 2, 0, 0, 0, 0, pts, n); }
static MS int32_t st_GdipDrawLine(void *g, void *p, float x1, float y1, float x2, float y2)
{ return gp_shape(g, gp_check(p, GPO_PEN), 1, 3, x1, y1, x2, y2, NULL, 0); }

/* ---- pens and brushes --------------------------------------------------- */

static MS int32_t st_GdipCreatePen1(uint32_t argb, float width, int32_t unit, void **out)
{
    gp_pen *p;
    (void)unit;
    if (!out) return GP_INVALIDARG;
    if (!(p = gp_new(sizeof *p, GPO_PEN))) return GP_OUTOFMEMORY;
    p->argb = argb; p->width = width;
    *out = p;
    return GP_OK;
}
static MS int32_t st_GdipDeletePen(void *p)
{ if (!gp_check(p, GPO_PEN)) return GP_INVALIDARG; free(p); return GP_OK; }
static MS int32_t st_GdipSetPenColor(void *pen, uint32_t argb)
{ gp_pen *p = gp_check(pen, GPO_PEN); if (p) p->argb = argb; return p ? GP_OK : GP_INVALIDARG; }
static MS int32_t st_GdipSetPenWidth(void *pen, float w)
{ gp_pen *p = gp_check(pen, GPO_PEN); if (p) p->width = w; return p ? GP_OK : GP_INVALIDARG; }
static MS int32_t st_GdipSetPenLineJoin(void *pen, int32_t j)
{ gp_pen *p = gp_check(pen, GPO_PEN); if (p) p->join = j; return GP_OK; }
static MS int32_t st_GdipSetPenLineCap197819(void *pen, int32_t s, int32_t e, int32_t d)
{ gp_pen *p = gp_check(pen, GPO_PEN); (void)e; (void)d; if (p) p->cap = s; return GP_OK; }
/* Dashes are accepted and ignored: a dashed outline drawn solid is a cosmetic
 * difference, where refusing the call is a control that does not appear. */
static MS int32_t st_GdipSetPenDashArray(void *pen, const float *d, int32_t n)
{ (void)pen; (void)d; (void)n; return GP_OK; }
static MS int32_t st_GdipSetPenDashOffset(void *pen, float o) { (void)pen; (void)o; return GP_OK; }
static MS int32_t st_GdipSetPenDashStyle(void *pen, int32_t s) { (void)pen; (void)s; return GP_OK; }

static MS int32_t st_GdipCreateSolidFill(uint32_t argb, void **out)
{
    gp_brush *b;
    if (!out) return GP_INVALIDARG;
    if (!(b = gp_new(sizeof *b, GPO_BRUSH))) return GP_OUTOFMEMORY;
    b->kind = GPB_SOLID; b->argb = argb;
    *out = b;
    return GP_OK;
}
static MS int32_t st_GdipSetSolidFillColor(void *brush, uint32_t argb)
{ gp_brush *b = gp_check(brush, GPO_BRUSH); if (b) b->argb = argb; return b ? GP_OK : GP_INVALIDARG; }
static MS int32_t st_GdipDeleteBrush(void *b)
{ if (!gp_check(b, GPO_BRUSH)) return GP_INVALIDARG; free(b); return GP_OK; }
static MS int32_t st_GdipCloneBrush(void *brush, void **out)
{
    gp_brush *b = gp_check(brush, GPO_BRUSH), *q;
    if (!b || !out) return GP_INVALIDARG;
    if (!(q = gp_new(sizeof *q, GPO_BRUSH))) return GP_OUTOFMEMORY;
    *q = *b; q->tag = GPO_BRUSH;
    *out = q;
    return GP_OK;
}
static MS int32_t st_GdipCreateLineBrush(const float *p1, const float *p2,
                                         uint32_t c1, uint32_t c2, int32_t wrap, void **out)
{
    gp_brush *b;
    (void)wrap;
    if (!out || !p1 || !p2) return GP_INVALIDARG;
    if (!(b = gp_new(sizeof *b, GPO_BRUSH))) return GP_OUTOFMEMORY;
    b->kind = GPB_LINEAR;
    b->argb = c1; b->argb2 = c2;
    b->x0 = p1[0]; b->y0 = p1[1]; b->x1 = p2[0]; b->y1 = p2[1];
    *out = b;
    return GP_OK;
}
/* A preset blend replaces the two end colours with a ramp. Only the ends are
 * kept: the intermediate stops shade a gradient that is already an
 * approximation, and the ends are what set its overall colour. */
static MS int32_t st_GdipSetLinePresetBlend(void *brush, const uint32_t *colors,
                                            const float *pos, int32_t n)
{
    gp_brush *b = gp_check(brush, GPO_BRUSH);
    (void)pos;
    if (!b || !colors || n < 2) return GP_INVALIDARG;
    b->argb = colors[0]; b->argb2 = colors[n - 1];
    return GP_OK;
}
static MS int32_t st_GdipCreatePathGradientFromPath(void *path, void **out)
{
    gp_path *p = gp_check(path, GPO_PATH);
    gp_brush *b;
    if (!p || !out) return GP_INVALIDARG;
    if (!(b = gp_new(sizeof *b, GPO_BRUSH))) return GP_OUTOFMEMORY;
    b->kind = GPB_PATHGRAD;
    b->argb = 0xFF808080u; b->argb2 = 0xFF808080u;
    *out = b;
    return GP_OK;
}
static MS int32_t st_GdipSetPathGradientCenterColor(void *brush, uint32_t argb)
{ gp_brush *b = gp_check(brush, GPO_BRUSH); if (b) b->argb = argb; return GP_OK; }
static MS int32_t st_GdipSetPathGradientCenterPoint(void *brush, const float *pt)
{ gp_brush *b = gp_check(brush, GPO_BRUSH); if (b && pt) { b->x0 = pt[0]; b->y0 = pt[1]; } return GP_OK; }
static MS int32_t st_GdipSetPathGradientSurroundColorsWithCount(void *brush,
                                                                const uint32_t *colors,
                                                                int32_t *count)
{
    gp_brush *b = gp_check(brush, GPO_BRUSH);
    if (b && colors && count && *count > 0) b->argb2 = colors[0];
    return GP_OK;
}
static MS int32_t st_GdipGetPathGradientPointCount(void *brush, int32_t *n)
{ (void)brush; if (n) *n = 1; return GP_OK; }

/* ---- fonts and text ----------------------------------------------------- */

static MS int32_t st_GdipCreateFontFamilyFromName(const uint16_t *name, void *coll, void **out)
{
    gp_family *f;
    uint32_t i;
    (void)coll;
    if (!out) return GP_INVALIDARG;
    if (!(f = gp_new(sizeof *f, GPO_FAMILY))) return GP_OUTOFMEMORY;
    for (i = 0; name && name[i] && i + 1 < sizeof f->name; i++)
        f->name[i] = (char)(name[i] < 0x100 ? name[i] : '?');
    *out = f;
    return GP_OK;
}
static MS int32_t st_GdipGetGenericFontFamilySansSerif(void **out)
{ return st_GdipCreateFontFamilyFromName(NULL, NULL, out); }
static MS int32_t st_GdipDeleteFontFamily(void *f)
{ if (!gp_check(f, GPO_FAMILY)) return GP_INVALIDARG; free(f); return GP_OK; }

/* The installed font collection.
 *
 * A plug-in that offers a font menu builds it from here: make the collection,
 * ask how many families are in it, then ask for that many. There is one face
 * behind this whole layer -- the FreeType face the DirectWrite shim measures
 * with -- so the honest collection has one family in it, and a menu built from
 * it has one entry that works rather than a list of names that do not.
 *
 * The collection handle is a fixed non-null token rather than an object: it
 * carries no state, and a caller that never frees it (there is no API to free
 * this one) then leaks nothing. Chord Organ calls all three inside its editor
 * setup and reads the count back before allocating. */
static const int32_t gp_installed_collection = GPO_FAMILY;

static MS int32_t st_GdipNewInstalledFontCollection(void **out)
{
    if (!out) return GP_INVALIDARG;
    *out = (void *)&gp_installed_collection;
    return GP_OK;
}
static MS int32_t st_GdipNewPrivateFontCollection(void **out)
{ return st_GdipNewInstalledFontCollection(out); }
static MS int32_t st_GdipDeletePrivateFontCollection(void **out)
{ (void)out; return GP_OK; }
static MS int32_t st_GdipPrivateAddFontFile(void *coll, const uint16_t *file)
{ (void)coll; (void)file; return GP_OK; }
static MS int32_t st_GdipPrivateAddMemoryFont(void *coll, const void *mem, int32_t n)
{ (void)coll; (void)mem; (void)n; return GP_OK; }

static MS int32_t st_GdipGetFontCollectionFamilyCount(void *coll, int32_t *found)
{
    if (!coll || !found) return GP_INVALIDARG;
    *found = 1;
    return GP_OK;
}
static MS int32_t st_GdipGetFontCollectionFamilyList(void *coll, int32_t sought,
                                                     void **families, int32_t *found)
{
    static const uint16_t sans[] = { 'S','a','n','s',0 };
    if (!coll || !families || !found) return GP_INVALIDARG;
    *found = 0;
    if (sought < 1) return GP_OK;
    if (st_GdipCreateFontFamilyFromName(sans, NULL, &families[0]) != GP_OK)
        return GP_OUTOFMEMORY;
    *found = 1;
    return GP_OK;
}
static MS int32_t st_GdipCloneFontFamily(void *fam, void **out)
{
    gp_family *f = (gp_family *)gp_check(fam, GPO_FAMILY), *c;
    if (!f || !out) return GP_INVALIDARG;
    if (!(c = (gp_family *)gp_new(sizeof *c, GPO_FAMILY))) return GP_OUTOFMEMORY;
    memcpy(c->name, f->name, sizeof c->name);
    *out = c;
    return GP_OK;
}
static MS int32_t st_GdipGetFamilyName(void *fam, uint16_t *name, uint16_t lang)
{
    gp_family *f = (gp_family *)gp_check(fam, GPO_FAMILY);
    const char *src;
    int i;
    (void)lang;
    if (!f || !name) return GP_INVALIDARG;
    src = f->name[0] ? f->name : "Sans";
    /* LF_FACESIZE is 32 including the terminator, and that is the buffer the
     * caller passed: writing the whole 64-byte name into it overruns a stack
     * structure in every caller that uses the documented size. */
    for (i = 0; i < 31 && src[i]; i++) name[i] = (unsigned char)src[i];
    name[i] = 0;
    return GP_OK;
}
static MS int32_t st_GdipCreateFont(void *family, float size, int32_t style,
                                    int32_t unit, void **out)
{
    gp_font *f;
    (void)unit;
    if (!out) return GP_INVALIDARG;
    if (!(f = gp_new(sizeof *f, GPO_FONT))) return GP_OUTOFMEMORY;
    f->size = size > 0 ? size : 12.0f;
    f->style = style;
    f->fam = gp_check(family, GPO_FAMILY);
    *out = f;
    return GP_OK;
}
static MS int32_t st_GdipDeleteFont(void *f)
{ if (!gp_check(f, GPO_FONT)) return GP_INVALIDARG; free(f); return GP_OK; }
static MS int32_t st_GdipGetFontSize(void *font, float *out)
{ gp_font *f = gp_check(font, GPO_FONT); if (out) *out = f ? f->size : 12.0f; return GP_OK; }
static MS int32_t st_GdipGetFontStyle(void *font, int32_t *out)
{ gp_font *f = gp_check(font, GPO_FONT); if (out) *out = f ? f->style : 0; return GP_OK; }
static MS int32_t st_GdipGetFamily(void *font, void **out)
{
    gp_font *f = gp_check(font, GPO_FONT);
    if (!f || !out) return GP_INVALIDARG;
    return st_GdipCreateFontFamilyFromName(NULL, NULL, out);
}
static MS int32_t st_GdipGetFontHeightGivenDPI(void *font, float dpi, float *out)
{
    gp_font *f = gp_check(font, GPO_FONT);
    (void)dpi;
    if (out) *out = (f ? f->size : 12.0f) * 1.2f;
    return GP_OK;
}
/* The design-unit metrics a caller scales by em size itself. The numbers are
 * the face's own where FreeType has one open. */
static MS int32_t st_GdipGetEmHeight(void *fam, int32_t style, uint16_t *out)
{
    FT_Face f = dw_ftface();
    (void)fam; (void)style;
    if (out) *out = (uint16_t)(f && f->units_per_EM ? f->units_per_EM : 2048);
    return GP_OK;
}
static MS int32_t st_GdipGetCellAscent(void *fam, int32_t style, uint16_t *out)
{
    FT_Face f = dw_ftface();
    (void)fam; (void)style;
    if (out) *out = (uint16_t)(f ? (f->ascender > 0 ? f->ascender : 1638) : 1638);
    return GP_OK;
}
static MS int32_t st_GdipGetCellDescent(void *fam, int32_t style, uint16_t *out)
{
    FT_Face f = dw_ftface();
    (void)fam; (void)style;
    if (out) *out = (uint16_t)(f ? (uint16_t)(-f->descender) : 410);
    return GP_OK;
}
static MS int32_t st_GdipGetLineSpacing(void *fam, int32_t style, uint16_t *out)
{
    FT_Face f = dw_ftface();
    (void)fam; (void)style;
    if (out) *out = (uint16_t)(f && f->height ? f->height : 2458);
    return GP_OK;
}

/* Measure and draw, both through the same FreeType face the DirectWrite shim
 * uses. One line: GDI+ callers in a plug-in editor are labelling controls. */
static void gp_text_extent(const uint16_t *str, int32_t len, float size,
                           float *w, float *h)
{
    FT_Face face = dw_ftface();
    float wid = 0;
    int32_t i;
    if (face) {
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(size > 0 ? size : 12.0f));
        for (i = 0; i < len && str[i]; i++)
            if (FT_Load_Char(face, (FT_ULong)str[i], FT_LOAD_DEFAULT) == 0)
                wid += (float)(face->glyph->advance.x >> 6);
        *h = (float)((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
    } else {
        for (i = 0; i < len && str[i]; i++) wid += size * 0.55f;
        *h = size * 1.2f;
    }
    *w = wid;
}
static MS int32_t st_GdipMeasureString(void *graphics, const uint16_t *str, int32_t len,
                                       void *font, const float *layout, void *fmt,
                                       float *bounds, int32_t *codepoints, int32_t *lines)
{
    gp_font *f = gp_check(font, GPO_FONT);
    float w = 0, h = 0;
    (void)graphics; (void)fmt;
    if (!bounds) return GP_INVALIDARG;
    if (len < 0) { len = 0; while (str && str[len]) len++; }
    gp_text_extent(str, len, f ? f->size : 12.0f, &w, &h);
    bounds[0] = layout ? layout[0] : 0;
    bounds[1] = layout ? layout[1] : 0;
    bounds[2] = w;
    bounds[3] = h;
    if (codepoints) *codepoints = len;
    if (lines) *lines = 1;
    return GP_OK;
}
static MS int32_t st_GdipDrawString(void *graphics, const uint16_t *str, int32_t len,
                                    void *font, const float *layout, void *fmt, void *brush)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_font *f = gp_check(font, GPO_FONT);
    gp_brush *b = gp_check(brush, GPO_BRUSH);
    FT_Face face = dw_ftface();
    int W, H;
    uint32_t *px = gp_pixels(g, &W, &H);
    float penx, peny;
    int32_t i;

    if (!g || !px || !layout) return GP_INVALIDARG;
    if (len < 0) { len = 0; while (str && str[len]) len++; }
    if (!face) return GP_OK;                      /* nothing to draw with */

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)((f ? f->size : 12.0f)));
    gp_apply(&g->xf, layout[0], layout[1], &penx, &peny);
    peny += (float)(face->size->metrics.ascender >> 6);

    for (i = 0; i < len && str[i]; i++) {
        FT_GlyphSlot sl;
        int gx, gy, row, col;
        if (FT_Load_Char(face, (FT_ULong)str[i], FT_LOAD_RENDER)) continue;
        sl = face->glyph;
        gx = (int)penx + sl->bitmap_left;
        gy = (int)peny - sl->bitmap_top;
        for (row = 0; row < (int)sl->bitmap.rows; row++) {
            int y = gy + row;
            if (y < 0 || y >= H) continue;
            for (col = 0; col < (int)sl->bitmap.width; col++) {
                int x = gx + col;
                uint8_t a = sl->bitmap.buffer[row * sl->bitmap.pitch + col];
                if (!a || x < 0 || x >= W) continue;
                if (g->has_clip &&
                    ((float)x < g->cx0 || (float)x >= g->cx1 ||
                     (float)y < g->cy0 || (float)y >= g->cy1)) continue;
                gp_blend(&px[(size_t)y * W + x], b ? b->argb : 0xFF000000u, a);
            }
        }
        penx += (float)(sl->advance.x >> 6);
    }
    return GP_OK;
}

/* ---- images ------------------------------------------------------------- */

static MS int32_t st_GdipCreateBitmapFromScan0(int32_t w, int32_t h, int32_t stride,
                                               int32_t fmt, uint8_t *scan0, void **out)
{
    gp_image *im;
    (void)fmt;
    if (!out || w <= 0 || h <= 0) return GP_INVALIDARG;
    if (!(im = gp_new(sizeof *im, GPO_IMAGE))) return GP_OUTOFMEMORY;
    im->w = w; im->h = h; im->owns = 1;
    if (!(im->px = calloc((size_t)w * h, 4))) { free(im); return GP_OUTOFMEMORY; }
    if (scan0) {
        int row;
        int sp = stride ? stride : w * 4;
        /* A negative stride means the caller's rows run bottom-up. */
        for (row = 0; row < h; row++)
            memcpy(im->px + (size_t)row * w,
                   scan0 + (ptrdiff_t)row * sp, (size_t)w * 4);
    }
    *out = im;
    return GP_OK;
}
static MS int32_t st_GdipCreateBitmapFromResource(void *inst, const uint16_t *name, void **out)
{
    /* Resource-backed artwork. GDI+ images are stored under RT_RCDATA rather
     * than RT_BITMAP -- they are whole PNG or BMP files, not DIBs -- so the
     * bytes come back as they were compiled in and image_decode settles which
     * of the two it is. */
    void *rsrc, *data = NULL;
    uint32_t sz = 0;
    gp_image *im;
    int w = 0, h = 0;
    uint32_t *px;
    (void)inst;
    if (!out) return GP_INVALIDARG;
    *out = NULL;
    if ((rsrc = res_lookup((const void *)10 /* RT_RCDATA */, name, g_rsrc)) != NULL) {
        sz = ((RES_DATA *)rsrc)->Size;
        data = image_base_for_rsrc(rsrc) + ((RES_DATA *)rsrc)->OffsetToData;
    }
    if (!data || !sz) return GP_GENERIC;
    if (!(px = image_decode(data, sz, &w, &h)) || w <= 0 || h <= 0) { free(px); return GP_GENERIC; }
    if (!(im = gp_new(sizeof *im, GPO_IMAGE))) { free(px); return GP_OUTOFMEMORY; }
    im->w = w; im->h = h; im->px = px; im->owns = 1;
    *out = im;
    return GP_OK;
}
/* The other way artwork arrives: the plug-in wraps its own bytes in an IStream
 * and hands that over. The stream is the plug-in's object, not ours, so the
 * only way to read it is through its vtable. COM's slot numbers apply --
 * 3 Read, 5 Seek -- and its methods are __stdcall with `this` first, which is
 * what MS means on both architectures. Seek's LARGE_INTEGER is 64-bit at i386
 * as well, so it is not the pointer-in-a-uint64 mistake: WIDTH-OK: fn_stm_seek */
typedef MS int32_t (*fn_stm_read)(void *, void *, uint32_t, uint32_t *);
typedef MS int32_t (*fn_stm_seek)(void *, int64_t, uint32_t, uint64_t *);

#define GP_STREAM_CHUNK  (64u * 1024u)
#define GP_STREAM_MAX    (64u * 1024u * 1024u)

/* Read a stream out in full. Size it with Seek before reading rather than
 * reading until it stops giving bytes: a plug-in's own IStream is often
 * minimal, and answers a read that asks for more than remains with S_FALSE
 * and nothing at all instead of the short count the interface calls for.
 * OrilRiver's does exactly that, which cost every one of its images its last
 * partial chunk and left the editor blank. */
static uint8_t *gp_stream_slurp(void *stream, size_t *outn)
{
    void **vtbl;
    uint8_t *buf;
    size_t n = 0, cap, size = 0;
    uint64_t end = 0;
    uint32_t want = GP_STREAM_CHUNK;

    *outn = 0;
    if (!stream) return NULL;
    vtbl = *(void ***)stream;
    if (!vtbl || !vtbl[3] || !vtbl[5]) return NULL;

    /* Seek is asked for the size, but not relied on: a minimal stream may
     * refuse STREAM_SEEK_END or leave the position unwritten. */
    if (((fn_stm_seek)vtbl[5])(stream, 0, 2 /* END */, &end) >= 0
        && end && end <= GP_STREAM_MAX)
        size = (size_t)end;
    ((fn_stm_seek)vtbl[5])(stream, 0, 0 /* SET */, NULL);

    cap = size ? size : (size_t)GP_STREAM_CHUNK * 2;
    if (!(buf = malloc(cap))) return NULL;

    for (;;) {
        uint32_t got = 0;
        int32_t hr;
        if (size) {
            if (n >= size) break;
            if (size - n < want) want = (uint32_t)(size - n);
        } else if (n + want > cap) {
            uint8_t *q;
            size_t bigger = cap * 2;
            if (bigger > GP_STREAM_MAX) break;
            if (!(q = realloc(buf, bigger))) break;
            buf = q; cap = bigger;
        }
        hr = ((fn_stm_read)vtbl[3])(stream, buf + n, want, &got);
        if (got > want) break;                  /* a broken stream */
        n += got;
        if (hr < 0) break;
        if (got == 0) {
            /* Nothing came back, but the stream may simply be refusing a read
             * longer than what is left of it rather than shortening it as the
             * interface requires. Halving the request finds the tail; only a
             * refusal of a single byte means the end. */
            if (want <= 1) break;
            want /= 2;
            continue;
        }
    }
    if (!n) { free(buf); return NULL; }
    *outn = n;
    return buf;
}

static MS int32_t st_GdipCreateBitmapFromStream(void *stream, void **out)
{
    uint8_t *data;
    size_t n = 0;
    gp_image *im;
    int w = 0, h = 0;
    uint32_t *px;

    if (!out) return GP_INVALIDARG;
    *out = NULL;
    if (!(data = gp_stream_slurp(stream, &n))) return GP_GENERIC;
    px = image_decode(data, n, &w, &h);
    free(data);
    if (!px || w <= 0 || h <= 0) { free(px); return GP_GENERIC; }
    if (!(im = gp_new(sizeof *im, GPO_IMAGE))) { free(px); return GP_OUTOFMEMORY; }
    im->w = w; im->h = h; im->px = px; im->owns = 1;
    *out = im;
    return GP_OK;
}
/* ICM means "apply the colour profile"; there is none to apply here, and the
 * loading is identical. GdipLoadImageFromStream is the same call under the
 * name a caller uses when it does not care that the result is a bitmap. */
static MS int32_t st_GdipCreateBitmapFromStreamICM(void *stream, void **out)
{ return st_GdipCreateBitmapFromStream(stream, out); }
static MS int32_t st_GdipLoadImageFromStream(void *stream, void **out)
{ return st_GdipCreateBitmapFromStream(stream, out); }
static MS int32_t st_GdipLoadImageFromStreamICM(void *stream, void **out)
{ return st_GdipCreateBitmapFromStream(stream, out); }
static MS int32_t st_GdipCreateHBITMAPFromBitmap(void *image, void **hbm, uint32_t back)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    (void)back;
    if (!im || !hbm) return GP_INVALIDARG;
    {   /* An ordinary Win32 bitmap object holding the same pixels: the caller
         * goes on to select it into a DC and blit it. */
        int idx = w32_obj_new(OBJ_BITMAP, im->w, im->h, 0);
        if (!idx) return GP_OUTOFMEMORY;
        memcpy(W.obj[idx].px, im->px, (size_t)im->w * im->h * 4);
        *hbm = w32_h(W32_OBJ_BASE, idx);
    }
    return GP_OK;
}
static MS int32_t st_GdipGetImageWidth(void *image, uint32_t *w)
{ gp_image *im = gp_check(image, GPO_IMAGE); if (w) *w = im ? (uint32_t)im->w : 0; return im ? GP_OK : GP_INVALIDARG; }
static MS int32_t st_GdipGetImageHeight(void *image, uint32_t *h)
{ gp_image *im = gp_check(image, GPO_IMAGE); if (h) *h = im ? (uint32_t)im->h : 0; return im ? GP_OK : GP_INVALIDARG; }
static MS int32_t st_GdipDisposeImage(void *image)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im) return GP_INVALIDARG;
    if (im->owns) free(im->px);
    free(im);
    return GP_OK;
}
static MS int32_t st_GdipCloneImage(void *image, void **out)
{
    gp_image *im = gp_check(image, GPO_IMAGE), *q;
    if (!im || !out) return GP_INVALIDARG;
    if (!(q = gp_new(sizeof *q, GPO_IMAGE))) return GP_OUTOFMEMORY;
    q->w = im->w; q->h = im->h; q->owns = 1;
    if (!(q->px = malloc((size_t)im->w * im->h * 4))) { free(q); return GP_OUTOFMEMORY; }
    memcpy(q->px, im->px, (size_t)im->w * im->h * 4);
    *out = q;
    return GP_OK;
}
/* BitmapData: width, height, stride, format, scan0, reserved. The buffer is
 * handed over directly rather than copied -- a caller locks a bitmap precisely
 * to write into the real pixels. */
static MS int32_t st_GdipBitmapLockBits(void *image, const int32_t *rc, uint32_t flags,
                                        int32_t fmt, void *data)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    uint8_t *d = data;
    (void)flags; (void)fmt;
    if (!im || !d) return GP_INVALIDARG;
    *(uint32_t *)(d + 0) = (uint32_t)(rc ? rc[2] : im->w);
    *(uint32_t *)(d + 4) = (uint32_t)(rc ? rc[3] : im->h);
    *(int32_t  *)(d + 8) = im->w * 4;
    *(int32_t  *)(d + 12) = 0x26200A;                    /* Format32bppARGB */
    *(void **)(d + 16) = im->px + (rc ? ((size_t)rc[1] * im->w + rc[0]) : 0);
    return GP_OK;
}
static MS int32_t st_GdipBitmapUnlockBits(void *image, void *data)
{ (void)data; return gp_check(image, GPO_IMAGE) ? GP_OK : GP_INVALIDARG; }

static MS int32_t st_GdipCreateImageAttributes(void **out)
{
    gp_imageattr *a;
    if (!out) return GP_INVALIDARG;
    if (!(a = gp_new(sizeof *a, GPO_IMAGEATTR))) return GP_OUTOFMEMORY;
    *out = a;
    return GP_OK;
}
static MS int32_t st_GdipDisposeImageAttributes(void *a)
{ if (!gp_check(a, GPO_IMAGEATTR)) return GP_INVALIDARG; free(a); return GP_OK; }
static MS int32_t st_GdipSetImageAttributesColorMatrix(void *attr, int32_t type, int32_t enable,
                                                       const float *m, const float *gray,
                                                       int32_t flags)
{
    gp_imageattr *a = gp_check(attr, GPO_IMAGEATTR);
    (void)type; (void)gray; (void)flags;
    if (!a) return GP_INVALIDARG;
    a->has_matrix = 0;
    if (enable && m) { memcpy(a->m, m, sizeof a->m); a->has_matrix = 1; }
    return GP_OK;
}

/* The one image call a skinned plug-in leans on: source rectangle to
 * destination rectangle, scaled, with an optional colour matrix. Nearest
 * neighbour -- a skin is drawn at or near its own size, and the alternative
 * costs more than it shows. */
/* The last of what a VCL-era plug-in resolves out of gdiplus.
 *
 * These were the remainder of Chord Organ's list once gdiplus became reachable
 * at all -- see the note on g_stockdlls in winstubs.h. Each is either the
 * integer or simplified form of something already here, or an honest answer
 * about a feature this shim does not model. */

/* Every image here is decoded to 32-bit ARGB, so the format is not a guess. */
#define GP_PF_32BPP_ARGB 0x0026200Au
static MS int32_t st_GdipGetImagePixelFormat(void *image, int32_t *fmt)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im || !fmt) return GP_INVALIDARG;
    *fmt = (int32_t)GP_PF_32BPP_ARGB;
    return GP_OK;
}
/* A direct-colour image has no palette, and the size of one is the header
 * alone -- which is what a caller allocates before asking for the entries. */
static MS int32_t st_GdipGetImagePaletteSize(void *image, int32_t *size)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    W32_APPROX();
    if (!im || !size) return GP_INVALIDARG;
    *size = 8;                                   /* flags + count, no entries */
    return GP_OK;
}
static MS int32_t st_GdipGetImagePalette(void *image, void *pal, int32_t size)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im || !pal || size < 8) return GP_INVALIDARG;
    memset(pal, 0, 8);                           /* no flags, zero entries */
    return GP_OK;
}

/* Straight from a file, which is how a skin loads when it is not going through
 * an IStream. The decoder is the same one the resource and stream paths use. */
static MS int32_t st_GdipCreateBitmapFromFile(const uint16_t *name, void **out)
{
    char path[1024];
    uint8_t *data = NULL;
    size_t n = 0;
    gp_image *im;
    uint32_t *px;
    int w = 0, h = 0;
    FILE *f;

    if (!out) return GP_INVALIDARG;
    *out = NULL;
    if (!name) return GP_INVALIDARG;
    w2c_path(name, path, sizeof path);
    if (!(f = fopen(path, "rb"))) return GP_GENERIC;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len > 0 && len <= (long)GP_STREAM_MAX && fseek(f, 0, SEEK_SET) == 0
            && (data = malloc((size_t)len)) != NULL)
            n = fread(data, 1, (size_t)len, f);
    }
    fclose(f);
    if (!data || !n) { free(data); return GP_GENERIC; }
    px = image_decode(data, n, &w, &h);
    free(data);
    if (!px || w <= 0 || h <= 0) { free(px); return GP_GENERIC; }
    if (!(im = gp_new(sizeof *im, GPO_IMAGE))) { free(px); return GP_OUTOFMEMORY; }
    im->w = w; im->h = h; im->px = px; im->owns = 1;
    *out = im;
    return GP_OK;
}
static MS int32_t st_GdipCreateBitmapFromFileICM(const uint16_t *name, void **out)
{ return st_GdipCreateBitmapFromFile(name, out); }

/* Draw at its natural size: the rect-to-rect form with both rectangles the
 * image's own. */
static MS int32_t st_GdipDrawImageRectRectI(void *graphics, void *image,
                                            int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                                            int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                                            int32_t unit, void *attr, void *cb, void *cbdata);
static MS int32_t st_GdipDrawImageI(void *graphics, void *image, int32_t x, int32_t y)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im) return GP_INVALIDARG;
    return st_GdipDrawImageRectRectI(graphics, image, x, y, im->w, im->h,
                                     0, 0, im->w, im->h, 2 /* UnitPixel */,
                                     NULL, NULL, NULL);
}

/* ---- regions ------------------------------------------------------------
 *
 * See gp_region. Everything here is rectangles; the combine modes are applied
 * to the bounding box, which is where this stops being GDI+ and starts being
 * enough of it. */
#define GP_REGION_HUGE 1.0e7f

static gp_region *gp_region_new(void)
{
    gp_region *r = (gp_region *)gp_new(sizeof *r, GPO_REGION);
    if (r) r->infinite = 1;
    return r;
}
static void gp_region_rect(gp_region *r, float x, float y, float w, float h)
{
    r->infinite = 0;
    r->x0 = w >= 0 ? x : x + w; r->x1 = w >= 0 ? x + w : x;
    r->y0 = h >= 0 ? y : y + h; r->y1 = h >= 0 ? y + h : y;
}
/* The bounds of a region as a rectangle, unbounded included: an infinite region
 * has no numbers of its own, and a caller asking for its bounds wants something
 * it can clip with rather than nothing. */
static void gp_region_box(const gp_region *r, float *x0, float *y0, float *x1, float *y1)
{
    if (r->infinite) {
        *x0 = *y0 = -GP_REGION_HUGE; *x1 = *y1 = GP_REGION_HUGE;
    } else {
        *x0 = r->x0; *y0 = r->y0; *x1 = r->x1; *y1 = r->y1;
    }
}

static MS int32_t st_GdipCreateRegion(void **out)
{
    if (!out) return GP_INVALIDARG;
    return (*out = gp_region_new()) ? GP_OK : GP_OUTOFMEMORY;
}
static MS int32_t st_GdipCreateRegionRect(const float *rect, void **out)
{
    gp_region *r;
    if (!rect || !out) return GP_INVALIDARG;
    if (!(r = gp_region_new())) return GP_OUTOFMEMORY;
    gp_region_rect(r, rect[0], rect[1], rect[2], rect[3]);
    *out = r;
    return GP_OK;
}
static MS int32_t st_GdipCreateRegionRectI(const int32_t *rect, void **out)
{
    float f[4];
    if (!rect) return GP_INVALIDARG;
    f[0] = (float)rect[0]; f[1] = (float)rect[1];
    f[2] = (float)rect[2]; f[3] = (float)rect[3];
    return st_GdipCreateRegionRect(f, out);
}
static MS int32_t st_GdipCreateRegionPath(void *path, void **out)
{
    float box[4] = { 0, 0, 0, 0 };
    gp_region *r;
    if (!out) return GP_INVALIDARG;
    if (!(r = gp_region_new())) return GP_OUTOFMEMORY;
    if (st_GdipGetPathWorldBounds(path, box, NULL, NULL) == GP_OK)
        gp_region_rect(r, box[0], box[1], box[2], box[3]);
    *out = r;
    return GP_OK;
}
static MS int32_t st_GdipCloneRegion(void *region, void **out)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION), *c;
    if (!r || !out) return GP_INVALIDARG;
    if (!(c = (gp_region *)gp_new(sizeof *c, GPO_REGION))) return GP_OUTOFMEMORY;
    *c = *r;
    c->tag = GPO_REGION;
    *out = c;
    return GP_OK;
}
static MS int32_t st_GdipDeleteRegion(void *region)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    /* A caller that never made one through this layer still frees it, and
     * failing that free is not worth the difference: succeed either way. */
    if (r) free(r);
    return GP_OK;
}
static MS int32_t st_GdipSetInfinite(void *region)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    if (!r) return GP_INVALIDARG;
    r->infinite = 1;
    return GP_OK;
}
static MS int32_t st_GdipSetEmpty(void *region)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    if (!r) return GP_INVALIDARG;
    r->infinite = 0; r->x0 = r->y0 = r->x1 = r->y1 = 0.0f;
    return GP_OK;
}
static MS int32_t st_GdipIsEmptyRegion(void *region, void *graphics, int32_t *out)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    (void)graphics;
    if (!r || !out) return GP_INVALIDARG;
    *out = !r->infinite && (r->x1 <= r->x0 || r->y1 <= r->y0);
    return GP_OK;
}
static MS int32_t st_GdipIsInfiniteRegion(void *region, void *graphics, int32_t *out)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    (void)graphics;
    if (!r || !out) return GP_INVALIDARG;
    *out = r->infinite;
    return GP_OK;
}
/* Combine modes: 0 Replace, 1 Intersect, 2 Union, 3 Xor, 4 Exclude,
 * 5 Complement. Intersect is exact for rectangles; the rest widen. */
static void gp_region_combine(gp_region *r, float x0, float y0, float x1, float y1,
                              int32_t mode)
{
    float a0, b0, a1, b1;
    switch (mode) {
    case 0:                                       /* Replace */
        r->infinite = 0; r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;
        return;
    case 1:                                       /* Intersect */
        if (r->infinite) { r->infinite = 0; r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; return; }
        r->x0 = r->x0 > x0 ? r->x0 : x0;
        r->y0 = r->y0 > y0 ? r->y0 : y0;
        r->x1 = r->x1 < x1 ? r->x1 : x1;
        r->y1 = r->y1 < y1 ? r->y1 : y1;
        if (r->x1 < r->x0) r->x1 = r->x0;
        if (r->y1 < r->y0) r->y1 = r->y0;
        return;
    case 4: case 5:                               /* Exclude, Complement */
        /* Neither is a rectangle. Leaving the region as it stands clips no
         * more than before, which draws too much rather than nothing. */
        W32_APPROX();
        return;
    default:                                      /* Union, Xor */
        if (r->infinite) return;
        gp_region_box(r, &a0, &b0, &a1, &b1);
        r->x0 = a0 < x0 ? a0 : x0;
        r->y0 = b0 < y0 ? b0 : y0;
        r->x1 = a1 > x1 ? a1 : x1;
        r->y1 = b1 > y1 ? b1 : y1;
        if (mode == 3) W32_APPROX();              /* Xor is not a rectangle */
        return;
    }
}
static MS int32_t st_GdipCombineRegionRect(void *region, const float *rect, int32_t mode)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    if (!r || !rect) return GP_INVALIDARG;
    gp_region_combine(r, rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3], mode);
    return GP_OK;
}
static MS int32_t st_GdipCombineRegionRectI(void *region, const int32_t *rect, int32_t mode)
{
    float f[4];
    if (!rect) return GP_INVALIDARG;
    f[0] = (float)rect[0]; f[1] = (float)rect[1];
    f[2] = (float)rect[2]; f[3] = (float)rect[3];
    return st_GdipCombineRegionRect(region, f, mode);
}
static MS int32_t st_GdipCombineRegionRegion(void *region, void *other, int32_t mode)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    gp_region *o = (gp_region *)gp_check(other, GPO_REGION);
    float x0, y0, x1, y1;
    if (!r || !o) return GP_INVALIDARG;
    if (o->infinite && mode == 1) return GP_OK;            /* intersect with all */
    gp_region_box(o, &x0, &y0, &x1, &y1);
    gp_region_combine(r, x0, y0, x1, y1, mode);
    return GP_OK;
}
static MS int32_t st_GdipCombineRegionPath(void *region, void *path, int32_t mode)
{
    float box[4] = { 0, 0, 0, 0 };
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    if (!r) return GP_INVALIDARG;
    if (st_GdipGetPathWorldBounds(path, box, NULL, NULL) != GP_OK) return GP_OK;
    gp_region_combine(r, box[0], box[1], box[0] + box[2], box[1] + box[3], mode);
    return GP_OK;
}
static MS int32_t st_GdipTranslateRegion(void *region, float dx, float dy)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    if (!r) return GP_INVALIDARG;
    if (!r->infinite) { r->x0 += dx; r->x1 += dx; r->y0 += dy; r->y1 += dy; }
    return GP_OK;
}
static MS int32_t st_GdipTranslateRegionI(void *region, int32_t dx, int32_t dy)
{ return st_GdipTranslateRegion(region, (float)dx, (float)dy); }
static MS int32_t st_GdipTransformRegion(void *region, void *matrix)
{
    /* Only the translation part, which is what a scrolled or offset control
     * uses it for; a rotated region is not a rectangle. */
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    gp_matrix *m = (gp_matrix *)gp_check(matrix, GPO_MATRIX);
    if (!r || !m) return GP_INVALIDARG;
    W32_APPROX();
    return st_GdipTranslateRegion(region, m->m[4], m->m[5]);
}
static MS int32_t st_GdipIsVisibleRegionPoint(void *region, float x, float y,
                                              void *graphics, int32_t *out)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    (void)graphics;
    if (!r || !out) return GP_INVALIDARG;
    *out = r->infinite || (x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1);
    return GP_OK;
}
static MS int32_t st_GdipIsVisibleRegionPointI(void *region, int32_t x, int32_t y,
                                               void *graphics, int32_t *out)
{ return st_GdipIsVisibleRegionPoint(region, (float)x, (float)y, graphics, out); }

static MS int32_t st_GdipGetRegionBounds(void *region, void *graphics, void *rect)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    gp_graphics *g = (gp_graphics *)gp_check(graphics, GPO_GRAPHICS);
    float *out = (float *)rect;
    float x0, y0, x1, y1;

    if (!out) return GP_INVALIDARG;
    if (r) {
        gp_region_box(r, &x0, &y0, &x1, &y1);
    } else if (g && g->has_clip) {           /* no region: report the clip */
        x0 = g->cx0; y0 = g->cy0; x1 = g->cx1; y1 = g->cy1;
    } else {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return GP_OK;
    }
    out[0] = x0; out[1] = y0; out[2] = x1 - x0; out[3] = y1 - y0;
    return GP_OK;
}
static MS int32_t st_GdipGetRegionBoundsI(void *region, void *graphics, int32_t *rect)
{
    float f[4];
    int32_t st = st_GdipGetRegionBounds(region, graphics, f);
    if (st == GP_OK && rect) {
        rect[0] = (int32_t)f[0]; rect[1] = (int32_t)f[1];
        rect[2] = (int32_t)f[2]; rect[3] = (int32_t)f[3];
    }
    return st;
}

/* The clip, which is where a region and a graphics meet. */
static MS int32_t st_GdipSetClipRegion(void *graphics, void *region, int32_t combine)
{
    gp_graphics *g = (gp_graphics *)gp_check(graphics, GPO_GRAPHICS);
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    if (!g || !r) return GP_INVALIDARG;
    if (r->infinite) { g->has_clip = 0; return GP_OK; }
    /* The region is already in world coordinates the caller set up, so this
     * goes through the same transform GdipSetClipRect applies. */
    return st_GdipSetClipRect(graphics, r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0,
                              combine);
}
static MS int32_t st_GdipGetClip(void *graphics, void *region)
{
    gp_graphics *g = (gp_graphics *)gp_check(graphics, GPO_GRAPHICS);
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    if (!g || !r) return GP_INVALIDARG;
    if (!g->has_clip) { r->infinite = 1; return GP_OK; }
    gp_region_rect(r, g->cx0, g->cy0, g->cx1 - g->cx0, g->cy1 - g->cy0);
    return GP_OK;
}
static MS int32_t st_GdipResetClip(void *graphics)
{
    gp_graphics *g = (gp_graphics *)gp_check(graphics, GPO_GRAPHICS);
    if (!g) return GP_INVALIDARG;
    g->has_clip = 0;
    return GP_OK;
}
static MS int32_t st_GdipFillRegion(void *graphics, void *brush, void *region)
{
    gp_region *r = (gp_region *)gp_check(region, GPO_REGION);
    float x0, y0, x1, y1;
    if (!r) return GP_INVALIDARG;
    gp_region_box(r, &x0, &y0, &x1, &y1);
    /* An infinite region fills the surface, not a ten-million-pixel rectangle
     * the rasteriser would spend real time on. */
    if (r->infinite) {
        gp_graphics *g = (gp_graphics *)gp_check(graphics, GPO_GRAPHICS);
        int w = 0, h = 0;
        if (!g || !gp_pixels(g, &w, &h)) return GP_INVALIDARG;
        x0 = y0 = 0.0f; x1 = (float)w; y1 = (float)h;
    }
    return st_GdipFillRectangle(graphics, brush, x0, y0, x1 - x0, y1 - y0);
}

/* A pen made from a brush takes the brush's colour, which for a solid brush is
 * exactly right and for a gradient is its starting colour. */
static MS int32_t st_GdipCreatePen1(uint32_t argb, float width, int32_t unit, void **out);
static MS int32_t st_GdipCreatePen2(void *brush, float width, int32_t unit, void **out)
{
    gp_brush *b = gp_check(brush, GPO_BRUSH);
    return st_GdipCreatePen1(b ? b->argb : 0xFF000000u, width, unit, out);
}
/* Getters. Each of these returns something the caller already gave this layer,
 * and each was a stand-in returning "not implemented" -- which a plug-in that
 * reads a setting, changes it, draws and puts it back treats as a failed draw. */
static MS int32_t st_GdipGetSmoothingMode(void *graphics, int32_t *out)
{
    gp_graphics *g = (gp_graphics *)gp_check(graphics, GPO_GRAPHICS);
    if (!g || !out) return GP_INVALIDARG;
    *out = g->smoothing;
    return GP_OK;
}
static MS int32_t st_GdipGetTextRenderingHint(void *graphics, int32_t *out)
{
    if (!gp_check(graphics, GPO_GRAPHICS) || !out) return GP_INVALIDARG;
    *out = 0;                                   /* SystemDefault */
    return GP_OK;
}
static MS int32_t st_GdipGetInterpolationMode(void *graphics, int32_t *out)
{
    if (!gp_check(graphics, GPO_GRAPHICS) || !out) return GP_INVALIDARG;
    *out = 0;                                   /* Default */
    return GP_OK;
}
static MS int32_t st_GdipGetPixelOffsetMode(void *graphics, int32_t *out)
{
    if (!gp_check(graphics, GPO_GRAPHICS) || !out) return GP_INVALIDARG;
    *out = 0;
    return GP_OK;
}
static MS int32_t st_GdipGetPenWidth(void *pen, float *out)
{
    gp_pen *p = (gp_pen *)gp_check(pen, GPO_PEN);
    if (!p || !out) return GP_INVALIDARG;
    *out = p->width;
    return GP_OK;
}
static MS int32_t st_GdipGetPenColor(void *pen, uint32_t *out)
{
    gp_pen *p = (gp_pen *)gp_check(pen, GPO_PEN);
    if (!p || !out) return GP_INVALIDARG;
    *out = p->argb;
    return GP_OK;
}
static MS int32_t st_GdipGetPenFillType(void *pen, int32_t *out)
{
    if (!gp_check(pen, GPO_PEN) || !out) return GP_INVALIDARG;
    *out = 0;                                   /* PenTypeSolidColor */
    return GP_OK;
}
/* Pen alignment: centre or inset. This rasteriser strokes centred, which is
 * the default, so accepting the call and drawing the same line is the whole of
 * the difference -- half a pen width on an inset border. */
static MS int32_t st_GdipSetPenMode(void *pen, int32_t mode)
{
    if (!gp_check(pen, GPO_PEN)) return GP_INVALIDARG;
    if (mode) W32_APPROX();
    return GP_OK;
}
static MS int32_t st_GdipGetPenMode(void *pen, int32_t *out)
{
    if (!gp_check(pen, GPO_PEN) || !out) return GP_INVALIDARG;
    *out = 0;                                   /* PenAlignmentCenter */
    return GP_OK;
}

static MS int32_t st_GdipSetPenEndCap(void *pen, int32_t cap)
{ gp_pen *p = gp_check(pen, GPO_PEN); if (p) p->cap = cap; return p ? GP_OK : GP_INVALIDARG; }
static MS int32_t st_GdipSetPenStartCap(void *pen, int32_t cap)
{ return st_GdipSetPenEndCap(pen, cap); }

/* A texture brush draws as the average of its image, the way a GDI pattern
 * brush does here: a tiled fill needs an origin this layer does not track, and
 * the tone is what a background is judged on. */
static MS int32_t st_GdipCreateSolidFill(uint32_t argb, void **out);
static MS int32_t st_GdipCreateTexture(void *image, int32_t wrap, void **out)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    W32_APPROX();                                /* the average, not the tile */
    uint64_t a = 0, r = 0, g = 0, b = 0;
    size_t n, i;
    (void)wrap;
    if (!out) return GP_INVALIDARG;
    if (!im || !im->px || im->w <= 0 || im->h <= 0)
        return st_GdipCreateSolidFill(0xFF808080u, out);
    n = (size_t)im->w * im->h;
    for (i = 0; i < n; i++) {
        a += (im->px[i] >> 24) & 0xff; r += (im->px[i] >> 16) & 0xff;
        g += (im->px[i] >> 8) & 0xff;  b += im->px[i] & 0xff;
    }
    return st_GdipCreateSolidFill((uint32_t)((a / n) << 24 | (r / n) << 16 |
                                             (g / n) << 8 | (b / n)), out);
}

/* A string format is alignment and flags. Kept so a caller reads back what it
 * set; the text calls here draw from the top left whatever it says, which is
 * the alignment nearly every caller asks for anyway. */
typedef struct { int tag, align, lalign, flags, trim; } gp_stringformat;
static MS int32_t st_GdipCreateStringFormat(int32_t flags, uint16_t lang, void **out)
{
    gp_stringformat *f;
    (void)lang;
    if (!out) return GP_INVALIDARG;
    if (!(f = gp_new(sizeof *f, GPO_STRINGFORMAT))) return GP_OUTOFMEMORY;
    f->flags = flags;
    *out = f;
    return GP_OK;
}
static MS int32_t st_GdipStringFormatGetGenericDefault(void **out)
{ return st_GdipCreateStringFormat(0, 0, out); }
static MS int32_t st_GdipDeleteStringFormat(void *fmt)
{ if (!gp_check(fmt, GPO_STRINGFORMAT)) return GP_INVALIDARG; free(fmt); return GP_OK; }
static MS int32_t st_GdipSetStringFormatAlign(void *fmt, int32_t align)
{
    gp_stringformat *f = gp_check(fmt, GPO_STRINGFORMAT);
    if (f) f->align = align;
    return f ? GP_OK : GP_INVALIDARG;
}
static MS int32_t st_GdipSetStringFormatLineAlign(void *fmt, int32_t align)
{
    gp_stringformat *f = gp_check(fmt, GPO_STRINGFORMAT);
    if (f) f->lalign = align;
    return f ? GP_OK : GP_INVALIDARG;
}
static MS int32_t st_GdipGetStringFormatAlign(void *fmt, int32_t *align)
{
    gp_stringformat *f = gp_check(fmt, GPO_STRINGFORMAT);
    if (!f || !align) return GP_INVALIDARG;
    *align = f->align;
    return GP_OK;
}

static MS int32_t st_GdipDrawImageRectRectI(void *graphics, void *image,
                                            int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                                            int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                                            int32_t unit, void *attr, void *cb, void *cbdata)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_image *im = gp_check(image, GPO_IMAGE);
    gp_imageattr *ia = gp_check(attr, GPO_IMAGEATTR);
    int W, H, x, y;
    uint32_t *px = gp_pixels(g, &W, &H);
    float ox, oy;

    (void)unit; (void)cb; (void)cbdata;
    if (!g || !im || !px || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return GP_INVALIDARG;
    gp_apply(&g->xf, (float)dx, (float)dy, &ox, &oy);

    for (y = 0; y < dh; y++) {
        int ty = (int)oy + y;
        int syy = sy + (int)((int64_t)y * sh / dh);
        if (ty < 0 || ty >= H || syy < 0 || syy >= im->h) continue;
        for (x = 0; x < dw; x++) {
            int tx = (int)ox + x;
            int sxx = sx + (int)((int64_t)x * sw / dw);
            uint32_t c;
            if (tx < 0 || tx >= W || sxx < 0 || sxx >= im->w) continue;
            if (g->has_clip &&
                ((float)tx < g->cx0 || (float)tx >= g->cx1 ||
                 (float)ty < g->cy0 || (float)ty >= g->cy1)) continue;
            c = im->px[(size_t)syy * im->w + sxx];
            if (ia && ia->has_matrix) {
                /* Only the alpha row is honoured, which is what a plug-in uses
                 * a colour matrix for here: fading a control while it is
                 * disabled. */
                uint32_t a = (uint32_t)(((c >> 24) & 0xFF) * ia->m[3][3]);
                c = (c & 0x00FFFFFFu) | ((a > 255 ? 255 : a) << 24);
            }
            gp_blend(&px[(size_t)ty * W + tx], c, 255);
        }
    }
    return GP_OK;
}

/* ---- matrices and the allocator ----------------------------------------- */

typedef struct { int tag; gp_matrix m; } gp_matobj;

static MS int32_t st_GdipCreateMatrix(void **out)
{
    gp_matobj *m;
    if (!out) return GP_INVALIDARG;
    if (!(m = gp_new(sizeof *m, GPO_MATRIX))) return GP_OUTOFMEMORY;
    gp_ident(&m->m);
    *out = m;
    return GP_OK;
}
static MS int32_t st_GdipCreateMatrix2(float a, float b, float c, float d,
                                       float e, float f, void **out)
{
    gp_matobj *m;
    if (!out) return GP_INVALIDARG;
    if (!(m = gp_new(sizeof *m, GPO_MATRIX))) return GP_OUTOFMEMORY;
    m->m.m[0] = a; m->m.m[1] = b; m->m.m[2] = c;
    m->m.m[3] = d; m->m.m[4] = e; m->m.m[5] = f;
    *out = m;
    return GP_OK;
}
static MS int32_t st_GdipSetMatrixElements(void *matrix, float a, float b, float c,
                                           float d, float e, float f)
{
    gp_matobj *m = gp_check(matrix, GPO_MATRIX);
    if (!m) return GP_INVALIDARG;
    m->m.m[0] = a; m->m.m[1] = b; m->m.m[2] = c;
    m->m.m[3] = d; m->m.m[4] = e; m->m.m[5] = f;
    return GP_OK;
}
static MS int32_t st_GdipDeleteMatrix(void *m)
{ if (!gp_check(m, GPO_MATRIX)) return GP_INVALIDARG; free(m); return GP_OK; }

static MS void *st_GdipAlloc(size_t n) { return malloc(n); }
static MS void  st_GdipFree(void *p) { free(p); }

/* AddPathString turns text into an outline. The glyph outlines are there in
 * FreeType, but a plug-in reaching for this wants a shape to fill rather than
 * text to read -- so the string's box is added instead, which keeps bounds and
 * hit-testing right where drawing it would be wrong. Said once, because a
 * caller doing this does it per label. */
static MS int32_t st_GdipAddPathString(void *path, const uint16_t *str, int32_t len,
                                       void *family, int32_t style, float em,
                                       const float *layout, void *fmt)
{
    gp_path *p = gp_check(path, GPO_PATH);
    static int said;
    float w = 0, h = 0;
    (void)family; (void)style; (void)fmt;
    if (!p || !layout) return GP_INVALIDARG;
    if (len < 0) { len = 0; while (str && str[len]) len++; }
    if (!said) {
        said = 1;
        PLOG("  [gdi+] AddPathString: adding the text's box rather than its "
             "outlines\n");
    }
    gp_text_extent(str, len, em, &w, &h);
    return st_GdipAddPathRectangle(path, layout[0], layout[1], w, h);
}

/* ---- the rest of the surface an editor reaches --------------------------
 *
 * Everything below was a stand-in until a plug-in was seen calling it. They are
 * the integer spellings of calls already here, the matrix operations a control
 * uses to place itself, and the cached bitmap a knob redraws itself from.
 * Grouped rather than scattered because none of them is interesting on its own:
 * each is the same drawing this file already does, reached by another name. */

/* Matrices. The elements live immediately after the tag, which is how
 * GdipGetWorldTransform already reads them. */
static gp_matrix *gp_mat(void *m)
{ return gp_check(m, GPO_MATRIX) ? (gp_matrix *)((int *)m + 1) : NULL; }

static MS int32_t st_GdipGetMatrixElements(void *matrix, float *out)
{
    gp_matrix *m = gp_mat(matrix);
    if (!m || !out) return GP_INVALIDARG;
    memcpy(out, m->m, sizeof m->m);
    return GP_OK;
}
static MS int32_t st_GdipTransformMatrixPoints(void *matrix, float *pts, int32_t n)
{
    gp_matrix *m = gp_mat(matrix);
    int32_t i;
    if (!m || !pts || n < 0) return GP_INVALIDARG;
    for (i = 0; i < n; i++) {
        float x, y;
        gp_apply(m, pts[i * 2], pts[i * 2 + 1], &x, &y);
        pts[i * 2] = x; pts[i * 2 + 1] = y;
    }
    return GP_OK;
}
static MS int32_t st_GdipTransformMatrixPointsI(void *matrix, int32_t *pts, int32_t n)
{
    gp_matrix *m = gp_mat(matrix);
    int32_t i;
    if (!m || !pts || n < 0) return GP_INVALIDARG;
    for (i = 0; i < n; i++) {
        float x, y;
        gp_apply(m, (float)pts[i * 2], (float)pts[i * 2 + 1], &x, &y);
        pts[i * 2] = (int32_t)x; pts[i * 2 + 1] = (int32_t)y;
    }
    return GP_OK;
}
/* order 0 is MatrixOrderPrepend, 1 is Append. Prepend is the default and the
 * one a caller building a transform out of steps means. */
static void gp_mat_mul(gp_matrix *dst, const gp_matrix *a, const gp_matrix *b)
{
    gp_matrix r;                       /* r = a then b, i.e. b * a */
    r.m[0] = a->m[0] * b->m[0] + a->m[1] * b->m[2];
    r.m[1] = a->m[0] * b->m[1] + a->m[1] * b->m[3];
    r.m[2] = a->m[2] * b->m[0] + a->m[3] * b->m[2];
    r.m[3] = a->m[2] * b->m[1] + a->m[3] * b->m[3];
    r.m[4] = a->m[4] * b->m[0] + a->m[5] * b->m[2] + b->m[4];
    r.m[5] = a->m[4] * b->m[1] + a->m[5] * b->m[3] + b->m[5];
    *dst = r;
}
static void gp_mat_combine(gp_matrix *m, const gp_matrix *step, int32_t order)
{
    if (order) gp_mat_mul(m, m, step);            /* Append */
    else       gp_mat_mul(m, step, m);            /* Prepend */
}
static void gp_mat_translate(gp_matrix *s, float dx, float dy)
{ s->m[0] = 1; s->m[1] = 0; s->m[2] = 0; s->m[3] = 1; s->m[4] = dx; s->m[5] = dy; }
static void gp_mat_scale(gp_matrix *s, float sx, float sy)
{ s->m[0] = sx; s->m[1] = 0; s->m[2] = 0; s->m[3] = sy; s->m[4] = 0; s->m[5] = 0; }
static void gp_mat_rotate(gp_matrix *s, float deg)
{
    float r = deg * 3.14159265358979f / 180.0f, c = cosf(r), n = sinf(r);
    s->m[0] = c; s->m[1] = n; s->m[2] = -n; s->m[3] = c; s->m[4] = 0; s->m[5] = 0;
}

static MS int32_t st_GdipTranslateMatrix(void *matrix, float dx, float dy, int32_t order)
{
    gp_matrix *m = gp_mat(matrix), step;
    if (!m) return GP_INVALIDARG;
    gp_mat_translate(&step, dx, dy); gp_mat_combine(m, &step, order);
    return GP_OK;
}
static MS int32_t st_GdipScaleMatrix(void *matrix, float sx, float sy, int32_t order)
{
    gp_matrix *m = gp_mat(matrix), step;
    if (!m) return GP_INVALIDARG;
    gp_mat_scale(&step, sx, sy); gp_mat_combine(m, &step, order);
    return GP_OK;
}
static MS int32_t st_GdipRotateMatrix(void *matrix, float angle, int32_t order)
{
    gp_matrix *m = gp_mat(matrix), step;
    if (!m) return GP_INVALIDARG;
    gp_mat_rotate(&step, angle); gp_mat_combine(m, &step, order);
    return GP_OK;
}
static MS int32_t st_GdipMultiplyMatrix(void *matrix, void *other, int32_t order)
{
    gp_matrix *m = gp_mat(matrix), *o = gp_mat(other);
    if (!m || !o) return GP_INVALIDARG;
    gp_mat_combine(m, o, order);
    return GP_OK;
}

static MS int32_t st_GdipScaleWorldTransform(void *graphics, float sx, float sy, int32_t order)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_matrix step;
    if (!g) return GP_INVALIDARG;
    gp_mat_scale(&step, sx, sy); gp_mat_combine(&g->xf, &step, order);
    return GP_OK;
}
static MS int32_t st_GdipRotateWorldTransform(void *graphics, float angle, int32_t order)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_matrix step;
    if (!g) return GP_INVALIDARG;
    gp_mat_rotate(&step, angle); gp_mat_combine(&g->xf, &step, order);
    return GP_OK;
}
static MS int32_t st_GdipResetWorldTransform(void *graphics)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    if (!g) return GP_INVALIDARG;
    g->xf.m[0] = 1; g->xf.m[1] = 0; g->xf.m[2] = 0;
    g->xf.m[3] = 1; g->xf.m[4] = 0; g->xf.m[5] = 0;
    return GP_OK;
}
static MS int32_t st_GdipMultiplyWorldTransform(void *graphics, void *matrix, int32_t order)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    gp_matrix *m = gp_mat(matrix);
    if (!g || !m) return GP_INVALIDARG;
    gp_mat_combine(&g->xf, m, order);
    return GP_OK;
}

/* A gradient given as a rectangle and an angle, rather than two points: the
 * axis runs across the rectangle in the direction of the angle, measured
 * clockwise from the x axis the way GDI+ measures it. */
static MS int32_t st_GdipCreateLineBrushFromRectWithAngle(const float *rect,
        uint32_t c1, uint32_t c2, float angle, int32_t scalable, int32_t wrap, void **out)
{
    float r = angle * 3.14159265358979f / 180.0f;
    float cx, cy, dx, dy, half, p1[2], p2[2];
    (void)scalable;
    if (!rect || !out) return GP_INVALIDARG;
    cx = rect[0] + rect[2] / 2.0f;
    cy = rect[1] + rect[3] / 2.0f;
    dx = cosf(r); dy = sinf(r);
    /* The extent of the rectangle along the axis, which is what makes a
     * 45-degree gradient reach both corners rather than stopping short. */
    half = (fabsf(dx) * rect[2] + fabsf(dy) * rect[3]) / 2.0f;
    p1[0] = cx - dx * half; p1[1] = cy - dy * half;
    p2[0] = cx + dx * half; p2[1] = cy + dy * half;
    return st_GdipCreateLineBrush(p1, p2, c1, c2, wrap, out);
}
static MS int32_t st_GdipCreateLineBrushFromRectWithAngleI(const int32_t *rect,
        uint32_t c1, uint32_t c2, float angle, int32_t scalable, int32_t wrap, void **out)
{
    float f[4];
    if (!rect) return GP_INVALIDARG;
    f[0] = (float)rect[0]; f[1] = (float)rect[1];
    f[2] = (float)rect[2]; f[3] = (float)rect[3];
    return st_GdipCreateLineBrushFromRectWithAngle(f, c1, c2, angle, scalable, wrap, out);
}
/* mode 0 horizontal, 1 vertical, 2 forward diagonal, 3 backward diagonal. */
static MS int32_t st_GdipCreateLineBrushFromRect(const float *rect, uint32_t c1,
                                                 uint32_t c2, int32_t mode,
                                                 int32_t wrap, void **out)
{
    static const float deg[4] = { 0.0f, 90.0f, 45.0f, 135.0f };
    return st_GdipCreateLineBrushFromRectWithAngle(
        rect, c1, c2, deg[mode >= 0 && mode < 4 ? mode : 0], 0, wrap, out);
}
static MS int32_t st_GdipCreateLineBrushFromRectI(const int32_t *rect, uint32_t c1,
                                                  uint32_t c2, int32_t mode,
                                                  int32_t wrap, void **out)
{
    float f[4];
    if (!rect) return GP_INVALIDARG;
    f[0] = (float)rect[0]; f[1] = (float)rect[1];
    f[2] = (float)rect[2]; f[3] = (float)rect[3];
    return st_GdipCreateLineBrushFromRect(f, c1, c2, mode, wrap, out);
}

static MS int32_t st_GdipSetStringFormatFlags(void *fmt, int32_t flags)
{
    gp_stringformat *f = gp_check(fmt, GPO_STRINGFORMAT);
    if (!f) return GP_INVALIDARG;
    f->flags = flags;
    return GP_OK;
}
static MS int32_t st_GdipGetStringFormatFlags(void *fmt, int32_t *out)
{
    gp_stringformat *f = gp_check(fmt, GPO_STRINGFORMAT);
    if (!f || !out) return GP_INVALIDARG;
    *out = f->flags;
    return GP_OK;
}
static MS int32_t st_GdipSetStringFormatTrimming(void *fmt, int32_t trim)
{
    gp_stringformat *f = gp_check(fmt, GPO_STRINGFORMAT);
    if (!f) return GP_INVALIDARG;
    f->trim = trim;
    return GP_OK;
}

/* A cached bitmap is GDI+'s promise to keep a device-ready copy of an image.
 * There is no device here, so it is the image -- borrowed, not copied, which
 * is why deleting one does not touch it. A knob that builds one per frame then
 * costs a small allocation rather than a screenful of pixels. */
typedef struct { int tag; gp_image *img; } gp_cached;

static MS int32_t st_GdipCreateCachedBitmap(void *image, void *graphics, void **out)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    gp_cached *c;
    (void)graphics;
    if (!im || !out) return GP_INVALIDARG;
    if (!(c = gp_new(sizeof *c, GPO_IMAGE))) return GP_OUTOFMEMORY;
    c->tag = GPO_CACHED;
    c->img = im;
    *out = c;
    return GP_OK;
}
static MS int32_t st_GdipDeleteCachedBitmap(void *cached)
{
    gp_cached *c = gp_check(cached, GPO_CACHED);
    if (!c) return GP_INVALIDARG;
    free(c);
    return GP_OK;
}
static MS int32_t st_GdipDrawCachedBitmap(void *graphics, void *cached, int32_t x, int32_t y)
{
    gp_cached *c = gp_check(cached, GPO_CACHED);
    if (!c || !c->img) return GP_INVALIDARG;
    return st_GdipDrawImageI(graphics, c->img, x, y);
}

/* The spellings of a draw this file already does. */
static MS int32_t st_GdipDrawImage(void *graphics, void *image, float x, float y)
{ return st_GdipDrawImageI(graphics, image, (int32_t)x, (int32_t)y); }
static MS int32_t st_GdipDrawImageRect(void *graphics, void *image,
                                       float x, float y, float w, float h)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im) return GP_INVALIDARG;
    return st_GdipDrawImageRectRectI(graphics, image, (int32_t)x, (int32_t)y,
                                     (int32_t)w, (int32_t)h, 0, 0, im->w, im->h,
                                     2 /* UnitPixel */, NULL, NULL, NULL);
}
static MS int32_t st_GdipDrawImageRectI(void *graphics, void *image,
                                        int32_t x, int32_t y, int32_t w, int32_t h)
{ return st_GdipDrawImageRect(graphics, image, (float)x, (float)y, (float)w, (float)h); }
static MS int32_t st_GdipDrawImagePointRectI(void *graphics, void *image,
        int32_t x, int32_t y, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t unit)
{
    return st_GdipDrawImageRectRectI(graphics, image, x, y, sw, sh,
                                     sx, sy, sw, sh, unit, NULL, NULL, NULL);
}
static MS int32_t st_GdipDrawImagePointRect(void *graphics, void *image,
        float x, float y, float sx, float sy, float sw, float sh, int32_t unit)
{
    return st_GdipDrawImageRectRectI(graphics, image, (int32_t)x, (int32_t)y,
                                     (int32_t)sw, (int32_t)sh, (int32_t)sx, (int32_t)sy,
                                     (int32_t)sw, (int32_t)sh, unit, NULL, NULL, NULL);
}
static MS int32_t st_GdipDrawImageRectRect(void *graphics, void *image,
        float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh,
        int32_t unit, void *attr, void *cb, void *cbdata)
{
    return st_GdipDrawImageRectRectI(graphics, image, (int32_t)dx, (int32_t)dy,
                                     (int32_t)dw, (int32_t)dh, (int32_t)sx, (int32_t)sy,
                                     (int32_t)sw, (int32_t)sh, unit, attr, cb, cbdata);
}

static MS int32_t st_GdipDrawLineI(void *g, void *p, int32_t x1, int32_t y1,
                                   int32_t x2, int32_t y2)
{ return st_GdipDrawLine(g, p, (float)x1, (float)y1, (float)x2, (float)y2); }
static MS int32_t st_GdipFillRectangleI(void *g, void *b, int32_t x, int32_t y,
                                        int32_t w, int32_t h)
{ return st_GdipFillRectangle(g, b, (float)x, (float)y, (float)w, (float)h); }
static MS int32_t st_GdipDrawRectangleI(void *g, void *p, int32_t x, int32_t y,
                                        int32_t w, int32_t h)
{ return st_GdipDrawRectangle(g, p, (float)x, (float)y, (float)w, (float)h); }
static MS int32_t st_GdipFillEllipseI(void *g, void *b, int32_t x, int32_t y,
                                      int32_t w, int32_t h)
{ return st_GdipFillEllipse(g, b, (float)x, (float)y, (float)w, (float)h); }
static MS int32_t st_GdipDrawEllipseI(void *g, void *p, int32_t x, int32_t y,
                                      int32_t w, int32_t h)
{ return st_GdipDrawEllipse(g, p, (float)x, (float)y, (float)w, (float)h); }

/* Arcs and pies, built as a path and then drawn the way any other path is. */
static MS int32_t st_GdipDrawArc(void *graphics, void *pen, float x, float y,
                                 float w, float h, float start, float sweep)
{
    void *path = NULL;
    int32_t st;
    if (st_GdipCreatePath(0, &path) != GP_OK) return GP_OUTOFMEMORY;
    st_GdipAddPathArc(path, x, y, w, h, start, sweep);
    st = st_GdipDrawPath(graphics, pen, path);
    st_GdipDeletePath(path);
    return st;
}
static MS int32_t st_GdipDrawArcI(void *g, void *p, int32_t x, int32_t y,
                                  int32_t w, int32_t h, float start, float sweep)
{ return st_GdipDrawArc(g, p, (float)x, (float)y, (float)w, (float)h, start, sweep); }

static MS int32_t st_GdipFillPie(void *graphics, void *brush, float x, float y,
                                 float w, float h, float start, float sweep)
{
    void *path = NULL;
    int32_t st;
    if (st_GdipCreatePath(0, &path) != GP_OK) return GP_OUTOFMEMORY;
    /* A pie is the arc closed back through the centre, which is what
     * ClosePathFigure does once the arc has been added after a move to it. */
    st_GdipAddPathLine(path, x + w / 2.0f, y + h / 2.0f, x + w / 2.0f, y + h / 2.0f);
    st_GdipAddPathArc(path, x, y, w, h, start, sweep);
    st_GdipClosePathFigure(path);
    st = st_GdipFillPath(graphics, brush, path);
    st_GdipDeletePath(path);
    return st;
}
static MS int32_t st_GdipFillPieI(void *g, void *b, int32_t x, int32_t y,
                                  int32_t w, int32_t h, float start, float sweep)
{ return st_GdipFillPie(g, b, (float)x, (float)y, (float)w, (float)h, start, sweep); }
static MS int32_t st_GdipDrawPie(void *graphics, void *pen, float x, float y,
                                 float w, float h, float start, float sweep)
{
    void *path = NULL;
    int32_t st;
    if (st_GdipCreatePath(0, &path) != GP_OK) return GP_OUTOFMEMORY;
    st_GdipAddPathLine(path, x + w / 2.0f, y + h / 2.0f, x + w / 2.0f, y + h / 2.0f);
    st_GdipAddPathArc(path, x, y, w, h, start, sweep);
    st_GdipClosePathFigure(path);
    st = st_GdipDrawPath(graphics, pen, path);
    st_GdipDeletePath(path);
    return st;
}
static MS int32_t st_GdipDrawPieI(void *g, void *p, int32_t x, int32_t y,
                                  int32_t w, int32_t h, float start, float sweep)
{ return st_GdipDrawPie(g, p, (float)x, (float)y, (float)w, (float)h, start, sweep); }

/* One pixel at a time, which is how a plug-in reads a skin's colour key or
 * writes a meter's tip. ARGB in and out, matching the buffer's own order. */
static MS int32_t st_GdipBitmapGetPixel(void *image, int32_t x, int32_t y, uint32_t *out)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im || !im->px || !out) return GP_INVALIDARG;
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return GP_INVALIDARG;
    *out = im->px[(size_t)y * im->w + x];
    return GP_OK;
}
static MS int32_t st_GdipBitmapSetPixel(void *image, int32_t x, int32_t y, uint32_t argb)
{
    gp_image *im = gp_check(image, GPO_IMAGE);
    if (!im || !im->px) return GP_INVALIDARG;
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return GP_INVALIDARG;
    im->px[(size_t)y * im->w + x] = argb;
    return GP_OK;
}

/* The number of points in a path, and the points themselves: a plug-in that
 * builds a shape and then measures it asks for these, and a count of zero sends
 * it down a path where it draws nothing. */
static MS int32_t st_GdipGetPointCount(void *path, int32_t *out)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p || !out) return GP_INVALIDARG;
    *out = p->n;
    return GP_OK;
}
static MS int32_t st_GdipGetPathPoints(void *path, float *pts, int32_t n)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p || !pts || n < p->n) return GP_INVALIDARG;
    memcpy(pts, p->pt, (size_t)p->n * 2 * sizeof(float));
    return GP_OK;
}
static MS int32_t st_GdipGetPathTypes(void *path, uint8_t *types, int32_t n)
{
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p || !types || n < p->n) return GP_INVALIDARG;
    memcpy(types, p->ty, (size_t)p->n);
    return GP_OK;
}
/* PathData is the pair of the two above behind one pointer: { count, points,
 * types }, with the caller owning the arrays it points at. */
static MS int32_t st_GdipGetPathData(void *path, void *data)
{
    struct { int32_t count; float *points; uint8_t *types; } *d = data;
    gp_path *p = gp_check(path, GPO_PATH);
    if (!p || !d) return GP_INVALIDARG;
    if (d->count < p->n) return GP_INVALIDARG;
    d->count = p->n;
    if (d->points) memcpy(d->points, p->pt, (size_t)p->n * 2 * sizeof(float));
    if (d->types) memcpy(d->types, p->ty, (size_t)p->n);
    return GP_OK;
}
/* This rasteriser flattens as it fills, so a path is already what Flatten
 * would make of it. */
static MS int32_t st_GdipFlattenPath(void *path, void *matrix, float flatness)
{
    (void)matrix; (void)flatness;
    return gp_check(path, GPO_PATH) ? GP_OK : GP_INVALIDARG;
}

#endif /* PELOAD_GDIPLUS_SHIM_H */
