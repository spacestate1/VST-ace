/* trackerwidget -- the whole Qt tracker as a widget a shell can host.
 *
 * Everything the tracker's window used to hold lives here: the pattern grid,
 * the parts list, the sample-set editor, the menus it asks its host for and
 * the status lines it writes. TrackerHost, below, is the contract with
 * whatever frames it -- the standalone tracker binary's TrackerWindow
 * (qt/main.cpp) is one, and the studio window hosts one in a tab beside the
 * synths. The widget draws the song and hands keys to core/; every decision
 * about what a key does or what a file means is made there.
 */
#ifndef TRACKER_QT_TRACKERWIDGET_H
#define TRACKER_QT_TRACKERWIDGET_H

#include "trk.h"
#include "drumkit.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QMimeData>
#include <QScrollBar>
#include <QSpinBox>
#include <QSlider>
#include <QDialog>
#include <QFormLayout>
#include <QPointer>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QHeaderView>
#include <QTableWidget>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// In a header now, so one per translation unit: a const pointer has external
// linkage in C++, and the anonymous namespace that gave it one copy per binary
// stayed behind in main.cpp.
inline const char *kKeysHelp =
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

// --------------------------------------------------------------- the grid --

class PatternView : public QWidget {
    Q_OBJECT
public:
    PatternView(trk_engine *e, trk_editor *ed, QWidget *parent = nullptr)
        : QWidget(parent), e_(e), ed_(ed)
    {
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setPointSizeF(f.pointSizeF() * 1.05);
        setFont(f);
        setFocusPolicy(Qt::StrongFocus);
        QFontMetrics fm(f);
        cw_ = fm.horizontalAdvance('0');
        ch_ = fm.height() + 2;
        asc_ = fm.ascent() + 1;
        updateSize();
    }

    int gutter() const { return cw_ * 4; }
    int colWidth() const { return std::max(cw_ * (TRK_CELL_CHARS + 2), 210); }
    int rowHeight() const { return ch_; }
    int charWidth() const { return cw_; }

    // How many tracks the song has: the grid, the headers and every hit test
    // go by it, not by the most there is room for.
    int nt() const
    {
        const int n = trk_song_of(e_)->ntracks;
        return n < 1 ? 1 : n > TRK_TRACKS ? TRK_TRACKS : n;
    }

    void updateSize()
    {
        trk_lock(e_);
        int rows = trk_song_of(e_)->pattern[ed_->pattern].rows;
        trk_unlock(e_);
        setFixedSize(gutter() + nt() * colWidth() + cw_, rows * ch_ + 2);
    }

    void setEditing(bool on) { editing_ = on; update(); }

    void setPlayRow(int pattern, int row)
    {
        if (pattern == playPat_ && row == playRow_) return;
        const int old = playRow_;
        playPat_ = pattern;
        playRow_ = row;
        updateRow(old);
        updateRow(row);
    }

    // Only the row that changed, not the grid: during playback this runs
    // thirty times a second.
    void updateRow(int r) { if (r >= 0) update(0, r * ch_, width(), ch_); }

signals:
    void edited();
    void cursorMoved();
    void noteTyped(int track, int note);
    void editToggled();
    void mutesChanged();             // a key muted, soloed or unmuted tracks
    void stepChanged();              // a key changed the edit step
    void trackAddRequested();        // Ctrl+Insert
    void trackRemoveRequested();     // Ctrl+Delete
    void clipped(int key);           // copied, cut or pasted: for the status line

protected:
    void paintEvent(QPaintEvent *ev) override
    {
        QPainter p(this);
        const QPalette pal = palette();
        p.fillRect(ev->rect(), pal.color(QPalette::Base));

        // Which notes each sample-set track has a sample on -- asked before
        // the song's lock is taken, which this call takes itself.
        unsigned char masks[TRK_TRACKS][16];
        bool sampled[TRK_TRACKS];
        for (int t = 0; t < TRK_TRACKS; t++) sampled[t] = trk_sample_mask(e_, t, masks[t]);
        const QColor missing(220, 50, 47);

        // What is drawn is copied out under the lock and drawn after it: the
        // lock is the scheduling thread's and the note path's too, and a
        // repaint of a few thousand drawText calls must not hold either up.
        trk_lock(e_);
        const trk_song *s = trk_song_of(e_);
        const trk_pattern *pt = &s->pattern[ed_->pattern];
        const int lpb = s->lpb > 0 ? s->lpb : 4;
        int mutes = 0;
        for (int t = 0; t < TRK_TRACKS; t++) if (s->track[t].mute) mutes |= 1 << t;
        const int ntr = nt();

        const int r0 = std::max(0, ev->rect().top() / ch_);
        const int r1 = std::min(pt->rows - 1, ev->rect().bottom() / ch_);
        trk_cell cells[TRK_ROWS_MAX][TRK_TRACKS];
        if (r1 >= r0) std::memcpy(cells[r0], pt->cell[r0], sizeof cells[0] * size_t(r1 - r0 + 1));
        trk_unlock(e_);
        QColor beat = pal.color(QPalette::AlternateBase);
        QColor bar = pal.color(QPalette::Mid);
        bar.setAlpha(60);
        QColor play = pal.color(QPalette::Highlight);
        play.setAlpha(70);
        // The cursor's row in the highlight colour while keys write into the
        // pattern, and grey while they only play.
        QColor cursorRow = editing_ ? pal.color(QPalette::Highlight) : QColor(128, 128, 128);
        cursorRow.setAlpha(editing_ ? 28 : 60);
        QColor dim = pal.color(QPalette::Text);
        dim.setAlpha(110);
        QColor faint = pal.color(QPalette::Text);
        faint.setAlpha(55);
        QColor selTint = pal.color(QPalette::Highlight);
        selTint.setAlpha(80);

        for (int r = r0; r <= r1; r++) {
            const int y = r * ch_;
            if (r % (lpb * 4) == 0)  p.fillRect(0, y, width(), ch_, bar);
            else if (r % lpb == 0)   p.fillRect(0, y, width(), ch_, beat);
            if (r == ed_->row)       p.fillRect(0, y, width(), ch_, cursorRow);
            if (ed_->pattern == playPat_ && r == playRow_)
                p.fillRect(0, y, width(), ch_, play);

            char num[8];
            std::snprintf(num, sizeof num, "%02X", r);
            p.setPen(r % lpb == 0 ? pal.color(QPalette::Text) : dim);
            p.drawText(cw_ / 2, y + asc_, QString::fromLatin1(num));

            for (int t = 0; t < ntr; t++) {
                const int x = gutter() + t * colWidth();
                char txt[TRK_CELL_CHARS + 1];
                trk_cell_text(&cells[r][t], txt);
                if (trk_selected(ed_, r, t))
                    p.fillRect(x - cw_ / 2, y, colWidth(), ch_, selTint);
                if (r == ed_->row && t == ed_->track) {
                    static const int start[TRK_FIELDS] = { TRK_COL_NOTE, TRK_COL_VEL,
                                                          TRK_COL_CC, TRK_COL_VAL };
                    static const int len[TRK_FIELDS] = { 3, 2, 2, 2 };
                    QRect fr(x + start[ed_->field] * cw_ - 1, y,
                             len[ed_->field] * cw_ + 2, ch_);
                    p.fillRect(fr, pal.color(QPalette::Highlight));
                }
                // Each field drawn on its own so the empty dots can be
                // fainter than what is actually there.
                for (int c = 0; c < TRK_CELL_CHARS; c++) {
                    const bool cur = r == ed_->row && t == ed_->track &&
                        ((ed_->field == TRK_F_NOTE && c < 3) ||
                         (ed_->field == TRK_F_VEL && c >= 4 && c < 6) ||
                         (ed_->field == TRK_F_CC && c >= 7 && c < 9) ||
                         (ed_->field == TRK_F_VAL && c >= 10));
                    if (txt[c] == ' ') continue;
                    QColor col = txt[c] == '.' ? faint : pal.color(QPalette::Text);
                    if (c >= 4 && txt[c] != '.') col = dim.lighter(100);
                    // A note its track's sample set has no sample on plays
                    // nothing: red, so it is seen before it is not heard.
                    const int nt = cells[r][t].note;
                    if (c < 3 && sampled[t] && nt <= 127 && !(masks[t][nt >> 3] & (1u << (nt & 7))))
                        col = missing;
                    if (mutes & (1 << t)) col.setAlpha(col.alpha() / 3);
                    if (cur) col = pal.color(QPalette::HighlightedText);
                    p.setPen(col);
                    p.drawText(x + c * cw_, y + asc_, QString(QChar(txt[c])));
                }
            }
        }

        p.setPen(faint);
        for (int t = 0; t <= ntr; t++) {
            const int x = gutter() + t * colWidth() - cw_;
            p.drawLine(x, ev->rect().top(), x, ev->rect().bottom());
        }
    }

    void keyPressEvent(QKeyEvent *ev) override
    {
        const int k = translate(ev);
        if (k < 0) { QWidget::keyPressEvent(ev); return; }
        // A held key repeats; a note or a digit must not, or holding one down
        // writes it into every row the cursor passes.
        if (ev->isAutoRepeat() && k < 0x100) return;
        if (k == TRK_K_TRACK_ADD)    { emit trackAddRequested(); return; }
        if (k == TRK_K_TRACK_REMOVE) { emit trackRemoveRequested(); return; }
        const quint32 sc = ev->nativeScanCode();
        if (k < 0x80 && sc > 0 && sc < sizeof down_) down_[sc] = (unsigned char)k;
        // A note typed: which track, and which note, before the cursor moves on.
        const int ntrack = ed_->track;
        const int nnote = ed_->field == TRK_F_NOTE || !ed_->edit ? trk_key_note(k, ed_->octave) : -1;
        const bool writes = ed_->edit;
        if (nnote >= 0) emit noteTyped(ntrack, nnote);
        if (trk_key(e_, ed_, k)) {
            updateSize();
            update();
            emit cursorMoved();
            if (writes && (k < 0x100 || k == TRK_K_DELETE || k == TRK_K_INSERT || k == TRK_K_BACKSPACE ||
                           k == TRK_K_CUT || k == TRK_K_PASTE || k == TRK_K_PASTE_MIX ||
                           (k >= TRK_K_TRANSPOSE_DOWN && k <= TRK_K_TRANSPOSE_OCT_UP)))
                emit edited();
            if (k == TRK_K_UNDO || k == TRK_K_REDO) emit edited();
            if (k == TRK_K_COPY || k == TRK_K_CUT || k == TRK_K_PASTE || k == TRK_K_PASTE_MIX) emit clipped(k);
            if (k == TRK_K_EDIT) emit editToggled();
            if (k == TRK_K_MUTE_TRACK || k == TRK_K_SOLO_TRACK || k == TRK_K_UNMUTE_ALL) emit mutesChanged();
            if (k == TRK_K_STEP_UP || k == TRK_K_STEP_DOWN) emit stepChanged();
        }
    }

    void keyReleaseEvent(QKeyEvent *ev) override
    {
        if (ev->isAutoRepeat()) return;
        // Released as the character it went down as: with Shift pressed in
        // between, '2' comes back up as '@' and would end nothing.
        const quint32 sc = ev->nativeScanCode();
        if (sc > 0 && sc < sizeof down_) {
            if (down_[sc]) trk_key_release(e_, ed_, down_[sc]);
            down_[sc] = 0;
            return;
        }
        const QString t = ev->text();
        if (t.size() == 1 && t[0].unicode() < 0x80)
            trk_key_release(e_, ed_, t[0].toLower().unicode());
    }

    void focusOutEvent(QFocusEvent *ev) override
    {
        // A key released in another window never comes back here, and its
        // preview would sound until something else stopped it.
        for (int t = 0; t < TRK_TRACKS; t++)
            if (ed_->held[t]) { trk_preview_off(e_, t); ed_->held[t] = 0; }
        std::memset(down_, 0, sizeof down_);
        QWidget::focusOutEvent(ev);
    }

    // Where a point falls: track and row, -1 track for the row numbers.
    bool cellAt(QPointF pos, int *t, int *r, int *field) const
    {
        const int x = int(pos.x()) - gutter();
        trk_lock(e_);
        const int rows = trk_song_of(e_)->pattern[ed_->pattern].rows;
        trk_unlock(e_);
        *r = std::clamp(int(pos.y()) / ch_, 0, rows - 1);
        if (x < 0) { *t = -1; *field = 0; return true; }
        *t = std::min(x / colWidth(), nt() - 1);
        const int c = (x % colWidth()) / cw_;
        *field = c < 4 ? TRK_F_NOTE : c < 7 ? TRK_F_VEL : c < 10 ? TRK_F_CC : TRK_F_VAL;
        return true;
    }

