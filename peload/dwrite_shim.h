/* DirectWrite shim.
 *
 * iPlug2 builds Skia's font manager with SkFontMgr_New_DirectWrite(), and Skia
 * reaches DirectWrite by LoadLibraryW(L"dwrite.dll") + GetProcAddress. With no
 * dwrite the factory is null, SkFontMgrRefDefault() caches that null, and the
 * first ->makeFromData() dereferences it. That is the single reason a Windows
 * plugin editor cannot draw here.
 *
 * DirectWrite is COM, so every interface is a vtable of a known length in a
 * known order. Rather than write those from memory and debug the crashes, each
 * interface starts life as a *probe*: a full-length vtable whose every slot is a
 * generated stub that reports its own index and returns E_NOTIMPL. Running the
 * plugin then tells us exactly which slots Skia calls, in order, and only those
 * need real implementations.
 *
 * Set PELOAD_VERBOSE=1 to see the trace. */
#ifndef PELOAD_DWRITE_SHIM_H
#define PELOAD_DWRITE_SHIM_H

/* The PNG and BMP readers the WIC shim below decodes with. Already written
 * for the macOS side's ImageIO, and the same two formats a Windows plug-in
 * ships its artwork in. */
#include "png_in.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#define DW_E_NOTIMPL   ((int32_t)0x80004001)
#define DW_E_NOINTERFACE ((int32_t)0x80004002)
#define DW_S_OK        ((int32_t)0)

/* Slot counts including the three IUnknown entries. Over-allocating is safe --
 * an unused slot is simply never called -- and under-allocating would let a
 * call run off the end of the table. */
/* Wide enough for the biggest vtable a plug-in reaches through here.
 * ID3D11DeviceContext has 108 methods and ID2D1DeviceContext is not far
 * behind; a probe shorter than the interface means a call past the end lands
 * on whatever follows the array, which presents as a jump to a null address
 * with no clue as to why. */
#define DWP_MAX_SLOTS 160

typedef struct dwprobe {
    void       **vtbl;               /* the object's vtable pointer */
    const char  *iface;              /* for logging */
    int32_t      refs;
    void        *impl[DWP_MAX_SLOTS];/* real implementations, NULL = probe */
    void        *ctx;                /* backing data for the real methods */
    int          index;              /* slot in g_dwp_objs */
    const uint8_t *iid_known;        /* the one interface this object claims */
} dwprobe;

/* One JIT'd stub per (object, slot) so a call can name both. Objects are
 * addressed by table index rather than by pointer, because packing a slot into
 * the low bits of a pointer would destroy the pointer. */
#define DWP_MAX_OBJS 512
static uint8_t *g_dwp_code;
static size_t   g_dwp_used;
static dwprobe *g_dwp_objs[DWP_MAX_OBJS];
static int      g_dwp_nobjs;

static MS int32_t dwp_report(uintptr_t packed)
{
    int idx  = (int)(packed >> 12);
    int slot = (int)(packed & 0xFFF);
    dwprobe *o = (idx >= 0 && idx < g_dwp_nobjs) ? g_dwp_objs[idx] : NULL;
    PLOG("  [dwrite] %-24s slot %2d  -> E_NOTIMPL\n",
         (o && o->iface) ? o->iface : "?", slot);
#if defined(__i386__)
    /* On i386 a COM method is stdcall and pops its own arguments, and a probe
     * slot is by definition one whose signature we do not have -- so the stub
     * below cannot balance the stack. Say so rather than let it corrupt
     * quietly. In practice this is unreached: across all 36 plugins with their
     * editors open, no probe slot is ever called, because the shim implements
     * everything they use. */
    fprintf(stderr, "  [dwrite] %s slot %d called on i386, where an unknown "
                    "signature cannot be returned from cleanly\n",
            (o && o->iface) ? o->iface : "?", slot);
#endif
    return DW_E_NOTIMPL;
}

/* A stub per (object, slot) that reports which slot was hit and returns
 * E_NOTIMPL. The original arguments are discarded -- the point is only to learn
 * the slot index, not to do the call. */
static void *dwp_stub(dwprobe *o, int slot)
{
    uint8_t *p;
    uintptr_t packed;

    if (!g_dwp_code) {
        g_dwp_code = mmap(NULL, 1 << 22, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_dwp_code == MAP_FAILED) return NULL;
    }
    if (g_dwp_used + 32 > (1u << 22)) return NULL;   /* arena exhausted */
    p = g_dwp_code + g_dwp_used;
    g_dwp_used += 32;

    packed = ((uintptr_t)o->index << 12) | (uintptr_t)(slot & 0xFFF);
#if defined(__i386__)
    /* push imm32 ; mov eax, dwp_report ; call eax ; mov eax, E_NOTIMPL ; ret
     * dwp_report is stdcall, so it pops the pushed argument itself. */
    *p++ = 0x68; memcpy(p, &packed, 4); p += 4;                   /* push imm32     */
    *p++ = 0xB8;
    { void *f = (void *)dwp_report; memcpy(p, &f, 4); p += 4; }   /* mov eax, fn    */
    *p++ = 0xFF; *p++ = 0xD0;                                     /* call eax       */
    *p++ = 0xB8;
    { uint32_t r = (uint32_t)DW_E_NOTIMPL; memcpy(p, &r, 4); p += 4; }
    *p++ = 0xC3;                                                  /* ret            */
#else
    /* mov rcx, imm64 ; mov rax, dwp_report ; jmp rax */
    *p++ = 0x48; *p++ = 0xB9; memcpy(p, &packed, 8); p += 8;      /* mov rcx, imm64 */
    *p++ = 0x48; *p++ = 0xB8;
    { void *f = (void *)dwp_report; memcpy(p, &f, 8); p += 8; }   /* mov rax, fn    */
    *p++ = 0xFF; *p++ = 0xE0;                                     /* jmp rax        */
#endif
    return g_dwp_code + g_dwp_used - 32;
}

