/* Direct3D 11 and Direct2D, far enough for a plug-in to draw with them.
 *
 * A SynthEdit VST3 builds its editor on D3D11: it makes a device, hangs a
 * Direct2D device context off it, and does every bit of its drawing through
 * that context. There is no GPU here and no prospect of one, but the drawing
 * itself is ordinary 2D -- filled rounded rectangles, bitmaps, text -- and the
 * rasteriser written for GDI+ already does all of it.
 *
 * So D3D11 exists here only as far as the plug-in needs to walk through it to
 * reach Direct2D: a device, a DXGI surface standing for the back buffer, and
 * the query-interface chain between them. Everything that would actually touch
 * hardware answers "no". Direct2D is where the work happens, and it renders
 * into the same w32_surf a GDI+ graphics object writes to.
 *
 * Objects reuse the dwprobe machinery from dwrite_shim.h: a full-length vtable
 * of probes, with the slots that matter overwritten. A slot nobody implemented
 * says so under PELOAD_VERBOSE and returns E_NOTIMPL, which is how the list
 * below was arrived at -- run it, read what it asked for, implement that.
 */
#ifndef PELOAD_D3D_SHIM_H
#define PELOAD_D3D_SHIM_H

#define D3D_OK        0
#define D3D_NOTIMPL   ((int32_t)0x80004001)
#define D3D_FAIL      ((int32_t)0x80004005)

/* The surface Direct2D is currently pointed at. A device context's target is
 * set by the plug-in from the swap chain's back buffer; both resolve to the
 * window's pixels, which is what the host presents. */
typedef struct {
    int       tag;
    w32_surf *surf;
    gp_image *img;
} d2_target;

/* The window the swap chain was made for. A plug-in nests its own drawing
 * window inside the one it was given -- VSTGUI inside the container, and the
 * GMPI drawing surface inside that -- and it is the innermost one that
 * Direct2D targets. Taking the display window instead drew into a buffer
 * nothing was reading. */
static int g_d2_hwnd;

static w32_surf *d2_window_surface(void)
{
    int i = g_d2_hwnd;
    if (!i || !W.wnd[i].used) i = W.display ? W.display : W.host;
    if (!i || !W.wnd[i].used) return NULL;
    /* A window created before its size was known has no buffer yet. */
    if (!W.wnd[i].surf.px && W.wnd[i].w > 0 && W.wnd[i].h > 0)
        w32_surf_size(&W.wnd[i].surf, W.wnd[i].w, W.wnd[i].h);
    return &W.wnd[i].surf;
}

/* ---- ID3D11Device, and the little of DXGI it is reached through --------- */

static MS int32_t d3d_qi_noop(void *self, const void *iid, void **out)
{ (void)self; (void)iid; if (out) *out = NULL; return D3D_NOTIMPL; }

static dwprobe *g_d3d_device;
static dwprobe *g_dxgi_surface;

static dwprobe *d3d_device(void);

/* ID3D11Device::QueryInterface. A plug-in asks the device for IDXGIDevice on
 * its way to Direct2D, and for ID3D11Device1 and friends on the way to
 * nothing. Handing back the device itself for the DXGI interfaces is enough:
 * every method it then calls is one of ours. */
/* A GUID, printed the way it is written down, so an unknown interface can be
 * looked up rather than guessed at from a slot number. */
