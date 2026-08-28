/* QuickDraw pictures.
 *
 * A PICT is a recording of drawing calls, not an image format, so decoding one in
 * general means implementing QuickDraw. That is not what this is for: the
 * pictures a VST plug-in carries are its artwork, exported from a paint program,
 * and those are a header followed by a single bitmap opcode. This handles that
 * shape -- the bitmap opcodes and the few header opcodes that precede them -- and
 * says so plainly when it meets a picture that draws with anything else, rather
 * than returning a partial image that looks like a decoding bug.
 *
 * The two bitmap opcodes that matter:
 *
 *   PackBitsRect  (0x0098)  an indexed image with a colour table
 *   DirectBitsRect(0x009A)  a direct-colour image, 16 or 32 bits per pixel
 *
 * Rows are compressed with PackBits unless rowBytes is under 8, in which case
 * they are stored plainly -- a rule that is easy to miss and produces garbage on
 * narrow images. For 32-bit pixels the compression is per component: a row is
 * stored as all the red bytes, then all the green, then all the blue, which is
 * why the unpacking and the pixel assembly are separate steps here.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pict.h"

typedef struct {
    const uint8_t *d;
    uint32_t       n, at;
    int            bad;          /* set once we have read past the end */
    char          *err;
    int            errlen;
} rd;

static void fail(rd *r, const char *fmt, ...)
{
    va_list ap;
    if (r->bad) return;          /* keep the first reason, not the last */
    r->bad = 1;
    if (!r->err || r->errlen <= 0) return;
    va_start(ap, fmt);
    vsnprintf(r->err, (size_t)r->errlen, fmt, ap);
    va_end(ap);
}

static uint8_t u8(rd *r)
{
    if (r->at + 1 > r->n) { fail(r, "the picture ends mid-value"); return 0; }
    return r->d[r->at++];
}

static uint16_t u16(rd *r)
{
    uint16_t v;
    if (r->at + 2 > r->n) { fail(r, "the picture ends mid-value"); return 0; }
    v = (uint16_t)((r->d[r->at] << 8) | r->d[r->at + 1]);
    r->at += 2;
    return v;
}

static void skip(rd *r, uint32_t n)
{
    if (r->at + n > r->n) { fail(r, "the picture ends mid-structure"); r->at = r->n; }
    else r->at += n;
}

typedef struct { int t, l, b, r; } rect;

static rect rd_rect(rd *r)
{
    rect q;
    q.t = (int16_t)u16(r); q.l = (int16_t)u16(r);
    q.b = (int16_t)u16(r); q.r = (int16_t)u16(r);
    return q;
}

/* ------------------------------------------------------------------ PackBits */

/* One PackBits run-length stream into `out`. Returns how many bytes it produced,
 * or -1 if the stream is malformed or would overrun. */
static long unpackbits(const uint8_t *src, uint32_t srclen, uint8_t *out,
                       uint32_t outlen)
{
    uint32_t si = 0, oi = 0;

    while (si < srclen && oi < outlen) {
        int flag = (int8_t)src[si++];
        if (flag >= 0) {                        /* flag+1 literal bytes */
            uint32_t n = (uint32_t)flag + 1;
            if (si + n > srclen || oi + n > outlen) return -1;
            memcpy(out + oi, src + si, n);
            si += n; oi += n;
        } else if (flag != -128) {              /* one byte, 1-flag times */
            uint32_t n = (uint32_t)(1 - flag);
            if (si >= srclen || oi + n > outlen) return -1;
            memset(out + oi, src[si++], n);
            oi += n;
        }
        /* -128 is a no-op, by the format's definition. */
    }
    return (long)oi;
}

/* ------------------------------------------------------------ the bitmap ops */

typedef struct {
    uint32_t *px;                /* the canvas, 0xAARRGGBB */
    int       w, h;
} canvas;

static void put(canvas *c, int x, int y, uint32_t argb)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->px[(long)y * c->w + x] = argb;
}

