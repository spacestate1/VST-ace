/* Dialogs, property sheets and the trackbar: what a plug-in's setup window is
 * made of.
 *
 * The layer in win32gui.h shows one window, the editor. A plug-in that keeps
 * its settings in a dialog -- Yamaha's S-YXG50 has a SETUP button that calls
 * PropertySheetA -- asks for a second, modal, window, and an import with no body
 * answers "nothing opened" and goes on. This file gives it one.
 *
 * Where it goes. The host shows whatever frame w32_editor_pixels returns, at
 * whatever size, so a dialog does not need a window of its own on screen: it is
 * a child of the host container placed below the editor, and the frame grows to
 * take it. Mouse input already reaches the child window under the pointer, so
 * the same path serves it. Closing it shrinks the frame back.
 *
 * What it is. A dialog is built from its template: the window ("#32770"), then
 * one child per item, from the same built-in classes everything else uses plus
 * the trackbar below. The plug-in's DLGPROC is called as Windows calls it --
 * WM_INITDIALOG first, then messages, with DWL_MSGRESULT, DWL_DLGPROC and
 * DWL_USER in the window's extra bytes where a plug-in looks for them. A
 * property sheet is a window of its own drawing a strip of tabs over a stack of
 * those dialogs, with OK, Cancel and Apply, and it sends each page the
 * PSN_SETACTIVE, PSN_KILLACTIVE, PSN_APPLY and PSN_RESET notifications a page
 * expects.
 *
 * Modal means a loop. PropertySheetA and DialogBoxParam return when the
 * dialog is over, so they run their own: pump the host's input, dispatch, wait,
 * until it ends. That loop has two exits besides the user's: the window going
 * away (the editor was closed) and w32_modal_abort(), which the helper's
 * server raises when the host is asking for something only the main loop can
 * answer -- closing the editor, say -- so a dialog left open cannot hold the
 * host waiting. A plug-in whose host has no input pump cannot be interacted
 * with at all, and its modal dialogs are cancelled at once rather than spun on
 * forever. */
#ifndef PELOAD_WIN32DLG_H
#define PELOAD_WIN32DLG_H

#ifndef WM_INITDIALOG
#define WM_INITDIALOG 0x0110
#endif
#define W32_WM_NOTIFY  0x004E
#define W32_WM_HSCROLL 0x0114
#define W32_WM_VSCROLL 0x0115
#define W32_WS_CHILD   0x40000000u
#define W32_WS_POPUP   0x80000000u
#define W32_WS_VISIBLE 0x10000000u
#define W32_WS_GROUP   0x00020000u

/* ---- the trackbar -------------------------------------------------------- */

#define TBM_GETPOS        0x0400
#define TBM_GETRANGEMIN   0x0401
#define TBM_GETRANGEMAX   0x0402
#define TBM_SETTIC        0x0404
#define TBM_SETPOS        0x0405
#define TBM_SETRANGE      0x0406
#define TBM_SETRANGEMIN   0x0407
#define TBM_SETRANGEMAX   0x0408
#define TBM_CLEARTICS     0x0409
#define TBM_SETTICFREQ    0x0414
#define TBM_SETPAGESIZE   0x0415
#define TBM_GETPAGESIZE   0x0416
#define TBM_SETLINESIZE   0x0417
#define TBM_GETLINESIZE   0x0418
#define TBM_GETTHUMBRECT  0x0419
#define TBM_GETCHANNELRECT 0x041A
#define TBS_NOTICKS       0x0010
#define TB_LINEUP 0
#define TB_LINEDOWN 1
#define TB_PAGEUP 2
#define TB_PAGEDOWN 3
#define TB_THUMBPOSITION 4
#define TB_THUMBTRACK 5
#define TB_ENDTRACK 8

#define TRK_THUMB_W 11
#define TRK_THUMB_H 21

static int trk_clamp(w32_wnd *w, int v)
{
    if (v < w->ctl_min) v = w->ctl_min;
    if (v > w->ctl_max) v = w->ctl_max;
    return v;
}

/* Pixels from the left of the window to the thumb's centre at `pos`. */
static int trk_x_of(w32_wnd *w, int pos)
{
    int half = TRK_THUMB_W / 2, span = w->w - 2 * half, range = w->ctl_max - w->ctl_min;
    if (range <= 0 || span <= 0) return half;
    return half + (int)((long long)(pos - w->ctl_min) * span / range);
}

static int trk_pos_at(w32_wnd *w, int x)
{
    int half = TRK_THUMB_W / 2, span = w->w - 2 * half, range = w->ctl_max - w->ctl_min;
    if (range <= 0 || span <= 0) return w->ctl_min;
    x -= half;
    if (x < 0) x = 0;
    if (x > span) x = span;
    return w->ctl_min + (int)(((long long)x * range + span / 2) / span);
}

static void trk_tell_parent(w32_wnd *w, int wi, int code)
{
    w32_wnd *p = (w->parent > 0 && W.wnd[w->parent].used) ? &W.wnd[w->parent] : NULL;
    if (!p) return;
    w32_call(p, (w->style & 2) ? W32_WM_VSCROLL : W32_WM_HSCROLL,
             (W_WPARAM)(((uint32_t)(w->ctl_check & 0xFFFF) << 16) | (uint32_t)code),
             (W_LPARAM)(intptr_t)w32_h(W32_HWND_BASE, wi));
}

static void trk_paint(w32_wnd *w)
{
    w32_surf *t = &w->surf;
    int cy = w->h / 2, tx = trk_x_of(w, w->ctl_check), i;
    ctl_fill(t, 0, 0, w->w, w->h, CLR_FACE);
    ctl_fill(t, TRK_THUMB_W / 2, cy - 2, w->w - TRK_THUMB_W / 2, cy + 2, CLR_WINDOW);
    ctl_bevel(t, TRK_THUMB_W / 2 - 1, cy - 3, w->w - TRK_THUMB_W / 2 + 1, cy + 3, 1);
    if (!(w->style & TBS_NOTICKS)) {
        int range = w->ctl_max - w->ctl_min;
        int step = range > 20 ? range / 10 : 1;
        for (i = w->ctl_min; i <= w->ctl_max && step > 0; i += step) {
            int x = trk_x_of(w, i);
            ctl_fill(t, x, cy + TRK_THUMB_H / 2 + 1, x + 1, cy + TRK_THUMB_H / 2 + 4, CLR_TEXT);
        }
    }
    {
        int l = tx - TRK_THUMB_W / 2, top = cy - TRK_THUMB_H / 2;
        ctl_fill(t, l, top, l + TRK_THUMB_W, top + TRK_THUMB_H, w->enabled ? CLR_FACE : CLR_SHADOW);
        ctl_bevel(t, l, top, l + TRK_THUMB_W, top + TRK_THUMB_H, 0);
    }
}

