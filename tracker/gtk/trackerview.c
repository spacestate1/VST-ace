/* trackerview -- the GTK 4 tracker, whole, as one widget a host packs. The
 * same program as the Qt one: it draws the song and hands keys to core/,
 * where every decision is made. The standalone tracker-gtk is a window around
 * this (gtk/main.c); a shell embeds the widget directly -- see trackerview.h. */
#include "trackerview.h"

#include "trk.h"
#include "drumkit.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_keys_help =
    "Pattern\n"
    "  arrows            move (left/right step through a cell's fields)\n"
    "  Tab / Shift+Tab   next / previous track\n"
    "  PgUp / PgDn       16 rows      Home / End   first / last row\n"
    "  z s x d c v g b h n j m       notes, one octave\n"
    "  q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 p   the octave above\n"
    "  1                 note-off\n"
    "  Space (or `)      edit mode on / off (off: note keys only play,\n"
    "                    nothing is written)\n"
    "  Delete or .       clear and advance\n"
    "  Insert            push the track down a row\n"
    "  Backspace         pull the track up over this row\n"
    "  0-9 a-f           hex, in the velocity and controller fields\n"
    "  [ ] or keypad / * octave down / up (a track's notes move with it)\n"
    "  Ctrl+keypad / *   edit step down / up\n"
    "  - =               previous / next pattern\n"
    "\n"
    "Selection and clipboard\n"
    "  Shift+arrows      select      Shift+PgUp / PgDn   select a page\n"
    "  Ctrl+A            select all\n"
    "  Ctrl+C / X / V    copy / cut / paste\n"
    "  Ctrl+Shift+V      paste mix: only what the clipboard has, over what is there\n"
    "  Ctrl+Z / Ctrl+Y   undo / redo (Ctrl+Shift+Z too)\n"
    "  Ctrl+F1 / F2      transpose selection (or cell) down / up a semitone\n"
    "  Ctrl+F3 / F4      the same, an octave\n"
    "\n"
    "Tracks\n"
    "  Ctrl+Insert       add a track after the cursor's  (+ Track)\n"
    "  Ctrl+Delete       remove the cursor's track  (- Track; asks if it has notes)\n"
    "  Alt+F9            mute the cursor's track\n"
    "  Alt+F10           solo it (again: everyone back)\n"
    "  Alt+Shift+F9      unmute all\n"
    "\n"
    "Transport\n"
    "  Enter             play pattern / stop\n"
    "  Shift+Enter       play the pattern from the cursor row\n"
    "  F5  play song     F6  play pattern     F8  stop\n"
    "  F7                record: play from the cursor row and write what you\n"
    "                    play -- keys or a MIDI input -- on the cursor's track.\n"
    "                    A count-in and click lead it in; F7 or F8 ends the take.\n"
    "                    Rec... sets the count-in, click, quantizing and input.\n"
    "  Escape or F12     panic: release every note everywhere\n"
    "\n"
    "Each track plays one window. Open vst-ace once per instrument, then pick\n"
    "the window under the track's name. A cell is note, velocity, controller\n"
    "number and controller value; empty velocity uses the track's.";

static const int k_lpbs[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
#define NLPB ((int)(sizeof k_lpbs / sizeof k_lpbs[0]))
#define MAXDEST 128
#define MAXSAMP (DK_MAX_KITS + TRK_TRACKS + 2)  /* every set, "none", and one not found */

/* One tracker, whole: the engine it plays, the editor state, and every
 * widget. trk_view_new allocates it; trk_view_widget builds the widgets on it
 * as one widget a host packs -- standalone, tracker_window_new wraps the view
 * in a window; a shell packs it into a tab. Every function below takes the
 * instance as its first parameter. The public name is trk_view; `ui` is the
 * same struct under its historical short name.
 *
 * The sample-set editor's state (E, further down) is still one-per-process:
 * it is a modal dialog of the window that opened it and keeps a pointer back
 * to its instance. */
struct trk_view {
    trk_engine *e;
    trk_editor  ed;
    trk_song   *saved;               /* the song as last saved, for "unsaved changes?" */
    char        path[4096];
    int         loading;             /* widgets being set from the song: do not write back */
    int         closing;

    GtkApplication *app;
    GtkWidget  *win;                 /* the window around the view, standalone; NULL embedded */
    GtkWidget  *view;                /* the tracker, whole, as one widget */
    GSimpleActionGroup *ag;          /* the view's commands, under the "win" prefix */
    GtkWidget  *area, *scroll, *headscroll, *status;
    GtkWidget  *cheatwin, *cheattext;  /* Help > Cheat Sheet, while it is open */
    GtkWidget  *bpm, *lpb, *pattern, *rows, *step, *follow, *editbox, *volume;
    GtkWidget  *recbtn;              /* the Rec button, lit while a take runs */
    int         rec_shown;           /* what recbtn says: trk_recording's last answer */
    GtkWidget  *recwin;              /* the recording options, while open */
    GtkWidget  *parts, *part_name;   /* the Parts panel */
    int         part_at, part_playing, filling_parts;
    int         drag_rows, drag_r, drag_t;   /* a drag selecting rows, or a block, and where it began */
    GtkWidget  *name[TRK_TRACKS], *dest[TRK_TRACKS], *chan[TRK_TRACKS];
    GtkWidget  *mute[TRK_TRACKS], *state[TRK_TRACKS], *oct[TRK_TRACKS];
    GtkWidget  *headbox[TRK_TRACKS];
    char       *destval[TRK_TRACKS][MAXDEST];   /* "client\tport" per dropdown item */
    int         ndest[TRK_TRACKS];
    GtkWidget  *sample[TRK_TRACKS];
    char       *sampleval[TRK_TRACKS][MAXSAMP]; /* a sample's name per dropdown item */
    int         nsample[TRK_TRACKS];
    char        last_dests[16384];

    PangoFontDescription *font;
    int         cw, ch, asc;
    int         colw;                /* a track's width, grid and header alike */
    int         play_pat, play_row;
    /* The character each held key went down as, by hardware keycode: a
     * repeat is dropped, and the release ends the note the press started
     * even if Shift has changed the character since ('2' going up as '@').
     * Keycode 0 -- a backend that gives none -- is kept by character, with
     * the press's keyval beside it: the release's keyval is translated with
     * the modifiers as they are THEN, so the character alone would not find
     * the press back, and the slot -- and the key, to the repeat guard --
     * would stay taken for the rest of the session. */
    unsigned char down[256 + 128];
    guint        downkv[128];          /* the keyval that opened a per-character slot */

    /* The shell's in-process destinations ("this window: <name>"), and its
     * answer to a tab close with an unsaved song. Both unset standalone --
     * see trackerview.h. */
    trk_view_sinks sinks;
    int         has_sinks;
    void       *sinks_ud;
    void      (*embed_close)(void *ud);
    void       *embed_close_ud;
    /* trk_view_ensure_saved's continuation: the same flow as a close, but
     * what runs at the end is the shell's next step, and nothing closes. */
    void      (*embed_ensure)(void *ud);
    void       *embed_ensure_ud;
    /* A song was just loaded (open_path succeeded): the shell reopens the
     * synths its sink lines name. Unset standalone. */
    void      (*song_opened)(void *ud);
    void       *song_opened_ud;

    guint       t_follow, t_reroute; /* the view's timers, removed on destroy */
    guint       t_refit;             /* a pending refit_columns, 0 when none is */
    int         embedded;            /* in a shell: its menus carry the file, samples and help commands */
};

typedef struct trk_view ui;

/* A callback that needs the instance and a number (a track, a button, what to
 * do next): GTK hands a handler one user pointer, so the two travel together.
 * Hung on the widget it belongs to, or freed by the callback itself when there
 * is no widget. */
typedef struct { ui *U; int n; } ui_ref;

static ui_ref *ui_ref_new(ui *U, int n, GObject *w)
{
    ui_ref *r = g_new(ui_ref, 1);
    r->U = U; r->n = n;
    g_object_set_data_full(w, "ui-ref", r, g_free);
    return r;
}

static int gutter(ui *U)   { return U->cw * 4; }
static int colwidth(ui *U) { return U->colw; }

static int cur_rows(ui *U)
{
    int r;
    trk_lock(U->e);
    r = trk_song_of(U->e)->pattern[U->ed.pattern].rows;
    trk_unlock(U->e);
    return r;
}

/* How many tracks the song has: the grid, the headers and every hit test go
 * by it, not by the most there is room for. */
static int ntr(ui *U)
{
    const int n = trk_song_of(U->e)->ntracks;
    return n < 1 ? 1 : n > TRK_TRACKS ? TRK_TRACKS : n;
}

static void update_size(ui *U)
{
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(U->area),
                                       gutter(U) + ntr(U) * colwidth(U) + U->cw);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(U->area), cur_rows(U) * U->ch + 2);
}

static void redraw(ui *U) { gtk_widget_queue_draw(U->area); }

static void status(ui *U, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(U->status), msg);
}

/* The window the view sits in, to parent dialogs and transients to: U->win
 * standalone, the host's window when the view is embedded. */
static GtkWindow *parent_window(ui *U)
{
    GtkRoot *root = U->view ? gtk_widget_get_root(U->view) : NULL;
    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

/* --------------------------------------------------------------- drawing */

typedef struct { double r, g, b, a; } rgba;

static void set(cairo_t *cr, rgba c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

static void text_at(cairo_t *cr, PangoLayout *l, double x, double y, const char *s, int n)
{
    pango_layout_set_text(l, s, n);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
}

static void draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u)
{
    GdkRGBA fgc;
    rgba fg, bg, beat, bar, play, cur_row, accent, dim, faint, white = { 1, 1, 1, 1 };
    double x1, y1, x2, y2;
    unsigned char masks[TRK_TRACKS][16];
    int sampled[TRK_TRACKS];
    trk_cell cells[TRK_ROWS_MAX][TRK_TRACKS];   /* the visible rows, copied */
    PangoLayout *l;
    const trk_song *s;
    const trk_pattern *pt;
    int r, r0, r1, t, lpb, mutes = 0, nt;
    ui *U = u; (void)h;

    gtk_widget_get_color(GTK_WIDGET(a), &fgc);
    fg = (rgba){ fgc.red, fgc.green, fgc.blue, 1 };
    /* The theme says what text looks like; the page is the opposite of it. */
    if (fgc.red + fgc.green + fgc.blue > 1.5) bg = (rgba){ 0.12, 0.12, 0.13, 1 };
    else                                      bg = (rgba){ 1, 1, 1, 1 };
    beat    = (rgba){ fg.r, fg.g, fg.b, 0.05 };
    bar     = (rgba){ fg.r, fg.g, fg.b, 0.11 };
    accent  = (rgba){ 0.21, 0.52, 0.89, 1 };
    play    = (rgba){ 0.21, 0.52, 0.89, 0.30 };
    /* Blue while keys write into the pattern, grey while they only play. */
    cur_row = U->ed.edit ? (rgba){ 0.21, 0.52, 0.89, 0.11 } : (rgba){ 0.5, 0.5, 0.5, 0.24 };
    dim     = (rgba){ fg.r, fg.g, fg.b, 0.60 };
    faint   = (rgba){ fg.r, fg.g, fg.b, 0.25 };

    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    set(cr, bg);
    cairo_paint(cr);

    l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, U->font);

    /* Which notes each sample-set track has a sample on -- asked before the
     * song's lock is taken, which this call takes itself. */
    for (t = 0; t < TRK_TRACKS; t++) sampled[t] = trk_sample_mask(U->e, t, masks[t]);

    /* What is drawn is copied out under the lock and drawn after it: the
     * lock is the scheduling thread's and the note path's too, and a redraw
     * of a few thousand Pango strings must not hold either up. */
    trk_lock(U->e);
    s = trk_song_of(U->e);
    pt = &s->pattern[U->ed.pattern];
    lpb = s->lpb > 0 ? s->lpb : 4;
    nt = s->ntracks < 1 ? 1 : s->ntracks > TRK_TRACKS ? TRK_TRACKS : s->ntracks;
    for (t = 0; t < TRK_TRACKS; t++) if (s->track[t].mute) mutes |= 1 << t;
    r0 = (int)(y1 / U->ch);
    if (r0 < 0) r0 = 0;
    r1 = (int)(y2 / U->ch);
    if (r1 > pt->rows - 1) r1 = pt->rows - 1;
    if (r1 >= r0) memcpy(cells[r0], pt->cell[r0], sizeof cells[0] * (size_t)(r1 - r0 + 1));
    trk_unlock(U->e);

    for (r = r0; r <= r1; r++) {
        const int y = r * U->ch;
        char num[12];
        if (r % (lpb * 4) == 0)      { set(cr, bar);  cairo_rectangle(cr, 0, y, w, U->ch); cairo_fill(cr); }
        else if (r % lpb == 0)       { set(cr, beat); cairo_rectangle(cr, 0, y, w, U->ch); cairo_fill(cr); }
        if (r == U->ed.row)           { set(cr, cur_row); cairo_rectangle(cr, 0, y, w, U->ch); cairo_fill(cr); }
        if (U->ed.pattern == U->play_pat && r == U->play_row)
                                     { set(cr, play); cairo_rectangle(cr, 0, y, w, U->ch); cairo_fill(cr); }

        snprintf(num, sizeof num, "%02X", r);
        set(cr, r % lpb == 0 ? fg : dim);
        text_at(cr, l, U->cw / 2.0, y + 1, num, -1);

        for (t = 0; t < nt; t++) {
            static const int start[TRK_FIELDS] = { TRK_COL_NOTE, TRK_COL_VEL, TRK_COL_CC, TRK_COL_VAL };
            static const int len[TRK_FIELDS]   = { 3, 2, 2, 2 };
            const int x = gutter(U) + t * colwidth(U);
            const int on_cursor = r == U->ed.row && t == U->ed.track;
            char txt[TRK_CELL_CHARS + 1];
            int f;
            trk_cell_text(&cells[r][t], txt);
            if (trk_selected(&U->ed, r, t)) {
                rgba sel = accent;
                sel.a = 0.3;
                set(cr, sel);
                cairo_rectangle(cr, x - U->cw / 2.0, y, colwidth(U), U->ch);
                cairo_fill(cr);
            }
            if (on_cursor) {
                set(cr, accent);
                cairo_rectangle(cr, x + start[U->ed.field] * U->cw - 1, y,
                                len[U->ed.field] * U->cw + 2, U->ch);
                cairo_fill(cr);
            }
            /* Field by field, so empty dots can be fainter than what is there
             * and the parameters quieter than the note. */
            for (f = 0; f < TRK_FIELDS; f++) {
                const char *p = txt + start[f];
                rgba c = p[0] == '.' ? faint : f == TRK_F_NOTE ? fg : dim;
                const int nt = cells[r][t].note;
                /* A note its track's sample set has no sample on plays
                 * nothing: red, so it is seen before it is not heard. */
                if (f == TRK_F_NOTE && sampled[t] && nt <= 127 && !(masks[t][nt >> 3] & (1u << (nt & 7))))
                    c = (rgba){ 0.86, 0.2, 0.18, 1 };
                if (mutes & (1 << t)) c.a /= 3;
                if (on_cursor && f == U->ed.field) c = white;
                set(cr, c);
                text_at(cr, l, x + start[f] * U->cw, y + 1, p, len[f]);
            }
        }
    }

    set(cr, faint);
    cairo_set_line_width(cr, 1);
    for (t = 0; t <= nt; t++) {
        const double x = gutter(U) + t * colwidth(U) - U->cw + 0.5;
        cairo_move_to(cr, x, y1);
        cairo_line_to(cr, x, y2);
    }
    cairo_stroke(cr);
    g_object_unref(l);
}

/* ------------------------------------------------------------ song <-> UI */

static void refresh_parts(ui *U);
static gboolean refresh_cheat_idle(gpointer u);
static void clip_status(ui *U, int key);

/* A header over every track the song has, and none over the room left. */
static void show_headers(ui *U)
{
    const int nt = ntr(U);
    int t;
    if (!U->headbox[0]) return;
    for (t = 0; t < TRK_TRACKS; t++) gtk_widget_set_visible(U->headbox[t], t < nt);
}

static void sync_from_song(ui *U)
{
    const trk_song *s;
    int i, t;

    U->loading = 1;
    trk_lock(U->e);
    s = trk_song_of(U->e);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->bpm), s->bpm);
    gtk_range_set_value(GTK_RANGE(U->volume), s->volume);
    for (i = 0; i < NLPB; i++)
        if (k_lpbs[i] == s->lpb) gtk_drop_down_set_selected(GTK_DROP_DOWN(U->lpb), (guint)i);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->pattern), U->ed.pattern);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->rows), s->pattern[U->ed.pattern].rows);
    U->ed.octave = s->track[U->ed.track].octave;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->step), U->ed.step);
    gtk_editable_set_text(GTK_EDITABLE(U->part_name), s->pattern[U->ed.pattern].name);
    for (t = 0; t < TRK_TRACKS; t++) {
        gtk_editable_set_text(GTK_EDITABLE(U->name[t]), s->track[t].name);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U->chan[t]), (guint)s->track[t].channel);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U->oct[t]), (guint)s->track[t].octave);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(U->mute[t]), s->track[t].mute);
    }
    trk_unlock(U->e);
    U->loading = 0;
    show_headers(U);
    refresh_parts(U);
}


static void refresh_samples(ui *U);
static void edit_shown(ui *U);
static void sync_mutes(ui *U);
static void on_track_add(GtkButton *b, gpointer u);
static void on_track_remove(GtkButton *b, gpointer u);
static void schedule_refit(ui *U);   /* defined beside refit_columns, below */

/* Help > Cheat Sheet: for each track playing a sample set, every sample
 * with its note and the key that types it at the octave set now; then the
 * note keys, for the tracks that play windows. Kept up to date while open. */
static void refresh_cheat(ui *U)
{
    static char buf[16384];
    GString *text;
    GtkTextBuffer *tb;
    GtkTextIter a, b;
    char *had;
    int t, window = -1;

    if (!U->cheatwin) return;
    text = g_string_new(NULL);
    static char names[TRK_TRACKS][TRK_NAME_LEN], sets[TRK_TRACKS][TRK_PATH_LEN];
    trk_lock(U->e);
    for (t = 0; t < TRK_TRACKS; t++) {
        snprintf(names[t], sizeof names[t], "%s", trk_song_of(U->e)->track[t].name);
        snprintf(sets[t], sizeof sets[t], "%s", trk_song_of(U->e)->track[t].samples);
    }
    trk_unlock(U->e);
    /* Each set once, under every track that plays it. */
    for (t = 0; t < TRK_TRACKS; t++) {
        char *line, *save = NULL;
        int u, seen = 0, many = 0;
        if (!sets[t][0]) { if (window < 0) window = t; continue; }
        for (u = 0; u < t; u++) if (!strcmp(sets[u], sets[t])) seen = 1;
        if (seen) continue;
        for (u = t + 1; u < TRK_TRACKS; u++) if (!strcmp(sets[u], sets[t])) many = 1;
        g_string_append_printf(text, "%s  --  track%s", sets[t], many ? "s" : "");
        for (u = t; u < TRK_TRACKS; u++)
            if (!strcmp(sets[u], sets[t]))
                g_string_append_printf(text, "%s %d %s", u == t ? "" : ",", u + 1, names[u]);
        g_string_append(text, "\n");
        trk_cheat_sheet(U->e, t, -1, 200, buf, sizeof buf);
        for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            g_string_append_printf(text, "  %s\n", line);
        g_string_append(text, "\n");
    }
    if (window >= 0) {
        char *line, *save = NULL;
        trk_cheat_sheet(U->e, window, -1, 200, buf, sizeof buf);
        g_string_append_printf(text, "Tracks that play a window (track %d's octave)\n", window + 1);
        for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            g_string_append_printf(text, "  %s\n", line);
    }
    g_string_append(text, "\nEach track has its own octave: [ and ] change the cursor's.\n");

    tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(U->cheattext));
    gtk_text_buffer_get_bounds(tb, &a, &b);
    had = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
    if (strcmp(had, text->str)) gtk_text_buffer_set_text(tb, text->str, -1);
    g_free(had);
    g_string_free(text, TRUE);
}

static void on_cheat_gone(GtkWidget *w, gpointer u)
{
    ui *U = u;
    (void)w;
    U->cheatwin = U->cheattext = NULL;
}

static void on_cheat(GSimpleAction *a, GVariant *v, gpointer u)
{
    GtkWidget *sw;
    ui *U = u;
    (void)a; (void)v;
    if (!U->cheatwin) {
        U->cheatwin = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(U->cheatwin), "Cheat Sheet");
        gtk_window_set_transient_for(GTK_WINDOW(U->cheatwin), parent_window(U));
        gtk_window_set_default_size(GTK_WINDOW(U->cheatwin), 520, 640);
        U->cheattext = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(U->cheattext), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(U->cheattext), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(U->cheattext), TRUE);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(U->cheattext), 8);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(U->cheattext), 6);
        sw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), U->cheattext);
        gtk_window_set_child(GTK_WINDOW(U->cheatwin), sw);
        g_signal_connect(U->cheatwin, "destroy", G_CALLBACK(on_cheat_gone), U);
    }
    refresh_cheat(U);
    gtk_window_present(GTK_WINDOW(U->cheatwin));
}

static void show_keys(ui *U);
static void show_columns(ui *U);
static void on_columns(GSimpleAction *a, GVariant *v, gpointer u) { ui *U = u; (void)a; (void)v; show_columns(U); }
static void on_keys(GSimpleAction *a, GVariant *v, gpointer u) { ui *U = u; (void)a; (void)v; show_keys(U); }

