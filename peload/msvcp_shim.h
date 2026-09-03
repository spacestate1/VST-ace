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

/* Recursive, and that is not a preference.
 *
 * _Lockit is a scoped lock the C++ library takes around locale work, and that
 * work nests: building a locale takes it, adding a facet takes it again. A
 * plain mutex deadlocks the second time, on one thread, with no CPU burned --
 * which presents as a plug-in that loads and then simply stops, and looks
 * nothing like a locking bug until you notice the process is in futex_wait
 * with a single thread. The same goes for the locale lock below it. */
static pthread_mutex_t mp_lock;
static pthread_mutex_t mp_lockit;
static pthread_once_t  mp_lock_once = PTHREAD_ONCE_INIT;

static void mp_locks_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mp_lock, &a);
    pthread_mutex_init(&mp_lockit, &a);
    pthread_mutexattr_destroy(&a);
}
static void mp_locks(void) { pthread_once(&mp_lock_once, mp_locks_init); }

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
    mp_locks();
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
    mp_locks();
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

/* One lock for every kind. _Lockit's argument selects between the locale,
 * iostream and debug locks; sharing one is coarser than the original and safe
 * only because it is recursive -- see mp_locks_init. */
static MS void *mp_Lockit_ctor(void *self, int32_t kind)
{ (void)kind; mp_locks(); pthread_mutex_lock(&mp_lockit); return self; }
static MS void *mp_Lockit_dtor(void *self)
{ pthread_mutex_unlock(&mp_lockit); return self; }

/* ---- the containers' debug base ------------------------------------------
 *
 * Every MSVC container derives from _Container_base12, which in a debug build
 * keeps a list of the iterators pointing into it. In a release build -- which
 * is what a shipping plug-in is -- that list is never populated, and the whole
 * family folds to almost nothing. Read out of msvcp120: the constructor is
 * `and QWORD PTR [rcx],0` and a return, and the destructor, _Orphan_all and
 * _Container_base0::_Swap_all are one shared `ret`, which the linker folded
 * together because they are the same instruction.
 *
 * Three of the eight entry points the Native Instruments plug-ins actually
 * reach are in here, and they are the easy three. */
typedef struct { void *proxy; } mp_container_base;

static MS void *mp_cb_ctor(mp_container_base *self)
{ if (self) self->proxy = NULL; return self; }
static MS void mp_cb_noop(void *self) { (void)self; }
/* _Getpfirst: the proxy's iterator list, which is always empty here. */
static MS void *mp_cb_getpfirst(mp_container_base *self)
{ return (self && self->proxy) ? (char *)self->proxy + 8 : NULL; }
static MS void mp_cb_swap(mp_container_base *a, mp_container_base *b)
{ if (a && b) { void *t = a->proxy; a->proxy = b->proxy; b->proxy = t; } }

/* ---- the iostream tier ---------------------------------------------------
 *
 * Enough of it to construct the objects a plug-in builds and to leave them in
 * the state its own inlined code reads back. Not a working iostream: nothing
 * here formats a number or moves a character. That is deliberate -- the three
 * plug-ins that need this construct a stream and one manipulator between them,
 * and never write through it.
 *
 * The layouts are msvcp120's, read with objdump:
 *
 *   basic_streambuf<char>   0x68 bytes. The get and put areas are held twice:
 *                           the pointers themselves at +0x08/+0x10 (first),
 *                           +0x28/+0x30 (next), +0x48/+0x4c (count), and
 *                           *pointers to those* at +0x18/+0x20, +0x38/+0x40,
 *                           +0x50/+0x58, which is how a derived streambuf
 *                           shares them. The locale is at +0x60.
 *
 *   ios_base                +0x18 _Fmtfl, initialised to 0x201; +0x20 _Prec,
 *                           to 6; +0x08, +0x14, +0x28, +0x30, +0x38, +0x40
 *                           zeroed. basic_ios adds _Mystrbuf at +0x48 and
 *                           _Tiestr at +0x50.
 *
 *   basic_iostream<char>    has virtual bases, so it is laid out through
 *                           vbtables rather than by offset: a vbptr at +0x00
 *                           whose table is {0, 32}, a second at +0x10 whose
 *                           table is {0, 16}, and the basic_ios subobject at
 *                           +0x20 -- which is where both of those point, 0+32
 *                           and 0x10+16. The constructor writes both vbptrs,
 *                           the vftable at the virtual base, zeroes the
 *                           istream character count at +0x08, and calls
 *                           basic_ios::init.
 *
 * Getting a vbtable wrong is not a subtle failure: the plug-in's own inlined
 * member access computes the subobject address from it, so every read after
 * that lands somewhere else entirely. They are reproduced exactly. */

/* The vbtables, which must exist as data the object can point at. */
static const int32_t mp_vbtable_iostream[2] = { 0, 32 };
static const int32_t mp_vbtable_ostream[2]  = { 0, 16 };

typedef struct {
    void    *vftable;          /* +0x00 */
    void    *pad08;            /* +0x08 */
    int32_t  state;            /* +0x10  _Mystate */
    int32_t  except;           /* +0x14  _Except  */
    int32_t  fmtfl;            /* +0x18 */
    int32_t  pad1c;
    int64_t  prec;             /* +0x20 */
    int64_t  wide;             /* +0x28 */
    void    *pad30, *pad38, *pad40;
    void    *strbuf;           /* +0x48  _Mystrbuf */
    void    *tiestr;           /* +0x50  _Tiestr   */
    int32_t  fillch;           /* +0x58 */
    int32_t  pad5c;
} mp_ios;

static MS void *mp_ios_ctor(mp_ios *self);

/* ios_base::_Init, with the constants the real one writes. */
static void mp_ios_init_fields(mp_ios *b)
{
    if (!b) return;
    b->pad08 = NULL;
    b->except = 0;
    b->fmtfl = 0x201;                     /* skipws | dec */
    b->prec = 6;
    b->wide = 0;
    b->pad30 = b->pad38 = b->pad40 = NULL;
    /* The real one asks the locale for a ctype and widens a space to find the
     * fill character. It is a space either way, and going through a facet to
     * discover that would mean building one. */
    b->fillch = ' ';
}

static MS void *mp_basic_ios_ctor(mp_ios *self)
{
    /* The protected default constructor sets the vftable and nothing else --
     * two instructions in the original. init() does the rest, later. */
    if (self) self->vftable = mp_facet_vft;
    return self;
}

static MS void *mp_basic_ios_init(mp_ios *self, void *sb, uint32_t isstd)
{
    (void)isstd;
    if (!self) return self;
    mp_ios_init_fields(self);
    self->strbuf = sb;
    self->tiestr = NULL;
    /* clear(), which init calls last: good if there is a buffer, bad if not. */
    self->state = sb ? 0 : 4;                     /* goodbit / badbit */
    return self;
}

/* The ostream suffix -- flush if unitbuf is set, which it is not here -- and
 * the streambuf lock, which guards a buffer nothing else touches. */
static MS void mp_ostream_osfx(void *self) { (void)self; }
static MS void mp_sb_lock(void *self)   { (void)self; }
static MS void mp_sb_unlock(void *self) { (void)self; }

/* Destructors for the three. Each object's storage belongs to whoever built
 * it -- these are all embedded in a plug-in's own object -- so there is
 * nothing here to release. */
static MS void *mp_stream_dtor(void *self, uint32_t deleting)
{ (void)deleting; return self; }

/* basic_streambuf<char>, for real.
 *
 * The get and put areas are held the way MSVC holds them, which is not the way
 * the standard describes them: a first pointer, a next pointer and a *count* of
 * characters remaining -- there is no end pointer -- and then a second set of
 * pointers *to* those three, which is what every accessor actually reads
 * through. The indirection is how a derived streambuf can point the base at
 * storage of its own, and a plug-in with its own streambuf does exactly that,
 * so reading the direct fields instead would see stale values.
 *
 *   egptr() is gptr() + *_IGcount, not a stored pointer. */
typedef struct {
    char    *first, *pfirst;      /* +0x08, +0x10  _Gfirst, _Pfirst */
    char   **ifirst, **ipfirst;   /* +0x18, +0x20  _IGfirst, _IPfirst */
    char    *next, *pnext;        /* +0x28, +0x30  _Gnext, _Pnext */
    char   **inext, **ipnext;     /* +0x38, +0x40  _IGnext, _IPnext */
    int32_t  gcount, pcount;      /* +0x48, +0x4c  _Gcount, _Pcount */
    int32_t *igcount, *ipcount;   /* +0x50, +0x58  _IGcount, _IPcount */
    void    *locale;              /* +0x60 */
} mp_streambuf_body;

#define MP_SB(self) ((mp_streambuf_body *)((char *)(self) + 8))
#define MP_EOF (-1)

static char *mp_sb_gptr(void *sb)
{ mp_streambuf_body *b = MP_SB(sb); return b->inext ? *b->inext : NULL; }
static char *mp_sb_pptr(void *sb)
{ mp_streambuf_body *b = MP_SB(sb); return b->ipnext ? *b->ipnext : NULL; }
static int32_t mp_sb_gcount(void *sb)
{ mp_streambuf_body *b = MP_SB(sb); return b->igcount ? *b->igcount : 0; }
static int32_t mp_sb_pcount(void *sb)
{ mp_streambuf_body *b = MP_SB(sb); return b->ipcount ? *b->ipcount : 0; }

/* The virtual slots, in the order msvcp120's own vftable has them -- confirmed
 * by which entries the linker folded together: _Unlock with _Lock, pbackfail
 * and underflow with overflow, seekpos with seekoff, sync with showmanyc, and
 * imbue with _Lock. Calling one of these on a plug-in's own streambuf has to
 * land on its override, so the numbering is not negotiable. */
enum {
    MP_SB_DTOR = 0, MP_SB_LOCK, MP_SB_UNLOCK, MP_SB_OVERFLOW, MP_SB_PBACKFAIL,
    MP_SB_SHOWMANYC, MP_SB_UNDERFLOW, MP_SB_UFLOW, MP_SB_XSGETN, MP_SB_XSPUTN,
    MP_SB_SEEKOFF, MP_SB_SEEKPOS, MP_SB_SETBUF, MP_SB_SYNC, MP_SB_IMBUE,
    MP_SB_NSLOTS
};

static void *mp_sb_slot(void *sb, int n)
{
    void **vft = sb ? *(void ***)sb : NULL;
    return vft ? vft[n] : NULL;
}

/* setg and setp, which a derived streambuf calls to publish its buffer. Note
 * that the count is what is stored, so setg's third argument -- the end -- is
 * turned into one here rather than kept. */
static MS void mp_sb_setg(void *self, char *gbeg, char *gnext, char *gend)
{
    mp_streambuf_body *b;
    if (!self) return;
    b = MP_SB(self);
    if (b->ifirst)  *b->ifirst  = gbeg;
    if (b->inext)   *b->inext   = gnext;
    if (b->igcount) *b->igcount = (int32_t)(gend - gnext);
}
static MS void mp_sb_setp(void *self, char *pbeg, char *pend)
{
    mp_streambuf_body *b;
    if (!self) return;
    b = MP_SB(self);
    if (b->ipfirst) *b->ipfirst = pbeg;
    if (b->ipnext)  *b->ipnext  = pbeg;
    if (b->ipcount) *b->ipcount = (int32_t)(pend - pbeg);
}
static MS char *mp_sb_eback(void *self)
{ mp_streambuf_body *b = MP_SB(self); return b->ifirst ? *b->ifirst : NULL; }
static MS char *mp_sb_gptr_pub(void *self)  { return mp_sb_gptr(self); }
static MS char *mp_sb_pptr_pub(void *self)  { return mp_sb_pptr(self); }
static MS char *mp_sb_egptr(void *self)
{ char *g = mp_sb_gptr(self); return g ? g + mp_sb_gcount(self) : NULL; }
static MS char *mp_sb_epptr(void *self)
{ char *p = mp_sb_pptr(self); return p ? p + mp_sb_pcount(self) : NULL; }
static MS void mp_sb_gbump(void *self, int32_t n)
{
    mp_streambuf_body *b = MP_SB(self);
    if (b->inext)   *b->inext   += n;
    if (b->igcount) *b->igcount -= n;
}
static MS void mp_sb_pbump(void *self, int32_t n)
{
    mp_streambuf_body *b = MP_SB(self);
    if (b->ipnext)  *b->ipnext  += n;
    if (b->ipcount) *b->ipcount -= n;
}

/* The default virtuals. A base streambuf has no buffer of its own, so the ones
 * that would move a character report failure -- which is what the real ones do
 * and what a derived class overrides. */
static MS int32_t mp_sb_v_overflow(void *self, int32_t c)  { (void)self;(void)c; return MP_EOF; }
static MS int32_t mp_sb_v_underflow(void *self)            { (void)self; return MP_EOF; }
static MS int32_t mp_sb_v_pbackfail(void *self, int32_t c) { (void)self;(void)c; return MP_EOF; }
static MS int64_t mp_sb_v_showmanyc(void *self)            { (void)self; return 0; }
static MS int32_t mp_sb_v_sync(void *self)                 { (void)self; return 0; }
static MS void   *mp_sb_v_setbuf(void *self, char *p, int64_t n)
{ (void)p; (void)n; return self; }
static MS void    mp_sb_v_imbue(void *self, const void *loc) { (void)self;(void)loc; }
static MS int64_t mp_sb_v_seekoff(void *self, int64_t off, int32_t way, int32_t which)
{ (void)self;(void)off;(void)way;(void)which; return -1; }

/* uflow: take the character under the get pointer and advance. The default
 * fetches through underflow, which a base streambuf cannot do. */
static MS int32_t mp_sb_v_uflow(void *self)
{
    int32_t (MS *underflow)(void *) = (int32_t (MS *)(void *))mp_sb_slot(self, MP_SB_UNDERFLOW);
    int32_t c;
    if (!underflow) return MP_EOF;
    c = underflow(self);
    if (c != MP_EOF) mp_sb_gbump(self, 1);
    return c;
}

static MS int64_t mp_sb_v_xsgetn(void *self, char *out, int64_t n)
{
    int64_t done = 0;
    while (done < n) {
        int32_t avail = mp_sb_gcount(self);
        if (avail > 0) {
            int64_t take = (n - done) < avail ? (n - done) : avail;
            memcpy(out + done, mp_sb_gptr(self), (size_t)take);
            mp_sb_gbump(self, (int32_t)take);
            done += take;
        } else {
            int32_t (MS *uflow)(void *) =
                (int32_t (MS *)(void *))mp_sb_slot(self, MP_SB_UFLOW);
            int32_t c = uflow ? uflow(self) : MP_EOF;
            if (c == MP_EOF) break;
            out[done++] = (char)c;
        }
    }
    return done;
}

static MS int64_t mp_sb_v_xsputn(void *self, const char *in, int64_t n)
{
    int64_t done = 0;
    while (done < n) {
        int32_t room = mp_sb_pcount(self);
        if (room > 0) {
            int64_t put = (n - done) < room ? (n - done) : room;
            memcpy(mp_sb_pptr(self), in + done, (size_t)put);
            mp_sb_pbump(self, (int32_t)put);
            done += put;
        } else {
            int32_t (MS *overflow)(void *, int32_t) =
                (int32_t (MS *)(void *, int32_t))mp_sb_slot(self, MP_SB_OVERFLOW);
            if (!overflow || overflow(self, (unsigned char)in[done]) == MP_EOF) break;
            done++;
        }
    }
    return done;
}

