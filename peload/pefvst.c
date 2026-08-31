/* Calling a Classic Mac OS VST plug-in.
 *
 * The plug-in's entry point returns an AEffect, and everything after that is
 * calling function pointers inside it. Three things make that different from
 * calling a Windows or Linux VST, and all three are easy to get subtly wrong:
 *
 *   - The AEffect lives in the guest's memory and is big-endian, so every field
 *     is read a word at a time rather than by casting a host struct over it. Its
 *     pointer fields are 32-bit, which is another reason a host struct will not
 *     do even on a big-endian host.
 *   - Its function pointers are TVectors, not code addresses. Calling one means
 *     loading the code address and the callee's TOC out of a two-word structure
 *     and setting r2 as well as the program counter -- the same thing the guest's
 *     own cross-TOC glue does.
 *   - The audio buffers are arrays of pointers to arrays of floats, all in guest
 *     memory and all big-endian, so each block is marshalled in and out. That
 *     copy is the price of running the thing at all.
 *
 * A call is made by pointing the program counter at the callee with the link
 * register set to an address that is never code; the interpreter runs until it
 * gets there, which is exactly what returning does. If the guest calls back into
 * the host on the way -- audioMaster, or anything from InterfaceLib -- the trap
 * window handles it and execution carries on, so re-entrancy needs no special
 * case here.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pefvst.h"

/* Where a call returns to. Not a valid instruction address, and outside the trap
 * window, so nothing else can land on it. */
#define RETURN_TO   0x0FFF0000u

/* The AEffect's layout, as offsets. */
#define AE_MAGIC        0
#define AE_DISPATCHER   4
#define AE_PROCESS      8
#define AE_SET_PARAM   12
#define AE_GET_PARAM   16
#define AE_NUM_PROGRAMS 20
#define AE_NUM_PARAMS  24
#define AE_NUM_INPUTS  28
#define AE_NUM_OUTPUTS 32
#define AE_FLAGS       36
#define AE_INITIAL_DELAY 48
#define AE_UNIQUE_ID   72
#define AE_VERSION     76
#define AE_PROCESS_REPLACING 80
#define AE_SIZE       144

#define AE_MAGIC_VALUE  0x56737450u          /* 'VstP' */

#define MAX_CHANNELS 32
#define BUDGET       200000000u              /* per call, before calling it a runaway */

struct pefvst {
    ppc      *m;
    pef      *p;
    cfm      *c;
    int       owns;                          /* whether close() frees the above */

    uint32_t  ae;                            /* the AEffect, in guest memory */
    int       inputs, outputs, params, programs, flags;
    int       unique_id, version;

    double    rate;
    int       block;

    /* Marshalling buffers, in guest memory: an array of channel pointers and the
     * channels themselves, allocated once. */
    uint32_t  in_vec, out_vec;
    uint32_t  in_buf[MAX_CHANNELS], out_buf[MAX_CHANNELS];
    uint32_t  scratch;                       /* 512 bytes for strings and rects */
    uint32_t  events;                        /* a VstEvents block for one note */

    int       editor_open;
    int       mouse_down;                    /* to spot the press transition */
    /* Set while guest code is running, with the thread that is running it. A
     * modal drag loop pumps the host from inside the guest, so a mouse event can
     * arrive while the guest is mid-call -- and dispatching again from there
     * would both re-enter the plugin and deadlock on the lock below. */
    int       in_guest;
    pthread_t guest_thread;
    char      err[256];

    /* There is one interpreter, and the host calls into it from two threads: the
     * audio thread for each block and the GUI thread for the editor. Both run
     * guest code, which means both write the same program counter and registers,
     * so every entry into the guest is serialised here. A block waiting on the
     * editor is a glitch; two threads interleaving inside ppc_run is corruption,
     * and the trade is not close. */
    pthread_mutex_t lock;
};

/* --------------------------------------------------------------- calling in */

static uint32_t gw(pefvst *v, uint32_t a)             { return ppc_read32(v->m, a); }
static void     sw(pefvst *v, uint32_t a, uint32_t x) { ppc_write32(v->m, a, x); }

static int fail(pefvst *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->err, sizeof v->err, fmt, ap);
    va_end(ap);
    return -1;
}

