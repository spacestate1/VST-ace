/* A minimal Objective-C runtime for Mach-O plugins.
 *
 * Cocoa cannot be cloned -- AppKit, CoreGraphics and Metal are closed. But the
 * *runtime* is just a dispatch mechanism over layouts that are documented ABI,
 * and the classes a plugin references have to be ours in any case. So this
 * implements the runtime and stands in for the Apple classes, the same way
 * win32gui.h stands in for GDI on the Windows side.
 *
 * Two decisions make it small:
 *
 *  - SEL is the selector's name string. In a compiled image a method_t.name and
 *    each __objc_selrefs entry already point at the name in __TEXT,__objc_methname;
 *    Apple's runtime later replaces them with uniqued pointers, but nothing
 *    requires that. Leaving them as strings means no fixup pass and lookup is a
 *    name comparison.
 *
 *  - An unrealized class's `bits` field points straight at its class_ro_t, which
 *    is what a statically compiled class in a binary contains. Reading that
 *    directly avoids implementing class realization at all.
 *
 * What this does NOT do is make a GUI work. Once the image binds, the plugin
 * starts sending messages, and every selector it sends needs an implementation
 * behind it. Unknown selectors are logged rather than guessed at, which is how
 * the list of what to implement next gets discovered.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "macshim.h"
#include "macobjc.h"

/* --------------------------------------------------------------- ABI layout */

typedef const char *SEL;                 /* the name string, see above */
typedef void *id;
typedef void (*IMP)(void);

typedef struct { SEL name; const char *types; IMP imp; } method_t;
typedef struct { uint32_t entsize, count; method_t first[1]; } method_list_t;

typedef struct {
    uint32_t flags, instanceStart, instanceSize, reserved;
    const uint8_t *ivarLayout;
    const char    *name;
    method_list_t *baseMethods;
    void          *baseProtocols;
    void          *ivars;
    const uint8_t *weakIvarLayout;
    void          *baseProperties;
} class_ro_t;

/* isa, superclass, a 16-byte cache, then bits. 40 bytes on x86-64. */
typedef struct objc_class {
    struct objc_class *isa;
    struct objc_class *superclass;
    void     *cache_buckets;
    uint32_t  cache_mask, cache_occupied;
    uintptr_t bits;
} objc_class;
_Static_assert(sizeof(objc_class) == 40, "objc_class is 40 bytes on x86-64");

#define FAST_DATA_MASK 0x00007ffffffffff8ul

static class_ro_t *class_ro(objc_class *c)
{ return c ? (class_ro_t *)(c->bits & FAST_DATA_MASK) : NULL; }

/* Every object begins with its class pointer, and nothing else -- the refcount
 * is deliberately *not* here.
 *
 * Apple's runtime keeps the count in the isa's spare bits or in a side table. A
 * plain `long refs` after the isa looks harmless until a plugin subclasses
 * NSObject, because the compiler puts that subclass's first ivar at offset 8 --
 * exactly where such a refcount would sit. nanovg's MNVGcontext keeps its Metal
 * command buffer there, so every retain of the context incremented the stored
 * command-buffer pointer by one, and the next message went to a misaligned
 * address. The same collision cost an NSView its text field (see macns.c).
 *
 * So the count goes in front of the object, behind a magic word marking the
 * allocation as ours. Class objects, constant CFStrings and anything else we did
 * not allocate have no such header and are treated as immortal, which is the
 * right answer for them anyway.
 *
 * Reaching zero retires an object but does not free it. There are no autorelease
 * pools here and no weak references, so the retain/release traffic a plugin emits
 * cannot be balanced faithfully -- and an over-release then hands the allocator a
 * block that is still in use, which surfaces much later as a garbage isa. nanovg's
 * completion handler did exactly that. Retiring instead of freeing turns an
 * unbalanced release into a no-op rather than a corrupted heap; the cost is that a
 * plugin's objects accumulate for as long as its editor is open. */
typedef struct { objc_class *isa; } obj_header;

#define OBJ_MAGIC 0x6f626a6331706531ull            /* "objc1pe1" */
typedef struct obj_prefix {
    unsigned long      magic;
    long               refs;
    struct obj_prefix *reg_next;   /* see the registry below */
} obj_prefix;

/* ---- which pointers this runtime allocated -----------------------------
 *
 * `obj_pre` answers "did we make this?", and it used to answer by reading the
 * sixteen bytes *before* the pointer and comparing a magic word. That is a read
 * through a pointer this runtime did not make, and it is asked constantly:
 * every retain, every release, every ARC helper.
 *
 * A plugin supplies pointers we did not make all the time, and legitimately.
 * dispatch objects are Objective-C objects on macOS, so ARC emits
 * objc_retainAutoreleasedReturnValue on a dispatch_semaphore_t -- and this
 * host's semaphores are a plain struct with nothing in front of them.
 * AddressSanitizer caught it reading sixteen bytes before one.
 *
 * So the same answer as the CoreFoundation side: a hash set of what we
 * allocated, keyed by address, with the link stored in the prefix so there is
 * nothing extra to allocate. An entry is removed when the object is retired,
 * which is also exactly when obj_pre should start saying no.
 *
 * Locked, because a plugin's dispatched blocks run on threads of their own and
 * allocate objects there -- see gcd_async. The bucket count is generous so the
 * chains stay at one or two entries and the lock is held for a handful of
 * instructions; this is on the path of every retain and every release. */
#define OBJREG_BUCKETS 8192
static obj_prefix *g_objreg[OBJREG_BUCKETS];
static pthread_mutex_t g_objreg_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned objreg_bucket(const void *p)
{ return (unsigned)((((uintptr_t)p) >> 4) * 2654435761u) % OBJREG_BUCKETS; }

static void objreg_add(obj_prefix *pre)
{
    unsigned b = objreg_bucket(pre + 1);
    pthread_mutex_lock(&g_objreg_lock);
    pre->reg_next = g_objreg[b];
    g_objreg[b] = pre;
    pthread_mutex_unlock(&g_objreg_lock);
}
static void objreg_del(obj_prefix *pre)
{
    unsigned b = objreg_bucket(pre + 1);
    obj_prefix **pp;
    pthread_mutex_lock(&g_objreg_lock);
    for (pp = &g_objreg[b]; *pp; pp = &(*pp)->reg_next)
        if (*pp == pre) { *pp = pre->reg_next; break; }
    pthread_mutex_unlock(&g_objreg_lock);
}

