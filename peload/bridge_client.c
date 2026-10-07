/* Host side of the 32-bit bridge: spawn peload32 --serve and drive it.
 *
 * Presents the same operations pehost.c implements in-process, so pehost can
 * dispatch to either without its callers knowing which. See bridge.h for the
 * protocol and for why the audio path uses shared memory rather than the socket.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bridge.h"
#include "bridge_client.h"

#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif

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
    _Atomic int dead;                  /* read on the audio thread, written anywhere */
    double      sr;
    int         bs;
    /* Requests posted whose completion we gave up waiting for. Must reach zero
     * before another is posted -- see bridge_render_io. */
    int         pending;
    int         behind;            /* consecutive blocks with no reply */
    float       last[2];           /* the last frame handed out, for fading a gap */
    int         last_valid;        /* a block went out since the last gap */
    int         regain;            /* the first block after a gap comes in over a ramp */
    int         reported_dead;
    _Atomic unsigned midi_dropped;     /* events lost to a full MIDI ring */
    /* What it takes to build the same helper again after one has died, and to
     * put the plug-in back the way the user had it. See bridge_recover. */
    char        path[1024];
    char        helper[32];
    float      *shadow;            /* the last value written to each parameter */
    int         nshadow;
    int         program;           /* the last program selected */
    int         restarts;
    /* Set while a restart is in flight. `dead` cannot serve for this: the
     * handshake with the new helper goes through the same bridge_call every
     * other op does, and that refuses to talk to a dead bridge -- so `dead` has
     * to be cleared before the new helper is asked anything, and something else
     * has to keep the audio thread away from a shared region that is being
     * replaced underneath it. Read by the audio thread, written by the thread
     * doing the restart; a torn read is not possible for an int, and the worst
     * a stale one costs is one block of silence either side. */
    _Atomic int silent;
    /* How many threads are inside an entry point that touches `sh`: the audio
     * thread in render/MIDI, any thread in set_param / editor input / pixels.
     * A restart sets `silent` and then waits for this to reach zero before it
     * unmaps the region, so nothing is ever left reading freed memory. */
    _Atomic int users;
    /* The helper, by handle rather than by number: only the thread that owns the
     * bridge (the one restarting or closing it) ever reaps, and that thread can
     * kill through this without any chance of hitting a reused pid. -1 where the
     * kernel has no pidfds. Closed only once `users` is zero, since the audio
     * thread uses it to cut off a wedged helper. */
    int         pidfd;
    /* Serialises request/reply round trips on the socket: two threads sharing it
     * would otherwise read each other's replies. Recursive so that a restart can
     * hold it across its whole sequence. Never taken on the audio thread. */
    pthread_mutex_t callmu;
};

static __thread char g_err[256];
const char *bridge_last_error(void) { return g_err; }

/* ------------------------------------------------------------------ helper */

/* peload32 sits next to whichever binary is running, so derive its path from
 * /proc/self/exe rather than trusting the working directory. PELOAD32 overrides,
 * which is what a test harness or an installed layout wants. */
/* A helper found by walking up from the binary: owned by whoever is running
 * this or by root, and neither it nor its directory writable by everyone. */
static int helper_trusted(const char *path)
{
    struct stat st;
    char dir[1024], *slash;
    if (stat(path, &st) != 0 || (st.st_uid != geteuid() && st.st_uid != 0) || (st.st_mode & S_IWOTH))
        return 0;
    snprintf(dir, sizeof dir, "%s", path);
    if (!(slash = strrchr(dir, '/'))) return 0;
    *slash = 0;
    if (stat(dir, &st) != 0 || (st.st_uid != geteuid() && st.st_uid != 0) || (st.st_mode & S_IWOTH))
        return 0;
    return 1;
}

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
        /* A helper that somebody else left on the way up is not ours to run:
         * it has to be owned by this user or by root, in a directory nobody
         * else can write to. */
        if (access(out, X_OK) == 0 && helper_trusted(out)) return 0;
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
    /* One round trip at a time. Never called from the audio thread: it can wait
     * up to the socket deadline. */
    pthread_mutex_lock(&b->callmu);
    if (b->dead || b->sock < 0) { pthread_mutex_unlock(&b->callmu); return -1; }
    if (send(b->sock, q, sizeof *q, MSG_NOSIGNAL) != (ssize_t)sizeof *q) {
        b->dead = 1;
        pthread_mutex_unlock(&b->callmu);
        return -1;
    }
    if (recv(b->sock, r ? r : &tmp, sizeof tmp, MSG_WAITALL) != (ssize_t)sizeof tmp) {
        /* Distinguish a stall from a death: both end the session for this
         * plugin, but only one of them is worth reporting as a hang. */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr, "bridge: helper stopped answering (op %d) -- "
                            "dropping it rather than hanging the host\n", q->op);
        b->dead = 1;
        pthread_mutex_unlock(&b->callmu);
        return -1;
    }
    pthread_mutex_unlock(&b->callmu);
    return 0;
}