    void moveCursor(int r, int t, int field)
    {
        trk_lock(e_);
        ed_->octave = trk_song_of(e_)->track[t].octave;    // each track's own
        trk_unlock(e_);
        ed_->row = r;
        ed_->track = t;
        ed_->field = field;
        ed_->digit = 0;
    }

    // Selecting: a click puts the cursor down and ends a selection; a drag
    // selects the block it covers; Shift-click grows the selection to the
    // cell; a click or drag on the row numbers selects whole rows.
    void mousePressEvent(QMouseEvent *ev) override
    {
        setFocus();
        int t, r, field;
        if (ev->button() == Qt::RightButton || !cellAt(ev->position(), &t, &r, &field)) return;
        if (t < 0) {
            rowDrag_ = true;
            dragR_ = (ev->modifiers() & Qt::ShiftModifier) && ed_->sel ? ed_->sel_r0 : r;
            trk_select(ed_, dragR_, 0, r, nt() - 1);
            ed_->row = r;
            ed_->track = 0;
        } else if (ev->modifiers() & Qt::ShiftModifier) {
            if (!ed_->sel) trk_select(ed_, ed_->row, ed_->track, ed_->row, ed_->track);
            trk_select(ed_, ed_->sel_r0, ed_->sel_t0, r, t);
            moveCursor(r, t, field);
        } else {
            trk_select_none(ed_);
            moveCursor(r, t, field);
            dragR_ = r;
            dragT_ = t;
            cellDrag_ = true;
        }
        update();
        emit cursorMoved();
    }

    void mouseMoveEvent(QMouseEvent *ev) override
    {
        int t, r, field;
        if (!(ev->buttons() & Qt::LeftButton) || !cellAt(ev->position(), &t, &r, &field)) return;
        if (rowDrag_) {
            trk_select(ed_, dragR_, 0, r, nt() - 1);
            ed_->track = 0;
        } else if (cellDrag_) {
            if (t < 0) t = 0;
            if (r == dragR_ && t == dragT_ && !ed_->sel) return;     // not moved off it yet
            trk_select(ed_, dragR_, dragT_, r, t);
            moveCursor(r, t, ed_->field);
        } else {
            return;
        }
        update();
        emit cursorMoved();
    }

    void mouseReleaseEvent(QMouseEvent *) override { rowDrag_ = cellDrag_ = false; }

    // Right-click: what can be done with the selection -- or, clicked
    // outside it, with the cell under the pointer.
    void contextMenuEvent(QContextMenuEvent *ev) override
    {
        int t, r, field;
        if (!cellAt(ev->pos(), &t, &r, &field)) return;
        if (t >= 0 && !trk_selected(ed_, r, t)) { trk_select_none(ed_); moveCursor(r, t, field); }
        if (t < 0 && !trk_selected(ed_, r, 0)) { trk_select(ed_, r, 0, r, nt() - 1); ed_->track = 0; }
        update();
        emit cursorMoved();

        int crows = 0, ctracks = 0;
        trk_clipboard(&crows, &ctracks);
        QMenu m(this);
        QAction *undo  = m.addAction("Undo", QKeySequence::Undo);
        m.addSeparator();
        QAction *copy  = m.addAction("Copy", QKeySequence::Copy);
        QAction *cut   = m.addAction("Cut", QKeySequence::Cut);
        QAction *paste = m.addAction(crows ? QString("Paste %1 row%2 x %3 track%4")
                                                 .arg(crows).arg(crows > 1 ? "s" : "")
                                                 .arg(ctracks).arg(ctracks > 1 ? "s" : "")
                                           : QString("Paste"), QKeySequence::Paste);
        QAction *clear = m.addAction("Clear", QKeySequence::Delete);
        m.addSeparator();
        QAction *col   = m.addAction("Select This Track's Column");
        QAction *all   = m.addAction("Select All", QKeySequence::SelectAll);
        paste->setEnabled(crows > 0 && ed_->edit);
        cut->setEnabled(ed_->edit);
        clear->setEnabled(ed_->edit);
        QAction *got = m.exec(ev->globalPos());
        if (!got) return;
        if (got == undo)       key(TRK_K_UNDO);
        else if (got == copy)  key(TRK_K_COPY);
        else if (got == cut)   key(TRK_K_CUT);
        else if (got == paste) key(TRK_K_PASTE);
        else if (got == clear) { trk_clear_block(e_, ed_); emit edited(); }
        else if (got == col) {
            trk_lock(e_);
            const int rows = trk_song_of(e_)->pattern[ed_->pattern].rows;
            trk_unlock(e_);
            const int tr = std::max(0, t);
            trk_select(ed_, 0, tr, rows - 1, tr);
            ed_->row = 0;
        } else if (got == all) key(TRK_K_SEL_ALL);
        update();
        emit cursorMoved();
    }

    void key(int k)
    {
        trk_key(e_, ed_, k);
        if (k == TRK_K_CUT || k == TRK_K_PASTE) emit edited();
        if (k == TRK_K_COPY || k == TRK_K_CUT || k == TRK_K_PASTE) emit clipped(k);
    }

    bool focusNextPrevChild(bool) override { return false; }   // Tab is ours

private:
    static int translate(QKeyEvent *ev)
    {
        const bool ctrl = ev->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        const bool shift = ev->modifiers() & Qt::ShiftModifier;
        const bool keypad = ev->modifiers() & Qt::KeypadModifier;
        // Furnace's keys, where they fit: Ctrl+Y redo, Ctrl+Shift+V paste mix,
        // Ctrl+F1-F4 transpose, Alt+F9/F10 mute and solo, Shift+PgUp/PgDn a
        // page of selection, Shift+Enter play from the cursor, the keypad's *
        // and / the octave (with Ctrl, the edit step).
        if (ev->modifiers() & Qt::AltModifier) {
            switch (ev->key()) {
            case Qt::Key_F9:  return shift ? TRK_K_UNMUTE_ALL : TRK_K_MUTE_TRACK;
            case Qt::Key_F10: return TRK_K_SOLO_TRACK;
            default: break;
            }
        }
        // Shift with the arrows selects; Ctrl with C, X, V and A is the clipboard.
        if (shift) {
            switch (ev->key()) {
            case Qt::Key_Up:    return TRK_K_SEL_UP;
            case Qt::Key_Down:  return TRK_K_SEL_DOWN;
            case Qt::Key_Left:  return TRK_K_SEL_LEFT;
            case Qt::Key_Right: return TRK_K_SEL_RIGHT;
            case Qt::Key_PageUp:   return TRK_K_SEL_PGUP;
            case Qt::Key_PageDown: return TRK_K_SEL_PGDN;
            case Qt::Key_Return: case Qt::Key_Enter:
                if (!(ev->modifiers() & Qt::ControlModifier)) return TRK_K_PLAY_FROM_CURSOR;
                break;
            default: break;
            }
        }
        if (ev->modifiers() & Qt::ControlModifier) {
            switch (ev->key()) {
            case Qt::Key_C: return TRK_K_COPY;
            case Qt::Key_X: return TRK_K_CUT;
            case Qt::Key_V: return shift ? TRK_K_PASTE_MIX : TRK_K_PASTE;
            case Qt::Key_A: return TRK_K_SEL_ALL;
            case Qt::Key_Z: return shift ? TRK_K_REDO : TRK_K_UNDO;
            case Qt::Key_Y: return TRK_K_REDO;
            case Qt::Key_F1: return TRK_K_TRANSPOSE_DOWN;
            case Qt::Key_F2: return TRK_K_TRANSPOSE_UP;
            case Qt::Key_F3: return TRK_K_TRANSPOSE_OCT_DOWN;
            case Qt::Key_F4: return TRK_K_TRANSPOSE_OCT_UP;
            case Qt::Key_Asterisk: if (keypad) return TRK_K_STEP_UP; break;
            case Qt::Key_Slash:    if (keypad) return TRK_K_STEP_DOWN; break;
            case Qt::Key_Insert:   return TRK_K_TRACK_ADD;
            case Qt::Key_Delete:   return TRK_K_TRACK_REMOVE;
            default: break;
            }
        }
        switch (ev->key()) {
        case Qt::Key_Up:        return TRK_K_UP;
        case Qt::Key_Down:      return TRK_K_DOWN;
        case Qt::Key_Left:      return TRK_K_LEFT;
        case Qt::Key_Right:     return TRK_K_RIGHT;
        case Qt::Key_PageUp:    return TRK_K_PGUP;
        case Qt::Key_PageDown:  return TRK_K_PGDN;
        case Qt::Key_Home:      return TRK_K_HOME;
        case Qt::Key_End:       return TRK_K_END;
        case Qt::Key_Tab:       return TRK_K_TAB;
        case Qt::Key_Backtab:   return TRK_K_BACKTAB;
        case Qt::Key_Delete:    return TRK_K_DELETE;
        case Qt::Key_Backspace: return TRK_K_BACKSPACE;
        case Qt::Key_Insert:    return TRK_K_INSERT;
        case Qt::Key_F5:        return TRK_K_PLAY_SONG;
        case Qt::Key_F6:        return TRK_K_PLAY_PATTERN;
        case Qt::Key_F7:        return TRK_K_RECORD;
        case Qt::Key_F8:        return TRK_K_STOP;
        case Qt::Key_Space:     return TRK_K_EDIT;
        case Qt::Key_Return: case Qt::Key_Enter: return TRK_K_TOGGLE;
        case Qt::Key_Asterisk:  if (keypad) return TRK_K_OCT_UP; break;
        case Qt::Key_Slash:     if (keypad) return TRK_K_OCT_DOWN; break;
        case Qt::Key_BracketLeft:  return TRK_K_OCT_DOWN;
        case Qt::Key_BracketRight: return TRK_K_OCT_UP;
        case Qt::Key_Minus:     return TRK_K_PAT_PREV;
        case Qt::Key_Equal:     return TRK_K_PAT_NEXT;
        case Qt::Key_QuoteLeft: return TRK_K_EDIT;
        default: break;
        }
        if (ctrl) return -1;
        const QString t = ev->text();
        if (t.size() == 1 && t[0].unicode() > 0x20 && t[0].unicode() < 0x7f)
            return t[0].toLower().unicode();
        return -1;
    }

    trk_engine *e_;
    trk_editor *ed_;
    unsigned char down_[256] = {};   // character each held key went down as, by scancode
    int cw_ = 8, ch_ = 16, asc_ = 12;
    int playPat_ = -1, playRow_ = -1;
    bool editing_ = true;
    bool rowDrag_ = false, cellDrag_ = false;   // a drag selecting rows, or a block
    int dragR_ = 0, dragT_ = 0;                 // where it began
};

// A piano of the typing keys, for the set editor: every key shows the sample
// on it, a click plays it, and a sample dragged from the list (or picked up
// with Learn and a key press) lands on the key it is dropped on.
class KeyMapWidget : public QWidget {
    Q_OBJECT
public:
    explicit KeyMapWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAcceptDrops(true);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
    }
    static constexpr int kSpan = 29;           // the keys z..p reach
    static const char *typingKeys() { return "zsxdcvgbhnjmq2w3er5t6y7ui9o0p"; }

    void setOctave(int o) { octave_ = std::clamp(o, 0, 8); update(); }
    int octave() const { return octave_; }
    void setNames(const QHash<int, QString> &n) { names_ = n; update(); }
    void setCurrent(int note) { cur_ = note; update(); }
    int baseNote() const { return (octave_ + 1) * 12; }

signals:
    void played(int note);                     // clicked, or its typing key pressed
    void dropped(int row, int note);           // a list row dropped on a key
    void menuAt(int note, QPoint global);

