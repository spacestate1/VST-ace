/* open/fdopen/fsync/O_NOFOLLOW: not in strict c99 without this. */
#define _POSIX_C_SOURCE 200809L

#include "wav.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <errno.h>
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
        if (v != v) v = 0.0;
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        pcm[i] = (int16_t)v;
    }

    wav_header(hdr, samples, channels, samplerate);

    /* Written under a temporary name next to the target and renamed over it
     * only once every byte and the close have succeeded: a full disk (which
     * fclose reports, not fwrite) must neither lose the old file nor leave a
     * truncated new one. */
    {
        char   tmp[4096];
        int    fd = -1, tries, ok;
        size_t plen = strlen(path);
        static unsigned counter;

        if (plen + 24 > sizeof tmp) { fprintf(stderr, "%s: path too long\n", path); free(pcm); return -1; }
        for (tries = 0; tries < 100 && fd < 0; tries++) {
            snprintf(tmp, sizeof tmp, "%s.tmp%ld.%u", path, (long)getpid(), counter++);
            fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
            if (fd < 0 && errno != EEXIST) break;
        }
        if (fd < 0 || !(f = fdopen(fd, "wb"))) {
            perror(path); if (fd >= 0) { close(fd); unlink(tmp); } free(pcm); return -1;
        }
        ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
             fwrite(pcm, 2, samples, f) == samples &&
             fflush(f) == 0 && fsync(fd) == 0;
        if (fclose(f) != 0) ok = 0;
        free(pcm);
        if (!ok || rename(tmp, path) != 0) {
            perror(path); unlink(tmp); return -1;
        }
    }
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

/* What a WAV may be to be read: the whole file is held in memory and then
 * again as floats, so a file that names a gigabyte is not a sample. */
#define WAV_MAX_BYTES (256L * 1024 * 1024)
#define WAV_MAX_RATE  768000
#define WAV_MAX_FLOATS ((size_t)128 * 1024 * 1024)   /* 512 MB of float output */


static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* The reader behind both of the public ones. outch 1: every channel summed
 * and averaged. outch 2: interleaved stereo -- a mono file on both sides, the
 * first two channels of anything wider. */
static int wav_read(const char *path, float **out, size_t *frames, int *samplerate,
                    int outch)
{
    FILE          *f;
    unsigned char *buf;
    long           len;
    size_t         pos, dpos = 0, dlen = 0;
    int            ch = 0, bits = 0, fmt = 0, sr = 0;
    size_t         n, i, c;
    float         *o;

    *out = NULL; *frames = 0;
    /* Opened without waiting and only if it is a plain file: a pad that names
     * a FIFO or a device would otherwise hold the load, and the window with
     * it, for as long as it liked. */
    {
        struct stat st;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY);
        if (fd < 0) return -1;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > WAV_MAX_BYTES) { close(fd); return -1; }
        if (!(f = fdopen(fd, "rb"))) { close(fd); return -1; }
    }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 44 || len > WAV_MAX_BYTES) { fclose(f); return -1; }
    if (!(buf = malloc((size_t)len))) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return -1; }
    fclose(f);

    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return -1; }

    /* Walk the chunk list; do not assume the data starts at offset 44. */
    pos = 12;
    while (pos + 8 <= (size_t)len) {
        uint32_t csz = rd32(buf + pos + 4);
        const unsigned char *body = buf + pos + 8;
        /* A chunk that says it is longer than the file is not read from: the
         * fmt body is 16 to 26 bytes of the file, so all of it has to be there. */
        if (!memcmp(buf + pos, "fmt ", 4) && csz >= 16 && pos + 8 + (size_t)csz <= (size_t)len) {
            fmt  = rd16(body);
            ch   = rd16(body + 2);
            sr   = (int)rd32(body + 4);
            bits = rd16(body + 14);
            /* WAVE_FORMAT_EXTENSIBLE carries the real format in its
             * sub-format GUID, whose first two bytes are the old tag. */
            if (fmt == 0xFFFE && csz >= 26) fmt = rd16(body + 24);
        } else if (!memcmp(buf + pos, "data", 4)) {
            dpos = pos + 8;
            dlen = csz;
            if (dpos + dlen > (size_t)len) dlen = (size_t)len - dpos;
        }
        pos += 8 + csz + (csz & 1);
    }
    if (!dlen || ch < 1 || bits < 8) { free(buf); return -1; }
    /* A rate is a positive number of samples a second. The word is unsigned
     * in the file and read as a signed int here: 0x80000000 and up is
     * negative, and a negative rate turns into a negative playback step and
     * a read before the start of the sample. 0 is "not said": 44.1 k. */
    if (sr < 0 || sr > WAV_MAX_RATE) { free(buf); return -1; }

    n = dlen / (size_t)(ch * (bits / 8));
    /* Bound the float copy before allocating it: 8-bit mono at 256 MB would
     * otherwise ask for 1 to 2 GB, four to eight times the file. */
    if (n > WAV_MAX_FLOATS / (size_t)outch) { free(buf); return -1; }
    if (!(o = malloc(n * (size_t)outch * sizeof *o))) { free(buf); return -1; }

    for (i = 0; i < n; i++) {
        double acc = 0.0;
        for (c = 0; c < (size_t)ch; c++) {
            const unsigned char *s = buf + dpos + (i * (size_t)ch + c) * (size_t)(bits / 8);
            double v = 0.0;
            if (bits == 8)       v = ((double)s[0] - 128.0) / 128.0;
            else if (bits == 16) v = (double)(int16_t)rd16(s) / 32768.0;
            else if (bits == 24) {
                int32_t x = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 |
                                      (uint32_t)s[2] << 24);
                v = (double)(x >> 8) / 8388608.0;
            } else if (bits == 32 && fmt == 3) {
                float fv; uint32_t u = rd32(s); memcpy(&fv, &u, 4);
                /* A NaN or an infinity in the file is silence, not a sample:
                 * one would run through every mix it was added to. */
                v = (fv == fv && fv < 1e9f && fv > -1e9f) ? fv : 0.0;
            } else if (bits == 32) {
                v = (double)(int32_t)rd32(s) / 2147483648.0;
            }
            if (outch == 1) acc += v;
            else if (c < 2) o[2 * i + c] = (float)v;
        }
        if (outch == 1) o[i] = (float)(acc / ch);
        else if (ch == 1) o[2 * i + 1] = o[2 * i];
    }
    free(buf);
    *out = o; *frames = n; *samplerate = sr ? sr : 44100;
    return 0;
}

int wav_read_mono(const char *path, float **out, size_t *frames, int *samplerate)
{
    return wav_read(path, out, frames, samplerate, 1);
}

int wav_read_stereo(const char *path, float **out, size_t *frames, int *samplerate)
{
    return wav_read(path, out, frames, samplerate, 2);
}
