/* audioout -- the studio's synth audio, to JACK or to ALSA directly.
 *
 * PipeWire is the studio's own path and lives in the hosts; this adds the two
 * others, behind one small interface, and the choice between all three:
 * kept in a file, named on a menu, changed while running.
 *
 *   JACK   a native client, "studio", two output ports, connected to the first
 *          two physical playback ports. libjack is loaded when it is asked for,
 *          so nothing here needs JACK installed -- and under pw-jack it is
 *          PipeWire's own. A server that is not running is not started.
 *   ALSA   a device written to by a thread of its own: the system default, or
 *          any PCM ALSA names (a card through plughw, PipeWire's or JACK's ALSA
 *          plug-in, dmix).
 *
 * The caller's render function fills interleaved stereo float and is called from
 * the backend's realtime thread, in blocks of at most `chunk` frames, whatever
 * block size the backend works in. */
#ifndef VSTACE_AUDIOOUT_H
#define VSTACE_AUDIOOUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { AO_AUTO = 0, AO_PIPEWIRE, AO_JACK, AO_ALSA };

typedef struct ao ao;

/* Fill `frames` frames of interleaved stereo, `frames` <= the chunk passed to
 * ao_open. Called on the backend's realtime thread: it must not block. */
typedef void (*ao_render_fn)(void *ud, float *out, int frames);

/* Open JACK or ALSA (AO_JACK, AO_ALSA) at `rate` Hz, rendering through `fn`.
 * `device` is the ALSA PCM name ("" for the default). Returns NULL with the
 * reason in err -- a JACK server that is not running, one running at another
 * rate than the plug-ins, a device that will not open. */
ao  *ao_open(int backend, const char *device, const char *client, int rate, int chunk,
             ao_render_fn fn, void *ud, char *err, size_t errn);
/* Stops the thread or callback and lets go of the device. Safe on NULL. */
void ao_close(ao *a);
/* Still running: a JACK server that went away, or an ALSA device that cannot
 * be recovered, makes this 0, and the caller should switch to something else. */
int  ao_alive(const ao *a);
const char *ao_describe(const ao *a);          /* "JACK, 256 frames at 48000 Hz" */
unsigned long ao_xruns(const ao *a);

/* Whether JACK can be used right now, and if not why ("libjack is not installed",
 * "no JACK server is running"). Opens and closes a client to find out. */
int  ao_jack_available(char *why, size_t n);

/* The ALSA playback devices worth offering: names for ao_open, labels to show. */
int  ao_alsa_devices(char names[][128], char labels[][96], int max);

/* The choice, kept between runs (~/.config/vst-ace/audio-backend) and, as
 * "backend" only, overridable by DW_BACKEND. */
typedef struct { int backend; char device[128]; } ao_choice;
void ao_choice_load(ao_choice *c);
int  ao_choice_save(const ao_choice *c);
const char *ao_backend_name(int backend);      /* "auto", "pipewire", "jack", "alsa" */
int  ao_backend_from_name(const char *name);   /* -1 when it names none of them */

#ifdef __cplusplus
}
#endif
#endif
