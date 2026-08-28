/* Mach-O loader. See machoload.h for why this is smaller than the PE side.
 *
 * The structure definitions here are written out rather than pulled from Apple's
 * <mach-o/loader.h>, which is not on a Linux box. They are the documented
 * on-disk layout and nothing more.
 *
 * The interesting part is the fixup encoding. Where PE has a flat relocation
 * table and an import descriptor array, Mach-O ships two little bytecode
 * streams in LC_DYLD_INFO_ONLY: a rebase program that slides absolute pointers
 * by the load bias, and a bind program that writes imported addresses. Both are
 * opcode-per-byte with ULEB operands, and both are interpreted below.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "machoload.h"
#include "macshim.h"
#include "macobjc.h"

/* ------------------------------------------------------------ format layout */

#define FAT_MAGIC_BE   0xBEBAFECAu        /* 0xCAFEBABE byte-swapped */
#define FAT_MAGIC_BE64 0xBFBAFECAu
#define MH_MAGIC_64    0xFEEDFACFu
#define CPU_TYPE_X86_64 0x01000007

#define MH_BUNDLE 8
#define MH_DYLIB  6

#define LC_REQ_DYLD        0x80000000u
#define LC_SEGMENT_64      0x19
#define LC_SYMTAB          0x02
#define LC_DYSYMTAB        0x0B
#define LC_LOAD_DYLIB      0x0C
#define LC_ID_DYLIB        0x0D
#define LC_LOAD_WEAK_DYLIB (0x18 | LC_REQ_DYLD)
#define LC_REEXPORT_DYLIB  (0x1F | LC_REQ_DYLD)
#define LC_DYLD_INFO       0x22
#define LC_DYLD_INFO_ONLY  (0x22 | LC_REQ_DYLD)
#define LC_DYLD_EXPORTS_TRIE (0x33 | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS (0x34 | LC_REQ_DYLD)

typedef struct { uint32_t magic, nfat_arch; } fat_header;
typedef struct { uint32_t cputype, cpusubtype, offset, size, align; } fat_arch;

typedef struct {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
} mach_header_64;

typedef struct { uint32_t cmd, cmdsize; } load_command;

typedef struct {
    uint32_t cmd, cmdsize;
    char     segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
} segment_command_64;

typedef struct {
    char     sectname[16], segname[16];
    uint64_t addr, size;
    uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
} section_64;

typedef struct {
    uint32_t cmd, cmdsize, rebase_off, rebase_size, bind_off, bind_size;
    uint32_t weak_bind_off, weak_bind_size, lazy_bind_off, lazy_bind_size;
    uint32_t export_off, export_size;
} dyld_info_command;

typedef struct {
    uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
} symtab_command;

typedef struct { uint32_t n_strx; uint8_t n_type, n_sect; uint16_t n_desc; uint64_t n_value; } nlist_64;

/* The pre-opcode binding mechanism. An image built before LC_DYLD_INFO -- or by
 * a toolchain targeting an older macOS -- carries no bind streams at all. Its
 * imports live in symbol-pointer sections whose slots are described by the
 * indirect symbol table, and its rebasing lives in ordinary relocation entries. */
typedef struct {
    uint32_t cmd, cmdsize;
    uint32_t ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym;
    uint32_t tocoff, ntoc, modtaboff, nmodtab;
    uint32_t extrefsymoff, nextrefsyms;
    uint32_t indirectsymoff, nindirectsyms;
    uint32_t extreloff, nextrel, locreloff, nlocrel;
} dysymtab_command;

typedef struct {
    int32_t  r_address;
    uint32_t r_bits;             /* symbolnum:24, pcrel:1, length:2, extern:1, type:4 */
} reloc_info;
#define RELOC_SYMBOLNUM(r) ((r)->r_bits & 0x00ffffffu)
#define RELOC_EXTERN(r)    (((r)->r_bits >> 27) & 1u)
#define RELOC_LENGTH(r)    (((r)->r_bits >> 25) & 3u)

#define S_NON_LAZY_SYMBOL_POINTERS 0x6
#define S_LAZY_SYMBOL_POINTERS     0x7
#define INDIRECT_SYMBOL_LOCAL 0x80000000u
#define INDIRECT_SYMBOL_ABS   0x40000000u

/* rebase opcodes */
#define REBASE_OPCODE_MASK      0xF0
#define REBASE_IMMEDIATE_MASK   0x0F
#define REBASE_OPCODE_DONE                      0x00
#define REBASE_OPCODE_SET_TYPE_IMM              0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20
#define REBASE_OPCODE_ADD_ADDR_ULEB             0x30
#define REBASE_OPCODE_ADD_ADDR_IMM_SCALED       0x40
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES       0x50
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES      0x60
#define REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB   0x70
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80

/* bind opcodes */
#define BIND_OPCODE_MASK        0xF0
#define BIND_IMMEDIATE_MASK     0x0F
#define BIND_OPCODE_DONE                        0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM       0x10
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB      0x20
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM       0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OPCODE_SET_TYPE_IMM                0x50
#define BIND_OPCODE_SET_ADDEND_SLEB             0x60
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70
#define BIND_OPCODE_ADD_ADDR_ULEB               0x80
#define BIND_OPCODE_DO_BIND                     0x90
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB       0xA0
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED 0xB0
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0xC0

#define BIND_SYMBOL_FLAGS_WEAK_IMPORT 0x1
#define EXPORT_SYMBOL_FLAGS_REEXPORT  0x08
#define EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER 0x10

/* --------------------------------------------------------------- the object */

#define MAX_SEG   16
#define MAX_IMP   4096
#define MAX_EXP   4096

typedef struct { char name[192]; unsigned long calls; } imprec;
typedef struct { char name[192]; void *addr; } exprec;

