/* dwplay -- the live DW-8000: ALSA MIDI in, computer keyboard, audio out.
 *
 * Split from its command line so that `dw` can run the same loop in its own
 * process instead of starting a second one. dwplay_cli.c is the standalone
 * program and does nothing this file does not: it turns argv into the struct
 * below, reads the two blobs off disk, and calls in.
 *
 * Only built when ALSA is present -- see HAVE_ALSA in the Makefile. */
#ifndef FB_DWPLAY_H
#define FB_DWPLAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The wavetable ROM, already in memory. The caller may have read it from a
     * file or pulled it out of a plugin's resources; this does not care which,
     * which is what lets `dw` skip the extract-to-disk step entirely. */
    const unsigned char *wavedst;
    size_t               wavedst_size;

    /* A PROGINIT bank, or NULL to run on the engine's built-in defaults. */
    const unsigned char *proginit;
    size_t               proginit_size;
    const char          *proginit_name;   /* what to call it if it is rejected */

    int         program;        /* index into the bank, clamped if out of range */
    const char *record_path;    /* capture everything played, or NULL */
    int         midi_channel;   /* 1..16, or 0 for omni */
} dwplay_opts;

/* Runs until Ctrl-C or shift-Q. Returns a process exit status. */
int dwplay_run(const dwplay_opts *o);

/* The note map, the key-repeat settings, and a live measurement of what this
 * terminal actually does with a held key. `dwplay --keys`, `dw keys`. */
int dwplay_keyboard_info(void);

/* Play an interleaved stereo buffer through the default device and wait for it
 * to finish. Returns 0 on success.
 *
 * `dw play` and `dw demo` use this. Without it they would have to write a WAV
 * to a temporary directory and hunt for a pw-play/paplay/aplay to hand it to,
 * which is what the shell launcher did and what it most obviously should not
 * have had to do. */
int dwplay_pcm(const double *interleaved, size_t frames, int samplerate);

#ifdef __cplusplus
}
#endif

#endif /* FB_DWPLAY_H */