/* Call a TVector in the guest. `gpr` holds r3..r10 and `fpr` f1; either may be
 * short, in which case the rest are left at zero. Returns 0 on success. */
static int call_tv(pefvst *v, uint32_t tvector, const uint32_t *gpr, int ngpr,
                   const double *fpr, int nfpr)
{
    ppc *m = v->m;
    uint32_t code, toc;
    int i;

    if (!tvector) return fail(v, "the plug-in left this entry point null");

    pthread_mutex_lock(&v->lock);
    code = gw(v, tvector);
    toc  = gw(v, tvector + 4);
    if (!code) {
        pthread_mutex_unlock(&v->lock);
        return fail(v, "the TVector at 0x%08x has no code address", tvector);
    }

    /* A fresh frame every call. The callee's prologue writes the return address
     * into the caller's linkage area at 8(r1), so there must be valid memory
     * above the stack pointer, which is why the stack top is not the very top of
     * the address space. */
    m->r[1] = PEF_STACK;
    sw(v, PEF_STACK, 0);                     /* the back chain ends here */

    for (i = 0; i < 8; i++) m->r[3 + i] = (i < ngpr) ? gpr[i] : 0;
    for (i = 0; i < 8; i++) m->f[1 + i].d = (i < nfpr) ? fpr[i] : 0.0;

    m->r[2] = toc;
    m->lr = RETURN_TO;
    m->pc = code;
    m->err[0] = 0;

    v->in_guest = 1;
    v->guest_thread = pthread_self();
    ppc_run(m, RETURN_TO, BUDGET);
    v->in_guest = 0;
    {
        /* Everything wanted from the machine has to be taken while the lock is
         * still held. Copying the error *message* matters as much as the flag:
         * another thread entering here would run the guest and overwrite
         * m->err, so reading it afterwards can report someone else's failure. */
        char why[sizeof m->err];
        uint32_t where = m->pc;
        int bad = m->err[0] != 0;
        if (bad) memcpy(why, m->err, sizeof why);
        pthread_mutex_unlock(&v->lock);
        if (bad) return fail(v, "%s", why);
        if (where != RETURN_TO)
            return fail(v, "the guest stopped at 0x%08x instead of returning", where);
    }
    return 0;
}

/* ------------------------------------------------------------- floats in guest */

static void put_float(pefvst *v, uint32_t at, float f)
{
    union { uint32_t u; float f; } cv;
    cv.f = f;
    sw(v, at, cv.u);
}

static float get_float(pefvst *v, uint32_t at)
{
    union { uint32_t u; float f; } cv;
    cv.u = gw(v, at);
    return cv.f;
}

/* ------------------------------------------------------------------- opening */

