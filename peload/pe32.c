/* peload32 -- load a Windows *32-bit* VST2 DLL natively on Linux, no Wine.
 *
 * The 64-bit loader's four gaps all have i386 counterparts, and each inverts in
 * a pleasing way:
 *
 *   container   PE32, not PE32+   -> no BaseOfData omission, 4-byte ImageBase,
 *                                    data directories at +96, HIGHLOW relocs
 *   ABI         stdcall/cdecl     -> __attribute__((stdcall)) for Win32,
 *                                    cdecl for the VST2 entry points
 *   TEB         fs:[0x18]         -> on x86-64 glibc uses %fs and Windows wants
 *                                    %gs; on i386 it is exactly the other way
 *                                    round, so %fs is the free one here
 *   imports     kernel32/...      -> the same stub table, stdcall-flavoured
 *
 * Built as a separate i386 binary because a process cannot execute both widths;
 * the 64-bit host drives it out of process.
 *
 * Audio only for now: no window layer, so no editors. */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64   /* i386: the file stubs do 64-bit offset math */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <signal.h>
#include <ucontext.h>
#include <strings.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <asm/ldt.h>

#if !defined(__i386__)
#error "pe32.c is the i386 loader; build it with -m32"
#endif

/* Win32 uses stdcall; the VST2 function pointers are cdecl. On x86-64 both
 * collapse into one convention, which is why the 64-bit loader needs only one
 * macro. */
#define WINAPI_  __attribute__((stdcall))
#define VSTCALL_ __attribute__((cdecl))

/* ------------------------------------------------------------ PE structures */

#pragma pack(push, 1)
typedef struct { uint16_t e_magic; uint8_t pad[58]; uint32_t e_lfanew; } DOS_HDR;
typedef struct {
    uint32_t Signature; uint16_t Machine, NumberOfSections;
    uint32_t TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    uint16_t SizeOfOptionalHeader, Characteristics;
} FILE_HDR;
typedef struct { uint32_t VirtualAddress, Size; } DATA_DIR;
typedef struct {
    uint16_t Magic; uint8_t MajorLinkerVersion, MinorLinkerVersion;
    uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint, BaseOfCode;
    /* PE32 keeps BaseOfData, which PE32+ drops, and ImageBase is 4 bytes. */
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment, FileAlignment;
    uint16_t MajorOSVersion, MinorOSVersion, MajorImageVersion, MinorImageVersion;
    uint16_t MajorSubsystemVersion, MinorSubsystemVersion;
    uint32_t Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t Subsystem, DllCharacteristics;
    uint32_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    uint32_t LoaderFlags, NumberOfRvaAndSizes;
    DATA_DIR DataDirectory[16];
} OPT_HDR32;
typedef struct {
    char Name[8];
    uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData;
    uint32_t PointerToRelocations, PointerToLinenumbers;
    uint16_t NumberOfRelocations, NumberOfLinenumbers;
    uint32_t Characteristics;
} SEC_HDR;
typedef struct {
    uint32_t OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk;
} IMP_DESC;
typedef struct {
    uint32_t Characteristics, TimeDateStamp;
    uint16_t MajorVersion, MinorVersion;
    uint32_t Name, Base, NumberOfFunctions, NumberOfNames;
    uint32_t AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
} EXP_DIR;
typedef struct { uint32_t VirtualAddress, SizeOfBlock; } RELOC_BLK;
typedef struct {
    uint32_t StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks;
    uint32_t SizeOfZeroFill, Characteristics;
} TLS_DIR32;
#pragma pack(pop)

#define DIR_EXPORT   0
#define DIR_IMPORT   1
#define DIR_RESOURCE 2
#define DIR_RELOC    5
#define DIR_TLS      9

static int pe_verbose(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("PELOAD_VERBOSE"); v = e && *e != '0'; } return v; }
#define PLOG(...) do { if (pe_verbose()) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- fake TEB */

/* i386 Windows reads the TEB through %fs; i386 Linux glibc uses %gs for its own
 * TLS, so %fs is ours. It needs a real LDT/GDT entry rather than a base MSR,
 * which is what set_thread_area gives us. */
#define TEB_SIZE 0x1000
#define TEB_SEH_LIST_32    0x00 /* NtTib.ExceptionList      */
#define TEB_STACK_BASE_32  0x04 /* NtTib.StackBase          */
#define TEB_STACK_LIMIT_32 0x08 /* NtTib.StackLimit         */
#define TEB_SELF_32     0x18    /* NtTib.Self               */
#define TEB_TLS_PTR_32  0x2C    /* ThreadLocalStoragePointer */
#define TLS_SLOTS 128

typedef struct {
    uint8_t *raw;
    void    *slots[TLS_SLOTS];
} teb32;

static __thread teb32 *g_teb;

