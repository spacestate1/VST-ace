/* plugview -- the plug-in half of dwstudio.
 *
 * dwstudio began as a front end for the engines in c/src: it read preset banks
 * out of a plug-in's resources and played them through a reimplementation. It
 * never loaded the plug-in. pestudio, the Qt window, does the opposite -- it
 * runs the real thing through pehost and shows the plug-in's own editor.
 *
 * This is that half, in GTK. It is the same pehost API pestudio drives, so the
 * two windows host identically and differ only in toolkit: pick a directory,
 * pick a plug-in, pick a program, move its parameters, or open the editor the
 * plug-in draws itself.
 *
 * Everything here runs on the GTK thread except plugview_render, which the
 * audio callback owns. */
#ifndef DW_PLUGVIEW_H
#define DW_PLUGVIEW_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the pane.
 *
 * `park` and `unpark` stop and restart the caller's audio callback. Loading a
 * plug-in frees the one the callback may be rendering out of, so they are not
 * optional -- dwstudio already had this pair for swapping engines. */
GtkWidget *plugview_new(void (*park)(void), void (*unpark)(void),
                        double samplerate, int blocksize);

/* Fill the plug-in list from a directory. Safe before the pane is realised. */
void plugview_scan(const char *dir);

/* The two File-menu commands. They were buttons in this pane; they are menu
 * items now, in the same place and under the same names as pestudio's, so the
 * way into a plug-in does not depend on which window is open.
 *
 * plugview_open_vst picks one plug-in and loads it, adding it to the list even
 * when it lives outside the scanned folder. plugview_load_folder picks a
 * folder and rescans. Both are asynchronous -- the dialog returns immediately
 * and the work happens when the user chooses. */
void plugview_open_vst(GtkWindow *parent);
void plugview_load_folder(GtkWindow *parent);

/* File > Save Patch / Open Patch: the plug-in's current parameters written as
 * JSON, and read back. The plug-in's own programs are its factory presets and
 * cannot be written to; this is where a sound somebody made goes. Same format
 * and same files as `va peload --save-patch` and `--patch`, and as pestudio.
 *
 * Opening a file holding several patches applies the first -- pestudio lists
 * them all, which is the one place the two windows differ, because this one has
 * no list to put them in. */
void plugview_save_patch(GtkWindow *parent);
void plugview_load_patch(GtkWindow *parent);

/* Settings > Plug-in Folders: the folders searched for plug-ins, each under
 * the platform it holds. Persisted, and shared with pestudio -- one answer per
 * machine to "where are my plug-ins", not one per window. See vstdirs.h. */
void plugview_edit_folders(GtkWindow *parent);

/* Settings > Enter Key / Serial: type a registration key into the editor of
 * whatever is loaded. Some plug-ins do nothing until something has been typed
 * into them -- daHornet keeps a registration panel over its own interface and
 * makes no sound until its serial number has been entered into an edit box on
 * it -- and a key is twenty-five characters nobody wants to mistype into a
 * skinned field with no visible caret. Asks once, then sends it a character at
 * a time to whatever the editor has focused. */
void plugview_enter_key(GtkWindow *parent);

/* True when a plug-in is loaded, i.e. when it -- and not an engine -- is what
 * should be heard. Cheap enough for the audio callback. */
int  plugview_active(void);

/* Audio thread. Fills `out` with `frames` interleaved stereo frames and
 * returns 1; returns 0 when no plug-in is loaded, leaving `out` untouched. */
int  plugview_render(float *out, int frames);

/* From the GTK thread, which is also where dwstudio's MIDI poll runs. No-ops
 * when nothing is loaded. */
void plugview_note_on(int note, int vel);
void plugview_note_off(int note);
void plugview_all_notes_off(void);
void plugview_program(int idx);

/* Pitch bend, in MIDI's own 14-bit form (0..16383, 8192 at rest). Left in that
 * form rather than converted to semitones because bend range is the plug-in's
 * parameter, and converting here would mean guessing it. */
void plugview_bend(int value14);

/* Walk the whole list unattended, opening each plug-in's editor in turn, and
 * report what happened for each. Switching plug-ins with an editor attached is
 * the failure-prone path and clicking through fifty-odd of them by hand is not
 * repeatable -- the same reason pestudio has --cycle, and the same option name
 * so the two can be compared on one corpus. */
void plugview_start_cycle(int ms);

/* Link in the data the scanned plug-ins are missing and this machine already
 * has -- a u-he release's Images and Fonts, which its installer would have put
 * in ~/.u-he/<Product>/. Reports what it did in the status line. A firmware ROM
 * is not in any download, so those plug-ins are reported and left alone.
 *
 * A menu command rather than something loading does by itself: it writes into
 * the user's home directory. */
void plugview_install_missing_data(void);

/* Close whatever is loaded. The caller must have parked the audio first. */
void plugview_shutdown(void);

#ifdef __cplusplus
}
#endif


/* The directory to open on when the caller named none: the checkout's own
 * corpus, else a standard system VST location, else $HOME. */
const char *plugview_default_dir(void);

#endif /* DW_PLUGVIEW_H */
