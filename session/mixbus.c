#define _GNU_SOURCE
#include "mixbus.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SOURCES 64
#define SECONDS 8
#define MIXBUS_SAFETY 2048                      /* 43 ms: a margin under what the slowest source has delivered */

typedef struct {
    atomic_bool     used;
    atomic_uint     gen;
    _Atomic uint64_t cursor;
    _Atomic double  last;
} source;

static source            g_src[SOURCES];
static _Atomic(_Atomic float *) g_ring;
static size_t            g_cap;                 /* frames; the ring holds cap * 2 floats */
static int               g_rate = 48000;
static double            g_t0;
static atomic_bool       g_active;
static atomic_int        g_error;               /* a write failed: the file is short */
static atomic_int        g_inflight;
static atomic_uint       g_gen;
static _Atomic uint64_t  g_drained, g_written, g_dropped, g_high;
static FILE             *g_out;
static pthread_t         g_writer;
static int               g_writer_on;
static char              g_path[4096];

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int mixbus_register(void)
{
    int i;
    for (i = 0; i < SOURCES; i++) {
        bool want = false;
        if (atomic_compare_exchange_strong(&g_src[i].used, &want, true)) {
            atomic_store(&g_src[i].gen, 0);
            return i;
        }
    }
    return -1;
}

void mixbus_release(int id)
{
    if (id >= 0 && id < SOURCES) atomic_store(&g_src[id].used, false);
}

