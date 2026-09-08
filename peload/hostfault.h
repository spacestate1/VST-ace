/* Say where a 64-bit host died, and in what.
 *
 * The i386 loader has had this since it was written (pe32.c): a plug-in that
 * faults gets an address, the section it is in, the registers and a walk back
 * up the stack. The 64-bit side had it only inside peserve, so a plug-in hosted
 * in process -- which is what the command line does by default -- dropped a
 * core file and said nothing at all.
 *
 * Written with write() rather than fprintf, because this runs on a stack that
 * has already gone wrong. hostsym.h names the function when the pc is in the
 * host's own code, which is the difference between "a shim did this" and "the
 * plug-in did this".
 */
#ifndef PELOAD_HOSTFAULT_H
#define PELOAD_HOSTFAULT_H

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "hostsym.h"

static void hostfault_say(const char *s)
{ ssize_t w = write(2, s, strlen(s)); (void)w; }

/* The executable mappings, which is what makes an address on the stack a return
 * address rather than a number. Read once per fault; the guest's image is in
 * here too, and naming it "the plug-in" is the whole point -- a recursion in the
 * plug-in and a recursion in a shim look identical from the stack otherwise. */
typedef struct { unsigned long lo, hi; char name[48]; } hostfault_map;

static int hostfault_maps(hostfault_map *m, int max)
{
    int fd = open("/proc/self/maps", O_RDONLY), n = 0;
    static char buf[32768];
    ssize_t got;
    char *p;

    if (fd < 0) return 0;
    got = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (got <= 0) return 0;
    buf[got] = 0;
    p = buf;
    while (p && *p && n < max) {
        unsigned long lo = 0, hi = 0;
        char perm[8], path[256];
        char *nl = strchr(p, '\n');
        int f;
        if (nl) *nl = 0;
        path[0] = 0;
        f = sscanf(p, "%lx-%lx %7s %*s %*s %*s %255s", &lo, &hi, perm, path);
        if (f >= 3 && perm[2] == 'x') {
            const char *base = path[0] ? strrchr(path, '/') : NULL;
            m[n].lo = lo;
            m[n].hi = hi;
            snprintf(m[n].name, sizeof m[n].name, "%s",
                     path[0] ? (base ? base + 1 : path) : "the plug-in's image");
            n++;
        }
        p = nl ? nl + 1 : NULL;
    }
    return n;
}

static void hostfault_report(int sig, siginfo_t *si, void *uc)
{
    static hostfault_map maps[192];
    int nmap;
    char line[512], sym[160];
    unsigned long pc = 0;
    int fd, n;

    nmap = hostfault_maps(maps, (int)(sizeof maps / sizeof maps[0]));

#if defined(__x86_64__)
    pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
    pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_EIP];
#else
    (void)uc;
#endif
    n = snprintf(line, sizeof line,
                 "\n*** %s at pc %#lx, faulting address %p\n",
                 sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
                 sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "fault",
                 pc, si ? si->si_addr : NULL);
    if (n > 0) hostfault_say(line);

    if (hostsym((uintptr_t)pc, sym, sizeof sym)) {
        n = snprintf(line, sizeof line, "    in %s -- host code, not the plug-in\n", sym);
        if (n > 0) hostfault_say(line);
    }
    /* A fault within a page or so of the stack pointer, on a write, is the
     * stack running out rather than a bad pointer -- which means recursion
     * that does not stop, and looking at the faulting address for a cause is
     * looking in the wrong place. */
    if (sig == SIGSEGV && si) {
        unsigned long sp = 0, addr = (unsigned long)si->si_addr;
#if defined(__x86_64__)
        sp = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RSP];
#elif defined(__i386__)
        sp = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_ESP];
#endif
        if (sp && addr + 0x10000 > sp && addr < sp + 0x10000)
            hostfault_say("    that is the stack running out, not a bad pointer"
                          " -- suspect recursion with no base case\n");
    }

    /* Which mapping, and how far into it: with the plug-in mapped at a base
     * chosen at run time, the offset is what identifies the instruction. */
    {
        int k;
        for (k = 0; k < nmap; k++)
            if (pc >= maps[k].lo && pc < maps[k].hi) {
                n = snprintf(line, sizeof line, "    +%#lx into %s\n",
                             pc - maps[k].lo, maps[k].name);
                if (n > 0) hostfault_say(line);
                break;
            }
    }
    (void)fd;
    /* Who is recursing.
     *
     * A stack that has run out was filled by something, and the something is
     * almost always one call site repeating. There is no frame pointer to walk
     * on x86-64, so this sweeps the stack for words that land in executable
     * memory -- return addresses -- and counts them. A recursion shows up as
     * one address with a very large count, which is the whole diagnosis: name
     * that address and the loop is found.
     *
     * The sweep starts a little above the faulting stack pointer, because the
     * page it fell off is not readable. */
    if (sig == SIGSEGV) {
        enum { SWEEP = 0x20000, TOP = 24 };
        /* Room for the whole repeating unit, not just its busiest three. */
        static struct { uintptr_t pc; unsigned long hits; } seen[256];
        int nseen = 0, i, j;
        uintptr_t sp = 0, lo, hi, prot_lo, prot_hi;
        uint32_t prot;
#if defined(__x86_64__)
        sp = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RSP];
