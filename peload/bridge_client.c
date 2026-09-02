/* Host side of the 32-bit bridge: spawn peload32 --serve and drive it.
 *
 * Presents the same operations pehost.c implements in-process, so pehost can
 * dispatch to either without its callers knowing which. See bridge.h for the
 * protocol and for why the audio path uses shared memory rather than the socket.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bridge.h"
#include "bridge_client.h"

struct bridge {
    pid_t       pid;
    int         sock;
    bridge_shm *sh;
    char        shm_name[64];

    char        name[128], vendor[128];
    int         nprograms, nparams, nin, nout, flags, uid;
    int         ed_w, ed_h, ed_open;
    uint32_t    ed_seen;
    /* A private copy of the last whole frame. The shared buffer cannot be handed
     * out directly: the helper rewrites it continuously, so anything reading it
     * in place sees a mixture of two frames. */
    unsigned int *ed_buf;
    size_t        ed_cap;
    int           ed_bw, ed_bh, ed_have;
    unsigned      ed_torn, ed_reads;   /* how often a read caught a write */
    int         dead;
    double      sr;
    int         bs;
    /* Requests posted whose completion we gave up waiting for. Must reach zero
     * before another is posted -- see bridge_render_io. */
    int         pending;
    int         behind;            /* consecutive blocks with no reply */
    int         reported_dead;
};

static char g_err[256];
const char *bridge_last_error(void) { return g_err; }

/* ------------------------------------------------------------------ helper */

/* peload32 sits next to whichever binary is running, so derive its path from
 * /proc/self/exe rather than trusting the working directory. PELOAD32 overrides,
 * which is what a test harness or an installed layout wants. */
static int bridge_helper_named(const char *helper, char *out, size_t n)
{
    const char *env = getenv(!strcmp(helper, "peload32") ? "PELOAD32" : "PESERVE");
    char exe[4096];
    ssize_t len;
    char *slash;

    int up;

    if (env && *env) { snprintf(out, n, "%s", env); return access(out, X_OK) == 0 ? 0 : -1; }
    if ((len = readlink("/proc/self/exe", exe, sizeof exe - 1)) <= 0) return -1;
    exe[len] = 0;
    if (!(slash = strrchr(exe, '/'))) return -1;
    *slash = 0;

    /* Beside the executable: where peload, peserve and pestudio all sit. */
    snprintf(out, n, "%s/%s", exe, helper);
    if (access(out, X_OK) == 0) return 0;

    /* Then peload/build, walking up.
     *
     * dwstudio is built in a directory of its own, so "beside me" finds
     * nothing -- and the only symptom was that 32-bit plug-ins quietly went
     * missing from its list, with nothing to say the helper was what was
     * absent. Anything else built outside peload/build gets the same benefit. */
    for (up = 0; up < 6; up++) {
        if (!(slash = strrchr(exe, '/'))) break;
        *slash = 0;
        if (!exe[0]) break;
        snprintf(out, n, "%s/peload/build/%s", exe, helper);
        if (access(out, X_OK) == 0) return 0;
    }
    return -1;
}

static int bridge_helper_path(char *out, size_t n)
{ return bridge_helper_named("peload32", out, n); }

/* Can the helper actually run, not merely does the file exist?
 *
 * access(X_OK) says a file is executable; it says nothing about whether its
 * interpreter and libraries are installed. For peload32 that gap is the whole
 * question. It is an i386 binary on an x86-64 host, so on a machine with no
 * 32-bit runtime the file is present and executable and exec still fails --
 * and the host, having been told the bridge was available, reported "the
 * helper died before reporting" rather than "the 32-bit libraries are not
 * installed". One of those is actionable.
 *
 * Deciding this by running it is the only honest answer: reading PT_INTERP
 * would catch a missing loader and not a missing libpipewire, and the set of
 * libraries is not ours to enumerate. `peload32` with no arguments prints its
 * usage and exits, which is a cheap and side-effect-free probe. Done once and
 * remembered, because a plug-in browser asks this question per file.
 *
 * The stderr of a failed exec is worth keeping: the dynamic linker names the
 * library it could not find, and that name is the most useful thing anyone can
 * be told here. */

