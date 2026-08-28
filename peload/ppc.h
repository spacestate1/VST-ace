/* A 32-bit PowerPC interpreter. See ppc.c for why this exists at all.
 *
 * The guest's memory is a flat block and a guest address is an index into it, so
 * nothing the interpreted code does can reach the host's address space.
 */
#ifndef PELOAD_PPC_H
#define PELOAD_PPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XER bits, in the host's bit numbering. */
#define PPC_XER_SO  0x80000000u        /* summary overflow */
#define PPC_XER_OV  0x40000000u        /* overflow         */
#define PPC_XER_CA  0x20000000u        /* carry            */

struct ppc;

/* A floating-point register, as both a number and the bits behind it. Both
 * matter: `fctiwz` leaves an integer in a floating register, and the only way to
 * get it into a general one is to store it with `stfd` and load the low word
 * back -- which is exactly how compiled PowerPC code converts float to int. Keep
 * only the double and that integer is lost. */
typedef union { double d; uint64_t u; } ppc_fpr;

typedef struct ppc {
    uint32_t  r[32];                   /* general purpose  */
    ppc_fpr   f[32];                   /* floating point   */
    uint32_t  pc, lr, ctr, cr, xer;
    uint32_t  fpscr;

    uint8_t  *mem;                     /* guest memory, big-endian throughout */
    uint32_t  memsize;

    uint64_t  icount;                  /* instructions retired */
    int       stopped;
    char      err[192];

    void     *host;                    /* whatever the embedder needs */

    /* Calls out to the host.
     *
     * An imported function is given a guest address in [trap_base, trap_base +
     * 4*trap_count). Nothing is ever executed there: when the program counter
     * lands in that window the interpreter calls `hostcall` with the slot index
     * and then returns to LR, which is exactly what a real subroutine would have
     * done. That keeps the guest unable to call anything the host did not
     * deliberately expose. */
    uint32_t  trap_base, trap_count;
    void    (*hostcall)(struct ppc *m, uint32_t index);
} ppc;

#define PPC_TRAP_BASE  0x0F000000u

ppc     *ppc_new(uint32_t memsize);
void     ppc_free(ppc *m);

/* Run until the guest stops, faults, reaches `until`, or exceeds `budget`
 * instructions. Returns how many it executed. A budget of 0 means no limit,
 * which is only safe when the guest is known to terminate. */
uint64_t ppc_run(ppc *m, uint32_t until, uint64_t budget);

/* One instruction, for tracing. Returns non-zero once the guest has stopped --
 * either because it faulted or because it called out to the host. Unlike
 * ppc_run() this does not clear `stopped`, so a caller can step up to a fault
 * and then look at `err`. */
int ppc_step(ppc *m);

/* Guest memory, byte-swapped on the way through. */
uint8_t  ppc_read8(ppc *m, uint32_t addr);
uint16_t ppc_read16(ppc *m, uint32_t addr);
uint32_t ppc_read32(ppc *m, uint32_t addr);
void     ppc_write8(ppc *m, uint32_t addr, uint8_t v);
void     ppc_write16(ppc *m, uint32_t addr, uint16_t v);
void     ppc_write32(ppc *m, uint32_t addr, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_PPC_H */
