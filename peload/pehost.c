/* pehost -- load a Windows x86-64 VST2 DLL natively on Linux, no Wine.
 *
 * Why this can work at all: a Windows x64 DLL contains the same machine code a
 * Linux x86-64 process runs. Only four things differ, and all four are cheap:
 *
 *   container   PE32+ instead of ELF     -> map sections and apply relocs here
 *   ABI         Microsoft x64 vs SysV    -> __attribute__((ms_abi)), no asm
 *   TEB         MSVC reads gs:[0x30]     -> arch_prctl(ARCH_SET_GS) on a fake
 *                                           TEB; glibc uses %fs so %gs is free
 *   imports     kernel32/user32/...      -> a table of native stubs
 *
 * This is deliberately headless: the DSP path is the target, the editor is not.
 * Unimplemented imports get a generated trampoline that records the call and
 * returns 0, so a run tells us exactly which of the 341 imported symbols the
 * plugin actually touches rather than making us guess.
 *
 * Public API: pehost.h. The CLI front end is peload_cli.c. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <dlfcn.h>
#include <dirent.h>

#include "bridge_client.h"
#include "macvsthost.h"
#include "macau.h"
/* The Cocoa stand-ins and the software Metal backend: a macOS VST3 draws
 * through them directly rather than through a backend of its own. */
#include "macshim.h"
#include "pefvst.h"
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>

#define MS __attribute__((ms_abi))

/* Set PELOAD_VERBOSE=1 for loader chatter: unresolved imports, TLS layout and
 * per-stage timings. Off by default -- a plugin probing forty optional APIs
 * through GetProcAddress should not bury the output. */
static int pe_verbose(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("PELOAD_VERBOSE"); v = e && *e != '0'; } return v; }

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
    /* PE32+ has no BaseOfData -- ImageBase follows BaseOfCode directly, and it
     * is 8 bytes here where PE32 has 4. Getting this wrong shifts every field
     * after it, including SizeOfImage and the whole data directory. */
    uint64_t ImageBase;
    uint32_t SectionAlignment, FileAlignment;
    uint16_t MajorOSVersion, MinorOSVersion, MajorImageVersion, MinorImageVersion;
    uint16_t MajorSubsystemVersion, MinorSubsystemVersion;
    uint32_t Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t Subsystem, DllCharacteristics;
    uint64_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    uint32_t LoaderFlags, NumberOfRvaAndSizes;
    DATA_DIR DataDirectory[16];
} OPT_HDR64;
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
    uint64_t StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks;
    uint32_t SizeOfZeroFill, Characteristics;
} TLS_DIR64;
#pragma pack(pop)

#define DIR_EXPORT   0
#define DIR_RESOURCE 2
#define DIR_IMPORT 1
#define DIR_RELOC  5
#define DIR_TLS    9

/* ---------------------------------------------------------------- fake TEB */

/* MSVC-generated code reaches TLS and the security cookie through the TEB at
 * %gs. On x86-64 Linux glibc keeps its own TLS in %fs, so %gs is ours to take. */
#define TLS_SLOTS 128
#define TEB_SIZE  0x1000
/* Offsets MSVC-generated code and the static CRT actually read. */
#define TEB_STACK_BASE   0x08
#define TEB_STACK_LIMIT  0x10
#define TEB_SELF         0x30
#define TEB_TLS_PTR      0x58   /* ThreadLocalStoragePointer: a POINTER to slots */
#define TEB_LAST_ERROR   0x68

typedef struct {
    uint8_t *raw;                 /* TEB_SIZE bytes, installed on %gs */
    void    *slots[TLS_SLOTS];
} teb_t;

static __thread teb_t *g_teb;

/* Every thread that runs plug-in code, so a thread-local slot can be cleared
 * everywhere it exists.
 *
 * Windows hands out a TlsAlloc slot that reads NULL on every thread. Once slots
 * are recycled between plug-ins -- which they now are, or a session runs out of
 * them -- a slot still holding the last plug-in's pointer on some other thread
 * is a fault waiting for whichever thread reads it before writing it. The audio
 * thread is exactly such a thread. */
#define MAX_TEBS 64
static teb_t          *g_tebs[MAX_TEBS];
static int             g_nteb;
static pthread_mutex_t g_teb_lock = PTHREAD_MUTEX_INITIALIZER;

static void teb_register(teb_t *t)
{
    pthread_mutex_lock(&g_teb_lock);
    if (g_nteb < MAX_TEBS) g_tebs[g_nteb++] = t;
    pthread_mutex_unlock(&g_teb_lock);
}
static void teb_clear_slot(uint32_t i)
{
    int k;
    if (i >= TLS_SLOTS) return;
    pthread_mutex_lock(&g_teb_lock);
    for (k = 0; k < g_nteb; k++)
        if (g_tebs[k]) g_tebs[k]->slots[i] = NULL;
    pthread_mutex_unlock(&g_teb_lock);
}
static void teb_clear_all_slots(void)
{
    int k;
    pthread_mutex_lock(&g_teb_lock);
    for (k = 0; k < g_nteb; k++)
        if (g_tebs[k]) memset(g_tebs[k]->slots + 8, 0,
                              (TLS_SLOTS - 8) * sizeof g_tebs[k]->slots[0]);
    pthread_mutex_unlock(&g_teb_lock);
}

/* The module's TLS template. TLS is per-thread by definition, so each thread
 * that runs plugin code needs its own copy of this block, not a shared one --
 * without it the audio thread reads a NULL TLS pointer on first use. */
static const uint8_t *g_tls_tmpl;
static size_t         g_tls_raw, g_tls_total;
static uint32_t       g_tls_index;

static void tls_bind_current_thread(void)
{
    uint8_t *blk;
    if (!g_tls_tmpl || !g_teb) return;
    if (g_teb->slots[g_tls_index]) return;              /* already bound */
    if (!(blk = calloc(1, g_tls_total ? g_tls_total : 1))) return;
    memcpy(blk, g_tls_tmpl, g_tls_raw);
    g_teb->slots[g_tls_index] = blk;
}

static int teb_install(void)
{
    teb_t *t = calloc(1, sizeof *t);
    if (!t) return -1;
    if (posix_memalign((void **)&t->raw, 4096, TEB_SIZE)) { free(t); return -1; }
    memset(t->raw, 0, TEB_SIZE);

    *(void **)(t->raw + TEB_SELF)    = t->raw;
    *(void **)(t->raw + TEB_TLS_PTR) = t->slots;
    /* Rough stack bounds; some CRT paths sanity-check against these. */
    {
        pthread_attr_t at; void *sp = NULL; size_t ss = 0;
        if (!pthread_getattr_np(pthread_self(), &at) &&
            !pthread_attr_getstack(&at, &sp, &ss)) {
            *(void **)(t->raw + TEB_STACK_LIMIT) = sp;
            *(void **)(t->raw + TEB_STACK_BASE)  = (uint8_t *)sp + ss;
            pthread_attr_destroy(&at);
        }
    }
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, t->raw) != 0) { free(t->raw); free(t); return -1; }
    g_teb = t;
    teb_register(t);
    tls_bind_current_thread();
    return 0;
}

/* ------------------------------------------------------------ loaded image */

typedef struct {
    uint8_t   *base;
    size_t     size;
    OPT_HDR64 *opt;
    FILE_HDR  *fh;
    SEC_HDR   *sec;
    int        nsec;
    char       name[128];
} image;

static uint8_t *g_file;
static size_t   g_filesz;

/* --------------------------------------------------- missing-import tracker */

#define MAX_IMPORTS 2048
typedef struct { char dll[32], sym[96]; unsigned long calls; } imprec;
static imprec  g_imp[MAX_IMPORTS];
static int     g_nimp;
static int      g_nresolved;      /* imports satisfied by a real stub */
static pefvst  *g_last_classic;   /* for pehost_import_stats on a Classic plugin */

/* Generated stubs live in executable pages of their own.
 *
 * One arena is normally the lot: a plug-in leaves a few hundred symbols
 * unimplemented. But stubs are never reclaimed when an image is unloaded, and a
 * pestudio session browsing a folder loads plug-in after plug-in -- so the old
 * fixed megabyte ran out after some 160 loads, and ran out by writing past the
 * end of it and executing whatever came next. Taking a fresh arena when one
 * fills is cheaper than counting, and the pages are lazily committed, so an
 * arena nobody fills costs nothing. */
#define TRAMP_ARENA (1 << 20)
#define TRAMP_STUB  32

static uint8_t *g_tramp;          /* the arena being filled */
static size_t   g_tramp_left;     /* bytes still free in it */

/* What a stub should hand back when it cannot do the thing.
 *
 * Returning 0 is the obvious choice and it is wrong for a large part of this
 * surface, because 0 is the *success* value across several Windows return
 * conventions. The generic stub was therefore not saying "this did not
 * happen"; for those symbols it was affirmatively claiming it had. Measured
 * cases from the corpus here:
 *
 *   _waccess           0 means "the file is there and you may read it"
 *   _wopen             0 is a valid descriptor -- specifically stdin
 *   GetIfTable         0 is NO_ERROR, over a table the caller never had filled
 *   CoGetObject        0 is S_OK, over an interface pointer left uninitialised
 *
 * Each of those sends the caller down its success path holding nothing, which
 * is strictly worse than an error it would have handled: NI Kontakt asked
 * whether its library existed, was told yes, opened it, got stdin, and read
 * silence out of it.
 *
 * The rule is only ever applied to symbols nobody has implemented -- a real
 * stub in g_stubs[] never reaches here -- so the worst case is a plugin taking
 * an error path for a call that was never going to work anyway.
 *
 * Two prefix rules cover whole families where the convention is unambiguous,
 * and the table names the individual symbols worth being sure about. Anything
 * not matched keeps the old 0, which remains right for the great majority:
 * handle- and pointer-returning calls, BOOLs, and counts. */

typedef struct { const char *dll, *sym; uint64_t val; } failval;

#define FV_ENOTIMPL   0x80004001ull   /* E_NOTIMPL, for HRESULT */
#define FV_NOTIMPL    120ull          /* ERROR_CALL_NOT_IMPLEMENTED, Win32 codes */
#define FV_STATUS     0xC0000002ull   /* STATUS_NOT_IMPLEMENTED, NTSTATUS */
#define FV_MINUS1     0xFFFFFFFFFFFFFFFFull

static const failval g_failvals[] = {
    /* CRT: 0 is success, or a descriptor. -1 is the failure both report. */
    { "msvcrt.dll", "_access",    FV_MINUS1 },
    { "msvcrt.dll", "_waccess",   FV_MINUS1 },
    { "msvcrt.dll", "_open",      FV_MINUS1 },
    { "msvcrt.dll", "_wopen",     FV_MINUS1 },
    { "msvcrt.dll", "_sopen",     FV_MINUS1 },
    { "msvcrt.dll", "_wsopen",    FV_MINUS1 },
    { "msvcrt.dll", "_sopen_s",   FV_MINUS1 },
    { "msvcrt.dll", "_creat",     FV_MINUS1 },
    { "msvcrt.dll", "_dup",       FV_MINUS1 },
    { "msvcrt.dll", "_fileno",    FV_MINUS1 },
    { "msvcrt.dll", "_chdir",     FV_MINUS1 },
    { "msvcrt.dll", "_wchdir",    FV_MINUS1 },
    { "msvcrt.dll", "_rmdir",     FV_MINUS1 },
    { "msvcrt.dll", "_wrmdir",    FV_MINUS1 },
    { "msvcrt.dll", "_chmod",     FV_MINUS1 },
    { "msvcrt.dll", "_wchmod",    FV_MINUS1 },
    { "msvcrt.dll", "_stat",      FV_MINUS1 },
    { "msvcrt.dll", "_stat32",    FV_MINUS1 },
    { "msvcrt.dll", "_stat64",    FV_MINUS1 },
    { "msvcrt.dll", "_wstat",     FV_MINUS1 },
    { "msvcrt.dll", "_wstat32",   FV_MINUS1 },
    { "msvcrt.dll", "_wstat64",   FV_MINUS1 },
    { "msvcrt.dll", "_fstat",     FV_MINUS1 },
    { "msvcrt.dll", "_fstat64",   FV_MINUS1 },
    { "msvcrt.dll", "_setmode",   FV_MINUS1 },
    { "msvcrt.dll", "_locking",   FV_MINUS1 },
    { "msvcrt.dll", "_commit",    FV_MINUS1 },
    { "msvcrt.dll", "_chsize",    FV_MINUS1 },
    { "msvcrt.dll", "_lseek",     FV_MINUS1 },
    { "msvcrt.dll", "_lseeki64",  FV_MINUS1 },
    { "msvcrt.dll", "_telli64",   FV_MINUS1 },
    { "msvcrt.dll", "_read",      FV_MINUS1 },
    { "msvcrt.dll", "_write",     FV_MINUS1 },
    { "msvcrt.dll", "fseek",      FV_MINUS1 },
    { "msvcrt.dll", "ftell",      FV_MINUS1 },
    { "msvcrt.dll", "fclose",     FV_MINUS1 },
    { "msvcrt.dll", "fflush",     FV_MINUS1 },
    { "msvcrt.dll", "remove",     FV_MINUS1 },
    { "msvcrt.dll", "rename",     FV_MINUS1 },
    { "msvcrt.dll", "_unlink",    FV_MINUS1 },
    { "msvcrt.dll", "_wunlink",   FV_MINUS1 },
    { "msvcrt.dll", "_mkdir",     FV_MINUS1 },
    { "msvcrt.dll", "_wmkdir",    FV_MINUS1 },
    { "msvcrt.dll", "system",     FV_MINUS1 },
    { "msvcrt.dll", "_wsystem",   FV_MINUS1 },
    { "msvcrt.dll", "_putenv",    FV_MINUS1 },
    { "msvcrt.dll", "_wputenv",   FV_MINUS1 },

    /* COM and the shell: HRESULT, where 0 is S_OK and the out-parameter the
     * caller is about to dereference was never written. */
    { "ole32.dll",    "CoGetObject",              FV_ENOTIMPL },
    { "ole32.dll",    "CoCreateInstance",         FV_ENOTIMPL },
    { "ole32.dll",    "CoCreateInstanceEx",       FV_ENOTIMPL },
    { "ole32.dll",    "CoGetClassObject",         FV_ENOTIMPL },
    { "ole32.dll",    "CoGetMalloc",              FV_ENOTIMPL },
    { "ole32.dll",    "CoCreateGuid",             FV_ENOTIMPL },
    { "ole32.dll",    "CLSIDFromString",          FV_ENOTIMPL },
    { "ole32.dll",    "CLSIDFromProgID",          FV_ENOTIMPL },
    { "ole32.dll",    "IIDFromString",            FV_ENOTIMPL },
    { "ole32.dll",    "StringFromCLSID",          FV_ENOTIMPL },
    { "ole32.dll",    "OleInitialize",            FV_ENOTIMPL },
    { "ole32.dll",    "StgOpenStorage",           FV_ENOTIMPL },
    { "ole32.dll",    "StgCreateDocfile",         FV_ENOTIMPL },
    { "oleaut32.dll", "VariantChangeType",        FV_ENOTIMPL },
    { "shell32.dll",  "SHGetKnownFolderPath",     FV_ENOTIMPL },
    { "shell32.dll",  "SHCreateItemFromParsingName", FV_ENOTIMPL },
    { "shlwapi.dll",  "UrlCreateFromPathW",       FV_ENOTIMPL },
    { "shlwapi.dll",  "UrlCreateFromPathA",       FV_ENOTIMPL },
    { "shlwapi.dll",  "SHGetValueW",              FV_NOTIMPL  },
    { "shlwapi.dll",  "SHGetValueA",              FV_NOTIMPL  },

    /* The IP helper: Win32 error codes, 0 being NO_ERROR. GetIfTable is the
     * one every failing plugin here imports -- it is how a licence binds to a
     * machine, and answering NO_ERROR over an unwritten table invites the
     * caller to fingerprint uninitialised stack. */
    { "iphlpapi.dll", "GetIfTable",           FV_NOTIMPL },
    { "iphlpapi.dll", "GetIfTable2",          FV_NOTIMPL },
    { "iphlpapi.dll", "GetIfEntry",           FV_NOTIMPL },
    { "iphlpapi.dll", "GetAdaptersInfo",      FV_NOTIMPL },
    { "iphlpapi.dll", "GetAdaptersAddresses", FV_NOTIMPL },
    { "iphlpapi.dll", "GetIpAddrTable",       FV_NOTIMPL },
    { "iphlpapi.dll", "GetNetworkParams",     FV_NOTIMPL },
    { "iphlpapi.dll", "GetIpForwardTable",    FV_NOTIMPL },

    /* Registry and service calls in advapi32 also report Win32 codes. */
    { "advapi32.dll", "RegCreateKeyExW",  FV_NOTIMPL },
    { "advapi32.dll", "RegCreateKeyExA",  FV_NOTIMPL },
    { "advapi32.dll", "RegSetValueExW",   FV_NOTIMPL },
    { "advapi32.dll", "RegSetValueExA",   FV_NOTIMPL },
    { "advapi32.dll", "RegDeleteKeyW",    FV_NOTIMPL },
    { "advapi32.dll", "RegDeleteValueW",  FV_NOTIMPL },
    { "advapi32.dll", "RegEnumKeyExW",    FV_NOTIMPL },
    { "advapi32.dll", "RegEnumValueW",    FV_NOTIMPL },
    { "advapi32.dll", "RegQueryInfoKeyW", FV_NOTIMPL },

    { NULL, NULL, 0 }
};

/* ntdll's Nt/Zw entry points all return NTSTATUS, where 0 is STATUS_SUCCESS.
 * The family is large and uniform, so it is matched by prefix rather than
 * listed. Rtl* is deliberately not: those return every convention there is. */
static int ntdll_status_call(const char *dll, const char *sym)
{
    if (strcasecmp(dll, "ntdll.dll") != 0) return 0;
    if ((sym[0] == 'N' && sym[1] == 't') || (sym[0] == 'Z' && sym[1] == 'w'))
        return sym[2] >= 'A' && sym[2] <= 'Z';
    return 0;
}

static uint64_t stub_failure_value(const char *dll, const char *sym)
{
    int i;
    for (i = 0; g_failvals[i].sym; i++)
        if (!strcasecmp(g_failvals[i].dll, dll) && !strcmp(g_failvals[i].sym, sym))
            return g_failvals[i].val;
    /* The CRT is reached under several names -- msvcrt, msvcr120, msvcr100 --
     * and the table spells it once. Match on the family instead of the file. */
    if (!strncasecmp(dll, "msvcr", 5) || !strncasecmp(dll, "msvcrt", 6))
        for (i = 0; g_failvals[i].sym; i++)
            if (!strcasecmp(g_failvals[i].dll, "msvcrt.dll") &&
                !strcmp(g_failvals[i].sym, sym))
                return g_failvals[i].val;
    if (ntdll_status_call(dll, sym)) return FV_STATUS;
    return 0;
}

static MS uint64_t missing_import(uint32_t idx)
{
    uint64_t v;
    if (idx >= (uint32_t)g_nimp) return 0;
    v = stub_failure_value(g_imp[idx].dll, g_imp[idx].sym);
    if (g_imp[idx].calls++ == 0 && pe_verbose()) {
        fprintf(stderr, "  [stub] %s!%s", g_imp[idx].dll, g_imp[idx].sym);
        /* Naming the value matters when it is not zero: it is the difference
         * between the caller taking its error path and the caller believing
         * the call worked. */
        if (v) fprintf(stderr, " -> 0x%llx (failure)", (unsigned long long)v);
        fputc('\n', stderr);
    }
    /* PELOAD_MSVCP_STRICT: stop at the first C++ library entry point
     * msvcp_shim.h does not implement, and name it.
     *
     * This is for building that shim out, and it exists because the default is
     * unhelpful there in a specific way: a stub answering 0 for "how many
     * characters did you write" sends the caller round a loop that never ends,
     * so the symbol that is missing never gets reported at all -- the run just
     * stops responding. Stopping here turns each round of that work into one
     * name instead of an afternoon. */
    {
        static int strict = -1;
        if (strict < 0) {
            const char *e = getenv("PELOAD_MSVCP_STRICT");
            strict = e && *e != '0';
        }
        if (strict && !strncasecmp(g_imp[idx].dll, "msvcp", 5)) {
            fprintf(stderr, "[msvcp] %s!%s is not implemented here\n",
                    g_imp[idx].dll, g_imp[idx].sym);
            fflush(stderr);
            abort();
        }
    }
    return v;
}

/* Emit a tiny MS-ABI stub: reserve shadow space, pass idx as arg1, call the
 * recorder, return its value. Original args are discarded -- we only need to
 * know the symbol was reached. */
static void *make_stub(uint32_t idx)
{
    uint8_t *stub, *p;

    if (g_tramp_left < TRAMP_STUB) {
        g_tramp = mmap(NULL, TRAMP_ARENA, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_tramp == MAP_FAILED) { perror("mmap tramp"); exit(1); }
        g_tramp_left = TRAMP_ARENA;
    }
    stub = g_tramp;
    g_tramp      += TRAMP_STUB;
    g_tramp_left -= TRAMP_STUB;

    /* sub rsp,0x28 ; mov ecx,idx ; mov rax,missing_import ; call rax
     * add rsp,0x28 ; ret
     * entry rsp%16==8, minus 40 -> 0 mod 16 at the call: correct for MS ABI. */
    p = stub;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28;
    *p++ = 0xB9; memcpy(p, &idx, 4); p += 4;
    *p++ = 0x48; *p++ = 0xB8;
    { void *fn = (void *)missing_import; memcpy(p, &fn, 8); p += 8; }
    *p++ = 0xFF; *p++ = 0xD0;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28;
    *p++ = 0xC3;
    return stub;
}

/* ------------------------------------------------------------- win32 stubs */

#include <stddef.h>
#include <stdatomic.h>
#include "pehost.h"
#include "peimage.h"
#include "win32host.h"
#include "winstubs.h"

/* ----------------------------------------------------------------- loading */

static void *rva(image *im, uint64_t r) { return im->base + r; }

