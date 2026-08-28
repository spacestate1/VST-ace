/* Sample-playback drum engine.
 *
 * A kit is a directory of WAV files; each file is mapped to a consecutive MIDI
 * note from DK_BASE_NOTE upward, so a keyboard or the on-screen piano plays
 * them directly. Samples are resampled on the fly to the engine rate, which
 * matters because sample libraries are mostly 44.1 kHz while the engines here
 * run at 48 kHz.
 *
 * Unlike the other three engines nothing here is reverse engineered: it plays
 * whatever WAVs you point it at. */
#ifndef DRUMKIT_H
#define DRUMKIT_H

#include <stddef.h>

#define DK_MAX_SAMPLES  64
#define DK_MAX_VOICES   32
#define DK_BASE_NOTE    36     /* C2 */
#define DK_NAME_MAX     64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drumkit drumkit;

/* Loads every .wav in `dir`, sorted by name, up to DK_MAX_SAMPLES.
 * Returns NULL if the directory has none. */
drumkit *drumkit_load(const char *dir, double samplerate);
void     drumkit_free(drumkit *k);

int         drumkit_count(const drumkit *k);
const char *drumkit_sample_name(const drumkit *k, int i);
int         drumkit_note_of(const drumkit *k, int i);   /* MIDI note for slot i */

void drumkit_note_on(drumkit *k, int note, int velocity);
void drumkit_note_off(drumkit *k, int note);            /* one-shots ignore this */
void drumkit_all_off(drumkit *k);
void drumkit_render(drumkit *k, double *out, int frames);   /* interleaved stereo */

void drumkit_set_gain(drumkit *k, double g);

#ifdef __cplusplus
}
#endif

#endif /* DRUMKIT_H */