/* GUIDs are {DWORD, WORD, WORD, BYTE[8]} with the first three little-endian. */
static void dwp_guid_str(const void *iid, char *out, size_t n)
{
    const uint8_t *b = iid;
    if (!iid) { snprintf(out, n, "(null)"); return; }
    snprintf(out, n, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             (unsigned)(b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24)),
             (unsigned)(b[4] | (b[5] << 8)), (unsigned)(b[6] | (b[7] << 8)),
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* Known interface identities. Claiming to be every interface asked for is what
 * made Skia call methods from a newer IDWriteFactory than this shim provides,
 * running off the end of the vtable we actually implement. */
static const uint8_t IID_IUnknown_b[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 };
/* IDWriteFactory {b859ee5a-d838-4b5b-a2e8-1adc7d93db48} */
static const uint8_t IID_IDWriteFactory_b[16] = {
    0x5a,0xee,0x59,0xb8, 0x38,0xd8, 0x5b,0x4b, 0xa2,0xe8,0x1a,0xdc,0x7d,0x93,0xdb,0x48 };

static MS int32_t dwp_qi(void *self, const void *iid, void **out)
{
    dwprobe *o = self;
    char g[64];

    dwp_guid_str(iid, g, sizeof g);
    if (out) *out = NULL;
    if (!iid) return DW_E_NOINTERFACE;

    if (!memcmp(iid, IID_IUnknown_b, 16) ||
        (o->iid_known && !memcmp(iid, o->iid_known, 16))) {
        PLOG("  [dwrite] %s QI %s -> ok\n", o->iface, g);
        if (out) { *out = self; o->refs++; }
        return DW_S_OK;
    }
    PLOG("  [dwrite] %s QI %s -> E_NOINTERFACE\n", o->iface, g);
    return DW_E_NOINTERFACE;
}
static MS uint32_t dwp_addref(void *self)
{ dwprobe *o = self; return (uint32_t)(++o->refs); }
static MS uint32_t dwp_release(void *self)
{ dwprobe *o = self; return (uint32_t)(o->refs > 0 ? --o->refs : 0); }

/* Build an object whose vtable is entirely probes, then let callers overwrite
 * individual slots with real implementations. */
static dwprobe *dwp_new(const char *iface)
{
    dwprobe *o = calloc(1, sizeof *o);
    int i;
    if (!o) return NULL;
    o->iface = iface;
    o->refs = 1;
    o->index = g_dwp_nobjs < DWP_MAX_OBJS ? g_dwp_nobjs : 0;
    if (g_dwp_nobjs < DWP_MAX_OBJS) g_dwp_objs[g_dwp_nobjs++] = o;
    o->vtbl = calloc(DWP_MAX_SLOTS, sizeof *o->vtbl);
    if (!o->vtbl) { free(o); return NULL; }
    for (i = 0; i < DWP_MAX_SLOTS; i++) o->vtbl[i] = dwp_stub(o, i);
    o->vtbl[0] = (void *)dwp_qi;
    o->vtbl[1] = (void *)dwp_addref;
    o->vtbl[2] = (void *)dwp_release;
    return o;
}

static void dwp_set(dwprobe *o, int slot, void *fn)
{ if (o && slot >= 0 && slot < DWP_MAX_SLOTS) o->vtbl[slot] = fn; }

/* An object with a caller-supplied shared vtable and no probe stubs. Skia
 * creates one glyph-run analysis per run of text, so JIT-ing 64 stubs each time
 * exhausted the code arena within a few plugin loads. */
static dwprobe *dwp_new_fixed(const char *iface, void **vtbl)
{
    dwprobe *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->iface = iface;
    o->refs = 1;
    o->vtbl = vtbl;
    o->index = -1;                    /* not a probe: never reported by slot */
    return o;
}

/* ---- factory methods, filled in as the probe reveals them --------------- */

static dwprobe *g_dwrite_syscoll;

/* Defined below, once the types they hand back exist. */
typedef struct dw_collection dw_collection_fwd;
static dwprobe *dw_make_font(struct dw_collection *c);

/* The collection's methods, defined below once dw_collection exists. */
static MS uint32_t dwc_GetFontFamilyCount(void *self);
static MS int32_t  dwc_GetFontFamily(void *self, uint32_t idx, void **out);
static MS int32_t  dwc_FindFamilyName(void *self, const uint16_t *name,
                                      uint32_t *index, int32_t *exists);

/* IDWriteFactory::GetSystemFontCollection(IDWriteFontCollection**, BOOL) */
static MS int32_t dwf_GetSystemFontCollection(void *self, void **out, int32_t checkUpdates)
{
    (void)self; (void)checkUpdates;
    PLOG("  [dwrite] IDWriteFactory::GetSystemFontCollection\n");
    if (!out) return DW_E_NOTIMPL;
    if (!g_dwrite_syscoll) {
        g_dwrite_syscoll = dwp_new("IDWriteFontCollection");
        /* The same methods the custom collection below gets. Without them the
         * system collection was a bare probe: every call answered E_NOTIMPL,
         * and a plug-in walking the system fonts to size its own text got a
         * null family it then dereferenced. Skia never noticed because it
         * registers a collection of its own and never asks for this one. */
        if (g_dwrite_syscoll) {
            dwp_set(g_dwrite_syscoll, 3, (void *)dwc_GetFontFamilyCount);
            dwp_set(g_dwrite_syscoll, 4, (void *)dwc_GetFontFamily);
            dwp_set(g_dwrite_syscoll, 5, (void *)dwc_FindFamilyName);
        }
    }
    if (!g_dwrite_syscoll) return DW_E_NOTIMPL;
    g_dwrite_syscoll->refs++;
    *out = g_dwrite_syscoll;
    return DW_S_OK;
}

/* The FreeType face everything here measures with, opened on demand below. */
static FT_Face dw_ftface(void);

/* ---- WIC, the imaging factory -------------------------------------------
 *
 * A plug-in that draws a bitmap interface asks COM for an IWICImagingFactory
 * and decodes its skin through it. Nothing here provided one, so
 * CoCreateInstance failed and the factory pointer stayed null in a structure
 * the plug-in then used without checking.
 *
 * Only the decoding path is here: name a file, get its pixels. Encoding,
 * metadata, palettes and colour management are not what a plug-in loading its
 * own artwork needs. */
/* One decoded image, shared by the decoder, its frame and any converter
 * wrapped around it. BGRA, top row first -- the format image_decode already
 * produces and the one WIC callers overwhelmingly ask for. */
typedef struct { uint32_t *px; int w, h; int refs; } wic_image;

static MS int32_t wic_GetSize(void *self, uint32_t *w, uint32_t *h)
{
    dwprobe *o = self;
    wic_image *im = o ? o->ctx : NULL;
    if (!im) return DW_E_NOTIMPL;
    if (w) *w = (uint32_t)im->w;
    if (h) *h = (uint32_t)im->h;
    return DW_S_OK;
}

/* GUID_WICPixelFormat32bppBGRA. The whole family shares a prefix and differs
 * in the last byte, which is what identifies the layout. */
static const uint8_t WIC_FMT_32bppBGRA[16] = {
    0x24,0xC3,0xDD,0x6F, 0x03,0x4E, 0xFE,0x4B,
    0xB1,0x85, 0x3D,0x77,0x76,0x8D,0xC9,0x0F };

static MS int32_t wic_GetPixelFormat(void *self, void *guid)
{
    (void)self;
    if (!guid) return DW_E_NOTIMPL;
    memcpy(guid, WIC_FMT_32bppBGRA, 16);
    return DW_S_OK;
}

static MS int32_t wic_GetResolution(void *self, double *dx, double *dy)
{ (void)self; if (dx) *dx = 96.0; if (dy) *dy = 96.0; return DW_S_OK; }

/* IWICBitmapSource::CopyPixels(WICRect const*, UINT stride, UINT bufsize, BYTE*)
 * A null rect means the whole image, which is how a caller blitting a skin
 * asks for it. */
static MS int32_t wic_CopyPixels(void *self, const void *rect, uint32_t stride,
                                 uint32_t bufsz, uint8_t *buf)
{
    dwprobe *o = self;
    wic_image *im = o ? o->ctx : NULL;
    int32_t x = 0, y = 0, w, h, row;
    const int32_t *r = rect;

    if (!im || !buf) return DW_E_NOTIMPL;
    w = im->w; h = im->h;
    if (r) { x = r[0]; y = r[1]; w = r[2]; h = r[3]; }
    if (x < 0 || y < 0 || w < 0 || h < 0 ||
        x + w > im->w || y + h > im->h) return (int32_t)0x80070057; /* E_INVALIDARG */
    if ((uint64_t)stride * (uint64_t)(h ? h - 1 : 0) + (uint64_t)w * 4 > bufsz)
        return (int32_t)0x8007000E;                                /* E_OUTOFMEMORY */
    for (row = 0; row < h; row++)
        memcpy(buf + (size_t)row * stride,
               im->px + (size_t)(y + row) * im->w + x, (size_t)w * 4);
    return DW_S_OK;
}

static void wic_source_slots(dwprobe *o)
{
    dwp_set(o, 3, (void *)wic_GetSize);
    dwp_set(o, 4, (void *)wic_GetPixelFormat);
    dwp_set(o, 5, (void *)wic_GetResolution);
    dwp_set(o, 7, (void *)wic_CopyPixels);
}

/* IWICBitmapDecoder::GetFrameCount / GetFrame. One frame: none of the artwork
 * a plug-in ships is animated. */
static MS int32_t wicd_GetFrameCount(void *self, uint32_t *n)
{ (void)self; if (!n) return DW_E_NOTIMPL; *n = 1; return DW_S_OK; }

static MS int32_t wicd_GetFrame(void *self, uint32_t idx, void **out)
{
    dwprobe *o = self, *f;
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (idx != 0 || !o || !o->ctx) return DW_E_NOTIMPL;
    if (!(f = dwp_new("IWICBitmapFrameDecode"))) return DW_E_NOTIMPL;
    f->ctx = o->ctx;
    ((wic_image *)f->ctx)->refs++;
    wic_source_slots(f);
    *out = f;
    return DW_S_OK;
}

/* IWICFormatConverter::Initialize(source, format, dither, palette, alpha, type)
 * Everything decoded here is already 32bpp BGRA, so the conversion is a
 * hand-over rather than a conversion -- but the object still has to behave
 * like a source afterwards, which is what the caller goes on to use. */
static MS int32_t wicc_Initialize(void *self, void *src, const void *fmt, uint32_t dither,
                                  void *pal, double alpha, uint32_t paltype)
{
    dwprobe *o = self, *s = src;
    (void)fmt; (void)dither; (void)pal; (void)alpha; (void)paltype;
    if (!o || !s || !s->ctx) return DW_E_NOTIMPL;
    o->ctx = s->ctx;
    ((wic_image *)o->ctx)->refs++;
    return DW_S_OK;
}
static MS int32_t wicc_CanConvert(void *self, const void *from, const void *to, int32_t *can)
{ (void)self; (void)from; (void)to; if (can) *can = 1; return DW_S_OK; }

/* IWICBitmapLock, and IWICBitmap::Lock behind it.
 *
 * A plug-in that composes its own artwork locks a bitmap and writes into the
 * pixels directly. The buffer is handed over rather than copied, which is the
 * whole point of locking, and the rectangle only shifts where it starts. */
static MS int32_t wicl_GetSize(void *self, uint32_t *w, uint32_t *h)
{ return wic_GetSize(self, w, h); }
static MS int32_t wicl_GetStride(void *self, uint32_t *stride)
{
    dwprobe *o = self;
    wic_image *im = o ? o->ctx : NULL;
    if (!im || !stride) return DW_E_NOTIMPL;
    *stride = (uint32_t)im->w * 4;
    return DW_S_OK;
}
static MS int32_t wicl_GetDataPointer(void *self, uint32_t *size, uint8_t **data)
{
    dwprobe *o = self;
    wic_image *im = o ? o->ctx : NULL;
    if (!im || !size || !data) return DW_E_NOTIMPL;
    *size = (uint32_t)(im->w * im->h * 4);
    *data = (uint8_t *)im->px;
    return DW_S_OK;
}
static MS int32_t wicb_Lock(void *self, const int32_t *rc, uint32_t flags, void **out)
{
    dwprobe *o = self, *l;
    (void)flags;
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (!o || !o->ctx) return DW_E_NOTIMPL;
    if (!(l = dwp_new("IWICBitmapLock"))) return DW_E_NOTIMPL;
    l->ctx = o->ctx;
    ((wic_image *)l->ctx)->refs++;
    dwp_set(l, 3, (void *)wicl_GetSize);
    dwp_set(l, 4, (void *)wicl_GetStride);
    dwp_set(l, 5, (void *)wicl_GetDataPointer);
    dwp_set(l, 6, (void *)wic_GetPixelFormat);
    (void)rc;
    *out = l;
    return DW_S_OK;
}

/* IWICImagingFactory::CreateBitmapFromSource(source, cacheOption, IWICBitmap**)
 * A cached copy of a decoded image. It is already in memory, so the "copy" is
 * another reference to the same pixels behaving as a source. */
static MS int32_t wic_CreateBitmapFromSource(void *self, void *src, uint32_t cache, void **out)
{
    dwprobe *s2 = src, *b;
    (void)self; (void)cache;
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (!s2 || !s2->ctx) return DW_E_NOTIMPL;
    if (!(b = dwp_new("IWICBitmap"))) return DW_E_NOTIMPL;
    b->ctx = s2->ctx;
    ((wic_image *)b->ctx)->refs++;
    wic_source_slots(b);
    dwp_set(b, 8, (void *)wicb_Lock);
    *out = b;
    return DW_S_OK;
}

/* IWICImagingFactory::CreateBitmap(w, h, pixelFormat, cacheOption, IWICBitmap**)
 *
 * A blank bitmap for the plug-in to compose into -- it locks this, writes its
 * pixels, and hands the result to Direct2D. Refusing it is not a missing
 * feature: the caller has nowhere to put the artwork it was about to build, so
 * whatever that artwork was simply never appears. Here that was the panel every
 * other control is drawn on top of, which is why the interface came out on a
 * flat grey background.
 *
 * The format argument is accepted and not honoured: everything in this shim is
 * 32-bit BGRA, which is what a caller composing an interface asks for, and
 * quietly producing a different depth would be worse than the refusal was. */
static MS int32_t wic_CreateBitmap(void *self, uint32_t w, uint32_t h,
                                   const void *fmt, uint32_t cache, void **out)
{
    dwprobe *b;
    wic_image *im;

    (void)self; (void)fmt; (void)cache;
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (!w || !h || w > 16384 || h > 16384) return (int32_t)0x80070057; /* E_INVALIDARG */
    if (!(im = (wic_image *)calloc(1, sizeof *im))) return DW_E_NOTIMPL;
    if (!(im->px = (uint32_t *)calloc((size_t)w * h, 4))) { free(im); return DW_E_NOTIMPL; }
    im->w = (int)w; im->h = (int)h; im->refs = 1;
    if (!(b = dwp_new("IWICBitmap"))) { free(im->px); free(im); return DW_E_NOTIMPL; }
    b->ctx = im;
    wic_source_slots(b);
    dwp_set(b, 8, (void *)wicb_Lock);
    PLOG("  [wic]   created a %ux%u bitmap to compose into\n", w, h);
    *out = b;
    return DW_S_OK;
}

static MS int32_t wic_CreateFormatConverter(void *self, void **out)
{
    dwprobe *c;
    (void)self;
    if (!out) return DW_E_NOTIMPL;
    if (!(c = dwp_new("IWICFormatConverter"))) return DW_E_NOTIMPL;
    wic_source_slots(c);
    dwp_set(c, 8, (void *)wicc_Initialize);
    dwp_set(c, 9, (void *)wicc_CanConvert);
    *out = c;
    return DW_S_OK;
}

/* IWICImagingFactory::CreateDecoderFromFilename(name, vendor, access, opts,
 *                                               IWICBitmapDecoder**) */
static MS int32_t wic_CreateDecoderFromFilename(void *self, const uint16_t *name,
                                                const void *vendor, uint32_t access,
                                                uint32_t opts, void **out)
{
    char n[1024], fixed[1024];
    dwprobe *dec;
    wic_image *im;
    uint8_t *bytes = NULL;
    long len = 0;
    FILE *fp;
    int w = 0, h = 0;
    uint32_t *px;

    (void)self; (void)vendor; (void)access; (void)opts;
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    w2c(name, n, sizeof n);
    path_fix(n, fixed, sizeof fixed);
    PLOG("  [wic] CreateDecoderFromFilename(\"%s\")\n", fixed);

    if (!(fp = fopen(fixed, "rb"))) return (int32_t)0x88982F04;  /* not found */
    fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (len > 0 && (bytes = malloc((size_t)len)) &&
        fread(bytes, 1, (size_t)len, fp) != (size_t)len) { free(bytes); bytes = NULL; }
    fclose(fp);
    if (!bytes) return (int32_t)0x8007000E;

    px = image_decode(bytes, (size_t)len, &w, &h);
    free(bytes);
    if (!px || w <= 0 || h <= 0) { free(px); return (int32_t)0x88982F50; } /* unsupported */

    if (!(im = calloc(1, sizeof *im))) { free(px); return (int32_t)0x8007000E; }
    im->px = px; im->w = w; im->h = h; im->refs = 1;
    if (!(dec = dwp_new("IWICBitmapDecoder"))) { free(px); free(im); return DW_E_NOTIMPL; }
    dec->ctx = im;
    dwp_set(dec, 12, (void *)wicd_GetFrameCount);
    dwp_set(dec, 13, (void *)wicd_GetFrame);
    PLOG("  [wic]   decoded %dx%d\n", w, h);
    *out = dec;
    return DW_S_OK;
}

static dwprobe *g_wic_factory;

static void *wic_factory(void)
{
    if (!g_wic_factory) {
        g_wic_factory = dwp_new("IWICImagingFactory");
        if (g_wic_factory) {
            dwp_set(g_wic_factory,  3, (void *)wic_CreateDecoderFromFilename);
            dwp_set(g_wic_factory, 10, (void *)wic_CreateFormatConverter);
            dwp_set(g_wic_factory, 17, (void *)wic_CreateBitmap);
            dwp_set(g_wic_factory, 18, (void *)wic_CreateBitmapFromSource);
        }
    }
    if (g_wic_factory) g_wic_factory->refs++;
    return g_wic_factory;
}

/* ---- text format and layout --------------------------------------------
 *
 * Skia never needed these: it lays text out itself and asks this shim only for
 * font files and glyph rasters. A plug-in that draws its own interface with
 * DirectWrite does the opposite -- it hands over a string and a font size and
 * expects the layout back -- so the two entry points below exist for it.
 *
 * The layout is one line, measured with FreeType through the same face
 * everything else here uses. That is honest for what it is asked to do: label
 * text in a plug-in editor, where the question is "how wide is this string"
 * and the answer decides whether the interface has a size at all. */
typedef struct {
    float    size;
    uint16_t family[64];
    uint16_t text[512];
    uint32_t textlen;
    float    maxw, maxh;
} dw_format;

static MS float dwtf_GetFontSize(void *self)
{ dwprobe *o = self; dw_format *f = o ? o->ctx : NULL; return f ? f->size : 12.0f; }

/* Every alignment and wrapping setter. A format that cannot be configured is
 * refused by some callers, and none of these change a single line's extent. */
static MS int32_t dwtf_SetAny(void *self, uint32_t v)
{ (void)self; (void)v; return DW_S_OK; }

/* DWRITE_TEXT_METRICS: left, top, width, widthIncludingTrailingWhitespace,
 * height, layoutWidth, layoutHeight (floats), then maxBidiReorderingDepth and
 * lineCount (UINT32). Twenty-eight bytes at both widths. */
/* How wide and tall the string is, measured through the same FreeType face
 * everything else here draws with. One line: these are editor labels. */
static void dw_measure(dw_format *f, float *wOut, float *hOut)
{
    FT_Face face = dw_ftface();
    float w = 0.0f, h = f ? f->size : 12.0f;
    uint32_t i;

    if (face && f) {
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(f->size > 0 ? f->size : 12.0f));
        for (i = 0; i < f->textlen; i++) {
            if (FT_Load_Char(face, (FT_ULong)f->text[i], FT_LOAD_DEFAULT) == 0)
                w += (float)(face->glyph->advance.x >> 6);
        }
        h = (float)((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
    } else if (f) {
        w = (float)f->textlen * f->size * 0.55f;      /* a plausible average */
    }
    if (wOut) *wOut = w;
    if (hOut) *hOut = h;
}

/* IDWriteTextLayout::DetermineMinWidth(FLOAT*) -- the narrowest the layout can
 * be without breaking a word. One line and no wrapping, so it is the width. */
static MS int32_t dwtl_DetermineMinWidth(void *self, float *out)
{
    dwprobe *o = self;
    if (!out) return DW_E_NOTIMPL;
    dw_measure(o ? o->ctx : NULL, out, NULL);
    return DW_S_OK;
}
static MS int32_t dwtl_SetMaxWidth(void *self, float v)
{ dwprobe *o = self; dw_format *f = o ? o->ctx : NULL; if (f) f->maxw = v; return DW_S_OK; }
static MS int32_t dwtl_SetMaxHeight(void *self, float v)
{ dwprobe *o = self; dw_format *f = o ? o->ctx : NULL; if (f) f->maxh = v; return DW_S_OK; }
static MS float dwtl_GetMaxWidth(void *self)
{ dwprobe *o = self; dw_format *f = o ? o->ctx : NULL; return f ? f->maxw : 0.0f; }
static MS float dwtl_GetMaxHeight(void *self)
{ dwprobe *o = self; dw_format *f = o ? o->ctx : NULL; return f ? f->maxh : 0.0f; }

/* DWRITE_OVERHANG_METRICS: how far the ink spills past the layout box. Nothing
 * here overhangs, and zeroes say exactly that. */
static MS int32_t dwtl_GetOverhangMetrics(void *self, void *out)
{ (void)self; if (!out) return DW_E_NOTIMPL; memset(out, 0, 16); return DW_S_OK; }

static MS int32_t dwtl_GetMetrics(void *self, void *out)
{
    dwprobe *o = self;
    dw_format *f = o ? o->ctx : NULL;
    float w, h;
    uint8_t *m = out;

    if (!out) return DW_E_NOTIMPL;
    dw_measure(f, &w, &h);
    memset(m, 0, 28);
    *(float *)(m + 8)  = w;                            /* width */
    *(float *)(m + 12) = w;                            /* + trailing whitespace */
    *(float *)(m + 16) = h;                            /* height */
    *(float *)(m + 20) = f ? f->maxw : w;              /* layoutWidth */
    *(float *)(m + 24) = f ? f->maxh : h;              /* layoutHeight */
    /* lineCount sits past those; one line, always. */
    return DW_S_OK;
}

/* The format's own getters. A probe is particularly bad for the two that
 * return a length rather than an HRESULT: E_NOTIMPL read as a UINT32 is
 * 2147500033, and a caller allocating that many characters gives up. */
static MS int32_t dwtf_GetFontCollection(void *self, void **out)
{
    (void)self;
    if (!out) return DW_E_NOTIMPL;
    if (!g_dwrite_syscoll) return DW_E_NOTIMPL;
    g_dwrite_syscoll->refs++;
    *out = g_dwrite_syscoll;
    return DW_S_OK;
}
static MS uint32_t dwtf_GetFontFamilyNameLength(void *self)
{
    dwprobe *o = self; dw_format *f = o ? o->ctx : NULL;
    uint32_t n = 0;
    if (f) while (f->family[n]) n++;
    return n;
}
static MS int32_t dwtf_GetFontFamilyName(void *self, uint16_t *buf, uint32_t size)
{
    dwprobe *o = self; dw_format *f = o ? o->ctx : NULL;
    uint32_t i = 0;
    if (!buf || !size) return DW_E_NOTIMPL;
    if (f) for (; f->family[i] && i + 1 < size; i++) buf[i] = f->family[i];
    buf[i] = 0;
    return DW_S_OK;
}
static MS uint32_t dwtf_GetFontWeight(void *self)  { (void)self; return 400; }
static MS uint32_t dwtf_GetFontStretch(void *self) { (void)self; return 5; }
static MS uint32_t dwtf_GetFontStyle(void *self)   { (void)self; return 0; }
static MS uint32_t dwtf_GetLocaleNameLength(void *self) { (void)self; return 5; }
static MS int32_t dwtf_GetLocaleName(void *self, uint16_t *buf, uint32_t size)
{
    static const char *en = "en-us";
    uint32_t i;
    (void)self;
    if (!buf || !size) return DW_E_NOTIMPL;
    for (i = 0; en[i] && i + 1 < size; i++) buf[i] = (uint16_t)en[i];
    buf[i] = 0;
    return DW_S_OK;
}

static dwprobe *dw_make_format(dw_format *f, const char *what)
{
    dwprobe *o = dwp_new(what);
    if (!o) return NULL;
    o->ctx = f;
    dwp_set(o, 3,  (void *)dwtf_SetAny);          /* SetTextAlignment      */
    dwp_set(o, 4,  (void *)dwtf_SetAny);          /* SetParagraphAlignment */
    dwp_set(o, 5,  (void *)dwtf_SetAny);          /* SetWordWrapping       */
    dwp_set(o, 6,  (void *)dwtf_SetAny);          /* SetReadingDirection   */
    dwp_set(o, 7,  (void *)dwtf_SetAny);          /* SetFlowDirection      */
    dwp_set(o, 19, (void *)dwtf_GetFontCollection);
    dwp_set(o, 20, (void *)dwtf_GetFontFamilyNameLength);
    dwp_set(o, 21, (void *)dwtf_GetFontFamilyName);
    dwp_set(o, 22, (void *)dwtf_GetFontWeight);
    dwp_set(o, 23, (void *)dwtf_GetFontStretch);
    dwp_set(o, 24, (void *)dwtf_GetFontStyle);
    dwp_set(o, 25, (void *)dwtf_GetFontSize);
    dwp_set(o, 26, (void *)dwtf_GetLocaleNameLength);
    dwp_set(o, 27, (void *)dwtf_GetLocaleName);
    return o;
}

/* IDWriteFactory::CreateTextFormat(family, collection, weight, style, stretch,
 *                                  size, locale, IDWriteTextFormat**) */
static MS int32_t dwf_CreateTextFormat(void *self, const uint16_t *family, void *coll,
                                       uint32_t weight, uint32_t style, uint32_t stretch,
                                       float size, const uint16_t *locale, void **out)
{
    dw_format *f;
    uint32_t i;
    (void)self; (void)coll; (void)weight; (void)style; (void)stretch; (void)locale;
    PLOG("  [dwrite] IDWriteFactory::CreateTextFormat(size %.1f)\n", (double)size);
    if (!out) return DW_E_NOTIMPL;
    if (!(f = calloc(1, sizeof *f))) return DW_E_NOTIMPL;
    f->size = size > 0 ? size : 12.0f;
    for (i = 0; family && family[i] && i + 1 < 64; i++) f->family[i] = family[i];
    *out = dw_make_format(f, "IDWriteTextFormat");
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}

/* IDWriteFactory::CreateTextLayout(string, len, format, maxW, maxH,
 *                                  IDWriteTextLayout**) */
static MS int32_t dwf_CreateTextLayout(void *self, const uint16_t *str, uint32_t len,
                                       void *format, float maxw, float maxh, void **out)
{
    dwprobe *fo = format;
    dw_format *src = fo ? fo->ctx : NULL, *f;
    dwprobe *o;
    uint32_t i;
    (void)self;
    PLOG("  [dwrite] IDWriteFactory::CreateTextLayout(%u chars)\n", len);
    if (!out) return DW_E_NOTIMPL;
    if (!(f = calloc(1, sizeof *f))) return DW_E_NOTIMPL;
    if (src) *f = *src;
    else f->size = 12.0f;
    f->maxw = maxw; f->maxh = maxh;
    for (i = 0; str && i < len && i + 1 < 512; i++) f->text[i] = str[i];
    f->textlen = i;
    if (!(o = dw_make_format(f, "IDWriteTextLayout"))) { free(f); return DW_E_NOTIMPL; }
    dwp_set(o, 28, (void *)dwtl_SetMaxWidth);
    dwp_set(o, 29, (void *)dwtl_SetMaxHeight);
    dwp_set(o, 42, (void *)dwtl_GetMaxWidth);
    dwp_set(o, 43, (void *)dwtl_GetMaxHeight);
    dwp_set(o, 57, (void *)dwtl_GetMetrics);
    dwp_set(o, 58, (void *)dwtl_GetOverhangMetrics);
    dwp_set(o, 60, (void *)dwtl_DetermineMinWidth);
    *out = o;
    return DW_S_OK;
}

/* IDWriteGdiInterop, the bridge back to GDI's way of naming a font.
 *
 * A plug-in that grew up on GDI describes what it wants with a LOGFONT and
 * asks DirectWrite to turn that into an IDWriteFont. There is one font here,
 * so the conversion is a formality -- but returning nothing means the caller
 * has no font at all, and it does not check. */
static MS int32_t dwgi_CreateFontFromLOGFONT(void *self, const void *lf, void **out)
{
    dwprobe *o = self;
    (void)lf;
    PLOG("  [dwrite] IDWriteGdiInterop::CreateFontFromLOGFONT\n");
    if (!out) return DW_E_NOTIMPL;
    *out = dw_make_font(o ? o->ctx : NULL);
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}
static MS int32_t dwgi_ConvertFontToLOGFONT(void *self, void *font, void *lf, int32_t *isSystem)
{
    (void)self; (void)font; (void)lf;
    PLOG("  [dwrite] IDWriteGdiInterop::ConvertFontToLOGFONT\n");
    if (isSystem) *isSystem = 1;
    return DW_S_OK;
}

/* IDWriteFactory::GetGdiInterop(IDWriteGdiInterop**) */
static MS int32_t dwf_GetGdiInterop(void *self, void **out)
{
    dwprobe *g;
    (void)self;
    PLOG("  [dwrite] IDWriteFactory::GetGdiInterop\n");
    if (!out) return DW_E_NOTIMPL;
    if (!(g = dwp_new("IDWriteGdiInterop"))) return DW_E_NOTIMPL;
    dwp_set(g, 3, (void *)dwgi_CreateFontFromLOGFONT);
    dwp_set(g, 4, (void *)dwgi_ConvertFontToLOGFONT);
    *out = g;
    return DW_S_OK;
}

/* Skia supplies its own IDWriteFontFileLoader and hands us a reference key;
 * asking that loader for a stream is how the font bytes come back to us. */
static void *g_dw_loader;

/* IDWriteFactory::RegisterFontFileLoader / UnregisterFontFileLoader */
static MS int32_t dwf_RegisterFontFileLoader(void *self, void *loader)
{
    (void)self;
    PLOG("  [dwrite] IDWriteFactory::RegisterFontFileLoader(%p)\n", loader);
    g_dw_loader = loader;
    return DW_S_OK;
}
static MS int32_t dwf_UnregisterFontFileLoader(void *self, void *loader)
{
    (void)self;
    PLOG("  [dwrite] IDWriteFactory::UnregisterFontFileLoader(%p)\n", loader);
    if (g_dw_loader == loader) g_dw_loader = NULL;
    return DW_S_OK;
}

/* A font file: remembers the loader and the opaque key Skia gave us, so the
 * bytes can be fetched on demand. */
typedef struct {
    void    *loader;
    uint8_t *key;
    uint32_t keysz;
} dw_fontfile;

/* IDWriteFontFile::Analyze(BOOL*, DWRITE_FONT_FILE_TYPE*, DWRITE_FONT_FACE_TYPE*, UINT32*) */
static MS int32_t dwff_Analyze(void *self, int32_t *isSupported, uint32_t *fileType,
                               uint32_t *faceType, uint32_t *numFaces)
{
    (void)self;
    PLOG("  [dwrite] IDWriteFontFile::Analyze -> TrueType, 1 face\n");
    if (isSupported) *isSupported = 1;
    if (fileType)    *fileType = 1;   /* DWRITE_FONT_FILE_TYPE_CFF=1? TRUETYPE=1 */
    if (faceType)    *faceType = 0;   /* DWRITE_FONT_FACE_TYPE_CFF=0/TRUETYPE=0 */
    if (numFaces)    *numFaces = 1;
    return DW_S_OK;
}
/* IDWriteFontFile::GetReferenceKey(void const**, UINT32*) */
static MS int32_t dwff_GetReferenceKey(void *self, const void **key, uint32_t *sz)
{
    dwprobe *o = self;
    dw_fontfile *f = o->ctx;
    if (key) *key = f ? f->key : NULL;
    if (sz)  *sz  = f ? f->keysz : 0;
    return DW_S_OK;
}
/* IDWriteFontFile::GetLoader(IDWriteFontFileLoader**) */
static MS int32_t dwff_GetLoader(void *self, void **loader)
{
    dwprobe *o = self;
    dw_fontfile *f = o->ctx;
    if (loader) *loader = f ? f->loader : NULL;
    return DW_S_OK;
}

/* Drops the open FreeType face so the next measurement reopens it from the
 * bytes just installed. Defined with the face itself, further down. */
static void dw_font_changed(void);

/* IDWriteFactory::CreateFontFileReference(WCHAR const*, FILETIME const*,
 *                                          IDWriteFontFile**)
 *
 * A font named by path rather than handed over as bytes. The file is read here
 * and becomes the face everything else measures and draws with, which is how a
 * plug-in shipping its own font gets to use it rather than whatever happened to
 * be registered first. */
static MS int32_t dwf_CreateFontFileReference(void *self, const uint16_t *path,
                                              const void *writetime, void **out)
{
    char n[1024], fixed[1024];
    dwprobe *ff;
    dw_fontfile *f;
    FILE *fp;
    long len;

    (void)self; (void)writetime;
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    w2c(path, n, sizeof n);
    path_fix(n, fixed, sizeof fixed);
    PLOG("  [dwrite] IDWriteFactory::CreateFontFileReference(\"%s\")\n", fixed);

    if ((fp = fopen(fixed, "rb")) != NULL) {
        fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (len > 0) {
            uint8_t *bytes = malloc((size_t)len);
            if (bytes && fread(bytes, 1, (size_t)len, fp) == (size_t)len) {
                /* Replaces whatever was registered before: a font asked for by
                 * name is the one the caller means to draw with. */
                free(g_font_bytes);
                g_font_bytes = bytes;
                g_font_size = (uint32_t)len;
                dw_font_changed();
            } else {
                free(bytes);
            }
        }
        fclose(fp);
    }

    if (!(ff = dwp_new("IDWriteFontFile"))) return DW_E_NOTIMPL;
    if (!(f = calloc(1, sizeof *f))) return DW_E_NOTIMPL;
    ff->ctx = f;
    dwp_set(ff, 3, (void *)dwff_GetReferenceKey);
    dwp_set(ff, 4, (void *)dwff_GetLoader);
    dwp_set(ff, 5, (void *)dwff_Analyze);
    *out = ff;
    return DW_S_OK;
}

/* Rendering parameters: gamma, contrast and the ClearType geometry. Nothing
 * here reads them -- glyphs come back from FreeType as coverage -- but the
 * object has to exist, because a caller stores it and passes it on. */
static MS int32_t dwf_CreateRenderingParams(void *self, void **out)
{
    (void)self;
    if (!out) return DW_E_NOTIMPL;
    *out = dwp_new("IDWriteRenderingParams");
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}
static MS int32_t dwf_CreateCustomRenderingParams(void *self, float gamma, float contrast,
                                                  float cleartype, uint32_t geom,
                                                  uint32_t mode, void **out)
{
    (void)self; (void)gamma; (void)contrast; (void)cleartype; (void)geom; (void)mode;
    if (!out) return DW_E_NOTIMPL;
    *out = dwp_new("IDWriteRenderingParams");
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}
/* The DirectWrite 2 form, which takes two more knobs and is the one a modern
 * plug-in reaches for. Same answer. */
static MS int32_t dwf_CreateCustomRenderingParams2(void *self, float gamma, float contrast,
                                                   float grayscale, float cleartype,
                                                   uint32_t geom, uint32_t mode,
                                                   uint32_t gridfit, void **out)
{
    (void)self; (void)gamma; (void)contrast; (void)grayscale; (void)cleartype;
    (void)geom; (void)mode; (void)gridfit;
    if (!out) return DW_E_NOTIMPL;
    *out = dwp_new("IDWriteRenderingParams");
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}

/* IDWriteFactory::CreateCustomFontFileReference(void const*, UINT32,
 *                                               IDWriteFontFileLoader*, IDWriteFontFile**) */
static MS int32_t dwf_CreateCustomFontFileReference(void *self, const void *key, uint32_t keysz,
                                                    void *loader, void **out)
{
    dwprobe *ff;
    dw_fontfile *f;

    (void)self;
    PLOG("  [dwrite] IDWriteFactory::CreateCustomFontFileReference(key %u bytes, loader %p)\n",
         keysz, loader);
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (!(ff = dwp_new("IDWriteFontFile"))) return DW_E_NOTIMPL;
    if (!(f = calloc(1, sizeof *f))) return DW_E_NOTIMPL;
    f->loader = loader ? loader : g_dw_loader;
    f->keysz = keysz;
    if (key && keysz) { f->key = malloc(keysz); if (f->key) memcpy(f->key, key, keysz); }
    ff->ctx = f;
    dwp_set(ff, 3, (void *)dwff_GetReferenceKey);
    dwp_set(ff, 4, (void *)dwff_GetLoader);
    dwp_set(ff, 5, (void *)dwff_Analyze);
    *out = ff;
    return DW_S_OK;
}

typedef struct dw_collection dw_collection;

/* Defined below, once the collection type is known. */

/* Skia also registers a collection loader and then asks us to build a custom
 * collection from it. Driving that loader's enumerator is how we get back the
 * IDWriteFontFile objects we handed out a moment earlier. */
static void *g_dw_collloader;

static MS int32_t dwf_RegisterFontCollectionLoader(void *self, void *loader)
{
    (void)self;
    PLOG("  [dwrite] IDWriteFactory::RegisterFontCollectionLoader(%p)\n", loader);
    g_dw_collloader = loader;
    return DW_S_OK;
}
static MS int32_t dwf_UnregisterFontCollectionLoader(void *self, void *loader)
{
    (void)self;
    if (g_dw_collloader == loader) g_dw_collloader = NULL;
    return DW_S_OK;
}

/* Calling conventions for the objects Skia gives us. */
typedef struct { void **vtbl; } dw_obj;
typedef MS int32_t (*fn_createEnum)(void *, void *, const void *, uint32_t, void **);
typedef MS int32_t (*fn_moveNext)(void *, int32_t *);
typedef MS int32_t (*fn_getCurFile)(void *, void **);

#define DW_MAX_FILES 8
struct dw_collection {
    void    *files[DW_MAX_FILES];
    int      nfiles;
};

/* IDWriteFactory::CreateCustomFontCollection(IDWriteFontCollectionLoader*,
 *                                            void const*, UINT32, IDWriteFontCollection**) */
static MS int32_t dwf_CreateCustomFontCollection(void *self, void *loader, const void *key,
                                                 uint32_t keysz, void **out)
{
    dwprobe *coll;
    dw_collection *c;
    dw_obj *ldr = loader ? loader : g_dw_collloader;
    void *enumerator = NULL;

    PLOG("  [dwrite] IDWriteFactory::CreateCustomFontCollection(key %u bytes)\n", keysz);
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (!ldr) return DW_E_NOTIMPL;

    if (!(coll = dwp_new("IDWriteFontCollection"))) return DW_E_NOTIMPL;
    if (!(c = calloc(1, sizeof *c))) return DW_E_NOTIMPL;
    coll->ctx = c;
    dwp_set(coll, 3, (void *)dwc_GetFontFamilyCount);
    dwp_set(coll, 4, (void *)dwc_GetFontFamily);

    /* IDWriteFontCollectionLoader::CreateEnumeratorFromKey is slot 3. */
    if (((fn_createEnum)ldr->vtbl[3])(ldr, self, key, keysz, &enumerator) == DW_S_OK
        && enumerator) {
        dw_obj *en = enumerator;
        int32_t more = 0;
        while (c->nfiles < DW_MAX_FILES &&
               ((fn_moveNext)en->vtbl[3])(en, &more) == DW_S_OK && more) {
            void *file = NULL;
            if (((fn_getCurFile)en->vtbl[4])(en, &file) == DW_S_OK && file)
                c->files[c->nfiles++] = file;
        }
        PLOG("  [dwrite]   enumerated %d font file(s)\n", c->nfiles);
    }
    *out = coll;
    return DW_S_OK;
}

/* One collection -> one family -> one font -> one font face. The plugin loads a
 * single embedded typeface, so there is nothing to choose between. */

static dwprobe *dw_make_fontface(dw_collection *c);
static dwprobe *dw_make_font(dw_collection *c);
static dwprobe *dw_make_family(dw_collection *c);

/* IDWriteFontCollection::GetFontFamilyCount() -> UINT32 (not an HRESULT) */
static MS uint32_t dwc_GetFontFamilyCount(void *self)
{ (void)self; PLOG("  [dwrite] IDWriteFontCollection::GetFontFamilyCount -> 1\n"); return 1; }

/* IDWriteFontCollection::GetFontFamily(UINT32, IDWriteFontFamily**) */
static MS int32_t dwc_GetFontFamily(void *self, uint32_t idx, void **out)
{
    dwprobe *o = self;
    PLOG("  [dwrite] IDWriteFontCollection::GetFontFamily(%u)\n", idx);
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (idx != 0) return DW_E_NOTIMPL;
    *out = dw_make_family(o->ctx);
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}

/* IDWriteFontCollection::FindFamilyName(WCHAR const*, UINT32*, BOOL*) */
static MS int32_t dwc_FindFamilyName(void *self, const uint16_t *name,
                                     uint32_t *index, int32_t *exists)
{
    (void)self; (void)name;
    /* One family, and it answers to every name. A plug-in asking for a font it
     * cannot have is better served by the one available than by "no such
     * font", which it typically treats as a reason to draw nothing. */
    if (index)  *index = 0;
    if (exists) *exists = 1;
    return DW_S_OK;
}

/* IDWriteFontFamily::GetFontCount() -> UINT32 */
static MS uint32_t dwfam_GetFontCount(void *self)
{ (void)self; return 1; }

/* IDWriteFontFamily::GetFont(UINT32, IDWriteFont**) */
static MS int32_t dwfam_GetFont(void *self, uint32_t idx, void **out)
{
    dwprobe *o = self;
    PLOG("  [dwrite] IDWriteFontFamily::GetFont(%u)\n", idx);
    if (!out) return DW_E_NOTIMPL;
    *out = (idx == 0) ? dw_make_font(o->ctx) : NULL;
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}

/* IDWriteFontFamily::GetFirstMatchingFont(weight, stretch, style, IDWriteFont**) */
static MS int32_t dwfam_GetFirstMatchingFont(void *self, uint32_t w, uint32_t st,
                                             uint32_t sty, void **out)
{
    dwprobe *o = self;
    (void)w; (void)st; (void)sty;
    PLOG("  [dwrite] IDWriteFontFamily::GetFirstMatchingFont\n");
    if (!out) return DW_E_NOTIMPL;
    *out = dw_make_font(o->ctx);
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}

/* IDWriteFont::CreateFontFace(IDWriteFontFace**) */
static MS int32_t dwfont_CreateFontFace(void *self, void **out)
{
    dwprobe *o = self;
    PLOG("  [dwrite] IDWriteFont::CreateFontFace\n");
    if (!out) return DW_E_NOTIMPL;
    *out = dw_make_fontface(o->ctx);
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}
static MS uint32_t dwfont_GetWeight(void *self)  { (void)self; return 400; }
static MS uint32_t dwfont_GetStretch(void *self) { (void)self; return 5; }
static MS uint32_t dwfont_GetStyle(void *self)   { (void)self; return 0; }
static MS int32_t  dwfont_IsSymbolFont(void *self) { (void)self; return 0; }
static MS uint32_t dwfont_GetSimulations(void *self) { (void)self; return 0; }

/* ---- the font face ------------------------------------------------------
 *
 * Backed directly by the sfnt bytes the plugin installed. Skia reads OpenType
 * tables straight out of the face, so serving them from the file is both the
 * simplest and the most faithful implementation. */

/* Locate an OpenType table. DirectWrite passes the tag in the byte order it
 * appears in the file, packed little-endian into a UINT32. */
static const uint8_t *dw_find_table(uint32_t tag, uint32_t *size)
{
    uint32_t ntables, i;
    uint8_t want[4];

    if (size) *size = 0;
    if (!g_font_bytes || g_font_size < 12) return NULL;
    want[0] = (uint8_t)(tag);
    want[1] = (uint8_t)(tag >> 8);
    want[2] = (uint8_t)(tag >> 16);
    want[3] = (uint8_t)(tag >> 24);

    ntables = be16(g_font_bytes + 4);
    for (i = 0; i < ntables; i++) {
        const uint8_t *rec = g_font_bytes + 12 + i * 16;
        if ((uint32_t)(rec - g_font_bytes) + 16 > g_font_size) break;
        if (!memcmp(rec, want, 4)) {
            uint32_t off = be32(rec + 8), len = be32(rec + 12);
            if (off + len > g_font_size) return NULL;
            if (size) *size = len;
            return g_font_bytes + off;
        }
    }
    return NULL;
}

/* IDWriteFontFace::TryGetFontTable(tag, void const**, UINT32*, void**, BOOL*) */
static MS int32_t dwfa_TryGetFontTable(void *self, uint32_t tag, const void **data,
                                       uint32_t *size, void **ctx, int32_t *exists)
{
    uint32_t sz = 0;
    const uint8_t *p = dw_find_table(tag, &sz);
    char t[5];

    (void)self;
    t[0] = (char)tag; t[1] = (char)(tag >> 8);
    t[2] = (char)(tag >> 16); t[3] = (char)(tag >> 24); t[4] = 0;
    if (ctx) *ctx = NULL;
    if (data) *data = p;
    if (size) *size = sz;
    if (exists) *exists = p ? 1 : 0;
    PLOG("  [dwrite] IDWriteFontFace::TryGetFontTable('%s') -> %s %u bytes\n",
         t, p ? "found" : "absent", sz);
    return DW_S_OK;
}
static MS void dwfa_ReleaseFontTable(void *self, void *ctx) { (void)self; (void)ctx; }

static MS uint32_t dwfa_GetType(void *self)        { (void)self; return 1; /* TRUETYPE */ }
static MS uint32_t dwfa_GetIndex(void *self)       { (void)self; return 0; }
static MS uint32_t dwfa_GetSimulations(void *self) { (void)self; return 0; }
static MS int32_t  dwfa_IsSymbolFont(void *self)   { (void)self; return 0; }

/* IDWriteFontFace::GetGlyphCount() -> UINT16, from maxp. */
static MS uint16_t dwfa_GetGlyphCount(void *self)
{
    uint32_t sz = 0;
    const uint8_t *maxp = dw_find_table(0x7078616D /* 'maxp' */, &sz);
    (void)self;
    return (maxp && sz >= 6) ? be16(maxp + 4) : 0;
}

/* IDWriteFontFace::GetMetrics(DWRITE_FONT_METRICS*) -- design units from head,
 * hhea and OS/2. Ten 16-bit fields, so 20 bytes at both widths. */
#define DWRITE_FONT_METRICS_SIZE 20
static MS void dwfa_GetMetrics(void *self, void *out)
{
    uint32_t sz = 0;
    const uint8_t *head = dw_find_table(0x64616568 /* 'head' */, &sz);
    uint16_t upem = (head && sz >= 20) ? be16(head + 18) : 1000;
    const uint8_t *hhea = dw_find_table(0x61656868 /* 'hhea' */, &sz);
    uint8_t *m = out;

    (void)self;
    if (!out) return;
    /* DWRITE_FONT_METRICS is ten 16-bit fields = 20 bytes, at both widths. The
     * caller's buffer is often a stack local, so zeroing 24 wrote 4 bytes past
     * it -- on i386 that is exactly where MSVC keeps the /GS cookie, and the
     * plugin then killed itself with __fastfail on the way out of the function
     * that asked for the metrics. */
    memset(m, 0, DWRITE_FONT_METRICS_SIZE);
    *(uint16_t *)(m + 0) = upem;                                   /* designUnitsPerEm */
    *(uint16_t *)(m + 2) = (hhea && sz >= 10) ? (uint16_t)be16(hhea + 4) : (uint16_t)(upem * 8 / 10);
    *(uint16_t *)(m + 4) = (hhea && sz >= 10) ? (uint16_t)(-(int16_t)be16(hhea + 6)) : (uint16_t)(upem / 5);
    *(uint16_t *)(m + 6) = 0;                                      /* lineGap */
    *(uint16_t *)(m + 8) = (uint16_t)(upem * 7 / 10);              /* capHeight */
    *(uint16_t *)(m + 10) = (uint16_t)(upem / 2);                  /* xHeight */
    *(int16_t *)(m + 12) = (int16_t)(-(int32_t)upem / 10);         /* underlinePosition */
    *(int16_t *)(m + 14) = (int16_t)(upem / 20);                   /* underlineThickness */
    *(int16_t *)(m + 16) = (int16_t)(upem / 3);                    /* strikethroughPosition */
    *(int16_t *)(m + 18) = (int16_t)(upem / 20);                   /* strikethroughThickness */
}

/* Glyph lookup and metrics go through FreeType rather than a hand-rolled cmap
 * and hmtx reader: it is already a dependency-free system library here, and it
 * handles every cmap subtable format correctly. */
static FT_Library g_ft;
static FT_Face    g_ftface;

static void dw_font_changed(void)
{ if (g_ftface) { FT_Done_Face(g_ftface); g_ftface = NULL; } }

static FT_Face dw_ftface(void)
{
    if (g_ftface) return g_ftface;
    if (!g_font_bytes || !g_font_size) return NULL;
    if (!g_ft && FT_Init_FreeType(&g_ft)) return NULL;
    if (FT_New_Memory_Face(g_ft, g_font_bytes, (FT_Long)g_font_size, 0, &g_ftface)) {
        g_ftface = NULL;
        return NULL;
    }
    PLOG("  [dwrite] FreeType opened '%s' (%ld glyphs, %d upem)\n",
         g_ftface->family_name ? g_ftface->family_name : "?",
         (long)g_ftface->num_glyphs, g_ftface->units_per_EM);
    return g_ftface;
}

/* IDWriteFontFace::GetGlyphIndices(UINT32 const*, UINT32, UINT16*) */
static MS int32_t dwfa_GetGlyphIndices(void *self, const uint32_t *cps, uint32_t count,
                                       uint16_t *out)
{
    FT_Face f = dw_ftface();
    uint32_t i;
    (void)self;
    if (!out) return DW_E_NOTIMPL;
    for (i = 0; i < count; i++)
        out[i] = f ? (uint16_t)FT_Get_Char_Index(f, cps ? cps[i] : 0) : 0;
    return DW_S_OK;
}

/* DWRITE_GLYPH_METRICS is 7 fields of 4 bytes, in font design units. */
static MS int32_t dwfa_GetDesignGlyphMetrics(void *self, const uint16_t *glyphs, uint32_t count,
                                             void *metrics, int32_t sideways)
{
    FT_Face f = dw_ftface();
    uint8_t *m = metrics;
    uint32_t i;

    (void)self; (void)sideways;
    if (!m) return DW_E_NOTIMPL;
    memset(m, 0, (size_t)count * 28);
    if (!f) return DW_S_OK;

    for (i = 0; i < count; i++) {
        uint8_t *e = m + (size_t)i * 28;
        FT_UInt g = glyphs ? glyphs[i] : 0;
        if (FT_Load_Glyph(f, g, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING)) continue;
        {
            FT_Glyph_Metrics *gm = &f->glyph->metrics;
            int32_t adv = (int32_t)gm->horiAdvance;
            int32_t lsb = (int32_t)gm->horiBearingX;
            int32_t rsb = adv - lsb - (int32_t)gm->width;
            int32_t asc = f->ascender, desc = -f->descender;
            *(int32_t *)(e + 0)  = lsb;
            *(uint32_t *)(e + 4) = (uint32_t)adv;
            *(int32_t *)(e + 8)  = rsb;
            *(int32_t *)(e + 12) = asc - (int32_t)gm->horiBearingY;
            *(uint32_t *)(e + 16) = (uint32_t)(asc + desc);
            *(int32_t *)(e + 20) = (int32_t)(gm->horiBearingY - (FT_Pos)gm->height) + desc;
            *(int32_t *)(e + 24) = asc;
        }
    }
    return DW_S_OK;
}

/* IDWriteFontFace::GetFiles(UINT32*, IDWriteFontFile**) */
static MS int32_t dwfa_GetFiles(void *self, uint32_t *count, void **files)
{
    dwprobe *o = self;
    dw_collection *c = o->ctx;
    uint32_t have = (c && c->nfiles > 0) ? 1u : 0u;
    if (!count) return DW_E_NOTIMPL;
    if (!files) { *count = have; return DW_S_OK; }
    if (have && *count >= 1) files[0] = c->files[0];
    *count = have;
    return DW_S_OK;
}

static dwprobe *dw_make_fontface(dw_collection *c)
{
    dwprobe *f = dwp_new("IDWriteFontFace");
    if (!f) return NULL;
    f->ctx = c;
    dwp_set(f,  3, (void *)dwfa_GetType);
    dwp_set(f,  4, (void *)dwfa_GetFiles);
    dwp_set(f,  5, (void *)dwfa_GetIndex);
    dwp_set(f,  6, (void *)dwfa_GetSimulations);
    dwp_set(f,  7, (void *)dwfa_IsSymbolFont);
    dwp_set(f,  8, (void *)dwfa_GetMetrics);
    dwp_set(f,  9, (void *)dwfa_GetGlyphCount);
    dwp_set(f, 10, (void *)dwfa_GetDesignGlyphMetrics);
    dwp_set(f, 11, (void *)dwfa_GetGlyphIndices);
    dwp_set(f, 12, (void *)dwfa_TryGetFontTable);
    dwp_set(f, 13, (void *)dwfa_ReleaseFontTable);
    return f;
}

/* The one family name this shim can honestly claim: whatever FreeType actually
 * opened. A plug-in asks for it to measure and to populate a font menu, so a
 * wrong name is better than none but the real one is better still. */
static const char *dw_family_name(void)
{
    FT_Face f = dw_ftface();
    if (f && f->family_name && f->family_name[0]) return f->family_name;
    return "Sans";
}

/* IDWriteLocalizedStrings, over a single string in a single locale.
 *
 * A plug-in reads a family name by asking the family for its names, finding
 * the entry for its locale and copying the string out. Three calls, and every
 * one of them was a probe. */
static MS uint32_t dwls_GetCount(void *self) { (void)self; return 1; }
static MS int32_t dwls_FindLocaleName(void *self, const uint16_t *locale,
                                      uint32_t *index, int32_t *exists)
{
    (void)self; (void)locale;
    /* Whatever locale is asked for is the one entry there is. Reporting "not
     * found" would be answering a question the caller cannot act on -- it has
     * nowhere else to look. */
    if (index)  *index = 0;
    if (exists) *exists = 1;
    return DW_S_OK;
}
static MS int32_t dwls_GetStringLength(void *self, uint32_t idx, uint32_t *len)
{
    (void)self;
    if (!len || idx) return DW_E_NOTIMPL;
    *len = (uint32_t)strlen(dw_family_name());
    return DW_S_OK;
}
static MS int32_t dwls_GetString(void *self, uint32_t idx, uint16_t *buf, uint32_t size)
{
    const char *n = dw_family_name();
    uint32_t i;
    (void)self;
    if (!buf || !size || idx) return DW_E_NOTIMPL;
    /* The size is in characters and includes the terminator, which is the part
     * a caller sizing from GetStringLength+1 depends on. */
    for (i = 0; n[i] && i + 1 < size; i++) buf[i] = (uint16_t)(unsigned char)n[i];
    buf[i] = 0;
    return DW_S_OK;
}
static MS int32_t dwls_GetLocaleNameLength(void *self, uint32_t idx, uint32_t *len)
{ (void)self; if (!len || idx) return DW_E_NOTIMPL; *len = 5; return DW_S_OK; }
static MS int32_t dwls_GetLocaleName(void *self, uint32_t idx, uint16_t *buf, uint32_t size)
{
    static const char *en = "en-us";
    uint32_t i;
    (void)self;
    if (!buf || !size || idx) return DW_E_NOTIMPL;
    for (i = 0; en[i] && i + 1 < size; i++) buf[i] = (uint16_t)en[i];
    buf[i] = 0;
    return DW_S_OK;
}

static dwprobe *dw_make_strings(void)
{
    dwprobe *s = dwp_new("IDWriteLocalizedStrings");
    if (!s) return NULL;
    dwp_set(s, 3, (void *)dwls_GetCount);
    dwp_set(s, 4, (void *)dwls_FindLocaleName);
    dwp_set(s, 5, (void *)dwls_GetLocaleNameLength);
    dwp_set(s, 6, (void *)dwls_GetLocaleName);
    dwp_set(s, 7, (void *)dwls_GetStringLength);
    dwp_set(s, 8, (void *)dwls_GetString);
    return s;
}

/* IDWriteFontFamily::GetFamilyNames(IDWriteLocalizedStrings**) */
static MS int32_t dwfam_GetFamilyNames(void *self, void **out)
{
    (void)self;
    if (!out) return DW_E_NOTIMPL;
    *out = dw_make_strings();
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}

/* IDWriteFontFamily::GetFontCollection(IDWriteFontCollection**), inherited
 * from IDWriteFontList. */
static MS int32_t dwfam_GetFontCollection(void *self, void **out)
{
    (void)self;
    if (!out) return DW_E_NOTIMPL;
    if (!g_dwrite_syscoll) return DW_E_NOTIMPL;
    g_dwrite_syscoll->refs++;
    *out = g_dwrite_syscoll;
    return DW_S_OK;
}

static dwprobe *dw_make_family(dw_collection *c)
{
    dwprobe *f = dwp_new("IDWriteFontFamily");
    if (!f) return NULL;
    f->ctx = c;
    dwp_set(f, 3, (void *)dwfam_GetFontCollection);
    dwp_set(f, 4, (void *)dwfam_GetFontCount);
    dwp_set(f, 5, (void *)dwfam_GetFont);
    dwp_set(f, 6, (void *)dwfam_GetFamilyNames);
    dwp_set(f, 7, (void *)dwfam_GetFirstMatchingFont);
    return f;
}
/* The face name, as distinct from the family name: "Regular" is what a font
 * with no style variants reports, and there is only the one here. */
static MS int32_t dwls_GetStringRegular(void *self, uint32_t idx, uint16_t *buf, uint32_t size)
{
    static const char *n = "Regular";
    uint32_t i;
    (void)self;
    if (!buf || !size || idx) return DW_E_NOTIMPL;
    for (i = 0; n[i] && i + 1 < size; i++) buf[i] = (uint16_t)n[i];
    buf[i] = 0;
    return DW_S_OK;
}
static MS int32_t dwls_GetStringLengthRegular(void *self, uint32_t idx, uint32_t *len)
{ (void)self; if (!len || idx) return DW_E_NOTIMPL; *len = 7; return DW_S_OK; }

/* IDWriteFont::GetFaceNames(IDWriteLocalizedStrings**) */
static MS int32_t dwfont_GetFaceNames(void *self, void **out)
{
    dwprobe *s;
    (void)self;
    if (!out) return DW_E_NOTIMPL;
    if (!(s = dw_make_strings())) return DW_E_NOTIMPL;
    dwp_set(s, 7, (void *)dwls_GetStringLengthRegular);
    dwp_set(s, 8, (void *)dwls_GetStringRegular);
    *out = s;
    return DW_S_OK;
}

/* IDWriteFont::GetFontFamily(IDWriteFontFamily**) */
static MS int32_t dwfont_GetFontFamily(void *self, void **out)
{
    dwprobe *o = self;
    if (!out) return DW_E_NOTIMPL;
    *out = dw_make_family(o ? o->ctx : NULL);
    return *out ? DW_S_OK : DW_E_NOTIMPL;
}

/* IDWriteFont::GetInformationalStrings(id, IDWriteLocalizedStrings**, BOOL*) */
static MS int32_t dwfont_GetInformationalStrings(void *self, uint32_t id,
                                                 void **out, int32_t *exists)
{
    (void)self; (void)id;
    /* Nothing to report, said properly: a caller reading *exists first will
     * not then dereference the null it was handed. */
    if (out)    *out = NULL;
    if (exists) *exists = 0;
    return DW_S_OK;
}

/* IDWriteFont::GetMetrics(DWRITE_FONT_METRICS*) -- the same numbers the font
 * face reports, because it is the same face. */
static MS void dwfont_GetMetrics(void *self, void *out)
{ dwfa_GetMetrics(self, out); }

/* IDWriteFont::HasCharacter(UINT32, BOOL*) */
static MS int32_t dwfont_HasCharacter(void *self, uint32_t ch, int32_t *has)
{
    FT_Face f = dw_ftface();
    (void)self;
    if (!has) return DW_E_NOTIMPL;
    *has = (f && FT_Get_Char_Index(f, (FT_ULong)ch)) ? 1 : 0;
    return DW_S_OK;
}

static dwprobe *dw_make_font(dw_collection *c)
{
    dwprobe *f = dwp_new("IDWriteFont");
    if (!f) return NULL;
    f->ctx = c;
    dwp_set(f, 3,  (void *)dwfont_GetFontFamily);
    dwp_set(f, 4,  (void *)dwfont_GetWeight);
    dwp_set(f, 5,  (void *)dwfont_GetStretch);
    dwp_set(f, 6,  (void *)dwfont_GetStyle);
    dwp_set(f, 7,  (void *)dwfont_IsSymbolFont);
    dwp_set(f, 8,  (void *)dwfont_GetFaceNames);
    dwp_set(f, 9,  (void *)dwfont_GetInformationalStrings);
    dwp_set(f, 10, (void *)dwfont_GetSimulations);
    dwp_set(f, 11, (void *)dwfont_GetMetrics);
    dwp_set(f, 12, (void *)dwfont_HasCharacter);
    dwp_set(f, 13, (void *)dwfont_CreateFontFace);
    return f;
}

/* ---- GDI text -----------------------------------------------------------
 *
 * TextOut and its relatives, served from the same FreeType face DirectWrite
 * measures with. This exists because the GDI text calls in win32gui.h were
 * no-ops: the Win32 layer was written for plug-ins that rasterise their own
 * interface and blit it, and a plug-in from the GDI era draws its text by
 * asking GDI to. Both kinds are in the corpus.
 *
 * Byte strings only, mapped through the face's Unicode cmap. That is right for
 * ASCII and for the Latin-1 range every plug-in here labels its controls in;
 * a genuine multi-byte code page would need the code page. */
static int dw_text_measure(const char *s, int n, int em_px,
                           int *w, int *h, int *ascent)
{
    FT_Face f = dw_ftface();
    int i, adv = 0;

    if (w) *w = 0;
    if (h) *h = 0;
    if (ascent) *ascent = 0;
    if (!f || em_px <= 0) return 0;
    FT_Set_Pixel_Sizes(f, 0, (FT_UInt)em_px);
    for (i = 0; i < n && s; i++) {
        FT_UInt gi = FT_Get_Char_Index(f, (FT_ULong)(unsigned char)s[i]);
        if (FT_Load_Glyph(f, gi, FT_LOAD_DEFAULT)) continue;
        adv += (int)(f->glyph->advance.x >> 6);
    }
    if (w) *w = adv;
    if (h) *h = (int)((f->size->metrics.ascender - f->size->metrics.descender) >> 6);
    if (ascent) *ascent = (int)(f->size->metrics.ascender >> 6);
    return 1;
}

/* Draw at a baseline, blended by glyph coverage. `clip4` is left, top, right,
 * bottom in device space, or NULL. */
static int dw_text_draw(uint32_t *px, int pw, int ph, int x, int baseline,
                        const char *s, int n, int em_px, uint32_t rgb,
                        const int32_t *clip4)
{
    FT_Face f = dw_ftface();
    int i, pen = x;

    if (!f || !px || em_px <= 0 || !s) return 0;
    FT_Set_Pixel_Sizes(f, 0, (FT_UInt)em_px);
    for (i = 0; i < n; i++) {
        FT_UInt gi = FT_Get_Char_Index(f, (FT_ULong)(unsigned char)s[i]);
        int row, col, gx, gy;

        if (FT_Load_Glyph(f, gi, FT_LOAD_DEFAULT)) continue;
        if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL)) {
            pen += (int)(f->glyph->advance.x >> 6);
            continue;
        }
        gx = pen + f->glyph->bitmap_left;
        gy = baseline - f->glyph->bitmap_top;
        for (row = 0; row < (int)f->glyph->bitmap.rows; row++) {
            int ty = gy + row;
            if (ty < 0 || ty >= ph) continue;
            if (clip4 && (ty < clip4[1] || ty >= clip4[3])) continue;
            for (col = 0; col < (int)f->glyph->bitmap.width; col++) {
                int tx = gx + col;
                uint32_t dst, r, g, b;
                uint8_t a;
                if (tx < 0 || tx >= pw) continue;
                if (clip4 && (tx < clip4[0] || tx >= clip4[2])) continue;
                a = f->glyph->bitmap.buffer[row * f->glyph->bitmap.pitch + col];
                if (!a) continue;
                dst = px[(size_t)ty * pw + tx];
                r = (((rgb >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * (255 - a)) / 255;
                g = (((rgb >>  8) & 0xFF) * a + ((dst >>  8) & 0xFF) * (255 - a)) / 255;
                b = ((  rgb        & 0xFF) * a + ( dst        & 0xFF) * (255 - a)) / 255;
                px[(size_t)ty * pw + tx] = (r << 16) | (g << 8) | b;
            }
        }
        pen += (int)(f->glyph->advance.x >> 6);
    }
    return 1;
}

/* ---- glyph rasterisation ------------------------------------------------
 *
 * Skia asks DirectWrite to turn a glyph run into an alpha texture. FreeType
 * renders the same glyphs, so the run is kept and rasterised on demand. */

/* DWRITE_GLYPH_RUN: fontFace, emSize, glyphCount, indices, advances, offsets,
 * isSideways, bidiLevel -- 48 bytes with the natural padding. */
typedef struct {
    void     *fontFace;
    float     emSize;
    uint32_t  glyphCount;
    const uint16_t *glyphIndices;
    const float    *glyphAdvances;
    const float    *glyphOffsets;   /* DWRITE_GLYPH_OFFSET: two floats each */
    int32_t   isSideways;
    uint32_t  bidiLevel;
} dw_glyph_run;

typedef struct {
    float     emSize, originX, originY;
    uint32_t  count;
    uint16_t *indices;
    float    *advances;
    float    *offsets;
} dw_run;

/* Lay the run out and either measure it or draw it. Returns the pixel bounds. */
static void dw_run_render(dw_run *r, int aliased, uint8_t *dst,
                          int32_t *bl, int32_t *bt, int32_t *br, int32_t *bb)
{
    FT_Face f = dw_ftface();
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
    uint32_t i;
    float penx;

    if (!f || !r || !r->count) { *bl = *bt = *br = *bb = 0; return; }
    FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(r->emSize + 0.5f));

    /* Two passes: measure, then draw once the caller knows the bounds. */
    penx = r->originX;
    for (i = 0; i < r->count; i++) {
        float ox = r->offsets ? r->offsets[i * 2] : 0.0f;
        float oy = r->offsets ? r->offsets[i * 2 + 1] : 0.0f;
        int gx, gy;
        if (FT_Load_Glyph(f, r->indices[i], FT_LOAD_DEFAULT)) goto advance;
        if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL)) goto advance;
        gx = (int)(penx + ox) + f->glyph->bitmap_left;
        gy = (int)(r->originY - oy) - f->glyph->bitmap_top;
        if (gx < minx) minx = gx;
        if (gy < miny) miny = gy;
        if (gx + (int)f->glyph->bitmap.width > maxx) maxx = gx + (int)f->glyph->bitmap.width;
        if (gy + (int)f->glyph->bitmap.rows  > maxy) maxy = gy + (int)f->glyph->bitmap.rows;
        if (dst) {
            int row, col;
            int W = *br - *bl, H = *bb - *bt;
            int bpp = aliased ? 1 : 3;
            for (row = 0; row < (int)f->glyph->bitmap.rows; row++) {
                int ty = gy + row - *bt;
                if (ty < 0 || ty >= H) continue;
                for (col = 0; col < (int)f->glyph->bitmap.width; col++) {
                    int tx = gx + col - *bl;
                    uint8_t v = f->glyph->bitmap.buffer[row * f->glyph->bitmap.pitch + col];
                    int k;
                    if (tx < 0 || tx >= W) continue;
                    for (k = 0; k < bpp; k++) {
                        uint8_t *p = &dst[((size_t)ty * W + tx) * bpp + k];
                        if (v > *p) *p = v;      /* glyphs may overlap */
                    }
                }
            }
        }
    advance:
        penx += r->advances ? r->advances[i] : (float)(f->glyph->advance.x >> 6);
    }
    if (maxx < minx) { minx = miny = maxx = maxy = 0; }
    if (!dst) { *bl = minx; *bt = miny; *br = maxx; *bb = maxy; }
}

