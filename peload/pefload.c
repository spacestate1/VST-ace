/* Load a PEF container into a PowerPC guest.
 *
 * PEF is the Code Fragment Manager's executable format: the thing a Classic Mac
 * OS or Carbon plug-in actually is. Three parts matter here.
 *
 * Sections. A PEF has a code section, a data section (possibly compressed with a
 * pattern language), and a loader section describing everything else. They are
 * placed at addresses this loader chooses, not addresses in the file -- a PEF is
 * position independent and expects to be told where it landed.
 *
 * Relocations. The data section is full of addresses that need fixing up, and PEF
 * expresses those as a compact bytecode rather than a table. The interpreter for
 * that bytecode is the fiddliest part of this file, and an unrecognised opcode
 * stops the load rather than being skipped -- a half-relocated image runs for a
 * while and then does something inexplicable.
 *
 * Imports, and what a TVector is. On PowerPC, a pointer to a function is not the
 * address of its code: it is the address of a two-word structure holding the code
 * address and the value the callee wants in r2 (its table of contents). That pair
 * is a TVector, and every imported and exported function here is one. Getting it
 * wrong means calling code with somebody else's globals.
 *
 * All PEF fields are big-endian.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pefload.h"

/* Where things go in the guest's address space; the layout is in pefload.h so a
 * caller can size its memory to match. Zero is deliberately left unused so a
 * null dereference lands somewhere obviously wrong. */
#define GUEST_CODE   PEF_CODE
#define GUEST_DATA   PEF_DATA
#define GUEST_HEAP   PEF_HEAP

static char g_err[256];
const char *pef_last_error(void) { return g_err; }

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

/* ------------------------------------------------------------ big-endian reads */

static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((p[0] << 8) | p[1]); }

/* ----------------------------------------------------------- section unpacking */

/* A pattern-initialised data section is a little bytecode of its own. There are
 * five opcodes, and the shape of an instruction is always the same: a first byte
 * holding the opcode and, in its low five bits, the first argument -- or zero,
 * meaning that argument follows as a variable-length value like the rest.
 *
 * The thing to get right is where the *data* sits relative to the arguments: all
 * of the arguments come first, and the bytes to be copied follow them. Reading a
 * block before its own repeat count, or letting the address of a shared block
 * drift as later blocks are consumed, produces a section that is the right length
 * and the wrong contents -- which then surfaces as the fragment misbehaving a
 * long way from here. */
static int unpack_pattern(const uint8_t *src, uint32_t srclen,
                          uint8_t *dst, uint32_t dstlen, uint32_t *produced)
{
    uint32_t si = 0, di = 0;

/* A variable-length count: seven bits at a time, big end first, high bit set
 * meaning another byte follows. */
#define VARINT(out) do {                                                       \
        uint8_t c_;                                                            \
        (out) = 0;                                                             \
        do {                                                                   \
            if (si >= srclen) return fail("pattern data ran out mid-count");    \
            c_ = src[si++];                                                    \
            (out) = ((out) << 7) | (uint32_t)(c_ & 0x7F);                       \
        } while (c_ & 0x80);                                                   \
    } while (0)

    while (si < srclen) {
        uint8_t b = src[si++];
        uint32_t op = b >> 5, cnt = b & 0x1F;
        uint32_t i;

        if (cnt == 0) VARINT(cnt);

        switch (op) {
        case 0:                                     /* zero `cnt` bytes       */
            if (di + cnt > dstlen) return fail("pattern zero overruns section");
            memset(dst + di, 0, cnt);
            di += cnt;
            break;

        case 1:                                     /* copy `cnt` bytes       */
            if (si + cnt > srclen || di + cnt > dstlen)
                return fail("pattern copy overruns");
            memcpy(dst + di, src + si, cnt);
            si += cnt; di += cnt;
            break;

        case 2: {                                   /* repeat a block         */
            /* blockSize is `cnt`, repeatCount follows it, and only then the
             * block's bytes. The count is an *additional* repeat count, so the
             * block is written repeatCount + 1 times. */
            uint32_t rep;
            VARINT(rep);
            if (si + cnt > srclen) return fail("pattern repeat overruns source");
            for (i = 0; i <= rep; i++) {
                if (di + cnt > dstlen) return fail("pattern repeat overruns section");
                memcpy(dst + di, src + si, cnt);
                di += cnt;
            }
            si += cnt;
            break; }

        case 3: case 4: {
            /* Interleave a shared block with per-iteration blocks:
             *
             *   3  commonSize, customSize, repeatCount -- the shared block's
             *      bytes are in the stream, followed by repeatCount custom blocks
             *   4  the same, except the shared block is zeros and takes no space
             *
             * The output is common, custom, common, custom, ... , common: the
             * shared part appears repeatCount + 1 times and the custom parts
             * repeatCount times.
             *
             * The argument order is commonSize, *customSize*, repeatCount. Taking
             * the last two the other way round consumes exactly the same number
             * of bytes -- the customs total customSize x repeatCount either way --
             * so the stream still parses to its end and only the output is the
             * wrong length. Checking the result against the section's declared
             * unpackedSize is what catches it. */
            uint32_t custom, rep, k, common_at, cust_at;
            VARINT(custom);
            VARINT(rep);
            common_at = si;
            cust_at = si + (op == 3 ? cnt : 0);
            if (cust_at > srclen) return fail("pattern %u overruns source", op);
            for (k = 0; k <= rep; k++) {
                if (di + cnt > dstlen)
                    return fail("pattern %u overruns section", op);
                if (op == 3) memcpy(dst + di, src + common_at, cnt);
                else         memset(dst + di, 0, cnt);
                di += cnt;
                if (k == rep) break;
                if (cust_at + custom > srclen || di + custom > dstlen)
                    return fail("pattern %u custom part overruns", op);
                memcpy(dst + di, src + cust_at, custom);
                di += custom;
                cust_at += custom;
            }
            si = cust_at;
            break; }

        default:
            return fail("unknown pattern opcode %u", op);
        }
    }
    if (produced) *produced = di;
    return 0;
