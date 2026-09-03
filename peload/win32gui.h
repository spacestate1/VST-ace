/* A minimal Win32 window/GDI layer, enough to display a Windows plugin's editor.
 *
 * Why this is tractable rather than "reimplement Wine": the import tables of
 * every plugin here show StretchDIBits and BitBlt with no OpenGL, Direct3D,
 * Direct2D or DirectWrite. iPlug2's Skia backend is therefore CPU raster -- the
 * plugin composites its whole interface into a memory bitmap and pushes it
 * through one call. So this file does not draw anything. It provides:
 *
 *   - window objects with a WndProc, and a message queue the host pumps
 *   - device contexts backed by plain 32-bit pixel buffers
 *   - StretchDIBits and BitBlt, the only two operations that move pixels
 *   - timers and synthesised mouse/keyboard messages
 *
 * Everything else (brushes, pens, regions, fonts) hands back plausible handles
 * because Skia never asks them to draw; text is rasterised inside the plugin
 * from font bytes it gets via GetFontData.
 *
 * Coordinates: a plugin editor is a child window filling its parent, so client
 * and window rects are the same and the origin is (0,0). That removes the whole
 * non-client area from the picture.
 *
 * Single-threaded by design: the host pumps from the GUI thread only. */
#ifndef PELOAD_WIN32GUI_H
#define PELOAD_WIN32GUI_H

/* ---- host-facing API (see win32host.h for the declarations the GUI uses) -- */

#define W32_MAX_WND   64
#define W32_MAX_DC  1024
#define W32_MAX_OBJ  8192
#define W32_MAX_CLS   64
#define W32_MSGQ     512
#define W32_MAX_TIMER 32

#define W32_HWND_BASE 0x00110000u
#define W32_HDC_BASE  0x00220000u
#define W32_HOOK_BASE 0x00660000u
#define W32_MAX_HOOK  16
#define W32_OBJ_BASE  0x00330000u

typedef struct { int32_t left, top, right, bottom; } W32RECT;
typedef struct { int32_t x, y; } W32POINT;

typedef struct {
    uint32_t biSize; int32_t biWidth, biHeight;
    uint16_t biPlanes, biBitCount;
    uint32_t biCompression, biSizeImage;
    int32_t  biXPelsPerMeter, biYPelsPerMeter;
    uint32_t biClrUsed, biClrImportant;
} W32BITMAPINFOHEADER;

typedef struct {
    void    *hdc;
    int32_t  fErase;
    W32RECT  rcPaint;
    int32_t  fRestore, fIncUpdate;
    uint8_t  rgbReserved[32];
} W32PAINTSTRUCT;

/* WPARAM, LPARAM and LRESULT are all pointer-sized: 4 bytes on Win32, 8 on
 * Win64. Fixing them at 64 bits made every WndProc call push 8 bytes too many
 * per parameter on i386, and a stdcall window procedure pops only what its own
 * signature says -- so the caller's stack walked 8 bytes further off on every
 * message until some later `pop`/`ret` picked up a wrong word. */
typedef uintptr_t W_WPARAM;
typedef intptr_t  W_LPARAM;
typedef intptr_t  W_LRESULT;

typedef struct {
    void    *hwnd;
    uint32_t message;
    W_WPARAM wParam;
    W_LPARAM lParam;
    uint32_t time;
    W32POINT pt;
    uint32_t lPrivate;
} W32MSG;

typedef struct {
    uint32_t style;
    void    *lpfnWndProc;
    int32_t  cbClsExtra, cbWndExtra;
    void    *hInstance, *hIcon, *hCursor, *hbrBackground;
    const char *lpszMenuName, *lpszClassName;
} W32WNDCLASSA;

/* messages */
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_NCCREATE 0x0081
#define WM_NCCALCSIZE 0x0083
#define WM_TIMER 0x0113
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_MOUSELEAVE 0x02A3

#define GWLP_WNDPROC   (-4)
#define GWL_STYLE     (-16)
#define GWL_EXSTYLE   (-20)
#define GWLP_USERDATA (-21)

/* Glyphs for the GDI text calls, from the same face. Declared with plain types
 * so FreeType's headers stay on that side of the wall. */
static int  dw_text_measure(const char *s, int n, int em_px,
                            int *w, int *h, int *ascent);
static int  dw_text_draw(uint32_t *px, int pw, int ph, int x, int baseline,
                         const char *s, int n, int em_px, uint32_t rgb,
                         const int32_t *clip4);

typedef struct { int w, h; uint32_t *px; } w32_surf;

typedef struct {
    int      used;
    void    *wndproc;              /* MS-ABI LRESULT(HWND,UINT,WPARAM,LPARAM) */
    W_LRESULT userdata;
    int32_t  style, exstyle;
    int      x, y, w, h;
    int      visible;
    int      parent;               /* window index, 0 = none */
    char     cls[64];
    /* Window text, and the little state a built-in control keeps. A plug-in
     * whose editor is standard controls asks for these back through
     * WM_GETTEXT, BM_GETCHECK and CB_GETCURSEL, and draws nothing sensible if
     * they do not remember what it set. */
    char     text[128];
    int      ctl_check;            /* BM_SETCHECK, or the current selection */
    char   (*items)[64];           /* combo box and list box contents */
    int      nitems;
    int      enabled;
    w32_surf surf;                 /* the client pixels the host displays */
    W32RECT  update;
    int      has_update;
} w32_wnd;

enum { OBJ_NONE = 0, OBJ_BITMAP, OBJ_BRUSH, OBJ_PEN, OBJ_FONT, OBJ_RGN };

typedef struct {
    int       used, kind;
    int       w, h;
    uint32_t *px;                  /* bitmaps */
    /* A DIB section additionally hands the plugin a pointer to its own pixels,
     * in the layout it asked for. When that layout is one we can read directly
     * -- 32 bits per pixel, top-down -- `dib` and `px` are the same allocation
     * and there is nothing to keep in step. Otherwise `dib` is what the plugin
     * writes and `px` is what we draw and blit, converted on demand. */
    uint8_t  *dib;
    int       dib_bpp, dib_stride, dib_flip, dib_alias, dib_565;
    int       px_drawn;            /* GDI has written this bitmap */
    int       mono;                /* 1 bit per pixel: a mask, not a picture */
    uint32_t  dib_pal[256];
    int       dib_pal_n;
    uint32_t  color;               /* brushes and pens */
    W32RECT   rc;                  /* regions */
    uint8_t   logfont[92];         /* LOGFONTW as handed to CreateFontIndirect */
    int       logfont_len;
} w32_obj;

/* GetUpdateRgn / GetRgnBox return codes. Returning NULLREGION is how the editor
 * silently declined to draw: iPlug2 treats it as "nothing is invalid". */
#define W32_RGN_ERROR   0
#define W32_RGN_NULL    1
#define W32_RGN_SIMPLE  2
#define W32_RGN_COMPLEX 3

typedef struct {
    int used;
    int wnd;                       /* window index, 0 for a memory DC */
    int bitmap;                    /* selected bitmap object, 0 if none */
    int font;                      /* selected font object, 0 if none */
    int in_paint;
    /* The viewport origin, which drawing is relative to. MFC moves it rather
     * than offsetting its own coordinates -- OnPaint on a scrolled or inset
     * view sets it once and then draws in its own space -- so ignoring it does
     * not lose a translation, it puts every element in the wrong place. */
    int32_t org_x, org_y;
    /* The clip box, and whether one has been set. Tracked mainly so RectVisible
     * can answer honestly: see st_RectVisible. */
    W32RECT clip;
    int     has_clip;
    /* The rest of the drawing state. This layer was written for plug-ins that
     * rasterise their own interface and blit it, so none of it was here: an
     * editor that draws with GDI selects a pen and a brush, sets a text colour
     * and expects them to be remembered. */
    int      pen, brush;           /* selected objects, 0 = stock */
    uint32_t text_color, bk_color;
    int      bk_mode;              /* 1 TRANSPARENT, 2 OPAQUE */
    uint32_t text_align;
    int32_t  cur_x, cur_y;         /* current position, for LineTo */
    int      inited;
} w32_dc;

typedef struct { int used; int wnd; uintptr_t id; uint32_t ms; double next; void *proc; } w32_timer;

static struct {
    w32_wnd   wnd[W32_MAX_WND];
    w32_dc    dc[W32_MAX_DC];
    w32_obj   obj[W32_MAX_OBJ];
    w32_timer timer[W32_MAX_TIMER];
    struct { int used; char name[64]; void *proc; } cls[W32_MAX_CLS];
    struct { int used, id; void *proc; } hook[W32_MAX_HOOK];
    W32MSG    q[W32_MSGQ];
    int       qhead, qtail;
    int       capture, focus;
    int       mouse_x, mouse_y;
    uint32_t  keys[256];
    int       host;                /* the container we create for the plugin  */
    int       display;             /* the plugin's own window, what we present */
    w32_host_hooks hooks;
    long n_paint, n_getdc, n_beginpaint, n_stretch, n_bitblt, n_timerproc;
    double last_input_ms;          /* when input last arrived, for the valve */
} W;

static double w32_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Give the host a chance to deliver input first: a plugin polling these is very
 * likely spinning in a drag loop, and stale answers mean it spins forever. */
static void w32_pump_input(void)
{ if (W.hooks.pump_input) W.hooks.pump_input(W.hooks.ud); }

/* ---- handle helpers ------------------------------------------------------ */

/* Handles are an index in a fixed window per object type, spaced sixteen apart.
 *
 * The spacing is not decoration. A consumer hashes a handle to find its own
 * wrapper for it, and MFC's CHandleMap -- which every MFC-era editor goes
 * through on each SelectObject -- hashes with `key >> 4`. Consecutive handles
 * differ only in the bits that shift discards, so every object this host handed
 * out landed in one bucket. Real Windows handles vary in the bits above the
 * bottom nibble; matching that costs four bits of index space and nothing else. */
#define W32_H_SHIFT  4
#define W32_H_WINDOW 0x20000u

static void *w32_h(uint32_t base, int idx)
{ return (void *)(uintptr_t)(base + ((uint32_t)idx << W32_H_SHIFT)); }
static int   w32_i(uint32_t base, const void *h)
{
    uintptr_t v = (uintptr_t)h;
    if (v < base || v >= base + W32_H_WINDOW) return 0;
    v -= base;
    /* A value inside the window that is not on a boundary is not one of ours. */
    if (v & ((1u << W32_H_SHIFT) - 1)) return 0;
    return (int)(v >> W32_H_SHIFT);
}
static w32_wnd *w32_wget(const void *h)
{
    int i = w32_i(W32_HWND_BASE, h);
    return (i > 0 && i < W32_MAX_WND && W.wnd[i].used) ? &W.wnd[i] : NULL;
}
/* Defined below; a DC needs its stock objects the first time it is looked up. */
static MS void *st_GetStockObject(int32_t i);

static w32_dc *w32_dcget(const void *h)
{
    int i = w32_i(W32_HDC_BASE, h);
    w32_dc *d = (i > 0 && i < W32_MAX_DC && W.dc[i].used) ? &W.dc[i] : NULL;
    /* Windows hands out a DC with black text on a white opaque background, and
     * a plug-in that never sets them is entitled to those. A zeroed struct
     * means white-on-black with a transparent background instead, which draws
     * invisible text and is a hard bug to see. */
    if (d && !d->inited) {
        d->inited = 1;
        d->text_color = 0x000000u;
        d->bk_color   = 0xFFFFFFu;
        d->bk_mode    = 2;                       /* OPAQUE */
        /* Windows has a stock pen, brush and font selected from the start, so
         * SelectObject always has a previous object to return. Answering NULL
         * instead hands a caller that wraps the result -- MFC does, on every
         * SelectObject -- a null where a handle belongs. */
        d->pen   = w32_i(W32_OBJ_BASE, st_GetStockObject(7));   /* BLACK_PEN */
        d->brush = w32_i(W32_OBJ_BASE, st_GetStockObject(0));   /* WHITE_BRUSH */
        d->font  = w32_i(W32_OBJ_BASE, st_GetStockObject(13));  /* SYSTEM_FONT */
    }
    return d;
}
static w32_obj *w32_oget(const void *h)
{
    int i = w32_i(W32_OBJ_BASE, h);
    return (i > 0 && i < W32_MAX_OBJ && W.obj[i].used) ? &W.obj[i] : NULL;
}

static int w32_obj_new(int kind, int w, int h, uint32_t color)
{
    int i;
    for (i = 1; i < W32_MAX_OBJ; i++) {
        if (W.obj[i].used) continue;
        memset(&W.obj[i], 0, sizeof W.obj[i]);
        W.obj[i].used = 1;
        W.obj[i].kind = kind;
        W.obj[i].color = color;
        if (kind == OBJ_BITMAP && w > 0 && h > 0) {
            W.obj[i].w = w; W.obj[i].h = h;
            W.obj[i].px = calloc((size_t)w * h, 4);
            if (!W.obj[i].px) { W.obj[i].used = 0; return 0; }
        }
        return i;
    }
    return 0;
}

/* Decode a packed DIB (an RT_BITMAP resource) into one of our bitmaps.
 *
 * Handles the depths these plugins actually ship: 1, 4, 8 (palettised), 16, 24
 * and 32 bits per pixel, uncompressed. Rows are bottom-up unless the height is
 * negative, and each row is padded to a four-byte boundary -- both are easy to
 * get wrong and produce a picture that is recognisably right and subtly sheared.
 */
void *w32_bitmap_from_dib(const uint8_t *dib, uint32_t len)
{
    uint32_t hdr, pal_entries, pal_off, row_bytes;
    int32_t w, h, y, x, flip = 1;
    uint16_t bpp;
    uint32_t compression;
    const uint8_t *pal, *rows;
    int idx;
    w32_obj *o;

    if (!dib || len < 40) return NULL;
    memcpy(&hdr, dib, 4);
    if (hdr < 40 || hdr > len) return NULL;         /* BITMAPCOREHEADER: not seen */
    memcpy(&w, dib + 4, 4);
    memcpy(&h, dib + 8, 4);
    memcpy(&bpp, dib + 14, 2);
    memcpy(&compression, dib + 16, 4);
    if (compression != 0) return NULL;              /* BI_RGB only */
    if (h < 0) { h = -h; flip = 0; }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;

    memcpy(&pal_entries, dib + 32, 4);              /* biClrUsed */
    if (!pal_entries && bpp <= 8) pal_entries = 1u << bpp;
    pal_off = hdr;
    pal = dib + pal_off;
    rows = pal + pal_entries * 4;
    row_bytes = ((uint32_t)w * bpp + 31) / 32 * 4;
    if ((uint32_t)(rows - dib) + row_bytes * (uint32_t)h > len) return NULL;

    if (getenv("PELOAD_DIB"))
        fprintf(stderr, "  [dib] %dx%d bpp=%u %s\n", w, h, bpp,
                flip ? "bottom-up" : "top-down");
    if (!(idx = w32_obj_new(OBJ_BITMAP, w, h, 0))) return NULL;
    o = &W.obj[idx];

    for (y = 0; y < h; y++) {
        const uint8_t *src = rows + (uint32_t)(flip ? h - 1 - y : y) * row_bytes;
        uint32_t *dst = o->px + (size_t)y * w;
        for (x = 0; x < w; x++) {
            uint32_t p;
            switch (bpp) {
            case 1:  p = (uint32_t)((src[x >> 3] >> (7 - (x & 7))) & 1); goto palette;
            case 4:  p = (uint32_t)((x & 1) ? (src[x >> 1] & 0xf) : (src[x >> 1] >> 4));
                     goto palette;
            case 8:  p = src[x]; goto palette;
            palette:
                if (p >= pal_entries) p = 0;
                /* Palette entries are stored B,G,R,reserved. */
                dst[x] = ((uint32_t)pal[p * 4 + 2] << 16) |
                         ((uint32_t)pal[p * 4 + 1] << 8) | pal[p * 4 + 0];
                break;
            case 16: {
                uint16_t v;
                memcpy(&v, src + x * 2, 2);
                /* Default 16-bit DIB is 5-5-5, high bit unused. */
                dst[x] = ((uint32_t)(((v >> 10) & 0x1f) * 255 / 31) << 16) |
                         ((uint32_t)(((v >> 5)  & 0x1f) * 255 / 31) << 8)  |
                          (uint32_t)((  v        & 0x1f) * 255 / 31);
                break; }
            case 24:
                dst[x] = ((uint32_t)src[x * 3 + 2] << 16) |
                         ((uint32_t)src[x * 3 + 1] << 8) | src[x * 3 + 0];
                break;
            case 32:
                memcpy(&dst[x], src + x * 4, 4);
                dst[x] &= 0x00ffffffu;              /* the alpha byte is unused */
                break;
            default:
                return NULL;
            }
        }
    }
    return w32_h(W32_OBJ_BASE, idx);
}

/* Where does drawing through this DC land? */
/* Where drawing through this DC lands. The caller supplies the scratch, because
 * a single static here silently aliases: BitBlt resolves two DCs, and when both
 * had a bitmap selected the source and destination became the same struct -- the
 * blit copied a bitmap onto itself and the editor stayed black. */
/* Keeping a DIB section and our own surface in step, in whichever direction the
 * plugin is actually using it.
 *
 * Only the layouts we cannot alias need this at all -- 32bpp top-down is one
 * buffer and always correct. For the rest there are two writers and they cannot
 * both be authoritative:
 *
 *   - the plugin writes raw pixels into the pointer CreateDIBSection returned,
 *     which is the entire reason that call exists. We must read them.
 *   - the plugin draws into the same bitmap with GDI, through a memory DC.
 *     Then our surface holds the drawing and the DIB buffer is stale.
 *
 * Syncing unconditionally on every access gets the second case exactly wrong:
 * it copies the untouched DIB over the drawing, so each call erases the one
 * before it and the editor comes out black with every call reporting success.
 * That is what the first pass here did.
 *
 * So the direction follows the role. A blit source pulls, unless GDI has drawn
 * into it -- in which case our surface is the newer of the two. A drawing target
 * records that GDI has written it, and pushes back when the bitmap leaves its DC,
 * which is when an application that means to read the bits goes to look. A
 * plugin that interleaves both, drawing first and then writing raw pixels, would
 * need a page-protection trick to detect; nothing here does it. */
static void w32_dib_pull(w32_obj *o)
{
    int y, x;

    if (!o || !o->dib || o->dib_alias || !o->px) return;
    for (y = 0; y < o->h; y++) {
        const uint8_t *src = o->dib +
            (size_t)(o->dib_flip ? o->h - 1 - y : y) * o->dib_stride;
        uint32_t *dst = o->px + (size_t)y * o->w;
        for (x = 0; x < o->w; x++) {
            switch (o->dib_bpp) {
            case 32: memcpy(&dst[x], src + x * 4, 4); dst[x] &= 0x00ffffffu; break;
            case 24: dst[x] = ((uint32_t)src[x * 3 + 2] << 16) |
                              ((uint32_t)src[x * 3 + 1] << 8) | src[x * 3 + 0]; break;
            case 16: { uint16_t v; memcpy(&v, src + x * 2, 2);
                       dst[x] = o->dib_565
                         ? (((uint32_t)(((v >> 11) & 0x1f) * 255 / 31) << 16) |
                            ((uint32_t)(((v >>  5) & 0x3f) * 255 / 63) << 8)  |
                             (uint32_t)((  v        & 0x1f) * 255 / 31))
                         : (((uint32_t)(((v >> 10) & 0x1f) * 255 / 31) << 16) |
                            ((uint32_t)(((v >>  5) & 0x1f) * 255 / 31) << 8)  |
                             (uint32_t)((  v        & 0x1f) * 255 / 31)); break; }
            case 8:  dst[x] = o->dib_pal[src[x]]; break;
            case 4:  dst[x] = o->dib_pal[(x & 1) ? (src[x >> 1] & 0xf)
                                                 : (src[x >> 1] >> 4)]; break;
            case 1:  dst[x] = o->dib_pal[(src[x >> 3] >> (7 - (x & 7))) & 1]; break;
            default: return;
            }
        }
    }
}

/* The other direction: what GDI drew, in the layout the plugin asked for. */
static void w32_dib_push(w32_obj *o)
{
    int y, x;

    if (!o || !o->dib || o->dib_alias || !o->px || !o->px_drawn) return;
    for (y = 0; y < o->h; y++) {
        uint8_t *dst = o->dib +
            (size_t)(o->dib_flip ? o->h - 1 - y : y) * o->dib_stride;
        const uint32_t *src = o->px + (size_t)y * o->w;
        for (x = 0; x < o->w; x++) {
            switch (o->dib_bpp) {
            case 32: { uint32_t v = src[x]; memcpy(dst + x * 4, &v, 4); break; }
            case 24: dst[x * 3 + 0] = (uint8_t)(src[x]);
                     dst[x * 3 + 1] = (uint8_t)(src[x] >> 8);
                     dst[x * 3 + 2] = (uint8_t)(src[x] >> 16); break;
            case 16: { uint32_t v = src[x];
                       uint16_t p = o->dib_565
                         ? (uint16_t)((((v >> 16) & 0xff) >> 3 << 11) |
                                      (((v >> 8)  & 0xff) >> 2 << 5)  |
                                       ((v        & 0xff) >> 3))
                         : (uint16_t)((((v >> 16) & 0xff) >> 3 << 10) |
                                      (((v >> 8)  & 0xff) >> 3 << 5)  |
                                       ((v        & 0xff) >> 3));
                       memcpy(dst + x * 2, &p, 2); break; }
            default: return;            /* palettised: no reverse mapping */
            }
        }
    }
}

/* Declared here because BitBlt needs them before the drawing section defines
 * them: the colour-order conversion, and the pattern the pattern rops use. */
static uint32_t w32_cr(uint32_t colorref);
static int      w32_brush_on(const w32_dc *d);
static uint32_t w32_brush_rgb(const w32_dc *d);

static w32_surf *w32_target_in_raw(w32_dc *d, w32_surf *tmp)
{
    if (!d) return NULL;
    if (d->bitmap) {
        w32_obj *o = &W.obj[d->bitmap];
        tmp->w = o->w; tmp->h = o->h; tmp->px = o->px;
        return tmp;
    }
    if (d->wnd && W.wnd[d->wnd].used) return &W.wnd[d->wnd].surf;
    return NULL;
}

/* Where drawing through this DC lands. Records that GDI has written the bitmap,
 * so a later blit from it does not pull the plugin's staler buffer over it. */
static w32_surf *w32_target_in(w32_dc *d, w32_surf *tmp)
{
    if (d && d->bitmap) W.obj[d->bitmap].px_drawn = 1;
    return w32_target_in_raw(d, tmp);
}

/* Push back only the rows a call touched. A full push is 1.7 MB for a panel-
 * sized DIB and the blits come in the dozens, so the rectangle matters. */
static void w32_dib_push_rect(w32_obj *o, int x0, int y0, int x1, int y1)
{
    int y, x;

    if (!o || !o->dib || o->dib_alias || !o->px) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > o->w) x1 = o->w;
    if (y1 > o->h) y1 = o->h;
    for (y = y0; y < y1; y++) {
        uint8_t *dst = o->dib +
            (size_t)(o->dib_flip ? o->h - 1 - y : y) * o->dib_stride;
        const uint32_t *src = o->px + (size_t)y * o->w;
        for (x = x0; x < x1; x++) {
            switch (o->dib_bpp) {
            case 32: { uint32_t v = src[x]; memcpy(dst + x * 4, &v, 4); break; }
            case 24: dst[x * 3 + 0] = (uint8_t)(src[x]);
                     dst[x * 3 + 1] = (uint8_t)(src[x] >> 8);
                     dst[x * 3 + 2] = (uint8_t)(src[x] >> 16); break;
            case 16: { uint32_t v = src[x];
                       uint16_t p = o->dib_565
                         ? (uint16_t)((((v >> 16) & 0xff) >> 3 << 11) |
                                      (((v >> 8)  & 0xff) >> 2 << 5)  |
                                       ((v        & 0xff) >> 3))
                         : (uint16_t)((((v >> 16) & 0xff) >> 3 << 10) |
                                      (((v >> 8)  & 0xff) >> 3 << 5)  |
                                       ((v        & 0xff) >> 3));
                       memcpy(dst + x * 2, &p, 2); break; }
            default: return;
            }
        }
    }
}

