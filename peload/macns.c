/* Foundation classes with real behaviour.
 *
 * The stand-in classes in macobjc.c let an image bind and answer alloc/retain/
 * release. That is not enough to run: a plugin asks NSString for its bytes and
 * NSArray for its contents, and a nil answer is dereferenced. So the classes it
 * actually uses get implemented here, one at a time, driven by the missed
 * selector log rather than by guessing at the whole framework.
 *
 * Every method takes (self, sel, ...) because that is what objc_msgSend hands
 * an implementation. Strings hold UTF-8 and convert at the unichar boundaries.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "macobjc.h"
#include "macshim.h"


typedef const char *SEL;
typedef void *id;

/* Instance layouts. The first two words match macobjc.c's obj_header, which is
 * what alloc fills in; everything after is ours. */
typedef struct { void *isa; long refs; char *s; long len; long cap; } nsstring;
typedef struct { void *isa; long refs; id *v; long n, cap; }        nsarray;
typedef struct { void *isa; long refs; id *k, *v; long n, cap; }    nsdict;
typedef struct { void *isa; long refs; double d; long i; int isint; } nsnumber;
typedef struct { void *isa; long refs; uint8_t *b; long n, cap; }   nsdata;

/* ------------------------------------------------------------------ helpers */

static id ns_new_of(const char *cls)
{
    /* Allocate through the class so the isa and refcount are set the same way an
     * ordinary [Foo alloc] would set them. */
    void *k = macobjc_class(cls);
    id (*alloc)(id, SEL);
    if (!k) return NULL;
    alloc = (id (*)(id, SEL))macobjc_lookup(k, "alloc");
    return alloc ? alloc(k, "alloc") : NULL;
}

static id str_make(const char *utf8, long n)
{
    nsstring *s = ns_new_of("NSString");
    if (!s) return NULL;
    if (n < 0) n = utf8 ? (long)strlen(utf8) : 0;
    s->cap = n + 1;
    s->s = malloc((size_t)s->cap);
    if (!s->s) return s;
    if (utf8 && n) memcpy(s->s, utf8, (size_t)n);
    s->s[n] = 0;
    s->len = n;
    return s;
}

/* Only objects allocated through our NSString carry our layout, so mutation has
 * to go through this. */
static nsstring *as_str(id p)
{
    if (!p) return NULL;
    return macobjc_isa_named(p, "NSString") || macobjc_isa_named(p, "NSMutableString")
           ? (nsstring *)p : NULL;
}

/* Reading a string is different from mutating one: an @"..." literal is not an
 * object at all. The compiler emits a constant CFString whose isa points at
 * ___CFConstantStringClassReference, so it fails an isa test against our class --
 * and treating that as "not a string" silently drops it.
 *
 * That is what broke every path a plugin builds: it appends resourcePath, @"/",
 * a name and @".png", and the two literal separators vanished, leaving
 * ".../Resourcesleftpng". Anything that only reads a string goes through here. */
static const char *str_bytes(id p, long *len)
{
    static char scratch[4][2048];
    static int turn;
    nsstring *s = as_str(p);
    char *buf;

    if (len) *len = 0;
    if (!p) return NULL;
    if (s) { if (len) *len = s->len; return s->s ? s->s : ""; }

    /* A CoreFoundation string, constant or otherwise. The buffers rotate because
     * callers legitimately hold two or three of these at once while joining. */
    buf = scratch[turn++ & 3];
    if (!macshim_cf_string_get(p, buf, (long)sizeof scratch[0])) return NULL;
    if (len) *len = (long)strlen(buf);
    return buf;
}