/* The bookkeeping for `p`, or NULL if we did not allocate it. */
static obj_prefix *obj_pre(void *p)
{
    obj_prefix *pre, *found = NULL;
    unsigned b;
    if (!p) return NULL;
    b = objreg_bucket(p);
    pthread_mutex_lock(&g_objreg_lock);
    for (pre = g_objreg[b]; pre; pre = pre->reg_next)
        if ((void *)(pre + 1) == p) { found = pre; break; }
    pthread_mutex_unlock(&g_objreg_lock);
    return (found && found->magic == OBJ_MAGIC) ? found : NULL;
}

static int oc_verbose(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("MACOBJC_VERBOSE"); v = e && *e != '0'; } return v; }
#define OLOG(...) do { if (oc_verbose()) fprintf(stderr, __VA_ARGS__); } while (0)

/* ------------------------------------------------------- unknown selectors */

/* Which selectors were sent to something that does not implement them. This is
 * the work list for making a GUI actually run, so it is recorded rather than
 * silently swallowed. */
#define MAX_MISSED 512
static struct { const char *cls, *sel; unsigned long n; } g_missed[MAX_MISSED];
static int g_nmissed;

static void note_missed(const char *cls, SEL sel)
{
    int i;
    for (i = 0; i < g_nmissed; i++)
        if (g_missed[i].sel == sel && g_missed[i].cls == cls) { g_missed[i].n++; return; }
    if (g_nmissed < MAX_MISSED) {
        g_missed[g_nmissed].cls = cls;
        g_missed[g_nmissed].sel = sel;
        g_missed[g_nmissed].n = 1;
        g_nmissed++;
        OLOG("  [objc] %s does not implement -%s\n", cls ? cls : "?", sel ? sel : "?");
    }
}

void macobjc_each_missed(void (*cb)(const char *cls, const char *sel,
                                    unsigned long n, void *ud), void *ud)
{
    int i;
    if (!cb) return;
    for (i = 0; i < g_nmissed; i++)
        cb(g_missed[i].cls, g_missed[i].sel, g_missed[i].n, ud);
}
int macobjc_missed_count(void) { return g_nmissed; }

/* ------------------------------------------------------------ class registry */

#define MAX_CLASSES 512
static objc_class *g_classes[MAX_CLASSES];
static int g_nclasses;

static const char *class_name(objc_class *c)
{
    class_ro_t *ro = class_ro(c);
    return (ro && ro->name) ? ro->name : "?";
}

static void register_class(objc_class *c)
{
    if (!c) return;
    if (g_nclasses >= MAX_CLASSES) {
        /* Said once, and out loud rather than through OLOG: a full table is not
         * a lost log line. An unregistered class is not recognised as one, so
         * every instance of it stops being an object as far as
         * macobjc_isa_named is concerned -- which is silent, and shows up much
         * later as a Foundation method that did nothing. */
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [objc] class table full at %d -- %s and any "
                            "later class will not be recognised\n",
                    MAX_CLASSES, class_name(c));
        }
        return;
    }
    g_classes[g_nclasses++] = c;
    OLOG("  [objc] class %s\n", class_name(c));
}

/* ---------------------------------------------------------------- dispatch */

/* Walk the class and its superclasses for a method with this name. */
static IMP find_method(objc_class *cls, SEL sel)
{
    objc_class *c;
    int depth = 0;
    if (!sel) return NULL;
    /* Bounded: a malformed image can present a superclass cycle, and an
     * unbounded walk turns that into a hang with no diagnostic. */
    for (c = cls; c && depth < 64; c = c->superclass, depth++) {
        class_ro_t *ro = class_ro(c);
        method_list_t *ml = ro ? ro->baseMethods : NULL;
        if (ml) {
            uint32_t i, es = ml->entsize & ~(uint32_t)3;
            const uint8_t *p = (const uint8_t *)ml->first;
            if (es < sizeof(method_t)) es = sizeof(method_t);
            for (i = 0; i < ml->count; i++) {
                const method_t *m = (const method_t *)(p + (size_t)i * es);
                /* An entry with a null imp must not be returned: the trampoline
                 * tail-jumps to whatever comes back, so NULL becomes a call to
                 * address zero rather than a missing method. */
                if (m->imp && m->name && (m->name == sel || !strcmp(m->name, sel)))
                    return m->imp;
            }
        }
    }
    return NULL;
}

/* Called from the assembly trampolines: resolve, or return the nil-returning
 * stub. Returning NULL is not an option -- the trampoline tail-jumps to whatever
 * comes back. The stub is in assembly because it has to zero the return
 * registers; see macobjc_msgsend.S. */
extern void macobjc_nil_return(void);
#define nil_return macobjc_nil_return

/* The Foundation implementations live in another translation unit, so they are
 * installed on first use rather than from a constructor -- constructor order
 * between translation units is not specified, and they need the classes first. */
extern void macns_install(void);
extern void macmetal_install(void);
static void ensure_installed(void)
{ static int done; if (!done) { done = 1; macns_install(); macmetal_install(); } }

/* Every dispatch, not just the ones that miss. Expensive, so it is off unless
 * MACOBJC_TRACE names a substring to match -- "Bundle" to watch resource
 * lookups, "" for everything. A missed-selector list only shows what went
 * unanswered; when the problem is a message never sent, this is what shows it. */
static const char *trace_filter(void)
{ static const char *f; static int done;
  if (!done) { done = 1; f = getenv("MACOBJC_TRACE"); } return f; }

/* The handful of messages that mean something to a CoreFoundation object. */
static id  cf_msg_retain(id self, SEL sel)
{ (void)sel; return (id)macshim_cf_retain_obj(self); }
static void cf_msg_release(id self, SEL sel)
{ (void)sel; macshim_cf_release_obj(self); }
static id  cf_msg_self(id self, SEL sel) { (void)sel; return self; }

static void (*cf_bridged(SEL sel))(void)
{
    if (!strcmp(sel, "release"))     return (IMP)cf_msg_release;
    if (!strcmp(sel, "retain"))      return (IMP)cf_msg_retain;
    if (!strcmp(sel, "autorelease")) return (IMP)cf_msg_self;
    if (!strcmp(sel, "self"))        return (IMP)cf_msg_self;
    /* Anything else is not something we can answer for a CF object, but a null
     * return is safe where walking a bogus class is not. */
    return nil_return;
}

