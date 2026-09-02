/* Foundation, CoreText, GCD, Blocks and the Darwin file-system variants.
 *
 * The interesting part here is that several of these cannot be forwarded to the
 * host's libc even though a function of the same name exists: Darwin's struct
 * stat and struct dirent have different layouts from Linux's, so a plugin
 * reading st_size or d_name off a Linux struct reads the wrong bytes. Those get
 * translated field by field. The layouts are ABI, written out below.
 *
 * The rest divides into things worth implementing (dispatch semaphores, blocks,
 * CFDictionary/CFSet) and things that only need to fail honestly (HTTP streams
 * for an update check). Failing honestly is not the same as returning zero and
 * hoping -- but it is only right when there is nothing to fall back to. Metal
 * used to be answered with NULL here on the theory that a plugin would pick
 * another renderer; in this corpus none has another renderer to pick, so it is
 * implemented for real in macmetal.c instead.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

#include "macshim.h"

/* ------------------------------------------------------- Darwin structs */

/* struct stat as Darwin lays it out on x86_64 (the $INODE64 variant), 144
 * bytes. Nothing about this matches Linux beyond the field names. */
typedef struct {
    int32_t  st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid, st_gid;
    int32_t  st_rdev;
    int32_t  _pad0;
    int64_t  st_atime_sec, st_atime_nsec;
    int64_t  st_mtime_sec, st_mtime_nsec;
    int64_t  st_ctime_sec, st_ctime_nsec;
    int64_t  st_btime_sec, st_btime_nsec;
    int64_t  st_size, st_blocks;
    int32_t  st_blksize;
    uint32_t st_flags, st_gen;
    int32_t  st_lspare;
    int64_t  st_qspare[2];
} darwin_stat;
_Static_assert(sizeof(darwin_stat) == 144, "Darwin struct stat is 144 bytes");

static void stat_to_darwin(const struct stat *s, darwin_stat *d)
{
    memset(d, 0, sizeof *d);
    d->st_dev   = (int32_t)s->st_dev;
    d->st_mode  = (uint16_t)s->st_mode;
    d->st_nlink = (uint16_t)s->st_nlink;
    d->st_ino   = s->st_ino;
    d->st_uid   = s->st_uid;
    d->st_gid   = s->st_gid;
    d->st_rdev  = (int32_t)s->st_rdev;
    d->st_atime_sec = s->st_atim.tv_sec; d->st_atime_nsec = s->st_atim.tv_nsec;
    d->st_mtime_sec = s->st_mtim.tv_sec; d->st_mtime_nsec = s->st_mtim.tv_nsec;
    d->st_ctime_sec = s->st_ctim.tv_sec; d->st_ctime_nsec = s->st_ctim.tv_nsec;
    d->st_btime_sec = s->st_ctim.tv_sec; d->st_btime_nsec = s->st_ctim.tv_nsec;
    d->st_size    = s->st_size;
    d->st_blocks  = s->st_blocks;
    d->st_blksize = (int32_t)s->st_blksize;
}

static int dw_stat(const char *p, darwin_stat *d)
{ struct stat s; if (stat(p, &s)) return -1; stat_to_darwin(&s, d); return 0; }
static int dw_lstat(const char *p, darwin_stat *d)
{ struct stat s; if (lstat(p, &s)) return -1; stat_to_darwin(&s, d); return 0; }
static int dw_fstat(int fd, darwin_stat *d)
{ struct stat s; if (fstat(fd, &s)) return -1; stat_to_darwin(&s, d); return 0; }

/* Darwin's struct statfs is larger and differently ordered; a plugin asking for
 * it wants free space, so fill the fields it will read and leave the rest zero. */
typedef struct {
    uint32_t f_bsize;
    int32_t  f_iosize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    uint32_t f_owner, f_type, f_flags, f_fssubtype;
    char     f_fstypename[16], f_mntonname[1024], f_mntfromname[1024];
    uint32_t f_reserved[8];
} darwin_statfs;

static int dw_fstatfs(int fd, darwin_statfs *d)
{
    struct statvfs v;
    if (!d || fstatvfs(fd, &v)) return -1;
    memset(d, 0, sizeof *d);
    d->f_bsize  = (uint32_t)v.f_bsize;
    d->f_iosize = (int32_t)v.f_bsize;
    d->f_blocks = v.f_blocks;
    d->f_bfree  = v.f_bfree;
    d->f_bavail = v.f_bavail;
    d->f_files  = v.f_files;
    d->f_ffree  = v.f_ffree;
    snprintf(d->f_fstypename, sizeof d->f_fstypename, "hfs");
    return 0;
}

/* Darwin struct dirent: 64-bit inode, an explicit name length, and a 1024-byte
 * name. Linux's has neither the seek offset nor the name length in that order. */
typedef struct {
    uint64_t d_ino, d_seekoff;
    uint16_t d_reclen, d_namlen;
    uint8_t  d_type;
    char     d_name[1024];
} darwin_dirent;

/* One buffer per open directory. A plugin walks one directory at a time here,
 * but keying on the DIR* keeps that from being an assumption. */
#define MAX_DIRS 16
static struct { DIR *d; darwin_dirent e; } g_dirs[MAX_DIRS];
static pthread_mutex_t g_dirs_lock = PTHREAD_MUTEX_INITIALIZER;

static void *dw_opendir(const char *path)
{
    DIR *d = opendir(path);
    int i;
    if (!d) return NULL;
    pthread_mutex_lock(&g_dirs_lock);
    for (i = 0; i < MAX_DIRS; i++)
        if (!g_dirs[i].d) { g_dirs[i].d = d; break; }
    pthread_mutex_unlock(&g_dirs_lock);
    return d;
}

static void *dw_readdir(void *dp)
{
    DIR *d = dp;
    struct dirent *e;
    darwin_dirent *out = NULL;
    int i;
    if (!d || !(e = readdir(d))) return NULL;
    pthread_mutex_lock(&g_dirs_lock);
    for (i = 0; i < MAX_DIRS; i++) if (g_dirs[i].d == d) { out = &g_dirs[i].e; break; }
    if (!out) { g_dirs[0].d = d; out = &g_dirs[0].e; }
    memset(out, 0, sizeof *out);
    out->d_ino = e->d_ino;
    out->d_type = e->d_type;
    out->d_namlen = (uint16_t)strlen(e->d_name);
    if (out->d_namlen > sizeof out->d_name - 1) out->d_namlen = sizeof out->d_name - 1;
    memcpy(out->d_name, e->d_name, out->d_namlen);
    out->d_reclen = (uint16_t)sizeof *out;
    pthread_mutex_unlock(&g_dirs_lock);
    return out;
}