/* The public members, which dispatch to whichever override the object has. */
static MS int64_t mp_sb_sputn(void *self, const char *in, int64_t n)
{
    int64_t (MS *xsputn)(void *, const char *, int64_t) =
        (int64_t (MS *)(void *, const char *, int64_t))mp_sb_slot(self, MP_SB_XSPUTN);
    return xsputn ? xsputn(self, in, n) : 0;
}
static MS int64_t mp_sb_sgetn(void *self, char *out, int64_t n)
{
    int64_t (MS *xsgetn)(void *, char *, int64_t) =
        (int64_t (MS *)(void *, char *, int64_t))mp_sb_slot(self, MP_SB_XSGETN);
    return xsgetn ? xsgetn(self, out, n) : 0;
}
static MS int32_t mp_sb_sbumpc(void *self)
{
    if (mp_sb_gcount(self) > 0) {
        int32_t c = (unsigned char)*mp_sb_gptr(self);
        mp_sb_gbump(self, 1);
        return c;
    }
    { int32_t (MS *uflow)(void *) = (int32_t (MS *)(void *))mp_sb_slot(self, MP_SB_UFLOW);
      return uflow ? uflow(self) : MP_EOF; }
}
static MS int32_t mp_sb_sgetc(void *self)
{
    if (mp_sb_gcount(self) > 0) return (unsigned char)*mp_sb_gptr(self);
    { int32_t (MS *underflow)(void *) =
        (int32_t (MS *)(void *))mp_sb_slot(self, MP_SB_UNDERFLOW);
      return underflow ? underflow(self) : MP_EOF; }
}
static MS int32_t mp_sb_snextc(void *self)
{ return mp_sb_sbumpc(self) == MP_EOF ? MP_EOF : mp_sb_sgetc(self); }
static MS int32_t mp_sb_sputc(void *self, int32_t c)
{
    if (mp_sb_pcount(self) > 0) {
        *mp_sb_pptr(self) = (char)c;
        mp_sb_pbump(self, 1);
        return c;
    }
    { int32_t (MS *overflow)(void *, int32_t) =
        (int32_t (MS *)(void *, int32_t))mp_sb_slot(self, MP_SB_OVERFLOW);
      return overflow ? overflow(self, c) : MP_EOF; }
}
static MS int32_t mp_sb_pubsync(void *self)
{
    int32_t (MS *sync)(void *) = (int32_t (MS *)(void *))mp_sb_slot(self, MP_SB_SYNC);
    return sync ? sync(self) : 0;
}
static MS void *mp_sb_getloc(void *self, void *out)
{
    mp_streambuf_body *b = self ? MP_SB(self) : NULL;
    if (out) {
        *(void **)out = b ? b->locale : NULL;
        /* It returns the locale *by value*, and the copy the caller destroys
         * will decref. Building the returned object here rather than through
         * the copy constructor means taking that reference by hand: without it
         * the _Locimp is released once too often and the next use of it reads a
         * freed object through a vftable that is no longer there. */
        mp_facet_Incref((mp_facet *)*(void **)out);
    }
    return out;
}

/* The pointer-stepping accessors a derived streambuf uses to walk its own
 * buffer. _Gninc returns the current get pointer and then advances; _Gnpreinc
 * advances first and then returns; _Gndec steps back. They differ only in when
 * the step happens, and a plug-in reading a character through the wrong one is
 * off by one for the rest of the stream. */
static MS char *mp_sb_pbase(void *self)
{ mp_streambuf_body *b = MP_SB(self); return b->ipfirst ? *b->ipfirst : NULL; }
static MS char *mp_sb_gninc(void *self)
{ char *g = mp_sb_gptr(self); mp_sb_gbump(self, 1); return g; }
static MS char *mp_sb_gnpreinc(void *self)
{ mp_sb_gbump(self, 1); return mp_sb_gptr(self); }
static MS char *mp_sb_gndec(void *self)
{ mp_sb_gbump(self, -1); return mp_sb_gptr(self); }
static MS char *mp_sb_pninc(void *self)
{ char *p = mp_sb_pptr(self); mp_sb_pbump(self, 1); return p; }

/* The rest of the public members. in_avail answers from the get area without
 * touching the buffer, and sungetc steps back into it. */
static MS int64_t mp_sb_in_avail(void *self)
{
    int32_t n = mp_sb_gcount(self);
    if (n > 0) return n;
    { int64_t (MS *showmanyc)(void *) =
        (int64_t (MS *)(void *))mp_sb_slot(self, MP_SB_SHOWMANYC);
      return showmanyc ? showmanyc(self) : 0; }
}
static MS int32_t mp_sb_sungetc(void *self)
{
    mp_streambuf_body *b = MP_SB(self);
    char *g = mp_sb_gptr(self), *first = b->ifirst ? *b->ifirst : NULL;
    if (g && first && g > first) { mp_sb_gbump(self, -1); return (unsigned char)*mp_sb_gptr(self); }
    { int32_t (MS *pbackfail)(void *, int32_t) =
        (int32_t (MS *)(void *, int32_t))mp_sb_slot(self, MP_SB_PBACKFAIL);
      return pbackfail ? pbackfail(self, MP_EOF) : MP_EOF; }
}
static MS void *mp_sb_pubsetbuf(void *self, char *b, int64_t n)
{
    void *(MS *setbuf)(void *, char *, int64_t) =
        (void *(MS *)(void *, char *, int64_t))mp_sb_slot(self, MP_SB_SETBUF);
    return setbuf ? setbuf(self, b, n) : self;
}
static MS void *mp_sb_pubimbue(void *self, void *out, const void *loc)
{
    void (MS *imbue)(void *, const void *) =
        (void (MS *)(void *, const void *))mp_sb_slot(self, MP_SB_IMBUE);
    mp_streambuf_body *b = self ? MP_SB(self) : NULL;
    if (out) *(void **)out = b ? b->locale : NULL;   /* the previous one */
    if (imbue) imbue(self, loc);
    return out;
}
static MS void *mp_sb_pubseekoff(void *self, void *out, int64_t off,
                                 int32_t way, int32_t which)
{
    int64_t (MS *seekoff)(void *, int64_t, int32_t, int32_t) =
        (int64_t (MS *)(void *, int64_t, int32_t, int32_t))mp_sb_slot(self, MP_SB_SEEKOFF);
    int64_t r = seekoff ? seekoff(self, off, way, which) : -1;
    /* fpos<int> is returned through a hidden pointer: the offset, then the
     * state and the file position, which a memory stream does not have. */
    if (out) { memset(out, 0, 24); *(int64_t *)out = r; }
    return out;
}

static void *mp_streambuf_vft[MP_SB_NSLOTS] = {
    (void *)mp_stream_dtor, (void *)mp_sb_lock, (void *)mp_sb_unlock,
    (void *)mp_sb_v_overflow, (void *)mp_sb_v_pbackfail,
    (void *)mp_sb_v_showmanyc, (void *)mp_sb_v_underflow, (void *)mp_sb_v_uflow,
    (void *)mp_sb_v_xsgetn, (void *)mp_sb_v_xsputn,
    (void *)mp_sb_v_seekoff, (void *)mp_sb_v_seekoff,
    (void *)mp_sb_v_setbuf, (void *)mp_sb_v_sync, (void *)mp_sb_v_imbue
};

static MS void *mp_streambuf_ctor(void *self)
{
    char *p = (char *)self;
    mp_streambuf_body *b;

    if (!self) return self;
    *(void **)p = mp_streambuf_vft;                /* +0x00 vftable */
    b = (mp_streambuf_body *)(p + 8);
    b->first = b->pfirst = b->next = b->pnext = NULL;
    b->gcount = b->pcount = 0;
    b->ifirst  = &b->first;   b->ipfirst = &b->pfirst;
    b->inext   = &b->next;    b->ipnext  = &b->pnext;
    b->igcount = &b->gcount;  b->ipcount = &b->pcount;
    b->locale  = mp_locale_Init(1);
    return self;
}

/* basic_iostream<char>::basic_iostream(basic_streambuf *). The third argument
 * is the flag MSVC passes to the constructor of a class with virtual bases,
 * saying whether this is the most derived object and therefore whether the
 * virtual base is this constructor's to set up. */
static MS void *mp_iostream_ctor(void *self, void *sb, uint32_t most_derived)
{
    char *p = (char *)self;
    mp_ios *ios;

    if (!self) return self;
    if (most_derived) {
        *(const void **)(p + 0x00) = mp_vbtable_iostream;
        *(const void **)(p + 0x10) = mp_vbtable_ostream;
    }
    /* Both vbtables put the virtual base at +0x20; taking it from the table
     * rather than assuming keeps this right if a caller laid it out for a
     * different derived class. */
    {
        const int32_t *vb = *(const int32_t **)(p + 0x00);
        int32_t off = vb ? vb[1] : 32;
        ios = (mp_ios *)(p + off);
        *(int32_t *)(p + off - 4) = 0;
    }
    *(void **)(p + 0x08) = NULL;                   /* the istream count */
    mp_basic_ios_ctor(ios);
    mp_basic_ios_init(ios, sb, 0);
    return self;
}

/* basic_ostream<T>::basic_ostream(basic_streambuf *, bool isstd).
 *
 * One virtual base rather than the iostream's two: a vbptr at +0x00 whose
 * table is {0, 16}, so the basic_ios subobject sits at +0x10. The most-derived
 * flag is the fourth argument here -- after the bool -- rather than the third,
 * which is where basic_iostream's is; MSVC appends it, so its position moves
 * with the constructor's own parameter count. Getting that wrong means testing
 * a register that holds something else and initialising the virtual base twice
 * or never. */
static MS void *mp_ostream_ctor(void *self, void *sb, uint32_t isstd,
                                uint32_t most_derived)
{
    char *p = (char *)self;
    const int32_t *vb;
    int32_t off;

    if (!self) return self;
    if (most_derived) {
        *(const void **)(p + 0x00) = mp_vbtable_ostream;
        *(void **)(p + 0x10) = mp_facet_vft;
    }
    vb = *(const int32_t **)(p + 0x00);
    off = vb ? vb[1] : 16;
    *(int32_t *)(p + off - 4) = off - 0x10;
    mp_basic_ios_ctor((mp_ios *)(p + off));
    mp_basic_ios_init((mp_ios *)(p + off), sb, isstd);
    return self;
}

/* basic_istream is the same shape; its extra field is the character count it
 * keeps for gcount(), which init leaves at zero. */
static MS void *mp_istream_ctor(void *self, void *sb, uint32_t isstd,
                                uint32_t most_derived)
{
    char *p = (char *)self;
    void *r = mp_ostream_ctor(self, sb, isstd, most_derived);
    if (self) *(void **)(p + 0x08) = NULL;
    return r;
}

/* ostream << manipulator. The manipulator takes and returns an ios_base&, and
 * the operator hands it the stream's ios_base subobject and returns the stream.
 * endl and flush are what arrive here; with nothing buffered there is nothing
 * for either to do beyond what the manipulator itself does. */
static MS void *mp_ostream_manip(void *self, void *(MS *fn)(void *))
{
    if (self && fn) {
        const int32_t *vb = *(const int32_t **)self;
        int32_t off = vb ? vb[1] : 32;
        fn((char *)self + off);
    }
    return self;
}

/* What a stream does once it has been built.
 *
 * Not an implementation of iostreams: no character moves through any of these.
 * The point is that a caller's loop ends. A plug-in reading from a stream this
 * host did not fill will loop until it is told the stream is exhausted, so an
 * extraction that quietly does nothing turns a crash into a hang -- which is
 * the worse of the two for a host scanning a folder. Reporting eof and fail is
 * both true and terminating: the stream really has nothing in it.
 *
 * eofbit is 1, failbit 2, badbit 4. */
static MS void mp_ios_setstate(mp_ios *self, int32_t st, uint32_t reraise)
{ (void)reraise; if (self) self->state |= st; }
static MS int32_t mp_ios_rdstate(mp_ios *self) { return self ? self->state : 4; }
static MS void mp_ios_clear(mp_ios *self, int32_t st, uint32_t reraise)
{ (void)reraise; if (self) self->state = st; }

/* std::chrono's tick source inside the C++ library, in 100ns units. Zero here
 * is the same lie _Query_perf_counter would be: a clock that never advances. */
static MS int64_t mp_Xtime_get_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 10000000ll + t.tv_nsec / 100;
}

/* ---- basic_streambuf<wchar_t> --------------------------------------------
 *
 * The same object as the narrow one -- the fields are pointers and counts
 * either way -- but the pointers step in two-byte elements, so gbump and pbump
 * cannot be shared. Windows' wchar_t is 16 bits. */
static MS void *mp_wstreambuf_ctor(void *self)
{
    char *p = (char *)self;
    mp_streambuf_body *b;

    if (!self) return self;
    *(void **)p = mp_streambuf_vft;
    b = MP_SB(p);
    b->first = b->pfirst = b->next = b->pnext = NULL;
    b->gcount = b->pcount = 0;
    b->ifirst  = &b->first;   b->ipfirst = &b->pfirst;
    b->inext   = &b->next;    b->ipnext  = &b->pnext;
    b->igcount = &b->gcount;  b->ipcount = &b->pcount;
    b->locale  = mp_locale_Init(1);
    return self;
}
static MS void mp_wsb_setg(void *self, uint16_t *gbeg, uint16_t *gnext, uint16_t *gend)
{
    mp_streambuf_body *b;
    if (!self) return;
    b = MP_SB(self);
    if (b->ifirst)  *b->ifirst  = (char *)gbeg;
    if (b->inext)   *b->inext   = (char *)gnext;
    if (b->igcount) *b->igcount = (int32_t)(gend - gnext);
}
static MS void mp_wsb_setp(void *self, uint16_t *pbeg, uint16_t *pend)
{
    mp_streambuf_body *b;
    if (!self) return;
    b = MP_SB(self);
    if (b->ipfirst) *b->ipfirst = (char *)pbeg;
    if (b->ipnext)  *b->ipnext  = (char *)pbeg;
    if (b->ipcount) *b->ipcount = (int32_t)(pend - pbeg);
}

/* ---- ctype<wchar_t> ------------------------------------------------------
 *
 * The facet a wide stream asks the locale for, and the first one msvcp builds
 * for itself rather than leaving to the plug-in's inlined use_facet -- which is
 * why the locale shim's empty facet vector is not enough here.
 *
 * 96 bytes, from the allocation its _Getcat makes, and fifteen virtuals in the
 * order its vftable has them: destructor, _Incref, _Decref, then each of is,
 * scan_is, scan_not, tolower, toupper, widen and narrow with the range form
 * before the single-character one. _Getcat returns 2, which is the ctype
 * category.
 *
 * The classifications are the C locale's, which is the locale this host has. */
#define MP_CT_UPPER 0x01
#define MP_CT_LOWER 0x02
#define MP_CT_DIGIT 0x04
#define MP_CT_SPACE 0x08
#define MP_CT_PUNCT 0x10
#define MP_CT_CNTRL 0x20
#define MP_CT_BLANK 0x40
#define MP_CT_HEX   0x80
#define MP_CT_ALPHA 0x103

