/* The 64-bit bridge server: host one plugin on behalf of another process.
 *
 * peload32 --serve exists because a 32-bit plugin cannot share an address space
 * with a 64-bit host. This exists for a different reason: crash isolation. A
 * plugin that faults takes its process down, and three in this corpus do --
 * TAL-U-No-62 during render, Ragnarok and OB-Xf during load. In-process there is
 * nothing to be done about that; behind the bridge the host loses a helper and
 * emits silence instead of dying.
 *
 * Much shorter than serve32.h, because it drives pehost rather than the VST2 ABI
 * directly -- so it serves Windows VST2, Windows VST3, native Linux VST3, macOS
 * VST2 and Audio Units without knowing which it has.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <time.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bridge.h"
#include "pehost.h"

typedef struct {
    pehost     *h;
    bridge_shm *sh;
    int         sock;
    pthread_t   audio;
    volatile int stop;
    int         editor_open;
    double      last_publish;      /* ms, CLOCK_MONOTONIC */
    double      last_idle;
} server;

static void audio_rt_priority(void)
{
    /* The caller's realtime thread is blocked waiting on us, so match it. Not
     * fatal if refused -- it only means more jitter. */
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = 20;
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}

/* Drain the parameter and MIDI rings the host writes into. Runs on the audio
 * thread, immediately before render, which is where both have to land. */
static void drain(server *s)
{
    bridge_shm *sh = s->sh;
    uint32_t t, hd;

    t  = atomic_load_explicit(&sh->p_tail, memory_order_relaxed);
    hd = atomic_load_explicit(&sh->p_head, memory_order_acquire);
    for (; t != hd; t++) {
        bridge_param p = sh->pq[t % BRIDGE_PARAMQ];
        pehost_set_param(s->h, p.index, p.value);
    }
    atomic_store_explicit(&sh->p_tail, t, memory_order_release);

    t  = atomic_load_explicit(&sh->m_tail, memory_order_relaxed);
    hd = atomic_load_explicit(&sh->m_head, memory_order_acquire);
    for (; t != hd; t++) {
        bridge_ev e = sh->mq[t % BRIDGE_MIDIQ];
        pehost_midi_at(s->h, e.status, e.d1, e.d2, e.at);
    }
    atomic_store_explicit(&sh->m_tail, t, memory_order_release);
}

static void *audio_thread(void *ud)
{
    server *s = ud;
    bridge_shm *sh = s->sh;

    pehost_thread_init();
    audio_rt_priority();

    while (!s->stop) {
        int n;
        if (bridge_sem_wait(&sh->req, NULL)) break;
        if (sh->abort || s->stop) { bridge_sem_post(&sh->done); break; }

        n = sh->frames;
        if (n < 0) n = 0;
        if (n > BRIDGE_MAX_FRAMES) n = BRIDGE_MAX_FRAMES;

        drain(s);
        pehost_render_io(s->h, sh->in, sh->out, n);
        bridge_sem_post(&sh->done);
    }
    return NULL;
}

/* Deliver whatever editor input the host has written.
 *
 * Called from the main loop and, crucially, from inside the plugin when it spins
 * in its own message loop -- that is the only way a modal drag ever sees the
 * button come up. The depth guard is because delivering an event calls the
 * plugin's wndproc, which may poll again and re-enter here. */
static void drain_input(server *s)
{
    static int depth;
    bridge_shm *sh = s->sh;
    uint32_t t, hd;

    if (depth > 4) return;
    depth++;
    t  = atomic_load_explicit(&sh->in_tail, memory_order_relaxed);
    hd = atomic_load_explicit(&sh->in_head, memory_order_acquire);
    for (; t != hd; t++) {
        bridge_input e = sh->inq[t % BRIDGE_INQ];
        atomic_store_explicit(&sh->in_tail, t + 1, memory_order_release);
        if (e.kind == BRIDGE_IN_KEY) pehost_editor_key(s->h, e.a, e.b, e.c);
        else                         pehost_editor_mouse(s->h, e.a, e.b, e.c, e.d, e.e);
    }
    depth--;
}

static server S;
static server *g_server;
int w32_paint_in_progress(void);

static void publish_pixels(server *s);

/* Called from inside the plugin while it spins in its own loop. Input has to
 * reach it, and -- just as important -- the frames it draws mid-drag have to
 * reach the host. Otherwise the whole drag renders as one jump at the end,
 * because the main loop that normally republishes is parked in this very call. */
static void publish_editor(server *s);

