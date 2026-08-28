/* A 32-bit PowerPC interpreter, for Classic Mac OS and Carbon plug-ins.
 *
 * Everything else in this project runs native code: a Windows plug-in is x86-64
 * machine code that this CPU executes directly, and the work is all in the ABI
 * and the libraries around it. A Classic Mac OS plug-in is different in kind --
 * it is PowerPC code, and there is no way to run it on x86-64 except to
 * interpret it. That is what this is.
 *
 * Two things follow from PowerPC being big-endian, and both are easy to get
 * wrong in ways that produce plausible nonsense:
 *
 *   - Guest memory is stored big-endian. Every load and store swaps. The guest's
 *     own byte order is the authority, not the host's.
 *   - The guest's address space is separate from ours. A guest pointer is an
 *     offset into `m->mem`, never a host pointer, so a bug cannot escape into the
 *     host's address space -- which matters when the code being run is a
 *     twenty-year-old binary of unknown provenance.
 *
 * Scope: the user-mode integer, branch, load/store, rotate/shift and floating
 * point instructions that compiled C actually emits, plus the condition register
 * and LR/CTR. Not here: supervisor instructions and the vector unit. Floating
 * point exceptions are not modelled either -- FPSCR can be read and written, but
 * nothing sets its exception bits, so the `Rc` forms leave CR1 clear.
 *
 * An unimplemented opcode stops the interpreter and names itself rather than
 * being skipped, because silently ignoring an instruction corrupts state in a way
 * that surfaces much later as nonsense.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc.h"

/* ------------------------------------------------------------------ memory */

/* Guest memory is one flat block. A guest address is an index into it, so an
 * out-of-range access is caught here rather than becoming a host segfault. */
static int mem_ok(ppc *m, uint32_t addr, uint32_t len)
{
    if ((uint64_t)addr + len > m->memsize) {
        snprintf(m->err, sizeof m->err,
                 "guest access to 0x%08x (%u byte%s) is outside its %u-byte memory",
                 addr, len, len == 1 ? "" : "s", m->memsize);
        m->stopped = 1;
        return 0;
    }
    return 1;
}

uint8_t ppc_read8(ppc *m, uint32_t a)
{ return mem_ok(m, a, 1) ? m->mem[a] : 0; }

uint16_t ppc_read16(ppc *m, uint32_t a)
{
    if (!mem_ok(m, a, 2)) return 0;
    return (uint16_t)((m->mem[a] << 8) | m->mem[a + 1]);       /* big-endian */
}

uint32_t ppc_read32(ppc *m, uint32_t a)
{
    if (!mem_ok(m, a, 4)) return 0;
    return ((uint32_t)m->mem[a] << 24) | ((uint32_t)m->mem[a + 1] << 16) |
           ((uint32_t)m->mem[a + 2] << 8) | m->mem[a + 3];
}

void ppc_write8(ppc *m, uint32_t a, uint8_t v)
{ if (mem_ok(m, a, 1)) m->mem[a] = v; }

void ppc_write16(ppc *m, uint32_t a, uint16_t v)
{
    if (!mem_ok(m, a, 2)) return;
    m->mem[a] = (uint8_t)(v >> 8); m->mem[a + 1] = (uint8_t)v;
}

void ppc_write32(ppc *m, uint32_t a, uint32_t v)
{
    if (!mem_ok(m, a, 4)) return;
    m->mem[a]     = (uint8_t)(v >> 24); m->mem[a + 1] = (uint8_t)(v >> 16);
    m->mem[a + 2] = (uint8_t)(v >> 8);  m->mem[a + 3] = (uint8_t)v;
}

static uint64_t ppc_read64(ppc *m, uint32_t a)
{
    if (!mem_ok(m, a, 8)) return 0;
    return ((uint64_t)ppc_read32(m, a) << 32) | ppc_read32(m, a + 4);
}

static void ppc_write64(ppc *m, uint32_t a, uint64_t v)
{
    if (!mem_ok(m, a, 8)) return;
    ppc_write32(m, a, (uint32_t)(v >> 32));
    ppc_write32(m, a + 4, (uint32_t)v);
}

/* A single-precision value in guest memory, widened to double. The guest's
 * registers are all double internally, which is what PowerPC does too: `lfs`
 * converts on the way in and `stfs` converts back on the way out. */
static double ld_f32(ppc *m, uint32_t a)
{
    union { uint32_t u; float f; } c;
    c.u = ppc_read32(m, a);
    return (double)c.f;
}

static void st_f32(ppc *m, uint32_t a, double d)
{
    union { uint32_t u; float f; } c;
    c.f = (float)d;
    ppc_write32(m, a, c.u);
}

/* ------------------------------------------------- condition and carry bits */

/* CR field 0, as the `Rc` bit of an arithmetic instruction sets it: less than,
 * greater than, equal, then summary overflow copied from XER. */
static void set_cr0(ppc *m, uint32_t v)
{
    uint32_t f = 0;
    if ((int32_t)v < 0)      f |= 8;
    else if ((int32_t)v > 0) f |= 4;
    else                     f |= 2;
    if (m->xer & PPC_XER_SO) f |= 1;
    m->cr = (m->cr & ~(0xFu << 28)) | (f << 28);
}

static void set_ca(ppc *m, int carry)
{ m->xer = carry ? (m->xer | PPC_XER_CA) : (m->xer & ~PPC_XER_CA); }

