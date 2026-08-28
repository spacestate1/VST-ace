/* The Classic Mac OS libraries a PEF plug-in imports: InterfaceLib, MathLib and
 * DragLib. See cfmlib.c for what is real and what is only shaped correctly. */
#ifndef PELOAD_CFMLIB_H
#define PELOAD_CFMLIB_H

#include <stddef.h>
#include <stdint.h>
#include "pefload.h"
#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfm cfm;

/* Bind a loaded fragment's imports to implementations. `support_dir` is the host
 * directory the guest sees as its Application Support folder -- the guest cannot
 * name anything outside it. Returns NULL and leaves pef_last_error() set on
 * failure. */
cfm  *cfm_new(ppc *m, pef *p, const char *support_dir);
void  cfm_free(cfm *c);

/* Give the guest its own resource fork, which is where a Classic plug-in keeps
 * its artwork. `fork` may be a bare resource fork or an AppleSingle/AppleDouble
 * wrapper around one; it must stay alive for the cfm's lifetime. Returns the
 * number of resources found, or -1 if it is not a resource fork. */
int   cfm_set_resource_fork(cfm *c, const uint8_t *fork, uint32_t len);

/* Find one resource in a fork without needing a cfm. This is how the PEF itself
 * is recovered: a Classic VST keeps its code in an 'aEff' resource. Returns a
 * pointer into `fork`, or NULL. */
const uint8_t *cfm_fork_find(const uint8_t *fork, uint32_t len,
                             uint32_t type, int id, uint32_t *size);

/* Install this as the CPU's hostcall. `m->host` must be the cfm. */
void  cfm_hostcall(ppc *m, uint32_t index);

/* Answer the host callback in `slot` as audioMaster. Without this a plug-in that
 * calls back gets an error, which for audioMasterVersion means it decides the
 * host is too old and gives up. */
void  cfm_set_audiomaster(cfm *c, uint32_t slot, double rate, int block);

/* Guest memory, from the same heap the plug-in allocates out of. Returns a guest
 * address, or 0 if there is no room. */
uint32_t cfm_guest_alloc(cfm *c, uint32_t size, int clear);

/* Which imports had no implementation, as a comma-separated list. Returns how
 * many there were, which is 0 when everything bound. */
int   cfm_unbound(cfm *c, char *buf, size_t n);

/* How many calls landed on an unimplemented import, and the last one's name. */
int   cfm_stub_calls(cfm *c, const char **last);

/* Tell the shim how big the editor is. Without this the pixels reported back are
 * whichever offscreen happens to be largest, which for a plug-in that keeps its
 * knob artwork in a tall filmstrip is the filmstrip rather than the window. */
void  cfm_set_editor_size(cfm *c, int w, int h);

/* Where the mouse is and whether it is down, in the editor's own coordinates.
 * A Classic VST editor has no event queue: it polls GetMouse and Button from
 * inside effEditIdle, so this is how a click reaches it. */
void  cfm_set_mouse(cfm *c, int x, int y, int down);

/* A Classic editor tracks a drag by spinning on StillDown and GetMouse rather
 * than by being sent events. While it spins, nothing else in this process runs --
 * so unless the host is given a chance to refresh the mouse from inside that
 * loop, the loop never ends and the value it computes comes from a position
 * frozen at the instant of the click. This installs that chance. */
void  cfm_set_input_pump(cfm *c, void (*fn)(void *ud), void *ud);

/* An offscreen the size of the editor, as something that can be passed to a
 * plug-in as the window to draw into. A Classic VST editor is given a WindowPtr
 * and refuses to open without one; since a WindowPtr begins with a colour
 * GrafPort, and so does a GWorld, one serves as the other. Returns a guest
 * address, or 0. */
uint32_t cfm_editor_window(cfm *c, int w, int h);

/* The offscreen pixels the guest has drawn into, as 32-bit BGRA, or NULL if it
 * has not made a GWorld yet. */
const uint32_t *cfm_gworld_pixels(cfm *c, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_CFMLIB_H */
