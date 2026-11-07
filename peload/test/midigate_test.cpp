/* The keyboard gate in MidiIo, on the note path: a USB keyboard plays the
 * instance marked live and no other, and nothing it started is left on.
 *
 * Talks to the sequencer for real, but addresses the port under test directly
 * rather than a shared device, so no other program on the machine hears it. Only
 * channel 10 is looked at: open() subscribes to whatever keyboards are plugged
 * in, and someone may be playing one. */
#include "midiio.h"

#include <QCoreApplication>
#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <time.h>
#include <vector>

struct Ev { int st, d1, d2; };
static std::mutex        g_mx;
static std::vector<Ev>   g_got;
static int               g_fails;

static void ms(int n) { struct timespec t = { n / 1000, (n % 1000) * 1000000L }; nanosleep(&t, nullptr); }

static snd_seq_t *open_client(const char *name, int *port)
{
    snd_seq_t *s = nullptr;
    if (snd_seq_open(&s, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) return nullptr;
    snd_seq_set_client_name(s, name);
    *port = snd_seq_create_simple_port(s, "out", SND_SEQ_PORT_CAP_READ, SND_SEQ_PORT_TYPE_APPLICATION);
    return s;
}

/* Sent, then waited for: the reader is a thread of its own. */
static void put(snd_seq_t *s, int port, int dc, int dp, snd_seq_event_t &ev)
{
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_dest(&ev, dc, dp);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(s, &ev);
    ms(15);
}
#define CH 9
static void on (snd_seq_t *s, int p, int c, int d, int n) { snd_seq_event_t e; snd_seq_ev_clear(&e); snd_seq_ev_set_noteon(&e, CH, n, 100); put(s, p, c, d, e); }
static void off(snd_seq_t *s, int p, int c, int d, int n) { snd_seq_event_t e; snd_seq_ev_clear(&e); snd_seq_ev_set_noteoff(&e, CH, n, 0); put(s, p, c, d, e); }
static void cc (snd_seq_t *s, int p, int c, int d, int n, int v) { snd_seq_event_t e; snd_seq_ev_clear(&e); snd_seq_ev_set_controller(&e, CH, n, v); put(s, p, c, d, e); }
static void bend(snd_seq_t *s, int p, int c, int d, int v) { snd_seq_event_t e; snd_seq_ev_clear(&e); snd_seq_ev_set_pitchbend(&e, CH, v); put(s, p, c, d, e); }
static void clk(snd_seq_t *s, int p, int c, int d) { snd_seq_event_t e; snd_seq_ev_clear(&e); e.type = SND_SEQ_EVENT_CLOCK; put(s, p, c, d, e); }

/* What arrived since the last look, as text, compared with what should have. */
static void expect(const char *what, const char *want)
{
    char have[512] = "";
    {
        std::lock_guard<std::mutex> g(g_mx);
        for (const Ev &e : g_got) {
            char one[32];
            snprintf(one, sizeof one, "%s%02X.%d.%d", have[0] ? " " : "", e.st, e.d1, e.d2);
            strncat(have, one, sizeof have - strlen(have) - 1);
        }
        g_got.clear();
    }
    const bool ok = !strcmp(have, want);
    printf("%-52s %s", what, ok ? "ok\n" : "FAIL\n");
    if (!ok) { printf("   wanted [%s]\n   got    [%s]\n", want, have); g_fails++; }
}

int main(int argc, char **argv)
{
    setenv("PESTUDIO_TEST_KEYBOARD", "midigate kbd", 1);
    QCoreApplication app(argc, argv);

    MidiIo m;
    m.setRealtimeSink([](int st, int d1, int d2) {
        if (st >= 0xF0 || (st & 15) == CH) {
            std::lock_guard<std::mutex> g(g_mx);
            g_got.push_back({ st, d1, d2 });
        }
    });
    QString err;
    if (!m.open(&err)) { printf("SKIP: %s\n", err.toUtf8().constData()); return 77; }
    const QStringList addr = m.portName().split(':');
    const int dc = addr[0].toInt(), dp = addr[1].toInt();

    int kp, ap;
    snd_seq_t *kb  = open_client("midigate kbd", &kp);
    snd_seq_t *app2 = open_client("midigate app", &ap);
    if (!kb || !app2) { printf("SKIP: no sequencer client\n"); return 77; }

    on(kb, kp, dc, dp, 60);  off(kb, kp, dc, dp, 60);
    expect("live: a key plays", "99.60.100 89.60.0");

    m.setKeyboardLive(false); ms(80);
    on(kb, kp, dc, dp, 61);  off(kb, kp, dc, dp, 61);
    cc(kb, kp, dc, dp, 74, 10);  bend(kb, kp, dc, dp, 500);
    expect("back: a key, a knob and the wheel do not", "");

    clk(kb, kp, dc, dp);
    expect("back: the keyboard's clock still arrives", "F8.0.0");

    on(app2, ap, dc, dp, 62);  off(app2, ap, dc, dp, 62);
    expect("back: a program's port still plays", "99.62.100 89.62.0");

    m.setKeyboardLive(true); ms(80);
    on(kb, kp, dc, dp, 63);
    expect("key pressed in front", "99.63.100");
    m.setKeyboardLive(false); ms(80);
    expect("  gone to the back: nothing else is sent yet", "");
    off(kb, kp, dc, dp, 63);
    expect("  let go there: the release arrives", "89.63.0");
    off(kb, kp, dc, dp, 63);
    expect("  a second release does not", "");

    m.setKeyboardLive(true); ms(80);
    bend(kb, kp, dc, dp, 1000);  cc(kb, kp, dc, dp, 1, 90);  cc(kb, kp, dc, dp, 64, 127);
    expect("wheel, mod and pedal moved in front", "E9.104.71 B9.1.90 B9.64.127");
    m.setKeyboardLive(false); ms(80);
    expect("  gone to the back: all three come to rest", "E9.0.64 B9.1.0 B9.64.0");
    m.setKeyboardLive(true); ms(80);
    m.setKeyboardLive(false); ms(80);
    expect("  and once only", "");

    m.setKeyboardLive(true); ms(80);
    bend(kb, kp, dc, dp, 1000);  bend(kb, kp, dc, dp, 0);  cc(kb, kp, dc, dp, 64, 0);
    expect("wheel back at rest, pedal up", "E9.104.71 E9.0.64 B9.64.0");
    m.setKeyboardLive(false); ms(80);
    expect("  nothing to bring to rest", "");

    /* Back again on a channel of its own: chosen, so it keeps its keyboard. */
    m.setChannelFilter(CH);
    on(kb, kp, dc, dp, 64);  off(kb, kp, dc, dp, 64);
    expect("back, but on its chosen channel: a key plays", "99.64.100 89.64.0");
    m.setChannelFilter(-1);

    m.stopInput();
    snd_seq_close(kb);
    snd_seq_close(app2);
    printf("%s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
