/* Where a slow plug-in is spending its time.
 *
 * A plug-in that takes twenty seconds to open its editor is doing something,
 * and nothing here could say what: the call counters in win32gui.h report two
 * repaints and one blit, which rules out the drawing and leaves everything
 * else. The question is always the same one -- is the time inside the plug-in,
 * or inside a shim it is calling far too often -- and a periodic look at the
 * program counter answers it.
 *
 * ITIMER_PROF, so the clock only runs while this process is on a CPU: a plug-in
 * blocked on a socket does not fill the samples with the wait. The handler
 * stores a program counter and returns, which is the whole of what it may do
 * safely; the naming and counting happen at the end.
 *
 * Host addresses are named through hostsym.h. Guest addresses cannot be named
 * -- a plug-in carries no symbols this can read -- so they are grouped by the
 * mapping they fall in and reported as an offset, which is enough to point at
 * the function in a disassembler and, more importantly, enough to say that the
 * time is the plug-in's rather than ours.
 *
 * PELOAD_PROFILE=1 turns it on.
 */
#ifndef PELOAD_HOSTPROF_H
#define PELOAD_HOSTPROF_H

#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <dlfcn.h>

#include "hostsym.h"

#define HOSTPROF_MAX 400000

static uintptr_t *g_prof_pc;
static volatile long g_prof_n;
static int g_prof_on;

static void hostprof_tick(int sig, siginfo_t *si, void *uc)
{
    uintptr_t pc;
    long i;
    (void)sig; (void)si;
#if defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
    pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_EIP];
#else
    pc = 0;
#endif
    i = g_prof_n;
    if (i < HOSTPROF_MAX) { g_prof_pc[i] = pc; g_prof_n = i + 1; }
}

static void hostprof_start(void)
{
    struct sigaction sa;
    struct itimerval it;

    if (g_prof_on || !getenv("PELOAD_PROFILE")) return;
    if (!(g_prof_pc = calloc(HOSTPROF_MAX, sizeof *g_prof_pc))) return;
    hostsym_init();

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = hostprof_tick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL)) return;

    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;                  /* 1 kHz */
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL)) return;
    g_prof_on = 1;
}

/* One mapping, for naming the guest samples. */
typedef struct { uintptr_t lo, hi; char name[64]; } hostprof_map;

static int hostprof_maps(hostprof_map *m, int max)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int n = 0;
    if (!f) return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8], path[256];
        int got = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &lo, &hi, perm, path);
        if (got < 3 || perm[2] != 'x') continue;    /* only executable mappings */
        m[n].lo = (uintptr_t)lo;
        m[n].hi = (uintptr_t)hi;
        snprintf(m[n].name, sizeof m[n].name, "%s",
                 got >= 4 ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path)
                          : "[the plug-in's image]");
        n++;
    }
    fclose(f);
    return n;
}

static void hostprof_report(void)
{
    enum { MAXMAP = 256, MAXBUCKET = 512, GRAIN = 0x1000 };
    static hostprof_map maps[MAXMAP];
    static struct { char label[96]; unsigned long hits; } b[MAXBUCKET];
    int nmap, nb = 0, i, j;
    long s, total = g_prof_n;

    if (!g_prof_on || total <= 0) return;
    /* Stop first: the report itself must not be sampled into its own result. */
    { struct itimerval off; memset(&off, 0, sizeof off); setitimer(ITIMER_PROF, &off, NULL); }
    g_prof_on = 0;
    nmap = hostprof_maps(maps, MAXMAP);

    for (s = 0; s < total; s++) {
        uintptr_t pc = g_prof_pc[s];
        char label[96];
        char sym[96];

        if (hostsym(pc, sym, sizeof sym)) {
            /* By function, not by instruction: the offset within it is noise. */
            char *plus = strchr(sym, '+');
            if (plus) *plus = 0;
            snprintf(label, sizeof label, "%s  (host)", sym);
        } else {
            const hostprof_map *m = NULL;
            Dl_info di;
            /* A shared library does have a dynamic symbol table, and dladdr
             * reads it: "memcpy" says far more than "libc.so.6+0x14e000". */
            if (dladdr((void *)pc, &di) && di.dli_sname && di.dli_fname) {
                const char *base = strrchr(di.dli_fname, '/');
                snprintf(label, sizeof label, "%s  (%s)", di.dli_sname,
                         base ? base + 1 : di.dli_fname);
                goto have_label;
            }
            for (i = 0; i < nmap; i++)
                if (pc >= maps[i].lo && pc < maps[i].hi) { m = &maps[i]; break; }
            if (m)
                snprintf(label, sizeof label, "%s+0x%lx", m->name,
                         (unsigned long)((pc - m->lo) & ~(uintptr_t)(GRAIN - 1)));
            else
                snprintf(label, sizeof label, "0x%lx", (unsigned long)pc);
        }
have_label:
        for (j = 0; j < nb; j++)
            if (!strcmp(b[j].label, label)) { b[j].hits++; break; }
        if (j == nb && nb < MAXBUCKET) {
            snprintf(b[nb].label, sizeof b[nb].label, "%s", label);
            b[nb].hits = 1;
            nb++;
        }
    }

    /* Selection sort over the top few; the tail is not worth ordering. */
    fprintf(stderr, "\nprofile: %ld sample(s) at 1 kHz of CPU time\n", total);
    for (i = 0; i < 15 && i < nb; i++) {
        int best = i;
        for (j = i + 1; j < nb; j++)
            if (b[j].hits > b[best].hits) best = j;
        if (best != i) {
            char tl[96]; unsigned long th;
            memcpy(tl, b[i].label, sizeof tl); th = b[i].hits;
            memcpy(b[i].label, b[best].label, sizeof b[i].label); b[i].hits = b[best].hits;
            memcpy(b[best].label, tl, sizeof b[best].label); b[best].hits = th;
        }
        if (!b[i].hits) break;
        fprintf(stderr, "  %5.1f%%  %8lu  %s\n",
                100.0 * (double)b[i].hits / (double)total, b[i].hits, b[i].label);
    }
    if (total >= HOSTPROF_MAX)
        fprintf(stderr, "  (the sample buffer filled; this is the first %d ms of CPU)\n",
                HOSTPROF_MAX);
}

#endif /* PELOAD_HOSTPROF_H */
