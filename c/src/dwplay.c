/* dwplay -- play the DW-8000 engine live.
 *
 * Two input paths, both active at once:
 *   - ALSA sequencer MIDI, auto-connected to any hardware keyboard found, so
 *     a controller or a virtual keyboard like vmpk just works.
 *   - The computer keyboard, tracker layout, for when nothing is plugged in.
 *
 * A terminal gives key-press but no key-release, so computer-keyboard notes
 * are one-shot: they trigger and release themselves after a fixed gate. A real
 * MIDI keyboard gets proper note-on/note-off and full velocity.
 *
 * The loop below is the whole program but not its command line: dwplay_cli.c
 * has that, and `dw` calls dwplay_run directly rather than starting a second
 * process to do the same thing. See dwplay.h. */

#define _POSIX_C_SOURCE 200809L

#include "dwplay.h"

#include "bank.h"
#include "dw_synth.h"
#include "dw_wavetable.h"
#include "rom.h"
#include "wav.h"
#include "wavedst.h"

#include <alsa/asoundlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SR       48000
/* One PipeWire quantum (1024 frames at 48 kHz = 21.3 ms). The audio thread
 * cannot get realtime priority here -- `ulimit -r` is 0 and the user is not in
 * an audio group -- so a 256-frame, 5.3 ms deadline at normal priority gets
 * preempted by the GUI or another application and underruns, which is heard as
 * hiss and crackle. Matching the quantum and asking for a deeper buffer trades
 * latency for not dropping samples. */
#define PERIOD    1024

/* A terminal reports key-press but not key-release, so a computer-keyboard
 * note has to release itself. It can still be made to feel like holding: the
 * terminal auto-repeats a held key, so the first press opens a window long
 * enough to bridge the initial repeat delay (~0.5 s on most setups), and each
 * repeat afterwards refreshes a much shorter one. Hold a key and the note
 * sustains; let go and it stops promptly. */
#define KEY_GATE_DEF 0.75   /* first press, covers the pre-repeat gap */
#define KEY_HOLD_DEF 0.16   /* refreshed by each auto-repeat */

/* Overridable, because the repeat delay is a compositor setting and varies:
 * 500 ms under GNOME's defaults, 600 ms under KWin's. If it exceeds KEY_GATE
 * a held key releases before the first repeat arrives and then retriggers,
 * which sounds like a stutter. `dwplay --keys` measures the real numbers. */
static double key_gate = KEY_GATE_DEF;
static double key_hold = KEY_HOLD_DEF;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void key_settings_from_env(void)
{
    const char *s;
    char *e;
    double v;
    if ((s = getenv("DW_KEY_GATE")) && *s) { v = strtod(s, &e); if (e != s && v > 0) key_gate = v; }
    if ((s = getenv("DW_KEY_HOLD")) && *s) { v = strtod(s, &e); if (e != s && v > 0) key_hold = v; }
}

static volatile sig_atomic_t running = 1;
static unsigned long g_xruns;   /* ALSA underruns, reported on exit */
static int g_midi_ch = -1;      /* -1 omni, else 0-15; see -c */
static void on_sigint(int s) { (void)s; running = 0; }

/* ---- terminal ---- */

static struct termios saved_term;
static int            term_raw = 0;

static void term_restore(void)
{
    if (term_raw) { tcsetattr(STDIN_FILENO, TCSANOW, &saved_term); term_raw = 0; }
}

static void term_setup(void)
{
    struct termios t;
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &saved_term)) return;
    t = saved_term;
    t.c_lflag &= (unsigned)~(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) { term_raw = 1; atexit(term_restore); }
}

/* Shows what the engine just received, so it is obvious the keyboard is live
 * and which note actually got triggered. Redrawn in place on one line. */
