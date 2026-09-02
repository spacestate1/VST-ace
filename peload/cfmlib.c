/* InterfaceLib, MathLib and DragLib, as far as a VST plug-in needs them.
 *
 * A Classic Mac OS plug-in imports from shared libraries that were part of the
 * system. There is no way to run the originals, so each imported symbol gets an
 * implementation here. The rule throughout is that a function either does the
 * real thing or reports that it did not: a stub that silently returns zero turns
 * a missing feature into wrong audio, which is far harder to find than a missing
 * feature that says so.
 *
 * Three notes on the calling convention, since everything here depends on it:
 *
 *   - Arguments arrive in r3..r10 and f1..f8, and the result goes back in r3, or
 *     f1 for a floating-point result. Structures passed by value would arrive in
 *     registers too, but nothing here takes one.
 *   - Everything the guest hands us is a guest address. It must go through the
 *     ppc_read and ppc_write accessors, never be dereferenced, both because the
 *     guest is big-endian and because a guest pointer means nothing to the host.
 *   - Mac OS results are 16-bit OSErr values, sign-extended into r3.
 *
 * The file calls are confined to one host directory. The guest can name a file
 * inside it and nothing else -- no absolute paths, no parent traversal -- which
 * matters because the code doing the naming is a twenty-year-old binary.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cfmlib.h"
#include "pict.h"
#include "macfont.h"

#define MAX_FILES     16
#define MAX_GWORLDS    8
#define MAX_DIRS       4
#define MAX_HANDLES   64
#define GW_MAX_PIXELS  (2048 * 2048)

/* Mac OS error codes, the few that come up. */
#define noErr          0
#define paramErr     (-50)
#define fnfErr       (-43)
#define memFullErr  (-108)
#define resNotFound (-192)
#define dirNFErr    (-120)

typedef struct {
    int      used;
    FILE    *fp;
    char     path[1024];
} cfm_file;

/* A Handle is a pointer to a master pointer, so the guest sees two levels of
 * indirection and we keep the bookkeeping on this side. */
typedef struct {
    int       used;
    uint32_t  handle;            /* guest address of the master pointer  */
    uint32_t  data;              /* guest address of the bytes           */
    uint32_t  size;
    uint32_t  type;              /* the resource type, if it is one      */
    int       id;
} cfm_handle;

typedef struct {
    int       used;
    uint32_t  guest;             /* the GWorldPtr the guest holds        */
    uint32_t  pixmap;            /* its PixMap, also in guest memory     */
    uint32_t  pixels;            /* the pixel data, in guest memory      */
    int       w, h, depth;
    uint32_t  rowbytes;
    int       clip_t, clip_l, clip_b, clip_r;   /* the clip region's bbox */
    int       ox, oy;            /* port origin, from SetOrigin          */
    unsigned long touched;       /* when it was last drawn into          */
} cfm_gworld;

struct cfm {
    ppc      *m;
    pef      *p;

    /* Every import resolved to one of these, or to NULL for "not implemented". */
    void    (**fn)(struct cfm *);
    uint32_t  nfn;
    char      unbound[1024];
    int       nunbound;
    int       stub_calls;
    char      last_stub[64];

    /* The guest heap: a bump allocator with a free list good enough for a
     * plug-in that allocates a handful of buffers and keeps them. */
    uint32_t  heap_next, heap_end;

    cfm_file  files[MAX_FILES];
    char      dirs[MAX_DIRS][1024];
    int       ndirs;

    /* audioMaster, if the embedder asked for it. */
    int       am_slot;                       /* -1 when there is none */
    double    am_rate;
    int       am_block;
    uint32_t  am_timeinfo;                   /* a VstTimeInfo in guest memory */
    double    am_samplepos;

    /* The guest's own resource fork, and the handles handed out from it. */
    const uint8_t *fork;
    uint32_t       forklen, fork_data, fork_map;
    int            nresources;
    cfm_handle     handles[MAX_HANDLES];
    int            pict_failures;
    char           pict_err[192];

    cfm_gworld gw[MAX_GWORLDS];
    uint32_t  cur_port;          /* the current GrafPort or GWorld       */
    uint32_t  pen_x, pen_y;
    int       pen_w, pen_h;      /* PenSize; QuickDraw starts at 1x1     */
    uint32_t  fore, back;        /* RGB, as 0x00RRGGBB                   */
    int       pen_mode, text_mode;
    int       tx_font, tx_face, tx_size;   /* TextFont/TextFace/TextSize */

    /* A scratch buffer of host-side pixels, for whoever wants to look at what
     * the guest drew, and the editor size that tells us which offscreen is the
     * one being looked at. */
    uint32_t *shadow;
    int       shadow_w, shadow_h;
    int       ed_w, ed_h;
    uint32_t  ed_window;         /* the port handed to the plug-in as a window */
    unsigned long draw_clock;    /* ticks once per drawing operation           */
    int       mouse_x, mouse_y, mouse_down;
    void    (*pump)(void *ud);   /* lets the embedder refresh input mid-loop */
    void     *pump_ud;
    double    last_pump;
    int       mem_err;           /* what MemError() will say */
    uint32_t  qd_globals;        /* QuickDraw's globals, once InitGraf runs */

    struct timespec t0;
};

/* ------------------------------------------------------------------ helpers */

static uint32_t arg(cfm *c, int n)      { return c->m->r[3 + n]; }
static double   farg(cfm *c, int n)     { return c->m->f[1 + n].d; }
static void     ret(cfm *c, uint32_t v) { c->m->r[3] = v; }
static void     retf(cfm *c, double v)  { c->m->f[1].d = v; }
static void     reterr(cfm *c, int e)   { c->m->r[3] = (uint32_t)(int32_t)e; }

static uint32_t g32(cfm *c, uint32_t a)             { return ppc_read32(c->m, a); }

/* A read that is allowed to come to nothing. The accessors above treat an
 * out-of-range address as the guest making a bad access and stop it, which is
 * right when the guest asked for that address -- but wrong when the host is
 * guessing. Anywhere a pointer is being probed rather than trusted, use this. */
static uint32_t peek32(cfm *c, uint32_t a)
{
    if ((uint64_t)a + 4 > (uint64_t)c->m->memsize) return 0;
    return ppc_read32(c->m, a);
}
static uint16_t g16(cfm *c, uint32_t a)             { return ppc_read16(c->m, a); }
static void     s32(cfm *c, uint32_t a, uint32_t v) { ppc_write32(c->m, a, v); }
static void     s16(cfm *c, uint32_t a, uint16_t v) { ppc_write16(c->m, a, v); }

/* A Mac Rect is four 16-bit values in the order top, left, bottom, right. */
typedef struct { int t, l, b, r; } rect;

static rect g_rect(cfm *c, uint32_t a)
{
    rect r;
    r.t = (int16_t)g16(c, a);      r.l = (int16_t)g16(c, a + 2);
    r.b = (int16_t)g16(c, a + 4);  r.r = (int16_t)g16(c, a + 6);
    return r;
}

/* A Pascal string: a length byte then that many characters. */
static void g_pstr(cfm *c, uint32_t a, char *out, size_t n)
{
    unsigned len = ppc_read8(c->m, a), i;
    if (len > n - 1) len = (unsigned)(n - 1);
    for (i = 0; i < len; i++) out[i] = (char)ppc_read8(c->m, a + 1 + i);
    out[len] = 0;
}

/* --------------------------------------------------------------- the heap */

/* Allocation carries an 8-byte header so DisposePtr can find the size, and the
 * result is 8-aligned because the guest will store doubles in it. */
static uint32_t heap_alloc(cfm *c, uint32_t size, int clear)
{
    uint32_t need = (size + 8 + 7u) & ~7u;
    uint32_t at;

    if (size > 0x02000000u || c->heap_next + need > c->heap_end) return 0;
    at = c->heap_next;
    c->heap_next += need;
    s32(c, at, size);
    s32(c, at + 4, 0x4D4D4F42u);            /* a marker, to catch bad frees */
    if (clear) {
        uint32_t k;
        for (k = 0; k < need - 8; k += 4) s32(c, at + 8 + k, 0);
    }
    return at + 8;
}

static void heap_free(cfm *c, uint32_t ptr)
{
    /* Memory is not reclaimed. A plug-in allocates its buffers once and holds
     * them for its lifetime, and a real free list would be a lot of machinery
     * for no benefit -- but a bad pointer is still worth catching. */
    if (!ptr) return;
    if (g32(c, ptr - 4) != 0x4D4D4F42u)
        fprintf(stderr, "peload: the guest disposed of 0x%08x, which it did not "
                        "allocate\n", ptr);
}

uint32_t cfm_guest_alloc(cfm *c, uint32_t size, int clear)
{
    return c ? heap_alloc(c, size, clear) : 0;
}

/* ------------------------------------------------------------- audioMaster */

/* A double in guest memory. VstTimeInfo is nothing but doubles and int32s, so
 * this and s32 are all it takes. */
static void s64f(cfm *c, uint32_t a, double d)
{
    union { double d; uint64_t u; } cv;
    cv.d = d;
    s32(c, a, (uint32_t)(cv.u >> 32));
    s32(c, a + 4, (uint32_t)cv.u);
}

void cfm_set_audiomaster(cfm *c, uint32_t slot, double rate, int block)
{
    if (!c) return;
    c->am_slot = (int)slot;
    c->am_rate = rate > 0 ? rate : 44100.0;
    c->am_block = block > 0 ? block : 512;
    if (!c->am_timeinfo) c->am_timeinfo = heap_alloc(c, 88, 1);
}

/* The host side of the VST 1.0 callback.
 *
 * Two answers matter more than the rest. audioMasterVersion must be a real
 * version or the plug-in concludes there is no host and refuses to build itself.
 * audioMasterGetTime must never return null, because plug-ins dereference it
 * without checking -- returning a filled-in structure that says "not playing" is
 * both true and safe.
 */
static void audio_master(cfm *c)
{
    ppc *m = c->m;
    uint32_t opcode = m->r[4];
    int32_t  index  = (int32_t)m->r[5];

    switch (opcode) {
    case 1:  m->r[3] = 2400; return;             /* audioMasterVersion       */
    case 0:  m->r[3] = 0;    return;             /* audioMasterAutomate      */
    case 2:  m->r[3] = 0;    return;             /* audioMasterCurrentId     */
    case 3:  m->r[3] = 0;    return;             /* audioMasterIdle          */
    case 6:  m->r[3] = 1;    return;             /* audioMasterWantMidi      */
    case 7:                                      /* audioMasterGetTime       */
        if (c->am_timeinfo) {
            s64f(c, c->am_timeinfo,      c->am_samplepos);
            s64f(c, c->am_timeinfo + 8,  c->am_rate);
            s64f(c, c->am_timeinfo + 16, 0.0);    /* nanoSeconds   */
            s64f(c, c->am_timeinfo + 24, 0.0);    /* ppqPos        */
            s64f(c, c->am_timeinfo + 32, 120.0);  /* tempo         */
            s64f(c, c->am_timeinfo + 40, 0.0);    /* barStartPos   */
            s64f(c, c->am_timeinfo + 48, 0.0);
            s64f(c, c->am_timeinfo + 56, 0.0);
            s32(c, c->am_timeinfo + 64, 4);       /* time signature */
            s32(c, c->am_timeinfo + 68, 4);
            s32(c, c->am_timeinfo + 72, 0);
            s32(c, c->am_timeinfo + 76, 0);
            s32(c, c->am_timeinfo + 80, 0);
            /* kVstTempoValid | kVstTimeSigValid, and not playing. */
            s32(c, c->am_timeinfo + 84, (1 << 10) | (1 << 13));
        }
        m->r[3] = c->am_timeinfo;
        return;
    case 13: m->r[3] = 0;    return;             /* audioMasterProcessEvents */
    case 16: m->r[3] = (uint32_t)(int32_t)c->am_rate;  return; /* GetSampleRate */
    case 17: m->r[3] = (uint32_t)c->am_block;    return;       /* GetBlockSize  */
    case 18: m->r[3] = 0;    return;             /* GetInputLatency          */
    case 19: m->r[3] = 0;    return;             /* GetOutputLatency         */
    case 23: m->r[3] = 2;    return;             /* GetCurrentProcessLevel: realtime */
    case 32: case 33: {                          /* GetVendorString/Product  */
        const char *s = (opcode == 32) ? "peload" : "peload Classic host";
        uint32_t p = m->r[7];
        if (p) { size_t k; for (k = 0; k <= strlen(s); k++)
                     ppc_write8(m, p + (uint32_t)k, (uint8_t)s[k]); }
        m->r[3] = 1;
        return; }
    case 34: m->r[3] = 1;    return;             /* GetVendorVersion         */
    case 37: m->r[3] = 0;    return;             /* CanDo: nothing special   */
    case 42: m->r[3] = 1;    return;             /* GetLanguage: English     */
    case 15: m->r[3] = 0;    return;             /* SizeWindow: refuse       */
    case 40: m->r[3] = 0;    return;             /* GetAutomationState: off  */
    default:
        /* Unknown opcodes get zero, which VST defines as "not handled". */
        (void)index;
        m->r[3] = 0;
        return;
    }
}

/* ----------------------------------------------------------------- handles */

