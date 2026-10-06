/* pestudio -- standalone Qt6 front end for the native Windows-VST2 loader.
 *
 * Browses the plugin directory, loads a DLL through pehost (no Wine), and plays
 * it live: programs, every exposed parameter, and a playable keyboard.
 *
 * All of that lives in qthost (qtgui/hostwindow.h) as HostWidget, one per
 * plug-in; this file is the QMainWindow shell around a single instance of it,
 * plus main(). The studio window replaces the shell and keeps the widget. */

#include <QtWidgets>

#include "hostwindow.h"

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
    }

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
    HostWidget *host_ = nullptr;
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