static MS W_LRESULT ctl_trackbar_proc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled, old;
    W_LRESULT r;

    if (!w) return 0;
    r = ctl_common(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    switch (msg) {
    case WM_CREATE:
        w->ctl_min = 0; w->ctl_max = 100; w->ctl_check = 0; w->ctl_line = 1; w->ctl_page = 0;
        return 0;
    case WM_PAINT: trk_paint(w); w->has_update = 0; return 0;
    case TBM_GETPOS:       return w->ctl_check;
    case TBM_GETRANGEMIN:  return w->ctl_min;
    case TBM_GETRANGEMAX:  return w->ctl_max;
    case TBM_SETPOS:
        w->ctl_check = trk_clamp(w, (int)(int32_t)lp); w->has_update = 1; return 0;
    case TBM_SETRANGE:
        w->ctl_min = (int)(int16_t)(lp & 0xFFFF); w->ctl_max = (int)(int16_t)((lp >> 16) & 0xFFFF);
        if (w->ctl_max < w->ctl_min) w->ctl_max = w->ctl_min;
        w->ctl_check = trk_clamp(w, w->ctl_check); w->has_update = 1; return 0;
    case TBM_SETRANGEMIN:  w->ctl_min = (int)(int32_t)lp; w->ctl_check = trk_clamp(w, w->ctl_check); w->has_update = 1; return 0;
    case TBM_SETRANGEMAX:  w->ctl_max = (int)(int32_t)lp; w->ctl_check = trk_clamp(w, w->ctl_check); w->has_update = 1; return 0;
    case TBM_SETTICFREQ: case TBM_SETTIC: case TBM_CLEARTICS: w->has_update = 1; return 0;
    case TBM_SETPAGESIZE:  old = w->ctl_page; w->ctl_page = (int)(int32_t)lp; return old;
    case TBM_GETPAGESIZE:  return w->ctl_page > 0 ? w->ctl_page : (w->ctl_max - w->ctl_min) / 5;
    case TBM_SETLINESIZE:  old = w->ctl_line; w->ctl_line = (int)(int32_t)lp; return old;
    case TBM_GETLINESIZE:  return w->ctl_line;
    case TBM_GETTHUMBRECT: {
        W32RECT *rc = (W32RECT *)(uintptr_t)lp;
        if (rc) {
            int tx = trk_x_of(w, w->ctl_check), cy = w->h / 2;
            rc->left = tx - TRK_THUMB_W / 2; rc->right = tx + TRK_THUMB_W / 2 + 1;
            rc->top = cy - TRK_THUMB_H / 2;  rc->bottom = cy + TRK_THUMB_H / 2 + 1;
        }
        return 0; }
    case TBM_GETCHANNELRECT: {
        W32RECT *rc = (W32RECT *)(uintptr_t)lp;
        if (rc) { rc->left = TRK_THUMB_W / 2; rc->right = w->w - TRK_THUMB_W / 2;
                  rc->top = w->h / 2 - 2; rc->bottom = w->h / 2 + 2; }
        return 0; }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = (int16_t)(lp & 0xFFFF), tx;
        if (!w->enabled) return 0;
        tx = trk_x_of(w, w->ctl_check);
        W.capture = wi;
        if (x >= tx - TRK_THUMB_W / 2 && x <= tx + TRK_THUMB_W / 2) {
            w->ctl_drag = 1;                        /* on the thumb: drag it */
        } else {                                    /* in the channel: a page that way */
            int page = w->ctl_page > 0 ? w->ctl_page : (w->ctl_max - w->ctl_min) / 5;
            if (page < 1) page = 1;
            w->ctl_check = trk_clamp(w, w->ctl_check + (x > tx ? page : -page));
            w->has_update = 1;
            trk_tell_parent(w, wi, x > tx ? TB_PAGEDOWN : TB_PAGEUP);
            w->ctl_drag = 0;
        }
        return 0; }
    case WM_MOUSEMOVE:
        if (w->ctl_drag && W.capture == wi) {
            int pos = trk_pos_at(w, (int16_t)(lp & 0xFFFF));
            if (pos != w->ctl_check) {
                w->ctl_check = pos; w->has_update = 1;
                trk_tell_parent(w, wi, TB_THUMBTRACK);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (W.capture == wi) W.capture = 0;
        if (w->ctl_drag) {
            w->ctl_drag = 0;
            trk_tell_parent(w, wi, TB_THUMBPOSITION);
        }
        trk_tell_parent(w, wi, TB_ENDTRACK);
        return 0;
    default: return 0;
    }
}

/* ---- dialog templates ---------------------------------------------------- */

typedef struct {
    const uint8_t *base, *end;
    int      ex;                         /* DLGTEMPLATEEX */
    uint32_t style, exstyle;
    int      nitems, x, y, cx, cy;
    char     title[96];
    int      pt;                         /* font size, 0 when the template has none */
    size_t   items;                      /* offset of the first item */
} dlg_tpl;

typedef struct {
    uint32_t style, exstyle;
    int      x, y, cx, cy;
    uint32_t id;
    char     cls[48], text[128];
} dlg_item;

static int dlg_u16(const dlg_tpl *d, size_t o, uint32_t *v)
{
    if (o + 2 > (size_t)(d->end - d->base)) return 0;
    *v = (uint32_t)d->base[o] | ((uint32_t)d->base[o + 1] << 8);
    return 1;
}
static int dlg_u32(const dlg_tpl *d, size_t o, uint32_t *v)
{
    if (o + 4 > (size_t)(d->end - d->base)) return 0;
    *v = (uint32_t)d->base[o] | ((uint32_t)d->base[o + 1] << 8) |
         ((uint32_t)d->base[o + 2] << 16) | ((uint32_t)d->base[o + 3] << 24);
    return 1;
}

/* A string or an ordinal, at `*o`. An ordinal comes back as "#N" -- which is
 * also how a STATIC says which bitmap or icon it shows. -1 on a template that
 * ends inside it. */
static int dlg_name(const dlg_tpl *d, size_t *o, char *out, size_t n)
{
    uint32_t c, v;
    size_t i = 0;
    if (!dlg_u16(d, *o, &c)) return -1;
    if (c == 0xFFFF) {
        if (!dlg_u16(d, *o + 2, &v)) return -1;
        snprintf(out, n, "#%u", v);
        *o += 4;
        return 0;
    }
    if (c == 0) { out[0] = 0; *o += 2; return 0; }
    for (;;) {
        if (!dlg_u16(d, *o, &c)) return -1;
        *o += 2;
        if (!c) break;
        if (i + 1 < n) out[i++] = c < 0x100 ? (char)c : '?';
    }
    out[i] = 0;
    return 0;
}

static int dlg_parse(const uint8_t *t, size_t len, dlg_tpl *d)
{
    uint32_t v, v2, sig;
    size_t o;
    char skip[96];
    memset(d, 0, sizeof *d);
    d->base = t; d->end = t + len;
    if (len < 18) return -1;
    if (!dlg_u16(d, 2, &sig)) return -1;
    d->ex = (sig == 0xFFFF);
    if (d->ex) {
        if (!dlg_u32(d, 8, &d->exstyle) || !dlg_u32(d, 12, &d->style) || !dlg_u16(d, 16, &v)) return -1;
        d->nitems = (int)v;
        if (!dlg_u16(d, 18, &v) || !dlg_u16(d, 20, &v2)) return -1;
        d->x = (int16_t)v; d->y = (int16_t)v2;
        if (!dlg_u16(d, 22, &v) || !dlg_u16(d, 24, &v2)) return -1;
        d->cx = (int16_t)v; d->cy = (int16_t)v2;
        o = 26;
    } else {
        if (!dlg_u32(d, 0, &d->style) || !dlg_u32(d, 4, &d->exstyle) || !dlg_u16(d, 8, &v)) return -1;
        d->nitems = (int)v;
        if (!dlg_u16(d, 10, &v) || !dlg_u16(d, 12, &v2)) return -1;
        d->x = (int16_t)v; d->y = (int16_t)v2;
        if (!dlg_u16(d, 14, &v) || !dlg_u16(d, 16, &v2)) return -1;
        d->cx = (int16_t)v; d->cy = (int16_t)v2;
        o = 18;
    }
    if (dlg_name(d, &o, skip, sizeof skip) || dlg_name(d, &o, skip, sizeof skip) ||
        dlg_name(d, &o, d->title, sizeof d->title)) return -1;
    if (d->style & 0x40) {                                  /* DS_SETFONT */
        if (!dlg_u16(d, o, &v)) return -1;
        d->pt = (int)v;
        o += d->ex ? 6 : 2;
        if (dlg_name(d, &o, skip, sizeof skip)) return -1;
    }
    d->items = o;
    if (d->nitems < 0 || d->nitems > 512) return -1;
    return 0;
}

/* The next item, at `*o`, which is advanced past it. Items sit on 4-byte
 * boundaries counted from the start of the template. */
static int dlg_item_next(const dlg_tpl *d, size_t *o, dlg_item *it)
{
    uint32_t v, v2, ex2, st;
    size_t p = (*o + 3) & ~(size_t)3;
    static const char *const atoms[] = { "BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX" };
    char cn[48];
    memset(it, 0, sizeof *it);
    if (d->ex) {
        if (!dlg_u32(d, p + 4, &ex2) || !dlg_u32(d, p + 8, &st)) return -1;
        it->exstyle = ex2; it->style = st;
        if (!dlg_u16(d, p + 12, &v) || !dlg_u16(d, p + 14, &v2)) return -1;
        it->x = (int16_t)v; it->y = (int16_t)v2;
        if (!dlg_u16(d, p + 16, &v) || !dlg_u16(d, p + 18, &v2)) return -1;
        it->cx = (int16_t)v; it->cy = (int16_t)v2;
        if (!dlg_u32(d, p + 20, &it->id)) return -1;
        p += 24;
    } else {
        if (!dlg_u32(d, p, &st) || !dlg_u32(d, p + 4, &ex2)) return -1;
        it->style = st; it->exstyle = ex2;
        if (!dlg_u16(d, p + 8, &v) || !dlg_u16(d, p + 10, &v2)) return -1;
        it->x = (int16_t)v; it->y = (int16_t)v2;
        if (!dlg_u16(d, p + 12, &v) || !dlg_u16(d, p + 14, &v2)) return -1;
        it->cx = (int16_t)v; it->cy = (int16_t)v2;
        if (!dlg_u16(d, p + 16, &v)) return -1;
        it->id = v;
        p += 18;
    }
    if (dlg_u16(d, p, &v) && v == 0xFFFF) {                 /* a system class by atom */
        if (!dlg_u16(d, p + 2, &v2)) return -1;
        snprintf(it->cls, sizeof it->cls, "%s",
                 (v2 >= 0x80 && v2 <= 0x85) ? atoms[v2 - 0x80] : "STATIC");
        p += 4;
    } else if (dlg_name(d, &p, cn, sizeof cn)) return -1;
    else snprintf(it->cls, sizeof it->cls, "%s", cn);
    if (dlg_name(d, &p, it->text, sizeof it->text)) return -1;
    if (!dlg_u16(d, p, &v)) return -1;
    p += 2 + v;                                             /* creation data, skipped */
    *o = p;
    return 0;
}

/* Dialog units to pixels. A template with a font is laid out for that font's
 * average character; one without uses the system font. 8-point MS Shell Dlg,
 * which is nearly every template, comes to 6 by 13 pixels per base unit pair. */
static void dlg_units(const dlg_tpl *d, int *bx, int *by)
{
    if (d->pt > 0) { *bx = (6 * d->pt + 4) / 8; *by = (13 * d->pt + 4) / 8; }
    else           { *bx = 8; *by = 16; }
}
static int dlg_px(int v, int base, int q) { return (v * base + (v >= 0 ? q / 2 : -q / 2)) / q; }

/* The template of a dialog resource. `name` is an id or a string. */
static int dlg_find_template(void *inst, const void *name, const uint8_t **out, size_t *len)
{
    void *rsrc = res_lookup((const void *)5 /* RT_DIALOG */, name, image_rsrc(inst));
    if (!rsrc) return -1;
    *len = ((RES_DATA *)rsrc)->Size;
    *out = image_base_for_rsrc(rsrc) + ((RES_DATA *)rsrc)->OffsetToData;
    return 0;
}

/* ---- the dialog window --------------------------------------------------- */

/* The text size dialog templates are laid out for: 8-point Tahoma is narrower
 * than the face the layer draws with, which at its default size overruns the
 * boxes the units were worked out for. */
#define DLG_EM 10

#define DLG_EXTRA 32      /* DLGWINDOWEXTRA, rounded up: MSGRESULT, DLGPROC, USER */

typedef MS intptr_t (*w32_dlgproc_fn)(void *, uint32_t, uintptr_t, intptr_t);

/* Hand a message to the plug-in's DLGPROC the way Windows does. DWL_MSGRESULT
 * is cleared first: a page that does not set it means "no", and a stale yes from
 * the last message would make PSN_KILLACTIVE refuse for ever. *handled says
 * whether the procedure returned TRUE. */
static W_LRESULT dlg_call(w32_wnd *w, int wi, uint32_t msg, W_WPARAM wp, W_LPARAM lp, int *handled)
{
    W_LRESULT res = 0;
    *handled = 0;
    if (!w->dlgproc) return 0;
    memset(w->extra, 0, sizeof(W_LRESULT));
    *handled = ((w32_dlgproc_fn)w->dlgproc)(w32_h(W32_HWND_BASE, wi), msg, wp, lp) != 0;
    if (*handled) memcpy(&res, w->extra, sizeof res);
    return res;
}

static MS W_LRESULT w32_dlg_wndproc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0, handled;
    W_LRESULT r;
    if (!w) return 0;
    if (msg == WM_PAINT) {
        if (w->dlgproc) {                               /* the plug-in may paint its own */
            r = dlg_call(w, wi, msg, wp, lp, &handled);
            if (handled) { w->has_update = 0; return r; }
        }
        ctl_fill(&w->surf, 0, 0, w->w, w->h, CLR_FACE);
        w->has_update = 0;
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    r = dlg_call(w, wi, msg, wp, lp, &handled);
    if (handled) return r;
    switch (msg) {
    case WM_COMMAND: {
        int id = (int)(wp & 0xFFFF);
        if (id == 1 || id == 2) {                       /* IDOK, IDCANCEL: end a DialogBox */
            if (w->ctl_drag == 0 && w->dlg_page == -2) { w->ctl_check = id; w->ctl_drag = 1; }
        }
        return 0; }
    case WM_CLOSE:
        if (w->dlg_page == -2) { w->ctl_check = 2; w->ctl_drag = 1; }
        return 0;
    default: return 0;
    }
}

/* Build the dialog a template describes. `parent` is the window it lives in
 * (the host container for one shown below the editor). Returns its window index,
 * or 0. The caller says where it goes; the size is the template's. */
static int dlg_build(const dlg_tpl *d, void *parent, int x, int y, int force_child,
                     void *dlgproc, intptr_t lparam, int *wpx, int *hpx)
{
    int bx, by, pw, ph, wi, k, first = 0;
    size_t o = d->items;
    uint32_t style = d->style;
    void *hdlg;
    dlg_item it;

    dlg_units(d, &bx, &by);
    pw = dlg_px(d->cx, bx, 4); ph = dlg_px(d->cy, by, 8);
    if (wpx) *wpx = pw;
    if (hpx) *hpx = ph;
    style = force_child ? (W32_WS_CHILD | W32_WS_VISIBLE) : (style | W32_WS_VISIBLE);
    hdlg = w32_create_id("#32770", d->title, x, y, pw, ph, parent, NULL, style, d->exstyle, 0);
    if (!hdlg) return 0;
    wi = w32_i(W32_HWND_BASE, hdlg);
    W.wnd[wi].nextra = DLG_EXTRA;
    W.wnd[wi].dlgproc = dlgproc;
    W.wnd[wi].dlg_page = -1;
    memcpy(W.wnd[wi].extra + sizeof(W_LRESULT), &dlgproc, sizeof dlgproc);   /* DWL_DLGPROC */
    for (k = 0; k < d->nitems; k++) {
        void *c;
        if (dlg_item_next(d, &o, &it)) break;
        c = w32_create_id(it.cls, it.text, dlg_px(it.x, bx, 4), dlg_px(it.y, by, 8),
                          dlg_px(it.cx, bx, 4), dlg_px(it.cy, by, 8), hdlg, NULL,
                          it.style, it.exstyle, (int)it.id);
        if (c) W.wnd[w32_i(W32_HWND_BASE, c)].ctl_em = DLG_EM;
        if (c && !first && (it.style & 0x10000) && (it.style & W32_WS_VISIBLE) &&
            strcasecmp(it.cls, "STATIC")) first = w32_i(W32_HWND_BASE, c);
    }
    {   /* WM_INITDIALOG: the control that would take focus, and the creation
         * parameter -- for a property-sheet page, the PROPSHEETPAGE itself. */
        int handled;
        (void)dlg_call(&W.wnd[wi], wi, WM_INITDIALOG,
                       (W_WPARAM)(uintptr_t)(first ? w32_h(W32_HWND_BASE, first) : NULL),
                       (W_LPARAM)lparam, &handled);
    }
    W.wnd[wi].has_update = 1;
    return wi;
}

/* A control with a picture in it. STATIC with SS_BITMAP names its bitmap
 * resource in its text as "#id"; it is drawn at its top left, once loaded. */
/* An icon from the plug-in's own resources: the group names the images, the
 * one nearest 32 pixels is read, and what its AND mask marks as outside the
 * shape (or its alpha, at 32 bits) shows the dialog behind. */
static void dlg_draw_icon(w32_wnd *w, int id)
{
    void *g = res_lookup((const void *)14 /* RT_GROUP_ICON */, (const void *)(uintptr_t)id, image_rsrc(NULL));
    const uint8_t *gd, *e, *best = NULL, *ic;
    void *ir;
    int n, i, bw = 0, bits, ncol, h2, iw, ih, x, y;
    uint32_t pal[256], clr_used;
    size_t xs, as, off;
    if (!g) return;
    gd = image_base_for_rsrc(g) + ((RES_DATA *)g)->OffsetToData;
    n = gd[4] | (gd[5] << 8);
    for (i = 0, e = gd + 6; i < n && i < 32; i++, e += 14) {
        int ew = e[0] ? e[0] : 256;
        if (!best || (ew <= 32 && ew > bw) || (bw > 32 && ew < bw)) { best = e; bw = ew; }
    }
    if (!best) return;
    ir = res_lookup((const void *)3 /* RT_ICON */, (const void *)(uintptr_t)(best[12] | (best[13] << 8)),
                    image_rsrc(NULL));
    if (!ir) return;
    ic = image_base_for_rsrc(ir) + ((RES_DATA *)ir)->OffsetToData;
    if (((RES_DATA *)ir)->Size < 40) return;
    iw = (int)(ic[4] | (ic[5] << 8) | (ic[6] << 16) | (ic[7] << 24));
    h2 = (int)(ic[8] | (ic[9] << 8) | (ic[10] << 16) | (ic[11] << 24));
    bits = ic[14] | (ic[15] << 8);
    clr_used = (uint32_t)(ic[32] | (ic[33] << 8) | (ic[34] << 16) | (ic[35] << 24));
    ih = h2 / 2;
    if (iw <= 0 || iw > 256 || ih <= 0 || ih > 256) return;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) return;
    ncol = bits <= 8 ? (clr_used ? (int)clr_used : 1 << bits) : 0;
    if (ncol > 256) return;
    off = 40;
    for (i = 0; i < ncol; i++, off += 4)
        pal[i] = ((uint32_t)ic[off + 2] << 16) | ((uint32_t)ic[off + 1] << 8) | ic[off];
    xs = (((size_t)iw * bits + 31) / 32) * 4;
    as = (((size_t)iw + 31) / 32) * 4;
    if (off + xs * ih + as * ih > ((RES_DATA *)ir)->Size) return;
    for (y = 0; y < ih && y < w->h; y++) {
        const uint8_t *row = ic + off + xs * (size_t)(ih - 1 - y);
        const uint8_t *msk = ic + off + xs * ih + as * (size_t)(ih - 1 - y);
        for (x = 0; x < iw && x < w->w; x++) {
            uint32_t c; int alpha = 255;
            if (bits == 32) {
                c = ((uint32_t)row[x * 4 + 2] << 16) | ((uint32_t)row[x * 4 + 1] << 8) | row[x * 4];
                alpha = row[x * 4 + 3];
                if (!alpha && (msk[x >> 3] & (0x80 >> (x & 7)))) continue;
                if (!alpha) alpha = 255;
            } else {
                if (msk[x >> 3] & (0x80 >> (x & 7))) continue;          /* outside the shape */
                if (bits == 24)      c = ((uint32_t)row[x * 3 + 2] << 16) | ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3];
                else if (bits == 8)  c = pal[row[x]];
                else if (bits == 4)  c = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
                else                 c = pal[(row[x >> 3] >> (7 - (x & 7))) & 1];
            }
            if (alpha < 255) {                       /* over the dialog face */
                uint32_t f = CLR_FACE, r, gg, b;
                r  = (((c >> 16) & 255) * alpha + ((f >> 16) & 255) * (255 - alpha)) / 255;
                gg = (((c >> 8) & 255) * alpha + ((f >> 8) & 255) * (255 - alpha)) / 255;
                b  = ((c & 255) * alpha + (f & 255) * (255 - alpha)) / 255;
                c = (r << 16) | (gg << 8) | b;
            }
            w->surf.px[(size_t)y * w->surf.w + x] = c;
        }
    }
}

