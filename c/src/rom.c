/* See rom.h. */

#include "rom.h"

#include "dw_synth.h"
#include "pe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char *rom_slurp(const char *path, size_t *size)
{
    FILE          *f = fopen(path, "rb");
    unsigned char *b;
    long           n;

    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || !(b = malloc((size_t)n ? (size_t)n : 1))) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        fclose(f); free(b); return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return b;
}

/* Two bytes decide it, so read two bytes: pe_open would pull the whole 8 MB
 * DLL into memory only to report that it is not one. */
static int looks_like_pe(const char *path)
{
    char   mz[2];
    size_t got;
    FILE  *f = fopen(path, "rb");

    if (!f) return 0;
    got = fread(mz, 1, sizeof mz, f);
    fclose(f);
    return got == sizeof mz && mz[0] == 'M' && mz[1] == 'Z';
}

/* What pe_walk_resources is looking for, and what it found. */
typedef struct {
    const char    *type, *name;
    unsigned char *data;
    size_t         size;
} hunt;

static int match(const char *type, const char *name, int type_id,
                 const unsigned char *data, uint32_t size, void *ud)
{
    hunt *h = ud;

    (void)type_id;
    if (strcmp(type, h->type) || strcmp(name, h->name)) return 0;
    if ((h->data = malloc(size ? size : 1))) {
        memcpy(h->data, data, size);
        h->size = size;
    }
    return 1;                            /* found it; stop the walk */
}

unsigned char *rom_resource(const char *path, const char *type,
                            const char *name, size_t *size)
{
    pe_image img;
    hunt     h;

    if (!looks_like_pe(path) || pe_open(&img, path)) return NULL;
    h.type = type; h.name = name; h.data = NULL; h.size = 0;
    pe_walk_resources(&img, match, &h);
    pe_close(&img);

    if (!h.data) return NULL;
    *size = h.size;
    return h.data;
}

unsigned char *rom_wavedst(const char *path, size_t *size)
{
    unsigned char *w;

    if (!looks_like_pe(path)) return rom_slurp(path, size);   /* a raw WAVEDST */

    if ((w = rom_resource(path, "DSTDATA", "WAVEDST", size))) return w;
    fprintf(stderr, "%s: no DSTDATA/WAVEDST resource\n", path);
    return NULL;
}

int rom_bank_parse(bank *b, const unsigned char *blob, size_t n, const char *whence)
{
    char id[5];

    if (bank_parse(b, blob, n)) return -1;
    if (b->nparam == DWP_COUNT) return 0;

    id[0] = (char)(b->fx_id >> 24); id[1] = (char)(b->fx_id >> 16);
    id[2] = (char)(b->fx_id >> 8);  id[3] = (char)b->fx_id;
    id[4] = 0;
    fprintf(stderr, "%s: %d-parameter bank (id '%s'); this engine needs "
                    "FB-7999's %d\n", whence, b->nparam, id, DWP_COUNT);
    bank_free(b);
    return -1;
}