/* PackBits over 16-bit units rather than bytes, which is what packType 3 means.
 * The flag byte counts *words*: a literal run copies flag+1 of them and a repeat
 * copies one word 1-flag times. Unpacking this a byte at a time yields a row that
 * is the wrong length and half the wrong values, and since the row's byte count
 * is framed separately the damage stays inside the image -- it shows up as a
 * picture that decodes to noise rather than as an error. */
static long unpackbits16(const uint8_t *src, uint32_t srclen, uint8_t *out,
                         uint32_t outlen)
{
    uint32_t si = 0, oi = 0;

    while (si < srclen && oi < outlen) {
        int flag = (int8_t)src[si++];
        if (flag >= 0) {
            uint32_t n = ((uint32_t)flag + 1) * 2;
            if (si + n > srclen || oi + n > outlen) return -1;
            memcpy(out + oi, src + si, n);
            si += n; oi += n;
        } else if (flag != -128) {
            uint32_t times = (uint32_t)(1 - flag), k;
            if (si + 2 > srclen || oi + times * 2 > outlen) return -1;
            for (k = 0; k < times; k++) {
                out[oi + k * 2]     = src[si];
                out[oi + k * 2 + 1] = src[si + 1];
            }
            si += 2;
            oi += times * 2;
        }
    }
    return (long)oi;
}

/* Read one row of pixel data, whether or not it is compressed. The cursor is
 * advanced by the row's framed length whatever happens to the unpacking, so a row
 * this cannot make sense of costs that row and not the rest of the picture. */
static int row_bytes_in(rd *r, int rowbytes, uint8_t *row, uint32_t rowlen,
                        int wordwise, int *produced)
{
    uint32_t n;
    long got;

    if (produced) *produced = 0;
    if (rowbytes < 8) {                         /* stored plainly */
        if (r->at + (uint32_t)rowbytes > r->n) return 0;
        memcpy(row, r->d + r->at, (uint32_t)rowbytes);
        r->at += (uint32_t)rowbytes;
        if (produced) *produced = rowbytes;
        return 1;
    }
    /* The byte count is a byte for narrow rows and a word for wide ones. */
    n = rowbytes > 250 ? u16(r) : u8(r);
    if (r->bad || r->at + n > r->n) return 0;
    got = wordwise ? unpackbits16(r->d + r->at, n, row, rowlen)
                   : unpackbits(r->d + r->at, n, row, rowlen);
    r->at += n;
    if (got > 0 && produced) *produced = (int)got;
    return got > 0;
}

/* Turn one unpacked row into pixels. `depth` and `cmp` describe the source, and
 * [x0, x1) is the part of the row the source rectangle selects. */
static void row_to_pixels(canvas *c, const uint8_t *row, int rowbytes,
                          int depth, int cmp, int width, int y, int dx, int dy,
                          int x0, int x1, const uint32_t *clut, int clutn)
{
    int x;

    if (x0 < 0) x0 = 0;
    if (x1 > width) x1 = width;

    switch (depth) {
    case 1: case 2: case 4: case 8: {
        int per = 8 / depth, mask = (1 << depth) - 1;
        for (x = x0; x < x1; x++) {
            int byte = x / per, shift = (per - 1 - (x % per)) * depth;
            int idx;
            if (byte >= rowbytes) break;
            idx = (row[byte] >> shift) & mask;
            put(c, dx + x, dy + y,
                idx < clutn ? clut[idx] : 0xFF000000u);
        }
        break; }
    case 16:
        for (x = x0; x < x1; x++) {
            unsigned v;
            int rr, gg, bb;
            if (x * 2 + 1 >= rowbytes) break;
            v = (unsigned)((row[x * 2] << 8) | row[x * 2 + 1]);
            /* 5 bits per component, in the low 15 -- scale to 8 bits so that
             * full-scale stays full-scale. */
            rr = ((v >> 10) & 31) * 255 / 31;
            gg = ((v >> 5)  & 31) * 255 / 31;
            bb = ( v        & 31) * 255 / 31;
            put(c, dx + x, dy + y,
                0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb);
        }
        break;
    case 32:
        /* Components are stored in planes across the row, not interleaved. With
         * three components the row is RRR...GGG...BBB; with four it is
         * AAA...RRR...GGG...BBB. */
        for (x = x0; x < x1; x++) {
            int rr, gg, bb;
            if (cmp == 3) {
                if (x + 2 * width >= rowbytes) break;
                rr = row[x]; gg = row[x + width]; bb = row[x + 2 * width];
            } else {
                if (x + 3 * width >= rowbytes) break;
                rr = row[x + width]; gg = row[x + 2 * width]; bb = row[x + 3 * width];
            }
            put(c, dx + x, dy + y,
                0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb);
        }
        break;
    default:
        break;
    }
}

