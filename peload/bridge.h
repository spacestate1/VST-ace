/* Protocol between pehost (64-bit) and peload32 --serve (32-bit).
 *
 * A process cannot execute both widths, so a 32-bit plugin has to live in
 * another process. This is the contract between the two, and it is shared
 * source: both sides compile this same header, one at -m64 and one at -m32.
 * Everything crossing the boundary therefore uses fixed-width types and
 * explicit padding -- no pointers, no long, no enums in structs.
 *
 * Two channels, split by who is waiting on them:
 *
 *   control    a UNIX socket carrying request/response for everything the GUI
 *              thread asks: names, counts, program changes, editor open. Each
 *              request gets exactly one reply, so the socket stays in step.
 *
 *   shared     an mmap'd region carrying the things that must not go through a
 *              socket: the audio block (the realtime thread cannot afford a
 *              round trip through the kernel's socket buffers), the editor's
 *              framebuffer (megabytes per frame), and two lock-free rings for
 *              parameter and MIDI writes that the audio side drains.
 *
 * The audio handshake is a pair of POSIX semaphores in the shared region. The
 * host fills `in`, posts `req`, and waits on `done` with a timeout; the helper's
 * worker waits on `req`, calls the plugin, and posts `done`. If the helper dies
 * or stalls, the host's wait times out and it emits silence rather than blocking
 * the whole PipeWire graph.
 */
#ifndef PELOAD_BRIDGE_H
#define PELOAD_BRIDGE_H

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define BRIDGE_MAGIC    0x50423332u        /* 'PB32' */
/* 2: MIDI events carry the frame they belong at, so a sequencer's timing
 * survives the crossing. Both sides are built together, so the check exists to
 * catch a stale helper binary rather than to support old ones. */
#define BRIDGE_VERSION  2

/* Generous enough for any period a host will ask for; the helper clamps. */
#define BRIDGE_MAX_FRAMES  8192
#define BRIDGE_MAX_CHAN    2               /* the host's engine is stereo */

/* An editor bigger than this is refused rather than silently cropped. The
 * largest here is 1480x660; this leaves room for a HiDPI plugin. */
#define BRIDGE_MAX_ED_W    2560
#define BRIDGE_MAX_ED_H    1600

#define BRIDGE_PARAMQ  1024
#define BRIDGE_MIDIQ   1024
#define BRIDGE_INQ     512

/* Nothing in the shared struct may be width-dependent, which rules out sem_t:
 * glibc's is 16 bytes on i386 and 32 on x86-64, so the two processes would not
 * even agree where the semaphore ends. A counting semaphore over one 32-bit word
 * and the futex syscall has the same layout everywhere.
 *
 * Only the host ever waits with a timeout, and the host is x86-64, so the
 * `struct timespec` handed to the kernel is unambiguous. The helper always waits
 * indefinitely and is woken by a post, so it never needs one -- which sidesteps
 * i386's 32-vs-64-bit time_t question entirely. */
typedef _Atomic uint32_t bridge_sem;

static inline void bridge_sem_post(bridge_sem *s)
{
    atomic_fetch_add_explicit(s, 1, memory_order_release);
    /* No FUTEX_PRIVATE_FLAG: the waiter is in another process. */
    syscall(SYS_futex, (void *)s, FUTEX_WAKE, 1, NULL, NULL, 0);
}

/* `rel` is a relative timeout, or NULL to wait indefinitely.
 * Returns 0 on success, -1 with errno == ETIMEDOUT on expiry. */
static inline int bridge_sem_wait(bridge_sem *s, const struct timespec *rel)
{
    for (;;) {
        uint32_t v = atomic_load_explicit(s, memory_order_acquire);
        while (v > 0) {
            if (atomic_compare_exchange_weak_explicit(s, &v, v - 1,
                    memory_order_acquire, memory_order_relaxed))
                return 0;
        }
        if (syscall(SYS_futex, (void *)s, FUTEX_WAIT, 0, rel, NULL, 0) < 0) {
            if (errno == ETIMEDOUT) return -1;
            if (errno != EAGAIN && errno != EINTR) return -1;
        }
    }
}

typedef struct { int32_t index; float value; } bridge_param;
/* `at` is the offset within the next block, or -1 for "as soon as it is drained".
 * Without it every event arriving between two callbacks collapsed onto the same
 * sample, which is what a sequencer hears as quantisation to the block size. */