static int w32_static_bitmap(w32_wnd *w, int wi)
{
    void *bm;
    w32_obj *o;
    int x, y;
    (void)wi;
    ctl_fill(&w->surf, 0, 0, w->w, w->h, CLR_FACE);
    if (w->text[0] != '#') return 1;
    if ((w->style & 0x1F) == 3) {                  /* SS_ICON */
        dlg_draw_icon(w, atoi(w->text + 1));
        return 1;
    }
    if (!w->ctl_line) {                         /* loaded once, kept as a handle */
        bm = load_bitmap_res((const void *)(uintptr_t)atoi(w->text + 1));
        w->ctl_line = bm ? (int)(uintptr_t)bm : -1;
    }
    if (w->ctl_line < 0) return 1;
    bm = (void *)(uintptr_t)w->ctl_line;
    o = w32_oget(bm);
    if (!o || !o->px) return 1;
    for (y = 0; y < o->h && y < w->h; y++)
        for (x = 0; x < o->w && x < w->w; x++)
            w->surf.px[(size_t)y * w->surf.w + x] = o->px[(size_t)y * o->w + x];
    return 1;
}

/* ---- where a dialog is shown --------------------------------------------- */

/* The container a dialog is parented to, and where under the editor it goes. */
static void *dlg_root(void)
{
    if (W.host && W.wnd[W.host].used) return w32_h(W32_HWND_BASE, W.host);
    return NULL;
}
static int dlg_below_editor(void)
{
    int d = W.display ? W.display : W.host;
    return (d && W.wnd[d].used) ? W.wnd[d].h : 0;
}

/* ---- when a dialog may be shown ------------------------------------------ */

/* Only when the user's own click or key brought it on. A plug-in also puts up
 * dialogs of its own accord -- a licence prompt or a warning, from inside
 * effEditOpen or an idle timer -- and with nobody there to answer, a modal one
 * would hold the editor from ever opening. Those get what they always got here:
 * the call returns at once, as if dismissed (0), and the plug-in goes on.
 * NI Massive opens its editor that way. */
static int dlg_allowed(void)
{
    /* Inside the user's click, or just after it: a plug-in may open its menu a
     * moment later than the click that asked for it (iPlug2 on macOS does). */
    if (W.user_input > 0) return 1;
    if (W.last_click_ms > 0 && w32_now_ms() - W.last_click_ms < 1500.0) return 1;
    PLOG("  [w32] a dialog nobody asked for: dismissed (user_input=%d)\n", W.user_input);
    return 0;
}

/* ---- modal loops --------------------------------------------------------- */

void w32_modal_abort(void) { W.modal_abort = 1; }

/* Spin until `*done`, the window `widx` goes, or the host asks for the plug-in
 * back. 1 if it ended by itself, 0 if cancelled from outside. Called from inside
 * the plug-in's own window procedure, which is where Windows would be too. */
static int w32_modal_wait(int widx, volatile int *done)
{
    int ok;
    if (!W.hooks.pump_input) {
        PLOG("  [w32] modal dialog with no input pump: cancelled\n");
        return 0;
    }
    W.modal_abort = 0;
    while (!*done && widx > 0 && W.wnd[widx].used && !W.modal_abort) {
        w32_pump_input();
        w32_pump();
        usleep(3000);
    }
    ok = !W.modal_abort && widx > 0 && W.wnd[widx].used;
    W.modal_abort = 0;
    return ok;
}

/* Take a dialog window and everything in it out of the tree. */
static void dlg_destroy_tree(int idx)
{
    int i;
    if (idx <= 0 || idx >= W32_MAX_WND || !W.wnd[idx].used) return;
    for (i = 1; i < W32_MAX_WND; i++)
        if (W.wnd[i].used && W.wnd[i].parent == idx) dlg_destroy_tree(i);
    if (!W.wnd[idx].used) return;
    w32_call(&W.wnd[idx], WM_DESTROY, 0, 0);
    if (W.sheet == idx) W.sheet = 0;
    if (W.focus == idx) W.focus = 0;
    if (W.capture == idx) W.capture = 0;
    w32_surf_free(&W.wnd[idx].surf);
    free(W.wnd[idx].items);
    memset(&W.wnd[idx], 0, sizeof W.wnd[idx]);
}

/* ---- property sheets ----------------------------------------------------- */

static MS int32_t st_LoadStringA(void *inst, uint32_t id, char *buf, int32_t max);   /* below */

/* A caption or title that is a pointer, or -- with its high word zero -- the id
 * of a string in the plug-in's own string table, the way MAKEINTRESOURCE
 * passes one. S-YXG50's sheet caption is the number 2. */
static const char *dlg_text_arg(void *inst, const char *s, char *buf, size_t n)
{
    if (!s) return "";
    if ((uintptr_t)s < 0x10000) {
        buf[0] = 0;
        st_LoadStringA(inst, (uint32_t)(uintptr_t)s, buf, (int32_t)n);
        return buf;
    }
    return s;
}

#define PSN_SETACTIVE   (-200)
#define PSN_KILLACTIVE  (-201)
#define PSN_APPLY       (-202)
#define PSN_RESET       (-203)
#define PSM_SETCURSEL   0x0465
#define PSM_CHANGED     0x0468
#define PSM_UNCHANGED   0x046D
#define PSM_APPLY       0x046E
#define PSM_PRESSBUTTON 0x0471
#define PSM_CANCELTOCLOSE 0x046B
#define PSM_GETTABCONTROL 0x0474
#define PSM_ISDIALOGMESSAGE 0x0475
#define PSH_PROPSHEETPAGE 0x0008
#define PSH_NOAPPLYNOW    0x0080
#define PSH_USECALLBACK   0x0100
#define PSP_DLGINDIRECT   0x0001
#define PSP_USETITLE      0x0008
#define PSB_OK 1
#define PSB_CANCEL 2
#define PSB_APPLYNOW 3
#define IDAPPLYNOW 0x3021

typedef struct {
    uint32_t dwSize, dwFlags;
    void *hInstance;
    const void *pszTemplate;               /* or pResource */
    void *hIcon;
    const char *pszTitle;
    void *pfnDlgProc;
    intptr_t lParam;
    void *pfnCallback;
    uint32_t *pcRefParent;
} w32_psp;

typedef struct {
    uint32_t dwSize, dwFlags;
    void *hwndParent, *hInstance, *hIcon;
    const char *pszCaption;
    uint32_t nPages;
    uintptr_t startPage;                   /* nStartPage or pStartPage */
    const void *ppsp;                      /* pages, or handles from CreatePropertySheetPage */
    void *pfnCallback;
} w32_psh;

typedef struct { void *hwndFrom; uintptr_t idFrom; int32_t code; intptr_t lParam; } w32_pshnotify;

#define SH_MAX_PAGES 16
#define SH_M   6                           /* margin around the tabs and buttons */
#define SH_TABH 21
#define SH_BTNW 75
#define SH_BTNH 23

static struct {
    int used, wnd, done, result, npages, cur, changed;
    int pw, ph, px0, py0;
    int apply_btn;
    int tabx[SH_MAX_PAGES + 1];
    struct {
        dlg_tpl tpl;
        uint8_t psp[sizeof(w32_psp) + 64];  /* a copy: the plug-in's may be a local */
        void   *dlgproc;
        char    title[64];
        int     dlg;                        /* the page's window once it has been shown */
    } page[SH_MAX_PAGES];
} SH;

static void sheet_notify(int page, int code, intptr_t lparam, W_LRESULT *res)
{
    w32_pshnotify n;
    w32_wnd *d;
    int wi = SH.page[page].dlg;
    if (!wi || !W.wnd[wi].used) { if (res) *res = 0; return; }
    d = &W.wnd[wi];
    n.hwndFrom = w32_h(W32_HWND_BASE, SH.wnd);
    n.idFrom = 0; n.code = code; n.lParam = lparam;
    {
        W_LRESULT r = w32_call(d, W32_WM_NOTIFY, 0, (W_LPARAM)(uintptr_t)&n);
        if (res) *res = r;
    }
}

/* Show page `i`: leave the one on show (which may refuse), make the page's
 * dialog the first time it is wanted, and tell it it is now in front. */
static void sheet_select(int i)
{
    W_LRESULT refuse = 0;
    if (i < 0 || i >= SH.npages) return;
    if (i == SH.cur && SH.page[i].dlg) return;
    if (SH.cur >= 0 && SH.page[SH.cur].dlg) {
        sheet_notify(SH.cur, PSN_KILLACTIVE, 0, &refuse);
        if (refuse) return;
        W.wnd[SH.page[SH.cur].dlg].visible = 0;
    }
    SH.cur = i;
    if (!SH.page[i].dlg) {
        int pw, ph;
        SH.page[i].dlg = dlg_build(&SH.page[i].tpl, w32_h(W32_HWND_BASE, SH.wnd), SH.px0, SH.py0, 1,
                                   SH.page[i].dlgproc, (intptr_t)SH.page[i].psp, &pw, &ph);
    }
    if (SH.page[i].dlg) {
        W.wnd[SH.page[i].dlg].visible = 1;
        W.wnd[SH.page[i].dlg].has_update = 1;
        sheet_notify(i, PSN_SETACTIVE, 0, NULL);
    }
    if (SH.wnd) W.wnd[SH.wnd].has_update = 1;
}

static void sheet_finish(int result)
{
    SH.result = result;
    SH.done = 1;
}

/* OK and Apply: every page that was shown is asked to commit. 1 if they all
 * did; 0 if a page refused (PSN_KILLACTIVE answered TRUE), in which case it is
 * brought to the front and nothing is applied. */
static int sheet_apply(int closing)
{
    int i;
    for (i = 0; i < SH.npages; i++) {
        W_LRESULT refuse = 0;
        if (!SH.page[i].dlg) continue;
        sheet_notify(i, PSN_KILLACTIVE, 0, &refuse);
        if (refuse) { sheet_select(i); return 0; }
    }
    for (i = 0; i < SH.npages; i++)
        if (SH.page[i].dlg) sheet_notify(i, PSN_APPLY, closing ? 1 : 0, NULL);
    SH.changed = 0;
    if (SH.apply_btn) { W.wnd[SH.apply_btn].enabled = 0; W.wnd[SH.apply_btn].has_update = 1; }
    return 1;
}

static void sheet_cancel(void)
{
    int i;
    for (i = 0; i < SH.npages; i++)
        if (SH.page[i].dlg) sheet_notify(i, PSN_RESET, 1, NULL);
    sheet_finish(0);
}

static void sheet_paint(w32_wnd *w, int wi)
{
    w32_surf *t = &w->surf;
    int i, frame_top = SH_M + SH_TABH - 1;
    ctl_fill(t, 0, 0, w->w, w->h, CLR_FACE);
    g_ctl_em = DLG_EM;
    /* The page's frame, then the tabs over its top edge. */
    ctl_bevel(t, SH.tabx[0], frame_top, SH.tabx[0] + SH.pw + 6, frame_top + SH.ph + 6, 0);
    for (i = 0; i < SH.npages; i++) {
        int x0 = SH.tabx[i], x1 = SH.tabx[i + 1];
        int act = (i == SH.cur), top = SH_M - (act ? 2 : 0), bot = frame_top + 1;
        ctl_fill(t, x0, top, x1, bot, CLR_FACE);
        ctl_fill(t, x0, top + 1, x1 - 1, top + 2, CLR_HILIGHT);          /* top */
        ctl_fill(t, x0, top + 1, x0 + 1, bot, CLR_HILIGHT);              /* left */
        ctl_fill(t, x1 - 1, top + 1, x1, bot, CLR_DKSHADOW);             /* right */
        ctl_fill(t, x1 - 2, top + 2, x1 - 1, bot, CLR_SHADOW);
        if (act) ctl_fill(t, x0 + 1, frame_top, x1 - 1, frame_top + 2, CLR_FACE);
        ctl_text(wi, x0 + 8, top + 4, SH.page[i].title, CLR_TEXT);
    }
    g_ctl_em = 0;
    w->has_update = 0;
}

