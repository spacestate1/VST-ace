/* macOS VST2 host, for pehost to dispatch to. Runs in-process: macOS x86-64 is
 * System V, so no ABI layer and no helper process. */
#ifndef PELOAD_MACVSTHOST_H
#define PELOAD_MACVSTHOST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct macvst macvst;

/* A macOS VST2 (a Mach-O bundle). */
macvst     *macvst_open(const char *path, double samplerate, int blocksize);

/* A native Linux VST2 (an ELF shared object). Same driver: VST2 is System V on
 * both, so only the way the entry point is found differs. */
macvst     *macvst_open_native(const char *path, double samplerate, int blocksize);
void        macvst_close(macvst *h);
const char *macvst_last_error(void);

const char *macvst_name(const macvst *h);
const char *macvst_vendor(const macvst *h);
int macvst_num_programs(const macvst *h);
int macvst_num_params(const macvst *h);
int macvst_num_inputs(const macvst *h);
int macvst_num_outputs(const macvst *h);
int macvst_is_synth(const macvst *h);
int macvst_unique_id(const macvst *h);

void  macvst_param_name(macvst *h, int i, char *buf, int n);
void  macvst_param_label(macvst *h, int i, char *buf, int n);
void  macvst_param_display(macvst *h, int i, char *buf, int n);
void  macvst_program_name(macvst *h, int i, char *buf, int n);
void  macvst_set_program(macvst *h, int i);
int   macvst_get_program(macvst *h);
float macvst_get_param(macvst *h, int i);
void  macvst_set_param(macvst *h, int i, float v);

/* Safe from the GUI thread while the audio thread renders. */
void  macvst_midi(macvst *h, int status, int d1, int d2);

/* Audio thread only. `src` may be NULL for an instrument. */
void  macvst_render_io(macvst *h, const float *src, float *interleaved, int frames);

/* Editor. Same contract as the Windows side: 0 means no editor, 2 means it
 * renders into a pixel buffer. GUI thread only. */
int   macvst_editor_kind(macvst *h);

/* Whether this was loaded as a native ELF rather than a Mach-O bundle. The
 * editors differ completely -- see macvst_editor_attach. */
int   macvst_is_native(macvst *h);

/* Open a native Linux editor as a child of an X11 window. Use this instead of
 * macvst_editor_open when macvst_is_native() is true; the pixel path has nothing
 * to read for such a plugin, and opening with no parent leaves it with a
 * top-level window of its own. */
int   macvst_editor_attach(macvst *h, unsigned long xid);
void  macvst_editor_size(macvst *h, int *w, int *hh);
int   macvst_editor_open(macvst *h);
void  macvst_editor_close(macvst *h);
void  macvst_editor_pump(macvst *h);
int   macvst_editor_pixels(macvst *h, const unsigned int **px, int *w, int *hh);
void  macvst_editor_mouse(macvst *h, int x, int y, int msg, int buttons, int wheel);
void  macvst_editor_key(macvst *h, int vk, int down, int ch);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_MACVSTHOST_H */
