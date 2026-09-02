/* CoreFoundation, libSystem and AudioUnit shims. See macshim.h.
 *
 * CoreFoundation gets a small real object model rather than stubs, because the
 * corpus stores its parameter metadata in CF dictionaries and formats parameter
 * values through CFNumberFormatter -- returning nothing there means a plugin
 * with no parameter names, which looks like a loader bug.
 *
 * Everything is reference counted the way CF is, but with no toll-free bridging
 * and no plists: the objects only ever travel between the plugin and us.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "macshim.h"

/* ------------------------------------------------------------------- types */

typedef const void *CFTypeRef;
typedef unsigned long CFTypeID;
typedef long CFIndex;
typedef unsigned char Boolean;
typedef int32_t OSStatus;

enum {
    CF_STRING = 1, CF_ARRAY, CF_DICT, CF_DATA, CF_NUMBER, CF_FORMATTER, CF_LOCALE,
    /* A UUID carries its sixteen bytes inline, so it is not a CFData however
     * much it looks like one -- see cf_uuid_create. */
    CF_UUID,
    CF_BOOLEAN
};

typedef struct cfobj {
    uint32_t         magic;
    CFTypeID         type;
    _Atomic long     refs;
    /* See the registry below: every object this shim minted is on a list, so
     * "is this one of ours?" can be answered without reading through it. */
    struct cfobj    *reg_next;
} cfobj;
#define CFMAGIC 0x43466F62u          /* 'CFob' */

/* ---- which pointers are ours ------------------------------------------
 *
 * A shim is asked constantly whether some pointer is a CoreFoundation object,
 * and the obvious test -- read the magic word and compare -- reads through
 * whatever it is handed. That is fine until a plugin hands over something that
 * is not a pointer at all, which happens for a specific and recurring reason:
 * an unresolved *data* import. A function import binds to a stub that reports
 * itself when called, but a data import that nothing implements is simply left
 * holding the image's own link-time value, and nothing says so. The plugin
 * then puts that value in a dictionary, and the fault lands inside CFRetain.
 *
 * It has now happened twice, with kCFBooleanTrue and again with a constant
 * this host has not identified, so the answer is structural: a hash set of
 * every object minted here, keyed by address. The test costs a hash and a
 * short walk, and it cannot fault. */
#define CFREG_BUCKETS 1024
static cfobj *g_cfreg[CFREG_BUCKETS];
static pthread_mutex_t g_cfreg_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned cfreg_bucket(const void *p)
{ return (unsigned)((((uintptr_t)p) >> 4) * 2654435761u) % CFREG_BUCKETS; }

static void cfreg_add(cfobj *o)
{
    unsigned b = cfreg_bucket(o);
    pthread_mutex_lock(&g_cfreg_lock);
    o->reg_next = g_cfreg[b];
    g_cfreg[b] = o;
    pthread_mutex_unlock(&g_cfreg_lock);
}
static void cfreg_del(cfobj *o)
{
    unsigned b = cfreg_bucket(o);
    cfobj **pp;
    pthread_mutex_lock(&g_cfreg_lock);
    for (pp = &g_cfreg[b]; *pp; pp = &(*pp)->reg_next)
        if (*pp == o) { *pp = o->reg_next; break; }
    pthread_mutex_unlock(&g_cfreg_lock);
}
static int cfreg_has(const void *p)
{
    unsigned b;
    cfobj *o;
    int found = 0;
    if (!p) return 0;
    b = cfreg_bucket(p);
    pthread_mutex_lock(&g_cfreg_lock);
    for (o = g_cfreg[b]; o; o = o->reg_next) if (o == p) { found = 1; break; }
    pthread_mutex_unlock(&g_cfreg_lock);
    return found;
}

typedef struct { cfobj o; char *s; CFIndex len; } cfstring;
typedef struct { cfobj o; const void **v; CFIndex n, cap; } cfarray;
typedef struct { cfobj o; const void **k, **v; CFIndex n, cap; } cfdict;
typedef struct { cfobj o; uint8_t *b; CFIndex n, cap; } cfdata;
typedef struct { cfobj o; int kind; double d; long long i; } cfnumber;
typedef struct { cfobj o; int minfrac, maxfrac; } cfformatter;

/* A CFSTR("...") literal is not one of our objects: the compiler emits a static
 * struct whose first word points at ___CFConstantStringClassReference. Making
 * that symbol a sentinel we own is what lets the accessors below recognise one
 * and read the C string straight out of it. */
typedef struct {
    const void *isa;
    unsigned long flags;
    const char *cstr;
    unsigned long len;
} cfconststring;

static void *g_const_string_class = (void *)"CFConstantString";

/* A CFSTR("...") literal is not one of ours, so it cannot be on the registry --
 * and it is a real object, so it has to be recognised anyway.
 *
 * What makes reading it safe is where it lives: a literal is in the plugin's
 * own __DATA, and the loader knows exactly which addresses that covers. So the
 * test is a range check against the mapped image -- two comparisons, no
 * syscall, and stricter than asking the kernel whether the page is mapped,
 * which would also accept a stack address that happened to look like one. */
static int is_const_string(const void *p)
{
    const cfconststring *c = p;
    if (!p || !macshim_dyld_image_contains(p, sizeof *c)) return 0;
    return c->isa == (const void *)&g_const_string_class;
}
static cfobj *as_obj(const void *p)
{
    cfobj *o = (cfobj *)p;
    if (!cfreg_has(p)) return NULL;
    return (o->magic == CFMAGIC) ? o : NULL;
}

static void *obj_new(size_t sz, CFTypeID t)
{
    cfobj *o = calloc(1, sz);
    if (!o) return NULL;
    o->magic = CFMAGIC;
    o->type = t;
    atomic_store(&o->refs, 1);
    cfreg_add(o);
    return o;
}

/* --------------------------------------------------------------- lifetime */

static CFTypeRef cf_retain(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    if (o) { atomic_fetch_add(&o->refs, 1); return p; }
    /* CoreGraphics objects are CoreFoundation objects too, and some of them
     * have no typed retain at all. */
    macquartz_cf_retain((void *)p);
    return p;
}

static void cf_release(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    if (!o) { macquartz_cf_release((void *)p); return; }
    if (atomic_fetch_sub(&o->refs, 1) != 1) return;
    switch (o->type) {
    case CF_STRING: free(((cfstring *)o)->s); break;
    case CF_ARRAY:  free(((cfarray *)o)->v); break;
    case CF_DICT:   free(((cfdict *)o)->k); free(((cfdict *)o)->v); break;
    case CF_DATA:   free(((cfdata *)o)->b); break;
    default: break;
    }
    cfreg_del(o);
    free(o);
}

static CFTypeID cf_gettypeid(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    if (o) return o->type;
    return is_const_string(p) ? CF_STRING : 0;
}
static CFTypeID cf_dict_typeid(void) { return CF_DICT; }

/* ---------------------------------------------------------------- strings */

static const char *str_of(const void *p, CFIndex *len)
{
    cfobj *o = as_obj(p);
    if (o && o->type == CF_STRING) {
        if (len) *len = ((cfstring *)o)->len;
        return ((cfstring *)o)->s;
    }
    if (is_const_string(p)) {
        const cfconststring *c = p;
        if (len) *len = (CFIndex)c->len;
        return c->cstr;
    }
    if (len) *len = 0;
    return NULL;
}

static CFTypeRef cf_string_create(void *alloc, const char *cstr, uint32_t enc)
{
    cfstring *s;
    (void)alloc; (void)enc;
    if (!cstr) return NULL;
    if (!(s = obj_new(sizeof *s, CF_STRING))) return NULL;
    s->len = (CFIndex)strlen(cstr);
    s->s = malloc((size_t)s->len + 1);
    if (!s->s) { free(s); return NULL; }
    memcpy(s->s, cstr, (size_t)s->len + 1);
    return s;
}

static Boolean cf_string_getcstring(CFTypeRef p, char *buf, CFIndex n, uint32_t enc)
{
    CFIndex len = 0;
    const char *s = str_of(p, &len);
    (void)enc;
    if (!buf || n <= 0) return 0;
    if (!s) { buf[0] = 0; return 0; }
    if (len >= n) { buf[0] = 0; return 0; }      /* CF fails rather than truncates */
    memcpy(buf, s, (size_t)len);
    buf[len] = 0;
    return 1;
}

/* ----------------------------------------------------------------- arrays */

static CFTypeRef cf_array_create(void *alloc, const void **vals, CFIndex n,
                                 const void *cb)
{
    cfarray *a;
    CFIndex i;
    (void)alloc; (void)cb;
    if (!(a = obj_new(sizeof *a, CF_ARRAY))) return NULL;
    a->cap = a->n = n > 0 ? n : 0;
    if (a->cap) {
        a->v = calloc((size_t)a->cap, sizeof *a->v);
        if (!a->v) { free(a); return NULL; }
        for (i = 0; i < a->n; i++) { a->v[i] = vals[i]; cf_retain(vals[i]); }
    }
    return a;
}
static CFIndex cf_array_count(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    return (o && o->type == CF_ARRAY) ? ((cfarray *)o)->n : 0;
}
static const void *cf_array_value(CFTypeRef p, CFIndex i)
{
    cfobj *o = as_obj(p);
    cfarray *a = (cfarray *)o;
    if (!o || o->type != CF_ARRAY || i < 0 || i >= a->n) return NULL;
    return a->v[i];
}

/* ----------------------------------------------------------- dictionaries */

/* Keys compare by string value when both sides are strings, and by pointer
 * otherwise -- which is what CFEqual does for the types in play here. */
static int key_eq(const void *a, const void *b)
{
    CFIndex la = 0, lb = 0;
    const char *sa, *sb;
    if (a == b) return 1;
    sa = str_of(a, &la); sb = str_of(b, &lb);
    if (sa && sb) return la == lb && !memcmp(sa, sb, (size_t)la);
    return 0;
}

static CFTypeRef cf_dict_create_mutable(void *alloc, CFIndex cap,
                                        const void *kcb, const void *vcb)
{
    cfdict *d;
    (void)alloc; (void)kcb; (void)vcb;
    if (!(d = obj_new(sizeof *d, CF_DICT))) return NULL;
    d->cap = cap > 0 ? cap : 8;
    d->k = calloc((size_t)d->cap, sizeof *d->k);
    d->v = calloc((size_t)d->cap, sizeof *d->v);
    if (!d->k || !d->v) { free(d->k); free(d->v); free(d); return NULL; }
    return d;
}