static MS W_LRESULT w32_sheet_wndproc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0;
    if (!w || !SH.used || wi != SH.wnd) return 0;
    switch (msg) {
    case WM_PAINT:  sheet_paint(w, wi); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        int x = (int16_t)(lp & 0xFFFF), y = (int16_t)((lp >> 16) & 0xFFFF), i;
        if (y >= SH_M - 2 && y < SH_M + SH_TABH)
            for (i = 0; i < SH.npages; i++)
                if (x >= SH.tabx[i] && x < SH.tabx[i + 1]) { sheet_select(i); break; }
        return 0; }
    case WM_COMMAND: {
        int id = (int)(wp & 0xFFFF);
        if (id == 1) { if (sheet_apply(1)) sheet_finish(1); }
        else if (id == 2) sheet_cancel();
        else if (id == IDAPPLYNOW) sheet_apply(0);
        return 0; }
    case WM_CLOSE: sheet_cancel(); return 0;
    case PSM_CHANGED:
        SH.changed = 1;
        if (SH.apply_btn) { W.wnd[SH.apply_btn].enabled = 1; W.wnd[SH.apply_btn].has_update = 1; }
        return 0;
    case PSM_UNCHANGED:
        if (SH.apply_btn) { W.wnd[SH.apply_btn].enabled = 0; W.wnd[SH.apply_btn].has_update = 1; }
        return 0;
    case PSM_APPLY:  sheet_apply(0); return 1;
    case PSM_SETCURSEL: sheet_select((int)wp); return 1;
    case PSM_CANCELTOCLOSE: return 0;
    case PSM_PRESSBUTTON:
        if (wp == PSB_OK) { if (sheet_apply(1)) sheet_finish(1); }
        else if (wp == PSB_CANCEL) sheet_cancel();
        else if (wp == PSB_APPLYNOW) sheet_apply(0);
        return 1;
    case PSM_GETTABCONTROL: return 0;
    case PSM_ISDIALOGMESSAGE: return 0;
    default: return 0;
    }
}

/* Escape cancels and Return confirms, as on a real sheet. Anything else goes on
 * to the editor. */
static int w32_sheet_key(int vk, int down, int ch)
{
    (void)ch;
    if (!SH.used || !W.sheet || W.sheet != SH.wnd) return 0;
    if (!down) return 0;
    if (vk == 27) { sheet_cancel(); return 1; }
    if (vk == 13) { if (sheet_apply(1)) sheet_finish(1); return 1; }
    return 0;
}

/* A dialog or sheet window going away by DestroyWindow: its controls and pages
 * go with it, and if it was the one under the editor, the editor has the room
 * back. Only ours -- an ordinary window's children are the plug-in's to free. */
static void w32_sheet_destroyed(int idx)
{
    int i;
    if (idx <= 0 || idx >= W32_MAX_WND || !W.wnd[idx].used) return;
    if (!strcasecmp(W.wnd[idx].cls, "#32770") || !strcasecmp(W.wnd[idx].cls, "#peloadsheet"))
        for (i = 1; i < W32_MAX_WND; i++)
            if (W.wnd[i].used && W.wnd[i].parent == idx) dlg_destroy_tree(i);
    if (W.sheet == idx) { W.sheet = 0; if (SH.wnd == idx) SH.used = 0; }
}

static intptr_t sheet_run(const w32_psh *h, const w32_psp *const *pages, int np)
{
    int i, pw = 0, ph = 0, bx, by, sw, sh_h, btn_y, nbtn = 3, x, xoff;
    char cap[96];
    void *root, *hs;
    (void)bx; (void)by;

    if (SH.used || W.sheet || np < 1 || np > SH_MAX_PAGES) return -1;
    memset(&SH, 0, sizeof SH);
    for (i = 0; i < np; i++) {
        const w32_psp *p = pages[i];
        const uint8_t *tpl = NULL;
        size_t len = 0;
        int cx, cy, qx, qy;
        if (!p) return -1;
        if (p->dwFlags & PSP_DLGINDIRECT) { tpl = (const uint8_t *)p->pszTemplate; len = 16384; }
        else if (dlg_find_template(p->hInstance, p->pszTemplate, &tpl, &len)) {
            PLOG("  [w32] property sheet page %d: no such dialog template\n", i);
            memset(&SH, 0, sizeof SH);
            return -1;
        }
        if (!tpl || dlg_parse(tpl, len, &SH.page[i].tpl)) { memset(&SH, 0, sizeof SH); return -1; }
        memcpy(SH.page[i].psp, p, p->dwSize < sizeof SH.page[i].psp ? p->dwSize : sizeof SH.page[i].psp);
        SH.page[i].dlgproc = p->pfnDlgProc;
        {
            char tb[64];
            snprintf(SH.page[i].title, sizeof SH.page[i].title, "%s",
                     ((p->dwFlags & PSP_USETITLE) && p->pszTitle)
                         ? dlg_text_arg(p->hInstance, p->pszTitle, tb, sizeof tb)
                         : SH.page[i].tpl.title);
        }
        if (!SH.page[i].title[0]) snprintf(SH.page[i].title, sizeof SH.page[i].title, "Page %d", i + 1);
        dlg_units(&SH.page[i].tpl, &qx, &qy);
        cx = dlg_px(SH.page[i].tpl.cx, qx, 4); cy = dlg_px(SH.page[i].tpl.cy, qy, 8);
        if (cx > pw) pw = cx;
        if (cy > ph) ph = cy;
    }
    if (h->dwFlags & PSH_NOAPPLYNOW) nbtn = 2;
    SH.npages = np; SH.pw = pw; SH.ph = ph;
    SH.py0 = SH_M + SH_TABH + 2;
    sw = pw + 2 * (SH_M + 3);
    if (sw < nbtn * (SH_BTNW + 6) + 2 * SH_M) sw = nbtn * (SH_BTNW + 6) + 2 * SH_M;
    {   /* As wide as the editor above it, the pages centred in that. */
        int d = W.display ? W.display : W.host;
        if (d && W.wnd[d].used && W.wnd[d].w > sw) sw = W.wnd[d].w;
    }
    xoff = (sw - (pw + 2 * (SH_M + 3))) / 2;
    SH.px0 = SH_M + 3 + xoff;
    btn_y = SH.py0 + ph + 3 + 10;
    sh_h = btn_y + SH_BTNH + SH_M;
    /* The tabs: each as wide as its caption wants. */
    x = SH_M + xoff;
    g_ctl_em = DLG_EM;
    for (i = 0; i < np; i++) {
        SH.tabx[i] = x;
        x += ctl_text_w(SH.page[i].title) + 18;
    }
    g_ctl_em = 0;
    SH.tabx[np] = x;

    root = dlg_root();
    hs = w32_create_id("#peloadsheet", dlg_text_arg(h->hInstance, h->pszCaption, cap, sizeof cap),
                       0, dlg_below_editor(),
                       sw, sh_h, root, NULL, W32_WS_CHILD | W32_WS_VISIBLE, 0, 0);
    if (!hs) { memset(&SH, 0, sizeof SH); return -1; }
    SH.wnd = w32_i(W32_HWND_BASE, hs);
    SH.used = 1;
    SH.cur = -1;
    W.sheet = SH.wnd;
    {   /* OK, Cancel, and Apply unless asked not to. */
        int bxp = sw - SH_M - nbtn * (SH_BTNW + 6) + 6;
        void *b;
        b = w32_create_id("BUTTON", "OK", bxp, btn_y, SH_BTNW, SH_BTNH, hs, NULL,
                          W32_WS_CHILD | W32_WS_VISIBLE | 1, 0, 1);
        (void)b;
        b = w32_create_id("BUTTON", "Cancel", bxp + SH_BTNW + 6, btn_y, SH_BTNW, SH_BTNH, hs, NULL,
                          W32_WS_CHILD | W32_WS_VISIBLE, 0, 2);
        (void)b;
        if (nbtn == 3) {
            b = w32_create_id("BUTTON", "Apply", bxp + 2 * (SH_BTNW + 6), btn_y, SH_BTNW, SH_BTNH, hs,
                              NULL, W32_WS_CHILD | W32_WS_VISIBLE, 0, IDAPPLYNOW);
            SH.apply_btn = b ? w32_i(W32_HWND_BASE, b) : 0;
            if (SH.apply_btn) W.wnd[SH.apply_btn].enabled = 0;
        }
    }
    {   /* The start page: a number, or with PSH_USEPSTARTPAGE a caption we do not match. */
        int start = (int)(h->startPage < (uintptr_t)np ? h->startPage : 0);
        sheet_select(start);
    }
    if ((h->dwFlags & PSH_USECALLBACK) && h->pfnCallback) {
        typedef MS int (*psh_cb)(void *, uint32_t, intptr_t);
        ((psh_cb)h->pfnCallback)(hs, 1 /* PSCB_INITIALIZED */, 0);
    }
    PLOG("  [w32] property sheet: %d page(s), %dx%d, modal\n", np, sw, sh_h);

    {
        int by_itself = w32_modal_wait(SH.wnd, &SH.done);
        PLOG("  [w32] property sheet over: %s, result %d\n",
             by_itself ? "closed by the user" : "cancelled from outside",
             SH.done ? SH.result : 0);
    }
    i = SH.done ? SH.result : 0;
    {   /* Gone: the pages are told, then the windows are. */
        int wi = SH.wnd;
        for (x = 0; x < SH.npages; x++)
            if (SH.page[x].dlg && W.wnd[SH.page[x].dlg].used) dlg_destroy_tree(SH.page[x].dlg);
        memset(&SH, 0, sizeof SH);
        dlg_destroy_tree(wi);
        W.sheet = 0;
    }
    return i;
}

static MS intptr_t st_PropertySheetA(const void *hdr)
{
    const w32_psh *h = (const w32_psh *)hdr;
    const w32_psp *pages[SH_MAX_PAGES];
    int i, np;
    if (!h || h->dwSize < 28 || !h->ppsp) return -1;
    if (!dlg_allowed()) return 0;
    PLOG("  [w32] PropertySheetA size=%u flags=%#x caption=%p pages=%u start=%#lx ppsp=%p cb=%p\n",
         h->dwSize, h->dwFlags, (const void *)h->pszCaption, h->nPages,
         (unsigned long)h->startPage, h->ppsp, h->pfnCallback);
    np = (int)h->nPages;
    if (np < 1 || np > SH_MAX_PAGES) return -1;
    if (h->dwFlags & PSH_PROPSHEETPAGE) {                     /* an array of PROPSHEETPAGE */
        const uint8_t *p = (const uint8_t *)h->ppsp;
        for (i = 0; i < np; i++) {
            const w32_psp *sp = (const w32_psp *)p;
            pages[i] = sp;
            if (sp->dwSize < 40) return -1;
            p += sp->dwSize;
        }
    } else {                                                   /* HPROPSHEETPAGEs */
        const w32_psp *const *hp = (const w32_psp *const *)h->ppsp;
        for (i = 0; i < np; i++) pages[i] = hp[i];
    }
    return sheet_run(h, pages, np);
}

/* CreatePropertySheetPageA: a handle that is a copy of the page, which is all
 * PropertySheet needs from it. */
static MS void *st_CreatePropertySheetPageA(const void *psp)
{
    const w32_psp *p = (const w32_psp *)psp;
    void *copy;
    if (!p || p->dwSize < 40 || p->dwSize > 256) return NULL;
    copy = malloc(p->dwSize);
    if (copy) memcpy(copy, p, p->dwSize);
    return copy;
}
static MS int32_t st_DestroyPropertySheetPage(void *h) { free(h); return 1; }
static MS void st_InitCommonControls(void) { }

/* ---- the DialogBox and CreateDialog family -------------------------------- */

static void *dlg_name_arg(const void *name, int wide, char *buf, size_t n)
{
    if ((uintptr_t)name < 0x10000) return (void *)name;
    if (wide) { w2c((const uint16_t *)name, buf, n); return buf; }
    return (void *)name;
}

/* The dialog under the editor, or a child of the window the plug-in names. */
static int dlg_create(void *inst, const uint8_t *tpl, size_t len, void *parent,
                      void *proc, intptr_t lparam, int modal)
{
    dlg_tpl d;
    int child, wi, w, h;
    void *where;
    if (!tpl || dlg_parse(tpl, len, &d)) return 0;
    if (!dlg_allowed()) return 0;
    child = (d.style & W32_WS_CHILD) && w32_wget(parent);
    if (!child && W.sheet) return 0;                  /* one thing under the editor at a time */
    where = child ? parent : dlg_root();
    {
        int bx, by;
        dlg_units(&d, &bx, &by);
        wi = dlg_build(&d, where, child ? dlg_px(d.x, bx, 4) : 0,
                       child ? dlg_px(d.y, by, 8) : dlg_below_editor(), 0, proc, lparam, &w, &h);
    }
    (void)inst;
    if (!wi) return 0;
    if (!(d.style & W32_WS_VISIBLE)) W.wnd[wi].visible = 0;
    if (!child) W.sheet = wi;
    if (modal) W.wnd[wi].dlg_page = -2;               /* DialogBox: EndDialog ends it */
    return wi;
}

static MS void *st_CreateDialogParamA(void *inst, const char *name, void *parent, void *proc, intptr_t lp)
{
    const uint8_t *tpl; size_t len; int wi;
    if (dlg_find_template(inst, name, &tpl, &len)) return NULL;
    wi = dlg_create(inst, tpl, len, parent, proc, lp, 0);
    return wi ? w32_h(W32_HWND_BASE, wi) : NULL;
}
static MS void *st_CreateDialogParamW(void *inst, const void *name, void *parent, void *proc, intptr_t lp)
{
    char b[128];
    return st_CreateDialogParamA(inst, (const char *)dlg_name_arg(name, 1, b, sizeof b), parent, proc, lp);
}
static MS void *st_CreateDialogIndirectParamA(void *inst, const void *tpl, void *parent, void *proc, intptr_t lp)
{
    int wi = dlg_create(inst, (const uint8_t *)tpl, 16384, parent, proc, lp, 0);
    return wi ? w32_h(W32_HWND_BASE, wi) : NULL;
}
static MS void *st_CreateDialogIndirectParamW(void *inst, const void *tpl, void *parent, void *proc, intptr_t lp)
{ return st_CreateDialogIndirectParamA(inst, tpl, parent, proc, lp); }