static int dw_closedir(void *dp)
{
    int i;
    pthread_mutex_lock(&g_dirs_lock);
    for (i = 0; i < MAX_DIRS; i++) if (g_dirs[i].d == dp) { g_dirs[i].d = NULL; break; }
    pthread_mutex_unlock(&g_dirs_lock);
    return closedir(dp);
}

/* --------------------------------------------------------------- Blocks */

/* A block is a struct whose fourth word is the function to call; the compiler
 * emits a reference to __NSConcreteStackBlock as its isa, and one to
 * __NSConcreteMallocBlock for the heap copy anything asynchronous has to make.
 *
 * The copy is the part that matters. A block literal lives on the *stack*, and
 * the contract is that whoever takes it beyond the enclosing call copies it to
 * the heap first -- which is exactly what dispatch_async does. Keeping the
 * stack pointer instead reads a frame that has since returned. It survives for
 * as long as nothing reuses that stack, which is why it took three plug-ins in
 * one browsing session to fault, on a thread with no connection to whatever the
 * host was doing at the time. */
static void *g_concrete_stack_block[8];
static void *g_concrete_malloc_block[8];

#define BLOCK_HAS_COPY_DISPOSE (1 << 25)
#define BLOCK_NEEDS_FREE       (1 << 24)
#define BLOCK_IS_GLOBAL        (1 << 28)

struct block_descriptor   { unsigned long reserved, size; };
/* Present only when BLOCK_HAS_COPY_DISPOSE says so, immediately after the
 * first descriptor. */
struct block_descriptor_2 { void (*copy)(void *dst, const void *src);
                            void (*dispose)(const void *); };
struct block_layout {
    void *isa;
    int   flags, reserved;
    void (*invoke)(void *);
    struct block_descriptor *descriptor;
};

static void *block_copy(void *p)
{
    struct block_layout *b = p, *c;
    size_t sz;

    if (!b || !b->descriptor) return b;
    if (b->flags & BLOCK_IS_GLOBAL) return b;      /* nothing to copy */
    if (b->flags & BLOCK_NEEDS_FREE) return b;     /* already on the heap */
    sz = (size_t)b->descriptor->size;
    /* A size outside this range is not a layout this understands, and copying
     * by it would be worse than not copying at all. */
    if (sz < sizeof *b || sz > (1u << 20)) return b;
    if (!(c = malloc(sz))) return b;
    memcpy(c, b, sz);
    c->isa   = g_concrete_malloc_block;
    c->flags = (b->flags & ~BLOCK_IS_GLOBAL) | BLOCK_NEEDS_FREE;
    /* The compiler emits a helper to fix up whatever the block captured --
     * objects to retain, other blocks to copy in turn. Skipping it leaves the
     * copy sharing the original's captures, which is the bug again one level
     * down. */
    if (b->flags & BLOCK_HAS_COPY_DISPOSE) {
        struct block_descriptor_2 *d2 = (struct block_descriptor_2 *)(b->descriptor + 1);
        if (d2->copy) d2->copy(c, b);
    }
    return c;
}

static void block_release(void *p)
{
    struct block_layout *b = p;
    if (!b || !(b->flags & BLOCK_NEEDS_FREE)) return;
    if (b->flags & BLOCK_HAS_COPY_DISPOSE) {
        struct block_descriptor_2 *d2 = (struct block_descriptor_2 *)(b->descriptor + 1);
        if (d2->dispose) d2->dispose(b);
    }
    free(b);
}

/* BLOCK_FIELD_IS_BLOCK: a captured block is copied rather than borrowed, for
 * the same reason the outer one is. Everything else is stored as-is -- objects
 * here are retired rather than freed, so borrowing one is safe. */
#define BLOCK_FIELD_IS_BLOCK 7
static void bl_object_assign(void *dst, const void *src, int flags)
{
    if (!dst) return;
    if ((flags & 0xff) == BLOCK_FIELD_IS_BLOCK)
        *(void **)dst = block_copy((void *)src);
    else
        *(const void **)dst = src;
}
static void bl_object_dispose(const void *obj, int flags)
{
    if ((flags & 0xff) == BLOCK_FIELD_IS_BLOCK) block_release((void *)obj);
}

/* ----------------------------------------------------------------- GCD */

typedef struct { sem_t s; } dispatch_sem;

static void *gcd_sem_create(long value)
{
    dispatch_sem *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    sem_init(&d->s, 0, (unsigned)(value > 0 ? value : 0));
    return d;
}
static long gcd_sem_signal(void *p)
{ if (p) sem_post(&((dispatch_sem *)p)->s); return 0; }
static long gcd_sem_wait(void *p, uint64_t timeout)
{
    (void)timeout;                    /* DISPATCH_TIME_FOREVER is the common case */
    if (!p) return -1;
    while (sem_wait(&((dispatch_sem *)p)->s)) if (errno != EINTR) return -1;
    return 0;
}

/* A block invoked on a queue. Running it on a fresh thread preserves the
 * asynchrony a plugin is relying on; running it inline would deadlock anything
 * that dispatches from a callback and then waits. */
/* In-flight jobs, so an image is never unmapped out from under one. A block's
 * `invoke` points into the plugin, and munmap while a job is pending jumps into
 * nothing -- see macshim_gcd_drain, which macho_close waits on. */
static pthread_mutex_t g_gcd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gcd_idle = PTHREAD_COND_INITIALIZER;
static int             g_gcd_inflight;

static void *gcd_thread(void *ud)
{
    struct block_layout *b = ud;
    if (b && b->invoke) b->invoke(b);
    block_release(b);
    pthread_mutex_lock(&g_gcd_lock);
    if (--g_gcd_inflight == 0) pthread_cond_broadcast(&g_gcd_idle);
    pthread_mutex_unlock(&g_gcd_lock);
    return NULL;
}
static void gcd_async(void *queue, void *block)
{
    pthread_t t;
    void *copy;
    (void)queue;
    if (!block) return;
    /* Copied to the heap first: the caller's frame is gone by the time the
     * thread runs. This is what real GCD does and what the compiler assumes. */
    copy = block_copy(block);
    pthread_mutex_lock(&g_gcd_lock);
    g_gcd_inflight++;
    pthread_mutex_unlock(&g_gcd_lock);
    if (pthread_create(&t, NULL, gcd_thread, copy) == 0) {
        pthread_detach(t);
    } else {
        pthread_mutex_lock(&g_gcd_lock);
        if (--g_gcd_inflight == 0) pthread_cond_broadcast(&g_gcd_idle);
        pthread_mutex_unlock(&g_gcd_lock);
        block_release(copy);
    }
}