static int bridge_op(bridge *b, int op, int a, int bb, int c, int d, int e, bridge_rep *r)
{
    bridge_req q;
    memset(&q, 0, sizeof q);
    q.op = op; q.a = a; q.b = bb; q.c = c; q.d = d; q.e = e;
    return bridge_call(b, &q, r);
}

/* Entry to anything that touches the shared region from a thread that does not
 * own the bridge. Pairs with the wait in bridge_teardown: this announces
 * itself first and only then looks at `silent`/`dead` (both seq_cst), so a
 * teardown that has set `silent` either sees this thread in `users` and waits,
 * or this thread sees `silent` and leaves. Neither side blocks or locks. */
static int bridge_enter(bridge *b)
{
    if (!b) return 0;
    atomic_fetch_add(&b->users, 1);
    if (b->dead || b->silent || !b->sh) {
        atomic_fetch_sub(&b->users, 1);
        return 0;
    }
    return 1;
}

static void bridge_leave(bridge *b) { atomic_fetch_sub(&b->users, 1); }

/* Everything bridge_close does except free the struct.
 *
 * A restart needs exactly this: the child gone, the socket and the shared
 * region released, and the bridge object still standing so the pointer the host
 * is holding stays good.
 *
 * Leaves `silent` set. Whoever restarts clears it once the new helper is up. */