static intptr_t dlg_run_modal(int wi)
{
    intptr_t res;
    if (!wi) return -1;
    (void)w32_modal_wait(wi, &W.wnd[wi].ctl_drag);
    res = (W.wnd[wi].used && W.wnd[wi].ctl_drag) ? (intptr_t)W.wnd[wi].ctl_check : 0;
    dlg_destroy_tree(wi);
    return res;
}
static MS intptr_t st_DialogBoxParamA(void *inst, const char *name, void *parent, void *proc, intptr_t lp)
{
    const uint8_t *tpl; size_t len;
    if (dlg_find_template(inst, name, &tpl, &len)) return -1;
    return dlg_run_modal(dlg_create(inst, tpl, len, parent, proc, lp, 1));
}
static MS intptr_t st_DialogBoxParamW(void *inst, const void *name, void *parent, void *proc, intptr_t lp)
{
    char b[128];
    return st_DialogBoxParamA(inst, (const char *)dlg_name_arg(name, 1, b, sizeof b), parent, proc, lp);
}
static MS intptr_t st_DialogBoxIndirectParamA(void *inst, const void *tpl, void *parent, void *proc, intptr_t lp)
{ return dlg_run_modal(dlg_create(inst, (const uint8_t *)tpl, 16384, parent, proc, lp, 1)); }
static MS intptr_t st_DialogBoxIndirectParamW(void *inst, const void *tpl, void *parent, void *proc, intptr_t lp)
{ return st_DialogBoxIndirectParamA(inst, tpl, parent, proc, lp); }

static MS int32_t st_EndDialog(void *hdlg, intptr_t result)
{
    w32_wnd *w = w32_wget(hdlg);
    if (!w) return 0;
    w->ctl_check = (int)result;
    w->ctl_drag = 1;
    return 1;
}

/* The control calls a DLGPROC makes on its own dialog. */
static MS int32_t st_CheckDlgButton(void *hdlg, int32_t id, uint32_t state)
{
    void *c = st_GetDlgItem(hdlg, id);
    if (!c) return 0;
    st_SendMessageA(c, BM_SETCHECK, state, 0);
    return 1;
}
static MS uint32_t st_IsDlgButtonChecked(void *hdlg, int32_t id)
{
    void *c = st_GetDlgItem(hdlg, id);
    return c ? (uint32_t)st_SendMessageA(c, BM_GETCHECK, 0, 0) : 0;
}
static MS int32_t st_CheckRadioButton(void *hdlg, int32_t first, int32_t last, int32_t check)
{
    int32_t id;
    for (id = first; id <= last; id++) {
        void *c = st_GetDlgItem(hdlg, id);
        if (c) st_SendMessageA(c, BM_SETCHECK, id == check, 0);
    }
    return 1;
}
static MS int32_t st_SetDlgItemTextA(void *hdlg, int32_t id, const char *s)
{
    void *c = st_GetDlgItem(hdlg, id);
    if (!c) return 0;
    st_SendMessageA(c, WM_SETTEXT, 0, (intptr_t)(uintptr_t)(s ? s : ""));
    return 1;
}
static MS int32_t st_SetDlgItemTextW(void *hdlg, int32_t id, const uint16_t *s)
{
    char b[256]; size_t i = 0;
    if (s) for (; s[i] && i + 1 < sizeof b; i++) b[i] = s[i] < 0x100 ? (char)s[i] : '?';
    b[i] = 0;
    return st_SetDlgItemTextA(hdlg, id, b);
}
static MS uint32_t st_GetDlgItemTextA(void *hdlg, int32_t id, char *out, int32_t n)
{
    void *c = st_GetDlgItem(hdlg, id);
    if (!out || n <= 0) return 0;
    out[0] = 0;
    return c ? (uint32_t)st_SendMessageA(c, WM_GETTEXT, (uintptr_t)n, (intptr_t)(uintptr_t)out) : 0;
}
static MS int32_t st_SetDlgItemInt(void *hdlg, int32_t id, uint32_t v, int32_t is_signed)
{
    char b[24];
    if (is_signed) snprintf(b, sizeof b, "%d", (int)v);
    else           snprintf(b, sizeof b, "%u", (unsigned)v);
    return st_SetDlgItemTextA(hdlg, id, b);
}
static MS uint32_t st_GetDlgItemInt(void *hdlg, int32_t id, int32_t *ok, int32_t is_signed)
{
    char b[32];
    long v;
    st_GetDlgItemTextA(hdlg, id, b, sizeof b);
    v = is_signed ? strtol(b, NULL, 10) : (long)strtoul(b, NULL, 10);
    if (ok) *ok = b[0] != 0;
    return (uint32_t)v;
}
static MS intptr_t st_DefDlgProcA(void *hdlg, uint32_t msg, uintptr_t wp, intptr_t lp)
{ (void)hdlg; (void)msg; (void)wp; (void)lp; return 0; }
static MS int32_t st_IsDialogMessageA(void *hdlg, const void *m)
{ (void)hdlg; (void)m; return 0; }


/* ---- menus ----------------------------------------------------------------
 *
 * A menu is a list of items and nothing else until TrackPopupMenu shows it.
 * The earlier version kept only a count, which was enough while no menu was
 * ever shown; showing one needs the text, the command ids, the check marks, the
 * greyed items, the separators and the submenus, and the functions that edit
 * them need to agree with what TrackPopupMenu will draw. */

#ifndef MF_GRAYED
#define MF_GRAYED       0x0001
#define MF_DISABLED     0x0002
#define MF_BITMAP       0x0004
#define MF_CHECKED      0x0008
#define MF_POPUP        0x0010
#define MF_MENUBARBREAK 0x0020
#define MF_MENUBREAK    0x0040
#define MF_OWNERDRAW    0x0100
#define MF_BYPOSITION   0x0400
#define MF_SEPARATOR    0x0800
#endif
#define W32_MFS_DEFAULT 0x1000
#define W32_MFT_RADIOCHECK 0x0200
#define W32_MAX_MENU 128
#define W32_HMENU_BASE 0x004D0000u              /* 'M' */

typedef struct { uint32_t flags; uintptr_t id; char text[112]; uintptr_t data; } w32_mitem;
static struct { int used, n, cap; w32_mitem *it; } g_menus[W32_MAX_MENU];

static void *w32_menu_new(void)
{
    int i;
    for (i = 1; i < W32_MAX_MENU; i++)
        if (!g_menus[i].used) {
            memset(&g_menus[i], 0, sizeof g_menus[i]);
            g_menus[i].used = 1;
            return (void *)(uintptr_t)(W32_HMENU_BASE | (unsigned)i);
        }
    return NULL;
}
static int w32_menu_idx(void *m)
{
    uintptr_t v = (uintptr_t)m;
    int i = (int)(v & 0xFFFF);
    if ((v & ~(uintptr_t)0xFFFF) != W32_HMENU_BASE) return 0;
    return (i > 0 && i < W32_MAX_MENU && g_menus[i].used) ? i : 0;
}