struct macho {
    uint8_t  *base;                  /* mapped image */
    size_t    span;                  /* bytes reserved */
    uint8_t  *file;                  /* the whole file, mapped read-only */
    size_t    filelen;
    /* Every file offset in a load command is relative to the start of its own
     * Mach-O image. In a universal binary that is the slice, not the file, so
     * all of them are read through `slice` rather than `file`. Getting this
     * wrong is quiet: the segment copies still succeed, just from the wrong
     * bytes, and the first thing that complains is a bind opcode stream that
     * decodes to nonsense. */
    uint8_t  *slice;
    size_t    slicelen;
    uint64_t  slide;                 /* base - lowest vmaddr */
    const mach_header_64 *mh;        /* inside `file` */

    segment_command_64 *seg[MAX_SEG];
    int       nseg;
    dyld_info_command  *dyld;
    symtab_command     *symtab;
    dysymtab_command   *dysym;

    imprec    imp[MAX_IMP];
    int       nimp, nresolved;
    exprec    exp[MAX_EXP];
    int       nexp;

    void     *init_funcs;            /* &__mod_init_func */
    size_t    init_count;

    char      bundle_id[128], bundle_name[128];
    /* The bundle directory. A plugin loads its own artwork and presets relative
     * to this, so without it every resource lookup returns nothing -- which
     * presents as a plugin that renders silence or faults on a null bitmap
     * rather than as a missing path. */
    char      bundle_path[4096];
};

/* The most recently opened image, so a diagnostic can describe an address while
 * macho_open itself is still on the stack -- which is exactly when a plugin that
 * hangs in its own initialisation needs describing. */
static macho *g_last_image;

static char g_err[512];
const char *macho_last_error(void) { return g_err; }
static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

static int mo_verbose(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("MACHO_VERBOSE"); v = e && *e != '0'; } return v; }
#define MLOG(...) do { if (mo_verbose()) fprintf(stderr, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------- ULEB */

static uint64_t uleb(const uint8_t **p, const uint8_t *end)
{
    uint64_t r = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
        if (shift > 63) break;
    }
    return r;
}

static int64_t sleb(const uint8_t **p, const uint8_t *end)
{
    int64_t r = 0;
    int shift = 0;
    uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        r |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40)) r |= -((int64_t)1 << shift);
    return r;
}

/* ------------------------------------------------- host symbol resolution */

/* Most of what a macOS plugin imports from libSystem is plain POSIX, and most of
 * what it imports from libc++ has the same Itanium mangling GCC uses -- so the
 * host's own libraries answer a good half of the import table directly. Only
 * what is left needs writing by hand. */
static void *host_lookup(const char *sym)
{
    static void *libs[12];
    static int n;
    int i, have_libcxx = 0;

    if (!n) {
        /* LLVM's libc++ is what Apple's is built from, and it uses the same
         * std::__1 inline namespace -- so 93 of the 97 libc++ symbols these
         * plugins import resolve straight against it. It is often not installed,
         * so a copy unpacked under thirdparty/ is looked for first; that needs no
         * root and changes nothing outside this tree. MACSHIM_LIBDIR overrides.
         *
         * libstdc++ stays in the list behind it: it answers the plain Itanium
         * names (operator new, __cxa_*) whatever libc++ is or is not present. */
        static const char *names[] = {
            "libc++.so.1", "libc++abi.so.1", "libstdc++.so.6",
            "libm.so.6", "libpthread.so.0", "libc.so.6", NULL
        };
        /* libc++abi first: libc++.so.1 depends on it, and dlopen of the
         * dependant fails outright if the dependency is not already loaded or
         * on the search path. */
        static const char *local[] = {
            "thirdparty/libcxxabi/usr/lib/libc++abi.so.1",
            "thirdparty/libcxx/usr/lib/libc++.so.1", NULL
        };
        char root[4096] = { 0 };
        const char *env = getenv("MACSHIM_LIBDIR");
        if (env && *env) {
            snprintf(root, sizeof root, "%s", env);
        } else {
            /* Walk up from the executable looking for thirdparty/, rather than
             * assuming a fixed depth. Stripping exactly two components works for
             * build/peload and silently fails for anything else -- and what it
             * fails at is loading libc++, so the symptom is a plugin crashing
             * somewhere unrelated with a handful of std:: imports unresolved. */
            ssize_t len = readlink("/proc/self/exe", root, sizeof root - 1);
            if (len > 0) {
                char *slash;
                root[len] = 0;
                while ((slash = strrchr(root, '/'))) {
                    struct stat st;
                    char probe[4200];
                    *slash = 0;
                    if (!root[0]) break;
                    snprintf(probe, sizeof probe, "%s/thirdparty", root);
                    if (!stat(probe, &st) && S_ISDIR(st.st_mode)) break;
                }
            } else {
                root[0] = 0;
            }
        }
        for (i = 0; local[i] && n < 12; i++) {
            char p[4600];
            void *h;
            if (!root[0]) break;
            snprintf(p, sizeof p, "%s/%s", root, local[i]);
            if ((h = dlopen(p, RTLD_LAZY | RTLD_GLOBAL))) {
                MLOG("  [macho] using %s\n", p);
                if (strstr(local[i], "libc++.so")) have_libcxx = 1;
                libs[n++] = h;
            } else {
                MLOG("  [macho] %s: %s\n", p, dlerror());
            }
        }
        for (i = 0; names[i] && n < 12; i++) {
            void *h = dlopen(names[i], RTLD_LAZY | RTLD_GLOBAL);
            if (h) {
                if (!strncmp(names[i], "libc++", 6)) have_libcxx = 1;
                libs[n++] = h;
            }
        }
        /* Worth saying out loud rather than logging: without libc++ a plugin
         * loses its std:: imports, and what the user sees is a crash somewhere
         * unrelated -- inside a static destructor, or in memcpy on a string that
         * was never constructed. Naming the cause here saves that hunt. */
        if (!have_libcxx)
            fprintf(stderr, "  [macho] warning: no libc++ found (looked under %s "
                            "and on the library path). A plugin's std:: imports "
                            "will not resolve; set MACSHIM_LIBDIR to the tree "
                            "holding thirdparty/.\n",
                    root[0] ? root : "(no root found)");
        if (n < 12) libs[n++] = RTLD_DEFAULT;      /* whatever we are linked to */
    }
    /* Mach-O prefixes every C symbol with an underscore the ELF world does not. */
    if (sym[0] == '_') sym++;
    for (i = 0; i < n; i++) {
        void *p = dlsym(libs[i], sym);
        if (p) return p;
    }
    return NULL;
}