static MS int32_t dwgra_GetAlphaTextureBounds(void *self, uint32_t type, void *rect);
static MS int32_t dwgra_CreateAlphaTexture(void *self, uint32_t type, const void *rect,
                                           uint8_t *buf, uint32_t bufSize);

/* IDWriteGlyphRunAnalysis::GetAlphaTextureBounds(DWRITE_TEXTURE_TYPE, RECT*) */
static MS int32_t dwgra_GetAlphaTextureBounds(void *self, uint32_t type, void *rect)
{
    dwprobe *o = self;
    dw_run *r = o->ctx;
    int32_t *rc = rect;
    (void)type;
    if (!rc) return DW_E_NOTIMPL;
    dw_run_render(r, type == 0, NULL, &rc[0], &rc[1], &rc[2], &rc[3]);
    return DW_S_OK;
}

/* IDWriteGlyphRunAnalysis::CreateAlphaTexture(type, RECT const*, BYTE*, UINT32) */
static MS int32_t dwgra_CreateAlphaTexture(void *self, uint32_t type, const void *rect,
                                           uint8_t *buf, uint32_t bufSize)
{
    dwprobe *o = self;
    dw_run *r = o->ctx;
    const int32_t *rc = rect;
    int32_t l, t, rr, b;

    if (!rc || !buf) return DW_E_NOTIMPL;
    l = rc[0]; t = rc[1]; rr = rc[2]; b = rc[3];
    memset(buf, 0, bufSize);
    dw_run_render(r, type == 0, buf, &l, &t, &rr, &b);
    return DW_S_OK;
}

