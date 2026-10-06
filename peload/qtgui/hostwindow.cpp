/* hostwindow -- out-of-line halves of hostwindow.h: the PipeWire device
 * enumeration, the X11 stuck-note watchdog, the re-entrancy counter, and
 * EditorHost's hooks owner. Everything else is inline in the header, exactly
 * where pestudio's main.cpp used to keep it. */

#include "hostwindow.h"

/* Declared in hostwindow.h, where the reason for it lives. */
int g_inPlugin;

/* ------------------------------------------------------ input devices
 *
 * What the machine has to listen with: microphones, USB interfaces, and the
 * monitor of anything that is playing. PipeWire calls them Audio/Source nodes,
 * and the registry is the only place that knows about them, so this opens a
 * short-lived connection of its own rather than reaching into the one the audio
 * thread is using. It runs for as long as the enumeration takes -- one core
 * round trip -- and only when the window asks. InputDevice itself is declared
 * in hostwindow.h, where the widget that consumes the list can see it. */
struct DevScan {
    QList<InputDevice> found;
    pw_main_loop      *loop = nullptr;
    int                sync = 0;
};

static void dev_global(void *data, uint32_t, uint32_t,
                       const char *type, uint32_t, const struct spa_dict *props)
{
    DevScan *s = static_cast<DevScan *>(data);
    if (!type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
    const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!cls || strcmp(cls, "Audio/Source") != 0) return;
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!name) return;
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    InputDevice d;
    d.node  = QString::fromUtf8(name);
    d.label = QString::fromUtf8(desc && *desc ? desc : name);
    s->found.push_back(d);
}

static void dev_core_done(void *data, uint32_t id, int seq)
{
    DevScan *s = static_cast<DevScan *>(data);
    if (id == PW_ID_CORE && seq == s->sync) pw_main_loop_quit(s->loop);
}

QList<InputDevice> listInputDevices()
{
    DevScan scan;
    pw_context  *ctx  = nullptr;
    pw_core     *core = nullptr;
    pw_registry *reg  = nullptr;
    spa_hook     rl{}, cl{};

    static const pw_registry_events rev = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global  = dev_global,
        .global_remove = nullptr,
    };
    static const pw_core_events cev = {
        .version = PW_VERSION_CORE_EVENTS,
        .info = nullptr, .done = dev_core_done, .ping = nullptr,
        .error = nullptr, .remove_id = nullptr,
        .bound_id = nullptr, .add_mem = nullptr, .remove_mem = nullptr,
    };

    scan.loop = pw_main_loop_new(nullptr);
    if (!scan.loop) return scan.found;
    ctx = pw_context_new(pw_main_loop_get_loop(scan.loop), nullptr, 0);
    if (ctx) core = pw_context_connect(ctx, nullptr, 0);
    if (core) {
        reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (reg) {
            pw_registry_add_listener(reg, &rl, &rev, &scan);
            pw_core_add_listener(core, &cl, &cev, &scan);
            /* Ask the server to say when it has finished replaying the globals
             * it already has; without it the loop waits forever for a device
             * that is not going to appear. */
            scan.sync = pw_core_sync(core, PW_ID_CORE, 0);
            pw_main_loop_run(scan.loop);
        }
    }
    if (reg)  pw_proxy_destroy(reinterpret_cast<pw_proxy *>(reg));
    if (core) pw_core_disconnect(core);
    if (ctx)  pw_context_destroy(ctx);
    pw_main_loop_destroy(scan.loop);
    return scan.found;
}

#ifdef PEHOST_HAVE_X11
KeyWatch::KeyWatch()
{
    /* Only the xcb platform has the foreign-X-window problem this exists
     * for, and only xcb has a Display to ask. On Wayland the release of a
     * key always reaches the window that has focus, which is ours, so the
     * watchdog is never needed and available() stays false. */
    if (!QGuiApplication::platformName().startsWith(QLatin1String("xcb")))
        return;
    if (auto *x = qApp->nativeInterface<QNativeInterface::QX11Application>())
        dpy_ = x->display();
    if (dpy_)
        query = [d = dpy_](char *keys32) { return XQueryKeymap(d, keys32) != 0; };
}

bool KeyWatch::tick()
{
    if (!dpy_ || !isKeyHeld || !noteKey || !release) return false;
    bool any = false;
    for (int n = 0; n < 128; n++) if (isKeyHeld(n)) { any = true; break; }
    if (!any) { up_.clear(); return false; }

    char keys[32];
    if (!query || !query(keys)) {
        /* The display is gone, so nothing more can be learned from it. Give
         * up silently: the log line below exists so a user report can confirm
         * this mechanism fired, and a dead display is not that. */
        dpy_ = nullptr;
        up_.clear();
        return false;
    }
    if (++sinceMap_ >= kRemapTicks) loadMap();

    for (int n = 0; n < 128; n++) {
        if (!isKeyHeld(n)) { up_.remove(n); continue; }
        const int k = noteKey(n);
        const QVector<int> kcs = kcs_.value(k);
        /* A char with no known keycode cannot be called up; treat it as down
         * rather than release a note on a guess. */
        bool down = kcs.isEmpty();
        for (int kc : kcs)
            if (keys[kc >> 3] & (1 << (kc & 7))) { down = true; break; }
        if (down) { up_.remove(n); continue; }
        if (++up_[n] < kGrace) continue;
        up_.remove(n);
        fprintf(stderr, "piano: key '%c' up but note %d held -- released "
                        "(release event lost)\n", char(k), n);
        release(k);
    }
    for (int n = 0; n < 128; n++) if (isKeyHeld(n)) return true;
    return false;
}

void KeyWatch::loadMap()
{
    kcs_.clear();
    sinceMap_ = 0;
    int minKC = 0, maxKC = 0, per = 0;
    XDisplayKeycodes(dpy_, &minKC, &maxKC);
    KeySym *map = XGetKeyboardMapping(dpy_, minKC, maxKC - minKC + 1, &per);
    if (!map) return;
    /* Index 0 of each group is the unshifted keysym -- the same assumption
     * the snoop's press path makes. For the letters and digits of the note
     * rows the keysym is the ASCII char. A char can sit on more than one
     * keycode; any of its keycodes down counts as the key down. */
    for (int n = 0; n < 128; n++) {
        const int k = noteKey(n);
        if (!k) continue;
        const KeySym want = KeySym(k);
        for (int kc = minKC; kc <= maxKC; kc++)
            if (map[(kc - minKC) * per] == want)
                kcs_[k].append(kc);
    }
    XFree(map);
}
#endif

EditorHost *EditorHost::self_ = nullptr;