/* An unresolved import gets a stub that names itself when called. Same idea as
 * the PE loader's: a plugin importing a symbol is not evidence it calls it, and
 * the difference decides whether it needs implementing. */
static uint8_t *g_tramp;
static size_t   g_tramp_used;
static macho   *g_stub_owner;

static void stub_report(unsigned long idx)
{
    macho *m = g_stub_owner;
    if (!m || idx >= (unsigned long)m->nimp) return;
    if (m->imp[idx].calls++ == 0)
        fprintf(stderr, "  [macho] unimplemented: %s\n", m->imp[idx].name);
}

static void *make_stub(unsigned long idx)
{
    uint8_t *p;

    if (!g_tramp) {
        g_tramp = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_tramp == MAP_FAILED) { g_tramp = NULL; return NULL; }
    }
    if (g_tramp_used + 32 > (1u << 20)) return NULL;
    p = g_tramp + g_tramp_used;
    g_tramp_used += 32;

    /* System V AMD64: first argument in %rdi, and the caller cleans up -- so
     * unlike i386 stdcall there is no argument count to get right. */
    *p++ = 0x48; *p++ = 0xBF; memcpy(p, &idx, 8); p += 8;      /* mov rdi, imm64 */
    *p++ = 0x48; *p++ = 0xB8;
    { void *f = (void *)stub_report; memcpy(p, &f, 8); p += 8; } /* mov rax, fn   */
    *p++ = 0x50;                                                /* push rax (align) */
    *p++ = 0x58;                                                /* pop rax         */
    *p++ = 0xFF; *p++ = 0xD0;                                   /* call rax        */
    *p++ = 0x48; *p++ = 0x31; *p++ = 0xC0;                      /* xor rax, rax    */
    *p++ = 0xC3;                                                /* ret             */
    return g_tramp + g_tramp_used - 32;
}

static void *resolve(macho *m, const char *sym, int weak)
{
    /* Our own implementations come first: a name we shim deliberately must win
     * over anything the host libraries happen to export under the same spelling. */
    void *p = macshim_lookup(sym);
    /* The objc runtime answers both _objc_* and the _OBJC_CLASS_$_* class
     * objects, which are data symbols generated per name rather than listed. */
    if (!p) p = macobjc_lookup_symbol(sym);
    if (!p) p = host_lookup(sym);
    int i;

    /* A template instantiation is emitted into every image that uses it and
     * marked N_WEAK_DEF, and the call still goes through a bound pointer so dyld
     * can coalesce the copies. With one image there is nothing to coalesce and
     * the definition is right here -- so an import naming something this image
     * defines binds to that, rather than to a stub that faults when called.
     * Last, so a shim or a host library still wins, which is the order dyld
     * would reach the same names in. */
    if (!p) p = macho_symbol(m, sym);

    if (p) { m->nresolved++; return p; }
    /* A weak import is allowed to be missing: the plugin tests it against NULL
     * before use, so a stub would be worse than nothing. */
    if (weak) return NULL;

    for (i = 0; i < m->nimp; i++)
        if (!strcmp(m->imp[i].name, sym)) return make_stub((unsigned long)i);
    if (m->nimp < MAX_IMP) {
        snprintf(m->imp[m->nimp].name, sizeof m->imp[m->nimp].name, "%s", sym);
        m->nimp++;
        return make_stub((unsigned long)(m->nimp - 1));
    }
    return NULL;
}

/* --------------------------------------------------------------- mapping */

static uint8_t *seg_addr(macho *m, int idx, uint64_t off)
{
    if (idx < 0 || idx >= m->nseg) return NULL;
    return m->base + (m->seg[idx]->vmaddr + off);
}

static int map_image(macho *m)
{
    uint64_t lo = ~0ull, hi = 0;
    int i;

    for (i = 0; i < m->nseg; i++) {
        segment_command_64 *s = m->seg[i];
        if (!s->vmsize) continue;
        /* Both values come from the file, so their sum can wrap -- and a wrapped
         * `hi` yields a span too small for the very addresses it was computed
         * from, which the copy below would then write past. */
        if (s->vmaddr + s->vmsize < s->vmaddr || s->vmsize > (1ull << 32))
            return fail("segment %.16s has an implausible size", s->segname);
        if (s->vmaddr < lo) lo = s->vmaddr;
        if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
    }
    if (lo == ~0ull) return fail("no segments to map");
    /* A bundle is linked at 0, so `lo` is 0 and the slide is just the base. */
    m->span = (size_t)(hi - lo + 0xFFFF) & ~(size_t)0xFFFF;
    m->base = mmap(NULL, m->span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m->base == MAP_FAILED) { m->base = NULL; return fail("mmap %zu: %s", m->span, strerror(errno)); }
    m->slide = (uint64_t)(uintptr_t)m->base - lo;

    for (i = 0; i < m->nseg; i++) {
        segment_command_64 *s = m->seg[i];
        uint8_t *at = m->base + (s->vmaddr - lo);
        size_t len = (size_t)((s->vmsize + 0xFFF) & ~0xFFFull);
        if (!len) continue;
        if (mprotect(at, len, PROT_READ | PROT_WRITE))
            return fail("mprotect %.16s: %s", s->segname, strerror(errno));
        if (s->filesize) {
            if (s->fileoff + s->filesize < s->fileoff ||
                s->fileoff + s->filesize > m->slicelen)
                return fail("segment %.16s runs past the slice", s->segname);
            /* The destination bound too: filesize is independent of vmsize in the
             * file, and a segment claiming more file bytes than virtual space
             * would write into whatever follows it -- or past the mapping
             * entirely, if it is the last one. */
            if (s->filesize > s->vmsize)
                return fail("segment %.16s claims more file than vm space",
                            s->segname);
            memcpy(at, m->slice + s->fileoff, (size_t)s->filesize);
        }
        MLOG("  [macho] %-16.16s %012llx +%-8llx file %08llx prot %x\n",
             s->segname, (unsigned long long)(uintptr_t)at,
             (unsigned long long)s->vmsize, (unsigned long long)s->fileoff,
             s->initprot);
    }
    return 0;
}