static uint32_t handle_new(cfm *c, uint32_t size, const uint8_t *bytes)
{
    int i, slot = -1;
    uint32_t data, h;

    for (i = 0; i < MAX_HANDLES; i++) if (!c->handles[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    if (!(data = heap_alloc(c, size ? size : 1, 0))) return 0;
    if (!(h = heap_alloc(c, 4, 0))) return 0;
    s32(c, h, data);
    if (bytes) {
        uint32_t k;
        for (k = 0; k < size; k++) ppc_write8(c->m, data + k, bytes[k]);
    }
    c->handles[slot].used = 1;
    c->handles[slot].handle = h;
    c->handles[slot].data = data;
    c->handles[slot].size = size;
    return h;
}

static cfm_handle *handle_find(cfm *c, uint32_t h)
{
    int i;
    for (i = 0; i < MAX_HANDLES; i++)
        if (c->handles[i].used && c->handles[i].handle == h) return &c->handles[i];
    return NULL;
}

/* ------------------------------------------------------- the resource fork */

static uint32_t fk32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t fk16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Locate the resource fork proper. An AppleSingle or AppleDouble wrapper -- what
 * a Classic file becomes once it has left a Mac filesystem -- puts it in one of
 * its entries, and entry id 2 is the resource fork. Returns 0 on success and
 * narrows [*base, *end) to the fork. */
static int fork_span(const uint8_t *fork, uint32_t len, uint32_t *base,
                     uint32_t *end)
{
    *base = 0;
    *end = len;
    if (len < 16) return -1;
    if (fk32(fork) == 0x00051600u || fk32(fork) == 0x00051607u) {
        uint32_t n, k;
        if (len < 26) return -1;
        n = fk16(fork + 24);
        for (k = 0; k < n; k++) {
            const uint8_t *e = fork + 26 + (uint32_t)k * 12;
            if ((uint32_t)(e - fork) + 12 > len) return -1;
            if (fk32(e) == 2) {
                uint32_t off = fk32(e + 4), sz = fk32(e + 8);
                if ((uint64_t)off + 16 > len) return -1;
                *base = off;
                *end = ((uint64_t)off + sz <= len) ? off + sz : len;
                return 0;
            }
        }
        return -1;                               /* no resource fork in there */
    }
    return 0;                                    /* already a bare fork       */
}

/* Find a resource's bytes. The map is a type list, each entry pointing at a list
 * of references, each of those at an offset into the data area where the resource
 * is stored as a 32-bit length followed by that many bytes. */
static const uint8_t *fork_lookup(const uint8_t *fork, uint32_t fdata,
                                  uint32_t fmap, uint32_t end,
                                  uint32_t type, int id, uint32_t *size)
{
    const uint8_t *typelist;
    uint32_t tlo;
    int ntypes, i;

    if (fmap + 30 > end) return NULL;
    tlo = fk16(fork + fmap + 24);
    if (fmap + tlo + 2 > end) return NULL;
    typelist = fork + fmap + tlo;
    ntypes = (int)(int16_t)fk16(typelist) + 1;

    for (i = 0; i < ntypes; i++) {
        const uint8_t *te = typelist + 2 + (uint32_t)i * 8;
        uint32_t ty;
        int nrefs, k;
        const uint8_t *refs;

        if ((uint32_t)(te - fork) + 8 > end) return NULL;
        ty = fk32(te);
        nrefs = (int)(int16_t)fk16(te + 4) + 1;
        refs = typelist + fk16(te + 6);
        if (ty != type) continue;

        for (k = 0; k < nrefs; k++) {
            const uint8_t *re = refs + (uint32_t)k * 12;
            uint32_t off, at, len;
            if ((uint32_t)(re - fork) + 12 > end) return NULL;
            if ((int)(int16_t)fk16(re) != id) continue;
            /* The data offset is the low three bytes; the top byte is flags. */
            off = fk32(re + 4) & 0x00FFFFFFu;
            at = fdata + off;
            if (at + 4 > end) return NULL;
            len = fk32(fork + at);
            if ((uint64_t)at + 4 + len > end) return NULL;
            if (size) *size = len;
            return fork + at + 4;
        }
    }
    return NULL;
}

const uint8_t *cfm_fork_find(const uint8_t *fork, uint32_t len,
                             uint32_t type, int id, uint32_t *size)
{
    uint32_t base, end;

    if (!fork || fork_span(fork, len, &base, &end)) return NULL;
    return fork_lookup(fork, base + fk32(fork + base),
                       base + fk32(fork + base + 4), end, type, id, size);
}

static const uint8_t *res_find(cfm *c, uint32_t type, int id, uint32_t *size)
{
    if (!c->fork) return NULL;
    return fork_lookup(c->fork, c->fork_data, c->fork_map, c->forklen,
                       type, id, size);
}

int cfm_set_resource_fork(cfm *c, const uint8_t *fork, uint32_t len)
{
    uint32_t base, end, dataoff, mapoff, datalen, maplen, tlo;
    int ntypes, i, total = 0;

    if (!c || !fork) return -1;
    if (fork_span(fork, len, &base, &end)) return -1;

    dataoff = fk32(fork + base);
    mapoff  = fk32(fork + base + 4);
    datalen = fk32(fork + base + 8);
    maplen  = fk32(fork + base + 12);
    if ((uint64_t)base + mapoff + 30 > end ||
        (uint64_t)base + dataoff + datalen > end || maplen < 30)
        return -1;

    c->fork = fork;
    c->forklen = end;
    c->fork_data = base + dataoff;
    c->fork_map = base + mapoff;

    /* Count what is in there, which doubles as a check that the map parses. */
    tlo = fk16(fork + c->fork_map + 24);
    if (c->fork_map + tlo + 2 > end) { c->fork = NULL; return -1; }
    ntypes = (int)(int16_t)fk16(fork + c->fork_map + tlo) + 1;
    if (ntypes < 0 || ntypes > 4096) { c->fork = NULL; return -1; }
    for (i = 0; i < ntypes; i++) {
        const uint8_t *te = fork + c->fork_map + tlo + 2 + (uint32_t)i * 8;
        if ((uint32_t)(te - fork) + 8 > end) { c->fork = NULL; return -1; }
        total += (int)(int16_t)fk16(te + 4) + 1;
    }
    c->nresources = total;
    return total;
}

/* ------------------------------------------------------------------- memory */

static void f_NewPtr(cfm *c)      { ret(c, heap_alloc(c, arg(c, 0), 0)); }
static void f_NewPtrClear(cfm *c) { ret(c, heap_alloc(c, arg(c, 0), 1)); }
static void f_DisposePtr(cfm *c)  { heap_free(c, arg(c, 0)); ret(c, 0); }

/* Handles are indirect, and the guest locks them before using the pointer. There
 * is no compaction here, so a handle is permanently "locked" and these do
 * nothing but succeed. */
static void f_HLock(cfm *c)   { (void)c; }
static void f_HUnlock(cfm *c) { (void)c; }

/* StuffHex(dst, pascalHexString) writes the bytes the hex digits describe. It is
 * how Mac code builds patterns and small resources inline. */
static void f_StuffHex(cfm *c)
{
    uint32_t dst = arg(c, 0), src = arg(c, 1);
    unsigned len = ppc_read8(c->m, src), i, out = 0;
    int hi = -1;

    for (i = 0; i < len; i++) {
        int ch = ppc_read8(c->m, src + 1 + i), v;
        if      (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else continue;
        if (hi < 0) hi = v;
        else { ppc_write8(c->m, dst + out++, (uint8_t)((hi << 4) | v)); hi = -1; }
    }
}


/* ------------------------------------------------- the Memory Manager, handles */

/* A Handle is a pointer to a master pointer. Classic code allocates most things
 * this way, resizes them in place, and expects MemError() to explain a failure --
 * so these are not optional even for a plug-in, and a stub that returns an error
 * code where a Handle should be produces a "pointer" the caller then writes
 * through. */
static void f_NewHandle(cfm *c)
{
    uint32_t h = handle_new(c, arg(c, 0), NULL);
    c->mem_err = h ? noErr : memFullErr;
    ret(c, h);
}

static void f_NewHandleClear(cfm *c)
{
    uint32_t h = handle_new(c, arg(c, 0), NULL);
    if (h) {
        uint32_t data = g32(c, h), k, n = arg(c, 0);
        for (k = 0; k + 4 <= n; k += 4) s32(c, data + k, 0);
        for (; k < n; k++) ppc_write8(c->m, data + k, 0);
    }
    c->mem_err = h ? noErr : memFullErr;
    ret(c, h);
}

static void f_DisposeHandle(cfm *c)
{
    cfm_handle *hd = handle_find(c, arg(c, 0));
    if (hd) hd->used = 0;
    c->mem_err = noErr;
}

static void f_GetHandleSize(cfm *c)
{
    cfm_handle *hd = handle_find(c, arg(c, 0));
    c->mem_err = hd ? noErr : -109 /* nilHandleErr */;
    ret(c, hd ? hd->size : 0);
}

/* SetHandleSize grows or shrinks a handle. There is no compaction here, so
 * growing means a fresh block and a copy, and the master pointer is updated --
 * which is exactly why callers are required to re-dereference afterwards. */
static void f_SetHandleSize(cfm *c)
{
    cfm_handle *hd = handle_find(c, arg(c, 0));
    uint32_t want = arg(c, 1);

    if (!hd) { c->mem_err = -109; return; }
    if (want <= hd->size) { hd->size = want; c->mem_err = noErr; return; }
    {
        uint32_t nd = heap_alloc(c, want, 1), k;
        if (!nd) { c->mem_err = memFullErr; return; }
        for (k = 0; k < hd->size; k++)
            ppc_write8(c->m, nd + k, ppc_read8(c->m, hd->data + k));
        hd->data = nd;
        hd->size = want;
        s32(c, hd->handle, nd);
        c->mem_err = noErr;
    }
}

static void f_MemError(cfm *c) { ret(c, (uint32_t)(int32_t)(int16_t)c->mem_err); }

/* Zone housekeeping. There is one flat heap here and nothing moves, so these have
 * nothing to do -- but they must succeed, because an application calls them
 * before it does anything else and gives up if they fail. */
static void f_MaxApplZone(cfm *c)  { c->mem_err = noErr; }
static void f_MoreMasters(cfm *c)  { c->mem_err = noErr; }
static void f_SetGrowZone(cfm *c)  { c->mem_err = noErr; }
static void f_FreeMem(cfm *c)      { ret(c, c->heap_end - c->heap_next); }
static void f_MaxMem(cfm *c)
{ if (arg(c, 0)) s32(c, arg(c, 0), c->heap_end - c->heap_next);
  ret(c, c->heap_end - c->heap_next); }
static void f_CompactMem(cfm *c)   { ret(c, c->heap_end - c->heap_next); }

/* ------------------------------------------------------- Toolbox initialisation */

/* InitGraf is handed the address of QuickDraw's globals -- specifically of
 * `thePort`, which sits 6 bytes into the block, with the rest above it. Code
 * reaches the other globals by offset from there, so the block has to exist and
 * be plausible even though nothing here draws to a screen. */
static void f_InitGraf(cfm *c)
{
    uint32_t g = arg(c, 0);

    if (!g) return;
    c->qd_globals = g;
    /* Laid out backwards from thePort, as QuickDraw does:
     *   -6 randSeed, +0 thePort, +4 white..black patterns, +36 arrow cursor,
     *   +104 screenBits (a BitMap), +122 thePort again in some headers. */
    s32(c, g - 6, 1);                            /* randSeed              */
    s32(c, g, 0);                                /* thePort: none yet     */
    { int k; for (k = 0; k < 8; k++) ppc_write8(c->m, g + 4 + (uint32_t)k, 0xFF); }
    { int k; for (k = 0; k < 8; k++) ppc_write8(c->m, g + 12 + (uint32_t)k, 0x00); }
    /* screenBits: a 1024x768 black-and-white screen is a plausible answer to a
     * question about a screen that does not exist. */
    s32(c, g + 104, 0);                          /* baseAddr              */
    s16(c, g + 108, 128);                        /* rowBytes              */
    s16(c, g + 110, 0); s16(c, g + 112, 0);
    s16(c, g + 114, 768); s16(c, g + 116, 1024);
}

static void f_InitFonts(cfm *c)   { (void)c; }
static void f_InitWindows(cfm *c) { (void)c; }
static void f_InitMenus(cfm *c)   { (void)c; }
static void f_TEInit(cfm *c)      { (void)c; }
static void f_InitDialogs(cfm *c) { (void)c; }
static void f_FlushEvents(cfm *c) { (void)c; }

/* Gestalt answers questions about the machine. Only a handful are ever asked
 * before an application decides whether it can run at all. */
static void f_Gestalt(cfm *c)
{
    uint32_t sel = arg(c, 0), out = arg(c, 1);
    uint32_t v;

    switch (sel) {
    case 0x73797376: v = 0x0904; break;          /* 'sysv': Mac OS 9.0.4   */
    case 0x71642020: v = 0x0200; break;          /* 'qd  ': Color QuickDraw*/
    case 0x71647267: v = 0x0200; break;          /* 'qdrg'                 */
    case 0x61707061: v = 2;     break;           /* 'appa': Appearance     */
    case 0x61777972: v = 0x0110; break;          /* 'awyr': Appearance ver */
    case 0x74656174: v = 0x0300; break;          /* 'teat': TextEdit       */
    case 0x666f6e74: v = 1;     break;           /* 'font'                 */
    case 0x70726f63: v = 0x0101; break;          /* 'proc': a PowerPC 604  */
    case 0x63707574: v = 0x0101; break;          /* 'cput'                 */
    case 0x6d616368: v = 406;   break;           /* 'mach'                 */
    default:
        /* Saying "I do not know that one" is a real answer and callers handle
         * it; inventing a value for an unknown selector is how a plug-in ends up
         * taking a path meant for hardware that is not here. */
        if (out) s32(c, out, 0);
        reterr(c, -5551 /* gestaltUndefSelectorErr */);
        return;
    }
    if (out) s32(c, out, v);
    reterr(c, noErr);
}

/* --------------------------------------------------------------------- math */

static void f_sin(cfm *c)   { retf(c, sin(farg(c, 0))); }
static void f_cos(cfm *c)   { retf(c, cos(farg(c, 0))); }
static void f_sinh(cfm *c)  { retf(c, sinh(farg(c, 0))); }
static void f_exp(cfm *c)   { retf(c, exp(farg(c, 0))); }
static void f_log(cfm *c)   { retf(c, log(farg(c, 0))); }
static void f_log10(cfm *c) { retf(c, log10(farg(c, 0))); }
static void f_floor(cfm *c) { retf(c, floor(farg(c, 0))); }
static void f_fabs(cfm *c)  { retf(c, fabs(farg(c, 0))); }
static void f_pow(cfm *c)   { retf(c, pow(farg(c, 0), farg(c, 1))); }
static void f_fmod(cfm *c)  { retf(c, fmod(farg(c, 0), farg(c, 1))); }
static void f_atan2(cfm *c) { retf(c, atan2(farg(c, 0), farg(c, 1))); }
static void f_sqrt(cfm *c)  { retf(c, sqrt(farg(c, 0))); }

/* num2dec(format, x, &decimal) breaks a double into the sign, exponent and
 * decimal digits that Mac OS printf is written against.
 *
 * The `decimal` struct is { SInt8 sgn; SInt8 unused; short exp; char sig[21]; }
 * where sig is a Pascal string of digits. `format` is a `decform`:
 * { SInt8 style; SInt8 unused; short digits } -- style 0 asks for a fixed number
 * of decimal places, style 1 for a number of significant digits. */
static void f_num2dec(cfm *c)
{
    uint32_t fmt = arg(c, 0), out = c->m->r[4];
    double   x   = farg(c, 0);
    int style = (int8_t)ppc_read8(c->m, fmt);
    int digits = (int16_t)g16(c, fmt + 2);
    char buf[64], *dot;
    int sgn = 0, exp10 = 0, n;

    if (x < 0 || (x == 0 && signbit(x))) { sgn = 1; x = -x; }
    if (digits < 0)  digits = 0;
    if (digits > 19) digits = 19;

    if (style == 0) {                       /* FIXEDDECIMAL */
        snprintf(buf, sizeof buf, "%.*f", digits, x);
        if ((dot = strchr(buf, '.')) != NULL) {
            memmove(dot, dot + 1, strlen(dot));
            exp10 = -digits;
        }
    } else {                                /* FLOATDECIMAL */
        snprintf(buf, sizeof buf, "%.*e", digits > 0 ? digits - 1 : 0, x);
        {
            char *e = strchr(buf, 'e');
            int ex = e ? atoi(e + 1) : 0;
            if (e) *e = 0;
            if ((dot = strchr(buf, '.')) != NULL) memmove(dot, dot + 1, strlen(dot));
            n = (int)strlen(buf);
            exp10 = ex - (n - 1);
        }
    }
    /* Trim the leading zeros a value below 1 produces, keeping at least one. */
    {
        char *s = buf;
        while (s[0] == '0' && s[1]) s++;
        if (s != buf) memmove(buf, s, strlen(s) + 1);
    }
    n = (int)strlen(buf);
    if (n > 20) n = 20;

    ppc_write8(c->m, out, (uint8_t)sgn);
    ppc_write8(c->m, out + 1, 0);
    s16(c, out + 2, (uint16_t)(int16_t)exp10);
    ppc_write8(c->m, out + 4, (uint8_t)n);
    { int i; for (i = 0; i < n; i++) ppc_write8(c->m, out + 5 + i, (uint8_t)buf[i]); }
}

/* --------------------------------------------------------------------- time */

static uint32_t ticks_now(cfm *c)
{
    struct timespec t;
    double dt;
    clock_gettime(CLOCK_MONOTONIC, &t);
    dt = (double)(t.tv_sec - c->t0.tv_sec) +
         (double)(t.tv_nsec - c->t0.tv_nsec) / 1e9;
    return (uint32_t)(dt * 60.0);          /* a tick is a sixtieth of a second */
}

static void f_TickCount(cfm *c)  { ret(c, ticks_now(c)); }
static void f_GetDblTime(cfm *c) { ret(c, 30); }

/* -------------------------------------------------------------------- files */

/* Map a guest FSSpec to a host path. The name is taken as a leaf inside one of
 * the directories we published; anything that tries to escape is refused. */
static int spec_path(cfm *c, uint32_t spec, char *out, size_t n)
{
    char name[64];
    int32_t parid = (int32_t)g32(c, spec + 2);
    int dir = (parid >= 2 && parid - 2 < c->ndirs) ? (int)(parid - 2) : 0;

    g_pstr(c, spec + 6, name, sizeof name);
    if (!name[0] || strchr(name, '/') || strstr(name, "..")) return 0;
    if (c->ndirs == 0) return 0;
    snprintf(out, n, "%s/%s", c->dirs[dir], name);
    return 1;
}

/* FSFindFolder(vRefNum, folderType, createFolder, FSRef *result). Only one
 * folder exists here, and every request resolves to it. */
static void f_FSFindFolder(cfm *c)
{
    uint32_t out = arg(c, 3);
    if (getenv("CFMFILE")) {
        uint32_t t = arg(c, 1);
        fprintf(stderr, "  [file] FSFindFolder type='%c%c%c%c'\n",
                (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t);
    }
    if (!out || c->ndirs == 0) { reterr(c, fnfErr); return; }
    /* An FSRef is opaque, so its contents are ours to choose. Tag it so
     * FSGetCatalogInfo can recognise one we made. */
    s32(c, out, 0x50454652u);                       /* 'PEFR' */
    s32(c, out + 4, 2);                             /* our directory ID */
    reterr(c, noErr);
}

/* FSGetCatalogInfo(ref, whichInfo, catalogInfo, outName, fsSpec, parentRef).
 * The plug-in uses it for one thing: turning the FSRef from FSFindFolder into an
 * FSSpec it can pass to FSMakeFSSpec. */
static void f_FSGetCatalogInfo(cfm *c)
{
    uint32_t ref = arg(c, 0), spec = arg(c, 4);
    uint32_t dirid;

    if (!ref || g32(c, ref) != 0x50454652u) { reterr(c, paramErr); return; }
    dirid = g32(c, ref + 4);
    if (spec) {
        s16(c, spec, 0xFFFF);                       /* a volume of our own    */
        s32(c, spec + 2, dirid);
        ppc_write8(c->m, spec + 6, 0);               /* an empty leaf name     */
    }
    reterr(c, noErr);
}

/* FSMakeFSSpec(vRefNum, dirID, fileName, FSSpec *spec) */
static void f_FSMakeFSSpec(cfm *c)
{
    uint32_t name = arg(c, 2), spec = arg(c, 3);
    int32_t dirid = (int32_t)arg(c, 1);
    char leaf[64], path[1024];
    unsigned len, i;

    if (!spec) { reterr(c, paramErr); return; }
    if (dirid < 2) dirid = 2;                        /* our only directory    */
    s16(c, spec, 0xFFFF);
    s32(c, spec + 2, (uint32_t)dirid);
    len = name ? ppc_read8(c->m, name) : 0;
    if (len > 63) len = 63;
    ppc_write8(c->m, spec + 6, (uint8_t)len);
    for (i = 0; i < len; i++)
        ppc_write8(c->m, spec + 7 + i, ppc_read8(c->m, name + 1 + i));

    /* Report whether the file exists, which is what the caller checks. */
    g_pstr(c, spec + 6, leaf, sizeof leaf);
    if (!spec_path(c, spec, path, sizeof path)) { reterr(c, paramErr); return; }
    if (getenv("CFMFILE"))
        fprintf(stderr, "  [file] FSMakeFSSpec \"%s\" -> %s (%s)\n",
                leaf, path, access(path, F_OK) == 0 ? "exists" : "MISSING");
    reterr(c, access(path, F_OK) == 0 ? noErr : fnfErr);
}

static int file_slot(cfm *c)
{
    int i;
    for (i = 0; i < MAX_FILES; i++) if (!c->files[i].used) return i;
    return -1;
}

/* FSpOpenDF(FSSpec *spec, SInt8 permission, short *refNum) */
static void f_FSpOpenDF(cfm *c)
{
    uint32_t spec = arg(c, 0), outref = arg(c, 2);
    int perm = (int)(arg(c, 1) & 0xFF);
    char path[1024];
    int slot;
    FILE *fp;

    if (!spec_path(c, spec, path, sizeof path)) { reterr(c, paramErr); return; }
    if ((slot = file_slot(c)) < 0)              { reterr(c, memFullErr); return; }
    /* Permission 1 is read-only, 2 write, 3 read/write. Opening for writing must
     * not truncate: Mac OS semantics are "open the existing fork". */
    fp = fopen(path, perm == 1 ? "rb" : "r+b");
    if (!fp && perm != 1) fp = fopen(path, "w+b");
    if (!fp) { reterr(c, fnfErr); return; }

    c->files[slot].used = 1;
    c->files[slot].fp = fp;
    snprintf(c->files[slot].path, sizeof c->files[slot].path, "%s", path);
    if (outref) s16(c, outref, (uint16_t)(slot + 1));
    reterr(c, noErr);
}

/* FSpCreate(FSSpec *spec, OSType creator, OSType type, ScriptCode script) */
static void f_FSpCreate(cfm *c)
{
    char path[1024];
    FILE *fp;
    if (!spec_path(c, arg(c, 0), path, sizeof path)) { reterr(c, paramErr); return; }
    if (access(path, F_OK) == 0) { reterr(c, -48 /* dupFNErr */); return; }
    if (!(fp = fopen(path, "wb"))) { reterr(c, fnfErr); return; }
    fclose(fp);
    reterr(c, noErr);
}

/* FSRead(short refNum, long *count, void *buffer) */
static void f_FSRead(cfm *c)
{
    int refnum = (int)(int16_t)(arg(c, 0) & 0xFFFF);
    uint32_t pcount = arg(c, 1), buf = arg(c, 2);
    uint32_t want, got;
    unsigned char tmp[4096];

    if (refnum < 1 || refnum > MAX_FILES || !c->files[refnum - 1].used)
        { reterr(c, paramErr); return; }
    want = pcount ? g32(c, pcount) : 0;
    got = 0;
    while (got < want) {
        size_t chunk = want - got, n;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        n = fread(tmp, 1, chunk, c->files[refnum - 1].fp);
        { size_t k; for (k = 0; k < n; k++) ppc_write8(c->m, buf + got + (uint32_t)k, tmp[k]); }
        got += (uint32_t)n;
        if (n < chunk) break;
    }
    if (pcount) s32(c, pcount, got);
    if (getenv("CFMFILE"))
        fprintf(stderr, "  [file] FSRead wanted %u byte(s), got %u\n", want, got);
    /* Short reads report end-of-file, which is how the caller finds the length. */
    reterr(c, got < want ? -39 /* eofErr */ : noErr);
}

static void f_FSClose(cfm *c)
{
    int refnum = (int)(int16_t)(arg(c, 0) & 0xFFFF);
    if (refnum < 1 || refnum > MAX_FILES || !c->files[refnum - 1].used)
        { reterr(c, paramErr); return; }
    fclose(c->files[refnum - 1].fp);
    c->files[refnum - 1].used = 0;
    reterr(c, noErr);
}

/* PBGetCatInfoSync(CInfoPBRec *pb) -- the old parameter-block interface. The
 * plug-in uses it to ask whether something exists, so answer that much. */
static void f_PBGetCatInfoSync(cfm *c)
{
    reterr(c, fnfErr);
}

/* ---------------------------------------------------------------- graphics */

/* A GWorld here is a real block of guest memory with a PixMap in front of it, so
 * the plug-in's drawing lands somewhere it can be read back from. The layout
 * follows the real one closely enough for the accessors the guest uses:
 *
 *   GWorld  +0  a PixMapHandle (pointer to pointer to PixMap)
 *   PixMap  +0  baseAddr, +4 rowBytes|0x8000, +6 bounds, +30 pixelSize
 */
static void gw_build(cfm *c, cfm_gworld *g)
{
    uint32_t pm, hh;

    g->rowbytes = (uint32_t)g->w * 4;
    g->clip_t = 0; g->clip_l = 0; g->clip_b = g->h; g->clip_r = g->w;
    g->ox = 0; g->oy = 0;
    g->pixels = heap_alloc(c, g->rowbytes * (uint32_t)g->h, 1);
    pm = heap_alloc(c, 52, 1);
    hh = heap_alloc(c, 4, 1);                 /* the PixMap handle           */
    s32(c, hh, pm);
    s32(c, pm, g->pixels);
    s16(c, pm + 4, (uint16_t)(0x8000u | g->rowbytes));
    s16(c, pm + 6, 0); s16(c, pm + 8, 0);
    s16(c, pm + 10, (uint16_t)g->h); s16(c, pm + 12, (uint16_t)g->w);
    s16(c, pm + 30, 32);                      /* pixelSize                   */
    s16(c, pm + 32, 1);                       /* cmpCount, direct            */
    s16(c, pm + 34, 8);                       /* cmpSize                     */

    g->pixmap = hh;

    /* A GWorldPtr is a CGrafPtr, and a WindowPtr is a pointer to a record that
     * begins with one, so building the real thing here means a GWorld can be
     * handed to a plug-in as the window to draw into. The layout matters: the
     * PixMap handle is at offset 2, not 0, because a two-byte `device` field
     * comes first, and portVersion's top bits are what mark the port as colour
     * rather than the black-and-white kind whose bitmap sits at the same place. */
    g->guest = heap_alloc(c, 108, 1);
    s16(c, g->guest, 0);                      /* device                      */
    s32(c, g->guest + 2, hh);                 /* portPixMap                  */
    s16(c, g->guest + 6, (uint16_t)0xC000);   /* portVersion: a colour port  */
    s32(c, g->guest + 8, 0);                  /* grafVars                    */
    s16(c, g->guest + 16, 0); s16(c, g->guest + 18, 0);        /* portRect  */
    s16(c, g->guest + 20, (uint16_t)g->h); s16(c, g->guest + 22, (uint16_t)g->w);
    s16(c, g->guest + 52, 1); s16(c, g->guest + 54, 1);        /* pnSize    */
    s16(c, g->guest + 66, 1);                                  /* pnVis     */
    s16(c, g->guest + 74, 12);                                 /* txSize    */
}

/* NewGWorld(GWorldPtr *out, short depth, const Rect *bounds, CTabHandle,
 *           GDHandle, GWorldFlags) */
static void f_NewGWorld(cfm *c)
{
    uint32_t out = arg(c, 0);
    rect b;
    int i, slot = -1;

    if (!out || !arg(c, 2)) { reterr(c, paramErr); return; }
    b = g_rect(c, arg(c, 2));
    for (i = 0; i < MAX_GWORLDS; i++) if (!c->gw[i].used) { slot = i; break; }
    if (slot < 0) { reterr(c, memFullErr); return; }

    c->gw[slot].w = b.r - b.l;
    c->gw[slot].h = b.b - b.t;
    c->gw[slot].depth = (int)(int16_t)(arg(c, 1) & 0xFFFF);
    if (c->gw[slot].w <= 0 || c->gw[slot].h <= 0 ||
        (long)c->gw[slot].w * c->gw[slot].h > GW_MAX_PIXELS)
        { reterr(c, paramErr); return; }

    c->gw[slot].used = 1;
    gw_build(c, &c->gw[slot]);
    if (!c->gw[slot].pixels) { c->gw[slot].used = 0; reterr(c, memFullErr); return; }
    s32(c, out, c->gw[slot].guest);
    reterr(c, noErr);
}

static cfm_gworld *gw_find(cfm *c, uint32_t guest)
{
    int i;
    for (i = 0; i < MAX_GWORLDS; i++)
        if (c->gw[i].used && c->gw[i].guest == guest) return &c->gw[i];
    return NULL;
}

/* Note that an offscreen has been drawn into. Which one a plug-in is currently
 * composing into is not something it announces, and asking after the fact is the
 * only way to know: some plug-ins draw straight into the window they were given,
 * others build the image in an offscreen of their own and blit it across only
 * when they decide the window needs it. */
static void touch_base(cfm *c, uint32_t pixelbase)
{
    int i;
    for (i = 0; i < MAX_GWORLDS; i++)
        if (c->gw[i].used && c->gw[i].pixels == pixelbase)
            { c->gw[i].touched = ++c->draw_clock; return; }
}

static void touch_port(cfm *c)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    if (g) g->touched = ++c->draw_clock;
}

static void f_DisposeGWorld(cfm *c)
{
    cfm_gworld *g = gw_find(c, arg(c, 0));
    if (g) g->used = 0;
}

static void f_GetGWorld(cfm *c)
{
    if (arg(c, 0)) s32(c, arg(c, 0), c->cur_port);
    if (arg(c, 1)) s32(c, arg(c, 1), 0);        /* no GDevice is modelled */
}

static void f_SetGWorld(cfm *c) { c->cur_port = arg(c, 0); }
static void f_GetPort(cfm *c)   { if (arg(c, 0)) s32(c, arg(c, 0), c->cur_port); }
static void f_SetPort(cfm *c)   { c->cur_port = arg(c, 0); }

static void f_GetGWorldPixMap(cfm *c)
{
    cfm_gworld *g = gw_find(c, arg(c, 0));
    ret(c, g ? g->pixmap : 0);
}

static void f_LockPixels(cfm *c)   { ret(c, 1); }     /* nothing moves here */
static void f_UnlockPixels(cfm *c) { (void)c; }

/* The pixels a PixMap or a GWorld points at. The guest passes CopyBits a
 * `BitMap *`, which for a colour port is really the PixMap. */
static int bits_of(cfm *c, uint32_t bits, uint32_t *base, uint32_t *rb,
                   int *w, int *h)
{
    /* What the guest hands CopyBits can be any of several things, and the
     * difference is only discoverable by trying:
     *
     *   a GWorldPtr           -- which is a CGrafPtr; its PixMap handle is at +2
     *   &port->portBits       -- which on a colour port *is* port+2, so the
     *                            handle is at the address itself, not past it
     *   a PixMapHandle        -- one level of indirection
     *   a PixMap              -- none
     *
     * Reading portBits as if it were the handle, or the handle as if it were the
     * port, both yield a plausible-looking address and then draw nothing, which
     * is why each form is checked against the offscreens we actually made rather
     * than guessed at from the value. */
    {
        cfm_gworld *g = gw_find(c, bits);
        int i, done = 0;

        if (g) { bits = peek32(c, g->pixmap); done = 1; }
        for (i = 0; !done && i < MAX_GWORLDS; i++) {
            if (!c->gw[i].used) continue;
            if (c->gw[i].pixmap == bits)              /* a PixMapHandle       */
                { bits = peek32(c, bits); done = 1; }
            else if (c->gw[i].pixmap == peek32(c, bits)) /* &portBits         */
                { bits = peek32(c, peek32(c, bits)); done = 1; }
            else if (c->gw[i].guest + 2 == bits)      /* our own port's field */
                { bits = peek32(c, c->gw[i].pixmap); done = 1; }
        }
        /* Anything else is taken at face value as a PixMap. */
    }
    /* The PixMap's own fields are still only believed, not known, so they are
     * read the same forgiving way -- and the caller checks the result. */
    if (!bits || (uint64_t)bits + 16 > (uint64_t)c->m->memsize) return 0;
    *base = g32(c, bits);
    *rb   = g16(c, bits + 4) & 0x3FFF;
    *h    = (int16_t)g16(c, bits + 10) - (int16_t)g16(c, bits + 6);
    *w    = (int16_t)g16(c, bits + 12) - (int16_t)g16(c, bits + 8);
    /* And the pixels themselves must be inside guest memory before anything
     * copies through them. */
    if ((uint64_t)*base + (uint64_t)*rb * (uint64_t)(*h > 0 ? *h : 0)
        > (uint64_t)c->m->memsize) return 0;
    return *base && *rb && *w > 0 && *h > 0;
}

/* CopyBits(srcBits, dstBits, srcRect, dstRect, mode, maskRgn). Nearest-neighbour
 * scaling, which is what QuickDraw did for a non-integer ratio. */
/* CFMBLIT=1 traces every CopyBits. Read once: this sits in the inner loop of
 * everything a Classic editor draws, and a getenv per blit is a syscall per
 * blit. */
static int blit_trace(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("CFMBLIT"); v = e && *e != '0'; }
    return v;
}

static void f_CopyBits(cfm *c)
{
    uint32_t sb, db, srb, drb;
    int sw, sh, dw, dh, y, x;
    rect sr, dr;

    if (!bits_of(c, arg(c, 0), &sb, &srb, &sw, &sh)) return;
    if (!bits_of(c, arg(c, 1), &db, &drb, &dw, &dh)) return;
    if (!arg(c, 2) || !arg(c, 3)) return;
    sr = g_rect(c, arg(c, 2));
    dr = g_rect(c, arg(c, 3));

    touch_base(c, db);
    if (blit_trace()) {
        /* Where the bits went, and whether the source had anything in it. The
         * question an empty panel raises is which of those two it is, and the
         * answer is not reachable from outside: this is the only place that
         * sees both ends of the copy. */
        long nz = 0; int yy, xx;
        for (yy = sr.t; yy < sr.b && yy < sh; yy++)
            for (xx = sr.l; xx < sr.r && xx < sw; xx++)
                if (yy >= 0 && xx >= 0 &&
                    (g32(c, sb + (uint32_t)yy * srb + (uint32_t)xx * 4) & 0xFFFFFF)) nz++;
        fprintf(stderr, "  [blit] src %dx%d (%d,%d %d,%d) -> dst %dx%d (%d,%d %d,%d)"
                        " mode %d  src non-black %ld\n",
                sw, sh, sr.l, sr.t, sr.r, sr.b, dw, dh, dr.l, dr.t, dr.r, dr.b,
                (int)arg(c, 4), nz);
    }
    for (y = dr.t; y < dr.b; y++) {
        int syy;
        if (y < 0 || y >= dh) continue;
        syy = sr.t + (dr.b > dr.t ? (y - dr.t) * (sr.b - sr.t) / (dr.b - dr.t) : 0);
        if (syy < 0 || syy >= sh) continue;
        for (x = dr.l; x < dr.r; x++) {
            int sxx;
            if (x < 0 || x >= dw) continue;
            sxx = sr.l + (dr.r > dr.l ? (x - dr.l) * (sr.r - sr.l) / (dr.r - dr.l) : 0);
            if (sxx < 0 || sxx >= sw) continue;
            s32(c, db + (uint32_t)y * drb + (uint32_t)x * 4,
                g32(c, sb + (uint32_t)syy * srb + (uint32_t)sxx * 4));
        }
    }
}

/* CopyMask(srcBits, maskBits, dstBits, srcRect, maskRect, dstRect) */
static void f_CopyMask(cfm *c)
{
    uint32_t sb, mb, db, srb, mrb, drb;
    int sw, sh, mw, mh, dw, dh, y, x;
    rect sr, dr;

    if (!bits_of(c, arg(c, 0), &sb, &srb, &sw, &sh)) return;
    if (!bits_of(c, arg(c, 1), &mb, &mrb, &mw, &mh)) return;
    if (!bits_of(c, arg(c, 2), &db, &drb, &dw, &dh)) return;
    if (!arg(c, 3) || !arg(c, 5)) return;
    sr = g_rect(c, arg(c, 3));
    dr = g_rect(c, arg(c, 5));

    touch_base(c, db);
    for (y = dr.t; y < dr.b; y++) {
        int syy = sr.t + (y - dr.t);
        if (y < 0 || y >= dh || syy < 0 || syy >= sh) continue;
        for (x = dr.l; x < dr.r; x++) {
            int sxx = sr.l + (x - dr.l);
            uint32_t mask;
            if (x < 0 || x >= dw || sxx < 0 || sxx >= sw) continue;
            if (syy >= mh || sxx >= mw) continue;
            mask = g32(c, mb + (uint32_t)syy * mrb + (uint32_t)sxx * 4);
            /* A white mask pixel means "leave the destination alone". */
            if ((mask & 0xFFFFFFu) == 0xFFFFFFu) continue;
            s32(c, db + (uint32_t)y * drb + (uint32_t)x * 4,
                g32(c, sb + (uint32_t)syy * srb + (uint32_t)sxx * 4));
        }
    }
}

/* Plot one pixel in the current port. Local coordinates become bitmap
 * coordinates through the port's origin, and the clip region bounds what
 * lands. The mode is QuickDraw's: copy writes, or unions, xor inverts the
 * shared bits, bic clears. The pat* family is treated as its src* equivalent
 * with the fore colour -- the pattern itself is only honoured by the fill
 * calls, which is where the corpus actually uses one. */
static void port_plot(cfm *c, int x, int y, uint32_t colour, int mode)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    uint32_t at, old;

    if (!g) return;
    x -= g->ox; y -= g->oy;
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) return;
    if (x < g->clip_l || x >= g->clip_r || y < g->clip_t || y >= g->clip_b)
        return;
    at = g->pixels + (uint32_t)y * g->rowbytes + (uint32_t)x * 4;
    switch (mode & 7) {
    case 1:  s32(c, at, g32(c, at) | colour); return;          /* srcOr    */
    case 2:  s32(c, at, g32(c, at) ^ colour); return;          /* srcXor   */
    case 3:  s32(c, at, g32(c, at) & ~colour); return;         /* srcBic   */
    case 4:  s32(c, at, ~colour & 0xFFFFFFu); return;          /* notSrcCopy */
    case 5:  s32(c, at, g32(c, at) | (~colour & 0xFFFFFFu)); return;
    case 6:  s32(c, at, g32(c, at) ^ (~colour & 0xFFFFFFu)); return;
    case 7:  old = g32(c, at); s32(c, at, old & colour); return; /* notSrcBic */
    default: s32(c, at, colour); return;                       /* srcCopy  */
    }
}