static void bridge_teardown(bridge *b)
{
    int i;
    if (!b) return;
    /* Before anything is released: from here the audio thread leaves the region
     * alone. It may be inside it right now, so wait for it -- a bounded wait,
     * because an in-flight render is itself bounded by its own deadline. */
    b->silent = 1;
    for (i = 0; i < 500 && atomic_load(&b->users) > 0; i++) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    if (!b->dead) {
        bridge_rep r;
        bridge_op(b, BR_QUIT, 0, 0, 0, 0, 0, &r);
    }
    pthread_mutex_lock(&b->callmu);
    if (b->sock >= 0) close(b->sock);
    b->sock = -1;
    pthread_mutex_unlock(&b->callmu);
    if (b->pid > 0) {
        int st = 0;
        siginfo_t si;
        /* It should be leaving on its own after BR_QUIT and the closed socket;
         * give it a moment, then insist. Looked at with WNOWAIT so it is still
         * ours -- not yet reaped, its pid not yet reusable -- when the group is
         * killed below. The audio thread never reaps or kills by number; this
         * is the only place a pid is waited on. */
        for (i = 0; i < 50; i++) {
            memset(&si, 0, sizeof si);
            if (waitid(P_PID, (id_t)b->pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid)
                break;
            { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
        }
        /* The group, not just the helper: a plug-in that forked a child of its
         * own leaves it behind otherwise. */
        kill(-b->pid, SIGKILL);
        kill(b->pid, SIGKILL);
        waitpid(b->pid, &st, 0);
        if (WIFSIGNALED(st) && WTERMSIG(st) != SIGKILL)
            fprintf(stderr, "bridge: the helper died on signal %d (%s)\n",
                    WTERMSIG(st), strsignal(WTERMSIG(st)));
    }
    b->pid = 0;
    /* Only now is the region unmapped, and only if nobody is still inside it: a
     * straggler that outlasted the wait keeps a mapping that is leaked rather
     * than pulled out from under it. */
    if (atomic_load(&b->users) == 0) {
        if (b->sh) munmap(b->sh, BRIDGE_SHM_SIZE);
        if (b->pidfd >= 0) close(b->pidfd);
        b->pidfd = -1;
    } else {
        fprintf(stderr, "bridge: audio thread still in the shared region; "
                        "leaving it mapped\n");
    }
    b->sh = NULL;
    if (b->shm_name[0]) shm_unlink(b->shm_name);
    b->shm_name[0] = 0;
    b->pending = b->behind = 0;
    b->ed_have = 0;
    b->ed_seen = 0;
}

void bridge_close(bridge *b)
{
    if (!b) return;
    if (b->ed_reads && getenv("PELOAD_VERBOSE"))
        fprintf(stderr, "  [bridge] editor: %u read(s), %u caught the helper "
                        "mid-frame and were retried\n", b->ed_reads, b->ed_torn);
    bridge_teardown(b);
    if (b->pidfd >= 0) close(b->pidfd);
    pthread_mutex_destroy(&b->callmu);
    free(b->ed_buf);
    free(b->shadow);
    free(b);
}

bridge *bridge_open(const char *dll, double samplerate, int blocksize)
{ return bridge_open_helper(dll, samplerate, blocksize, "peload32"); }

/* Wait a moment for a helper that is already on its way out.
 *
 * Every caller of this has just watched the socket close, so the helper is
 * exiting -- but exiting is not the same as reaped, and a single WNOHANG loses
 * that race often enough that "the helper died before reporting" was the usual
 * message for a plug-in whose init threw. Ten milliseconds at a time up to half
 * a second, the same shape as the wait in bridge_close, turns that into the
 * signal that actually killed it.
 *
 * Only for paths where the load has already failed. The render path has its own
 * WNOHANG checks and must keep them: half a second inside an audio callback is
 * a dropout, and there the question is being asked while the stream is live.
 *
 * Returns 1 and fills *st when the child was collected. */
static int reap_briefly(bridge *b, int *st)
{
    int i;
    if (!b || b->pid <= 0) return 0;
    for (i = 0; i < 50; i++) {
        if (waitpid(b->pid, st, WNOHANG) == b->pid) { b->pid = 0; return 1; }
        { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    }
    return 0;
}

/* Start a helper into an already-allocated bridge.
 *
 * Split out of bridge_open_helper so that bridge_recover can run the same
 * sequence a second time without the caller's pointer changing underneath it.
 * Returns 0 on success; on failure the bridge is torn down but still allocated,
 * and g_err says why. */
static int bridge_spawn(bridge *b, const char *dll, const char *helper_name)
{
    char helper[4096], fdarg[16];
    int sv[2], fd, shmfd = -1;
    bridge_rep rep;
    double samplerate = b->sr;
    int blocksize = b->bs;

    if (bridge_helper_named(helper_name, helper, sizeof helper)) {
        snprintf(g_err, sizeof g_err,
                 "%s not found next to this binary", helper_name);
        return -1;
    }

    /* The region the helper shares with the host is an anonymous file handed
     * down to it, sealed against shrinking and growing -- not a name in
     * /dev/shm. A name can be opened by any process of this user, and a helper
     * (or another plug-in's helper) that opens it and truncates it takes the
     * host's next read of it down with SIGBUS, the audio thread's included.
     * The seals make the size final; no name means no other helper's way in.
     * Where memfd_create is not there the named region is used as before, and
     * unlinked as soon as the helper has attached. */
    shmfd = memfd_create("peload-bridge", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (shmfd >= 0) {
        if (ftruncate(shmfd, (off_t)BRIDGE_SHM_SIZE) ||
            fcntl(shmfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) {
            snprintf(g_err, sizeof g_err, "sealing the shared region: %s", strerror(errno));
            close(shmfd); bridge_teardown(b); return -1;
        }
        b->shm_name[0] = 0;
        fd = shmfd;
    } else {
        snprintf(b->shm_name, sizeof b->shm_name, "/peload32-%d-%p", (int)getpid(), (void *)b);
        shm_unlink(b->shm_name);
        if ((fd = shm_open(b->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600)) < 0) {
            snprintf(g_err, sizeof g_err, "shm_open: %s", strerror(errno));
            b->shm_name[0] = 0; bridge_teardown(b); return -1;
        }
        if (ftruncate(fd, (off_t)BRIDGE_SHM_SIZE)) {
            snprintf(g_err, sizeof g_err, "ftruncate: %s", strerror(errno));
            close(fd); bridge_teardown(b); return -1;
        }
    }
    b->sh = mmap(NULL, BRIDGE_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shmfd < 0) close(fd);               /* a memfd stays open: the helper is handed it below */
    if (b->sh == MAP_FAILED) {
        snprintf(g_err, sizeof g_err, "mmap: %s", strerror(errno));
        if (shmfd >= 0) close(shmfd);
        b->sh = NULL; bridge_teardown(b); return -1;
    }
    memset(b->sh, 0, sizeof *b->sh);
    b->sh->magic = BRIDGE_MAGIC;
    b->sh->version = BRIDGE_VERSION;
    /* The futex-word semaphores start at zero, which memset already did. */

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) {
        snprintf(g_err, sizeof g_err, "socketpair: %s", strerror(errno));
        if (shmfd >= 0) close(shmfd);
        bridge_teardown(b); return -1;
    }

    if ((b->pid = fork()) < 0) {
        snprintf(g_err, sizeof g_err, "fork: %s", strerror(errno));
        close(sv[0]); close(sv[1]);
        if (shmfd >= 0) close(shmfd);
        bridge_teardown(b); return -1;
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
        /* The region may have come out as fd 3 or 4 -- the numbers the helper
         * is about to be given for the socket and for it -- so it is moved
         * clear of them before either is placed. */
        if (shmfd >= 0 && shmfd <= 4) {
            int moved = fcntl(shmfd, F_DUPFD, 10);
            if (moved < 0) _exit(127);
            close(shmfd);
            shmfd = moved;
        }
        if (dup2(sv[1], 3) < 0) _exit(127);
        if (sv[1] != 3) close(sv[1]);
        fcntl(3, F_SETFD, 0);                       /* keep it across exec */
        if (shmfd >= 0) {
            if (shmfd != 4 && dup2(shmfd, 4) < 0) _exit(127);
            if (shmfd != 4) close(shmfd);
            fcntl(4, F_SETFD, 0);                   /* the sealed region, as fd 4 */
        }
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
        /* Nothing else the host holds open belongs in the helper: other plug-ins'
         * sockets and sealed regions, config files, a MIDI device. A leaked
         * socket end keeps a dead sibling's peer from ever seeing EOF. Only
         * 0-4 are meant to cross: stdio, the control socket, the region. */
        if (syscall(SYS_close_range, 5u, ~0u, 0u) != 0) {
            struct rlimit rl;
            int fdn, top = 4096;
            if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
                rl.rlim_cur < 65536) top = (int)rl.rlim_cur;
            for (fdn = 5; fdn < top; fdn++) close(fdn);
        }
        execl(helper, helper_name, dll, "--serve", "3", "--shm", shmfd >= 0 ? "fd:4" : b->shm_name,
              "--rate", sr, "--block", bsz, (char *)NULL);
        /* Only reached when exec itself failed, and saying so is worth the two
         * lines: from the parent this is indistinguishable from a helper that
         * started and then crashed, which sends the reader looking at the
         * plug-in instead of at the file that could not be run. */
        {
            char msg[512];
            int n = snprintf(msg, sizeof msg,
                             "bridge: cannot run %s: %s\n", helper, strerror(errno));
            if (n > 0) { ssize_t w = write(2, msg, (size_t)n); (void)w; }
        }
        _exit(127);
    }
    close(sv[1]);
    if (shmfd >= 0) close(shmfd);               /* the helper has its own */
    pthread_mutex_lock(&b->callmu);
    b->sock = sv[0];
    pthread_mutex_unlock(&b->callmu);
    /* A handle that stays good however long the zombie waits to be reaped; the
     * audio thread uses it to cut off a wedged helper without ever naming a pid
     * that something else may have been given since. */
    b->pidfd = (int)syscall(SYS_pidfd_open, b->pid, 0);

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
            /* Reaped here rather than left for bridge_close: its own pid==0
             * check would otherwise see an already-collected child as "not
             * gone yet" and spend half a second waiting for what already
             * happened. reap_briefly clears b->pid itself. */
            int st = 0, reaped = reap_briefly(b, &st);
            if (reaped && WIFSIGNALED(st))
                snprintf(g_err, sizeof g_err, "the helper crashed (signal %d, %s)"
                         " before reporting", WTERMSIG(st), strsignal(WTERMSIG(st)));
            else
                snprintf(g_err, sizeof g_err, "the helper died before reporting");
        }
        bridge_teardown(b); return -1;
    }
    /* The helper has attached: a named region has served its purpose. */
    if (b->shm_name[0]) { shm_unlink(b->shm_name); b->shm_name[0] = 0; }
    /* A reply's text is the helper's to fill: end it, whatever it put there. */
    rep.text[sizeof rep.text - 1] = 0;
    rep.text2[sizeof rep.text2 - 1] = 0;
    snprintf(b->name, sizeof b->name, "%s", rep.text);
    snprintf(b->vendor, sizeof b->vendor, "%s", rep.text2);
    b->nprograms = rep.a; b->nparams = rep.b;
    b->nin = rep.c;       b->nout = rep.d;
    b->flags = rep.e;     b->uid = rep.g;
    return 0;
}

bridge *bridge_open_helper(const char *dll, double samplerate, int blocksize,
                           const char *helper_name)
{
    bridge *b;

    if (!helper_name) helper_name = "peload32";
    if (!(b = calloc(1, sizeof *b))) return NULL;
    b->sock = -1;
    b->pidfd = -1;
    {
        pthread_mutexattr_t ma;
        pthread_mutexattr_init(&ma);
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&b->callmu, &ma);
        pthread_mutexattr_destroy(&ma);
    }
    b->sr = samplerate;
    b->bs = blocksize > 0 ? blocksize : 512;
    b->program = -1;
    snprintf(b->path, sizeof b->path, "%s", dll ? dll : "");
    snprintf(b->helper, sizeof b->helper, "%s", helper_name);

    if (bridge_spawn(b, dll, helper_name)) { bridge_close(b); return NULL; }
    /* One slot per parameter, so a restart can put back what the user set.
     * Seeded with what the plug-in starts at, so a parameter the user never
     * touched comes back as the plug-in's own default rather than zero. */
    if (b->nparams > 0 && (b->shadow = calloc((size_t)b->nparams, sizeof *b->shadow))) {
        int i;
        b->nshadow = b->nparams;
        for (i = 0; i < b->nparams; i++) b->shadow[i] = bridge_get_param(b, i);
    }
    return b;
}

