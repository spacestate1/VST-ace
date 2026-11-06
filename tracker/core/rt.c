/* Realtime priority for the tracker's audio thread.
 *
 * The thread that feeds the sample tracks to the sound card runs at ordinary
 * priority, with a short buffer. When the machine is busy -- the window, two
 * synth helpers, a plug-in loading -- it can be kept off the CPU for longer
 * than the buffer holds, the card runs dry, and that is a click or a pop in the
 * drums. PipeWire's own threads are not exposed to this because they ask the
 * system's RealtimeKit for realtime scheduling; this does the same.
 *
 * How that is asked for is shared with the plug-in helpers: peload/rtprio.h.
 *
 * RealtimeKit refuses a thread unless the process has an RLIMIT_RTTIME -- the
 * most CPU a realtime thread may use without blocking -- so one is set. The
 * audio thread blocks in the card's write every period, far inside it. */
#define _GNU_SOURCE
#include "trk.h"

#include "../../peload/rtprio.h"

int trk_audio_thread_realtime(int priority, char *how, size_t hown)
{
    return rtprio_request(priority, how, hown);
}

/* Give the calling thread its ordinary scheduling back. A realtime thread that
 * has gone wrong -- rendering slower than the audio it makes, for a long run --
 * would starve the rest of the machine at its priority; the audio thread calls
 * this on itself when that happens and carries on, audibly late but no longer
 * a threat to the desktop. */
void trk_audio_thread_normal(void)
{
    rtprio_normal();
}
