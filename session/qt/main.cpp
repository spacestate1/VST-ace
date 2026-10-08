/* studio -- the session shell: one window, a tab per synth plug-in, one tab
 * for the tracker.
 *
 * pestudio hosts one plug-in per window; studio is the same host several times
 * over under one roof, beside the tracker, so a song and the instruments it
 * plays live in one window. The tabs do the work: a synth tab is a HostWidget
 * (peload/qtgui/hostwindow.h) and the tracker tab is a TrackerWidget
 * (tracker/qt/trackerwidget.h). This file is only what both ask a frame for --
 * a menu bar, a status line, tab bookkeeping, and a say over which tab's piano
 * answers the computer keyboard.
 *
 * And the routing: every synth tab is registered with the tracker's engine as
 * an in-process MIDI sink (trk_add_sink), named by its plug-in, so a track can
 * play a tab directly -- the track's destination list shows it as "this
 * window: <name>" beside the ALSA windows, and picking it moves the track,
 * exactly as moving it to another window does. Delivery is sample-accurate:
 * the engine's delivery thread hands each tab's Engine wall-clock-stamped
 * blocks (sinkDeliver), and the Engine places every event in its own rendered
 * block on the sample (Engine::injectMidi). A tab closing unregisters its
 * sink first -- trk_remove_sink waits out any delivery in flight -- so the
 * tracker never calls into a dead engine, and tracks routed there fall back
 * to the ALSA windows their songs name.
 *
 * Two more things a frame owes several tabs rather than one. A plug-in whose
 * helper died for good -- its HostWidget restarted it three times and gave up
 * -- is marked on its tab by the shell's watchdog, and File > Reload plug-in
 * is the way back; the recoverable deaths never reach here, HostWidget
 * restarts those itself and says so on the status line. And the whole session
 * -- the open tabs with their plug-ins and sounds, the song, which tracks
 * play which tab -- is one JSON file (session/sessfile.h), written from
 * File > Save session and read back by File > Open session or --session. */

#include <QtWidgets>

#include <cstring>
#include <vector>

#include "hostwindow.h"
#include "trackerwidget.h"

extern "C" {
#include "trk.h"
#include "version.h"
#include "patch.h"
#include "sessfile.h"
#include "recentfiles.h"
}

/* The tracker's delivery into a synth tab: the engine's delivery thread calls
 * this with one block of events, each placed in the block by its frame
 * offset; the tab's Engine re-places them into its own rendered block by
 * wall-clock time. Never blocks, never calls back into the tracker -- the
 * delivery lock is held while this runs. */
struct SinkCtx { Engine *eng; HostWidget *tab; };

/* The tracker's fader for a track alone on this synth: the tab's output gain. */
static void sinkGain(void *ud, double g)
{
    static_cast<SinkCtx *>(ud)->eng->trackGain_.store(float(g), std::memory_order_relaxed);
}

/* The tracker's sample tracks on the studio mix bus (File > Record studio mix). */
static void trackerTap(void *ud, const double *x, int frames)
{
    MixBus::instance().feed(int(intptr_t(ud)), x, frames);
}