/* Bring a dead helper back.
 *
 * A plug-in that faults takes its helper with it, and until now that was the
 * end of it: the bridge went dead, the audio went silent, and the message said
 * to reload the plug-in by hand. None of that is necessary. The host knows
 * which plug-in it was, which helper ran it and -- because every parameter
 * write goes through this file -- what the user had set, so it can simply be
 * started again.
 *
 * Called from the thread that owns the plug-in, never from the audio callback:
 * this forks, execs and waits for a plug-in to initialise, which is hundreds of
 * milliseconds. The audio thread meanwhile sees `silent` and returns silence
 * without touching the shared region; teardown waits for any block already in
 * flight before it unmaps, which is what makes that safe.
 *
 * Returns 1 when the plug-in is running again. A plug-in that faults on
 * something it will meet again faults again here, so the restart count is what
 * a caller should use to decide to stop trying. */
int bridge_recover(bridge *b)
{
    int i, was_open, prog;
    float *saved = NULL;
    int nsaved = 0;

    if (!b) return 0;
    if (!b->dead) return 1;
    if (!b->path[0]) return 0;

    /* Held throughout, so no other thread's op lands on the socket while the new
     * helper is being handshaken. (Recursive; the audio thread never takes it.) */
    pthread_mutex_lock(&b->callmu);

    was_open = b->ed_open;
    prog = b->program;
    /* What the user had set, taken before anything can change it: the program
     * change below reads the new helper's defaults back into shadow[], which
     * would otherwise be what got replayed. */
    if (b->shadow && b->nshadow > 0 &&
        (saved = malloc((size_t)b->nshadow * sizeof *saved))) {
        nsaved = b->nshadow;
        memcpy(saved, b->shadow, (size_t)nsaved * sizeof *saved);
    }
    /* Teardown sets `silent` and waits for the audio thread to leave the region,
     * so the unmap cannot pull it out from under a render in flight. */
    bridge_teardown(b);
    b->restarts++;
    fprintf(stderr, "bridge: restarting the helper for %s (attempt %d)\n",
            b->path, b->restarts);
    /* Cleared before the spawn, not after: bridge_spawn's own handshake is an
     * ordinary op, and an op against a bridge still marked dead is refused. The
     * audio thread stays out through `silent`, not `dead`. */
    b->dead = 0;
    if (bridge_spawn(b, b->path, b->helper[0] ? b->helper : "peload32")) {
        fprintf(stderr, "bridge: it did not come back: %s\n", g_err);
        b->dead = 1;
        b->silent = 0;
        free(saved);
        pthread_mutex_unlock(&b->callmu);
        return 0;
    }

    b->reported_dead = 0;
    b->ed_open = 0;

    /* >= 0, not > 0: program 0 is a program, and -1 is the "never selected"
     * this starts at. */
    if (prog >= 0 && prog < b->nprograms)
        bridge_set_program(b, prog);
    /* Replayed through the socket: the rings belong to the audio thread, which is
     * still held off, and bridge_set_param refuses while `silent`. Done after the
     * program so the user's values win over the program's. */
    for (i = 0; i < nsaved && i < b->nparams; i++) {
        bridge_req q;
        bridge_rep r;
        memset(&q, 0, sizeof q);
        q.op = BR_SET_PARAM; q.a = i; q.f = saved[i];
        if (bridge_call(b, &q, &r)) break;
        b->shadow[i] = saved[i];
    }
    free(saved);
    /* bridge_editor_open returns 0 for success, like the other ops here -- not
     * a truth value. Testing it as one reported every successful reopen as a
     * failure. */
    if (was_open && bridge_editor_open(b) != 0)
        fprintf(stderr, "bridge: the plug-in is back, but its editor did not "
                        "reopen\n");
    b->silent = 0;                    /* audio again */
    pthread_mutex_unlock(&b->callmu);
    fprintf(stderr, "bridge: %s is running again\n", b->name);
    return 1;
}

