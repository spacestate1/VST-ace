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
        used = cheat_line(buf, used, n, width, "[ ]  octave");
        lines = 5;
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

int trk_paste(trk_engine *e, trk_editor *ed)
{
    trk_song *s = trk_song_of(e);
    int r, t, h, w, rows, top = ed->row, left = ed->track;
    if (!ed->edit || !clip.rows) return 0;
    trk_lock(e);
    rows = s->pattern[ed->pattern].rows;
    /* Cut off at the pattern's last row and the last track. */
    h = clip.rows < rows - top ? clip.rows : rows - top;
    w = clip.tracks < TRK_TRACKS - left ? clip.tracks : TRK_TRACKS - left;
    for (r = 0; r < h; r++)
        for (t = 0; t < w; t++)
            s->pattern[ed->pattern].cell[top + r][left + t] = clip.cell[r][t];
    trk_unlock(e);
    /* What went down is selected, so it can be seen; the cursor stays at
     * its top-left. */
    trk_select(ed, top, left, top + h - 1, left + w - 1);
    ed->row = top;
    ed->track = left;
    return h * w;
}

int trk_clipboard(int *rows, int *tracks)
{
    if (rows) *rows = clip.rows;
    if (tracks) *tracks = clip.tracks;
    return clip.rows * clip.tracks;
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
    int rows, preview = -1, pvel = 0;

    trk_lock(e);
    rows = rows_of(e, ed);
    if (ed->row >= rows) ed->row = rows - 1;
    /* Each track has its own octave: the note keys play the cursor's. */
    ed->octave = s->track[ed->track].octave;

    /* A plain move ends a selection; the Shift moves grow it from where the
     * cursor was. */
    if ((key >= TRK_K_UP && key <= TRK_K_BACKTAB) && key != TRK_K_DELETE &&
        key != TRK_K_BACKSPACE && key != TRK_K_INSERT)
        ed->sel = 0;
    if (key >= TRK_K_SEL_UP && key <= TRK_K_SEL_RIGHT && !ed->sel) {
        ed->sel = 1;
        ed->sel_r0 = ed->sel_r1 = ed->row;
        ed->sel_t0 = ed->sel_t1 = ed->track;
    }

    switch (key) {
    case TRK_K_SEL_UP:    if (ed->row > 0) ed->row--;              ed->sel_r1 = ed->row; break;
    case TRK_K_SEL_DOWN:  if (ed->row < rows - 1) ed->row++;       ed->sel_r1 = ed->row; break;
    case TRK_K_SEL_LEFT:  if (ed->track > 0) ed->track--;          ed->sel_t1 = ed->track; break;
    case TRK_K_SEL_RIGHT: if (ed->track < TRK_TRACKS - 1) ed->track++; ed->sel_t1 = ed->track; break;
    case TRK_K_SEL_ALL:
        ed->sel = 1; ed->sel_r0 = 0; ed->sel_t0 = 0; ed->sel_r1 = rows - 1; ed->sel_t1 = TRK_TRACKS - 1;
        break;
    case TRK_K_COPY: case TRK_K_CUT: case TRK_K_PASTE:
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
            ed->track = (ed->track + TRK_TRACKS - 1) % TRK_TRACKS;
        }
        break;
    case TRK_K_RIGHT:
        ed->digit = 0;
        if (++ed->field >= TRK_FIELDS) {
            ed->field = 0;
            ed->track = (ed->track + 1) % TRK_TRACKS;
        }
        break;
    case TRK_K_TAB:
        ed->track = (ed->track + 1) % TRK_TRACKS;              ed->field = 0; break;
    case TRK_K_BACKTAB:
        ed->track = (ed->track + TRK_TRACKS - 1) % TRK_TRACKS; ed->field = 0; break;

    case TRK_K_EDIT: ed->edit = !ed->edit; ed->digit = 0; break;

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
        if (ed->field == TRK_F_NOTE) { c->note = TRK_EMPTY; c->vel = TRK_EMPTY; }
        else *field_ptr(c, ed->field) = TRK_EMPTY;
        advance(e, ed);
        break;
    }
    case TRK_K_INSERT: {
        trk_pattern *p = &s->pattern[ed->pattern];
        int r;
        if (!ed->edit) break;
        for (r = rows - 1; r > ed->row; r--) p->cell[r][ed->track] = p->cell[r - 1][ed->track];
        memset(&p->cell[ed->row][ed->track], TRK_EMPTY, sizeof(trk_cell));
        break;
    }
    case TRK_K_BACKSPACE: {
        trk_pattern *p = &s->pattern[ed->pattern];
        int r;
        if (!ed->edit) break;
        for (r = ed->row; r < rows - 1; r++) p->cell[r][ed->track] = p->cell[r + 1][ed->track];
        memset(&p->cell[rows - 1][ed->track], TRK_EMPTY, sizeof(trk_cell));
        break;
    }

    case TRK_K_OCT_DOWN: if (ed->octave > 0) s->track[ed->track].octave = --ed->octave; break;
    case TRK_K_OCT_UP:   if (ed->octave < 9) s->track[ed->track].octave = ++ed->octave; break;
    case TRK_K_PAT_PREV:
        if (ed->pattern > 0) ed->pattern--;
        if (ed->row >= rows_of(e, ed)) ed->row = rows_of(e, ed) - 1;
        break;
    case TRK_K_PAT_NEXT:
        if (ed->pattern < TRK_PATTERNS - 1) ed->pattern++;
        if (ed->row >= rows_of(e, ed)) ed->row = rows_of(e, ed) - 1;
        break;

    case TRK_K_PLAY_SONG: case TRK_K_PLAY_PATTERN:
    case TRK_K_STOP:      case TRK_K_TOGGLE:
        break;                                  /* below, unlocked */

    default: {
        trk_cell *c = &s->pattern[ed->pattern].cell[ed->row][ed->track];
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
                c->note = TRK_NOTE_OFF;
                c->vel = TRK_EMPTY;
                advance(e, ed);
            } else if (n >= 0) {
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
            trk_preview_off(e, t);
            ed->held[t] = 0;
        }
}