static void show_note(int note, const char *src)
{
    static const char *nm[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    printf("\r  \033[K%-4s %-3s%-2d  (note %3d)   ", src, nm[note % 12], note / 12 - 1, note);
    fflush(stdout);
}

/* Tracker layout: two octaves across the bottom two letter rows. */
static int key_to_semitone(int c)
{
    static const char *lo = "zsxdcvgbhnjm";   /* C  C# D  D# E  F  F# G  G# A  A# B */
    static const char *hi = "q2w3er5t6y7u";
    const char *p;
    if ((p = strchr(lo, c))) return (int)(p - lo);
    if ((p = strchr(hi, c))) return (int)(p - hi) + 12;
    return -1;
}

/* Step to another program and announce it. Shared by [ ] and the arrow keys. */
static void step_preset(dw_synth *syn, const bank *bk, int *prog, int delta)
{
    if (!bk || bk->count <= 0) return;
    *prog = ((*prog + delta) % bk->count + bk->count) % bk->count;
    dw_synth_set_program(syn, bk->prog[*prog].param);
    printf("\r  \033[Kpreset %d: %s\n", *prog, bk->prog[*prog].name);
    fflush(stdout);
}

/* --keys: show the note map and measure what the terminal actually does with
 * a held key. The repeat delay/rate is a compositor setting with no portable
 * way to query it under Wayland, so measure it rather than trust a config
 * file -- the numbers here are what dwplay itself sees. */
static int keyboard_info(void)
{
    static const char *nm[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    const char *lo = "zsxdcvgbhnjm", *hi = "q2w3er5t6y7u";
    double last_t = 0.0, first_gap = -1.0, gaps[64];
    int    last_c = -1, ngaps = 0, i;

    printf("\nnote map (base octave C3 = MIDI 48, shift with - and =)\n\n  lower  ");
    for (i = 0; i < 12; i++) printf("%c=%-3s", lo[i], nm[i]);
    printf("\n  upper  ");
    for (i = 0; i < 12; i++) printf("%c=%-3s", hi[i], nm[i]);
    printf("\n\n  [ ] preset   - = octave   space all-notes-off   Ctrl-C / shift-Q quit\n");

    printf("\ncurrent gate settings (override with DW_KEY_GATE / DW_KEY_HOLD):\n"
           "  first press holds  %.2f s\n"
           "  each repeat holds  %.2f s\n", key_gate, key_hold);

    if (!isatty(STDIN_FILENO)) {
        printf("\n(stdin is not a terminal -- cannot measure repeat timing)\n");
        return 0;
    }

    term_setup();
    printf("\nhold down one letter key for ~2 s, then press shift-Q...\n\n");
    fflush(stdout);

    for (;;) {
        unsigned char ch;
        double t;
        if (read(STDIN_FILENO, &ch, 1) != 1) { struct timespec ns = {0, 2000000}; nanosleep(&ns, NULL); continue; }
        if (ch == 'Q' || ch == 3) break;
        t = now_sec();
        if ((int)ch == last_c && last_t > 0.0) {
            double gap = t - last_t;
            if (first_gap < 0.0)        first_gap = gap;
            else if (ngaps < 64)        gaps[ngaps++] = gap;
        } else {
            first_gap = -1.0; ngaps = 0;
        }
        last_c = ch; last_t = t;
    }
    term_restore();

    printf("\n");
    if (first_gap < 0.0) {
        printf("no repeats seen -- either the key was not held, or key repeat is off.\n"
               "With repeat off, a held key cannot sustain; raise DW_KEY_GATE instead.\n");
        return 0;
    }
    {
        double sum = 0.0;
        for (i = 0; i < ngaps; i++) sum += gaps[i];
        printf("measured: repeat delay %.0f ms", first_gap * 1000.0);
        if (ngaps) printf(", then %.0f ms between repeats (%.0f/s)",
                          sum / ngaps * 1000.0, ngaps / sum);
        printf("\n\n");
        if (first_gap > key_gate)
            printf("PROBLEM: the delay (%.0f ms) is longer than the first-press gate\n"
                   "(%.0f ms), so a held note releases before the first repeat and then\n"
                   "retriggers -- that is the stutter. Fix with:\n"
                   "  DW_KEY_GATE=%.2f ./dw.sh live\n",
                   first_gap * 1000.0, key_gate * 1000.0, first_gap + 0.15);
        else
            printf("OK: the gate (%.0f ms) covers the repeat delay (%.0f ms), so holding\n"
                   "a key sustains cleanly.\n", key_gate * 1000.0, first_gap * 1000.0);
    }
    return 0;
}

/* ---- ALSA sequencer ---- */

/* Subscribes our input port to every hardware MIDI source we can find.
 * Returns how many connections were made. */
static int midi_autoconnect(snd_seq_t *seq, int my_port)
{
    snd_seq_client_info_t *cinfo = NULL;
    snd_seq_port_info_t   *pinfo = NULL;
    int connected = 0;
    const unsigned int need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;

    /* The _alloca forms these are usually written with need alloca(), which
     * strict c99 does not declare; the heap forms are equivalent. */
    if (snd_seq_client_info_malloc(&cinfo) < 0) return 0;
    if (snd_seq_port_info_malloc(&pinfo) < 0) { snd_seq_client_info_free(cinfo); return 0; }
    snd_seq_client_info_set_client(cinfo, -1);

    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == SND_SEQ_CLIENT_SYSTEM || client == snd_seq_client_id(seq))
            continue;

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            unsigned int type = snd_seq_port_info_get_type(pinfo);
            snd_seq_addr_t src;

            if ((caps & need) != need) continue;
            if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;

            src.client = client;
            src.port   = snd_seq_port_info_get_port(pinfo);
            if (snd_seq_connect_from(seq, my_port, src.client, src.port) == 0) {
                printf("  MIDI in: %s: %s (%d:%d)\n",
                       snd_seq_client_info_get_name(cinfo),
                       snd_seq_port_info_get_name(pinfo), src.client, src.port);
                connected++;
            }
        }
    }
    snd_seq_port_info_free(pinfo);
    snd_seq_client_info_free(cinfo);
    return connected;
}