/* An add that reports carry the way PowerPC defines it. */
static uint32_t add_carry(ppc *m, uint32_t a, uint32_t b, uint32_t cin, int wantca)
{
    uint64_t r = (uint64_t)a + b + cin;
    if (wantca) set_ca(m, (r >> 32) != 0);
    return (uint32_t)r;
}

static uint32_t cr_field(ppc *m, int n)
{ return (m->cr >> (28 - 4 * n)) & 0xF; }

static void cr_set_field(ppc *m, int n, uint32_t f)
{ m->cr = (m->cr & ~(0xFu << (28 - 4 * n))) | ((f & 0xF) << (28 - 4 * n)); }

/* A single CR bit, numbered from the most significant end as the architecture
 * does: bit 0 of the CR is 0x80000000. */
static int cr_bit(ppc *m, int i)
{ return (int)((m->cr >> (31 - i)) & 1); }

static void cr_put(ppc *m, int i, int v)
{
    uint32_t msk = 1u << (31 - i);
    m->cr = v ? (m->cr | msk) : (m->cr & ~msk);
}

/* ------------------------------------------------------------ bit twiddling */

/* PowerPC counts bits from the most significant end: bit 0 is 0x80000000. The
 * rotate-and-mask instructions are defined in those terms, and translating them
 * to the other convention is the single most error-prone part of the encoding. */
static uint32_t rotl32(uint32_t v, int n)
{ n &= 31; return n ? ((v << n) | (v >> (32 - n))) : v; }

static uint32_t mask_mb_me(int mb, int me)
{
    uint32_t m;
    if (mb <= me) {
        /* A contiguous run from mb to me inclusive. */
        m = (me - mb == 31) ? 0xFFFFFFFFu
                            : (((1u << (me - mb + 1)) - 1) << (31 - me));
    } else {
        /* mb > me wraps: the run runs off the end and continues from bit 0. */
        m = ~mask_mb_me(me + 1, mb - 1);
    }
    return m;
}

/* ------------------------------------------------------------------ decoding */

#define OPCD(i)  ((int)((i) >> 26))
#define RT(i)    ((int)(((i) >> 21) & 31))
#define RA(i)    ((int)(((i) >> 16) & 31))
#define RB(i)    ((int)(((i) >> 11) & 31))
#define RC(i)    ((int)((i) & 1))
#define OE(i)    ((int)(((i) >> 10) & 1))
#define XO(i)    ((int)(((i) >> 1) & 0x3FF))
#define SIMM(i)  ((int32_t)(int16_t)((i) & 0xFFFF))
#define UIMM(i)  ((uint32_t)((i) & 0xFFFF))
#define SH(i)    ((int)(((i) >> 11) & 31))
#define MB(i)    ((int)(((i) >> 6) & 31))
#define ME(i)    ((int)(((i) >> 1) & 31))
#define BO(i)    ((int)(((i) >> 21) & 31))
#define BI(i)    ((int)(((i) >> 16) & 31))
#define LK(i)    ((int)((i) & 1))
#define AA(i)    ((int)(((i) >> 1) & 1))

/* Floating-point registers sit in the same instruction fields as the general
 * ones; only the third source operand (for the multiply-add forms) is new. */
#define FRT(i)   RT(i)
#define FRA(i)   RA(i)
#define FRB(i)   RB(i)
#define FRC(i)   ((int)(((i) >> 6) & 31))
#define XOA(i)   ((int)(((i) >> 1) & 31))     /* the A-form sub-opcode */
#define BF(i)    ((int)(((i) >> 23) & 7))
#define BT(i)    ((int)(((i) >> 21) & 31))    /* a single CR bit, as a target */
#define BA(i)    ((int)(((i) >> 16) & 31))
#define BB(i)    ((int)(((i) >> 11) & 31))

static void unimplemented(ppc *m, uint32_t insn, const char *what)
{
    snprintf(m->err, sizeof m->err,
             "unimplemented instruction 0x%08x at 0x%08x (%s)",
             insn, m->pc, what);
    m->stopped = 1;
}

/* Effective address for a D-form load/store. */
static uint32_t ea_d(ppc *m, uint32_t insn)
{ return (RA(insn) ? m->r[RA(insn)] : 0) + (uint32_t)SIMM(insn); }

/* ...and for an X-form (indexed) one. */
static uint32_t ea_x(ppc *m, uint32_t insn)
{ return (RA(insn) ? m->r[RA(insn)] : 0) + m->r[RB(insn)]; }

/* Should this conditional branch be taken? BO encodes both a counter test and a
 * condition test, either of which may be disabled. */
static int branch_taken(ppc *m, uint32_t insn)
{
    int bo = BO(insn), bi = BI(insn);
    int ctr_ok, cond_ok;

    if (!(bo & 4)) {                       /* decrement and test CTR */
        m->ctr--;
        ctr_ok = (bo & 2) ? (m->ctr == 0) : (m->ctr != 0);
    } else {
        ctr_ok = 1;
    }
    if (!(bo & 16)) {                      /* test a CR bit */
        int bit = (m->cr >> (31 - bi)) & 1;
        cond_ok = (bo & 8) ? bit : !bit;
    } else {
        cond_ok = 1;
    }
    return ctr_ok && cond_ok;
}

/* --------------------------------------------------------------- execution */