static int16_t mp_ctype_mask(uint32_t c)
{
    int16_t m = 0;
    if (c > 0xFF) return (int16_t)(iswalpha((wint_t)c) ? MP_CT_ALPHA : 0);
    if (isupper((int)c)) m |= MP_CT_UPPER;
    if (islower((int)c)) m |= MP_CT_LOWER;
    if (isdigit((int)c)) m |= MP_CT_DIGIT;
    if (isspace((int)c)) m |= MP_CT_SPACE;
    if (ispunct((int)c)) m |= MP_CT_PUNCT;
    if (iscntrl((int)c)) m |= MP_CT_CNTRL;
    if (c == ' ' || c == '\t') m |= MP_CT_BLANK;
    if (isxdigit((int)c)) m |= MP_CT_HEX;
    if (isalpha((int)c)) m |= MP_CT_ALPHA;
    return m;
}

static MS int32_t mp_ctw_is_ch(void *self, int16_t mask, uint16_t ch)
{ (void)self; return (mp_ctype_mask(ch) & mask) != 0; }
static MS const uint16_t *mp_ctw_is_range(void *self, const uint16_t *first,
                                          const uint16_t *last, int16_t *out)
{
    (void)self;
    while (first != last) *out++ = mp_ctype_mask(*first++);
    return last;
}
static MS const uint16_t *mp_ctw_scan_is(void *self, int16_t mask,
                                         const uint16_t *first, const uint16_t *last)
{
    (void)self;
    while (first != last && !(mp_ctype_mask(*first) & mask)) first++;
    return first;
}
static MS const uint16_t *mp_ctw_scan_not(void *self, int16_t mask,
                                          const uint16_t *first, const uint16_t *last)
{
    (void)self;
    while (first != last && (mp_ctype_mask(*first) & mask)) first++;
    return first;
}
static MS uint16_t mp_ctw_tolower_ch(void *self, uint16_t c)
{ (void)self; return (uint16_t)towlower((wint_t)c); }
static MS const uint16_t *mp_ctw_tolower_range(void *self, uint16_t *first,
                                               const uint16_t *last)
{
    (void)self;
    while (first != (uint16_t *)last) { *first = (uint16_t)towlower((wint_t)*first); first++; }
    return last;
}
static MS uint16_t mp_ctw_toupper_ch(void *self, uint16_t c)
{ (void)self; return (uint16_t)towupper((wint_t)c); }
static MS const uint16_t *mp_ctw_toupper_range(void *self, uint16_t *first,
                                               const uint16_t *last)
{
    (void)self;
    while (first != (uint16_t *)last) { *first = (uint16_t)towupper((wint_t)*first); first++; }
    return last;
}
static MS uint16_t mp_ctw_widen_ch(void *self, char c)
{ (void)self; return (uint16_t)(unsigned char)c; }
static MS const char *mp_ctw_widen_range(void *self, const char *first,
                                         const char *last, uint16_t *out)
{
    (void)self;
    while (first != last) *out++ = (uint16_t)(unsigned char)*first++;
    return last;
}
static MS char mp_ctw_narrow_ch(void *self, uint16_t c, char dflt)
{ (void)self; return c < 0x100 ? (char)c : dflt; }
static MS const uint16_t *mp_ctw_narrow_range(void *self, const uint16_t *first,
                                              const uint16_t *last, char dflt, char *out)
{
    (void)self;
    while (first != last) { *out++ = *first < 0x100 ? (char)*first : dflt; first++; }
    return last;
}

static void *mp_ctypew_vft[15] = {
    (void *)mp_facet_dtor, (void *)mp_facet_Incref, (void *)mp_facet_Decref,
    (void *)mp_ctw_is_range,      (void *)mp_ctw_is_ch,
    (void *)mp_ctw_scan_is,       (void *)mp_ctw_scan_not,
    (void *)mp_ctw_tolower_range, (void *)mp_ctw_tolower_ch,
    (void *)mp_ctw_toupper_range, (void *)mp_ctw_toupper_ch,
    (void *)mp_ctw_widen_range,   (void *)mp_ctw_widen_ch,
    (void *)mp_ctw_narrow_range,  (void *)mp_ctw_narrow_ch
};

/* The public members. Each is a one-line dispatch to its virtual, and each has
 * to go through the vftable rather than call our implementation directly: a
 * plug-in may have installed a ctype of its own, and the public member is how
 * its override gets reached. */
enum {
    MP_CT_DTOR = 0, MP_CT_INCREF, MP_CT_DECREF,
    MP_CT_IS_RANGE, MP_CT_IS_CH, MP_CT_SCAN_IS, MP_CT_SCAN_NOT,
    MP_CT_TOLOWER_RANGE, MP_CT_TOLOWER_CH, MP_CT_TOUPPER_RANGE, MP_CT_TOUPPER_CH,
    MP_CT_WIDEN_RANGE, MP_CT_WIDEN_CH, MP_CT_NARROW_RANGE, MP_CT_NARROW_CH
};
static void *mp_ct_slot(void *self, int n)
{ void **v = self ? *(void ***)self : NULL; return v ? v[n] : NULL; }

static MS uint16_t mp_ctw_widen(void *self, char c)
{
    uint16_t (MS *f)(void *, char) =
        (uint16_t (MS *)(void *, char))mp_ct_slot(self, MP_CT_WIDEN_CH);
    return f ? f(self, c) : (uint16_t)(unsigned char)c;
}
static MS const char *mp_ctw_widen_r(void *self, const char *a, const char *b, uint16_t *o)
{
    const char *(MS *f)(void *, const char *, const char *, uint16_t *) =
        (const char *(MS *)(void *, const char *, const char *, uint16_t *))
        mp_ct_slot(self, MP_CT_WIDEN_RANGE);
    return f ? f(self, a, b, o) : b;
}
static MS char mp_ctw_narrow(void *self, uint16_t c, char d)
{
    char (MS *f)(void *, uint16_t, char) =
        (char (MS *)(void *, uint16_t, char))mp_ct_slot(self, MP_CT_NARROW_CH);
    return f ? f(self, c, d) : (c < 0x100 ? (char)c : d);
}
static MS const uint16_t *mp_ctw_narrow_r(void *self, const uint16_t *a,
                                          const uint16_t *b, char d, char *o)
{
    const uint16_t *(MS *f)(void *, const uint16_t *, const uint16_t *, char, char *) =
        (const uint16_t *(MS *)(void *, const uint16_t *, const uint16_t *, char, char *))
        mp_ct_slot(self, MP_CT_NARROW_RANGE);
    return f ? f(self, a, b, d, o) : b;
}
static MS int32_t mp_ctw_is(void *self, int16_t m, uint16_t c)
{
    int32_t (MS *f)(void *, int16_t, uint16_t) =
        (int32_t (MS *)(void *, int16_t, uint16_t))mp_ct_slot(self, MP_CT_IS_CH);
    return f ? f(self, m, c) : 0;
}
static MS const uint16_t *mp_ctw_is_r(void *self, const uint16_t *a,
                                      const uint16_t *b, int16_t *o)
{
    const uint16_t *(MS *f)(void *, const uint16_t *, const uint16_t *, int16_t *) =
        (const uint16_t *(MS *)(void *, const uint16_t *, const uint16_t *, int16_t *))
        mp_ct_slot(self, MP_CT_IS_RANGE);
    return f ? f(self, a, b, o) : b;
}
static MS uint16_t mp_ctw_tolower(void *self, uint16_t c)
{
    uint16_t (MS *f)(void *, uint16_t) =
        (uint16_t (MS *)(void *, uint16_t))mp_ct_slot(self, MP_CT_TOLOWER_CH);
    return f ? f(self, c) : c;
}
static MS uint16_t mp_ctw_toupper(void *self, uint16_t c)
{
    uint16_t (MS *f)(void *, uint16_t) =
        (uint16_t (MS *)(void *, uint16_t))mp_ct_slot(self, MP_CT_TOUPPER_CH);
    return f ? f(self, c) : c;
}
static MS const uint16_t *mp_ctw_scan_is_p(void *self, int16_t m,
                                           const uint16_t *a, const uint16_t *b)
{
    const uint16_t *(MS *f)(void *, int16_t, const uint16_t *, const uint16_t *) =
        (const uint16_t *(MS *)(void *, int16_t, const uint16_t *, const uint16_t *))
        mp_ct_slot(self, MP_CT_SCAN_IS);
    return f ? f(self, m, a, b) : b;
}
static MS const uint16_t *mp_ctw_scan_not_p(void *self, int16_t m,
                                            const uint16_t *a, const uint16_t *b)
{
    const uint16_t *(MS *f)(void *, int16_t, const uint16_t *, const uint16_t *) =
        (const uint16_t *(MS *)(void *, int16_t, const uint16_t *, const uint16_t *))
        mp_ct_slot(self, MP_CT_SCAN_NOT);
    return f ? f(self, m, a, b) : b;
}

/* ctype<wchar_t>::_Getcat(const facet **ppf, const locale *ploc): build the
 * facet if the caller has not got one, and answer with the category it belongs
 * to. Two is ctype's. */
static MS size_t mp_ctypew_Getcat(const void **ppf, const void *ploc)
{
    (void)ploc;
    if (ppf && !*ppf) {
        mp_facet *f = (mp_facet *)w32_alloc(96, 1);   /* the size its own _Getcat allocates */
        if (f) { f->vftable = mp_ctypew_vft; f->ref = 1; }
        *ppf = f;
    }
    return 2;
}

/* ---- ctype<char> --------------------------------------------------------- */

/* Its _Getcat allocates 0x30 bytes and copies a 32-byte _Ctypevec from
 * _Locinfo::_Getctype() into +0x10, so the object is facet(16) + _Ctypevec(32).
 * table() is `mov rax,[rcx+0x18]; ret`, which puts _Ctypevec::_Table at +0x18 --
 * and that matters, because ctype<char>::is() is not virtual: callers index the
 * table themselves, so the pointer has to lead somewhere real.
 *
 * The table is indexed by the char, which may be signed, so the array carries
 * 128 entries below the pointer we hand out and the classic 256 above it. */

#define MP_CTC_LOW 128
static int16_t mp_ctc_storage[MP_CTC_LOW + 256];
static int16_t *mp_ctc_table;

static void mp_ctc_build(void)
{
    int i;
    if (mp_ctc_table) return;
    for (i = 0; i < 256; i++)
        mp_ctc_storage[MP_CTC_LOW + i] = mp_ctype_mask((uint32_t)i);
    for (i = 0; i < MP_CTC_LOW; i++)                  /* chars read as negative */
        mp_ctc_storage[i] = mp_ctc_storage[MP_CTC_LOW + 128 + i];
    mp_ctc_table = mp_ctc_storage + MP_CTC_LOW;
}
static MS const int16_t *mp_ctc_classic_table(void)
{ mp_ctc_build(); return mp_ctc_table; }

/* The eleven virtuals, in the order the real vftable at 0x55e50 holds them.
 * ctype<char> has no do_is or do_scan_is: those are inline over the table. */
static MS const char *mp_ctc_tolower_range(void *self, char *first, const char *last)
{ (void)self; while (first != (char *)last) { *first = (char)tolower((unsigned char)*first); first++; } return last; }
static MS char mp_ctc_tolower_ch(void *self, char c)
{ (void)self; return (char)tolower((unsigned char)c); }
static MS const char *mp_ctc_toupper_range(void *self, char *first, const char *last)
{ (void)self; while (first != (char *)last) { *first = (char)toupper((unsigned char)*first); first++; } return last; }
static MS char mp_ctc_toupper_ch(void *self, char c)
{ (void)self; return (char)toupper((unsigned char)c); }
static MS const char *mp_ctc_widen_range(void *self, const char *first,
                                         const char *last, char *out)
{ (void)self; while (first != last) *out++ = *first++; return last; }
static MS char mp_ctc_widen_ch(void *self, char c) { (void)self; return c; }
static MS const char *mp_ctc_narrow_range(void *self, const char *first,
                                          const char *last, char dflt, char *out)
{ (void)self; (void)dflt; while (first != last) *out++ = *first++; return last; }
static MS char mp_ctc_narrow_ch(void *self, char c, char dflt)
{ (void)self; (void)dflt; return c; }

static void *mp_ctypec_vft[11] = {
    (void *)mp_facet_dtor, (void *)mp_facet_Incref, (void *)mp_facet_Decref,
    (void *)mp_ctc_tolower_range, (void *)mp_ctc_tolower_ch,
    (void *)mp_ctc_toupper_range, (void *)mp_ctc_toupper_ch,
    (void *)mp_ctc_widen_range,   (void *)mp_ctc_widen_ch,
    (void *)mp_ctc_narrow_range,  (void *)mp_ctc_narrow_ch
};

/* _Ctypevec as _Getcat copies it: page, table, delete flag, locale name. */
typedef struct {
    void    *vftable;
    int32_t  ref;
    int32_t  pad;
    uint32_t page;                  /* +0x10 */
    uint32_t pad2;
    const int16_t *table;           /* +0x18, what table() returns */
    int32_t  delfl;                 /* +0x20 */
    int32_t  pad3;
    const void *localename;         /* +0x28 */
} mp_ctypec;                        /* 0x30, the size its _Getcat allocates */

static MS size_t mp_ctypec_Getcat(const void **ppf, const void *ploc)
{
    (void)ploc;
    if (ppf && !*ppf) {
        mp_ctypec *f = (mp_ctypec *)w32_alloc(sizeof *f, 1);
        if (f) {
            mp_ctc_build();
            f->vftable = mp_ctypec_vft;
            /* The real _Getcat leaves this at zero and lets _Addfac take the
             * first reference. Ours starts held, so a locale that is rebuilt
             * cannot drop the classic facet out from under a plug-in. */
            f->ref   = 1;
            f->table = mp_ctc_table;
        }
        *ppf = f;
    }
    return 2;                        /* _X_CTYPE */
}
static size_t mp_ctypec_id_value;    /* std::ctype<char>::id */

/* The public members. Non-virtual in the real library too, but a plug-in that
 * installed its own ctype still has to be reached, so they dispatch. */
static MS const int16_t *mp_ctc_table_of(const mp_ctypec *self)
{ return self ? self->table : mp_ctc_classic_table(); }
static MS char mp_ctc_widen(void *self, char c)
{
    char (MS *f)(void *, char) = (char (MS *)(void *, char))mp_ct_slot(self, 8);
    return f ? f(self, c) : c;
}
static MS const char *mp_ctc_widen_p(void *self, const char *a, const char *b, char *o)
{
    const char *(MS *f)(void *, const char *, const char *, char *) =
        (const char *(MS *)(void *, const char *, const char *, char *))mp_ct_slot(self, 7);
    return f ? f(self, a, b, o) : b;
}
static MS char mp_ctc_narrow(void *self, char c, char d)
{
    char (MS *f)(void *, char, char) = (char (MS *)(void *, char, char))mp_ct_slot(self, 10);
    return f ? f(self, c, d) : c;
}
static MS const char *mp_ctc_narrow_p(void *self, const char *a, const char *b,
                                      char d, char *o)
{
    const char *(MS *f)(void *, const char *, const char *, char, char *) =
        (const char *(MS *)(void *, const char *, const char *, char, char *))mp_ct_slot(self, 9);
    return f ? f(self, a, b, d, o) : b;
}
static MS char mp_ctc_tolower(void *self, char c)
{
    char (MS *f)(void *, char) = (char (MS *)(void *, char))mp_ct_slot(self, 4);
    return f ? f(self, c) : (char)tolower((unsigned char)c);
}
static MS const char *mp_ctc_tolower_p(void *self, char *a, const char *b)
{
    const char *(MS *f)(void *, char *, const char *) =
        (const char *(MS *)(void *, char *, const char *))mp_ct_slot(self, 3);
    return f ? f(self, a, b) : b;
}
static MS char mp_ctc_toupper(void *self, char c)
{
    char (MS *f)(void *, char) = (char (MS *)(void *, char))mp_ct_slot(self, 6);
    return f ? f(self, c) : (char)toupper((unsigned char)c);
}
static MS const char *mp_ctc_toupper_p(void *self, char *a, const char *b)
{
    const char *(MS *f)(void *, char *, const char *) =
        (const char *(MS *)(void *, char *, const char *))mp_ct_slot(self, 5);
    return f ? f(self, a, b) : b;
}
static MS int32_t mp_ctc_is(const mp_ctypec *self, int16_t m, char c)
{ return (mp_ctc_table_of(self)[(unsigned char)c] & m) != 0; }
static MS const char *mp_ctc_is_p(const mp_ctypec *self, const char *a,
                                  const char *b, int16_t *o)
{
    const int16_t *t = mp_ctc_table_of(self);
    while (a != b) *o++ = t[(unsigned char)*a++];
    return b;
}
static MS const char *mp_ctc_scan_is(const mp_ctypec *self, int16_t m,
                                     const char *a, const char *b)
{
    const int16_t *t = mp_ctc_table_of(self);
    while (a != b && !(t[(unsigned char)*a] & m)) a++;
    return a;
}
static MS const char *mp_ctc_scan_not(const mp_ctypec *self, int16_t m,
                                      const char *a, const char *b)
{
    const int16_t *t = mp_ctc_table_of(self);
    while (a != b && (t[(unsigned char)*a] & m)) a++;
    return a;
}
static MS const int16_t *mp_ctc_table_pub(const mp_ctypec *self)
{ return mp_ctc_table_of(self); }
static const size_t mp_ctc_table_size = 256;

