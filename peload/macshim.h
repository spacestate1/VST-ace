/* macOS framework shims for the Mach-O loader.
 *
 * The host's own libc and libstdc++ answer about half of a plugin's imports
 * directly, because Mach-O only differs by a leading underscore and Apple's
 * libc++ uses the same Itanium mangling. What is left is the framework surface,
 * and that is what lives here: one table per framework, consulted before the
 * host libraries so a name we implement deliberately always wins.
 */
#ifndef PELOAD_MACSHIM_H
#define PELOAD_MACSHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A shim is a name and either a function or a data address -- Mach-O binds both
 * through the same mechanism, and several of these (___stack_chk_guard, _pi,
 * ___stderrp) are data. */
typedef struct { const char *name; void *addr; } macshim_entry;

extern const macshim_entry macshim_vdsp[];
extern const macshim_entry macshim_corefoundation[];
extern const macshim_entry macshim_libsystem[];
extern const macshim_entry macshim_pthread[];
extern const macshim_entry macshim_audiounit[];
extern const macshim_entry macshim_foundation[];
extern const macshim_entry macshim_quartz[];
/* ImageIO: the PNG artwork a VSTGUI editor is made of. */
extern const macshim_entry macshim_imageio[];
extern const macshim_entry macshim_quartz2[];
extern const macshim_entry macshim_quartz3[];
extern const macshim_entry macshim_cf2[];
extern const macshim_entry macshim_mach[];
extern const macshim_entry macshim_files[];
extern const macshim_entry macshim_metal[];

/* CF objects built in macfound.c must share the header used in macshim.c, so
 * they are created through these rather than duplicated. */
void       *macshim_cf_string(const char *s);
void       *macshim_cf_dict_create_mutable_pub(long cap);
void        macshim_cf_dict_set_pub(void *d, const void *k, const void *v);
void       *macshim_cf_number_int(long v);
const void *macshim_lookup_retain(void *p);
int         macshim_cf_string_get(void *p, char *buf, long n);

/* CoreFoundation and Objective-C objects are interchangeable on macOS: a
 * CFStringRef can be sent -release. These let the runtime recognise one and do
 * the right thing rather than read the CF header as an isa. */
int         macshim_cf_is_object(const void *p);
const void *macshim_cf_retain_obj(const void *p);
void        macshim_cf_release_obj(const void *p);
void       *macshim_cf_data(const void *bytes, size_t n);
void       *macshim_cf_array(const void **vals, long n);

/* Any string object as UTF-8 -- one of ours, a CFString, or an @"..." literal
 * (which is a constant CFString and so matches no class). Defined in macns.c. */
const char *macns_utf8(void *str);

/* Nothing here runs a real run loop, so the host fires the editor's timers --
 * which is how an iPlug2 editor on macOS gets its frames. Defined in macns.c. */
void macns_fire_timers(void);

/* Turn a view's dirty flag into a drawRect: -- there is no run loop to do it. */
void macns_draw_dirty(void);

/* Drop per-plugin GUI state when a plugin closes. Without this the previous
 * plugin's timers keep firing into an editor that no longer exists. */
void macns_reset_gui(void);

/* A host-owned NSView to hand a plugin that needs a parent -- a macOS VST3's
 * IPlugView::attached wants one. Defined in macns.c. */
void *macns_make_view(int w, int h);

/* Point NSGraphicsContext's current context at a CGContextRef, which is what a
 * view fetches inside drawRect:. Defined in macns.c. */
void macns_set_graphics_port(void *cg);
/* The host-side AU calls a plugin makes back into whoever loaded it. Filled in
 * by macau.c once an instance exists; until then they report "not
 * initialised". */
void macshim_set_au_callbacks(
    int32_t (*getparam)(void *, uint32_t, uint32_t, uint32_t, float *),
    int32_t (*getprop)(void *, uint32_t, uint32_t, uint32_t, void *, uint32_t *),
    int32_t (*render)(void *, uint32_t *, const void *, uint32_t, uint32_t, void *));
