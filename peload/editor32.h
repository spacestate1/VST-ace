/* Editor support for the i386 loader.
 *
 * The Win32 window layer and the DirectWrite shim are width-agnostic C -- the
 * only thing that had to change for i386 was the JIT'd probe stub in
 * dwrite_shim.h. So this is the same sequence pehost.c uses on x86-64: create a
 * container window, hand it to effEditOpen, let the plugin build its interface
 * inside it, then pump.
 *
 * Two ways out: --editor-png renders a frame and writes it to a file, which is
 * testable with no display at all; --editor opens an X11 window and runs it
 * live.
 */
#ifndef PELOAD_EDITOR32_H
#define PELOAD_EDITOR32_H

#include "win32host.h"
#include "png_out.h"

/* VST2 flags bit 0: the plugin has an editor. */
#define EFF_HAS_EDITOR 1

/* ERect is four int16_t in top, left, bottom, right order -- not four ints, and
 * not left-first. */
typedef struct { int16_t top, left, bottom, right; } ed_rect;

static int ed_size(AEffect32 *fx, int *w, int *h)
{
    ed_rect *r = NULL;
    intptr_t rc;

    *w = *h = 0;
    if (!(fx->flags & EFF_HAS_EDITOR)) {
        fprintf(stderr, "editor: plugin does not advertise one (flags 0x%x)\n",
                (unsigned)fx->flags);
        return -1;
    }
    rc = fx->dispatcher(fx, effEditGetRect, 0, 0, &r, 0.0f);
    if (!r) {
        fprintf(stderr, "editor: effEditGetRect returned %ld and wrote no rect\n",
                (long)rc);
        return -1;
    }
    *w = r->right - r->left;
    *h = r->bottom - r->top;
    if (*w <= 0 || *h <= 0) {
        fprintf(stderr, "editor: rect is empty (t %d l %d b %d r %d)\n",
                r->top, r->left, r->bottom, r->right);
        return -1;
    }
    return 0;
}

/* Open the editor. Mirrors pehost_editor_open, including the two guards that
 * were learned the hard way: a plugin reporting no size, and a plugin whose
 * font never reached the text backend (painting that dereferences an empty
 * cache). */
static int ed_open(AEffect32 *fx, int *w, int *h)
{
    void *container;

    if (ed_size(fx, w, h)) {
        fprintf(stderr, "editor: no usable editor size reported -- not opening\n");
        return -1;
    }
    if (!(container = w32_create_host_window(*w, *h))) return -1;

    if (!fx->dispatcher(fx, effEditOpen, 0, 0, container, 0.0f)) {
        /* Some plugins return 0 yet still create the window, so trust the
         * window rather than the return code. */
        if (!w32_root_window()) { fprintf(stderr, "editor: effEditOpen failed\n"); return -1; }
    }
    /* See pehost.c: this opcode needs a real out-pointer, not NULL. */
    { ed_rect *rr = NULL; fx->dispatcher(fx, effEditGetRect, 0, 0, &rr, 0.0f); }
    fx->dispatcher(fx, effEditTop, 0, 0, NULL, 0.0f);

    if (!w32_font_pipeline_ok()) {
        fprintf(stderr, "editor: a registered font did not load -- refusing the "
                        "editor rather than crashing\n");
        fx->dispatcher(fx, effEditClose, 0, 0, NULL, 0.0f);
        w32_reset();
        return -1;
    }
    w32_show_editor();
    return 0;
}

static void ed_pump(AEffect32 *fx)
{
    w32_pump();
    /* VST2 editors animate off effEditIdle rather than a timer of their own. */
    fx->dispatcher(fx, effEditIdle, 0, 0, NULL, 0.0f);
}

/* ------------------------------------------------------------------- PNG out */

/* A minimal PNG writer: zlib stored blocks, so no compression library. The
 * point is a file that any viewer opens, not a small one. */
/* The PNG writer lives in png_out.h so the macOS editor path shares it. */
#define ed_write_png png_write_bgrx

/* Open the editor, let it settle, and write one frame out. `frames` pumps
 * matter: plugins draw over several idle passes rather than all at once. */
static int ed_render_png(AEffect32 *fx, const char *path, int frames)
{
    const uint32_t *px;
    int w, h, i, nonblack = 0, npix;

    if (ed_open(fx, &w, &h)) return 1;
    printf("editor: %dx%d\n", w, h);
    for (i = 0; i < (frames > 0 ? frames : 30); i++) {
        ed_pump(fx);
        { struct timespec ts = { 0, 16000000 }; nanosleep(&ts, NULL); }
    }
    if (!w32_editor_pixels(&px, &w, &h) || !px || w <= 0 || h <= 0) {
        fprintf(stderr, "editor: no pixels came back\n");
        return 1;
    }
    npix = w * h;
    for (i = 0; i < npix; i++) if ((px[i] & 0xFFFFFF) != 0) nonblack++;
    printf("editor: %d of %d pixels painted (%.1f%%)\n",
           nonblack, npix, 100.0 * nonblack / npix);
    if (ed_write_png(path, px, w, h)) return 1;
    printf("wrote %s\n", path);
    fx->dispatcher(fx, effEditClose, 0, 0, NULL, 0.0f);
    w32_reset();
    return nonblack > 0 ? 0 : 1;
}

#endif /* PELOAD_EDITOR32_H */
