/* Native implementations of the Win32 surface a headless VST2 plugin touches.
 *
 * Everything here is __attribute__((ms_abi)) so the compiler emits Microsoft
 * x64 entry points -- shadow space, RCX/RDX/R8/R9 argument slots, and
 * RSI/RDI/XMM6-15 preservation -- while the bodies are ordinary SysV C that
 * can call glibc freely. That attribute is what removes the need for any
 * hand-written thunk.
 *
 * Anything not listed in g_stubs[] gets a generated tracking stub from
 * pehost.c instead, so a run reports precisely which symbols were reached.
 * Implement to the measurement, not to the import table. */
#ifndef PELOAD_WINSTUBS_H
#define PELOAD_WINSTUBS_H

#include <ctype.h>
#include <unistd.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <fenv.h>
#include <fnmatch.h>
#include <dirent.h>
#include <malloc.h>
#include <wctype.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/vfs.h>

typedef struct { const char *dll, *sym; void *fn; } winstub;

/* The C runtime is __cdecl at both widths, unlike the stdcall Win32 surface,
 * so its stubs cannot use MS: on i386 a stdcall callee pops its own arguments
 * and would unbalance the caller's stack on every call. */
#ifdef __i386__
#define MSCRT
#else
#define MSCRT MS
#endif

/* MSVC's C++ *member* functions are __thiscall on i386: `this` arrives in ECX
 * and only the remaining arguments are on the stack. Declaring them stdcall
 * like the rest of the Win32 surface shifts every argument along by one, so
 * exception(char const *const &, int) read the int as the message pointer and
 * dereferenced 1 -- which is what took all four 32-bit NI plug-ins down inside
 * a static initialiser. At 64-bit there is one convention and this is MS. */
#ifdef __i386__
#define MSTHIS __attribute__((thiscall))
#else
#define MSTHIS MS
#endif

#include "mscxxeh.h"

#define PLOG(...) do { if (pe_verbose()) fprintf(stderr, __VA_ARGS__); } while (0)

/* More than one PE image can be mapped at once: the plugin, plus any real
 * runtime DLL side-loaded to satisfy its imports. Resources are per-image -- an
 * RVA means nothing without knowing which base it is relative to -- so a base
 * and its resource directory have to travel together, and a second image must
 * not overwrite the first's.
 *
 * Doing exactly that made Absynth's own resources invisible: side-loading
 * msvcp120.dll replaced the plugin's resource base with the runtime's, and a
 * stock Microsoft DLL carries nothing but a version resource. Every lookup
 * afterwards searched a directory holding one entry. */
#define W32_MAX_IMAGES 16
typedef struct { uint8_t *base, *rsrc; } w32_image;
static w32_image g_images[W32_MAX_IMAGES];
static uint8_t *g_image_base;   /* the plugin: what a NULL module handle means */
static uint8_t *g_rsrc;         /* its resource directory */

static void winstubs_add_image(void *base, void *rsrc)
{
    int i;
    for (i = 0; i < W32_MAX_IMAGES; i++) {
        if (g_images[i].base == (uint8_t *)base) { g_images[i].rsrc = rsrc; return; }
        if (!g_images[i].base) {
            g_images[i].base = base; g_images[i].rsrc = rsrc; return;
        }
    }
    /* Said out loud. Running off the end of this loop used to be silent, and a
     * silent failure here does not look like a failure here: the image simply
     * never gets registered, and every resource lookup it makes afterwards is
     * answered from whichever image the table still holds. */
    PLOG("  [w32] image table full (%d) -- %p not registered, its resources "
         "will resolve against another image\n", W32_MAX_IMAGES, base);
}

/* Forget an image that has been unmapped.
 *
 * The table only ever gained entries. Nothing removed them when a plug-in was
 * closed, so eight plug-ins into a browsing session it was full of bases that
 * no longer existed, the ninth plug-in's image was never registered, and its
 * resource lookups resolved against a predecessor's directory in memory that
 * had been handed back to the kernel. From the outside that is an editor that
 * gradually stops responding -- a control whose bitmap comes back empty draws
 * but does not behave -- and then a fault. Switching between nine plug-ins was
 * enough to reach it every time. */
static int winstubs_drop_image(void *base)
{
    int i, primary = 0;
    if (!base) return 0;
    for (i = 0; i < W32_MAX_IMAGES; i++)
        if (g_images[i].base == (uint8_t *)base) {
            g_images[i].base = NULL;
            g_images[i].rsrc = NULL;
        }
    if (g_image_base == (uint8_t *)base) {
        g_image_base = NULL; g_rsrc = NULL; primary = 1;
    }
    return primary;             /* the caller reclaims the plug-in's TLS */
}
static void winstubs_init(void *base, void *rsrc)
{ g_image_base = base; g_rsrc = rsrc; winstubs_add_image(base, rsrc); }

/* Used by the side-load path to put the plugin back after a supporting module
 * has registered itself. */
static void winstubs_primary_save(void **base, void **rsrc)
{ *base = g_image_base; *rsrc = g_rsrc; }
static void winstubs_primary_restore(void *base, void *rsrc)
{ if (base) { g_image_base = base; g_rsrc = rsrc; } }

/* A module handle names which image to resolve against; NULL means the plugin,
 * as it does on Windows for the calling module. */
static uint8_t *image_rsrc(void *mod)
{
    int i;
    if (!mod) return g_rsrc;
    for (i = 0; i < W32_MAX_IMAGES; i++)
        if (g_images[i].base == (uint8_t *)mod) return g_images[i].rsrc;
    return g_rsrc;
}
/* For the calls that are handed a resource pointer with no module alongside it
 * (LockResource takes only the handle), the owning image is the one whose
 * resource directory starts closest below it. */
static uint8_t *image_base_for_rsrc(const void *p)
{
    int i, best = -1;
    for (i = 0; i < W32_MAX_IMAGES; i++) {
        if (!g_images[i].rsrc || (const uint8_t *)p < g_images[i].rsrc) continue;
        if (best < 0 || g_images[i].rsrc > g_images[best].rsrc) best = i;
    }
    return best >= 0 ? g_images[best].base : g_image_base;
}

static __thread uint32_t g_last_error;

/* UTF-16 to narrow, defined with the file helpers below. */
static void w2c(const uint16_t *w, char *out, size_t n);

/* ------------------------------------------------------------ heap / memory */

/* ---- the allocator every plug-in-facing malloc goes through -------------
 *
 * A plug-in frees things. Sometimes it frees something it never got from us, or
 * frees it twice, and until now that reached glibc's free() directly: a wild
 * pointer took the host down, and a plausible-but-wrong one quietly corrupted
 * the heap. The second is much worse. It cost a long chase -- an MFC editor
 * faulting inside its own CMapPtrToPtr::Lookup on a bucket array full of small
 * integers, several subsystems away from whatever had scribbled on it.
 *
 * So every block handed to a plug-in is recorded, and every free asks whether
 * this is one of them.
 *
 * A header in front of the block was the obvious way to do that and is wrong:
 * validating a foreign pointer then means reading the sixteen bytes before it,
 * which is out of bounds by definition. ASan says so immediately, and a pointer
 * that happens to sit at the start of a page says so with the fault this exists
 * to prevent. A side table never touches memory it does not own, and it keeps
 * the pointers we hand out as ordinary malloc pointers -- so host code that
 * frees one directly still works.
 *
 * All the families share the table deliberately. On Windows malloc *is*
 * HeapAlloc on the process heap, so a plug-in may legally allocate with one and
 * free with another; separate bookkeeping per family would break exactly the
 * mixing that works on the real thing. The same goes for every stub that hands
 * back a buffer the caller frees -- FormatMessage, _wcsdup, _Getdays -- which
 * must allocate here rather than with malloc.
 *
 * Removal happens before the free, which is what catches a double free: the
 * second one no longer finds the pointer. Refusing it leaks rather than
 * corrupting, and a leak is something a host survives. */

#define W32_HEAP_MIN 1024u
#define W32_HEAP_DEAD ((void *)(uintptr_t)1)     /* tombstone */

static struct {
    void   **key;
    size_t  *size;
    size_t   cap, used, dead;
    pthread_mutex_t m;
    int      ready;
} g_heap;

/* Refused frees, counted rather than reported: each one is logged under
 * PELOAD_VERBOSE, and a plug-in that does this does it thousands of times. */
static long g_alloc_foreign;

static size_t w32_heap_hash(const void *p, size_t cap)
{
    /* The low four bits of a malloc pointer carry no information; mix the rest
     * so a run of same-sized allocations does not walk one probe chain. */
    uintptr_t v = (uintptr_t)p >> 4;
    v ^= v >> 13;
    v *= 0x9E3779B1u;
    return (size_t)(v ^ (v >> 15)) & (cap - 1);
}

/* Insert into a table known to have room. Used by both the public insert and
 * the rehash, which is why it takes the arrays rather than reading the global. */
static void w32_heap_put(void **key, size_t *size, size_t cap,
                         void *p, size_t n)
{
    size_t i = w32_heap_hash(p, cap);
    while (key[i] && key[i] != W32_HEAP_DEAD && key[i] != p)
        i = (i + 1) & (cap - 1);
    key[i] = p;
    size[i] = n;
}

static int w32_heap_grow(void)
{
    size_t cap = g_heap.cap ? g_heap.cap * 2 : W32_HEAP_MIN;
    void **key;
    size_t *size, i;

    /* Tombstones alone can fill a table; rehashing at the same capacity clears
     * them, which is why the new capacity is chosen from the live count. */
    while (cap > W32_HEAP_MIN && g_heap.used * 4 < cap) cap /= 2;
    if (!(key = (void **)calloc(cap, sizeof *key))) return 0;
    if (!(size = (size_t *)calloc(cap, sizeof *size))) { free(key); return 0; }
    for (i = 0; i < g_heap.cap; i++)
        if (g_heap.key[i] && g_heap.key[i] != W32_HEAP_DEAD)
            w32_heap_put(key, size, cap, g_heap.key[i], g_heap.size[i]);
    free(g_heap.key);
    free(g_heap.size);
    g_heap.key = key; g_heap.size = size; g_heap.cap = cap; g_heap.dead = 0;
    return 1;
}

static void w32_heap_lock(void)
{
    if (!g_heap.ready) {                  /* first call, before any thread */
        pthread_mutex_init(&g_heap.m, NULL);
        g_heap.ready = 1;
    }
    pthread_mutex_lock(&g_heap.m);
}
static void w32_heap_unlock(void) { pthread_mutex_unlock(&g_heap.m); }

static void w32_heap_note(void *p, size_t n)
{
    if (!p) return;
    w32_heap_lock();
    if ((g_heap.used + g_heap.dead + 1) * 4 >= g_heap.cap * 3) {
        if (!w32_heap_grow()) { w32_heap_unlock(); return; }
    }
    w32_heap_put(g_heap.key, g_heap.size, g_heap.cap, p, n);
    g_heap.used++;
    w32_heap_unlock();
}

/* Find and remove. Returns the recorded size, or (size_t)-1 when the pointer is
 * not ours -- which a caller must distinguish from a genuine zero-sized block. */
static size_t w32_heap_take(void *p)
{
    size_t i, n;

    if (!p || !g_heap.cap) return (size_t)-1;
    w32_heap_lock();
    i = w32_heap_hash(p, g_heap.cap);
    while (g_heap.key[i]) {
        if (g_heap.key[i] == p) {
            n = g_heap.size[i];
            g_heap.key[i] = W32_HEAP_DEAD;
            g_heap.used--; g_heap.dead++;
            w32_heap_unlock();
            return n;
        }
        i = (i + 1) & (g_heap.cap - 1);
    }
    w32_heap_unlock();
    return (size_t)-1;
}

static size_t w32_heap_peek(const void *p)
{
    size_t i;

    if (!p || !g_heap.cap) return (size_t)-1;
    w32_heap_lock();
    i = w32_heap_hash(p, g_heap.cap);
    while (g_heap.key[i]) {
        if (g_heap.key[i] == p) {
            size_t n = g_heap.size[i];
            w32_heap_unlock();
            return n;
        }
        i = (i + 1) & (g_heap.cap - 1);
    }
    w32_heap_unlock();
    return (size_t)-1;
}

/* PELOAD_GUARD_HEAP: put every plug-in allocation against an unmapped page.
 *
 * The checked heap above stops a bad free from corrupting anything. It does
 * nothing about the other direction -- a plug-in writing past the end of a
 * block it legitimately owns -- and that is the harder failure, because the
 * damage lands in whatever the allocator handed out next. It cost a long chase
 * here: an MFC editor faulting inside its own CMapPtrToPtr::Lookup on a bucket
 * array full of small integers, with nothing to say who had written them.
 *
 * With this set, a block ends exactly at a page boundary and the page after it
 * is PROT_NONE, so the overrunning store faults on the instruction that makes
 * it. The guest's address minus the image base is an RVA, which is a question
 * Ghidra can answer.
 *
 * A page and some rounding per allocation, so this is a diagnostic and not a
 * default. The offset from the mapping to the pointer is recomputed from the
 * recorded size rather than stored in a header, because reading a header means
 * reading memory in front of a pointer that may not be ours. */
static int w32_guard_heap(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("PELOAD_GUARD_HEAP"); v = e && *e != '0'; }
    return v;
}

#define W32_PAGE 4096u

static size_t w32_guard_pad(size_t n)
{ return (W32_PAGE - (n % W32_PAGE)) % W32_PAGE; }

static void *w32_guard_alloc(size_t n)
{
    size_t pad = w32_guard_pad(n);
    size_t len = pad + n + W32_PAGE;
    uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    /* The last page of the mapping is the guard; the block ends where it
     * begins, so one byte past the block is already unmapped. */
    if (mprotect(m + pad + n, W32_PAGE, PROT_NONE) != 0) {
        munmap(m, len);
        return NULL;
    }
    return m + pad;
}

static void w32_guard_free(void *p, size_t n)
{
    size_t pad = w32_guard_pad(n);
    munmap((uint8_t *)p - pad, pad + n + W32_PAGE);
}

static void *w32_alloc(size_t n, int zero)
{
    void *p;
    if (!n) n = 1;
    if (w32_guard_heap()) {
        if (!(p = w32_guard_alloc(n))) return NULL;   /* mmap is already zero */
        (void)zero;
    } else if (!(p = zero ? calloc(1, n) : malloc(n))) {
        return NULL;
    }
    w32_heap_note(p, n);
    return p;
}

static void w32_free(void *p)
{
    size_t n;
    if (!p) return;                               /* free(NULL) is a no-op */
    if ((n = w32_heap_take(p)) == (size_t)-1) {
        g_alloc_foreign++;
        PLOG("  [heap] refused a free of %p: not an allocation of ours\n", p);
        return;
    }
    if (w32_guard_heap()) w32_guard_free(p, n);
    else free(p);
}

static void *w32_realloc(void *p, size_t n, int zero)
{
    size_t was;
    void *fresh;

    if (!p) return w32_alloc(n, zero);
    if ((was = w32_heap_take(p)) == (size_t)-1) {
        g_alloc_foreign++;
        PLOG("  [heap] refused a realloc of %p: not an allocation of ours\n", p);
        return NULL;
    }
    if (!n) n = 1;
    if (w32_guard_heap()) {
        /* No realloc against a guard page: a fresh mapping, a copy, and the old
         * one unmapped, so the block keeps ending where the guard begins. */
        if (!(fresh = w32_guard_alloc(n))) {
            w32_heap_note(p, was);
            return NULL;
        }
        memcpy(fresh, p, was < n ? was : n);
        w32_guard_free(p, was);
        w32_heap_note(fresh, n);
        return fresh;
    }
    if (!(fresh = realloc(p, n))) {
        w32_heap_note(p, was);                    /* still live: put it back */
        return NULL;
    }
    if (zero && n > was) memset((char *)fresh + was, 0, n - was);
    w32_heap_note(fresh, n);
    return fresh;
}

/* The requested size, which is what MSVC's _msize and HeapSize report -- glibc's
 * usable size is larger, so a caller comparing it against its own request saw a
 * mismatch. Zero for a pointer we do not know, as those calls do for a bad
 * handle. */
static size_t w32_alloc_size(const void *p)
{
    size_t n = w32_heap_peek(p);
    return n == (size_t)-1 ? 0 : n;
}

/* Through w32_alloc, not strdup: the plug-in's free is w32_free, and that only
 * frees what this host recorded handing out. */
static MSCRT char *w32_strdup_guest(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)w32_alloc(n, 0);
    if (d) memcpy(d, s, n);
    return d;
}

static MS void *st_GetProcessHeap(void) { return (void *)0x48454150; /* 'HEAP' */ }

static MS void *st_HeapAlloc(void *h, uint32_t flags, size_t sz)
{ (void)h; return w32_alloc(sz, (flags & 8) != 0); }
static MS int32_t st_HeapFree(void *h, uint32_t f, void *p)
{ (void)h;(void)f; w32_free(p); return 1; }
static MS void *st_HeapReAlloc(void *h, uint32_t f, void *p, size_t sz)
{ (void)h; return w32_realloc(p, sz, (f & 8) != 0); }
static MS size_t st_HeapSize(void *h, uint32_t f, void *p)
{ (void)h;(void)f; return w32_alloc_size(p); }
static MS void *st_HeapCreate(uint32_t a, size_t b, size_t c)
{ (void)a;(void)b;(void)c; return (void *)0x48454150; }
static MS int32_t st_HeapDestroy(void *h) { (void)h; return 1; }
static MS int32_t st_HeapSetInformation(void *a, int b, void *c, size_t d)
{ (void)a;(void)b;(void)c;(void)d; return 1; }
static MS int32_t st_HeapValidate(void *a, uint32_t b, const void *c)
{ (void)a;(void)b;(void)c; return 1; }

static MS void *st_VirtualAlloc(void *addr, size_t sz, uint32_t type, uint32_t prot)
{
    int p = PROT_READ | PROT_WRITE;
    void *r;
    (void)type;
    if (prot == 0x40 || prot == 0x20) p |= PROT_EXEC;      /* EXECUTE_READ(WRITE) */
    r = mmap(addr, sz, p, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return r == MAP_FAILED ? NULL : r;
}
static MS int32_t st_VirtualFree(void *a, size_t sz, uint32_t t)
{ (void)t; if (a) munmap(a, sz ? sz : 4096); return 1; }
static MS int32_t st_VirtualProtect(void *a, size_t sz, uint32_t prot, uint32_t *old)
{
    int p = PROT_READ;
    if (prot == 0x04 || prot == 0x40 || prot == 0x80) p |= PROT_WRITE;
    if (prot == 0x10 || prot == 0x20 || prot == 0x40) p |= PROT_EXEC;
    if (old) *old = 0x04;
    return mprotect((void *)((uintptr_t)a & ~0xFFFul), sz + 0xFFF, p) == 0;
}
static MS size_t st_VirtualQuery(void *a, void *buf, size_t len)
{ (void)a; if (buf) memset(buf, 0, len); return len; }

static MS void *st_GlobalAlloc(uint32_t f, size_t sz)
{ return w32_alloc(sz, (f & 0x40) != 0); }
static MS void *st_GlobalFree(void *p) { w32_free(p); return NULL; }
static MS void *st_GlobalLock(void *p) { return p; }
static MS int32_t st_GlobalUnlock(void *p) { (void)p; return 1; }
static MS void *st_LocalAlloc(uint32_t f, size_t sz)
{ return w32_alloc(sz, (f & 0x40) != 0); }
static MS void *st_LocalFree(void *p) { w32_free(p); return NULL; }

/* The rest of the Local/Global family, which is not optional the moment a
 * plug-in is old enough to use it.
 *
 * A stub returning NULL here is not a soft failure: NULL from an allocator is
 * the caller's signal that the machine is out of memory, and a plug-in built on
 * MFC turns that straight into `throw CMemoryException` -- which is how the
 * first 32-bit plug-in tried here died, several subsystems away from the stub
 * that lied to it. The allocation it was making was 32 bytes.
 *
 * A handle and its pointer are the same value throughout, which is what the
 * Lock/Unlock pair above already assumes: the moveable-memory distinction the
 * API is built around exists for a 16-bit segmented heap that this is not.
 *
 * Size is answered from the allocator rather than remembered. glibc knows the
 * usable size of a block, which may exceed what was asked for -- and so may
 * Windows', for the same reason, so a caller that trusts the answer is already
 * required to cope with it. */
static void *w32_heap_realloc(void *p, size_t sz, uint32_t flags)
{ return w32_realloc(p, sz, (flags & 0x40) != 0); }
static MS void *st_LocalReAlloc(void *p, size_t sz, uint32_t f)
{ return w32_heap_realloc(p, sz, f); }
static MS void *st_GlobalReAlloc(void *p, size_t sz, uint32_t f)
{ return w32_heap_realloc(p, sz, f); }
static MS void *st_LocalLock(void *p) { return p; }
static MS int32_t st_LocalUnlock(void *p) { (void)p; return 1; }
static MS void *st_LocalHandle(void *p) { return p; }
static MS void *st_GlobalHandle(void *p) { return p; }
static MS size_t st_LocalSize(void *p) { return w32_alloc_size(p); }
static MS size_t st_GlobalSize(void *p) { return w32_alloc_size(p); }
/* LMEM_/GMEM_ flags of a live fixed block: not discarded, lock count zero. */
static MS uint32_t st_LocalFlags(void *p) { (void)p; return 0; }
static MS uint32_t st_GlobalFlags(void *p) { (void)p; return 0; }

/* ------------------------------------------------------- critical sections */

/* CRITICAL_SECTION is 40 bytes on x64; we only use the first 8 to hold a
 * recursive pthread mutex, which is what the Windows semantics require. */
/* Windows structures whose size differs between Win32 and Win64.
 *
 * Zeroing a caller-supplied struct with a hardcoded byte count is only safe for
 * the width it was written for: STARTUPINFOW is 104 bytes on x86-64 but 68 on
 * i386, so an over-long memset walks off the end of the guest's own stack frame
 * and takes its return address with it. Declaring the layouts and using sizeof
 * lets the compiler produce the right number for whichever ABI is being built.
 */
typedef struct {
    void    *DebugInfo;
    int32_t  LockCount, RecursionCount;
    void    *OwningThread, *LockSemaphore;
    uintptr_t SpinCount;
} W_CRITICAL_SECTION;                          /* 24 on i386, 40 on x86-64 */

typedef struct {
    uint32_t dwOemId;
    uint32_t dwPageSize;
    void    *lpMinimumApplicationAddress, *lpMaximumApplicationAddress;
    uintptr_t dwActiveProcessorMask;
    uint32_t dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    uint16_t wProcessorLevel, wProcessorRevision;
} W_SYSTEM_INFO;                               /* 36 on i386, 48 on x86-64 */

typedef struct {
    uint32_t cb;
    void    *lpReserved, *lpDesktop, *lpTitle;
    uint32_t dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
    uint32_t dwFillAttribute, dwFlags;
    uint16_t wShowWindow, cbReserved2;
    void    *lpReserved2, *hStdInput, *hStdOutput, *hStdError;
} W_STARTUPINFO;                               /* 68 on i386, 104 on x86-64 */

typedef struct { uint32_t MaxCharSize; uint8_t DefaultChar[2], LeadByte[12]; } W_CPINFO;

typedef struct {
    uint32_t dwFileAttributes;
    uint32_t ftCreation[2], ftAccess[2], ftWrite[2];
    uint32_t nFileSizeHigh, nFileSizeLow, nNumberOfLinks;
    uint32_t nFileIndexHigh, nFileIndexLow;
} W_BY_HANDLE_FILE_INFORMATION;

/* A recursive mutex, which is what the Windows locking semantics require. Kept
 * separate from clearing the caller's object because the objects differ in size:
 * see st_InitializeSRWLock. */
static pthread_mutex_t *w32_new_mutex(void)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    if (!m) return NULL;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

static MS void st_InitializeCriticalSection(void *cs)
{
    int dbg = getenv("PELOAD_CSDEBUG") != NULL;
    static long n;
    if (dbg) fprintf(stderr, "  [cs] #%ld cs=%p enter\n", ++n, cs);
    memset(cs, 0, sizeof(W_CRITICAL_SECTION));
    if (dbg) fprintf(stderr, "  [cs] #%ld memset done\n", n);
    *(pthread_mutex_t **)cs = w32_new_mutex();
    if (dbg) fprintf(stderr, "  [cs] #%ld mutex %p done\n", n, *(void **)cs);
}
static MS int32_t st_InitializeCriticalSectionAndSpinCount(void *cs, uint32_t n)
{ (void)n; st_InitializeCriticalSection(cs); return 1; }
static MS int32_t st_InitializeCriticalSectionEx(void *cs, uint32_t n, uint32_t f)
{ (void)n;(void)f; st_InitializeCriticalSection(cs); return 1; }
static MS void st_EnterCriticalSection(void *cs)
{ pthread_mutex_t *m = *(pthread_mutex_t **)cs; if (m) pthread_mutex_lock(m); }
static MS void st_LeaveCriticalSection(void *cs)
{ pthread_mutex_t *m = *(pthread_mutex_t **)cs; if (m) pthread_mutex_unlock(m); }
static MS int32_t st_TryEnterCriticalSection(void *cs)
{ pthread_mutex_t *m = *(pthread_mutex_t **)cs; return m ? pthread_mutex_trylock(m) == 0 : 1; }
static MS void st_DeleteCriticalSection(void *cs)
{
    pthread_mutex_t *m = *(pthread_mutex_t **)cs;
    if (m) { pthread_mutex_destroy(m); free(m); }
    *(pthread_mutex_t **)cs = NULL;
}

/* SRW locks and one-time init: a plain mutex is semantically sufficient here. */
/* An SRWLOCK is a single pointer-sized word, not a CRITICAL_SECTION. Clearing it
 * as though it were the larger structure writes 32 bytes past its end on x86-64,
 * and when one is embedded in a heap allocation -- which is where a C++ runtime
 * keeps them -- the damage surfaces later as a corrupt heap, a long way from
 * here. Only the one word this object actually owns may be touched. */
static MS void st_InitializeSRWLock(void *l)
{ *(pthread_mutex_t **)l = w32_new_mutex(); }
static MS void st_AcquireSRWLockExclusive(void *l) { st_EnterCriticalSection(l); }
static MS void st_AcquireSRWLockShared(void *l) { st_EnterCriticalSection(l); }
static MS void st_ReleaseSRWLockExclusive(void *l) { st_LeaveCriticalSection(l); }
static MS void st_ReleaseSRWLockShared(void *l) { st_LeaveCriticalSection(l); }
static MS int32_t st_TryAcquireSRWLockExclusive(void *l) { return st_TryEnterCriticalSection(l); }

static MS int32_t st_InitOnceBeginInitialize(void *once, uint32_t f, int32_t *pending, void **ctx)
{
    (void)f;
    if (ctx) *ctx = NULL;
    if (pending) *pending = (*(uint64_t *)once == 0);
    return 1;
}
static MS int32_t st_InitOnceComplete(void *once, uint32_t f, void *ctx)
{ (void)f;(void)ctx; *(uint64_t *)once = 1; return 1; }
static MS int32_t st_InitOnceExecuteOnce(void *once, void *fn, void *param, void **ctx)
{
    MS int32_t (*f)(void *, void *, void **) = fn;
    if (*(uint64_t *)once) return 1;
    *(uint64_t *)once = 1;
    return f ? f(once, param, ctx) : 1;
}
static MS void st_InitializeConditionVariable(void *c) { memset(c, 0, 8); }
static MS void st_WakeConditionVariable(void *c) { (void)c; }
static MS void st_WakeAllConditionVariable(void *c) { (void)c; }
/* SLIST_HEADER is 8 bytes on i386 and 16 on x86-64 */
static MS void st_InitializeSListHead(void *h)
{ memset(h, 0, sizeof(void *) == 8 ? 16 : 8); }
static MS void *st_InterlockedFlushSList(void *h) { (void)h; return NULL; }

/* The Interlocked family.
 *
 * These are compiler intrinsics in mingw, so they carry no @N in the import
 * libraries and the arity table cannot cover them -- which is how they surfaced:
 * the i386 loader warned it was stubbing them with an unknown arity. Stubbing
 * them is not survivable regardless of arity, because callers use them for
 * reference counts, and a stub that always returns 0 makes every release look
 * like the last one. GCC's atomics give the documented semantics exactly.
 *
 * Note which value each returns: Increment/Decrement return the *new* value,
 * Exchange and ExchangeAdd return the *old* one. */
static MS int32_t st_InterlockedIncrement(volatile int32_t *p)
{ return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static MS int32_t st_InterlockedDecrement(volatile int32_t *p)
{ return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static MS int32_t st_InterlockedExchange(volatile int32_t *p, int32_t v)
{ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static MS int32_t st_InterlockedExchangeAdd(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static MS int32_t st_InterlockedCompareExchange(volatile int32_t *p, int32_t xchg, int32_t cmp)
{ __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return cmp; }                    /* cmp receives the previous value on failure */
static MS int32_t st_InterlockedOr(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
static MS int32_t st_InterlockedAnd(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
static MS int32_t st_InterlockedXor(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST); }
static MS int64_t st_InterlockedIncrement64(volatile int64_t *p)
{ return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static MS int64_t st_InterlockedDecrement64(volatile int64_t *p)
{ return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static MS int64_t st_InterlockedExchangeAdd64(volatile int64_t *p, int64_t v)
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static MS void *st_InterlockedExchangePointer(void *volatile *p, void *v)
{ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static MS void *st_InterlockedCompareExchangePointer(void *volatile *p, void *xchg, void *cmp)
{ __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return cmp; }

/* -------------------------------------------------------------------- TLS */

/* Slots 0..7 belong to module TLS; the rest are handed out here.
 *
 * TlsFree used to accept and do nothing, and the allocator only counted upward,
 * so every slot a plug-in took was gone for the life of the process. A hundred
 * and twenty slots sounds like plenty until you notice that browsing spends
 * them: about eighty plug-ins into a session TlsAlloc starts returning
 * TLS_OUT_OF_INDEXES, the Microsoft runtime's DllMain fails on that, and every
 * plug-in loaded afterwards is reported as "DllMain failed" -- the host
 * refusing perfectly good plug-ins because of what it had already thrown away
 * on their predecessors. */
static uint32_t g_tls_used[(TLS_SLOTS + 31) / 32];

static MS uint32_t st_TlsAlloc(void)
{
    uint32_t i;
    for (i = 8; i < TLS_SLOTS; i++)
        if (!(g_tls_used[i >> 5] & (1u << (i & 31)))) {
            g_tls_used[i >> 5] |= 1u << (i & 31);
            teb_clear_slot(i);          /* NULL on every thread, as Windows does */
            return i;
        }
    return 0xFFFFFFFFu;                          /* TLS_OUT_OF_INDEXES */
}
static MS int32_t st_TlsFree(uint32_t i)
{
    if (i < 8 || i >= TLS_SLOTS) return 0;
    g_tls_used[i >> 5] &= ~(1u << (i & 31));
    if (g_teb) g_teb->slots[i] = NULL;
    return 1;
}

/* Every slot the outgoing plug-in held. A plug-in's runtime does not reliably
 * free its own on the way out, and once its image is unmapped the slots are
 * dead whether it freed them or not. */
static void winstubs_reset_tls(void)
{
    memset(g_tls_used, 0, sizeof g_tls_used);
    teb_clear_all_slots();
}
static MS void *st_TlsGetValue(uint32_t i)
{ return i < TLS_SLOTS && g_teb ? g_teb->slots[i] : NULL; }
static MS int32_t st_TlsSetValue(uint32_t i, void *v)
{ if (i < TLS_SLOTS && g_teb) { g_teb->slots[i] = v; return 1; } return 0; }
static MS uint32_t st_FlsAlloc(void *cb) { (void)cb; return st_TlsAlloc(); }
static MS int32_t st_FlsFree(uint32_t i) { return st_TlsFree(i); }
static MS void *st_FlsGetValue(uint32_t i) { return st_TlsGetValue(i); }
static MS int32_t st_FlsSetValue(uint32_t i, void *v) { return st_TlsSetValue(i, v); }

/* ------------------------------------------------------- process / thread */

static MS uint32_t st_GetLastError(void) { return g_last_error; }
static MS void st_SetLastError(uint32_t e) { g_last_error = e; }
static MS uint32_t st_GetCurrentThreadId(void) { return (uint32_t)(uintptr_t)pthread_self(); }
static MS uint32_t st_GetCurrentProcessId(void) { return (uint32_t)getpid(); }
static MS void *st_GetCurrentProcess(void) { return (void *)(intptr_t)-1; }
static MS void *st_GetCurrentThread(void) { return (void *)(intptr_t)-2; }
static MS void *st_EncodePointer(void *p) { return p; }
static MS void *st_DecodePointer(void *p) { return p; }
static MS int32_t st_IsProcessorFeaturePresent(uint32_t f) { (void)f; return 1; }
static MS int32_t st_IsDebuggerPresent(void) { return 0; }
static MS void *st_SetUnhandledExceptionFilter(void *f) { (void)f; return NULL; }
static MS int32_t st_UnhandledExceptionFilter(void *p) { (void)p; return 1; }
static MS void st_TerminateProcess(void *h, uint32_t c)
{ (void)h; fprintf(stderr, "[win] TerminateProcess(%u)\n", c); exit((int)c); }
static MS void st_ExitProcess(uint32_t c)
{ fprintf(stderr, "[win] ExitProcess(%u)\n", c); exit((int)c); }
/* The one raise that is not an error.
 *
 * 0x406D1388 is how a thread announces its name to a debugger: the code wraps
 * RaiseException in a __try whose __except swallows it, because with no debugger
 * attached nobody is listening. That __try block contains nothing but the raise,
 * so returning normally lands exactly where the __except would have continued --
 * which is why this needs no unwinding to be correct.
 *
 * The name is worth keeping: it goes to the host thread, so anything looking at
 * this process sees the names the plugin chose. */
static MS void st_RaiseException(uint32_t code, uint32_t f, uint32_t n,
                                 const uintptr_t *a)
{
    if (code == 0x406D1388u && n >= 2 && a) {
        /* THREADNAME_INFO: type (always 0x1000), name, thread id, flags. */
        const char *name = (const char *)(uintptr_t)a[1];
        if (a[0] == 0x1000 && name) {
            char t[16];                       /* pthread's limit, including NUL */
            snprintf(t, sizeof t, "%s", name);
            pthread_setname_np(pthread_self(), t);
            PLOG("  [win] thread named \'%s\'\n", name);
        }
        return;
    }
    /* Naming every C++ throw, not just the ones nobody catches, is what turns
     * "it stopped" into "it stopped on this". A throw that is caught two frames
     * up is normal and says nothing; one thrown during a plug-in's start-up
     * usually names the Win32 call that disappointed it. */
#if defined(__i386__)
    if (code == 0xE06D7363u && n >= 3 && a && pe_verbose()) {
        ms_exc_rec32 probe;
        const char *tn;
        memset(&probe, 0, sizeof probe);
        probe.ExceptionCode    = code;
        probe.NumberParameters = 3;
        probe.ExceptionInformation[0] = (uint32_t)a[0];
        probe.ExceptionInformation[1] = (uint32_t)a[1];
        probe.ExceptionInformation[2] = (uint32_t)a[2];
        tn = ms_throw_typename32(&probe);
        PLOG("  [eh] throw %s (object 0x%x)\n", tn ? tn : "?", (unsigned)a[1]);
    }
    /* Hand it to the plug-in's own handlers. A C++ throw arrives here as
     * 0xE06D7363 with the object and its ThrowInfo in the parameters, and the
     * __CxxFrameHandler that catches it never returns -- so anything below this
     * block is the unhandled case, which is the only one worth reporting. */
    {
        ms_exc_rec32 rec;
        uint32_t i;

        memset(&rec, 0, sizeof rec);
        rec.ExceptionCode     = code;
        rec.ExceptionFlags    = f;
        rec.ExceptionAddress  = __builtin_return_address(0);
        rec.NumberParameters  = n > 15 ? 15 : n;
        for (i = 0; i < rec.NumberParameters && a; i++)
            rec.ExceptionInformation[i] = (uint32_t)a[i];
        if (ms_dispatch32(&rec)) return;      /* a handler resumed execution */
    }
    fprintf(stderr, "[win] RaiseException(0x%08x): no handler in the chain "
                    "accepted it\n", code);
#else
    (void)f;
    fprintf(stderr, "[win] RaiseException(0x%08x): structured exception "
                    "dispatch is not implemented at this width, so this cannot "
                    "be delivered to a handler\n", code);
#endif
    fflush(stderr);
    abort();
}

#if defined(__i386__)
/* The other half a catch needs: everything between the throw and the frame the
 * handler chose has to have its __finally blocks and destructors run before
 * control lands in the catch. MSVC's handler asks for that by calling this and
 * expects it to come back, which a plain C function does -- it preserves the
 * callee-saved registers its caller is relying on by construction. */
static MS void st_RtlUnwind(void *target, void *target_ip, void *rec, void *ret)
{ ms_unwind32(target, target_ip, (ms_exc_rec32 *)rec, ret); }
#endif
static MS void st_SetErrorMode(uint32_t m) { (void)m; }
static MS uint32_t st_GetVersion(void) { return 0x0A00; }

/* Report Windows 10. Filling this in matters: a plugin that checks the OS
 * version during DllMain treats a zero return as "cannot run here" and refuses
 * to initialise, which is not obviously a version check when all you see is
 * "DllMain failed".
 *
 * dwOSVersionInfoSize tells us whether the caller passed OSVERSIONINFO or the
 * EX form, so write only as far as it says the buffer goes. The layout is the
 * same at both widths -- every field is a DWORD or WORD. */
static MS int32_t st_GetVersionExA(void *p)
{
    uint32_t *v = p;
    if (!v) return 0;
    if (v[0] < 5 * 4 + 128) return 0;          /* too small to be an OSVERSIONINFOA */
    v[1] = 10; v[2] = 0; v[3] = 19045;
    v[4] = 2;                                  /* VER_PLATFORM_WIN32_NT */
    memset(&v[5], 0, 128);                     /* szCSDVersion: no service pack */
    if (v[0] >= 5 * 4 + 128 + 8) {             /* OSVERSIONINFOEXA */
        uint8_t *ex = (uint8_t *)&v[5] + 128;
        *(uint16_t *)(ex + 0) = 0;             /* wServicePackMajor */
        *(uint16_t *)(ex + 2) = 0;             /* wServicePackMinor */
        *(uint16_t *)(ex + 4) = 0x100;         /* wSuiteMask: VER_SUITE_SINGLEUSERTS */
        ex[6] = 1;                             /* wProductType: VER_NT_WORKSTATION */
        ex[7] = 0;
    }
    return 1;
}
static MS int32_t st_GetVersionExW(void *p)
{
    uint32_t *v = p;
    if (!v) return 0;
    if (v[0] < 5 * 4 + 256) return 0;          /* OSVERSIONINFOW: WCHAR[128] */
    v[1] = 10; v[2] = 0; v[3] = 19045;
    v[4] = 2;
    memset(&v[5], 0, 256);
    if (v[0] >= 5 * 4 + 256 + 8) {
        uint8_t *ex = (uint8_t *)&v[5] + 256;
        *(uint16_t *)(ex + 0) = 0;
        *(uint16_t *)(ex + 2) = 0;
        *(uint16_t *)(ex + 4) = 0x100;
        ex[6] = 1;
        ex[7] = 0;
    }
    return 1;
}
static MS void st_Sleep(uint32_t ms)
{ struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static MS int32_t st_SwitchToThread(void) { sched_yield(); return 1; }
static MS void st_GetSystemInfo(void *si)
{
    if (!si) return;
    { W_SYSTEM_INFO *s = si;
      long n = sysconf(_SC_NPROCESSORS_ONLN);
      memset(s, 0, sizeof *s);
      s->dwPageSize             = 4096;
      s->dwNumberOfProcessors   = (uint32_t)(n > 0 ? n : 1);
      s->dwActiveProcessorMask  = n > 0 && n < (long)(8 * sizeof(uintptr_t))
                                    ? ((uintptr_t)1 << n) - 1 : (uintptr_t)-1;
      s->dwAllocationGranularity = 0x10000; }
}

/* ------------------------------------------------------------------- time */

static MS int32_t st_QueryPerformanceCounter(int64_t *v)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    if (v) *v = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
    return 1;
}
static MS int32_t st_QueryPerformanceFrequency(int64_t *v)
{ if (v) *v = 1000000000LL; return 1; }
static MS void st_GetSystemTimeAsFileTime(uint64_t *ft)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    /* FILETIME: 100 ns ticks since 1601-01-01 */
    if (ft) *ft = (uint64_t)t.tv_sec * 10000000ULL + t.tv_nsec / 100 + 116444736000000000ULL;
}
/* Thread and process CPU accounting. The Concurrency runtime measures how much
 * work a virtual processor is getting through with GetThreadTimes, and a stub
 * answering FALSE is a failure it does not carry on from. FILETIME is 100 ns
 * ticks; for the two CPU figures the epoch does not enter into it, they are
 * durations. */
static uint64_t w32_ts_to_ft(const struct timespec *t)
{ return (uint64_t)t->tv_sec * 10000000ULL + (uint64_t)t->tv_nsec / 100; }

static uint64_t g_proc_start_ft;
static MS int32_t st_GetThreadTimes(void *h, uint64_t *create, uint64_t *exit_,
                                    uint64_t *kernel, uint64_t *user)
{
    struct timespec t;
    (void)h;
    if (!g_proc_start_ft) {
        struct timespec r;
        clock_gettime(CLOCK_REALTIME, &r);
        g_proc_start_ft = w32_ts_to_ft(&r) + 116444736000000000ULL;
    }
    if (create) *create = g_proc_start_ft;
    if (exit_)  *exit_  = 0;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) { g_last_error = 5; return 0; }
    if (user)   *user   = w32_ts_to_ft(&t);
    if (kernel) *kernel = 0;
    return 1;
}
static MS int32_t st_GetProcessTimes(void *h, uint64_t *create, uint64_t *exit_,
                                     uint64_t *kernel, uint64_t *user)
{
    struct timespec t;
    (void)h;
    if (!g_proc_start_ft) {
        struct timespec r;
        clock_gettime(CLOCK_REALTIME, &r);
        g_proc_start_ft = w32_ts_to_ft(&r) + 116444736000000000ULL;
    }
    if (create) *create = g_proc_start_ft;
    if (exit_)  *exit_  = 0;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) { g_last_error = 5; return 0; }
    if (user)   *user   = w32_ts_to_ft(&t);
    if (kernel) *kernel = 0;
    return 1;
}
static MS int32_t st_GetSystemTimes(uint64_t *idle, uint64_t *kernel, uint64_t *user)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (idle)   *idle   = w32_ts_to_ft(&t);
    if (kernel) *kernel = 0;
    if (user)   *user   = w32_ts_to_ft(&t);
    return 1;
}
static MS int32_t st_QueryProcessCycleTime(void *h, uint64_t *cycles)
{
    struct timespec t;
    (void)h;
    if (!cycles) { g_last_error = 87; return 0; }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    *cycles = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    return 1;
}

static MS uint32_t st_GetTickCount(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static MS uint64_t st_GetTickCount64(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* --------------------------------------------------------- module / loader */

static MS void *st_GetModuleHandleA(const char *n) { (void)n; return g_image_base; }
/* The Windows Runtime apartment calls, which live in combase.dll.
 *
 * msvcr120's Concurrency runtime resolves RoInitialize and RoUninitialize and
 * throws scheduler_resource_allocation_error if either is missing -- it does
 * not treat them as optional, even in a plain desktop process. Reporting the
 * apartment as already initialised is what a desktop process sees anyway:
 * S_FALSE, not an error. */
/* Event tracing. Nothing here records anything, but every one of these has to
 * exist: the MSVC CRT resolves RegisterTraceGuidsW through GetProcAddress,
 * stores the result XOR'd with a cookie of its own, and then calls it back
 * having compared the slot against EncodePointer(NULL) -- a comparison that
 * cannot match, because the two encodings are unrelated. On Windows that is
 * harmless, since advapi32 always has the function; here a NULL decoded to a
 * call through address zero, which is where the 32-bit NI plug-ins landed once
 * their scheduler started. Answering ERROR_SUCCESS and doing nothing is what a
 * process with no tracing session sees. */
static MS uint32_t st_RegisterTraceGuidsW(void *req, void *ctx, const void *guid,
                                          uint32_t count, void *reg,
                                          const uint16_t *mofpath,
                                          const uint16_t *mofres, uint64_t *handle)
{
    (void)req; (void)ctx; (void)guid; (void)count; (void)reg;
    (void)mofpath; (void)mofres;
    if (handle) *handle = 1;                   /* a handle, not a session */
    return 0;                                  /* ERROR_SUCCESS */
}
static MS uint32_t st_RegisterTraceGuidsA(void *req, void *ctx, const void *guid,
                                          uint32_t count, void *reg,
                                          const char *mofpath, const char *mofres,
                                          uint64_t *handle)
{
    (void)mofpath; (void)mofres;
    return st_RegisterTraceGuidsW(req, ctx, guid, count, reg, NULL, NULL, handle);
}
static MS uint32_t st_UnregisterTraceGuids(uint64_t handle) { (void)handle; return 0; }
static MS uint64_t st_GetTraceLoggerHandle(void *buf) { (void)buf; return 0; }
static MS uint8_t  st_GetTraceEnableLevel(uint64_t h) { (void)h; return 0; }
static MS uint32_t st_GetTraceEnableFlags(uint64_t h) { (void)h; return 0; }
static MS uint32_t st_TraceEvent(uint64_t h, void *ev) { (void)h; (void)ev; return 0; }
static MS uint32_t st_EventRegister(const void *id, void *cb, void *ctx, uint64_t *h)
{ (void)id; (void)cb; (void)ctx; if (h) *h = 1; return 0; }
static MS uint32_t st_EventUnregister(uint64_t h) { (void)h; return 0; }
static MS uint32_t st_EventWrite(uint64_t h, const void *desc, uint32_t n, void *d)
{ (void)h; (void)desc; (void)n; (void)d; return 0; }
static MS uint32_t st_EventWriteTransfer(uint64_t h, const void *desc, const void *a,
                                         const void *b, uint32_t n, void *d)
{ (void)h;(void)desc;(void)a;(void)b;(void)n;(void)d; return 0; }
static MS int32_t st_EventEnabled(uint64_t h, const void *desc)
{ (void)h; (void)desc; return 0; }
static MS int32_t st_EventProviderEnabled(uint64_t h, uint8_t lvl, uint64_t kw)
{ (void)h; (void)lvl; (void)kw; return 0; }
static MS uint32_t st_EventSetInformation(uint64_t h, uint32_t cls, void *info, uint32_t n)
{ (void)h;(void)cls;(void)info;(void)n; return 0; }
static MS uint32_t st_EventActivityIdControl(uint32_t code, void *guid)
{ (void)code; (void)guid; return 0; }

static MS int32_t st_RoInitialize(uint32_t type)
{ (void)type; return 1; }                      /* S_FALSE: already initialised */
static MS void st_RoUninitialize(void) { }
static MS int32_t st_RoGetActivationFactory(void *cls, const void *iid, void **out)
{ (void)cls; (void)iid; if (out) *out = NULL; return (int32_t)0x80004001; }  /* E_NOTIMPL */
static MS int32_t st_RoActivateInstance(void *cls, void **out)
{ (void)cls; if (out) *out = NULL; return (int32_t)0x80004001; }

/* A module handle for a name this host knows, rather than the plug-in's own
 * base for every question asked. GetProcAddress searches the whole table by
 * name regardless, so this only sharpens which handle a caller gets back. */
static void *w32_lib_handle(const char *name);            /* below */
static MS void *st_GetModuleHandleW(const void *n)
{
    char b[256];
    void *h;
    if (!n) return g_image_base;
    w2c((const uint16_t *)n, b, sizeof b);
    h = w32_lib_handle(b);
    return h ? h : g_image_base;
}
static MS int32_t st_GetModuleHandleExW(uint32_t f, const void *n, void **out)
{ (void)f;(void)n; if (out) *out = g_image_base; return 1; }
/* The A form was missing while the W form was here, so a plug-in that asked
 * for its own module handle this way got the generic stub's zero -- and read
 * that as "I am not loaded", which is not a state its code has a path for. */
static MS int32_t st_GetModuleHandleExA(uint32_t f, const char *n, void **out)
{ (void)f;(void)n; if (out) *out = g_image_base; return 1; }
/* The plug-in's own file, spelled the way a Windows program expects it.
 *
 * This used to answer with a fixed "C:\\peload\\plugin.dll" no matter what was
 * loaded. That is enough for a plug-in that only wants *a* name, and wrong for
 * the very common one that takes the directory out of it and loads its own data
 * from beside itself -- the fixed answer normalises to /peload/, which holds
 * nothing. A SynthEdit plug-in asks for its .sem modules that way and got seven
 * nulls in a row, then threw because its module registry was empty.
 *
 * The real path goes back instead, with the separators flipped and a drive
 * letter in front, so a guest splitting it on a backslash finds the right
 * pieces and path_norm_n turns whatever it rebuilds back into the host path it
 * came from. */
static char g_image_path_win[1024];

static void winstubs_set_image_path(const char *host_path)
{
    size_t i;
    if (!host_path || !*host_path) { g_image_path_win[0] = 0; return; }
    snprintf(g_image_path_win, sizeof g_image_path_win, "C:%s%s",
             host_path[0] == '/' ? "" : "/", host_path);
    for (i = 0; g_image_path_win[i]; i++)
        if (g_image_path_win[i] == '/') g_image_path_win[i] = '\\';
}

/* Saved across a side-load: pe_module_load installs whatever it loads as the
 * primary image, and a runtime DLL fetched from runtime/ must not become the
 * answer to "where am I". */
static void winstubs_image_path_save(char *out, size_t n)
{ snprintf(out, n, "%s", g_image_path_win); }
static void winstubs_image_path_restore(const char *p)
{ snprintf(g_image_path_win, sizeof g_image_path_win, "%s", p ? p : ""); }

static const char *winstubs_image_path(void)
{ return g_image_path_win[0] ? g_image_path_win : "C:\\peload\\plugin.dll"; }

static MS uint32_t st_GetModuleFileNameA(void *h, char *buf, uint32_t sz)
{ (void)h; return (uint32_t)snprintf(buf, sz, "%s", winstubs_image_path()); }
static MS uint32_t st_GetModuleFileNameW(void *h, uint16_t *buf, uint32_t sz)
{
    const char *s = winstubs_image_path();
    uint32_t i;
    (void)h;
    for (i = 0; s[i] && i + 1 < sz; i++) buf[i] = (uint16_t)s[i];
    if (sz) buf[i] = 0;
    return i;
}
#define H_DWRITE ((void *)0x44575254)          /* 'DWRT' */

#ifndef PELOAD_NO_GUI_LAYER
/* Defined in dwrite_shim.h, which is included further down with the GUI layer. */
static MS int32_t st_DWriteCreateFactory(uint32_t type, const void *iid, void **out);
#endif

/* Synthetic module handles.
 *
 * Returning NULL from LoadLibrary is not the safe default it looks like: the
 * MSVC CRT probes for optional APIs with
 *
 *     m = LoadLibraryExW(L"api-ms-win-core-fibers-l1-1-1", ...);
 *     if (!m) m = LoadLibraryExW(L"kernel32", ...);
 *     p = GetProcAddress(m, "FlsAlloc");
 *
 * and several of those call sites use the result without checking, because on
 * Windows the second LoadLibrary cannot fail. Handing back a real handle per
 * system DLL, and answering GetProcAddress from the stub table, turns those
 * probes into ordinary hits or honest NULLs on a name we truly lack.
 *
 * The handle encodes an index so GetProcAddress knows which DLL was asked for;
 * an API-set name resolves to whichever DLL actually hosts those exports. */
static const char *const g_sysdlls[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "ole32.dll",
    "advapi32.dll", "shell32.dll", "shlwapi.dll", "winmm.dll",
    "comctl32.dll", "msvcrt.dll", "version.dll", "oleaut32.dll",
    "combase.dll",
};
#define NSYSDLL   ((int)(sizeof g_sysdlls / sizeof *g_sysdlls))
#define H_DLL(i)  ((void *)(uintptr_t)(0x5044u << 16 | (unsigned)(i)))   /* 'PD' */

/* DLLs that are part of any stock Windows install but that this host implements
 * nothing for.
 *
 * LoadLibrary has to succeed for these. msvcr120's Concurrency runtime opens
 * combase.dll with LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32) and, if it
 * comes back NULL, throws scheduler_resource_allocation_error out of its own
 * initialisation -- which is where all four 32-bit NI plug-ins stopped, with an
 * exception whose text says nothing about a missing DLL. On Windows the file is
 * always there, so answering "no such library" is the wrong answer to give.
 *
 * GetProcAddress against one of these still returns NULL for anything not
 * implemented, which is exactly what a caller probing for an optional export
 * expects to see. The graphics DLLs are deliberately absent: a plug-in that
 * finds d3d11 present takes its Direct3D path, and the fallback it takes today
 * is the one this host actually draws. */
static const char *const g_stockdlls[] = {
    "ntdll", "kernelbase", "sechost", "rpcrt4", "crypt32", "bcrypt",
    "ncrypt", "wintrust", "secur32", "psapi", "imm32", "uxtheme", "dwmapi",
    "powrprof", "setupapi", "iphlpapi", "mpr", "netapi32", "userenv", "usp10",
    "msimg32", "dbghelp", "propsys", "oleacc", "avrt", "winhttp", "wininet",
    "urlmon", "winspool", "cfgmgr32", "kernel.appcore", "shcore", "profapi",
};
#define NSTOCKDLL  ((int)(sizeof g_stockdlls / sizeof *g_stockdlls))
#define H_STOCK(i) ((void *)(uintptr_t)(0x5350u << 16 | (unsigned)(i)))  /* 'SP' */

static int w32_stock_index(void *h)
{
    uintptr_t v = (uintptr_t)h;
    if ((v >> 16) != 0x5350u) return -1;
    return (int)(v & 0xFFFF) < NSTOCKDLL ? (int)(v & 0xFFFF) : -1;
}

static int w32_dll_index(void *h)
{
    uintptr_t v = (uintptr_t)h;
    if ((v >> 16) != 0x5044u) return -1;
    return (int)(v & 0xFFFF) < NSYSDLL ? (int)(v & 0xFFFF) : -1;
}

static void *w32_lib_handle(const char *name)
{
    char b[256];
    size_t i;
    int d;

    if (!name || !*name) return NULL;
    /* Skia reaches DirectWrite this way. */
    if (strcasestr(name, "dwrite")) return H_DWRITE;

    /* strip any directory and normalise: names arrive with and without ".dll" */
    { const char *p = strrchr(name, '\\'), *q = strrchr(name, '/');
      if (q > p) p = q;
      snprintf(b, sizeof b, "%s", p ? p + 1 : name); }
    for (i = 0; b[i]; i++) b[i] = (char)tolower((unsigned char)b[i]);
    i = strlen(b);
    if (i > 4 && !strcmp(b + i - 4, ".dll")) b[i - 4] = 0;

    /* API sets are façades; map each to the DLL that really exports it */
    if (!strncmp(b, "api-ms-win-core-", 16) || !strncmp(b, "api-ms-win-crt-", 15))
        return H_DLL(0);                                        /* kernel32 */
    if (!strncmp(b, "api-ms-win-", 11))
        return H_DLL(0);

    for (d = 0; d < NSYSDLL; d++) {
        char cur[32];
        snprintf(cur, sizeof cur, "%s", g_sysdlls[d]);
        cur[strlen(cur) - 4] = 0;                               /* drop ".dll" */
        if (!strcmp(b, cur)) return H_DLL(d);
    }
    for (d = 0; d < NSTOCKDLL; d++)
        if (!strcmp(b, g_stockdlls[d])) return H_STOCK(d);
    return NULL;
}

static void *winstub_lookup(const char *dll, const char *sym);      /* below */

/* Loading a DLL that is genuinely on disk.
 *
 * LoadLibrary only ever answered for the system DLLs this host shims and
 * returned NULL for everything else -- including a file sitting right there
 * that this program, a PE loader, knows exactly how to map. A plug-in shipping
 * part of itself as separate DLLs got nothing back: SynthEdit keeps its modules
 * in .sem files beside the plug-in and loads them this way, so its DSP graph
 * came up empty and it rendered silence with no error anywhere.
 *
 * The mapping cannot happen here. The loader that can do it is whichever one
 * included this header, and there are two of them at different widths, so each
 * installs its own pair and a wrong-width DLL simply fails to map. */
static void *(*g_real_load)(const char *path);
static void *(*g_real_sym)(void *module, const char *name);
static void winstubs_set_loader(void *(*load)(const char *),
                                void *(*sym)(void *, const char *))
{ g_real_load = load; g_real_sym = sym; }

/* Defined further down, with the rest of the path handling. */
static const char *path_fix(const char *in, char *buf, size_t n);

/* A real module's handle is its mapped base -- an ordinary address, and never
 * one of the small tagged values the shimmed DLLs use. */
static void *w32_load_real(const char *name)
{
    char p[1024];
    if (!name || !*name || !g_real_load) return NULL;
    path_fix(name, p, sizeof p);
    if (access(p, R_OK) != 0) return NULL;
    return g_real_load(p);
}

static MS void *st_LoadLibraryA(const char *n)
{
    void *h = w32_lib_handle(n);
    if (!h) h = w32_load_real(n);
    PLOG("  [win] LoadLibraryA(\"%s\") -> %p\n", n ? n : "?", h);
    return h;
}
static MS void *st_LoadLibraryW(const uint16_t *n)
{
    char b[256];
    void *h;
    w2c(n, b, sizeof b);
    h = w32_lib_handle(b);
    if (!h) h = w32_load_real(b);
    PLOG("  [win] LoadLibraryW(\"%s\") -> %p\n", b, h);
    return h;
}
static MS void *st_LoadLibraryExW(const uint16_t *n, void *hf, uint32_t f)
{
    char b[256];
    void *h;
    (void)hf; (void)f;
    w2c(n, b, sizeof b);
    h = w32_lib_handle(b);
    if (!h) h = w32_load_real(b);
    PLOG("  [win] LoadLibraryExW(\"%s\") -> %p\n", b, h);
    return h;
}
static MS void *st_LoadLibraryExA(const char *n, void *hf, uint32_t f)
{
    void *h;
    (void)hf; (void)f;
    h = w32_lib_handle(n);
    if (!h) h = w32_load_real(n);
    PLOG("  [win] LoadLibraryExA(\"%s\") -> %p\n", n ? n : "?", h);
    return h;
}
static MS int32_t st_FreeLibrary(void *h) { (void)h; return 1; }
static MS void *st_GetProcAddress(void *h, const char *n)
{
#ifndef PELOAD_NO_GUI_LAYER
    if (h == H_DWRITE && n && !strcmp(n, "DWriteCreateFactory")) {
        PLOG("  [win] GetProcAddress(\"%s\") -> shim\n", n);
        return (void *)st_DWriteCreateFactory;
    }
#endif
    /* A handle this host mapped itself answers from its own exports first: the
     * module really does define these, and a same-named stub would shadow the
     * genuine one. A miss falls through rather than returning -- the handle may
     * simply not be one of ours, and the name search below still has to run. */
    if (n && h && h != H_DWRITE && w32_dll_index(h) < 0 &&
        w32_stock_index(h) < 0 && g_real_sym) {
        void *fn = g_real_sym(h, n);
        if (fn) { PLOG("  [win] GetProcAddress(\"%s\") -> real\n", n); return fn; }
    }
    if (n) {
        int d = w32_dll_index(h);
        void *fn = NULL;
        if (d >= 0) fn = winstub_lookup(g_sysdlls[d], n);
        /* an API-set handle resolves to kernel32, but the export it fronts may
         * live elsewhere in our table, so fall back to a name-only search */
        for (d = 0; !fn && d < NSYSDLL; d++) fn = winstub_lookup(g_sysdlls[d], n);
        if (fn) { PLOG("  [win] GetProcAddress(\"%s\") -> stub\n", n); return fn; }
    }
    PLOG("  [win] GetProcAddress(\"%s\") -> NULL\n", n ? n : "?");
    return NULL;
}
static MS int32_t st_DisableThreadLibraryCalls(void *h) { (void)h; return 1; }

static MS const char *st_GetCommandLineA(void) { return "peload.exe"; }
static MS const uint16_t *st_GetCommandLineW(void)
{ static const uint16_t w[] = { 'p','e','l','o','a','d','.','e','x','e',0 }; return w; }
#define W32_ENV_MAX 64
static const char *w32_env_block(void);
static MS void *st_GetEnvironmentStringsW(void)
{
    static uint16_t block[W32_ENV_MAX * 600];
    static int built;
    if (!built) {
        const char *a = w32_env_block();
        size_t i = 0;
        for (;;) {
            size_t n = strlen(a + i);
            size_t k;
            for (k = 0; k <= n; k++) block[i + k] = (uint16_t)(unsigned char)a[i + k];
            i += n + 1;
            if (!a[i]) { block[i] = 0; break; }
        }
        built = 1;
    }
    return block;
}
static MS int32_t st_FreeEnvironmentStringsW(void *p) { (void)p; return 1; }
static MS void st_GetStartupInfoW(void *si)
{ if (si) { memset(si, 0, sizeof(W_STARTUPINFO));
            ((W_STARTUPINFO *)si)->cb = sizeof(W_STARTUPINFO); } }
static MS void *st_GetStdHandle(uint32_t n) { return (void *)(uintptr_t)(n == 0xFFFFFFF6u ? 1 : 2); }
static MS int32_t st_SetStdHandle(uint32_t n, void *h) { (void)n;(void)h; return 1; }
static MS void st_OutputDebugStringA(const char *s)
{ fprintf(stderr, "  [dbg] %s", s ? s : "(null)"); }

/* ------------------------------------------------------ locale / codepage */

static MS uint32_t st_GetACP(void) { return 1252; }
static MS uint32_t st_GetOEMCP(void) { return 437; }
static MS int32_t st_IsValidCodePage(uint32_t cp) { (void)cp; return 1; }
static MS int32_t st_GetCPInfo(uint32_t cp, void *info)
{
    (void)cp;
    if (!info) return 0;
    memset(info, 0, sizeof(W_CPINFO));
    ((W_CPINFO *)info)->MaxCharSize = 1;
    return 1;
}
static MS int32_t st_MultiByteToWideChar(uint32_t cp, uint32_t f, const char *in, int32_t inlen,
                                        uint16_t *out, int32_t outlen)
{
    int32_t n, i;
    (void)cp; (void)f;
    if (!in) return 0;
    n = (inlen < 0) ? (int32_t)strlen(in) + 1 : inlen;
    if (!out || !outlen) return n;
    for (i = 0; i < n && i < outlen; i++) out[i] = (uint8_t)in[i];
    return i;
}
static MS int32_t st_WideCharToMultiByte(uint32_t cp, uint32_t f, const uint16_t *in, int32_t inlen,
                                        char *out, int32_t outlen, const char *def, int32_t *used)
{
    int32_t n, i;
    (void)cp; (void)f; (void)def;
    if (used) *used = 0;
    if (!in) return 0;
    if (inlen < 0) { n = 0; while (in[n]) n++; n++; } else n = inlen;
    if (!out || !outlen) return n;
    for (i = 0; i < n && i < outlen; i++) out[i] = in[i] < 256 ? (char)in[i] : '?';
    return i;
}
static MS int32_t st_LCMapStringW(uint32_t l, uint32_t f, const uint16_t *in, int32_t inlen,
                                 uint16_t *out, int32_t outlen)
{
    int32_t n, i;
    (void)l;
    if (!in) return 0;
    if (inlen < 0) { n = 0; while (in[n]) n++; n++; } else n = inlen;
    if (!out || !outlen) return n;
    for (i = 0; i < n && i < outlen; i++)
        out[i] = (f & 0x100) ? (uint16_t)towlower(in[i])
               : (f & 0x200) ? (uint16_t)towupper(in[i]) : in[i];
    return i;
}
static MS int32_t st_CompareStringW(uint32_t l, uint32_t f, const uint16_t *a, int32_t na,
                                   const uint16_t *b, int32_t nb)
{
    int32_t i = 0;
    (void)l; (void)f;
    while ((na < 0 || i < na) && (nb < 0 || i < nb) && a[i] && b[i] && a[i] == b[i]) i++;
    if (a[i] == b[i]) return 2;
    return a[i] < b[i] ? 1 : 3;
}
static MS int32_t st_GetLocaleInfoW(uint32_t l, uint32_t t, uint16_t *out, int32_t n)
{ (void)l;(void)t; if (out && n) out[0] = 0; return 1; }
static MS uint32_t st_GetUserDefaultLCID(void) { return 0x0409; }
static MS int32_t st_IsValidLocale(uint32_t l, uint32_t f) { (void)l;(void)f; return 1; }
static MS int32_t st_GetStringTypeW(uint32_t t, const uint16_t *s, int32_t n, uint16_t *out)
{
    int32_t i;
    (void)t;
    if (!s || !out) return 0;
    if (n < 0) { n = 0; while (s[n]) n++; }
    for (i = 0; i < n; i++) {
        uint16_t c = 0;
        if (iswupper(s[i])) c |= 1;
        if (iswlower(s[i])) c |= 2;
        if (iswdigit(s[i])) c |= 4;
        if (iswspace(s[i])) c |= 8;
        if (iswpunct(s[i])) c |= 0x10;
        if (iswalpha(s[i])) c |= 0x100;
        out[i] = c;
    }
    return 1;
}
static MS int32_t st_EnumSystemLocalesW(void *fn, uint32_t f) { (void)fn;(void)f; return 1; }
static MS uint32_t st_GetTimeZoneInformation(void *tz) { if (tz) memset(tz, 0, 172); return 0; }

static MS int32_t st_lstrlenA(const char *s) { return s ? (int32_t)strlen(s) : 0; }
static MS char *st_lstrcpyA(char *d, const char *s) { return strcpy(d, s); }
static MS char *st_lstrcatA(char *d, const char *s) { return strcat(d, s); }
static MS int32_t st_lstrcmpA(const char *a, const char *b) { return strcmp(a, b); }
static MS int32_t st_lstrcmpiA(const char *a, const char *b) { return strcasecmp(a, b); }
/* CharNext/CharPrev, both widths.
 *
 * CharNextA was defined here but never registered, so it and its three
 * relatives all resolved to the generic stub -- which returns 0. That is fatal
 * rather than merely wrong: these return a pointer the caller immediately
 * dereferences, so a plugin walking a string with them faults on the first
 * step. Synth1 does exactly that inside VSTPluginMain, right after DllMain,
 * and dies at a null read with no AEffect ever returned.
 *
 * The pointer arithmetic is the single-byte reading of the contract. Windows
 * consults the thread's ANSI codepage here and steps two bytes over a DBCS
 * lead byte; everything this host does is Latin-1 already (see w32_vfmt's own
 * note), and a plugin formatting paths and preset names never sees the
 * difference. CharPrev's guard is the important half of its contract: at or
 * before the start of the string it must return the start, not walk behind it. */
static MS const char *st_CharNextA(const char *s) { return (s && *s) ? s + 1 : s; }
static MS const char *st_CharPrevA(const char *start, const char *cur)
{ return (start && cur && cur > start) ? cur - 1 : start; }
static MS const uint16_t *st_CharNextW(const uint16_t *s)
{ return (s && *s) ? s + 1 : s; }
static MS const uint16_t *st_CharPrevW(const uint16_t *start, const uint16_t *cur)
{ return (start && cur && cur > start) ? cur - 1 : start; }

/* --------------------------------------------------------------- SEH stubs */

/* Only reached when C++ actually throws. Stubbed so a throw fails loudly
 * rather than corrupting state; implementing real unwinding means parsing
 * .pdata/.xdata and is a separate piece of work. */
/* CONTEXT: 716 bytes on i386, 1232 on x86-64 */
static MS void st_RtlCaptureContext(void *c)
{ if (c) memset(c, 0, sizeof(void *) == 8 ? 1232 : 716); }
static MS void *st_RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *hist)
{ (void)pc;(void)hist; if (base) *base = (uint64_t)(uintptr_t)g_image_base; return NULL; }
static MS void *st_RtlVirtualUnwind(uint32_t t, uint64_t b, uint64_t pc, void *fn,
                                   void *ctx, void **data, uint64_t *frame, void *ptrs)
{ (void)t;(void)b;(void)pc;(void)fn;(void)ctx;(void)data;(void)frame;(void)ptrs; return NULL; }
static MS void *st_RtlPcToFileHeader(void *pc, void **base)
{ (void)pc; if (base) *base = g_image_base; return g_image_base; }
static MS void st_RtlUnwindEx(void *a, void *b, void *c, void *d, void *e, void *f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
  fprintf(stderr, "[win] RtlUnwindEx -- unwinding unsupported\n"); abort(); }
static MS int32_t st_RtlAddFunctionTable(void *t, uint32_t n, uint64_t b)
{ (void)t;(void)n;(void)b; return 1; }

/* Windows path separators.
 *
 * A plugin handed a path from this side will often rebuild it with backslashes
 * before handing it back: Absynth turned /home/user/... into \home\user\...
 * and then every open, mkdir and directory scan failed -- not because anything
 * was missing, but because the filesystem cannot use that spelling. Normalising
 * at each entry point costs one copy and removes the whole class of "but the
 * file is right there" failure.
 *
 * Only separators change. A backslash is a legal character in a Unix filename in
 * principle, but no path this host hands out contains one, so nothing that was
 * part of a name turns into a directory boundary. Drive letters are left alone:
 * there is no sensible place to point C: at, and failing to find it is the
 * honest outcome. */
static const char *peload_data_root(void);

/* Undo what a guest's own path arithmetic did to a path this host gave it.
 *
 * The host hands out real POSIX paths -- SHGetFolderPath answers with
 * `/home/you/.peload/AppData/Roaming` and so on -- and a plugin that treats
 * that as an opaque string is fine. Plugins do not treat it as opaque. They
 * run it through their own Windows path handling, where a leading `/` means
 * "the root of the current drive" rather than "the root", and it comes back
 * spelled a way the filesystem cannot use. Two shapes turned up in the corpus,
 * from the same cause in opposite directions:
 *
 *   Kontakt re-anchored it against the current drive and asked for
 *     C:/home/you/.peload/AppData/Local/Native Instruments/Kontakt 5/...
 *   FM8 and Absynth dropped the root marker instead and asked for
 *     home/you/.peload/Documents/Native Instruments/FM8/Sounds
 *
 * which is why a settings tree kept appearing in the working directory beside
 * the real one. Neither is a bug in the plugin: both are what Windows path
 * rules say those strings mean. Normalising them back here is one memmove and
 * removes the whole class, where translating at each of the thirty-odd entry
 * points that take a path would not.
 *
 * Backslashes are converted first, so everything below sees one separator. */
static char *path_norm_n(char *p, size_t n)
{
    char *q;
    size_t len;

    if (!p) return p;
    for (q = p; *q; q++) if (*q == '\\') *q = '/';

    /* The extended-length and device prefixes, \\?\ and \\.\ , now spelled
     * with forward slashes. Dropping them leaves the drive letter below. */
    if (!strncmp(p, "//?/", 4) || !strncmp(p, "//./", 4))
        memmove(p, p + 4, strlen(p + 4) + 1);

    /* A drive letter. There are no drives here, and the only way one appears
     * is a guest having anchored one of our own paths against the current
     * drive, so what follows it is the path we originally handed out. */
    if (p[0] && p[1] == ':' &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) {
        memmove(p, p + 2, strlen(p + 2) + 1);
        if (!p[0]) { p[0] = '/'; p[1] = 0; }
    }

    /* The opposite slip: our own data root with its leading slash eaten. Only
     * that prefix is re-anchored -- a genuinely relative path a plugin chose
     * for itself is left alone, exactly as make_parents_in_root leaves it. */
    if (p[0] != '/') {
        const char *root = peload_data_root();
        if (root && root[0] == '/') {
            size_t rl = strlen(root) - 1;              /* root without its '/' */
            if (!strncmp(p, root + 1, rl) &&
                (p[rl] == '/' || p[rl] == 0)) {
                len = strlen(p);
                if (len + 2 <= n) {
                    memmove(p + 1, p, len + 1);
                    p[0] = '/';
                }
            }
        }
    }
    return p;
}
static char *w2c_path(const uint16_t *w, char *out, size_t n)
{ w2c(w, out, n); return path_norm_n(out, n); }
static const char *path_fix(const char *in, char *buf, size_t n)
{ if (!in) return NULL; snprintf(buf, n, "%s", in); return path_norm_n(buf, n); }

/* The root of the directory tree this host hands out through SHGetFolderPath.
 * Everything below it is ours to manage; nothing above it is touched. */
static const char *peload_data_root(void)
{
    static char root[512];
    if (!root[0]) {
        const char *home = getenv("HOME");
        snprintf(root, sizeof root, "%s/.peload", (home && *home) ? home : "/tmp");
    }
    return root;
}

/* Make the parent directories of a file about to be created.
 *
 * A plugin asks for its settings folder, builds a path underneath it and opens a
 * file there, expecting its installer to have created the intermediate
 * directories. There is no installer here, so the host creates them -- every NI
 * instrument wants a different one (`Native Instruments/<product>/`), and doing it
 * by hand per plugin does not scale and does not help anyone else.
 *
 * Deliberately limited to the tree above: a path the plugin chose somewhere else
 * on the filesystem is left alone, and a failure to open it stays a failure. */
static void make_parents_in_root(const char *path)
{
    const char *root = peload_data_root();
    size_t rlen = strlen(root);
    char tmp[1024];
    size_t i;

    if (!path || strncmp(path, root, rlen) != 0 || path[rlen] != '/') return;
    snprintf(tmp, sizeof tmp, "%s", path);
    /* From just past the root, so the root itself is created too if need be. */
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        mkdir(tmp, 0755);
        tmp[i] = '/';
    }
}

/* --------------------------------------------- handles: files, events, threads */

/* One table for every kernel object, because Windows HANDLEs are opaque and
 * CloseHandle/WaitForSingleObject must work on any of them. Handles are table
 * indices biased away from 0 and from the -1/-2 pseudo-handles above. */
typedef enum { H_FREE = 0, H_FILE, H_EVENT, H_MUTEX, H_SEM, H_THREAD, H_MAP } htype;
typedef struct {
    htype           type;
    int             fd;
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             signaled, manual, count;
    pthread_t       th;
    void           *start, *param;
    uint64_t        map_size;          /* H_MAP: the section's size */
    int             map_prot;          /* H_MAP: the mmap protection */
    int             map_own_fd;        /* H_MAP: we made the backing file */
    int             borrowed;          /* H_FILE: the CRT owns this fd, not us */
    char            map_name[64];      /* H_MAP: the section's name, if any */
} hobj;

#define H_BIAS 0x100
#define H_MAX  1024
static hobj           *g_hobj[H_MAX];
static int             g_hnext = 1;
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;

static void *h_new(htype t)
{
    hobj *o = calloc(1, sizeof *o);
    int i;
    if (!o) return NULL;
    o->type = t;
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->c, NULL);
    pthread_mutex_lock(&g_hlock);
    i = (g_hnext < H_MAX) ? g_hnext++ : 0;
    if (i) g_hobj[i] = o;
    pthread_mutex_unlock(&g_hlock);
    if (!i) { free(o); return NULL; }
    return (void *)(intptr_t)(i + H_BIAS);
}
static hobj *h_get(void *h, htype want)
{
    intptr_t i = (intptr_t)h - H_BIAS;
    hobj *o;
    if (i <= 0 || i >= H_MAX) return NULL;
    o = g_hobj[i];
    if (!o || (want != H_FREE && o->type != want)) return NULL;
    return o;
}

/* wide -> narrow for the W entry points; plugin paths here are ASCII */
static void w2c(const uint16_t *w, char *out, size_t n)
{
    size_t i = 0;
    if (!w) { if (n) out[0] = 0; return; }
    for (; w[i] && i + 1 < n; i++) out[i] = w[i] < 256 ? (char)w[i] : '?';
    out[i] = 0;
}

static void *file_open(const char *name, uint32_t access, uint32_t disp)
{
    int fl, fd;
    hobj *o;
    void *h;
    if ((access & 0x40000000u) && (access & 0x80000000u)) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    if (disp == 2) fl |= O_CREAT | O_TRUNC;        /* CREATE_ALWAYS */
    else if (disp == 1) fl |= O_CREAT | O_EXCL;    /* CREATE_NEW    */
    else if (disp == 4) fl |= O_CREAT;             /* OPEN_ALWAYS   */
    {   /* Normalised here as well: this is the one point every open passes
         * through, whichever entry point was called. */
        char np[1024];
        path_fix(name, np, sizeof np);
        fd = open(np, fl, 0644);
        /* A create that failed only because its directory is missing: make the
         * directories, inside this host's own tree, and try once more. */
        if (fd < 0 && errno == ENOENT && (fl & O_CREAT)) {
            make_parents_in_root(np);
            fd = open(np, fl, 0644);
        }
        /* Logged because a file that is never opened looks identical to one that
         * is opened and read as empty, and the two want opposite investigations. */
        PLOG("  [win] open(%s) %s%s%s -> %s\n", np,
             (fl & O_RDWR) ? "rw" : (fl & O_WRONLY) ? "w" : "r",
             (fl & O_CREAT) ? "+create" : "", (fl & O_TRUNC) ? "+trunc" : "",
             fd < 0 ? strerror(errno) : "ok");
    }
    if (fd < 0) { g_last_error = 2; return (void *)(intptr_t)-1; }
    if (!(h = h_new(H_FILE))) { close(fd); return (void *)(intptr_t)-1; }
    o = h_get(h, H_FILE); o->fd = fd;
    return h;
}
static MS void *st_CreateFileA(const char *name, uint32_t access, uint32_t share,
                              void *sa, uint32_t disp, uint32_t flags, void *tmpl)
{ char p[1024]; (void)share;(void)sa;(void)flags;(void)tmpl;
  return file_open(path_fix(name, p, sizeof p), access, disp); }
static MS void *st_CreateFileW(const uint16_t *name, uint32_t access, uint32_t share,
                              void *sa, uint32_t disp, uint32_t flags, void *tmpl)
{
    char p[1024];
    (void)share;(void)sa;(void)flags;(void)tmpl;
    w2c_path(name, p, sizeof p);
    return file_open(p, access, disp);
}
/* OVERLAPPED, as both the file transfers and the locking calls receive it. Only
 * the offset pair matters here: nothing is asynchronous, so Internal and hEvent
 * are never consulted. */
typedef struct {
    uintptr_t Internal, InternalHigh;
    uint32_t  Offset, OffsetHigh;
    void     *hEvent;
} W32OVERLAPPED;

/* An OVERLAPPED carries the offset to read or write at, and ignoring it is not a
 * missing feature but a wrong answer: the transfer happens wherever the file
 * pointer happens to be.
 *
 * SQLite's Windows VFS reads and writes every page this way. With the offset
 * dropped, Absynth's database was *written* correctly -- those writes ran in
 * order -- and then read back from the end of the file, so the header came out as
 * whatever was there and SQLite reported "file is encrypted or is not a
 * database". A real sqlite3 opened the same file without complaint, which is what
 * pointed at the reader rather than the writer.
 *
 * Windows updates the file pointer after a positioned transfer on a synchronous
 * handle, so that is mirrored here; the transfer itself uses pread/pwrite so the
 * offset cannot be disturbed by another thread between the seek and the read. */
static uint64_t ov_offset(const void *ov, int *positioned)
{
    const W32OVERLAPPED *o = ov;
    *positioned = ov != NULL;
    if (!o) return 0;
    return ((uint64_t)o->OffsetHigh << 32) | o->Offset;
}

static MS int32_t st_ReadFile(void *h, void *buf, uint32_t n, uint32_t *got, void *ov)
{
    hobj *o = h_get(h, H_FILE);
    ssize_t r;
    int positioned;
    uint64_t off = ov_offset(ov, &positioned);

    if (!o) { g_last_error = 6; return 0; }
    if (positioned) {
        r = pread(o->fd, buf, n, (off_t)off);
        if (r > 0) lseek(o->fd, (off_t)(off + (uint64_t)r), SEEK_SET);
    } else {
        r = read(o->fd, buf, n);
    }
    if (got) *got = r > 0 ? (uint32_t)r : 0;
    if (r < 0) g_last_error = 30;               /* ERROR_READ_FAULT */
    return r >= 0;
}
static MS int32_t st_WriteFile(void *h, const void *buf, uint32_t n, uint32_t *put, void *ov)
{
    hobj *o = h_get(h, H_FILE);
    ssize_t r;
    int positioned;
    uint64_t off = ov_offset(ov, &positioned);

    if (!o) {   /* stdout/stderr pseudo-handles from GetStdHandle */
        if (put) *put = n;
        return 1;
    }
    if (positioned) {
        r = pwrite(o->fd, buf, n, (off_t)off);
        if (r > 0) lseek(o->fd, (off_t)(off + (uint64_t)r), SEEK_SET);
    } else {
        r = write(o->fd, buf, n);
    }
    if (put) *put = r > 0 ? (uint32_t)r : 0;
    if (r < 0) g_last_error = 29;               /* ERROR_WRITE_FAULT */
    return r >= 0;
}
static MS int32_t st_CloseHandle(void *h)
{
    intptr_t i = (intptr_t)h - H_BIAS;
    hobj *o = h_get(h, H_FREE);
    if (!o) return 1;
    if (o->type == H_FILE && o->fd >= 0 && !o->borrowed) close(o->fd);
    if (o->type == H_MAP && o->map_own_fd && o->fd >= 0) close(o->fd);
    pthread_mutex_lock(&g_hlock);
    g_hobj[i] = NULL;
    pthread_mutex_unlock(&g_hlock);
    pthread_mutex_destroy(&o->m);
    pthread_cond_destroy(&o->c);
    free(o);
    return 1;
}
static MS uint32_t st_SetFilePointer(void *h, int32_t lo, int32_t *hi, uint32_t whence)
{
    hobj *o = h_get(h, H_FILE);
    off_t off = lo;
    if (!o) return 0xFFFFFFFFu;
    if (hi) off |= ((off_t)*hi) << 32;
    off = lseek(o->fd, off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    if (hi) *hi = (int32_t)(off >> 32);
    return (uint32_t)off;
}
static MS uint32_t st_GetFileSize(void *h, uint32_t *hi)
{
    hobj *o = h_get(h, H_FILE);
    struct stat s;
    if (!o || fstat(o->fd, &s)) return 0xFFFFFFFFu;
    if (hi) *hi = (uint32_t)(s.st_size >> 32);
    return (uint32_t)s.st_size;
}
/* _get_osfhandle bridges the CRT's file descriptors to Win32 HANDLEs, and it
 * is not a nicety: Kontakt opens its database with the CRT, asks for the
 * handle, and maps the file. A stub answering zero sent CreateFileMapping down
 * the anonymous path instead, so the "database" it went on to parse was a
 * zeroed page -- and the length prefix at the front of it, being all zeros,
 * never terminated. The handle is borrowed, exactly as on Windows: closing it
 * is the caller's bug, not ours, so CloseHandle leaves the descriptor alone. */
static MSCRT intptr_t st__get_osfhandle(int fd)
{
    hobj *o;
    void *h;
    int i;
    if (fd < 0) return -1;
    for (i = 1; i < H_MAX; i++) {
        o = g_hobj[i];
        if (o && o->type == H_FILE && o->fd == fd)
            return (intptr_t)(i + H_BIAS);
    }
    if (!(h = h_new(H_FILE))) return -1;
    o = h_get(h, H_FILE);
    o->fd = fd;
    o->borrowed = 1;
    return (intptr_t)h;
}
static MSCRT int st__open_osfhandle(intptr_t h, int flags)
{
    hobj *o = h_get((void *)h, H_FILE);
    (void)flags;
    return o ? o->fd : -1;
}

static MS int32_t st_CloseHandle(void *h);

/* ------------------------------------------------------ file mapping ------
 *
 * Every binary in the corpus imports CreateFileMapping and MapViewOfFile, and
 * a stub answering NULL is not a small thing: mapping a file is how a plug-in
 * reads its sample bank, its wavetables or its preset library without copying
 * them, and the fallback path -- when there is one -- reads the whole file into
 * the heap instead. It is mmap either way, so this is the one place where doing
 * it properly is less work than pretending.
 *
 * A mapping with no file behind it (hFile == INVALID_HANDLE_VALUE) is anonymous
 * memory, which is how Windows programs share a block within a process; those
 * get a name so OpenFileMapping can find them again. */
enum {
    W_PAGE_NOACCESS = 0x01, W_PAGE_READONLY = 0x02, W_PAGE_READWRITE = 0x04,
    W_PAGE_WRITECOPY = 0x08, W_PAGE_EXECUTE_READ = 0x20,
    W_PAGE_EXECUTE_READWRITE = 0x40
};
enum {
    W_FILE_MAP_COPY = 1, W_FILE_MAP_WRITE = 2, W_FILE_MAP_READ = 4,
    W_FILE_MAP_EXECUTE = 0x20
};

typedef struct { void *base; size_t len; } w32_view;
#define W32_VIEW_MAX 256
static w32_view g_views[W32_VIEW_MAX];
static pthread_mutex_t g_view_lock = PTHREAD_MUTEX_INITIALIZER;

static void w32_view_note(void *base, size_t len)
{
    int i;
    pthread_mutex_lock(&g_view_lock);
    for (i = 0; i < W32_VIEW_MAX; i++)
        if (!g_views[i].base) { g_views[i].base = base; g_views[i].len = len; break; }
    pthread_mutex_unlock(&g_view_lock);
}
static size_t w32_view_take(void *base)
{
    size_t n = 0;
    int i;
    pthread_mutex_lock(&g_view_lock);
    for (i = 0; i < W32_VIEW_MAX; i++)
        if (g_views[i].base == base) { n = g_views[i].len; g_views[i].base = NULL; break; }
    pthread_mutex_unlock(&g_view_lock);
    return n;
}

static int w32_prot_of(uint32_t page)
{
    switch (page & 0xFF) {
    case W_PAGE_NOACCESS:              return PROT_NONE;
    case W_PAGE_READONLY:              return PROT_READ;
    case W_PAGE_WRITECOPY:             return PROT_READ | PROT_WRITE;
    case W_PAGE_EXECUTE_READ:          return PROT_READ | PROT_EXEC;
    case W_PAGE_EXECUTE_READWRITE:     return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                           return PROT_READ | PROT_WRITE;
    }
}

static void *w32_create_mapping(void *hfile, uint32_t page, uint32_t hi, uint32_t lo,
                                const char *name)
{
    hobj *f = h_get(hfile, H_FILE);
    uint64_t size = ((uint64_t)hi << 32) | lo;
    void *h;
    hobj *m;
    int i;

    /* A name already in use is that same section, which is the point of names. */
    if (name && *name)
        for (i = 1; i < H_MAX; i++) {
            hobj *o = g_hobj[i];
            if (o && o->type == H_MAP && !strcmp(o->map_name, name)) {
                g_last_error = 183;                 /* ERROR_ALREADY_EXISTS */
                return (void *)(intptr_t)(i + H_BIAS);
            }
        }
    if (!f && hfile != (void *)(intptr_t)-1 && hfile != NULL) {
        g_last_error = 6;
        return NULL;
    }
    if (f && !size) {
        struct stat st;
        if (fstat(f->fd, &st) == 0) size = (uint64_t)st.st_size;
    }
    if (!size) { g_last_error = 87; return NULL; }
    if (f && (page & 0xFF) != W_PAGE_READONLY) {
        struct stat st;                              /* grow to the asked-for size */
        if (fstat(f->fd, &st) == 0 && (uint64_t)st.st_size < size)
            if (ftruncate(f->fd, (off_t)size) != 0) { g_last_error = 5; return NULL; }
    }
    if (!(h = h_new(H_MAP))) { g_last_error = 8; return NULL; }
    m = h_get(h, H_MAP);
    m->fd = f ? f->fd : -1;
    if (!f) {
        /* A section with no file behind it is still *shared*: two views of one
         * section see the same bytes, which is the whole point of naming them.
         * MAP_ANONYMOUS gives every view its own zeroed pages instead, and a
         * plug-in that writes a structure through one view and reads it back
         * through another gets zeros -- which is a null pointer as soon as the
         * structure holds one. A memfd is anonymous memory that mmap can share. */
        int fd = memfd_create(name && *name ? name : "peload-section", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
            if (fd >= 0) close(fd);
            st_CloseHandle(h);
            g_last_error = 8;
            return NULL;
        }
        m->fd = fd;
        m->map_own_fd = 1;
    }
    m->map_size = size;
    m->map_prot = w32_prot_of(page);
    if (name && *name) snprintf(m->map_name, sizeof m->map_name, "%s", name);
    /* A fresh section reports no error at all, and that is load-bearing: the
     * caller distinguishes "I created this, so I must fill it in" from "it was
     * already there, so it already holds something" by asking GetLastError for
     * ERROR_ALREADY_EXISTS. Leaving whatever the last call happened to set had
     * Kontakt read an uninitialised section as though another process had
     * written it, and walk a 7-bit length prefix off the end of the page. */
    g_last_error = 0;
    return h;
}
static MS void *st_CreateFileMappingA(void *hfile, void *sa, uint32_t page,
                                      uint32_t hi, uint32_t lo, const char *name)
{
    void *h;
    (void)sa;
    h = w32_create_mapping(hfile, page, hi, lo, name);
    PLOG("  [win] CreateFileMappingA(%s, %u bytes) -> %p\n",
         name ? name : "(unnamed)", (unsigned)(((uint64_t)hi << 32) | lo), h);
    return h;
}
static MS void *st_CreateFileMappingW(void *hfile, void *sa, uint32_t page,
                                      uint32_t hi, uint32_t lo, const uint16_t *name)
{
    char b[64];
    if (name) w2c(name, b, sizeof b); else b[0] = 0;
    return st_CreateFileMappingA(hfile, sa, page, hi, lo, name ? b : NULL);
}
static MS void *st_OpenFileMappingA(uint32_t access, int32_t inherit, const char *name)
{
    int i;
    (void)access; (void)inherit;
    if (!name) { g_last_error = 87; return NULL; }
    for (i = 1; i < H_MAX; i++) {
        hobj *o = g_hobj[i];
        if (o && o->type == H_MAP && !strcmp(o->map_name, name))
            return (void *)(intptr_t)(i + H_BIAS);
    }
    g_last_error = 2;                                /* ERROR_FILE_NOT_FOUND */
    return NULL;
}
static MS void *st_OpenFileMappingW(uint32_t access, int32_t inherit, const uint16_t *name)
{ char b[64]; w2c(name, b, sizeof b); return st_OpenFileMappingA(access, inherit, b); }

static MS void *st_MapViewOfFileEx(void *hmap, uint32_t access, uint32_t hi,
                                   uint32_t lo, size_t bytes, void *want)
{
    hobj *m = h_get(hmap, H_MAP);
    uint64_t off = ((uint64_t)hi << 32) | lo;
    int prot = 0, flags;
    void *p;

    if (!m) { g_last_error = 6; return NULL; }
    if (access & (W_FILE_MAP_READ | W_FILE_MAP_COPY)) prot |= PROT_READ;
    if (access & (W_FILE_MAP_WRITE | W_FILE_MAP_COPY)) prot |= PROT_READ | PROT_WRITE;
    if (access & W_FILE_MAP_EXECUTE) prot |= PROT_EXEC;
    if (!prot) prot = m->map_prot;
    if (!bytes) bytes = (size_t)(m->map_size - off);
    /* FILE_MAP_COPY is copy-on-write: the file must not see the writes. */
    flags = (access & W_FILE_MAP_COPY) ? MAP_PRIVATE : MAP_SHARED;
    p = mmap(want, bytes, prot, flags, m->fd, (off_t)off);
    PLOG("  [win] MapViewOfFile(fd=%d off=%llu bytes=%zu prot=%d) -> %p\n",
         m->fd, (unsigned long long)off, bytes, prot, p == MAP_FAILED ? NULL : p);
    if (p == MAP_FAILED) { g_last_error = 8; return NULL; }
    w32_view_note(p, bytes);
    return p;
}
static MS void *st_MapViewOfFile(void *hmap, uint32_t access, uint32_t hi,
                                 uint32_t lo, size_t bytes)
{ return st_MapViewOfFileEx(hmap, access, hi, lo, bytes, NULL); }
static MS int32_t st_UnmapViewOfFile(void *base)
{
    size_t n = w32_view_take(base);
    if (!n) { g_last_error = 487; return 0; }        /* ERROR_INVALID_ADDRESS */
    return munmap(base, n) == 0;
}
static MS int32_t st_FlushViewOfFile(void *base, size_t bytes)
{
    if (!bytes) bytes = 4096;
    return msync(base, bytes, MS_SYNC) == 0;
}

/* The 64-bit file positions, which every modern CRT uses in place of the
 * DWORD-and-a-pointer pair. */
static MS int32_t st_GetFileSizeEx(void *h, int64_t *size)
{
    hobj *o = h_get(h, H_FILE);
    struct stat st;
    if (!o || !size || fstat(o->fd, &st)) { g_last_error = 6; return 0; }
    *size = (int64_t)st.st_size;
    return 1;
}
static MS int32_t st_SetFilePointerEx(void *h, int64_t off, int64_t *newpos,
                                      uint32_t whence)
{
    hobj *o = h_get(h, H_FILE);
    off_t r;
    if (!o) { g_last_error = 6; return 0; }
    r = lseek(o->fd, (off_t)off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    if (r == (off_t)-1) { g_last_error = 87; return 0; }
    if (newpos) *newpos = (int64_t)r;
    return 1;
}

static MS int32_t st_FlushFileBuffers(void *h)
{ hobj *o = h_get(h, H_FILE); return o ? fsync(o->fd) == 0 : 1; }
static MS uint32_t st_GetFileType(void *h) { (void)h; return 1; /* FILE_TYPE_DISK */ }
static MS int32_t st_SetEndOfFile(void *h)
{ hobj *o = h_get(h, H_FILE); return o ? ftruncate(o->fd, lseek(o->fd, 0, SEEK_CUR)) == 0 : 0; }
/* Normalised here as well as in the wide form: a plugin may build the narrow
 * spelling itself. */
static MS uint32_t st_GetFileAttributesA(const char *p)
{ struct stat s; char b[1024];
  if (!p || stat(path_fix(p, b, sizeof b), &s)) return 0xFFFFFFFFu;
  return S_ISDIR(s.st_mode) ? 0x10 : 0x80; }
static MS uint32_t st_GetFileAttributesW(const uint16_t *p)
{ char b[1024]; return st_GetFileAttributesA(w2c_path(p, b, sizeof b)); }
static MS int32_t st_DeleteFileA(const char *p)
{ char b[1024]; return p && unlink(path_fix(p, b, sizeof b)) == 0; }
static MS int32_t st_DeleteFileW(const uint16_t *p)
{ char b[1024]; return unlink(w2c_path(p, b, sizeof b)) == 0; }
static MS int32_t st_GetFileInformationByHandle(void *h, void *info)
{ (void)h; if (info) memset(info, 0, sizeof(W_BY_HANDLE_FILE_INFORMATION)); return 1; }

/* ------------------------------------------------------- events / semaphores */

static MS void *st_CreateEventA(void *sa, int32_t manual, int32_t initial, const char *name)
{
    void *h = h_new(H_EVENT);
    hobj *o;
    (void)sa; (void)name;
    if (!h) return NULL;
    o = h_get(h, H_EVENT);
    o->manual = manual; o->signaled = initial;
    return h;
}
static MS void *st_CreateEventW(void *sa, int32_t manual, int32_t initial, const uint16_t *name)
{ (void)name; return st_CreateEventA(sa, manual, initial, NULL); }

/* Directory change notification.
 *
 * A plug-in watches its own folder so it can pick up a skin or a module
 * dropped in beside it while it runs. Nothing here rewrites a plug-in's
 * directory underneath it, so a handle that is valid and never signals is the
 * honest answer -- and it is what the caller needs, because the *handle* is
 * what gets checked. Returning NULL was read as a failure of the host rather
 * than as "no changes": SynthEdit printed "ERROR: Unexpected NULL from
 * FindFirstChangeNotification" and abandoned building its DSP graph, which
 * then rendered silence with no other symptom.
 *
 * An unsignalled event is exactly the right object: WaitForSingleObject on it
 * times out, which is what a watcher polling for changes expects to see. */
static MS void *st_FindFirstChangeNotificationA(const char *path, int32_t sub, uint32_t filter)
{ (void)path; (void)sub; (void)filter; return st_CreateEventA(NULL, 1, 0, NULL); }
static MS void *st_FindFirstChangeNotificationW(const uint16_t *path, int32_t sub, uint32_t filter)
{ (void)path; (void)sub; (void)filter; return st_CreateEventA(NULL, 1, 0, NULL); }
static MS int32_t st_FindNextChangeNotification(void *h)
{ return h != NULL; }
static MS int32_t st_FindCloseChangeNotification(void *h)
{ return st_CloseHandle(h); }
static MS void *st_CreateEventExW(void *sa, const uint16_t *name, uint32_t flags, uint32_t acc)
{
    (void)acc; (void)name;
    /* CREATE_EVENT_MANUAL_RESET = 1, CREATE_EVENT_INITIAL_SET = 2 */
    return st_CreateEventA(sa, (flags & 1) != 0, (flags & 2) != 0, NULL);
}
static MS int32_t st_SetEvent(void *h)
{
    hobj *o = h_get(h, H_EVENT);
    if (!o) return 0;
    pthread_mutex_lock(&o->m);
    o->signaled = 1;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return 1;
}
static MS int32_t st_ResetEvent(void *h)
{
    hobj *o = h_get(h, H_EVENT);
    if (!o) return 0;
    pthread_mutex_lock(&o->m);
    o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return 1;
}
static MS uint32_t st_WaitForSingleObject(void *h, uint32_t ms)
{
    hobj *o = h_get(h, H_FREE);
    if (!o) return 0xFFFFFFFFu;                    /* WAIT_FAILED */
    if (o->type == H_THREAD) { pthread_join(o->th, NULL); return 0; }
    pthread_mutex_lock(&o->m);
    while (!o->signaled) {
        if (ms == 0) { pthread_mutex_unlock(&o->m); return 0x102; /* TIMEOUT */ }
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&o->c, &o->m);
        else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&o->c, &o->m, &ts) != 0) {
                pthread_mutex_unlock(&o->m); return 0x102;
            }
        }
    }
    if (!o->manual) o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return 0;                                       /* WAIT_OBJECT_0 */
}
static MS uint32_t st_WaitForSingleObjectEx(void *h, uint32_t ms, int32_t alert)
{ (void)alert; return st_WaitForSingleObject(h, ms); }

/* WaitForMultipleObjects.
 *
 * A stub for this is worse than a missing one: it returns 0, which is
 * WAIT_OBJECT_0 -- "the first handle signalled". A plug-in watching its own
 * directory reads that as "your files changed" and does so again on every
 * pass, so a watcher that should idle instead rebuilds for ever.
 *
 * Polled rather than waited on properly. A correct implementation needs one
 * condition variable shared by every object in the set, which the handle table
 * here is not built for; polling costs a millisecond of latency on a path that
 * is waiting for something to happen anyway, and it gets the *answers* right,
 * which is the part callers branch on.
 *
 * bWaitAll asks for every handle at once; anything else returns as soon as one
 * is ready, and the index of that one is the return value. */
static MS uint32_t st_WaitForMultipleObjects(uint32_t n, void *const *h,
                                             int32_t all, uint32_t ms)
{
    uint32_t waited = 0, i;
    if (!n || !h) return 0xFFFFFFFFu;                     /* WAIT_FAILED */
    for (;;) {
        uint32_t ready = 0;
        for (i = 0; i < n; i++) {
            if (st_WaitForSingleObject(h[i], 0) == 0) {
                if (!all) return i;                        /* WAIT_OBJECT_0 + i */
                ready++;
            }
        }
        if (all && ready == n) return 0;
        if (ms == 0) return 0x102;                         /* WAIT_TIMEOUT */
        if (ms != 0xFFFFFFFFu && waited >= ms) return 0x102;
        { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
        waited++;
    }
}
static MS uint32_t st_WaitForMultipleObjectsEx(uint32_t n, void *const *h,
                                               int32_t all, uint32_t ms, int32_t alert)
{ (void)alert; return st_WaitForMultipleObjects(n, h, all, ms); }
static MS void *st_CreateSemaphoreA(void *sa, int32_t init, int32_t max, const char *n)
{
    void *h = h_new(H_SEM);
    hobj *o;
    (void)sa;(void)max;(void)n;
    if (!h) return NULL;
    o = h_get(h, H_SEM); o->count = init; o->signaled = init > 0; o->manual = 0;
    return h;
}
static MS void *st_CreateSemaphoreW(void *sa, int32_t i, int32_t m, const uint16_t *n)
{ (void)n; return st_CreateSemaphoreA(sa, i, m, NULL); }
static MS int32_t st_ReleaseSemaphore(void *h, int32_t n, int32_t *prev)
{
    hobj *o = h_get(h, H_SEM);
    if (!o) return 0;
    pthread_mutex_lock(&o->m);
    if (prev) *prev = o->count;
    o->count += n; o->signaled = o->count > 0;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return 1;
}
static MS void *st_CreateMutexA(void *sa, int32_t owned, const char *n)
{
    void *h = h_new(H_MUTEX);
    hobj *o;
    (void)sa;(void)n;
    if (!h) return NULL;
    o = h_get(h, H_MUTEX); o->signaled = !owned; o->manual = 0;
    return h;
}
static MS int32_t st_ReleaseMutex(void *h) { return st_SetEvent(h); }

static MS int32_t st_SleepConditionVariableCS(void *cv, void *cs, uint32_t ms)
{ (void)cv; st_LeaveCriticalSection(cs); st_Sleep(ms == 0xFFFFFFFFu ? 1 : ms);
  st_EnterCriticalSection(cs); return 1; }
static MS int32_t st_SleepConditionVariableSRW(void *cv, void *l, uint32_t ms, uint32_t f)
{ (void)cv;(void)f; st_LeaveCriticalSection(l); st_Sleep(ms == 0xFFFFFFFFu ? 1 : ms);
  st_EnterCriticalSection(l); return 1; }

/* ------------------------------------------------------------------ threads */

/* Every thread that will run plugin code needs its own fake TEB, or the first
 * gs:[0x30] access on that thread faults. */
/* How many threads are inside the plug-in's own code right now.
 *
 * The image is unmapped when the plug-in closes, and a thread still running in
 * it at that moment faults on whatever instruction it was about to execute --
 * a segfault on a thread nobody is watching, after every line the run was
 * going to print. Windows has the same hazard and leaves it to the plug-in to
 * shut its threads down; this host cannot rely on that, so it counts them and
 * declines to unmap while any are left. */
static volatile int g_guest_threads;
int w32_guest_threads(void) { return g_guest_threads; }

static void *thread_trampoline(void *ud)
{
    hobj *o = ud;
    MS uint32_t (*start)(void *) = (MS uint32_t (*)(void *))o->start;
    teb_install();
    __sync_fetch_and_add(&g_guest_threads, 1);
    start(o->param);
    __sync_fetch_and_sub(&g_guest_threads, 1);
    return NULL;
}
static MS void *st_CreateThread(void *sa, size_t stack, void *start, void *param,
                               uint32_t flags, uint32_t *tid)
{
    void *h = h_new(H_THREAD);
    hobj *o;
    (void)sa; (void)stack; (void)flags;
    if (!h) return NULL;
    o = h_get(h, H_THREAD);
    o->start = start; o->param = param;
    if (pthread_create(&o->th, NULL, thread_trampoline, o) != 0) { st_CloseHandle(h); return NULL; }
    if (tid) *tid = (uint32_t)(uintptr_t)o->th;
    return h;
}
/* The thread-pool wait registration.
 *
 * A stub answering zero is "registration failed", and the Concurrency runtime
 * turns that straight into scheduler_resource_allocation_error -- the last of
 * the four things standing between the 32-bit NI plug-ins and a working
 * scheduler. What it asks for is genuinely simple: wait on the object, then
 * call back on some other thread. A thread each is not how Windows does it,
 * but it is the same contract, and the callback runs where the caller expects
 * it to -- not on the thread that registered it. */
/* Threads this layer started that will call back into the plug-in.
 *
 * Each of them holds a function pointer into the plug-in's image, so each has
 * to be stopped before that image is unmapped -- the same reason w32_reset
 * drops the windows and the timers. A wait or a timer still running when the
 * plug-in goes calls its callback through freed memory, which is a segfault
 * with no handler on it and nothing in the log: the crash lands after every
 * line the run was going to print. */
#define W32_WORKER_MAX 256
static void *g_w32_waits[W32_WORKER_MAX];
static void *g_w32_tps[W32_WORKER_MAX];
static int   g_w32_nwaits, g_w32_ntps;
static volatile int g_w32_worker_quit;      /* set once teardown starts */
static volatile int g_w32_work_running;     /* detached QueueUserWorkItem threads */
static pthread_mutex_t g_w32_worker_lock = PTHREAD_MUTEX_INITIALIZER;

static void w32_worker_add(void **list, int *n, void *p)
{
    pthread_mutex_lock(&g_w32_worker_lock);
    if (*n < W32_WORKER_MAX) list[(*n)++] = p;
    pthread_mutex_unlock(&g_w32_worker_lock);
}
static void w32_worker_drop(void **list, int *n, void *p)
{
    int i;
    pthread_mutex_lock(&g_w32_worker_lock);
    for (i = 0; i < *n; i++)
        if (list[i] == p) { list[i] = list[--(*n)]; break; }
    pthread_mutex_unlock(&g_w32_worker_lock);
}

typedef struct {
    void    *obj;                 /* the handle waited on */
    void    *cb, *ctx;            /* WAITORTIMERCALLBACK and its parameter */
    uint32_t ms, flags;
    volatile int stop, running;
    pthread_t th;
} w32_wait;

#define W32_WT_EXECUTEONLYONCE 0x00000008u

static void *w32_wait_thread(void *ud)
{
    w32_wait *w = ud;
    teb_install();
    while (!w->stop) {
        uint32_t r = st_WaitForSingleObject(w->obj, w->ms);
        if (w->stop || g_w32_worker_quit) break;
        if (r == 0xFFFFFFFFu) break;                      /* WAIT_FAILED */
        if (w->cb) {
            void (MS *cb)(void *, uint8_t) = (void (MS *)(void *, uint8_t))w->cb;
            __sync_fetch_and_add(&g_guest_threads, 1);
            cb(w->ctx, (uint8_t)(r == 0x102));            /* TimerOrWaitFired */
            __sync_fetch_and_sub(&g_guest_threads, 1);
        }
        if (w->flags & W32_WT_EXECUTEONLYONCE) break;
    }
    w->running = 0;
    return NULL;
}
static MS int32_t st_RegisterWaitForSingleObject(void **out, void *obj, void *cb,
                                                 void *ctx, uint32_t ms, uint32_t flags)
{
    w32_wait *w;
    if (!out) { g_last_error = 87; return 0; }
    if (!(w = calloc(1, sizeof *w))) { g_last_error = 8; return 0; }
    w->obj = obj; w->cb = cb; w->ctx = ctx; w->ms = ms; w->flags = flags;
    w->running = 1;
    if (pthread_create(&w->th, NULL, w32_wait_thread, w) != 0) {
        free(w);
        g_last_error = 8;
        PLOG("  [win] RegisterWaitForSingleObject: thread creation failed\n");
        return 0;
    }
    *out = w;
    w32_worker_add(g_w32_waits, &g_w32_nwaits, w);
    PLOG("  [win] RegisterWaitForSingleObject(obj=%p ms=%u flags=0x%x) -> %p\n",
         obj, ms, flags, (void *)w);
    return 1;
}
/* The Ex form returns the wait handle itself rather than writing it out. */
static MS void *st_RegisterWaitForSingleObjectEx(void *obj, void *cb, void *ctx,
                                                 uint32_t ms, uint32_t flags)
{
    void *h = NULL;
    return st_RegisterWaitForSingleObject(&h, obj, cb, ctx, ms, flags) ? h : NULL;
}
static void w32_wait_stop(w32_wait *w)
{
    if (!w) return;
    w->stop = 1;
    st_SetEvent(w->obj);                       /* wake it so it can notice */
    pthread_join(w->th, NULL);
    free(w);
}
static MS int32_t st_UnregisterWait(void *h)
{
    if (!h) { g_last_error = 6; return 0; }
    w32_worker_drop(g_w32_waits, &g_w32_nwaits, h);
    w32_wait_stop(h);
    return 1;
}
static MS int32_t st_UnregisterWaitEx(void *h, void *ev)
{ (void)ev; return st_UnregisterWait(h); }

/* ------------------------------------------------------- the thread pool ---
 *
 * The Vista thread-pool API, which msvcr120's Concurrency runtime resolves by
 * name and then insists on: CreateThreadpoolTimer answering NULL is the last
 * thing between the four 32-bit NI plug-ins and a scheduler, and it reports the
 * failure as scheduler_resource_allocation_error rather than falling back to
 * the older timer-queue calls it also knows about.
 *
 * One thread per object rather than a shared pool. That is not how Windows
 * spends its threads, but the contract callers depend on is the one kept here:
 * the callback runs on some other thread, Set arms it, Close waits for it to
 * stop, and WaitFor... blocks until any callback in flight has finished. */
typedef void (MS *w32_tp_timer_cb)(void *inst, void *ctx, void *timer);
typedef void (MS *w32_tp_wait_cb)(void *inst, void *ctx, void *wait, uint32_t result);
typedef void (MS *w32_tp_work_cb)(void *inst, void *ctx, void *work);

typedef struct {
    int              kind;             /* 0 timer, 1 wait, 2 work */
    void            *cb, *ctx;
    pthread_t        th;
    pthread_mutex_t  m;
    pthread_cond_t   c;
    int              armed, stop, started, in_callback;
    int64_t          due_ms;           /* timer: first fire, relative */
    uint32_t         period_ms;        /* timer: 0 for one-shot */
    void            *wait_obj;         /* wait: the handle */
    int64_t          wait_ms;          /* wait: timeout, -1 for infinite */
} w32_tp;

static void w32_tp_sleep_ms(int64_t ms)
{
    struct timespec t;
    if (ms <= 0) return;
    t.tv_sec = (time_t)(ms / 1000);
    t.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&t, NULL);
}
static void *w32_tp_thread(void *ud)
{
    w32_tp *t = ud;
    teb_install();
    pthread_mutex_lock(&t->m);
    for (;;) {
        while (!t->armed && !t->stop)
            pthread_cond_wait(&t->c, &t->m);
        if (t->stop || g_w32_worker_quit) break;
        if (t->kind == 0) {                       /* timer */
            int64_t due = t->due_ms;
            uint32_t period = t->period_ms;
            pthread_mutex_unlock(&t->m);
            w32_tp_sleep_ms(due);
            pthread_mutex_lock(&t->m);
            if (t->stop || !t->armed) continue;
            t->in_callback = 1;
            pthread_mutex_unlock(&t->m);
            ((w32_tp_timer_cb)t->cb)(NULL, t->ctx, t);
            pthread_mutex_lock(&t->m);
            t->in_callback = 0;
            pthread_cond_broadcast(&t->c);
            if (period) t->due_ms = period; else t->armed = 0;
        } else if (t->kind == 1) {                /* wait */
            void *obj = t->wait_obj;
            uint32_t ms = t->wait_ms < 0 ? 0xFFFFFFFFu : (uint32_t)t->wait_ms;
            uint32_t r;
            pthread_mutex_unlock(&t->m);
            r = st_WaitForSingleObject(obj, ms);
            pthread_mutex_lock(&t->m);
            if (t->stop || !t->armed) continue;
            t->armed = 0;                          /* a wait fires once */
            t->in_callback = 1;
            pthread_mutex_unlock(&t->m);
            ((w32_tp_wait_cb)t->cb)(NULL, t->ctx, t, r);
            pthread_mutex_lock(&t->m);
            t->in_callback = 0;
            pthread_cond_broadcast(&t->c);
        } else {                                   /* work */
            t->armed = 0;
            t->in_callback = 1;
            pthread_mutex_unlock(&t->m);
            ((w32_tp_work_cb)t->cb)(NULL, t->ctx, t);
            pthread_mutex_lock(&t->m);
            t->in_callback = 0;
            pthread_cond_broadcast(&t->c);
        }
    }
    pthread_mutex_unlock(&t->m);
    return NULL;
}
static void *w32_tp_new(int kind, void *cb, void *ctx)
{
    w32_tp *t;
    if (!cb) { g_last_error = 87; return NULL; }
    if (!(t = calloc(1, sizeof *t))) { g_last_error = 8; return NULL; }
    t->kind = kind; t->cb = cb; t->ctx = ctx;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    if (pthread_create(&t->th, NULL, w32_tp_thread, t) != 0) {
        pthread_mutex_destroy(&t->m);
        pthread_cond_destroy(&t->c);
        free(t);
        g_last_error = 8;
        return NULL;
    }
    t->started = 1;
    w32_worker_add(g_w32_tps, &g_w32_ntps, t);
    return t;
}
static void w32_tp_close(void *h)
{
    w32_tp *t = h;
    if (!t) return;
    w32_worker_drop(g_w32_tps, &g_w32_ntps, t);
    pthread_mutex_lock(&t->m);
    t->stop = 1;
    t->armed = 0;
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
    if (t->kind == 1 && t->wait_obj) st_SetEvent(t->wait_obj);   /* break the wait */
    if (t->started) pthread_join(t->th, NULL);
    pthread_mutex_destroy(&t->m);
    pthread_cond_destroy(&t->c);
    free(t);
}
static void w32_tp_flush(void *h, int32_t cancel)
{
    w32_tp *t = h;
    if (!t) return;
    pthread_mutex_lock(&t->m);
    if (cancel) t->armed = 0;
    while (t->in_callback) pthread_cond_wait(&t->c, &t->m);
    pthread_mutex_unlock(&t->m);
}

static MS void *st_CreateThreadpoolTimer(void *cb, void *ctx, void *env)
{ (void)env; return w32_tp_new(0, cb, ctx); }
static MS void st_SetThreadpoolTimer(void *h, const int64_t *due, uint32_t period,
                                     uint32_t window)
{
    w32_tp *t = h;
    (void)window;
    if (!t) return;
    pthread_mutex_lock(&t->m);
    if (!due) {
        t->armed = 0;                              /* NULL cancels it */
    } else {
        /* A negative FILETIME is a delay in 100 ns units; a positive one is an
         * absolute time, which for a timer that has just been set is now. */
        t->due_ms = (*due < 0) ? (-*due) / 10000 : 0;
        t->period_ms = period;
        t->armed = 1;
    }
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
}
static MS void st_WaitForThreadpoolTimerCallbacks(void *h, int32_t cancel)
{ w32_tp_flush(h, cancel); }
static MS void st_CloseThreadpoolTimer(void *h) { w32_tp_close(h); }

static MS void *st_CreateThreadpoolWait(void *cb, void *ctx, void *env)
{ (void)env; return w32_tp_new(1, cb, ctx); }
static MS void st_SetThreadpoolWait(void *h, void *obj, const int64_t *timeout)
{
    w32_tp *t = h;
    if (!t) return;
    pthread_mutex_lock(&t->m);
    if (!obj) {
        t->armed = 0;
    } else {
        t->wait_obj = obj;
        t->wait_ms = timeout ? ((*timeout < 0) ? (-*timeout) / 10000 : 0) : -1;
        t->armed = 1;
    }
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
}
static MS void st_WaitForThreadpoolWaitCallbacks(void *h, int32_t cancel)
{ w32_tp_flush(h, cancel); }
static MS void st_CloseThreadpoolWait(void *h) { w32_tp_close(h); }

static MS void *st_CreateThreadpoolWork(void *cb, void *ctx, void *env)
{ (void)env; return w32_tp_new(2, cb, ctx); }
static MS void st_SubmitThreadpoolWork(void *h)
{
    w32_tp *t = h;
    if (!t) return;
    pthread_mutex_lock(&t->m);
    t->armed = 1;
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
}
static MS void st_WaitForThreadpoolWorkCallbacks(void *h, int32_t cancel)
{ w32_tp_flush(h, cancel); }
static MS void st_CloseThreadpoolWork(void *h) { w32_tp_close(h); }

/* Stop everything this layer started that could call into the plug-in, and wait
 * for it to actually be out of that code. Called from w32_reset, which runs
 * while the image is still mapped. */
static void w32_stop_workers(void)
{
    void *waits[W32_WORKER_MAX], *tps[W32_WORKER_MAX];
    int nw, nt, i, spins;

    g_w32_worker_quit = 1;
    /* Copied out and the lists cleared under the lock; the stopping itself
     * joins threads, which must not be done holding it. */
    pthread_mutex_lock(&g_w32_worker_lock);
    nw = g_w32_nwaits; nt = g_w32_ntps;
    for (i = 0; i < nw; i++) waits[i] = g_w32_waits[i];
    for (i = 0; i < nt; i++) tps[i]  = g_w32_tps[i];
    g_w32_nwaits = g_w32_ntps = 0;
    pthread_mutex_unlock(&g_w32_worker_lock);

    for (i = 0; i < nw; i++) w32_wait_stop(waits[i]);
    for (i = 0; i < nt; i++) w32_tp_close(tps[i]);

    /* Work items are detached and cannot be joined, so wait for the count to
     * come back to zero -- briefly, because a plug-in that never returns from
     * one must not hold up the teardown for ever. */
    for (spins = 0; g_w32_work_running > 0 && spins < 500; spins++)
        usleep(1000);
    if (g_w32_work_running > 0)
        PLOG("  [win] %d work item(s) still running at teardown\n",
             g_w32_work_running);
    g_w32_worker_quit = 0;                    /* the next plug-in starts clean */
}

/* A full barrier across every thread. __sync_synchronize is the local half of
 * it; the cross-thread half is what the kernel call does on Windows, and there
 * is no portable equivalent -- but every caller uses this to publish writes it
 * has already made, which the barrier does cover. */
static MS void st_FlushProcessWriteBuffers(void) { __sync_synchronize(); }

/* The stack guarantee is a guard-page reservation for handling stack overflow;
 * with no structured overflow handling to run, reporting success is truthful
 * about what the caller can then rely on. */
static MS int32_t st_SetThreadStackGuarantee(uint32_t *bytes)
{ if (bytes) *bytes = 0; return 1; }

/* QueueUserWorkItem: run it on a thread of its own and let it go. */
typedef struct { void *fn, *ctx; } w32_workitem;
static void *w32_work_thread(void *ud)
{
    w32_workitem it = *(w32_workitem *)ud;
    free(ud);
    teb_install();
    if (it.fn && !g_w32_worker_quit) {
        __sync_fetch_and_add(&g_guest_threads, 1);
        ((uint32_t (MS *)(void *))it.fn)(it.ctx);
        __sync_fetch_and_sub(&g_guest_threads, 1);
    }
    __sync_fetch_and_sub(&g_w32_work_running, 1);
    return NULL;
}
static MS int32_t st_QueueUserWorkItem(void *fn, void *ctx, uint32_t flags)
{
    pthread_t t;
    w32_workitem *it;
    (void)flags;
    if (!(it = malloc(sizeof *it))) { g_last_error = 8; return 0; }
    it->fn = fn; it->ctx = ctx;
    __sync_fetch_and_add(&g_w32_work_running, 1);
    if (pthread_create(&t, NULL, w32_work_thread, it) != 0) {
        __sync_fetch_and_sub(&g_w32_work_running, 1);
        free(it);
        g_last_error = 8;
        return 0;
    }
    pthread_detach(t);
    return 1;
}

static MS void st_ExitThread(uint32_t code) { (void)code; pthread_exit(NULL); }
static MS int32_t st_GetExitCodeThread(void *h, uint32_t *code)
{ (void)h; if (code) *code = 0; return 1; }
static MS uint32_t st_ResumeThread(void *h) { (void)h; return 1; }
static MS int32_t st_SetThreadPriority(void *h, int32_t p) { (void)h;(void)p; return 1; }
static MS int32_t st_GetThreadPriority(void *h) { (void)h; return 0; }
static MS uint32_t st_GetTempPathA(uint32_t n, char *buf)
{ return (uint32_t)snprintf(buf, n, "/tmp/"); }
/* Same two-call contract as the wide form above. */
static MS uint32_t st_GetFullPathNameA(const char *n, uint32_t len, char *buf, char **part)
{
    char full[1200], cwd[1024], fixed[1024];
    uint32_t need;

    if (part) *part = NULL;
    if (!n) { g_last_error = 87; return 0; }
    path_fix(n, fixed, sizeof fixed);
    if (fixed[0] == '/') snprintf(full, sizeof full, "%s", fixed);
    else {
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
        snprintf(full, sizeof full, "%s/%s", cwd, fixed);
    }
    need = (uint32_t)strlen(full);
    if (!buf || len <= need) return need + 1;
    memcpy(buf, full, need + 1);
    if (part) {
        char *slash = strrchr(buf, '/');
        *part = (slash && slash[1]) ? slash + 1 : NULL;
    }
    return need;
}
static MS uint32_t st_GetCurrentDirectoryA(uint32_t n, char *buf)
{ return getcwd(buf, n) ? (uint32_t)strlen(buf) : 0; }
static MS int32_t st_AreFileApisANSI(void) { return 1; }
static MS int32_t st_WriteConsoleW(void *h, const void *b, uint32_t n, uint32_t *w, void *r)
{ (void)h;(void)b;(void)r; if (w) *w = n; return 1; }
static MS int32_t st_GetConsoleMode(void *h, uint32_t *m) { (void)h; if (m) *m = 0; return 0; }
static MS uint32_t st_GetConsoleCP(void) { return 437; }
static MS uint32_t st_GetConsoleOutputCP(void) { return 437; }

/* ------------------------------------------------------------- resources */

/* Full Bucket plugins keep their wavetables and preset banks in .rsrc, so
 * these have to genuinely work -- they walk the three-level resource tree in
 * the image we already mapped. */
#pragma pack(push, 1)
typedef struct {
    uint32_t Characteristics, TimeDateStamp;
    uint16_t MajorVersion, MinorVersion, NumberOfNamedEntries, NumberOfIdEntries;
} RES_DIR;
typedef struct { uint32_t Name, OffsetToData; } RES_ENT;
typedef struct { uint32_t OffsetToData, Size, CodePage, Reserved; } RES_DATA;
#pragma pack(pop)

/* An id or a string, rendered for a log line. Printing just "#ord" hides the one
 * fact that identifies which resource was wanted. */
static const char *res_label(const void *v, char *buf, size_t n)
{
    if ((uintptr_t)v < 0x10000) { snprintf(buf, n, "#%u", (unsigned)(uintptr_t)v); return buf; }
    return (const char *)v;
}

static RES_ENT *res_find(RES_DIR *d, const void *id, uint8_t *rbase)
{
    RES_ENT *e = (RES_ENT *)((uint8_t *)d + sizeof *d);
    int n = d->NumberOfNamedEntries + d->NumberOfIdEntries, i;
    uintptr_t want = (uintptr_t)id;

    for (i = 0; i < n; i++) {
        if (want < 0x10000) {                       /* integer id */
            if (!(e[i].Name & 0x80000000u) && e[i].Name == (uint32_t)want) return &e[i];
        } else if (e[i].Name & 0x80000000u) {       /* string name */
            uint16_t *s = (uint16_t *)(rbase + (e[i].Name & 0x7FFFFFFF));
            int len = s[0], j;
            const char *w = id;
            for (j = 0; j < len && w[j]; j++)
                if (towupper(s[1 + j]) != towupper((unsigned char)w[j])) break;
            if (j == len && !w[j]) return &e[i];
        }
    }
    return NULL;
}

/* Both name and type may be an integer id (a pointer value below 0x10000) or a
 * string. The wide entry point converts to narrow first: resource directory
 * names are UTF-16 on disk, but comparing them needs one consistent encoding,
 * and forwarding wide strings into the narrow comparison silently failed every
 * lookup -- which is what made the plugin's own resource callback crash. */
/* What a directory level actually holds. A lookup that fails is otherwise
 * indistinguishable from a resource that is absent by design, and the two want
 * opposite responses. */
static void res_dump_level(const char *what, RES_DIR *d, uint8_t *rbase)
{
    RES_ENT *e = (RES_ENT *)((uint8_t *)d + sizeof *d);
    int n = d->NumberOfNamedEntries + d->NumberOfIdEntries, i;
    char line[512];
    size_t off = 0;
    for (i = 0; i < n && off + 24 < sizeof line; i++) {
        if (e[i].Name & 0x80000000u) {
            uint16_t *w = (uint16_t *)(rbase + (e[i].Name & 0x7FFFFFFF));
            int len = w[0], j;
            if (len > 16) len = 16;
            line[off++] = ' ';
            for (j = 0; j < len && off + 1 < sizeof line; j++)
                line[off++] = w[1 + j] < 0x100 ? (char)w[1 + j] : '?';
        } else {
            off += (size_t)snprintf(line + off, sizeof line - off, " #%u", e[i].Name);
        }
    }
    line[off] = 0;
    PLOG("  [res] %s holds%s\n", what, off ? line : " nothing");
}

static void *res_lookup(const void *type, const void *name, uint8_t *rbase)
{
    RES_DIR *lvl2, *lvl3;
    RES_ENT *e;

    char lb[64];
    if (!rbase) { PLOG("  [res] no resource directory is mapped\n"); return NULL; }
    if (!(e = res_find((RES_DIR *)rbase, type, rbase))) {
        res_dump_level("type level", (RES_DIR *)rbase, rbase);
        return NULL;
    }
    if (!(e->OffsetToData & 0x80000000u)) return NULL;
    lvl2 = (RES_DIR *)(rbase + (e->OffsetToData & 0x7FFFFFFF));
    if (!(e = res_find(lvl2, name, rbase))) {
        snprintf(lb, sizeof lb, "type %s", res_label(type, lb + 32, 32));
        res_dump_level(lb, lvl2, rbase);
        return NULL;
    }
    if (!(e->OffsetToData & 0x80000000u)) return NULL;
    lvl3 = (RES_DIR *)(rbase + (e->OffsetToData & 0x7FFFFFFF));
    e = (RES_ENT *)((uint8_t *)lvl3 + sizeof *lvl3);   /* first language */
    return rbase + e->OffsetToData;
}

static MS void *st_FindResourceA(void *mod, const char *name, const char *type)
{
    void *r;
    (void)mod;
    char nb[32], tb[32];
    r = res_lookup(type, name, image_rsrc(mod));
    PLOG("  [res] FindResourceA(name=%s type=%s) -> %p\n",
         res_label(name, nb, sizeof nb), res_label(type, tb, sizeof tb), r);
    return r;
}

static MS void *st_FindResourceW(void *mod, const uint16_t *name, const uint16_t *type)
{
    char nbuf[256], tbuf[256];
    const void *n, *t;
    (void)mod;
    if ((uintptr_t)name < 0x10000) n = (const void *)name;
    else { w2c(name, nbuf, sizeof nbuf); n = nbuf; }
    if ((uintptr_t)type < 0x10000) t = (const void *)type;
    else { w2c(type, tbuf, sizeof tbuf); t = tbuf; }
    {
        void *r = res_lookup(t, n, image_rsrc(mod));
        char nb2[32], tb2[32];
        PLOG("  [res] FindResourceW(name=%s type=%s) -> %p\n",
             res_label(n, nb2, sizeof nb2), res_label(t, tb2, sizeof tb2), r);
        return r;
    }
}

static MS uint32_t st_SizeofResource(void *mod, void *rsrc)
{ (void)mod; return rsrc ? ((RES_DATA *)rsrc)->Size : 0; }
static MS void *st_LoadResource(void *mod, void *rsrc)
{ (void)mod; return rsrc ? image_base_for_rsrc(rsrc)
                             + ((RES_DATA *)rsrc)->OffsetToData : NULL; }
static MS void *st_LockResource(void *h) { return h; }
static MS int32_t st_FreeResource(void *h) { (void)h; return 1; }

/* iPlug2 finds its embedded images and fonts by enumerating .rsrc rather than
 * asking for them by name, so this has to genuinely walk the resource tree.
 * Returning 0 here left the plugin with no assets and it faulted. */
/* The trailing LONG_PTR is pointer-sized, so it is spelled intptr_t and not
 * int64_t -- the same rule winstubs32.h's header states for every Windows type
 * that changes width. It matters more here than in a stub signature, because
 * this is a call *into* guest code: at i386 an 8-byte push leaves four bytes
 * behind after the plugin's own stdcall callback has popped its four
 * arguments, and iPlug2 enumerates every resource it owns through this. The
 * entry points below already take intptr_t; this is where it was widened. */
typedef MS int32_t (*enumresnameA)(void *, const char *, char *, intptr_t);
typedef MS int32_t (*enumresnameW)(void *, const uint16_t *, uint16_t *, intptr_t);

static RES_DIR *res_type_dir(const void *type, uint8_t *rbase)
{
    RES_ENT *e;
    if (!rbase) return NULL;
    if (!(e = res_find((RES_DIR *)rbase, type, rbase))) return NULL;
    if (!(e->OffsetToData & 0x80000000u)) return NULL;
    return (RES_DIR *)(rbase + (e->OffsetToData & 0x7FFFFFFF));
}

/* Hand each entry name to the callback, as an integer id or a string, matching
 * how the resource directory stores it. */
/* `key` looks the type up in .rsrc; `cbtype` is what the callback expects to
 * receive, which for a wide callback must stay UTF-16 rather than the narrow
 * copy used for the lookup. */
static int32_t res_enum(void *mod, const void *key, const void *cbtype,
                        void *cb, intptr_t param, int wide)
{
    RES_DIR *d = res_type_dir(key, image_rsrc(mod));
    RES_ENT *e;
    int n, i;

    if (!d) {
        PLOG("  [win] EnumResourceNames: type not found\n");
        g_last_error = 1813;   /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        return 0;
    }
    e = (RES_ENT *)((uint8_t *)d + sizeof *d);
    n = d->NumberOfNamedEntries + d->NumberOfIdEntries;
    for (i = 0; i < n; i++) {
        int keep;
        if (e[i].Name & 0x80000000u) {           /* string name */
            const uint16_t *s = (const uint16_t *)(g_rsrc + (e[i].Name & 0x7FFFFFFF));
            int len = s[0], j;
            uint16_t wbuf[256];
            char abuf[256];
            if (len > 255) len = 255;
            for (j = 0; j < len; j++) wbuf[j] = s[1 + j];
            wbuf[len] = 0;
            if (wide) keep = ((enumresnameW)cb)(mod, cbtype, wbuf, param);
            else { w2c(wbuf, abuf, sizeof abuf);
                   keep = ((enumresnameA)cb)(mod, cbtype, abuf, param); }
        } else {                                  /* integer id */
            uintptr_t id = e[i].Name & 0xFFFF;
            if (wide) keep = ((enumresnameW)cb)(mod, cbtype, (uint16_t *)id, param);
            else      keep = ((enumresnameA)cb)(mod, cbtype, (char *)id, param);
        }
        if (!keep) break;                         /* callback asked us to stop */
    }
    return 1;
}

static MS int32_t st_EnumResourceNamesA(void *mod, const char *type, void *cb, intptr_t p)
{
    PLOG("  [win] EnumResourceNamesA(type=%s)\n",
         (uintptr_t)type < 0x10000 ? "<id>" : type);
    return res_enum(mod ? mod : g_image_base, type, type, cb, p, 0);
}
static MS int32_t st_EnumResourceNamesW(void *mod, const uint16_t *type, void *cb, intptr_t p)
{
    char t[128];
    /* Types are usually integer ids, which arrive in the pointer itself. */
    if ((uintptr_t)type < 0x10000) {
        PLOG("  [win] EnumResourceNamesW(id=%lu)\n", (unsigned long)(uintptr_t)type);
        return res_enum(mod ? mod : g_image_base, (const void *)type, type, cb, p, 1);
    }
    w2c(type, t, sizeof t);
    PLOG("  [win] EnumResourceNamesW(type=%s)\n", t);
    return res_enum(mod ? mod : g_image_base, t, type, cb, p, 1);
}

/* Version gate: report every requested condition as satisfied. A plugin that
 * thinks it is on an ancient Windows takes odd fallback paths. */
static MS uint64_t st_VerSetConditionMask(uint64_t mask, uint32_t type, uint8_t cond)
{
    int i;
    for (i = 0; i < 8; i++)
        if (type & (1u << i)) mask |= ((uint64_t)(cond & 7)) << (i * 3);
    return mask;
}
/* VerifyVersionInfo compares the caller's version fields against this machine's,
 * each with its own comparison operator taken from the condition mask.
 *
 * Answering "yes" unconditionally is not a harmless simplification. The usual way
 * to discover the version is to raise a candidate while VerifyVersionInfo still
 * reports "the OS is at least this", and a function that always agrees turns that
 * into an endless loop -- which is what Absynth does here, spinning with
 * VerSetConditionMask as its hottest frame and never reaching effOpen.
 *
 * The values reported must agree with st_GetVersionExW above; two different
 * answers about the same machine is its own class of bug. */
#define W_VER_MINORVERSION      0x01
#define W_VER_MAJORVERSION      0x02
#define W_VER_BUILDNUMBER       0x04
#define W_VER_PLATFORMID        0x08
#define W_VER_SERVICEPACKMINOR  0x10
#define W_VER_SERVICEPACKMAJOR  0x20
#define W_VER_SUITENAME         0x40
#define W_VER_PRODUCT_TYPE      0x80

/* The condition for type bit i sits in bits (3i, 3i+2) of the mask, which is how
 * st_VerSetConditionMask writes it. */
enum { W_VER_EQUAL = 1, W_VER_GREATER, W_VER_GREATER_EQUAL, W_VER_LESS,
       W_VER_LESS_EQUAL };

static int w32_ver_cmp_ok(uint32_t have, uint32_t want, int cond)
{
    switch (cond) {
    case W_VER_EQUAL:         return have == want;
    case W_VER_GREATER:       return have >  want;
    case W_VER_GREATER_EQUAL: return have >= want;
    case W_VER_LESS:          return have <  want;
    case W_VER_LESS_EQUAL:    return have <= want;
    default:                  return 1;      /* no condition asked for */
    }
}

static MS int32_t st_VerifyVersionInfoW(void *info, uint32_t type, uint64_t mask)
{
    /* OSVERSIONINFOEXW, as st_GetVersionExW lays it out. */
    const uint32_t *v = info;
    const uint8_t *ex;
    int i;
    struct { uint32_t bit; uint32_t have, want; } t[8];
    int n = 0;

    if (!v) { g_last_error = 87 /* ERROR_INVALID_PARAMETER */; return 0; }
    ex = (const uint8_t *)&v[5] + 256;

    if (type & W_VER_MAJORVERSION) { t[n].bit = W_VER_MAJORVERSION; t[n].have = 10;    t[n].want = v[1]; n++; }
    if (type & W_VER_MINORVERSION) { t[n].bit = W_VER_MINORVERSION; t[n].have = 0;     t[n].want = v[2]; n++; }
    if (type & W_VER_BUILDNUMBER)  { t[n].bit = W_VER_BUILDNUMBER;  t[n].have = 19045; t[n].want = v[3]; n++; }
    if (type & W_VER_PLATFORMID)   { t[n].bit = W_VER_PLATFORMID;   t[n].have = 2;     t[n].want = v[4]; n++; }
    if (v[0] >= 5 * 4 + 256 + 8) {
        if (type & W_VER_SERVICEPACKMAJOR)
            { t[n].bit = W_VER_SERVICEPACKMAJOR; t[n].have = 0; t[n].want = *(const uint16_t *)(ex + 0); n++; }
        if (type & W_VER_SERVICEPACKMINOR)
            { t[n].bit = W_VER_SERVICEPACKMINOR; t[n].have = 0; t[n].want = *(const uint16_t *)(ex + 2); n++; }
        if (type & W_VER_PRODUCT_TYPE)
            { t[n].bit = W_VER_PRODUCT_TYPE; t[n].have = 1; t[n].want = ex[6]; n++; }
    }
    /* VER_SUITENAME is a bit test rather than a comparison, and nothing here
     * belongs to any suite, so a request for one cannot be satisfied. */
    if (type & W_VER_SUITENAME) {
        g_last_error = 1151 /* ERROR_OLD_WIN_VERSION */;
        return 0;
    }

    for (i = 0; i < n; i++) {
        int shift = 0, b = (int)t[i].bit;
        while (b > 1) { b >>= 1; shift += 3; }
        if (!w32_ver_cmp_ok(t[i].have, t[i].want,
                            (int)((mask >> shift) & 7))) {
            g_last_error = 1151;
            return 0;
        }
    }
    return 1;
}

static MS int32_t st_VerifyVersionInfoA(void *info, uint32_t type, uint64_t mask)
{ return st_VerifyVersionInfoW(info, type, mask); }

static MS int32_t st_PathFileExistsA(const char *p)
{ struct stat st; char b[1024];
  return p && stat(path_fix(p, b, sizeof b), &st) == 0; }
static MS int32_t st_PathFileExistsW(const uint16_t *p)
{ char b[1024]; return st_PathFileExistsA(w2c_path(p, b, sizeof b)); }

/* ---------------------------------------------------------------- ole32 */

static MS void *st_CoTaskMemAlloc(size_t n) { return w32_alloc(n, 0); }
static MS void st_CoTaskMemFree(void *p) { w32_free(p); }
static MS int32_t st_OleInitialize(void *r) { (void)r; return 0; }
static MS void st_OleUninitialize(void) { }
static MS int32_t st_CoInitialize(void *r) { (void)r; return 0; }
static MS void st_CoUninitialize(void) { }

/* ------------------------------------------ Win32 windowing for editors --- */

#ifndef PELOAD_NO_GUI_LAYER
#include "win32host.h"
#include "win32gui.h"
#include "dwrite_shim.h"
#include "gdiplus_shim.h"
#include "d3d_shim.h"
#include "msvcp_shim.h"

/* The last few stubs this corpus actually reaches.
 *
 * lstrcpyn is the one that matters: it is a string copy, and a stub that copies
 * nothing leaves the caller with whatever its buffer already held. Nobody
 * checks a string copy. A plug-in assembling the name of a skin image it is
 * about to load gets an empty name and loads nothing, and what you see is a
 * missing picture rather than a failed call. */
static MS char *st_lstrcpynA(char *dst, const char *src, int32_t n)
{
    int32_t i;
    if (!dst || n <= 0) return dst;
    if (!src) { dst[0] = 0; return dst; }
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return dst;
}
static MS uint16_t *st_lstrcpynW(uint16_t *dst, const uint16_t *src, int32_t n)
{
    int32_t i;
    if (!dst || n <= 0) return dst;
    if (!src) { dst[0] = 0; return dst; }
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return dst;
}
/* Milliseconds since the host started, which is what timeGetTime means. A stub
 * answering 0 does not read as an error -- there is no error value -- it reads
 * as "no time has passed", every time it is asked. Anything driving an
 * animation, a debounce or a first-frame test from it never advances. */
static MS uint32_t st_timeGetTime(void)
{
    static double t0;
    double now = w32_now_ms();
    if (t0 == 0.0) t0 = now;
    return (uint32_t)(now - t0);
}
static MS uint32_t st_timeBeginPeriod(uint32_t ms) { (void)ms; return 0; }
static MS uint32_t st_timeEndPeriod(uint32_t ms)   { (void)ms; return 0; }

/* Whether a class name is registered, and what with. A caller uses this to
 * decide whether to register its own -- MFC asks before every window it
 * creates -- and answering "no" for a class we do have makes it register a
 * second one over the top. */
static MS int32_t st_GetClassInfoA(void *inst, const char *name, void *out)
{
    (void)inst;
    if (!name || !out) return 0;
    {
        void *proc = w32_class_proc(name);
        if (!proc) return 0;
        /* WNDCLASSA: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance,
         * hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName. */
        memset(out, 0, 40);
        *(void **)((char *)out + 4) = proc;
        *(const char **)((char *)out + 36) = name;
        return 1;
    }
}

/* The path questions a plug-in asks before it decides where its own data lives.
 *
 * All four were stubs answering zero, and a caller reads that as "there is no
 * such volume", "this is not a root", "there is no directory" -- so it concludes
 * it has nowhere to keep anything and stops looking. */
static MS int32_t st_GetVolumeInformationA(const char *root, char *name, uint32_t namelen,
                                           uint32_t *serial, uint32_t *maxcomp,
                                           uint32_t *flags, char *fsname, uint32_t fslen)
{
    (void)root;
    if (name && namelen) snprintf(name, namelen, "%s", "peload");
    if (serial)  *serial = 0x50454C44u;
    if (maxcomp) *maxcomp = 255;
    if (flags)   *flags = 0;
    if (fsname && fslen) snprintf(fsname, fslen, "%s", "NTFS");
    return 1;
}
static MS int32_t st_CreateDirectoryA(const char *path, void *sa)
{
    char host[1024];
    (void)sa;
    if (!path) return 0;
    snprintf(host, sizeof host, "%s", path);
    path_norm_n(host, sizeof host);
    if (mkdir(host, 0777) == 0) return 1;
    g_last_error = (errno == EEXIST) ? 183 : 3;   /* ALREADY_EXISTS / PATH_NOT_FOUND */
    return 0;
}
/* Nothing this host serves is a UNC path; a drive letter is its own root. */
static MS int32_t st_PathIsUNCA(const char *p)
{ return p && p[0] == '\\' && p[1] == '\\'; }
static MS int32_t st_PathStripToRootA(char *p)
{
    if (!p) return 0;
    if (p[0] && p[1] == ':') { p[2] = '\\'; p[3] = 0; return 1; }
    if (p[0] == '\\' && p[1] == '\\') {            /* \\server\share */
        char *q = strchr(p + 2, '\\');
        if (q) q = strchr(q + 1, '\\');
        if (q) *q = 0;
        return 1;
    }
    return 0;
}

/* A halftone palette is meaningless at 32 bits per pixel, and a caller that
 * gets NULL for one concludes the display cannot be drawn on. */
static MS void *st_CreateHalftonePalette(void *hdc)
{ (void)hdc; return w32_h(W32_OBJ_BASE, w32_obj_new(OBJ_BRUSH, 0, 0, 0)); }
static MS void *st_SelectPalette(void *hdc, void *pal, int32_t force)
{ (void)hdc; (void)force; return pal; }
static MS uint32_t st_RealizePalette(void *hdc) { (void)hdc; return 0; }

#endif

/* ------------------------------------------------------------- registry */

static MS int32_t st_RegOpenKeyExA(void *k, const char *s, uint32_t o, uint32_t a, void **out)
{ (void)k;(void)s;(void)o;(void)a; if (out) *out = NULL; return 2 /* ERROR_FILE_NOT_FOUND */; }
static MS int32_t st_RegQueryValueExA(void *k, const char *s, uint32_t *r, uint32_t *t,
                                     void *d, uint32_t *n)
{ (void)k;(void)s;(void)r;(void)t;(void)d;(void)n; return 2; }
static MS int32_t st_RegCloseKey(void *k) { (void)k; return 0; }

static MS void st_GetStartupInfoA(void *si) { st_GetStartupInfoW(si); }
static MS uint32_t st_GetSystemDirectoryA(char *buf, uint32_t n)
{ return (uint32_t)snprintf(buf, n, "C:\\Windows\\System32"); }
static MS uint32_t st_GetWindowsDirectoryA(char *buf, uint32_t n)
{ return (uint32_t)snprintf(buf, n, "C:\\Windows"); }
/* GetPrivateProfileString and GetPrivateProfileInt were placeholders here that
 * read no file and handed back the caller's own default -- which was the right
 * shape for a stub, and is why plugins kept initialising. The real ones, with
 * the write side beside them so settings survive the session, are further down
 * with the rest of the profile API. */
/* GetFullPathName has a two-call contract, and getting the first call wrong is
 * silent: with no buffer, or one too small, it returns the size *required*
 * including the terminator, and writes nothing; with room it fills the buffer and
 * returns the length *excluding* the terminator. Zero means failure.
 *
 * The old version returned zero for the size query, because its copy loop tested
 * `i + 1 < len` against a length of nought and its terminator write was guarded
 * on the same. SQLite asks exactly that way -- winFullPathname calls it with a
 * null buffer first -- read the zero as failure, and reported
 * "unable to open database file" for a file it never tried to open. That is what
 * left Absynth without its content database, and its editor dereferencing the
 * database object it expects always to be there.
 *
 * A relative path is also resolved against the working directory here, which is
 * the other half of what the name promises. */
static MS uint32_t st_GetFullPathNameW(const uint16_t *nm, uint32_t len,
                                       uint16_t *buf, uint16_t **part)
{
    char n[1024], full[1200], cwd[1024];
    uint32_t need, i;

    if (part) *part = NULL;
    if (!nm) { g_last_error = 87; return 0; }
    w2c_path(nm, n, sizeof n);
    if (n[0] == '/') snprintf(full, sizeof full, "%s", n);
    else {
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
        snprintf(full, sizeof full, "%s/%s", cwd, n);
    }
    need = (uint32_t)strlen(full);
    if (!buf || len <= need) return need + 1;      /* room needed, with the NUL */
    for (i = 0; i <= need; i++) buf[i] = (uint16_t)(unsigned char)full[i];
    if (part) {
        uint16_t *last = NULL;
        for (i = 0; i < need; i++)
            if (buf[i] == '/' || buf[i] == '\\') last = buf + i + 1;
        *part = (last && *last) ? last : NULL;
    }
    return need;
}

/* The "Ex" locale calls take a locale *name* rather than an LCID. This host has
 * one locale, so the name is not consulted -- but answering zero meant the CRT
 * believed the call had failed, which is a different thing from "the C locale
 * says the same". */
static MS int32_t st_LCMapStringEx(const uint16_t *loc, uint32_t f,
                                   const uint16_t *in, int32_t inlen,
                                   uint16_t *out, int32_t outlen,
                                   void *ver, void *reserved, intptr_t sort)
{
    (void)loc; (void)ver; (void)reserved; (void)sort;
    return st_LCMapStringW(0x0409, f, in, inlen, out, outlen);
}
static MS int32_t st_CompareStringEx(const uint16_t *loc, uint32_t f,
                                     const uint16_t *a, int32_t na,
                                     const uint16_t *b, int32_t nb,
                                     void *ver, void *reserved, intptr_t sort)
{
    (void)loc; (void)ver; (void)reserved; (void)sort;
    return st_CompareStringW(0x0409, f, a, na, b, nb);
}
static MS int32_t st_GetLocaleInfoEx(const uint16_t *loc, uint32_t t,
                                     uint16_t *out, int32_t n)
{ (void)loc; return st_GetLocaleInfoW(0x0409, t, out, n); }

static MS uint32_t st_GetSystemDirectoryW(uint16_t *buf, uint32_t n)
{
    char a[512];
    uint32_t need = st_GetSystemDirectoryA(a, sizeof a);
    uint32_t i;
    if (!need) return 0;
    if (!buf || n <= need) return need + 1;
    for (i = 0; i < need; i++) buf[i] = (uint16_t)(unsigned char)a[i];
    buf[need] = 0;
    return need;
}

/* Dates and times, formatted the way the C locale would. A plug-in stamping a
 * preset with the time got an empty string and wrote a file with no date in it. */
static MS int32_t st_GetDateFormatA(uint32_t lcid, uint32_t flags, const void *st,
                                    const char *fmt, char *out, int32_t n)
{
    time_t now = time(NULL);
    struct tm tmv;
    char b[128];
    (void)lcid; (void)st; (void)fmt;
    localtime_r(&now, &tmv);
    strftime(b, sizeof b, (flags & 0x1) ? "%d/%m/%Y" : "%d %B %Y", &tmv);
    if (!out || n <= (int32_t)strlen(b)) return (int32_t)strlen(b) + 1;
    memcpy(out, b, strlen(b) + 1);
    return (int32_t)strlen(b) + 1;
}
static MS int32_t st_GetDateFormatW(uint32_t lcid, uint32_t flags, const void *st,
                                    const uint16_t *fmt, uint16_t *out, int32_t n)
{
    char b[128];
    int32_t need, i;
    (void)fmt;
    need = st_GetDateFormatA(lcid, flags, st, NULL, b, (int32_t)sizeof b);
    if (!out || n < need) return need;
    for (i = 0; b[i]; i++) out[i] = (uint16_t)(unsigned char)b[i];
    out[i] = 0;
    return need;
}
static MS int32_t st_GetTimeFormatA(uint32_t lcid, uint32_t flags, const void *st,
                                    const char *fmt, char *out, int32_t n)
{
    time_t now = time(NULL);
    struct tm tmv;
    char b[128];
    (void)lcid; (void)st; (void)fmt; (void)flags;
    localtime_r(&now, &tmv);
    strftime(b, sizeof b, "%H:%M:%S", &tmv);
    if (!out || n <= (int32_t)strlen(b)) return (int32_t)strlen(b) + 1;
    memcpy(out, b, strlen(b) + 1);
    return (int32_t)strlen(b) + 1;
}
static MS int32_t st_GetTimeFormatW(uint32_t lcid, uint32_t flags, const void *st,
                                    const uint16_t *fmt, uint16_t *out, int32_t n)
{
    char b[128];
    int32_t need, i;
    (void)fmt;
    need = st_GetTimeFormatA(lcid, flags, st, NULL, b, (int32_t)sizeof b);
    if (!out || n < need) return need;
    for (i = 0; b[i]; i++) out[i] = (uint16_t)(unsigned char)b[i];
    out[i] = 0;
    return need;
}

/* Nothing is connected to a pipe here, and "no data waiting" is the truthful
 * answer -- distinct from the stub's "the call failed", which a reader loop
 * treats as a broken pipe. */
static MS int32_t st_PeekNamedPipe(void *h, void *buf, uint32_t n, uint32_t *read_,
                                   uint32_t *avail, uint32_t *left)
{
    (void)h; (void)buf; (void)n;
    if (read_) *read_ = 0;
    if (avail) *avail = 0;
    if (left)  *left  = 0;
    return 1;
}
static MS uint32_t st_GetCurrentDirectoryW(uint32_t n, uint16_t *buf)
{ char c[512]; uint32_t i = 0; if (!getcwd(c, sizeof c)) return 0;
  for (; c[i] && i + 1 < n; i++) buf[i] = (uint8_t)c[i]; if (n) buf[i] = 0; return i; }
static MS int32_t st_CreateDirectoryW(const uint16_t *p, void *sa)
{
    char b[1024];
    (void)sa;
    w2c_path(p, b, sizeof b);
    if (mkdir(b, 0755) == 0 || errno == EEXIST) return 1;
    /* Windows' CreateDirectory does not create intermediates either, but a plugin
     * walking its own settings tree expects the parents an installer would have
     * left. Only inside this host's tree, and only after the plain attempt. */
    if (errno == ENOENT) {
        make_parents_in_root(b);
        return mkdir(b, 0755) == 0 || errno == EEXIST;
    }
    return 0;
}
static MS int32_t st_SHGetFolderPathA(void *hwnd, int32_t csidl, void *tok, uint32_t f, char *out)
{ (void)hwnd;(void)csidl;(void)tok;(void)f; if (out) snprintf(out, 260, "/tmp"); return 0; }

static MS uint32_t st_SetHandleCount(uint32_t n) { return n; }
/* LoadBitmap, for real.
 *
 * Returning NULL here is what cost TAL's U-NO-62 its editor: it loads its
 * background this way, stored the NULL, and the next write through it was a
 * store to address zero. A plugin that cannot get its artwork also cannot size
 * its window, which is why the same plugin reported an empty editor rect.
 *
 * An RT_BITMAP resource is a *packed* DIB -- BITMAPINFOHEADER, then the palette,
 * then bottom-up rows padded to four bytes. There is no BITMAPFILEHEADER, which
 * is the usual thing to get wrong. */
void *w32_bitmap_from_dib(const uint8_t *dib, uint32_t len);

static void *load_bitmap_res(const void *name)
{
    void *rsrc = res_lookup((const void *)2 /* RT_BITMAP */, name, g_rsrc);
    const uint8_t *bits;
    uint32_t len;

    if (!rsrc) return NULL;
    len = ((RES_DATA *)rsrc)->Size;
    bits = image_base_for_rsrc(rsrc) + ((RES_DATA *)rsrc)->OffsetToData;
    return w32_bitmap_from_dib(bits, len);
}

static MS void *st_LoadBitmapA(void *inst, const char *name)
{
    void *b;
    (void)inst;
    b = load_bitmap_res(name);
    PLOG("  [res] LoadBitmapA(%s) -> %p\n",
         (uintptr_t)name < 0x10000 ? "#ord" : name, b);
    return b;
}
static MS void *st_LoadBitmapW(void *inst, const void *name)
{ (void)inst; return load_bitmap_res(name); }

/* LoadString, for real -- same reasoning as LoadBitmap above: returning 0
 * unread is a plugin's cue that its own config/resource load failed, and
 * that is a way for VSTPluginMain to come back with no AEffect at all
 * (syxg50 calls this right after finding its #110 dialog resource, and gets
 * no further). An RT_STRING resource packs 16 consecutive string-table
 * entries per block -- block (id>>4)+1, indexed within it by id&15 -- each a
 * uint16 length followed by that many UTF-16 code units with no terminator.
 *
 * Every length in that walk is a number out of the plugin's own file, so the
 * walk is bounded by the resource's declared Size rather than trusted to stay
 * inside it. These are third-party binaries being read: a block truncated at
 * the wrong point would otherwise send the walk off the end of the mapped
 * image, and a length word of 0xffff would do it on the first step. It is the
 * same reason load_bitmap_res hands w32_bitmap_from_dib a length instead of
 * letting it find the end itself. */
static const uint16_t *res_string_entry(uint32_t id, int *len_out)
{
    void           *rsrc;
    const uint16_t *w, *end;
    int             i, idx = (int)(id & 15);

    *len_out = 0;
    if (!(rsrc = res_lookup((const void *)6 /* RT_STRING */,
                            (const void *)(uintptr_t)((id >> 4) + 1), g_rsrc)))
        return NULL;

    w   = (const uint16_t *)(image_base_for_rsrc(rsrc) + ((RES_DATA *)rsrc)->OffsetToData);
    end = w + ((RES_DATA *)rsrc)->Size / sizeof *w;

    /* Each step needs the length word itself and the characters it claims. */
    for (i = 0; i <= idx; i++) {
        if (w >= end || (size_t)(end - w) < (size_t)w[0] + 1) return NULL;
        if (i == idx) { *len_out = w[0]; return w + 1; }
        w += 1 + w[0];
    }
    return NULL;
}

static MS int32_t st_LoadStringA(void *inst, uint32_t id, char *buf, int32_t max)
{
    const uint16_t *s;
    int             len, i;

    (void)inst;
    if (!buf || max <= 0) return 0;
    buf[0] = 0;
    if (!(s = res_string_entry(id, &len))) return 0;
    if (len > max - 1) len = max - 1;
    for (i = 0; i < len; i++) buf[i] = s[i] < 0x100 ? (char)s[i] : '?';
    buf[len] = 0;
    return len;
}

/* The wide form, which plugins reach for just as often -- and which a plugin
 * is stranded by in exactly the same way when it comes back 0. Every other
 * resource call here is paired A and W for that reason. */
static MS int32_t st_LoadStringW(void *inst, uint32_t id, uint16_t *buf, int32_t max)
{
    const uint16_t *s;
    int             len, i;

    (void)inst;
    if (!buf || max <= 0) return 0;
    buf[0] = 0;
    if (!(s = res_string_entry(id, &len))) return 0;
    if (len > max - 1) len = max - 1;
    for (i = 0; i < len; i++) buf[i] = s[i];
    buf[len] = 0;
    return len;
}

/* ---------------------------------------------------- CRT static init ---
 *
 * MSVC emits every C++ global's constructor as a function pointer in the
 * .CRT$XC* / .CRT$XI* sections and has DllMain walk them through _initterm.
 * Stubbing those to a no-op therefore leaves every global in the image
 * unconstructed, and the first one used reads whatever the section happened
 * to contain -- which is how a plugin faults on a return address of
 * 0x2020202020202020 with nothing in its own code at fault.
 *
 * Plugins that link the CRT statically run this loop inside their own image
 * and never import it, which is why the Full Bucket set never needed it. */

typedef void (MSCRT *crt_pvfv)(void);
typedef int  (MSCRT *crt_pifv)(void);

static MSCRT void st__initterm(crt_pvfv *first, crt_pvfv *last)
{
    int n = 0;
    for (; first && first < last; ++first)
        if (*first) { (*first)(); n++; }
    PLOG("  [crt] _initterm: ran %d initialiser(s)\n", n);
}

/* The _e form is the same walk over functions that can fail; a non-zero
 * return aborts the rest and is reported to the caller as an errno. */
static MSCRT int st__initterm_e(crt_pifv *first, crt_pifv *last)
{
    int n = 0;
    for (; first && first < last; ++first)
        if (*first) {
            int r = (*first)();
            n++;
            if (r) {
                /* Report the failing entry where objdump can be pointed at it
                 * directly; a failure here aborts the whole table, so which
                 * one it was is the only useful fact. */
                PLOG("  [crt] _initterm_e: initialiser %d at image+0x%zx returned %d\n",
                     n, (size_t)((uint8_t *)*first - g_image_base), r);
                return r;
            }
        }
    PLOG("  [crt] _initterm_e: ran %d initialiser(s)\n", n);
    return 0;
}

/* --------------------------------------------------------- volume info ---
 *
 * Absynth reads the volume serial number and formats it into a string as part
 * of a machine fingerprint -- the same routine goes on to reach IPHLPAPI for
 * a MAC address. A licence is issued against whatever this produces, so the
 * value has to be real and stable across runs: returning zero would give
 * every machine the same identity, which is both wrong and the sort of wrong
 * that only shows up much later.
 *
 * f_fsid is stable for the life of the filesystem and differs between
 * machines, which is the property the caller actually wants. */

static void c2w_(const char *s, uint16_t *out, size_t n)
{
    size_t i = 0;
    if (!out || !n) return;
    for (; s[i] && i + 1 < n; i++) out[i] = (uint8_t)s[i];
    out[i] = 0;
}

static MS int32_t st_GetVolumeInformationW(const uint16_t *root, uint16_t *namebuf,
                                           uint32_t namelen, uint32_t *serial,
                                           uint32_t *maxcomp, uint32_t *flags,
                                           uint16_t *fsbuf, uint32_t fslen)
{
    struct statfs sf;
    uint32_t id = 0;
    (void)root;
    if (statfs("/", &sf) == 0) {
        /* fsid is two ints; fold them so both halves reach the serial. */
        uint32_t a = (uint32_t)sf.f_fsid.__val[0], b = (uint32_t)sf.f_fsid.__val[1];
        id = a ^ (b * 0x9e3779b9u);
    }
    if (!id) id = 0xC0FFEE01u;          /* never hand back zero */
    if (serial)  *serial = id;
    if (maxcomp) *maxcomp = 255;
    if (flags)   *flags = 0;
    c2w_("", namebuf, namelen);
    c2w_("NTFS", fsbuf, fslen);
    PLOG("  [vol] GetVolumeInformationW -> serial 0x%08x\n", id);
    return 1;
}

/* ------------------------------------------------- CRT: memory, strings ---
 *
 * A plugin linking the CRT dynamically imports its allocator instead of
 * carrying one, so these cannot be stubs. Absynth's first .CRT$XI entry is
 * literally `p = _malloc_crt(0x100); if (!p) return 1;`, and returning NULL
 * there aborts the whole initialiser table before a single C++ global is
 * constructed.
 *
 * The _crt-suffixed forms are the CRT's own internal allocator. They differ
 * from the public ones only in which heap they charge and how they report
 * failure to the CRT's error hooks -- neither observable here -- so one
 * implementation serves both.
 *
 * Everything here is __cdecl at both widths, hence MSCRT rather than MS. */

static MSCRT void  *st_malloc(size_t n)                    { return w32_alloc(n, 0); }
static MSCRT void  *st_calloc(size_t n, size_t s)          { return w32_alloc(n * s, 1); }
static MSCRT void  *st_realloc(void *p, size_t n)          { return w32_realloc(p, n, 0); }
static MSCRT void   st_free(void *p)                       { w32_free(p); }
static MSCRT void  *st__recalloc(void *p, size_t n, size_t s)
{
    /* Grows without zeroing the new tail, which is what the CRT documents;
     * callers that need it zeroed use _recalloc_crt on fresh memory. */
    return w32_realloc(p, n * s, 0);
}
/* MSVC's _msize reports the requested size; glibc reports the usable size,
 * which is >= that. Callers use it to decide whether to grow, so erring
 * large is safe -- but it is not exact, and a caller comparing it for
 * equality against its own request would see a mismatch. */
static MSCRT size_t st__msize(void *p) { return w32_alloc_size(p); }

static MSCRT void *st_op_new(size_t n)    { return w32_alloc(n, 0); }
static MSCRT void  st_op_delete(void *p)  { w32_free(p); }

static MSCRT void   *st_memcpy(void *d, const void *s, size_t n)  { return memcpy(d, s, n); }
static MSCRT void   *st_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static MSCRT void   *st_memset(void *d, int c, size_t n)          { return memset(d, c, n); }
static MSCRT int     st_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static MSCRT size_t  st_strlen(const char *s)                     { return strlen(s); }
static MSCRT char   *st_strcpy(char *d, const char *s)            { return strcpy(d, s); }
static MSCRT char   *st_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static MSCRT char   *st_strcat(char *d, const char *s)            { return strcat(d, s); }
static MSCRT int     st_strcmp(const char *a, const char *b)      { return strcmp(a, b); }
static MSCRT int     st_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static MSCRT int     st__stricmp(const char *a, const char *b)    { return strcasecmp(a, b); }
/* C says const in, const out; the Windows prototypes these stand in for do
 * not. Casting here is the entire difference, and it is what the real CRT
 * does too. */
static MSCRT char   *st_strchr(const char *s, int c)              { return (char *)strchr(s, c); }
static MSCRT char   *st_strrchr(const char *s, int c)             { return (char *)strrchr(s, c); }
static MSCRT char   *st_strstr(const char *h, const char *n)      { return (char *)strstr(h, n); }
static MSCRT char   *st__strdup(const char *s)
{ return w32_strdup_guest(s ? s : ""); }
static MSCRT size_t  st_wcslen(const uint16_t *s)
{ size_t n = 0; while (s && s[n]) n++; return n; }

/* The rest of the wide-character C runtime.
 *
 * wcslen alone was here because it was the only one anything had needed. The
 * 2015 toolchain changed that: its CRT arrives as the api-ms-win-crt-* apisets
 * (see crt_alias), and a plug-in built with it does every string and number
 * operation through these rather than through the narrow entry points. A
 * SynthEdit plug-in parses its whole patch database out of UTF-16 XML this way.
 *
 * Stubbed, none of this fails loudly. wcsrchr returns null and a path walk
 * quietly finds no extension; wcstod returns 0 and every number in the patch
 * reads as zero. The plug-in then walks off the end of its own data, a long way
 * from here.
 *
 * wchar_t is UTF-16 on Windows and 32-bit here, so the host's own wcs* cannot be
 * called with these pointers -- each one is written out against uint16_t.
 * Const in, non-const out, as the Windows prototypes have it and for the same
 * reason strchr above does. */
static MSCRT uint16_t *st_wcschr(const uint16_t *s, uint16_t c)
{ for (; s && *s; s++) if (*s == c) return (uint16_t *)s;
  return (s && !c) ? (uint16_t *)s : NULL; }
static MSCRT uint16_t *st_wcsrchr(const uint16_t *s, uint16_t c)
{ const uint16_t *r = NULL; for (; s && *s; s++) if (*s == c) r = s;
  return (uint16_t *)((!r && s && !c) ? s : r); }
static MSCRT int st_wcscmp(const uint16_t *a, const uint16_t *b)
{ while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
static MSCRT int st_wcsncmp(const uint16_t *a, const uint16_t *b, size_t n)
{ for (; n && *a && *a == *b; n--, a++, b++) { }
  return n ? (int)*a - (int)*b : 0; }
/* Case folding is ASCII-only, which is what every caller here compares:
 * file extensions, XML element names, parameter identifiers. */
static uint16_t w_lower(uint16_t c)
{ return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c; }
static MSCRT int st__wcsicmp(const uint16_t *a, const uint16_t *b)
{ while (*a && w_lower(*a) == w_lower(*b)) { a++; b++; }
  return (int)w_lower(*a) - (int)w_lower(*b); }
static MSCRT int st__wcsnicmp(const uint16_t *a, const uint16_t *b, size_t n)
{ for (; n && *a && w_lower(*a) == w_lower(*b); n--, a++, b++) { }
  return n ? (int)w_lower(*a) - (int)w_lower(*b) : 0; }
static MSCRT uint16_t *st_wcscpy(uint16_t *d, const uint16_t *s)
{ uint16_t *r = d; while ((*d++ = *s++) != 0) { } return r; }
static MSCRT uint16_t *st_wcsncpy(uint16_t *d, const uint16_t *s, size_t n)
{ uint16_t *r = d; for (; n && *s; n--) *d++ = *s++; while (n--) *d++ = 0; return r; }
static MSCRT uint16_t *st_wcscat(uint16_t *d, const uint16_t *s)
{ uint16_t *r = d; while (*d) d++; while ((*d++ = *s++) != 0) { } return r; }
static MSCRT uint16_t *st_wcsstr(const uint16_t *h, const uint16_t *n)
{
    size_t i, j;
    if (!h || !n) return NULL;
    if (!*n) return (uint16_t *)h;
    for (i = 0; h[i]; i++) {
        for (j = 0; n[j] && h[i + j] == n[j]; j++) { }
        if (!n[j]) return (uint16_t *)(h + i);
    }
    return NULL;
}
static MSCRT uint16_t *st__wcsdup(const uint16_t *s)
{
    size_t n = st_wcslen(s), i;
    uint16_t *d = (uint16_t *)w32_alloc((n + 1) * sizeof *d, 0);
    if (!d) return NULL;
    for (i = 0; i <= n; i++) d[i] = s[i];
    return d;
}

/* Wide numeric conversion, on top of the host's narrow parsers.
 *
 * A number is ASCII in every locale that reaches here, so the leading run of
 * ASCII is copied down to a narrow buffer and parsed there. Copying one
 * character per character is what keeps the end pointer exact -- the offset the
 * narrow parser stopped at is the same offset in the wide string. A non-ASCII
 * character ends the copy, which is where a number would have ended anyway. */
#define W_NUMBUF 160
static size_t w_narrow_prefix(const uint16_t *s, char *buf, size_t n)
{
    size_t i = 0;
    if (!s) { buf[0] = 0; return 0; }
    while (s[i] && s[i] < 0x80 && i + 1 < n) { buf[i] = (char)s[i]; i++; }
    buf[i] = 0;
    return i;
}
#define W_NUM(name, type, call)                                               \
    static MSCRT type st_##name(const uint16_t *s, uint16_t **end, int base)  \
    {                                                                         \
        char b[W_NUMBUF], *e = b;                                             \
        type v;                                                               \
        (void)base;                                                           \
        w_narrow_prefix(s, b, sizeof b);                                      \
        v = call;                                                             \
        if (end) *end = (uint16_t *)s + (size_t)(e - b);                      \
        return v;                                                             \
    }
W_NUM(wcstol,  long,          strtol(b, &e, base))
W_NUM(wcstoul, unsigned long, strtoul(b, &e, base))
static MSCRT double st_wcstod(const uint16_t *s, uint16_t **end)
{
    char b[W_NUMBUF], *e = b;
    double v;
    w_narrow_prefix(s, b, sizeof b);
    v = strtod(b, &e);
    if (end) *end = (uint16_t *)s + (size_t)(e - b);
    return v;
}
static MSCRT int st__wtoi(const uint16_t *s)
{ char b[W_NUMBUF]; w_narrow_prefix(s, b, sizeof b); return atoi(b); }
static MSCRT double st__wtof(const uint16_t *s)
{ char b[W_NUMBUF]; w_narrow_prefix(s, b, sizeof b); return atof(b); }

/* Maths. A synthesiser reaches these on every block, so leaving them stubbed
 * to 0 does not fail loudly -- it renders silence or NaNs, which is worse. */
#define M1(f) static MSCRT double st_##f(double x) { return f(x); }
/* The parameter cannot be called `f` here: `f##f` would paste it with itself and
 * define st_sqrtsqrt calling sqrtsqrt, rather than st_sqrtf calling sqrtf. The
 * export table below asks for st_sqrtf by name. */
#define M1F(fn) static MSCRT float st_##fn##f(float x) { return fn##f(x); }
#define M2(f) static MSCRT double st_##f(double x, double y) { return f(x, y); }
M1(sqrt) M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan)
M1(exp) M1(log) M1(log10) M1(floor) M1(ceil) M1(fabs)
M1(sinh) M1(cosh) M1(tanh)
M1F(sqrt) M1F(sin) M1F(cos) M1F(tan) M1F(exp) M1F(log) M1F(fabs)
M2(pow) M2(fmod) M2(atan2)
#undef M1
#undef M1F
#undef M2
static MSCRT float  st_powf(float x, float y)   { return powf(x, y); }
static MSCRT float  st_atan2f(float x, float y) { return atan2f(x, y); }
static MSCRT double st_ldexp(double x, int e)   { return ldexp(x, e); }
static MSCRT double st_frexp(double x, int *e)  { return frexp(x, e); }
static MSCRT double st_modf(double x, double *i){ return modf(x, i); }

/* The guest's comparator uses the guest convention, so it cannot be handed
 * straight to glibc's qsort. qsort_r carries it through as context instead,
 * which avoids both a trampoline and a thread-local. */
typedef int (MSCRT *crt_cmp)(const void *, const void *);
static int crt_cmp_thunk(const void *a, const void *b, void *ctx)
{ return ((crt_cmp)ctx)(a, b); }
static MSCRT void st_qsort(void *base, size_t n, size_t sz, crt_cmp cmp)
{ qsort_r(base, n, sz, crt_cmp_thunk, (void *)cmp); }
static MSCRT void *st_bsearch(const void *key, const void *base, size_t n,
                              size_t sz, crt_cmp cmp)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = (const char *)base + mid * sz;
        int r = cmp(key, p);
        if (r == 0) return (void *)p;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static MSCRT int    st_abs(int v)                  { return v < 0 ? -v : v; }
static MSCRT int    st_atoi(const char *s)         { return atoi(s); }
static MSCRT double st_atof(const char *s)         { return atof(s); }
static MSCRT long   st_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
static MSCRT double st_strtod(const char *s, char **e)        { return strtod(s, e); }
static MSCRT int    st_rand(void)                  { return rand(); }
static MSCRT void   st_srand(unsigned s)           { srand(s); }
/* ------------------------------------------------------------ environment --
 *
 * getenv and _wgetenv used to answer NULL for everything, and NULL is not a
 * neutral answer. NI Massive asserts and calls ExitProcess(1) when
 * _wgetenv(L"CommonProgramFiles(x86)") comes back empty, because on Windows
 * that variable is always set -- the plug-in never loads and there is nothing
 * in the log to say why.
 *
 * The values are the same ~/.peload prefix the shell folders use, so a path
 * built from %APPDATA% and one from SHGetFolderPath land in the same place, and
 * the directories are created as they are handed out so that opening one works.
 * The host's own environment is deliberately not exposed: a plug-in reading
 * PATH or HOME would get Linux paths where it expects Windows ones. Anything a
 * plug-in sets for itself is kept and takes precedence. */

static const char *w32_guest_dir(const char *leaf, char *buf, size_t n)
{
    const char *home = getenv("HOME");
    char t[512];
    size_t i;
    if (!home || !*home) home = "/tmp";
    if (leaf && *leaf) snprintf(buf, n, "%s/.peload/%s", home, leaf);
    else               snprintf(buf, n, "%s/.peload", home);
    snprintf(t, sizeof t, "%s", buf);
    for (i = 1; t[i]; i++)
        if (t[i] == '/') { t[i] = 0; mkdir(t, 0755); t[i] = '/'; }
    mkdir(t, 0755);
    return buf;
}

typedef struct {
    char     name[64];
    char     value[512];
    uint16_t wide[512];
    int      wide_ok;
} w32_envvar;
static w32_envvar g_env[W32_ENV_MAX];
static int g_nenv;
static pthread_mutex_t g_env_lock = PTHREAD_MUTEX_INITIALIZER;

static void w32_env_put(const char *name, const char *value)
{
    int i;
    for (i = 0; i < g_nenv; i++)
        if (!strcasecmp(g_env[i].name, name)) break;
    if (i == g_nenv) {
        if (g_nenv >= W32_ENV_MAX) return;
        snprintf(g_env[i].name, sizeof g_env[i].name, "%s", name);
        g_nenv++;
    }
    snprintf(g_env[i].value, sizeof g_env[i].value, "%s", value ? value : "");
    g_env[i].wide_ok = 0;
}
static void w32_env_dir(const char *name, const char *leaf)
{
    char buf[512];
    w32_env_put(name, w32_guest_dir(leaf, buf, sizeof buf));
}

static void w32_env_build(void)
{
    char win[512], buf[600];
    const char *user = getenv("USER");
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    w32_env_dir("ProgramFiles",            "Program Files");
    w32_env_dir("ProgramW6432",            "Program Files");
    w32_env_dir("ProgramFiles(x86)",       "Program Files (x86)");
    w32_env_dir("CommonProgramFiles",      "Program Files/Common Files");
    w32_env_dir("CommonProgramW6432",      "Program Files/Common Files");
    w32_env_dir("CommonProgramFiles(x86)", "Program Files (x86)/Common Files");
    w32_env_dir("ProgramData",             "ProgramData");
    w32_env_dir("ALLUSERSPROFILE",         "ProgramData");
    w32_env_dir("APPDATA",                 "AppData/Roaming");
    w32_env_dir("LOCALAPPDATA",            "AppData/Local");
    w32_env_dir("USERPROFILE",             "");
    w32_env_dir("PUBLIC",                  "Public");
    w32_env_dir("TEMP",                    "Temp");
    w32_env_dir("TMP",                     "Temp");
    w32_guest_dir("Windows", win, sizeof win);
    w32_env_put("SystemRoot", win);
    w32_env_put("windir", win);
    snprintf(buf, sizeof buf, "%s/system32", win);
    w32_env_put("PATH", buf);
    snprintf(buf, sizeof buf, "%s/system32/cmd.exe", win);
    w32_env_put("ComSpec", buf);
    w32_env_put("SystemDrive", "C:");
    w32_env_put("HOMEDRIVE",   "C:");
    w32_env_put("HOMEPATH",    "\\");
    w32_env_put("OS",          "Windows_NT");
    w32_env_put("PATHEXT",     ".COM;.EXE;.BAT;.CMD");
    w32_env_put("PROCESSOR_ARCHITECTURE", sizeof(void *) == 8 ? "AMD64" : "x86");
    snprintf(buf, sizeof buf, "%ld", ncpu > 0 ? ncpu : 1);
    w32_env_put("NUMBER_OF_PROCESSORS", buf);
    if (gethostname(buf, sizeof buf) != 0) snprintf(buf, sizeof buf, "PELOAD");
    buf[sizeof buf - 1] = 0;
    { char *d = strchr(buf, '.'); if (d) *d = 0; }
    w32_env_put("COMPUTERNAME", buf);
    w32_env_put("USERDOMAIN",   buf);
    w32_env_put("USERNAME", (user && *user) ? user : "user");
}

static const char *w32_env_find(const char *name)
{
    static int built;
    const char *r = NULL;
    int i;
    if (!name || !*name) return NULL;
    pthread_mutex_lock(&g_env_lock);
    if (!built) { built = 1; w32_env_build(); }
    for (i = 0; i < g_nenv; i++)
        if (!strcasecmp(g_env[i].name, name)) { r = g_env[i].value; break; }
    pthread_mutex_unlock(&g_env_lock);
    return r;
}
static const uint16_t *w32_env_find_w(const char *name)
{
    const uint16_t *r = NULL;
    int i;
    if (!w32_env_find(name)) return NULL;             /* also builds the table */
    pthread_mutex_lock(&g_env_lock);
    for (i = 0; i < g_nenv; i++)
        if (!strcasecmp(g_env[i].name, name)) {
            if (!g_env[i].wide_ok) {
                size_t k;
                for (k = 0; g_env[i].value[k] && k < 511; k++)
                    g_env[i].wide[k] = (uint16_t)(unsigned char)g_env[i].value[k];
                g_env[i].wide[k] = 0;
                g_env[i].wide_ok = 1;
            }
            r = g_env[i].wide;
            break;
        }
    pthread_mutex_unlock(&g_env_lock);
    return r;
}
static void w32_env_store(const char *name, const char *value)
{
    if (!name || !*name) return;
    w32_env_find("OS");                                /* make sure it is built */
    pthread_mutex_lock(&g_env_lock);
    w32_env_put(name, value);
    pthread_mutex_unlock(&g_env_lock);
}

static MS uint32_t st_GetEnvironmentVariableA(const char *name, char *buf, uint32_t n)
{
    const char *v = w32_env_find(name);
    uint32_t need;
    if (!v) { g_last_error = 203; return 0; }          /* ERROR_ENVVAR_NOT_FOUND */
    need = (uint32_t)strlen(v);
    if (!buf || n <= need) return need + 1;
    memcpy(buf, v, need + 1);
    return need;
}
static MS uint32_t st_GetEnvironmentVariableW(const uint16_t *name, uint16_t *buf, uint32_t n)
{
    char nb[128];
    const uint16_t *v;
    uint32_t need = 0;
    w2c(name, nb, sizeof nb);
    v = w32_env_find_w(nb);
    if (!v) { g_last_error = 203; return 0; }
    while (v[need]) need++;
    if (!buf || n <= need) return need + 1;
    memcpy(buf, v, (need + 1) * sizeof *v);
    return need;
}
static MS int32_t st_SetEnvironmentVariableA(const char *name, const char *value)
{ w32_env_store(name, value); return 1; }
static MS int32_t st_SetEnvironmentVariableW(const uint16_t *name, const uint16_t *value)
{
    char nb[128], vb[512];
    w2c(name, nb, sizeof nb);
    if (value) w2c(value, vb, sizeof vb); else vb[0] = 0;
    w32_env_store(nb, value ? vb : "");
    return 1;
}

/* %NAME% substitution, which is how a plug-in written for Windows spells a
 * path in a configuration file. An unknown name is left as it stands, which is
 * what Windows does. */
static MS uint32_t st_ExpandEnvironmentStringsA(const char *src, char *dst, uint32_t n)
{
    char out[2048];
    size_t o = 0;
    const char *p = src;
    if (!src) return 0;
    while (*p && o < sizeof out - 1) {
        const char *e;
        if (*p == '%' && (e = strchr(p + 1, '%')) != NULL) {
            char nb[128];
            size_t len = (size_t)(e - p - 1);
            const char *v;
            if (len < sizeof nb) {
                memcpy(nb, p + 1, len);
                nb[len] = 0;
                if ((v = w32_env_find(nb)) != NULL) {
                    o += (size_t)snprintf(out + o, sizeof out - o, "%s", v);
                    p = e + 1;
                    continue;
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    if (!dst || n <= o) return (uint32_t)o + 1;
    memcpy(dst, out, o + 1);
    return (uint32_t)o + 1;
}
static MS uint32_t st_ExpandEnvironmentStringsW(const uint16_t *src, uint16_t *dst, uint32_t n)
{
    char sb[2048], ob[2048];
    uint32_t need;
    size_t i;
    if (!src) return 0;
    w2c(src, sb, sizeof sb);
    st_ExpandEnvironmentStringsA(sb, ob, sizeof ob);
    need = (uint32_t)strlen(ob);
    if (!dst || n <= need) return need + 1;
    for (i = 0; i < need; i++) dst[i] = (uint16_t)(unsigned char)ob[i];
    dst[need] = 0;
    return need + 1;
}

/* The environment as one block of NAME=VALUE strings, double-NUL terminated. */
static const char *w32_env_block(void)
{
    static char block[W32_ENV_MAX * 600];
    static int built;
    size_t o = 0;
    int i;
    if (built) return block;
    w32_env_find("OS");
    pthread_mutex_lock(&g_env_lock);
    for (i = 0; i < g_nenv && o < sizeof block - 2; i++)
        o += (size_t)snprintf(block + o, sizeof block - o - 1, "%s=%s",
                              g_env[i].name, g_env[i].value) + 1;
    block[o] = 0;
    built = 1;
    pthread_mutex_unlock(&g_env_lock);
    return block;
}
static MS void *st_GetEnvironmentStringsA(void) { return (void *)w32_env_block(); }
static MS int32_t st_FreeEnvironmentStringsA(void *p) { (void)p; return 1; }

static MSCRT int st__putenv(const char *s)
{
    char nb[128];
    const char *eq;
    if (!s || !(eq = strchr(s, '='))) return -1;
    if ((size_t)(eq - s) >= sizeof nb) return -1;
    memcpy(nb, s, (size_t)(eq - s));
    nb[eq - s] = 0;
    w32_env_store(nb, eq + 1);
    return 0;
}
static MSCRT int st_getenv_s(size_t *need, char *buf, size_t n, const char *name)
{
    const char *v = w32_env_find(name);
    if (need) *need = v ? strlen(v) + 1 : 0;
    if (!v) { if (buf && n) buf[0] = 0; return 0; }
    if (!buf || n < strlen(v) + 1) return 34;          /* ERANGE */
    memcpy(buf, v, strlen(v) + 1);
    return 0;
}
static MSCRT int st__dupenv_s(char **out, size_t *len, const char *name)
{
    const char *v = w32_env_find(name);
    if (!out) return 22;                               /* EINVAL */
    *out = NULL;
    if (len) *len = 0;
    if (!v) return 0;
    if (!(*out = (char *)w32_strdup_guest(v))) return 12;   /* ENOMEM */
    if (len) *len = strlen(v) + 1;
    return 0;
}

static MSCRT char  *st_getenv(const char *n)
{ return (char *)w32_env_find(n); }
static MSCRT char  *st_strerror(int e)             { return strerror(e); }
/* CLOCKS_PER_SEC is 1000 on Windows and 1000000 here, so the raw value would
 * be a thousand times too large to any caller dividing by the Windows one. */
static MSCRT long   st_clock(void)
{ return (long)(clock() / (CLOCKS_PER_SEC / 1000)); }

/* ------------------------------------------------------------ the table */

/* ------------------------------------------------- dialogs and the shell ---
 *
 * These are the last of the imports every binary in the corpus asks for. A stub
 * answering zero reads as "the user pressed Cancel", which is harmless but also
 * means File > Open never opens anything. zenity is a real chooser and it is
 * what a desktop Linux box has; with no display, or with zenity absent, the
 * answer falls back to Cancel, which is the same thing a plug-in sees when
 * someone changes their mind. */
#include <sys/wait.h>
static int w32_run_capture(char *const argv[], char *out, size_t n)
{
    int fds[2];
    pid_t pid;
    ssize_t got = 0;
    int status = -1;

    if (out && n) out[0] = 0;
    if (pipe(fds) != 0) return -1;
    if ((pid = fork()) < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    if (out && n > 1) {
        got = read(fds[0], out, n - 1);
        if (got < 0) got = 0;
        out[got] = 0;
        while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r'))
            out[--got] = 0;
    }
    close(fds[0]);
    waitpid(pid, &status, 0);
    return (status == 0) ? 0 : -1;
}
static int w32_have_display(void)
{
    const char *a = getenv("DISPLAY"), *b = getenv("WAYLAND_DISPLAY");
    return (a && *a) || (b && *b);
}

typedef struct {
    uint32_t    lStructSize;
    void       *hwndOwner;
    void       *hInstance;
    const void *lpstrFilter;
    void       *lpstrCustomFilter;
    uint32_t    nMaxCustFilter;
    uint32_t    nFilterIndex;
    void       *lpstrFile;
    uint32_t    nMaxFile;
    void       *lpstrFileTitle;
    uint32_t    nMaxFileTitle;
    const void *lpstrInitialDir;
    const void *lpstrTitle;
    uint32_t    Flags;
    uint16_t    nFileOffset, nFileExtension;
    const void *lpstrDefExt;
    intptr_t    lCustData;
    void       *lpfnHook;
    const void *lpTemplateName;
} W_OPENFILENAME;

static int w32_file_dialog(W_OPENFILENAME *o, int save, int wide)
{
    char picked[1024];
    char *argv[6];
    int n = 0;

    if (!o || !o->lpstrFile || !w32_have_display()) return 0;
    argv[n++] = (char *)"zenity";
    argv[n++] = (char *)"--file-selection";
    if (save) { argv[n++] = (char *)"--save"; argv[n++] = (char *)"--confirm-overwrite"; }
    argv[n] = NULL;
    if (w32_run_capture(argv, picked, sizeof picked) != 0 || !picked[0]) return 0;
    if (wide) {
        uint16_t *w = (uint16_t *)o->lpstrFile;
        uint32_t i, max = o->nMaxFile ? o->nMaxFile : 260;
        for (i = 0; picked[i] && i + 1 < max; i++) w[i] = (uint16_t)(unsigned char)picked[i];
        w[i] = 0;
    } else {
        snprintf((char *)o->lpstrFile, o->nMaxFile ? o->nMaxFile : 260, "%s", picked);
    }
    return 1;
}
static MS int32_t st_GetOpenFileNameW(W_OPENFILENAME *o) { return w32_file_dialog(o, 0, 1); }
static MS int32_t st_GetSaveFileNameW(W_OPENFILENAME *o) { return w32_file_dialog(o, 1, 1); }
static MS int32_t st_GetOpenFileNameA(W_OPENFILENAME *o) { return w32_file_dialog(o, 0, 0); }
static MS int32_t st_GetSaveFileNameA(W_OPENFILENAME *o) { return w32_file_dialog(o, 1, 0); }

/* The folder picker comes in two halves: one returns an opaque item list, the
 * other turns it into a path. The path itself is the only thing either end
 * actually holds, so that is what the "item list" is. */
static MS void *st_SHBrowseForFolderW(void *bi)
{
    char picked[1024], *dup;
    char *argv[] = { (char *)"zenity", (char *)"--file-selection",
                     (char *)"--directory", NULL };
    (void)bi;
    if (!w32_have_display()) return NULL;
    if (w32_run_capture(argv, picked, sizeof picked) != 0 || !picked[0]) return NULL;
    if (!(dup = malloc(strlen(picked) + 1))) return NULL;
    strcpy(dup, picked);
    return dup;
}
static MS void *st_SHBrowseForFolderA(void *bi) { return st_SHBrowseForFolderW(bi); }
static MS int32_t st_SHGetPathFromIDListW(void *idl, uint16_t *out)
{
    const char *p = idl;
    size_t i;
    if (!p || !out) return 0;
    for (i = 0; p[i] && i < 259; i++) out[i] = (uint16_t)(unsigned char)p[i];
    out[i] = 0;
    return 1;
}
static MS int32_t st_SHGetPathFromIDListA(void *idl, char *out)
{
    if (!idl || !out) return 0;
    snprintf(out, 260, "%s", (const char *)idl);
    return 1;
}

/* CHOOSECOLOR: the picked value goes back through rgbResult, which sits after
 * the four pointer-or-DWORD fields at the head of the structure. */
typedef struct {
    uint32_t lStructSize;
    void    *hwndOwner;
    void    *hInstance;
    uint32_t rgbResult;
    uint32_t *lpCustColors;
    uint32_t Flags;
    intptr_t lCustData;
    void    *lpfnHook;
    const void *lpTemplateName;
} W_CHOOSECOLOR;

static MS int32_t st_ChooseColorW(W_CHOOSECOLOR *cc)
{
    char picked[128];
    unsigned r = 0, g = 0, b = 0;
    char *argv[] = { (char *)"zenity", (char *)"--color-selection", NULL };
    if (!cc || !w32_have_display()) return 0;
    if (w32_run_capture(argv, picked, sizeof picked) != 0) return 0;
    if (sscanf(picked, "rgb(%u,%u,%u)", &r, &g, &b) != 3 &&
        sscanf(picked, "#%2x%2x%2x", &r, &g, &b) != 3)
        return 0;
    cc->rgbResult = (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16);
    return 1;
}
static MS int32_t st_ChooseColorA(W_CHOOSECOLOR *cc) { return st_ChooseColorW(cc); }

/* ShellExecute opens a document or a link with whatever the desktop uses for
 * it. Only the verbs that mean "open this" are honoured; "print" and the rest
 * report that no association was found, which is a documented answer. */
static MS void *st_ShellExecuteA(void *hwnd, const char *verb, const char *file,
                                 const char *params, const char *dir, int32_t show)
{
    pid_t pid;
    (void)hwnd; (void)params; (void)dir; (void)show;
    if (!file || !*file) return (void *)(intptr_t)2;         /* ERROR_FILE_NOT_FOUND */
    if (verb && *verb && strcasecmp(verb, "open") && strcasecmp(verb, "explore"))
        return (void *)(intptr_t)31;                         /* SE_ERR_NOASSOC */
    PLOG("  [win] ShellExecute(\"%s\")\n", file);
    /* Only when someone is actually looking. A plug-in that opens its manual or
     * a "buy the full version" page during start-up would otherwise spray
     * browser windows across an unattended corpus run, and a headless machine
     * has nothing for xdg-open to hand the document to anyway. */
    if (!w32_have_display()) return (void *)(intptr_t)42;
    if ((pid = fork()) == 0) {
        char *argv[3];
        argv[0] = (char *)"xdg-open";
        argv[1] = (char *)file;
        argv[2] = NULL;
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) return (void *)(intptr_t)8;                 /* out of memory */
    return (void *)(intptr_t)42;                             /* > 32 means success */
}
static MS void *st_ShellExecuteW(void *hwnd, const uint16_t *verb, const uint16_t *file,
                                 const uint16_t *params, const uint16_t *dir, int32_t show)
{
    char v[64], f[1024];
    if (verb) w2c(verb, v, sizeof v); else v[0] = 0;
    if (file) w2c(file, f, sizeof f); else f[0] = 0;
    (void)params; (void)dir;
    return st_ShellExecuteA(hwnd, verb ? v : NULL, file ? f : NULL, NULL, NULL, show);
}

/* ------------------------------------------------------------- wininet ----
 *
 * Answered as a machine with no network, deliberately and consistently. A
 * plug-in reaching for the internet is checking for updates or calling home,
 * and a host has no business doing either on its behalf; what it does have a
 * duty to do is give a coherent answer, because the stub's zeros are not one --
 * a NULL handle from InternetOpen followed by a "success" from InternetReadFile
 * is a state no caller has a path for. Offline is a state every one of them
 * handles, because it happens on real machines. */
#define H_INET ((void *)(uintptr_t)0x494E4554u)              /* 'INET' */

static MS void *st_InternetOpenA(const char *agent, uint32_t access,
                                 const char *proxy, const char *bypass, uint32_t f)
{ (void)agent;(void)access;(void)proxy;(void)bypass;(void)f; return H_INET; }
static MS void *st_InternetOpenW(const uint16_t *agent, uint32_t access,
                                 const uint16_t *proxy, const uint16_t *bypass, uint32_t f)
{ (void)agent;(void)access;(void)proxy;(void)bypass;(void)f; return H_INET; }
static MS int32_t st_InternetGetConnectedState(uint32_t *flags, uint32_t reserved)
{
    (void)reserved;
    if (flags) *flags = 0;
    g_last_error = 2250;                                     /* ERROR_NOT_CONNECTED */
    return 0;
}
static MS void *st_InternetConnectA(void *h, const char *host, uint16_t port,
                                    const char *user, const char *pass,
                                    uint32_t svc, uint32_t f, uintptr_t ctx)
{
    (void)h;(void)host;(void)port;(void)user;(void)pass;(void)svc;(void)f;(void)ctx;
    g_last_error = 12029;                                    /* ERROR_INTERNET_CANNOT_CONNECT */
    return NULL;
}
static MS void *st_InternetConnectW(void *h, const uint16_t *host, uint16_t port,
                                    const uint16_t *user, const uint16_t *pass,
                                    uint32_t svc, uint32_t f, uintptr_t ctx)
{
    (void)host; (void)user; (void)pass;
    return st_InternetConnectA(h, NULL, port, NULL, NULL, svc, f, ctx);
}
static MS void *st_InternetOpenUrlA(void *h, const char *url, const char *hdrs,
                                    uint32_t hlen, uint32_t f, uintptr_t ctx)
{
    (void)h;(void)url;(void)hdrs;(void)hlen;(void)f;(void)ctx;
    g_last_error = 12029;
    return NULL;
}
static MS void *st_HttpOpenRequestA(void *conn, const char *verb, const char *obj,
                                    const char *ver, const char *ref,
                                    const char **accept, uint32_t f, uintptr_t ctx)
{
    (void)conn;(void)verb;(void)obj;(void)ver;(void)ref;(void)accept;(void)f;(void)ctx;
    g_last_error = 12029;
    return NULL;
}
static MS int32_t st_HttpSendRequestA(void *req, const char *hdrs, uint32_t hlen,
                                      void *opt, uint32_t olen)
{ (void)req;(void)hdrs;(void)hlen;(void)opt;(void)olen; g_last_error = 12029; return 0; }
static MS int32_t st_HttpSendRequestW(void *req, const uint16_t *hdrs, uint32_t hlen,
                                      void *opt, uint32_t olen)
{ (void)req;(void)hdrs;(void)hlen;(void)opt;(void)olen; g_last_error = 12029; return 0; }
static MS int32_t st_HttpQueryInfoA(void *req, uint32_t level, void *buf,
                                    uint32_t *len, uint32_t *idx)
{ (void)req;(void)level;(void)buf;(void)idx; if (len) *len = 0; g_last_error = 12150; return 0; }
static MS int32_t st_InternetReadFile(void *h, void *buf, uint32_t n, uint32_t *got)
{
    (void)h; (void)buf; (void)n;
    if (got) *got = 0;                                       /* zero read is EOF */
    return 1;
}
static MS int32_t st_InternetQueryDataAvailable(void *h, uint32_t *avail,
                                                uint32_t f, uintptr_t ctx)
{ (void)h;(void)f;(void)ctx; if (avail) *avail = 0; return 1; }
static MS int32_t st_InternetSetOptionA(void *h, uint32_t opt, void *buf, uint32_t n)
{ (void)h;(void)opt;(void)buf;(void)n; return 1; }
static MS int32_t st_InternetCloseHandle(void *h) { (void)h; return 1; }

/* Console reads see end-of-file: there is no console attached. */
static MS int32_t st_ReadConsoleW(void *h, void *buf, uint32_t n, uint32_t *got, void *r)
{ (void)h;(void)buf;(void)n;(void)r; if (got) *got = 0; return 1; }
static MS int32_t st_WriteConsoleA(void *h, const void *b, uint32_t n, uint32_t *w, void *r)
{
    (void)h; (void)r;
    if (b && n) fwrite(b, 1, n, stderr);
    if (w) *w = n;
    return 1;
}

#define S(dll, name) { dll, #name, (void *)st_##name }
/* C++ operator new/delete have no identifier spelling, so they are entered by
 * their decorated names -- which differ between widths, and both are listed
 * because one table serves both loaders. */

/* ------------------------------------------- the Concurrency runtime ------ */

/* Concurrency::critical_section and Concurrency::details::_Condition_variable.
 *
 * In MSVC 2013 these live in the *C* runtime, not the C++ one, and a real
 * MSVCP120 fetches them by GetProcAddress while initialising -- so without them
 * it cannot start, which is why they are here rather than left stubbed.
 *
 * The object's storage belongs to the caller and its declared size is not
 * something this side gets to choose. So only the first word of it is used, as a
 * handle to state allocated here. Writing further into an object whose layout the
 * other side decided is the same mistake documented for Objective-C ivars in
 * README.md, and it would corrupt whatever the caller keeps after the field.
 *
 * A failed allocation leaves the handle null and the operations become no-ops:
 * that loses mutual exclusion rather than crashing, and it is reported once. */
typedef struct { pthread_mutex_t m; } conc_cs;
typedef struct { pthread_cond_t  c; } conc_cv;

static void conc_oom(const char *what)
{
    static int said;
    if (!said) { said = 1;
        fprintf(stderr, "peload: out of memory for a %s; locking is disabled\n", what); }
}

static MSCRT void *st_cs_ctor(void **self)
{
    conc_cs *s;
    if (!self) return self;
    if ((s = calloc(1, sizeof *s)) != NULL) pthread_mutex_init(&s->m, NULL);
    else conc_oom("critical_section");
    *self = s;
    return self;                                   /* a constructor returns this */
}

static MSCRT void st_cs_dtor(void **self)
{
    if (!self || !*self) return;
    pthread_mutex_destroy(&((conc_cs *)*self)->m);
    free(*self);
    *self = NULL;                                  /* so a second destroy is safe */
}

static MSCRT void st_cs_lock(void **self)
{ if (self && *self) pthread_mutex_lock(&((conc_cs *)*self)->m); }

static MSCRT void st_cs_unlock(void **self)
{ if (self && *self) pthread_mutex_unlock(&((conc_cs *)*self)->m); }

static MSCRT uint8_t st_cs_try_lock(void **self)
{
    if (!self || !*self) return 1;                 /* uncontended by definition */
    return pthread_mutex_trylock(&((conc_cs *)*self)->m) == 0;
}

static MSCRT void *st_cv_ctor(void **self)
{
    conc_cv *s;
    if (!self) return self;
    if ((s = calloc(1, sizeof *s)) != NULL) pthread_cond_init(&s->c, NULL);
    else conc_oom("condition variable");
    *self = s;
    return self;
}

static MSCRT void st_cv_dtor(void **self)
{
    if (!self || !*self) return;
    pthread_cond_destroy(&((conc_cv *)*self)->c);
    free(*self);
    *self = NULL;
}

static MSCRT void st_cv_wait(void **self, void **cs)
{
    if (!self || !*self || !cs || !*cs) return;
    pthread_cond_wait(&((conc_cv *)*self)->c, &((conc_cs *)*cs)->m);
}

/* Returns whether it was woken rather than timing out. */
static MSCRT uint8_t st_cv_wait_for(void **self, void **cs, uint32_t ms)
{
    struct timespec ts;
    if (!self || !*self || !cs || !*cs) return 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&((conc_cv *)*self)->c,
                                  &((conc_cs *)*cs)->m, &ts) == 0;
}

static MSCRT void st_cv_notify_one(void **self)
{ if (self && *self) pthread_cond_signal(&((conc_cv *)*self)->c); }

static MSCRT void st_cv_notify_all(void **self)
{ if (self && *self) pthread_cond_broadcast(&((conc_cv *)*self)->c); }

/* Nothing here runs inside a task collection, so nothing is ever cancelling. */
static MSCRT uint8_t st_IsCurrentTaskCollectionCanceling(void) { return 0; }

/* set_new_handler returns the handler it replaced. */
static void *g_new_handler;
static MSCRT void *st_set_new_handler(void *h)
{ void *old = g_new_handler; g_new_handler = h; return old; }


/* ------------------------------------------------------ locale and stdio --- */

/* The C++ library asks the C runtime for these while constructing its stream and
 * locale objects, and then reads through what it is given. Stubs that return
 * nothing are worse than useless here: the caller copies from the pointer it was
 * handed, so a null one is a fault a long way from the missing function.
 *
 * All of it describes the "C" locale, which is what a plug-in gets unless it asks
 * for another one. */

/* The character-class table MSVC's ctype macros index directly. The pointer is
 * offset by 128 because those macros index with a plain char, which is signed:
 * a caller passing a byte above 127 lands at a negative index, and the table has
 * to have room there. */
static unsigned short g_ctype[384];
static int g_ctype_ready;

static const unsigned short *w32_ctype_table(void)
{
    if (!g_ctype_ready) {
        int c;
        for (c = 0; c < 128; c++) {
            unsigned short f = 0;
            if (isupper(c))  f |= 0x0001;      /* _UPPER   */
            if (islower(c))  f |= 0x0002;      /* _LOWER   */
            if (isdigit(c))  f |= 0x0004;      /* _DIGIT   */
            if (isspace(c))  f |= 0x0008;      /* _SPACE   */
            if (ispunct(c))  f |= 0x0010;      /* _PUNCT   */
            if (iscntrl(c))  f |= 0x0020;      /* _CONTROL */
            if (c == ' ' || c == '\t') f |= 0x0040;   /* _BLANK */
            if (isxdigit(c)) f |= 0x0080;      /* _HEX     */
            g_ctype[128 + c] = f;
        }
        g_ctype_ready = 1;
    }
    return g_ctype + 128;
}

static MSCRT const unsigned short *st___pctype_func(void) { return w32_ctype_table(); }

/* MSVC's FILE. __iob_func hands back the array holding stdin, stdout and stderr;
 * the C++ library takes their addresses while wiring up cin, cout and cerr, so
 * the three have to exist and be distinct even though nothing is read through
 * them here. */
typedef struct {
    char *_ptr;
    int   _cnt;
    char *_base;
    int   _flag, _file, _charbuf, _bufsiz;
    char *_tmpfname;
} W_FILE;

static W_FILE g_iob[3];
static MSCRT void *st___iob_func(void)
{
    g_iob[0]._file = 0; g_iob[1]._file = 1; g_iob[2]._file = 2;
    return g_iob;
}

/* setlocale returns a pointer to storage it owns, and callers may hold onto it,
 * so this cannot be a string literal the caller might write through. */
static char g_locale_name[32] = "C";
static MSCRT char *st_setlocale(int cat, const char *loc)
{
    (void)cat;
    /* Only the C locale is provided. Reporting that honestly is better than
     * claiming to have switched: a caller that checks gets the truth. */
    if (loc && *loc && strcmp(loc, "C") != 0 && strcmp(loc, "POSIX") != 0)
        return NULL;
    return g_locale_name;
}

static MSCRT int st___lc_codepage_func(void) { return 1252; }   /* Latin-1 */
static MSCRT int st___mb_cur_max_func(void)  { return 1; }
static MSCRT int st__configthreadlocale(int m) { (void)m; return 1; }

static MSCRT int st___crtInitializeCriticalSectionEx(void *cs, uint32_t spin,
                                                    uint32_t flags)
{ (void)spin; (void)flags; st_InitializeCriticalSection(cs); return 1; }


/* The per-category locale descriptors. MSVC has six categories -- LC_ALL,
 * COLLATE, CTYPE, MONETARY, NUMERIC, TIME -- and the C++ library walks these
 * arrays while deciding what a locale means. For the "C" locale every name is
 * null and every handle is the invariant one, which is what these report. */
enum { W_LC_CATEGORIES = 6 };

static uint16_t *g_lc_names[W_LC_CATEGORIES];
static MSCRT uint16_t **st___lc_locale_name_func(void) { return g_lc_names; }

static uint32_t g_lc_handles[W_LC_CATEGORIES];
static MSCRT uint32_t *st___lc_handle_func(void) { return g_lc_handles; }

static MSCRT int st___lc_collate_cp_func(void) { return 1252; }

/* The "C" locale's numeric and monetary conventions, as localeconv reports them:
 * a full stop for the decimal point and nothing else specified. */
typedef struct {
    char *decimal_point, *thousands_sep, *grouping;
    char *int_curr_symbol, *currency_symbol, *mon_decimal_point;
    char *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space;
    char n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
    uint16_t *_W_decimal_point, *_W_thousands_sep;
    uint16_t *_W_int_curr_symbol, *_W_currency_symbol, *_W_mon_decimal_point;
    uint16_t *_W_mon_thousands_sep, *_W_positive_sign, *_W_negative_sign;
} W_LCONV;

static MSCRT void *st_localeconv(void)
{
    static char dot[] = ".", empty[] = "";
    static uint16_t wdot[] = { '.', 0 }, wempty[] = { 0 };
    static W_LCONV lc;
    static int ready;
    if (!ready) {
        ready = 1;
        lc.decimal_point = dot;
        lc.thousands_sep = lc.grouping = empty;
        lc.int_curr_symbol = lc.currency_symbol = empty;
        lc.mon_decimal_point = lc.mon_thousands_sep = lc.mon_grouping = empty;
        lc.positive_sign = lc.negative_sign = empty;
        /* CHAR_MAX means "unspecified", which is the C locale's answer. */
        lc.int_frac_digits = lc.frac_digits = (char)127;
        lc.p_cs_precedes = lc.p_sep_by_space = (char)127;
        lc.n_cs_precedes = lc.n_sep_by_space = (char)127;
        lc.p_sign_posn = lc.n_sign_posn = (char)127;
        lc._W_decimal_point = wdot;
        lc._W_thousands_sep = lc._W_int_curr_symbol = wempty;
        lc._W_currency_symbol = lc._W_mon_decimal_point = wempty;
        lc._W_mon_thousands_sep = lc._W_positive_sign = wempty;
        lc._W_negative_sign = wempty;
    }
    return &lc;
}


/* ------------------------------------------------- ctype and small string --- */

/* Character classification and case folding. These are the plainest functions in
 * the C library and they were reaching stubs, which meant every one of them
 * answered with whatever happened to be in the return register -- so any code
 * parsing a string took an arbitrary branch per character. They are wrapped
 * rather than aliased directly because the C runtime's take an int and the host's
 * are macros in some builds.
 *
 * The classification is the "C" locale's, matching w32_ctype_table above. */
static MSCRT int st_isalpha(int c) { return c >= 0 && c < 128 ? isalpha(c) : 0; }
static MSCRT int st_isupper(int c) { return c >= 0 && c < 128 ? isupper(c) : 0; }
static MSCRT int st_islower(int c) { return c >= 0 && c < 128 ? islower(c) : 0; }
static MSCRT int st_isdigit(int c) { return c >= 0 && c < 128 ? isdigit(c) : 0; }
static MSCRT int st_isxdigit(int c){ return c >= 0 && c < 128 ? isxdigit(c) : 0; }
static MSCRT int st_isspace(int c) { return c >= 0 && c < 128 ? isspace(c) : 0; }
static MSCRT int st_ispunct(int c) { return c >= 0 && c < 128 ? ispunct(c) : 0; }
static MSCRT int st_isalnum(int c) { return c >= 0 && c < 128 ? isalnum(c) : 0; }
static MSCRT int st_isprint(int c) { return c >= 0 && c < 128 ? isprint(c) : 0; }
static MSCRT int st_isgraph(int c) { return c >= 0 && c < 128 ? isgraph(c) : 0; }
static MSCRT int st_iscntrl(int c) { return c >= 0 && c < 128 ? iscntrl(c) : 0; }
static MSCRT int st_tolower(int c) { return c >= 0 && c < 128 ? tolower(c) : c; }
static MSCRT int st_toupper(int c) { return c >= 0 && c < 128 ? toupper(c) : c; }
static MSCRT void *st_memchr(const void *p, int c, size_t n) { return (void *)memchr(p, c, n); }

/* Nothing here is unwinding, so no exception is in flight. */
static MSCRT uint8_t st___uncaught_exception(void) { return 0; }

/* _wsplitpath breaks a wide path into its parts. A stub leaves the caller's
 * buffers untouched -- and it passes uninitialised stack, so what it reads back
 * is whatever was there. Each buffer the caller supplied gets at least an empty
 * string. */
static MSCRT void st__wsplitpath(const uint16_t *path, uint16_t *drive,
                                 uint16_t *dir, uint16_t *fname, uint16_t *ext)
{
    size_t i, n = 0, slash = (size_t)-1, dot = (size_t)-1;

    if (drive) drive[0] = 0;
    if (dir)   dir[0]   = 0;
    if (fname) fname[0] = 0;
    if (ext)   ext[0]   = 0;
    if (!path) return;

    while (path[n]) n++;
    for (i = 0; i < n; i++) {
        if (path[i] == '\\' || path[i] == '/') { slash = i; dot = (size_t)-1; }
        else if (path[i] == '.') dot = i;
    }
    /* A drive letter, if the path opens with one. */
    if (n >= 2 && path[1] == ':' && drive) {
        drive[0] = path[0]; drive[1] = ':'; drive[2] = 0;
    }
    {
        size_t dstart = (n >= 2 && path[1] == ':') ? 2 : 0;
        size_t dend   = (slash == (size_t)-1) ? dstart : slash + 1;
        size_t fend   = (dot == (size_t)-1) ? n : dot;
        size_t k;
        if (dir) {
            for (k = dstart; k < dend; k++) dir[k - dstart] = path[k];
            dir[dend - dstart] = 0;
        }
        if (fname) {
            for (k = dend; k < fend; k++) fname[k - dend] = path[k];
            fname[fend - dend] = 0;
        }
        if (ext && dot != (size_t)-1) {
            for (k = dot; k < n; k++) ext[k - dot] = path[k];
            ext[n - dot] = 0;
        }
    }
}


/* ------------------------------------------------------ process and locale --- */

/* IsWow64Process reports whether a process is a 32-bit one running under the
 * 64-bit emulation layer. This one is native, so the answer is no -- but the
 * answer is returned through a pointer, and a stub that does not write it leaves
 * the caller reading its own uninitialised stack. What the caller then does with
 * that is unpredictable by construction. */
static MS int32_t st_IsWow64Process(void *proc, int32_t *wow64)
{
    (void)proc;
    if (wow64) *wow64 = 0;
    return 1;
}

/* US English, and consistently so across every way of asking. A plug-in picks its
 * resources and number formatting from these, and disagreeing answers send it
 * looking for a localisation that was never built. */
#define W_LANGID_EN_US 0x0409u

static MS uint16_t st_GetUserDefaultLangID(void)       { return W_LANGID_EN_US; }
static MS uint16_t st_GetSystemDefaultLangID(void)     { return W_LANGID_EN_US; }
static MS uint16_t st_GetUserDefaultUILanguage(void)   { return W_LANGID_EN_US; }
static MS uint16_t st_GetSystemDefaultUILanguage(void) { return W_LANGID_EN_US; }
static MS uint16_t st_GetThreadUILanguage(void)        { return W_LANGID_EN_US; }
static MS uint32_t st_GetSystemDefaultLCID(void)       { return W_LANGID_EN_US; }
static MS uint32_t st_GetThreadLocale(void)            { return W_LANGID_EN_US; }
static MS int32_t  st_SetThreadLocale(uint32_t l)      { (void)l; return 1; }


/* --------------------------------------------------- processor topology --- */

/* Both of these report through a caller-supplied buffer, so leaving them
 * unimplemented hands back nothing and the caller reads its own uninitialised
 * memory -- a synthesiser sizing its worker pool from that will do something
 * arbitrary. The numbers come from the machine rather than being invented. */

/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION: a processor mask, a relationship, then a
 * union. 32 bytes on x86-64. */
typedef struct {
    uintptr_t ProcessorMask;
    uint32_t  Relationship;
    uint32_t  pad;
    uint8_t   u[16];
} W_SLPI;

enum { W_RelationProcessorCore = 0, W_RelationNumaNode = 1, W_RelationCache = 2 };

static long w32_ncpu(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

/* The processor topology, which the Concurrency runtime's ResourceManager
 * builds its scheduler out of before main ever runs.
 *
 * A stub answering zero here does not mean "one processor": it means the call
 * failed, and ConcRT's answer to a failed topology query is to throw
 * Concurrency::scheduler_resource_allocation_error out of a static
 * initialiser. That is where all four 32-bit NI plug-ins stopped, with nothing
 * in the log but an exception nobody caught. */
static uintptr_t w32_cpu_mask(void)
{
    long n = w32_ncpu();
    if (n >= (long)(sizeof(uintptr_t) * 8)) return ~(uintptr_t)0;
    return ((uintptr_t)1 << n) - 1;
}
static MS int32_t st_GetProcessAffinityMask(void *h, uintptr_t *proc, uintptr_t *sys)
{
    (void)h;
    if (!proc || !sys) { g_last_error = 87; return 0; }
    *proc = *sys = w32_cpu_mask();
    return 1;
}
static MS int32_t st_SetProcessAffinityMask(void *h, uintptr_t mask)
{ (void)h; (void)mask; return 1; }
/* Returns the *previous* mask, and zero means failure. */
static MS uintptr_t st_SetThreadAffinityMask(void *h, uintptr_t mask)
{ (void)h; (void)mask; return w32_cpu_mask(); }
static MS int32_t st_SetThreadIdealProcessor(void *h, uint32_t n)
{ (void)h; (void)n; return 0; }
static MS uint32_t st_GetActiveProcessorCount(uint16_t group)
{ (void)group; return (uint32_t)w32_ncpu(); }
static MS uint16_t st_GetActiveProcessorGroupCount(void) { return 1; }
static MS uint32_t st_GetMaximumProcessorCount(uint16_t group)
{ (void)group; return (uint32_t)w32_ncpu(); }
static MS uint16_t st_GetMaximumProcessorGroupCount(void) { return 1; }
static MS int32_t st_GetNumaHighestNodeNumber(uint32_t *n)
{ if (!n) { g_last_error = 87; return 0; } *n = 0; return 1; }
static MS int32_t st_GetNumaNodeProcessorMask(uint8_t node, uint64_t *mask)
{
    if (!mask) { g_last_error = 87; return 0; }
    *mask = node ? 0 : (uint64_t)w32_cpu_mask();
    return 1;
}
static MS uint32_t st_GetCurrentProcessorNumber(void)
{
#ifdef _GNU_SOURCE
    int c = sched_getcpu();
    return c < 0 ? 0 : (uint32_t)c;
#else
    return 0;
#endif
}

/* The processor-group API, which is where the Concurrency runtime actually
 * stopped. ConcRT resolves SetThreadGroupAffinity and GetThreadGroupAffinity
 * through GetProcAddress and, finding neither, throws
 * scheduler_resource_allocation_error rather than falling back -- so answering
 * NULL for them is not the harmless "this is an older Windows" it looks like.
 * One group holding every processor is the truthful answer for any machine
 * with 64 or fewer cores, which is the only shape KAFFINITY can describe. */
enum { W_RelationProcessorPackage = 3, W_RelationGroup = 4, W_RelationAll = 0xFFFF };

typedef struct { uintptr_t Mask; uint16_t Group; uint16_t Reserved[3]; } W_GROUP_AFFINITY;

typedef struct {
    uint8_t  Flags, EfficiencyClass, Reserved[20];
    uint16_t GroupCount;
    W_GROUP_AFFINITY GroupMask[1];
} W_PROC_REL;
typedef struct {
    uint32_t NodeNumber;
    uint8_t  Reserved[20];
    W_GROUP_AFFINITY GroupMask;
} W_NUMA_REL;
typedef struct {
    uint8_t   MaximumProcessorCount, ActiveProcessorCount, Reserved[38];
    uintptr_t ActiveProcessorMask;
} W_GROUP_INFO;
typedef struct {
    uint16_t MaximumGroupCount, ActiveGroupCount;
    uint8_t  Reserved[20];
    W_GROUP_INFO GroupInfo[1];
} W_GROUP_REL;
typedef struct {
    uint32_t Relationship, Size;
    union { W_PROC_REL Processor; W_NUMA_REL NumaNode; W_GROUP_REL Group; } u;
} W_SLPIEX;

static void w32_group_affinity(W_GROUP_AFFINITY *g)
{
    memset(g, 0, sizeof *g);
    g->Mask = w32_cpu_mask();
    g->Group = 0;
}

/* Entries are variable length and packed nose to tail, each carrying its own
 * Size; the caller walks them by that field, so it has to be right. */
static uint32_t w32_slpiex_emit(uint32_t rel, uint8_t *out, uint32_t room, uint32_t *used)
{
    long n = w32_ncpu(), i;
    uint32_t total = 0;
    int want_core = (rel == W_RelationProcessorCore || rel == W_RelationAll);
    int want_numa = (rel == W_RelationNumaNode || rel == W_RelationAll);
    int want_pkg  = (rel == W_RelationProcessorPackage || rel == W_RelationAll);
    int want_grp  = (rel == W_RelationGroup || rel == W_RelationAll);

    if (n > 64) n = 64;
#define W32_EMIT(kind, sz)                                                     \
    do {                                                                       \
        uint32_t need = (uint32_t)(offsetof(W_SLPIEX, u) + (sz));              \
        need = (need + 7u) & ~7u;                                              \
        if (out && total + need <= room) {                                     \
            W_SLPIEX *e = (W_SLPIEX *)(out + total);                           \
            memset(e, 0, need);                                                \
            e->Relationship = (kind);                                          \
            e->Size = need;                                                    \
            cur = e;                                                           \
        } else cur = NULL;                                                     \
        total += need;                                                         \
    } while (0)

    {
        W_SLPIEX *cur;
        if (want_core)
            for (i = 0; i < n; i++) {
                W32_EMIT(W_RelationProcessorCore, sizeof(W_PROC_REL));
                if (cur) {
                    cur->u.Processor.Flags = 0;          /* no SMT sibling */
                    cur->u.Processor.GroupCount = 1;
                    cur->u.Processor.GroupMask[0].Mask = (uintptr_t)1 << i;
                }
            }
        if (want_numa) {
            W32_EMIT(W_RelationNumaNode, sizeof(W_NUMA_REL));
            if (cur) w32_group_affinity(&cur->u.NumaNode.GroupMask);
        }
        if (want_pkg) {
            W32_EMIT(W_RelationProcessorPackage, sizeof(W_PROC_REL));
            if (cur) {
                cur->u.Processor.GroupCount = 1;
                w32_group_affinity(&cur->u.Processor.GroupMask[0]);
            }
        }
        if (want_grp) {
            W32_EMIT(W_RelationGroup, sizeof(W_GROUP_REL));
            if (cur) {
                cur->u.Group.MaximumGroupCount = 1;
                cur->u.Group.ActiveGroupCount  = 1;
                cur->u.Group.GroupInfo[0].MaximumProcessorCount = (uint8_t)n;
                cur->u.Group.GroupInfo[0].ActiveProcessorCount  = (uint8_t)n;
                cur->u.Group.GroupInfo[0].ActiveProcessorMask   = w32_cpu_mask();
            }
        }
    }
#undef W32_EMIT
    if (used) *used = total;
    return total;
}

static MS int32_t st_GetLogicalProcessorInformationEx(uint32_t rel, void *buf, uint32_t *len)
{
    uint32_t need;
    if (!len) { g_last_error = 87; return 0; }
    need = w32_slpiex_emit(rel, NULL, 0, NULL);
    if (!need) { g_last_error = 87; return 0; }
    if (!buf || *len < need) {
        *len = need;
        g_last_error = 122;                      /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    w32_slpiex_emit(rel, (uint8_t *)buf, *len, len);
    return 1;
}
static MS int32_t st_GetThreadGroupAffinity(void *h, W_GROUP_AFFINITY *g)
{
    (void)h;
    if (!g) { g_last_error = 87; return 0; }
    w32_group_affinity(g);
    return 1;
}
static MS int32_t st_SetThreadGroupAffinity(void *h, const W_GROUP_AFFINITY *g,
                                            W_GROUP_AFFINITY *prev)
{
    (void)h; (void)g;
    if (prev) w32_group_affinity(prev);
    return 1;
}
static MS int32_t st_GetNumaNodeProcessorMaskEx(uint16_t node, W_GROUP_AFFINITY *g)
{
    if (!g) { g_last_error = 87; return 0; }
    w32_group_affinity(g);
    if (node) { g->Mask = 0; return 0; }
    return 1;
}
static MS int32_t st_GetThreadIdealProcessorEx(void *h, void *p)
{
    (void)h;
    if (!p) { g_last_error = 87; return 0; }
    memset(p, 0, 4);                             /* PROCESSOR_NUMBER */
    return 1;
}
static MS int32_t st_SetThreadIdealProcessorEx(void *h, void *p, void *prev)
{ (void)h; (void)p; if (prev) memset(prev, 0, 4); return 1; }

/* PROCESSOR_NUMBER: group, number, reserved. */
static MS void st_GetCurrentProcessorNumberEx(void *p)
{
    if (!p) return;
    memset(p, 0, 4);
    ((uint8_t *)p)[2] = (uint8_t)st_GetCurrentProcessorNumber();
}
static MS int32_t st_GetNumaProximityNodeEx(uint32_t prox, uint16_t *node)
{ (void)prox; if (!node) { g_last_error = 87; return 0; } *node = 0; return 1; }
static MS int32_t st_GetNumaAvailableMemoryNodeEx(uint16_t node, uint64_t *bytes)
{
    if (!bytes) { g_last_error = 87; return 0; }
    *bytes = node ? 0 : (uint64_t)sysconf(_SC_AVPHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
    return node ? 0 : 1;
}
static MS int32_t st_GetNumaNodeNumberFromHandle(void *h, uint16_t *node)
{ (void)h; if (!node) { g_last_error = 87; return 0; } *node = 0; return 1; }
static MS int32_t st_QueryThreadCycleTime(void *h, uint64_t *cycles)
{
    struct timespec t;
    (void)h;
    if (!cycles) { g_last_error = 87; return 0; }
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    *cycles = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    return 1;
}

static MS int32_t st_GetLogicalProcessorInformation(void *buf, uint32_t *len)
{
    long n = w32_ncpu(), i;
    uint32_t need;

    if (n > 64) n = 64;                        /* one mask's worth */
    need = (uint32_t)((n + 1) * sizeof(W_SLPI));
    if (!len) { g_last_error = 87; return 0; }
    if (!buf || *len < need) {
        *len = need;
        g_last_error = 122;                    /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    {
        W_SLPI *e = buf;
        memset(e, 0, need);
        for (i = 0; i < n; i++) {
            e[i].ProcessorMask = (uintptr_t)1 << i;
            e[i].Relationship  = W_RelationProcessorCore;
            /* u[0] is the flags byte: 0 means this core has no second thread,
             * which is the honest answer without hyper-threading topology. */
        }
        e[n].ProcessorMask = n >= 64 ? (uintptr_t)-1 : (((uintptr_t)1 << n) - 1);
        e[n].Relationship  = W_RelationNumaNode;
        *len = need;
    }
    return 1;
}

/* PROCESSOR_POWER_INFORMATION, one per logical processor: six 32-bit fields. */
typedef struct {
    uint32_t Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
} W_PPI;

static uint32_t w32_cpu_mhz(void)
{
    /* cpufreq reports kHz when it is present; otherwise fall back to what
     * /proc/cpuinfo says. Either is a real number for this machine. */
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    unsigned long khz = 0;
    if (f) { if (fscanf(f, "%lu", &khz) != 1) khz = 0; fclose(f); }
    if (khz) return (uint32_t)(khz / 1000);
    if ((f = fopen("/proc/cpuinfo", "r")) != NULL) {
        char line[256];
        double mhz = 0;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "cpu MHz : %lf", &mhz) == 1) break;
        fclose(f);
        if (mhz > 0) return (uint32_t)mhz;
    }
    return 2400;
}

static MS uint32_t st_CallNtPowerInformation(int level, void *in, uint32_t inlen,
                                            void *out, uint32_t outlen)
{
    (void)in; (void)inlen;
    if (level == 11 /* ProcessorInformation */) {
        long n = w32_ncpu(), i;
        uint32_t mhz = w32_cpu_mhz();
        if (!out || outlen < n * sizeof(W_PPI)) return 0xC0000023u; /* BUFFER_TOO_SMALL */
        {
            W_PPI *p = out;
            memset(p, 0, (size_t)n * sizeof *p);
            for (i = 0; i < n; i++) {
                p[i].Number     = (uint32_t)i;
                p[i].MaxMhz     = mhz;
                p[i].CurrentMhz = mhz;
                p[i].MhzLimit   = mhz;
                p[i].MaxIdleState = 0;
                p[i].CurrentIdleState = 0;
            }
        }
        return 0;                               /* STATUS_SUCCESS */
    }
    /* Anything else is genuinely not provided, and saying so is better than
     * returning success over a buffer nothing filled in. */
    return 0xC0000002u;                         /* STATUS_NOT_IMPLEMENTED */
}


/* ------------------------------------------------- machine and drive info --- */

static MS void st_OutputDebugStringW(const uint16_t *w)
{ char b[1024]; w2c(w, b, sizeof b); fprintf(stderr, "  [dbg] %s", b); }

/* The machine's own hostname, which is what a caller asking for it wants -- and
 * what a licence tied to this machine would be issued against. */
static MS int32_t st_GetComputerNameA(char *buf, uint32_t *len)
{
    char host[256];
    uint32_t n;
    if (gethostname(host, sizeof host) != 0) snprintf(host, sizeof host, "localhost");
    host[sizeof host - 1] = 0;
    { char *dot = strchr(host, '.'); if (dot) *dot = 0; }
    n = (uint32_t)strlen(host);
    if (!len) { g_last_error = 87; return 0; }
    if (!buf || *len <= n) { *len = n + 1; g_last_error = 111 /* BUFFER_OVERFLOW */; return 0; }
    memcpy(buf, host, n + 1);
    *len = n;
    return 1;
}

static MS int32_t st_GetComputerNameW(uint16_t *buf, uint32_t *len)
{
    char host[256];
    uint32_t n, i;
    if (!len) { g_last_error = 87; return 0; }
    { uint32_t hl = sizeof host; if (!st_GetComputerNameA(host, &hl)) { 
          snprintf(host, sizeof host, "localhost"); } }
    n = (uint32_t)strlen(host);
    if (!buf || *len <= n) { *len = n + 1; g_last_error = 111; return 0; }
    for (i = 0; i <= n; i++) buf[i] = (uint16_t)(unsigned char)host[i];
    *len = n;
    return 1;
}

/* One fixed drive, C:. Reporting none at all sends a caller looking for somewhere
 * to write and finding nowhere, which it is entitled to treat as fatal. The
 * buffer is a run of NUL-terminated strings ended by a second NUL. */
static MS uint32_t st_GetLogicalDriveStringsW(uint32_t len, uint16_t *buf)
{
    static const uint16_t c[] = { 'C', ':', '\\', 0, 0 };
    uint32_t need = 5;
    if (!buf || len < need) return need;
    memcpy(buf, c, sizeof c);
    return need - 1;                       /* not counting the final NUL */
}

static MS uint32_t st_GetLogicalDriveStringsA(uint32_t len, char *buf)
{
    static const char c[] = { 'C', ':', '\\', 0, 0 };
    uint32_t need = 5;
    if (!buf || len < need) return need;
    memcpy(buf, c, sizeof c);
    return need - 1;
}

static MS uint32_t st_GetLogicalDrives(void) { return 1u << 2; }   /* just C: */

enum { W_DRIVE_NO_ROOT_DIR = 1, W_DRIVE_FIXED = 3 };

static MS uint32_t st_GetDriveTypeA(const char *root)
{
    /* Everything this host exposes is one fixed disk. */
    if (!root || !*root) return W_DRIVE_FIXED;
    if ((root[0] | 32) == 'c') return W_DRIVE_FIXED;
    return W_DRIVE_NO_ROOT_DIR;
}

static MS uint32_t st_GetDriveTypeW(const uint16_t *root)
{ char b[64]; if (!root) return W_DRIVE_FIXED; w2c(root, b, sizeof b); return st_GetDriveTypeA(b); }

/* Device-arrival notifications never fire here, but the registration has to
 * succeed: a null return is an error the caller may refuse to continue past. */
static MS void *st_RegisterDeviceNotificationW(void *recip, void *filter, uint32_t f)
{ (void)recip;(void)filter;(void)f; return (void *)0x4E494446; /* 'NIDF' */ }
static MS void *st_RegisterDeviceNotificationA(void *recip, void *filter, uint32_t f)
{ return st_RegisterDeviceNotificationW(recip, filter, f); }
static MS int32_t st_UnregisterDeviceNotification(void *h) { (void)h; return 1; }

/* --------------------------------------------------------- a C++ throw ----- */

/* _CxxThrowException is how MSVC raises a C++ exception: it hands over the object
 * and a description of its type, and never returns -- the compiler emits no code
 * for the case where it does. A stub that returns therefore drops execution into
 * a path the compiler proved unreachable, and whatever happens next is arbitrary.
 *
 * Real dispatch needs Windows x64 SEH and MSVC's frame-handler tables, which is
 * not in place. Until it is, the honest thing is to say what was thrown and stop,
 * because the type name is exactly what identifies why -- and it is right there in
 * the ThrowInfo the caller passed. */
/* Read memory that might not be mapped, without risking a fault. The kernel
 * reports a bad address as an error here, where a plain dereference would raise
 * SIGSEGV. Used for diagnostics that follow pointers found by guesswork. */
static long safe_peek(const void *src, void *dst, size_t n)
{
    struct iovec l, r;
    l.iov_base = dst; l.iov_len = n;
    r.iov_base = (void *)src; r.iov_len = n;
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0);
}

#if defined(__x86_64__)
/* Capture the *guest's* register state at the throw, in assembly, for the same
 * reason setjmp needed it: the unwinder has to start from the frame that threw,
 * and by the time C code runs it is looking at its own frame instead.
 *
 * The stack maths: entry rsp is 8 mod 16 (the call pushed a return address onto
 * an aligned stack). 80 bytes for the context and 40 for the callee's shadow
 * space plus padding leaves rsp 0 mod 16 at the call, so the callee sees the
 * 8 mod 16 it is entitled to. */
__asm__(
".text\n"
".globl peload_cxx_throw\n"
".type peload_cxx_throw,@function\n"
"peload_cxx_throw:\n"
"    subq  $80, %rsp\n"
"    movq  80(%rsp), %rax\n"        /* the throwing instruction's return address */
"    movq  %rax,  0(%rsp)\n"
"    leaq  88(%rsp), %rax\n"        /* the guest's rsp before it called us */
"    movq  %rax,  8(%rsp)\n"
"    movq  %rbx, 16(%rsp)\n"
"    movq  %rbp, 24(%rsp)\n"
"    movq  %rsi, 32(%rsp)\n"
"    movq  %rdi, 40(%rsp)\n"
"    movq  %r12, 48(%rsp)\n"
"    movq  %r13, 56(%rsp)\n"
"    movq  %r14, 64(%rsp)\n"
"    movq  %r15, 72(%rsp)\n"
"    subq  $40, %rsp\n"
"    leaq  40(%rsp), %r8\n"         /* third argument: the captured state */
"    call  peload_cxx_throw_c\n"
"    addq  $120, %rsp\n"
"    ret\n"                          /* only if the dispatcher declined to act */
".size peload_cxx_throw,.-peload_cxx_throw\n"
);

/* Declared with the Microsoft convention: the assembly above reads its arguments
 * from %rcx and %rdx, and the guest calls it that way. */
MS void peload_cxx_throw(void *object, const void *throwinfo);

typedef struct {
    uint64_t rip, rsp, rbx, rbp, rsi, rdi, r12, r13, r14, r15;
} cxx_throw_state;

/* Also Microsoft-convention: the assembly puts its arguments in %rcx/%rdx/%r8.
 * Declaring it System V would read them from %rdi/%rsi/%rdx instead -- which is
 * three wrong pointers, and a fault before the first line runs. */
MS void peload_cxx_throw_c(void *object, const ms_throwinfo *ti, cxx_throw_state *st);

/* Walk the guest's frames looking for a catch that accepts this object, and say
 * what was found. Dispatch itself -- copying the object into the catch's slot,
 * running the intervening destructors and transferring to the funclet -- is not
 * in place yet, so this stops rather than pretending to have handled it. What it
 * does establish is whether a handler exists at all: if none does, the throw is
 * genuinely fatal and no amount of dispatch machinery would change it. */
MS void peload_cxx_throw_c(void *object, const ms_throwinfo *ti, cxx_throw_state *st)
{
    const uint8_t *base = g_image_base;
    ms_ctx ctx;
    const char *tname = "type unknown";
    int depth, handler_frames = 0;

    memset(&ctx, 0, sizeof ctx);
    ctx.rip = st->rip;
    ctx.reg[MS_RSP] = st->rsp;
    ctx.reg[MS_RBX] = st->rbx; ctx.reg[MS_RBP] = st->rbp;
    ctx.reg[MS_RSI] = st->rsi; ctx.reg[MS_RDI] = st->rdi;
    ctx.reg[12] = st->r12; ctx.reg[13] = st->r13;
    ctx.reg[14] = st->r14; ctx.reg[15] = st->r15;

    /* Where the throw came from, as an image offset. Without it the type name says
     * what went wrong but nothing about which code decided so. */
    if (pe_verbose() && st->rip >= (uint64_t)(uintptr_t)base)
        fprintf(stderr, "  [eh] throw raised at image+0x%x\n",
                (unsigned)(st->rip - (uint64_t)(uintptr_t)base));

    /* Most exception classes carry the reason as a char* member -- it is the whole
     * point of the object -- so the first few words are scanned for one. That
     * message is far more use than the type name: "unable to open database file"
     * says what to fix, where CppSQLite3Exception does not.
     *
     * The words are guesses, so they are read through process_vm_readv rather
     * than dereferenced: it reports unreadable memory as an error instead of
     * raising a fault. Dereferencing directly crashed the dispatcher on the first
     * object whose members were not pointers -- diagnostics that can kill the
     * process are worse than none. */
    if (pe_verbose() && object) {
        uint64_t words[6];
        int k;
        if (safe_peek(object, words, sizeof words) == (long)sizeof words) {
            for (k = 0; k < 6; k++) {
                char text[121];
                long got;
                int i, printable = 0;
                if (words[k] < 0x10000) continue;
                got = safe_peek((const void *)(uintptr_t)words[k], text, sizeof text - 1);
                if (got <= 3) continue;
                text[got] = 0;
                for (i = 0; i < got; i++) {
                    unsigned char c = (unsigned char)text[i];
                    if (c == 0) break;
                    if (c < 0x20 || c > 0x7E) { printable = -1; break; }
                    printable++;
                }
                if (printable > 3) {
                    text[printable] = 0;
                    fprintf(stderr, "  [eh]   message: \"%s\"\n", text);
                }
            }
        }
    }

    if (ti && ti->pCatchableTypeArray) {
        const int32_t *cta = (const int32_t *)(base + ti->pCatchableTypeArray);
        if (cta[0] > 0) {
            const ms_catchabletype *ct =
                (const ms_catchabletype *)(base + cta[1]);
            if (ct->pType) tname = ((const ms_typedesc *)(base + ct->pType))->name;
        }
    }

    for (depth = 0; depth < 64; depth++) {
        uint32_t rva, hrva;
        const ms_runtime_function *fn;
        const void *hdata = NULL;

        if (ctx.rip < (uint64_t)(uintptr_t)base) break;
        rva = (uint32_t)(ctx.rip - (uint64_t)(uintptr_t)base);
        fn = ms_find_function(base, rva);
        hrva = ms_handler_rva(base, fn, &hdata);
        if (hrva && hdata) {
            const ms_funcinfo *fi =
                (const ms_funcinfo *)(base + *(const uint32_t *)hdata);
            handler_frames++;
            if (ms_funcinfo_ok(fi) && fi->nTryBlocks && fi->tryBlockMap) {
                int state = ms_state_for_ip(base, fi, rva);
                const ms_tryblock *tb =
                    (const ms_tryblock *)(base + fi->tryBlockMap);
                uint32_t t;
                for (t = 0; t < fi->nTryBlocks; t++) {
                    int c;
                    const ms_handlertype *h;
                    if (state < tb[t].tryLow || state > tb[t].tryHigh) continue;
                    if (!tb[t].handlerArray) continue;
                    h = (const ms_handlertype *)(base + tb[t].handlerArray);
                    for (c = 0; c < tb[t].nCatches; c++) {
                        uint64_t frame;
                        void *cont;
                        uint64_t saved[8];

                        if (!ms_catch_matches(base, &h[c], ti, base)) continue;

                        frame = ms_establisher_frame(base, fn, &ctx);
                        if (pe_verbose())
                            fprintf(stderr, "  [eh] %s caught by catch (%s) "
                                    "%d frame(s) up, funclet image+0x%x\n",
                                    tname,
                                    h[c].pType ? ((const ms_typedesc *)
                                        (base + h[c].pType))->name : "...",
                                    depth, (unsigned)h[c].addressOfHandler);

                        /* Release what this frame owns between where the throw
                         * happened and the try being entered. Everything further
                         * in was already released as the walk passed through it. */
                        ms_run_unwind(base, fi, frame, state, tb[t].tryLow - 1);

                        ms_deliver_object(base, &h[c], ti, object, frame);

                        /* The funclet runs the catch body and hands back where in
                         * the parent to carry on. It is called on this stack, and
                         * reaches the parent's variables through the frame it is
                         * given. */
                        cont = ((ms_catch_funclet)(void *)
                                (base + h[c].addressOfHandler))(NULL, frame);
                        if (!cont) {
                            fprintf(stderr, "[win] a catch funclet gave no "
                                            "continuation address\n");
                            fflush(stderr);
                            abort();
                        }
                        saved[0] = ctx.reg[MS_RBX]; saved[1] = ctx.reg[MS_RBP];
                        saved[2] = ctx.reg[MS_RSI]; saved[3] = ctx.reg[MS_RDI];
                        saved[4] = ctx.reg[12]; saved[5] = ctx.reg[13];
                        saved[6] = ctx.reg[14]; saved[7] = ctx.reg[15];
                        peload_cxx_resume((uint64_t)(uintptr_t)cont,
                                          ctx.reg[MS_RSP], saved);
                        /* not reached */
                        abort();
                    }
                }
            }
        }
        /* Nothing here caught it, so this frame is being discarded: run its
         * destructors before the walk leaves it behind. Innermost first, which is
         * the order this loop already visits them in. */
        if (hrva && hdata) {
            const ms_funcinfo *fi =
                (const ms_funcinfo *)(base + *(const uint32_t *)hdata);
            if (ms_funcinfo_ok(fi)) {
                int state = ms_state_for_ip(base, fi, rva);
                if (state >= 0)
                    ms_run_unwind(base, fi, ms_establisher_frame(base, fn, &ctx),
                                  state, -1);
            }
        }
        if (!ms_unwind_step(base, &ctx)) break;
    }

    fprintf(stderr, "[win] C++ throw of %s (object %p) is unhandled\n",
            tname, object);
    fprintf(stderr, "[win]   walked %d frame(s), %d with a language handler, "
                    "none of them catching this type\n",
            depth + 1, handler_frames);
    fflush(stderr);
    abort();
}
#define st__CxxThrowException peload_cxx_throw
#else
/* On i386 a C++ throw is an ordinary Windows exception, and this host already
 * dispatches those: RaiseException walks the fs:[0] chain and hands the record
 * to each __CxxFrameHandler in turn. So _CxxThrowException is what MSVC's own
 * is -- the three parameters that make an exception record a C++ one -- rather
 * than the abort that used to stand here, which took down every i386 plug-in
 * that opened an editor and threw inside it. */
static MS void st__CxxThrowException(void *object, const void *throwinfo)
{
    uintptr_t a[3];
    a[0] = 0x19930520u;                       /* EH_MAGIC_NUMBER1 */
    a[1] = (uintptr_t)object;
    a[2] = (uintptr_t)throwinfo;
    st_RaiseException(0xE06D7363u, 1 /* EXCEPTION_NONCONTINUABLE */, 3, a);
    /* A C++ throw that nobody catches does not come back. */
    fprintf(stderr, "[win] C++ throw returned from dispatch -- nothing caught it\n");
    fflush(stderr);
    abort();
}
#endif

/* ------------------------------------------------------------ dynamic_cast --- */

/* __RTDynamicCast is what `dynamic_cast<T*>(p)` compiles to.
 *
 * A stub returning 0 is not a soft failure. The compiler emits a null check
 * only where the source says the cast might fail; a cast the programmer knows
 * cannot fail is used immediately, so a stub turns every one of them into a
 * null dereference somewhere else entirely. That is exactly how it presented:
 * a VST3 plug-in faulted reading address 0x3c8, which is `lea rcx,[rax+0x3c8]`
 * on a NULL rax two instructions after the call, while the *next* block in the
 * same function tested for null before using the same pointer.
 *
 * MSVC records the whole class hierarchy in the image, so this is a lookup
 * rather than a guess. The object's vftable is preceded by a pointer to a
 * CompleteObjectLocator, which names the type, says where this subobject sits
 * inside the complete object, and points at a hierarchy descriptor listing
 * every base. Walk the bases for one whose mangled name matches the target and
 * the cast is that base's address; find none and the cast genuinely fails.
 *
 * Types are compared by mangled name rather than by TypeDescriptor address: a
 * class defined in two images has two descriptors, and comparing addresses
 * would refuse a cast that must succeed. Names are what MSVC's own runtime
 * compares for the same reason. */

typedef struct {
    uint32_t signature;         /* 0: fields are pointers (i386). 1: RVAs (x64) */
    uint32_t offset;            /* this vftable's offset within the complete object */
    uint32_t cdOffset;
    uint32_t pTypeDescriptor;
    uint32_t pClassDescriptor;
    uint32_t pSelf;             /* signature 1 only: this locator's own RVA */
} ms_objlocator;

typedef struct {
    uint32_t signature, attributes, numBaseClasses, pBaseClassArray;
} ms_hierarchy;

/* RTTIBaseClassDescriptor, read by offset rather than as a struct: the array
 * holds pointers to these, so only the field offsets matter and a trailing
 * field this does not use cannot shift anything. */
#define BCD_TYPEDESC 0
#define BCD_MDISP    8
#define BCD_PDISP    12
#define BCD_VDISP    16

/* The mangled name inside a TypeDescriptor: a vftable pointer, a spare word,
 * then the name. Two pointer-widths in, at either width. */
static const char *ms_td_name(const void *td)
{ return td ? (const char *)td + 2 * sizeof(void *) : NULL; }

static MS void *st___RTDynamicCast(void *inptr, int32_t VfDelta,
                                   void *SrcType, void *TargetType,
                                   int32_t isReference)
{
    const ms_objlocator *col;
    const ms_hierarchy *h;
    const uint8_t *base, *complete;
    const uint32_t *arr;
    const char *want;
    uint32_t i;

    (void)VfDelta; (void)SrcType;
    if (!inptr || !TargetType) return NULL;

    /* vftable[-1] is the locator. */
    {
        const void *const *vft = *(const void *const **)inptr;
        if (!vft) return NULL;
        col = (const ms_objlocator *)vft[-1];
    }
    if (!col) return NULL;

    /* Where RVAs are measured from. The locator carries its own RVA precisely
     * so a 64-bit image can recover its base from any object, which is what
     * makes this work without knowing which module the object came from --
     * and a plug-in's objects and its runtime's are not always the same one. */
    if (col->signature == 1) {
        base = (const uint8_t *)col - col->pSelf;
    } else {
        base = NULL;                              /* i386: fields are pointers */
    }
#define MS_RTTI_AT(x) (base ? base + (x) : (const uint8_t *)(uintptr_t)(x))

    want = ms_td_name(TargetType);
    if (!want) return NULL;

    complete = (const uint8_t *)inptr - col->offset;

    h = (const ms_hierarchy *)MS_RTTI_AT(col->pClassDescriptor);
    if (!h || !h->numBaseClasses || h->numBaseClasses > 4096) return NULL;
    arr = (const uint32_t *)MS_RTTI_AT(h->pBaseClassArray);
    if (!arr) return NULL;

    for (i = 0; i < h->numBaseClasses; i++) {
        const uint8_t *bcd = MS_RTTI_AT(arr[i]);
        const void *td;
        int32_t mdisp, pdisp, vdisp;
        const uint8_t *p;

        if (!bcd) continue;
        td = MS_RTTI_AT(*(const uint32_t *)(bcd + BCD_TYPEDESC));
        if (!td || strcmp(ms_td_name(td), want) != 0) continue;

        mdisp = *(const int32_t *)(bcd + BCD_MDISP);
        pdisp = *(const int32_t *)(bcd + BCD_PDISP);
        vdisp = *(const int32_t *)(bcd + BCD_VDISP);

        p = complete + mdisp;
        if (pdisp >= 0) {
            /* A virtual base: step to the vbtable pointer, then to the entry
             * in the table that says where the base actually landed. */
            p += pdisp;
            p += *(const int32_t *)(*(const uint8_t *const *)p + vdisp);
        }
        return (void *)(uintptr_t)p;
    }
#undef MS_RTTI_AT

    /* A cast that legitimately fails. For a pointer cast that is a null return
     * and the caller's business; for a reference cast MSVC throws std::bad_cast,
     * which cannot be raised from here -- so say so rather than hand back a
     * null the compiler never emitted a check for. */
    if (isReference)
        fprintf(stderr, "[win] dynamic_cast to %s failed on a reference; "
                        "std::bad_cast cannot be thrown from here\n", want);
    return NULL;
}

/* ------------------------------------------------------- the printf family --- */

/* Variadic stubs need the Microsoft convention's own va_list on x86-64: MS
 * varargs sit in different registers and stack slots from System V's, so the
 * host's va_start would read the wrong ones. On i386 the CRT is plain cdecl and
 * the host's macros are correct. */
#ifdef __i386__
#define MSVA_LIST  va_list
#define MSVA_START va_start
#define MSVA_END   va_end
#define MSVA_ARG   va_arg
#else
#define MSVA_LIST  __builtin_ms_va_list
#define MSVA_START __builtin_ms_va_start
#define MSVA_END   __builtin_ms_va_end
#define MSVA_ARG   va_arg   /* generic over the list type */
#endif

/* Formatting has to be done here rather than handed to the host's vsnprintf,
 * because there is no way to turn a Microsoft va_list into a System V one. Each
 * conversion is pulled off the argument list by hand and rendered with a
 * single-conversion snprintf, which keeps the awkward parts -- width, precision,
 * flags, long doubles -- in the host's hands.
 *
 * `wide_s` says what an undecorated %s means: wchar_t in the wide entry points,
 * char in the narrow ones. Getting that backwards prints one character of a
 * UTF-16 string, or walks off the end of a narrow one.
 *
 * Known limit: output is assembled narrow, so a non-ASCII argument to a wide
 * format loses anything outside Latin-1. Every caller reaching this so far
 * formats paths, numbers and version strings. */
static size_t w32_vfmt(char *out, size_t cap, const char *fmt, int wide_s,
                       MSVA_LIST ap)
{
    size_t n = 0;
    char spec[80], tmp[512];

/* The character is evaluated before the bounds test, not inside it: PUTS
 * passes *p_++, and skipping the write on a full buffer would skip the
 * increment with it and spin forever. */
#define PUT(ch) do { char c_ = (char)(ch); \
                     if (n + 1 < cap) out[n] = c_; n++; } while (0)
#define PUTS(s) do { const char *p_ = (s); while (*p_) PUT(*p_++); } while (0)

    while (*fmt) {
        int si = 0, lng = 0, wstr = wide_s, conv;
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;
        if (*fmt == '%') { PUT('%'); fmt++; continue; }
        spec[si++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && si < 8) spec[si++] = *fmt++;
        if (*fmt == '*') { int w = MSVA_ARG(ap, int);
                           si += (size_t)snprintf(spec + si, 16, "%d", w); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9' && si < 24) spec[si++] = *fmt++;
        if (*fmt == '.') {
            spec[si++] = *fmt++;
            if (*fmt == '*') { int p = MSVA_ARG(ap, int);
                               si += (size_t)snprintf(spec + si, 16, "%d", p); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9' && si < 44) spec[si++] = *fmt++;
        }
        for (;;) {                               /* length modifiers */
            if (!strncmp(fmt, "I64", 3))                 { lng = 2; fmt += 3; }
            else if (fmt[0] == 'l' && fmt[1] == 'l')     { lng = 2; fmt += 2; }
            else if (fmt[0] == 'l' || fmt[0] == 'w')     { lng = 1; wstr = 1; fmt++; }
            else if (fmt[0] == 'h')                      { lng = -1; wstr = 0; fmt++; }
            else if (strchr("ztj", fmt[0]) && fmt[0])    { lng = 2; fmt++; }
            else if (fmt[0] == 'L')                      { lng = 3; fmt++; }
            else break;
        }
        conv = *fmt ? *fmt++ : 0;
        spec[si] = 0;

        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            char *e = spec + si;
            if (lng == 2) { *e++ = 'l'; *e++ = 'l'; }
            *e++ = (char)conv; *e = 0;
            if (lng == 2) {
                long long v = MSVA_ARG(ap, long long);
                snprintf(tmp, sizeof tmp, spec, v);
            } else {
                int v = MSVA_ARG(ap, int);
                snprintf(tmp, sizeof tmp, spec, v);
            }
            PUTS(tmp);
            break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        case 'a': case 'A': {
            double v = MSVA_ARG(ap, double);
            spec[si] = (char)conv; spec[si + 1] = 0;
            snprintf(tmp, sizeof tmp, spec, v);
            PUTS(tmp);
            break; }
        case 'c': {
            unsigned ch = (unsigned)MSVA_ARG(ap, int);
            if (wstr) { PUT(ch < 0x100 ? (char)ch : '?'); }
            else      { PUT((char)ch); }
            break; }
        case 'S':                                /* the other width from %s */
            wstr = !wide_s;
            /* fall through */
        case 's': {
            const void *p = MSVA_ARG(ap, void *);
            spec[si] = 's'; spec[si + 1] = 0;
            if (!p) { snprintf(tmp, sizeof tmp, spec, "(null)"); PUTS(tmp); break; }
            if (wstr) {
                const uint16_t *w = p;
                size_t i, len = 0;
                while (w[len]) len++;
                if (len > sizeof tmp - 1) len = sizeof tmp - 1;
                for (i = 0; i < len; i++) tmp[i] = w[i] < 0x100 ? (char)w[i] : '?';
                tmp[len] = 0;
                { char t2[512]; snprintf(t2, sizeof t2, spec, tmp); PUTS(t2); }
            } else {
                snprintf(tmp, sizeof tmp, spec, (const char *)p);
                PUTS(tmp);
            }
            break; }
        case 'p': {
            void *v = MSVA_ARG(ap, void *);
            snprintf(tmp, sizeof tmp, "%p", v);
            PUTS(tmp);
            break; }
        case 0:
            break;
        default:                                 /* unknown: show it verbatim */
            PUT('%'); PUT(conv);
            break;
        }
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
#undef PUT
#undef PUTS
}

static void w32_narrow_to_w(const char *s, uint16_t *out, size_t n)
{
    size_t i = 0;
    if (!out || !n) return;
    for (; s && s[i] && i + 1 < n; i++) out[i] = (uint16_t)(unsigned char)s[i];
    out[i] = 0;
}

/* Narrow entry points. The _s forms differ only in taking the buffer size, which
 * is the size this already honours. */
static MSCRT int st_sprintf(char *b, const char *f, ...)
{ MSVA_LIST a; size_t r; MSVA_START(a, f); r = w32_vfmt(b, (size_t)-1, f, 0, a); MSVA_END(a); return (int)r; }
static MSCRT int st_sprintf_s(char *b, size_t n, const char *f, ...)
{ MSVA_LIST a; size_t r; MSVA_START(a, f); r = w32_vfmt(b, n, f, 0, a); MSVA_END(a); return (int)r; }
static MSCRT int st__snprintf(char *b, size_t n, const char *f, ...)
{ MSVA_LIST a; size_t r; MSVA_START(a, f); r = w32_vfmt(b, n, f, 0, a); MSVA_END(a); return (int)r; }
static MSCRT int st__snprintf_s(char *b, size_t n, size_t cnt, const char *f, ...)
{ MSVA_LIST a; size_t r; (void)cnt; MSVA_START(a, f); r = w32_vfmt(b, n, f, 0, a); MSVA_END(a); return (int)r; }
static MSCRT int st_vsprintf(char *b, const char *f, MSVA_LIST a)
{ return (int)w32_vfmt(b, (size_t)-1, f, 0, a); }
static MSCRT int st__vsnprintf(char *b, size_t n, const char *f, MSVA_LIST a)
{ return (int)w32_vfmt(b, n, f, 0, a); }
static MSCRT int st_vsnprintf_s(char *b, size_t n, size_t cnt, const char *f, MSVA_LIST a)
{ (void)cnt; return (int)w32_vfmt(b, n, f, 0, a); }
static MSCRT int st__vsnprintf_l(char *b, size_t n, const char *f, void *loc, MSVA_LIST a)
{ (void)loc; return (int)w32_vfmt(b, n, f, 0, a); }

/* --------------------------------------------- the rest of the 2015 CRT --- */

/* What a 2015-built plug-in reaches for that nothing here had needed yet. Each
 * one was found by watching which stubs an actual load and render touched, so
 * the list is what the corpus asks for rather than a guess at the API.
 *
 * Windows `long` is 32 bits, which is why strtoul and atol return fixed-width
 * types here rather than the host's long. */
static MSCRT uint32_t st_strtoul(const char *s, char **e, int b)
{ return (uint32_t)strtoul(s, e, b); }
static MSCRT uint64_t st__strtoui64(const char *s, char **e, int b)
{ return (uint64_t)strtoull(s, e, b); }
static MSCRT int64_t st__strtoi64(const char *s, char **e, int b)
{ return (int64_t)strtoll(s, e, b); }
static MSCRT int32_t st_atol(const char *s)   { return (int32_t)atol(s); }
static MSCRT int64_t st__atoi64(const char *s){ return (int64_t)strtoll(s, NULL, 10); }

/* The wide ctype family. ASCII-only, as the narrow ones beside it already are. */
#define W32_ISW(name, expr) \
    static MSCRT int st_##name(uint32_t c) { return (c) < 128 ? (expr) : 0; }
W32_ISW(iswspace,  isspace((int)c))
W32_ISW(iswalpha,  isalpha((int)c))
W32_ISW(iswdigit,  isdigit((int)c))
W32_ISW(iswalnum,  isalnum((int)c))
W32_ISW(iswupper,  isupper((int)c))
W32_ISW(iswlower,  islower((int)c))
W32_ISW(iswpunct,  ispunct((int)c))
W32_ISW(iswxdigit, isxdigit((int)c))
W32_ISW(iswcntrl,  iscntrl((int)c))
W32_ISW(iswprint,  isprint((int)c))
static MSCRT uint32_t st_towlower(uint32_t c)
{ return c < 128 ? (uint32_t)tolower((int)c) : c; }
static MSCRT uint32_t st_towupper(uint32_t c)
{ return c < 128 ? (uint32_t)toupper((int)c) : c; }

/* The bounds-checked string copies. They return an errno_t, 0 for success, and
 * truncate rather than overrun -- which is the whole reason a caller picked the
 * _s form. */
static MSCRT int st_strcpy_s(char *d, size_t n, const char *src)
{
    size_t i = 0;
    if (!d || !n) return 22;                                   /* EINVAL */
    if (!src) { d[0] = 0; return 22; }
    while (src[i] && i + 1 < n) { d[i] = src[i]; i++; }
    d[i] = 0;
    return src[i] ? 34 : 0;                                    /* ERANGE */
}
static MSCRT int st_wcscpy_s(uint16_t *d, size_t n, const uint16_t *src)
{
    size_t i = 0;
    if (!d || !n) return 22;
    if (!src) { d[0] = 0; return 22; }
    while (src[i] && i + 1 < n) { d[i] = src[i]; i++; }
    d[i] = 0;
    return src[i] ? 34 : 0;
}
static MSCRT int st_wcsncpy_s(uint16_t *d, size_t n, const uint16_t *src, size_t cnt)
{
    size_t i = 0;
    if (!d || !n) return 22;
    if (!src) { d[0] = 0; return 22; }
    while (src[i] && i < cnt && i + 1 < n) { d[i] = src[i]; i++; }
    d[i] = 0;
    return 0;
}
static MSCRT int st_wcscat_s(uint16_t *d, size_t n, const uint16_t *src)
{
    size_t i = 0;
    if (!d || !n || !src) return 22;
    while (i < n && d[i]) i++;
    return st_wcscpy_s(d + i, n - i, src);
}

/* _wsplitpath_s and _splitpath_s. Only the pieces a path actually has are
 * written; the rest are emptied, which is what a caller reading only `ext`
 * depends on. */
static MSCRT int st__wsplitpath_s(const uint16_t *path,
                                  uint16_t *drv, size_t dn, uint16_t *dir, size_t dirn,
                                  uint16_t *nam, size_t nn, uint16_t *ext, size_t en)
{
    size_t len = st_wcslen(path), slash = (size_t)-1, dot = (size_t)-1, i;
    if (drv && dn) drv[0] = 0;
    if (dir && dirn) dir[0] = 0;
    if (nam && nn) nam[0] = 0;
    if (ext && en) ext[0] = 0;
    if (!path) return 22;
    for (i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') { slash = i; dot = (size_t)-1; }
        else if (path[i] == '.') dot = i;
    }
    if (dir && dirn && slash != (size_t)-1) {
        size_t n = slash + 1 < dirn ? slash + 1 : dirn - 1;
        for (i = 0; i < n; i++) dir[i] = path[i];
        dir[n] = 0;
    }
    {
        size_t b = (slash == (size_t)-1) ? 0 : slash + 1;
        size_t e = (dot == (size_t)-1 || dot < b) ? len : dot;
        if (nam && nn) {
            size_t n = (e - b) < nn - 1 ? (e - b) : nn - 1;
            for (i = 0; i < n; i++) nam[i] = path[b + i];
            nam[n] = 0;
        }
        if (ext && en && dot != (size_t)-1 && dot >= b) {
            size_t n = (len - dot) < en - 1 ? (len - dot) : en - 1;
            for (i = 0; i < n; i++) ext[i] = path[dot + i];
            ext[n] = 0;
        }
    }
    return 0;
}

/* Floating-point classification. _fdtest reports what a float is, and the
 * caller branches on it -- a stub answering 0 says "zero" about every value. */
static MSCRT int st__fdtest(float *px)
{
    float v = px ? *px : 0.0f;
    if (v != v) return 2;                       /* _NANCODE  */
    if (v > 3.4028234663852886e+38f || v < -3.4028234663852886e+38f) return 1; /* _INFCODE */
    if (v == 0.0f) return 0;
    return -1;                                  /* _FINITE */
}
static MSCRT int st__dtest(double *px)
{
    double v = px ? *px : 0.0;
    if (v != v) return 2;
    if (v > 1.7976931348623157e+308 || v < -1.7976931348623157e+308) return 1;
    if (v == 0.0) return 0;
    return -1;
}

/* The standard streams, and the one printf that writes to them. Only this
 * host's own stubs ever see these FILE pointers, so handing back the real ones
 * is self-consistent -- and it means a plug-in's diagnostics reach the terminal
 * instead of vanishing. */
static MSCRT void *st___acrt_iob_func(uint32_t which)
{ return which == 0 ? (void *)stdin : which == 1 ? (void *)stdout : (void *)stderr; }
static MSCRT int st___stdio_common_vfprintf(uint64_t opt, void *f, const char *fmt,
                                            void *loc, MSVA_LIST a)
{
    char b[2048];
    size_t n;
    (void)opt; (void)loc;
    n = w32_vfmt(b, sizeof b, fmt, 0, a);
    fwrite(b, 1, n, f ? (FILE *)f : stderr);
    return (int)n;
}

/* atexit bookkeeping. Reporting success without recording anything means a
 * module's static destructors do not run at exit -- which costs nothing in a
 * host that unloads plug-ins by dropping the mapping, and beats failing the
 * registration, which the CRT treats as a fatal startup error. */
static MSCRT int st__initialize_onexit_table(void *t)   { (void)t; return 0; }
static MSCRT int st__register_onexit_function(void *t, void *f)
{ (void)t; (void)f; return 0; }
static MSCRT int st__execute_onexit_table(void *t)      { (void)t; return 0; }
static MSCRT int st___uncaught_exceptions(void)         { return 0; }
/* The CRT brackets a locale read with these. Nothing here changes the locale
 * under a plug-in, so the lock has nothing to protect -- but it must return
 * rather than fall to a stub, because the unlock is what the caller balances. */
static MSCRT void st__lock_locales(void)   { }
static MSCRT void st__unlock_locales(void) { }
static MS int32_t st___vcrt_InitializeCriticalSectionEx(void *cs, uint32_t spin, uint32_t fl)
{ (void)fl; return st_InitializeCriticalSectionAndSpinCount(cs, spin); }

/* The locale's day and month names, in the CRT's own packed form: a separator
 * character, then abbreviated and full names alternating. <locale>'s time facet
 * indexes straight into this, so a null would fault rather than degrade.
 *
 * The buffer is malloc'd because the caller frees it -- that is the contract,
 * not a detail. Returning a literal here cost a "free(): invalid pointer" abort
 * the moment a locale was destroyed, which is a worse failure than the missing
 * stub had been. The plug-in's free is this host's free, so one strdup matches
 * the other side exactly. */
static MSCRT char *st__Getdays(void)
{ return w32_strdup_guest(":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday"
                          ":Thu:Thursday:Fri:Friday:Sat:Saturday"); }
static MSCRT char *st__Getmonths(void)
{ return w32_strdup_guest(":Jan:January:Feb:February:Mar:March:Apr:April:May:May"
                          ":Jun:June:Jul:July:Aug:August:Sep:September:Oct:October"
                          ":Nov:November:Dec:December"); }

/* ------------------------------------------------------------ the scanf family --- */

/* sscanf, written out rather than delegated.
 *
 * The host's own vsscanf cannot be handed this argument list: on x86-64 the
 * guest passes Microsoft's va_list and the host's expects System V's, so every
 * pointer would be read from the wrong register or stack slot. That is the same
 * reason w32_vfmt exists instead of a call to vsnprintf.
 *
 * It was worth writing because a stub is silently destructive here. A plug-in
 * parsing its own patch data reads integers with this; a stub assigns nothing
 * and returns 0, so every value it believed it had parsed keeps whatever was in
 * the variable, and the failure surfaces much later as a lookup that finds
 * nothing. That is exactly how it turned up -- a SynthEdit VST3 read its module
 * graph out of XML, every module id came back unset, and the registry lookup
 * returned null to a call site with no null check.
 *
 * Windows `long` is 32 bits at both widths while the host's is 64 at one of
 * them, so %ld must store four bytes here and not eight. Storing eight would
 * scribble on whatever the guest put next to the variable -- silent, and not
 * obviously this function's fault afterwards. */

#define W32_SCANBUF 512

/* One conversion's worth of input, copied out so the host's parsers can have a
 * NUL-terminated string with the field width already applied. */
static size_t w32_scan_span(const char *s, size_t width, const char *accept,
                            char *buf, size_t cap)
{
    size_t n = 0;
    while (s[n] && n < width && n + 1 < cap && strchr(accept, s[n]) != NULL) n++;
    memcpy(buf, s, n);
    buf[n] = 0;
    return n;
}

static void w32_scan_put_int(MSVA_LIST *ap, int lmod, long long v)
{
    void *q = MSVA_ARG(*ap, void *);
    if (!q) return;
    switch (lmod) {
    case 2:  *(int8_t  *)q = (int8_t)v;  break;   /* hh */
    case 1:  *(int16_t *)q = (int16_t)v; break;   /* h  */
    case 4:  *(int64_t *)q = v;          break;   /* ll, I64, j, z, t */
    /* l, and the default. Windows long is 32-bit, so both store four bytes. */
    default: *(int32_t *)q = (int32_t)v; break;
    }
}

static void w32_scan_put_dbl(MSVA_LIST *ap, int lmod, double v)
{
    void *q = MSVA_ARG(*ap, void *);
    if (!q) return;
    if (lmod >= 3) *(double *)q = v;              /* l, L: double */
    else           *(float  *)q = (float)v;       /* bare %f is a float* */
}

/* Write a scanned string out at the caller's character width. */
static void w32_scan_put_str(MSVA_LIST *ap, int wide, const char *v, size_t n,
                             int terminate)
{
    void *q = MSVA_ARG(*ap, void *);
    size_t i;
    if (!q) return;
    if (wide) {
        uint16_t *w = (uint16_t *)q;
        for (i = 0; i < n; i++) w[i] = (uint8_t)v[i];
        if (terminate) w[n] = 0;
    } else {
        memcpy(q, v, n);
        if (terminate) ((char *)q)[n] = 0;
    }
}

static const char W32_DIGITS[]  = "0123456789";
static const char W32_HEXDIG[]  = "0123456789abcdefABCDEF";

static int w32_scan_core(const char *in, const char *fmt, int wide_str, MSVA_LIST ap)
{
    const char *s = in, *p = fmt;
    char buf[W32_SCANBUF];
    int assigned = 0;

    while (*p) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*s)) s++;
            p++;
            continue;
        }
        if (*p != '%') {
            if (*s != *p) return assigned;
            s++; p++;
            continue;
        }
        p++;
        if (*p == '%') {
            while (isspace((unsigned char)*s)) s++;
            if (*s != '%') return assigned;
            s++; p++;
            continue;
        }
        {
            int suppress = 0, lmod = 0, base = 10;
            size_t width = (size_t)-1, n;
            char conv;

            if (*p == '*') { suppress = 1; p++; }
            if (isdigit((unsigned char)*p)) {
                width = 0;
                while (isdigit((unsigned char)*p)) width = width * 10 + (size_t)(*p++ - '0');
            }
            if (*p == 'h')      { p++; lmod = 1; if (*p == 'h') { p++; lmod = 2; } }
            else if (*p == 'l') { p++; lmod = 3; if (*p == 'l') { p++; lmod = 4; } }
            else if (*p == 'L') { p++; lmod = 3; }
            else if (*p == 'j' || *p == 'z' || *p == 't') { p++; lmod = 4; }
            else if (*p == 'I') {                          /* MSVC's I64 / I32 */
                if (p[1] == '6' && p[2] == '4')      { p += 3; lmod = 4; }
                else if (p[1] == '3' && p[2] == '2') { p += 3; lmod = 0; }
                else                                 { p += 1; lmod = 4; }
            }
            conv = *p++;
            if (!conv) break;

            /* Every conversion but these three skips leading whitespace. */
            if (conv != 'c' && conv != 'n' && conv != '[')
                while (isspace((unsigned char)*s)) s++;

            switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
                const char *acc = W32_DIGITS;
                char *end = NULL;
                long long v;
                size_t off = 0;
                if (conv == 'o') { acc = "01234567"; base = 8; }
                else if (conv == 'x' || conv == 'X' || conv == 'p') { acc = W32_HEXDIG; base = 16; }
                else if (conv == 'i') { acc = W32_HEXDIG; base = 0; }
                /* A sign is part of the field, and not in the accept set. */
                if ((*s == '-' || *s == '+') && width) { buf[0] = *s++; off = 1; }
                if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    if (off + 2 < sizeof buf) { buf[off] = s[0]; buf[off+1] = s[1]; }
                    s += 2; off += 2;
                }
                n = w32_scan_span(s, width == (size_t)-1 ? width : width - off,
                                  acc, buf + off, sizeof buf - off);
                if (!n && !off) return assigned;
                s += n;
                v = (long long)strtoull(buf, &end, base);
                if (!suppress) { w32_scan_put_int(&ap, lmod, v); assigned++; }
                break;
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': {
                char *end = NULL;
                double v;
                n = w32_scan_span(s, width, "0123456789+-.eEpxXaAbBcCdDfF",
                                  buf, sizeof buf);
                if (!n) return assigned;
                v = strtod(buf, &end);
                if (end == buf) return assigned;
                s += (size_t)(end - buf);        /* only what strtod consumed */
                if (!suppress) { w32_scan_put_dbl(&ap, lmod, v); assigned++; }
                break;
            }
            case 's': {
                const char *b = s;
                n = 0;
                while (s[n] && !isspace((unsigned char)s[n]) && n < width) n++;
                if (!n) return assigned;
                s += n;
                /* %s in a wide scan writes wchar_t unless %hs said otherwise. */
                if (!suppress) {
                    w32_scan_put_str(&ap, wide_str ? (lmod != 1) : (lmod >= 3), b, n, 1);
                    assigned++;
                }
                break;
            }
            case 'c': {
                const char *b = s;
                size_t want = (width == (size_t)-1) ? 1 : width;
                for (n = 0; n < want && s[n]; n++) { }
                if (n < want) return assigned;
                s += n;
                if (!suppress) {
                    w32_scan_put_str(&ap, wide_str ? (lmod != 1) : (lmod >= 3), b, n, 0);
                    assigned++;
                }
                break;
            }
            case '[': {
                char set[128];
                size_t sn = 0;
                int negate = 0;
                const char *b;
                if (*p == '^') { negate = 1; p++; }
                if (*p == ']') { set[sn++] = *p++; }
                while (*p && *p != ']' && sn + 1 < sizeof set) set[sn++] = *p++;
                set[sn] = 0;
                if (*p == ']') p++;
                b = s; n = 0;
                while (s[n] && n < width) {
                    int in_set = strchr(set, s[n]) != NULL;
                    if (negate ? in_set : !in_set) break;
                    n++;
                }
                if (!n) return assigned;
                s += n;
                if (!suppress) {
                    w32_scan_put_str(&ap, wide_str ? (lmod != 1) : (lmod >= 3), b, n, 1);
                    assigned++;
                }
                break;
            }
            case 'n':
                if (!suppress) w32_scan_put_int(&ap, lmod, (long long)(s - in));
                break;               /* %n is not an assignment for the count */
            default:
                return assigned;
            }
        }
    }
    return assigned;
}

/* The input may be handed over with an explicit length rather than a
 * terminator, so bound it before the scanner sees it. */
static int w32_vsscanf(const char *in, size_t len, const char *fmt, MSVA_LIST ap)
{
    char tmp[4096];
    if (!in || !fmt) return -1;
    if (len != (size_t)-1 && len < sizeof tmp && strnlen(in, len) == len) {
        memcpy(tmp, in, len);
        tmp[len] = 0;
        in = tmp;
    }
    return w32_scan_core(in, fmt, 0, ap);
}

/* Wide input and format, narrowed the same way w32_vfmtw narrows its output --
 * everything this host handles is Latin-1 already. String results still go back
 * out as wchar_t, which is what wide_str selects. */
static int w32_vswscanf(const uint16_t *in, const uint16_t *fmt, MSVA_LIST ap)
{
    char ni[4096], nf[1024];
    size_t i;
    if (!in || !fmt) return -1;
    for (i = 0; i + 1 < sizeof ni && in[i]; i++)  ni[i] = (char)(in[i] < 0x100 ? in[i] : '?');
    ni[i] = 0;
    for (i = 0; i + 1 < sizeof nf && fmt[i]; i++) nf[i] = (char)(fmt[i] < 0x100 ? fmt[i] : '?');
    nf[i] = 0;
    return w32_scan_core(ni, nf, 1, ap);
}

static MSCRT int st_sscanf(const char *b, const char *f, ...)
{ MSVA_LIST a; int r; MSVA_START(a, f); r = w32_vsscanf(b, (size_t)-1, f, a); MSVA_END(a); return r; }
static MSCRT int st_sscanf_s(const char *b, const char *f, ...)
{ MSVA_LIST a; int r; MSVA_START(a, f); r = w32_vsscanf(b, (size_t)-1, f, a); MSVA_END(a); return r; }
static MSCRT int st_swscanf(const uint16_t *b, const uint16_t *f, ...)
{ MSVA_LIST a; int r; MSVA_START(a, f); r = w32_vswscanf(b, f, a); MSVA_END(a); return r; }
static MSCRT int st_swscanf_s(const uint16_t *b, const uint16_t *f, ...)
{ MSVA_LIST a; int r; MSVA_START(a, f); r = w32_vswscanf(b, f, a); MSVA_END(a); return r; }
static MSCRT int st_vsscanf(const char *b, const char *f, MSVA_LIST a)
{ return w32_vsscanf(b, (size_t)-1, f, a); }

/* The 2015 CRT funnels every scanf and sprintf through one of these, with the
 * options word carrying what used to be the difference between the _s and
 * plain forms. The buffer size is honoured either way here, so the options are
 * not consulted. */
static MSCRT int st___stdio_common_vsscanf(uint64_t opt, const char *b, size_t n,
                                           const char *f, void *loc, MSVA_LIST a)
{ (void)opt; (void)loc; return w32_vsscanf(b, n, f, a); }
static MSCRT int st___stdio_common_vswscanf(uint64_t opt, const uint16_t *b, size_t n,
                                            const uint16_t *f, void *loc, MSVA_LIST a)
{ (void)opt; (void)n; (void)loc; return w32_vswscanf(b, f, a); }
static MSCRT int st___stdio_common_vsprintf(uint64_t opt, char *b, size_t n,
                                            const char *f, void *loc, MSVA_LIST a)
{ (void)opt; (void)loc; return (int)w32_vfmt(b, b ? n : 0, f, 0, a); }
/* _vsnprintf_s carries a count *as well as* the buffer size, so it takes one
 * more argument than the call above. Sharing an implementation between them
 * shifts every argument past the buffer -- the format pointer arrives holding a
 * length, and the first %s dereferences it. */
static MSCRT int st___stdio_common_vsnprintf_s(uint64_t opt, char *b, size_t n,
                                               size_t cnt, const char *f,
                                               void *loc, MSVA_LIST a)
{ (void)opt; (void)cnt; (void)loc; return (int)w32_vfmt(b, b ? n : 0, f, 0, a); }

/* Wide entry points: format narrow, then widen. */
static size_t w32_vfmtw(uint16_t *b, size_t n, const uint16_t *wf, MSVA_LIST a)
{
    char nf[1024], nb[2048];
    size_t i = 0, r;
    for (; wf && wf[i] && i + 1 < sizeof nf; i++)
        nf[i] = wf[i] < 0x100 ? (char)wf[i] : '?';
    nf[i] = 0;
    r = w32_vfmt(nb, sizeof nb, nf, 1, a);
    w32_narrow_to_w(nb, b, n);
    return r;
}

static MSCRT int st_swprintf_s(uint16_t *b, size_t n, const uint16_t *f, ...)
{ MSVA_LIST a; size_t r; MSVA_START(a, f); r = w32_vfmtw(b, n, f, a); MSVA_END(a); return (int)r; }
static MSCRT int st__snwprintf_s(uint16_t *b, size_t n, size_t c, const uint16_t *f, ...)
{ MSVA_LIST a; size_t r; (void)c; MSVA_START(a, f); r = w32_vfmtw(b, n, f, a); MSVA_END(a); return (int)r; }
static MSCRT int st__snwprintf(uint16_t *b, size_t n, const uint16_t *f, ...)
{ MSVA_LIST a; size_t r; MSVA_START(a, f); r = w32_vfmtw(b, n, f, a); MSVA_END(a); return (int)r; }
static MSCRT int st__vsnwprintf(uint16_t *b, size_t n, const uint16_t *f, MSVA_LIST a)
{ return (int)w32_vfmtw(b, n, f, a); }
static MSCRT int st__vswprintf_c_l(uint16_t *b, size_t n, const uint16_t *f,
                                   void *loc, MSVA_LIST a)
{ (void)loc; return (int)w32_vfmtw(b, n, f, a); }
static MSCRT int st__vswprintf_c(uint16_t *b, size_t n, const uint16_t *f, MSVA_LIST a)
{ return (int)w32_vfmtw(b, n, f, a); }
static MSCRT int st__vsnwprintf_l(uint16_t *b, size_t n, const uint16_t *f,
                                  void *loc, MSVA_LIST a)
{ (void)loc; return (int)w32_vfmtw(b, n, f, a); }
/* The 2015 CRT's wide sprintf core, the partner of __stdio_common_vsprintf. */
static MSCRT int st___stdio_common_vswprintf(uint64_t opt, uint16_t *b, size_t n,
                                             const uint16_t *f, void *loc, MSVA_LIST a)
{ (void)opt; (void)loc; return (int)w32_vfmtw(b, b ? n : 0, f, a); }

/* wsprintf and wvsprintf, user32's own formatters. The two halves do not share
 * a calling convention, which is the whole reason they are spelled differently
 * here -- and getting that wrong is silent at x86-64 and fatal at i386.
 *
 * wsprintfA/W are WINAPIV: the one cdecl pair in an otherwise stdcall DLL,
 * because a stdcall callee cannot pop an argument list it has no way to count.
 * A cdecl export carries no @N, so gen_arity.py finds nothing to record and
 * win32_arity.h has no entry for either -- which is why the loader says
 * "unknown stdcall arity, assuming 0" when a plugin imports one. Assuming 0
 * happens to be right for cdecl, since the caller cleans up, so the stack
 * survives; what does not survive is the call returning 0 with nothing
 * written, leaving the caller to use a buffer it believes was filled.
 *
 * wvsprintfA/W take the whole argument list as one va_list parameter, so there
 * is nothing variadic left to count and they are ordinary WINAPI stdcall.
 * win32_arity.h has them at 12 bytes -- three arguments -- and that is the
 * authority to follow: a cdecl stub here returns without popping them, so at
 * i386 the plugin's ESP is left 12 bytes low on every call and its stack is
 * wrong from that point on. MS, not MSCRT, for these two.
 *
 * The 1024 cap is the documented contract, not a shortcut: Windows refuses to
 * write more than 1024 bytes (or wide characters) here regardless of how big
 * the caller's buffer is, and callers size their buffers knowing that. */
#define WSPRINTF_MAX 1024
static MSCRT int st_wsprintfA(char *b, const char *f, ...)
{ MSVA_LIST a; size_t r; MSVA_START(a, f); r = w32_vfmt(b, WSPRINTF_MAX, f, 0, a); MSVA_END(a); return (int)r; }
static MSCRT int st_wsprintfW(uint16_t *b, const uint16_t *f, ...)
{ MSVA_LIST a; size_t r; MSVA_START(a, f); r = w32_vfmtw(b, WSPRINTF_MAX, f, a); MSVA_END(a); return (int)r; }
static MS int st_wvsprintfA(char *b, const char *f, MSVA_LIST a)
{ return (int)w32_vfmt(b, WSPRINTF_MAX, f, 0, a); }
static MS int st_wvsprintfW(uint16_t *b, const uint16_t *f, MSVA_LIST a)
{ return (int)w32_vfmtw(b, WSPRINTF_MAX, f, a); }

/* ------------------------------------------------- odds and ends reached ---- */

static MS int32_t st_DuplicateHandle(void *sp, void *sh, void *tp, void **th,
                                     uint32_t acc, int32_t inherit, uint32_t opts)
{
    (void)sp; (void)tp; (void)acc; (void)inherit; (void)opts;
    /* Handles here are indices into one table, so a duplicate is the same value.
     * That makes CloseHandle on either one close the object, which differs from
     * Windows -- but a plugin that duplicates a handle and closes one copy would
     * otherwise be left holding a handle this side had already released. */
    if (th) *th = sh;
    return 1;
}

static MS void *st_LoadIconW(void *inst, const void *name)
{ (void)inst; (void)name; return (void *)0x49434F4E; /* 'ICON' */ }
static MS void *st_LoadIconA(void *inst, const void *name)
{ return st_LoadIconW(inst, name); }
static MS void *st_LoadCursorW(void *inst, const void *name)
{ (void)name; return st_LoadCursorA(inst, NULL); }

/* No keyboard layout is attached, so nothing translates. Reporting zero is the
 * documented "no mapping" answer rather than an invented key code. */
static MS uint32_t st_MapVirtualKeyW(uint32_t code, uint32_t type)
{ (void)type; return code >= 'A' && code <= 'Z' ? code : 0; }
static MS uint32_t st_MapVirtualKeyA(uint32_t code, uint32_t type)
{ return st_MapVirtualKeyW(code, type); }
static MS int32_t st_ToUnicode(uint32_t vk, uint32_t sc, const uint8_t *ks,
                               uint16_t *buf, int32_t n, uint32_t f)
{ (void)vk;(void)sc;(void)ks;(void)buf;(void)n;(void)f; return 0; }

/* A process token, enough to be opened and closed. Nothing here consults it for
 * a privilege decision. */
static MS int32_t st_OpenProcessToken(void *proc, uint32_t access, void **tok)
{ (void)proc; (void)access; if (tok) *tok = (void *)0x544F4B4E; return 1; }

/* --------------------------------------------------------------- errno ----- */

/* _errno returns a *pointer* to the caller's errno, which the caller then reads
 * and writes through. A stub returning whatever was in the return register hands
 * over an arbitrary address to be written to, so this is one of the cases where
 * not implementing it is worse than failing outright. Per-thread, as the CRT
 * documents it. */
static __thread int g_crt_errno;
static __thread uint32_t g_crt_doserrno;

static MSCRT int *st__errno(void) { return &g_crt_errno; }
static MSCRT uint32_t *st___doserrno(void) { return &g_crt_doserrno; }
static MSCRT int st__get_errno(int *out) { if (out) *out = g_crt_errno; return 0; }
static MSCRT int st__set_errno(int v) { g_crt_errno = v; return 0; }
static MSCRT int st__get_doserrno(uint32_t *out)
{ if (out) *out = g_crt_doserrno; return 0; }
static MSCRT int st__set_doserrno(uint32_t v) { g_crt_doserrno = v; return 0; }

/* ------------------------------------------------------ std::exception ----- */

/* MSVC 2013's std::exception is { vftable, const char *what, bool dofree }, and
 * its constructors are imported from the C runtime rather than inlined. A
 * constructor that does nothing leaves the vftable pointer holding whatever was
 * in the memory, and the first virtual call -- what(), or the destructor on the
 * way out of a scope -- jumps through it.
 *
 * The vftable has to be a real one for that reason: two slots, the deleting
 * destructor and what(), in the order the compiler emitted the calls against. */
typedef struct { void **vft; const char *what; int dofree; } cxx_exception;

static MSTHIS const char *st_cxx_exc_what(cxx_exception *e)
{ return e && e->what ? e->what : "Unknown exception"; }
static MSTHIS void *st_cxx_exc_del(cxx_exception *e, unsigned flags)
{
    (void)flags;
    if (e && e->dofree && e->what) { free((void *)e->what); e->what = NULL; }
    return e;
}
static void *g_cxx_exc_vft[2] = { (void *)st_cxx_exc_del, (void *)st_cxx_exc_what };

static MSTHIS cxx_exception *st_cxx_exc_ctor(cxx_exception *e)
{ if (e) { e->vft = g_cxx_exc_vft; e->what = NULL; e->dofree = 0; } return e; }

/* exception(char const * const &) -- the argument is a *reference* to a pointer,
 * so it arrives as a pointer to the string pointer, not the string. */
static MSTHIS cxx_exception *st_cxx_exc_ctor_s(cxx_exception *e, const char *const *msg)
{
    if (!e) return e;
    e->vft = g_cxx_exc_vft;
    e->what = msg ? *msg : NULL;
    e->dofree = 0;                        /* the caller's literal, not ours */
    return e;
}
static MSTHIS cxx_exception *st_cxx_exc_ctor_si(cxx_exception *e, const char *const *msg, int noalloc)
{ (void)noalloc; return st_cxx_exc_ctor_s(e, msg); }
static MSTHIS cxx_exception *st_cxx_exc_copy(cxx_exception *e, const cxx_exception *o)
{
    if (!e) return e;
    e->vft = g_cxx_exc_vft;
    e->what = o ? o->what : NULL;
    e->dofree = 0;                        /* share, so neither frees the other's */
    return e;
}
static MSTHIS void st_cxx_exc_dtor(cxx_exception *e)
{ if (e && e->dofree && e->what) { free((void *)e->what); e->what = NULL; } }

/* ------------------------------------------------------- process token ----- */

enum { W_TokenUser = 1, W_TokenElevation = 20, W_TokenIntegrityLevel = 25 };

static MS int32_t st_GetTokenInformation(void *tok, uint32_t cls, void *buf,
                                        uint32_t len, uint32_t *ret)
{
    uint32_t need = 4;
    (void)tok;
    /* Only the sizes and shapes actually asked for. Anything else reports the
     * documented failure rather than leaving the caller's buffer untouched and
     * claiming success -- which would have it read uninitialised memory. */
    switch (cls) {
    case W_TokenElevation:                     /* TOKEN_ELEVATION { DWORD } */
        need = 4;
        if (ret) *ret = need;
        if (!buf || len < need) { g_last_error = 122; return 0; }
        *(uint32_t *)buf = 0;                  /* not elevated */
        return 1;
    default:
        if (ret) *ret = 0;
        g_last_error = 87;                     /* ERROR_INVALID_PARAMETER */
        return 0;
    }
}

/* ------------------------------------------------------ registry, wide ----- */

/* There is no Windows registry here, and inventing one would mean inventing its
 * contents. Reads of keys nobody wrote report ERROR_FILE_NOT_FOUND, which is the
 * truth: the key does not exist on this machine. What is worth supporting is the
 * round trip -- a plugin that stores a setting and reads it back in the same
 * session gets its own value, not a lie about someone else's. Nothing here
 * fabricates installation or licence state. */
#define W32_REG_MAX 64
typedef struct {
    int used;
    char path[256], name[128];
    uint32_t type;
    unsigned char data[512];
    uint32_t len;
} w32_regval;
static w32_regval g_reg[W32_REG_MAX];
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

/* Open keys are the hash of their path, so a handle carries which key it is
 * without a table: nothing here needs per-open state. */
static void *reg_handle(const char *path, char *out, size_t n)
{
    uint32_t h = 2166136261u;
    const char *p = path ? path : "";
    for (; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    if (out) snprintf(out, n, "%s", path ? path : "");
    return (void *)(uintptr_t)(0x52470000u | (h & 0xFFFF));
}

/* A path for a handle, recovered by re-hashing what we stored. Subkeys are
 * appended so nesting works without a tree. */
static char g_reg_open[W32_REG_MAX][256];
static int reg_remember(const char *path)
{
    int i;
    for (i = 0; i < W32_REG_MAX; i++)
        if (!strcmp(g_reg_open[i], path)) return i;
    for (i = 0; i < W32_REG_MAX; i++)
        if (!g_reg_open[i][0]) { snprintf(g_reg_open[i], 256, "%s", path); return i; }
    return -1;
}
static const char *reg_path_of(void *k)
{
    uintptr_t v = (uintptr_t)k;
    if (v >= 0x80000000u) return "HKEY";          /* a predefined root */
    if ((v & 0xFFFF0000u) != 0x52470000u) return NULL;
    { int i = (int)(v & 0xFFFF);
      return (i >= 0 && i < W32_REG_MAX && g_reg_open[i][0]) ? g_reg_open[i] : NULL; }
}
static void *reg_open_path(const char *path)
{
    int i;
    pthread_mutex_lock(&g_reg_lock);
    i = reg_remember(path);
    pthread_mutex_unlock(&g_reg_lock);
    if (i < 0) return NULL;
    return (void *)(uintptr_t)(0x52470000u | (unsigned)i);
}
static void reg_join(char *out, size_t n, void *parent, const char *sub)
{
    const char *pp = reg_path_of(parent);
    snprintf(out, n, "%s\\%s", pp ? pp : "HKEY", sub ? sub : "");
}

static MS int32_t st_RegCreateKeyExA(void *k, const char *sub, uint32_t res,
        const char *cls, uint32_t opt, uint32_t acc, void *sa, void **out,
        uint32_t *disp)
{
    char path[256];
    (void)res; (void)cls; (void)opt; (void)acc; (void)sa;
    reg_join(path, sizeof path, k, sub);
    if (out) *out = reg_open_path(path);
    if (disp) *disp = 1;                          /* REG_CREATED_NEW_KEY */
    return (out && !*out) ? 8 /* NOT_ENOUGH_MEMORY */ : 0;
}
static MS int32_t st_RegCreateKeyExW(void *k, const uint16_t *sub, uint32_t res,
        const uint16_t *cls, uint32_t opt, uint32_t acc, void *sa, void **out,
        uint32_t *disp)
{
    char s[192];
    (void)cls;
    w2c(sub, s, sizeof s);
    return st_RegCreateKeyExA(k, s, res, NULL, opt, acc, sa, out, disp);
}

static MS int32_t st_RegSetValueExA(void *k, const char *name, uint32_t res,
        uint32_t type, const void *data, uint32_t len)
{
    const char *path = reg_path_of(k);
    int i, slot = -1;
    (void)res;
    if (!path) return 6 /* ERROR_INVALID_HANDLE */;
    if (len > sizeof g_reg[0].data) return 87;
    pthread_mutex_lock(&g_reg_lock);
    for (i = 0; i < W32_REG_MAX; i++) {
        if (g_reg[i].used && !strcmp(g_reg[i].path, path)
            && !strcmp(g_reg[i].name, name ? name : "")) { slot = i; break; }
        if (!g_reg[i].used && slot < 0) slot = i;
    }
    if (slot < 0) { pthread_mutex_unlock(&g_reg_lock); return 8; }
    g_reg[slot].used = 1;
    snprintf(g_reg[slot].path, sizeof g_reg[slot].path, "%s", path);
    snprintf(g_reg[slot].name, sizeof g_reg[slot].name, "%s", name ? name : "");
    g_reg[slot].type = type;
    g_reg[slot].len = len;
    if (data && len) memcpy(g_reg[slot].data, data, len);
    pthread_mutex_unlock(&g_reg_lock);
    return 0;
}
static MS int32_t st_RegSetValueExW(void *k, const uint16_t *name, uint32_t res,
        uint32_t type, const void *data, uint32_t len)
{ char n[128]; w2c(name, n, sizeof n); return st_RegSetValueExA(k, n, res, type, data, len); }

static MS int32_t st_RegOpenKeyExW(void *k, const uint16_t *sub, uint32_t opt,
                                   uint32_t acc, void **out)
{
    char s[192], path[256];
    int i, found = 0;
    (void)opt; (void)acc;
    w2c(sub, s, sizeof s);
    reg_join(path, sizeof path, k, s);
    /* An open only succeeds if something was actually written under it. */
    pthread_mutex_lock(&g_reg_lock);
    for (i = 0; i < W32_REG_MAX; i++)
        if (g_reg[i].used && !strncmp(g_reg[i].path, path, strlen(path))) { found = 1; break; }
    pthread_mutex_unlock(&g_reg_lock);
    if (!found) { if (out) *out = NULL; return 2; }
    if (out) *out = reg_open_path(path);
    return 0;
}

static MS int32_t st_RegQueryValueExW(void *k, const uint16_t *name, uint32_t *res,
        uint32_t *type, void *data, uint32_t *len)
{
    char n[128];
    const char *path = reg_path_of(k);
    int i;
    (void)res;
    w2c(name, n, sizeof n);
    if (!path) return 6;
    pthread_mutex_lock(&g_reg_lock);
    for (i = 0; i < W32_REG_MAX; i++) {
        if (!g_reg[i].used || strcmp(g_reg[i].path, path) || strcmp(g_reg[i].name, n))
            continue;
        if (type) *type = g_reg[i].type;
        if (!data) { if (len) *len = g_reg[i].len; pthread_mutex_unlock(&g_reg_lock); return 0; }
        if (!len || *len < g_reg[i].len) {
            if (len) *len = g_reg[i].len;
            pthread_mutex_unlock(&g_reg_lock);
            return 234 /* ERROR_MORE_DATA */;
        }
        memcpy(data, g_reg[i].data, g_reg[i].len);
        *len = g_reg[i].len;
        pthread_mutex_unlock(&g_reg_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_reg_lock);
    if (len) *len = 0;
    return 2;
}
static MS int32_t st_RegDeleteValueW(void *k, const uint16_t *name)
{
    char n[128];
    const char *path = reg_path_of(k);
    int i;
    w2c(name, n, sizeof n);
    if (!path) return 6;
    pthread_mutex_lock(&g_reg_lock);
    for (i = 0; i < W32_REG_MAX; i++)
        if (g_reg[i].used && !strcmp(g_reg[i].path, path) && !strcmp(g_reg[i].name, n))
            g_reg[i].used = 0;
    pthread_mutex_unlock(&g_reg_lock);
    return 0;
}
static MS int32_t st_RegDeleteKeyW(void *k, const uint16_t *sub)
{ (void)k; (void)sub; return 0; }
static MS int32_t st_RegEnumKeyExW(void *k, uint32_t idx, uint16_t *name, uint32_t *nlen,
        uint32_t *res, uint16_t *cls, uint32_t *clen, void *ft)
{ (void)k;(void)idx;(void)name;(void)res;(void)cls;(void)ft;
  if (nlen) *nlen = 0; if (clen) *clen = 0;
  return 259 /* ERROR_NO_MORE_ITEMS */; }
static MS int32_t st_RegEnumValueW(void *k, uint32_t idx, uint16_t *name, uint32_t *nlen,
        uint32_t *res, uint32_t *type, void *data, uint32_t *dlen)
{ (void)k;(void)idx;(void)name;(void)res;(void)type;(void)data;
  if (nlen) *nlen = 0; if (dlen) *dlen = 0;
  return 259; }
static MS int32_t st_RegQueryInfoKeyW(void *k, uint16_t *cls, uint32_t *clen,
        uint32_t *res, uint32_t *nkeys, uint32_t *maxk, uint32_t *maxkc,
        uint32_t *nvals, uint32_t *maxv, uint32_t *maxvd, uint32_t *sec, void *ft)
{
    (void)k; (void)cls; (void)res; (void)sec; (void)ft;
    if (clen) *clen = 0;
    if (nkeys) *nkeys = 0; if (maxk) *maxk = 0; if (maxkc) *maxkc = 0;
    if (nvals) *nvals = 0; if (maxv) *maxv = 0; if (maxvd) *maxvd = 0;
    return 0;
}
static MS int32_t st_RegFlushKey(void *k) { (void)k; return 0; }

/* ---------------------------------------------------------- shell paths ---- */

/* Real directories, created on demand. Handing back /tmp for the place a plugin
 * keeps its settings means those settings do not survive a reboot -- and a user
 * who moves a licence file into the folder the plugin asked for should find it
 * still there. */
static const char *shell_folder(int32_t csidl)
{
    static char buf[512];
    const char *home = getenv("HOME");
    const char *leaf;
    if (!home || !*home) home = "/tmp";
    switch (csidl & 0xFF) {
    case 0x1A: leaf = "AppData/Roaming";  break;   /* CSIDL_APPDATA */
    case 0x1C: leaf = "AppData/Local";    break;   /* CSIDL_LOCAL_APPDATA */
    case 0x23: leaf = "ProgramData";      break;   /* CSIDL_COMMON_APPDATA */
    case 0x05: leaf = "Documents";        break;   /* CSIDL_PERSONAL */
    case 0x0D: leaf = "Music";            break;   /* CSIDL_MYMUSIC */
    case 0x26: case 0x2A: leaf = "Program Files"; break;
    case 0x00: case 0x10: leaf = "Desktop"; break;
    default:   leaf = "AppData/Roaming";  break;
    }
    snprintf(buf, sizeof buf, "%s/.peload/%s", home, leaf);
    {   /* mkdir -p, component by component */
        char t[512]; size_t i;
        snprintf(t, sizeof t, "%s", buf);
        for (i = 1; t[i]; i++)
            if (t[i] == '/') { t[i] = 0; mkdir(t, 0755); t[i] = '/'; }
        mkdir(t, 0755);
    }
    return buf;
}
static MS int32_t st_SHGetFolderPathW(void *hwnd, int32_t csidl, void *tok,
                                      uint32_t f, uint16_t *out)
{
    const char *p;
    size_t i;
    (void)hwnd; (void)tok; (void)f;
    if (!out) return 0x80070057;                   /* E_INVALIDARG */
    p = shell_folder(csidl);
    for (i = 0; p[i] && i < 259; i++) out[i] = (uint16_t)(unsigned char)p[i];
    out[i] = 0;
    return 0;
}

/* -------------------------------------------------------- FormatMessage ---- */

enum { W_FM_ALLOCATE_BUFFER = 0x0100, W_FM_FROM_SYSTEM = 0x1000 };

static MS uint32_t st_FormatMessageA(uint32_t flags, void *src, uint32_t id,
        uint32_t lang, char *buf, uint32_t size, void *args)
{
    char msg[256];
    uint32_t n;
    (void)src; (void)lang; (void)args;
    snprintf(msg, sizeof msg, "Error %u", id);
    n = (uint32_t)strlen(msg);
    if (flags & W_FM_ALLOCATE_BUFFER) {
        /* buf is really a char** to be filled with an allocation the caller
         * frees with LocalFree. Writing the string into it instead would store
         * text over a pointer variable. */
        char **out = (char **)buf;
        char *p = (char *)w32_alloc(n + 1, 0);
        if (!p) return 0;
        memcpy(p, msg, n + 1);
        if (out) *out = p; else { free(p); return 0; }
        return n;
    }
    if (!buf || size == 0) return 0;
    snprintf(buf, size, "%s", msg);
    return n < size ? n : size - 1;
}
static MS uint32_t st_FormatMessageW(uint32_t flags, void *src, uint32_t id,
        uint32_t lang, uint16_t *buf, uint32_t size, void *args)
{
    char msg[256];
    uint32_t n, i;
    (void)src; (void)lang; (void)args;
    snprintf(msg, sizeof msg, "Error %u", id);
    n = (uint32_t)strlen(msg);
    if (flags & W_FM_ALLOCATE_BUFFER) {
        uint16_t **out = (uint16_t **)buf;
        uint16_t *p = (uint16_t *)w32_alloc((n + 1) * sizeof *p, 0);
        if (!p) return 0;
        for (i = 0; i <= n; i++) p[i] = (uint16_t)(unsigned char)msg[i];
        if (out) *out = p; else { free(p); return 0; }
        return n;
    }
    if (!buf || size == 0) return 0;
    for (i = 0; i < n && i + 1 < size; i++) buf[i] = (uint16_t)(unsigned char)msg[i];
    buf[i] = 0;
    return i;
}

/* ------------------------------------------------- IO completion ports ----- */

/* A real queue, because the alternative is a spin.
 *
 * A thread pool parks on GetQueuedCompletionStatus with INFINITE and expects to
 * block until work arrives. The first version of this returned WAIT_TIMEOUT
 * immediately in that case, and Absynth's three worker threads each burned a
 * core doing nothing -- the same "a wait that does not wait" fault as a stubbed
 * Sleep, and just as invisible, because every thread was making progress through
 * its loop as fast as it could.
 *
 * Queueing for real also means a posted completion is delivered rather than
 * dropped, so a pool that hands itself work actually runs it. */
#define W32_MAX_PORTS 8
#define W32_PORT_Q    64
#define W32_PORT_TAG  0x494F0000u          /* 'IO' */

typedef struct {
    int used;
    pthread_mutex_t m;
    pthread_cond_t  c;
    struct { uint32_t bytes; uintptr_t key; void *ov; } q[W32_PORT_Q];
    int head, tail, count;
} w32_iocp;

static w32_iocp g_ports[W32_MAX_PORTS];
static pthread_mutex_t g_ports_lock = PTHREAD_MUTEX_INITIALIZER;

static w32_iocp *port_get(void *h)
{
    unsigned v = (unsigned)(uintptr_t)h, i;
    if ((v & 0xFFFF0000u) != W32_PORT_TAG) return NULL;
    i = v & 0xFFFFu;
    return (i < W32_MAX_PORTS && g_ports[i].used) ? &g_ports[i] : NULL;
}

static MS void *st_CreateIoCompletionPort(void *file, void *port, uintptr_t key,
                                          uint32_t threads)
{
    int i;
    (void)file; (void)key; (void)threads;
    /* With an existing port this is an association, and there is no per-file
     * state to keep: nothing here starts overlapped IO. */
    if (port_get(port)) return port;
    pthread_mutex_lock(&g_ports_lock);
    for (i = 0; i < W32_MAX_PORTS; i++) {
        if (g_ports[i].used) continue;
        memset(&g_ports[i], 0, sizeof g_ports[i]);
        pthread_mutex_init(&g_ports[i].m, NULL);
        pthread_cond_init(&g_ports[i].c, NULL);
        g_ports[i].used = 1;
        pthread_mutex_unlock(&g_ports_lock);
        return (void *)(uintptr_t)(W32_PORT_TAG | (unsigned)i);
    }
    pthread_mutex_unlock(&g_ports_lock);
    g_last_error = 8;
    return NULL;
}

static MS int32_t st_GetQueuedCompletionStatus(void *port, uint32_t *bytes,
        uintptr_t *key, void **ov, uint32_t ms)
{
    w32_iocp *p = port_get(port);
    int rc = 0;

    if (bytes) *bytes = 0;
    if (key) *key = 0;
    if (ov) *ov = NULL;
    if (!p) { g_last_error = 6; return 0; }

    pthread_mutex_lock(&p->m);
    while (!p->count) {
        if (ms == 0) { pthread_mutex_unlock(&p->m); g_last_error = 258; return 0; }
        if (ms == 0xFFFFFFFFu) {
            pthread_cond_wait(&p->c, &p->m);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(ms / 1000u);
            ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&p->c, &p->m, &ts) != 0 && !p->count) {
                pthread_mutex_unlock(&p->m);
                g_last_error = 258;             /* WAIT_TIMEOUT */
                return 0;
            }
        }
    }
    if (bytes) *bytes = p->q[p->head].bytes;
    if (key) *key = p->q[p->head].key;
    if (ov) *ov = p->q[p->head].ov;
    p->head = (p->head + 1) % W32_PORT_Q;
    p->count--;
    rc = 1;
    pthread_mutex_unlock(&p->m);
    return rc;
}

static MS int32_t st_PostQueuedCompletionStatus(void *port, uint32_t bytes,
        uintptr_t key, void *ov)
{
    w32_iocp *p = port_get(port);
    if (!p) { g_last_error = 6; return 0; }
    pthread_mutex_lock(&p->m);
    if (p->count >= W32_PORT_Q) {
        pthread_mutex_unlock(&p->m);
        g_last_error = 298;                     /* ERROR_TOO_MANY_POSTS */
        return 0;
    }
    p->q[p->tail].bytes = bytes;
    p->q[p->tail].key = key;
    p->q[p->tail].ov = ov;
    p->tail = (p->tail + 1) % W32_PORT_Q;
    p->count++;
    pthread_cond_signal(&p->c);
    pthread_mutex_unlock(&p->m);
    return 1;
}


/* --------------------------------------------------- CRT thread creation ---- */

/* _beginthreadex is how the MSVC C++ runtime spawns a thread -- std::thread goes
 * through it, not through CreateThread, because the CRT needs per-thread state
 * set up first. Stubbed, it returns whatever was in the return register; the
 * std::thread constructor reads that as a failure and throws std::system_error,
 * which is precisely what Absynth did.
 *
 * The signature differs from CreateThread's: the start routine returns unsigned
 * rather than taking the DWORD-returning shape, and the thread id is unsigned*.
 * Same underlying thread either way. */
static MSCRT uintptr_t st__beginthreadex(void *security, uint32_t stack,
        void *start, void *arg, uint32_t initflag, uint32_t *tid)
{
    void *h = st_CreateThread(security, stack, start, arg, initflag, tid);
    return (uintptr_t)h;
}
static MSCRT void st__endthreadex(uint32_t code) { (void)code; pthread_exit(NULL); }
static MSCRT uintptr_t st__beginthread(void *start, uint32_t stack, void *arg)
{ return (uintptr_t)st_CreateThread(NULL, stack, start, arg, 0, NULL); }
static MSCRT void st__endthread(void) { pthread_exit(NULL); }

/* ------------------------------------------------------ setjmp / longjmp ---- */

/* These have to save the *guest's* context, so they are written in assembly.
 *
 * The tempting shortcut -- have st__setjmp call the host's setjmp and return its
 * result -- does not work. The host's setjmp records the frame of the stub, and
 * that frame is dead the moment the stub returns; the return address it saved
 * lives in stack the guest has since reused for its own calls. longjmp would
 * then resume through whatever overwrote it.
 *
 * What must be captured instead is the state at the stub's entry, which is the
 * guest's: the caller's stack pointer, the return address, and the Microsoft
 * callee-saved set -- rbx, rbp, rdi, rsi, r12-r15 and xmm6-xmm15. Note that rdi,
 * rsi and the upper xmm registers are callee-saved on Windows but *not* in the
 * System V convention, so a host longjmp would not restore them even if the
 * frame survived.
 *
 * The layout below is private to this pair. MSVC's jmp_buf is 256 bytes on
 * x86-64 and 64 on i386, both of which leave room. */
#if defined(__x86_64__)
__asm__(
".text\n"
".globl peload_ms_setjmp\n"
".type peload_ms_setjmp,@function\n"
"peload_ms_setjmp:\n"
"    movq  (%rsp), %rax\n"            /* the guest's continuation */
"    leaq  8(%rsp), %r8\n"            /* its stack pointer after our ret */
"    movq  %r8,   0(%rcx)\n"
"    movq  %rbp,  8(%rcx)\n"
"    movq  %rbx, 16(%rcx)\n"
"    movq  %rdi, 24(%rcx)\n"
"    movq  %rsi, 32(%rcx)\n"
"    movq  %r12, 40(%rcx)\n"
"    movq  %r13, 48(%rcx)\n"
"    movq  %r14, 56(%rcx)\n"
"    movq  %r15, 64(%rcx)\n"
"    movq  %rax, 72(%rcx)\n"
"    movabsq $0x50454C4F53455421, %rax\n"   /* 'PELOSET!' */
"    movq  %rax, 240(%rcx)\n"
"    movups %xmm6,   80(%rcx)\n"
"    movups %xmm7,   96(%rcx)\n"
"    movups %xmm8,  112(%rcx)\n"
"    movups %xmm9,  128(%rcx)\n"
"    movups %xmm10, 144(%rcx)\n"
"    movups %xmm11, 160(%rcx)\n"
"    movups %xmm12, 176(%rcx)\n"
"    movups %xmm13, 192(%rcx)\n"
"    movups %xmm14, 208(%rcx)\n"
"    movups %xmm15, 224(%rcx)\n"
"    xorl  %eax, %eax\n"
"    ret\n"
".size peload_ms_setjmp,.-peload_ms_setjmp\n"
".globl peload_ms_longjmp\n"
".type peload_ms_longjmp,@function\n"
"peload_ms_longjmp:\n"
"    movabsq $0x50454C4F53455421, %rax\n"
"    cmpq  %rax, 240(%rcx)\n"
"    jne   peload_ms_badjmp\n"       /* never written by our setjmp */
"    movl  %edx, %eax\n"              /* the value setjmp is to return */
"    testl %eax, %eax\n"
"    jne   1f\n"
"    movl  $1, %eax\n"                /* longjmp(buf, 0) still returns 1 */
"1:  movq   8(%rcx), %rbp\n"
"    movq  16(%rcx), %rbx\n"
"    movq  24(%rcx), %rdi\n"
"    movq  32(%rcx), %rsi\n"
"    movq  40(%rcx), %r12\n"
"    movq  48(%rcx), %r13\n"
"    movq  56(%rcx), %r14\n"
"    movq  64(%rcx), %r15\n"
"    movups  80(%rcx), %xmm6\n"
"    movups  96(%rcx), %xmm7\n"
"    movups 112(%rcx), %xmm8\n"
"    movups 128(%rcx), %xmm9\n"
"    movups 144(%rcx), %xmm10\n"
"    movups 160(%rcx), %xmm11\n"
"    movups 176(%rcx), %xmm12\n"
"    movups 192(%rcx), %xmm13\n"
"    movups 208(%rcx), %xmm14\n"
"    movups 224(%rcx), %xmm15\n"
"    movq  72(%rcx), %r8\n"
"    movq   0(%rcx), %rsp\n"          /* last: %rcx must stay live until here */
"    jmp   *%r8\n"
".size peload_ms_longjmp,.-peload_ms_longjmp\n"
);
#elif defined(__i386__)
__asm__(
".text\n"
".globl peload_ms_setjmp\n"
".type peload_ms_setjmp,@function\n"
"peload_ms_setjmp:\n"
"    movl  4(%esp), %ecx\n"           /* buf -- cdecl, on the stack */
"    movl  (%esp), %eax\n"            /* the guest's continuation */
"    leal  4(%esp), %edx\n"           /* its stack pointer after our ret */
"    movl  %edx,  0(%ecx)\n"
"    movl  %ebp,  4(%ecx)\n"
"    movl  %ebx,  8(%ecx)\n"
"    movl  %esi, 12(%ecx)\n"
"    movl  %edi, 16(%ecx)\n"
"    movl  %eax, 20(%ecx)\n"
"    movl  $0x53455421, 24(%ecx)\n"   /* 'SET!' */
"    xorl  %eax, %eax\n"
"    ret\n"
".size peload_ms_setjmp,.-peload_ms_setjmp\n"
".globl peload_ms_longjmp\n"
".type peload_ms_longjmp,@function\n"
"peload_ms_longjmp:\n"
"    movl  4(%esp), %ecx\n"
"    cmpl  $0x53455421, 24(%ecx)\n"
"    jne   peload_ms_badjmp\n"
"    movl  8(%esp), %eax\n"
"    testl %eax, %eax\n"
"    jne   1f\n"
"    movl  $1, %eax\n"
"1:  movl   4(%ecx), %ebp\n"
"    movl   8(%ecx), %ebx\n"
"    movl  12(%ecx), %esi\n"
"    movl  16(%ecx), %edi\n"
"    movl  20(%ecx), %edx\n"
"    movl   0(%ecx), %esp\n"
"    jmp   *%edx\n"
".size peload_ms_longjmp,.-peload_ms_longjmp\n"
);
#endif

#if defined(__x86_64__) || defined(__i386__)
/* Declared with the C runtime's convention because the assembly reads its
 * argument where that convention puts it -- %rcx on x86-64, the stack on i386 --
 * and registered *directly*, with no C wrapper. A wrapper would defeat the
 * point: setjmp would record the wrapper's frame, and that frame dies the
 * instant it returns. */
MSCRT int  peload_ms_setjmp(void *buf);
MSCRT void peload_ms_longjmp(void *buf, int val);

/* Jumped to from the assembly when the buffer lacks the magic our setjmp wrote,
 * so it was never initialised by this pair. Jumping through it anyway would land
 * on whatever the memory happened to hold. */
void peload_ms_badjmp(void);
void peload_ms_badjmp(void)
{
    fprintf(stderr, "[win] longjmp with a jump buffer no setjmp of ours wrote "
                    "-- refusing to jump through it\n");
    fflush(stderr);
    abort();
}
#define st__setjmp    peload_ms_setjmp
#define st__setjmpex  peload_ms_setjmp
#define st_longjmp    peload_ms_longjmp
#endif

/* ----------------------------------------------- directory enumeration ----- */

/* FindFirstFile fills a 592-byte WIN32_FIND_DATA in the caller's frame and hands
 * back a handle to continue from. Unimplemented, the caller reads a name out of
 * uninitialised stack and tries to open it. */
typedef struct {
    uint32_t attrs;
    uint32_t ctime_lo, ctime_hi, atime_lo, atime_hi, wtime_lo, wtime_hi;
    uint32_t size_hi, size_lo;
    uint32_t res0, res1;
    uint16_t name[260];
    uint16_t altname[14];
} W32_FIND_DATAW;

typedef struct {
    uint32_t attrs;
    uint32_t ctime_lo, ctime_hi, atime_lo, atime_hi, wtime_lo, wtime_hi;
    uint32_t size_hi, size_lo;
    uint32_t res0, res1;
    char name[260];
    char altname[14];
} W32_FIND_DATAA;

#define W32_MAX_FINDS 16
#define W32_FIND_TAG   0x46490000u   /* 'FI' */
typedef struct { int used; DIR *d; char dir[512], pat[256]; } w32_find;
static w32_find g_finds[W32_MAX_FINDS];
static pthread_mutex_t g_find_lock = PTHREAD_MUTEX_INITIALIZER;

enum { W_FILE_ATTRIBUTE_DIRECTORY = 0x10, W_FILE_ATTRIBUTE_NORMAL = 0x80 };

/* Windows time is 100-ns ticks since 1601; Unix time is seconds since 1970. */
static void unix_to_filetime(time_t t, uint32_t *lo, uint32_t *hi)
{
    uint64_t ft = ((uint64_t)t + 11644473600ULL) * 10000000ULL;
    *lo = (uint32_t)ft;
    *hi = (uint32_t)(ft >> 32);
}

/* Split "dir/pattern" into its parts. A pattern with no separator searches the
 * current directory. */
static void find_split(const char *spec, char *dir, size_t dn, char *pat, size_t pn)
{
    const char *slash = NULL, *p;
    for (p = spec; p && *p; p++) if (*p == '/' || *p == '\\') slash = p;
    if (!slash) { snprintf(dir, dn, "."); snprintf(pat, pn, "%s", spec ? spec : "*"); return; }
    { size_t len = (size_t)(slash - spec);
      if (len >= dn) len = dn - 1;
      memcpy(dir, spec, len); dir[len] = 0; }
    if (!dir[0]) snprintf(dir, dn, "/");
    snprintf(pat, pn, "%s", slash + 1);
    if (!pat[0]) snprintf(pat, pn, "*");
}

/* Windows wildcards are not fnmatch's. "*.*" is the idiomatic "everything" and
 * matches names with no dot at all, where fnmatch requires the dot to be there.
 * A plugin scanning with "*.*" and finding none of its extensionless files
 * concludes its content is missing. */
static int find_match(const char *pat, const char *name)
{
    if (!pat || !*pat) return 1;
    if (!strcmp(pat, "*") || !strcmp(pat, "*.*")) return 1;
    return fnmatch(pat, name, FNM_CASEFOLD) == 0;
}

static int find_fill(w32_find *f, char *name, size_t nn, uint32_t *attrs,
                     uint64_t *size, time_t *mtime)
{
    struct dirent *de;
    while ((de = readdir(f->d))) {
        char full[1024];
        struct stat st;
        /* "." and ".." are *not* skipped: Windows returns them, every caller is
         * written expecting them, and dropping them makes an empty directory
         * indistinguishable from one that does not exist -- which is what had
         * Absynth conclude its own library folders were absent. */
        if (!find_match(f->pat, de->d_name)) continue;
        snprintf(full, sizeof full, "%s/%s", f->dir, de->d_name);
        if (stat(full, &st) != 0) continue;
        snprintf(name, nn, "%s", de->d_name);
        *attrs = S_ISDIR(st.st_mode) ? W_FILE_ATTRIBUTE_DIRECTORY
                                     : W_FILE_ATTRIBUTE_NORMAL;
        *size = (uint64_t)st.st_size;
        *mtime = st.st_mtime;
        return 1;
    }
    return 0;
}

static void find_store_w(W32_FIND_DATAW *d, const char *name, uint32_t attrs,
                         uint64_t size, time_t mtime)
{
    size_t i;
    memset(d, 0, sizeof *d);
    d->attrs = attrs;
    d->size_lo = (uint32_t)size;
    d->size_hi = (uint32_t)(size >> 32);
    unix_to_filetime(mtime, &d->wtime_lo, &d->wtime_hi);
    d->ctime_lo = d->atime_lo = d->wtime_lo;
    d->ctime_hi = d->atime_hi = d->wtime_hi;
    for (i = 0; name[i] && i < 259; i++) d->name[i] = (uint16_t)(unsigned char)name[i];
    d->name[i] = 0;
}
static void find_store_a(W32_FIND_DATAA *d, const char *name, uint32_t attrs,
                         uint64_t size, time_t mtime)
{
    memset(d, 0, sizeof *d);
    d->attrs = attrs;
    d->size_lo = (uint32_t)size;
    d->size_hi = (uint32_t)(size >> 32);
    unix_to_filetime(mtime, &d->wtime_lo, &d->wtime_hi);
    d->ctime_lo = d->atime_lo = d->wtime_lo;
    d->ctime_hi = d->atime_hi = d->wtime_hi;
    snprintf(d->name, sizeof d->name, "%s", name);
}

static void *find_open(const char *spec, char *name, size_t nn, uint32_t *attrs,
                       uint64_t *size, time_t *mtime)
{
    int i;
    w32_find *f = NULL;
    pthread_mutex_lock(&g_find_lock);
    for (i = 0; i < W32_MAX_FINDS; i++) if (!g_finds[i].used) { f = &g_finds[i]; break; }
    if (!f) { pthread_mutex_unlock(&g_find_lock); g_last_error = 4; return (void *)(intptr_t)-1; }
    memset(f, 0, sizeof *f);
    find_split(spec, f->dir, sizeof f->dir, f->pat, sizeof f->pat);
    if (!(f->d = opendir(f->dir))) {
        pthread_mutex_unlock(&g_find_lock);
        g_last_error = 3;                      /* ERROR_PATH_NOT_FOUND */
        return (void *)(intptr_t)-1;
    }
    f->used = 1;
    if (!find_fill(f, name, nn, attrs, size, mtime)) {
        closedir(f->d); f->used = 0;
        pthread_mutex_unlock(&g_find_lock);
        g_last_error = 2;                      /* ERROR_FILE_NOT_FOUND */
        return (void *)(intptr_t)-1;
    }
    pthread_mutex_unlock(&g_find_lock);
    return (void *)(uintptr_t)(W32_FIND_TAG | (unsigned)(f - g_finds));
}
/* A search handle is a tag plus the slot index, so a handle from somewhere else
 * is rejected rather than indexing the table with whatever it happens to hold. */
static w32_find *find_get(void *h)
{
    unsigned v = (unsigned)(uintptr_t)h, i;
    if ((v & 0xFFFF0000u) != W32_FIND_TAG) return NULL;
    i = v & 0xFFFFu;
    return (i < W32_MAX_FINDS && g_finds[i].used) ? &g_finds[i] : NULL;
}

static MS void *st_FindFirstFileW(const uint16_t *spec, W32_FIND_DATAW *out)
{
    char s[1024], name[512];
    uint32_t attrs = 0; uint64_t size = 0; time_t mt = 0;
    void *h;
    w2c_path(spec, s, sizeof s);
    h = find_open(s, name, sizeof name, &attrs, &size, &mt);
    if (h != (void *)(intptr_t)-1 && out) find_store_w(out, name, attrs, size, mt);
    PLOG("  [win] FindFirstFileW(%s) -> %p\n", s, h);
    return h;
}
static MS void *st_FindFirstFileA(const char *spec, W32_FIND_DATAA *out)
{
    char name[512], p[1024];
    uint32_t attrs = 0; uint64_t size = 0; time_t mt = 0;
    void *h = find_open(spec ? path_fix(spec, p, sizeof p) : "*",
                        name, sizeof name, &attrs, &size, &mt);
    if (h != (void *)(intptr_t)-1 && out) find_store_a(out, name, attrs, size, mt);
    return h;
}

/* FindFirstFileEx is what a modern CRT calls; the extra arguments select a
 * cheaper info level and an optional filter, and neither changes which files
 * come back for the "*" patterns plug-ins use. Answering NULL because of the
 * spelling alone meant a plug-in enumerating its own preset directory found
 * nothing there. fSearchOp is a hint on Windows as well -- documented as not
 * always honoured -- so matching on the pattern is within the contract. */
static MS void *st_FindFirstFileExW(const uint16_t *spec, int32_t level, void *out,
                                    int32_t op, void *filter, uint32_t flags)
{ (void)level; (void)op; (void)filter; (void)flags; return st_FindFirstFileW(spec, out); }
static MS void *st_FindFirstFileExA(const char *spec, int32_t level, void *out,
                                    int32_t op, void *filter, uint32_t flags)
{ (void)level; (void)op; (void)filter; (void)flags; return st_FindFirstFileA(spec, out); }
static MS int32_t st_FindNextFileW(void *h, W32_FIND_DATAW *out)
{
    w32_find *f = find_get(h);
    char name[512];
    uint32_t attrs = 0; uint64_t size = 0; time_t mt = 0;
    if (!f) { g_last_error = 6; return 0; }
    pthread_mutex_lock(&g_find_lock);
    if (!find_fill(f, name, sizeof name, &attrs, &size, &mt)) {
        pthread_mutex_unlock(&g_find_lock);
        g_last_error = 18;                     /* ERROR_NO_MORE_FILES */
        return 0;
    }
    pthread_mutex_unlock(&g_find_lock);
    if (out) find_store_w(out, name, attrs, size, mt);
    return 1;
}
static MS int32_t st_FindNextFileA(void *h, W32_FIND_DATAA *out)
{
    w32_find *f = find_get(h);
    char name[512];
    uint32_t attrs = 0; uint64_t size = 0; time_t mt = 0;
    if (!f) { g_last_error = 6; return 0; }
    pthread_mutex_lock(&g_find_lock);
    if (!find_fill(f, name, sizeof name, &attrs, &size, &mt)) {
        pthread_mutex_unlock(&g_find_lock);
        g_last_error = 18; return 0;
    }
    pthread_mutex_unlock(&g_find_lock);
    if (out) find_store_a(out, name, attrs, size, mt);
    return 1;
}
static MS int32_t st_FindClose(void *h)
{
    w32_find *f = find_get(h);
    if (!f) return 0;
    pthread_mutex_lock(&g_find_lock);
    if (f->d) closedir(f->d);
    f->used = 0; f->d = NULL;
    pthread_mutex_unlock(&g_find_lock);
    return 1;
}

/* ------------------------------------------------------------ odds again --- */

static MSCRT uint16_t *st__wgetenv(const uint16_t *name)
{
    char nb[128];
    if (!name) return NULL;
    w2c(name, nb, sizeof nb);
    return (uint16_t *)w32_env_find_w(nb);
}
static MSCRT int st__wdupenv_s(uint16_t **out, size_t *len, const uint16_t *name)
{
    char nb[128];
    const uint16_t *v;
    size_t n = 0;
    if (!out) return 22;
    *out = NULL;
    if (len) *len = 0;
    if (!name) return 0;
    w2c(name, nb, sizeof nb);
    if (!(v = w32_env_find_w(nb))) return 0;
    while (v[n]) n++;
    if (!(*out = (uint16_t *)w32_alloc((n + 1) * 2, 0))) return 12;
    memcpy(*out, v, (n + 1) * 2);
    if (len) *len = n + 1;
    return 0;
}

/* type_info comparison is by name: MSVC guarantees one descriptor per type
 * within an image, but not across images, and the runtime compares the mangled
 * name for exactly that reason. Returning an uninitialised register here decides
 * type identity at random. */
typedef struct { void *vft, *spare; char name[1]; } w32_type_info;
static MSTHIS int8_t st_type_info_eq(const w32_type_info *a, const w32_type_info *b)
{ return (int8_t)(a == b || (a && b && !strcmp(a->name, b->name))); }
static MSTHIS int8_t st_type_info_ne(const w32_type_info *a, const w32_type_info *b)
{ return (int8_t)!st_type_info_eq(a, b); }
static MSTHIS const char *st_type_info_name(const w32_type_info *t)
{ return t ? t->name : ""; }

static MS int32_t st_RegDeleteValueA(void *k, const char *name)
{ uint16_t w[128]; c2w_(name ? name : "", w, 128); return st_RegDeleteValueW(k, w); }

/* Display geometry. One monitor, the size the host reports, at the origin. */
static MS void *st_GetDesktopWindow(void) { return NULL; }
static MS void *st_FindWindowA(const char *cls, const char *name)
{ (void)cls; (void)name; return NULL; }    /* no other process's windows exist */
static MS void *st_FindWindowW(const uint16_t *cls, const uint16_t *name)
{ (void)cls; (void)name; return NULL; }
static MS void *st_FindWindowExW(void *p, void *c, const uint16_t *cls, const uint16_t *nm)
{ (void)p;(void)c;(void)cls;(void)nm; return NULL; }


/* GetDeviceCaps.
 *
 * A stub answering 0 to everything is not neutral here. LOGPIXELSX is how a
 * plug-in finds the display's DPI, and it scales its whole layout by
 * dpi/96 -- so zero collapses the interface to nothing. That is exactly what
 * happened: a SynthEdit VST3 built its view, asked for the DPI, computed a
 * scale of zero and reported a 0x0 editor, which this host then declined to
 * open because there was nothing to open.
 *
 * The values are an ordinary desktop's, and the ones that describe the screen
 * come from the same place GetSystemMetrics answers from, so a plug-in that
 * asks both ways gets one story. */
static MS int32_t st_GetDeviceCaps(void *hdc, int32_t index)
{
    (void)hdc;
    switch (index) {
    case 2:   return 1;                          /* TECHNOLOGY: DT_RASDISPLAY */
    case 4:   return 520;                        /* HORZSIZE, mm */
    case 6:   return 320;                        /* VERTSIZE, mm */
    case 8:   return st_GetSystemMetrics(0);     /* HORZRES */
    case 10:  return st_GetSystemMetrics(1);     /* VERTRES */
    case 12:  return 32;                         /* BITSPIXEL */
    case 14:  return 1;                          /* PLANES */
    case 24:  return -1;                         /* NUMCOLORS: more than 8bpp */
    case 26:  return 0;                          /* RASTERCAPS */
    case 88:                                     /* LOGPIXELSX */
    case 90:  return 96;                         /* LOGPIXELSY */
    case 104: return 1;                          /* SIZEPALETTE / COLORRES */
    case 118: return 1;                          /* SHADEBLENDCAPS */
    default:  return 0;
    }
}

/* dwData is an LPARAM: pointer-sized, so intptr_t rather than int64_t. As an
 * int64_t this stub was a five-argument stdcall function at i386 -- it popped
 * 20 bytes where win32_arity.h says EnumDisplayMonitors takes 16, so a plugin
 * calling it had its stack unwound four bytes too far, and the callback below
 * was handed an argument list four bytes wider than it pops. */
typedef MS int32_t (*monitorenumproc)(void *, void *, void *, intptr_t);
static MS int32_t st_EnumDisplayMonitors(void *dc, void *clip, void *cb, intptr_t data)
{
    W32RECT r;
    (void)dc; (void)clip;
    r.left = 0; r.top = 0;
    r.right = st_GetSystemMetrics(0);          /* SM_CXSCREEN */
    r.bottom = st_GetSystemMetrics(1);
    if (!cb) return 0;
    /* One monitor, handle 1. A callback that gets no monitors at all concludes
     * there is no display and gives up placing its window. */
    return ((monitorenumproc)cb)((void *)0x4D4F4E31, NULL, &r, data);
}

/* The taskbar. No appbar exists, so the work area is the whole screen -- which
 * is what a zero return and an untouched structure would *not* say. */
static MS uintptr_t st_SHAppBarMessage(uint32_t msg, void *data)
{ (void)msg; (void)data; return 0; }

/* -------------------------------------------------- waiting, for real ------- */

/* A wait that does not wait turns a polling loop into a spin. Absynth's startup
 * hung here at full CPU rather than progressing: nothing was broken, it was
 * simply never yielding. */
static MS uint32_t st_SleepEx(uint32_t ms, int32_t alertable)
{
    struct timespec ts;
    (void)alertable;
    if (ms == 0xFFFFFFFFu) ms = 1000;         /* INFINITE: still yield */
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
    return 0;                                  /* no APC was delivered */
}

/* Concurrency::wait, the C++ runtime's cooperative yield. */
static MSCRT void st_conc_wait(uint32_t ms) { st_SleepEx(ms, 0); }

/* Every thread belongs to the one implicit scheduler here, so they all report
 * the same id -- which is true, rather than a different number each call. */
static MSCRT uint32_t st_conc_scheduler_id(void) { return 1; }

/* ------------------------------------------------------ CRT internals ------- */

/* _lock/_unlock guard the CRT's own tables by index. One mutex per index, and a
 * recursive one because the CRT takes some of these while already holding them. */
#define CRT_LOCKS 64
static pthread_mutex_t g_crt_locks[CRT_LOCKS];
static pthread_once_t g_crt_locks_once = PTHREAD_ONCE_INIT;
static void crt_locks_init(void)
{
    pthread_mutexattr_t a;
    int i;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (i = 0; i < CRT_LOCKS; i++) pthread_mutex_init(&g_crt_locks[i], &a);
    pthread_mutexattr_destroy(&a);
}
static MSCRT void st__lock(int which)
{
    pthread_once(&g_crt_locks_once, crt_locks_init);
    if (which >= 0 && which < CRT_LOCKS) pthread_mutex_lock(&g_crt_locks[which]);
}
static MSCRT void st__unlock(int which)
{
    pthread_once(&g_crt_locks_once, crt_locks_init);
    if (which >= 0 && which < CRT_LOCKS) pthread_mutex_unlock(&g_crt_locks[which]);
}

/* __dllonexit registers a destructor for a DLL's atexit list, and returns the
 * function it was given. Returning garbage instead is read as failure by the
 * caller that checks -- and the ones that do not check store it. */
typedef void (*onexit_fn)(void);
static MSCRT onexit_fn st___dllonexit(onexit_fn f, onexit_fn **begin, onexit_fn **end)
{
    (void)begin; (void)end;
    /* Not recorded: these run at unload, and this host tears the image down
     * without a matching FreeLibrary. Reporting success is honest about what
     * happens -- the function was accepted; it will simply never be called. */
    return f;
}

static MSCRT double st_log2(double x) { return log2(x); }
static MSCRT float st_log2f(float x) { return log2f(x); }
static MSCRT size_t st_strcspn(const char *s, const char *rej)
{ return strcspn(s ? s : "", rej ? rej : ""); }
static MSCRT size_t st_strspn(const char *s, const char *acc)
{ return strspn(s ? s : "", acc ? acc : ""); }
static MSCRT char *st_strpbrk(const char *s, const char *acc)
{ return s && acc ? (char *)strpbrk(s, acc) : NULL; }

/* ----------------------------------------------------- monitor geometry ----- */

/* MONITORINFO, and MONITORINFOEX with a device name after it. cbSize says which
 * the caller passed; writing the longer one into the shorter would run past it. */
typedef struct {
    uint32_t cbSize;
    W32RECT  rcMonitor, rcWork;
    uint32_t dwFlags;
} W32MONITORINFO;

static MS int32_t st_GetMonitorInfoW(void *mon, W32MONITORINFO *mi)
{
    (void)mon;
    if (!mi || mi->cbSize < sizeof *mi) return 0;
    mi->rcMonitor.left = 0; mi->rcMonitor.top = 0;
    mi->rcMonitor.right = st_GetSystemMetrics(0);
    mi->rcMonitor.bottom = st_GetSystemMetrics(1);
    /* No taskbar, so the work area is the whole screen. */
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = 1;                            /* MONITORINFOF_PRIMARY */
    if (mi->cbSize >= sizeof *mi + 64) {
        /* MONITORINFOEXW: a 32-character device name follows. */
        uint16_t *dev = (uint16_t *)((uint8_t *)mi + sizeof *mi);
        static const char nm[] = "\\\\\\\\.\\\\DISPLAY1";
        size_t i;
        for (i = 0; nm[i] && i < 31; i++) dev[i] = (uint16_t)(unsigned char)nm[i];
        dev[i] = 0;
    }
    return 1;
}
static MS int32_t st_GetMonitorInfoA(void *mon, W32MONITORINFO *mi)
{
    (void)mon;
    if (!mi || mi->cbSize < sizeof *mi) return 0;
    st_GetMonitorInfoW(mon, mi);
    if (mi->cbSize >= sizeof *mi + 32)
        snprintf((char *)((uint8_t *)mi + sizeof *mi), 32, "\\\\\\\\.\\\\DISPLAY1");
    return 1;
}
static MS void *st_MonitorFromWindow(void *hwnd, uint32_t flags)
{ (void)hwnd; (void)flags; return (void *)0x4D4F4E31; }
static MS void *st_MonitorFromPoint(int64_t pt, uint32_t flags)
{ (void)pt; (void)flags; return (void *)0x4D4F4E31; }
static MS void *st_MonitorFromRect(const W32RECT *r, uint32_t flags)
{ (void)r; (void)flags; return (void *)0x4D4F4E31; }

/* ------------------------------------------- floating-point control -------- */

/* _control87 and friends are how a plugin asks for denormals to be flushed and
 * for a rounding mode -- and an audio plugin does ask. Reporting a garbage
 * control word back leaves the caller believing the machine is in a state it is
 * not, and a synth that thinks denormals are being flushed when they are not
 * loses a large amount of CPU to them in its filter tails.
 *
 * On x86-64 all arithmetic that matters goes through SSE, so MXCSR is the
 * register to act on. The x87 word is left alone: nothing generated for this
 * target computes with it. */
enum {
    W_MCW_DN = 0x03000000u, W_DN_SAVE = 0x00000000u, W_DN_FLUSH = 0x01000000u,
    W_MCW_EM = 0x0008001Fu,
    W_MCW_RC = 0x00000300u, W_RC_NEAR = 0, W_RC_DOWN = 0x100u,
                            W_RC_UP = 0x200u, W_RC_CHOP = 0x300u,
    W_MCW_PC = 0x00030000u
};

#if defined(__x86_64__) || defined(__i386__)
static uint32_t mxcsr_read(void)
{ uint32_t v; __asm__ __volatile__("stmxcsr %0" : "=m"(v)); return v; }
static void mxcsr_write(uint32_t v)
{ __asm__ __volatile__("ldmxcsr %0" : : "m"(v)); }
#else
static uint32_t mxcsr_read(void) { return 0; }
static void mxcsr_write(uint32_t v) { (void)v; }
#endif

/* MXCSR: bit 15 flush-to-zero, bit 6 denormals-are-zero, bits 13-14 rounding,
 * bits 7-12 the exception masks, bits 0-5 the sticky status flags. */
static uint32_t fp_to_msvc(uint32_t mx)
{
    uint32_t out = 0;
    out |= ((mx & (1u << 15)) && (mx & (1u << 6))) ? W_DN_FLUSH : W_DN_SAVE;
    switch ((mx >> 13) & 3u) {
    case 0: out |= W_RC_NEAR; break;
    case 1: out |= W_RC_DOWN; break;
    case 2: out |= W_RC_UP;   break;
    default: out |= W_RC_CHOP; break;
    }
    /* An unmasked exception is one the caller wants to be told about; MSVC's
     * _EM_ bits are set when the exception is *masked*, same sense as MXCSR. */
    if (mx & (1u << 7))  out |= 0x10u;   /* invalid   */
    if (mx & (1u << 8))  out |= 0x08u;   /* denormal  */
    if (mx & (1u << 9))  out |= 0x04u;   /* divide by zero */
    if (mx & (1u << 10)) out |= 0x02u;   /* overflow  */
    if (mx & (1u << 11)) out |= 0x01u;   /* underflow */
    if (mx & (1u << 12)) out |= 0x080000u; /* inexact */
    return out;
}

static MSCRT uint32_t st__control87(uint32_t newval, uint32_t mask)
{
    uint32_t mx = mxcsr_read();
    if (mask & W_MCW_DN) {
        if ((newval & W_MCW_DN) == W_DN_FLUSH) mx |= (1u << 15) | (1u << 6);
        else                                   mx &= ~((1u << 15) | (1u << 6));
    }
    if (mask & W_MCW_RC) {
        uint32_t rc = newval & W_MCW_RC, bits;
        bits = rc == W_RC_DOWN ? 1u : rc == W_RC_UP ? 2u : rc == W_RC_CHOP ? 3u : 0u;
        mx = (mx & ~(3u << 13)) | (bits << 13);
    }
    if (mask & W_MCW_EM) {
        uint32_t m = mx;
        if (newval & 0x10u) m |= (1u << 7);  else if (mask & 0x10u) m &= ~(1u << 7);
        if (newval & 0x08u) m |= (1u << 8);  else if (mask & 0x08u) m &= ~(1u << 8);
        if (newval & 0x04u) m |= (1u << 9);  else if (mask & 0x04u) m &= ~(1u << 9);
        if (newval & 0x02u) m |= (1u << 10); else if (mask & 0x02u) m &= ~(1u << 10);
        if (newval & 0x01u) m |= (1u << 11); else if (mask & 0x01u) m &= ~(1u << 11);
        mx = m;
    }
    /* _MCW_PC has no effect: SSE is always the width of its operands, and there
     * is no precision control to set. Silently accepted rather than refused,
     * which is what it amounts to on any x86-64 runtime. */
    mxcsr_write(mx);
    return fp_to_msvc(mx);
}
static MSCRT uint32_t st__controlfp(uint32_t n, uint32_t m) { return st__control87(n, m); }
static MSCRT int st__controlfp_s(uint32_t *cur, uint32_t n, uint32_t m)
{ uint32_t r = st__control87(n, m); if (cur) *cur = r; return 0; }

/* The sticky status flags, in MSVC's bit positions. */
static uint32_t fp_status(uint32_t mx)
{
    uint32_t s = 0;
    if (mx & (1u << 0)) s |= 0x10u;      /* invalid   */
    if (mx & (1u << 1)) s |= 0x08u;      /* denormal  */
    if (mx & (1u << 2)) s |= 0x04u;      /* divide by zero */
    if (mx & (1u << 3)) s |= 0x02u;      /* overflow  */
    if (mx & (1u << 4)) s |= 0x01u;      /* underflow */
    if (mx & (1u << 5)) s |= 0x080000u;  /* inexact   */
    return s;
}
static MSCRT uint32_t st__statusfp(void) { return fp_status(mxcsr_read()); }
static MSCRT uint32_t st__clearfp(void)
{
    uint32_t mx = mxcsr_read();
    uint32_t was = fp_status(mx);
    mxcsr_write(mx & ~0x3Fu);            /* clear the sticky flags only */
    return was;
}
static MSCRT void st__fpreset(void) { st__control87(W_DN_SAVE | W_RC_NEAR,
                                                    W_MCW_DN | W_MCW_RC); }
static MSCRT int st_fegetenv(void *env)
{ if (env) *(uint32_t *)env = mxcsr_read(); return 0; }
static MSCRT int st_fesetenv(const void *env)
{ if (env) mxcsr_write(*(const uint32_t *)env); return 0; }
static MSCRT int st_fegetround(void)
{ uint32_t r = (mxcsr_read() >> 13) & 3u;
  return r == 1 ? FE_DOWNWARD : r == 2 ? FE_UPWARD : r == 3 ? FE_TOWARDZERO : FE_TONEAREST; }
static MSCRT int st_fesetround(int mode)
{
    uint32_t bits = mode == FE_DOWNWARD ? 1u : mode == FE_UPWARD ? 2u
                  : mode == FE_TOWARDZERO ? 3u : 0u;
    mxcsr_write((mxcsr_read() & ~(3u << 13)) | (bits << 13));
    return 0;
}

/* ------------------------------------------------- aligned allocation ------ */

/* A plugin aligns its buffers so it can load them with aligned SSE moves, and a
 * stub returning a stale register value is a store to an arbitrary address on
 * the first write. posix_memalign gives the same guarantee, and _aligned_free
 * must then be free() rather than anything of its own. */
/* MSVC's aligned allocator, including the offset form.
 *
 * `_aligned_offset_malloc(size, align, off)` returns a pointer p such that
 * `p + off` is aligned -- not p itself. FM8 uses it with an offset of 8 for a
 * block laid out as an 8-byte count followed by 16-byte-aligned data, then writes
 * that data with `movaps`. Ignoring the offset put the data 8 bytes out and the
 * first aligned store faulted, which killed the audio thread -- and out of
 * process, the helper with it, so FM8's editor never appeared in the host.
 *
 * These cannot use posix_memalign, because the offset form has no equivalent and
 * because _aligned_free must be able to recover the original allocation. The base
 * pointer is stored in the word immediately before the returned address, which is
 * what _aligned_free and _aligned_msize read back. Memory from these must
 * therefore be released with _aligned_free and not free(), exactly as MSVC
 * documents. */
static void *aligned_alloc_off(size_t size, size_t align, size_t off)
{
    void *raw;
    uintptr_t p;

    if (align < sizeof(void *)) align = sizeof(void *);
    if (align & (align - 1)) { g_crt_errno = 22; return NULL; }  /* not a power of two */
    raw = malloc(size + align + off + sizeof(void *));
    if (!raw) { g_crt_errno = 12; return NULL; }
    /* Align (base + off) upward, then step back by off so the caller's data --
     * which starts at off -- lands on the boundary. */
    p = (uintptr_t)raw + sizeof(void *) + off;
    p = (p + align - 1) & ~(uintptr_t)(align - 1);
    p -= off;
    ((void **)p)[-1] = raw;
    return (void *)p;
}

static MSCRT void *st__aligned_malloc(size_t size, size_t align)
{ return aligned_alloc_off(size ? size : 1, align, 0); }
static MSCRT void *st__aligned_offset_malloc(size_t size, size_t align, size_t off)
{ return aligned_alloc_off(size ? size : 1, align, off); }
static MSCRT void st__aligned_free(void *p)
{ if (p) free(((void **)p)[-1]); }
static MSCRT size_t st__aligned_msize(void *p, size_t align, size_t off)
{
    (void)align;
    if (!p) return 0;
    /* What is left of the block from the caller's pointer onward. */
    return malloc_usable_size(((void **)p)[-1])
         - (size_t)((char *)p - (char *)((void **)p)[-1]) - off;
}
static MSCRT void *st__aligned_offset_realloc(void *p, size_t size, size_t align, size_t off)
{
    void *n;
    size_t old;
    if (!p) return aligned_alloc_off(size ? size : 1, align, off);
    if (!size) { st__aligned_free(p); return NULL; }
    old = st__aligned_msize(p, align, off);
    n = aligned_alloc_off(size, align, off);
    if (!n) return NULL;
    memcpy(n, p, old < size ? old : size);
    st__aligned_free(p);
    return n;
}
static MSCRT void *st__aligned_realloc(void *p, size_t size, size_t align)
{ return st__aligned_offset_realloc(p, size, align, 0); }

static MSCRT int st__fcloseall(void) { return 0; }
static MSCRT void *st__localtime64(const int64_t *t)
{ static __thread struct tm out; time_t v = t ? (time_t)*t : 0;
  return localtime_r(&v, &out); }
static MSCRT void *st__gmtime64(const int64_t *t)
{ static __thread struct tm out; time_t v = t ? (time_t)*t : 0;
  return gmtime_r(&v, &out); }

/* ------------------------------------------------------------- Winsock ----- */

/* WSAStartup is the first thing any code touching sockets calls, and it reports
 * through a caller-supplied structure as well as its return value. Stubbed, its
 * return register decides whether the caller believes networking exists -- and a
 * subsystem that concludes it does not may leave itself half-constructed rather
 * than fail outright.
 *
 * Winsock genuinely is available here (these are Berkeley sockets underneath), so
 * reporting success is honest. What is *not* provided is socket creation: nothing
 * has asked for it yet, and a plugin reaching for the network is a decision worth
 * surfacing rather than quietly allowing. */
typedef struct {
    uint16_t wVersion, wHighVersion;
    uint16_t iMaxSockets, iMaxUdpDg;        /* x64 puts these before the strings */
    char    *lpVendorInfo;
    char     szDescription[257];
    char     szSystemStatus[129];
} W32WSADATA;

static MS int32_t st_WSAStartup(uint16_t wanted, W32WSADATA *out)
{
    if (!out) return 10014;                  /* WSAEFAULT */
    memset(out, 0, sizeof *out);
    /* Answer with the version asked for when it is one we can honour, which is
     * what a real Winsock does -- it does not force the caller up to 2.2. */
    out->wVersion = wanted <= 0x0202 ? wanted : 0x0202;
    out->wHighVersion = 0x0202;
    out->iMaxSockets = 0;                    /* meaningless from Winsock 2 on */
    out->iMaxUdpDg = 0;
    out->lpVendorInfo = NULL;
    snprintf(out->szDescription, sizeof out->szDescription, "peload sockets");
    snprintf(out->szSystemStatus, sizeof out->szSystemStatus, "Running");
    PLOG("  [win] WSAStartup(%u.%u) -> ok\n", wanted & 0xFF, wanted >> 8);
    return 0;
}
static MS int32_t st_WSACleanup(void) { return 0; }
static MS int32_t st_WSAGetLastError(void) { return (int32_t)g_last_error; }
static MS void st_WSASetLastError(int32_t e) { g_last_error = (uint32_t)e; }

static MS uint32_t st_htonl_(uint32_t v) { return htonl(v); }
static MS uint16_t st_htons_(uint16_t v) { return htons(v); }
static MS uint32_t st_ntohl_(uint32_t v) { return ntohl(v); }
static MS uint16_t st_ntohs_(uint16_t v) { return ntohs(v); }

static MS int32_t st_ws_gethostname(char *name, int32_t len)
{
    if (!name || len <= 0) return -1;
    if (gethostname(name, (size_t)len) != 0) {
        snprintf(name, (size_t)len, "localhost");
    }
    name[len - 1] = 0;
    return 0;
}

/* GetFileAttributesEx reports through a caller-supplied structure, so a stub
 * leaves the caller reading uninitialised stack as a file's size and timestamps.
 * SQLite uses it to decide whether a database exists and how big it is. */
typedef struct {
    uint32_t dwFileAttributes;
    uint32_t ftCreation_lo, ftCreation_hi;
    uint32_t ftAccess_lo, ftAccess_hi;
    uint32_t ftWrite_lo, ftWrite_hi;
    uint32_t nFileSizeHigh, nFileSizeLow;
} W32_FILE_ATTRIBUTE_DATA;

static MS int32_t st_GetFileAttributesExA(const char *name, uint32_t level, void *out)
{
    W32_FILE_ATTRIBUTE_DATA *d = out;
    struct stat st;
    char fixed[1024];
    (void)level;
    if (!name || !d) { g_last_error = 87; return 0; }
    if (stat(path_fix(name, fixed, sizeof fixed), &st) != 0) {
        g_last_error = 2;                        /* ERROR_FILE_NOT_FOUND */
        return 0;
    }
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? W_FILE_ATTRIBUTE_DIRECTORY
                                              : W_FILE_ATTRIBUTE_NORMAL;
    unix_to_filetime(st.st_mtime, &d->ftWrite_lo, &d->ftWrite_hi);
    d->ftCreation_lo = d->ftAccess_lo = d->ftWrite_lo;
    d->ftCreation_hi = d->ftAccess_hi = d->ftWrite_hi;
    d->nFileSizeLow  = (uint32_t)((uint64_t)st.st_size & 0xFFFFFFFFu);
    d->nFileSizeHigh = (uint32_t)((uint64_t)st.st_size >> 32);
    return 1;
}
static MS int32_t st_GetFileAttributesExW(const uint16_t *name, uint32_t level, void *out)
{ char b[1024]; if (!name) { g_last_error = 87; return 0; }
  return st_GetFileAttributesExA(w2c_path(name, b, sizeof b), level, out); }

/* ------------------------------------------------------- file locking ------ */

/* SQLite's whole concurrency protocol is byte-range locks at fixed offsets, and
 * it reads a failed lock as "another connection holds it" -- SQLITE_BUSY. With
 * LockFileEx stubbed, Absynth's database was permanently busy and every query
 * against it threw.
 *
 * Implemented with fcntl locks, which are the real thing: they are visible across
 * processes, so two peload instances sharing a database still interlock.
 *
 * One documented difference: POSIX locks belong to the *process*, where Windows
 * locks belong to the *handle*. Two connections inside one process therefore do
 * not conflict here where they would on Windows. That is the same relaxation Wine
 * makes, and it errs towards letting work proceed; it would matter for a plugin
 * that opens one database twice and relies on the lock to serialise itself. */
enum { W_LOCKFILE_EXCLUSIVE = 0x1, W_LOCKFILE_FAIL_IMMEDIATELY = 0x2 };

static int file_lock_op(void *h, int type, uint64_t off, uint64_t len, int wait)
{
    hobj *o = h_get(h, H_FILE);
    struct flock fl;
    if (!o) { g_last_error = 6; return 0; }
    memset(&fl, 0, sizeof fl);
    fl.l_type = (short)type;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)off;
    /* A zero length means "to end of file" for fcntl, which is also what a zero
     * count means to LockFile -- but a genuine zero-byte range is a no-op there,
     * so it is refused rather than turned into a whole-file lock. */
    if (len == 0) return 1;
    fl.l_len = (off_t)len;
    if (fcntl(o->fd, wait ? F_SETLKW : F_SETLK, &fl) != 0) {
        g_last_error = 33;                     /* ERROR_LOCK_VIOLATION */
        return 0;
    }
    return 1;
}

static MS int32_t st_LockFile(void *h, uint32_t offLo, uint32_t offHi,
                              uint32_t lenLo, uint32_t lenHi)
{
    return file_lock_op(h, F_WRLCK,
                        ((uint64_t)offHi << 32) | offLo,
                        ((uint64_t)lenHi << 32) | lenLo, 0);
}
static MS int32_t st_UnlockFile(void *h, uint32_t offLo, uint32_t offHi,
                                uint32_t lenLo, uint32_t lenHi)
{
    return file_lock_op(h, F_UNLCK,
                        ((uint64_t)offHi << 32) | offLo,
                        ((uint64_t)lenHi << 32) | lenLo, 0);
}
static MS int32_t st_LockFileEx(void *h, uint32_t flags, uint32_t reserved,
                                uint32_t lenLo, uint32_t lenHi,
                                const W32OVERLAPPED *ov)
{
    uint64_t off = ov ? (((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : 0;
    (void)reserved;
    return file_lock_op(h, (flags & W_LOCKFILE_EXCLUSIVE) ? F_WRLCK : F_RDLCK,
                        off, ((uint64_t)lenHi << 32) | lenLo,
                        !(flags & W_LOCKFILE_FAIL_IMMEDIATELY));
}
static MS int32_t st_UnlockFileEx(void *h, uint32_t reserved,
                                  uint32_t lenLo, uint32_t lenHi,
                                  const W32OVERLAPPED *ov)
{
    uint64_t off = ov ? (((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : 0;
    (void)reserved;
    return file_lock_op(h, F_UNLCK, off, ((uint64_t)lenHi << 32) | lenLo, 0);
}

/* ------------------------------------------------------------ wall clock --- */

/* SYSTEMTIME, filled through the caller's pointer. A stub leaves whatever was on
 * the stack to be read as a date, and code that stamps a record or compares
 * against "now" then behaves arbitrarily. */
typedef struct {
    uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} W32SYSTEMTIME;

static void tm_to_systime(const struct tm *t, long msec, W32SYSTEMTIME *o)
{
    o->wYear   = (uint16_t)(t->tm_year + 1900);
    o->wMonth  = (uint16_t)(t->tm_mon + 1);
    o->wDayOfWeek = (uint16_t)t->tm_wday;
    o->wDay    = (uint16_t)t->tm_mday;
    o->wHour   = (uint16_t)t->tm_hour;
    o->wMinute = (uint16_t)t->tm_min;
    o->wSecond = (uint16_t)t->tm_sec;
    o->wMilliseconds = (uint16_t)msec;
}
static MS void st_GetSystemTime(W32SYSTEMTIME *o)
{
    struct timespec ts;
    struct tm g;
    if (!o) return;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &g);
    tm_to_systime(&g, ts.tv_nsec / 1000000L, o);
}
static MS void st_GetLocalTime(W32SYSTEMTIME *o)
{
    struct timespec ts;
    struct tm l;
    if (!o) return;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &l);
    tm_to_systime(&l, ts.tv_nsec / 1000000L, o);
}
static MS int32_t st_SystemTimeToFileTime(const W32SYSTEMTIME *in, uint32_t *out)
{
    struct tm t;
    time_t secs;
    if (!in || !out) return 0;
    memset(&t, 0, sizeof t);
    t.tm_year = in->wYear - 1900; t.tm_mon = in->wMonth - 1; t.tm_mday = in->wDay;
    t.tm_hour = in->wHour; t.tm_min = in->wMinute; t.tm_sec = in->wSecond;
    secs = timegm(&t);                          /* SYSTEMTIME here is UTC */
    unix_to_filetime(secs, &out[0], &out[1]);
    return 1;
}
static MS int32_t st_FileTimeToSystemTime(const uint32_t *in, W32SYSTEMTIME *out)
{
    uint64_t ft;
    time_t secs;
    struct tm g;
    if (!in || !out) return 0;
    ft = ((uint64_t)in[1] << 32) | in[0];
    secs = (time_t)(ft / 10000000ULL) - 11644473600LL;
    gmtime_r(&secs, &g);
    tm_to_systime(&g, (long)((ft / 10000ULL) % 1000ULL), out);
    return 1;
}
static MS int32_t st_FileTimeToLocalFileTime(const uint32_t *in, uint32_t *out)
{ if (!in || !out) return 0; out[0] = in[0]; out[1] = in[1]; return 1; }
static MS int32_t st_LocalFileTimeToFileTime(const uint32_t *in, uint32_t *out)
{ if (!in || !out) return 0; out[0] = in[0]; out[1] = in[1]; return 1; }
static MS int32_t st_SystemTimeToTzSpecificLocalTime(const void *tz,
        const W32SYSTEMTIME *in, W32SYSTEMTIME *out)
{ (void)tz; if (!in || !out) return 0; *out = *in; return 1; }

#define SM(dll, mangled, fn) { dll, mangled, (void *)fn }
/* ------------------------------------------------ the C runtime's file API --
 *
 * There was no stdio here at all, narrow or wide. Most plugins never notice --
 * they reach for CreateFileW and get the Win32 path above -- but a plugin that
 * opens its content through the CRT got the generic stub for every call, and
 * the generic stub returns 0. For this corner of the CRT that return value is
 * not merely unhelpful, it is an affirmative lie: `_waccess` returns 0 to mean
 * "the file is there", and `_wopen` returns 0 to mean "descriptor 0", which is
 * stdin. Kontakt asks `_waccess` whether its library exists, is told yes, opens
 * it with `_wopen`, is handed stdin, and reads nothing -- which is what a
 * sampler rendering silence looks like from outside.
 *
 * These are thin wrappers over the host's own libc. The FILE* handed back is
 * the host's, which is safe precisely because it is opaque: a plugin passes it
 * to fread and fclose rather than reading its fields. Every path goes through
 * path_fix, so backslashes arrive normalised as everywhere else, and a create
 * makes its parents inside this host's tree exactly as file_open does. */

enum {
    MSO_RDONLY = 0x0000, MSO_WRONLY = 0x0001, MSO_RDWR   = 0x0002,
    MSO_APPEND = 0x0008, MSO_CREAT  = 0x0100, MSO_TRUNC  = 0x0200,
    MSO_EXCL   = 0x0400
};

/* Windows' _O_* bits are not Linux's -- _O_CREAT is 0x100 there and 0100 here,
 * so passing them through unchanged would ask for something else entirely. */
static int crt_oflags(int msflags)
{
    int fl;
    if (msflags & MSO_RDWR)        fl = O_RDWR;
    else if (msflags & MSO_WRONLY) fl = O_WRONLY;
    else                           fl = O_RDONLY;
    if (msflags & MSO_APPEND) fl |= O_APPEND;
    if (msflags & MSO_CREAT)  fl |= O_CREAT;
    if (msflags & MSO_TRUNC)  fl |= O_TRUNC;
    if (msflags & MSO_EXCL)   fl |= O_EXCL;
    return fl;   /* _O_TEXT and _O_BINARY have no effect on this platform */
}

static int crt_open_path(const char *path, int msflags, int pmode)
{
    int fl = crt_oflags(msflags), fd;
    fd = open(path, fl, pmode ? (mode_t)pmode : 0644);
    if (fd < 0 && errno == ENOENT && (fl & O_CREAT)) {
        make_parents_in_root(path);
        fd = open(path, fl, pmode ? (mode_t)pmode : 0644);
    }
    PLOG("  [crt] open(%s) -> %s\n", path, fd < 0 ? strerror(errno) : "ok");
    return fd;
}

static MSCRT int st__open(const char *name, int flags, int pmode)
{ char p[1024]; if (!name) return -1;
  return crt_open_path(path_fix(name, p, sizeof p), flags, pmode); }
static MSCRT int st__wopen(const uint16_t *name, int flags, int pmode)
{ char p[1024]; if (!name) return -1;
  w2c_path(name, p, sizeof p); return crt_open_path(p, flags, pmode); }
static MSCRT int st__close(int fd)   { return fd < 0 ? -1 : close(fd); }
static MSCRT int st__read(int fd, void *b, unsigned n)  { return (int)read(fd, b, n); }
static MSCRT int st__write(int fd, const void *b, unsigned n) { return (int)write(fd, b, n); }
static MSCRT long st__lseek(int fd, long off, int whence)
{ return (long)lseek(fd, off, whence); }
static MSCRT int64_t st__lseeki64(int fd, int64_t off, int whence)
{ return (int64_t)lseek(fd, (off_t)off, whence); }
static MSCRT int st__eof(int fd)
{ off_t cur = lseek(fd, 0, SEEK_CUR), end = lseek(fd, 0, SEEK_END);
  if (cur < 0 || end < 0) return -1; lseek(fd, cur, SEEK_SET); return cur >= end; }

/* _access and _waccess return 0 when the file has the mode asked for and -1
 * when it does not -- the opposite polarity to most of Win32, and the reason
 * the generic stub was so damaging here. Windows has no X_OK, so an execute
 * query is answered as existence. */
static int crt_access(const char *p, int mode)
{ return access(p, (mode & 2) ? W_OK : (mode & 4) ? R_OK : F_OK); }
static MSCRT int st__access(const char *n, int mode)
{ char p[1024]; if (!n) return -1; return crt_access(path_fix(n, p, sizeof p), mode); }
static MSCRT int st__waccess(const uint16_t *n, int mode)
{ char p[1024]; if (!n) return -1; w2c_path(n, p, sizeof p); return crt_access(p, mode); }

/* MSVC accepts mode letters this platform does not -- ",ccs=UTF-8", and the
 * commit and share flags N, S, R, T, D -- so the string is filtered down to
 * the ones fopen understands rather than passed through and rejected whole. */
static void crt_mode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; in && *in && o + 1 < n; in++) {
        if (*in == ',') break;                      /* ",ccs=..." */
        if (strchr("rwa+b", *in)) out[o++] = *in;
    }
    if (!o) out[o++] = 'r';
    out[o] = 0;
}

static FILE *crt_fopen_path(const char *path, const char *mode)
{
    char m[8];
    FILE *f;
    crt_mode(mode, m, sizeof m);
    f = fopen(path, m);
    if (!f && errno == ENOENT && !strchr(m, 'r')) {
        make_parents_in_root(path);
        f = fopen(path, m);
    }
    PLOG("  [crt] fopen(%s,%s) -> %s\n", path, m, f ? "ok" : strerror(errno));
    return f;
}

static MSCRT void *st_fopen(const char *name, const char *mode)
{ char p[1024]; if (!name) return NULL;
  return crt_fopen_path(path_fix(name, p, sizeof p), mode); }
static MSCRT void *st__wfopen(const uint16_t *name, const uint16_t *mode)
{ char p[1024], m[32]; if (!name) return NULL;
  w2c_path(name, p, sizeof p); w2c(mode, m, sizeof m);
  return crt_fopen_path(p, m); }
static MSCRT int st_fopen_s(void **out, const char *name, const char *mode)
{ if (!out) return 22; *out = st_fopen(name, mode); return *out ? 0 : 2; }
static MSCRT int st__wfopen_s(void **out, const uint16_t *name, const uint16_t *mode)
{ if (!out) return 22; *out = st__wfopen(name, mode); return *out ? 0 : 2; }
static MSCRT void *st__fdopen(int fd, const char *mode)
{ char m[8]; crt_mode(mode, m, sizeof m); return fdopen(fd, m); }
static MSCRT int st__fileno(void *f) { return f ? fileno((FILE *)f) : -1; }

static MSCRT int st_fclose(void *f)  { return f ? fclose((FILE *)f) : -1; }
static MSCRT size_t st_fread(void *b, size_t sz, size_t n, void *f)
{ return f ? fread(b, sz, n, (FILE *)f) : 0; }
static MSCRT size_t st_fwrite(const void *b, size_t sz, size_t n, void *f)
{ return f ? fwrite(b, sz, n, (FILE *)f) : 0; }
static MSCRT int st_fseek(void *f, long off, int wh)
{ return f ? fseek((FILE *)f, off, wh) : -1; }
static MSCRT int st__fseeki64(void *f, int64_t off, int wh)
{ return f ? fseeko((FILE *)f, (off_t)off, wh) : -1; }
static MSCRT long st_ftell(void *f) { return f ? ftell((FILE *)f) : -1L; }
static MSCRT int64_t st__ftelli64(void *f) { return f ? (int64_t)ftello((FILE *)f) : -1; }
static MSCRT void st_rewind(void *f) { if (f) rewind((FILE *)f); }
static MSCRT int st_feof(void *f)   { return f ? feof((FILE *)f) : 1; }
static MSCRT int st_ferror(void *f) { return f ? ferror((FILE *)f) : 1; }
static MSCRT void st_clearerr(void *f) { if (f) clearerr((FILE *)f); }
static MSCRT int st_fflush(void *f) { return f ? fflush((FILE *)f) : 0; }
static MSCRT int st_fgetc(void *f)  { return f ? fgetc((FILE *)f) : -1; }
static MSCRT int st_fputc(int c, void *f) { return f ? fputc(c, (FILE *)f) : -1; }
static MSCRT int st_ungetc(int c, void *f) { return f ? ungetc(c, (FILE *)f) : -1; }
static MSCRT char *st_fgets(char *b, int n, void *f)
{ return f ? fgets(b, n, (FILE *)f) : NULL; }
static MSCRT int st_fputs(const char *s, void *f) { return f ? fputs(s, (FILE *)f) : -1; }
static MSCRT int st_setvbuf(void *f, char *b, int m, size_t sz)
{ return f ? setvbuf((FILE *)f, b, m, sz) : -1; }

static MSCRT int st_remove(const char *n)
{ char p[1024]; return n ? remove(path_fix(n, p, sizeof p)) : -1; }
static MSCRT int st__wremove(const uint16_t *n)
{ char p[1024]; if (!n) return -1; w2c_path(n, p, sizeof p); return remove(p); }
static MSCRT int st__unlink(const char *n)
{ char p[1024]; return n ? unlink(path_fix(n, p, sizeof p)) : -1; }
static MSCRT int st__wunlink(const uint16_t *n)
{ char p[1024]; if (!n) return -1; w2c_path(n, p, sizeof p); return unlink(p); }
static MSCRT int st_rename(const char *a, const char *b)
{ char p[1024], q[1024]; if (!a || !b) return -1;
  path_fix(a, p, sizeof p); path_fix(b, q, sizeof q); return rename(p, q); }
static MSCRT int st__wrename(const uint16_t *a, const uint16_t *b)
{ char p[1024], q[1024]; if (!a || !b) return -1;
  w2c_path(a, p, sizeof p); w2c_path(b, q, sizeof q); return rename(p, q); }
static int crt_mkdir_at(const char *p)
{ if (mkdir(p, 0755) == 0) return 0;
  if (errno == EEXIST) return -1;              /* Windows fails this too */
  if (errno == ENOENT) { make_parents_in_root(p); return mkdir(p, 0755); }
  return -1; }
static MSCRT int st__mkdir(const char *n)
{ char p[1024]; if (!n) return -1; return crt_mkdir_at(path_fix(n, p, sizeof p)); }
static MSCRT int st__wmkdir(const uint16_t *n)
{ char p[1024]; if (!n) return -1; w2c_path(n, p, sizeof p); return crt_mkdir_at(p); }

/* The legacy unbounded _vswprintf, which predates the _c suffixed form above
 * and takes no size. Given a bound anyway: the alternative is trusting the
 * caller's buffer to fit whatever the format produces. */
static MSCRT int st__vswprintf(uint16_t *b, const uint16_t *f, MSVA_LIST a)
{ return (int)w32_vfmtw(b, 4096, f, a); }

/* time_t is 64-bit in this runtime whichever width the guest is, which is the
 * whole point of the _time64 spelling. */
static MSCRT int64_t st__time64(int64_t *t)
{ int64_t v = (int64_t)time(NULL); if (t) *t = v; return v; }
static MSCRT int32_t st__time32(int32_t *t)
{ int32_t v = (int32_t)time(NULL); if (t) *t = v; return v; }


/* --------------------------------------------------- the private profile API

 * GetPrivateProfileString and its neighbours -- the .ini file API, which is how
 * a plugin written before the registry became fashionable keeps its settings.
 * Nothing implemented it, so every read returned the generic stub's 0 and every
 * write went nowhere: settings did not persist, and the plugins that use it
 * (stigma64 and sixtraq64 here) silently started from defaults every time.
 *
 * Windows resolves a bare filename against the Windows directory. Doing that
 * literally would scatter .ini files across the working directory, which is
 * where `\FullBucketMusic\*.ini` came from, so a relative name is anchored
 * under this host's own tree instead -- the same tree SHGetFolderPath hands
 * out, which is the place a settings file belongs and survives a reboot. */

static const char *ini_path(const char *name, char *buf, size_t n)
{
    char fixed[1024];
    if (!name || !*name) return NULL;
    path_fix(name, fixed, sizeof fixed);
    if (fixed[0] == '/') { snprintf(buf, n, "%s", fixed); return buf; }
    snprintf(buf, n, "%s/%s", peload_data_root(), fixed);
    return buf;
}

/* One pass over the file, calling back per key in the section asked for. The
 * files are a few hundred bytes; parsing on each call is cheaper than any cache
 * that would then have to be invalidated by the write side below. */
typedef void (*ini_cb)(const char *key, const char *val, void *ctx);

static int ini_walk(const char *path, const char *want, ini_cb cb, void *ctx)
{
    FILE *f = fopen(path, "r");
    char line[1024], cur[128];
    int found = 0;
    if (!f) return 0;
    cur[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *e, *eq;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) continue;
            *close = 0;
            snprintf(cur, sizeof cur, "%s", s + 1);
            if (want && !strcasecmp(cur, want)) found = 1;
            continue;
        }
        if (!want || strcasecmp(cur, want) != 0) continue;
        if (!(eq = strchr(s, '='))) continue;
        *eq = 0;
        {   /* trim the key's trailing and the value's leading whitespace */
            char *ke = eq;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
            eq++;
            while (*eq == ' ' || *eq == '\t') eq++;
            if (cb) cb(s, eq, ctx);
        }
    }
    fclose(f);
    return found;
}

typedef struct { const char *key; char val[1024]; int hit; } ini_find;
static void ini_find_cb(const char *k, const char *v, void *ctx)
{
    ini_find *fnd = (ini_find *)ctx;
    if (fnd->hit || strcasecmp(k, fnd->key) != 0) return;
    snprintf(fnd->val, sizeof fnd->val, "%s", v);
    fnd->hit = 1;
}

static MS uint32_t st_GetPrivateProfileStringA(const char *sect, const char *key,
        const char *def, char *out, uint32_t size, const char *file)
{
    char path[1024];
    ini_find fnd;
    const char *src;
    uint32_t n;

    if (!out || !size) return 0;
    fnd.key = key; fnd.val[0] = 0; fnd.hit = 0;
    if (key && ini_path(file, path, sizeof path))
        ini_walk(path, sect, ini_find_cb, &fnd);
    src = fnd.hit ? fnd.val : (def ? def : "");
    n = (uint32_t)strlen(src);
    if (n > size - 1) n = size - 1;
    memcpy(out, src, n);
    out[n] = 0;
    return n;
}
static MS uint32_t st_GetPrivateProfileStringW(const uint16_t *sect, const uint16_t *key,
        const uint16_t *def, uint16_t *out, uint32_t size, const uint16_t *file)
{
    char s[256], k[256], d[1024], f[1024], nb[1024];
    uint32_t n;
    if (!out || !size) return 0;
    if (sect) w2c(sect, s, sizeof s);
    if (key)  w2c(key, k, sizeof k);
    if (def)  w2c(def, d, sizeof d);
    if (file) w2c(file, f, sizeof f);
    n = st_GetPrivateProfileStringA(sect ? s : NULL, key ? k : NULL,
                                    def ? d : NULL, nb, sizeof nb,
                                    file ? f : NULL);
    {   uint32_t i;
        for (i = 0; i < n && i + 1 < size; i++) out[i] = (uint8_t)nb[i];
        out[i] = 0;
        return i;
    }
}
static MS uint32_t st_GetPrivateProfileIntA(const char *sect, const char *key,
                                            int32_t def, const char *file)
{
    char buf[64];
    if (!st_GetPrivateProfileStringA(sect, key, "", buf, sizeof buf, file))
        return (uint32_t)def;
    return (uint32_t)strtol(buf, NULL, 0);
}
static MS uint32_t st_GetPrivateProfileIntW(const uint16_t *sect, const uint16_t *key,
                                            int32_t def, const uint16_t *file)
{
    char s[256], k[256], f[1024];
    if (sect) w2c(sect, s, sizeof s);
    if (key)  w2c(key, k, sizeof k);
    if (file) w2c(file, f, sizeof f);
    return st_GetPrivateProfileIntA(sect ? s : NULL, key ? k : NULL, def,
                                    file ? f : NULL);
}

/* GetPrivateProfileSection reports the whole section as "key=value" runs, each
 * NUL-terminated, the lot terminated by a second NUL. A stub that returned 0
 * left the caller reading its own uninitialised buffer for that structure. */
typedef struct { char *out; uint32_t size, used; } ini_sect;
static void ini_sect_cb(const char *k, const char *v, void *ctx)
{
    ini_sect *c = (ini_sect *)ctx;
    uint32_t need = (uint32_t)(strlen(k) + strlen(v) + 2);
    if (c->used + need + 1 > c->size) return;         /* leave room for the final NUL */
    c->used += (uint32_t)snprintf(c->out + c->used, c->size - c->used, "%s=%s", k, v) + 1;
}
static MS uint32_t st_GetPrivateProfileSectionA(const char *sect, char *out,
                                                uint32_t size, const char *file)
{
    char path[1024];
    ini_sect c;
    if (!out || size < 2) { if (out && size) out[0] = 0; return 0; }
    c.out = out; c.size = size - 1; c.used = 0;
    memset(out, 0, size);
    if (ini_path(file, path, sizeof path))
        ini_walk(path, sect, ini_sect_cb, &c);
    out[c.used] = 0;
    return c.used ? c.used : 0;
}
static MS uint32_t st_GetPrivateProfileSectionW(const uint16_t *sect, uint16_t *out,
                                                uint32_t size, const uint16_t *file)
{
    char s[256], f[1024], nb[4096];
    uint32_t n, i;
    if (!out || size < 2) { if (out && size) out[0] = 0; return 0; }
    if (sect) w2c(sect, s, sizeof s);
    if (file) w2c(file, f, sizeof f);
    n = st_GetPrivateProfileSectionA(sect ? s : NULL, nb, sizeof nb, file ? f : NULL);
    for (i = 0; i < n && i + 1 < size; i++) out[i] = (uint8_t)nb[i];
    out[i] = 0;
    if (i + 1 < size) out[i + 1] = 0;
    return i;
}

/* The write side. An .ini is small enough to rewrite whole, which is also what
 * makes the semantics easy to get right: a NULL value deletes the key, a NULL
 * key deletes the section, and a key in a section that does not exist yet
 * appends both. */
typedef struct { char *buf; size_t len, cap; } ini_buf;
static void ib_add(ini_buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    char tmp[1200];
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (b->len + (size_t)n + 1 > b->cap) {
        size_t want = (b->len + (size_t)n + 1) * 2;
        char *p = (char *)realloc(b->buf, want);
        if (!p) return;
        b->buf = p; b->cap = want;
    }
    memcpy(b->buf + b->len, tmp, (size_t)n);
    b->len += (size_t)n;
    b->buf[b->len] = 0;
}

static int ini_write(const char *path, const char *sect, const char *key,
                     const char *val)
{
    FILE *f;
    char line[1024], cur[128];
    ini_buf out;
    int in_target = 0, wrote = 0, seen_section = 0;

    if (!sect) return 0;
    out.buf = NULL; out.len = 0; out.cap = 0;
    cur[0] = 0;

    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof line, f)) {
            char trimmed[1024], *s = line, *eq;
            size_t L;
            snprintf(trimmed, sizeof trimmed, "%s", line);
            L = strlen(trimmed);
            while (L && (trimmed[L-1] == '\n' || trimmed[L-1] == '\r')) trimmed[--L] = 0;
            s = trimmed;
            while (*s == ' ' || *s == '\t') s++;

            if (*s == '[') {
                char *close = strchr(s, ']');
                /* leaving the target section: append a key that was never found */
                if (in_target && key && val && !wrote) {
                    ib_add(&out, "%s=%s\n", key, val);
                    wrote = 1;
                }
                in_target = 0;
                if (close) {
                    char nm[128];
                    *close = 0;
                    snprintf(nm, sizeof nm, "%s", s + 1);
                    *close = ']';
                    snprintf(cur, sizeof cur, "%s", nm);
                    if (!strcasecmp(cur, sect)) { in_target = 1; seen_section = 1; }
                }
                /* a NULL key deletes the whole section, header included */
                if (in_target && !key) continue;
                ib_add(&out, "%s\n", trimmed);
                continue;
            }
            if (in_target && !key) continue;                  /* dropping section */
            if (in_target && key && (eq = strchr(s, '=')) != NULL) {
                char k[256];
                size_t kl = (size_t)(eq - s);
                if (kl >= sizeof k) kl = sizeof k - 1;
                memcpy(k, s, kl); k[kl] = 0;
                while (kl && (k[kl-1] == ' ' || k[kl-1] == '\t')) k[--kl] = 0;
                if (!strcasecmp(k, key)) {
                    if (val) { ib_add(&out, "%s=%s\n", key, val); wrote = 1; }
                    else wrote = 1;                            /* NULL value deletes */
                    continue;
                }
            }
            ib_add(&out, "%s\n", trimmed);
        }
        fclose(f);
    }
    /* the target section was the last in the file, or was not there at all */
    if (key && val && !wrote) {
        if (!seen_section) ib_add(&out, "[%s]\n", sect);
        ib_add(&out, "%s=%s\n", key, val);
    }

    {
        FILE *w = fopen(path, "w");
        if (!w) {
            make_parents_in_root(path);
            w = fopen(path, "w");
        }
        if (!w) { free(out.buf); return 0; }
        if (out.buf && out.len) fwrite(out.buf, 1, out.len, w);
        fclose(w);
    }
    free(out.buf);
    return 1;
}

static MS int32_t st_WritePrivateProfileStringA(const char *sect, const char *key,
                                                const char *val, const char *file)
{
    char path[1024];
    if (!ini_path(file, path, sizeof path)) return 0;
    return ini_write(path, sect, key, val);
}
static MS int32_t st_WritePrivateProfileStringW(const uint16_t *sect, const uint16_t *key,
                                                const uint16_t *val, const uint16_t *file)
{
    char s[256], k[256], v[1024], f[1024];
    if (sect) w2c(sect, s, sizeof s);
    if (key)  w2c(key, k, sizeof k);
    if (val)  w2c(val, v, sizeof v);
    if (file) w2c(file, f, sizeof f);
    return st_WritePrivateProfileStringA(sect ? s : NULL, key ? k : NULL,
                                         val ? v : NULL, file ? f : NULL);
}

/* WritePrivateProfileSection replaces a section outright, taking the same
 * double-NUL-terminated "key=value" run GetPrivateProfileSection hands back. */
static MS int32_t st_WritePrivateProfileSectionA(const char *sect, const char *data,
                                                 const char *file)
{
    char path[1024];
    const char *p;
    if (!ini_path(file, path, sizeof path) || !sect) return 0;
    ini_write(path, sect, NULL, NULL);           /* drop what was there */
    if (!data) return 1;
    {   /* recreate the header, then every pair in turn */
        FILE *w = fopen(path, "a");
        if (!w) return 0;
        fprintf(w, "[%s]\n", sect);
        fclose(w);
    }
    for (p = data; *p; p += strlen(p) + 1) {
        char k[256], *eq;
        snprintf(k, sizeof k, "%s", p);
        if ((eq = strchr(k, '=')) == NULL) continue;
        *eq = 0;
        st_WritePrivateProfileStringA(sect, k, eq + 1, file);
    }
    return 1;
}
static MS int32_t st_WritePrivateProfileSectionW(const uint16_t *sect,
                                                 const uint16_t *data,
                                                 const uint16_t *file)
{
    char s[256], f[1024], nb[4096];
    size_t i = 0, o = 0;
    if (sect) w2c(sect, s, sizeof s);
    if (file) w2c(file, f, sizeof f);
    if (data) {   /* the run is double-NUL terminated, so copy it as one block */
        while (o + 1 < sizeof nb) {
            if (!data[i] && !data[i + 1]) break;
            nb[o++] = data[i] < 256 ? (char)data[i] : '?';
            i++;
        }
    }
    nb[o] = 0; if (o + 1 < sizeof nb) nb[o + 1] = 0;
    return st_WritePrivateProfileSectionA(sect ? s : NULL, data ? nb : NULL,
                                          file ? f : NULL);
}


/* ------------------------------------------------- Wine's debug plumbing ---

 * Only reached when a *Wine* build of a runtime DLL is loaded as a real
 * dependency, which is the redistributable way to give the i386 loader a C++
 * standard library: Microsoft's msvcp120 may not be shipped with anything,
 * Wine's may. Wine compiles its TRACE/WARN/ERR macros down to these four
 * ntdll entry points, so its msvcp120 imports them even in a build that never
 * logs anything, and without them every one of those imports took the generic
 * stub -- which is how four plugins that had got as far as loading the whole
 * C++ runtime still died, now at a Wine debug call rather than in their own
 * code.
 *
 * All four are __cdecl in Wine, not stdcall, so they are declared MSCRT.
 * Answering "this channel is switched off" is both the cheapest and the most
 * faithful thing to do: a plugin is not asking to be traced, and Wine's own
 * default is every channel off. */

/* struct __wine_debug_channel is { unsigned char flags; char name[15]; } --
 * only the flags byte is read here, and only to say nothing is enabled. */
static MSCRT int st___wine_dbg_get_channel_flags(void *channel)
{ (void)channel; return 0; }

/* Returns nonzero to suppress the message. Wine's TRACE expands to
 * `if (!__wine_dbg_header(...)) ...`, so 0 here would ask for the body to run
 * and then hand the text to __wine_dbg_output below. */
static MSCRT int st___wine_dbg_header(int cls, void *channel, const char *func)
{ (void)cls; (void)channel; (void)func; return -1; }

/* Reached only if something logs despite the above -- an ERR channel, which
 * Wine leaves on by default. Worth seeing rather than dropping: it is the
 * runtime explaining itself. */
static MSCRT int st___wine_dbg_output(const char *str)
{
    if (!str) return 0;
    if (pe_verbose()) fputs(str, stderr);
    return (int)strlen(str);
}

/* Wine hands the result to a printf that runs after the caller returns, so it
 * has to outlive this call. Wine's own implementation uses a per-thread ring
 * of buffers for exactly this; the same shape here, one buffer set per thread,
 * so nothing is freed and nothing is shared between threads. */
static MSCRT const char *st___wine_dbg_strdup(const char *str)
{
    enum { RING = 32, CELL = 256 };
    static __thread char ring[RING][CELL];
    static __thread unsigned next;
    char *slot;
    if (!str) return NULL;
    slot = ring[next++ % RING];
    snprintf(slot, CELL, "%s", str);
    return slot;
}

#ifndef PELOAD_NO_GUI_LAYER
/* CoCreateInstance, for the one class this host can actually make.
 *
 * Everything else is refused by name rather than silently: a plug-in that
 * asked for something unavailable gets REGDB_E_CLASSNOTREG, which is what
 * Windows says when a class is not registered and is a condition callers
 * already handle. */
static void *wic_factory(void);
static MS int32_t st_CoCreateInstance(const void *rclsid, void *outer, uint32_t ctx,
                                      const void *riid, void **ppv)
{
    /* CLSID_WICImagingFactory {CACAF262-9370-4615-A13B-9F5539DA4C0A} and the
     * _2 spelling {317D06E8-5F24-433D-BDF7-79CE68D8ABC2}, which is the same
     * object with a longer vtable. */
    static const uint8_t wic1[16] = {
        0x62,0xF2,0xCA,0xCA, 0x70,0x93, 0x15,0x46,
        0xA1,0x3B, 0x9F,0x55,0x39,0xDA,0x4C,0x0A };
    static const uint8_t wic2[16] = {
        0xE8,0x06,0x7D,0x31, 0x24,0x5F, 0x3D,0x43,
        0xBD,0xF7, 0x79,0xCE,0x68,0xD8,0xAB,0xC2 };
    (void)outer; (void)ctx; (void)riid;
    if (!ppv) return (int32_t)0x80070057;                    /* E_INVALIDARG */
    *ppv = NULL;
    if (rclsid && (!memcmp(rclsid, wic1, 16) || !memcmp(rclsid, wic2, 16))) {
        *ppv = wic_factory();
        PLOG("  [win] CoCreateInstance(WICImagingFactory) -> %p\n", *ppv);
        return *ppv ? 0 : (int32_t)0x80004005;
    }
    PLOG("  [win] CoCreateInstance(<unknown class>) -> REGDB_E_CLASSNOTREG\n");
    return (int32_t)0x80040154;                              /* REGDB_E_CLASSNOTREG */
}
#endif

static const winstub g_stubs[] = {
    /* CRT: registered against msvcrt.dll and reached from every versioned
     * runtime name through crt_alias() below */
    S("msvcrt.dll", _initterm), S("msvcrt.dll", _initterm_e),
    S("msvcrt.dll", malloc), S("msvcrt.dll", calloc),
    S("msvcrt.dll", realloc), S("msvcrt.dll", free),
    S("msvcrt.dll", _recalloc), S("msvcrt.dll", _msize),
    /* the CRT-internal allocator, same implementation */
#if defined(__x86_64__)
    /* The MSVC C++ library, natively -- see msvcp_shim.h for how each layout
     * was derived. Registered under msvcrt.dll because crt_alias maps every
     * msvcp/msvcr/vcruntime spelling onto it, so one set of entries answers
     * msvcp120, msvcp140 and msvcp140_1 alike. These are the fallback: the
     * import resolver still prefers a real msvcp wherever one is installed. */
    { "msvcrt.dll", "_Mtx_init_in_situ",    (void *)mp_Mtx_init_in_situ },
    { "msvcrt.dll", "_Mtx_destroy_in_situ", (void *)mp_Mtx_destroy_in_situ },
    { "msvcrt.dll", "_Mtx_init",            (void *)mp_Mtx_init },
    { "msvcrt.dll", "_Mtx_destroy",         (void *)mp_Mtx_destroy },
    { "msvcrt.dll", "_Mtx_lock",            (void *)mp_Mtx_lock },
    { "msvcrt.dll", "_Mtx_unlock",          (void *)mp_Mtx_unlock },
    { "msvcrt.dll", "_Cnd_init_in_situ",    (void *)mp_Cnd_init_in_situ },
    { "msvcrt.dll", "_Cnd_destroy_in_situ", (void *)mp_Cnd_destroy_in_situ },
    { "msvcrt.dll", "_Cnd_init",            (void *)mp_Cnd_init },
    { "msvcrt.dll", "_Cnd_destroy",         (void *)mp_Cnd_destroy },
    { "msvcrt.dll", "_Cnd_wait",            (void *)mp_Cnd_wait },
    { "msvcrt.dll", "_Cnd_signal",          (void *)mp_Cnd_signal },
    { "msvcrt.dll", "_Cnd_broadcast",       (void *)mp_Cnd_broadcast },
    { "msvcrt.dll", "_Mtx_trylock",         (void *)mp_Mtx_trylock },
    { "msvcrt.dll", "_Mtx_timedlock",       (void *)mp_Mtx_timedlock },
    { "msvcrt.dll", "_Mtx_current_owns",    (void *)mp_Mtx_current_owns },
    { "msvcrt.dll", "_Mtx_clear_owner",     (void *)mp_Mtx_clear_owner },
    { "msvcrt.dll", "_Mtx_reset_owner",     (void *)mp_Mtx_reset_owner },
    { "msvcrt.dll", "_Mtx_getconcrtcs",     (void *)mp_Mtx_getconcrtcs },
    { "msvcrt.dll", "_Cnd_timedwait",       (void *)mp_Cnd_timedwait },
    { "msvcrt.dll", "_Cnd_register_at_thread_exit",
                                            (void *)mp_Cnd_register_at_thread_exit },
    { "msvcrt.dll", "_Cnd_unregister_at_thread_exit",
                                            (void *)mp_Cnd_unregister_at_thread_exit },
    { "msvcrt.dll", "_Cnd_do_broadcast_at_thread_exit",
                                            (void *)mp_Cnd_do_broadcast_at_thread_exit },
    { "msvcrt.dll", "_Thrd_id",             (void *)mp_Thrd_id },
    { "msvcrt.dll", "_Thrd_create",         (void *)mp_Thrd_create },
    { "msvcrt.dll", "_Thrd_start",          (void *)mp_Thrd_start },
    { "msvcrt.dll", "_Thrd_current",        (void *)mp_Thrd_current },
    { "msvcrt.dll", "_Thrd_equal",          (void *)mp_Thrd_equal },
    { "msvcrt.dll", "_Thrd_lt",             (void *)mp_Thrd_lt },
    { "msvcrt.dll", "_Thrd_join",           (void *)mp_Thrd_join },
    { "msvcrt.dll", "_Thrd_detach",         (void *)mp_Thrd_detach },
    { "msvcrt.dll", "_Thrd_exit",           (void *)mp_Thrd_exit },
    { "msvcrt.dll", "_Thrd_yield",          (void *)mp_Thrd_yield },
    { "msvcrt.dll", "_Thrd_sleep",          (void *)mp_Thrd_sleep },
    { "msvcrt.dll", "_Query_perf_counter",  (void *)mp_Query_perf_counter },
    { "msvcrt.dll", "_Query_perf_frequency",(void *)mp_Query_perf_frequency },
    { "msvcrt.dll", "_Dtest",               (void *)mp_Dtest },
    { "msvcrt.dll", "_Wcscoll",             (void *)mp_Wcscoll },
    { "msvcrt.dll", "_Wcsxfrm",             (void *)mp_Wcsxfrm },
    { "msvcrt.dll", "?_Init@locale@std@@CAPEAV_Locimp@12@_N@Z",
                                            (void *)mp_locale_Init },
    { "msvcrt.dll", "?_Getgloballocale@locale@std@@CAPEAV_Locimp@12@XZ",
                                            (void *)mp_Getgloballocale },
    { "msvcrt.dll", "?_New_Locimp@_Locimp@locale@std@@CAPEAV123@AEBV123@@Z",
                                            (void *)mp_New_Locimp_copy },
    { "msvcrt.dll", "?_New_Locimp@_Locimp@locale@std@@CAPEAV123@_N@Z",
                                            (void *)mp_New_Locimp_b },
    { "msvcrt.dll", "?_Locimp_Addfac@_Locimp@locale@std@@CAXPEAV123@PEAVfacet@23@_K@Z",
                                            (void *)mp_Locimp_Addfac },
    { "msvcrt.dll", "??Bid@locale@std@@QEAA_KXZ", (void *)mp_id_value },
    { "msvcrt.dll", "??0_Lockit@std@@QEAA@H@Z",   (void *)mp_Lockit_ctor },
    { "msvcrt.dll", "??1_Lockit@std@@QEAA@XZ",    (void *)mp_Lockit_dtor },
    { "msvcrt.dll", "??0facet@locale@std@@IEAA@_K@Z", (void *)mp_facet_ctor },
    { "msvcrt.dll", "??1facet@locale@std@@MEAA@XZ",   (void *)mp_facet_dtor_plain },
    { "msvcrt.dll", "?_Incref@facet@locale@std@@UEAAXXZ", (void *)mp_facet_Incref },
    { "msvcrt.dll", "?_Decref@facet@locale@std@@UEAAPEAV_Facet_base@3@XZ",
                                            (void *)mp_facet_Decref },
    { "msvcrt.dll", "??0?$codecvt@_WDU_Mbstatet@@@std@@QEAA@_K@Z",
                                            (void *)mp_codecvt_ctor },
    { "msvcrt.dll", "??1?$codecvt@_WDU_Mbstatet@@@std@@MEAA@XZ",
                                            (void *)mp_facet_dtor_plain },
    { "msvcrt.dll", "?_Xbad_alloc@std@@YAXXZ",        (void *)mp_Xbad_alloc },
    { "msvcrt.dll", "?_Xlength_error@std@@YAXPEBD@Z", (void *)mp_Xlength_error },
    { "msvcrt.dll", "?_Xout_of_range@std@@YAXPEBD@Z", (void *)mp_Xout_of_range },
    { "msvcrt.dll", "?_Xinvalid_argument@std@@YAXPEBD@Z",
                                            (void *)mp_Xinvalid_argument },
    { "msvcrt.dll", "?_Xbad_function_call@std@@YAXXZ",
                                            (void *)mp_Xbad_function_call },
    { "msvcrt.dll", "?_Xregex_error@std@@YAXW4error_type@regex_constants@1@@Z",
                                            (void *)mp_Xregex_error },
    { "msvcrt.dll", "?_Throw_C_error@std@@YAXH@Z",   (void *)mp_Throw_C_error },
    { "msvcrt.dll", "?_Throw_Cpp_error@std@@YAXH@Z", (void *)mp_Throw_Cpp_error },
    { "msvcrt.dll", "?uncaught_exception@std@@YA_NXZ",
                                            (void *)mp_uncaught_exception },
    { "msvcrt.dll", "?__ExceptionPtrCreate@@YAXPEAX@Z",
                                            (void *)mp_ExceptionPtrCreate },
    { "msvcrt.dll", "?__ExceptionPtrDestroy@@YAXPEAX@Z",
                                            (void *)mp_ExceptionPtrDestroy },
    { "msvcrt.dll", "?__ExceptionPtrCurrentException@@YAXPEAX@Z",
                                            (void *)mp_ExceptionPtrCurrentException },
    { "msvcrt.dll", "?__ExceptionPtrRethrow@@YAXPEBX@Z",
                                            (void *)mp_ExceptionPtrRethrow },
    /* A data export: the IAT slot holds the address, and the plug-in reads
     * through it. */
    { "msvcrt.dll", "?_BADOFF@std@@3_JB",   (void *)&mp_BADOFF },
    /* The eight entry points the Native Instruments plug-ins reach in
     * MSVCP120: the container debug base, which folds to almost nothing in a
     * release build, and just enough of the iostream tier to construct the
     * objects they build. See msvcp_shim.h for where each layout came from. */
    { "msvcrt.dll", "??0_Container_base12@std@@QEAA@XZ",      (void *)mp_cb_ctor },
    { "msvcrt.dll", "??0_Container_base12@std@@QEAA@AEBU01@@Z", (void *)mp_cb_ctor },
    { "msvcrt.dll", "??1_Container_base12@std@@QEAA@XZ",      (void *)mp_cb_noop },
    { "msvcrt.dll", "?_Orphan_all@_Container_base12@std@@QEAAXXZ", (void *)mp_cb_noop },
    { "msvcrt.dll", "?_Getpfirst@_Container_base12@std@@QEBAPEAPEAU_Iterator_base12@2@XZ",
                                                              (void *)mp_cb_getpfirst },
    { "msvcrt.dll", "?_Swap_all@_Container_base12@std@@QEAAXAEAU12@@Z",
                                                              (void *)mp_cb_swap },
    { "msvcrt.dll", "?_Swap_all@_Container_base0@std@@QEAAXAEAU12@@Z",
                                                              (void *)mp_cb_noop },
    { "msvcrt.dll", "??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAA@XZ",
                                                              (void *)mp_streambuf_ctor },
    { "msvcrt.dll", "??0?$basic_ios@DU?$char_traits@D@std@@@std@@IEAA@XZ",
                                                              (void *)mp_basic_ios_ctor },
    { "msvcrt.dll", "??0?$basic_iostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@@Z",
                                                              (void *)mp_iostream_ctor },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAVios_base@1@AEAV21@@Z@Z",
                                                              (void *)mp_ostream_manip },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEAI@Z",
                                                              (void *)mp_ist_uint },
    { "msvcrt.dll", "?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAAXH_N@Z",
                                                              (void *)mp_ios_setstate },
    { "msvcrt.dll", "?clear@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAAXH_N@Z",
                                                              (void *)mp_ios_clear },
    { "msvcrt.dll", "?rdstate@ios_base@std@@QEBAHXZ",         (void *)mp_ios_rdstate },
    { "msvcrt.dll", "?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAXXZ",
                                                              (void *)mp_ostream_osfx },
    { "msvcrt.dll", "?_Lock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ",
                                                              (void *)mp_sb_lock },
    { "msvcrt.dll", "?_Unlock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ",
                                                              (void *)mp_sb_unlock },
    { "msvcrt.dll", "??1?$basic_ios@DU?$char_traits@D@std@@@std@@UEAA@XZ",
                                                              (void *)mp_stream_dtor },
    { "msvcrt.dll", "??1?$basic_iostream@DU?$char_traits@D@std@@@std@@UEAA@XZ",
                                                              (void *)mp_stream_dtor },
    { "msvcrt.dll", "??1?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAA@XZ",
                                                              (void *)mp_stream_dtor },
    { "msvcrt.dll", "??0?$codecvt@_WDH@std@@QEAA@_K@Z",       (void *)mp_codecvt_ctor },
    { "msvcrt.dll", "_Xtime_get_ticks",                       (void *)mp_Xtime_get_ticks },
    { "msvcrt.dll", "?setg@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z",
                                                              (void *)mp_sb_setg },
    { "msvcrt.dll", "?setp@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD0@Z",
                                                              (void *)mp_sb_setp },
    { "msvcrt.dll", "?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JPEBD_J@Z",
                                                              (void *)mp_sb_sputn },
    { "msvcrt.dll", "?sgetn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JPEAD_J@Z",
                                                              (void *)mp_sb_sgetn },
    { "msvcrt.dll", "?sbumpc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ",
                                                              (void *)mp_sb_sbumpc },
    { "msvcrt.dll", "?sgetc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ",
                                                              (void *)mp_sb_sgetc },
    { "msvcrt.dll", "?snextc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ",
                                                              (void *)mp_sb_snextc },
    { "msvcrt.dll", "?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHD@Z",
                                                              (void *)mp_sb_sputc },
    { "msvcrt.dll", "?pubsync@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ",
                                                              (void *)mp_sb_pubsync },
    { "msvcrt.dll", "?_Orphan_all@_Container_base0@std@@QEAAXXZ", (void *)mp_cb_noop },
    /* The protected virtuals, which are exported and can be called directly --
     * a derived streambuf calls its base's xsputn rather than going through the
     * vftable. Left as tracking stubs, xsputn returns "wrote nothing", and a
     * caller looping until it has written everything never finishes. That was
     * the hang. */
    { "msvcrt.dll", "?xsputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEBD_J@Z",  (void *)mp_sb_v_xsputn },
    { "msvcrt.dll", "?xsgetn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEAD_J@Z",  (void *)mp_sb_v_xsgetn },
    { "msvcrt.dll", "?overflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHH@Z",      (void *)mp_sb_v_overflow },
    { "msvcrt.dll", "?pbackfail@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHH@Z",     (void *)mp_sb_v_pbackfail },
    { "msvcrt.dll", "?underflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ",      (void *)mp_sb_v_underflow },
    { "msvcrt.dll", "?uflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ",          (void *)mp_sb_v_uflow },
    { "msvcrt.dll", "?showmanyc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JXZ",     (void *)mp_sb_v_showmanyc },
    { "msvcrt.dll", "?sync@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ",           (void *)mp_sb_v_sync },
    { "msvcrt.dll", "?setbuf@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAPEAV12@PEAD_J@Z", (void *)mp_sb_v_setbuf },
    { "msvcrt.dll", "?imbue@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAXAEBVlocale@2@@Z", (void *)mp_sb_v_imbue },
    { "msvcrt.dll", "?in_avail@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JXZ",      (void *)mp_sb_in_avail },
    { "msvcrt.dll", "?sungetc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ",        (void *)mp_sb_sungetc },
    { "msvcrt.dll", "?pubsetbuf@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAPEAV12@PEAD_J@Z", (void *)mp_sb_pubsetbuf },
    { "msvcrt.dll", "?pubimbue@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA?AVlocale@2@AEBV32@@Z", (void *)mp_sb_pubimbue },
    { "msvcrt.dll", "?pubseekoff@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA?AV?$fpos@H@2@_JHH@Z", (void *)mp_sb_pubseekoff },
    { "msvcrt.dll", "?pubseekoff@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA?AV?$fpos@H@2@_JII@Z", (void *)mp_sb_pubseekoff },
    { "msvcrt.dll", "?getloc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEBA?AVlocale@2@XZ", (void *)mp_sb_getloc },
    /* The protected accessors. A derived streambuf walks its own buffer
     * through these, so each has to agree with setg and setp about where the
     * pointers and the counts live. */
    { "msvcrt.dll", "?eback@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ",   (void *)mp_sb_eback },
    { "msvcrt.dll", "?gptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ",    (void *)mp_sb_gptr_pub },
    { "msvcrt.dll", "?egptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ",   (void *)mp_sb_egptr },
    { "msvcrt.dll", "?pbase@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ",   (void *)mp_sb_pbase },
    { "msvcrt.dll", "?pptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ",    (void *)mp_sb_pptr_pub },
    { "msvcrt.dll", "?epptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ",   (void *)mp_sb_epptr },
    { "msvcrt.dll", "?gbump@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXH@Z",     (void *)mp_sb_gbump },
    { "msvcrt.dll", "?pbump@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXH@Z",     (void *)mp_sb_pbump },
    { "msvcrt.dll", "?_Gninc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAPEADXZ",  (void *)mp_sb_gninc },
    { "msvcrt.dll", "?_Gnpreinc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAPEADXZ", (void *)mp_sb_gnpreinc },
    { "msvcrt.dll", "?_Gndec@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAPEADXZ",  (void *)mp_sb_gndec },
    { "msvcrt.dll", "?_Pninc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAPEADXZ",  (void *)mp_sb_pninc },
    { "msvcrt.dll", "??0?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEAA@XZ",            (void *)mp_wstreambuf_ctor },
    { "msvcrt.dll", "?setg@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEAAXPEA_W00@Z",  (void *)mp_wsb_setg },
    { "msvcrt.dll", "?setp@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEAAXPEA_W0@Z",   (void *)mp_wsb_setp },
    { "msvcrt.dll", "?setp@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z",
                                                              (void *)mp_sb_setp3 },
    { "msvcrt.dll", "?setp@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEAAXPEA_W00@Z",
                                                              (void *)mp_wsb_setp3 },
    /* The VS2015 spellings of the same facets, and the rest of the
     * istream surface. msvcp140 renames the codecvt state type and the
     * fpos it is built on; the implementations are shared. */
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@F@Z",
                                                          (void *)mp_ost_short },
    { "msvcrt.dll", "?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z",
                                                          (void *)mp_Fiopen_a },
    { "msvcrt.dll", "?_Ipfx@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA_N_N@Z",
                                                          (void *)mp_ist_ipfx },
    { "msvcrt.dll", "?get@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAHXZ",
                                                          (void *)mp_ist_get1 },
    { "msvcrt.dll", "?peek@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAHXZ",
                                                          (void *)mp_ist_peek },
    { "msvcrt.dll", "?ignore@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@_JH@Z",
                                                          (void *)mp_ist_ignore },
    { "msvcrt.dll", "?seekg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@_JH@Z",
                                                          (void *)mp_ist_seekg_dir },
    { "msvcrt.dll", "?seekg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@V?$fpos@U_Mbstatet@@@2@@Z",
                                                          (void *)mp_ist_seekg },
    { "msvcrt.dll", "?tellg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA?AV?$fpos@U_Mbstatet@@@2@XZ",
                                                          (void *)mp_ist_tellg },
    { "msvcrt.dll", "_Getcoll",
                                                          (void *)mp_Getcoll },
    { "msvcrt.dll", "?id@?$collate@_W@std@@2V0locale@2@A",
                                                          (void *)&mp_collate_w_id_value },
    { "msvcrt.dll", "?tolower@?$ctype@_W@std@@QEBAPEB_WPEA_WPEB_W@Z",
                                                          (void *)mp_ctw_tolower_p },
    { "msvcrt.dll", "?toupper@?$ctype@_W@std@@QEBAPEB_WPEA_WPEB_W@Z",
                                                          (void *)mp_ctw_toupper_p },
    { "msvcrt.dll", "?_Getcat@?$codecvt@DDU_Mbstatet@@@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z",
                                                          (void *)mp_cvtc_Getcat },
    { "msvcrt.dll", "?_Getcat@?$codecvt@_WDU_Mbstatet@@@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z",
                                                          (void *)mp_cvtw_Getcat },
    { "msvcrt.dll", "?id@?$codecvt@DDU_Mbstatet@@@std@@2V0locale@2@A",
                                                          (void *)&mp_cvtc_id_value },
    { "msvcrt.dll", "?id@?$codecvt@_WDU_Mbstatet@@@std@@2V0locale@2@A",
                                                          (void *)&mp_cvtw_id_value },
    { "msvcrt.dll", "?in@?$codecvt@DDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEBD1AEAPEBDPEAD3AEAPEAD@Z",
                                                          (void *)mp_cvtc_in },
    { "msvcrt.dll", "?out@?$codecvt@DDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEBD1AEAPEBDPEAD3AEAPEAD@Z",
                                                          (void *)mp_cvtc_out },
    { "msvcrt.dll", "?unshift@?$codecvt@DDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEAD1AEAPEAD@Z",
                                                          (void *)mp_cvtc_unshift },
    { "msvcrt.dll", "?in@?$codecvt@_WDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEBD1AEAPEBDPEA_W3AEAPEA_W@Z",
                                                          (void *)mp_cvtw_in },
    { "msvcrt.dll", "?out@?$codecvt@_WDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEB_W1AEAPEB_WPEAD3AEAPEAD@Z",
                                                          (void *)mp_cvtw_out },
    { "msvcrt.dll", "?unshift@?$codecvt@_WDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEAD1AEAPEAD@Z",
                                                          (void *)mp_cvtw_unshift },
    /* _Yarn, the locale's owned string -- see msvcp_shim.h. */
    { "msvcrt.dll", "??0?$_Yarn@D@std@@QEAA@XZ",              (void *)mp_yarn_ctor },
    { "msvcrt.dll", "??0?$_Yarn@_W@std@@QEAA@XZ",             (void *)mp_yarn_ctor },
    { "msvcrt.dll", "??0?$_Yarn@D@std@@QEAA@PEBD@Z",          (void *)mp_yarn_ctor_s },
    { "msvcrt.dll", "??0?$_Yarn@D@std@@QEAA@AEBV01@@Z",       (void *)mp_yarn_ctor_copy },
    { "msvcrt.dll", "??1?$_Yarn@D@std@@QEAA@XZ",              (void *)mp_yarn_tidy },
    { "msvcrt.dll", "??1?$_Yarn@_W@std@@QEAA@XZ",             (void *)mp_yarn_tidy },
    { "msvcrt.dll", "?_Tidy@?$_Yarn@D@std@@AEAAXXZ",          (void *)mp_yarn_tidy },
    { "msvcrt.dll", "?_Tidy@?$_Yarn@_W@std@@AEAAXXZ",         (void *)mp_yarn_tidy },
    { "msvcrt.dll", "??4?$_Yarn@D@std@@QEAAAEAV01@PEBD@Z",    (void *)mp_yarn_assign },
    { "msvcrt.dll", "??4?$_Yarn@D@std@@QEAAAEAV01@AEBV01@@Z", (void *)mp_yarn_assign_y },
    { "msvcrt.dll", "??4?$_Yarn@_W@std@@QEAAAEAV01@PEB_W@Z",  (void *)mp_yarn_assign_w },
    { "msvcrt.dll", "?_C_str@?$_Yarn@D@std@@QEBAPEBDXZ",      (void *)mp_yarn_cstr },
    { "msvcrt.dll", "?_C_str@?$_Yarn@_W@std@@QEBAPEB_WXZ",    (void *)mp_yarn_cstr },
    { "msvcrt.dll", "?c_str@?$_Yarn@D@std@@QEBAPEBDXZ",       (void *)mp_yarn_cstr },
    { "msvcrt.dll", "?_Empty@?$_Yarn@D@std@@QEBA_NXZ",        (void *)mp_yarn_empty },
    { "msvcrt.dll", "?_Empty@?$_Yarn@_W@std@@QEBA_NXZ",       (void *)mp_yarn_empty },
    { "msvcrt.dll", "?empty@?$_Yarn@D@std@@QEBA_NXZ",         (void *)mp_yarn_empty },
    /* The remaining msvcp120 surface: codecvt, _Locinfo, the classic
     * locale, the istream extractors and the standard streams. */
    { "msvcrt.dll", "xtime_get",
                                                          (void *)mp_xtime_get },
    { "msvcrt.dll", "_Xtime_diff_to_millis2",
                                                          (void *)mp_Xtime_diff_to_millis2 },
    { "msvcrt.dll", "_Getcvt",
                                                          (void *)mp_Getcvt },
    { "msvcrt.dll", "_FDtest",
                                                          (void *)mp_FDtest },
    { "msvcrt.dll", "_Exp",
                                                          (void *)mp_Exp },
    { "msvcrt.dll", "_FExp",
                                                          (void *)mp_FExp },
    { "msvcrt.dll", "_Inf",
                                                          (void *)&mp_Inf_v },
    { "msvcrt.dll", "_Nan",
                                                          (void *)&mp_Nan_v },
    { "msvcrt.dll", "_FInf",
                                                          (void *)&mp_FInf_v },
    { "msvcrt.dll", "_FNan",
                                                          (void *)&mp_FNan_v },
    { "msvcrt.dll", "?_Syserror_map@std@@YAPEBDH@Z",
                                                          (void *)mp_Syserror_map },
    { "msvcrt.dll", "?_Winerror_map@std@@YAPEBDH@Z",
                                                          (void *)mp_Winerror_map },
    { "msvcrt.dll", "?_Future_error_map@std@@YAPEBDH@Z",
                                                          (void *)mp_Future_error_map },
    { "msvcrt.dll", "?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z",
                                                          (void *)mp_Fiopen },
    { "msvcrt.dll", "?classic@locale@std@@SAAEBV12@XZ",
                                                          (void *)mp_locale_classic },
    { "msvcrt.dll", "??0_Locinfo@std@@QEAA@PEBD@Z",
                                                          (void *)mp_locinfo_ctor },
    { "msvcrt.dll", "??1_Locinfo@std@@QEAA@XZ",
                                                          (void *)mp_locinfo_dtor },
    { "msvcrt.dll", "?_Gettrue@_Locinfo@std@@QEBAPEBDXZ",
                                                          (void *)mp_locinfo_gettrue },
    { "msvcrt.dll", "?_Getfalse@_Locinfo@std@@QEBAPEBDXZ",
                                                          (void *)mp_locinfo_getfalse },
    { "msvcrt.dll", "?_Getcat@?$codecvt@DDH@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z",
                                                          (void *)mp_cvtc_Getcat },
    { "msvcrt.dll", "?_Getcat@?$codecvt@_WDH@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z",
                                                          (void *)mp_cvtw_Getcat },
    { "msvcrt.dll", "?id@?$codecvt@DDH@std@@2V0locale@2@A",
                                                          (void *)&mp_cvtc_id_value },
    { "msvcrt.dll", "?id@?$codecvt@_WDH@std@@2V0locale@2@A",
                                                          (void *)&mp_cvtw_id_value },
    { "msvcrt.dll", "?id@?$ctype@_W@std@@2V0locale@2@A",
                                                          (void *)&mp_ctypew_id_value },
    { "msvcrt.dll", "?id@?$numpunct@D@std@@2V0locale@2@A",
                                                          (void *)&mp_numpunct_id_value },
    { "msvcrt.dll", "?in@?$codecvt@DDH@std@@QEBAHAEAHPEBD1AEAPEBDPEAD3AEAPEAD@Z",
                                                          (void *)mp_cvtc_in },
    { "msvcrt.dll", "?out@?$codecvt@DDH@std@@QEBAHAEAHPEBD1AEAPEBDPEAD3AEAPEAD@Z",
                                                          (void *)mp_cvtc_out },
    { "msvcrt.dll", "?unshift@?$codecvt@DDH@std@@QEBAHAEAHPEAD1AEAPEAD@Z",
                                                          (void *)mp_cvtc_unshift },
    { "msvcrt.dll", "?in@?$codecvt@_WDH@std@@QEBAHAEAHPEBD1AEAPEBDPEA_W3AEAPEA_W@Z",
                                                          (void *)mp_cvtw_in },
    { "msvcrt.dll", "?out@?$codecvt@_WDH@std@@QEBAHAEAHPEB_W1AEAPEB_WPEAD3AEAPEAD@Z",
                                                          (void *)mp_cvtw_out },
    { "msvcrt.dll", "?unshift@?$codecvt@_WDH@std@@QEBAHAEAHPEAD1AEAPEAD@Z",
                                                          (void *)mp_cvtw_unshift },
    { "msvcrt.dll", "?do_length@?$codecvt@_WDH@std@@MEBAHAEAHPEBD1_K@Z",
                                                          (void *)mp_cvtw_length },
    { "msvcrt.dll", "?do_length@?$codecvt@DDH@std@@MEBAHAEAHPEBD1_K@Z",
                                                          (void *)mp_cvtc_length },
    { "msvcrt.dll", "?always_noconv@codecvt_base@std@@QEBA_NXZ",
                                                          (void *)mp_cvtc_always_noconv },
    { "msvcrt.dll", "??_7ios_base@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7?$basic_ios@DU?$char_traits@D@std@@@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7?$basic_ios@_WU?$char_traits@_W@std@@@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7?$basic_ostream@DU?$char_traits@D@std@@@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7?$basic_ostream@_WU?$char_traits@_W@std@@@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7?$basic_istream@DU?$char_traits@D@std@@@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7facet@locale@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7_Facet_base@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7codecvt_base@std@@6B@",
                                                          (void *)mp_facet_vft },
    { "msvcrt.dll", "??_7?$codecvt@_WDH@std@@6B@",
                                                          (void *)mp_codecvt_w_vft },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEAH@Z",
                                                          (void *)mp_ist_int },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEAJ@Z",
                                                          (void *)mp_ist_long },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEAK@Z",
                                                          (void *)mp_ist_ulong },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEA_J@Z",
                                                          (void *)mp_ist_i64 },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEA_K@Z",
                                                          (void *)mp_ist_u64 },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEAM@Z",
                                                          (void *)mp_ist_float },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEAN@Z",
                                                          (void *)mp_ist_double },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEA_N@Z",
                                                          (void *)mp_ist_bool },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAV01@AEAV01@@Z@Z",
                                                          (void *)mp_istream_manip_is },
    { "msvcrt.dll", "??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAVios_base@1@AEAV21@@Z@Z",
                                                          (void *)mp_ostream_manip },
    { "msvcrt.dll", "?read@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@PEAD_J@Z",
                                                          (void *)mp_ist_read },
    { "msvcrt.dll", "?seekg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@V?$fpos@H@2@@Z",
                                                          (void *)mp_ist_seekg },
    { "msvcrt.dll", "?tellg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA?AV?$fpos@H@2@XZ",
                                                          (void *)mp_ist_tellg },
    { "msvcrt.dll", "?ws@std@@YAAEAV?$basic_istream@DU?$char_traits@D@std@@@std@@AEAV21@@Z",
                                                          (void *)mp_ws },
    { "msvcrt.dll", "?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A",
                                                          (void *)mp_cout_obj },
    { "msvcrt.dll", "?cerr@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A",
                                                          (void *)mp_cerr_obj },
    { "msvcrt.dll", "?clog@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A",
                                                          (void *)mp_clog_obj },
    { "msvcrt.dll", "?cin@std@@3V?$basic_istream@DU?$char_traits@D@std@@@1@A",
                                                          (void *)mp_cin_obj },
    /* _Pad, the std::thread launch handshake -- see msvcp_shim.h. */
    { "msvcrt.dll", "??0_Pad@std@@QEAA@XZ",                   (void *)mp_pad_ctor },
    { "msvcrt.dll", "??1_Pad@std@@QEAA@XZ",                   (void *)mp_pad_dtor },
    { "msvcrt.dll", "?_Launch@_Pad@std@@QEAAXPEAU_Thrd_imp_t@@@Z", (void *)mp_pad_launch },
    { "msvcrt.dll", "?_Release@_Pad@std@@QEAAXXZ",            (void *)mp_pad_release },
    /* ios_base, basic_ios, the wide streambuf and the ostream
     * inserters -- see msvcp_shim.h for where each layout came from. */
    { "msvcrt.dll", "?flags@ios_base@std@@QEBAHXZ",
                                                              (void *)mp_ios_flags_get },
    { "msvcrt.dll", "?flags@ios_base@std@@QEAAHH@Z",
                                                              (void *)mp_ios_flags_set },
    { "msvcrt.dll", "?setf@ios_base@std@@QEAAHH@Z",
                                                              (void *)mp_ios_setf },
    { "msvcrt.dll", "?setf@ios_base@std@@QEAAHHH@Z",
                                                              (void *)mp_ios_setf_mask },
    { "msvcrt.dll", "?unsetf@ios_base@std@@QEAAXH@Z",
                                                              (void *)mp_ios_unsetf },
    { "msvcrt.dll", "?width@ios_base@std@@QEBA_JXZ",
                                                              (void *)mp_ios_width_get },
    { "msvcrt.dll", "?width@ios_base@std@@QEAA_J_J@Z",
                                                              (void *)mp_ios_width_set },
    { "msvcrt.dll", "?precision@ios_base@std@@QEBA_JXZ",
                                                              (void *)mp_ios_prec_get },
    { "msvcrt.dll", "?precision@ios_base@std@@QEAA_J_J@Z",
                                                              (void *)mp_ios_prec_set },
    { "msvcrt.dll", "?good@ios_base@std@@QEBA_NXZ",
                                                              (void *)mp_ios_good },
    { "msvcrt.dll", "?bad@ios_base@std@@QEBA_NXZ",
                                                              (void *)mp_ios_bad },
    { "msvcrt.dll", "?eof@ios_base@std@@QEBA_NXZ",
                                                              (void *)mp_ios_eof },
    { "msvcrt.dll", "?fail@ios_base@std@@QEBA_NXZ",
                                                              (void *)mp_ios_fail },
    { "msvcrt.dll", "?getloc@ios_base@std@@QEBA?AVlocale@2@XZ",
                                                              (void *)mp_ios_getloc },
    { "msvcrt.dll", "?_Ios_base_dtor@ios_base@std@@CAXPEAV12@@Z",
                                                              (void *)mp_ios_base_dtor },
    { "msvcrt.dll", "?rdbuf@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBAPEAV?$basic_streambuf@DU?$char_traits@D@std@@@2@XZ",
                                                              (void *)mp_bios_rdbuf },
    { "msvcrt.dll", "?rdbuf@?$basic_ios@_WU?$char_traits@_W@std@@@std@@QEBAPEAV?$basic_streambuf@_WU?$char_traits@_W@std@@@2@XZ",
                                                              (void *)mp_bios_rdbuf },
    { "msvcrt.dll", "?tie@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBAPEAV?$basic_ostream@DU?$char_traits@D@std@@@2@XZ",
                                                              (void *)mp_bios_tie },
    { "msvcrt.dll", "?widen@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADD@Z",
                                                              (void *)mp_bios_widen },
    { "msvcrt.dll", "?widen@?$basic_ios@_WU?$char_traits@_W@std@@@std@@QEBA_WD@Z",
                                                              (void *)mp_bios_widen_w },
    { "msvcrt.dll", "?narrow@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADDD@Z",
                                                              (void *)mp_bios_narrow },
    { "msvcrt.dll", "?fill@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADXZ",
                                                              (void *)mp_bios_fill },
    { "msvcrt.dll", "?imbue@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAA?AVlocale@2@AEBV32@@Z",
                                                              (void *)mp_bios_imbue },
    { "msvcrt.dll", "?imbue@?$basic_ios@_WU?$char_traits@_W@std@@@std@@QEAA?AVlocale@2@AEBV32@@Z",
                                                              (void *)mp_bios_imbue },
    { "msvcrt.dll", "?eback@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEBAPEA_WXZ",
                                                              (void *)mp_sb_eback },
    { "msvcrt.dll", "?gptr@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEBAPEA_WXZ",
                                                              (void *)mp_sb_gptr_pub },
    { "msvcrt.dll", "?egptr@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEBAPEA_WXZ",
                                                              (void *)mp_sb_egptr },
    { "msvcrt.dll", "?pbase@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEBAPEA_WXZ",
                                                              (void *)mp_sb_pbase },
    { "msvcrt.dll", "?pptr@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEBAPEA_WXZ",
                                                              (void *)mp_sb_pptr_pub },
    { "msvcrt.dll", "?epptr@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEBAPEA_WXZ",
                                                              (void *)mp_sb_epptr },
    { "msvcrt.dll", "?gbump@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEAAXH@Z",
                                                              (void *)mp_wsb_gbump },
    { "msvcrt.dll", "?pbump@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEAAXH@Z",
                                                              (void *)mp_wsb_pbump },
    { "msvcrt.dll", "?_Pninc@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@IEAAPEA_WXZ",
                                                              (void *)mp_wsb_pninc },
    { "msvcrt.dll", "?sputc@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@QEAAG_W@Z",
                                                              (void *)mp_wsb_sputc },
    { "msvcrt.dll", "?sputn@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@QEAA_JPEB_W_J@Z",
                                                              (void *)mp_wsb_sputn },
    { "msvcrt.dll", "?xsputn@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@MEAA_JPEB_W_J@Z",
                                                              (void *)mp_wsb_v_xsputn },
    { "msvcrt.dll", "?xsgetn@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@MEAA_JPEA_W_J@Z",
                                                              (void *)mp_wsb_v_xsgetn },
    { "msvcrt.dll", "?uflow@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@MEAAGXZ",
                                                              (void *)mp_wsb_v_uflow },
    { "msvcrt.dll", "?showmanyc@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@MEAA_JXZ",
                                                              (void *)mp_sb_v_showmanyc },
    { "msvcrt.dll", "?sync@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@MEAAHXZ",
                                                              (void *)mp_sb_v_sync },
    { "msvcrt.dll", "?setbuf@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@MEAAPEAV12@PEA_W_J@Z",
                                                              (void *)mp_sb_v_setbuf },
    { "msvcrt.dll", "?imbue@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@MEAAXAEBVlocale@2@@Z",
                                                              (void *)mp_sb_v_imbue },
    { "msvcrt.dll", "?_Lock@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@UEAAXXZ",
                                                              (void *)mp_sb_lock },
    { "msvcrt.dll", "?_Unlock@?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@UEAAXXZ",
                                                              (void *)mp_sb_unlock },
    { "msvcrt.dll", "?_Init@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXXZ",
                                                              (void *)mp_sb_init },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@G@Z",
                                                              (void *)mp_ost_ushort },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@H@Z",
                                                              (void *)mp_ost_int },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@I@Z",
                                                              (void *)mp_ost_uint },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@J@Z",
                                                              (void *)mp_ost_long },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@K@Z",
                                                              (void *)mp_ost_ulong },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@M@Z",
                                                              (void *)mp_ost_float },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@N@Z",
                                                              (void *)mp_ost_double },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@O@Z",
                                                              (void *)mp_ost_ldouble },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@_J@Z",
                                                              (void *)mp_ost_i64 },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@_K@Z",
                                                              (void *)mp_ost_u64 },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@_N@Z",
                                                              (void *)mp_ost_bool },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@PEBX@Z",
                                                              (void *)mp_ost_ptr },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@G@Z",
                                                              (void *)mp_wost_ushort },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@H@Z",
                                                              (void *)mp_wost_int },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@I@Z",
                                                              (void *)mp_wost_uint },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@J@Z",
                                                              (void *)mp_wost_long },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@K@Z",
                                                              (void *)mp_wost_ulong },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@M@Z",
                                                              (void *)mp_wost_float },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@N@Z",
                                                              (void *)mp_wost_double },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@O@Z",
                                                              (void *)mp_wost_ldouble },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@_J@Z",
                                                              (void *)mp_wost_i64 },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@_K@Z",
                                                              (void *)mp_wost_u64 },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@_N@Z",
                                                              (void *)mp_wost_bool },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@PEBX@Z",
                                                              (void *)mp_wost_ptr },
    { "msvcrt.dll", "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAV01@AEAV01@@Z@Z",
                                                              (void *)mp_ostream_manip_os },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@P6AAEAV01@AEAV01@@Z@Z",
                                                              (void *)mp_ostream_manip_os },
    { "msvcrt.dll", "??6?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV01@P6AAEAVios_base@1@AEAV21@@Z@Z",
                                                              (void *)mp_ostream_manip },
    { "msvcrt.dll", "?put@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@D@Z",
                                                              (void *)mp_ost_put },
    { "msvcrt.dll", "?put@?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV12@_W@Z",
                                                              (void *)mp_wost_put },
    { "msvcrt.dll", "?write@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@PEBD_J@Z",
                                                              (void *)mp_ost_write },
    { "msvcrt.dll", "?write@?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV12@PEB_W_J@Z",
                                                              (void *)mp_wost_write },
    { "msvcrt.dll", "?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@XZ",
                                                              (void *)mp_ost_flush },
    { "msvcrt.dll", "?flush@?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAAEAV12@XZ",
                                                              (void *)mp_ost_flush },
    { "msvcrt.dll", "?setw@std@@YA?AU?$_Smanip@_J@1@_J@Z",
                                                              (void *)mp_setw },
    { "msvcrt.dll", "?setprecision@std@@YA?AU?$_Smanip@_J@1@_J@Z",
                                                              (void *)mp_setprecision },
    { "msvcrt.dll", "?setiosflags@std@@YA?AU?$_Smanip@H@1@H@Z",
                                                              (void *)mp_setiosflags },
    { "msvcrt.dll", "?resetiosflags@std@@YA?AU?$_Smanip@H@1@H@Z",
                                                              (void *)mp_resetiosflags },
    /* ctype<char>: the classic narrow facet, laid out as its own _Getcat
     * builds it -- see msvcp_shim.h. table() has to be real because is() is
     * inlined into the caller and indexes it directly. */
    { "msvcrt.dll", "?_Getcat@?$ctype@D@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z",
                                                              (void *)mp_ctypec_Getcat },
    { "msvcrt.dll", "?classic_table@?$ctype@D@std@@SAPEBFXZ", (void *)mp_ctc_classic_table },
    { "msvcrt.dll", "?table@?$ctype@D@std@@QEBAPEBFXZ",       (void *)mp_ctc_table_pub },
    { "msvcrt.dll", "?table_size@?$ctype@D@std@@2_KB",        (void *)&mp_ctc_table_size },
    { "msvcrt.dll", "?id@?$ctype@D@std@@2V0locale@2@A",       (void *)&mp_ctypec_id_value },
    { "msvcrt.dll", "?widen@?$ctype@D@std@@QEBADD@Z",         (void *)mp_ctc_widen },
    { "msvcrt.dll", "?widen@?$ctype@D@std@@QEBAPEBDPEBD0PEAD@Z", (void *)mp_ctc_widen_p },
    { "msvcrt.dll", "?narrow@?$ctype@D@std@@QEBADDD@Z",       (void *)mp_ctc_narrow },
    { "msvcrt.dll", "?narrow@?$ctype@D@std@@QEBAPEBDPEBD0DPEAD@Z", (void *)mp_ctc_narrow_p },
    { "msvcrt.dll", "?tolower@?$ctype@D@std@@QEBADD@Z",       (void *)mp_ctc_tolower },
    { "msvcrt.dll", "?tolower@?$ctype@D@std@@QEBAPEBDPEADPEBD@Z", (void *)mp_ctc_tolower_p },
    { "msvcrt.dll", "?toupper@?$ctype@D@std@@QEBADD@Z",       (void *)mp_ctc_toupper },
    { "msvcrt.dll", "?toupper@?$ctype@D@std@@QEBAPEBDPEADPEBD@Z", (void *)mp_ctc_toupper_p },
    { "msvcrt.dll", "?is@?$ctype@D@std@@QEBA_NFD@Z",          (void *)mp_ctc_is },
    { "msvcrt.dll", "?is@?$ctype@D@std@@QEBAPEBDPEBD0PEAF@Z", (void *)mp_ctc_is_p },
    { "msvcrt.dll", "?scan_is@?$ctype@D@std@@QEBAPEBDFPEBD0@Z",  (void *)mp_ctc_scan_is },
    { "msvcrt.dll", "?scan_not@?$ctype@D@std@@QEBAPEBDFPEBD0@Z", (void *)mp_ctc_scan_not },
    { "msvcrt.dll", "?exceptions@ios_base@std@@QEAAXH@Z",     (void *)mp_ios_exceptions_set },
    { "msvcrt.dll", "?exceptions@ios_base@std@@QEBAHXZ",      (void *)mp_ios_exceptions_get },
    { "msvcrt.dll", "?_Getcat@?$ctype@_W@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z",
                                                  (void *)mp_ctypew_Getcat },
    { "msvcrt.dll", "?widen@?$ctype@_W@std@@QEBA_WD@Z",            (void *)mp_ctw_widen },
    { "msvcrt.dll", "?widen@?$ctype@_W@std@@QEBAPEBDPEBD0PEA_W@Z", (void *)mp_ctw_widen_r },
    { "msvcrt.dll", "?narrow@?$ctype@_W@std@@QEBAD_WD@Z",          (void *)mp_ctw_narrow },
    { "msvcrt.dll", "?narrow@?$ctype@_W@std@@QEBAPEB_WPEB_W0DPEAD@Z", (void *)mp_ctw_narrow_r },
    { "msvcrt.dll", "?is@?$ctype@_W@std@@QEBA_NF_W@Z",             (void *)mp_ctw_is },
    { "msvcrt.dll", "?is@?$ctype@_W@std@@QEBAPEB_WPEB_W0PEAF@Z",   (void *)mp_ctw_is_r },
    { "msvcrt.dll", "?tolower@?$ctype@_W@std@@QEBA_W_W@Z",         (void *)mp_ctw_tolower },
    { "msvcrt.dll", "?toupper@?$ctype@_W@std@@QEBA_W_W@Z",         (void *)mp_ctw_toupper },
    { "msvcrt.dll", "?scan_is@?$ctype@_W@std@@QEBAPEB_WFPEB_W0@Z", (void *)mp_ctw_scan_is_p },
    { "msvcrt.dll", "?scan_not@?$ctype@_W@std@@QEBAPEB_WFPEB_W0@Z", (void *)mp_ctw_scan_not_p },
    /* The wide basic_ios is the narrow one: the constructor sets a vftable and
     * leaves the fields to init, and the fields are element-size independent. */
    { "msvcrt.dll", "??0?$basic_ios@_WU?$char_traits@_W@std@@@std@@IEAA@XZ",
                                                  (void *)mp_basic_ios_ctor },
    { "msvcrt.dll", "??1?$basic_ios@_WU?$char_traits@_W@std@@@std@@UEAA@XZ",
                                                  (void *)mp_stream_dtor },
    { "msvcrt.dll", "??1?$basic_streambuf@_WU?$char_traits@_W@std@@@std@@UEAA@XZ",
                                                  (void *)mp_stream_dtor },
    { "msvcrt.dll", "??0?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAA@PEAV?$basic_streambuf@_WU?$char_traits@_W@std@@@1@_N@Z", (void *)mp_ostream_ctor },
    { "msvcrt.dll", "??0?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z", (void *)mp_ostream_ctor },
    { "msvcrt.dll", "??0?$basic_istream@_WU?$char_traits@_W@std@@@std@@QEAA@PEAV?$basic_streambuf@_WU?$char_traits@_W@std@@@1@_N@Z", (void *)mp_istream_ctor },
    { "msvcrt.dll", "??0?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z", (void *)mp_istream_ctor },
    { "msvcrt.dll", "??1?$basic_ostream@_WU?$char_traits@_W@std@@@std@@UEAA@XZ",             (void *)mp_stream_dtor },
    { "msvcrt.dll", "??1?$basic_ostream@DU?$char_traits@D@std@@@std@@UEAA@XZ",             (void *)mp_stream_dtor },
    { "msvcrt.dll", "??1?$basic_istream@_WU?$char_traits@_W@std@@@std@@UEAA@XZ",             (void *)mp_stream_dtor },
    { "msvcrt.dll", "??1?$basic_istream@DU?$char_traits@D@std@@@std@@UEAA@XZ",             (void *)mp_stream_dtor },
    /* The wide equivalents. The ios state fields do not depend on the element
     * type, so these are the same implementations under different names. */
    { "msvcrt.dll", "?clear@?$basic_ios@_WU?$char_traits@_W@std@@@std@@QEAAXH_N@Z",    (void *)mp_ios_clear },
    { "msvcrt.dll", "?setstate@?$basic_ios@_WU?$char_traits@_W@std@@@std@@QEAAXH_N@Z", (void *)mp_ios_setstate },
    { "msvcrt.dll", "?_Osfx@?$basic_ostream@_WU?$char_traits@_W@std@@@std@@QEAAXXZ",       (void *)mp_ostream_osfx },
#endif
    { "msvcrt.dll", "_malloc_crt",   (void *)st_malloc },
    { "msvcrt.dll", "_calloc_crt",   (void *)st_calloc },
    { "msvcrt.dll", "_realloc_crt",  (void *)st_realloc },
    { "msvcrt.dll", "_free_crt",     (void *)st_free },
    { "msvcrt.dll", "_recalloc_crt", (void *)st__recalloc },
    /* printf family, narrow and wide */
    { "msvcrt.dll", "_setjmp", (void *)st__setjmp },
    { "msvcrt.dll", "_setjmpex", (void *)st__setjmpex },
    /* _setjmp3 is what the MSVC compiler emits for setjmp() on i386, and it has
     * to come from the same pair as longjmp: with the real CRT loaded, its
     * _setjmp3 wrote the buffer and our longjmp found no magic in it and
     * refused to jump -- which took NI Absynth and FM8 down. The extra
     * arguments describe unwind state we do not keep, and the buffer is in the
     * same place either way. */
    { "msvcrt.dll", "_setjmp3", (void *)st__setjmp },
    { "msvcrt.dll", "longjmp", (void *)st_longjmp },
    { "msvcrt.dll", "_beginthreadex", (void *)st__beginthreadex },
    { "msvcrt.dll", "_endthreadex", (void *)st__endthreadex },
    { "msvcrt.dll", "_beginthread", (void *)st__beginthread },
    { "msvcrt.dll", "_endthread", (void *)st__endthread },
    { "msvcrt.dll", "_control87", (void *)st__control87 },
    { "msvcrt.dll", "_controlfp", (void *)st__controlfp },
    { "msvcrt.dll", "_controlfp_s", (void *)st__controlfp_s },
    { "msvcrt.dll", "_statusfp", (void *)st__statusfp },
    { "msvcrt.dll", "_clearfp", (void *)st__clearfp },
    { "msvcrt.dll", "_fpreset", (void *)st__fpreset },
    { "msvcrt.dll", "fegetenv", (void *)st_fegetenv },
    { "msvcrt.dll", "fesetenv", (void *)st_fesetenv },
    { "msvcrt.dll", "fegetround", (void *)st_fegetround },
    { "msvcrt.dll", "fesetround", (void *)st_fesetround },
    { "msvcrt.dll", "_aligned_malloc", (void *)st__aligned_malloc },
    { "msvcrt.dll", "_aligned_free", (void *)st__aligned_free },
    { "msvcrt.dll", "_aligned_offset_malloc", (void *)st__aligned_offset_malloc },
    { "msvcrt.dll", "_aligned_offset_realloc", (void *)st__aligned_offset_realloc },
    { "msvcrt.dll", "_aligned_realloc", (void *)st__aligned_realloc },
    { "msvcrt.dll", "_aligned_msize", (void *)st__aligned_msize },
    { "msvcrt.dll", "_fcloseall", (void *)st__fcloseall },
    { "msvcrt.dll", "_localtime64", (void *)st__localtime64 },
    { "msvcrt.dll", "_gmtime64", (void *)st__gmtime64 },
    { "msvcrt.dll", "_errno", (void *)st__errno },
    { "msvcrt.dll", "__doserrno", (void *)st___doserrno },
    { "msvcrt.dll", "_get_errno", (void *)st__get_errno },
    { "msvcrt.dll", "_set_errno", (void *)st__set_errno },
    { "msvcrt.dll", "_get_doserrno", (void *)st__get_doserrno },
    { "msvcrt.dll", "_set_doserrno", (void *)st__set_doserrno },
    /* std::exception, 64-bit then 32-bit decorations */
    SM("msvcrt.dll", "??0exception@std@@QEAA@XZ", st_cxx_exc_ctor),
    SM("msvcrt.dll", "??0exception@std@@QEAA@AEBQEBD@Z", st_cxx_exc_ctor_s),
    SM("msvcrt.dll", "??0exception@std@@QEAA@AEBQEBDH@Z", st_cxx_exc_ctor_si),
    SM("msvcrt.dll", "??0exception@std@@QEAA@AEBV01@@Z", st_cxx_exc_copy),
    SM("msvcrt.dll", "??1exception@std@@UEAA@XZ", st_cxx_exc_dtor),
    SM("msvcrt.dll", "?what@exception@std@@UEBAPEBDXZ", st_cxx_exc_what),
    SM("msvcrt.dll", "??0exception@std@@QAE@XZ", st_cxx_exc_ctor),
    SM("msvcrt.dll", "??0exception@std@@QAE@ABQBD@Z", st_cxx_exc_ctor_s),
    SM("msvcrt.dll", "??0exception@std@@QAE@ABQBDH@Z", st_cxx_exc_ctor_si),
    SM("msvcrt.dll", "??0exception@std@@QAE@ABV01@@Z", st_cxx_exc_copy),
    SM("msvcrt.dll", "??1exception@std@@UAE@XZ", st_cxx_exc_dtor),
    SM("msvcrt.dll", "?what@exception@std@@UBEPBDXZ", st_cxx_exc_what),
    S("kernel32.dll", SleepEx),
    SM("msvcrt.dll", "?wait@Concurrency@@YAXI@Z", st_conc_wait),
    SM("msvcrt.dll", "?_Id@_CurrentScheduler@details@Concurrency@@SAIXZ", st_conc_scheduler_id),
    { "msvcrt.dll", "_lock", (void *)st__lock },
    { "msvcrt.dll", "_unlock", (void *)st__unlock },
    { "msvcrt.dll", "__dllonexit", (void *)st___dllonexit },
    { "msvcrt.dll", "log2", (void *)st_log2 },
    { "msvcrt.dll", "log2f", (void *)st_log2f },
    { "msvcrt.dll", "strcspn", (void *)st_strcspn },
    { "msvcrt.dll", "strspn", (void *)st_strspn },
    { "msvcrt.dll", "strpbrk", (void *)st_strpbrk },
    S("user32.dll", GetMonitorInfoA), S("user32.dll", GetMonitorInfoW),
    S("user32.dll", MonitorFromWindow), S("user32.dll", MonitorFromPoint),
    S("user32.dll", MonitorFromRect),
    S("kernel32.dll", LockFile), S("kernel32.dll", LockFileEx),
    S("kernel32.dll", UnlockFile), S("kernel32.dll", UnlockFileEx),
    S("kernel32.dll", GetSystemTime), S("kernel32.dll", GetLocalTime),
    S("kernel32.dll", SystemTimeToFileTime),
    S("kernel32.dll", FileTimeToSystemTime),
    S("kernel32.dll", FileTimeToLocalFileTime),
    S("kernel32.dll", LocalFileTimeToFileTime),
    S("kernel32.dll", SystemTimeToTzSpecificLocalTime),
    S("kernel32.dll", GetFileAttributesExA), S("kernel32.dll", GetFileAttributesExW),
    S("kernel32.dll", FindFirstFileA), S("kernel32.dll", FindFirstFileW),
    S("kernel32.dll", FindNextFileA), S("kernel32.dll", FindNextFileW),
    S("kernel32.dll", FindClose),
    { "msvcrt.dll", "_wgetenv", (void *)st__wgetenv },
    SM("msvcrt.dll", "??8type_info@@QEBA_NAEBV0@@Z", st_type_info_eq),
    SM("msvcrt.dll", "??9type_info@@QEBA_NAEBV0@@Z", st_type_info_ne),
    SM("msvcrt.dll", "?name@type_info@@QEBAPEBDXZ", st_type_info_name),
    SM("msvcrt.dll", "??8type_info@@QBE_NABV0@@Z", st_type_info_eq),
    SM("msvcrt.dll", "??9type_info@@QBE_NABV0@@Z", st_type_info_ne),
    S("advapi32.dll", RegDeleteValueA),
    /* ws2_32 is bound by ordinal, and the loader looks those up under this
     * spelling. Both forms are registered so either binding resolves. */
    { "ws2_32.dll", "ordinal#115", (void *)st_WSAStartup },
    { "ws2_32.dll", "ordinal#116", (void *)st_WSACleanup },
    { "ws2_32.dll", "ordinal#111", (void *)st_WSAGetLastError },
    { "ws2_32.dll", "ordinal#112", (void *)st_WSASetLastError },
    { "ws2_32.dll", "ordinal#57",  (void *)st_ws_gethostname },
    { "ws2_32.dll", "ordinal#8",   (void *)st_htonl_ },
    { "ws2_32.dll", "ordinal#9",   (void *)st_htons_ },
    { "ws2_32.dll", "ordinal#14",  (void *)st_ntohl_ },
    { "ws2_32.dll", "ordinal#15",  (void *)st_ntohs_ },
    { "ws2_32.dll", "WSAStartup",  (void *)st_WSAStartup },
    { "ws2_32.dll", "WSACleanup",  (void *)st_WSACleanup },
    { "ws2_32.dll", "WSAGetLastError", (void *)st_WSAGetLastError },
    { "ws2_32.dll", "WSASetLastError", (void *)st_WSASetLastError },
    { "ws2_32.dll", "gethostname", (void *)st_ws_gethostname },
    { "ws2_32.dll", "htonl", (void *)st_htonl_ },
    { "ws2_32.dll", "htons", (void *)st_htons_ },
    { "ws2_32.dll", "ntohl", (void *)st_ntohl_ },
    { "ws2_32.dll", "ntohs", (void *)st_ntohs_ },
#ifndef PELOAD_NO_GUI_LAYER
    /* DirectWrite. Skia finds this with LoadLibrary and GetProcAddress, which
     * the handle above already served; a plug-in that imports it straight from
     * DWrite.dll reached the generic stub instead, and that returns E_NOTIMPL
     * without filling the factory out-parameter the caller then dereferences. */
    { "dwrite.dll", "DWriteCreateFactory", (void *)st_DWriteCreateFactory },
#endif
#ifndef PELOAD_NO_GUI_LAYER
    { "ole32.dll", "CoCreateInstance", (void *)st_CoCreateInstance },
#endif
    S("gdiplus.dll", GdiplusStartup), S("gdiplus.dll", GdiplusShutdown),
#ifndef PELOAD_NO_GUI_LAYER
    /* GDI+. The whole surface this corpus imports -- see gdiplus_shim.h. */
    S("gdiplus.dll", GdiplusStartup), S("gdiplus.dll", GdiplusShutdown), S("gdiplus.dll", GdipCreatePath),
    S("gdiplus.dll", GdipClonePath), S("gdiplus.dll", GdipDeletePath), S("gdiplus.dll", GdipMeasureString),
    S("gdiplus.dll", GdipDrawString), S("gdiplus.dll", GdipGetFontHeightGivenDPI), S("gdiplus.dll", GdipGetFontSize),
    S("gdiplus.dll", GdipGetFontStyle), S("gdiplus.dll", GdipGetFamily), S("gdiplus.dll", GdipDeleteFont),
    S("gdiplus.dll", GdipCreateFont), S("gdiplus.dll", GdipGetLineSpacing), S("gdiplus.dll", GdipGetCellDescent),
    S("gdiplus.dll", GdipGetCellAscent), S("gdiplus.dll", GdipGetEmHeight), S("gdiplus.dll", GdipGetGenericFontFamilySansSerif),
    S("gdiplus.dll", GdipDeleteFontFamily), S("gdiplus.dll", GdipCreateFontFamilyFromName), S("gdiplus.dll", GdipSetClipRect),
    S("gdiplus.dll", GdipDrawImageRectRectI), S("gdiplus.dll", GdipFillPath), S("gdiplus.dll", GdipFillEllipse),
    S("gdiplus.dll", GdipFillPolygon), S("gdiplus.dll", GdipFillRectangle), S("gdiplus.dll", GdipDrawPath),
    S("gdiplus.dll", GdipDrawPolygon), S("gdiplus.dll", GdipDrawEllipse), S("gdiplus.dll", GdipDrawRectangle),
    S("gdiplus.dll", GdipDrawLine), S("gdiplus.dll", GdipGetDpiY), S("gdiplus.dll", GdipSetPageUnit),
    S("gdiplus.dll", GdipGetWorldTransform), S("gdiplus.dll", GdipTranslateWorldTransform), S("gdiplus.dll", GdipSetWorldTransform),
    S("gdiplus.dll", GdipSetInterpolationMode), S("gdiplus.dll", GdipSetTextRenderingHint), S("gdiplus.dll", GdipSetPixelOffsetMode),
    S("gdiplus.dll", GdipSetSmoothingMode), S("gdiplus.dll", GdipDeleteGraphics), S("gdiplus.dll", GdipCreateFromHWND),
    S("gdiplus.dll", GdipCreateFromHDC), S("gdiplus.dll", GdipSetImageAttributesColorMatrix), S("gdiplus.dll", GdipDisposeImageAttributes),
    S("gdiplus.dll", GdipCreateImageAttributes), S("gdiplus.dll", GdipBitmapUnlockBits), S("gdiplus.dll", GdipBitmapLockBits),
    S("gdiplus.dll", GdipCreateBitmapFromResource), S("gdiplus.dll", GdipCreateHBITMAPFromBitmap), S("gdiplus.dll", GdipCreateBitmapFromScan0),
    S("gdiplus.dll", GdipCreateBitmapFromStreamICM), S("gdiplus.dll", GdipGetImageHeight), S("gdiplus.dll", GdipGetImageWidth),
    S("gdiplus.dll", GdipGetImageGraphicsContext), S("gdiplus.dll", GdipDisposeImage), S("gdiplus.dll", GdipCloneImage),
    S("gdiplus.dll", GdipSetPenDashArray), S("gdiplus.dll", GdipSetPenDashOffset), S("gdiplus.dll", GdipSetPenDashStyle),
    S("gdiplus.dll", GdipSetPenColor), S("gdiplus.dll", GdipSetPenLineJoin), S("gdiplus.dll", GdipSetPenLineCap197819),
    S("gdiplus.dll", GdipSetPenWidth), S("gdiplus.dll", GdipDeletePen), S("gdiplus.dll", GdipCreatePen1),
    S("gdiplus.dll", GdipGetPathGradientPointCount), S("gdiplus.dll", GdipSetPathGradientCenterPoint), S("gdiplus.dll", GdipSetPathGradientSurroundColorsWithCount),
    S("gdiplus.dll", GdipSetPathGradientCenterColor), S("gdiplus.dll", GdipCreatePathGradientFromPath), S("gdiplus.dll", GdipSetLinePresetBlend),
    S("gdiplus.dll", GdipCreateLineBrush), S("gdiplus.dll", GdipSetSolidFillColor), S("gdiplus.dll", GdipCreateSolidFill),
    S("gdiplus.dll", GdipDeleteBrush), S("gdiplus.dll", GdipCloneBrush), S("gdiplus.dll", GdipSetMatrixElements),
    S("gdiplus.dll", GdipDeleteMatrix), S("gdiplus.dll", GdipCreateMatrix2), S("gdiplus.dll", GdipCreateMatrix),
    S("gdiplus.dll", GdipIsVisiblePathPoint), S("gdiplus.dll", GdipFree), S("gdiplus.dll", GdipAlloc),
    S("gdiplus.dll", GdipGetPathWorldBounds), S("gdiplus.dll", GdipTransformPath), S("gdiplus.dll", GdipStartPathFigure),
    S("gdiplus.dll", GdipClosePathFigure), S("gdiplus.dll", GdipGetPathLastPoint), S("gdiplus.dll", GdipAddPathLine),
    S("gdiplus.dll", GdipAddPathArc), S("gdiplus.dll", GdipAddPathBezier), S("gdiplus.dll", GdipAddPathRectangle),
    S("gdiplus.dll", GdipAddPathEllipse), S("gdiplus.dll", GdipAddPathString), S("gdiplus.dll", GdipSetPathFillMode),
#endif
#ifndef PELOAD_NO_GUI_LAYER
    { "d3d11.dll", "D3D11CreateDevice", (void *)st_D3D11CreateDevice },
    /* d2d1.dll exports D2D1CreateFactory as ordinal 1, and that is how a
     * plug-in imports it -- there is no name in the import table to match. */
    { "d2d1.dll", "ordinal#1", (void *)st_D2D1CreateFactory },
    { "d2d1.dll", "D2D1CreateFactory", (void *)st_D2D1CreateFactory },
#endif
    S("gdi32.dll", GetDeviceCaps),
    S("gdi32.dll", SetDIBitsToDevice), S("gdi32.dll", GetClipBox),
    S("gdi32.dll", GdiFlush),
    S("user32.dll", GetDialogBaseUnits), S("user32.dll", MapDialogRect),
    S("user32.dll", IsWindow), S("user32.dll", IsWindowVisible),
    S("user32.dll", IsWindowEnabled), S("user32.dll", IsWindowUnicode),
    S("user32.dll", IsChild),
    S("user32.dll", GetDesktopWindow),
    S("user32.dll", FindWindowA), S("user32.dll", FindWindowW),
    S("user32.dll", FindWindowExW),
    S("user32.dll", EnumDisplayMonitors),
    S("shell32.dll", SHAppBarMessage),
    S("advapi32.dll", RegOpenKeyExW), S("advapi32.dll", RegQueryValueExW),
    S("advapi32.dll", RegCreateKeyExA), S("advapi32.dll", RegCreateKeyExW),
    S("advapi32.dll", RegSetValueExA), S("advapi32.dll", RegSetValueExW),
    S("advapi32.dll", RegDeleteValueW), S("advapi32.dll", RegDeleteKeyW),
    S("advapi32.dll", RegEnumKeyExW), S("advapi32.dll", RegEnumValueW),
    S("advapi32.dll", RegQueryInfoKeyW), S("advapi32.dll", RegFlushKey),
    S("shell32.dll", SHGetFolderPathW),
    S("kernel32.dll", FormatMessageA), S("kernel32.dll", FormatMessageW),
    S("kernel32.dll", CreateIoCompletionPort),
    S("kernel32.dll", GetQueuedCompletionStatus),
    S("kernel32.dll", PostQueuedCompletionStatus),
    S("advapi32.dll", GetTokenInformation),
    { "msvcrt.dll", "sprintf",   (void *)st_sprintf },
    { "msvcrt.dll", "sprintf_s", (void *)st_sprintf_s },
    { "msvcrt.dll", "_snprintf", (void *)st__snprintf },
    { "msvcrt.dll", "_snprintf_s", (void *)st__snprintf_s },
    { "msvcrt.dll", "vsprintf",  (void *)st_vsprintf },
    /* scanf. Absent entirely until now -- and a stub for it assigns nothing
     * while reporting success, so a caller keeps whatever was in the variable.
     * The __stdio_common_* forms are how anything built with 2015 or later
     * reaches both scanf and sprintf. */
    { "msvcrt.dll", "strtoul", (void *)st_strtoul },
    { "msvcrt.dll", "_strtoui64", (void *)st__strtoui64 },
    { "msvcrt.dll", "_strtoi64", (void *)st__strtoi64 },
    { "msvcrt.dll", "strtoull", (void *)st__strtoui64 },
    { "msvcrt.dll", "strtoll", (void *)st__strtoi64 },
    { "msvcrt.dll", "atol", (void *)st_atol },
    { "msvcrt.dll", "_atoi64", (void *)st__atoi64 },
    S("msvcrt.dll", iswspace), S("msvcrt.dll", iswalpha), S("msvcrt.dll", iswdigit),
    S("msvcrt.dll", iswalnum), S("msvcrt.dll", iswupper), S("msvcrt.dll", iswlower),
    S("msvcrt.dll", iswpunct), S("msvcrt.dll", iswxdigit), S("msvcrt.dll", iswcntrl),
    S("msvcrt.dll", iswprint), S("msvcrt.dll", towlower), S("msvcrt.dll", towupper),
    S("msvcrt.dll", strcpy_s), S("msvcrt.dll", wcscpy_s), S("msvcrt.dll", wcsncpy_s),
    S("msvcrt.dll", wcscat_s), S("msvcrt.dll", _wsplitpath_s),
    S("msvcrt.dll", _fdtest), S("msvcrt.dll", _dtest),
    { "msvcrt.dll", "__acrt_iob_func", (void *)st___acrt_iob_func },
    { "msvcrt.dll", "__stdio_common_vfprintf", (void *)st___stdio_common_vfprintf },
    { "msvcrt.dll", "__stdio_common_vsnprintf_s", (void *)st___stdio_common_vsnprintf_s },
    { "msvcrt.dll", "_initialize_onexit_table", (void *)st__initialize_onexit_table },
    { "msvcrt.dll", "_register_onexit_function", (void *)st__register_onexit_function },
    { "msvcrt.dll", "_execute_onexit_table", (void *)st__execute_onexit_table },
    { "msvcrt.dll", "__uncaught_exceptions", (void *)st___uncaught_exceptions },
    { "msvcrt.dll", "__vcrt_InitializeCriticalSectionEx", (void *)st___vcrt_InitializeCriticalSectionEx },
    { "msvcrt.dll", "_lock_locales", (void *)st__lock_locales },
    { "msvcrt.dll", "_unlock_locales", (void *)st__unlock_locales },
    { "msvcrt.dll", "_Getdays", (void *)st__Getdays },
    { "msvcrt.dll", "_Getmonths", (void *)st__Getmonths },
    { "msvcrt.dll", "sscanf",   (void *)st_sscanf },
    { "msvcrt.dll", "sscanf_s", (void *)st_sscanf_s },
    { "msvcrt.dll", "swscanf",  (void *)st_swscanf },
    { "msvcrt.dll", "swscanf_s",(void *)st_swscanf_s },
    { "msvcrt.dll", "vsscanf",  (void *)st_vsscanf },
    { "msvcrt.dll", "__stdio_common_vsscanf",   (void *)st___stdio_common_vsscanf },
    { "msvcrt.dll", "__stdio_common_vswscanf",  (void *)st___stdio_common_vswscanf },
    { "msvcrt.dll", "__stdio_common_vsprintf",  (void *)st___stdio_common_vsprintf },
    { "msvcrt.dll", "__stdio_common_vsprintf_s",(void *)st___stdio_common_vsprintf },
    { "msvcrt.dll", "__stdio_common_vswprintf", (void *)st___stdio_common_vswprintf },
    { "msvcrt.dll", "__stdio_common_vswprintf_s",(void *)st___stdio_common_vswprintf },
    { "msvcrt.dll", "_vsnprintf", (void *)st__vsnprintf },
    { "msvcrt.dll", "vsnprintf", (void *)st__vsnprintf },
    { "msvcrt.dll", "vsnprintf_s", (void *)st_vsnprintf_s },
    { "msvcrt.dll", "_vsnprintf_s", (void *)st_vsnprintf_s },
    { "msvcrt.dll", "_vsnprintf_l", (void *)st__vsnprintf_l },
    { "msvcrt.dll", "swprintf_s", (void *)st_swprintf_s },
    { "msvcrt.dll", "_snwprintf", (void *)st__snwprintf },
    { "msvcrt.dll", "_snwprintf_s", (void *)st__snwprintf_s },
    { "msvcrt.dll", "_vsnwprintf", (void *)st__vsnwprintf },
    { "msvcrt.dll", "_vsnwprintf_l", (void *)st__vsnwprintf_l },
    { "msvcrt.dll", "_vswprintf_c", (void *)st__vswprintf_c },
    { "msvcrt.dll", "_vswprintf_c_l", (void *)st__vswprintf_c_l },
    S("kernel32.dll", DuplicateHandle),
    S("user32.dll", LoadIconA), S("user32.dll", LoadIconW),
    S("user32.dll", LoadCursorW),
    S("user32.dll", MapVirtualKeyA), S("user32.dll", MapVirtualKeyW),
    S("user32.dll", ToUnicode),
    S("advapi32.dll", OpenProcessToken),
    S("kernel32.dll", GetComputerNameA), S("kernel32.dll", GetComputerNameW),
    S("kernel32.dll", GetLogicalDriveStringsA), S("kernel32.dll", GetLogicalDriveStringsW),
    S("kernel32.dll", GetLogicalDrives),
    S("kernel32.dll", GetDriveTypeA), S("kernel32.dll", GetDriveTypeW),
    S("kernel32.dll", OutputDebugStringW),
    S("user32.dll", RegisterDeviceNotificationA),
    S("user32.dll", RegisterDeviceNotificationW),
    S("user32.dll", UnregisterDeviceNotification),
    { "msvcrt.dll", "_CxxThrowException", (void *)st__CxxThrowException },
    /* dynamic_cast. Imported from VCRUNTIME140 by anything built with 2015 or
     * later, and reached here through crt_alias. */
    { "msvcrt.dll", "__RTDynamicCast", (void *)st___RTDynamicCast },
    S("kernel32.dll", GetLogicalProcessorInformation),
    S("kernel32.dll", GetProcessAffinityMask), S("kernel32.dll", SetProcessAffinityMask),
    S("kernel32.dll", SetThreadAffinityMask), S("kernel32.dll", SetThreadIdealProcessor),
    S("kernel32.dll", GetActiveProcessorCount), S("kernel32.dll", GetActiveProcessorGroupCount),
    S("kernel32.dll", GetMaximumProcessorCount), S("kernel32.dll", GetMaximumProcessorGroupCount),
    S("kernel32.dll", GetNumaHighestNodeNumber), S("kernel32.dll", GetNumaNodeProcessorMask),
    S("kernel32.dll", GetCurrentProcessorNumber),
    S("kernel32.dll", GetLogicalProcessorInformationEx),
    S("kernel32.dll", GetThreadGroupAffinity), S("kernel32.dll", SetThreadGroupAffinity),
    S("kernel32.dll", GetNumaNodeProcessorMaskEx),
    S("kernel32.dll", GetThreadIdealProcessorEx), S("kernel32.dll", SetThreadIdealProcessorEx),
    /* Dialogs, the shell and a network that is honestly not there. */
    S("comdlg32.dll", GetOpenFileNameW), S("comdlg32.dll", GetSaveFileNameW),
    S("comdlg32.dll", GetOpenFileNameA), S("comdlg32.dll", GetSaveFileNameA),
    S("comdlg32.dll", ChooseColorW), S("comdlg32.dll", ChooseColorA),
    S("shell32.dll", ShellExecuteA), S("shell32.dll", ShellExecuteW),
    S("shell32.dll", SHBrowseForFolderW), S("shell32.dll", SHBrowseForFolderA),
    S("shell32.dll", SHGetPathFromIDListW), S("shell32.dll", SHGetPathFromIDListA),
    S("wininet.dll", InternetOpenA), S("wininet.dll", InternetOpenW),
    S("wininet.dll", InternetConnectA), S("wininet.dll", InternetConnectW),
    S("wininet.dll", InternetOpenUrlA), S("wininet.dll", InternetGetConnectedState),
    S("wininet.dll", HttpOpenRequestA), S("wininet.dll", HttpSendRequestA),
    S("wininet.dll", HttpSendRequestW), S("wininet.dll", HttpQueryInfoA),
    S("wininet.dll", InternetReadFile), S("wininet.dll", InternetQueryDataAvailable),
    S("wininet.dll", InternetSetOptionA), S("wininet.dll", InternetCloseHandle),
    S("kernel32.dll", ReadConsoleW), S("kernel32.dll", WriteConsoleA),
    S("kernel32.dll", FindFirstFileExA), S("kernel32.dll", FindFirstFileExW),
    S("kernel32.dll", GetSystemDirectoryW),
    S("kernel32.dll", LCMapStringEx), S("kernel32.dll", CompareStringEx),
    S("kernel32.dll", GetLocaleInfoEx),
    S("kernel32.dll", GetDateFormatA), S("kernel32.dll", GetDateFormatW),
    S("kernel32.dll", GetTimeFormatA), S("kernel32.dll", GetTimeFormatW),
    S("kernel32.dll", PeekNamedPipe),
    /* File mapping and the 64-bit file positions -- see the block above them. */
    S("msvcrt.dll", _get_osfhandle), S("msvcrt.dll", _open_osfhandle),
    S("kernel32.dll", CreateFileMappingA), S("kernel32.dll", CreateFileMappingW),
    S("kernel32.dll", OpenFileMappingA), S("kernel32.dll", OpenFileMappingW),
    S("kernel32.dll", MapViewOfFile), S("kernel32.dll", MapViewOfFileEx),
    S("kernel32.dll", UnmapViewOfFile), S("kernel32.dll", FlushViewOfFile),
    S("kernel32.dll", GetFileSizeEx), S("kernel32.dll", SetFilePointerEx),
    S("kernel32.dll", GetThreadTimes), S("kernel32.dll", GetProcessTimes),
    S("kernel32.dll", GetSystemTimes), S("kernel32.dll", QueryProcessCycleTime),
    S("kernel32.dll", CreateThreadpoolTimer), S("kernel32.dll", SetThreadpoolTimer),
    S("kernel32.dll", WaitForThreadpoolTimerCallbacks), S("kernel32.dll", CloseThreadpoolTimer),
    S("kernel32.dll", CreateThreadpoolWait), S("kernel32.dll", SetThreadpoolWait),
    S("kernel32.dll", WaitForThreadpoolWaitCallbacks), S("kernel32.dll", CloseThreadpoolWait),
    S("kernel32.dll", CreateThreadpoolWork), S("kernel32.dll", SubmitThreadpoolWork),
    S("kernel32.dll", WaitForThreadpoolWorkCallbacks), S("kernel32.dll", CloseThreadpoolWork),
    S("kernel32.dll", FlushProcessWriteBuffers), S("kernel32.dll", SetThreadStackGuarantee),
    S("kernel32.dll", RegisterWaitForSingleObject),
    S("kernel32.dll", RegisterWaitForSingleObjectEx),
    S("kernel32.dll", UnregisterWait), S("kernel32.dll", UnregisterWaitEx),
    S("kernel32.dll", QueueUserWorkItem),
    S("advapi32.dll", RegisterTraceGuidsW), S("advapi32.dll", RegisterTraceGuidsA),
    S("advapi32.dll", UnregisterTraceGuids), S("advapi32.dll", GetTraceLoggerHandle),
    S("advapi32.dll", GetTraceEnableLevel), S("advapi32.dll", GetTraceEnableFlags),
    S("advapi32.dll", TraceEvent),
    S("advapi32.dll", EventRegister), S("advapi32.dll", EventUnregister),
    S("advapi32.dll", EventWrite), S("advapi32.dll", EventWriteTransfer),
    S("advapi32.dll", EventEnabled), S("advapi32.dll", EventProviderEnabled),
    S("advapi32.dll", EventSetInformation), S("advapi32.dll", EventActivityIdControl),
    S("combase.dll", RoInitialize), S("combase.dll", RoUninitialize),
    S("combase.dll", RoGetActivationFactory), S("combase.dll", RoActivateInstance),
    S("kernel32.dll", GetCurrentProcessorNumberEx), S("kernel32.dll", QueryThreadCycleTime),
    S("kernel32.dll", GetNumaProximityNodeEx), S("kernel32.dll", GetNumaAvailableMemoryNodeEx),
    S("kernel32.dll", GetNumaNodeNumberFromHandle),
    { "powrprof.dll", "CallNtPowerInformation", (void *)st_CallNtPowerInformation },
    S("kernel32.dll", IsWow64Process),
    S("kernel32.dll", GetUserDefaultLangID), S("kernel32.dll", GetSystemDefaultLangID),
    S("kernel32.dll", GetUserDefaultUILanguage),
    S("kernel32.dll", GetSystemDefaultUILanguage),
    S("kernel32.dll", GetThreadUILanguage),
    S("kernel32.dll", GetSystemDefaultLCID),
    S("kernel32.dll", GetThreadLocale), S("kernel32.dll", SetThreadLocale),
    /* ctype, case folding and a couple of small string helpers. */
    { "msvcrt.dll", "isalpha",  (void *)st_isalpha },
    { "msvcrt.dll", "isupper",  (void *)st_isupper },
    { "msvcrt.dll", "islower",  (void *)st_islower },
    { "msvcrt.dll", "isdigit",  (void *)st_isdigit },
    { "msvcrt.dll", "isxdigit", (void *)st_isxdigit },
    { "msvcrt.dll", "isspace",  (void *)st_isspace },
    { "msvcrt.dll", "ispunct",  (void *)st_ispunct },
    { "msvcrt.dll", "isalnum",  (void *)st_isalnum },
    { "msvcrt.dll", "isprint",  (void *)st_isprint },
    { "msvcrt.dll", "isgraph",  (void *)st_isgraph },
    { "msvcrt.dll", "iscntrl",  (void *)st_iscntrl },
    { "msvcrt.dll", "tolower",  (void *)st_tolower },
    { "msvcrt.dll", "toupper",  (void *)st_toupper },
    { "msvcrt.dll", "_tolower", (void *)st_tolower },
    { "msvcrt.dll", "_toupper", (void *)st_toupper },
    { "msvcrt.dll", "memchr",   (void *)st_memchr },
    { "msvcrt.dll", "_wsplitpath", (void *)st__wsplitpath },
    { "msvcrt.dll", "__uncaught_exception", (void *)st___uncaught_exception },
    { "msvcrt.dll", "___lc_locale_name_func", (void *)st___lc_locale_name_func },
    { "msvcrt.dll", "___lc_handle_func",      (void *)st___lc_handle_func },
    { "msvcrt.dll", "___lc_collate_cp_func",  (void *)st___lc_collate_cp_func },
    { "msvcrt.dll", "localeconv",             (void *)st_localeconv },
    /* Locale and stdio, which the C++ library reads through while it builds its
     * stream objects. */
    { "msvcrt.dll", "__pctype_func",      (void *)st___pctype_func },
    { "msvcrt.dll", "__iob_func",         (void *)st___iob_func },
    { "msvcrt.dll", "setlocale",          (void *)st_setlocale },
    { "msvcrt.dll", "___lc_codepage_func",(void *)st___lc_codepage_func },
    { "msvcrt.dll", "___mb_cur_max_func", (void *)st___mb_cur_max_func },
    { "msvcrt.dll", "_configthreadlocale",(void *)st__configthreadlocale },
    { "msvcrt.dll", "__crtInitializeCriticalSectionEx",
                                          (void *)st___crtInitializeCriticalSectionEx },
    /* The Concurrency runtime, which MSVC 2013 puts in the C runtime and a real
     * MSVCP120 resolves at load time. */
    SM("msvcrt.dll", "??0critical_section@Concurrency@@QEAA@XZ", st_cs_ctor),
    SM("msvcrt.dll", "??1critical_section@Concurrency@@QEAA@XZ", st_cs_dtor),
    SM("msvcrt.dll", "?lock@critical_section@Concurrency@@QEAAXXZ", st_cs_lock),
    SM("msvcrt.dll", "?unlock@critical_section@Concurrency@@QEAAXXZ", st_cs_unlock),
    SM("msvcrt.dll", "?try_lock@critical_section@Concurrency@@QEAA_NXZ", st_cs_try_lock),
    SM("msvcrt.dll", "??0_Condition_variable@details@Concurrency@@QEAA@XZ", st_cv_ctor),
    SM("msvcrt.dll", "??1_Condition_variable@details@Concurrency@@QEAA@XZ", st_cv_dtor),
    SM("msvcrt.dll",
       "?wait@_Condition_variable@details@Concurrency@@QEAAXAEAVcritical_section@3@@Z",
       st_cv_wait),
    SM("msvcrt.dll",
       "?wait_for@_Condition_variable@details@Concurrency@@QEAA_NAEAVcritical_section@3@I@Z",
       st_cv_wait_for),
    SM("msvcrt.dll",
       "?notify_one@_Condition_variable@details@Concurrency@@QEAAXXZ", st_cv_notify_one),
    SM("msvcrt.dll",
       "?notify_all@_Condition_variable@details@Concurrency@@QEAAXXZ", st_cv_notify_all),
    SM("msvcrt.dll",
       "?IsCurrentTaskCollectionCanceling@Context@Concurrency@@SA_NXZ",
       st_IsCurrentTaskCollectionCanceling),
    SM("msvcrt.dll", "?_set_new_handler@@YAP6AH_K@ZP6AH0@Z@Z", st_set_new_handler),
    /* operator new / delete, x86-64 then i386 decorations */
    SM("msvcrt.dll", "??2@YAPEAX_K@Z",  st_op_new),
    SM("msvcrt.dll", "??_U@YAPEAX_K@Z", st_op_new),
    SM("msvcrt.dll", "??3@YAXPEAX@Z",   st_op_delete),
    SM("msvcrt.dll", "??_V@YAXPEAX@Z",  st_op_delete),
    SM("msvcrt.dll", "??2@YAPAXI@Z",    st_op_new),
    SM("msvcrt.dll", "??_U@YAPAXI@Z",   st_op_new),
    SM("msvcrt.dll", "??3@YAXPAX@Z",    st_op_delete),
    SM("msvcrt.dll", "??_V@YAXPAX@Z",   st_op_delete),
    S("msvcrt.dll", memcpy), S("msvcrt.dll", memmove),
    S("msvcrt.dll", memset), S("msvcrt.dll", memcmp),
    S("msvcrt.dll", strlen), S("msvcrt.dll", strcpy), S("msvcrt.dll", strncpy),
    S("msvcrt.dll", strcat), S("msvcrt.dll", strcmp), S("msvcrt.dll", strncmp),
    S("msvcrt.dll", _stricmp), S("msvcrt.dll", strchr), S("msvcrt.dll", strrchr),
    S("msvcrt.dll", strstr), S("msvcrt.dll", _strdup), S("msvcrt.dll", wcslen),
    /* The wide CRT. Reached under msvcrt.dll for every 2015-and-later spelling
     * too -- crt_alias maps the api-ms-win-crt-* apisets and vcruntime onto
     * this table rather than duplicating it per name. */
    S("msvcrt.dll", wcschr), S("msvcrt.dll", wcsrchr), S("msvcrt.dll", wcsstr),
    S("msvcrt.dll", wcscmp), S("msvcrt.dll", wcsncmp), S("msvcrt.dll", _wcsicmp),
    S("msvcrt.dll", _wcsnicmp), S("msvcrt.dll", wcscpy), S("msvcrt.dll", wcsncpy),
    S("msvcrt.dll", wcscat), S("msvcrt.dll", _wcsdup),
    S("msvcrt.dll", wcstol), S("msvcrt.dll", wcstoul), S("msvcrt.dll", wcstod),
    S("msvcrt.dll", _wtoi), S("msvcrt.dll", _wtof),
    /* maths */
    S("msvcrt.dll", sqrt), S("msvcrt.dll", sin), S("msvcrt.dll", cos),
    S("msvcrt.dll", tan), S("msvcrt.dll", asin), S("msvcrt.dll", acos),
    S("msvcrt.dll", atan), S("msvcrt.dll", exp), S("msvcrt.dll", log),
    S("msvcrt.dll", log10), S("msvcrt.dll", floor), S("msvcrt.dll", ceil),
    S("msvcrt.dll", fabs), S("msvcrt.dll", sinh), S("msvcrt.dll", cosh),
    S("msvcrt.dll", tanh), S("msvcrt.dll", pow), S("msvcrt.dll", fmod),
    S("msvcrt.dll", atan2), S("msvcrt.dll", powf), S("msvcrt.dll", atan2f),
    S("msvcrt.dll", sqrtf), S("msvcrt.dll", sinf), S("msvcrt.dll", cosf),
    S("msvcrt.dll", tanf), S("msvcrt.dll", expf), S("msvcrt.dll", logf),
    S("msvcrt.dll", fabsf), S("msvcrt.dll", ldexp), S("msvcrt.dll", frexp),
    S("msvcrt.dll", modf),
    /* utility */
    S("msvcrt.dll", qsort), S("msvcrt.dll", bsearch), S("msvcrt.dll", abs),
    S("msvcrt.dll", atoi), S("msvcrt.dll", atof), S("msvcrt.dll", strtol),
    S("msvcrt.dll", strtod), S("msvcrt.dll", rand), S("msvcrt.dll", srand),
    S("msvcrt.dll", getenv), S("msvcrt.dll", strerror), S("msvcrt.dll", clock),
    S("msvcrt.dll", _putenv), S("msvcrt.dll", getenv_s),
    S("msvcrt.dll", _dupenv_s), S("msvcrt.dll", _wdupenv_s),
    /* Wine's debug entry points, imported by any Wine-built runtime DLL the
     * real-dependency loader maps in. */
    S("ntdll.dll", __wine_dbg_get_channel_flags),
    S("ntdll.dll", __wine_dbg_header),
    S("ntdll.dll", __wine_dbg_output),
    S("ntdll.dll", __wine_dbg_strdup),
    /* the .ini settings API, both widths */
    S("kernel32.dll", GetPrivateProfileStringA), S("kernel32.dll", GetPrivateProfileStringW),
    S("kernel32.dll", GetPrivateProfileIntA), S("kernel32.dll", GetPrivateProfileIntW),
    S("kernel32.dll", GetPrivateProfileSectionA), S("kernel32.dll", GetPrivateProfileSectionW),
    S("kernel32.dll", WritePrivateProfileStringA), S("kernel32.dll", WritePrivateProfileStringW),
    S("kernel32.dll", WritePrivateProfileSectionA), S("kernel32.dll", WritePrivateProfileSectionW),
    /* the CRT's file API: stdio, the low-level descriptors under it, and the
     * wide spellings of both. Kontakt reaches for the wide ones and got the
     * generic stub for every call before these existed. */
    S("msvcrt.dll", fopen), S("msvcrt.dll", _wfopen),
    S("msvcrt.dll", fopen_s), S("msvcrt.dll", _wfopen_s),
    S("msvcrt.dll", _fdopen), S("msvcrt.dll", _fileno),
    S("msvcrt.dll", fclose), S("msvcrt.dll", fread), S("msvcrt.dll", fwrite),
    S("msvcrt.dll", fseek), S("msvcrt.dll", _fseeki64),
    S("msvcrt.dll", ftell), S("msvcrt.dll", _ftelli64),
    S("msvcrt.dll", rewind), S("msvcrt.dll", feof), S("msvcrt.dll", ferror),
    S("msvcrt.dll", clearerr), S("msvcrt.dll", fflush),
    S("msvcrt.dll", fgetc), S("msvcrt.dll", fputc), S("msvcrt.dll", ungetc),
    S("msvcrt.dll", fgets), S("msvcrt.dll", fputs), S("msvcrt.dll", setvbuf),
    S("msvcrt.dll", _open), S("msvcrt.dll", _wopen), S("msvcrt.dll", _close),
    S("msvcrt.dll", _read), S("msvcrt.dll", _write),
    S("msvcrt.dll", _lseek), S("msvcrt.dll", _lseeki64), S("msvcrt.dll", _eof),
    S("msvcrt.dll", _access), S("msvcrt.dll", _waccess),
    S("msvcrt.dll", remove), S("msvcrt.dll", _wremove),
    S("msvcrt.dll", _unlink), S("msvcrt.dll", _wunlink),
    S("msvcrt.dll", rename), S("msvcrt.dll", _wrename),
    S("msvcrt.dll", _mkdir), S("msvcrt.dll", _wmkdir),
    S("msvcrt.dll", _vswprintf),
    S("msvcrt.dll", _time64), S("msvcrt.dll", _time32),
    /* interlocked */
    S("kernel32.dll", InterlockedIncrement), S("kernel32.dll", InterlockedDecrement),
    S("kernel32.dll", InterlockedExchange), S("kernel32.dll", InterlockedExchangeAdd),
    S("kernel32.dll", InterlockedCompareExchange),
    S("kernel32.dll", InterlockedOr), S("kernel32.dll", InterlockedAnd),
    S("kernel32.dll", InterlockedXor),
    S("kernel32.dll", InterlockedIncrement64), S("kernel32.dll", InterlockedDecrement64),
    S("kernel32.dll", InterlockedExchangeAdd64),
    S("kernel32.dll", InterlockedExchangePointer),
    S("kernel32.dll", InterlockedCompareExchangePointer),
    /* heap / memory */
    S("kernel32.dll", GetProcessHeap), S("kernel32.dll", HeapAlloc),
    S("kernel32.dll", HeapFree), S("kernel32.dll", HeapReAlloc),
    S("kernel32.dll", HeapSize), S("kernel32.dll", HeapCreate),
    S("kernel32.dll", HeapDestroy), S("kernel32.dll", HeapSetInformation),
    S("kernel32.dll", HeapValidate),
    S("kernel32.dll", VirtualAlloc), S("kernel32.dll", VirtualFree),
    S("kernel32.dll", VirtualProtect), S("kernel32.dll", VirtualQuery),
    S("kernel32.dll", GlobalAlloc), S("kernel32.dll", GlobalFree),
    S("kernel32.dll", GlobalLock), S("kernel32.dll", GlobalUnlock),
    S("kernel32.dll", LocalAlloc), S("kernel32.dll", LocalFree),
    S("kernel32.dll", LocalReAlloc), S("kernel32.dll", GlobalReAlloc),
    S("kernel32.dll", LocalLock), S("kernel32.dll", LocalUnlock),
    S("kernel32.dll", LocalHandle), S("kernel32.dll", GlobalHandle),
    S("kernel32.dll", LocalSize), S("kernel32.dll", GlobalSize),
    S("kernel32.dll", LocalFlags), S("kernel32.dll", GlobalFlags),
    /* sync */
    S("kernel32.dll", InitializeCriticalSection),
    S("kernel32.dll", InitializeCriticalSectionAndSpinCount),
    S("kernel32.dll", InitializeCriticalSectionEx),
    S("kernel32.dll", EnterCriticalSection), S("kernel32.dll", LeaveCriticalSection),
    S("kernel32.dll", TryEnterCriticalSection), S("kernel32.dll", DeleteCriticalSection),
    S("kernel32.dll", InitializeSRWLock),
    S("kernel32.dll", AcquireSRWLockExclusive), S("kernel32.dll", AcquireSRWLockShared),
    S("kernel32.dll", ReleaseSRWLockExclusive), S("kernel32.dll", ReleaseSRWLockShared),
    S("kernel32.dll", TryAcquireSRWLockExclusive),
    S("kernel32.dll", InitOnceBeginInitialize), S("kernel32.dll", InitOnceComplete),
    S("kernel32.dll", InitOnceExecuteOnce),
    S("kernel32.dll", InitializeConditionVariable),
    S("kernel32.dll", WakeConditionVariable), S("kernel32.dll", WakeAllConditionVariable),
    S("kernel32.dll", InitializeSListHead), S("kernel32.dll", InterlockedFlushSList),
    /* TLS */
    S("kernel32.dll", TlsAlloc), S("kernel32.dll", TlsFree),
    S("kernel32.dll", TlsGetValue), S("kernel32.dll", TlsSetValue),
    S("kernel32.dll", FlsAlloc), S("kernel32.dll", FlsFree),
    S("kernel32.dll", FlsGetValue), S("kernel32.dll", FlsSetValue),
    /* process / thread */
    S("kernel32.dll", GetLastError), S("kernel32.dll", SetLastError),
    S("kernel32.dll", GetCurrentThreadId), S("kernel32.dll", GetCurrentProcessId),
    S("kernel32.dll", GetCurrentProcess), S("kernel32.dll", GetCurrentThread),
    S("kernel32.dll", EncodePointer), S("kernel32.dll", DecodePointer),
    S("kernel32.dll", GetVolumeInformationW),
    S("kernel32.dll", IsProcessorFeaturePresent), S("kernel32.dll", IsDebuggerPresent),
    S("kernel32.dll", SetUnhandledExceptionFilter),
    S("kernel32.dll", UnhandledExceptionFilter),
    S("kernel32.dll", TerminateProcess), S("kernel32.dll", ExitProcess),
    S("kernel32.dll", RaiseException), S("kernel32.dll", SetErrorMode),
    S("kernel32.dll", GetVersion), S("kernel32.dll", Sleep),
    S("kernel32.dll", SwitchToThread), S("kernel32.dll", GetSystemInfo),
    /* time */
    S("kernel32.dll", QueryPerformanceCounter), S("kernel32.dll", QueryPerformanceFrequency),
    S("kernel32.dll", GetSystemTimeAsFileTime),
    S("kernel32.dll", GetTickCount), S("kernel32.dll", GetTickCount64),
    /* module / loader */
    S("kernel32.dll", GetModuleHandleA), S("kernel32.dll", GetModuleHandleW),
    S("kernel32.dll", GetModuleHandleExW), S("kernel32.dll", GetModuleHandleExA),
    S("kernel32.dll", WaitForMultipleObjects), S("kernel32.dll", WaitForMultipleObjectsEx),
    S("kernel32.dll", FindFirstChangeNotificationA),
    S("kernel32.dll", FindFirstChangeNotificationW),
    S("kernel32.dll", FindNextChangeNotification),
    S("kernel32.dll", FindCloseChangeNotification),
    S("kernel32.dll", GetModuleFileNameA), S("kernel32.dll", GetModuleFileNameW),
    S("kernel32.dll", LoadLibraryA), S("kernel32.dll", LoadLibraryW),
    S("kernel32.dll", LoadLibraryExW), S("kernel32.dll", LoadLibraryExA),
    S("kernel32.dll", FreeLibrary),
    S("kernel32.dll", GetProcAddress), S("kernel32.dll", DisableThreadLibraryCalls),
    S("kernel32.dll", GetCommandLineA), S("kernel32.dll", GetCommandLineW),
    S("kernel32.dll", GetEnvironmentStringsW), S("kernel32.dll", FreeEnvironmentStringsW),
    /* The environment a Windows plug-in expects to find -- see w32_env_build. */
    S("kernel32.dll", GetEnvironmentStringsA), S("kernel32.dll", FreeEnvironmentStringsA),
    S("kernel32.dll", GetEnvironmentVariableA), S("kernel32.dll", GetEnvironmentVariableW),
    S("kernel32.dll", SetEnvironmentVariableA), S("kernel32.dll", SetEnvironmentVariableW),
    S("kernel32.dll", ExpandEnvironmentStringsA), S("kernel32.dll", ExpandEnvironmentStringsW),
    S("kernel32.dll", GetStartupInfoW), S("kernel32.dll", GetStdHandle),
    S("kernel32.dll", GetStartupInfoA), S("kernel32.dll", GetSystemDirectoryA),
    S("kernel32.dll", GetWindowsDirectoryA), S("kernel32.dll", GetPrivateProfileStringA),
    S("kernel32.dll", GetPrivateProfileIntA),
    S("kernel32.dll", GetFullPathNameW), S("kernel32.dll", GetCurrentDirectoryW),
    S("kernel32.dll", CreateDirectoryW),
    S("kernel32.dll", SetHandleCount),
#ifndef PELOAD_NO_GUI_LAYER
    S("user32.dll", SetTimer), S("user32.dll", KillTimer),
#endif
    S("user32.dll", LoadBitmapA), S("user32.dll", LoadBitmapW),
    S("user32.dll", LoadStringA), S("user32.dll", LoadStringW),
    /* String walking and user32's own formatter. All six were resolving to the
     * generic stub, and the four Char* ones hand back a pointer the caller
     * dereferences straight away -- see their definitions. */
    S("user32.dll", CharNextA), S("user32.dll", CharPrevA),
    S("user32.dll", CharNextW), S("user32.dll", CharPrevW),
    S("user32.dll", wsprintfA), S("user32.dll", wsprintfW),
    S("user32.dll", wvsprintfA), S("user32.dll", wvsprintfW),
    S("shell32.dll", SHGetFolderPathA),
    S("kernel32.dll", SetStdHandle), S("kernel32.dll", OutputDebugStringA),
    /* locale */
    S("kernel32.dll", GetACP), S("kernel32.dll", GetOEMCP),
    S("kernel32.dll", IsValidCodePage), S("kernel32.dll", GetCPInfo),
    S("kernel32.dll", MultiByteToWideChar), S("kernel32.dll", WideCharToMultiByte),
    S("kernel32.dll", LCMapStringW), S("kernel32.dll", CompareStringW),
    S("kernel32.dll", GetLocaleInfoW), S("kernel32.dll", GetUserDefaultLCID),
    S("kernel32.dll", IsValidLocale), S("kernel32.dll", GetStringTypeW),
    S("kernel32.dll", EnumSystemLocalesW), S("kernel32.dll", GetTimeZoneInformation),
    /* SEH */
    S("kernel32.dll", RtlCaptureContext), S("kernel32.dll", RtlLookupFunctionEntry),
    S("kernel32.dll", RtlVirtualUnwind), S("kernel32.dll", RtlPcToFileHeader),
    S("kernel32.dll", RtlUnwindEx), S("kernel32.dll", RtlAddFunctionTable),
#if defined(__i386__)
    S("kernel32.dll", RtlUnwind), S("ntdll.dll", RtlUnwind),
#endif
    /* files */
    S("kernel32.dll", CreateFileA), S("kernel32.dll", ReadFile),
    S("kernel32.dll", WriteFile), S("kernel32.dll", CloseHandle),
    S("kernel32.dll", SetFilePointer), S("kernel32.dll", GetFileSize),
    S("kernel32.dll", FlushFileBuffers), S("kernel32.dll", GetFileType),
    S("kernel32.dll", SetEndOfFile), S("kernel32.dll", GetFileAttributesA),
    S("kernel32.dll", CreateFileW), S("kernel32.dll", GetFileAttributesW),
    S("kernel32.dll", DeleteFileW), S("kernel32.dll", GetFileInformationByHandle),
    /* events / semaphores / mutexes / threads */
    S("kernel32.dll", CreateEventA), S("kernel32.dll", CreateEventW),
    S("kernel32.dll", CreateEventExW),
    S("kernel32.dll", SetEvent), S("kernel32.dll", ResetEvent),
    S("kernel32.dll", WaitForSingleObject), S("kernel32.dll", WaitForSingleObjectEx),
    S("kernel32.dll", CreateSemaphoreA), S("kernel32.dll", CreateSemaphoreW),
    S("kernel32.dll", ReleaseSemaphore),
    S("kernel32.dll", CreateMutexA), S("kernel32.dll", ReleaseMutex),
    S("kernel32.dll", SleepConditionVariableCS),
    S("kernel32.dll", SleepConditionVariableSRW),
    S("kernel32.dll", CreateThread), S("kernel32.dll", ExitThread),
    S("kernel32.dll", GetExitCodeThread), S("kernel32.dll", ResumeThread),
    S("kernel32.dll", SetThreadPriority), S("kernel32.dll", GetThreadPriority),
    S("kernel32.dll", DeleteFileA), S("kernel32.dll", GetTempPathA),
    S("kernel32.dll", GetFullPathNameA), S("kernel32.dll", GetCurrentDirectoryA),
    S("kernel32.dll", AreFileApisANSI), S("kernel32.dll", WriteConsoleW),
    S("kernel32.dll", GetConsoleMode), S("kernel32.dll", GetConsoleCP),
    S("kernel32.dll", GetConsoleOutputCP),
    /* resources */
    S("kernel32.dll", EnumResourceNamesA), S("kernel32.dll", EnumResourceNamesW),
    S("kernel32.dll", GetVersionExA), S("kernel32.dll", GetVersionExW),
    S("kernel32.dll", VerSetConditionMask),
    S("kernel32.dll", VerifyVersionInfoW), S("kernel32.dll", VerifyVersionInfoA),
    S("shlwapi.dll", PathFileExistsA), S("shlwapi.dll", PathFileExistsW),
    S("kernel32.dll", FindResourceA), S("kernel32.dll", FindResourceW),
    S("kernel32.dll", SizeofResource), S("kernel32.dll", LoadResource),
    S("kernel32.dll", LockResource), S("kernel32.dll", FreeResource),
    /* ole */
    S("ole32.dll", CoTaskMemAlloc), S("ole32.dll", CoTaskMemFree),
    S("ole32.dll", OleInitialize), S("ole32.dll", OleUninitialize),
    S("ole32.dll", CoInitialize), S("ole32.dll", CoUninitialize),
#ifndef PELOAD_NO_GUI_LAYER
    /* user32: windows */
    S("user32.dll", RegisterClassA), S("user32.dll", RegisterClassW),
    S("user32.dll", UnregisterClassA), S("user32.dll", UnregisterClassW),
    S("user32.dll", CreateWindowExA), S("user32.dll", CreateWindowExW),
    S("user32.dll", DestroyWindow), S("user32.dll", ShowWindow),
    S("user32.dll", SetWindowPos), S("user32.dll", MoveWindow),
    S("user32.dll", GetWindowRect), S("user32.dll", GetClientRect),
    S("user32.dll", GetWindowInfo),
    S("user32.dll", GetParent), S("user32.dll", GetAncestor),
    S("user32.dll", GetClassNameA), S("user32.dll", GetClassNameW),
    S("user32.dll", SetWindowTextA), S("user32.dll", SetWindowTextW),
    S("user32.dll", BringWindowToTop), S("user32.dll", GetWindowThreadProcessId),
    S("user32.dll", EnumWindows),
    S("user32.dll", GetWindowLongA), S("user32.dll", GetWindowLongW),
    S("user32.dll", GetWindowLongPtrA), S("user32.dll", GetWindowLongPtrW),
    S("user32.dll", SetWindowLongA), S("user32.dll", SetWindowLongW),
    S("user32.dll", SetWindowLongPtrA), S("user32.dll", SetWindowLongPtrW),
    /* user32: messages */
    S("user32.dll", DefWindowProcA), S("user32.dll", DefWindowProcW),
    S("user32.dll", SendMessageA), S("user32.dll", SendMessageW),
    S("user32.dll", PostMessageA), S("user32.dll", PostMessageW),
    S("user32.dll", PeekMessageA), S("user32.dll", TranslateMessage),
    S("user32.dll", DispatchMessageA),
    S("user32.dll", CallWindowProcA), S("user32.dll", CallWindowProcW),
    S("user32.dll", RegisterWindowMessageA), S("user32.dll", RegisterWindowMessageW),
    S("user32.dll", GetMessageExtraInfo),
    /* user32: painting */
    S("user32.dll", BeginPaint), S("user32.dll", EndPaint),
    S("user32.dll", GetDC), S("user32.dll", GetWindowDC), S("user32.dll", ReleaseDC),
    S("user32.dll", InvalidateRect), S("user32.dll", ValidateRect),
    S("user32.dll", GetUpdateRect), S("user32.dll", GetUpdateRgn),
    S("user32.dll", UpdateWindow), S("user32.dll", RedrawWindow),
    S("user32.dll", FillRect), S("user32.dll", DrawTextA),
    /* user32: input */
    S("user32.dll", SetCapture), S("user32.dll", ReleaseCapture), S("user32.dll", GetCapture),
    S("user32.dll", WindowFromPoint), S("user32.dll", ChildWindowFromPoint),
    S("user32.dll", ChildWindowFromPointEx),
    S("user32.dll", SetClassLongPtrW), S("user32.dll", SetClassLongPtrA),
    S("user32.dll", SetClassLongW), S("user32.dll", SetClassLongA),
    S("user32.dll", SetFocus), S("user32.dll", GetCursorPos), S("user32.dll", SetCursorPos),
    S("user32.dll", SetCursor), S("user32.dll", GetCursor), S("user32.dll", LoadCursorA),
    S("user32.dll", ShowCursor), S("user32.dll", GetKeyState), S("user32.dll", GetAsyncKeyState),
    S("user32.dll", GetKeyboardState), S("user32.dll", GetKeyboardLayout),
    S("user32.dll", ToAscii), S("user32.dll", VkKeyScanExA),
    S("user32.dll", TrackMouseEvent), S("user32.dll", GetDoubleClickTime),
    S("user32.dll", ClientToScreen), S("user32.dll", ScreenToClient),
    S("user32.dll", MapWindowPoints),
    /* user32: system + misc */
    S("user32.dll", GetSysColor), S("user32.dll", GetSysColorBrush),
    S("user32.dll", GetSystemMetrics), S("user32.dll", SystemParametersInfoW),
    S("user32.dll", MessageBoxA), S("user32.dll", MessageBoxW),
    S("user32.dll", OpenClipboard), S("user32.dll", CloseClipboard),
    S("user32.dll", EmptyClipboard), S("user32.dll", GetClipboardData),
    S("user32.dll", SetClipboardData), S("user32.dll", IsClipboardFormatAvailable),
    S("user32.dll", CreatePopupMenu), S("user32.dll", AppendMenuA),
    S("user32.dll", AppendMenuW), S("user32.dll", DestroyMenu),
    S("user32.dll", TrackPopupMenu), S("user32.dll", RegisterTouchWindow),
    S("user32.dll", CloseTouchInputHandle), S("user32.dll", GetTouchInputInfo),
    /* gdi32 */
    S("gdi32.dll", StretchDIBits), S("gdi32.dll", BitBlt),
    S("gdi32.dll", CreateCompatibleDC), S("gdi32.dll", CreateCompatibleBitmap),
    S("gdi32.dll", CreateBitmap), S("gdi32.dll", CreateDIBSection),
    S("gdi32.dll", CreateFontA), S("gdi32.dll", GetBkColor),
    S("gdi32.dll", GetBkMode),
    S("gdi32.dll", TextOutA), S("gdi32.dll", TextOutW),
    S("gdi32.dll", ExtTextOutA), S("gdi32.dll", ExtTextOutW),
    S("gdi32.dll", SetTextAlign), S("gdi32.dll", GetTextAlign),
    S("gdi32.dll", GetTextMetricsA), S("gdi32.dll", GetTextMetricsW),
    S("gdi32.dll", GetTextExtentPointA),
    S("user32.dll", DrawTextA), S("user32.dll", DrawTextW),
    S("kernel32.dll", GetVolumeInformationA),
    S("kernel32.dll", CreateDirectoryA),
    S("shlwapi.dll", PathIsUNCA), S("shlwapi.dll", PathStripToRootA),
    S("winmm.dll", timeGetTime), S("winmm.dll", timeBeginPeriod),
    S("winmm.dll", timeEndPeriod), S("user32.dll", GetClassInfoA),
    S("gdi32.dll", CreateHalftonePalette), S("gdi32.dll", SelectPalette),
    S("gdi32.dll", RealizePalette),
    S("kernel32.dll", lstrlenA), S("kernel32.dll", lstrcpyA),
    S("kernel32.dll", lstrcatA), S("kernel32.dll", lstrcmpA),
    S("kernel32.dll", lstrcmpiA),
    S("kernel32.dll", lstrcpynA), S("kernel32.dll", lstrcpynW),
    S("gdi32.dll", SetDIBits), S("gdi32.dll", GetDIBits),
    S("gdi32.dll", CreateDIBitmap),
    S("gdi32.dll", SetRectRgn), S("gdi32.dll", CombineRgn),
    S("gdi32.dll", ExtSelectClipRgn),
    S("gdi32.dll", CreatePen), S("gdi32.dll", Rectangle),
    S("gdi32.dll", PatBlt), S("gdi32.dll", SetPixel),
    S("gdi32.dll", SetPixelV), S("gdi32.dll", GetPixel),
    S("gdi32.dll", Polyline), S("gdi32.dll", GetTextColor),
    S("gdi32.dll", RectVisible), S("gdi32.dll", IntersectClipRect),
    S("gdi32.dll", ExcludeClipRect), S("gdi32.dll", SetViewportOrgEx),
    S("gdi32.dll", OffsetViewportOrgEx), S("gdi32.dll", GetViewportOrgEx),
    S("gdi32.dll", SetWindowOrgEx), S("gdi32.dll", GetWindowOrgEx),
    S("user32.dll", SetRect), S("user32.dll", SetRectEmpty),
    S("user32.dll", CopyRect), S("user32.dll", OffsetRect),
    S("user32.dll", InflateRect), S("user32.dll", IntersectRect),
    S("user32.dll", UnionRect), S("user32.dll", SubtractRect),
    S("user32.dll", EqualRect), S("user32.dll", IsRectEmpty),
    S("user32.dll", PtInRect),
    S("user32.dll", SetWindowsHookExA), S("user32.dll", SetWindowsHookExW),
    S("user32.dll", UnhookWindowsHookEx), S("user32.dll", CallNextHookEx),
    S("gdi32.dll", DeleteDC),
    S("gdi32.dll", SelectObject), S("gdi32.dll", DeleteObject),
    S("gdi32.dll", GetCurrentObject), S("gdi32.dll", GetObjectA), S("gdi32.dll", GetObjectW),
    S("gdi32.dll", CreateSolidBrush), S("gdi32.dll", CreateBrushIndirect),
    S("gdi32.dll", CreatePenIndirect),
    S("gdi32.dll", CreateFontIndirectA), S("gdi32.dll", CreateFontIndirectW),
    S("gdi32.dll", CreateRectRgn), S("gdi32.dll", SelectClipRgn),
    S("gdi32.dll", GetRgnBox), S("gdi32.dll", GetRegionData),
    S("gdi32.dll", GetStockObject),
    S("gdi32.dll", SetBkColor), S("gdi32.dll", SetBkMode), S("gdi32.dll", SetTextColor),
    S("gdi32.dll", SetDCBrushColor), S("gdi32.dll", SetROP2),
    S("gdi32.dll", MoveToEx), S("gdi32.dll", LineTo), S("gdi32.dll", Ellipse),
    S("gdi32.dll", DPtoLP),
    S("gdi32.dll", GetFontData), S("gdi32.dll", AddFontMemResourceEx),
    S("gdi32.dll", RemoveFontMemResourceEx),
    S("gdi32.dll", EnumFontFamiliesExA), S("gdi32.dll", EnumFontFamiliesExW),
    S("gdi32.dll", GetTextExtentPoint32A),
    S("gdi32.dll", GetTextFaceA), S("gdi32.dll", GetTextFaceW),
    /* timers */
    S("user32.dll", SetTimer), S("user32.dll", KillTimer),
    /* shell32 / comctl32 / ole32 extras */
    S("shell32.dll", DragAcceptFiles), S("shell32.dll", DragQueryFileA),
    S("shell32.dll", DragQueryFileW), S("shell32.dll", DragQueryPoint),
    S("comctl32.dll", InitCommonControlsEx),
    S("ole32.dll", RegisterDragDrop), S("ole32.dll", RevokeDragDrop),
    S("ole32.dll", DoDragDrop), S("ole32.dll", CoCreateGuid),
#endif
    /* registry */
    S("advapi32.dll", RegOpenKeyExA), S("advapi32.dll", RegQueryValueExA),
    S("advapi32.dll", RegCloseKey),
    { NULL, NULL, NULL }
};
#undef S

/* Every MSVC vintage ships the same CRT surface under its own DLL name --
 * msvcr90/100/110/120, the msvcp C++ halves, vcruntime140, ucrtbase -- so a
 * stub registered once against msvcrt.dll answers for all of them. Matching
 * the literal name instead would need a fresh copy of the table per vintage. */
/* Every spelling of the C runtime resolves to the one implementation here.
 *
 * The 2015 toolchain split the CRT into fifteen apiset DLLs --
 * api-ms-win-crt-runtime-l1-1-0.dll, -stdio-, -heap-, -math- and so on -- each
 * a forwarder onto ucrtbase. A plug-in built with MSVC 2015 or later imports
 * every CRT function through those names rather than through msvcrt.dll, so
 * without them here the whole C runtime missed the table and fell through to
 * the generic do-nothing stub.
 *
 * The one that made it fatal rather than merely wrong is _initterm_e. It walks
 * an array of initialiser function pointers and calls each one, which is how a
 * C++ module runs its static constructors; stubbed out, it returns success
 * having run none of them. A side-loaded MSVCP140 therefore mapped, reported
 * that it had initialised, and left every locale and iostream global null --
 * and the plug-in that went on to use one faulted a long way from the cause.
 * The implementation was already here and registered under msvcrt.dll; only
 * the name was missing.
 *
 * Prefix rather than exact match: the apiset names carry a version suffix that
 * changes between Windows releases (-l1-1-0 today), and the import table's copy
 * is truncated by some plug-ins besides. */
static const char *crt_alias(const char *dll)
{
    if (!strncasecmp(dll, "msvcr", 5) || !strncasecmp(dll, "msvcp", 5) ||
        !strncasecmp(dll, "vcruntime", 9) || !strncasecmp(dll, "ucrtbase", 8) ||
        !strncasecmp(dll, "api-ms-win-crt-", 15))
        return "msvcrt.dll";
    return NULL;
}

static void *winstub_lookup(const char *dll, const char *sym)
{
    const char *alias;
    int i;
    for (i = 0; g_stubs[i].sym; i++)
        if (!strcasecmp(g_stubs[i].dll, dll) && !strcmp(g_stubs[i].sym, sym))
            return g_stubs[i].fn;
    if ((alias = crt_alias(dll)) != NULL && strcasecmp(alias, dll) != 0)
        for (i = 0; g_stubs[i].sym; i++)
            if (!strcasecmp(g_stubs[i].dll, alias) && !strcmp(g_stubs[i].sym, sym))
                return g_stubs[i].fn;
    return NULL;
}

#endif /* PELOAD_WINSTUBS_H */