protected:
    bool isBlack(int idx) const { int m = idx % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; }
    int whiteIndex(int idx) const   // white keys before this semitone
    {
        static const int pre[12] = { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };
        return (idx / 12) * 7 + pre[idx % 12];
    }
    QRectF keyRect(int idx) const
    {
        const double ww = double(width()) / 17.0, h = height();
        if (isBlack(idx)) {
            const double x = whiteIndex(idx) * ww;      // the boundary after its white
            return QRectF(x - ww * 0.3, 0, ww * 0.6, h * 0.6);
        }
        return QRectF(whiteIndex(idx) * ww, 0, ww, h);
    }
    int keyAt(const QPointF &pt) const
    {
        for (int pass = 0; pass < 2; pass++)            // black keys sit on top
            for (int i = 0; i < kSpan; i++)
                if (isBlack(i) == (pass == 0) && keyRect(i).contains(pt)) {
                    const int n = baseNote() + i;
                    return n >= 0 && n <= 127 ? n : -1;
                }
        return -1;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        const QPalette &pal = palette();
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < kSpan; i++) {
                if (isBlack(i) != (pass == 1)) continue;
                const int n = baseNote() + i;
                if (n > 127) continue;
                const QRectF r = keyRect(i).adjusted(0.5, 0.5, -0.5, -0.5);
                const bool has = names_.contains(n);
                QColor fill = isBlack(i) ? QColor(50, 50, 55) : QColor(245, 245, 245);
                if (has) fill = isBlack(i) ? QColor(35, 110, 170) : QColor(150, 205, 245);
                if (n == cur_) fill = isBlack(i) ? QColor(210, 130, 20) : QColor(255, 205, 110);
                p.setPen(QColor(90, 90, 90));
                p.setBrush(fill);
                p.drawRect(r);
                const QColor ink = isBlack(i) && !has && n != cur_ ? QColor(220, 220, 220)
                                   : (isBlack(i) ? QColor(255, 255, 255) : QColor(20, 20, 20));
                p.setPen(ink);
                QFont f = font();
                f.setPointSizeF(std::max(7.0, f.pointSizeF() * 0.85));
                p.setFont(f);
                const QFontMetrics fm(f);
                char nn[5];
                drumkit_note_name(n, nn);
                const QString key = QString(QChar(typingKeys()[i])).toUpper();
                p.drawText(QRectF(r.left(), r.bottom() - fm.height() * 2 - 2, r.width(), fm.height()),
                           Qt::AlignHCenter, key);
                p.drawText(QRectF(r.left(), r.bottom() - fm.height() - 1, r.width(), fm.height()),
                           Qt::AlignHCenter, QString::fromLatin1(nn));
                if (has) {                              // the sample's name, up the key
                    p.save();
                    p.translate(r.center().x() + fm.ascent() / 2.0 - 1,
                                r.bottom() - fm.height() * 2 - 6);
                    p.rotate(-90);
                    const double room = r.height() - fm.height() * 2 - 12;
                    p.drawText(QPointF(0, 0), fm.elidedText(names_.value(n), Qt::ElideRight, int(room)));
                    p.restore();
                }
            }
        if (hasFocus()) { p.setPen(pal.color(QPalette::Highlight)); p.setBrush(Qt::NoBrush);
                          p.drawRect(rect().adjusted(0, 0, -1, -1)); }
    }

    void mousePressEvent(QMouseEvent *ev) override
    {
        setFocus();
        const int n = keyAt(ev->position());
        if (n < 0) return;
        if (ev->button() == Qt::RightButton) emit menuAt(n, ev->globalPosition().toPoint());
        else emit played(n);
    }
    void keyPressEvent(QKeyEvent *ev) override
    {
        if (ev->isAutoRepeat() || ev->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
            QWidget::keyPressEvent(ev); return;
        }
        const QString t = ev->text().toLower();
        const int n = t.size() == 1 ? trk_key_note(t[0].toLatin1(), octave_) : -1;
        if (n >= 0) emit played(n);
        else QWidget::keyPressEvent(ev);
    }
    void dragEnterEvent(QDragEnterEvent *ev) override
    {
        if (ev->mimeData()->hasFormat("application/x-qabstractitemmodeldatalist")) ev->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *ev) override
    {
        const int n = keyAt(ev->position());
        cur_ = n; update();
        if (n >= 0) ev->acceptProposedAction(); else ev->ignore();
    }
    void dropEvent(QDropEvent *ev) override
    {
        const int n = keyAt(ev->position());
        QByteArray d = ev->mimeData()->data("application/x-qabstractitemmodeldatalist");
        QDataStream ds(&d, QIODevice::ReadOnly);
        int row = -1, col = 0;
        if (!ds.atEnd()) ds >> row >> col;
        if (n >= 0 && row >= 0) { ev->acceptProposedAction(); emit dropped(row, n); }
    }

private:
    int octave_ = 4, cur_ = -1;
    QHash<int, QString> names_;
};

// ------------------------------------------------------ the set editor --
//
// Samples > Edit Sample Set: which note plays which WAV in a set, its gain
// and choke group, WAVs added and taken out -- and saved as the set's
// kit.txt, which every track playing the set then plays. The set is a
// dk_map from drumkit.h, so this and the GTK editor read and write the same.

class SetEditor : public QDialog {
    Q_OBJECT
public:
    SetEditor(trk_engine *e, const QString &start, QWidget *parent)
        : QDialog(parent), e_(e), map_(new dk_map)
    {
        setWindowTitle("Edit Sample Set");
        resize(860, 700);
        auto *v = new QVBoxLayout(this);
        auto *top = new QHBoxLayout;
        sets_ = new QComboBox;
        sets_->setMinimumContentsLength(30);
        static char buf[32768];
        trk_list_sample_sets(e_, buf, sizeof buf);
        for (const QString &l : QString::fromUtf8(buf).split('\n', Qt::SkipEmptyParts))
            sets_->addItem(l.section('\t', 0, 0), l.section('\t', 0, 0));
        int at = sets_->findData(start);
        if (at < 0 && !start.isEmpty()) { sets_->addItem(start, start); at = sets_->count() - 1; }
        top->addWidget(new QLabel("Set:"));
        top->addWidget(sets_, 1);
        v->addLayout(top);
        dir_ = new QLabel;
        dir_->setStyleSheet("color: #777;");
        dir_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(dir_);

        keys_ = new KeyMapWidget;
        auto *kbar = new QHBoxLayout;
        kbar->addWidget(new QLabel("Keys from octave"));
        octave_ = new QSpinBox;
        octave_->setRange(0, 8);
        octave_->setValue(4);
        kbar->addWidget(octave_);
        learn_ = new QPushButton("Learn");
        learn_->setCheckable(true);
        learn_->setToolTip("Pick a sample in the list, press Learn, then click a key "
                           "or press its typing key: the sample moves onto it");
        kbar->addWidget(learn_);
        auto *hint = new QLabel("Click a key to hear it. Drag a sample from the list onto a key, "
                                "or use Learn. Right-click a key for more.");
        hint->setStyleSheet("color: #777;");
        hint->setWordWrap(true);
        kbar->addWidget(hint, 1);
        v->addLayout(kbar);
        v->addWidget(keys_);
        outside_ = new QLabel;
        outside_->setStyleSheet("color: #777;");
        v->addWidget(outside_);

        table_ = new QTableWidget(0, 5);
        table_->setDragEnabled(true);
        table_->setDragDropMode(QAbstractItemView::DragOnly);
        table_->setHorizontalHeaderLabels({ "Note", "Sample", "Gain dB", "Choke", "" });
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table_->verticalHeader()->setVisible(false);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        v->addWidget(table_, 1);

        auto *row = new QHBoxLayout;
        auto *add = new QPushButton("Add WAVs…");
        auto *del = new QPushButton("Remove");
        auto *save = new QPushButton("Save");
        auto *close = new QPushButton("Close");
        save->setDefault(true);
        row->addWidget(add);
        row->addWidget(del);
        row->addStretch(1);
        row->addWidget(save);
        row->addWidget(close);
        v->addLayout(row);
        status_ = new QLabel;
        status_->setWordWrap(true);
        v->addWidget(status_);

        connect(sets_, &QComboBox::activated, this, [this](int) {
            if (dirty_ && !askDiscard()) { sets_->setCurrentIndex(sets_->findData(name_)); return; }
            load(sets_->currentData().toString());
        });
        connect(add, &QPushButton::clicked, this, &SetEditor::addWavs);
        connect(del, &QPushButton::clicked, this, &SetEditor::removeRows);
        connect(save, &QPushButton::clicked, this, &SetEditor::save);
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        connect(table_, &QTableWidget::itemChanged, this, [this] { if (!filling_) dirty_ = true; refreshKeys(); });
        connect(octave_, &QSpinBox::valueChanged, this, [this](int o) { keys_->setOctave(o); refreshKeys(); });
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
            const int r = selectedRow();
            keys_->setCurrent(r >= 0 ? noteOfRow(r) : -1);
        });
        connect(keys_, &KeyMapWidget::played, this, &SetEditor::keyPlayed);
        connect(keys_, &KeyMapWidget::dropped, this, [this](int row, int note) { assignRow(row, note); });
        connect(keys_, &KeyMapWidget::menuAt, this, &SetEditor::keyMenu);

        if (at >= 0) sets_->setCurrentIndex(at);
        if (sets_->count()) load(sets_->currentData().toString());
    }

signals:
    void saved();

protected:
    void closeEvent(QCloseEvent *ev) override
    {
        if (dirty_ && !askDiscard()) { ev->ignore(); return; }
        dirty_ = false;
        ev->accept();
    }
    void reject() override { close(); }      // Escape asks too