/* Serve one request. Returns non-zero if the host asked us to quit. */
static int handle_request(server *s, const bridge_req *qp, bridge_rep *rp)
{
    bridge_req q = *qp;
    bridge_rep r = *rp;
        switch (q.op) {
        case BR_HELLO:
            snprintf(r.text, sizeof r.text, "%s", pehost_name(s->h));
            snprintf(r.text2, sizeof r.text2, "%s", pehost_vendor(s->h));
            r.a = pehost_num_programs(s->h);
            r.b = pehost_num_params(s->h);
            r.c = pehost_num_inputs(s->h);
            r.d = pehost_num_outputs(s->h);
            /* The flags field only has to carry "is a synth" across. */
            r.e = pehost_is_synth(s->h) ? 0x100 : 0;
            r.g = pehost_unique_id(s->h);
            break;
        case BR_PARAM_NAME:    pehost_param_name(s->h, q.a, r.text, sizeof r.text); break;
        case BR_PARAM_LABEL:   pehost_param_label(s->h, q.a, r.text, sizeof r.text); break;
        case BR_PARAM_DISPLAY: pehost_param_display(s->h, q.a, r.text, sizeof r.text); break;
        case BR_PARAM_GET:     r.f = pehost_get_param(s->h, q.a); break;
        case BR_PROGRAM_NAME:  pehost_program_name(s->h, q.a, r.text, sizeof r.text); break;
        case BR_SET_PROGRAM:   pehost_set_program(s->h, q.a); break;
        case BR_GET_PROGRAM:   r.a = pehost_get_program(s->h); break;
        case BR_EDITOR_KIND:   r.a = pehost_editor_kind(s->h); break;
        case BR_EDITOR_SIZE:   pehost_editor_size(s->h, &r.a, &r.b); break;
        case BR_EDITOR_OPEN:
            if (s->editor_open) { r.a = s->sh->ed_w; r.b = s->sh->ed_h; break; }
            if (pehost_editor_open(s->h)) { r.ok = 0; break; }
            s->editor_open = 1;
            pehost_editor_size(s->h, &r.a, &r.b);
            if (r.a > BRIDGE_MAX_ED_W || r.b > BRIDGE_MAX_ED_H) {
                fprintf(stderr, "peserve: editor %dx%d exceeds the shared buffer\n",
                        r.a, r.b);
                s->editor_open = 0;
                r.ok = 0;
                break;
            }
            s->sh->ed_w = r.a; s->sh->ed_h = r.b;
            publish_editor(s);
            break;
        case BR_EDITOR_CLOSE:
            if (s->editor_open) { pehost_editor_detach(s->h); s->editor_open = 0; }
            break;
        case BR_EDITOR_MOUSE:  /* legacy: the host now writes these to memory */
                               pehost_editor_mouse(s->h, q.a, q.b, q.c, q.d, q.e); break;
        case BR_EDITOR_KEY:    pehost_editor_key(s->h, q.a, q.b, q.c); break;
        case BR_ALL_NOTES_OFF: pehost_all_notes_off(s->h); break;
        case BR_IMPORT_STATS:  pehost_import_stats(&r.a, &r.b, &r.c); break;
        case BR_QUIT:
            return 1;                            /* the caller leaves the loop */
        default:
            r.ok = 0;
            break;
        }
    *rp = r;
    return 0;
}

/* Answer what is safe to answer while the plugin is spinning.
 *
 * A modal drag loop parks this thread inside the plugin, and everything the host
 * asks meanwhile -- parameter values for its UI, mostly -- would otherwise wait
 * out the five-second socket deadline and the window would freeze. Reads are fine
 * to serve re-entrantly; anything structural (opening or closing the editor,
 * changing program, quitting) is peeked at and left for the main loop. */
static int safe_while_spinning(int op)
{
    switch (op) {
    case BR_PARAM_NAME: case BR_PARAM_LABEL: case BR_PARAM_DISPLAY:
    case BR_PARAM_GET:  case BR_PROGRAM_NAME: case BR_GET_PROGRAM:
    case BR_EDITOR_KIND: case BR_EDITOR_SIZE: case BR_IMPORT_STATS:
        return 1;
    default:
        return 0;
    }
}