int bridge_restarts(const bridge *b) { return b ? b->restarts : 0; }

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
    /* A dead helper answers from the shadow: the last value written to each
     * parameter, which is what a recovery replays anyway -- and what a shell
     * captures before reloading a plug-in that would not come back, so the
     * reload keeps its sound instead of zeroing it. */
    if (b && b->dead && i >= 0 && i < b->nshadow) return b->shadow[i];
    if (!b || bridge_op(b, BR_PARAM_GET, i, 0, 0, 0, 0, &r) || !r.ok) return 0.0f;
    return r.f;
}

/* Queued through shared memory, not the socket: a slider drag would otherwise
 * make one round trip per pixel, and the value has to land on the audio side
 * anyway. Any thread may call this. */
void bridge_set_param(bridge *b, int i, float v)
{
    bridge_shm *s;
    uint32_t pos;
    if (!b || i < 0 || i >= b->nparams) return;
    /* Remembered even when the helper is down or being restarted, so a restart
     * replays what the user last set rather than losing a change made during the
     * gap. */
    if (i < b->nshadow) b->shadow[i] = v;
    if (!bridge_enter(b)) return;
    s = b->sh;
    if (bridge_ring_claim(&s->p_head, &s->p_tail, BRIDGE_PARAMQ, &pos)) {
        s->pq[pos % BRIDGE_PARAMQ].index = i;
        s->pq[pos % BRIDGE_PARAMQ].value = v;
        bridge_ring_publish(&s->pq[pos % BRIDGE_PARAMQ].seq, pos);
    }                                                    /* full: drop */
    bridge_leave(b);
}

