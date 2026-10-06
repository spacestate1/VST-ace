/* studio -- the session shell: one window, a tab per synth plug-in, one tab
 * for the tracker.
 *
 * pestudio hosts one plug-in per window; studio is the same host several times
 * over under one roof, beside the tracker, so a song and the instruments it
 * plays live in one window. The tabs do the work: a synth tab is a HostWidget
 * (peload/qtgui/hostwindow.h) and the tracker tab is a TrackerWidget
 * (tracker/qt/trackerwidget.h). This file is only what both ask a frame for --
 * a menu bar, a status line, tab bookkeeping, and a say over which tab's piano
 * answers the computer keyboard. */

#include <QtWidgets>

#include "hostwindow.h"
#include "trackerwidget.h"

extern "C" {
#include "trk.h"
#include "version.h"
}

/* The shell answers to both widgets' host interfaces. HostShell::addMenu and
 * TrackerHost::addMenu are the same signature, so one override serves both;
 * the status line is likewise shared. */
class SessionShell : public QMainWindow, public HostShell, public TrackerHost {
    Q_OBJECT
public:
    SessionShell()
    {
        setWindowTitle("studio -- vst-ace session");
        resize(1280, 800);

        /* The File and Help menus the window itself owns. Everything else on
         * the menu bar belongs to a tab -- see addMenu. */
        QMenu *file = menuBar()->addMenu("&File");
        file->addAction("New &synth...", this, &SessionShell::newSynth);
        newTracker_ = file->addAction("New &tracker", this, &SessionShell::newTracker);
        file->addAction("&Open song...", this, &SessionShell::openSong);
        file->addSeparator();
        QAction *quit = file->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);

        QMenu *help = menuBar()->addMenu("&Help");
        help->addAction("&About studio", this, &SessionShell::about);

        /* No tabs at start: the canvas opens blank, with the way in said on
         * it. The stack is the hint page until the first tab exists. */
        stack_ = new QStackedWidget(this);
        hint_ = new QLabel(
            "Nothing is open.\n\n"
            "File > New synth... opens a plug-in host in a tab, one tab per "
            "plug-in.\nFile > New tracker opens the pattern sequencer; File > "
            "Open song... loads a song into it.", this);
        hint_->setAlignment(Qt::AlignCenter);
        hint_->setStyleSheet("color:#888");
        stack_->addWidget(hint_);

        tabs_ = new QTabWidget;
        tabs_->setTabsClosable(true);
        tabs_->setMovable(true);
        stack_->addWidget(tabs_);
        setCentralWidget(stack_);

        connect(tabs_, &QTabWidget::currentChanged, this,
                &SessionShell::arbitrateKeys);
        connect(tabs_, &QTabWidget::tabCloseRequested, this,
                &SessionShell::closeTab);