/* Help, last of the buttons: what every key does, and the cheat sheet. */
static void help_menu(GtkMenuButton *mb, gpointer u)
{
    GMenu *m = g_menu_new();
    (void)u;
    g_menu_append(m, "Keys", "win.keys");
    g_menu_append(m, "Columns", "win.columns");
    g_menu_append(m, "Cheat Sheet", "win.cheat");
    gtk_menu_button_set_menu_model(mb, G_MENU_MODEL(m));
    g_object_unref(m);
}

static void update_states(ui *U)
{
    int t, kits = 0;
    for (t = 0; t < TRK_TRACKS; t++) {
        int named, kit, ok = trk_routed(U->e, t);
        trk_lock(U->e);
        kit = trk_song_of(U->e)->track[t].samples[0] != 0;
        named = kit || trk_song_of(U->e)->track[t].client[0] != 0;
        trk_unlock(U->e);
        kits |= kit;
        /* A sample set picks its sample by the note itself, so the octave
         * -- which would only move those notes onto other samples -- is off. */
        gtk_widget_set_sensitive(U->oct[t], !kit);
        gtk_widget_set_tooltip_text(U->oct[t], kit ? "A sample set picks its sample by the note: the octave is fixed"
                                                   : "The octave the note keys play on this track");
        gtk_label_set_markup(GTK_LABEL(U->state[t]),
                             !named ? "" : ok ? "<span foreground='#3a3'>●</span>"
                                              : "<span foreground='#c33'>○</span>");
        gtk_widget_set_tooltip_text(U->state[t], !named ? NULL
                                    : kit ? (ok ? "sample set loaded" : "no sample set by that name, or no audio output")
                                    : ok ? "connected" : "that window is not open");
    }
    refresh_cheat(U);
    /* Where the samples play, or why they cannot. */
    if (kits && *trk_audio_status(U->e)) status(U, trk_audio_status(U->e));
}

/* Every window that can be played, plus whatever a track names that is not
 * open right now -- kept and marked, so a song loaded before its windows
 * does not forget where its tracks go. After the windows, the shell's
 * in-process destinations ("this window: <name>"); a track routed to one
 * shows it, and its window names wait in the song for the track to be routed
 * back -- the same lists the Qt shell's tracker widget builds. */
static void refresh_dests(ui *U, int force)
{
    char buf[sizeof U->last_dests];
    const char *sinknames[64];
    char routed[TRK_TRACKS][TRK_DEST_LEN];
    int t, nsinks = 0;

    trk_list_dests(U->e, buf, sizeof buf);
    if (U->has_sinks && U->sinks.names)
        nsinks = U->sinks.names(U->sinks_ud, sinknames, 64);
    /* The sink each track plays, so a routing changed from elsewhere -- a
     * routed tab closing -- rebuilds the lists on the next pass. */
    for (t = 0; t < TRK_TRACKS; t++) {
        int id = trk_sink_of(U->e, t);
        routed[t][0] = 0;
        if (id >= 0) trk_sink_name(U->e, id, routed[t], sizeof routed[t]);
    }
    {   /* The change detector has to see the sinks and the routing too, or a
         * shell tab opening, closing or renaming never rebuilds the lists. */
        size_t l = strlen(buf);
        int i;
        for (i = 0; i < nsinks && l < sizeof buf; i++)
            l += snprintf(buf + l, sizeof buf - l, "|%s", sinknames[i]);
        for (t = 0; t < TRK_TRACKS && l < sizeof buf; t++)
            l += snprintf(buf + l, sizeof buf - l, "|%s", routed[t]);
    }
    if (!force && !strcmp(buf, U->last_dests)) { update_states(U); return; }
    memcpy(U->last_dests, buf, sizeof buf);

    U->loading = 1;
    for (t = 0; t < TRK_TRACKS; t++) {
        GtkStringList *sl = gtk_string_list_new(NULL);
        char want[2 * TRK_DEST_LEN + 2], *copy, *line, *save = NULL;
        const char *want_sink = routed[t];
        int i, sel = 0;

        for (i = 0; i < U->ndest[t]; i++) g_free(U->destval[t][i]);
        U->ndest[t] = 0;
        trk_lock(U->e);
        snprintf(want, sizeof want, "%s\t%s", trk_song_of(U->e)->track[t].client,
                 trk_song_of(U->e)->track[t].port);
        trk_unlock(U->e);

        gtk_string_list_append(sl, "(nowhere)");
        U->destval[t][U->ndest[t]++] = g_strdup("\t");
        /* A sink-routed track's window names stay in the song; the list shows
         * the sinks it can be routed among instead. */
        if (!want_sink[0]) {
            copy = g_strdup(buf);
            for (line = strtok_r(copy, "\n", &save); line && U->ndest[t] < MAXDEST - 1;
                 line = strtok_r(NULL, "\n", &save)) {
                char *tab = strchr(line, '\t'), label[300];
                if (!tab) continue;
                snprintf(label, sizeof label, "%.*s: %s", (int)(tab - line), line, tab + 1);
                gtk_string_list_append(sl, label);
                if (!strcmp(line, want)) sel = U->ndest[t];
                U->destval[t][U->ndest[t]++] = g_strdup(line);
            }
            g_free(copy);
            if (!sel && strcmp(want, "\t")) {
                char *tab = strchr(want, '\t'), label[300];
                snprintf(label, sizeof label, "%.*s: %s (not open)", (int)(tab - want), want, tab + 1);
                gtk_string_list_append(sl, label);
                sel = U->ndest[t];
                U->destval[t][U->ndest[t]++] = g_strdup(want);
            }
        }
        for (i = 0; i < nsinks && U->ndest[t] < MAXDEST - 1; i++) {
            char label[TRK_DEST_LEN + 32], v[TRK_DEST_LEN + 8];
            snprintf(label, sizeof label, "this window: %.*s", TRK_DEST_LEN - 1, sinknames[i]);
            gtk_string_list_append(sl, label);
            if (want_sink[0] && !strcmp(sinknames[i], want_sink)) sel = U->ndest[t];
            snprintf(v, sizeof v, "sink\t%.*s", TRK_DEST_LEN - 1, sinknames[i]);
            U->destval[t][U->ndest[t]++] = g_strdup(v);
        }
        if (want_sink[0] && !sel && U->ndest[t] < MAXDEST - 1) {
            char label[TRK_DEST_LEN + 40], v[TRK_DEST_LEN + 8];
            snprintf(label, sizeof label, "this window: %.*s (closed)", TRK_DEST_LEN - 1, want_sink);
            gtk_string_list_append(sl, label);
            sel = U->ndest[t];
            snprintf(v, sizeof v, "sink\t%.*s", TRK_DEST_LEN - 1, want_sink);
            U->destval[t][U->ndest[t]++] = g_strdup(v);
        }
        gtk_drop_down_set_model(GTK_DROP_DOWN(U->dest[t]), G_LIST_MODEL(sl));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U->dest[t]), (guint)sel);
        g_object_unref(sl);
    }
    U->loading = 0;
    refresh_samples(U);
    update_states(U);
    /* The models were (re)assigned here, possibly long after the first fit:
     * what the closed boxes draw now is what the columns have to hold. */
    schedule_refit(U);
}

/* Each track's sample-set box: every set there is, and whatever a track
 * names that is not there -- kept and marked, as a window that is not open
 * is. A track with a set plays no window, so its window box is greyed out. */
static void refresh_samples(ui *U)
{
    static char buf[32768];
    int t;

    trk_list_sample_sets(U->e, buf, sizeof buf);
    U->loading = 1;
    for (t = 0; t < TRK_TRACKS; t++) {
        GtkStringList *sl = gtk_string_list_new(NULL);
        char want[TRK_PATH_LEN], *copy, *line, *save = NULL;
        int i, sel = 0;

        for (i = 0; i < U->nsample[t]; i++) g_free(U->sampleval[t][i]);
        U->nsample[t] = 0;
        trk_lock(U->e);
        snprintf(want, sizeof want, "%s", trk_song_of(U->e)->track[t].samples);
        trk_unlock(U->e);

        gtk_string_list_append(sl, "(no samples)");
        U->sampleval[t][U->nsample[t]++] = g_strdup("");
        copy = g_strdup(buf);
        for (line = strtok_r(copy, "\n", &save); line && U->nsample[t] < MAXSAMP - 1;
             line = strtok_r(NULL, "\n", &save)) {
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            gtk_string_list_append(sl, line);
            if (want[0] && (!strcmp(line, want) || !strcmp(tab + 1, want))) sel = U->nsample[t];
            U->sampleval[t][U->nsample[t]++] = g_strdup(line);
        }
        g_free(copy);
        if (!sel && want[0]) {
            char label[TRK_PATH_LEN + 16];
            snprintf(label, sizeof label, "%s (not found)", want);
            gtk_string_list_append(sl, label);
            sel = U->nsample[t];
            U->sampleval[t][U->nsample[t]++] = g_strdup(want);
        }
        gtk_drop_down_set_model(GTK_DROP_DOWN(U->sample[t]), G_LIST_MODEL(sl));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U->sample[t]), (guint)sel);
        g_object_unref(sl);
        gtk_widget_set_sensitive(U->dest[t], !want[0]);
    }
    U->loading = 0;
}

static void on_sample(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    int t = r->n;
    guint i = gtk_drop_down_get_selected(d);
    (void)ps;
    if (U->loading || i >= (guint)U->nsample[t]) return;
    trk_lock(U->e);
    snprintf(trk_song_of(U->e)->track[t].samples, TRK_PATH_LEN, "%s", U->sampleval[t][i]);
    trk_unlock(U->e);
    trk_route(U->e);
    gtk_widget_set_sensitive(U->dest[t], U->sampleval[t][i][0] == 0);
    update_states(U);
}

/* ------------------------------------------------------ Load Sample Set */

static void on_samples_folder(GObject *src, GAsyncResult *res, gpointer u)
{
    ui *U = u;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path, msg[TRK_PATH_LEN + 64];
    if (!f) return;
    if ((path = g_file_get_path(f))) {
        trk_add_sample_set(U->e, path);
        refresh_dests(U, 1);
        snprintf(msg, sizeof msg, "loaded %s -- pick it in a track's sample-set box", path);
        status(U, msg);
        g_free(path);
    }
    g_object_unref(f);
}

static void on_load_samples(GSimpleAction *a, GVariant *v, gpointer u)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    ui *U = u;
    (void)a; (void)v;
    gtk_file_dialog_set_title(d, "Load a sample set (a folder of WAVs)");
    gtk_file_dialog_select_folder(d, parent_window(U), NULL, on_samples_folder, U);
    g_object_unref(d);
}

/* ------------------------------------------------------ Edit Sample Set
 *
 * Which note plays which WAV in a set, its gain and choke group, WAVs added
 * and taken out -- saved as the set's kit.txt, which every track playing the
 * set then plays. The rows live in a dk_map from drumkit.h, as in the Qt
 * window's editor, so both read and write the same. */

static struct {
    ui        *U;                     /* the window that opened it */
    GtkWidget *win, *sets, *dir, *grid, *status, *kbscroll, *octave, *paned;
    GtkWidget *kkey[128], *kdd[128], *kplay[128], *krow[128];   /* the vertical keyboard, by note */
    char     **choice;                  /* what the drop-downs offer: files, relative to the set */
    int        nchoice;
    int        sel, menu_note, curnote; /* the picked row, the key a menu is on, the picked key */
    dk_map    *m;
    char       name[TRK_PATH_LEN];
    char      *setval[MAXSAMP];
    int        nsets, cur, dirty, filling, pending;   /* pending: a set to switch to */
} E;

static void kb_refresh(void);
static void ed_status(const char *msg) { gtk_label_set_text(GTK_LABEL(E.status), msg); }

static int ed_dir(char *out, size_t n)
{
    return trk_sample_set_dir(E.U->e, E.name, out, n);
}

static void ed_note_changed(GtkEditable *ed, gpointer u)
{
    int i = GPOINTER_TO_INT(u);
    if (E.filling || i >= E.m->n) return;
    E.m->pad[i].note = drumkit_note_parse(gtk_editable_get_text(ed));
    E.dirty = 1;
    kb_refresh();
}

static void ed_gain_changed(GtkSpinButton *s, gpointer u)
{
    int i = GPOINTER_TO_INT(u);
    if (E.filling || i >= E.m->n) return;
    E.m->pad[i].gain_db = gtk_spin_button_get_value(s);
    E.dirty = 1;
}

static void ed_choke_changed(GtkSpinButton *s, gpointer u)
{
    int i = GPOINTER_TO_INT(u);
    if (E.filling || i >= E.m->n) return;
    E.m->pad[i].choke = gtk_spin_button_get_value_as_int(s);
    E.dirty = 1;
}

static void ed_play(GtkButton *b, gpointer u)
{
    int i = GPOINTER_TO_INT(u);
    char dir[TRK_PATH_LEN] = "", msg[256];
    (void)b;
    if (i >= E.m->n) return;
    ed_dir(dir, sizeof dir);
    if (trk_audition(E.U->e, dir, E.m->pad[i].file, E.m->pad[i].gain_db, 100)) {
        snprintf(msg, sizeof msg, "that WAV would not play. %s", trk_audio_status(E.U->e));
        ed_status(msg);
    }
}


static void ed_fill(void);

/* ---- the keys: a vertical keyboard, highest at the top, scrolled. Every
 * note the tracker reaches is a row: the key, a drop-down of the samples it
 * can have, and a button to hear it. ---------------------------------- */

#define KB_LO 12
#define KB_HI 127
static const char kb_keys[] = "zsxdcvgbhnjmq2w3er5t6y7ui9o0p";

static int kb_black(int n) { int m = n % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; }

static int ed_pad_of(int note)
{
    int i;
    for (i = 0; i < E.m->n; i++) if (E.m->pad[i].note == note) return i;
    return -1;
}
static const char *ed_pad_name(int i)
{
    const char *b = strrchr(E.m->pad[i].file, '/');
    return b ? b + 1 : E.m->pad[i].file;
}

static void kb_item_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u)
{
    GtkWidget *l = gtk_label_new("");
    (void)f; (void)u;
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
    gtk_label_set_width_chars(GTK_LABEL(l), 12);
    gtk_list_item_set_child(li, l);
}

static void kb_item_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u)
{
    GtkStringObject *o = gtk_list_item_get_item(li);
    (void)f; (void)u;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), o ? gtk_string_object_get_string(o) : "");
}

static void kb_css(void)
{
    static int done;
    GtkCssProvider *cp;
    if (done) return;
    done = 1;
    cp = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cp,
        ".km-key { font-family: monospace; padding: 3px 6px; border: 1px solid #777; min-width: 78px; }"
        ".km-white { background: #f2f2f2; color: #111; }"
        ".km-black { background: #3a3a40; color: #eee; }"
        ".km-white.km-has { background: #96cdf5; }"
        ".km-black.km-has { background: #23709f; color: #fff; }"
        ".km-white.km-cur { background: #ffcd6e; }"
        ".km-black.km-cur { background: #d28214; color: #fff; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(cp),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(cp);
}

static void kb_scan(const char *root, const char *rel, GPtrArray *out)
{
    char path[TRK_PATH_LEN];
    const char *name;
    GDir *d;
    snprintf(path, sizeof path, "%s%s%s", root, rel[0] ? "/" : "", rel);
    if (!(d = g_dir_open(path, 0, NULL))) return;
    while ((name = g_dir_read_name(d))) {
        char sub[TRK_PATH_LEN], full[TRK_PATH_LEN + 8];
        size_t l = strlen(name);
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", name);
        snprintf(full, sizeof full, "%s/%s", root, sub);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) kb_scan(root, sub, out);
        else if (l > 4 && !g_ascii_strcasecmp(name + l - 4, ".wav")) g_ptr_array_add(out, g_strdup(sub));
    }
    g_dir_close(d);
}

static int kb_cmp(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(char *const *)a, *(char *const *)b);
}

static char *kb_label(const char *file)
{
    const char *b = strrchr(file, '/');
    char *t = g_strdup(b ? b + 1 : file);
    size_t l = strlen(t);
    if (l > 4 && !g_ascii_strcasecmp(t + l - 4, ".wav")) t[l - 4] = 0;
    return t;
}

/* The drop-downs' list: every WAV in the set's folder, then any pad that
 * names one from elsewhere. */
static void kb_choices(void)
{
    GPtrArray *a = g_ptr_array_new();
    GtkStringList *sl = gtk_string_list_new(NULL);
    char dir[TRK_PATH_LEN] = "";
    int i, n;
    guint k;
    if (!ed_dir(dir, sizeof dir)) kb_scan(dir, "", a);
    g_ptr_array_sort(a, kb_cmp);
    for (i = 0; i < E.m->n; i++) {
        int found = 0;
        for (k = 0; k < a->len; k++) if (!strcmp(a->pdata[k], E.m->pad[i].file)) found = 1;
        if (!found) g_ptr_array_add(a, g_strdup(E.m->pad[i].file));
    }
    for (i = 0; i < E.nchoice; i++) g_free(E.choice[i]);
    g_free(E.choice);
    E.nchoice = (int)a->len;
    E.choice = g_new0(char *, a->len + 1);
    gtk_string_list_append(sl, "— none —");
    for (k = 0; k < a->len; k++) {
        char *lb = kb_label(a->pdata[k]);
        E.choice[k] = a->pdata[k];
        gtk_string_list_append(sl, lb);
        g_free(lb);
    }
    g_ptr_array_free(a, FALSE);
    E.filling = 1;
    for (n = KB_LO; n <= KB_HI; n++)
        if (E.kdd[n]) gtk_drop_down_set_model(GTK_DROP_DOWN(E.kdd[n]), G_LIST_MODEL(sl));
    E.filling = 0;
    g_object_unref(sl);
}

static int kb_choice_of(const char *file)
{
    int k;
    for (k = 0; k < E.nchoice; k++) if (!strcmp(E.choice[k], file)) return k;
    return -1;
}

static void kb_refresh(void)
{
    int n, i;
    static const char *const cls[] = { "km-has", "km-cur" };
    if (!E.kbscroll) return;
    for (i = 0; i < E.m->n; i++) if (kb_choice_of(E.m->pad[i].file) < 0) { kb_choices(); break; }
    E.filling = 1;
    for (n = KB_LO; n <= KB_HI; n++) {
        int pad = ed_pad_of(n), idx = (n - (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(E.octave)) + 1) * 12);
        char nn[5], txt[24];
        drumkit_note_name(n, nn);
        snprintf(txt, sizeof txt, "%-5s%c", nn,
                 idx >= 0 && idx < 29 ? (kb_keys[idx] >= 'a' && kb_keys[idx] <= 'z' ? kb_keys[idx] - 32 : kb_keys[idx]) : ' ');
        gtk_label_set_text(GTK_LABEL(E.kkey[n]), txt);
        gtk_widget_remove_css_class(E.kkey[n], cls[0]);
        gtk_widget_remove_css_class(E.kkey[n], cls[1]);
        if (pad >= 0) gtk_widget_add_css_class(E.kkey[n], cls[0]);
        if (n == E.curnote) gtk_widget_add_css_class(E.kkey[n], cls[1]);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(E.kdd[n]),
                                   pad >= 0 ? (guint)(kb_choice_of(E.m->pad[pad].file) + 1) : 0);
        gtk_widget_set_sensitive(E.kplay[n], pad >= 0);
    }
    E.filling = 0;
}

static gboolean kb_scroll_idle(gpointer u)
{
    int note = GPOINTER_TO_INT(u);
    graphene_rect_t b;
    GtkAdjustment *va;
    if (!E.kbscroll || note < KB_LO || note > KB_HI) return G_SOURCE_REMOVE;
    va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(E.kbscroll));
    if (gtk_widget_compute_bounds(E.krow[note], gtk_widget_get_parent(E.krow[note]), &b))
        gtk_adjustment_set_value(va, b.origin.y - gtk_adjustment_get_page_size(va) + 90);
    return G_SOURCE_REMOVE;
}

static void kb_scroll_to(int note) { g_timeout_add(60, kb_scroll_idle, GINT_TO_POINTER(note)); }

static void ed_play_pad(int i)
{
    char dir[TRK_PATH_LEN] = "", msg[256];
    ed_dir(dir, sizeof dir);
    if (trk_audition(E.U->e, dir, E.m->pad[i].file, E.m->pad[i].gain_db, 100)) {
        snprintf(msg, sizeof msg, "that WAV would not play. %s", trk_audio_status(E.U->e));
        ed_status(msg);
    }
}

/* Put a pad on a note. A pad already there swaps to this one's old note, so
 * nothing is lost and no two pads share a key. */
static void ed_assign(int row, int note)
{
    int other, old;
    char nn[5], msg[200];
    if (row < 0 || row >= E.m->n || note < 0 || note > 127) return;
    old = E.m->pad[row].note;
    other = ed_pad_of(note);
    if (other >= 0 && other != row && old >= 0) E.m->pad[other].note = old;
    E.m->pad[row].note = note;
    E.sel = row;
    E.curnote = note;
    E.dirty = 1;
    ed_fill();
    drumkit_note_name(note, nn);
    snprintf(msg, sizeof msg, "%s is on %s -- Save to keep it", ed_pad_name(row), nn);
    ed_status(msg);
    ed_play_pad(row);
}

static void kb_played(int note, int hear)
{
    int pad = ed_pad_of(note);
    E.sel = pad;
    E.curnote = note;
    ed_fill();
    if (pad >= 0 && hear) ed_play_pad(pad);
}

