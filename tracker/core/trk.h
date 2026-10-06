/* tracker -- a pattern sequencer that plays other programs over MIDI.
 *
 * Open a vst-ace window per instrument, give each track of the tracker one of
 * them as its destination, and the tracker plays them all: every track is its
 * own ALSA sequencer port, connected to whatever that track names, and a
 * separate clock port drives tempo-synced plug-ins. A track can instead play
 * a sample set -- a folder of WAVs, a note each -- which the tracker sounds
 * itself.
 *
 * This is the whole of it apart from the drawing. The Qt and GTK windows are
 * thin -- they translate keys into trk_key() calls and paint what
 * trk_cell_text() says -- so the two can never disagree about what a key does
 * or what a song file means.
 *
 * Threading: the engine runs its own scheduling thread. Anything that reads
 * or writes the song from another thread holds trk_lock() around it; the
 * trk_* calls below that take the engine do their own locking.
 */
#ifndef TRK_H
#define TRK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRK_TRACKS     8
#define TRK_ROWS_MAX   256
#define TRK_PATTERNS   100
#define TRK_ORDER_MAX  256
#define TRK_NAME_LEN   32
#define TRK_DEST_LEN   128
#define TRK_PATH_LEN   512

/* Cell values. A note is 0..127; the two markers sit above the MIDI range. */
#define TRK_EMPTY      0xFF          /* note, vel, cc or val: nothing here */
#define TRK_NOTE_OFF   0xFE          /* note: release whatever this track holds */

typedef struct {
    uint8_t note;                    /* 0..127, TRK_NOTE_OFF or TRK_EMPTY */
    uint8_t vel;                     /* 1..127, or TRK_EMPTY: the track's default */
    uint8_t cc;                      /* controller number, or TRK_EMPTY */
    uint8_t val;                     /* controller value, or TRK_EMPTY */
} trk_cell;

/* A pattern is a part of the song -- "Verse", "Chorus" -- and the order
 * list plays parts in sequence, a part as often as it appears in it. */
typedef struct {
    int      rows;                   /* 1..TRK_ROWS_MAX */
    char     name[TRK_NAME_LEN];     /* "" until named: shown as its number */
    trk_cell cell[TRK_ROWS_MAX][TRK_TRACKS];
} trk_pattern;

typedef struct {
    char name[TRK_NAME_LEN];
    /* Where it plays: an ALSA client name and one of its port names, as
     * aconnect lists them -- "pestudio 2" and "pestudio in". Names rather
     * than numbers, because the numbers change every time a window opens and
     * the names are what a saved song has to find again. Empty: nowhere.
     */
    char client[TRK_DEST_LEN];
    char port[TRK_DEST_LEN];
    /* Or a sample set, which the tracker plays itself: a name from
     * trk_list_sample_sets ("drum-singles"), or a folder's path. The note
     * picks the sample, as the set's kit.txt says or from C-4 up. Set, it
     * wins over client and port. Empty: none. */
    char samples[TRK_PATH_LEN];
    int  channel;                    /* 0..15 */
    int  octave;                     /* 0..9: the note keys' octave on this track */
    int  velocity;                   /* used where a cell gives none */
    int  mute;
} trk_track;

typedef struct {
    double      bpm;
    int         lpb;                 /* rows per beat: one of trk_lpb_ok() */
    int         volume;              /* master, in percent, 0..150: what the tracker sounds itself */
    trk_track   track[TRK_TRACKS];
    trk_pattern pattern[TRK_PATTERNS];
    int         order[TRK_ORDER_MAX];
    int         norder;              /* >= 1 */
} trk_song;

/* ------------------------------------------------------------------ song */

void trk_song_init(trk_song *s);                 /* empty song, one pattern */
int  trk_song_load(trk_song *s, const char *path, char *err, size_t errn);
int  trk_song_save(const trk_song *s, const char *path, char *err, size_t errn);
int  trk_lpb_ok(int lpb);                        /* divides the queue's PPQ */
int  trk_pattern_used(const trk_song *s, int p); /* any cell set */
/* A part as a list shows it: its name, or "Part 3" for pattern 3. buf >= TRK_NAME_LEN + 8. */
const char *trk_part_label(const trk_song *s, int p, char *buf);

