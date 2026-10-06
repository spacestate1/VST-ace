/* tracker-gtk -- the GTK 4 window. The same program as the Qt one: it draws
 * the song and hands keys to core/, where every decision is made. */
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
    "  `                 edit mode on / off (off: note keys only play)\n"
    "  Delete or .       clear and advance\n"
    "  Insert            push the track down a row\n"
    "  Backspace         pull the track up over this row\n"
    "  0-9 a-f           hex, in the velocity and controller fields\n"
    "  [ ]               octave down / up\n"
    "  - =               previous / next pattern\n"
    "\n"
    "Transport\n"
    "  F5  play song     F6  play pattern     F8  stop\n"
    "  Space             play pattern / stop\n"
    "  Escape            panic: release every note everywhere\n"
    "\n"
    "Each track plays one window. Open vst-ace once per instrument, then pick\n"
    "the window under the track's name. A cell is note, velocity, controller\n"
    "number and controller value; empty velocity uses the track's.";

static const int k_lpbs[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
#define NLPB ((int)(sizeof k_lpbs / sizeof k_lpbs[0]))
#define MAXDEST 128
#define MAXSAMP (DK_MAX_KITS + TRK_TRACKS + 2)  /* every set, "none", and one not found */

/* One tracker window, whole: the engine it plays, the editor state, and every
 * widget. main() makes one; a shell embedding the view would make one per
 * window. Every function below takes it as its first parameter.
 *
 * The sample-set editor's state (E, further down) is still one-per-process:
 * it is a modal dialog of the window that opened it and keeps a pointer back
 * to its instance. */
typedef struct {
    trk_engine *e;
    trk_editor  ed;
    trk_song   *saved;               /* the song as last saved, for "unsaved changes?" */
    char        path[4096];
    int         loading;             /* widgets being set from the song: do not write back */
    int         closing;

    GtkApplication *app;
    GtkWidget  *win, *area, *scroll, *headscroll, *status;
    GtkWidget  *cheatwin, *cheattext;  /* Help > Cheat Sheet, while it is open */
    GtkWidget  *bpm, *lpb, *pattern, *rows, *step, *follow, *editbox, *volume;
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

    const char *open;                /* a song named on the command line, or NULL */
} ui;

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

static void update_size(ui *U)
{
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(U->area),
                                       gutter(U) + TRK_TRACKS * colwidth(U) + U->cw);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(U->area), cur_rows(U) * U->ch + 2);
}

static void redraw(ui *U) { gtk_widget_queue_draw(U->area); }

static void status(ui *U, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(U->status), msg);
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
    PangoLayout *l;
    const trk_song *s;
    const trk_pattern *pt;
    int r, r0, r1, t, lpb, mutes = 0;
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

    trk_lock(U->e);
    s = trk_song_of(U->e);
    pt = &s->pattern[U->ed.pattern];
    lpb = s->lpb > 0 ? s->lpb : 4;
    for (t = 0; t < TRK_TRACKS; t++) if (s->track[t].mute) mutes |= 1 << t;
    r0 = (int)(y1 / U->ch);
    if (r0 < 0) r0 = 0;
    r1 = (int)(y2 / U->ch);
    if (r1 > pt->rows - 1) r1 = pt->rows - 1;

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

        for (t = 0; t < TRK_TRACKS; t++) {
            static const int start[TRK_FIELDS] = { TRK_COL_NOTE, TRK_COL_VEL, TRK_COL_CC, TRK_COL_VAL };
            static const int len[TRK_FIELDS]   = { 3, 2, 2, 2 };
            const int x = gutter(U) + t * colwidth(U);
            const int on_cursor = r == U->ed.row && t == U->ed.track;
            char txt[TRK_CELL_CHARS + 1];
            int f;
            trk_cell_text(&pt->cell[r][t], txt);
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
                const int nt = pt->cell[r][t].note;
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
    trk_unlock(U->e);

    set(cr, faint);
    cairo_set_line_width(cr, 1);
    for (t = 0; t <= TRK_TRACKS; t++) {
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
    refresh_parts(U);
}


static void refresh_samples(ui *U);
static void edit_shown(ui *U);

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
        gtk_window_set_transient_for(GTK_WINDOW(U->cheatwin), GTK_WINDOW(U->win));
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
static void on_keys(GSimpleAction *a, GVariant *v, gpointer u) { ui *U = u; (void)a; (void)v; show_keys(U); }

/* Help, last of the buttons: what every key does, and the cheat sheet. */
static void help_menu(GtkMenuButton *mb, gpointer u)
{
    GMenu *m = g_menu_new();
    (void)u;
    g_menu_append(m, "Keys", "win.keys");
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
 * does not forget where its tracks go. */
static void refresh_dests(ui *U, int force)
{
    char buf[sizeof U->last_dests];
    int t;

    trk_list_dests(U->e, buf, sizeof buf);
    if (!force && !strcmp(buf, U->last_dests)) { update_states(U); return; }
    memcpy(U->last_dests, buf, sizeof buf);

    U->loading = 1;
    for (t = 0; t < TRK_TRACKS; t++) {
        GtkStringList *sl = gtk_string_list_new(NULL);
        char want[2 * TRK_DEST_LEN + 2], *copy, *line, *save = NULL;
        int i, sel = 0;

        for (i = 0; i < U->ndest[t]; i++) g_free(U->destval[t][i]);
        U->ndest[t] = 0;
        trk_lock(U->e);
        snprintf(want, sizeof want, "%s\t%s", trk_song_of(U->e)->track[t].client,
                 trk_song_of(U->e)->track[t].port);
        trk_unlock(U->e);

        gtk_string_list_append(sl, "(nowhere)");
        U->destval[t][U->ndest[t]++] = g_strdup("\t");
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
        gtk_drop_down_set_model(GTK_DROP_DOWN(U->dest[t]), G_LIST_MODEL(sl));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U->dest[t]), (guint)sel);
        g_object_unref(sl);
    }
    U->loading = 0;
    refresh_samples(U);
    update_states(U);
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
    gtk_file_dialog_select_folder(d, GTK_WINDOW(U->win), NULL, on_samples_folder, U);
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
    GtkWidget *win, *sets, *dir, *grid, *status;
    dk_map    *m;
    char       name[TRK_PATH_LEN];
    char      *setval[MAXSAMP];
    int        nsets, cur, dirty, filling, pending;   /* pending: a set to switch to */
} E;

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