static MS uint32_t dwgra_release(void *self)
{
    dwprobe *o = self;
    if (--o->refs > 0) return (uint32_t)o->refs;
    {
        dw_run *r = o->ctx;
        if (r) { free(r->indices); free(r->advances); free(r->offsets); free(r); }
    }
    free(o);
    return 0;
}
static MS int32_t dwgra_qi(void *self, const void *iid, void **out)
{
    dwprobe *o = self;
    (void)iid;
    if (out) { *out = self; o->refs++; }
    return DW_S_OK;
}
static MS uint32_t dwgra_addref(void *self) { dwprobe *o = self; return (uint32_t)(++o->refs); }

static void *g_dwgra_vtbl[8];

/* IDWriteFactory::CreateGlyphRunAnalysis(run, ppd, transform, renderMode,
 *                                        measureMode, originX, originY, out) */
static MS int32_t dwf_CreateGlyphRunAnalysis(void *self, const void *runp, float ppd,
                                             const void *transform, uint32_t renderMode,
                                             uint32_t measureMode, float ox, float oy,
                                             void **out)
{
    const dw_glyph_run *g = runp;
    dwprobe *a;
    dw_run *r;

    (void)self; (void)transform; (void)renderMode; (void)measureMode;
    if (!out) return DW_E_NOTIMPL;
    *out = NULL;
    if (!g || !g->glyphCount) return DW_E_NOTIMPL;

    if (!g_dwgra_vtbl[0]) {
        g_dwgra_vtbl[0] = (void *)dwgra_qi;
        g_dwgra_vtbl[1] = (void *)dwgra_addref;
        g_dwgra_vtbl[2] = (void *)dwgra_release;
        g_dwgra_vtbl[3] = (void *)dwgra_GetAlphaTextureBounds;
        g_dwgra_vtbl[4] = (void *)dwgra_CreateAlphaTexture;
        g_dwgra_vtbl[5] = (void *)dwgra_release;    /* GetAlphaBlendParams: unused */
    }
    if (!(a = dwp_new_fixed("IDWriteGlyphRunAnalysis", g_dwgra_vtbl))) return DW_E_NOTIMPL;
    if (!(r = calloc(1, sizeof *r))) return DW_E_NOTIMPL;
    r->emSize  = g->emSize * (ppd > 0.0f ? ppd : 1.0f);
    r->originX = ox;
    r->originY = oy;
    r->count   = g->glyphCount;
    r->indices = malloc(sizeof(uint16_t) * g->glyphCount);
    if (r->indices && g->glyphIndices)
        memcpy(r->indices, g->glyphIndices, sizeof(uint16_t) * g->glyphCount);
    if (g->glyphAdvances) {
        r->advances = malloc(sizeof(float) * g->glyphCount);
        if (r->advances) memcpy(r->advances, g->glyphAdvances, sizeof(float) * g->glyphCount);
    }
    if (g->glyphOffsets) {
        r->offsets = malloc(sizeof(float) * 2 * g->glyphCount);
        if (r->offsets) memcpy(r->offsets, g->glyphOffsets, sizeof(float) * 2 * g->glyphCount);
    }
    a->ctx = r;
    *out = a;
    return DW_S_OK;
}