        statusBar()->showMessage("open a synth or the tracker from the File menu", 0);
    }

    /* The command line's session: each --synth a tab, --tracker the tracker,
     * --song into it. */
    void openSynth(const QString &plugin)
    {
        HostWidget *h = addSynthTab();
        if (!h->loadPlugin(plugin))
            statusBar()->showMessage("could not load " + plugin, 0);
    }
    bool openTracker() { return addTrackerTab() != nullptr; }
    bool openSongPath(const QString &path)
    {
        TrackerWidget *t = addTrackerTab();
        return t && t->openPath(path);
    }
    int tabCount() const { return tabs_->count(); }

    /* Scripted exercise for a machine with no XTEST, in the idiom of
     * pestudio's --cycle: what clicking through the smoke test by hand cannot
     * prove twice. Every second the next tab is brought to the front (so the
     * keys-live arbitration runs), a note is played into every loaded synth
     * (so its peak meter has something to say), and each engine's callback
     * count and peak are printed. Three seconds before the end the first
     * synth tab is closed through the ordinary close path, and at `ms` the
     * application quits. */
    void startSmokeDrive(int ms)
    {
        smokeStep_ = 0;
        auto *tick = new QTimer(this);
        connect(tick, &QTimer::timeout, this, [this] {
            const int n = tabs_->count();
            if (n) tabs_->setCurrentIndex(smokeStep_ % n);
            for (int i = 0; i < n; i++) {
                auto *h = qobject_cast<HostWidget *>(tabs_->widget(i));
                if (!h) {
                    fprintf(stderr, "smoke: tab %d \"%s\" (tracker)\n",
                            i, qPrintable(tabs_->tabText(i)));
                    continue;
                }
                pehost *ph = h->engine()->host();
                if (ph) {
                    pehost_note_off(ph, smokeNote_);
                    smokeNote_ = 60 + (smokeStep_ + i) % 12;
                    pehost_note_on(ph, smokeNote_, 100);
                }
                unsigned dropped = 0, spilled = 0;
                if (ph) pehost_midi_stats(ph, &dropped, &spilled);
                fprintf(stderr, "smoke: tab %d \"%s\" callbacks=%lu peak=%.3f "
                                "keysLive=%d dropped=%u spilled=%u%s\n",
                        i, qPrintable(tabs_->tabText(i)),
                        h->engine()->callbacks(), h->engine()->peak(),
                        int(h->keysLive()), dropped, spilled,
                        ph ? "" : " (no plug-in)");
            }
            smokeStep_++;
            fflush(stderr);
        });
        tick->start(1000);
        QTimer::singleShot(qMax(1000, ms - 3000), this, [this] {
            for (int i = 0; i < tabs_->count(); i++)
                if (qobject_cast<HostWidget *>(tabs_->widget(i))) {
                    fprintf(stderr, "smoke: closing tab %d\n", i);
                    fflush(stderr);
                    closeTab(i);
                    break;
                }
        });
        QTimer::singleShot(ms, qApp, &QCoreApplication::quit);
    }

    /* -- HostShell / TrackerHost ---------------------------------------- */
    void statusMessage(const QString &text, int timeoutMs) override
    { statusBar()->showMessage(text, timeoutMs); }
    void showStatus(const QString &msg, int ms) override { statusMessage(msg, ms); }
    void requestQuit() override { close(); }

    /* A tab's menus are added where it asks, while it is being constructed:
     * building_ marks that window, and the menus collected go to the tab once
     * it exists, so closing the tab can take them off the bar again. They stay
     * put while the tab lives rather than following the current tab -- a menu
     * that appears and disappears as tabs are switched is where actions go to
     * be lost. */
    QMenu *addMenu(const QString &title) override
    {
        QMenu *m = menuBar()->addMenu(title);
        if (building_) pendingMenus_ << m;
        return m;
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        /* The tracker's song may have unsaved changes; closing the window asks
         * exactly as closing its tab does. */
        if (tracker_ && !tracker_->confirmClose()) { e->ignore(); return; }
        /* The crash marker belongs to the process, and the close comes to the
         * shell rather than to any HostWidget -- see HostWidget::closeEvent. */
        HostWidget::clearCrashMarker();
        QMainWindow::closeEvent(e);
    }