static void ed_remove(GtkButton *b, gpointer u)
{
    int i = GPOINTER_TO_INT(u);
    (void)b;
    if (i >= E.m->n) return;
    memmove(&E.m->pad[i], &E.m->pad[i + 1], sizeof E.m->pad[0] * (size_t)(E.m->n - i - 1));
    E.m->n--;
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
    ed_fill();
    ed_status("");
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
    free(E.m);
    gtk_widget_grab_focus(E.U->area);
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
    gtk_window_set_transient_for(GTK_WINDOW(E.win), GTK_WINDOW(U->win));
    gtk_window_set_modal(GTK_WINDOW(E.win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(E.win), 760, 520);
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

    E.grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(E.grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(E.grid), 2);
    sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), E.grid);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);

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
    gtk_menu_button_set_menu_model(mb, G_MENU_MODEL(m));
    g_object_unref(m);
}

static void update_title(ui *U)
{
    char title[4200];
    const char *base = U->path[0] ? strrchr(U->path, '/') : NULL;
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
    trk_lock(U->e);
    r = trk_song_save(trk_song_of(U->e), path, err, sizeof err);
    if (!r) memcpy(U->saved, trk_song_of(U->e), sizeof(trk_song));
    trk_unlock(U->e);
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
    trk_lock(U->e);
    memcpy(trk_song_of(U->e), tmp, sizeof *tmp);
    trk_unlock(U->e);
    memcpy(U->saved, tmp, sizeof *tmp);
    snprintf(U->path, sizeof U->path, "%s", path);
    trk_editor_init(&U->ed);
    trk_set_bpm(U->e, tmp->bpm);
    free(tmp);
    reset_view(U);
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
    /* Shift with the arrows selects; Ctrl with C, X, V and A is the clipboard. */
    if (st & GDK_SHIFT_MASK)
        switch (kv) {
        case GDK_KEY_Up:    return TRK_K_SEL_UP;
        case GDK_KEY_Down:  return TRK_K_SEL_DOWN;
        case GDK_KEY_Left:  return TRK_K_SEL_LEFT;
        case GDK_KEY_Right: return TRK_K_SEL_RIGHT;
        default: break;
        }
    if (st & GDK_CONTROL_MASK)
        switch (kv) {
        case GDK_KEY_c: case GDK_KEY_C: return TRK_K_COPY;
        case GDK_KEY_x: case GDK_KEY_X: return TRK_K_CUT;
        case GDK_KEY_v: case GDK_KEY_V: return TRK_K_PASTE;
        case GDK_KEY_a: case GDK_KEY_A: return TRK_K_SEL_ALL;
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
    case GDK_KEY_F8:        return TRK_K_STOP;
    case GDK_KEY_space:     return TRK_K_TOGGLE;
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
    if (kv == GDK_KEY_Escape) { trk_panic(U->e); return TRUE; }
    key = translate(kv, st);
    if (key < 0) return FALSE;
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
    if (key == TRK_K_COPY || key == TRK_K_CUT || key == TRK_K_PASTE) clip_status(U, key);
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
    if (*t > TRK_TRACKS - 1) *t = TRK_TRACKS - 1;
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
        trk_select(&U->ed, U->drag_r, 0, r, TRK_TRACKS - 1);
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
        trk_select(&U->ed, U->drag_r, 0, r, TRK_TRACKS - 1);
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
    if (!U->ed.edit && key != TRK_K_COPY) { status(U, "edit is off -- ` to edit, then cut or paste"); return; }
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
    if (t < 0 && !trk_selected(&U->ed, r, 0)) { trk_select(&U->ed, r, 0, r, TRK_TRACKS - 1); U->ed.track = 0; }
    redraw(U);
    cursor_moved(U);

    trk_clipboard(&rows, &tracks);
    if (rows) snprintf(paste, sizeof paste, "Paste %d row%s x %d track%s",
                       rows, rows > 1 ? "s" : "", tracks, tracks > 1 ? "s" : "");
    else snprintf(paste, sizeof paste, "Paste");
    g_menu_append(a, "Copy", "win.copy");
    g_menu_append(a, "Cut", "win.cut");
    g_menu_append(a, paste, "win.paste");
    g_menu_append(a, "Clear", "win.clear");
    g_menu_append(b, "Select This Track's Column", "win.select-column");
    g_menu_append(b, "Select All", "win.select-all");
    g_menu_append_section(m, NULL, G_MENU_MODEL(a));
    g_menu_append_section(m, NULL, G_MENU_MODEL(b));
    {
        GAction *pa = g_action_map_lookup_action(G_ACTION_MAP(U->win), "paste");
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

static gboolean follow_playback(gpointer u)
{
    ui *U = u;
    int o, p, r;
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
static gboolean refresh_cheat_idle(gpointer u) { refresh_cheat(u); return G_SOURCE_REMOVE; }
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
    trk_lock(U->e);
    trk_song_of(U->e)->track[t].octave = o;
    trk_unlock(U->e);
    if (t == U->ed.track) U->ed.octave = o;
    g_idle_add(refresh_cheat_idle, U);
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
        if (!write_to(U, path) && close_after) { U->closing = 1; gtk_window_close(GTK_WINDOW(U->win)); }
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
    gtk_file_dialog_set_initial_name(d, U->path[0] ? strrchr(U->path, '/') + 1 : "song.trk");
    {
        ui_ref *r = g_new(ui_ref, 1);
        r->U = U; r->n = close_after;
        gtk_file_dialog_save(d, GTK_WINDOW(U->win), NULL, save_done, r);
    }
    g_object_unref(d);
}

static void do_save(ui *U, int close_after)
{
    if (!U->path[0]) { save_as(U, close_after); return; }
    if (!write_to(U, U->path) && close_after) { U->closing = 1; gtk_window_close(GTK_WINDOW(U->win)); }
}

typedef enum { AFTER_NEW, AFTER_OPEN, AFTER_CLOSE } after_t;

static void after_confirm(ui *U, after_t what)
{
    if (what == AFTER_CLOSE) { U->closing = 1; gtk_window_close(GTK_WINDOW(U->win)); return; }
    if (what == AFTER_NEW) {
        trk_stop(U->e);
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
        gtk_file_dialog_open(d, GTK_WINDOW(U->win), NULL, open_done, U);
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
        if (what == AFTER_CLOSE) do_save(U, 1);
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
        gtk_alert_dialog_choose(d, GTK_WINDOW(U->win), NULL, confirm_done, r);
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

static void show_keys(ui *U)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("tracker keys");
    gtk_alert_dialog_set_detail(d, k_keys_help);
    gtk_alert_dialog_show(d, GTK_WINDOW(U->win));
    g_object_unref(d);
}

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
 * sits over its own column. */
static gboolean refit_columns(gpointer u)
{
    ui *U = u;
    int t, w = U->colw;
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

static void build(ui *U, GtkApplication *app)
{
    static const char *bnames[] = { "▶ Song", "▶ Pattern", "■ Stop", "Panic",
                                    "New", "Open…", "Save", "Save As…", "Keys" };
    static const char *btips[] = {
        "Play the song from the order entry holding this pattern (F5)",
        "Loop this pattern (F6, or Space)",
        "Stop and release every note (F8)",
        "Release every note on every track, playing or not (Escape)",
        "Start an empty song", "Open a song", "Save the song (to its file)",
        "Save the song to a new file", "What every key does" };
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
    U->win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(U->win), 1340, 760);
    g_signal_connect(U->win, "close-request", G_CALLBACK(on_close), U);

    v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(v, 6);
    gtk_widget_set_margin_end(v, 6);
    gtk_widget_set_margin_top(v, 6);
    gtk_window_set_child(GTK_WINDOW(U->win), v);

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (i = 0; i < 9; i++) {
        GtkWidget *b;
        if (i == 8) {
            /* Help, where Keys was: Keys and the cheat sheet. */
            static const GActionEntry acts[] = {
                { "keys",  on_keys,  NULL, NULL, NULL, {0} },
                { "cheat", on_cheat, NULL, NULL, NULL, {0} },
            };
            GtkWidget *mb = gtk_menu_button_new();
            g_action_map_add_action_entries(G_ACTION_MAP(U->win), acts, G_N_ELEMENTS(acts), U);
            gtk_application_set_accels_for_action(app, "win.cheat", (const char *[]){ "F1", NULL });
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
        if (i == 3) gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        if (i == 7) {
            /* Samples, beside the file buttons: load a set from anywhere,
             * or edit one. */
            static const GActionEntry acts[] = {
                { "load-samples", on_load_samples, NULL, NULL, NULL, {0} },
                { "edit-samples", on_edit_samples, NULL, NULL, NULL, {0} },
            };
            GtkWidget *mb = gtk_menu_button_new();
            g_action_map_add_action_entries(G_ACTION_MAP(U->win), acts, G_N_ELEMENTS(acts), U);
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
    gtk_box_append(GTK_BOX(v), bar);


    /* Track headers, scrolled sideways with the grid by sharing its
     * adjustment. */
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(gl, gutter(U), -1);
    gtk_box_append(GTK_BOX(head), gl);
    for (t = 0; t < TRK_TRACKS; t++) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *row;
        GtkListItemFactory *fshort = gtk_signal_list_item_factory_new();
        GtkListItemFactory *flong = gtk_signal_list_item_factory_new();
        boxes[t] = box;
        gtk_widget_set_margin_end(box, 6);
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
        static const GActionEntry acts[] = {
            { "copy", on_copy, NULL, NULL, NULL, {0} },
            { "cut", on_cut, NULL, NULL, NULL, {0} },
            { "paste", on_paste, NULL, NULL, NULL, {0} },
            { "clear", on_clear, NULL, NULL, NULL, {0} },
            { "select-column", on_selcol, NULL, NULL, NULL, {0} },
            { "select-all", on_selall, NULL, NULL, NULL, {0} },
        };
        GtkGesture *drag = gtk_gesture_drag_new(), *menu = gtk_gesture_click_new();
        g_action_map_add_action_entries(G_ACTION_MAP(U->win), acts, G_N_ELEMENTS(acts), U);
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
    g_timeout_add(33, follow_playback, U);
    g_timeout_add(2000, reroute, U);
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
    g_action_group_activate_action(G_ACTION_GROUP(U->win), "cheat", NULL);
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
    key(U, GDK_KEY_space);
    pump(100);
    check(!trk_playing(U->e), "Space stops");

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

static void activate(GtkApplication *app, gpointer u)
{
    ui *U = u;
    build(U, app);
    if (U->open) open_path(U, U->open);
    else reset_view(U);
    gtk_window_present(GTK_WINDOW(U->win));
    /* Again once it is on screen: a theme's drop-downs measure narrower
     * before they are styled than they draw. */
    g_timeout_add(200, refit_columns, U);
    gtk_widget_grab_focus(U->area);
#ifdef TRACKER_UITEST
    g_idle_add(uitest, U);
#endif
}

int main(int argc, char **argv)
{
    char err[256];
    int rc;
    char *args[] = { argv[0], NULL };
    ui *U = calloc(1, sizeof *U);

    if (!(U->e = trk_open(err, sizeof err))) {
        fprintf(stderr, "tracker: %s\n", err);
        return 1;
    }
    U->saved = malloc(sizeof *U->saved);
    trk_song_init(U->saved);
    trk_editor_init(&U->ed);
    if (argc > 1) U->open = argv[1];
#ifdef TRACKER_UITEST
    snprintf(g_outdir, sizeof g_outdir, "%s", argc > 2 ? argv[2] : ".");
#endif
    /* Not unique: two trackers driving different windows is a reasonable
     * thing to want, and GApplication would otherwise hand the second
     * launch to the first and exit. */
    U->app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(U->app, "activate", G_CALLBACK(activate), U);
    rc = g_application_run(G_APPLICATION(U->app), 1, args);
    g_object_unref(U->app);
    trk_close(U->e);         /* releases anything still sounding */
#ifdef TRACKER_UITEST
    return g_fail;
#endif
    free(U->saved);
    free(U);
    return rc;
}
