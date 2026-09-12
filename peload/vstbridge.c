/* vst-ace as a Linux VST2 plug-in: the hosts, on a track.
 *
 * Everything else in this tree is a host you run. This is the same host with
 * the sides swapped -- a .so a Linux DAW loads like any other plug-in, which
 * then loads a Windows VST2 or VST3, a macOS bundle or a Classic fragment
 * behind it and forwards audio, MIDI, parameters, state and the editor. The
 * point is that Ardour, Reaper, Bitwig, Qtractor and Carla already know how to
 * load a Linux VST2, and none of them will ever know how to load a .dll.
 *
 * Which plug-in a given copy of this stands in for is decided by its own file
 * name, the way yabridge does it: install it as the name of the plug-in and
 * put the real thing beside it.
 *
 *     ~/.vst/FB-3300.so       -> a copy of, or symlink to, this library
 *     ~/.vst/FB-3300.dll      -> the Windows plug-in it fronts
 *
 * A `.vstace` file holding a path works too, for a plug-in that has to live
 * somewhere else, and VSTACE_TARGET overrides both -- which is what the tests
 * use, and the quickest way to try one without installing anything.
 *
 * Threading follows pehost's own rule, which is also the DAW's: processReplacing
 * on the audio thread and nothing else, every other entry point on the main
 * thread.
 */

#define _GNU_SOURCE
#define VST2_SYSV              /* this side is a Linux plug-in: System V ABI */

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vst2.h"
#include "pehost.h"

/* The editor rectangle, as the DAW expects it back from effEditGetRect. */
typedef struct { int16_t top, left, bottom, right; } ERect;

/* Opcodes the shared header does not name, because the hosts in this tree
 * never had to answer them. Here they are asked of us. */
#define effGetVstVersion 58

/* audioMaster opcodes this side asks the DAW about. */
#define amGetTime      7
#define amSizeWindow  15
#define amGetSampleRate 16
#define amCurrentId     2
#define amIdle          3

/* VstTimeInfo.flags bits worth asking for: tempo, position, meter, and
 * whether the transport is rolling. A synced delay or an arpeggiator in the
 * hosted plug-in reads all four, and getting them from the DAW rather than
 * assuming 120 bpm is the difference between in time and not. */
#define amWantTime ((1 << 9) | (1 << 10) | (1 << 13))

#define MAXCH 2                     /* pehost renders interleaved stereo */

typedef struct {
    AEffect         eff;            /* first: the DAW is handed &inst->eff */
    audioMasterCb   master;
    pehost         *h;
    double          rate;
    int             block;
    char            name[64];
    char            vendor[64];
    char            target[PATH_MAX];
    float          *in, *out;       /* interleaved scratch */
    int             cap;
    int             nin, nout;
    int             editor;
    ERect           rect;
    int             failed;         /* opened, but with nothing behind it */
} inst;

/* ------------------------------------------------------------- the target */

/* Where this copy of the library was loaded from. A symlink is reported as
 * the symlink, which is the whole trick: the name the DAW used is the name
 * the plug-in is installed under. */
static int self_path(char *out, size_t n)
{
    Dl_info info;
    if (!dladdr((void *)(uintptr_t)&self_path, &info) || !info.dli_fname)
        return 0;
    snprintf(out, n, "%s", info.dli_fname);
    return out[0] != 0;
}

static int readable(const char *p)
{
    return p && p[0] && access(p, R_OK) == 0;
}

/* Read a path out of a .vstace file: the first non-blank, non-# line. */
static int read_pointer_file(const char *path, char *out, size_t n)
{
    char line[PATH_MAX];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len || line[0] == '#') continue;
        snprintf(out, n, "%s", line);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/* The plug-in this copy fronts, by the rules in the file header. */