static void sinkDeliver(void *ud, double wall, const trk_sink_ev *evs, int n)
{
    SinkCtx *ctx = static_cast<SinkCtx *>(ud);
    for (int i = 0; i < n; i++) {
        ctx->eng->injectMidi(wall + double(evs[i].frame) / TRK_SINK_RATE,
                             evs[i].status, evs[i].d1, evs[i].d2);
        /* The keys of the tab it plays on follow the notes: a note-on lights
         * one, a note-off or a note-on at velocity 0 puts it up, and the
         * all-notes-off controllers let every key up. */
        const int st = evs[i].status & 0xF0;
        if (st == 0x90 && evs[i].d2 > 0)            ctx->tab->showPlayedNote(evs[i].d1, true);
        else if (st == 0x80 || st == 0x90)          ctx->tab->showPlayedNote(evs[i].d1, false);
        else if (st == 0xB0 && (evs[i].d1 == 123 || evs[i].d1 == 120)) ctx->tab->showPlayedNote(-1, false);
    }
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

        /* The File and Help menus the window itself owns. The tabs' menus
         * merge into these by title -- see addMenu -- and unlike a tab's
         * share they never leave the bar and are never hidden by syncMenus. */
        QMenu *file = menuBar()->addMenu("&File");
        file->addAction("New &synth...", this, &SessionShell::newSynth);
        newTracker_ = file->addAction("New &tracker", this, &SessionShell::newTracker);
        openSongAct_ = file->addAction("&Open song...", this, &SessionShell::openSong);
        /* Open Recent: songs and sessions, from the list the GTK shell keeps too.
         * Built when the menu is about to show, so it is always current. */
        recentMenu_ = file->addMenu("Open &Recent");
        connect(recentMenu_, &QMenu::aboutToShow, this, &SessionShell::fillRecent);
        /* Where a tab's own File items go -- see attributeMenus. */
        fileAnchor_ = file->addSeparator();
        file->addAction("Op&en session...", this, &SessionShell::openSession);
        file->addAction("&Save session", this, &SessionShell::saveSession);
        file->addAction("Save session &as...", this, &SessionShell::saveSessionAs);
        file->addSeparator();
        reloadAct_ = file->addAction("&Reload plug-in", this, &SessionShell::reloadPlugin);
        reloadAct_->setEnabled(false);
        file->addSeparator();
        /* Plug-in folders from the window itself: a setting is not to be out of
         * reach until a synth tab is in front. */
        file->addAction("&Plug-ins...", this, &SessionShell::pluginManager);
        file->addAction("Plug-in &folders...", this, &SessionShell::pluginFolders);
        file->addAction("&Audio output...", this, &SessionShell::audioSettings);
        file->addSeparator();
        /* One take of everything the studio plays: every synth tab and the
         * tracker's sample tracks, mixed. The tabs' own Record buttons take
         * one tab alone. */
        mixAct_ = file->addAction("Record studio &mix", QKeySequence("Ctrl+Shift+R"), this,
                                  &SessionShell::toggleMixRecord);
        mixLabel_ = new QLabel;
        mixLabel_->setStyleSheet("color: #d04040; font-weight: bold;");
        statusBar()->addPermanentWidget(mixLabel_);
        mixTimer_ = new QTimer(this);
        connect(mixTimer_, &QTimer::timeout, this, &SessionShell::tickMixRecord);
        file->addSeparator();
        QAction *quit = file->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);

        QMenu *help = menuBar()->addMenu("&Help");
        helpAnchor_ = help->addAction("&About studio", this, &SessionShell::about);
        fileMenu_ = file;
        helpMenu_ = help;
        shellMenus_ << file << help;

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

        /* Once a second: the tab-face bookkeeping nothing else owns. A tab
         * whose plug-in's helper died for good is marked on its tab (the
         * recoverable deaths are HostWidget's own business -- it restarts
         * them and says so on the status line), and every tab's tooltip says
         * what its audio is doing, from the engine's callback counter. */
        auto *watch = new QTimer(this);
        connect(watch, &QTimer::timeout, this, &SessionShell::watchTabs);
        watch->start(1000);

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
        /* openPath tells the host -- songOpened below -- so the tracker
         * tab's own File > Open takes the same road as this one. */
        TrackerWidget *t = addTrackerTab();
        const bool ok = t && t->openPath(path);
        if (ok) recent_add("song", QFileInfo(path).absoluteFilePath().toLocal8Bit().constData());
        return ok;
    }

    /* File > Open Recent. */
    void fillRecent()
    {
        recentMenu_->clear();
        recent_item items[RECENT_SHOWN];
        const int n = recent_list(items, RECENT_SHOWN);
        for (int i = 0; i < n; i++) {
            const QString path = QString::fromLocal8Bit(items[i].path);
            const bool session = !strcmp(items[i].kind, "session");
            const QFileInfo fi(path);
            QAction *a = recentMenu_->addAction(
                QString("%1%2  \u2014  %3").arg(fi.fileName(), session ? " (session)" : "",
                                              fi.absolutePath()).replace('&', "&&"));
            a->setToolTip(path);
            connect(a, &QAction::triggered, this, [this, path, session] {
                if (session) openSessionPath(path); else openSongPath(path);
            });
        }
        if (!n) recentMenu_->addAction("(nothing yet)")->setEnabled(false);
        recentMenu_->addSeparator();
        QAction *clear = recentMenu_->addAction("Clear Recent", this, [] { recent_clear(); });
        clear->setEnabled(n > 0);
    }
    int tabCount() const { return tabs_->count(); }

    /* Restore a saved session in place of whatever is open: the tabs with
     * their plug-ins and sounds, then the song, then -- last, because they
     * name the tabs -- the routings. The file is read before anything is
     * closed, so a session that will not parse costs nothing, and an unsaved
     * song is asked about before it goes (Cancel leaves everything as it
     * was). Anything in the file that will not come back is said about and
     * skipped, not fatal: half a session back is worth more than none. */
    bool openSessionPath(const QString &path)
    {
        char err[512];
        sess_file *s = sess_read(path.toLocal8Bit().constData(), err, sizeof err);
        if (!s) {
            statusBar()->showMessage(QString("could not open the session: %1")
                                         .arg(QString::fromLocal8Bit(err)), 0);
            fprintf(stderr, "session: could not open %s -- %s\n", qPrintable(path), err);
            fflush(stderr);
            return false;
        }
        recent_add("session", QFileInfo(path).absoluteFilePath().toLocal8Bit().constData());
        /* A session names the plug-ins to load, and loading one runs it: a
         * file somebody sent is not to choose what runs. The ones in folders
         * set up for this program go ahead; any other is asked about. */
        {
            QStringList unknown;
            for (int i = 0; i < s->nsynths && i < 16; i++) {
                const char *pl = s->synths[i].plugin;
                if (pl && *pl && !vstdirs_contains(pl)) unknown << QString::fromLocal8Bit(pl);
            }
            if (!unknown.isEmpty() && !qEnvironmentVariableIsSet("STUDIO_TRUST_SESSIONS") &&
                QMessageBox::question(this, "Open session",
                    QString("This session loads %1 plug-in%2 from outside the folders "
                            "set up for studio. Loading a plug-in runs it.\n\n%3\n\nLoad %4?")
                        .arg(unknown.size()).arg(unknown.size() > 1 ? "s" : "")
                        .arg(unknown.join("\n"), unknown.size() > 1 ? "them" : "it"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
                sess_free(s);
                statusBar()->showMessage("session not opened -- its plug-ins were not approved", 5000);
                return false;
            }
        }
        if (!closeAllTabs()) {
            sess_free(s);
            statusBar()->showMessage("session not opened -- the song was kept", 5000);
            fprintf(stderr, "session: %s not opened -- cancelled at the song\n",
                    qPrintable(path));
            fflush(stderr);
            return false;
        }
        sessionPath_.clear();
        QStringList trouble;
        QList<HostWidget *> made;            /* the tab each saved synth came back as */
        /* A file is not to choose how many windows and helper processes open:
         * the GTK shell stops at MAXTABS, and so does this. */
        static const int kMaxSynthTabs = 16;
        if (s->nsynths > kMaxSynthTabs)
            trouble << QString("%1 synths past the %2-tab limit").arg(s->nsynths - kMaxSynthTabs).arg(kMaxSynthTabs);
        for (int i = 0; i < s->nsynths && i < kMaxSynthTabs; i++) {
            made << nullptr;
            if (!s->synths[i].plugin || !*s->synths[i].plugin) continue;
            const QString plugin = QString::fromLocal8Bit(s->synths[i].plugin);
            HostWidget *h = addSynthTab();
            if (!h->loadPlugin(plugin)) {
                /* No empty tab left standing for it: the message says what
                 * did not come back, and an empty tab would be saved over the
                 * session as one that never had anything in it. */
                closeTabNow(tabs_->indexOf(h));
                trouble << QFileInfo(plugin).fileName();
                continue;
            }
            made[i] = h;
            QString why;
            if (s->synths[i].patch && !h->applyPatchText(s->synths[i].patch, &why))
                trouble << QString("%1 (its sound: %2)").arg(QFileInfo(plugin).fileName(), why);
        }
        if (s->song && *s->song) {
            if (!openSongPath(QString::fromLocal8Bit(s->song)))
                trouble << QString::fromLocal8Bit(s->song);
        } else if (s->nroutes) {
            addTrackerTab();             /* the routes need somewhere to point */
        }
        /* Last, now that the tabs exist to be found: by the saved synth's
         * index, mapped to the tab this restore made for it, so a tab that was
         * already open -- or a second instance of the same plug-in -- cannot
         * capture the route. A file without indices falls back to the name,
         * among this restore's tabs only. A route whose tab failed to load is
         * dropped. */
        int routed = 0;
        for (int i = 0; i < s->nroutes; i++) {
            const sess_route &r = s->routes[i];
            bool done = false;
            if (trkEngine_ && r.track >= 0 && r.track < TRK_TRACKS) {
                const QString sink = QString::fromUtf8(r.sink ? r.sink : "");
                for (const SinkEntry &e : sinks_) {
                    const qsizetype at = made.indexOf(e.tab);
                    if (at < 0) continue;
                    if (r.synth >= 0 ? at == r.synth : e.name == sink) {
                        trk_route_sink(trkEngine_, r.track, e.id);
                        routed++;
                        done = true;
                        break;
                    }
                }
            }
            if (!done)
                trouble << QString("track %1's route to %2").arg(r.track + 1)
                               .arg(QString::fromUtf8(r.sink && *r.sink ? r.sink : "a synth"));
        }
        sessionPath_ = path;
        statusBar()->showMessage(
            trouble.isEmpty()
                ? QString("session restored from %1").arg(path)
                : QString("session restored; could not bring back: %1")
                      .arg(trouble.join(", ")), 0);
        fprintf(stderr, "session: restored %s -- %d tab(s), %d route(s)%s\n",
                qPrintable(path), s->nsynths, routed,
                trouble.isEmpty() ? "" : " (with losses)");
        fflush(stderr);
        sess_free(s);
        return true;
    }

    /* Save on a clean exit, for --save-session: the unsaved-song question has
     * already been answered by closeEvent's confirmClose before this runs. */
    void saveSessionOnExit(const QString &path) { saveOnExit_ = path; }

    /* --route <track>: play the track into the first synth tab, in-process,
     * and start the song. A four-note figure goes into the pattern first so
     * the scripted proof does not depend on the song's contents. */
    bool routeTrack(int track)
    {
        if (!trkEngine_ || track < 0 || track >= TRK_TRACKS || sinks_.isEmpty())
            return false;
        trk_lock(trkEngine_);
        trk_song *s = trk_song_of(trkEngine_);
        if (s->pattern[0].rows < 16) s->pattern[0].rows = 16;
        for (int i = 0; i < 4; i++) {
            s->pattern[0].cell[i * 4][track].note = uint8_t(60 + i * 4);
            s->pattern[0].cell[i * 4][track].vel = 100;
        }
        trk_unlock(trkEngine_);
        if (tracker_) tracker_->markClean();   // the figure is the drive's, not the user's
        trk_route_sink(trkEngine_, track, sinks_.first().id);
        if (trk_sink_of(trkEngine_, track) < 0) return false;
        routed_ = true;
        trk_play(trkEngine_, TRK_PLAY_SONG, 0, 0);
        return true;
    }

    /* Scripted exercise for a machine with no XTEST, in the idiom of
     * pestudio's --cycle: what clicking through the smoke test by hand cannot
     * prove twice. Every second the next tab is brought to the front (so the
     * keys-live arbitration runs), a note is played into every loaded synth
     * (so its peak meter has something to say), and each engine's callback
     * count and peak are printed. Three seconds before the end the first
     * synth tab is closed through the ordinary close path, and at `ms` the
     * application quits. With --route the note injection is skipped -- the
     * tracker is playing the routed tab by then, and its peak has to be the
     * tracker's alone to prove anything. */
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
                    /* The tracker: how its first two tracks are routed, as the
                     * destination boxes show it. */
                    auto *tw = qobject_cast<TrackerWidget *>(tabs_->widget(i));
                    fprintf(stderr, "smoke: tab %d \"%s\" (tracker)%s dest1=\"%s\" dest2=\"%s\"\n",
                            i, qPrintable(tabs_->tabText(i)),
                            trkEngine_ && trk_playing(trkEngine_) ? " playing" : "",
                            tw ? qPrintable(tw->dest(0)->currentText()) : "",
                            tw ? qPrintable(tw->dest(1)->currentText()) : "");
                    continue;
                }
                pehost *ph = h->engine()->host();
                if (ph && !routed_) {
                    pehost_note_off(ph, smokeNote_);
                    smokeNote_ = 60 + (smokeStep_ + i) % 12;
                    pehost_note_on(ph, smokeNote_, 100);
                }
                unsigned dropped = 0, spilled = 0;
                if (ph) pehost_midi_stats(ph, &dropped, &spilled);
                fprintf(stderr, "smoke: tab %d \"%s\" callbacks=%lu peak=%.3f "
                                "keysLive=%d dropped=%u spilled=%u injected=%lu placed=%lu "
                                "dead=%d restarts=%d%s\n",
                        i, qPrintable(tabs_->tabText(i)),
                        h->engine()->callbacks(), h->engine()->peak(),
                        int(h->keysLive()), dropped, spilled,
                        h->engine()->midiInjected(), h->engine()->midiPlaced(),
                        int(h->pluginDead()), ph ? pehost_restarts(ph) : 0,
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

    /* The menus of every tab share one menu bar, merged by title: a tab
     * asking for "File" gets the window's own File menu, "About" lands in
     * "Help", and only a title nobody has yet -- "Settings", "Samples" --
     * opens a new menu. What a tab added to a merged menu is remembered per
     * tab (see attributeMenus), and syncMenus shows exactly the current
     * tab's share, so there is one File menu whose contents follow the tab
     * in front -- and one live Ctrl+O, Ctrl+S and Ctrl+Q at a time, since a
     * background tab's actions are disabled as well as invisible. */
    QMenu *addMenu(const QString &title) override
    {
        const QString key = menuKey(title);
        const QList<QAction *> bar = menuBar()->actions();
        for (QAction *a : bar) {
            QMenu *m = a->menu();
            if (m && menuKey(m->title()) == key) {
                noteHandout(m);
                return m;
            }
        }
        /* A tab's own menu goes before Help, which stays last as a menu bar
         * has it: File, Samples, Help -- not File, Help, Samples. */
        QMenu *m = new QMenu(title, menuBar());
        menuBar()->insertMenu(helpMenu_ ? helpMenu_->menuAction() : nullptr, m);
        noteHandout(m);
        return m;
    }

    /* Titles compare without the accelerator marker, and the host's "About"
     * is the shell's "Help". */
    static QString menuKey(QString title)
    {
        title.remove('&');
        if (title == "About") title = QStringLiteral("Help");
        return title;
    }

    /* While a tab is being built, each menu handed out is remembered with
     * the action count it had at hand-out: everything past that index when
     * construction finishes is this tab's. Only the first hand-out per
     * build counts -- a menu a tab asks for twice is still one span. */
    void noteHandout(QMenu *m)
    {
        if (!building_) return;
        for (const auto &p : pendingStarts_)
            if (p.first == m) return;
        pendingStarts_.append({ m, m->actions().count() });
    }

    /* Called once a tab's constructor has run: everything a handed-out menu
     * gained past its snapshot is attributed to the tab. The tab's own Quit
     * is dropped on the way -- the window's File > Quit already closes the
     * shell, which is what both widgets' Quit did all along: the tracker's
     * goes through requestQuit, the host's closes its top-level window, and
     * both arrive at this window's closeEvent. */
    void attributeMenus(QWidget *tab)
    {
        QList<QPair<QMenu *, QAction *>> acts;
        for (const auto &p : pendingStarts_) {
            QMenu *m = p.first;
            /* A copy: the Quit drop below edits the menu being walked. */
            const QList<QAction *> all = m->actions();
            for (int i = p.second; i < all.count(); i++) {
                QAction *a = all.at(i);
                if (a->shortcut() == QKeySequence(QKeySequence::Quit) &&
                    QString(a->text()).remove('&') == "Quit") {
                    m->removeAction(a);
                    delete a;
                    continue;
                }
                acts.append({ m, a });
            }
        }
        pendingStarts_.clear();
        arrangeShare(acts, qobject_cast<TrackerWidget *>(tab) != nullptr);
        actionsOf_[tab] = acts;
    }

    /* A tab's items arrive at the end of the merged menus, which for File is
     * after Quit. They are moved to where they belong: a tab's File items go
     * after the window's New and Open and before the session group, its Help
     * items ahead of About, and the separator it left before Quit is
     * dropped. The
     * tracker's song commands are named for what they act on, because beside
     * "New synth" a bare "New" says nothing. */
    void arrangeShare(QList<QPair<QMenu *, QAction *>> &acts, bool tracker)
    {
        static const QHash<QString, QString> songNames = {
            { "New", "New song" }, { "Open…", "Open song…" }, { "Save", "Save song" },
            { "Save As…", "Save song as…" }, { "Export MIDI…", "Export song as MIDI…" },
        };
        /* Separators closing a tab's File items are the tab's own Quit gap. */
        for (int i = acts.count() - 1; i >= 0; i--) {
            QMenu *m = acts.at(i).first;
            QAction *a = acts.at(i).second;
            if (m != fileMenu_) continue;
            if (!a->isSeparator()) break;
            m->removeAction(a);
            delete a;
            acts.removeAt(i);
        }
        for (const auto &p : acts) {
            QMenu *m = p.first;
            QAction *a = p.second;
            if (m == fileMenu_) {
                if (tracker) {
                    const auto it = songNames.constFind(QString(a->text()).remove('&'));
                    if (it != songNames.constEnd()) a->setText(it.value());
                }
                m->insertAction(fileAnchor_, a);
            } else if (helpMenu_ && m == helpMenu_) {
                m->insertAction(helpAnchor_, a);
            }
        }
    }

    /* The menu bar follows the tab in front: each tab's actions show only
     * while it is current, and are disabled besides, so a background tab's
     * shortcuts answer nothing. A merged menu with nobody showing leaves
     * the bar until some tab has something visible in it again; the shell's
     * own File and Help stay always. Tab moves need nothing here --
     * visibility is keyed on the widget, not the index. */
    void syncMenus()
    {
        QWidget *cur = tabs_->currentWidget();
        /* The tracker in front has its own Open song; one is enough. */
        if (openSongAct_) openSongAct_->setVisible(!(tracker_ && cur == tracker_));
        for (auto it = actionsOf_.constBegin(); it != actionsOf_.constEnd(); ++it) {
            const bool on = it.key() == cur;
            for (const auto &p : it.value()) {
                p.second->setVisible(on);
                p.second->setEnabled(on);
            }
        }
        const QList<QAction *> bar = menuBar()->actions();
        for (QAction *a : bar) {
            QMenu *m = a->menu();
            if (!m || shellMenus_.contains(m)) continue;
            bool any = false;
            const QList<QAction *> acts = m->actions();
            for (QAction *ma : acts)
                if (ma->isVisible()) { any = true; break; }
            a->setVisible(any);
        }
    }

    /* -- TrackerHost: the synth tabs as in-process tracker destinations ---- */

    /* Every synth tab is a destination, named by its plug-in. The tracker
     * offers them after the ALSA windows; picking one routes that track to
     * the tab directly, no trip through the sequencer. */
    QStringList midiSinks() override
    {
        QStringList names;
        for (const SinkEntry &s : sinks_) names << s.name;
        return names;
    }
    void midiSinkPicked(int track, const QString &name) override
    {
        if (!trkEngine_) return;
        if (name.isEmpty()) { trk_route_sink(trkEngine_, track, -1); return; }
        for (const SinkEntry &s : sinks_)
            if (s.name == name) { trk_route_sink(trkEngine_, track, s.id); return; }
    }
    /* A song was loaded -- the shell's Open, the tracker tab's own, or the
     * command line -- so the synths its sink lines name come back too. */
    void songOpened() override
    {
        reopenSongSynths();
        restoreSongSounds();
        if (tracker_ && !tracker_->songPath().isEmpty())     /* the tracker's own Open too */
            recent_add("song", QFileInfo(tracker_->songPath()).absoluteFilePath().toLocal8Bit().constData());
    }
    void songSaved(const QString &path) override
    {
        saveSongSounds(path);
        recent_add("song", QFileInfo(path).absoluteFilePath().toLocal8Bit().constData());
    }

    /* The sounds a song's synths were on. The song file is plain text for the
     * tracks and cells; what each synth was playing is a patch, and a patch is
     * a whole plug-in's parameters. So it goes beside the song, in
     * <song>.sounds, in the session file's own format: one synth entry (plug-in
     * and patch) for each synth tab a track plays, and a route naming the
     * track and the synth's sink name. Opening the song puts them back. */
    static QString soundsPath(const QString &song) { return song + ".sounds"; }

    void saveSongSounds(const QString &song)
    {
        if (!trkEngine_) return;
        sess_file s;
        memset(&s, 0, sizeof s);
        std::vector<char *> pool;
        auto keep = [&pool](char *p) { pool.push_back(p); return p; };
        QList<HostWidget *> recorded;
        for (int t = 0; t < TRK_TRACKS; t++) {
            const int id = trk_sink_of(trkEngine_, t);
            if (id < 0) continue;
            HostWidget *tab = nullptr;
            QString sinkName;
            for (const SinkEntry &e : sinks_)
                if (e.id == id) { tab = e.tab; sinkName = e.name; break; }
            if (!tab || tab->loadedPath().isEmpty()) continue;
            int at = int(recorded.indexOf(tab));
            if (at < 0) {
                auto *sy = static_cast<sess_synth *>(
                    realloc(s.synths, sizeof(sess_synth) * size_t(s.nsynths + 1)));
                if (!sy) continue;
                s.synths = sy;
                sy = &s.synths[s.nsynths];
                sy->plugin = keep(strdup(QFileInfo(tab->loadedPath()).absoluteFilePath()
                                             .toLocal8Bit().constData()));
                sy->patch = nullptr;
                if (tab->engine()->host()) {
                    char perr[128];
                    sy->patch = patch_capture(tab->engine()->host(), sy->plugin, perr, sizeof perr);
                }
                recorded << tab;
                at = s.nsynths++;
            }
            auto *r = static_cast<sess_route *>(
                realloc(s.routes, sizeof(sess_route) * size_t(s.nroutes + 1)));
            if (!r) continue;
            s.routes = r;
            r = &s.routes[s.nroutes++];
            r->track = t;
            r->synth = at;
            r->sink = keep(strdup(sinkName.toLocal8Bit().constData()));
        }
        s.song = keep(strdup(QFileInfo(song).absoluteFilePath().toLocal8Bit().constData()));
        char err[512];
        const QByteArray out = soundsPath(song).toLocal8Bit();
        if (!s.nsynths) {
            QFile::remove(soundsPath(song));          /* nothing to remember: no stale file */
        } else if (sess_write(out.constData(), &s, err, sizeof err)) {
            statusBar()->showMessage(QString("song saved, but not the synths' sounds: %1")
                                         .arg(QString::fromLocal8Bit(err)), 0);
        }
        for (char *p : pool) free(p);
        for (int i = 0; i < s.nsynths; i++) free(s.synths[i].patch);
        free(s.synths);
        free(s.routes);
    }

    /* The other half: each synth the song's routes name gets its saved
     * patch. Matched by sink name among the tabs that exist now, which
     * reopenSongSynths has just made. */
    void restoreSongSounds()
    {
        if (!tracker_ || tracker_->songPath().isEmpty()) return;
        char err[512];
        sess_file *s = sess_read(soundsPath(tracker_->songPath()).toLocal8Bit().constData(),
                                 err, sizeof err);
        if (!s) return;                      /* no sounds saved with it */
        QStringList trouble;
        QSet<int> done;
        for (int i = 0; i < s->nroutes; i++) {
            const sess_route &r = s->routes[i];
            if (r.synth < 0 || r.synth >= s->nsynths || done.contains(r.synth)) continue;
            const char *patch = s->synths[r.synth].patch;
            if (!patch) continue;
            const QString sink = QString::fromUtf8(r.sink ? r.sink : "");
            for (const SinkEntry &e : sinks_) {
                if (e.name != sink) continue;
                /* A tab of that name that now holds a different plug-in would be
                 * handed another instrument's parameters: say so, and leave it. */
                const char *want = s->synths[r.synth].plugin;
                if (want && *want &&
                    QFileInfo(QString::fromLocal8Bit(want)).fileName().compare(
                        QFileInfo(e.tab->loadedPath()).fileName(), Qt::CaseInsensitive) != 0) {
                    trouble << QString("%1 (a different plug-in)").arg(sink);
                    done.insert(r.synth);
                    break;
                }
                QString why;
                if (!e.tab->applyPatchText(patch, &why))
                    trouble << QString("%1 (%2)").arg(sink, why);
                done.insert(r.synth);
                break;
            }
        }
        sess_free(s);
        if (!trouble.isEmpty())
            statusMessage("song's sounds not restored: " + trouble.join(", "), 0);
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        /* The tracker's song may have unsaved changes; closing the window asks
         * exactly as closing its tab does. */
        if (tracker_ && !tracker_->confirmClose()) { e->ignore(); return; }
        if (MixBus::instance().active()) stopMixRecord(false);      /* the take is finished, not lost */
        /* --save-session writes here, on the clean exit: the song question
         * above has been answered by now, so what gets written is what the
         * answer left behind. */
        if (!saveOnExit_.isEmpty()) writeSession(saveOnExit_, false);
        /* The tabs are deleted with the window, not through closeTab, so
         * their tracker destinations are unregistered here: after the last
         * trk_remove_sink returns the delivery thread is provably out of
         * every tab's engine, whatever order the widgets die in. The engine
         * itself is left to the process exit, as before -- with no sinks
         * left, its threads touch nothing of the tabs'. */
        while (!sinks_.isEmpty()) removeSink(sinks_.first().tab);
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
         * own them -- collected under building_ and attributed after. */
        building_ = true;
        auto *h = new HostWidget(this);
        building_ = false;
        attributeMenus(h);
        /* Whether this tab answers the computer keyboard is read at each key
         * from where the tab bar is now, not from a flag set on tab changes:
         * see HostWidget::setFrontCheck. */
        h->setFrontCheck([this, h] { return tabs_ && tabs_->currentWidget() == h; });

        const int ix = tabs_->addTab(h, title.isEmpty() ? QString("synth") : title);
        /* Which plug-in is loaded, on the tab. HostWidget sets its widget
         * title to the plug-in's name on every load -- including the one it
         * makes during construction, before the connect below, so the title
         * is taken verbatim as well. */
        if (!h->windowTitle().isEmpty()) tabs_->setTabText(ix, h->windowTitle());
        connect(h, &QWidget::windowTitleChanged, this, [this, h](const QString &t) {
            const int i = tabs_->indexOf(h);
            if (i >= 0) tabs_->setTabText(i, t.isEmpty() ? QString("synth") : t);
            renameSink(h);           // the tracker's destination list follows
        });
        ensureSink(h);               // a destination the tracker can play directly
        tabs_->setCurrentIndex(ix);
        updateCanvas();
        syncMenus();        /* currentChanged can fire before the attribution */
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
        trackerMixId_ = MixBus::instance().registerSource();
        trk_set_tap(trkEngine_, &trackerTap, reinterpret_cast<void *>(intptr_t(trackerMixId_)));
        /* The engine did not exist when earlier synth tabs opened; register
         * their destinations now. */
        for (int i = 0; i < tabs_->count(); i++)
            if (auto *h = qobject_cast<HostWidget *>(tabs_->widget(i))) ensureSink(h);
        building_ = true;
        auto *t = new TrackerWidget(trkEngine_, this);
        building_ = false;
        attributeMenus(t);
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
        syncMenus();        /* currentChanged can fire before the attribution */
        return t;
    }

    void newSynth() { addSynthTab(); }
    /* File > Plug-in folders: the dialog of the synth tab in front or, failing
     * that, any other -- and with none open, a new one, which is where its
     * rescans land. */
    /* The tab holding this plug-in, or none. */
    HostWidget *hostHolding(const QString &path)
    {
        for (int i = 0; i < tabs_->count(); i++) {
            auto *h = qobject_cast<HostWidget *>(tabs_->widget(i));
            if (h && h->loadedPluginPath() == path) return h;
        }
        return nullptr;
    }

    HostWidget *anyHost(bool make)
    {
        HostWidget *h = qobject_cast<HostWidget *>(tabs_->currentWidget());
        for (int i = 0; !h && i < tabs_->count(); i++) h = qobject_cast<HostWidget *>(tabs_->widget(i));
        return h ? h : (make ? addSynthTab() : nullptr);
    }

    /* File > Plug-ins: every plug-in the scan found, in one list -- loaded into
     * a new tab, unloaded (the tab that holds it is closed), or taken off the
     * list. Taking one off does not delete its file: the path goes in the
     * hidden list and the scans skip it, until it is put back. */
    void pluginManager()
    {
        if (pluginMgr_) { pluginMgr_->raise(); pluginMgr_->activateWindow(); return; }
        if (!anyHost(true)) return;               // the scan lives in a synth tab
        auto *d = new QDialog(this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle("Plug-ins");
        d->resize(640, 520);
        auto *v = new QVBoxLayout(d);
        auto *search = new QLineEdit;
        search->setPlaceholderText("Search: a letter, then a keyword");
        search->setClearButtonEnabled(true);
        auto *list = new QListWidget;
        auto *note = new QLabel;
        note->setEnabled(false);
        auto *row = new QHBoxLayout;
        auto *load = new QPushButton("Load");
        auto *unload = new QPushButton("Unload");
        auto *remove = new QPushButton("Remove from list");
        unload->setToolTip("Closes the tab that has this plug-in open");
        remove->setToolTip("Takes it off the list. The file stays where it is; "
                           "Show removed lets you put it back");
        auto *showRemoved = new QCheckBox("Show removed");
        auto *add = new QPushButton("Add plug-in...");
        auto *addBundle = new QPushButton("Add bundle...");
        add->setToolTip("Pick a plug-in file: use it for this session only, or install it "
                        "into the folder for its kind (Windows, Linux, macOS...)");
        addBundle->setToolTip("The same for a .vst3 / .vst / .component bundle folder");
        auto *rescan = new QPushButton("Rescan");
        auto *close = new QPushButton("Close");
        for (QWidget *w : std::initializer_list<QWidget *>{ load, unload, remove, showRemoved, add, addBundle })
            row->addWidget(w);
        row->addStretch(1);
        row->addWidget(rescan);
        row->addWidget(close);
        v->addWidget(search);
        v->addWidget(list, 1);
        v->addWidget(note);
        v->addLayout(row);

        auto selected = [list]() -> QString {
            QListWidgetItem *it = list->currentItem();
            return it ? it->data(Qt::UserRole).toString() : QString();
        };
        auto sync = [=] {
            const QString p = selected();
            load->setEnabled(!p.isEmpty() && !showRemoved->isChecked());
            unload->setEnabled(!p.isEmpty() && !showRemoved->isChecked() && hostHolding(p));
            remove->setEnabled(!p.isEmpty());
            remove->setText(showRemoved->isChecked() ? "Put back" : "Remove from list");
        };
        auto refill = [=] {
            const QString needle = search->text();
            list->clear();
            int shown = 0, total = 0;
            if (showRemoved->isChecked()) {
                std::unique_ptr<char[][VSTDIRS_PATHLEN]> hid(new char[512][VSTDIRS_PATHLEN]);
                const int n = vstdirs_hidden_list(hid.get(), 512);
                for (int i = 0; i < n; i++) {
                    const QString p = QString::fromLocal8Bit(hid[i]);
                    total++;
                    if (!needle.isEmpty() && !p.contains(needle, Qt::CaseInsensitive)) continue;
                    auto *it = new QListWidgetItem(QFileInfo(p).fileName() + "   (" + p + ")");
                    it->setData(Qt::UserRole, p);
                    list->addItem(it);
                    shown++;
                }
                note->setText(QString("%1 of %2 plug-ins taken off the list").arg(shown).arg(total));
            } else if (HostWidget *h = anyHost(false)) {
                for (const HostWidget::PluginRef &e : h->availablePlugins()) {
                    total++;
                    // One letter: names that start with it; more: a keyword.
                    if (needle.size() == 1 ? !e.label.startsWith(needle, Qt::CaseInsensitive)
                                           : (!needle.isEmpty() && !e.label.contains(needle, Qt::CaseInsensitive) &&
                                              !e.path.contains(needle, Qt::CaseInsensitive))) continue;
                    const bool loaded = hostHolding(e.path) != nullptr;
                    auto *it = new QListWidgetItem((loaded ? "● " : "") + e.label + (loaded ? "   loaded" : ""));
                    it->setData(Qt::UserRole, e.path);
                    it->setToolTip(e.path);
                    if (!e.loadable) it->setForeground(Qt::gray);
                    if (loaded) { QFont f = it->font(); f.setBold(true); it->setFont(f); }
                    list->addItem(it);
                    shown++;
                }
                note->setText(QString("%1 of %2 plug-ins available").arg(shown).arg(total));
            } else {
                note->setText("no list yet -- open a synth tab to scan");
            }
            sync();
        };
        auto rescanAll = [this] {
            for (int i = 0; i < tabs_->count(); i++)
                if (auto *h = qobject_cast<HostWidget *>(tabs_->widget(i))) h->rescanPlugins();
        };
        connect(search, &QLineEdit::textChanged, d, refill);
        connect(showRemoved, &QCheckBox::toggled, d, refill);
        connect(list, &QListWidget::currentRowChanged, d, sync);
        connect(rescan, &QPushButton::clicked, d, [=] { rescanAll(); refill(); });
        auto addOne = [=](bool bundle) {
            HostWidget *h = anyHost(false);
            if (!h || !h->addPluginInteractive(d, bundle)) return;
            rescanAll();                      // every tab's list shows it
            refill();
        };
        connect(add, &QPushButton::clicked, d, [=] { addOne(false); });
        connect(addBundle, &QPushButton::clicked, d, [=] { addOne(true); });
        connect(close, &QPushButton::clicked, d, &QDialog::close);
        connect(load, &QPushButton::clicked, d, [=] {
            const QString p = selected();
            if (p.isEmpty()) return;
            if (HostWidget *held = hostHolding(p)) { tabs_->setCurrentWidget(held); return; }
            HostWidget *h = addSynthTab();
            if (!h->loadPlugin(p)) {
                closeTabNow(tabs_->indexOf(h));
                statusBar()->showMessage("that plug-in could not be loaded", 5000);
            }
            refill();
        });
        connect(unload, &QPushButton::clicked, d, [=] {
            if (HostWidget *held = hostHolding(selected())) closeTabNow(tabs_->indexOf(held));
            refill();
        });
        connect(remove, &QPushButton::clicked, d, [=] {
            const QByteArray p = selected().toLocal8Bit();
            if (p.isEmpty()) return;
            if (showRemoved->isChecked()) vstdirs_unhide(p.constData());
            else vstdirs_hide(p.constData());
            rescanAll();
            refill();
        });
        pluginMgr_ = d;
        refill();
        d->show();
    }

    /* File > Audio output: PipeWire, JACK or ALSA for every synth tab. The
     * choice is kept for the next run; a tab whose backend will not open falls
     * back to PipeWire and says so in its own status line. */
    void audioSettings()
    {
        ao_choice cur;
        ao_choice_load(&cur);
        QDialog dlg(this);
        dlg.setWindowTitle("Audio output");
        auto *lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel("Where the synths play. Changes apply at once."));
        auto *autoB = new QRadioButton("Automatic (PipeWire)");
        auto *pwB   = new QRadioButton("PipeWire");
        auto *jackB = new QRadioButton("JACK");
        auto *alsaB = new QRadioButton("ALSA");
        char why[256] = "";
        const bool jackOk = ao_jack_available(why, sizeof why) != 0;
        if (!jackOk) { jackB->setToolTip(why); jackB->setText(QString("JACK (%1)").arg(why)); }
        for (auto *b : {autoB, pwB, jackB, alsaB}) lay->addWidget(b);
        auto *dev = new QComboBox;
        dev->addItem("System default", "");
        static char names[32][128], labels[32][96];
        const int nd = ao_alsa_devices(names, labels, 32);
        for (int i = 0; i < nd; i++) dev->addItem(labels[i], QString::fromUtf8(names[i]));
        int di = dev->findData(QString::fromUtf8(cur.device));
        if (di < 0 && cur.device[0]) { dev->addItem(cur.device, QString::fromUtf8(cur.device)); di = dev->count() - 1; }
        dev->setCurrentIndex(di < 0 ? 0 : di);
        lay->addWidget(dev);
        auto sync = [=] { dev->setEnabled(alsaB->isChecked()); };
        connect(alsaB, &QRadioButton::toggled, &dlg, sync);
        (cur.backend == AO_PIPEWIRE ? pwB : cur.backend == AO_JACK ? jackB
            : cur.backend == AO_ALSA ? alsaB : autoB)->setChecked(true);
        sync();
        auto *result = new QLabel;
        result->setWordWrap(true);
        lay->addWidget(result);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
        lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
        connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dlg, [&] {
            ao_choice c{};
            c.backend = pwB->isChecked() ? AO_PIPEWIRE : jackB->isChecked() ? AO_JACK
                      : alsaB->isChecked() ? AO_ALSA : AO_AUTO;
            snprintf(c.device, sizeof c.device, "%s",
                     c.backend == AO_ALSA ? dev->currentData().toString().toUtf8().constData() : "");
            ao_choice_save(&c);
            QStringList out;
            for (int i = 0; i < tabs_->count(); i++)
                if (auto *h = qobject_cast<HostWidget *>(tabs_->widget(i))) {
                    QString m;
                    h->switchAudio(c, &m);
                    out << QString("%1: %2").arg(tabs_->tabText(i), m);
                }
            result->setText(out.isEmpty() ? "Saved; the next synth will use it." : out.join("\n"));
        });
        dlg.exec();
    }

    void pluginFolders()
    {
        HostWidget *h = qobject_cast<HostWidget *>(tabs_->currentWidget());
        for (int i = 0; !h && i < tabs_->count(); i++) h = qobject_cast<HostWidget *>(tabs_->widget(i));
        if (!h) h = addSynthTab();
        if (h) h->showPluginFolders();
    }
    void newTracker() { addTrackerTab(); }
    void openSong()
    {
        const QString p = QFileDialog::getOpenFileName(this, "Open song", QString(),
                                                       "Tracker songs (*.trk);;All files (*)");
        if (!p.isEmpty()) openSongPath(p);
    }

    /* Open song, part two: the synths the song's tracks play.
     *
     * A track routed to a synth tab saves the tab's sink name ("track N sink
     * <name>"), and loading the song brings the name back -- but not the tab,
     * so the name said where the track used to play, not where it plays now.
     * This puts the tabs back: every distinct sink name that is not an open
     * tab already is resolved to a plug-in and loaded in a tab of its own,
     * and every track that named a sink is routed to it. A name nothing
     * answers to leaves its tracks on their ALSA windows, exactly where a
     * song load always left them. */
    void reopenSongSynths()
    {
        if (!trkEngine_) return;
        pluginScan_.clear();             /* the folders may have moved since */

        /* The tracks naming a sink, and the names, once each in first-seen
         * order. trk_route_sink takes the engine's lock itself, so everything
         * the song has to say is collected under the lock here and the
         * routing happens below, outside it. */
        QString trackSink[TRK_TRACKS];
        QStringList names;
        trk_lock(trkEngine_);
        const trk_song *song = trk_song_of(trkEngine_);
        for (int t = 0; t < TRK_TRACKS; t++) {
            trackSink[t] = QString::fromUtf8(song->track[t].sink);
            if (!trackSink[t].isEmpty() && !names.contains(trackSink[t]))
                names << trackSink[t];
        }
        trk_unlock(trkEngine_);
        if (names.isEmpty()) return;

        /* Each name to the sink it means: one already open wins by name,
         * otherwise a new tab is made for it. */
        QHash<QString, int> sinkFor;
        QStringList missing;
        int opened = 0;
        for (const QString &full : names) {
            int id = -1;
            for (const SinkEntry &e : sinks_)
                if (e.name == full) { id = e.id; break; }
            if (id < 0) {
                /* A second tab of one plug-in is uniquified as "name 2", so
                 * the plug-in is the name with a trailing " N" off. */
                QString base = full;
                const int sp = base.lastIndexOf(' ');
                if (sp > 0) {
                    bool digits = true;
                    for (int i = sp + 1; i < base.size(); i++)
                        if (!base[i].isDigit()) { digits = false; break; }
                    if (digits) base.truncate(sp);
                }
                const QString path = resolvePlugin(base);
                if (path.isEmpty()) { missing << full; continue; }
                HostWidget *h = addSynthTab();
                if (!h->loadPlugin(path)) {
                    /* No empty tab left standing for it -- the session
                     * restore reasons the same way. */
                    closeTabNow(tabs_->indexOf(h));
                    statusMessage("could not load " + path, 0);
                    fprintf(stderr, "song: %s's plug-in %s would not load\n",
                            qPrintable(full), qPrintable(path));
                    fflush(stderr);
                    missing << full;
                    continue;
                }
                /* The tab registered its sink under the plug-in's own name,
                 * uniquified had the name been taken: this tab's entry whose
                 * name is the full name, the base, or a spelling of it. */
                for (const SinkEntry &e : sinks_) {
                    if (e.tab != h) continue;
                    if (e.name == full || e.name == base ||
                        e.name.startsWith(base)) { id = e.id; break; }
                }
                if (id >= 0) opened++;
            }
            if (id < 0) { missing << full; continue; }
            sinkFor.insert(full, id);
        }

        /* Now that the tabs exist: route. trk_route_sink also refreshes the
         * track's saved name mirror, so the name the song carries is the live
         * sink's from here on. */
        for (int t = 0; t < TRK_TRACKS; t++) {
            if (trackSink[t].isEmpty()) continue;
            const auto it = sinkFor.constFind(trackSink[t]);
            if (it != sinkFor.constEnd()) trk_route_sink(trkEngine_, t, it.value());
        }

        statusMessage(QString("reopened %1 synth(s) from the song%2")
                          .arg(opened)
                          .arg(missing.isEmpty() ? QString()
                                                 : " -- not found: " + missing.join(", ")),
                      0);
    }

    /* -- the session file ------------------------------------------------- */

    void openSession()
    {
        const QString p = QFileDialog::getOpenFileName(
            this, "Open session", QString(),
            "vst-ace sessions (*.vstace);;All files (*)");
        if (!p.isEmpty()) openSessionPath(p);
    }
    void saveSession()
    {
        if (sessionPath_.isEmpty()) { saveSessionAs(); return; }
        writeSession(sessionPath_);
    }
    void saveSessionAs()
    {
        QString p = QFileDialog::getSaveFileName(
            this, "Save session", sessionPath_.isEmpty() ? "session.vstace" : sessionPath_,
            "vst-ace sessions (*.vstace);;All files (*)");
        if (p.isEmpty()) return;
        if (QFileInfo(p).suffix().isEmpty()) p += ".vstace";
        writeSession(p);
    }

    /* The whole session as one JSON file: which synth tabs with which
     * plug-ins and their sounds, the song, which tracks play which tab. A
     * dirty song is the song's own question and is asked through the
     * tracker's ordinary save flow first -- Save and Discard both let the
     * write go ahead (the file records the song's path either way), Cancel
     * stops it. */
    bool writeSession(const QString &path, bool askSong = true)
    {
        /* askSong false: the caller has already put the song question --
         * closeEvent, through confirmClose -- and a Discard there must not be
         * asked again here. */
        if (askSong && tracker_ && tracker_->isDirty()) {
            const auto b = QMessageBox::question(
                this, "studio", "Save changes to the song first?",
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            if (b == QMessageBox::Cancel) return false;
            if (b == QMessageBox::Save && !tracker_->saveSong()) return false;
        }

        sess_file s;
        memset(&s, 0, sizeof s);
        std::vector<char *> pool;            /* everything strdup'd, freed below */
        auto keep = [&pool](char *p) { pool.push_back(p); return p; };
        QList<HostWidget *> recorded;        /* s.synths[i] came from recorded[i] */
        for (int i = 0; i < tabs_->count(); i++) {
            auto *h = qobject_cast<HostWidget *>(tabs_->widget(i));
            if (!h || h->loadedPath().isEmpty()) continue;
            auto *sy = static_cast<sess_synth *>(
                realloc(s.synths, sizeof(sess_synth) * size_t(s.nsynths + 1)));
            if (!sy) continue;
            s.synths = sy;
            sy = &s.synths[s.nsynths++];
            recorded << h;
            /* Absolute, whatever --synth was given as: the file has to mean
             * the same thing from any working directory. */
            sy->plugin = keep(strdup(QFileInfo(h->loadedPath()).absoluteFilePath()
                                         .toLocal8Bit().constData()));
            sy->patch = nullptr;
            if (h->engine()->host()) {
                char perr[128];
                sy->patch = patch_capture(h->engine()->host(), sy->plugin,
                                          perr, sizeof perr);
            }
        }
        const QString song = tracker_ && !tracker_->songPath().isEmpty()
                           ? QFileInfo(tracker_->songPath()).absoluteFilePath() : QString();
        s.song = keep(strdup(song.toLocal8Bit().constData()));
        if (trkEngine_) {
            for (int t = 0; t < TRK_TRACKS; t++) {
                char nm[TRK_DEST_LEN] = "";
                const int id = trk_sink_of(trkEngine_, t);
                if (id < 0) continue;
                trk_sink_name(trkEngine_, id, nm, sizeof nm);
                auto *r = static_cast<sess_route *>(
                    realloc(s.routes, sizeof(sess_route) * size_t(s.nroutes + 1)));
                if (!r) continue;
                s.routes = r;
                r = &s.routes[s.nroutes++];
                r->track = t;
                r->synth = -1;
                for (const SinkEntry &e : sinks_)
                    if (e.id == id) { r->synth = int(recorded.indexOf(e.tab)); break; }
                r->sink = keep(strdup(nm));
            }
        }

        char err[512];
        const int rc = sess_write(path.toLocal8Bit().constData(), &s, err, sizeof err);
        for (char *p : pool) free(p);
        for (int i = 0; i < s.nsynths; i++) free(s.synths[i].patch);
        free(s.synths);
        free(s.routes);
        if (rc) {
            statusBar()->showMessage(QString("could not save the session: %1")
                                         .arg(QString::fromLocal8Bit(err)), 0);
            return false;
        }
        sessionPath_ = path;
        recent_add("session", QFileInfo(path).absoluteFilePath().toLocal8Bit().constData());
        statusBar()->showMessage("session saved to " + path, 5000);
        fprintf(stderr, "session: saved %s\n", qPrintable(path));
        fflush(stderr);
        return true;
    }

    /* File > Record studio mix. */
    void toggleMixRecord()
    {
        MixBus &mb = MixBus::instance();
        if (mb.active()) {
            stopMixRecord(true);
            return;
        }
        QDir().mkpath("renders");
        beginMixRecord(QString("renders/mix-%1.wav")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
    }

public:
    /* --record-mix <file>: the take starts as the window comes up, and is
     * finished, with no question asked, when it closes. */
    bool beginMixRecord(const QString &file)
    {
        if (!MixBus::instance().start(file, kSampleRate)) {
            statusBar()->showMessage("could not open " + file + " for writing", 0);
            return false;
        }
        mixAct_->setText("Stop recording studio &mix");
        mixTimer_->start(200);
        statusBar()->showMessage("recording the studio mix to " + file, 0);
        fprintf(stderr, "studio: recording the mix to %s\n", qPrintable(file));
        fflush(stderr);
        return true;
    }
    bool playSong()
    {
        if (!trkEngine_) return false;
        trk_play(trkEngine_, TRK_PLAY_SONG, 0, 0);
        return true;
    }

private:

    void tickMixRecord()
    {
        MixBus &mb = MixBus::instance();
        if (!mb.active()) { mixLabel_->clear(); return; }
        const double secs = double(mb.frames()) / kSampleRate;
        mixLabel_->setText(QString("\u25cf mix  %1:%2.%3")
            .arg(int(secs) / 60, 2, 10, QChar('0'))
            .arg(int(secs) % 60, 2, 10, QChar('0'))
            .arg(int(secs * 10) % 10));
    }

    /* Ends the take. `ask`: offer to move it, as a tab's recorder does -- not on
     * quit, where the file is simply left in renders/. */
    void stopMixRecord(bool ask)
    {
        MixBus &mb = MixBus::instance();
        const uint64_t dropped = mb.dropped();
        const QString file = mb.stop();
        if (file.isEmpty()) return;
        const double secs = double(MixBus::instance().frames()) / kSampleRate;
        mixTimer_->stop();
        mixLabel_->clear();
        mixAct_->setText("Record studio &mix");
        QString finalPath = file;
        if (ask) {
            QString target = QFileDialog::getSaveFileName(
                this, "Save the studio mix as",
                QDir(mixDir_.isEmpty() ? QFileInfo(file).absolutePath() : mixDir_)
                    .filePath(QFileInfo(file).fileName()),
                "WAV audio (*.wav);;All files (*)");
            if (!target.isEmpty() &&
                QFileInfo(target).absoluteFilePath() != QFileInfo(file).absoluteFilePath()) {
                if (!target.endsWith(".wav", Qt::CaseInsensitive)) target += ".wav";
                QFile::remove(target);
                if (QFile::rename(file, target) || (QFile::copy(file, target) && QFile::remove(file))) {
                    finalPath = target;
                    mixDir_ = QFileInfo(target).absolutePath();
                } else {
                    statusBar()->showMessage("could not write " + target + " -- left it in " + file, 0);
                }
            }
        }
        QString msg = QString("wrote %1 -- %2.%3 s").arg(finalPath).arg(int(secs)).arg(int(secs * 10) % 10);
        if (dropped) msg += QString("  (%1 frames dropped -- the disk could not keep up)").arg(dropped);
        if (mb.failed()) msg += "  (a write failed -- the disk is full? the file is incomplete)";
        statusBar()->showMessage(msg, 0);
        fprintf(stderr, "studio: %s\n", qPrintable(msg));
        fflush(stderr);
    }

    /* File > Reload plug-in: the way back for a tab whose plug-in stopped
     * and would not restart, which is also what the action's tooltip says.
     * Reloading is the ordinary load path, so a tab that was never dead is
     * only reloaded. */
    void reloadPlugin()
    {
        auto *h = qobject_cast<HostWidget *>(tabs_->currentWidget());
        if (!h || h->loadedPath().isEmpty()) return;
        const QString path = h->loadedPath();     /* the load rewrites it */
        /* The sound comes along: captured first -- from a dead helper that is
         * the bridge's record of what was last set, which is what a restart
         * would have put back -- and applied to the fresh instance. */
        char perr[128];
        char *sound = h->engine()->host()
                    ? patch_capture(h->engine()->host(), path.toLocal8Bit().constData(),
                                    perr, sizeof perr)
                    : nullptr;
        statusBar()->showMessage("reloading " + path + " ...", 3000);
        if (!h->loadPlugin(path)) {
            statusBar()->showMessage("could not reload " + path, 0);
        } else if (sound) {
            QString why;
            if (!h->applyPatchText(sound, &why))
                statusBar()->showMessage("reloaded " + path + ", but not its sound: " + why, 0);
        }
        free(sound);
    }

    /* The once-a-second tab bookkeeping. A tab whose plug-in died for good
     * says so on its face until it is reloaded; every tab's tooltip says what
     * its audio is doing, from the engine's callback counter against the
     * ~187.5 blocks a second a 256-frame quantum at 48 kHz should be making. */
    void watchTabs()
    {
        for (int i = 0; i < tabs_->count(); i++) {
            auto *h = qobject_cast<HostWidget *>(tabs_->widget(i));
            if (!h) continue;
            const bool dead = h->pluginDead();
            const bool marked = deadTabs_.contains(h);
            if (dead && !marked) {
                deadTabs_.insert(h);
                tabs_->setTabText(i, h->windowTitle() + " (stopped)");
                statusBar()->showMessage(
                    QString("the plug-in in tab \"%1\" stopped responding and "
                            "would not restart -- File > Reload plug-in tries again")
                        .arg(h->windowTitle()), 0);
            } else if (!dead && marked) {
                deadTabs_.remove(h);
                tabs_->setTabText(i, h->windowTitle().isEmpty()
                                         ? QString("synth") : h->windowTitle());
            }
            const unsigned long calls = h->engine()->callbacks();
            const unsigned long rate = calls - lastCalls_.value(h, calls);
            lastCalls_[h] = calls;
            tabs_->setTabToolTip(i, QString("audio: %1 blocks/s (%2%)%3")
                                        .arg(rate)
                                        .arg(int(rate * 100.0 / 187.5 + 0.5))
                                        .arg(dead ? QString(" -- plug-in stopped")
                                                  : QString()));
        }
        reloadAct_->setEnabled(qobject_cast<HostWidget *>(tabs_->currentWidget()) &&
                               !static_cast<HostWidget *>(tabs_->currentWidget())
                                    ->loadedPath().isEmpty());
    }

    void closeTab(int ix)
    {
        QWidget *w = tabs_->widget(ix);
        if (!w) return;
        if (w == tracker_ && !tracker_->confirmClose()) return;
        closeTabNow(ix);
    }

    /* Every tab, for a session opened in place of this one. The song is
     * asked about once, up front -- false on Cancel, with nothing closed --
     * and the tabs then go without asking again, since after a Discard the
     * song is still dirty and would be asked about a second time. */
    bool closeAllTabs()
    {
        if (tracker_ && !tracker_->confirmClose()) return false;
        while (tabs_->count()) closeTabNow(tabs_->count() - 1);
        return true;
    }

    /* The close itself, any question already answered. */
    void closeTabNow(int ix)
    {
        QWidget *w = tabs_->widget(ix);
        if (!w) return;

        /* Actions first, while the widget still exists: they are parented to
         * the menus, not the widget, so nothing else takes them off the bar
         * -- and after the widget is gone a menu would be holding actions
         * whose slots were the dead widget's. */
        for (const auto &p : actionsOf_.take(w)) {
            p.first->removeAction(p.second);
            if (QMenu *sub = p.second->menu()) delete sub;   /* its Inputs submenus */
            delete p.second;
        }
        tabs_->removeTab(ix);
        if (w == tracker_) {
            delete w;                    /* done with the engine: playback is stopped */
            tracker_ = nullptr;
            trk_set_tap(trkEngine_, nullptr, nullptr);     /* returns once no call is in flight */
            MixBus::instance().releaseSource(trackerMixId_);
            trackerMixId_ = -1;
            trk_close(trkEngine_);
            trkEngine_ = nullptr;
            sinks_.clear();            /* the destinations died with the engine */
            newTracker_->setEnabled(true);
        } else {
            /* Unregister first: the tracker's delivery thread may be calling
             * into the tab's engine right now, and trk_remove_sink waits that
             * out -- after it returns, nothing touches the engine again, and
             * tracks routed here fall back to their ALSA windows. */
            removeSink(static_cast<HostWidget *>(w));
            deadTabs_.remove(static_cast<HostWidget *>(w));
            lastCalls_.remove(static_cast<HostWidget *>(w));
            /* ~HostWidget lets go of the plug-in cleanly: the MIDI reader is
             * stopped and the editors detached in its body, then the Engine's
             * own teardown stops the PipeWire loop before pehost_close. */
            delete w;
        }
        /* A merged menu the close emptied leaves the bar with its tab. */
        const QList<QAction *> bar = menuBar()->actions();
        for (QAction *a : bar) {
            QMenu *m = a->menu();
            if (m && !shellMenus_.contains(m) && m->actions().isEmpty()) {
                menuBar()->removeAction(a);
                delete m;
            }
        }
        updateCanvas();
        arbitrateKeys();
    }

    /* The tracker destinations: one per synth tab, named by its plug-in. */
    struct SinkEntry { HostWidget *tab; int id; QString name; SinkCtx *ctx; };

    QString uniqueSinkName(const QString &base, const HostWidget *exclude) const
    {
        const QString b = base.isEmpty() ? QString("synth") : base;
        QString name = b;
        for (int n = 2; ; n++) {
            bool taken = false;
            for (const SinkEntry &s : sinks_)
                if (s.tab != exclude && s.name == name) { taken = true; break; }
            if (!taken) return name;
            name = QString("%1 %2").arg(b).arg(n);
        }
    }
    void ensureSink(HostWidget *h)
    {
        if (!trkEngine_ || !h) return;
        for (const SinkEntry &s : sinks_) if (s.tab == h) return;
        SinkCtx *ctx = new SinkCtx{ h->engine(), h };
        const int id = trk_add_sink(trkEngine_,
                                    uniqueSinkName(h->windowTitle(), h).toUtf8().constData(),
                                    &sinkDeliver, ctx);
        if (id < 0) { delete ctx; return; }
        trk_sink_set_gain_cb(trkEngine_, id, &sinkGain);
        char nm[TRK_DEST_LEN] = "";
        trk_sink_name(trkEngine_, id, nm, sizeof nm);
        sinks_.append({ h, id, QString::fromUtf8(nm), ctx });
    }
    void removeSink(HostWidget *h)
    {
        for (int i = 0; i < sinks_.size(); i++)
            if (sinks_[i].tab == h) {
                if (trkEngine_) trk_remove_sink(trkEngine_, sinks_[i].id);   // waits out a delivery in flight
                delete sinks_[i].ctx;
                sinks_.removeAt(i);
                return;
            }
    }
    void renameSink(HostWidget *h)
    {
        for (SinkEntry &s : sinks_)
            if (s.tab == h) {
                const QString name = uniqueSinkName(h->windowTitle(), h);
                if (name == s.name) return;
                s.name = name;
                if (trkEngine_) trk_sink_rename(trkEngine_, s.id, name.toUtf8().constData());
                return;
            }
    }

    /* A sink name back to the plug-in it came from, as an absolute path, or
     * empty when nothing on this machine answers to it. The name is the
     * plug-in's own -- it was the tab's window title when the song was saved
     * -- and the scan the HostWidgets made of the plug-in folders keeps its
     * result private to them, so this walks the same folders itself (once per
     * song open; reopenSongSynths clears pluginScan_ on the way in) and
     * matches the name against what it finds. An open tab under exactly that
     * title answers first: its loaded path IS that plug-in, no guessing. On
     * disk the file's name and the plug-in's name usually agree ("Surge
     * XT.vst3", "TAL-NoiseMaker.so"); where they differ it is by decoration
     * or a vendor's tag, so the later stages compare stripped of punctuation
     * ("FB-7999" is fb799964.dll), then allow a tag of trailing digits
     * ("blooo" is blooo64.dll), then one up front ("FM8" is NI FM8.dll) --
     * the shortest spelling wins, as the least decorated. */
    QString resolvePlugin(const QString &baseName)
    {
        for (int i = 0; i < tabs_->count(); i++) {
            auto *h = qobject_cast<HostWidget *>(tabs_->widget(i));
            if (h && !h->loadedPath().isEmpty() && h->windowTitle() == baseName)
                return h->loadedPath();
        }
        for (int i = 0; i < tabs_->count(); i++) {
            auto *h = qobject_cast<HostWidget *>(tabs_->widget(i));
            if (h && !h->loadedPath().isEmpty() &&
                !h->windowTitle().compare(baseName, Qt::CaseInsensitive))
                return h->loadedPath();
        }
        if (pluginScan_.isEmpty()) scanPlugins();
        for (const auto &p : pluginScan_)
            if (p.first == baseName) return p.second;
        for (const auto &p : pluginScan_)
            if (!p.first.compare(baseName, Qt::CaseInsensitive)) return p.second;
        const QString want = bareName(baseName);
        for (const auto &p : pluginScan_)
            if (bareName(p.first) == want) return p.second;
        /* Trailing digits, then a prefix of any kind; the shortest match, in
         * each stage, is the plug-in rather than its sibling editions. */
        QString best;
        int bestLen = 0;
        for (const auto &p : pluginScan_) {
            const QString have = bareName(p.first);
            if (have.size() <= want.size() || !have.startsWith(want)) continue;
            bool tag = true;
            for (int i = want.size(); i < have.size() && tag; i++)
                if (!have[i].isDigit()) tag = false;
            if (tag && (best.isEmpty() || have.size() < bestLen)) {
                best = p.second;
                bestLen = have.size();
            }
        }
        if (!best.isEmpty() || want.size() < 3) return best;
        for (const auto &p : pluginScan_) {
            const QString have = bareName(p.first);
            if (have.size() > want.size() && have.endsWith(want) &&
                (best.isEmpty() || have.size() < bestLen)) {
                best = p.second;
                bestLen = have.size();
            }
        }
        return best;
    }

    /* The name with its punctuation and case off, for the looser stages:
     * "FB-7999" and "fb799964" have to meet. */
    static QString bareName(const QString &s)
    {
        QString out;
        for (QChar c : s)
            if (c.isLetterOrNumber()) out += c.toLower();
        return out;
    }

    /* Fill pluginScan_ with (stem, absolute path) for everything that names
     * like a plug-in under the folders the HostWidgets scan: the corpora
     * found walking up from the binary, the system VST locations, and the
     * user's own list from vstdirs. The same roots HostWidget::scanRoots
     * finds, duplicated here because that scan's result is private -- and
     * walked without the sniffing scanRoot does: opening every file to prove
     * it a plug-in is the cost of a startup scan, not of answering one name,
     * and a stem only ever reaches loadPlugin by matching a sink name, which
     * is the certain test. */
    void scanPlugins()
    {
        QStringList roots;
        auto addRoot = [&roots](const QString &p) {
            if (p.isEmpty()) return;
            const QString abs = QDir(p).absolutePath();
            if (QDir(abs).exists() && !roots.contains(abs)) roots << abs;
        };
        static const char *corpus[] = {
            "windows/VST2-64", "windows/VST3", "linux/extracted", "windows/VST2-32",
#if PESTUDIO_MAC
            "macos/VST2", "macos/VST3", "macos/AU",
#endif
#if PESTUDIO_CLASSIC
            "macos/classic",
#endif
        };
        QDir up(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 6; i++) {
            for (const char *rel : corpus) addRoot(up.absoluteFilePath(rel));
            if (!up.cdUp()) break;
        }
        const QString home = QDir::homePath();
        for (const QString &d : { home + "/.vst", home + "/.vst3",
                                  QString("/usr/lib/vst"), QString("/usr/lib/vst3"),
                                  QString("/usr/local/lib/vst"), QString("/usr/local/lib/vst3"),
                                  QString("/usr/lib/x86_64-linux-gnu/vst"),
                                  QString("/usr/lib/x86_64-linux-gnu/vst3") })
            addRoot(d);
        for (const char *var : { "VST_PATH", "VST3_PATH" }) {
            const QString e = qEnvironmentVariable(var);
            if (e.isEmpty()) continue;
            const QStringList parts = e.split(':', Qt::SkipEmptyParts);
            for (const QString &part : parts) addRoot(part);
        }
        vstdir userDirs[VSTDIRS_MAX];
        const int nUser = vstdirs_load(userDirs, VSTDIRS_MAX);
        for (int i = 0; i < nUser; i++)
            addRoot(QString::fromLocal8Bit(userDirs[i].path));

        QStringList queue = roots;
        int guard = 0;
        while (!queue.isEmpty() && guard++ < 4000) {
            QDir d(queue.takeFirst());
            const QFileInfoList es = d.entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                QDir::Name | QDir::IgnoreCase);
            for (const QFileInfo &fi : es) {
                const QString nm = fi.fileName();
                /* A .vst3 or macOS .vst bundle is a directory and a leaf, as
                 * in scanRoot; .lv2 innards are walked but never offered. */
                const bool leafDir = fi.isDir() &&
                    (nm.endsWith(".vst3", Qt::CaseInsensitive) ||
                     nm.endsWith(".vst", Qt::CaseInsensitive));
                const bool leafFile = fi.isFile() &&
                    (nm.endsWith(".dll", Qt::CaseInsensitive) ||
                     nm.endsWith(".vst3", Qt::CaseInsensitive) ||
                     (nm.endsWith(".so", Qt::CaseInsensitive) &&
                      !fi.absoluteFilePath().contains(".lv2/")));
                if (leafDir || leafFile)
                    pluginScan_ << qMakePair(fi.completeBaseName(),
                                             fi.absoluteFilePath());
                else if (fi.isDir())
                    queue << fi.absoluteFilePath();
            }
        }
    }

    /* Only the tab in front answers the computer keyboard. Every HostWidget
     * filters keys application-wide, and in one window they would otherwise
     * all answer at once -- see HostWidget::setKeysLive. Switching away from a
     * tab releases its held notes on the way out. The menus follow the same
     * tab, so both move from this one place. */
    void arbitrateKeys()
    {
        QWidget *cur = tabs_->currentWidget();
        for (int i = 0; i < tabs_->count(); i++)
            if (auto *h = qobject_cast<HostWidget *>(tabs_->widget(i)))
                /* Not also `h->isVisible()`: a tab opened at startup is arbitrated
                 * before the window is shown, so it was switched off for good --
                 * the keys then worked only while the on-screen keyboard itself
                 * had focus, and died the moment a program or patch was clicked. */
                h->setKeysLive(tabs_->widget(i) == cur);
        syncMenus();
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
    QMenu          *recentMenu_ = nullptr;
    QAction        *mixAct_ = nullptr;      /* File > Record studio mix */
    QLabel         *mixLabel_ = nullptr;
    QTimer         *mixTimer_ = nullptr;
    QString         mixDir_;                /* where the last mix went */
    int             trackerMixId_ = -1;     /* the tracker's place on the mix bus */
    QAction        *reloadAct_ = nullptr;
    TrackerWidget  *tracker_ = nullptr;
    QMenu          *helpMenu_ = nullptr;   /* the window's own, kept last in the bar */
    QPointer<QDialog> pluginMgr_;          /* File > Plug-ins, while it is open */
    QMenu          *fileMenu_ = nullptr;
    QAction        *fileAnchor_ = nullptr, *helpAnchor_ = nullptr, *openSongAct_ = nullptr;
    trk_engine     *trkEngine_ = nullptr;
    QList<SinkEntry> sinks_;             /* every synth tab the tracker can play directly */
    /* The plug-in folders as (file stem, absolute path) pairs, for
     * resolvePlugin -- filled lazily by scanPlugins, cleared by
     * reopenSongSynths so a song opened later in the run sees the folders as
     * they are then. */
    QList<QPair<QString, QString>> pluginScan_;
    QString        sessionPath_;         /* the file Save session writes without asking */
    QString        saveOnExit_;          /* --save-session: written on the clean exit */
    QSet<HostWidget *>           deadTabs_;    /* tabs already marked (stopped) */
    QHash<HostWidget *, unsigned long> lastCalls_;  /* callback counts, for the tooltips */
    bool           routed_ = false;      /* --route: the tracker drives the proof, not smoke's notes */
    /* Set around each tab's construction, which is when both widgets build
     * their menus through addMenu; each menu handed out is remembered with
     * the action count it had then, and what it gained past that count is
     * attributed to the tab once it exists -- see attributeMenus. */
    bool           building_ = false;
    QList<QPair<QMenu *, int>> pendingStarts_;
    QHash<QWidget *, QList<QPair<QMenu *, QAction *>>> actionsOf_;
    QList<QMenu *> shellMenus_;      /* File and Help: always on the bar */
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

    /* A session can be named rather than clicked together:
     *
     *   studio --synth blooo64.dll --synth "Surge XT.vst3" --tracker
     *   studio --song song.trk
     *   studio --session take-five.vstace   a saved session, whole
     *   studio --route 2 --synth ... --tracker   track 2 plays the first synth tab
     *   studio --smoke 15000 --synth ...   scripted exercise, then quit
     *   studio --quit-after 15000 ...      just quit then
     *   studio --save-session out.vstace ...   write the session on the clean exit
     */
    QStringList synths;
    QString song, session, saveSession;
    bool wantTracker = false, wantPlay = false;
    QString recordMix;
    int quitAfter = 0, smoke = 0, route = -1;
    /* Read before QApplication sees argv, which it edits: Qt takes
     * -session/--session as its own X11 session-management option and removes
     * it, so --session never got here. What is not ours is passed on to Qt,
     * and whatever Qt does not take either is the unknown argument. */
    std::vector<char *> qtArgv{argv[0]};
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--synth" && i + 1 < argc)
            synths << QString::fromLocal8Bit(argv[++i]);
        else if (a == "--tracker")
            wantTracker = true;
        else if (a == "--song" && i + 1 < argc)
            song = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--session" && i + 1 < argc)
            session = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--save-session" && i + 1 < argc)
            saveSession = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--route" && i + 1 < argc)
            route = atoi(argv[++i]);
        else if (a == "--quit-after" && i + 1 < argc)
            quitAfter = atoi(argv[++i]);
        else if (a == "--smoke" && i + 1 < argc)
            smoke = atoi(argv[++i]);
        else if (a == "--record-mix" && i + 1 < argc)
            recordMix = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--play")
            wantPlay = true;
        else if (a == "--backend" && i + 1 < argc) {
            /* The audio choice reads DW_BACKEND itself, so the flag is that. */
            const char *name = argv[++i];
            if (ao_backend_from_name(name) < 0)
                fprintf(stderr, "studio: unknown backend '%s' (want auto, pipewire, jack or alsa) -- using the saved choice\n", name);
            else
                setenv("DW_BACKEND", name, 1);
        }
        else if (a == "--help" || a == "-h") {
            printf("studio [--synth <plug-in>]... [--tracker] [--song <file.trk>]\n"
                   "       [--session <file.vstace>] [--save-session <file.vstace>]\n"
                   "       [--route <track>] [--smoke <ms>] [--quit-after <ms>]\n"
                   "       [--backend auto|pipewire|jack|alsa]\n"
                   "       [--record-mix <file.wav>] [--play]\n\n"
                   "The session window: a tab per synth plug-in, one for the "
                   "tracker.\nWith no arguments it opens on a blank canvas.\n"
                   "--session restores a session saved with File > Save session; "
                   "--save-session\nwrites one on the clean exit.\n"
                   "--route plays the numbered track into the first synth tab, "
                   "in-process,\nand starts the song -- the scripted proof of the "
                   "direct routing.\n");
            return 0;
        } else {
            qtArgv.push_back(argv[i]);
        }
    }
    int qtArgc = int(qtArgv.size());
    qtArgv.push_back(nullptr);

    QApplication app(qtArgc, qtArgv.data());
    app.setApplicationName("studio");
    /* Before any plugin is opened: the Classic backend is handed this when its
     * shim is built. */
    pehost_set_input_pump(pump_input, nullptr);

    if (qtArgc > 1) {
        fprintf(stderr, "studio: unknown argument %s -- try --help\n", qtArgv[1]);
        return 2;
    }

    SessionShell w;
    w.show();
    if (!session.isEmpty()) w.openSessionPath(session);
    for (const QString &s : synths) w.openSynth(s);
    if (wantTracker) w.openTracker();
    if (!song.isEmpty()) w.openSongPath(song);
    if (!recordMix.isEmpty()) w.beginMixRecord(recordMix);
    if (wantPlay) w.playSong();
    if (!saveSession.isEmpty()) w.saveSessionOnExit(saveSession);
    if (route > 0 && !w.routeTrack(route - 1))
        fprintf(stderr, "studio: --route %d failed (no synth tab, or no tracker?)\n", route);
    if (smoke > 0)
        w.startSmokeDrive(smoke);
    else if (quitAfter > 0)
        QTimer::singleShot(quitAfter, &app, &QCoreApplication::quit);
    return app.exec();
}

#include "main.moc"