static void kb_remove_pad(int i)
{
    memmove(&E.m->pad[i], &E.m->pad[i + 1], sizeof E.m->pad[0] * (size_t)(E.m->n - i - 1));
    E.m->n--;
    E.sel = -1;
    E.dirty = 1;
}

/* The drop-down beside a key: this sample on this key. One already on another
 * key moves (swapping with what is here); one not in the set yet is added,
 * replacing what is here. */
static void kb_dd_changed(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    int note = GPOINTER_TO_INT(u), idx = (int)gtk_drop_down_get_selected(d), here, have = -1, i;
    char nn[5], msg[240];
    (void)ps;
    if (E.filling || !E.nchoice) return;
    here = ed_pad_of(note);
    if (idx <= 0 || idx > E.nchoice) {
        if (here >= 0) { kb_remove_pad(here); ed_fill(); }
        return;
    }
    for (i = 0; i < E.m->n; i++) if (!strcmp(E.m->pad[i].file, E.choice[idx - 1])) have = i;
    if (have >= 0) { ed_assign(have, note); return; }
    if (here >= 0) snprintf(E.m->pad[here].file, sizeof E.m->pad[here].file, "%s", E.choice[idx - 1]);
    else {
        dk_pad *p;
        if (E.m->n >= DK_MAX_SAMPLES) return;
        p = &E.m->pad[E.m->n++];
        memset(p, 0, sizeof *p);
        p->note = note;
        snprintf(p->file, sizeof p->file, "%s", E.choice[idx - 1]);
        here = E.m->n - 1;
    }
    E.sel = here;
    E.curnote = note;
    E.dirty = 1;
    ed_fill();
    drumkit_note_name(note, nn);
    snprintf(msg, sizeof msg, "%s is on %s -- Save to keep it", ed_pad_name(here), nn);
    ed_status(msg);
    ed_play_pad(here);
}

static void kb_menu_put(GtkButton *b, gpointer u)
{
    (void)u;
    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER)));
    if (E.sel >= 0) ed_assign(E.sel, E.menu_note);
}

static void kb_menu_clear(GtkButton *b, gpointer u)
{
    int i = ed_pad_of(E.menu_note);
    (void)u;
    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER)));
    if (i < 0) return;
    kb_remove_pad(i);
    ed_fill();
}

static void kb_chosen(GObject *src, GAsyncResult *res, gpointer u)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path, dir[TRK_PATH_LEN] = "";
    size_t dl;
    int i;
    (void)u;
    if (!f) return;
    path = g_file_get_path(f);
    g_object_unref(f);
    if (!path) return;
    ed_dir(dir, sizeof dir);
    dl = strlen(dir);
    if ((i = ed_pad_of(E.menu_note)) < 0) {
        if (E.m->n >= DK_MAX_SAMPLES) { g_free(path); return; }
        i = E.m->n++;
    }
    memset(&E.m->pad[i], 0, sizeof E.m->pad[i]);
    E.m->pad[i].note = E.menu_note;
    if (dl && !strncmp(path, dir, dl) && path[dl] == '/') snprintf(E.m->pad[i].file, sizeof E.m->pad[i].file, "%s", path + dl + 1);
    else snprintf(E.m->pad[i].file, sizeof E.m->pad[i].file, "%s", path);
    g_free(path);
    E.sel = i;
    E.dirty = 1;
    ed_fill();
    ed_play_pad(i);
}

static GtkFileDialog *kb_wav_dialog(const char *title)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *wav = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    char dir[TRK_PATH_LEN] = "";
    gtk_file_filter_set_name(wav, "WAV files");
    gtk_file_filter_add_suffix(wav, "wav");
    gtk_file_filter_add_suffix(wav, "WAV");
    g_list_store_append(filters, wav);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    gtk_file_dialog_set_title(d, title);
    if (!ed_dir(dir, sizeof dir)) {
        GFile *start = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(d, start);
        g_object_unref(start);
    }
    g_object_unref(wav);
    g_object_unref(filters);
    return d;
}

static void kb_menu_choose(GtkButton *b, gpointer u)
{
    GtkFileDialog *d = kb_wav_dialog("A WAV for this key");
    (void)u;
    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER)));
    gtk_file_dialog_open(d, GTK_WINDOW(E.win), NULL, kb_chosen, NULL);
    g_object_unref(d);
}

/* Every WAV of a folder onto keys, one after another. */
static void kb_folder_done(GObject *src, GAsyncResult *res, gpointer u)
{
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path, dir[TRK_PATH_LEN] = "", msg[TRK_PATH_LEN + 120], nn[5] = "";
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    GDir *d;
    size_t dl;
    int note = 60, i, added = 0, skipped = 0, first = -1;
    guint k;
    (void)u;
    if (!f) { g_ptr_array_free(names, TRUE); return; }
    path = g_file_get_path(f);
    g_object_unref(f);
    if (!path || !(d = g_dir_open(path, 0, NULL))) { g_free(path); g_ptr_array_free(names, TRUE); return; }
    while ((name = g_dir_read_name(d))) {
        size_t l = strlen(name);
        if (l > 4 && !g_ascii_strcasecmp(name + l - 4, ".wav")) g_ptr_array_add(names, g_strdup(name));
    }
    g_dir_close(d);
    g_ptr_array_sort(names, kb_cmp);
    ed_dir(dir, sizeof dir);
    dl = strlen(dir);
    if (E.curnote >= 0) note = E.curnote;
    else for (i = 0; i < E.m->n; i++) if (E.m->pad[i].note >= note) note = E.m->pad[i].note + 1;
    for (k = 0; k < names->len; k++) {
        char full[TRK_PATH_LEN + 300], rel[512];
        dk_pad *p;
        snprintf(full, sizeof full, "%s/%s", path, (char *)names->pdata[k]);
        if (dl && !strncmp(full, dir, dl) && full[dl] == '/') snprintf(rel, sizeof rel, "%s", full + dl + 1);
        else snprintf(rel, sizeof rel, "%s", full);
        for (i = 0; i < E.m->n; i++) if (!strcmp(E.m->pad[i].file, rel)) break;
        if (i < E.m->n) { skipped++; continue; }
        while (note <= 127 && ed_pad_of(note) >= 0) note++;
        if (note > 127 || E.m->n >= DK_MAX_SAMPLES) break;
        if (first < 0) first = note;
        p = &E.m->pad[E.m->n++];
        memset(p, 0, sizeof *p);
        p->note = note++;
        snprintf(p->file, sizeof p->file, "%s", rel);
        added++;
    }
    if (added) { E.dirty = 1; E.curnote = first; }
    if (first >= 0) drumkit_note_name(first, nn);
    snprintf(msg, sizeof msg, added ? "added %d WAV%s from %s starting at %s -- Save to keep them"
                                    : "nothing new in %s (%d already in the set)%s",
             added ? added : 0, "", path, nn);
    if (added) snprintf(msg, sizeof msg, "added %d WAV%s from %s starting at %s%s -- Save to keep them",
                        added, added == 1 ? "" : "s", path, nn, skipped ? " (some already in the set)" : "");
    else snprintf(msg, sizeof msg, "nothing new in %s (%d already in the set)", path, skipped);
    g_free(path);
    g_ptr_array_free(names, TRUE);
    kb_choices();
    ed_fill();
    ed_status(msg);
    if (first >= 0) kb_scroll_to(first);
}

static void kb_load_folder(GtkButton *b, gpointer u)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    char dir[TRK_PATH_LEN] = "";
    (void)b; (void)u;
    gtk_file_dialog_set_title(d, "A folder of WAVs");
    if (!ed_dir(dir, sizeof dir)) {
        GFile *start = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(d, start);
        g_object_unref(start);
    }
    gtk_file_dialog_select_folder(d, GTK_WINDOW(E.win), NULL, kb_folder_done, NULL);
    g_object_unref(d);
}

static void kb_key_pressed(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    int note = GPOINTER_TO_INT(u);
    (void)n; (void)x; (void)y;
    gtk_widget_grab_focus(E.kbscroll);
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) == 3) {
        GtkWidget *pop = gtk_popover_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *b;
        char nn[5], label[160];
        drumkit_note_name(note, nn);
        E.menu_note = note;
        if (E.sel >= 0) snprintf(label, sizeof label, "Put \"%s\" on %s", ed_pad_name(E.sel), nn);
        else snprintf(label, sizeof label, "Put the picked sample on %s", nn);
        b = gtk_button_new_with_label(label);
        gtk_widget_set_sensitive(b, E.sel >= 0);
        g_signal_connect(b, "clicked", G_CALLBACK(kb_menu_put), NULL);
        gtk_box_append(GTK_BOX(box), b);
        snprintf(label, sizeof label, "Choose a WAV for %s…", nn);
        b = gtk_button_new_with_label(label);
        g_signal_connect(b, "clicked", G_CALLBACK(kb_menu_choose), NULL);
        gtk_box_append(GTK_BOX(box), b);
        snprintf(label, sizeof label, "Take the sample off %s", nn);
        b = gtk_button_new_with_label(label);
        gtk_widget_set_sensitive(b, ed_pad_of(note) >= 0);
        g_signal_connect(b, "clicked", G_CALLBACK(kb_menu_clear), NULL);
        gtk_box_append(GTK_BOX(box), b);
        gtk_popover_set_child(GTK_POPOVER(pop), box);
        gtk_widget_set_parent(pop, E.kkey[note]);
        g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
        gtk_popover_popup(GTK_POPOVER(pop));
        return;
    }
    kb_played(note, 0);
}

static void kb_play_clicked(GtkButton *b, gpointer u) { (void)b; kb_played(GPOINTER_TO_INT(u), 1); }

static gboolean kb_key(GtkEventControllerKey *c, guint keyval, guint code, GdkModifierType m, gpointer u)
{
    int note;
    (void)c; (void)code; (void)u;
    if (m & (GDK_CONTROL_MASK | GDK_ALT_MASK) || keyval > 0x7e) return FALSE;
    note = trk_key_note((int)keyval, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(E.octave)));
    if (note < 0) return FALSE;
    kb_played(note, 1);
    return TRUE;
}

static gboolean kb_drop(GtkDropTarget *t, const GValue *v, double x, double y, gpointer u)
{
    (void)t; (void)x; (void)y;
    ed_assign(g_value_get_int(v), GPOINTER_TO_INT(u));
    return TRUE;
}

static GdkContentProvider *row_drag_prepare(GtkDragSource *s, double x, double y, gpointer u)
{
    (void)s; (void)x; (void)y;
    return gdk_content_provider_new_typed(G_TYPE_INT, GPOINTER_TO_INT(u));
}

static void row_picked(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    int i = GPOINTER_TO_INT(u);
    (void)g; (void)n; (void)x; (void)y;
    E.sel = i;
    if (i >= 0 && i < E.m->n) E.curnote = E.m->pad[i].note;
    ed_fill();
}

static void octave_changed(GtkSpinButton *s, gpointer u) { (void)s; (void)u; kb_refresh(); }

/* The keyboard's rows, built once: a hundred and sixteen, scrolled. */
static GtkWidget *kb_build(void)
{
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1), *sw = gtk_scrolled_window_new();
    GtkEventController *keyc = gtk_event_controller_key_new();
    int n;
    kb_css();
    for (n = KB_HI; n >= KB_LO; n--) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkGesture *click = gtk_gesture_click_new();
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_COPY);
        E.krow[n] = row;
        E.kkey[n] = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(E.kkey[n]), 0);
        gtk_widget_add_css_class(E.kkey[n], "km-key");
        gtk_widget_add_css_class(E.kkey[n], kb_black(n) ? "km-black" : "km-white");
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
        g_signal_connect(click, "pressed", G_CALLBACK(kb_key_pressed), GINT_TO_POINTER(n));
        gtk_widget_add_controller(E.kkey[n], GTK_EVENT_CONTROLLER(click));
        E.kdd[n] = gtk_drop_down_new(NULL, NULL);
        {
            GtkListItemFactory *fac = gtk_signal_list_item_factory_new();
            g_signal_connect(fac, "setup", G_CALLBACK(kb_item_setup), NULL);
            g_signal_connect(fac, "bind", G_CALLBACK(kb_item_bind), NULL);
            gtk_drop_down_set_factory(GTK_DROP_DOWN(E.kdd[n]), fac);
            g_object_unref(fac);
        }
        gtk_widget_set_hexpand(E.kdd[n], TRUE);
        g_signal_connect(E.kdd[n], "notify::selected", G_CALLBACK(kb_dd_changed), GINT_TO_POINTER(n));
        E.kplay[n] = gtk_button_new_with_label("▶");
        gtk_widget_set_tooltip_text(E.kplay[n], "Hear it");
        g_signal_connect(E.kplay[n], "clicked", G_CALLBACK(kb_play_clicked), GINT_TO_POINTER(n));
        g_signal_connect(drop, "drop", G_CALLBACK(kb_drop), GINT_TO_POINTER(n));
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop));
        gtk_box_append(GTK_BOX(row), E.kkey[n]);
        gtk_box_append(GTK_BOX(row), E.kdd[n]);
        gtk_box_append(GTK_BOX(row), E.kplay[n]);
        gtk_box_append(GTK_BOX(list), row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_set_focusable(sw, TRUE);
    g_signal_connect(keyc, "key-pressed", G_CALLBACK(kb_key), NULL);
    gtk_widget_add_controller(sw, keyc);
    E.kbscroll = sw;
    return sw;
}

static void ed_fill(void);

static void ed_remove(GtkButton *b, gpointer u)
{
    int i = GPOINTER_TO_INT(u);
    (void)b;
    if (i >= E.m->n) return;
    memmove(&E.m->pad[i], &E.m->pad[i + 1], sizeof E.m->pad[0] * (size_t)(E.m->n - i - 1));
    E.m->n--;
    E.sel = -1;
    E.dirty = 1;
    ed_fill();
}

/* The grid rebuilt from the map: a row a pad. */
static void ed_fill(void)
{
    GtkWidget *c;
    int i;
    static const char *const head[] = { "Note", "Sample", "Gain dB", "Choke" };

    E.filling = 1;
    while ((c = gtk_widget_get_first_child(E.grid))) gtk_grid_remove(GTK_GRID(E.grid), c);
    for (i = 0; i < 4; i++) {
        GtkWidget *l = gtk_label_new(head[i]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_add_css_class(l, "dim-label");
        gtk_grid_attach(GTK_GRID(E.grid), l, i, 0, 1, 1);
    }
    for (i = 0; i < E.m->n; i++) {
        const dk_pad *p = &E.m->pad[i];
        char nn[5] = "";
        const char *base = strrchr(p->file, '/');
        GtkWidget *note = gtk_entry_new(), *file, *gain, *choke, *play, *del;
        if (p->note >= 0) drumkit_note_name(p->note, nn);
        gtk_editable_set_text(GTK_EDITABLE(note), nn);
        gtk_editable_set_width_chars(GTK_EDITABLE(note), 4);
        file = gtk_label_new(base ? base + 1 : p->file);
        gtk_label_set_xalign(GTK_LABEL(file), 0);
        if (i == E.sel) gtk_widget_add_css_class(file, "heading");
        {   /* click it to pick it; drag it onto a key to put it there */
            GtkGesture *click = gtk_gesture_click_new();
            GtkDragSource *drag = gtk_drag_source_new();
            g_signal_connect(click, "pressed", G_CALLBACK(row_picked), GINT_TO_POINTER(i));
            gtk_widget_add_controller(file, GTK_EVENT_CONTROLLER(click));
            gtk_drag_source_set_actions(drag, GDK_ACTION_COPY);
            g_signal_connect(drag, "prepare", G_CALLBACK(row_drag_prepare), GINT_TO_POINTER(i));
            gtk_widget_add_controller(file, GTK_EVENT_CONTROLLER(drag));
        }
        gtk_label_set_ellipsize(GTK_LABEL(file), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(file, TRUE);
        gtk_widget_set_tooltip_text(file, p->file);
        gain = gtk_spin_button_new_with_range(-60, 24, 0.5);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(gain), 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gain), p->gain_db);
        choke = gtk_spin_button_new_with_range(0, 99, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(choke), p->choke);
        gtk_widget_set_tooltip_text(choke, "Pads with the same number cut each other off; 0: none");
        play = gtk_button_new_with_label("▶");
        gtk_widget_set_tooltip_text(play, "Listen to it");
        del = gtk_button_new_with_label("✕");
        gtk_widget_set_tooltip_text(del, "Take it out of the set");
        g_signal_connect(note, "changed", G_CALLBACK(ed_note_changed), GINT_TO_POINTER(i));
        g_signal_connect(gain, "value-changed", G_CALLBACK(ed_gain_changed), GINT_TO_POINTER(i));
        g_signal_connect(choke, "value-changed", G_CALLBACK(ed_choke_changed), GINT_TO_POINTER(i));
        g_signal_connect(play, "clicked", G_CALLBACK(ed_play), GINT_TO_POINTER(i));
        g_signal_connect(del, "clicked", G_CALLBACK(ed_remove), GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(E.grid), note,  0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(E.grid), file,  1, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(E.grid), gain,  2, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(E.grid), choke, 3, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(E.grid), play,  4, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(E.grid), del,   5, i + 1, 1, 1);
    }
    E.filling = 0;
    kb_refresh();
}

static void ed_load(int k)
{
    char dir[TRK_PATH_LEN], line[TRK_PATH_LEN + 64];
    if (k < 0 || k >= E.nsets) return;
    E.cur = k;
    snprintf(E.name, sizeof E.name, "%s", E.setval[k]);
    E.dirty = 0;
    if (ed_dir(dir, sizeof dir)) {
        memset(E.m, 0, sizeof *E.m);
        gtk_label_set_text(GTK_LABEL(E.dir), "not found");
    } else {
        drumkit_map_read(E.m, dir);
        snprintf(line, sizeof line, "%s   %s", dir,
                 E.m->mapped ? "(kit.txt)" : "(no kit.txt yet: every WAV from C-4 up)");
        gtk_label_set_text(GTK_LABEL(E.dir), line);
    }
    E.sel = -1;
    E.curnote = -1;
    kb_choices();
    if (E.m->n) {
        int lowest = 127, j;
        for (j = 0; j < E.m->n; j++) if (E.m->pad[j].note < lowest) lowest = E.m->pad[j].note;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(E.octave), lowest / 12 - 1 < 0 ? 0 : lowest / 12 - 1);
    }
    ed_fill();
    ed_status("");
    if (E.m->n) {
        int lowest = 127, j;
        for (j = 0; j < E.m->n; j++) if (E.m->pad[j].note < lowest) lowest = E.m->pad[j].note;
        kb_scroll_to(lowest);
    }
}

/* Asked before changes are thrown away: by Close, or by picking another set. */
static void ed_discard_answer(GObject *src, GAsyncResult *res, gpointer u)
{
    int b = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    (void)u;
    if (b != 1) {                                  /* Cancel: put the set back */
        E.filling = 1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(E.sets), (guint)E.cur);
        E.filling = 0;
        return;
    }
    E.dirty = 0;
    if (E.pending >= 0) ed_load(E.pending);
    else gtk_window_destroy(GTK_WINDOW(E.win));
}

static void ed_ask_discard(int pending)
{
    char q[TRK_PATH_LEN + 40];
    GtkAlertDialog *a;
    const char *buttons[] = { "Cancel", "Discard", NULL };
    snprintf(q, sizeof q, "Discard the changes to %s?", E.name);
    a = gtk_alert_dialog_new("%s", q);
    gtk_alert_dialog_set_buttons(a, buttons);
    gtk_alert_dialog_set_cancel_button(a, 0);
    gtk_alert_dialog_set_default_button(a, 0);
    E.pending = pending;
    gtk_alert_dialog_choose(a, GTK_WINDOW(E.win), NULL, ed_discard_answer, NULL);
    g_object_unref(a);
}

static void ed_set_picked(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    int k = (int)gtk_drop_down_get_selected(d);
    (void)ps; (void)u;
    if (E.filling || k == E.cur) return;
    if (E.dirty) ed_ask_discard(k);
    else ed_load(k);
}

static void ed_added(GObject *src, GAsyncResult *res, gpointer u)
{
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    char dir[TRK_PATH_LEN] = "";
    size_t dl;
    guint i;
    (void)u;
    if (!files) return;
    ed_dir(dir, sizeof dir);
    dl = strlen(dir);
    for (i = 0; i < g_list_model_get_n_items(files) && E.m->n < DK_MAX_SAMPLES; i++) {
        GFile *f = g_list_model_get_item(files, i);
        char *path = g_file_get_path(f);
        int note = DK_BASE_NOTE - 1, j;
        if (path) {
            dk_pad *p;
            for (j = 0; j < E.m->n; j++) if (E.m->pad[j].note > note) note = E.m->pad[j].note;
            if (note >= 127) { ed_status("no notes left above the last pad"); g_free(path); g_object_unref(f); break; }
            p = &E.m->pad[E.m->n++];
            memset(p, 0, sizeof *p);
            p->note = note + 1;
            /* From the set's own folder, by name; from anywhere else, by path. */
            if (dl && !strncmp(path, dir, dl) && path[dl] == '/')
                snprintf(p->file, sizeof p->file, "%s", path + dl + 1);
            else
                snprintf(p->file, sizeof p->file, "%s", path);
            E.dirty = 1;
            g_free(path);
        }
        g_object_unref(f);
    }
    g_object_unref(files);
    ed_fill();
}

