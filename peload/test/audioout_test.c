/* audioout: the ALSA and JACK backends against what is here to run them.
 * ALSA goes to the "null" device; JACK needs a server -- run this under pw-jack
 * (or a real jackd) to exercise it, and without one the JACK checks say so and
 * are skipped. */
#include "audioout.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
static void check(int ok, const char *what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) g_fail++; }

static atomic_ulong g_calls, g_frames;
static atomic_int g_maxframes;
static void render(void *ud, float *out, int frames)
{
    int i, m;
    (void)ud;
    atomic_fetch_add(&g_calls, 1);
    atomic_fetch_add(&g_frames, (unsigned long)frames);
    m = atomic_load(&g_maxframes);
    if (frames > m) atomic_store(&g_maxframes, frames);
    for (i = 0; i < frames; i++) { out[2 * i] = 0.0f; out[2 * i + 1] = 0.0f; }
}
static void nap(int ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

int main(void)
{
    char err[256], why[128];
    ao *a;
    ao_choice c, d;
    char names[24][128], labels[24][96];
    int n;

    printf("alsa\n");
    atomic_store(&g_calls, 0); atomic_store(&g_frames, 0); atomic_store(&g_maxframes, 0);
    a = ao_open(AO_ALSA, "null", "t", 48000, 256, render, NULL, err, sizeof err);
    check(a != NULL, "the null device opens");
    nap(300);
    check(a && atomic_load(&g_calls) >= 10, "the render function is called, steadily");
    check(atomic_load(&g_maxframes) <= 256, "in blocks no larger than the chunk");
    check(a && ao_alive(a) && strstr(ao_describe(a), "ALSA"), "it is alive, and says what it is");
    ao_close(a);
    {
        const unsigned long after = atomic_load(&g_calls);
        nap(100);
        check(atomic_load(&g_calls) == after, "closing stops the calls for good");
    }
    a = ao_open(AO_ALSA, "no-such-device-anywhere", "t", 48000, 256, render, NULL, err, sizeof err);
    check(a == NULL && err[0], "a device that does not exist is refused, with a reason");
    check(ao_open(AO_PIPEWIRE, "", "t", 48000, 256, render, NULL, err, sizeof err) == NULL, "PipeWire is not this library's");
    check(ao_open(AO_ALSA, "null", "t", 48000, 256, NULL, NULL, err, sizeof err) == NULL, "no render function: refused");
    ao_close(NULL);                                           /* must not crash */
    for (n = 0; n < 40; n++) {                                /* open and close in a loop: nothing piles up */
        a = ao_open(AO_ALSA, "null", "t", 48000, 128, render, NULL, err, sizeof err);
        if (!a) break;
        ao_close(a);
    }
    check(n == 40, "forty opens and closes in a row");

    printf("devices\n");
    n = ao_alsa_devices(names, labels, 24);
    check(n >= 1 && !strcmp(names[0], "default"), "the system default is listed first");

    printf("jack\n");
    if (ao_jack_available(why, sizeof why)) {
        atomic_store(&g_calls, 0); atomic_store(&g_frames, 0); atomic_store(&g_maxframes, 0);
        a = ao_open(AO_JACK, "", "studio-test", 48000, 64, render, NULL, err, sizeof err);
        check(a != NULL, err[0] ? err : "a JACK client opens");
        nap(400);
        check(a && atomic_load(&g_calls) >= 10, "JACK calls the render function");
        check(atomic_load(&g_maxframes) <= 64, "split into chunks no larger than asked, whatever JACK's own block");
        check(a && strstr(ao_describe(a), "JACK"), "and says what it is");
        ao_close(a);
        a = ao_open(AO_JACK, "", "studio-test", 11025, 64, render, NULL, err, sizeof err);
        check(a == NULL && strstr(err, "Hz"), "a rate JACK is not running at is refused, saying so");
        ao_close(a);
    } else {
        printf("  (skipped: %s)\n", why);
        a = ao_open(AO_JACK, "", "studio-test", 48000, 64, render, NULL, err, sizeof err);
        check(a == NULL && err[0], "with no JACK, the open fails with a reason");
    }

    printf("the choice\n");
    {
        char dir[] = "/tmp/aotest-XXXXXX";
        mkdtemp(dir);
        setenv("XDG_CONFIG_HOME", dir, 1);
        unsetenv("DW_BACKEND");
        ao_choice_load(&c);
        check(c.backend == AO_AUTO && !c.device[0], "with nothing saved: automatic");
        c.backend = AO_ALSA; snprintf(c.device, sizeof c.device, "plughw:CARD=X,DEV=0");
        check(ao_choice_save(&c) == 0, "saved");
        ao_choice_load(&d);
        check(d.backend == AO_ALSA && !strcmp(d.device, c.device), "and read back");
        setenv("DW_BACKEND", "jack", 1);
        ao_choice_load(&d);
        check(d.backend == AO_JACK, "DW_BACKEND overrides the backend");
        setenv("DW_BACKEND", "nonsense", 1);
        ao_choice_load(&d);
        check(d.backend == AO_ALSA, "a name that is none of them is ignored");
        unsetenv("DW_BACKEND");
        c.backend = AO_JACK; snprintf(c.device, sizeof c.device, "bad\nline");
        ao_choice_save(&c);
        ao_choice_load(&d);
        check(d.backend == AO_JACK && !strchr(d.device, '\n'), "a device name with a line break does not corrupt the file");
        { char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) { } }
    }
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail;
}