/* "C-4", "C#4", "===" for a note-off, "..." for empty. buf >= 4. */
const char *trk_note_name(int note, char *buf);

/* One cell as the grid shows it, 12 columns: "C-4 64 07 7F", with dots for
 * what is empty. buf >= 13. Column offsets of the fields are TRK_COL_* . */
const char *trk_cell_text(const trk_cell *c, char *buf);
enum { TRK_COL_NOTE = 0, TRK_COL_VEL = 4, TRK_COL_CC = 7, TRK_COL_VAL = 10,
       TRK_CELL_CHARS = 12 };

/* ---------------------------------------------------------------- engine */

typedef struct trk_engine trk_engine;

/* Opens the sequencer client "tracker" (numbered if one is running already),
 * one output port per track, a clock port, and the scheduling thread. NULL
 * with a reason in err when ALSA's sequencer is not there. */
trk_engine *trk_open(char *err, size_t errn);
void        trk_close(trk_engine *e);            /* stops, releases, closes */

trk_song   *trk_song_of(trk_engine *e);
void        trk_lock(trk_engine *e);
void        trk_unlock(trk_engine *e);
const char *trk_client_name(trk_engine *e);      /* as other programs list it */
/* What the samples play out of -- "samples: default, 38 ms" -- or why they
 * cannot; empty until a track has a sample set. */
const char *trk_audio_status(trk_engine *e);

/* Subscribe each track's port to what its client/port names, and the clock
 * port to every distinct client the tracks play. Call after changing a
 * track's destination, or after other windows have come and gone. Returns
 * how many tracks name a destination that is not there right now. */
int  trk_route(trk_engine *e);
/* Whether track t is connected to what it names, as of the last trk_route. */
int  trk_routed(trk_engine *e, int t);

/* Every port another program offers to be played on, as "client\tport"
 * lines, written into buf; returns the count. Our own ports are left out. */
int  trk_list_dests(trk_engine *e, char *buf, size_t n);

/* Every sample set a track can play, as "name\tpath" lines; returns the
 * count. Those loaded with trk_add_sample_set (named by their path), then
 * each folder of WAVs under the sample folders and VA_KITS. */
int  trk_list_sample_sets(trk_engine *e, char *buf, size_t n);
/* A folder from anywhere, offered from now on. */
void trk_add_sample_set(trk_engine *e, const char *path);
/* A set's folder, from what a track calls it. -1 when there is none. */
int  trk_sample_set_dir(trk_engine *e, const char *name, char *out, size_t n);
/* What track t's note plays: 1 and the sample's name, when its set has one
 * there; 0 when it has none, with "drum-singles runs C-4 to G#5" in name;
 * -1 when the track plays no set, or its set is not loaded. */
int  trk_sample_at(trk_engine *e, int t, int note, char *name, size_t n);
/* Which notes track t's sample set has a sample on, as 128 bits (note n is
 * bit n%8 of mask[n/8]). Returns 1, or 0 -- mask untouched -- when the track
 * plays no loaded set. Takes the engine's lock: call it before trk_lock, as
 * a grid does to mark the notes that will play nothing. */
int  trk_sample_mask(trk_engine *e, int t, unsigned char mask[16]);
/* Track t's samples in note order, "note\tname" a line (note as MIDI
 * 0..127); returns the count, 0 when the track plays no set or it is not
 * loaded. What a cheat sheet under the track lists. */
int  trk_list_pads(trk_engine *e, int t, char *buf, size_t n);
/* Read a set again -- its kit.txt has been changed -- and route. */
void trk_reload_sample_set(trk_engine *e, const char *name);
/* Play one WAV now, as an editor's Play button does: `file` from `dir`, or
 * a path of its own. -1 when it will not load or there is no output. */
int  trk_audition(trk_engine *e, const char *dir, const char *file, double gain_db, int vel);

