/* Decode a QuickDraw picture. See pict.c for which opcodes are handled. */
#ifndef PELOAD_PICT_H
#define PELOAD_PICT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode `data` into a freshly allocated buffer of 32-bit 0xAARRGGBB pixels,
 * which the caller frees. Returns 1 on success; on failure returns 0 and puts a
 * reason in `err`. `*w`/`*h` are the picture's frame, which is what the caller
 * should scale from -- a picture may draw into only part of it. */
int pict_decode(const uint8_t *data, uint32_t len, uint32_t **pixels,
                int *w, int *h, char *err, int errlen);

/* The frame alone, without decoding the image. */
int pict_size(const uint8_t *data, uint32_t len, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_PICT_H */