/* The directory the current image came from. A plugin that needs a runtime DLL
 * has usually been shipped alongside one. */
static char g_image_dir[1024];

static int map_image(image *im, const char *path)
{
    int fd;
    struct stat st;
    DOS_HDR *dos;
    uint8_t *f;
    int i;

    {   /* Remember the directory, for finding runtime DLLs shipped alongside. */
        const char *slash = strrchr(path, '/');
        if (slash && (size_t)(slash - path) < sizeof g_image_dir)
            snprintf(g_image_dir, sizeof g_image_dir, "%.*s",
                     (int)(slash - path), path);
    }
    /* And the file itself, so GetModuleFileName can answer with where the
     * plug-in really is rather than a placeholder -- which is how a plug-in
     * that loads its own data from beside itself finds it. */
    winstubs_set_image_path(path);

    im->base = NULL;
    im->size = 0;

    if ((fd = open(path, O_RDONLY)) < 0) { perror(path); return -1; }
    if (fstat(fd, &st)) { close(fd); return -1; }
    f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) { perror("mmap file"); return -1; }
    g_file = f; g_filesz = st.st_size;

    /* Every size below comes out of the file, so every one of them is checked.
     * A truncated download is the realistic case -- and without these an
     * out-of-range SizeOfHeaders or section VirtualAddress writes past the end of
     * the image we just mapped.
     *
     * They all leave through `fail`, which hands back whatever has been mapped so
     * far. Returning on the spot leaked the file mapping, and past the point
     * where the image exists it leaked that too -- tens of megabytes for every
     * rejected plug-in, which pestudio produces by the folder. */
#define BAD(msg) do { fprintf(stderr, "%s\n", (msg)); goto fail; } while (0)
#define FITS(off, len) ((uint64_t)(off) + (uint64_t)(len) <= (uint64_t)st.st_size)

    if ((uint64_t)st.st_size < sizeof(DOS_HDR)) BAD("file too small for a DOS header");
    dos = (DOS_HDR *)f;
    if (dos->e_magic != 0x5A4D) BAD("not MZ");
    if (!FITS(dos->e_lfanew, sizeof(FILE_HDR))) BAD("e_lfanew points outside the file");
    im->fh = (FILE_HDR *)(f + dos->e_lfanew);
    if (im->fh->Signature != 0x4550) BAD("not PE");
    if (im->fh->Machine != 0x8664) {
        fprintf(stderr, "machine 0x%x -- this loader is x86-64 only\n", im->fh->Machine);
        goto fail;
    }
    if (im->fh->SizeOfOptionalHeader < sizeof(OPT_HDR64) ||
        !FITS(dos->e_lfanew + sizeof(FILE_HDR), im->fh->SizeOfOptionalHeader))
        BAD("optional header does not fit in the file");
    im->opt = (OPT_HDR64 *)((uint8_t *)im->fh + sizeof *im->fh);
    if (im->opt->Magic != 0x20B) BAD("not PE32+");
    im->sec = (SEC_HDR *)((uint8_t *)im->opt + im->fh->SizeOfOptionalHeader);
    im->nsec = im->fh->NumberOfSections;
    if (im->nsec > 96) BAD("implausible section count");
    if (!FITS((uint8_t *)im->sec - f, (size_t)im->nsec * sizeof(SEC_HDR)))
        BAD("section table runs past the end of the file");

    im->size = im->opt->SizeOfImage;
    if (im->size < im->opt->SizeOfHeaders || im->size > (1u << 31))
        BAD("implausible SizeOfImage");
    im->base = mmap(NULL, im->size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (im->base == MAP_FAILED) { im->base = NULL; perror("mmap image"); goto fail; }
    /* Where it landed, because every question about a crash in guest code
     * starts here: an address minus this is an RVA, and an RVA is what a
     * disassembler and a Ghidra project can both be asked about. Finding it
     * from /proc/self/maps instead -- the anonymous r-xp region -- works and is
     * a detour for something the loader already knows. */
    PLOG("image: %u KiB at %p (link base 0x%llx)\n", im->size / 1024,
         (void *)im->base, (unsigned long long)im->opt->ImageBase);

    if (!FITS(0, im->opt->SizeOfHeaders)) BAD("SizeOfHeaders exceeds the file");
    memcpy(im->base, f, im->opt->SizeOfHeaders);
    for (i = 0; i < im->nsec; i++) {
        SEC_HDR *s = &im->sec[i];
        if (!s->SizeOfRawData) continue;
        /* Both ends: the source has to be inside the file and the destination
         * inside the image. Either one being wrong is a straight overflow. */
        if (!FITS(s->PointerToRawData, s->SizeOfRawData)) {
            fprintf(stderr, "section %d raw data runs past the end of the file "
                            "-- truncated download?\n", i);
            goto fail;
        }
        if ((uint64_t)s->VirtualAddress + s->SizeOfRawData > (uint64_t)im->size) {
            fprintf(stderr, "section %d maps outside the image\n", i);
            goto fail;
        }
        memcpy(im->base + s->VirtualAddress, f + s->PointerToRawData, s->SizeOfRawData);
    }
#undef FITS
#undef BAD
    /* Rebase our header pointers into the mapped copy. */
    {
        uint32_t lfanew = dos->e_lfanew;      /* read before the file goes away */
        im->fh  = (FILE_HDR *)(im->base + lfanew);
        im->opt = (OPT_HDR64 *)((uint8_t *)im->fh + sizeof *im->fh);
        im->sec = (SEC_HDR *)((uint8_t *)im->opt + im->fh->SizeOfOptionalHeader);
    }

    /* Every section is copied into the image now, so the file mapping has done
     * its job. Holding it was leaking the whole DLL -- nearly 9 MB for a Full
     * Bucket VST3 -- on every single load. */
    munmap(f, (size_t)st.st_size);
    g_file = NULL;
    g_filesz = 0;
    return 0;

fail:
    /* Cleared as well as unmapped: the caller sees a zeroed image and can hand
     * it to pe_module_unload without knowing how far this got. */
    if (im->base) munmap(im->base, im->size);
    im->base = NULL;
    im->size = 0;
    munmap(f, (size_t)st.st_size);
    g_file = NULL;
    g_filesz = 0;
    return -1;
}

static int apply_relocs(image *im)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_RELOC];
    int64_t delta = (int64_t)(uintptr_t)im->base - (int64_t)im->opt->ImageBase;
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
        cnt = (b->SizeOfBlock - sizeof *b) / 2;
        for (i = 0; i < cnt; i++) {
            int type = e[i] >> 12, off = e[i] & 0xFFF;
            if (type == 10) {                      /* DIR64 */
                uint64_t *t = (uint64_t *)rva(im, b->VirtualAddress + off);
                *t += delta; n++;
            } else if (type != 0) {
                fprintf(stderr, "unhandled reloc type %d\n", type);
            }
        }
        p += b->SizeOfBlock;
    }
    return n;
}

static int protect_sections(image *im)
{
    int i;
    for (i = 0; i < im->nsec; i++) {
        SEC_HDR *s = &im->sec[i];
        int prot = PROT_READ;
        if (s->Characteristics & 0x80000000u) prot |= PROT_WRITE;   /* WRITE */
        if (s->Characteristics & 0x20000000u) prot |= PROT_EXEC;    /* EXECUTE */
        if (mprotect(im->base + s->VirtualAddress,
                     (s->VirtualSize + 0xFFF) & ~0xFFFul, prot))
            perror("mprotect");
    }
    return 0;
}

/* ------------------------------------------------ real runtime libraries --- */

/* Some imports are far better served by a real implementation than by a stub.
 *
 * MSVCP120 is the MSVC 2013 C++ standard library, and unlike the C runtime it is
 * not a flat surface of functions: its objects have a layout the compiler baked
 * into the plugin. The plugin constructs a stream on its own stack and writes
 * vtable pointers straight into it at fixed offsets, so a hand-written stand-in
 * has to reproduce that layout exactly -- and a struct that is merely
 * functionally equivalent corrupts the object instead. A real DLL *is* the
 * layout, so loading one removes the whole hazard rather than managing it.
 *
 * Only redistributable runtimes are loaded this way: libraries a plugin would
 * have found already present on a Windows machine. Point PELOAD_DLL_PATH at a
 * directory holding them (colon-separated); Wine's copies and Microsoft's own
 * both work, and one is looked for beside the plugin too.
 */
#define MAX_REAL 6
static struct { char dll[64]; pe_module m; int tried, ok; } g_real[MAX_REAL];
static int g_nreal;

/* The C++ library, in any of its vintages. The C runtimes (msvcr*, ucrtbase,
 * vcruntime) stay implemented here: they have no object layout to get wrong. */
static int wants_real_image(const char *dll)
{ if (getenv("PELOAD_NO_REAL_MSVCP")) return 0;
  return strncasecmp(dll, "msvcp", 5) == 0; }

/* Try one directory, both as the import spelled the name and in lower case:
 * Windows does not distinguish them and the file on disk usually is lower case,
 * while an import table usually is not. */
static int try_dir(const char *dir, size_t dirlen, const char *dll,
                   char *out, size_t n)
{
    char lower[64];
    size_t i;

    if (!dirlen || dirlen > n - sizeof lower - 2) return 0;
    snprintf(out, n, "%.*s/%s", (int)dirlen, dir, dll);
    if (access(out, R_OK) == 0) return 1;
    for (i = 0; dll[i] && i < sizeof lower - 1; i++)
        lower[i] = (char)tolower((unsigned char)dll[i]);
    lower[i] = 0;
    if (strcmp(lower, dll) == 0) return 0;
    snprintf(out, n, "%.*s/%s", (int)dirlen, dir, lower);
    return access(out, R_OK) == 0;
}

static int find_real_dll(const char *dll, char *out, size_t n)
{
    /* Wine's own directories are deliberately not searched.
     *
     * They hold Wine's reimplementations, which are real PE files with real
     * code and load perfectly well -- and then fault inside their own startup,
     * because they are compiled against Wine's ntdll and expect the PEB, heap
     * and loader data it provides. Searching them does not produce a working
     * runtime; it converts "this plug-in loads with some imports stubbed" into
     * "this plug-in dies", which on a Debian box with wine installed took
     * the loader from 36 of 40 plug-ins to none of them. The runtime directories
     * below, and PELOAD_DLL_PATH, are the supported places. */

    /* Width matters here and nothing was checking it. A real runtime DLL is an
     * ordinary PE and the loader will map whichever one it is handed, so the
     * two widths get separate directories rather than sharing `runtime/` and
     * hoping. peload32 looked only where the 64-bit runtime lives, found no
     * 32-bit msvcp120.dll -- there is no such thing in an x86_64-windows
     * directory -- and every one of MSVCP120's iostream and locale imports
     * fell through to the generic stub. On i386 that stub returns 0 and pops
     * nothing, so the stack drifted four bytes per argument until a later ret
     * jumped into nothing: all four Native Instruments plugins died of it with
     * SIGSEGV on a null or near-null address, while the same four loaded on
     * x86-64. */
#ifdef __i386__
    static const char *const fallback[] = {
        "/usr/lib/vst-ace/runtime32",
        NULL
    };
    static const char *const runtime_dirs[] = {
        "/runtime32", "/../runtime32", "/../../runtime32", "", NULL
    };
#else
    static const char *const fallback[] = {
        "/usr/lib/vst-ace/runtime",
        NULL
    };
    static const char *const runtime_dirs[] = {
        "/runtime", "/../runtime", "/../../runtime", "", NULL
    };
#endif
    const char *env = getenv("PELOAD_DLL_PATH");
    int i;

    /* Given directories first, so a deliberate choice always wins. */
    if (env && *env) {
        const char *p = env;
        while (*p) {
            const char *e = strchr(p, ':');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (try_dir(p, len, dll, out, n)) return 1;
            if (!e) break;
            p = e + 1;
        }
    }
    if (g_image_dir[0] && try_dir(g_image_dir, strlen(g_image_dir), dll, out, n))
        return 1;

    /* A `runtime/` directory shipped alongside the host, found relative to the
     * running executable rather than the working directory.
     *
     * Without this the runtime had to be named in PELOAD_DLL_PATH on every
     * launch, which pestudio does not do -- so a plugin needing a real runtime
     * loaded from the command line and failed from the GUI, with the difference
     * invisible to whoever pressed the button. The host knows where it is
     * installed; it should look there. */
    {
        char exe[1024];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (len > 0) {
            const char *const *rel = runtime_dirs;
            char *slash;
            exe[len] = 0;
            if ((slash = strrchr(exe, '/')) != NULL) {
                *slash = 0;                       /* the directory holding it */
                for (i = 0; rel[i]; i++) {
                    char dir[1200];
                    snprintf(dir, sizeof dir, "%s%s", exe, rel[i]);
                    if (try_dir(dir, strlen(dir), dll, out, n)) return 1;
                }
            }
        }
    }

    for (i = 0; fallback[i]; i++)
        if (try_dir(fallback[i], strlen(fallback[i]), dll, out, n)) return 1;
    return 0;
}

/* The mapped module for `dll`, loading it the first time it is asked for.
 * Failure is remembered so a missing runtime is reported once, not per symbol. */
static pe_module *real_module(const char *dll)
{
    char path[1024], err[256] = "";
    int i;

    for (i = 0; i < g_nreal; i++)
        if (!strcasecmp(g_real[i].dll, dll))
            return g_real[i].ok ? &g_real[i].m : NULL;
    if (g_nreal >= MAX_REAL) return NULL;

    i = g_nreal++;
    snprintf(g_real[i].dll, sizeof g_real[i].dll, "%s", dll);
    /* Marked as attempted before loading: this runs from inside import
     * resolution, and the module's own imports are resolved the same way. */
    g_real[i].tried = 1;

    if (!find_real_dll(dll, path, sizeof path)) {
        fprintf(stderr, "peload: no %s on this machine -- using the built-in "
                        "implementation. For the real one, put a copy beside "
                        "the plug-in or name its directory in "
                        "PELOAD_DLL_PATH.\n", dll);
        return NULL;
    }
    {   /* pe_module_load installs whatever it loads as the primary image, which
         * is right when it *is* the plugin (the VST3 path) and wrong here: this
         * runs from inside the plugin's own import resolution. Leaving the
         * runtime in place made the plugin's resources unreachable. */
        void *pbase, *prsrc;
        char ppath[1024];
        int rc;
        winstubs_primary_save(&pbase, &prsrc);
        winstubs_image_path_save(ppath, sizeof ppath);
        rc = pe_module_load(path, &g_real[i].m, err, sizeof err);
        winstubs_primary_restore(pbase, prsrc);
        winstubs_image_path_restore(ppath);
        if (rc != 0) {
            fprintf(stderr, "peload: %s at %s would not load: %s\n", dll, path, err);
            return NULL;
        }
    }
    g_real[i].ok = 1;
    if (pe_verbose())
        fprintf(stderr, "  [real] %s <- %s\n", dll, path);
    return &g_real[i].m;
}

/* ------------------------------------------------ LoadLibrary, for real DLLs --- */

/* A plug-in that ships part of itself as separate DLLs and loads them at run
 * time. SynthEdit does exactly this: its modules are .sem files beside the
 * plug-in, ordinary PE DLLs under another extension, and it calls LoadLibrary
 * on each one by full path.
 *
 * Kept in a table so a module asked for twice is mapped once and keeps the same
 * handle, which is what a caller comparing handles expects -- and what stops a
 * graph with forty references to one module mapping it forty times.
 *
 * The primary image is saved across the load for the same reason real_module
 * does it: pe_module_load installs whatever it maps as the primary image, and a
 * module loaded on the plug-in's behalf must not become the answer to "where am
 * I" or take over the plug-in's resources. */
#define MAX_LOADED 64
static struct { char path[1024]; pe_module m; } g_loaded[MAX_LOADED];
static int g_nloaded;

static void *pehost_load_dll(const char *path)
{
    char err[256] = "", ppath[1024];
    void *pbase, *prsrc;
    int i, rc;

    for (i = 0; i < g_nloaded; i++)
        if (!strcmp(g_loaded[i].path, path)) return g_loaded[i].m.base;
    if (g_nloaded >= MAX_LOADED) return NULL;

    winstubs_primary_save(&pbase, &prsrc);
    winstubs_image_path_save(ppath, sizeof ppath);
    rc = pe_module_load(path, &g_loaded[g_nloaded].m, err, sizeof err);
    winstubs_primary_restore(pbase, prsrc);
    winstubs_image_path_restore(ppath);

    if (rc != 0) {
        if (pe_verbose())
            fprintf(stderr, "  [win] %s would not load: %s\n", path, err);
        return NULL;
    }
    snprintf(g_loaded[g_nloaded].path, sizeof g_loaded[g_nloaded].path, "%s", path);
    if (pe_verbose())
        fprintf(stderr, "  [win] loaded %s\n", path);
    return g_loaded[g_nloaded++].m.base;
}

static void *pehost_dll_symbol(void *module, const char *name)
{
    int i;
    for (i = 0; i < g_nloaded; i++)
        if (g_loaded[i].m.base == module)
            return pe_module_export(&g_loaded[i].m, name);
    return NULL;
}

/* Names the C++ runtime a plug-in needed and could not have, for the caller to
 * report. Empty when nothing was missing. */
static char g_missing_real[64];

static int resolve_imports(image *im)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_IMPORT];
    IMP_DESC *desc;
    int resolved = 0, stubbed = 0;

    g_missing_real[0] = 0;

    if (!d.VirtualAddress) return 0;
    for (desc = rva(im, d.VirtualAddress); desc->Name; desc++) {
        const char *dll = rva(im, desc->Name);
        uint64_t *lookup = rva(im, desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                            : desc->FirstThunk);
        uint64_t *iat = rva(im, desc->FirstThunk);
        for (; *lookup; lookup++, iat++) {
            const char *sym;
            void *fn;
            char ordbuf[32];

            if (*lookup & (1ull << 63)) {
                snprintf(ordbuf, sizeof ordbuf, "ordinal#%llu",
                         (unsigned long long)(*lookup & 0xFFFF));
                sym = ordbuf;
            } else {
                sym = (const char *)rva(im, (*lookup & 0x7FFFFFFF)) + 2;
            }

            /* A real implementation beats anything here, so it is asked first.
             * Ordinal imports are skipped: the C++ library is bound by name. */
            fn = NULL;
            if (wants_real_image(dll) && !(*lookup & (1ull << 63))) {
                pe_module *rm = real_module(dll);
                if (rm) fn = pe_module_export(rm, sym);
            }
            if (!fn) fn = winstub_lookup(dll, sym);
            /* Refusing the load here used to be right: with none of the C++
             * library implemented, a plug-in resolved several hundred stubs and
             * faulted on the first constructor, and naming the missing DLL was
             * the only actionable thing to say.
             *
             * msvcp_shim.h implements the part of it a plug-in actually starts
             * on -- locale, the facet base, _Lockit, the mutex and condition
             * primitives -- so a machine with no Microsoft runtime is no longer
             * automatically a failed load. What is left unimplemented is
             * imported far more often than it is called: this plug-in imports
             * 145 entry points from MSVCP140 and reaches six. Each of those is
             * a tracking stub that names itself on first call, which is a
             * better diagnostic than a refusal, because it reports what was
             * wanted rather than what was absent. */
            if (fn) { resolved++; }
            else {
                if (g_nimp < MAX_IMPORTS) {
                    snprintf(g_imp[g_nimp].dll, sizeof g_imp[0].dll, "%s", dll);
                    snprintf(g_imp[g_nimp].sym, sizeof g_imp[0].sym, "%s", sym);
                    fn = make_stub((uint32_t)g_nimp);
                    g_nimp++;
                } else fn = make_stub(0);
                stubbed++;
            }
            *iat = (uint64_t)(uintptr_t)fn;
        }
    }
    g_nresolved = resolved;
    if (pe_verbose())
        fprintf(stderr, "imports: %d implemented, %d stubbed\n", resolved, stubbed);
    return g_missing_real[0] ? -1 : 0;
}

static int setup_tls(image *im)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_TLS];
    TLS_DIR64 *t;
    uint64_t *cb;

    if (!d.VirtualAddress) return 0;
    t = rva(im, d.VirtualAddress);

    g_tls_tmpl  = (const uint8_t *)(uintptr_t)t->StartAddressOfRawData;
    g_tls_raw   = (size_t)(t->EndAddressOfRawData - t->StartAddressOfRawData);
    g_tls_total = g_tls_raw + t->SizeOfZeroFill;
    g_tls_index = 0;                       /* single module, so slot 0 is ours */
    *(uint32_t *)(uintptr_t)t->AddressOfIndex = g_tls_index;

    tls_bind_current_thread();
    if (pe_verbose())
        fprintf(stderr, "tls: %zu bytes (%zu raw), slot %u\n",
                g_tls_total, g_tls_raw, g_tls_index);

    if (t->AddressOfCallBacks) {
        int n = 0;
        for (cb = (uint64_t *)(uintptr_t)t->AddressOfCallBacks; *cb; cb++) {
            MS void (*f)(void *, uint32_t, void *) =
                (MS void (*)(void *, uint32_t, void *))(uintptr_t)*cb;
            f(im->base, 1 /* DLL_PROCESS_ATTACH */, NULL);
            n++;
        }
        if (pe_verbose()) fprintf(stderr, "tls: ran %d callback(s)\n", n);
    }
    return 0;
}

static void *find_export(image *im, const char *want)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_EXPORT];
    EXP_DIR *e;
    uint32_t *names, *funcs;
    uint16_t *ords;
    uint32_t i;

    if (!d.VirtualAddress) return NULL;
    e = rva(im, d.VirtualAddress);
    names = rva(im, e->AddressOfNames);
    funcs = rva(im, e->AddressOfFunctions);
    ords  = rva(im, e->AddressOfNameOrdinals);
    for (i = 0; i < e->NumberOfNames; i++) {
        const char *nm = rva(im, names[i]);
        if (!strcmp(nm, want)) return rva(im, funcs[ords[i]]);
    }
    return NULL;
}