static void port_put(cfm *c, int x, int y, uint32_t colour)
{
    port_plot(c, x, y, colour, 0);
}

/* An 8x8 Pattern is eight bytes, bit 7 leftmost. Set bits take the fore
 * colour, clear bits the back colour -- a dither between two colours is how a
 * Classic editor gets its shaded backgrounds, which is why ignoring the
 * pattern (as this used to) flattened them to one solid fill. */
static uint32_t pat_pixel(cfm *c, uint32_t pat, int x, int y)
{
    unsigned row = pat ? ppc_read8(c->m, pat + (uint32_t)(y & 7)) : 0xFF;
    return (row & (0x80u >> (x & 7))) ? c->fore : c->back;
}

static void f_FillRect(cfm *c)
{
    touch_port(c);
    rect r;
    int y, x;
    uint32_t pat;
    if (!arg(c, 0)) return;
    r = g_rect(c, arg(c, 0));
    pat = arg(c, 1);
    for (y = r.t; y < r.b; y++)
        for (x = r.l; x < r.r; x++)
            port_put(c, x, y, pat_pixel(c, pat, x, y));
}

/* PaintRect fills with the pen pattern in the pen mode; the pen pattern here
 * is solid fore colour, so this is a mode-honouring fore fill. */
static void f_PaintRect(cfm *c)
{
    touch_port(c);
    rect r;
    int y, x;
    if (!arg(c, 0)) return;
    r = g_rect(c, arg(c, 0));
    for (y = r.t; y < r.b; y++)
        for (x = r.l; x < r.r; x++)
            port_plot(c, x, y, c->fore, c->pen_mode);
}

