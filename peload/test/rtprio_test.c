/* Does a guarded realtime thread survive running far past RealtimeKit's limit?
 *
 *   rtprio-test
 *
 * Runs in a realtime thread for longer than the 200 ms at which the kernel kills
 * the process. Getting to the end is most of the test: failing, this dies on
 * SIGKILL and says nothing. Also checks that a thread can give realtime back,
 * which the kernel refuses unless the reset-on-fork flag stays in the request.
 * Exits 77 where no realtime priority can be had, since nothing was tested. */
#define _GNU_SOURCE
#include "rtprio.h"

#include <time.h>

static int policy(void) { return sched_getscheduler(0) & ~SCHED_RESET_ON_FORK; }
static int is_rt(void)  { return policy() == SCHED_FIFO || policy() == SCHED_RR; }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void *gives_back(void *ud)
{
    char how[160];
    int *result = ud;
    if (!rtprio_request(20, how, sizeof how)) { *result = 77; return NULL; }
    rtprio_normal();
    *result = is_rt() ? 1 : 0;
    printf("%s, then rtprio_normal: %s\n", how, is_rt() ? "STILL REALTIME" : "ordinary");
    return NULL;
}

static void *overruns(void *ud)
{
    char how[160];
    int *result = ud;
    volatile unsigned long spin = 0;
    double t0;
    if (!rtprio_request_guarded(20, how, sizeof how)) {
        printf("%s\n", how);
        *result = 77;
        return NULL;
    }
    printf("guarded: %s\n", how);
    t0 = now();
    while (now() - t0 < 0.6) spin++;
    *result = is_rt() ? 1 : 0;
    printf("ran %.0f ms without blocking and lived, now %s\n",
           (now() - t0) * 1e3, is_rt() ? "STILL REALTIME" : "ordinary");
    rtprio_guard_release();
    return NULL;
}

int main(void)
{
    pthread_t t;
    int r1 = 2, r2 = 2;
    pthread_create(&t, NULL, gives_back, &r1);
    pthread_join(t, NULL);
    pthread_create(&t, NULL, overruns, &r2);
    pthread_join(t, NULL);
    if (r1 == 77 || r2 == 77) { printf("SKIP: no realtime priority here\n"); return 77; }
    printf("%s\n", r1 || r2 ? "FAIL" : "ok");
    return r1 || r2;
}