/* Any thread, including the audio thread: claims a slot and never waits. */
void bridge_midi_at(bridge *b, int status, int d1, int d2, int at)
{
    bridge_shm *s;
    uint32_t pos;
    if (!bridge_enter(b)) return;
    s = b->sh;
    if (!bridge_ring_claim(&s->m_head, &s->m_tail, BRIDGE_MIDIQ, &pos)) {
        /* Lost, and it may have been a note-off. The helper releases
         * everything once it has drained what got through. */
        if (!atomic_fetch_add(&b->midi_dropped, 1))
            fprintf(stderr, "bridge: the MIDI ring overflowed -- some MIDI was "
                            "lost; releasing every sounding note\n");
        atomic_store_explicit(&s->m_release, 1, memory_order_release);
        bridge_leave(b);
        return;
    }
    s->mq[pos % BRIDGE_MIDIQ].at     = at;
    s->mq[pos % BRIDGE_MIDIQ].status = (uint8_t)status;
    s->mq[pos % BRIDGE_MIDIQ].d1     = (uint8_t)d1;
    s->mq[pos % BRIDGE_MIDIQ].d2     = (uint8_t)d2;
    s->mq[pos % BRIDGE_MIDIQ].pad    = 0;
    bridge_ring_publish(&s->mq[pos % BRIDGE_MIDIQ].seq, pos);
    bridge_leave(b);
}