static int protect_segments(macho *m)
{
    int i;
    for (i = 0; i < m->nseg; i++) {
        segment_command_64 *s = m->seg[i];
        int prot = 0;
        size_t len = (size_t)((s->vmsize + 0xFFF) & ~0xFFFull);
        if (!len) continue;
        if (s->initprot & 1) prot |= PROT_READ;
        if (s->initprot & 2) prot |= PROT_WRITE;
        if (s->initprot & 4) prot |= PROT_EXEC;
        if (!prot) prot = PROT_READ;
        /* __TEXT is r-x on macOS, but the bind pass writes into it for
         * TEXT_ABSOLUTE32 fixups, so protections go on afterwards. */
        if (mprotect(m->base + (s->vmaddr), len, prot))
            MLOG("  [macho] mprotect %.16s failed: %s\n", s->segname, strerror(errno));
    }
    return 0;
}

/* --------------------------------------------------------- rebase / bind */

static int do_rebase(macho *m)
{
    const uint8_t *p, *end;
    int seg = -1, type = 1;
    uint64_t off = 0;
    uint64_t count, skip, i;

    if (!m->dyld || !m->dyld->rebase_size) return 0;
    p = m->slice + m->dyld->rebase_off;
    end = p + m->dyld->rebase_size;

    while (p < end) {
        uint8_t b = *p++;
        uint8_t op = b & REBASE_OPCODE_MASK, imm = b & REBASE_IMMEDIATE_MASK;
        switch (op) {
        case REBASE_OPCODE_DONE: p = end; break;
        case REBASE_OPCODE_SET_TYPE_IMM: type = imm; break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg = imm; off = uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_ULEB: off += uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: off += (uint64_t)imm * 8; break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
            count = (op == REBASE_OPCODE_DO_REBASE_IMM_TIMES) ? imm : uleb(&p, end);
            for (i = 0; i < count; i++) {
                uint64_t *slot = (uint64_t *)seg_addr(m, seg, off);
                if (!slot) return fail("rebase: bad segment %d", seg);
                if (type == 1) *slot += m->slide;
                off += 8;
            }
            break;
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
            uint64_t *slot = (uint64_t *)seg_addr(m, seg, off);
            if (!slot) return fail("rebase: bad segment %d", seg);
            if (type == 1) *slot += m->slide;
            off += 8 + uleb(&p, end);
            break;
        }
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
            count = uleb(&p, end); skip = uleb(&p, end);
            for (i = 0; i < count; i++) {
                uint64_t *slot = (uint64_t *)seg_addr(m, seg, off);
                if (!slot) return fail("rebase: bad segment %d", seg);
                if (type == 1) *slot += m->slide;
                off += 8 + skip;
            }
            break;
        default:
            return fail("rebase: unknown opcode 0x%02x", b);
        }
    }
    return 0;
}

static int run_bind_stream(macho *m, const uint8_t *p, const uint8_t *end,
                           int lazy, const char *what)
{
    const uint8_t *start = p;
    int seg = -1, type = 1, flags = 0;
    int64_t addend = 0;
    uint64_t off = 0, count, skip, i;
    char sym[192] = { 0 };

    while (p < end) {
        uint8_t b = *p++;
        uint8_t op = b & BIND_OPCODE_MASK, imm = b & BIND_IMMEDIATE_MASK;
        switch (op) {
        case BIND_OPCODE_DONE:
            /* In the lazy stream DONE separates entries rather than ending the
             * program, so keep going. */
            if (!lazy) p = end;
            break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: (void)uleb(&p, end); break;
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            flags = imm;
            /* Always walk to the terminator, even once the name stops fitting:
             * stopping early leaves `p` mid-string and every opcode after that
             * is read from the middle of a symbol name. C++ mangled names run
             * well past any buffer worth reserving, so this is not theoretical. */
            { size_t n = 0;
              while (p < end && *p) {
                  if (n + 1 < sizeof sym) sym[n++] = (char)*p;
                  p++;
              }
              sym[n] = 0;
              if (p < end) p++;                 /* the NUL */
            }
            break;
        case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg = imm; off = uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: off += uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: {
            uint64_t *slot = (uint64_t *)seg_addr(m, seg, off);
            void *v;
            if (!slot) return fail("bind: bad segment %d", seg);
            v = resolve(m, sym, (flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0);
            if (type == 1) *slot = (uint64_t)(uintptr_t)v + (uint64_t)addend;
            off += 8;
            if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB) off += uleb(&p, end);
            if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) off += (uint64_t)imm * 8;
            break;
        }
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            count = uleb(&p, end); skip = uleb(&p, end);
            for (i = 0; i < count; i++) {
                uint64_t *slot = (uint64_t *)seg_addr(m, seg, off);
                void *v;
                if (!slot) return fail("bind: bad segment %d", seg);
                v = resolve(m, sym, (flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0);
                if (type == 1) *slot = (uint64_t)(uintptr_t)v + (uint64_t)addend;
                off += 8 + skip;
            }
            break;
        default:
            return fail("%s bind: unknown opcode 0x%02x at +%td (stream is %td bytes)",
                        what, b, p - 1 - start, end - start);
        }
    }
    return 0;
}

