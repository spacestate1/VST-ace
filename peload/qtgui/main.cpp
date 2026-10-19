/* pestudio -- standalone Qt6 front end for the native Windows-VST2 loader.
 *
 * Browses the plugin directory, loads a DLL through pehost (no Wine), and plays
 * it live: programs, every exposed parameter, and a playable keyboard.
 *
 * All of that lives in qthost (qtgui/hostwindow.h) as HostWidget, one per
 * plug-in; this file is the QMainWindow shell around a single instance of it,
 * plus main(). The studio window replaces the shell and keeps the widget. */

#include <QtWidgets>
#include <cstdio>

#include "hostwindow.h"
#include "reaperlink.h"

/* pestudio's shell: one HostWidget under the menu bar and the status bar the
 * widget asks its shell for. Everything per-plug-in is in the widget; this is
 * only the frame, which is the part the multi-plug-in studio window swaps
 * out. */
class StudioShell : public QMainWindow, public HostShell {
public:
    StudioShell(const QString &dir, const QString &plugin,
                const QString &bankFile, patch_bank *bank, int startPatch)
    {
        setWindowTitle("pestudio -- native Windows VST2 host");
        host_ = new HostWidget(this, dir, plugin, bankFile, bank, startPatch, this);
        setCentralWidget(host_);
        resize(1180, 760);

        /* Connect > REAPER, the studio window's switch for this window's one
         * synth -- see reaperlink.h. Several pestudios can be on at once: each
         * takes the next REAPER input pair and MIDI output none of the others
         * holds. The choice is the studio's too, so a new pestudio comes up
         * connected when the last one was left connected. */
        /* Real check boxes rather than checkable actions: many menu styles draw
         * nothing for an unticked action, so off cannot be told from absent. */
        QMenu *cm = menuBar()->addMenu("&Connect");
        auto addBox = [cm](const QString &text, const QString &tip) {
            auto *wa = new QWidgetAction(cm);
            auto *pad = new QWidget;
            auto *lay = new QHBoxLayout(pad);
            lay->setContentsMargins(8, 4, 8, 4);
            auto *box = new QCheckBox(text);
            box->setToolTip(tip);
            lay->addWidget(box);
            wa->setDefaultWidget(pad);
            cm->addAction(wa);
            return box;
        };
        reaperBox_ = addBox("Connect to REAPER",
                            "Link this synth to REAPER: its audio to the next free pair of "
                            "REAPER's inputs, and the next free REAPER MIDI output to it");
        connectInfo_ = cm->addAction("REAPER: off");
        connectInfo_->setEnabled(false);
        connect(reaperBox_, &QCheckBox::toggled, this, [this](bool on) { reaperToggled(on); });
        cm->addSeparator();
        bridgeBox_ = addBox("Show in DAWs' FX list",
                            "Put this plug-in in ~/.vst as a Linux VST2, so REAPER (or any DAW) "
                            "loads it on a track itself. Rescan plug-ins in the DAW afterwards.");
        connect(bridgeBox_, &QCheckBox::clicked, this, [this](bool on) { bridgeToggled(on); });
        auto *watch = new QTimer(this);
        connect(watch, &QTimer::timeout, this, [this] { updateConnect(); updateBridge(); });
        watch->start(1000);
        if (loadReaperPref()) reaperBox_->setChecked(true);

        /* Each start: the bridge copies already in ~/.vst brought up to this
         * build, and the helpers' directory written down again -- so neither a
         * rebuild nor a moved tree leaves DAWs running an old or lost bridge. */
        if (bridgeUsable()) {
            saveHelperDir();
            const int n = refreshBridgeCopies();
            if (n > 0)
                statusBar()->showMessage(QString("updated %1 plug-in%2 in %3 to this build -- restart "
                                                 "the DAW to use them").arg(n).arg(n == 1 ? "" : "s").arg(vstDir()), 0);
        }
    }
    ~StudioShell() override { rl_close(rl_); }       /* the links it made go with it */

    HostWidget *host() const { return host_; }