int dwplay_keyboard_info(void)
{
    key_settings_from_env();
    return keyboard_info();
}

int dwplay_pcm(const double *interleaved, size_t frames, int samplerate)
{
    snd_pcm_t *pcm = NULL;
    short     *out;
    size_t     i;
    int        rc = -1;

    if (!frames) return 0;
    if (!(out = malloc(frames * 2 * sizeof *out))) return -1;
    for (i = 0; i < frames * 2; i++) {
        double v = interleaved[i] * 32767.0;
        out[i] = (short)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
    }

    /* A 200 ms buffer: this writes a finished render rather than synthesising
     * against a deadline, so latency is irrelevant and not underrunning is
     * not. */
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "cannot open audio device\n");
        free(out);
        return -1;
    }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           2, (unsigned)samplerate, 1, 200000) < 0) {
        fprintf(stderr, "cannot configure audio device\n");
        goto done;
    }
    {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, out, (snd_pcm_uframes_t)frames);
        if (n < 0) n = snd_pcm_recover(pcm, (int)n, 1);
        if (n < 0) { fprintf(stderr, "playback failed: %s\n", snd_strerror((int)n)); goto done; }
    }
    snd_pcm_drain(pcm);
    rc = 0;

done:
    snd_pcm_close(pcm);
    free(out);
    return rc;
}

/* ---- the live session ---- */

#define ONESHOTS 16     /* computer-keyboard notes waiting to release */

/* The state a run works on. A struct rather than a dozen locals threaded
 * through every stage below, which is what it was when all of this lived in
 * one three-hundred-line function. */
typedef struct {
    wavedst      wd;
    dw_wavetable wt;
    dw_synth     syn;
    bank         bk;
    int          have_bank;
    int          prog;
    int          transpose;
    /* Notes started from the computer keyboard, released on a timer. */
    struct { int note, active; double left; } oneshot[ONESHOTS];
} session;

static int session_open(session *s, const dwplay_opts *o)
{
    memset(s, 0, sizeof *s);
    s->prog = o->program;

    if (o->proginit) {
        const char *whence = o->proginit_name ? o->proginit_name : "preset bank";
        if (rom_bank_parse(&s->bk, o->proginit, o->proginit_size, whence)) return -1;
        s->have_bank = 1;
        if (s->prog < 0 || s->prog >= s->bk.count) s->prog = 0;
    }

    /* The blobs belong to the caller and outlive this call; wavedst_load and
     * bank_parse both decode into storage of their own, so nothing here has to
     * hold on to them. */
    if (wavedst_load(&s->wd, o->wavedst, o->wavedst_size, 0)) {
        fprintf(stderr, "bad WAVEDST\n"); return -1;
    }
    if (dw_wavetable_build(&s->wt, &s->wd, SR)) {
        fprintf(stderr, "mip build failed\n"); return -1;
    }
    if (dw_synth_init(&s->syn, &s->wt, SR)) {
        fprintf(stderr, "synth init failed\n"); return -1;
    }
    if (s->have_bank) dw_synth_set_program(&s->syn, s->bk.prog[s->prog].param);
    return 0;
}