static int do_bind(macho *m)
{
    dyld_info_command *d = m->dyld;
    if (!d) return 0;
    if (d->bind_size &&
        run_bind_stream(m, m->slice + d->bind_off,
                        m->slice + d->bind_off + d->bind_size, 0, "regular"))
        return -1;
    if (d->weak_bind_size &&
        run_bind_stream(m, m->slice + d->weak_bind_off,
                        m->slice + d->weak_bind_off + d->weak_bind_size, 0, "weak"))
        return -1;
    /* Lazy binds would normally be resolved on first call through dyld_stub_binder.
     * There is no dyld here, so bind them eagerly -- the plugin cannot tell. */
    if (d->lazy_bind_size &&
        run_bind_stream(m, m->slice + d->lazy_bind_off,
                        m->slice + d->lazy_bind_off + d->lazy_bind_size, 1, "lazy"))
        return -1;
    return 0;
}

/* ------------------------------------------------------ classic binding */

/* Walk every symbol-pointer section and fill each slot from the indirect symbol
 * table: section->reserved1 is that section's first index, so slot i of the
 * section corresponds to indirect[reserved1 + i], which indexes the symbol table.
 * A slot marked LOCAL or ABS is not an import and only needs the slide. */
static int bind_symbol_pointers(macho *m)
{
    const uint32_t *indirect;
    const nlist_64 *syms;
    const char *strs;
    int i;

    if (!m->dysym || !m->symtab || !m->dysym->nindirectsyms) return 0;
    indirect = (const uint32_t *)(m->slice + m->dysym->indirectsymoff);
    syms     = (const nlist_64 *)(m->slice + m->symtab->symoff);
    strs     = (const char *)(m->slice + m->symtab->stroff);

    for (i = 0; i < m->nseg; i++) {
        segment_command_64 *sg = m->seg[i];
        const section_64 *sec = (const section_64 *)((const uint8_t *)sg + sizeof *sg);
        uint32_t k;
        for (k = 0; k < sg->nsects; k++) {
            uint32_t type = sec[k].flags & 0xff;
            uint64_t *slot;
            uint32_t n, j;
            if (type != S_NON_LAZY_SYMBOL_POINTERS && type != S_LAZY_SYMBOL_POINTERS)
                continue;
            slot = (uint64_t *)(m->base + sec[k].addr);
            n = (uint32_t)(sec[k].size / 8);
            for (j = 0; j < n; j++) {
                uint32_t idx = sec[k].reserved1 + j;
                uint32_t sy;
                if (idx >= m->dysym->nindirectsyms) break;
                sy = indirect[idx];
                if (sy & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
                    /* Not an import: the stored value is an address in this
                     * image and only needs the load bias. */
                    if (!(sy & INDIRECT_SYMBOL_ABS)) slot[j] += m->slide;
                    continue;
                }
                if (sy >= m->symtab->nsyms) continue;
                if (syms[sy].n_strx >= m->symtab->strsize) continue;
                slot[j] = (uint64_t)(uintptr_t)resolve(m, strs + syms[sy].n_strx,
                                                       (syms[sy].n_desc & 0x40) != 0);
            }
            MLOG("  [macho] %.16s: %u symbol pointer(s) bound\n", sec[k].sectname, n);
        }
    }
    return 0;
}

/* Relocation entries. External ones name a symbol; local ones only need the
 * slide. Both are 8-byte records whose r_address is an offset from the first
 * segment's vmaddr. */
/* A relocation's r_address is an offset from the first *writable* segment, not
 * from the first segment. Using __TEXT instead puts every entry a whole segment
 * too low -- which corrupts read-only memory and leaves __DATA unrelocated, so
 * the first thing to run out of __mod_init_func jumps to a link-time address. */
static uint64_t reloc_base(macho *m)
{
    int i;
    for (i = 0; i < m->nseg; i++)
        if (m->seg[i]->initprot & 2) return m->seg[i]->vmaddr;
    return m->nseg ? m->seg[0]->vmaddr : 0;
}

static int apply_relocs(macho *m)
{
    const nlist_64 *syms;
    const char *strs;
    uint64_t first = reloc_base(m);
    uint32_t i;

    if (!m->dysym) return 0;
    syms = m->symtab ? (const nlist_64 *)(m->slice + m->symtab->symoff) : NULL;
    strs = m->symtab ? (const char *)(m->slice + m->symtab->stroff) : NULL;

    if (m->dysym->nextrel && syms && strs) {
        const reloc_info *r = (const reloc_info *)(m->slice + m->dysym->extreloff);
        for (i = 0; i < m->dysym->nextrel; i++) {
            uint32_t sy = RELOC_SYMBOLNUM(&r[i]);
            uint64_t *at;
            if (!RELOC_EXTERN(&r[i]) || sy >= m->symtab->nsyms) continue;
            if (syms[sy].n_strx >= m->symtab->strsize) continue;
            at = (uint64_t *)(m->base + first + (uint64_t)(uint32_t)r[i].r_address);
            *at = (uint64_t)(uintptr_t)resolve(m, strs + syms[sy].n_strx,
                                              (syms[sy].n_desc & 0x40) != 0);
        }
        MLOG("  [macho] %u external relocation(s)\n", m->dysym->nextrel);
    }
    if (m->dysym->nlocrel) {
        const reloc_info *r = (const reloc_info *)(m->slice + m->dysym->locreloff);
        for (i = 0; i < m->dysym->nlocrel; i++) {
            uint64_t *at;
            if (RELOC_EXTERN(&r[i]) || RELOC_LENGTH(&r[i]) != 3) continue;  /* 8-byte only */
            at = (uint64_t *)(m->base + first + (uint64_t)(uint32_t)r[i].r_address);
            *at += m->slide;
        }
        MLOG("  [macho] %u local relocation(s), base 0x%llx\n",
             m->dysym->nlocrel, (unsigned long long)first);
    }
    return 0;
}