/* Called by every primitive that writes through a DC, so a DIB section the
 * plug-in also writes directly is current the moment the call returns.
 *
 * Without it the two buffers take turns being right and the picture is
 * whichever won last: a plug-in that composes its interface with BitBlt into a
 * DIB and then blits that DIB onward gets its work overwritten by the pull on
 * the way out, and two controls out of forty survive. */
static void w32_dib_out(w32_dc *d, int x0, int y0, int x1, int y1)
{
    if (!d || !d->bitmap) return;
    w32_dib_push_rect(&W.obj[d->bitmap], x0, y0, x1, y1);
}
static void w32_dib_out_all(w32_dc *d)
{
    if (!d || !d->bitmap) return;
    w32_dib_push_rect(&W.obj[d->bitmap], 0, 0,
                      W.obj[d->bitmap].w, W.obj[d->bitmap].h);
}

/* A blit source. Pulls the plugin's own pixels in when they are the newer of
 * the two; see w32_dib_push for why that is conditional. */
static w32_surf *w32_source_in(w32_dc *d, w32_surf *tmp)
{
    if (d && d->bitmap) w32_dib_pull(&W.obj[d->bitmap]);
    return w32_target_in_raw(d, tmp);
}

/* A window's pixels. Under PELOAD_GUARD_HEAP these sit against an unmapped page
 * like every plug-in allocation does -- a surface is one of the few host buffers
 * a plug-in is handed a pointer into, so it is one of the few a plug-in can run
 * off the end of, and finding that by watching which free() aborts is the slow
 * way round. */
static void w32_surf_free(w32_surf *s)
{
    if (!s || !s->px) return;
    if (w32_guard_heap()) w32_guard_free(s->px, (size_t)s->w * s->h * 4);
    else free(s->px);
    s->px = NULL;
}

static void w32_surf_size(w32_surf *s, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (s->w == w && s->h == h && s->px) return;
    w32_surf_free(s);
    s->px = w32_guard_heap() ? (uint32_t *)w32_guard_alloc((size_t)w * h * 4)
                             : (uint32_t *)calloc((size_t)w * h, 4);
    s->w = s->px ? w : 0;
    s->h = s->px ? h : 0;
}

/* ---- the WndProc call ---------------------------------------------------- */

typedef MS W_LRESULT (*w32_wndproc)(void *, uint32_t, W_WPARAM, W_LPARAM);

static W_LRESULT w32_call(w32_wnd *w, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wndproc p;
    int idx;
    if (!w || !w->wndproc) return 0;
    p = (w32_wndproc)w->wndproc;
    idx = (int)(w - W.wnd);
    return p(w32_h(W32_HWND_BASE, idx), msg, wp, lp);
}

/* ---- rectangles ---------------------------------------------------------- */

/* USER32's rectangle arithmetic, none of which was here.
 *
 * These look too trivial to matter and are the opposite. They carry no state
 * and touch no device, so a generic stub returns 0 and leaves the caller's
 * RECT exactly as it was -- which does not read as failure to the caller,
 * because these are the functions nobody checks. Layout code offsets a
 * rectangle and gets it back unmoved, unions two and gets neither, then
 * iterates until it fits. It never fits.
 *
 * That is what an endless SelectObject(font)/SelectObject(NULL) pair with a
 * growing stack turned out to be: a text layout recursing on a rectangle that
 * every pass left unchanged, until the stack ran out 72 frames deep.
 *
 * The empty-rectangle convention is the one detail worth stating: Windows calls
 * a rectangle empty when right <= left or bottom <= top, and the return value of
 * every function here reports whether the *result* is non-empty rather than
 * whether the call worked. Callers branch on it. */
static int w32_rect_empty(const W32RECT *r)
{ return !r || r->right <= r->left || r->bottom <= r->top; }

static MS int32_t st_SetRect(W32RECT *r, int32_t l, int32_t t, int32_t rt, int32_t b)
{ if (!r) return 0; r->left = l; r->top = t; r->right = rt; r->bottom = b; return 1; }

static MS int32_t st_SetRectEmpty(W32RECT *r)
{ if (!r) return 0; r->left = r->top = r->right = r->bottom = 0; return 1; }

static MS int32_t st_CopyRect(W32RECT *d, const W32RECT *s)
{ if (!d || !s) return 0; *d = *s; return 1; }

static MS int32_t st_OffsetRect(W32RECT *r, int32_t dx, int32_t dy)
{
    if (!r) return 0;
    r->left += dx; r->right += dx; r->top += dy; r->bottom += dy;
    return 1;
}

static MS int32_t st_InflateRect(W32RECT *r, int32_t dx, int32_t dy)
{
    if (!r) return 0;
    r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy;
    return 1;
}

static MS int32_t st_IntersectRect(W32RECT *d, const W32RECT *a, const W32RECT *b)
{
    if (!d || !a || !b) return 0;
    d->left   = a->left   > b->left   ? a->left   : b->left;
    d->top    = a->top    > b->top    ? a->top    : b->top;
    d->right  = a->right  < b->right  ? a->right  : b->right;
    d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (w32_rect_empty(d)) { st_SetRectEmpty(d); return 0; }
    return 1;
}

static MS int32_t st_UnionRect(W32RECT *d, const W32RECT *a, const W32RECT *b)
{
    int ae, be;
    if (!d || !a || !b) return 0;
    ae = w32_rect_empty(a); be = w32_rect_empty(b);
    /* An empty rectangle contributes nothing rather than dragging the union out
     * to include the origin, which is what taking the min of all four edges
     * unconditionally would do. */
    if (ae && be) { st_SetRectEmpty(d); return 0; }
    if (ae) { *d = *b; return 1; }
    if (be) { *d = *a; return 1; }
    d->left   = a->left   < b->left   ? a->left   : b->left;
    d->top    = a->top    < b->top    ? a->top    : b->top;
    d->right  = a->right  > b->right  ? a->right  : b->right;
    d->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return 1;
}

/* a minus b, but only when the difference is still a rectangle: b has to span
 * a completely in one axis and cut a whole edge off the other. Anything else
 * leaves a unchanged, which is what Windows does -- the result of subtracting
 * a hole from the middle of a rectangle is not a rectangle. */
static MS int32_t st_SubtractRect(W32RECT *d, const W32RECT *a, const W32RECT *b)
{
    W32RECT ov;
    if (!d || !a || !b) return 0;
    *d = *a;
    if (w32_rect_empty(a)) { st_SetRectEmpty(d); return 0; }
    if (!st_IntersectRect(&ov, a, b)) return 1;          /* nothing to take */
    if (ov.left <= a->left && ov.right >= a->right) {
        if (ov.top <= a->top)          d->top    = ov.bottom;
        else if (ov.bottom >= a->bottom) d->bottom = ov.top;
    } else if (ov.top <= a->top && ov.bottom >= a->bottom) {
        if (ov.left <= a->left)        d->left   = ov.right;
        else if (ov.right >= a->right) d->right  = ov.left;
    }
    if (w32_rect_empty(d)) { st_SetRectEmpty(d); return 0; }
    return 1;
}