static void serve_pending(server *s)
{
    int i;
    for (i = 0; i < 32; i++) {                  /* bounded: this is re-entrant */
        bridge_req q;
        bridge_rep r;
        ssize_t n = recv(s->sock, &q, sizeof q, MSG_PEEK | MSG_DONTWAIT);
        if (n != (ssize_t)sizeof q) return;      /* nothing whole waiting */
        if (!safe_while_spinning(q.op)) return;  /* leave it for the main loop */
        if (recv(s->sock, &q, sizeof q, MSG_WAITALL) != (ssize_t)sizeof q) return;
        memset(&r, 0, sizeof r);
        r.ok = 1;
        handle_request(s, &q, &r);
        if (send(s->sock, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r) return;
    }
}

static void on_pump_input(void *ud)
{
    static int inside;
    (void)ud;
    if (!g_server || inside) return;
    inside = 1;
    drain_input(g_server);
    serve_pending(g_server);
    /* Idle it too, rate-limited. A plugin spinning in its own loop still expects
     * the host's idle to come round -- that is where it repaints -- and without
     * it a drag arrives as one jump when the loop finally exits. Re-entrant into
     * the plugin by construction, which is why `inside` guards it. */
    if (g_server->editor_open) {
        struct timespec now;
        double t;
        clock_gettime(CLOCK_MONOTONIC, &now);
        t = (double)now.tv_sec * 1e3 + (double)now.tv_nsec / 1e6;
        if (t - g_server->last_idle > 12.0) {
            g_server->last_idle = t;
            pehost_editor_pump(g_server->h);
        }
    }
    publish_pixels(g_server);
    inside = 0;
}

/* Republish the editor's pixels. The host blits whatever is current rather than
 * asking for a frame, so this only has to keep the buffer fresh. */
/* Rate-limited, because this is called from three places in the loop below --
 * including after every single request. A pump costs a repaint, and a publish
 * costs a copy of the whole framebuffer (nearly a megabyte for a 695x350 editor);
 * doing both per mouse-move means the helper spends its main thread redrawing
 * instead of answering. Thirty a second is smoother than any plugin repaints
 * anyway. */
/* Copy whatever the plugin has drawn. No pump: this is called from inside the
 * plugin as well, and pumping it there would re-enter its own message handling. */
static void publish_pixels(server *s)
{
    const unsigned int *px;
    int w, h;
    bridge_shm *sh = s->sh;
    struct timespec now;
    double t;

    if (!s->editor_open) return;
    /* Never mid-repaint: publishing between BeginPaint and EndPaint captures a
     * frame that is part old and part new. This is called from inside the plugin
     * as well as from the pump, which is exactly when a paint is likely to be
     * open. Skipping costs nothing -- the next tick publishes the whole frame. */
    if (w32_paint_in_progress()) return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    t = (double)now.tv_sec * 1e3 + (double)now.tv_nsec / 1e6;
    if (t - s->last_publish < 12.0) return;              /* ~80 Hz ceiling */
    s->last_publish = t;

    if (!pehost_editor_pixels(s->h, &px, &w, &h) || !px || w <= 0 || h <= 0) return;
    if (w > BRIDGE_MAX_ED_W || h > BRIDGE_MAX_ED_H) return;
    /* A sequence lock around the copy: odd generation means "being written".
     *
     * The host used to read this buffer straight out of shared memory while this
     * memcpy was running, so it saw the top of one frame and the bottom of the
     * next -- controls drawn twice at two positions, and a flicker every time a
     * partly-copied frame landed on screen. The counter existed but nothing
     * excluded a torn read. */
    atomic_fetch_add_explicit(&sh->ed_gen, 1, memory_order_release);
    sh->ed_w = w; sh->ed_h = h;
    memcpy(bridge_pixels(sh), px, (size_t)w * h * 4);
    atomic_fetch_add_explicit(&sh->ed_gen, 1, memory_order_release);
}

static void publish_editor(server *s)
{
    if (!s->editor_open) return;
    pehost_editor_pump(s->h);
    publish_pixels(s);
}


/* Say where it died.
 *
 * A crash in here takes the helper process with it, and the host only sees the
 * socket close -- which reports as "the helper is gone" with nothing to act on.
 * The fault address alone is not much use either, because the plugin is mapped at
 * a base chosen at run time; what identifies the instruction is the offset into
 * the mapping, which /proc/self/maps still has at this point.
 *
 * Written with write() rather than fprintf: this runs on a broken stack. */
static void crash_report(int sig, siginfo_t *si, void *uc)
{
    char line[512];
    unsigned long pc = 0;
    int fd;
    int n;

#if defined(__x86_64__)
    pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
#endif
    n = snprintf(line, sizeof line,
                 "peserve: died on signal %d at pc %#lx, faulting address %p\n",
                 sig, pc, si ? si->si_addr : NULL);
    if (n > 0) { ssize_t w = write(2, line, (size_t)n); (void)w; }

    /* Which mapping the pc is in, and how far into it -- enough to turn into an
     * RVA against the plugin image. */
    if ((fd = open("/proc/self/maps", O_RDONLY)) >= 0) {
        char buf[8192];
        ssize_t got = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (got > 0) {
            char *p = buf;
            buf[got] = 0;
            while (p && *p) {
                unsigned long lo = 0, hi = 0;
                char *nl = strchr(p, '\n');
                if (nl) *nl = 0;
                if (sscanf(p, "%lx-%lx", &lo, &hi) == 2 && pc >= lo && pc < hi) {
                    n = snprintf(line, sizeof line,
                                 "peserve:   pc is +%#lx into [%s]\n", pc - lo, p);
                    if (n > 0) { ssize_t w = write(2, line, (size_t)n); (void)w; }
                    break;
                }
                p = nl ? nl + 1 : NULL;
            }
        }
    }
    /* Back to the default so the exit status still says how it died. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_report(void)
{
    struct sigaction sa;
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    size_t i;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_report;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}

int main(int argc, char **argv)
{
    install_crash_report();
    const char *path = NULL, *shm_name = NULL;
    int i, sock = -1, fd;
    double sr = 48000.0;
    int bs = 512;
    bridge_shm *sh;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--serve") && i + 1 < argc)     sock = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shm") && i + 1 < argc)  shm_name = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) sr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) bs = atoi(argv[++i]);
        else if (argv[i][0] != '-')                          path = argv[i];
    }
    if (!path || sock < 0 || !shm_name) {
        fprintf(stderr, "usage: peserve <plugin> --serve <fd> --shm <name>\n"
                        "                [--rate HZ] [--block N]\n"
                        "\nNot meant to be run by hand: pehost spawns this when a\n"
                        "plugin is to be isolated.\n");
        return 2;
    }

    if ((fd = shm_open(shm_name, O_RDWR, 0600)) < 0) { perror("shm_open"); return 1; }
    sh = mmap(NULL, BRIDGE_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sh == MAP_FAILED) { perror("mmap"); return 1; }
    if (sh->magic != BRIDGE_MAGIC || sh->version != BRIDGE_VERSION) {
        fprintf(stderr, "peserve: shared region is not v%d\n", BRIDGE_VERSION);
        return 1;
    }

    /* Do not outlive the host. A helper whose parent dies would otherwise sit
     * there holding a plugin -- which is exactly what the hung ones did before
     * the input pump existed, and they are invisible once detached. */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) return 0;              /* already orphaned */

    pehost_thread_init();
    if (!(S.h = pehost_open(path, sr, bs))) {
        /* The host is waiting on a reply, so answer before leaving -- otherwise
         * it blocks until its own deadline instead of reporting the real error. */
        bridge_rep r;
        memset(&r, 0, sizeof r);
        snprintf(r.text, sizeof r.text, "%s", pehost_last_error());
        send(sock, &r, sizeof r, MSG_NOSIGNAL);
        fprintf(stderr, "peserve: %s\n", pehost_last_error());
        return 1;
    }
    S.sh = sh;
    S.sock = sock;
    g_server = &S;
    /* So a plugin spinning in its own message loop still receives input. */
    pehost_set_input_pump(on_pump_input, &S);
    sh->nin  = pehost_num_inputs(S.h);
    sh->nout = pehost_num_outputs(S.h);

    if (pthread_create(&S.audio, NULL, audio_thread, &S) != 0) {
        fprintf(stderr, "peserve: cannot start the audio thread\n");
        return 1;
    }

    for (;;) {
        bridge_req q;
        bridge_rep r;
        struct pollfd pfd;

        pfd.fd = sock; pfd.events = POLLIN; pfd.revents = 0;
        if (poll(&pfd, 1, S.editor_open ? 4 : 500) < 0 && errno != EINTR) break;
        drain_input(&S);
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) { publish_editor(&S); continue; }
        if (recv(sock, &q, sizeof q, MSG_WAITALL) != (ssize_t)sizeof q) break;

        memset(&r, 0, sizeof r);
        r.ok = 1;
        { int quit = handle_request(&S, &q, &r);
          if (quit) { send(sock, &r, sizeof r, MSG_NOSIGNAL); goto done; } }
        if (send(sock, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r) break;
        publish_editor(&S);
    }

done:
    S.stop = 1;
    sh->abort = 1;
    bridge_sem_post(&sh->req);
    pthread_join(S.audio, NULL);
    pehost_close(S.h);
    return 0;
}