/* A colour table, as PackBitsRect carries one. */
static int read_clut(rd *r, uint32_t *clut, int max)
{
    uint16_t flags;
    int n, i;

    skip(r, 4);                                  /* ctSeed  */
    flags = u16(r);
    n = (int)(int16_t)u16(r) + 1;                /* ctSize is the count minus 1 */
    if (n < 0 || n > max) { fail(r, "a colour table with %d entries", n); return 0; }
    for (i = 0; i < n; i++) {
        int idx = (int)u16(r);
        int rr = u16(r) >> 8, gg = u16(r) >> 8, bb = u16(r) >> 8;
        /* With the device flag set the index field is ignored and entries are in
         * order; otherwise the entry names its own slot. */
        if (!(flags & 0x8000)) { if (idx < 0 || idx >= max) idx = i; }
        else idx = i;
        clut[idx] = 0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) |
                    (uint32_t)bb;
    }
    return n;
}

/* PackBitsRect / DirectBitsRect. `direct` says which, and decides both whether a
 * base address precedes the PixMap and whether a colour table follows it. */
static void do_bits(rd *r, canvas *c, int direct, int with_region, rect frame)
{
    uint32_t clut[256];
    int clutn = 0, rowbytes, depth = 1, cmp = 1, packtype = 0;
    rect bounds, src, dst;
    uint8_t *row;
    uint32_t rowlen;
    int y, pix_is_pixmap, dx, dy, x0, x1, y0, y1, wordwise, rowgot;

    if (direct) skip(r, 4);                      /* baseAddr, meaningless here */
    rowbytes = (int)u16(r);
    pix_is_pixmap = (rowbytes & 0x8000) != 0 || direct;
    rowbytes &= 0x3FFF;
    bounds = rd_rect(r);

    if (pix_is_pixmap) {
        skip(r, 2);                              /* pmVersion */
        packtype = (int)u16(r);
        skip(r, 4);                              /* packSize  */
        skip(r, 8);                              /* hRes, vRes */
        skip(r, 2);                              /* pixelType */
        depth = (int)u16(r);
        cmp = (int)u16(r);
        skip(r, 2);                              /* cmpSize   */
        skip(r, 4);                              /* planeBytes */
        skip(r, 4);                              /* pmTable   */
        skip(r, 4);                              /* pmReserved */
    } else {
        depth = 1; cmp = 1;
    }
    if (!direct) clutn = read_clut(r, clut, 256);

    src = rd_rect(r);
    dst = rd_rect(r);
    skip(r, 2);                                  /* transfer mode */
    if (with_region) { uint16_t rn = u16(r); if (rn >= 2) skip(r, rn - 2u); }
    if (r->bad) return;

    if (rowbytes <= 0 || bounds.r <= bounds.l || bounds.b <= bounds.t) {
        fail(r, "a bitmap with an empty %dx%d bounds",
             bounds.r - bounds.l, bounds.b - bounds.t);
        return;
    }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 &&
        depth != 16 && depth != 32) {
        fail(r, "a bitmap %d bits deep", depth);
        return;
    }
    /* packType 1 means the rows are not compressed even though they are wide.
     * packType 3 compresses 16-bit words rather than bytes; 4 compresses bytes,
     * with the components stored in planes. Getting this wrong does not fail --
     * it silently produces a different picture. */
    if (packtype == 1) rowbytes = 0;
    wordwise = (packtype == 3) || (packtype == 0 && depth == 16);

    /* A 32-bit image with three components stores only three bytes per pixel, so
     * the row is narrower than rowBytes would suggest for four. */
    rowlen = (uint32_t)(bounds.r - bounds.l) * 4u + 32u;
    if ((uint32_t)rowbytes + 32u > rowlen) rowlen = (uint32_t)rowbytes + 32u;
    if (!(row = calloc(rowlen, 1))) { fail(r, "out of memory"); return; }

    /* The source rectangle says which part of the bitmap to draw and the
     * destination where it lands; both are in the picture's own coordinates, so
     * the frame's origin comes off to give a canvas position. */
    dx = dst.l + bounds.l - src.l - frame.l;
    dy = dst.t + bounds.t - src.t - frame.t;
    x0 = src.l - bounds.l; x1 = src.r - bounds.l;
    y0 = src.t - bounds.t; y1 = src.b - bounds.t;

    for (y = 0; y < bounds.b - bounds.t; y++) {
        int keep = rowbytes;
        if (packtype == 1) keep = 0;
        memset(row, 0, rowlen);
        rowgot = 0;
        if (!row_bytes_in(r, keep ? rowbytes : 0, row, rowlen, wordwise, &rowgot)) {
            if (packtype == 1) {
                /* Uncompressed: take rowBytes straight from the stream. */
                uint32_t take = (uint32_t)((bounds.r - bounds.l) *
                                           (depth == 32 ? cmp : 1));
                if (r->at + take > r->n) break;
                memcpy(row, r->d + r->at, take);
                r->at += take;
                rowgot = (int)take;
            } else if (r->bad || r->at >= r->n) {
                break;                           /* genuinely out of data */
            } else {
                /* The row's framed length has still been consumed, so the cursor
                 * is where the next row starts. Skipping this row keeps the rest
                 * of the picture -- and, more importantly, keeps the parser in
                 * step with the stream instead of reading pixels as opcodes. */
                continue;
            }
        }
        if (y < y0 || y >= y1) continue;      /* outside the source rectangle */
        /* Bound the conversion by what this row actually unpacked to, not by the
         * size of the buffer holding it: a short row would otherwise be read to
         * the buffer's end and drawn as however much of it happened to be
         * cleared. */
        row_to_pixels(c, row, rowgot, depth, cmp, bounds.r - bounds.l, y,
                      dx, dy, x0, x1, clut, clutn);
    }
    free(row);
    /* Suppress a trailing "ends mid-value" from a picture whose last row was
     * short: the image is already drawn. */
    r->bad = 0;
}