static void session_close(session *s)
{
    dw_synth_free(&s->syn);
    dw_wavetable_free(&s->wt);
    wavedst_free(&s->wd);
    if (s->have_bank) bank_free(&s->bk);
}

/* ---- devices ---- */

static snd_pcm_t *audio_open(void)
{
    snd_pcm_t *pcm = NULL;

    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "cannot open audio device\n");
        return NULL;
    }
    /* 50 ms; see DW_PERIOD/DW_LATENCY in dwstudio */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           2, SR, 1, 50000) < 0) {
        fprintf(stderr, "cannot configure audio device\n");
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

static snd_seq_t *midi_open(int *port)
{
    snd_seq_t *seq = NULL;

    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        fprintf(stderr, "cannot open ALSA sequencer\n");
        return NULL;
    }
    snd_seq_set_client_name(seq, "dwplay");
    *port = snd_seq_create_simple_port(seq, "dwplay in",
                SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTHESIZER);
    return seq;
}

/* ---- input ---- */

static void midi_drain(session *s, snd_seq_t *seq)
{
    snd_seq_event_t *ev;

    while (snd_seq_event_input(seq, &ev) >= 0) {
        if (g_midi_ch >= 0 &&
            (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF ||
             ev->type == SND_SEQ_EVENT_PGMCHANGE || ev->type == SND_SEQ_EVENT_CONTROLLER) &&
            ev->data.note.channel != g_midi_ch)
            continue;

        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            /* A note-on at velocity 0 is a note-off; running status makes that
             * the usual way keyboards release a key. */
            if (ev->data.note.velocity > 0) {
                dw_synth_note_on(&s->syn, ev->data.note.note, ev->data.note.velocity);
                show_note(ev->data.note.note, "midi");
            } else {
                dw_synth_note_off(&s->syn, ev->data.note.note);
            }
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            dw_synth_note_off(&s->syn, ev->data.note.note);
            break;
        case SND_SEQ_EVENT_PGMCHANGE:
            if (s->have_bank) {
                s->prog = ev->data.control.value % s->bk.count;
                dw_synth_set_program(&s->syn, s->bk.prog[s->prog].param);
                printf("  preset %d: %s\n", s->prog, s->bk.prog[s->prog].name);
                fflush(stdout);
            }
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            if (ev->data.control.param == 123) dw_synth_all_off(&s->syn);   /* all notes off */
            break;
        default: break;
        }
    }
}

/* Three octaves either way, and the line is cleared first because show_note
 * leaves the cursor parked on one. */
static void transpose_by(session *s, int semitones)
{
    int t = s->transpose + semitones;

    if (t < -36 || t > 36) return;
    s->transpose = t;
    printf("\r  \033[Koctave %+d\n", s->transpose / 12);
    fflush(stdout);
}

/* Already sounding means the terminal is repeating a held key: extend the note
 * rather than retrigger it, so the envelope does not restart on every repeat. */
static void key_note_on(session *s, int note)
{
    int k;

    for (k = 0; k < ONESHOTS; k++)
        if (s->oneshot[k].active && s->oneshot[k].note == note) {
            s->oneshot[k].left = key_hold;
            return;
        }

    dw_synth_note_on(&s->syn, note, 100);
    show_note(note, "key");
    for (k = 0; k < ONESHOTS; k++)
        if (!s->oneshot[k].active) {
            s->oneshot[k].active = 1;
            s->oneshot[k].note   = note;
            s->oneshot[k].left   = key_gate;
            return;
        }
}