static int adopt(pefvst *v, uint32_t ae, char *err, int errlen)
{
    uint32_t magic;
    int i;

    if (!ae) {
        /* A plug-in returns null from its entry point when it decides not to
         * start. The commonest reason by far is that it looked for a settings or
         * authorisation file and did not find one, so say where it would look --
         * that is the actionable part. */
        snprintf(err, (size_t)errlen,
                 "the plug-in declined to start: its entry point returned no "
                 "AEffect. A plug-in does this when it will not run in the "
                 "environment it found -- most often because a settings or "
                 "authorisation file it expects is not in the support directory");
        return -1;
    }
    magic = gw(v, ae + AE_MAGIC);
    if (magic != AE_MAGIC_VALUE) {
        snprintf(err, (size_t)errlen,
                 "what came back is not an AEffect: its magic is 0x%08x, not 'VstP'",
                 magic);
        return -1;
    }
    v->ae       = ae;
    v->programs = (int)gw(v, ae + AE_NUM_PROGRAMS);
    v->params   = (int)gw(v, ae + AE_NUM_PARAMS);
    v->inputs   = (int)gw(v, ae + AE_NUM_INPUTS);
    v->outputs  = (int)gw(v, ae + AE_NUM_OUTPUTS);
    v->flags    = (int)gw(v, ae + AE_FLAGS);
    v->unique_id = (int)gw(v, ae + AE_UNIQUE_ID);
    v->version  = (int)gw(v, ae + AE_VERSION);

    if (v->inputs < 0 || v->inputs > MAX_CHANNELS ||
        v->outputs < 0 || v->outputs > MAX_CHANNELS) {
        snprintf(err, (size_t)errlen, "%d inputs and %d outputs, which is not "
                 "a plausible plug-in", v->inputs, v->outputs);
        return -1;
    }
    if (v->params < 0 || v->params > 8192) v->params = 0;

    /* The marshalling buffers, allocated once. The channel-pointer arrays and the
     * channels themselves are separate blocks because that is what the guest
     * expects to walk: a pointer per channel, each to `block` floats. */
    {
        uint32_t need = (uint32_t)v->block * 4u;
        v->in_vec  = cfm_guest_alloc(v->c, (uint32_t)(v->inputs  + 1) * 4u, 1);
        v->out_vec = cfm_guest_alloc(v->c, (uint32_t)(v->outputs + 1) * 4u, 1);
        v->scratch = cfm_guest_alloc(v->c, 1024, 1);
        v->events  = cfm_guest_alloc(v->c, 256, 1);
        if (!v->in_vec || !v->out_vec || !v->scratch || !v->events) {
            snprintf(err, (size_t)errlen, "out of guest memory for the buffers");
            return -1;
        }
        for (i = 0; i < v->inputs; i++) {
            if (!(v->in_buf[i] = cfm_guest_alloc(v->c, need, 1))) {
                snprintf(err, (size_t)errlen, "out of guest memory for input %d", i);
                return -1;
            }
            sw(v, v->in_vec + (uint32_t)i * 4, v->in_buf[i]);
        }
        for (i = 0; i < v->outputs; i++) {
            if (!(v->out_buf[i] = cfm_guest_alloc(v->c, need, 1))) {
                snprintf(err, (size_t)errlen, "out of guest memory for output %d", i);
                return -1;
            }
            sw(v, v->out_vec + (uint32_t)i * 4, v->out_buf[i]);
        }
    }

    /* The opening sequence a host owes a VST 1.0 plug-in, in order. Note that
     * effSetSampleRate carries the rate in `opt` and effSetBlockSize the size in
     * `value` -- they do not agree with each other, and swapping them leaves a
     * plug-in running at whatever it defaulted to. */
    pefvst_dispatch(v, PV_OPEN, 0, 0, 0, 0.0f);
    pefvst_dispatch(v, PV_SET_SAMPLE_RATE, 0, 0, 0, (float)v->rate);
    pefvst_dispatch(v, PV_SET_BLOCK_SIZE, 0, (int32_t)v->block, 0, 0.0f);
    pefvst_dispatch(v, PV_MAINS_CHANGED, 0, 1, 0, 0.0f);
    if (v->err[0]) { snprintf(err, (size_t)errlen, "%s", v->err); return -1; }
    return 0;
}