/* ------------------------------------------------------- in-process sinks --
 *
 * A MIDI destination inside this process, registered alongside the ALSA
 * outputs: a synth the same program hosts, played without a trip through the
 * sequencer. A track routed to a sink plays there INSTEAD of its ALSA window
 * -- one destination per track, as with a window or a sample set; the window
 * its client/port name stays in the song, and routing back (-1) reconnects
 * it. Sink routing is runtime state: a saved song carries no sink.
 *
 * Timing mirrors the sample sets: the scheduling thread queues every event
 * against the queue's tick clock, and a delivery thread turns the tick into
 * wall time exactly as the ALSA path's timestamps mean it, calling the sink
 * once per block with the block's start and each event's sample offset in it
 * -- so a sink gets the placement the kernel's queue gives an ALSA window.
 * Clock, start/stop and song position follow a sink-routed track as they
 * follow a window. */

#define TRK_SINK_RATE   48000          /* the clock `frame` counts on */
typedef struct {
    uint32_t frame;                    /* sample offset within the block */
    uint8_t  status, d1, d2;           /* a raw MIDI message */
} trk_sink_ev;

/* Called from the engine's delivery thread, up to one block ahead of `wall`
 * (CLOCK_MONOTONIC seconds, when the block's first sample is due). It runs
 * under the delivery lock: keep it fast, never block, and never call back
 * into the engine. */
typedef void (*trk_sink_fn)(void *ud, double wall, const trk_sink_ev *evs, int n);

/* A named in-process destination -- the name is what a destination list
 * shows. Returns its id (>= 0), or -1 when the slots are full. */
int  trk_add_sink(trk_engine *e, const char *name, trk_sink_fn fn, void *ud);
/* Unregister: tracks routed to the sink go back to their ALSA windows (what
 * they have sounding is released to the sink first, while it is still there
 * to hear it), and any delivery call in flight is waited out -- after this
 * returns, the sink is never called again. */
void trk_remove_sink(trk_engine *e, int id);
void trk_sink_rename(trk_engine *e, int id, const char *name);
/* Route track t to a sink, or back to its ALSA window with id -1. What the
 * track has sounding is released to the destination it is leaving. */
void trk_route_sink(trk_engine *e, int t, int id);
/* The sink track t plays, or -1. */
int  trk_sink_of(trk_engine *e, int t);
/* A sink's name, copied out; -1 when there is no such sink. */
int  trk_sink_name(trk_engine *e, int id, char *buf, size_t n);

enum { TRK_PLAY_PATTERN = 0, TRK_PLAY_SONG = 1 };
/* Start at a position: `order` indexes the order list (the pattern is
 * order[] of it) for TRK_PLAY_SONG; for TRK_PLAY_PATTERN `order` is the
 * pattern number itself, looped. */
void trk_play(trk_engine *e, int mode, int order, int row);
void trk_stop(trk_engine *e);                    /* and release every note */
int  trk_playing(trk_engine *e);
/* What is sounding now -- not what has been scheduled ahead. -1s when
 * stopped. */
void trk_position(trk_engine *e, int *order, int *pattern, int *row);
void trk_set_bpm(trk_engine *e, double bpm);     /* takes effect at once */

/* Play one note now, as feedback while entering it. Released by
 * trk_preview_off, by the next preview on that track, or by trk_panic. */
void trk_preview(trk_engine *e, int track, int note, int vel);
void trk_preview_off(trk_engine *e, int track);
/* Note-off for every note this program may have started, on every track,
 * then CC 123 and 120 on every channel any track uses. */
void trk_panic(trk_engine *e);

/* ----------------------------------------------------------- arranging --
 *
 * The order list, changed as a Parts panel changes it. Each takes the
 * engine's lock itself; each returns where the entry it acted on now is
 * (the place to select), or -1 when it could not. */

/* Insert pattern p into the order after entry `at` (-1: at the front). */
int  trk_order_insert(trk_engine *e, int at, int p);
/* Take entry `at` out of the order -- the pattern itself is kept. The last
 * entry stays: a song plays at least one part. */
int  trk_order_remove(trk_engine *e, int at);
/* Move entry `at` by `by` places, up (-1) or down (+1). */
int  trk_order_move(trk_engine *e, int at, int by);
/* A pattern no part uses yet -- empty, unnamed, at the default length -- or
 * -1 when all TRK_PATTERNS are taken. With `from` >= 0, it starts as a copy
 * of that pattern, named "<its name> 2". */
int  trk_pattern_new(trk_engine *e, int from);

/* ---------------------------------------------------------------- editor */

/* Cursor and entry state. Kept here rather than in the windows so that what
 * every key does is decided once. */