/* The final byte of a CSI/SS3 sequence. */
static void arrow_key(session *s, int final)
{
    switch (final) {
    case 'A': step_preset(&s->syn, s->have_bank ? &s->bk : NULL, &s->prog, +1); break;
    case 'B': step_preset(&s->syn, s->have_bank ? &s->bk : NULL, &s->prog, -1); break;
    case 'C': transpose_by(s, +12); break;
    case 'D': transpose_by(s, -12); break;
    default: break;                            /* other sequences ignored */
    }
}

/* One ordinary key. Returns 0 if it was the quit key. */
static int key_press(session *s, int c)
{
    int semi;

    if (c == 3 || c == 'Q') return 0;                       /* Ctrl-C, shift-Q */
    if (c == ' ') {
        dw_synth_all_off(&s->syn);
        memset(s->oneshot, 0, sizeof s->oneshot);
        return 1;
    }
    if (c == '-') { transpose_by(s, -12); return 1; }
    if (c == '=') { transpose_by(s, +12); return 1; }
    if ((c == '[' || c == ']') && s->have_bank) {
        step_preset(&s->syn, &s->bk, &s->prog, c == ']' ? +1 : -1);
        return 1;
    }
    if ((semi = key_to_semitone(c)) >= 0) key_note_on(s, 48 + semi + s->transpose);
    return 1;
}

/* Consume whatever complete keys `kb` holds, leaving any partial escape
 * sequence for the next block. Returns 0 when the user asked to quit.
 *
 * Bytes are buffered rather than handled as they arrive because an escape
 * sequence can be split across reads, and an arrow key is three bytes
 * (ESC [ A). */
static int keyboard_drain(session *s, unsigned char *kb, int *kbn, int *esc_stall)
{
    int kbpos = 0, quit = 0;

    while (kbpos < *kbn) {
        int c = kb[kbpos];

        if (c == 27) {
            /* CSI (ESC [) or SS3 (ESC O), then optional parameter bytes, then
             * a final byte in 0x40..0x7e. */
            int j = kbpos + 1, k;
            if (j >= *kbn) break;                       /* wait for more */
            if (kb[j] == '[' || kb[j] == 'O') {
                for (k = j + 1; k < *kbn; k++)
                    if (kb[k] >= 0x40 && kb[k] <= 0x7e) break;
                if (k >= *kbn) break;                   /* wait for the final byte */
                arrow_key(s, kb[k]);
                kbpos = k + 1;
            } else {
                kbpos = j + 1;                          /* ESC + a plain byte */
            }
            *esc_stall = 0;
            continue;
        }

        kbpos++;
        if (!key_press(s, c)) { quit = 1; break; }
    }

    /* Keep whatever was left mid-sequence for the next block. If it never
     * completes -- a bare ESC, which nothing is bound to -- drop it rather
     * than letting it wedge the buffer. */
    if (kbpos > 0) {
        memmove(kb, kb + kbpos, (size_t)(*kbn - kbpos));
        *kbn -= kbpos;
        *esc_stall = 0;
    } else if (*kbn > 0 && ++*esc_stall > 20) {
        *kbn = 0;
        *esc_stall = 0;
    }
    return !quit;
}

/* ---- output ---- */

/* Age the computer-keyboard one-shots by exactly one block. */
static void age_oneshots(session *s)
{
    int i;

    for (i = 0; i < ONESHOTS; i++) {
        if (!s->oneshot[i].active) continue;
        s->oneshot[i].left -= (double)PERIOD / (double)SR;
        if (s->oneshot[i].left <= 0.0) {
            dw_synth_note_off(&s->syn, s->oneshot[i].note);
            s->oneshot[i].active = 0;
        }
    }
}

/* An xrun or a sink change should not end the session. Recover in place and
 * keep going; only give up if the device stays broken. Returns 0 when it has. */
static int audio_write(snd_pcm_t *pcm, const short *pcmbuf, int *consecutive_errors)
{
    long n = snd_pcm_writei(pcm, pcmbuf, PERIOD);

    if (n >= 0) { *consecutive_errors = 0; return 1; }

    g_xruns++;
    if (snd_pcm_recover(pcm, (int)n, 1) >= 0 || snd_pcm_prepare(pcm) >= 0) {
        *consecutive_errors = 0;
        return 1;
    }
    if (++*consecutive_errors > 200) {
        fprintf(stderr, "\naudio device stopped responding: %s\n", snd_strerror((int)n));
        return 0;
    }
    return 1;
}

