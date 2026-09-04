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
    GPO_FONT, GPO_FAMILY, GPO_MATRIX, GPO_IMAGE, GPO_IMAGEATTR, GPO_STRINGFORMAT
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
    int W, H, i, y, sub;
    uint32_t *px = gp_pixels(g, &W, &H);
    float *xs = NULL, minY = 1e30f, maxY = -1e30f;
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
    }
    if (minY < 0) minY = 0;
    if (maxY > (float)H) maxY = (float)H;

    for (y = (int)minY; y < (int)maxY + 1 && y < H; y++) {
        if (y < 0) continue;
        memset(cov, 0, (size_t)W);
        for (sub = 0; sub < GP_SS; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) / GP_SS;
            int start = 0, x;
            memset(wind, 0, ((size_t)W + 1) * sizeof *wind);
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
                        int xi = (int)floorf(ix + 0.5f);
                        if (xi < 0) xi = 0;
                        if (xi > W) xi = W;
                        wind[xi] += dir;
                    }
                }
            }
            {   /* Accumulate the crossings into spans. */
                int acc = 0, inside;
                for (x = 0; x < W; x++) {
                    acc += wind[x];
                    inside = p->fillmode ? (acc != 0) : (acc & 1);
                    if (inside && cov[x] < 255)
                        cov[x] = (uint8_t)(cov[x] + 255 / GP_SS);
                }
            }
        }
        for (i = 0; i < W; i++) {
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

/* Regions are not modelled as objects; the clip is a rectangle on the
 * graphics. Bounds therefore report the clip, or an empty rectangle when none
 * is set, and deleting one is a no-op that must still succeed. */
static MS int32_t st_GdipGetRegionBounds(void *region, void *graphics, void *rect)
{
    gp_graphics *g = gp_check(graphics, GPO_GRAPHICS);
    W32_APPROX();                                /* the clip, not a region */
    float *r = rect;
    (void)region;
    if (!r) return GP_INVALIDARG;
    if (g && g->has_clip) {
        r[0] = g->cx0; r[1] = g->cy0;
        r[2] = g->cx1 - g->cx0; r[3] = g->cy1 - g->cy0;
    } else {
        r[0] = r[1] = r[2] = r[3] = 0.0f;
    }
    return GP_OK;
}
static MS int32_t st_GdipDeleteRegion(void *region) { (void)region; return GP_OK; }

/* A pen made from a brush takes the brush's colour, which for a solid brush is
 * exactly right and for a gradient is its starting colour. */
static MS int32_t st_GdipCreatePen1(uint32_t argb, float width, int32_t unit, void **out);
static MS int32_t st_GdipCreatePen2(void *brush, float width, int32_t unit, void **out)
{
    gp_brush *b = gp_check(brush, GPO_BRUSH);
    return st_GdipCreatePen1(b ? b->argb : 0xFF000000u, width, unit, out);
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

#endif /* PELOAD_GDIPLUS_SHIM_H */