static void write_header(uint64_t frames)
{
    const uint32_t bytes = (uint32_t)(frames * 4);
    unsigned char h[44];
    uint32_t v32;
    uint16_t v16;
    long keep = ftell(g_out);
    memcpy(h, "RIFF", 4);
    v32 = 36 + bytes;               memcpy(h + 4, &v32, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v32 = 16;                       memcpy(h + 16, &v32, 4);
    v16 = 1;                        memcpy(h + 20, &v16, 2);
    v16 = 2;                        memcpy(h + 22, &v16, 2);
    v32 = (uint32_t)g_rate;         memcpy(h + 24, &v32, 4);
    v32 = (uint32_t)g_rate * 4;     memcpy(h + 28, &v32, 4);
    v16 = 4;                        memcpy(h + 32, &v16, 2);
    v16 = 16;                       memcpy(h + 34, &v16, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(g_out, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, g_out);
    if (keep > 0) fseek(g_out, keep, SEEK_SET);   /* back to where the data is appended */
    fflush(g_out);
}

/* Everything before `limit`, to the file. The writer thread, or mixbus_stop once
 * it has joined. */
static void drain_to(uint64_t limit)
{
    uint64_t pos = atomic_load(&g_drained);
    _Atomic float *ring = atomic_load(&g_ring);
    int16_t pcm[4096 * 2];
    if (limit <= pos || !ring) return;
    while (pos < limit) {
        size_t chunk = (size_t)(limit - pos > 4096 ? 4096 : limit - pos), i;
        for (i = 0; i < chunk * 2; i++) {
            float v = atomic_exchange(&ring[(pos * 2 + i) % (g_cap * 2)], 0.0f);
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            pcm[i] = (int16_t)(v * 32767.0f);
        }
        if (fwrite(pcm, sizeof pcm[0], chunk * 2, g_out) != chunk * 2) atomic_store(&g_error, 1);   /* a full disk, say */
        pos += chunk;
        atomic_fetch_add(&g_written, chunk);
        atomic_store(&g_drained, pos);
    }
    write_header(atomic_load(&g_written));          /* playable at any moment */
}

/* How far the writer may go: what every source still feeding has gone past. The
 * wall clock is not used for this -- an audio device's clock runs a little off
 * the system's, and over a long take a source on a slow clock would fall behind
 * a limit taken from the wall and have its blocks arrive too late to be kept.
 * A source that has not fed for a quarter of a second, or not since this take
 * began, is not waited for. Returns 0 when there is nobody to wait for. */
static int writer_limit(double t, uint64_t *limit)
{
    unsigned g = atomic_load(&g_gen);
    uint64_t low = UINT64_MAX;
    int i, any = 0;
    for (i = 0; i < SOURCES; i++) {
        source *s = &g_src[i];
        if (!atomic_load(&s->used) || atomic_load(&s->gen) != g) continue;
        if (t - atomic_load_explicit(&s->last, memory_order_relaxed) > 0.25) continue;
        {
            uint64_t c = atomic_load_explicit(&s->cursor, memory_order_relaxed);
            if (c < low) low = c;
            any = 1;
        }
    }
    if (!any || low <= MIXBUS_SAFETY) return 0;
    *limit = low - MIXBUS_SAFETY;
    return 1;
}

static void *writer_main(void *u)
{
    (void)u;
    while (atomic_load(&g_active)) {
        uint64_t limit;
        struct timespec ts = { 0, 20 * 1000000L };
        if (writer_limit(now(), &limit)) drain_to(limit);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int mixbus_start(const char *path, int rate)
{
    size_t i;
    _Atomic float *ring;
    if (atomic_load(&g_active)) return -1;
    if (!(g_out = fopen(path, "wb"))) return -1;
    g_rate = rate;
    /* The ring is made once and never freed: an audio thread that was inside a
     * feed when the last take ended may still be reading the pointer, and the
     * few megabytes are cheaper than that being a crash. A later take at a rate
     * that needs more room than the first is refused. */
    ring = atomic_load(&g_ring);
    if (!ring) {
        g_cap = (size_t)rate * SECONDS;
        ring = malloc(g_cap * 2 * sizeof *ring);
        if (!ring) { fclose(g_out); g_out = NULL; return -1; }
        atomic_store(&g_ring, ring);
    } else if ((size_t)rate * SECONDS > g_cap) {
        fclose(g_out); g_out = NULL;
        return -1;
    }
    for (i = 0; i < g_cap * 2; i++) atomic_init(&ring[i], 0.0f);
    atomic_store(&g_error, 0);
    atomic_store(&g_drained, 0);
    atomic_store(&g_written, 0);
    atomic_store(&g_dropped, 0);
    atomic_store(&g_high, 0);
    g_t0 = now();
    atomic_fetch_add(&g_gen, 1);
    snprintf(g_path, sizeof g_path, "%s", path);
    write_header(0);
    atomic_store(&g_active, true);
    g_writer_on = pthread_create(&g_writer, NULL, writer_main, NULL) == 0;
    return 0;
}

int mixbus_stop(char *path, size_t n)
{
    int i;
    if (!atomic_exchange(&g_active, false)) return -1;
    for (i = 0; i < 100 && atomic_load(&g_inflight) > 0; i++) {
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);                       /* a block in flight lands */
    }
    if (g_writer_on) pthread_join(g_writer, NULL);
    g_writer_on = 0;
    drain_to(atomic_load(&g_high));
    write_header(atomic_load(&g_written));
    if (fclose(g_out) != 0) atomic_store(&g_error, 1);
    g_out = NULL;
    if (path && n) snprintf(path, n, "%s", g_path);
    return 0;
}

int      mixbus_active(void)  { return atomic_load(&g_active); }
int      mixbus_failed(void)  { return atomic_load(&g_error); }
uint64_t mixbus_frames(void)  { return atomic_load(&g_written); }
uint64_t mixbus_dropped(void) { return atomic_load(&g_dropped); }

static void add(int id, const float *f, const double *d, int frames)
{
    source *s;
    double t;
    uint64_t wall, c, dr, hw;
    unsigned g;
    _Atomic float *ring;
    int i;

    if (!atomic_load(&g_active) || id < 0 || id >= SOURCES) return;
    atomic_fetch_add(&g_inflight, 1);
    ring = atomic_load(&g_ring);
    if (atomic_load(&g_active) && ring) {
        s = &g_src[id];
        t = now();
        wall = t > g_t0 ? (uint64_t)((t - g_t0) * g_rate) : 0;
        c = atomic_load_explicit(&s->cursor, memory_order_relaxed);
        g = atomic_load(&g_gen);
        /* Started with this take, or quiet for a while: back on the wall clock. */
        if (atomic_load(&s->gen) != g || t - atomic_load_explicit(&s->last, memory_order_relaxed) > 0.25) {
            c = wall;
            atomic_store(&s->gen, g);
        }
        dr = atomic_load(&g_drained);
        if (c < dr) c = dr;                         /* late: what is past is gone */
        if (c + (uint64_t)frames - dr > g_cap) {    /* the writer is far behind */
            atomic_fetch_add(&g_dropped, (uint64_t)frames);
        } else {
            for (i = 0; i < frames * 2; i++) {
                float v = f ? f[i] : (float)d[i];
                _Atomic float *slot = &ring[(c * 2 + (uint64_t)i) % (g_cap * 2)];
                float old = atomic_load_explicit(slot, memory_order_relaxed);
                while (!atomic_compare_exchange_weak_explicit(slot, &old, old + v,
                           memory_order_relaxed, memory_order_relaxed)) { }
            }
        }
        c += (uint64_t)frames;
        atomic_store_explicit(&s->cursor, c, memory_order_relaxed);
        atomic_store_explicit(&s->last, t, memory_order_relaxed);
        hw = atomic_load(&g_high);
        while (c > hw && !atomic_compare_exchange_weak(&g_high, &hw, c)) { }
    }
    atomic_fetch_sub(&g_inflight, 1);
}

void mixbus_feed_f(int id, const float *x, int frames)  { add(id, x, NULL, frames); }
void mixbus_feed_d(int id, const double *x, int frames) { add(id, NULL, x, frames); }