static void list_exports(image *im)
{
    DATA_DIR d = im->opt->DataDirectory[DIR_EXPORT];
    EXP_DIR *e;
    uint32_t *names, i;
    if (!d.VirtualAddress) { fprintf(stderr, "no exports\n"); return; }
    e = rva(im, d.VirtualAddress);
    names = rva(im, e->AddressOfNames);
    fprintf(stderr, "exports (%u):", e->NumberOfNames);
    for (i = 0; i < e->NumberOfNames; i++)
        fprintf(stderr, " %s", (const char *)rva(im, names[i]));
    fprintf(stderr, "\n");
}


/* Resolve a .vst3 bundle to the binary inside it, or pass a plain file through. */
static int pehost_resolve(const char *path, char *out, size_t n)
{
    struct stat st;
    static const char *arch[] = { "x86_64-linux", "x86_64-win", "x86-win", NULL };
    int i;

    if (stat(path, &st)) return -1;
    if (S_ISREG(st.st_mode)) { snprintf(out, n, "%s", path); return 0; }
    for (i = 0; arch[i]; i++) {
        char dir[1024];
        DIR *d;
        struct dirent *e;
        snprintf(dir, sizeof dir, "%s/Contents/%s", path, arch[i]);
        if (!(d = opendir(dir))) continue;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 3 && (!strcmp(e->d_name + l - 3, ".so") ||
                          (l > 4 && !strcasecmp(e->d_name + l - 4, ".dll")) ||
                          (l > 5 && !strcmp(e->d_name + l - 5, ".vst3")))) {
                snprintf(out, n, "%s/%s", dir, e->d_name);
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }
    return -1;
}

/* i386 images are loadable when a peload32 helper is available to run them. */
static int pehost_is_i386(const char *path)
{
    char bin[1024];
    unsigned char hdr[2];
    FILE *f;
    uint32_t lfanew;
    uint16_t machine;
    int r = 0;

    if (!path || pehost_resolve(path, bin, sizeof bin)) return 0;
    if (!(f = fopen(bin, "rb"))) return 0;
    if (fread(hdr, 1, 2, f) == 2 && hdr[0] == 'M' && hdr[1] == 'Z' &&
        !fseek(f, 0x3c, SEEK_SET) && fread(&lfanew, 4, 1, f) == 1 &&
        !fseek(f, (long)lfanew + 4, SEEK_SET) && fread(&machine, 2, 1, f) == 1)
        r = (machine == 0x14c);
    fclose(f);
    return r;
}

/* A macOS bundle: a directory with Contents/MacOS holding a Mach-O, or a bare
 * Mach-O file. Returns 1 for a VST2 (.vst), 2 for an Audio Unit (.component). */
/* Name a Mac binary format this loader does not run in-process, or NULL if it is
 * not one.
 *
 * Worth telling apart rather than lumping together as "unrecognised", because
 * each implies something different about whether it is worth pursuing. A plug-in
 * for Classic Mac OS is not merely an old Mach-O -- it is a different executable
 * format (PEF, the Code Fragment Manager's) for a different processor. That one
 * is interpreted rather than run: see pefload.c and ppc.c. Mac OS X on PowerPC
 * is a third case, right format and wrong instruction set, and is not handled.
 */
static const char *classic_mac_format(const unsigned char *h, size_t n)
{
    uint32_t be;

    if (n < 8) return NULL;
    /* PEF: the Code Fragment Manager container used by Mac OS 8/9 and by Carbon
     * on Mac OS X. The tag is literally "Joy!peff". */
    if (!memcmp(h, "Joy!peff", 8))
        return "Classic Mac OS / Carbon (CFM/PEF, PowerPC) -- interpreted, "
               "not loaded in-process";
    /* StuffIt: how Classic-era plug-ins were nearly always shipped. */
    if (n >= 6 && (!memcmp(h, "StuffIt", 7) || !memcmp(h, "SIT!", 4)))
        return "StuffIt archive (Classic Mac OS) -- unpack it first";
    /* AppleSingle / AppleDouble: a file with its resource fork wrapped up, which
     * is what a Classic plug-in looks like once it has left a Mac filesystem. */
    be = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    if (be == 0x00051600u || be == 0x00051607u)
        return "Classic Mac OS resource fork (AppleSingle/Double)";
    /* Mach-O for PowerPC: Mac OS X, but not this instruction set. Big-endian
     * magic distinguishes it from the little-endian Intel ones. */
    if (be == 0xFEEDFACEu || be == 0xFEEDFACFu) {
        uint32_t cpu = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) |
                       ((uint32_t)h[6] << 8) | h[7];
        if (cpu == 18 || cpu == 0x01000012u)
            return "Mac OS X for PowerPC";
        return "big-endian Mach-O, so PowerPC";
    }
    return NULL;
}

/* Why this file could not be mapped, in terms of what it actually is. Falls back
 * to the plain message when the format is one we would normally accept. */
static void explain_unmappable(const char *path, char *out, size_t n)
{
    unsigned char hdr[16];
    const char *mac = NULL;
    FILE *f = fopen(path, "rb");
    size_t got = 0;

    if (f) { got = fread(hdr, 1, sizeof hdr, f); fclose(f); }
    if (got >= 8) mac = classic_mac_format(hdr, got);
    if (mac) snprintf(out, n, "%s", mac);
    else     snprintf(out, n, "cannot map %s", path);
}

/* A native Linux VST2: a plain ELF shared object exporting VSTPluginMain (or the
 * pre-2.4 `main`). Checked by looking at the export, not the file name, because
 * these ship as bare .so files with no convention worth trusting -- and because a
 * VST3's inner .so must not match, which is why the VST3 test runs first. */
/* A plugin the dynamic linker loads directly -- a native Linux VST2 or VST3.
 * These are the ones that must stay in this process even when isolation is on,
 * because their editors are X11 windows embedded into one of ours and the bridge
 * carries pixels, not window ids. */
static int pehost_is_native_elf(const char *path)
{
    char bin[1024];
    unsigned char hdr[5];
    FILE *f;
    int ok;

    if (!path || pehost_resolve(path, bin, sizeof bin)) return 0;
    if (!(f = fopen(bin, "rb"))) return 0;
    ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr;
    fclose(f);
    return ok && !memcmp(hdr, "\177ELF", 4);
}

/* Does this ELF define a VST2 entry point? Read out of the dynamic symbol
 * table, without loading anything.
 *
 * Returns 1 yes, 0 no, -1 "cannot tell from the tables" -- a stripped object
 * with no usable .dynsym, which the caller settles the old way.
 *
 * Only defined symbols count: every plugin here imports something called
 * `main` from libc's startup glue, and an undefined entry would match every
 * shared object in the tree. */
static int elf_defines_vst_entry(const char *path)
{
    int          fd, verdict = -1;
    struct stat  st;
    unsigned char *m = MAP_FAILED;
    Elf64_Ehdr  *eh;
    Elf64_Shdr  *sh;
    unsigned     i;

    if ((fd = open(path, O_RDONLY)) < 0) return -1;
    if (fstat(fd, &st) || (size_t)st.st_size < sizeof *eh) { close(fd); return -1; }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;

    /* Every bound below is checked against the file size before it is used: a
     * truncated or hand-edited .so is the realistic case here, and this runs
     * over every shared object in a scanned tree. */
    eh = (Elf64_Ehdr *)m;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64)
        goto out;
    if (!eh->e_shoff || eh->e_shentsize != sizeof *sh || !eh->e_shnum) goto out;
    if (eh->e_shoff > (uint64_t)st.st_size ||
        (uint64_t)eh->e_shnum * sizeof *sh > (uint64_t)st.st_size - eh->e_shoff)
        goto out;
    sh = (Elf64_Shdr *)(m + eh->e_shoff);

    for (i = 0; i < eh->e_shnum; i++) {
        Elf64_Sym  *sym;
        const char *str;
        uint64_t    n, j, strsz;

        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_entsize != sizeof *sym) continue;
        if (sh[i].sh_link >= eh->e_shnum) continue;
        if (sh[i].sh_offset > (uint64_t)st.st_size ||
            sh[i].sh_size > (uint64_t)st.st_size - sh[i].sh_offset) continue;
        strsz = sh[sh[i].sh_link].sh_size;
        if (sh[sh[i].sh_link].sh_offset > (uint64_t)st.st_size ||
            strsz > (uint64_t)st.st_size - sh[sh[i].sh_link].sh_offset) continue;

        str = (const char *)(m + sh[sh[i].sh_link].sh_offset);
        sym = (Elf64_Sym *)(m + sh[i].sh_offset);
        n   = sh[i].sh_size / sizeof *sym;
        verdict = 0;                       /* a table was read; 0 means "not here" */
        for (j = 0; j < n; j++) {
            const char *nm;
            uint64_t    off = sym[j].st_name;
            if (!off || off >= strsz || sym[j].st_shndx == SHN_UNDEF) continue;
            nm = str + off;
            /* strnlen against the section end: a name running off the end of a
             * truncated string table would otherwise be read past the mapping. */
            if (strnlen(nm, (size_t)(strsz - off)) == (size_t)(strsz - off)) continue;
            if (!strcmp(nm, "VSTPluginMain") || !strcmp(nm, "main")) {
                verdict = 1;
                break;
            }
        }
        if (verdict == 1) break;
    }
out:
    munmap(m, (size_t)st.st_size);
    return verdict;
}

int pehost_is_native_vst2(const char *path)
{
    unsigned char hdr[20];
    FILE *f;
    void *dl, *sym;
    size_t l;
    int   quick;

    if (!path) return 0;
    l = strlen(path);
    if (l < 3 || strcmp(path + l - 3, ".so")) return 0;
    if (!(f = fopen(path, "rb"))) return 0;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return 0; }
    fclose(f);
    if (memcmp(hdr, "\177ELF", 4) || hdr[4] != 2) return 0;   /* ELF64 only */

    /* Ask the symbol table first, because the alternative below answers by
     * *loading the plugin*: dlopen maps the image, resolves it and runs its ELF
     * constructors, and the handle is deliberately never closed. This test is
     * what a plugin scanner runs over every .so in a tree, so a directory
     * listing was executing every plugin's initialisation code and leaving them
     * all resident -- 39 plugins and 66 MB of them here, before anything had
     * been clicked. Reading the table touches two sections and runs nothing:
     * the same answer on all 39, in 1 ms rather than 29, for 1 MB rather than
     * 66.
     *
     * A plugin that exports the entry point but cannot actually be loaded -- a
     * missing dependency -- now reaches the list and fails when it is picked,
     * with the linker's reason. That is the trade the scanner already makes
     * everywhere else: absent from the list looks like a scan that missed it,
     * and "why will it not load" is a better question than "why is it not
     * there". */
    if ((quick = elf_defines_vst_entry(path)) >= 0) return quick;

    /* No readable .dynsym -- stripped, or a layout this does not understand.
     * Fall back to asking the linker, which is authoritative. */
    if (!(dl = dlopen(path, RTLD_LAZY | RTLD_LOCAL))) return 0;
    sym = dlsym(dl, "VSTPluginMain");
    if (!sym) sym = dlsym(dl, "main");
    /* Left open: if this answers yes it is about to be loaded for real, and
     * several of these crash inside their own teardown when dlclose runs it. */
    return sym != NULL;
}

static int pehost_macho_kind(const char *path)
{
    char p[4096];
    struct stat st;
    size_t l;
    unsigned char hdr[4];
    FILE *f;
    DIR *d;
    int found = 0;

    if (!path || stat(path, &st)) return 0;
    l = strlen(path);
    if (S_ISDIR(st.st_mode)) {
        snprintf(p, sizeof p, "%s/Contents/MacOS", path);
        if (stat(p, &st) || !S_ISDIR(st.st_mode)) return 0;
        if (!(d = opendir(p))) return 0;
        { struct dirent *e;
          while ((e = readdir(d))) if (e->d_name[0] != '.') { found = 1; break; } }
        closedir(d);
        if (!found) return 0;
        if (l > 10 && !strcasecmp(path + l - 10, ".component")) return 2;
        if (l > 4  && !strcasecmp(path + l - 4,  ".vst"))       return 1;
        return 0;                       /* a .vst3 bundle is not handled here */
    }
    if (!(f = fopen(path, "rb"))) return 0;
    found = (fread(hdr, 1, 4, f) == 4);
    fclose(f);
    if (!found) return 0;
    /* MH_MAGIC_64, or a universal binary (its header is big-endian). */
    if (hdr[0] == 0xCF && hdr[1] == 0xFA && hdr[2] == 0xED && hdr[3] == 0xFE) return 1;
    if (hdr[0] == 0xCA && hdr[1] == 0xFE && hdr[2] == 0xBA && hdr[3] == 0xBE) return 1;
    return 0;
}

/* ---- data a plugin needs but does not carry ---------------------------- */

static int dir_exists(const char *p)
{ struct stat st; return p && *p && !stat(p, &st) && S_ISDIR(st.st_mode); }

/* Does this directory hold anything with one of these extensions? */
static int dir_has_ext(const char *dir, const char *const *exts)
{
    DIR           *d = opendir(dir);
    struct dirent *e;
    int            found = 0;

    if (!d) return 0;
    while (!found && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        int    i;
        for (i = 0; exts[i] && !found; i++) {
            size_t el = strlen(exts[i]);
            if (l > el && !strcasecmp(e->d_name + l - el, exts[i])) found = 1;
        }
    }
    closedir(d);
    return found;
}

/* The directory holding the plugin, and the name its vendor keys folders by --
 * which is the containing directory's name, not the file's: u-he ships
 * "Zebralette3/Zebralette3.64.so" and a .vst3 bundle is itself the name. */
static void plugin_dir_and_product(const char *path, char *dir, size_t dirn,
                                   char *product, size_t prodn)
{
    const char *slash;
    char        tmp[1024];
    size_t      l;

    dir[0] = product[0] = 0;
    snprintf(tmp, sizeof tmp, "%s", path);
    l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;

    if (l > 5 && !strcasecmp(tmp + l - 5, ".vst3")) {
        /* A bundle names itself; the product is the bundle without its suffix. */
        snprintf(dir, dirn, "%s", tmp);
        slash = strrchr(tmp, '/');
        snprintf(product, prodn, "%.*s",
                 (int)(l - (slash ? (size_t)(slash - tmp) + 1 : 0) - 5),
                 slash ? slash + 1 : tmp);
        return;
    }
    if (!(slash = strrchr(tmp, '/'))) return;
    snprintf(dir, dirn, "%.*s", (int)(slash - tmp), tmp);
    if ((slash = strrchr(dir, '/'))) snprintf(product, prodn, "%s", slash + 1);
}

/* Walk `from` against `to`, entry by entry. Anything `to` already has is left
 * exactly as it is; anything it lacks is counted, and linked when `link` is set.
 *
 * Recursive, because the missing part is usually inside a folder that does
 * exist. u-he leaves an empty Locale/ behind and a Scripts/ holding only the
 * tag database the plugin writes itself, so a pass that only looked for folders
 * that were absent altogether called both of those complete and the plugin went
 * on failing to find en.uhe-locale and EditorSetup.txt. A symlink on the `to`
 * side is skipped rather than followed: it is already pointing at the release.
 *
 * `names` collects the first few top-level names for a status line. */
static int data_walk(const char *from, const char *to, int link, int depth,
                     char *names, size_t namesn, int *nnames)
{
    DIR           *d;
    struct dirent *e;
    int            missing = 0;

    if (depth > 6 || !(d = opendir(from))) return 0;
    while ((e = readdir(d))) {
        char        src[2048], dst[2048];
        struct stat ls, ss;

        if (e->d_name[0] == '.') continue;
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", to,   e->d_name);
        if (stat(src, &ss)) continue;

        if (lstat(dst, &ls) != 0) {                      /* nothing there yet */
            missing++;
            if (names && nnames) {
                if (*nnames < 4)
                    snprintf(names + strlen(names), namesn - strlen(names),
                             "%s%s", *nnames ? ", " : "", e->d_name);
                (*nnames)++;
            }
            /* A link rather than a copy, so the data has one home and stays in
             * step with the release it came from. A moved corpus leaves a
             * dangling link, which is at least visible; a stale copy is not. */
            if (link && symlink(src, dst) != 0) missing--;
            continue;
        }
        if (S_ISLNK(ls.st_mode)) continue;               /* already provided */
        if (S_ISDIR(ss.st_mode) && S_ISDIR(ls.st_mode))
            missing += data_walk(src, dst, link, depth + 1, names, namesn, nnames);
    }
    closedir(d);
    return missing;
}

int pehost_data_check(const char *path, pehost_data_need *out)
{
    pehost_data_need scratch;
    const char      *home = getenv("HOME");
    char             dir[1024], product[64], p[1024], q[1024];
    static const char *const rom_exts[] = { ".bin", ".mid", NULL };

    if (!out) out = &scratch;
    memset(out, 0, sizeof *out);
    if (!path || !*path || !home || !*home) return 0;

    plugin_dir_and_product(path, dir, sizeof dir, product, sizeof product);
    if (!product[0]) return 0;
    snprintf(out->product, sizeof out->product, "%s", product);

    /* A firmware ROM. The emulation creates its own roms/ folder the first time
     * it runs and then refuses to make a sound until something is in it, so the
     * folder's existence is the signal that this is such a plugin and its
     * emptiness is the problem. Nothing can be linked in: the firmware belongs
     * to the hardware it came from and is not in the download. */
    snprintf(p, sizeof p, "%s/.local/share/The Usual Suspects/%s/roms", home, product);
    if (dir_exists(p) && !dir_has_ext(p, rom_exts)) {
        snprintf(out->need, sizeof out->need,
                 "no firmware ROM -- it will load but stay silent until one "
                 "512k .bin, or a set of .mid files, is put here");
        snprintf(out->where, sizeof out->where, "%s", p);
        return 1;
    }

    /* GUI resources. u-he's installer copies the release's Data folder into
     * ~/.u-he/<Product>/; unpacked rather than installed, the plugin finds the
     * folder its presets live in but no Images or Fonts, and draws an editor
     * with no artwork on it. Both halves have to be there for this to be the
     * situation -- the release's Data to copy from, and the user folder that
     * says the plugin looks there at all. */
    snprintf(p, sizeof p, "%s/.u-he/%s/Data", home, product);
    snprintf(q, sizeof q, "%s/Data", dir);
    if (dir_exists(p) && dir_exists(q)) {
        char list[96];
        int  nn = 0;

        list[0] = 0;
        if (data_walk(q, p, 0, 0, list, sizeof list, &nn)) {
            snprintf(out->need, sizeof out->need,
                     "installed data incomplete (%s%s) -- its editor draws "
                     "without artwork", list, nn > 4 ? ", ..." : "");
            snprintf(out->where, sizeof out->where, "%s", p);
            snprintf(out->from,  sizeof out->from,  "%s", q);
            out->repairable = 1;
            return 1;
        }
    }
    return 0;
}

int pehost_data_repair(const pehost_data_need *need, char *err, int errn)
{
    struct stat st;
    int         n;

    if (err && errn) err[0] = 0;
    if (!need || !need->repairable || !need->from[0] || !need->where[0]) {
        if (err) snprintf(err, (size_t)errn, "nothing to link");
        return -1;
    }
    if (stat(need->from, &st) || !S_ISDIR(st.st_mode)) {
        if (err) snprintf(err, (size_t)errn, "cannot read %s", need->from);
        return -1;
    }
    /* Never over the top of something already there: what the installer or the
     * user put in place wins over what a release tree happens to hold. That is
     * data_walk's rule, not a special case here. */
    n = data_walk(need->from, need->where, 1, 0, NULL, 0, NULL);
    if (!n && err) snprintf(err, (size_t)errn, "nothing was linked");
    return n ? n : -1;
}

/* Isolation: run a plugin in a helper even though this process could load it.
 * Off by default, because in-process costs nothing and keeps the editor path
 * simple. Worth turning on when browsing plugins that may fault -- three in this
 * corpus do, and in-process a fault takes the host with it. */
static int g_isolate = -1;

void pehost_set_isolation(int on) { g_isolate = on ? 1 : 0; }

int pehost_isolation(void)
{
    if (g_isolate < 0) {
        const char *e = getenv("PEHOST_ISOLATE");
        g_isolate = (e && *e != '0') ? 1 : 0;
    }
    return g_isolate && bridge_isolation_available();
}

int pehost_is_bridged(const char *path)
{ return pehost_is_i386(path) && bridge_available(); }

int pehost_can_load(const char *path, char *why, int whyn)
{
    char bin[1024];
    int mk;
    unsigned char hdr[16];
    FILE *f;
    uint32_t lfanew;
    uint16_t machine;

    if (why && whyn) why[0] = 0;
    if ((mk = pehost_macho_kind(path))) return 1;      /* macOS bundle */
    /* Classic Mac OS. Checked before pehost_resolve, which looks for a native
     * binary and would not know what to make of a resource fork. */
    if (pehost_is_classic_mac(path)) return 1;
    if (!path || pehost_resolve(path, bin, sizeof bin)) {
        if (why) snprintf(why, (size_t)whyn, "no binary inside");
        return 0;
    }
    if (!(f = fopen(bin, "rb"))) {
        if (why) snprintf(why, (size_t)whyn, "unreadable");
        return 0;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return 0; }
    if (hdr[0] == 0x7F && hdr[1] == 'E') { fclose(f); return 1; }   /* ELF */
    if (hdr[0] != 'M' || hdr[1] != 'Z') {
        const char *mac = classic_mac_format(hdr, sizeof hdr);
        fclose(f);
        if (why) snprintf(why, (size_t)whyn, "%s",
                          mac ? mac : "not a PE, ELF or Mach-O image");
        return 0;
    }
    if (fseek(f, 0x3c, SEEK_SET) || fread(&lfanew, 4, 1, f) != 1 ||
        fseek(f, (long)lfanew + 4, SEEK_SET) || fread(&machine, 2, 1, f) != 1) {
        fclose(f);
        if (why) snprintf(why, (size_t)whyn, "truncated PE header");
        return 0;
    }
    fclose(f);
    if (machine == 0x8664) return 1;
    if (machine == 0x14c) {
        if (bridge_available()) return 1;               /* runs in peload32 */
        /* Say which of the two it is. "No helper" and "the helper is here but
         * its 32-bit libraries are not" want completely different actions from
         * whoever is reading, and the second is the common one now that
         * peload32 ships in the same package as everything else. */
        if (why) {
            const char *r = bridge_unavailable_reason();
            snprintf(why, (size_t)whyn, "32-bit x86 -- %s",
                     r ? r : "no peload32 helper");
        }
        return 0;
    }
    if (why) snprintf(why, (size_t)whyn, "machine 0x%x", machine);
    return 0;
}

