/* Load a macOS Mach-O bundle natively on Linux.
 *
 * The counterpart to pehost for Apple's format. It is a smaller job than the
 * Windows side for one reason: macOS x86_64 uses the System V AMD64 ABI, the
 * same one Linux uses, so there is no calling-convention layer at all -- a
 * function pointer out of a Mach-O image is directly callable. (macOS keeps
 * thread-local state in %gs where Linux uses %fs, so even the segment-register
 * trick the Win64 loader needs is already free here.)
 *
 * What is left: pick the right slice out of a universal binary, map the
 * segments, run the rebase/bind opcode streams, and satisfy the imports.
 */
#ifndef PELOAD_MACHOLOAD_H
#define PELOAD_MACHOLOAD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct macho macho;

/* Open a bundle directory (Foo.vst, Foo.component) or a bare Mach-O file.
 * Returns NULL on failure; macho_last_error() says why. */
macho      *macho_open(const char *path);
void        macho_close(macho *m);
const char *macho_last_error(void);

/* Run the image's initialisers (__mod_init_func). Separate from open so a
 * caller can inspect a broken image without executing any of it. */
int         macho_run_init(macho *m);

/* Look up an exported symbol. `name` is written as the source declares it --
 * "VSTPluginMain", not "_VSTPluginMain"; the leading underscore Mach-O adds is
 * handled here. */
void       *macho_symbol(macho *m, const char *name);

/* Describe an address as "image+0x... (__TEXT)" when it falls inside the mapped
 * image, or "(host)" when it does not. For reporting where a plugin is stuck or
 * has faulted -- the raw pointers alone say nothing. */
void        macho_describe(const macho *m, const void *addr, char *out, size_t n);

/* Bundle metadata from Contents/Info.plist, empty if absent. */
const char *macho_bundle_path(const macho *m);
/* The executable inside the bundle, and where it is mapped -- what the dyld
 * shims answer with. */
const char *macho_binary_path(const macho *m);
const void *macho_image_base(const macho *m);
size_t      macho_image_span(const macho *m);
const char *macho_bundle_id(const macho *m);
const char *macho_bundle_name(const macho *m);

/* Diagnostics. `reached` counts imports whose stub was actually called, which
 * is the number that says whether a missing symbol matters in practice. */
void        macho_import_stats(const macho *m, int *resolved, int *stubbed,
                               int *reached);
/* Walk the unresolved imports, newest first: cb(name, calls, ud). */
void        macho_each_stub(const macho *m,
                            void (*cb)(const char *name, unsigned long calls, void *ud),
                            void *ud);

/* Every exported symbol, for finding an entry point whose name we do not know
 * in advance -- an AU factory is called <Something>Factory, not a fixed name. */
void        macho_each_export(const macho *m,
                              void (*cb)(const char *name, void *addr, void *ud),
                              void *ud);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_MACHOLOAD_H */