private:
    bool askDiscard()
    {
        return QMessageBox::question(this, "Edit Sample Set",
                   "Discard the changes to " + name_ + "?",
                   QMessageBox::Discard | QMessageBox::Cancel) == QMessageBox::Discard;
    }

    QString setDir() const
    {
        char dir[TRK_PATH_LEN] = "";
        trk_sample_set_dir(e_, name_.toUtf8().constData(), dir, sizeof dir);
        return QString::fromUtf8(dir);
    }

    void load(const QString &name)
    {
        name_ = name;
        dirty_ = false;
        const QString dir = setDir();
        if (dir.isEmpty()) {
            std::memset(map_.get(), 0, sizeof *map_);
            dir_->setText("not found");
        } else {
            drumkit_map_read(map_.get(), dir.toUtf8().constData());
            dir_->setText(dir + (map_->mapped ? "   (kit.txt)"
                                              : "   (no kit.txt yet: every WAV from C-4 up)"));
        }
        fill();
        status_->clear();
    }

    void addRow(int note, const QString &file, double gain, int choke)
    {
        char nn[5] = "";
        if (note >= 0) drumkit_note_name(note, nn);
        const int r = table_->rowCount();
        table_->insertRow(r);
        table_->setItem(r, 0, new QTableWidgetItem(QString::fromLatin1(nn)));
        auto *f = new QTableWidgetItem(QFileInfo(file).completeBaseName());
        f->setData(Qt::UserRole, file);
        f->setToolTip(file);
        f->setFlags(f->flags() & ~Qt::ItemIsEditable);
        table_->setItem(r, 1, f);
        auto *g = new QDoubleSpinBox;
        g->setRange(-60, 24);
        g->setDecimals(1);
        g->setValue(gain);
        auto *c = new QSpinBox;
        c->setRange(0, 99);
        c->setSpecialValueText("none");
        c->setValue(choke);
        auto *play = new QPushButton("▶");
        play->setToolTip("Listen to it");
        play->setFixedWidth(36);
        table_->setCellWidget(r, 2, g);
        table_->setCellWidget(r, 3, c);
        table_->setCellWidget(r, 4, play);
        connect(g, &QDoubleSpinBox::valueChanged, this, [this] { dirty_ = true; });
        connect(c, &QSpinBox::valueChanged, this, [this] { dirty_ = true; });
        connect(play, &QPushButton::clicked, this, [this, f, g] {
            if (trk_audition(e_, setDir().toUtf8().constData(),
                             f->data(Qt::UserRole).toString().toUtf8().constData(), g->value(), 100))
                status_->setText("that WAV would not play. " + QString::fromUtf8(trk_audio_status(e_)));
        });
    }

    int selectedRow() const
    {
        const auto rows = table_->selectionModel()->selectedRows();
        return rows.isEmpty() ? -1 : rows.first().row();
    }
    int noteOfRow(int r) const
    {
        if (!table_->item(r, 0)) return -1;     // a row still being built
        return drumkit_note_parse(table_->item(r, 0)->text().trimmed().toUtf8().constData());
    }
    int rowOfNote(int note) const
    {
        for (int r = 0; r < table_->rowCount(); r++) if (noteOfRow(r) == note) return r;
        return -1;
    }
    void audition(int r)
    {
        auto *g = static_cast<QDoubleSpinBox *>(table_->cellWidget(r, 2));
        if (trk_audition(e_, setDir().toUtf8().constData(),
                         table_->item(r, 1)->data(Qt::UserRole).toString().toUtf8().constData(),
                         g->value(), 100))
            status_->setText("that WAV would not play. " + QString::fromUtf8(trk_audio_status(e_)));
    }

    // The keyboard shows what the list says, so it is rebuilt from the list.
    void refreshKeys()
    {
        QHash<int, QString> names;
        int outside = 0;
        const int lo = keys_->baseNote(), hi = lo + KeyMapWidget::kSpan;
        for (int r = 0; r < table_->rowCount(); r++) {
            const int n = noteOfRow(r);
            if (n < 0 || !table_->item(r, 1)) continue;
            names.insert(n, table_->item(r, 1)->text());
            if (n < lo || n >= hi) outside++;
        }
        keys_->setNames(names);
        outside_->setText(outside ? QString("%1 sample%2 on keys outside these octaves -- change "
                                            "\"Keys from octave\" to see them")
                                        .arg(outside).arg(outside == 1 ? "" : "s")
                                  : QString());
    }

    // Put a sample on a note. A sample already there swaps to the one's old
    // note, so nothing is lost and no two pads share a key.
    void assignRow(int row, int note)
    {
        if (row < 0 || row >= table_->rowCount() || note < 0 || note > 127) return;
        const int old = noteOfRow(row), other = rowOfNote(note);
        char nn[5];
        filling_ = true;
        if (other >= 0 && other != row && old >= 0) {
            drumkit_note_name(old, nn);
            table_->item(other, 0)->setText(QString::fromLatin1(nn));
        }
        drumkit_note_name(note, nn);
        table_->item(row, 0)->setText(QString::fromLatin1(nn));
        filling_ = false;
        dirty_ = true;
        table_->selectRow(row);
        refreshKeys();
        keys_->setCurrent(note);
        status_->setText(QString("%1 is on %2 -- Save to keep it")
                             .arg(table_->item(row, 1)->text(), QString::fromLatin1(nn)));
        audition(row);
    }

    void keyPlayed(int note)
    {
        if (learn_->isChecked()) {
            const int r = selectedRow();
            if (r >= 0) assignRow(r, note);
            else status_->setText("pick a sample in the list first");
            learn_->setChecked(false);
            return;
        }
        const int r = rowOfNote(note);
        keys_->setCurrent(note);
        if (r < 0) { table_->clearSelection(); return; }
        table_->selectRow(r);
        keys_->setCurrent(note);
        audition(r);
    }

    void keyMenu(int note, QPoint at)
    {
        const int r = rowOfNote(note), sel = selectedRow();
        char nn[5];
        drumkit_note_name(note, nn);
        QMenu m;
        QAction *put = m.addAction(sel >= 0 ? QString("Put \"%1\" on %2").arg(table_->item(sel, 1)->text(), nn)
                                            : QString("Put the selected sample on %1").arg(nn));
        put->setEnabled(sel >= 0);
        QAction *pick = m.addAction(QString("Choose a WAV for %1…").arg(nn));
        QAction *clear = m.addAction(QString("Take the sample off %1").arg(nn));
        clear->setEnabled(r >= 0);
        QAction *got = m.exec(at);
        if (got == put) assignRow(sel, note);
        else if (got == pick) {
            const QString dir = setDir();
            if (dir.isEmpty()) return;
            const QString f = QFileDialog::getOpenFileName(this, QString("A WAV for %1").arg(nn), dir,
                                                           "WAV files (*.wav *.WAV)");
            if (f.isEmpty()) return;
            if (r >= 0) table_->removeRow(r);
            const QString base = dir + '/';
            filling_ = true;
            addRow(note, f.startsWith(base) ? f.mid(base.size()) : f, 0.0, 0);
            filling_ = false;
            dirty_ = true;
            refreshKeys();
            audition(table_->rowCount() - 1);
        } else if (got == clear && r >= 0) {
            table_->removeRow(r);
            dirty_ = true;
            refreshKeys();
        }
    }

    void fill()
    {
        filling_ = true;
        table_->setRowCount(0);
        for (int i = 0; i < map_->n; i++)
            addRow(map_->pad[i].note, QString::fromUtf8(map_->pad[i].file),
                   map_->pad[i].gain_db, map_->pad[i].choke);
        table_->resizeColumnToContents(0);
        filling_ = false;
        int lowest = 127;
        for (int i = 0; i < map_->n; i++) lowest = std::min(lowest, map_->pad[i].note);
        if (map_->n) octave_->setValue(std::clamp(lowest / 12 - 1, 0, 8));
        keys_->setOctave(octave_->value());
        refreshKeys();
    }

    void addWavs()
    {
        const QString dir = setDir();
        if (dir.isEmpty()) return;
        const QStringList files = QFileDialog::getOpenFileNames(this, "Add WAVs", dir,
                                                                "WAV files (*.wav *.WAV)");
        const QString base = dir + '/';
        for (const QString &f : files) {
            // The next note above every pad there is.
            int note = DK_BASE_NOTE - 1;
            for (int r = 0; r < table_->rowCount(); r++)
                note = std::max(note, drumkit_note_parse(table_->item(r, 0)->text().trimmed()
                                                             .toUtf8().constData()));
            if (note >= 127) { status_->setText("no notes left above the last pad"); break; }
            // From the set's own folder, by name; from anywhere else, by path.
            addRow(note + 1, f.startsWith(base) ? f.mid(base.size()) : f, 0.0, 0);
            dirty_ = true;
            refreshKeys();
        }
    }

    void removeRows()
    {
        QList<int> rows;
        for (const QModelIndex &i : table_->selectionModel()->selectedRows()) rows << i.row();
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int r : rows) table_->removeRow(r);
        if (!rows.isEmpty()) dirty_ = true;
        refreshKeys();
    }

    void save()
    {
        if (name_.isEmpty() || setDir().isEmpty()) return;
        dk_map *m = map_.get();
        m->n = 0;
        for (int r = 0; r < table_->rowCount() && m->n < DK_MAX_SAMPLES; r++) {
            dk_pad *p = &m->pad[m->n++];
            const QString nt = table_->item(r, 0)->text().trimmed();
            p->note = drumkit_note_parse(nt.toUtf8().constData());
            if (p->note < 0) {
                status_->setText(QString("row %1: \"%2\" is not a note -- C-4, F#5, or 0-127")
                                     .arg(r + 1).arg(nt));
                return;
            }
            std::snprintf(p->file, sizeof p->file, "%s",
                          table_->item(r, 1)->data(Qt::UserRole).toString().toUtf8().constData());
            p->gain_db = static_cast<QDoubleSpinBox *>(table_->cellWidget(r, 2))->value();
            p->choke = static_cast<QSpinBox *>(table_->cellWidget(r, 3))->value();
        }
        if (const char *why = drumkit_map_check(m)) { status_->setText(QString::fromUtf8(why)); return; }
        if (drumkit_map_save(m)) {
            status_->setText(QString("could not save %1/kit.txt: %2")
                                 .arg(QString::fromUtf8(m->dir), QString::fromUtf8(std::strerror(errno))));
            return;
        }
        m->mapped = 1;
        dirty_ = false;
        trk_reload_sample_set(e_, name_.toUtf8().constData());
        dir_->setText(QString::fromUtf8(m->dir) + "   (kit.txt)");
        status_->setText(QString("saved %1/kit.txt -- %2 pads; tracks playing this set play it now")
                             .arg(QString::fromUtf8(m->dir)).arg(m->n));
        emit saved();
    }

    trk_engine *e_;
    std::unique_ptr<dk_map> map_;
    QComboBox *sets_;
    QLabel *dir_, *status_;
    QTableWidget *table_;
    KeyMapWidget *keys_;
    QSpinBox *octave_;
    QPushButton *learn_;
    QLabel *outside_;
    QString name_;
    bool dirty_ = false, filling_ = false;
};

// -------------------------------------------------------- the host interface --
//
// TrackerWidget is the whole tracker UI as a plain widget, so a shell can
// host it in a tab. What it needs from whatever window holds it -- top-level
// menus to fill, a status line to write to, a way to ask to quit -- is a
// TrackerHost; the standalone window at the bottom is one.

class TrackerHost {
public:
    virtual ~TrackerHost() = default;
    virtual QMenu *addMenu(const QString &title) = 0;          // a top-level menu, to fill
    virtual void showStatus(const QString &msg, int ms) = 0;   // on the status line; ms a timeout
    virtual void requestQuit() = 0;                            // File > Quit
    // In-process destinations the shell can play a track to directly, beyond
    // the ALSA windows trk_list_dests offers -- studio's synth tabs, named by
    // plug-in. The defaults are no destinations and a pick ignored, which is
    // the standalone tracker's whole answer: nothing there changes.
    virtual QStringList midiSinks() { return {}; }
    // A track's destination picked one of midiSinks -- or a window again
    // (name empty). The shell makes the routing change.
    virtual void midiSinkPicked(int track, const QString &name) { (void)track; (void)name; }
    // A song was just loaded into the engine (openPath succeeded). A shell
    // with synth tabs reopens the ones the song's sink lines name; the
    // default is nothing, the standalone's whole answer.
    virtual void songOpened() {}
};

// ------------------------------------------------------------- the widget --

