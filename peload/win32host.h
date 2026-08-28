/* Host side of the Win32 window layer: what a UI toolkit needs in order to show
 * a Windows plugin's editor and feed it input.
 *
 * The plugin renders its interface into a pixel buffer we own (see win32gui.h);
 * `present` hands that buffer over so the toolkit can put it on screen. Nothing
 * here draws, and nothing here is thread-safe: call it all from the UI thread. */
#ifndef PELOAD_WIN32HOST_H
#define PELOAD_WIN32HOST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ud;
    /* The window's pixels changed. `px` is w*h 32-bit BGRX, top-down, and stays
     * valid only for the duration of the call. */
    void (*present)(void *ud, const unsigned int *px, int w, int h);
    /* The plugin resized its own window and wants the container to follow. */
    void (*resize)(void *ud, int w, int h);
    /* The plugin is spinning in its own message loop -- a chance to deliver input
     * that has arrived since it started.
     *
     * Needed because a modal drag loop is a trap otherwise. TAL's editor polls
     * GetAsyncKeyState(VK_LBUTTON) until the button comes up, and the only agent
     * that knows it came up is the host -- which is stuck inside the very
     * wndproc call that started the loop. Under the bridge the host is a separate
     * process and can publish the state through shared memory, and this is where
     * the helper picks it up. Called from PeekMessage/GetMessage and from the
     * key-state and cursor queries. May be NULL. */
    void (*pump_input)(void *ud);
} w32_host_hooks;

void  w32_set_hooks(const w32_host_hooks *h);

/* Install just the input pump (see the struct above). Separate from set_hooks so
 * the toolkit's present callback is not disturbed. */
void  w32_set_input_pump(void (*fn)(void *), void *ud);

/* Create the container window an editor is parented to, and return its HWND to
 * pass to effEditOpen or IPlugView::attached. Idempotent. */
void *w32_create_host_window(int w, int h);

/* The window whose pixels represent the editor -- the plugin's own window once
 * it has made one, otherwise the container. NULL if there is no editor. */
void *w32_root_window(void);

/* The size of a window we created, for a plugin that builds its editor but does
 * not report a rect. Returns 0 if the handle is unknown or the size is empty. */
int   w32_window_size(void *hwnd, int *w, int *h);

/* Pull the editor's current pixels instead of waiting for a present callback. */
int   w32_editor_pixels(const unsigned int **px, int *w, int *h);

/* Tell the plugin's window it is now this size. */
void  w32_set_client_size(int w, int h);

/* Mark the editor visible and force a full repaint. */
void  w32_show_editor(void);

/* Dispatch queued messages, fire due timers, repaint invalid windows.
 * Drive this from a ~16 ms UI timer. */
void  w32_pump(void);

/* Discard all window state. Call when the plugin that owns it goes away. */
void  w32_reset(void);

/* False when a plugin registered a font that never made it through to the text
 * backend -- its editor cannot paint and must not be pumped. */
int   w32_font_pipeline_ok(void);

/* Diagnostic: how many times each drawing entry point was reached. */
void  w32_stats(void);

/* Synthesised input. `msg` is a WM_* mouse message; `buttons` is the MK_* mask.
 * Wheel notches go in `wheel` (positive = away from the user). */
void  w32_mouse(int x, int y, int msg, int buttons, int wheel);
void  w32_key(int vk, int down, int ch);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_WIN32HOST_H */