private:
    HostWidget *addSynthTab(const QString &title = QString())
    {
        /* The crash marker means "the plug-in being loaded when the last
         * session died", and the HostWidget constructor reads whatever it
         * finds. With several hosts in one process, a tab's own load leaves
         * the marker behind for the NEXT tab's constructor, where it reads as
         * a crash that never happened -- the status line came up saying
         * "skipped blooo64.dll -- it did not survive the last session" about
         * the plug-in playing happily in the tab beside it. The first tab's
         * read is the honest one; clear before the rest. */
        if (sawHost_) HostWidget::clearCrashMarker();
        sawHost_ = true;
        /* Menus are built inside the constructor, before there is a widget to
         * own them -- collected under building_ and handed over after. */
        building_ = true;
        auto *h = new HostWidget(this);
        building_ = false;
        menusOf_[h] = pendingMenus_;
        pendingMenus_.clear();

        const int ix = tabs_->addTab(h, title.isEmpty() ? QString("synth") : title);
        /* Which plug-in is loaded, on the tab. HostWidget sets its widget
         * title to the plug-in's name on every load -- including the one it
         * makes during construction, before the connect below, so the title
         * is taken verbatim as well. */
        if (!h->windowTitle().isEmpty()) tabs_->setTabText(ix, h->windowTitle());
        connect(h, &QWidget::windowTitleChanged, this, [this, h](const QString &t) {
            const int i = tabs_->indexOf(h);
            if (i >= 0) tabs_->setTabText(i, t.isEmpty() ? QString("synth") : t);
        });
        tabs_->setCurrentIndex(ix);
        updateCanvas();
        return h;
    }

    TrackerWidget *addTrackerTab()
    {
        if (tracker_) {
            tabs_->setCurrentWidget(tracker_);
            return tracker_;
        }
        /* One engine per process, opened on first use: trk_open claims an ALSA
         * sequencer client, and a blank canvas should not be holding one. */
        char err[256];
        trkEngine_ = trk_open(err, sizeof err);
        if (!trkEngine_) {
            QMessageBox::warning(this, "studio",
                                 QString("the tracker could not start: %1")
                                     .arg(QString::fromLocal8Bit(err)));
            return nullptr;
        }
        building_ = true;
        auto *t = new TrackerWidget(trkEngine_, this);
        building_ = false;
        menusOf_[t] = pendingMenus_;
        pendingMenus_.clear();
        tracker_ = t;
        const int ix = tabs_->addTab(t, t->windowTitle());
        connect(t, &QWidget::windowTitleChanged, this, [this, t](const QString &s) {
            const int i = tabs_->indexOf(t);
            if (i >= 0) tabs_->setTabText(i, s);
        });
        tabs_->setCurrentIndex(ix);
        /* One tracker per process -- the engine behind it is single-instance,
         * so a second tab would only be two faces of one song. */
        newTracker_->setEnabled(false);
        updateCanvas();
        return t;
    }

    void newSynth() { addSynthTab(); }
    void newTracker() { addTrackerTab(); }
    void openSong()
    {
        const QString p = QFileDialog::getOpenFileName(this, "Open song", QString(),
                                                       "Tracker songs (*.trk);;All files (*)");
        if (!p.isEmpty()) openSongPath(p);
    }

    void closeTab(int ix)
    {
        QWidget *w = tabs_->widget(ix);
        if (!w) return;
        if (w == tracker_ && !tracker_->confirmClose()) return;

        /* Menus first, while the widget still exists to be asked nothing. */
        for (QMenu *m : menusOf_.take(w)) {
            menuBar()->removeAction(m->menuAction());
            delete m;
        }
        tabs_->removeTab(ix);
        if (w == tracker_) {
            delete w;                    /* done with the engine: playback is stopped */
            tracker_ = nullptr;
            trk_close(trkEngine_);
            trkEngine_ = nullptr;
            newTracker_->setEnabled(true);
        } else {
            /* ~HostWidget lets go of the plug-in cleanly: the MIDI reader is
             * stopped and the editors detached in its body, then the Engine's
             * own teardown stops the PipeWire loop before pehost_close. */
            delete w;
        }
        updateCanvas();
        arbitrateKeys();
    }

    /* Only the tab in front answers the computer keyboard. Every HostWidget
     * filters keys application-wide, and in one window they would otherwise
     * all answer at once -- see HostWidget::setKeysLive. Switching away from a
     * tab releases its held notes on the way out. */
    void arbitrateKeys()
    {
        QWidget *cur = tabs_->currentWidget();
        for (int i = 0; i < tabs_->count(); i++)
            if (auto *h = qobject_cast<HostWidget *>(tabs_->widget(i)))
                h->setKeysLive(h->isVisible() && tabs_->widget(i) == cur);
    }

    void updateCanvas()
    { stack_->setCurrentWidget(tabs_->count() ? static_cast<QWidget *>(tabs_)
                                              : static_cast<QWidget *>(hint_)); }

    void about()
    {
        /* A plain QDialog, not QMessageBox::about: the platform theme path
         * behind the convenience function has crashed this process before --
         * the note is where HostWidget builds its own About. */
        QDialog dlg(this);
        dlg.setWindowTitle("About studio");
        dlg.setModal(true);
        QString git = QString(VSTACE_GIT).isEmpty()
                          ? QString() : QString("<br>Commit %1").arg(VSTACE_GIT);
        QLabel *body = new QLabel(
            QString("<b>vst-ace %1</b><br><br>"
                    "Built %2%3<br><br>"
                    "A session window: each tab hosts one plug-in natively -- "
                    "Windows, macOS or Linux, no Wine, no emulation -- and one "
                    "tab is the pattern tracker that plays them over MIDI."
                    "<br><br>"
                    "This window is studio (Qt %4).")
                .arg(VSTACE_VERSION).arg(VSTACE_BUILD_DATE).arg(git)
                .arg(QT_VERSION_STR), &dlg);
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        body->setMinimumWidth(420);
        QPushButton *close = new QPushButton("Close", &dlg);
        close->setDefault(true);
        connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->addWidget(body);
        QHBoxLayout *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(close);
        lay->addLayout(row);
        dlg.exec();
    }

    QStackedWidget *stack_;
    QTabWidget     *tabs_;
    QLabel         *hint_ = nullptr;
    QAction        *newTracker_ = nullptr;
    TrackerWidget  *tracker_ = nullptr;
    trk_engine     *trkEngine_ = nullptr;
    /* Set around each tab's construction, which is when both widgets build
     * their menus through addMenu; the menus collected are filed under the
     * new tab once it exists -- see menusOf_. */
    bool           building_ = false;
    QList<QMenu *> pendingMenus_;
    QHash<QWidget *, QList<QMenu *>> menusOf_;
    int            smokeStep_ = 0, smokeNote_ = 60;
    bool           sawHost_ = false;   /* the crash marker is the first tab's */
};

