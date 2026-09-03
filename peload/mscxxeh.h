/* MSVC C++ exceptions on x86-64: unwind data, handler search, type matching.
 *
 * Why this exists at all: Absynth carries a language handler on 12,178 of its
 * 42,803 functions. Throwing and catching is not an error path there, it is how
 * the code is written, and a host that cannot dispatch an exception cannot run
 * it past its first database lookup.
 *
 * There is no unwind information to be guessed at here -- x86-64 Windows records
 * it all in the image. `.pdata` is an array of RUNTIME_FUNCTION, one per
 * non-leaf function, sorted by address; each points at an UNWIND_INFO in
 * `.xdata` describing exactly which registers the prolog saved and where. That
 * is enough to walk from any instruction back to the caller, which is what both
 * the handler search and the eventual transfer need.
 *
 * The pieces, in the order the runtime uses them:
 *
 *   1. find the RUNTIME_FUNCTION covering a pc          ms_find_function
 *   2. undo its prolog to recover the caller's context  ms_unwind_step
 *   3. ask whether it declared a language handler       ms_handler_rva
 *   4. read MSVC's own tables to find its try blocks    ms_funcinfo
 *   5. match the thrown type against each catch clause  ms_catch_matches
 *
 * Steps 1 and 2 are the architecture's; 4 and 5 are the compiler's, and their
 * layouts are fixed by MSVC 2013 rather than documented. */

#ifndef PELOAD_MSCXXEH_H
#define PELOAD_MSCXXEH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__x86_64__)

/* ------------------------------------------------------------ unwind data --- */

typedef struct { uint32_t Begin, End, Unwind; } ms_runtime_function;

/* Register numbering used by the unwind codes -- the hardware's encoding, not a
 * choice: 0=rax, 1=rcx, 2=rdx, 3=rbx, 4=rsp, 5=rbp, 6=rsi, 7=rdi, 8..15=r8..r15.
 * Only the callee-saved ones are ever recorded, but the index space is all 16. */
enum { MS_RAX, MS_RCX, MS_RDX, MS_RBX, MS_RSP, MS_RBP, MS_RSI, MS_RDI };

typedef struct {
    uint64_t rip;
    uint64_t reg[16];          /* reg[MS_RSP] is the stack pointer */
} ms_ctx;

enum {
    MS_UWOP_PUSH_NONVOL = 0, MS_UWOP_ALLOC_LARGE = 1, MS_UWOP_ALLOC_SMALL = 2,
    MS_UWOP_SET_FPREG = 3, MS_UWOP_SAVE_NONVOL = 4, MS_UWOP_SAVE_NONVOL_FAR = 5,
    MS_UWOP_EPILOG = 6, MS_UWOP_SPARE = 7,
    MS_UWOP_SAVE_XMM128 = 8, MS_UWOP_SAVE_XMM128_FAR = 9,
    MS_UWOP_PUSH_MACHFRAME = 10
};
enum { MS_UNW_EHANDLER = 1, MS_UNW_UHANDLER = 2, MS_UNW_CHAININFO = 4 };

/* The exception directory, read straight from the mapped headers. Going through
 * the loader would mean threading it through every call site for no gain -- the
 * headers are mapped and say where it is. */
static const ms_runtime_function *ms_pdata(const uint8_t *base, uint32_t *count)
{
    uint32_t pe, ddir, rva, size;
    uint16_t magic;
    *count = 0;
    if (!base || base[0] != 'M' || base[1] != 'Z') return NULL;
    pe = *(const uint32_t *)(base + 0x3C);
    if (*(const uint32_t *)(base + pe) != 0x00004550u) return NULL;   /* "PE\0\0" */
    magic = *(const uint16_t *)(base + pe + 24);
    if (magic != 0x20B) return NULL;                                  /* PE32+ only */
    ddir = pe + 24 + 112;
    rva  = *(const uint32_t *)(base + ddir + 3 * 8);
    size = *(const uint32_t *)(base + ddir + 3 * 8 + 4);
    if (!rva || size < sizeof(ms_runtime_function)) return NULL;
    *count = size / (uint32_t)sizeof(ms_runtime_function);
    return (const ms_runtime_function *)(base + rva);
}

/* .pdata is sorted, so this is a binary search. With 42,803 entries a linear
 * scan per frame per throw would be the slowest thing in the host. */