static void banner(const session *s, snd_seq_t *seq, int port)
{
    printf("\ndwplay -- %d waves, %d voices, %d Hz\n\n", s->wt.nwaves, s->syn.nvoices, SR);
    if (midi_autoconnect(seq, port) == 0)
        printf("  MIDI in: nothing found (use the computer keyboard, or run\n"
               "           'aconnect <src> dwplay' in another terminal)\n");
    printf("\n  keys   zsxdcvgbhnjm = lower octave,  q2w3er5t6y7u = upper\n"
           "         up/down or [ ] preset   left/right or - = octave\n"
           "         space = all notes off\n"
           "         Ctrl-C or shift-Q to quit\n\n");
    if (s->have_bank) printf("  preset %d: %s\n", s->prog, s->bk.prog[s->prog].name);
    fflush(stdout);
}

/* ---- the loop ---- */

int dwplay_run(const dwplay_opts *o)
{
    session    s;
    snd_seq_t *seq = NULL;
    snd_pcm_t *pcm = NULL;
    int        port = -1, audio_errors = 0, i, rc = 1;

    double *buf = NULL, *rec = NULL;
    short  *pcmbuf = NULL;
    size_t  recn = 0, reccap = 0;

    /* Raw terminal input, buffered so a split escape sequence still parses. */
    unsigned char kb[64];
    int           kbn = 0, esc_stall = 0;

    g_midi_ch = (o->midi_channel >= 1 && o->midi_channel <= 16)
                    ? o->midi_channel - 1 : -1;

    dw_tuning_from_env();
    key_settings_from_env();

    /* record_path captures everything played, capped at two minutes. Mostly a
     * test hook: it makes "did pressing a key actually make sound?"
     * answerable. */
    if (o->record_path) {
        reccap = (size_t)SR * 120 * 2;
        if (!(rec = malloc(reccap * sizeof *rec))) { fprintf(stderr, "out of memory\n"); return 1; }
    }

    if (session_open(&s, o)) goto done;
    if (!(pcm = audio_open())) goto done;
    if (!(seq = midi_open(&port))) goto done;

    buf    = malloc((size_t)PERIOD * 2 * sizeof *buf);
    pcmbuf = malloc((size_t)PERIOD * 2 * sizeof *pcmbuf);
    if (!buf || !pcmbuf) { fprintf(stderr, "out of memory\n"); goto done; }

    signal(SIGINT, on_sigint);
    term_setup();
    banner(&s, seq, port);

    while (running) {
        midi_drain(&s, seq);

        /* VMIN=0/VTIME=0 makes read() return 0 immediately when idle, so this
         * never blocks the audio loop. */
        if (term_raw) {
            ssize_t r;
            while (kbn < (int)sizeof kb &&
                   (r = read(STDIN_FILENO, kb + kbn, sizeof kb - (size_t)kbn)) > 0)
                kbn += (int)r;
        }
        if (!keyboard_drain(&s, kb, &kbn, &esc_stall)) running = 0;

        dw_synth_render(&s.syn, buf, PERIOD);
        if (rec && recn + (size_t)PERIOD * 2 <= reccap) {
            memcpy(rec + recn, buf, (size_t)PERIOD * 2 * sizeof *rec);
            recn += (size_t)PERIOD * 2;
        }
        for (i = 0; i < PERIOD * 2; i++) {
            double v = buf[i] * 32767.0;
            pcmbuf[i] = (short)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
        }

        age_oneshots(&s);
        if (!audio_write(pcm, pcmbuf, &audio_errors)) running = 0;
    }

    term_restore();
    printf("\n");
    if (g_xruns) printf("audio underruns: %lu\n", g_xruns);
    if (rec && recn && !wav_write_stereo16(o->record_path, rec, recn / 2, SR))
        printf("captured %.1fs -> %s\n", (double)recn / (2.0 * SR), o->record_path);
    rc = 0;

done:
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); }
    if (seq) snd_seq_close(seq);
    session_close(&s);
    free(rec); free(buf); free(pcmbuf);
    return rc;
}