/* Let the window system deliver input while a plugin spins in its own drag
 * loop. Bounded, because this is called from inside that loop. Same pump
 * pestudio installs. */
static void pump_input(void *ud)
{
    (void)ud;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
}

int main(int argc, char **argv)
{
    /* Plugin editors are X11 windows, so this process has to be an X11 client
     * -- the same coercion pestudio does, for the same reason: under Qt's
     * Wayland backend winId() is a surface handle, and a plugin handed one as
     * an X11 embed id dies inside its own toolkit looking like a crash on
     * load. Set QT_QPA_PLATFORM yourself to override. */
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        const QByteArray session = qgetenv("XDG_SESSION_TYPE");
        if (session == "wayland" || qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
            if (qEnvironmentVariableIsSet("DISPLAY")) {
                qputenv("QT_QPA_PLATFORM", "xcb");
                fprintf(stderr, "studio: Wayland session -- using the xcb "
                                "backend so plugin editors can embed\n");
            } else {
                fprintf(stderr, "studio: Wayland session with no DISPLAY; "
                                "plugin editors need XWayland and will be "
                                "refused\n");
            }
        }
    }
    /* Out-of-process hosting by default, as pestudio: a plug-in that faults
     * costs a helper subprocess rather than the whole session. */
    if (!qEnvironmentVariableIsSet("PEHOST_ISOLATE")) {
        pehost_set_isolation(1);
        fprintf(stderr, "studio: hosting plugins out-of-process "
                        "(PEHOST_ISOLATE=0 to disable)\n");
    }

    QApplication app(argc, argv);
    app.setApplicationName("studio");
    /* Before any plugin is opened: the Classic backend is handed this when its
     * shim is built. */
    pehost_set_input_pump(pump_input, nullptr);

    /* A session can be named rather than clicked together:
     *
     *   studio --synth blooo64.dll --synth "Surge XT.vst3" --tracker
     *   studio --song song.trk
     *   studio --smoke 15000 --synth ...   scripted exercise, then quit
     *   studio --quit-after 15000 ...      just quit then
     */
    QStringList synths;
    QString song;
    bool wantTracker = false;
    int quitAfter = 0, smoke = 0;
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--synth" && i + 1 < argc)
            synths << QString::fromLocal8Bit(argv[++i]);
        else if (a == "--tracker")
            wantTracker = true;
        else if (a == "--song" && i + 1 < argc)
            song = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--quit-after" && i + 1 < argc)
            quitAfter = atoi(argv[++i]);
        else if (a == "--smoke" && i + 1 < argc)
            smoke = atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            printf("studio [--synth <plug-in>]... [--tracker] [--song <file.trk>]\n"
                   "       [--smoke <ms>] [--quit-after <ms>]\n\n"
                   "The session window: a tab per synth plug-in, one for the "
                   "tracker.\nWith no arguments it opens on a blank canvas.\n");
            return 0;
        } else {
            fprintf(stderr, "studio: unknown argument %s -- try --help\n",
                    qPrintable(a));
            return 2;
        }
    }

    SessionShell w;
    w.show();
    for (const QString &s : synths) w.openSynth(s);
    if (wantTracker) w.openTracker();
    if (!song.isEmpty()) w.openSongPath(song);
    if (smoke > 0)
        w.startSmokeDrive(smoke);
    else if (quitAfter > 0)
        QTimer::singleShot(quitAfter, &app, &QCoreApplication::quit);
    return app.exec();
}

#include "main.moc"