static const char *d3d_guid(const uint8_t *g, char *buf, size_t n)
{
    if (!g) return "(null)";
    snprintf(buf, n, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             *(const uint32_t *)g, *(const uint16_t *)(g + 4),
             *(const uint16_t *)(g + 6),
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

static MS uint32_t d3d_addref(void *self) { (void)self; return 2; }
static MS uint32_t d3d_release(void *self) { (void)self; return 1; }

/* One object per interface, so a probe report names the interface it is really
 * about. Returning the same object for every QueryInterface made slot numbers
 * from four different vtables collide in the log, which is worse than useless.
 *
 * The interfaces are singletons: there is one device, one adapter and one
 * swap chain here, and a plug-in that asks twice means the same thing twice. */
static dwprobe *dxgi_device(void);
static dwprobe *dxgi_adapter(void);
static dwprobe *dxgi_factory(void);
static dwprobe *d2d_factory_obj(void);
/* Defined below, once the objects they hand back exist. */
static MS int32_t dxgi_CreateSwapChainForHwnd(void *self, void *dev, void *hwnd,
                                              const void *desc, const void *fs,
                                              void *restrict_to, void **out);
static MS int32_t d3d_CheckFormatSupport(void *self, uint32_t fmt, uint32_t *support);

/* The GUIDs a plug-in asks this device for, first four bytes each -- enough to
 * tell them apart, and the full value is logged when none matches. */
#define GUID32(p) (*(const uint32_t *)(p))
#define IID_IDXGIDevice32   0x54EC77FAu
#define IID_IDXGIDevice1_32 0x77DB970Fu
#define IID_ID3D11Device1   0xA04BFB29u

static MS int32_t d3d_device_qi(void *self, const uint8_t *iid, void **out)
{
    char b[64];
    (void)self;
    if (!out) return D3D_NOTIMPL;
    *out = NULL;
    if (iid && (GUID32(iid) == IID_IDXGIDevice32 || GUID32(iid) == IID_IDXGIDevice1_32)) {
        *out = dxgi_device();
        PLOG("  [d3d] ID3D11Device::QueryInterface(IDXGIDevice)\n");
        return *out ? D3D_OK : D3D_NOTIMPL;
    }
    /* Anything else is another face of the device itself. */
    *out = d3d_device();
    PLOG("  [d3d] ID3D11Device::QueryInterface(%s) -> self\n",
         d3d_guid(iid, b, sizeof b));
    return D3D_OK;
}

/* ---- IDXGIDevice / IDXGIAdapter / IDXGIFactory --------------------------
 *
 * The walk a plug-in makes from a device to the factory that can give it a
 * swap chain. None of it does anything: the "swap chain" presents into the
 * window's own pixel buffer, which is what this host reads back. */

static MS int32_t dxgi_GetParent(void *self, const uint8_t *iid, void **out)
{
    char b[64];
    (void)self;
    if (!out) return D3D_NOTIMPL;
    *out = dxgi_factory();
    PLOG("  [d3d] IDXGIObject::GetParent(%s) -> factory\n", d3d_guid(iid, b, sizeof b));
    return *out ? D3D_OK : D3D_NOTIMPL;
}
static MS int32_t dxgi_dev_GetAdapter(void *self, void **out)
{
    (void)self;
    if (!out) return D3D_NOTIMPL;
    *out = dxgi_adapter();
    PLOG("  [d3d] IDXGIDevice::GetAdapter\n");
    return *out ? D3D_OK : D3D_NOTIMPL;
}
static MS int32_t dxgi_qi_self(void *self, const uint8_t *iid, void **out)
{
    char b[64];
    if (!out) return D3D_NOTIMPL;
    *out = self;
    PLOG("  [d3d] QueryInterface(%s) -> self\n", d3d_guid(iid, b, sizeof b));
    return D3D_OK;
}

static dwprobe *dxgi_device(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("IDXGIDevice")) != NULL) {
        dwp_set(o, 0, (void *)dxgi_qi_self);
        dwp_set(o, 1, (void *)d3d_addref);
        dwp_set(o, 2, (void *)d3d_release);
        dwp_set(o, 6, (void *)dxgi_GetParent);
        dwp_set(o, 7, (void *)dxgi_dev_GetAdapter);
    }
    return o;
}
static dwprobe *dxgi_adapter(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("IDXGIAdapter")) != NULL) {
        dwp_set(o, 0, (void *)dxgi_qi_self);
        dwp_set(o, 1, (void *)d3d_addref);
        dwp_set(o, 2, (void *)d3d_release);
        dwp_set(o, 6, (void *)dxgi_GetParent);
    }
    return o;
}
static dwprobe *dxgi_factory(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("IDXGIFactory2")) != NULL) {
        dwp_set(o, 0,  (void *)dxgi_qi_self);
        dwp_set(o, 1,  (void *)d3d_addref);
        dwp_set(o, 2,  (void *)d3d_release);
        dwp_set(o, 15, (void *)dxgi_CreateSwapChainForHwnd);
    }
    return o;
}

/* ---- the swap chain ----------------------------------------------------
 *
 * There is nothing to swap. The "back buffer" is the window's own pixel
 * buffer, which is what this host reads back and presents, so a plug-in
 * drawing into the surface it gets from GetBuffer is drawing exactly where it
 * needs to and Present has nothing left to do. */
static dwprobe *dxgi_surface(void);

static MS int32_t dxgi_sc_GetBuffer(void *self, uint32_t idx, const uint8_t *iid, void **out)
{
    char b[64];
    (void)self; (void)idx;
    if (!out) return D3D_NOTIMPL;
    *out = dxgi_surface();
    PLOG("  [d3d] IDXGISwapChain::GetBuffer(%u, %s)\n", idx, d3d_guid(iid, b, sizeof b));
    return *out ? D3D_OK : D3D_NOTIMPL;
}
static MS int32_t dxgi_sc_Present(void *self, uint32_t sync, uint32_t flags)
{ (void)self; (void)sync; (void)flags; return D3D_OK; }
static MS int32_t dxgi_sc_Present1(void *self, uint32_t sync, uint32_t flags, const void *p)
{ (void)self; (void)sync; (void)flags; (void)p; return D3D_OK; }
static MS int32_t dxgi_sc_ResizeBuffers(void *self, uint32_t n, uint32_t w, uint32_t h,
                                        uint32_t fmt, uint32_t flags)
{ (void)self; (void)n; (void)w; (void)h; (void)fmt; (void)flags; return D3D_OK; }
static MS int32_t dxgi_sc_GetDevice(void *self, const uint8_t *iid, void **out)
{ (void)self; (void)iid; if (out) *out = d3d_device(); return D3D_OK; }

static dwprobe *dxgi_swapchain(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("IDXGISwapChain1")) != NULL) {
        dwp_set(o, 0,  (void *)dxgi_qi_self);
        dwp_set(o, 1,  (void *)d3d_addref);
        dwp_set(o, 2,  (void *)d3d_release);
        dwp_set(o, 6,  (void *)dxgi_GetParent);
        dwp_set(o, 7,  (void *)dxgi_sc_GetDevice);
        dwp_set(o, 8,  (void *)dxgi_sc_Present);
        dwp_set(o, 9,  (void *)dxgi_sc_GetBuffer);
        dwp_set(o, 13, (void *)dxgi_sc_ResizeBuffers);
        dwp_set(o, 22, (void *)dxgi_sc_Present1);
    }
    return o;
}