void (*macobjc_lookup(void *selfp, const char *selname))(void)
{
    ensure_installed();
    { const char *f = trace_filter();
      if (f && selfp && selname) {
          const char *cn = class_name(((obj_header *)selfp)->isa);
          if (!*f || strstr(cn, f) || strstr(selname, f))
              fprintf(stderr, "  [objc] %s -%s\n", cn, selname);
      } }
    id self = selfp;
    SEL sel = selname;
    objc_class *cls;
    IMP imp;
    if (!self) return nil_return;                /* messaging nil yields nil */
    /* A CoreFoundation object may arrive here: CF and Objective-C types are
     * interchangeable on macOS, so `[cfString release]` is legal and WDL's
     * compatibility layer relies on it. Reading the CF header as an isa and
     * walking it as a class is how that used to end. */
    if (macshim_cf_is_object(self)) return cf_bridged(sel);
    cls = ((obj_header *)self)->isa;
    if ((imp = find_method(cls, sel))) return imp;
    note_missed(class_name(cls), sel);
    return nil_return;                           /* never NULL: see find_method */
}

/* Defined with the class registry further down. Declared here because the type
 * tests and the class accessors below have to refuse anything that is not a
 * class this runtime minted -- they are asked about pointers that are not
 * objects at all. */
static int known_class(const objc_class *c);

/* objc_msgSendSuper2 gets a struct { receiver, current class } and starts the
 * search at that class's superclass. */
typedef struct { id receiver; objc_class *cls; } super2;

void (*macobjc_lookup_super(void *sp, const char *selname))(void)
{
    ensure_installed();
    super2 *s = sp;
    SEL sel = selname;
    IMP imp;
    if (!s || !s->receiver) return nil_return;
    if ((imp = find_method(s->cls ? s->cls->superclass : NULL, sel))) return imp;
    note_missed(s->cls ? class_name(s->cls) : "?", sel);
    return nil_return;
}
/* The older objc_msgSendSuper: s->cls is already where the search starts, not
 * the class to search above it. The two structs are the same shape, so only the
 * lookup differs -- and getting them the wrong way round is silent, because a
 * class that inherits the method it is overriding still finds *a* method. */
void (*macobjc_lookup_super1(void *sp, const char *selname))(void)
{
    ensure_installed();
    super2 *s = sp;
    SEL sel = selname;
    IMP imp;
    if (!s || !s->receiver) return nil_return;
    if ((imp = find_method(s->cls, sel))) return imp;
    note_missed(s->cls ? class_name(s->cls) : "?", sel);
    return nil_return;
}

void *macobjc_super_receiver(void *sp)
{ super2 *s = sp; return s ? s->receiver : NULL; }

/* class_getSuperclass / class_getName / object_getClass. A class built at
 * runtime is walked by the code that built it, which is the one case where a
 * plugin asks the runtime about a class rather than messaging it. */
static void *oc_class_get_superclass(void *cls)
{ return (cls && known_class(cls)) ? ((objc_class *)cls)->superclass : NULL; }
static const char *oc_class_get_name(void *cls)
{ return (cls && known_class(cls)) ? class_name(cls) : ""; }
static void *oc_object_get_class(void *obj)
{ return obj ? ((obj_header *)obj)->isa : NULL; }

/* --------------------------------------------------- NSObject and stand-ins */

/* Root behaviour, so alloc/init/retain/release work on anything. The methods
 * are ordinary C functions with (self, sel) leading, which is the calling
 * convention objc_msgSend hands them. */
static id ns_alloc(id self, SEL sel)
{
    objc_class *cls = (objc_class *)self;        /* a class message */
    class_ro_t *ro = class_ro(cls);
    /* Our own classes lay a struct over the instance, so never allocate less
     * than the two words those structs begin with.
     *
     * And then some slack, because instanceSize is not reliably the size of the
     * instance here. Objective-C's modern runtime slides a subclass's ivars at
     * class realization, against whatever its superclass turned out to be;
     * nothing here realizes classes, so a plugin's subclass keeps the offsets
     * the compiler wrote against Apple's headers and this host's stand-in
     * superclass is a different size. AddressSanitizer caught the result: an
     * Audio Unit's Cocoa view factory reading eight bytes off the end of the
     * thirty-two it had been given, from inside a Metal completion handler. The
     * slack turns that from a heap overflow into a read of zeroes -- which is
     * what a field the plugin has not written should be anyway. */
    size_t sz = ro && ro->instanceSize > 16 ? ro->instanceSize : 16;
    sz += 256;
    obj_prefix *pre = calloc(1, sizeof *pre + sz);
    obj_header *o;
    (void)sel;
    if (!pre) return NULL;
    pre->magic = OBJ_MAGIC;
    pre->refs = 1;
    o = (obj_header *)(pre + 1);
    o->isa = cls;
    objreg_add(pre);
    return o;
}
static id  ns_init(id self, SEL sel)          { (void)sel; return self; }
static id  ns_self(id self, SEL sel)          { (void)sel; return self; }
static id  ns_retain(id self, SEL sel)
{ obj_prefix *pre = obj_pre(self); (void)sel; if (pre) pre->refs++; return self; }
/* Retire rather than free: see the note above obj_prefix. */
static unsigned long g_retired;

static void obj_retire(obj_prefix *pre)
{
    if (!pre) return;
    objreg_del(pre);
    pre->magic = 0;          /* a further release finds no header and does nothing */
    g_retired++;
}

unsigned long macobjc_retired_count(void) { return g_retired; }

static void ns_release(id self, SEL sel)
{
    obj_prefix *pre = obj_pre(self);
    (void)sel;
    if (pre && --pre->refs <= 0) obj_retire(pre);
}
static void ns_dealloc(id self, SEL sel)
{ (void)sel; obj_retire(obj_pre(self)); }
static id  ns_class(id self, SEL sel)
{ (void)sel; return self ? ((obj_header *)self)->isa : NULL; }
/* +class, which is not -class.
 *
 * An instance's -class is its isa; a class's +class is the class itself. One
 * implementation cannot serve both, and the instance form applied to a class
 * hands back the *metaclass*. That is not a cosmetic difference: VSTGUI builds
 * its NSView subclass with objc_allocateClassPair([NSView class], ...), so it
 * was handed NSView's metaclass, its subclass inherited from the class-side of
 * the pair, and [super initWithFrame:] then looked for an instance method among
 * class methods and found nothing. Every VSTGUI editor in this corpus stopped
 * exactly there. */
static id  ns_class_self(id self, SEL sel) { (void)sel; return self; }
static signed char ns_responds(id self, SEL sel, SEL q)
{ (void)sel; return self && find_method(((obj_header *)self)->isa, q) ? 1 : 0; }
static signed char ns_no(id self, SEL sel)    { (void)self; (void)sel; return 0; }

/* isKindOfClass:, answered rather than refused.
 *
 * This used to return NO for everything, which is a lie about every object the
 * runtime hands out. The chain walk is bounded and gated on classes this
 * runtime knows, for the same reason macobjc_isa_named is: the question gets
 * asked about things that are not objects. */
