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
#include <QScrollBar>
#include <QSpinBox>
#include <QSlider>
#include <QDialog>
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
    "number and controller value; empty velocity uses the track's.\n";

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

    void updateSize()
    {
        trk_lock(e_);
        int rows = trk_song_of(e_)->pattern[ed_->pattern].rows;
        trk_unlock(e_);
        setFixedSize(gutter() + TRK_TRACKS * colWidth() + cw_, rows * ch_ + 2);
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

        trk_lock(e_);
        const trk_song *s = trk_song_of(e_);
        const trk_pattern *pt = &s->pattern[ed_->pattern];
        const int lpb = s->lpb > 0 ? s->lpb : 4;
        int mutes = 0;
        for (int t = 0; t < TRK_TRACKS; t++) if (s->track[t].mute) mutes |= 1 << t;

        const int r0 = std::max(0, ev->rect().top() / ch_);
        const int r1 = std::min(pt->rows - 1, ev->rect().bottom() / ch_);
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

            for (int t = 0; t < TRK_TRACKS; t++) {
                const int x = gutter() + t * colWidth();
                char txt[TRK_CELL_CHARS + 1];
                trk_cell_text(&pt->cell[r][t], txt);
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
                    const int nt = pt->cell[r][t].note;
                    if (c < 3 && sampled[t] && nt <= 127 && !(masks[t][nt >> 3] & (1u << (nt & 7))))
                        col = missing;
                    if (mutes & (1 << t)) col.setAlpha(col.alpha() / 3);
                    if (cur) col = pal.color(QPalette::HighlightedText);
                    p.setPen(col);
                    p.drawText(x + c * cw_, y + asc_, QString(QChar(txt[c])));
                }
            }
        }
        trk_unlock(e_);

        p.setPen(faint);
        for (int t = 0; t <= TRK_TRACKS; t++) {
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
                           k == TRK_K_CUT || k == TRK_K_PASTE))
                emit edited();
            if (k == TRK_K_COPY || k == TRK_K_CUT || k == TRK_K_PASTE) emit clipped(k);
            if (k == TRK_K_EDIT) emit editToggled();
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
        *t = std::min(x / colWidth(), TRK_TRACKS - 1);
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
            trk_select(ed_, dragR_, 0, r, TRK_TRACKS - 1);
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
            trk_select(ed_, dragR_, 0, r, TRK_TRACKS - 1);
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
        if (t < 0 && !trk_selected(ed_, r, 0)) { trk_select(ed_, r, 0, r, TRK_TRACKS - 1); ed_->track = 0; }
        update();
        emit cursorMoved();

        int crows = 0, ctracks = 0;
        trk_clipboard(&crows, &ctracks);
        QMenu m(this);
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
        if (got == copy)       key(TRK_K_COPY);
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
        // Shift with the arrows selects; Ctrl with C, X, V and A is the clipboard.
        if (ev->modifiers() & Qt::ShiftModifier) {
            switch (ev->key()) {
            case Qt::Key_Up:    return TRK_K_SEL_UP;
            case Qt::Key_Down:  return TRK_K_SEL_DOWN;
            case Qt::Key_Left:  return TRK_K_SEL_LEFT;
            case Qt::Key_Right: return TRK_K_SEL_RIGHT;
            default: break;
            }
        }
        if (ev->modifiers() & Qt::ControlModifier) {
            switch (ev->key()) {
            case Qt::Key_C: return TRK_K_COPY;
            case Qt::Key_X: return TRK_K_CUT;
            case Qt::Key_V: return TRK_K_PASTE;
            case Qt::Key_A: return TRK_K_SEL_ALL;
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
        case Qt::Key_F8:        return TRK_K_STOP;
        case Qt::Key_Space:     return TRK_K_TOGGLE;
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
        resize(760, 520);
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

        table_ = new QTableWidget(0, 5);
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
        connect(table_, &QTableWidget::itemChanged, this, [this] { if (!filling_) dirty_ = true; });

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

    void fill()
    {
        filling_ = true;
        table_->setRowCount(0);
        for (int i = 0; i < map_->n; i++)
            addRow(map_->pad[i].note, QString::fromUtf8(map_->pad[i].file),
                   map_->pad[i].gain_db, map_->pad[i].choke);
        table_->resizeColumnToContents(0);
        filling_ = false;
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
        }
    }

    void removeRows()
    {
        QList<int> rows;
        for (const QModelIndex &i : table_->selectionModel()->selectedRows()) rows << i.row();
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int r : rows) table_->removeRow(r);
        if (!rows.isEmpty()) dirty_ = true;
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
        playSong->setToolTip("Play the song from the order entry holding this pattern (F5)");
        playPat->setToolTip("Loop this pattern (F6, or Space)");
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
        for (QWidget *w : std::initializer_list<QWidget *>{ playSong, playPat, stop, panic, bpm_, lpb_,
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
        auto *hl = new QHBoxLayout(head);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(0);
        hl->addSpacing(view_->gutter());
        for (int t = 0; t < TRK_TRACKS; t++) {
            auto *box = new QWidget;
            box->setFixedWidth(view_->colWidth());
            auto *bl = new QVBoxLayout(box);
            bl->setContentsMargins(0, 0, 6, 2);
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
        gridCol->addWidget(scroll_, 1);
        connect(scroll_->horizontalScrollBar(), &QScrollBar::valueChanged,
                headScroll_->horizontalScrollBar(), &QScrollBar::setValue);

        // Menus, on the host's menu bar.
        QMenu *file = host_->addMenu("&File");
        file->addAction("&New", QKeySequence::New, this, &TrackerWidget::newSong);
        file->addAction("&Open…", QKeySequence::Open, this, &TrackerWidget::openSong);
        file->addAction("&Save", QKeySequence::Save, this, &TrackerWidget::save);
        file->addAction("Save &As…", QKeySequence::SaveAs, this, &TrackerWidget::saveAs);
        file->addSeparator();
        file->addAction("&Quit", QKeySequence::Quit, this, [this] { host_->requestQuit(); });
        // Samples: the folder of WAVs the tracks' samples come from. Rebuilt
        // each time it opens, so a folder dropped in since is there.
        samplesMenu_ = host_->addMenu("&Samples");
        connect(samplesMenu_, &QMenu::aboutToShow, this, &TrackerWidget::rebuildSamplesMenu);
        QMenu *help = host_->addMenu("&Help");
        help->addAction("&Keys", this, [this] {
            QMessageBox box(this);
            box.setWindowTitle("tracker keys");
            box.setText(QString::fromUtf8(kKeysHelp));
            QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            box.setFont(f);
            box.exec();
        });
        help->addAction("&Cheat Sheet", QKeySequence(Qt::Key_F1), this, &TrackerWidget::showCheat);

        // Wiring.
        connect(playSong, &QPushButton::clicked, this, [this] { key(TRK_K_PLAY_SONG); });
        connect(playPat, &QPushButton::clicked, this, [this] { key(TRK_K_PLAY_PATTERN); });
        connect(stop, &QPushButton::clicked, this, [this] { key(TRK_K_STOP); });
        connect(panic, &QPushButton::clicked, this, [this] { trk_panic(e_); view_->setFocus(); });
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
                const QStringList d = dest_[t]->currentData().toString().split('\t');
                trk_lock(e_);
                trk_track *k = &trk_song_of(e_)->track[t];
                std::snprintf(k->client, TRK_DEST_LEN, "%s", d.value(0).toUtf8().constData());
                std::snprintf(k->port, TRK_DEST_LEN, "%s", d.value(1).toUtf8().constData());
                trk_unlock(e_);
                trk_route(e_);
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
            // A track's octave, from its own box.
            connect(oct_[t], &QComboBox::activated, this, [this, t](int o) {
                trk_lock(e_);
                trk_song_of(e_)->track[t].octave = o;
                trk_unlock(e_);
                if (t == ed_.track) ed_.octave = o;
                QTimer::singleShot(0, this, &TrackerWidget::refreshCheat);
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
                host_->showStatus("edit is off -- ` to edit, then cut or paste", 4000);
            else
                host_->showStatus((k == TRK_K_COPY ? "copied " : k == TRK_K_CUT ? "cut " : "pasted ")
                                         + size, 3000);
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
        return true;
    }

    // For the scripted test in uitest() below.
    PatternView *view() const { return view_; }
    trk_editor *editor() { return &ed_; }
    QComboBox *dest(int t) const { return dest_[t]; }
    bool writeSong(const QString &p) { return writeTo(p); }

    // How wide the standalone window opens; a shell sizes the widget itself.
    int preferredWidth() const
    {
        return std::min(1400, view_->gutter() + TRK_TRACKS * view_->colWidth() + 40);
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
        refreshParts();
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
    void refreshDests(bool force)
    {
        static char buf[16384];
        trk_list_dests(e_, buf, sizeof buf);
        const QString list = QString::fromUtf8(buf);
        if (!force && list == lastDests_) { updateStates(); return; }
        lastDests_ = list;
        const QStringList lines = list.split('\n', Qt::SkipEmptyParts);
        for (int t = 0; t < TRK_TRACKS; t++) {
            QComboBox *c = dest_[t];
            if (c->view()->isVisible()) continue;          // not under the user's pointer
            trk_lock(e_);
            const QString want = QString::fromUtf8(trk_song_of(e_)->track[t].client) + '\t' +
                                 QString::fromUtf8(trk_song_of(e_)->track[t].port);
            trk_unlock(e_);
            c->blockSignals(true);
            c->clear();
            c->addItem("(nowhere)", QString("\t"));
            int sel = 0;
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

    void followPlayback()
    {
        int o, p, r;
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

    bool writeTo(const QString &p)
    {
        char err[512];
        trk_lock(e_);
        const int r = trk_song_save(trk_song_of(e_), p.toLocal8Bit().constData(), err, sizeof err);
        if (!r) std::memcpy(saved_.get(), trk_song_of(e_), sizeof(trk_song));
        trk_unlock(e_);
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
    QLineEdit *name_[TRK_TRACKS];
    QComboBox *dest_[TRK_TRACKS];
    QComboBox *sample_[TRK_TRACKS];
    QMenu     *samplesMenu_ = nullptr;
    QSpinBox *chan_[TRK_TRACKS];
    QComboBox *oct_[TRK_TRACKS];
    QCheckBox *mute_[TRK_TRACKS];
    QLabel *state_[TRK_TRACKS];
};

#endif /* TRACKER_QT_TRACKERWIDGET_H */