/* Wait up to `ms` for every dispatched block to finish. 1 when they all did. */
int macshim_gcd_drain(int ms)
{
    struct timespec ts;
    int ok;

    pthread_mutex_lock(&g_gcd_lock);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_gcd_inflight > 0)
        if (pthread_cond_timedwait(&g_gcd_idle, &g_gcd_lock, &ts) != 0) break;
    ok = g_gcd_inflight == 0;
    pthread_mutex_unlock(&g_gcd_lock);
    return ok;
}
static void *g_main_queue[8];
static void *gcd_data_create(const void *buf, size_t len, void *q, void *destructor)
{ (void)buf; (void)len; (void)q; (void)destructor; return NULL; }

/* ------------------------------------------------------------------ dyld
 *
 * A plugin asks dyld which image its own code is in, and builds its bundle from
 * the answer. VSTGUI does exactly that -- `InitMachOLibrary` calls `dladdr` on
 * one of its own functions, walks the path up to the `.vst`, and hands the
 * result to `CFBundleCreate`. With nothing to answer, `gBundleRef` stayed null,
 * `CFBundleCopyResourceURL` found no artwork, the background bitmap was empty,
 * and the editor sized itself to nothing. Twelve of the thirty-one macOS VST2
 * bundles here stop at that one question.
 *
 * There is exactly one image, so the list has one entry. `dladdr` is shadowed
 * deliberately: the host's own knows nothing about a mapping made with mmap,
 * and would answer for whatever ELF happens to sit at that address. A guest
 * address outside the image falls through to the real one, because a plugin
 * asking about a host callback should get the truth about it. */
static char        g_image_path[4096];
static const void *g_image_base;
static size_t      g_image_span;

void macshim_set_dyld_image(const char *binpath, const void *base, size_t span)
{
    snprintf(g_image_path, sizeof g_image_path, "%s", binpath ? binpath : "");
    g_image_base = base;
    g_image_span = span;
}
int macshim_dyld_image_is(const void *base) { return base && base == g_image_base; }

/* Does the loaded image cover `n` bytes at `p`? Asked before reading through a
 * pointer the plugin supplied: anything the plugin can legitimately hand over
 * that this host did not allocate is in its own mapping, and everything else
 * is not worth reading. */
int macshim_dyld_image_contains(const void *p, size_t n)
{
    const uint8_t *a = p, *lo = g_image_base;
    if (!a || !lo || !g_image_span) return 0;
    return a >= lo && a + n <= lo + g_image_span;
}

static uint32_t dyld_image_count(void) { return g_image_base ? 1u : 0u; }
static const char *dyld_get_image_name(uint32_t i)
{ return (i == 0 && g_image_base) ? g_image_path : NULL; }
static const void *dyld_get_image_header(uint32_t i)
{ return i == 0 ? g_image_base : NULL; }
/* The image is mapped where its own load commands ask for, so nothing slid. */
static long dyld_get_image_vmaddr_slide(uint32_t i) { (void)i; return 0; }

typedef struct { const char *dli_fname; void *dli_fbase;
                 const char *dli_sname; void *dli_saddr; } mac_dl_info;

static int mac_dladdr(const void *addr, mac_dl_info *info)
{
    const uint8_t *p = addr;
    if (!info) return 0;
    if (g_image_base && p >= (const uint8_t *)g_image_base &&
        p < (const uint8_t *)g_image_base + g_image_span) {
        info->dli_fname = g_image_path;
        info->dli_fbase = (void *)g_image_base;
        /* No symbol name: this host has no symbolizer, and every caller here
         * wants the file rather than the function. */
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
        return 1;
    }
    {   Dl_info host;
        if (dladdr(addr, &host)) {
            info->dli_fname = host.dli_fname;
            info->dli_fbase = host.dli_fbase;
            info->dli_sname = host.dli_sname;
            info->dli_saddr = host.dli_saddr;
            return 1;
        } }
    return 0;
}

/* --------------------------------------------------------- odds and ends */

static int  sys_gestalt(uint32_t sel, int32_t *out)
{ (void)sel; if (out) *out = 0; return -1; /* gestaltUndefSelectorErr */ }
static double sys_exp10(double x) { return pow(10.0, x); }
static int  sys_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static void sys_assert_rtn(const char *fn, const char *file, int line, const char *expr)
{
    fprintf(stderr, "macho: assertion failed in %s (%s:%d): %s\n",
            fn ? fn : "?", file ? file : "?", line, expr ? expr : "?");
    abort();
}
/* objc classes point their cache at this when empty. */
static void *g_objc_empty_cache[8];
/* No Metal here. NULL is the documented "no device", which a plugin can handle;
 * a fake device pointer would fault later somewhere unrelated. */
static char *dw_realpath(const char *path, char *out)
{ return realpath(path, out); }

/* ------------------------------------------------------- CoreFoundation */

/* These extend the object model in macshim.c. CFRetain/CFRelease there accept
 * anything with the shared header, so objects made here interoperate. */

static double cf_abs_time_now(void)
{
    /* CFAbsoluteTime counts seconds from 2001-01-01. */
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9 - 978307200.0;
}
static uint32_t cf_bundle_version(void *b) { (void)b; return 0; }

/* An immutable dictionary/set is a mutable one that nobody mutates. */

static void *cf_dict_create(void *alloc, const void **keys, const void **vals,
                            long n, const void *kcb, const void *vcb)
{
    void *d;
    long i;
    (void)alloc; (void)kcb; (void)vcb;
    d = macshim_cf_dict_create_mutable_pub(n > 0 ? n : 1);
    if (!d) return NULL;
    for (i = 0; i < n; i++) macshim_cf_dict_set_pub(d, keys[i], vals[i]);
    return d;
}
static void *cf_set_create(void *alloc, const void **vals, long n, const void *cb)
{
    /* Backed by a dictionary mapping each value to itself: the corpus only ever
     * asks a set for membership. */
    void *d;
    long i;
    (void)alloc; (void)cb;
    d = macshim_cf_dict_create_mutable_pub(n > 0 ? n : 1);
    if (!d) return NULL;
    for (i = 0; i < n; i++) macshim_cf_dict_set_pub(d, vals[i], vals[i]);
    return d;
}