static signed char ns_is_kind(id self, SEL sel, void *cls)
{
    objc_class *c;
    int depth = 0;
    (void)sel;
    if (!self || !cls) return 0;
    c = ((obj_header *)self)->isa;
    if (!known_class(c) || !known_class((const objc_class *)cls)) return 0;
    for (; c && depth < 64; c = c->superclass, depth++) {
        if (c == (objc_class *)cls) return 1;
        if (c->superclass && !known_class(c->superclass)) break;
    }
    return 0;
}

/* isMemberOfClass: is the exact class, not the chain -- a separate question
 * from isKindOfClass:, and answering both the same way would tell a plugin its
 * subclass is its superclass. */
static signed char ns_is_member(id self, SEL sel, void *cls)
{
    (void)sel;
    if (!self || !cls) return 0;
    return known_class(((obj_header *)self)->isa) &&
           ((obj_header *)self)->isa == (objc_class *)cls;
}

/* performSelector: and its two argument-carrying forms.
 *
 * Not decoration: a plugin uses these exactly when it cannot name a selector at
 * compile time, which is what a class it built at runtime forces. VSTGUI does
 * that to reach its own NSView subclass's initialiser --
 * [[VSTGUI_NSView alloc] performSelector:@selector(initWithFrame:andCFrame:)
 *                                 withObject:.. withObject:..] -- so with these
 * missing the send returned nil, the frame was never built, and the editor
 * reported an empty rect. Every VSTGUI editor in this corpus stopped there.
 *
 * A selector the receiver does not implement gets nil, which is what Objective-C
 * does for an unhandled message rather than what it does for performSelector:
 * (which raises). Nothing here can raise, and nil is the answer the caller
 * already has to survive. */
static id ns_perform(id self, SEL sel, SEL q)
{
    IMP imp;
    (void)sel;
    if (!self || !q) return NULL;
    imp = find_method(((obj_header *)self)->isa, q);
    return imp ? ((id (*)(id, SEL))imp)(self, q) : NULL;
}
static id ns_perform1(id self, SEL sel, SEL q, id a)
{
    IMP imp;
    (void)sel;
    if (!self || !q) return NULL;
    imp = find_method(((obj_header *)self)->isa, q);
    return imp ? ((id (*)(id, SEL, id))imp)(self, q, a) : NULL;
}
static id ns_perform2(id self, SEL sel, SEL q, id a, id b)
{
    IMP imp;
    (void)sel;
    if (!self || !q) return NULL;
    imp = find_method(((obj_header *)self)->isa, q);
    return imp ? ((id (*)(id, SEL, id, id))imp)(self, q, a, b) : NULL;
}

static method_t g_nsobject_methods[] = {
    { "alloc",              "@@:",   (IMP)ns_alloc },
    { "new",                "@@:",   (IMP)ns_alloc },
    { "init",               "@@:",   (IMP)ns_init },
    { "self",               "@@:",   (IMP)ns_self },
    { "retain",             "@@:",   (IMP)ns_retain },
    { "release",            "v@:",   (IMP)ns_release },
    { "autorelease",        "@@:",   (IMP)ns_self },
    { "dealloc",            "v@:",   (IMP)ns_dealloc },
    { "class",              "#@:",   (IMP)ns_class },
    { "respondsToSelector:", "c@::", (IMP)ns_responds },
    { "isKindOfClass:",     "c@:#",  (IMP)ns_is_kind },
    { "isMemberOfClass:",   "c@:#",  (IMP)ns_is_member },
    { "conformsToProtocol:", "c@:@", (IMP)ns_no },
    { "performSelector:",   "@@::",  (IMP)ns_perform },
    { "performSelector:withObject:", "@@::@", (IMP)ns_perform1 },
    { "performSelector:withObject:withObject:", "@@::@@", (IMP)ns_perform2 }
};
static method_list_t *g_nsobject_ml;

/* The Apple classes a plugin references. Each is a real class object inheriting
 * NSObject, so alloc/init/retain/release behave; anything else is logged. This
 * is the surface that has to grow for a GUI to run. */
static const char *const g_apple_classes[] = {
    "NSObject", "NSApplication", "NSAutoreleasePool", "NSBundle", "NSColor",
    "NSColorPanel", "NSCursor", "NSData", "NSDictionary", "NSEvent",
    "NSFileManager", "NSFont", "NSGraphicsContext", "NSImage", "NSMenu",
    "NSMenuItem", "NSMutableArray", "NSMutableDictionary", "NSNotificationCenter",
    "NSNumber", "NSOpenGLContext", "NSOpenGLPixelFormat", "NSOpenPanel",
    "NSPasteboard", "NSSavePanel", "NSScreen", "NSString", "NSTextField",
    "NSTextView", "NSThread", "NSTimer", "NSTrackingArea", "NSURL", "NSValue",
    "NSView", "NSWindow", "NSWorkspace", "NSArray", "NSAlert", "NSBezierPath",
    "NSScrollView", "NSClipView", "NSPopUpButton", "NSSlider", "NSButton",
    "CALayer", "CAMetalLayer", "CATransaction",
    "MTLRenderPassDescriptor", "MTLRenderPipelineDescriptor",
    "MTLDepthStencilDescriptor", "MTLTextureDescriptor", "MTLSamplerDescriptor",
    "MTKView", "MTLStencilDescriptor", "MTLVertexDescriptor",
    "NSCell", "NSControl",
    "NSAffineTransform", "NSAnimationContext", "NSBitmapImageRep",
    "NSDraggingItem", "NSFormatter", "NSMutableCharacterSet", "NSMutableString",
    "NSPasteboardItem", "NSProcessInfo", "NSRunLoop", "NSTextFieldCell",
    NULL
};

#define MAX_SYNTH 224
#define MAX_METHODS 96

/* Each stand-in carries its own writable method list, so real behaviour can be
 * attached class by class as the missed-selector log says what is needed. A "+"
 * prefix on the selector puts the method on the metaclass, which is where a
 * class method has to live for [NSString stringWithFoo:] to resolve. */
typedef struct {
    uint32_t entsize, count;
    method_t m[MAX_METHODS];
} synth_ml;

#define MAX_IVARS 8

/* An Ivar, as the runtime hands it out: a pointer to this record. It lives in
 * the class table and so stays valid for the life of the class, which is what
 * the API promises. */
typedef struct { char name[32]; uint32_t off, size; } synth_ivar;