/* UTF-16 to UTF-8, for item text that is not plain ASCII. */
static void menu_utf8(const uint16_t *w, char *out, size_t n)
{
    size_t o = 0;
    if (!n) return;
    for (; w && *w && o + 4 < n; w++) {
        uint32_t c = *w;
        if (c >= 0xD800 && c < 0xDC00 && w[1] >= 0xDC00 && w[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[1] - 0xDC00); w++;
        }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}

/* Put an item in at `pos` (n or more appends). */
static int menu_insert(int mi, int pos, uint32_t flags, uintptr_t id, const char *text)
{
    w32_mitem *it;
    if (!mi) return 0;
    if (pos < 0 || pos > g_menus[mi].n) pos = g_menus[mi].n;
    if (g_menus[mi].n == g_menus[mi].cap) {
        int nc = g_menus[mi].cap ? g_menus[mi].cap * 2 : 16;
        w32_mitem *g = (w32_mitem *)realloc(g_menus[mi].it, (size_t)nc * sizeof *g);
        if (!g) return 0;
        g_menus[mi].it = g; g_menus[mi].cap = nc;
    }
    memmove(&g_menus[mi].it[pos + 1], &g_menus[mi].it[pos],
            (size_t)(g_menus[mi].n - pos) * sizeof g_menus[mi].it[0]);
    it = &g_menus[mi].it[pos];
    memset(it, 0, sizeof *it);
    it->flags = flags;
    it->id = id;
    if (!(flags & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) && text)
        snprintf(it->text, sizeof it->text, "%s", text);
    g_menus[mi].n++;
    return 1;
}

/* An item, by command id (looked for in submenus too, as Windows does) or by
 * position. *owner says which menu held it. */
static w32_mitem *menu_find(int mi, uintptr_t id, uint32_t flags, int *owner, int *posout)
{
    int i;
    if (!mi) return NULL;
    if (flags & MF_BYPOSITION) {
        if ((int)id < 0 || (int)id >= g_menus[mi].n) return NULL;
        if (owner) *owner = mi;
        if (posout) *posout = (int)id;
        return &g_menus[mi].it[id];
    }
    for (i = 0; i < g_menus[mi].n; i++) {
        w32_mitem *it = &g_menus[mi].it[i];
        if (!(it->flags & MF_POPUP) && !(it->flags & MF_SEPARATOR) && it->id == id) {
            if (owner) *owner = mi;
            if (posout) *posout = i;
            return it;
        }
    }
    for (i = 0; i < g_menus[mi].n; i++)
        if (g_menus[mi].it[i].flags & MF_POPUP) {
            w32_mitem *r = menu_find(w32_menu_idx((void *)g_menus[mi].it[i].id), id, flags, owner, posout);
            if (r) return r;
        }
    return NULL;
}

static MS void *st_CreatePopupMenu(void) { return w32_menu_new(); }
static MS void *st_CreateMenu(void) { return w32_menu_new(); }

static MS int32_t st_AppendMenuA(void *m, uint32_t f, uintptr_t id, const char *s)
{ return menu_insert(w32_menu_idx(m), -1, f, id, s); }
static MS int32_t st_AppendMenuW(void *m, uint32_t f, uintptr_t id, const uint16_t *s)
{
    char b[112];
    if ((f & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) || !s) b[0] = 0; else menu_utf8(s, b, sizeof b);
    return menu_insert(w32_menu_idx(m), -1, f, id, b);
}
static int menu_pos_of(int mi, uint32_t pos, uint32_t f)
{
    int owner, at = -1;
    if (pos == 0xFFFFFFFFu) return -1;
    if (f & MF_BYPOSITION) return (int)pos;
    return menu_find(mi, pos, 0, &owner, &at) && owner == mi ? at : -1;
}
static MS int32_t st_InsertMenuA(void *m, uint32_t pos, uint32_t f, uintptr_t id, const char *s)
{ int mi = w32_menu_idx(m); return menu_insert(mi, menu_pos_of(mi, pos, f), f, id, s); }
static MS int32_t st_InsertMenuW(void *m, uint32_t pos, uint32_t f, uintptr_t id, const uint16_t *s)
{
    char b[112];
    int mi = w32_menu_idx(m);
    if ((f & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) || !s) b[0] = 0; else menu_utf8(s, b, sizeof b);
    return menu_insert(mi, menu_pos_of(mi, pos, f), f, id, b);
}

/* MENUITEMINFO, in the native layout of whichever build this is. */
typedef struct {
    uint32_t cbSize, fMask, fType, fState, wID;
    void *hSubMenu, *hbmpChecked, *hbmpUnchecked;
    uintptr_t dwItemData;
    void *dwTypeData;
    uint32_t cch;
    void *hbmpItem;
} w32_mii;
#define W32_MIIM_STATE 0x01
#define W32_MIIM_ID 0x02
#define W32_MIIM_SUBMENU 0x04
#define W32_MIIM_TYPE 0x10
#define W32_MIIM_DATA 0x20
#define W32_MIIM_STRING 0x40
#define W32_MIIM_FTYPE 0x100

/* An item's flags from a MENUITEMINFO's type and state words. */
static void menu_from_mii(w32_mitem *it, const w32_mii *mi, int wide)
{
    uint32_t m = mi->fMask;
    if (m & (W32_MIIM_TYPE | W32_MIIM_FTYPE)) {
        it->flags &= ~(uint32_t)(MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW);
        if (mi->fType & MF_SEPARATOR) it->flags |= MF_SEPARATOR;
        if (mi->fType & MF_OWNERDRAW) it->flags |= MF_OWNERDRAW;
        if (mi->fType & W32_MFT_RADIOCHECK) it->flags |= W32_MFT_RADIOCHECK << 16;
    }
    if (m & W32_MIIM_STATE) {
        it->flags &= ~(uint32_t)(MF_GRAYED | MF_DISABLED | MF_CHECKED);
        if (mi->fState & 3) it->flags |= MF_GRAYED;
        if (mi->fState & MF_CHECKED) it->flags |= MF_CHECKED;
    }
    if (m & W32_MIIM_ID) it->id = mi->wID;
    if (m & W32_MIIM_SUBMENU) {
        if (mi->hSubMenu) { it->flags |= MF_POPUP; it->id = (uintptr_t)mi->hSubMenu; }
        else it->flags &= ~(uint32_t)MF_POPUP;
    }
    if (m & W32_MIIM_DATA) it->data = mi->dwItemData;
    if ((m & (W32_MIIM_STRING | W32_MIIM_TYPE)) && mi->dwTypeData && !(it->flags & (MF_SEPARATOR | MF_OWNERDRAW))) {
        if (wide) menu_utf8((const uint16_t *)mi->dwTypeData, it->text, sizeof it->text);
        else snprintf(it->text, sizeof it->text, "%s", (const char *)mi->dwTypeData);
    }
}
static int menu_insert_mii(void *m, uint32_t item, int32_t bypos, const w32_mii *mi, int wide)
{
    int h = w32_menu_idx(m), pos;
    w32_mitem tmp;
    if (!h || !mi) return 0;
    pos = bypos ? (int)item : menu_pos_of(h, item, 0);
    if (!menu_insert(h, pos, 0, 0, NULL)) return 0;
    if (pos < 0 || pos > g_menus[h].n - 1) pos = g_menus[h].n - 1;
    memset(&tmp, 0, sizeof tmp);
    menu_from_mii(&tmp, mi, wide);
    g_menus[h].it[pos] = tmp;
    return 1;
}
static MS int32_t st_InsertMenuItemA(void *m, uint32_t item, int32_t bypos, const void *info)
{ return menu_insert_mii(m, item, bypos, (const w32_mii *)info, 0); }
static MS int32_t st_InsertMenuItemW(void *m, uint32_t item, int32_t bypos, const void *info)
{ return menu_insert_mii(m, item, bypos, (const w32_mii *)info, 1); }

static MS int32_t st_GetMenuItemCount(void *m)
{ int i = w32_menu_idx(m); return i ? g_menus[i].n : -1; }
static MS uint32_t st_GetMenuItemID(void *m, int32_t pos)
{
    int i = w32_menu_idx(m);
    if (!i || pos < 0 || pos >= g_menus[i].n) return 0xFFFFFFFFu;
    if (g_menus[i].it[pos].flags & (MF_POPUP | MF_SEPARATOR)) return 0xFFFFFFFFu;
    return (uint32_t)g_menus[i].it[pos].id;
}
static MS void *st_GetSubMenu(void *m, int32_t pos)
{
    int i = w32_menu_idx(m);
    if (!i || pos < 0 || pos >= g_menus[i].n || !(g_menus[i].it[pos].flags & MF_POPUP)) return NULL;
    return (void *)g_menus[i].it[pos].id;
}

static int menu_get_mii(void *m, uint32_t item, int32_t bypos, w32_mii *mi, int wide)
{
    int h = w32_menu_idx(m), owner, pos;
    w32_mitem *it;
    if (!h || !mi) return 0;
    it = menu_find(h, item, bypos ? MF_BYPOSITION : 0, &owner, &pos);
    if (!it) return 0;
    if (mi->fMask & (W32_MIIM_TYPE | W32_MIIM_FTYPE))
        mi->fType = ((it->flags & MF_SEPARATOR) ? MF_SEPARATOR : 0) | ((it->flags & MF_OWNERDRAW) ? MF_OWNERDRAW : 0) |
                    ((it->flags >> 16) & W32_MFT_RADIOCHECK);
    if (mi->fMask & W32_MIIM_STATE)
        mi->fState = ((it->flags & (MF_GRAYED | MF_DISABLED)) ? 3 : 0) | ((it->flags & MF_CHECKED) ? MF_CHECKED : 0);
    if (mi->fMask & W32_MIIM_ID) mi->wID = (it->flags & MF_POPUP) ? 0 : (uint32_t)it->id;
    if (mi->fMask & W32_MIIM_SUBMENU) mi->hSubMenu = (it->flags & MF_POPUP) ? (void *)it->id : NULL;
    if (mi->fMask & W32_MIIM_DATA) mi->dwItemData = it->data;
    if ((mi->fMask & (W32_MIIM_STRING | W32_MIIM_TYPE)) && !(it->flags & MF_SEPARATOR)) {
        size_t n = strlen(it->text);
        if (mi->dwTypeData && mi->cch) {
            size_t k;
            if (wide) { uint16_t *o = (uint16_t *)mi->dwTypeData;
                        for (k = 0; k < n && k + 1 < mi->cch; k++) o[k] = (unsigned char)it->text[k]; o[k] = 0; mi->cch = (uint32_t)k; }
            else { char *o = (char *)mi->dwTypeData;
                   for (k = 0; k < n && k + 1 < mi->cch; k++) o[k] = it->text[k]; o[k] = 0; mi->cch = (uint32_t)k; }
        } else mi->cch = (uint32_t)n;
    }
    (void)owner; (void)pos;
    return 1;
}
static MS int32_t st_GetMenuItemInfoA(void *m, uint32_t item, int32_t bypos, void *info)
{ return menu_get_mii(m, item, bypos, (w32_mii *)info, 0); }
static MS int32_t st_GetMenuItemInfoW(void *m, uint32_t item, int32_t bypos, void *info)
{ return menu_get_mii(m, item, bypos, (w32_mii *)info, 1); }
static int menu_set_mii(void *m, uint32_t item, int32_t bypos, const w32_mii *mi, int wide)
{
    int h = w32_menu_idx(m), owner, pos;
    w32_mitem *it;
    if (!h || !mi) return 0;
    it = menu_find(h, item, bypos ? MF_BYPOSITION : 0, &owner, &pos);
    if (!it) return 0;
    menu_from_mii(it, mi, wide);
    return 1;
}
static MS int32_t st_SetMenuItemInfoA(void *m, uint32_t item, int32_t bypos, const void *info)
{ return menu_set_mii(m, item, bypos, (const w32_mii *)info, 0); }
static MS int32_t st_SetMenuItemInfoW(void *m, uint32_t item, int32_t bypos, const void *info)
{ return menu_set_mii(m, item, bypos, (const w32_mii *)info, 1); }

static MS int32_t st_GetMenuStringA(void *m, uint32_t id, char *buf, int32_t n, uint32_t flags)
{
    int h = w32_menu_idx(m), owner, pos;
    w32_mitem *it = h ? menu_find(h, id, flags, &owner, &pos) : NULL;
    if (!it) { if (buf && n > 0) buf[0] = 0; return 0; }
    if (!buf || n <= 0) return (int32_t)strlen(it->text);
    snprintf(buf, (size_t)n, "%s", it->text);
    return (int32_t)strlen(buf);
}
static MS int32_t st_GetMenuStringW(void *m, uint32_t id, uint16_t *buf, int32_t n, uint32_t flags)
{
    char b[112]; int32_t k, got;
    got = st_GetMenuStringA(m, id, b, sizeof b, flags);
    if (!buf || n <= 0) return got;
    for (k = 0; k < got && k + 1 < n; k++) buf[k] = (unsigned char)b[k];
    buf[k] = 0;
    return k;
}
static MS uint32_t st_GetMenuState(void *m, uint32_t id, uint32_t flags)
{
    int h = w32_menu_idx(m), owner, pos;
    w32_mitem *it = h ? menu_find(h, id, flags, &owner, &pos) : NULL;
    if (!it) return 0xFFFFFFFFu;
    return (it->flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_SEPARATOR | MF_POPUP)) |
           ((it->flags & MF_POPUP) ? ((uint32_t)g_menus[w32_menu_idx((void *)it->id)].n << 8) : 0);
}
static MS uint32_t st_CheckMenuItem(void *m, uint32_t id, uint32_t flags)
{
    int h = w32_menu_idx(m), owner, pos;
    w32_mitem *it = h ? menu_find(h, id, flags, &owner, &pos) : NULL;
    uint32_t was;
    if (!it) return 0xFFFFFFFFu;
    was = (it->flags & MF_CHECKED) ? MF_CHECKED : 0;
    if (flags & MF_CHECKED) it->flags |= MF_CHECKED; else it->flags &= ~(uint32_t)MF_CHECKED;
    return was;
}
static MS uint32_t st_EnableMenuItem(void *m, uint32_t id, uint32_t flags)
{
    int h = w32_menu_idx(m), owner, pos;
    w32_mitem *it = h ? menu_find(h, id, flags, &owner, &pos) : NULL;
    uint32_t was;
    if (!it) return 0xFFFFFFFFu;
    was = (it->flags & (MF_GRAYED | MF_DISABLED)) ? (it->flags & (MF_GRAYED | MF_DISABLED)) : 0;
    it->flags &= ~(uint32_t)(MF_GRAYED | MF_DISABLED);
    it->flags |= flags & (MF_GRAYED | MF_DISABLED);
    return was;
}
static MS int32_t st_CheckMenuRadioItem(void *m, uint32_t first, uint32_t last, uint32_t check, uint32_t flags)
{
    int h = w32_menu_idx(m), i, lo, hi, ck;
    if (!h) return 0;
    lo = menu_pos_of(h, first, flags); hi = menu_pos_of(h, last, flags); ck = menu_pos_of(h, check, flags);
    if (lo < 0 || hi < 0 || ck < 0) return 0;
    for (i = lo; i <= hi && i < g_menus[h].n; i++) {
        g_menus[h].it[i].flags &= ~(uint32_t)MF_CHECKED;
        g_menus[h].it[i].flags |= (W32_MFT_RADIOCHECK << 16);
        if (i == ck) g_menus[h].it[i].flags |= MF_CHECKED;
    }
    return 1;
}
static void menu_free(int h);
static int menu_remove(void *m, uint32_t id, uint32_t flags, int destroy)
{
    int h = w32_menu_idx(m), pos = menu_pos_of(h, id, flags);
    if (!h || pos < 0 || pos >= g_menus[h].n) return 0;
    if (destroy && (g_menus[h].it[pos].flags & MF_POPUP)) menu_free(w32_menu_idx((void *)g_menus[h].it[pos].id));
    memmove(&g_menus[h].it[pos], &g_menus[h].it[pos + 1], (size_t)(g_menus[h].n - pos - 1) * sizeof g_menus[h].it[0]);
    g_menus[h].n--;
    return 1;
}
static MS int32_t st_DeleteMenu(void *m, uint32_t id, uint32_t flags) { return menu_remove(m, id, flags, 1); }
static MS int32_t st_RemoveMenu(void *m, uint32_t id, uint32_t flags) { return menu_remove(m, id, flags, 0); }
static MS int32_t st_ModifyMenuA(void *m, uint32_t pos, uint32_t f, uintptr_t id, const char *s)
{
    int h = w32_menu_idx(m), at = menu_pos_of(h, pos, f);
    if (!h || at < 0 || at >= g_menus[h].n) return 0;
    g_menus[h].it[at].flags = f & ~(uint32_t)MF_BYPOSITION;
    g_menus[h].it[at].id = id;
    g_menus[h].it[at].text[0] = 0;
    if (!(f & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) && s)
        snprintf(g_menus[h].it[at].text, sizeof g_menus[h].it[at].text, "%s", s);
    return 1;
}
static MS int32_t st_ModifyMenuW(void *m, uint32_t pos, uint32_t f, uintptr_t id, const uint16_t *s)
{
    char b[112];
    if ((f & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) || !s) b[0] = 0; else menu_utf8(s, b, sizeof b);
    return st_ModifyMenuA(m, pos, f, id, b);
}
static MS int32_t st_SetMenuDefaultItem(void *m, uint32_t item, uint32_t bypos) { (void)item; (void)bypos; return w32_menu_idx(m) ? 1 : 0; }
static MS int32_t st_GetMenuInfo(void *m, void *info) { (void)info; return w32_menu_idx(m) ? 1 : 0; }
static MS int32_t st_SetMenuInfo(void *m, const void *info) { (void)info; return w32_menu_idx(m) ? 1 : 0; }
static MS int32_t st_SetMenu(void *hwnd, void *m) { (void)hwnd; (void)m; return 1; }
static MS int32_t st_IsMenu(void *m) { return w32_menu_idx(m) ? 1 : 0; }
static void menu_free(int h)
{
    int i;
    if (!h || !g_menus[h].used) return;
    g_menus[h].used = 0;                         /* first: a menu may name itself */
    for (i = 0; i < g_menus[h].n; i++)
        if (g_menus[h].it[i].flags & MF_POPUP) menu_free(w32_menu_idx((void *)g_menus[h].it[i].id));
    free(g_menus[h].it);
    g_menus[h].it = NULL; g_menus[h].n = g_menus[h].cap = 0;
}
static MS int32_t st_DestroyMenu(void *m)
{
    int h = w32_menu_idx(m);
    if (!h) return 0;
    menu_free(h);
    return 1;
}

/* ---- showing a menu -------------------------------------------------------- */

#define TPM_CENTERALIGN  0x0004
#define TPM_RIGHTALIGN   0x0008
#define TPM_VCENTERALIGN 0x0010
#define TPM_BOTTOMALIGN  0x0020
#define TPM_NONOTIFY     0x0080
#define TPM_RETURNCMD    0x0100
#define PM_MAXLV   4
#define PM_ITEM_H  20
#define PM_SEP_H   7
#define PM_EM      11
#define PM_MAXVIEW 520        /* a taller menu scrolls */

static struct {
    int  active, done, result, armed, moved;
    int  depth;                              /* levels open, 1.. */
    int  ox, oy;                             /* where the pointer was when it opened */
    struct { int menu, wnd, hot, top, total; } lv[PM_MAXLV];
} PM;

static int pm_item_h(const w32_mitem *it) { return (it->flags & MF_SEPARATOR) ? PM_SEP_H : PM_ITEM_H; }

/* "&Name\tCtrl+N" is a name and an accelerator; the ampersand marks a mnemonic
 * that is only drawn as plain text here. */
