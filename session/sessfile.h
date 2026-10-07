/* sessfile -- a session of the studio window as one JSON file.
 *
 * What a session is: which synth tabs are open, with which plug-in in each
 * and the sound it was making (a patch, in patch.h's own format, embedded
 * verbatim); which song the tracker tab has open; and which of the song's
 * tracks play which tab directly. Everything named by absolute path, so the
 * file means the same thing from any working directory:
 *
 *   {
 *     "session": "vst-ace",
 *     "version": 1,
 *     "synths": [
 *       { "plugin": "/abs/blooo64.dll", "patch": { ...patch JSON... } },
 *       { "plugin": "/abs/Surge XT.vst3" }
 *     ],
 *     "song":   "/abs/song.trk",
 *     "routes": [ { "track": 0, "synth": 0, "sink": "blooo" } ]
 *   }
 *
 * A synth tab with nothing loaded is not recorded -- restoring it would be an
 * empty tab, which is no part of a session. "song" is "" when no tracker tab
 * was open or its song was never saved; "routes" names tracks (0-based)
 * routed to a synth tab, by the tab's index in "synths" -- which the restore
 * maps to the tab it just created, so tabs already open, or two tabs hosting
 * the same plug-in, cannot capture the route -- and by the tab's destination
 * name, kept for reading and as the fallback when "synth" is absent. A track routed to an ALSA window is the song file's
 * own business and is not repeated here.
 *
 * Both shells read and write this through the same code, so a session saved
 * in studio opens in studiogtk and back. The parser is hand-written in the
 * idiom of patch.c's: strict about structure, forgiving about content --
 * unknown keys are skipped, so a newer file still loads. */
#ifndef VSTACE_SESSFILE_H
#define VSTACE_SESSFILE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *plugin;   /* absolute path of the plug-in the tab hosted */
    char *patch;    /* its sound, patch.h JSON text, or NULL */
} sess_synth;

typedef struct {
    int   track;    /* 0-based */
    int   synth;    /* index into sess_file.synths, -1 when not known */
    char *sink;     /* the synth tab's destination name */
} sess_route;

typedef struct {
    sess_synth *synths;
    int         nsynths;
    char       *song;       /* absolute path, "" for none */
    sess_route *routes;
    int         nroutes;
} sess_file;

/* Write `s` to `path`. Returns 0, or -1 with a one-line reason in `err`. */
int sess_write(const char *path, const sess_file *s, char *err, int errn);

/* Read one back. Returns NULL with a one-line reason in `err`; the caller
 * frees with sess_free. */
sess_file *sess_read(const char *path, char *err, int errn);
void       sess_free(sess_file *s);

#ifdef __cplusplus
}
#endif

#endif /* VSTACE_SESSFILE_H */
