/* FB-7999 preset banks (the BANK_A / BANK_B / PROG6000 PROGINIT resources).
 *
 * Each is a VST2 FXB with an opaque plugin chunk:
 *
 *   'CcnK'  u32be byteSize   'FBCh'  u32be version(2)
 *   'fb79'  u32be fxVersion  u32be numPrograms(64)   byte future[128]
 *   u32be chunkSize   byte chunk[chunkSize]
 *
 * and the chunk itself is iPlug2's little-endian serialisation:
 *
 *   'tffp'  u32le 0x00010000
 *   64 x { u32le nameLen; char name[nameLen];
 *          u8 version(1); double param[69]; u32le five(5); u32le revision }
 *
 * The body is 561 bytes = 1 + 69*8 + 8. Note the single leading byte: the
 * doubles are misaligned by one from the start of the record. Reading them
 * from offset 9 with no trailer also consumes the chunk exactly -- the sizes
 * happen to agree -- but shifts every value into the neighbouring parameter's
 * slot. The 69 count is corroborated by the registration loop in
 * FUN_18052f240, which bounds at 0x45 = 69.
 *
 * `revision` is constant within a bank but differs between them (502 for
 * BANK_A, 259 for BANK_B, 1001 for PROG6000); it coincides with the FXB
 * header's fxVersion only for BANK_A, so it is a bank revision of its own.
 *
 * Program values are stored as doubles but hold the DW-8000's own integer
 * ranges: 1..16 for the two waveform selectors, 0..31 for levels and envelope
 * stages, 0..63 for cutoff, 0..7 for velocity. Volume is the only parameter
 * the factory banks ever store as a fraction. */
#ifndef FB_BANK_H
#define FB_BANK_H

#include <stddef.h>
#include <stdint.h>

#define BANK_NAME_MAX    64
#define BANK_NPARAM      69    /* FB-7999 specifically */
#define BANK_BODY_BYTES  561   /* FB-7999: 1 + 69*8 + 8 */
#define BANK_MAXPARAM   256    /* upper bound for auto-detected layouts */
#define BANK_MAXBODY   4096

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     name[BANK_NAME_MAX];
    uint8_t  version;              /* always 1 */
    double   param[BANK_MAXPARAM]; /* filled only when the body is doubles */
    uint32_t five;                 /* FB-7999: always 5 */
    uint32_t revision;             /* FB-7999: per-bank */
} bank_program;

/* Parameter names, recovered from the InitParam call sites in FUN_18052f240.
 * Indices 60..68 are registered by a loop as "reserved".
 * Returns NULL for out-of-range indices. */
const char *bank_param_name(int index);

typedef struct {
    uint32_t      fx_id;        /* 'fb79', 'fb02', 'kp30', ... */
    uint32_t      fx_version;   /* 502 for BANK_A, 403 for BANK_B, 10001 for PROG6000 */
    uint32_t      num_programs; /* as declared in the FXB header */
    uint32_t      chunk_size;
    int           count;        /* programs actually parsed */
    int           body_bytes;   /* detected per-program body size */
    int           nparam;       /* doubles in the body, or 0 if it is not doubles */
    bank_program *prog;
} bank;

/* The FXB container and the 'tffp' chunk are identical across Full Bucket's
 * whole catalogue; only the four-character id, the program count and the
 * per-program body differ. The body size is detected from the record stride.
 *
 * When the body has FB-7999's shape (u8 version, N doubles, two u32s) the
 * parameters are decoded and `nparam` is set. Other plugins pack their body
 * differently -- FB-02 stores a version string and then the FM voice as raw
 * bytes, mirroring the FB-01's own voice dump -- and those are left as
 * `nparam = 0`, names still readable. */
int  bank_parse(bank *b, const unsigned char *data, size_t size);
void bank_free(bank *b);

/* A program by index, or by any case-insensitive part of its name -- which is
 * what lets `dw play "slap bass"` work. Returns -1 if nothing matches. */
int  bank_find(const bank *b, const char *sel);

/* Optional index->name table, one parameter name per line. Returns the number
 * of names read, or -1. Pass NULL to `names` for bare "p00" labels. */
typedef struct { char **name; int count; } bank_names;

int  bank_names_load(bank_names *n, const char *path);
void bank_names_free(bank_names *n);

void bank_print(const bank *b, const bank_names *names, int verbose);
int  bank_write_csv(const bank *b, const bank_names *names, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* FB_BANK_H */