static void pm_split(const char *t, char *name, size_t n, char *acc, size_t an)
{
    size_t o = 0; const char *p;
    acc[0] = 0;
    for (p = t; *p && *p != '\t' && o + 1 < n; p++) {
        if (*p == '&' && p[1]) continue;
        name[o++] = *p;
    }
    name[o] = 0;
    if (*p == '\t') snprintf(acc, an, "%s", p + 1);
}

static void pm_size(int mi, int *w, int *h, int *total)
{
    int i, tw = 0, aw = 0, hh = 4;
    char name[112], acc[64];
    g_ctl_em = PM_EM;
    for (i = 0; i < g_menus[mi].n; i++) {
        const w32_mitem *it = &g_menus[mi].it[i];
        hh += pm_item_h(it);
        if (it->flags & MF_SEPARATOR) continue;
        pm_split(it->text, name, sizeof name, acc, sizeof acc);
        { int a = ctl_text_w(name), b = ctl_text_w(acc); if (a > tw) tw = a; if (b > aw) aw = b; }
    }
    g_ctl_em = 0;
    *w = 26 + tw + (aw ? 24 + aw : 0) + 22;
    if (*w < 80) *w = 80;
    *total = hh;
    *h = hh > PM_MAXVIEW ? PM_MAXVIEW : hh;
}

static int pm_level_of_wnd(int wi)
{
    int k;
    for (k = 0; k < PM.depth; k++) if (PM.lv[k].wnd == wi) return k;
    return -1;
}

/* The item under a point in a level's own coordinates, or -1. */
static int pm_hit(int k, int lx, int ly)
{
    int mi = PM.lv[k].menu, y = 2 - PM.lv[k].top, i;
    w32_wnd *w = &W.wnd[PM.lv[k].wnd];
    if (lx < 2 || lx >= w->w - 2 || ly < 2 || ly >= w->h - 2) return -1;
    for (i = 0; i < g_menus[mi].n; i++) {
        int ih = pm_item_h(&g_menus[mi].it[i]);
        if (ly >= y && ly < y + ih) return i;
        y += ih;
    }
    return -1;
}

static int pm_selectable(const w32_mitem *it)
{ return !(it->flags & (MF_SEPARATOR | MF_GRAYED | MF_DISABLED)); }

static void pm_paint(w32_wnd *w, int wi)
{
    int k = pm_level_of_wnd(wi), mi, i, y;
    w32_surf *t = &w->surf;
    if (k < 0) { w->has_update = 0; return; }
    mi = PM.lv[k].menu;
    ctl_fill(t, 0, 0, w->w, w->h, CLR_FACE);
    ctl_bevel(t, 0, 0, w->w, w->h, 0);
    g_ctl_em = PM_EM;
    y = 2 - PM.lv[k].top;
    for (i = 0; i < g_menus[mi].n; i++) {
        const w32_mitem *it = &g_menus[mi].it[i];
        int ih = pm_item_h(it), hot = (i == PM.lv[k].hot) && pm_selectable(it);
        if (y + ih > 2 && y < w->h - 2) {
            char name[112], acc[64];
            uint32_t fg;
            if (it->flags & MF_SEPARATOR) {
                ctl_fill(t, 4, y + 3, w->w - 4, y + 4, CLR_SHADOW);
                ctl_fill(t, 4, y + 4, w->w - 4, y + 5, CLR_HILIGHT);
            } else {
                pm_split(it->text, name, sizeof name, acc, sizeof acc);
                if (hot) ctl_fill(t, 2, y, w->w - 2, y + ih, 0x000080u);
                fg = (it->flags & (MF_GRAYED | MF_DISABLED)) ? CLR_GRAYTEXT : hot ? CLR_HILIGHT : CLR_TEXT;
                ctl_text(wi, 24, y + (ih - 14) / 2, name, fg);
                if (acc[0]) ctl_text(wi, w->w - 16 - ctl_text_w(acc), y + (ih - 14) / 2, acc, fg);
                if (it->flags & MF_CHECKED) {
                    int cx = 11, cy = y + ih / 2, d;
                    if ((it->flags >> 16) & W32_MFT_RADIOCHECK)
                        for (d = -2; d <= 2; d++) ctl_fill(t, cx - (3 - (d < 0 ? -d : d)), cy + d, cx + (3 - (d < 0 ? -d : d)) + 1, cy + d + 1, fg);
                    else {
                        for (d = 0; d < 3; d++) ctl_fill(t, cx - 4 + d, cy + d - 1, cx - 3 + d, cy + d + 2, fg);
                        for (d = 0; d < 5; d++) ctl_fill(t, cx - 1 + d, cy + 1 - d, cx + d, cy + 4 - d, fg);
                    }
                }
                if (it->flags & MF_POPUP) {                      /* the arrow for a submenu */
                    int ax = w->w - 10, ay = y + ih / 2, d;
                    for (d = 0; d < 4; d++) ctl_fill(t, ax + d, ay - 4 + d, ax + d + 1, ay + 5 - d, fg);
                }
            }
        }
        y += ih;
    }
    if (PM.lv[k].total > w->h) {                                  /* scrolling is possible */
        if (PM.lv[k].top > 0) ctl_fill(t, w->w / 2 - 3, 4, w->w / 2 + 4, 5, CLR_TEXT);
        if (PM.lv[k].top + w->h - 4 < PM.lv[k].total) ctl_fill(t, w->w / 2 - 3, w->h - 6, w->w / 2 + 4, w->h - 5, CLR_TEXT);
    }
    g_ctl_em = 0;
    w->has_update = 0;
}

static MS W_LRESULT w32_popup_wndproc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    (void)wp; (void)lp;
    if (!w) return 0;
    if (msg == WM_PAINT) { pm_paint(w, (int)(w - W.wnd)); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return 0;
}

static void pm_redraw(void)
{
    int k;
    for (k = 0; k < PM.depth; k++)
        if (PM.lv[k].wnd && W.wnd[PM.lv[k].wnd].used) W.wnd[PM.lv[k].wnd].has_update = 1;
}

/* Open a menu as level `k`, near (x, y) in the host's coordinates. The frame is
 * grown to take it if it does not fit, but it is moved first to fit the editor
 * where it can be. */
static int pm_open_level(int k, int mi, int x, int y, int align_right, int align_bottom)
{
    int w, h, total, d = W.display ? W.display : W.host, fw, fh, wi;
    void *hw;
    pm_size(mi, &w, &h, &total);
    if (align_right) x -= w;
    if (align_bottom) y -= h;
    fw = (d && W.wnd[d].used) ? W.wnd[d].w : W.ext_w;     /* no window: a Mac editor's frame */
    fh = (d && W.wnd[d].used) ? W.wnd[d].h : W.ext_h;
    if (k > 0) {                  /* a submenu fits the room the menus above it have made */
        int j, ox, oy, root = (W.host && W.wnd[W.host].used) ? W.host : 0;
        for (j = 0; j < k; j++) {
            if (!PM.lv[j].wnd || !W.wnd[PM.lv[j].wnd].used) continue;
            w32_origin_of(PM.lv[j].wnd, root, &ox, &oy);
            if (oy + W.wnd[PM.lv[j].wnd].h > fh) fh = oy + W.wnd[PM.lv[j].wnd].h;
            if (ox + W.wnd[PM.lv[j].wnd].w > fw) fw = ox + W.wnd[PM.lv[j].wnd].w;
        }
    }
    if (fw >= w && x + w > fw) x = fw - w;
    if (fh >= h && y + h > fh) y = fh - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    hw = w32_create_id("#peloadpopup", "", x, y, w, h, dlg_root(), NULL, W32_WS_CHILD | W32_WS_VISIBLE, 0, 0);
    if (!hw) return 0;
    wi = w32_i(W32_HWND_BASE, hw);
    PM.lv[k].menu = mi; PM.lv[k].wnd = wi; PM.lv[k].hot = -1; PM.lv[k].top = 0; PM.lv[k].total = total;
    PM.depth = k + 1;
    W.wnd[wi].has_update = 1;
    return wi;
}

static void pm_close_to(int depth)
{
    while (PM.depth > depth) {
        PM.depth--;
        if (PM.lv[PM.depth].wnd) dlg_destroy_tree(PM.lv[PM.depth].wnd);
        PM.lv[PM.depth].wnd = 0;
    }
}

static void pm_open_sub(int k, int item)
{
    int mi = PM.lv[k].menu, sub, y, i, ox, oy;
    const w32_mitem *it = &g_menus[mi].it[item];
    w32_wnd *w = &W.wnd[PM.lv[k].wnd];
    int root = (W.host && W.wnd[W.host].used) ? W.host : 0;
    if (!(it->flags & MF_POPUP) || !pm_selectable(it)) return;
    sub = w32_menu_idx((void *)it->id);
    if (!sub) return;
    if (PM.depth > k + 1 && PM.lv[k + 1].menu == sub) return;          /* already open */
    pm_close_to(k + 1);
    if (k + 1 >= PM_MAXLV) return;
    y = 2 - PM.lv[k].top;
    for (i = 0; i < item; i++) y += pm_item_h(&g_menus[mi].it[i]);
    w32_origin_of(PM.lv[k].wnd, root, &ox, &oy);
    pm_open_level(k + 1, sub, ox + w->w - 3, oy + y - 2, 0, 0);
}

static void pm_finish(int result) { PM.result = result; PM.done = 1; }

static void pm_choose(int k, int item)
{
    const w32_mitem *it = &g_menus[PM.lv[k].menu].it[item];
    if (!pm_selectable(it)) return;
    if (it->flags & MF_POPUP) { pm_open_sub(k, item); return; }
    pm_finish((int)it->id ? (int)it->id : -1);       /* an id of 0 is a choice too; -1 stands for it */
}

static void pm_scroll(int k, int dy)
{
    int max = PM.lv[k].total - W.wnd[PM.lv[k].wnd].h;
    if (max <= 0) return;
    PM.lv[k].top += dy;
    if (PM.lv[k].top < 0) PM.lv[k].top = 0;
    if (PM.lv[k].top > max) PM.lv[k].top = max;
    pm_redraw();
}

/* All pointer input belongs to an open menu. Returns 1 when it took the event. */
static int w32_popup_mouse(int x, int y, int msg, int buttons, int wheel)
{
    int k, root, found = -1, lx = 0, ly = 0, item = -1;
    (void)buttons;
    if (!PM.active) return 0;
    root = (W.host && W.wnd[W.host].used) ? W.host : 0;
    for (k = PM.depth - 1; k >= 0; k--) {
        int ox, oy;
        w32_wnd *w;
        if (!PM.lv[k].wnd || !W.wnd[PM.lv[k].wnd].used) continue;
        w = &W.wnd[PM.lv[k].wnd];
        w32_origin_of(PM.lv[k].wnd, root, &ox, &oy);
        if (x >= ox && x < ox + w->w && y >= oy && y < oy + w->h) { found = k; lx = x - ox; ly = y - oy; break; }
    }
    if (found >= 0) item = pm_hit(found, lx, ly);
    if (abs(x - PM.ox) > 5 || abs(y - PM.oy) > 5) PM.moved = 1;
    switch (msg) {
    case 0x0200: /* WM_MOUSEMOVE */
        if (found >= 0 && item != PM.lv[found].hot) {
            PM.lv[found].hot = item;
            if (item >= 0 && (g_menus[PM.lv[found].menu].it[item].flags & MF_POPUP)) pm_open_sub(found, item);
            else if (PM.depth > found + 1) pm_close_to(found + 1);
            pm_redraw();
        }
        break;
    case 0x0201: case 0x0204: case 0x0207: /* a button goes down */
        if (found < 0) { pm_finish(0); break; }                 /* a click outside closes it */
        PM.armed = 1;
        if (item >= 0) { PM.lv[found].hot = item; pm_redraw(); if (g_menus[PM.lv[found].menu].it[item].flags & MF_POPUP) pm_open_sub(found, item); }
        break;
    case 0x0202: case 0x0205: case 0x0208: /* a button comes up */
        /* The release of the click that opened the menu is not a choice: the
         * menu comes up under the pointer, so the first item would be taken. A
         * choice needs a press inside the menu, or the pointer to have been
         * dragged to an item. */
        if (found >= 0 && item >= 0 && (PM.armed || PM.moved)) pm_choose(found, item);
        break;
    case 0x020A: /* WM_MOUSEWHEEL */
        if (found >= 0) pm_scroll(found, wheel > 0 ? -PM_ITEM_H * 2 : PM_ITEM_H * 2);
        break;
    default: break;
    }
    return 1;
}

/* Up and Down move through the items that can be chosen, Right opens a submenu
 * and Left closes one, Return chooses and Escape closes. */