static long u8_to_u16(const char *s, long n, uint16_t *out, long cap)
{
    long i = 0, w = 0;
    while (i < n && w < cap) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int len;
        if (c < 0x80)        { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { cp = 0xFFFD; len = 1; }
        if (i + len > n) break;
        { int k; for (k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F); }
        i += len;
        if (cp < 0x10000) out[w++] = (uint16_t)cp;
        else if (w + 2 <= cap) {
            cp -= 0x10000;
            out[w++] = (uint16_t)(0xD800 + (cp >> 10));
            out[w++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else break;
    }
    return w;
}

/* ------------------------------------------------------------------ NSString */

static id str_with_cstring(id self, SEL sel, const char *c, long enc)
{ (void)self; (void)sel; (void)enc; return str_make(c, -1); }
static id str_with_utf8(id self, SEL sel, const char *c)
{ (void)self; (void)sel; return str_make(c, -1); }
static id str_empty(id self, SEL sel) { (void)self; (void)sel; return str_make("", 0); }

static id str_init_cstring(id self, SEL sel, const char *c, long enc)
{
    nsstring *s = as_str(self);
    long n;
    (void)sel; (void)enc;
    if (!s) return self;
    n = c ? (long)strlen(c) : 0;
    free(s->s);
    s->cap = n + 1;
    s->s = malloc((size_t)s->cap);
    if (s->s) { if (n) memcpy(s->s, c, (size_t)n); s->s[n] = 0; s->len = n; }
    return self;
}
static id str_init_utf8(id self, SEL sel, const char *c)
{ return str_init_cstring(self, sel, c, 4); }
static id str_init(id self, SEL sel) { return str_init_cstring(self, sel, "", 4); }

static const char *str_utf8(id self, SEL sel)
{ const char *s = str_bytes(self, NULL); (void)sel; return s ? s : ""; }
static const char *str_cstring_enc(id self, SEL sel, long enc)
{ (void)enc; return str_utf8(self, sel); }
static long str_length(id self, SEL sel)
{
    /* -length counts UTF-16 units, not bytes: a plugin indexing a string with it
     * would run off the end if this returned the byte count. */
    long n = 0, i = 0, units = 0;
    const char *b = str_bytes(self, &n);
    (void)sel;
    if (!b) return 0;
    while (i < n) {
        unsigned char c = (unsigned char)b[i];
        int len = (c < 0x80) ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        units += (len == 4) ? 2 : 1;              /* astral planes take a pair */
        i += len;
    }
    return units;
}

static signed char str_is_equal(id self, SEL sel, id other)
{
    long al = 0, bl = 0;
    const char *a = str_bytes(self, &al), *b = str_bytes(other, &bl);
    (void)sel;
    if (!a || !b) return (self == other) ? 1 : 0;
    return (al == bl && !memcmp(a, b, (size_t)al)) ? 1 : 0;
}
static id str_copy(id self, SEL sel)
{ long n = 0; const char *b = str_bytes(self, &n); (void)sel;
  return b ? str_make(b, n) : NULL; }
static id str_copy_zone(id self, SEL sel, void *z) { (void)z; return str_copy(self, sel); }
static id str_description(id self, SEL sel) { (void)sel; return self; }
static unsigned long str_hash(id self, SEL sel)
{
    long n = 0, i;
    const char *b = str_bytes(self, &n);
    unsigned long h = 5381;
    (void)sel;
    if (!b) return 0;
    for (i = 0; i < n; i++) h = h * 33 + (unsigned char)b[i];
    return h;
}
static uint16_t str_char_at(id self, SEL sel, long idx)
{
    long len = 0;
    const char *b = str_bytes(self, &len);
    uint16_t buf[8];
    (void)sel;
    if (!b) return 0;
    /* Small strings dominate here; converting the prefix is simpler than an
     * index cache and correct for surrogate pairs. */
    { long n = u8_to_u16(b, len, buf, 8);
      if (idx >= 0 && idx < n) return buf[idx]; }
    {
        uint16_t *big = malloc(((size_t)len + 1) * 2);
        uint16_t r = 0;
        if (big) {
            long n = u8_to_u16(b, len, big, len + 1);
            if (idx >= 0 && idx < n) r = big[idx];
            free(big);
        }
        return r;
    }
}

/* +stringWithFormat: and -appendFormat: cover %@ %s %d %u %f %g %% -- the set
 * that appears in parameter and preset text. */
static void fmt_into(char *out, size_t cap, const char *f, va_list ap)
{
    size_t w = 0;
    out[0] = 0;
    if (!f) return;
    while (*f && w + 1 < cap) {
        if (*f != '%') { out[w++] = *f++; continue; }
        f++;
        while (*f && strchr("0123456789.-+ #", *f)) f++;
        switch (*f) {
        case '@': { const char *s = str_bytes(va_arg(ap, id), NULL);
                    w += (size_t)snprintf(out + w, cap - w, "%s", s ? s : "(null)");
                    break; }
        case 's': w += (size_t)snprintf(out + w, cap - w, "%s", va_arg(ap, const char *)); break;
        case 'd': case 'i':
                  w += (size_t)snprintf(out + w, cap - w, "%d", va_arg(ap, int)); break;
        case 'u': w += (size_t)snprintf(out + w, cap - w, "%u", va_arg(ap, unsigned)); break;
        case 'f': case 'g':
                  w += (size_t)snprintf(out + w, cap - w, "%g", va_arg(ap, double)); break;
        case '%': out[w++] = '%'; break;
        default:  out[w++] = '%'; if (*f) out[w++] = *f; break;
        }
        if (*f) f++;
        if (w >= cap) w = cap - 1;
    }
    out[w] = 0;
}

/* The format is a literal in almost every call, so it has to be read the way
 * literals are read -- an isa test against our class fails on one, and taking
 * that for "no format" produced an empty string from every +stringWithFormat:
 * in the process. */
static id str_with_format(id self, SEL sel, id fmt, ...)
{
    char buf[1024];
    const char *f = str_bytes(fmt, NULL);
    va_list ap;
    (void)self; (void)sel;
    va_start(ap, fmt);
    fmt_into(buf, sizeof buf, f ? f : "", ap);
    va_end(ap);
    return str_make(buf, -1);
}

/* --------------------------------------------------------- NSMutableString */

static id mstr_with_capacity(id self, SEL sel, long cap)
{ (void)self; (void)sel; (void)cap; return str_make("", 0); }

static void mstr_set_string(id self, SEL sel, id other);
static id str_with_string(id self, SEL sel, id other)
{
    const char *t; long n = 0;
    (void)self; (void)sel;
    t = str_bytes(other, &n);
    return str_make(t ? t : "", t ? n : 0);
}
static id mstr_init_with_string(id self, SEL sel, id other)
{ mstr_set_string(self, sel, other); return self; }

static void mstr_append(id self, SEL sel, id other)
{
    nsstring *s = as_str(self);
    const char *src;
    long n = 0;
    (void)sel;
    /* Only the receiver has to be ours; the argument is read, so a literal or a
     * CFString counts. Appending @"/" between two paths is the common case and
     * it used to vanish. */
    if (!s || !(src = str_bytes(other, &n)) || !n) return;
    if (s->len + n + 1 > s->cap) {
        long nc = (s->len + n + 1) * 2;
        char *p = realloc(s->s, (size_t)nc);
        if (!p) return;
        /* [x appendString:x] reads the buffer the realloc just moved. */
        if (src == s->s) src = p;
        s->s = p; s->cap = nc;
    }
    memmove(s->s + s->len, src, (size_t)n);
    s->len += n;
    s->s[s->len] = 0;
}
static void mstr_append_format(id self, SEL sel, id fmt, ...)
{
    char buf[1024];
    const char *f = str_bytes(fmt, NULL);
    va_list ap;
    id tmp;
    (void)sel;
    va_start(ap, fmt);
    fmt_into(buf, sizeof buf, f ? f : "", ap);
    va_end(ap);
    tmp = str_make(buf, -1);
    mstr_append(self, "appendString:", tmp);
}
static void mstr_set_string(id self, SEL sel, id other)
{
    nsstring *s = as_str(self);
    (void)sel;
    if (s) { s->len = 0; if (s->s) s->s[0] = 0; }
    mstr_append(self, "appendString:", other);
}

/* ----------------------------------------------------------------- NSArray */

static id arr_make(void)
{
    nsarray *a = ns_new_of("NSMutableArray");
    if (!a) return NULL;
    a->cap = 8;
    a->v = calloc((size_t)a->cap, sizeof *a->v);
    return a;
}
static nsarray *as_arr(id p)
{
    return (p && (macobjc_isa_named(p, "NSArray") || macobjc_isa_named(p, "NSMutableArray")))
           ? (nsarray *)p : NULL;
}
static id arr_new(id self, SEL sel) { (void)self; (void)sel; return arr_make(); }
static id arr_with_capacity(id self, SEL sel, long c)
{ (void)self; (void)sel; (void)c; return arr_make(); }
static long arr_count(id self, SEL sel)
{ nsarray *a = as_arr(self); (void)sel; return a ? a->n : 0; }
static id arr_object_at(id self, SEL sel, long i)
{
    nsarray *a = as_arr(self);
    (void)sel;
    return (a && i >= 0 && i < a->n) ? a->v[i] : NULL;
}
static void arr_add(id self, SEL sel, id o)
{
    nsarray *a = as_arr(self);
    (void)sel;
    if (!a) return;
    if (a->n == a->cap) {
        long nc = a->cap * 2;
        id *nv = realloc(a->v, (size_t)nc * sizeof *nv);
        if (!nv) return;
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = o;
}
static void arr_remove_all(id self, SEL sel)
{ nsarray *a = as_arr(self); (void)sel; if (a) a->n = 0; }

/* ------------------------------------------------------------------ NSNumber */

static id num_make(double d, long i, int isint)
{
    nsnumber *n = ns_new_of("NSNumber");
    if (!n) return NULL;
    n->d = d; n->i = i; n->isint = isint;
    return n;
}
static nsnumber *as_num(id p)
{ return (p && macobjc_isa_named(p, "NSNumber")) ? (nsnumber *)p : NULL; }
static id num_with_int(id s, SEL sl, int v)     { (void)s; (void)sl; return num_make(v, v, 1); }
static id num_with_float(id s, SEL sl, float v) { (void)s; (void)sl; return num_make(v, (long)v, 0); }
static id num_with_double(id s, SEL sl, double v){ (void)s; (void)sl; return num_make(v, (long)v, 0); }
static id num_with_bool(id s, SEL sl, signed char v)
{ (void)s; (void)sl; return num_make(v ? 1 : 0, v ? 1 : 0, 1); }
static int   num_int(id self, SEL sel)    { nsnumber *n = as_num(self); (void)sel; return n ? (int)n->i : 0; }
static float num_float(id self, SEL sel)  { nsnumber *n = as_num(self); (void)sel; return n ? (float)n->d : 0.0f; }
static double num_double(id self, SEL sel){ nsnumber *n = as_num(self); (void)sel; return n ? n->d : 0.0; }
static signed char num_bool(id self, SEL sel) { return num_int(self, sel) != 0; }

/* -------------------------------------------------------------- NSDictionary */

static nsdict *as_dict(id p)
{
    return (p && (macobjc_isa_named(p, "NSDictionary") ||
                  macobjc_isa_named(p, "NSMutableDictionary"))) ? (nsdict *)p : NULL;
}
static id dict_make(void)
{
    nsdict *d = ns_new_of("NSMutableDictionary");
    if (!d) return NULL;
    d->cap = 8;
    d->k = calloc((size_t)d->cap, sizeof *d->k);
    d->v = calloc((size_t)d->cap, sizeof *d->v);
    return d;
}
static id dict_new(id self, SEL sel) { (void)self; (void)sel; return dict_make(); }
static id dict_with_capacity(id self, SEL sel, long c)
{ (void)self; (void)sel; (void)c; return dict_make(); }
static long dict_find(nsdict *d, id key)
{
    long i;
    for (i = 0; i < d->n; i++) {
        if (d->k[i] == key) return i;
        if (str_is_equal(d->k[i], "isEqualToString:", key)) return i;
    }
    return -1;
}
static id dict_object_for(id self, SEL sel, id key)
{
    nsdict *d = as_dict(self);
    long at;
    (void)sel;
    if (!d) return NULL;
    return (at = dict_find(d, key)) >= 0 ? d->v[at] : NULL;
}
static void dict_set(id self, SEL sel, id val, id key)
{
    nsdict *d = as_dict(self);
    long at;
    (void)sel;
    if (!d) return;
    if ((at = dict_find(d, key)) >= 0) { d->v[at] = val; return; }
    if (d->n == d->cap) {
        long nc = d->cap * 2;
        id *nk = realloc(d->k, (size_t)nc * sizeof *nk);
        id *nv = realloc(d->v, (size_t)nc * sizeof *nv);
        if (!nk || !nv) { if (nk) d->k = nk; if (nv) d->v = nv; return; }
        d->k = nk; d->v = nv; d->cap = nc;
    }
    d->k[d->n] = key; d->v[d->n] = val; d->n++;
}
static long dict_count(id self, SEL sel)
{ nsdict *d = as_dict(self); (void)sel; return d ? d->n : 0; }

/* ------------------------------------------------------------------- NSView */

/* A plugin's editor is its own NSView subclass, and it inherits the whole view
 * lifecycle from NSView -- geometry, the subview tree, invalidation. So these
 * have to behave, not merely exist: -initWithFrame: returning nil is what the
 * plugin then dereferences.
 *
 * NSRect is four doubles. At 32 bytes it is a MEMORY-class type, so it arrives on
 * the stack and is returned through the hidden pointer that objc_msgSend_stret
 * carries -- which is why that trampoline had to exist before any of this could
 * work.
 */
typedef struct { double x, y; } NSPoint;
typedef struct { double w, h; } NSSize;
typedef struct { NSPoint origin; NSSize size; } NSRect;

/* The screen this host pretends to have. One number, because a plugin converts
 * between screen coordinates counted from the bottom and window coordinates
 * counted from the top and expects both to be measured against the same
 * display -- see the pointer section further down for what disagreeing cost. */
#define SCREEN_W 1920
#define SCREEN_H 1080

/* A view's state deliberately lives *outside* the object.
 *
 * Objective-C's modern runtime has non-fragile ivars: the compiler emits a
 * subclass's ivar offsets against whatever superclass size it saw in the SDK,
 * and the runtime slides them when it realizes the class. Nothing here realizes
 * classes, so those compile-time offsets stand as written -- which means any
 * field kept inside the instance lands on top of one of the plugin's own ivars.
 *
 * It cost a crash to find. This struct's frame.size.height sat at offset 40,
 * exactly where IGraphicsView keeps mTextFieldView, so sizing the editor stored
 * 648.0 over the text field and tearing the editor down sent setDelegate: to a
 * double. Anything a plugin can subclass has to be stored to the side. */
typedef struct view_state {
    struct view_state *next;
    id     obj;
    NSRect frame, bounds;
    id     superview;
    id     subviews;                 /* an NSMutableArray */
    id     window;
    int    needs_display, hidden;
    /* What actually needs repainting. A view that says "this knob changed" and
     * is told to redraw its whole self repaints every control it owns, which on
     * a 858x648 editor is most of a million pixels for a dial that covers a few
     * thousand -- see the dirty-rect note above macns_draw_dirty. */
    NSRect dirty;
    void  *layer;
    /* An NSControl's cell, for the text-entry path: the plugin builds one, hands
     * it over with -setCell:, and reads it back. */
    id     cell;
} nsview;

#define VIEW_BUCKETS 64
static nsview *g_views[VIEW_BUCKETS];

static nsview *as_view(id p)
{
    unsigned h;
    nsview *v;

    if (!p || !macobjc_isa_named(p, "NSView")) return NULL;
    h = (unsigned)(((uintptr_t)p >> 4) % VIEW_BUCKETS);
    for (v = g_views[h]; v; v = v->next)
        if (v->obj == p) return v;
    if (!(v = calloc(1, sizeof *v))) return NULL;
    v->obj = p;
    v->next = g_views[h];
    g_views[h] = v;
    return v;
}

static id view_init_frame(id self, SEL sel, NSRect r)
{
    nsview *v = as_view(self);
    (void)sel;
    if (!v) return self;
    /* Reset rather than merely assign: the entry may be left over from a view at
     * the same address that has since been freed. */
    v->superview = v->window = NULL;
    v->layer = NULL;
    v->needs_display = v->hidden = 0;
    memset(&v->dirty, 0, sizeof v->dirty);
    v->frame = r;
    /* A view's bounds start at the origin with the frame's size. */
    v->bounds.origin.x = 0.0; v->bounds.origin.y = 0.0;
    v->bounds.size = r.size;
    v->subviews = arr_make();
    return self;
}
/* An initialiser that only has to return the object. Enough for the cells and
 * the odds and ends that carry no state this host keeps. */
static id view_init_self(id self, SEL sel, id arg)
{ (void)sel; (void)arg; return self; }

/* ---- NSControl and NSCell ---------------------------------------------
 *
 * A plugin's text entry is a stack of three: an NSTextField subclass, an
 * NSTextFieldCell subclass inside it, and a string. Almost every message it
 * sends them is configuration -- editable, bordered, background, alignment --
 * which nothing here draws and which therefore only has to be accepted. What
 * does have to be real is the cell: the plugin sets one and reads it back, and
 * a nil answer is a control it then sends messages to. */
static void ctl_set1(id self, SEL sel, id arg)
{ (void)self; (void)sel; (void)arg; }
static void ctl_set_flag(id self, SEL sel, signed char v)
{ (void)self; (void)sel; (void)v; }
static void ctl_set_cell(id self, SEL sel, id c)
{ nsview *v = as_view(self); (void)sel; if (v) v->cell = c; }
static id   ctl_cell(id self, SEL sel)
{ nsview *v = as_view(self); (void)sel; return v ? v->cell : NULL; }
static id   ctl_nil(id self, SEL sel) { (void)self; (void)sel; return NULL; }
static id view_init(id self, SEL sel)
{
    NSRect z;
    memset(&z, 0, sizeof z);
    return view_init_frame(self, sel, z);
}
static NSRect view_frame(id self, SEL sel)
{
    nsview *v = as_view(self);
    NSRect z;
    (void)sel;
    if (v) return v->frame;
    memset(&z, 0, sizeof z);
    return z;
}
static NSRect view_bounds(id self, SEL sel)
{
    nsview *v = as_view(self);
    NSRect z;
    (void)sel;
    if (v) return v->bounds;
    memset(&z, 0, sizeof z);
    return z;
}
static void view_set_frame(id self, SEL sel, NSRect r)
{
    nsview *v = as_view(self);
    (void)sel;
    if (!v) return;
    v->frame = r;
    v->bounds.size = r.size;
}
static void view_set_bounds(id self, SEL sel, NSRect r)
{ nsview *v = as_view(self); (void)sel; if (v) v->bounds = r; }
static void view_set_frame_size(id self, SEL sel, NSSize s)
{
    nsview *v = as_view(self);
    (void)sel;
    if (!v) return;
    v->frame.size = s;
    v->bounds.size = s;
}
static void view_add_subview(id self, SEL sel, id sub)
{
    nsview *v = as_view(self), *s = as_view(sub);
    (void)sel;
    if (!v || !sub) return;
    if (!v->subviews) v->subviews = arr_make();
    arr_add(v->subviews, "addObject:", sub);
    if (s) { s->superview = self; s->window = v->window; }
}
static void view_remove_from_super(id self, SEL sel)
{
    nsview *v = as_view(self);
    (void)sel;
    if (v) { v->superview = NULL; v->window = NULL; }
}
static id   view_superview(id self, SEL sel)
{ nsview *v = as_view(self); (void)sel; return v ? v->superview : NULL; }
static id   view_subviews(id self, SEL sel)
{ nsview *v = as_view(self); (void)sel; return v ? v->subviews : NULL; }
/* The view a layer was installed on, or the first to ask to be redrawn -- the
 * one the editor actually draws into, and the one input is delivered to. */
static id g_editor_view;

/* The editor view, but only while it is still one.
 *
 * A view belongs to the plugin that built it, and its class lives in that
 * plugin's image. When the image is unmapped the pointer stays behind, and the
 * next thing to message it walks a class structure that is no longer there --
 * which is a fault in the host, on whatever the *next* plugin was doing.
 * as_view answers no for an object whose class the runtime no longer knows,
 * which is exactly the question worth asking here, so a close path that forgot
 * to reset costs nothing rather than the process. */
static id live_editor_view(void)
{
    if (g_editor_view && !as_view(g_editor_view)) g_editor_view = NULL;
    return g_editor_view;
}


/* ---- the window --------------------------------------------------------
 *
 * There is no window server here, and for a long time no window either: a view
 * answered `nil` and that seemed harmless, because nothing is ever shown. It is
 * not harmless. A plugin that grabs a control asks its window to convert the
 * cursor's screen position into window coordinates, and a message to nil
 * returns zero -- so iPlug2 stored the mouse-down position as (0,0), and the
 * first drag after the press moved the control by the entire distance from the
 * top-left corner. Every macOS editor here could be clicked and not dragged.
 *
 * So there is a window, and its geometry is chosen to make the two coordinate
 * systems agree with what macns_post_mouse already synthesises: an event's
 * locationInWindow is the pointer's position measured from the *view's* bottom
 * edge, which puts the window's origin at the top-left of the screen. Everything
 * below follows from that one placement. */
static double editor_height(void)
{
    nsview *v = as_view(g_editor_view);
    return v ? v->bounds.size.h : 0.0;
}
static double editor_width(void)
{
    nsview *v = as_view(g_editor_view);
    return v ? v->bounds.size.w : 0.0;
}
/* Screen y of the window's bottom edge: the window is flush with the top of the
 * screen, so its bottom is a view's height down from the top. */
static double window_origin_y(void) { return (double)SCREEN_H - editor_height(); }

static id   view_window(id self, SEL sel)
{
    static id cached;
    nsview *v = as_view(self);
    (void)sel;
    if (v && v->window) return v->window;
    if (!cached) cached = ns_new_of("NSWindow");
    if (v) v->window = cached;
    return cached;
}

static NSRect win_frame(id self, SEL sel)
{
    NSRect r;
    (void)self; (void)sel;
    r.origin.x = 0.0;
    r.origin.y = window_origin_y();
    r.size.w = editor_width();
    r.size.h = editor_height();
    return r;
}
/* Where the pointer is, in the window's coordinates.
 *
 * A control that tracks a gesture by polling rather than by waiting for
 * mouseDragged: asks this, and Dr. Device's XY pad is one of them: it reads the
 * pointer four times across a drag and puts its handle wherever the answer
 * says. Unimplemented, the answer was the zero point every time, so the pad
 * animated -- the picture changed on fifty-eight of fifty-nine frames -- while
 * the handle sat in the corner and the parameters never moved. It looked like
 * the control ignoring the mouse; it was the control being told the mouse was
 * at the origin.
 *
 * Same convention and same virtual pointer as the events carry, so a plugin
 * that mixes the two sees one pointer rather than two. */
static NSPoint win_mouse_location(id self, SEL sel)
{
    NSPoint p;
    double h = editor_height();
    (void)self; (void)sel;
    macns_pointer(&p.x, &p.y);
    if (h > 0.0) p.y = h - p.y;
    return p;
}
/* A control asking to take the keyboard is told it did. Answering nil here is a
 * "no", and a control that checks gives up whatever it was starting. */
static signed char win_make_first_responder(id self, SEL sel, id who)
{ (void)self; (void)sel; (void)who; return 1; }

static NSRect win_rect_from_screen(id self, SEL sel, NSRect r)
{ (void)self; (void)sel; r.origin.y -= window_origin_y(); return r; }
static NSRect win_rect_to_screen(id self, SEL sel, NSRect r)
{ (void)self; (void)sel; r.origin.y += window_origin_y(); return r; }
static NSPoint win_point_from_screen(id self, SEL sel, NSPoint p)
{ (void)self; (void)sel; p.y -= window_origin_y(); return p; }
static NSPoint win_point_to_screen(id self, SEL sel, NSPoint p)
{ (void)self; (void)sel; p.y += window_origin_y(); return p; }
static id win_content_view(id self, SEL sel) { (void)self; (void)sel; return g_editor_view; }
static long win_number(id self, SEL sel) { (void)self; (void)sel; return 1; }

/* A view that asks to be redrawn is the view that draws, which is how an editor
 * with no layer is recognised -- g_editor_view is otherwise only set when a
 * CAMetalLayer is installed, and a Core Graphics editor never installs one. */
/* The rectangle drawRect: is running for, so the two questions a plugin asks
 * from inside it -- getRectsBeingDrawn:count: and needsToDrawRect: -- have the
 * same answer AppKit would give. Empty outside a draw. */
static NSRect g_draw_rect;

static int rect_empty(NSRect r)
{ return r.size.w <= 0.0 || r.size.h <= 0.0; }

/* The smallest rectangle covering both. An empty one contributes nothing --
 * without that, a union starting from a zeroed rect always includes the
 * origin and the "dirty" area is the whole top-left quadrant. */
static NSRect rect_union(NSRect a, NSRect b)
{
    double x0, y0, x1, y1;
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    x0 = a.origin.x < b.origin.x ? a.origin.x : b.origin.x;
    y0 = a.origin.y < b.origin.y ? a.origin.y : b.origin.y;
    x1 = a.origin.x + a.size.w > b.origin.x + b.size.w
       ? a.origin.x + a.size.w : b.origin.x + b.size.w;
    y1 = a.origin.y + a.size.h > b.origin.y + b.size.h
       ? a.origin.y + a.size.h : b.origin.y + b.size.h;
    a.origin.x = x0; a.origin.y = y0; a.size.w = x1 - x0; a.size.h = y1 - y0;
    return a;
}
static NSRect rect_intersect(NSRect a, NSRect b)
{
    double x0, y0, x1, y1;
    NSRect r;
    x0 = a.origin.x > b.origin.x ? a.origin.x : b.origin.x;
    y0 = a.origin.y > b.origin.y ? a.origin.y : b.origin.y;
    x1 = a.origin.x + a.size.w < b.origin.x + b.size.w
       ? a.origin.x + a.size.w : b.origin.x + b.size.w;
    y1 = a.origin.y + a.size.h < b.origin.y + b.size.h
       ? a.origin.y + a.size.h : b.origin.y + b.size.h;
    r.origin.x = x0; r.origin.y = y0;
    r.size.w = x1 - x0 > 0.0 ? x1 - x0 : 0.0;
    r.size.h = y1 - y0 > 0.0 ? y1 - y0 : 0.0;
    return r;
}
static int rect_meets(NSRect a, NSRect b)
{ return !rect_empty(rect_intersect(a, b)); }

static void view_set_needs_display(id self, SEL sel, signed char yes)
{
    nsview *v = as_view(self);
    (void)sel;
    if (!v) return;
    v->needs_display = yes ? 1 : 0;
    /* No rectangle given means the whole view. */
    if (yes) v->dirty = v->bounds;
    else     memset(&v->dirty, 0, sizeof v->dirty);
    if (yes && !g_editor_view) g_editor_view = self;
}
static void view_set_needs_display_rect(id self, SEL sel, NSRect r)
{
    nsview *v = as_view(self);
    (void)sel;
    if (!v) return;
    v->needs_display = 1;
    v->dirty = rect_union(v->dirty, r);
    if (!g_editor_view) g_editor_view = self;
}
/* Which rectangles drawRect: is being asked to draw.
 *
 * This one has to be implemented rather than left to the nil return, because it
 * answers through *out-parameters*: an unimplemented selector zeroes the return
 * registers and leaves the caller's own locals alone, so the plugin read a
 * count and an array pointer that were whatever the stack happened to hold.
 * With one set of stack contents the count came out zero and the loop was
 * skipped; with another -- loading libc++ was enough to change it -- the count
 * was large and the array pointer was not one, and VSTGUI walked it. The
 * editor's own drawing was fine either way; the crash was the host declining to
 * answer a question the caller could not tell had gone unanswered.
 *
 * One rect: whatever drawRect: was handed, which is the union of what the
 * plugin invalidated. Answering "the whole view" here is what made a dial cost
 * a full repaint -- iPlug2 draws exactly the rectangles this reports. The
 * storage is static because the caller keeps the pointer for the length of the
 * call, and there is one editor. */
static void view_rects_being_drawn(id self, SEL sel, NSRect **rects, long *count)
{
    static NSRect one;
    nsview *v = as_view(self);
    (void)sel;
    if (!rect_empty(g_draw_rect))  one = g_draw_rect;
    else if (v)                    one = v->bounds;
    else                           memset(&one, 0, sizeof one);
    if (rects) *rects = &one;
    if (count) *count = rect_empty(one) ? 0 : 1;
}
/* Whether a control has to repaint. Answering yes to everything is safe but
 * costs the caller a full redraw; the rect being drawn is the honest answer. */
static signed char view_needs_to_draw(id self, SEL sel, NSRect r)
{
    (void)self; (void)sel;
    if (rect_empty(g_draw_rect)) return 1;
    return rect_meets(r, g_draw_rect) ? 1 : 0;
}

static void view_set_hidden(id self, SEL sel, signed char yes)
{ nsview *v = as_view(self); (void)sel; if (v) v->hidden = yes ? 1 : 0; }
static signed char view_yes(id self, SEL sel) { (void)self; (void)sel; return 1; }
/* NSView's own default for -isFlipped is NO: a view counts from the bottom
 * unless it says otherwise. Answering YES here instead cost every editor whose
 * class does not override it -- iPlug2 overrides and returns YES, so those were
 * right by luck, while Automaton and Tattoo hit-tested a click at `height - y`
 * and answered for whatever control was mirrored opposite the one under the
 * pointer. Their editors drew correctly and could not be operated. */
static signed char view_no(id self, SEL sel) { (void)self; (void)sel; return 0; }
static void view_void(id self, SEL sel) { (void)self; (void)sel; }
static void *view_layer(id self, SEL sel)
{ nsview *v = as_view(self); (void)sel; return v ? v->layer : NULL; }
/* The editor's view is the one a layer was installed on: input has to be
 * delivered to that object, and nothing else identifies it. */
static void view_set_layer(id self, SEL sel, void *l)
{
    nsview *v = as_view(self);
    (void)sel;
    if (v) v->layer = l;
    if (l) g_editor_view = self;
}

/* Window coordinates to view coordinates.
 *
 * There is one view and no window to offset against, so the only work left is
 * the flip -- and the flip is the whole point. An NSEvent's locationInWindow
 * has its origin at the bottom left, because that is what Cocoa windows use;
 * a *flipped* view, which every plugin editor here is, counts from the top, and
 * AppKit does that conversion here. Returning the point unchanged meant every
 * click landed at `height - y`: the editor was mirrored vertically, so a slider
 * at the bottom answered for one at the top and, more often, nothing answered
 * at all. The editors drew perfectly and could not be operated.
 *
 * Whether to flip is the view's own answer, not an assumption: `isFlipped` is
 * a method the plugin overrides, and a VSTGUI view says no where an iPlug2 one
 * says yes. `from` is nil for a window-relative point, which is the only case a
 * plugin here uses. */
static int view_flipped(id v)
{
    signed char (*imp)(id, SEL) =
        (signed char (*)(id, SEL))macobjc_lookup(v, "isFlipped");
    return imp ? imp(v, "isFlipped") != 0 : 0;
}
static NSPoint view_convert_point(id self, SEL sel, NSPoint p, id from)
{
    nsview *v = as_view(self);
    int fl;
    (void)sel; (void)from;
    fl = view_flipped(self);
    if (v && v->bounds.size.h > 0.0 && fl)
        p.y = v->bounds.size.h - p.y;
    return p;
}
/* The same flip, about the rectangle's far edge rather than its origin -- a
 * flipped rect keeps its height and moves its top to where its bottom was. */
static NSRect view_convert_rect(id self, SEL sel, NSRect r, id from)
{
    nsview *v = as_view(self);
    (void)sel; (void)from;
    if (v && v->bounds.size.h > 0.0 && view_flipped(self))
        r.origin.y = v->bounds.size.h - r.origin.y - r.size.h;
    return r;
}
/* The other direction: view coordinates out to the window's. The flip is its own
 * inverse, so these are the same arithmetic -- they exist because a plugin that
 * warps the cursor converts outward, and a missing selector there is a zero
 * rather than an error. */
static NSPoint view_convert_point_to(id self, SEL sel, NSPoint p, id to)
{ return view_convert_point(self, sel, p, to); }
static NSRect view_convert_rect_to(id self, SEL sel, NSRect r, id to)
{ return view_convert_rect(self, sel, r, to); }


/* Fast enumeration -- what `for (id x in array)` compiles to. Returning zero
 * from this does not fail loudly; the loop simply never runs a single iteration,
 * which is why it is worth implementing rather than stubbing. */
typedef struct {
    unsigned long  state;
    id            *itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long  extra[5];
} ns_fast_enum;

static unsigned long arr_fast_enum(id self, SEL sel, ns_fast_enum *st,
                                   id *buf, unsigned long len)
{
    static unsigned long never_mutates;
    nsarray *a = as_arr(self);
    unsigned long i, n = 0;

    (void)sel;
    if (!a || !st || !buf || !len) return 0;
    /* The runtime compares *mutationsPtr across calls to catch a container
     * mutated mid-loop; a stable address means "never mutated". */
    st->mutationsPtr = &never_mutates;
    st->itemsPtr = buf;
    for (i = st->state; i < (unsigned long)a->n && n < len; i++) buf[n++] = a->v[i];
    st->state = i;
    return n;
}

/* --------------------------------------------- screen, paths, file manager */

/* A screen has to report a non-zero size and scale: the plugin divides by the
 * backing scale factor to convert points to pixels, and a nil screen answering
 * zero is a division by zero rather than a missing feature. */
typedef struct { void *isa; long refs; } nssimple;

static id screen_main(id self, SEL sel)
{
    static id cached;
    (void)self; (void)sel;
    if (!cached) cached = ns_new_of("NSScreen");
    return cached;
}
static NSRect screen_frame(id self, SEL sel)
{
    NSRect r;
    (void)self; (void)sel;
    r.origin.x = 0.0; r.origin.y = 0.0;
    r.size.w = (double)SCREEN_W; r.size.h = (double)SCREEN_H;
    return r;
}
static double screen_scale(id self, SEL sel) { (void)self; (void)sel; return 1.0; }

/* ---- NSGraphicsContext -------------------------------------------------
 *
 * What a view draws into. AppKit focuses a context on the view before calling
 * drawRect: and the view fetches it back with `[[NSGraphicsContext
 * currentContext] graphicsPort]`; nothing here did that, so every VSTGUI editor
 * asked for its context, got nil, and dereferenced it a few instructions later.
 *
 * The context is a bitmap the host owns, which is also the editor's
 * framebuffer -- see macquartz_editor_context. There is one, because there is
 * one editor. */
typedef struct { void *isa; long refs; void *cg; } nsgctx;

static id g_gcontext;

/* Point the current context at a CGContextRef, before drawRect: is called. */
void macns_set_graphics_port(void *cg)
{
    nsgctx *g;
    if (!g_gcontext) g_gcontext = ns_new_of("NSGraphicsContext");
    if ((g = (nsgctx *)g_gcontext)) g->cg = cg;
}

static id   gctx_current(id self, SEL sel)
{ (void)self; (void)sel; return g_gcontext; }
static void *gctx_port(id self, SEL sel)
{ nsgctx *g = (nsgctx *)self; (void)sel; return g ? g->cg : NULL; }
static id   gctx_with_port(id self, SEL sel, void *cg, signed char flipped)
{ (void)self; (void)sel; (void)flipped; macns_set_graphics_port(cg); return g_gcontext; }
static void gctx_set_current(id self, SEL sel, id ctx)
{ (void)self; (void)sel; if (ctx) g_gcontext = ctx; }
static void gctx_void(id self, SEL sel) { (void)self; (void)sel; }
static signed char gctx_flipped(id self, SEL sel) { (void)self; (void)sel; return 1; }

/* NSAnimationContext groups implicit animations. With nothing animating, the
 * grouping calls only need to nest without complaint. */
static id anim_current(id self, SEL sel)
{
    static id cached;
    (void)self; (void)sel;
    if (!cached) cached = ns_new_of("NSAnimationContext");
    return cached;
}
static void anim_void(id self, SEL sel) { (void)self; (void)sel; }
static void anim_set_duration(id self, SEL sel, double d)
{ (void)self; (void)sel; (void)d; }

/* Path arithmetic on NSString. These are pure string operations, so they are
 * exactly right rather than approximations. */
static id path_last_component(id self, SEL sel)
{
    const char *p = str_bytes(self, NULL), *slash;
    (void)sel;
    if (!p) return NULL;
    slash = strrchr(p, '/');
    return str_make(slash ? slash + 1 : p, -1);
}
static id path_delete_last_component(id self, SEL sel)
{
    const char *p = str_bytes(self, NULL), *slash;
    (void)sel;
    if (!p) return NULL;
    slash = strrchr(p, '/');
    if (!slash) return str_make("", 0);
    return str_make(p, (long)(slash - p));
}
static id path_delete_extension(id self, SEL sel)
{
    long n = 0;
    const char *p = str_bytes(self, &n), *dot, *slash;
    (void)sel;
    if (!p) return NULL;
    dot = strrchr(p, '.');
    slash = strrchr(p, '/');
    /* A dot in a directory name is not an extension. */
    if (!dot || (slash && dot < slash)) return str_make(p, n);
    return str_make(p, (long)(dot - p));
}
static id path_extension(id self, SEL sel)
{
    const char *p = str_bytes(self, NULL), *dot, *slash;
    (void)sel;
    if (!p) return str_make("", 0);
    dot = strrchr(p, '.');
    slash = strrchr(p, '/');
    if (!dot || (slash && dot < slash)) return str_make("", 0);
    return str_make(dot + 1, -1);
}
static id path_append_component(id self, SEL sel, id other)
{
    long al = 0;
    const char *a = str_bytes(self, &al), *b = str_bytes(other, NULL);
    char buf[4096];
    (void)sel;
    if (!a) return NULL;
    snprintf(buf, sizeof buf, "%s%s%s", a,
             (al && a[al - 1] == '/') ? "" : "/", b ? b : "");
    return str_make(buf, -1);
}
static id path_append_string(id self, SEL sel, id other)
{
    const char *a = str_bytes(self, NULL), *b = str_bytes(other, NULL);
    char buf[4096];
    (void)sel;
    if (!a) return NULL;
    snprintf(buf, sizeof buf, "%s%s", a, b ? b : "");
    return str_make(buf, -1);
}

static id fm_default(id self, SEL sel)
{
    static id cached;
    (void)self; (void)sel;
    if (!cached) cached = ns_new_of("NSFileManager");
    return cached;
}
static signed char fm_exists(id self, SEL sel, id path)
{
    const char *p = str_bytes(path, NULL);
    struct stat st;
    int ok;
    (void)self; (void)sel;
    ok = (p && !stat(p, &st)) ? 1 : 0;
    /* Logged because this is where a plugin silently gives up on its artwork:
     * iPlug2 tests for the file and, finding nothing, returns an empty bitmap
     * whose frame count is zero -- and the divide by that is the first symptom. */
    if (getenv("MACOBJC_VERBOSE"))
        fprintf(stderr, "  [fm] fileExistsAtPath(\"%s\") -> %s\n",
                p ? p : "(not a string)", ok ? "yes" : "NO");
    return (signed char)ok;
}

/* CAMetalLayer. There is no Metal here, so -device answering nil is the honest
 * result -- and the plugin is expected to test it, because a Mac without a
 * supported GPU returns nil too. */
static void  layer_void(id self, SEL sel) { (void)self; (void)sel; }
static void  layer_set_ptr(id self, SEL sel, void *v)
{ (void)self; (void)sel; (void)v; }


/* ------------------------------------------------- timers, run loop, misc */

/* A plugin drives its editor's animation from a timer on the run loop. Nothing
 * runs that loop here, so a timer is accepted and never fires -- the editor still
 * gets pumped through effEditIdle, which is how the Windows side drives it too. */
/* Timers have to be real, because on macOS a plugin's editor animates from one.
 * The Windows side gets its frames from effEditIdle; here iPlug2 installs a
 * timer on the run loop and draws from that, so a timer that is accepted and
 * never fires is an editor that opens and stays blank. Nothing runs a real run
 * loop, so the host fires them by hand -- see macns_fire_timers. */
typedef struct { void *isa; long refs; id target; SEL action; id info;
                 double interval; int repeats, dead; } nstimer;

#define MAX_TIMERS 32
static nstimer *g_timers[MAX_TIMERS];
static int      g_ntimers;

static id timer_make(id self, SEL sel, double interval, id target, SEL action,
                     id info, signed char repeats)
{
    nstimer *t = (nstimer *)ns_new_of("NSTimer");
    (void)self; (void)sel;
    if (!t) return NULL;
    t->target = target;
    t->action = action;
    t->info = info;
    t->interval = interval;
    t->repeats = repeats ? 1 : 0;
    /* Scheduled variants are on the run loop already. */
    if (g_ntimers < MAX_TIMERS) g_timers[g_ntimers++] = t;
    return t;
}

/* Fire every live timer once. The interval is ignored: the host decides the
 * frame rate by how often it pumps, which is the same contract the Windows
 * editors run under. */
/* Drop everything tied to the plugin that is going away. A timer left behind
 * fires into a torn-down editor on the next pump, and the editor view pointer
 * would otherwise still name an object whose image has been unmapped. */
/* ---- the pointer ------------------------------------------------------
 *
 * There is one editor view and no window to offset against, so this host's
 * screen, window and view coordinates are the same thing: the position the host
 * last posted, with the origin at the top left. Everything that reports where
 * the pointer is has to agree on that one number, because a plugin does not
 * merely read it -- it does arithmetic across the two conventions.
 *
 * iPlug2 is the instructive case. Grabbing a control makes it hide the cursor
 * and store the position through `[NSEvent mouseLocation]`, which is in *screen*
 * coordinates counted from the bottom, and it converts with
 * `CGDisplayPixelsHigh() - mouse.y`. With mouseLocation unimplemented and so
 * answering (0,0), the stored position was a thousand pixels away from the
 * pointer, and the first mouseDragged: after the press moved the control by the
 * whole distance: a one-pixel drag took a slider from 0.43 to full scale, in
 * whichever direction it was dragged. Every macOS editor here was operable only
 * by clicking, never by dragging.
 *
 * SCREEN_H is what CGDisplayPixelsHigh and NSScreen's frame both report -- see
 * macns_screen_height, which macquartz.c asks rather than repeating.
 *
 * There are two positions, not one, and the difference is the whole reason a
 * drag behaves. `real` is where the host says the pointer is -- a Qt or GTK
 * coordinate, ultimately the X pointer. `virt` is where the *plugin* thinks it
 * is, and a plugin moves it: grabbing a knob makes iPlug2 hide the cursor and
 * warp it back to the grab point after every event, so that what it reads next
 * is the physical movement since then and nothing else. On a real Mac the
 * window server does that warp and the next event arrives from the new place.
 * Nothing warps an X pointer here, so the host does it in software: each event
 * advances `virt` by however far `real` moved, and a warp sets `virt` outright.
 *
 * Without it the plugin measured every event against the point it had warped
 * to, so each one moved the control by the whole distance from the press rather
 * than by one step. A drag accelerated away and hit the end of its range in
 * five pixels -- smooth-looking, and unusable. */
static double g_real_x, g_real_y;      /* where the host says the pointer is */
static double g_virt_x, g_virt_y;      /* where the plugin thinks it is */
static int    g_have_last;

int macns_screen_height(void) { return SCREEN_H; }

/* Where the pointer is, as the plugin sees it, in top-left coordinates. */
void macns_pointer(double *x, double *y)
{ if (x) *x = g_virt_x; if (y) *y = g_virt_y; }

/* A warp. CGDisplayMoveCursorToPoint is how a plugin pins the cursor while
 * dragging, and the position it reads back afterwards has to be the one it
 * asked for -- while the real pointer, which nothing here can move, carries on
 * where it was. */
void macns_set_pointer(double x, double y)
{ g_virt_x = x; g_virt_y = y; }

void macns_reset_gui(void)
{
    int i;
    for (i = 0; i < g_ntimers; i++) g_timers[i] = NULL;
    g_ntimers = 0;
    g_editor_view = NULL;
    /* See macns_post_mouse: a pointer delta measured against the previous
     * plugin's editor is worse than no delta at all. */
    g_have_last = 0;
    g_real_x = g_real_y = g_virt_x = g_virt_y = 0.0;
}

/* Nothing here runs a real Cocoa run loop, so nothing turns "this view is dirty"
 * into a draw. A view marks itself with setNeedsDisplay[InRect]: and expects
 * drawRect: to follow -- Ragnarok's editor asks twenty times and, until this
 * existed, was never once asked to paint. */
void macns_draw_dirty(void)
{
    id self = live_editor_view();
    nsview *v = as_view(self);
    void (*imp)(id, SEL, NSRect);
    NSRect full, r;

    if (!v || !v->needs_display) return;
    v->needs_display = 0;
    full = v->bounds;
    if (rect_empty(full)) full = v->frame;
    if (rect_empty(full)) return;
    /* What the plugin asked for, clipped to the view. A plugin that only ever
     * says setNeedsDisplay: still gets the whole thing; one that names the knob
     * it changed gets the knob. Falling back to the whole view when the union
     * came out empty keeps a plugin that marks itself dirty by some route this
     * does not model painting at all rather than not at all. */
    r = rect_intersect(v->dirty, full);
    if (rect_empty(r)) r = full;
    memset(&v->dirty, 0, sizeof v->dirty);
    /* Focus a context on the view first. AppKit hands drawRect: a context that
     * is already set up; a plugin that fetches it and finds nil dereferences
     * what it gets a few instructions later. The context is the whole editor --
     * it is the framebuffer, and sizing it to the dirty rect would throw the
     * rest of the picture away every time a dial moved. */
    {   void *ctx = macquartz_editor_context((int)full.size.w, (int)full.size.h);
        macquartz_begin_draw(ctx, view_flipped(self), full.size.h);
        macns_set_graphics_port(ctx); }
    imp = (void (*)(id, SEL, NSRect))macobjc_lookup(self, "drawRect:");
    if (!imp) return;
    g_draw_rect = r;
    imp(self, "drawRect:", r);
    memset(&g_draw_rect, 0, sizeof g_draw_rect);
}

void macns_fire_timers(void)
{
    int i;
    /* Run-loop timers as well as NSTimers: VSTGUI 4 schedules its redraw on
     * the former, and a plugin does not care which kind the host understands. */
    macshim_fire_cf_timers();
    for (i = 0; i < g_ntimers; i++) {
        nstimer *t = g_timers[i];
        void (*imp)(id, SEL, id);
        if (!t || t->dead || !t->target || !t->action) continue;
        imp = (void (*)(id, SEL, id))macobjc_lookup(t->target, t->action);
        if (imp) imp(t->target, t->action, t);
        if (!t->repeats) t->dead = 1;
    }
}
static id runloop_current(id self, SEL sel)
{
    static id cached;
    (void)self; (void)sel;
    if (!cached) cached = ns_new_of("NSRunLoop");
    return cached;
}
static void runloop_add_timer(id self, SEL sel, id timer, id mode)
{ (void)self; (void)sel; (void)timer; (void)mode; }
static void timer_invalidate(id self, SEL sel)
{
    nstimer *t = self;
    (void)sel;
    if (t) t->dead = 1;
}
static id timer_userinfo(id self, SEL sel)
{ nstimer *t = self; (void)sel; return t ? t->info : NULL; }

static signed char str_contains(id self, SEL sel, id needle)
{
    const char *a = str_bytes(self, NULL), *b = str_bytes(needle, NULL);
    (void)sel;
    if (!a || !b) return 0;
    return strstr(a, b) ? 1 : 0;
}
static id path_append_extension(id self, SEL sel, id ext)
{
    const char *a = str_bytes(self, NULL), *e = str_bytes(ext, NULL);
    char buf[4096];
    (void)sel;
    if (!a) return NULL;
    snprintf(buf, sizeof buf, "%s.%s", a, e ? e : "");
    return str_make(buf, -1);
}

/* +arrayWithObjects: is nil-terminated and variadic. */
static id arr_with_objects(id self, SEL sel, id first, ...)
{
    id a = arr_make();
    va_list ap;
    id o;
    (void)self; (void)sel;
    if (!a || !first) return a;
    arr_add(a, "addObject:", first);
    va_start(ap, first);
    while ((o = va_arg(ap, id))) arr_add(a, "addObject:", o);
    va_end(ap);
    return a;
}


/* ----------------------------------------------------------------- NSBundle */

/* A plugin finds its own artwork, wavetables and preset bank through its bundle.
 * With this unimplemented every lookup returned nil, and the plugins that need
 * resource data rendered silence -- which looks like a DSP problem rather than a
 * path problem. Worth noting: every plugin in the corpus asks for its bundle, so
 * the missing selector was not on its own a useful signal; only the ones that
 * actually load something from it are affected.
 *
 * Paths are real, taken from the bundle the loader opened. */
typedef struct { void *isa; long refs; char path[1024]; } nsbundle;

static nsbundle *as_bundle(id p)
{ return (p && macobjc_isa_named(p, "NSBundle")) ? (nsbundle *)p : NULL; }

/* Cached, because a plugin compares bundle pointers and expects mainBundle to be
 * the same object every time -- but only for as long as it is the same bundle.
 * Caching it outright leaks the first plugin's bundle into the second: load two
 * macOS plugins in a row and the second one looks for its artwork in the first
 * one's Resources, gets an empty bitmap back, and divides by its zero frame
 * count. That is a SIGFPE on the second load, every time, in either order. */
static id bundle_main(id self, SEL sel)
{
    static id cached;
    nsbundle *b = as_bundle(cached);
    const char *want = macshim_bundle_path();
    (void)self; (void)sel;

    if (b && !strcmp(b->path, want ? want : "")) return cached;
    if (!(b = ns_new_of("NSBundle"))) return NULL;
    snprintf(b->path, sizeof b->path, "%s", want ? want : "");
    cached = b;
    return b;
}
/* Any identifier resolves to this bundle: only one image is ever loaded. */
static id bundle_with_id(id self, SEL sel, id ident)
{ (void)ident; return bundle_main(self, sel); }

static id bundle_path(id self, SEL sel)
{ nsbundle *b = as_bundle(self); (void)sel; return str_make(b ? b->path : "", -1); }

static id bundle_resource_path(id self, SEL sel)
{
    nsbundle *b = as_bundle(self);
    char p[1200];
    (void)sel;
    if (!b) return NULL;
    snprintf(p, sizeof p, "%s/Contents/Resources", b->path);
    return str_make(p, -1);
}

/* The type may be nil or already part of the name; both spellings appear.
 * Returns nil when the file is absent, which is what a caller tests. */
static id bundle_path_for_resource(id self, SEL sel, id name, id type)
{
    nsbundle *b = as_bundle(self);
    long tl = 0;
    const char *n = str_bytes(name, NULL), *t = str_bytes(type, &tl);
    char p[1400];
    struct stat st;
    (void)sel;
    if (!b || !n) return NULL;
    if (t && tl)
        snprintf(p, sizeof p, "%s/Contents/Resources/%s.%s", b->path, n, t);
    else
        snprintf(p, sizeof p, "%s/Contents/Resources/%s", b->path, n);
    if (stat(p, &st)) {
        /* Logged, because a nil here is a plugin quietly losing its artwork --
         * exactly the failure that is invisible from the outside. */
        if (getenv("MACOBJC_VERBOSE"))
            fprintf(stderr, "  [bundle] pathForResource(%s%s%s) -> not found\n",
                    n, (t && tl) ? "." : "", t ? t : "");
        return NULL;
    }
    if (getenv("MACOBJC_VERBOSE"))
        fprintf(stderr, "  [bundle] pathForResource -> %s\n", p);
    return str_make(p, -1);
}
static id bundle_url_for_resource(id self, SEL sel, id name, id ext)
{ return bundle_path_for_resource(self, sel, name, ext); }
static id bundle_info_value(id self, SEL sel, id key)
{ (void)self; (void)sel; (void)key; return NULL; }
static signed char bundle_load(id self, SEL sel) { (void)self; (void)sel; return 1; }


/* -------------------------------------------------------------------- setup */

/* ----------------------------------------------------------------- NSEvent */

/* Cocoa delivers input as messages carrying an NSEvent, so that is what has to
 * be synthesised. Two conventions matter and both are easy to get backwards:
 *
 *  - locationInWindow is in window coordinates with y measured up from the
 *    bottom, and iPlug2 flips it against the view's height to get its own
 *    top-down y. Hand it a top-down y and every click lands mirrored.
 *  - scrollWheel deltas are in "lines", positive away from the user, whereas the
 *    host reports Windows-style wheel notches of 120.
 */
enum {
    EV_LMOUSEDOWN = 1, EV_LMOUSEUP = 2, EV_RMOUSEDOWN = 3, EV_RMOUSEUP = 4,
    EV_MOUSEMOVED = 5, EV_LMOUSEDRAGGED = 6, EV_RMOUSEDRAGGED = 7,
    EV_KEYDOWN = 10, EV_KEYUP = 11, EV_SCROLLWHEEL = 22,
    EV_OMOUSEDOWN = 25, EV_OMOUSEUP = 26, EV_OMOUSEDRAGGED = 27
};

typedef struct {
    void *isa; long refs;
    long    type;
    NSPoint loc;
    long    button, clicks, mods, keycode;
    double  dx, dy;
    id      chars;
} nsevent;

static nsevent *as_event(id p)
{ return (p && macobjc_isa_named(p, "NSEvent")) ? (nsevent *)p : NULL; }

static long    ev_type(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? e->type : 0; }
static NSPoint ev_loc(id self, SEL sel)
{ nsevent *e = as_event(self); NSPoint z = { 0, 0 }; (void)sel; return e ? e->loc : z; }
static long    ev_button(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? e->button : 0; }
static long    ev_clicks(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? e->clicks : 1; }
static unsigned long ev_mods(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? (unsigned long)e->mods : 0; }
static unsigned short ev_keycode(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? (unsigned short)e->keycode : 0; }
static double  ev_dx(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? e->dx : 0.0; }
static double  ev_dy(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? e->dy : 0.0; }
static id      ev_chars(id self, SEL sel)
{ nsevent *e = as_event(self); (void)sel; return e ? e->chars : NULL; }
static signed char ev_no(id self, SEL sel) { (void)self; (void)sel; return 0; }
static double  ev_pressure(id self, SEL sel) { (void)self; (void)sel; return 1.0; }
/* +mouseLocation: where the pointer is, in screen coordinates with the origin at
 * the bottom left. The host keeps it in its own top-left coordinates, so this is
 * the one conversion. */
static NSPoint ev_mouse_location(id self, SEL sel)
{
    NSPoint p;
    (void)self; (void)sel;
    p.x = g_virt_x;
    p.y = (double)SCREEN_H - g_virt_y;
    return p;
}
static id      ev_nil(id self, SEL sel) { (void)self; (void)sel; return NULL; }

static nsevent *event_make(long type, double x, double y)
{
    nsevent *e = (nsevent *)ns_new_of("NSEvent");
    if (!e) return NULL;
    e->type = type;
    e->loc.x = x;
    e->loc.y = y;
    e->clicks = 1;
    return e;
}

/* Send one event to the editor's view, if it implements the handler. */
static void send_event(const char *sel, nsevent *e)
{
    void (*imp)(id, SEL, id);
    id v = live_editor_view();
    if (!v || !e) return;
    imp = (void (*)(id, SEL, id))macobjc_lookup(v, sel);
    if (imp) imp(v, sel, e);
}

void macns_post_mouse(int x, int y, int msg, int buttons, int wheel)
{
    /* Windows message numbers, because that is what the host already speaks. */
    enum { WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
           WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
           WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020A };
    nsview *v = as_view(live_editor_view());
    double h = v ? v->bounds.size.h : 0.0;
    nsevent *e;
    double dx, dy;
    long type;
    const char *sel;

    if (!v) return;

    /* Advance the plugin's pointer by however far the real one moved, and start
     * a fresh gesture from wherever the button actually went down -- a press is
     * the one moment the two are certainly the same place. See the pointer
     * section above for why there are two of them. */
    dx = g_have_last ? (double)x - g_real_x : 0.0;
    dy = g_have_last ? (double)y - g_real_y : 0.0;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN ||
        msg == WM_MBUTTONDOWN || !g_have_last) {
        g_virt_x = (double)x; g_virt_y = (double)y;
    } else {
        g_virt_x += dx;
        g_virt_y += dy;
    }
    g_real_x = (double)x; g_real_y = (double)y; g_have_last = 1;

    /* The event carries the virtual position, in Cocoa's convention: y grows
     * upward from the bottom of the view. It also carries how far the pointer
     * moved, because a control that tracks a gesture by accumulating deltas --
     * most knobs, and any slider with gearing -- moves not at all when the
     * delta is always zero. That one is in the host's own top-left direction,
     * which is what AppKit reports and what iPlug2's `mValue -= dY` is written
     * against. */
    e = event_make(0, g_virt_x, h > 0.0 ? h - g_virt_y : g_virt_y);
    if (!e) return;
    e->dx = dx;
    e->dy = dy;

    switch (msg) {
    case WM_LBUTTONDOWN:   type = EV_LMOUSEDOWN; sel = "mouseDown:";      break;
    case WM_LBUTTONDBLCLK: type = EV_LMOUSEDOWN; sel = "mouseDown:";
                           e->clicks = 2;                                 break;
    case WM_LBUTTONUP:     type = EV_LMOUSEUP;   sel = "mouseUp:";        break;
    case WM_RBUTTONDOWN:   type = EV_RMOUSEDOWN; sel = "rightMouseDown:";
                           e->button = 1;                                 break;
    case WM_RBUTTONUP:     type = EV_RMOUSEUP;   sel = "rightMouseUp:";
                           e->button = 1;                                 break;
    case WM_MBUTTONDOWN:   type = EV_OMOUSEDOWN; sel = "otherMouseDown:";
                           e->button = 2;                                 break;
    case WM_MBUTTONUP:     type = EV_OMOUSEUP;   sel = "otherMouseUp:";
                           e->button = 2;                                 break;
    case WM_MOUSEWHEEL:
        type = EV_SCROLLWHEEL; sel = "scrollWheel:";
        /* A notch is 120 in the host's units and one line in Cocoa's. */
        e->dy = (double)wheel / 120.0;
        break;
    case WM_MOUSEMOVE:
    default:
        /* A drag is a move with a button held, and it is a different selector. */
        if (buttons & 1)      { type = EV_LMOUSEDRAGGED; sel = "mouseDragged:"; }
        else if (buttons & 2) { type = EV_RMOUSEDRAGGED; sel = "rightMouseDragged:";
                                e->button = 1; }
        else if (buttons & 4) { type = EV_OMOUSEDRAGGED; sel = "otherMouseDragged:";
                                e->button = 2; }
        else                  { type = EV_MOUSEMOVED;    sel = "mouseMoved:"; }
        break;
    }
    e->type = type;
    send_event(sel, e);
}

void macns_post_key(int vk, int down, int ch)
{
    nsevent *e = event_make(down ? EV_KEYDOWN : EV_KEYUP, 0.0, 0.0);
    char c[2];
    if (!e) return;
    e->keycode = vk;
    c[0] = (char)(ch ? ch : 0);
    c[1] = 0;
    e->chars = str_make(c, ch ? 1 : 0);
    send_event(down ? "keyDown:" : "keyUp:", e);
}

typedef struct { const char *cls, *sel; void *imp; } entry;

static const entry g_table[] = {
    /* NSString */
    { "NSString", "+stringWithCString:encoding:", str_with_cstring },
    { "NSString", "+stringWithUTF8String:",       str_with_utf8 },
    { "NSString", "+stringWithCString:",          str_with_utf8 },
    { "NSString", "+string",                      str_empty },
    { "NSString", "+stringWithFormat:",           str_with_format },
    { "NSString", "initWithCString:encoding:",    str_init_cstring },
    { "NSString", "initWithUTF8String:",          str_init_utf8 },
    { "NSString", "init",                         str_init },
    { "NSString", "UTF8String",                   str_utf8 },
    { "NSString", "cStringUsingEncoding:",        str_cstring_enc },
    { "NSString", "cString",                      str_utf8 },
    { "NSString", "length",                       str_length },
    { "NSString", "isEqualToString:",             str_is_equal },
    { "NSString", "isEqual:",                     str_is_equal },
    { "NSString", "copy",                         str_copy },
    { "NSString", "copyWithZone:",                str_copy_zone },
    { "NSString", "description",                  str_description },
    { "NSString", "hash",                         str_hash },
    { "NSString", "characterAtIndex:",            str_char_at },

    { "NSMutableString", "+stringWithCapacity:",  mstr_with_capacity },
    { "NSMutableString", "initWithString:",       mstr_init_with_string },
    { "NSMutableString", "+stringWithString:",    str_with_string },
    { "NSString",        "+stringWithString:",    str_with_string },
    { "NSMutableString", "+string",               str_empty },
    { "NSMutableString", "appendString:",         mstr_append },
    { "NSMutableString", "appendFormat:",         mstr_append_format },
    { "NSMutableString", "setString:",            mstr_set_string },
    { "NSMutableString", "UTF8String",            str_utf8 },
    { "NSMutableString", "length",                str_length },

    /* NSArray */
    { "NSArray", "+array",              arr_new },
    { "NSArray", "count",               arr_count },
    { "NSArray", "objectAtIndex:",      arr_object_at },
    { "NSMutableArray", "+array",       arr_new },
    { "NSMutableArray", "+arrayWithCapacity:", arr_with_capacity },
    { "NSMutableArray", "init",         str_description },   /* returns self */
    { "NSMutableArray", "count",        arr_count },
    { "NSMutableArray", "objectAtIndex:", arr_object_at },
    { "NSMutableArray", "addObject:",   arr_add },
    { "NSMutableArray", "removeAllObjects", arr_remove_all },

    /* NSNumber */
    { "NSNumber", "+numberWithInt:",     num_with_int },
    { "NSNumber", "+numberWithFloat:",   num_with_float },
    { "NSNumber", "+numberWithDouble:",  num_with_double },
    { "NSNumber", "+numberWithBool:",    num_with_bool },
    { "NSNumber", "intValue",            num_int },
    { "NSNumber", "floatValue",          num_float },
    { "NSNumber", "doubleValue",         num_double },
    { "NSNumber", "boolValue",           num_bool },

    /* NSDictionary */
    /* NSView */
    { "NSView", "initWithFrame:",        view_init_frame },
    { "NSView", "init",                  view_init },
    { "NSView", "frame",                 view_frame },
    { "NSView", "bounds",                view_bounds },
    { "NSView", "setFrame:",             view_set_frame },
    { "NSView", "setBounds:",            view_set_bounds },
    { "NSView", "setFrameSize:",         view_set_frame_size },
    { "NSView", "addSubview:",           view_add_subview },
    { "NSView", "removeFromSuperview",   view_remove_from_super },
    { "NSView", "superview",             view_superview },
    { "NSView", "subviews",              view_subviews },
    { "NSView", "window",                view_window },
    { "NSView", "setNeedsDisplay:",      view_set_needs_display },
    { "NSView", "setNeedsDisplayInRect:", view_set_needs_display_rect },
    { "NSView", "setHidden:",            view_set_hidden },
    { "NSView", "isHidden",              view_void },
    { "NSView", "acceptsFirstResponder", view_yes },
    { "NSView", "isFlipped",             view_no },
    { "NSView", "setWantsLayer:",        view_set_needs_display },
    { "NSView", "layer",                 view_layer },
    { "NSView", "setLayer:",             view_set_layer },
    { "NSView", "display",               view_void },
    { "NSView", "convertPoint:fromView:", view_convert_point },
    { "NSView", "convertRect:fromView:",  view_convert_rect },
    { "NSView", "convertPoint:toView:",   view_convert_point_to },
    { "NSView", "getRectsBeingDrawn:count:", view_rects_being_drawn },
    { "NSView", "needsToDrawRect:",       view_needs_to_draw },
    { "NSView", "convertRect:toView:",    view_convert_rect_to },

    /* screen, animation, paths, file manager, layer */
    { "NSWindow", "frame",                win_frame },
    { "NSWindow", "contentView",          win_content_view },
    { "NSWindow", "convertRectFromScreen:",  win_rect_from_screen },
    { "NSWindow", "convertRectToScreen:",    win_rect_to_screen },
    { "NSWindow", "convertPointFromScreen:", win_point_from_screen },
    { "NSWindow", "convertPointToScreen:",   win_point_to_screen },
    { "NSWindow", "convertScreenToBase:",    win_point_from_screen },
    { "NSWindow", "convertBaseToScreen:",    win_point_to_screen },
    { "NSWindow", "screen",               screen_main },
    { "NSWindow", "windowNumber",         win_number },
    { "NSWindow", "mouseLocationOutsideOfEventStream", win_mouse_location },
    { "NSWindow", "makeFirstResponder:",  win_make_first_responder },
    { "NSWindow", "backingScaleFactor",   screen_scale },
    /* An NSCell is initialised by a text or image, not by a frame -- and a
     * plugin's cell subclass calls up to it. */
    { "NSCell", "initTextCell:",          view_init_self },
    { "NSCell", "initImageCell:",         view_init_self },
    { "NSTextFieldCell", "initTextCell:", view_init_self },
    /* Configuration a plugin's text entry sets and nothing here draws. */
    { "NSControl", "setCell:",            ctl_set_cell },
    { "NSControl", "cell",                ctl_cell },
    { "NSControl", "setEditable:",        ctl_set_flag },
    { "NSControl", "setSelectable:",      ctl_set_flag },
    { "NSControl", "setEnabled:",         ctl_set_flag },
    { "NSControl", "setBordered:",        ctl_set_flag },
    { "NSControl", "setBezeled:",         ctl_set_flag },
    { "NSControl", "setDrawsBackground:", ctl_set_flag },
    { "NSControl", "setUsesSingleLineMode:", ctl_set_flag },
    { "NSControl", "setRefusesFirstResponder:", ctl_set_flag },
    { "NSControl", "setStringValue:",     ctl_set1 },
    { "NSControl", "setFont:",            ctl_set1 },
    { "NSControl", "setTextColor:",       ctl_set1 },
    { "NSControl", "setBackgroundColor:", ctl_set1 },
    { "NSControl", "setDelegate:",        ctl_set1 },
    { "NSControl", "setTarget:",          ctl_set1 },
    { "NSControl", "setAction:",          ctl_set1 },
    { "NSControl", "setFormatter:",       ctl_set1 },
    { "NSControl", "setAlignment:",       ctl_set1 },
    { "NSControl", "setLineBreakMode:",   ctl_set1 },
    { "NSControl", "setFocusRingType:",   ctl_set1 },
    { "NSControl", "selectText:",         ctl_set1 },
    { "NSControl", "stringValue",         ctl_nil },
    { "NSControl", "currentEditor",       ctl_nil },
    { "NSControl", "abortEditing",        view_yes },
    { "NSControl", "validateEditing",     view_void },
    { "NSCell", "setWraps:",              ctl_set_flag },
    { "NSCell", "setScrollable:",         ctl_set_flag },
    { "NSCell", "setEditable:",           ctl_set_flag },
    { "NSCell", "setSelectable:",         ctl_set_flag },
    { "NSCell", "setBordered:",           ctl_set_flag },
    { "NSCell", "setBezeled:",            ctl_set_flag },
    { "NSCell", "setDrawsBackground:",    ctl_set_flag },
    { "NSCell", "setUsesSingleLineMode:", ctl_set_flag },
    { "NSCell", "setTitle:",              ctl_set1 },
    { "NSCell", "setStringValue:",        ctl_set1 },
    { "NSCell", "setFont:",               ctl_set1 },
    { "NSCell", "setTextColor:",          ctl_set1 },
    { "NSCell", "setBackgroundColor:",    ctl_set1 },
    { "NSCell", "setAlignment:",          ctl_set1 },
    { "NSCell", "setLineBreakMode:",      ctl_set1 },
    { "NSCell", "setFocusRingType:",      ctl_set1 },
    { "NSCell", "stringValue",            ctl_nil },
    { "NSGraphicsContext", "+currentContext",      gctx_current },
    { "NSGraphicsContext", "+setCurrentContext:",  gctx_set_current },
    { "NSGraphicsContext", "+graphicsContextWithGraphicsPort:flipped:", gctx_with_port },
    { "NSGraphicsContext", "+saveGraphicsState",   gctx_void },
    { "NSGraphicsContext", "+restoreGraphicsState", gctx_void },
    { "NSGraphicsContext", "saveGraphicsState",    gctx_void },
    { "NSGraphicsContext", "restoreGraphicsState", gctx_void },
    { "NSGraphicsContext", "graphicsPort",         gctx_port },
    { "NSGraphicsContext", "CGContext",            gctx_port },
    { "NSGraphicsContext", "flushGraphics",        gctx_void },
    { "NSGraphicsContext", "isFlipped",            gctx_flipped },
    { "NSScreen", "+mainScreen",          screen_main },
    { "NSScreen", "+deepestScreen",       screen_main },
    { "NSScreen", "frame",                screen_frame },
    { "NSScreen", "visibleFrame",         screen_frame },
    { "NSScreen", "backingScaleFactor",   screen_scale },
    { "NSAnimationContext", "+currentContext", anim_current },
    { "NSAnimationContext", "+beginGrouping",  anim_void },
    { "NSAnimationContext", "+endGrouping",    anim_void },
    { "NSAnimationContext", "setDuration:",    anim_set_duration },
    { "NSString", "lastPathComponent",             path_last_component },
    { "NSString", "stringByDeletingLastPathComponent", path_delete_last_component },
    { "NSString", "stringByDeletingPathExtension", path_delete_extension },
    { "NSString", "pathExtension",                 path_extension },
    { "NSString", "stringByAppendingPathComponent:", path_append_component },
    { "NSString", "stringByAppendingString:",      path_append_string },
    { "NSFileManager", "+defaultManager",   fm_default },
    { "NSFileManager", "fileExistsAtPath:", fm_exists },
    { "NSBundle", "bundleIdentifier",       bundle_path },
    /* CAMetalLayer belongs to macmetal.c, which backs it with a real
     * framebuffer. Stubs here would win, because the earlier installer's
     * methods are found first. */
    { "CALayer", "+layer",                  anim_current },
    { "CALayer", "setContentsScale:",       anim_set_duration },
    { "CALayer", "setNeedsDisplay",         layer_void },

    /* timers, run loop, and the NSView methods a subclass inherits */
    { "NSTimer", "+timerWithTimeInterval:target:selector:userInfo:repeats:", timer_make },
    { "NSTimer", "+scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", timer_make },
    { "NSTimer", "invalidate",            timer_invalidate },
    { "NSTimer", "userInfo",              timer_userinfo },
    { "NSArray", "countByEnumeratingWithState:objects:count:",        arr_fast_enum },
    { "NSMutableArray", "countByEnumeratingWithState:objects:count:", arr_fast_enum },
    { "NSView", "removeAllToolTips",      view_void },

    /* NSEvent, so synthesised input reads the way Cocoa's would */
    { "NSEvent", "type",                  ev_type },
    { "NSEvent", "locationInWindow",      ev_loc },
    { "NSEvent", "buttonNumber",          ev_button },
    { "NSEvent", "clickCount",            ev_clicks },
    { "NSEvent", "modifierFlags",         ev_mods },
    { "NSEvent", "keyCode",               ev_keycode },
    { "NSEvent", "deltaX",                ev_dx },
    { "NSEvent", "deltaY",                ev_dy },
    { "NSEvent", "scrollingDeltaX",       ev_dx },
    { "NSEvent", "scrollingDeltaY",       ev_dy },
    { "NSEvent", "characters",            ev_chars },
    { "NSEvent", "charactersIgnoringModifiers", ev_chars },
    { "NSEvent", "isARepeat",             ev_no },
    { "NSEvent", "hasPreciseScrollingDeltas", ev_no },
    { "NSEvent", "isDirectionInvertedFromDevice", ev_no },
    { "NSEvent", "pressure",              ev_pressure },
    /* Screen coordinates, counted from the bottom -- the one place this host
     * has to speak Cocoa's convention rather than its own. */
    { "NSEvent", "+mouseLocation",        ev_mouse_location },
    { "NSEvent", "window",                ev_nil },
    { "NSRunLoop", "+currentRunLoop",     runloop_current },
    { "NSRunLoop", "+mainRunLoop",        runloop_current },
    { "NSRunLoop", "addTimer:forMode:",   runloop_add_timer },
    { "NSString", "containsString:",      str_contains },
    { "NSString", "stringByAppendingPathExtension:", path_append_extension },
    { "NSArray", "+arrayWithObjects:",    arr_with_objects },
    { "NSMutableArray", "+arrayWithObjects:", arr_with_objects },
    { "NSView", "setLayerContentsRedrawPolicy:", anim_set_duration },
    { "NSView", "registerForDraggedTypes:",      layer_set_ptr },
    { "NSView", "unregisterDraggedTypes",        layer_void },

    /* NSBundle */
    { "NSBundle", "+mainBundle",            bundle_main },
    { "NSBundle", "+bundleWithIdentifier:", bundle_with_id },
    { "NSBundle", "+bundleWithPath:",       bundle_with_id },
    { "NSBundle", "bundlePath",             bundle_path },
    { "NSBundle", "resourcePath",           bundle_resource_path },
    { "NSBundle", "pathForResource:ofType:", bundle_path_for_resource },
    { "NSBundle", "URLForResource:withExtension:", bundle_url_for_resource },
    { "NSBundle", "objectForInfoDictionaryKey:", bundle_info_value },
    { "NSBundle", "load",                   bundle_load },

    { "NSDictionary", "+dictionary",       dict_new },
    { "NSDictionary", "objectForKey:",     dict_object_for },
    { "NSDictionary", "count",             dict_count },
    { "NSMutableDictionary", "+dictionary", dict_new },
    { "NSMutableDictionary", "+dictionaryWithCapacity:", dict_with_capacity },
    { "NSMutableDictionary", "objectForKey:", dict_object_for },
    { "NSMutableDictionary", "setObject:forKey:", dict_set },
    { "NSMutableDictionary", "count",      dict_count },
    { NULL, NULL, NULL }
};

/* Read any string, ours or CoreFoundation's, as UTF-8. macmetal.c needs this to
 * read a shader function name out of an @"..." literal. */
const char *macns_utf8(void *str) { return str_bytes(str, NULL); }

/* A host-owned NSView, for a plugin that needs a parent to graft its own view
 * into. VST2 on macOS is handed NULL and makes its own top-level view, but VST3
 * has no such convention -- IPlugView::attached wants a real NSView, and a
 * plugin handed nothing simply refuses. This is that container: an instance of
 * the same stand-in class the plugin's own views inherit from, so addSubview:
 * and the geometry calls all behave. */
void *macns_make_view(int w, int h)
{
    id v = ns_new_of("NSView");
    NSRect r;
    id (*initf)(id, SEL, NSRect);

    if (!v) return NULL;
    r.origin.x = 0.0; r.origin.y = 0.0;
    r.size.w = w > 0 ? (double)w : 0.0;
    r.size.h = h > 0 ? (double)h : 0.0;
    initf = (id (*)(id, SEL, NSRect))macobjc_lookup(v, "initWithFrame:");
    return initf ? initf(v, "initWithFrame:", r) : v;
}

void macns_install(void)
{
    int i;
    for (i = 0; g_table[i].cls; i++)
        macobjc_add_method(g_table[i].cls, g_table[i].sel, g_table[i].imp);
}