/* URLs are only used to name files, so one carries its path as a string. */
static void *cf_url_from_path(void *alloc, void *path, int style, unsigned char isdir)
{ (void)alloc; (void)style; (void)isdir; return path ? (void *)macshim_lookup_retain(path) : NULL; }
static void *cf_url_from_string(void *alloc, void *str, void *base)
{ (void)alloc; (void)base; return str ? (void *)macshim_lookup_retain(str) : NULL; }

/* The run loop exists so a plugin can schedule a repeating timer for its UI.
 *
 * Accepting one and never firing it was not as harmless as it looked. VSTGUI 4
 * does not repaint a control when its value changes -- it marks the view dirty
 * and lets CFrame::idle turn dirty views into invalidated rectangles, and idle
 * is driven by exactly this timer. With it dead, dragging a knob on Tattoo
 * moved the parameter on all sixty frames and repainted the picture on none of
 * them: the sound followed the mouse and the knob sat still. Fired here, the
 * host's pump drives the plugin's idle work the way a run loop would.
 *
 * Fired once per pump rather than on a clock, which is what the NSTimer path
 * beside this does: the host's frame is the only clock here, and gating on wall
 * time would make what an editor draws depend on how long the machine took to
 * get there. A one-shot -- interval zero, which is how deferred work is
 * scheduled -- fires once and retires. */
typedef struct {
    void (*cb)(void *timer, void *info);
    void  *info;
    double interval;
    double due;                 /* frames still to wait */
    int    scheduled, dead;
} cf_timer;

#define MAX_CF_TIMERS 64
static cf_timer g_cf_timers[MAX_CF_TIMERS];
static int      g_ncf_timers;

/* CFRunLoopTimerContext, whose `info` is the argument the callback is given --
 * the struct itself is the caller's and is not kept. */
typedef struct {
    long  version;
    void *info;
    void *(*retain)(const void *);
    void  (*release)(const void *);
    void *copy_description;
} cf_timer_ctx;

static void *g_main_runloop[8];
static void *g_common_modes[8];
static void *g_allocator_default;
static void *cf_runloop_get_main(void) { return g_main_runloop; }
static void *cf_runloop_timer_create(void *alloc, double fire, double interval,
                                     uint32_t flags, long order,
                                     void (*cb)(void *, void *), void *ctx)
{
    cf_timer *t;
    (void)alloc; (void)fire; (void)flags; (void)order;
    if (!cb || g_ncf_timers >= MAX_CF_TIMERS) return g_main_runloop;
    t = &g_cf_timers[g_ncf_timers++];
    t->cb = cb;
    t->info = ctx ? ((const cf_timer_ctx *)ctx)->info : NULL;
    t->interval = interval;
    t->due = 0.0;
    t->scheduled = 0;
    t->dead = 0;
    return t;
}
static int cf_timer_is_ours(const void *p)
{ return p && (const cf_timer *)p >= g_cf_timers
           && (const cf_timer *)p < g_cf_timers + MAX_CF_TIMERS; }
static void cf_runloop_add_timer(void *rl, void *t, void *mode)
{ (void)rl; (void)mode; if (cf_timer_is_ours(t)) ((cf_timer *)t)->scheduled = 1; }
static void cf_runloop_timer_invalidate(void *t)
{ if (cf_timer_is_ours(t)) ((cf_timer *)t)->dead = 1; }

/* One round of whatever the plugin scheduled. Called from macns_fire_timers, so
 * a CFRunLoopTimer and an NSTimer are driven by the same pump. */
void macshim_fire_cf_timers(void)
{
    int i;
    for (i = 0; i < g_ncf_timers; i++) {
        cf_timer *t = &g_cf_timers[i];
        if (t->dead || !t->scheduled || !t->cb) continue;
        if (t->interval > 0.0) {
            /* Counted in frames rather than seconds. The host's pump is the
             * only clock here, and gating on wall time would make what an
             * editor draws depend on how long the machine took to get here --
             * two runs of the same capture would not match. A frame is taken
             * to be a sixtieth, which is what both windows pump at. */
            t->due -= 1.0;
            if (t->due > 0.0) continue;
            t->due += t->interval * 60.0;
            if (t->due < 0.0) t->due = 0.0;
        } else {
            t->dead = 1;                           /* one-shot */
        }
        t->cb(t, t->info);
    }
}

/* HTTP is only ever an update check in this corpus. Failing to create the
 * request is the cleanest outcome: the plugin reports "could not check" and
 * carries on, where a half-working stream would hang it. */
static void *cf_http_request_create(void *a, void *method, void *url, void *ver)
{ (void)a;(void)method;(void)url;(void)ver; return NULL; }
static void *cf_read_stream_for_http(void *a, void *req)
{ (void)a;(void)req; return NULL; }
static unsigned char cf_read_stream_open(void *s) { (void)s; return 0; }
static void cf_read_stream_close(void *s) { (void)s; }
static long cf_read_stream_read(void *s, uint8_t *buf, long n)
{ (void)s;(void)buf;(void)n; return -1; }
static void *g_http_1_1;
static void *g_type_set_cb[8];

/* ------------------------------------------------------------- CoreText */

/* Text shaping is the one part of the GUI path that cannot be faked usefully.
 * The fonts and descriptors themselves live in macquartz.c beside the other
 * CoreText objects; what is left here are the attribute keys, which a plugin
 * puts in a dictionary and never looks inside. */
static void *g_ct_family_name, *g_ct_style_name, *g_ct_font_url;

/* --------------------------------------------------- Foundation data/calls */

static void  *g_NSApp;
static double g_NSAppKitVersion = 2022.0;             /* macOS 12-era AppKit */
static void  *g_NSFilenamesPboardType, *g_NSStringPboardType;
static void  *g_NSImageHintCTM, *g_NSViewFrameDidChangeNotification;

static unsigned char ns_application_load(void) { return 0; }
static void *ns_home_directory(void)
{
    const char *h = getenv("HOME");
    return macshim_cf_string(h ? h : "/tmp");
}
static void *ns_search_paths(unsigned long dir, unsigned long domain, unsigned char expand)
{ (void)dir; (void)domain; (void)expand; return NULL; }
static long ns_run_alert_panel(void *title, void *msg, void *a, void *b, void *c, ...)
{
    (void)title; (void)a; (void)b; (void)c;
    fprintf(stderr, "macho: the plugin tried to show an alert panel\n");
    return 1;                                          /* NSAlertDefaultReturn */
}