/* ios_base::exceptions(iostate). The real one masks to the valid state bits,
 * stores at +0x14 and falls into clear(rdstate(), false) so that a state already
 * set is re-thrown the moment it is asked for. */
static MS void mp_ios_exceptions_set(mp_ios *self, int32_t mask)
{
    if (!self) return;
    self->except = mask & 0x17;
    mp_ios_clear(self, self->state, 0);
}
static MS int32_t mp_ios_exceptions_get(const mp_ios *self)
{ return self ? self->except : 0; }

/* ---- basic_streambuf<wchar_t>: the parts that count in elements ---------- */

/* Everything that only moves a stored pointer about is shared with the narrow
 * streambuf; these are the ones where an element is two bytes, so sharing them
 * would step half a character at a time. */
/* VS2015 added the three-argument setp, which places the put pointer somewhere
 * other than the start of the buffer. */
static MS void mp_sb_setp3(void *self, char *pbeg, char *pnext, char *pend)
{
    mp_streambuf_body *b;
    if (!self) return;
    b = MP_SB(self);
    if (b->ipfirst) *b->ipfirst = pbeg;
    if (b->ipnext)  *b->ipnext  = pnext;
    if (b->ipcount) *b->ipcount = (int32_t)(pend - pnext);
}
static MS void mp_wsb_setp3(void *self, uint16_t *pbeg, uint16_t *pnext, uint16_t *pend)
{
    mp_streambuf_body *b;
    if (!self) return;
    b = MP_SB(self);
    if (b->ipfirst) *b->ipfirst = (char *)pbeg;
    if (b->ipnext)  *b->ipnext  = (char *)pnext;
    if (b->ipcount) *b->ipcount = (int32_t)(pend - pnext);
}

static MS void mp_wsb_gbump(void *self, int32_t n)
{
    mp_streambuf_body *b = MP_SB(self);
    if (b->inext)   *b->inext   += (ptrdiff_t)n * 2;
    if (b->igcount) *b->igcount -= n;
}
static MS void mp_wsb_pbump(void *self, int32_t n)
{
    mp_streambuf_body *b = MP_SB(self);
    if (b->ipnext)  *b->ipnext  += (ptrdiff_t)n * 2;
    if (b->ipcount) *b->ipcount -= n;
}
static MS uint16_t *mp_wsb_pninc(void *self)
{
    uint16_t *p = (uint16_t *)mp_sb_pptr(self);
    mp_wsb_pbump(self, 1);
    return p;
}
static MS uint16_t *mp_wsb_gninc(void *self)
{
    uint16_t *p = (uint16_t *)mp_sb_gptr(self);
    mp_wsb_gbump(self, 1);
    return p;
}
#define MP_WEOF 0xFFFF                       /* char_traits<wchar_t>::eof() */

static MS uint32_t mp_wsb_v_overflow(void *self, uint32_t c) { (void)self;(void)c; return MP_WEOF; }
static MS uint32_t mp_wsb_v_underflow(void *self)            { (void)self; return MP_WEOF; }
static MS uint32_t mp_wsb_v_pbackfail(void *self, uint32_t c){ (void)self;(void)c; return MP_WEOF; }
static MS uint32_t mp_wsb_v_uflow(void *self)
{
    uint32_t (MS *underflow)(void *) =
        (uint32_t (MS *)(void *))mp_sb_slot(self, MP_SB_UNDERFLOW);
    uint32_t c = underflow ? underflow(self) : MP_WEOF;
    if (c != MP_WEOF) mp_wsb_gbump(self, 1);
    return c;
}
static MS int64_t mp_wsb_v_xsgetn(void *self, uint16_t *out, int64_t n)
{
    int64_t done = 0;
    while (done < n) {
        int32_t avail = mp_sb_gcount(self);
        if (avail > 0) {
            int64_t take = (n - done) < avail ? (n - done) : avail;
            memcpy(out + done, mp_sb_gptr(self), (size_t)take * 2);
            mp_wsb_gbump(self, (int32_t)take);
            done += take;
        } else {
            uint32_t (MS *uflow)(void *) =
                (uint32_t (MS *)(void *))mp_sb_slot(self, MP_SB_UFLOW);
            uint32_t c = uflow ? uflow(self) : MP_WEOF;
            if (c == MP_WEOF) break;
            out[done++] = (uint16_t)c;
        }
    }
    return done;
}
static MS int64_t mp_wsb_v_xsputn(void *self, const uint16_t *in, int64_t n)
{
    int64_t done = 0;
    while (done < n) {
        int32_t room = mp_sb_pcount(self);
        if (room > 0) {
            int64_t put = (n - done) < room ? (n - done) : room;
            memcpy(mp_sb_pptr(self), in + done, (size_t)put * 2);
            mp_wsb_pbump(self, (int32_t)put);
            done += put;
        } else {
            uint32_t (MS *overflow)(void *, uint32_t) =
                (uint32_t (MS *)(void *, uint32_t))mp_sb_slot(self, MP_SB_OVERFLOW);
            if (!overflow || overflow(self, in[done]) == MP_WEOF) break;
            done++;
        }
    }
    return done;
}
static MS uint32_t mp_wsb_sputc(void *self, uint16_t c)
{
    if (mp_sb_pcount(self) > 0) {
        *(uint16_t *)mp_sb_pptr(self) = c;
        mp_wsb_pbump(self, 1);
        return c;
    } else {
        uint32_t (MS *overflow)(void *, uint32_t) =
            (uint32_t (MS *)(void *, uint32_t))mp_sb_slot(self, MP_SB_OVERFLOW);
        return overflow ? overflow(self, c) : MP_WEOF;
    }
}
static MS int64_t mp_wsb_sputn(void *self, const uint16_t *in, int64_t n)
{
    int64_t (MS *xsputn)(void *, const uint16_t *, int64_t) =
        (int64_t (MS *)(void *, const uint16_t *, int64_t))mp_sb_slot(self, MP_SB_XSPUTN);
    return xsputn ? xsputn(self, in, n) : 0;
}

static void *mp_wstreambuf_vft[MP_SB_NSLOTS] = {
    (void *)mp_facet_dtor,       (void *)mp_sb_lock,      (void *)mp_sb_unlock,
    (void *)mp_wsb_v_overflow,   (void *)mp_wsb_v_pbackfail,
    (void *)mp_sb_v_showmanyc,   (void *)mp_wsb_v_underflow,
    (void *)mp_wsb_v_uflow,      (void *)mp_wsb_v_xsgetn, (void *)mp_wsb_v_xsputn,
    (void *)mp_sb_v_seekoff,     (void *)mp_sb_v_seekoff, (void *)mp_sb_v_setbuf,
    (void *)mp_sb_v_sync,        (void *)mp_sb_v_imbue
};

/* ---- ios_base ------------------------------------------------------------ */

/* The format flags, with msvcp120's values -- the default fmtfl of 0x201 that
 * ios_base::_Init writes is skipws|dec, which fixes the whole set. */
enum {
    MP_FMT_SKIPWS = 0x0001, MP_FMT_UNITBUF = 0x0002, MP_FMT_UPPERCASE = 0x0004,
    MP_FMT_SHOWBASE = 0x0008, MP_FMT_SHOWPOINT = 0x0010, MP_FMT_SHOWPOS = 0x0020,
    MP_FMT_LEFT = 0x0040, MP_FMT_RIGHT = 0x0080, MP_FMT_INTERNAL = 0x0100,
    MP_FMT_DEC = 0x0200, MP_FMT_OCT = 0x0400, MP_FMT_HEX = 0x0800,
    MP_FMT_SCI = 0x1000, MP_FMT_FIXED = 0x2000, MP_FMT_BOOLALPHA = 0x4000,
    MP_FMT_ADJUST = MP_FMT_LEFT | MP_FMT_RIGHT | MP_FMT_INTERNAL,
    MP_FMT_BASE   = MP_FMT_DEC | MP_FMT_OCT | MP_FMT_HEX,
    MP_FMT_FLOAT  = MP_FMT_SCI | MP_FMT_FIXED
};
enum { MP_ST_GOOD = 0, MP_ST_EOF = 1, MP_ST_FAIL = 2, MP_ST_BAD = 4 };

static MS int32_t mp_ios_flags_get(const mp_ios *b) { return b ? b->fmtfl : 0; }
static MS int32_t mp_ios_flags_set(mp_ios *b, int32_t f)
{ int32_t o; if (!b) return 0; o = b->fmtfl; b->fmtfl = (uint16_t)f; return o; }
static MS int32_t mp_ios_setf(mp_ios *b, int32_t f)
{ int32_t o; if (!b) return 0; o = b->fmtfl; b->fmtfl |= (uint16_t)f; return o; }
static MS int32_t mp_ios_setf_mask(mp_ios *b, int32_t f, int32_t m)
{ int32_t o; if (!b) return 0; o = b->fmtfl; b->fmtfl = (b->fmtfl & ~m) | (f & m); return o; }
static MS void mp_ios_unsetf(mp_ios *b, int32_t m) { if (b) b->fmtfl &= ~m; }
static MS int64_t mp_ios_width_get(const mp_ios *b) { return b ? b->wide : 0; }
static MS int64_t mp_ios_width_set(mp_ios *b, int64_t w)
{ int64_t o; if (!b) return 0; o = b->wide; b->wide = w; return o; }
static MS int64_t mp_ios_prec_get(const mp_ios *b) { return b ? b->prec : 6; }
static MS int64_t mp_ios_prec_set(mp_ios *b, int64_t p)
{ int64_t o; if (!b) return 0; o = b->prec; b->prec = p; return o; }
static MS int32_t mp_ios_good(const mp_ios *b) { return (b && b->state == 0) ? 1 : 0; }
static MS int32_t mp_ios_bad(const mp_ios *b)  { return (b && (b->state & MP_ST_BAD)) ? 1 : 0; }
static MS int32_t mp_ios_eof(const mp_ios *b)  { return (b && (b->state & MP_ST_EOF)) ? 1 : 0; }
static MS int32_t mp_ios_fail(const mp_ios *b)
{ return (b && (b->state & (MP_ST_FAIL | MP_ST_BAD))) ? 1 : 0; }

/* getloc returns a locale by value. MSVC passes `this` first and the hidden
 * return pointer second, which is the opposite of the C convention, and the
 * disassembly is unambiguous: `mov rax,[rcx+0x40]` reads _Ploc off `this`
 * while rdx is the buffer it fills. The returned locale holds the _Locimp and
 * takes a reference on it. */
static MS void *mp_ios_getloc(mp_ios *b, void **ret)
{
    if (!ret) return ret;
    /* _Ploc is a locale the ios_base owns, one per stream, so that imbue on one
     * stream does not reach another. The real _Init allocates it there; putting
     * it off until something asks keeps construction to what the original does. */
    if (b && !b->pad40) {
        void **slot = (void **)w32_alloc(sizeof(void *), 1);
        if (slot) { slot[0] = mp_locale_Init(1); b->pad40 = slot; }
    }
    *ret = (b && b->pad40) ? *(void **)b->pad40 : mp_locale_Init(1);
    mp_facet_Incref((mp_facet *)*ret);
    return ret;
}
static MS void mp_ios_base_dtor(mp_ios *b)
{
    if (b && b->pad40) { w32_free(b->pad40); b->pad40 = NULL; }
}

/* ---- basic_ios ----------------------------------------------------------- */

static MS void *mp_bios_rdbuf(const mp_ios *b) { return b ? b->strbuf : NULL; }
static MS void *mp_bios_tie(const mp_ios *b)   { return b ? b->tiestr : NULL; }
static MS char  mp_bios_widen(const mp_ios *b, char c) { (void)b; return c; }
static MS uint16_t mp_bios_widen_w(const mp_ios *b, char c)
{ (void)b; return (uint16_t)(unsigned char)c; }
static MS char mp_bios_narrow(const mp_ios *b, char c, char d) { (void)b;(void)d; return c; }
static MS int32_t mp_bios_fill(const mp_ios *b) { return b ? b->fillch : ' '; }
static MS int32_t mp_bios_fill_set(mp_ios *b, int32_t c)
{ int32_t o; if (!b) return ' '; o = b->fillch; b->fillch = c; return o; }
/* imbue returns the previous locale by value, `this` first again. */
static MS void *mp_bios_imbue(mp_ios *b, void **ret, const void **loc)
{
    if (ret) mp_ios_getloc(b, ret);
    if (b && loc && b->pad40) *(const void **)b->pad40 = *loc;
    return ret;
}

/* ---- formatting ---------------------------------------------------------- */

/* What num_put would produce, produced directly.
 *
 * The real inserters reach the number through use_facet<num_put<E>>, and a
 * program that installs a num_put of its own gets it consulted. Nothing in the
 * plug-in corpus does that -- none of them so much as import num_put -- so what
 * is reproduced here is the formatting itself and the writes through the
 * streambuf's own xsputn, which is the part every caller can observe. */
typedef struct { char s[512]; int len; int prefix; } mp_fmtbuf;

static void mp_fmt_int(mp_fmtbuf *o, uint64_t mag, int neg, int32_t fl)
{
    char digits[80];
    int n = 0, base = 10, upper = (fl & MP_FMT_UPPERCASE) != 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if ((fl & MP_FMT_BASE) == MP_FMT_HEX) base = 16;
    else if ((fl & MP_FMT_BASE) == MP_FMT_OCT) base = 8;
    o->len = o->prefix = 0;
    if (base == 10) {
        if (neg) o->s[o->len++] = '-';
        else if (fl & MP_FMT_SHOWPOS) o->s[o->len++] = '+';
    }
    if (fl & MP_FMT_SHOWBASE) {
        if (base == 16) { o->s[o->len++] = '0'; o->s[o->len++] = upper ? 'X' : 'x'; }
        else if (base == 8 && mag != 0) o->s[o->len++] = '0';
    }
    o->prefix = o->len;
    do { digits[n++] = set[mag % (unsigned)base]; mag /= (unsigned)base; } while (mag);
    while (n > 0) o->s[o->len++] = digits[--n];
}