static const ms_runtime_function *ms_find_function(const uint8_t *base, uint32_t rva)
{
    uint32_t n, lo = 0, hi;
    const ms_runtime_function *p = ms_pdata(base, &n);
    if (!p || !n) return NULL;
    hi = n - 1;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (rva < p[mid].Begin) { if (!mid) break; hi = mid - 1; }
        else if (rva >= p[mid].End) lo = mid + 1;
        else return &p[mid];
    }
    return NULL;
}

/* The handler RVA sits after the unwind codes, which are padded to an even
 * count. A function whose flags say CHAININFO has no handler of its own -- its
 * entry continues a parent's, and the search has to follow that instead. */
static uint32_t ms_handler_rva(const uint8_t *base, const ms_runtime_function *fn,
                               const void **handler_data)
{
    const uint8_t *ui;
    uint32_t flags, count, off;
    if (handler_data) *handler_data = NULL;
    if (!fn) return 0;
    ui = base + fn->Unwind;
    flags = (uint32_t)(ui[0] >> 3);
    count = ui[2];
    if (flags & MS_UNW_CHAININFO) return 0;
    if (!(flags & (MS_UNW_EHANDLER | MS_UNW_UHANDLER))) return 0;
    off = 4u + 2u * ((count + 1u) & ~1u);
    if (handler_data) *handler_data = ui + off + 4;
    return *(const uint32_t *)(ui + off);
}

/* Undo one function's prolog, leaving ctx describing its caller.
 *
 * The codes are listed in decreasing prolog offset, i.e. already in the order an
 * epilogue would undo them, so they are applied front to back. Anything that
 * only records where a register was saved leaves the stack pointer alone; only
 * the pushes and the explicit allocations move it.
 *
 * Returns 0 when the walk cannot continue. */
/* Unwinding only ever moves the stack pointer up, and never by more than a
 * function's frame. Checking that turns a wrong guess about a frame into a
 * stopped walk instead of a read through a wild pointer -- which matters because
 * this code runs while the process is already in trouble. */
static int ms_rsp_plausible(uint64_t was, uint64_t now)
{
    return now > was && (now & 7u) == 0 && now - was < (1u << 20);
}