/* EraseRect fills with the background -- the port's bkPat, solid back colour
 * here. */
static void f_EraseRect(cfm *c)
{
    touch_port(c);
    rect r;
    int y, x;
    if (!arg(c, 0)) return;
    r = g_rect(c, arg(c, 0));
    for (y = r.t; y < r.b; y++)
        for (x = r.l; x < r.r; x++)
            port_put(c, x, y, c->back);
}

/* FrameRect strokes the rectangle's outline with the pen. */
static void f_FrameRect(cfm *c)
{
    touch_port(c);
    rect r;
    int y, x;
    if (!arg(c, 0)) return;
    r = g_rect(c, arg(c, 0));
    for (x = r.l; x < r.r; x++)
        for (y = 0; y < c->pen_h; y++) {
            port_put(c, x, r.t + y, c->fore);
            port_put(c, x, r.b - 1 - y, c->fore);
        }
    for (y = r.t; y < r.b; y++)
        for (x = 0; x < c->pen_w; x++) {
            port_put(c, r.l + x, y, c->fore);
            port_put(c, r.r - 1 - x, y, c->fore);
        }
}

static void f_InvertRect(cfm *c)
{
    touch_port(c);
    rect r;
    int y, x;
    cfm_gworld *g;
    if (!arg(c, 0)) return;
    r = g_rect(c, arg(c, 0));
    if (!(g = gw_find(c, c->cur_port))) return;
    for (y = r.t; y < r.b; y++)
        for (x = r.l; x < r.r; x++)
            port_plot(c, x, y, 0xFFFFFF, 2 /* srcXor */);
}

/* ScrollRect(rect, dh, dv, updateRgn) shifts the pixels inside the rect.
 * The vacated area is reported through the update region, which we do not
 * track -- editors here redraw from effEditIdle, so filling it with the back
 * colour is the honest equivalent. */
static void f_ScrollRect(cfm *c)
{
    touch_port(c);
    rect r;
    int dh = (int)(int16_t)(arg(c, 1) & 0xFFFF);
    int dv = (int)(int16_t)(arg(c, 2) & 0xFFFF);
    int y, x;
    cfm_gworld *g;
    uint32_t tmp;
    if (!arg(c, 0) || !(g = gw_find(c, c->cur_port))) return;
    r = g_rect(c, arg(c, 0));
    for (y = r.t; y < r.b; y++) {
        for (x = r.l; x < r.r; x++) {
            int sx = x - dh, sy = y - dv;
            tmp = c->back;
            if (sx >= r.l && sx < r.r && sy >= r.t && sy < r.b &&
                sx - g->ox >= 0 && sy - g->oy >= 0 &&
                sx - g->ox < g->w && sy - g->oy < g->h)
                tmp = g32(c, g->pixels + (uint32_t)(sy - g->oy) * g->rowbytes +
                          (uint32_t)(sx - g->ox) * 4);
            port_put(c, x, y, tmp);
        }
    }
}

static void f_MoveTo(cfm *c) { c->pen_x = arg(c, 0); c->pen_y = arg(c, 1); }
static void f_Move(cfm *c)
{
    c->pen_x = (uint32_t)((int32_t)c->pen_x + (int16_t)(arg(c, 0) & 0xFFFF));
    c->pen_y = (uint32_t)((int32_t)c->pen_y + (int16_t)(arg(c, 1) & 0xFFFF));
}

/* The pen is pen_w x pen_h and draws in the pen mode; QuickDraw hangs the pen
 * off the line's top edge, which nobody here depends on, so the pen is drawn
 * centred on the ideal line instead. */
static void line_to(cfm *c, int x1, int y1)
{
    int x0 = (int)(int16_t)(c->pen_x & 0xFFFF), y0 = (int)(int16_t)(c->pen_y & 0xFFFF);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    int px, py;

    for (;;) {
        for (py = 0; py < c->pen_h; py++)
            for (px = 0; px < c->pen_w; px++)
                port_plot(c, x0 + px, y0 + py, c->fore, c->pen_mode);
        if (x0 == x1 && y0 == y1) break;
        { int e2 = 2 * err;
          if (e2 >= dy) { err += dy; x0 += sx; }
          if (e2 <= dx) { err += dx; y0 += sy; } }
    }
    c->pen_x = (uint32_t)(uint16_t)x1; c->pen_y = (uint32_t)(uint16_t)y1;
}

static void f_LineTo(cfm *c)
{
    touch_port(c);
    line_to(c, (int)(int16_t)(arg(c, 0) & 0xFFFF), (int)(int16_t)(arg(c, 1) & 0xFFFF));
}

static void f_Line(cfm *c)
{
    int x = (int)(int16_t)(c->pen_x & 0xFFFF) + (int16_t)(arg(c, 0) & 0xFFFF);
    int y = (int)(int16_t)(c->pen_y & 0xFFFF) + (int16_t)(arg(c, 1) & 0xFFFF);
    touch_port(c);
    line_to(c, x, y);
}

static void f_PenSize(cfm *c)
{
    c->pen_w = (int)(int16_t)(arg(c, 0) & 0xFFFF);
    c->pen_h = (int)(int16_t)(arg(c, 1) & 0xFFFF);
    if (c->pen_w < 1) c->pen_w = 1;
    if (c->pen_h < 1) c->pen_h = 1;
}

/* PenNormal resets the pen to a 1x1 patCopy pen. patCopy is mode 8, and the
 * low three bits of it are srcCopy, which is how the plotter reads it. */
static void f_PenNormal(cfm *c) { c->pen_w = c->pen_h = 1; c->pen_mode = 8; }

/* The eight classic QuickDraw colours, which ForeColor and BackColor name. */
static uint32_t classic_colour(uint32_t which)
{
    switch (which) {
    case 33:  return 0x000000;               /* blackColor   */
    case 30:  return 0xFFFFFF;               /* whiteColor   */
    case 205: return 0xFF0000;               /* redColor     */
    case 341: return 0x00FF00;               /* greenColor   */
    case 409: return 0x0000FF;               /* blueColor    */
    case 273: return 0x00FFFF;               /* cyanColor    */
    case 137: return 0xFF00FF;               /* magentaColor */
    case 69:  return 0xFFFF00;               /* yellowColor  */
    default:  return 0x000000;
    }
}

static void f_ForeColor(cfm *c) { c->fore = classic_colour(arg(c, 0)); }
static void f_BackColor(cfm *c) { c->back = classic_colour(arg(c, 0)); }

/* An RGBColor is three 16-bit components. */
static uint32_t g_rgb(cfm *c, uint32_t a)
{
    return ((uint32_t)(g16(c, a) >> 8) << 16) |
           ((uint32_t)(g16(c, a + 2) >> 8) << 8) |
            (uint32_t)(g16(c, a + 4) >> 8);
}

static void s_rgb(cfm *c, uint32_t a, uint32_t rgb)
{
    s16(c, a,     (uint16_t)(((rgb >> 16) & 0xFF) * 0x101));
    s16(c, a + 2, (uint16_t)(((rgb >> 8)  & 0xFF) * 0x101));
    s16(c, a + 4, (uint16_t)((rgb & 0xFF) * 0x101));
}

static void f_RGBForeColor(cfm *c) { if (arg(c, 0)) c->fore = g_rgb(c, arg(c, 0)); }
static void f_RGBBackColor(cfm *c) { if (arg(c, 0)) c->back = g_rgb(c, arg(c, 0)); }
static void f_GetForeColor(cfm *c) { if (arg(c, 0)) s_rgb(c, arg(c, 0), c->fore); }
static void f_GetBackColor(cfm *c) { if (arg(c, 0)) s_rgb(c, arg(c, 0), c->back); }

static void f_PenMode(cfm *c)  { c->pen_mode = (int)arg(c, 0); }
static void f_TextMode(cfm *c) { c->text_mode = (int)arg(c, 0); }

/* A PenState is { Point pnLoc; Point pnSize; short pnMode; Pattern pnPat; }. */
static void f_GetPenState(cfm *c)
{
    uint32_t a = arg(c, 0);
    if (!a) return;
    s16(c, a, (uint16_t)c->pen_y); s16(c, a + 2, (uint16_t)c->pen_x);
    s16(c, a + 4, (uint16_t)c->pen_h); s16(c, a + 6, (uint16_t)c->pen_w);
    s16(c, a + 8, (uint16_t)c->pen_mode);
}

static void f_SetPenState(cfm *c)
{
    uint32_t a = arg(c, 0);
    if (!a) return;
    c->pen_y = g16(c, a); c->pen_x = g16(c, a + 2);
    c->pen_h = (int16_t)g16(c, a + 4); c->pen_w = (int16_t)g16(c, a + 6);
    if (c->pen_w < 1) c->pen_w = 1;
    if (c->pen_h < 1) c->pen_h = 1;
    c->pen_mode = (int)(int16_t)g16(c, a + 8);
}

/* GetPen writes a Point, which is { v, h } -- vertical first. */
static void f_GetPen(cfm *c)
{
    uint32_t a = arg(c, 0);
    if (!a) return;
    s16(c, a, (uint16_t)c->pen_y); s16(c, a + 2, (uint16_t)c->pen_x);
}

/* -------------------------------------------------------------------- text */

/* Two bitmap fonts -- 9px and 13px -- picked by the TextSize, larger sizes
 * integer-scaled from the 13px table. The corpus asks for 9-12pt almost
 * everywhere. The metrics below are what both GetFontInfo and StringWidth
 * report, so a plug-in's layout math and our drawing agree with each other
 * even though the glyphs are only approximately Geneva. */
static const macfont_glyph *tx_face_table(cfm *c, int *k, int *rows, int *base)
{
    int s = c->tx_size > 0 ? c->tx_size : 12;
    if (s <= 10) {
        *k = 1; *rows = MACFONT9_ROWS; *base = MACFONT9_BASELINE;
        return macfont9;
    }
    *k = (s + 8) / 13;
    if (*k < 1) *k = 1;
    *rows = MACFONT13_ROWS; *base = MACFONT13_BASELINE;
    return macfont13;
}

