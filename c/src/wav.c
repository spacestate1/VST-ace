#include "wav.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v);       p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v); p[1] = (unsigned char)(v >> 8);
}

static void wav_header(unsigned char *hdr, size_t samples, int channels,
                       int samplerate)
{
    uint32_t bytes = (uint32_t)(samples * 2);

    hdr[0]='R'; hdr[1]='I'; hdr[2]='F'; hdr[3]='F';
    put32(hdr + 4, 36 + bytes);
    hdr[8]='W'; hdr[9]='A'; hdr[10]='V'; hdr[11]='E';
    hdr[12]='f'; hdr[13]='m'; hdr[14]='t'; hdr[15]=' ';
    put32(hdr + 16, 16);                        /* fmt chunk size  */
    put16(hdr + 20, 1);                         /* PCM             */
    put16(hdr + 22, (uint16_t)channels);
    put32(hdr + 24, (uint32_t)samplerate);
    put32(hdr + 28, (uint32_t)samplerate * 2u * (uint32_t)channels);
    put16(hdr + 32, (uint16_t)(2 * channels));  /* block align     */
    put16(hdr + 34, 16);                        /* bits per sample */
    hdr[36]='d'; hdr[37]='a'; hdr[38]='t'; hdr[39]='a';
    put32(hdr + 40, bytes);
}

static int wav_write(const char *path, const double *data, size_t samples,
                     int channels, int samplerate, double scale)
{
    unsigned char hdr[44];
    int16_t      *pcm;
    size_t        i;
    FILE         *f;

    if (!(pcm = malloc(samples * sizeof *pcm))) return -1;
    for (i = 0; i < samples; i++) {
        /* truncation toward zero, as numpy's float->int16 cast does */
        double v = data[i] * scale * 32767.0;
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        pcm[i] = (int16_t)v;
    }

    wav_header(hdr, samples, channels, samplerate);

    if (!(f = fopen(path, "wb"))) { perror(path); free(pcm); return -1; }
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        fwrite(pcm, 2, samples, f) != samples) {
        perror(path); fclose(f); free(pcm); return -1;
    }
    fclose(f);
    free(pcm);
    return 0;
}

int wav_write_mono16(const char *path, const double *data, size_t n, int samplerate)
{
    double peak = 0.0;
    size_t i;

    for (i = 0; i < n; i++) {
        double a = fabs(data[i]);
        if (a > peak) peak = a;
    }
    return wav_write(path, data, n, 1, samplerate, (peak > 0.0) ? 0.98 / peak : 1.0);
}

int wav_write_stereo16(const char *path, const double *interleaved,
                       size_t frames, int samplerate)
{
    return wav_write(path, interleaved, frames * 2, 2, samplerate, 1.0);
}

/* ---- reading ----------------------------------------------------------- */

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int wav_read_mono(const char *path, float **out, size_t *frames, int *samplerate)
{
    FILE          *f;
    unsigned char *buf;
    long           len;
    size_t         pos, dpos = 0, dlen = 0;
    int            ch = 0, bits = 0, fmt = 0, sr = 0;
    size_t         n, i, c;
    float         *o;

    *out = NULL; *frames = 0;
    if (!(f = fopen(path, "rb"))) return -1;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 44) { fclose(f); return -1; }
    if (!(buf = malloc((size_t)len))) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return -1; }
    fclose(f);

    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return -1; }

    /* Walk the chunk list; do not assume the data starts at offset 44. */
    pos = 12;
    while (pos + 8 <= (size_t)len) {
        uint32_t csz = rd32(buf + pos + 4);
        const unsigned char *body = buf + pos + 8;
        if (!memcmp(buf + pos, "fmt ", 4) && csz >= 16) {
            fmt  = rd16(body);
            ch   = rd16(body + 2);
            sr   = (int)rd32(body + 4);
            bits = rd16(body + 14);
        } else if (!memcmp(buf + pos, "data", 4)) {
            dpos = pos + 8;
            dlen = csz;
            if (dpos + dlen > (size_t)len) dlen = (size_t)len - dpos;
        }
        pos += 8 + csz + (csz & 1);
    }
    if (!dlen || ch < 1 || bits < 8) { free(buf); return -1; }

    n = dlen / (size_t)(ch * (bits / 8));
    if (!(o = malloc(n * sizeof *o))) { free(buf); return -1; }

    for (i = 0; i < n; i++) {
        double acc = 0.0;
        for (c = 0; c < (size_t)ch; c++) {
            const unsigned char *s = buf + dpos + (i * (size_t)ch + c) * (size_t)(bits / 8);
            if (bits == 8)       acc += ((double)s[0] - 128.0) / 128.0;
            else if (bits == 16) acc += (double)(int16_t)rd16(s) / 32768.0;
            else if (bits == 24) {
                int32_t v = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 |
                                      (uint32_t)s[2] << 24);
                acc += (double)(v >> 8) / 8388608.0;
            } else if (bits == 32 && fmt == 3) {
                float fv; uint32_t u = rd32(s); memcpy(&fv, &u, 4); acc += fv;
            } else if (bits == 32) {
                acc += (double)(int32_t)rd32(s) / 2147483648.0;
            }
        }
        o[i] = (float)(acc / ch);
    }
    free(buf);
    *out = o; *frames = n; *samplerate = sr ? sr : 44100;
    return 0;
}