/* ------------------------------------------------ PE loading for other APIs */

/* A Windows VST3 is the same kind of image as a VST2 DLL; only the interface on
 * top differs. Everything here is the VST2 path's own setup sequence. */
static double pe_now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int pe_module_load(const char *path, pe_module *m, char *err, int errlen)
{
    /* A local, and it has to be: this function re-enters itself. Resolving the
     * imports side-loads a real msvcp* through real_module, which maps that DLL
     * by calling straight back in here -- so while the image struct was a
     * static, the inner load overwrote the outer one's. Everything after the
     * import pass then ran against the runtime rather than the plug-in: its
     * sections re-protected, its DllMain run a second time, and the caller
     * handed the runtime's base to look up GetPluginFactory in.
     *
     * winstubs_primary_save/restore in real_module guards the other piece of
     * shared state across the same nesting. */
    image im;
    void *entry;
    double t0 = pe_now(), tMap, tRel, tImp, tTls;

    memset(&im, 0, sizeof im);
    if (map_image(&im, path)) {
        explain_unmappable(path, err, (size_t)errlen);
        return -1;
    }
    tMap = pe_now();
    if (apply_relocs(&im) < 0) { snprintf(err, errlen, "relocation failed"); goto fail; }
    tRel = pe_now();

    winstubs_init(im.base,
                  im.opt->DataDirectory[DIR_RESOURCE].VirtualAddress
                    ? im.base + im.opt->DataDirectory[DIR_RESOURCE].VirtualAddress
                    : NULL);
    if (resolve_imports(&im) < 0) {
        snprintf(err, errlen, "%s is needed and was not found -- put a copy "
                              "beside the plug-in, or name its directory in "
                              "PELOAD_DLL_PATH", g_missing_real);
        goto fail;
    }
    protect_sections(&im);
    tImp = pe_now();
    setup_tls(&im);
    tTls = pe_now();

    entry = im.opt->AddressOfEntryPoint ? rva(&im, im.opt->AddressOfEntryPoint) : NULL;
    if (entry) {
        MS int32_t (*dllmain)(void *, uint32_t, void *) =
            (MS int32_t (*)(void *, uint32_t, void *))entry;
        if (!dllmain(im.base, 1 /* DLL_PROCESS_ATTACH */, NULL)) {
            snprintf(err, errlen, "DllMain failed");
            goto fail;
        }
    }
    if (pe_verbose())
        fprintf(stderr, "pe: map %.0f ms, reloc %.0f ms, imports %.0f ms, tls %.0f ms, "
                        "DllMain %.0f ms\n",
                (tMap - t0) * 1e3, (tRel - tMap) * 1e3, (tImp - tRel) * 1e3,
                (tTls - tImp) * 1e3, (pe_now() - tTls) * 1e3);
    m->base = im.base;
    m->size = (unsigned long)im.size;
    return 0;

fail:
    /* The image is mapped by now, and nobody else has a handle on it. */
    munmap(im.base, im.size);
    return -1;
}

/* Give the image back. Each load maps SizeOfImage afresh -- around 20 MB for a
 * Full Bucket VST3 -- so without this, switching plugins grows the process
 * until it starts swapping. */
void pe_module_unload(pe_module *m)
{
    if (!m || !m->base) return;
    /* Out of the resource registry before it is out of the address space: an
     * entry naming an unmapped base is worse than no entry at all. Losing the
     * plug-in itself also frees the thread-local slots it was holding. */
    if (winstubs_drop_image(m->base)) winstubs_reset_tls();
    munmap(m->base, m->size);
    m->base = NULL;
    m->size = 0;
}

void *pe_module_export(const pe_module *m, const char *name)
{
    image im;
    DOS_HDR *dos;

    if (!m || !m->base) return NULL;
    memset(&im, 0, sizeof im);
    im.base = m->base;
    dos = (DOS_HDR *)m->base;
    im.fh  = (FILE_HDR *)(im.base + dos->e_lfanew);
    im.opt = (OPT_HDR64 *)((uint8_t *)im.fh + sizeof *im.fh);
    im.sec = (SEC_HDR *)((uint8_t *)im.opt + im.fh->SizeOfOptionalHeader);
    im.nsec = im.fh->NumberOfSections;
    return find_export(&im, name);
}

/* ========================================================== public API ==== */

#include "vst2.h"
#include "vst3.h"

/* Deep enough that no realistic burst can reach the bottom. A dropped event is
 * not a glitch that passes -- if it was a note-off, the note never stops -- so
 * the queue is sized to make dropping essentially impossible rather than merely
 * unlikely. At 24 bytes an entry this costs under 100 KB. */
#define EVQ 4096
enum { EV_MIDI = 1, EV_PARAM, EV_PROGRAM };
/* `t` is when the event arrived (monotonic ns), `at` an explicit frame offset
 * when the caller knows one (a sequencer does; a keyboard does not). Together
 * they are what turns a block-quantised event into a placed one. */
typedef struct {
    unsigned char type, a, b, c;
    float    v;
    uint64_t t;
    int32_t  at;          /* >= 0: use directly; < 0: derive from t */
} ev_t;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct pehost {
    int       is_v3;      /* VST3 goes through vst3.c, VST2 through here */
    unsigned  in_mask;    /* which input channels the fed signal reaches; 0 = all */
    /* A 32-bit plugin cannot run in this process at all, so it runs in a
     * peload32 helper and every call below forwards over the bridge. Callers
     * see no difference: that is the point of dispatching here rather than
     * making each of them width-aware. */
    bridge   *br;
    /* macOS plugins run in-process: their x86-64 ABI is System V, the same one
     * this host uses, so a Mach-O function pointer is directly callable. `mv` is
     * a mac VST2, `au` an Audio Unit. */
    macvst   *mv;
    macau    *au;
    /* An AU addresses parameters by identifier, not by index, so the list is
     * fetched once and indexed through. */
    unsigned *au_ids;
    int       au_nids;
    v3host   *v3;
    /* A Classic Mac OS plugin is PowerPC code, so it is interpreted rather than
     * called. `cl` owns the interpreter, the fragment and the system-library
     * shim; the file it came from stays mapped because the resource fork is read
     * from it lazily, for artwork. */
    pefvst   *cl;
    uint8_t  *cl_file;
    long      cl_filelen;
    /* De-interleaving buffers for the Classic path, owned per host rather than
     * shared: a static here would be silently common to every plugin a caller
     * had open, and the audio threads would overwrite each other's channels. */
    float    *cl_io;
    int       is_mac;     /* a Mach-O bundle, in process or behind the helper */
    image     im;
    AEffect  *fx;
    double    sr;
    int       bs;
    char      name[64], vendor[64];
    int       program;

    /* scratch buffers sized for the plugin's own channel counts. These are
     * allocated to the declared counts rather than a fixed maximum: a plugin
     * writes to every output it advertises, so handing it a shorter array than
     * numOutputs is an out-of-bounds write we would be asking for. */
    float   **in, **out;
    int       nin, nout, cap;

    /* lock-free SPSC queue: GUI thread produces, audio thread consumes */
    ev_t              evq[EVQ];
    _Atomic unsigned  head, tail;

    /* The event block handed to effProcessEvents, kept alive until the next one
     * replaces it -- see render_io_block. */
    VstEvents    *evblk;
    int           evblk_n;
    VstMidiEvent  evmidi[256];

    /* MIDI placement. `blk_t0` is when the previous block started; an event that
     * arrived during it is placed proportionally into this one, which preserves
     * the spacing between events even though the batch is a block late. */
    uint64_t blk_t0;
    unsigned ev_dropped;
    unsigned ev_spilled;
};

static char g_err[256];
const char *pehost_last_error(void) { return g_err; }

int pehost_thread_init(void)
{
    /* Installed here rather than at each entry point: every path into this
     * host goes through thread init first, and LoadLibrary has to be able to
     * reach the real loader from whichever one was used. */
    winstubs_set_loader(pehost_load_dll, pehost_dll_symbol);
    return g_teb ? 0 : teb_install();
}

/* The transport handed back from audioMasterGetTime. File scope because the
 * plugin keeps the pointer after the callback returns. */
static VstTimeInfo g_transport;
static double      g_play_pos;

/* The transport the plugin is told about. Global because audioMasterGetTime has
 * no handle to work from -- the callback is per-plugin but the clock is the
 * host's, and one plugin is loaded at a time. */
static double g_tempo   = 120.0;
static int    g_tsig_n  = 4, g_tsig_d = 4;
static int    g_playing = 1;

/* Tempo derived from MIDI clock: 24 ticks to the quarter note.
 *
 * Measured per tick rather than per quarter note. Averaging over a whole beat
 * meant a tempo change took a beat to appear, which is audible as a synced delay
 * lagging behind a ritardando; a per-tick estimate with a short filter converges
 * in a few ticks and still ignores the jitter that any clock carries.
 *
 * The filter is a median of the last five intervals followed by a light ease.
 * A median is what rejects a single late tick outright -- an average lets it
 * move the result, which is exactly the wobble this is meant to avoid. */
#define CLK_HIST 5
static uint64_t g_clk_last;
static double   g_clk_iv[CLK_HIST];
static int      g_clk_n;

static void clock_tick(uint64_t now)
{
    double iv, sorted[CLK_HIST], med, bpm;
    int i, j, n;

    if (!g_clk_last) { g_clk_last = now; return; }
    iv = (double)(now - g_clk_last) / 1e9;
    g_clk_last = now;
    /* 15..3000 BPM at 24 ticks a beat; anything outside is not a clock. */
    if (iv < 60.0 / 3000.0 / 24.0 || iv > 60.0 / 15.0 / 24.0) return;

    for (i = CLK_HIST - 1; i > 0; i--) g_clk_iv[i] = g_clk_iv[i - 1];
    g_clk_iv[0] = iv;
    if (g_clk_n < CLK_HIST) g_clk_n++;

    n = g_clk_n;
    for (i = 0; i < n; i++) sorted[i] = g_clk_iv[i];
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (sorted[j] < sorted[i]) { double t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    med = sorted[n / 2];
    if (med <= 0.0) return;
    bpm = 60.0 / (med * 24.0);
    /* Snap when the difference is large -- that is a deliberate tempo change, not
     * jitter -- and ease otherwise. */
    if (g_tempo <= 0.0 || bpm > g_tempo * 1.05 || bpm < g_tempo * 0.95) g_tempo = bpm;
    else g_tempo = g_tempo * 0.7 + bpm * 0.3;
}


/* What the plugin asks the host. Restored deliberately close to the original: the
 * only additions are that the sample rate and block size answered are the ones
 * this host actually opened with, rather than constants that happened to match. */
static double g_cb_rate  = 48000.0;
static int    g_cb_block = 512;

static MS intptr_t host_callback(AEffect *fx, int32_t op, int32_t idx,
                                 intptr_t val, void *ptr, float opt)
{
    (void)fx; (void)idx; (void)val; (void)opt;
    switch (op) {
    case 0:  return 0;              /* audioMasterAutomate      */
    case 1:  return 2400;           /* audioMasterVersion       */
    case 2:  return 0;              /* audioMasterCurrentId     */
    case 6:  return 0;              /* audioMasterWantMidi      */
    case 7:                         /* audioMasterGetTime       */
        /* Not NULL: a plugin that reads tempo dereferences what it is given,
         * and four u-he plugins crashed in processReplacing over it. */
        vst_time_set_full(&g_transport, g_play_pos, g_cb_rate,
                          g_tempo, g_tsig_n, g_tsig_d, g_playing);
        return (intptr_t)&g_transport;
    case 13: return 0;              /* audioMasterSizeWindow    */
    case 16: return (intptr_t)g_cb_rate;   /* audioMasterGetSampleRate */
    case 17: return g_cb_block;            /* audioMasterGetBlockSize  */
    case 23: return 1;              /* audioMasterGetCurrentProcessLevel */
    case 32:                        /* audioMasterGetVendorString */
    case 33:                        /* audioMasterGetProductString */
        if (ptr) snprintf(ptr, 64, "peload");
        return 1;
    case 34: return 1000;           /* audioMasterGetVendorVersion */
    case 42: return 1;              /* audioMasterUpdateDisplay */
    default:
        if (pe_verbose()) fprintf(stderr, "  [host] unhandled opcode %d\n", op);
        return 0;
    }
}

static void ev_push_at(pehost *h, unsigned char t, unsigned char a, unsigned char b,
                       unsigned char c, float v, int32_t at)
{
    unsigned hd = atomic_load_explicit(&h->head, memory_order_relaxed);
    unsigned tl = atomic_load_explicit(&h->tail, memory_order_acquire);
    if (((hd + 1) & (EVQ - 1)) == (tl & (EVQ - 1))) {
        /* Dropping silently is how a sequencer ends up with a held note nobody
         * asked for: the note-on got through and its note-off did not. Say so,
         * once, so it is a reported fault rather than a mystery. */
        if (!h->ev_dropped++)
            fprintf(stderr, "pehost: the event queue overflowed -- some MIDI was "
                            "lost, and a note may hang\n");
        return;
    }
    h->evq[hd & (EVQ - 1)] = (ev_t){ t, a, b, c, v, mono_ns(), at };
    atomic_store_explicit(&h->head, hd + 1, memory_order_release);
}
static void ev_push(pehost *h, unsigned char t, unsigned char a, unsigned char b,
                    unsigned char c, float v)
{ ev_push_at(h, t, a, b, c, v, -1); }

static void free_bufs(pehost *h)
{
    int i;
    if (h->in)  for (i = 0; i < h->nin;  i++) { free(h->in[i]);  h->in[i]  = NULL; }
    if (h->out) for (i = 0; i < h->nout; i++) { free(h->out[i]); h->out[i] = NULL; }
    h->cap = 0;
}

/* Audio buffers, aligned for SSE and rounded up to a whole number of vectors.
 *
 * A plugin is entitled to assume the buffers a host gives it are suitable for
 * aligned vector loads and stores -- `movaps` faults rather than running slowly
 * when they are not -- and to process in whole vectors, running past the frame
 * count to the end of the last one. Ordinary malloc gives 16 bytes here, which is
 * enough for SSE but not for the AVX path a plugin may take, and it rounds
 * nothing up. 32 bytes, and a length rounded to a multiple of 8 floats, covers
 * both. */
#define PEHOST_BUF_ALIGN 32
#define PEHOST_BUF_ROUND 8

static float *alloc_audio_buf(int frames)
{
    size_t n = ((size_t)frames + PEHOST_BUF_ROUND - 1) & ~(size_t)(PEHOST_BUF_ROUND - 1);
    void *p = NULL;
    if (posix_memalign(&p, PEHOST_BUF_ALIGN, n * sizeof(float)) != 0) return NULL;
    memset(p, 0, n * sizeof(float));
    return p;
}

static int alloc_bufs(pehost *h, int frames)
{
    int i;
    if (frames <= h->cap) return 0;
    free_bufs(h);
    for (i = 0; i < h->nin; i++)
        if (!(h->in[i] = alloc_audio_buf(frames))) return -1;
    for (i = 0; i < h->nout; i++)
        if (!(h->out[i] = alloc_audio_buf(frames))) return -1;
    h->cap = frames;
    return 0;
}

/* True for a .vst3 file or bundle directory. */
static int looks_like_vst3(const char *path)
{
    size_t l = path ? strlen(path) : 0;
    return l > 5 && !strcasecmp(path + l - 5, ".vst3");
}

/* Is this a Classic Mac OS plug-in, and if so where is its code?
 *
 * Two shapes turn up. A bare PEF is the code by itself, extracted from a
 * resource fork by something else. The fork itself -- which is what a Classic
 * plug-in actually is -- keeps the code in an 'aEff' resource alongside the
 * artwork, and is the better thing to load because the artwork comes with it.
 *
 * Returns 1 and fills in where the PEF starts within `file`, or 0.
 */
static int classic_pef(const uint8_t *file, long len, uint32_t *off, uint32_t *n)
{
    const uint8_t *aeff;
    uint32_t size = 0;

    if (!file || len < 16) return 0;
    if (!memcmp(file, "Joy!peff", 8)) {
        *off = 0;
        *n = (uint32_t)len;
        return 1;
    }
    aeff = cfm_fork_find(file, (uint32_t)len, 0x61456666u /* 'aEff' */, 0, &size);
    if (aeff && size >= 16 && !memcmp(aeff, "Joy!peff", 8)) {
        *off = (uint32_t)(aeff - file);
        *n = size;
        return 1;
    }
    return 0;
}

/* Does this resource map list an 'aEff'? The map is self-contained -- a type list
 * whose offset is relative to the map's own start -- so this needs the map and
 * nothing else. */
static int map_has_aeff(const unsigned char *map, uint32_t maplen)
{
    uint32_t tlo;
    int ntypes, i;

    if (maplen < 30) return 0;
    tlo = be16((const uint8_t *)map + 24);
    if ((uint64_t)tlo + 2 > maplen) return 0;
    ntypes = (int)(int16_t)be16((const uint8_t *)map + tlo) + 1;
    if (ntypes < 0 || ntypes > 4096) return 0;
    for (i = 0; i < ntypes; i++) {
        uint32_t at = tlo + 2 + (uint32_t)i * 8;
        if ((uint64_t)at + 8 > maplen) return 0;
        if (be32((const uint8_t *)map + at) == 0x61456666u) return 1;      /* 'aEff' */
    }
    return 0;
}

/* Is this file a Classic Mac OS plug-in?
 *
 * This runs on every file in a plug-in tree, and the cost that matters is not how
 * much is read but how many files are *opened*. A Linux plug-in tree is tens of
 * thousands of files; opening each one costs a seek on a cold cache, and thirty
 * thousand of those take over a minute -- which is indistinguishable from the
 * program hanging when a directory is selected. Reading less per file does not
 * help, because the reading was never the expensive part.
 *
 * So decide from the name and the size, which the caller's directory listing has
 * already paid for, and only then open anything. Classic Mac files carry no
 * extension by convention -- Mac OS identified them by type and creator codes --
 * so "no extension" is the signal, together with the few suffixes such a file
 * picks up on the way off a Mac. The consequence to know about: a Classic plug-in
 * renamed to something like `.dat` will not be recognised, and the fix is to
 * rename it.
 *
 * What is left is opened, and even then only its header and resource map are
 * read: the map says whether there is an 'aEff' resource, which is what makes
 * this a VST rather than some other Classic file.
 */
static int classic_name_plausible(const char *path)
{
    static const char *ok[] = { ".vstclassic", ".rsrc", ".pef", ".vst", NULL };
    const char *base = strrchr(path, '/');
    const char *dot;
    int i;

    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    if (!dot) return 1;                       /* no extension: the usual case */
    for (i = 0; ok[i]; i++) if (!strcasecmp(dot, ok[i])) return 1;
    return 0;
}

int pehost_is_classic_mac(const char *path)
{
    unsigned char hdr[16];
    unsigned char *map;
    FILE *f;
    long len;
    uint32_t base = 0, dataoff, mapoff, datalen, maplen, be;
    int yes = 0;
    struct stat st;

    if (!path || !classic_name_plausible(path)) return 0;
    /* stat rather than open: metadata the caller has already looked at, and no
     * touching of the file's contents. A fork holding a PEF is never tiny. */
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    if (st.st_size < 16 * 1024 || st.st_size > 512L * 1024 * 1024) return 0;

    if (!(f = fopen(path, "rb"))) return 0;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return 0; }

    /* A bare PEF says so in its first eight bytes. */
    if (!memcmp(hdr, "Joy!peff", 8)) { fclose(f); return 1; }
    /* Anything that is plainly a native image is not this. */
    if (hdr[0] == 'M' && hdr[1] == 'Z')  { fclose(f); return 0; }
    if (hdr[0] == 0x7F && hdr[1] == 'E') { fclose(f); return 0; }

    len = (long)st.st_size;

    /* An AppleSingle/Double wrapper keeps the fork in entry id 2. */
    be = be32((const uint8_t *)hdr);
    if (be == 0x00051600u || be == 0x00051607u) {
        unsigned char ent[12];
        uint32_t n, k;
        if (fseek(f, 24, SEEK_SET) || fread(ent, 1, 2, f) != 2) { fclose(f); return 0; }
        n = be16((const uint8_t *)ent);
        if (n > 64) { fclose(f); return 0; }
        for (k = 0; k < n; k++) {
            if (fread(ent, 1, sizeof ent, f) != sizeof ent) { fclose(f); return 0; }
            if (be32((const uint8_t *)ent) == 2) { base = be32((const uint8_t *)ent + 4); break; }
        }
        if (!base || (uint64_t)base + 16 > (uint64_t)len) { fclose(f); return 0; }
        if (fseek(f, (long)base, SEEK_SET) ||
            fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return 0; }
    }

    dataoff = be32((const uint8_t *)hdr);
    mapoff  = be32((const uint8_t *)hdr + 4);
    datalen = be32((const uint8_t *)hdr + 8);
    maplen  = be32((const uint8_t *)hdr + 12);
    if (maplen < 30 || maplen > 4u * 1024 * 1024 || mapoff < dataoff ||
        (uint64_t)base + dataoff + datalen > (uint64_t)len ||
        (uint64_t)base + mapoff + maplen > (uint64_t)len) { fclose(f); return 0; }

    if ((map = malloc(maplen)) != NULL) {
        if (!fseek(f, (long)(base + mapoff), SEEK_SET) &&
            fread(map, 1, maplen, f) == maplen)
            yes = map_has_aeff(map, maplen);
        free(map);
    }
    fclose(f);
    return yes;
}

/* The input pump, installed process-wide before any plugin is opened. The Win32
 * window layer keeps its own copy; the Classic shim is handed it when a plugin is
 * opened, because its shim does not exist until then. */
