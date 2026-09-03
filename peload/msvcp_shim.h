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
    if (out) *(void **)out = b ? b->locale : NULL;
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
static MS void *mp_istream_extract(void *self, void *out)
{
    (void)out;
    if (self) {
        const int32_t *vb = *(const int32_t **)self;
        int32_t off = vb ? vb[1] : 32;
        mp_ios *b = (mp_ios *)((char *)self + off);
        b->state |= 1 | 2;                         /* eofbit | failbit */
    }
    return self;
}

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
