/* The PE loader on its own, so a Windows VST3 can reuse it.
 *
 * pehost.c already maps PE32+ images, applies relocations, resolves imports
 * against the Win32 stub layer, sets up TLS and runs DllMain -- all of which a
 * Windows .vst3 needs just as much as a VST2 .dll does. Only the API on top
 * differs. This exposes that machinery to vst3.c.
 *
 * One module at a time: the stub layer keeps a single image base for resource
 * lookups, so loading two PE plugins at once would have them share it. */
#ifndef PELOAD_PEIMAGE_H
#define PELOAD_PEIMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void *base; unsigned long size; } pe_module;

/* Map and initialise a PE32+ DLL: relocate, bind imports, install TLS, call
 * DllMain(DLL_PROCESS_ATTACH). Returns 0 on success, and fills `err` otherwise.
 * The caller must have run pehost_thread_init() on this thread first. */
int   pe_module_load(const char *path, pe_module *m, char *err, int errlen);

/* Unmap the image. Only safe once nothing is executing inside it. */
void  pe_module_unload(pe_module *m);

/* Look up an exported symbol by name; NULL if absent. */
void *pe_module_export(const pe_module *m, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_PEIMAGE_H */
