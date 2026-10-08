/* A recorder of everything a studio plays.
 *
 * Several sources render audio on threads of their own -- the synth tabs' mix,
 * the tracker's sample tracks -- with no single place the whole mix passes
 * through. Each source adds what it renders into this bus, and one writer
 * thread takes the sum to a 16-bit stereo WAV.
 *
 * The sources share a 48 kHz clock but their callbacks are not aligned. Each
 * keeps a cursor of its own that starts at the wall-clock position of its first
 * block and moves on by exactly what it feeds, so a source's blocks join up with
 * no gap or overlap whatever the jitter between callbacks; a source quiet for a
 * while is put back on the wall clock. The writer takes only what every source
 * still feeding has gone past (less a margin), so each has delivered it -- measured
 * on the sources' own cursors, not the wall clock, which an audio device's clock
 * drifts from. Adding is atomic, so two audio threads can land on the same
 * frame, and nothing on the feeding side allocates, locks or makes a system
 * call. (The Qt shell has the same design in hostwindow.h's MixBus.) */
#ifndef MIXBUS_H
#define MIXBUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int      mixbus_register(void);                 /* a source's id, or -1 when they are all taken */
void     mixbus_release(int id);

/* Begin a take at `rate` Hz into `path`. 0, or -1 when the file would not open
 * or one is already running. */
int      mixbus_start(const char *path, int rate);
/* End it. Copies the file's path into `path` and returns 0; -1 when nothing was
 * recording. */
int      mixbus_stop(char *path, size_t n);

int      mixbus_active(void);
int      mixbus_failed(void);                   /* a write failed (a full disk): the file is short */
uint64_t mixbus_frames(void);                   /* written so far */
uint64_t mixbus_dropped(void);                  /* lost because the writer fell behind */

/* Audio threads: interleaved stereo. */
void     mixbus_feed_f(int id, const float *x, int frames);
void     mixbus_feed_d(int id, const double *x, int frames);

#ifdef __cplusplus
}
#endif
#endif