class TrackerWidget : public QWidget {
    Q_OBJECT
public:
    TrackerWidget(trk_engine *e, TrackerHost *host, QWidget *parent = nullptr)
        : QWidget(parent), e_(e), host_(host), saved_(new trk_song)
    {
        trk_editor_init(&ed_);
        trk_song_init(saved_.get());

        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(6, 6, 6, 0);
        v->setSpacing(4);

        // Transport and song settings.
        auto *bar = new QHBoxLayout;
        auto *playSong = new QPushButton("▶ Song");
        auto *playPat = new QPushButton("▶ Pattern");
        auto *stop = new QPushButton("■ Stop");
        auto *panic = new QPushButton("Panic");
        recBtn_ = new QPushButton("● Rec");
        auto *recOpt = new QPushButton("Rec…");
        recBtn_->setToolTip("Record: play, and write the notes you play into the pattern, on the cursor's "
                            "track (F7)");
        recOpt->setToolTip("Recording options: count-in, metronome, quantizing, MIDI input");
        recBtn_->setFocusPolicy(Qt::NoFocus);
        recOpt->setFocusPolicy(Qt::NoFocus);
        playSong->setToolTip("Play the song from the order entry holding this pattern (F5)");
        playPat->setToolTip("Loop this pattern (F6)");
        stop->setToolTip("Stop and release every note (F8)");
        panic->setToolTip("Release every note on every track, playing or not (Escape)");
        bpm_ = new QDoubleSpinBox;
        bpm_->setRange(20, 999);
        bpm_->setDecimals(1);
        bpm_->setSuffix(" bpm");
        lpb_ = new QComboBox;
        for (int l : { 1, 2, 3, 4, 6, 8, 12, 16 }) lpb_->addItem(QString("%1 rows/beat").arg(l), l);
        pattern_ = new QSpinBox;
        pattern_->setRange(0, TRK_PATTERNS - 1);
        pattern_->setPrefix("pattern ");
        rows_ = new QSpinBox;
        rows_->setRange(1, TRK_ROWS_MAX);
        rows_->setSuffix(" rows");
        rows_->setToolTip("How many rows this part has");
        step_ = new QSpinBox;
        step_->setRange(0, 16);
        step_->setPrefix("step ");
        follow_ = new QCheckBox("follow");
        follow_->setChecked(true);
        follow_->setToolTip("Keep the cursor on the row that is playing");
        edit_ = new QCheckBox("edit");
        edit_->setChecked(true);
        edit_->setToolTip("Keys write into the pattern. Off, note keys only play, to try "
                          "them out -- ` (backtick) turns it on and off");
        edit_->setFocusPolicy(Qt::NoFocus);
        for (QWidget *w : std::initializer_list<QWidget *>{ playSong, playPat, stop, panic, recBtn_, recOpt, bpm_, lpb_,
                                                           pattern_, step_, follow_,
                                                           edit_ })
            bar->addWidget(w);
        // Master volume: what the tracker sounds itself -- the sample tracks.
        // The windows that play the MIDI tracks have their own.
        volLabel_ = new QLabel("vol 100%");
        volume_ = new QSlider(Qt::Horizontal);
        volume_->setRange(0, 150);
        volume_->setValue(100);
        volume_->setFixedWidth(120);
        volume_->setFocusPolicy(Qt::NoFocus);
        volume_->setToolTip("Master volume of the sample tracks (the windows playing the MIDI "
                            "tracks have their own)");
        bar->addSpacing(8);
        bar->addWidget(volLabel_);
        bar->addWidget(volume_);
        bar->addSpacing(8);
        auto *addTr = new QPushButton("+ Track");
        auto *delTr = new QPushButton("− Track");
        addTr->setToolTip("Add an empty track after the cursor's (up to 16)");
        delTr->setToolTip("Remove the cursor's track, notes and all (undo brings it back)");
        addTr->setFocusPolicy(Qt::NoFocus);
        delTr->setFocusPolicy(Qt::NoFocus);
        connect(addTr, &QPushButton::clicked, this, [this] { addTrack(); });
        connect(delTr, &QPushButton::clicked, this, [this] { removeTrack(); });
        bar->addWidget(addTr);
        bar->addWidget(delTr);
        bar->addStretch(1);
        v->addLayout(bar);

        // Parts, left of the grid: the song in order, a part as often as it
        // plays. Grid and headers stack to the right of it.
        auto *split = new QHBoxLayout;
        auto *gridCol = new QVBoxLayout;
        split->addWidget(buildParts());
        split->addLayout(gridCol, 1);
        v->addLayout(split, 1);

        // The grid, with the track headers above it scrolled sideways with it.
        view_ = new PatternView(e_, &ed_);
        auto *head = new QWidget;
        headStrip_ = head;
        auto *hl = new QHBoxLayout(head);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(0);
        /* The grid draws a track's left divider one character left of its
         * text; a header spans divider to divider, so it sits over its column. */
        hl->addSpacing(view_->gutter() - view_->charWidth());
        for (int t = 0; t < TRK_TRACKS; t++) {
            auto *box = new QWidget;
            headBox_[t] = box;
            box->setFixedWidth(view_->colWidth());
            auto *bl = new QVBoxLayout(box);
            bl->setContentsMargins(3, 0, 3, 2);
            bl->setSpacing(2);
            name_[t] = new QLineEdit;
            dest_[t] = new QComboBox;
            dest_[t]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            dest_[t]->setMinimumContentsLength(8);
            dest_[t]->setToolTip("Which window this track plays");
            sample_[t] = new QComboBox;
            sample_[t]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            sample_[t]->setMinimumContentsLength(8);
            sample_[t]->setToolTip("A sample set for this track to play instead of a "
                                   "window: the note picks the sample");
            auto *row = new QHBoxLayout;
            row->setSpacing(4);
            chan_[t] = new QSpinBox;
            chan_[t]->setRange(1, 16);
            chan_[t]->setPrefix("ch ");
            chan_[t]->setToolTip("MIDI channel");
            oct_[t] = new QComboBox;
            for (int o = 0; o <= 9; o++) oct_[t]->addItem(QString("oct %1").arg(o), o);
            oct_[t]->setToolTip("The octave the note keys play on this track");
            oct_[t]->setFocusPolicy(Qt::NoFocus);
            mute_[t] = new QCheckBox("mute");
            state_[t] = new QLabel;
            row->addWidget(chan_[t]);
            row->addWidget(oct_[t]);
            row->addWidget(mute_[t]);
            row->addWidget(state_[t]);
            row->addStretch(1);
            bl->addWidget(name_[t]);
            bl->addWidget(dest_[t]);
            bl->addWidget(sample_[t]);
            bl->addLayout(row);
            hl->addWidget(box);
        }
        hl->addStretch(1);

        headScroll_ = new QScrollArea;
        headScroll_->setWidget(head);
        headScroll_->setWidgetResizable(false);
        headScroll_->setFrameShape(QFrame::NoFrame);
        headScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        headScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        headScroll_->setFixedHeight(head->sizeHint().height());
        head->setFixedWidth(view_->width());
        gridCol->addWidget(headScroll_);

        scroll_ = new QScrollArea;
        scroll_->setWidget(view_);
        scroll_->setWidgetResizable(false);
        scroll_->setFocusProxy(view_);
        // Frameless, like the header strip: a frame here would inset the grid
        // by frameWidth() and leave the controls that far left of their
        // columns for good.
        scroll_->setFrameShape(QFrame::NoFrame);
        gridCol->addWidget(scroll_, 1);
        connect(scroll_->horizontalScrollBar(), &QScrollBar::valueChanged,
                headScroll_->horizontalScrollBar(), &QScrollBar::setValue);
        // Copying the value is not the whole of keeping the two together. The
        // header has no scrollbars of its own while the grid has a vertical
        // one, so the grid's viewport is narrower and its horizontal maximum
        // larger: near the right edge the header's bar clamps first and the
        // controls drift left of their columns. Following the grid bar's
        // range means the header can always go exactly as far as the grid,
        // and the reverse value connection keeps the pair in step whichever
        // side a resize moves.
        connect(scroll_->horizontalScrollBar(), &QScrollBar::rangeChanged,
                headScroll_->horizontalScrollBar(), &QScrollBar::setRange);
        connect(headScroll_->horizontalScrollBar(), &QScrollBar::valueChanged,
                scroll_->horizontalScrollBar(), &QScrollBar::setValue);
        headScroll_->horizontalScrollBar()->setRange(
            scroll_->horizontalScrollBar()->minimum(),
            scroll_->horizontalScrollBar()->maximum());

        // Menus, on the host's menu bar.
        QMenu *file = host_->addMenu("&File");
        file->addAction("&New", QKeySequence::New, this, &TrackerWidget::newSong);
        file->addAction("&Open…", QKeySequence::Open, this, &TrackerWidget::openSong);
        file->addAction("&Save", QKeySequence::Save, this, &TrackerWidget::save);
        file->addAction("Save &As…", QKeySequence::SaveAs, this, &TrackerWidget::saveAs);
        file->addAction("&Export MIDI…", this, &TrackerWidget::exportMidi);
        file->addAction("Export recorded &take as MIDI…", this, &TrackerWidget::exportTake);
        file->addSeparator();
        file->addAction("&Quit", QKeySequence::Quit, this, [this] { host_->requestQuit(); });
        // Samples: the folder of WAVs the tracks' samples come from. Built
        // once, here: the two commands are static, and a shell that merges
        // menus attributes to this tab what exists when construction ends.
        samplesMenu_ = host_->addMenu("&Samples");
        rebuildSamplesMenu();
        QMenu *help = host_->addMenu("&Help");
        help->addAction("&Keys", this, [this] { showText("tracker keys", QString::fromUtf8(kKeysHelp)); });
        help->addAction("&Columns", this, [this] { showText("tracker columns", QString::fromUtf8(trk_columns_help())); });
        help->addAction("&Cheat Sheet", QKeySequence(Qt::Key_F1), this, &TrackerWidget::showCheat);

        // Wiring.
        connect(playSong, &QPushButton::clicked, this, [this] { key(TRK_K_PLAY_SONG); });
        connect(playPat, &QPushButton::clicked, this, [this] { key(TRK_K_PLAY_PATTERN); });
        connect(stop, &QPushButton::clicked, this, [this] { key(TRK_K_STOP); });
        connect(panic, &QPushButton::clicked, this, [this] { trk_panic(e_); view_->setFocus(); });
        connect(recBtn_, &QPushButton::clicked, this, [this] { key(TRK_K_RECORD); showRec(); });
        connect(recOpt, &QPushButton::clicked, this, [this] { showRecOptions(); });
        connect(bpm_, &QDoubleSpinBox::valueChanged, this, [this](double b) {
            if (loading_) return;
            trk_set_bpm(e_, b);
        });
        connect(lpb_, &QComboBox::currentIndexChanged, this, [this](int) {
            if (loading_) return;
            trk_lock(e_);
            trk_song_of(e_)->lpb = lpb_->currentData().toInt();
            trk_unlock(e_);
            view_->update();
        });
        connect(pattern_, &QSpinBox::valueChanged, this, [this](int p) {
            if (loading_) return;
            ed_.pattern = p;
            syncFromSong();
            view_->updateSize();
            view_->update();
        });
        connect(rows_, &QSpinBox::valueChanged, this, [this](int r) {
            if (loading_) return;
            trk_lock(e_);
            trk_song_of(e_)->pattern[ed_.pattern].rows = r;
            if (ed_.row >= r) ed_.row = r - 1;
            trk_unlock(e_);
            view_->updateSize();
            view_->update();
            refreshParts();
        });
        connect(step_, &QSpinBox::valueChanged, this, [this](int s) { ed_.step = s; });
        connect(follow_, &QCheckBox::toggled, this, [this](bool on) { ed_.follow = on; });
        connect(volume_, &QSlider::valueChanged, this, [this](int v) {
            volLabel_->setText(QString("vol %1%").arg(v));
            if (loading_) return;
            trk_lock(e_);
            trk_song_of(e_)->volume = v;
            trk_unlock(e_);
        });
        connect(edit_, &QCheckBox::toggled, this, [this](bool on) {
            ed_.edit = on;
            editShown();
            view_->setFocus();
        });
        for (int t = 0; t < TRK_TRACKS; t++) {
            connect(name_[t], &QLineEdit::editingFinished, this, [this, t] {
                trk_lock(e_);
                std::snprintf(trk_song_of(e_)->track[t].name, TRK_NAME_LEN, "%s",
                              name_[t]->text().toUtf8().constData());
                trk_unlock(e_);
            });
            connect(dest_[t], &QComboBox::activated, this, [this, t](int) {
                const QString data = dest_[t]->currentData().toString();
                if (data.startsWith("sink\t")) {
                    // An in-process tab: the shell owns the routing change.
                    host_->midiSinkPicked(t, data.mid(5));
                } else {
                    host_->midiSinkPicked(t, QString());   // back to a window, if it was a sink
                    const QStringList d = data.split('\t');
                    trk_lock(e_);
                    trk_track *k = &trk_song_of(e_)->track[t];
                    std::snprintf(k->client, TRK_DEST_LEN, "%s", d.value(0).toUtf8().constData());
                    std::snprintf(k->port, TRK_DEST_LEN, "%s", d.value(1).toUtf8().constData());
                    trk_unlock(e_);
                    trk_route(e_);
                }
                refreshDests(true);
                view_->setFocus();
            });
            connect(sample_[t], &QComboBox::activated, this, [this, t](int) {
                trk_lock(e_);
                std::snprintf(trk_song_of(e_)->track[t].samples, TRK_PATH_LEN, "%s",
                              sample_[t]->currentData().toString().toUtf8().constData());
                trk_unlock(e_);
                trk_route(e_);
                refreshDests(true);
                view_->setFocus();
            });
            // A track's octave, from its own box. The call locks itself and
            // moves the track's notes with the change, so the grid redraws.
            connect(oct_[t], &QComboBox::activated, this, [this, t](int o) {
                trk_track_set_octave(e_, t, o);
                if (t == ed_.track) ed_.octave = o;
                QTimer::singleShot(0, this, &TrackerWidget::refreshCheat);
                view_->update();
                view_->setFocus();
            });
            connect(chan_[t], &QSpinBox::valueChanged, this, [this, t](int c) {
                if (loading_) return;
                trk_lock(e_);
                trk_song_of(e_)->track[t].channel = c - 1;
                trk_unlock(e_);
            });
            connect(mute_[t], &QCheckBox::toggled, this, [this, t](bool on) {
                if (loading_) return;
                trk_lock(e_);
                trk_song_of(e_)->track[t].mute = on;
                trk_unlock(e_);
                view_->update();
            });
        }
        connect(view_, &PatternView::cursorMoved, this, &TrackerWidget::cursorMoved);
        connect(view_, &PatternView::clipped, this, [this](int k) {
            int rows = 0, tracks = 0;
            trk_clipboard(&rows, &tracks);
            const QString size = QString("%1 row%2 x %3 track%4").arg(rows).arg(rows > 1 ? "s" : "")
                                     .arg(tracks).arg(tracks > 1 ? "s" : "");
            if (!ed_.edit && k != TRK_K_COPY)
                host_->showStatus("edit is off -- Space to edit, then cut or paste", 4000);
            else
                host_->showStatus((k == TRK_K_COPY ? "copied " : k == TRK_K_CUT ? "cut " : "pasted ")
                                         + size, 3000);
        });
        // A key muted or soloed tracks: the header boxes show the song's state.
        connect(view_, &PatternView::mutesChanged, this, [this] {
            trk_lock(e_);
            for (int t = 0; t < TRK_TRACKS; t++) {
                mute_[t]->blockSignals(true);
                mute_[t]->setChecked(trk_song_of(e_)->track[t].mute);
                mute_[t]->blockSignals(false);
            }
            trk_unlock(e_);
            view_->update();
        });
        connect(view_, &PatternView::trackAddRequested, this, [this] { addTrack(); });
        connect(view_, &PatternView::trackRemoveRequested, this, [this] { removeTrack(); });
        connect(view_, &PatternView::stepChanged, this, [this] {
            step_->blockSignals(true);
            step_->setValue(ed_.step);
            step_->blockSignals(false);
        });
        connect(view_, &PatternView::editToggled, this, [this] {
            edit_->blockSignals(true);
            edit_->setChecked(ed_.edit);
            edit_->blockSignals(false);
            editShown();
        });
        // On a sample-set track, what the note plays -- or that it plays
        // nothing, and where the set's samples are.
        connect(view_, &PatternView::noteTyped, this, [this](int t, int note) {
            char what[TRK_PATH_LEN + 64], nn[5];
            const int r = trk_sample_at(e_, t, note, what, sizeof what);
            if (r < 0) return;
            drumkit_note_name(note, nn);
            host_->showStatus(r ? QString("%1  %2").arg(nn, QString::fromUtf8(what))
                                       : QString("no sample on %1: %2").arg(nn, QString::fromUtf8(what)),
                                     r ? 3000 : 6000);
        });

        auto *esc = new QAction(this);
        esc->setShortcut(Qt::Key_Escape);
        esc->setShortcutContext(Qt::WindowShortcut);
        connect(esc, &QAction::triggered, this, [this] { trk_panic(e_); });
        addAction(esc);
        auto *f12 = new QAction(this);                  // Furnace's panic key
        f12->setShortcut(Qt::Key_F12);
        f12->setShortcutContext(Qt::WindowShortcut);
        connect(f12, &QAction::triggered, this, [this] { trk_panic(e_); });
        addAction(f12);

        // Playback position, thirty times a second; routing every two, so a
        // window opened after the song was loaded is found and connected.
        auto *tick = new QTimer(this);
        connect(tick, &QTimer::timeout, this, &TrackerWidget::followPlayback);
        tick->start(33);
        auto *route = new QTimer(this);
        connect(route, &QTimer::timeout, this, [this] { trk_route(e_); refreshDests(false); });
        route->start(2000);

        syncFromSong();
        refreshDests(true);
        updateTitle();
        view_->setFocus();
    }