/* Mac Roman high characters, folded to the nearest ASCII the font has. */
static int mac_roman(int ch)
{
    switch (ch & 0xFF) {
    case 0xD2: case 0xD3: return '"';        /* curly double quotes */
    case 0xD4: case 0xD5: return '\'';       /* curly single quotes */
    case 0xD0: case 0xD1: return '-';        /* en and em dash      */
    case 0x85: return '.';                   /* ellipsis            */
    case 0xA5: return '*';                   /* bullet              */
    case 0xA9: return 'c';                   /* (c)                 */
    case 0xAA: return 'T';                   /* (TM)                */
    default:   return ch;
    }
}

static int char_width(cfm *c, int ch)
{
    int k, rows, base;
    const macfont_glyph *tab = tx_face_table(c, &k, &rows, &base);
    ch = mac_roman(ch);
    if (ch < MACFONT_FIRST || ch > MACFONT_LAST) ch = '?';
    return tab[ch - MACFONT_FIRST].advance * k + ((c->tx_face & 1) != 0);
}

/* Draw one glyph with its left edge at x and its baseline at `baseline`, in
 * the fore colour and the current text mode. Bold is a 1px overdraw;
 * underline a row below the baseline; italic is not faked (a sheared bitmap
 * reads worse than a straight one). */
static int draw_char(cfm *c, int ch, int x, int baseline)
{
    int k, rows, base, gy, gx, sx, sy, adv, b;
    const macfont_glyph *tab = tx_face_table(c, &k, &rows, &base);
    const macfont_glyph *gl;

    ch = mac_roman(ch);
    if (ch < MACFONT_FIRST || ch > MACFONT_LAST) ch = '?';
    gl = &tab[ch - MACFONT_FIRST];
    adv = char_width(c, ch);
    for (b = 0; b <= ((c->tx_face & 1) != 0); b++)        /* bold overdraw */
        for (gy = 0; gy < rows; gy++) {
            uint16_t bits = gl->rows[gy];
            if (!bits) continue;
            for (gx = 0; gx < gl->w; gx++) {
                if (!((bits >> gx) & 1)) continue;
                for (sy = 0; sy < k; sy++)
                    for (sx = 0; sx < k; sx++)
                        port_plot(c, x + b + gx * k + sx,
                                  baseline + (gy - base) * k + sy,
                                  c->fore, c->text_mode);
            }
        }
    if (c->tx_face & 4) {
        int i;
        for (i = 0; i < adv; i++)
            port_plot(c, x + i, baseline + k, c->fore, c->text_mode);
    }
    return adv;
}

static int text_width_n(cfm *c, uint32_t s, int n)
{
    int i, w = 0;
    for (i = 0; i < n; i++)
        w += char_width(c, ppc_read8(c->m, s + (uint32_t)i));
    return w;
}

static void draw_text_n(cfm *c, uint32_t s, int n)
{
    int i, x = (int)(int16_t)(c->pen_x & 0xFFFF);
    int base = (int)(int16_t)(c->pen_y & 0xFFFF);
    touch_port(c);
    for (i = 0; i < n; i++)
        x += draw_char(c, ppc_read8(c->m, s + (uint32_t)i), x, base);
    c->pen_x = (uint32_t)(uint16_t)x;
}

static void f_DrawString(cfm *c)
{
    uint32_t s = arg(c, 0);
    int n;
    if (!s) return;
    n = ppc_read8(c->m, s);
    draw_text_n(c, s + 1, n);
}

/* DrawText(textBuf, firstByte, byteCount) draws raw bytes, not a Pascal
 * string. */
static void f_DrawText(cfm *c)
{
    draw_text_n(c, arg(c, 0) + (int32_t)arg(c, 1), (int32_t)arg(c, 2));
}

static void f_StringWidth(cfm *c)
{
    uint32_t s = arg(c, 0);
    ret(c, s ? (uint32_t)text_width_n(c, s + 1, ppc_read8(c->m, s)) : 0);
}

static void f_TextWidth(cfm *c)
{
    ret(c, (uint32_t)text_width_n(c, arg(c, 0) + (int32_t)arg(c, 1),
                                  (int32_t)arg(c, 2)));
}

static void f_CharWidth(cfm *c)
{
    ret(c, (uint32_t)char_width(c, (int)(arg(c, 0) & 0xFF)));
}

static void f_TextFont(cfm *c) { c->tx_font = (int)(int16_t)(arg(c, 0) & 0xFFFF); }
static void f_TextFace(cfm *c) { c->tx_face = (int)(arg(c, 0) & 0xFF); }
static void f_TextSize(cfm *c) { c->tx_size = (int)(int16_t)(arg(c, 0) & 0xFFFF); }

/* FontInfo is { short ascent, descent, widMax, leading }. */
static void f_GetFontInfo(cfm *c)
{
    uint32_t a = arg(c, 0);
    int k, rows, base;
    if (!a) return;
    tx_face_table(c, &k, &rows, &base);
    s16(c, a,     (uint16_t)(base * k));
    s16(c, a + 2, (uint16_t)((rows - base) * k));
    s16(c, a + 4, (uint16_t)(12 * k));
    s16(c, a + 6, (uint16_t)(2 * k));
}

/* c2pstr converts a C string to a Pascal string in place: the bytes shift up
 * by one and the length lands in front. */
static void f_c2pstr(cfm *c)
{
    uint32_t s = arg(c, 0);
    int n = 0, i;
    if (!s) { ret(c, 0); return; }
    while (n < 255 && ppc_read8(c->m, s + (uint32_t)n)) n++;
    for (i = n - 1; i >= 0; i--)
        ppc_write8(c->m, s + 1 + (uint32_t)i, ppc_read8(c->m, s + (uint32_t)i));
    ppc_write8(c->m, s, (uint8_t)n);
    ret(c, s);
}

static void f_p2cstr(cfm *c)
{
    uint32_t s = arg(c, 0);
    int n, i;
    if (!s) { ret(c, 0); return; }
    n = ppc_read8(c->m, s);
    for (i = 0; i < n; i++)
        ppc_write8(c->m, s + (uint32_t)i, ppc_read8(c->m, s + 1 + (uint32_t)i));
    ppc_write8(c->m, s + (uint32_t)n, 0);
    ret(c, s);
}

/* ------------------------------------------------------------------- rects */

static void f_SetRect(cfm *c)
{
    uint32_t a = arg(c, 0);              /* (rect, left, top, right, bottom) */
    if (!a) return;
    s16(c, a,     (uint16_t)arg(c, 2));  /* top    */
    s16(c, a + 2, (uint16_t)arg(c, 1));  /* left   */
    s16(c, a + 4, (uint16_t)arg(c, 4));  /* bottom */
    s16(c, a + 6, (uint16_t)arg(c, 3));  /* right  */
}

static void f_InsetRect(cfm *c)
{
    uint32_t a = arg(c, 0);
    int dh = (int16_t)(arg(c, 1) & 0xFFFF), dv = (int16_t)(arg(c, 2) & 0xFFFF);
    rect r;
    if (!a) return;
    r = g_rect(c, a);
    r.t += dv; r.b -= dv; r.l += dh; r.r -= dh;
    if (r.b < r.t) r.b = r.t;
    if (r.r < r.l) r.r = r.l;
    s16(c, a, (uint16_t)r.t);     s16(c, a + 2, (uint16_t)r.l);
    s16(c, a + 4, (uint16_t)r.b); s16(c, a + 6, (uint16_t)r.r);
}

static void f_OffsetRect(cfm *c)
{
    uint32_t a = arg(c, 0);
    int dh = (int16_t)(arg(c, 1) & 0xFFFF), dv = (int16_t)(arg(c, 2) & 0xFFFF);
    rect r;
    if (!a) return;
    r = g_rect(c, a);
    s16(c, a,     (uint16_t)(r.t + dv)); s16(c, a + 2, (uint16_t)(r.l + dh));
    s16(c, a + 4, (uint16_t)(r.b + dv)); s16(c, a + 6, (uint16_t)(r.r + dh));
}

static rect rect_sect(rect a, rect b)
{
    rect r;
    r.t = a.t > b.t ? a.t : b.t;
    r.l = a.l > b.l ? a.l : b.l;
    r.b = a.b < b.b ? a.b : b.b;
    r.r = a.r < b.r ? a.r : b.r;
    if (r.b < r.t) r.b = r.t;
    if (r.r < r.l) r.r = r.l;
    return r;
}

static int rect_empty(rect r) { return r.t >= r.b || r.l >= r.r; }

static void s_rect(cfm *c, uint32_t a, rect r)
{
    s16(c, a, (uint16_t)r.t);     s16(c, a + 2, (uint16_t)r.l);
    s16(c, a + 4, (uint16_t)r.b); s16(c, a + 6, (uint16_t)r.r);
}

static void f_SectRect(cfm *c)
{
    rect r;
    if (!arg(c, 2)) { ret(c, 0); return; }
    r = rect_sect(g_rect(c, arg(c, 0)), g_rect(c, arg(c, 1)));
    s_rect(c, arg(c, 2), r);
    ret(c, !rect_empty(r));
}

static void f_UnionRect(cfm *c)
{
    rect a, b, r;
    if (!arg(c, 2)) return;
    a = g_rect(c, arg(c, 0)); b = g_rect(c, arg(c, 1));
    r.t = a.t < b.t ? a.t : b.t;
    r.l = a.l < b.l ? a.l : b.l;
    r.b = a.b > b.b ? a.b : b.b;
    r.r = a.r > b.r ? a.r : b.r;
    s_rect(c, arg(c, 2), r);
}

/* A Point in a register packs v into the high word and h into the low one. */
static void f_PtInRect(cfm *c)
{
    uint32_t pt = arg(c, 0);
    int v = (int16_t)(pt >> 16), h = (int16_t)(pt & 0xFFFF);
    rect r = g_rect(c, arg(c, 1));
    ret(c, v >= r.t && v < r.b && h >= r.l && h < r.r);
}

static void f_EqualRect(cfm *c)
{
    rect a = g_rect(c, arg(c, 0)), b = g_rect(c, arg(c, 1));
    ret(c, a.t == b.t && a.l == b.l && a.b == b.b && a.r == b.r);
}

static void f_EmptyRect(cfm *c) { ret(c, rect_empty(g_rect(c, arg(c, 0)))); }

static void f_Pt2Rect(cfm *c)
{
    uint32_t p1 = arg(c, 0), p2 = arg(c, 1);
    rect r;
    if (!arg(c, 2)) return;
    r.t = (int16_t)(p1 >> 16); r.l = (int16_t)(p1 & 0xFFFF);
    r.b = (int16_t)(p2 >> 16); r.r = (int16_t)(p2 & 0xFFFF);
    s_rect(c, arg(c, 2), r);
}

/* ------------------------------------------- regions (rectangular) and clip */

/* A guest Region is { int16 rgnSize; Rect rgnBBox; }, and a rgnSize of 10 is
 * exactly a rectangular region on a real Mac -- which is all any plug-in here
 * builds, so a rect is the whole implementation and the guest can even read
 * the record back without being surprised. */
static uint32_t rgn_data(cfm *c, uint32_t rgnh)
{
    uint32_t d = rgnh ? peek32(c, rgnh) : 0;
    return d;
}

static rect rgn_get(cfm *c, uint32_t rgnh)
{
    uint32_t d = rgn_data(c, rgnh);
    rect r = { 0, 0, 0, 0 };
    if (d) r = g_rect(c, d + 2);
    return r;
}

static void rgn_set(cfm *c, uint32_t rgnh, rect r)
{
    uint32_t d = rgn_data(c, rgnh);
    if (!d) return;
    s16(c, d, 10);
    s_rect(c, d + 2, r);
}

static void f_NewRgn(cfm *c)
{
    uint32_t h = heap_alloc(c, 4, 0), d = heap_alloc(c, 10, 1);
    if (!h || !d) { ret(c, 0); return; }
    s32(c, h, d);
    s16(c, d, 10);                             /* a rectangular region */
    ret(c, h);
}

/* The heap never reclaims, so disposing is nothing -- a leak by policy, the
 * same as DisposePtr above. */
static void f_DisposeRgn(cfm *c) { (void)c; }

static void f_RectRgn(cfm *c)    { rgn_set(c, arg(c, 0), g_rect(c, arg(c, 1))); }

static void f_SetRectRgn(cfm *c)  /* (rgn, left, top, right, bottom) */
{
    rect r;
    r.l = (int16_t)(arg(c, 1) & 0xFFFF); r.t = (int16_t)(arg(c, 2) & 0xFFFF);
    r.r = (int16_t)(arg(c, 3) & 0xFFFF); r.b = (int16_t)(arg(c, 4) & 0xFFFF);
    rgn_set(c, arg(c, 0), r);
}

static void f_CopyRgn(cfm *c) { rgn_set(c, arg(c, 0), rgn_get(c, arg(c, 1))); }

static void f_SetEmptyRgn(cfm *c)
{
    rect r = { 0, 0, 0, 0 };
    rgn_set(c, arg(c, 0), r);
}

static void f_EmptyRgn(cfm *c) { ret(c, rect_empty(rgn_get(c, arg(c, 0)))); }

static void f_OffsetRgn(cfm *c)
{
    rect r = rgn_get(c, arg(c, 0));
    int dh = (int16_t)(arg(c, 1) & 0xFFFF), dv = (int16_t)(arg(c, 2) & 0xFFFF);
    r.t += dv; r.b += dv; r.l += dh; r.r += dh;
    rgn_set(c, arg(c, 0), r);
}

static void f_InsetRgn(cfm *c)
{
    rect r = rgn_get(c, arg(c, 0));
    int dh = (int16_t)(arg(c, 1) & 0xFFFF), dv = (int16_t)(arg(c, 2) & 0xFFFF);
    r.t += dv; r.b -= dv; r.l += dh; r.r -= dh;
    if (r.b < r.t) r.b = r.t;
    if (r.r < r.l) r.r = r.l;
    rgn_set(c, arg(c, 0), r);
}

static void f_SectRgn(cfm *c)
{
    rgn_set(c, arg(c, 2),
            rect_sect(rgn_get(c, arg(c, 0)), rgn_get(c, arg(c, 1))));
}

static void f_UnionRgn(cfm *c)
{
    rect a = rgn_get(c, arg(c, 0)), b = rgn_get(c, arg(c, 1)), r;
    r.t = a.t < b.t ? a.t : b.t;
    r.l = a.l < b.l ? a.l : b.l;
    r.b = a.b > b.b ? a.b : b.b;
    r.r = a.r > b.r ? a.r : b.r;
    rgn_set(c, arg(c, 2), r);
}

static void f_PtInRgn(cfm *c)
{
    uint32_t pt = arg(c, 0);
    int v = (int16_t)(pt >> 16), h = (int16_t)(pt & 0xFFFF);
    rect r = rgn_get(c, arg(c, 1));
    ret(c, v >= r.t && v < r.b && h >= r.l && h < r.r);
}

static void f_EqualRgn(cfm *c)
{
    rect a = rgn_get(c, arg(c, 0)), b = rgn_get(c, arg(c, 1));
    ret(c, a.t == b.t && a.l == b.l && a.b == b.b && a.r == b.r);
}

static void rgn_paint(cfm *c, uint32_t rgnh, int what)
{
    rect r = rgn_get(c, rgnh);
    int y, x;
    touch_port(c);
    for (y = r.t; y < r.b; y++)
        for (x = r.l; x < r.r; x++)
            port_put(c, x, y, what == 2 ? c->back : c->fore);
}

static void f_EraseRgn(cfm *c) { rgn_paint(c, arg(c, 0), 2); }
static void f_PaintRgn(cfm *c) { rgn_paint(c, arg(c, 0), 0); }
static void f_FillRgn(cfm *c)  { rgn_paint(c, arg(c, 0), 0); }

static void f_InvertRgn(cfm *c)
{
    rect r = rgn_get(c, arg(c, 0));
    int y, x;
    touch_port(c);
    for (y = r.t; y < r.b; y++)
        for (x = r.l; x < r.r; x++)
            port_plot(c, x, y, 0xFFFFFF, 2);
}