static int w32_popup_key(int vk, int down, int ch)
{
    int k, mi, n, i, step;
    (void)ch;
    if (!PM.active) return 0;
    if (!down) return 1;
    k = PM.depth - 1;
    mi = PM.lv[k].menu; n = g_menus[mi].n;
    switch (vk) {
    case 27: if (PM.depth > 1) { pm_close_to(PM.depth - 1); pm_redraw(); } else pm_finish(0); break;
    case 13: if (PM.lv[k].hot >= 0) pm_choose(k, PM.lv[k].hot); break;
    case 0x27: if (PM.lv[k].hot >= 0) { pm_open_sub(k, PM.lv[k].hot);
                   if (PM.depth > k + 1) { int j; for (j = 0; j < g_menus[PM.lv[k + 1].menu].n; j++)
                       if (pm_selectable(&g_menus[PM.lv[k + 1].menu].it[j])) { PM.lv[k + 1].hot = j; break; } pm_redraw(); } }
               break;
    case 0x25: if (PM.depth > 1) { pm_close_to(PM.depth - 1); pm_redraw(); } break;
    case 0x26: case 0x28:
        step = vk == 0x28 ? 1 : -1;
        i = PM.lv[k].hot;
        for (;;) {
            i += step;
            if (i < 0 || i >= n) {
                if (PM.lv[k].hot < 0 || (i = vk == 0x28 ? 0 : n - 1) == PM.lv[k].hot) break;
            }
            if (pm_selectable(&g_menus[mi].it[i])) { PM.lv[k].hot = i; break; }
            if (i == PM.lv[k].hot) break;
        }
        {   /* keep it in view */
            int y0 = 2, j, yh;
            for (j = 0; j < PM.lv[k].hot; j++) y0 += pm_item_h(&g_menus[mi].it[j]);
            yh = pm_item_h(&g_menus[mi].it[PM.lv[k].hot < 0 ? 0 : PM.lv[k].hot]);
            if (y0 < PM.lv[k].top) PM.lv[k].top = y0 - 2;
            if (y0 + yh > PM.lv[k].top + W.wnd[PM.lv[k].wnd].h - 2) PM.lv[k].top = y0 + yh - W.wnd[PM.lv[k].wnd].h + 2;
            if (PM.lv[k].top < 0) PM.lv[k].top = 0;
        }
        pm_redraw();
        break;
    default: break;
    }
    return 1;
}

static int pm_track(void *menu, uint32_t flags, int x, int y, void *owner)
{
    int mi = w32_menu_idx(menu), wi, result;
    if (!mi || PM.active || g_menus[mi].n == 0) return 0;
    if (!dlg_allowed()) return 0;
    memset(&PM, 0, sizeof PM);
    PM.ox = W.mouse_x; PM.oy = W.mouse_y;
    wi = pm_open_level(0, mi, x, y, (flags & TPM_RIGHTALIGN) != 0, (flags & TPM_BOTTOMALIGN) != 0);
    if (!wi) return 0;
    if (flags & TPM_CENTERALIGN) { W.wnd[wi].x -= W.wnd[wi].w / 2; if (W.wnd[wi].x < 0) W.wnd[wi].x = 0; }
    PM.active = 1;
    (void)w32_modal_wait(wi, &PM.done);
    result = PM.done ? PM.result : 0;
    pm_close_to(0);
    memset(&PM, 0, sizeof PM);
    if (result == -1) result = 0;               /* a command whose id is 0 is indistinguishable from none, as on Windows */
    if (flags & TPM_RETURNCMD) return result;
    if (result && !(flags & TPM_NONOTIFY) && owner) w32_post(owner, WM_COMMAND, (uintptr_t)result, 0);
    return result != 0;
}
static MS int32_t st_TrackPopupMenu(void *m, uint32_t f, int32_t x, int32_t y, int32_t r, void *h, const void *rc)
{ (void)r; (void)rc; return pm_track(m, f, x, y, h); }
static MS int32_t st_TrackPopupMenuEx(void *m, uint32_t f, int32_t x, int32_t y, void *h, const void *tpm)
{ (void)tpm; return pm_track(m, f, x, y, h); }

/* ---- message boxes ---------------------------------------------------------- */

#define MB_TYPEMASK_ 0x0F
static struct {
    int used, done, result, wnd, nb, def;
    int ids[3];
    char text[600], caption[96];
} MB;

static void mb_wrap(const char *t, int maxw, char lines[][96], int *n, int cap)
{
    const char *p = t;
    *n = 0;
    while (*p && *n < cap) {
        char line[96]; int o = 0;
        while (*p && *p != '\n') {
            const char *sp = p; int wl = 0;
            while (sp[wl] && sp[wl] != ' ' && sp[wl] != '\n' && wl < 90) wl++;
            if (o && 0) break;
            line[o] = 0;
            {   char tmp[96]; int tl = o + (o ? 1 : 0) + wl;
                if (tl >= 95) tl = 94;
                snprintf(tmp, sizeof tmp, "%s%s%.*s", line, o ? " " : "", wl, sp);
                g_ctl_em = DLG_EM + 1;
                if (o && ctl_text_w(tmp) > maxw) { g_ctl_em = 0; break; }
                g_ctl_em = 0;
                snprintf(line, sizeof line, "%s", tmp); o = (int)strlen(line);
            }
            p += wl; while (*p == ' ') p++;
        }
        snprintf(lines[*n], 96, "%s", line); (*n)++;
        if (*p == '\n') p++;
        if (!o && !*p) break;
    }
}

static MS W_LRESULT w32_msg_wndproc(void *hwnd, uint32_t msg, W_WPARAM wp, W_LPARAM lp)
{
    w32_wnd *w = w32_wget(hwnd);
    int wi = w ? (int)(w - W.wnd) : 0;
    (void)lp;
    if (!w || !MB.used || wi != MB.wnd) return 0;
    if (msg == WM_PAINT) {
        char lines[12][96]; int n, i;
        ctl_fill(&w->surf, 0, 0, w->w, w->h, CLR_FACE);
        ctl_bevel(&w->surf, 0, 0, w->w, w->h, 0);
        g_ctl_em = DLG_EM + 1;
        ctl_text(wi, 14, 10, MB.caption, CLR_TEXT);
        ctl_fill(&w->surf, 10, 28, w->w - 10, 29, CLR_SHADOW);
        g_ctl_em = 0;
        mb_wrap(MB.text, w->w - 40, lines, &n, 12);
        g_ctl_em = DLG_EM + 1;
        for (i = 0; i < n; i++) ctl_text(wi, 20, 38 + i * 16, lines[i], CLR_TEXT);
        g_ctl_em = 0;
        w->has_update = 0;
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_COMMAND) { MB.result = (int)(wp & 0xFFFF); MB.done = 1; return 0; }
    return 0;
}

static intptr_t mb_run(void *owner, const char *text, const char *caption, uint32_t type)
{
    static const struct { int n; int ids[3]; const char *lab[3]; } sets[] = {
        { 1, { 1, 0, 0 }, { "OK", "", "" } },
        { 2, { 1, 2, 0 }, { "OK", "Cancel", "" } },
        { 3, { 3, 4, 5 }, { "Abort", "Retry", "Ignore" } },
        { 3, { 6, 7, 2 }, { "Yes", "No", "Cancel" } },
        { 2, { 6, 7, 0 }, { "Yes", "No", "" } },
        { 2, { 4, 2, 0 }, { "Retry", "Cancel", "" } },
        { 3, { 2, 10, 11 }, { "Cancel", "Try Again", "Continue" } },
    };
    int set = (int)(type & MB_TYPEMASK_), i, w, h, bx, nl;
    char lines[12][96];
    void *hw;
    (void)owner;
    if (!dlg_allowed()) return 1;                     /* nobody there to ask: as it always was */
    if (MB.used || W.sheet || PM.active) return 1;
    if (set > 6) set = 0;
    snprintf(MB.text, sizeof MB.text, "%s", text ? text : "");
    snprintf(MB.caption, sizeof MB.caption, "%s", caption && *caption ? caption : "Message");
    mb_wrap(MB.text, 360, lines, &nl, 12);
    w = 400; h = 38 + nl * 16 + 24 + 23 + 14;
    if (h < 110) h = 110;
    hw = w32_create_id("#peloadmsg", MB.caption, 0, dlg_below_editor(), w, h, dlg_root(), NULL,
                       W32_WS_CHILD | W32_WS_VISIBLE, 0, 0);
    if (!hw) return 1;
    MB.used = 1; MB.done = 0; MB.result = 0;
    MB.wnd = w32_i(W32_HWND_BASE, hw);
    W.sheet = MB.wnd;
    bx = (w - sets[set].n * 86) / 2 + 6;
    for (i = 0; i < sets[set].n; i++) {
        void *b = w32_create_id("BUTTON", sets[set].lab[i], bx + i * 86, h - 23 - 12, 75, 23, hw, NULL,
                                W32_WS_CHILD | W32_WS_VISIBLE | (i == 0 ? 1 : 0), 0, sets[set].ids[i]);
        if (b) W.wnd[w32_i(W32_HWND_BASE, b)].ctl_em = DLG_EM;
    }
    MB.def = sets[set].ids[0];
    (void)w32_modal_wait(MB.wnd, &MB.done);
    i = MB.done ? MB.result : sets[set].ids[sets[set].n - 1];
    {   int wi = MB.wnd; memset(&MB, 0, sizeof MB); dlg_destroy_tree(wi); W.sheet = 0; }
    return i;
}
static MS int32_t st_MessageBoxA(void *h, const char *t, const char *c, uint32_t f)
{
    if (!W.user_input) fprintf(stderr, "[plugin] %s: %s\n", c ? c : "", t ? t : "");
    return (int32_t)mb_run(h, t, c, f);
}
static MS int32_t st_MessageBoxW(void *h, const uint16_t *t, const uint16_t *c, uint32_t f)
{
    char tb[600], cb[96];
    menu_utf8(t, tb, sizeof tb);
    menu_utf8(c, cb, sizeof cb);
    return (int32_t)mb_run(h, tb, cb, f);
}

/* Which windows a dialog, a sheet or a menu has put over the editor, bottom to
 * top: the compositor draws them and sizes the frame to hold them. */
static int w32_overlays(int *out, int max)
{
    int n = 0, k;
    if (W.sheet > 0 && W.sheet < W32_MAX_WND && W.wnd[W.sheet].used && W.wnd[W.sheet].surf.px &&
        W.wnd[W.sheet].visible && n < max) out[n++] = W.sheet;
    if (PM.active)
        for (k = 0; k < PM.depth && n < max; k++)
            if (PM.lv[k].wnd && W.wnd[PM.lv[k].wnd].used && W.wnd[PM.lv[k].wnd].surf.px) out[n++] = PM.lv[k].wnd;
    return n;
}

/* The window state is gone -- the editor closed -- and with it any menu or
 * dialog that was up. */
static void w32_dlg_reset(void)
{
    memset(&SH, 0, sizeof SH);
    memset(&PM, 0, sizeof PM);
    memset(&MB, 0, sizeof MB);
    W.sheet = 0;
}



/* ---- for the loaders that are not Win32 ----------------------------------------
 *
 * A macOS plug-in's NSMenu is shown by the same popup: the Cocoa shim builds a
 * menu here, has it shown, and gets back which item was chosen. Its frames do
 * not come through w32_editor_pixels, so the host hands them to
 * pe_overlay_compose, which adds whatever is up over them. */

void *pe_menu_new(void) { return w32_menu_new(); }
int pe_menu_append(void *m, uint32_t flags, uintptr_t id, const char *text)
{ return menu_insert(w32_menu_idx(m), -1, flags, id, text); }
void pe_menu_destroy(void *m) { int h = w32_menu_idx(m); if (h) menu_free(h); }
/* The command chosen, or 0. `x`, `y` are in the editor frame, top left first. */
int pe_menu_popup(void *m, int x, int y) { return pm_track(m, TPM_RETURNCMD, x, y, NULL); }
/* Where the pointer last was, and how tall the editor's frame is: what a menu
 * asked for in screen coordinates or a bottom-left origin needs. */
void pe_pointer(int *x, int *y) { if (x) *x = W.mouse_x; if (y) *y = W.mouse_y; }
int pe_frame_height(void) { return W.ext_h; }
/* Marks the user's own click or key as inside the plug-in, so a menu or dialog
 * it opens in answer is allowed. */
void pe_user_input(int delta) { W.user_input += delta; if (delta > 0) W.last_click_ms = w32_now_ms(); }

static w32_surf g_present_ov;
int pe_overlay_compose(const unsigned int *base, int bw, int bh,
                       const unsigned int **out, int *ow, int *oh)
{
    int ov[8], n, k, cw = bw, ch = bh, y;
    W.ext_w = bw; W.ext_h = bh;
    n = w32_overlays(ov, 8);
    if (!n || !base) return 0;
    for (k = 0; k < n; k++) {
        int ox, oy;
        w32_origin_of(ov[k], 0, &ox, &oy);
        if (ox + W.wnd[ov[k]].w > cw) cw = ox + W.wnd[ov[k]].w;
        if (oy + W.wnd[ov[k]].h > ch) ch = oy + W.wnd[ov[k]].h;
    }
    w32_surf_size(&g_present_ov, cw, ch);
    if (!g_present_ov.px) return 0;
    if (cw != bw || ch != bh) for (y = 0; y < cw * ch; y++) g_present_ov.px[y] = 0x00808080u;
    for (y = 0; y < bh; y++) memcpy(g_present_ov.px + (size_t)y * cw, base + (size_t)y * bw, (size_t)bw * 4);
    for (k = 0; k < n; k++) {
        int ox, oy, yy, xx;
        w32_wnd *o = &W.wnd[ov[k]];
        w32_origin_of(ov[k], 0, &ox, &oy);
        for (yy = 0; yy < o->surf.h; yy++) {
            if (oy + yy < 0 || oy + yy >= ch) continue;
            for (xx = 0; xx < o->surf.w; xx++) {
                if (ox + xx < 0 || ox + xx >= cw) continue;
                g_present_ov.px[(size_t)(oy + yy) * cw + ox + xx] = o->surf.px[(size_t)yy * o->surf.w + xx];
            }
        }
        w32_composite_children(ov[k], &g_present_ov, ox, oy);
    }
    *out = g_present_ov.px; *ow = cw; *oh = ch;
    return 1;
}


#endif /* PELOAD_WIN32DLG_H */