/* One instruction. Returns 0 to carry on, non-zero when the interpreter should
 * stop (a fault, or a call out to the host). */
static int step(ppc *m)
{
    uint32_t insn;
    uint32_t next;
    int op;

    if (m->stopped) return 1;

    /* A call into the trap window is a call into the host. */
    if (m->trap_count && m->pc >= m->trap_base &&
        m->pc < m->trap_base + 4 * m->trap_count) {
        uint32_t idx = (m->pc - m->trap_base) / 4;
        if (!m->hostcall) {
            snprintf(m->err, sizeof m->err,
                     "guest called host slot %u with no handler installed", idx);
            m->stopped = 1;
            return 1;
        }
        m->hostcall(m, idx);
        m->icount++;
        if (m->stopped) return 1;
        m->pc = m->lr & ~3u;               /* return, as a subroutine would */
        return 0;
    }

    insn = ppc_read32(m, m->pc);
    next = m->pc + 4;
    op = OPCD(insn);

    switch (op) {
    case 14:                                            /* addi  / li     */
        m->r[RT(insn)] = (RA(insn) ? m->r[RA(insn)] : 0) + (uint32_t)SIMM(insn);
        break;
    case 15:                                            /* addis / lis    */
        m->r[RT(insn)] = (RA(insn) ? m->r[RA(insn)] : 0) +
                         ((uint32_t)UIMM(insn) << 16);
        break;
    case 12:                                            /* addic          */
        m->r[RT(insn)] = add_carry(m, m->r[RA(insn)], (uint32_t)SIMM(insn), 0, 1);
        break;
    case 13:                                            /* addic.         */
        m->r[RT(insn)] = add_carry(m, m->r[RA(insn)], (uint32_t)SIMM(insn), 0, 1);
        set_cr0(m, m->r[RT(insn)]);
        break;
    case 7:                                             /* mulli          */
        /* Done unsigned on purpose. PowerPC defines these to keep the low 32
         * bits and discard the rest, which for signed C operands is overflow --
         * undefined behaviour, not wrapping. Guest code overflows them
         * deliberately: any hash or random-number generator does. Unsigned
         * arithmetic wraps by definition and gives the same bits. */
        m->r[RT(insn)] = m->r[RA(insn)] * (uint32_t)SIMM(insn);
        break;
    case 8:                                             /* subfic         */
        m->r[RT(insn)] = add_carry(m, ~m->r[RA(insn)], (uint32_t)SIMM(insn), 1, 1);
        break;

    case 10: {                                          /* cmpli          */
        uint32_t a = m->r[RA(insn)], b = UIMM(insn);
        cr_set_field(m, (int)((insn >> 23) & 7),
                     (a < b ? 8 : a > b ? 4 : 2) | ((m->xer & PPC_XER_SO) ? 1 : 0));
        break; }
    case 11: {                                          /* cmpi           */
        int32_t a = (int32_t)m->r[RA(insn)], b = SIMM(insn);
        cr_set_field(m, (int)((insn >> 23) & 7),
                     (a < b ? 8 : a > b ? 4 : 2) | ((m->xer & PPC_XER_SO) ? 1 : 0));
        break; }

    case 24: m->r[RA(insn)] = m->r[RT(insn)] | UIMM(insn); break;         /* ori  */
    case 25: m->r[RA(insn)] = m->r[RT(insn)] | (UIMM(insn) << 16); break; /* oris */
    case 26: m->r[RA(insn)] = m->r[RT(insn)] ^ UIMM(insn); break;         /* xori */
    case 27: m->r[RA(insn)] = m->r[RT(insn)] ^ (UIMM(insn) << 16); break; /* xoris*/
    case 28: m->r[RA(insn)] = m->r[RT(insn)] & UIMM(insn);                /* andi.*/
             set_cr0(m, m->r[RA(insn)]); break;
    case 29: m->r[RA(insn)] = m->r[RT(insn)] & (UIMM(insn) << 16);        /* andis.*/
             set_cr0(m, m->r[RA(insn)]); break;

    case 20: {                                          /* rlwimi         */
        uint32_t msk = mask_mb_me(MB(insn), ME(insn));
        uint32_t rot = rotl32(m->r[RT(insn)], SH(insn));
        m->r[RA(insn)] = (rot & msk) | (m->r[RA(insn)] & ~msk);
        if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
        break; }
    case 21: {                                          /* rlwinm         */
        uint32_t msk = mask_mb_me(MB(insn), ME(insn));
        m->r[RA(insn)] = rotl32(m->r[RT(insn)], SH(insn)) & msk;
        if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
        break; }
    case 23: {                                          /* rlwnm          */
        uint32_t msk = mask_mb_me(MB(insn), ME(insn));
        m->r[RA(insn)] = rotl32(m->r[RT(insn)], (int)(m->r[RB(insn)] & 31)) & msk;
        if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
        break; }

    /* loads and stores, D-form */
    case 32: m->r[RT(insn)] = ppc_read32(m, ea_d(m, insn)); break;        /* lwz  */
    case 33: { uint32_t a = ea_d(m, insn);                               /* lwzu */
               m->r[RT(insn)] = ppc_read32(m, a); m->r[RA(insn)] = a; break; }
    case 34: m->r[RT(insn)] = ppc_read8(m, ea_d(m, insn)); break;         /* lbz  */
    case 35: { uint32_t a = ea_d(m, insn);                               /* lbzu */
               m->r[RT(insn)] = ppc_read8(m, a); m->r[RA(insn)] = a; break; }
    case 36: ppc_write32(m, ea_d(m, insn), m->r[RT(insn)]); break;        /* stw  */
    case 37: { uint32_t a = ea_d(m, insn);                               /* stwu */
               ppc_write32(m, a, m->r[RT(insn)]); m->r[RA(insn)] = a; break; }
    case 38: ppc_write8(m, ea_d(m, insn), (uint8_t)m->r[RT(insn)]); break;/* stb  */
    case 39: { uint32_t a = ea_d(m, insn);                               /* stbu */
               ppc_write8(m, a, (uint8_t)m->r[RT(insn)]); m->r[RA(insn)] = a; break; }
    case 40: m->r[RT(insn)] = ppc_read16(m, ea_d(m, insn)); break;        /* lhz  */
    case 42: m->r[RT(insn)] =                                             /* lha  */
             (uint32_t)(int32_t)(int16_t)ppc_read16(m, ea_d(m, insn)); break;
    case 44: ppc_write16(m, ea_d(m, insn), (uint16_t)m->r[RT(insn)]); break; /* sth */

    /* floating-point loads and stores, D-form. `lfs`/`stfs` convert between
     * single and the double the registers actually hold; `lfd`/`stfd` move the
     * bits untouched, which is what makes the fctiwz-then-stfd idiom work. */
    case 48: m->f[FRT(insn)].d = ld_f32(m, ea_d(m, insn)); break;         /* lfs  */
    case 49: { uint32_t a = ea_d(m, insn);                               /* lfsu */
               m->f[FRT(insn)].d = ld_f32(m, a); m->r[RA(insn)] = a; break; }
    case 50: m->f[FRT(insn)].u = ppc_read64(m, ea_d(m, insn)); break;     /* lfd  */
    case 51: { uint32_t a = ea_d(m, insn);                               /* lfdu */
               m->f[FRT(insn)].u = ppc_read64(m, a); m->r[RA(insn)] = a; break; }
    case 52: st_f32(m, ea_d(m, insn), m->f[FRT(insn)].d); break;          /* stfs */
    case 53: { uint32_t a = ea_d(m, insn);                               /* stfsu*/
               st_f32(m, a, m->f[FRT(insn)].d); m->r[RA(insn)] = a; break; }
    case 54: ppc_write64(m, ea_d(m, insn), m->f[FRT(insn)].u); break;     /* stfd */
    case 55: { uint32_t a = ea_d(m, insn);                               /* stfdu*/
               ppc_write64(m, a, m->f[FRT(insn)].u); m->r[RA(insn)] = a; break; }

    case 59:                                            /* single-precision */
    case 63: {                                          /* double + the rest */
        int single = (op == 59);
        double a = m->f[FRA(insn)].d, b = m->f[FRB(insn)].d, c = m->f[FRC(insn)].d;
        double v;
        int have = 1;

        /* The A-form sub-opcode is five bits. None of its values collide with
         * the low five bits of the X-form opcodes handled below, so it is safe
         * to try it first and fall through when it does not match. */
        switch (XOA(insn)) {
        case 18: v = a / b;                    break;   /* fdiv   */
        case 20: v = a - b;                    break;   /* fsub   */
        case 21: v = a + b;                    break;   /* fadd   */
        case 22: v = sqrt(a);                  break;   /* fsqrt  */
        case 23: v = (a >= 0.0 && !isnan(a)) ? c : b;   /* fsel   */
                 break;
        case 24: v = 1.0 / b;                  break;   /* fres   */
        case 25: v = a * c;                    break;   /* fmul   */
        case 26: v = 1.0 / sqrt(b);            break;   /* frsqrte*/
        case 28: v = a * c - b;                break;   /* fmsub  */
        case 29: v = a * c + b;                break;   /* fmadd  */
        case 30: v = -(a * c - b);             break;   /* fnmsub */
        case 31: v = -(a * c + b);             break;   /* fnmadd */
        default: have = 0; v = 0.0;            break;
        }

        if (have) {
            /* The single-precision forms round to float before storing, and the
             * rounding is observable -- skipping it makes a plug-in's arithmetic
             * quietly more accurate than the code was written against. */
            m->f[FRT(insn)].d = single ? (double)(float)v : v;
            if (RC(insn)) cr_set_field(m, 1, 0);
            break;
        }
        if (op == 59) { unimplemented(m, insn, "opcode 59"); return 1; }

        switch (XO(insn)) {
        case 0: case 32: {                              /* fcmpu / fcmpo  */
            uint32_t f;
            if (isnan(a) || isnan(b)) f = 1;            /* unordered      */
            else if (a < b)          f = 8;
            else if (a > b)          f = 4;
            else                     f = 2;
            cr_set_field(m, BF(insn), f);
            break; }
        case 12: m->f[FRT(insn)].d = (double)(float)b; break;      /* frsp  */
        case 14: case 15: {                             /* fctiw / fctiwz */
            /* Truncate or round to a 32-bit integer, saturating rather than
             * wrapping, and leave it in the low word where `stfd` will put it
             * somewhere a `lwz` can reach. */
            double r = (XO(insn) == 15) ? trunc(b) : nearbyint(b);
            int32_t i32;
            if (isnan(r))                 i32 = 0;
            else if (r >= 2147483647.0)   i32 = 2147483647;
            else if (r <= -2147483648.0)  i32 = -2147483647 - 1;
            else                          i32 = (int32_t)r;
            m->f[FRT(insn)].u = (uint32_t)i32;
            break; }
        case 40: m->f[FRT(insn)].d = -b;               break;      /* fneg  */
        case 72: m->f[FRT(insn)].d = b;                break;      /* fmr   */
        case 136: m->f[FRT(insn)].d = -fabs(b);        break;      /* fnabs */
        case 264: m->f[FRT(insn)].d = fabs(b);         break;      /* fabs  */
        case 583: m->f[FRT(insn)].u = m->fpscr;        break;      /* mffs  */
        case 711: {                                     /* mtfsf          */
            int fm = (int)((insn >> 17) & 0xFF), k;
            uint32_t src = (uint32_t)m->f[FRB(insn)].u;
            for (k = 0; k < 8; k++)
                if (fm & (1 << (7 - k))) {
                    uint32_t msk = 0xFu << (28 - 4 * k);
                    m->fpscr = (m->fpscr & ~msk) | (src & msk);
                }
            break; }
        case 38: m->fpscr |=  (1u << (31 - BF(insn) * 4)); break;  /* mtfsb1 */
        case 70: m->fpscr &= ~(1u << (31 - BF(insn) * 4)); break;  /* mtfsb0 */
        case 64: cr_set_field(m, BF(insn),                        /* mcrfs  */
                              (m->fpscr >> (28 - 4 * ((int)(insn >> 18) & 7))) & 0xF);
                 break;
        default: unimplemented(m, insn, "opcode 63"); return 1;
        }
        if (RC(insn)) cr_set_field(m, 1, 0);
        break; }

    case 46: {                                          /* lmw            */
        uint32_t a = ea_d(m, insn);
        int r;
        for (r = RT(insn); r < 32; r++, a += 4) m->r[r] = ppc_read32(m, a);
        break; }
    case 47: {                                          /* stmw           */
        uint32_t a = ea_d(m, insn);
        int r;
        for (r = RT(insn); r < 32; r++, a += 4) ppc_write32(m, a, m->r[r]);
        break; }

    case 18: {                                          /* b / bl / ba    */
        int32_t li = (int32_t)((insn & 0x03FFFFFC) << 6) >> 6;   /* sign extend */
        uint32_t target = AA(insn) ? (uint32_t)li : m->pc + (uint32_t)li;
        if (LK(insn)) m->lr = next;
        next = target;
        break; }
    case 16: {                                          /* bc             */
        int32_t bd = (int32_t)(int16_t)(insn & 0xFFFC);
        uint32_t target = AA(insn) ? (uint32_t)bd : m->pc + (uint32_t)bd;
        int taken = branch_taken(m, insn);
        if (LK(insn)) m->lr = next;
        if (taken) next = target;
        break; }
    case 19:                                            /* bclr / bcctr   */
        switch (XO(insn)) {
        case 16: {                                      /* bclr  (blr)    */
            int taken = branch_taken(m, insn);
            uint32_t target = m->lr & ~3u;
            if (LK(insn)) m->lr = next;
            if (taken) next = target;
            break; }
        case 528: {                                     /* bcctr (bctr)   */
            int taken = branch_taken(m, insn);
            uint32_t target = m->ctr & ~3u;
            if (LK(insn)) m->lr = next;
            if (taken) next = target;
            break; }
        case 0: {                                       /* mcrf           */
            cr_set_field(m, (int)((insn >> 23) & 7), cr_field(m, (int)((insn >> 18) & 7)));
            break; }
        /* The condition-register logical operations, which combine two single CR
         * bits into a third. A compiler emits these for short-circuit `&&` and
         * `||` over floating-point comparisons, so they are not exotic. */
        case 33: case 129: case 193: case 225:
        case 257: case 289: case 417: case 449: {
            int a = cr_bit(m, BA(insn)), b = cr_bit(m, BB(insn)), v;
            switch (XO(insn)) {
            case 257: v =  (a && b);  break;            /* crand          */
            case 449: v =  (a || b);  break;            /* cror           */
            case 193: v =  (a != b);  break;            /* crxor          */
            case 225: v = !(a && b);  break;            /* crnand         */
            case 33:  v = !(a || b);  break;            /* crnor          */
            case 289: v =  (a == b);  break;            /* creqv          */
            case 129: v =  (a && !b); break;            /* crandc         */
            default:  v =  (a || !b); break;            /* crorc  (417)   */
            }
            cr_put(m, BT(insn), v);
            break; }
        case 150: break;                                /* isync -- no-op */
        default: unimplemented(m, insn, "opcode 19"); return 1;
        }
        break;

    case 31:                                            /* the X-form pile */
        switch (XO(insn)) {
        case 266: case 10:                              /* add / addc     */
            m->r[RT(insn)] = add_carry(m, m->r[RA(insn)], m->r[RB(insn)], 0,
                                      XO(insn) == 10);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 40:                                        /* subf           */
            m->r[RT(insn)] = m->r[RB(insn)] - m->r[RA(insn)];
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 8:                                         /* subfc          */
            m->r[RT(insn)] = add_carry(m, ~m->r[RA(insn)], m->r[RB(insn)], 1, 1);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 138:                                       /* adde           */
            m->r[RT(insn)] = add_carry(m, m->r[RA(insn)], m->r[RB(insn)],
                                       (m->xer & PPC_XER_CA) ? 1 : 0, 1);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 104:                                       /* neg            */
            /* Unsigned: negating the most negative value is overflow in C, and
             * PowerPC defines it to produce that same value back. */
            m->r[RT(insn)] = ~m->r[RA(insn)] + 1u;
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 235:                                       /* mullw          */
            /* Unsigned for the same reason as mulli: the low 32 bits of a
             * signed product are the low 32 bits of the unsigned one. */
            m->r[RT(insn)] = m->r[RA(insn)] * m->r[RB(insn)];
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 491: {                                     /* divw           */
            int32_t a = (int32_t)m->r[RA(insn)], b = (int32_t)m->r[RB(insn)];
            /* PowerPC leaves the result undefined rather than trapping; pick 0
             * so a divide by zero cannot take the host down. */
            m->r[RT(insn)] = (b == 0 || (a == INT32_MIN && b == -1))
                             ? 0 : (uint32_t)(a / b);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break; }
        case 459: {                                     /* divwu          */
            uint32_t b = m->r[RB(insn)];
            m->r[RT(insn)] = b ? m->r[RA(insn)] / b : 0;
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break; }
        case 28:  m->r[RA(insn)] = m->r[RT(insn)] & m->r[RB(insn)];        /* and */
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 444: m->r[RA(insn)] = m->r[RT(insn)] | m->r[RB(insn)];        /* or  */
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 316: m->r[RA(insn)] = m->r[RT(insn)] ^ m->r[RB(insn)];        /* xor */
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 476: m->r[RA(insn)] = ~(m->r[RT(insn)] & m->r[RB(insn)]);     /* nand*/
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 124: m->r[RA(insn)] = ~(m->r[RT(insn)] | m->r[RB(insn)]);     /* nor */
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 60:  m->r[RA(insn)] = m->r[RT(insn)] & ~m->r[RB(insn)];       /* andc*/
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 954: m->r[RA(insn)] =                                        /* extsb*/
                  (uint32_t)(int32_t)(int8_t)m->r[RT(insn)];
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 922: m->r[RA(insn)] =                                        /* extsh*/
                  (uint32_t)(int32_t)(int16_t)m->r[RT(insn)];
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 24: {                                      /* slw            */
            uint32_t n = m->r[RB(insn)] & 63;
            m->r[RA(insn)] = n < 32 ? (m->r[RT(insn)] << n) : 0;
            if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break; }
        case 536: {                                     /* srw            */
            uint32_t n = m->r[RB(insn)] & 63;
            m->r[RA(insn)] = n < 32 ? (m->r[RT(insn)] >> n) : 0;
            if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break; }
        case 792: {                                     /* sraw           */
            uint32_t n = m->r[RB(insn)] & 63;
            int32_t v = (int32_t)m->r[RT(insn)];
            if (n > 31) n = 31;
            set_ca(m, v < 0 && (v & ((1u << n) - 1)) != 0);
            m->r[RA(insn)] = (uint32_t)(v >> n);
            if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break; }
        case 824: {                                     /* srawi          */
            int n = SH(insn);
            int32_t v = (int32_t)m->r[RT(insn)];
            set_ca(m, v < 0 && (v & ((1u << n) - 1)) != 0);
            m->r[RA(insn)] = (uint32_t)(v >> n);
            if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break; }
        case 0: {                                       /* cmp            */
            int32_t a = (int32_t)m->r[RA(insn)], b = (int32_t)m->r[RB(insn)];
            cr_set_field(m, (int)((insn >> 23) & 7),
                         (a < b ? 8 : a > b ? 4 : 2) |
                         ((m->xer & PPC_XER_SO) ? 1 : 0));
            break; }
        case 32: {                                      /* cmpl           */
            uint32_t a = m->r[RA(insn)], b = m->r[RB(insn)];
            cr_set_field(m, (int)((insn >> 23) & 7),
                         (a < b ? 8 : a > b ? 4 : 2) |
                         ((m->xer & PPC_XER_SO) ? 1 : 0));
            break; }
        /* indexed loads and stores */
        case 23:  m->r[RT(insn)] = ppc_read32(m, ea_x(m, insn)); break;    /* lwzx */
        case 87:  m->r[RT(insn)] = ppc_read8(m, ea_x(m, insn)); break;     /* lbzx */
        case 279: m->r[RT(insn)] = ppc_read16(m, ea_x(m, insn)); break;    /* lhzx */
        case 343: m->r[RT(insn)] =                                         /* lhax */
                  (uint32_t)(int32_t)(int16_t)ppc_read16(m, ea_x(m, insn)); break;
        case 151: ppc_write32(m, ea_x(m, insn), m->r[RT(insn)]); break;    /* stwx */
        case 215: ppc_write8(m, ea_x(m, insn), (uint8_t)m->r[RT(insn)]); break;/* stbx*/
        case 407: ppc_write16(m, ea_x(m, insn), (uint16_t)m->r[RT(insn)]); break;/* sthx*/
        /* floating-point loads and stores, indexed */
        case 535: m->f[FRT(insn)].d = ld_f32(m, ea_x(m, insn)); break;   /* lfsx  */
        case 567: { uint32_t a = ea_x(m, insn);                          /* lfsux */
                    m->f[FRT(insn)].d = ld_f32(m, a); m->r[RA(insn)] = a; break; }
        case 599: m->f[FRT(insn)].u = ppc_read64(m, ea_x(m, insn)); break;/* lfdx */
        case 631: { uint32_t a = ea_x(m, insn);                          /* lfdux */
                    m->f[FRT(insn)].u = ppc_read64(m, a); m->r[RA(insn)] = a; break; }
        case 663: st_f32(m, ea_x(m, insn), m->f[FRT(insn)].d); break;    /* stfsx */
        case 695: { uint32_t a = ea_x(m, insn);                          /* stfsux*/
                    st_f32(m, a, m->f[FRT(insn)].d); m->r[RA(insn)] = a; break; }
        case 727: ppc_write64(m, ea_x(m, insn), m->f[FRT(insn)].u); break;/* stfdx*/
        case 759: { uint32_t a = ea_x(m, insn);                          /* stfdux*/
                    ppc_write64(m, a, m->f[FRT(insn)].u); m->r[RA(insn)] = a; break; }
        /* special registers. Only the three that compiled code touches. */
        case 339:                                       /* mfspr          */
            switch (((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5)) {
            case 1:  m->r[RT(insn)] = m->xer; break;
            case 8:  m->r[RT(insn)] = m->lr;  break;
            case 9:  m->r[RT(insn)] = m->ctr; break;
            default: unimplemented(m, insn, "mfspr of an unmodelled register");
                     return 1;
            }
            break;
        case 467:                                       /* mtspr          */
            switch (((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5)) {
            case 1:  m->xer = m->r[RT(insn)]; break;
            case 8:  m->lr  = m->r[RT(insn)]; break;
            case 9:  m->ctr = m->r[RT(insn)]; break;
            default: unimplemented(m, insn, "mtspr of an unmodelled register");
                     return 1;
            }
            break;
        case 19: m->r[RT(insn)] = m->cr; break;         /* mfcr           */
        case 144: {                                     /* mtcrf          */
            uint32_t fxm = (insn >> 12) & 0xFF, msk = 0;
            int i;
            for (i = 0; i < 8; i++) if (fxm & (0x80 >> i)) msk |= 0xFu << (28 - 4 * i);
            m->cr = (m->cr & ~msk) | (m->r[RT(insn)] & msk);
            break; }
        case 11:                                        /* mulhwu         */
            m->r[RT(insn)] = (uint32_t)(((uint64_t)m->r[RA(insn)] *
                                         (uint64_t)m->r[RB(insn)]) >> 32);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 75:                                        /* mulhw          */
            m->r[RT(insn)] = (uint32_t)(((int64_t)(int32_t)m->r[RA(insn)] *
                                         (int64_t)(int32_t)m->r[RB(insn)]) >> 32);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 26: {                                      /* cntlzw         */
            uint32_t v = m->r[RT(insn)];
            int n = 0;
            while (n < 32 && !(v & 0x80000000u)) { n++; v <<= 1; }
            m->r[RA(insn)] = (uint32_t)n;
            if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
            break; }
        case 284: m->r[RA(insn)] = ~(m->r[RT(insn)] ^ m->r[RB(insn)]);     /* eqv */
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;
        case 412: m->r[RA(insn)] = m->r[RT(insn)] | ~m->r[RB(insn)];       /* orc */
                  if (RC(insn)) set_cr0(m, m->r[RA(insn)]);
                  break;

        /* The extended-arithmetic forms, which carry XER[CA] in and out. Each is
         * an add of three things, which is how the architecture defines them. */
        case 136:                                       /* subfe          */
            m->r[RT(insn)] = add_carry(m, ~m->r[RA(insn)], m->r[RB(insn)],
                                       (m->xer & PPC_XER_CA) ? 1 : 0, 1);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 200:                                       /* subfze         */
            m->r[RT(insn)] = add_carry(m, ~m->r[RA(insn)], 0,
                                       (m->xer & PPC_XER_CA) ? 1 : 0, 1);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 232:                                       /* subfme         */
            m->r[RT(insn)] = add_carry(m, ~m->r[RA(insn)], 0xFFFFFFFFu,
                                       (m->xer & PPC_XER_CA) ? 1 : 0, 1);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 202:                                       /* addze          */
            m->r[RT(insn)] = add_carry(m, m->r[RA(insn)], 0,
                                       (m->xer & PPC_XER_CA) ? 1 : 0, 1);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;
        case 234:                                       /* addme          */
            m->r[RT(insn)] = add_carry(m, m->r[RA(insn)], 0xFFFFFFFFu,
                                       (m->xer & PPC_XER_CA) ? 1 : 0, 1);
            if (RC(insn)) set_cr0(m, m->r[RT(insn)]);
            break;

        /* Indexed loads and stores that also write the address back. */
        case 55:  { uint32_t a = ea_x(m, insn);                          /* lwzux */
                    m->r[RT(insn)] = ppc_read32(m, a); m->r[RA(insn)] = a; break; }
        case 119: { uint32_t a = ea_x(m, insn);                          /* lbzux */
                    m->r[RT(insn)] = ppc_read8(m, a);  m->r[RA(insn)] = a; break; }
        case 311: { uint32_t a = ea_x(m, insn);                          /* lhzux */
                    m->r[RT(insn)] = ppc_read16(m, a); m->r[RA(insn)] = a; break; }
        case 375: { uint32_t a = ea_x(m, insn);                          /* lhaux */
                    m->r[RT(insn)] = (uint32_t)(int32_t)(int16_t)ppc_read16(m, a);
                    m->r[RA(insn)] = a; break; }
        case 183: { uint32_t a = ea_x(m, insn);                          /* stwux */
                    ppc_write32(m, a, m->r[RT(insn)]); m->r[RA(insn)] = a; break; }
        case 247: { uint32_t a = ea_x(m, insn);                          /* stbux */
                    ppc_write8(m, a, (uint8_t)m->r[RT(insn)]);
                    m->r[RA(insn)] = a; break; }
        case 439: { uint32_t a = ea_x(m, insn);                          /* sthux */
                    ppc_write16(m, a, (uint16_t)m->r[RT(insn)]);
                    m->r[RA(insn)] = a; break; }

        /* The byte-reversed forms exist precisely to read little-endian data on a
         * big-endian machine, so these swap relative to every other access. */
        case 534: { uint32_t v = ppc_read32(m, ea_x(m, insn));           /* lwbrx */
                    m->r[RT(insn)] = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
                                     ((v >> 8) & 0xFF00) | (v >> 24);
                    break; }
        case 662: { uint32_t v = m->r[RT(insn)];                         /* stwbrx*/
                    ppc_write32(m, ea_x(m, insn),
                                ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
                                ((v >> 8) & 0xFF00) | (v >> 24));
                    break; }
        case 790: { uint32_t v = ppc_read16(m, ea_x(m, insn));           /* lhbrx */
                    m->r[RT(insn)] = ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
                    break; }
        case 918: { uint32_t v = m->r[RT(insn)];                         /* sthbrx*/
                    ppc_write16(m, ea_x(m, insn),
                                (uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)));
                    break; }

        /* The reservation pair. There is one thread here, so a reservation can
         * never be lost and the store always succeeds. */
        case 20: m->r[RT(insn)] = ppc_read32(m, ea_x(m, insn)); break;   /* lwarx */
        case 150:                                                       /* stwcx.*/
            ppc_write32(m, ea_x(m, insn), m->r[RT(insn)]);
            cr_set_field(m, 0, 2 | ((m->xer & PPC_XER_SO) ? 1 : 0));
            break;

        case 983:                                       /* stfiwx         */
            ppc_write32(m, ea_x(m, insn), (uint32_t)m->f[FRT(insn)].u);
            break;

        case 512:                                       /* mcrxr          */
            cr_set_field(m, BF(insn), (m->xer >> 28) & 0xF);
            m->xer &= ~(PPC_XER_SO | PPC_XER_OV | PPC_XER_CA);
            break;

        case 4:                                         /* tw             */
            /* A compiler emits this for a check it expects never to fire, so
             * reaching one means the guest has decided something is wrong. */
            snprintf(m->err, sizeof m->err,
                     "the guest trapped (tw) at 0x%08x", m->pc);
            m->stopped = 1;
            return 1;

        case 598: break;                                /* sync  -- no-op */
        case 982: break;                                /* icbi  -- no-op */
        case 54:  break;                                /* dcbst -- no-op */
        case 86:  break;                                /* dcbf  -- no-op */
        case 278: break;                                /* dcbt  -- no-op */
        case 246: break;                                /* dcbtst-- no-op */
        case 470: break;                                /* dcbi  -- no-op */
        case 1014: {                                    /* dcbz           */
            /* This one is not a no-op: it zeroes a cache line, and code that
             * uses it to clear memory depends on that. */
            uint32_t a = ea_x(m, insn) & ~31u;
            int k;
            for (k = 0; k < 32; k += 4) ppc_write32(m, a + (uint32_t)k, 0);
            break; }
        default:
            unimplemented(m, insn, "opcode 31 extended");
            return 1;
        }
        break;

    default:
        unimplemented(m, insn, "primary opcode");
        return 1;
    }

    m->pc = next;
    m->icount++;
    return 0;
}

/* ------------------------------------------------------------------- driver */

ppc *ppc_new(uint32_t memsize)
{
    ppc *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->mem = calloc(1, memsize ? memsize : 1);
    if (!m->mem) { free(m); return NULL; }
    m->memsize = memsize;
    return m;
}

void ppc_free(ppc *m)
{ if (m) { free(m->mem); free(m); } }

/* Run until the guest stops, faults, reaches `until`, or burns the budget.
 *
 * The budget is not optional: a twenty-year-old binary given a subtly wrong
 * environment will loop forever, and a host that hangs is worse than one that
 * reports a runaway. Returns the number of instructions executed. */
int ppc_step(ppc *m) { return step(m); }

uint64_t ppc_run(ppc *m, uint32_t until, uint64_t budget)
{
    uint64_t start = m->icount;

    m->stopped = 0;
    m->err[0] = 0;
    while (!m->stopped) {
        if (until && m->pc == until) break;
        if (budget && m->icount - start >= budget) {
            snprintf(m->err, sizeof m->err,
                     "ran %llu instructions without stopping -- runaway guest",
                     (unsigned long long)(m->icount - start));
            m->stopped = 1;
            break;
        }
        if (step(m)) break;
    }
    return m->icount - start;
}