static int   g_probe_done;
static int   g_probe_ok;
static char  g_probe_why[256];

static void bridge_probe(const char *path)
{
    int  fd[2];
    pid_t pid;
    int  status = 0;
    char buf[256];
    ssize_t got = 0;

    g_probe_done = 1;
    g_probe_ok = 0;
    g_probe_why[0] = 0;

    if (pipe(fd) != 0) {
        /* No pipe: fall back to trusting the file, which is where this
         * started. Better than refusing to bridge over a resource shortage. */
        g_probe_ok = 1;
        return;
    }
    if ((pid = fork()) < 0) { close(fd[0]); close(fd[1]); g_probe_ok = 1; return; }

    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(fd[1], 2);                       /* the linker's complaint */
        if (null >= 0) { dup2(null, 0); dup2(null, 1); }
        close(fd[0]); close(fd[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }

    close(fd[1]);
    got = read(fd[0], buf, sizeof buf - 1);
    close(fd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }

    /* Usage output and a non-zero exit are both fine -- it ran. Only a failure
     * to start at all disqualifies it, which is exec's 127 or a signal. */
    if (WIFEXITED(status) && WEXITSTATUS(status) != 127) {
        g_probe_ok = 1;
        return;
    }

    if (got > 0) {
        char *nl;
        buf[got] = 0;
        if ((nl = strchr(buf, '\n')) != NULL) *nl = 0;
        snprintf(g_probe_why, sizeof g_probe_why, "%s", buf);
    } else {
        snprintf(g_probe_why, sizeof g_probe_why,
                 "%s could not be started", path);
    }
}

/* Why the 32-bit helper is unusable, or NULL when it is fine. */
const char *bridge_unavailable_reason(void)
{
    char p[4096];
    if (getenv("PELOAD_IS_SERVER")) return "already inside the helper";
    if (bridge_helper_path(p, sizeof p) != 0)
        return "peload32 is not installed beside the other programs";
    if (!g_probe_done) bridge_probe(p);
    return g_probe_ok ? NULL : g_probe_why;
}

int bridge_available(void)
{
    /* Present *and* runnable. The file existing was the old test and it was
     * not enough -- see bridge_probe above. */
    return bridge_unavailable_reason() == NULL;
}

int bridge_isolation_available(void)
{
    char p[4096];
    if (getenv("PELOAD_IS_SERVER")) return 0;
    return bridge_helper_named("peserve", p, sizeof p) == 0;
}

/* ------------------------------------------------------------------- setup */

static int bridge_call(bridge *b, const bridge_req *q, bridge_rep *r)
{
    bridge_rep tmp;
    /* Cleared before the call, not after: on a dead socket the caller still
     * reads this, and an uninitialised reply meant a failed load reported
     * itself as whatever was on the stack. */
    if (r) memset(r, 0, sizeof *r);
    memset(&tmp, 0, sizeof tmp);
    if (b->dead) return -1;
    if (send(b->sock, q, sizeof *q, MSG_NOSIGNAL) != (ssize_t)sizeof *q) { b->dead = 1; return -1; }
    if (recv(b->sock, r ? r : &tmp, sizeof tmp, MSG_WAITALL) != (ssize_t)sizeof tmp) {
        /* Distinguish a stall from a death: both end the session for this
         * plugin, but only one of them is worth reporting as a hang. */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr, "bridge: helper stopped answering (op %d) -- "
                            "dropping it rather than hanging the host\n", q->op);
        b->dead = 1;
        return -1;
    }
    return 0;
}

static int bridge_op(bridge *b, int op, int a, int bb, int c, int d, int e, bridge_rep *r)
{
    bridge_req q;
    memset(&q, 0, sizeof q);
    q.op = op; q.a = a; q.b = bb; q.c = c; q.d = d; q.e = e;
    return bridge_call(b, &q, r);
}