static void mp_fmt_float(mp_fmtbuf *o, long double v, int32_t fl, int64_t prec)
{
    char spec[16];
    int i = 0;
    char conv;
    if ((fl & MP_FMT_FLOAT) == MP_FMT_FIXED) conv = 'f';
    else if ((fl & MP_FMT_FLOAT) == MP_FMT_SCI) conv = 'e';
    else conv = 'g';
    if (fl & MP_FMT_UPPERCASE) conv = (char)toupper((unsigned char)conv);
    spec[i++] = '%';
    if (fl & MP_FMT_SHOWPOS) spec[i++] = '+';
    if (fl & MP_FMT_SHOWPOINT) spec[i++] = '#';
    spec[i++] = '.'; spec[i++] = '*'; spec[i++] = 'L'; spec[i++] = conv;
    spec[i] = 0;
    o->len = snprintf(o->s, sizeof o->s, spec, (int)prec, v);
    if (o->len < 0) o->len = 0;
    if (o->len > (int)sizeof o->s - 1) o->len = (int)sizeof o->s - 1;
    o->prefix = (o->len > 0 && (o->s[0] == '-' || o->s[0] == '+')) ? 1 : 0;
}

/* The stream side: pad to width, write through the buffer, clear the width.
 * A stream with no buffer, or a short write, is badbit -- as the real one. */
static mp_ios *mp_ios_of(void *stream)
{
    const int32_t *vb;
    if (!stream) return NULL;
    vb = *(const int32_t **)stream;
    return (mp_ios *)((char *)stream + (vb ? vb[1] : 16));
}
static void *mp_ost_write_pad(void *stream, const mp_fmtbuf *f, int widechars)
{
    mp_ios *b = mp_ios_of(stream);
    void *sb = b ? b->strbuf : NULL;
    int64_t w = b ? b->wide : 0;
    int64_t pad = w > f->len ? w - f->len : 0;
    int32_t fill = b ? b->fillch : ' ';
    int32_t adj = b ? (b->fmtfl & MP_FMT_ADJUST) : 0;
    int64_t before = 0, mid = 0, after = 0;
    int64_t k;
    int ok = 1;

    if (!b) return stream;
    b->wide = 0;
    if (!sb || !mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_BAD, 0); return stream; }
    if (adj == MP_FMT_LEFT)          after = pad;
    else if (adj == MP_FMT_INTERNAL) mid = pad;
    else                             before = pad;

#define MP_EMIT(ptr, n) do {                                                  \
        if (widechars) {                                                      \
            uint16_t wb[512]; int64_t q, chunk;                               \
            const char *src = (ptr); int64_t left = (n);                      \
            while (left > 0 && ok) {                                          \
                chunk = left > 512 ? 512 : left;                              \
                for (q = 0; q < chunk; q++) wb[q] = (uint16_t)(unsigned char)src[q]; \
                if (mp_wsb_sputn(sb, wb, chunk) != chunk) ok = 0;             \
                src += chunk; left -= chunk;                                  \
            }                                                                 \
        } else if (mp_sb_sputn(sb, (ptr), (n)) != (n)) ok = 0;                \
    } while (0)
#define MP_FILL(cnt) do {                                                     \
        char fb[64]; int64_t left = (cnt), chunk;                             \
        memset(fb, (char)fill, sizeof fb);                                    \
        while (left > 0 && ok) {                                              \
            chunk = left > 64 ? 64 : left;                                    \
            MP_EMIT(fb, chunk);                                               \
            left -= chunk;                                                    \
        }                                                                     \
    } while (0)

    MP_FILL(before);
    if (mid) {
        MP_EMIT(f->s, f->prefix);
        MP_FILL(mid);
        MP_EMIT(f->s + f->prefix, f->len - f->prefix);
    } else {
        MP_EMIT(f->s, f->len);
    }
    MP_FILL(after);
    (void)k;
#undef MP_EMIT
#undef MP_FILL
    if (!ok) mp_ios_setstate(b, MP_ST_BAD, 0);
    return stream;
}

/* The inserters. Signed and unsigned of every width, the two float widths,
 * bool and const void*, narrow and wide. Each is the same three steps: check
 * the stream is good, format, pad and write. */
#define MP_OSTREAM_INT(name, ctype, isneg, wide)                              \
static MS void *name(void *os, ctype v)                                       \
{                                                                             \
    mp_ios *b = mp_ios_of(os);                                                \
    mp_fmtbuf f;                                                              \
    uint64_t mag;                                                             \
    int neg = 0;                                                              \
    if (!b) return os;                                                        \
    if (!mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_FAIL, 0); return os; }     \
    if (isneg && (int64_t)v < 0) { neg = 1; mag = (uint64_t)-(int64_t)v; }     \
    else mag = (uint64_t)v;                                                   \
    mp_fmt_int(&f, mag, neg, b->fmtfl);                                       \
    return mp_ost_write_pad(os, &f, wide);                                    \
}
MP_OSTREAM_INT(mp_ost_short,   int16_t,  1, 0)
MP_OSTREAM_INT(mp_ost_ushort,  uint16_t, 0, 0)
MP_OSTREAM_INT(mp_ost_int,     int32_t,  1, 0)
MP_OSTREAM_INT(mp_ost_uint,    uint32_t, 0, 0)
MP_OSTREAM_INT(mp_ost_long,    int32_t,  1, 0)
MP_OSTREAM_INT(mp_ost_ulong,   uint32_t, 0, 0)
MP_OSTREAM_INT(mp_ost_i64,     int64_t,  1, 0)
MP_OSTREAM_INT(mp_ost_u64,     uint64_t, 0, 0)
MP_OSTREAM_INT(mp_wost_short,  int16_t,  1, 1)
MP_OSTREAM_INT(mp_wost_ushort, uint16_t, 0, 1)
MP_OSTREAM_INT(mp_wost_int,    int32_t,  1, 1)
MP_OSTREAM_INT(mp_wost_uint,   uint32_t, 0, 1)
MP_OSTREAM_INT(mp_wost_long,   int32_t,  1, 1)
MP_OSTREAM_INT(mp_wost_ulong,  uint32_t, 0, 1)
MP_OSTREAM_INT(mp_wost_i64,    int64_t,  1, 1)
MP_OSTREAM_INT(mp_wost_u64,    uint64_t, 0, 1)

#define MP_OSTREAM_FLT(name, ctype, wide)                                     \
static MS void *name(void *os, ctype v)                                       \
{                                                                             \
    mp_ios *b = mp_ios_of(os);                                                \
    mp_fmtbuf f;                                                              \
    if (!b) return os;                                                        \
    if (!mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_FAIL, 0); return os; }     \
    mp_fmt_float(&f, (long double)v, b->fmtfl, b->prec);                       \
    return mp_ost_write_pad(os, &f, wide);                                    \
}
MP_OSTREAM_FLT(mp_ost_float,   float,       0)
MP_OSTREAM_FLT(mp_ost_double,  double,      0)
MP_OSTREAM_FLT(mp_ost_ldouble, long double, 0)
MP_OSTREAM_FLT(mp_wost_float,  float,       1)
MP_OSTREAM_FLT(mp_wost_double, double,      1)
MP_OSTREAM_FLT(mp_wost_ldouble,long double, 1)

#define MP_OSTREAM_BOOL(name, wide)                                           \
static MS void *name(void *os, uint8_t v)                                     \
{                                                                             \
    mp_ios *b = mp_ios_of(os);                                                \
    mp_fmtbuf f;                                                              \
    if (!b) return os;                                                        \
    if (!mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_FAIL, 0); return os; }     \
    if (b->fmtfl & MP_FMT_BOOLALPHA) {                                        \
        const char *t = v ? "true" : "false";                                 \
        f.len = (int)strlen(t); f.prefix = 0;                                 \
        memcpy(f.s, t, (size_t)f.len);                                        \
    } else {                                                                  \
        mp_fmt_int(&f, v ? 1u : 0u, 0, b->fmtfl);                             \
    }                                                                         \
    return mp_ost_write_pad(os, &f, wide);                                    \
}
MP_OSTREAM_BOOL(mp_ost_bool,  0)
MP_OSTREAM_BOOL(mp_wost_bool, 1)

/* A pointer prints the way %p does on Win64: uppercase hex, no 0x, padded to
 * the full width of the pointer. */
#define MP_OSTREAM_PTR(name, wide)                                            \
static MS void *name(void *os, const void *v)                                 \
{                                                                             \
    mp_ios *b = mp_ios_of(os);                                                \
    mp_fmtbuf f;                                                              \
    if (!b) return os;                                                        \
    if (!mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_FAIL, 0); return os; }     \
    f.len = snprintf(f.s, sizeof f.s, "%016llX",                              \
                     (unsigned long long)(uintptr_t)v);                       \
    if (f.len < 0) f.len = 0;                                                 \
    f.prefix = 0;                                                             \
    return mp_ost_write_pad(os, &f, wide);                                    \
}
MP_OSTREAM_PTR(mp_ost_ptr,  0)
MP_OSTREAM_PTR(mp_wost_ptr, 1)

/* put, write and flush. */
static MS void *mp_ost_put(void *os, char c)
{
    mp_ios *b = mp_ios_of(os);
    if (!b) return os;
    if (!b->strbuf || !mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_BAD, 0); return os; }
    if (mp_sb_sputc(b->strbuf, (unsigned char)c) == MP_EOF)
        mp_ios_setstate(b, MP_ST_BAD, 0);
    return os;
}
static MS void *mp_wost_put(void *os, uint16_t c)
{
    mp_ios *b = mp_ios_of(os);
    if (!b) return os;
    if (!b->strbuf || !mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_BAD, 0); return os; }
    if (mp_wsb_sputc(b->strbuf, c) == MP_WEOF)
        mp_ios_setstate(b, MP_ST_BAD, 0);
    return os;
}
static MS void *mp_ost_write(void *os, const char *sdata, int64_t n)
{
    mp_ios *b = mp_ios_of(os);
    if (!b) return os;
    if (!b->strbuf || !mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_BAD, 0); return os; }
    if (mp_sb_sputn(b->strbuf, sdata, n) != n) mp_ios_setstate(b, MP_ST_BAD, 0);
    return os;
}
static MS void *mp_wost_write(void *os, const uint16_t *sdata, int64_t n)
{
    mp_ios *b = mp_ios_of(os);
    if (!b) return os;
    if (!b->strbuf || !mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_BAD, 0); return os; }
    if (mp_wsb_sputn(b->strbuf, sdata, n) != n) mp_ios_setstate(b, MP_ST_BAD, 0);
    return os;
}
static MS void *mp_ost_flush(void *os)
{
    mp_ios *b = mp_ios_of(os);
    if (b && b->strbuf && mp_sb_pubsync(b->strbuf) == -1)
        mp_ios_setstate(b, MP_ST_BAD, 0);
    return os;
}

/* basic_streambuf::_Init(), the protected reset: it points the indirection
 * fields back at the object's own pointers, which is what every constructor
 * does before a derived class calls setg/setp. */
static MS void mp_sb_init(void *self)
{
    mp_streambuf_body *b;
    if (!self) return;
    b = MP_SB(self);
    b->first = b->pfirst = b->next = b->pnext = NULL;
    b->gcount = b->pcount = 0;
    b->ifirst  = &b->first;   b->ipfirst = &b->pfirst;
    b->inext   = &b->next;    b->ipnext  = &b->pnext;
    b->igcount = &b->gcount;  b->ipcount = &b->pcount;
}

/* ostream << manipulator, where the manipulator takes the stream itself rather
 * than its ios_base -- endl, ends and flush are all this shape. */
static MS void *mp_ostream_manip_os(void *os, void *(MS *fn)(void *))
{ return (os && fn) ? fn(os) : os; }

/* The manipulators that carry a value. _Smanip is { void (*fn)(ios_base&, T); T }
 * -- sixteen bytes, returned through a hidden pointer that follows nothing else,
 * since these are free functions. */
typedef struct { void *fn; int64_t arg; } mp_smanip;
static MS void mp_smanip_setw(mp_ios *b, int64_t n)    { if (b) b->wide = n; }
static MS void mp_smanip_setprec(mp_ios *b, int64_t n) { if (b) b->prec = n; }
static MS void mp_smanip_setfl(mp_ios *b, int64_t n)   { if (b) b->fmtfl |= (uint16_t)n; }
static MS void mp_smanip_resetfl(mp_ios *b, int64_t n) { if (b) b->fmtfl &= ~(uint16_t)n; }
static MS mp_smanip *mp_setw(mp_smanip *ret, int64_t n)
{ if (ret) { ret->fn = (void *)mp_smanip_setw; ret->arg = n; } return ret; }
static MS mp_smanip *mp_setprecision(mp_smanip *ret, int64_t n)
{ if (ret) { ret->fn = (void *)mp_smanip_setprec; ret->arg = n; } return ret; }
static MS mp_smanip *mp_setiosflags(mp_smanip *ret, int32_t n)
{ if (ret) { ret->fn = (void *)mp_smanip_setfl; ret->arg = n; } return ret; }
static MS mp_smanip *mp_resetiosflags(mp_smanip *ret, int32_t n)
{ if (ret) { ret->fn = (void *)mp_smanip_resetfl; ret->arg = n; } return ret; }

/* MSVC's xtime: seconds since the epoch and a nanosecond remainder. */
typedef struct { int64_t sec; long nsec; } mp_xtime;

/* ---- codecvt ------------------------------------------------------------- */

/* codecvt<wchar_t,char,int>::_Getcat allocates 0x40, which is facet(16) plus the
 * 48-byte _Cvtvec _Getcvt fills, and the vftable has the ten slots the narrow
 * and wide facets share. The wide conversion is done as UTF-8: on Windows this
 * facet follows the ANSI code page, and UTF-8 is what that means on the host
 * these plug-ins are actually running on. ASCII -- which is all any of them
 * put through it -- is identical either way. */
enum { MP_CVT_OK = 0, MP_CVT_PARTIAL = 1, MP_CVT_ERROR = 2, MP_CVT_NOCONV = 3 };

typedef struct {
    void    *vftable;
    int32_t  ref;
    int32_t  pad;
    uint32_t page;        /* +0x10, the _Cvtvec */
    uint32_t mbcurmax;
    char     rest[40];
} mp_codecvt;             /* 0x40 */

static MS void *mp_Getcvt(void *out)
{
    if (out) {
        memset(out, 0, 48);
        ((uint32_t *)out)[0] = 65001;      /* CP_UTF8 */
        ((uint32_t *)out)[1] = 4;          /* MB_CUR_MAX */
    }
    return out;
}

/* codecvt<char,char,int>: the identity facet. Every conversion is noconv. */
static MS int32_t mp_cvtc_always_noconv(void *self) { (void)self; return 1; }
static MS int32_t mp_cvtc_out(void *self, int32_t *st, const char *f, const char *fe,
                              const char **fn, char *t, char *te, char **tn)
{ (void)self;(void)st;(void)fe;(void)te; if (fn) *fn = f; if (tn) *tn = t; return MP_CVT_NOCONV; }
static MS int32_t mp_cvtc_in(void *self, int32_t *st, const char *f, const char *fe,
                             const char **fn, char *t, char *te, char **tn)
{ return mp_cvtc_out(self, st, f, fe, fn, t, te, tn); }
static MS int32_t mp_cvtc_unshift(void *self, int32_t *st, char *t, char *te, char **tn)
{ (void)self;(void)st;(void)te; if (tn) *tn = t; return MP_CVT_NOCONV; }
static MS int32_t mp_cvtc_length(void *self, int32_t *st, const char *f,
                                 const char *fe, size_t mx)
{
    size_t n = (size_t)(fe - f);
    (void)self;(void)st;
    return (int32_t)(n < mx ? n : mx);
}
static MS int32_t mp_cvtc_encoding(void *self)   { (void)self; return 1; }
static MS int32_t mp_cvtc_max_length(void *self) { (void)self; return 1; }
static MS int32_t mp_cvtw_encoding(void *self)   { (void)self; return 0; }
static MS int32_t mp_cvtw_max_length(void *self) { (void)self; return 4; }