static MS int32_t st_EqualRect(const W32RECT *a, const W32RECT *b)
{
    if (!a || !b) return 0;
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

static MS int32_t st_IsRectEmpty(const W32RECT *r) { return w32_rect_empty(r); }

/* The point is passed by value: two longs, not a pointer. Windows treats the
 * right and bottom edges as outside, so adjacent rectangles do not both claim
 * the same pixel. */
static MS int32_t st_PtInRect(const W32RECT *r, int32_t x, int32_t y)
{
    if (!r) return 0;
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

/* ---- hooks --------------------------------------------------------------- */

/* SetWindowsHookEx, because MFC does not treat it as optional.
 *
 * AfxHookWindowCreate installs a WH_CBT filter before every CreateWindowEx and
 * calls AfxThrowMemoryException() if it cannot -- so a stub returning NULL does
 * not cost a hook, it costs the editor, by way of a C++ throw from inside window
 * creation. That is what "effEditOpen failed" was.
 *
 * Installing it is only half. MFC uses the HCBT_CREATEWND that follows to attach
 * its CWnd to the new HWND and subclass the window procedure; a hook that is
 * registered and never called leaves every MFC window unattached, which fails
 * later and further away. So the hook has to actually fire, which is why
 * w32_create calls it below.
 *
 * WH_CBT is the only type dispatched. The others in the corpus -- keyboard and
 * mouse filters -- would need a real input queue to be meaningful, and a plugin
 * that installs one still gets a handle back and works without it. */
#define W32_WH_CBT        5
#define W32_HCBT_CREATEWND 3
#define W32_HCBT_DESTROYWND 4

typedef MS W_LRESULT (*w32_hookproc)(int32_t, W_WPARAM, W_LPARAM);

/* The chain runs in install order, most recent first, which is what Windows
 * does and what CallNextHookEx walks. */
static int w32_hook_next(int after, int id)
{
    int i;
    for (i = after - 1; i >= 1; i--)
        if (W.hook[i].used && W.hook[i].id == id) return i;
    return 0;
}

static W_LRESULT w32_hook_call(int id, int32_t code, W_WPARAM wp, W_LPARAM lp)
{
    int i = w32_hook_next(W32_MAX_HOOK, id);
    if (!i) return 0;
    return ((w32_hookproc)W.hook[i].proc)(code, wp, lp);
}

static MS void *st_SetWindowsHookExA(int32_t id, void *proc, void *mod, uint32_t tid)
{
    int i;
    (void)mod; (void)tid;
    if (!proc) return NULL;
    for (i = 1; i < W32_MAX_HOOK; i++) {
        if (W.hook[i].used) continue;
        W.hook[i].used = 1; W.hook[i].id = id; W.hook[i].proc = proc;
        PLOG("  [w32] SetWindowsHookEx id=%d proc=%p -> #%d\n", id, proc, i);
        return w32_h(W32_HOOK_BASE, i);
    }
    return NULL;
}
static MS void *st_SetWindowsHookExW(int32_t id, void *proc, void *mod, uint32_t tid)
{ return st_SetWindowsHookExA(id, proc, mod, tid); }

static MS int32_t st_UnhookWindowsHookEx(void *h)
{
    int i = w32_i(W32_HOOK_BASE, h);
    if (i <= 0 || i >= W32_MAX_HOOK || !W.hook[i].used) return 0;
    memset(&W.hook[i], 0, sizeof W.hook[i]);
    return 1;
}

/* A hook that declines passes the call along. Returning 0 without walking the
 * rest of the chain would silently drop whatever the next filter does -- and
 * MFC's own filter calls this on every code it does not handle. */
static MS W_LRESULT st_CallNextHookEx(void *h, int32_t code, W_WPARAM wp, W_LPARAM lp)
{
    int i = w32_i(W32_HOOK_BASE, h), nxt;
    if (i <= 0 || i >= W32_MAX_HOOK) i = W32_MAX_HOOK;
    nxt = w32_hook_next(i, (i < W32_MAX_HOOK && W.hook[i].used) ? W.hook[i].id
                                                                : W32_WH_CBT);
    if (!nxt) return 0;
    return ((w32_hookproc)W.hook[nxt].proc)(code, wp, lp);
}

/* ---- window management --------------------------------------------------- */

/* lpszClassName is either a string or an atom -- the value RegisterClass
 * returned, low 16 bits with the high bits clear. Windows accepts both, and a
 * caller that keeps the atom instead of the string is entitled to. Treating one
 * as a pointer dereferences a small integer and faults on the first byte, which
 * is exactly what NI's window classes did. */
static const char *w32_cls_name(const void *cls, char *buf, size_t n, int wide)
{
    if (!cls) return NULL;
    if ((uintptr_t)cls < 0x10000) {
        unsigned i = (unsigned)((uintptr_t)cls & 0xFFFF);
        if (i >= 0xC000) i -= 0xC000;
        if (i >= 1 && i < W32_MAX_CLS && W.cls[i].used) return W.cls[i].name;
        return NULL;                            /* an atom we never handed out */
    }
    if (wide) { w2c((const uint16_t *)cls, buf, n); return buf; }
    return (const char *)cls;
}

static MS uint16_t st_RegisterClassA(const W32WNDCLASSA *c)
{
    int i;
    if (!c || !c->lpszClassName) return 0;
    /* The offset matters more than the address: it is what a disassembler wants,
     * and the address moves every run. */
    PLOG("  [w32] RegisterClass '%s' proc=%p (image+0x%lx)\n", c->lpszClassName,
         c->lpfnWndProc,
         g_image_base ? (unsigned long)((uint8_t *)c->lpfnWndProc - g_image_base) : 0ul);
    for (i = 1; i < W32_MAX_CLS; i++) {
        if (W.cls[i].used) continue;
        W.cls[i].used = 1;
        snprintf(W.cls[i].name, sizeof W.cls[i].name, "%s", c->lpszClassName);
        W.cls[i].proc = c->lpfnWndProc;
        return (uint16_t)(0xC000 + i);
    }
    return 0;
}
/* The W form differs only in the class-name encoding. */
static MS uint16_t st_RegisterClassW(const W32WNDCLASSA *c)
{
    W32WNDCLASSA a;
    char name[64];
    if (!c) return 0;
    a = *c;
    a.lpszClassName = w32_cls_name(c->lpszClassName, name, sizeof name, 1);
    return st_RegisterClassA(&a);
}
static MS int32_t st_UnregisterClassA(const char *n, void *inst)
{
    int i;
    char c[64];
    (void)inst;
    n = w32_cls_name(n, c, sizeof c, 0);
    for (i = 1; i < W32_MAX_CLS; i++)
        if (W.cls[i].used && n && !strcmp(W.cls[i].name, n)) { W.cls[i].used = 0; return 1; }
    return 1;
}
static MS int32_t st_UnregisterClassW(const uint16_t *n, void *inst)
{ char b[64]; return st_UnregisterClassA(w32_cls_name(n, b, sizeof b, 1), inst); }

/* Defined further down; the control classes draw through them. */
static void *w32_dc_new(int wndidx);
static void  w32_text_at(w32_dc *d, w32_surf *t, int x, int y,
                         const char *s, int n, int opaque_rect_first,
                         const W32RECT *bg);

/* Wide to narrow for a window caption. winstubs.h has one of these, and it is
 * defined after this header is included. */
static void w2c_local(const uint16_t *w, char *out, size_t n)
{
    size_t i = 0;
    if (!w) { if (n) out[0] = 0; return; }
    for (; w[i] && i + 1 < n; i++) out[i] = w[i] < 256 ? (char)w[i] : '?';
    out[i] = 0;
}

/* ---- the built-in window classes ----------------------------------------
 *
 * BUTTON, STATIC, EDIT, COMBOBOX, LISTBOX and SCROLLBAR are not registered by
 * the application that uses them -- they come with the system, and a plug-in
 * creates one by name and expects it to draw itself and answer its own
 * messages. This host had none, so every such child was created with no window
 * procedure at all: invisible, inert, and silent about it.
 *
 * It does not matter for a plug-in whose editor is one big skinned bitmap, and
 * it is the whole editor for one built as a dialog -- which a great many from
 * the COMCTL32 era are.
 *
 * These draw the classic look rather than a themed one. A themed control is a
 * bitmap from a .msstyles file this host has no business shipping, and the
 * classic look is what the same code drew before XP and what these plug-ins
 * were designed against. */

#define WM_SETTEXT       0x000C
#define WM_GETTEXT       0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_ENABLE        0x000A
#define WM_SETFONT       0x0030
#define WM_GETFONT       0x0031

#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define BM_GETSTATE 0x00F2
#define BM_SETSTATE 0x00F3

#define CB_ADDSTRING     0x0143
#define CB_GETCOUNT      0x0146
#define CB_GETCURSEL     0x0147
#define CB_GETLBTEXT     0x0148
#define CB_GETLBTEXTLEN  0x0149
#define CB_INSERTSTRING  0x014A
#define CB_RESETCONTENT  0x014B
#define CB_SETCURSEL     0x014E

#define LB_ADDSTRING     0x0180
#define LB_RESETCONTENT  0x0184
#define LB_SETCURSEL     0x0186
#define LB_GETCURSEL     0x0188
#define LB_GETTEXT       0x0189
#define LB_GETCOUNT      0x018B

/* Button styles live in the low four bits of the window style. */
#define BS_TYPEMASK        0x0F
#define BS_PUSHBUTTON      0
#define BS_DEFPUSHBUTTON   1
#define BS_CHECKBOX        2
#define BS_AUTOCHECKBOX    3
#define BS_RADIOBUTTON     4
#define BS_3STATE          5
#define BS_AUTO3STATE      6
#define BS_GROUPBOX        7
#define BS_AUTORADIOBUTTON 9

/* The classic system colours, which is what these controls are drawn in. */
#define CLR_FACE      0xC0C0C0u
#define CLR_SHADOW    0x808080u
#define CLR_DKSHADOW  0x000000u
#define CLR_HILIGHT   0xFFFFFFu
#define CLR_WINDOW    0xFFFFFFu
#define CLR_TEXT      0x000000u
#define CLR_GRAYTEXT  0x808080u

static void ctl_fill(w32_surf *t, int l, int tp, int r, int b, uint32_t rgb)
{
    int y, x;
    if (!t || !t->px) return;
    for (y = tp; y < b; y++) {
        if (y < 0 || y >= t->h) continue;
        for (x = l; x < r; x++) {
            if (x < 0 || x >= t->w) continue;
            t->px[(size_t)y * t->w + x] = rgb;
        }
    }
}

/* The two-pixel bevel every classic control is drawn with: light above and
 * left, dark below and right, reversed when the control is sunken or pressed. */
static void ctl_bevel(w32_surf *t, int l, int tp, int r, int b, int sunken)
{
    uint32_t tl = sunken ? CLR_SHADOW : CLR_HILIGHT;
    uint32_t br = sunken ? CLR_HILIGHT : CLR_SHADOW;
    uint32_t tl2 = sunken ? CLR_DKSHADOW : CLR_FACE;
    uint32_t br2 = sunken ? CLR_FACE : CLR_DKSHADOW;
    ctl_fill(t, l, tp, r, tp + 1, tl);
    ctl_fill(t, l, tp, l + 1, b, tl);
    ctl_fill(t, l + 1, tp + 1, r - 1, tp + 2, tl2);
    ctl_fill(t, l + 1, tp + 1, l + 2, b - 1, tl2);
    ctl_fill(t, l, b - 1, r, b, br);
    ctl_fill(t, r - 1, tp, r, b, br);
    ctl_fill(t, l + 1, b - 2, r - 1, b - 1, br2);
    ctl_fill(t, r - 2, tp + 1, r - 1, b - 1, br2);
}

/* Text through the same rasteriser everything else uses. A DC is borrowed for
 * it because that is what carries the colour and the font. */
static void ctl_text(int wi, int x, int y, const char *txt, uint32_t rgb)
{
    void *hdc;
    w32_dc *d;
    w32_surf ttmp, *t;

    if (!txt || !*txt) return;
    if (!(hdc = w32_dc_new(wi))) return;
    d = w32_dcget(hdc);
    if (d) {
        d->text_color = ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
        d->bk_mode = 1;                            /* TRANSPARENT */
        t = w32_target_in_raw(d, &ttmp);
        if (t) w32_text_at(d, t, x, y, txt, (int)strlen(txt), 0, NULL);
        d->used = 0;
    }
}

static int ctl_text_w(const char *txt)
{
    int w = 0, h = 0;
    if (!txt || !*txt) return 0;
    dw_text_measure(txt, (int)strlen(txt), 12, &w, &h, NULL);
    return w;
}

static void ctl_paint_button(w32_wnd *w, int wi)
{
    w32_surf *t = &w->surf;
    int type = w->style & BS_TYPEMASK;
    int cw = w->w, ch = w->h;
    uint32_t fg = w->enabled ? CLR_TEXT : CLR_GRAYTEXT;

    if (type == BS_GROUPBOX) {
        /* A frame with the caption sitting in a gap at the top left. */
        int cap = ctl_text_w(w->text);
        ctl_fill(t, 0, 0, cw, ch, CLR_FACE);
        ctl_fill(t, 0, 6, cw, 7, CLR_SHADOW);
        ctl_fill(t, 0, 6, 1, ch, CLR_SHADOW);
        ctl_fill(t, 0, ch - 1, cw, ch, CLR_HILIGHT);
        ctl_fill(t, cw - 1, 6, cw, ch, CLR_HILIGHT);
        if (cap) { ctl_fill(t, 8, 0, 12 + cap, 14, CLR_FACE);
                   ctl_text(wi, 10, 1, w->text, fg); }
        return;
    }
    if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
        type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON ||
        type == BS_3STATE || type == BS_AUTO3STATE) {
        int box = 13, top = (ch - box) / 2;
        ctl_fill(t, 0, 0, cw, ch, CLR_FACE);
        ctl_fill(t, 0, top, box, top + box, CLR_WINDOW);
        ctl_bevel(t, 0, top, box, top + box, 1);
        if (w->ctl_check) {
            /* A tick, drawn as the two strokes it is made of. */
            int i;
            for (i = 0; i < 3; i++) ctl_fill(t, 3 + i, top + 5 + i, 4 + i, top + 8 + i, fg);
            for (i = 0; i < 4; i++) ctl_fill(t, 6 + i, top + 7 - i, 7 + i, top + 10 - i, fg);
        }
        ctl_text(wi, box + 4, (ch - 14) / 2, w->text, fg);
        return;
    }
    /* A push button: face, bevel, and the caption centred. */
    ctl_fill(t, 0, 0, cw, ch, CLR_FACE);
    ctl_bevel(t, 0, 0, cw, ch, w->ctl_check ? 1 : 0);
    {
        int tw = ctl_text_w(w->text);
        ctl_text(wi, (cw - tw) / 2 + (w->ctl_check ? 1 : 0),
                 (ch - 14) / 2 + (w->ctl_check ? 1 : 0), w->text, fg);
    }
}

static void ctl_paint_static(w32_wnd *w, int wi)
{
    /* SS_ styles 0-2 are text alignments; the frame and rectangle styles are
     * 4-7 and draw no text at all. */
    int style = w->style & 0x1F;
    w32_surf *t = &w->surf;

    if (style >= 4 && style <= 7) {
        ctl_fill(t, 0, 0, w->w, w->h,
                 style == 4 ? CLR_DKSHADOW : style == 5 ? CLR_FACE : CLR_WINDOW);
        return;
    }
    ctl_fill(t, 0, 0, w->w, w->h, CLR_FACE);
    {
        int tw = ctl_text_w(w->text), x = 0;
        if (style == 1) x = (w->w - tw) / 2;        /* SS_CENTER */
        else if (style == 2) x = w->w - tw;         /* SS_RIGHT  */
        ctl_text(wi, x, (w->h - 14) / 2, w->text,
                 w->enabled ? CLR_TEXT : CLR_GRAYTEXT);
    }
}

static void ctl_paint_edit(w32_wnd *w, int wi)
{
    w32_surf *t = &w->surf;
    ctl_fill(t, 0, 0, w->w, w->h, CLR_WINDOW);
    ctl_bevel(t, 0, 0, w->w, w->h, 1);
    ctl_text(wi, 3, (w->h - 14) / 2, w->text,
             w->enabled ? CLR_TEXT : CLR_GRAYTEXT);
}

static void ctl_paint_combo(w32_wnd *w, int wi)
{
    w32_surf *t = &w->surf;
    int bw = 16, bh = w->h < 21 ? w->h : 21;
    const char *sel = (w->items && w->ctl_check >= 0 && w->ctl_check < w->nitems)
                        ? w->items[w->ctl_check] : w->text;

    ctl_fill(t, 0, 0, w->w, bh, CLR_WINDOW);
    ctl_bevel(t, 0, 0, w->w, bh, 1);
    /* The drop-down button, and the arrow on it. */
    ctl_fill(t, w->w - bw, 2, w->w - 2, bh - 2, CLR_FACE);
    ctl_bevel(t, w->w - bw, 2, w->w - 2, bh - 2, 0);
    {
        int cx = w->w - bw / 2 - 1, cy = bh / 2, i;
        for (i = 0; i < 4; i++)
            ctl_fill(t, cx - 3 + i, cy - 2 + i, cx + 4 - i, cy - 1 + i, CLR_TEXT);
    }
    ctl_text(wi, 3, (bh - 14) / 2, sel, w->enabled ? CLR_TEXT : CLR_GRAYTEXT);
}

static void ctl_paint_listbox(w32_wnd *w, int wi)
{
    w32_surf *t = &w->surf;
    int i, y = 2;
    ctl_fill(t, 0, 0, w->w, w->h, CLR_WINDOW);
    ctl_bevel(t, 0, 0, w->w, w->h, 1);
    for (i = 0; i < w->nitems && y + 14 < w->h; i++, y += 14) {
        if (i == w->ctl_check) {
            ctl_fill(t, 2, y, w->w - 2, y + 14, 0x000080u);   /* the selection */
            ctl_text(wi, 4, y, w->items[i], CLR_HILIGHT);
        } else {
            ctl_text(wi, 4, y, w->items[i], CLR_TEXT);
        }
    }
}

static void ctl_paint_scrollbar(w32_wnd *w)
{
    w32_surf *t = &w->surf;
    ctl_fill(t, 0, 0, w->w, w->h, 0xE0E0E0u);
    ctl_bevel(t, 0, 0, w->w, w->h, 1);
}

/* Room for one more item, grown a few at a time. */
static int ctl_items_room(w32_wnd *w)
{
    char (*p)[64];
    if (w->items && (w->nitems % 16) != 0) return 1;
    p = (char (*)[64])realloc(w->items, (size_t)(w->nitems + 16) * 64);
    if (!p) return 0;
    w->items = p;
    return 1;
}

static W_LRESULT ctl_common(w32_wnd *w, int wi, uint32_t msg, W_WPARAM wp, W_LPARAM lp,
                            int *handled)
{
    *handled = 1;
    switch (msg) {
    case WM_SETTEXT:
        snprintf(w->text, sizeof w->text, "%s", lp ? (const char *)(uintptr_t)lp : "");
        w->has_update = 1;
        w->update.left = 0; w->update.top = 0;
        w->update.right = w->w; w->update.bottom = w->h;
        return 1;
    case WM_GETTEXT: {
        char *out = (char *)(uintptr_t)lp;
        size_t n = (size_t)wp;
        if (!out || !n) return 0;
        snprintf(out, n, "%s", w->text);
        return (W_LRESULT)strlen(out); }
    case WM_GETTEXTLENGTH:
        return (W_LRESULT)strlen(w->text);
    case WM_ENABLE:
        w->enabled = wp != 0;
        w->has_update = 1;
        return 0;
    case WM_SETFONT:
        w->has_update = 1;
        return 0;
    case WM_GETFONT:
        return 0;
    case WM_ERASEBKGND:
        return 1;                                  /* the paint covers it */
    default:
        *handled = 0;
        return 0;
    }
}

static MS W_LRESULT ctl_button_proc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled;
    W_LRESULT r;

    if (!w) return 0;
    r = ctl_common(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    switch (msg) {
    case WM_PAINT:      ctl_paint_button(w, wi); w->has_update = 0; return 0;
    case BM_GETCHECK:   return w->ctl_check;
    case BM_SETCHECK:   w->ctl_check = (int)wp; w->has_update = 1; return 0;
    case BM_GETSTATE:   return w->ctl_check;
    case BM_SETSTATE:   w->ctl_check = (int)wp; w->has_update = 1; return 0;
    default:            return 0;
    }
}

static MS W_LRESULT ctl_static_proc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled;
    W_LRESULT r;
    if (!w) return 0;
    r = ctl_common(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    if (msg == WM_PAINT) { ctl_paint_static(w, wi); w->has_update = 0; }
    return 0;
}

static MS W_LRESULT ctl_edit_proc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled;
    W_LRESULT r;
    if (!w) return 0;
    r = ctl_common(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    if (msg == WM_PAINT) { ctl_paint_edit(w, wi); w->has_update = 0; }
    return 0;
}

static MS W_LRESULT ctl_combo_proc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled;
    W_LRESULT r;
    const char *sz = (const char *)(uintptr_t)lp;

    if (!w) return 0;
    r = ctl_common(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    switch (msg) {
    case WM_PAINT:   ctl_paint_combo(w, wi); w->has_update = 0; return 0;
    case CB_ADDSTRING:
        if (!sz || !ctl_items_room(w)) return -1;   /* CB_ERRSPACE */
        snprintf(w->items[w->nitems], 64, "%s", sz);
        return w->nitems++;
    case CB_GETCOUNT:   return w->nitems;
    case CB_GETCURSEL:  return w->nitems ? w->ctl_check : -1;
    case CB_SETCURSEL:
        if ((int)wp < -1 || (int)wp >= w->nitems) { w->ctl_check = -1; return -1; }
        w->ctl_check = (int)wp; w->has_update = 1; return w->ctl_check;
    case CB_GETLBTEXT: {
        char *out = (char *)(uintptr_t)lp;
        if (!out || (int)wp < 0 || (int)wp >= w->nitems) return -1;
        strcpy(out, w->items[wp]);
        return (W_LRESULT)strlen(out); }
    case CB_GETLBTEXTLEN:
        if ((int)wp < 0 || (int)wp >= w->nitems) return -1;
        return (W_LRESULT)strlen(w->items[wp]);
    case CB_RESETCONTENT:
        w->nitems = 0; w->ctl_check = -1; w->has_update = 1; return 0;
    default: return 0;
    }
}

static MS W_LRESULT ctl_list_proc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled;
    W_LRESULT r;
    const char *sz = (const char *)(uintptr_t)lp;

    if (!w) return 0;
    r = ctl_common(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    switch (msg) {
    case WM_PAINT:  ctl_paint_listbox(w, wi); w->has_update = 0; return 0;
    case LB_ADDSTRING:
        if (!sz || !ctl_items_room(w)) return -1;
        snprintf(w->items[w->nitems], 64, "%s", sz);
        return w->nitems++;
    case LB_GETCOUNT:  return w->nitems;
    case LB_GETCURSEL: return w->nitems ? w->ctl_check : -1;
    case LB_SETCURSEL: w->ctl_check = (int)wp; w->has_update = 1; return w->ctl_check;
    case LB_GETTEXT: {
        char *out = (char *)(uintptr_t)lp;
        if (!out || (int)wp < 0 || (int)wp >= w->nitems) return -1;
        strcpy(out, w->items[wp]);
        return (W_LRESULT)strlen(out); }
    case LB_RESETCONTENT:
        w->nitems = 0; w->ctl_check = -1; w->has_update = 1; return 0;
    default: return 0;
    }
}

static MS W_LRESULT ctl_scroll_proc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled;
    W_LRESULT r;
    if (!w) return 0;
    r = ctl_common(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    if (msg == WM_PAINT) { ctl_paint_scrollbar(w); w->has_update = 0; }
    return 0;
}

/* The system classes, matched case-insensitively as Windows does. */
static void *w32_builtin_class(const char *cls)
{
    if (!cls) return NULL;
    if (!strcasecmp(cls, "BUTTON"))    return (void *)ctl_button_proc;
    if (!strcasecmp(cls, "STATIC"))    return (void *)ctl_static_proc;
    if (!strcasecmp(cls, "EDIT"))      return (void *)ctl_edit_proc;
    if (!strcasecmp(cls, "COMBOBOX"))  return (void *)ctl_combo_proc;
    if (!strcasecmp(cls, "LISTBOX"))   return (void *)ctl_list_proc;
    if (!strcasecmp(cls, "SCROLLBAR")) return (void *)ctl_scroll_proc;
    return NULL;
}

/* The window procedure a class was registered with, or NULL if it was not.
 * GetClassInfo answers from this. */
static void *w32_class_proc(const char *name)
{
    int i;
    if (!name) return NULL;
    for (i = 1; i < W32_MAX_CLS; i++)
        if (W.cls[i].used && !strcmp(W.cls[i].name, name)) return W.cls[i].proc;
    return NULL;
}

static void *w32_create(const char *cls, const char *name, int x, int y, int w, int h,
                        void *parent, void *param, uint32_t style, uint32_t exstyle)
{
    int i, ci = 0, pi;
    void *hwnd;

    for (i = 1; i < W32_MAX_CLS; i++)
        if (W.cls[i].used && cls && !strcmp(W.cls[i].name, cls)) { ci = i; break; }

    for (i = 1; i < W32_MAX_WND; i++) {
        if (W.wnd[i].used) continue;
        memset(&W.wnd[i], 0, sizeof W.wnd[i]);
        W.wnd[i].used = 1;
        /* A class the application registered, or one of the system's. A child
         * created as BUTTON or EDIT gets no procedure at all otherwise, and
         * cannot draw or answer for itself. */
        W.wnd[i].wndproc = ci ? W.cls[ci].proc : w32_builtin_class(cls);
        W.wnd[i].enabled = 1;
        /* WS_VISIBLE. A control created without it is deliberately hidden, and
         * a helper window a plug-in keeps off-screen must not be composited
         * over the interface. */
        W.wnd[i].visible = (style & 0x10000000u) != 0;
        W.wnd[i].ctl_check = w32_builtin_class(cls) &&
                             (!strcasecmp(cls, "COMBOBOX") ||
                              !strcasecmp(cls, "LISTBOX")) ? -1 : 0;
        if (name) snprintf(W.wnd[i].text, sizeof W.wnd[i].text, "%s", name);
        W.wnd[i].x = x; W.wnd[i].y = y;
        W.wnd[i].w = w > 0 ? w : 1;
        W.wnd[i].h = h > 0 ? h : 1;
        W.wnd[i].style = style;
        W.wnd[i].exstyle = exstyle;
        snprintf(W.wnd[i].cls, sizeof W.wnd[i].cls, "%s", cls ? cls : "");
        pi = w32_i(W32_HWND_BASE, parent);
        W.wnd[i].parent = (pi > 0 && pi < W32_MAX_WND) ? pi : 0;
        w32_surf_size(&W.wnd[i].surf, W.wnd[i].w, W.wnd[i].h);
        hwnd = w32_h(W32_HWND_BASE, i);
        PLOG("  [w32] CreateWindow #%d cls='%s' %dx%d parent=%d proc=%p\n",
             i, cls ? cls : "", W.wnd[i].w, W.wnd[i].h, W.wnd[i].parent,
             W.wnd[i].wndproc);

        /* The window the plugin parents to our container is its editor, and
         * that is the one whose pixels we show. Without this distinction we
         * would present our own empty container instead. */
        if (W.host && W.wnd[i].parent == W.host && !W.display) W.display = i;
        else if (!W.host && !W.display) W.display = i;

        {   /* CREATESTRUCT is only read for lpCreateParams in practice, and
             * iPlug2 stashes its instance pointer there. */
            struct { void *lpCreateParams, *hInstance, *hMenu, *hwndParent;
                     int32_t cy, cx, y, x; int32_t style; const char *name, *cls;
                     uint32_t exstyle; } cs;
            memset(&cs, 0, sizeof cs);
            cs.lpCreateParams = param;
            cs.hwndParent = parent;
            cs.cx = W.wnd[i].w; cs.cy = W.wnd[i].h;
            cs.x = x; cs.y = y;
            cs.style = (int32_t)style;
            cs.cls = cls;
            /* Before WM_NCCREATE, as Windows does: MFC attaches its CWnd here
             * and subclasses the window procedure, so anything sent earlier
             * would go to the class procedure it is about to replace. */
            {   struct { void *cs; void *insert_after; } cbt;
                cbt.cs = &cs; cbt.insert_after = NULL;
                w32_hook_call(W32_WH_CBT, W32_HCBT_CREATEWND,
                              (W_WPARAM)(uintptr_t)hwnd, (W_LPARAM)&cbt);
            }
            w32_call(&W.wnd[i], WM_NCCREATE, 0, (W_LPARAM)&cs);
            w32_call(&W.wnd[i], WM_CREATE,   0, (W_LPARAM)&cs);
        }
        w32_call(&W.wnd[i], WM_SIZE, 0, (W_LPARAM)((W.wnd[i].h << 16) | (W.wnd[i].w & 0xFFFF)));
        W.wnd[i].has_update = 1;
        W.wnd[i].update.left = 0; W.wnd[i].update.top = 0;
        W.wnd[i].update.right = W.wnd[i].w; W.wnd[i].update.bottom = W.wnd[i].h;
        return hwnd;
    }
    return NULL;
}

static MS void *st_CreateWindowExA(uint32_t ex, const char *cls, const char *name,
                                  uint32_t style, int32_t x, int32_t y, int32_t w, int32_t h,
                                  void *parent, void *menu, void *inst, void *param)
{
    char c[64];
    (void)menu; (void)inst;
    return w32_create(w32_cls_name(cls, c, sizeof c, 0), name, x, y, w, h,
                      parent, param, style, ex);
}
static MS void *st_CreateWindowExW(uint32_t ex, const uint16_t *cls, const uint16_t *name,
                                  uint32_t style, int32_t x, int32_t y, int32_t w, int32_t h,
                                  void *parent, void *menu, void *inst, void *param)
{
    char c[64], n[128];
    (void)menu; (void)inst;
    /* The caption arrives wide; the controls draw it narrow. */
    if (name) w2c_local(name, n, sizeof n);
    return w32_create(w32_cls_name(cls, c, sizeof c, 1), name ? n : NULL,
                      x, y, w, h, parent, param, style, ex);
}

/* Whether a handle names a live window.
 *
 * A plugin handed a parent HWND checks it before building its interface into it,
 * and a stub answering with whatever was in the return register decides that at
 * random. FM8 registered all four of its window classes and then created none of
 * them: it had asked whether the container it was given was real, been told no,
 * and declined. Nothing was left to paint, which is why 80 WM_PAINTs produced a
 * black image. */
static MS int32_t st_IsWindow(const void *hwnd)
{ return w32_wget(hwnd) != NULL; }
static MS int32_t st_IsWindowVisible(const void *hwnd)
{ w32_wnd *w = w32_wget(hwnd); return w && w->visible; }
static MS int32_t st_IsWindowEnabled(const void *hwnd)
{ return w32_wget(hwnd) != NULL; }        /* nothing here disables a window */
static MS int32_t st_IsWindowUnicode(const void *hwnd)
{ return w32_wget(hwnd) != NULL; }        /* class names arrive wide */
static MS int32_t st_IsChild(const void *parent, const void *hwnd)
{
    w32_wnd *w = w32_wget(hwnd);
    int pi = w32_i(W32_HWND_BASE, parent), guard = 0;
    if (!w || pi <= 0) return 0;
    while (w && w->parent && guard++ < W32_MAX_WND) {
        if (w->parent == pi) return 1;
        w = (w->parent > 0 && w->parent < W32_MAX_WND) ? &W.wnd[w->parent] : NULL;
    }
    return 0;
}

/* Dialog base units: the average width and height of a character in the system
 * font, packed low word / high word. Windows returns 8 and 16 for the default
 * 8-point font, and layout code divides by the low word to convert dialog units
 * into pixels -- FM8 computes (width * 4) / LOWORD(this), so a stub returning
 * zero is not a wrong layout, it is a divide by zero. */
static MS uint32_t st_GetDialogBaseUnits(void)
{ return (16u << 16) | 8u; }

/* The same conversion, done for a whole rectangle: 4 horizontal units and 8
 * vertical units per character cell, which is what the units above encode. */
static MS int32_t st_MapDialogRect(void *hwnd, W32RECT *r)
{
    (void)hwnd;
    if (!r) return 0;
    r->left   = r->left   * 8 / 4;
    r->right  = r->right  * 8 / 4;
    r->top    = r->top    * 16 / 8;
    r->bottom = r->bottom * 16 / 8;
    return 1;
}

static MS int32_t st_DestroyWindow(void *hwnd)
{
    w32_wnd *w = w32_wget(hwnd);
    int i;
    if (!w) return 0;
    w32_call(w, WM_DESTROY, 0, 0);
    i = (int)(w - W.wnd);
    w32_surf_free(&w->surf);
    memset(w, 0, sizeof *w);
    if (W.display == i) W.display = 0;
    if (W.host == i) W.host = 0;
    return 1;
}

static MS int32_t st_ShowWindow(void *hwnd, int32_t cmd)
{
    w32_wnd *w = w32_wget(hwnd);
    if (!w) return 0;
    w->visible = (cmd != 0);
    w32_call(w, WM_SHOWWINDOW, (W_WPARAM)w->visible, 0);
    return 1;
}

static void w32_resize(w32_wnd *w, int nw, int nh)
{
    if (nw <= 0 || nh <= 0) return;
    if (w->w == nw && w->h == nh) return;
    w->w = nw; w->h = nh;
    w32_surf_size(&w->surf, nw, nh);
    w32_call(w, WM_SIZE, 0, (W_LPARAM)((nh << 16) | (nw & 0xFFFF)));
    w->has_update = 1;
    w->update.left = 0; w->update.top = 0; w->update.right = nw; w->update.bottom = nh;
}

static MS int32_t st_SetWindowPos(void *hwnd, void *after, int32_t x, int32_t y,
                                 int32_t w, int32_t h, uint32_t flags)
{
    w32_wnd *p = w32_wget(hwnd);
    (void)after;
    if (!p) return 0;
    if (!(flags & 0x0002)) { p->x = x; p->y = y; }     /* SWP_NOMOVE */
    if (!(flags & 0x0001)) w32_resize(p, w, h);        /* SWP_NOSIZE */
    return 1;
}
static MS int32_t st_MoveWindow(void *hwnd, int32_t x, int32_t y, int32_t w, int32_t h, int32_t rp)
{
    w32_wnd *p = w32_wget(hwnd);
    (void)rp;
    if (!p) return 0;
    p->x = x; p->y = y;
    w32_resize(p, w, h);
    return 1;
}
static MS int32_t st_GetWindowRect(void *hwnd, W32RECT *r)
{
    w32_wnd *w = w32_wget(hwnd);
    if (!w || !r) return 0;
    r->left = w->x; r->top = w->y;
    r->right = w->x + w->w; r->bottom = w->y + w->h;
    return 1;
}
static MS int32_t st_GetClientRect(void *hwnd, W32RECT *r)
{
    w32_wnd *w = w32_wget(hwnd);
    if (!r) return 0;
    r->left = r->top = 0;
    r->right = w ? w->w : 0;
    r->bottom = w ? w->h : 0;
    return 1;
}
/* GetWindowInfo. The whole structure, not a return code: a caller reads
 * rcClient out of it and never looks at what the call returned. */
static MS int32_t st_GetWindowInfo(void *hwnd, void *pwi)
{
    w32_wnd *w = w32_wget(hwnd);
    uint8_t *p = pwi;
    W32RECT rc;
    if (!p) return 0;
    /* cbSize at 0, rcWindow at 4, rcClient at 20, dwStyle 36, dwExStyle 40,
     * dwWindowStatus 44, cxWindowBorders 48, cyWindowBorders 52,
     * atomWindowType 56, wCreatorVersion 58. Sixty bytes, both widths. */
    memset(p + 4, 0, 56);
    st_GetWindowRect(hwnd, &rc);
    memcpy(p + 4, &rc, sizeof rc);
    rc.left = rc.top = 0;
    rc.right  = w ? w->w : 0;
    rc.bottom = w ? w->h : 0;
    memcpy(p + 20, &rc, sizeof rc);
    *(uint32_t *)(p + 44) = 1;              /* WS_ACTIVECAPTION */
    *(uint16_t *)(p + 58) = 0x0500;
    return 1;
}

static MS void *st_GetParent(void *hwnd)
{
    w32_wnd *w = w32_wget(hwnd);
    return (w && w->parent) ? w32_h(W32_HWND_BASE, w->parent) : NULL;
}
static MS void *st_GetAncestor(void *hwnd, uint32_t flags)
{ (void)flags; return st_GetParent(hwnd) ? st_GetParent(hwnd) : hwnd; }
static MS int32_t st_GetClassNameA(void *hwnd, char *buf, int32_t n)
{
    w32_wnd *w = w32_wget(hwnd);
    if (!buf || n <= 0) return 0;
    return (int32_t)snprintf(buf, (size_t)n, "%s", w ? w->cls : "");
}
static MS int32_t st_GetClassNameW(void *hwnd, uint16_t *buf, int32_t n)
{
    char b[64];
    int i;
    st_GetClassNameA(hwnd, b, sizeof b);
    for (i = 0; b[i] && i + 1 < n; i++) buf[i] = (uint8_t)b[i];
    if (n) buf[i] = 0;
    return i;
}
static MS int32_t st_SetWindowTextA(void *h, const char *s) { (void)h;(void)s; return 1; }
static MS int32_t st_SetWindowTextW(void *h, const uint16_t *s) { (void)h;(void)s; return 1; }
static MS int32_t st_BringWindowToTop(void *h) { (void)h; return 1; }
static MS uint32_t st_GetWindowThreadProcessId(void *h, uint32_t *pid)
{ (void)h; if (pid) *pid = (uint32_t)getpid(); return (uint32_t)(uintptr_t)pthread_self(); }
static MS int32_t st_EnumWindows(void *fn, intptr_t p) { (void)fn;(void)p; return 1; }

static W_LRESULT w32_getlong(void *hwnd, int32_t idx)
{
    w32_wnd *w = w32_wget(hwnd);
    if (!w) return 0;
    switch (idx) {
    case GWLP_USERDATA: return w->userdata;
    case GWLP_WNDPROC:  return (W_LRESULT)(intptr_t)w->wndproc;
    case GWL_STYLE:     return w->style;
    case GWL_EXSTYLE:   return w->exstyle;
    default:            return 0;
    }
}
static W_LRESULT w32_setlong(void *hwnd, int32_t idx, W_LRESULT v)
{
    w32_wnd *w = w32_wget(hwnd);
    W_LRESULT old;
    if (!w) return 0;
    old = w32_getlong(hwnd, idx);
    switch (idx) {
    case GWLP_USERDATA: w->userdata = v; break;
    case GWLP_WNDPROC:  w->wndproc = (void *)(intptr_t)v; break;
    case GWL_STYLE:     w->style = (int32_t)v; break;
    case GWL_EXSTYLE:   w->exstyle = (int32_t)v; break;
    default: break;
    }
    return old;
}
static MS W_LRESULT st_GetWindowLongPtrA(void *h, int32_t i) { return w32_getlong(h, i); }
static MS W_LRESULT st_GetWindowLongPtrW(void *h, int32_t i) { return w32_getlong(h, i); }
static MS int32_t st_GetWindowLongA(void *h, int32_t i) { return (int32_t)w32_getlong(h, i); }
static MS int32_t st_GetWindowLongW(void *h, int32_t i) { return (int32_t)w32_getlong(h, i); }
static MS W_LRESULT st_SetWindowLongPtrA(void *h, int32_t i, W_LRESULT v) { return w32_setlong(h, i, v); }
static MS W_LRESULT st_SetWindowLongPtrW(void *h, int32_t i, W_LRESULT v) { return w32_setlong(h, i, v); }
static MS int32_t st_SetWindowLongA(void *h, int32_t i, int32_t v)
{ return (int32_t)w32_setlong(h, i, v); }
static MS int32_t st_SetWindowLongW(void *h, int32_t i, int32_t v)
{ return (int32_t)w32_setlong(h, i, v); }

/* ---- messages ----------------------------------------------------------- */

static MS intptr_t st_DefWindowProcA(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    (void)hwnd; (void)wp; (void)lp;
    /* Claiming the background is erased avoids a flash of undrawn window, and
     * the plugin repaints everything anyway. */
    if (msg == WM_ERASEBKGND) return 1;
    return 0;
}
static MS intptr_t st_DefWindowProcW(void *h, uint32_t m, uintptr_t w, intptr_t l)
{ return st_DefWindowProcA(h, m, w, l); }

static MS intptr_t st_SendMessageA(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    w32_wnd *w = w32_wget(hwnd);
    return w ? w32_call(w, msg, wp, lp) : 0;
}
static MS intptr_t st_SendMessageW(void *h, uint32_t m, uintptr_t w, intptr_t l)
{ return st_SendMessageA(h, m, w, l); }

static void w32_post(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    int next = (W.qhead + 1) % W32_MSGQ;
    if (next == W.qtail) return;                  /* full: drop, like Windows */
    memset(&W.q[W.qhead], 0, sizeof W.q[0]);
    W.q[W.qhead].hwnd = hwnd;
    W.q[W.qhead].message = msg;
    W.q[W.qhead].wParam = wp;
    W.q[W.qhead].lParam = lp;
    W.qhead = next;
}
static MS int32_t st_PostMessageA(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{ w32_post(hwnd, msg, wp, lp); return 1; }
static MS int32_t st_PostMessageW(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{ w32_post(hwnd, msg, wp, lp); return 1; }

static MS int32_t st_PeekMessageA(W32MSG *m, void *hwnd, uint32_t f1, uint32_t f2, uint32_t rm)
{
    (void)hwnd; (void)f1; (void)f2;
    w32_pump_input();                 /* a spinning plugin gets fresh input here */
    if (W.qtail == W.qhead || !m) return 0;
    *m = W.q[W.qtail];
    if (rm & 1) W.qtail = (W.qtail + 1) % W32_MSGQ;   /* PM_REMOVE */
    return 1;
}
static MS int32_t st_TranslateMessage(const W32MSG *m) { (void)m; return 0; }
static MS W_LRESULT st_DispatchMessageA(const W32MSG *m)
{
    w32_wnd *w;
    if (!m) return 0;
    w = w32_wget(m->hwnd);
    return w ? w32_call(w, m->message, m->wParam, m->lParam) : 0;
}
static MS intptr_t st_CallWindowProcA(void *proc, void *hwnd, uint32_t msg,
                                    uintptr_t wp, intptr_t lp)
{
    w32_wndproc p = (w32_wndproc)proc;
    return p ? p(hwnd, msg, wp, lp) : st_DefWindowProcA(hwnd, msg, wp, lp);
}
static MS intptr_t st_CallWindowProcW(void *proc, void *h, uint32_t m, uintptr_t w, intptr_t l)
{ return st_CallWindowProcA(proc, h, m, w, l); }

static MS uint32_t st_RegisterWindowMessageA(const char *s)
{
    /* Stable per-name ids above WM_APP; a hash is enough since only equality
     * matters to the plugin. */
    uint32_t hash = 0xC000;
    if (s) while (*s) hash = hash * 31u + (unsigned char)*s++;
    return 0xC000 + (hash & 0x3FFF);
}
static MS uint32_t st_RegisterWindowMessageW(const uint16_t *s)
{ char b[128]; w2c(s, b, sizeof b); return st_RegisterWindowMessageA(b); }
static MS W_LPARAM st_GetMessageExtraInfo(void) { return 0; }

/* ---- device contexts ---------------------------------------------------- */

static void *w32_dc_new(int wndidx)
{
    int i;
    for (i = 1; i < W32_MAX_DC; i++) {
        if (W.dc[i].used) continue;
        memset(&W.dc[i], 0, sizeof W.dc[i]);
        W.dc[i].used = 1;
        W.dc[i].wnd = wndidx;
        return w32_h(W32_HDC_BASE, i);
    }
    return NULL;
}
static void w32_present(int wndidx)
{
    w32_wnd *w;
    if (wndidx <= 0 || wndidx >= W32_MAX_WND) return;
    w = &W.wnd[wndidx];
    if (!w->used || !w->surf.px || !W.hooks.present) return;
    /* Only the plugin's editor window reaches the screen; our container is
     * just somewhere for it to live. */
    if (wndidx != (W.display ? W.display : W.host)) return;
    W.hooks.present(W.hooks.ud, w->surf.px, w->surf.w, w->surf.h);
}

static MS void *st_GetDC(void *hwnd)
{
    W.n_getdc++;
    w32_wnd *w = w32_wget(hwnd);
    return w32_dc_new(w ? (int)(w - W.wnd) : 0);
}
static MS void *st_GetWindowDC(void *hwnd) { return st_GetDC(hwnd); }
static MS int32_t st_ReleaseDC(void *hwnd, void *hdc)
{
    w32_dc *d = w32_dcget(hdc);
    (void)hwnd;
    if (!d) return 0;
    if (d->wnd && !d->bitmap) w32_present(d->wnd);
    d->used = 0;
    return 1;
}
static MS void *st_CreateCompatibleDC(void *hdc)
{ void *r; (void)hdc; r = w32_dc_new(0); PLOG("  [w32] CreateCompatibleDC -> %p\n", r); return r; }
static MS int32_t st_DeleteDC(void *hdc)
{
    w32_dc *d = w32_dcget(hdc);
    if (!d) return 0;
    d->used = 0;
    return 1;
}

/* How many paints are open right now.
 *
 * A frame is only whole between EndPaint and the next BeginPaint. The bridge
 * helper copies the window surface on a timer and also from inside the plugin,
 * so without this it can copy halfway through a repaint and publish a frame that
 * is part old and part new -- the same artefact as a torn transfer, but arising
 * at the source, where the sequence lock on the shared buffer cannot see it. */
static int g_w32_paint_depth;
int w32_paint_in_progress(void);
int w32_paint_in_progress(void) { return g_w32_paint_depth > 0; }

static MS void *st_BeginPaint(void *hwnd, W32PAINTSTRUCT *ps)
{
    W.n_beginpaint++;
    PLOG("  [w32] BeginPaint hwnd=%p\n", hwnd);
    w32_wnd *w = w32_wget(hwnd);
    void *hdc;
    if (!w || !ps) return NULL;
    hdc = w32_dc_new((int)(w - W.wnd));
    memset(ps, 0, sizeof *ps);
    ps->hdc = hdc;
    ps->rcPaint = w->has_update ? w->update
                                : (W32RECT){ 0, 0, w->w, w->h };
    {
        w32_dc *d = w32_dcget(hdc);
        if (d) d->in_paint = 1;
        g_w32_paint_depth++;
    }
    return hdc;
}
static MS int32_t st_EndPaint(void *hwnd, const W32PAINTSTRUCT *ps)
{
    w32_wnd *w = w32_wget(hwnd);
    w32_dc *d = ps ? w32_dcget(ps->hdc) : NULL;
    if (w) { w->has_update = 0; memset(&w->update, 0, sizeof w->update); }
    if (d) { int wi = d->wnd; d->used = 0; w32_present(wi); }
    if (g_w32_paint_depth > 0) g_w32_paint_depth--;
    return 1;
}
static MS int32_t st_InvalidateRect(void *hwnd, const W32RECT *r, int32_t erase)
{
    w32_wnd *w = w32_wget(hwnd);
    (void)erase;
    if (!w) return 0;
    if (!r) {
        w->update.left = 0; w->update.top = 0;
        w->update.right = w->w; w->update.bottom = w->h;
    } else if (!w->has_update) {
        w->update = *r;
    } else {                                     /* union with what is pending */
        if (r->left   < w->update.left)   w->update.left   = r->left;
        if (r->top    < w->update.top)    w->update.top    = r->top;
        if (r->right  > w->update.right)  w->update.right  = r->right;
        if (r->bottom > w->update.bottom) w->update.bottom = r->bottom;
    }
    w->has_update = 1;
    return 1;
}
static MS int32_t st_ValidateRect(void *hwnd, const W32RECT *r)
{
    w32_wnd *w = w32_wget(hwnd);
    (void)r;
    if (w) w->has_update = 0;
    return 1;
}
static MS int32_t st_GetUpdateRect(void *hwnd, W32RECT *r, int32_t erase)
{
    w32_wnd *w = w32_wget(hwnd);
    (void)erase;
    if (!w) return 0;
    if (r) *r = w->update;
    return w->has_update;
}
/* Hand back the window's invalid area as a real region. iPlug2 reads its bounds
 * and draws only that; an empty or NULLREGION answer means it draws nothing. */
static MS int32_t st_GetUpdateRgn(void *hwnd, void *rgn, int32_t erase)
{
    w32_wnd *w = w32_wget(hwnd);
    w32_obj *o = w32_oget(rgn);
    W32RECT r;

    (void)erase;
    if (!w) return W32_RGN_ERROR;
    if (w->has_update) r = w->update;
    else { r.left = 0; r.top = 0; r.right = 0; r.bottom = 0; }
    /* Clamp to the window: a stale invalid rect from before a resize would
     * otherwise ask the plugin to draw outside its own surface. */
    if (r.right > w->w) r.right = w->w;
    if (r.bottom > w->h) r.bottom = w->h;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (o && o->kind == OBJ_RGN) o->rc = r;
    PLOG("  [w32] GetUpdateRgn -> %d,%d..%d,%d type=%d rgn=%p\n", r.left, r.top, r.right,
         r.bottom, (r.right > r.left && r.bottom > r.top) ? W32_RGN_SIMPLE : W32_RGN_NULL, rgn);
    return (r.right > r.left && r.bottom > r.top) ? W32_RGN_SIMPLE : W32_RGN_NULL;
}
static MS int32_t st_UpdateWindow(void *hwnd)
{
    w32_wnd *w = w32_wget(hwnd);
    if (w && w->has_update) w32_call(w, WM_PAINT, 0, 0);
    return 1;
}
static MS int32_t st_RedrawWindow(void *hwnd, const W32RECT *r, void *rgn, uint32_t f)
{ (void)rgn;(void)f; st_InvalidateRect(hwnd, r, 1); return 1; }

/* ---- the pixel path ----------------------------------------------------- */

/* The one call that matters: the plugin hands over a finished 32-bit image and
 * asks for it on screen. Everything above exists to make this reachable. */
/* SetDIBitsToDevice: an unscaled blit from a DIB straight to a device.
 *
 * This is how both FM8 and Kontakt put their interface on screen -- they render
 * into a DIB section in software and then hand it over in one call. Unimplemented,
 * they painted perfectly and nothing arrived: WM_PAINT ran, BeginPaint ran, and
 * the window stayed black.
 *
 * Differences from StretchDIBits that matter: there is no scaling, and the source
 * is addressed from the *bottom* of the DIB for a bottom-up bitmap. `startScan`
 * and `scanLines` describe which band of the DIB `bits` actually covers, so a
 * caller may hand over one strip of a taller image; ignoring that draws the wrong
 * rows. */
static MS int32_t st_SetDIBitsToDevice(void *hdc, int32_t xd, int32_t yd,
                                      uint32_t w, uint32_t h,
                                      int32_t xs, int32_t ys,
                                      uint32_t startScan, uint32_t scanLines,
                                      const void *bits, const void *bmi,
                                      uint32_t usage)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp;
    w32_surf *t = w32_target_in(d, &ttmp);
    const W32BITMAPINFOHEADER *bh = bmi;
    const uint32_t *src = bits;
    int sw, sh, topdown, row, col;

    (void)usage;
    if (!t || !t->px || !bh || !src) return 0;
    W.n_stretch++;
    PLOG("  [w32] SetDIBitsToDevice %ux%u -> dc%s at %d,%d src %d,%d "
         "scan %u+%u (%d bpp)\n",
         w, h, d->bitmap ? " (memory)" : " (window)", xd, yd, xs, ys,
         startScan, scanLines, bh->biBitCount);
    if (bh->biBitCount != 32) return 0;
    sw = bh->biWidth;
    sh = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    topdown = bh->biHeight < 0;
    if (sw <= 0 || sh <= 0 || (int32_t)w <= 0 || (int32_t)h <= 0) return 0;
    if (scanLines == 0) scanLines = (uint32_t)sh;

    for (row = 0; row < (int)h; row++) {
        int ty = yd + row;
        int sy = ys + row;                 /* same scale, so row for row */
        int band;
        const uint32_t *srow;
        uint32_t *drow;
        if (ty < 0 || ty >= t->h) continue;
        if (sy < 0 || sy >= sh) continue;
        /* Which line of the supplied buffer holds source row sy. For a bottom-up
         * DIB the buffer runs upward from the bottom of the image. */
        band = topdown ? sy - (int)startScan : (sh - 1 - sy) - (int)startScan;
        if (band < 0 || band >= (int)scanLines) continue;
        srow = src + (size_t)band * sw;
        drow = t->px + (size_t)ty * t->w;
        for (col = 0; col < (int)w; col++) {
            int tx = xd + col, sx = xs + col;
            if (tx < 0 || tx >= t->w || sx < 0 || sx >= sw) continue;
            drow[tx] = srow[sx];
        }
    }
    if (d->wnd && !d->bitmap && !d->in_paint) w32_present(d->wnd);
    return (int32_t)h;
}

/* The clip box, which a renderer uses to skip work outside it. Reporting an empty
 * one tells it everything is clipped away and it draws nothing at all. */
static MS int32_t st_GetClipBox(void *hdc, W32RECT *r)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp;
    w32_surf *t = w32_target_in_raw(d, &ttmp);

    if (!r) return 0;
    r->left = 0; r->top = 0;
    r->right  = t ? t->w : 0;
    r->bottom = t ? t->h : 0;
    /* Honour a clip the caller set. Reporting the whole surface while
     * IntersectClipRect quietly recorded something smaller is not a harmless
     * over-estimate: a caller that clips its own drawing against this answer
     * concludes it may write anywhere on the surface, and writes there.
     *
     * SynthEdit composes through 128x128 tiles and asks for this to bound each
     * one. Given the whole surface it never clamped, and its alpha blit ran
     * twelve rows off the end of a tile into the next allocation -- which is
     * what had MFC faulting inside its own handle map, a subsystem away. */
    if (d && d->has_clip) {
        if (d->clip.left   > r->left)   r->left   = d->clip.left;
        if (d->clip.top    > r->top)    r->top    = d->clip.top;
        if (d->clip.right  < r->right)  r->right  = d->clip.right;
        if (d->clip.bottom < r->bottom) r->bottom = d->clip.bottom;
    }
    /* Back into the caller's space: it draws in logical coordinates, and the
     * viewport origin is what separates those from the surface. */
    if (d) {
        r->left -= d->org_x; r->right  -= d->org_x;
        r->top  -= d->org_y; r->bottom -= d->org_y;
    }
    if (!t) return 0;
    return w32_rect_empty(r) ? W32_RGN_NULL : W32_RGN_SIMPLE;
}

/* ---- clipping and the viewport origin ------------------------------------ */

/* Whether any of a rectangle would land inside the clip box.
 *
 * A renderer asks this before drawing each element so it can skip the ones that
 * are off-screen, and it does not check for failure -- there is nothing to fail.
 * So a stub returning 0 does not degrade anything: it says "none of your
 * interface is visible", and a well-written plugin dutifully draws none of it.
 * This host had no RectVisible, and that alone accounted for an MFC editor
 * making zero drawing calls into a window it had correctly created and sized. */
static MS int32_t st_RectVisible(void *hdc, const W32RECT *r)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp;
    w32_surf *t;
    W32RECT box, want;

    if (!d || !r) return 0;
    t = w32_target_in_raw(d, &ttmp);
    if (!t) return 0;
    box.left = 0; box.top = 0; box.right = t->w; box.bottom = t->h;
    if (d->has_clip) {
        if (d->clip.left   > box.left)   box.left   = d->clip.left;
        if (d->clip.top    > box.top)    box.top    = d->clip.top;
        if (d->clip.right  < box.right)  box.right  = d->clip.right;
        if (d->clip.bottom < box.bottom) box.bottom = d->clip.bottom;
    }
    /* The caller's rectangle is in its own space, which the viewport origin
     * translates out of. */
    want = *r;
    want.left += d->org_x; want.right  += d->org_x;
    want.top  += d->org_y; want.bottom += d->org_y;
    return !(want.right <= box.left || want.left >= box.right ||
             want.bottom <= box.top || want.top  >= box.bottom);
}