static void f_FrameRgn(cfm *c)
{
    rect r = rgn_get(c, arg(c, 0));
    int y, x;
    touch_port(c);
    for (x = r.l; x < r.r; x++) { port_put(c, x, r.t, c->fore); port_put(c, x, r.b - 1, c->fore); }
    for (y = r.t; y < r.b; y++) { port_put(c, r.l, y, c->fore); port_put(c, r.r - 1, y, c->fore); }
}

/* The clip is a rectangle per port -- a port's clipRgn, reduced to its bbox,
 * which is what every caller here sets it to anyway. */
static void f_GetClip(cfm *c)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    rect r = { 0, 0, 0, 0 };
    if (g) { r.t = g->clip_t + g->oy; r.l = g->clip_l + g->ox;
             r.b = g->clip_b + g->oy; r.r = g->clip_r + g->ox; }
    rgn_set(c, arg(c, 0), r);
}

static void f_SetClip(cfm *c)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    rect r;
    if (!g) return;
    r = rgn_get(c, arg(c, 0));
    if (rect_empty(r)) { r.t = 0; r.l = 0; r.b = 32767; r.r = 32767; }
    g->clip_t = r.t - g->oy; g->clip_l = r.l - g->ox;
    g->clip_b = r.b - g->oy; g->clip_r = r.r - g->ox;
}

static void f_ClipRect(cfm *c)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    rect r, cl;
    if (!g || !arg(c, 0)) return;
    r = g_rect(c, arg(c, 0));
    cl.t = g->clip_t + g->oy; cl.l = g->clip_l + g->ox;
    cl.b = g->clip_b + g->oy; cl.r = g->clip_r + g->ox;
    cl = rect_sect(cl, r);
    g->clip_t = cl.t - g->oy; g->clip_l = cl.l - g->ox;
    g->clip_b = cl.b - g->oy; g->clip_r = cl.r - g->ox;
}

/* ------------------------------------------------------------- port origin */

/* SetOrigin(h, v) makes local (h, v) the top-left of the port's bitmap: a
 * pixel lands at local minus origin, which port_plot applies. The clip is
 * stored in bitmap coordinates, so it shifts the other way. */
static void f_SetOrigin(cfm *c)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    int h = (int16_t)(arg(c, 0) & 0xFFFF), v = (int16_t)(arg(c, 1) & 0xFFFF);
    int dh, dv;
    if (!g) return;
    dh = g->ox - h; dv = g->oy - v;
    g->clip_l += dh; g->clip_r += dh;
    g->clip_t += dv; g->clip_b += dv;
    g->ox = h; g->oy = v;
}

/* Global and local differ by the port origin; with no screen, the origin is
 * almost always zero and these are the identity -- which is the right answer
 * for the mouse coordinates the corpus converts with them. */
static void f_GlobalToLocal(cfm *c)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    uint32_t a = arg(c, 0);
    if (!a) return;
    s16(c, a,     (uint16_t)(int16_t)(g16(c, a)     + (g ? g->oy : 0)));
    s16(c, a + 2, (uint16_t)(int16_t)(g16(c, a + 2) + (g ? g->ox : 0)));
}

static void f_LocalToGlobal(cfm *c)
{
    cfm_gworld *g = gw_find(c, c->cur_port);
    uint32_t a = arg(c, 0);
    if (!a) return;
    s16(c, a,     (uint16_t)(int16_t)(g16(c, a)     - (g ? g->oy : 0)));
    s16(c, a + 2, (uint16_t)(int16_t)(g16(c, a + 2) - (g ? g->ox : 0)));
}

static double mono_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void cfm_set_input_pump(cfm *c, void (*fn)(void *ud), void *ud)
{
    if (!c) return;
    c->pump = fn;
    c->pump_ud = ud;
}

/* Called from every function that reports input state. Two jobs: let the host
 * refresh that state, and stop a spin loop burning the interpreter.
 *
 * Both need rate limiting, because a drag loop calls these millions of times a
 * second -- pumping the host's event queue that often would be far slower than
 * the plugin. Between pumps, hand the CPU back instead of spinning: the loop is
 * waiting for a human to move or release a mouse, and nothing it computes in the
 * meantime matters. */
static void input_poll(cfm *c)
{
    double t;

    if (!c->pump) return;
    t = mono_now();
    if (t - c->last_pump >= 0.0005) {          /* at most 2000 times a second */
        c->last_pump = t;
        c->pump(c->pump_ud);
    } else if (c->mouse_down) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000;                   /* 100 us */
        nanosleep(&ts, NULL);
    }
}

/* ------------------------------------------------------ cursor and input */

static void f_InitCursor(cfm *c) { (void)c; }
static void f_SetCursor(cfm *c)  { (void)c; }
static void f_GetCursor(cfm *c)  { ret(c, 0); }      /* no cursor resources */

/* The mouse, as the embedder last reported it. A Classic editor polls these from
 * effEditIdle rather than being sent events, so they are the whole input path. */
static void f_Button(cfm *c)    { input_poll(c); ret(c, (uint32_t)(c->mouse_down != 0)); }
/* StillDown asks whether the button that started the current click is also still
 * down. Answering it from the same state is what lets a plug-in's drag loop both
 * track a drag and eventually see the release. */
static void f_StillDown(cfm *c) { input_poll(c); ret(c, (uint32_t)(c->mouse_down != 0)); }
/* A Point is { short v; short h; } -- vertical first, which is the opposite of
 * every other coordinate pair and the easy mistake to make here. */
static void f_GetMouse(cfm *c)
{
    uint32_t a = arg(c, 0);
    input_poll(c);
    if (!a) return;
    s16(c, a,     (uint16_t)(int16_t)c->mouse_y);
    s16(c, a + 2, (uint16_t)(int16_t)c->mouse_x);
    if (getenv("CFMMOUSE")) {
        cfm_gworld *g = gw_find(c, c->cur_port);
        fprintf(stderr, "  GetMouse -> (%d,%d) down=%d  port=0x%08x%s",
                c->mouse_x, c->mouse_y, c->mouse_down, c->cur_port,
                c->cur_port == c->ed_window ? " (the editor window)" : "");
        if (g) fprintf(stderr, " %dx%d", g->w, g->h);
        fprintf(stderr, "  portRect=(%d,%d,%d,%d)\n",
                (int16_t)g16(c, c->cur_port + 16), (int16_t)g16(c, c->cur_port + 18),
                (int16_t)g16(c, c->cur_port + 20), (int16_t)g16(c, c->cur_port + 22));
    }
}
static void f_GetKeys(cfm *c)
{
    uint32_t a = arg(c, 0);
    input_poll(c);
    if (a) { s32(c, a, 0); s32(c, a + 4, 0); s32(c, a + 8, 0); s32(c, a + 12, 0); }
}

/* ------------------------------------------------------------- misc, later */

static void f_tan(cfm *c)  { retf(c, tan(farg(c, 0))); }
static void f_atan(cfm *c) { retf(c, atan(farg(c, 0))); }
static void f_acos(cfm *c) { retf(c, acos(farg(c, 0))); }
static void f_asin(cfm *c) { retf(c, asin(farg(c, 0))); }
static void f_nan(cfm *c)  { retf(c, (double)NAN); }

/* MSL's __fpclassifyf/__fpclassify: the glibc FP_* order (NaN, infinite, zero,
 * subnormal, normal), which is what the callers compare against. */
static void f___fpclassifyf(cfm *c)
{
    double d = farg(c, 0);
    ret(c, (uint32_t)fpclassify(d));
}

/* dec2num is num2dec's inverse: the decimal struct is
 * { SInt8 sgn; SInt8 unused; short exp; char sig[21] } and the value is
 * (-1)^sgn * sig-as-integer * 10^exp. */
static void f_dec2num(cfm *c)
{
    uint32_t a = arg(c, 0);
    int sgn, exp10, n, i;
    double v = 0.0;
    if (!a) { retf(c, 0.0); return; }
    sgn = ppc_read8(c->m, a);
    exp10 = (int16_t)g16(c, a + 2);
    n = ppc_read8(c->m, a + 4);
    if (n > 20) n = 20;
    for (i = 0; i < n; i++) {
        int dgt = ppc_read8(c->m, a + 5 + (uint32_t)i) - '0';
        if (dgt < 0 || dgt > 9) break;
        v = v * 10.0 + dgt;
    }
    v *= pow(10.0, (double)exp10);
    retf(c, sgn ? -v : v);
}

/* Seconds since 1904-01-01, as Mac OS counts them. */
static void f_GetDateTime(cfm *c)
{
    uint32_t secs = (uint32_t)time(NULL) + 2082844800u;
    if (arg(c, 0)) s32(c, arg(c, 0), secs);
    ret(c, secs);
}

/* No event is ever pending: the guest is a plug-in, not an application, and
 * its input arrives through effEditMouse and the Button/GetMouse poll. The
 * one thing a real event loop would give it is a chance for the host to run,
 * which input_poll provides. */
static void event_none(cfm *c)
{
    uint32_t evt = arg(c, 1);
    int i;
    input_poll(c);
    if (evt) for (i = 0; i < 16; i++) ppc_write8(c->m, evt + (uint32_t)i, 0);
    ret(c, 0);
}
static void f_GetNextEvent(cfm *c)  { event_none(c); }
static void f_WaitNextEvent(cfm *c) { event_none(c); }
static void f_SystemTask(cfm *c)    { input_poll(c); }

static void f_Delay(cfm *c)
{
    uint32_t ticks = arg(c, 0);
    struct timespec ts;
    if (ticks > 600) ticks = 600;        /* ten seconds is long enough */
    ts.tv_sec = ticks / 60;
    ts.tv_nsec = (long)(ticks % 60) * 16666666L;
    nanosleep(&ts, NULL);
    if (arg(c, 1)) s32(c, arg(c, 1), ticks_now(c));
}

/* Invalidation means "redraw this later". The host redraws the whole editor
 * on every pump, so there is nothing to record -- but the call must succeed
 * or the plug-in concludes its window is broken. */
static void f_noop_ok(cfm *c) { (void)c; }

static void f_WaitMouseUp(cfm *c)   { input_poll(c); ret(c, !c->mouse_down); }

/* WaitMouseMoved asks whether the mouse left the point. The drag loops that
 * call it want to know whether to bother redrawing, so answer honestly. */
static void f_WaitMouseMoved(cfm *c)
{
    uint32_t pt = arg(c, 0);
    input_poll(c);
    ret(c, c->mouse_x != (int16_t)(pt & 0xFFFF) ||
           c->mouse_y != (int16_t)(pt >> 16));
}

/* Gestalt additions and CFM introspection. Adding a selector is nothing here;
 * finding a symbol nobody registered answers "not there". */
static void f_NewGestaltValue(cfm *c)     { reterr(c, noErr); }
static void f_ReplaceGestaltValue(cfm *c) { reterr(c, noErr); }
static void f_CountSymbols(cfm *c)        { ret(c, 0); }
static void f_FindSymbol(cfm *c)          { ret(c, 0); }
static void f_DisposeRoutineDescriptor(cfm *c) { (void)c; }

/* Internet Config: there is no internet configuration. Fail politely at the
 * door and the plug-in disables its "visit our site" button. */
static void f_ICStart(cfm *c)           { if (arg(c, 0)) s32(c, arg(c, 0), 0);
                                          reterr(c, paramErr); }
static void f_ICStop(cfm *c)            { reterr(c, noErr); }
static void f_ICLaunchURL(cfm *c)       { reterr(c, paramErr); }
static void f_ICFindConfigFile(cfm *c)  { reterr(c, paramErr); }

/* ------------------------------------------------- TextEdit (single line) */

/* A minimal TextEdit: enough for the numeric-entry fields the Destroy FX
 * editors pop up over a value when it is clicked. Single style, single line,
 * no scrap -- the TERec layout below is the real one (documented in Inside
 * Macintosh: TextEdit), so a plug-in that reads its fields directly sees
 * consistent values. */
#define TE_DEST      0    /* Rect destRect   */
#define TE_VIEW      8    /* Rect viewRect   */
#define TE_SELRECT   16   /* Rect selRect    */
#define TE_LINEH     24   /* short           */
#define TE_ASCENT    26   /* short           */
#define TE_SELPOINT  28   /* Point           */
#define TE_SELSTART  32   /* short           */
#define TE_SELEND    34   /* short           */
#define TE_ACTIVE    36   /* short           */
#define TE_JUST      58   /* short           */
#define TE_LENGTH    60   /* short           */
#define TE_HTEXT     62   /* Handle          */
#define TE_TXFONT    74   /* short           */
#define TE_TXFACE    76   /* byte + filler   */
#define TE_TXMODE    78   /* short           */
#define TE_TXSIZE    80   /* short           */
#define TE_INPORT    82   /* GrafPtr         */
#define TE_NLINES    94   /* short           */
#define TE_LINESTART 96   /* short[]         */
#define TE_BYTES     128

static uint32_t te_rec(cfm *c, uint32_t hte)
{
    return hte ? peek32(c, hte) : 0;
}

static uint32_t te_text(cfm *c, uint32_t te)
{
    uint32_t h = g32(c, te + TE_HTEXT);
    return h ? peek32(c, h) : 0;
}

static int te_len(cfm *c, uint32_t te)
{
    return (int)(int16_t)g16(c, te + TE_LENGTH);
}

static void f_TENew(cfm *c)
{
    uint32_t dr = arg(c, 0), vr = arg(c, 1);
    uint32_t h, te, ht, hd;
    int k, rows, base;

    h  = heap_alloc(c, 4, 0);
    te = heap_alloc(c, TE_BYTES, 1);
    ht = heap_alloc(c, 4, 0);              /* the text handle   */
    hd = heap_alloc(c, 256, 1);            /* and its bytes     */
    if (!h || !te || !ht || !hd) { ret(c, 0); return; }
    s32(c, h, te);
    s32(c, ht, hd);
    s32(c, te + TE_HTEXT, ht);
    if (dr) { rect r = g_rect(c, dr); s_rect(c, te + TE_DEST, r);
              s_rect(c, te + TE_SELRECT, r); }
    if (vr) { rect r = g_rect(c, vr); s_rect(c, te + TE_VIEW, r); }
    tx_face_table(c, &k, &rows, &base);
    s16(c, te + TE_LINEH,  (uint16_t)((rows + 2) * k));
    s16(c, te + TE_ASCENT, (uint16_t)(base * k));
    s16(c, te + TE_SELPOINT,     0xFFFF);
    s16(c, te + TE_SELPOINT + 2, 0xFFFF);
    s16(c, te + TE_TXFONT, (uint16_t)c->tx_font);
    ppc_write8(c->m, te + TE_TXFACE, (uint8_t)c->tx_face);
    s16(c, te + TE_TXMODE, (uint16_t)c->text_mode);
    s16(c, te + TE_TXSIZE, (uint16_t)c->tx_size);
    s32(c, te + TE_INPORT, c->cur_port);
    s16(c, te + TE_NLINES, 1);
    ret(c, h);
}

static void f_TEDispose(cfm *c)    { (void)c; }   /* the heap never reclaims */
static void f_TEActivate(cfm *c)   { uint32_t te = te_rec(c, arg(c, 0));
                                     if (te) s16(c, te + TE_ACTIVE, 1); }
static void f_TEDeactivate(cfm *c) { uint32_t te = te_rec(c, arg(c, 0));
                                     if (te) s16(c, te + TE_ACTIVE, 0); }
static void f_TEIdle(cfm *c)       { (void)c; }

static void f_TESetText(cfm *c)
{
    uint32_t src = arg(c, 0), te = te_rec(c, arg(c, 2));
    int32_t len = (int32_t)arg(c, 1);
    uint32_t hd;
    int i;
    if (!te || !src || len < 0) return;
    if (len > 255) len = 255;
    if (!(hd = te_text(c, te))) return;
    for (i = 0; i < len; i++)
        ppc_write8(c->m, hd + (uint32_t)i, ppc_read8(c->m, src + (uint32_t)i));
    s16(c, te + TE_LENGTH, (uint16_t)len);
    s16(c, te + TE_SELEND, (uint16_t)len);
    s16(c, te + TE_SELSTART, 0);
}

static void f_TEGetText(cfm *c)
{
    uint32_t te = te_rec(c, arg(c, 0));
    ret(c, te ? g32(c, te + TE_HTEXT) : 0);
}