#undef VARINT
}

/* --------------------------------------------------------------- relocations */

/* The relocation bytecode. It walks a cursor `rAddr` through the section being
 * relocated, adding a section's base or an imported symbol's address to each word
 * it visits. `sectionC` and `sectionD` are the code and data bases it may add. */
static int run_relocs(pef *p, const uint8_t *ins, uint32_t nbytes,
                      uint32_t sect_base, uint32_t sect_len)
{
    uint32_t rAddr = sect_base;             /* the cursor being relocated     */
    uint32_t importIndex = 0;               /* the next import a run will use */
    uint32_t sectionC = p->code_base;       /* what "code section" means here */
    uint32_t sectionD = p->data_base;       /* ...and "data section"          */
    uint32_t i = 0;
    ppc *m = p->cpu;
    /* The repeat opcodes loop over the blocks just executed. `rpt_at` remembers
     * which repeat instruction is currently running so that arriving at it again
     * after the loop drains does not re-arm it. */
    uint32_t rpt_at = 0xFFFFFFFFu, rpt_left = 0;

#define NEED(n) do { if ((uint64_t)rAddr + (n) > (uint64_t)sect_base + sect_len) \
                        return fail("relocation at 0x%08x writes past the section", \
                                    rAddr); } while (0)
#define ADDTO(a, d) do { NEED(4); ppc_write32(m, (a), ppc_read32(m, (a)) + (d)); } while (0)

    /* Opcode field widths vary and getting one wrong desynchronises everything
     * after it. The top three bits select the family:
     *
     *   000, 001   RelocBySectDWithSkip -- a 2-bit opcode, then an 8-bit skip
     *              and a 6-bit count. The widest field is the skip, which is why
     *              reading it as 6+8 walks the cursor off the end of the section.
     *   010, 011   the run and "small" forms -- a 7-bit opcode and a 9-bit
     *              count or index.
     *   100        RelocIncrPosition and RelocSmRepeat -- a 4-bit opcode. Note
     *              that these have the top bit set: a relocation word is not
     *              required to begin with a zero.
     *   101        the "large" forms -- a 6-bit opcode and a 26-bit operand
     *              spanning two blocks.
     */
    while (i + 1 < nbytes) {
        uint16_t w = rd16(ins + i);
        uint32_t here = i;
        uint32_t op7 = (uint32_t)(w >> 9);       /* run and small forms  */
        uint32_t cnt = (uint32_t)(w & 0x1FF) + 1;
        uint32_t idx = (uint32_t)(w & 0x1FF);
        uint32_t k;

        i += 2;

        switch (w >> 13) {
        case 0: case 1: {                        /* RelocBySectDWithSkip */
            uint32_t skip = (w >> 6) & 0xFF, n = w & 0x3F;
            rAddr += 4 * skip;
            for (k = 0; k < n; k++) { ADDTO(rAddr, sectionD); rAddr += 4; }
            continue; }

        case 2:                                  /* the run forms        */
            switch (op7) {
            case 0x20:                           /* RelocBySectC         */
                for (k = 0; k < cnt; k++) { ADDTO(rAddr, sectionC); rAddr += 4; }
                continue;
            case 0x21:                           /* RelocBySectD         */
                for (k = 0; k < cnt; k++) { ADDTO(rAddr, sectionD); rAddr += 4; }
                continue;
            case 0x22:                           /* RelocTVector12       */
                for (k = 0; k < cnt; k++) {
                    ADDTO(rAddr, sectionC); ADDTO(rAddr + 4, sectionD);
                    rAddr += 12;                 /* the third word is reserved */
                }
                continue;
            case 0x23:                           /* RelocTVector8        */
                for (k = 0; k < cnt; k++) {
                    ADDTO(rAddr, sectionC); ADDTO(rAddr + 4, sectionD);
                    rAddr += 8;
                }
                continue;
            case 0x24:                           /* RelocVTable8         */
                for (k = 0; k < cnt; k++) { ADDTO(rAddr, sectionD); rAddr += 8; }
                continue;
            case 0x25:                           /* RelocImportRun       */
                for (k = 0; k < cnt; k++) {
                    if (importIndex >= p->nimports)
                        return fail("a run of imports reached %u, past the %u this "
                                    "fragment declares", importIndex, p->nimports);
                    ADDTO(rAddr, p->imports[importIndex].addr);
                    importIndex++;
                    rAddr += 4;
                }
                continue;
            default:
                return fail("unknown run relocation 0x%04x at offset %u", w, here);
            }

        case 3:                                  /* the small forms      */
            switch (op7) {
            case 0x30:                           /* RelocSmByImport      */
                if (idx >= p->nimports)
                    return fail("import %u named, only %u declared", idx, p->nimports);
                ADDTO(rAddr, p->imports[idx].addr);
                importIndex = idx + 1;
                rAddr += 4;
                continue;
            case 0x31:                           /* RelocSmSetSectC      */
                if (idx >= p->nsections) return fail("section %u out of range", idx);
                sectionC = p->sect_addr[idx];
                continue;
            case 0x32:                           /* RelocSmSetSectD      */
                if (idx >= p->nsections) return fail("section %u out of range", idx);
                sectionD = p->sect_addr[idx];
                continue;
            case 0x33:                           /* RelocSmBySection     */
                if (idx >= p->nsections) return fail("section %u out of range", idx);
                ADDTO(rAddr, p->sect_addr[idx]);
                rAddr += 4;
                continue;
            default:
                return fail("unknown small relocation 0x%04x at offset %u", w, here);
            }

        case 4:
            if ((w >> 12) == 8) {                /* RelocIncrPosition    */
                rAddr += (w & 0xFFF) + 1;
                continue;
            } else {                             /* RelocSmRepeat        */
                uint32_t blocks = ((w >> 8) & 0xF) + 1;
                uint32_t times  = (w & 0xFF) + 1;
                if (rpt_at != here) { rpt_at = here; rpt_left = times; }
                if (rpt_left) {
                    uint32_t back = 2 * blocks + 2;   /* the blocks, and this one */
                    rpt_left--;
                    if (back > i) return fail("a repeat at offset %u reaches back "
                                              "before the stream", here);
                    i -= back;
                } else {
                    rpt_at = 0xFFFFFFFFu;
                }
                continue;
            }

        case 5: {                                /* the large forms      */
            uint32_t op6 = (uint32_t)(w >> 10), lo, arg;
            if (i + 1 >= nbytes)
                return fail("a two-block relocation at offset %u is truncated", here);
            lo = rd16(ins + i); i += 2;
            arg = ((uint32_t)(w & 0x3FF) << 16) | lo;

            switch (op6) {
            case 0x28:                           /* RelocSetPosition     */
                rAddr = sect_base + arg;
                continue;
            case 0x29:                           /* RelocLgByImport      */
                if (arg >= p->nimports)
                    return fail("import %u named, only %u declared", arg, p->nimports);
                ADDTO(rAddr, p->imports[arg].addr);
                importIndex = arg + 1;
                rAddr += 4;
                continue;
            case 0x2C: {                         /* RelocLgRepeat        */
                uint32_t blocks = (arg >> 22) + 1;
                uint32_t times  = (arg & 0x3FFFFF) + 1;
                if (rpt_at != here) { rpt_at = here; rpt_left = times; }
                if (rpt_left) {
                    uint32_t back = 2 * blocks + 4;   /* the blocks, and these two */
                    rpt_left--;
                    if (back > i) return fail("a repeat at offset %u reaches back "
                                              "before the stream", here);
                    i -= back;
                } else {
                    rpt_at = 0xFFFFFFFFu;
                }
                continue; }
            case 0x2D: {                         /* RelocLgSetOrBySection */
                uint32_t sub = (arg >> 22) & 0xF, s = arg & 0x3FFFFF;
                if (s >= p->nsections) return fail("section %u out of range", s);
                if (sub == 0)      { ADDTO(rAddr, p->sect_addr[s]); rAddr += 4; }
                else if (sub == 1) sectionC = p->sect_addr[s];
                else if (sub == 2) sectionD = p->sect_addr[s];
                else return fail("unknown large set relocation %u at offset %u",
                                 sub, here);
                continue; }
            default:
                return fail("unknown large relocation 0x%04x at offset %u", w, here);
            }
        }

        default:                                 /* 110 and 111 are reserved */
            return fail("reserved relocation word 0x%04x at offset %u", w, here);
        }
    }
