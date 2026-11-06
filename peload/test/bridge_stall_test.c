/* What a stalled helper costs the host's realtime thread.
 *
 *   bridge-stall-test <plug-in> [frames]
 *
 * Plays a held chord on a realtime thread, paced as a graph would, and stops the
 * helper for 200 ms part way through. The host must keep its thread moving, and
 * pick the plug-in up again afterwards without staying a block out of phase.
 * Silent: nothing is opened but the plug-in. Exits 77 with no realtime priority
 * or no helper, since then this does not test what it says. */
#define _GNU_SOURCE
#include "pehost.h"
#include "rtprio.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#define MAX_FRAMES 8192
#define RATE 48000.0

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static pid_t helper_of_this_process(void)
{
    char cmd[64];
    long pid = 0;
    FILE *f;
    snprintf(cmd, sizeof cmd, "pgrep -P %ld -n", (long)getpid());
    if ((f = popen(cmd, "r"))) {
        if (fscanf(f, "%ld", &pid) != 1) pid = 0;
        pclose(f);
    }
    return (pid_t)pid;
}

static float peak_of(const float *b, int n)
{
    float pk = 0;
    int i;
    for (i = 0; i < n * 2; i++) if (fabsf(b[i]) > pk) pk = fabsf(b[i]);
    return pk;
}

static struct { pehost *h; int frames; pid_t helper; int result; } G;

static void *audio(void *ud)
{
    static float out[MAX_FRAMES * 2];
    const double period = G.frames / RATE;
    const int settle = (int)(1.0 / period), stall_at = settle + (int)(0.5 / period);
    const int total = stall_at + (int)(1.5 / period);
    int k, silent_after = 0, silent_during = 0, resumed = 0;
    double next, worst_before = 0, worst_stall = 0, worst_after = 0, stop_at = 0;
    char how[160];
    (void)ud;

    pehost_thread_init();
    if (!rtprio_request(20, how, sizeof how)) { printf("%s\n", how); G.result = 77; return NULL; }
    printf("host thread: %s; %d frames is %.1f ms\n", how, G.frames, period * 1e3);

    next = now();
    for (k = 0; k < total; k++) {
        double t0, d;
        float pk;
        if (k == stall_at) { kill(G.helper, SIGSTOP); stop_at = now(); }
        if (stop_at > 0 && !resumed && now() - stop_at > 0.2) { kill(G.helper, SIGCONT); resumed = k; }
        t0 = now();
        pehost_render(G.h, out, G.frames);
        d = now() - t0;
        pk = peak_of(out, G.frames);
        if (k < stall_at) { if (k > settle && d > worst_before) worst_before = d; }
        else if (!resumed) { if (d > worst_stall) worst_stall = d; if (pk == 0) silent_during++; }
        else if (k > resumed + (int)(0.5 / period)) { if (d > worst_after) worst_after = d; if (pk == 0) silent_after++; }
        next += period;
        d = next - now();
        if (d > 0) {
            struct timespec ts = { 0, (long)(d * 1e9) };
            nanosleep(&ts, NULL);
        } else if (d < -0.5) next = now();
    }
    printf("longest block: before %.2f ms, during the stall %.2f ms, after %.2f ms\n",
           worst_before * 1e3, worst_stall * 1e3, worst_after * 1e3);
    printf("silent blocks: %d during the stall, %d in the last second\n", silent_during, silent_after);
    /* The budget is a period plus the wait to collect what is still in flight. */
    G.result = worst_stall > period + 0.003 || silent_after > 0 || worst_after > 2 * period;
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t t;
    int notes[] = { 48, 55, 60, 64, 67, 72 }, i;
    G.frames = argc > 2 ? atoi(argv[2]) : 256;
    if (argc < 2 || G.frames < 16 || G.frames > MAX_FRAMES) {
        fprintf(stderr, "usage: %s <plug-in> [frames]\n", argv[0]);
        return 2;
    }
    pehost_set_isolation(1);
    if (!(G.h = pehost_open(argv[1], RATE, G.frames))) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    if (!(G.helper = helper_of_this_process())) {
        printf("SKIP: this plug-in has no helper process\n");
        pehost_close(G.h);
        return 77;
    }
    for (i = 0; i < 6; i++) pehost_note_on(G.h, notes[i], 100);
    pthread_create(&t, NULL, audio, NULL);
    pthread_join(t, NULL);
    pehost_close(G.h);
    if (G.result == 77) { printf("SKIP: no realtime priority here\n"); return 77; }
    printf("%s\n", G.result ? "FAIL" : "ok");
    return G.result;
}