static struct {
    objc_class cls, meta;
    class_ro_t ro, mro;
    synth_ml   ml, mml;
    char name[64];
    int  used;
    /* Ivars added at runtime -- see objc_allocateClassPair below. A class the
     * plugin assembled has no compiled-in offsets to reach its own fields by,
     * so it asks the runtime; class_getInstanceVariable answers from here. */
    uint32_t ivar_next;
    int      nivar;
    synth_ivar ivar[MAX_IVARS];
} g_synth[MAX_SYNTH];
static int g_nsynth;
static objc_class *g_nsobject;

static objc_class *synth_class(const char *name, objc_class *super)
{
    int i;
    /* Take a slot freed by objc_disposeClassPair before growing into a new one:
     * a plugin that builds its view class each time its editor opens would
     * otherwise exhaust the table over a session. */
    for (i = 0; i < g_nsynth; i++) if (!g_synth[i].used) break;
    if (i == g_nsynth) {
        if (g_nsynth >= MAX_SYNTH) return NULL;
        g_nsynth++;
    }
    memset(&g_synth[i], 0, sizeof g_synth[i]);
    g_synth[i].used = 1;
    snprintf(g_synth[i].name, sizeof g_synth[i].name, "%s", name);

    g_synth[i].ro.name = g_synth[i].name;
    g_synth[i].ro.instanceStart = (uint32_t)sizeof(obj_header);
    g_synth[i].ro.instanceSize  = (uint32_t)sizeof(obj_header) + 512;
    g_synth[i].ml.entsize = sizeof(method_t);
    g_synth[i].ml.count = 0;
    g_synth[i].mml.entsize = sizeof(method_t);
    g_synth[i].mml.count = 0;
    g_synth[i].ro.baseMethods = (method_list_t *)&g_synth[i].ml;

    g_synth[i].mro = g_synth[i].ro;
    g_synth[i].mro.flags = 1;                   /* RO_META */
    g_synth[i].mro.baseMethods = (method_list_t *)&g_synth[i].mml;

    /* The root class holds NSObject's behaviour on both sides, so alloc/init/
     * retain/release resolve for every instance and every class. The two sides
     * are the same list except where the class form differs from the instance
     * one -- see ns_class_self. */
    if (!super) {
        size_t k, n = sizeof g_nsobject_methods / sizeof *g_nsobject_methods;
        for (k = 0; k < n && k < MAX_METHODS; k++) {
            g_synth[i].ml.m[k] = g_nsobject_methods[k];
            g_synth[i].mml.m[k] = g_nsobject_methods[k];
            if (!strcmp(g_nsobject_methods[k].name, "class"))
                g_synth[i].mml.m[k].imp = (IMP)ns_class_self;
        }
        g_synth[i].ml.count = (uint32_t)(n < MAX_METHODS ? n : MAX_METHODS);
        g_synth[i].mml.count = g_synth[i].ml.count;
    }

    g_synth[i].cls.bits = (uintptr_t)&g_synth[i].ro;
    g_synth[i].meta.bits = (uintptr_t)&g_synth[i].mro;
    g_synth[i].cls.isa = &g_synth[i].meta;
    g_synth[i].cls.superclass = super;
    /* The root metaclass is its own isa -- that is correct and harmless, isa is
     * never walked. Its *superclass* must be the root class, not itself: pointing
     * it at itself makes the superclass chain a cycle, and the method walk below
     * then never terminates. */
    g_synth[i].meta.isa = g_nsobject ? g_nsobject->isa : &g_synth[i].meta;
    g_synth[i].meta.superclass = super ? super->isa : &g_synth[i].cls;
    register_class(&g_synth[i].cls);
    return &g_synth[i].cls;
}

/* ------------------------------------------------- the plugin's own classes */

/* A compiled image lists its classes in __DATA,__objc_classlist as pointers to
 * class objects whose superclass field already points at the class we bound for
 * _OBJC_CLASS_$_Whatever. So nothing needs fixing up -- they only need to be
 * known, so a message to one can be traced. */
void macobjc_register_image_classes(void *const *classlist, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        objc_class *c = classlist[i];
        if (c) register_class(c);
    }
}

/* The other half, for when the image goes. The registry is a plain array
 * searched linearly, so a slot is freed by moving the last entry into it --
 * order carries no meaning here. */
void macobjc_forget_image_classes(void *const *classlist, size_t count)
{
    size_t i;
    int k;
    for (i = 0; i < count; i++) {
        objc_class *c = classlist[i];
        if (!c) continue;
        for (k = 0; k < g_nclasses; k++)
            if (g_classes[k] == c) {
                g_classes[k] = g_classes[--g_nclasses];
                break;
            }
    }
}

/* --------------------------------------------------------- retain/autorelease */

static id oc_retain(id p)
{ obj_prefix *pre = obj_pre(p); if (pre) pre->refs++; return p; }
static void oc_release(id p)
{
    obj_prefix *pre = obj_pre(p);
    /* No header means immortal (a class object, an @"..." literal) or already
     * retired. Either way there is nothing to do -- and checking this before
     * touching the isa matters, because a retired object's isa may be anything. */
    if (!pre) return;
    /* Route through -release if the class overrides it. */
    { IMP imp = find_method(((obj_header *)p)->isa, "release");
      if (imp && imp != (IMP)ns_release) { ((void (*)(id, SEL))imp)(p, "release"); return; } }
    if (--pre->refs <= 0) obj_retire(pre);
}
/* There is no autorelease pool here, so autoreleasing is a no-op and objects
 * handed out that way simply leak. That is safe. What is *not* safe is treating
 * objc_retainAutoreleasedReturnValue as a no-op: ARC emits it paired with an
 * objc_release, so skipping the retain makes the release the one that frees the
 * object. A plugin doing `id d = [layer device];` then destroyed its own Metal
 * device on the next line, and the crash surfaced later as a garbage isa. */
static id   oc_autorelease(id p) { return p; }
static id   oc_retain_autorelease(id p) { return oc_retain(p); }
static id   oc_retain_autoreleased_return(id p) { return oc_retain(p); }
static id   oc_autorelease_return(id p) { return p; }
static id   oc_unsafe_claim_autoreleased_return(id p) { return p; }
static void *oc_pool_push(void) { return (void *)1; }
static void  oc_pool_pop(void *tok) { (void)tok; }
static void  oc_store_strong(id *loc, id v)
{
    if (!loc) return;
    if (*loc == v) return;
    oc_retain(v);
    oc_release(*loc);
    *loc = v;
}
static id oc_alloc(objc_class *cls) { return ns_alloc(cls, "alloc"); }
static id oc_alloc_init(objc_class *cls) { return ns_alloc(cls, "alloc"); }
static void oc_enumeration_mutation(id obj)
{ (void)obj; fprintf(stderr, "macho: collection mutated while enumerating\n"); }
static int  oc_personality(void) { return 0; }
/* SEL is the selector's name string in this runtime, so registering one is the
 * identity. Returning NULL here would send every dynamically-built message to
 * nothing. */
