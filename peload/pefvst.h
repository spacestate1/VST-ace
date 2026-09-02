/* A Classic Mac OS VST plug-in, as a VST. See pefvst.c for the ABI details.
 *
 * pefvst_open() does the whole job: load the PEF, bind its imports, call its
 * entry point and adopt the AEffect that comes back. pefvst_attach() is the same
 * thing starting from an AEffect that already exists in guest memory, which is
 * what the tests use -- the calling machinery is the part worth exercising, and
 * it does not care where the AEffect came from.
 */
#ifndef PELOAD_PEFVST_H
#define PELOAD_PEFVST_H

#include <stdint.h>
#include "cfmlib.h"
#include "pefload.h"
#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* VST 1.0 opcodes, the ones used here. */
#define PV_OPEN            0
#define PV_CLOSE           1
#define PV_SET_PROGRAM     2
#define PV_GET_PROGRAM     3
#define PV_GET_PROGRAM_NAME 5
#define PV_GET_PARAM_LABEL 6
#define PV_GET_PARAM_DISPLAY 7
#define PV_GET_PARAM_NAME  8
#define PV_SET_SAMPLE_RATE 10
#define PV_SET_BLOCK_SIZE  11
#define PV_MAINS_CHANGED   12
#define PV_EDIT_GET_RECT   13
#define PV_EDIT_OPEN       14
#define PV_EDIT_CLOSE      15
/* The editor opcodes, in the order VST 1.0 defines them. Worth writing out
 * rather than guessing: draw, mouse and key sit *below* idle, and opcode 22 --
 * where a reasonable guess puts the mouse -- is effIdentify, which answers
 * 'NvEf' and looks enough like a handled click to mislead.
 *
 * A Mac VST 1.0 editor is not sent Mac events and does not poll for a press: the
 * host tells it with effEditMouse, x in `index` and y in `value`. */
#define PV_EDIT_DRAW       16
#define PV_EDIT_MOUSE      17
#define PV_EDIT_KEY        18
#define PV_EDIT_IDLE       19
#define PV_EDIT_TOP        20
#define PV_EDIT_SLEEP      21
#define PV_PROCESS_EVENTS  25
#define PV_GET_EFFECT_NAME 45
#define PV_GET_VENDOR      47
#define PV_GET_PRODUCT     48

#define PV_FLAG_HAS_EDITOR   (1 << 0)
#define PV_FLAG_CAN_REPLACING (1 << 4)
#define PV_FLAG_IS_SYNTH     (1 << 8)

typedef struct pefvst pefvst;

pefvst *pefvst_open(const uint8_t *pef, uint32_t peflen,
                    const uint8_t *fork, uint32_t forklen,
                    const char *support_dir, double rate, int block,
                    char *err, int errlen);

/* Adopt an AEffect that is already in the guest's memory. Takes ownership of
 * nothing: the caller keeps the machine, fragment and shim. */
pefvst *pefvst_attach(ppc *m, pef *p, cfm *c, uint32_t aeffect,
                      double rate, int block, char *err, int errlen);

void pefvst_close(pefvst *v);

int  pefvst_inputs(pefvst *v);
int  pefvst_outputs(pefvst *v);
int  pefvst_params(pefvst *v);
int  pefvst_programs(pefvst *v);
int  pefvst_flags(pefvst *v);
int  pefvst_unique_id(pefvst *v);
int  pefvst_version(pefvst *v);

/* One block. `in` and `out` are host-side arrays of channel pointers. Returns 0
 * on success, or -1 with pefvst_error() set if the guest faulted. */
int   pefvst_process(pefvst *v, const float *const *in, float *const *out,
                     int frames);

float pefvst_get_param(pefvst *v, int index);
void  pefvst_set_param(pefvst *v, int index, float value);

/* A string the plug-in supplies. Returns 1 if it wrote one. */
int   pefvst_string(pefvst *v, int opcode, int index, char *out, int n);

/* The raw dispatcher, for opcodes with no wrapper. `ptr` is a guest address. */
int32_t pefvst_dispatch(pefvst *v, int opcode, int index, int32_t value,
                        uint32_t ptr, float opt);

/* A note, delivered as a VST MIDI event. */
void  pefvst_note(pefvst *v, int on, int key, int velocity);

/* The editor. pefvst_editor_size() reports what the plug-in asks for;
 * pefvst_editor_pixels() returns what it has drawn, as 32-bit BGRA. */
int   pefvst_editor_open(pefvst *v);

/* Ask the plug-in to paint the whole editor -- effEditDraw. On Mac the host
 * owns the editor's window, so a plug-in paints when it is told to and not
 * before; without this a Classic editor draws its background at open and never
 * its controls. Sent on open and on every idle. */
void  pefvst_editor_draw(pefvst *v);

/* Report the mouse to the editor and give it a chance to react. `down` is 1
 * while a button is held. Returns 1 if the editor said it handled the click. */
int   pefvst_editor_mouse(pefvst *v, int x, int y, int down);

/* Give the plug-in a way to see fresh input from inside its own drag loop. */
void  pefvst_set_input_pump(pefvst *v, void (*fn)(void *ud), void *ud);

/* A key, as effEditKey. `ch` is the character; returns 1 if it was handled. */
int   pefvst_editor_key(pefvst *v, int ch);
int   pefvst_editor_size(pefvst *v, int *w, int *h);
const uint32_t *pefvst_editor_pixels(pefvst *v, int *w, int *h);

const char *pefvst_error(pefvst *v);

/* Internals, for probing what a plug-in actually returned. */
uint32_t pefvst_scratch(pefvst *v);
ppc     *pefvst_machine(pefvst *v);

/* How many instructions the guest has executed, which is the honest measure of
 * what this costs. */
uint64_t pefvst_icount(pefvst *v);

/* Import diagnostics: how many library symbols the shim bound, how many it had
 * to leave unbound, and how many of those the guest then called anyway. The
 * unbound names are available through `names` (comma-separated) when it is
 * non-NULL and `namesz` is big enough. */
int pefvst_import_stats(pefvst *v, int *bound, int *unbound, int *stub_calls,
                        char *names, int namesz);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_PEFVST_H */