static int ms_unwind_step(const uint8_t *base, ms_ctx *ctx)
{
    const ms_runtime_function *fn;
    const uint8_t *ui;
    uint32_t rva, flags, count, i;
    uint64_t rsp0;
    int guard = 0;

    if (!ctx->rip) return 0;
    if (!ctx->reg[MS_RSP] || (ctx->reg[MS_RSP] & 7u)) return 0;
    rsp0 = ctx->reg[MS_RSP];
    rva = (uint32_t)(ctx->rip - (uint64_t)(uintptr_t)base);
    fn = ms_find_function(base, rva);
    if (!fn) {
        /* A leaf function saves nothing and allocates nothing, so its return
         * address is exactly at the stack pointer. */
        ctx->rip = *(const uint64_t *)ctx->reg[MS_RSP];
        ctx->reg[MS_RSP] += 8;
        return ctx->rip != 0 && ms_rsp_plausible(rsp0, ctx->reg[MS_RSP]);
    }

again:
    if (++guard > 32) return 0;                  /* a chain that does not end */
    ui = base + fn->Unwind;
    flags = (uint32_t)(ui[0] >> 3);
    count = ui[2];
    {
        uint32_t framereg = ui[3] & 0x0F, frameoff = (uint32_t)(ui[3] >> 4);
        const uint16_t *code = (const uint16_t *)(ui + 4);
        /* A function with a frame register has already pointed it at a fixed
         * spot in the frame, so the stack pointer is recoverable from it
         * regardless of what the body did to rsp afterwards. */
        if (framereg && ctx->reg[framereg]
            && (ctx->reg[framereg] & 7u) == 0)
            ctx->reg[MS_RSP] = ctx->reg[framereg] - frameoff * 16u;

        for (i = 0; i < count; ) {
            uint32_t op = (code[i] >> 8) & 0x0F, info = (uint32_t)(code[i] >> 12);
            switch (op) {
            case MS_UWOP_PUSH_NONVOL:
                ctx->reg[info] = *(const uint64_t *)ctx->reg[MS_RSP];
                ctx->reg[MS_RSP] += 8;
                i += 1;
                break;
            case MS_UWOP_ALLOC_LARGE:
                if (info == 0) { ctx->reg[MS_RSP] += (uint64_t)code[i + 1] * 8; i += 2; }
                else { ctx->reg[MS_RSP] += (uint64_t)code[i + 1]
                                         | ((uint64_t)code[i + 2] << 16); i += 3; }
                break;
            case MS_UWOP_ALLOC_SMALL:
                ctx->reg[MS_RSP] += (uint64_t)(info + 1) * 8;
                i += 1;
                break;
            case MS_UWOP_SET_FPREG:
                /* Already applied above; listed so the walk stays in step. */
                i += 1;
                break;
            case MS_UWOP_SAVE_NONVOL:
                ctx->reg[info] = *(const uint64_t *)(ctx->reg[MS_RSP]
                                                    + (uint64_t)code[i + 1] * 8);
                i += 2;
                break;
            case MS_UWOP_SAVE_NONVOL_FAR:
                ctx->reg[info] = *(const uint64_t *)(ctx->reg[MS_RSP]
                                   + ((uint64_t)code[i + 1] | ((uint64_t)code[i + 2] << 16)));
                i += 3;
                break;
            case MS_UWOP_SAVE_XMM128:      i += 2; break;   /* no effect on rsp */
            case MS_UWOP_SAVE_XMM128_FAR:  i += 3; break;
            case MS_UWOP_EPILOG:           i += 1; break;
            case MS_UWOP_SPARE:            i += 2; break;
            case MS_UWOP_PUSH_MACHFRAME:
                /* A trap frame: the hardware pushed rip/cs/eflags/rsp/ss, and an
                 * error code as well when info says so. */
                ctx->reg[MS_RSP] += info ? 48 : 40;
                i += 1;
                break;
            default:
                return 0;                        /* an opcode we do not know */
            }
        }
        if (flags & MS_UNW_CHAININFO) {
            /* The parent entry follows the codes; its prolog has still to be
             * undone before the frame is whole. */
            uint32_t off = 4u + 2u * ((count + 1u) & ~1u);
            fn = (const ms_runtime_function *)(ui + off);
            flags = 0;
            goto again;
        }
    }
    if (!ms_rsp_plausible(rsp0, ctx->reg[MS_RSP] + 8)) return 0;
    ctx->rip = *(const uint64_t *)ctx->reg[MS_RSP];
    ctx->reg[MS_RSP] += 8;
    return ctx->rip != 0;
}

/* ------------------------------------------------- MSVC's own EH tables ----- */

/* Laid out by the compiler, not by the architecture. These are MSVC 2013's
 * shapes; the magic number is what confirms it at run time rather than by
 * assumption. */
typedef struct {
    uint32_t magic;
    int32_t  maxState;
    int32_t  unwindMap;         /* RVA */
    uint32_t nTryBlocks;
    int32_t  tryBlockMap;       /* RVA */
    uint32_t nIpMap;
    int32_t  ipToStateMap;      /* RVA */
    int32_t  esTypeList;        /* RVA */
    int32_t  ehFlags;
} ms_funcinfo;

typedef struct {
    int32_t tryLow, tryHigh, catchHigh;
    int32_t nCatches;
    int32_t handlerArray;       /* RVA */
} ms_tryblock;

typedef struct {
    uint32_t adjectives;
    int32_t  pType;             /* RVA to a TypeDescriptor; 0 means catch(...) */
    int32_t  dispCatchObj;
    int32_t  addressOfHandler;  /* RVA of the catch funclet */
    uint32_t frame;
} ms_handlertype;

typedef struct { uint32_t ip; int32_t state; } ms_ip2state;

/* A TypeDescriptor is a vtable pointer, a spare word, then the mangled name. */
typedef struct { void *vft; void *spare; char name[1]; } ms_typedesc;

typedef struct {
    uint32_t attributes;
    int32_t  pmfnUnwind;
    int32_t  pForwardCompat;
    int32_t  pCatchableTypeArray;
} ms_throwinfo;

typedef struct {
    uint32_t properties;
    int32_t  pType;             /* RVA to a TypeDescriptor */
    int32_t  thisDisp_mdisp, thisDisp_pdisp, thisDisp_vdisp;
    int32_t  sizeOrOffset;
    int32_t  copyFunction;
} ms_catchabletype;