static void ed_add(GtkButton *b, gpointer u)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *wav = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    char dir[TRK_PATH_LEN];
    (void)b; (void)u;
    if (ed_dir(dir, sizeof dir)) { g_object_unref(d); g_object_unref(wav); g_object_unref(filters); return; }
    gtk_file_filter_set_name(wav, "WAV files");
    gtk_file_filter_add_suffix(wav, "wav");
    gtk_file_filter_add_suffix(wav, "WAV");
    g_list_store_append(filters, wav);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    gtk_file_dialog_set_title(d, "Add WAVs");
    {
        GFile *start = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(d, start);
        g_object_unref(start);
    }
    gtk_file_dialog_open_multiple(d, GTK_WINDOW(E.win), NULL, ed_added, NULL);
    g_object_unref(wav);
    g_object_unref(filters);
    g_object_unref(d);
}

static void ed_save(GtkButton *b, gpointer u)
{
    char msg[TRK_PATH_LEN + 96], line[TRK_PATH_LEN + 64];
    const char *why;
    int i;
    (void)b; (void)u;
    if (!E.name[0] || !E.m->dir[0]) return;
    for (i = 0; i < E.m->n; i++)
        if (E.m->pad[i].note < 0) {
            snprintf(msg, sizeof msg, "row %d: not a note -- C-4, F#5, or 0-127", i + 1);
            ed_status(msg);
            return;
        }
    if ((why = drumkit_map_check(E.m))) { ed_status(why); return; }
    if (drumkit_map_save(E.m)) {
        snprintf(msg, sizeof msg, "could not save %s/kit.txt: %s", E.m->dir, strerror(errno));
        ed_status(msg);
        return;
    }
    E.m->mapped = 1;
    E.dirty = 0;
    trk_reload_sample_set(E.U->e, E.name);
    refresh_dests(E.U, 1);
    snprintf(line, sizeof line, "%s   (kit.txt)", E.m->dir);
    gtk_label_set_text(GTK_LABEL(E.dir), line);
    snprintf(msg, sizeof msg, "saved %s/kit.txt -- %d pads; tracks playing this set play it now",
             E.m->dir, E.m->n);
    ed_status(msg);
}

static gboolean ed_close_request(GtkWindow *w, gpointer u)
{
    (void)w; (void)u;
    if (!E.dirty) return FALSE;
    ed_ask_discard(-1);
    return TRUE;                                   /* the answer closes it */
}

static void ed_close(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    gtk_window_close(GTK_WINDOW(E.win));
}

static void ed_destroyed(GtkWidget *w, gpointer u)
{
    int i;
    (void)w; (void)u;
    for (i = 0; i < E.nsets; i++) g_free(E.setval[i]);
    for (i = 0; i < E.nchoice; i++) g_free(E.choice[i]);
    g_free(E.choice);
    free(E.m);
    /* Not when the editor is going down with its view's tab: the grid is
     * being destroyed too, and there is nothing to focus. */
    if (!E.U->closing) gtk_widget_grab_focus(E.U->area);
    memset(&E, 0, sizeof E);
}

static void on_edit_samples(GSimpleAction *a, GVariant *v, gpointer u)
{
    static char buf[32768];
    GtkWidget *box, *top, *sw, *row, *bt;
    GtkStringList *sl;
    char cur[TRK_PATH_LEN], *copy, *line, *save = NULL;
    int start = 0;
    ui *U = u;
    (void)a; (void)v;

    E.U = U;
    if (E.win) { gtk_window_present(GTK_WINDOW(E.win)); return; }
    if (!(E.m = calloc(1, sizeof *E.m))) return;
    trk_lock(U->e);
    snprintf(cur, sizeof cur, "%s", trk_song_of(U->e)->track[U->ed.track].samples);
    trk_unlock(U->e);

    /* The set the cursor's track plays, to begin with; any other from here. */
    sl = gtk_string_list_new(NULL);
    trk_list_sample_sets(U->e, buf, sizeof buf);
    copy = g_strdup(buf);
    for (line = strtok_r(copy, "\n", &save); line && E.nsets < MAXSAMP - 1;
         line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (cur[0] && (!strcmp(cur, line) || !strcmp(cur, tab + 1))) start = E.nsets;
        gtk_string_list_append(sl, line);
        E.setval[E.nsets++] = g_strdup(line);
    }
    g_free(copy);
    if (!E.nsets) { g_object_unref(sl); free(E.m); E.m = NULL; status(U, "no sample sets found"); return; }

    E.win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(E.win), "Edit Sample Set");
    gtk_window_set_transient_for(GTK_WINDOW(E.win), parent_window(U));
    gtk_window_set_modal(GTK_WINDOW(E.win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(E.win), 1180, 720);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);   gtk_widget_set_margin_bottom(box, 8);
    gtk_window_set_child(GTK_WINDOW(E.win), box);

    top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    E.sets = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);     /* takes sl */
    gtk_widget_set_hexpand(E.sets, TRUE);
    gtk_box_append(GTK_BOX(top), gtk_label_new("Set:"));
    gtk_box_append(GTK_BOX(top), E.sets);
    gtk_box_append(GTK_BOX(box), top);
    E.dir = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(E.dir), 0);
    gtk_label_set_selectable(GTK_LABEL(E.dir), TRUE);
    gtk_widget_add_css_class(E.dir, "dim-label");
    gtk_box_append(GTK_BOX(box), E.dir);

    E.sel = -1;
    E.curnote = -1;
    {
        GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *hint, *lb;
        lb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(bar), gtk_label_new("Typing keys from octave"));
        E.octave = gtk_spin_button_new_with_range(0, 8, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(E.octave), 4);
        g_signal_connect(E.octave, "value-changed", G_CALLBACK(octave_changed), NULL);
        gtk_box_append(GTK_BOX(bar), E.octave);
        hint = gtk_label_new(NULL);
        gtk_widget_set_hexpand(hint, TRUE);
        gtk_box_append(GTK_BOX(bar), hint);
        hint = gtk_button_new_with_label("Load a folder…");
        gtk_widget_set_tooltip_text(hint, "Put every WAV in a folder on keys, one after another, from the picked key (or after the last used one)");
        g_signal_connect(hint, "clicked", G_CALLBACK(kb_load_folder), NULL);
        gtk_box_append(GTK_BOX(bar), hint);
        gtk_box_append(GTK_BOX(lb), bar);
        hint = gtk_label_new("Pick a sample for a key from its drop-down, or drag one from the list onto a key. Click a key and press typing keys to hear them. Right-click a key for more.");
        gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_widget_add_css_class(hint, "dim-label");
        gtk_box_append(GTK_BOX(lb), hint);
        gtk_box_append(GTK_BOX(lb), kb_build());
        gtk_widget_set_size_request(lb, 420, -1);
        E.paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_start_child(GTK_PANED(E.paned), lb);
        gtk_paned_set_shrink_start_child(GTK_PANED(E.paned), FALSE);
        gtk_paned_set_resize_start_child(GTK_PANED(E.paned), TRUE);
        gtk_paned_set_position(GTK_PANED(E.paned), 520);
        gtk_widget_set_vexpand(E.paned, TRUE);
        gtk_box_append(GTK_BOX(box), E.paned);
    }

    E.grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(E.grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(E.grid), 2);
    sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), E.grid);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_paned_set_end_child(GTK_PANED(E.paned), sw);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    bt = gtk_button_new_with_label("Add WAVs…");
    g_signal_connect(bt, "clicked", G_CALLBACK(ed_add), NULL);
    gtk_box_append(GTK_BOX(row), bt);
    bt = gtk_label_new(NULL);
    gtk_widget_set_hexpand(bt, TRUE);
    gtk_box_append(GTK_BOX(row), bt);
    bt = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(bt, "suggested-action");
    g_signal_connect(bt, "clicked", G_CALLBACK(ed_save), NULL);
    gtk_box_append(GTK_BOX(row), bt);
    bt = gtk_button_new_with_label("Close");
    g_signal_connect(bt, "clicked", G_CALLBACK(ed_close), NULL);
    gtk_box_append(GTK_BOX(row), bt);
    gtk_box_append(GTK_BOX(box), row);
    E.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(E.status), 0);
    gtk_label_set_wrap(GTK_LABEL(E.status), TRUE);
    gtk_box_append(GTK_BOX(box), E.status);

    E.filling = 1;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(E.sets), (guint)start);
    E.filling = 0;
    g_signal_connect(E.sets, "notify::selected", G_CALLBACK(ed_set_picked), NULL);
    g_signal_connect(E.win, "close-request", G_CALLBACK(ed_close_request), NULL);
    g_signal_connect(E.win, "destroy", G_CALLBACK(ed_destroyed), NULL);
    ed_load(start);
    gtk_window_present(GTK_WINDOW(E.win));
}

/* Samples, beside the file buttons. */
static void samples_menu(GtkMenuButton *mb, gpointer u)
{
    GMenu *m = g_menu_new();
    (void)u;
    g_menu_append(m, "Load Sample Set…", "win.load-samples");
    g_menu_append(m, "Edit Sample Set…", "win.edit-samples");
    g_menu_append(m, "Audio Output…", "win.audio-output");
    gtk_menu_button_set_menu_model(mb, G_MENU_MODEL(m));
    g_object_unref(m);
}

static void update_title(ui *U)
{
    char title[4200];
    const char *base = U->path[0] ? strrchr(U->path, '/') : NULL;
    if (!U->win) return;    /* embedded: the host names its own frames */
    snprintf(title, sizeof title, "%s — tracker (%s)",
             U->path[0] ? (base ? base + 1 : U->path) : "untitled", trk_client_name(U->e));
    gtk_window_set_title(GTK_WINDOW(U->win), title);
}

static int dirty(ui *U)
{
    int d;
    trk_lock(U->e);
    d = memcmp(trk_song_of(U->e), U->saved, sizeof(trk_song)) != 0;
    trk_unlock(U->e);
    return d;
}

static void reset_view(ui *U)
{
    trk_route(U->e);
    sync_from_song(U);
    refresh_dests(U, 1);
    update_size(U);
    redraw(U);
    update_title(U);
}

static int write_to(ui *U, const char *path)
{
    char err[512];
    int r;
    /* A copy under the lock, the file written from it after: the lock is the
     * scheduler's and the note path's, and a large song takes tens of
     * milliseconds to write. */
    trk_song *snap = malloc(sizeof *snap);
    if (!snap) { status(U, "out of memory"); return -1; }
    trk_lock(U->e);
    memcpy(snap, trk_song_of(U->e), sizeof *snap);
    trk_unlock(U->e);
    r = trk_song_save(snap, path, err, sizeof err);
    if (!r) memcpy(U->saved, snap, sizeof(trk_song));
    free(snap);
    if (r) { status(U, err); return -1; }
    if (path != U->path) snprintf(U->path, sizeof U->path, "%s", path);
    update_title(U);
    {
        char msg[4200];
        snprintf(msg, sizeof msg, "Saved %s", path);
        status(U, msg);
    }
    return 0;
}

static int open_path(ui *U, const char *path)
{
    char err[512];
    trk_song *tmp = malloc(sizeof *tmp);
    if (!tmp) return -1;
    if (trk_song_load(tmp, path, err, sizeof err)) {
        status(U, err);
        free(tmp);
        return -1;
    }
    trk_stop(U->e);
    trk_undo_clear(U->e);   /* a loaded song starts with no history */
    trk_unroute_sinks(U->e);   /* the engine's routing and the new song's sink names agree */
    trk_lock(U->e);
    memcpy(trk_song_of(U->e), tmp, sizeof *tmp);
    trk_unlock(U->e);
    memcpy(U->saved, tmp, sizeof *tmp);
    snprintf(U->path, sizeof U->path, "%s", path);
    trk_editor_init(&U->ed);
    trk_set_bpm(U->e, tmp->bpm);
    free(tmp);
    reset_view(U);
    if (U->song_opened) U->song_opened(U->song_opened_ud);
    return 0;
}

/* ------------------------------------------------------------------ keys */

static void cursor_moved(ui *U)
{
    GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(U->scroll));
    GtkAdjustment *ha = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(U->scroll));
    double y = U->ed.row * U->ch, x = gutter(U) + U->ed.track * colwidth(U);
    double vv = gtk_adjustment_get_value(va), vp = gtk_adjustment_get_page_size(va);
    double hv = gtk_adjustment_get_value(ha), hp = gtk_adjustment_get_page_size(ha);

    trk_record_arm(U->e, U->ed.track);   /* the MIDI input plays and records on the cursor's track */
    U->loading = 1;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->pattern), U->ed.pattern);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->rows), cur_rows(U));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(U->oct[U->ed.track]), (guint)U->ed.octave);
    g_idle_add(refresh_cheat_idle, U);   /* [ and ] change it too */
    if (U->part_name) {
        char name[TRK_NAME_LEN];
        trk_lock(U->e);
        snprintf(name, sizeof name, "%s", trk_song_of(U->e)->pattern[U->ed.pattern].name);
        trk_unlock(U->e);
        gtk_editable_set_text(GTK_EDITABLE(U->part_name), name);
    }
    U->loading = 0;
    if (y < vv + U->ch * 2)          gtk_adjustment_set_value(va, y - U->ch * 2);
    else if (y > vv + vp - U->ch * 3) gtk_adjustment_set_value(va, y - vp + U->ch * 3);
    if (x < hv)                      gtk_adjustment_set_value(ha, x - gutter(U));
    else if (x + colwidth(U) > hv + hp) gtk_adjustment_set_value(ha, x + colwidth(U) - hp);
}

static int translate(guint kv, GdkModifierType st)
{
    gunichar c;
    /* Shift with the arrows selects; Ctrl with C, X, V and A is the clipboard.
     * Furnace's keys, where they fit: Ctrl+Y redo, Ctrl+Shift+V paste mix,
     * Ctrl+F1-F4 transpose, Alt+F9/F10 mute and solo, Shift+PgUp/PgDn a page
     * of selection, the keypad's * and / the octave. */
    if (st & GDK_ALT_MASK)
        switch (kv) {
        case GDK_KEY_F9:  return (st & GDK_SHIFT_MASK) ? TRK_K_UNMUTE_ALL : TRK_K_MUTE_TRACK;
        case GDK_KEY_F10: return TRK_K_SOLO_TRACK;
        default: break;
        }
    if (st & GDK_SHIFT_MASK)
        switch (kv) {
        case GDK_KEY_Up:    return TRK_K_SEL_UP;
        case GDK_KEY_Down:  return TRK_K_SEL_DOWN;
        case GDK_KEY_Left:  return TRK_K_SEL_LEFT;
        case GDK_KEY_Right: return TRK_K_SEL_RIGHT;
        case GDK_KEY_Page_Up:   return TRK_K_SEL_PGUP;
        case GDK_KEY_Page_Down: return TRK_K_SEL_PGDN;
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: if (!(st & GDK_CONTROL_MASK)) return TRK_K_PLAY_FROM_CURSOR; break;
        default: break;
        }
    if (st & GDK_CONTROL_MASK)
        switch (kv) {
        case GDK_KEY_c: case GDK_KEY_C: return TRK_K_COPY;
        case GDK_KEY_x: case GDK_KEY_X: return TRK_K_CUT;
        case GDK_KEY_v: case GDK_KEY_V: return (st & GDK_SHIFT_MASK) ? TRK_K_PASTE_MIX : TRK_K_PASTE;
        case GDK_KEY_a: case GDK_KEY_A: return TRK_K_SEL_ALL;
        case GDK_KEY_z: case GDK_KEY_Z: return (st & GDK_SHIFT_MASK) ? TRK_K_REDO : TRK_K_UNDO;
        case GDK_KEY_y: case GDK_KEY_Y: return TRK_K_REDO;
        case GDK_KEY_F1: return TRK_K_TRANSPOSE_DOWN;
        case GDK_KEY_F2: return TRK_K_TRANSPOSE_UP;
        case GDK_KEY_F3: return TRK_K_TRANSPOSE_OCT_DOWN;
        case GDK_KEY_F4: return TRK_K_TRANSPOSE_OCT_UP;
        case GDK_KEY_KP_Multiply: return TRK_K_STEP_UP;
        case GDK_KEY_KP_Divide:   return TRK_K_STEP_DOWN;
        case GDK_KEY_Insert:      return TRK_K_TRACK_ADD;
        case GDK_KEY_Delete:      return TRK_K_TRACK_REMOVE;
        default: break;
        }
    switch (kv) {
    case GDK_KEY_Up:        return TRK_K_UP;
    case GDK_KEY_Down:      return TRK_K_DOWN;
    case GDK_KEY_Left:      return TRK_K_LEFT;
    case GDK_KEY_Right:     return TRK_K_RIGHT;
    case GDK_KEY_Page_Up:   return TRK_K_PGUP;
    case GDK_KEY_Page_Down: return TRK_K_PGDN;
    case GDK_KEY_Home:      return TRK_K_HOME;
    case GDK_KEY_End:       return TRK_K_END;
    case GDK_KEY_Tab:       return (st & GDK_SHIFT_MASK) ? TRK_K_BACKTAB : TRK_K_TAB;
    case GDK_KEY_ISO_Left_Tab: return TRK_K_BACKTAB;
    case GDK_KEY_Delete:    return TRK_K_DELETE;
    case GDK_KEY_BackSpace: return TRK_K_BACKSPACE;
    case GDK_KEY_Insert:    return TRK_K_INSERT;
    case GDK_KEY_F5:        return TRK_K_PLAY_SONG;
    case GDK_KEY_F6:        return TRK_K_PLAY_PATTERN;
    case GDK_KEY_F7:        return TRK_K_RECORD;
    case GDK_KEY_F8:        return TRK_K_STOP;
    case GDK_KEY_space:     return TRK_K_EDIT;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return TRK_K_TOGGLE;
    case GDK_KEY_KP_Multiply: return TRK_K_OCT_UP;
    case GDK_KEY_KP_Divide:   return TRK_K_OCT_DOWN;
    case GDK_KEY_bracketleft:  return TRK_K_OCT_DOWN;
    case GDK_KEY_bracketright: return TRK_K_OCT_UP;
    case GDK_KEY_minus:     return TRK_K_PAT_PREV;
    case GDK_KEY_equal:     return TRK_K_PAT_NEXT;
    case GDK_KEY_grave:     return TRK_K_EDIT;
    default: break;
    }
    if (st & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return -1;
    c = gdk_keyval_to_unicode(kv);
    if (c > 0x20 && c < 0x7f) return (int)g_unichar_tolower(c);
    return -1;
}

/* Two keyvals that are one key under different modifiers -- '2' and '@' --
 * map to the same keycode. How a release finds its press when Shift changed
 * in between and there is no hardware code to go by. */
static int same_key(ui *U, guint a, guint b)
{
    GdkKeymapKey *ka, *kb;
    int na, nb, i, j, same = 0;
    GdkDisplay *d = gtk_widget_get_display(U->area);
    if (!gdk_display_map_keyval(d, a, &ka, &na)) return 0;
    if (gdk_display_map_keyval(d, b, &kb, &nb)) {
        for (i = 0; i < na && !same; i++)
            for (j = 0; j < nb && !same; j++)
                same = ka[i].keycode == kb[j].keycode;
        g_free(kb);
    }
    g_free(ka);
    return same;
}

static gboolean on_key(GtkEventControllerKey *k, guint kv, guint code,
                       GdkModifierType st, gpointer u)
{
    ui *U = u;
    int key;
    (void)k; (void)code;
    if (kv == GDK_KEY_Escape || kv == GDK_KEY_F12) { trk_panic(U->e); return TRUE; }
    key = translate(kv, st);
    if (key < 0) return FALSE;
    if (key == TRK_K_TRACK_ADD)    { on_track_add(NULL, U); return TRUE; }
    if (key == TRK_K_TRACK_REMOVE) { on_track_remove(NULL, U); return TRUE; }
    /* A held key repeats; a note or a digit must not, or holding one down
     * writes it into every row the cursor passes. */
    if (key < 0x80) {
        unsigned slot = code > 0 && code < 256 ? code : 256 + (unsigned)key;
        if (U->down[slot]) return TRUE;
        U->down[slot] = (unsigned char)key;
        if (slot >= 256) U->downkv[key] = kv;   /* so the release finds it back */
    }
    /* On a sample-set track, what a typed note plays -- or that it plays
     * nothing, and where the set's samples are. */
    if (key < 0x80 && (U->ed.field == TRK_F_NOTE || !U->ed.edit) && trk_key_note(key, U->ed.octave) >= 0) {
        char what[TRK_PATH_LEN + 64], nn[5], msg[TRK_PATH_LEN + 96];
        int note = trk_key_note(key, U->ed.octave);
        int r = trk_sample_at(U->e, U->ed.track, note, what, sizeof what);
        drumkit_note_name(note, nn);
        if (r > 0) { snprintf(msg, sizeof msg, "%s  %s", nn, what); status(U, msg); }
        else if (r == 0) { snprintf(msg, sizeof msg, "no sample on %s: %s", nn, what); status(U, msg); }
    }
    if (trk_key(U->e, &U->ed, key)) {
        update_size(U);
        redraw(U);
        cursor_moved(U);
    }
    if (key == TRK_K_COPY || key == TRK_K_CUT || key == TRK_K_PASTE || key == TRK_K_PASTE_MIX) clip_status(U, key);
    if (key == TRK_K_MUTE_TRACK || key == TRK_K_SOLO_TRACK || key == TRK_K_UNMUTE_ALL) sync_mutes(U);
    if (key == TRK_K_STEP_UP || key == TRK_K_STEP_DOWN) {
        U->loading = 1;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->step), U->ed.step);
        U->loading = 0;
    }
    if (key == TRK_K_EDIT) {
        U->loading = 1;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(U->editbox), U->ed.edit);
        U->loading = 0;
        edit_shown(U);
    }
    return TRUE;
}