#define W32_NULLREGION   1
#define W32_SIMPLEREGION 2

static MS int32_t st_IntersectClipRect(void *hdc, int32_t l, int32_t t,
                                       int32_t r, int32_t b)
{
    w32_dc *d = w32_dcget(hdc);
    if (!d) return 0;
    l += d->org_x; r += d->org_x; t += d->org_y; b += d->org_y;
    if (!d->has_clip) {
        d->clip.left = l; d->clip.top = t; d->clip.right = r; d->clip.bottom = b;
        d->has_clip = 1;
    } else {
        if (l > d->clip.left)   d->clip.left   = l;
        if (t > d->clip.top)    d->clip.top    = t;
        if (r < d->clip.right)  d->clip.right  = r;
        if (b < d->clip.bottom) d->clip.bottom = b;
    }
    return w32_rect_empty(&d->clip) ? W32_NULLREGION : W32_SIMPLEREGION;
}

/* A rectangle taken *out* of the clip region cannot be expressed as one box, and
 * a wrong box here is worse than none: narrowing the clip to the hole would stop
 * everything else being drawn. Accepted and not applied, which costs at most
 * some overdraw the next paint covers. */
static MS int32_t st_ExcludeClipRect(void *hdc, int32_t l, int32_t t,
                                     int32_t r, int32_t b)
{ (void)l;(void)t;(void)r;(void)b; return w32_dcget(hdc) ? W32_SIMPLEREGION : 0; }

static MS int32_t st_SetViewportOrgEx(void *hdc, int32_t x, int32_t y, W32POINT *old)
{
    w32_dc *d = w32_dcget(hdc);
    if (!d) return 0;
    if (old) { old->x = d->org_x; old->y = d->org_y; }
    d->org_x = x; d->org_y = y;
    return 1;
}
static MS int32_t st_OffsetViewportOrgEx(void *hdc, int32_t dx, int32_t dy, W32POINT *old)
{
    w32_dc *d = w32_dcget(hdc);
    if (!d) return 0;
    if (old) { old->x = d->org_x; old->y = d->org_y; }
    d->org_x += dx; d->org_y += dy;
    return 1;
}
/* Always writes the point, even when the DC is not one we know.
 *
 * A caller reads this back to work out where in its own buffer to draw, and
 * does not check the return value -- there is nothing it could do about a
 * failure anyway. Leaving the POINT untouched therefore does not report an
 * error, it hands back whatever was on the caller's stack, and the index
 * computed from it lands wherever that garbage points.
 *
 * SynthEdit's alpha blit is exactly this caller. It composes into a fixed
 * 128x128 tile at a stride of 128 pixels:
 *
 *     GetViewportOrgEx(dc, &org);
 *     dst = tile + (((0x7f - org.y) - y) * 0x80 + org.x + x) * 4;
 *
 * so an uninitialised org is a wild write into whatever follows that tile. It
 * is the corruption that had MFC faulting in its own handle map, several
 * subsystems away. Zero is the honest answer for a DC with no origin set, and
 * it is what Windows would have left there. */
static MS int32_t st_GetViewportOrgEx(void *hdc, W32POINT *p)
{
    w32_dc *d = w32_dcget(hdc);
    if (!p) return 0;
    p->x = d ? d->org_x : 0;
    p->y = d ? d->org_y : 0;
    return d != NULL;
}
/* The window origin is the same translation with the opposite sign: it names the
 * logical point that maps to the viewport origin. Only one of the two is used by
 * anything here, but a plugin that sets this one and not the other still expects
 * its drawing to move. */
static MS int32_t st_SetWindowOrgEx(void *hdc, int32_t x, int32_t y, W32POINT *old)
{
    w32_dc *d = w32_dcget(hdc);
    if (!d) return 0;
    if (old) { old->x = -d->org_x; old->y = -d->org_y; }
    d->org_x = -x; d->org_y = -y;
    return 1;
}
static MS int32_t st_GetWindowOrgEx(void *hdc, W32POINT *p)
{
    w32_dc *d = w32_dcget(hdc);
    if (!p) return 0;                             /* see st_GetViewportOrgEx */
    p->x = d ? -d->org_x : 0;
    p->y = d ? -d->org_y : 0;
    return d != NULL;
}

static MS int32_t st_GdiFlush(void) { return 1; }

static MS int32_t st_StretchDIBits(void *hdc, int32_t xd, int32_t yd, int32_t wd, int32_t hd,
                                  int32_t xs, int32_t ys, int32_t ws, int32_t hs,
                                  const void *bits, const void *bmi, uint32_t usage, uint32_t rop)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp;
    w32_surf *t = w32_target_in(d, &ttmp);
    const W32BITMAPINFOHEADER *bh = bmi;
    const uint32_t *src = bits;
    int sw, sh, topdown, dy, dx;

    (void)usage; (void)rop;
    if (!t || !t->px || !bh || !src) return 0;
    W.n_stretch++;
    PLOG("  [w32] StretchDIBits %dx%d -> dc%s at %d,%d %dx%d (%d bpp)\n",
         bh->biWidth, bh->biHeight, d->bitmap ? " (memory)" : " (window)",
         xd, yd, wd, hd, bh->biBitCount);
    if (bh->biBitCount != 32) return 0;           /* Skia always gives us 32bpp */

    sw = bh->biWidth;
    sh = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    topdown = bh->biHeight < 0;
    if (sw <= 0 || sh <= 0 || wd <= 0 || hd <= 0) return 0;

    for (dy = 0; dy < hd; dy++) {
        int ty = yd + dy;
        int sy;
        const uint32_t *srow;
        uint32_t *drow;
        if (ty < 0 || ty >= t->h) continue;
        sy = ys + (hs == hd ? dy : (int)((int64_t)dy * hs / hd));
        if (sy < 0 || sy >= sh) continue;
        srow = src + (size_t)(topdown ? sy : sh - 1 - sy) * sw;
        drow = t->px + (size_t)ty * t->w;
        if (ws == wd) {
            for (dx = 0; dx < wd; dx++) {
                int tx = xd + dx, sx = xs + dx;
                if (tx < 0 || tx >= t->w || sx < 0 || sx >= sw) continue;
                drow[tx] = srow[sx];
            }
        } else {
            for (dx = 0; dx < wd; dx++) {
                int tx = xd + dx;
                int sx = xs + (int)((int64_t)dx * ws / wd);
                if (tx < 0 || tx >= t->w || sx < 0 || sx >= sw) continue;
                drow[tx] = srow[sx];
            }
        }
    }
    if (d->wnd && !d->bitmap && !d->in_paint) w32_present(d->wnd);
    return hd;
}

/* Raster operations.
 *
 * BitBlt took a rop and discarded it, which is SRCCOPY for everything. That is
 * right for the great majority of blits and wrong for the one pattern every
 * GDI-era plug-in uses to draw a shape with transparency: the mask is blitted
 * with SRCAND to punch a hole, then the artwork with SRCPAINT to fill it. Done
 * as two SRCCOPYs, the second simply overwrites the first and whatever was
 * underneath is gone.
 *
 * The ternary rop codes encode a whole truth table in their high byte; these
 * are the fifteen with names, which is what callers actually pass. */
#define W32_SRCCOPY     0x00CC0020u
#define W32_SRCPAINT    0x00EE0086u
#define W32_SRCAND      0x008800C6u
#define W32_SRCINVERT   0x00660046u
#define W32_SRCERASE    0x00440328u
#define W32_NOTSRCCOPY  0x00330008u
#define W32_NOTSRCERASE 0x001100A6u
#define W32_MERGECOPY   0x00C000CAu
#define W32_MERGEPAINT  0x00BB0226u
#define W32_PATCOPY     0x00F00021u
#define W32_PATPAINT    0x00FB0A09u
#define W32_PATINVERT   0x005A0049u
#define W32_DSTINVERT   0x00550009u
#define W32_BLACKNESS   0x00000042u
#define W32_WHITENESS   0x00FF0062u

/* Whether the operation reads the source at all. The ones that do not must not
 * be clipped away just because the source rectangle is off its bitmap. */
static int w32_rop_needs_src(uint32_t rop)
{
    switch (rop) {
    case W32_PATCOPY: case W32_PATINVERT: case W32_DSTINVERT:
    case W32_BLACKNESS: case W32_WHITENESS:
        return 0;
    default:
        return 1;
    }
}

