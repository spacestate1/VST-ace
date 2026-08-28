/* peload32 --serve: run a 32-bit plugin on behalf of a 64-bit host.
 *
 * Mirrors what a normal host does with two threads, because that is what VST2
 * expects: the editor and the parameter/metadata calls on the main thread, and
 * process() on a dedicated audio thread. Here the audio thread is driven by a
 * semaphore in shared memory instead of a soundcard callback -- the host's
 * realtime thread posts it and waits for the reply.
 *
 * Both threads touch plugin code, so both install their own TEB.
 */
#ifndef PELOAD_SERVE32_H
#define PELOAD_SERVE32_H

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include "bridge.h"

typedef struct {
    AEffect32  *fx;
    bridge_shm *sh;
    int         sock;
    int         nin, nout;
    float     **ins, **outs;
    int         cap;                     /* frames the buffers are sized for */
    int         editor_open;
    pthread_t   audio;
    volatile int stop;
    /* One VstEvents block, allocated once and reused every callback: the
     * pointer array must be contiguous with the header, so it cannot be a
     * separate member. See VSTEVENTS32_BYTES. */
    void          *evbuf;
    VstMidiEvent32 evm[BRIDGE_MIDIQ];
} serve_state;

/* Idle poll timeout. With an editor open the loop doubles as its 60 Hz pump, so
 * it must come back promptly; without one there is nothing to do between
 * requests. */
static int sv_poll_ms(const serve_state *s) { return s->editor_open ? 16 : 500; }

static void sv_teb_once(void)
{
    static __thread int done;
    if (!done) { done = 1; if (teb_install()) fprintf(stderr, "serve: no TEB\n"); }
}

static int sv_alloc(serve_state *s, int frames)
{
    int i, nchan;
    if (frames <= s->cap) return 0;
    for (i = 0; s->ins && i < s->nin; i++)  free(s->ins[i]);
    for (i = 0; s->outs && i < s->nout; i++) free(s->outs[i]);
    free(s->ins); free(s->outs);
    nchan = s->nin > s->nout ? s->nin : s->nout;
    s->ins  = calloc((size_t)nchan + 1, sizeof *s->ins);
    s->outs = calloc((size_t)nchan + 1, sizeof *s->outs);
    if (!s->ins || !s->outs) return -1;
    for (i = 0; i < s->nin; i++)
        if (!(s->ins[i] = calloc((size_t)frames, sizeof **s->ins))) return -1;
    for (i = 0; i < s->nout; i++)
        if (!(s->outs[i] = calloc((size_t)frames, sizeof **s->outs))) return -1;
    s->cap = frames;
    return 0;
}

/* Drain the parameter and MIDI rings. Runs on the audio thread, immediately
 * before process(), which is where VST2 wants both to land. */
static void sv_drain(serve_state *s)
{
    bridge_shm *sh = s->sh;
    uint32_t t, h;
    int n = 0;

    t = atomic_load_explicit(&sh->p_tail, memory_order_relaxed);
    h = atomic_load_explicit(&sh->p_head, memory_order_acquire);
    for (; t != h; t++) {
        bridge_param p = sh->pq[t % BRIDGE_PARAMQ];
        if (p.index >= 0 && p.index < s->fx->numParams)
            s->fx->setParameter(s->fx, p.index, p.value);
    }
    atomic_store_explicit(&sh->p_tail, t, memory_order_release);

    t = atomic_load_explicit(&sh->m_tail, memory_order_relaxed);
    h = atomic_load_explicit(&sh->m_head, memory_order_acquire);
    for (; t != h && n < BRIDGE_MIDIQ; t++, n++) {
        bridge_ev m = sh->mq[t % BRIDGE_MIDIQ];
        VstMidiEvent32 *ev = &s->evm[n];
        memset(ev, 0, sizeof *ev);
        ev->type = 1;
        ev->byteSize = sizeof *ev;
        ev->midiData[0] = (char)m.status;
        ev->midiData[1] = (char)m.d1;
        ev->midiData[2] = (char)m.d2;
        vstevents32_array(s->evbuf)[n] = ev;
    }
    atomic_store_explicit(&sh->m_tail, t, memory_order_release);
    if (n) {
        VstEvents32 *ve = s->evbuf;
        ve->numEvents = n;
        ve->reserved  = 0;
        s->fx->dispatcher(s->fx, effProcessEvents, 0, 0, ve, 0.0f);
    }
}