static void __attribute__((constructor)) init_found_constants(void)
{
    g_NSFilenamesPboardType = macshim_cf_string("NSFilenamesPboardType");
    g_NSStringPboardType    = macshim_cf_string("NSStringPboardType");
    g_NSImageHintCTM        = macshim_cf_string("NSImageHintCTM");
    g_NSViewFrameDidChangeNotification =
        macshim_cf_string("NSViewFrameDidChangeNotification");
    g_ct_family_name = macshim_cf_string("NSFontFamilyAttribute");
    g_ct_style_name  = macshim_cf_string("NSFontFaceAttribute");
    g_ct_font_url    = macshim_cf_string("NSCTFontFileURLAttribute");
    g_http_1_1       = macshim_cf_string("HTTP/1.1");
}

/* ------------------------------------------------------------------ table */

const macshim_entry macshim_foundation[] = {
    /* Darwin file-system variants -- translated, not forwarded */
    { "_stat$INODE64",    dw_stat },
    { "_lstat$INODE64",   dw_lstat },
    { "_fstat$INODE64",   dw_fstat },
    { "_fstatfs$INODE64", dw_fstatfs },
    { "_opendir$INODE64", dw_opendir },
    { "_readdir$INODE64", dw_readdir },
    { "_closedir",        dw_closedir },
    { "_realpath$DARWIN_EXTSN", dw_realpath },

    /* blocks */
    { "__Block_object_assign",  bl_object_assign },
    { "__Block_object_dispose", bl_object_dispose },
    { "__NSConcreteStackBlock", g_concrete_stack_block },
    { "__NSConcreteMallocBlock", g_concrete_malloc_block },
    { "__NSConcreteGlobalBlock", g_concrete_malloc_block },
    { "__Block_copy",           block_copy },
    { "__Block_release",        block_release },

    /* GCD */
    { "_dispatch_semaphore_create", gcd_sem_create },
    { "_dispatch_semaphore_signal", gcd_sem_signal },
    { "_dispatch_semaphore_wait",   gcd_sem_wait },
    { "_dispatch_async",            gcd_async },
    { "_dispatch_data_create",      gcd_data_create },
    { "__dispatch_main_q",          g_main_queue },

    /* odds and ends */
    { "_Gestalt",        sys_gestalt },
    { "___exp10",        sys_exp10 },
    { "___tolower",      sys_tolower },
    { "___assert_rtn",   sys_assert_rtn },
    { "__objc_empty_cache", g_objc_empty_cache },

    /* CoreFoundation additions */
    { "_CFAbsoluteTimeGetCurrent", cf_abs_time_now },
    { "_CFBundleGetVersionNumber", cf_bundle_version },
    { "_CFDictionaryCreate",       cf_dict_create },
    { "_CFSetCreate",              cf_set_create },
    { "_CFURLCreateWithFileSystemPath", cf_url_from_path },
    { "_CFURLCreateWithString",    cf_url_from_string },
    { "_CFRunLoopGetMain",         cf_runloop_get_main },
    /* One run loop here, so current and main are the same object -- and a
     * plugin that schedules on the one it is handed then schedules on the one
     * that is actually run. */
    { "_CFRunLoopGetCurrent",      cf_runloop_get_main },
    { "_CFRunLoopTimerCreate",     cf_runloop_timer_create },
    { "_CFRunLoopAddTimer",        cf_runloop_add_timer },
    { "_CFRunLoopTimerInvalidate", cf_runloop_timer_invalidate },
    { "_CFHTTPMessageCreateRequest",     cf_http_request_create },
    { "_CFReadStreamCreateForHTTPRequest", cf_read_stream_for_http },
    { "_CFReadStreamOpen",  cf_read_stream_open },
    { "_CFReadStreamClose", cf_read_stream_close },
    { "_CFReadStreamRead",  cf_read_stream_read },
    { "_kCFAllocatorDefault",     &g_allocator_default },
    { "_kCFRunLoopCommonModes",   &g_common_modes },
    { "_kCFTypeSetCallBacks",     g_type_set_cb },
    { "_kCFHTTPVersion1_1",       &g_http_1_1 },

    /* CoreText -- present so the image binds; no shaping yet */
    { "_kCTFontFamilyNameAttribute", &g_ct_family_name },
    { "_kCTFontStyleNameAttribute",  &g_ct_style_name },
    { "_kCTFontURLAttribute",        &g_ct_font_url },

    /* Foundation */
    { "_NSApp",                 &g_NSApp },
    { "_NSAppKitVersionNumber", &g_NSAppKitVersion },
    { "_NSApplicationLoad",     ns_application_load },
    { "_NSHomeDirectory",       ns_home_directory },
    { "_NSSearchPathForDirectoriesInDomains", ns_search_paths },
    { "_NSRunAlertPanel",       ns_run_alert_panel },
    { "_NSFilenamesPboardType", &g_NSFilenamesPboardType },
    { "_NSStringPboardType",    &g_NSStringPboardType },
    { "_NSImageHintCTM",        &g_NSImageHintCTM },
    { "_NSViewFrameDidChangeNotification", &g_NSViewFrameDidChangeNotification },
    { NULL, NULL }
};

/* ------------------------------------------------------- mach and the rest */

/* mach_absolute_time counts in units the caller converts with mach_timebase_info.
 * Reporting a 1:1 timebase and nanoseconds is self-consistent and exact. */
static uint64_t mach_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
typedef struct { uint32_t numer, denom; } mach_timebase;
static int mach_timebase_info_fn(mach_timebase *info)
{ if (info) { info->numer = 1; info->denom = 1; } return 0; }

/* Carbon Component Manager instance storage, which an older AU uses to hang its
 * state off the instance the host passed it. A small table is all that needs. */
#define MAX_CI 64
static struct { void *ci, *storage; } g_ci[MAX_CI];
static pthread_mutex_t g_ci_lock = PTHREAD_MUTEX_INITIALIZER;

static void *get_component_instance_storage(void *ci)
{
    void *r = NULL;
    int i;
    pthread_mutex_lock(&g_ci_lock);
    for (i = 0; i < MAX_CI; i++) if (g_ci[i].ci == ci) { r = g_ci[i].storage; break; }
    pthread_mutex_unlock(&g_ci_lock);
    return r;
}
static int set_component_instance_storage(void *ci, void *storage)
{
    int i, slot = -1;
    pthread_mutex_lock(&g_ci_lock);
    for (i = 0; i < MAX_CI; i++) if (g_ci[i].ci == ci) { slot = i; break; }
    if (slot < 0) for (i = 0; i < MAX_CI; i++) if (!g_ci[i].ci) { slot = i; break; }
    if (slot >= 0) { g_ci[slot].ci = ci; g_ci[slot].storage = storage; }
    pthread_mutex_unlock(&g_ci_lock);
    return slot >= 0 ? 0 : -1;
}

