/* rom -- getting FB-7999's data out of wherever it happens to be.
 *
 * The wavetable and the three preset banks are PE resources inside the plugin
 * binary. They can equally be plain files on disk, left there by an earlier
 * `fbextract resources` run. Every tool here wants one or the other, and each
 * had grown its own copy of the same forty lines of "read the file, and if it
 * turns out to be a DLL go and walk its resource directory instead".
 *
 * This is that copy, once. */
#ifndef FB_ROM_H
#define FB_ROM_H

#include <stddef.h>

#include "bank.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read a whole file. Reports the failure itself; the caller frees the result. */
unsigned char *rom_slurp(const char *path, size_t *size);

/* One named resource out of a PE binary -- `type` and `name` as fbextract
 * prints them, e.g. ("DSTDATA", "WAVEDST") or ("BANK_A", "PROGINIT").
 * NULL if `path` is not a PE, or carries no such resource. Quiet either way:
 * asking is how a caller finds out. */
unsigned char *rom_resource(const char *path, const char *type,
                            const char *name, size_t *size);

/* The wavetable, from either shape of input: a raw WAVEDST file, or a plugin
 * binary to pull it out of. This is what every -w option accepts. */
unsigned char *rom_wavedst(const char *path, size_t *size);

/* Parse a PROGINIT blob and confirm it is FB-7999's.
 *
 * The engine is a DW-8000, and only FB-7999's 69-parameter layout means
 * anything to it. Other Full Bucket banks parse perfectly well but carry a
 * different body, so they have to be turned away here rather than read as
 * nonsense parameter values. `whence` names the source in the error message.
 * Returns 0 on success, and leaves nothing allocated otherwise. */
int rom_bank_parse(bank *b, const unsigned char *blob, size_t n,
                   const char *whence);

#ifdef __cplusplus
}
#endif

#endif /* FB_ROM_H */
