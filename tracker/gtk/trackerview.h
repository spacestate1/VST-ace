/* trackerview -- the GTK tracker as one widget a shell can embed.
 *
 * The standalone tracker-gtk wraps this in a window (tracker/gtk/main.c) and
 * nothing here changes for it: no sinks, the window's own title and close
 * flow. A shell -- studiogtk -- makes one of these on the engine it owns,
 * packs trk_view_widget() into a tab, offers its synth tabs as in-process
 * destinations through trk_view_set_sinks(), and closes the tab through
 * trk_view_confirm_close() so an unsaved song is still asked about.
 *
 * Everything runs on the GTK thread. The engine is the caller's: opened
 * before trk_view_new, stopped and closed after the view is freed. */
#ifndef TRK_VIEW_H
#define TRK_VIEW_H

#include <gtk/gtk.h>

#include "trk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trk_view trk_view;

/* The shell's in-process MIDI destinations, beside the ALSA windows
 * trk_list_dests lists -- the mirror of the Qt shell's midiSinks /
 * midiSinkPicked. A track's destination list shows them as "this window:
 * <name>" after the windows. The defaults -- no provider set -- are no
 * destinations and nothing offered, which is the standalone's whole answer. */
typedef struct {
    /* Fill out[0..max) with the destination names; return the count. Names
     * must stay valid until the next call and be unique. */
    int  (*names)(void *ud, const char **out, int max);
    /* A track's destination picked the sink called name -- or a window (or
     * nowhere) again, name NULL, to route the track back off sinks. The shell
     * makes the routing change (trk_route_sink). */
    void (*picked)(void *ud, int track, const char *name);
} trk_view_sinks;

/* Allocate the view on an engine. No GTK yet -- this may run before the
 * toolkit is up. trk_view_widget builds the widgets and is called once GTK
 * is; it returns the same widget on every call. */
trk_view  *trk_view_new(trk_engine *e);
GtkWidget *trk_view_widget(trk_view *v);

/* Load a song into the view (0 on success), or reset the view onto the
 * engine's current song. */
int        trk_view_open(trk_view *v, const char *path);
void       trk_view_reset(trk_view *v);

/* The toolbar's song commands, for a shell's menus: the exact code paths the
 * view's own New, Save and Save As buttons run, so an embedded view and the
 * standalone behave alike. New song asks about unsaved changes first (the
 * Save/Discard/Cancel flow, cancel is silence). Save writes the song's file,
 * turning into Save As when it was never saved; Save As always asks for a
 * file. */
void       trk_view_new_song(trk_view *v);
void       trk_view_save(trk_view *v);
void       trk_view_save_as(trk_view *v);
/* Export MIDI: asks for a .mid file and writes the song into it. Not a save. */
void       trk_view_export_midi(trk_view *v);

/* A shell that carries the view's file, samples and help commands in its own
 * menu bar says so before the widget is made: the toolbar then keeps only the
 * transport. The commands themselves, for that menu's actions. */
void       trk_view_set_embedded(trk_view *v, int on);
/* Samples > Audio Output: the window for choosing where the tracker's own audio goes. */
void       trk_view_audio_output(trk_view *v);
/* Recording: the last take as a MIDI file, and the options window. */
void       trk_view_export_take(trk_view *v);
void       trk_view_record_options(trk_view *v);
void       trk_view_load_samples(trk_view *v);
void       trk_view_edit_samples(trk_view *v);
void       trk_view_show_keys(trk_view *v);
void       trk_view_show_columns(trk_view *v);
void       trk_view_show_cheat(trk_view *v);

/* The shell's destinations; NULL takes them away again. The api struct is
 * copied. Refreshes the destination lists when the widget exists. */
void       trk_view_set_sinks(trk_view *v, const trk_view_sinks *api, void *ud);

/* Called after a song is loaded -- the view's own Open button and a shell's
 * open alike -- so the shell can reopen the synths the song's sink lines
 * name. Unset: nothing, the standalone's answer. */
void       trk_view_set_song_opened(trk_view *v, void (*cb)(void *ud), void *ud);

/* Mark the song as saved -- for a shell that wrote into it itself, as the Qt
 * shell does before routing a track for its scripted proof. */
void       trk_view_mark_clean(trk_view *v);

int        trk_view_dirty(trk_view *v);

/* Where the song lives, "" when it was never saved -- what a shell records
 * in a session file. */
const char *trk_view_path(trk_view *v);

/* Make sure the song is saved before whatever cb does -- a shell writing a
 * session file, which records the song by its path. Runs the same
 * Save/Discard/Cancel flow as trk_view_confirm_close when there are unsaved
 * changes; cb is called when the way is clear (saved, discarded, or nothing
 * to lose), never on cancel -- so ud must not be something only cb frees.
 * Unlike confirm_close, the view is left as it was, not marked closing. */
void       trk_view_ensure_saved(trk_view *v, void (*cb)(void *ud), void *ud);

/* Ask whether the view may be destroyed, asking the user first when the song
 * has unsaved changes (the same Save/Discard/Cancel flow the standalone's
 * window close runs). cb is called -- synchronously when the song is clean,
 * from the dialog's answer otherwise -- only when the close should proceed;
 * cancel is silence. Playback is left to the caller. */
void       trk_view_confirm_close(trk_view *v, void (*cb)(void *ud), void *ud);

/* Free the view. The widget must already be destroyed -- removing it from
 * its container is enough; the view's timers and child windows go with it. */
void       trk_view_free(trk_view *v);

/* The standalone binary: wrap the view in an application window with the
 * standalone's title, size and close-request save flow, present it, and load
 * song (or reset onto an empty song when NULL). Called from activate. */
void       trk_view_standalone(trk_view *v, GtkApplication *app, const char *song);

#ifdef TRACKER_UITEST
/* Install the scripted drive (an idle source; argv: song, output dir) and
 * report how many checks failed once it has run. See uitest() in
 * trackerview.c. */
void       trk_view_uitest(trk_view *v, const char *outdir);
int        trk_view_uitest_failures(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TRK_VIEW_H */