/* Replace the selection with what TEKey delivers. */
static void te_replace_sel(cfm *c, uint32_t te, const uint8_t *ins, int nins)
{
    uint32_t hd = te_text(c, te);
    int len = te_len(c, te), i;
    int ss = (int)(int16_t)g16(c, te + TE_SELSTART);
    int se = (int)(int16_t)g16(c, te + TE_SELEND);
    int tail;
    if (!hd) return;
    if (ss < 0) ss = 0;
    if (se < ss) se = ss;
    if (se > len) se = len;
    tail = len - se;
    if (ss + nins + tail > 255) nins = 255 - ss - tail;
    if (nins < 0) nins = 0;
    /* Shifting the tail right overlaps its source, so copy it backwards. */
    if (ss + nins > se)
        for (i = tail - 1; i >= 0; i--)
            ppc_write8(c->m, hd + (uint32_t)(ss + nins + i),
                       ppc_read8(c->m, hd + (uint32_t)(se + i)));
    else
        for (i = 0; i < tail; i++)
            ppc_write8(c->m, hd + (uint32_t)(ss + nins + i),
                       ppc_read8(c->m, hd + (uint32_t)(se + i)));
    for (i = 0; i < nins; i++)
        ppc_write8(c->m, hd + (uint32_t)(ss + i), ins[i]);
    len = ss + nins + tail;
    s16(c, te + TE_LENGTH, (uint16_t)len);
    s16(c, te + TE_SELSTART, (uint16_t)(ss + nins));
    s16(c, te + TE_SELEND,   (uint16_t)(ss + nins));
}

static void f_TEKey(cfm *c)
{
    int key = (int)(arg(c, 0) & 0xFF);
    uint32_t te = te_rec(c, arg(c, 1));
    uint8_t ch;
    if (!te || !g16(c, te + TE_ACTIVE)) return;
    if (key == 8 || key == 0x7F) {                   /* backspace */
        int ss = (int)(int16_t)g16(c, te + TE_SELSTART);
        int se = (int)(int16_t)g16(c, te + TE_SELEND);
        if (ss == se && se > 0) { ss = se - 1; s16(c, te + TE_SELSTART, (uint16_t)ss); }
        te_replace_sel(c, te, NULL, 0);
        return;
    }
    if (key < 32 && key != 13) return;               /* return ends it too */
    if (key == 13) return;                           /* the caller reads it back */
    ch = (uint8_t)key;
    te_replace_sel(c, te, &ch, 1);
}

static void f_TESetSelect(cfm *c)
{
    uint32_t te = te_rec(c, arg(c, 2));
    int32_t ss = (int32_t)arg(c, 0), se = (int32_t)arg(c, 1);
    int len;
    if (!te) return;
    len = te_len(c, te);
    if (ss < 0) ss = 0;
    if (se < ss) se = ss;
    if (ss > len) ss = len;
    if (se > len) se = len;
    s16(c, te + TE_SELSTART, (uint16_t)ss);
    s16(c, te + TE_SELEND,   (uint16_t)se);
}

static void f_TEDelete(cfm *c)
{
    uint32_t te = te_rec(c, arg(c, 0));
    if (te) te_replace_sel(c, te, NULL, 0);
}

static void f_TESetAlignment(cfm *c)
{
    uint32_t te = te_rec(c, arg(c, 1));
    if (te) s16(c, te + TE_JUST, (uint16_t)arg(c, 0));
}

/* The x offset of character i within the field, from the left edge of the
 * destination rect, honouring the alignment. */
static int te_char_x(cfm *c, uint32_t te, int i)
{
    uint32_t hd = te_text(c, te);
    rect d = g_rect(c, te + TE_DEST);
    int just = (int)(int16_t)g16(c, te + TE_JUST);
    int x, total, n = te_len(c, te);
    if (i > n) i = n;
    x = d.l + 2;
    if (just == 1 || just == -1) {                     /* centre or right */
        total = hd ? text_width_n(c, hd, n) : 0;
        if (just == 1) x = d.l + (d.r - d.l - total) / 2;
        else           x = d.r - 2 - total;
    }
    return x + (hd && i > 0 ? text_width_n(c, hd, i) : 0);
}

static void f_TEClick(cfm *c)
{
    uint32_t pt = arg(c, 0), te = te_rec(c, arg(c, 2));
    uint32_t hd;
    int v, hx, i, n, x0;
    if (!te) return;
    hd = te_text(c, te);
    v = (int16_t)(pt >> 16); hx = (int16_t)(pt & 0xFFFF);
    (void)v;
    n = te_len(c, te);
    for (i = 0; i < n; i++) {                          /* first gap past the click */
        int xa = te_char_x(c, te, i), xb = te_char_x(c, te, i + 1);
        if (hx < (xa + xb) / 2) break;
    }
    s16(c, te + TE_SELSTART, (uint16_t)i);
    s16(c, te + TE_SELEND,   (uint16_t)i);
    s16(c, te + TE_SELPOINT,     (uint16_t)(int16_t)(pt >> 16));
    s16(c, te + TE_SELPOINT + 2, (uint16_t)(int16_t)(pt & 0xFFFF));
    x0 = te_char_x(c, te, i);                          /* selRect at the caret */
    s16(c, te + TE_SELRECT + 2, (uint16_t)x0);
    s16(c, te + TE_SELRECT + 6, (uint16_t)x0);
}

/* TEUpdate(rUpdate, hTE): draw the field. The text goes on the font ascent
 * line inside the destination rect, with a real selection highlight and a
 * caret, since a text field the user cannot see is not a text field. */
static void f_TEUpdate(cfm *c)
{
    uint32_t te = te_rec(c, arg(c, 1));
    uint32_t hd, save_font, save_face, save_size, save_mode;
    rect d;
    int n, i, x, base, active, ss, se;
    int k, rows, asc;

    if (!te) return;
    hd = te_text(c, te);
    d = g_rect(c, te + TE_DEST);
    n = te_len(c, te);
    active = (int16_t)g16(c, te + TE_ACTIVE) != 0;
    ss = (int16_t)g16(c, te + TE_SELSTART);
    se = (int16_t)g16(c, te + TE_SELEND);

    /* The field keeps its own style; borrow the current port state around the
     * draw and put it back after. */
    save_font = c->tx_font; save_face = c->tx_face;
    save_size = c->tx_size; save_mode = c->text_mode;
    c->tx_font = (int16_t)g16(c, te + TE_TXFONT);
    c->tx_face = ppc_read8(c->m, te + TE_TXFACE);
    c->tx_size = (int16_t)g16(c, te + TE_TXSIZE);
    c->text_mode = 0;                                  /* srcCopy */
    tx_face_table(c, &k, &rows, &asc);

    for (i = d.t; i < d.b; i++) {                      /* erase to background */
        int j;
        for (j = d.l; j < d.r; j++) port_put(c, j, i, c->back);
    }
    touch_port(c);
    base = d.t + asc * k;
    /* The selection highlight goes under the text. */
    if (active && se > ss) {
        int xa = te_char_x(c, te, ss), xb = te_char_x(c, te, se), yy, xx;
        for (yy = d.t; yy < d.b; yy++)
            for (xx = xa; xx < xb; xx++)
                port_plot(c, xx, yy, 0xFFFFFF, 2 /* xor: inverted highlight */);
    }
    x = te_char_x(c, te, 0);
    for (i = 0; i < n && hd; i++)
        x += draw_char(c, ppc_read8(c->m, hd + (uint32_t)i), x, base);
    if (active && ss == se) {                          /* the caret */
        int cx = te_char_x(c, te, ss), yy;
        for (yy = d.t + 1; yy < d.b - 1; yy++)
            port_plot(c, cx, yy, 0xFFFFFF, 2);
    }
    c->tx_font = save_font; c->tx_face = save_face;
    c->tx_size = save_size; c->text_mode = (int)save_mode;
}

/* ------------------------------------------------------------- resources */

/* GetResource(type, id) -- a copy of the resource in guest memory, behind a
 * handle. Asking twice for the same resource returns the same handle, which is
 * what Mac OS does and what a caller that releases once depends on. Returning
 * NULL for something absent is also correct, and callers handle it. */
static void f_GetResource(cfm *c)
{
    uint32_t type = arg(c, 0);
    int id = (int)(int16_t)(arg(c, 1) & 0xFFFF);
    const uint8_t *bytes;
    uint32_t size = 0, h;
    int i;

    for (i = 0; i < MAX_HANDLES; i++)
        if (c->handles[i].used && c->handles[i].type == type &&
            c->handles[i].id == id)
            { ret(c, c->handles[i].handle); return; }

    if (!(bytes = res_find(c, type, id, &size))) { ret(c, 0); return; }
    if (!(h = handle_new(c, size, bytes))) { ret(c, 0); return; }
    { cfm_handle *hd = handle_find(c, h);
      if (hd) { hd->type = type; hd->id = id; } }
    ret(c, h);
}

static void f_ReleaseResource(cfm *c)
{
    /* The bytes stay: a plug-in that releases a resource and later asks for it
     * again is common, and the copy is small next to the artwork it came from. */
    (void)c;
}

/* GetPicture(picID) is GetResource('PICT', picID) with the type implied. It is
 * how an editor fetches its artwork, so a plug-in that cannot get a picture
 * cannot work out how big its window should be. */
static void f_GetPicture(cfm *c)
{
    uint32_t save = c->m->r[4];
    c->m->r[4] = c->m->r[3];                     /* the id becomes argument 2 */
    c->m->r[3] = 0x50494354u;                    /* 'PICT'                    */
    f_GetResource(c);
    c->m->r[4] = save;
}

/* ------------------------------------------------------------------- misc */

/* On a PowerPC-only system a routine descriptor is not needed: the "UPP" the
 * caller wants is the TVector it already has. */
static void f_NewRoutineDescriptor(cfm *c) { ret(c, arg(c, 0)); }

static void f_ExitToShell(cfm *c)
{
    snprintf(c->m->err, sizeof c->m->err, "the guest called ExitToShell");
    c->m->stopped = 1;
}

/* The picture behind a PicHandle, as bytes we can decode. The guest may hand us
 * a handle we made, or a pointer straight at picture data. */
static const uint8_t *pic_bytes(cfm *c, uint32_t pichandle, uint32_t *len)
{
    cfm_handle *hd = handle_find(c, pichandle);
    int i;

    if (hd && hd->size) {
        /* It came from the resource fork, so the original bytes are still here
         * and there is no need to read them back out of guest memory. */
        uint32_t size;
        const uint8_t *b = res_find(c, hd->type, hd->id, &size);
        if (b && size == hd->size) { *len = size; return b; }
    }
    /* Not one of ours. Look for a resource whose data pointer the guest is
     * holding, which is what happens after HLock and a dereference. */
    for (i = 0; i < MAX_HANDLES; i++)
        if (c->handles[i].used && c->handles[i].data == pichandle) {
            uint32_t size;
            const uint8_t *b = res_find(c, c->handles[i].type, c->handles[i].id, &size);
            if (b) { *len = size; return b; }
        }
    return NULL;
}

/* DrawPicture(PicHandle, const Rect *dstRect) -- decode the picture and scale it
 * into the destination, nearest-neighbour, as QuickDraw would. */
static void f_DrawPicture(cfm *c)
{
    touch_port(c);
    uint32_t len = 0;
    const uint8_t *bytes = pic_bytes(c, arg(c, 0), &len);
    uint32_t *px = NULL;
    int pw = 0, ph = 0, y, x;
    char err[192] = "";
    rect r;

    if (!arg(c, 1)) return;
    r = g_rect(c, arg(c, 1));

    if (!bytes || !pict_decode(bytes, len, &px, &pw, &ph, err, sizeof err)) {
        /* Leaving the destination as it was would read as corruption rather than
         * a missing image, so clear it and record why, once. */
        for (y = r.t; y < r.b; y++)
            for (x = r.l; x < r.r; x++) port_put(c, x, y, c->back);
        if (!c->pict_failures++) {
            snprintf(c->pict_err, sizeof c->pict_err, "%s",
                     bytes ? err : "the picture is not in the resource fork");
            /* Say it once. A picture that will not decode leaves a blank area in
             * the editor, and a blank area with no explanation is the kind of
             * thing that gets blamed on the plug-in. */
            fprintf(stderr, "peload: a picture would not decode -- %s\n",
                    c->pict_err);
        }
        return;
    }

    for (y = r.t; y < r.b; y++) {
        int sy = (r.b > r.t) ? (y - r.t) * ph / (r.b - r.t) : 0;
        if (sy < 0 || sy >= ph) continue;
        for (x = r.l; x < r.r; x++) {
            int sx = (r.r > r.l) ? (x - r.l) * pw / (r.r - r.l) : 0;
            if (sx < 0 || sx >= pw) continue;
            /* The canvas is 0xAARRGGBB and a GWorld pixel is the same layout. */
            port_put(c, x, y, px[(long)sy * pw + sx] & 0x00FFFFFFu);
        }
    }
    free(px);
}

/* GetPictInfo(PicHandle, PictInfo *, verb, colors, method, bank). Only the
 * picture's size is ever wanted, and that much is real. */
static void f_GetPictInfo(cfm *c)
{
    uint32_t len = 0;
    const uint8_t *bytes = pic_bytes(c, arg(c, 0), &len);
    uint32_t out = arg(c, 1);
    int w = 0, h = 0;

    if (!bytes || !out || !pict_size(bytes, len, &w, &h)) { reterr(c, paramErr); return; }
    /* PictInfo is version(0) uniqueColors(2) thePalette(6) theColorTable(10)
     * hRes(14) vRes(18) depth(22) sourceRect(24). The fields are 2-byte aligned,
     * as everything on a 68k-derived ABI is, so sourceRect really is at 24 and
     * not at the 16 that natural alignment would give. */
    s16(c, out, 0);
    s32(c, out + 2, 0);
    s32(c, out + 6, 0); s32(c, out + 10, 0);
    s32(c, out + 14, 72 << 16); s32(c, out + 18, 72 << 16);
    s16(c, out + 22, 32);
    s16(c, out + 24, 0); s16(c, out + 26, 0);
    s16(c, out + 28, (uint16_t)h); s16(c, out + 30, (uint16_t)w);
    reterr(c, noErr);
}

/* ---------------------------------------------------------------- DragLib */

/* Drag and drop has nothing to do with producing audio, and a plug-in that asks
 * about a drag when none is happening should be told there is none. */
static void f_CountDragItems(cfm *c)
{ if (arg(c, 1)) s16(c, arg(c, 1), 0); reterr(c, noErr); }
static void f_GetDragItemReferenceNumber(cfm *c) { reterr(c, paramErr); }
static void f_GetFlavorData(cfm *c)              { reterr(c, paramErr); }
static void f_GetFlavorDataSize(cfm *c)          { reterr(c, paramErr); }
static void f_InstallReceiveHandler(cfm *c)      { reterr(c, noErr); }
static void f_RemoveReceiveHandler(cfm *c)       { reterr(c, noErr); }

/* ------------------------------------------------------------ the bindings */

typedef struct { const char *name; void (*fn)(cfm *); } binding;

