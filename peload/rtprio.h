/* Realtime priority for an audio thread: the scheduler, then RealtimeKit.
 *
 * Asking the scheduler alone gets nothing on an ordinary desktop, where the
 * user's rtprio limit is 0, and fails quietly. A helper's audio thread left at
 * ordinary priority is the worst case of that: the host's realtime thread is
 * blocked waiting for it, so the whole graph waits on a thread any compile job
 * can keep off the CPU. PipeWire's own threads get their priority from
 * RealtimeKit; this does the same.
 *
 * Two ways, in order: the scheduler directly (works where the user's rtprio
 * limit allows it), then RealtimeKit over the system bus. libdbus is opened at
 * run time, so nothing is linked or required: with no libdbus, no rtkit or a
 * refusal, the thread simply stays as it was and the caller is told why.
 *
 * A header, like the rest of what the loaders share: the 32-bit helper is one
 * translation unit built on its own. Everything is per translation unit. */
#ifndef VSTACE_RTPRIO_H
#define VSTACE_RTPRIO_H

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000      /* <sched.h> hides it without _GNU_SOURCE */
#endif

/* RealtimeKit's defaults: it refuses a higher priority, and a process whose
 * limit on realtime CPU without blocking is any longer. */
#define RTPRIO_RTKIT_MAX_PRIO   20
#define RTPRIO_RTKIT_MAX_USEC   200000

/* The few libdbus things used, declared here so no dbus header is needed. The
 * two opaque structs are given generous room. */
typedef struct { char opaque[96]; } rtprio_dbus_error;
typedef struct { char opaque[256]; } rtprio_dbus_iter;
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;

#define RTPRIO_BUS_SYSTEM   1               /* DBUS_BUS_SYSTEM */
#define RTPRIO_TYPE_UINT64  ((int)'t')
#define RTPRIO_TYPE_UINT32  ((int)'u')

static inline void rtprio_say(char *why, size_t n, const char *fmt, const char *a)
{
    if (why && n) snprintf(why, n, fmt, a ? a : "");
}

/* Ask for realtime scheduling for the calling thread. Returns 1 when it got it;
 * either way `how` says what happened, for a log line. Threads the caller goes
 * on to create do not inherit it. */
static inline int rtprio_request(int priority, char *how, size_t hown)
{
    struct sched_param sp;
    void *lib;
    rtprio_dbus_error err;
    rtprio_dbus_iter it;
    DBusConnection *c;
    DBusMessage *m, *r;
    uint64_t tid;
    uint32_t prio;
    struct rlimit rl;
    int ok = 0;

    void (*error_init)(rtprio_dbus_error *);
    int  (*error_is_set)(const rtprio_dbus_error *);
    void (*error_free)(rtprio_dbus_error *);
    DBusConnection *(*bus_get_private)(int, rtprio_dbus_error *);
    void (*set_exit)(DBusConnection *, int);
    DBusMessage *(*new_call)(const char *, const char *, const char *, const char *);
    void (*init_append)(DBusMessage *, rtprio_dbus_iter *);
    int  (*append_basic)(rtprio_dbus_iter *, int, const void *);
    DBusMessage *(*send_block)(DBusConnection *, DBusMessage *, int, rtprio_dbus_error *);
    void (*msg_unref)(DBusMessage *);
    void (*conn_close)(DBusConnection *);
    void (*conn_unref)(DBusConnection *);

    if (how && hown) how[0] = 0;
    if (priority < 1) priority = 1;

    /* 1: straight from the scheduler, if the user's limits permit it. */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = priority;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) == 0) {
        rtprio_say(how, hown, "realtime (scheduler)%s", "");
        return 1;
    }

    /* 2: RealtimeKit, over the system bus. */
    /* NODELETE: libdbus keeps global state that only dbus_shutdown frees, and
     * unloading it here would orphan that state on every call -- one leak a time
     * the audio thread starts. Kept mapped, the state is reused by the next call
     * (and shared, harmlessly, with any other user of libdbus in the process). */
    lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!lib) { rtprio_say(how, hown, "no realtime priority: %s", "libdbus is not installed"); return 0; }
