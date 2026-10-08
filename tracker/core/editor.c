/* What each key does in the pattern editor. Both windows hand keys here, so
 * the bindings are written down once:
 *
 *   arrows            move; left/right step through a cell's fields
 *   Tab / Shift-Tab   next / previous track
 *   PgUp / PgDn       16 rows; Home / End the first and last row
 *   note keys         zsxdcvgbhnjm one octave, q2w3er5t6y7u i9o0p the next
 *   1                 note-off
 *   `                 edit mode on / off: off, note keys only play, and
 *                     nothing is written or moved -- for trying keys out
 *   Delete or .       clear the field and advance
 *   Insert            push the rest of the track down a row
 *   Backspace         pull the rest of the track up over this row
 *   0-9 a-f           hex, in the velocity and controller fields
 *
 * The transport, octave and pattern keys arrive as their own codes; which
 * physical keys produce them is the window's business.
 */
#include "trk.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* engine.c's, the undo stack living with the engine: a snapshot of the song
 * as it is, taken under the lock just before an edit (trk_undo_push), and
 * the latest one put back, the caller holding the lock (trk_undo_locked).
 * Not in trk.h -- the windows get trk_undo and TRK_K_UNDO instead. */
void trk_undo_push(trk_engine *e);
int  trk_undo_locked(trk_engine *e);
int  trk_redo_locked(trk_engine *e);
/* engine.c's recording, called with the lock held: a note from a key goes to
 * the take instead of the cell under the cursor. */
int  trk_recording_locked(trk_engine *e);
void trk_record_note_locked(trk_engine *e, int track, int note, int vel, int on);
void trk_record_stopnote_locked(trk_engine *e, int track);

void trk_editor_init(trk_editor *ed)
{
    memset(ed, 0, sizeof *ed);
    ed->octave = 4;
    ed->step = 1;
    ed->follow = 1;
    ed->edit = 1;
}

int trk_key_note(int key, int octave)
{
    static const char lo[] = "zsxdcvgbhnjm";
    static const char hi[] = "q2w3er5t6y7ui9o0p";
    const char *p;
    int idx = -1, n;

    if (key <= 0 || key > 0x7f) return -1;
    key = tolower(key);
    if ((p = strchr(lo, key)) && *p)      idx = (int)(p - lo);
    else if ((p = strchr(hi, key)) && *p) idx = 12 + (int)(p - hi);
    if (idx < 0) return -1;
    n = (octave + 1) * 12 + idx;
    return n >= 12 && n <= 127 ? n : -1;
}

/* Move every note of one track by `semis`, so that changing the track's
 * octave does not leave what was typed sounding where it no longer reads.
 * Every pattern is walked, as the save loop walks them, not just the ones
 * the order lists. Note-offs, empty cells and velocity/controller-only
 * rows stay; the shift clamps where entry does, C-0 (12) to G-9 (127).
 * Returns the notes moved. The caller holds the lock. */
static int shift_track_notes(trk_song *s, int track, int semis)
{
    int p, r, n, moved = 0;
    for (p = 0; p < TRK_PATTERNS; p++)
        for (r = 0; r < s->pattern[p].rows; r++) {
            trk_cell *c = &s->pattern[p].cell[r][track];
            if (c->note > 127) continue;   /* TRK_NOTE_OFF or TRK_EMPTY */
            n = c->note + semis;
            c->note = (uint8_t)(n < 12 ? 12 : n > 127 ? 127 : n);
            moved++;
        }
    return moved;
}

int trk_track_set_octave(trk_engine *e, int track, int octave)
{
    trk_song *s;
    int delta, moved = 0;
    if (track < 0 || track >= TRK_TRACKS) return -1;
    if (octave < 0) octave = 0;
    if (octave > 9) octave = 9;
    s = trk_song_of(e);
    trk_lock(e);
    if (s->track[track].samples[0]) {      /* a sample set: the note is the pad, the octave is fixed */
        trk_unlock(e);
        return 0;
    }
    delta = (octave - s->track[track].octave) * 12;
    if (delta) {
        trk_undo_push(e);
        moved = shift_track_notes(s, track, delta);
        s->track[track].octave = octave;
    }
    trk_unlock(e);
    return moved;
}

