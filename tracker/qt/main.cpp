// tracker -- the standalone binary: a thin shell around TrackerWidget, which
// lives in qt/trackerwidget.h so the studio window can host the same widget in
// a tab. This file is only the QMainWindow behind the widget's TrackerHost --
// a menu bar and a status line -- plus main() and, under TRACKER_UITEST, the
// scripted offscreen drive.
#include "trackerwidget.h"

#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>

#include <QElapsedTimer>

namespace {

// --------------------------------------------------- the standalone window --
//
// The tracker binary's shell: a menu bar and a status line behind the
// widget's TrackerHost, the window title following the song's, and closing
// the window ends the song session.

class TrackerWindow : public QMainWindow, public TrackerHost {
public:
    explicit TrackerWindow(trk_engine *e)
    {
        tracker_ = new TrackerWidget(e, this, this);
        setCentralWidget(tracker_);
        connect(tracker_, &QWidget::windowTitleChanged, this, &QWidget::setWindowTitle);
        setWindowTitle(tracker_->windowTitle());   // set before the connect above
        resize(tracker_->preferredWidth(), 760);
    }

    TrackerWidget *tracker() const { return tracker_; }

    QMenu *addMenu(const QString &title) override { return menuBar()->addMenu(title); }
    void showStatus(const QString &msg, int ms) override { statusBar()->showMessage(msg, ms); }
    void requestQuit() override { close(); }

protected:
    void closeEvent(QCloseEvent *ev) override
    {
        if (tracker_->confirmClose()) ev->accept();
        else ev->ignore();
    }

private:
    TrackerWidget *tracker_;
};

#ifdef TRACKER_UITEST
// The window driven by real key events, offscreen, with a picture taken at
// each step. Run it with QT_QPA_PLATFORM=offscreen; argv: song, output dir.
// Exit status is the number of failed checks.
int g_fail = 0;

void check(bool ok, const char *what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

void press(QWidget *w, int key, const QString &text = QString(), Qt::KeyboardModifiers m = Qt::NoModifier)
{
    QKeyEvent down(QEvent::KeyPress, key, m, text);
    QKeyEvent up(QEvent::KeyRelease, key, m, text);
    QApplication::sendEvent(w, &down);
    QApplication::sendEvent(w, &up);
}

void wait(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

int uitest(trk_engine *e, TrackerWindow &w, const QString &outdir)
{
    TrackerWidget *tw = w.tracker();
    PatternView *v = tw->view();
    trk_editor *ed = tw->editor();
    auto shot = [&](const char *name) {
        w.grab().save(outdir + "/" + name);
        std::printf("  picture: %s/%s\n", outdir.toLocal8Bit().constData(), name);
    };
    auto cell = [&](int p, int r, int t) {
        trk_lock(e);
        trk_cell c = trk_song_of(e)->pattern[p].cell[r][t];
        trk_unlock(e);
        return c;
    };

    wait(300);
    shot("01-loaded.png");
    {   // The set editor, as Samples > Edit Sample Set opens it.
        SetEditor se(e, "drum-singles", &w);
        se.show();
        wait(400);
        se.grab().save(outdir + "/05-editor.png");
        std::printf("  picture: %s/05-editor.png\n", outdir.toLocal8Bit().constData());
        se.hide();
    }
    {   // A block selected -- and the cursor put back, for the steps after.
        const int row = ed->row, track = ed->track;
        trk_select(ed, 2, 0, 9, 2);
        v->update();
        wait(200);
        shot("07-selected.png");
        trk_select_none(ed);
        ed->row = row;
        ed->track = track;
        v->update();
    }
    {   // Help > Cheat Sheet.
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "&Cheat Sheet") a->trigger();
        wait(300);
        if (QDialog *d = w.findChild<QDialog *>()) {
            d->grab().save(outdir + "/06-cheat.png");
            std::printf("  picture: %s/06-cheat.png\n", outdir.toLocal8Bit().constData());
            d->hide();
        }
    }
    check(cell(0, 0, 0).note == 36, "song loaded: C-2 on track 1, row 0");
    check(tw->dest(0)->currentText().startsWith("aseqdump"), "track 1 shows its destination");
    check(tw->dest(2)->currentText().contains("(not open)"), "a closed destination is kept and marked");

    std::printf("typing\n");
    v->setFocus();
    press(v, Qt::Key_Home);
    press(v, Qt::Key_Tab);
    press(v, Qt::Key_Tab);                        // track 3
    check(ed->track == 2 && ed->row == 0, "Tab moves to track 3");
    press(v, Qt::Key_Z, "z");                     // C-4
    press(v, Qt::Key_C, "c");                     // E-4
    press(v, Qt::Key_B, "b");                     // G-4
    press(v, Qt::Key_1, "1");                     // off
    check(cell(0, 0, 2).note == 60 && cell(0, 1, 2).note == 64 &&
          cell(0, 2, 2).note == 67 && cell(0, 3, 2).note == TRK_NOTE_OFF,
          "z c b 1 wrote C-4 E-4 G-4 ===, one row each");
    check(ed->row == 4, "cursor advanced by the step");
    press(v, Qt::Key_Up);
    press(v, Qt::Key_Up);
    press(v, Qt::Key_Up);                         // row 1
    press(v, Qt::Key_Right);                      // velocity
    press(v, Qt::Key_4, "4");
    press(v, Qt::Key_0, "0");
    check(cell(0, 1, 2).vel == 0x40, "hex 4 0 into velocity");
    press(v, Qt::Key_Right);                      // cc
    press(v, Qt::Key_9, "9");
    press(v, Qt::Key_9, "9");                     // clamps to 79
    check(cell(0, 2, 2).cc == 0x79, "high digit above 7 clamps to 7");
    press(v, Qt::Key_BracketRight);
    check(ed->octave == 5, "] raises the octave");
    press(v, Qt::Key_Delete);
    check(cell(0, 3, 2).cc == TRK_EMPTY, "Delete clears the field under the cursor");
    shot("02-typed.png");

    std::printf("playing\n");
    press(v, Qt::Key_F6);
    wait(700);
    int o, p, r;
    trk_position(e, &o, &p, &r);
    check(trk_playing(e) && p == 0 && r > 0, "F6 plays the pattern and the position moves");
    shot("03-playing.png");
    wait(500);
    press(v, Qt::Key_F8);
    wait(100);
    check(!trk_playing(e), "F8 stops");

    press(v, Qt::Key_F5);
    wait(2300);                                   // into the second order entry
    trk_position(e, &o, &p, &r);
    check(trk_playing(e) && o >= 1, "F5 plays the song through the order list");
    check(ed->pattern == p, "the editor follows the pattern being played");
    shot("04-song.png");
    press(v, Qt::Key_Return);
    wait(100);
    check(!trk_playing(e), "Enter stops");
    {   // F7 records: the count-in first, then the take; Stop ends it.
        trk_rec_opts o;
        trk_record_get(e, &o);
        o.count_in = 0; o.metronome = 0;
        trk_record_set(e, &o);
        press(v, Qt::Key_F7);
        wait(150);
        check(trk_recording(e) == 1, "F7 starts a take");
        press(v, Qt::Key_F8);
        wait(100);
        check(trk_recording(e) == 0 && !trk_playing(e), "Stop ends it");
    }
    {   // Space is edit mode now: it toggles, and with it off the note keys only play.
        const int was = ed->edit;
        press(v, Qt::Key_Space);
        check(ed->edit == !was, "Space toggles edit mode");
        press(v, Qt::Key_Space);
        check(ed->edit == was, "and back");
    }

    std::printf("saving\n");
    const QString path = outdir + "/saved.trk";
    check(tw->writeSong(path), "saved");
    {
        trk_song *back = (trk_song *)std::malloc(sizeof(trk_song));
        char err[256];
        const int rc = trk_song_load(back, path.toLocal8Bit().constData(), err, sizeof err);
        trk_lock(e);
        check(rc == 0 && std::memcmp(back, trk_song_of(e), sizeof(trk_song)) == 0,
              "what was saved loads back as the same song");
        trk_unlock(e);
        std::free(back);
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail;
}
#endif

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("tracker");
    char err[256];
    trk_engine *e = trk_open(err, sizeof err);
    if (!e) {
        QMessageBox::critical(nullptr, "tracker", QString::fromLocal8Bit(err));
        return 1;
    }
    int rc;
    {
        TrackerWindow w(e);
        if (argc > 1) w.tracker()->openPath(QString::fromLocal8Bit(argv[1]));
        w.show();
#ifdef TRACKER_UITEST
        rc = uitest(e, w, argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString("."));
#else
        rc = app.exec();
#endif
    }
    trk_close(e);       // releases anything still sounding
    return rc;
}