    bool openPath(const QString &path)
    {
        char err[512];
        auto tmp = std::make_unique<trk_song>();
        if (trk_song_load(tmp.get(), path.toLocal8Bit().constData(), err, sizeof err)) {
            QMessageBox::warning(this, "tracker", QString::fromLocal8Bit(err));
            return false;
        }
        trk_stop(e_);
        trk_undo_clear(e_);          /* a loaded song starts with no history */
        trk_unroute_sinks(e_);       /* the engine's routing and the new song's sink names agree */
        trk_lock(e_);
        std::memcpy(trk_song_of(e_), tmp.get(), sizeof *tmp);
        trk_unlock(e_);
        std::memcpy(saved_.get(), tmp.get(), sizeof *tmp);
        path_ = path;
        trk_editor_init(&ed_);
        trk_set_bpm(e_, tmp->bpm);
        trk_route(e_);
        syncFromSong();
        refreshDests(true);
        view_->updateSize();
        view_->update();
        updateTitle();
        host_->songOpened();   // a shell reopens the synths the song names
        return true;
    }

    // For the scripted test in uitest() below.
    PatternView *view() const { return view_; }
    trk_editor *editor() { return &ed_; }
    QComboBox *dest(int t) const { return dest_[t]; }
    bool writeSong(const QString &p) { return writeTo(p); }
    // A shell saving a session asks these: where the song lives ("" when it
    // was never saved), whether it has unsaved changes, and a way to put that
    // right through the ordinary save flow before the session file is written.
    QString songPath() const { return path_; }
    bool isDirty() { return dirty(); }
    bool saveSong() { return save(); }
    // A scripted drive that changed the song for its own purposes: closing
    // should not ask about those changes.
    void markClean()
    {
        trk_lock(e_);
        std::memcpy(saved_.get(), trk_song_of(e_), sizeof(trk_song));
        trk_unlock(e_);
    }

    // For the window test: whether a track has its header showing.
    bool headerVisible(int t) const { return t >= 0 && t < TRK_TRACKS && headBox_[t] && headBox_[t]->isVisibleTo(this); }

    // How wide the standalone window opens; a shell sizes the widget itself.
    int preferredWidth() const
    {
        return std::min(1400, view_->gutter() + view_->nt() * view_->colWidth() + 40);
    }

    // Ending the song session, as closing the standalone window does: asks
    // about unsaved changes first, then stops playback. True means go ahead.
    bool confirmClose()
    {
#ifdef TRACKER_UITEST
        trk_stop(e_);
        return true;
#else
        if (!confirmDiscard()) return false;
        trk_stop(e_);
        return true;
#endif
    }

private:
    void key(int k)
    {
        trk_key(e_, &ed_, k);
        view_->setFocus();
    }

    // Song -> widgets, without the widgets writing back while it happens.
    void syncFromSong()
    {
        loading_ = true;
        trk_lock(e_);
        const trk_song *s = trk_song_of(e_);
        bpm_->setValue(s->bpm);
        lpb_->setCurrentIndex(std::max(0, lpb_->findData(s->lpb)));
        volume_->setValue(s->volume);
        pattern_->setValue(ed_.pattern);
        rows_->setValue(s->pattern[ed_.pattern].rows);
        ed_.octave = s->track[ed_.track].octave;
        step_->setValue(ed_.step);
        partName_->setText(QString::fromUtf8(s->pattern[ed_.pattern].name));
        for (int t = 0; t < TRK_TRACKS; t++) {
            name_[t]->setText(QString::fromUtf8(s->track[t].name));
            chan_[t]->setValue(s->track[t].channel + 1);
            oct_[t]->setCurrentIndex(s->track[t].octave);
            mute_[t]->setChecked(s->track[t].mute);
        }
        trk_unlock(e_);
        loading_ = false;
        showHeaders();
        refreshParts();
    }

    // A header over every track the song has, and none over the room left.
    void showHeaders()
    {
        const int n = view_->nt();
        for (int t = 0; t < TRK_TRACKS; t++)
            if (headBox_[t]) headBox_[t]->setVisible(t < n);
        if (headStrip_) headStrip_->setFixedWidth(view_->width());
    }

    // Everything that shows a track, again, after the tracks have moved.
    void tracksChanged(const QString &msg)
    {
        if (ed_.track >= view_->nt()) ed_.track = view_->nt() - 1;
        trk_select_none(&ed_);
        view_->updateSize();
        syncFromSong();
        refreshDests(true);
        refreshSamples();
        view_->update();
        cursorMoved();
        host_->showStatus(msg, 5000);
    }

    // + Track: an empty one after the cursor's.
    void addTrack()
    {
        if (trk_track_insert(e_, ed_.track + 1) == 0) {
            ed_.track++;
            tracksChanged(QString("track %1 added -- Ctrl+Z takes it back").arg(ed_.track + 1));
        } else {
            host_->showStatus(QString("no room: a song has at most %1 tracks").arg(TRK_TRACKS), 5000);
        }
        view_->setFocus();
    }

    // - Track: the cursor's, notes and all -- asked about first when it holds any.
    void removeTrack()
    {
        const int t = ed_.track;
        if (trk_track_used(e_, t) &&
            QMessageBox::question(this, "Remove track",
                                  QString("Remove track %1? Its notes go with it. Undo (Ctrl+Z) brings the track back.")
                                      .arg(t + 1),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
            view_->setFocus();
            return;
        }
        if (trk_track_remove(e_, t) == 0)
            tracksChanged(QString("track %1 removed -- Ctrl+Z brings it back").arg(t + 1));
        else
            host_->showStatus("a song keeps at least one track", 5000);
        view_->setFocus();
    }

    // ---------------------------------------------------------------- parts
    //
    // The song is its parts in order -- a part is a pattern, and plays as
    // often as it is in the list. Picking one edits it; the buttons and
    // dragging change the order through trk_order_*, which the GTK window
    // uses too.

    QWidget *buildParts()
    {
        auto *panel = new QWidget;
        panel->setFixedWidth(200);
        auto *l = new QVBoxLayout(panel);
        l->setContentsMargins(0, 0, 6, 0);
        l->setSpacing(4);
        l->addWidget(new QLabel("Parts"));
        partName_ = new QLineEdit;
        partName_->setPlaceholderText("name this part");
        partName_->setToolTip("The name of the part being edited -- Intro, Verse, Chorus");
        // Its length beside its name: how many rows this part has.
        auto *nameRow = new QHBoxLayout;
        nameRow->setSpacing(4);
        nameRow->addWidget(partName_, 1);
        nameRow->addWidget(rows_);
        l->addLayout(nameRow);
        parts_ = new QListWidget;
        parts_->setDragDropMode(QAbstractItemView::InternalMove);
        parts_->setDefaultDropAction(Qt::MoveAction);
        parts_->setToolTip("The song, in order. Click a part to edit it; drag to move it");
        l->addWidget(parts_, 1);

        struct B { const char *label, *tip; int what; };
        static const B bs[] = {
            { "New",    "Add a new, empty part after this one", 0 },
            { "Copy",   "Add a copy of this part after it, to change", 1 },
            { "Again",  "Play this same part again after it", 2 },
            { "Remove", "Take this part out of the song (it is kept, and comes back with Again)", 3 },
            { "▲",      "Move this part earlier", 4 },
            { "▼",      "Move this part later", 5 },
        };
        auto *grid = new QGridLayout;
        grid->setSpacing(2);
        for (int i = 0; i < 6; i++) {
            auto *b = new QPushButton(QString::fromUtf8(bs[i].label));
            b->setToolTip(bs[i].tip);
            b->setFocusPolicy(Qt::NoFocus);
            const int what = bs[i].what;
            connect(b, &QPushButton::clicked, this, [this, what] { partAction(what); });
            grid->addWidget(b, i / 3, i % 3);
        }
        l->addLayout(grid);

        connect(parts_, &QListWidget::currentRowChanged, this, [this](int r) {
            if (fillingParts_ || r < 0) return;
            editPart(r);
        });
        // A drag finished: the order is whatever the list now says.
        connect(parts_->model(), &QAbstractItemModel::rowsMoved, this, [this] {
            if (fillingParts_) return;
            trk_lock(e_);
            trk_song *s = trk_song_of(e_);
            for (int i = 0; i < parts_->count() && i < TRK_ORDER_MAX; i++)
                s->order[i] = parts_->item(i)->data(Qt::UserRole).toInt();
            trk_unlock(e_);
            partAt_ = parts_->currentRow();
            refreshParts();
        });
        connect(partName_, &QLineEdit::editingFinished, this, [this] {
            trk_lock(e_);
            std::snprintf(trk_song_of(e_)->pattern[ed_.pattern].name, TRK_NAME_LEN, "%s",
                          partName_->text().trimmed().toUtf8().constData());
            trk_unlock(e_);
            refreshParts();
            view_->setFocus();
        });
        return panel;
    }

    void refreshParts()
    {
        if (!parts_) return;
        fillingParts_ = true;
        trk_lock(e_);
        const trk_song *s = trk_song_of(e_);
        const int n = s->norder;
        if (partAt_ >= n) partAt_ = n - 1;
        if (partAt_ < 0) partAt_ = 0;
        parts_->clear();
        for (int i = 0; i < n; i++) {
            char lbl[TRK_NAME_LEN + 8];
            trk_part_label(s, s->order[i], lbl);
            auto *it = new QListWidgetItem(QString("%1  %2  (%3)").arg(i + 1, 2)
                                               .arg(QString::fromUtf8(lbl)).arg(s->pattern[s->order[i]].rows));
            it->setData(Qt::UserRole, s->order[i]);
            it->setToolTip(QString("pattern %1, %2 rows").arg(s->order[i]).arg(s->pattern[s->order[i]].rows));
            if (i == partPlaying_) {
                QFont f = it->font();
                f.setBold(true);
                it->setFont(f);
                it->setText(it->text() + "   ▶");
            }
            parts_->addItem(it);
        }
        trk_unlock(e_);
        parts_->setCurrentRow(partAt_);
        fillingParts_ = false;
    }

    // Order entry r into the grid: its pattern is what the keys now edit.
    void editPart(int r)
    {
        trk_lock(e_);
        const trk_song *s = trk_song_of(e_);
        const int p = r >= 0 && r < s->norder ? s->order[r] : -1;
        trk_unlock(e_);
        if (p < 0) return;
        partAt_ = r;
        ed_.part = r;
        ed_.pattern = p;
        syncFromSong();
        view_->updateSize();
        view_->update();
        view_->setFocus();
    }

    void partAction(int what)
    {
        int at = partAt_, r = -1;
        trk_lock(e_);
        const int cur = trk_song_of(e_)->order[at];
        trk_unlock(e_);
        switch (what) {
        case 0: case 1: {
            const int p = trk_pattern_new(e_, what == 1 ? cur : -1);
            if (p < 0) { host_->showStatus("every pattern is in use", 4000); return; }
            r = trk_order_insert(e_, at, p);
            break;
        }
        case 2: r = trk_order_insert(e_, at, cur); break;
        case 3:
            r = trk_order_remove(e_, at);
            if (r < 0) host_->showStatus("a song keeps at least one part", 3000);
            break;
        case 4: r = trk_order_move(e_, at, -1); break;
        case 5: r = trk_order_move(e_, at, +1); break;
        }
        if (r >= 0) partAt_ = r;
        refreshParts();
        editPart(partAt_);
    }

    // The destination lists: every window that can be played, plus whatever
    // a track names that is not open right now -- kept, and marked, so
    // loading a song before its windows does not forget where tracks go.
    // After the windows, the shell's in-process destinations (studio's synth
    // tabs); a track routed to one shows it, and its window names wait in the
    // song for the track to be routed back.
    void refreshDests(bool force)
    {
        static char buf[16384];
        trk_list_dests(e_, buf, sizeof buf);
        const QStringList sinks = host_->midiSinks();
        // The sink each track plays, so a routing changed from elsewhere --
        // a routed tab closing -- rebuilds the lists on the next pass.
        QStringList routed;
        for (int t = 0; t < TRK_TRACKS; t++) {
            char nm[TRK_DEST_LEN] = "";
            const int id = trk_sink_of(e_, t);
            if (id >= 0) trk_sink_name(e_, id, nm, sizeof nm);
            routed << QString::fromUtf8(nm);
        }
        const QString list = QString::fromUtf8(buf) + '|' + sinks.join('\n') + '|' + routed.join('\n');
        if (!force && list == lastDests_) { updateStates(); return; }
        lastDests_ = list;
        const QStringList lines = QString::fromUtf8(buf).split('\n', Qt::SkipEmptyParts);
        for (int t = 0; t < TRK_TRACKS; t++) {
            QComboBox *c = dest_[t];
            if (c->view()->isVisible()) continue;          // not under the user's pointer
            trk_lock(e_);
            const QString want = QString::fromUtf8(trk_song_of(e_)->track[t].client) + '\t' +
                                 QString::fromUtf8(trk_song_of(e_)->track[t].port);
            trk_unlock(e_);
            const QString sinkName = routed[t];
            c->blockSignals(true);
            c->clear();
            c->addItem("(nowhere)", QString("\t"));
            int sel = 0;
            if (sinkName.isEmpty()) {
                for (const QString &l : lines) {
                    const QStringList d = l.split('\t');
                    c->addItem(d.value(0) + ": " + d.value(1), l);
                    if (l == want) sel = c->count() - 1;
                }
                if (!sel && want != "\t") {
                    const QStringList d = want.split('\t');
                    c->addItem(d.value(0) + ": " + d.value(1) + " (not open)", want);
                    sel = c->count() - 1;
                }
            }
            for (const QString &s : sinks) {
                c->addItem("this window: " + s, QString("sink\t") + s);
                if (!sinkName.isEmpty() && s == sinkName) sel = c->count() - 1;
            }
            if (!sinkName.isEmpty() && !sel) {
                c->addItem("this window: " + sinkName + " (closed)", QString("sink\t") + sinkName);
                sel = c->count() - 1;
            }
            c->setCurrentIndex(sel);
            c->setToolTip(c->currentText());
            c->blockSignals(false);
        }
        refreshSamples();
        updateStates();
    }

    // Each track's sample-set box: every set there is, and whatever a track
    // names that is not there -- kept, and marked, as a window that is not
    // open is. A track with a set plays no window, so its window box is
    // greyed out.
    void refreshSamples()
    {
        static char buf[32768];
        trk_list_sample_sets(e_, buf, sizeof buf);
        const QStringList lines = QString::fromUtf8(buf).split('\n', Qt::SkipEmptyParts);
        for (int t = 0; t < TRK_TRACKS; t++) {
            QComboBox *c = sample_[t];
            if (c->view()->isVisible()) continue;
            trk_lock(e_);
            const QString want = QString::fromUtf8(trk_song_of(e_)->track[t].samples);
            trk_unlock(e_);
            c->blockSignals(true);
            c->clear();
            c->addItem("(no samples)", QString());
            int sel = 0;
            for (const QString &l : lines) {
                const QString name = l.section('\t', 0, 0), path = l.section('\t', 1);
                c->addItem(name, name);
                c->setItemData(c->count() - 1, path, Qt::ToolTipRole);
                if (!want.isEmpty() && (want == name || want == path)) sel = c->count() - 1;
            }
            if (!sel && !want.isEmpty()) {
                c->addItem(want + " (not found)", want);
                sel = c->count() - 1;
            }
            c->setCurrentIndex(sel);
            c->setToolTip(sel ? c->currentText()
                              : "A sample set for this track to play instead of a window: "
                                "the note picks the sample");
            c->blockSignals(false);
            dest_[t]->setEnabled(want.isEmpty());
        }
    }

    void rebuildSamplesMenu()
    {
        samplesMenu_->clear();
        samplesMenu_->addAction("&Load Sample Set…", this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(this, "Load a sample set "
                                                                  "(a folder of WAVs)");
            if (dir.isEmpty()) return;
            trk_add_sample_set(e_, dir.toUtf8().constData());
            refreshDests(true);
            host_->showStatus("loaded " + dir + " -- pick it in a track's sample-set box",
                                     6000);
        });
        // The set the cursor's track plays, to begin with; any other from
        // the editor's own list.
        samplesMenu_->addAction("&Edit Sample Set…", this, [this] {
            trk_lock(e_);
            const QString cur = QString::fromUtf8(trk_song_of(e_)->track[ed_.track].samples);
            trk_unlock(e_);
            SetEditor dlg(e_, cur, this);
            connect(&dlg, &SetEditor::saved, this, [this] { refreshDests(true); });
            dlg.exec();
            view_->setFocus();
        });
        samplesMenu_->addSeparator();
        samplesMenu_->addAction("&Audio Output…", this, [this] { showAudioOutput(); });
    }