#define RTPRIO_SYM(var, name) do { *(void **)&(var) = dlsym(lib, name); if (!(var)) { rtprio_say(how, hown, "no realtime priority: %s", "libdbus lacks " name); dlclose(lib); return 0; } } while (0)
    RTPRIO_SYM(error_init, "dbus_error_init");
    RTPRIO_SYM(error_is_set, "dbus_error_is_set");
    RTPRIO_SYM(error_free, "dbus_error_free");
    RTPRIO_SYM(bus_get_private, "dbus_bus_get_private");
    RTPRIO_SYM(set_exit, "dbus_connection_set_exit_on_disconnect");
    RTPRIO_SYM(new_call, "dbus_message_new_method_call");
    RTPRIO_SYM(init_append, "dbus_message_iter_init_append");
    RTPRIO_SYM(append_basic, "dbus_message_iter_append_basic");
    RTPRIO_SYM(send_block, "dbus_connection_send_with_reply_and_block");
    RTPRIO_SYM(msg_unref, "dbus_message_unref");
    RTPRIO_SYM(conn_close, "dbus_connection_close");
    RTPRIO_SYM(conn_unref, "dbus_connection_unref");
#undef RTPRIO_SYM

    /* RealtimeKit wants a finite limit on how long a realtime thread may run
     * without blocking; 200 ms is its own ceiling. It is a limit on the whole
     * process, and a hard one that cannot be raised again, and a realtime thread
     * that outruns it is killed with the process. So a limit someone has already
     * set is left alone, and this one is only ever set when none is, and only
     * once. Only realtime threads are bound by it; the plug-ins here are not. */
    if (getrlimit(RLIMIT_RTTIME, &rl) == 0 && rl.rlim_max == RLIM_INFINITY) {
        rl.rlim_cur = rl.rlim_max = RTPRIO_RTKIT_MAX_USEC;
        if (setrlimit(RLIMIT_RTTIME, &rl) != 0) { /* RealtimeKit will say if it matters */ }
    }

    memset(&err, 0, sizeof err);
    error_init(&err);
    c = bus_get_private(RTPRIO_BUS_SYSTEM, &err);
    if (!c) {
        rtprio_say(how, hown, "no realtime priority: %s", "no system bus");
        if (error_is_set(&err)) error_free(&err);
        dlclose(lib);
        return 0;
    }
    set_exit(c, 0);                      /* a lost bus must not take the program with it */
    m = new_call("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
                 "org.freedesktop.RealtimeKit1", "MakeThreadRealtime");
    if (m) {
        tid = (uint64_t)syscall(SYS_gettid);
        prio = (uint32_t)(priority > RTPRIO_RTKIT_MAX_PRIO ? RTPRIO_RTKIT_MAX_PRIO : priority);
        init_append(m, &it);
        append_basic(&it, RTPRIO_TYPE_UINT64, &tid);
        append_basic(&it, RTPRIO_TYPE_UINT32, &prio);
        r = send_block(c, m, 1000, &err);
        if (r) { msg_unref(r); ok = 1; }
        msg_unref(m);
    }
    if (!ok) {
        rtprio_say(how, hown, "no realtime priority: %s",
                   error_is_set(&err) && ((const char **)&err)[1] ? ((const char **)&err)[1] : "RealtimeKit refused");
        if (error_is_set(&err)) error_free(&err);
    } else {
        rtprio_say(how, hown, "realtime (RealtimeKit)%s", "");
    }
    conn_close(c);
    conn_unref(c);
    dlclose(lib);
    return ok;
}

/* Give the calling thread its ordinary scheduling back. The flag stays in the
 * request: both ways of getting realtime here set reset-on-fork, and the kernel
 * refuses an unprivileged thread that tries to clear it, with EPERM and no other
 * effect -- the thread would simply stay realtime. */
static inline void rtprio_normal(void)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    pthread_setschedparam(pthread_self(), SCHED_OTHER | SCHED_RESET_ON_FORK, &sp);
}

