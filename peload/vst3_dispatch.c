/* Route a .vst3 to whichever ABI build can drive it.
 *
 * vst3.c is compiled twice -- once with SysV vtables for native Linux bundles,
 * once with Microsoft x64 vtables for Windows images loaded by the PE loader.
 * Both copies are internally static, so their only external symbols are the
 * sv_/ms_ prefixed entry points below. This file picks between them by looking
 * at the binary itself rather than trusting the file name. */
#define _GNU_SOURCE
#include "vst3.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The two builds' entry points. Declared with void * because each build has its
 * own private definition of the host struct. */
#define DECL(pfx) \
    void *pfx##_v3_open(const char *, double, int, char *, int); \
    void  pfx##_v3_close(void *); \
    const char *pfx##_v3_name(const void *); \
    const char *pfx##_v3_vendor(const void *); \
    int   pfx##_v3_num_params(const void *); \
    int   pfx##_v3_num_inputs(const void *); \
    int   pfx##_v3_num_outputs(const void *); \
    int   pfx##_v3_is_synth(const void *); \
    int   pfx##_v3_num_programs(const void *); \
    void  pfx##_v3_program_name(void *, int, char *, int); \
    void  pfx##_v3_set_program(void *, int); \
    int   pfx##_v3_get_program(const void *); \
    void  pfx##_v3_param_name(void *, int, char *, int); \
    void  pfx##_v3_param_display(void *, int, char *, int); \
    float pfx##_v3_get_param(void *, int); \
    void  pfx##_v3_set_param(void *, int, float); \
    void  pfx##_v3_midi(void *, int, int, int); \
    void  pfx##_v3_set_input_mask(void *, unsigned); \
    void  pfx##_v3_render(void *, const float *, float *, int); \
    int   pfx##_v3_has_editor(void *); \
    void  pfx##_v3_editor_size(void *, int *, int *); \
    int   pfx##_v3_editor_can_resize(void *); \
    int   pfx##_v3_editor_attach(void *, unsigned long); \
    int   pfx##_v3_editor_is_hwnd(void *); \
    int   pfx##_v3_editor_attach_hwnd(void *, void *); \
    int   pfx##_v3_is_macho(void *); \
    int   pfx##_v3_editor_is_nsview(void *); \
    int   pfx##_v3_editor_attach_nsview(void *, void *); \
    void  pfx##_v3_editor_detach(void *); \
    void  pfx##_v3_editor_resized(void *, int, int); \
    void  pfx##_v3_set_runloop_hooks(const v3_runloop_hooks *); \
    void  pfx##_v3_runloop_fd(void *, int); \
    void  pfx##_v3_runloop_timer(void *)
DECL(sv);
DECL(ms);
#undef DECL

/* The wrapper the public API hands out: which build owns it, plus its host. */
struct v3host {
    int   ms;
    void *inner;
};

/* ---------------------------------------------------------------- sniffing */

/* Same bundle layout walk vst3.c uses, repeated here because the format has to
 * be known before either build is entered. */
static int resolve_binary(const char *path, char *out, size_t n)
{
    struct stat st;
    static const char *arch[] = { "x86_64-linux", "x86_64-win", "x86-win", NULL };
    int i;

    if (stat(path, &st)) return -1;
    if (S_ISREG(st.st_mode)) { snprintf(out, n, "%s", path); return 0; }

    /* macOS first, and the bundle rather than the binary. A .vst3 built for
     * macOS keeps its code in Contents/MacOS with no extension to recognise it
     * by, and the Mach-O loader wants the bundle anyway -- it reads Info.plist
     * and resolves Contents/MacOS itself. */
    {   char p[1024];
        snprintf(p, sizeof p, "%s/Contents/MacOS", path);
        if (!stat(p, &st) && S_ISDIR(st.st_mode)) { snprintf(out, n, "%s", path); return 0; }
    }

    for (i = 0; arch[i]; i++) {
        char dir[1024];
        DIR *d;
        struct dirent *e;
        snprintf(dir, sizeof dir, "%s/Contents/%s", path, arch[i]);
        if (!(d = opendir(dir))) continue;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 3 && (!strcmp(e->d_name + l - 3, ".so") ||
                          (l > 4 && !strcasecmp(e->d_name + l - 4, ".dll")) ||
                          (l > 5 && !strcmp(e->d_name + l - 5, ".vst3")))) {
                snprintf(out, n, "%s/%s", dir, e->d_name);
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }
    return -1;
}