static void (*g_pump_fn)(void *);
static void  *g_pump_ud;

/* The one directory a Classic plug-in can reach. It is where the guest's
 * Application Support folder appears, which is where a plug-in keeps its
 * settings -- and, for the ones that need authorising, where the authorisation
 * file its own installer produces has to go. */
const char *pehost_classic_support_dir(void)
{
    static char dir[1024];
    const char *env = getenv("PELOAD_CLASSIC_SUPPORT");
    const char *home;

    if (dir[0]) return dir;
    if (env && *env) { snprintf(dir, sizeof dir, "%s", env); return dir; }
    if ((env = getenv("XDG_DATA_HOME")) && *env)
        snprintf(dir, sizeof dir, "%s/peload/classic-support", env);
    else if ((home = getenv("HOME")) && *home)
        snprintf(dir, sizeof dir, "%s/.local/share/peload/classic-support", home);
    else
        snprintf(dir, sizeof dir, "/tmp/peload-classic-support");
    return dir;
}

static pehost *classic_open(const char *path, double samplerate, int blocksize)
{
    pehost *h;
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long len;
    uint32_t off = 0, n = 0;
    char err[256] = "";
    pefvst *v;

    if (!f) { snprintf(g_err, sizeof g_err, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 16 || len > 64L * 1024 * 1024 || !(buf = malloc((size_t)len))) {
        fclose(f);
        snprintf(g_err, sizeof g_err, "%s is not a plausible size", path);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(buf);
        snprintf(g_err, sizeof g_err, "could not read %s", path);
        return NULL;
    }
    fclose(f);

    if (!classic_pef(buf, len, &off, &n)) {
        free(buf);
        snprintf(g_err, sizeof g_err, "no 'aEff' code resource in %s", path);
        return NULL;
    }

    /* The whole file doubles as the resource fork: the artwork lives beside the
     * code, and a bare PEF simply has none, which the shim reports rather than
     * pretending otherwise. */
    v = pefvst_open(buf + off, n, buf, (uint32_t)len,
                    pehost_classic_support_dir(), samplerate, blocksize,
                    err, sizeof err);
    if (!v) {
        free(buf);
        snprintf(g_err, sizeof g_err, "%s (%s)",
                 err[0] ? err : "could not start it",
                 pehost_classic_support_dir());
        return NULL;
    }
    if (!(h = calloc(1, sizeof *h))) { pefvst_close(v); free(buf); return NULL; }
    /* A Classic editor tracks a drag by spinning on the mouse, so it needs the
     * pump to see anything change. Without it a dial follows the click and then
     * nothing else. */
    if (g_pump_fn) pefvst_set_input_pump(v, g_pump_fn, g_pump_ud);
    h->cl = v;
    h->cl_file = buf;
    h->cl_filelen = len;
    h->sr = samplerate;
    h->bs = blocksize > 0 ? blocksize : 512;
    g_last_classic = v;    /* for pehost_import_stats */
    return h;
}

static pehost *open_vst3(const char *path, double samplerate, int blocksize);
static pehost *open_inproc_pe(const char *path, double samplerate, int blocksize);

pehost *pehost_open(const char *path, double samplerate, int blocksize)
{
    pehost *h;

    /* Classic Mac OS first: it is not a format any of the paths below could
     * mistake for one of theirs, and it must not reach the isolation helper,
     * which loads native images only. */
    if (pehost_is_classic_mac(path)) return classic_open(path, samplerate, blocksize);

    /* Isolation is checked before the in-process paths so it applies to every
     * format the helper can host -- which is all of them, since the server drives
     * pehost too. i386 is excluded: it has its own helper below and must use it. */
    /* i386 is excluded because it has its own helper below and must use it.
     * Native ELF plugins are excluded because isolating them would cost them
     * their editor -- see pehost_is_native_elf. */
    if (pehost_isolation() && !pehost_is_i386(path) && !pehost_is_native_elf(path)) {
        bridge *br = bridge_open_helper(path, samplerate, blocksize, "peserve");
        if (!br) { snprintf(g_err, sizeof g_err, "%s", bridge_last_error()); return NULL; }
        if (!(h = calloc(1, sizeof *h))) { bridge_close(br); return NULL; }
        h->br = br;
        /* Remember what the helper is hosting: with the plugin out of process
         * there is no h->mv to tell a Mach-O bundle from a PE, and callers use
         * that to label which backend drew the editor. */
        h->is_mac = pehost_macho_kind(path) != 0;
        h->sr = samplerate;
        h->bs = blocksize > 0 ? blocksize : 512;
        return h;
    }

    {   /* macOS x86-64 needs no helper and no ABI layer -- the only difference
         * from a native plugin is the image format. */
        int mk = pehost_macho_kind(path);
        if (mk == 1) {
            macvst *mv = macvst_open(path, samplerate, blocksize);
            if (!mv) { snprintf(g_err, sizeof g_err, "%s", macvst_last_error()); return NULL; }
            if (!(h = calloc(1, sizeof *h))) { macvst_close(mv); return NULL; }
            h->mv = mv;
            h->sr = samplerate;
            h->bs = blocksize > 0 ? blocksize : 512;
            return h;
        }
        if (mk == 2) {
            macau *au = macau_open(path, samplerate, blocksize);
            if (!au) {
                /* An Audio Unit that is a VST in a wrapper.
                 *
                 * Symbiosis takes a VST2 and bolts a Component Manager entry
                 * point onto it; the result is one binary exporting both
                 * SymbiosisEntry and VSTPluginMain, with no AudioComponent
                 * factory and no AudioComponents key in its Info.plist -- so
                 * the modern AU path has nothing to call. The VST2 inside is
                 * the whole plugin, editor included, and this host already
                 * knows how to run it. Every Audio Damage Audio Unit is one of
                 * these. Only tried after the AU path has failed, and only
                 * succeeds if the bundle really does export a VST entry, so a
                 * genuinely broken AU still reports its own error. */
                macvst *mv = macvst_open(path, samplerate, blocksize);
                if (mv) {
                    if (!(h = calloc(1, sizeof *h))) { macvst_close(mv); return NULL; }
                    h->mv = mv;
                    h->sr = samplerate;
                    h->bs = blocksize > 0 ? blocksize : 512;
                    return h;
                }
                snprintf(g_err, sizeof g_err, "%s", macau_last_error());
                return NULL;
            }
            if (macau_configure(au)) {
                snprintf(g_err, sizeof g_err, "%s", macau_last_error());
                macau_close(au); return NULL;
            }
            if (!(h = calloc(1, sizeof *h))) { macau_close(au); return NULL; }
            h->au = au;
            /* Size the array from the unit rather than guessing: ModulAir
             * declares over five hundred parameters. */
            {   int n = macau_param_count(au);
                if (n > 0 && (h->au_ids = calloc((size_t)n, sizeof *h->au_ids)))
                    h->au_nids = macau_num_params(au, h->au_ids, n);
            }
            h->sr = samplerate;
            h->bs = blocksize > 0 ? blocksize : 512;
            return h;
        }
    }

    /* i386 cannot be loaded into this process at all, so it goes to a helper.
     * Dispatching here rather than in the callers is what keeps the GUI, the
     * CLI and the renderer width-agnostic. */
    if (pehost_is_i386(path)) {
        bridge *br;
        if (!bridge_available()) {
            snprintf(g_err, sizeof g_err,
                     "%s is 32-bit and no peload32 helper was found", path);
            return NULL;
        }
        if (!(br = bridge_open(path, samplerate, blocksize))) {
            snprintf(g_err, sizeof g_err, "%s", bridge_last_error());
            return NULL;
        }
        if (!(h = calloc(1, sizeof *h))) { bridge_close(br); return NULL; }
        h->br = br;
        h->sr = samplerate;
        h->bs = blocksize > 0 ? blocksize : 512;
        return h;
    }
    /* A native Linux VST2 shares the System V VST2 driver with the macOS one:
     * same ABI, same AEffect, only the way the entry point is found differs. */
    if (pehost_is_native_vst2(path)) {
        macvst *mv = macvst_open_native(path, samplerate, blocksize);
        if (!mv) { snprintf(g_err, sizeof g_err, "%s", macvst_last_error()); return NULL; }
        if (!(h = calloc(1, sizeof *h))) { macvst_close(mv); return NULL; }
        h->mv = mv;
        h->sr = samplerate;
        h->bs = blocksize > 0 ? blocksize : 512;
        return h;
    }

    /* VST3 is a different API on the same machine code. Native Linux bundles
     * load with dlopen and need no PE loader at all. */
    if (looks_like_vst3(path)) return open_vst3(path, samplerate, blocksize);

    /* Anything left is a Windows x86-64 PE, hosted in-process as a VST2. */
    return open_inproc_pe(path, samplerate, blocksize);
}

/* A VST3 on any platform. v3_open chooses the Microsoft or System V vtables by
 * looking at the binary, so this one path serves Windows and native Linux
 * bundles both -- and is what PEHOST_KIND_WIN_VST3 / LINUX_VST3 force. */
static pehost *open_vst3(const char *path, double samplerate, int blocksize)
{
    pehost *h;
    if (!(h = calloc(1, sizeof *h))) { snprintf(g_err, sizeof g_err, "oom"); return NULL; }
    h->is_v3 = 1;
    h->sr = samplerate;
    h->bs = blocksize;
    if (!(h->v3 = v3_open(path, samplerate, blocksize, g_err, (int)sizeof g_err))) {
        free(h);
        return NULL;
    }
    snprintf(h->name,   sizeof h->name,   "%s", v3_name(h->v3));
    snprintf(h->vendor, sizeof h->vendor, "%s", v3_vendor(h->v3));
    h->nin  = v3_num_inputs(h->v3);
    h->nout = v3_num_outputs(h->v3);
    /* Which loader took it, for labelling. A macOS bundle went through the
     * Mach-O path, so pehost_is_macos should say so and the editor should be
     * described as the backend that actually drew it. */
    h->is_mac = v3_is_macho(h->v3);
    return h;
}

/* The in-process path: map the PE, run its imports and DllMain, take the AEffect
 * from VSTPluginMain. pehost_open's default for any image that is not classic,
 * Mach-O, i386, native ELF or VST3 -- and what PEHOST_KIND_WIN_VST2_64 forces. */
static pehost *open_inproc_pe(const char *path, double samplerate, int blocksize)
{
    pehost *h;
    void *entry, *vm;

    g_err[0] = 0;
    if (pehost_thread_init()) { snprintf(g_err, sizeof g_err, "cannot install TEB"); return NULL; }
    if (!(h = calloc(1, sizeof *h))) { snprintf(g_err, sizeof g_err, "oom"); return NULL; }
    h->sr = samplerate;
    h->bs = blocksize;

    if (map_image(&h->im, path)) {
        explain_unmappable(path, g_err, sizeof g_err);
        goto fail;
    }
    if (apply_relocs(&h->im) < 0) {
        snprintf(g_err, sizeof g_err, "relocation failed");
        goto fail;
    }
    winstubs_init(h->im.base,
                  h->im.opt->DataDirectory[DIR_RESOURCE].VirtualAddress
                    ? h->im.base + h->im.opt->DataDirectory[DIR_RESOURCE].VirtualAddress
                    : NULL);
    if (resolve_imports(&h->im) < 0) {
        snprintf(g_err, sizeof g_err, "%s is needed and was not found -- put a "
                                      "copy beside the plug-in, or name its "
                                      "directory in PELOAD_DLL_PATH",
                 g_missing_real);
        goto fail;
    }
    protect_sections(&h->im);
    setup_tls(&h->im);

    entry = h->im.opt->AddressOfEntryPoint ? rva(&h->im, h->im.opt->AddressOfEntryPoint) : NULL;
    if (entry) {
        MS int32_t (*dllmain)(void *, uint32_t, void *) = (MS int32_t (*)(void *, uint32_t, void *))entry;
        if (!dllmain(h->im.base, 1, NULL)) {
            snprintf(g_err, sizeof g_err, "DllMain failed");
            goto fail;
        }
    }

    if (!(vm = find_export(&h->im, "VSTPluginMain")) && !(vm = find_export(&h->im, "main"))) {
        snprintf(g_err, sizeof g_err, "no VSTPluginMain export");
        goto fail;
    }
    {
        MS AEffect *(*f)(audioMasterCb) = (MS AEffect *(*)(audioMasterCb))vm;
        {   /* One allocation for the largest batch render_io_block can build, so the
         * audio thread never has to grow it. */
        size_t maxev = sizeof h->evmidi / sizeof h->evmidi[0];
        size_t bytes = offsetof(VstEvents, events) + maxev * sizeof(void *);
        h->evblk = calloc(1, bytes);
        h->evblk_n = h->evblk ? (int)maxev : 0;
    }
    g_cb_rate = samplerate > 0 ? samplerate : 48000.0;
    g_cb_block = blocksize > 0 ? blocksize : 512;
    h->fx = f(host_callback);
    }
    if (!h->fx || h->fx->magic != 0x56737450) {
        snprintf(g_err, sizeof g_err, "VSTPluginMain gave no valid AEffect");
        goto fail;
    }

    h->nin  = h->fx->numInputs;
    h->nout = h->fx->numOutputs;
    if (h->nout < 1) h->nout = 1;
    if (h->nin < 0 || h->nin > 256 || h->nout > 256) {
        snprintf(g_err, sizeof g_err, "implausible channel count (in %d out %d)",
                 h->nin, h->nout);
        pehost_close(h); return NULL;
    }
    {
        int n = h->nin > h->nout ? h->nin : h->nout;
        h->in  = calloc((size_t)n + 1, sizeof *h->in);
        h->out = calloc((size_t)n + 1, sizeof *h->out);
        if (!h->in || !h->out) {
            snprintf(g_err, sizeof g_err, "channel array allocation failed");
            pehost_close(h); return NULL;
        }
    }
    if (alloc_bufs(h, blocksize > 0 ? blocksize : 512)) {
        snprintf(g_err, sizeof g_err, "buffer allocation failed");
        pehost_close(h); return NULL;
    }

    h->fx->dispatcher(h->fx, effOpen, 0, 0, NULL, 0.0f);
    h->fx->dispatcher(h->fx, effSetSampleRate, 0, 0, NULL, (float)samplerate);
    h->fx->dispatcher(h->fx, effSetBlockSize, 0, blocksize, NULL, 0.0f);
    h->fx->dispatcher(h->fx, effMainsChanged, 0, 1, NULL, 0.0f);

    h->fx->dispatcher(h->fx, effGetEffectName, 0, 0, h->name, 0.0f);
    h->fx->dispatcher(h->fx, effGetVendorString, 0, 0, h->vendor, 0.0f);
    return h;

fail:
    /* Not pehost_close: there is no AEffect worth dispatching effClose to on any
     * of the paths that come here -- on the last one there is an AEffect-shaped
     * pointer that failed its magic check, which is the one thing that must not
     * be called into. So the image goes back by hand.
     *
     * It has to go back at all because these are ordinary outcomes, not fatal
     * ones: pestudio tries whatever is clicked, and every refusal used to keep
     * its SizeOfImage mapped -- 8 to 20 MB a go. */
    {
        pe_module img = { h->im.base, (unsigned long)h->im.size };
        pe_module_unload(&img);
    }
    free(h->evblk);
    free(h);
    return NULL;
}

/* ------------------------------------------------ classify & forced loaders */

/* The PE "machine" word from a resolved binary, 0 on any failure. Little-endian
 * fields read natively on the x86-64 host, same as map_image's own gate. */
static uint16_t pe_machine_of(const char *bin)
{
    FILE *f = fopen(bin, "rb");
    unsigned char mz[2];
    uint32_t lfanew;
    uint16_t machine;
    if (!f) return 0;
    if (fread(mz, 1, 2, f) != 2 || mz[0] != 'M' || mz[1] != 'Z') { fclose(f); return 0; }
    if (fseek(f, 0x3c, SEEK_SET) || fread(&lfanew, 4, 1, f) != 1 ||
        fseek(f, (long)lfanew + 4, SEEK_SET) || fread(&machine, 2, 1, f) != 1) {
        fclose(f); return 0;
    }
    fclose(f);
    return machine;
}

/* First bytes of a file, for the ELF-vs-other test. n<=8. */
static int head_bytes(const char *bin, unsigned char *buf, int n)
{
    FILE *f = fopen(bin, "rb");
    int got;
    if (!f) return 0;
    got = (int)fread(buf, 1, (size_t)n, f);
    fclose(f);
    return got;
}

static const struct { pehost_kind k; const char *name, *label; } k_tab[] = {
    { PEHOST_KIND_AUTO,        "auto",        "Auto-detect"         },
    { PEHOST_KIND_UNKNOWN,     "unknown",     "Unknown"             },
    { PEHOST_KIND_WIN_VST2_64, "win-vst2-64", "Windows VST2 64-bit" },
    { PEHOST_KIND_WIN_VST2_32, "win-vst2-32", "Windows VST2 32-bit" },
    { PEHOST_KIND_WIN_VST3,    "win-vst3",    "Windows VST3"        },
    { PEHOST_KIND_LINUX_VST3,  "linux-vst3",  "Linux VST3"          },
    { PEHOST_KIND_LINUX_VST2,  "linux-vst2",  "Linux VST2"          },
    { PEHOST_KIND_MAC_VST2,    "mac-vst2",    "macOS VST2"          },
    { PEHOST_KIND_MAC_VST3,    "mac-vst3",    "macOS VST3"          },
    { PEHOST_KIND_MAC_AU,      "mac-au",      "macOS Audio Unit"    },
    { PEHOST_KIND_CLASSIC_MAC, "classic-mac", "Mac OS 9 VST"        },
};

const char *pehost_kind_name(pehost_kind k)
{
    size_t i;
    for (i = 0; i < sizeof k_tab / sizeof k_tab[0]; i++)
        if (k_tab[i].k == k) return k_tab[i].name;
    return "unknown";
}

const char *pehost_kind_label(pehost_kind k)
{
    size_t i;
    for (i = 0; i < sizeof k_tab / sizeof k_tab[0]; i++)
        if (k_tab[i].k == k) return k_tab[i].label;
    return "Unknown";
}

pehost_kind pehost_kind_from_name(const char *name)
{
    size_t i;
    if (!name || !*name) return PEHOST_KIND_AUTO;
    for (i = 0; i < sizeof k_tab / sizeof k_tab[0]; i++)
        if (!strcasecmp(name, k_tab[i].name)) return k_tab[i].k;
    return PEHOST_KIND_AUTO;   /* unknown flag falls back to sniffing */
}

/* Small setter so each branch below reads as one line of intent. */
static void info_set(pehost_info *o, pehost_kind k, const char *os,
                     const char *arch, const char *fmt, int loadable)
{
    o->kind = k;
    snprintf(o->os,     sizeof o->os,     "%s", os   ? os   : "");
    snprintf(o->arch,   sizeof o->arch,   "%s", arch ? arch : "");
    snprintf(o->format, sizeof o->format, "%s", fmt  ? fmt  : "");
    snprintf(o->label,  sizeof o->label,  "%s", pehost_kind_label(k));
    o->loadable = loadable;
}

/* Does this PE file export a plug-in entry point?
 *
 * The machine field says a file is a Windows binary; it does not say the file
 * is a plug-in. Every .dll was being offered on its extension alone, so a
 * browser pointed at a directory with an installer in it listed setup.dll
 * beside the synthesisers, and picking one produced a load failure rather than
 * an explanation. The ELF side has always answered this properly -- see
 * pehost_is_native_vst2, which reads .dynsym rather than dlopen'ing every
 * candidate -- and this is the same test for the other format.
 *
 * The export directory is read straight out of the file: headers, the section
 * table to turn an RVA into a file offset, then the name array. Nothing is
 * mapped and nothing runs. A plug-in that exports the entry point and still
 * cannot load reaches the list and fails when picked, with a reason, which is
 * the trade the scanner already makes elsewhere. */
static int pe_rva_to_off(FILE *f, long sect_off, int nsec, uint32_t rva, uint32_t *out)
{
    int i;
    for (i = 0; i < nsec; i++) {
        uint8_t sh[40];
        uint32_t va, vsz, rsz, praw;
        if (fseek(f, sect_off + (long)i * 40, SEEK_SET)) return 0;
        if (fread(sh, 1, sizeof sh, f) != sizeof sh) return 0;
        memcpy(&vsz,  sh + 8,  4);
        memcpy(&va,   sh + 12, 4);
        memcpy(&rsz,  sh + 16, 4);
        memcpy(&praw, sh + 20, 4);
        /* VirtualSize can be 0 in old images; the raw size bounds it then. */
        if (!vsz) vsz = rsz;
        if (rva >= va && rva < va + vsz) {
            uint32_t d = rva - va;
            if (d >= rsz) return 0;              /* inside the zero-fill tail */
            *out = praw + d;
            return 1;
        }
    }
    return 0;
}

static int pe_exports_plugin_entry(const char *bin)
{
    FILE *f;
    unsigned char mz[2];
    uint32_t lfanew, expva = 0, expsz = 0, namecnt = 0, namerva = 0, nameoff, diroff;
    uint16_t nsec = 0, optsz = 0, magic = 0;
    long sect_off;
    uint32_t i;
    int found = 0;

    if (!bin || !(f = fopen(bin, "rb"))) return 0;
    if (fread(mz, 1, 2, f) != 2 || mz[0] != 'M' || mz[1] != 'Z') goto out;
    if (fseek(f, 0x3c, SEEK_SET) || fread(&lfanew, 4, 1, f) != 1) goto out;
    /* COFF header: signature(4) machine(2) nsections(2) ... optsize(2) */
    if (fseek(f, (long)lfanew + 6, SEEK_SET) || fread(&nsec, 2, 1, f) != 1) goto out;
    if (fseek(f, (long)lfanew + 20, SEEK_SET) || fread(&optsz, 2, 1, f) != 1) goto out;
    if (!nsec || nsec > 96 || optsz < 96) goto out;
    if (fseek(f, (long)lfanew + 24, SEEK_SET) || fread(&magic, 2, 1, f) != 1) goto out;

    /* The data directory sits after the optional header's fixed part, which is
     * a different length in the two formats: 96 bytes for PE32, 112 for PE32+.
     * Export is directory 0. */
    diroff = (magic == 0x20B) ? 112u : (magic == 0x10B) ? 96u : 0u;
    if (!diroff || optsz < diroff + 8) goto out;
    if (fseek(f, (long)lfanew + 24 + (long)diroff, SEEK_SET)) goto out;
    if (fread(&expva, 4, 1, f) != 1 || fread(&expsz, 4, 1, f) != 1) goto out;
    if (!expva || !expsz) goto out;              /* exports nothing at all */

    sect_off = (long)lfanew + 24 + optsz;
    if (!pe_rva_to_off(f, sect_off, nsec, expva, &nameoff)) goto out;

    /* IMAGE_EXPORT_DIRECTORY: NumberOfNames at +24, AddressOfNames at +32. */
    if (fseek(f, (long)nameoff + 24, SEEK_SET) || fread(&namecnt, 4, 1, f) != 1) goto out;
    if (fseek(f, (long)nameoff + 32, SEEK_SET) || fread(&namerva, 4, 1, f) != 1) goto out;
    if (!namecnt || namecnt > 65536) goto out;
    if (!pe_rva_to_off(f, sect_off, nsec, namerva, &nameoff)) goto out;

    for (i = 0; i < namecnt && !found; i++) {
        uint32_t srva, soff;
        char nm[64];
        size_t n;
        if (fseek(f, (long)nameoff + (long)i * 4, SEEK_SET)) break;
        if (fread(&srva, 4, 1, f) != 1) break;
        if (!pe_rva_to_off(f, sect_off, nsec, srva, &soff)) continue;
        if (fseek(f, (long)soff, SEEK_SET)) continue;
        n = fread(nm, 1, sizeof nm - 1, f);
        nm[n] = 0;
        /* VST2's entry point, its pre-2.4 spelling, and VST3's factory. The
         * bare "main" is why this cannot simply look for a substring: plenty
         * of libraries export something containing it. */
        if (!strcmp(nm, "VSTPluginMain") || !strcmp(nm, "main") ||
            !strcmp(nm, "GetPluginFactory"))
            found = 1;
    }
out:
    fclose(f);
    return found;
}

/* True when this path is a Windows plug-in worth offering: a PE that exports a
 * VST entry point. Exposed because both plug-in browsers need the same test --
 * they were deciding on the file extension, which is why setup.dll appeared in
 * the list. */
int pehost_is_windows_vst(const char *path)
{
    char bin[1024];
    if (!path || !*path) return 0;
    if (pehost_resolve(path, bin, sizeof bin) != 0)
        snprintf(bin, sizeof bin, "%s", path);
    if (!pe_machine_of(bin)) return 0;
    return pe_exports_plugin_entry(bin);
}

pehost_kind pehost_classify(const char *path, pehost_info *out)
{
    pehost_info scratch;
    char bin[1024];
    unsigned char h5[8];
    uint16_t machine;
    int mk;

    if (!out) out = &scratch;
    memset(out, 0, sizeof *out);
    out->kind = PEHOST_KIND_UNKNOWN;
    snprintf(out->label, sizeof out->label, "%s", pehost_kind_label(PEHOST_KIND_UNKNOWN));
    if (!path || !*path) { snprintf(out->why, sizeof out->why, "no path"); return out->kind; }

    /* Record the code file we would actually load; a bundle resolves to the
     * binary inside, a bare file to itself. Harmless if resolution fails. */
    if (pehost_resolve(path, bin, sizeof bin) == 0)
        snprintf(out->binary, sizeof out->binary, "%s", bin);
    else
        snprintf(out->binary, sizeof out->binary, "%s", path);

    /* The order mirrors pehost_open so the verdict names the loader that would
     * actually run -- except VST3 is tested before i386, so a 32-bit .vst3 is
     * reported as the VST3 it is rather than mistaken for a 32-bit VST2. */
    if (pehost_is_classic_mac(path)) {
        info_set(out, PEHOST_KIND_CLASSIC_MAC, "classic", "ppc", "VST2", 1);
        return out->kind;
    }
    if ((mk = pehost_macho_kind(path)) == 1) {
        info_set(out, PEHOST_KIND_MAC_VST2, "macos", "x86-64", "VST2", 1);
        return out->kind;
    }
    if (mk == 2) {
        info_set(out, PEHOST_KIND_MAC_AU, "macos", "x86-64", "AU", 1);
        return out->kind;
    }

    if (looks_like_vst3(path)) {
        struct stat mst;
        char macdir[1024];
        snprintf(macdir, sizeof macdir, "%s/Contents/MacOS", path);
        if (!stat(macdir, &mst) && S_ISDIR(mst.st_mode)) {
            /* Hosted by the same SysV VST3 code that drives a native Linux
             * bundle -- macOS x86-64 shares the ABI, so only the loader
             * underneath differs. */
            info_set(out, PEHOST_KIND_MAC_VST3, "macos", "x86-64", "VST3", 1);
            return out->kind;
        }
        if (pehost_resolve(path, bin, sizeof bin) != 0) {
            snprintf(out->format, sizeof out->format, "VST3");
            snprintf(out->why, sizeof out->why, "no binary inside the .vst3 bundle");
            return out->kind;   /* UNKNOWN */
        }
        machine = pe_machine_of(bin);
        if (machine == 0x8664) {
            info_set(out, PEHOST_KIND_WIN_VST3, "windows", "x86-64", "VST3", 1);
        } else if (machine == 0x14c) {
            info_set(out, PEHOST_KIND_WIN_VST3, "windows", "i386", "VST3", 0);
            snprintf(out->why, sizeof out->why, "32-bit Windows VST3 has no helper");
        } else if (head_bytes(bin, h5, 5) >= 5 && !memcmp(h5, "\177ELF", 4)) {
            int is64 = h5[4] == 2;
            info_set(out, PEHOST_KIND_LINUX_VST3, "linux", is64 ? "x86-64" : "i386",
                     "VST3", is64);
            if (!is64) snprintf(out->why, sizeof out->why, "32-bit Linux VST3 unsupported");
        } else {
            snprintf(out->format, sizeof out->format, "VST3");
            snprintf(out->why, sizeof out->why, "unrecognised binary in .vst3 bundle");
        }
        return out->kind;
    }

    if (pehost_is_native_vst2(path)) {
        info_set(out, PEHOST_KIND_LINUX_VST2, "linux", "x86-64", "VST2", 1);
        return out->kind;
    }

    /* Left with a Windows PE .dll, or a file we cannot place. */
    if (pehost_resolve(path, bin, sizeof bin) != 0) {
        snprintf(out->why, sizeof out->why, "no loadable binary at this path");
        return out->kind;   /* UNKNOWN */
    }
    machine = pe_machine_of(bin);
    /* A PE that exports no entry point is a Windows binary, not a plug-in --
     * an installer, a support library, a resource DLL. Saying so here is what
     * keeps setup.dll out of the browsers, which used to offer every .dll they
     * found and fail only when one was picked. */
    if ((machine == 0x8664 || machine == 0x14c) && !pe_exports_plugin_entry(bin)) {
        snprintf(out->os, sizeof out->os, "windows");
        snprintf(out->arch, sizeof out->arch, "%s", machine == 0x8664 ? "x86-64" : "i386");
        snprintf(out->why, sizeof out->why,
                 "PE binary but no VST entry point (VSTPluginMain or GetPluginFactory)");
        return out->kind;   /* UNKNOWN */
    }
    if (machine == 0x8664) {
        info_set(out, PEHOST_KIND_WIN_VST2_64, "windows", "x86-64", "VST2", 1);
    } else if (machine == 0x14c) {
        int have = bridge_available();
        info_set(out, PEHOST_KIND_WIN_VST2_32, "windows", "i386", "VST2", have);
        /* Not "no helper built" any more: it may be built, shipped and sitting
         * right there, with only its 32-bit libraries missing. Those are two
         * different problems for whoever reads this. */
        if (!have) {
            const char *r = bridge_unavailable_reason();
            snprintf(out->why, sizeof out->why, "%s",
                     r ? r : "no peload32 helper built");
        }
    } else if (head_bytes(bin, h5, 5) >= 2 && !memcmp(h5, "\177E", 2)) {
        snprintf(out->os, sizeof out->os, "linux");
        snprintf(out->why, sizeof out->why,
                 "ELF object but no VST2 entry point (VSTPluginMain)");
    } else if (machine) {
        snprintf(out->why, sizeof out->why, "unsupported PE machine 0x%x", machine);
    } else {
        snprintf(out->why, sizeof out->why, "not a PE, ELF or Mach-O plugin");
    }
    return out->kind;
}

/* Like pehost_open, but forced to a specific backend. AUTO defers to the sniff
 * chain; every other kind skips detection so a download the sniffer cannot name
 * still gets its chance, reporting that backend's own error if the guess is
 * wrong. Isolation is deliberately not applied to a forced load -- forcing is
 * "I know what this is", and the plain backend keeps the editor path simple. */
pehost *pehost_open_as(const char *path, pehost_kind kind,
                       double samplerate, int blocksize)
{
    pehost *h;
    int bs = blocksize > 0 ? blocksize : 512;

    if (kind == PEHOST_KIND_AUTO) return pehost_open(path, samplerate, blocksize);

    g_err[0] = 0;
    switch (kind) {
    case PEHOST_KIND_CLASSIC_MAC:
        return classic_open(path, samplerate, blocksize);

    case PEHOST_KIND_MAC_VST2: {
        macvst *mv = macvst_open(path, samplerate, blocksize);
        if (!mv) { snprintf(g_err, sizeof g_err, "%s", macvst_last_error()); return NULL; }
        if (!(h = calloc(1, sizeof *h))) { macvst_close(mv); return NULL; }
        h->mv = mv; h->sr = samplerate; h->bs = bs; return h;
    }
    case PEHOST_KIND_LINUX_VST2: {
        macvst *mv = macvst_open_native(path, samplerate, blocksize);
        if (!mv) { snprintf(g_err, sizeof g_err, "%s", macvst_last_error()); return NULL; }
        if (!(h = calloc(1, sizeof *h))) { macvst_close(mv); return NULL; }
        h->mv = mv; h->sr = samplerate; h->bs = bs; return h;
    }
    case PEHOST_KIND_MAC_AU: {
        macau *au = macau_open(path, samplerate, blocksize);
        if (!au) { snprintf(g_err, sizeof g_err, "%s", macau_last_error()); return NULL; }
        if (macau_configure(au)) {
            snprintf(g_err, sizeof g_err, "%s", macau_last_error());
            macau_close(au); return NULL;
        }
        if (!(h = calloc(1, sizeof *h))) { macau_close(au); return NULL; }
        h->au = au;
        {   int n = macau_param_count(au);
            if (n > 0 && (h->au_ids = calloc((size_t)n, sizeof *h->au_ids)))
                h->au_nids = macau_num_params(au, h->au_ids, n);
        }
        h->sr = samplerate; h->bs = bs; return h;
    }
    case PEHOST_KIND_WIN_VST2_32: {
        bridge *br;
        if (!bridge_available()) {
            snprintf(g_err, sizeof g_err,
                     "%s forced 32-bit but no peload32 helper was found", path);
            return NULL;
        }
        if (!(br = bridge_open(path, samplerate, blocksize))) {
            snprintf(g_err, sizeof g_err, "%s", bridge_last_error());
            return NULL;
        }
        if (!(h = calloc(1, sizeof *h))) { bridge_close(br); return NULL; }
        h->br = br; h->sr = samplerate; h->bs = bs; return h;
    }
    case PEHOST_KIND_WIN_VST3:
    case PEHOST_KIND_LINUX_VST3:
    case PEHOST_KIND_MAC_VST3:
        return open_vst3(path, samplerate, blocksize);

    case PEHOST_KIND_WIN_VST2_64:
        return open_inproc_pe(path, samplerate, blocksize);

    default:
        snprintf(g_err, sizeof g_err, "cannot force loader '%s'", pehost_kind_name(kind));
        return NULL;
    }
}

void pehost_close(pehost *h)
{
    /* Released up front: every backend below returns by its own path, and the
     * event block belongs to the handle rather than to any one of them. */
    if (h) { free(h->evblk); h->evblk = NULL; h->evblk_n = 0; }
    if (h && h->cl) {
        if (g_last_classic == h->cl) g_last_classic = NULL;
        pefvst_close(h->cl);
        free(h->cl_file);
        free(h->cl_io);
        free(h);
        return;
    }
    if (!h) return;
    if (h->br) { bridge_close(h->br); free(h); return; }
    if (h->mv) { macvst_close(h->mv); free(h); return; }
    if (h->au) { macau_close(h->au); free(h->au_ids); free(h); return; }
    /* Tear the editor down before the image goes: the Win32 layer holds the
     * plugin's WndProc, and pumping after the unmap would jump into freed
     * memory. */
    w32_reset();
    if (h->is_v3) {
        /* A macOS VST3 draws through the Cocoa stand-ins and the software Metal
         * backend, so it has the same teardown a macOS VST2 does: whatever is
         * left pointing at its view or its layer has to go before its image
         * does. Without this the next plugin's first mouse event went to a view
         * in an unmapped image. */
        if (v3_is_macho(h->v3)) {
            macns_reset_gui();
            macmetal_reset();
            macquartz_reset_editor();
        }
        v3_close(h->v3);
        free(h);
        return;
    }
    if (h->fx) {
        h->fx->dispatcher(h->fx, effMainsChanged, 0, 0, NULL, 0.0f);
        h->fx->dispatcher(h->fx, effClose, 0, 0, NULL, 0.0f);
    }
    free_bufs(h);
    free(h->in);  h->in  = NULL;
    free(h->out); h->out = NULL;
    {   /* Hand the image back so repeated loads do not accumulate. */
        pe_module m = { h->im.base, (unsigned long)h->im.size };
        pe_module_unload(&m);
    }
    free(h);
}

/* Every accessor below asks the same question of whichever backend the handle
 * turned out to be. They are written the long way -- one backend per line, the
 * null check taken once at the top -- because the short way they were in got to
 * five repetitions of `h &&` per function and a three-deep ternary sharing a
 * line with the `if` above it, and nobody could see what any of them answered
 * for a VST2. pehost_is_synth, further down, was always in this shape; these
 * now match it. */

const char *pehost_name(const pehost *h)
{
    if (!h) return "";
    if (h->mv) return macvst_name(h->mv);
    if (h->au) return "Audio Unit";
    if (h->br) return bridge_name(h->br);
    return h->name;
}

const char *pehost_vendor(const pehost *h)
{
    if (!h) return "";
    if (h->mv) return macvst_vendor(h->mv);
    if (h->au) return "";
    if (h->br) return bridge_vendor(h->br);
    return h->vendor;
}

int pehost_num_programs(const pehost *h)
{
    if (!h) return 0;
    if (h->mv) return macvst_num_programs(h->mv);
    if (h->au) return 0;
    if (h->br) return bridge_num_programs(h->br);
    if (h->is_v3) return v3_num_programs(h->v3);
    return h->fx ? h->fx->numPrograms : 0;
}

int pehost_num_params(const pehost *h)
{
    if (!h) return 0;
    if (h->cl) return pefvst_params(h->cl);
    if (h->mv) return macvst_num_params(h->mv);
    if (h->au) return h->au_nids;
    if (h->br) return bridge_num_params(h->br);
    if (h->is_v3) return v3_num_params(h->v3);
    return h->fx ? h->fx->numParams : 0;
}

int pehost_num_inputs(const pehost *h)
{
    if (!h) return 0;
    if (h->cl) return pefvst_inputs(h->cl);
    if (h->mv) return macvst_num_inputs(h->mv);
    if (h->au) return macau_is_effect(h->au) ? 2 : 0;
    if (h->br) return bridge_num_inputs(h->br);
    return h->nin;
}

int pehost_num_outputs(const pehost *h)
{
    if (!h) return 0;
    if (h->cl) return pefvst_outputs(h->cl);
    if (h->mv) return macvst_num_outputs(h->mv);
    if (h->au) return 2;
    if (h->br) return bridge_num_outputs(h->br);
    return h->nout;
}

int pehost_unique_id(const pehost *h)
{
    if (!h) return 0;
    if (h->cl) return pefvst_unique_id(h->cl);
    if (h->mv) return macvst_unique_id(h->mv);
    if (h->au) return 0;
    if (h->br) return bridge_unique_id(h->br);
    /* A VST3 has a 128-bit class id, not a VST2 uniqueID; 0 says "not one". */
    return !h->is_v3 && h->fx ? h->fx->uniqueID : 0;
}
int pehost_is_synth(const pehost *h)
{
    if (!h) return 0;
    if (h->cl) return (pefvst_flags(h->cl) & PV_FLAG_IS_SYNTH) != 0;
    if (h->mv) return macvst_is_synth(h->mv);
    if (h->au) return !macau_is_effect(h->au);
    if (h->br) return bridge_is_synth(h->br);
    if (h->is_v3) return v3_is_synth(h->v3);
    return h->fx ? (h->fx->flags & 0x100) != 0 : 0;         /* effFlagsIsSynth */
}

void pehost_program_name(pehost *h, int idx, char *buf, int n)
{
    if (!buf || n <= 0) return;
    buf[0] = 0;

    if (h && h->cl)          pefvst_string(h->cl, PV_GET_PROGRAM_NAME, idx, buf, n);
    else if (h && h->mv)     macvst_program_name(h->mv, idx, buf, n);
    else if (h && h->br)     bridge_program_name(h->br, idx, buf, n);
    else if (h && h->is_v3)  v3_program_name(h->v3, idx, buf, n);
    else if (h && h->fx) {
        /* The indexed query first: it names any program without disturbing the
         * current one.
         *
         * Its return value cannot be trusted to mean "answered". JUCE returns 1
         * and leaves the buffer untouched, so a plugin with no names at all looks
         * like a plugin that named everything -- which is why every program came
         * out blank rather than falling through to something readable. What the
         * buffer holds is the only reliable signal. */
        h->fx->dispatcher(h->fx, effGetProgramNameIndexed, idx, -1, buf, 0.0f);
        buf[n - 1] = 0;
        /* Nothing indexed: the plugin may still name the *current* program, which
         * is all effGetProgramName can report. */
        if (!buf[0] && idx == h->program)
            h->fx->dispatcher(h->fx, effGetProgramName, 0, 0, buf, 0.0f);
    }
    buf[n - 1] = 0;

    /* VST2 names live in a fixed-width field and are routinely space-padded, so
     * trim before deciding whether anything was said. */
    {
        int e = (int)strlen(buf);
        while (e > 0 && (unsigned char)buf[e - 1] <= ' ') buf[--e] = 0;
    }
    /* Something readable rather than a bare number against nothing. Plenty of
     * plugins never name their programs -- 20 of the 32 DISTRHO-Ports builds do
     * not, and Dexed reports 32 programs and names none of them because its
     * presets live in its own cartridge browser rather than in VST2 program
     * slots. One unnamed program is the plugin's default state; several want
     * telling apart, so they are numbered as a host would number them. */
    if (!buf[0]) {
        int total = pehost_num_programs(h);
        if (total <= 1) snprintf(buf, (size_t)n, "Default");
        else            snprintf(buf, (size_t)n, "Program %d", idx + 1);
    }
}
void pehost_set_program(pehost *h, int idx)
{
    if (h && h->cl) { pefvst_dispatch(h->cl, PV_SET_PROGRAM, 0, idx, 0, 0.0f); return; }
    if (h && h->mv) { macvst_set_program(h->mv, idx); return; }
    if (h && h->br) { bridge_set_program(h->br, idx); return; }
    if (h && h->is_v3) { v3_set_program(h->v3, idx); h->program = idx; return; }
    if (!h || !h->fx || idx < 0 || idx >= h->fx->numPrograms) return;
    h->program = idx;
    h->fx->dispatcher(h->fx, effSetProgram, 0, idx, NULL, 0.0f);
}
int pehost_get_program(pehost *h) {
    if (h && h->cl) return (int)pefvst_dispatch(h->cl, PV_GET_PROGRAM, 0, 0, 0, 0.0f);
    if (h && h->mv) return macvst_get_program(h->mv);
    if (h && h->br) return bridge_get_program(h->br); return h ? h->program : 0; }

void pehost_param_name(pehost *h, int i, char *buf, int n)
{
    if (h && h->cl) { pefvst_string(h->cl, PV_GET_PARAM_NAME, i, buf, n); return; }
    if (h && h->mv) { macvst_param_name(h->mv, i, buf, n); return; }
    if (h && h->au) { if (i >= 0 && i < h->au_nids)
            macau_param_info(h->au, h->au_ids[i], buf, n, NULL, NULL, NULL);
        else if (n > 0) buf[0] = 0;
        return; }
    if (h && h->br) { bridge_param_name(h->br, i, buf, n); return; }
    buf[0] = 0;
    if (h && h->is_v3) { v3_param_name(h->v3, i, buf, n); return; }
    if (h && h->fx) h->fx->dispatcher(h->fx, effGetParamName, i, 0, buf, 0.0f);
    buf[n-1] = 0;
}
void pehost_param_label(pehost *h, int i, char *buf, int n)
{
    if (h && h->cl) { pefvst_string(h->cl, PV_GET_PARAM_LABEL, i, buf, n); return; }
    if (h && h->mv) { macvst_param_label(h->mv, i, buf, n); return; }
    if (h && h->au) { if (n > 0) buf[0] = 0; return; }
    if (h && h->br) { bridge_param_label(h->br, i, buf, n); return; }
    buf[0] = 0;
    if (h && h->is_v3) return;      /* VST3 folds units into the display string */
    if (h && h->fx) h->fx->dispatcher(h->fx, effGetParamLabel, i, 0, buf, 0.0f);
    buf[n-1] = 0;
}
void pehost_param_display(pehost *h, int i, char *buf, int n)
{
    if (h && h->cl) { pefvst_string(h->cl, PV_GET_PARAM_DISPLAY, i, buf, n); return; }
    if (h && h->mv) { macvst_param_display(h->mv, i, buf, n); return; }
    if (h && h->au) { if (n > 0) snprintf(buf, (size_t)n, "%.3f",
            (i >= 0 && i < h->au_nids) ? macau_get_param(h->au, h->au_ids[i]) : 0.0f);
        return; }
    if (h && h->br) { bridge_param_display(h->br, i, buf, n); return; }
    buf[0] = 0;
    if (h && h->is_v3) { v3_param_display(h->v3, i, buf, n); return; }
    if (h && h->fx) h->fx->dispatcher(h->fx, effGetParamDisplay, i, 0, buf, 0.0f);
    buf[n-1] = 0;
}
float pehost_get_param(pehost *h, int i)
{
    if (h && h->cl) return pefvst_get_param(h->cl, i);
    if (h && h->mv) return macvst_get_param(h->mv, i);
    if (h && h->au) return (i >= 0 && i < h->au_nids)
                          ? macau_get_param(h->au, h->au_ids[i]) : 0.0f;
    if (h && h->br) return bridge_get_param(h->br, i);
    if (h && h->is_v3) return v3_get_param(h->v3, i);
    return (h && h->fx && h->fx->getParameter) ? h->fx->getParameter(h->fx, i) : 0.0f;
}
void pehost_set_param(pehost *h, int i, float v)
{
    if (h && h->cl) { pefvst_set_param(h->cl, i, v); return; }
    if (h && h->mv) { macvst_set_param(h->mv, i, v); return; }
    if (h && h->au) { if (i >= 0 && i < h->au_nids)
            macau_set_param(h->au, h->au_ids[i], v);
        return; }
    if (h && h->br) { bridge_set_param(h->br, i, v); return; }
    if (h && h->is_v3) { v3_set_param(h->v3, i, v); return; }
    if (h) ev_push(h, EV_PARAM, (unsigned char)(i & 0xff), (unsigned char)(i >> 8), 0, v); }

void pehost_flush_params(pehost *h)
{
    unsigned hd, tl;

    /* Every backend above applies parameter writes as they are made, so only
     * the in-process VST2 path has anything queued to flush.
     *
     * The 32-bit bridge is the exception deliberately left alone: its ring is
     * drained by the helper's audio thread immediately before each render, and
     * draining it from here would put a second consumer on a single-consumer
     * queue. A caller that needs a bridged plugin's values read back renders a
     * block instead, which is what peload does. */
    if (!h || !h->fx || h->is_v3 || h->br || h->mv || h->au || h->cl) return;

    hd = atomic_load_explicit(&h->head, memory_order_acquire);
    tl = atomic_load_explicit(&h->tail, memory_order_relaxed);
    for (; tl != hd; tl++) {
        ev_t *e = &h->evq[tl & (EVQ - 1)];
        if (e->type == EV_PARAM) {
            if (h->fx->setParameter)
                h->fx->setParameter(h->fx, e->a | (e->b << 8), e->v);
        } else if (e->type == EV_PROGRAM) {
            h->fx->dispatcher(h->fx, effSetProgram, 0, e->a, NULL, 0.0f);
        } else continue;
        /* Marked consumed rather than removed: the tail belongs to the audio
         * thread, and the render drain already skips any type it does not
         * recognise. MIDI is left where it is -- a note applied twice is not
         * the harmless no-op that setting a parameter twice is. */
        e->type = 0;
    }
}

void pehost_locate(pehost *h, double ppq)
{
    (void)h;
    if (ppq < 0.0) ppq = 0.0;
    /* The transport is kept in samples, so convert through the current tempo.
     * A later tempo change re-derives ppqPos from this position, which is what a
     * host does when the two disagree. */
    g_play_pos = ppq * (60.0 / (g_tempo > 0.0 ? g_tempo : 120.0)) * g_cb_rate;
}
double pehost_position(const pehost *h)
{
    (void)h;
    return g_cb_rate > 0.0
         ? g_play_pos / g_cb_rate * ((g_tempo > 0.0 ? g_tempo : 120.0) / 60.0) : 0.0;
}

void pehost_set_tempo(pehost *h, double bpm, int tsig_num, int tsig_den)
{
    (void)h;
    if (bpm > 0.0) g_tempo = bpm;
    if (tsig_num > 0) g_tsig_n = tsig_num;
    if (tsig_den > 0) g_tsig_d = tsig_den;
}
double pehost_tempo(const pehost *h) { (void)h; return g_tempo; }

void pehost_set_playing(pehost *h, int playing, int rewind)
{
    (void)h;
    g_playing = playing ? 1 : 0;
    if (rewind) g_play_pos = 0.0;
    if (!playing) { g_clk_last = 0; g_clk_n = 0; }
}
int pehost_playing(const pehost *h) { (void)h; return g_playing; }

void pehost_midi(pehost *h, int status, int d1, int d2)
{
    /* System realtime: a sequencer's clock and transport. These are not voice
     * messages and most plugins never see them -- what they are for is telling
     * the host what tempo to report and whether the song is rolling. */
    if ((status & 0xF8) == 0xF8) {
        switch (status) {
        case 0xF8: clock_tick(mono_ns()); return;          /* clock */
        case 0xFA: pehost_set_playing(h, 1, 1); return;    /* start */
        case 0xFB: pehost_set_playing(h, 1, 0); return;    /* continue */
        case 0xFC: pehost_set_playing(h, 0, 0); return;    /* stop */
        default: return;
        }
    }
    /* Song position pointer: where the sequencer has located to, in sixteenth
     * notes since the start. Without it, locating mid-song left the transport
     * running from wherever it happened to be, so a plugin syncing to bar lines
     * lined up with nothing. System common, not realtime -- it carries two data
     * bytes, which is why it cannot go in the switch above. */
    if (status == 0xF2) {
        int sixteenths = (d1 & 0x7F) | ((d2 & 0x7F) << 7);
        pehost_locate(h, (double)sixteenths / 4.0);
        return;
    }

    /* Program change selects the patch, not just a message passed along.
     *
     * Most VST2 instruments leave patch selection to the host and ignore the MIDI
     * message entirely, so forwarding it alone changes nothing -- a sequencer's
     * program changes were silently doing nothing unless the GUI happened to be
     * driving the list. The event is still forwarded afterwards, because a plugin
     * that does act on it is entitled to see it. */
    if (h && (status & 0xF0) == 0xC0) {
        int prog = d1 & 0x7F;
        if (prog < pehost_num_programs(h)) {
            /* Queued for the audio thread rather than dispatched here.
             *
             * MIDI arrives on its own reader thread, so calling effSetProgram
             * from it re-enters the plugin while processReplacing is running on
             * another -- a third thread inside code that expects at most the GUI
             * and the audio one. A sequencer sending program changes while notes
             * play would hit that constantly. Parameters already take this route;
             * this now matches them.
             *
             * h->program is set straight away so the host's idea of the current
             * patch is right immediately, which is what the UI reads. */
            if (!h->cl && !h->mv && !h->au && !h->br && !h->is_v3 && h->fx) {
                h->program = prog;
                ev_push(h, EV_PROGRAM, (unsigned char)prog, 0, 0, 0.0f);
            } else {
                pehost_set_program(h, prog);
            }
        }
    }

    if (h && h->cl) {
        /* VST 1.0 knows only note on and note off; anything else would have to
         * be a raw MIDI event the plug-in may not read. */
        if ((status & 0xF0) == 0x90 && d2 > 0) pefvst_note(h->cl, 1, d1, d2);
        else if ((status & 0xF0) == 0x80 || (status & 0xF0) == 0x90)
            pefvst_note(h->cl, 0, d1, 0);
        return;
    }
    if (h && h->mv) { macvst_midi(h->mv, status, d1, d2); return; }
    if (h && h->au) { macau_midi(h->au, status, d1, d2); return; }
    if (h && h->br) { bridge_midi(h->br, status, d1, d2); return; }
    if (h && h->is_v3) { v3_midi(h->v3, status, d1, d2); return; }
    if (h) ev_push(h, EV_MIDI, (unsigned char)status,
                   (unsigned char)(d1 & 0x7f), (unsigned char)(d2 & 0x7f), 0.0f);
}
void pehost_midi_at(pehost *h, int status, int d1, int d2, int frame)
{
    /* The bridge carries the offset across to the helper, which applies it on the
     * far side -- so a 32-bit plugin and an isolated one keep their timing. */
    if (h && h->br) { bridge_midi_at(h->br, status, d1, d2, frame); return; }
    if (h && !h->cl && !h->mv && !h->au && !h->is_v3) {
        ev_push_at(h, EV_MIDI, (unsigned char)status,
                   (unsigned char)(d1 & 0x7f), (unsigned char)(d2 & 0x7f), 0.0f,
                   frame < 0 ? 0 : frame);
        return;
    }
    pehost_midi(h, status, d1, d2);
}

void pehost_midi_stats(const pehost *h, unsigned *dropped, unsigned *spilled)
{
    if (dropped) *dropped = h ? h->ev_dropped : 0;
    if (spilled) *spilled = h ? h->ev_spilled : 0;
}

void pehost_set_input_mask(pehost *h, unsigned mask)
{
    if (!h) return;
    h->in_mask = mask;
    if (h->br) bridge_set_input_mask(h->br, mask);
    if (h->is_v3) v3_set_input_mask(h->v3, mask);
}

int pehost_alive(const pehost *h)
{
    if (!h) return 0;
    if (h->br) return bridge_alive(h->br);
    return 1;                    /* in process: if it had died, so would we */
}

void pehost_note_on(pehost *h, int note, int vel)  { pehost_midi(h, 0x90, note, vel); }
void pehost_note_off(pehost *h, int note)          { pehost_midi(h, 0x80, note, 0); }
void pehost_all_notes_off(pehost *h)
{
    if (h && h->br) { bridge_all_notes_off(h->br); return; }
    pehost_midi(h, 0xB0, 123, 0);       /* all notes off  */
    pehost_midi(h, 0xB0, 120, 0);       /* all sound off  */
}

void pehost_render(pehost *h, float *inter, int frames)
{ pehost_render_io(h, NULL, inter, frames); }

static void render_io_block(pehost *h, const float *src, float *inter,
                            int frames)
{
    /* Large enough that an ordinary bar of dense sequencing fits in one
     * block; anything beyond spills to the next rather than vanishing. */
    struct { VstEvents ev; VstMidiEvent m[256]; } pkt;
    unsigned hd, tl;
    int nev = 0, i, k;

    if (h && h->cl) {
        /* The interpreter works in separate channels, so de-interleave in and
         * re-interleave out. */
        enum { CLMAX = 8192 };
        float *ci0, *ci1, *co0, *co1;
        const float *in[2]; float *out[2];
        int i, n = frames > CLMAX ? CLMAX : frames;
        if (!h->cl_io &&
            !(h->cl_io = calloc((size_t)CLMAX * 4, sizeof *h->cl_io))) {
            memset(inter, 0, (size_t)frames * 2 * sizeof *inter);
            return;
        }
        ci0 = h->cl_io; ci1 = ci0 + CLMAX; co0 = ci1 + CLMAX; co1 = co0 + CLMAX;
        for (i = 0; i < n; i++) {
            ci0[i] = src ? src[2 * i] : 0.0f;
            ci1[i] = src ? src[2 * i + 1] : 0.0f;
        }
        in[0] = ci0; in[1] = ci1; out[0] = co0; out[1] = co1;
        if (pefvst_process(h->cl, in, out, n)) {
            memset(inter, 0, (size_t)frames * 2 * sizeof *inter);
            return;
        }
        for (i = 0; i < n; i++) {
            inter[2 * i]     = co0[i];
            /* A mono plug-in leaves the second output untouched, so send its one
             * channel to both rather than silence on the right. */
            inter[2 * i + 1] = pefvst_outputs(h->cl) > 1 ? co1[i] : co0[i];
        }
        if (frames > n) memset(inter + (size_t)n * 2, 0,
                               (size_t)(frames - n) * 2 * sizeof *inter);
        return;
    }
    if (h && h->mv) { macvst_render_io(h->mv, src, inter, frames); return; }
    if (h && h->au) {
        /* The AU host works in separate channels; the engine here is
         * interleaved, so de-interleave in and re-interleave out. */
        static float li[8192], ri[8192], lo[8192], ro[8192];
        int i, n = frames > 8192 ? 8192 : frames;
        for (i = 0; i < n; i++) {
            li[i] = src ? src[2 * i] : 0.0f;
            ri[i] = src ? src[2 * i + 1] : 0.0f;
        }
        if (macau_render(h->au, li, ri, lo, ro, n, NULL)) {
            memset(inter, 0, (size_t)frames * 2 * sizeof *inter);
            return;
        }
        for (i = 0; i < n; i++) { inter[2 * i] = lo[i]; inter[2 * i + 1] = ro[i]; }
        if (frames > n) memset(inter + (size_t)n * 2, 0,
                               (size_t)(frames - n) * 2 * sizeof *inter);
        return;
    }
    if (h && h->br) { bridge_render_io(h->br, src, inter, frames); return; }
    if (h && h->is_v3) { v3_render(h->v3, src, inter, frames); return; }
    if (!h || !h->fx) { memset(inter, 0, (size_t)frames * 2 * sizeof *inter); return; }
    if (frames > h->cap && alloc_bufs(h, frames)) {
        memset(inter, 0, (size_t)frames * 2 * sizeof *inter); return;
    }

    /* Drain the queue: MIDI is batched into one effProcessEvents call, while
     * parameter writes go straight through (setParameter is realtime-safe). */
    memset(&pkt, 0, sizeof pkt);
    hd = atomic_load_explicit(&h->head, memory_order_acquire);
    tl = atomic_load_explicit(&h->tail, memory_order_relaxed);
    {
        /* Where in this block each event belongs.
         *
         * deltaFrames was never set, so every event in a batch landed on sample
         * zero: a chord and an arpeggio came out identical, and nothing played
         * between block boundaries. At 256 frames that quantises everything to
         * 5.3 ms, which a sequencer hears as jitter.
         *
         * Events queued during the previous block are placed proportionally
         * across this one. The batch is still a block late -- that is inherent in
         * draining at the top of a callback -- but the *spacing* between events
         * survives, which is what makes a rhythm sound right. A caller that knows
         * the exact offset says so and is used verbatim. */
        uint64_t now = mono_ns();
        uint64_t t0  = h->blk_t0 ? h->blk_t0 : now;
        /* Converted at the sample rate, not by dividing the measured gap between
         * callbacks. The gap jitters -- that is what a scheduler does -- and
         * dividing by it fed every wobble straight into where notes landed. An
         * event three milliseconds after the last callback belongs three
         * milliseconds into this block, whatever the callback did. */
        double   ns_per_frame = 1e9 / (g_cb_rate > 0.0 ? g_cb_rate : 48000.0);
        h->blk_t0 = now;

        for (; tl != hd; tl++) {
            ev_t e = h->evq[tl & (EVQ - 1)];
            if (e.type == EV_PARAM) {
                if (h->fx->setParameter)
                    h->fx->setParameter(h->fx, e.a | (e.b << 8), e.v);
                continue;
            }
            if (e.type == EV_PROGRAM) {
                h->fx->dispatcher(h->fx, effSetProgram, 0, e.a, NULL, 0.0f);
                continue;
            }
            if (e.type != EV_MIDI) continue;
            if (nev >= (int)(sizeof pkt.m / sizeof pkt.m[0])) {
                /* Leave the rest queued rather than discarding them: a dropped
                 * note-off is a note that never stops. They go out at the top of
                 * the next block, still in order. */
                h->ev_spilled++;
                break;
            }
            {
                VstMidiEvent *m = &pkt.m[nev++];
                int32_t d;
                if (e.at >= 0) d = e.at;
                else if (e.t > t0)
                    d = (int32_t)((double)(e.t - t0) / ns_per_frame);
                else d = 0;
                if (d < 0) d = 0;
                if (d > frames - 1) d = frames - 1;
                m->type = 1;                /* kVstMidiType */
                m->byteSize = (int32_t)sizeof *m;
                m->deltaFrames = d;
                m->midiData[0] = (char)e.a;
                m->midiData[1] = (char)e.b;
                m->midiData[2] = (char)e.c;
            }
        }
    }
    atomic_store_explicit(&h->tail, tl, memory_order_release);

    if (nev) {
        /* VstEvents declares events[2] but is really variable-length: the pointer
         * array must be contiguous and numEvents long.
         *
         * Both the block and the events it points at live on the host and stay
         * valid until the *next* call, because a plugin is entitled to read them
         * during processReplacing rather than copying them out of
         * effProcessEvents. FM8 does exactly that: with the block freed as soon as
         * the dispatch returned, it walked released memory and took an event
         * pointer out of whatever had reused it. Out of process that killed the
         * helper, which is why its editor never appeared.
         *
         * Keeping them also takes a malloc and a free off the audio path, where
         * neither belongs. */
        void **arr;
        /* Allocated once when the plugin was opened. Growing it here would put a
         * realloc on the audio thread, which is the one place an allocator must
         * not appear. */
        if (!h->evblk || h->evblk_n < nev) goto no_events;
        memcpy(h->evmidi, pkt.m, (size_t)nev * sizeof pkt.m[0]);
        arr = (void **)((char *)h->evblk + offsetof(VstEvents, events));
        h->evblk->numEvents = nev;
        h->evblk->reserved = 0;
        for (i = 0; i < nev; i++) arr[i] = &h->evmidi[i];
        h->fx->dispatcher(h->fx, effProcessEvents, 0, 0, h->evblk, 0.0f);
    }
no_events:

    for (k = 0; k < h->nin; k++) {
        /* Which inputs the fed signal reaches. A plug-in with more than two
         * inputs is often two separate things -- a modulator and a carrier, on
         * FBVC -- and feeding a microphone to both puts the raw voice into the
         * output as well as into the analysis. The mask lets a caller send it
         * only where it belongs; the default is every channel, which is what a
         * plain stereo effect wants. */
        int fed = src && (h->in_mask == 0 || (h->in_mask & (1u << k)));
        if (fed) {
            int c = k < 2 ? k : k % 2;
            for (i = 0; i < frames; i++) h->in[k][i] = src[2 * i + c];
        } else {
            memset(h->in[k], 0, (size_t)frames * sizeof **h->in);
        }
    }
    for (k = 0; k < h->nout; k++) memset(h->out[k], 0, (size_t)frames * sizeof **h->out);

    h->fx->processReplacing(h->fx, h->nin ? h->in : NULL, h->out, frames);
    /* Only while rolling: a stopped transport whose position keeps moving makes
     * every tempo-synced plugin think the song is still running. */
    if (g_playing) g_play_pos += frames;

    for (i = 0; i < frames; i++) {
        inter[2 * i]     = h->out[0][i];
        inter[2 * i + 1] = h->nout >= 2 ? h->out[1][i] : h->out[0][i];
    }
}

/* One block, split so that no plugin is ever handed more frames than the block
 * size it was told at load time.
 *
 * That number is a promise, not a hint: a plugin sizes its internal buffers to it
 * and is entitled to assume it is never exceeded. Hosts get this wrong because
 * the audio backend's buffer is not always the size that was asked for -- a
 * PipeWire graph quantum can be larger than the requested latency -- and the
 * result is the plugin writing past the end of its own buffers. Cardinal asserts
 * and dies; a plugin without assertions corrupts memory quietly, which is worse.
 * Splitting here covers every caller rather than each one separately. */
void pehost_render_io(pehost *h, const float *src, float *inter, int frames)
{
    int bs, done;

    if (!h) {
        if (inter && frames > 0)
            memset(inter, 0, (size_t)frames * 2 * sizeof *inter);
        return;
    }
    bs = h->bs > 0 ? h->bs : 512;
    if (frames <= bs) { render_io_block(h, src, inter, frames); return; }
    for (done = 0; done < frames; ) {
        int chunk = frames - done;
        if (chunk > bs) chunk = bs;
        render_io_block(h, src ? src + (size_t)done * 2 : NULL,
                        inter + (size_t)done * 2, chunk);
        done += chunk;
    }
}

/* VST2 editor flag and rect come from the dispatcher. */
#define EFF_HAS_EDITOR 1

/* ---- msvcp: ours against Microsoft's -----------------------------------
 *
 * msvcp_shim.h reimplements part of the MSVC C++ library from layouts read out
 * of the real DLL. Reading a disassembly correctly and writing the same bytes
 * are different claims, and the second one is the one that matters: a plug-in
 * embeds these objects in its own and reaches into them with offsets its
 * compiler baked in, so a field in the wrong place is a wrong answer rather
 * than a failure.
 *
 * So: run both, on identical memory, and compare. Where the two differ only
 * because a pointer points into its own object -- which basic_streambuf is
 * full of -- the offset is compared instead of the address. A vftable or a
 * locale pointer differs by construction and is reported as such rather than
 * counted as a mismatch.
 *
 * Needs a real msvcp120.dll to compare against; says so and stops if there is
 * none. */
#define MPT_MAX 0x100

static const char *mpt_word_kind(const uint8_t *obj, size_t objlen, uint64_t v,
                                 int64_t *rel)
{
    uintptr_t base = (uintptr_t)obj;
    if (!v) return "null";
    if ((uintptr_t)v >= base && (uintptr_t)v < base + objlen) {
        *rel = (int64_t)((uintptr_t)v - base);
        return "self";
    }
    return "extern";
}

static int mpt_compare(const char *what, const uint8_t *a, const uint8_t *b,
                       size_t len)
{
    size_t i;
    int bad = 0, ext = 0;

    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t va, vb;
        int64_t ra = -1, rb = -1;
        const char *ka, *kb;
        memcpy(&va, a + i, 8);
        memcpy(&vb, b + i, 8);
        ka = mpt_word_kind(a, len, va, &ra);
        kb = mpt_word_kind(b, len, vb, &rb);
        if (!strcmp(ka, "self") && !strcmp(kb, "self")) {
            if (ra != rb) {
                fprintf(stderr, "    +0x%02zx  self+0x%llx vs self+0x%llx  MISMATCH\n",
                        i, (unsigned long long)ra, (unsigned long long)rb);
                bad++;
            }
            continue;
        }
        if (!strcmp(ka, "extern") && !strcmp(kb, "extern")) { ext++; continue; }
        if (va != vb) {
            fprintf(stderr, "    +0x%02zx  %016llx (%s) vs %016llx (%s)  MISMATCH\n",
                    i, (unsigned long long)va, ka, (unsigned long long)vb, kb);
            bad++;
        }
    }
    fprintf(stderr, "  %-34s %s", what, bad ? "DIFFERS" : "same");
    if (ext) fprintf(stderr, " (%d pointer%s out of the object, not compared)",
                     ext, ext == 1 ? "" : "s");
    fprintf(stderr, "\n");
    return bad;
}