/* Drop all per-plugin font state. FT_New_Memory_Face references the buffer it is
 * given rather than copying it, so the face must go before the bytes do --
 * otherwise loading a second plugin leaves FreeType pointing at freed memory. */
static dwprobe *g_dwrite_factory;

/* A plugin whose font never reached DirectWrite will null-deref on its first
 * paint: Skia's font cache stays empty and iPlug2 dereferences the miss. Having
 * registered a font but never asked for a factory is exactly that state, and it
 * is worth detecting so the editor can be refused instead of crashing. */
static int dw_font_pipeline_ok(void)
{ return !g_font_bytes || g_dwrite_factory != NULL; }

/* The font buffer is about to be replaced or freed. */
static void dw_font_bytes_changing(void)
{ if (g_ftface) { FT_Done_Face(g_ftface); g_ftface = NULL; } }

static void dw_reset(void)
{
    if (g_ftface) { FT_Done_Face(g_ftface); g_ftface = NULL; }
    free(g_font_bytes);
    g_font_bytes = NULL;
    g_font_size = 0;
    g_font_family[0] = 0;
    /* The cached factory and collection belong to the plugin going away; the
     * next one builds its own through DWriteCreateFactory. */
    g_dwrite_factory = NULL;
    g_dwrite_syscoll = NULL;
    g_dw_loader = NULL;
    g_dw_collloader = NULL;
}