void macmetal_reset(void);

/* The framebuffer of an editor that draws through Core Graphics instead of Metal
 * -- Ragnarok is the one here. Defined in macquartz.c. */
int  macquartz_editor_pixels(const unsigned int **px, int *w, int *h);
void macquartz_reset_editor(void);
/* A bitmap context the host owns, for an editor that draws through drawRect:
 * rather than making a surface of its own. Returns the CGContextRef. */
void *macquartz_editor_context(int w, int h);
/* Reset the editor context to what AppKit hands drawRect:, flip included. */
void  macquartz_begin_draw(void *ctx, int flipped, double height);

/* Fire whatever the plugin scheduled on the run loop. See macfound.c. */
void  macshim_fire_cf_timers(void);
/* CFRetain/CFRelease reaching a CoreGraphics object -- a CGImageSourceRef has
 * no typed release. 1 when the pointer was one of macquartz.c's. */
int   macquartz_cf_release(void *p);
int   macquartz_cf_retain(void *p);

/* Input for the editor's view, as synthesised NSEvents. Message numbers are the
 * Windows ones the host already speaks, so both editor backends take the same
 * call. Defined in macns.c. */
void macns_post_mouse(int x, int y, int msg, int buttons, int wheel);
void macns_post_key(int vk, int down, int ch);

/* Where the pointer is, in the host's own top-left coordinates, and how tall the
 * screen this host pretends to have is. Everything that reports a cursor
 * position has to agree on both -- CoreGraphics counts from the top and Cocoa
 * from the bottom, and a plugin converts between them. Defined in macns.c;
 * macquartz.c's CGDisplayPixelsHigh and CGDisplayMoveCursorToPoint go through
 * these rather than inventing their own. */
/* Wait up to `ms` for every block a plugin dispatched asynchronously to finish.
 * 1 when they all did. A block's code is in the plugin's image, so the image
 * must not be unmapped while one is pending -- see macho_close. Defined in
 * macfound.c. */
int  macshim_gcd_drain(int ms);

/* Which Mach-O image is loaded, for the dyld shims: a plugin asks dyld which
 * image its own code is in and builds its bundle path from the answer. Set by
 * machoload.c on open and cleared on close. Defined in macfound.c. */
void macshim_set_dyld_image(const char *binpath, const void *base, size_t span);
int  macshim_dyld_image_is(const void *base);
int  macshim_dyld_image_contains(const void *p, size_t n);

void macns_pointer(double *x, double *y);
void macns_set_pointer(double x, double y);
int  macns_screen_height(void);

/* The editor's framebuffer, owned by macmetal.c: CAMetalLayer's drawable is the
 * buffer the plugin renders into, so there is nothing to copy out of a GPU. */
int  macmetal_pixels(const unsigned int **px, int *w, int *h);
int  macmetal_active(void);
void macmetal_set_size(int w, int h);

/* Rasterizer cost since the last call: triangles, pixels shaded, milliseconds.
 * Reading it resets the counters. */
void macmetal_stats(unsigned long *tris, unsigned long *shaded, double *ms);

/* The bundle directory currently being loaded. Plugins resolve their own artwork
 * and presets relative to it. */
void        macshim_set_bundle(const char *path);
const char *macshim_bundle_path(void);

/* Which image is being loaded, so its atexit handlers can be attributed to it
 * and run before it is unmapped. */
void        macshim_set_image(const void *token);
void        macshim_run_atexit(const void *token);
const void *macquartz_provider_bytes(void *provider, size_t *len);

/* NULL when nothing implements `sym`. */
void *macshim_lookup(const char *sym);

/* Exposed so the FFT can be checked against a reference implementation instead
 * of being taken on trust. */
void *macshim_fftsetup(unsigned long log2n);
void  macshim_fft_zrip(void *setup, float *realp, float *imagp,
                       unsigned long log2n, int dir);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_MACSHIM_H */