static void on_key_up(GtkEventControllerKey *k, guint kv, guint code,
                      GdkModifierType st, gpointer u)
{
    ui *U = u;
    gunichar c = 0;
    (void)k; (void)st;
    if (code > 0 && code < 256) {
        if (!U->down[code]) return;
        c = U->down[code];
        U->down[code] = 0;
    } else {
        /* No keycode to go by: the press this release ends is the one this
         * keyval -- or this key -- went down as. The release's own
         * character is no guide: Shift may have changed since the press
         * ('2' comes back up as '@'), and looking it up by character then
         * clears nothing and leaves the press's slot held -- the repeat
         * guard swallowing that key for the rest of the session. */
        int at = -1, i, lone = -1, nheld = 0;
        gunichar uc = gdk_keyval_to_unicode(kv);
        if (uc > 0x20 && uc < 0x7f) c = g_unichar_tolower(uc);
        for (i = 0; i < 128; i++) if (U->down[256 + i]) { lone = i; nheld++; }
        for (i = 0; i < 128 && at < 0; i++)
            if (U->down[256 + i] && U->downkv[i] == kv) at = i;
        /* Shifted since the press: another keyval, but the same key -- the
         * keycodes the two map to agree. */
        for (i = 0; i < 128 && at < 0; i++)
            if (U->down[256 + i] && same_key(U, U->downkv[i], kv)) at = i;
        if (at < 0 && c && U->down[256 + c]) at = c;   /* as translated */
        /* Every match failed: with just one fallback key held, this is its
         * release -- a backend without keymap levels can pair nothing.
         * Ending the wrong preview is harmless; a slot left held is not. */
        if (at < 0 && nheld == 1) at = lone;
        if (at >= 0) {
            c = U->down[256 + at];
            U->down[256 + at] = 0;
            U->downkv[at] = 0;
        } else {
            /* A press this window never saw: released in another window, or
             * before focus. The release-time character, as a last resort. */
            if (!c) return;
            U->down[256 + c] = 0;
        }
    }
    trk_key_release(U->e, &U->ed, (int)c);
}

/* A key released in another window never comes back here, and its preview
 * would sound until something else stopped it. */
static void on_focus_leave(GtkEventControllerFocus *f, gpointer u)
{
    ui *U = u;
    int t;
    (void)f;
    if (U->closing) return;      /* the tab is coming down: the engine may already be */
    for (t = 0; t < TRK_TRACKS; t++)
        if (U->ed.held[t]) { trk_preview_off(U->e, t); U->ed.held[t] = 0; }
    memset(U->down, 0, sizeof U->down);
    memset(U->downkv, 0, sizeof U->downkv);
}

/* Where a point falls: track and row, track -1 for the row numbers. */
static void cell_at(ui *U, double x, double y, int *t, int *r, int *field)
{
    int rows = cur_rows(U), c;
    *r = (int)y / U->ch;
    if (*r < 0) *r = 0;
    if (*r > rows - 1) *r = rows - 1;
    x -= gutter(U);
    if (x < 0) { *t = -1; *field = 0; return; }
    *t = (int)x / colwidth(U);
    if (*t > ntr(U) - 1) *t = ntr(U) - 1;
    c = ((int)x % colwidth(U)) / U->cw;
    *field = c < 4 ? TRK_F_NOTE : c < 7 ? TRK_F_VEL : c < 10 ? TRK_F_CC : TRK_F_VAL;
}

static void move_cursor(ui *U, int r, int t, int field)
{
    U->ed.row = r;
    U->ed.track = t;
    U->ed.field = field;
    U->ed.digit = 0;
    trk_lock(U->e);
    U->ed.octave = trk_song_of(U->e)->track[t].octave;   /* each track's own */
    trk_unlock(U->e);
}

/* Selecting: a click puts the cursor down and ends a selection; a drag
 * selects the block it covers; Shift-click grows the selection to the cell;
 * a click or drag on the row numbers selects whole rows. */
static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    int t, r, field;
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    ui *U = u;
    (void)n;
    gtk_widget_grab_focus(U->area);
    cell_at(U, x, y, &t, &r, &field);
    if (t < 0) {
        U->drag_rows = 1;
        U->drag_r = (st & GDK_SHIFT_MASK) && U->ed.sel ? U->ed.sel_r0 : r;
        trk_select(&U->ed, U->drag_r, 0, r, ntr(U) - 1);
        U->ed.track = 0;
    } else if (st & GDK_SHIFT_MASK) {
        if (!U->ed.sel) trk_select(&U->ed, U->ed.row, U->ed.track, U->ed.row, U->ed.track);
        trk_select(&U->ed, U->ed.sel_r0, U->ed.sel_t0, r, t);
        move_cursor(U, r, t, field);
        U->drag_rows = 0;
    } else {
        trk_select_none(&U->ed);
        move_cursor(U, r, t, field);
        U->drag_rows = 0;
        U->drag_r = r;
        U->drag_t = t;
    }
    redraw(U);
    cursor_moved(U);
}

static void on_drag(GtkGestureDrag *g, double dx, double dy, gpointer u)
{
    double x0, y0;
    int t, r, field;
    ui *U = u;
    if (!gtk_gesture_drag_get_start_point(g, &x0, &y0)) return;
    if (dx * dx + dy * dy < 16) return;                 /* a click, not a drag */
    cell_at(U, x0 + dx, y0 + dy, &t, &r, &field);
    if (U->drag_rows) {
        trk_select(&U->ed, U->drag_r, 0, r, ntr(U) - 1);
        U->ed.track = 0;
    } else {
        if (t < 0) t = 0;
        trk_select(&U->ed, U->drag_r, U->drag_t, r, t);
        move_cursor(U, r, t, U->ed.field);
    }
    redraw(U);
    cursor_moved(U);
}

static void clip_status(ui *U, int key)
{
    char msg[96];
    int rows = 0, tracks = 0;
    trk_clipboard(&rows, &tracks);
    if (!U->ed.edit && key != TRK_K_COPY) { status(U, "edit is off -- Space to edit, then cut or paste"); return; }
    snprintf(msg, sizeof msg, "%s %d row%s x %d track%s",
             key == TRK_K_COPY ? "copied" : key == TRK_K_CUT ? "cut" : "pasted",
             rows, rows > 1 ? "s" : "", tracks, tracks > 1 ? "s" : "");
    status(U, msg);
}

static void grid_key(ui *U, int key)
{
    trk_key(U->e, &U->ed, key);
    if (key == TRK_K_COPY || key == TRK_K_CUT || key == TRK_K_PASTE) clip_status(U, key);
    redraw(U);
    cursor_moved(U);
}

static void on_copy(GSimpleAction *a, GVariant *v, gpointer u)  { ui *U = u; (void)a; (void)v; grid_key(U, TRK_K_COPY); }
static void on_undo(GSimpleAction *a, GVariant *v, gpointer u)  { ui *U = u; (void)a; (void)v; grid_key(U, TRK_K_UNDO); }
static void on_cut(GSimpleAction *a, GVariant *v, gpointer u)   { ui *U = u; (void)a; (void)v; grid_key(U, TRK_K_CUT); }
static void on_paste(GSimpleAction *a, GVariant *v, gpointer u) { ui *U = u; (void)a; (void)v; grid_key(U, TRK_K_PASTE); }
static void on_selall(GSimpleAction *a, GVariant *v, gpointer u){ ui *U = u; (void)a; (void)v; grid_key(U, TRK_K_SEL_ALL); }
static void on_clear(GSimpleAction *a, GVariant *v, gpointer u)
{
    ui *U = u;
    (void)a; (void)v;
    if (!U->ed.edit) { status(U, "edit is off -- ` to edit, then clear"); return; }
    trk_clear_block(U->e, &U->ed);
    redraw(U);
}
static void on_selcol(GSimpleAction *a, GVariant *v, gpointer u)
{
    ui *U = u;
    (void)a; (void)v;
    trk_select(&U->ed, 0, U->ed.track, cur_rows(U) - 1, U->ed.track);
    U->ed.row = 0;
    redraw(U);
    cursor_moved(U);
}

/* Right-click: what can be done with the selection -- or, clicked outside
 * it, with the cell under the pointer. */
static void on_context(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    GMenu *m = g_menu_new(), *a = g_menu_new(), *b = g_menu_new();
    GtkWidget *pop;
    GdkRectangle at = { (int)x, (int)y, 1, 1 };
    int t, r, field, rows = 0, tracks = 0;
    char paste[64];
    ui *U = u;
    (void)g; (void)n;
    gtk_widget_grab_focus(U->area);
    cell_at(U, x, y, &t, &r, &field);
    if (t >= 0 && !trk_selected(&U->ed, r, t)) { trk_select_none(&U->ed); move_cursor(U, r, t, field); }
    if (t < 0 && !trk_selected(&U->ed, r, 0)) { trk_select(&U->ed, r, 0, r, ntr(U) - 1); U->ed.track = 0; }
    redraw(U);
    cursor_moved(U);

    trk_clipboard(&rows, &tracks);
    if (rows) snprintf(paste, sizeof paste, "Paste %d row%s x %d track%s",
                       rows, rows > 1 ? "s" : "", tracks, tracks > 1 ? "s" : "");
    else snprintf(paste, sizeof paste, "Paste");
    g_menu_append(a, "Undo", "win.undo");
    g_menu_append(a, "Copy", "win.copy");
    g_menu_append(a, "Cut", "win.cut");
    g_menu_append(a, paste, "win.paste");
    g_menu_append(a, "Clear", "win.clear");
    g_menu_append(b, "Select This Track's Column", "win.select-column");
    g_menu_append(b, "Select All", "win.select-all");
    g_menu_append_section(m, NULL, G_MENU_MODEL(a));
    g_menu_append_section(m, NULL, G_MENU_MODEL(b));
    {
        GAction *pa = g_action_map_lookup_action(G_ACTION_MAP(U->ag), "paste");
        if (pa) g_simple_action_set_enabled(G_SIMPLE_ACTION(pa), rows > 0 && U->ed.edit);
    }
    pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(m));
    gtk_widget_set_parent(pop, U->area);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &at);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
    g_object_unref(a); g_object_unref(b); g_object_unref(m);
}

static void transport(ui *U, int key)
{
    trk_key(U->e, &U->ed, key);
    gtk_widget_grab_focus(U->area);
}

/* ---------------------------------------------------------------- timers */

/* The Rec button says what the engine is doing: lit while a take runs, and
 * counting in while the click leads the take in. */
static void show_rec(ui *U)
{
    const int rs = trk_recording(U->e);
    if (!U->recbtn || rs == U->rec_shown) return;
    U->rec_shown = rs;
    gtk_button_set_label(GTK_BUTTON(U->recbtn), rs == 2 ? "● Count-in…" : rs ? "● Recording" : "● Rec");
    if (rs) gtk_widget_add_css_class(U->recbtn, "destructive-action");
    else gtk_widget_remove_css_class(U->recbtn, "destructive-action");
    if (rs == 1) status(U, "recording -- play the keys; F7 or Stop ends the take");
}

static gboolean follow_playback(gpointer u)
{
    ui *U = u;
    int o, p, r;
    show_rec(U);
    trk_position(U->e, &o, &p, &r);
    /* The part playing, marked; and, following, the one being edited. */
    if (o != U->part_playing || (o >= 0 && U->ed.follow && o != U->part_at)) {
        U->part_playing = o;
        if (o >= 0 && U->ed.follow) U->part_at = U->ed.part = o;
        refresh_parts(U);
    }
    if (p != U->play_pat || r != U->play_row) {
        U->play_pat = p;
        U->play_row = r;
        redraw(U);
    }
    if (p < 0 || !U->ed.follow) return G_SOURCE_CONTINUE;
    if (p != U->ed.pattern) {
        U->ed.pattern = p;
        update_size(U);
    }
    if (U->ed.row != r) {
        GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(U->scroll));
        U->ed.row = r;
        /* Centred, so what is coming is as visible as what has passed. */
        gtk_adjustment_set_value(va, r * U->ch - gtk_adjustment_get_page_size(va) / 2);
        U->loading = 1;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->pattern), p);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->rows), cur_rows(U));
        U->loading = 0;
    }
    return G_SOURCE_CONTINUE;
}

/* Every two seconds, so a window opened after the song was loaded is found
 * and connected without anything being clicked. */
static gboolean reroute(gpointer u)
{
    ui *U = u;
    trk_route(U->e);
    refresh_dests(U, 0);
    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- widgets */

static void on_bpm(GtkSpinButton *s, gpointer u)
{ ui *U = u; if (!U->loading) trk_set_bpm(U->e, gtk_spin_button_get_value(s)); }

static void on_lpb(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    guint i = gtk_drop_down_get_selected(d);
    ui *U = u;
    (void)ps;
    if (U->loading || i >= NLPB) return;
    trk_lock(U->e);
    trk_song_of(U->e)->lpb = k_lpbs[i];
    trk_unlock(U->e);
    redraw(U);
}

static void on_pattern(GtkSpinButton *s, gpointer u)
{
    ui *U = u;
    if (U->loading) return;
    U->ed.pattern = gtk_spin_button_get_value_as_int(s);
    if (U->ed.row >= cur_rows(U)) U->ed.row = cur_rows(U) - 1;
    U->loading = 1;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U->rows), cur_rows(U));
    U->loading = 0;
    update_size(U);
    redraw(U);
}

static void on_rows(GtkSpinButton *s, gpointer u)
{
    int r = gtk_spin_button_get_value_as_int(s);
    ui *U = u;
    if (U->loading) return;
    trk_lock(U->e);
    trk_song_of(U->e)->pattern[U->ed.pattern].rows = r;
    trk_unlock(U->e);
    if (U->ed.row >= r) U->ed.row = r - 1;
    update_size(U);
    redraw(U);
    refresh_parts(U);
}

/* The cheat sheet a moment later, not here: the box is also set from inside
 * sync_from_song, which holds the song's lock. */
static gboolean refresh_cheat_idle(gpointer u)
{
    if (!((ui *)u)->closing) refresh_cheat(u);      /* queued before the view was freed */
    return G_SOURCE_REMOVE;
}
static void on_step(GtkSpinButton *s, gpointer u)   { ui *U = u; U->ed.step = gtk_spin_button_get_value_as_int(s); }
static void on_follow(GtkCheckButton *c, gpointer u) { ui *U = u; U->ed.follow = gtk_check_button_get_active(c); }

static void on_volume(GtkRange *r, gpointer u)
{
    ui *U = u;
    if (U->loading) return;
    trk_lock(U->e);
    trk_song_of(U->e)->volume = (int)(gtk_range_get_value(r) + 0.5);
    trk_unlock(U->e);
}

/* Edit mode, said where it cannot be missed: off, the grid is not being
 * written to, however much the keys are played. */
static void edit_shown(ui *U)
{
    status(U, U->ed.edit ? "edit on: keys write into the pattern"
                     : "edit off: note keys only play -- ` to edit again");
    redraw(U);
}

static void on_editbox(GtkCheckButton *c, gpointer u)
{
    ui *U = u;
    if (U->loading) return;
    U->ed.edit = gtk_check_button_get_active(c);
    edit_shown(U);
    gtk_widget_grab_focus(U->area);
}

static void on_name(GtkEditable *e, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    int t = r->n;
    if (U->loading) return;
    trk_lock(U->e);
    snprintf(trk_song_of(U->e)->track[t].name, TRK_NAME_LEN, "%s", gtk_editable_get_text(e));
    trk_unlock(U->e);
}

static void on_dest(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    int t = r->n;
    guint i = gtk_drop_down_get_selected(d);
    const char *v, *tab;
    trk_track *k;
    (void)ps;
    if (U->loading || i >= (guint)U->ndest[t]) return;
    v = U->destval[t][i];
    /* An in-process tab: the shell owns the routing change. Picking a window
     * (or nowhere) again routes the track back off sinks first. */
    if (!strncmp(v, "sink\t", 5)) {
        if (U->has_sinks && U->sinks.picked) U->sinks.picked(U->sinks_ud, t, v + 5);
        update_states(U);
        return;
    }
    if (U->has_sinks && U->sinks.picked) U->sinks.picked(U->sinks_ud, t, NULL);
    tab = strchr(v, '\t');
    trk_lock(U->e);
    k = &trk_song_of(U->e)->track[t];
    snprintf(k->client, TRK_DEST_LEN, "%.*s", tab ? (int)(tab - v) : (int)strlen(v), v);
    snprintf(k->port, TRK_DEST_LEN, "%s", tab ? tab + 1 : "");
    trk_unlock(U->e);
    trk_route(U->e);
    update_states(U);
}

/* A track's octave from its own box; the toolbar's follows when it is the
 * cursor's track. */
static void on_oct(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    int t = r->n, o = (int)gtk_drop_down_get_selected(d);
    (void)ps;
    if (U->loading) return;
    /* The core moves the octave and the track's existing notes together
     * (±12 per step, clamped), under the engine lock. */
    trk_track_set_octave(U->e, t, o);
    if (t == U->ed.track) U->ed.octave = o;
    g_idle_add(refresh_cheat_idle, U);
    redraw(U);
    /* "oct 9" draws wider than "oct 0", and the box is not ellipsized. */
    schedule_refit(U);
    gtk_widget_grab_focus(U->area);
}

static void on_chan(GtkDropDown *d, GParamSpec *ps, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    int t = r->n;
    (void)ps;
    if (U->loading) return;
    trk_lock(U->e);
    trk_song_of(U->e)->track[t].channel = (int)gtk_drop_down_get_selected(d) & 15;
    trk_unlock(U->e);
    /* "ch 16" draws wider than "ch 1", and the box is not ellipsized. */
    schedule_refit(U);
}

/* The mute boxes show the song's mute state; a key (mute, solo, unmute all)
 * changes it behind them. */
static void sync_mutes(ui *U)
{
    int t;
    U->loading = 1;
    trk_lock(U->e);
    for (t = 0; t < TRK_TRACKS; t++)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(U->mute[t]), trk_song_of(U->e)->track[t].mute);
    trk_unlock(U->e);
    U->loading = 0;
}

static void on_mute(GtkCheckButton *c, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    int t = r->n;
    if (U->loading) return;
    trk_lock(U->e);
    trk_song_of(U->e)->track[t].mute = gtk_check_button_get_active(c);
    trk_unlock(U->e);
    redraw(U);
}

/* A destination is named in full in the list that drops down, and cut short
 * with an ellipsis in the closed box -- which has to keep to its track's
 * column, or the headers stop lining up with the grid under them. */
static void dest_setup(GtkSignalListItemFactory *f, GtkListItem *it, gpointer short_form)
{
    GtkWidget *l = gtk_label_new(NULL);
    (void)f;
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (short_form) {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(l), 6);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 6);
    }
    gtk_list_item_set_child(it, l);
}

static void dest_bind(GtkSignalListItemFactory *f, GtkListItem *it, gpointer u)
{
    GtkStringObject *o = gtk_list_item_get_item(it);
    (void)f; (void)u;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(it)), gtk_string_object_get_string(o));
}

static GtkWidget *spin(double lo, double hi, double step, int digits)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s), (guint)digits);
    return s;
}

static GtkWidget *labelled(const char *text, GtkWidget *w)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(b), gtk_label_new(text));
    gtk_box_append(GTK_BOX(b), w);
    return b;
}

/* ------------------------------------------------------------- the files */

static void close_confirmed(ui *U);
static void ensure_confirmed(ui *U);

/* What a save that was asked for on the way to something else goes on to:
 * the close it was asked for, a shell's ensure_saved step, or nothing. */
enum { SAVED_THEN_NOTHING, SAVED_THEN_CLOSE, SAVED_THEN_ENSURE };

static void saved_then(ui *U, int then)
{
    if (then == SAVED_THEN_CLOSE) close_confirmed(U);
    else if (then == SAVED_THEN_ENSURE) ensure_confirmed(U);
}

static void open_done(GObject *src, GAsyncResult *res, gpointer u)
{
    ui *U = u;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    {
        char *p = g_file_get_path(f);
        if (p) open_path(U, p);
        g_free(p);
    }
    g_object_unref(f);
}

static void save_done(GObject *src, GAsyncResult *res, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    int close_after = r->n;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *p;
    g_free(r);
    if (!f) return;
    p = g_file_get_path(f);
    if (p) {
        char path[4096];
        const char *base = strrchr(p, '/');
        snprintf(path, sizeof path, "%s%s", p, base && !strchr(base, '.') ? ".trk" : "");
        if (!write_to(U, path)) saved_then(U, close_after);
    }
    g_free(p);
    g_object_unref(f);
}

static GtkFileDialog *song_dialog(const char *title)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *ft = gtk_file_filter_new();
    GListStore *fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    gtk_file_dialog_set_title(d, title);
    gtk_file_filter_set_name(ft, "Tracker songs");
    gtk_file_filter_add_pattern(ft, "*.trk");
    g_list_store_append(fs, ft);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    g_object_unref(ft);
    g_object_unref(fs);
    return d;
}

static void save_as(ui *U, int close_after)
{
    GtkFileDialog *d = song_dialog("Save song");
    {   /* A song opened by a bare file name has no '/' in its path. */
        const char *slash = strrchr(U->path, '/');
        gtk_file_dialog_set_initial_name(d, !U->path[0] ? "song.trk" : slash ? slash + 1 : U->path);
    }
    {
        ui_ref *r = g_new(ui_ref, 1);
        r->U = U; r->n = close_after;
        gtk_file_dialog_save(d, parent_window(U), NULL, save_done, r);
    }
    g_object_unref(d);
}