static CFIndex dict_find(cfdict *d, const void *key)
{
    CFIndex i;
    for (i = 0; i < d->n; i++) if (key_eq(d->k[i], key)) return i;
    return -1;
}

static void cf_dict_set(CFTypeRef p, const void *key, const void *val)
{
    cfobj *o = as_obj(p);
    cfdict *d = (cfdict *)o;
    CFIndex at;
    if (!o || o->type != CF_DICT) return;
    if ((at = dict_find(d, key)) >= 0) {
        cf_release(d->v[at]);
        d->v[at] = val;
        cf_retain(val);
        return;
    }
    if (d->n == d->cap) {
        CFIndex nc = d->cap * 2;
        const void **nk = realloc(d->k, (size_t)nc * sizeof *nk);
        const void **nv = realloc(d->v, (size_t)nc * sizeof *nv);
        if (!nk || !nv) { d->k = nk ? nk : d->k; d->v = nv ? nv : d->v; return; }
        d->k = nk; d->v = nv; d->cap = nc;
    }
    d->k[d->n] = key; cf_retain(key);
    d->v[d->n] = val; cf_retain(val);
    d->n++;
}

static const void *cf_dict_get(CFTypeRef p, const void *key)
{
    cfobj *o = as_obj(p);
    cfdict *d = (cfdict *)o;
    CFIndex at;
    if (!o || o->type != CF_DICT) return NULL;
    return (at = dict_find(d, key)) >= 0 ? d->v[at] : NULL;
}
static Boolean cf_dict_get_if(CFTypeRef p, const void *key, const void **out)
{
    const void *v = cf_dict_get(p, key);
    if (out) *out = v;
    return v != NULL;
}
static Boolean cf_dict_contains(CFTypeRef p, const void *key)
{ return cf_dict_get(p, key) != NULL; }
static CFIndex cf_dict_count(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    return (o && o->type == CF_DICT) ? ((cfdict *)o)->n : 0;
}
static void cf_dict_keys_values(CFTypeRef p, const void **keys, const void **vals)
{
    cfobj *o = as_obj(p);
    cfdict *d = (cfdict *)o;
    CFIndex i;
    if (!o || o->type != CF_DICT) return;
    for (i = 0; i < d->n; i++) {
        if (keys) keys[i] = d->k[i];
        if (vals) vals[i] = d->v[i];
    }
}

/* ------------------------------------------------------------------- data */

static CFTypeRef cf_data_create_mutable(void *alloc, CFIndex cap)
{
    cfdata *d;
    (void)alloc;
    if (!(d = obj_new(sizeof *d, CF_DATA))) return NULL;
    d->cap = cap > 0 ? cap : 64;
    d->b = malloc((size_t)d->cap);
    if (!d->b) { free(d); return NULL; }
    return d;
}
static void cf_data_append(CFTypeRef p, const uint8_t *bytes, CFIndex n)
{
    cfobj *o = as_obj(p);
    cfdata *d = (cfdata *)o;
    if (!o || o->type != CF_DATA || !bytes || n <= 0) return;
    if (d->n + n > d->cap) {
        CFIndex nc = (d->n + n) * 2;
        uint8_t *nb = realloc(d->b, (size_t)nc);
        if (!nb) return;
        d->b = nb; d->cap = nc;
    }
    memcpy(d->b + d->n, bytes, (size_t)n);
    d->n += n;
}
static const uint8_t *cf_data_ptr(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    return (o && o->type == CF_DATA) ? ((cfdata *)o)->b : NULL;
}
static CFIndex cf_data_len(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    return (o && o->type == CF_DATA) ? ((cfdata *)o)->n : 0;
}

/* ---------------------------------------------------------------- numbers */

/* CFNumberType values that matter here. */
enum { kCFNumberSInt32Type = 3, kCFNumberSInt64Type = 4,
       kCFNumberFloat32Type = 5, kCFNumberFloat64Type = 6,
       kCFNumberIntType = 9, kCFNumberDoubleType = 13 };

static CFTypeRef cf_number_create(void *alloc, int32_t type, const void *valp)
{
    cfnumber *n;
    (void)alloc;
    if (!valp) return NULL;
    if (!(n = obj_new(sizeof *n, CF_NUMBER))) return NULL;
    n->kind = type;
    switch (type) {
    case kCFNumberSInt32Type: case kCFNumberIntType:
        n->i = *(const int32_t *)valp; n->d = (double)n->i; break;
    case kCFNumberSInt64Type:
        n->i = *(const int64_t *)valp; n->d = (double)n->i; break;
    case kCFNumberFloat32Type:
        n->d = *(const float *)valp; n->i = (long long)n->d; break;
    default:
        n->d = *(const double *)valp; n->i = (long long)n->d; break;
    }
    return n;
}

/* ---- CFBoolean ---------------------------------------------------------
 *
 * Two singletons, and they have to be real objects rather than sentinels: a
 * plugin puts kCFBooleanTrue into a dictionary, and the dictionary retains what
 * it is given. Unbound, the slot still held the image's own link-time value,
 * which CFRetain then dereferenced -- a fault inside a shim that was only asked
 * to store something. */
typedef struct { cfobj o; int v; } cfboolean;
static cfboolean g_true  = { { CFMAGIC, CF_BOOLEAN, 2, NULL }, 1 };
static cfboolean g_false = { { CFMAGIC, CF_BOOLEAN, 2, NULL }, 0 };
/* Statically built, so they miss obj_new and have to join the registry by
 * hand -- otherwise CFBooleanGetValue would not recognise its own singletons. */
static void __attribute__((constructor)) cf_boolean_register(void)
{ cfreg_add(&g_true.o); cfreg_add(&g_false.o); }
static void *g_k_boolean_true  = &g_true;
static void *g_k_boolean_false = &g_false;

static Boolean cf_boolean_value(CFTypeRef p)
{ cfobj *o = as_obj(p); return (o && o->type == CF_BOOLEAN) ? (Boolean)((cfboolean *)o)->v : 0; }
static CFTypeID cf_boolean_typeid(void) { return CF_BOOLEAN; }

/* A CFNumber from a plain integer, for the shims that build dictionaries of
 * their own -- ImageIO's image properties are the case here. */
void *macshim_cf_number_int(long v)
{
    int32_t i = (int32_t)v;
    return (void *)cf_number_create(NULL, kCFNumberSInt32Type, &i);
}

static Boolean cf_number_get(CFTypeRef p, int32_t type, void *out)
{
    cfobj *o = as_obj(p);
    cfnumber *n = (cfnumber *)o;
    if (!o || o->type != CF_NUMBER || !out) return 0;
    switch (type) {
    case kCFNumberSInt32Type: case kCFNumberIntType:
        *(int32_t *)out = (int32_t)n->i; break;
    case kCFNumberSInt64Type: *(int64_t *)out = n->i; break;
    case kCFNumberFloat32Type: *(float *)out = (float)n->d; break;
    default: *(double *)out = n->d; break;
    }
    return 1;
}

/* ------------------------------------------------------------- formatter */

/* Used to turn a parameter value into the text a host displays, so the
 * fraction-digit properties are honoured and the rest ignored. */
static const char *k_maxfrac = "kCFNumberFormatterMaxFractionDigits";
static const char *k_minfrac = "kCFNumberFormatterMinFractionDigits";

static CFTypeRef cf_formatter_create(void *alloc, void *locale, int style)
{
    cfformatter *f;
    (void)alloc; (void)locale; (void)style;
    if (!(f = obj_new(sizeof *f, CF_FORMATTER))) return NULL;
    f->maxfrac = 3;
    f->minfrac = 0;
    return f;
}

static void cf_formatter_set(CFTypeRef p, const void *key, const void *val)
{
    cfobj *o = as_obj(p);
    cfformatter *f = (cfformatter *)o;
    CFIndex kl = 0;
    const char *k = str_of(key, &kl);
    int32_t v = 0;
    if (!o || o->type != CF_FORMATTER || !k) return;
    cf_number_get(val, kCFNumberSInt32Type, &v);
    if (!strcmp(k, k_maxfrac)) f->maxfrac = v;
    else if (!strcmp(k, k_minfrac)) f->minfrac = v;
}

static CFTypeRef cf_formatter_string(void *alloc, CFTypeRef fmt,
                                     int32_t type, const void *valp)
{
    cfobj *o = as_obj(fmt);
    cfformatter *f = (cfformatter *)o;
    char buf[64];
    double d = 0.0;
    int digits = 3;

    (void)alloc;
    if (!valp) return NULL;
    switch (type) {
    case kCFNumberSInt32Type: case kCFNumberIntType: d = *(const int32_t *)valp; break;
    case kCFNumberSInt64Type: d = (double)*(const int64_t *)valp; break;
    case kCFNumberFloat32Type: d = *(const float *)valp; break;
    default: d = *(const double *)valp; break;
    }
    if (o && o->type == CF_FORMATTER) digits = f->maxfrac;
    if (digits < 0) digits = 0;
    if (digits > 15) digits = 15;
    snprintf(buf, sizeof buf, "%.*f", digits, d);
    /* Trim to the minimum fraction digits the caller asked for. */
    if (o && o->type == CF_FORMATTER && f->maxfrac > f->minfrac && strchr(buf, '.')) {
        char *end = buf + strlen(buf) - 1;
        int frac = (int)(strlen(strchr(buf, '.') + 1));
        while (*end == '0' && frac > f->minfrac) { *end-- = 0; frac--; }
        if (*end == '.') *end = 0;
    }
    return cf_string_create(NULL, buf, 0);
}

static Boolean cf_formatter_value(CFTypeRef fmt, CFTypeRef str, int32_t type,
                                  void *out)
{
    CFIndex len = 0;
    const char *s = str_of(str, &len);
    double d;
    (void)fmt;
    if (!s || !out) return 0;
    d = strtod(s, NULL);
    switch (type) {
    case kCFNumberSInt32Type: case kCFNumberIntType: *(int32_t *)out = (int32_t)d; break;
    case kCFNumberSInt64Type: *(int64_t *)out = (long long)d; break;
    case kCFNumberFloat32Type: *(float *)out = (float)d; break;
    default: *(double *)out = d; break;
    }
    return 1;
}

static CFTypeRef cf_locale_copy_current(void) { return obj_new(sizeof(cfobj), CF_LOCALE); }