/* ---------------------------------------------------------------- exports */

/* The export table is a trie: each node carries an optional terminal payload
 * and a list of labelled edges, and a symbol name is the concatenation of edge
 * labels along the path. Walk it depth-first, accumulating the prefix. */
static void walk_exports(macho *m, const uint8_t *start, const uint8_t *end,
                         const uint8_t *node, char *prefix, size_t plen)
{
    uint64_t term, flags, addr;
    const uint8_t *p = node;
    uint8_t nchild;
    int i;

    if (node < start || node >= end) return;
    term = uleb(&p, end);
    if (term) {
        const uint8_t *q = p;
        flags = uleb(&q, end);
        if (flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
            (void)uleb(&q, end);                  /* ordinal; re-exports unsupported */
        } else {
            addr = uleb(&q, end);
            if (flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) (void)uleb(&q, end);
            if (m->nexp < MAX_EXP && plen) {
                snprintf(m->exp[m->nexp].name, sizeof m->exp[m->nexp].name, "%s", prefix);
                m->exp[m->nexp].addr = m->base + addr;
                m->nexp++;
            }
        }
        p += term;
    }
    if (p >= end) return;
    nchild = *p++;
    for (i = 0; i < nchild && p < end; i++) {
        size_t n = plen;
        while (p < end && *p) { if (n + 1 < 192) prefix[n++] = (char)*p; p++; }
        if (p < end) p++;                         /* the NUL */
        prefix[n] = 0;
        { uint64_t child = uleb(&p, end);
          walk_exports(m, start, end, start + child, prefix, n); }
    }
}

static void collect_exports(macho *m)
{
    char prefix[192] = { 0 };
    if (m->dyld && m->dyld->export_size) {
        const uint8_t *s = m->slice + m->dyld->export_off;
        walk_exports(m, s, s + m->dyld->export_size, s, prefix, 0);
    }
    /* A bundle with no export trie still has a symbol table. */
    if (!m->nexp && m->symtab) {
        const nlist_64 *sy = (const nlist_64 *)(m->slice + m->symtab->symoff);
        const char *str = (const char *)(m->slice + m->symtab->stroff);
        uint32_t i;
        for (i = 0; i < m->symtab->nsyms && m->nexp < MAX_EXP; i++) {
            /* N_EXT and defined in a section */
            if (!(sy[i].n_type & 0x01) || (sy[i].n_type & 0x0e) != 0x0e) continue;
            if (sy[i].n_strx >= m->symtab->strsize) continue;
            snprintf(m->exp[m->nexp].name, sizeof m->exp[m->nexp].name,
                     "%s", str + sy[i].n_strx);
            m->exp[m->nexp].addr = m->base + sy[i].n_value;
            m->nexp++;
        }
    }
}

/* ---------------------------------------------------------------- Info.plist */

/* Enough of a plist reader to pull two string values. Not a general parser:
 * <key>X</key><string>Y</string> is the only shape needed here. */
static void plist_string(const char *xml, const char *key, char *out, size_t n)
{
    const char *k, *s, *e;
    char pat[64];
    out[0] = 0;
    snprintf(pat, sizeof pat, "<key>%s</key>", key);
    if (!(k = strstr(xml, pat))) return;
    if (!(s = strstr(k, "<string>"))) return;
    s += 8;
    if (!(e = strstr(s, "</string>"))) return;
    if ((size_t)(e - s) >= n) e = s + n - 1;
    memcpy(out, s, (size_t)(e - s));
    out[e - s] = 0;
}

/* -------------------------------------------------------------------- open */

/* A plugin is a directory: Foo.vst/Contents/MacOS/Foo. Resolve that to the
 * executable, and read Info.plist while we are there. */
static int resolve_bundle(macho *m, const char *path, char *out, size_t n)
{
    struct stat st;
    char p[4096];
    DIR *d;

    if (stat(path, &st)) return fail("%s: %s", path, strerror(errno));
    if (!S_ISDIR(st.st_mode)) { snprintf(out, n, "%s", path); return 0; }
    snprintf(m->bundle_path, sizeof m->bundle_path, "%s", path);

    snprintf(p, sizeof p, "%s/Contents/Info.plist", path);
    { FILE *f = fopen(p, "rb");
      if (f) {
          char *buf = malloc(65536);
          size_t got = buf ? fread(buf, 1, 65535, f) : 0;
          if (buf) {
              buf[got] = 0;
              plist_string(buf, "CFBundleIdentifier", m->bundle_id, sizeof m->bundle_id);
              plist_string(buf, "CFBundleName", m->bundle_name, sizeof m->bundle_name);
              if (!m->bundle_name[0])
                  plist_string(buf, "CFBundleExecutable", m->bundle_name, sizeof m->bundle_name);
              free(buf);
          }
          fclose(f);
      } }

    /* CFBundleExecutable is authoritative, but the directory listing is a fine
     * fallback -- there is only ever one file in Contents/MacOS. */
    snprintf(p, sizeof p, "%s/Contents/MacOS", path);
    if (m->bundle_name[0]) {
        snprintf(out, n, "%s/%s", p, m->bundle_name);
        if (!stat(out, &st) && S_ISREG(st.st_mode)) return 0;
    }
    if ((d = opendir(p))) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(out, n, "%s/%s", p, e->d_name);
            if (!stat(out, &st) && S_ISREG(st.st_mode)) { closedir(d); return 0; }
        }
        closedir(d);
    }
    return fail("%s has no Contents/MacOS executable", path);
}