static const binding g_bindings[] = {
    /* memory */
    { "NewPtr", f_NewPtr }, { "NewPtrClear", f_NewPtrClear },
    { "DisposePtr", f_DisposePtr }, { "HLock", f_HLock }, { "HUnlock", f_HUnlock },
    { "StuffHex", f_StuffHex },
    { "NewHandle", f_NewHandle }, { "NewHandleClear", f_NewHandleClear },
    { "DisposeHandle", f_DisposeHandle }, { "GetHandleSize", f_GetHandleSize },
    { "SetHandleSize", f_SetHandleSize }, { "MemError", f_MemError },
    { "MaxApplZone", f_MaxApplZone }, { "MoreMasters", f_MoreMasters },
    { "MoreMasterPointers", f_MoreMasters }, { "SetGrowZone", f_SetGrowZone },
    { "FreeMem", f_FreeMem }, { "MaxMem", f_MaxMem }, { "CompactMem", f_CompactMem },
    /* Toolbox initialisation */
    { "InitGraf", f_InitGraf }, { "InitFonts", f_InitFonts },
    { "InitWindows", f_InitWindows }, { "InitMenus", f_InitMenus },
    { "TEInit", f_TEInit }, { "InitDialogs", f_InitDialogs },
    { "FlushEvents", f_FlushEvents }, { "Gestalt", f_Gestalt },
    /* math */
    { "sin", f_sin }, { "cos", f_cos }, { "sinh", f_sinh }, { "exp", f_exp },
    { "log", f_log }, { "log10", f_log10 }, { "floor", f_floor },
    { "fabs", f_fabs }, { "pow", f_pow }, { "fmod", f_fmod },
    { "atan2", f_atan2 }, { "num2dec", f_num2dec }, { "sqrt", f_sqrt },
    { "tan", f_tan }, { "atan", f_atan }, { "acos", f_acos },
    { "asin", f_asin }, { "nan", f_nan }, { "dec2num", f_dec2num },
    { "__fpclassifyf", f___fpclassifyf }, { "__fpclassify", f___fpclassifyf },
    /* time */
    { "TickCount", f_TickCount }, { "GetDblTime", f_GetDblTime },
    { "GetDateTime", f_GetDateTime }, { "Delay", f_Delay },
    /* events: none are ever pending, but the host must get its chance to run */
    { "GetNextEvent", f_GetNextEvent }, { "WaitNextEvent", f_WaitNextEvent },
    { "SystemTask", f_SystemTask },
    { "WaitMouseUp", f_WaitMouseUp }, { "WaitMouseMoved", f_WaitMouseMoved },
    /* window invalidation: the host redraws everything anyway */
    { "InvalRect", f_noop_ok }, { "ValidRect", f_noop_ok },
    { "InvalRgn", f_noop_ok }, { "ValidRgn", f_noop_ok },
    { "InvalWindowRect", f_noop_ok }, { "InvalWindowRgn", f_noop_ok },
    /* Gestalt additions and CFM introspection */
    { "NewGestaltValue", f_NewGestaltValue },
    { "ReplaceGestaltValue", f_ReplaceGestaltValue },
    { "CountSymbols", f_CountSymbols }, { "FindSymbol", f_FindSymbol },
    { "DisposeRoutineDescriptor", f_DisposeRoutineDescriptor },
    /* Internet Config */
    { "ICStart", f_ICStart }, { "ICStop", f_ICStop },
    { "ICLaunchURL", f_ICLaunchURL }, { "ICFindConfigFile", f_ICFindConfigFile },
    /* TextEdit: single-line fields */
    { "TENew", f_TENew }, { "TEDispose", f_TEDispose },
    { "TEActivate", f_TEActivate }, { "TEDeactivate", f_TEDeactivate },
    { "TEIdle", f_TEIdle }, { "TESetText", f_TESetText },
    { "TEGetText", f_TEGetText }, { "TEKey", f_TEKey },
    { "TESetSelect", f_TESetSelect }, { "TEDelete", f_TEDelete },
    { "TESetAlignment", f_TESetAlignment }, { "TEClick", f_TEClick },
    { "TEUpdate", f_TEUpdate },
    /* files */
    { "FSFindFolder", f_FSFindFolder },
    { "FSGetCatalogInfo", f_FSGetCatalogInfo },
    { "FSMakeFSSpec", f_FSMakeFSSpec }, { "FSpOpenDF", f_FSpOpenDF },
    { "FSpCreate", f_FSpCreate }, { "FSRead", f_FSRead }, { "FSClose", f_FSClose },
    { "PBGetCatInfoSync", f_PBGetCatInfoSync },
    /* graphics */
    { "NewGWorld", f_NewGWorld }, { "DisposeGWorld", f_DisposeGWorld },
    { "GetGWorld", f_GetGWorld }, { "SetGWorld", f_SetGWorld },
    { "GetGWorldPixMap", f_GetGWorldPixMap },
    { "LockPixels", f_LockPixels }, { "UnlockPixels", f_UnlockPixels },
    { "GetPort", f_GetPort }, { "SetPort", f_SetPort },
    { "CopyBits", f_CopyBits }, { "CopyMask", f_CopyMask },
    { "FillRect", f_FillRect }, { "MoveTo", f_MoveTo }, { "LineTo", f_LineTo },
    { "Move", f_Move }, { "Line", f_Line },
    { "PaintRect", f_PaintRect }, { "EraseRect", f_EraseRect },
    { "FrameRect", f_FrameRect }, { "InvertRect", f_InvertRect },
    { "ScrollRect", f_ScrollRect },
    { "PenSize", f_PenSize }, { "PenNormal", f_PenNormal },
    { "GetPen", f_GetPen },
    { "ForeColor", f_ForeColor }, { "BackColor", f_BackColor },
    { "RGBForeColor", f_RGBForeColor }, { "RGBBackColor", f_RGBBackColor },
    { "GetForeColor", f_GetForeColor }, { "GetBackColor", f_GetBackColor },
    { "PenMode", f_PenMode }, { "TextMode", f_TextMode },
    { "GetPenState", f_GetPenState }, { "SetPenState", f_SetPenState },
    { "DrawPicture", f_DrawPicture }, { "GetPictInfo", f_GetPictInfo },
    /* text */
    { "DrawString", f_DrawString }, { "DrawText", f_DrawText },
    { "StringWidth", f_StringWidth }, { "TextWidth", f_TextWidth },
    { "CharWidth", f_CharWidth },
    { "TextFont", f_TextFont }, { "TextFace", f_TextFace },
    { "TextSize", f_TextSize }, { "GetFontInfo", f_GetFontInfo },
    { "c2pstr", f_c2pstr }, { "p2cstr", f_p2cstr },
    /* rects */
    { "SetRect", f_SetRect }, { "InsetRect", f_InsetRect },
    { "OffsetRect", f_OffsetRect }, { "SectRect", f_SectRect },
    { "UnionRect", f_UnionRect }, { "PtInRect", f_PtInRect },
    { "EqualRect", f_EqualRect }, { "EmptyRect", f_EmptyRect },
    { "Pt2Rect", f_Pt2Rect },
    /* regions and clip */
    { "NewRgn", f_NewRgn }, { "DisposeRgn", f_DisposeRgn },
    { "RectRgn", f_RectRgn }, { "SetRectRgn", f_SetRectRgn },
    { "CopyRgn", f_CopyRgn }, { "SetEmptyRgn", f_SetEmptyRgn },
    { "EmptyRgn", f_EmptyRgn }, { "OffsetRgn", f_OffsetRgn },
    { "InsetRgn", f_InsetRgn }, { "SectRgn", f_SectRgn },
    { "UnionRgn", f_UnionRgn }, { "PtInRgn", f_PtInRgn },
    { "EqualRgn", f_EqualRgn }, { "EraseRgn", f_EraseRgn },
    { "PaintRgn", f_PaintRgn }, { "FillRgn", f_FillRgn },
    { "InvertRgn", f_InvertRgn }, { "FrameRgn", f_FrameRgn },
    { "GetClip", f_GetClip }, { "SetClip", f_SetClip },
    { "ClipRect", f_ClipRect },
    /* port origin */
    { "SetOrigin", f_SetOrigin },
    { "GlobalToLocal", f_GlobalToLocal }, { "LocalToGlobal", f_LocalToGlobal },
    /* cursor and input */
    { "InitCursor", f_InitCursor }, { "SetCursor", f_SetCursor },
    { "GetCursor", f_GetCursor }, { "Button", f_Button },
    { "GetMouse", f_GetMouse }, { "GetKeys", f_GetKeys },
    { "StillDown", f_StillDown },
    /* resources */
    { "GetResource", f_GetResource }, { "ReleaseResource", f_ReleaseResource },
    { "GetPicture", f_GetPicture },
    /* misc */
    { "NewRoutineDescriptor", f_NewRoutineDescriptor },
    { "ExitToShell", f_ExitToShell },
    /* DragLib */
    { "CountDragItems", f_CountDragItems },
    { "GetDragItemReferenceNumber", f_GetDragItemReferenceNumber },
    { "GetFlavorData", f_GetFlavorData },
    { "GetFlavorDataSize", f_GetFlavorDataSize },
    { "InstallReceiveHandler", f_InstallReceiveHandler },
    { "RemoveReceiveHandler", f_RemoveReceiveHandler },
    { NULL, NULL }
};

/* -------------------------------------------------------------- the shim */

cfm *cfm_new(ppc *m, pef *p, const char *support_dir)
{
    cfm *c = calloc(1, sizeof *c);
    uint32_t i;

    if (!c) return NULL;
    c->m = m;
    c->p = p;
    clock_gettime(CLOCK_MONOTONIC, &c->t0);
    c->fore = 0x000000;
    c->back = 0xFFFFFF;
    c->pen_w = 1; c->pen_h = 1;
    c->pen_mode = 8;                           /* patCopy */
    c->tx_size = 12;
    c->am_slot = -1;          /* no audioMaster until one is asked for */

    /* The heap starts after the TVectors and runs up to the stack, leaving the
     * stack a generous amount of room to grow down into. */
    c->heap_next = PEF_HEAP;
    c->heap_end  = PEF_STACK - 0x00400000u;

    if (support_dir && *support_dir) {
        mkdir(support_dir, 0700);
        snprintf(c->dirs[0], sizeof c->dirs[0], "%s", support_dir);
        c->ndirs = 1;
    }

    c->fn = calloc(p->nimports ? p->nimports : 1, sizeof *c->fn);
    if (!c->fn) { free(c); return NULL; }
    c->nfn = p->nimports;

    for (i = 0; i < p->nimports; i++) {
        const binding *b;
        for (b = g_bindings; b->name; b++)
            if (!strcmp(b->name, p->imports[i].name)) { c->fn[i] = b->fn; break; }
        if (!c->fn[i]) {
            c->nunbound++;
            if (strlen(c->unbound) + strlen(p->imports[i].name) + 3 <
                sizeof c->unbound) {
                if (c->unbound[0]) strcat(c->unbound, ", ");
                strcat(c->unbound, p->imports[i].name);
            }
        }
    }

    m->host = c;
    m->hostcall = cfm_hostcall;
    return c;
}

void cfm_free(cfm *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < MAX_FILES; i++) if (c->files[i].used) fclose(c->files[i].fp);
    free(c->shadow);
    free(c->fn);
    free(c);
}

void cfm_hostcall(ppc *m, uint32_t index)
{
    cfm *c = (cfm *)m->host;
    int slot;

    if (!c) return;

    /* A trap past the imports is one of ours -- a callback we handed the guest. */
    slot = pef_host_slot(c->p, index);
    if (slot >= 0) {
        if (slot == c->am_slot) { audio_master(c); return; }
        snprintf(m->err, sizeof m->err,
                 "the guest called host callback slot %d, which nothing claimed",
                 slot);
        m->stopped = 1;
        return;
    }

    /* CFMTRACE names every call the guest makes, which is the only practical way
     * to find out what a twenty-year-old editor is actually asking for. */
    if (index < c->nfn && getenv("CFMTRACE"))
        fprintf(stderr, "cfm: %s\n", c->p->imports[index].name);
    if (index < c->nfn && c->fn[index]) { c->fn[index](c); return; }

    /* An unimplemented import. Returning an error rather than success is the
     * lesser wrong: a caller that checks will take its failure path, whereas a
     * fabricated success leaves it using a buffer nobody filled in. */
    if (index < c->nfn) {
        snprintf(c->last_stub, sizeof c->last_stub, "%s", c->p->imports[index].name);
        c->stub_calls++;
        m->r[3] = (uint32_t)(int32_t)paramErr;
        return;
    }
    snprintf(m->err, sizeof m->err,
             "the guest called host trap %u, which is past the %u imports it "
             "declared", index, c->nfn);
    m->stopped = 1;
}

int cfm_unbound(cfm *c, char *buf, size_t n)
{
    if (buf && n) snprintf(buf, n, "%s", c->unbound);
    return c->nunbound;
}

int cfm_stub_calls(cfm *c, const char **last)
{
    if (last) *last = c->last_stub[0] ? c->last_stub : NULL;
    return c->stub_calls;
}

void cfm_set_editor_size(cfm *c, int w, int h)
{
    if (!c) return;
    c->ed_w = w;
    c->ed_h = h;
}

uint32_t cfm_editor_window(cfm *c, int w, int h)
{
    int i, slot = -1;

    if (!c || w <= 0 || h <= 0 || (long)w * h > GW_MAX_PIXELS) return 0;
    if (c->ed_window) return c->ed_window;
    for (i = 0; i < MAX_GWORLDS; i++) if (!c->gw[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    c->gw[slot].used = 1;
    c->gw[slot].w = w;
    c->gw[slot].h = h;
    c->gw[slot].depth = 32;
    gw_build(c, &c->gw[slot]);
    if (!c->gw[slot].pixels) { c->gw[slot].used = 0; return 0; }
    c->ed_window = c->gw[slot].guest;
    c->ed_w = w;
    c->ed_h = h;
    /* The plug-in will SetPort to it, but make it current now so anything drawn
     * before that lands somewhere sensible rather than nowhere. */
    c->cur_port = c->ed_window;
    return c->ed_window;
}

void cfm_set_mouse(cfm *c, int x, int y, int down)
{
    if (!c) return;
    c->mouse_x = x;
    c->mouse_y = y;
    c->mouse_down = down;
}

const uint32_t *cfm_gworld_pixels(cfm *c, int *w, int *h)
{
    cfm_gworld *g = NULL;
    int i, y, x;

    /* When the editor's size is known, the offscreen that matches it is the
     * window's backing store. Preferring the largest instead picks whichever
     * piece of artwork is biggest -- and a knob filmstrip, being one frame wide
     * and thirty tall, is easily larger in area than the window it draws into. */
    /* The window handed to the plug-in is the obvious answer, but only if the
     * plug-in actually drew there. One that composes elsewhere and blits across
     * on its own schedule would otherwise show as a black rectangle, so the
     * choice is the most recently drawn offscreen of the right size -- the window
     * included, and preferred when it ties. */
    if (c->ed_w > 0 && c->ed_h > 0) {
        unsigned long best = 0;
        for (i = 0; i < MAX_GWORLDS; i++) {
            if (!c->gw[i].used || !c->gw[i].touched) continue;
            if (c->gw[i].w != c->ed_w || c->gw[i].h != c->ed_h) continue;
            if (c->gw[i].touched > best ||
                (c->gw[i].touched == best && c->gw[i].guest == c->ed_window)) {
                best = c->gw[i].touched;
                g = &c->gw[i];
            }
        }
    }
    if (!g && c->ed_window)
        for (i = 0; i < MAX_GWORLDS; i++)
            if (c->gw[i].used && c->gw[i].guest == c->ed_window) { g = &c->gw[i]; break; }
    if (!g && c->ed_w > 0 && c->ed_h > 0) {
        for (i = 0; i < MAX_GWORLDS; i++)
            if (c->gw[i].used && c->gw[i].w == c->ed_w && c->gw[i].h == c->ed_h)
                { g = &c->gw[i]; break; }
        /* Failing an exact match, the one closest in shape to the window. */
        if (!g) {
            long best = -1;
            for (i = 0; i < MAX_GWORLDS; i++) {
                long d;
                if (!c->gw[i].used) continue;
                d = labs((long)c->gw[i].w - c->ed_w) + labs((long)c->gw[i].h - c->ed_h);
                if (best < 0 || d < best) { best = d; g = &c->gw[i]; }
            }
        }
    }
    /* With no editor size to go on, the largest is the best guess available. */
    if (!g)
        for (i = 0; i < MAX_GWORLDS; i++)
            if (c->gw[i].used && (!g || c->gw[i].w * c->gw[i].h > g->w * g->h))
                g = &c->gw[i];
    if (!g) return NULL;

    if (c->shadow_w != g->w || c->shadow_h != g->h) {
        free(c->shadow);
        c->shadow = malloc((size_t)g->w * (size_t)g->h * 4);
        if (!c->shadow) { c->shadow_w = c->shadow_h = 0; return NULL; }
        c->shadow_w = g->w; c->shadow_h = g->h;
    }
    for (y = 0; y < g->h; y++)
        for (x = 0; x < g->w; x++)
            c->shadow[y * g->w + x] =
                g32(c, g->pixels + (uint32_t)y * g->rowbytes + (uint32_t)x * 4);
    if (w) *w = g->w;
    if (h) *h = g->h;
    return c->shadow;
}