/* ---- the factory -------------------------------------------------------- */

/* DWriteCreateFactory(DWRITE_FACTORY_TYPE, REFIID, IUnknown**) */
static MS int32_t st_DWriteCreateFactory(uint32_t type, const void *iid, void **out)
{
    PLOG("  [dwrite] DWriteCreateFactory(type=%u)\n", type);
    if (!out) return DW_E_NOTIMPL;
    if (!g_dwrite_factory) {
        g_dwrite_factory = dwp_new("IDWriteFactory");
        if (g_dwrite_factory) {
            g_dwrite_factory->iid_known = IID_IDWriteFactory_b;
            dwp_set(g_dwrite_factory,  3, (void *)dwf_GetSystemFontCollection);
            dwp_set(g_dwrite_factory, 17, (void *)dwf_GetGdiInterop);
            dwp_set(g_dwrite_factory,  7, (void *)dwf_CreateFontFileReference);
            dwp_set(g_dwrite_factory, 10, (void *)dwf_CreateRenderingParams);
            dwp_set(g_dwrite_factory, 12, (void *)dwf_CreateCustomRenderingParams);
            dwp_set(g_dwrite_factory, 29, (void *)dwf_CreateCustomRenderingParams2);
            dwp_set(g_dwrite_factory, 15, (void *)dwf_CreateTextFormat);
            dwp_set(g_dwrite_factory, 18, (void *)dwf_CreateTextLayout);
            dwp_set(g_dwrite_factory,  8, (void *)dwf_CreateCustomFontFileReference);
            dwp_set(g_dwrite_factory, 13, (void *)dwf_RegisterFontFileLoader);
            dwp_set(g_dwrite_factory, 14, (void *)dwf_UnregisterFontFileLoader);
            dwp_set(g_dwrite_factory,  4, (void *)dwf_CreateCustomFontCollection);
            dwp_set(g_dwrite_factory,  5, (void *)dwf_RegisterFontCollectionLoader);
            dwp_set(g_dwrite_factory,  6, (void *)dwf_UnregisterFontCollectionLoader);
            dwp_set(g_dwrite_factory, 23, (void *)dwf_CreateGlyphRunAnalysis);
        }
    }
    if (!g_dwrite_factory) return DW_E_NOTIMPL;
    (void)iid;
    g_dwrite_factory->refs++;
    *out = g_dwrite_factory;
    return DW_S_OK;
}

#endif /* PELOAD_DWRITE_SHIM_H */