/* Universal binaries hold several images; pick the x86_64 one. An arm64-only
 * plugin is simply not runnable here and says so rather than misloading. */
static const mach_header_64 *pick_slice(macho *m)
{
    const fat_header *fh = (const fat_header *)m->file;
    uint32_t magic = *(const uint32_t *)m->file;

    if (magic == MH_MAGIC_64) {
        m->slice = m->file;
        m->slicelen = m->filelen;
        return (const mach_header_64 *)m->file;
    }

    if (magic == FAT_MAGIC_BE || magic == FAT_MAGIC_BE64) {
        /* fat headers are big-endian regardless of the slices inside */
        uint32_t n = __builtin_bswap32(fh->nfat_arch), i;
        const fat_arch *a = (const fat_arch *)(m->file + sizeof *fh);
        int saw_arm = 0, saw_ppc = 0, saw_i386 = 0;
        for (i = 0; i < n && (const uint8_t *)(a + i + 1) <= m->file + m->filelen; i++) {
            uint32_t cpu = __builtin_bswap32(a[i].cputype);
            uint32_t off = __builtin_bswap32(a[i].offset);
            if (cpu == CPU_TYPE_X86_64) {
                uint32_t sz = __builtin_bswap32(a[i].size);
                if ((size_t)off + sizeof(mach_header_64) > m->filelen) break;
                if ((size_t)off + sz > m->filelen) sz = (uint32_t)(m->filelen - off);
                m->slice = m->file + off;
                m->slicelen = sz;
                MLOG("  [macho] universal: x86_64 slice at %u, %u bytes\n", off, sz);
                return (const mach_header_64 *)m->slice;
            }
            if (cpu == 0x0100000C) saw_arm = 1;    /* CPU_TYPE_ARM64  */
            if (cpu == 18 || cpu == 0x01000012) saw_ppc = 1;  /* PPC, PPC64 */
            if (cpu == 7) saw_i386 = 1;                       /* CPU_TYPE_X86 */
        }
        /* Name the architectures that *are* there. "No x86_64 slice" leaves the
         * reader guessing whether the file is broken or simply for another
         * machine, and those call for very different responses. */
        if (saw_ppc)
            fail(saw_i386 ? "a PowerPC/i386 binary: Mac OS X of the PowerPC era, "
                            "with no x86-64 slice"
                          : "a PowerPC binary: Mac OS X of the PowerPC era, and "
                            "not this instruction set");
        else if (saw_arm)  fail("arm64-only binary: no x86_64 slice to run");
        else if (saw_i386) fail("an i386-only Mach-O: 32-bit Intel macOS, which "
                                "this 64-bit loader cannot map");
        else               fail("universal binary with no x86_64 slice");
        return NULL;
    }
    /* Not Mach-O at all. The Classic-era formats are worth naming: a PEF is the
     * Code Fragment Manager's container from Mac OS 8/9, a different executable
     * format for a different processor -- not something this loader could ever
     * grow into running. */
    if (m->filelen >= 8 && !memcmp(m->file, "Joy!peff", 8))
        fail("a Classic Mac OS / Carbon plug-in (CFM/PEF, PowerPC), not Mach-O");
    else if (m->filelen >= 7 && !memcmp(m->file, "StuffIt", 7))
        fail("a StuffIt archive, not a plug-in -- unpack it first");
    else if (magic == 0xFEEDFACE || magic == 0xFEEDFACF)
        fail("a big-endian Mach-O, so PowerPC: Mac OS X, but not this "
             "instruction set");
    else
        fail("not a Mach-O image (magic 0x%08x)", magic);
    return NULL;
}

macho *macho_open(const char *path)
{
    macho *m = calloc(1, sizeof *m);
    char bin[4096];
    int fd;
    struct stat st;
    const load_command *lc;
    uint32_t i;

    if (!m) return NULL;
    g_err[0] = 0;
    if (resolve_bundle(m, path, bin, sizeof bin)) { free(m); return NULL; }

    if ((fd = open(bin, O_RDONLY)) < 0) { fail("%s: %s", bin, strerror(errno)); free(m); return NULL; }
    if (fstat(fd, &st)) { close(fd); fail("fstat: %s", strerror(errno)); free(m); return NULL; }
    m->filelen = (size_t)st.st_size;
    m->file = mmap(NULL, m->filelen, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m->file == MAP_FAILED) { m->file = NULL; fail("mmap file: %s", strerror(errno)); free(m); return NULL; }

    if (!(m->mh = pick_slice(m))) { macho_close(m); return NULL; }
    if (m->mh->filetype != MH_BUNDLE && m->mh->filetype != MH_DYLIB) {
        fail("filetype %u is not a bundle or dylib", m->mh->filetype);
        macho_close(m); return NULL;
    }

    lc = (const load_command *)((const uint8_t *)m->mh + sizeof *m->mh);
    for (i = 0; i < m->mh->ncmds; i++) {
        if ((const uint8_t *)lc + sizeof *lc > m->slice + m->slicelen) break;
        switch (lc->cmd) {
        case LC_SEGMENT_64:
            if (m->nseg < MAX_SEG) m->seg[m->nseg++] = (segment_command_64 *)lc;
            break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY:
            m->dyld = (dyld_info_command *)lc; break;
        case LC_SYMTAB:
            m->symtab = (symtab_command *)lc; break;
        case LC_DYSYMTAB:
            m->dysym = (dysymtab_command *)lc; break;
        case LC_DYLD_CHAINED_FIXUPS:
            fail("this image uses chained fixups, which are not implemented yet");
            macho_close(m); return NULL;
        default: break;
        }
        lc = (const load_command *)((const uint8_t *)lc + lc->cmdsize);
        if (!((const load_command *)lc)->cmdsize) break;
    }
    if (!m->nseg) { fail("no LC_SEGMENT_64"); macho_close(m); return NULL; }

    if (map_image(m)) { macho_close(m); return NULL; }

    g_stub_owner = m;
    g_last_image = m;
    macshim_set_bundle(m->bundle_path);
    macshim_set_image(m);
    /* Before binding, not after: an image's own weak definitions are what some
     * of its imports refer to, and resolve() cannot see them otherwise. */
    collect_exports(m);
    if (m->dyld) {
        if (do_rebase(m)) { macho_close(m); return NULL; }
        if (do_bind(m))   { macho_close(m); return NULL; }
    } else {
        /* No dyld info: this is a pre-opcode image, so bind the classic way. */
        MLOG("  [macho] no LC_DYLD_INFO; using the indirect symbol table\n");
        if (apply_relocs(m))          { macho_close(m); return NULL; }
        if (bind_symbol_pointers(m))  { macho_close(m); return NULL; }
    }

    /* Find __mod_init_func before protections go on. */
    for (i = 0; i < (uint32_t)m->nseg; i++) {
        segment_command_64 *s = m->seg[i];
        const section_64 *sec = (const section_64 *)((const uint8_t *)s + sizeof *s);
        uint32_t k;
        for (k = 0; k < s->nsects; k++) {
            if ((sec[k].flags & 0xff) == 0x09) {      /* S_MOD_INIT_FUNC_POINTERS */
                m->init_funcs = m->base + sec[k].addr;
                m->init_count = (size_t)(sec[k].size / 8);
            }
        }
    }
    protect_segments(m);

    MLOG("  [macho] %d segment(s), %d export(s), %d import(s) resolved, %d stubbed\n",
         m->nseg, m->nexp, m->nresolved, m->nimp);
    return m;
}