/* IDXGISurface, which is only ever a way of naming the back buffer. Direct2D
 * turns it into a render target, and that is where it becomes real again. */
static MS int32_t dxgi_surf_GetDesc(void *self, void *desc)
{
    w32_surf *s = d2_window_surface();
    uint32_t *d = desc;
    (void)self;
    if (!d) return D3D_NOTIMPL;
    d[0] = (uint32_t)(s ? s->w : 0);          /* Width  */
    d[1] = (uint32_t)(s ? s->h : 0);          /* Height */
    d[2] = 87;                                /* DXGI_FORMAT_B8G8R8A8_UNORM */
    d[3] = 1; d[4] = 0;                       /* SampleDesc */
    return D3D_OK;
}
static dwprobe *dxgi_surface(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("IDXGISurface")) != NULL) {
        dwp_set(o, 0, (void *)dxgi_qi_self);
        dwp_set(o, 1, (void *)d3d_addref);
        dwp_set(o, 2, (void *)d3d_release);
        dwp_set(o, 6, (void *)dxgi_GetParent);
        dwp_set(o, 8, (void *)dxgi_surf_GetDesc);
    }
    return o;
}

/* IDXGIFactory2::CreateSwapChainForHwnd(device, hwnd, desc1, fullscreen,
 *                                       restrictTo, IDXGISwapChain1**) */
static MS int32_t dxgi_CreateSwapChainForHwnd(void *self, void *dev, void *hwnd,
                                              const void *desc, const void *fs,
                                              void *restrict_to, void **out)
{
    (void)self; (void)dev; (void)desc; (void)fs; (void)restrict_to;
    {   /* Remember which window this chain presents to; everything Direct2D
         * draws goes into its pixels. */
        w32_wnd *w = w32_wget(hwnd);
        int i;
        g_d2_hwnd = 0;
        for (i = 1; w && i < W32_MAX_WND; i++)
            if (&W.wnd[i] == w) { g_d2_hwnd = i; break; }
    }
    PLOG("  [d3d] IDXGIFactory2::CreateSwapChainForHwnd(window #%d)\n", g_d2_hwnd);
    if (!out) return D3D_NOTIMPL;
    *out = dxgi_swapchain();
    return *out ? D3D_OK : D3D_NOTIMPL;
}

/* ID3D11Device::CheckFormatSupport. Everything is supported: the formats a
 * plug-in asks about are all 32-bit colour, and the rasteriser behind this
 * works in exactly one of them. */
static MS int32_t d3d_CheckFormatSupport(void *self, uint32_t fmt, uint32_t *support)
{
    (void)self; (void)fmt;
    if (support) *support = 0x20 | 0x80 | 0x4000;   /* TEXTURE2D | RENDER_TARGET | DISPLAY */
    return D3D_OK;
}

static dwprobe *d3d_device(void)
{
    if (!g_d3d_device) {
        g_d3d_device = dwp_new("ID3D11Device");
        if (g_d3d_device) {
            dwp_set(g_d3d_device, 0, (void *)d3d_device_qi);
            dwp_set(g_d3d_device, 1, (void *)d3d_addref);
            dwp_set(g_d3d_device, 2, (void *)d3d_release);
            dwp_set(g_d3d_device, 29, (void *)d3d_CheckFormatSupport);
        }
    }
    return g_d3d_device;
}

/* D3D11CreateDevice(adapter, driverType, software, flags, featureLevels,
 *                   nLevels, sdkVersion, ppDevice, pFeatureLevel, ppContext) */
static MS int32_t st_D3D11CreateDevice(void *adapter, int32_t driver, void *sw,
                                       uint32_t flags, const uint32_t *levels,
                                       uint32_t nlevels, uint32_t sdk,
                                       void **device, uint32_t *level, void **ctx)
{
    (void)adapter; (void)driver; (void)sw; (void)flags; (void)levels;
    (void)nlevels; (void)sdk;
    PLOG("  [d3d] D3D11CreateDevice(driverType=%d)\n", (int)driver);
    if (device) *device = d3d_device();
    if (level)  *level = 0xb000;                      /* D3D_FEATURE_LEVEL_11_0 */
    if (ctx)    *ctx = d3d_device();
    return (device && !*device) ? D3D_FAIL : D3D_OK;
}



/* ---- Direct2D -----------------------------------------------------------
 *
 * This is where the drawing actually happens. Everything above exists so the
 * plug-in can walk from a device to a device context; from here on the calls
 * are ordinary 2D and go to the same rasteriser GDI+ uses.
 *
 * The context draws into the window's own pixel buffer. Direct2D's model --
 * BeginDraw, a pile of Fill/Draw calls, EndDraw -- maps onto that directly,
 * because there is no queue and nothing to flush. */

static dwprobe *d2d_context(void);

typedef struct { int tag; uint32_t argb; } d2_brush;