static void *sv_audio_thread(void *ud)
{
    serve_state *s = ud;
    bridge_shm *sh = s->sh;

    sv_teb_once();
    /* The host's realtime thread is blocked waiting on us, so match it. Not
     * fatal if the policy is refused -- it just means more jitter. */
    {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = 20;
        pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
    }

    while (!s->stop) {
        int n, i, j;
        if (bridge_sem_wait(&sh->req, NULL)) break;
        if (sh->abort || s->stop) { bridge_sem_post(&sh->done); break; }

        n = sh->frames;
        if (n < 0) n = 0;
        if (n > BRIDGE_MAX_FRAMES) n = BRIDGE_MAX_FRAMES;
        if (sv_alloc(s, n > 0 ? n : 1)) { bridge_sem_post(&sh->done); continue; }

        sv_drain(s);

        for (i = 0; i < s->nin; i++)
            for (j = 0; j < n; j++)
                s->ins[i][j] = sh->in[(size_t)j * BRIDGE_MAX_CHAN + (i % BRIDGE_MAX_CHAN)];
        for (i = 0; i < s->nout; i++) memset(s->outs[i], 0, (size_t)n * sizeof **s->outs);

        s->fx->processReplacing(s->fx, s->nin ? s->ins : NULL, s->outs, n);

        for (j = 0; j < n; j++) {
            sh->out[(size_t)j * 2]     = s->outs[0][j];
            sh->out[(size_t)j * 2 + 1] = s->nout >= 2 ? s->outs[1][j] : s->outs[0][j];
        }
        bridge_sem_post(&sh->done);
    }
    return NULL;
}

/* ------------------------------------------------------------ control loop */

static void sv_text(bridge_rep *r, const char *s)
{ snprintf(r->text, sizeof r->text, "%s", s ? s : ""); }

static void sv_publish_editor(serve_state *s)
{
    const uint32_t *px;
    int w, h;
    bridge_shm *sh = s->sh;

    if (!s->editor_open) return;
    ed_pump(s->fx);
    if (!w32_editor_pixels(&px, &w, &h) || !px || w <= 0 || h <= 0) return;
    if (w > BRIDGE_MAX_ED_W || h > BRIDGE_MAX_ED_H) return;
    sh->ed_w = w; sh->ed_h = h;
    memcpy(bridge_pixels(sh), px, (size_t)w * h * 4);
    atomic_fetch_add_explicit(&sh->ed_gen, 1, memory_order_release);
}

