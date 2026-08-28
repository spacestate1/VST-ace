/* 16-bit mono PCM RIFF writer. */
#ifndef FB_WAV_H
#define FB_WAV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Peak-normalises `data` to 0.98 FS, truncates toward zero to int16 (matching
 * numpy's .astype(int16)) and writes a mono WAV. Returns 0 on success. */
int wav_write_mono16(const char *path, const double *data, size_t n, int samplerate);

/* Writes interleaved stereo without normalising -- levels stay comparable
 * between renders, so a quiet preset sounds quiet. Samples are clamped. */
int wav_write_stereo16(const char *path, const double *interleaved,
                       size_t frames, int samplerate);

/* Reads a RIFF/WAVE file into a mono float buffer, normalised to -1..1.
 * Handles 8/16/24/32-bit PCM and 32-bit float; multi-channel is summed to
 * mono. Chunks are walked properly rather than assuming data starts at 44 --
 * plenty of sample libraries carry LIST/fact chunks first.
 * Caller frees *out. Returns 0 on success. */
int wav_read_mono(const char *path, float **out, size_t *frames, int *samplerate);

#ifdef __cplusplus
}
#endif

#endif /* FB_WAV_H */
