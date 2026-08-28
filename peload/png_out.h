/* A minimal PNG writer, shared by every editor path that needs to prove what a
 * plugin actually drew.
 *
 * Stored (uncompressed) deflate blocks only: the point is a file an image viewer
 * will open, with no zlib dependency and nothing to get wrong. Input is the
 * framebuffer format all of the editor backends here produce -- 32-bit BGRX, one
 * word per pixel, top row first.
 */
#ifndef PELOAD_PNG_OUT_H
#define PELOAD_PNG_OUT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t png_crc(const uint8_t *d, size_t n, uint32_t c)
{
    static uint32_t tbl[256];
    static int built;
    size_t i;
    if (!built) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            tbl[k] = v;
        }
        built = 1;
    }
    for (i = 0; i < n; i++) c = tbl[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return c;
}

static void png_be32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v; }

static void png_chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t hdr[4];
    uint32_t c;
    png_be32(hdr, (uint32_t)n);
    fwrite(hdr, 1, 4, f);
    fwrite(tag, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    c = png_crc((const uint8_t *)tag, 4, 0xFFFFFFFFu);
    c = png_crc(data, n, c) ^ 0xFFFFFFFFu;
    png_be32(hdr, c);
    fwrite(hdr, 1, 4, f);
}

static int png_write_bgrx(const char *path, const uint32_t *px, int w, int h)
{
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    FILE *f = fopen(path, "wb");
    uint8_t ihdr[13], *raw, *z;
    size_t rawlen, zlen, i, off;
    uint32_t a = 1, b = 0;                       /* adler32 */
    int y, x;

    if (!f) { perror(path); return -1; }
    fwrite(sig, 1, 8, f);

    png_be32(ihdr, (uint32_t)w); png_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;  /* RGB8 */
    png_chunk(f, "IHDR", ihdr, sizeof ihdr);

    /* one filter byte per row, then RGB triples */
    rawlen = (size_t)h * (1 + (size_t)w * 3);
    if (!(raw = malloc(rawlen))) { fclose(f); return -1; }
    off = 0;
    for (y = 0; y < h; y++) {
        raw[off++] = 0;                          /* filter: none */
        for (x = 0; x < w; x++) {
            uint32_t v = px[(size_t)y * w + x];  /* BGRX */
            raw[off++] = (uint8_t)(v >> 16);
            raw[off++] = (uint8_t)(v >> 8);
            raw[off++] = (uint8_t)v;
        }
    }
    for (i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }

    /* zlib stream: 2-byte header, stored deflate blocks of <= 65535, adler32 */
    zlen = 2 + rawlen + 5 * ((rawlen + 65534) / 65535) + 4;
    if (!(z = malloc(zlen))) { free(raw); fclose(f); return -1; }
    off = 0;
    z[off++] = 0x78; z[off++] = 0x01;
    for (i = 0; i < rawlen; ) {
        size_t n = rawlen - i > 65535 ? 65535 : rawlen - i;
        z[off++] = (i + n >= rawlen) ? 1 : 0;    /* BFINAL */
        z[off++] = (uint8_t)n; z[off++] = (uint8_t)(n >> 8);
        z[off++] = (uint8_t)~n; z[off++] = (uint8_t)(~n >> 8);
        memcpy(z + off, raw + i, n);
        off += n; i += n;
    }
    png_be32(z + off, (b << 16) | a); off += 4;
    png_chunk(f, "IDAT", z, off);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 0;
}


#endif /* PELOAD_PNG_OUT_H */