/* ------------------------------------------------------------------ the walk */

static int parse(rd *r, canvas *c, rect *frame, int decode)
{
    int v2;

    skip(r, 2);                                  /* picSize, unreliable */
    *frame = rd_rect(r);
    if (r->bad) return 0;
    if (frame->r <= frame->l || frame->b <= frame->t) {
        fail(r, "a picture frame of %dx%d", frame->r - frame->l,
             frame->b - frame->t);
        return 0;
    }
    if (!decode) return 1;

    /* Version 2 announces itself; version 1 goes straight to opcodes, and its
     * opcodes are single bytes rather than words. */
    v2 = 0;
    if (r->at + 2 <= r->n && r->d[r->at] == 0x00 && r->d[r->at + 1] == 0x11) {
        uint16_t ver;
        skip(r, 2);
        ver = u16(r);
        v2 = 1;
        if (ver == 0x0C00 || (r->at + 2 <= r->n &&
                              r->d[r->at] == 0x0C && r->d[r->at + 1] == 0x00)) {
            if (ver != 0x0C00) skip(r, 2);
            skip(r, 24);                         /* the extended header */
        }
    }

    for (;;) {
        uint32_t op;
        if (r->at >= r->n) return 1;             /* ran out; keep what we drew */
        if (v2) {
            if (r->at & 1) r->at++;              /* opcodes are word-aligned */
            if (r->at + 2 > r->n) return 1;
            op = u16(r);
        } else {
            op = u8(r);
        }
        if (r->bad) return 0;

        switch (op) {
        case 0x0000: break;                      /* NOP        */
        case 0x00FF: return 1;                   /* OpEndPic   */
        case 0x001E: break;                      /* DefHilite  */
        case 0x0001: {                           /* Clip       */
            uint16_t n = u16(r);
            if (n >= 2) skip(r, n - 2u); else skip(r, 8);
            break; }
        case 0x0098: do_bits(r, c, 0, 0, *frame); if (r->bad) return 0; break;
        case 0x0099: do_bits(r, c, 0, 1, *frame); if (r->bad) return 0; break;
        case 0x009A: do_bits(r, c, 1, 0, *frame); if (r->bad) return 0; break;
        case 0x009B: do_bits(r, c, 1, 1, *frame); if (r->bad) return 0; break;
        case 0x00A0: skip(r, 2); break;          /* ShortComment */
        case 0x00A1: { skip(r, 2); skip(r, u16(r)); break; }   /* LongComment */
        /* The state-setting opcodes, whose payloads are fixed sizes. Skipping
         * them is right: they affect drawing we do not do. */
        case 0x0003: case 0x0004: case 0x0005: case 0x0008:
        case 0x000D: case 0x0011: case 0x0015: case 0x0016:
        case 0x0018: case 0x0019: case 0x001A: case 0x001B:
        case 0x001C: case 0x001D: case 0x0020: case 0x0021:
            skip(r, op == 0x0020 ? 8u : (op == 0x0021 ? 4u :
                 (op == 0x001A || op == 0x001B || op == 0x001C ? 6u : 2u)));
            break;
        case 0x0002: case 0x0007: case 0x000A: case 0x000B:
        case 0x000C: case 0x000E: case 0x000F:
            skip(r, op == 0x0007 || op == 0x000B ? 4u : 8u);
            break;
        case 0x0009: skip(r, 8); break;          /* PnPat      */
        case 0x0010: skip(r, 8); break;          /* TxRatio    */
        default:
            /* Anything else is real drawing, and guessing its length would
             * desynchronise the stream. Stop and say what stopped us. */
            fail(r, "picture opcode 0x%04x is not implemented", op);
            return 0;
        }
    }
}

