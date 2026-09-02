/* A PNG reader, for the artwork a plugin loads through ImageIO.
 *
 * VSTGUI-era plugins keep their whole interface as PNG resources -- 551 of them
 * across this corpus -- and load each one with CGImageSourceCreateWithURL. Every
 * pixel of those editors comes through here, so there is no drawing them without
 * a decoder, and the background bitmap is what an editor sizes itself from: with
 * no image it reports 0x0 and is refused before it draws anything at all.
 *
 * Deflate is implemented here rather than linked, for the same reason the writer
 * beside it emits stored blocks: this host has no zlib dependency, and one
 * added for a plugin's artwork would have to be carried by every package.
 * Inflate is about two hundred lines and is checked against the corpus rather
 * than trusted -- tools/regress.py decodes every PNG the plug-ins carry and
 * compares the result with Python's own reader.
 *
 * Output is the framebuffer format the rest of this host uses: 32-bit BGRX, one
 * word per pixel, top row first, so an image can be handed to CGContextDrawImage
 * without a conversion in between.
 */
#ifndef PELOAD_PNG_IN_H
#define PELOAD_PNG_IN_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- inflate */

/* Canonical Huffman, counted by length -- the decoder walks lengths shortest
 * first and never builds a lookup table, which is slower per symbol and far
 * shorter to get right. */
typedef struct { uint16_t count[16]; uint16_t sym[288]; } pin_huff;

typedef struct {
    const uint8_t *in;
    size_t         n, pos;
    uint32_t       bits;      /* bit reservoir, LSB first */
    int            nbits;
    uint8_t       *out;
    size_t         cap, len;
    int            bad;
} pin_inf;

static uint32_t pin_bits(pin_inf *s, int need)
{
    uint32_t v;
    while (s->nbits < need) {
        if (s->pos >= s->n) { s->bad = 1; return 0; }
        s->bits |= (uint32_t)s->in[s->pos++] << s->nbits;
        s->nbits += 8;
    }
    v = s->bits & ((1u << need) - 1u);
    s->bits >>= need;
    s->nbits -= need;
    return v;
}

static void pin_build(pin_huff *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    int i, len;
    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (i = 0; i < n; i++) if (lengths[i]) h->sym[offs[lengths[i]]++] = (uint16_t)i;
}