/* D2D1_COLOR_F is four floats, straight-alpha and in 0..1. */
static uint32_t d2_color(const float *c)
{
    uint32_t r, g, b, a;
    if (!c) return 0xFF000000u;
    r = (uint32_t)(c[0] * 255.0f + 0.5f); g = (uint32_t)(c[1] * 255.0f + 0.5f);
    b = (uint32_t)(c[2] * 255.0f + 0.5f); a = (uint32_t)(c[3] * 255.0f + 0.5f);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (a > 255) a = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* The transform, which everything drawn goes through.
 *
 * Discarding it was not a missing feature, it was every control drawn at the
 * origin: a skinned editor positions each piece of its interface by setting the
 * transform and then blitting at 0,0, so ignoring it stacks the whole interface
 * in the top-left corner. That is what the first picture out of this shim was
 * -- a logo and one knob, overdrawing each other.
 *
 * D2D1_MATRIX_3X2_F is m11, m12, m21, m22, dx, dy, which is the order and the
 * convention gp_matrix already uses, so it copies straight across. */
static float g_d2_xf[6] = { 1, 0, 0, 1, 0, 0 };

static MS void d2_SetTransform(void *self, const float *m)
{
    (void)self;
    if (m) memcpy(g_d2_xf, m, sizeof g_d2_xf);
    else { g_d2_xf[0] = g_d2_xf[3] = 1; g_d2_xf[1] = g_d2_xf[2] = 0;
           g_d2_xf[4] = g_d2_xf[5] = 0; }
}
/* Read back far more often than it is set -- 298 calls to 0 in one paint here.
 * A slot that answers E_NOTIMPL leaves the caller's matrix whatever was on its
 * stack, and it then draws relative to that. */
static MS void d2_GetTransform(void *self, float *m)
{ (void)self; if (m) memcpy(m, g_d2_xf, sizeof g_d2_xf); }
#define D2_CLIP_DEPTH 32
typedef struct { int has; float x0, y0, x1, y1; } d2_clip;
static d2_clip g_d2_clip[D2_CLIP_DEPTH];      /* [0] is "no clip"; the top is in force */
static int     g_d2_clipn;

/* A point through the transform in force, which is the space a clip rectangle
 * arrives in. */
static void d2_xf_point(float x, float y, float *ox, float *oy)
{
    *ox = g_d2_xf[0] * x + g_d2_xf[2] * y + g_d2_xf[4];
    *oy = g_d2_xf[1] * x + g_d2_xf[3] * y + g_d2_xf[5];
}

/* A graphics object over the window surface, made fresh per call: the
 * rasteriser keeps no state worth caching and the transform lives on the
 * context. */
static gp_graphics *d2_gfx(dwprobe *ctx)
{
    static gp_graphics g;
    w32_surf *s = d2_window_surface();
    memset(&g, 0, sizeof g);
    g.tag = GPO_GRAPHICS;
    g.surf = s;
    memcpy(g.xf.m, g_d2_xf, sizeof g_d2_xf);
    /* Rebuilt for every call, so the clip has to be applied here rather than
     * left on the object -- see d2_PushAxisAlignedClip. */
    if (g_d2_clip[g_d2_clipn].has) {
        g.has_clip = 1;
        g.cx0 = g_d2_clip[g_d2_clipn].x0;
        g.cy0 = g_d2_clip[g_d2_clipn].y0;
        g.cx1 = g_d2_clip[g_d2_clipn].x1;
        g.cy1 = g_d2_clip[g_d2_clipn].y1;
    }
    (void)ctx;
    return s ? &g : NULL;
}

/* A brush is set up once and recoloured per control, so SetColor is not
 * optional: 56 calls in a paint that creates 158 brushes. */
static MS void d2_brush_SetColor(void *self, const float *color)
{
    dwprobe *o = self;
    d2_brush *b = o ? o->ctx : NULL;
    if (b) b->argb = d2_color(color);
}
static MS void *d2_brush_GetColor(void *self, float *out)
{
    dwprobe *o = self;
    d2_brush *b = o ? o->ctx : NULL;
    uint32_t c = b ? b->argb : 0xFF000000u;
    if (out) {
        out[0] = (float)((c >> 16) & 0xFF) / 255.0f;
        out[1] = (float)((c >> 8) & 0xFF) / 255.0f;
        out[2] = (float)(c & 0xFF) / 255.0f;
        out[3] = (float)((c >> 24) & 0xFF) / 255.0f;
    }
    return out;
}
static MS void d2_brush_SetOpacity(void *self, float o) { (void)self; (void)o; }
static MS float d2_brush_GetOpacity(void *self) { (void)self; return 1.0f; }

static MS int32_t d2_CreateSolidColorBrush(void *self, const float *color,
                                           const void *props, void **out)
{
    PLOG("  [d2d] CreateSolidColorBrush\n");
    d2_brush *b;
    (void)self; (void)props;
    if (!out) return D3D_NOTIMPL;
    if (!(b = calloc(1, sizeof *b))) return D3D_FAIL;
    b->tag = GPO_BRUSH;
    b->argb = d2_color(color);
    /* Handed back as a probe so the caller can Release it and set its colour;
     * the colour is what any drawing call actually reads. */
    {
        dwprobe *o = dwp_new("ID2D1SolidColorBrush");
        if (!o) { free(b); return D3D_FAIL; }
        o->ctx = b;
        dwp_set(o, 1, (void *)d3d_addref);
        dwp_set(o, 2, (void *)d3d_release);
        dwp_set(o, 4, (void *)d2_brush_SetOpacity);
        dwp_set(o, 5, (void *)d2_brush_GetOpacity);
        dwp_set(o, 8, (void *)d2_brush_SetColor);
        dwp_set(o, 9, (void *)d2_brush_GetColor);
        *out = o;
    }
    return D3D_OK;
}
static uint32_t d2_brush_argb(void *brush)
{
    dwprobe *o = brush;
    d2_brush *b = o ? o->ctx : NULL;
    return b ? b->argb : 0xFF000000u;
}

/* D2D1_RECT_F is left, top, right, bottom. */
static MS int32_t d2_FillRectangle(void *self, const float *rc, void *brush)
{
    gp_graphics *g = d2_gfx(self);
    gp_brush b;
    gp_path p;
    if (!g || !rc) return D3D_NOTIMPL;
    memset(&b, 0, sizeof b); b.tag = GPO_BRUSH; b.kind = GPB_SOLID;
    b.argb = d2_brush_argb(brush);
    memset(&p, 0, sizeof p); p.fillmode = 1;
    gp_moveto(&p, rc[0], rc[1]); gp_lineto(&p, rc[2], rc[1]);
    gp_lineto(&p, rc[2], rc[3]); gp_lineto(&p, rc[0], rc[3]);
    if (p.n) p.ty[p.n - 1] |= 0x80;
    gp_fill_path_brush(g, &p, &b);
    free(p.pt); free(p.ty);
    return D3D_OK;
}
/* Clear paints the clip region, not the whole target -- which matters for the
 * same reason the clip itself does. */
static MS int32_t d2_Clear(void *self, const float *color)
{
    gp_graphics *g = d2_gfx(self);
    int W, H, x, y, x0, y0, x1, y1;
    uint32_t *px = gp_pixels(g, &W, &H), c = d2_color(color);
    if (!px) return D3D_NOTIMPL;
    x0 = 0; y0 = 0; x1 = W; y1 = H;
    if (g->has_clip) {
        if (g->cx0 > (float)x0) x0 = (int)g->cx0;
        if (g->cy0 > (float)y0) y0 = (int)g->cy0;
        if (g->cx1 < (float)x1) x1 = (int)g->cx1;
        if (g->cy1 < (float)y1) y1 = (int)g->cy1;
    }
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            px[(size_t)y * W + x] = c | 0xFF000000u;
    return D3D_OK;
}
static MS void d2_BeginDraw(void *self)
{
    (void)self;
    /* The clip stack is balanced within one frame. Starting each frame with it
     * empty means a plug-in that pushes without popping loses its clip at the
     * frame boundary rather than clipping every later frame away to nothing. */
    g_d2_clipn = 0;
    PLOG("  [d2d] BeginDraw\n");
}
static MS int32_t d2_EndDraw(void *self, uint64_t *tag1, uint64_t *tag2)
{
    (void)self;
    if (tag1) *tag1 = 0;
    if (tag2) *tag2 = 0;
    /* Nothing to swap: the pixels went straight into the window's own buffer.
     * They still have to reach the window the host actually presents, which is
     * an ancestor of this one -- there is no compositor here to walk the child
     * windows, and this is the only child that draws. */
    {
        int disp = W.display ? W.display : W.host;
        if (g_d2_hwnd && disp && g_d2_hwnd != disp &&
            W.wnd[g_d2_hwnd].surf.px && W.wnd[disp].surf.px) {
            w32_surf *src = &W.wnd[g_d2_hwnd].surf, *dst = &W.wnd[disp].surf;
            int y, rows = src->h < dst->h ? src->h : dst->h;
            int cols = src->w < dst->w ? src->w : dst->w;
            for (y = 0; y < rows; y++)
                memcpy(dst->px + (size_t)y * dst->w,
                       src->px + (size_t)y * src->w, (size_t)cols * 4);
        }
        w32_present(disp);
    }
    return D3D_OK;
}
/* ID2D1DeviceContext::CreateBitmapFromDxgiSurface(surface, props, ID2D1Bitmap1**)
 * The back buffer, named as something Direct2D can draw into. It is the
 * window's pixel buffer either way, so the object only has to exist and be
 * accepted by SetTarget. */
static MS int32_t d2_CreateBitmapFromDxgiSurface(void *self, void *surface,
                                                 const void *props, void **out)
{
    dwprobe *o;
    (void)self; (void)surface; (void)props;
    if (!out) return D3D_NOTIMPL;
    if (!(o = dwp_new("ID2D1Bitmap1"))) return D3D_FAIL;
    dwp_set(o, 0, (void *)dxgi_qi_self);
    dwp_set(o, 1, (void *)d3d_addref);
    dwp_set(o, 2, (void *)d3d_release);
    PLOG("  [d2d] CreateBitmapFromDxgiSurface -> the window's own pixels\n");
    *out = o;
    return D3D_OK;
}
static MS int32_t d2_SetTarget(void *self, void *target)
{ (void)self; PLOG("  [d2d] SetTarget(%p)\n", target); return D3D_OK; }
static MS void d2_SetDpi(void *self, float x, float y) { (void)self; (void)x; (void)y; }
static MS void d2_GetDpi(void *self, float *x, float *y)
{ (void)self; if (x) *x = 96.0f; if (y) *y = 96.0f; }

/* ID2D1RenderTarget::DrawText -- the labels on every control. The size comes
 * from the text format, the colour from the brush, and the glyphs from the same
 * FreeType face the GDI text calls and DirectWrite use. */
static MS int32_t d2_DrawText(void *self, const uint16_t *str, uint32_t len,
                              void *format, const float *rc, void *brush,
                              uint32_t opts, uint32_t measure)
{
    gp_graphics *g = d2_gfx(self);
    dwprobe *fo = format;
    dw_format *f = fo ? (dw_format *)fo->ctx : NULL;
    char buf[512];
    uint32_t i;
    int n = 0, tw = 0, th = 0, asc = 0, em;
    float x, y;
    uint32_t argb;
    int W, H;
    uint32_t *px = gp_pixels(g, &W, &H);

    (void)opts; (void)measure;
    if (!g || !px || !str || !rc) return D3D_NOTIMPL;
    em = (int)((f ? f->size : 12.0f) + 0.5f);
    for (i = 0; i < len && n + 1 < (int)sizeof buf; i++)
        buf[n++] = str[i] < 256 ? (char)str[i] : '?';
    buf[n] = 0;
    if (!n) return D3D_OK;

    dw_text_measure(buf, n, em, &tw, &th, &asc);
    /* The layout rectangle is in the caller's space; the transform is what puts
     * it where the control actually is. */
    gp_apply(&g->xf, rc[0], rc[1], &x, &y);
    argb = d2_brush_argb(brush);
    dw_text_draw(px, W, H, (int)x, (int)y + asc, buf, n, em,
                 argb & 0x00FFFFFFu, NULL);
    return D3D_OK;
}

/* ---- bitmaps ------------------------------------------------------------
 *
 * A skinned editor is almost entirely bitmaps: this one decodes 96 PNGs through
 * WIC, hands each to Direct2D, and then draws its whole interface by blitting
 * pieces of them. So an ID2D1Bitmap here is a gp_image, which is what the GDI+
 * rasteriser beside this already knows how to scale and alpha-blend.
 *
 * Two ABI details, both observed rather than assumed. GetPixelFormat returns an
 * eight-byte struct, and MSVC compiled its caller to pass a buffer and use the
 * returned pointer -- so it takes a hidden out-pointer and returns it, which is
 * what the disassembly of the caller requires:
 *
 *     puVar2 = (*(code **)(*rt + 400))(rt, local_res8);
 *     local_18 = *puVar2;
 *
 * A stub returning 0 there is not a missing pixel format, it is a null
 * dereference two instructions later. GetSize and GetPixelSize are the same
 * shape for the same reason. */

/* DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED: what the window
 * surface actually is. */
static MS void *d2_GetPixelFormat(void *self, uint32_t *out)
{
    (void)self;
    if (out) { out[0] = 87; out[1] = 1; }
    return out;
}
static MS void *d2_GetSize(void *self, float *out)
{
    w32_surf *s = d2_window_surface();
    (void)self;
    if (out) { out[0] = s ? (float)s->w : 0.0f; out[1] = s ? (float)s->h : 0.0f; }
    return out;
}
static MS void *d2_GetPixelSize(void *self, uint32_t *out)
{
    w32_surf *s = d2_window_surface();
    (void)self;
    if (out) { out[0] = s ? (uint32_t)s->w : 0u; out[1] = s ? (uint32_t)s->h : 0u; }
    return out;
}

static MS void *d2_bmp_GetSize(void *self, float *out)
{
    dwprobe *o = self;
    gp_image *im = o ? o->ctx : NULL;
    if (out) { out[0] = im ? (float)im->w : 0.0f; out[1] = im ? (float)im->h : 0.0f; }
    return out;
}
static MS void *d2_bmp_GetPixelSize(void *self, uint32_t *out)
{
    dwprobe *o = self;
    gp_image *im = o ? o->ctx : NULL;
    if (out) { out[0] = im ? (uint32_t)im->w : 0u; out[1] = im ? (uint32_t)im->h : 0u; }
    return out;
}
static MS void *d2_bmp_GetDpi(void *self, float *out)
{ (void)self; if (out) { out[0] = 96.0f; out[1] = 96.0f; } return out; }

static dwprobe *d2_bitmap_wrap(gp_image *im)
{
    dwprobe *o = dwp_new("ID2D1Bitmap");
    if (!o) return NULL;
    o->ctx = im;
    dwp_set(o, 0, (void *)dxgi_qi_self);
    dwp_set(o, 1, (void *)d3d_addref);
    dwp_set(o, 2, (void *)d3d_release);
    dwp_set(o, 4, (void *)d2_bmp_GetSize);
    dwp_set(o, 5, (void *)d2_bmp_GetPixelSize);
    dwp_set(o, 6, (void *)d2_GetPixelFormat);
    dwp_set(o, 7, (void *)d2_bmp_GetDpi);
    return o;
}

/* ID2D1RenderTarget::CreateBitmap(D2D1_SIZE_U, const void *src, UINT pitch,
 *                                 const D2D1_BITMAP_PROPERTIES *, ID2D1Bitmap **)
 * The size is an eight-byte struct passed by value, which lands in one
 * register: two packed 32-bit unsigneds. */
static MS int32_t d2_CreateBitmap(void *self, uint64_t size, const void *src,
                                  uint32_t pitch, const void *props, void **out)
{
    gp_image *im;
    uint32_t w = (uint32_t)(size & 0xFFFFFFFFu), h = (uint32_t)(size >> 32);
    uint32_t y;

    (void)self; (void)props;
    if (!out) return D3D_NOTIMPL;
    if (!w || !h || w > 16384 || h > 16384) return D3D_FAIL;
    if (!(im = (gp_image *)calloc(1, sizeof *im))) return D3D_FAIL;
    im->tag = GPO_IMAGE; im->w = (int)w; im->h = (int)h; im->owns = 1;
    if (!(im->px = (uint32_t *)calloc((size_t)w * h, 4))) { free(im); return D3D_FAIL; }
    if (src) {
        if (!pitch) pitch = w * 4;
        for (y = 0; y < h; y++)
            memcpy(im->px + (size_t)y * w, (const uint8_t *)src + (size_t)y * pitch, w * 4);
    }
    PLOG("  [d2d] CreateBitmap %ux%u\n", w, h);
    *out = d2_bitmap_wrap(im);
    if (!*out) { free(im->px); free(im); return D3D_FAIL; }
    return D3D_OK;
}

/* The one the skin goes through: a decoded WIC image becomes a D2D bitmap. */
static MS int32_t d2_CreateBitmapFromWicBitmap(void *self, void *wicsrc,
                                               const void *props, void **out)
{
    dwprobe *w = wicsrc;
    wic_image *src = w ? (wic_image *)w->ctx : NULL;
    gp_image *im;

    (void)self; (void)props;
    if (!out) return D3D_NOTIMPL;
    if (!src || !src->px || src->w <= 0 || src->h <= 0) return D3D_FAIL;
    if (!(im = (gp_image *)calloc(1, sizeof *im))) return D3D_FAIL;
    im->tag = GPO_IMAGE; im->w = src->w; im->h = src->h; im->owns = 1;
    if (!(im->px = (uint32_t *)malloc((size_t)src->w * src->h * 4))) {
        free(im); return D3D_FAIL;
    }
    /* Copied rather than aliased: the caller is entitled to release the WIC
     * bitmap the moment this returns, and a skin that then draws from freed
     * pixels is a picture that is right the first time and garbage afterwards. */
    memcpy(im->px, src->px, (size_t)src->w * src->h * 4);
    PLOG("  [d2d] CreateBitmapFromWicBitmap %dx%d\n", im->w, im->h);
    *out = d2_bitmap_wrap(im);
    if (!*out) { free(im->px); free(im); return D3D_FAIL; }
    return D3D_OK;
}

/* DrawBitmap(bitmap, destRect, opacity, interpolation, srcRect). The opacity is
 * the fourth argument and a float, so it arrives in XMM3 -- writing the
 * signature out in this order is what puts it there. */
static MS int32_t d2_DrawBitmap(void *self, void *bitmap, const float *dest,
                                float opacity, uint32_t interp, const float *src)
{
    gp_graphics *g = d2_gfx(self);
    dwprobe *o = bitmap;
    gp_image *im = o ? (gp_image *)o->ctx : NULL;
    float dl, dt, dr, db, sl, st, sr, sb;

    (void)interp;
    if (!g || !im) return D3D_NOTIMPL;
    if (src) { sl = src[0]; st = src[1]; sr = src[2]; sb = src[3]; }
    else     { sl = 0; st = 0; sr = (float)im->w; sb = (float)im->h; }
    if (dest) { dl = dest[0]; dt = dest[1]; dr = dest[2]; db = dest[3]; }
    else      { dl = 0; dt = 0; dr = sr - sl; db = sb - st; }
    if (dr <= dl || db <= dt || sr <= sl || sb <= st) return D3D_OK;
    /* Opacity below a whole pixel's worth is not worth a pass over the bitmap;
     * above it, the alpha row of a colour matrix is what GDI+ honours here. */
    if (opacity < 0.004f) return D3D_OK;
    {
        gp_imageattr ia;
        memset(&ia, 0, sizeof ia);
        ia.tag = GPO_IMAGEATTR;
        if (opacity < 0.996f) {
            int i;
            for (i = 0; i < 5; i++) ia.m[i][i] = 1.0f;
            ia.m[3][3] = opacity;
            ia.has_matrix = 1;
        }
        st_GdipDrawImageRectRectI(g, im, (int32_t)dl, (int32_t)dt,
                                  (int32_t)(dr - dl), (int32_t)(db - dt),
                                  (int32_t)sl, (int32_t)st,
                                  (int32_t)(sr - sl), (int32_t)(sb - st),
                                  2, ia.has_matrix ? &ia : NULL, NULL, NULL);
    }
    return D3D_OK;
}

/* Clipping. A rectangle pushed here bounds every later call until it is popped;
 * the rasteriser takes one clip rectangle, so they nest by intersection. */
/* Clipping, and it is not decoration.
 *
 * A plug-in repainting one knob pushes that knob's rectangle, redraws its whole
 * background bitmap under it, and then draws only the controls the rectangle
 * touches -- everything else is still on the window from the last frame, which
 * is how Windows works. Recording the rectangle and then ignoring it let that
 * background bitmap cover the entire editor, so adjusting one knob wiped every
 * other control off the panel and left the bare backdrop behind.
 *
 * Nested pushes intersect, and the stack remembers what was in force so a pop
 * puts it back. */
static MS int32_t d2_PushAxisAlignedClip(void *self, const float *rc, uint32_t mode)
{
    d2_clip c;
    float x0, y0, x1, y1, t;
    (void)self; (void)mode;
    if (g_d2_clipn >= D2_CLIP_DEPTH - 1 || !rc) return D3D_OK;
    d2_xf_point(rc[0], rc[1], &x0, &y0);
    d2_xf_point(rc[2], rc[3], &x1, &y1);
    if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
    c = g_d2_clip[g_d2_clipn];
    if (c.has) {                                   /* intersect with the outer one */
        if (x0 < c.x0) x0 = c.x0;
        if (y0 < c.y0) y0 = c.y0;
        if (x1 > c.x1) x1 = c.x1;
        if (y1 > c.y1) y1 = c.y1;
    }
    g_d2_clipn++;
    g_d2_clip[g_d2_clipn].has = 1;
    g_d2_clip[g_d2_clipn].x0 = x0;
    g_d2_clip[g_d2_clipn].y0 = y0;
    g_d2_clip[g_d2_clipn].x1 = x1;
    g_d2_clip[g_d2_clipn].y1 = y1;
    return D3D_OK;
}
static MS int32_t d2_PopAxisAlignedClip(void *self)
{ (void)self; if (g_d2_clipn > 0) g_d2_clipn--; return D3D_OK; }

static MS int32_t d2_Flush(void *self, void *tag1, void *tag2)
{ (void)self; (void)tag1; (void)tag2; return D3D_OK; }
static MS int32_t d2_SetAntialiasMode(void *self, uint32_t m)
{ (void)self; (void)m; return D3D_OK; }
static MS uint32_t d2_GetAntialiasMode(void *self) { (void)self; return 0; }

static dwprobe *d2d_context(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("ID2D1DeviceContext")) != NULL) {
        dwp_set(o, 0,  (void *)dxgi_qi_self);
        dwp_set(o, 1,  (void *)d3d_addref);
        dwp_set(o, 2,  (void *)d3d_release);
        dwp_set(o, 4,  (void *)d2_CreateBitmap);
        dwp_set(o, 5,  (void *)d2_CreateBitmapFromWicBitmap);
        dwp_set(o, 8,  (void *)d2_CreateSolidColorBrush);
        dwp_set(o, 17, (void *)d2_FillRectangle);
        dwp_set(o, 26, (void *)d2_DrawBitmap);
        dwp_set(o, 32, (void *)d2_SetAntialiasMode);
        dwp_set(o, 33, (void *)d2_GetAntialiasMode);
        dwp_set(o, 42, (void *)d2_Flush);
        dwp_set(o, 45, (void *)d2_PushAxisAlignedClip);
        dwp_set(o, 46, (void *)d2_PopAxisAlignedClip);
        dwp_set(o, 50, (void *)d2_GetPixelFormat);
        dwp_set(o, 53, (void *)d2_GetSize);
        dwp_set(o, 54, (void *)d2_GetPixelSize);
        dwp_set(o, 27, (void *)d2_DrawText);
        dwp_set(o, 30, (void *)d2_SetTransform);
        dwp_set(o, 31, (void *)d2_GetTransform);
        dwp_set(o, 47, (void *)d2_Clear);
        dwp_set(o, 48, (void *)d2_BeginDraw);
        dwp_set(o, 49, (void *)d2_EndDraw);
        dwp_set(o, 51, (void *)d2_SetDpi);
        dwp_set(o, 52, (void *)d2_GetDpi);
        dwp_set(o, 62, (void *)d2_CreateBitmapFromDxgiSurface);
        dwp_set(o, 74, (void *)d2_SetTarget);
    }
    return o;
}