static int teb_install(void)
{
    struct user_desc d;
    teb32 *t = calloc(1, sizeof *t);

    if (!t) return -1;
    if (posix_memalign((void **)&t->raw, 4096, TEB_SIZE)) { free(t); return -1; }
    memset(t->raw, 0, TEB_SIZE);
    *(uint32_t *)(t->raw + TEB_SELF_32)    = (uint32_t)(uintptr_t)t->raw;
    *(uint32_t *)(t->raw + TEB_TLS_PTR_32) = (uint32_t)(uintptr_t)t->slots;
    /* An empty SEH chain is terminated by -1, not 0: an MSVC __try prologue
     * pushes the old fs:[0] and a 0 there reads as a valid record at address 0.
     * StackBase/StackLimit matter too -- __chkstk walks toward the limit when a
     * function has large locals, and a zero limit means it walks to address 0. */
    *(uint32_t *)(t->raw + TEB_SEH_LIST_32)   = 0xFFFFFFFFu;
    {
        pthread_attr_t a;
        void *sp = NULL; size_t ssz = 0;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            pthread_attr_getstack(&a, &sp, &ssz);
            pthread_attr_destroy(&a);
        }
        if (sp) {
            *(uint32_t *)(t->raw + TEB_STACK_BASE_32)  = (uint32_t)(uintptr_t)sp + ssz;
            *(uint32_t *)(t->raw + TEB_STACK_LIMIT_32) = (uint32_t)(uintptr_t)sp;
        }
    }

    memset(&d, 0, sizeof d);
    d.entry_number = -1;                 /* let the kernel choose */
    d.base_addr    = (unsigned long)(uintptr_t)t->raw;
    d.limit        = 0xfffff;
    d.seg_32bit    = 1;
    d.limit_in_pages = 1;
    d.contents     = 0;                  /* data */
    d.read_exec_only = 0;
    d.seg_not_present = 0;
    d.useable      = 1;
    if (syscall(SYS_set_thread_area, &d) != 0) {
        perror("set_thread_area");
        free(t->raw); free(t);
        return -1;
    }
    /* Load the returned selector into %fs. */
    {
        unsigned short sel = (unsigned short)((d.entry_number << 3) | 3);
        __asm__ volatile ("movw %0, %%fs" :: "r"(sel));
    }
    g_teb = t;
    PLOG("teb: fs base %p via set_thread_area entry %d\n",
         (void *)t->raw, d.entry_number);
    return 0;
}

/* ------------------------------------------------------------ loaded image */

typedef struct {
    uint8_t   *base;
    uint32_t   size;
    OPT_HDR32 *opt;
    FILE_HDR  *fh;
    SEC_HDR   *sec;
    int        nsec;
} image32;

/* ------------------------------------------------------------ crash reports */

/* Guest faults kill the process, and a bare "SIGSEGV at 0x0" says nothing about
 * which plugin code got there. i386 MSVC builds keep frame pointers often
 * enough that walking %ebp recovers the call chain even when %eip is garbage --
 * a `ret` to 0 leaves the frames below it intact. Addresses are reported as
 * image+offset so they can be fed straight to objdump. */
static uint8_t *g_img_lo, *g_img_hi;

static const char *sec_of(uint8_t *p, image32 *im)
{
    int i;
    if (!im) return NULL;
    for (i = 0; i < im->nsec; i++) {
        uint8_t *a = im->base + im->sec[i].VirtualAddress;
        if (p >= a && p < a + im->sec[i].VirtualSize) return (const char *)im->sec[i].Name;
    }
    return NULL;
}
static image32 *g_im;

static void loc(char *out, size_t n, uint32_t a)
{
    uint8_t *p = (uint8_t *)(uintptr_t)a;
    const char *sn;
    if (p >= g_img_lo && p < g_img_hi && (sn = sec_of(p, g_im)))
        snprintf(out, n, "image+0x%-8x (%.8s)", (unsigned)(p - g_img_lo), sn);
    else if (p >= g_img_lo && p < g_img_hi)
        snprintf(out, n, "image+0x%-8x", (unsigned)(p - g_img_lo));
    else
        snprintf(out, n, "0x%08x %s", a, a < 0x10000 ? "<not mapped>" : "(host)");
}

