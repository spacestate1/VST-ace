/* NSMenu and NSMenuItem, and the popup a plug-in opens with them.
 *
 * iPlug2's macOS popup menu is a subclass of NSMenu that fills itself with
 * addItemWithTitle:action:keyEquivalent: and setSubmenu:forItem:, then sends
 * popUpMenuPositioningItem:atLocation:inView: and reads which item the user
 * chose when that returns -- which it only does once the menu is gone. The
 * stand-in classes answered none of this, so every options menu, preset menu and
 * drop-down in the 76 macOS plug-ins that have one did nothing.
 *
 * What these classes hold is kept in tables here, keyed by the object, rather
 * than in the objects: a plug-in's own NSMenu subclass lays its instance
 * variables where Apple's header put them, after a superclass of a size nobody
 * here knows, and anything of ours written into the instance would land on them.
 *
 * Showing the menu is the Windows layer's job (see win32dlg.h): it already
 * draws a popup over an editor's frame, runs the modal loop that waits for the
 * user, and takes the pointer while it is open. This file builds one of those
 * from the NSMenu when it is popped up, and when something is chosen, sends the
 * item's action to its target, which is what Cocoa does before the pop-up call
 * returns. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "macobjc.h"

typedef const char *SEL;
typedef void *id;

const char *macns_utf8(void *str);
void *macns_new_object(const char *cls);
void *macns_make_string(const char *utf8);

/* From the Windows layer (win32dlg.h). */
void *pe_menu_new(void);
int   pe_menu_append(void *m, uint32_t flags, uintptr_t id, const char *text);
int   pe_menu_popup(void *m, int x, int y);
void  pe_menu_destroy(void *m);
void  pe_pointer(int *x, int *y);
int   pe_frame_height(void);

#define MF_GRAYED    0x0001
#define MF_CHECKED   0x0008
#define MF_POPUP     0x0010
#define MF_SEPARATOR 0x0800

typedef struct mitem {
    id   obj;
    char title[128], key[8], action[96];
    long tag, state, indent;
    int  enabled, sep, hidden;
    id   sub, target, menu, repr;
} mitem;

typedef struct mmenu {
    id    obj;
    char  title[128];
    mitem **it;
    int   n, cap;
    id    delegate, super_;
} mmenu;

/* Object -> record, open addressing. Objects here are never freed, so entries
 * are not either. */
typedef struct { id key; void *val; } slot;
typedef struct { slot *s; size_t cap, n; } table;

static size_t hash_ptr(id p) { uintptr_t v = (uintptr_t)p; v ^= v >> 17; v *= 0x9E3779B97F4A7C15ull; return (size_t)(v >> 20); }

static void *tab_get(table *t, id key)
{
    size_t i;
    if (!t->cap || !key) return NULL;
    for (i = hash_ptr(key) & (t->cap - 1); t->s[i].key; i = (i + 1) & (t->cap - 1))
        if (t->s[i].key == key) return t->s[i].val;
    return NULL;
}

static void tab_put(table *t, id key, void *val)
{
    size_t i;
    if ((t->n + 1) * 2 > t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64, k;
        slot *ns = (slot *)calloc(nc, sizeof *ns);
        if (!ns) return;
        for (k = 0; k < t->cap; k++)
            if (t->s[k].key) {
                for (i = hash_ptr(t->s[k].key) & (nc - 1); ns[i].key; i = (i + 1) & (nc - 1)) ;
                ns[i] = t->s[k];
            }
        free(t->s);
        t->s = ns; t->cap = nc;
    }
    for (i = hash_ptr(key) & (t->cap - 1); t->s[i].key; i = (i + 1) & (t->cap - 1))
        if (t->s[i].key == key) { t->s[i].val = val; return; }
    t->s[i].key = key; t->s[i].val = val; t->n++;
}

static table g_items, g_menus;