/* 1 = PE (Windows), 0 = ELF or a macOS bundle, -1 = neither.
 *
 * macOS and Linux share an answer because they share an ABI: both are System V,
 * so both are driven by the sv_ build. What differs is the loader underneath,
 * and that is vst3.c's business rather than this file's -- it recognises the
 * bundle layout again on the way in. A directory reaching here is that bundle,
 * since resolve_binary returns a file for every other layout. */
static int binary_is_pe(const char *bin)
{
    unsigned char magic[4] = { 0, 0, 0, 0 };
    struct stat st;
    FILE *f;
    size_t got;

    if (!stat(bin, &st) && S_ISDIR(st.st_mode)) return 0;
    if (!(f = fopen(bin, "rb"))) return -1;
    got = fread(magic, 1, 4, f);
    fclose(f);
    if (got < 4) return -1;
    if (magic[0] == 'M' && magic[1] == 'Z') return 1;
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') return 0;
    /* MH_MAGIC_64, or either width of universal binary. */
    if (magic[0] == 0xcf && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) return 0;
    if (magic[0] == 0xca && magic[1] == 0xfe && magic[2] == 0xba &&
        (magic[3] == 0xbe || magic[3] == 0xbf)) return 0;
    return -1;
}

/* --------------------------------------------------------------- public API */

v3host *v3_open(const char *path, double samplerate, int blocksize,
                char *err, int errlen)
{
    char bin[1024];
    int pe;
    v3host *w;

    if (resolve_binary(path, bin, sizeof bin)) {
        snprintf(err, errlen, "no VST3 binary inside %s", path);
        return NULL;
    }
    if ((pe = binary_is_pe(bin)) < 0) {
        snprintf(err, errlen, "%s is neither a PE nor an ELF image", bin);
        return NULL;
    }
    if (!(w = calloc(1, sizeof *w))) { snprintf(err, errlen, "oom"); return NULL; }
    w->ms = pe;

    /* Hand the resolved binary over, so neither build re-walks the bundle. */
    w->inner = pe ? ms_v3_open(bin, samplerate, blocksize, err, errlen)
                  : sv_v3_open(bin, samplerate, blocksize, err, errlen);
    if (!w->inner) { free(w); return NULL; }
    return w;
}