static void fault_handler(int sig, siginfo_t *si, void *uc)
{
    static volatile int nested;
    mcontext_t *m = &((ucontext_t *)uc)->uc_mcontext;
    uint32_t eip = (uint32_t)m->gregs[REG_EIP], ebp = (uint32_t)m->gregs[REG_EBP];
    uint32_t esp = (uint32_t)m->gregs[REG_ESP];
    char b[96];
    int depth;

    /* The frame walk and stack sweep below read memory we only believe is
     * valid. If one of them faults, say so once and stop -- a handler that
     * re-enters itself buries the first report under its own. */
    if (nested++) {
        fprintf(stderr, "*** fault while reporting a fault; stopping\n");
        fflush(stderr);
        _exit(139);
    }

    fprintf(stderr, "\n*** %s in guest code\n",
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGILL ? "SIGILL" : "SIGFPE");
    loc(b, sizeof b, (uint32_t)(uintptr_t)si->si_addr);
    fprintf(stderr, "    addr %s\n", b);
    loc(b, sizeof b, eip); fprintf(stderr, "    eip  %s\n", b);
    fprintf(stderr, "    esp  0x%08x   ebp 0x%08x\n", esp, ebp);
    if (!eip)
        fprintf(stderr, "    (eip 0 means a call or ret through a null/clobbered"
                        " address -- suspect a stub arity or a missing import)\n");

    fprintf(stderr, "    eax 0x%08x  ebx 0x%08x  ecx 0x%08x  edx 0x%08x\n"
                    "    esi 0x%08x  edi 0x%08x  fs 0x%04x  tid %ld\n",
            (uint32_t)m->gregs[REG_EAX], (uint32_t)m->gregs[REG_EBX],
            (uint32_t)m->gregs[REG_ECX], (uint32_t)m->gregs[REG_EDX],
            (uint32_t)m->gregs[REG_ESI], (uint32_t)m->gregs[REG_EDI],
            (unsigned)(m->gregs[REG_FS] & 0xffff), syscall(SYS_gettid));

    for (depth = 0; depth < 24 && ebp >= esp - 0x1000 && ebp < esp + 0x100000; depth++) {
        uint32_t ret = *(uint32_t *)(uintptr_t)(ebp + 4);
        uint32_t nxt = *(uint32_t *)(uintptr_t)ebp;
        if (!ret) break;
        loc(b, sizeof b, ret);
        fprintf(stderr, "    #%-2d  %s\n", depth, b);
        if (nxt <= ebp) break;
        ebp = nxt;
    }
    /* the frame chain is often the first casualty; sweep the stack for words
     * that land in the image's code, newest first, and print the callers */
    if (depth == 0) {
        uint32_t a, hits = 0;
        fprintf(stderr, "    frame chain broken; stack words pointing into the image:\n");
        uint32_t top = g_teb ? *(uint32_t *)(g_teb->raw + TEB_STACK_BASE_32) : 0;
        uint32_t end = esp + 0x4000;
        if (top && top < end) end = top;             /* do not read past the stack */
        for (a = esp; a < end && hits < 20; a += 4) {
            uint8_t *v = (uint8_t *)(uintptr_t) * (uint32_t *)(uintptr_t)a;
            if (v > g_img_lo && v < g_img_hi && sec_of(v, g_im)) {
                loc(b, sizeof b, (uint32_t)(uintptr_t)v);
                fprintf(stderr, "      [esp+0x%-5x] %s\n", a - esp, b);
                hits++;
            }
        }
    }
    fflush(stderr);
    _exit(139);
}

static void faults_report(image32 *im)
{
    struct sigaction sa;
    g_im = im; g_img_lo = im->base; g_img_hi = im->base + im->size;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}

/* --------------------------------------------------- missing-import tracker */

#include "win32_arity.h"

#define MAX_IMPORTS 4096
typedef struct { char dll[32], sym[96]; unsigned long calls; } imprec;
static imprec   g_imp[MAX_IMPORTS];
static int      g_nimp, g_nresolved;
static uint8_t *g_tramp;
static size_t   g_tramp_used;

static void WINAPI_ missing_import_report(uint32_t idx)
{
    if (idx < (uint32_t)g_nimp && g_imp[idx].calls++ == 0 && pe_verbose())
        fprintf(stderr, "  [stub] %s!%s\n", g_imp[idx].dll, g_imp[idx].sym);
}

/* An i386 stdcall callee pops its own arguments, so a stub cannot end in a bare
 * `ret`: the caller would carry on with the arguments still on the stack, and
 * 4 bytes of drift per argument eventually sends some later `ret` into nothing.
 * (This hazard does not exist on x86-64 -- the caller always cleans up -- which
 * is why the 64-bit loader gets away with a plain return.)
 *
 * win32_arity.h, generated from the mingw-w64 import libraries, supplies the
 * real byte count per export, so each stub can `ret N` exactly as the function
 * it stands in for would. An export we have no count for is reported loudly
 * rather than silently corrupting the stack. */
static int g_unknown_arity;