/* CF callback structs. Only their addresses are ever passed back to us. */
static const char g_dict_key_cb[64];
static const char g_dict_val_cb[64];

/* CFStringRef constants, as our own string objects so str_of() reads them. */
static cfstring g_k_maxfrac = { { CFMAGIC, CF_STRING, 1, NULL }, (char *)0, 0 };
static cfstring g_k_minfrac = { { CFMAGIC, CF_STRING, 1, NULL }, (char *)0, 0 };
static void __attribute__((constructor)) init_cf_constants(void)
{
    g_k_maxfrac.s = (char *)k_maxfrac; g_k_maxfrac.len = (CFIndex)strlen(k_maxfrac);
    g_k_minfrac.s = (char *)k_minfrac; g_k_minfrac.len = (CFIndex)strlen(k_minfrac);
    /* Built statically, so they missed obj_new and have to join the registry by
     * hand -- an object the registry has not seen is not recognised as one, and
     * these are handed to str_of like any other string. */
    cfreg_add(&g_k_maxfrac.o);
    cfreg_add(&g_k_minfrac.o);
}
static void *g_p_maxfrac = &g_k_maxfrac;
static void *g_p_minfrac = &g_k_minfrac;

const macshim_entry macshim_corefoundation[] = {
    { "_CFRetain",  cf_retain },
    { "_CFRelease", cf_release },
    { "_CFGetTypeID", cf_gettypeid },
    { "_CFStringCreateWithCString", cf_string_create },
    { "_CFStringGetCString",        cf_string_getcstring },
    { "_CFArrayCreate",             cf_array_create },
    { "_CFArrayGetCount",           cf_array_count },
    { "_CFArrayGetValueAtIndex",    cf_array_value },
    { "_CFDictionaryCreateMutable", cf_dict_create_mutable },
    { "_CFDictionarySetValue",      cf_dict_set },
    { "_CFDictionaryGetValue",      cf_dict_get },
    { "_CFDictionaryGetValueIfPresent", cf_dict_get_if },
    { "_CFDictionaryContainsKey",   cf_dict_contains },
    { "_CFDictionaryGetCount",      cf_dict_count },
    { "_CFDictionaryGetKeysAndValues", cf_dict_keys_values },
    { "_CFDictionaryGetTypeID",     cf_dict_typeid },
    { "_CFDataCreateMutable",       cf_data_create_mutable },
    { "_CFDataAppendBytes",         cf_data_append },
    { "_CFDataGetBytePtr",          cf_data_ptr },
    { "_CFDataGetLength",           cf_data_len },
    { "_CFNumberCreate",            cf_number_create },
    { "_kCFBooleanTrue",            &g_k_boolean_true },
    { "_kCFBooleanFalse",           &g_k_boolean_false },
    { "_CFBooleanGetValue",         cf_boolean_value },
    { "_CFBooleanGetTypeID",        cf_boolean_typeid },
    { "_CFNumberGetValue",          cf_number_get },
    { "_CFNumberFormatterCreate",   cf_formatter_create },
    { "_CFNumberFormatterSetProperty", cf_formatter_set },
    { "_CFNumberFormatterCreateStringWithValue", cf_formatter_string },
    { "_CFNumberFormatterGetValueFromString",    cf_formatter_value },
    { "_CFLocaleCopyCurrent",       cf_locale_copy_current },
    /* data symbols */
    { "_kCFTypeDictionaryKeyCallBacks",   (void *)g_dict_key_cb },
    { "_kCFTypeDictionaryValueCallBacks", (void *)g_dict_val_cb },
    { "_kCFNumberFormatterMaxFractionDigits", &g_p_maxfrac },
    { "_kCFNumberFormatterMinFractionDigits", &g_p_minfrac },
    { "___CFConstantStringClassReference",     &g_const_string_class },
    { NULL, NULL }
};

/* ------------------------------------------------------------- libSystem */

/* sincos returning both results in registers. On x86-64 a two-double struct
 * comes back in xmm0/xmm1, which is exactly Apple's calling convention for
 * these, so the plain C form is correct. */
typedef struct { double s, c; } sincos_d;
typedef struct { float  s, c; } sincos_f;
static sincos_d ls_sincos_stret(double x)
{ sincos_d r; r.s = sin(x); r.c = cos(x); return r; }
static sincos_f ls_sincosf_stret(float x)
{ sincos_f r; r.s = sinf(x); r.c = cosf(x); return r; }

static void ls_memset_pattern16(void *dst, const void *pat, size_t n)
{
    uint8_t *d = dst;
    size_t i;
    if (!d || !pat) return;
    for (i = 0; i < n; i++) d[i] = ((const uint8_t *)pat)[i & 15];
}

static void *ls_reallocf(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n) free(p);                 /* the whole point of reallocf */
    return q;
}

static int ls_sysctlbyname(const char *name, void *out, size_t *len,
                           const void *nv, size_t nl)
{
    (void)nv; (void)nl;
    /* Only the CPU-count queries matter, and only for choosing a work-group
     * size; anything else reports "no such name" rather than inventing data. */
    if (name && out && len && *len >= sizeof(int) &&
        (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.logicalcpu") ||
         !strcmp(name, "hw.physicalcpu") || !strcmp(name, "hw.activecpu"))) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        *(int *)out = (int)(n > 0 ? n : 1);
        *len = sizeof(int);
        return 0;
    }
    if (len) *len = 0;
    return -1;
}

