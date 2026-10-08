/* Sample-playback drum engine.
 *
 * A kit is a directory of WAV files. Which note plays which file is said by a
 * kit.txt in that directory, when there is one:
 *
 *   # pad <note> [gain <dB>] [choke <group>] file <name.wav>
 *   pad C-4 file Kick 32.wav
 *   pad F#2 choke 1 file Hat closed.wav
 *   pad A#2 choke 1 gain -3 file Hat open.wav
 *
 * Notes are written as the tracker writes them (C-4 is MIDI 60) or as MIDI
 * numbers; under C-0 the octave goes negative (C--1 is MIDI 0). Pads in one
 * choke group cut each other off, the way a closed hi-hat stops an open one.
 * The file name is the rest of the line, spaces and all. Without a kit.txt --
 * or with one that names not one pad -- every .wav is a pad, sorted by name,
 * on consecutive notes from DK_BASE_NOTE up -- C-4, where a tracker's keys
 * start -- or from low enough for them all to fit, in a set of more than 68.
 * drumkit_write_map writes exactly that mapping out as a kit.txt to start
 * from.
 *
 * Samples are stereo, and resampled on the fly to the engine rate, which
 * matters because sample libraries are mostly 44.1 kHz while the engines here
 * run at 48 kHz.
 *
 * Unlike the other three engines nothing here is reverse engineered: it plays
 * whatever WAVs you point it at. */
#ifndef DRUMKIT_H
#define DRUMKIT_H

#include <stddef.h>

#define DK_MAX_SAMPLES  128
#define DK_MAX_VOICES   32
#define DK_BASE_NOTE    60     /* C-4 */
#define DK_NAME_MAX     64
#define DK_MAP_FILE     "kit.txt"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drumkit drumkit;

/* ---------------------------------------------------- a set, to edit -- */

/* What a kit.txt says, or would: one pad per sample. Read from a folder,
 * changed by an editor, saved back as its kit.txt, and loaded to play. A
 * file is named from the set's folder, or by a path of its own. */
typedef struct {
    int    note;                     /* 0..127 */
    char   file[512];
    double gain_db;                  /* -60..24 */
    int    choke;                    /* 0: none, 1..99 */
} dk_pad;

typedef struct {
    char   dir[512];
    int    mapped;                   /* read from a kit.txt, not just the folder */
    int    n;
    dk_pad pad[DK_MAX_SAMPLES];
} dk_map;

/* The set in `dir` as its kit.txt says, or every .wav by name from C-4 up.
 * A kit.txt that names no pad -- empty, or every line rejected -- falls back
 * to the folder scan as if it were not there. Returns how many pads. */
int         drumkit_map_read(dk_map *m, const char *dir);
/* NULL when it can be saved, or what is wrong with it: two pads on a note. */
const char *drumkit_map_check(const dk_map *m);
/* As dir/kit.txt, replacing what is there. -1 with errno set on failure. */
int         drumkit_map_save(const dk_map *m);
drumkit    *drumkit_load_map(const dk_map *m, double samplerate);
int         drumkit_note_parse(const char *s);           /* "C-4" or 60 ("C--1" is 0); -1 */
void        drumkit_note_name(int note, char *buf);      /* buf >= 5 */

/* Loads the kit in `dir`: as its kit.txt says, or every .wav sorted by name.
 * A line of kit.txt that cannot be read, or names a file that will not load,
 * is reported on stderr and skipped; the rest of the kit still loads. A
 * kit.txt that yields no pad at all falls back to the folder scan.
 * Returns NULL if no sample loaded. */
drumkit *drumkit_load(const char *dir, double samplerate);
void     drumkit_free(drumkit *k);

int         drumkit_count(const drumkit *k);
const char *drumkit_sample_name(const drumkit *k, int i);
int         drumkit_note_of(const drumkit *k, int i);   /* MIDI note for slot i */
int         drumkit_choke_of(const drumkit *k, int i);  /* 0: none */
double      drumkit_gain_db_of(const drumkit *k, int i);
int         drumkit_mapped(const drumkit *k);           /* loaded from kit.txt */

/* Writes the kit as it is loaded to dir/kit.txt. Refuses, returning -1 with
 * errno EEXIST, rather than replace one that is there. A failed write leaves
 * no file behind. */
int drumkit_write_map(const drumkit *k, const char *dir);

void drumkit_note_on(drumkit *k, int note, int velocity);
void drumkit_note_off(drumkit *k, int note);            /* one-shots ignore this */

/* One sample by its slot, as a tracker plays it: `rate` 1 as recorded, 2 an
 * octave up. A hit cuts -- with a short fade -- whatever is still sounding in
 * the same `group` (0 up: a track, say), and the pads in its choke group.
 * drumkit_release fades out a group: a note-off. */
void drumkit_play(drumkit *k, int slot, double rate, int velocity, int group);
void drumkit_release(drumkit *k, int group);
int  drumkit_find_sample(const drumkit *k, const char *name);   /* slot, or -1 */
int  drumkit_slot_at(const drumkit *k, int note);                /* slot, or -1 */
void drumkit_all_off(drumkit *k);
void drumkit_fade_all(drumkit *k);   /* every voice out over the short fade */
void drumkit_render(drumkit *k, double *out, int frames);   /* interleaved stereo */

void drumkit_set_gain(drumkit *k, double g);

/* Leave the output unlimited. By default drumkit_render ends in a per-sample
 * soft limiter, which bends the waveform of every hit that peaks above 0.7
 * and, on stacked hits, adds audible distortion. A host that mixes several
 * kits and limits the sum itself (the tracker does, with a look-ahead
 * limiter) turns it off. */
void drumkit_set_raw(drumkit *k, int raw);

/* ------------------------------------------------------- finding kits -- */

#define DK_MAX_KITS 64

typedef struct {
    char path[512];
    char name[96];       /* the folder's name -- "pack / folder" one level
                            down -- for a menu */
} drumkit_place;

/* Every kit under the sample folders: those named in VA_KITS (colon
 * separated) first, then /storage01/synth_stuff/drums and
 * /storage01/synth_stuff/furnace. A kit is a folder holding .wav files -- a
 * root itself, a folder in it, or a folder one further down, since kits are
 * often nested. Sorted by name within each root. Returns how many, at most
 * `max`. Both windows list kits from here, so they offer the same ones. */
int drumkit_find(drumkit_place *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DRUMKIT_H */
