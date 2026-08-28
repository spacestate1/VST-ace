/* Host an Audio Unit loaded by machoload.
 *
 * An AUv2 exposes no render entry point: the bundle exports a factory that
 * returns an AudioComponentPlugInInterface, and every method is fetched from
 * that interface's Lookup by numeric selector. This wraps that protocol.
 */
#ifndef PELOAD_MACAU_H
#define PELOAD_MACAU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct macau macau;

/* Load the bundle, run its initialisers, call the factory and fetch the
 * methods. Does not configure or initialise the unit -- see macau_configure. */
macau      *macau_open(const char *bundle, double samplerate, int blocksize);
void        macau_close(macau *a);
const char *macau_last_error(void);

/* Set the stream format on both scopes, install the input pull callback, and
 * call Initialize. Must succeed before macau_render. */
int  macau_configure(macau *a);

/* True when the unit accepted an input format, i.e. it processes audio rather
 * than generating it. */
int  macau_is_effect(const macau *a);

/* Format an address as an image offset, for reporting where a plugin is stuck. */
void macau_describe(const macau *a, const void *addr, char *out, size_t n);

/* Parameter count, so a caller can size its own array. */
int   macau_param_count(macau *a);
int   macau_num_params(macau *a, unsigned *ids, int max);
int   macau_param_info(macau *a, uint32_t id, char *name, int namelen,
                       float *minv, float *maxv, float *defv);
float macau_get_param(macau *a, uint32_t id);
void  macau_set_param(macau *a, uint32_t id, float v);

/* One block. `in_l`/`in_r` may be NULL for an instrument; `time` carries the
 * running sample position the unit is told about and is advanced on success. */
/* MIDI into an instrument unit. Returns non-zero if the unit took it. */
int  macau_midi(macau *a, int status, int d1, int d2);
int  macau_has_midi(const macau *a);

int  macau_render(macau *a, const float *in_l, const float *in_r,
                  float *out_l, float *out_r, int frames, double *time);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_MACAU_H */