int pehost_msvcp_selftest(void)
{
    pe_module *rm = real_module("msvcp120.dll");
    uint8_t mine[MPT_MAX], theirs[MPT_MAX];
    int bad = 0, ran = 0;

    if (!rm) {
        fprintf(stderr, "msvcp selftest: no real msvcp120.dll to compare against.\n"
                        "Put one in runtime/ or name its directory in "
                        "PELOAD_DLL_PATH.\n");
        return 2;
    }
    fprintf(stderr, "msvcp selftest: ours against Microsoft's msvcp120\n");

#define MPT_BOTH(sym, len, call)                                              \
    do {                                                                      \
        void *r = pe_module_export(rm, (sym));                                \
        void *o = winstub_lookup("msvcp120.dll", (sym));                      \
        if (!r || !o) {                                                       \
            fprintf(stderr, "  %-34s skipped (%s)\n", (sym),                  \
                    !r ? "not in the DLL" : "not implemented here");          \
            break;                                                            \
        }                                                                     \
        memset(mine, 0xA5, sizeof mine);                                      \
        memset(theirs, 0xA5, sizeof theirs);                                  \
        { void *self = theirs; call(r); }                                     \
        { void *self = mine;   call(o); }                                     \
        ran++;                                                                \
        bad += mpt_compare((sym), theirs, mine, (len));                       \
    } while (0)

    /* _Container_base12: one pointer, zeroed. */
#define CALL_CB(fn) ((void *(MS *)(void *))(fn))(self)
    MPT_BOTH("??0_Container_base12@std@@QEAA@XZ", 8, CALL_CB);

    /* basic_streambuf<char>: 0x68 bytes, six of them pointers into itself. */
#define CALL_SB(fn) ((void *(MS *)(void *))(fn))(self)
    MPT_BOTH("??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAA@XZ", 0x68, CALL_SB);

    /* basic_ios<char>: the protected constructor, which sets a vftable. */
#define CALL_IOS(fn) ((void *(MS *)(void *))(fn))(self)
    MPT_BOTH("??0?$basic_ios@DU?$char_traits@D@std@@@std@@IEAA@XZ", 8, CALL_IOS);

#undef MPT_BOTH

    /* Constructing the same object is half the claim. The other half is that
     * it behaves the same afterwards, so: publish a get and a put area through
     * setg and setp, write through sputn, and compare the object, what came out
     * and what each said it wrote. */
    {
        void *ctor_r = pe_module_export(rm, "??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAA@XZ");
        void *ctor_o = winstub_lookup("msvcp120.dll", "??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAA@XZ");
        void *setg_r = pe_module_export(rm, "?setg@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z");
        void *setg_o = winstub_lookup("msvcp120.dll", "?setg@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z");
        void *setp_r = pe_module_export(rm, "?setp@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD0@Z");
        void *setp_o = winstub_lookup("msvcp120.dll", "?setp@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD0@Z");
        void *sputn_r = pe_module_export(rm, "?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JPEBD_J@Z");
        void *sputn_o = winstub_lookup("msvcp120.dll", "?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JPEBD_J@Z");

        if (ctor_r && ctor_o && setg_r && setg_o && setp_r && setp_o &&
            sputn_r && sputn_o) {
            char gr[16], go[16], pr[16], po[16];
            int64_t nr, no;

            memset(mine, 0xA5, sizeof mine); memset(theirs, 0xA5, sizeof theirs);
            memcpy(gr, "GETAREA-", 8); memcpy(go, "GETAREA-", 8);
            memset(pr, '.', sizeof pr); memset(po, '.', sizeof po);

            ((void *(MS *)(void *))ctor_r)(theirs);
            ((void *(MS *)(void *))ctor_o)(mine);
            ((void (MS *)(void *, char *, char *, char *))setg_r)(theirs, gr, gr, gr + 8);
            ((void (MS *)(void *, char *, char *, char *))setg_o)(mine,   go, go, go + 8);
            ((void (MS *)(void *, char *, char *))setp_r)(theirs, pr, pr + 8);
            ((void (MS *)(void *, char *, char *))setp_o)(mine,   po, po + 8);
            nr = ((int64_t (MS *)(void *, const char *, int64_t))sputn_r)(theirs, "hello", 5);
            no = ((int64_t (MS *)(void *, const char *, int64_t))sputn_o)(mine,   "hello", 5);

            ran++;
            bad += mpt_compare("setg + setp + sputn (object)", theirs, mine, 0x68);
            if (nr != no) {
                fprintf(stderr, "    sputn returned %lld vs %lld  MISMATCH\n",
                        (long long)nr, (long long)no);
                bad++;
            } else {
                fprintf(stderr, "  %-34s same (%lld character%s)\n",
                        "sputn return", (long long)nr, nr == 1 ? "" : "s");
            }
            if (memcmp(pr, po, sizeof pr)) {
                fprintf(stderr, "    put area differs: '%.8s' vs '%.8s'  MISMATCH\n", pr, po);
                bad++;
            } else {
                fprintf(stderr, "  %-34s same ('%.5s')\n", "put area contents", pr);
            }
        } else {
            fprintf(stderr, "  %-34s skipped (an entry point is missing)\n",
                    "setg + setp + sputn");
        }
    }

    fprintf(stderr, "msvcp selftest: %d compared, %d field%s differ\n",
            ran, bad, bad == 1 ? "" : "s");
    return bad ? 1 : 0;
}

