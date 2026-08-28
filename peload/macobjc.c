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
typedef struct { unsigned long magic; long refs; } obj_prefix;

/* The bookkeeping for `p`, or NULL if we did not allocate it. */
static obj_prefix *obj_pre(void *p)
{
    obj_prefix *pre;
    if (!p) return NULL;
    pre = (obj_prefix *)p - 1;
    return pre->magic == OBJ_MAGIC ? pre : NULL;
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
    if (!c || g_nclasses >= MAX_CLASSES) return;
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
void *macobjc_super_receiver(void *sp)
{ super2 *s = sp; return s ? s->receiver : NULL; }

/* --------------------------------------------------- NSObject and stand-ins */

/* Root behaviour, so alloc/init/retain/release work on anything. The methods
 * are ordinary C functions with (self, sel) leading, which is the calling
 * convention objc_msgSend hands them. */
static id ns_alloc(id self, SEL sel)
{
    objc_class *cls = (objc_class *)self;        /* a class message */
    class_ro_t *ro = class_ro(cls);
    /* Our own classes lay a struct over the instance, so never allocate less
     * than the two words those structs begin with. */
    size_t sz = ro && ro->instanceSize > 16 ? ro->instanceSize : 16;
    obj_prefix *pre = calloc(1, sizeof *pre + sz);
    obj_header *o;
    (void)sel;
    if (!pre) return NULL;
    pre->magic = OBJ_MAGIC;
    pre->refs = 1;
    o = (obj_header *)(pre + 1);
    o->isa = cls;
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
static signed char ns_responds(id self, SEL sel, SEL q)
{ (void)sel; return self && find_method(((obj_header *)self)->isa, q) ? 1 : 0; }
static signed char ns_no(id self, SEL sel)    { (void)self; (void)sel; return 0; }

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
    { "isKindOfClass:",     "c@:#",  (IMP)ns_no },
    { "conformsToProtocol:", "c@:@", (IMP)ns_no }
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

static struct {
    objc_class cls, meta;
    class_ro_t ro, mro;
    synth_ml   ml, mml;
    char name[64];
    int  used;
    /* Ivars added at runtime -- see objc_allocateClassPair below. Names are kept
     * even though nothing looks an offset up yet: the moment a plugin imports
     * object_getInstanceVariable this is what it would be answered from, and a
     * bump pointer alone could not answer it. */
    uint32_t ivar_next;
    int      nivar;
    struct { char name[32]; uint32_t off, size; } ivar[MAX_IVARS];
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
     * retain/release resolve for every instance and every class. */
    if (!super) {
        size_t k, n = sizeof g_nsobject_methods / sizeof *g_nsobject_methods;
        for (k = 0; k < n && k < MAX_METHODS; k++) {
            g_synth[i].ml.m[k] = g_nsobject_methods[k];
            g_synth[i].mml.m[k] = g_nsobject_methods[k];
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
    for (i = 0; i < g_nsynth; i++)
        if (g_synth[i].used && !strcmp(g_synth[i].name, name)) return &g_synth[i].cls;
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