/* CoreMIDI packet lists. The layout is ABI and the building is trivial, so this
 * is real -- a plugin assembling MIDI output should get a valid list. */
typedef struct { uint64_t timeStamp; uint16_t length; uint8_t data[256]; } MIDIPacket;
typedef struct { uint32_t numPackets; MIDIPacket packet[1]; } MIDIPacketList;

static void *midi_packet_list_init(MIDIPacketList *pl)
{
    if (!pl) return NULL;
    pl->numPackets = 0;
    return &pl->packet[0];
}
static void *midi_packet_list_add(MIDIPacketList *pl, size_t listsize,
                                  MIDIPacket *cur, uint64_t ts,
                                  size_t nbytes, const uint8_t *data)
{
    if (!pl || !cur || !data || nbytes > sizeof cur->data) return NULL;
    if ((size_t)((uint8_t *)cur - (uint8_t *)pl) + sizeof *cur > listsize) return NULL;
    cur->timeStamp = ts;
    cur->length = (uint16_t)nbytes;
    memcpy(cur->data, data, nbytes);
    pl->numPackets++;
    /* Packets are packed to their actual length, aligned to 4. */
    { size_t adv = offsetof(MIDIPacket, data) + ((nbytes + 3) & ~(size_t)3);
      return (uint8_t *)cur + adv; }
}

/* The pre-dlopen dyld API. Nothing new is ever loaded here, so a lookup finds
 * nothing -- which a plugin treats as "symbol absent" and handles. */
static void *ns_add_image(const char *path, uint32_t opts) { (void)path; (void)opts; return NULL; }
static void *ns_lookup_symbol_in_image(void *img, const char *sym, uint32_t opts)
{ (void)img; (void)sym; (void)opts; return NULL; }
static void *ns_address_of_symbol(void *sym) { (void)sym; return NULL; }

static int32_t osatomic_add32(int32_t amount, volatile int32_t *v)
{ return __atomic_add_fetch(v, amount, __ATOMIC_SEQ_CST); }
static int sys_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int sys_sysctl(int *name, unsigned nlen, void *old, size_t *oldlen,
                      void *nw, size_t nlen2)
{ (void)name; (void)nlen; (void)old; (void)nw; (void)nlen2;
  if (oldlen) *oldlen = 0; return -1; }
static int pt_cond_timedwait_rel(void *cond, void *mutex, const struct timespec *rel)
{ (void)cond; (void)mutex; (void)rel; return 0; }

const macshim_entry macshim_mach[] = {
    { "_mach_absolute_time",   mach_now },
    { "_mach_timebase_info",   mach_timebase_info_fn },
    { "_GetComponentInstanceStorage", get_component_instance_storage },
    { "_SetComponentInstanceStorage", set_component_instance_storage },
    { "_MIDIPacketListInit",   midi_packet_list_init },
    { "_MIDIPacketListAdd",    midi_packet_list_add },
    /* dyld, so a plugin can find the image its own code is in. */
    { "__dyld_image_count",    dyld_image_count },
    { "__dyld_get_image_name", dyld_get_image_name },
    { "__dyld_get_image_header", dyld_get_image_header },
    { "__dyld_get_image_vmaddr_slide", dyld_get_image_vmaddr_slide },
    { "_dladdr",               mac_dladdr },
    { "_NSAddImage",           ns_add_image },
    { "_NSLookupSymbolInImage", ns_lookup_symbol_in_image },
    { "_NSAddressOfSymbol",    ns_address_of_symbol },
    { "_OSAtomicAdd32Barrier", osatomic_add32 },
    { "___toupper",            sys_toupper },
    { "_sysctl",               sys_sysctl },
    { "_pthread_cond_timedwait_relative_np", pt_cond_timedwait_rel },
    { NULL, NULL }
};

/* ------------------------------------------------------------ file tracing */

/* Which files a plugin opens, and whether it succeeded. Set MACHO_TRACE_FILES=1.
 *
 * This exists because guessing was not working: three plugins render silence,
 * and two plausible theories about resource loading were both wrong. The shim
 * table is consulted before the host libraries, so interposing here shows what
 * the plugin actually asks the filesystem for. */
static int trace_files(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("MACHO_TRACE_FILES"); v = e && *e != '0'; } return v; }

/* Resolve a relative path against the loaded bundle's parent directory.
 *
 * A plugin builds paths to its own resources as "<Name>.vst/Contents/Resources/x"
 * and relies on the working directory being the folder its bundle sits in. That
 * holds when a host launches from there and not otherwise, so the same lookup
 * fails here with no error the plugin reports -- it simply has no wavetables and
 * renders silence. Retrying relative to the bundle's parent gives it exactly the
 * view it expects, and only ever applies after a plain open has already failed.
 */
static int bundle_relative(const char *path, char *out, size_t n)
{
    const char *bp = macshim_bundle_path();
    const char *slash;
    size_t dirlen;

    if (!path || path[0] == '/' || !bp || !bp[0]) return 0;
    if (!(slash = strrchr(bp, '/'))) return 0;
    dirlen = (size_t)(slash - bp);
    if (dirlen + 1 + strlen(path) + 1 > n) return 0;
    memcpy(out, bp, dirlen);
    out[dirlen] = '/';
    snprintf(out + dirlen + 1, n - dirlen - 1, "%s", path);
    return 1;
}

static void *tf_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    char alt[4600];
    int retried = 0;

    if (!f && bundle_relative(path, alt, sizeof alt)) {
        f = fopen(alt, mode);
        retried = 1;
    }
    if (trace_files())
        fprintf(stderr, "  [file] fopen(\"%s\", \"%s\") -> %s%s\n",
                path ? path : "(null)", mode ? mode : "?",
                f ? "ok" : "FAILED", retried ? " (via the bundle)" : "");
    return f;
}
static int tf_open(const char *path, int flags, ...)
{
    /* The variadic mode argument only matters when creating, which a plugin
     * reading its resources does not do -- but pass it through regardless. */
    va_list ap;
    int mode = 0, fd;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    fd = open(path, flags, (mode_t)mode);
    if (fd < 0) {
        char alt[4600];
        if (bundle_relative(path, alt, sizeof alt)) fd = open(alt, flags, (mode_t)mode);
    }
    if (trace_files())
        fprintf(stderr, "  [file] open(\"%s\", 0x%x) -> %s\n",
                path ? path : "(null)", flags, fd >= 0 ? "ok" : "FAILED");
    return fd;
}