    void statusMessage(const QString &text, int timeoutMs) override
    { statusBar()->showMessage(text, timeoutMs); }
    QMenu *addMenu(const QString &title) override
    { return menuBar()->addMenu(title); }

protected:
    /* The crash marker is cleared when the top level closes, which is this
     * window, not the HostWidget inside it. Same marker, same semantics --
     * see HostWidget::clearCrashMarker. */
    void closeEvent(QCloseEvent *e) override
    {
        HostWidget::clearCrashMarker();
        QMainWindow::closeEvent(e);
    }

private:
    /* The same file the studio window keeps its switch in. */
    static QString connectPrefPath()
    {
        const QByteArray x = qgetenv("XDG_CONFIG_HOME");
        const QString base = x.isEmpty() ? QDir::homePath() + "/.config" : QString::fromLocal8Bit(x);
        return base + "/vst-ace/connect";
    }
    static bool loadReaperPref()
    {
        QFile f(connectPrefPath());
        return f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).contains("reaper 1");
    }
    static void saveReaperPref(bool on)
    {
        QDir().mkpath(QFileInfo(connectPrefPath()).absolutePath());
        QSaveFile f(connectPrefPath());
        if (f.open(QIODevice::WriteOnly)) { f.write(on ? "reaper 1\n" : "reaper 0\n"); f.commit(); }
    }

    void reaperToggled(bool on)
    {
        if (on && !rl_ && !(rl_ = rl_open())) {
            statusBar()->showMessage("could not connect to PipeWire -- REAPER links need it", 0);
            QSignalBlocker b(reaperBox_);
            reaperBox_->setChecked(false);
            return;
        }
        saveReaperPref(on);
        connectSig_.clear();
        updateConnect();
    }

    /* Once a second and on the switch: the synth, when one is loaded, to the
     * linker -- only when something changed -- and its status to the menu. Its
     * MIDI client is named when the plug-in's MIDI opens, which can be after
     * the load, so this keeps looking rather than being told once. */
    void updateConnect()
    {
        if (!rl_) return;
        const bool loaded = !host_->loadedPath().isEmpty();
        const QByteArray node = host_->graphNodeName();
        const QString midi = host_->midiClientName();
        QString sig = reaperBox_->isChecked() ? "on" : "off";
        if (loaded) sig += "|" + QString::fromUtf8(node) + "/" + midi;
        if (sig != connectSig_) {
            connectSig_ = sig;
            rl_synth rs;
            memset(&rs, 0, sizeof rs);
            snprintf(rs.node, sizeof rs.node, "%s", node.constData());
            snprintf(rs.alsa_client, sizeof rs.alsa_client, "%s", midi.toUtf8().constData());
            rl_set(rl_, &rs, loaded ? 1 : 0, reaperBox_->isChecked());
        }
        char st[300];
        rl_status(rl_, st, sizeof st);
        connectInfo_->setText(QString::fromUtf8(st));
    }

    /* Connect > Show in DAWs' FX list: whatever pestudio has loaded, put where a
     * DAW looks for plug-ins. Three routes, by what the plug-in is:
     *
     *   Linux VST2   a link to it in ~/.vst -- the DAW loads it itself
     *   Linux VST3   a link to its bundle in ~/.vst3, likewise
     *   the rest     the bridge (peload/vstbridge.c): a copy of it in ~/.vst
     *                named after the plug-in, and a .vstace beside the copy
     *                naming the real file; inside the DAW it loads that file
     *                with the same loaders pestudio uses
     *
     * A copy of the bridge and not a link: the dynamic loader opens one file
     * once whatever it is called, and the bridge tells which plug-in it stands
     * for by the name it was opened under, so two links would both be the first
     * plug-in. */
    enum FxRoute { RouteNone, RouteLinkVst2, RouteLinkVst3, RouteBridge };
    static QString vstDir()  { return QDir::homePath() + "/.vst"; }
    static QString vst3Dir() { return QDir::homePath() + "/.vst3"; }
    static QString bridgeLib() { return QCoreApplication::applicationDirPath() + "/libvst-ace-bridge.so"; }
    /* A bridge copy, as opposed to a real Linux plug-in of the same name that
     * must not be overwritten or removed. Asked once a second for the menu, so
     * the answer is kept until the file changes rather than read again. */
    static bool isBridgeCopy(const QString &so)
    {
        static QHash<QString, QPair<QDateTime, bool>> seen;
        const QFileInfo fi(so);
        if (!fi.exists()) return false;
        auto it = seen.constFind(so);
        if (it != seen.constEnd() && it->first == fi.lastModified()) return it->second;
        QFile f(so);
        const bool is = f.open(QIODevice::ReadOnly) && f.readAll().contains("VSTACE_TARGET");
        seen.insert(so, { fi.lastModified(), is });
        return is;
    }
    /* The bridge and the helpers need only libc, libm, X11 and FreeType (and
     * PipeWire and ALSA for peload32), which every desktop a DAW runs on has,
     * so copies of them work outside the AppImage too. What does not is the
     * AppImage's own directory: a mount that moves on every run. */
    static bool inAppImage() { return qEnvironmentVariableIsSet("APPIMAGE"); }
    static bool bridgeUsable() { return QFileInfo::exists(bridgeLib()); }
    /* Copy `src` to `dst` without writing into a file a running DAW may have
     * open: a new file renamed over the old, which the DAW keeps until it
     * restarts. */
    static bool replaceWith(const QString &src, const QString &dst)
    {
        const QString tmp = dst + ".new";
        QFile::remove(tmp);
        if (!QFile::copy(src, tmp)) return false;
        QFile::setPermissions(tmp, QFile::permissions(src));
        if (::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(dst).constData()) != 0) {
            QFile::remove(tmp);
            return false;
        }
        return true;
    }
    static bool installBridge(const QString &so) { return replaceWith(bridgeLib(), so); }
    static bool sameBytes(const QString &a, const QString &b)
    {
        QFile fa(a), fb(b);
        return fa.size() == fb.size() && fa.open(QIODevice::ReadOnly) && fb.open(QIODevice::ReadOnly) &&
               fa.readAll() == fb.readAll();
    }
    /* Every bridge copy in ~/.vst that is not this build's bridge, replaced.
     * Returns how many were. */
    static int refreshBridgeCopies()
    {
        int n = 0;
        const QFileInfoList all = QDir(vstDir()).entryInfoList({ "*.so" }, QDir::Files);
        for (const QFileInfo &fi : all) {
            if (fi.isSymLink() || !isBridgeCopy(fi.filePath()) || sameBytes(fi.filePath(), bridgeLib()))
                continue;
            if (installBridge(fi.filePath())) n++;
        }
        return n;
    }
    /* A link we made: one that points at this plug-in, and so is ours to remove. */
    static bool isLinkTo(const QString &link, const QString &target)
    {
        const QFileInfo fi(link);
        return fi.isSymLink() &&
               QFileInfo(fi.symLinkTarget()).canonicalFilePath() == QFileInfo(target).canonicalFilePath();
    }
    static bool occupied(const QString &p) { return QFileInfo::exists(p) || QFileInfo(p).isSymLink(); }

    /* Classified once per plug-in rather than every tick: classifying a native
     * .so dlopens it. */
    FxRoute route()
    {
        const QString path = host_->loadedPath();
        if (path != routePath_) {
            routePath_ = path;
            route_ = RouteNone;
            if (!path.isEmpty()) {
                pehost_info in;
                pehost_classify(path.toLocal8Bit().constData(), &in);
                const bool linux_ = !strcmp(in.os, "linux");
                route_ = linux_ && !strcmp(in.format, "VST2") ? RouteLinkVst2
                       : linux_ && !strcmp(in.format, "VST3") ? RouteLinkVst3
                       : RouteBridge;
            }
        }
        return route_;
    }
    /* The bundle for a VST3, which the loaded path may be inside of. */
    QString vst3Bundle() const
    {
        QString p = host_->loadedPath();
        while (!p.isEmpty() && p != "/" && !p.endsWith(".vst3", Qt::CaseInsensitive))
            p = QFileInfo(p).path();
        return p.endsWith(".vst3", Qt::CaseInsensitive) ? p : host_->loadedPath();
    }
    QString fxName() const
    {
        const QString p = route_ == RouteLinkVst3 ? vst3Bundle() : host_->loadedPath();
        return QFileInfo(p).completeBaseName();
    }
    /* Where this plug-in's entry in the DAW's folders goes. */
    QString fxEntry()
    {
        switch (route()) {
        case RouteLinkVst3: return vst3Dir() + "/" + QFileInfo(vst3Bundle()).fileName();
        case RouteNone:     return QString();
        default:            return vstDir() + "/" + fxName() + ".so";
        }
    }
    bool inFxList()
    {
        const QString e = fxEntry();
        switch (route()) {
        case RouteLinkVst2: return isLinkTo(e, host_->loadedPath());
        case RouteLinkVst3: return isLinkTo(e, vst3Bundle());
        case RouteBridge:   return isBridgeCopy(e);
        default:            return false;
        }
    }

    void updateBridge()
    {
        const bool loaded = route() != RouteNone;
        QSignalBlocker b(bridgeBox_);
        bridgeBox_->setEnabled(loaded);
        bridgeBox_->setChecked(loaded && inFxList());
        bridgeBox_->setText(loaded ? "Show " + fxName() + " in DAWs' FX list"
                                   : "Show in DAWs' FX list (load a plug-in first)");
    }

    void bridgeToggled(bool on)
    {
        const FxRoute r = route();
        const QString e = fxEntry();
        const QString ptr = vstDir() + "/" + fxName() + ".vstace";
        if (r == RouteNone) return;
        if (!on) {
            if (inFxList()) { QFile::remove(e); if (r == RouteBridge) QFile::remove(ptr); }
            statusBar()->showMessage(fxName() + " taken out of the DAWs' list -- rescan plug-ins in the DAW", 0);
            updateBridge();
            return;
        }
        QString why;
        if (inFxList())
            ;                                        /* already there */
        else if (r == RouteLinkVst2 || r == RouteLinkVst3) {
            const QString target = r == RouteLinkVst3 ? vst3Bundle() : host_->loadedPath();
            if (occupied(e))
                why = e + " is another plug-in -- left alone";
            else {
                QDir().mkpath(QFileInfo(e).path());
                if (!QFile::link(target, e)) why = "could not make the link " + e;
            }
        } else if (!QFileInfo::exists(bridgeLib()))
            why = "the bridge is missing: no " + bridgeLib() + " (va build makes it)";
        else if (occupied(e) && !isBridgeCopy(e))
            why = e + " is another plug-in, not a bridge copy -- left alone";
        else {
            QDir().mkpath(vstDir());
            QSaveFile p(ptr);
            if (!installBridge(e))
                why = "could not copy the bridge to " + e;
            else if (!p.open(QIODevice::WriteOnly) || p.write(host_->loadedPath().toLocal8Bit() + "\n") < 0 ||
                     !p.commit()) {
                QFile::remove(e);
                why = "could not write " + ptr;
            } else
                saveHelperDir();
        }
        if (!why.isEmpty()) statusBar()->showMessage("FX list: " + why, 0);
        else statusBar()->showMessage(fxName() + " is in " + QFileInfo(e).path() + " -- in REAPER: Options > "
                                      "Preferences > Plug-ins > VST > Re-scan, then add it to a track", 0);
        updateBridge();
    }

    /* Where the helpers live for a DAW: beside pestudio, except in the
     * AppImage, whose directory is gone once it exits -- there they are copied
     * to ~/.local/share/vst-ace/helpers, and brought up to date each start. */
    static QString helperDir()
    {
        const QString here = QCoreApplication::applicationDirPath();
        if (!inAppImage()) return here;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                            "/vst-ace/helpers";
        QDir().mkpath(dir);
        for (const char *name : { "peserve", "peload32" }) {
            const QString src = here + "/" + name, dst = dir + "/" + name;
            if (QFileInfo::exists(src) && !(QFileInfo::exists(dst) && sameBytes(src, dst)))
                replaceWith(src, dst);
        }
        return dir;
    }

    /* Inside a DAW the bridge cannot find peserve or peload32 (32-bit
     * plug-ins) beside the executable, which is the DAW: this is where it
     * looks instead -- see bridge_helper_named in bridge_client.c. */
    static void saveHelperDir()
    {
        const QByteArray x = qgetenv("XDG_CONFIG_HOME");
        const QString cfg = (x.isEmpty() ? QDir::homePath() + "/.config" : QString::fromLocal8Bit(x)) +
                            "/vst-ace/helpers";
        QDir().mkpath(QFileInfo(cfg).path());
        QSaveFile f(cfg);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(helperDir().toLocal8Bit() + "\n");
            f.commit();
        }
    }

    HostWidget *host_ = nullptr;
    rl         *rl_ = nullptr;
    QCheckBox  *reaperBox_ = nullptr, *bridgeBox_ = nullptr;
    QAction    *connectInfo_ = nullptr;
    QString     connectSig_;
    QString     routePath_;
    FxRoute     route_ = RouteNone;
};