pefvst *pefvst_attach(ppc *m, pef *p, cfm *c, uint32_t aeffect,
                      double rate, int block, char *err, int errlen)
{
    pefvst *v = calloc(1, sizeof *v);

    if (!v) { snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
    v->m = m; v->p = p; v->c = c;
    pthread_mutex_init(&v->lock, NULL);
    v->rate = rate > 0 ? rate : 44100.0;
    v->block = block > 0 ? block : 512;
    if (adopt(v, aeffect, err, errlen)) {
        if (v->err[0] && err && errlen) snprintf(err, (size_t)errlen, "%s", v->err);
        pthread_mutex_destroy(&v->lock);
        free(v);
        return NULL;
    }
    return v;
}

pefvst *pefvst_open(const uint8_t *pefbytes, uint32_t peflen,
                    const uint8_t *fork, uint32_t forklen,
                    const char *support_dir, double rate, int block,
                    char *err, int errlen)
{
    ppc *m = NULL;
    pef *p = NULL;
    cfm *c = NULL;
    pefvst *v = NULL;
    uint32_t cb, ae;
    uint8_t *copy = NULL;

    if (err && errlen) err[0] = 0;
    if (!(m = ppc_new(PEF_MEMSIZE))) {
        snprintf(err, (size_t)errlen, "out of memory for the guest");
        return NULL;
    }
    /* The loader reads from the caller's buffer but relocation and everything
     * after it reads from guest memory, so a copy is not needed for correctness
     * -- but pef keeps no reference to the file either, so nothing to keep. */
    (void)copy;

    if (!(p = pef_load(pefbytes, peflen, m, NULL, PEF_MAX_IMPORTS))) {
        snprintf(err, (size_t)errlen, "%s", pef_last_error());
        ppc_free(m);
        return NULL;
    }
    if (!(c = cfm_new(m, p, support_dir))) {
        snprintf(err, (size_t)errlen, "could not bind the system libraries");
        pef_free(p); ppc_free(m);
        return NULL;
    }
    if (fork && forklen) cfm_set_resource_fork(c, fork, forklen);

    /* audioMaster, as a TVector the guest can call. */
    if (!(cb = pef_host_callback(p, 0))) {
        snprintf(err, (size_t)errlen, "%s", pef_last_error());
        cfm_free(c); pef_free(p); ppc_free(m);
        return NULL;
    }
    cfm_set_audiomaster(c, 0, rate > 0 ? rate : 44100.0, block > 0 ? block : 512);

    if (!p->main_code) {
        snprintf(err, (size_t)errlen, "the fragment has no entry point");
        cfm_free(c); pef_free(p); ppc_free(m);
        return NULL;
    }

    /* Call main(audioMaster). */
    {
        pefvst tmp;
        int failed;
        memset(&tmp, 0, sizeof tmp);
        tmp.m = m; tmp.p = p; tmp.c = c;
        /* call_tv takes the lock, so this throwaway needs a real mutex. Zeroed
         * memory happens to be a valid unlocked mutex on glibc, which is exactly
         * why leaving it out would go unnoticed here and break elsewhere. */
        pthread_mutex_init(&tmp.lock, NULL);
        failed = call_tv(&tmp, p->main_tvector, &cb, 1, NULL, 0) != 0;
        if (failed) snprintf(err, (size_t)errlen, "calling the entry point: %s",
                             tmp.err);
        pthread_mutex_destroy(&tmp.lock);
        if (failed) { cfm_free(c); pef_free(p); ppc_free(m); return NULL; }
        ae = m->r[3];
    }

    if (!(v = pefvst_attach(m, p, c, ae, rate, block, err, errlen))) {
        cfm_free(c); pef_free(p); ppc_free(m);
        return NULL;
    }
    v->owns = 1;
    return v;
}

void pefvst_close(pefvst *v)
{
    if (!v) return;
    if (v->ae) {
        if (v->editor_open) pefvst_dispatch(v, PV_EDIT_CLOSE, 0, 0, 0, 0.0f);
        pefvst_dispatch(v, PV_MAINS_CHANGED, 0, 0, 0, 0.0f);
        pefvst_dispatch(v, PV_CLOSE, 0, 0, 0, 0.0f);
    }
    if (v->owns) { cfm_free(v->c); pef_free(v->p); ppc_free(v->m); }
    pthread_mutex_destroy(&v->lock);
    free(v);
}

int pefvst_inputs(pefvst *v)   { return v ? v->inputs : 0; }
int pefvst_outputs(pefvst *v)  { return v ? v->outputs : 0; }
int pefvst_params(pefvst *v)   { return v ? v->params : 0; }
int pefvst_programs(pefvst *v) { return v ? v->programs : 0; }
int pefvst_flags(pefvst *v)    { return v ? v->flags : 0; }
int pefvst_unique_id(pefvst *v){ return v ? v->unique_id : 0; }
int pefvst_version(pefvst *v)  { return v ? v->version : 0; }
const char *pefvst_error(pefvst *v) { return v && v->err[0] ? v->err : NULL; }
uint64_t pefvst_icount(pefvst *v)   { return v ? v->m->icount : 0; }
uint32_t pefvst_scratch(pefvst *v)  { return v ? v->scratch : 0; }
ppc     *pefvst_machine(pefvst *v)  { return v ? v->m : NULL; }

/* ---------------------------------------------------------------- dispatcher */

int32_t pefvst_dispatch(pefvst *v, int opcode, int index, int32_t value,
                        uint32_t ptr, float opt)
{
    uint32_t g[5];
    double f[1];

    if (!v || !v->ae) return 0;
    g[0] = v->ae;
    g[1] = (uint32_t)opcode;
    g[2] = (uint32_t)index;
    g[3] = (uint32_t)value;
    g[4] = ptr;
    f[0] = (double)opt;
    if (call_tv(v, gw(v, v->ae + AE_DISPATCHER), g, 5, f, 1)) return 0;
    return (int32_t)v->m->r[3];
}

int pefvst_string(pefvst *v, int opcode, int index, char *out, int n)
{
    int i;

    if (!v || !out || n <= 0) return 0;
    out[0] = 0;
    /* The plug-in writes into a buffer the host supplies; VST 1.0 promises at
     * most 64 bytes for these, and the scratch block is far larger. */
    for (i = 0; i < 64; i++) ppc_write8(v->m, v->scratch + (uint32_t)i, 0);
    pefvst_dispatch(v, opcode, index, 0, v->scratch, 0.0f);
    for (i = 0; i < n - 1 && i < 64; i++) {
        uint8_t ch = ppc_read8(v->m, v->scratch + (uint32_t)i);
        if (!ch) break;
        out[i] = (char)ch;
    }
    out[i] = 0;
    return i > 0;
}

/* ----------------------------------------------------------------- parameters */

float pefvst_get_param(pefvst *v, int index)
{
    uint32_t g[2];

    if (!v || !v->ae || index < 0 || index >= v->params) return 0.0f;
    g[0] = v->ae; g[1] = (uint32_t)index;
    if (call_tv(v, gw(v, v->ae + AE_GET_PARAM), g, 2, NULL, 0)) return 0.0f;
    /* A float result comes back in f1. */
    return (float)v->m->f[1].d;
}

void pefvst_set_param(pefvst *v, int index, float value)
{
    uint32_t g[2];
    double f[1];

    if (!v || !v->ae || index < 0 || index >= v->params) return;
    g[0] = v->ae; g[1] = (uint32_t)index;
    f[0] = (double)value;
    call_tv(v, gw(v, v->ae + AE_SET_PARAM), g, 2, f, 1);
}

/* -------------------------------------------------------------------- audio */

int pefvst_process(pefvst *v, const float *const *in, float *const *out,
                   int frames)
{
    uint32_t g[4], fn;
    int ch, i, replacing;

    if (!v || !v->ae) return -1;
    if (frames <= 0) return 0;
    if (frames > v->block)
        return fail(v, "asked for %d frames but the plug-in was told %d",
                    frames, v->block);

    /* Copy the input in, converting to the guest's byte order. */
    for (ch = 0; ch < v->inputs; ch++) {
        const float *src = in ? in[ch] : NULL;
        for (i = 0; i < frames; i++)
            put_float(v, v->in_buf[ch] + (uint32_t)i * 4, src ? src[i] : 0.0f);
    }
    /* processReplacing overwrites the output, but process *accumulates* into it,
     * so the output must start at silence either way for the result to mean what
     * the caller expects. */
    for (ch = 0; ch < v->outputs; ch++)
        for (i = 0; i < frames; i++)
            sw(v, v->out_buf[ch] + (uint32_t)i * 4, 0);

    replacing = (v->flags & PV_FLAG_CAN_REPLACING) != 0;
    fn = gw(v, v->ae + (replacing ? AE_PROCESS_REPLACING : AE_PROCESS));
    if (!fn && replacing) { fn = gw(v, v->ae + AE_PROCESS); replacing = 0; }
    if (!fn) return fail(v, "the plug-in has neither process nor processReplacing");

    g[0] = v->ae;
    g[1] = v->in_vec;
    g[2] = v->out_vec;
    g[3] = (uint32_t)frames;
    if (call_tv(v, fn, g, 4, NULL, 0)) return -1;

    for (ch = 0; ch < v->outputs; ch++) {
        float *dst = out ? out[ch] : NULL;
        if (!dst) continue;
        for (i = 0; i < frames; i++)
            dst[i] = get_float(v, v->out_buf[ch] + (uint32_t)i * 4);
    }
    return 0;
}

/* --------------------------------------------------------------------- notes */

/* A VstEvents block holding one VstMidiEvent, laid out in guest memory:
 *
 *   VstEvents     numEvents(4) reserved(4) events[1](4)
 *   VstMidiEvent  type(4) byteSize(4) deltaFrames(4) flags(4)
 *                 noteLength(4) noteOffset(4) midiData[4] detune(1)
 *                 noteOffVelocity(1) reserved1(1) reserved2(1)
 */
void pefvst_note(pefvst *v, int on, int key, int velocity)
{
    uint32_t ev, me;

    if (!v || !v->ae || !v->events) return;
    ev = v->events;
    me = v->events + 64;

    sw(v, ev, 1);                            /* numEvents */
    sw(v, ev + 4, 0);
    sw(v, ev + 8, me);

    sw(v, me, 1);                            /* kVstMidiType */
    sw(v, me + 4, 32);                       /* byteSize     */
    sw(v, me + 8, 0);                        /* deltaFrames  */
    sw(v, me + 12, 0);                       /* flags        */
    sw(v, me + 16, 0);                       /* noteLength   */
    sw(v, me + 20, 0);                       /* noteOffset   */
    ppc_write8(v->m, me + 24, (uint8_t)(on ? 0x90 : 0x80));
    ppc_write8(v->m, me + 25, (uint8_t)(key & 0x7F));
    ppc_write8(v->m, me + 26, (uint8_t)(on ? (velocity & 0x7F) : 0));
    ppc_write8(v->m, me + 27, 0);
    sw(v, me + 28, 0);

    pefvst_dispatch(v, PV_PROCESS_EVENTS, 0, 0, ev, 0.0f);
}

/* -------------------------------------------------------------------- editor */

int pefvst_editor_size(pefvst *v, int *w, int *h)
{
    uint32_t rectp;

    if (!v || !v->ae || !(v->flags & PV_FLAG_HAS_EDITOR)) return 0;
    /* effEditGetRect hands back a pointer to a Rect through `ptr`, so what the
     * scratch word receives is an address, not the rectangle itself. */
    sw(v, v->scratch, 0);
    pefvst_dispatch(v, PV_EDIT_GET_RECT, 0, 0, v->scratch, 0.0f);
    if (!(rectp = gw(v, v->scratch))) return 0;
    {
        int rw = (int16_t)ppc_read16(v->m, rectp + 6) -
                 (int16_t)ppc_read16(v->m, rectp + 2);
        int rh = (int16_t)ppc_read16(v->m, rectp + 4) -
                 (int16_t)ppc_read16(v->m, rectp);
        if (rw <= 0 || rh <= 0 || rw > 8192 || rh > 8192)
            return fail(v, "the plug-in asked for a %dx%d editor", rw, rh), 0;
        /* Telling the shim the size is what lets it pick the right offscreen
         * out of the several a plug-in makes. */
        cfm_set_editor_size(v->c, rw, rh);
        if (w) *w = rw;
        if (h) *h = rh;
    }
    return 1;
}

int pefvst_editor_open(pefvst *v)
{
    int w = 0, h = 0;

    if (!v || !v->ae || !(v->flags & PV_FLAG_HAS_EDITOR)) return 0;
    /* Ask the size first: it is what lets the shim tell the window's offscreen
     * from the artwork the window is composited out of. */
    pefvst_editor_size(v, &w, &h);
    /* A Classic editor is given the window it should draw into, and refuses to
     * open without one -- so provide an offscreen that really is a colour
     * GrafPort. What it draws there is what gets read back. */
    {
        uint32_t win = cfm_editor_window(v->c, w > 0 ? w : 400, h > 0 ? h : 300);
        int32_t r = pefvst_dispatch(v, PV_EDIT_OPEN, 0, 0, win, 0.0f);
        if (!r && !v->err[0])
            /* Not fatal: some plug-ins return nothing useful here. Say so once
             * rather than treating it as success or as failure. */
            fprintf(stderr, "peload: the plug-in's effEditOpen returned 0 -- its "
                            "editor may not be fully set up\n");
    }
    if (v->err[0]) return 0;
    v->editor_open = 1;
    pefvst_dispatch(v, PV_EDIT_IDLE, 0, 0, 0, 0.0f);
    pefvst_editor_draw(v);
    return 1;
}

/* effEditDraw: tell the plug-in to paint, and where.
 *
 * On Mac the host owns the editor's window, so the plug-in does not paint when
 * it feels like it -- it paints when the host says the window needs it, which
 * on a real Mac is the update event the Window Manager delivers. There is no
 * window and no Window Manager here, so nothing was ever telling it to paint,
 * and the whole opcode went unused: defined in pefvst.h since the beginning and
 * dispatched from nowhere.
 *
 * What that looked like was an editor that drew its background at effEditOpen
 * and nothing after. A Destroy FX editor decodes its artwork into small
 * offscreens on open -- a 17x112 slider track, a 17x17 knob -- and composites
 * them when it is asked to draw. It was building every control and never being
 * asked, so the panel stayed as the background left it.
 *
 * The rect is the whole editor: partial-area drawing is an optimisation for a
 * real screen with overlapping windows, and there is neither here. */
void pefvst_editor_draw(pefvst *v)
{
    int w = 0, h = 0;

    if (!v || !v->ae || !v->editor_open) return;
    pefvst_editor_size(v, &w, &h);
    if (w <= 0 || h <= 0) return;
    /* An ERect is four int16 in top, left, bottom, right order -- the same
     * layout as the Rect effEditGetRect hands back. */
    ppc_write16(v->m, v->scratch,      0);
    ppc_write16(v->m, v->scratch + 2,  0);
    ppc_write16(v->m, v->scratch + 4,  (uint16_t)h);
    ppc_write16(v->m, v->scratch + 6,  (uint16_t)w);
    pefvst_dispatch(v, PV_EDIT_DRAW, 0, 0, v->scratch, 0.0f);
}

int pefvst_editor_mouse(pefvst *v, int x, int y, int down)
{
    int press, handled = 0;

    if (!v || !v->editor_open) return 0;

    /* Always record where the mouse is. This is the only thing a plug-in in a
     * drag loop is looking at, and it must keep changing for the drag to track
     * the pointer rather than the point it started from. */
    press = down && !v->mouse_down;
    v->mouse_down = down;
    cfm_set_mouse(v->c, x, y, down);

    /* If the guest is already running on this thread, we are being called from
     * its own drag loop by way of the input pump. Updating the mouse above is
     * exactly what it wanted; dispatching into it again is not. */
    if (v->in_guest && pthread_equal(v->guest_thread, pthread_self()))
        return 0;

    /* effEditMouse means "a click happened here", so it goes once, on the press.
     * Sending it again for every movement while the button is held would restart
     * the gesture at each new position -- which makes a dial jump instead of
     * follow, and leaves it wherever the last restart put it. */
    if (press)
        handled = pefvst_dispatch(v, PV_EDIT_MOUSE, x, y, 0, 0.0f) != 0;
    /* The editor redraws when it is given the chance, so the idle is what makes
     * the result visible. */
    pefvst_dispatch(v, PV_EDIT_IDLE, 0, 0, 0, 0.0f);
    return handled;
}

void pefvst_set_input_pump(pefvst *v, void (*fn)(void *ud), void *ud)
{ if (v) cfm_set_input_pump(v->c, fn, ud); }

int pefvst_editor_key(pefvst *v, int ch)
{
    int handled;
    if (!v || !v->editor_open) return 0;
    handled = pefvst_dispatch(v, PV_EDIT_KEY, 0, ch, 0, 0.0f) != 0;
    pefvst_dispatch(v, PV_EDIT_IDLE, 0, 0, 0, 0.0f);
    return handled;
}

const uint32_t *pefvst_editor_pixels(pefvst *v, int *w, int *h)
{
    const uint32_t *px;

    if (!v) return NULL;
    /* The idle call locks for itself; the copy that follows reads the guest's
     * memory directly and so needs the lock too, or it can capture a frame the
     * audio thread is halfway through changing. */
    if (v->editor_open) pefvst_dispatch(v, PV_EDIT_IDLE, 0, 0, 0, 0.0f);
    pthread_mutex_lock(&v->lock);
    px = cfm_gworld_pixels(v->c, w, h);
    pthread_mutex_unlock(&v->lock);
    return px;
}