int pict_size(const uint8_t *data, uint32_t len, int *w, int *h)
{
    rd r;
    rect frame;
    char err[8];

    memset(&r, 0, sizeof r);
    r.d = data; r.n = len; r.err = err; r.errlen = (int)sizeof err;
    if (!parse(&r, NULL, &frame, 0)) return 0;
    if (w) *w = frame.r - frame.l;
    if (h) *h = frame.b - frame.t;
    return 1;
}

int pict_decode(const uint8_t *data, uint32_t len, uint32_t **pixels,
                int *w, int *h, char *err, int errlen)
{
    rd r;
    canvas c;
    rect frame;
    long i, n;

    if (err && errlen) err[0] = 0;
    memset(&r, 0, sizeof r);
    memset(&c, 0, sizeof c);
    r.d = data; r.n = len; r.err = err; r.errlen = errlen;

    /* A picture from a resource fork starts at the 512-byte header only when it
     * came from a file; a PICT resource has none. Tell them apart by whether the
     * frame that follows makes sense. */
    if (!parse(&r, NULL, &frame, 0)) {
        if (len > 512) {
            memset(&r, 0, sizeof r);
            r.d = data + 512; r.n = len - 512; r.err = err; r.errlen = errlen;
            if (!parse(&r, NULL, &frame, 0)) return 0;
            data += 512; len -= 512;
        } else {
            return 0;
        }
    }

    c.w = frame.r - frame.l;
    c.h = frame.b - frame.t;
    if ((long)c.w * c.h > 64L * 1024 * 1024) {
        if (err) snprintf(err, (size_t)errlen, "a picture of %dx%d", c.w, c.h);
        return 0;
    }
    n = (long)c.w * c.h;
    if (!(c.px = malloc((size_t)n * 4))) {
        if (err) snprintf(err, (size_t)errlen, "out of memory");
        return 0;
    }
    /* Opaque white, which is what an undrawn part of a picture's frame is. */
    for (i = 0; i < n; i++) c.px[i] = 0xFFFFFFFFu;

    memset(&r, 0, sizeof r);
    r.d = data; r.n = len; r.err = err; r.errlen = errlen;
    if (!parse(&r, &c, &frame, 1)) { free(c.px); return 0; }

    *pixels = c.px;
    if (w) *w = c.w;
    if (h) *h = c.h;
    return 1;
}