static uint32_t w32_rop_apply(uint32_t rop, uint32_t s, uint32_t d, uint32_t p)
{
    switch (rop) {
    case W32_SRCCOPY:     return s;
    case W32_SRCPAINT:    return d | s;
    case W32_SRCAND:      return d & s;
    case W32_SRCINVERT:   return d ^ s;
    case W32_SRCERASE:    return d & ~s;
    case W32_NOTSRCCOPY:  return ~s;
    case W32_NOTSRCERASE: return ~(d | s);
    case W32_MERGECOPY:   return s & p;
    case W32_MERGEPAINT:  return d | ~s;
    case W32_PATCOPY:     return p;
    case W32_PATPAINT:    return d | p | ~s;
    case W32_PATINVERT:   return d ^ p;
    case W32_DSTINVERT:   return ~d;
    case W32_BLACKNESS:   return 0;
    case W32_WHITENESS:   return 0x00FFFFFFu;
    /* An unnamed ternary rop is rare enough that copying the source is a better
     * answer than refusing to draw, and it is what this did for every rop
     * until now. */
    default:              return s;
    }
}

static MS int32_t st_BitBlt(void *dst, int32_t x, int32_t y, int32_t w, int32_t h,
                            void *src, int32_t sx, int32_t sy, uint32_t rop)
{
    w32_dc *dd = w32_dcget(dst), *sd = w32_dcget(src);
    w32_surf dtmp, stmp;
    w32_surf *ds = w32_target_in(dd, &dtmp), *ss = w32_source_in(sd, &stmp);
    int j, i;

    W.n_bitblt++;
    if (!ds || !ds->px) return 0;
    if (!ss || !ss->px) return 0;
    /* Both DCs' viewport origins apply, each to its own coordinates.
     *
     * Leaving them out is not a small offset error. SynthEdit composes its
     * panel as forty 128x128 tiles through one small bitmap, moving the origin
     * to bring each tile's region into it -- so a source offset of 128,0 with
     * an origin of -128,0 means "the start of the tile", and taken literally it
     * is 128 pixels off the end of a 128-wide bitmap, which clips away to
     * nothing. Every tile but the first came out empty, which looks exactly
     * like a plug-in that stopped drawing after the first control. */
    x += dd->org_x;  y += dd->org_y;
    sx += sd->org_x; sy += sd->org_y;
    {
        /* A monochrome source is expanded through the destination DC's colours:
         * a 0 bit takes the text colour and a 1 bit the background colour,
         * which is how an application paints a shape in the colours the DC is
         * set to rather than in black and white. */
        int mono = sd->bitmap && W.obj[sd->bitmap].mono && !(dd->bitmap &&
                   W.obj[dd->bitmap].mono);
        uint32_t fg = w32_cr(dd->text_color), bg = w32_cr(dd->bk_color);
        uint32_t pat = w32_brush_on(dd) ? w32_brush_rgb(dd) : 0;

        for (j = 0; j < h; j++) {
            int ty = y + j, fy = sy + j;
            if (ty < 0 || ty >= ds->h) continue;
            for (i = 0; i < w; i++) {
                int tx = x + i, fx = sx + i;
                uint32_t sv = 0, dv, out;
                if (tx < 0 || tx >= ds->w) continue;
                if (w32_rop_needs_src(rop)) {
                    if (fy < 0 || fy >= ss->h || fx < 0 || fx >= ss->w) continue;
                    sv = ss->px[(size_t)fy * ss->w + fx];
                    if (mono) sv = sv ? bg : fg;
                }
                dv = ds->px[(size_t)ty * ds->w + tx];
                out = w32_rop_apply(rop, sv, dv, pat);
                ds->px[(size_t)ty * ds->w + tx] = out & 0x00FFFFFFu;
            }
        }
    }
    w32_dib_out(dd, x, y, x + w, y + h);
    PLOG("  [w32] BitBlt %dx%d to %s#%d at %d,%d from %s#%d at %d,%d\n", w, h,
         dd->bitmap ? "bmp" : "wnd", dd->bitmap ? dd->bitmap : dd->wnd, x, y,
         sd->bitmap ? "bmp" : "wnd", sd->bitmap ? sd->bitmap : sd->wnd, sx, sy);
    if (dd->wnd && !dd->bitmap && !dd->in_paint) w32_present(dd->wnd);
    return 1;
}

static MS void *st_CreateCompatibleBitmap(void *hdc, int32_t w, int32_t h)
{
    void *r;
    (void)hdc;
    /* Windows gives a 1x1 monochrome bitmap for a zero size rather than
     * nothing; handing back a bitmap with no pixels invites a null write. */
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    { int ix = w32_i(W32_OBJ_BASE, (r = w32_h(W32_OBJ_BASE,
                                    w32_obj_new(OBJ_BITMAP, w, h, 0))));
      PLOG("  [w32] CreateCompatibleBitmap bmp#%d %dx%d\n", ix, w, h); }
    return r;
}
/* SetDIBits and its relatives: putting DIB pixels into a device bitmap.
 *
 * A plug-in that builds a picture in memory and then wants GDI to blit it
 * creates the bitmap with CreateBitmap and fills it with these -- none of which
 * existed here, so the bitmap stayed the zeroes it was created with and every
 * blit from it drew black. That is how MinimogueVA's background panel came out
 * black with its whole interface drawn on top.
 *
 * Rows are DWORD-aligned, and bottom-up unless the height is negative, which is
 * the same convention w32_bitmap_from_dib decodes and the opposite of the
 * WORD-aligned top-down bits CreateBitmap itself takes. */
static int w32_dib_into_obj(w32_obj *o, const void *bmi, const void *bits,
                            uint32_t start, uint32_t lines, uint32_t usage)
{
    const uint8_t *h = (const uint8_t *)bmi;
    const uint8_t *pal, *rows;
    uint32_t hdrsz, comp, clrused, stride, pal_n;
    int32_t w, ht, y, x;
    uint16_t bpp;
    int flip = 1;

    if (!o || o->kind != OBJ_BITMAP || !o->px || !bmi || !bits) return 0;
    memcpy(&hdrsz, h + 0, 4);
    if (hdrsz < 40) return 0;
    memcpy(&w, h + 4, 4);
    memcpy(&ht, h + 8, 4);
    memcpy(&bpp, h + 14, 2);
    memcpy(&comp, h + 16, 4);
    memcpy(&clrused, h + 32, 4);
    if (ht < 0) { ht = -ht; flip = 0; }
    if (w <= 0 || ht <= 0 || comp != 0) return 0;

    pal_n = clrused ? clrused : (bpp <= 8 ? (1u << bpp) : 0);
    pal = h + hdrsz;
    rows = (const uint8_t *)bits;
    stride = ((uint32_t)w * bpp + 31) / 32 * 4;
    if (start + lines > (uint32_t)ht) lines = (uint32_t)ht - start;

    for (y = 0; y < (int32_t)lines; y++) {
        int32_t sy = (int32_t)start + y;                 /* row within the DIB */
        int32_t dy = flip ? ht - 1 - sy : sy;            /* row in our surface */
        const uint8_t *src = rows + (size_t)y * stride;
        uint32_t *dst;
        if (dy < 0 || dy >= o->h) continue;
        dst = o->px + (size_t)dy * o->w;
        for (x = 0; x < w && x < o->w; x++) {
            uint32_t p;
            switch (bpp) {
            case 32: memcpy(&dst[x], src + x * 4, 4); dst[x] &= 0x00ffffffu; continue;
            case 24: dst[x] = ((uint32_t)src[x * 3 + 2] << 16) |
                              ((uint32_t)src[x * 3 + 1] << 8) | src[x * 3 + 0]; continue;
            case 16: { uint16_t v; memcpy(&v, src + x * 2, 2);
                       dst[x] = ((uint32_t)(((v >> 10) & 0x1f) * 255 / 31) << 16) |
                                ((uint32_t)(((v >> 5)  & 0x1f) * 255 / 31) << 8)  |
                                 (uint32_t)((  v        & 0x1f) * 255 / 31); continue; }
            case 8:  p = src[x]; break;
            case 4:  p = (x & 1) ? (src[x >> 1] & 0xf) : (uint32_t)(src[x >> 1] >> 4); break;
            case 1:  p = (src[x >> 3] >> (7 - (x & 7))) & 1; break;
            default: return 0;
            }
            /* DIB_PAL_COLORS indexes the DC's palette, which this layer does
             * not keep; the table that follows the header is the other case
             * and the one everything here uses. */
            if (usage == 0 && pal_n && p < pal_n)
                dst[x] = ((uint32_t)pal[p * 4 + 2] << 16) |
                         ((uint32_t)pal[p * 4 + 1] << 8) | pal[p * 4 + 0];
            else
                dst[x] = p ? 0xFFFFFFu : 0;
        }
    }
    o->px_drawn = 1;
    return (int)lines;
}

static MS int32_t st_SetDIBits(void *hdc, void *hbm, uint32_t start, uint32_t lines,
                               const void *bits, const void *bmi, uint32_t usage)
{
    w32_obj *o = w32_oget(hbm);
    int n;
    (void)hdc;
    n = w32_dib_into_obj(o, bmi, bits, start, lines, usage);
    PLOG("  [w32] SetDIBits %u lines -> bmp %p (%d done)\n", lines, hbm, n);
    return n;
}

/* CreateDIBitmap(hdc, header, init, bits, bmi, usage): a device bitmap made
 * from DIB data, which is CreateBitmap and SetDIBits in one call. */
#define W32_CBM_INIT 4
static MS void *st_CreateDIBitmap(void *hdc, const void *hdr, uint32_t init,
                                  const void *bits, const void *bmi, uint32_t usage)
{
    int32_t w = 0, ht = 0;
    int idx, flip;
    (void)hdc;
    if (!hdr) return NULL;
    memcpy(&w, (const uint8_t *)hdr + 4, 4);
    memcpy(&ht, (const uint8_t *)hdr + 8, 4);
    flip = ht < 0 ? -1 : 1;
    if (flip < 0) ht = -ht;
    if (w <= 0 || ht <= 0) return NULL;
    if (!(idx = w32_obj_new(OBJ_BITMAP, w, ht, 0))) return NULL;
    PLOG("  [w32] CreateDIBitmap bmp#%d %dx%d\n", idx, w, ht);
    if ((init & W32_CBM_INIT) && bits && bmi)
        w32_dib_into_obj(&W.obj[idx], bmi, bits, 0, (uint32_t)ht, usage);
    return w32_h(W32_OBJ_BASE, idx);
}

/* The reverse: our pixels back out as a DIB. A caller passing a null buffer is
 * asking for the header to be filled in, which is how it learns the size. */
static MS int32_t st_GetDIBits(void *hdc, void *hbm, uint32_t start, uint32_t lines,
                               void *bits, void *bmi, uint32_t usage)
{
    w32_obj *o = w32_oget(hbm);
    uint8_t *h = (uint8_t *)bmi;
    uint32_t hdrsz, stride;
    uint16_t bpp;
    int32_t ht;
    uint32_t y;

    (void)hdc; (void)usage;
    if (!o || o->kind != OBJ_BITMAP || !h) return 0;
    memcpy(&hdrsz, h, 4);
    if (hdrsz < 40) return 0;
    if (!bits) {                                   /* describe the bitmap */
        int32_t w = o->w, hh = o->h;
        uint16_t planes = 1, b = 32;
        uint32_t comp = 0, szimg = (uint32_t)o->w * o->h * 4;
        memcpy(h + 4, &w, 4); memcpy(h + 8, &hh, 4);
        memcpy(h + 12, &planes, 2); memcpy(h + 14, &b, 2);
        memcpy(h + 16, &comp, 4); memcpy(h + 20, &szimg, 4);
        return o->h;
    }
    memcpy(&bpp, h + 14, 2);
    memcpy(&ht, h + 8, 4);
    if (bpp != 32 && bpp != 24) return 0;           /* what a caller asks for */
    stride = ((uint32_t)o->w * bpp + 31) / 32 * 4;
    if (start + lines > (uint32_t)o->h) lines = (uint32_t)o->h - start;
    for (y = 0; y < lines; y++) {
        uint32_t sy = start + y;
        uint32_t dy = ht > 0 ? (uint32_t)o->h - 1 - sy : sy;   /* bottom-up */
        const uint32_t *src;
        uint8_t *dst = (uint8_t *)bits + (size_t)y * stride;
        int x;
        if (dy >= (uint32_t)o->h) continue;
        src = o->px + (size_t)dy * o->w;
        for (x = 0; x < o->w; x++) {
            if (bpp == 32) { uint32_t v = src[x]; memcpy(dst + x * 4, &v, 4); }
            else { dst[x * 3 + 0] = (uint8_t)src[x];
                   dst[x * 3 + 1] = (uint8_t)(src[x] >> 8);
                   dst[x * 3 + 2] = (uint8_t)(src[x] >> 16); }
        }
    }
    return (int32_t)lines;
}

/* The bitmap a plugin can draw into itself.
 *
 * This is how an application gets at pixels directly, and every GDI-era editor
 * in this corpus reaches for it: make a DIB section, select it into a memory DC,
 * draw both with GDI and by writing the returned pointer, then blit. Returning
 * NULL is not a soft failure -- a caller reads it as out of memory, and one
 * built on MFC turns that into `throw CMemoryException` a long way from here.
 * That is exactly how the first 32-bit plugin tried against this host died.
 *
 * hSection is refused rather than half-honoured: mapping the caller's own file
 * mapping would mean sharing pages we do not own, and a plugin that passes one
 * is better told no than handed a private buffer it thinks is shared. Nothing
 * in this corpus passes one. */
static MS void *st_CreateDIBSection(void *hdc, const void *bmi, uint32_t usage,
                                    void **bits, void *section, uint32_t offset)
{
    const uint8_t *h = (const uint8_t *)bmi;
    uint32_t hdrsz, comp, clrused, stride;
    int32_t w, ht;
    uint16_t bpp;
    int idx, flip = 1, alias, i;
    w32_obj *o;

    (void)hdc; (void)offset;
    if (bits) *bits = NULL;
    if (!bmi || section) return NULL;

    memcpy(&hdrsz, h + 0, 4);
    if (hdrsz < 40) return NULL;
    memcpy(&w,   h + 4,  4);
    memcpy(&ht,  h + 8,  4);
    memcpy(&bpp, h + 14, 2);
    memcpy(&comp, h + 16, 4);
    memcpy(&clrused, h + 32, 4);
    if (ht < 0) { ht = -ht; flip = 0; }
    if (w <= 0 || ht <= 0 || w > 16384 || ht > 16384) return NULL;
    if (comp != 0 && comp != 3) return NULL;            /* BI_RGB, BI_BITFIELDS */
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return NULL;

    stride = ((uint32_t)w * bpp + 31) / 32 * 4;
    /* 32 bits per pixel top-down is byte-for-byte what we keep anyway, so the
     * plugin writes into our own surface and no conversion ever runs. */
    alias = (bpp == 32 && !flip && stride == (uint32_t)w * 4);

    if (!(idx = w32_obj_new(OBJ_BITMAP, w, ht, 0))) return NULL;
    o = &W.obj[idx];
    o->dib_bpp = bpp; o->dib_stride = (int)stride; o->dib_flip = flip;
    o->dib_alias = alias;

    if (alias) {
        o->dib = (uint8_t *)o->px;
    } else if (!(o->dib = calloc((size_t)ht, stride))) {
        free(o->px); memset(o, 0, sizeof *o); return NULL;
    }

    /* 16-bit has no default: BI_RGB means 5-5-5, and the 5-6-5 that hardware
     * actually prefers arrives as BI_BITFIELDS with the masks spelled out. */
    if (bpp == 16 && comp == 3) {
        uint32_t rmask; memcpy(&rmask, h + hdrsz, 4);
        o->dib_565 = (rmask == 0xF800u);
    }
    if (bpp <= 8) {
        const uint8_t *pal = h + hdrsz;
        int n = (int)(clrused ? clrused : (1u << bpp));
        if (n > 256) n = 256;
        o->dib_pal_n = n;
        /* DIB_PAL_COLORS indexes the DC's palette rather than carrying colours.
         * Nothing here keeps one, so those entries stay black -- which is what
         * the call would produce against a default palette anyway. */
        if (usage == 0)
            for (i = 0; i < n; i++)
                o->dib_pal[i] = ((uint32_t)pal[i * 4 + 2] << 16) |
                                ((uint32_t)pal[i * 4 + 1] << 8) | pal[i * 4 + 0];
    }

    if (bits) *bits = o->dib;
    PLOG("  [w32] CreateDIBSection bmp#%d %dx%d %ubpp %s%s -> bits %p\n",
         idx, w, ht, bpp, flip ? "bottom-up" : "top-down",
         alias ? " (aliased)" : "", (void *)o->dib);
    return w32_h(W32_OBJ_BASE, idx);
}

/* CreateBitmap(w, h, planes, bitsPerPixel, bits) -- and the bits are the point.
 *
 * Discarding them returned a bitmap of the right size full of zeroes, which is
 * indistinguishable from success until something blits it: MinimogueVA builds
 * its background panel this way and drew its whole interface onto black.
 *
 * The rows are packed to a two-byte boundary here, not the four a DIB uses --
 * CreateBitmap takes device-dependent bits, and getting that wrong shears the
 * picture by a pixel per row rather than failing. */
static MS void *st_CreateBitmap(int32_t w, int32_t h, uint32_t planes,
                                uint32_t bpp, const void *bits)
{
    int idx, x, y;
    w32_obj *o;
    uint32_t stride;
    const uint8_t *src = (const uint8_t *)bits;

    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (!(idx = w32_obj_new(OBJ_BITMAP, w, h, 0))) return NULL;
    o = &W.obj[idx];
    PLOG("  [w32] CreateBitmap bmp#%d %dx%d %u planes %ubpp bits=%p\n",
         idx, w, h, planes, bpp, bits);
    /* A one-bit bitmap is a mask. Windows does not blit it as pixels: a 0 bit
     * takes the destination's text colour and a 1 bit its background colour,
     * which is how an application paints a shape in whatever colour the DC is
     * set to rather than in black and white. */
    o->mono = (bpp == 1 && planes == 1);
    if (!src || planes != 1) return w32_h(W32_OBJ_BASE, idx);

    stride = ((uint32_t)w * bpp + 15) / 16 * 2;
    for (y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *dst = o->px + (size_t)y * w;
        for (x = 0; x < w; x++) {
            switch (bpp) {
            case 32: memcpy(&dst[x], row + x * 4, 4); dst[x] &= 0x00ffffffu; break;
            case 24: dst[x] = ((uint32_t)row[x * 3 + 2] << 16) |
                              ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3 + 0]; break;
            case 16: { uint16_t v; memcpy(&v, row + x * 2, 2);
                       dst[x] = ((uint32_t)(((v >> 10) & 0x1f) * 255 / 31) << 16) |
                                ((uint32_t)(((v >> 5)  & 0x1f) * 255 / 31) << 8)  |
                                 (uint32_t)((  v        & 0x1f) * 255 / 31); break; }
            /* Monochrome, which is what a mask created this way is: set means
             * white. Anything deeper without a palette cannot be resolved, and
             * leaving it black is better than inventing colours. */
            case 1:  dst[x] = ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 0xFFFFFFu : 0;
                     break;
            default: y = h; x = w; break;
            }
        }
    }
    return w32_h(W32_OBJ_BASE, idx);
}

static MS void *st_SelectObject(void *hdc, void *obj)
{
    PLOG("  [w32] SelectObject dc=%p obj=%p\n", hdc, obj);
    w32_dc *d = w32_dcget(hdc);
    w32_obj *o = w32_oget(obj);
    void *prev;
    if (!d) return NULL;
    if (!o) return NULL;
    if (o->kind == OBJ_BITMAP) {
        /* Whatever was selected before is finished being drawn into: give the
         * plugin back what GDI put there, in its own layout. */
        if (d && d->bitmap && d->bitmap != (int)(o - W.obj))
            w32_dib_push(&W.obj[d->bitmap]);
        prev = d->bitmap ? w32_h(W32_OBJ_BASE, d->bitmap) : NULL;
        d->bitmap = (int)(o - W.obj);
        return prev;
    }
    if (o->kind == OBJ_FONT) {
        prev = d->font ? w32_h(W32_OBJ_BASE, d->font) : NULL;
        d->font = (int)(o - W.obj);
        return prev;
    }
    if (o->kind == OBJ_PEN) {
        prev = d->pen ? w32_h(W32_OBJ_BASE, d->pen) : NULL;
        d->pen = (int)(o - W.obj);
        return prev;
    }
    if (o->kind == OBJ_BRUSH) {
        prev = d->brush ? w32_h(W32_OBJ_BASE, d->brush) : NULL;
        d->brush = (int)(o - W.obj);
        return prev;
    }
    return obj;
}
static MS void *st_GetCurrentObject(void *hdc, uint32_t type)
{
    w32_dc *d = w32_dcget(hdc);
    (void)type;
    return (d && d->bitmap) ? w32_h(W32_OBJ_BASE, d->bitmap) : NULL;
}
static MS int32_t st_DeleteObject(void *obj)
{
    w32_obj *o = w32_oget(obj);
    if (!o) return 1;
    if (!o->dib_alias) free(o->dib);       /* aliased, px is the same block */
    free(o->px);
    memset(o, 0, sizeof *o);
    return 1;
}
static MS int32_t st_GetObjectA(void *obj, int32_t n, void *out)
{
    w32_obj *o = w32_oget(obj);
    /* BITMAP: { LONG type; LONG w; LONG h; LONG widthBytes; WORD planes; WORD bpp; void *bits; } */
    struct { int32_t type, w, h, widthBytes; uint16_t planes, bpp; void *bits; } bm;
    if (!o || !out) return 0;
    if (o->kind == OBJ_FONT) {
        int copy = o->logfont_len ? o->logfont_len : 0;
        if (copy > n) copy = n;
        if (copy > 0) memcpy(out, o->logfont, (size_t)copy);
        if (n > copy) memset((char *)out + copy, 0, (size_t)(n - copy));
        return copy ? copy : n;
    }
    if (o->kind == OBJ_BITMAP && n >= (int32_t)sizeof bm) {
        memset(&bm, 0, sizeof bm);
        bm.type = 7; bm.w = o->w; bm.h = o->h; bm.planes = 1;
        /* A DIB section has to describe the layout the plugin asked for, not
         * ours. Answering 32bpp at a stride of w*4 for a 24bpp bitmap sends the
         * caller's own row arithmetic into the wrong bytes, and pointing bmBits
         * at our internal surface rather than the buffer CreateDIBSection
         * returned means whatever it writes there is overwritten the next time
         * the DIB is read. */
        if (o->dib) {
            bm.widthBytes = o->dib_stride;
            bm.bpp = (uint16_t)o->dib_bpp;
            bm.bits = o->dib;
        } else {
            /* A device-dependent bitmap has no bits the caller may touch, and
             * Windows says so with NULL. Handing ours over invites a write at
             * whatever stride the caller assumes. */
            bm.widthBytes = o->w * 4; bm.bpp = 32; bm.bits = NULL;
        }
        memcpy(out, &bm, sizeof bm);
        return (int32_t)sizeof bm;
    }
    memset(out, 0, (size_t)n);
    return n;
}
static MS int32_t st_GetObjectW(void *o, int32_t n, void *out) { return st_GetObjectA(o, n, out); }

/* ---- GDI odds and ends Skia never actually draws with ------------------- */

static MS void *st_CreateSolidBrush(uint32_t c)
{ return w32_h(W32_OBJ_BASE, w32_obj_new(OBJ_BRUSH, 0, 0, c)); }
/* LOGBRUSH { UINT lbStyle; COLORREF lbColor; ULONG_PTR lbHatch; } -- the colour
 * is the second word at both widths. Discarding it made every brush black,
 * which on a black background is indistinguishable from not drawing. */