/* Let the window system deliver input while a plugin spins in its own drag loop.
 * Bounded, because this is called from inside that loop. */
static void pump_input(void *ud)
{
    (void)ud;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
}

int main(int argc, char **argv)
{
    /* Plugin editors are X11 windows, so this process has to be an X11 client.
     *
     * A VST3 editor is attached by handing the plugin a window id it treats as
     * an X11 Window (kPlatformTypeX11EmbedWindowID); a VST2 editor on Linux is
     * the same idea. Under Qt's Wayland backend, winId() is a Wayland surface
     * handle instead -- so the plugin runs Xlib calls against a window that does
     * not exist and dies inside its own GL setup, which looks for all the world
     * like a crash on load. XWayland is present on every Wayland desktop that
     * can run these plugins at all, so asking for xcb costs nothing and makes
     * embedding work. Set QT_QPA_PLATFORM yourself to override. */
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        const QByteArray session = qgetenv("XDG_SESSION_TYPE");
        if (session == "wayland" || qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
            if (qEnvironmentVariableIsSet("DISPLAY")) {
                qputenv("QT_QPA_PLATFORM", "xcb");
                fprintf(stderr, "pestudio: Wayland session -- using the xcb "
                                "backend so plugin editors can embed\n");
            } else {
                fprintf(stderr, "pestudio: Wayland session with no DISPLAY; "
                                "plugin editors need XWayland and will be "
                                "refused\n");
            }
        }
    }
    /* Isolate by default. This is a browser: it loads a hundred plugins in a
     * session, and one that faults would otherwise take the whole window down
     * and lose the user's place. TAL-U-No-62 does exactly that, in its own code,
     * every time. Behind the helper the crash costs a subprocess and the host
     * reports it. Editors still work -- they arrive as pixels through shared
     * memory -- except for native Linux plugins, which embed an X11 window and
     * are therefore kept in process by pehost regardless of this. */
    if (!qEnvironmentVariableIsSet("PEHOST_ISOLATE")) {
        pehost_set_isolation(1);
        fprintf(stderr, "pestudio: hosting plugins out-of-process "
                        "(PEHOST_ISOLATE=0 to disable)\n");
    }

    QApplication app(argc, argv);
    /* Must be installed before any plugin is opened: the Classic backend is handed
     * this when its shim is built, and without it a dial follows the click and
     * then nothing else. */
    pehost_set_input_pump(pump_input, nullptr);
    /* The positional argument is either a tree to browse or one plugin to open:
     *
     *   pestudio /path/to/linux/extracted            browse a tree
     *   pestudio fb799964.dll                        open one plugin
     *   pestudio fb799964.dll --patch glass-pad.json  ... with a patch applied
     *
     * Which it is comes from the filesystem rather than from a separate flag,
     * because a macOS plugin is a bundle -- a directory -- and so is a .vst3, so
     * "is it a directory" cannot decide it. pehost is asked instead: anything it
     * recognises as loadable is a plugin, and everything else is a root to scan.
     */
    QString dir, plugin, bankFile, pick;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cycle")) { i++; continue; }   /* takes a value */
        if (!strcmp(argv[i], "--patch") && i + 1 < argc) {
            bankFile = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "--pick") && i + 1 < argc) {
            pick = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (argv[i][0] != '-' && dir.isEmpty() && plugin.isEmpty() &&
            bankFile.isEmpty()) {
            const QString arg = QString::fromLocal8Bit(argv[i]);
            const QByteArray raw = QFileInfo(arg).absoluteFilePath().toLocal8Bit();
            char why[128] = "";
            if (arg.endsWith(".json", Qt::CaseInsensitive))
                bankFile = arg;
            else if (QFileInfo(arg).exists() &&
                     (pehost_can_load(raw.constData(), why, sizeof why) ||
                      pehost_is_classic_mac(raw.constData()) ||
                      pehost_is_native_vst2(raw.constData())))
                plugin = QFileInfo(arg).absoluteFilePath();
            else
                dir = arg;
        }
    }

    /* A bank names the plugin it was written for, so naming the bank is enough
     * to open both -- which is what makes a set of patches a thing you can hand
     * someone rather than a thing they have to be told how to load. */
    patch_bank *bank = nullptr;
    int startPatch = 0;
    if (!bankFile.isEmpty()) {
        char err[256];
        bank = patch_bank_read(bankFile.toLocal8Bit().constData(), err, sizeof err);
        if (!bank) {
            fprintf(stderr, "pestudio: %s\n", err);
            return 2;
        }
        if (patch_bank_is_multi_plugin(bank))
            fprintf(stderr, "pestudio: %s spans several plugins -- selecting a "
                            "patch loads the one it names\n",
                    qPrintable(QFileInfo(bankFile).fileName()));
        fprintf(stderr, "pestudio: %s -- %d patch(es) for %s\n",
                qPrintable(QFileInfo(bankFile).fileName()), patch_bank_count(bank),
                *patch_bank_plugin_name(bank) ? patch_bank_plugin_name(bank)
                                              : "an unnamed plugin");
        /* --pick takes a name or an index: a bank is read by people and driven
         * by scripts, and those want different handles on the same thing.
         *
         * Resolved before the plugin below, not after: in a cross-machine bank
         * each patch names its own plugin, so which patch was picked is what
         * decides which synth to open. */
        if (!pick.isEmpty()) {
            startPatch = -1;
            for (int i = 0; i < patch_bank_count(bank); i++)
                if (pick.compare(QString::fromLocal8Bit(patch_bank_patch_name(bank, i)),
                                 Qt::CaseInsensitive) == 0) { startPatch = i; break; }
            bool isNum = false;
            if (startPatch < 0) {
                int n = pick.toInt(&isNum);
                if (isNum && n >= 0 && n < patch_bank_count(bank)) startPatch = n;
            }
            if (startPatch < 0) {
                fprintf(stderr, "pestudio: no patch called \"%s\" in %s -- it has:\n",
                        qPrintable(pick), qPrintable(bankFile));
                for (int i = 0; i < patch_bank_count(bank); i++)
                    fprintf(stderr, "    %s\n", patch_bank_patch_name(bank, i));
                patch_bank_free(bank);
                return 2;
            }
        }
        if (plugin.isEmpty()) {
            /* The plugin the *starting* patch wants, which in a cross-machine
             * bank is not the bank default. */
            const char *pp = patch_bank_patch_plugin_path(bank, startPatch);
            if (*pp && QFileInfo(QString::fromLocal8Bit(pp)).exists())
                plugin = QString::fromLocal8Bit(pp);
            else if (*pp)
                fprintf(stderr, "pestudio: %s says its plugin is at %s, which is "
                                "not there -- name the plugin instead\n",
                        qPrintable(bankFile), pp);
            else
                fprintf(stderr, "pestudio: %s names no \"pluginPath\" -- name the "
                                "plugin as well\n", qPrintable(bankFile));
        }
    }
    fprintf(stderr, "pestudio: dir=\"%s\" plugin=\"%s\" bank=\"%s\"\n",
            qPrintable(dir), qPrintable(plugin), qPrintable(bankFile));
    fflush(stderr);
    StudioShell w(dir, plugin, bankFile, bank, startPatch);
    w.show();
    /* --cycle <ms> walks the whole list unattended, opening each editor in
     * turn. Switching plugins with an editor attached is the failure-prone
     * path, and clicking through 90-odd of them by hand is not repeatable. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--cycle")) {
            int ms = (i + 1 < argc) ? atoi(argv[i + 1]) : 0;
            w.host()->startCycle(ms > 0 ? ms : 1500);
            break;
        }
    return app.exec();
}
