/* Minimal PE32/PE32+ reader: just enough to walk the .rsrc directory.
 * Replaces the `7z x fb799964.dll .rsrc` step with something buildable. */
#ifndef FB_PE_H
#define FB_PE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t vaddr, vsize, raw_ptr, raw_size;
} pe_section;

typedef struct {
    unsigned char *data;
    size_t         size;
    int            is64;
    uint32_t       rsrc_rva, rsrc_size;
    pe_section    *sec;
    int            nsec;
} pe_image;

/* Reads the whole file into memory. Returns 0 on success, <0 on error. */
int  pe_open(pe_image *img, const char *path);
void pe_close(pe_image *img);

/* Returns a file offset, or -1 if the RVA is not inside any section. */
long pe_rva_to_off(const pe_image *img, uint32_t rva);

/* Called once per resource leaf. Return non-zero to abort the walk.
 * `type` and `name` are NUL-terminated ASCII; numeric ids are rendered as
 * their RT_* short name when known, otherwise as decimal digits. */
typedef int (*pe_rsrc_cb)(const char *type, const char *name, int type_id,
                          const unsigned char *data, uint32_t size, void *ud);

int pe_walk_resources(const pe_image *img, pe_rsrc_cb cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* FB_PE_H */