static MS void *st_CreateBrushIndirect(const void *lb)
{
    uint32_t style = 0, c = 0;
    if (lb) { memcpy(&style, lb, 4); memcpy(&c, (const char *)lb + 4, 4); }
    /* BS_NULL/BS_HOLLOW draws nothing; carried as a pen/brush of its own so the
     * primitives below can tell "fill with black" from "do not fill". */
    return w32_h(W32_OBJ_BASE,
                 w32_obj_new(OBJ_BRUSH, style == 1 ? -1 : 0, 0, c));
}
/* LOGPEN { UINT lopnStyle; POINT lopnWidth; COLORREF lopnColor; } */
static MS void *st_CreatePenIndirect(const void *lp)
{
    uint32_t style = 0, c = 0; int32_t wx = 1;
    if (lp) {
        memcpy(&style, lp, 4);
        memcpy(&wx, (const char *)lp + 4, 4);
        memcpy(&c, (const char *)lp + 12, 4);
    }
    return w32_h(W32_OBJ_BASE,
                 w32_obj_new(OBJ_PEN, style == 5 ? -1 : (wx > 0 ? wx : 1), 0, c));
}
/* CreatePen(style, width, colour). PS_NULL is style 5 and draws nothing. */
static MS void *st_CreatePen(int32_t style, int32_t width, uint32_t c)
{
    return w32_h(W32_OBJ_BASE,
                 w32_obj_new(OBJ_PEN, style == 5 ? -1 : (width > 0 ? width : 1), 0, c));
}
/* Keep the LOGFONT the caller described, widened to the W layout so there is
 * one shape to read it back from.
 *
 * Neither form stored anything before, which is invisible while nothing draws
 * text and wrong the moment something does: lfHeight is the size, so every
 * string came out at the same default no matter what the plug-in asked for, and
 * lfFaceName is how it recognises the font it installed from memory.
 *
 * LOGFONTA is 28 numeric bytes then char lfFaceName[32]; LOGFONTW is the same
 * 28 then WCHAR[32]. Only the name differs. */
static void *w32_font_from_logfont(const void *lf, int wide)
{
    int idx = w32_obj_new(OBJ_FONT, 0, 0, 0);
    w32_obj *o;

    if (!idx) return NULL;
    o = &W.obj[idx];
    if (lf) {
        memcpy(o->logfont, lf, 28);
        if (wide) {
            memcpy(o->logfont + 28, (const char *)lf + 28, 64);
        } else {
            const char *nm = (const char *)lf + 28;
            uint16_t *w = (uint16_t *)(o->logfont + 28);
            int i;
            for (i = 0; i < 31 && nm[i]; i++) w[i] = (uint8_t)nm[i];
            w[i] = 0;
        }
        o->logfont_len = 92;
    }
    return w32_h(W32_OBJ_BASE, idx);
}
static MS void *st_CreateFontIndirectA(const void *lf)
{
    int32_t h = 0;
    if (lf) memcpy(&h, lf, 4);
    PLOG("  [font] CreateFontIndirectA height=%d\n", (int)h);
    return w32_font_from_logfont(lf, 0);
}
static MS void *st_CreateFontIndirectW(const void *lf)
{
    int32_t h = 0;
    if (lf) memcpy(&h, lf, 4);
    PLOG("  [font] CreateFontIndirectW height=%d\n", (int)h);
    return w32_font_from_logfont(lf, 1);
}
/* CreateFont's fourteen arguments are a LOGFONT spelled out, so it becomes one. */
static MS void *st_CreateFontA(int32_t h, int32_t w, int32_t esc, int32_t orient,
                               int32_t weight, uint32_t italic, uint32_t underline,
                               uint32_t strike, uint32_t charset, uint32_t outprec,
                               uint32_t clipprec, uint32_t quality, uint32_t pitch,
                               const char *face)
{
    uint8_t lf[60];
    memset(lf, 0, sizeof lf);
    memcpy(lf + 0, &h, 4); memcpy(lf + 4, &w, 4);
    memcpy(lf + 8, &esc, 4); memcpy(lf + 12, &orient, 4);
    memcpy(lf + 16, &weight, 4);
    lf[20] = (uint8_t)italic; lf[21] = (uint8_t)underline;
    lf[22] = (uint8_t)strike; lf[23] = (uint8_t)charset;
    lf[24] = (uint8_t)outprec; lf[25] = (uint8_t)clipprec;
    lf[26] = (uint8_t)quality; lf[27] = (uint8_t)pitch;
    if (face) snprintf((char *)lf + 28, 32, "%s", face);
    return w32_font_from_logfont(lf, 0);
}
static MS void *st_CreateRectRgn(int32_t l, int32_t t, int32_t r, int32_t b)
{
    int i = w32_obj_new(OBJ_RGN, 0, 0, 0);
    if (i) {
        W.obj[i].rc.left = l; W.obj[i].rc.top = t;
        W.obj[i].rc.right = r; W.obj[i].rc.bottom = b;
    }
    return w32_h(W32_OBJ_BASE, i);
}
/* Selecting a clip region replaces whatever the DC had; a NULL region removes
 * the clip entirely, and that is the only way a caller has of widening one
 * again -- IntersectClipRect can only narrow.
 *
 * Answering 1 and doing nothing meant the clip a caller set for one piece of
 * work stayed set for the next. SynthEdit composes its panel as forty 128x128
 * tiles through one DC, clipping to each in turn: the first tile drew, the
 * second intersected with the first and came out empty, and every tile after
 * that was empty too. Two controls out of a hundred, both in the top-left
 * corner, which is exactly the shape that says "the first one worked". */
static MS int32_t st_SelectClipRgn(void *hdc, void *rgn)
{
    w32_dc *d = w32_dcget(hdc);
    w32_obj *o = w32_oget(rgn);

    if (!d) return W32_RGN_ERROR;
    if (!rgn) { d->has_clip = 0; return W32_RGN_SIMPLE; }
    if (!o || o->kind != OBJ_RGN) return W32_RGN_ERROR;
    d->clip = o->rc;
    d->has_clip = 1;
    return w32_rect_empty(&d->clip) ? W32_RGN_NULL : W32_RGN_SIMPLE;
}
/* The same, and the extra flags are about how the region is combined with the
 * meta-region, which this layer does not have. */
static MS int32_t st_ExtSelectClipRgn(void *hdc, void *rgn, int32_t mode)
{ (void)mode; return st_SelectClipRgn(hdc, rgn); }
static MS int32_t st_GetRgnBox(void *rgn, W32RECT *r)
{
    w32_obj *o = w32_oget(rgn);
    if (!o || o->kind != OBJ_RGN) { if (r) memset(r, 0, sizeof *r); return W32_RGN_ERROR; }
    if (r) *r = o->rc;
    PLOG("  [w32] GetRgnBox -> %d,%d..%d,%d\n", o->rc.left, o->rc.top, o->rc.right, o->rc.bottom);
    return (o->rc.right > o->rc.left && o->rc.bottom > o->rc.top)
             ? W32_RGN_SIMPLE : W32_RGN_NULL;
}
/* The rectangles making up a region, in the form GDI reports them.
 *
 * The old stub returned 0 on the theory that a caller would fall back to
 * GetRgnBox. Some do. The one that matters here does not:
 *
 *     n = GetRegionData(hrgn, 0, NULL);
 *     if (n != 0) { ... read the rects, add each to the dirty list ... }
 *
 * so zero is not "ask me another way", it is "this region is empty" -- and a
 * plug-in whose paint handler draws only what its dirty list contains then
 * draws nothing, on every WM_PAINT, having built its whole interface and its
 * whole Direct2D device chain first. That was the blank editor.
 *
 * RGNDATA is a 32-byte header followed by the rectangles: dwSize, iType,
 * nCount, nRgnSize, then the bounding box. Regions here are a single rectangle,
 * so nCount is one and the bound is that rectangle.
 *
 * The three return values are all specified and all different: the required
 * byte count when there is no buffer, the byte count again on success, and zero
 * when the buffer given is too small. */
#define W32_RDH_RECTANGLES 1

static MS uint32_t st_GetRegionData(void *rgn, uint32_t n, void *d)
{
    w32_obj *o = w32_oget(rgn);
    struct {
        uint32_t dwSize, iType, nCount, nRgnSize;
        W32RECT  rcBound;
    } hdr;
    uint32_t need = (uint32_t)(sizeof hdr + sizeof(W32RECT));

    if (!o || o->kind != OBJ_RGN) return 0;
    if (!d) return need;                          /* just asking the size */
    if (n < need) return 0;

    hdr.dwSize   = (uint32_t)sizeof hdr;
    hdr.iType    = W32_RDH_RECTANGLES;
    hdr.nCount   = 1;
    hdr.nRgnSize = (uint32_t)sizeof(W32RECT);
    hdr.rcBound  = o->rc;
    memcpy(d, &hdr, sizeof hdr);
    memcpy((char *)d + sizeof hdr, &o->rc, sizeof o->rc);
    PLOG("  [w32] GetRegionData -> 1 rect %d,%d..%d,%d\n",
         o->rc.left, o->rc.top, o->rc.right, o->rc.bottom);
    return need;
}

/* Two more that go with it, now that regions carry meaning. A region here is
 * one rectangle, so a union is the bounding box of the two -- larger than GDI's
 * answer, never smaller, which costs some overdraw and cannot lose a pixel that
 * should have been repainted. */
static MS int32_t st_SetRectRgn(void *rgn, int32_t l, int32_t t, int32_t r, int32_t b)
{
    w32_obj *o = w32_oget(rgn);
    if (!o || o->kind != OBJ_RGN) return 0;
    o->rc.left = l; o->rc.top = t; o->rc.right = r; o->rc.bottom = b;
    return 1;
}
static MS int32_t st_CombineRgn(void *dst, void *a, void *b, int32_t mode)
{
    w32_obj *od = w32_oget(dst), *oa = w32_oget(a), *ob = w32_oget(b);
    W32RECT r;

    if (!od || od->kind != OBJ_RGN) return W32_RGN_ERROR;
    if (!oa || oa->kind != OBJ_RGN) return W32_RGN_ERROR;
    switch (mode) {
    case 5:                                       /* RGN_COPY */
        od->rc = oa->rc;
        break;
    case 2:                                        /* RGN_AND */
        if (!ob || ob->kind != OBJ_RGN) return W32_RGN_ERROR;
        if (!st_IntersectRect(&r, &oa->rc, &ob->rc)) { st_SetRectEmpty(&od->rc); break; }
        od->rc = r;
        break;
    case 3:                                        /* RGN_OR  */
    case 4:                                        /* RGN_XOR: as a union */
        if (!ob || ob->kind != OBJ_RGN) return W32_RGN_ERROR;
        st_UnionRect(&r, &oa->rc, &ob->rc);
        od->rc = r;
        break;
    case 1:                                        /* RGN_DIFF */
        if (!ob || ob->kind != OBJ_RGN) return W32_RGN_ERROR;
        st_SubtractRect(&r, &oa->rc, &ob->rc);
        od->rc = r;
        break;
    default:
        return W32_RGN_ERROR;
    }
    return w32_rect_empty(&od->rc) ? W32_RGN_NULL : W32_RGN_SIMPLE;
}
/* The stock objects, each of the kind it is actually named after.
 *
 * This returned a brush for every index, which was harmless while SelectObject
 * ignored pens and brushes and is not now: selecting BLACK_PEN set the DC's
 * brush, so a plug-in outlining a shape filled it instead. The colours matter
 * for the same reason -- LTGRAY_BRUSH is the face of nearly every dialog
 * control drawn in this corpus, and black is not a shade of grey. */
static MS void *st_GetStockObject(int32_t i)
{
    static int cache[24];
    int k = (i >= 0 && i < 24) ? i : 0;

    if (!cache[k]) {
        switch (k) {
        case 0:  cache[k] = w32_obj_new(OBJ_BRUSH, 0, 0, 0xFFFFFFu); break;
        case 1:  cache[k] = w32_obj_new(OBJ_BRUSH, 0, 0, 0xC0C0C0u); break;
        case 2:  cache[k] = w32_obj_new(OBJ_BRUSH, 0, 0, 0x808080u); break;
        case 3:  cache[k] = w32_obj_new(OBJ_BRUSH, 0, 0, 0x404040u); break;
        case 4:  cache[k] = w32_obj_new(OBJ_BRUSH, 0, 0, 0x000000u); break;
        case 5:  cache[k] = w32_obj_new(OBJ_BRUSH, -1, 0, 0); break;   /* NULL_BRUSH */
        case 6:  cache[k] = w32_obj_new(OBJ_PEN, 1, 0, 0xFFFFFFu); break;
        case 7:  cache[k] = w32_obj_new(OBJ_PEN, 1, 0, 0x000000u); break;
        case 8:  cache[k] = w32_obj_new(OBJ_PEN, -1, 0, 0); break;      /* NULL_PEN */
        case 18: cache[k] = w32_obj_new(OBJ_BRUSH, 0, 0, 0xFFFFFFu); break; /* DC_BRUSH */
        case 19: cache[k] = w32_obj_new(OBJ_PEN, 1, 0, 0x000000u); break;   /* DC_PEN */
        default: cache[k] = w32_obj_new(OBJ_FONT, 0, 0, 0); break;      /* the fonts */
        }
    }
    return w32_h(W32_OBJ_BASE, cache[k]);
}

static MS uint32_t st_SetBkColor(void *hdc, uint32_t c)
{
    w32_dc *d = w32_dcget(hdc);
    uint32_t was;
    if (!d) return 0xFFFFFFFFu;
    was = d->bk_color; d->bk_color = c;
    return was;
}
static MS uint32_t st_GetBkColor(void *hdc)
{ w32_dc *d = w32_dcget(hdc); return d ? d->bk_color : 0xFFFFFFFFu; }
static MS int32_t st_SetBkMode(void *hdc, int32_t m)
{
    w32_dc *d = w32_dcget(hdc);
    int was;
    if (!d) return 0;
    was = d->bk_mode; d->bk_mode = m;
    return was;
}
static MS int32_t st_GetBkMode(void *hdc)
{ w32_dc *d = w32_dcget(hdc); return d ? d->bk_mode : 0; }
/* ---- GDI drawing --------------------------------------------------------- */

/* COLORREF is 0x00BBGGRR -- red in the low byte -- and the surfaces here are
 * 0x00RRGGBB. Writing one into the other swaps red and blue, which is subtle
 * enough to survive review and obvious the moment anything draws a colour that
 * is not grey. FillRect did exactly that before this. */
static uint32_t w32_cr(uint32_t colorref)
{
    return ((colorref & 0xFFu) << 16) | (colorref & 0xFF00u) |
           ((colorref >> 16) & 0xFFu);
}

/* Whether a pen or brush draws at all. Width -1 is how PS_NULL and BS_NULL are
 * carried; both mean "skip this part of the shape" rather than "use black". */
static int w32_pen_on(const w32_dc *d)
{ return !d->pen || W.obj[d->pen].w >= 0; }
static int w32_brush_on(const w32_dc *d)
{ return !d->brush || W.obj[d->brush].w >= 0; }
static uint32_t w32_pen_rgb(const w32_dc *d)
{ return w32_cr(d->pen ? W.obj[d->pen].color : 0); }
static uint32_t w32_brush_rgb(const w32_dc *d)
{ return w32_cr(d->brush ? W.obj[d->brush].color : 0xFFFFFFu); }

/* One pixel, in device space, honouring the clip box. Every primitive below
 * goes through here so the clip is applied in exactly one place. */
static void w32_plot(w32_surf *t, const w32_dc *d, int x, int y, uint32_t rgb)
{
    if (!t || !t->px || x < 0 || y < 0 || x >= t->w || y >= t->h) return;
    if (d->has_clip &&
        (x < d->clip.left || x >= d->clip.right ||
         y < d->clip.top  || y >= d->clip.bottom)) return;
    t->px[(size_t)y * t->w + x] = rgb;
}

static void w32_fill_span(w32_surf *t, const w32_dc *d, int x0, int x1, int y,
                          uint32_t rgb)
{ for (; x0 < x1; x0++) w32_plot(t, d, x0, y, rgb); }

/* Bresenham, with a square nib for a wide pen. Windows draws a line from the
 * current position up to but not including the end point; the end point is
 * where the next LineTo starts. */
static void w32_line(w32_surf *t, const w32_dc *d, int x0, int y0, int x1, int y1,
                     uint32_t rgb, int width)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;
    int half = width > 1 ? width / 2 : 0;

    for (;;) {
        if (half) {
            int i, j;
            for (j = -half; j <= half; j++)
                for (i = -half; i <= half; i++)
                    w32_plot(t, d, x0 + i, y0 + j, rgb);
        } else {
            w32_plot(t, d, x0, y0, rgb);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

static MS uint32_t st_SetTextColor(void *hdc, uint32_t c)
{
    w32_dc *d = w32_dcget(hdc);
    uint32_t was;
    if (!d) return 0xFFFFFFFFu;                  /* CLR_INVALID */
    was = d->text_color; d->text_color = c;
    return was;
}
static MS uint32_t st_GetTextColor(void *hdc)
{ w32_dc *d = w32_dcget(hdc); return d ? d->text_color : 0xFFFFFFFFu; }
static MS uint32_t st_SetDCBrushColor(void *dc, uint32_t c) { (void)dc; return c; }
static MS int32_t st_SetROP2(void *dc, int32_t m) { (void)dc; return m; }

static MS int32_t st_MoveToEx(void *hdc, int32_t x, int32_t y, W32POINT *p)
{
    w32_dc *d = w32_dcget(hdc);
    if (!d) return 0;
    if (p) { p->x = d->cur_x - d->org_x; p->y = d->cur_y - d->org_y; }
    d->cur_x = x + d->org_x; d->cur_y = y + d->org_y;
    return 1;
}
static MS int32_t st_LineTo(void *hdc, int32_t x, int32_t y)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    if (!d) return 0;
    t = w32_target_in(d, &ttmp);
    x += d->org_x; y += d->org_y;
    if (t && w32_pen_on(d))
        w32_line(t, d, d->cur_x, d->cur_y, x, y, w32_pen_rgb(d),
                 d->pen ? W.obj[d->pen].w : 1);
    d->cur_x = x; d->cur_y = y;
    w32_dib_out_all(d);
    return 1;
}
static MS int32_t st_Rectangle(void *hdc, int32_t l, int32_t tp, int32_t r, int32_t b)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    int y;
    if (!d || !(t = w32_target_in(d, &ttmp))) return 0;
    l += d->org_x; r += d->org_x; tp += d->org_y; b += d->org_y;
    /* Windows fills the interior with the brush and outlines it with the pen,
     * and the right and bottom edges are outside the filled area. */
    if (w32_brush_on(d)) {
        uint32_t f = w32_brush_rgb(d);
        for (y = tp + 1; y < b - 1; y++) w32_fill_span(t, d, l + 1, r - 1, y, f);
    }
    if (w32_pen_on(d)) {
        uint32_t c = w32_pen_rgb(d);
        int wd = d->pen ? W.obj[d->pen].w : 1;
        w32_line(t, d, l, tp, r - 1, tp, c, wd);
        w32_line(t, d, r - 1, tp, r - 1, b - 1, c, wd);
        w32_line(t, d, r - 1, b - 1, l, b - 1, c, wd);
        w32_line(t, d, l, b - 1, l, tp, c, wd);
    }
    w32_dib_out(d, l, tp, r, b);
    return 1;
}
/* Midpoint ellipse: four symmetric arcs, filled by spanning between the two
 * horizontal extremes at each y so the brush and the outline agree. */
static MS int32_t st_Ellipse(void *hdc, int32_t l, int32_t tp, int32_t r, int32_t b)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    double cx, cy, rx, ry;
    int y;
    if (!d || !(t = w32_target_in(d, &ttmp))) return 0;
    l += d->org_x; r += d->org_x; tp += d->org_y; b += d->org_y;
    rx = (r - l) / 2.0; ry = (b - tp) / 2.0;
    if (rx <= 0 || ry <= 0) return 1;
    cx = l + rx; cy = tp + ry;
    for (y = tp; y < b; y++) {
        double dy = ((double)y + 0.5 - cy) / ry;
        double q = 1.0 - dy * dy, hw;
        int x0, x1;
        if (q <= 0) continue;
        hw = rx * sqrt(q);
        x0 = (int)(cx - hw + 0.5); x1 = (int)(cx + hw + 0.5);
        if (w32_brush_on(d)) w32_fill_span(t, d, x0, x1, y, w32_brush_rgb(d));
        if (w32_pen_on(d)) {
            uint32_t c = w32_pen_rgb(d);
            w32_plot(t, d, x0, y, c);
            w32_plot(t, d, x1 - 1, y, c);
        }
    }
    w32_dib_out(d, l, tp, r, b);
    return 1;
}
/* PatBlt with PATCOPY is how MFC's FillSolidRect paints, so it is a rectangle
 * fill with the brush rather than a pattern operation. BLACKNESS and WHITENESS
 * ignore the brush, which is the whole point of them. */
static MS int32_t st_PatBlt(void *hdc, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t rop)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    uint32_t c;
    int j;
    if (!d || !(t = w32_target_in(d, &ttmp))) return 0;
    x += d->org_x; y += d->org_y;
    if (rop == 0x00000042u) c = 0x000000u;             /* BLACKNESS */
    else if (rop == 0x00FF0062u) c = 0xFFFFFFu;        /* WHITENESS */
    else if (w32_brush_on(d)) c = w32_brush_rgb(d);
    else return 1;
    for (j = y; j < y + h; j++) w32_fill_span(t, d, x, x + w, j, c);
    w32_dib_out(d, x, y, x + w, y + h);
    return 1;
}
static MS uint32_t st_SetPixel(void *hdc, int32_t x, int32_t y, uint32_t c)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    if (!d || !(t = w32_target_in(d, &ttmp))) return 0xFFFFFFFFu;
    w32_plot(t, d, x + d->org_x, y + d->org_y, w32_cr(c));
    w32_dib_out(d, x + d->org_x, y + d->org_y, x + d->org_x + 1, y + d->org_y + 1);
    return c;
}
static MS int32_t st_SetPixelV(void *hdc, int32_t x, int32_t y, uint32_t c)
{ return st_SetPixel(hdc, x, y, c) != 0xFFFFFFFFu; }
static MS uint32_t st_GetPixel(void *hdc, int32_t x, int32_t y)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    uint32_t v;
    if (!d || !(t = w32_source_in(d, &ttmp)) || !t->px) return 0xFFFFFFFFu;
    x += d->org_x; y += d->org_y;
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return 0xFFFFFFFFu;
    v = t->px[(size_t)y * t->w + x];
    return w32_cr(v);                            /* back to COLORREF order */
}
static MS int32_t st_Polyline(void *hdc, const W32POINT *pts, int32_t n)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    int i;
    if (!d || !pts || n < 2 || !(t = w32_target_in(d, &ttmp))) return 0;
    if (!w32_pen_on(d)) return 1;
    for (i = 0; i + 1 < n; i++)
        w32_line(t, d, pts[i].x + d->org_x, pts[i].y + d->org_y,
                 pts[i + 1].x + d->org_x, pts[i + 1].y + d->org_y,
                 w32_pen_rgb(d), d->pen ? W.obj[d->pen].w : 1);
    w32_dib_out_all(d);
    return 1;
}
static MS int32_t st_DPtoLP(void *dc, W32POINT *p, int32_t n) { (void)dc;(void)p;(void)n; return 1; }
static MS int32_t st_FillRect(void *hdc, const W32RECT *r, void *brush)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp;
    w32_surf *t = w32_target_in(d, &ttmp);
    w32_obj *b = w32_oget(brush);
    uint32_t c;
    int y;
    if (!t || !t->px || !r || !d) return 0;
    if (b && b->w < 0) return 1;                 /* BS_NULL fills nothing */
    c = w32_cr(b ? b->color : 0xFFFFFFu);
    for (y = r->top + d->org_y; y < r->bottom + d->org_y; y++)
        w32_fill_span(t, d, r->left + d->org_x, r->right + d->org_x, y, c);
    w32_dib_out(d, r->left + d->org_x, r->top + d->org_y,
                r->right + d->org_x, r->bottom + d->org_y);
    return 1;
}
/* ---- GDI text ------------------------------------------------------------ */

/* The pixel height of the selected font. LOGFONT's lfHeight is the character
 * cell height, negative when the caller means the character height itself --
 * the sign is a unit, not a direction, and a plug-in that asks for -11 wants
 * eleven pixels rather than a font measured upside down. */
static int w32_font_px(const w32_dc *d)
{
    int32_t h = 0;
    if (d && d->font && W.obj[d->font].used && W.obj[d->font].logfont_len >= 4)
        memcpy(&h, W.obj[d->font].logfont, 4);
    if (h < 0) h = -h;
    if (h < 4 || h > 400) h = 12;       /* the system font, near enough */
    return (int)h;
}

