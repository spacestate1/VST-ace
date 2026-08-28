#include "pe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- little-endian scalar reads with bounds checking ---- */

static int rd16(const pe_image *img, size_t off, uint16_t *out)
{
    if (off + 2 > img->size) return -1;
    *out = (uint16_t)(img->data[off] | (img->data[off + 1] << 8));
    return 0;
}

static int rd32(const pe_image *img, size_t off, uint32_t *out)
{
    if (off + 4 > img->size) return -1;
    *out = (uint32_t)img->data[off]        | ((uint32_t)img->data[off + 1] << 8) |
           ((uint32_t)img->data[off + 2] << 16) | ((uint32_t)img->data[off + 3] << 24);
    return 0;
}

int pe_open(pe_image *img, const char *path)
{
    FILE    *f;
    long     len;
    uint32_t e_lfanew, sig, ddcount, opt_off, sh_off;
    uint16_t nsec, optsize, magic;
    int      i;

    memset(img, 0, sizeof *img);

    if (!(f = fopen(path, "rb"))) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return -1; }

    img->size = (size_t)len;
    if (!(img->data = malloc(img->size))) { fclose(f); return -1; }
    if (fread(img->data, 1, img->size, f) != img->size) {
        fclose(f); free(img->data); img->data = NULL; return -1;
    }
    fclose(f);

    if (img->size < 0x40 || img->data[0] != 'M' || img->data[1] != 'Z') {
        fprintf(stderr, "%s: not an MZ image\n", path); goto fail;
    }
    if (rd32(img, 0x3c, &e_lfanew)) goto fail;
    if (rd32(img, e_lfanew, &sig) || sig != 0x00004550u) {  /* "PE\0\0" */
        fprintf(stderr, "%s: no PE signature\n", path); goto fail;
    }

    if (rd16(img, e_lfanew + 6,  &nsec))    goto fail;
    if (rd16(img, e_lfanew + 20, &optsize)) goto fail;

    opt_off = e_lfanew + 24;
    if (rd16(img, opt_off, &magic)) goto fail;
    if (magic == 0x20b)      img->is64 = 1;   /* PE32+ */
    else if (magic == 0x10b) img->is64 = 0;   /* PE32   */
    else { fprintf(stderr, "%s: unknown optional header magic 0x%x\n", path, magic); goto fail; }

    /* Data directory sits right after the optional header's fixed part:
     * 112 bytes in for PE32+, 96 for PE32. Entry 2 is the resource table. */
    {
        uint32_t dd_off = opt_off + (img->is64 ? 112 : 96);
        if (rd32(img, opt_off + (img->is64 ? 108 : 92), &ddcount)) goto fail;
        if (ddcount < 3) { fprintf(stderr, "%s: no resource data directory\n", path); goto fail; }
        if (rd32(img, dd_off + 2 * 8,     &img->rsrc_rva))  goto fail;
        if (rd32(img, dd_off + 2 * 8 + 4, &img->rsrc_size)) goto fail;
    }

    sh_off    = opt_off + optsize;
    img->nsec = nsec;
    if (!(img->sec = calloc(nsec ? nsec : 1, sizeof *img->sec))) goto fail;
    for (i = 0; i < nsec; i++) {
        size_t s = sh_off + (size_t)i * 40;
        if (rd32(img, s +  8, &img->sec[i].vsize)   ||
            rd32(img, s + 12, &img->sec[i].vaddr)   ||
            rd32(img, s + 16, &img->sec[i].raw_size)||
            rd32(img, s + 20, &img->sec[i].raw_ptr)) goto fail;
    }
    return 0;

fail:
    pe_close(img);
    return -1;
}

void pe_close(pe_image *img)
{
    free(img->data);
    free(img->sec);
    memset(img, 0, sizeof *img);
}

long pe_rva_to_off(const pe_image *img, uint32_t rva)
{
    int i;
    for (i = 0; i < img->nsec; i++) {
        const pe_section *s = &img->sec[i];
        /* A section's mapped span is the larger of its virtual and raw size;
         * resource data always lives inside the raw part. */
        uint32_t span = s->vsize > s->raw_size ? s->vsize : s->raw_size;
        if (rva >= s->vaddr && rva < s->vaddr + span) {
            uint32_t delta = rva - s->vaddr;
            if (delta >= s->raw_size) return -1;   /* in BSS-like tail */
            return (long)(s->raw_ptr + delta);
        }
    }
    return -1;
}

/* ---- resource directory ---- */