static mitem *item_of(id obj, int create)
{
    mitem *m = (mitem *)tab_get(&g_items, obj);
    if (m || !create || !obj) return m;
    m = (mitem *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->obj = obj; m->enabled = 1;
    tab_put(&g_items, obj, m);
    return m;
}
static mmenu *menu_of(id obj, int create)
{
    mmenu *m = (mmenu *)tab_get(&g_menus, obj);
    if (m || !create || !obj) return m;
    m = (mmenu *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->obj = obj;
    tab_put(&g_menus, obj, m);
    return m;
}

static void set_str(char *dst, size_t n, id str)
{
    const char *s = str ? macns_utf8(str) : NULL;
    snprintf(dst, n, "%s", s ? s : "");
}

/* ---------------------------------------------------------------- NSMenuItem */

static id mi_separator(id self, SEL sel)
{
    id it = macns_new_object("NSMenuItem");
    mitem *m = item_of(it, 1);
    (void)self; (void)sel;
    if (m) m->sep = 1;
    return it;
}
static id mi_init_full(id self, SEL sel, id title, SEL action, id key)
{
    mitem *m = item_of(self, 1);
    (void)sel;
    if (!m) return self;
    set_str(m->title, sizeof m->title, title);
    snprintf(m->action, sizeof m->action, "%s", action ? action : "");
    set_str(m->key, sizeof m->key, key);
    return self;
}
static id mi_init(id self, SEL sel) { (void)sel; item_of(self, 1); return self; }
static id mi_title(id self, SEL sel)
{ mitem *m = item_of(self, 1); (void)sel; return macns_make_string(m ? m->title : ""); }
static void mi_set_title(id self, SEL sel, id t)
{ mitem *m = item_of(self, 1); (void)sel; if (m) set_str(m->title, sizeof m->title, t); }
static long mi_tag(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? m->tag : 0; }
static void mi_set_tag(id self, SEL sel, long t) { mitem *m = item_of(self, 1); (void)sel; if (m) m->tag = t; }
static long mi_state(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? m->state : 0; }
static void mi_set_state(id self, SEL sel, long s) { mitem *m = item_of(self, 1); (void)sel; if (m) m->state = s; }
static signed char mi_enabled(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? (signed char)m->enabled : 1; }
static void mi_set_enabled(id self, SEL sel, signed char e) { mitem *m = item_of(self, 1); (void)sel; if (m) m->enabled = e != 0; }
static signed char mi_hidden(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? (signed char)m->hidden : 0; }
static void mi_set_hidden(id self, SEL sel, signed char h) { mitem *m = item_of(self, 1); (void)sel; if (m) m->hidden = h != 0; }
static signed char mi_is_sep(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? (signed char)m->sep : 0; }
static signed char mi_has_sub(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m && m->sub ? 1 : 0; }
static id mi_submenu(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? m->sub : NULL; }
static void mi_set_submenu(id self, SEL sel, id sub)
{
    mitem *m = item_of(self, 1);
    mmenu *s = menu_of(sub, 0);
    (void)sel;
    if (!m) return;
    m->sub = sub;
    if (s) s->super_ = m->menu;
}
static id mi_target(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? m->target : NULL; }
static void mi_set_target(id self, SEL sel, id t) { mitem *m = item_of(self, 1); (void)sel; if (m) m->target = t; }
static SEL mi_action(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m && m->action[0] ? m->action : NULL; }
static void mi_set_action(id self, SEL sel, SEL a)
{ mitem *m = item_of(self, 1); (void)sel; if (m) snprintf(m->action, sizeof m->action, "%s", a ? a : ""); }
static id mi_menu(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? m->menu : NULL; }
static id mi_repr(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? m->repr : NULL; }
static void mi_set_repr(id self, SEL sel, id r) { mitem *m = item_of(self, 1); (void)sel; if (m) m->repr = r; }
static id mi_key(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return macns_make_string(m ? m->key : ""); }
static void mi_set_key(id self, SEL sel, id k) { mitem *m = item_of(self, 1); (void)sel; if (m) set_str(m->key, sizeof m->key, k); }
static long mi_indent(id self, SEL sel) { mitem *m = item_of(self, 1); (void)sel; return m ? m->indent : 0; }
static void mi_set_indent(id self, SEL sel, long i) { mitem *m = item_of(self, 1); (void)sel; if (m) m->indent = i; }
static void mi_ignore1(id self, SEL sel, id a) { (void)self; (void)sel; (void)a; }
static void mi_ignore_long(id self, SEL sel, long a) { (void)self; (void)sel; (void)a; }

/* ------------------------------------------------------------------- NSMenu */

static id nm_init(id self, SEL sel) { (void)sel; menu_of(self, 1); return self; }
static id nm_init_title(id self, SEL sel, id title)
{
    mmenu *m = menu_of(self, 1);
    (void)sel;
    if (m) set_str(m->title, sizeof m->title, title);
    return self;
}
static id nm_title(id self, SEL sel)
{ mmenu *m = menu_of(self, 1); (void)sel; return macns_make_string(m ? m->title : ""); }
static void nm_set_title(id self, SEL sel, id t)
{ mmenu *m = menu_of(self, 1); (void)sel; if (m) set_str(m->title, sizeof m->title, t); }

static void nm_insert(mmenu *m, id item, long at)
{
    mitem *it = item_of(item, 1);
    if (!m || !it) return;
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 16;
        mitem **g = (mitem **)realloc(m->it, (size_t)nc * sizeof *g);
        if (!g) return;
        m->it = g; m->cap = nc;
    }
    if (at < 0 || at > m->n) at = m->n;
    memmove(&m->it[at + 1], &m->it[at], (size_t)(m->n - at) * sizeof *m->it);
    m->it[at] = it;
    m->n++;
    it->menu = m->obj;
}
static void nm_add_item(id self, SEL sel, id item)
{ (void)sel; nm_insert(menu_of(self, 1), item, -1); }
static void nm_insert_item(id self, SEL sel, id item, long at)
{ (void)sel; nm_insert(menu_of(self, 1), item, at); }
static id nm_add_with_title(id self, SEL sel, id title, SEL action, id key)
{
    id item = macns_new_object("NSMenuItem");
    (void)sel;
    if (!item) return NULL;
    mi_init_full(item, "initWithTitle:action:keyEquivalent:", title, action, key);
    nm_insert(menu_of(self, 1), item, -1);
    return item;
}
static id nm_insert_with_title(id self, SEL sel, id title, SEL action, id key, long at)
{
    id item = macns_new_object("NSMenuItem");
    (void)sel;
    if (!item) return NULL;
    mi_init_full(item, "initWithTitle:action:keyEquivalent:", title, action, key);
    nm_insert(menu_of(self, 1), item, at);
    return item;
}
static void nm_set_submenu(id self, SEL sel, id sub, id item)
{
    mitem *it = item_of(item, 1);
    (void)self; (void)sel;
    if (it) mi_set_submenu(item, "setSubmenu:", sub);
}
static long nm_count(id self, SEL sel) { mmenu *m = menu_of(self, 1); (void)sel; return m ? m->n : 0; }
static id nm_item_at(id self, SEL sel, long i)
{ mmenu *m = menu_of(self, 1); (void)sel; return (m && i >= 0 && i < m->n) ? m->it[i]->obj : NULL; }
static long nm_index_of(id self, SEL sel, id item)
{
    mmenu *m = menu_of(self, 1);
    int i;
    (void)sel;
    for (i = 0; m && i < m->n; i++) if (m->it[i]->obj == item) return i;
    return -1;
}
static long nm_index_of_tag(id self, SEL sel, long tag)
{
    mmenu *m = menu_of(self, 1);
    int i;
    (void)sel;
    for (i = 0; m && i < m->n; i++) if (m->it[i]->tag == tag) return i;
    return -1;
}
static id nm_item_with_tag(id self, SEL sel, long tag)
{ long i = nm_index_of_tag(self, sel, tag); return i >= 0 ? nm_item_at(self, sel, i) : NULL; }
static id nm_item_with_title(id self, SEL sel, id title)
{
    mmenu *m = menu_of(self, 1);
    char t[128];
    int i;
    (void)sel;
    set_str(t, sizeof t, title);
    for (i = 0; m && i < m->n; i++) if (!strcmp(m->it[i]->title, t)) return m->it[i]->obj;
    return NULL;
}
static void nm_remove_at(id self, SEL sel, long i)
{
    mmenu *m = menu_of(self, 1);
    (void)sel;
    if (!m || i < 0 || i >= m->n) return;
    m->it[i]->menu = NULL;
    memmove(&m->it[i], &m->it[i + 1], (size_t)(m->n - i - 1) * sizeof *m->it);
    m->n--;
}
static void nm_remove_item(id self, SEL sel, id item)
{ long i = nm_index_of(self, sel, item); if (i >= 0) nm_remove_at(self, sel, i); }
static void nm_remove_all(id self, SEL sel)
{
    mmenu *m = menu_of(self, 1);
    (void)sel;
    while (m && m->n) nm_remove_at(self, sel, m->n - 1);
}
static id nm_supermenu(id self, SEL sel) { mmenu *m = menu_of(self, 1); (void)sel; return m ? m->super_ : NULL; }
static id nm_delegate(id self, SEL sel) { mmenu *m = menu_of(self, 1); (void)sel; return m ? m->delegate : NULL; }
static void nm_set_delegate(id self, SEL sel, id d) { mmenu *m = menu_of(self, 1); (void)sel; if (m) m->delegate = d; }
static id nm_nil(id self, SEL sel) { (void)self; (void)sel; return NULL; }
static void nm_void(id self, SEL sel) { (void)self; (void)sel; }
static void nm_ignore1(id self, SEL sel, id a) { (void)self; (void)sel; (void)a; }
static void nm_ignore_bool(id self, SEL sel, signed char a) { (void)self; (void)sel; (void)a; }
static void nm_ignore_dbl(id self, SEL sel, double a) { (void)self; (void)sel; (void)a; }

/* ---------------------------------------------------------------- the popup */

typedef struct { mitem **map; int n, cap; } idmap;

/* A Windows-layer menu from an NSMenu, every command given a number that maps
 * back to its NSMenuItem. */
static void *build(mmenu *m, idmap *im, int depth)
{
    void *h = pe_menu_new();
    int i;
    if (!h || depth > 6) return h;
    for (i = 0; i < m->n; i++) {
        mitem *it = m->it[i];
        uint32_t f = 0;
        if (it->hidden) continue;
        if (it->sep) { pe_menu_append(h, MF_SEPARATOR, 0, NULL); continue; }
        if (!it->enabled) f |= MF_GRAYED;
        if (it->state) f |= MF_CHECKED;
        if (it->sub && menu_of(it->sub, 0)) {
            void *sh = build(menu_of(it->sub, 0), im, depth + 1);
            pe_menu_append(h, f | MF_POPUP, (uintptr_t)sh, it->title);
        } else {
            if (im->n + 2 > im->cap) {
                int nc = im->cap ? im->cap * 2 : 64;
                mitem **g = (mitem **)realloc(im->map, (size_t)nc * sizeof *g);
                if (!g) break;
                im->map = g; im->cap = nc;
            }
            im->map[++im->n] = it;
            pe_menu_append(h, f, (uintptr_t)im->n, it->title);
        }
    }
    return h;
}

/* Show the menu, wait, and send the chosen item's action to its target. YES
 * when something was chosen. `x`, `y` are in the editor's frame, top left first. */
static signed char popup_at(id self, int x, int y)
{
    mmenu *m = menu_of(self, 1);
    idmap im = { NULL, 0, 0 };
    void *h;
    int chosen;
    signed char ok = 0;
    if (!m || !m->n) { if (getenv("MACOBJC_VERBOSE")) fprintf(stderr, "  [menu] popup of an empty menu\n"); return 0; }
    h = build(m, &im, 0);
    chosen = h ? pe_menu_popup(h, x, y) : 0;
    if (getenv("MACOBJC_VERBOSE"))
        fprintf(stderr, "  [menu] popup of %d item(s) at %d,%d -> %d\n", m->n, x, y, chosen);
    if (h) pe_menu_destroy(h);
    if (chosen > 0 && chosen <= im.n) {
        mitem *it = im.map[chosen];
        ok = 1;
        if (it->action[0] && it->target) {
            void (*imp)(id, SEL, id) = (void (*)(id, SEL, id))macobjc_lookup(it->target, it->action);
            if (imp) imp(it->target, it->action, it->obj);
        }
    }
    free(im.map);
    return ok;
}

/* The location is in the view's coordinates, which for iPlug2's flipped view
 * count down from the top; for an ordinary view they count up from the bottom,
 * and with no view at all they are screen coordinates, for which the pointer is
 * the best place to open. */
static signed char nm_popup(id self, SEL sel, id item, double lx, double ly, id view)
{
    int x, y;
    (void)sel; (void)item;
    if (!view) pe_pointer(&x, &y);
    else {
        signed char (*flipped)(id, SEL) = (signed char (*)(id, SEL))macobjc_lookup(view, "isFlipped");
        int fh = pe_frame_height();
        x = (int)lx;
        y = (flipped && flipped(view, "isFlipped")) || fh <= 0 ? (int)ly : fh - (int)ly;
    }
    return popup_at(self, x, y);
}

/* +popUpContextMenu:withEvent:forView: opens at the event, wherever that was. */
static void nm_context(id cls, SEL sel, id menu, id event, id view)
{
    int x, y;
    (void)cls; (void)sel; (void)event; (void)view;
    pe_pointer(&x, &y);
    popup_at(menu, x, y);
}
/* -popUpMenu... on an NSPopUpButton's cell, and other spellings of the same ask. */
static signed char nm_popup_simple(id self, SEL sel)
{
    int x, y;
    (void)sel;
    pe_pointer(&x, &y);
    return popup_at(self, x, y);
}

/* ------------------------------------------------------------ installation */

typedef struct { const char *cls, *sel; void *imp; } entry;
static const entry g_table[] = {
    { "NSMenuItem", "+separatorItem",                   mi_separator },
    { "NSMenuItem", "init",                             mi_init },
    { "NSMenuItem", "initWithTitle:action:keyEquivalent:", mi_init_full },
    { "NSMenuItem", "title",                            mi_title },
    { "NSMenuItem", "setTitle:",                        mi_set_title },
    { "NSMenuItem", "tag",                              mi_tag },
    { "NSMenuItem", "setTag:",                          mi_set_tag },
    { "NSMenuItem", "state",                            mi_state },
    { "NSMenuItem", "setState:",                        mi_set_state },
    { "NSMenuItem", "isEnabled",                        mi_enabled },
    { "NSMenuItem", "setEnabled:",                      mi_set_enabled },
    { "NSMenuItem", "isHidden",                         mi_hidden },
    { "NSMenuItem", "setHidden:",                       mi_set_hidden },
    { "NSMenuItem", "isSeparatorItem",                  mi_is_sep },
    { "NSMenuItem", "hasSubmenu",                       mi_has_sub },
    { "NSMenuItem", "submenu",                          mi_submenu },
    { "NSMenuItem", "setSubmenu:",                      mi_set_submenu },
    { "NSMenuItem", "target",                           mi_target },
    { "NSMenuItem", "setTarget:",                       mi_set_target },
    { "NSMenuItem", "action",                           mi_action },
    { "NSMenuItem", "setAction:",                       mi_set_action },
    { "NSMenuItem", "menu",                             mi_menu },
    { "NSMenuItem", "representedObject",                mi_repr },
    { "NSMenuItem", "setRepresentedObject:",            mi_set_repr },
    { "NSMenuItem", "keyEquivalent",                    mi_key },
    { "NSMenuItem", "setKeyEquivalent:",                mi_set_key },
    { "NSMenuItem", "indentationLevel",                 mi_indent },
    { "NSMenuItem", "setIndentationLevel:",             mi_set_indent },
    { "NSMenuItem", "setKeyEquivalentModifierMask:",    mi_ignore_long },
    { "NSMenuItem", "setImage:",                        mi_ignore1 },
    { "NSMenuItem", "setOnStateImage:",                 mi_ignore1 },
    { "NSMenuItem", "setOffStateImage:",                mi_ignore1 },
    { "NSMenuItem", "setMixedStateImage:",              mi_ignore1 },
    { "NSMenuItem", "setAttributedTitle:",              mi_ignore1 },
    { "NSMenuItem", "setToolTip:",                      mi_ignore1 },

    { "NSMenu", "init",                                 nm_init },
    { "NSMenu", "initWithTitle:",                       nm_init_title },
    { "NSMenu", "title",                                nm_title },
    { "NSMenu", "setTitle:",                            nm_set_title },
    { "NSMenu", "addItem:",                             nm_add_item },
    { "NSMenu", "insertItem:atIndex:",                  nm_insert_item },
    { "NSMenu", "addItemWithTitle:action:keyEquivalent:", nm_add_with_title },
    { "NSMenu", "insertItemWithTitle:action:keyEquivalent:atIndex:", nm_insert_with_title },
    { "NSMenu", "setSubmenu:forItem:",                  nm_set_submenu },
    { "NSMenu", "numberOfItems",                        nm_count },
    { "NSMenu", "itemAtIndex:",                         nm_item_at },
    { "NSMenu", "indexOfItem:",                         nm_index_of },
    { "NSMenu", "indexOfItemWithTag:",                  nm_index_of_tag },
    { "NSMenu", "itemWithTag:",                         nm_item_with_tag },
    { "NSMenu", "itemWithTitle:",                       nm_item_with_title },
    { "NSMenu", "removeItemAtIndex:",                   nm_remove_at },
    { "NSMenu", "removeItem:",                          nm_remove_item },
    { "NSMenu", "removeAllItems",                       nm_remove_all },
    { "NSMenu", "supermenu",                            nm_supermenu },
    { "NSMenu", "delegate",                             nm_delegate },
    { "NSMenu", "setDelegate:",                         nm_set_delegate },
    { "NSMenu", "highlightedItem",                      nm_nil },
    { "NSMenu", "update",                               nm_void },
    { "NSMenu", "setAutoenablesItems:",                 nm_ignore_bool },
    { "NSMenu", "setShowsStateColumn:",                 nm_ignore_bool },
    { "NSMenu", "setFont:",                             nm_ignore1 },
    { "NSMenu", "setMinimumWidth:",                     nm_ignore_dbl },
    { "NSMenu", "popUpMenuPositioningItem:atLocation:inView:", nm_popup },
    { "NSMenu", "+popUpContextMenu:withEvent:forView:", nm_context },
    { "NSMenu", "popUpMenu",                            nm_popup_simple },
    { NULL, NULL, NULL }
};

void macmenu_install(void)
{
    int i;
    for (i = 0; g_table[i].cls; i++)
        macobjc_add_method(g_table[i].cls, g_table[i].sel, g_table[i].imp);
}