/* TA_* alignment. The default is the top-left corner with the y coordinate
 * naming the top of the cell; TA_BASELINE means it names the baseline instead,
 * which is what a caller that has measured its own text uses. */
#define W32_TA_RIGHT    2
#define W32_TA_CENTER   6
#define W32_TA_BOTTOM   8
#define W32_TA_BASELINE 24

static void w32_text_at(w32_dc *d, w32_surf *t, int x, int y,
                        const char *s, int n, int opaque_rect_first,
                        const W32RECT *bg)
{
    int tw = 0, th = 0, asc = 0, em = w32_font_px(d);
    int32_t clip[4];
    const int32_t *pclip = NULL;

    if (!t || !t->px || !s || n <= 0) return;
    dw_text_measure(s, n, em, &tw, &th, &asc);

    switch (d->text_align & 6) {
    case W32_TA_RIGHT:  x -= tw;     break;
    case W32_TA_CENTER: x -= tw / 2; break;
    default: break;
    }
    if (d->text_align & W32_TA_BASELINE) y -= asc;   /* y named the baseline */
    else if (d->text_align & W32_TA_BOTTOM) y -= th;

    /* OPAQUE mode paints the cell behind the glyphs; a plug-in relies on it to
     * erase what it drew last time rather than clearing first. */
    if ((opaque_rect_first || d->bk_mode == 2) && th > 0) {
        W32RECT r;
        int j;
        if (bg) r = *bg;
        else { r.left = x; r.top = y; r.right = x + tw; r.bottom = y + th; }
        for (j = r.top; j < r.bottom; j++)
            w32_fill_span(t, d, r.left, r.right, j, w32_cr(d->bk_color));
    }
    if (d->has_clip) {
        clip[0] = d->clip.left; clip[1] = d->clip.top;
        clip[2] = d->clip.right; clip[3] = d->clip.bottom;
        pclip = clip;
    }
    dw_text_draw(t->px, t->w, t->h, x, y + asc, s, n, em,
                 w32_cr(d->text_color), pclip);
    PLOG("  [w32] text \"%.*s\" at %d,%d %dpx colour %06x -> %s#%d\n",
         n > 24 ? 24 : n, s, x, y, em, (unsigned)w32_cr(d->text_color),
         d->bitmap ? "bmp" : "wnd", d->bitmap ? d->bitmap : d->wnd);
    w32_dib_out(d, x, y, x + tw, y + th);
}

static int w32_strn(const char *s, int32_t n)
{ return n < 0 ? (s ? (int)strlen(s) : 0) : (int)n; }

static MS int32_t st_TextOutA(void *hdc, int32_t x, int32_t y,
                              const char *s, int32_t n)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    if (!d || !(t = w32_target_in(d, &ttmp))) return 0;
    w32_text_at(d, t, x + d->org_x, y + d->org_y, s, w32_strn(s, n), 0, NULL);
    return 1;
}
/* Wide text, narrowed. The corpus labels its controls in Latin-1; a character
 * outside it becomes '?' rather than silently truncating the string. */
static MS int32_t st_TextOutW(void *hdc, int32_t x, int32_t y,
                              const uint16_t *ws, int32_t n)
{
    char buf[512];
    int i, len = 0;
    if (!ws) return 0;
    if (n < 0) { for (n = 0; ws[n]; n++) {} }
    for (i = 0; i < n && len + 1 < (int)sizeof buf; i++)
        buf[len++] = ws[i] < 256 ? (char)ws[i] : '?';
    buf[len] = 0;
    return st_TextOutA(hdc, x, y, buf, len);
}

#define W32_ETO_OPAQUE 2

static MS int32_t st_ExtTextOutA(void *hdc, int32_t x, int32_t y, uint32_t opts,
                                 const W32RECT *rc, const char *s, uint32_t n,
                                 const int32_t *dx)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    W32RECT bg;
    (void)dx;                                    /* per-character spacing */
    if (!d || !(t = w32_target_in(d, &ttmp))) return 0;
    if (rc) {
        bg = *rc;
        bg.left += d->org_x; bg.right  += d->org_x;
        bg.top  += d->org_y; bg.bottom += d->org_y;
    }
    w32_text_at(d, t, x + d->org_x, y + d->org_y, s, (int)n,
                (opts & W32_ETO_OPAQUE) != 0, (rc && (opts & W32_ETO_OPAQUE)) ? &bg : NULL);
    return 1;
}
static MS int32_t st_ExtTextOutW(void *hdc, int32_t x, int32_t y, uint32_t opts,
                                 const W32RECT *rc, const uint16_t *ws, uint32_t n,
                                 const int32_t *dx)
{
    char buf[512];
    uint32_t i;
    int len = 0;
    if (!ws) return 0;
    for (i = 0; i < n && len + 1 < (int)sizeof buf; i++)
        buf[len++] = ws[i] < 256 ? (char)ws[i] : '?';
    return st_ExtTextOutA(hdc, x, y, opts, rc, buf, (uint32_t)len, dx);
}

static MS uint32_t st_SetTextAlign(void *hdc, uint32_t a)
{
    w32_dc *d = w32_dcget(hdc);
    uint32_t was;
    if (!d) return 0xFFFFFFFFu;
    was = d->text_align; d->text_align = a;
    return was;
}
static MS uint32_t st_GetTextAlign(void *hdc)
{ w32_dc *d = w32_dcget(hdc); return d ? d->text_align : 0xFFFFFFFFu; }

#define W32_DT_CENTER    0x1
#define W32_DT_RIGHT     0x2
#define W32_DT_VCENTER   0x4
#define W32_DT_BOTTOM    0x8
#define W32_DT_SINGLELINE 0x20
#define W32_DT_CALCRECT  0x400

/* Single-line DrawText, which is what a control label is. Multi-line wrapping
 * is not attempted: reporting the single-line height for a block that would
 * wrap is a wrong layout, where drawing nothing is no layout at all, and every
 * caller in this corpus passes DT_SINGLELINE. */
static MS int32_t st_DrawTextA(void *hdc, const char *s, int32_t n, W32RECT *r,
                               uint32_t f)
{
    w32_dc *d = w32_dcget(hdc);
    w32_surf ttmp, *t;
    int len = w32_strn(s, n), tw = 0, th = 0, asc = 0, x, y;
    int em;

    if (!d || !r) return 0;
    em = w32_font_px(d);
    dw_text_measure(s, len, em, &tw, &th, &asc);
    if (f & W32_DT_CALCRECT) {                   /* measure only */
        r->right = r->left + tw;
        r->bottom = r->top + th;
        return th;
    }
    if (!(t = w32_target_in(d, &ttmp))) return 0;
    x = r->left + d->org_x;
    y = r->top + d->org_y;
    if (f & W32_DT_CENTER)     x += ((r->right - r->left) - tw) / 2;
    else if (f & W32_DT_RIGHT) x += (r->right - r->left) - tw;
    if (f & W32_DT_VCENTER)    y += ((r->bottom - r->top) - th) / 2;
    else if (f & W32_DT_BOTTOM) y += (r->bottom - r->top) - th;
    {   /* alignment is applied here, not by the TA_ state */
        uint32_t save = d->text_align;
        d->text_align = 0;
        w32_text_at(d, t, x, y, s, len, 0, NULL);
        d->text_align = save;
    }
    return th;
}
static MS int32_t st_DrawTextW(void *hdc, const uint16_t *ws, int32_t n,
                               W32RECT *r, uint32_t f)
{
    char buf[512];
    int i, len = 0;
    if (!ws) return 0;
    if (n < 0) { for (n = 0; ws[n]; n++) {} }
    for (i = 0; i < n && len + 1 < (int)sizeof buf; i++)
        buf[len++] = ws[i] < 256 ? (char)ws[i] : '?';
    buf[len] = 0;
    return st_DrawTextA(hdc, buf, len, r, f);
}

/* TEXTMETRIC, which layout code divides by. tmHeight and tmAveCharWidth are the
 * two fields anything here reads; a zeroed structure is a divide by zero. */
static MS int32_t st_GetTextMetricsA(void *hdc, void *out)
{
    w32_dc *d = w32_dcget(hdc);
    struct { int32_t height, ascent, descent, intleading, extleading,
                     aveCharWidth, maxCharWidth, weight, overhang,
                     digitizedAspectX, digitizedAspectY;
             uint8_t first, last, def, brk, italic, underlined, struckOut,
                     pitchAndFamily, charSet; } tm;
    int w = 0, h = 0, asc = 0, em;

    if (!out) return 0;
    em = w32_font_px(d);
    dw_text_measure("nnnnnnnnnn", 10, em, &w, &h, &asc);
    memset(&tm, 0, sizeof tm);
    tm.height = h ? h : em;
    tm.ascent = asc ? asc : em;
    tm.descent = tm.height - tm.ascent;
    tm.aveCharWidth = w ? w / 10 : em / 2;
    tm.maxCharWidth = tm.aveCharWidth * 2;
    tm.weight = 400;
    tm.first = 32; tm.last = 255; tm.def = '?'; tm.brk = ' ';
    memcpy(out, &tm, sizeof tm);
    return 1;
}
static MS int32_t st_GetTextMetricsW(void *hdc, void *out)
{ return st_GetTextMetricsA(hdc, out); }

/* The plugin registers its embedded TTF here. Keeping the bytes lets GetFontData
 * serve them, and parsing the family name out of the font lets
 * EnumFontFamiliesEx report it -- which is how the plugin discovers what to put
 * in a LOGFONT for the font it just installed from memory. */
/* Owned copy, not an alias. Windows documents AddFontMemResourceEx as taking
 * its own copy, so the caller is free to release the buffer the moment it
 * returns -- aliasing it meant GetFontData could later serve freed memory. */
static uint8_t *g_font_bytes;
static uint32_t g_font_size;
static char     g_font_family[64];

/* Defined with the DirectWrite shim, which owns the FreeType face. */
static void dw_font_bytes_changing(void);
static void dw_reset(void);
static int  dw_font_pipeline_ok(void);

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Pull nameID 1 (family) out of the sfnt `name` table. Prefers the Windows
 * platform record, which is big-endian UTF-16. */
static void font_family_from_sfnt(const uint8_t *f, uint32_t len, char *out, size_t outn)
{
    uint32_t ntables, i, nameOff = 0, nameLen = 0;
    const uint8_t *nt;
    uint16_t count, strOff, r;

    if (outn) out[0] = 0;
    if (!f || len < 12) return;
    ntables = be16(f + 4);
    for (i = 0; i < ntables; i++) {
        const uint8_t *rec = f + 12 + i * 16;
        if ((uint32_t)(rec - f) + 16 > len) return;
        if (!memcmp(rec, "name", 4)) {
            nameOff = be32(rec + 8);
            nameLen = be32(rec + 12);
            break;
        }
    }
    if (!nameOff || nameOff + 6 > len) return;
    nt = f + nameOff;
    count = be16(nt + 2);
    strOff = be16(nt + 4);
    for (r = 0; r < count; r++) {
        const uint8_t *rec = nt + 6 + r * 12;
        uint16_t plat, nameID, rlen, roff;
        if ((uint32_t)(rec - f) + 12 > len) return;
        plat   = be16(rec);
        nameID = be16(rec + 6);
        rlen   = be16(rec + 8);
        roff   = be16(rec + 10);
        if (nameID != 1) continue;                 /* family name */
        if (nameOff + strOff + roff + rlen > len) continue;
        {
            const uint8_t *sp = nt + strOff + roff;
            size_t o = 0, k;
            if (plat == 3 || plat == 0) {          /* UTF-16BE */
                for (k = 0; k + 1 < rlen && o + 1 < outn; k += 2) {
                    uint16_t ch = be16(sp + k);
                    out[o++] = (ch && ch < 128) ? (char)ch : '?';
                }
            } else {                                /* single byte */
                for (k = 0; k < rlen && o + 1 < outn; k++) out[o++] = (char)sp[k];
            }
            out[o] = 0;
            if (o) return;                          /* first usable one wins */
        }
    }
    (void)nameLen;
}

/* Fonts: Skia asks for the raw bytes and rasterises text itself, so all that is
 * needed is a real TTF to hand back. */
typedef MS int32_t (*enumfontproc)(const void *, const void *, uint32_t, W_LPARAM);

/* The family of whatever font is selected into this DC, falling back to the one
 * the plugin installed from memory. Answering with a fixed name made the plugin
 * conclude its own embedded font was not the one it had just installed. */
static const char *w32_dc_family(void *hdc)
{
    w32_dc *d = w32_dcget(hdc);
    if (d && d->font && W.obj[d->font].used && W.obj[d->font].logfont_len >= 92) {
        /* lfFaceName sits at offset 28 of LOGFONTW, as UTF-16. */
        static char nm[64];
        const uint16_t *w = (const uint16_t *)(W.obj[d->font].logfont + 28);
        int i = 0;
        for (; w[i] && i + 1 < (int)sizeof nm; i++) nm[i] = (w[i] < 128) ? (char)w[i] : '?';
        nm[i] = 0;
        if (nm[0]) return nm;
    }
    return g_font_family[0] ? g_font_family : "Sans";
}

static MS uint32_t st_GetFontData(void *dc, uint32_t table, uint32_t off, void *buf, uint32_t len)
{
    PLOG("  [font] GetFontData table=0x%x off=%u buf=%p len=%u\n", table, off, buf, len);
    (void)dc;
    if (!g_font_bytes || !g_font_size) return 0xFFFFFFFFu;   /* GDI_ERROR */
    if (table != 0) return 0xFFFFFFFFu;                       /* whole file only */
    if (off >= g_font_size) return 0xFFFFFFFFu;
    {
        uint32_t avail = g_font_size - off;
        if (!buf) return avail;
        if (len > avail) len = avail;
        memcpy(buf, g_font_bytes + off, len);
        return len;
    }
}
static MS void *st_AddFontMemResourceEx(void *p, uint32_t sz, void *r, uint32_t *cnt)
{
    PLOG("  [font] AddFontMemResourceEx %u bytes\n", sz);
    (void)r;
    if (p && sz) {
        const unsigned char *b = p;
        PLOG("  [font]   magic %02x %02x %02x %02x (%s)\n", b[0], b[1], b[2], b[3],
             (b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) ? "TrueType" :
             (b[0] == 'O' && b[1] == 'T' && b[2] == 'T' && b[3] == 'O') ? "OpenType" :
             (b[0] == 't' && b[1] == 'r' && b[2] == 'u' && b[3] == 'e') ? "TrueType/mac" :
             "NOT A FONT");
        /* Declared in dwrite_shim.h; the FreeType face references these bytes
         * and must be closed before they are released. */
        dw_font_bytes_changing();
        free(g_font_bytes);
        g_font_bytes = malloc(sz);
        if (g_font_bytes) {
            memcpy(g_font_bytes, p, sz);
            g_font_size = sz;
        } else {
            g_font_size = 0;
        }
        font_family_from_sfnt(p, sz, g_font_family, sizeof g_font_family);
        PLOG("  [font]   family '%s'\n", g_font_family);
    } else {
        PLOG("  [font]   !! p=%p sz=%u -- the plugin got no font bytes\n", p, sz);
    }
    if (cnt) *cnt = 1;
    return (void *)0x464F4E54;                  /* 'FONT' */
}
static MS int32_t st_RemoveFontMemResourceEx(void *h) { (void)h; return 1; }
static MS int32_t st_EnumFontFamiliesExA(void *dc, void *lf, void *proc, intptr_t p, uint32_t f)
{
    uint8_t elf[256], ntm[100];
    PLOG("  [font] EnumFontFamiliesExA proc=%p family='%s'\n", proc, g_font_family);
    (void)dc; (void)lf; (void)f;
    if (!proc || !g_font_family[0]) return 0;
    memset(elf, 0, sizeof elf);
    memset(ntm, 0, sizeof ntm);
    *(int32_t *)(elf + 0)  = -12;
    *(int32_t *)(elf + 16) = 400;
    elf[23] = 1;
    snprintf((char *)(elf + 28), 32, "%s", g_font_family);      /* LOGFONTA face */
    snprintf((char *)(elf + 60), 64, "%s", g_font_family);      /* elfFullName   */
    *(int32_t *)(ntm + 0) = 12;
    *(int32_t *)(ntm + 4) = 10;
    return ((enumfontproc)proc)(elf, ntm, 0x0004, p);
}
/* Report the font the plugin installed from memory. Enumerating nothing leaves
 * it unable to discover the family name of its own embedded font. */

static MS int32_t st_EnumFontFamiliesExW(void *dc, void *lf, void *proc, intptr_t p, uint32_t f)
{
    uint8_t elf[348], ntm[100];
    uint16_t *face;
    int i;

    PLOG("  [font] EnumFontFamiliesExW proc=%p family='%s'\n", proc, g_font_family);
    (void)dc; (void)lf; (void)f;
    if (!proc || !g_font_family[0]) return 0;

    memset(elf, 0, sizeof elf);
    memset(ntm, 0, sizeof ntm);
    /* LOGFONTW: lfHeight -12, lfWeight 400, lfCharSet DEFAULT, face at +28 */
    *(int32_t *)(elf + 0)  = -12;
    *(int32_t *)(elf + 16) = 400;
    elf[23] = 1;                       /* lfCharSet = DEFAULT_CHARSET */
    face = (uint16_t *)(elf + 28);
    for (i = 0; g_font_family[i] && i < 31; i++) face[i] = (uint8_t)g_font_family[i];
    face[i] = 0;
    /* elfFullName at +92, elfStyle at +220 */
    { uint16_t *full = (uint16_t *)(elf + 92);
      for (i = 0; g_font_family[i] && i < 63; i++) full[i] = (uint8_t)g_font_family[i];
      full[i] = 0; }
    { uint16_t *style = (uint16_t *)(elf + 220);
      const char *r = "Regular";
      for (i = 0; r[i] && i < 31; i++) style[i] = (uint8_t)r[i];
      style[i] = 0; }
    /* Plausible metrics; nothing here scales layout on its own. */
    *(int32_t *)(ntm + 0)  = 12;       /* tmHeight  */
    *(int32_t *)(ntm + 4)  = 10;       /* tmAscent  */
    *(int32_t *)(ntm + 8)  = 2;        /* tmDescent */
    *(int32_t *)(ntm + 28) = 400;      /* tmWeight  */

    return ((enumfontproc)proc)(elf, ntm, 0x0004 /* TRUETYPE_FONTTYPE */, p);
}
static MS int32_t st_GetTextExtentPoint32A(void *hdc, const char *s, int32_t n,
                                          W32POINT *sz)
{
    w32_dc *d = w32_dcget(hdc);
    int w = 0, h = 0;
    if (!sz) return 0;
    /* Measured with the selected font rather than guessed at seven pixels a
     * character. Layout that centres a label or sizes a control to its text was
     * being handed a width unrelated to what would be drawn. */
    if (!dw_text_measure(s, w32_strn(s, n), w32_font_px(d), &w, &h, NULL)) {
        w = n > 0 ? n * 7 : 0; h = 14;           /* no face: keep it non-zero */
    }
    sz->x = w; sz->y = h;
    return 1;
}
static MS int32_t st_GetTextExtentPointA(void *hdc, const char *s, int32_t n,
                                         W32POINT *sz)
{ return st_GetTextExtentPoint32A(hdc, s, n, sz); }
static MS int32_t st_GetTextFaceA(void *dc, int32_t n, char *buf)
{
    const char *f = w32_dc_family(dc);
    PLOG("  [font] GetTextFaceA -> '%s'\n", f);
    if (buf && n) snprintf(buf, (size_t)n, "%s", f);
    return (int32_t)strlen(f);
}
static MS int32_t st_GetTextFaceW(void *dc, int32_t n, uint16_t *buf)
{
    const char *f = w32_dc_family(dc);
    int i;
    PLOG("  [font] GetTextFaceW -> '%s'\n", f);
    if (!buf || n <= 0) return (int32_t)strlen(f);
    for (i = 0; f[i] && i + 1 < n; i++) buf[i] = (uint8_t)f[i];
    buf[i] = 0;
    return i;
}

/* ---- input, cursors, metrics -------------------------------------------- */

static MS void *st_SetCapture(void *hwnd)
{
    w32_wnd *w = w32_wget(hwnd);
    void *prev = W.capture ? w32_h(W32_HWND_BASE, W.capture) : NULL;
    W.capture = w ? (int)(w - W.wnd) : 0;
    return prev;
}
static int w32_child_at(int wnd, int *x, int *y);          /* below */

/* Which window is under a point. The editor picture is this host's whole
 * screen, so a screen point and a client point of the root are the same thing.
 * A plug-in that hit-tests its own panel this way got zero from the stub and
 * concluded the pointer was over nothing. */
static MS void *st_WindowFromPoint(W32POINT pt)
{
    int root = (W.host && W.wnd[W.host].used) ? W.host
             : (W.display && W.wnd[W.display].used) ? W.display : 0;
    int x = pt.x, y = pt.y, hit;
    if (!root) return NULL;
    hit = w32_child_at(root, &x, &y);
    return hit ? w32_h(W32_HWND_BASE, hit) : NULL;
}
/* The immediate child of a parent that contains a point given in the parent's
 * client coordinates -- the parent itself when none does. */
static MS void *st_ChildWindowFromPoint(void *parent, W32POINT pt)
{
    w32_wnd *p = w32_wget(parent);
    int i, pi;
    if (!p) return NULL;
    pi = (int)(p - W.wnd);
    for (i = W32_MAX_WND - 1; i >= 1; i--) {
        w32_wnd *c = &W.wnd[i];
        if (!c->used || c->parent != pi || !c->visible) continue;
        if (pt.x < c->x || pt.x >= c->x + c->w) continue;
        if (pt.y < c->y || pt.y >= c->y + c->h) continue;
        return w32_h(W32_HWND_BASE, i);
    }
    return parent;
}
static MS void *st_ChildWindowFromPointEx(void *parent, W32POINT pt, uint32_t flags)
{ (void)flags; return st_ChildWindowFromPoint(parent, pt); }
/* SetClassLongPtr keeps no per-class state here beyond what RegisterClass
 * recorded, and every caller in the corpus uses it to swap a cursor or an
 * icon; reporting the previous value as none is what a fresh class holds. */
static MS W_LRESULT st_SetClassLongPtrW(void *hwnd, int32_t index, W_LRESULT v)
{ (void)hwnd; (void)index; (void)v; return 0; }
static MS W_LRESULT st_SetClassLongPtrA(void *hwnd, int32_t index, W_LRESULT v)
{ return st_SetClassLongPtrW(hwnd, index, v); }
static MS uint32_t st_SetClassLongW(void *hwnd, int32_t index, uint32_t v)
{ (void)hwnd; (void)index; (void)v; return 0; }
static MS uint32_t st_SetClassLongA(void *hwnd, int32_t index, uint32_t v)
{ return st_SetClassLongW(hwnd, index, v); }

static MS int32_t st_ReleaseCapture(void) { W.capture = 0; return 1; }
static MS void *st_GetCapture(void)
{ return W.capture ? w32_h(W32_HWND_BASE, W.capture) : NULL; }
static MS void *st_SetFocus(void *hwnd)
{
    w32_wnd *w = w32_wget(hwnd);
    void *prev = W.focus ? w32_h(W32_HWND_BASE, W.focus) : NULL;
    W.focus = w ? (int)(w - W.wnd) : 0;
    if (w) w32_call(w, WM_SETFOCUS, 0, 0);
    return prev;
}
static MS int32_t st_GetCursorPos(W32POINT *p)
{ w32_pump_input(); if (p) { p->x = W.mouse_x; p->y = W.mouse_y; } return 1; }
static MS int32_t st_SetCursorPos(int32_t x, int32_t y) { W.mouse_x = x; W.mouse_y = y; return 1; }
static MS void *st_SetCursor(void *c) { return c; }
static MS void *st_GetCursor(void) { return (void *)0x43555253; }
static MS void *st_LoadCursorA(void *inst, const char *name)
{ (void)inst;(void)name; return (void *)0x43555253; }
static MS int32_t st_ShowCursor(int32_t show) { (void)show; return 1; }
static MS int16_t st_GetKeyState(int32_t vk)
{ return (int16_t)((vk >= 0 && vk < 256 && W.keys[vk]) ? (int16_t)0x8000 : 0); }
/* A safety valve for a modal drag loop with nobody able to feed it.
 *
 * A plugin that polls for the button release spins until it sees one. With the
 * bridge there is a separate process to publish that release (pump_input above);
 * hosted in-process there is not -- the thread that would deliver it is the one
 * inside the loop. Rather than hang the host forever, a button that has been
 * reported held for half a second with no new input reads as released. A real
 * drag generates events continuously, so this only fires when nothing can. */