#elif defined(__i386__)
        sp = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_ESP];
#endif
        (void)prot_lo; (void)prot_hi; (void)prot;
        if (sp) {
            uintptr_t a;
            lo = (sp + 0xFFF) & ~(uintptr_t)0xFFF;      /* past the dead page */
            hi = lo + SWEEP;
            for (a = lo; a + sizeof(uintptr_t) <= hi; a += sizeof(uintptr_t)) {
                uintptr_t v;
                char sym[96];
                memcpy(&v, (const void *)a, sizeof v);
                if (v < 0x1000) continue;
                /* In executable memory, so it is a return address rather than
                 * data -- whether or not anything here can put a name to it. */
                for (j = 0; j < nmap; j++)
                    if (v >= maps[j].lo && v < maps[j].hi) break;
                if (j == nmap) continue;
                for (j = 0; j < nseen; j++)
                    if (seen[j].pc == v) { seen[j].hits++; break; }
                if (j == nseen && nseen < (int)(sizeof seen / sizeof seen[0])) {
                    seen[nseen].pc = v;
                    seen[nseen].hits = 1;
                    nseen++;
                }
            }
            /* The innermost frames in the order they are on the stack. A
             * count says which function is repeating; the order says what it
             * repeats *with*, which is the cycle itself. */
            {
                uintptr_t a2;
                int shown = 0;
                hostfault_say("    innermost stack, outward:\n");
                for (a2 = lo; a2 + sizeof(uintptr_t) <= hi && shown < 12;
                     a2 += sizeof(uintptr_t)) {
                    uintptr_t v;
                    char sym2[96];
                    Dl_info di2;
                    int k;
                    memcpy(&v, (const void *)a2, sizeof v);
                    if (v < 0x1000) continue;
                    for (k = 0; k < nmap; k++)
                        if (v >= maps[k].lo && v < maps[k].hi) break;
                    if (k == nmap) continue;
                    if (!hostsym(v, sym2, sizeof sym2)) {
                        if (dladdr((void *)v, &di2) && di2.dli_sname)
                            snprintf(sym2, sizeof sym2, "%s  (%s)", di2.dli_sname,
                                     maps[k].name);
                        else
                            snprintf(sym2, sizeof sym2, "%s+0x%lx", maps[k].name,
                                     v - maps[k].lo);
                    }
                    n = snprintf(line, sizeof line, "      %s\n", sym2);
                    if (n > 0) hostfault_say(line);
                    shown++;
                }
            }
            for (i = 0; i < 8 && i < nseen; i++) {
                int best = i;
                char sym[96];
                Dl_info di;
                for (j = i + 1; j < nseen; j++)
                    if (seen[j].hits > seen[best].hits) best = j;
                if (best != i) {
                    uintptr_t tp = seen[i].pc; unsigned long th = seen[i].hits;
                    seen[i] = seen[best];
                    seen[best].pc = tp; seen[best].hits = th;
                }
                if (seen[i].hits < 4) break;          /* not a pattern */
                if (!hostsym(seen[i].pc, sym, sizeof sym)) {
                    if (dladdr((void *)seen[i].pc, &di) && di.dli_sname) {
                        snprintf(sym, sizeof sym, "%s", di.dli_sname);
                    } else {
                        int k;
                        snprintf(sym, sizeof sym, "0x%lx", (unsigned long)seen[i].pc);
                        for (k = 0; k < nmap; k++)
                            if (seen[i].pc >= maps[k].lo && seen[i].pc < maps[k].hi) {
                                snprintf(sym, sizeof sym, "%s+0x%lx", maps[k].name,
                                         seen[i].pc - maps[k].lo);
                                break;
                            }
                    }
                }
                n = snprintf(line, sizeof line,
                             "    on the stack %lu time(s): %s\n", seen[i].hits, sym);
                if (n > 0) hostfault_say(line);
            }
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

/* Install for the fatal signals a guest can raise. Call once, early, while
 * there is still a working stack to parse the symbol table on.
 *
 * On an alternate stack, which is the difference between a report and nothing
 * at all for the one fault that is hardest to diagnose: a stack overflow. The
 * handler for it runs on the stack that just overflowed, faults again on the
 * same guard page, and the process dies without a word -- which is exactly what
 * unbounded recursion inside a plug-in looked like from outside. */
static void hostfault_install(void)
{
    /* Not SIGSTKSZ: glibc made it a sysconf() call, so it cannot size an
     * array. 64 KiB is far more than this handler uses. */
    static char altstack[65536];
    stack_t ss;
    struct sigaction sa;
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    size_t i;

    hostsym_init();
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = hostfault_report;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}

#endif /* PELOAD_HOSTFAULT_H */