int macho_run_init(macho *m)
{
    size_t i;
    if (!m || !m->init_funcs) return 0;
    for (i = 0; i < m->init_count; i++) {
        void (*fn)(int, char **, char **, char **) =
            ((void (**)(int, char **, char **, char **))m->init_funcs)[i];
        char *argv[] = { (char *)"plugin", NULL };
        char *envp[] = { NULL };
        if (!fn) continue;
        MLOG("  [macho] init %zu/%zu -> %p\n", i + 1, m->init_count, (void *)fn);
        fn(1, argv, envp, envp);
    }
    return 0;
}

void macho_close(macho *m)
{
    if (!m) return;
    /* Destructors first, while the code they live in is still mapped. */
    macshim_run_atexit(m);
    if (g_stub_owner == m) g_stub_owner = NULL;
    if (g_last_image == m) g_last_image = NULL;
    if (m->base) munmap(m->base, m->span);
    if (m->file) munmap(m->file, m->filelen);
    free(m);
}

void *macho_symbol(macho *m, const char *name)
{
    char want[192];
    int i;
    if (!m || !name) return NULL;
    snprintf(want, sizeof want, "_%s", name);
    for (i = 0; i < m->nexp; i++)
        if (!strcmp(m->exp[i].name, want) || !strcmp(m->exp[i].name, name))
            return m->exp[i].addr;
    return NULL;
}

void macho_describe(const macho *m, const void *addr, char *out, size_t n)
{
    const uint8_t *p = addr;
    int i;
    if (!out || n == 0) return;
    if (!m) m = g_last_image;
    if (!m || !m->base || p < m->base || p >= m->base + m->span) {
        /* Outside the image: name the host library and symbol, because "(host)"
         * alone does not say whether we are stuck in libc, libstdc++ or a shim. */
        Dl_info di;
        if (dladdr(addr, &di) && di.dli_fname) {
            const char *base = strrchr(di.dli_fname, '/');
            snprintf(out, n, "%s`%s+0x%lx", base ? base + 1 : di.dli_fname,
                     di.dli_sname ? di.dli_sname : "?",
                     di.dli_saddr ? (unsigned long)((const uint8_t *)addr -
                                                    (const uint8_t *)di.dli_saddr) : 0ul);
        } else {
            snprintf(out, n, "%p (unknown)", addr);
        }
        return;
    }
    for (i = 0; i < m->nseg; i++) {
        segment_command_64 *s = m->seg[i];
        const uint8_t *lo = m->base + s->vmaddr;
        if (p >= lo && p < lo + s->vmsize) {
            snprintf(out, n, "image+0x%-6lx (%.16s)",
                     (unsigned long)(p - m->base), s->segname);
            return;
        }
    }
    snprintf(out, n, "image+0x%lx", (unsigned long)(p - m->base));
}

const char *macho_bundle_path(const macho *m) { return m ? m->bundle_path : ""; }
const char *macho_bundle_id(const macho *m)   { return m ? m->bundle_id : ""; }
const char *macho_bundle_name(const macho *m) { return m ? m->bundle_name : ""; }

void macho_import_stats(const macho *m, int *resolved, int *stubbed, int *reached)
{
    int i, hit = 0;
    if (resolved) *resolved = m ? m->nresolved : 0;
    if (stubbed)  *stubbed  = m ? m->nimp : 0;
    if (m) for (i = 0; i < m->nimp; i++) if (m->imp[i].calls) hit++;
    if (reached) *reached = hit;
}

void macho_each_stub(const macho *m,
                     void (*cb)(const char *, unsigned long, void *), void *ud)
{
    int i;
    if (!m || !cb) return;
    for (i = 0; i < m->nimp; i++) cb(m->imp[i].name, m->imp[i].calls, ud);
}

void macho_each_export(const macho *m,
                       void (*cb)(const char *, void *, void *), void *ud)
{
    int i;
    if (!m || !cb) return;
    for (i = 0; i < m->nexp; i++) cb(m->exp[i].name, m->exp[i].addr, ud);
}