static void *make_stub(uint32_t idx)
{
    uint8_t *p;
    int argb = win32_arity_of(g_imp[idx].sym);

    if (argb < 0) {
        argb = 0;
        if (g_imp[idx].sym[0] != '#') {          /* ordinal imports have no name */
            fprintf(stderr, "  [stub] %s!%s: unknown stdcall arity, assuming 0 --"
                            " a call to it will unbalance the stack\n",
                    g_imp[idx].dll, g_imp[idx].sym);
            g_unknown_arity++;
        }
    }

    if (!g_tramp) {
        g_tramp = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (g_tramp == MAP_FAILED) { perror("mmap tramp"); exit(1); }
    }
    p = g_tramp + g_tramp_used;
    g_tramp_used += 24;

    *p++ = 0x68; memcpy(p, &idx, 4); p += 4;           /* push imm32 (index)   */
    *p++ = 0xB8;                                        /* mov eax, report      */
    { void *fn = (void *)missing_import_report; memcpy(p, &fn, 4); p += 4; }
    *p++ = 0xFF; *p++ = 0xD0;                           /* call eax (stdcall,   */
                                                        /*   pops its own arg)  */
    *p++ = 0x31; *p++ = 0xC0;                           /* xor eax, eax         */
    if (argb) {
        *p++ = 0xC2;                                    /* ret imm16            */
        *p++ = (uint8_t)(argb & 0xff); *p++ = (uint8_t)(argb >> 8);
    } else {
        *p++ = 0xC3;                                    /* ret                  */
    }
    return g_tramp + g_tramp_used - 24;
}

/* ------------------------------------------------------------- win32 stubs */

#include "winstubs32.h"

/* ------------------------------------------------------------- VST2 on i386 */

#include "vst2_32.h"

#include "editor32.h"       /* needs AEffect32, so it follows vst2_32.h */

#ifdef PELOAD32_X11
#include "x11win32.h"
#endif

#include "serve32.h"        /* --serve: host a plugin for the 64-bit pehost */

#ifdef PELOAD32_AUDIO
#include "play32.h"
#endif

/* ----------------------------------------------------------------- loading */

static void *rva(image32 *im, uint32_t r) { return im->base + r; }

static int map_image(image32 *im, const char *path)
{
    int fd, i;
    struct stat st;
    DOS_HDR *dos;
    uint8_t *f;

    if ((fd = open(path, O_RDONLY)) < 0) { perror(path); return -1; }
    if (fstat(fd, &st)) { close(fd); return -1; }
    f = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) { perror("mmap file"); return -1; }

    dos = (DOS_HDR *)f;
    if (dos->e_magic != 0x5A4D) { fprintf(stderr, "not MZ\n"); return -1; }
    im->fh = (FILE_HDR *)(f + dos->e_lfanew);
    if (im->fh->Signature != 0x4550) { fprintf(stderr, "not PE\n"); return -1; }
    if (im->fh->Machine != 0x14c) {
        fprintf(stderr, "machine 0x%x -- this loader is i386 only\n", im->fh->Machine);
        return -1;
    }
    im->opt = (OPT_HDR32 *)((uint8_t *)im->fh + sizeof *im->fh);
    if (im->opt->Magic != 0x10B) { fprintf(stderr, "not PE32\n"); return -1; }
    im->sec = (SEC_HDR *)((uint8_t *)im->opt + im->fh->SizeOfOptionalHeader);
    im->nsec = im->fh->NumberOfSections;
    im->size = im->opt->SizeOfImage;

    im->base = mmap(NULL, im->size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (im->base == MAP_FAILED) { perror("mmap image"); return -1; }

    memcpy(im->base, f, im->opt->SizeOfHeaders);
    for (i = 0; i < im->nsec; i++) {
        SEC_HDR *s = &im->sec[i];
        if (s->SizeOfRawData)
            memcpy(im->base + s->VirtualAddress, f + s->PointerToRawData, s->SizeOfRawData);
    }
    {
        uint32_t lfanew = dos->e_lfanew;
        im->fh  = (FILE_HDR *)(im->base + lfanew);
        im->opt = (OPT_HDR32 *)((uint8_t *)im->fh + sizeof *im->fh);
        im->sec = (SEC_HDR *)((uint8_t *)im->opt + im->fh->SizeOfOptionalHeader);
    }
    munmap(f, (size_t)st.st_size);
    PLOG("image: %u KiB at %p (link base 0x%x)\n",
         im->size / 1024, (void *)im->base, im->opt->ImageBase);
    return 0;
}

static int apply_relocs(image32 *im)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_RELOC];
    int32_t delta = (int32_t)((uint32_t)(uintptr_t)im->base - im->opt->ImageBase);
    uint8_t *p, *end;
    int n = 0;

    if (!delta) return 0;
    if (!d.VirtualAddress) { fprintf(stderr, "no relocs and base busy\n"); return -1; }
    p = rva(im, d.VirtualAddress); end = p + d.Size;
    while (p < end) {
        RELOC_BLK *b = (RELOC_BLK *)p;
        uint16_t *e = (uint16_t *)(p + sizeof *b);
        int cnt, i;
        if (!b->SizeOfBlock) break;
        cnt = (int)((b->SizeOfBlock - sizeof *b) / 2);
        for (i = 0; i < cnt; i++) {
            int type = e[i] >> 12, off = e[i] & 0xFFF;
            if (type == 3) {                       /* HIGHLOW: the i386 form */
                uint32_t *t = (uint32_t *)rva(im, b->VirtualAddress + off);
                *t += (uint32_t)delta;
                n++;
            } else if (type != 0) {
                fprintf(stderr, "unhandled reloc type %d\n", type);
            }
        }
        p += b->SizeOfBlock;
    }
    return n;
}