static int serve_run(AEffect32 *fx, int sock, const char *shm_path)
{
    static serve_state S;
    bridge_shm *sh;
    int fd;

    memset(&S, 0, sizeof S);
    S.fx = fx;
    S.sock = sock;

    if ((fd = shm_open(shm_path, O_RDWR, 0600)) < 0) { perror("shm_open"); return 1; }
    sh = mmap(NULL, BRIDGE_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sh == MAP_FAILED) { perror("mmap shm"); return 1; }
    if (sh->magic != BRIDGE_MAGIC || sh->version != BRIDGE_VERSION) {
        fprintf(stderr, "serve: shared region is not v%d\n", BRIDGE_VERSION);
        return 1;
    }
    S.sh = sh;
    S.nin  = fx->numInputs;
    S.nout = fx->numOutputs < 1 ? 1 : fx->numOutputs;
    if (S.nin < 0 || S.nin > 256 || S.nout > 256) {
        fprintf(stderr, "serve: implausible channel count\n");
        return 1;
    }
    sh->nin = S.nin; sh->nout = S.nout;

    if (!(S.evbuf = calloc(1, VSTEVENTS32_BYTES(BRIDGE_MIDIQ)))) {
        fprintf(stderr, "serve: out of memory\n");
        return 1;
    }

    if (pthread_create(&S.audio, NULL, sv_audio_thread, &S) != 0) {
        fprintf(stderr, "serve: cannot start audio thread\n");
        return 1;
    }

    for (;;) {
        bridge_req q;
        bridge_rep r;
        struct pollfd pfd;
        ssize_t n;

        /* Poll rather than block, so the editor keeps animating between
         * requests -- a VST2 editor only redraws when it is pumped. */
        pfd.fd = sock; pfd.events = POLLIN; pfd.revents = 0;
        if (poll(&pfd, 1, sv_poll_ms(&S)) < 0 && errno != EINTR) break;
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) { sv_publish_editor(&S); continue; }

        n = recv(sock, &q, sizeof q, MSG_WAITALL);
        if (n != (ssize_t)sizeof q) break;               /* host went away */

        memset(&r, 0, sizeof r);
        r.ok = 1;
        switch (q.op) {
        case BR_HELLO: {
            char nm[64] = { 0 }, vn[64] = { 0 };
            fx->dispatcher(fx, effGetEffectName, 0, 0, nm, 0.0f);
            fx->dispatcher(fx, effGetVendorString, 0, 0, vn, 0.0f);
            sv_text(&r, nm);
            snprintf(r.text2, sizeof r.text2, "%s", vn);
            r.a = fx->numPrograms; r.b = fx->numParams;
            r.c = fx->numInputs;   r.d = fx->numOutputs;
            r.e = fx->flags;       r.g = fx->uniqueID;
            break;
        }
        case BR_PARAM_NAME: {
            char b[64] = { 0 };
            fx->dispatcher(fx, effGetParamName, q.a, 0, b, 0.0f);
            sv_text(&r, b); break;
        }
        case BR_PARAM_LABEL: {
            char b[64] = { 0 };
            fx->dispatcher(fx, effGetParamLabel, q.a, 0, b, 0.0f);
            sv_text(&r, b); break;
        }
        case BR_PARAM_DISPLAY: {
            char b[64] = { 0 };
            fx->dispatcher(fx, effGetParamDisplay, q.a, 0, b, 0.0f);
            sv_text(&r, b); break;
        }
        case BR_PARAM_GET:
            r.f = (q.a >= 0 && q.a < fx->numParams) ? fx->getParameter(fx, q.a) : 0.0f;
            break;
        case BR_PROGRAM_NAME: {
            char b[64] = { 0 };
            if (!fx->dispatcher(fx, effGetProgramNameIndexed, q.a, -1, b, 0.0f) && !b[0])
                snprintf(b, sizeof b, "Program %d", q.a + 1);
            sv_text(&r, b); break;
        }
        case BR_SET_PROGRAM:
            fx->dispatcher(fx, effSetProgram, 0, q.a, NULL, 0.0f);
            break;
        case BR_GET_PROGRAM:
            r.a = (int32_t)fx->dispatcher(fx, effGetProgram, 0, 0, NULL, 0.0f);
            break;
        case BR_EDITOR_KIND:
            r.a = (fx->flags & EFF_HAS_EDITOR) ? 2 /* PIXELS */ : 0;
            break;
        case BR_EDITOR_SIZE: {
            int w = 0, h = 0;
            ed_size(fx, &w, &h);
            r.a = w; r.b = h; break;
        }
        case BR_EDITOR_OPEN: {
            int w = 0, h = 0;
            if (S.editor_open) { r.a = sh->ed_w; r.b = sh->ed_h; break; }
            if (ed_open(fx, &w, &h)) { r.ok = 0; break; }
            if (w > BRIDGE_MAX_ED_W || h > BRIDGE_MAX_ED_H) {
                fprintf(stderr, "serve: editor %dx%d exceeds the shared buffer\n", w, h);
                fx->dispatcher(fx, effEditClose, 0, 0, NULL, 0.0f);
                w32_reset();
                r.ok = 0; break;
            }
            S.editor_open = 1;
            sh->ed_w = w; sh->ed_h = h;
            r.a = w; r.b = h;
            sv_publish_editor(&S);
            break;
        }
        case BR_EDITOR_CLOSE:
            if (S.editor_open) {
                fx->dispatcher(fx, effEditClose, 0, 0, NULL, 0.0f);
                w32_reset();
                S.editor_open = 0;
            }
            break;
        case BR_EDITOR_MOUSE:
            w32_mouse(q.a, q.b, q.c, q.d, q.e);
            break;
        case BR_EDITOR_KEY:
            w32_key(q.a, q.b, q.c);
            break;
        case BR_ALL_NOTES_OFF: {
            int ch;
            for (ch = 0; ch < 16; ch++) {
                bridge_ev m;
                uint32_t hd = atomic_load_explicit(&sh->m_head, memory_order_relaxed);
                m.status = (uint8_t)(0xB0 | ch); m.d1 = 123; m.d2 = 0; m.pad = 0;
                sh->mq[hd % BRIDGE_MIDIQ] = m;
                atomic_store_explicit(&sh->m_head, hd + 1, memory_order_release);
            }
            break;
        }
        case BR_IMPORT_STATS: {
            int i, hit = 0;
            for (i = 0; i < g_nimp; i++) if (g_imp[i].calls) hit++;
            r.a = g_nresolved; r.b = g_nimp; r.c = hit;
            break;
        }
        case BR_QUIT:
            r.ok = 1;
            send(sock, &r, sizeof r, MSG_NOSIGNAL);
            goto done;
        default:
            r.ok = 0;
            break;
        }
        if (send(sock, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r) break;
        sv_publish_editor(&S);
    }

done:
    S.stop = 1;
    sh->abort = 1;
    bridge_sem_post(&sh->req);
    pthread_join(S.audio, NULL);
    fx->dispatcher(fx, effMainsChanged, 0, 0, NULL, 0.0f);
    fx->dispatcher(fx, effClose, 0, 0, NULL, 0.0f);
    return 0;
}

#endif /* PELOAD_SERVE32_H */
