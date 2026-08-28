/* Minimal VST3 host, sufficient to play a plugin and drive its parameters.
 *
 * VST3 is COM-shaped: every interface begins with the three FUnknown slots and
 * objects are reached through vtables. Nothing from the Steinberg SDK is used
 * here -- the interfaces are declared directly, which keeps this a plain C
 * module with no C++ or licence entanglement.
 *
 * Handles native Linux .vst3 bundles (ELF, loaded with dlopen). The interface
 * declarations are deliberately calling-convention agnostic so the same code
 * can later drive a Windows .vst3 mapped by the PE loader, where the vtable
 * uses the Microsoft ABI instead. */
#ifndef PELOAD_VST3_H
#define PELOAD_VST3_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v3host v3host;

/* ---- editor support -----------------------------------------------------
 *
 * A VST3 editor draws itself: the host supplies a native parent window and the
 * plugin creates its own child window inside it. On Linux that parent is an X11
 * window id, so a Qt widget's winId() is all that is needed and the host writes
 * no drawing code.
 *
 * Linux plugins also expect the host to own the event loop -- they register
 * their X11 descriptors and timers with us rather than spinning their own, and
 * JUCE-based editors will not repaint otherwise. The UI layer supplies these
 * hooks; this module calls v3_runloop_fd/v3_runloop_timer back into the plugin
 * when they fire. */
typedef struct {
    void  *ud;
    void (*add_fd)(void *ud, void *handler, int fd);
    void (*del_fd)(void *ud, void *handler);
    void (*add_timer)(void *ud, void *handler, unsigned long long ms);
    void (*del_timer)(void *ud, void *handler);
    void (*resize)(void *ud, int w, int h);
} v3_runloop_hooks;

void v3_set_runloop_hooks(const v3_runloop_hooks *h);
void v3_runloop_fd(void *handler, int fd);
void v3_runloop_timer(void *handler);

/* 1 if the plugin offers an editor embeddable in an X11 window. */
int  v3_has_editor(v3host *h);
/* Natural editor size in pixels; 0 if unknown. */
void v3_editor_size(v3host *h, int *w, int *h_out);
int  v3_editor_can_resize(v3host *h);
/* Attach to an X11 window id. Returns 0 on success. */
int  v3_editor_attach(v3host *h, unsigned long x11_window);
/* Windows VST3: the editor wants an HWND, which the Win32 layer supplies. */
int  v3_editor_is_hwnd(v3host *h);
int  v3_editor_attach_hwnd(v3host *h, void *hwnd);
void v3_editor_detach(v3host *h);
/* Tell a resizable editor its new size. */
void v3_editor_resized(v3host *h, int w, int h_px);

/* `path` may be a .vst3 bundle directory or the shared object inside it. */
v3host *v3_open(const char *path, double samplerate, int blocksize,
                char *err, int errlen);
void    v3_close(v3host *h);

const char *v3_name(const v3host *h);
const char *v3_vendor(const v3host *h);
int         v3_num_params(const v3host *h);
int         v3_num_inputs(const v3host *h);
int         v3_num_outputs(const v3host *h);
int         v3_is_synth(const v3host *h);
int         v3_num_programs(const v3host *h);

void  v3_program_name(v3host *h, int idx, char *buf, int n);
void  v3_set_program(v3host *h, int idx);
int   v3_get_program(const v3host *h);

void  v3_param_name(v3host *h, int i, char *buf, int n);
void  v3_param_display(v3host *h, int i, char *buf, int n);
float v3_get_param(v3host *h, int i);
void  v3_set_param(v3host *h, int i, float v);      /* queued */

void  v3_midi(v3host *h, int status, int d1, int d2);  /* queued */

/* Interleaved stereo in (may be NULL) and out. Drains the queue first. */
void  v3_render(v3host *h, const float *in, float *out, int frames);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_VST3_H */