static int ls_cas64_barrier(int64_t oldv, int64_t newv, volatile int64_t *p)
{ return __atomic_compare_exchange_n(p, &oldv, newv, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
static void ls_memory_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static void ls_dispatch_once_f(long *pred, void *ctx, void (*fn)(void *))
{
    /* dispatch_once_t is a long that dyld guarantees starts at zero. */
    static _Atomic int lock;
    if (!pred || !fn) return;
    if (__atomic_load_n(pred, __ATOMIC_ACQUIRE) == ~0L) return;
    while (atomic_exchange(&lock, 1)) { }
    if (__atomic_load_n(pred, __ATOMIC_RELAXED) != ~0L) {
        fn(ctx);
        __atomic_store_n(pred, ~0L, __ATOMIC_RELEASE);
    }
    atomic_store(&lock, 0);
}

/* BSD ctype internals. A plugin reaching __maskrune is doing isalpha() on a
 * char; the ASCII answer is the right one for parameter-name parsing. */
static int ls_maskrune(int c, unsigned long f)
{
    unsigned long m = 0;
    if (c >= 'A' && c <= 'Z') m |= 0x00000100ul | 0x00008000ul;  /* upper|alpha */
    if (c >= 'a' && c <= 'z') m |= 0x00000200ul | 0x00008000ul;  /* lower|alpha */
    if (c >= '0' && c <= '9') m |= 0x00000400ul;                 /* digit */
    if (c == ' ' || c == '\t') m |= 0x00000040ul;                /* blank */
    return (int)(m & f);
}
/* _DefaultRuneLocale is a large struct the plugin only passes around. */
static uint8_t g_default_rune_locale[4096];

static uintptr_t g_stack_chk_guard = 0x00000aff0d0a0000ul;
static double    g_pi = 3.14159265358979323846;

/* Lazy binds are all resolved at load time, so nothing should ever route
 * through the stub binder. Say so loudly rather than jumping to zero. */
static void ls_stub_binder(void)
{
    fprintf(stderr, "macho: dyld_stub_binder was called -- a lazy bind was "
                    "missed at load time\n");
    abort();
}

/* __stderrp is a data symbol: a FILE* the plugin dereferences, not a function.
 * Apple exports the pointer itself, so the bind target must be the slot. */
static FILE *g_stderr_slot;
static FILE *g_stdout_slot;
static void __attribute__((constructor)) init_stdio_slots(void)
{ g_stderr_slot = stderr; g_stdout_slot = stdout; }

/* libc++ throw helpers that libstdc++ has no equivalent of. A plugin only
 * reaches these when a container allocation has already failed, so reporting and
 * aborting is more useful than unwinding into a plugin that cannot cope. */
static void lcpp_throw_length_error(void *self)
{
    (void)self;
    fprintf(stderr, "macho: plugin hit std::length_error (container overflow)\n");
    abort();
}
static void *lcpp_bad_alloc_ctor(void *self) { return self; }

/* RTTI for Apple's CoreAudio SDK exception classes, which live in AudioToolbox
 * on macOS. The Itanium ABI makes these buildable: __ZTS<n><name> is just the
 * mangled name as a C string, and __ZTI<n><name> is a two-word object -- the
 * vptr of __cxxabiv1::__class_type_info followed by a pointer to that string.
 * Borrowing the vptr out of libstdc++'s own vtable is what makes the result a
 * type the host runtime will actually match on, so a plugin that throws
 * CAException gets caught by its own handler instead of terminating.
 *
 * A vtable's first two words are the offset-to-top and the typeinfo pointer, so
 * the vptr a class stores is the vtable address plus two. */
typedef struct { const void *vptr; const char *name; } fake_typeinfo;

static const char g_zts_ca[]  = "11CAException";
static const char g_zts_cax[] = "12CAXException";
static const char g_zts_auk[] = "12AUKernelBase";
/* Template instantiations, so absent from the shared library however complete it
 * is -- they exist only in whichever object instantiated them. */
static const char g_zts_fnb[] = "NSt3__110__function6__baseIFfiEEE";
static const char g_zts_vbc[] = "NSt3__120__vector_base_commonILb1EEE";
static const char g_zts_cvt[] = "NSt3__118codecvt_utf8_utf16IDsLm1114111ELNS_12codecvt_modeE0EEE";
static fake_typeinfo g_zti_ca, g_zti_cax, g_zti_auk, g_zti_fnb, g_zti_cvt, g_zti_vbc;

static void __attribute__((constructor)) init_fake_rtti(void)
{
    void *h = dlopen("libstdc++.so.6", RTLD_LAZY | RTLD_GLOBAL);
    void **vt = h ? dlsym(h, "_ZTVN10__cxxabiv117__class_type_infoE") : NULL;
    const void *vptr = vt ? (const void *)(vt + 2) : NULL;
    g_zti_ca.vptr  = vptr; g_zti_ca.name  = g_zts_ca;
    g_zti_cax.vptr = vptr; g_zti_cax.name = g_zts_cax;
    g_zti_auk.vptr = vptr; g_zti_auk.name = g_zts_auk;
    g_zti_fnb.vptr = vptr; g_zti_fnb.name = g_zts_fnb;
    g_zti_cvt.vptr = vptr; g_zti_cvt.name = g_zts_cvt;
    g_zti_vbc.vptr = vptr; g_zti_vbc.name = g_zts_vbc;
}

/* -------------------------------------------------------------- atexit */

/* A plugin's static destructors are registered through __cxa_atexit. Forwarding
 * that to the host's puts them in the *host's* exit list, pointing into the
 * plugin image -- so unmapping the image leaves the process with a list of calls
 * into freed memory, and it dies at exit rather than at the point of the mistake.
 *
 * They are recorded per image instead, and run when that image is closed. That is
 * what dlclose does for a shared object, and it is what dyld does for a bundle.
 */
#define MAX_ATEXIT 512
static struct { void (*fn)(void *); void *arg; const void *image; } g_atexit[MAX_ATEXIT];
static int g_natexit;
static const void *g_cur_image;

void macshim_set_image(const void *token) { g_cur_image = token; }

static int sh_cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (!fn) return 0;
    if (g_natexit >= MAX_ATEXIT) return -1;
    g_atexit[g_natexit].fn = fn;
    g_atexit[g_natexit].arg = arg;
    g_atexit[g_natexit].image = g_cur_image;
    g_natexit++;
    return 0;
}
static int sh_atexit(void (*fn)(void))
{ return sh_cxa_atexit((void (*)(void *))fn, NULL, NULL); }

void macshim_run_atexit(const void *token)
{
    int i;
    /* Reverse order, as a destructor list requires, and drop each entry before
     * calling it so a handler that re-enters cannot run twice. */
    for (i = g_natexit - 1; i >= 0; i--) {
        void (*fn)(void *);
        void *arg;
        if (g_atexit[i].image != token) continue;
        fn = g_atexit[i].fn;
        arg = g_atexit[i].arg;
        g_atexit[i].fn = NULL;
        g_atexit[i].image = NULL;
        if (fn) fn(arg);
    }
    /* Compact, so a later image does not inherit the holes. */
    { int w = 0;
      for (i = 0; i < g_natexit; i++) if (g_atexit[i].fn) g_atexit[w++] = g_atexit[i];
      g_natexit = w; }
}

const macshim_entry macshim_libsystem[] = {
    { "___sincos_stret",  ls_sincos_stret },
    { "___sincosf_stret", ls_sincosf_stret },
    { "_memset_pattern16", ls_memset_pattern16 },
    { "_reallocf",         ls_reallocf },
    { "_sysctlbyname",     ls_sysctlbyname },
    { "_OSAtomicCompareAndSwap64Barrier", ls_cas64_barrier },
    { "_OSMemoryBarrier",  ls_memory_barrier },
    { "_dispatch_once_f",  ls_dispatch_once_f },
    { "___maskrune",       ls_maskrune },
    { "dyld_stub_binder",  ls_stub_binder },
    { "___cxa_atexit",     sh_cxa_atexit },
    { "_atexit",           sh_atexit },
    { "__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv",
                             lcpp_throw_length_error },
    { "__ZNSt9bad_allocC1Ev", lcpp_bad_alloc_ctor },
    /* data */
    { "__DefaultRuneLocale", g_default_rune_locale },
    { "___stderrp",          &g_stderr_slot },
    { "__ZTS11CAException",  (void *)g_zts_ca },
    { "__ZTS12CAXException", (void *)g_zts_cax },
    { "__ZTS12AUKernelBase", (void *)g_zts_auk },
    { "__ZTI11CAException",  &g_zti_ca },
    { "__ZTI12CAXException", &g_zti_cax },
    { "__ZTI12AUKernelBase", &g_zti_auk },
    { "__ZTSNSt3__110__function6__baseIFfiEEE", (void *)g_zts_fnb },
    { "__ZTINSt3__110__function6__baseIFfiEEE", &g_zti_fnb },
    { "__ZTSNSt3__120__vector_base_commonILb1EEE", (void *)g_zts_vbc },
    { "__ZTINSt3__120__vector_base_commonILb1EEE", &g_zti_vbc },
    { "__ZTSNSt3__118codecvt_utf8_utf16IDsLm1114111ELNS_12codecvt_modeE0EEE", (void *)g_zts_cvt },
    { "__ZTINSt3__118codecvt_utf8_utf16IDsLm1114111ELNS_12codecvt_modeE0EEE", &g_zti_cvt },
    { "___stdoutp",          &g_stdout_slot },
    { "___stack_chk_guard",  &g_stack_chk_guard },
    { "_pi",                 &g_pi },
    { NULL, NULL }
};

/* Small public surface so macfound.c can build CF objects that interoperate
 * with the ones here -- same header, so CFRetain/CFRelease accept both. */
/* Is this one of our CoreFoundation objects?
 *
 * Needed because CF and Objective-C objects are interchangeable on macOS -- a
 * CFStringRef can be sent -release, and WDL's compatibility layer does exactly
 * that. Without this the runtime read the CF magic word as an isa and walked it
 * as a class. */
int macshim_cf_is_object(const void *p)
{
    const cfobj *o = p;
    return o && o->magic == CFMAGIC;
}

/* retain/release for a CF object reached through an Objective-C message. */
const void *macshim_cf_retain_obj(const void *p)  { return cf_retain(p); }
void        macshim_cf_release_obj(const void *p) { cf_release(p); }

void *macshim_cf_string(const char *s) { return (void *)cf_string_create(NULL, s, 0); }
void *macshim_cf_dict_create_mutable_pub(long cap)
{ return (void *)cf_dict_create_mutable(NULL, (CFIndex)cap, NULL, NULL); }
void  macshim_cf_dict_set_pub(void *d, const void *k, const void *v)
{ cf_dict_set(d, k, v); }
const void *macshim_lookup_retain(void *p) { return cf_retain(p); }
int macshim_cf_string_get(void *p, char *buf, long n)
{ return cf_string_getcstring(p, buf, (CFIndex)n, 0); }
void *macshim_cf_array(const void **vals, long n)
{ return (void *)cf_array_create(NULL, vals, (CFIndex)n, NULL); }
void *macshim_cf_data(const void *bytes, size_t n)
{
    void *d = (void *)cf_data_create_mutable(NULL, (CFIndex)n + 1);
    if (d) cf_data_append(d, bytes, (CFIndex)n);
    return d;
}

/* ----------------------------------------------------------------- pthread */

/* Apple's pthread types cannot be handed to glibc. Their sizes differ (a
 * pthread_mutex_t is 64 bytes on macOS, 40 on glibc) and, more decisively, a
 * statically initialised Apple mutex is not zero: PTHREAD_MUTEX_INITIALIZER
 * writes a signature word. glibc reads that as a mutex which is already held,
 * so the first lock deadlocks -- which is exactly how this surfaced, spinning
 * inside pthread_mutex_lock during the plugin's Open.
 *
 * So the plugin's storage is treated as ours: a magic word plus a pointer to a
 * real glibc object, created on first use. The plugin only ever passes these
 * back to us, so it never sees the difference.
 */
#define PT_MAGIC 0x50544D58754A6C6Bull      /* arbitrary, just not a valid sig */

typedef struct { uint64_t magic; void *real; } pt_slot;

/* A lock guarding lazy creation. Contention here is only between threads
 * initialising the *same* object for the first time. */
static pthread_mutex_t g_pt_gate = PTHREAD_MUTEX_INITIALIZER;

static void *pt_get(void *opaque, size_t sz, void (*init)(void *))
{
    pt_slot *s = opaque;
    if (!s) return NULL;
    if (s->magic == PT_MAGIC && s->real) return s->real;
    pthread_mutex_lock(&g_pt_gate);
    if (!(s->magic == PT_MAGIC && s->real)) {
        void *r = calloc(1, sz);
        if (r) {
            if (init) init(r);
            s->magic = PT_MAGIC;
            s->real = r;
        }
    }
    pthread_mutex_unlock(&g_pt_gate);
    return s->real;
}

static void pt_mutex_default(void *m) { pthread_mutex_init(m, NULL); }

/* Apple: NORMAL 0, ERRORCHECK 1, RECURSIVE 2.
 * glibc: TIMED 0, RECURSIVE 1, ERRORCHECK 2.
 * The two swap 1 and 2, so passing the value through would silently turn a
 * recursive mutex into an error-checking one and deadlock on re-entry. */
static int pt_type_from_apple(int t)
{
    switch (t) {
    case 1:  return PTHREAD_MUTEX_ERRORCHECK;
    case 2:  return PTHREAD_MUTEX_RECURSIVE;
    default: return PTHREAD_MUTEX_NORMAL;
    }
}

typedef struct { uint64_t magic; int type; } pt_attr;

static int pt_mutexattr_init(void *a)
{
    pt_attr *p = a;
    if (!p) return 22;
    p->magic = PT_MAGIC;
    p->type = 0;
    return 0;
}
static int pt_mutexattr_destroy(void *a) { (void)a; return 0; }
static int pt_mutexattr_settype(void *a, int type)
{
    pt_attr *p = a;
    if (!p) return 22;
    if (p->magic != PT_MAGIC) pt_mutexattr_init(p);
    p->type = type;
    return 0;
}

static int pt_mutex_init(void *m, const void *attr)
{
    pt_slot *s = m;
    const pt_attr *a = attr;
    pthread_mutex_t *real;
    pthread_mutexattr_t ma;
    if (!s) return 22;
    real = calloc(1, sizeof *real);
    if (!real) return 12;
    pthread_mutexattr_init(&ma);
    if (a && a->magic == PT_MAGIC)
        pthread_mutexattr_settype(&ma, pt_type_from_apple(a->type));
    pthread_mutex_init(real, &ma);
    pthread_mutexattr_destroy(&ma);
    /* Replace rather than leak if the plugin re-inits. */
    if (s->magic == PT_MAGIC && s->real) {
        pthread_mutex_destroy(s->real);
        free(s->real);
    }
    s->magic = PT_MAGIC;
    s->real = real;
    return 0;
}

static int pt_mutex_destroy(void *m)
{
    pt_slot *s = m;
    if (!s || s->magic != PT_MAGIC || !s->real) return 0;
    pthread_mutex_destroy(s->real);
    free(s->real);
    s->real = NULL;
    s->magic = 0;
    return 0;
}

static int pt_mutex_lock(void *m)
{
    void *r = pt_get(m, sizeof(pthread_mutex_t), pt_mutex_default);
    return r ? pthread_mutex_lock(r) : 22;
}
static int pt_mutex_unlock(void *m)
{
    void *r = pt_get(m, sizeof(pthread_mutex_t), pt_mutex_default);
    return r ? pthread_mutex_unlock(r) : 22;
}
static int pt_mutex_trylock(void *m)
{
    void *r = pt_get(m, sizeof(pthread_mutex_t), pt_mutex_default);
    return r ? pthread_mutex_trylock(r) : 22;
}

/* pthread_once_t is likewise non-zero when statically initialised. */
typedef struct { uint64_t magic; int done; } pt_once_slot;
static int pt_once(void *o, void (*fn)(void))
{
    pt_once_slot *p = o;
    if (!p || !fn) return 22;
    pthread_mutex_lock(&g_pt_gate);
    if (p->magic != PT_MAGIC || !p->done) {
        p->magic = PT_MAGIC;
        p->done = 1;
        pthread_mutex_unlock(&g_pt_gate);
        fn();
        return 0;
    }
    pthread_mutex_unlock(&g_pt_gate);
    return 0;
}

/* pthread_t is an opaque pointer-sized handle on both, so these pass through. */
static void *pt_self(void) { return (void *)pthread_self(); }
static int   pt_equal(void *a, void *b) { return a == b; }

const macshim_entry macshim_pthread[] = {
    { "_pthread_mutex_init",        pt_mutex_init },
    { "_pthread_mutex_destroy",     pt_mutex_destroy },
    { "_pthread_mutex_lock",        pt_mutex_lock },
    { "_pthread_mutex_unlock",      pt_mutex_unlock },
    { "_pthread_mutex_trylock",     pt_mutex_trylock },
    { "_pthread_mutexattr_init",    pt_mutexattr_init },
    { "_pthread_mutexattr_destroy", pt_mutexattr_destroy },
    { "_pthread_mutexattr_settype", pt_mutexattr_settype },
    { "_pthread_once",              pt_once },
    { "_pthread_self",              pt_self },
    { "_pthread_equal",             pt_equal },
    { NULL, NULL }
};


/* ------------------------------------------------ CFString, the rest of it */

/* Parameter names and value text travel as CFStrings, so these are real. Strings
 * are held as UTF-8 and converted at the UTF-16 boundaries, which is where the
 * CoreFoundation API insists on unichar. */
static void cfstr_setlen(cfstring *s, CFIndex n) { s->len = n; s->s[n] = 0; }

static int cfstr_grow(cfstring *s, CFIndex extra)
{
    char *p = realloc(s->s, (size_t)s->len + (size_t)extra + 1);
    if (!p) return 0;
    s->s = p;
    return 1;
}

static CFTypeRef cf_string_create_mutable(void *alloc, CFIndex maxlen)
{
    cfstring *s;
    (void)alloc; (void)maxlen;
    if (!(s = obj_new(sizeof *s, CF_STRING))) return NULL;
    if (!(s->s = calloc(1, 1))) { free(s); return NULL; }
    s->len = 0;
    return s;
}

static CFTypeRef cf_string_create_copy(void *alloc, CFTypeRef src)
{
    CFIndex n = 0;
    const char *p = str_of(src, &n);
    (void)alloc;
    return p ? cf_string_create(NULL, p, 0) : NULL;
}

static void cf_string_append_cstring(CFTypeRef p, const char *add, uint32_t enc)
{
    cfobj *o = as_obj(p);
    cfstring *s = (cfstring *)o;
    CFIndex n;
    (void)enc;
    if (!o || o->type != CF_STRING || !add) return;
    n = (CFIndex)strlen(add);
    if (!cfstr_grow(s, n)) return;
    memcpy(s->s + s->len, add, (size_t)n);
    cfstr_setlen(s, s->len + n);
}

/* UTF-16 to UTF-8, for the unichar-based entry points. */
static CFIndex u16_to_u8(const uint16_t *u, CFIndex n, char *out, CFIndex cap)
{
    CFIndex i, w = 0;
    for (i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
            u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        }
        if (c < 0x80)         { if (w + 1 > cap) break; out[w++] = (char)c; }
        else if (c < 0x800)   { if (w + 2 > cap) break;
                                out[w++] = (char)(0xC0 | (c >> 6));
                                out[w++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { if (w + 3 > cap) break;
                                out[w++] = (char)(0xE0 | (c >> 12));
                                out[w++] = (char)(0x80 | ((c >> 6) & 0x3F));
                                out[w++] = (char)(0x80 | (c & 0x3F)); }
        else                  { if (w + 4 > cap) break;
                                out[w++] = (char)(0xF0 | (c >> 18));
                                out[w++] = (char)(0x80 | ((c >> 12) & 0x3F));
                                out[w++] = (char)(0x80 | ((c >> 6) & 0x3F));
                                out[w++] = (char)(0x80 | (c & 0x3F)); }
    }
    return w;
}

static CFTypeRef cf_string_with_chars_nocopy(void *alloc, const uint16_t *chars,
                                             CFIndex n, void *dealloc)
{
    char *buf;
    CFTypeRef r;
    (void)alloc; (void)dealloc;
    if (!chars || n < 0) return NULL;
    if (!(buf = malloc((size_t)n * 4 + 1))) return NULL;
    buf[u16_to_u8(chars, n, buf, n * 4)] = 0;
    r = cf_string_create(NULL, buf, 0);
    free(buf);
    return r;
}
static CFTypeRef cf_string_mutable_external(void *alloc, uint16_t *chars,
                                            CFIndex n, CFIndex cap, void *d)
{ (void)cap; (void)d; return cf_string_with_chars_nocopy(alloc, chars, n, NULL); }

static void cf_string_append_chars(CFTypeRef p, const uint16_t *chars, CFIndex n)
{
    char *buf;
    if (!chars || n <= 0) return;
    if (!(buf = malloc((size_t)n * 4 + 1))) return;
    buf[u16_to_u8(chars, n, buf, n * 4)] = 0;
    cf_string_append_cstring(p, buf, 0);
    free(buf);
}

static CFTypeRef cf_string_with_bytes(void *alloc, const uint8_t *b, CFIndex n,
                                      uint32_t enc, unsigned char external)
{
    char *buf;
    CFTypeRef r;
    (void)alloc; (void)external;
    if (!b || n < 0) return NULL;
    /* 0x0100 and 0x0101 are the UTF-16 encodings; everything else is treated as
     * bytes, which covers ASCII and UTF-8. */
    if (enc == 0x0100 || enc == 0x0101)
        return cf_string_with_chars_nocopy(NULL, (const uint16_t *)b, n / 2, NULL);
    if (!(buf = malloc((size_t)n + 1))) return NULL;
    memcpy(buf, b, (size_t)n);
    buf[n] = 0;
    r = cf_string_create(NULL, buf, 0);
    free(buf);
    return r;
}

static CFIndex cf_string_get_length(CFTypeRef p)
{ CFIndex n = 0; str_of(p, &n); return n; }

static CFIndex cf_string_max_size(CFIndex len, uint32_t enc)
{ return (enc == 0x0100 || enc == 0x0101) ? len * 2 + 2 : len * 4 + 1; }

/* CFRange is two CFIndexes, and CFStringGetBytes takes one *by value*. On
 * System V a 16-byte struct of two integers goes in two registers rather than
 * through a pointer, so declaring the parameter as a `void *` shifted every
 * argument after it by one register: `buf` arrived where `cap` was read, `cap`
 * where `used` was, and the write-back pointer was a stack address the callee
 * then stored through. Deputy's VST3 asks for all 2191 of its parameter names
 * this way and faulted on the first. */
typedef struct { CFIndex location, length; } CFRange;

static CFIndex cf_string_get_bytes(CFTypeRef p, CFRange range, uint32_t enc,
                                   uint8_t loss, unsigned char ext,
                                   uint8_t *buf, CFIndex cap, CFIndex *used)
{
    CFIndex n = 0;
    const char *s = str_of(p, &n);
    (void)loss; (void)ext;
    if (used) *used = 0;
    if (!s) return 0;
    /* The range is in characters. These strings are ASCII -- see below -- so it
     * is a byte range too. Clamped rather than trusted: a range past the end is
     * a plugin's arithmetic, not something to read through. */
    if (range.location < 0 || range.location > n) return 0;
    s += range.location;
    n -= range.location;
    if (range.length >= 0 && range.length < n) n = range.length;

    if (enc == 0x0100 || enc == 0x0101) {
        /* UTF-8 back to UTF-16, ASCII-only fast path: these strings are
         * parameter names, so anything else is rare and lossy either way.
         * A NULL buffer is a caller asking how much room to reserve, which is
         * the documented way to use this call and must not write anything. */
        CFIndex i, w = 0;
        for (i = 0; i < n; i++) {
            if (buf) {
                if ((w + 1) * 2 > cap) break;
                ((uint16_t *)buf)[w] = (uint8_t)s[i];
            }
            w++;
        }
        if (used) *used = w * 2;
        return w;
    }
    if (buf) {
        CFIndex k = n < cap ? n : cap;
        memcpy(buf, s, (size_t)k);
        if (used) *used = k;
        return k;
    }
    if (used) *used = n;
    return n;
}

static int32_t cf_string_compare(CFTypeRef a, CFTypeRef b, uint64_t opts)
{
    CFIndex la = 0, lb = 0;
    const char *sa = str_of(a, &la), *sb = str_of(b, &lb);
    int r;
    if (!sa || !sb) return sa ? 1 : (sb ? -1 : 0);
    r = (opts & 1) ? strcasecmp(sa, sb) : strcmp(sa, sb);   /* bit 0: caseless */
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}
static int32_t cf_string_compare_opts(CFTypeRef a, void *range, CFTypeRef b,
                                      uint64_t opts)
{ (void)range; return cf_string_compare(a, b, opts); }

static double cf_string_double(CFTypeRef p)
{ CFIndex n = 0; const char *s = str_of(p, &n); return s ? strtod(s, NULL) : 0.0; }
static int32_t cf_string_int(CFTypeRef p)
{ CFIndex n = 0; const char *s = str_of(p, &n); return s ? (int32_t)strtol(s, NULL, 10) : 0; }

static void cf_string_case(CFTypeRef p, int up)
{
    cfobj *o = as_obj(p);
    cfstring *s = (cfstring *)o;
    CFIndex i;
    if (!o || o->type != CF_STRING || !s->s) return;
    for (i = 0; i < s->len; i++)
        s->s[i] = up ? (char)toupper((unsigned char)s->s[i])
                     : (char)tolower((unsigned char)s->s[i]);
}
static void cf_string_upper(CFTypeRef p, void *loc) { (void)loc; cf_string_case(p, 1); }
static void cf_string_lower(CFTypeRef p, void *loc) { (void)loc; cf_string_case(p, 0); }
static void cf_string_normalize(CFTypeRef p, long form) { (void)p; (void)form; }

static CFIndex cf_string_find_replace(CFTypeRef p, CFTypeRef find, CFTypeRef repl,
                                      void *range, uint64_t opts)
{
    cfobj *o = as_obj(p);
    cfstring *s = (cfstring *)o;
    CFIndex lf = 0, lr = 0, count = 0;
    const char *f = str_of(find, &lf), *r = str_of(repl, &lr);
    char *out, *w, *scan;
    (void)range; (void)opts;
    if (!o || o->type != CF_STRING || !s->s || !f || !lf) return 0;
    /* Worst case every character becomes the replacement. */
    if (!(out = malloc((size_t)s->len * (size_t)(lr > 1 ? lr : 1) + (size_t)lr + 1)))
        return 0;
    w = out; scan = s->s;
    while (*scan) {
        if (!strncmp(scan, f, (size_t)lf)) {
            if (r && lr) { memcpy(w, r, (size_t)lr); w += lr; }
            scan += lf;
            count++;
        } else {
            *w++ = *scan++;
        }
    }
    *w = 0;
    free(s->s);
    s->s = out;
    s->len = w - out;
    return count;
}

/* CFStringCreateWithFormat: only %@ %s %d %u %f %g and %% appear here, and %@ is
 * always another string. A general printf clone would be more code than value. */
static CFTypeRef cf_string_with_format_v(void *alloc, void *opts, CFTypeRef fmt,
                                         va_list ap)
{
    CFIndex fl = 0;
    const char *f = str_of(fmt, &fl);
    char out[1024];
    size_t w = 0;
    (void)alloc; (void)opts;
    if (!f) return NULL;
    while (*f && w + 1 < sizeof out) {
        if (*f != '%') { out[w++] = *f++; continue; }
        f++;
        while (*f && strchr("0123456789.-+ #", *f)) f++;     /* skip flags/width */
        switch (*f) {
        case '@': { CFIndex n = 0;
                    const char *s = str_of(va_arg(ap, void *), &n);
                    w += (size_t)snprintf(out + w, sizeof out - w, "%s", s ? s : "(null)");
                    break; }
        case 's': w += (size_t)snprintf(out + w, sizeof out - w, "%s", va_arg(ap, const char *)); break;
        case 'd': case 'i':
                  w += (size_t)snprintf(out + w, sizeof out - w, "%d", va_arg(ap, int)); break;
        case 'u': w += (size_t)snprintf(out + w, sizeof out - w, "%u", va_arg(ap, unsigned)); break;
        case 'f': case 'g':
                  w += (size_t)snprintf(out + w, sizeof out - w, "%g", va_arg(ap, double)); break;
        case '%': out[w++] = '%'; break;
        default:  out[w++] = '%'; if (*f) out[w++] = *f; break;
        }
        if (*f) f++;
        if (w >= sizeof out) w = sizeof out - 1;
    }
    out[w] = 0;
    return cf_string_create(NULL, out, 0);
}
static CFTypeRef cf_string_with_format(void *alloc, void *opts, CFTypeRef fmt, ...)
{
    va_list ap;
    CFTypeRef r;
    va_start(ap, fmt);
    r = cf_string_with_format_v(alloc, opts, fmt, ap);
    va_end(ap);
    return r;
}

/* --------------------------------------------------------- arrays and data */

static CFTypeRef cf_array_create_mutable(void *alloc, CFIndex cap, const void *cb)
{
    cfarray *a;
    (void)alloc; (void)cb;
    if (!(a = obj_new(sizeof *a, CF_ARRAY))) return NULL;
    a->cap = cap > 0 ? cap : 8;
    a->v = calloc((size_t)a->cap, sizeof *a->v);
    if (!a->v) { free(a); return NULL; }
    return a;
}
static void cf_array_append(CFTypeRef p, const void *v)
{
    cfobj *o = as_obj(p);
    cfarray *a = (cfarray *)o;
    if (!o || o->type != CF_ARRAY) return;
    if (a->n == a->cap) {
        CFIndex nc = a->cap * 2;
        const void **nv = realloc(a->v, (size_t)nc * sizeof *nv);
        if (!nv) return;
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = v;
    cf_retain(v);
}
static CFTypeRef cf_data_create(void *alloc, const uint8_t *b, CFIndex n)
{
    CFTypeRef d = cf_data_create_mutable(alloc, n + 1);
    if (d) cf_data_append(d, b, n);
    return d;
}
static CFIndex cf_get_retain_count(CFTypeRef p)
{ cfobj *o = as_obj(p); return o ? (CFIndex)atomic_load(&o->refs) : 1; }

/* CFAllocator: the plugin only ever passes these straight back. */
static void *cf_alloc_allocate(void *a, CFIndex size, uint32_t hint)
{ (void)a; (void)hint; return malloc(size > 0 ? (size_t)size : 1); }
static void  cf_alloc_deallocate(void *a, void *p) { (void)a; free(p); }
static void *g_allocator_null;

/* CFUUID */
typedef struct { cfobj o; uint8_t b[16]; } cfuuid;
static CFTypeRef cf_uuid_create(void *alloc)
{
    /* Its own type, not CF_DATA. A cfdata's first field after the header is a
     * pointer to its bytes; a cfuuid's is the bytes themselves. Tagged as data,
     * releasing one sent sixteen random bytes' first eight to free() --
     * "free(): invalid pointer", from a plugin that had done nothing wrong. */
    cfuuid *u = obj_new(sizeof *u, CF_UUID);
    int i;
    (void)alloc;
    if (!u) return NULL;
    /* Not cryptographic; a plugin uses this to key its own instance table. */
    for (i = 0; i < 16; i++) u->b[i] = (uint8_t)(rand() & 0xff);
    u->b[6] = (uint8_t)((u->b[6] & 0x0f) | 0x40);
    u->b[8] = (uint8_t)((u->b[8] & 0x3f) | 0x80);
    return u;
}
typedef struct { uint8_t b[16]; } cfuuid_bytes;
static cfuuid_bytes cf_uuid_bytes(void *alloc, CFTypeRef u)
{
    cfuuid_bytes out;
    (void)alloc;
    memset(&out, 0, sizeof out);
    if (u) memcpy(out.b, ((cfuuid *)u)->b, 16);
    return out;
}

/* ---------------------------------------------------------------- CFBundle */

/* Bundles are only asked for their identifier, their Info.plist values and a
 * function pointer -- and the only image loaded is the plugin itself, which the
 * loader already has. Nothing here loads anything new. */
static cfdict *g_main_bundle_info;
static void   *g_main_bundle;
static char    g_bundle_path[4096];
static void __attribute__((constructor)) init_bundle(void)
{
    g_main_bundle_info = (cfdict *)cf_dict_create_mutable(NULL, 8, NULL, NULL);
    g_main_bundle = g_main_bundle_info;
    /* Held forever, deliberately. CFBundleGetInfoDictionary is a Get, so a
     * plugin does not own what it returns -- but a plugin that releases it
     * anyway would take the host's only info dictionary with it, and the *next*
     * plugin would then find an empty one and look up nothing. That is not
     * hypothetical: it is why a second VSTGUI editor in one session could not
     * find the font its Info.plist names, and asked the bundle for a file
     * called ".png". */
    if (g_main_bundle_info) atomic_fetch_add(&g_main_bundle_info->o.refs, 1000);
}

/* ---- Info.plist -------------------------------------------------------
 *
 * The bundle's info dictionary was an empty one, which is enough for a plugin
 * that only asks whether a key exists and quite a lot less than enough for one
 * that keeps *data* there. Audio Damage's editors do: the Info.plist names the
 * font each of their bitmap fonts is stored in --
 *
 *     <key>FontrastInfo_20000</key> <string>tahoma9</string>
 *
 * -- and the editor reads that key, asks the bundle for `tahoma9.tab`, and
 * loads the glyph table from it. With the dictionary empty the lookup returned
 * nothing and the whole load was skipped, which is where the crash seven of
 * these editors died in began.
 *
 * The parser is a scan for <key> followed by its value, not an XML reader:
 * these files are machine-written and the elements that carry anything a
 * plugin reads are strings, integers and booleans. Keys inside a nested dict
 * or array are skipped, so an AudioComponents entry cannot shadow a top-level
 * one. */
static const char *plist_tag(const char *p, const char *end, const char *tag,
                             const char **body, CFIndex *len)
{
    char open[32], close[32];
    const char *a, *b;
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    if (!(a = memmem(p, (size_t)(end - p), open, strlen(open)))) return NULL;
    a += strlen(open);
    if (!(b = memmem(a, (size_t)(end - a), close, strlen(close)))) return NULL;
    *body = a;
    *len = (CFIndex)(b - a);
    return b + strlen(close);
}

/* Turn &amp; and friends back into what they were. */
static void plist_unescape(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if (!strncmp(r, "&amp;", 5))       { *w++ = '&';  r += 5; continue; }
            if (!strncmp(r, "&lt;", 4))        { *w++ = '<';  r += 4; continue; }
            if (!strncmp(r, "&gt;", 4))        { *w++ = '>';  r += 4; continue; }
            if (!strncmp(r, "&quot;", 6))      { *w++ = '"';  r += 6; continue; }
            if (!strncmp(r, "&apos;", 6))      { *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = 0;
}

#define PLIST_MAX (1u << 20)      /* an Info.plist is a few kilobytes */

static void plist_load(const char *path, void *dict)
{
    char key[256], val[1024];
    char *file = NULL;
    const char *p, *end, *body;
    CFIndex n = 0, len;
    FILE *f = fopen(path, "rb");
    long size;
    size_t got;
    int depth = 0;

    if (!f) return;
    /* Read the whole file rather than a fixed buffer's worth. A truncated
     * Info.plist is worse than none: the keys a plugin reads sit at the end as
     * often as the start, and losing them fails much later and looks like
     * something else entirely. */
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (unsigned long)size > PLIST_MAX) { fclose(f); return; }
    if (!(file = malloc((size_t)size + 1))) { fclose(f); return; }
    got = fread(file, 1, (size_t)size, f);
    fclose(f);
    file[got] = 0;
    p = file; end = file + got;

    /* Past the outermost <dict>, then key by key. */
    while (p < end) {
        const char *k = memmem(p, (size_t)(end - p), "<key>", 5);
        const char *nest = memmem(p, (size_t)(end - p), "<dict>", 6);
        const char *arr  = memmem(p, (size_t)(end - p), "<array>", 7);
        const char *nend = memmem(p, (size_t)(end - p), "</dict>", 7);
        const char *aend = memmem(p, (size_t)(end - p), "</array>", 8);
        const char *first = k;
        if (nest && (!first || nest < first)) first = nest;
        if (arr  && (!first || arr  < first)) first = arr;
        if (nend && (!first || nend < first)) first = nend;
        if (aend && (!first || aend < first)) first = aend;
        if (!first) break;
        if (first == nest) { depth++; p = nest + 6; continue; }
        if (first == arr)  { depth++; p = arr + 7;  continue; }
        if (first == nend) { depth--; p = nend + 7; continue; }
        if (first == aend) { depth--; p = aend + 8; continue; }

        /* first == k: a key. */
        p = plist_tag(k, end, "key", &body, &len);
        if (!p) break;
        if (depth != 1) continue;            /* nested: not ours to publish */
        if (len >= (CFIndex)sizeof key) len = (CFIndex)sizeof key - 1;
        memcpy(key, body, (size_t)len); key[len] = 0;
        plist_unescape(key);

        /* The value is whatever element comes next. */
        {   const char *q = p;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
            if (q + 8 <= end && !strncmp(q, "<string>", 8)) {
                const char *e = memmem(q, (size_t)(end - q), "</string>", 9);
                if (!e) break;
                len = (CFIndex)(e - (q + 8));
                if (len >= (CFIndex)sizeof val) len = (CFIndex)sizeof val - 1;
                memcpy(val, q + 8, (size_t)len); val[len] = 0;
                plist_unescape(val);
                macshim_cf_dict_set_pub(dict, macshim_cf_string(key),
                                        macshim_cf_string(val));
                p = e + 9;
            } else if (q + 9 <= end && !strncmp(q, "<integer>", 9)) {
                const char *e = memmem(q, (size_t)(end - q), "</integer>", 10);
                if (!e) break;
                len = (CFIndex)(e - (q + 9));
                if (len >= (CFIndex)sizeof val) len = (CFIndex)sizeof val - 1;
                memcpy(val, q + 9, (size_t)len); val[len] = 0;
                macshim_cf_dict_set_pub(dict, macshim_cf_string(key),
                                        macshim_cf_number_int(atol(val)));
                p = e + 10;
            } else if (q + 7 <= end && !strncmp(q, "<true/>", 7)) {
                macshim_cf_dict_set_pub(dict, macshim_cf_string(key), &g_true);
                p = q + 7;
            } else if (q + 8 <= end && !strncmp(q, "<false/>", 8)) {
                macshim_cf_dict_set_pub(dict, macshim_cf_string(key), &g_false);
                p = q + 8;
            }
            /* Anything else -- a nested dict or array -- is left for the walk
             * above to step over on the next pass. */
        }
        n++;
    }
    if (getenv("MACCF_VERBOSE"))
        fprintf(stderr, "  [cf] Info.plist: %ld key(s) from %s\n", (long)n, path);
    free(file);
}

/* Empty a dictionary, keeping the object. */
static void cf_dict_clear(CFTypeRef p)
{
    cfobj *o = as_obj(p);
    cfdict *d = (cfdict *)o;
    CFIndex i;
    if (!o || o->type != CF_DICT) return;
    for (i = 0; i < d->n; i++) { cf_release(d->k[i]); cf_release(d->v[i]); }
    d->n = 0;
}

/* Set by the loader once it knows which bundle is being opened.
 *
 * The info dictionary is emptied first, because "the main bundle" means the
 * plugin that is loaded *now*. Left to accumulate, a plugin read the previous
 * one's keys -- and since two of these keep the name of their bitmap font
 * under the same key, the second one to load looked up a font belonging to the
 * first, read a glyph table that did not describe it, and crashed on the first
 * character it drew. */
void macshim_set_bundle(const char *path)
{
    char plist[4200];
    snprintf(g_bundle_path, sizeof g_bundle_path, "%s", path ? path : "");
    if (!g_main_bundle_info) return;
    cf_dict_clear(g_main_bundle_info);
    if (!g_bundle_path[0]) return;
    snprintf(plist, sizeof plist, "%s/Contents/Info.plist", g_bundle_path);
    plist_load(plist, g_main_bundle_info);
}
const char *macshim_bundle_path(void) { return g_bundle_path; }

static void *cf_bundle_get_main(void) { return g_main_bundle; }
static void *cf_bundle_create(void *alloc, void *url) { (void)alloc; (void)url; return g_main_bundle; }
static void *cf_bundle_with_id(CFTypeRef ident) { (void)ident; return g_main_bundle; }
static void *cf_bundle_identifier(void *b) { (void)b; return NULL; }

/* A URL is a string carrying a path here, so the bundle URL is the bundle
 * directory and a resource URL is a path built under it. */
static void *cf_bundle_copy_url_real(void *b)
{
    if (getenv("MACCF_VERBOSE")) fprintf(stderr, "  [cf] CFBundleCopyBundleURL -> %s\n", g_bundle_path); (void)b; return g_bundle_path[0] ? (void *)cf_string_create(NULL, g_bundle_path, 0) : NULL; }

static void *cf_bundle_copy_resource_url(void *b, CFTypeRef name, CFTypeRef ext,
                                         CFTypeRef sub)
{
    char p[4600], nm[256] = { 0 }, ex[64] = { 0 }, sb[256] = { 0 };
    struct stat st;
    CFIndex n = 0;
    const char *s;
    (void)b;
    if (!g_bundle_path[0]) return NULL;
    if ((s = str_of(name, &n))) snprintf(nm, sizeof nm, "%s", s);
    if ((s = str_of(ext, &n)))  snprintf(ex, sizeof ex, "%s", s);
    if ((s = str_of(sub, &n)))  snprintf(sb, sizeof sb, "%s", s);
    if (sb[0]) snprintf(p, sizeof p, "%s/Contents/Resources/%s/%s%s%s",
                        g_bundle_path, sb, nm, ex[0] ? "." : "", ex);
    else       snprintf(p, sizeof p, "%s/Contents/Resources/%s%s%s",
                        g_bundle_path, nm, ex[0] ? "." : "", ex);
    if (stat(p, &st)) {
        if (getenv("MACOBJC_VERBOSE"))
            fprintf(stderr, "  [bundle] CFBundleCopyResourceURL(%s.%s) -> not found\n",
                    nm, ex);
        return NULL;
    }
    if (getenv("MACOBJC_VERBOSE"))
        fprintf(stderr, "  [bundle] CFBundleCopyResourceURL -> %s\n", p);
    return (void *)cf_string_create(NULL, p, 0);
}
static void *cf_bundle_info_value(void *b, CFTypeRef key)
{ (void)b; return (void *)cf_dict_get(g_main_bundle_info, key); }
/* The whole dictionary, for a caller that walks it rather than asking key by
 * key. It is the same one CFBundleGetValueForInfoDictionaryKey reads, so the two
 * cannot disagree. */
static void *cf_bundle_info_dict(void *b) { (void)b; return (void *)g_main_bundle_info; }
static void *cf_bundle_copy_url(void *b) { (void)b; return NULL; }
static unsigned char cf_bundle_load(void *b) { (void)b; return 1; }
static void cf_bundle_unload(void *b) { (void)b; }
static void *cf_bundle_function(void *b, CFTypeRef name)
{ (void)b; (void)name; return NULL; }
static unsigned char cf_url_fsrep(CFTypeRef url, unsigned char resolve,
                                  uint8_t *buf, CFIndex cap)
{
    CFIndex n = 0;
    const char *s = str_of(url, &n);
    (void)resolve;
    if (getenv("MACCF_VERBOSE"))
        fprintf(stderr, "  [cf] CFURLGetFileSystemRepresentation(%s) cap=%ld\n",
                s ? s : "(not a string)", (long)cap);
    if (!s || !buf || n >= cap) { if (buf && cap) buf[0] = 0; return 0; }
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    return 1;
}
/* ---- URLs -------------------------------------------------------------
 *
 * A URL here is one of this shim's CFStrings carrying a POSIX path, which is
 * enough for everything a plugin does with one: build it from a path, walk up a
 * directory or two, hand it to CFBundleCreate, and read it back as a path.
 * VSTGUI's `InitMachOLibrary` does exactly that -- ask dyld which image its code
 * is in, turn the answer into a URL, delete the last three path components to
 * get from Contents/MacOS/Foo to Foo.vst, and create the bundle. */
static CFTypeRef cf_url_from_fsrep(void *alloc, const uint8_t *buf, CFIndex len,
                                   unsigned char isdir)
{
    char path[4096];
    CFIndex n = len;
    (void)alloc; (void)isdir;
    if (!buf) return NULL;
    if (n < 0 || n >= (CFIndex)sizeof path) n = (CFIndex)sizeof path - 1;
    memcpy(path, buf, (size_t)n);
    path[n] = 0;
    return (CFTypeRef)cf_string_create(NULL, path, 0);
}
static CFTypeRef cf_url_with_fspath(void *alloc, CFTypeRef path, long style,
                                    unsigned char isdir)
{
    CFIndex n = 0;
    const char *s = str_of(path, &n);
    (void)alloc; (void)style; (void)isdir;
    return s ? (CFTypeRef)cf_string_create(NULL, s, 0) : NULL;
}
/* The parent directory. A trailing slash is dropped first, so that deleting the
 * last component of "/a/b/" gives "/a" rather than "/a/b". */
static CFTypeRef cf_url_delete_last(void *alloc, CFTypeRef url)
{
    char path[4096];
    CFIndex n = 0;
    const char *s = str_of(url, &n);
    char *slash;
    (void)alloc;
    if (!s) return NULL;
    snprintf(path, sizeof path, "%s", s);
    n = (CFIndex)strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    if ((slash = strrchr(path, '/')) && slash != path) *slash = 0;
    else if (slash) slash[1] = 0;
    return (CFTypeRef)cf_string_create(NULL, path, 0);
}
static CFTypeRef cf_url_copy_fspath(void *alloc, CFTypeRef url, long style)
{
    CFIndex n = 0;
    const char *s = str_of(url, &n);
    (void)alloc; (void)style;
    return s ? (CFTypeRef)cf_string_create(NULL, s, 0) : NULL;
}
static CFTypeRef cf_url_copy_last(void *alloc, CFTypeRef url)
{
    CFIndex n = 0;
    const char *s = str_of(url, &n);
    const char *slash;
    (void)alloc;
    if (!s) return NULL;
    slash = strrchr(s, '/');
    return (CFTypeRef)cf_string_create(NULL, slash ? slash + 1 : s, 0);
}
static CFTypeRef cf_url_append(void *alloc, CFTypeRef url, CFTypeRef comp,
                               unsigned char isdir)
{
    char path[4096];
    CFIndex n = 0, m = 0;
    const char *s = str_of(url, &n), *c = str_of(comp, &m);
    (void)alloc; (void)isdir;
    if (!s) return NULL;
    snprintf(path, sizeof path, "%s%s%s", s,
             (n && s[n - 1] == '/') ? "" : "/", c ? c : "");
    return (CFTypeRef)cf_string_create(NULL, path, 0);
}

static void *g_k_bundle_version;
static void *g_type_array_cb[8];

static void cf_runloop_remove_timer(void *rl, void *t, void *mode)
{ (void)rl; (void)t; (void)mode; }

/* NoCopy is a promise about ownership, not about behaviour: copying is always
 * allowed and avoids inheriting the caller's deallocator. */
static CFTypeRef cf_str_nocopy(void *alloc, const char *c, uint32_t enc, void *d)
{ (void)alloc; (void)enc; (void)d; return cf_string_create(NULL, c, 0); }

/* An attributed string keeps only its text here; nothing consumes the attributes
 * until text layout exists. */
static CFTypeRef cf_attributed_create(void *alloc, CFTypeRef str, CFTypeRef attrs)
{ (void)alloc; (void)attrs; return cf_retain(str); }

static int32_t cf_user_alert(double timeout, uint32_t flags, void *icon,
                             void *sound, void *loc, CFTypeRef header,
                             CFTypeRef message, CFTypeRef d1, CFTypeRef d2,
                             CFTypeRef d3, uint32_t *response)
{
    CFIndex n = 0;
    const char *h = str_of(header, &n);
    (void)timeout; (void)flags; (void)icon; (void)sound; (void)loc;
    (void)message; (void)d1; (void)d2; (void)d3;
    fprintf(stderr, "macho: the plugin tried to show an alert: %s\n", h ? h : "?");
    if (response) *response = 0;
    return 0;
}

const macshim_entry macshim_cf2[] = {
    { "_CFStringCreateWithCStringNoCopy", cf_str_nocopy },
    { "_CFAttributedStringCreate",   cf_attributed_create },
    { "_CFUserNotificationDisplayAlert", cf_user_alert },
    { "_CFStringCreateMutable",      cf_string_create_mutable },
    { "_CFStringCreateCopy",         cf_string_create_copy },
    { "_CFStringAppendCString",      cf_string_append_cstring },
    { "_CFStringAppendCharacters",   cf_string_append_chars },
    { "_CFStringCreateWithCharactersNoCopy", cf_string_with_chars_nocopy },
    { "_CFStringCreateMutableWithExternalCharactersNoCopy", cf_string_mutable_external },
    { "_CFStringCreateWithBytes",    cf_string_with_bytes },
    { "_CFStringGetLength",          cf_string_get_length },
    { "_CFStringGetMaximumSizeForEncoding", cf_string_max_size },
    { "_CFStringGetBytes",           cf_string_get_bytes },
    { "_CFStringCompare",            cf_string_compare },
    { "_CFStringCompareWithOptions", cf_string_compare_opts },
    { "_CFStringGetDoubleValue",     cf_string_double },
    { "_CFStringGetIntValue",        cf_string_int },
    { "_CFStringUppercase",          cf_string_upper },
    { "_CFStringLowercase",          cf_string_lower },
    { "_CFStringNormalize",          cf_string_normalize },
    { "_CFStringFindAndReplace",     cf_string_find_replace },
    { "_CFStringCreateWithFormat",   cf_string_with_format },
    { "_CFStringCreateWithFormatAndArguments", cf_string_with_format_v },
    { "_CFArrayCreateMutable",       cf_array_create_mutable },
    { "_CFArrayAppendValue",         cf_array_append },
    { "_CFDataCreate",               cf_data_create },
    { "_CFGetRetainCount",           cf_get_retain_count },
    { "_CFAllocatorAllocate",        cf_alloc_allocate },
    { "_CFAllocatorDeallocate",      cf_alloc_deallocate },
    { "_CFUUIDCreate",               cf_uuid_create },
    { "_CFUUIDGetUUIDBytes",         cf_uuid_bytes },
    { "_CFBundleGetMainBundle",      cf_bundle_get_main },
    { "_CFBundleCreate",             cf_bundle_create },
    { "_CFBundleGetBundleWithIdentifier", cf_bundle_with_id },
    { "_CFBundleGetIdentifier",      cf_bundle_identifier },
    { "_CFBundleGetValueForInfoDictionaryKey", cf_bundle_info_value },
    { "_CFBundleGetInfoDictionary",  cf_bundle_info_dict },
    { "_CFBundleCopyBundleURL",      cf_bundle_copy_url_real },
    { "_CFBundleCopyResourceURL",    cf_bundle_copy_resource_url },
    { "_CFBundleCopyResourcesDirectoryURL", cf_bundle_copy_url_real },
    { "_CFBundleLoadExecutable",     cf_bundle_load },
    { "_CFBundleUnloadExecutable",   cf_bundle_unload },
    { "_CFBundleGetFunctionPointerForName", cf_bundle_function },
    { "_CFURLGetFileSystemRepresentation",  cf_url_fsrep },
    { "_CFURLCreateFromFileSystemRepresentation", cf_url_from_fsrep },
    { "_CFURLCreateWithFileSystemPath",     cf_url_with_fspath },
    { "_CFURLCreateCopyDeletingLastPathComponent", cf_url_delete_last },
    { "_CFURLCreateCopyAppendingPathComponent", cf_url_append },
    { "_CFURLCopyFileSystemPath",           cf_url_copy_fspath },
    { "_CFURLCopyLastPathComponent",        cf_url_copy_last },
    { "_CFURLCopyPath",                     cf_url_copy_fspath },
    { "_CFRunLoopRemoveTimer",       cf_runloop_remove_timer },
    { "_kCFAllocatorNull",           &g_allocator_null },
    { "_kCFBundleVersionKey",        &g_k_bundle_version },
    { "_kCFTypeArrayCallBacks",      g_type_array_cb },
    { NULL, NULL }
};

/* ------------------------------------------------------------- AudioUnit */


/* These are the host-side AU calls a plugin makes back into its host: reading
 * its own parameters and properties, and rendering a dependency. They are
 * filled in by the AU host (macau.c) once an instance exists; until then they
 * report "not initialised" rather than crashing. */
static OSStatus (*g_get_param)(void *au, uint32_t id, uint32_t scope,
                               uint32_t elem, float *out);
static OSStatus (*g_get_prop)(void *au, uint32_t id, uint32_t scope,
                              uint32_t elem, void *data, uint32_t *size);
static OSStatus (*g_render)(void *au, uint32_t *flags, const void *ts,
                            uint32_t bus, uint32_t frames, void *bufs);

void macshim_set_au_callbacks(
    OSStatus (*getparam)(void *, uint32_t, uint32_t, uint32_t, float *),
    OSStatus (*getprop)(void *, uint32_t, uint32_t, uint32_t, void *, uint32_t *),
    OSStatus (*render)(void *, uint32_t *, const void *, uint32_t, uint32_t, void *))
{ g_get_param = getparam; g_get_prop = getprop; g_render = render; }

#define kAudioUnitErr_Uninitialized (-10867)

static OSStatus au_get_parameter(void *au, uint32_t id, uint32_t scope,
                                 uint32_t elem, float *out)
{
    if (g_get_param) return g_get_param(au, id, scope, elem, out);
    if (out) *out = 0.0f;
    return kAudioUnitErr_Uninitialized;
}
static OSStatus au_get_property(void *au, uint32_t id, uint32_t scope,
                                uint32_t elem, void *data, uint32_t *size)
{
    if (g_get_prop) return g_get_prop(au, id, scope, elem, data, size);
    if (size) *size = 0;
    return kAudioUnitErr_Uninitialized;
}
static OSStatus au_render(void *au, uint32_t *flags, const void *ts,
                          uint32_t bus, uint32_t frames, void *bufs)
{
    if (g_render) return g_render(au, flags, ts, bus, frames, bufs);
    return kAudioUnitErr_Uninitialized;
}
/* AUParameterSet and AUEventListenerNotify exist so a plugin's own UI can push
 * a value back; with no UI here there is nothing to notify. */
static OSStatus au_parameter_set(void *listener, void *obj, const void *ev,
                                 float value, uint32_t buffer_offset)
{ (void)listener; (void)obj; (void)ev; (void)value; (void)buffer_offset; return 0; }
static OSStatus au_event_notify(void *listener, void *obj, const void *ev)
{ (void)listener; (void)obj; (void)ev; return 0; }

const macshim_entry macshim_audiounit[] = {
    { "_AudioUnitGetParameter", au_get_parameter },
    { "_AudioUnitGetProperty",  au_get_property },
    { "_AudioUnitRender",       au_render },
    { "_AUParameterSet",        au_parameter_set },
    { "_AUEventListenerNotify", au_event_notify },
    { NULL, NULL }
};

/* ---------------------------------------------------------------- lookup */

void *macshim_lookup(const char *sym)
{
    static const macshim_entry *const tables[] = {
        macshim_vdsp, macshim_corefoundation, macshim_libsystem,
        macshim_pthread, macshim_audiounit, macshim_foundation,
        macshim_quartz, macshim_quartz2, macshim_quartz3,
        macshim_cf2, macshim_mach, macshim_files, macshim_metal,
        macshim_imageio, NULL
    };
    int t, i;
    if (!sym) return NULL;
    for (t = 0; tables[t]; t++)
        for (i = 0; tables[t][i].name; i++)
            if (!strcmp(tables[t][i].name, sym)) return tables[t][i].addr;
    return NULL;
}