void bridge_close(bridge *b)
{
    if (b && b->ed_reads && getenv("PELOAD_VERBOSE"))
        fprintf(stderr, "  [bridge] editor: %u read(s), %u caught the helper "
                        "mid-frame and were retried\n", b->ed_reads, b->ed_torn);
    if (b) { free(b->ed_buf); b->ed_buf = NULL; b->ed_cap = 0; b->ed_have = 0; }
    if (!b) return;
    if (!b->dead) {
        bridge_rep r;
        bridge_op(b, BR_QUIT, 0, 0, 0, 0, 0, &r);
    }
    if (b->sock >= 0) close(b->sock);
    if (b->pid > 0) {
        int st;
        /* It should be leaving on its own after BR_QUIT and the closed socket;
         * give it a moment, then insist. */
        int i;
        for (i = 0; i < 50; i++) {
            if (waitpid(b->pid, &st, WNOHANG) == b->pid) { b->pid = 0; break; }
            { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
        }
        if (b->pid > 0) {
            /* The group, not just the helper: a plug-in that forked a child of
             * its own leaves it behind otherwise. */
            kill(-b->pid, SIGKILL);
            kill(b->pid, SIGKILL);
            waitpid(b->pid, &st, 0);
        }
    }
    if (b->sh) munmap(b->sh, BRIDGE_SHM_SIZE);
    if (b->shm_name[0]) shm_unlink(b->shm_name);
    free(b);
}

bridge *bridge_open(const char *dll, double samplerate, int blocksize)
{ return bridge_open_helper(dll, samplerate, blocksize, "peload32"); }

bridge *bridge_open_helper(const char *dll, double samplerate, int blocksize,
                           const char *helper_name)
{
    bridge *b;
    char helper[4096], fdarg[16];
    int sv[2], fd;
    bridge_rep rep;

    if (!helper_name) helper_name = "peload32";
    if (bridge_helper_named(helper_name, helper, sizeof helper)) {
        snprintf(g_err, sizeof g_err,
                 "%s not found next to this binary", helper_name);
        return NULL;
    }
    if (!(b = calloc(1, sizeof *b))) return NULL;
    b->sock = -1;
    b->sr = samplerate;
    b->bs = blocksize > 0 ? blocksize : 512;

    snprintf(b->shm_name, sizeof b->shm_name, "/peload32-%d-%p", (int)getpid(), (void *)b);
    shm_unlink(b->shm_name);
    if ((fd = shm_open(b->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600)) < 0) {
        snprintf(g_err, sizeof g_err, "shm_open: %s", strerror(errno));
        b->shm_name[0] = 0; bridge_close(b); return NULL;
    }
    if (ftruncate(fd, (off_t)BRIDGE_SHM_SIZE)) {
        snprintf(g_err, sizeof g_err, "ftruncate: %s", strerror(errno));
        close(fd); bridge_close(b); return NULL;
    }
    b->sh = mmap(NULL, BRIDGE_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (b->sh == MAP_FAILED) {
        snprintf(g_err, sizeof g_err, "mmap: %s", strerror(errno));
        b->sh = NULL; bridge_close(b); return NULL;
    }
    memset(b->sh, 0, sizeof *b->sh);
    b->sh->magic = BRIDGE_MAGIC;
    b->sh->version = BRIDGE_VERSION;
    /* The futex-word semaphores start at zero, which memset already did. */

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) {
        snprintf(g_err, sizeof g_err, "socketpair: %s", strerror(errno));
        bridge_close(b); return NULL;
    }

    if ((b->pid = fork()) < 0) {
        snprintf(g_err, sizeof g_err, "fork: %s", strerror(errno));
        close(sv[0]); close(sv[1]);
        bridge_close(b); return NULL;
    }
    if (b->pid == 0) {
        /* child */
        char sr[32], bsz[32];
        /* Its own process group, so a plug-in that forks a child of its own --
         * and a couple of them wedge their audio thread that way on certain
         * presets -- can be killed as a group rather than leaving the fork
         * orphaned and spinning on a core. */
        setpgid(0, 0);
        close(sv[0]);
        if (dup2(sv[1], 3) < 0) _exit(127);
        if (sv[1] != 3) close(sv[1]);
        fcntl(3, F_SETFD, 0);                       /* keep it across exec */
        /* The helper's normal chatter would interleave with the host's; keep
         * stderr for diagnostics but drop stdout unless asked. */
        if (!getenv("PELOAD_VERBOSE")) {
            int null = open("/dev/null", O_WRONLY);
            if (null >= 0) { dup2(null, 1); close(null); }
        }
        snprintf(sr, sizeof sr, "%d", (int)samplerate);
        snprintf(bsz, sizeof bsz, "%d", blocksize > 0 ? blocksize : 512);
        /* So the helper does not try to isolate its own plugin in turn. */
        setenv("PELOAD_IS_SERVER", "1", 1);
        execl(helper, helper_name, dll, "--serve", "3", "--shm", b->shm_name,
              "--rate", sr, "--block", bsz, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    b->sock = sv[0];

    /* A deadline on the request socket.
     *
     * Every synchronous op -- parameter displays, editor mouse and keys, opening
     * the editor -- is a round trip to the helper, and pestudio makes them from
     * its GUI thread. With no timeout a helper that stalls freezes the window
     * outright, and a frozen window never gets to send the note-off for whatever
     * the user was playing. Five seconds is far longer than any legitimate op
     * (the slowest here is opening a big plugin's editor, a few hundred ms) and
     * far shorter than a person's patience. */
    {
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(b->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(b->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    (void)fdarg;

    if (bridge_op(b, BR_HELLO, 0, 0, 0, 0, 0, &rep) || !rep.ok) {
        /* The server sends a reply with ok == 0 and a reason when the plugin
         * itself failed to load; a dead socket means it faulted before that --
         * most often a signal, such as SIGABRT out of the "structured
         * exception dispatch is not implemented" path in winstubs.h when a
         * plugin's own init throws a C++ exception. Same treatment as
         * bridge_render_io: say which signal rather than just "died". */
        if (rep.text[0]) {
            snprintf(g_err, sizeof g_err, "%s", rep.text);
        } else {
            int st = 0, reaped = b->pid > 0 && waitpid(b->pid, &st, WNOHANG) == b->pid;
            /* Reaped here rather than left for bridge_close: its own pid==0
             * check would otherwise see an already-collected child as "not
             * gone yet" and spend half a second waiting for what already
             * happened. */
            if (reaped) b->pid = 0;
            if (reaped && WIFSIGNALED(st))
                snprintf(g_err, sizeof g_err, "the helper crashed (signal %d, %s)"
                         " before reporting", WTERMSIG(st), strsignal(WTERMSIG(st)));
            else
                snprintf(g_err, sizeof g_err, "the helper died before reporting");
        }
        bridge_close(b); return NULL;
    }
    snprintf(b->name, sizeof b->name, "%s", rep.text);
    snprintf(b->vendor, sizeof b->vendor, "%s", rep.text2);
    b->nprograms = rep.a; b->nparams = rep.b;
    b->nin = rep.c;       b->nout = rep.d;
    b->flags = rep.e;     b->uid = rep.g;
    return b;
}

/* ---------------------------------------------------------------- metadata */

const char *bridge_name(const bridge *b)   { return b ? b->name : ""; }
const char *bridge_vendor(const bridge *b) { return b ? b->vendor : ""; }
int bridge_num_programs(const bridge *b)   { return b ? b->nprograms : 0; }
int bridge_num_params(const bridge *b)     { return b ? b->nparams : 0; }
int bridge_num_inputs(const bridge *b)     { return b ? b->nin : 0; }
int bridge_num_outputs(const bridge *b)    { return b ? b->nout : 0; }
int bridge_is_synth(const bridge *b)       { return b ? ((b->flags & 0x100) != 0) : 0; }
int bridge_unique_id(const bridge *b)      { return b ? b->uid : 0; }

static void bridge_text(bridge *b, int op, int idx, char *buf, int n)
{
    bridge_rep r;
    if (n > 0) buf[0] = 0;
    if (!b || bridge_op(b, op, idx, 0, 0, 0, 0, &r) || !r.ok) return;
    snprintf(buf, (size_t)n, "%s", r.text);
}

int bridge_alive(const bridge *b) { return b && !b->dead; }
void bridge_set_input_mask(bridge *b, unsigned mask)
{
    bridge_rep r;
    if (!b || b->dead) return;
    bridge_op(b, BR_INPUT_MASK, (int)mask, 0, 0, 0, 0, &r);
}

void bridge_param_name(bridge *b, int i, char *buf, int n)
{ bridge_text(b, BR_PARAM_NAME, i, buf, n); }
void bridge_param_label(bridge *b, int i, char *buf, int n)
{ bridge_text(b, BR_PARAM_LABEL, i, buf, n); }
void bridge_param_display(bridge *b, int i, char *buf, int n)
{ bridge_text(b, BR_PARAM_DISPLAY, i, buf, n); }
void bridge_program_name(bridge *b, int i, char *buf, int n)
{ bridge_text(b, BR_PROGRAM_NAME, i, buf, n); }

float bridge_get_param(bridge *b, int i)
{
    bridge_rep r;
    if (!b || bridge_op(b, BR_PARAM_GET, i, 0, 0, 0, 0, &r) || !r.ok) return 0.0f;
    return r.f;
}

/* Queued through shared memory, not the socket: a slider drag would otherwise
 * make one round trip per pixel, and the value has to land on the audio side
 * anyway. */
void bridge_set_param(bridge *b, int i, float v)
{
    bridge_shm *s;
    uint32_t h, t;
    if (!b || b->dead || i < 0 || i >= b->nparams) return;
    s = b->sh;
    h = atomic_load_explicit(&s->p_head, memory_order_relaxed);
    t = atomic_load_explicit(&s->p_tail, memory_order_acquire);
    if (h - t >= BRIDGE_PARAMQ) return;                  /* full: drop */
    s->pq[h % BRIDGE_PARAMQ].index = i;
    s->pq[h % BRIDGE_PARAMQ].value = v;
    atomic_store_explicit(&s->p_head, h + 1, memory_order_release);
}

void bridge_midi_at(bridge *b, int status, int d1, int d2, int at)
{
    bridge_shm *s;
    uint32_t h, t;
    if (!b || b->dead) return;
    s = b->sh;
    h = atomic_load_explicit(&s->m_head, memory_order_relaxed);
    t = atomic_load_explicit(&s->m_tail, memory_order_acquire);
    if (h - t >= BRIDGE_MIDIQ) return;
    s->mq[h % BRIDGE_MIDIQ].at     = at;
    s->mq[h % BRIDGE_MIDIQ].status = (uint8_t)status;
    s->mq[h % BRIDGE_MIDIQ].d1     = (uint8_t)d1;
    s->mq[h % BRIDGE_MIDIQ].d2     = (uint8_t)d2;
    s->mq[h % BRIDGE_MIDIQ].pad    = 0;
    atomic_store_explicit(&s->m_head, h + 1, memory_order_release);
}

void bridge_midi(bridge *b, int status, int d1, int d2)
{ bridge_midi_at(b, status, d1, d2, -1); }

void bridge_set_program(bridge *b, int i)
{ bridge_rep r; if (b) bridge_op(b, BR_SET_PROGRAM, i, 0, 0, 0, 0, &r); }

int bridge_get_program(bridge *b)
{
    bridge_rep r;
    if (!b || bridge_op(b, BR_GET_PROGRAM, 0, 0, 0, 0, 0, &r) || !r.ok) return 0;
    return r.a;
}

void bridge_all_notes_off(bridge *b)
{ bridge_rep r; if (b) bridge_op(b, BR_ALL_NOTES_OFF, 0, 0, 0, 0, 0, &r); }

void bridge_import_stats(bridge *b, int *impl, int *stub, int *hit)
{
    bridge_rep r;
    if (impl) *impl = 0;
    if (stub) *stub = 0;
    if (hit)  *hit  = 0;
    if (!b || bridge_op(b, BR_IMPORT_STATS, 0, 0, 0, 0, 0, &r) || !r.ok) return;
    if (impl) *impl = r.a;
    if (stub) *stub = r.b;
    if (hit)  *hit  = r.c;
}

/* ------------------------------------------------------------------- audio */

/* Called from the host's realtime thread. Posts the request and waits, with a
 * deadline: if the helper has died or stalled we return silence rather than
 * stalling the whole graph. */
void bridge_render_io(bridge *b, const float *in, float *out, int frames)
{
    bridge_shm *s;
    struct timespec ts;
    int i;

    if (frames <= 0) return;
    if (!b || b->dead || frames > BRIDGE_MAX_FRAMES) {
        /* Say so once. Silence with no explanation is the worst possible failure
         * mode: it looks like a plugin that stopped working rather than a helper
         * that is gone. */
        if (b && b->dead && !b->reported_dead) {
            /* How it went is the whole question. A signal means the plugin
             * faulted and the address is worth chasing; a clean exit status means
             * it decided to leave. Saying only "gone" leaves both looking the
             * same, and neither reproducible without the reporter's audio
             * device. */
            int st = 0;
            b->reported_dead = 1;
            if (b->pid > 0 && waitpid(b->pid, &st, WNOHANG) == b->pid) {
                if (WIFSIGNALED(st))
                    fprintf(stderr, "bridge: the helper died on signal %d (%s)"
                                    " -- audio will be silent until this plugin"
                                    " is reloaded\n",
                            WTERMSIG(st), strsignal(WTERMSIG(st)));
                else
                    fprintf(stderr, "bridge: the helper exited with status %d"
                                    " -- audio will be silent until this plugin"
                                    " is reloaded\n", WEXITSTATUS(st));
            } else {
                fprintf(stderr, "bridge: the helper is gone (still reaping) -- "
                                "audio will be silent until this plugin is "
                                "reloaded\n");
            }
        }
        memset(out, 0, (size_t)frames * 2 * sizeof *out);
        return;
    }
    s = b->sh;

    /* Collect anything still in flight before posting again.
     *
     * A missed deadline does not cancel the request: the helper finishes it and
     * posts `done` regardless. Leaving that post uncollected makes the *next*
     * wait succeed instantly while the helper is still writing the shared
     * buffers, and host and helper then stay exactly one block out of phase for
     * the rest of the session -- reading output as it is being written and
     * overwriting `in` and `frames` mid-render. One xrun became permanently
     * broken audio that way, which is what "it froze" looks like from outside. */
    while (b->pending > 0) {
        struct timespec drain;
        drain.tv_sec = 0;
        drain.tv_nsec = 2000000;                     /* 2 ms */
        if (bridge_sem_wait(&s->done, &drain)) break;
        b->pending--;
    }
    if (b->pending > 0) {                            /* still behind: skip a block */
        /* Persistently behind means it is not coming back -- most likely its audio
         * thread died, which leaves the process alive and the socket open, so
         * nothing else notices. */
        if (++b->behind >= 200) {
            int st = 0;
            int exited = (b->pid > 0 && waitpid(b->pid, &st, WNOHANG) == b->pid);
            fprintf(stderr, "bridge: the helper has stopped rendering (%d blocks "
                            "with no reply)%s\n", b->behind,
                    exited ? " -- it exited"
                           : " -- its audio thread is stuck; giving up on it");
            /* Not coming back: a render that has produced nothing for a second
             * is wedged inside the plug-in, not merely slow. Mark the helper
             * dead so the host stops waiting on it and the window can say so --
             * pehost_alive turns false and the editor and audio are reported
             * gone rather than silently frozen. Kill its whole group first, so
             * a plug-in that wedged itself by forking does not leave the fork
             * spinning on a core for the rest of the session. */
            if (!exited && b->pid > 0) { kill(-b->pid, SIGKILL); kill(b->pid, SIGKILL); }
            b->dead = 1;
        }
        memset(out, 0, (size_t)frames * 2 * sizeof *out);
        return;
    }
    b->behind = 0;

    if (in) memcpy(s->in, in, (size_t)frames * 2 * sizeof *in);
    else    memset(s->in, 0, (size_t)frames * 2 * sizeof *s->in);
    s->frames = frames;

    bridge_sem_post(&s->req);

    /* Two periods plus a floor, so a slow first block does not trip it. */
    {
        long ns = (long)(2.0 * 1e9 * frames / (b->sr > 0 ? b->sr : 48000.0)) + 50000000L;
        ts.tv_sec  = ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
    }
    if (bridge_sem_wait(&s->done, &ts)) {
        if (!s->xruns)
            fprintf(stderr, "bridge: helper missed its deadline (%d frames); "
                            "emitting silence\n", frames);
        s->xruns++;
        b->pending++;                                /* collected on the next call */
        memset(out, 0, (size_t)frames * 2 * sizeof *out);
        return;
    }
    memcpy(out, s->out, (size_t)frames * 2 * sizeof *out);
    (void)i;
}

unsigned bridge_xruns(const bridge *b) { return b ? b->sh->xruns : 0; }

/* ------------------------------------------------------------------ editor */

int bridge_editor_kind(bridge *b)
{
    bridge_rep r;
    if (!b || bridge_op(b, BR_EDITOR_KIND, 0, 0, 0, 0, 0, &r) || !r.ok) return 0;
    return r.a;
}

void bridge_editor_size(bridge *b, int *w, int *h)
{
    bridge_rep r;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!b || bridge_op(b, BR_EDITOR_SIZE, 0, 0, 0, 0, 0, &r) || !r.ok) return;
    if (w) *w = r.a;
    if (h) *h = r.b;
}

int bridge_editor_open(bridge *b)
{
    bridge_rep r;
    if (!b || bridge_op(b, BR_EDITOR_OPEN, 0, 0, 0, 0, 0, &r) || !r.ok) return -1;
    b->ed_w = r.a; b->ed_h = r.b; b->ed_open = 1;
    return 0;
}

void bridge_editor_close(bridge *b)
{
    bridge_rep r;
    if (!b || !b->ed_open) return;
    bridge_op(b, BR_EDITOR_CLOSE, 0, 0, 0, 0, 0, &r);
    b->ed_open = 0;
}

/* Spin a few times, then sleep in short steps: a publish takes well under a
 * millisecond and they are 16 ms apart, so a reader that lands in one is never
 * waiting long. Bounded at roughly five milliseconds in total. */
#define BRIDGE_ED_TRIES 64
static void ed_backoff(int tries)
{
    if (tries < 8) { sched_yield(); return; }
    { struct timespec ts = { 0, 100000 };      /* 0.1 ms */
      nanosleep(&ts, NULL); }
}

/* The helper republishes pixels on its own 60 Hz pump, so there is nothing to
 * ask for -- just read whatever is current. */
int bridge_editor_pixels(bridge *b, const unsigned int **px, int *w, int *h)
{
    bridge_shm *sh;
    int tries;

    if (!b || b->dead || !b->ed_open) return 0;
    sh = b->sh;
    b->ed_reads++;

    /* Read the frame under the helper's sequence lock: take the generation, copy,
     * then check it did not move. An odd value means a write is in progress.
     * Copying is what makes this safe -- returning a pointer into the shared
     * buffer, as this used to, hands the caller memory that keeps changing under
     * it however carefully the counter is checked. */
    /* Wait for the writer rather than give up on it.
     *
     * Eight spins and a sched_yield each is a shorter budget than the writer's
     * critical section: publishing a 1096x586 editor is a two-and-a-half
     * megabyte memcpy, and a reader that arrives inside one saw the same odd
     * generation on all eight attempts and returned "no frame". With no earlier
     * frame to fall back on -- which is exactly the situation on the first read
     * after opening an editor -- that reached the host as "editor produced no
     * pixels", and it is why no 32-bit editor ever appeared. The window is
     * bounded by one copy, so the right thing is to keep looking for a few
     * milliseconds. */
    for (tries = 0; tries < BRIDGE_ED_TRIES; tries++) {
        uint32_t g0 = atomic_load_explicit(&sh->ed_gen, memory_order_acquire);
        int cw, ch;
        size_t bytes;

        if (g0 & 1u) { b->ed_torn++; ed_backoff(tries); continue; }  /* mid-write */
        cw = sh->ed_w; ch = sh->ed_h;
        if (cw <= 0 || ch <= 0 || cw > BRIDGE_MAX_ED_W || ch > BRIDGE_MAX_ED_H)
            break;
        bytes = (size_t)cw * (size_t)ch * 4;
        if (bytes > b->ed_cap) {
            unsigned int *grown = realloc(b->ed_buf, bytes);
            if (!grown) break;
            b->ed_buf = grown;
            b->ed_cap = bytes;
        }
        memcpy(b->ed_buf, bridge_pixels(sh), bytes);
        if (atomic_load_explicit(&sh->ed_gen, memory_order_acquire) != g0) {
            b->ed_torn++;
            ed_backoff(tries);
            continue;                                  /* it changed: try again */
        }
        b->ed_bw = cw; b->ed_bh = ch; b->ed_have = 1;
        b->ed_seen = g0;
        break;
    }

    /* If every attempt raced, show the last whole frame rather than a torn one or
     * nothing: a repeated frame reads as a pause, a torn one as a glitch. */
    if (!b->ed_have) return 0;
    if (px) *px = b->ed_buf;
    if (w)  *w  = b->ed_bw;
    if (h)  *h  = b->ed_bh;
    return 1;
}

/* Editor input goes into shared memory, with no reply waited for.
 *
 * It used to be a request op and that could not work: a plugin in a modal drag
 * loop polls for the button release, and the helper's main thread is inside the
 * wndproc that started the loop, so it never reaches the socket to read it. The
 * op timed out after five seconds and the editor was dead. Keys have exactly the
 * same problem -- holding one while adjusting a control stalled op 14 -- so both
 * take this path. */
static void push_input(bridge *b, int kind, int a, int bb, int cc, int d, int e)
{
    bridge_shm *s;
    uint32_t h, t;
    if (!b || b->dead) return;
    s = b->sh;
    h = atomic_load_explicit(&s->in_head, memory_order_relaxed);
    t = atomic_load_explicit(&s->in_tail, memory_order_acquire);
    if (h - t >= BRIDGE_INQ) return;                    /* full: drop a move */
    s->inq[h % BRIDGE_INQ].kind = kind;
    s->inq[h % BRIDGE_INQ].a = a;
    s->inq[h % BRIDGE_INQ].b = bb;
    s->inq[h % BRIDGE_INQ].c = cc;
    s->inq[h % BRIDGE_INQ].d = d;
    s->inq[h % BRIDGE_INQ].e = e;
    atomic_store_explicit(&s->in_head, h + 1, memory_order_release);
}

void bridge_editor_mouse(bridge *b, int x, int y, int msg, int buttons, int wheel)
{ push_input(b, BRIDGE_IN_MOUSE, x, y, msg, buttons, wheel); }

void bridge_editor_key(bridge *b, int vk, int down, int ch)
{ push_input(b, BRIDGE_IN_KEY, vk, down, ch, 0, 0); }