static const char *rt_name(uint32_t id)
{
    switch (id) {
    case  1: return "CURSOR";        case  2: return "BITMAP";
    case  3: return "ICON";          case  4: return "MENU";
    case  5: return "DIALOG";        case  6: return "STRING";
    case  7: return "FONTDIR";       case  8: return "FONT";
    case  9: return "ACCELERATOR";   case 10: return "RCDATA";
    case 11: return "MESSAGETABLE";  case 12: return "GROUP_CURSOR";
    case 14: return "GROUP_ICON";    case 16: return "VERSION";
    case 17: return "DLGINCLUDE";    case 19: return "PLUGPLAY";
    case 20: return "VXD";           case 21: return "ANICURSOR";
    case 22: return "ANIICON";       case 23: return "HTML";
    case 24: return "MANIFEST";      default: return NULL;
    }
}

/* Resource name strings are UTF-16LE with a u16 length prefix and no NUL.
 * Everything in this binary is plain ASCII, so narrow with a replacement. */
static void rsrc_string(const pe_image *img, size_t off, char *out, size_t cap)
{
    uint16_t n = 0, i;
    size_t   w = 0;

    out[0] = '\0';
    if (rd16(img, off, &n)) return;
    for (i = 0; i < n && w + 1 < cap; i++) {
        uint16_t ch = 0;
        if (rd16(img, off + 2 + (size_t)i * 2, &ch)) break;
        out[w++] = (ch && ch < 0x80) ? (char)ch : '_';
    }
    out[w] = '\0';
}

typedef struct {
    const pe_image *img;
    size_t          base;     /* file offset of the resource directory root */
    pe_rsrc_cb      cb;
    void           *ud;
} walk_ctx;

static int walk_dir(walk_ctx *ctx, size_t dir_off, int level,
                    const char *type, int type_id, const char *name)
{
    const pe_image *img = ctx->img;
    uint16_t named = 0, ids = 0;
    int      i, total, rc;

    /* A resource tree is exactly three levels deep (type / name / language).
     * Refusing to go deeper keeps a corrupt or hostile image from driving this
     * into unbounded recursion via a self-referencing directory entry. */
    if (level > 2) return 0;

    if (rd16(img, dir_off + 12, &named) || rd16(img, dir_off + 14, &ids)) return -1;
    total = (int)named + (int)ids;

    for (i = 0; i < total; i++) {
        size_t   ent = dir_off + 16 + (size_t)i * 8;
        uint32_t id_or_name, child;
        char     label[256];

        if (rd32(img, ent, &id_or_name) || rd32(img, ent + 4, &child)) return -1;

        if (id_or_name & 0x80000000u) {
            rsrc_string(img, ctx->base + (id_or_name & 0x7fffffffu), label, sizeof label);
        } else {
            const char *rt = (level == 0) ? rt_name(id_or_name) : NULL;
            if (rt) snprintf(label, sizeof label, "%s", rt);
            else    snprintf(label, sizeof label, "%u", id_or_name);
        }

        if (child & 0x80000000u) {
            size_t sub = ctx->base + (child & 0x7fffffffu);
            int    tid = (level == 0 && !(id_or_name & 0x80000000u)) ? (int)id_or_name : type_id;
            if (level == 0)      rc = walk_dir(ctx, sub, 1, label, tid, name);
            else if (level == 1) rc = walk_dir(ctx, sub, 2, type,  tid, label);
            else                 rc = walk_dir(ctx, sub, 3, type,  tid, name);
            if (rc) return rc;
        } else {
            /* Leaf: IMAGE_RESOURCE_DATA_ENTRY { RVA, Size, CodePage, Reserved } */
            size_t   de = ctx->base + child;
            uint32_t data_rva, data_size;
            long     off;

            if (rd32(img, de, &data_rva) || rd32(img, de + 4, &data_size)) return -1;
            if ((off = pe_rva_to_off(img, data_rva)) < 0) continue;
            if ((size_t)off + data_size > img->size) continue;

            rc = ctx->cb(type, name, type_id, img->data + off, data_size, ctx->ud);
            if (rc) return rc;
        }
    }
    return 0;
}

int pe_walk_resources(const pe_image *img, pe_rsrc_cb cb, void *ud)
{
    walk_ctx ctx;
    long     base;

    if (!img->rsrc_rva) { fprintf(stderr, "image has no resource directory\n"); return -1; }
    if ((base = pe_rva_to_off(img, img->rsrc_rva)) < 0) {
        fprintf(stderr, "resource RVA 0x%x not mapped\n", img->rsrc_rva); return -1;
    }
    ctx.img  = img;
    ctx.base = (size_t)base;
    ctx.cb   = cb;
    ctx.ud   = ud;
    return walk_dir(&ctx, (size_t)base, 0, "", 0, "");
}