static int ms_funcinfo_ok(const ms_funcinfo *fi)
{
    /* The three magic numbers MSVC has used. Anything else means the handler
     * data is not a FuncInfo and must not be walked as one. */
    return fi && (fi->magic == 0x19930520u || fi->magic == 0x19930521u
                  || fi->magic == 0x19930522u);
}

/* Which try state a pc is in. The map is sorted by ip, and the entry that
 * applies is the last one at or below the pc. */
static int ms_state_for_ip(const uint8_t *base, const ms_funcinfo *fi, uint32_t rva)
{
    const ms_ip2state *m;
    uint32_t i;
    int state = -1;
    if (!fi->ipToStateMap || !fi->nIpMap) return -1;
    m = (const ms_ip2state *)(base + fi->ipToStateMap);
    for (i = 0; i < fi->nIpMap; i++) {
        if (m[i].ip > rva) break;
        state = m[i].state;
    }
    return state;
}

/* Does this catch clause accept the thrown object?
 *
 * A catch matches when its type is one of the types the throw declared itself
 * catchable as -- which is how base classes are caught by reference. Comparison
 * is by mangled name rather than by descriptor address: MSVC emits one
 * descriptor per type per image, so the same type in two images is two
 * addresses, and this host has more than one image mapped. */
static int ms_catch_matches(const uint8_t *base, const ms_handlertype *h,
                            const ms_throwinfo *ti, const uint8_t *ti_base)
{
    const ms_catchabletype *const *unused = NULL;
    const int32_t *cta;
    int32_t n, i;
    const ms_typedesc *want;
    (void)unused;

    if (!h->pType) return 1;                            /* catch (...) */
    if (!ti || !ti->pCatchableTypeArray) return 0;
    want = (const ms_typedesc *)(base + h->pType);
    cta = (const int32_t *)(ti_base + ti->pCatchableTypeArray);
    n = cta[0];
    if (n < 0 || n > 64) return 0;
    for (i = 1; i <= n; i++) {
        const ms_catchabletype *ct = (const ms_catchabletype *)(ti_base + cta[i]);
        const ms_typedesc *got;
        if (!ct->pType) continue;
        got = (const ms_typedesc *)(ti_base + ct->pType);
        if (!strcmp(got->name, want->name)) return 1;
    }
    return 0;
}


/* The establisher frame: the base of a function's fixed stack allocation, which
 * is what its catch funclets use to reach the parent's locals. With a frame
 * register it is that register less the recorded offset; without one it is simply
 * the stack pointer, because MSVC does not move rsp after the prolog in a
 * function that has an EH funclet. */
static uint64_t ms_establisher_frame(const uint8_t *base,
                                    const ms_runtime_function *fn,
                                    const ms_ctx *ctx)
{
    const uint8_t *ui;
    uint32_t framereg, frameoff;
    if (!fn) return ctx->reg[MS_RSP];
    ui = base + fn->Unwind;
    framereg = ui[3] & 0x0F;
    frameoff = (uint32_t)(ui[3] >> 4);
    if (!framereg) return ctx->reg[MS_RSP];
    return ctx->reg[framereg] - (uint64_t)frameoff * 16u;
}

/* Adjectives on a catch clause. Only the two that change what gets written into
 * the catch's variable are needed here. */
enum { MS_HT_ISREFERENCE = 0x08, MS_HT_ISCOMPLUSEH = 0x80000000u };

/* A catch funclet is called with the establisher frame in its second argument
 * and returns the address in the parent to continue at. */
typedef MS void *(*ms_catch_funclet)(void *unused, uint64_t frame);

/* Resume in a frame that is still live: restore the callee-saved registers and
 * the stack pointer, then jump. Written in assembly for the same reason longjmp
 * is -- the target frame is not this one, and no C construct can leave for it. */