static const char *oc_sel_get_uid(const char *name) { return name; }

/* -------------------------------------------------------------------- setup */

static void __attribute__((constructor)) init_objc(void)
{
    size_t i, n = 0;
    static struct { uint32_t entsize, count; method_t m[16]; } nsml;

    while (g_apple_classes[n]) n++;

    nsml.entsize = sizeof(method_t);
    nsml.count = sizeof g_nsobject_methods / sizeof *g_nsobject_methods;
    memcpy(nsml.m, g_nsobject_methods, sizeof g_nsobject_methods);
    g_nsobject_ml = (method_list_t *)&nsml;

    g_nsobject = synth_class("NSObject", NULL);
    for (i = 0; i < n; i++) {
        if (!strcmp(g_apple_classes[i], "NSObject")) continue;
        synth_class(g_apple_classes[i], g_nsobject);
    }
    /* Then the hierarchy. Every stand-in above is minted as a direct NSObject
     * subclass, which is enough for anything a plugin only messages -- and
     * wrong the moment it *subclasses* one. An NSTextField is an NSView, so a
     * plugin's IGraphicsTextField expects [super initWithFrame:] to find
     * NSView's; with the chain flat it found nothing, the field was never
     * built, and clicking a control that offers text entry took the host down.
     *
     * Only the relationships a plugin actually leans on are named. The rest
     * stay flat, because inventing a hierarchy nothing needs is a way to move a
     * method resolution somewhere surprising. */
    {
        static const struct { const char *cls, *super; } parents[] = {
            { "NSControl",       "NSView"    },
            { "NSTextField",     "NSControl" },
            { "NSTextView",      "NSView"    },
            { "NSScrollView",    "NSView"    },
            { "NSClipView",      "NSView"    },
            { "NSButton",        "NSControl" },
            { "NSPopUpButton",   "NSButton"  },
            { "NSSlider",        "NSControl" },
            { "NSTextFieldCell", "NSCell"    },
            { NULL, NULL }
        };
        int k;
        for (k = 0; parents[k].cls; k++) {
            objc_class *c = macobjc_class(parents[k].cls);
            objc_class *p = macobjc_class(parents[k].super);
            if (!c || !p) continue;
            c->superclass = p;
            c->isa->superclass = p->isa;   /* the metaclass chain runs alongside */
        }
    }
}

/* Attach an implementation. `sel` beginning with '+' means a class method. */
int macobjc_add_method(const char *cls, const char *sel, void *imp)
{
    int i, meta = (sel && sel[0] == '+');
    const char *name = meta ? sel + 1 : sel;
    for (i = 0; i < g_nsynth; i++) {
        synth_ml *ml;
        if (!g_synth[i].used || strcmp(g_synth[i].name, cls)) continue;
        ml = meta ? &g_synth[i].mml : &g_synth[i].ml;
        if (ml->count >= MAX_METHODS) return -1;
        ml->m[ml->count].name = name;
        ml->m[ml->count].types = "";
        ml->m[ml->count].imp = (IMP)imp;
        ml->count++;
        return 0;
    }
    return -1;
}

/* Is this object's class, or an ancestor, called `name`? Used by the Foundation
 * implementations to refuse an object whose layout is not theirs -- a plugin can
 * hand over a CFString built on the CoreFoundation side. */
/* Is this a class object we know about? The isa of anything we did not allocate
 * is not a class at all, and walking it dereferences whatever happened to sit
 * there. */
static int known_class(const objc_class *c)
{
    int i;
    if (!c) return 0;
    for (i = 0; i < g_nclasses; i++) if (g_classes[i] == c) return 1;
    /* Metaclasses are not registered, so accept the pair's other half too. */
    for (i = 0; i < g_nsynth; i++)
        if (g_synth[i].used && (c == &g_synth[i].cls || c == &g_synth[i].meta)) return 1;
    return 0;
}

/* A type test that has to survive being handed something that is not an object.
 *
 * It is asked about every receiver reaching a Foundation method, and a plugin
 * legitimately passes things we never allocated -- an @"..." literal is a
 * constant CFString, not one of ours. Walking the superclass chain from a
 * non-object read a plausible pointer out of read-only data and dereferenced it,
 * which is a segfault inside what is only meant to be a question. Anything whose
 * isa is not a class this runtime minted simply is not the class being asked
 * about, which is the honest answer and a safe one. */
int macobjc_isa_named(void *obj, const char *name)
{
    objc_class *c;
    int depth = 0;
    if (!obj || !name) return 0;
    c = ((obj_header *)obj)->isa;
    if (!known_class(c)) return 0;
    for (; c && depth < 64; c = c->superclass, depth++) {
        if (!strcmp(class_name(c), name)) return 1;
        if (c->superclass && !known_class(c->superclass)) break;
    }
    return 0;
}

/* Metal hands out objects typed only by protocol -- id<MTLDevice>, id<MTLBuffer>
 * -- so no _OBJC_CLASS_$_MTLDevice exists to stand in for, and nothing in the
 * image ever names the class. Those have to be minted deliberately. */
void *macobjc_define_class(const char *name)
{
    void *k = macobjc_class(name);
    if (k) return k;
    /* init_objc is a constructor, so the root class already exists. */
    return g_nsobject ? synth_class(name, g_nsobject) : NULL;
}

void *macobjc_class(const char *name)
{
    int i;
    if (!name) return NULL;
    /* Our stand-ins first. A plugin that asks for "NSView" wants the class its
     * own subclasses were bound against, not some image's like-named one. */
    for (i = 0; i < g_nsynth; i++)
        if (g_synth[i].used && !strcmp(g_synth[i].name, name)) return &g_synth[i].cls;
    /* Then the image's own. NSClassFromString is how an Audio Unit's host is
     * meant to reach the Cocoa view class the unit names in
     * kAudioUnitProperty_CocoaUI -- the class is compiled into the plugin, so
     * answering only for stand-ins meant the name never resolved and the view
     * was never built. */
    for (i = 0; i < g_nclasses; i++)
        if (!strcmp(class_name(g_classes[i]), name)) return g_classes[i];
    return NULL;
}

/* ------------------------------------------- classes a plugin builds itself */

/* A view class assembled at runtime rather than compiled in.
 *
 * Audio Damage's editors are written that way -- allocateClassPair over NSView,
 * class_addMethod for drawRect: and the mouse handlers, then registerClassPair --
 * and with these missing the whole sequence returned nil and the editor was
 * refused. That is the entire reason those five plugins loaded and played but
 * showed no interface.
 *
 * Nothing new is needed underneath: a stand-in class already carries a writable
 * method list, which is exactly what class_addMethod wants, and a selector here
 * is its own name string, so the SEL the plugin passes is usable as-is. */

