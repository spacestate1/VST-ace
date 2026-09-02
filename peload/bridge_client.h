/* Host side of the 32-bit bridge. Mirrors the subset of pehost.h that a
 * out-of-process plugin can support, so pehost.c can dispatch to either.
 *
 * Threading matches pehost's contract: bridge_render_io from the audio thread,
 * everything else from the GUI thread. bridge_set_param and bridge_midi are the
 * two exceptions -- they are lock-free writes into shared memory, so the GUI
 * thread may call them while audio runs.
 */
#ifndef PELOAD_BRIDGE_CLIENT_H
#define PELOAD_BRIDGE_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bridge bridge;

/* Is a peload32 helper present? False means 32-bit plugins cannot be hosted. */
int         bridge_available(void);

/* Why the 32-bit helper cannot be used, or NULL when it can. Worth showing a
 * user verbatim: when the helper is present but its i386 runtime is not, this
 * is the dynamic linker naming the library that is missing. */
const char *bridge_unavailable_reason(void);
const char *bridge_last_error(void);

bridge *bridge_open(const char *dll_path, double samplerate, int blocksize);

/* Same, naming the helper: "peload32" for a 32-bit plugin, "peserve" to isolate a
 * plugin this process could have loaded itself. */
bridge *bridge_open_helper(const char *dll_path, double samplerate, int blocksize,
                           const char *helper_name);

/* Is a peserve helper present, so any plugin can be isolated on request? */
int     bridge_isolation_available(void);
void    bridge_close(bridge *b);

const char *bridge_name(const bridge *b);
const char *bridge_vendor(const bridge *b);
int bridge_num_programs(const bridge *b);
int bridge_num_params(const bridge *b);
int bridge_num_inputs(const bridge *b);
int bridge_num_outputs(const bridge *b);
int bridge_is_synth(const bridge *b);
/* Whether the helper is still there. A plug-in that faults takes its helper
 * with it, and everything after that quietly returns nothing -- a frozen
 * editor and silence, with no way for the window to say which. */
int bridge_alive(const bridge *b);
void bridge_set_input_mask(bridge *b, unsigned mask);
int bridge_unique_id(const bridge *b);

void  bridge_param_name(bridge *b, int i, char *buf, int n);
void  bridge_param_label(bridge *b, int i, char *buf, int n);
void  bridge_param_display(bridge *b, int i, char *buf, int n);
void  bridge_program_name(bridge *b, int i, char *buf, int n);
float bridge_get_param(bridge *b, int i);
void  bridge_set_param(bridge *b, int i, float v);
void  bridge_set_program(bridge *b, int i);
int   bridge_get_program(bridge *b);

void  bridge_midi(bridge *b, int status, int d1, int d2);
/* With the frame within the next block at which it belongs; -1 for "on drain".
 * This is what keeps a sequencer's timing intact across the process boundary. */
void  bridge_midi_at(bridge *b, int status, int d1, int d2, int at);
void  bridge_all_notes_off(bridge *b);

void  bridge_render_io(bridge *b, const float *in, float *out, int frames);

/* How many times the helper missed its deadline. Non-zero means the bridge is
 * dropping audio, which is worth surfacing rather than hiding. */
unsigned bridge_xruns(const bridge *b);

int  bridge_editor_kind(bridge *b);          /* 0 none, 2 pixels */
void bridge_editor_size(bridge *b, int *w, int *h);
int  bridge_editor_open(bridge *b);
void bridge_editor_close(bridge *b);
int  bridge_editor_pixels(bridge *b, const unsigned int **px, int *w, int *h);
void bridge_editor_mouse(bridge *b, int x, int y, int msg, int buttons, int wheel);
void bridge_editor_key(bridge *b, int vk, int down, int ch);

void bridge_import_stats(bridge *b, int *implemented, int *stubbed, int *hit);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_BRIDGE_CLIENT_H */