__asm__(
".text\n"
".globl peload_cxx_resume\n"
".type peload_cxx_resume,@function\n"
"peload_cxx_resume:\n"          /* (rcx = rip, rdx = rsp, r8 = saved regs) */
"    movq   0(%r8), %rbx\n"
"    movq   8(%r8), %rbp\n"
"    movq  16(%r8), %rsi\n"
"    movq  24(%r8), %rdi\n"
"    movq  32(%r8), %r12\n"
"    movq  40(%r8), %r13\n"
"    movq  48(%r8), %r14\n"
"    movq  56(%r8), %r15\n"
"    movq  %rdx, %rsp\n"
"    jmp   *%rcx\n"
".size peload_cxx_resume,.-peload_cxx_resume\n"
);
MS void peload_cxx_resume(uint64_t rip, uint64_t rsp, const uint64_t *regs);

/* Put the thrown object where the catch clause expects to find its variable.
 *
 * By reference -- the common form for exceptions -- stores the address. By value
 * needs a copy, and MSVC records how: a copy constructor when the type has one,
 * otherwise a flat copy of the recorded size. Getting this wrong writes a
 * pointer into a variable the catch body reads as an object, or the reverse. */
static void ms_deliver_object(const uint8_t *base, const ms_handlertype *h,
                              const ms_throwinfo *ti, void *object, uint64_t frame)
{
    void *dest;
    const int32_t *cta;
    int32_t n, i;
    const ms_typedesc *want;

    if (!h->dispCatchObj) return;              /* catch (...) with no variable */
    dest = (void *)(uintptr_t)(frame + (uint64_t)(int64_t)h->dispCatchObj);
    if (h->adjectives & MS_HT_ISREFERENCE) { *(void **)dest = object; return; }
    if (!h->pType || !ti || !ti->pCatchableTypeArray) return;

    want = (const ms_typedesc *)(base + h->pType);
    cta = (const int32_t *)(base + ti->pCatchableTypeArray);
    n = cta[0];
    for (i = 1; i <= n && i <= 64; i++) {
        const ms_catchabletype *ct = (const ms_catchabletype *)(base + cta[i]);
        const ms_typedesc *got;
        if (!ct->pType) continue;
        got = (const ms_typedesc *)(base + ct->pType);
        if (strcmp(got->name, want->name)) continue;
        if (ct->copyFunction) {
            /* A copy constructor, called the way MSVC calls one: object in the
             * first argument, source in the second. */
            typedef MS void *(*copyctor)(void *self, void *src);
            ((copyctor)(void *)(base + ct->copyFunction))(dest, object);
        } else if (ct->sizeOrOffset > 0) {
            memcpy(dest, object, (size_t)ct->sizeOrOffset);
        }
        return;
    }
}


/* The unwind map: for each try state, which destructors to run on the way out and
 * which state that leaves you in. This is how a C++ frame releases what it owns
 * while an exception passes through it.
 *
 * Skipping these does not corrupt memory directly, but anything the discarded
 * frames were holding stays held -- a lock, a file, a refcount. That is not a
 * theoretical worry: with them skipped, FM8 and Absynth would dispatch their
 * startup exception and then their audio thread would miss every deadline, which
 * is what a mutex nobody will now release looks like from outside. */
typedef struct { int32_t toState; int32_t action; } ms_unwindmap;

typedef MS void (*ms_unwind_funclet)(void *unused, uint64_t frame);

/* Walk from `from` down to `target`, running each state's action. The map is a
 * chain rather than a sequence -- each entry names the state it leaves behind --
 * so it is followed, not counted through. */
static void ms_run_unwind(const uint8_t *base, const ms_funcinfo *fi,
                          uint64_t frame, int from, int target)
{
    const ms_unwindmap *um;
    int s = from, guard = 0;

    if (!fi || !fi->unwindMap || fi->maxState <= 0) return;
    um = (const ms_unwindmap *)(base + fi->unwindMap);
    while (s > target && s >= 0 && s < fi->maxState && guard++ < 1024) {
        int next = um[s].toState;
        if (um[s].action)
            ((ms_unwind_funclet)(void *)(base + um[s].action))(NULL, frame);
        /* A malformed chain that does not descend would spin. */
        if (next >= s) break;
        s = next;
    }
}

#endif  /* __x86_64__ */

#if defined(__i386__)