/* ----- the same, for a thread that runs code nobody here can bound -----
 *
 * The limit above is a kill: a realtime thread that runs 200 ms without blocking
 * takes its process with it, on SIGKILL, with nothing said. Before a thread is
 * realtime that same 200 ms is only a late block, and a plug-in is free to take
 * it -- a program change runs on the audio thread, and loading one can be slow.
 * Asking for realtime must not turn that into a crash.
 *
 * So the limit gets a soft half as well. At the soft limit the kernel sends
 * SIGXCPU, well before the hard one, and the handler puts the guarded threads
 * back to ordinary priority: the block is late, as it would have been, and
 * nothing dies. It stays there -- something that overran once can again, and a
 * glitchy thread at ordinary priority beats a dead one. */
#define RTPRIO_GUARD_SOFT_USEC  100000
#define RTPRIO_GUARD_SLOTS      8

__attribute__((unused)) static atomic_long rtprio_guarded[RTPRIO_GUARD_SLOTS];
__attribute__((unused)) static atomic_int  rtprio_guard_said;

static inline void rtprio_on_xcpu(int sig)
{
    static const char msg[] = "rtprio: an audio thread ran too long without "
                              "blocking; back to ordinary priority\n";
    const int saved = errno;
    int i, any = 0;
    (void)sig;
    /* Raw calls only: this can land on any thread, at any moment. */
    for (i = 0; i < RTPRIO_GUARD_SLOTS; i++) {
        const long tid = atomic_load(&rtprio_guarded[i]);
        if (tid) {
            struct sched_param sp;
            memset(&sp, 0, sizeof sp);
            if (syscall(SYS_sched_setscheduler, tid, SCHED_OTHER | SCHED_RESET_ON_FORK, &sp) == 0)
                any = 1;
        }
    }
    if (any && !atomic_exchange(&rtprio_guard_said, 1)) {
        ssize_t w = write(2, msg, sizeof msg - 1);
        (void)w;
    }
    errno = saved;
}

/* Stop guarding the calling thread; it is leaving. A thread id is reused once
 * the thread has gone, and the handler must not touch whoever gets this one. */
static inline void rtprio_guard_release(void)
{
    const long tid = (long)syscall(SYS_gettid);
    int i;
    for (i = 0; i < RTPRIO_GUARD_SLOTS; i++) {
        long want = tid;
        atomic_compare_exchange_strong(&rtprio_guarded[i], &want, 0);
    }
}

/* rtprio_request, for a thread that runs plug-in code. Call
 * rtprio_guard_release before the thread ends. Replaces this process's SIGXCPU
 * handling, and its soft limit on realtime CPU. */
static inline int rtprio_request_guarded(int priority, char *how, size_t hown)
{
    const long tid = (long)syscall(SYS_gettid);
    struct sigaction sa;
    struct rlimit rl;
    int i, slot = -1;

    for (i = 0; i < RTPRIO_GUARD_SLOTS && slot < 0; i++) {
        long want = 0;
        if (atomic_compare_exchange_strong(&rtprio_guarded[i], &want, tid)) slot = i;
    }
    if (slot < 0) {
        rtprio_say(how, hown, "no realtime priority: %s", "too many guarded threads");
        return 0;
    }

    /* The handler first: SIGXCPU's default action is to kill the process. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = rtprio_on_xcpu;
    sa.sa_flags = SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGXCPU, &sa, NULL) != 0 || getrlimit(RLIMIT_RTTIME, &rl) != 0) {
        rtprio_guard_release();
        rtprio_say(how, hown, "no realtime priority: %s", "cannot guard the thread");
        return 0;
    }
    /* A hard limit already set -- inherited from a host whose PipeWire asked
     * RealtimeKit -- cannot be raised, so the soft one has to fit under it. */
    if (rl.rlim_max == RLIM_INFINITY) rl.rlim_max = RTPRIO_RTKIT_MAX_USEC;
    rl.rlim_cur = rl.rlim_max > RTPRIO_GUARD_SOFT_USEC ? RTPRIO_GUARD_SOFT_USEC : rl.rlim_max / 2;
    if (setrlimit(RLIMIT_RTTIME, &rl) != 0) {
        rtprio_guard_release();
        rtprio_say(how, hown, "no realtime priority: %s", "cannot limit the thread");
        return 0;
    }

    if (!rtprio_request(priority, how, hown)) {
        rtprio_guard_release();
        return 0;
    }
    return 1;
}

#endif /* VSTACE_RTPRIO_H */