static int protect_sections(image32 *im)
{
    int i;
    for (i = 0; i < im->nsec; i++) {
        SEC_HDR *s = &im->sec[i];
        int prot = PROT_READ;
        if (s->Characteristics & 0x80000000u) prot |= PROT_WRITE;
        if (s->Characteristics & 0x20000000u) prot |= PROT_EXEC;
        if (mprotect(im->base + s->VirtualAddress,
                     (s->VirtualSize + 0xFFF) & ~0xFFFu, prot))
            perror("mprotect");
    }
    return 0;
}

static int resolve_imports(image32 *im)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_IMPORT];
    IMP_DESC *desc;
    int resolved = 0, stubbed = 0;

    if (!d.VirtualAddress) return 0;
    for (desc = rva(im, d.VirtualAddress); desc->Name; desc++) {
        const char *dll = rva(im, desc->Name);
        uint32_t *lookup = rva(im, desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                            : desc->FirstThunk);
        uint32_t *iat = rva(im, desc->FirstThunk);
        for (; *lookup; lookup++, iat++) {
            const char *sym;
            void *fn;
            char ordbuf[32];

            if (*lookup & 0x80000000u) {           /* by ordinal: 32-bit flag */
                snprintf(ordbuf, sizeof ordbuf, "ordinal#%u", *lookup & 0xFFFF);
                sym = ordbuf;
            } else {
                sym = (const char *)rva(im, *lookup & 0x7FFFFFFF) + 2;
            }
            fn = winstub_lookup(dll, sym);
            if (fn) resolved++;
            else {
                if (g_nimp < MAX_IMPORTS) {
                    snprintf(g_imp[g_nimp].dll, sizeof g_imp[0].dll, "%s", dll);
                    snprintf(g_imp[g_nimp].sym, sizeof g_imp[0].sym, "%s", sym);
                    fn = make_stub((uint32_t)g_nimp);
                    g_nimp++;
                } else fn = make_stub(0);
                stubbed++;
            }
            *iat = (uint32_t)(uintptr_t)fn;
        }
    }
    g_nresolved = resolved;
    PLOG("imports: %d implemented, %d stubbed\n", resolved, stubbed);
    return 0;
}

static const uint8_t *g_tls_tmpl;
static uint32_t       g_tls_raw, g_tls_total, g_tls_index;

static void tls_bind_current_thread(void)
{
    uint8_t *blk;
    if (!g_tls_tmpl || !g_teb) return;
    if (g_teb->slots[g_tls_index]) return;
    if (!(blk = calloc(1, g_tls_total ? g_tls_total : 1))) return;
    memcpy(blk, g_tls_tmpl, g_tls_raw);
    g_teb->slots[g_tls_index] = blk;
}

static int setup_tls(image32 *im)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_TLS];
    TLS_DIR32 *t;
    uint32_t *cb;

    if (!d.VirtualAddress) return 0;
    t = rva(im, d.VirtualAddress);
    g_tls_tmpl  = (const uint8_t *)(uintptr_t)t->StartAddressOfRawData;
    g_tls_raw   = t->EndAddressOfRawData - t->StartAddressOfRawData;
    g_tls_total = g_tls_raw + t->SizeOfZeroFill;
    g_tls_index = 0;
    *(uint32_t *)(uintptr_t)t->AddressOfIndex = g_tls_index;
    tls_bind_current_thread();
    PLOG("tls: %u bytes (%u raw), slot %u\n", g_tls_total, g_tls_raw, g_tls_index);

    if (t->AddressOfCallBacks) {
        int n = 0;
        for (cb = (uint32_t *)(uintptr_t)t->AddressOfCallBacks; *cb; cb++) {
            void WINAPI_ (*f)(void *, uint32_t, void *) =
                (void WINAPI_ (*)(void *, uint32_t, void *))(uintptr_t)*cb;
            f(im->base, 1, NULL);
            n++;
        }
        PLOG("tls: ran %d callback(s)\n", n);
    }
    return 0;
}

static void *find_export(image32 *im, const char *want)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_EXPORT];
    EXP_DIR *e;
    uint32_t *names, *funcs, i;
    uint16_t *ords;

    if (!d.VirtualAddress) return NULL;
    e = rva(im, d.VirtualAddress);
    names = rva(im, e->AddressOfNames);
    funcs = rva(im, e->AddressOfFunctions);
    ords  = rva(im, e->AddressOfNameOrdinals);
    for (i = 0; i < e->NumberOfNames; i++)
        if (!strcmp((const char *)rva(im, names[i]), want)) return rva(im, funcs[ords[i]]);
    return NULL;
}

/* --------------------------------------------------------------- VST2 host */


/* The transport the plugin reads through audioMasterGetTime. File scope because
 * the plugin keeps the pointer after the callback returns. */
static VstTimeInfo g_transport;
static double      g_play_pos;

