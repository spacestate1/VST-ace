/* Links between the studio's synths and REAPER, made in the PipeWire graph.
 *
 * With REAPER enabled the studio looks for REAPER in the graph -- a JACK client
 * called REAPER (REAPER on pipewire-jack), or ALSA sequencer ports of a client
 * called REAPER (the Midi-Bridge) -- and links each loaded synth to it:
 *
 *   audio  the synth's stereo output  ->  a pair of REAPER's audio inputs
 *   MIDI   a REAPER MIDI output       ->  the synth's MIDI input
 *
 * Synth k gets REAPER's inputs 2k+1 and 2k+2 and REAPER's k-th MIDI output, so
 * the order of the synths in the studio is the order of the tracks to set up in
 * REAPER. Links are made as REAPER and the synths appear and removed when it is
 * switched off; it is the graph that does the work, so nothing here is in the
 * audio path. Only links made here are ever removed. */
#ifndef REAPERLINK_H
#define REAPERLINK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char node[128];           /* the synth's PipeWire stream node.name, e.g. "pestudio 2" */
    char alsa_client[128];    /* its ALSA sequencer client name, for its MIDI input; may be "" */
    char midi_port[96];       /* when the client has a port for each synth: this synth's, as a
                               * piece of the port's name; "" when the client has just the one */
} rl_synth;

typedef struct rl rl;

/* Connect to PipeWire. NULL when there is no server. */
rl  *rl_open(void);
void rl_close(rl *r);                       /* removes every link it made */

/* The synths to link, in order, and whether to. Replaces what was set before. */
void rl_set(rl *r, const rl_synth *synths, int n, int enabled);

/* One line for a menu: whether REAPER was found and what is linked. */
void rl_status(rl *r, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif
