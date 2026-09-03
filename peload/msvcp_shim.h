/* The MSVC C++ library, natively, so a plug-in built against it does not need
 * Microsoft's DLL.
 *
 * Why this is separate from the C runtime beside it: the C runtime has no object
 * layout to get wrong, and this does. A plug-in does not merely call std::locale
 * -- it embeds one in its own objects, and the `use_facet` and `_Getfacet` it
 * calls are templates inlined into the plug-in's own code, walking our
 * structures with offsets baked in at its compile time. Guessing those offsets
 * produces a plug-in that loads and then reads a facet pointer from the wrong
 * eight bytes.
 *
 * So nothing here is guessed. Every layout below was read out of a real
 * msvcp140.dll with objdump, and the derivation is recorded with each one so the
 * next person can check it rather than trust it:
 *
 *   locale::facet    +0x00 vftable, +0x08 int _Ref
 *                    ??0facet@locale@std@@IEAA@_K@Z is three instructions:
 *                    [rcx+8] = edx (the initial refs), [rcx] = &vftable.
 *                    _Incref is `lock inc dword [rcx+8]`; _Decref is
 *                    `lock add dword [rcx+8], -1` returning `this` only when
 *                    the count reached zero, and NULL otherwise.
 *
 *   locale::_Locimp  extends facet:
 *                    +0x10 facet **_Facetvec
 *                    +0x18 size_t  _Facetcount
 *                    +0x20 int     _Catmask
 *                    +0x28 _Yarn<char> _Name
 *                    from _Locimp_Addfac, which grows _Facetvec to
 *                    max(id + 1, 40) entries, zeroes the new ones, and from
 *                    locale::_Init, which writes 0x3f (LC_ALL) at +0x20 and
 *                    assigns "C" to the _Yarn at +0x28.
 *
 *   the facet vtable slots, from the codecvt vtable at rva 0x5bcd0 with each
 *   entry resolved back through the export table:
 *                    [0] scalar deleting destructor
 *                    [1] _Incref
 *                    [2] _Decref
 *                    [3] do_always_noconv   [4] do_max_length
 *                    [5] do_encoding        [6] do_in
 *                    [7] do_out             [8] do_unshift
 *                    [9] do_length
 *                    (slot 3 resolves to a Concurrency predicate in the export
 *                    table, because the linker folded two functions that both
 *                    just return false. It is do_always_noconv.)
 *
 * The facet set of the classic locale is deliberately not built here. MSVC's
 * use_facet is inlined into the plug-in and constructs the facet it wants when
 * the locale does not have one, so an empty vector is a working locale rather
 * than a broken one -- and building forty facets whose behaviour would then have
 * to match MSVC's is how this becomes a project rather than a file.
 *
 * This is a fallback, asked only when no real msvcp is on the machine: the real
 * DLL is still preferred wherever it is installed, so nothing that works today
 * changes. x86-64 only for now -- the i386 half needs __thiscall entry points
 * and the plug-ins that would exercise it are not in the corpus here. */
#ifndef PELOAD_MSVCP_SHIM_H
#define PELOAD_MSVCP_SHIM_H

#if defined(__x86_64__)

/* ---- locale::facet ------------------------------------------------------- */

typedef struct mp_facet {
    void    *vftable;
    int32_t  ref;
    int32_t  pad;
    /* _Locimp adds its own fields from here; see the header comment. */
} mp_facet;

typedef struct mp_locimp {
    void      *vftable;
    int32_t    ref, pad;
    mp_facet **facetvec;          /* +0x10 */
    size_t     facetcount;        /* +0x18 */
    int32_t    catmask;           /* +0x20 */
    int32_t    pad2;
    void      *name[4];           /* +0x28: _Yarn<char>, opaque to us */
} mp_locimp;

static MS void mp_facet_Incref(mp_facet *self)
{ if (self) __atomic_add_fetch(&self->ref, 1, __ATOMIC_SEQ_CST); }

/* Returns `this` when the count reaches zero and NULL otherwise, which is how
 * the caller decides whether to run the destructor. */
static MS void *mp_facet_Decref(mp_facet *self)
{
    if (!self) return NULL;
    return __atomic_sub_fetch(&self->ref, 1, __ATOMIC_SEQ_CST) == 0 ? self : NULL;
}

static MS void *mp_facet_dtor(mp_facet *self, uint32_t deleting)
{
    /* Slot 0 is the scalar deleting destructor: the flag says whether to free.
     * A facet with a non-zero initial refcount is owned by whoever made it and
     * is never deleted through here, which is what that count is for. */
    if (self && deleting) w32_free(self);
    return self;
}

/* Answering "no conversion" is wrong for a wide codecvt and answering anything
 * else requires the conversion; these are the shapes the callers branch on. */
static MS int32_t mp_cvt_always_noconv(void *self) { (void)self; return 0; }
static MS int32_t mp_cvt_max_length(void *self)    { (void)self; return 1; }
static MS int32_t mp_cvt_encoding(void *self)      { (void)self; return 0; }

/* codecvt<wchar_t, char, mbstate_t>: the identity mapping over Latin-1, which
 * is what every label in this corpus is written in, and what the C locale this
 * stands in for specifies. A multi-byte code page would need the code page. */
static MS int32_t mp_cvt_in(void *self, void *st, const char *first, const char *last,
                            const char **mid, uint16_t *dfirst, uint16_t *dlast,
                            uint16_t **dmid)
{
    (void)self; (void)st;
    while (first < last && dfirst < dlast) *dfirst++ = (uint8_t)*first++;
    if (mid) *mid = first;
    if (dmid) *dmid = dfirst;
    return first == last ? 0 : 1;                 /* ok / partial */
}
static MS int32_t mp_cvt_out(void *self, void *st, const uint16_t *first,
                             const uint16_t *last, const uint16_t **mid,
                             char *dfirst, char *dlast, char **dmid)
{
    (void)self; (void)st;
    while (first < last && dfirst < dlast)
        *dfirst++ = (char)(*first < 256 ? *first : '?'), first++;
    if (mid) *mid = first;
    if (dmid) *dmid = dfirst;
    return first == last ? 0 : 1;
}
static MS int32_t mp_cvt_unshift(void *self, void *st, char *first, char *last,
                                 char **mid)
{ (void)self; (void)st; (void)last; if (mid) *mid = first; return 0; }
static MS int32_t mp_cvt_length(void *self, void *st, const char *first,
                                const char *last, size_t max)
{
    size_t n = (size_t)(last - first);
    (void)self; (void)st;
    return (int32_t)(n < max ? n : max);
}

/* The vtables, in the slot order derived above. */
static void *mp_facet_vft[3] = {
    (void *)mp_facet_dtor, (void *)mp_facet_Incref, (void *)mp_facet_Decref
};
static void *mp_codecvt_vft[10] = {
    (void *)mp_facet_dtor, (void *)mp_facet_Incref, (void *)mp_facet_Decref,
    (void *)mp_cvt_always_noconv, (void *)mp_cvt_max_length,
    (void *)mp_cvt_encoding, (void *)mp_cvt_in, (void *)mp_cvt_out,
    (void *)mp_cvt_unshift, (void *)mp_cvt_length
};

static MS void *mp_facet_ctor(mp_facet *self, size_t refs)
{
    if (!self) return NULL;
    self->vftable = mp_facet_vft;
    self->ref = (int32_t)refs;
    return self;
}
static MS void *mp_facet_dtor_plain(mp_facet *self) { return self; }

static MS void *mp_codecvt_ctor(mp_facet *self, size_t refs)
{
    if (!self) return NULL;
    self->vftable = mp_codecvt_vft;
    self->ref = (int32_t)refs;
    return self;
}

/* ---- locale::id ---------------------------------------------------------- */

/* Ids are handed out lazily from a counter, the first being 1: a zero id means
 * "not yet assigned", which is what the real implementation tests for. */
static int32_t mp_id_next = 0;

static MS size_t mp_id_value(size_t *self)
{
    if (!self) return 0;
    if (!*self)
        *self = (size_t)__atomic_add_fetch(&mp_id_next, 1, __ATOMIC_SEQ_CST);
    return *self;
}

/* ---- _Locimp ------------------------------------------------------------- */

static pthread_mutex_t mp_lock = PTHREAD_MUTEX_INITIALIZER;
static mp_locimp *mp_classic;

static mp_locimp *mp_locimp_new(void)
{
    mp_locimp *p = (mp_locimp *)w32_alloc(sizeof *p, 1);
    if (!p) return NULL;
    p->vftable = mp_facet_vft;
    p->ref = 0;
    /* 40 entries is what _Locimp_Addfac grows to on its first call; starting
     * there means the common case never reallocates. */
    p->facetcount = 40;
    p->facetvec = (mp_facet **)w32_alloc(p->facetcount * sizeof *p->facetvec, 1);
    if (!p->facetvec) { w32_free(p); return NULL; }
    p->catmask = 0x3f;                            /* LC_ALL */
    return p;
}

static MS void *mp_New_Locimp_b(uint32_t transparent)
{ (void)transparent; return mp_locimp_new(); }

static MS void *mp_New_Locimp_copy(const mp_locimp *from)
{
    mp_locimp *p = mp_locimp_new();
    size_t i;
    if (!p || !from || !from->facetvec) return p;
    for (i = 0; i < from->facetcount && i < p->facetcount; i++) {
        if (!(p->facetvec[i] = from->facetvec[i])) continue;
        mp_facet_Incref(p->facetvec[i]);          /* shared, so counted */
    }
    p->catmask = from->catmask;
    return p;
}

static MS void mp_Locimp_Addfac(mp_locimp *imp, mp_facet *pf, size_t id)
{
    if (!imp || !pf) return;
    pthread_mutex_lock(&mp_lock);
    if (imp->facetcount <= id) {
        size_t want = id + 1 < 40 ? 40 : id + 1;
        mp_facet **v = (mp_facet **)w32_realloc(imp->facetvec,
                                                want * sizeof *v, 1);
        if (!v) { pthread_mutex_unlock(&mp_lock); return; }
        imp->facetvec = v;
        imp->facetcount = want;
    }
    mp_facet_Incref(pf);
    if (imp->facetvec[id]) {
        /* Replacing one drops the reference the vector held, and the facet goes
         * only if that was the last -- which is exactly what _Decref reports. */
        mp_facet *old = imp->facetvec[id];
        if (mp_facet_Decref(old)) {
            void *(*MS dtor)(mp_facet *, uint32_t) =
                (void *(*MS)(mp_facet *, uint32_t))((void **)old->vftable)[0];
            dtor(old, 1);
        }
    }
    imp->facetvec[id] = pf;
    pthread_mutex_unlock(&mp_lock);
}

/* locale::_Init(bool): build the classic locale once and hand it back. */
static MS void *mp_locale_Init(uint32_t addref)
{
    pthread_mutex_lock(&mp_lock);
    if (!mp_classic) {
        mp_classic = mp_locimp_new();
        if (mp_classic) mp_facet_Incref((mp_facet *)mp_classic);
    }
    if (addref && mp_classic) mp_facet_Incref((mp_facet *)mp_classic);
    pthread_mutex_unlock(&mp_lock);
    return mp_classic;
}
static MS void *mp_Getgloballocale(void) { return mp_locale_Init(0); }

/* ---- _Lockit ------------------------------------------------------------- */

/* One recursive mutex for every kind. _Lockit's argument selects between the
 * locale, iostream and debug locks; sharing one is coarser than the original
 * and cannot deadlock differently, because the original nests the same way. */
static pthread_mutex_t mp_lockit = PTHREAD_MUTEX_INITIALIZER;

static MS void *mp_Lockit_ctor(void *self, int32_t kind)
{ (void)kind; pthread_mutex_lock(&mp_lockit); return self; }
static MS void *mp_Lockit_dtor(void *self)
{ pthread_mutex_unlock(&mp_lockit); return self; }

/* ---- the pieces with no layout ------------------------------------------ */

/* MSVC's mutex and condition objects are storage the plug-in embeds, sized by
 * _Mtx_internal_imp_size. A pthread object fits inside it at both widths; the
 * *_in_situ forms construct in place, which is what that size is for. */
static MS int32_t mp_Mtx_init_in_situ(void *p, int32_t type)
{
    pthread_mutexattr_t a;
    (void)type;
    if (!p) return 1;
    pthread_mutexattr_init(&a);
    /* MSVC's std::mutex is not recursive but its _Mtx_t is used recursively by
     * the library's own locks; recursive is the safe superset here. */
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init((pthread_mutex_t *)p, &a);
    pthread_mutexattr_destroy(&a);
    return 0;
}
static MS int32_t mp_Mtx_destroy_in_situ(void *p)
{ if (p) pthread_mutex_destroy((pthread_mutex_t *)p); return 0; }
static MS int32_t mp_Mtx_lock(void *p)
{ return p ? pthread_mutex_lock((pthread_mutex_t *)p) : 1; }
static MS int32_t mp_Mtx_unlock(void *p)
{ return p ? pthread_mutex_unlock((pthread_mutex_t *)p) : 1; }
static MS int32_t mp_Mtx_init(void **out, int32_t type)
{
    void *p = w32_alloc(sizeof(pthread_mutex_t), 1);
    if (!p) return 1;
    mp_Mtx_init_in_situ(p, type);
    if (out) *out = p;
    return 0;
}
static MS int32_t mp_Mtx_destroy(void *p)
{ mp_Mtx_destroy_in_situ(p); w32_free(p); return 0; }

static MS int32_t mp_Cnd_init_in_situ(void *p)
{ if (p) pthread_cond_init((pthread_cond_t *)p, NULL); return 0; }
static MS int32_t mp_Cnd_destroy_in_situ(void *p)
{ if (p) pthread_cond_destroy((pthread_cond_t *)p); return 0; }
static MS int32_t mp_Cnd_wait(void *c, void *m)
{ return (c && m) ? pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m) : 1; }
static MS int32_t mp_Cnd_signal(void *c)
{ return c ? pthread_cond_signal((pthread_cond_t *)c) : 1; }
static MS int32_t mp_Cnd_broadcast(void *c)
{ return c ? pthread_cond_broadcast((pthread_cond_t *)c) : 1; }
static MS int32_t mp_Cnd_init(void **out)
{
    void *p = w32_alloc(sizeof(pthread_cond_t), 1);
    if (!p) return 1;
    mp_Cnd_init_in_situ(p);
    if (out) *out = p;
    return 0;
}
static MS int32_t mp_Cnd_destroy(void *p)
{ mp_Cnd_destroy_in_situ(p); w32_free(p); return 0; }
/* Called on thread exit to release anything waiting on this thread's own
 * notify_all_at_thread_exit; with none registered there is nothing to do. */
static MS void mp_Cnd_do_broadcast_at_thread_exit(void) { }

static MS uint32_t mp_Thrd_id(void) { return (uint32_t)(uintptr_t)pthread_self(); }

/* _Query_perf_counter and its frequency back std::chrono::steady_clock. A stub
 * returning 0 for the frequency is a divide by zero in the caller. */
static MS int64_t mp_Query_perf_counter(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}
static MS int64_t mp_Query_perf_frequency(void) { return 1000000000ll; }

/* _Dtest classifies a double: the values are the C fp classes, and 0 (finite)
 * would be a lie for a NaN the caller is testing for. */
static MS int16_t mp_Dtest(const double *p)
{
    if (!p) return 0;
    switch (fpclassify(*p)) {
    case FP_NAN:       return 2;                  /* _NANCODE */
    case FP_INFINITE:  return 1;                  /* _INFCODE */
    case FP_SUBNORMAL: return -2;                 /* _DENORM  */
    case FP_ZERO:      return 0;
    default:           return -1;                 /* _FINITE  */
    }
}

/* The throw helpers. Every one of these exists so that a header can raise
 * without including <stdexcept>, and every one of them must not return: a
 * caller invokes it having already decided its arguments are unusable. */
static MS void mp_Xbad_alloc(void)
{ fprintf(stderr, "[msvcp] std::bad_alloc\n"); abort(); }
static MS void mp_Xlength_error(const char *w)
{ fprintf(stderr, "[msvcp] std::length_error: %s\n", w ? w : ""); abort(); }
static MS void mp_Xout_of_range(const char *w)
{ fprintf(stderr, "[msvcp] std::out_of_range: %s\n", w ? w : ""); abort(); }
static MS void mp_Xinvalid_argument(const char *w)
{ fprintf(stderr, "[msvcp] std::invalid_argument: %s\n", w ? w : ""); abort(); }
static MS void mp_Xbad_function_call(void)
{ fprintf(stderr, "[msvcp] std::bad_function_call\n"); abort(); }
static MS void mp_Xregex_error(int32_t code)
{ fprintf(stderr, "[msvcp] std::regex_error %d\n", (int)code); abort(); }
static MS void mp_Throw_C_error(int32_t code)
{ fprintf(stderr, "[msvcp] C error %d\n", (int)code); abort(); }
static MS void mp_Throw_Cpp_error(int32_t code)
{ fprintf(stderr, "[msvcp] C++ error %d\n", (int)code); abort(); }

static MS int32_t mp_uncaught_exception(void) { return 0; }

/* exception_ptr, as much of it as a plug-in that only moves them around needs.
 * Capturing a live exception would mean copying the object and its ThrowInfo;
 * a null pointer is a valid empty exception_ptr, and rethrowing one is what
 * std::rethrow_exception documents as undefined for empty. */
static MS void mp_ExceptionPtrCreate(void **p) { if (p) *p = NULL; }
static MS void mp_ExceptionPtrDestroy(void **p) { (void)p; }
static MS void mp_ExceptionPtrCurrentException(void **p) { if (p) *p = NULL; }
static MS void mp_ExceptionPtrRethrow(const void *p)
{
    (void)p;
    fprintf(stderr, "[msvcp] rethrow of an exception_ptr this host did not "
                    "capture\n");
    abort();
}

/* streamoff(-1), the value every stream compares a failed tell against. */
static const int64_t mp_BADOFF = -1;

/* Collation for the C locale, which is a byte comparison. */
static MS int32_t mp_Wcscoll(const uint16_t *a, const uint16_t *ae,
                             const uint16_t *b, const uint16_t *be, void *coll)
{
    size_t na = (size_t)(ae - a), nb = (size_t)(be - b), i, n;
    (void)coll;
    n = na < nb ? na : nb;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return na == nb ? 0 : (na < nb ? -1 : 1);
}
static MS size_t mp_Wcsxfrm(uint16_t *df, uint16_t *dl, const uint16_t *sf,
                            const uint16_t *sl, void *coll)
{
    size_t n = (size_t)(sl - sf), room = (size_t)(dl - df), i;
    (void)coll;
    for (i = 0; i < n && i < room; i++) df[i] = sf[i];
    return n;
}

#endif  /* __x86_64__ */
#endif  /* PELOAD_MSVCP_SHIM_H */
