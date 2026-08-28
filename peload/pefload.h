/* Load a PEF container -- a Classic Mac OS / Carbon code fragment -- into a
 * PowerPC guest. See pefload.c for what a TVector is and why it matters. */
#ifndef PELOAD_PEFLOAD_H
#define PELOAD_PEFLOAD_H

#include <stdarg.h>
#include <stdint.h>
#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PEF_MAX_SECTIONS 16
#define PEF_MAX_IMPORTS  512

/* Where things live in the guest's flat memory. The stack grows down from
 * PEF_STACK, so everything below it and above the heap belongs to the stack;
 * PEF_MEMSIZE is what a caller must hand to ppc_new() for the layout to fit. */
#define PEF_CODE     0x01000000u
#define PEF_DATA     0x02000000u
#define PEF_TVEC     0x03F00000u   /* TVectors: imports, and our own callbacks */
#define PEF_HEAP     0x04000000u
#define PEF_STACK    0x07FF0000u   /* initial r1, with room to spare above */
#define PEF_MEMSIZE  0x08000000u

/* Trap slots past the imports, for callbacks the host hands to the guest. */
#define PEF_HOST_TRAPS 16

typedef struct {
    char     name[64];
    uint32_t cls;                  /* 0 code, 1 data, 2 TVector, 3 TOC, 4 glue */
    int      weak;                 /* the fragment can run without it          */
    uint32_t trap;                 /* the guest address that calls out to us    */
    uint32_t addr;                 /* what a reference to this symbol becomes   */
} pef_import;

typedef struct {
    ppc      *cpu;
    uint32_t  nsections;
    uint32_t  sect_addr[PEF_MAX_SECTIONS];
    uint32_t  sect_len[PEF_MAX_SECTIONS];
    uint32_t  code_base, code_len;
    uint32_t  data_base, data_used;

    uint32_t  nimports;
    pef_import imports[PEF_MAX_IMPORTS];

    uint32_t  tvec_next;           /* bump pointer into the TVector area */

    /* The entry point is a TVector, so it is two words: where the code is and
     * what the callee wants in r2. */
    uint32_t  main_tvector, main_code, main_toc;
} pef;

pef        *pef_load(const uint8_t *file, uint32_t len, ppc *cpu,
                     const char **import_names, uint32_t max_imports);
void        pef_free(pef *p);
const char *pef_last_error(void);

/* Build a TVector in guest memory and return its address. Anywhere the guest
 * expects a function pointer it expects one of these, not a code address. */
uint32_t    pef_tvector(pef *p, uint32_t code, uint32_t toc);

/* A TVector the guest can call that arrives at `cpu->hostcall`. `slot` is in
 * [0, PEF_HOST_TRAPS); pef_host_slot() turns the index hostcall receives back
 * into a slot, and returns -1 when the index is an imported symbol instead. */
uint32_t    pef_host_callback(pef *p, uint32_t slot);
int         pef_host_slot(const pef *p, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_PEFLOAD_H */