/* Which stand-in owns this class object, and which side of the pair it is. */
static int synth_index(objc_class *c, int *is_meta)
{
    int i;
    if (!c) return -1;
    for (i = 0; i < g_nsynth; i++) {
        if (!g_synth[i].used) continue;
        if (c == &g_synth[i].cls)  { if (is_meta) *is_meta = 0; return i; }
        if (c == &g_synth[i].meta) { if (is_meta) *is_meta = 1; return i; }
    }
    return -1;
}

static void *oc_look_up_class(const char *name)
{ return name ? macobjc_class(name) : NULL; }

static void *oc_allocate_class_pair(void *superclass, const char *name, size_t extra)
{
    objc_class *sup = superclass, *c;
    class_ro_t *sro;
    int i;

    if (!name) return NULL;
    /* nil when the name is taken, as Apple does -- these plugins look the class
     * up first and only build it when absent, so answering anything else would
     * have them register a second class over the first. */
    if (macobjc_class(name)) { OLOG("  [objc] class %s already exists\n", name); return NULL; }
    if (!(c = synth_class(name, sup))) {
        fprintf(stderr, "macho: no room for runtime class %s (%d in use)\n", name, g_nsynth);
        return NULL;
    }
    if ((i = synth_index(c, NULL)) < 0) return NULL;

    /* Ivars start past everything the superclass laid down. Our NSView and the
     * rest keep a struct at the front of the instance, and an ivar placed over
     * it would quietly overwrite the view's own state. */
    sro = class_ro(sup);
    g_synth[i].ro.instanceStart = sro ? sro->instanceSize : (uint32_t)sizeof(obj_header);
    g_synth[i].ivar_next = g_synth[i].ro.instanceStart + (uint32_t)extra;
    g_synth[i].ro.instanceSize = g_synth[i].ivar_next;
    OLOG("  [objc] runtime class %s : %s\n", name, sup ? class_name(sup) : "(root)");
    return c;
}

static int oc_class_add_method(void *cls, SEL sel, IMP imp, const char *types)
{
    int meta = 0, i = synth_index(cls, &meta);
    synth_ml *ml;
    uint32_t k;

    if (i < 0 || !sel || !imp) return 0;
    ml = meta ? &g_synth[i].mml : &g_synth[i].ml;
    /* Apple adds only when this class does not already define it -- and looks no
     * further than this class, so an override of an inherited method still
     * takes. */
    for (k = 0; k < ml->count; k++)
        if (ml->m[k].name && !strcmp(ml->m[k].name, sel)) return 0;
    if (ml->count >= MAX_METHODS) {
        fprintf(stderr, "macho: %s has no room for -%s\n", g_synth[i].name, sel);
        return 0;
    }
    ml->m[ml->count].name  = sel;
    ml->m[ml->count].types = types ? types : "";
    ml->m[ml->count].imp   = imp;
    ml->count++;
    return 1;
}

static int oc_class_add_ivar(void *cls, const char *name, size_t size,
                             uint8_t alignment, const char *types)
{
    int i = synth_index(cls, NULL);
    uint32_t align, off;

    (void)types;
    if (i < 0 || !name || !size) return 0;
    if (g_synth[i].nivar >= MAX_IVARS) {
        fprintf(stderr, "macho: %s has no room for ivar %s\n", g_synth[i].name, name);
        return 0;
    }
    /* The argument is a log2 alignment, which is what the compiler passes. */
    align = alignment < 31 ? (1u << alignment) : 1u;
    off = (g_synth[i].ivar_next + align - 1) & ~(align - 1);

    snprintf(g_synth[i].ivar[g_synth[i].nivar].name,
             sizeof g_synth[i].ivar[0].name, "%s", name);
    g_synth[i].ivar[g_synth[i].nivar].off  = off;
    g_synth[i].ivar[g_synth[i].nivar].size = (uint32_t)size;
    g_synth[i].nivar++;

    g_synth[i].ivar_next = off + (uint32_t)size;
    g_synth[i].ro.instanceSize = g_synth[i].ivar_next;
    return 1;
}

/* ---- reaching a runtime class's fields ---------------------------------
 *
 * A class assembled at runtime has no compiled-in ivar offsets, so the code
 * that built it asks the runtime where its fields went. VSTGUI keeps the CFrame
 * its view draws through in exactly such an ivar: with the lookup missing the
 * store went nowhere, and the view was built, marked itself dirty a hundred and
 * fifty times, and then drew nothing, because drawRect: could not find the
 * frame it was meant to draw.
 *
 * Every accessor bounds-checks the offset against the instance it is handed.
 * The offsets come from our own table, but the object does not have to. */
static void *oc_class_get_instance_variable(void *cls, const char *name)
{
    int i = synth_index(cls, NULL), k;
    if (i < 0 || !name) return NULL;
    for (k = 0; k < g_synth[i].nivar; k++)
        if (!strcmp(g_synth[i].ivar[k].name, name)) return &g_synth[i].ivar[k];
    return NULL;
}
static const char *oc_ivar_get_name(void *iv)
{ return iv ? ((synth_ivar *)iv)->name : ""; }
static long oc_ivar_get_offset(void *iv)
{ return iv ? (long)((synth_ivar *)iv)->off : 0; }

/* Where in `obj` an ivar sits, or NULL if it cannot be placed there. */
static void *ivar_slot(id obj, const synth_ivar *iv)
{
    class_ro_t *ro;
    if (!obj || !iv) return NULL;
    ro = class_ro(((obj_header *)obj)->isa);
    if (!ro || iv->off + iv->size > ro->instanceSize) return NULL;
    return (uint8_t *)obj + iv->off;
}

static void oc_object_set_ivar(id obj, void *iv, id value)
{ void *p = ivar_slot(obj, iv); if (p) memcpy(p, &value, sizeof value); }
static id oc_object_get_ivar(id obj, void *iv)
{ void *p = ivar_slot(obj, iv); id v = NULL; if (p) memcpy(&v, p, sizeof v); return v; }

/* The older, name-keyed pair. Both return the Ivar they found, as Apple's do. */
static void *oc_object_set_instance_variable(id obj, const char *name, void *value)
{
    synth_ivar *iv;
    void *p;
    if (!obj) return NULL;
    iv = oc_class_get_instance_variable(((obj_header *)obj)->isa, name);
    if ((p = ivar_slot(obj, iv))) memcpy(p, &value, sizeof value);
    return iv;
}
static void *oc_object_get_instance_variable(id obj, const char *name, void **out)
{
    synth_ivar *iv;
    void *p;
    if (out) *out = NULL;
    if (!obj) return NULL;
    iv = oc_class_get_instance_variable(((obj_header *)obj)->isa, name);
    if ((p = ivar_slot(obj, iv)) && out) memcpy(out, p, sizeof *out);
    return iv;
}