/* File > Export MIDI: the song as a standard MIDI file, one track per
 * playing track, for a DAW to import. Not a save: the song's own file, its
 * dirty state and its title stay as they were. */
static void export_done(GObject *src, GAsyncResult *res, gpointer u)
{
    ui *U = u;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *p;
    if (!f) return;
    p = g_file_get_path(f);
    if (p) {
        char path[4096], err[256] = "";
        const char *base = strrchr(p, '/');
        int r;
        snprintf(path, sizeof path, "%s%s", p, base && !strchr(base, '.') ? ".mid" : "");
        trk_song *snap = malloc(sizeof *snap);        /* copied under the lock, written after */
        if (snap) {
            trk_lock(U->e);
            memcpy(snap, trk_song_of(U->e), sizeof *snap);
            trk_unlock(U->e);
            r = trk_song_export_midi(snap, path, err, sizeof err);
            free(snap);
        } else {
            r = -1;
            snprintf(err, sizeof err, "out of memory");
        }
        if (r) {
            char m[512];
            snprintf(m, sizeof m, "export failed: %s", err);
            status(U, m);
        } else {
            char m[4200];
            snprintf(m, sizeof m, "Exported %s", path);
            status(U, m);
        }
    }
    g_free(p);
    g_object_unref(f);
}

static void export_midi(ui *U)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *ft = gtk_file_filter_new();
    GListStore *fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    char name[512] = "song.mid", *dot;
    gtk_file_dialog_set_title(d, "Export MIDI");
    gtk_file_filter_set_name(ft, "MIDI files");
    gtk_file_filter_add_pattern(ft, "*.mid");
    g_list_store_append(fs, ft);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    if (U->path[0]) {
        const char *slash = strrchr(U->path, '/');
        snprintf(name, sizeof name, "%s", slash ? slash + 1 : U->path);
        if ((dot = strrchr(name, '.'))) *dot = 0;
        strncat(name, ".mid", sizeof name - strlen(name) - 1);
    }
    gtk_file_dialog_set_initial_name(d, name);
    gtk_file_dialog_save(d, parent_window(U), NULL, export_done, U);
    g_object_unref(ft);
    g_object_unref(fs);
    g_object_unref(d);
}

static void do_save(ui *U, int close_after)
{
    if (!U->path[0]) { save_as(U, close_after); return; }
    if (!write_to(U, U->path)) saved_then(U, close_after);
}

typedef enum { AFTER_NEW, AFTER_OPEN, AFTER_CLOSE, AFTER_ENSURE } after_t;

/* The close the flow has been building to: the standalone's window closes;
 * an embedded view answers the shell that asked through
 * trk_view_confirm_close, and the shell takes the tab down. */
static void close_confirmed(ui *U)
{
    void (*cb)(void *) = U->embed_close;
    U->closing = 1;
    if (cb) { U->embed_close = NULL; cb(U->embed_close_ud); }
    else gtk_window_close(GTK_WINDOW(U->win));
}

/* trk_view_ensure_saved's end: the way is clear, the shell goes on. Unlike a
 * close, the view stays exactly as it was -- `closing` is not touched. */
static void ensure_confirmed(ui *U)
{
    void (*cb)(void *) = U->embed_ensure;
    if (cb) { U->embed_ensure = NULL; cb(U->embed_ensure_ud); }
}

static void after_confirm(ui *U, after_t what)
{
    if (what == AFTER_CLOSE) { close_confirmed(U); return; }
    if (what == AFTER_ENSURE) { ensure_confirmed(U); return; }
    if (what == AFTER_NEW) {
        trk_stop(U->e);
        trk_undo_clear(U->e);   /* a new song starts with no history */
        trk_unroute_sinks(U->e);
        trk_lock(U->e);
        trk_song_init(trk_song_of(U->e));
        trk_unlock(U->e);
        trk_song_init(U->saved);
        U->path[0] = 0;
        trk_editor_init(&U->ed);
        trk_set_bpm(U->e, 120);
        reset_view(U);
        return;
    }
    {
        GtkFileDialog *d = song_dialog("Open song");
        gtk_file_dialog_open(d, parent_window(U), NULL, open_done, U);
        g_object_unref(d);
    }
}

static void confirm_done(GObject *src, GAsyncResult *res, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    after_t what = (after_t)r->n;
    int b = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    g_free(r);
    if (b == 1) after_confirm(U, what);                 /* discard */
    else if (b == 2) {                               /* save first */
        if (what == AFTER_CLOSE) do_save(U, SAVED_THEN_CLOSE);
        else if (what == AFTER_ENSURE) do_save(U, SAVED_THEN_ENSURE);
        else if (!U->path[0]) save_as(U, 0);
        else if (!write_to(U, U->path)) after_confirm(U, what);
    }
}

/* Asks only when there is something to lose. */
static void confirm_then(ui *U, after_t what)
{
    static const char *buttons[] = { "Cancel", "Discard", "Save", NULL };
    GtkAlertDialog *d;
    if (!dirty(U)) { after_confirm(U, what); return; }
    d = gtk_alert_dialog_new("Save changes to the song first?");
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 2);
    {
        ui_ref *r = g_new(ui_ref, 1);
        r->U = U; r->n = what;
        gtk_alert_dialog_choose(d, parent_window(U), NULL, confirm_done, r);
    }
    g_object_unref(d);
}

static gboolean on_close(GtkWindow *w, gpointer u)
{
    ui *U = u;
    (void)w;
    if (U->closing || !dirty(U)) { trk_stop(U->e); return FALSE; }
    confirm_then(U, AFTER_CLOSE);
    return TRUE;
}

/* A help text in a window of its own, in a fixed-width font: the tables in
 * it are aligned with spaces, and an alert dialog's proportional font would
 * pull them out of line. */
static void show_text(ui *U, const char *title, const char *text)
{
    GtkWidget *win = gtk_window_new(), *sw = gtk_scrolled_window_new(), *l = gtk_label_new(text);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent_window(U));
    gtk_window_set_default_size(GTK_WINDOW(win), 700, 560);
    gtk_widget_add_css_class(l, "monospace");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_margin_start(l, 14);
    gtk_widget_set_margin_end(l, 14);
    gtk_widget_set_margin_top(l, 12);
    gtk_widget_set_margin_bottom(l, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), l);
    gtk_window_set_child(GTK_WINDOW(win), sw);
    gtk_window_present(GTK_WINDOW(win));
}

static void show_columns(ui *U) { show_text(U, "tracker columns", trk_columns_help()); }
static void show_keys(ui *U)    { show_text(U, "tracker keys", k_keys_help); }

/* ---- adding and removing tracks ---- */

/* Everything that shows a track, again, after the tracks have moved. */
static void tracks_changed(ui *U, const char *msg)
{
    if (U->ed.track >= ntr(U)) U->ed.track = ntr(U) - 1;
    trk_select_none(&U->ed);
    sync_from_song(U);
    refresh_dests(U, 1);
    refresh_samples(U);
    update_size(U);
    schedule_refit(U);
    redraw(U);
    cursor_moved(U);
    status(U, msg);
}

static void on_track_add(GtkButton *b, gpointer u)
{
    ui *U = u;
    char msg[96];
    (void)b;
    if (trk_track_insert(U->e, U->ed.track + 1) == 0) {
        U->ed.track++;
        snprintf(msg, sizeof msg, "track %d added -- Ctrl+Z takes it back", U->ed.track + 1);
        tracks_changed(U, msg);
    } else {
        snprintf(msg, sizeof msg, "no room: a song has at most %d tracks", TRK_TRACKS);
        status(U, msg);
    }
    gtk_widget_grab_focus(U->area);
}

static void do_track_remove(ui *U, int t)
{
    char msg[96];
    if (trk_track_remove(U->e, t) == 0) {
        snprintf(msg, sizeof msg, "track %d removed -- Ctrl+Z brings it back", t + 1);
        tracks_changed(U, msg);
    } else {
        status(U, "a song keeps at least one track");
    }
    gtk_widget_grab_focus(U->area);
}

typedef struct { ui *U; int t; } rm_ask;

static void remove_answered(GObject *src, GAsyncResult *res, gpointer u)
{
    rm_ask *k = u;
    int b = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    if (b == 1 && !k->U->closing) do_track_remove(k->U, k->t);
    g_object_unref(src);
    g_free(k);
}

static void on_track_remove(GtkButton *b, gpointer u)
{
    ui *U = u;
    const int t = U->ed.track;
    (void)b;
    if (!trk_track_used(U->e, t)) { do_track_remove(U, t); return; }
    {   /* It holds notes: ask first. */
        static const char *buttons[] = { "Cancel", "Remove track", NULL };
        GtkAlertDialog *d;
        rm_ask *k = g_new0(rm_ask, 1);
        char head[96];
        k->U = U; k->t = t;
        snprintf(head, sizeof head, "Remove track %d?", t + 1);
        d = gtk_alert_dialog_new("%s", head);
        gtk_alert_dialog_set_detail(d, "Its notes go with it. Undo (Ctrl+Z) brings the track back.");
        gtk_alert_dialog_set_buttons(d, buttons);
        gtk_alert_dialog_set_cancel_button(d, 0);
        gtk_alert_dialog_set_default_button(d, 0);
        gtk_alert_dialog_choose(d, parent_window(U), NULL, remove_answered, k);
    }
}

/* ---- Samples > Audio Output ---- */

typedef struct {
    ui *U;
    GtkWidget *win, *list, *note;
    char names[24][TRK_DEST_LEN], labels[24][96];
    int n;
} audio_dlg;

static void audio_fill(audio_dlg *A)
{
    const char *cur = trk_audio_device(A->U->e);
    const char *now = *cur ? cur : "default";
    GtkWidget *c;
    int i;
    while ((c = gtk_widget_get_first_child(A->list))) gtk_list_box_remove(GTK_LIST_BOX(A->list), c);
    for (i = 0; i < A->n; i++) {
        char *label = g_strdup_printf("%s%s", A->labels[i], !strcmp(now, A->names[i]) ? "   ● in use" : "");
        GtkWidget *row = gtk_list_box_row_new(), *l = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
        g_object_set_data(G_OBJECT(row), "idx", GINT_TO_POINTER(i + 1));
        gtk_list_box_append(GTK_LIST_BOX(A->list), row);
        g_free(label);
    }
}

static void audio_row_activated(GtkListBox *b, GtkListBoxRow *row, gpointer u)
{
    audio_dlg *A = u;
    const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "idx")) - 1;
    int r;
    (void)b;
    if (i < 0 || i >= A->n) return;
    r = trk_audio_set_device(A->U->e, !strcmp(A->names[i], "default") ? "" : A->names[i]);
    gtk_label_set_text(GTK_LABEL(A->note), *trk_audio_status(A->U->e) ? trk_audio_status(A->U->e)
                                           : r ? "that device would not open" : "switched");
    status(A->U, gtk_label_get_text(GTK_LABEL(A->note)));
    audio_fill(A);
}

static void audio_gone(GtkWidget *w, gpointer u) { (void)w; g_free(u); }

static void show_audio_output(ui *U)
{
    audio_dlg *A = g_new0(audio_dlg, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *info, *sw, *intro;
    A->U = U;
    A->n = trk_audio_devices(A->names, A->labels, 24);
    A->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(A->win), "Audio output");
    gtk_window_set_transient_for(GTK_WINDOW(A->win), parent_window(U));
    gtk_window_set_default_size(GTK_WINDOW(A->win), 520, 420);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);   gtk_widget_set_margin_bottom(box, 12);
    intro = gtk_label_new("Where sample-set tracks play. Click one to switch to it; the choice is kept.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
    gtk_box_append(GTK_BOX(box), intro);
    A->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(A->list), GTK_SELECTION_SINGLE);
    g_signal_connect(A->list, "row-activated", G_CALLBACK(audio_row_activated), A);
    sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), A->list);
    gtk_box_append(GTK_BOX(box), sw);
    A->note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(A->note), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(A->note), TRUE);
    gtk_box_append(GTK_BOX(box), A->note);
    info = gtk_label_new("Synth tabs in the studio play through PipeWire, which also serves JACK programs "
                         "(pipewire-jack) and ALSA programs (pipewire-alsa). This list is for the tracker's own "
                         "sample playback.");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(info), TRUE);
    gtk_widget_add_css_class(info, "dim-label");
    gtk_box_append(GTK_BOX(box), info);
    audio_fill(A);
    g_signal_connect(A->win, "destroy", G_CALLBACK(audio_gone), A);
    gtk_window_set_child(GTK_WINDOW(A->win), box);
    gtk_window_present(GTK_WINDOW(A->win));
}

static void on_audio_output(GSimpleAction *a, GVariant *v, gpointer u) { (void)a; (void)v; show_audio_output(u); }

static void on_export_button(GtkButton *b, gpointer u) { (void)b; export_midi(u); }

/* Export the last take as a MIDI file: the notes as they were played, at
 * their exact times, not as they were rounded onto rows. */
static void take_done(GObject *src, GAsyncResult *res, gpointer u)
{
    ui *U = u;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *p;
    if (!f) return;
    p = g_file_get_path(f);
    if (p) {
        char path[4096], err[256] = "", m[4400];
        const char *base = strrchr(p, '/');
        snprintf(path, sizeof path, "%s%s", p, base && !strchr(base, '.') ? ".mid" : "");
        if (trk_take_export_midi(U->e, path, err, sizeof err)) snprintf(m, sizeof m, "take not exported: %s", err);
        else snprintf(m, sizeof m, "Exported the take to %s", path);
        status(U, m);
    }
    g_free(p);
    g_object_unref(f);
}

static void export_take(ui *U)
{
    GtkFileDialog *d;
    GtkFileFilter *ft;
    GListStore *fs;
    if (!trk_take_events(U->e)) { status(U, "nothing has been recorded yet -- F7 records a take"); return; }
    d = gtk_file_dialog_new();
    ft = gtk_file_filter_new();
    fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    gtk_file_dialog_set_title(d, "Export recorded take");
    gtk_file_filter_set_name(ft, "MIDI files");
    gtk_file_filter_add_pattern(ft, "*.mid");
    g_list_store_append(fs, ft);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    gtk_file_dialog_set_initial_name(d, "take.mid");
    gtk_file_dialog_save(d, parent_window(U), NULL, take_done, U);
    g_object_unref(ft);
    g_object_unref(fs);
    g_object_unref(d);
}

/* ---- recording options: each change applies at once ---- */

typedef struct {
    ui *U;
    GtkWidget *count, *metro, *quant, *noteoff, *monitor, *offset, *input;
    char inputs[64][TRK_DEST_LEN];
    int  ninputs, loading;
} recopts;

static void rec_apply(GtkWidget *w, gpointer u)
{
    recopts *R = u;
    trk_rec_opts o;
    (void)w;
    if (R->loading) return;
    o.count_in  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(R->count));
    o.metronome = gtk_check_button_get_active(GTK_CHECK_BUTTON(R->metro));
    o.quantize  = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(R->quant));
    o.note_off  = gtk_check_button_get_active(GTK_CHECK_BUTTON(R->noteoff));
    o.monitor   = gtk_check_button_get_active(GTK_CHECK_BUTTON(R->monitor));
    o.offset_ms = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(R->offset));
    trk_record_set(R->U->e, &o);
}

static void rec_input_picked(GObject *d, GParamSpec *ps, gpointer u)
{
    recopts *R = u;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
    char m[TRK_DEST_LEN + 64];
    (void)ps;
    if (R->loading) return;
    if (i == 0) { trk_input_connect(R->U->e, ""); status(R->U, "MIDI input: none"); return; }
    if (i - 1 < (guint)R->ninputs && trk_input_connect(R->U->e, R->inputs[i - 1]) == 0)
        snprintf(m, sizeof m, "MIDI input: %s", R->inputs[i - 1]);
    else
        snprintf(m, sizeof m, "could not connect that MIDI input");
    status(R->U, m);
}

static void rec_window_gone(GtkWidget *w, gpointer u)
{
    recopts *R = u;
    (void)w;
    R->U->recwin = NULL;
    g_free(R);
}

static GtkWidget *rec_row(GtkWidget *grid, int row, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
    return w;
}

static void show_rec_options(ui *U)
{
    recopts *R;
    GtkWidget *win, *grid, *help;
    static const char *const quant[] = { "Nearest row", "Row that is sounding", NULL };
    const char **ins;
    trk_rec_opts o;
    int i;

    if (U->recwin) { gtk_window_present(GTK_WINDOW(U->recwin)); return; }
    R = g_new0(recopts, 1);
    R->U = U;
    R->loading = 1;
    trk_record_get(U->e, &o);
    R->ninputs = trk_input_list(U->e, R->inputs, 64);
    ins = g_new0(const char *, (gsize)R->ninputs + 2);
    ins[0] = "(none)";
    for (i = 0; i < R->ninputs; i++) ins[i + 1] = R->inputs[i];

    win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Recording options");
    gtk_window_set_transient_for(GTK_WINDOW(win), parent_window(U));
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_widget_set_margin_start(grid, 16); gtk_widget_set_margin_end(grid, 16);
    gtk_widget_set_margin_top(grid, 14);   gtk_widget_set_margin_bottom(grid, 14);

    R->count = rec_row(grid, 0, "Count-in (bars)", gtk_spin_button_new_with_range(0, 4, 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(R->count), o.count_in);
    R->metro = rec_row(grid, 1, "Metronome click", gtk_check_button_new());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(R->metro), o.metronome);
    R->quant = rec_row(grid, 2, "Notes go to", gtk_drop_down_new_from_strings(quant));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(R->quant), (guint)o.quantize);
    R->noteoff = rec_row(grid, 3, "Write === when a key is let go", gtk_check_button_new());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(R->noteoff), o.note_off);
    R->offset = rec_row(grid, 4, "Keyboard timing offset (ms)", gtk_spin_button_new_with_range(-200, 200, 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(R->offset), o.offset_ms);
    R->input = rec_row(grid, 5, "MIDI input", gtk_drop_down_new_from_strings(ins));
    R->monitor = rec_row(grid, 6, "Hear the MIDI input on the cursor's track", gtk_check_button_new());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(R->monitor), o.monitor);
    for (i = 0; i < R->ninputs; i++)
        if (!strcmp(R->inputs[i], trk_input_connected(U->e))) gtk_drop_down_set_selected(GTK_DROP_DOWN(R->input), (guint)i + 1);
    help = gtk_label_new("A note you play is written to the row it falls on. 'Nearest row' rounds a\n"
                         "note struck a little before the next row up to it. The offset places keyboard\n"
                         "notes earlier (positive) or later, to make up for a slow keyboard or screen.\n"
                         "A MIDI input is timed by the sequencer, so it needs no offset.");
    gtk_label_set_xalign(GTK_LABEL(help), 0.0f);
    gtk_widget_add_css_class(help, "dim-label");
    gtk_grid_attach(GTK_GRID(grid), help, 0, 7, 2, 1);

    g_signal_connect(R->count, "value-changed", G_CALLBACK(rec_apply), R);
    g_signal_connect(R->offset, "value-changed", G_CALLBACK(rec_apply), R);
    g_signal_connect(R->metro, "toggled", G_CALLBACK(rec_apply), R);
    g_signal_connect(R->noteoff, "toggled", G_CALLBACK(rec_apply), R);
    g_signal_connect(R->monitor, "toggled", G_CALLBACK(rec_apply), R);
    g_signal_connect(R->quant, "notify::selected", G_CALLBACK(rec_apply), R);
    g_signal_connect(R->input, "notify::selected", G_CALLBACK(rec_input_picked), R);
    g_signal_connect(win, "destroy", G_CALLBACK(rec_window_gone), R);
    g_free((gpointer)ins);
    gtk_window_set_child(GTK_WINDOW(win), grid);
    U->recwin = win;
    R->loading = 0;
    gtk_window_present(GTK_WINDOW(win));
}

static void on_rec_button(GtkButton *b, gpointer u)
{
    ui *U = u;
    (void)b;
    trk_key(U->e, &U->ed, TRK_K_RECORD);
    show_rec(U);
    gtk_widget_grab_focus(U->area);
}
static void on_rec_options(GtkButton *b, gpointer u) { (void)b; show_rec_options(u); }

static void on_button(GtkButton *b, gpointer u)
{
    ui_ref *r = u;
    ui *U = r->U;
    (void)b;
    switch (r->n) {
    case 0: transport(U, TRK_K_PLAY_SONG); break;
    case 1: transport(U, TRK_K_PLAY_PATTERN); break;
    case 2: transport(U, TRK_K_STOP); break;
    case 3: trk_panic(U->e); gtk_widget_grab_focus(U->area); break;
    case 4: confirm_then(U, AFTER_NEW); break;
    case 5: confirm_then(U, AFTER_OPEN); break;
    case 6: do_save(U, 0); break;
    case 7: save_as(U, 0); break;
    case 8: show_keys(U); break;
    }
}

/* ----------------------------------------------------------- the window */

static void measure_font(ui *U)
{
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    PangoContext *pc = pango_font_map_create_context(fm);
    PangoLayout *l = pango_layout_new(pc);
    PangoRectangle ink, log;
    U->font = pango_font_description_from_string("Monospace 10");
    pango_layout_set_font_description(l, U->font);
    pango_layout_set_text(l, "0000000000", -1);
    pango_layout_get_pixel_extents(l, &ink, &log);
    U->cw = log.width / 10;
    U->ch = log.height + 2;
    U->asc = PANGO_PIXELS(pango_layout_get_baseline(l));
    g_object_unref(l);
    g_object_unref(pc);
}

/* ---------------------------------------------------------------- parts
 *
 * The song is its parts in order -- a part is a pattern, and plays as often
 * as it is in the list. Picking one edits it; the buttons change the order
 * through trk_order_*, as the Qt window's do. */