/* codecvt<wchar_t,char,int>, UTF-16 against UTF-8. */
static int mp_u8_len(unsigned char c)
{ return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0; }

static MS int32_t mp_cvtw_always_noconv(void *self) { (void)self; return 0; }
static MS int32_t mp_cvtw_out(void *self, int32_t *st, const uint16_t *f,
                              const uint16_t *fe, const uint16_t **fn,
                              char *t, char *te, char **tn)
{
    int32_t r = MP_CVT_OK;
    (void)self; (void)st;
    while (f < fe) {
        uint32_t c = *f;
        int need;
        if (c >= 0xD800 && c < 0xDC00 && f + 1 < fe &&
            f[1] >= 0xDC00 && f[1] < 0xE000)
            c = 0x10000u + ((c - 0xD800u) << 10) + (f[1] - 0xDC00u);
        need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (t + need > te) { r = MP_CVT_PARTIAL; break; }
        if (need == 1) *t++ = (char)c;
        else if (need == 2) { *t++ = (char)(0xC0 | (c >> 6)); *t++ = (char)(0x80 | (c & 0x3F)); }
        else if (need == 3) { *t++ = (char)(0xE0 | (c >> 12)); *t++ = (char)(0x80 | ((c >> 6) & 0x3F)); *t++ = (char)(0x80 | (c & 0x3F)); }
        else { *t++ = (char)(0xF0 | (c >> 18)); *t++ = (char)(0x80 | ((c >> 12) & 0x3F)); *t++ = (char)(0x80 | ((c >> 6) & 0x3F)); *t++ = (char)(0x80 | (c & 0x3F)); }
        f += (c >= 0x10000) ? 2 : 1;
    }
    if (fn) *fn = f;
    if (tn) *tn = t;
    return r;
}
static MS int32_t mp_cvtw_in(void *self, int32_t *st, const char *f, const char *fe,
                             const char **fn, uint16_t *t, uint16_t *te, uint16_t **tn)
{
    int32_t r = MP_CVT_OK;
    (void)self; (void)st;
    while (f < fe) {
        int n = mp_u8_len((unsigned char)*f), k;
        uint32_t c;
        if (n == 0) { r = MP_CVT_ERROR; break; }
        if (f + n > fe) { r = MP_CVT_PARTIAL; break; }
        c = n == 1 ? (uint32_t)(unsigned char)*f
          : (uint32_t)((unsigned char)*f & (0xFF >> (n + 1)));
        for (k = 1; k < n; k++) c = (c << 6) | ((unsigned char)f[k] & 0x3F);
        if (c >= 0x10000) {
            if (t + 2 > te) { r = MP_CVT_PARTIAL; break; }
            c -= 0x10000;
            *t++ = (uint16_t)(0xD800 + (c >> 10));
            *t++ = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (t + 1 > te) { r = MP_CVT_PARTIAL; break; }
            *t++ = (uint16_t)c;
        }
        f += n;
    }
    if (fn) *fn = f;
    if (tn) *tn = t;
    return r;
}
static MS int32_t mp_cvtw_unshift(void *self, int32_t *st, char *t, char *te, char **tn)
{ (void)self;(void)st;(void)te; if (tn) *tn = t; return MP_CVT_OK; }
static MS int32_t mp_cvtw_length(void *self, int32_t *st, const char *f,
                                 const char *fe, size_t mx)
{
    const char *p = f;
    size_t made = 0;
    (void)self; (void)st;
    while (p < fe && made < mx) {
        int n = mp_u8_len((unsigned char)*p);
        if (n == 0 || p + n > fe) break;
        p += n;
        made++;
    }
    return (int32_t)(p - f);
}

static void *mp_codecvt_c_vft[10] = {
    (void *)mp_facet_dtor, (void *)mp_facet_Incref, (void *)mp_facet_Decref,
    (void *)mp_cvtc_out, (void *)mp_cvtc_in, (void *)mp_cvtc_unshift,
    (void *)mp_cvtc_length, (void *)mp_cvtc_max_length, (void *)mp_cvtc_encoding,
    (void *)mp_cvtc_always_noconv
};
static void *mp_codecvt_w_vft[10] = {
    (void *)mp_facet_dtor, (void *)mp_facet_Incref, (void *)mp_facet_Decref,
    (void *)mp_cvtw_out, (void *)mp_cvtw_in, (void *)mp_cvtw_unshift,
    (void *)mp_cvtw_length, (void *)mp_cvtw_max_length, (void *)mp_cvtw_encoding,
    (void *)mp_cvtw_always_noconv
};
static size_t mp_cvtc_id_value, mp_cvtw_id_value;
static size_t mp_numpunct_id_value, mp_timeput_id_value, mp_ctypew_id_value;

static void *mp_codecvt_new(void **vft)
{
    mp_codecvt *f = (mp_codecvt *)w32_alloc(sizeof *f, 1);
    if (f) {
        f->vftable = vft;
        f->ref = 1;
        mp_Getcvt(&f->page);
    }
    return f;
}
static MS size_t mp_cvtc_Getcat(const void **ppf, const void *ploc)
{ (void)ploc; if (ppf && !*ppf) *ppf = mp_codecvt_new(mp_codecvt_c_vft); return 1; }
static MS size_t mp_cvtw_Getcat(const void **ppf, const void *ploc)
{ (void)ploc; if (ppf && !*ppf) *ppf = mp_codecvt_new(mp_codecvt_w_vft); return 1; }

/* ---- _Yarn --------------------------------------------------------------- */

/* The small owned string the locale keeps its names in. _C_str is
 * `mov rax,[rcx]; test rax,rax; jne done; lea rax,[rcx+8]` -- a null pointer
 * falls back to an inline terminator at +0x08, which is why the narrow and wide
 * _C_str, _Empty, _Tidy and destructor are all the same code in the real DLL: a
 * zero at +8 reads as an empty string at either width. operator= frees what is
 * there, then copies the argument; assigning a pointer to itself is a no-op,
 * which is checked first. */
typedef struct { void *ptr; char nul[8]; } mp_yarn;

static MS void mp_yarn_tidy(mp_yarn *y)
{ if (y) { if (y->ptr) w32_free(y->ptr); y->ptr = NULL; } }
static MS void *mp_yarn_ctor(mp_yarn *y)
{ if (y) { y->ptr = NULL; memset(y->nul, 0, sizeof y->nul); } return y; }
static MS const void *mp_yarn_cstr(const mp_yarn *y)
{ return y ? (y->ptr ? y->ptr : (const void *)y->nul) : NULL; }
static MS int32_t mp_yarn_empty(const mp_yarn *y)
{ const char *p = (const char *)mp_yarn_cstr(y); return (!p || *p == 0) ? 1 : 0; }

static MS void *mp_yarn_assign(mp_yarn *y, const char *s)
{
    if (!y || y->ptr == (void *)s) return y;
    mp_yarn_tidy(y);
    if (s) {
        size_t n = strlen(s) + 1;
        char *p = (char *)w32_alloc(n, 0);
        y->ptr = p;
        if (p) memcpy(p, s, n);
    }
    return y;
}
static MS void *mp_yarn_assign_w(mp_yarn *y, const uint16_t *s)
{
    if (!y || y->ptr == (void *)s) return y;
    mp_yarn_tidy(y);
    if (s) {
        size_t n = 0;
        uint16_t *p;
        while (s[n]) n++;
        n = (n + 1) * 2;
        p = (uint16_t *)w32_alloc(n, 0);
        y->ptr = p;
        if (p) memcpy(p, s, n);
    }
    return y;
}
static MS void *mp_yarn_assign_y(mp_yarn *y, const mp_yarn *o)
{ return mp_yarn_assign(y, (const char *)mp_yarn_cstr(o)); }
static MS void *mp_yarn_ctor_s(mp_yarn *y, const char *s)
{ mp_yarn_ctor(y); return mp_yarn_assign(y, s); }
static MS void *mp_yarn_ctor_copy(mp_yarn *y, const mp_yarn *o)
{ mp_yarn_ctor(y); return mp_yarn_assign_y(y, o); }

/* ---- _Locinfo ------------------------------------------------------------ */

/* Only its name and the two boolean words are ever asked for here; the object
 * is the 0x50 bytes its constructor clears. */
typedef struct { char name[64]; char pad[32]; } mp_locinfo;
static MS void *mp_locinfo_ctor(mp_locinfo *self, const char *name)
{
    if (!self) return self;
    memset(self, 0, sizeof *self);
    snprintf(self->name, sizeof self->name, "%s", name ? name : "C");
    return self;
}
static MS void mp_locinfo_dtor(mp_locinfo *self) { (void)self; }
static MS const char *mp_locinfo_gettrue(const mp_locinfo *self)  { (void)self; return "true"; }
static MS const char *mp_locinfo_getfalse(const mp_locinfo *self) { (void)self; return "false"; }

/* ---- locale::classic ----------------------------------------------------- */

static void *mp_classic_locale[1];
static MS void *mp_locale_classic(void)
{
    if (!mp_classic_locale[0]) mp_classic_locale[0] = mp_locale_Init(1);
    return mp_classic_locale;
}

/* ---- xtime and the numeric helpers --------------------------------------- */

static MS int32_t mp_xtime_get(mp_xtime *t, int32_t type)
{
    struct timespec ts;
    if (!t || type != 1) return 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    t->sec = (int64_t)ts.tv_sec;
    t->nsec = ts.tv_nsec;
    return 1;
}
static MS int64_t mp_Xtime_diff_to_millis2(const mp_xtime *a, const mp_xtime *b)
{
    int64_t ms;
    if (!a || !b) return 0;
    ms = (a->sec - b->sec) * 1000 + (a->nsec - b->nsec + 999999) / 1000000;
    return ms < 0 ? 0 : ms;
}

/* _Dtest and friends classify a value the way the CRT's own maths does:
 * 2 for a NaN, 1 for an infinity, -2 for a denormal, 0 for anything finite. */
enum { MP_FP_NAN = 2, MP_FP_INF = 1, MP_FP_FINITE = 0, MP_FP_DENORM = -2 };
static MS int16_t mp_FDtest(float *px)
{
    if (!px) return MP_FP_NAN;
    if (isnan(*px)) return MP_FP_NAN;
    if (isinf(*px)) return MP_FP_INF;
    if (fpclassify(*px) == FP_SUBNORMAL) return MP_FP_DENORM;
    return MP_FP_FINITE;
}
/* _Exp(&x, y, eoff) leaves y * e^x * 2^eoff in x and classifies the result. */
static MS int16_t mp_Exp(double *px, double y, int16_t eoff)
{
    double v;
    if (!px) return MP_FP_NAN;
    v = ldexp(y * exp(*px), eoff);
    *px = v;
    if (isnan(v)) return MP_FP_NAN;
    if (isinf(v)) return MP_FP_INF;
    return MP_FP_FINITE;
}
static MS int16_t mp_FExp(float *px, float y, int16_t eoff)
{
    double d;
    int16_t r;
    if (!px) return MP_FP_NAN;
    d = *px;
    r = mp_Exp(&d, y, eoff);
    *px = (float)d;
    return r;
}
static const double mp_Inf_v  = (double)INFINITY;
static const double mp_Nan_v  = (double)NAN;
static const float  mp_FInf_v = (float)INFINITY;
static const float  mp_FNan_v = (float)NAN;

/* ---- the error maps ------------------------------------------------------ */

static MS const char *mp_Syserror_map(int32_t e)
{ const char *m = strerror(e); return m ? m : "unknown error"; }
static MS const char *mp_Winerror_map(int32_t e) { (void)e; return "Windows error"; }
static MS const char *mp_Future_error_map(int32_t e)
{
    switch (e) {
    case 0: return "broken promise";
    case 1: return "future already retrieved";
    case 2: return "promise already satisfied";
    case 3: return "no state";
    default: return "unknown future error";
    }
}

/* ---- _Fiopen ------------------------------------------------------------- */

/* What basic_filebuf::open goes through. The mode bits are ios_base::openmode:
 * in=1, out=2, ate=4, app=8, trunc=0x10, binary=0x20. */
static MS void *mp_Fiopen(const uint16_t *wname, int32_t mode, int32_t prot)
{
    char path[1024], mstr[8];
    size_t i = 0;
    int j = 0;
    int in = (mode & 1) != 0, out = (mode & 2) != 0;
    int app = (mode & 8) != 0, trunc = (mode & 0x10) != 0;
    (void)prot;
    if (!wname) return NULL;
    while (wname[i] && i < sizeof path - 1) { path[i] = (char)wname[i]; i++; }
    path[i] = 0;
    if (in && out)      { mstr[j++] = trunc ? 'w' : 'r'; mstr[j++] = '+'; }
    else if (out)       { mstr[j++] = app ? 'a' : 'w'; }
    else                { mstr[j++] = 'r'; }
    if (mode & 0x20) mstr[j++] = 'b';
    mstr[j] = 0;
    return fopen(path, mstr);
}

/* ---- basic_istream ------------------------------------------------------- */

/* The extractors, the mirror of the inserters: skip whitespace if skipws is
 * set, read as many characters as the conversion wants, put back the one that
 * ended it. A conversion that matches nothing is failbit, and running out of
 * input on the way is eofbit as well -- which is what callers branch on. */