int pehost_editor_kind(pehost *h)
{
    /* A Classic editor draws into a GWorld -- guest memory -- so what comes
     * back is pixels, the same as a Windows editor. */
    if (h && h->cl) return (pefvst_flags(h->cl) & PV_FLAG_HAS_EDITOR)
                          ? PEHOST_EDITOR_PIXELS : PEHOST_EDITOR_NONE;
    /* A macOS VST2 editor draws through the software Metal backend and hands
     * back pixels, the same shape as a Windows editor. An Audio Unit reaches
     * the same backend by a different road -- see macau_editor_kind. */
    if (h && h->mv) {
        if (!macvst_editor_kind(h->mv)) return PEHOST_EDITOR_NONE;
        /* A native Linux VST2 is an X11 client: it embeds into a window we give
         * it. Reporting PIXELS for it sent the caller down the read-a-buffer
         * path, which opened the editor with no parent and left the plugin
         * holding a top-level window of its own. */
        return macvst_is_native(h->mv) ? PEHOST_EDITOR_X11 : PEHOST_EDITOR_PIXELS;
    }
    if (h && h->au)
        return macau_editor_kind(h->au) ? PEHOST_EDITOR_PIXELS : PEHOST_EDITOR_NONE;
    if (h && h->br) return bridge_editor_kind(h->br);
    if (!h) return PEHOST_EDITOR_NONE;
    if (h->is_v3) {
        /* A macOS VST3 draws into a view of ours and hands back pixels, exactly
         * as a macOS VST2 does -- asked first, because such a plugin supports
         * neither of the other two platform types and would otherwise be
         * reported as having no editor at all. */
        if (v3_is_macho(h->v3))
            return v3_editor_is_nsview(h->v3) ? PEHOST_EDITOR_PIXELS : PEHOST_EDITOR_NONE;
        /* A native Linux VST3 embeds into an X11 window; a Windows one goes
         * through the Win32 layer and gives us pixels. */
        if (v3_has_editor(h->v3)) return PEHOST_EDITOR_X11;
        return v3_editor_is_hwnd(h->v3) ? PEHOST_EDITOR_PIXELS : PEHOST_EDITOR_NONE;
    }
    if (h->fx && (h->fx->flags & EFF_HAS_EDITOR)) return PEHOST_EDITOR_PIXELS;
    return PEHOST_EDITOR_NONE;
}