static void edit_part(ui *U, int r)
{
    const trk_song *s = trk_song_of(U->e);
    int p;
    trk_lock(U->e);
    p = r >= 0 && r < s->norder ? s->order[r] : -1;
    trk_unlock(U->e);
    if (p < 0) return;
    U->part_at = r;
    U->ed.part = r;
    U->ed.pattern = p;
    sync_from_song(U);
    update_size(U);
    redraw(U);
    gtk_widget_grab_focus(U->area);
}

static void refresh_parts(ui *U)
{
    const trk_song *s = trk_song_of(U->e);
    GtkWidget *c;
    int i, n;

    if (!U->parts) return;
    U->filling_parts = 1;
    while ((c = gtk_widget_get_first_child(U->parts))) gtk_list_box_remove(GTK_LIST_BOX(U->parts), c);
    trk_lock(U->e);
    n = s->norder;
    if (U->part_at >= n) U->part_at = n - 1;
    if (U->part_at < 0) U->part_at = 0;
    for (i = 0; i < n; i++) {
        char lbl[TRK_NAME_LEN + 8], *text, tip[64];
        GtkWidget *l;
        trk_part_label(s, s->order[i], lbl);
        text = g_markup_printf_escaped(i == U->part_playing ? "<b>%2d  %s  (%d)   ▶</b>" : "%2d  %s  (%d)",
                                       i + 1, lbl, s->pattern[s->order[i]].rows);
        l = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(l), text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        snprintf(tip, sizeof tip, "pattern %d, %d rows", s->order[i], s->pattern[s->order[i]].rows);
        gtk_widget_set_tooltip_text(l, tip);
        gtk_list_box_append(GTK_LIST_BOX(U->parts), l);
        g_free(text);
    }
    trk_unlock(U->e);
    gtk_list_box_select_row(GTK_LIST_BOX(U->parts),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(U->parts), U->part_at));
    U->filling_parts = 0;
}

static void on_part_selected(GtkListBox *b, GtkListBoxRow *row, gpointer u)
{
    ui *U = u;
    (void)b;
    if (U->filling_parts || !row) return;
    edit_part(U, gtk_list_box_row_get_index(row));
}

static void on_part_name(GtkEditable *ed, gpointer u)
{
    ui *U = u;
    if (U->loading) return;
    trk_lock(U->e);
    snprintf(trk_song_of(U->e)->pattern[U->ed.pattern].name, TRK_NAME_LEN, "%s",
             gtk_editable_get_text(ed));
    trk_unlock(U->e);
    refresh_parts(U);
}

static void on_part_button(GtkButton *b, gpointer u)
{
    ui_ref *ref = u;
    ui *U = ref->U;
    int what = ref->n, at = U->part_at, r = -1, cur;
    (void)b;
    trk_lock(U->e);
    cur = trk_song_of(U->e)->order[at];
    trk_unlock(U->e);
    switch (what) {
    case 0: case 1: {
        int p = trk_pattern_new(U->e, what == 1 ? cur : -1);
        if (p < 0) { status(U, "every pattern is in use"); return; }
        r = trk_order_insert(U->e, at, p);
        break;
    }
    case 2: r = trk_order_insert(U->e, at, cur); break;
    case 3:
        r = trk_order_remove(U->e, at);
        if (r < 0) status(U, "a song keeps at least one part");
        break;
    case 4: r = trk_order_move(U->e, at, -1); break;
    case 5: r = trk_order_move(U->e, at, +1); break;
    }
    if (r >= 0) U->part_at = r;
    refresh_parts(U);
    edit_part(U, U->part_at);
}

static GtkWidget *build_parts(ui *U)
{
    static const struct { const char *label, *tip; } bs[] = {
        { "New",    "Add a new, empty part after this one" },
        { "Copy",   "Add a copy of this part after it, to change" },
        { "Again",  "Play this same part again after it" },
        { "Remove", "Take this part out of the song (it is kept, and comes back with Again)" },
        { "▲",      "Move this part earlier" },
        { "▼",      "Move this part later" },
    };
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *l, *sw, *grid;
    int i;

    gtk_widget_set_size_request(panel, 200, -1);
    /* Set, so the name box stretching within it does not stretch the panel. */
    gtk_widget_set_hexpand(panel, FALSE);
    gtk_widget_set_margin_end(panel, 6);
    l = gtk_label_new("Parts");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(panel), l);
    U->part_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(U->part_name), "name this part");
    gtk_widget_set_tooltip_text(U->part_name, "The name of the part being edited -- Intro, Verse, Chorus");
    {   /* Its length beside its name: how many rows this part has. */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_hexpand(U->part_name, TRUE);
        gtk_editable_set_width_chars(GTK_EDITABLE(U->part_name), 6);
        gtk_widget_set_tooltip_text(U->rows, "How many rows this part has");
        gtk_box_append(GTK_BOX(row), U->part_name);
        gtk_box_append(GTK_BOX(row), U->rows);
        gtk_box_append(GTK_BOX(panel), row);
    }
    U->parts = gtk_list_box_new();
    gtk_widget_set_tooltip_text(U->parts, "The song, in order. Click a part to edit it");
    sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), U->parts);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(panel), sw);
    grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    for (i = 0; i < 6; i++) {
        GtkWidget *b = gtk_button_new_with_label(bs[i].label);
        gtk_widget_set_tooltip_text(b, bs[i].tip);
        gtk_widget_set_focus_on_click(b, FALSE);
        g_signal_connect(b, "clicked", G_CALLBACK(on_part_button),
                             ui_ref_new(U, i, G_OBJECT(b)));
        gtk_grid_attach(GTK_GRID(grid), b, i % 3, i / 3, 1, 1);
    }
    gtk_box_append(GTK_BOX(panel), grid);
    g_signal_connect(U->parts, "row-selected", G_CALLBACK(on_part_selected), U);
    g_signal_connect(U->part_name, "changed", G_CALLBACK(on_part_name), U);
    return panel;
}

/* The grid's columns as wide as the headers really drew, so each header
 * sits over its own column. Grow-only: a header that drew narrower than the
 * column was fitted for is not what the fit is for. */
static gboolean refit_columns(gpointer u)
{
    ui *U = u;
    int t, w = U->colw;
    U->t_refit = 0;               /* it ran: schedule_refit can ask again */
    for (t = 0; t < TRK_TRACKS; t++) {
        int got = gtk_widget_get_width(U->headbox[t]);
        if (got + 6 > w) w = got + 6;
    }
    if (w != U->colw) {
        U->colw = w;
        for (t = 0; t < TRK_TRACKS; t++) gtk_widget_set_size_request(U->headbox[t], U->colw - 6, -1);
        update_size(U);
        redraw(U);
    }
    return G_SOURCE_REMOVE;
}

/* The refit, once, however often it is asked for before it runs. A header
 * box can outgrow its column only after something it holds changes -- the
 * view first drawing under its theme (a theme's drop-downs measure narrower
 * before they are styled than they draw), a destination model assigned after
 * the first fit, a channel or octave picked whose closed box draws wider --
 * and each of those asks. The 200 ms is the one the standalone always used,
 * so a burst of asks settles into one re-measure. */
static void schedule_refit(ui *U)
{
    if (!U->t_refit) U->t_refit = g_timeout_add(200, refit_columns, U);
}

/* Shown -- a window presented, or a notebook page switched to: the note keys
 * live on the drawing area, so it takes the focus. A moment after the map:
 * a notebook moving to a page pulls the focus onto its tab as the switch
 * finishes, and only a grab after that sticks. */
static gboolean grab_area_idle(gpointer u)
{
    ui *U = u;
    if (!U->closing && U->view && gtk_widget_get_mapped(U->view))
        gtk_widget_grab_focus(U->area);
    return G_SOURCE_REMOVE;
}

static void on_view_map(GtkWidget *w, gpointer u)
{
    (void)w;
    g_idle_add(grab_area_idle, u);
}

/* The tracker, whole, as one widget a host packs -- standalone a window's
 * child, in a shell a notebook page. The commands the menus and F1 name go
 * on the view itself under the "win" prefix, so two views in one process
 * (even one window) each dispatch their own. */
static void on_view_destroy(GtkWidget *w, gpointer u);

static GtkWidget *tracker_view_new(ui *U)
{
    static const char *bnames[] = { "▶ Song", "▶ Pattern", "■ Stop", "Panic",
                                    "New", "Open…", "Save", "Save As…", "Keys" };
    static const char *btips[] = {
        "Play the song from the order entry holding this pattern (F5)",
        "Loop this pattern (F6)",
        "Stop and release every note (F8)",
        "Release every note on every track, playing or not (Escape)",
        "Start an empty song", "Open a song", "Save the song (to its file)",
        "Save the song to a new file", "What every key does" };
    static const GActionEntry acts[] = {
        { "keys",         on_keys,         NULL, NULL, NULL, {0} },
        { "columns",      on_columns,      NULL, NULL, NULL, {0} },
        { "cheat",        on_cheat,        NULL, NULL, NULL, {0} },
        { "load-samples", on_load_samples, NULL, NULL, NULL, {0} },
        { "edit-samples", on_edit_samples, NULL, NULL, NULL, {0} },
        { "audio-output", on_audio_output, NULL, NULL, NULL, {0} },
        { "copy",         on_copy,         NULL, NULL, NULL, {0} },
        { "undo",         on_undo,         NULL, NULL, NULL, {0} },
        { "cut",          on_cut,          NULL, NULL, NULL, {0} },
        { "paste",        on_paste,        NULL, NULL, NULL, {0} },
        { "clear",        on_clear,        NULL, NULL, NULL, {0} },
        { "select-column", on_selcol,      NULL, NULL, NULL, {0} },
        { "select-all",   on_selall,       NULL, NULL, NULL, {0} },
    };
    const char *lpbn[NLPB + 1], *chans[17];
    char lpbs[NLPB][16], chs[16][8];
    GtkWidget *boxes[TRK_TRACKS];
    GtkWidget *v, *bar, *orow, *head, *gl;
    GtkEventController *kc, *fc;
    GtkGesture *click;
    int i, t;

    measure_font(U);
    for (i = 0; i < 16; i++) { snprintf(chs[i], sizeof chs[i], "ch %d", i + 1); chans[i] = chs[i]; }
    chans[16] = NULL;

    U->view = v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(v, 6);
    gtk_widget_set_margin_end(v, 6);
    gtk_widget_set_margin_top(v, 6);
    U->ag = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(U->ag), acts, G_N_ELEMENTS(acts), U);
    gtk_widget_insert_action_group(v, "win", G_ACTION_GROUP(U->ag));
    /* F1, once an application accel on "win.cheat": a shortcut on the view
     * fires wherever the focus is inside it, standalone or embedded. */
    {
        GtkEventController *sc = gtk_shortcut_controller_new();
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
            gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F1, 0),
                             gtk_named_action_new("win.cheat")));
        gtk_widget_add_controller(v, sc);
    }

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (i = 0; i < 9; i++) {
        GtkWidget *b;
        /* Embedded, the shell's menu bar has New, Open, Save, Save As, Export
         * MIDI, Samples and Help; only the transport stays on the toolbar. */
        if (U->embedded && i >= 4) continue;
        if (i == 8) {
            /* Help, where Keys was: Keys and the cheat sheet. */
            GtkWidget *mb = gtk_menu_button_new();
            gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "Help");
            gtk_widget_set_tooltip_text(mb, "What every key does, and the cheat sheet (F1)");
            gtk_widget_set_focus_on_click(mb, FALSE);
            gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(mb), help_menu, NULL, NULL);
            gtk_box_append(GTK_BOX(bar), mb);
            continue;
        }
        b = gtk_button_new_with_label(bnames[i]);
        gtk_widget_set_tooltip_text(b, btips[i]);
        gtk_widget_set_focus_on_click(b, FALSE);
        g_signal_connect(b, "clicked", G_CALLBACK(on_button), ui_ref_new(U, i, G_OBJECT(b)));
        gtk_box_append(GTK_BOX(bar), b);
        if (i == 3) {
            GtkWidget *rb = gtk_button_new_with_label("● Rec"), *ob = gtk_button_new_with_label("Rec…");
            U->recbtn = rb;
            gtk_widget_set_tooltip_text(rb, "Record: play, and write the notes you play into the pattern, on the "
                                            "cursor's track (F7)");
            gtk_widget_set_tooltip_text(ob, "Recording options: count-in, metronome, quantizing, MIDI input");
            gtk_widget_set_focus_on_click(rb, FALSE);
            gtk_widget_set_focus_on_click(ob, FALSE);
            g_signal_connect(rb, "clicked", G_CALLBACK(on_rec_button), U);
            g_signal_connect(ob, "clicked", G_CALLBACK(on_rec_options), U);
            gtk_box_append(GTK_BOX(bar), rb);
            gtk_box_append(GTK_BOX(bar), ob);
            gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        }
        if (i == 7) {
            GtkWidget *eb = gtk_button_new_with_label("Export MIDI…");
            gtk_widget_set_tooltip_text(eb, "Write the song as a MIDI file, one track per playing track, "
                                            "to import into a DAW such as REAPER");
            gtk_widget_set_focus_on_click(eb, FALSE);
            g_signal_connect(eb, "clicked", G_CALLBACK(on_export_button), U);
            gtk_box_append(GTK_BOX(bar), eb);
        }
        if (i == 7) {
            /* Samples, beside the file buttons: load a set from anywhere,
             * or edit one. */
            GtkWidget *mb = gtk_menu_button_new();
            gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "Samples");
            gtk_widget_set_tooltip_text(mb, "Load a sample set from a folder, or edit one: "
                                            "which note plays which WAV");
            gtk_widget_set_focus_on_click(mb, FALSE);
            gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(mb), samples_menu, NULL, NULL);
            gtk_box_append(GTK_BOX(bar), mb);
        }
    }
    gtk_box_append(GTK_BOX(v), bar);

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    U->bpm = spin(20, 999, 1, 1);
    for (i = 0; i < NLPB; i++) { snprintf(lpbs[i], sizeof lpbs[i], "%d", k_lpbs[i]); lpbn[i] = lpbs[i]; }
    lpbn[NLPB] = NULL;
    U->lpb = gtk_drop_down_new_from_strings(lpbn);
    U->pattern = spin(0, TRK_PATTERNS - 1, 1, 0);
    U->rows = spin(1, TRK_ROWS_MAX, 1, 0);
    U->step = spin(0, 16, 1, 0);
    U->follow = gtk_check_button_new_with_label("follow");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(U->follow), TRUE);
    gtk_widget_set_tooltip_text(U->follow, "Keep the cursor on the row that is playing");
    U->editbox = gtk_check_button_new_with_label("edit");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(U->editbox), TRUE);
    gtk_widget_set_focus_on_click(U->editbox, FALSE);
    gtk_widget_set_tooltip_text(U->editbox, "Keys write into the pattern. Off, note keys only "
                                           "play, to try them out -- ` (backtick) turns it on and off");
    gtk_box_append(GTK_BOX(bar), labelled("bpm", U->bpm));
    gtk_box_append(GTK_BOX(bar), labelled("rows/beat", U->lpb));
    gtk_box_append(GTK_BOX(bar), labelled("pattern", U->pattern));
    gtk_box_append(GTK_BOX(bar), labelled("step", U->step));
    gtk_box_append(GTK_BOX(bar), U->follow);
    gtk_box_append(GTK_BOX(bar), U->editbox);
    /* Master volume: what the tracker sounds itself -- the sample tracks. The
     * windows that play the MIDI tracks have their own. */
    U->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 150, 1);
    gtk_range_set_value(GTK_RANGE(U->volume), 100);
    gtk_scale_set_draw_value(GTK_SCALE(U->volume), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(U->volume), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(U->volume), 0);
    gtk_widget_set_size_request(U->volume, 140, -1);
    gtk_widget_set_focus_on_click(U->volume, FALSE);
    gtk_widget_set_tooltip_text(U->volume, "Master volume of the sample tracks, in percent (the "
                                          "windows playing the MIDI tracks have their own)");
    gtk_box_append(GTK_BOX(bar), labelled("vol", U->volume));
    g_signal_connect(U->volume, "value-changed", G_CALLBACK(on_volume), U);
    {   /* Tracks: one added after the cursor's, or the cursor's taken away. */
        GtkWidget *add = gtk_button_new_with_label("+ Track"), *del = gtk_button_new_with_label("− Track");
        gtk_widget_set_tooltip_text(add, "Add an empty track after the cursor's (up to 16)");
        gtk_widget_set_tooltip_text(del, "Remove the cursor's track, notes and all (undo brings it back)");
        gtk_widget_set_focus_on_click(add, FALSE);
        gtk_widget_set_focus_on_click(del, FALSE);
        g_signal_connect(add, "clicked", G_CALLBACK(on_track_add), U);
        g_signal_connect(del, "clicked", G_CALLBACK(on_track_remove), U);
        gtk_box_append(GTK_BOX(bar), add);
        gtk_box_append(GTK_BOX(bar), del);
    }
    gtk_box_append(GTK_BOX(v), bar);


    /* Track headers, scrolled sideways with the grid by sharing its
     * adjustment. */
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    /* The grid draws a track's left divider one character left of its text;
     * a header spans divider to divider, so it sits over its column. */
    gtk_widget_set_size_request(gl, gutter(U) - U->cw, -1);
    gtk_box_append(GTK_BOX(head), gl);
    for (t = 0; t < TRK_TRACKS; t++) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *row;
        GtkListItemFactory *fshort = gtk_signal_list_item_factory_new();
        GtkListItemFactory *flong = gtk_signal_list_item_factory_new();
        boxes[t] = box;
        gtk_widget_set_margin_start(box, 3);
        gtk_widget_set_margin_end(box, 3);
        U->name[t] = gtk_entry_new();
        gtk_editable_set_width_chars(GTK_EDITABLE(U->name[t]), 4);
        g_signal_connect(fshort, "setup", G_CALLBACK(dest_setup), GINT_TO_POINTER(1));
        g_signal_connect(fshort, "bind", G_CALLBACK(dest_bind), NULL);
        g_signal_connect(flong, "setup", G_CALLBACK(dest_setup), NULL);
        g_signal_connect(flong, "bind", G_CALLBACK(dest_bind), NULL);
        U->dest[t] = gtk_drop_down_new(NULL, NULL);
        gtk_drop_down_set_factory(GTK_DROP_DOWN(U->dest[t]), fshort);
        gtk_drop_down_set_list_factory(GTK_DROP_DOWN(U->dest[t]), flong);
        g_object_unref(fshort);
        g_object_unref(flong);
        gtk_widget_set_tooltip_text(U->dest[t], "Which window this track plays");
        U->sample[t] = gtk_drop_down_new(NULL, NULL);
        {   /* Shortened in the box, whole in the list, as the window box is:
             * sample names run long, and the header must stay over its column. */
            GtkListItemFactory *ss = gtk_signal_list_item_factory_new();
            GtkListItemFactory *sl = gtk_signal_list_item_factory_new();
            g_signal_connect(ss, "setup", G_CALLBACK(dest_setup), GINT_TO_POINTER(1));
            g_signal_connect(ss, "bind", G_CALLBACK(dest_bind), NULL);
            g_signal_connect(sl, "setup", G_CALLBACK(dest_setup), NULL);
            g_signal_connect(sl, "bind", G_CALLBACK(dest_bind), NULL);
            gtk_drop_down_set_factory(GTK_DROP_DOWN(U->sample[t]), ss);
            gtk_drop_down_set_list_factory(GTK_DROP_DOWN(U->sample[t]), sl);
            g_object_unref(ss);
            g_object_unref(sl);
        }
        gtk_widget_set_tooltip_text(U->sample[t], "A sample set for this track to play instead "
                                                 "of a window: the note picks the sample");
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        U->chan[t] = gtk_drop_down_new_from_strings(chans);
        gtk_widget_set_tooltip_text(U->chan[t], "MIDI channel");
        {
            static const char *const octs[] = { "oct 0", "oct 1", "oct 2", "oct 3", "oct 4",
                                                "oct 5", "oct 6", "oct 7", "oct 8", "oct 9", NULL };
            U->oct[t] = gtk_drop_down_new_from_strings(octs);
            gtk_widget_set_tooltip_text(U->oct[t], "The octave the note keys play on this track");
            gtk_widget_set_focus_on_click(U->oct[t], FALSE);
        }
        U->mute[t] = gtk_check_button_new_with_label("mute");
        U->state[t] = gtk_label_new(NULL);
        gtk_box_append(GTK_BOX(row), U->chan[t]);
        gtk_box_append(GTK_BOX(row), U->oct[t]);
        gtk_box_append(GTK_BOX(row), U->mute[t]);
        gtk_box_append(GTK_BOX(row), U->state[t]);
        gtk_box_append(GTK_BOX(box), U->name[t]);
        gtk_box_append(GTK_BOX(box), U->dest[t]);
        gtk_box_append(GTK_BOX(box), U->sample[t]);
        gtk_box_append(GTK_BOX(box), row);
        gtk_box_append(GTK_BOX(head), box);
        g_signal_connect(U->name[t], "changed", G_CALLBACK(on_name),
                         ui_ref_new(U, t, G_OBJECT(U->name[t])));
        g_signal_connect(U->dest[t], "notify::selected", G_CALLBACK(on_dest),
                         ui_ref_new(U, t, G_OBJECT(U->dest[t])));
        g_signal_connect(U->sample[t], "notify::selected", G_CALLBACK(on_sample),
                         ui_ref_new(U, t, G_OBJECT(U->sample[t])));
        g_signal_connect(U->chan[t], "notify::selected", G_CALLBACK(on_chan),
                         ui_ref_new(U, t, G_OBJECT(U->chan[t])));
        g_signal_connect(U->oct[t], "notify::selected", G_CALLBACK(on_oct),
                         ui_ref_new(U, t, G_OBJECT(U->oct[t])));
        g_signal_connect(U->mute[t], "toggled", G_CALLBACK(on_mute),
                         ui_ref_new(U, t, G_OBJECT(U->mute[t])));
    }

    /* One width for a track everywhere: the grid's cell, or what the
     * header's widgets need under this theme, whichever is wider. Measured
     * rather than assumed, because a theme's spin buttons and drop-downs are
     * not a fixed size, and the headers have to sit over their columns. */
    U->colw = U->cw * (TRK_CELL_CHARS + 2);
    if (U->colw < 156) U->colw = 156;
    for (t = 0; t < TRK_TRACKS; t++) {
        int min = 0, nat = 0;
        gtk_widget_measure(boxes[t], GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
        /* Natural, not minimum: drop-downs are drawn at their natural width,
         * and a header wider than its column no longer sits over it. */
        if (nat + 6 > U->colw) U->colw = nat + 6;
    }
    for (t = 0; t < TRK_TRACKS; t++) {
        gtk_widget_set_size_request(boxes[t], U->colw - 6, -1);
        U->headbox[t] = boxes[t];
    }

    U->area = gtk_drawing_area_new();
    gtk_widget_set_focusable(U->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(U->area), draw, U, NULL);
    kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), U);
    g_signal_connect(kc, "key-released", G_CALLBACK(on_key_up), U);
    gtk_widget_add_controller(U->area, kc);
    fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "leave", G_CALLBACK(on_focus_leave), U);
    gtk_widget_add_controller(U->area, fc);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), U);
    gtk_widget_add_controller(U->area, GTK_EVENT_CONTROLLER(click));
    {   /* Dragging selects; right-click offers the clipboard. */
        GtkGesture *drag = gtk_gesture_drag_new(), *menu = gtk_gesture_click_new();
        g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag), U);
        gtk_widget_add_controller(U->area, GTK_EVENT_CONTROLLER(drag));
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu), GDK_BUTTON_SECONDARY);
        g_signal_connect(menu, "pressed", G_CALLBACK(on_context), U);
        gtk_widget_add_controller(U->area, GTK_EVENT_CONTROLLER(menu));
    }

    U->scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(U->scroll), U->area);
    gtk_widget_set_vexpand(U->scroll, TRUE);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(U->scroll), TRUE);

    U->headscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(U->headscroll), head);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(U->headscroll),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_hadjustment(GTK_SCROLLED_WINDOW(U->headscroll),
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(U->scroll)));
    /* Parts, left of the grid: the song in order. Headers and grid stack to
     * the right of it. */
    orow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(orow, TRUE);
    U->part_playing = -1;
    gtk_box_append(GTK_BOX(orow), build_parts(U));
    {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(col), U->headscroll);
        gtk_box_append(GTK_BOX(col), U->scroll);
        gtk_box_append(GTK_BOX(orow), col);
    }
    gtk_box_append(GTK_BOX(v), orow);


    U->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(U->status), 0);
    gtk_widget_set_margin_bottom(U->status, 4);
    gtk_box_append(GTK_BOX(v), U->status);

    g_signal_connect(U->bpm, "value-changed", G_CALLBACK(on_bpm), U);
    g_signal_connect(U->lpb, "notify::selected", G_CALLBACK(on_lpb), U);
    g_signal_connect(U->pattern, "value-changed", G_CALLBACK(on_pattern), U);
    g_signal_connect(U->rows, "value-changed", G_CALLBACK(on_rows), U);
    g_signal_connect(U->step, "value-changed", G_CALLBACK(on_step), U);
    g_signal_connect(U->follow, "toggled", G_CALLBACK(on_follow), U);
    g_signal_connect(U->editbox, "toggled", G_CALLBACK(on_editbox), U);

    U->play_pat = U->play_row = -1;
    U->t_follow = g_timeout_add(33, follow_playback, U);
    U->t_reroute = g_timeout_add(2000, reroute, U);
    g_signal_connect(v, "map", G_CALLBACK(on_view_map), U);
    g_signal_connect(v, "destroy", G_CALLBACK(on_view_destroy), U);
    return v;
}