    // Samples > Audio Output: where the tracker's own audio (sample-set
    // tracks) goes -- the system default, PipeWire, JACK, PulseAudio or a
    // sound card -- switched live and remembered. Choosing a row applies it.
    void showAudioOutput()
    {
        QDialog d(this);
        d.setWindowTitle("Audio output");
        auto *v = new QVBoxLayout(&d);
        v->addWidget(new QLabel("Where sample-set tracks play. Click one to switch to it; the choice is kept."));
        auto *list = new QListWidget;
        auto *note = new QLabel;
        note->setWordWrap(true);
        static char names[24][TRK_DEST_LEN], labels[24][96];
        const int n = trk_audio_devices(names, labels, 24);
        auto cur = [this] { return QString::fromUtf8(trk_audio_device(e_)); };
        auto fill = [&] {
            list->blockSignals(true);
            list->clear();
            const QString now = cur().isEmpty() ? QString("default") : cur();
            for (int i = 0; i < n; i++) {
                auto *it = new QListWidgetItem(QString::fromUtf8(labels[i]) +
                                               (now == names[i] ? "   ● in use" : ""));
                it->setData(Qt::UserRole, QString::fromUtf8(names[i]));
                list->addItem(it);
                if (now == names[i]) list->setCurrentItem(it);
            }
            list->blockSignals(false);
        };
        fill();
        connect(list, &QListWidget::itemClicked, &d, [&](QListWidgetItem *it) {
            const QByteArray name = it->data(Qt::UserRole).toString().toUtf8();
            const bool def = name == "default";
            const int r = trk_audio_set_device(e_, def ? "" : name.constData());
            note->setText(QString::fromUtf8(trk_audio_status(e_)).isEmpty()
                              ? (r ? "that device would not open" : "switched")
                              : QString::fromUtf8(trk_audio_status(e_)));
            fill();
            host_->showStatus(note->text(), 5000);
        });
        v->addWidget(list, 1);
        v->addWidget(note);
        auto *info = new QLabel("Synth tabs in the studio play through PipeWire, which also serves JACK programs "
                                "(pipewire-jack) and ALSA programs (pipewire-alsa). This list is for the tracker's own "
                                "sample playback.");
        info->setWordWrap(true);
        info->setEnabled(false);
        v->addWidget(info);
        auto *close = new QPushButton("Close");
        connect(close, &QPushButton::clicked, &d, &QDialog::accept);
        v->addWidget(close, 0, Qt::AlignRight);
        d.resize(520, 420);
        d.exec();
        view_->setFocus();
    }

    // Edit mode, said where it cannot be missed: off, the grid is not
    // being written to, however much the keys are played.
    void editShown()
    {
        view_->setEditing(ed_.edit);
        host_->showStatus(ed_.edit ? "edit on: keys write into the pattern"
                                          : "edit off: note keys only play -- ` to edit again",
                                 ed_.edit ? 2500 : 0);
    }

    // Help > Cheat Sheet: for each track playing a sample set, every sample
    // with its note and the key that types it at the octave set now; then
    // the note keys, for the tracks that play windows. Kept up to date while
    // it is open.
    void showCheat()
    {
        if (!cheatDlg_) {
            cheatDlg_ = new QDialog(this);
            cheatDlg_->setWindowTitle("Cheat Sheet");
            cheatDlg_->resize(520, 640);
            auto *l = new QVBoxLayout(cheatDlg_);
            cheatText_ = new QPlainTextEdit;
            cheatText_->setReadOnly(true);
            cheatText_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            cheatText_->setLineWrapMode(QPlainTextEdit::NoWrap);
            l->addWidget(cheatText_);
        }
        cheatDlg_->show();                 // first: refreshCheat skips a hidden one
        refreshCheat();
        cheatDlg_->raise();
        cheatDlg_->activateWindow();
    }

    void refreshCheat()
    {
        static char buf[16384];
        if (!cheatDlg_ || !cheatDlg_->isVisible()) return;
        QString text, names[TRK_TRACKS], sets[TRK_TRACKS];
        int window = -1;
        trk_lock(e_);
        for (int t = 0; t < TRK_TRACKS; t++) {
            names[t] = QString::fromUtf8(trk_song_of(e_)->track[t].name);
            sets[t] = QString::fromUtf8(trk_song_of(e_)->track[t].samples);
        }
        trk_unlock(e_);
        // Each set once, under every track that plays it.
        for (int t = 0; t < TRK_TRACKS; t++) {
            if (sets[t].isEmpty()) { if (window < 0) window = t; continue; }
            bool seen = false;
            for (int u = 0; u < t; u++) seen |= sets[u] == sets[t];
            if (seen) continue;
            QStringList who;
            for (int u = t; u < TRK_TRACKS; u++)
                if (sets[u] == sets[t]) who << QString("%1 %2").arg(u + 1).arg(names[u]);
            trk_cheat_sheet(e_, t, -1, 200, buf, sizeof buf);
            text += QString("%1  --  track%2 %3\n").arg(sets[t], who.size() > 1 ? "s" : "",
                                                      who.join(", "));
            for (const QString &line : QString::fromUtf8(buf).split('\n', Qt::SkipEmptyParts))
                text += "  " + line + "\n";
            text += "\n";
        }
        if (window >= 0) {
            trk_cheat_sheet(e_, window, -1, 200, buf, sizeof buf);
            text += QString("Tracks that play a window (track %1's octave)\n").arg(window + 1);
            for (const QString &line : QString::fromUtf8(buf).split('\n', Qt::SkipEmptyParts))
                text += "  " + line + "\n";
        }
        text += "\nEach track has its own octave: [ and ] change the cursor's.\n";
        if (cheatText_->toPlainText() != text) {
            const int at = cheatText_->verticalScrollBar()->value();
            cheatText_->setPlainText(text);
            cheatText_->verticalScrollBar()->setValue(at);
        }
    }

    void updateStates()
    {
        bool kits = false;
        for (int t = 0; t < TRK_TRACKS; t++) {
            trk_lock(e_);
            const bool kit = trk_song_of(e_)->track[t].samples[0] != 0;
            const bool named = kit || trk_song_of(e_)->track[t].client[0] != 0;
            trk_unlock(e_);
            const bool ok = trk_routed(e_, t);
            kits |= kit;
            // A sample set picks its sample by the note itself, so the octave
            // -- which would only move those notes onto other samples -- is off.
            oct_[t]->setEnabled(!kit);
            oct_[t]->setToolTip(kit ? "A sample set picks its sample by the note: the octave is fixed"
                                    : "The octave the note keys play on this track");
            state_[t]->setText(!named ? "" : ok ? "●" : "○");
            state_[t]->setStyleSheet(ok ? "color: #3a3;" : "color: #c33;");
            state_[t]->setToolTip(!named ? ""
                                  : kit ? (ok ? "sample set loaded" : "no sample set by that name, or no audio output")
                                  : ok ? "connected" : "that window is not open");
        }
        refreshCheat();
        // Where the samples play, or why they cannot -- only once a track has one.
        const QString audio = QString::fromUtf8(trk_audio_status(e_));
        if (kits && !audio.isEmpty() && audio != audioShown_) {
            host_->showStatus(audio, 6000);
            audioShown_ = audio;
        }
    }

    // A help text in a window of its own: a fixed-width font and no wrapping,
    // because the tables in it are aligned with spaces and a message box would
    // wrap them out of line.
    void showText(const QString &title, const QString &text)
    {
        QDialog d(this);
        d.setWindowTitle(title);
        auto *l = new QVBoxLayout(&d);
        auto *t = new QPlainTextEdit;
        t->setReadOnly(true);
        t->setLineWrapMode(QPlainTextEdit::NoWrap);
        t->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        t->setPlainText(text);
        l->addWidget(t);
        auto *ok = new QPushButton("Close");
        connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
        l->addWidget(ok, 0, Qt::AlignRight);
        QFontMetrics fm(t->font());
        int w = 0;
        for (const QString &ln : text.split('\n')) w = std::max(w, fm.horizontalAdvance(ln));
        d.resize(std::min(w + 80, 1000), 600);
        d.exec();
    }