#undef ADDTO
#undef NEED
    return 0;
}

/* ------------------------------------------------------------------- loading */

pef *pef_load(const uint8_t *file, uint32_t len, ppc *cpu,
              const char **import_names, uint32_t max_imports)
{
    pef *p;
    uint32_t nsec, i, loader_off = 0, loader_len = 0;
    uint32_t trap = PPC_TRAP_BASE;

    g_err[0] = 0;
    if (len < 40 || memcmp(file, "Joy!peff", 8))
        { fail("not a PEF container"); return NULL; }
    if (memcmp(file + 8, "pwpc", 4))
        { fail("PEF is for '%.4s', not PowerPC", file + 8); return NULL; }

    /* Catch an undersized guest here rather than as a puzzling stray access the
     * first time the fragment pushes a stack frame. */
    if (cpu->memsize < PEF_MEMSIZE) {
        fail("the guest has %u bytes of memory but this layout needs %u",
             cpu->memsize, PEF_MEMSIZE);
        return NULL;
    }

    if (!(p = calloc(1, sizeof *p))) { fail("out of memory"); return NULL; }
    p->cpu = cpu;
    nsec = rd16(file + 32);
    if (nsec > PEF_MAX_SECTIONS) { fail("%u sections", nsec); free(p); return NULL; }
    p->nsections = nsec;

    /* First pass: place the instantiated sections and copy them in. */
    for (i = 0; i < nsec; i++) {
        const uint8_t *sh = file + 40 + i * 28;
        uint32_t total = rd32(sh + 8), unpacked = rd32(sh + 12);
        uint32_t packed = rd32(sh + 16), coff = rd32(sh + 20);
        uint8_t kind = sh[24];
        uint32_t base = 0;

        if ((uint64_t)coff + packed > len)
            { fail("section %u runs past the file", i); free(p); return NULL; }

        switch (kind) {
        case 0: base = p->code_base ? p->code_base : GUEST_CODE; break; /* code */
        case 1: case 2: case 3:                                    /* data etc */
            base = p->data_base ? p->data_base
                                : GUEST_DATA + ((p->data_used + 0xFFFFu) & ~0xFFFFu);
            break;
        case 4: loader_off = coff; loader_len = packed; continue;  /* loader   */
        default: continue;                                         /* debug    */
        }

        if (kind == 0) { p->code_base = base; p->code_len = total; }
        else if (!p->data_base) { p->data_base = base; }

        p->sect_addr[i] = base;
        p->sect_len[i] = total;

        if ((uint64_t)base + total > cpu->memsize)
            { fail("section %u does not fit in guest memory", i); free(p); return NULL; }

        if (kind == 2) {                        /* pattern-initialised data */
            uint8_t *tmp = calloc(1, total ? total : 1);
            uint32_t got = 0;
            if (!tmp) { fail("out of memory"); free(p); return NULL; }
            if (unpack_pattern(file + coff, packed, tmp, total, &got)) {
                free(tmp); free(p); return NULL;
            }
            /* The section says how much initialised data it holds, so check it.
             * A pattern stream can be misread in ways that still consume every
             * byte and still end tidily -- the length is the only thing that
             * notices, and without this the fragment simply behaves oddly later. */
            if (unpacked && got != unpacked) {
                fail("section %u unpacked to %u bytes, but declares %u",
                     i, got, unpacked);
                free(tmp); free(p); return NULL;
            }
            memcpy(cpu->mem + base, tmp, total);
            free(tmp);
        } else {
            uint32_t n = packed < total ? packed : total;
            memcpy(cpu->mem + base, file + coff, n);
            if (total > n) memset(cpu->mem + base + n, 0, total - n);
        }
        if (kind != 0) p->data_used += total;
    }

    if (!loader_len) { fail("no loader section"); free(p); return NULL; }

    /* Second pass: the loader section -- imports, then relocations. */
    {
        const uint8_t *L = file + loader_off;
        int32_t mainSection = (int32_t)rd32(L + 0);
        uint32_t mainOffset = rd32(L + 4);
        uint32_t nlibs = rd32(L + 24), nsyms = rd32(L + 28);
        uint32_t nrelsec = rd32(L + 32), relOff = rd32(L + 36);
        uint32_t strOff = rd32(L + 40);
        uint32_t symtab = 56 + nlibs * 24;

        if (nsyms > max_imports)
            { fail("%u imports, room for %u", nsyms, max_imports); free(p); return NULL; }
        p->nimports = nsyms;

        /* Every imported symbol gets a trap address. A function import is a
         * TVector, so it needs a two-word structure in guest memory whose code
         * word is the trap; a data import is just the address. */
        for (i = 0; i < nsyms; i++) {
            uint32_t v = rd32(L + symtab + i * 4);
            uint32_t nameoff = v & 0xFFFFFF;
            /* The high byte holds flags as well as the class: the class is the
             * low four bits and 0x80 marks a weak import. Comparing the whole
             * byte against the class therefore misses every weak symbol, and a
             * weakly imported function then gets the bare trap address where a
             * TVector should be -- which faults the moment the guest reads it,
             * some way from the actual mistake. */
            uint32_t cls = (v >> 24) & 0x0F;
            const char *nm = (const char *)(L + strOff + nameoff);
            snprintf(p->imports[i].name, sizeof p->imports[i].name, "%s", nm);
            p->imports[i].cls = cls;
            p->imports[i].weak = ((v >> 24) & 0x80) != 0;
            if (import_names) import_names[i] = p->imports[i].name;

            p->imports[i].trap = trap + 4 * i;
            /* A TVector import is referenced by the address of the TVector; a
             * code or data import by the thing itself. */
            p->imports[i].addr = (cls == 2 /* TVector */)
                               ? pef_tvector(p, trap + 4 * i, p->data_base)
                               : trap + 4 * i;
        }
        cpu->trap_base = trap;
        /* The window covers the host's own callbacks as well, so that a TVector
         * we hand the guest is callable the same way an import is. */
        cpu->trap_count = nsyms + PEF_HOST_TRAPS;

        /* Relocations, one instruction stream per relocated section. */
        for (i = 0; i < nrelsec; i++) {
            const uint8_t *rh = L + 56 + nlibs * 24 + nsyms * 4 + i * 12;
            uint16_t sect = rd16(rh);
            uint32_t cnt = rd32(rh + 4), off = rd32(rh + 8);
            if (sect >= nsec) continue;
            if (run_relocs(p, L + relOff + off, cnt * 2,
                           p->sect_addr[sect], p->sect_len[sect])) {
                free(p); return NULL;
            }
        }

        /* The entry point. `mainSection` names a section and `mainOffset` an
         * offset within it, and what lives there is a TVector -- not code. */
        if (mainSection >= 0 && (uint32_t)mainSection < nsec) {
            uint32_t tvaddr = p->sect_addr[mainSection] + mainOffset;
            p->main_tvector = tvaddr;
            p->main_code = ppc_read32(cpu, tvaddr);
            p->main_toc  = ppc_read32(cpu, tvaddr + 4);
        }
    }
    return p;
}

void pef_free(pef *p) { free(p); }

/* --------------------------------------------------------------- TVectors */

uint32_t pef_tvector(pef *p, uint32_t code, uint32_t toc)
{
    uint32_t at;

    if (!p->tvec_next) p->tvec_next = PEF_TVEC;
    if (p->tvec_next + 8 > PEF_HEAP) { fail("out of TVector space"); return 0; }
    at = p->tvec_next;
    p->tvec_next += 8;
    ppc_write32(p->cpu, at, code);
    ppc_write32(p->cpu, at + 4, toc);
    return at;
}

uint32_t pef_host_callback(pef *p, uint32_t slot)
{
    if (slot >= PEF_HOST_TRAPS) { fail("host callback slot %u", slot); return 0; }
    /* The TOC does not matter -- the trap never executes PowerPC code -- but the
     * guest's glue will load r2 from it, so give it something legitimate. */
    return pef_tvector(p, p->cpu->trap_base + 4 * (p->nimports + slot),
                       p->data_base);
}

int pef_host_slot(const pef *p, uint32_t index)
{
    if (index < p->nimports) return -1;
    return (int)(index - p->nimports);
}