/* ------------------------------------------- Carbon File Manager (FSRef) */

/* Enough of the FSRef API for a plugin to find and read its own resources.
 *
 * These were stubs, and stubs are worse here than absence: every one of them
 * reports through a caller buffer and returns an OSStatus, so returning 0 told
 * the plugin noErr while leaving its FSRef whatever the stack held. Audio
 * Damage's editors then opened a fork on that garbage and took the process with
 * them -- the SIGBUS that made three of them look like they crashed on load.
 *
 * An FSRef is documented as 80 opaque bytes and both ends of it are ours, so it
 * used to carry the path outright, which made the whole family ordinary POSIX
 * calls and removed any need for a table. Eighty bytes is not a path, though:
 * a resource inside a bundle in this corpus is comfortably over that, and a
 * truncated one opens nothing. So the eighty bytes carry a magic word and an
 * index instead, and the paths live beside them. */
#define FSREF_MAGIC 0x46537265u          /* 'FSre' */
#define MAX_FSREF   128

typedef struct { uint32_t magic; int32_t slot; uint8_t pad[72]; } fs_ref;
enum { fsErr_none = 0, fsErr_notFound = -43, fsErr_param = -50, fsErr_io = -36 };

/* HFSUniStr255: a length word followed by UTF-16. */
typedef struct { uint16_t length; uint16_t unicode[255]; } hfs_uni_str;

/* The paths the refs above point at. A ref is handed out and kept by the
 * plugin for as long as it likes, so slots are not recycled while any could
 * still be live; the table is small and a plugin makes a handful. */
static struct { int used; char path[4096]; } g_fsrefs[MAX_FSREF];
static pthread_mutex_t g_fsref_lock = PTHREAD_MUTEX_INITIALIZER;

static void fs_set(fs_ref *r, const char *p)
{
    int i;
    if (!r) return;
    memset(r, 0, sizeof *r);
    if (!p || !*p) return;
    pthread_mutex_lock(&g_fsref_lock);
    for (i = 0; i < MAX_FSREF; i++) if (!g_fsrefs[i].used) break;
    if (i == MAX_FSREF) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [fs] FSRef table full at %d -- refs are being "
                            "recycled, and a stale one names the wrong file\n",
                    MAX_FSREF);
        }
        i = 0;
    }
    g_fsrefs[i].used = 1;
    snprintf(g_fsrefs[i].path, sizeof g_fsrefs[i].path, "%s", p);
    pthread_mutex_unlock(&g_fsref_lock);
    r->magic = FSREF_MAGIC;
    r->slot = i;
}

/* The path a ref names, or NULL for anything that is not one of ours -- a
 * plugin hands over stack garbage as an FSRef often enough that the magic word
 * is the whole point. Read under the same lock that fills the table, because a
 * dispatched block can be opening a file on a thread of its own. */
static const char *fs_path(const fs_ref *r)
{
    const char *p = NULL;
    if (!r || r->magic != FSREF_MAGIC) return NULL;
    if (r->slot < 0 || r->slot >= MAX_FSREF) return NULL;
    pthread_mutex_lock(&g_fsref_lock);
    if (g_fsrefs[r->slot].used) p = g_fsrefs[r->slot].path;
    pthread_mutex_unlock(&g_fsref_lock);
    return p;
}

static int fs_ok(const fs_ref *r) { return fs_path(r) != NULL; }

static int32_t fs_find_folder(int16_t vol, uint32_t type, uint8_t create, fs_ref *out)
{
    const char *home = getenv("HOME");
    char buf[80];
    (void)vol;
    if (!out) return fsErr_param;
    if (!home || !*home) home = "/tmp";
    /* Only the folders a plugin actually asks for on the way to its own
     * settings. Anything else is refused rather than pointed somewhere wrong. */
    switch (type) {
    case 0x70726566: /* 'pref' -- Preferences        */
        snprintf(buf, sizeof buf, "%s/Library/Preferences", home); break;
    case 0x61737570: /* 'asup' -- Application Support */
        snprintf(buf, sizeof buf, "%s/Library/Application Support", home); break;
    case 0x646f6320: /* 'doc ' -- Documents          */
        snprintf(buf, sizeof buf, "%s/Documents", home); break;
    case 0x666f6e74: /* 'font' -- Fonts              */
        snprintf(buf, sizeof buf, "%s/Library/Fonts", home); break;
    case 0x6c696272: /* 'libr' -- Library            */
        snprintf(buf, sizeof buf, "%s/Library", home); break;
    default:
        return fsErr_notFound;
    }
    if (create) { char c[128]; snprintf(c, sizeof c, "%s", buf); (void)mkdir(c, 0755); }
    fs_set(out, buf);
    return access(buf, F_OK) == 0 ? fsErr_none : fsErr_notFound;
}

static int32_t fs_make_ref_unicode(const fs_ref *parent, int32_t nlen,
                                   const uint16_t *name, uint32_t enc, fs_ref *out)
{
    char leaf[64];
    int i, n = (int)nlen;
    (void)enc;
    if (!out || !fs_ok(parent) || !name) return fsErr_param;
    if (n < 0) n = 0;
    if (n > (int)sizeof leaf - 1) n = (int)sizeof leaf - 1;
    /* UTF-16 down to bytes. A resource name outside Latin-1 would be mangled,
     * which is worth knowing but not worth a converter here. */
    for (i = 0; i < n; i++) leaf[i] = (char)(name[i] & 0xff);
    leaf[n] = '\0';
    { char full[160];
      snprintf(full, sizeof full, "%s/%s", fs_path(parent), leaf);
      fs_set(out, full); }
    return access(fs_path(out), F_OK) == 0 ? fsErr_none : fsErr_notFound;
}

/* The data fork has no name, which is exactly what this reports. */
static int32_t fs_get_data_fork_name(hfs_uni_str *out)
{ if (!out) return fsErr_param; out->length = 0; return fsErr_none; }

/* Only the fields a resource loader reads: whether it is a directory and how
 * big the data fork is. The rest of FSCatalogInfo is left zeroed, which is the
 * honest answer for information this has not got. */