/* MSVC C++ exceptions on i386: the fs:[0] registration chain.
 *
 * The x86-64 half above reads tables out of the image because that is where
 * Windows puts them. i386 has none: a function that needs a handler pushes an
 * EXCEPTION_REGISTRATION_RECORD -- {prev, handler} -- onto its own stack and
 * links it at fs:[0], so the chain is built by the plug-in as it runs. Every
 * MSVC __try prologue in the corpus does exactly this, and the peload32 TEB has
 * had a real %fs segment with the -1 terminator since it was written. The chain
 * was there all along; nothing walked it.
 *
 * That makes this half short, and the shortness is the point: the compiler's
 * tables are read by the plug-in's own __CxxFrameHandler, which is guest code we
 * simply call. We provide the two things it cannot provide itself -- somebody to
 * walk the chain when an exception is raised, and RtlUnwind to run the
 * termination handlers between the throw and the catch it picked.
 *
 * Control does not come back from a handler that catches. It calls RtlUnwind and
 * then jumps straight to its catch block with its own frame pointer, which
 * abandons every host frame between here and there -- the same thing longjmp
 * does, and safe for the same reason: this path holds no lock and owns no
 * allocation. */

typedef struct ms_exc_rec32 {
    uint32_t ExceptionCode;
    uint32_t ExceptionFlags;
    struct ms_exc_rec32 *ExceptionRecord;
    void    *ExceptionAddress;
    uint32_t NumberParameters;
    uint32_t ExceptionInformation[15];
} ms_exc_rec32;

typedef struct ms_seh32 {
    struct ms_seh32 *prev;
    void            *handler;
} ms_seh32;

/* What a handler may answer. Anything other than the first two means the
 * runtime has found a state we did not create and cannot repair. */
enum { MS_DISP_CONTINUE = 0, MS_DISP_SEARCH = 1,
       MS_DISP_NESTED = 2, MS_DISP_COLLIDED = 3 };

enum { MS_EH_NONCONTINUABLE = 0x01, MS_EH_UNWINDING     = 0x02,
       MS_EH_EXIT_UNWIND    = 0x04, MS_EH_STACK_INVALID = 0x08,
       MS_EH_NESTED_CALL    = 0x10, MS_EH_TARGET_UNWIND = 0x20,
       MS_EH_COLLIDED_UNWIND = 0x40 };

/* The end of the chain is -1, not NULL: a __try prologue pushes the old fs:[0]
 * unconditionally, and a zero there would read as a record at address 0. */
#define MS_SEH_END ((ms_seh32 *)0xFFFFFFFFu)

/* i386 CONTEXT is 716 bytes. A handler is handed one because the ABI says so;
 * MSVC's reads it only on the paths the kernel uses to resume a thread, which a
 * raise from in here cannot take. Zeroed at the right size is therefore correct,
 * and a smaller buffer would not be -- the handler may write to it. */
#define MS_CONTEXT32_SIZE 716

typedef int (*ms_seh_handler32)(ms_exc_rec32 *, void *, void *, void *);

static ms_seh32 *ms_seh_head(void)
{ ms_seh32 *h; __asm__ volatile ("movl %%fs:0, %0" : "=r"(h)); return h; }

static void ms_seh_set_head(ms_seh32 *f)
{ __asm__ volatile ("movl %0, %%fs:0" :: "r"(f) : "memory"); }

/* A frame has to be on the stack, aligned, and further from the top than the one
 * that named it. Windows checks the same three things, and for the same reason:
 * the chain is guest-writable memory, and a plug-in that overruns a buffer on
 * the stack corrupts it. Walking a corrupt chain calls whatever the overrun
 * left behind, so a raise that should report a bad throw instead jumps into
 * data. Refusing to walk is the honest failure. */
static int ms_seh_plausible(const ms_seh32 *f, const ms_seh32 *prev)
{
    if (!f || f == MS_SEH_END)   return 0;
    if ((uintptr_t)f & 3)        return 0;
    if (prev && f <= prev)       return 0;
    return f->handler != NULL;
}

/* RtlUnwind: run every termination handler between here and `target_frame`,
 * unlinking each as it goes.
 *
 * A handler is popped after it runs, not before -- one that raises again must
 * not find the frame it is unwinding still linked, or the second raise walks
 * into it and recurses. */