#define FWD_VOID(name, ...) \
    do { if (!h || !h->inner) return; \
         if (h->ms) ms_##name(h->inner, __VA_ARGS__); \
         else       sv_##name(h->inner, __VA_ARGS__); } while (0)

#define FWD_RET(type, name, dflt, ...) \
    do { if (!h || !h->inner) return dflt; \
         return h->ms ? ms_##name(h->inner, __VA_ARGS__) \
                      : sv_##name(h->inner, __VA_ARGS__); } while (0)

#define FWD_RET0(type, name, dflt) \
    do { if (!h || !h->inner) return dflt; \
         return h->ms ? ms_##name(h->inner) : sv_##name(h->inner); } while (0)

void v3_close(v3host *h)
{
    if (!h) return;
    if (h->inner) { if (h->ms) ms_v3_close(h->inner); else sv_v3_close(h->inner); }
    free(h);
}

const char *v3_name(const v3host *h)   { FWD_RET0(const char *, v3_name, ""); }
const char *v3_vendor(const v3host *h) { FWD_RET0(const char *, v3_vendor, ""); }
int v3_num_params(const v3host *h)   { FWD_RET0(int, v3_num_params, 0); }
int v3_num_inputs(const v3host *h)   { FWD_RET0(int, v3_num_inputs, 0); }
int v3_num_outputs(const v3host *h)  { FWD_RET0(int, v3_num_outputs, 0); }
int v3_is_synth(const v3host *h)     { FWD_RET0(int, v3_is_synth, 0); }
int v3_num_programs(const v3host *h) { FWD_RET0(int, v3_num_programs, 0); }
int v3_get_program(const v3host *h)  { FWD_RET0(int, v3_get_program, 0); }

void v3_program_name(v3host *h, int i, char *b, int n)
{ if (n) b[0] = 0; FWD_VOID(v3_program_name, i, b, n); }
void v3_set_program(v3host *h, int i) { FWD_VOID(v3_set_program, i); }
void v3_param_name(v3host *h, int i, char *b, int n)
{ if (n) b[0] = 0; FWD_VOID(v3_param_name, i, b, n); }
void v3_param_display(v3host *h, int i, char *b, int n)
{ if (n) b[0] = 0; FWD_VOID(v3_param_display, i, b, n); }
float v3_get_param(v3host *h, int i) { FWD_RET(float, v3_get_param, 0.0f, i); }
void v3_set_param(v3host *h, int i, float v) { FWD_VOID(v3_set_param, i, v); }
void v3_midi(v3host *h, int st, int d1, int d2) { FWD_VOID(v3_midi, st, d1, d2); }
void v3_set_input_mask(v3host *h, unsigned m) { FWD_VOID(v3_set_input_mask, m); }

/* Run-loop plumbing is process-wide rather than per-plugin, so both builds get
 * the same hooks and either may call back. */
void v3_set_runloop_hooks(const v3_runloop_hooks *hk)
{ sv_v3_set_runloop_hooks(hk); ms_v3_set_runloop_hooks(hk); }

/* The handler belongs to whichever build registered it; only one of the two can
 * own a given pointer, and the non-owner's vtable call would be wrong. In
 * practice editors only work on the ELF path, so route there. */
void v3_runloop_fd(void *handler, int fd)    { sv_v3_runloop_fd(handler, fd); }
void v3_runloop_timer(void *handler)         { sv_v3_runloop_timer(handler); }

int v3_has_editor(v3host *h)
{ if (!h || !h->inner) return 0;
  return h->ms ? ms_v3_has_editor(h->inner) : sv_v3_has_editor(h->inner); }

void v3_editor_size(v3host *h, int *w, int *hh)
{ if (w) *w = 0; if (hh) *hh = 0; if (!h || !h->inner) return;
  if (h->ms) ms_v3_editor_size(h->inner, w, hh); else sv_v3_editor_size(h->inner, w, hh); }

int v3_editor_can_resize(v3host *h)
{ if (!h || !h->inner) return 0;
  return h->ms ? ms_v3_editor_can_resize(h->inner) : sv_v3_editor_can_resize(h->inner); }

int v3_editor_attach(v3host *h, unsigned long xid)
{ if (!h || !h->inner) return -1;
  return h->ms ? ms_v3_editor_attach(h->inner, xid) : sv_v3_editor_attach(h->inner, xid); }

int v3_editor_is_hwnd(v3host *h)
{ if (!h || !h->inner) return 0;
  return h->ms ? ms_v3_editor_is_hwnd(h->inner) : sv_v3_editor_is_hwnd(h->inner); }

int v3_editor_attach_hwnd(v3host *h, void *hwnd)
{ if (!h || !h->inner) return -1;
  return h->ms ? ms_v3_editor_attach_hwnd(h->inner, hwnd)
               : sv_v3_editor_attach_hwnd(h->inner, hwnd); }

/* Only the SysV build ever loads a Mach-O, so these three need no ms_ arm --
 * but they still go through the wrapper, because the caller holds a v3host and
 * has no business knowing which build owns it. */
int v3_is_macho(v3host *h)
{ return (h && h->inner && !h->ms) ? sv_v3_is_macho(h->inner) : 0; }

int v3_editor_is_nsview(v3host *h)
{ return (h && h->inner && !h->ms) ? sv_v3_editor_is_nsview(h->inner) : 0; }

int v3_editor_attach_nsview(v3host *h, void *nsview)
{ return (h && h->inner && !h->ms) ? sv_v3_editor_attach_nsview(h->inner, nsview) : -1; }

void v3_editor_detach(v3host *h)
{ if (!h || !h->inner) return;
  if (h->ms) ms_v3_editor_detach(h->inner); else sv_v3_editor_detach(h->inner); }

void v3_editor_resized(v3host *h, int w, int hp)
{ if (!h || !h->inner) return;
  if (h->ms) ms_v3_editor_resized(h->inner, w, hp); else sv_v3_editor_resized(h->inner, w, hp); }

void v3_render(v3host *h, const float *in, float *out, int frames)
{
    if (!h || !h->inner) { memset(out, 0, (size_t)frames * 2 * sizeof *out); return; }
    if (h->ms) ms_v3_render(h->inner, in, out, frames);
    else       sv_v3_render(h->inner, in, out, frames);
}