void trk_note_rgb(int note, int dark, unsigned char rgb[3])
{
    /* Low to high across the rainbow: C1 red, through yellow, green, cyan and
     * blue, to violet at C7; notes beyond that keep the end colours. */
    double h, sat = 0.80, val = dark ? 0.95 : 0.72, f, p, q, t, r, g, b;
    int i;
    if (note < 24) note = 24;
    if (note > 96) note = 96;
    h = (note - 24) / 72.0 * 270.0 / 60.0;          /* 0..4.5 of the six 60-degree sectors */
    i = (int)h;
    f = h - i;
    p = val * (1.0 - sat);
    q = val * (1.0 - sat * f);
    t = val * (1.0 - sat * (1.0 - f));
    switch (i) {
    case 0:  r = val; g = t;   b = p;   break;
    case 1:  r = q;   g = val; b = p;   break;
    case 2:  r = p;   g = val; b = t;   break;
    case 3:  r = p;   g = q;   b = val; break;
    default: r = t;   g = p;   b = val; break;
    }
    rgb[0] = (unsigned char)(r * 255.0 + 0.5);
    rgb[1] = (unsigned char)(g * 255.0 + 0.5);
    rgb[2] = (unsigned char)(b * 255.0 + 0.5);
}

int trk_track_fit_octave(trk_engine *e, int track)
{
    unsigned char mask[16];
    int n, oct;
    if (track < 0 || track >= TRK_TRACKS) return -1;
    if (!trk_sample_mask(e, track, mask)) return -1;      /* no loaded set */
    for (n = 0; n < 128; n++) if (mask[n / 8] & (1 << (n % 8))) break;
    if (n == 128) return -1;
    oct = n / 12 - 1;
    if (oct < 0) oct = 0;
    if (oct > 9) oct = 9;
    trk_lock(e);
    trk_song_of(e)->track[track].octave = oct;
    trk_unlock(e);
    return oct;
}

const char *trk_columns_help(void)
{
    return
        "THE ROW NUMBER (left edge)\n"
        "\n"
        "  Shows     What it means\n"
        "  --------  --------------------------------------------------------\n"
        "  00 01 ..  The row, in hex. Rows on the beat and on the bar are\n"
        "            shaded.\n"
        "\n"
        "EACH TRACK'S HEADER (above its column)\n"
        "\n"
        "  Box       What it changes\n"
        "  --------  --------------------------------------------------------\n"
        "  name      What the track is called. Saved with the song.\n"
        "  window    Which program or synth tab plays this track.\n"
        "  samples   A sample set the tracker plays itself, instead of a\n"
        "            window.\n"
        "  ch        The MIDI channel the track sends on, 1 to 16.\n"
        "  oct       The octave the note keys type at. Changing it moves the\n"
        "            track's notes too. A sample set sets it to where its\n"
        "            first sample is.\n"
        "  mute      Silences the track. Its notes stay.\n"
        "  dot       Green: connected. Red: that window or sample set was not\n"
        "            found.\n"
        "\n"
        "EACH CELL (four columns per track, e.g.  C-4 64 07 7F)\n"
        "\n"
        "  Column  Example  What it changes                    Values\n"
        "  ------  -------  ---------------------------------  --------------\n"
        "  Note    C-4      The note played. It ends the note  C-0 to G-9\n"
        "                   before it. For a sample set it\n"
        "                   picks the sample.\n"
        "          ===      Stops the note that is sounding\n"
        "                   (key 1).\n"
        "  Vel     64       How hard the note is played (its   01 to 7F (hex)\n"
        "                   volume). Empty: the track's own\n"
        "                   velocity.\n"
        "  CC      07       Which MIDI controller to move: 01  00 to 7F (hex)\n"
        "                   mod wheel, 07 volume, 0A pan, 40\n"
        "                   sustain.\n"
        "  Val     7F       The new value for that             00 to 7F (hex)\n"
        "                   controller.\n"
        "\n"
        "  A controller is sent only when both CC and Val are filled in. It needs no\n"
        "  note on the row. Sample sets ignore controllers. Empty columns show dots.";
}

/* One line into buf, cut to width characters -- counting a UTF-8 sequence
 * as one, since sample names are not all ASCII. */
static size_t cheat_line(char *buf, size_t used, size_t n, int width, const char *line)
{
    int chars = 0;
    const unsigned char *p = (const unsigned char *)line;
    while (*p && used + 2 < n) {
        size_t len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        size_t k;
        if (chars == width) break;
        for (k = 0; k < len && p[k] && used + 2 < n; k++) buf[used++] = (char)p[k];
        p += k;
        chars++;
    }
    if (used + 1 < n) buf[used++] = '\n';
    if (n) buf[used < n ? used : n - 1] = 0;
    return used;
}