static int mp_ist_skipws(mp_ios *b)
{
    void *sb = b ? b->strbuf : NULL;
    int c;
    if (!sb) return -1;
    if (!(b->fmtfl & MP_FMT_SKIPWS)) return mp_sb_sgetc(sb);
    for (;;) {
        c = mp_sb_sgetc(sb);
        if (c == MP_EOF) return MP_EOF;
        if (!isspace((unsigned char)c)) return c;
        mp_sb_sbumpc(sb);
    }
}
static int mp_ist_gather(void *is, char *out, size_t max, const char *extra)
{
    mp_ios *b = mp_ios_of(is);
    void *sb = b ? b->strbuf : NULL;
    size_t n = 0;
    int c;
    if (!b) return 0;
    if (!sb || !mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_FAIL, 0); return 0; }
    c = mp_ist_skipws(b);
    while (c != MP_EOF && n + 1 < max &&
           (isalnum((unsigned char)c) || strchr(extra, c) != NULL)) {
        out[n++] = (char)c;
        mp_sb_sbumpc(sb);
        c = mp_sb_sgetc(sb);
    }
    out[n] = 0;
    if (c == MP_EOF) mp_ios_setstate(b, MP_ST_EOF, 0);
    if (n == 0) { mp_ios_setstate(b, MP_ST_FAIL, 0); return 0; }
    return 1;
}
static int mp_ist_base(const mp_ios *b)
{
    int32_t f = b->fmtfl & MP_FMT_BASE;
    return f == MP_FMT_HEX ? 16 : f == MP_FMT_OCT ? 8 : f == MP_FMT_DEC ? 10 : 0;
}
#define MP_ISTREAM_INT(name, ctype, conv)                                     \
static MS void *name(void *is, ctype *v)                                      \
{                                                                             \
    char buf[128];                                                            \
    mp_ios *b = mp_ios_of(is);                                                \
    if (!b || !v) return is;                                                  \
    if (mp_ist_gather(is, buf, sizeof buf, "+-xXabcdefABCDEF")) {              \
        char *end = NULL;                                                     \
        conv;                                                                 \
        if (end == buf) mp_ios_setstate(b, MP_ST_FAIL, 0);                    \
    }                                                                         \
    return is;                                                                \
}
MP_ISTREAM_INT(mp_ist_int,   int32_t,  *v = (int32_t)strtol(buf, &end, mp_ist_base(b)))
MP_ISTREAM_INT(mp_ist_uint,  uint32_t, *v = (uint32_t)strtoul(buf, &end, mp_ist_base(b)))
MP_ISTREAM_INT(mp_ist_long,  int32_t,  *v = (int32_t)strtol(buf, &end, mp_ist_base(b)))
MP_ISTREAM_INT(mp_ist_ulong, uint32_t, *v = (uint32_t)strtoul(buf, &end, mp_ist_base(b)))
MP_ISTREAM_INT(mp_ist_i64,   int64_t,  *v = (int64_t)strtoll(buf, &end, mp_ist_base(b)))
MP_ISTREAM_INT(mp_ist_u64,   uint64_t, *v = (uint64_t)strtoull(buf, &end, mp_ist_base(b)))
MP_ISTREAM_INT(mp_ist_float, float,    *v = strtof(buf, &end))
MP_ISTREAM_INT(mp_ist_double,double,   *v = strtod(buf, &end))
static MS void *mp_ist_bool(void *is, uint8_t *v)
{
    char buf[16];
    mp_ios *b = mp_ios_of(is);
    if (!b || !v) return is;
    if (!mp_ist_gather(is, buf, sizeof buf, "")) return is;
    if (b->fmtfl & MP_FMT_BOOLALPHA) {
        if (!strcmp(buf, "true")) *v = 1;
        else if (!strcmp(buf, "false")) *v = 0;
        else mp_ios_setstate(b, MP_ST_FAIL, 0);
    } else {
        *v = (uint8_t)(buf[0] != '0');
    }
    return is;
}
static MS void *mp_ist_read(void *is, char *out, int64_t n)
{
    mp_ios *b = mp_ios_of(is);
    int64_t got;
    if (!b) return is;
    if (!b->strbuf || !mp_ios_good(b)) { mp_ios_setstate(b, MP_ST_FAIL, 0); return is; }
    got = mp_sb_sgetn(b->strbuf, out, n);
    *(int64_t *)((char *)is + 0x08) = got;        /* the count gcount() returns */
    if (got != n) mp_ios_setstate(b, MP_ST_EOF | MP_ST_FAIL, 0);
    return is;
}
static MS void *mp_ist_seekg(void *is, int64_t pos)
{
    mp_ios *b = mp_ios_of(is);
    if (b && b->strbuf) {
        int64_t (MS *seekoff)(void *, int64_t, int32_t, int32_t) =
            (int64_t (MS *)(void *, int64_t, int32_t, int32_t))
            mp_sb_slot(b->strbuf, MP_SB_SEEKOFF);
        if (seekoff && seekoff(b->strbuf, pos, 0, 1) < 0)
            mp_ios_setstate(b, MP_ST_FAIL, 0);
    }
    return is;
}
static MS int64_t *mp_ist_tellg(void *is, int64_t *ret)
{
    mp_ios *b = mp_ios_of(is);
    int64_t (MS *seekoff)(void *, int64_t, int32_t, int32_t) =
        (b && b->strbuf)
        ? (int64_t (MS *)(void *, int64_t, int32_t, int32_t))
          mp_sb_slot(b->strbuf, MP_SB_SEEKOFF)
        : NULL;
    if (ret) *ret = seekoff ? seekoff(b->strbuf, 0, 1, 1) : -1;
    return ret;
}
static MS void *mp_istream_manip_is(void *is, void *(MS *fn)(void *))
{ return (is && fn) ? fn(is) : is; }
static MS void *mp_ws(void *is)
{
    mp_ios *b = mp_ios_of(is);
    if (b && b->strbuf) {
        int c;
        while ((c = mp_sb_sgetc(b->strbuf)) != MP_EOF && isspace((unsigned char)c))
            mp_sb_sbumpc(b->strbuf);
        if (c == MP_EOF) mp_ios_setstate(b, MP_ST_EOF, 0);
    }
    return is;
}

/* The rest of the istream surface. _Ipfx is the sentry: flush whatever this
 * stream is tied to, skip leading whitespace unless told not to or skipws is
 * off, and report whether the stream is still good -- a stream that is not is
 * failbit before the caller reads anything. */
static MS int32_t mp_ist_ipfx(void *is, uint8_t noskip)
{
    mp_ios *b = mp_ios_of(is);
    if (!b) return 0;
    if (mp_ios_good(b)) {
        if (b->tiestr) mp_ost_flush(b->tiestr);
        if (!noskip && (b->fmtfl & MP_FMT_SKIPWS) && b->strbuf) {
            int c;
            while ((c = mp_sb_sgetc(b->strbuf)) != MP_EOF && isspace((unsigned char)c))
                mp_sb_sbumpc(b->strbuf);
            if (c == MP_EOF) mp_ios_setstate(b, MP_ST_EOF, 0);
        }
        if (mp_ios_good(b)) return 1;
    }
    mp_ios_setstate(b, MP_ST_FAIL, 0);
    return 0;
}
static MS int32_t mp_ist_get1(void *is)
{
    mp_ios *b = mp_ios_of(is);
    int c;
    if (!b || !b->strbuf) return MP_EOF;
    c = mp_sb_sbumpc(b->strbuf);
    *(int64_t *)((char *)is + 0x08) = (c == MP_EOF) ? 0 : 1;
    if (c == MP_EOF) mp_ios_setstate(b, MP_ST_EOF | MP_ST_FAIL, 0);
    return c;
}
static MS int32_t mp_ist_peek(void *is)
{
    mp_ios *b = mp_ios_of(is);
    int c;
    if (!b || !b->strbuf) return MP_EOF;
    c = mp_sb_sgetc(b->strbuf);
    if (c == MP_EOF) mp_ios_setstate(b, MP_ST_EOF, 0);
    return c;
}
static MS void *mp_ist_ignore(void *is, int64_t n, int32_t delim)
{
    mp_ios *b = mp_ios_of(is);
    int64_t done = 0;
    if (!b || !b->strbuf) return is;
    while (n < 0 || done < n) {
        int c = mp_sb_sbumpc(b->strbuf);
        if (c == MP_EOF) { mp_ios_setstate(b, MP_ST_EOF, 0); break; }
        done++;
        if (delim != MP_EOF && c == delim) break;
    }
    *(int64_t *)((char *)is + 0x08) = done;
    return is;
}
static MS void *mp_ist_seekg_dir(void *is, int64_t off, int32_t dir)
{
    mp_ios *b = mp_ios_of(is);
    if (b && b->strbuf) {
        int64_t (MS *seekoff)(void *, int64_t, int32_t, int32_t) =
            (int64_t (MS *)(void *, int64_t, int32_t, int32_t))
            mp_sb_slot(b->strbuf, MP_SB_SEEKOFF);
        if (seekoff && seekoff(b->strbuf, off, dir, 1) < 0)
            mp_ios_setstate(b, MP_ST_FAIL, 0);
    }
    return is;
}

/* _Getcoll fills a _Collvec the way _Getcvt fills a _Cvtvec. */
static MS void *mp_Getcoll(void *out)
{
    if (out) { memset(out, 0, 16); ((uint32_t *)out)[0] = 65001; }
    return out;
}
static size_t mp_collate_w_id_value;

/* The wide ctype range forms, as public members rather than virtuals. */
static MS const uint16_t *mp_ctw_tolower_p(void *self, uint16_t *a, const uint16_t *b)
{
    const uint16_t *(MS *f)(void *, uint16_t *, const uint16_t *) =
        (const uint16_t *(MS *)(void *, uint16_t *, const uint16_t *))
        mp_ct_slot(self, MP_CT_TOLOWER_RANGE);
    return f ? f(self, a, b) : b;
}
static MS const uint16_t *mp_ctw_toupper_p(void *self, uint16_t *a, const uint16_t *b)
{
    const uint16_t *(MS *f)(void *, uint16_t *, const uint16_t *) =
        (const uint16_t *(MS *)(void *, uint16_t *, const uint16_t *))
        mp_ct_slot(self, MP_CT_TOUPPER_RANGE);
    return f ? f(self, a, b) : b;
}

/* The narrow-name _Fiopen: VS2015 exports both spellings. */
static MS void *mp_Fiopen_a(const char *name, int32_t mode, int32_t prot)
{
    uint16_t w[1024];
    size_t i = 0;
    if (!name) return NULL;
    while (name[i] && i < 1023) { w[i] = (uint16_t)(unsigned char)name[i]; i++; }
    w[i] = 0;
    return mp_Fiopen(w, mode, prot);
}

/* ---- cout, cerr and cin -------------------------------------------------- */

/* Real objects, because they are imported as data: the plug-in gets their
 * address at load time and there is no later chance to build them. Each is a
 * stream over a streambuf that writes to the matching stdio stream, so a
 * plug-in's tracing actually comes out. */
typedef struct { void *vft; mp_streambuf_body b; FILE *f; } mp_stdiobuf;

static MS int32_t mp_stdio_v_overflow(void *self, int32_t c)
{
    mp_stdiobuf *sb = (mp_stdiobuf *)self;
    if (c == MP_EOF) return 0;
    if (!sb->f || fputc((unsigned char)c, sb->f) == EOF) return MP_EOF;
    return c & 0xFF;
}
static MS int64_t mp_stdio_v_xsputn(void *self, const char *in, int64_t n)
{
    mp_stdiobuf *sb = (mp_stdiobuf *)self;
    if (!sb->f || n <= 0) return 0;
    return (int64_t)fwrite(in, 1, (size_t)n, sb->f);
}
static MS int32_t mp_stdio_v_sync(void *self)
{
    mp_stdiobuf *sb = (mp_stdiobuf *)self;
    return (sb->f && fflush(sb->f) == 0) ? 0 : -1;
}
static MS int32_t mp_stdio_v_underflow(void *self)
{
    mp_stdiobuf *sb = (mp_stdiobuf *)self;
    int c;
    if (!sb->f) return MP_EOF;
    c = fgetc(sb->f);
    if (c == EOF) return MP_EOF;
    ungetc(c, sb->f);
    return c;
}
static MS int32_t mp_stdio_v_uflow(void *self)
{
    mp_stdiobuf *sb = (mp_stdiobuf *)self;
    int c = sb->f ? fgetc(sb->f) : EOF;
    return c == EOF ? MP_EOF : c;
}
static MS int64_t mp_stdio_v_xsgetn(void *self, char *out, int64_t n)
{
    mp_stdiobuf *sb = (mp_stdiobuf *)self;
    if (!sb->f || n <= 0) return 0;
    return (int64_t)fread(out, 1, (size_t)n, sb->f);
}
static void *mp_stdio_vft[MP_SB_NSLOTS] = {
    (void *)mp_facet_dtor,        (void *)mp_sb_lock,     (void *)mp_sb_unlock,
    (void *)mp_stdio_v_overflow,  (void *)mp_sb_v_pbackfail,
    (void *)mp_sb_v_showmanyc,    (void *)mp_stdio_v_underflow,
    (void *)mp_stdio_v_uflow,     (void *)mp_stdio_v_xsgetn, (void *)mp_stdio_v_xsputn,
    (void *)mp_sb_v_seekoff,      (void *)mp_sb_v_seekoff, (void *)mp_sb_v_setbuf,
    (void *)mp_stdio_v_sync,      (void *)mp_sb_v_imbue
};

static mp_stdiobuf mp_cout_buf, mp_cerr_buf, mp_clog_buf, mp_cin_buf;
static char mp_cout_obj[256], mp_cerr_obj[256], mp_clog_obj[256], mp_cin_obj[256];

static void mp_stdiobuf_init(mp_stdiobuf *sb, FILE *f)
{
    sb->vft = mp_stdio_vft;
    mp_sb_init(sb);
    sb->b.locale = mp_locale_Init(1);
    sb->f = f;
}
static void mp_std_streams_init(void) __attribute__((constructor));
static void mp_std_streams_init(void)
{
    mp_stdiobuf_init(&mp_cout_buf, stdout);
    mp_stdiobuf_init(&mp_cerr_buf, stderr);
    mp_stdiobuf_init(&mp_clog_buf, stderr);
    mp_stdiobuf_init(&mp_cin_buf,  stdin);
    mp_ostream_ctor(mp_cout_obj, &mp_cout_buf, 1, 1);
    mp_ostream_ctor(mp_cerr_obj, &mp_cerr_buf, 1, 1);
    mp_ostream_ctor(mp_clog_obj, &mp_clog_buf, 1, 1);
    mp_istream_ctor(mp_cin_obj,  &mp_cin_buf,  1, 1);
}

/* ---- the pieces with no layout ------------------------------------------ */

/* MSVC's mutex and condition objects are storage the plug-in embeds, sized by
 * _Mtx_internal_imp_size. A pthread object fits inside it at both widths; the
 * *_in_situ forms construct in place, which is what that size is for. */
/* MSVC's threading primitives, taken from the shapes msvcp120 itself builds.
 *
 * _Mtx_init calls calloc(1, 0x48) and writes the result *through* its argument,
 * because in VC12 _Mtx_t is an opaque void * and every entry point is handed
 * its address: the real _Mtx_lock, _Mtx_unlock, _Mtx_current_owns, _Mtx_destroy
 * and do_lock all open with `mov rax, [rcx]`. From VS2015 the mutex lives in the
 * caller's own storage and the same exported names take the object directly.
 * Both spellings arrive here under one registration, so rather than guess which
 * runtime a plug-in was built against, the objects say which they are: an object
 * we made carries a magic in its first eight bytes, and anything else is a slot
 * holding a pointer to one. Getting this wrong is not a subtle failure -- locking
 * the slot instead of the mutex blocks the only thread for ever.
 *
 * The field layout is msvcp120's: the lock sits at +0x08 where it keeps a
 * CRITICAL_SECTION, the owning thread at +0x40 and the recursion count at +0x44.
 * do_lock compares the owner before it touches the critical section and simply
 * counts up when the caller already holds it, so a recursive pthread mutex is
 * the faithful reading of _Mtx_plain here, not just the cautious one. */

#define MP_MTX_MAGIC 0x4d74784d70656c6fULL      /* "MtxMpelo" */
#define MP_CND_MAGIC 0x436e644d70656c6fULL      /* "CndMpelo" */

typedef struct {
    uint64_t magic;                              /* +0x00 */
    union { pthread_mutex_t m; char raw[56]; } u;/* +0x08, where MSVC's CS goes */
    uint32_t owner;                              /* +0x40 owning thread, -1 free */
    int32_t  count;                              /* +0x44 recursion depth */
    int32_t  type;                               /* _Mtx_plain / _try / _recursive */
} mp_mtx;                                        /* 80 bytes: fits VS2015 in-situ */

typedef struct {
    uint64_t magic;
    union { pthread_cond_t c; char raw[56]; } u;
} mp_cnd;

enum { MP_THRD_SUCCESS = 0, MP_THRD_NOMEM = 1, MP_THRD_TIMEDOUT = 2,
       MP_THRD_BUSY = 3, MP_THRD_ERROR = 4 };