static int find_target(char *out, size_t n)
{
    static const char *const ext[] = { ".dll", ".vst3", ".vst", ".component",
                                       ".so", NULL };
    char self[PATH_MAX], stem[PATH_MAX], probe[PATH_MAX];
    const char *env = getenv("VSTACE_TARGET");
    char *dot;
    int i;

    if (readable(env)) { snprintf(out, n, "%s", env); return 1; }

    if (!self_path(self, sizeof self)) return 0;
    snprintf(stem, sizeof stem, "%s", self);
    if ((dot = strrchr(stem, '.')) && !strcmp(dot, ".so")) *dot = 0;

    snprintf(probe, sizeof probe, "%s.vstace", stem);
    if (readable(probe) && read_pointer_file(probe, out, n) && readable(out))
        return 1;

    for (i = 0; ext[i]; i++) {
        snprintf(probe, sizeof probe, "%s%s", stem, ext[i]);
        /* Not ourselves: a bare .so beside us is this library, not a target. */
        if (!strcmp(probe, self)) continue;
        if (readable(probe)) { snprintf(out, n, "%s", probe); return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------------ audio */

static int grow(inst *p, int frames)
{
    float *a, *b;
    if (frames <= p->cap) return 1;
    a = realloc(p->in,  (size_t)frames * MAXCH * sizeof *a);
    if (!a) return 0;
    p->in = a;
    b = realloc(p->out, (size_t)frames * MAXCH * sizeof *b);
    if (!b) return 0;
    p->out = b;
    p->cap = frames;
    return 1;
}

/* The DAW's transport, handed to the hosted plug-in.
 *
 * Asked once per block rather than cached: a user can move the playhead or
 * change the tempo between any two blocks, and a plug-in that syncs to either
 * has no other way to hear about it. A host that answers NULL -- offline
 * bounce in some DAWs -- simply leaves the last values in place. */
static void pass_transport(inst *p)
{
    VstTimeInfo *t;
    if (!p->master) return;
    t = (VstTimeInfo *)(uintptr_t)p->master(&p->eff, amGetTime, 0, amWantTime,
                                            NULL, 0.0f);
    if (!t) return;
    if (t->flags & (1 << 10))
        pehost_set_tempo(p->h, t->tempo > 0.0 ? t->tempo : 120.0,
                         (t->flags & (1 << 13)) ? t->timeSigNumerator : 4,
                         (t->flags & (1 << 13)) ? t->timeSigDenominator : 4);
    if (t->flags & (1 << 9)) pehost_locate(p->h, t->ppqPos);
    pehost_set_playing(p->h, (t->flags & (1 << 1)) != 0, 0);
}

static void process_replacing(AEffect *e, float **in, float **out, int32_t frames)
{
    inst *p = e->object;
    int i, c;

    if (!p || !p->h || frames <= 0) {
        /* Nothing behind us: silence, not whatever was in the buffer. A DAW
         * that got the host's uninitialised scratch would play it. */
        if (out) for (c = 0; c < (int)e->numOutputs; c++)
            if (out[c]) memset(out[c], 0, (size_t)frames * sizeof **out);
        return;
    }
    if (!grow(p, frames)) return;

    for (i = 0; i < frames; i++)
        for (c = 0; c < MAXCH; c++)
            p->in[i * MAXCH + c] = (in && c < p->nin && in[c]) ? in[c][i] : 0.0f;

    pass_transport(p);
    pehost_render_io(p->h, p->in, p->out, frames);

    for (c = 0; c < (int)e->numOutputs && c < MAXCH; c++)
        if (out && out[c])
            for (i = 0; i < frames; i++) out[c][i] = p->out[i * MAXCH + c];
    /* More outputs than we render: silence them rather than leave them stale. */
    for (c = MAXCH; c < (int)e->numOutputs; c++)
        if (out && out[c]) memset(out[c], 0, (size_t)frames * sizeof **out);
}

static void set_parameter(AEffect *e, int32_t i, float v)
{
    inst *p = e->object;
    if (p && p->h) pehost_set_param(p->h, i, v);
}

static float get_parameter(AEffect *e, int32_t i)
{
    inst *p = e->object;
    return (p && p->h) ? pehost_get_param(p->h, i) : 0.0f;
}

/* ------------------------------------------------------------------- MIDI */

static void feed_events(inst *p, VstEvents *ev)
{
    void **list;
    int i;
    if (!ev || !p->h || ev->numEvents <= 0) return;
    /* `events` is declared with two slots and is really numEvents long. */
    list = (void **)&ev->events[0];
    for (i = 0; i < ev->numEvents; i++) {
        VstMidiEvent *m = list[i];
        unsigned char st, a, b;
        if (!m || m->type != 1) continue;          /* kVstMidiType */
        st = (unsigned char)m->midiData[0];
        a  = (unsigned char)m->midiData[1];
        b  = (unsigned char)m->midiData[2];
        /* deltaFrames places it inside the block: without it a chord and an
         * arpeggio arrive identically, every note on sample zero. */
        pehost_midi_at(p->h, st, a, b, m->deltaFrames);
    }
}

/* -------------------------------------------------------------- dispatcher */

static void copy_str(void *ptr, const char *s, size_t cap)
{
    if (!ptr) return;
    snprintf((char *)ptr, cap, "%s", s ? s : "");
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val,
                           void *ptr, float opt)
{
    inst *p = e->object;
    char buf[256];

    if (!p) return 0;
    switch (op) {
    case effOpen:  return 0;
    case effClose:
        if (p->h) { if (p->editor) pehost_editor_detach(p->h); pehost_close(p->h); }
        free(p->in); free(p->out); free(p);
        return 0;

    case effSetSampleRate:
        /* pehost fixes the rate when the plug-in opens, so a DAW that changes
         * it afterwards needs the plug-in reopened. Recorded either way, so
         * the next open uses it. */
        p->rate = opt > 0.0f ? (double)opt : p->rate;
        return 0;
    case effSetBlockSize:
        p->block = (int)val > 0 ? (int)val : p->block;
        return 0;
    case effMainsChanged:
        if (!val) pehost_all_notes_off(p->h);
        return 0;

    case effGetEffectName:   copy_str(ptr, p->name,   32); return 1;
    case effGetProductString:copy_str(ptr, p->name,   64); return 1;
    case effGetVendorString: copy_str(ptr, p->vendor, 64); return 1;
    case effGetVendorVersion: return 1;
    case effGetVstVersion:   return 2400;
    case effGetPlugCategory: return (p->h && pehost_is_synth(p->h)) ? 2 : 1;

    case effGetProgram:      return p->h ? pehost_get_program(p->h) : 0;
    case effSetProgram:
        if (p->h) pehost_set_program(p->h, (int)val);
        return 0;
    case effGetProgramName:
        if (p->h) { pehost_program_name(p->h, pehost_get_program(p->h), buf, sizeof buf);
                    copy_str(ptr, buf, 24); }
        return 0;
    case effGetProgramNameIndexed:
        if (!p->h || idx < 0 || idx >= pehost_num_programs(p->h)) return 0;
        pehost_program_name(p->h, idx, buf, sizeof buf);
        copy_str(ptr, buf, 24);
        return 1;

    case effGetParamName:
        if (p->h) { pehost_param_name(p->h, idx, buf, sizeof buf); copy_str(ptr, buf, 16); }
        return 0;
    case effGetParamLabel:
        if (p->h) { pehost_param_label(p->h, idx, buf, sizeof buf); copy_str(ptr, buf, 8); }
        return 0;
    case effGetParamDisplay:
        if (p->h) { pehost_param_display(p->h, idx, buf, sizeof buf); copy_str(ptr, buf, 8); }
        return 0;
    case effCanBeAutomated:  return 1;

    case effProcessEvents:   feed_events(p, ptr); return 1;

    /* State. The DAW saves this with the session, and for a plug-in whose
     * sound is not fully described by its parameters it is the only thing
     * that brings the session back sounding the same. */
    case effGetChunk: {
        const void *data = NULL;
        int n = p->h ? pehost_get_state(p->h, idx ? 1 : 0, &data) : 0;
        if (n > 0 && data && ptr) { *(const void **)ptr = data; return n; }
        return 0;
    }
    case effSetChunk:
        return (p->h && ptr && val > 0 &&
                pehost_set_state(p->h, idx ? 1 : 0, ptr, (int)val)) ? 1 : 0;

    /* Editor. The DAW hands an X11 window id in `ptr` and the hosted plug-in's
     * own editor is reparented into it -- the same path pestudio uses. */
    case effEditGetRect: {
        int w = 0, hgt = 0;
        if (!p->h || !pehost_has_editor(p->h)) return 0;
        pehost_editor_size(p->h, &w, &hgt);
        if (w <= 0 || hgt <= 0) { w = 400; hgt = 300; }
        p->rect.top = 0; p->rect.left = 0;
        p->rect.bottom = (int16_t)hgt; p->rect.right = (int16_t)w;
        if (ptr) *(ERect **)ptr = &p->rect;
        return 1;
    }
    case effEditOpen:
        if (!p->h || !pehost_has_editor(p->h)) return 0;
        if (!pehost_editor_attach(p->h, (unsigned long)(uintptr_t)ptr)) return 0;
        p->editor = 1;
        return 1;
    case effEditClose:
        if (p->h && p->editor) { pehost_editor_detach(p->h); p->editor = 0; }
        return 0;
    case effEditIdle:
        if (p->h && p->editor) pehost_editor_pump(p->h);
        return 0;

    case effCanDo:
        if (!ptr) return 0;
        if (!strcmp((const char *)ptr, "receiveVstEvents") ||
            !strcmp((const char *)ptr, "receiveVstMidiEvent")) return 1;
        if (!strcmp((const char *)ptr, "sendVstEvents") ||
            !strcmp((const char *)ptr, "sendVstMidiEvent")) return 0;
        return 0;
    case effGetTailSize: return 0;
    default:
        (void)idx; (void)val; (void)opt;
        return 0;
    }
}

/* ------------------------------------------------------------------- entry */

/* A stand-in AEffect for when there is nothing behind us.
 *
 * Returning NULL from VSTPluginMain makes a DAW say "failed to load" and stop,
 * with no room to say why. An effect that loads, names the problem in its own
 * name and passes silence keeps the track, the session and the user's place in
 * it -- and the reason is in the log rather than guessed at. */
static void silent_process(AEffect *e, float **in, float **out, int32_t frames)
{
    int c;
    (void)in;
    if (!out) return;
    for (c = 0; c < (int)e->numOutputs; c++)
        if (out[c]) memset(out[c], 0, (size_t)frames * sizeof **out);
}

static AEffect *make_effect(inst *p)
{
    AEffect *e = &p->eff;
    memset(e, 0, sizeof *e);
    e->magic       = 0x56737450;                 /* 'VstP' */
    e->dispatcher  = dispatcher;
    e->setParameter = set_parameter;
    e->getParameter = get_parameter;
    e->processReplacing = p->failed ? silent_process : process_replacing;
    e->numInputs   = p->nin;
    e->numOutputs  = p->nout;
    e->numParams   = p->h ? pehost_num_params(p->h) : 0;
    e->numPrograms = p->h ? pehost_num_programs(p->h) : 0;
    e->uniqueID    = p->h ? pehost_unique_id(p->h) : 0x56414365; /* 'VACe' */
    e->version     = 1;
    e->object      = p;
    e->ioRatio     = 1.0f;
    e->flags       = effFlagsCanReplacing;
    if (p->h && pehost_is_synth(p->h))  e->flags |= effFlagsIsSynth;
    if (p->h && pehost_has_editor(p->h)) e->flags |= effFlagsHasEditor;
    /* Only claim chunk state when the hosted plug-in actually has some:
     * a DAW that is told there is a chunk and handed nothing saves nothing
     * and restores nothing, silently. */
    if (p->h && pehost_has_state(p->h)) e->flags |= effFlagsProgramChunks;
    return e;
}

AEffect *VSTPluginMain(audioMasterCb master);

AEffect *VSTPluginMain(audioMasterCb master)
{
    inst *p = calloc(1, sizeof *p);
    double rate = 48000.0;
    int block = 512;

    if (!p) return NULL;
    p->master = master;

    /* The DAW's rate, if it will say before the plug-in is opened. Both are
     * confirmed again through effSetSampleRate/effSetBlockSize, but pehost
     * fixes them at open, so asking first is what gets them right. */
    if (master) {
        float r = (float)master(&p->eff, amGetSampleRate, 0, 0, NULL, 0.0f);
        if (r > 0.0f) rate = (double)r;
    }
    p->rate = rate;
    p->block = block;

    if (!find_target(p->target, sizeof p->target)) {
        fprintf(stderr, "vst-ace: this copy does not say which plug-in it "
                        "stands for. Install it as <name>.so beside <name>.dll, "
                        "or put the path in <name>.vstace, or set "
                        "VSTACE_TARGET.\n");
        p->failed = 1;
        snprintf(p->name, sizeof p->name, "vst-ace (no target)");
        snprintf(p->vendor, sizeof p->vendor, "vst-ace");
        p->nin = 2; p->nout = 2;
        return make_effect(p);
    }

    pehost_thread_init();
    p->h = pehost_open(p->target, rate, block);
    if (!p->h) {
        fprintf(stderr, "vst-ace: %s did not load: %s\n",
                p->target, pehost_last_error());
        p->failed = 1;
        snprintf(p->name, sizeof p->name, "vst-ace (load failed)");
        snprintf(p->vendor, sizeof p->vendor, "vst-ace");
        p->nin = 2; p->nout = 2;
        return make_effect(p);
    }

    snprintf(p->name, sizeof p->name, "%s", pehost_name(p->h));
    snprintf(p->vendor, sizeof p->vendor, "%s", pehost_vendor(p->h));
    p->nin  = pehost_num_inputs(p->h);
    p->nout = pehost_num_outputs(p->h);
    if (p->nin  > MAXCH) p->nin  = MAXCH;
    if (p->nout > MAXCH) p->nout = MAXCH;
    if (p->nout < 1) p->nout = MAXCH;         /* the render path is stereo */

    fprintf(stderr, "vst-ace: hosting %s (%s) -- %d in, %d out, %d params%s\n",
            p->name, p->target, p->nin, p->nout, pehost_num_params(p->h),
            pehost_has_state(p->h) ? ", with opaque state" : "");
    return make_effect(p);
}

/* Older hosts, and a few current ones, still look for `main`. */
AEffect *main_plugin(audioMasterCb master) __attribute__((alias("VSTPluginMain")));