int trk_cheat_sheet(trk_engine *e, int t, int octave, int width, char *buf, size_t n)
{
    static const char keys[] = "zsxdcvgbhnjmq2w3er5t6y7ui9o0p";
    static char pads[16384];
    char line[256], nn[4];
    size_t used = 0;
    int count = 0, lines = 0, i;

    if (n) buf[0] = 0;
    trk_lock(e);
    i = trk_song_of(e)->track[t].samples[0] != 0;
    if (octave < 0) octave = trk_song_of(e)->track[t].octave;
    trk_unlock(e);
    if (i) {
        char *p, *save = NULL;
        count = trk_list_pads(e, t, pads, sizeof pads);
        if (!count) return (int)(cheat_line(buf, used, n, width, "set not loaded") ? 1 : 0);
        for (p = strtok_r(pads, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
            char *tab = strchr(p, '\t');
            int note = atoi(p), key = ' ';
            const char *k;
            for (k = keys; *k; k++) if (trk_key_note(*k, octave) == note) { key = *k; break; }
            snprintf(line, sizeof line, "%c %s %s", key, trk_note_name(note, nn), tab ? tab + 1 : "");
            used = cheat_line(buf, used, n, width, line);
            lines++;
        }
        return lines;
    }
    {   /* The note keys at this octave, and the two that are not notes. */
        char lo[4], hi[4];
        snprintf(line, sizeof line, "zsxdcvgbhnjm %s..%s",
                 trk_note_name(trk_key_note('z', octave), lo), trk_note_name(trk_key_note('m', octave), hi));
        used = cheat_line(buf, used, n, width, line);
        snprintf(line, sizeof line, "q2w3er5t6y7u %s..%s",
                 trk_note_name(trk_key_note('q', octave), lo), trk_note_name(trk_key_note('u', octave), hi));
        used = cheat_line(buf, used, n, width, line);
        used = cheat_line(buf, used, n, width, "1  note-off (===)");
        used = cheat_line(buf, used, n, width, ".  clear, advance");
        used = cheat_line(buf, used, n, width, "[ ]  octave (notes move too)");
        used = cheat_line(buf, used, n, width, "space edit   ctrl+z/y undo");
        lines = 6;
    }
    (void)used;
    return lines;
}

/* ------------------------------------------------------------ arranging */

int trk_order_insert(trk_engine *e, int at, int p)
{
    trk_song *s = trk_song_of(e);
    int r = -1;
    if (p < 0 || p >= TRK_PATTERNS) return -1;
    trk_lock(e);
    if (s->norder < TRK_ORDER_MAX) {
        if (at < -1) at = -1;
        if (at >= s->norder) at = s->norder - 1;
        trk_undo_push(e);
        memmove(&s->order[at + 2], &s->order[at + 1], sizeof s->order[0] * (size_t)(s->norder - at - 1));
        s->order[at + 1] = p;
        s->norder++;
        r = at + 1;
    }
    trk_unlock(e);
    return r;
}

int trk_order_remove(trk_engine *e, int at)
{
    trk_song *s = trk_song_of(e);
    int r = -1;
    trk_lock(e);
    if (at >= 0 && at < s->norder && s->norder > 1) {
        trk_undo_push(e);
        memmove(&s->order[at], &s->order[at + 1], sizeof s->order[0] * (size_t)(s->norder - at - 1));
        s->norder--;
        r = at < s->norder ? at : s->norder - 1;
    }
    trk_unlock(e);
    return r;
}

int trk_order_move(trk_engine *e, int at, int by)
{
    trk_song *s = trk_song_of(e);
    int r = -1, to = at + by, v;
    trk_lock(e);
    if (at >= 0 && at < s->norder && to >= 0 && to < s->norder) {
        trk_undo_push(e);
        v = s->order[at];
        if (to > at) memmove(&s->order[at], &s->order[at + 1], sizeof s->order[0] * (size_t)(to - at));
        else         memmove(&s->order[to + 1], &s->order[to], sizeof s->order[0] * (size_t)(at - to));
        s->order[to] = v;
        r = to;
    }
    trk_unlock(e);
    return r;
}

int trk_pattern_new(trk_engine *e, int from)
{
    trk_song *s = trk_song_of(e);
    int p, i, r = -1;
    trk_lock(e);
    for (p = 0; p < TRK_PATTERNS && r < 0; p++) {
        int listed = 0;
        for (i = 0; i < s->norder; i++) if (s->order[i] == p) listed = 1;
        if (listed || p == from || trk_pattern_used(s, p) || s->pattern[p].name[0] ||
            s->pattern[p].rows != 64) continue;
        r = p;
    }
    if (r >= 0 && from >= 0 && from < TRK_PATTERNS) {
        char base[TRK_NAME_LEN + 8];
        trk_undo_push(e);
        s->pattern[r] = s->pattern[from];
        trk_part_label(s, from, base);
        snprintf(s->pattern[r].name, sizeof s->pattern[r].name, "%.*s 2",
                 (int)sizeof s->pattern[r].name - 3, base);
    }
    trk_unlock(e);
    return r;
}

/* ----------------------------------------------------------- selecting */

static struct {
    int      rows, tracks;
    trk_cell cell[TRK_ROWS_MAX][TRK_TRACKS];
} clip;

void trk_select(trk_editor *ed, int r0, int t0, int r1, int t1)
{
    ed->sel = 1;
    ed->sel_r0 = r0; ed->sel_t0 = t0;
    ed->sel_r1 = r1; ed->sel_t1 = t1;
    ed->row = r1; ed->track = t1;
}

void trk_select_none(trk_editor *ed) { ed->sel = 0; }

int trk_selection(const trk_editor *ed, int *r0, int *t0, int *r1, int *t1)
{
    if (!ed->sel) return 0;
    *r0 = ed->sel_r0 < ed->sel_r1 ? ed->sel_r0 : ed->sel_r1;
    *r1 = ed->sel_r0 < ed->sel_r1 ? ed->sel_r1 : ed->sel_r0;
    *t0 = ed->sel_t0 < ed->sel_t1 ? ed->sel_t0 : ed->sel_t1;
    *t1 = ed->sel_t0 < ed->sel_t1 ? ed->sel_t1 : ed->sel_t0;
    return 1;
}

int trk_selected(const trk_editor *ed, int r, int t)
{
    int r0, t0, r1, t1;
    return trk_selection(ed, &r0, &t0, &r1, &t1) && r >= r0 && r <= r1 && t >= t0 && t <= t1;
}

/* The block to work on: what is selected, or the cursor's cell, kept inside
 * the pattern as it is now. */
static void block(trk_engine *e, const trk_editor *ed, int *r0, int *t0, int *r1, int *t1)
{
    int rows = trk_song_of(e)->pattern[ed->pattern].rows;
    if (!trk_selection(ed, r0, t0, r1, t1)) { *r0 = *r1 = ed->row; *t0 = *t1 = ed->track; }
    if (*r1 >= rows) *r1 = rows - 1;
    if (*r0 > *r1) *r0 = *r1;
}

int trk_copy(trk_engine *e, trk_editor *ed)
{
    const trk_song *s = trk_song_of(e);
    int r0, t0, r1, t1, r, t;
    trk_lock(e);
    block(e, ed, &r0, &t0, &r1, &t1);
    clip.rows = r1 - r0 + 1;
    clip.tracks = t1 - t0 + 1;
    for (r = r0; r <= r1; r++)
        for (t = t0; t <= t1; t++) clip.cell[r - r0][t - t0] = s->pattern[ed->pattern].cell[r][t];
    trk_unlock(e);
    return clip.rows * clip.tracks;
}

int trk_clear_block(trk_engine *e, trk_editor *ed)
{
    trk_song *s = trk_song_of(e);
    int r0, t0, r1, t1, r;
    if (!ed->edit) return 0;
    trk_lock(e);
    block(e, ed, &r0, &t0, &r1, &t1);
    trk_undo_push(e);
    for (r = r0; r <= r1; r++)
        memset(&s->pattern[ed->pattern].cell[r][t0], TRK_EMPTY, sizeof(trk_cell) * (size_t)(t1 - t0 + 1));
    trk_unlock(e);
    return (r1 - r0 + 1) * (t1 - t0 + 1);
}

int trk_cut(trk_engine *e, trk_editor *ed)
{
    if (!ed->edit) return 0;
    trk_copy(e, ed);
    return trk_clear_block(e, ed);
}

/* Paste: all of the clipboard, or -- mixed -- only the fields it has, over
 * what is there. */
static int paste(trk_engine *e, trk_editor *ed, int mix)
{
    trk_song *s = trk_song_of(e);
    int r, t, h, w, rows, top = ed->row, left = ed->track;
    if (!ed->edit || !clip.rows) return 0;
    trk_lock(e);
    rows = s->pattern[ed->pattern].rows;
    /* Cut off at the pattern's last row and the last track. */
    h = clip.rows < rows - top ? clip.rows : rows - top;
    w = clip.tracks < s->ntracks - left ? clip.tracks : s->ntracks - left;
    trk_undo_push(e);
    for (r = 0; r < h; r++)
        for (t = 0; t < w; t++) {
            trk_cell *d = &s->pattern[ed->pattern].cell[top + r][left + t];
            const trk_cell *c = &clip.cell[r][t];
            if (!mix) { *d = *c; continue; }
            if (c->note != TRK_EMPTY) d->note = c->note;
            if (c->vel != TRK_EMPTY)  d->vel = c->vel;
            if (c->cc != TRK_EMPTY)   d->cc = c->cc;
            if (c->val != TRK_EMPTY)  d->val = c->val;
        }
    trk_unlock(e);
    /* What went down is selected, so it can be seen; the cursor stays at
     * its top-left. */
    trk_select(ed, top, left, top + h - 1, left + w - 1);
    ed->row = top;
    ed->track = left;
    return h * w;
}

int trk_paste(trk_engine *e, trk_editor *ed) { return paste(e, ed, 0); }

int trk_clipboard(int *rows, int *tracks)
{
    if (rows) *rows = clip.rows;
    if (tracks) *tracks = clip.tracks;
    return clip.rows * clip.tracks;
}

size_t trk_clipboard_text(char *buf, size_t n)
{
    size_t used = 0;
    int r, t;
    if (n) buf[0] = 0;
    for (r = 0; r < clip.rows; r++) {
        for (t = 0; t < clip.tracks; t++) {
            char name[4] = "-";
            if (used + 8 >= n) return used;
            if (t) { memcpy(buf + used, " | ", 3); used += 3; }
            /* only the note, a dash when empty; no dots, vel or controller */
            if (clip.cell[r][t].note != TRK_EMPTY) trk_note_name(clip.cell[r][t].note, name);
            memcpy(buf + used, name, strlen(name));
            used += strlen(name);
        }
        buf[used++] = '\n';
        buf[used] = 0;
    }
    return used;
}

static int hexval(int key)
{
    if (key >= '0' && key <= '9') return key - '0';
    key = tolower(key);
    if (key >= 'a' && key <= 'f') return key - 'a' + 10;
    return -1;
}

static int rows_of(trk_engine *e, const trk_editor *ed)
{
    return trk_song_of(e)->pattern[ed->pattern].rows;
}

static void advance(trk_engine *e, trk_editor *ed)
{
    int rows = rows_of(e, ed);
    ed->row = (ed->row + ed->step) % rows;
    ed->digit = 0;
}

static uint8_t *field_ptr(trk_cell *c, int field)
{
    switch (field) {
    case TRK_F_VEL: return &c->vel;
    case TRK_F_CC:  return &c->cc;
    case TRK_F_VAL: return &c->val;
    default:        return &c->note;
    }
}

int trk_key(trk_engine *e, trk_editor *ed, int key)
{
    trk_song *s = trk_song_of(e);
    int rows, preview = -1, pvel = 0, nt;

    trk_lock(e);
    nt = s->ntracks < 1 ? 1 : s->ntracks > TRK_TRACKS ? TRK_TRACKS : s->ntracks;
    if (ed->track >= nt) ed->track = nt - 1;       /* tracks were taken away under the cursor */
    rows = rows_of(e, ed);
    if (ed->row >= rows) ed->row = rows - 1;
    /* Each track has its own octave: the note keys play the cursor's. */
    ed->octave = s->track[ed->track].octave;

    /* A plain move ends a selection; the Shift moves grow it from where the
     * cursor was. */
    if ((key >= TRK_K_UP && key <= TRK_K_BACKTAB) && key != TRK_K_DELETE &&
        key != TRK_K_BACKSPACE && key != TRK_K_INSERT)
        ed->sel = 0;
    if (((key >= TRK_K_SEL_UP && key <= TRK_K_SEL_RIGHT) || key == TRK_K_SEL_PGUP || key == TRK_K_SEL_PGDN) &&
        !ed->sel) {
        ed->sel = 1;
        ed->sel_r0 = ed->sel_r1 = ed->row;
        ed->sel_t0 = ed->sel_t1 = ed->track;
    }

    switch (key) {
    case TRK_K_SEL_UP:    if (ed->row > 0) ed->row--;              ed->sel_r1 = ed->row; break;
    case TRK_K_SEL_DOWN:  if (ed->row < rows - 1) ed->row++;       ed->sel_r1 = ed->row; break;
    case TRK_K_SEL_LEFT:  if (ed->track > 0) ed->track--;          ed->sel_t1 = ed->track; break;
    case TRK_K_SEL_RIGHT: if (ed->track < nt - 1) ed->track++; ed->sel_t1 = ed->track; break;
    case TRK_K_SEL_PGUP:  ed->row = ed->row >= 16 ? ed->row - 16 : 0;           ed->sel_r1 = ed->row; break;
    case TRK_K_SEL_PGDN:  ed->row = ed->row + 16 < rows ? ed->row + 16 : rows - 1; ed->sel_r1 = ed->row; break;
    case TRK_K_SEL_ALL:
        ed->sel = 1; ed->sel_r0 = 0; ed->sel_t0 = 0; ed->sel_r1 = rows - 1; ed->sel_t1 = nt - 1;
        break;
    case TRK_K_COPY: case TRK_K_CUT: case TRK_K_PASTE: case TRK_K_PASTE_MIX:
        break;                                  /* below, unlocked */
    case TRK_K_UP:    ed->row = (ed->row + rows - 1) % rows; ed->digit = 0; break;
    case TRK_K_DOWN:  ed->row = (ed->row + 1) % rows;        ed->digit = 0; break;
    case TRK_K_PGUP:  ed->row = ed->row >= 16 ? ed->row - 16 : 0;           break;
    case TRK_K_PGDN:  ed->row = ed->row + 16 < rows ? ed->row + 16 : rows - 1; break;
    case TRK_K_HOME:  ed->row = 0;        break;
    case TRK_K_END:   ed->row = rows - 1; break;
    case TRK_K_LEFT:
        ed->digit = 0;
        if (--ed->field < 0) {
            ed->field = TRK_FIELDS - 1;
            ed->track = (ed->track + nt - 1) % nt;
        }
        break;
    case TRK_K_RIGHT:
        ed->digit = 0;
        if (++ed->field >= TRK_FIELDS) {
            ed->field = 0;
            ed->track = (ed->track + 1) % nt;
        }
        break;
    case TRK_K_TAB:
        ed->track = (ed->track + 1) % nt;                      ed->field = 0; break;
    case TRK_K_BACKTAB:
        ed->track = (ed->track + nt - 1) % nt;                 ed->field = 0; break;

    case TRK_K_EDIT: ed->edit = !ed->edit; ed->digit = 0; break;

    case TRK_K_REDO:
        if (!trk_redo_locked(e)) { trk_unlock(e); return 0; }
        ed->sel = 0;
        ed->digit = 0;
        rows = rows_of(e, ed);
        if (ed->row >= rows) ed->row = rows - 1;
        break;

    /* Transpose: the selection, or the cell under the cursor; edit mode
     * only, as every change to the pattern is. Notes go no lower than C-0
     * nor higher than G-9, where entry stops; note-offs and empty cells
     * stay. */
    case TRK_K_TRANSPOSE_DOWN: case TRK_K_TRANSPOSE_UP:
    case TRK_K_TRANSPOSE_OCT_DOWN: case TRK_K_TRANSPOSE_OCT_UP: {
        const int by = key == TRK_K_TRANSPOSE_DOWN ? -1 : key == TRK_K_TRANSPOSE_UP ? 1
                     : key == TRK_K_TRANSPOSE_OCT_DOWN ? -12 : 12;
        int r0, t0, r1, t1, r, t, any = 0;
        if (!ed->edit) break;
        block(e, ed, &r0, &t0, &r1, &t1);
        for (r = r0; r <= r1 && !any; r++)
            for (t = t0; t <= t1; t++)
                if (s->pattern[ed->pattern].cell[r][t].note <= 127) { any = 1; break; }
        if (!any) break;
        trk_undo_push(e);
        for (r = r0; r <= r1; r++)
            for (t = t0; t <= t1; t++) {
                trk_cell *c = &s->pattern[ed->pattern].cell[r][t];
                int n;
                if (c->note > 127) continue;
                n = c->note + by;
                c->note = (uint8_t)(n < 12 ? 12 : n > 127 ? 127 : n);
            }
        break;
    }

    /* Mute and solo the cursor's track. The track header's mute box is the
     * same state; the windows re-read it after the key. Not edits: no undo. */
    case TRK_K_MUTE_TRACK:
        s->track[ed->track].mute = !s->track[ed->track].mute;
        break;
    case TRK_K_SOLO_TRACK: {
        int t, others_silent = 1;
        for (t = 0; t < nt; t++)
            if (t != ed->track && !s->track[t].mute) others_silent = 0;
        /* Soloing the one already alone brings everyone back. */
        if (others_silent && !s->track[ed->track].mute)
            for (t = 0; t < nt; t++) s->track[t].mute = 0;
        else
            for (t = 0; t < nt; t++) s->track[t].mute = t != ed->track;
        break;
    }
    case TRK_K_UNMUTE_ALL: {
        int t;
        for (t = 0; t < TRK_TRACKS; t++) s->track[t].mute = 0;
        break;
    }

    case TRK_K_STEP_UP:   if (ed->step < 16) ed->step++; break;
    case TRK_K_STEP_DOWN: if (ed->step > 0)  ed->step--; break;

    case TRK_K_UNDO:
        if (!trk_undo_locked(e)) { trk_unlock(e); return 0; }
        /* The restore may have changed the pattern under the cursor. */
        ed->sel = 0;
        ed->digit = 0;
        rows = rows_of(e, ed);
        if (ed->row >= rows) ed->row = rows - 1;
        break;

    case TRK_K_DELETE:
    case '.': {
        if (!ed->edit) break;
        if (ed->sel && key == TRK_K_DELETE) {   /* a selection: all of it */
            trk_unlock(e);
            trk_clear_block(e, ed);
            trk_lock(e);
            break;
        }
        trk_cell *c = &s->pattern[ed->pattern].cell[ed->row][ed->track];
        /* Clearing a note takes its velocity with it -- a velocity alone on
         * a row does nothing, and would only look like it did. */
        if (ed->field == TRK_F_NOTE) {
            if (c->note != TRK_EMPTY || c->vel != TRK_EMPTY) trk_undo_push(e);
            c->note = TRK_EMPTY; c->vel = TRK_EMPTY;
        } else {
            if (*field_ptr(c, ed->field) != TRK_EMPTY) trk_undo_push(e);
            *field_ptr(c, ed->field) = TRK_EMPTY;
        }
        advance(e, ed);
        break;
    }
    case TRK_K_INSERT: {
        trk_pattern *p = &s->pattern[ed->pattern];
        int r;
        if (!ed->edit) break;
        trk_undo_push(e);
        for (r = rows - 1; r > ed->row; r--) p->cell[r][ed->track] = p->cell[r - 1][ed->track];
        memset(&p->cell[ed->row][ed->track], TRK_EMPTY, sizeof(trk_cell));
        break;
    }
    case TRK_K_BACKSPACE: {
        trk_pattern *p = &s->pattern[ed->pattern];
        int r;
        if (!ed->edit) break;
        trk_undo_push(e);
        for (r = ed->row; r < rows - 1; r++) p->cell[r][ed->track] = p->cell[r + 1][ed->track];
        memset(&p->cell[rows - 1][ed->track], TRK_EMPTY, sizeof(trk_cell));
        break;
    }

    case TRK_K_OCT_DOWN:
        if (s->track[ed->track].samples[0]) break;     /* a sample set: fixed */
        if (ed->octave > 0) {
            trk_undo_push(e);
            s->track[ed->track].octave = --ed->octave;
            shift_track_notes(s, ed->track, -12);
        }
        break;
    case TRK_K_OCT_UP:
        if (s->track[ed->track].samples[0]) break;
        if (ed->octave < 9) {
            trk_undo_push(e);
            s->track[ed->track].octave = ++ed->octave;
            shift_track_notes(s, ed->track, +12);
        }
        break;
    case TRK_K_PAT_PREV:
        if (ed->pattern > 0) ed->pattern--;
        if (ed->row >= rows_of(e, ed)) ed->row = rows_of(e, ed) - 1;
        break;
    case TRK_K_PAT_NEXT:
        if (ed->pattern < TRK_PATTERNS - 1) ed->pattern++;
        if (ed->row >= rows_of(e, ed)) ed->row = rows_of(e, ed) - 1;
        break;

    case TRK_K_PLAY_SONG: case TRK_K_PLAY_PATTERN: case TRK_K_PLAY_FROM_CURSOR:
    case TRK_K_STOP:      case TRK_K_TOGGLE:    case TRK_K_RECORD:
        break;                                  /* below, unlocked */

    default: {
        trk_cell *c = &s->pattern[ed->pattern].cell[ed->row][ed->track];
        if (trk_recording_locked(e)) {
            /* Recording: the key is played in, at its time, on the cursor's
             * track -- not written at the cursor, and the cursor stays where
             * the playing is taking it. */
            int n = trk_key_note(key, ed->octave);
            if (key == '1') { trk_record_stopnote_locked(e, ed->track); break; }
            if (n < 0) { trk_unlock(e); return 0; }
            trk_record_note_locked(e, ed->track, n, -1, 1);
            preview = n;
            pvel = 0;
            break;
        }
        if (!ed->edit) {
            /* Trying keys out: a note key sounds, from whichever column the
             * cursor is in, and nothing is written or moved. */
            int n = trk_key_note(key, ed->octave);
            if (n < 0) { trk_unlock(e); return 0; }
            preview = n;
            pvel = c->vel != TRK_EMPTY ? c->vel : 0;
        } else if (ed->field == TRK_F_NOTE) {
            int n = trk_key_note(key, ed->octave);
            if (key == '1') {
                if (c->note != TRK_NOTE_OFF || c->vel != TRK_EMPTY) trk_undo_push(e);
                c->note = TRK_NOTE_OFF;
                c->vel = TRK_EMPTY;
                advance(e, ed);
            } else if (n >= 0) {
                if (c->note != (uint8_t)n) trk_undo_push(e);
                c->note = (uint8_t)n;
                preview = n;
                pvel = c->vel != TRK_EMPTY ? c->vel : 0;
                advance(e, ed);
            } else {
                trk_unlock(e);
                return 0;
            }
        } else {
            uint8_t *v = field_ptr(c, ed->field);
            int d = hexval(key), cur;
            if (d < 0 || !ed->edit) { trk_unlock(e); return 0; }
            cur = *v == TRK_EMPTY ? 0 : *v;
            if (ed->digit == 0) {
                /* One undo step per value: the snapshot goes with the first
                 * digit, the second writes into it. */
                trk_undo_push(e);
                /* MIDI data stops at 7F, so the high digit does too. */
                *v = (uint8_t)((d > 7 ? 7 : d) << 4 | (cur & 0x0F));
                ed->digit = 1;
            } else {
                *v = (uint8_t)((cur & 0xF0) | d);
                advance(e, ed);
            }
        }
        break;
    }
    }
    ed->octave = s->track[ed->track].octave;   /* the cursor may have changed track */
    trk_unlock(e);

    /* The engine calls take its lock themselves. */
    switch (key) {
    case TRK_K_PLAY_SONG: {
        int o = 0, i;
        trk_lock(e);
        /* From the part picked in the Parts list, when it is this pattern;
         * otherwise from the first place the pattern plays. */
        if (ed->part >= 0 && ed->part < s->norder && s->order[ed->part] == ed->pattern) o = ed->part;
        else for (i = 0; i < s->norder; i++) if (s->order[i] == ed->pattern) { o = i; break; }
        trk_unlock(e);
        trk_play(e, TRK_PLAY_SONG, o, 0);
        break;
    }
    case TRK_K_PLAY_PATTERN: trk_play(e, TRK_PLAY_PATTERN, ed->pattern, 0); break;
    case TRK_K_PLAY_FROM_CURSOR: trk_play(e, TRK_PLAY_PATTERN, ed->pattern, ed->row); break;
    case TRK_K_RECORD:
        /* A second press ends the take; otherwise it records into the pattern
         * under the cursor from the cursor's row, looping. */
        if (trk_recording(e)) trk_stop(e);
        else { trk_record_arm(e, ed->track); trk_record_start(e, TRK_PLAY_PATTERN, ed->pattern, ed->row); }
        break;
    case TRK_K_PASTE_MIX: paste(e, ed, 1); break;
    case TRK_K_COPY:  trk_copy(e, ed);  break;
    case TRK_K_CUT:   trk_cut(e, ed);   break;
    case TRK_K_PASTE: trk_paste(e, ed); break;
    case TRK_K_STOP:         trk_stop(e); break;
    case TRK_K_TOGGLE:
        if (trk_playing(e)) trk_stop(e);
        else                trk_play(e, TRK_PLAY_PATTERN, ed->pattern, 0);
        break;
    default: break;
    }
    if (preview >= 0) {
        /* Released by the key coming back up, not by the cursor: the step
         * has already moved it to another row. */
        int t = ed->track;
        if (ed->held[t]) trk_preview_off(e, t);
        trk_preview(e, t, preview, pvel);
        ed->held[t] = tolower(key);
        ed->held_note[t] = preview;
    }
    return 1;
}

void trk_key_release(trk_engine *e, trk_editor *ed, int key)
{
    int t;
    if (key <= 0 || key > 0x7f) return;
    key = tolower(key);
    for (t = 0; t < TRK_TRACKS; t++)
        if (ed->held[t] == key) {
            trk_record_note(e, t, ed->held_note[t], -1, 0);    /* the release, if a take is running */
            trk_preview_off(e, t);
            ed->held[t] = 0;
        }
}