static int pin_sym(pin_inf *s, const pin_huff *h)
{
    int code = 0, first = 0, index = 0, len, count;
    for (len = 1; len <= 15; len++) {
        code |= (int)pin_bits(s, 1);
        if (s->bad) return -1;
        count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->bad = 1;
    return -1;
}

static void pin_put(pin_inf *s, uint8_t b)
{
    if (s->len >= s->cap) { s->bad = 1; return; }
    s->out[s->len++] = b;
}

static const uint16_t pin_lbase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  pin_lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t pin_dbase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  pin_dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void pin_codes(pin_inf *s, const pin_huff *lit, const pin_huff *dist)
{
    for (;;) {
        int sym = pin_sym(s, lit);
        if (s->bad) return;
        if (sym < 256) { pin_put(s, (uint8_t)sym); }
        else if (sym == 256) return;                       /* end of block */
        else {
            int len, d;
            size_t from;
            sym -= 257;
            if (sym >= 29) { s->bad = 1; return; }
            len = pin_lbase[sym] + (int)pin_bits(s, pin_lext[sym]);
            d = pin_sym(s, dist);
            if (s->bad || d < 0 || d >= 30) { s->bad = 1; return; }
            {   size_t back = pin_dbase[d] + (size_t)pin_bits(s, pin_dext[d]);
                if (back > s->len) { s->bad = 1; return; }
                from = s->len - back; }
            while (len-- > 0 && !s->bad) pin_put(s, s->out[from++]);
        }
        if (s->bad) return;
    }
}

/* The fixed tables of RFC 1951 section 3.2.6, built once on first use. */
static void pin_fixed(pin_huff *lit, pin_huff *dist)
{
    uint8_t l[288];
    int i;
    for (i = 0;   i < 144; i++) l[i] = 8;
    for (i = 144; i < 256; i++) l[i] = 9;
    for (i = 256; i < 280; i++) l[i] = 7;
    for (i = 280; i < 288; i++) l[i] = 8;
    pin_build(lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    pin_build(dist, l, 30);
}

static int pin_dynamic(pin_inf *s, pin_huff *lit, pin_huff *dist)
{
    static const uint8_t order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    uint8_t lengths[288 + 32];
    pin_huff clen;
    int nlit, ndist, ncode, i = 0;

    nlit  = (int)pin_bits(s, 5) + 257;
    ndist = (int)pin_bits(s, 5) + 1;
    ncode = (int)pin_bits(s, 4) + 4;
    if (s->bad || nlit > 286 || ndist > 30) return 0;

    memset(lengths, 0, 19);
    for (i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)pin_bits(s, 3);
    pin_build(&clen, lengths, 19);

    for (i = 0; i < nlit + ndist; ) {
        int sym = pin_sym(s, &clen), rep;
        uint8_t v = 0;
        if (s->bad) return 0;
        if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
        if (sym == 16) {
            if (i == 0) return 0;
            v = lengths[i - 1];
            rep = 3 + (int)pin_bits(s, 2);
        } else if (sym == 17) rep = 3 + (int)pin_bits(s, 3);
        else                  rep = 11 + (int)pin_bits(s, 7);
        if (i + rep > nlit + ndist) return 0;
        while (rep-- > 0) lengths[i++] = v;
    }
    if (lengths[256] == 0) return 0;         /* no end-of-block code */
    pin_build(lit, lengths, nlit);
    pin_build(dist, lengths + nlit, ndist);
    return 1;
}

/* Inflate `in` into a buffer of exactly `outcap` bytes -- PNG says how large
 * that is, so nothing has to grow. Returns the number of bytes produced. */
static size_t pin_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t outcap)
{
    pin_inf s;
    int last;

    memset(&s, 0, sizeof s);
    s.in = in; s.n = n; s.out = out; s.cap = outcap;
    do {
        pin_huff lit, dist;
        int type;
        last = (int)pin_bits(&s, 1);
        type = (int)pin_bits(&s, 2);
        if (s.bad) break;
        if (type == 0) {                                   /* stored */
            uint32_t len;
            s.bits = 0; s.nbits = 0;                       /* to a byte boundary */
            if (s.pos + 4 > s.n) { s.bad = 1; break; }
            len = (uint32_t)s.in[s.pos] | ((uint32_t)s.in[s.pos + 1] << 8);
            s.pos += 4;                                    /* LEN then ~LEN */
            if (s.pos + len > s.n || s.len + len > s.cap) { s.bad = 1; break; }
            memcpy(s.out + s.len, s.in + s.pos, len);
            s.pos += len; s.len += len;
        } else if (type == 1) {
            pin_fixed(&lit, &dist);
            pin_codes(&s, &lit, &dist);
        } else if (type == 2) {
            if (!pin_dynamic(&s, &lit, &dist)) { s.bad = 1; break; }
            pin_codes(&s, &lit, &dist);
        } else { s.bad = 1; break; }
    } while (!last && !s.bad);

    return s.bad ? 0 : s.len;
}

/* ----------------------------------------------------------------- PNG */

static int pin_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* One pass of scanlines, in place: turn filtered rows into raw ones. */
static int pin_unfilter(uint8_t *rows, size_t h, size_t stride, size_t bpp)
{
    size_t y, i;
    for (y = 0; y < h; y++) {
        uint8_t *cur = rows + y * (stride + 1);
        int filter = cur[0];
        uint8_t *raw = cur + 1;
        const uint8_t *up = y ? rows + (y - 1) * (stride + 1) + 1 : NULL;
        switch (filter) {
        case 0: break;
        case 1: for (i = bpp; i < stride; i++) raw[i] = (uint8_t)(raw[i] + raw[i - bpp]); break;
        case 2: if (up) for (i = 0; i < stride; i++) raw[i] = (uint8_t)(raw[i] + up[i]); break;
        case 3:
            for (i = 0; i < stride; i++) {
                int a = i >= bpp ? raw[i - bpp] : 0, b = up ? up[i] : 0;
                raw[i] = (uint8_t)(raw[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (i = 0; i < stride; i++) {
                int a = i >= bpp ? raw[i - bpp] : 0;
                int b = up ? up[i] : 0;
                int c = (up && i >= bpp) ? up[i - bpp] : 0;
                raw[i] = (uint8_t)(raw[i] + pin_paeth(a, b, c));
            }
            break;
        default: return 0;
        }
    }
    return 1;
}

typedef struct {
    int      w, h, depth, colour, interlace;
    uint8_t  pal[256 * 3];
    uint8_t  alpha[256];
    int      npal, nalpha;
} pin_head;

/* One pixel out of a raw scanline, as B,G,R,A. */
static void pin_pixel(const pin_head *hd, const uint8_t *row, size_t x, uint8_t *out)
{
    int step = hd->depth == 16 ? 2 : 1;
    switch (hd->colour) {
    case 0: {                                   /* greyscale */
        uint8_t g = hd->depth == 8 || hd->depth == 16 ? row[x * step] : 0;
        if (hd->depth < 8) {
            int per = 8 / hd->depth, shift = 8 - hd->depth * (int)(x % (size_t)per + 1);
            int max = (1 << hd->depth) - 1;
            g = (uint8_t)(((row[x / (size_t)per] >> shift) & max) * 255 / max);
        }
        out[0] = out[1] = out[2] = g; out[3] = 255;
        break; }
    case 2:                                     /* truecolour */
        out[2] = row[x * 3 * step];
        out[1] = row[x * 3 * step + step];
        out[0] = row[x * 3 * step + 2 * step];
        out[3] = 255;
        break;
    case 3: {                                   /* palette */
        int per = hd->depth < 8 ? 8 / hd->depth : 1, idx;
        if (hd->depth < 8) {
            int shift = 8 - hd->depth * (int)(x % (size_t)per + 1);
            idx = (row[x / (size_t)per] >> shift) & ((1 << hd->depth) - 1);
        } else idx = row[x];
        if (idx >= hd->npal) idx = 0;
        out[2] = hd->pal[idx * 3];
        out[1] = hd->pal[idx * 3 + 1];
        out[0] = hd->pal[idx * 3 + 2];
        out[3] = idx < hd->nalpha ? hd->alpha[idx] : 255;
        break; }
    case 4:                                     /* grey + alpha */
        out[0] = out[1] = out[2] = row[x * 2 * step];
        out[3] = row[x * 2 * step + step];
        break;
    default:                                    /* 6: truecolour + alpha */
        out[2] = row[x * 4 * step];
        out[1] = row[x * 4 * step + step];
        out[0] = row[x * 4 * step + 2 * step];
        out[3] = row[x * 4 * step + 3 * step];
        break;
    }
}

static int pin_channels(int colour)
{
    switch (colour) {
    case 0: return 1; case 2: return 3; case 3: return 1;
    case 4: return 2; case 6: return 4; default: return 0;
    }
}

/* Bytes one pass's scanline occupies, for `px` pixels. */
static size_t pin_stride(const pin_head *hd, size_t px)
{
    size_t bits = px * (size_t)pin_channels(hd->colour) * (size_t)hd->depth;
    return (bits + 7) / 8;
}

static uint32_t pin_be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3]; }

/* Adam7. Pass 0 is every eighth pixel of every eighth row; pass 6 is every
 * remaining row entire. */
static const int pin_xo[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const int pin_yo[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const int pin_xs[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const int pin_ys[7] = { 8, 8, 8, 4, 4, 2, 2 };

/* Decode `data` into a freshly malloc'd BGRX buffer. Returns it, and sets the width and height,
 * or NULL -- there is no partial success, because half an editor is worse than
 * none. The caller frees. */
static uint32_t *png_decode(const uint8_t *data, size_t n, int *w, int *h)
{
    pin_head hd;
    const uint8_t *p = data + 8;
    uint8_t *idat = NULL, *raw = NULL;
    size_t idatlen = 0, idatcap = 0, rawcap = 0, got;
    uint32_t *out = NULL;
    int pass, npass, ok = 0;

    if (!data || n < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    memset(&hd, 0, sizeof hd);

    /* Chunks. IDAT may be split arbitrarily, so they are joined first. */
    while ((size_t)(p - data) + 8 <= n) {
        uint32_t clen = pin_be32(p);
        const char *tag = (const char *)p + 4;
        const uint8_t *body = p + 8;
        if ((size_t)(body - data) + clen + 4 > n) break;
        if (!memcmp(tag, "IHDR", 4) && clen >= 13) {
            hd.w = (int)pin_be32(body);
            hd.h = (int)pin_be32(body + 4);
            hd.depth = body[8]; hd.colour = body[9]; hd.interlace = body[12];
            if (hd.w <= 0 || hd.h <= 0 || hd.w > 16384 || hd.h > 16384) goto done;
            if (!pin_channels(hd.colour)) goto done;
            if (hd.colour == 3 ? (hd.depth != 1 && hd.depth != 2 && hd.depth != 4 &&
                                  hd.depth != 8)
                               : (hd.depth != 8 && hd.depth != 16)) goto done;
            if (body[10] != 0 || body[11] != 0) goto done;   /* compression/filter */
        } else if (!memcmp(tag, "PLTE", 4)) {
            hd.npal = (int)(clen / 3);
            if (hd.npal > 256) hd.npal = 256;
            memcpy(hd.pal, body, (size_t)hd.npal * 3);
        } else if (!memcmp(tag, "tRNS", 4) && hd.colour == 3) {
            hd.nalpha = (int)(clen > 256 ? 256 : clen);
            memcpy(hd.alpha, body, (size_t)hd.nalpha);
        } else if (!memcmp(tag, "IDAT", 4)) {
            if (idatlen + clen > idatcap) {
                size_t want = (idatlen + clen) * 2 + 4096;
                uint8_t *bigger = realloc(idat, want);
                if (!bigger) goto done;
                idat = bigger; idatcap = want;
            }
            memcpy(idat + idatlen, body, clen);
            idatlen += clen;
        } else if (!memcmp(tag, "IEND", 4)) break;
        p = body + clen + 4;
    }
    if (!idat || idatlen < 3 || !hd.w) goto done;

    /* PNG says exactly how much the stream expands to, so the buffer is sized
     * rather than grown -- and a stream that claims more is refused. */
    npass = hd.interlace ? 7 : 1;
    for (pass = 0; pass < npass; pass++) {
        size_t pw = hd.interlace
            ? (size_t)((hd.w - pin_xo[pass] + pin_xs[pass] - 1) / pin_xs[pass])
            : (size_t)hd.w;
        size_t ph = hd.interlace
            ? (size_t)((hd.h - pin_yo[pass] + pin_ys[pass] - 1) / pin_ys[pass])
            : (size_t)hd.h;
        if (!pw || !ph) continue;
        rawcap += ph * (pin_stride(&hd, pw) + 1);
    }
    /* A bound on what a header can ask for. The dimensions are already capped
     * at 16384, but seven interlace passes of a 16-bit RGBA image still comes
     * to a gigabyte, and a file claiming that is not artwork. */
    if (rawcap > (size_t)(256u << 20)) goto done;
    if (!(raw = malloc(rawcap ? rawcap : 1))) goto done;
    /* Two bytes of zlib header in front, four of Adler-32 behind; neither is
     * deflate's business and the checksum is not worth the code. */
    got = pin_inflate(idat + 2, idatlen - 2, raw, rawcap);
    if (got != rawcap) goto done;

    if (!(out = calloc((size_t)hd.w * (size_t)hd.h, 4))) goto done;
    {
        uint8_t *cursor = raw;
        for (pass = 0; pass < npass; pass++) {
            size_t pw = hd.interlace
                ? (size_t)((hd.w - pin_xo[pass] + pin_xs[pass] - 1) / pin_xs[pass])
                : (size_t)hd.w;
            size_t ph = hd.interlace
                ? (size_t)((hd.h - pin_yo[pass] + pin_ys[pass] - 1) / pin_ys[pass])
                : (size_t)hd.h;
            size_t stride = pin_stride(&hd, pw), bpp, y, x;
            if (!pw || !ph) continue;
            bpp = ((size_t)pin_channels(hd.colour) * (size_t)hd.depth + 7) / 8;
            if (!bpp) bpp = 1;
            if (!pin_unfilter(cursor, ph, stride, bpp)) { free(out); out = NULL; goto done; }
            for (y = 0; y < ph; y++) {
                const uint8_t *row = cursor + y * (stride + 1) + 1;
                size_t dy = hd.interlace
                    ? (size_t)pin_yo[pass] + y * (size_t)pin_ys[pass] : y;
                for (x = 0; x < pw; x++) {
                    size_t dx = hd.interlace
                        ? (size_t)pin_xo[pass] + x * (size_t)pin_xs[pass] : x;
                    if (dx >= (size_t)hd.w || dy >= (size_t)hd.h) continue;
                    pin_pixel(&hd, row, x,
                              (uint8_t *)&out[dy * (size_t)hd.w + dx]);
                }
            }
            cursor += ph * (stride + 1);
        }
    }
    if (w) *w = hd.w;
    if (h) *h = hd.h;
    ok = 1;

done:
    free(idat);
    free(raw);
    if (!ok) { free(out); out = NULL; }
    return out;
}

/* ----------------------------------------------------------------- BMP
 *
 * The same editors that keep their artwork as PNG sometimes keep some of it as
 * Windows BMP -- four of the twelve VSTGUI plugins here do, and one of them
 * keeps its *background* that way, which is the image an editor sizes itself
 * from. ImageIO reads both, so this does too, and the caller sniffs rather than
 * trusting the extension.
 *
 * Only what is actually present: an uncompressed 8-bit palettised or 24-bit
 * BITMAPINFOHEADER image. Anything else is refused rather than guessed at. */
static uint32_t *bmp_decode(const uint8_t *d, size_t n, int *w, int *h)
{
    uint32_t off, hdr, comp, *out;
    int32_t  bw, bh;
    uint16_t bpp;
    size_t   stride, y, x, palbase, ncol;
    int      flip = 1;

    if (!d || n < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    off  = (uint32_t)d[10] | ((uint32_t)d[11] << 8) |
           ((uint32_t)d[12] << 16) | ((uint32_t)d[13] << 24);
    hdr  = (uint32_t)d[14] | ((uint32_t)d[15] << 8) |
           ((uint32_t)d[16] << 16) | ((uint32_t)d[17] << 24);
    if (hdr < 40) return NULL;
    memcpy(&bw, d + 18, 4);
    memcpy(&bh, d + 22, 4);
    memcpy(&bpp, d + 28, 2);
    comp = (uint32_t)d[30] | ((uint32_t)d[31] << 8) |
           ((uint32_t)d[32] << 16) | ((uint32_t)d[33] << 24);
    if (comp != 0) return NULL;                       /* no RLE here */
    if (bpp != 8 && bpp != 24) return NULL;
    if (bh < 0) { bh = -bh; flip = 0; }               /* top-down */
    if (bw <= 0 || bh <= 0 || bw > 16384 || bh > 16384) return NULL;

    stride = (((size_t)bw * bpp + 31) / 32) * 4;      /* rows are 4-aligned */
    if ((size_t)off + stride * (size_t)bh > n) return NULL;
    palbase = 14 + hdr;
    ncol = (off > palbase) ? (off - palbase) / 4 : 0;
    if (bpp == 8 && ncol == 0) return NULL;

    if (!(out = calloc((size_t)bw * (size_t)bh, 4))) return NULL;
    for (y = 0; y < (size_t)bh; y++) {
        const uint8_t *row = d + off + (flip ? ((size_t)bh - 1 - y) : y) * stride;
        uint8_t *dst = (uint8_t *)&out[y * (size_t)bw];
        for (x = 0; x < (size_t)bw; x++, dst += 4) {
            if (bpp == 8) {
                size_t idx = row[x];
                const uint8_t *c = d + palbase + (idx < ncol ? idx : 0) * 4;
                dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2];   /* stored BGR */
            } else {
                dst[0] = row[x * 3];
                dst[1] = row[x * 3 + 1];
                dst[2] = row[x * 3 + 2];
            }
            dst[3] = 255;                              /* BMP carries no alpha */
        }
    }
    if (w) *w = bw;
    if (h) *h = bh;
    return out;
}

/* Decode whichever of the two this is, by its own first bytes. */
static uint32_t *image_decode(const uint8_t *d, size_t n, int *w, int *h)
{
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return png_decode(d, n, w, h);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')            return bmp_decode(d, n, w, h);
    return NULL;
}

#endif /* PELOAD_PNG_IN_H */