/* Open a macOS VST3's editor.
 *
 * The one thing this needs that the other backends do not is a parent view:
 * IPlugView::attached takes an NSView and a plugin handed nothing refuses. The
 * runtime can mint one, so the host makes a container the size the plugin asked
 * for, hands it over, and then pumps exactly as it does for a macOS VST2 --
 * timers, then dirty rects, until a frame lands. */
static int open_macho_v3_editor(pehost *h)
{
    void *parent;
    int w = 0, ht = 0, k;

    if (!v3_editor_is_nsview(h->v3)) return -1;
    v3_editor_size(h->v3, &w, &ht);
    if (!(parent = macns_make_view(w, ht))) return -1;
    if (v3_editor_attach_nsview(h->v3, parent) != 0) return -1;

    /* Ask again now the view exists: a plugin that sizes itself from artwork it
     * had not loaded yet answers the first call with an empty rect. */
    if (w <= 0 || ht <= 0) v3_editor_size(h->v3, &w, &ht);
    if (w > 0 && ht > 0) macmetal_set_size(w, ht);

    for (k = 0; k < 16; k++) {
        const unsigned int *px; int pw = 0, ph = 0;
        pehost_editor_pump(h);
        if (pehost_editor_pixels(h, &px, &pw, &ph) && px && pw > 0 && ph > 0)
            return 0;
    }
    v3_editor_detach(h->v3);
    return -1;
}

int pehost_editor_open(pehost *h)
{
    if (h && h->cl) return pefvst_editor_open(h->cl) ? 0 : -1;
    if (h && h->mv) return macvst_editor_open(h->mv);
    if (h && h->au) return macau_editor_open(h->au);
    if (h && h->br) return bridge_editor_open(h->br);
    if (h && h->is_v3 && v3_is_macho(h->v3)) return open_macho_v3_editor(h);
    void *container;
    int w = 0, ht = 0;

    if (!h) return -1;
    pehost_editor_size(h, &w, &ht);
    /* No usable geometry means the plugin never told us how big its editor is;
     * everything downstream then works from a zero rect. Refuse rather than
     * hand it a guess and crash inside its own layout. */
    if ((w <= 0 || ht <= 0) && !h->is_v3 && h->fx) {
        /* Some plugins only know their size once the editor exists. TAL's
         * U-NO-62 answers effEditGetRect with an empty rect until effEditOpen
         * has run, and refusing on that alone cost it its editor entirely.
         *
         * So: open into a provisional container, ask again, then close and start
         * over at the size it names. Only reached for a plugin that would
         * otherwise be turned away, so a second open costs nothing that was not
         * already lost. */
        void *probe = w32_create_host_window(1600, 1200);
        if (probe) {
            int k;
            h->fx->dispatcher(h->fx, effEditOpen, 0, 0, probe, 0.0f);
            /* Let it settle before asking. A plugin creates its window first and
             * sizes it once its artwork is loaded, so both the rect and the
             * window are meaningless on the very first pass -- TAL's is 1x1
             * until it has painted. */
            for (k = 0; k < 30; k++) {
                w32_pump();
                h->fx->dispatcher(h->fx, effEditIdle, 0, 0, NULL, 0.0f);
                pehost_editor_size(h, &w, &ht);
                if (w <= 0 || ht <= 0) {
                    /* A plugin that sized its own window has told us the answer,
                     * just not through the opcode meant for it. */
                    void *pw = w32_root_window();
                    if (pw) w32_window_size(pw, &w, &ht);
                }
                if (w > 1 && ht > 1) break;
            }
            h->fx->dispatcher(h->fx, effEditClose, 0, 0, NULL, 0.0f);
        } else {
            fprintf(stderr, "editor: probe could not create a container\n");
        }
        w32_reset();
        if (w > 1 && ht > 1)
            fprintf(stderr, "editor: %s reported %dx%d only after opening\n",
                    h->name, w, ht);
    }
    /* No usable geometry means the plugin never told us how big its editor is;
     * everything downstream then works from a zero rect. Refuse rather than
     * hand it a guess and crash inside its own layout. */
    if (w <= 0 || ht <= 0) {
        fprintf(stderr, "editor: %s reports no editor size -- not opening\n", h->name);
        return -1;
    }
    container = w32_create_host_window(w, ht);
    if (!container) return -1;

    if (h->is_v3) {
        int r = v3_editor_attach_hwnd(h->v3, container);
        if (r) return r;
        /* Then tell it how big it is. attached() alone is not the sequence a
         * VST3 host performs -- onSize follows it -- and a plug-in that lays
         * itself out on that call has, until now, never been asked to.
         *
         * This is what left MinimogueVA's editor blank. Its window opened, its
         * Direct2D chain was built, its 96 images were decoded, and its paint
         * handler ran on every WM_PAINT and returned immediately, because the
         * handler draws only the rectangles in its dirty list and nothing had
         * ever put one there. Reading the object at paint time said so exactly:
         * the view pointer set, the device context created, and the list's
         * begin and end both null. Laying out is what fills it. */
        v3_editor_resized(h->v3, w, ht);
        /* Same guard as the VST2 path: a font that never reached the text
         * backend means the first paint dereferences an empty cache. */
        if (!w32_font_pipeline_ok()) {
            fprintf(stderr, "editor: %s registered a font that did not load -- "
                            "refusing the editor rather than crashing\n", h->name);
            v3_editor_detach(h->v3);
            w32_reset();
            return -1;
        }
        w32_show_editor();
        return 0;
    }

    /* VST2: hand the plugin our container and let it build its editor inside. */
    if (!h->fx) return -1;
    if (!h->fx->dispatcher(h->fx, effEditOpen, 0, 0, container, 0.0f)) {
        /* Some plugins return 0 from effEditOpen yet still create the window,
         * so trust the window rather than the return code. */
        if (!w32_root_window()) return -1;
    }
    /* A real out-pointer, never NULL: effEditGetRect's contract is that the
     * plugin writes an ERect* through it. Most check first; TAL's U-NO-62 stores
     * unconditionally, so passing NULL here was a store to address zero inside
     * the plugin -- which is exactly the crash that had it written off as
     * "faults in its own code". */
    { struct { int16_t top, left, bottom, right; } *rr = NULL;
      h->fx->dispatcher(h->fx, effEditGetRect, 0, 0, &rr, 0.0f); }
    h->fx->dispatcher(h->fx, effEditTop, 0, 0, NULL, 0.0f);

    /* Refuse an editor whose font never reached the text backend: painting it
     * would dereference the empty font cache and take the host down. */
    if (!w32_font_pipeline_ok()) {
        fprintf(stderr, "editor: %s registered a font that did not load -- "
                        "refusing the editor rather than crashing\n", h->name);
        h->fx->dispatcher(h->fx, effEditClose, 0, 0, NULL, 0.0f);
        w32_reset();
        return -1;
    }
    w32_show_editor();
    return 0;
}

void pehost_editor_pump(pehost *h)
{
    if (h && h->cl) {
        /* Idle, then draw. A Classic editor repaints when the host asks it to
         * -- see pefvst_editor_draw -- so a pump that only idles leaves every
         * control the plug-in has just made dirty still unpainted. */
        pefvst_dispatch(h->cl, PV_EDIT_IDLE, 0, 0, 0, 0.0f);
        pefvst_editor_draw(h->cl);
        return;
    }
    if (h && h->mv) { macvst_editor_pump(h->mv); return; }
    if (h && h->au) { macau_editor_pump(h->au); return; }
    if (h && h->br) return;   /* the helper pumps its own editor */
    if (h && h->is_v3 && v3_is_macho(h->v3)) {
        /* No run loop here either, so the host fires the editor's timers and
         * turns its dirty rects into draws. */
        macns_fire_timers();
        macns_draw_dirty();
        return;
    }
    w32_pump();
    /* VST2 editors animate off effEditIdle rather than a timer of their own. */
    if (h && !h->is_v3 && h->fx)
        h->fx->dispatcher(h->fx, effEditIdle, 0, 0, NULL, 0.0f);
    /* A VST3's editor is told here what its processor reported. */
    if (h && h->is_v3 && h->v3) v3_ui_idle(h->v3);
}

int pehost_editor_pixels(pehost *h, const unsigned int **px, int *w, int *height)
{
    if (h && h->cl) {
        const unsigned int *p = pefvst_editor_pixels(h->cl, w, height);
        if (px) *px = p;
        return p != NULL;
    }
    if (h && h->mv) return macvst_editor_pixels(h->mv, px, w, height);
    if (h && h->au) return macau_editor_pixels(h->au, px, w, height);
    if (h && h->br) return bridge_editor_pixels(h->br, px, w, height);
    if (h && h->is_v3 && v3_is_macho(h->v3))
        return macmetal_pixels(px, w, height) || macquartz_editor_pixels(px, w, height);
    return w32_editor_pixels(px, w, height);
}

/* Let the host feed input to a plugin that is spinning in its own message loop.
 * Forwarded to the window layer, which calls it from PeekMessage and from the
 * key-state queries. */
/* Installed process-wide, before any plugin is opened, so it is remembered here
 * and handed to each backend that needs it. The Win32 window layer keeps its own
 * copy; the Classic shim is given it when a plugin is opened, since its shim does
 * not exist until then. */
void pehost_set_input_pump(void (*fn)(void *), void *ud)
{
    g_pump_fn = fn;
    g_pump_ud = ud;
    w32_set_input_pump(fn, ud);
}

void pehost_editor_mouse(pehost *h, int x, int y, int msg, int buttons, int wheel)
{
    if (h && h->cl) {
        /* The window layer's messages are Win32 ones; all a Classic editor can
         * use is where the pointer is and whether a button is held. */
        pefvst_editor_mouse(h->cl, x, y, (buttons & 1) != 0);
        return;
    }
    if (h && h->mv) { macvst_editor_mouse(h->mv, x, y, msg, buttons, wheel); return; }
    if (h && h->au) { macau_editor_mouse(h->au, x, y, msg, buttons, wheel); return; }
    if (h && h->br) { bridge_editor_mouse(h->br, x, y, msg, buttons, wheel); return; }
    if (h && h->is_v3 && v3_is_macho(h->v3)) {
        /* Post only -- the pump paints. See macvst_editor_mouse. */
        macns_post_mouse(x, y, msg, buttons, wheel);
        return;
    }
    (void)h;
    w32_mouse(x, y, msg, buttons, wheel);
}

void pehost_editor_key(pehost *h, int vk, int down, int ch)
{
    if (h && h->cl) { if (down) pefvst_editor_key(h->cl, ch ? ch : vk); return; }
    if (h && h->mv) { macvst_editor_key(h->mv, vk, down, ch); return; }
    if (h && h->au) { macau_editor_key(h->au, vk, down, ch); return; }
    if (h && h->br) { bridge_editor_key(h->br, vk, down, ch); return; }
    if (h && h->is_v3 && v3_is_macho(h->v3)) {
        macns_post_key(vk, down, ch);
        return;
    } (void)h; w32_key(vk, down, ch); }

/* True for a plugin loaded from a Mach-O bundle. Callers use it to label what
 * they are looking at, not to change behaviour. */
int pehost_is_macos(pehost *h) { return h && (h->mv || h->au || h->is_mac || h->cl); }

/* True for a plugin interpreted from a Classic bundle. */
int pehost_is_classic(pehost *h) { return h && h->cl != NULL; }

int pehost_has_editor(pehost *h)
{ return pehost_editor_kind(h) != PEHOST_EDITOR_NONE; }
void pehost_editor_size(pehost *h, int *w, int *height)
{
    if (w) *w = 0;
    if (height) *height = 0;
    if (!h) return;
    if (h->cl) { pefvst_editor_size(h->cl, w, height); return; }
    if (h->mv) { macvst_editor_size(h->mv, w, height); return; }
    if (h->au) { macau_editor_size(h->au, w, height); return; }
    if (h->br) { bridge_editor_size(h->br, w, height); return; }
    if (h->is_v3) { v3_editor_size(h->v3, w, height); return; }
    if (h->fx) {
        /* ERect is four int16 in top,left,bottom,right order. */
        struct { int16_t top, left, bottom, right; } *r = NULL;
        h->fx->dispatcher(h->fx, effEditGetRect, 0, 0, &r, 0.0f);
        if (r) {
            if (w) *w = r->right - r->left;
            if (height) *height = r->bottom - r->top;
        }
    }
}
int pehost_editor_can_resize(pehost *h)
{ return (h && !h->br && h->is_v3) ? v3_editor_can_resize(h->v3) : 0; }
int pehost_editor_attach(pehost *h, unsigned long xid)
{
    if (h && h->mv && macvst_is_native(h->mv))
        return macvst_editor_attach(h->mv, xid);
    return (h && !h->br && h->is_v3) ? v3_editor_attach(h->v3, xid) : -1;
}
void pehost_editor_detach(pehost *h)
{
    if (h && h->mv) { macvst_editor_close(h->mv); return; }
    if (h && h->au) { macau_editor_close(h->au); return; }
    if (h && h->br) { bridge_editor_close(h->br); return; }
    if (h && h->is_v3) v3_editor_detach(h->v3);
}
void pehost_editor_resized(pehost *h, int w, int height)
{ if (h && !h->br && h->is_v3) v3_editor_resized(h->v3, w, height); }

void pehost_import_stats(int *implemented, int *stubbed, int *called)
{
    int i, c = 0;
    /* A Classic plug-in has no PE import table; its numbers live in the cfm
     * shim's binding table instead. */
    if (g_last_classic) {
        int bound = 0, unbound = 0, calls = 0;
        char names[256] = "";
        if (pefvst_import_stats(g_last_classic, &bound, &unbound, &calls,
                                names, sizeof names) == 0) {
            if (implemented) *implemented = bound;
            if (stubbed)     *stubbed     = unbound;
            if (called)      *called      = calls;
            if (unbound)
                fprintf(stderr, "imports with no implementation: %s\n", names);
            return;
        }
    }
    for (i = 0; i < g_nimp; i++) if (g_imp[i].calls) c++;
    if (implemented) *implemented = g_nresolved;
    if (stubbed)     *stubbed     = g_nimp;
    if (called)      *called      = c;
}