static MS int32_t d2d_dev_CreateDeviceContext(void *self, uint32_t opts, void **out)
{
    (void)self; (void)opts;
    PLOG("  [d2d] ID2D1Device::CreateDeviceContext\n");
    if (!out) return D3D_NOTIMPL;
    *out = d2d_context();
    return *out ? D3D_OK : D3D_FAIL;
}
static dwprobe *d2d_device(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("ID2D1Device")) != NULL) {
        dwp_set(o, 0, (void *)dxgi_qi_self);
        dwp_set(o, 1, (void *)d3d_addref);
        dwp_set(o, 2, (void *)d3d_release);
        dwp_set(o, 4, (void *)d2d_dev_CreateDeviceContext);
    }
    return o;
}
static MS int32_t d2d_fac_CreateDevice(void *self, void *dxgidev, void **out)
{
    (void)self; (void)dxgidev;
    PLOG("  [d2d] ID2D1Factory1::CreateDevice\n");
    if (!out) return D3D_NOTIMPL;
    *out = d2d_device();
    return *out ? D3D_OK : D3D_FAIL;
}
static MS void d2d_fac_GetDesktopDpi(void *self, float *x, float *y)
{ (void)self; if (x) *x = 96.0f; if (y) *y = 96.0f; }

static dwprobe *d2d_factory_obj(void)
{
    static dwprobe *o;
    if (!o && (o = dwp_new("ID2D1Factory1")) != NULL) {
        dwp_set(o, 0,  (void *)dxgi_qi_self);
        dwp_set(o, 1,  (void *)d3d_addref);
        dwp_set(o, 2,  (void *)d3d_release);
        dwp_set(o, 4,  (void *)d2d_fac_GetDesktopDpi);
        dwp_set(o, 17, (void *)d2d_fac_CreateDevice);
    }
    return o;
}

/* D2D1CreateFactory(type, riid, options, void **factory) -- imported by
 * ordinal 1, which is how d2d1.dll exports it. */
static MS int32_t st_D2D1CreateFactory(uint32_t type, const uint8_t *iid,
                                       const void *opts, void **out)
{
    char b[64];
    (void)type; (void)opts;
    PLOG("  [d2d] D2D1CreateFactory(%s)\n", d3d_guid(iid, b, sizeof b));
    if (!out) return D3D_NOTIMPL;
    *out = d2d_factory_obj();
    return *out ? D3D_OK : D3D_FAIL;
}

#endif /* PELOAD_D3D_SHIM_H */
