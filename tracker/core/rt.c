/* Realtime priority for the tracker's audio thread.
 *
 * The thread that feeds the sample tracks to the sound card runs at ordinary
 * priority, with a short buffer. When the machine is busy -- the window, two
 * synth helpers, a plug-in loading -- it can be kept off the CPU for longer
 * than the buffer holds, the card runs dry, and that is a click or a pop in the
 * drums. PipeWire's own threads are not exposed to this because they ask the
 * system's RealtimeKit for realtime scheduling; this does the same.
 *
 * Two ways, in order: the scheduler directly (works where the user's rtprio
 * limit allows it), then RealtimeKit over the system bus. libdbus is opened at
 * run time, so nothing is linked or required: with no libdbus, no rtkit or a
 * refusal, the thread simply stays as it was and the caller is told why.
 *
 * RealtimeKit refuses a thread unless the process has an RLIMIT_RTTIME -- the
 * most CPU a realtime thread may use without blocking -- so one is set. The
 * audio thread blocks in the card's write every period, far inside it. */
#define _GNU_SOURCE
#include "trk.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The few libdbus things used, declared here so no dbus header is needed. The
 * two opaque structs are given generous room. */
typedef struct { char opaque[96]; } rt_dbus_error;
typedef struct { char opaque[256]; } rt_dbus_iter;
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;

#define RT_BUS_SYSTEM 1                  /* DBUS_BUS_SYSTEM */
#define RT_TYPE_UINT64 ((int)'t')
#define RT_TYPE_UINT32 ((int)'u')

static void rt_say(char *why, size_t n, const char *fmt, const char *a)
{
    if (why && n) snprintf(why, n, fmt, a ? a : "");
}

int trk_audio_thread_realtime(int priority, char *how, size_t hown)
{
    struct sched_param sp;
    void *lib;
    rt_dbus_error err;
    rt_dbus_iter it;
    DBusConnection *c;
    DBusMessage *m, *r;
    uint64_t tid;
    uint32_t prio;
    struct rlimit rl;
    int ok = 0;

    void (*error_init)(rt_dbus_error *);
    int  (*error_is_set)(const rt_dbus_error *);
    void (*error_free)(rt_dbus_error *);
    DBusConnection *(*bus_get_private)(int, rt_dbus_error *);
    void (*set_exit)(DBusConnection *, int);
    DBusMessage *(*new_call)(const char *, const char *, const char *, const char *);
    void (*init_append)(DBusMessage *, rt_dbus_iter *);
    int  (*append_basic)(rt_dbus_iter *, int, const void *);
    DBusMessage *(*send_block)(DBusConnection *, DBusMessage *, int, rt_dbus_error *);
    void (*msg_unref)(DBusMessage *);
    void (*conn_close)(DBusConnection *);
    void (*conn_unref)(DBusConnection *);

    if (how && hown) how[0] = 0;
    if (priority < 1) priority = 1;

    /* 1: straight from the scheduler, if the user's limits permit it. */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = priority;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) == 0) {
        rt_say(how, hown, "realtime (scheduler)%s", "");
        return 1;
    }

    /* 2: RealtimeKit, over the system bus. */
    /* NODELETE: libdbus keeps global state that only dbus_shutdown frees, and
     * unloading it here would orphan that state on every call -- one leak a time
     * the audio thread starts. Kept mapped, the state is reused by the next call
     * (and shared, harmlessly, with any other user of libdbus in the process). */
    lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!lib) { rt_say(how, hown, "no realtime priority: %s", "libdbus is not installed"); return 0; }
#define SYM(var, name) do { *(void **)&(var) = dlsym(lib, name); if (!(var)) { rt_say(how, hown, "no realtime priority: %s", "libdbus lacks " name); dlclose(lib); return 0; } } while (0)
    SYM(error_init, "dbus_error_init");
    SYM(error_is_set, "dbus_error_is_set");
    SYM(error_free, "dbus_error_free");
    SYM(bus_get_private, "dbus_bus_get_private");
    SYM(set_exit, "dbus_connection_set_exit_on_disconnect");
    SYM(new_call, "dbus_message_new_method_call");
    SYM(init_append, "dbus_message_iter_init_append");
    SYM(append_basic, "dbus_message_iter_append_basic");
    SYM(send_block, "dbus_connection_send_with_reply_and_block");
    SYM(msg_unref, "dbus_message_unref");
    SYM(conn_close, "dbus_connection_close");
    SYM(conn_unref, "dbus_connection_unref");
#undef SYM

    /* RealtimeKit wants a finite limit on how long a realtime thread may run
     * without blocking; 200 ms is its own ceiling. It is a limit on the whole
     * process, and a hard one that cannot be raised again, and a realtime thread
     * that outruns it is killed with the process. So a limit someone has already
     * set is left alone, and this one is only ever set when none is, and only
     * once. Only realtime threads are bound by it; the plug-ins here are not. */
    if (getrlimit(RLIMIT_RTTIME, &rl) == 0 && rl.rlim_max == RLIM_INFINITY) {
        rl.rlim_cur = rl.rlim_max = 200000;
        if (setrlimit(RLIMIT_RTTIME, &rl) != 0) { /* RealtimeKit will say if it matters */ }
    }

    memset(&err, 0, sizeof err);
    error_init(&err);
    c = bus_get_private(RT_BUS_SYSTEM, &err);
    if (!c) {
        rt_say(how, hown, "no realtime priority: %s", "no system bus");
        if (error_is_set(&err)) error_free(&err);
        dlclose(lib);
        return 0;
    }
    set_exit(c, 0);                      /* a lost bus must not take the program with it */
    m = new_call("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
                 "org.freedesktop.RealtimeKit1", "MakeThreadRealtime");
    if (m) {
        tid = (uint64_t)syscall(SYS_gettid);
        prio = (uint32_t)priority;
        init_append(m, &it);
        append_basic(&it, RT_TYPE_UINT64, &tid);
        append_basic(&it, RT_TYPE_UINT32, &prio);
        r = send_block(c, m, 1000, &err);
        if (r) { msg_unref(r); ok = 1; }
        msg_unref(m);
    }
    if (!ok) {
        rt_say(how, hown, "no realtime priority: %s",
               error_is_set(&err) && ((const char **)&err)[1] ? ((const char **)&err)[1] : "RealtimeKit refused");
        if (error_is_set(&err)) error_free(&err);
    } else {
        rt_say(how, hown, "realtime (RealtimeKit)%s", "");
    }
    conn_close(c);
    conn_unref(c);
    dlclose(lib);
    return ok;
}

/* Give the calling thread its ordinary scheduling back. A realtime thread that
 * has gone wrong -- rendering slower than the audio it makes, for a long run --
 * would starve the rest of the machine at its priority; the audio thread calls
 * this on itself when that happens and carries on, audibly late but no longer
 * a threat to the desktop. */
void trk_audio_thread_normal(void)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
}