    // The Rec button says what the engine is doing: lit while a take runs,
    // and counting in while the click leads the take in.
    void showRec()
    {
        const int rs = trk_recording(e_);
        if (rs == recShown_) return;
        recShown_ = rs;
        recBtn_->setText(rs == 2 ? "● Count-in…" : rs ? "● Recording" : "● Rec");
        recBtn_->setStyleSheet(rs ? "QPushButton { background: #c0392b; color: white; }" : "");
        if (rs == 1) host_->showStatus("recording -- play the keys; F7 or Stop ends the take", 6000);
    }

    // Recording options: each change applies at once.
    void showRecOptions()
    {
        if (recDlg_) { recDlg_->raise(); recDlg_->activateWindow(); return; }
        auto *d = new QDialog(this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle("Recording options");
        auto *form = new QFormLayout(d);
        trk_rec_opts o;
        trk_record_get(e_, &o);
        auto *count = new QSpinBox;
        count->setRange(0, 4);
        count->setValue(o.count_in);
        auto *metro = new QCheckBox;
        metro->setChecked(o.metronome);
        auto *quant = new QComboBox;
        quant->addItems({ "Nearest row", "Row that is sounding" });
        quant->setCurrentIndex(o.quantize);
        auto *noteoff = new QCheckBox;
        noteoff->setChecked(o.note_off);
        auto *offset = new QSpinBox;
        offset->setRange(-200, 200);
        offset->setSuffix(" ms");
        offset->setValue(o.offset_ms);
        auto *input = new QComboBox;
        input->addItem("(none)");
        char names[64][TRK_DEST_LEN];
        const int n = trk_input_list(e_, names, 64);
        for (int i = 0; i < n; i++) {
            input->addItem(QString::fromUtf8(names[i]));
            if (!std::strcmp(names[i], trk_input_connected(e_))) input->setCurrentIndex(i + 1);
        }
        auto *monitor = new QCheckBox;
        monitor->setChecked(o.monitor);
        form->addRow("Count-in (bars)", count);
        form->addRow("Metronome click", metro);
        form->addRow("Notes go to", quant);
        form->addRow("Write === when a key is let go", noteoff);
        form->addRow("Keyboard timing offset", offset);
        form->addRow("MIDI input", input);
        form->addRow("Hear the MIDI input on the cursor's track", monitor);
        auto *help = new QLabel(
            "A note you play is written to the row it falls on. 'Nearest row' rounds a\n"
            "note struck a little before the next row up to it. The offset places keyboard\n"
            "notes earlier (positive) or later, to make up for a slow keyboard or screen.\n"
            "A MIDI input is timed by the sequencer, so it needs no offset.");
        help->setEnabled(false);
        form->addRow(help);
        auto apply = [this, count, metro, quant, noteoff, offset, monitor] {
            trk_rec_opts c;
            c.count_in = count->value();
            c.metronome = metro->isChecked();
            c.quantize = quant->currentIndex();
            c.note_off = noteoff->isChecked();
            c.monitor = monitor->isChecked();
            c.offset_ms = offset->value();
            trk_record_set(e_, &c);
        };
        connect(count, &QSpinBox::valueChanged, d, apply);
        connect(offset, &QSpinBox::valueChanged, d, apply);
        connect(metro, &QCheckBox::toggled, d, apply);
        connect(noteoff, &QCheckBox::toggled, d, apply);
        connect(monitor, &QCheckBox::toggled, d, apply);
        connect(quant, &QComboBox::currentIndexChanged, d, apply);
        connect(input, &QComboBox::activated, d, [this, input](int i) {
            if (i == 0) { trk_input_connect(e_, ""); host_->showStatus("MIDI input: none", 3000); return; }
            const QByteArray nm = input->itemText(i).toUtf8();
            host_->showStatus(trk_input_connect(e_, nm.constData()) == 0
                                  ? "MIDI input: " + input->itemText(i)
                                  : QString("could not connect that MIDI input"), 4000);
        });
        recDlg_ = d;
        d->show();
    }

    // File > Export recorded take: the notes as they were played, at their
    // exact times, not as they were rounded onto rows.
    void exportTake()
    {
        if (!trk_take_events(e_)) {
            host_->showStatus("nothing has been recorded yet -- F7 records a take", 4000);
            return;
        }
        QString p = QFileDialog::getSaveFileName(this, "Export recorded take", "take.mid",
                                                 "MIDI files (*.mid);;All files (*)");
        if (p.isEmpty()) return;
        if (QFileInfo(p).suffix().isEmpty()) p += ".mid";
        char err[512];
        if (trk_take_export_midi(e_, p.toLocal8Bit().constData(), err, sizeof err)) {
            QMessageBox::warning(this, "tracker", QString::fromLocal8Bit(err));
            return;
        }
        host_->showStatus("Exported the take to " + p, 3000);
    }

    void followPlayback()
    {
        int o, p, r;
        showRec();
        trk_position(e_, &o, &p, &r);
        view_->setPlayRow(p, r);
        // The part playing, marked; and, following, the one being edited.
        if (o != partPlaying_ || (o >= 0 && ed_.follow && o != partAt_)) {
            partPlaying_ = o;
            if (o >= 0 && ed_.follow) partAt_ = ed_.part = o;
            refreshParts();
        }
        if (p < 0 || !ed_.follow) return;
        if (p != ed_.pattern) {
            ed_.pattern = p;
            loading_ = true;
            pattern_->setValue(p);
            trk_lock(e_);
            rows_->setValue(trk_song_of(e_)->pattern[p].rows);
            trk_unlock(e_);
            loading_ = false;
            view_->updateSize();
        }
        if (ed_.row != r) {
            const int was = ed_.row;
            ed_.row = r;
            scrollToRow(r);
            view_->updateRow(was);
            view_->updateRow(r);
        }
    }

    void cursorMoved()
    {
        trk_record_arm(e_, ed_.track);     // the MIDI input plays and records on the cursor's track
        loading_ = true;
        pattern_->setValue(ed_.pattern);
        trk_lock(e_);
        rows_->setValue(trk_song_of(e_)->pattern[ed_.pattern].rows);
        trk_unlock(e_);
        oct_[ed_.track]->setCurrentIndex(ed_.octave);     // [ and ] change it too
        // A moment later, not here: this can run while the song is locked.
        QTimer::singleShot(0, this, &TrackerWidget::refreshCheat);
        partName_->setText(QString::fromUtf8(trk_song_of(e_)->pattern[ed_.pattern].name));
        loading_ = false;
        scroll_->ensureVisible(view_->gutter() + ed_.track * view_->colWidth(),
                               ed_.row * view_->rowHeight(), view_->colWidth() / 2,
                               view_->rowHeight() * 3);
    }

    void scrollToRow(int r)
    {
        // Centred, so what is coming is as visible as what has passed.
        QScrollBar *sb = scroll_->verticalScrollBar();
        sb->setValue(r * view_->rowHeight() - scroll_->viewport()->height() / 2);
    }

    bool dirty()
    {
        trk_lock(e_);
        const bool d = std::memcmp(trk_song_of(e_), saved_.get(), sizeof(trk_song)) != 0;
        trk_unlock(e_);
        return d;
    }

    bool confirmDiscard()
    {
        if (!dirty()) return true;
        const auto b = QMessageBox::question(this, "tracker", "Save changes to the song first?",
                                             QMessageBox::Save | QMessageBox::Discard |
                                             QMessageBox::Cancel);
        if (b == QMessageBox::Cancel) return false;
        if (b == QMessageBox::Save) return save();
        return true;
    }

    void newSong()
    {
        if (!confirmDiscard()) return;
        trk_stop(e_);
        trk_undo_clear(e_);          /* a new song starts with no history */
        trk_unroute_sinks(e_);
        trk_lock(e_);
        trk_song_init(trk_song_of(e_));
        trk_unlock(e_);
        trk_song_init(saved_.get());
        path_.clear();
        trk_editor_init(&ed_);
        trk_set_bpm(e_, 120);
        trk_route(e_);
        syncFromSong();
        refreshDests(true);
        view_->updateSize();
        view_->update();
        updateTitle();
    }

    void openSong()
    {
        if (!confirmDiscard()) return;
        const QString p = QFileDialog::getOpenFileName(this, "Open song", QString(),
                                                       "Tracker songs (*.trk);;All files (*)");
        if (!p.isEmpty()) openPath(p);
    }

    bool save()
    {
        if (path_.isEmpty()) return saveAs();
        return writeTo(path_);
    }

    bool saveAs()
    {
        QString p = QFileDialog::getSaveFileName(this, "Save song", path_.isEmpty() ? "song.trk" : path_,
                                                 "Tracker songs (*.trk);;All files (*)");
        if (p.isEmpty()) return false;
        if (QFileInfo(p).suffix().isEmpty()) p += ".trk";
        return writeTo(p);
    }

    // File > Export MIDI: the song as a standard MIDI file, one track per
    // playing track, for a DAW to import. Not a save -- the song's own file,
    // its dirty state and its title stay as they were.
    void exportMidi()
    {
        QString base = path_.isEmpty() ? QString("song") : QFileInfo(path_).completeBaseName();
        QString p = QFileDialog::getSaveFileName(this, "Export MIDI", base + ".mid",
                                                 "MIDI files (*.mid);;All files (*)");
        if (p.isEmpty()) return;
        if (QFileInfo(p).suffix().isEmpty()) p += ".mid";
        char err[512];
        // A copy under the lock, the file written from it after: the lock is
        // the scheduler's and the note path's.
        auto snap = std::make_unique<trk_song>();
        trk_lock(e_);
        std::memcpy(snap.get(), trk_song_of(e_), sizeof(trk_song));
        trk_unlock(e_);
        const int r = trk_song_export_midi(snap.get(), p.toLocal8Bit().constData(), err, sizeof err);
        if (r) {
            QMessageBox::warning(this, "tracker", QString::fromLocal8Bit(err));
            return;
        }
        host_->showStatus("Exported " + p, 3000);
    }

    bool writeTo(const QString &p)
    {
        char err[512];
        auto snap = std::make_unique<trk_song>();     // copied under the lock, written after
        trk_lock(e_);
        std::memcpy(snap.get(), trk_song_of(e_), sizeof(trk_song));
        trk_unlock(e_);
        const int r = trk_song_save(snap.get(), p.toLocal8Bit().constData(), err, sizeof err);
        if (!r) std::memcpy(saved_.get(), snap.get(), sizeof(trk_song));
        if (r) {
            QMessageBox::warning(this, "tracker", QString::fromLocal8Bit(err));
            return false;
        }
        path_ = p;
        updateTitle();
        host_->showStatus("Saved " + p, 3000);
        return true;
    }

    void updateTitle()
    {
        setWindowTitle(QString("%1 — tracker (%2)")
                           .arg(path_.isEmpty() ? QString("untitled") : QFileInfo(path_).fileName(),
                                QString::fromUtf8(trk_client_name(e_))));
    }

    trk_engine *e_;
    TrackerHost *host_;
    trk_editor ed_;
    std::unique_ptr<trk_song> saved_;
    QString path_, lastDests_;
    bool loading_ = false;

    PatternView *view_;
    QScrollArea *scroll_, *headScroll_;
    QDialog *cheatDlg_ = nullptr;          // Help > Cheat Sheet, once opened
    QPlainTextEdit *cheatText_ = nullptr;
    QDoubleSpinBox *bpm_;
    QComboBox *lpb_;
    QSpinBox *pattern_, *rows_, *step_;
    QString audioShown_;                  // the kits' output, as last reported
    QCheckBox *follow_, *edit_;
    QSlider *volume_;
    QLabel *volLabel_;
    QLineEdit *partName_ = nullptr;
    QListWidget *parts_ = nullptr;
    int partAt_ = 0;                      // the order entry being edited
    int partPlaying_ = -1;                // the one playing, as last shown
    bool fillingParts_ = false;
    QWidget   *headStrip_ = nullptr;          // the header row, scrolled with the grid
    QWidget   *headBox_[TRK_TRACKS] = {};      // a track's header: shown while the song has the track
    QLineEdit *name_[TRK_TRACKS];
    QComboBox *dest_[TRK_TRACKS];
    QComboBox *sample_[TRK_TRACKS];
    QMenu     *samplesMenu_ = nullptr;
    QPushButton *recBtn_ = nullptr;
    int        recShown_ = 0;
    QPointer<QDialog> recDlg_;
    QSpinBox *chan_[TRK_TRACKS];
    QComboBox *oct_[TRK_TRACKS];
    QCheckBox *mute_[TRK_TRACKS];
    QLabel *state_[TRK_TRACKS];
};

#endif /* TRACKER_QT_TRACKERWIDGET_H */
