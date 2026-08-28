/* A live X11 window for the i386 loader's editors.
 *
 * The plugin draws into a pixel buffer the Win32 layer owns (win32gui.h); this
 * puts that buffer on screen and feeds X input back in as WM_* messages. It is
 * deliberately plain Xlib -- pestudio already covers the toolkit case on
 * x86-64, and a 32-bit process cannot use it, so the point here is to have no
 * dependency beyond libX11.
 *
 * Runs on the main thread. Audio, if --play is also given, is on PipeWire's own
 * thread, so the two do not block each other.
 */
#ifndef PELOAD_X11WIN32_H
#define PELOAD_X11WIN32_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

/* The subset of WM_* the Win32 layer understands. */
enum {
    XW_MOUSEMOVE = 0x0200, XW_LBUTTONDOWN = 0x0201, XW_LBUTTONUP = 0x0202,
    XW_RBUTTONDOWN = 0x0204, XW_RBUTTONUP = 0x0205,
    XW_MBUTTONDOWN = 0x0207, XW_MBUTTONUP = 0x0208, XW_MOUSEWHEEL = 0x020A
};
#define XW_MK_LBUTTON 0x0001
#define XW_MK_RBUTTON 0x0002
#define XW_MK_MBUTTON 0x0010

static int xw_buttons(unsigned state)
{
    int m = 0;
    if (state & Button1Mask) m |= XW_MK_LBUTTON;
    if (state & Button3Mask) m |= XW_MK_RBUTTON;
    if (state & Button2Mask) m |= XW_MK_MBUTTON;
    return m;
}

/* X11 keysym -> Windows virtual key, for the keys an editor plausibly wants. */
static int xw_vk(KeySym ks)
{
    if (ks >= XK_a && ks <= XK_z) return (int)(ks - XK_a) + 'A';
    if (ks >= XK_A && ks <= XK_Z) return (int)(ks - XK_A) + 'A';
    if (ks >= XK_0 && ks <= XK_9) return (int)(ks - XK_0) + '0';
    switch (ks) {
    case XK_BackSpace: return 0x08;
    case XK_Tab:       return 0x09;
    case XK_Return: case XK_KP_Enter: return 0x0D;
    case XK_Escape:    return 0x1B;
    case XK_space:     return 0x20;
    case XK_Prior:     return 0x21;
    case XK_Next:      return 0x22;
    case XK_End:       return 0x23;
    case XK_Home:      return 0x24;
    case XK_Left:      return 0x25;
    case XK_Up:        return 0x26;
    case XK_Right:     return 0x27;
    case XK_Down:      return 0x28;
    case XK_Delete:    return 0x2E;
    default:           return 0;
    }
}

static int xw_run(AEffect32 *fx, int cycles)
{
    Display *dpy;
    Window win;
    GC gc;
    XImage *img = NULL;
    Atom wm_delete;
    int screen, w = 0, h = 0, imgw = 0, imgh = 0, depth;
    Visual *vis;
    char *imgbuf = NULL;
    int running = 1, frames = 0;

    if (ed_open(fx, &w, &h)) return 1;
    printf("editor: %dx%d\n", w, h);

    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "editor: no X display (set DISPLAY, or use --editor-png)\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    vis    = DefaultVisual(dpy, screen);
    depth  = DefaultDepth(dpy, screen);
    if (depth != 24 && depth != 32) {
        fprintf(stderr, "editor: need a 24- or 32-bit display, got %d\n", depth);
        XCloseDisplay(dpy);
        return 1;
    }

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              (unsigned)w, (unsigned)h, 0,
                              BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XStoreName(dpy, win, "peload32");
    {   /* ask the window manager not to resize us: the editor is a fixed size */
        XSizeHints *sh = XAllocSizeHints();
        if (sh) {
            sh->flags = PMinSize | PMaxSize;
            sh->min_width = sh->max_width = w;
            sh->min_height = sh->max_height = h;
            XSetWMNormalHints(dpy, win, sh);
            XFree(sh);
        }
    }
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | KeyPressMask | KeyReleaseMask |
                           StructureNotifyMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);

    printf("editor: window open -- close it, or Ctrl-C, to stop.\n");
    fflush(stdout);

    while (running) {
        const unsigned int *px;
        int pw, ph;

        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            switch (e.type) {
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == wm_delete) running = 0;
                break;
            case MotionNotify:
                w32_mouse(e.xmotion.x, e.xmotion.y, XW_MOUSEMOVE,
                          xw_buttons(e.xmotion.state), 0);
                break;
            case ButtonPress:
                /* X reports wheel notches as buttons 4 and 5. */
                if (e.xbutton.button == 4 || e.xbutton.button == 5)
                    w32_mouse(e.xbutton.x, e.xbutton.y, XW_MOUSEWHEEL,
                              xw_buttons(e.xbutton.state),
                              e.xbutton.button == 4 ? 1 : -1);
                else
                    w32_mouse(e.xbutton.x, e.xbutton.y,
                              e.xbutton.button == 1 ? XW_LBUTTONDOWN :
                              e.xbutton.button == 3 ? XW_RBUTTONDOWN : XW_MBUTTONDOWN,
                              xw_buttons(e.xbutton.state), 0);
                break;
            case ButtonRelease:
                if (e.xbutton.button != 4 && e.xbutton.button != 5)
                    w32_mouse(e.xbutton.x, e.xbutton.y,
                              e.xbutton.button == 1 ? XW_LBUTTONUP :
                              e.xbutton.button == 3 ? XW_RBUTTONUP : XW_MBUTTONUP,
                              xw_buttons(e.xbutton.state), 0);
                break;
            case KeyPress: case KeyRelease: {
                char buf[8] = { 0 };
                KeySym ks = 0;
                int n = XLookupString(&e.xkey, buf, sizeof buf - 1, &ks, NULL);
                int vk = xw_vk(ks);
                if (vk) w32_key(vk, e.type == KeyPress, n > 0 ? buf[0] : 0);
                break;
            }
            default: break;
            }
        }

        ed_pump(fx);

        if (w32_editor_pixels(&px, &pw, &ph) && px && pw > 0 && ph > 0) {
            if (!img || pw != imgw || ph != imgh) {
                if (img) XDestroyImage(img);          /* frees imgbuf too */
                imgbuf = malloc((size_t)pw * ph * 4);
                if (!imgbuf) break;
                img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0,
                                   imgbuf, (unsigned)pw, (unsigned)ph, 32, pw * 4);
                if (!img) { free(imgbuf); break; }
                imgw = pw; imgh = ph;
            }
            /* The Win32 layer's buffer is BGRX, which is what a 24/32-bit
             * TrueColor X visual wants on a little-endian host -- so this is a
             * straight copy, not a conversion. */
            memcpy(imgbuf, px, (size_t)pw * ph * 4);
            XPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned)pw, (unsigned)ph);
        }
        XFlush(dpy);

        if (cycles > 0 && ++frames >= cycles) running = 0;
        { struct timespec ts = { 0, 16000000 }; nanosleep(&ts, NULL); }
    }

    fx->dispatcher(fx, effEditClose, 0, 0, NULL, 0.0f);
    w32_reset();
    if (img) XDestroyImage(img);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

#endif /* PELOAD_X11WIN32_H */