void bridge_midi(bridge *b, int status, int d1, int d2)
{ bridge_midi_at(b, status, d1, d2, -1); }

void bridge_set_program(bridge *b, int i)
{
    bridge_rep r;
    if (!b) return;
    bridge_op(b, BR_SET_PROGRAM, i, 0, 0, 0, 0, &r);
    b->program = i;
    /* A program change moves every parameter at once, so the values held on
     * this side belong to the old program. Re-reading them keeps a later
     * restart from dragging the previous patch back over the new one. */
    if (!b->dead) {
        int k;
        for (k = 0; k < b->nshadow; k++) b->shadow[k] = bridge_get_param(b, k);
    }
}

int bridge_get_program(bridge *b)
{
    bridge_rep r;
    /* Dead: the program last selected, as recovery would reselect it. */
    if (b && b->dead) return b->program >= 0 ? b->program : 0;
    if (!b || bridge_op(b, BR_GET_PROGRAM, 0, 0, 0, 0, 0, &r) || !r.ok) return 0;
    return r.a;
}

/* A flag in shared memory rather than BR_ALL_NOTES_OFF: this is reachable from
 * the audio thread (pehost_release_all), which must never make a socket round
 * trip. The helper's audio thread answers the flag once the MIDI ring has
 * drained, which is what the op did anyway. */
void bridge_all_notes_off(bridge *b)
{
    if (!bridge_enter(b)) return;
    atomic_store_explicit(&b->sh->m_release, 1, memory_order_release);
    bridge_leave(b);
}

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
/* A block the helper did not deliver -- a missed deadline, a dead helper --
 * goes out as silence, and silence that begins on a nonzero sample is a click.
 * The first such block carries the last frame it had, faded to zero over a
 * couple of ms; the block that follows the gap comes in over a ramp for the
 * same reason. */
#define BRIDGE_GAP_RAMP 96

static void bridge_gap(bridge *b, float *out, int frames)
{
    int i, n = frames < BRIDGE_GAP_RAMP ? frames : BRIDGE_GAP_RAMP;
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    if (!b) return;
    if (b->last_valid) {
        for (i = 0; i < n; i++) {
            float g = 1.0f - (float)(i + 1) / (float)n;
            out[2 * i]     = b->last[0] * g;
            out[2 * i + 1] = b->last[1] * g;
        }
        b->last_valid = 0;
    }
    b->regain = 1;
}