static int mp_dbg(void)
{ static int v = -1; if (v < 0) v = getenv("PELOAD_MPDEBUG") != NULL; return v; }

static uint32_t mp_tid(void) { return (uint32_t)(uintptr_t)pthread_self(); }

/* A pointer worth dereferencing: NULL and the small integers a VS2015 in-situ
 * object starts with must not be followed. */
static int mp_ptrish(const void *p)
{ return (uintptr_t)p > 0xffff && ((uintptr_t)p & 7) == 0; }

static mp_mtx *mp_mtx_of(void *p)
{
    mp_mtx *o;
    if (!p) return NULL;
    if (*(uint64_t *)p == MP_MTX_MAGIC) return (mp_mtx *)p;   /* the object */
    o = *(mp_mtx **)p;                                        /* or a slot */
    if (!mp_ptrish(o) || o->magic != MP_MTX_MAGIC) return NULL;
    return o;
}
static mp_cnd *mp_cnd_of(void *p)
{
    mp_cnd *o;
    if (!p) return NULL;
    if (*(uint64_t *)p == MP_CND_MAGIC) return (mp_cnd *)p;
    o = *(mp_cnd **)p;
    if (!mp_ptrish(o) || o->magic != MP_CND_MAGIC) return NULL;
    return o;
}

static MS int32_t mp_Mtx_init_in_situ(void *p, int32_t type)
{
    mp_mtx *o = (mp_mtx *)p;
    pthread_mutexattr_t a;
    if (!o) return MP_THRD_ERROR;
    o->magic = MP_MTX_MAGIC;
    o->owner = 0xffffffffu;
    o->count = 0;
    o->type  = type;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&o->u.m, &a);
    pthread_mutexattr_destroy(&a);
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Mtx_destroy_in_situ(void *p)
{
    mp_mtx *o = mp_mtx_of(p);
    if (!o) return MP_THRD_SUCCESS;
    pthread_mutex_destroy(&o->u.m);
    o->magic = 0;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Mtx_lock(void *p)
{
    mp_mtx *o = mp_mtx_of(p);
    if (mp_dbg()) fprintf(stderr, "  [mp] Mtx_lock %p -> obj %p\n", p, (void *)o);
    if (!o) return MP_THRD_ERROR;
    if (pthread_mutex_lock(&o->u.m) != 0) return MP_THRD_ERROR;
    o->owner = mp_tid();
    o->count++;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Mtx_trylock(void *p)
{
    mp_mtx *o = mp_mtx_of(p);
    if (!o) return MP_THRD_ERROR;
    if (pthread_mutex_trylock(&o->u.m) != 0) return MP_THRD_BUSY;
    o->owner = mp_tid();
    o->count++;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Mtx_timedlock(void *p, const mp_xtime *t)
{
    mp_mtx *o = mp_mtx_of(p);
    struct timespec ts;
    if (!o) return MP_THRD_ERROR;
    if (!t) return mp_Mtx_lock(p);
    ts.tv_sec = (time_t)t->sec;
    ts.tv_nsec = t->nsec;
    if (pthread_mutex_timedlock(&o->u.m, &ts) != 0) return MP_THRD_TIMEDOUT;
    o->owner = mp_tid();
    o->count++;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Mtx_unlock(void *p)
{
    mp_mtx *o = mp_mtx_of(p);
    if (mp_dbg()) fprintf(stderr, "  [mp] Mtx_unlock %p -> obj %p\n", p, (void *)o);
    if (!o) return MP_THRD_ERROR;
    if (--o->count <= 0) { o->count = 0; o->owner = 0xffffffffu; }
    pthread_mutex_unlock(&o->u.m);
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Mtx_current_owns(void *p)
{
    mp_mtx *o = mp_mtx_of(p);
    return (o && o->count != 0 && o->owner == mp_tid()) ? 1 : 0;
}
static MS void mp_Mtx_clear_owner(void *p)
{ mp_mtx *o = mp_mtx_of(p); if (o) { o->owner = 0xffffffffu; o->count = 0; } }
static MS void mp_Mtx_reset_owner(void *p)
{ mp_mtx *o = mp_mtx_of(p); if (o) { o->owner = mp_tid(); o->count = 1; } }
/* The concurrency runtime asks for the critical section inside the mutex. */
static MS void *mp_Mtx_getconcrtcs(void *p)
{ mp_mtx *o = mp_mtx_of(p); return o ? (void *)&o->u : NULL; }

static MS int32_t mp_Mtx_init(void **out, int32_t type)
{
    mp_mtx *o;
    if (out) *out = NULL;
    o = (mp_mtx *)w32_alloc(sizeof *o, 1);
    if (!o) return MP_THRD_NOMEM;
    mp_Mtx_init_in_situ(o, type);
    if (out) *out = o;
    if (mp_dbg()) fprintf(stderr, "  [mp] Mtx_init slot %p -> obj %p (type %d)\n",
                          (void *)out, (void *)o, type);
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Mtx_destroy(void *p)
{
    mp_mtx *o = mp_mtx_of(p);
    if (!o) return MP_THRD_SUCCESS;
    mp_Mtx_destroy_in_situ(o);
    if ((void *)o != p) { w32_free(o); *(void **)p = NULL; }
    return MP_THRD_SUCCESS;
}

static MS int32_t mp_Cnd_init_in_situ(void *p)
{
    mp_cnd *o = (mp_cnd *)p;
    if (!o) return MP_THRD_ERROR;
    o->magic = MP_CND_MAGIC;
    pthread_cond_init(&o->u.c, NULL);
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Cnd_destroy_in_situ(void *p)
{
    mp_cnd *o = mp_cnd_of(p);
    if (!o) return MP_THRD_SUCCESS;
    pthread_cond_destroy(&o->u.c);
    o->magic = 0;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Cnd_init(void **out)
{
    mp_cnd *o;
    if (out) *out = NULL;
    o = (mp_cnd *)w32_alloc(sizeof *o, 1);
    if (!o) return MP_THRD_NOMEM;
    mp_Cnd_init_in_situ(o);
    if (out) *out = o;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Cnd_destroy(void *p)
{
    mp_cnd *o = mp_cnd_of(p);
    if (!o) return MP_THRD_SUCCESS;
    mp_Cnd_destroy_in_situ(o);
    if ((void *)o != p) { w32_free(o); *(void **)p = NULL; }
    return MP_THRD_SUCCESS;
}
/* The wait drops the mutex, so the owner and depth we are holding on its behalf
 * have to come off with it and go back on when the wait returns. A depth above
 * one would be a std::condition_variable waiting on a lock it holds twice, which
 * the C++ library does not do. */
static MS int32_t mp_Cnd_wait(void *cp, void *mp)
{
    mp_cnd *c = mp_cnd_of(cp);
    mp_mtx *m = mp_mtx_of(mp);
    int32_t depth;
    if (!c || !m) return MP_THRD_ERROR;
    depth = m->count;
    m->count = 0;
    m->owner = 0xffffffffu;
    pthread_cond_wait(&c->u.c, &m->u.m);
    m->owner = mp_tid();
    m->count = depth;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Cnd_timedwait(void *cp, void *mp, const mp_xtime *t)
{
    mp_cnd *c = mp_cnd_of(cp);
    mp_mtx *m = mp_mtx_of(mp);
    struct timespec ts;
    int32_t depth;
    int r;
    if (!c || !m) return MP_THRD_ERROR;
    if (!t) return mp_Cnd_wait(cp, mp);
    ts.tv_sec = (time_t)t->sec;
    ts.tv_nsec = t->nsec;
    depth = m->count;
    m->count = 0;
    m->owner = 0xffffffffu;
    r = pthread_cond_timedwait(&c->u.c, &m->u.m, &ts);
    m->owner = mp_tid();
    m->count = depth;
    return r == 0 ? MP_THRD_SUCCESS : MP_THRD_TIMEDOUT;
}
static MS int32_t mp_Cnd_signal(void *cp)
{ mp_cnd *c = mp_cnd_of(cp); return c ? (pthread_cond_signal(&c->u.c), MP_THRD_SUCCESS) : MP_THRD_ERROR; }
static MS int32_t mp_Cnd_broadcast(void *cp)
{ mp_cnd *c = mp_cnd_of(cp); return c ? (pthread_cond_broadcast(&c->u.c), MP_THRD_SUCCESS) : MP_THRD_ERROR; }
/* notify_all_at_thread_exit. Nothing here registers a condition to broadcast on
 * the way out, so the pair is a matched no-op rather than a missing symbol. */
static MS void mp_Cnd_register_at_thread_exit(void *c, void *m, int32_t *done)
{ (void)c; (void)m; (void)done; }
static MS void mp_Cnd_unregister_at_thread_exit(void *m) { (void)m; }
static MS void mp_Cnd_do_broadcast_at_thread_exit(void) { }

/* _Thrd_t is { void *handle; unsigned id; } -- _Thrd_equal compares [rcx+8]
 * against [rdx+8], and _Thrd_join waits on [rcx] then reads the exit code. It is
 * sixteen bytes, so the MS ABI hands it about by address in both directions. */
typedef struct { void *hnd; uint32_t id; } mp_thrd;

typedef int32_t (MS *mp_thrd_start_t)(void *);
typedef struct { mp_thrd_start_t fn; void *arg; } mp_thrd_launch;

/* A guest thread needs its TEB before it runs a single instruction of plug-in
 * code: MSVC's __chkstk reads the stack limit from gs:0x10 on every function
 * with a frame over a page, and without a TEB there that is a wild pointer it
 * then writes zeros through. This is the same trampoline contract the Win32
 * CreateThread path uses. */
static void *mp_thrd_tramp(void *v)
{
    mp_thrd_launch l = *(mp_thrd_launch *)v;
    free(v);
    teb_install();
    return (void *)(intptr_t)(l.fn ? l.fn(l.arg) : 0);
}
static MS int32_t mp_Thrd_create(mp_thrd *thr, mp_thrd_start_t fn, void *arg)
{
    mp_thrd_launch *l;
    pthread_t t;
    if (!thr || !fn) return MP_THRD_ERROR;
    if (!(l = (mp_thrd_launch *)malloc(sizeof *l))) return MP_THRD_NOMEM;
    l->fn = fn;
    l->arg = arg;
    if (pthread_create(&t, NULL, mp_thrd_tramp, l) != 0) { free(l); return MP_THRD_ERROR; }
    thr->hnd = (void *)t;
    thr->id  = (uint32_t)(uintptr_t)t;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Thrd_start(mp_thrd *thr, mp_thrd_start_t fn, void *arg)
{ return mp_Thrd_create(thr, fn, arg); }
static MS void *mp_Thrd_current(mp_thrd *out)
{
    if (out) { out->hnd = (void *)pthread_self(); out->id = mp_tid(); }
    return out;
}
static MS uint32_t mp_Thrd_id(void) { return mp_tid(); }
static MS int32_t mp_Thrd_equal(const mp_thrd *a, const mp_thrd *b)
{ return (a && b && a->id == b->id) ? 1 : 0; }
static MS int32_t mp_Thrd_lt(const mp_thrd *a, const mp_thrd *b)
{ return (a && b && a->id < b->id) ? 1 : 0; }
static MS int32_t mp_Thrd_join(mp_thrd *thr, int32_t *res)
{
    void *rv = NULL;
    if (!thr || !thr->hnd) return MP_THRD_ERROR;
    if (pthread_join((pthread_t)thr->hnd, &rv) != 0) return MP_THRD_ERROR;
    if (res) *res = (int32_t)(intptr_t)rv;
    thr->hnd = NULL;
    return MP_THRD_SUCCESS;
}
static MS int32_t mp_Thrd_detach(mp_thrd *thr)
{
    if (!thr || !thr->hnd) return MP_THRD_ERROR;
    pthread_detach((pthread_t)thr->hnd);
    thr->hnd = NULL;
    return MP_THRD_SUCCESS;
}
static MS void mp_Thrd_exit(int32_t code)
{ pthread_exit((void *)(intptr_t)code); }
static MS void mp_Thrd_yield(void) { sched_yield(); }
/* _Thrd_sleep takes an absolute xtime, not a duration. */
static MS int32_t mp_Thrd_sleep(const mp_xtime *t)
{
    struct timespec ts;
    if (!t) return MP_THRD_ERROR;
    ts.tv_sec = (time_t)t->sec;
    ts.tv_nsec = t->nsec;
    while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
    return MP_THRD_SUCCESS;
}

/* ---- _Pad: how std::thread gets a thread started ------------------------- */

/* Read straight out of msvcp120. The constructor at 0x3d06c writes its vftable,
 * calls _Cnd_init on +0x08 and _Mtx_init(+0x10, _Mtx_plain), clears the flag at
 * +0x18 and then *takes* the mutex -- the destructor's first act is to unlock
 * it, which is how we know. _Launch hands _Thrd_start the entry at 0x3d238 and
 * then loops `while (!_Started) _Cnd_wait(&_Cond, &_Mtx)`; that entry calls
 * vftable slot 0, the pure _Go the plug-in's thread body overrides, and then
 * _Cnd_do_broadcast_at_thread_exit. _Release, on the new thread, takes the
 * mutex, sets the flag, signals and drops it.
 *
 * The handshake is the whole point: it keeps the launching thread inside
 * _Launch until the new one has copied whatever the _Pad was holding for it. */
typedef struct {
    void    *vftable;      /* +0x00, _Go at slot 0 */
    void    *cond;         /* +0x08  _Cnd_t slot */
    void    *mtx;          /* +0x10  _Mtx_t slot */
    uint8_t  started;      /* +0x18 */
} mp_pad;

static MS int32_t mp_pad_pure(void *self)
{
    (void)self;
    fprintf(stderr, "  [mp] _Pad::_Go called on a base _Pad -- no thread body\n");
    return 0;
}
static void *mp_pad_vft[1] = { (void *)mp_pad_pure };

static MS int32_t mp_pad_call(void *arg)
{
    mp_pad *p = (mp_pad *)arg;
    int32_t r = 0;
    if (p && p->vftable) {
        int32_t (MS *go)(void *) = (int32_t (MS *)(void *))((void **)p->vftable)[0];
        if (go) r = go(p);
    }
    mp_Cnd_do_broadcast_at_thread_exit();
    return r;
}
static MS void *mp_pad_ctor(mp_pad *self)
{
    if (!self) return self;
    self->vftable = mp_pad_vft;
    mp_Cnd_init(&self->cond);
    mp_Mtx_init(&self->mtx, 1);          /* _Mtx_plain */
    self->started = 0;
    mp_Mtx_lock(&self->mtx);
    return self;
}
static MS void mp_pad_dtor(mp_pad *self)
{
    if (!self) return;
    self->vftable = mp_pad_vft;
    mp_Mtx_unlock(&self->mtx);
    mp_Mtx_destroy(&self->mtx);
    mp_Cnd_destroy(&self->cond);
}
static MS void mp_pad_launch(mp_pad *self, mp_thrd *thr)
{
    if (!self || !thr) return;
    if (mp_Thrd_start(thr, mp_pad_call, self) != MP_THRD_SUCCESS) return;
    while (!self->started)
        mp_Cnd_wait(&self->cond, &self->mtx);
}
static MS void mp_pad_release(mp_pad *self)
{
    if (!self) return;
    mp_Mtx_lock(&self->mtx);
    self->started = 1;
    mp_Cnd_signal(&self->cond);
    mp_Mtx_unlock(&self->mtx);
}


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