static intptr_t VSTCALL_ host_callback(AEffect32 *fx, int32_t op, int32_t idx,
                                      intptr_t val, void *ptr, float opt)
{
    (void)fx; (void)idx; (void)val; (void)opt;
    switch (op) {
    case 1:  return 2400;      /* audioMasterVersion       */
    case 16: return 48000;     /* audioMasterGetSampleRate */
    case 17: return 512;       /* audioMasterGetBlockSize  */
    case 23: return 1;         /* GetCurrentProcessLevel   */
    case 7:                    /* audioMasterGetTime       */
        /* Not NULL: a plugin that reads tempo dereferences what it is handed. */
        vst_time_set(&g_transport, g_play_pos, 48000.0);
        return (intptr_t)&g_transport;
    case 32: case 33: if (ptr) snprintf(ptr, 64, "peload32"); return 1;
    case 34: return 1000;
    default: return 0;
    }
}

static int write_wav(const char *path, const float *inter, int frames, int sr)
{
    unsigned char h[44];
    uint32_t bytes = (uint32_t)frames * 4u;
    int16_t *pcm;
    FILE *f;
    int i;

    if (!(pcm = malloc((size_t)frames * 2 * sizeof *pcm))) return 0;
    for (i = 0; i < frames * 2; i++) {
        double v = (double)inter[i] * 32767.0;
        pcm[i] = (int16_t)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
    }
    memcpy(h, "RIFF", 4);
    { uint32_t v = 36 + bytes; memcpy(h + 4, &v, 4); }
    memcpy(h + 8, "WAVEfmt ", 8);
    { uint32_t v = 16;     memcpy(h + 16, &v, 4); }
    { uint16_t v = 1;      memcpy(h + 20, &v, 2); }
    { uint16_t v = 2;      memcpy(h + 22, &v, 2); }
    { uint32_t v = sr;     memcpy(h + 24, &v, 4); }
    { uint32_t v = sr * 4; memcpy(h + 28, &v, 4); }
    { uint16_t v = 4;      memcpy(h + 32, &v, 2); }
    { uint16_t v = 16;     memcpy(h + 34, &v, 2); }
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    if (!(f = fopen(path, "wb"))) { perror(path); free(pcm); return 0; }
    fwrite(h, 1, sizeof h, f);
    fwrite(pcm, 2, (size_t)frames * 2, f);
    fclose(f); free(pcm);
    return 1;
}

/* Every failure between here and serve_run() used to just return, which in
 * --serve mode meant exiting with the socket never opened. The client reads
 * that as a dead socket and reports "the helper died before reporting" --
 * true, but it throws away the specific reason this process already had in
 * hand. Send it across instead, the same way serve_run's own BR_HELLO would
 * have. */
static void serve_report_fail(int fd, const char *msg)
{
    bridge_rep r;
    memset(&r, 0, sizeof r);
    r.ok = 0;
    snprintf(r.text, sizeof r.text, "%s", msg);
    send(fd, &r, sizeof r, MSG_NOSIGNAL);
}