static size_t oc_class_get_instance_size(void *cls)
{
    class_ro_t *ro = (cls && known_class(cls)) ? class_ro(cls) : NULL;
    return ro ? ro->instanceSize : 0;
}

/* The class is already in the table and already dispatchable, so there is
 * nothing to finalise. Kept because the plugin calls it and a missing import is
 * what started this. */
static void oc_register_class_pair(void *cls)
{
    int i = synth_index(cls, NULL);
    if (i >= 0) OLOG("  [objc] registered %s (%u bytes, %d ivar(s))\n",
                     g_synth[i].name, g_synth[i].ro.instanceSize, g_synth[i].nivar);
}

/* Frees the slot for reuse. Apple's contract is that no instance of the class
 * may exist when this is called, which is what makes reuse safe -- and reuse is
 * what keeps a session that opens editors repeatedly from running the table
 * dry. */
static void oc_dispose_class_pair(void *cls)
{
    int i = synth_index(cls, NULL), k;
    if (i < 0) return;
    OLOG("  [objc] disposed %s\n", g_synth[i].name);
    for (k = 0; k < g_nclasses; k++)
        if (g_classes[k] == &g_synth[i].cls) {
            g_classes[k] = g_classes[--g_nclasses];
            break;
        }
    g_synth[i].used = 0;
    g_synth[i].name[0] = '\0';
}

/* ------------------------------------------------------------------- table */

/* _OBJC_CLASS_$_Foo binds to the class object; _OBJC_METACLASS_$_Foo to its
 * metaclass. Both are data symbols. Built at first lookup so the table does not
 * have to list 106 entries by hand. */
static const macshim_entry *objc_class_entry(const char *sym)
{
    static struct { char name[96]; void *addr; } cache[MAX_SYNTH * 2];
    static macshim_entry out;
    int meta = 0;
    const char *base;
    int i;

    if (!strncmp(sym, "_OBJC_CLASS_$_", 14))          base = sym + 14;
    else if (!strncmp(sym, "_OBJC_METACLASS_$_", 18)) { base = sym + 18; meta = 1; }
    else return NULL;

    for (i = 0; i < g_nsynth; i++) {
        if (strcmp(g_synth[i].name, base)) continue;
        out.name = sym;
        out.addr = meta ? (void *)&g_synth[i].meta : (void *)&g_synth[i].cls;
        return &out;
    }
    (void)cache;
    return NULL;
}

const macshim_entry macshim_objc[] = {
    { "_objc_msgSend",            (void *)(void (*)(void))macobjc_msgSend },
    { "_objc_msgSend_stret",      (void *)(void (*)(void))macobjc_msgSend_stret },
    /* Legacy fixup dispatch. These are the entry points an older image's
     * message_ref is initialised to; without them every message it sends goes to
     * whatever the unresolved import was bound to. */
    { "_objc_msgSend_fixup",       (void *)(void (*)(void))macobjc_msgSend_fixup },
    { "_objc_msgSend_stret_fixup", (void *)(void (*)(void))macobjc_msgSend_stret_fixup },
    { "_objc_msgSendSuper2_fixup", (void *)(void (*)(void))macobjc_msgSendSuper2_fixup },
    { "_objc_msgSendSuper2",      (void *)(void (*)(void))macobjc_msgSendSuper2 },
    { "_objc_msgSendSuper2_stret", (void *)(void (*)(void))macobjc_msgSendSuper2_stret },
    { "_objc_msgSendSuper",       (void *)(void (*)(void))macobjc_msgSendSuper },
    { "_objc_msgSendSuper_stret", (void *)(void (*)(void))macobjc_msgSendSuper_stret },
    { "_objc_retain",             oc_retain },
    { "_objc_release",            oc_release },
    { "_objc_autorelease",        oc_autorelease },
    { "_objc_retainAutorelease",  oc_retain_autorelease },
    { "_objc_retainAutoreleasedReturnValue", oc_retain_autoreleased_return },
    { "_objc_autoreleaseReturnValue",        oc_autorelease_return },
    { "_objc_unsafeClaimAutoreleasedReturnValue", oc_unsafe_claim_autoreleased_return },
    { "_objc_autoreleasePoolPush", oc_pool_push },
    { "_objc_autoreleasePoolPop",  oc_pool_pop },
    { "_objc_storeStrong",         oc_store_strong },
    { "_objc_alloc",               oc_alloc },
    { "_objc_alloc_init",          oc_alloc_init },
    { "_objc_enumerationMutation", oc_enumeration_mutation },
    { "___objc_personality_v0",    oc_personality },
    { "_sel_getUid",               oc_sel_get_uid },
    { "_sel_registerName",         oc_sel_get_uid },
    { "_sel_getName",              oc_sel_get_uid },
    { "_objc_lookUpClass",         oc_look_up_class },
    { "_objc_getClass",            oc_look_up_class },
    { "_objc_allocateClassPair",   oc_allocate_class_pair },
    { "_objc_registerClassPair",   oc_register_class_pair },
    { "_objc_disposeClassPair",    oc_dispose_class_pair },
    { "_class_addMethod",          oc_class_add_method },
    { "_class_addIvar",            oc_class_add_ivar },
    { "_class_getSuperclass",      oc_class_get_superclass },
    { "_class_getName",            oc_class_get_name },
    { "_class_getInstanceSize",    oc_class_get_instance_size },
    { "_class_getInstanceVariable", oc_class_get_instance_variable },
    { "_ivar_getName",             oc_ivar_get_name },
    { "_ivar_getOffset",           oc_ivar_get_offset },
    { "_object_setIvar",           oc_object_set_ivar },
    { "_object_getIvar",           oc_object_get_ivar },
    { "_object_setInstanceVariable", oc_object_set_instance_variable },
    { "_object_getInstanceVariable", oc_object_get_instance_variable },
    { "_object_getClass",          oc_object_get_class },
    { NULL, NULL }
};

void *macobjc_lookup_symbol(const char *sym)
{
    const macshim_entry *e;
    int i;
    for (i = 0; macshim_objc[i].name; i++)
        if (!strcmp(macshim_objc[i].name, sym)) return macshim_objc[i].addr;
    if ((e = objc_class_entry(sym))) return e->addr;
    return NULL;
}