static void bridge_delivered(bridge *b, float *out, int frames)
{
    if (b->regain) {
        int i, n = frames < BRIDGE_GAP_RAMP ? frames : BRIDGE_GAP_RAMP;
        for (i = 0; i < n; i++) {
            float g = (float)(i + 1) / (float)n;
            out[2 * i] *= g;
            out[2 * i + 1] *= g;
        }
        b->regain = 0;
    }
    if (frames > 0) {
        b->last[0] = out[2 * (frames - 1)];
        b->last[1] = out[2 * (frames - 1) + 1];
        b->last_valid = 1;
    }
}

static void bridge_render_inner(bridge *b, const float *in, float *out, int frames);

void bridge_render_io(bridge *b, const float *in, float *out, int frames)
{
    if (frames <= 0) return;
    /* Being restarted: silence, and none of the bookkeeping below -- the shared
     * region it would read is in the middle of being replaced. */
    if (b && b->silent && !b->dead) {
        bridge_gap(b, out, frames);
        return;
    }
    if (!b || b->dead || frames > BRIDGE_MAX_FRAMES) {
        /* Say so once. Silence with no explanation is the worst possible failure
         * mode: it looks like a plugin that stopped working rather than a helper
         * that is gone. How it died is reported by whoever reaps it (see
         * bridge_teardown): the audio thread does not wait on children. */
        if (b && b->dead && !b->reported_dead) {
            b->reported_dead = 1;
            fprintf(stderr, "bridge: the helper is gone -- restarting it\n");
        }
        bridge_gap(b, out, frames);
        return;
    }
    /* In flight is counted for the whole block, so a restart waits for it. */
    if (!bridge_enter(b)) {
        bridge_gap(b, out, frames);
        return;
    }
    bridge_render_inner(b, in, out, frames);
    bridge_leave(b);
}

static void bridge_render_inner(bridge *b, const float *in, float *out, int frames)
{
    bridge_shm *s;
    struct timespec ts;

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
            fprintf(stderr, "bridge: the helper has stopped rendering (%d blocks "
                            "with no reply) -- its audio thread is stuck; giving "
                            "up on it\n", b->behind);
            /* Not coming back: a render that has produced nothing for a second
             * is wedged inside the plug-in, not merely slow. Mark the helper
             * dead so the host stops waiting on it and the window can say so --
             * pehost_alive turns false and the editor and audio are reported
             * gone rather than silently frozen.
             *
             * Killed through the pidfd, never by pid: this thread does not reap,
             * and the thread that does may already have, in which case a pid
             * could belong to something else by now. The pidfd stays valid
             * (teardown waits for this thread to leave before closing it), and
             * the helper's process group is swept by teardown. */
            if (b->pidfd >= 0)
                syscall(SYS_pidfd_send_signal, b->pidfd, SIGKILL, NULL, 0);
            b->dead = 1;
        }
        bridge_gap(b, out, frames);
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
        bridge_gap(b, out, frames);
        return;
    }
    memcpy(out, s->out, (size_t)frames * 2 * sizeof *out);
    bridge_delivered(b, out, frames);
}

unsigned bridge_xruns(const bridge *b)
{
    unsigned x = 0;
    if (bridge_enter((bridge *)b)) { x = b->sh->xruns; bridge_leave((bridge *)b); }
    return x;
}

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

    if (!b || !b->ed_open || !bridge_enter(b)) return 0;
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

    bridge_leave(b);
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
    uint32_t pos;
    bridge_input *q;
    if (!bridge_enter(b)) return;
    s = b->sh;
    if (bridge_ring_claim(&s->in_head, &s->in_tail, BRIDGE_INQ, &pos)) {   /* full: drop a move */
        q = &s->inq[pos % BRIDGE_INQ];
        q->kind = kind;
        q->a = a;
        q->b = bb;
        q->c = cc;
        q->d = d;
        q->e = e;
        bridge_ring_publish(&q->seq, pos);
    }
    bridge_leave(b);
}

void bridge_editor_mouse(bridge *b, int x, int y, int msg, int buttons, int wheel)
{ push_input(b, BRIDGE_IN_MOUSE, x, y, msg, buttons, wheel); }

void bridge_editor_key(bridge *b, int vk, int down, int ch)
{ push_input(b, BRIDGE_IN_KEY, vk, down, ch, 0, 0); }