static int32_t fs_get_catalog_info(const fs_ref *ref, uint64_t whichInfo,
                                   void *info, hfs_uni_str *outName,
                                   void *fsSpec, fs_ref *parent)
{
    struct stat st;
    (void)whichInfo; (void)fsSpec;
    if (!fs_ok(ref)) return fsErr_param;
    if (stat(fs_path(ref), &st)) return fsErr_notFound;
    if (info) {
        /* nodeFlags is the first field; bit 4 marks a directory. */
        unsigned char *ci = info;
        memset(ci, 0, 144);
        if (S_ISDIR(st.st_mode)) ci[1] |= 0x10;
        /* dataLogicalSize sits at offset 88 in FSCatalogInfo. */
        { uint64_t sz = (uint64_t)st.st_size; memcpy(ci + 88, &sz, sizeof sz); }
    }
    if (outName) {
        const char *slash = strrchr(fs_path(ref), '/');
        const char *leaf = slash ? slash + 1 : fs_path(ref);
        int i, n = (int)strlen(leaf);
        if (n > 255) n = 255;
        for (i = 0; i < n; i++) outName->unicode[i] = (unsigned char)leaf[i];
        outName->length = (uint16_t)n;
    }
    if (parent) {
        char up[80];
        const char *slash;
        snprintf(up, sizeof up, "%s", fs_path(ref));
        if ((slash = strrchr(up, '/')) && slash != up) *(char *)slash = '\0';
        fs_set(parent, up);
    }
    return fsErr_none;
}

/* Fork refnums are plain descriptors offset so that 0 is never handed out --
 * Carbon treats 0 as a valid refnum but a plugin testing one for truth would
 * read a real fork as closed. */
#define FS_FORK_BIAS 1000

static int32_t fs_open_fork(const fs_ref *ref, int32_t nameLen, const uint16_t *name,
                            int8_t perms, int16_t *refnum)
{
    int fd, fl;
    (void)nameLen; (void)name;
    if (!fs_ok(ref) || !refnum) return fsErr_param;
    /* fsRdPerm 1, fsWrPerm 2, fsRdWrPerm 3. */
    fl = (perms & 2) ? ((perms & 1) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if ((fd = open(fs_path(ref), fl)) < 0) return fsErr_notFound;
    *refnum = (int16_t)(fd + FS_FORK_BIAS);
    return fsErr_none;
}

static int32_t fs_read_fork(int16_t refnum, uint16_t posMode, int64_t posOff,
                            uint32_t count, void *buf, uint32_t *actual)
{
    int fd = refnum - FS_FORK_BIAS;
    ssize_t got;
    if (actual) *actual = 0;
    if (fd < 0 || !buf) return fsErr_param;
    /* fsAtMark 0, fsFromStart 1, fsFromLEOF 2, fsFromMark 3. */
    switch (posMode & 3) {
    case 1: if (lseek(fd, (off_t)posOff, SEEK_SET) < 0) return fsErr_io; break;
    case 2: if (lseek(fd, (off_t)posOff, SEEK_END) < 0) return fsErr_io; break;
    case 3: if (lseek(fd, (off_t)posOff, SEEK_CUR) < 0) return fsErr_io; break;
    default: break;
    }
    if ((got = read(fd, buf, count)) < 0) return fsErr_io;
    if (actual) *actual = (uint32_t)got;
    /* eofErr, which is how a reader knows to stop. */
    return (uint32_t)got < count ? -39 : fsErr_none;
}

/* FSGetForkSize(refnum, SInt64 *out). A plugin reading a fork whole asks how
 * big it is first, allocates, and reads -- so without this it allocated
 * nothing and the read had nowhere to go. */
static int32_t fs_get_fork_size(int16_t refnum, int64_t *out)
{
    int fd = refnum - FS_FORK_BIAS;
    off_t here, end;
    if (out) *out = 0;
    if (fd < 0) return fsErr_param;
    here = lseek(fd, 0, SEEK_CUR);
    end  = lseek(fd, 0, SEEK_END);
    lseek(fd, here, SEEK_SET);
    if (end < 0) return fsErr_io;
    if (out) *out = (int64_t)end;
    return fsErr_none;
}

/* CFURLGetFSRef(url, FSRef *out) -> Boolean, and its inverse.
 *
 * This is the door into everything above, and it was shut. Audio Damage's
 * editors keep their bitmap font's glyph table in a file they reach this way,
 * and with the call answering false the whole load was skipped -- leaving a
 * field the constructor never zeroes holding whatever the allocator had left
 * there, which the first call to getCharacterInfo dereferenced. Seven of the
 * twelve VSTGUI editors here died on it, in a font routine, a long way from
 * the question that had gone unanswered. */
static unsigned char fs_url_get_ref(void *url, fs_ref *out)
{
    char path[4096];
    if (!out || !url || !macshim_cf_string_get(url, path, sizeof path)) return 0;
    if (access(path, F_OK) != 0) return 0;
    fs_set(out, path);
    return 1;
}
static void *fs_url_from_ref(void *alloc, const fs_ref *ref)
{
    const char *p = fs_path(ref);
    (void)alloc;
    return p ? macshim_cf_string(p) : NULL;
}

static int32_t fs_close_fork(int16_t refnum)
{
    int fd = refnum - FS_FORK_BIAS;
    if (fd < 0) return fsErr_param;
    return close(fd) ? fsErr_io : fsErr_none;
}

/* Ticks since boot at the classic 60.15 Hz. Used for timing and for seeding,
 * so it has to advance -- a constant makes an animation stand still. */
static uint32_t fs_tick_count(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 60 + (uint64_t)ts.tv_nsec / 16666667ull);
}

const macshim_entry macshim_files[] = {
    { "_fopen",            tf_fopen },
    { "_fopen$UNIX2003",   tf_fopen },
    { "_open",             tf_open },
    { "_open$UNIX2003",    tf_open },
    { "_FSFindFolder",       fs_find_folder },
    { "_FSMakeFSRefUnicode", fs_make_ref_unicode },
    { "_FSGetDataForkName",  fs_get_data_fork_name },
    { "_FSGetCatalogInfo",   fs_get_catalog_info },
    { "_FSOpenFork",         fs_open_fork },
    { "_FSGetForkSize",      fs_get_fork_size },
    { "_FSReadFork",         fs_read_fork },
    { "_FSCloseFork",        fs_close_fork },
    { "_CFURLGetFSRef",      fs_url_get_ref },
    { "_CFURLCreateFromFSRef", fs_url_from_ref },
    { "_TickCount",          fs_tick_count },
    { NULL, NULL }
};
