/* Naming an address in this host's own code.
 *
 * A guest that faults inside a shim reports as "eip 0x56688027 (host)", which
 * says the plug-in reached this loader and nothing about where. The interesting
 * half is always which stub it was in: a wrong stdcall arity, a float read as a
 * pointer, an out-parameter never written -- each of those lands in a
 * particular function, and the name of that function is the whole diagnosis.
 *
 * dladdr cannot give it. Every stub here is `static`, so none of them is in the
 * dynamic symbol table; the names live in .symtab, which is in the executable
 * on disk and not mapped at run time. So this reads /proc/self/exe once at
 * start-up and keeps the function symbols, which makes the lookup itself a
 * search over memory the process already owns -- safe to do from inside a
 * signal handler, where opening and parsing an ELF file would not be.
 *
 * A stripped binary has no .symtab and hostsym() then finds nothing, which is
 * the behaviour without this file at all.
 */
#ifndef PELOAD_HOSTSYM_H
#define PELOAD_HOSTSYM_H

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uintptr_t addr; uintptr_t size; const char *name; } hostsym_ent;

static hostsym_ent *g_hostsym;
static int          g_nhostsym;
static char        *g_hostsym_strs;

static int hostsym_bias_cb(struct dl_phdr_info *info, size_t sz, void *out)
{
    (void)sz;
    /* The first object with an empty name is the main executable; its
     * dlpi_addr is the offset every st_value has to be shifted by under PIE. */
    if (info->dlpi_name && !info->dlpi_name[0]) {
        *(uintptr_t *)out = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}

static void hostsym_init(void)
{
    uintptr_t bias = 0;
    struct stat st;
    uint8_t *f = NULL;
    ElfW(Ehdr) *eh;
    ElfW(Shdr) *sh;
    ElfW(Sym) *syms = NULL;
    const char *strs = NULL;
    size_t nsym = 0, strsz = 0;
    int fd, i, keep = 0;

    if (g_hostsym) return;
    dl_iterate_phdr(hostsym_bias_cb, &bias);
    if ((fd = open("/proc/self/exe", O_RDONLY)) < 0) return;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof(ElfW(Ehdr))) { close(fd); return; }
    f = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) return;

    eh = (ElfW(Ehdr) *)f;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || !eh->e_shoff) goto done;
    sh = (ElfW(Shdr) *)(f + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        if (sh[i].sh_link >= eh->e_shnum) continue;
        syms  = (ElfW(Sym) *)(f + sh[i].sh_offset);
        nsym  = sh[i].sh_entsize ? sh[i].sh_size / sh[i].sh_entsize : 0;
        strs  = (const char *)(f + sh[sh[i].sh_link].sh_offset);
        strsz = sh[sh[i].sh_link].sh_size;
        break;
    }
    if (!syms || !nsym) goto done;

    /* Two passes: count the functions, then copy them and their names out of
     * the mapping so nothing here holds the file open. */
    for (i = 0; (size_t)i < nsym; i++)
        if ((syms[i].st_info & 0xf) == STT_FUNC && syms[i].st_value && syms[i].st_size)
            keep++;
    if (!keep) goto done;
    g_hostsym = calloc((size_t)keep, sizeof *g_hostsym);
    g_hostsym_strs = malloc(strsz ? strsz : 1);
    if (!g_hostsym || !g_hostsym_strs) { free(g_hostsym); g_hostsym = NULL; goto done; }
    memcpy(g_hostsym_strs, strs, strsz);
    for (i = 0; (size_t)i < nsym && g_nhostsym < keep; i++) {
        if ((syms[i].st_info & 0xf) != STT_FUNC || !syms[i].st_value || !syms[i].st_size)
            continue;
        if (syms[i].st_name >= strsz) continue;
        g_hostsym[g_nhostsym].addr = (uintptr_t)syms[i].st_value + bias;
        g_hostsym[g_nhostsym].size = (uintptr_t)syms[i].st_size;
        g_hostsym[g_nhostsym].name = g_hostsym_strs + syms[i].st_name;
        g_nhostsym++;
    }
done:
    if (f && f != MAP_FAILED) munmap(f, (size_t)st.st_size);
}

/* "winstub_lookup+0x2c" for an address inside one of this host's functions,
 * or 0 if it is not in any of them. Reads nothing but memory it already has. */
static int hostsym(uintptr_t addr, char *out, size_t n)
{
    int i;
    for (i = 0; i < g_nhostsym; i++) {
        if (addr < g_hostsym[i].addr || addr >= g_hostsym[i].addr + g_hostsym[i].size)
            continue;
        if (addr == g_hostsym[i].addr) snprintf(out, n, "%s", g_hostsym[i].name);
        else snprintf(out, n, "%s+0x%lx", g_hostsym[i].name,
                      (unsigned long)(addr - g_hostsym[i].addr));
        return 1;
    }
    return 0;
}

#endif /* PELOAD_HOSTSYM_H */