/* The widget going away ends everything that holds a pointer into the
 * instance: the two timers, the cheat sheet, and a sample-set editor this
 * view opened. A shell closing a tab removes the page and this is what makes
 * trk_view_free safe to call afterwards; standalone, it runs at process exit. */
static void on_view_destroy(GtkWidget *w, gpointer u)
{
    ui *U = u;
    (void)w;
    U->closing = 1;
    if (U->t_follow) { g_source_remove(U->t_follow); U->t_follow = 0; }
    if (U->t_reroute) { g_source_remove(U->t_reroute); U->t_reroute = 0; }
    if (U->t_refit) { g_source_remove(U->t_refit); U->t_refit = 0; }
    if (U->cheatwin) gtk_window_destroy(GTK_WINDOW(U->cheatwin));
    if (U->recwin) gtk_window_destroy(GTK_WINDOW(U->recwin));
    if (E.U == U && E.win) gtk_window_destroy(GTK_WINDOW(E.win));
    U->view = NULL;
}

/* Standalone: the view in its own application window, with the window's
 * size, its title (update_title, from the view's file flows), and the
 * close-request save flow. */
static void tracker_window_new(ui *U, GtkApplication *app)
{
    U->win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(U->win), 1340, 760);
    g_signal_connect(U->win, "close-request", G_CALLBACK(on_close), U);
    gtk_window_set_child(GTK_WINDOW(U->win), trk_view_widget(U));
}

#ifdef TRACKER_UITEST
/* The window driven through the same handler real keys reach, with a
 * picture at each step. Run under GDK_BACKEND=broadway; argv: song, output
 * dir. Exit status is the number of failed checks. */
static int g_fail;
static char g_outdir[2048];

static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static void key(ui *U, guint kv)
{
    on_key(NULL, kv, 0, 0, U);
    on_key_up(NULL, kv, 0, 0, U);
}

static void pump(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end) g_main_context_iteration(NULL, FALSE);
}

static void shot_of(GtkWidget *w, const char *name)
{
    int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    GdkPaintable *p = gtk_widget_paintable_new(w);
    GtkSnapshot *s;
    GskRenderNode *node = NULL;
    char path[4096];
    int tries;
    /* A widget's paintable is what its last frame drew, and is empty until
     * one has been drawn since the change -- so ask for one and wait. */
    for (tries = 0; tries < 20; tries++) {
        gtk_widget_queue_draw(w);
        pump(50);
        s = gtk_snapshot_new();
        gdk_paintable_snapshot(p, s, width, height);
        node = gtk_snapshot_free_to_node(s);
        if (node) break;
    }
    snprintf(path, sizeof path, "%s/%s", g_outdir, name);
    if (node) {
        GskRenderer *r = gtk_native_get_renderer(gtk_widget_get_native(w));
        GdkTexture *t = gsk_renderer_render_texture(r, node,
                            &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height));
        gdk_texture_save_to_png(t, path);
        g_object_unref(t);
        gsk_render_node_unref(node);
        printf("  picture: %s\n", path);
    }
    g_object_unref(p);
}

static void shot(ui *U, const char *name) { shot_of(U->win, name); }

static trk_cell cell(ui *U, int p, int r, int t)
{
    trk_cell c;
    trk_lock(U->e);
    c = trk_song_of(U->e)->pattern[p].cell[r][t];
    trk_unlock(U->e);
    return c;
}

static gboolean uitest(gpointer u)
{
    ui *U = u;
    int o, p, r;
    pump(400);
    shot(U, "g01-loaded.png");
    /* The set editor, as Samples > Edit Sample Set opens it. */
    on_edit_samples(NULL, NULL, U);
    pump(500);
    if (E.win) {
        shot_of(E.win, "g05-editor.png");
        kb_scroll_to(110);
        pump(400);
        shot_of(E.win, "g05c-keymap-top.png");
        E.curnote = 64;
        kb_refresh();
        kb_scroll_to(64);
        pump(400);
        shot_of(E.win, "g05d-keymap-picked.png");
        if (E.m->n >= 2) {   /* Samples onto keys: a swap, then a free key. */
            int a = E.m->pad[0].note, b = E.m->pad[1].note;
            ed_assign(0, b);
            check(E.m->pad[0].note == b && E.m->pad[1].note == a,
                  "dropping a sample on a taken key swaps the two");
            ed_assign(1, 100);
            check(E.m->pad[1].note == 100, "dropping on a free key moves the sample there");
            {   /* The drop-down beside a key: a sample from the list, then none. */
                char file[512];
                int rows = E.m->n;
                snprintf(file, sizeof file, "%s", E.m->pad[1].file);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(E.kdd[90]), (guint)(kb_choice_of(file) + 1));
                check(E.m->pad[1].note == 90, "choosing a sample from a key's drop-down moves it there");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(E.kdd[90]), 0);
                check(E.m->n == rows - 1, "choosing none takes the sample off the key");
            }
            kb_scroll_to(E.m->pad[0].note);
            pump(400);
            shot_of(E.win, "g05b-keymap.png");
        } else check(0, "the editor has samples to put on keys");
        E.dirty = 0;
        gtk_window_destroy(GTK_WINDOW(E.win));
        pump(100);
    }
    {   /* A block selected -- and the cursor put back, for the steps after. */
        int row = U->ed.row, track = U->ed.track;
        trk_select(&U->ed, 2, 0, 9, 2);
        redraw(U);
        pump(200);
        shot(U, "g07-selected.png");
        trk_select_none(&U->ed);
        U->ed.row = row;
        U->ed.track = track;
        redraw(U);
    }
    /* Help > Cheat Sheet. */
    g_action_group_activate_action(G_ACTION_GROUP(U->ag), "cheat", NULL);
    pump(400);
    if (U->cheatwin) {
        shot_of(U->cheatwin, "g06-cheat.png");
        gtk_window_destroy(GTK_WINDOW(U->cheatwin));
        pump(100);
    }
    check(cell(U, 0, 0, 0).note == 36, "song loaded: C-2 on track 1, row 0");
    check(gtk_drop_down_get_selected(GTK_DROP_DOWN(U->dest[2])) == (guint)(U->ndest[2] - 1) &&
          strstr(U->destval[2][U->ndest[2] - 1], "not-open-yet"), "a closed destination is kept");

    printf("typing\n");
    key(U, GDK_KEY_Home);
    key(U, GDK_KEY_Tab);
    key(U, GDK_KEY_Tab);
    check(U->ed.track == 2 && U->ed.row == 0, "Tab moves to track 3");
    key(U, GDK_KEY_z); key(U, GDK_KEY_c); key(U, GDK_KEY_b); key(U, GDK_KEY_1);
    check(cell(U, 0, 0, 2).note == 60 && cell(U, 0, 1, 2).note == 64 &&
          cell(U, 0, 2, 2).note == 67 && cell(U, 0, 3, 2).note == TRK_NOTE_OFF,
          "z c b 1 wrote C-4 E-4 G-4 ===");
    key(U, GDK_KEY_Up); key(U, GDK_KEY_Up); key(U, GDK_KEY_Up);
    key(U, GDK_KEY_Right);
    key(U, GDK_KEY_4); key(U, GDK_KEY_0);
    check(cell(U, 0, 1, 2).vel == 0x40, "hex 4 0 into velocity");
    key(U, GDK_KEY_ISO_Left_Tab);
    check(U->ed.track == 1 && U->ed.field == TRK_F_NOTE, "Shift+Tab back a track");
    {   /* A repeat -- a press with no release between -- writes once. */
        int row = U->ed.row;
        on_key(NULL, GDK_KEY_q, 0, 0, U);
        on_key(NULL, GDK_KEY_q, 0, 0, U);
        on_key_up(NULL, GDK_KEY_q, 0, 0, U);
        check(U->ed.row == row + 1, "a held key writes once, not on every repeat");
    }
    {   /* Pressed as '2', released after Shift went down -- the release
         * translates differently ('@' on a us layout), and must still find
         * and clear the press, or the repeat guard above swallows that key
         * for the rest of the session. */
        int row = U->ed.row;
        on_key(NULL, GDK_KEY_2, 0, 0, U);
        on_key_up(NULL, GDK_KEY_at, 0, 0, U);
        on_key(NULL, GDK_KEY_2, 0, 0, U);
        on_key_up(NULL, GDK_KEY_2, 0, 0, U);
        check(U->ed.row == row + 2, "a release translated differently still ends the press");
    }
    pump(300);
    shot(U, "g02-typed.png");

    printf("playing\n");
    key(U, GDK_KEY_F6);
    pump(700);
    trk_position(U->e, &o, &p, &r);
    check(trk_playing(U->e) && p == 0 && r > 0, "F6 plays the pattern and the position moves");
    shot(U, "g03-playing.png");
    key(U, GDK_KEY_F8);
    pump(100);
    check(!trk_playing(U->e), "F8 stops");
    key(U, GDK_KEY_F5);
    pump(2300);
    trk_position(U->e, &o, &p, &r);
    check(trk_playing(U->e) && o >= 1 && U->ed.pattern == p,
          "F5 plays through the order list, the editor following");
    key(U, GDK_KEY_Return);
    pump(100);
    check(!trk_playing(U->e), "Enter stops");
    {   /* Tracks added after the cursor's and taken away again. */
        const int before = trk_song_of(U->e)->ntracks;
        int i;
        for (i = 0; i < 4; i++) on_track_add(NULL, U);
        pump(100);
        check(trk_song_of(U->e)->ntracks == before + 4, "four tracks added");
        check(gtk_widget_get_visible(U->headbox[before + 3]) && !gtk_widget_get_visible(U->headbox[before + 4]),
              "a header over each, none over the room left");
        shot(U, "g08-tracks-added.png");
        for (i = 0; i < 4; i++) { U->ed.track = trk_song_of(U->e)->ntracks - 1; on_track_remove(NULL, U); }
        pump(100);
        check(trk_song_of(U->e)->ntracks == before, "and taken away again");
        U->ed.track = 0;
    }
    {   /* F7 records: the take runs until Stop ends it. */
        trk_rec_opts o;
        trk_record_get(U->e, &o);
        o.count_in = 0; o.metronome = 0;
        trk_record_set(U->e, &o);
        key(U, GDK_KEY_F7);
        pump(150);
        check(trk_recording(U->e) == 1, "F7 starts a take");
        key(U, GDK_KEY_F8);
        pump(100);
        check(trk_recording(U->e) == 0 && !trk_playing(U->e), "Stop ends it");
    }
    {   /* Space is edit mode now: it toggles, and with it off the note keys only play. */
        const int was = U->ed.edit;
        key(U, GDK_KEY_space);
        check(U->ed.edit == !was, "Space toggles edit mode");
        key(U, GDK_KEY_space);
        check(U->ed.edit == was, "and back");
    }

    printf("saving\n");
    {
        char path[4096], err[256];
        trk_song *back = malloc(sizeof *back);
        int rc;
        snprintf(path, sizeof path, "%s/gsaved.trk", g_outdir);
        check(write_to(U, path) == 0, "saved");
        rc = trk_song_load(back, path, err, sizeof err);
        trk_lock(U->e);
        check(rc == 0 && !memcmp(back, trk_song_of(U->e), sizeof *back),
              "what was saved loads back as the same song");
        trk_unlock(U->e);
        free(back);
    }
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    U->closing = 1;
    g_application_quit(G_APPLICATION(U->app));
    return G_SOURCE_REMOVE;
}
#endif

/* ------------------------------------------------------------- the API
 *
 * What trackerview.h declares: the standalone's window wrapper above, and
 * the embedding surface a shell uses. */

trk_view *trk_view_new(trk_engine *e)
{
    ui *U = calloc(1, sizeof *U);
    U->e = e;
    U->saved = malloc(sizeof *U->saved);
    trk_song_init(U->saved);
    trk_editor_init(&U->ed);
    return U;
}

GtkWidget *trk_view_widget(trk_view *v)
{
    ui *U = v;
    if (!U->view) {
        tracker_view_new(U);
        /* The post-map fit, for every host: the headers are measured before
         * the theme has styled them, and only the standalone used to get the
         * second measurement. */
        schedule_refit(U);
    }
    return U->view;
}

int  trk_view_open(trk_view *v, const char *path) { return open_path(v, path); }
void trk_view_reset(trk_view *v) { reset_view(v); }

/* A shell menu's song commands: cases 4, 6 and 7 of on_button, so the toolbar
 * and the host's menu can never drift apart. */
void trk_view_new_song(trk_view *v) { confirm_then(v, AFTER_NEW); }
void trk_view_save(trk_view *v)     { do_save(v, 0); }
void trk_view_save_as(trk_view *v)  { save_as(v, 0); }
void trk_view_set_embedded(trk_view *v, int on) { ((ui *)v)->embedded = on; }
void trk_view_audio_output(trk_view *v) { show_audio_output(v); }
void trk_view_export_take(trk_view *v) { export_take(v); }
void trk_view_record_options(trk_view *v) { show_rec_options(v); }
void trk_view_load_samples(trk_view *v) { on_load_samples(NULL, NULL, v); }
void trk_view_edit_samples(trk_view *v) { on_edit_samples(NULL, NULL, v); }
void trk_view_show_keys(trk_view *v)    { on_keys(NULL, NULL, v); }
void trk_view_show_columns(trk_view *v) { on_columns(NULL, NULL, v); }
void trk_view_show_cheat(trk_view *v)   { on_cheat(NULL, NULL, v); }
void trk_view_export_midi(trk_view *v) { export_midi(v); }

int  trk_view_dirty(trk_view *v) { return dirty(v); }
const char *trk_view_path(trk_view *v) { return ((ui *)v)->path; }

void trk_view_mark_clean(trk_view *v)
{
    ui *U = v;
    trk_lock(U->e);
    memcpy(U->saved, trk_song_of(U->e), sizeof *U->saved);
    trk_unlock(U->e);
}

void trk_view_set_sinks(trk_view *v, const trk_view_sinks *api, void *ud)
{
    ui *U = v;
    U->has_sinks = api != NULL;
    if (api) U->sinks = *api;
    U->sinks_ud = ud;
    if (U->view) refresh_dests(U, 1);
}

void trk_view_set_song_opened(trk_view *v, void (*cb)(void *ud), void *ud)
{
    ui *U = v;
    U->song_opened = cb;
    U->song_opened_ud = ud;
}

void trk_view_confirm_close(trk_view *v, void (*cb)(void *ud), void *ud)
{
    ui *U = v;
    if (!dirty(U)) { cb(ud); return; }
    U->embed_close = cb;
    U->embed_close_ud = ud;
    confirm_then(U, AFTER_CLOSE);
}

/* The same flow as confirm_close, with "the close proceeds" read as "the way
 * is clear": Save saves and calls cb, Discard calls cb, Cancel is silence.
 * Nothing closes -- cb is the shell's own next step -- so it runs through
 * AFTER_ENSURE rather than AFTER_CLOSE, which would mark the view closing. */
void trk_view_ensure_saved(trk_view *v, void (*cb)(void *ud), void *ud)
{
    ui *U = v;
    if (!dirty(U)) { cb(ud); return; }
    U->embed_ensure = cb;
    U->embed_ensure_ud = ud;
    confirm_then(U, AFTER_ENSURE);
}

/* The view's memory, freed after whatever GTK still has queued for it has
 * run. A shell removes the tab's page and frees the view in the same breath,
 * but GTK finishes destroying the page's widgets later -- and until it has,
 * its timers, idles and focus handlers hold this pointer. */
static gboolean free_view_later(gpointer u)
{
    ui *U = u;
    free(U->saved);
    free(U);
    return G_SOURCE_REMOVE;
}

void trk_view_free(trk_view *v)
{
    ui *U = v;
    U->closing = 1;                       /* every callback that checks it stands down */
    if (U->t_follow) { g_source_remove(U->t_follow); U->t_follow = 0; }
    if (U->t_reroute) { g_source_remove(U->t_reroute); U->t_reroute = 0; }
    if (U->t_refit) { g_source_remove(U->t_refit); U->t_refit = 0; }
    /* Its widgets down now, rather than whenever GTK gets to it: the signal
     * handlers that run while they go (focus leaving) see `closing`. */
    if (U->view) {
        /* Standalone, the window still holds it: let go first, or the dispose
         * below is a widget freed with a parent. */
        GtkWidget *view = U->view, *par = gtk_widget_get_parent(view);
        g_object_ref(view);
        if (par && GTK_IS_WINDOW(par)) gtk_window_set_child(GTK_WINDOW(par), NULL);
        g_object_run_dispose(G_OBJECT(view));
        g_object_unref(view);
    }
    g_idle_add_full(G_PRIORITY_LOW, free_view_later, U, NULL);
}

void trk_view_standalone(trk_view *v, GtkApplication *app, const char *song)
{
    ui *U = v;
    U->app = app;
    tracker_window_new(U, app);
    if (song) open_path(U, song);
    else reset_view(U);
    gtk_window_present(GTK_WINDOW(U->win));
    gtk_widget_grab_focus(U->area);
}

#ifdef TRACKER_UITEST
void trk_view_uitest(trk_view *v, const char *outdir)
{
    snprintf(g_outdir, sizeof g_outdir, "%s", outdir ? outdir : ".");
    g_idle_add(uitest, v);
}
int trk_view_uitest_failures(void) { return g_fail; }
#endif