typedef struct { uint8_t status, d1, d2, pad; int32_t at; } bridge_ev;
/* Editor input, of either kind. Mouse: a=x b=y c=msg d=buttons e=wheel.
 * Key: a=vk b=down c=ch. One ring for both, because both have to bypass the
 * request socket -- a plugin spinning in its own loop cannot answer either. */
enum { BRIDGE_IN_MOUSE = 0, BRIDGE_IN_KEY = 1 };
typedef struct { int32_t kind, a, b, c, d, e; } bridge_input;

typedef struct {
    uint32_t magic, version;
    uint32_t abort;                        /* helper should exit */

    /* ---- audio, host -> helper -> host ---- */
    bridge_sem req, done;
    int32_t  frames, nin, nout;
    uint32_t xruns;                        /* helper missed a deadline */
    float    in [BRIDGE_MAX_CHAN * BRIDGE_MAX_FRAMES];
    float    out[BRIDGE_MAX_CHAN * BRIDGE_MAX_FRAMES];

    /* ---- parameter writes, GUI thread -> audio ---- */
    _Atomic uint32_t p_head, p_tail;
    bridge_param     pq[BRIDGE_PARAMQ];

    /* ---- MIDI, GUI thread -> audio ---- */
    _Atomic uint32_t m_head, m_tail;
    bridge_ev      mq[BRIDGE_MIDIQ];

    /* ---- editor, helper -> host ----
     * The helper pumps the Win32 layer on its own thread and republishes the
     * pixels here, bumping `ed_gen` when they change. The host blits whatever
     * is current; it never asks for a frame. */
    int32_t          ed_w, ed_h;
    _Atomic uint32_t ed_gen;

    /* Editor input, delivered through memory rather than the request socket.
     *
     * A plugin's drag loop polls for the button release while the helper's main
     * thread is stuck inside the wndproc call that started the loop -- so an
     * op-and-reply can never carry the release. TAL's editor hangs on exactly
     * that. Written by the host with no round trip, drained by the helper both
     * from its own loop and from inside a spinning plugin's PeekMessage. */
    bridge_input     inq[BRIDGE_INQ];
    _Atomic uint32_t in_head, in_tail;
    uint32_t         ed_px[(size_t)BRIDGE_MAX_ED_W * 16];   /* header only */
} bridge_shm;

/* The framebuffer is far too big to sit in the struct, so the mapping is
 * bridge_shm followed by BRIDGE_MAX_ED_W*BRIDGE_MAX_ED_H pixels. ed_px above is
 * just where it starts. */
#define BRIDGE_SHM_SIZE \
    (sizeof(bridge_shm) + (size_t)BRIDGE_MAX_ED_W * BRIDGE_MAX_ED_H * 4)

static inline uint32_t *bridge_pixels(bridge_shm *s) { return s->ed_px; }

/* ------------------------------------------------------------- control ops */

enum {
    BR_HELLO = 1,        /* -> plugin path        <- info                    */
    BR_PARAM_NAME,       /* -> index              <- text                    */
    BR_PARAM_LABEL,
    BR_PARAM_DISPLAY,
    BR_PARAM_GET,        /* -> index              <- f32                     */
    BR_PROGRAM_NAME,     /* -> index              <- text                    */
    BR_SET_PROGRAM,      /* -> index              <- ok                      */
    BR_GET_PROGRAM,      /*                       <- index                   */
    BR_EDITOR_KIND,      /*                       <- kind                    */
    BR_EDITOR_OPEN,      /*                       <- ok, w, h                */
    BR_EDITOR_CLOSE,
    BR_EDITOR_SIZE,      /*                       <- w, h                    */
    BR_EDITOR_MOUSE,     /* -> x,y,msg,buttons,wheel                         */
    BR_EDITOR_KEY,       /* -> vk,down,ch                                    */
    BR_ALL_NOTES_OFF,
    BR_IMPORT_STATS,     /*                       <- implemented,stubbed,hit */
    BR_INPUT_MASK,       /* -> mask                                          */
    BR_QUIT
};

/* One fixed-size message each way keeps framing trivial: a short read means the
 * peer is gone, and there is never a partial record to resynchronise from. */
typedef struct {
    int32_t op;
    int32_t a, b, c, d, e;
    float   f;
} bridge_req;

typedef struct {
    int32_t ok;
    int32_t a, b, c, d, e, g;
    float   f;
    char    text[128];       /* param/program name, or the effect name  */
    char    text2[128];      /* vendor, on BR_HELLO                     */
} bridge_rep;

#endif /* PELOAD_BRIDGE_H */