enum { TRK_F_NOTE = 0, TRK_F_VEL, TRK_F_CC, TRK_F_VAL, TRK_FIELDS };

typedef struct {
    int pattern;                     /* the pattern being edited */
    int row, track, field;
    int digit;                       /* 0: next hex digit is the high one */
    int octave;                      /* the cursor's track's octave, as trk_key last left it */
    int step;                        /* rows to advance after an entry, 0..16 */
    int follow;                      /* cursor follows playback */
    int edit;                        /* keys write into the pattern; off, notes only sound */
    int part;                        /* the order entry being edited: where F5 starts */
    /* A block of cells selected, to copy, cut, paste over or clear: from one
     * corner to the other, either way round. */
    int sel;                         /* a block is selected */
    int sel_r0, sel_t0, sel_r1, sel_t1;
    int held[TRK_TRACKS];            /* key char previewing on each track, or 0 */
} trk_editor;

void trk_editor_init(trk_editor *ed);

/* Keys that are not characters. Everything printable arrives as itself. */
enum {
    TRK_K_UP = 0x100, TRK_K_DOWN, TRK_K_LEFT, TRK_K_RIGHT,
    TRK_K_PGUP, TRK_K_PGDN, TRK_K_HOME, TRK_K_END,
    TRK_K_TAB, TRK_K_BACKTAB, TRK_K_DELETE, TRK_K_BACKSPACE, TRK_K_INSERT,
    TRK_K_PLAY_SONG, TRK_K_PLAY_PATTERN, TRK_K_STOP, TRK_K_TOGGLE,
    TRK_K_OCT_DOWN, TRK_K_OCT_UP, TRK_K_PAT_PREV, TRK_K_PAT_NEXT,
    TRK_K_EDIT,                      /* edit mode on / off */
    /* Selecting -- the arrows with Shift: the block from where it started to
     * the cursor -- and the clipboard. */
    TRK_K_SEL_UP, TRK_K_SEL_DOWN, TRK_K_SEL_LEFT, TRK_K_SEL_RIGHT,
    TRK_K_SEL_ALL, TRK_K_COPY, TRK_K_CUT, TRK_K_PASTE
};

/* Feed one key press. Returns non-zero when the screen should be redrawn.
 * Note keys sound a preview on the cursor's track until trk_key_release
 * gets the same character. */
int  trk_key(trk_engine *e, trk_editor *ed, int key);
void trk_key_release(trk_engine *e, trk_editor *ed, int key);

/* Selecting. trk_select sets a block, corners either way round, and moves
 * the cursor to the second; trk_selection gives it top-left to bottom-right,
 * returning 0 when nothing is selected. */
void trk_select(trk_editor *ed, int r0, int t0, int r1, int t1);
void trk_select_none(trk_editor *ed);
int  trk_selection(const trk_editor *ed, int *r0, int *t0, int *r1, int *t1);
int  trk_selected(const trk_editor *ed, int r, int t);

/* The clipboard, shared by every pattern and part. Copy takes the block
 * selected, or the cursor's cell; paste puts it down with its top-left at
 * the cursor, cut off at the pattern's edges; clear empties the block. Each
 * returns how many cells it touched. Cut, paste and clear change nothing
 * with edit mode off. trk_clipboard gives the size of what is held. */
int  trk_copy(trk_engine *e, trk_editor *ed);
int  trk_cut(trk_engine *e, trk_editor *ed);
int  trk_paste(trk_engine *e, trk_editor *ed);
int  trk_clear_block(trk_engine *e, trk_editor *ed);
int  trk_clipboard(int *rows, int *tracks);

/* The note a computer-keyboard key plays at an octave, or -1. Two rows,
 * tracker fashion: zsxdcvgbhnjm is one octave, q2w3er5t6y7u the next. */
int  trk_key_note(int key, int octave);

/* The cheat sheet for track t, lines of at most `width` characters: for a
 * track playing a sample set, each sample with its note and the key that
 * types it at `octave` ("z C-4 Clap 08"); for any other, the note keys.
 * `octave` -1 is the track's own.
 * Returns the number of lines. */
int  trk_cheat_sheet(trk_engine *e, int t, int octave, int width, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* TRK_H */