static void ms_unwind32(void *target_frame, void *target_ip,
                        ms_exc_rec32 *rec, void *retval)
{
    ms_exc_rec32 local;
    uint8_t ctx[MS_CONTEXT32_SIZE];
    ms_seh32 *target = target_frame ? (ms_seh32 *)target_frame : MS_SEH_END;
    ms_seh32 *f, *last = NULL;
    void *dispatch = NULL;
    int guard = 0;

    (void)target_ip; (void)retval;

    if (!rec) {
        memset(&local, 0, sizeof local);
        local.ExceptionCode  = 0xC0000027u;      /* STATUS_UNWIND */
        local.ExceptionFlags = MS_EH_NONCONTINUABLE;
        rec = &local;
    }
    rec->ExceptionFlags |= MS_EH_UNWINDING;
    if (target == MS_SEH_END) rec->ExceptionFlags |= MS_EH_EXIT_UNWIND;

    memset(ctx, 0, sizeof ctx);
    for (f = ms_seh_head(); f != target && ms_seh_plausible(f, last); ) {
        ms_seh32 *next = f->prev;
        if (++guard > 4096) break;
        ((ms_seh_handler32)f->handler)(rec, f, ctx, &dispatch);
        ms_seh_set_head(next);
        last = f;
        f = next;
    }
    if (f == target) ms_seh_set_head(target);
}

/* Deliver `rec` to the chain. Returns non-zero only when a handler asked for
 * execution to continue -- a handler that catches never comes back at all. */
/* The mangled name of a thrown C++ type, for the report when nothing catches it.
 *
 * A plug-in that throws during init and dies with a bare 0xE06D7363 tells you
 * nothing; the type name usually names the subsystem that gave up. MSVC hangs
 * the whole description off the second exception parameter, and on i386 every
 * pointer in that structure is absolute -- the RVA form is x86-64's.
 *
 * Returns NULL rather than guessing when any link does not look like what it
 * should: this runs on the failure path, where the data is by definition
 * already suspect. */
static const char *ms_throw_typename32(const ms_exc_rec32 *rec)
{
    typedef struct { uint32_t attrs, unwind, fwd, catchable; } throwinfo32;
    typedef struct { uint32_t props, type, disp, size, copy; } catchable32;
    const throwinfo32 *ti;
    const uint32_t *arr;
    const catchable32 *ct;
    const char *name;

    if (!rec || rec->ExceptionCode != 0xE06D7363u || rec->NumberParameters < 3)
        return NULL;
    ti = (const throwinfo32 *)(uintptr_t)rec->ExceptionInformation[2];
    if (!ti || !ti->catchable) return NULL;
    arr = (const uint32_t *)(uintptr_t)ti->catchable;
    if (!arr[0] || arr[0] > 64) return NULL;       /* count of catchable types */
    ct = (const catchable32 *)(uintptr_t)arr[1];   /* the most derived one */
    if (!ct || !ct->type) return NULL;
    /* TypeDescriptor: vftable, spare, then the mangled name inline. */
    name = (const char *)(uintptr_t)ct->type + 8;
    return (name[0] == '.' || name[0] == '?') ? name : NULL;
}

static int ms_eh_verbose(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("PELOAD_VERBOSE");
                                  v = e && *e != '0'; } return v; }

static int ms_dispatch32(ms_exc_rec32 *rec)
{
    uint8_t ctx[MS_CONTEXT32_SIZE];
    ms_seh32 *f, *last = NULL;
    void *dispatch = NULL;
    int guard = 0;

    if (ms_eh_verbose())
        fprintf(stderr, "  [eh] raise 0x%08x, chain head %p\n",
                rec->ExceptionCode, (void *)ms_seh_head());

    memset(ctx, 0, sizeof ctx);
    for (f = ms_seh_head(); ms_seh_plausible(f, last); last = f, f = f->prev) {
        int d;
        if (++guard > 4096) break;
        d = ((ms_seh_handler32)f->handler)(rec, f, ctx, &dispatch);
        if (ms_eh_verbose())
            fprintf(stderr, "  [eh]   frame %p handler %p -> %d\n",
                    (void *)f, f->handler, d);
        if (d == MS_DISP_CONTINUE) return 1;
        if (d != MS_DISP_SEARCH)   break;
    }
    {
        const char *t = ms_throw_typename32(rec);
        fprintf(stderr, "[eh] nothing caught 0x%08x after %d frame(s)%s%s\n",
                rec->ExceptionCode, guard, t ? "; thrown type " : "", t ? t : "");
    }
    return 0;
}

#endif  /* __i386__ */
#endif  /* PELOAD_MSCXXEH_H */