static MS int16_t st_GetAsyncKeyState(int32_t vk)
{
    w32_pump_input();
    if ((vk == 1 || vk == 2 || vk == 4) && W.keys[vk] && !W.hooks.pump_input) {
        double now = w32_now_ms();
        if (W.last_input_ms > 0.0 && now - W.last_input_ms > 500.0) {
            W.keys[1] = W.keys[2] = W.keys[4] = 0;
            fprintf(stderr, "  [w32] releasing a stuck mouse button: the plugin "
                            "is polling for it and no input is arriving\n");
            return 0;
        }
    }
    return st_GetKeyState(vk);
}
static MS int32_t st_GetKeyboardState(uint8_t *st)
{
    int i;
    if (!st) return 0;
    for (i = 0; i < 256; i++) st[i] = W.keys[i] ? 0x80 : 0;
    return 1;
}
static MS void *st_GetKeyboardLayout(uint32_t t) { (void)t; return (void *)0x409; }
static MS int32_t st_ToAscii(uint32_t vk, uint32_t sc, const uint8_t *ks, uint16_t *out, uint32_t f)
{
    (void)sc; (void)ks; (void)f;
    if (!out) return 0;
    if (vk >= 32 && vk < 127) { out[0] = (uint16_t)vk; return 1; }
    return 0;
}
static MS int16_t st_VkKeyScanExA(char c, void *layout)
{ (void)layout; return (int16_t)(unsigned char)toupper((unsigned char)c); }
static MS int32_t st_TrackMouseEvent(void *e) { (void)e; return 1; }
static MS uint32_t st_GetDoubleClickTime(void) { return 400; }
static MS int32_t st_ClientToScreen(void *hwnd, W32POINT *p)
{
    w32_wnd *w = w32_wget(hwnd);
    if (w && p) { p->x += w->x; p->y += w->y; }
    return 1;
}
static MS int32_t st_ScreenToClient(void *hwnd, W32POINT *p)
{
    w32_wnd *w = w32_wget(hwnd);
    if (w && p) { p->x -= w->x; p->y -= w->y; }
    return 1;
}
static MS int32_t st_MapWindowPoints(void *from, void *to, W32POINT *p, uint32_t n)
{
    w32_wnd *f = w32_wget(from), *t = w32_wget(to);
    uint32_t i;
    if (!p) return 0;
    for (i = 0; i < n; i++) {
        if (f) { p[i].x += f->x; p[i].y += f->y; }
        if (t) { p[i].x -= t->x; p[i].y -= t->y; }
    }
    return 1;
}
static MS uint32_t st_GetSysColor(int32_t i) { (void)i; return 0x00C0C0C0; }
static MS void *st_GetSysColorBrush(int32_t i) { (void)i; return st_GetStockObject(0); }
static MS int32_t st_GetSystemMetrics(int32_t i)
{
    switch (i) {
    case 0:  return 1920;      /* SM_CXSCREEN */
    case 1:  return 1080;      /* SM_CYSCREEN */
    case 32: case 33: return 4;
    default: return 0;
    }
}
static MS int32_t st_SystemParametersInfoW(uint32_t a, uint32_t b, void *p, uint32_t f)
{ (void)a;(void)b;(void)f; if (p) memset(p, 0, 4); return 1; }
static MS int32_t st_MessageBoxA(void *h, const char *t, const char *c, uint32_t f)
{ (void)h;(void)f; fprintf(stderr, "[plugin] %s: %s\n", c ? c : "", t ? t : ""); return 1; }
static MS int32_t st_MessageBoxW(void *h, const uint16_t *t, const uint16_t *c, uint32_t f)
{ (void)h;(void)t;(void)c;(void)f; return 1; }

/* ---- timers ------------------------------------------------------------- */

static double w32_now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static MS uintptr_t st_SetTimer(void *hwnd, uintptr_t id, uint32_t ms, void *proc)
{
    w32_wnd *w = w32_wget(hwnd);
    PLOG("  [w32] SetTimer hwnd=%p id=%llu %u ms proc=%p\n", hwnd,
         (unsigned long long)id, ms, proc);
    int i, wi = w ? (int)(w - W.wnd) : 0;
    for (i = 0; i < W32_MAX_TIMER; i++)
        if (W.timer[i].used && W.timer[i].wnd == wi && W.timer[i].id == id) {
            W.timer[i].ms = ms ? ms : 10;
            /* Re-arming may also change the callback, and keeping the old one
             * means the plug-in's new handler never runs while the old one
             * keeps being called. */
            W.timer[i].proc = proc;
            W.timer[i].next = w32_now() + W.timer[i].ms / 1000.0;
            return id;
        }
    for (i = 0; i < W32_MAX_TIMER; i++) {
        if (W.timer[i].used) continue;
        W.timer[i].used = 1;
        W.timer[i].wnd = wi;
        W.timer[i].id = id ? id : (uint64_t)(i + 1);
        W.timer[i].ms = ms ? ms : 10;
        W.timer[i].proc = proc;
        W.timer[i].next = w32_now() + W.timer[i].ms / 1000.0;
        return W.timer[i].id;
    }
    return 0;
}
static MS int32_t st_KillTimer(void *hwnd, uintptr_t id)
{
    w32_wnd *w = w32_wget(hwnd);
    int i, wi = w ? (int)(w - W.wnd) : 0;
    for (i = 0; i < W32_MAX_TIMER; i++)
        if (W.timer[i].used && W.timer[i].wnd == wi && W.timer[i].id == id)
            W.timer[i].used = 0;
    return 1;
}

/* ---- clipboard, menus, drag and drop: accepted and ignored -------------- */

static MS int32_t st_OpenClipboard(void *h) { (void)h; return 0; }
static MS int32_t st_CloseClipboard(void) { return 1; }
static MS int32_t st_EmptyClipboard(void) { return 1; }
static MS void *st_GetClipboardData(uint32_t f) { (void)f; return NULL; }
static MS void *st_SetClipboardData(uint32_t f, void *h) { (void)f; return h; }
static MS int32_t st_IsClipboardFormatAvailable(uint32_t f) { (void)f; return 0; }
static MS void *st_CreatePopupMenu(void) { return (void *)0x4D454E55; }
static MS int32_t st_AppendMenuA(void *m, uint32_t f, uintptr_t id, const char *s)
{ (void)m;(void)f;(void)id;(void)s; return 1; }
static MS int32_t st_AppendMenuW(void *m, uint32_t f, uintptr_t id, const uint16_t *s)
{ (void)m;(void)f;(void)id;(void)s; return 1; }
static MS int32_t st_DestroyMenu(void *m) { (void)m; return 1; }
static MS int32_t st_TrackPopupMenu(void *m, uint32_t f, int32_t x, int32_t y,
                                   int32_t r, void *h, const void *rc)
{ (void)m;(void)f;(void)x;(void)y;(void)r;(void)h;(void)rc; return 0; }
static MS int32_t st_RegisterTouchWindow(void *h, uint32_t f) { (void)h;(void)f; return 0; }
static MS int32_t st_CloseTouchInputHandle(void *h) { (void)h; return 1; }
static MS int32_t st_GetTouchInputInfo(void *h, uint32_t n, void *in, int32_t sz)
{ (void)h;(void)n;(void)in;(void)sz; return 0; }
static MS int32_t st_InitCommonControlsEx(const void *p) { (void)p; return 1; }
static MS int32_t st_DragAcceptFiles(void *h, int32_t a) { (void)h;(void)a; return 1; }
static MS uint32_t st_DragQueryFileA(void *h, uint32_t i, char *b, uint32_t n)
{ (void)h;(void)i; if (b && n) b[0] = 0; return 0; }
static MS uint32_t st_DragQueryFileW(void *h, uint32_t i, uint16_t *b, uint32_t n)
{ (void)h;(void)i; if (b && n) b[0] = 0; return 0; }
static MS int32_t st_DragQueryPoint(void *h, W32POINT *p)
{ (void)h; if (p) { p->x = 0; p->y = 0; } return 0; }
static MS int32_t st_RegisterDragDrop(void *h, void *t) { (void)h;(void)t; return 0; }
static MS int32_t st_RevokeDragDrop(void *h) { (void)h; return 0; }
static MS int32_t st_DoDragDrop(void *a, void *b, uint32_t c, uint32_t *d)
{ (void)a;(void)b;(void)c; if (d) *d = 0; return 0; }
static MS int32_t st_CoCreateGuid(void *g)
{
    /* Unique enough: a counter is all a plugin needs from this. */
    static uint32_t n;
    if (g) { memset(g, 0, 16); n++; memcpy(g, &n, 4); }
    return 0;
}

/* ---- host-facing entry points ------------------------------------------ */

void w32_set_hooks(const w32_host_hooks *h)
{ if (h) W.hooks = *h; else memset(&W.hooks, 0, sizeof W.hooks); }

/* Only the input pump, so installing it cannot clobber `present`. The window
 * layer and the host learn about these at different times. */
void w32_set_input_pump(void (*fn)(void *), void *ud)
{
    W.hooks.pump_input = fn;
    if (ud) W.hooks.ud = ud;
}

/* The container an editor is parented to. Created on demand so a plugin that is
 * never shown pays nothing. */
void *w32_create_host_window(int w, int h)
{
    void *hwnd;
    if (W.host && W.wnd[W.host].used) return w32_h(W32_HWND_BASE, W.host);
    /* A class with no WndProc: nothing is ever dispatched to the container. */
    {
        W32WNDCLASSA c;
        memset(&c, 0, sizeof c);
        c.lpszClassName = "peloadHostContainer";
        c.lpfnWndProc = NULL;
        st_RegisterClassA(&c);
    }
    hwnd = w32_create("peloadHostContainer", NULL, 0, 0, w, h, NULL, NULL, 0, 0);
    W.host = w32_i(W32_HWND_BASE, hwnd);
    W.display = 0;                    /* the plugin's window claims this next */
    return hwnd;
}

void *w32_root_window(void)
{
    int i = W.display ? W.display : W.host;
    return i ? w32_h(W32_HWND_BASE, i) : NULL;
}

/* The size of a window we created for the plugin.
 *
 * Needed because a plugin is not obliged to answer effEditGetRect usefully:
 * TAL's U-NO-62 builds its editor, sizes its own window, and still reports an
 * empty rect. The window it made is the honest answer. */
int w32_window_size(void *hwnd, int *w, int *h)
{
    int i = w32_i(W32_HWND_BASE, hwnd);
    if (i <= 0 || i >= W32_MAX_WND || !W.wnd[i].used) return 0;
    if (w) *w = W.wnd[i].w;
    if (h) *h = W.wnd[i].h;
    return (W.wnd[i].w > 0 && W.wnd[i].h > 0);
}

void w32_set_client_size(int w, int h)
{
    if (W.host && W.wnd[W.host].used)       w32_resize(&W.wnd[W.host], w, h);
    if (W.display && W.wnd[W.display].used) w32_resize(&W.wnd[W.display], w, h);
}

/* Mark the editor shown and invalidate it. Plugins commonly gate drawing on the
 * window being visible, and nothing here otherwise ever calls ShowWindow. */
void w32_show_editor(void)
{
    int i;
    for (i = 1; i < W32_MAX_WND; i++) {
        if (!W.wnd[i].used) continue;
        /* Showing the editor shows the windows the plug-in built, not the ones
         * it deliberately left hidden: a child without WS_VISIBLE keeps what
         * ShowWindow last said about it. */
        if (!W.wnd[i].parent) W.wnd[i].visible = 1;
        if (W.wnd[i].wndproc) {
            w32_call(&W.wnd[i], WM_SHOWWINDOW, 1, 0);
            w32_call(&W.wnd[i], WM_SIZE, 0,
                     (W_LPARAM)((W.wnd[i].h << 16) | (W.wnd[i].w & 0xFFFF)));
        }
        W.wnd[i].has_update = 1;
        W.wnd[i].update.left = 0; W.wnd[i].update.top = 0;
        W.wnd[i].update.right = W.wnd[i].w; W.wnd[i].update.bottom = W.wnd[i].h;
    }
}

/* Copy of the editor's pixels, for a host that wants to pull rather than be
 * pushed to. Returns 0 if there is nothing to show. */
void w32_stats(void)
{
    /* Which window actually holds pixels. "Nothing was drawn" and "everything
     * was drawn into a window we do not present" look identical from the
     * capture, and the difference is the whole question. */
    {
        int i;
        for (i = 1; i < W32_MAX_WND; i++) {
            long nb = 0, n;
            if (!W.wnd[i].used || !W.wnd[i].surf.px) continue;
            n = (long)W.wnd[i].surf.w * W.wnd[i].surf.h;
            { long k; for (k = 0; k < n; k++)
                if (W.wnd[i].surf.px[k] & 0xFFFFFF) nb++; }
            fprintf(stderr, "  [w32] window #%d '%s' %dx%d parent=%d %ld/%ld "
                            "painted%s%s\n", i, W.wnd[i].cls,
                    W.wnd[i].w, W.wnd[i].h, W.wnd[i].parent, nb, n,
                    i == W.display ? "  <- presented" : "",
                    i == W.host ? "  (container)" : "");
        }
    }
    fprintf(stderr, "  [w32] calls: WM_PAINT %ld, BeginPaint %ld, GetDC %ld, "
                    "StretchDIBits %ld, BitBlt %ld, TIMERPROC %ld\n",
            W.n_paint, W.n_beginpaint, W.n_getdc, W.n_stretch, W.n_bitblt, W.n_timerproc);
}

/* Child windows drawn over their parent, which is what a window manager would
 * be doing. Composited into a buffer of our own rather than into the parent's
 * surface: the parent keeps painting into that, and a child blitted onto it
 * would still be there on the next frame with nothing to erase it.
 *
 * It matters for any plug-in whose editor is built from real controls rather
 * than one skinned bitmap -- without it every BUTTON and EDIT paints itself
 * correctly into a surface nobody ever looks at. */
static w32_surf g_present;

static void w32_composite_children(int parent, w32_surf *out, int ox, int oy)
{
    int i, guard = 0;

    for (i = 1; i < W32_MAX_WND; i++) {
        w32_wnd *c = &W.wnd[i];
        int cx, cy, y, x;
        if (!c->used || c->parent != parent || !c->visible || !c->surf.px) continue;
        if (guard++ > W32_MAX_WND) break;
        cx = ox + c->x; cy = oy + c->y;
        for (y = 0; y < c->surf.h; y++) {
            int ty = cy + y;
            if (ty < 0 || ty >= out->h) continue;
            for (x = 0; x < c->surf.w; x++) {
                int tx = cx + x;
                if (tx < 0 || tx >= out->w) continue;
                out->px[(size_t)ty * out->w + tx] =
                    c->surf.px[(size_t)y * c->surf.w + x];
            }
        }
        w32_composite_children(i, out, cx, cy);
    }
}

int w32_editor_pixels(const unsigned int **px, int *w, int *h)
{
    int i = W.display ? W.display : W.host;
    w32_surf *s;

    if (!i || !W.wnd[i].used || !W.wnd[i].surf.px) return 0;
    s = &W.wnd[i].surf;
    w32_surf_size(&g_present, s->w, s->h);
    if (!g_present.px) return 0;
    memcpy(g_present.px, s->px, (size_t)s->w * s->h * 4);
    w32_composite_children(i, &g_present, 0, 0);
    if (px) *px = g_present.px;
    if (w)  *w  = g_present.w;
    if (h)  *h  = g_present.h;
    return 1;
}

/* Drain queued messages, fire due timers, and repaint anything invalid.
 * Called from the host's UI thread on a timer. */
void w32_pump(void)
{
    double now = w32_now();
    int i, guard = 0;

    while (W.qtail != W.qhead && guard++ < W32_MSGQ) {
        W32MSG m = W.q[W.qtail];
        W.qtail = (W.qtail + 1) % W32_MSGQ;
        {
            w32_wnd *w = w32_wget(m.hwnd);
            if (w) w32_call(w, m.message, m.wParam, m.lParam);
        }
    }
    for (i = 0; i < W32_MAX_TIMER; i++) {
        if (!W.timer[i].used || now < W.timer[i].next) continue;
        W.timer[i].next = now + W.timer[i].ms / 1000.0;
        /* A timer carrying a TIMERPROC is called directly rather than having
         * WM_TIMER posted -- and with a NULL hwnd that callback is the only way
         * it ever runs. iPlug2 drives its whole redraw from exactly this form,
         * so ignoring it left the editor permanently blank. */
        if (W.timer[i].proc) {
            /* TIMERPROC: (HWND, UINT, UINT_PTR, DWORD) */
            MS void (*tp)(void *, uint32_t, uintptr_t, uint32_t) =
                (MS void (*)(void *, uint32_t, uintptr_t, uint32_t))W.timer[i].proc;
            void *hw = W.timer[i].wnd ? w32_h(W32_HWND_BASE, W.timer[i].wnd) : NULL;
            W.n_timerproc++;
            PLOG("  [w32] timer #%d id=%llu %ums -> proc %p\n", i,
                 (unsigned long long)W.timer[i].id, W.timer[i].ms,
                 W.timer[i].proc);
            tp(hw, WM_TIMER, W.timer[i].id, (uint32_t)(now * 1000.0));
        } else if (W.timer[i].wnd && W.wnd[W.timer[i].wnd].used) {
            w32_call(&W.wnd[W.timer[i].wnd], WM_TIMER, W.timer[i].id, 0);
        }
    }
    for (i = 1; i < W32_MAX_WND; i++)
        if (W.wnd[i].used && W.wnd[i].has_update && W.wnd[i].wndproc) {
            W.n_paint++;
            w32_call(&W.wnd[i], WM_PAINT, 0, 0);
        }
}

/* Drop every window, DC, object and timer. Essential when a plugin is closed:
 * its image is unmapped, so any WndProc still referenced here would be a call
 * into freed memory on the next pump. */
int w32_font_pipeline_ok(void) { return dw_font_pipeline_ok(); }

void w32_reset(void)
{
    int i;
    /* First, because the rest of this drops the windows those threads would
     * paint into, and the plug-in's image goes right after it. */
    w32_stop_workers();
    for (i = 1; i < W32_MAX_WND; i++) {
        if (!W.wnd[i].used) continue;
        w32_surf_free(&W.wnd[i].surf);
        memset(&W.wnd[i], 0, sizeof W.wnd[i]);
    }
    for (i = 1; i < W32_MAX_OBJ; i++) {
        if (!W.obj[i].used) continue;
        free(W.obj[i].px);
        memset(&W.obj[i], 0, sizeof W.obj[i]);
    }
    for (i = 1; i < W32_MAX_DC; i++) W.dc[i].used = 0;
    for (i = 0; i < W32_MAX_TIMER; i++) W.timer[i].used = 0;
    for (i = 1; i < W32_MAX_CLS; i++) W.cls[i].used = 0;
    W.qhead = W.qtail = 0;
    W.host = W.display = 0;
    W.capture = W.focus = 0;
    dw_reset();
}

/* Windows hands the mouse to the deepest child window under the pointer, in
 * that child's own client coordinates -- and to the capture window alone once
 * one is held. Delivering everything to the plug-in's top-level window instead
 * meant an editor whose controls live on a child saw nothing at all: a VSTGUI
 * wrapper around a GMPI panel, which is how SynthEdit builds one, painted
 * perfectly and ignored every click. The window under the pointer is also what
 * WindowFromPoint answers, and a plug-in that asks got zero.
 *
 * The walk keeps the last window that has a WndProc, because a container with
 * none cannot be a target: the message would go nowhere rather than to the
 * parent that would have handled it. */
static int w32_child_at(int wnd, int *x, int *y)
{
    int best = wnd, bx = *x, by = *y, stepped = 1;
    while (stepped) {
        int i;
        stepped = 0;
        for (i = W32_MAX_WND - 1; i >= 1; i--) {          /* last created is on top */
            w32_wnd *c = &W.wnd[i];
            if (!c->used || c->parent != wnd || !c->visible) continue;
            if (*x < c->x || *x >= c->x + c->w) continue;
            if (*y < c->y || *y >= c->y + c->h) continue;
            *x -= c->x;
            *y -= c->y;
            wnd = i;
            stepped = 1;
            if (c->wndproc) { best = i; bx = *x; by = *y; }
            break;
        }
    }
    *x = bx;
    *y = by;
    return best;
}
/* A window's client origin, relative to the root the host's coordinates are in. */
static void w32_origin_of(int i, int root, int *ox, int *oy)
{
    int x = 0, y = 0;
    while (i > 0 && i != root && W.wnd[i].used) {
        x += W.wnd[i].x;
        y += W.wnd[i].y;
        i = W.wnd[i].parent;
    }
    *ox = x;
    *oy = y;
}

void w32_mouse(int x, int y, int msg, int buttons, int wheel)
{
    w32_wnd *w;
    int root = (W.host && W.wnd[W.host].used) ? W.host
             : (W.display && W.wnd[W.display].used) ? W.display : 0;
    int target;
    int cx = x, cy = y;

    if (!root) return;
    if (W.capture && W.wnd[W.capture].used) {
        int ox, oy;
        w32_origin_of(W.capture, root, &ox, &oy);
        target = W.capture;
        cx = x - ox;
        cy = y - oy;
    } else {
        target = w32_child_at(root, &cx, &cy);
        if (!W.wnd[target].wndproc && W.display && W.wnd[W.display].used) {
            int ox, oy;
            w32_origin_of(W.display, root, &ox, &oy);
            target = W.display;
            cx = x - ox;
            cy = y - oy;
        }
    }
    if (!target || !W.wnd[target].used) return;
    w = &W.wnd[target];
    PLOG("  [w32] mouse msg 0x%x at %d,%d -> wnd #%d cls='%s' local %d,%d%s\n",
         (unsigned)msg, x, y, target, w->cls, cx, cy,
         W.capture ? " (captured)" : "");
    x = cx;
    y = cy;
    W.mouse_x = x; W.mouse_y = y;
    /* Record the buttons as key state too. Not every GUI reads the mouse from
     * messages: TAL's polls GetAsyncKeyState(VK_LBUTTON) with GetCursorPos on
     * each idle, so with the button state missing its editor painted perfectly
     * and ignored every click. VK_LBUTTON 1, VK_RBUTTON 2, VK_MBUTTON 4. */
    W.keys[1] = (buttons & 1) ? 1 : 0;
    W.keys[2] = (buttons & 2) ? 1 : 0;
    W.keys[4] = (buttons & 4) ? 1 : 0;
    W.last_input_ms = w32_now_ms();
    if (msg == WM_MOUSEWHEEL)
        w32_call(w, WM_MOUSEWHEEL, ((W_WPARAM)(uint16_t)(int16_t)(wheel * 120) << 16) | (uint32_t)buttons,
                 (int64_t)(((uint32_t)(uint16_t)y << 16) | (uint16_t)x));
    else
        w32_call(w, (uint32_t)msg, (uint64_t)buttons,
                 (int64_t)(((uint32_t)(uint16_t)y << 16) | (uint16_t)x));
}

void w32_key(int vk, int down, int ch)
{
    w32_wnd *w;
    int target = W.focus ? W.focus : (W.display ? W.display : W.host);
    if (!target || !W.wnd[target].used) return;
    w = &W.wnd[target];
    if (vk >= 0 && vk < 256) W.keys[vk] = down ? 1 : 0;
    w32_call(w, down ? WM_KEYDOWN : WM_KEYUP, (uint64_t)vk, 1);
    if (down && ch > 0) w32_call(w, WM_CHAR, (uint64_t)ch, 1);
}

#endif /* PELOAD_WIN32GUI_H */