int main(int argc, char **argv)
{
    image32 im;
    const char *path = NULL, *wav = NULL;
    int secs = 2, note = 60, prog = 0, dump = 0, play = 0, i;
    int edframes = 30, edframes_set = 0, editor = 0, serve_fd = -1;
    /* Told to us by the bridge, so the helper agrees with the host it serves. */
    double srate = 48000.0;
    int    bsize = 512;
    const char *midi_port = NULL, *edpng = NULL, *shm_path = NULL;
    void *entry, *vm;
    AEffect32 *fx;

    /* a fault in guest code takes the process down with it; don't lose the log
     * that says how far we got */
    setvbuf(stdout, NULL, _IONBF, 0);

    memset(&im, 0, sizeof im);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--render") && i + 1 < argc)       wav  = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc)    secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--note") && i + 1 < argc)    note = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--program") && i + 1 < argc) prog = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--params"))                  dump = 1;
        else if (!strcmp(argv[i], "--play"))                    play = 1;
        else if (!strcmp(argv[i], "--editor"))                   editor = 1;
        else if (!strcmp(argv[i], "--serve") && i + 1 < argc)  serve_fd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shm") && i + 1 < argc)    shm_path = argv[++i];
        /* The bridge passes these. Without them the values landed on the
         * `argv[i][0] != '-'` arm below and became the plugin path, so every
         * bridged 32-bit load failed with "512: No such file or directory" --
         * which is to say pestudio could not host a 32-bit plugin at all. */
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc)   srate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--block") && i + 1 < argc)  bsize = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--editor-png") && i + 1 < argc) edpng = argv[++i];
        else if (!strcmp(argv[i], "--editor-frames") && i + 1 < argc)
                       { edframes = atoi(argv[++i]); edframes_set = 1; }
        else if (!strcmp(argv[i], "--midi") && i + 1 < argc)    midi_port = argv[++i];
        else if (argv[i][0] != '-')                             path = argv[i];
    }
    if (!path) {
        fprintf(stderr,
            "usage: peload32 <plugin.dll> [--params] [--render out.wav]\n"
            "                [--secs N] [--note N] [--program N]\n"
            "                [--editor] [--editor-png out.png] [--editor-frames N]\n"
            "                [--rate HZ] [--block N]\n"
#ifdef PELOAD32_AUDIO
            "                [--play] [--midi CLIENT:PORT]\n"
            "\n"
            "  --play   open a PipeWire output and play live from MIDI in.\n"
            "           With no --midi, every hardware keyboard is subscribed.\n"
#endif
            );
        return 2;
    }

    if (teb_install()) {
        fprintf(stderr, "cannot install fake TEB\n");
        if (serve_fd >= 0) serve_report_fail(serve_fd, "cannot install fake TEB");
        return 1;
    }
    if (map_image(&im, path)) {
        if (serve_fd >= 0) serve_report_fail(serve_fd, "cannot map plug-in image");
        return 1;
    }
    if (apply_relocs(&im) < 0) {
        if (serve_fd >= 0) serve_report_fail(serve_fd, "relocation failed");
        return 1;
    }
    faults_report(&im);
    winstubs_init(im.base, im.opt->DataDirectory[DIR_RESOURCE].VirtualAddress
                            ? im.base + im.opt->DataDirectory[DIR_RESOURCE].VirtualAddress
                            : NULL);
    resolve_imports(&im);
    protect_sections(&im);
    setup_tls(&im);

    entry = im.opt->AddressOfEntryPoint ? rva(&im, im.opt->AddressOfEntryPoint) : NULL;
    if (entry) {
        int32_t WINAPI_ (*dllmain)(void *, uint32_t, void *) =
            (int32_t WINAPI_ (*)(void *, uint32_t, void *))entry;
        int32_t r = dllmain(im.base, 1, NULL);
        PLOG("DllMain returned %d\n", r);
        if (!r) {
            fprintf(stderr, "DllMain failed\n");
            if (serve_fd >= 0) serve_report_fail(serve_fd, "DllMain failed");
            return 1;
        }
    }

    if (!(vm = find_export(&im, "VSTPluginMain")) && !(vm = find_export(&im, "main"))) {
        fprintf(stderr, "no VSTPluginMain export\n");
        if (serve_fd >= 0) serve_report_fail(serve_fd, "no VSTPluginMain export");
        return 1;
    }
    {
        AEffect32 *VSTCALL_ (*f)(host_cb32) = (AEffect32 *VSTCALL_ (*)(host_cb32))vm;
        fx = f(host_callback);
    }
    if (!fx || fx->magic != 0x56737450) {
        fprintf(stderr, "VSTPluginMain gave no valid AEffect (fx=%p)\n", (void *)fx);
        if (serve_fd >= 0) serve_report_fail(serve_fd, "VSTPluginMain gave no valid AEffect");
        return 1;
    }

    printf("\n");
    { char nm[64] = { 0 }, vn[64] = { 0 };
      fx->dispatcher(fx, effOpen, 0, 0, NULL, 0.0f);
      fx->dispatcher(fx, effSetSampleRate, 0, 0, NULL, (float)srate);
      fx->dispatcher(fx, effSetBlockSize, 0, bsize, NULL, 0.0f);
      fx->dispatcher(fx, effMainsChanged, 0, 1, NULL, 0.0f);
      fx->dispatcher(fx, effGetEffectName, 0, 0, nm, 0.0f);
      fx->dispatcher(fx, effGetVendorString, 0, 0, vn, 0.0f);
      printf("%s -- %s\n", nm, vn);
    }
    printf("  uniqueID 0x%08x  %s  in %d  out %d  programs %d  params %d\n",
           fx->uniqueID, (fx->flags & 0x100) ? "synth" : "effect",
           fx->numInputs, fx->numOutputs, fx->numPrograms, fx->numParams);

    if (prog > 0 && prog < fx->numPrograms)
        fx->dispatcher(fx, effSetProgram, 0, prog, NULL, 0.0f);
    { char pn[64] = { 0 };
      fx->dispatcher(fx, effGetProgramName, 0, 0, pn, 0.0f);
      printf("  program %d: \"%s\"\n", prog, pn); }

    { int n = fx->numParams, lim = dump ? n : (n < 8 ? n : 8);
      printf("\nparameters:\n");
      for (i = 0; i < lim; i++) {
          char nb[64] = { 0 }, db[64] = { 0 };
          fx->dispatcher(fx, effGetParamName, i, 0, nb, 0.0f);
          fx->dispatcher(fx, effGetParamDisplay, i, 0, db, 0.0f);
          printf("  %3d %-24s %-10s (raw %.4f)\n", i, nb, db,
                 fx->getParameter ? fx->getParameter(fx, i) : 0.0f);
      }
      if (lim < n) printf("  ... %d more (--params for all)\n", n - lim);
    }

    if (wav) {
        int bs = 512, total = 48000 * secs, done = 0;
        int nin = fx->numInputs > 8 ? 8 : fx->numInputs;
        /* The plugin writes to every one of its declared outputs, so the
         * pointer array has to be that long. Truncating the count but still
         * passing a shorter array is what a drum machine with 16 outs reads off
         * the end of -- give it exactly what it asked for. */
        int nout = fx->numOutputs, nchan;
        float **ins, **outs, *inter;
        struct { VstEvents32 ev; VstMidiEvent32 m; } pkt;
        double peak = 0.0;

        if (nout < 1) nout = 1;
        if (nin > 256 || nout > 256) {
            fprintf(stderr, "implausible channel count (in %d out %d)\n", nin, nout);
            return 1;
        }
        nchan = nin > nout ? nin : nout;
        ins  = calloc((size_t)nchan + 1, sizeof *ins);
        outs = calloc((size_t)nchan + 1, sizeof *outs);
        if (!ins || !outs) return 1;
        for (i = 0; i < nin; i++)  ins[i]  = calloc((size_t)bs, sizeof **ins);
        for (i = 0; i < nout; i++) outs[i] = calloc((size_t)bs, sizeof **outs);
        if (!(inter = malloc((size_t)total * 2 * sizeof *inter))) return 1;

        memset(&pkt, 0, sizeof pkt);
        pkt.ev.numEvents = 1;
        pkt.ev.events[0] = &pkt.m;
        pkt.m.type = 1; pkt.m.byteSize = 24;
        pkt.m.midiData[0] = (char)0x90;
        pkt.m.midiData[1] = (char)note;
        pkt.m.midiData[2] = 100;
        fx->dispatcher(fx, effProcessEvents, 0, 0, &pkt.ev, 0.0f);

        while (done < total) {
            int n = (total - done < bs) ? total - done : bs, j;
            if (done <= total * 2 / 3 && done + n > total * 2 / 3) {
                pkt.m.midiData[0] = (char)0x80;
                pkt.m.midiData[2] = 0;
                fx->dispatcher(fx, effProcessEvents, 0, 0, &pkt.ev, 0.0f);
            }
            for (j = 0; j < nin; j++) {
                int k;
                for (k = 0; k < n; k++)
                    ins[j][k] = ((done + k) % 12000 == 0) ? 0.5f : 0.0f;
            }
            for (j = 0; j < nout; j++) memset(outs[j], 0, (size_t)bs * sizeof **outs);
            fx->processReplacing(fx, nin ? ins : NULL, outs, n);
            g_play_pos += n;
            for (j = 0; j < n; j++) {
                inter[(done + j) * 2]     = outs[0][j];
                inter[(done + j) * 2 + 1] = nout >= 2 ? outs[1][j] : outs[0][j];
            }
            done += n;
        }
        for (i = 0; i < total * 2; i++) if (fabs(inter[i]) > peak) peak = fabs(inter[i]);
        printf("\nrendered %d frames, peak %.4f%s\n", total, peak,
               peak < 1e-9 ? "  !! silence" : "");
        if (write_wav(wav, inter, total, 48000)) printf("wrote %s\n", wav);
    }

    /* --serve hands this plugin to a 64-bit pehost over a socket and a shared
     * region; see bridge.h. It never returns until the host disconnects. */
    if (serve_fd >= 0) {
        if (!shm_path) { fprintf(stderr, "--serve needs --shm\n"); return 2; }
        return serve_run(fx, serve_fd, shm_path);
    }

    if (edpng) return ed_render_png(fx, edpng, edframes);

#ifdef PELOAD32_AUDIO
    /* --play starts the audio thread and returns; with --editor the window loop
     * then runs on this thread, so a 32-bit plugin can be played and tweaked at
     * once. Without --editor, play_run parks here forever. */
    if (play) {
        if (editor) { if (play_start(fx, argc, argv, midi_port)) return 1; }
        else        return play_run(fx, argc, argv, midi_port);
    }
#else
    if (play) fprintf(stderr, "built without 32-bit PipeWire/ALSA; --play unavailable\n");
#endif

#ifdef PELOAD32_X11
    /* --editor-frames bounds the loop so a scripted run closes its own window;
     * 0 (the default here) means run until the window is closed. */
    if (editor) return xw_run(fx, edframes_set ? edframes : 0);
#else
    if (editor) fprintf(stderr, "built without 32-bit X11; --editor unavailable\n");
#endif

    { int hit = 0;
      for (i = 0; i < g_nimp; i++) if (g_imp[i].calls) hit++;
      printf("\nimports: %d implemented, %d stubbed, %d stubs reached\n",
             g_nresolved, g_nimp, hit);
      for (i = 0; i < g_nimp; i++)
          if (g_imp[i].calls)
              printf("    %-16s %-40s %lu\n", g_imp[i].dll, g_imp[i].sym, g_imp[i].calls);
    }
    fflush(NULL);
    _exit(0);
}
