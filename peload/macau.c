/* Host an Audio Unit loaded by machoload.
 *
 * An AUv2 does not export a render function. It exports a factory returning an
 * AudioComponentPlugInInterface -- Open, Close, and a Lookup that maps a numeric
 * selector to a method. Everything else goes through Lookup.
 *
 * The selector numbering came out of the plugins themselves rather than from
 * memory: Lookup answers for a selector it implements and returns NULL for one
 * it does not, so probing 0..0x1f across the corpus produced {1..7, 9, 10, 11,
 * 14..18} on all 23. That matches AUComponent.h with GetParameter at 6 and
 * Render at 14, and the four absent-but-in-range ones (0 Range, 19 ComplexRender,
 * 20 Process, 21 ProcessMultiple) are exactly the ones AUBase leaves alone.
 *
 * Structure layouts here (ASBD, AudioBufferList, AudioTimeStamp) are ABI, so
 * they are written out with their sizes asserted at compile time.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macau.h"
#include "macshim.h"
#include "machoload.h"

typedef int32_t OSStatus;

/* ------------------------------------------------------------ AU selectors */

enum {
    SEL_INITIALIZE        = 1,
    SEL_UNINITIALIZE      = 2,
    SEL_GET_PROPERTY_INFO = 3,
    SEL_GET_PROPERTY      = 4,
    SEL_SET_PROPERTY      = 5,
    SEL_GET_PARAMETER     = 6,
    SEL_SET_PARAMETER     = 7,
    SEL_RESET             = 9,
    SEL_RENDER            = 14,
    /* MusicDevice selectors live in their own range, which is why a scan of the
     * low numbers never found them. Without this an instrument AU renders
     * correctly and silently, because nothing ever gives it a note. */
    SEL_MIDI_EVENT        = 0x0101,
    SEL_START_NOTE        = 0x0102,
    SEL_STOP_NOTE         = 0x0103
};

/* properties */
enum {
    P_CLASS_INFO       = 0,
    P_SAMPLE_RATE      = 2,
    P_PARAMETER_LIST   = 3,
    P_PARAMETER_INFO   = 4,
    P_STREAM_FORMAT    = 8,
    P_MAX_FRAMES       = 14,
    P_SET_RENDER_CALLBACK = 23,
    P_ELEMENT_COUNT    = 11
};
enum { SCOPE_GLOBAL = 0, SCOPE_INPUT = 1, SCOPE_OUTPUT = 2 };

/* ------------------------------------------------------------- ABI structs */

typedef struct {
    double   mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket;
    uint32_t mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved;
} ASBD;
_Static_assert(sizeof(ASBD) == 40, "AudioStreamBasicDescription is 40 bytes");

#define FMT_LPCM  0x6D63706Cu             /* 'lpcm' */
#define FLAG_FLOAT          (1u << 0)
#define FLAG_PACKED         (1u << 3)
#define FLAG_NONINTERLEAVED (1u << 5)

typedef struct { uint32_t mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
_Static_assert(sizeof(AudioBuffer) == 16, "AudioBuffer is 16 bytes");

typedef struct { uint32_t mNumberBuffers; AudioBuffer mBuffers[8]; } ABL;
_Static_assert(offsetof(ABL, mBuffers) == 8, "mBuffers follows an aligned pad");

typedef struct {
    int16_t mSubframes, mSubframeDivisor;
    uint32_t mCounter, mType, mFlags;
    int16_t mHours, mMinutes, mSeconds, mFrames;
} SMPTETime;

typedef struct {
    double   mSampleTime;
    uint64_t mHostTime;
    double   mRateScalar;
    uint64_t mWordClockTime;
    SMPTETime mSMPTETime;
    uint32_t mFlags, mReserved;
} AudioTimeStamp;
_Static_assert(sizeof(AudioTimeStamp) == 64, "AudioTimeStamp is 64 bytes");
#define TS_SAMPLE_TIME_VALID 1

typedef struct {
    OSStatus (*inputProc)(void *refCon, uint32_t *flags, const AudioTimeStamp *ts,
                          uint32_t bus, uint32_t frames, ABL *data);
    void *inputProcRefCon;
} AURenderCallbackStruct;

typedef struct {
    /* AudioUnitParameterInfo begins with the name -- there is no length field in
     * front of it. A placeholder here shifted the name by four bytes, which cost
     * every parameter the first four characters of its name ("Voices" read back
     * as "es"). The fields after it were unaffected, because name[52] plus four
     * lands exactly where the struct's own padding would have ended, which is
     * why the values looked right and only the labels were wrong. */
    char     name[52];
    void    *unitName;
    uint32_t clumpID;
    void    *cfNameString;
    uint32_t unit;
    float    minValue, maxValue, defaultValue;
    uint32_t flags;
} AUParameterInfo;

/* --------------------------------------------------------------- the host */

typedef struct plugin_iface {
    OSStatus (*Open)(void *self, void *inst);
    OSStatus (*Close)(void *self);
    void    *(*Lookup)(int16_t selector);
    void    *reserved;
} plugin_iface;

struct macau {
    macho        *img;
    plugin_iface *iface;
    void         *self;
    int           opened, initialised;
    double        sr;
    uint32_t      maxframes, nchan;
    int           is_effect, interleaved;
    /* The render clock belongs to the unit's stream, not to whoever calls it. A
     * caller that passes no timestamp used to get mSampleTime = 0 on every
     * block, and a unit that keys off the timeline -- every spectral processor
     * here -- then produced nothing at all. */
    double        sample_time;

    OSStatus (*Initialize)(void *self);
    OSStatus (*Uninitialize)(void *self);
    OSStatus (*GetPropertyInfo)(void *self, uint32_t id, uint32_t scope,
                                uint32_t elem, uint32_t *size, uint8_t *writable);
    OSStatus (*GetProperty)(void *self, uint32_t id, uint32_t scope,
                            uint32_t elem, void *data, uint32_t *size);
    OSStatus (*SetProperty)(void *self, uint32_t id, uint32_t scope,
                            uint32_t elem, const void *data, uint32_t size);
    OSStatus (*GetParameter)(void *self, uint32_t id, uint32_t scope,
                             uint32_t elem, float *value);
    OSStatus (*SetParameter)(void *self, uint32_t id, uint32_t scope,
                             uint32_t elem, float value, uint32_t offset);
    OSStatus (*MIDIEvent)(void *self, uint32_t status, uint32_t d1, uint32_t d2,
                          uint32_t offsetFrames);
    OSStatus (*Reset)(void *self, uint32_t scope, uint32_t elem);
    OSStatus (*Render)(void *self, uint32_t *flags, const AudioTimeStamp *ts,
                       uint32_t bus, uint32_t frames, ABL *data);

    /* input the plugin pulls from us */
    const float *in_l, *in_r;
    uint32_t     in_frames, in_pos;
    float       *scratch;            /* de-interleave staging */
    size_t       scratch_n;
};

/* Export-name scan, as a file-scope callback: a nested function would be a GCC
 * extension and this is plain C. */
typedef struct { const char *suffix; const char *found; } entry_scan;

static void note_entry(const char *name, void *addr, void *ud)
{
    entry_scan *q = ud;
    size_t ln = strlen(name), ls = strlen(q->suffix);
    (void)addr;
    if (!q->found && ln > ls && !strcmp(name + ln - ls, q->suffix)) q->found = name;
}

static int au_verbose(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("MACAU_VERBOSE"); v = e && *e != '0'; } return v; }
/* Stage tracing: a plugin that hangs or faults inside one of these calls gives
 * no other clue which one it was. */
#define ALOG(x) do { if (au_verbose()) { fprintf(stderr, "  [au] " x "\n"); fflush(stderr); } } while (0)

static char g_err[256];
const char *macau_last_error(void) { return g_err; }

static void set_asbd(ASBD *f, double sr, uint32_t chans)
{
    memset(f, 0, sizeof *f);
    f->mSampleRate = sr;
    f->mFormatID = FMT_LPCM;
    /* The AU canonical format: 32-bit float, packed, one buffer per channel. */
    f->mFormatFlags = FLAG_FLOAT | FLAG_PACKED | FLAG_NONINTERLEAVED;
    f->mFramesPerPacket = 1;
    f->mBytesPerPacket = 4;
    f->mBytesPerFrame = 4;              /* per channel, because non-interleaved */
    f->mChannelsPerFrame = chans;
    f->mBitsPerChannel = 32;
}

/* The callback the effect pulls its input through. Hands out whatever the caller
 * gave macau_render(), zero-padded past the end. */
static OSStatus pull_input(void *refCon, uint32_t *flags, const AudioTimeStamp *ts,
                           uint32_t bus, uint32_t frames, ABL *data)
{
    macau *a = refCon;
    uint32_t b, i;
    (void)flags; (void)ts; (void)bus;
    if (!data) return -1;
    for (b = 0; b < data->mNumberBuffers; b++) {
        float *dst = data->mBuffers[b].mData;
        const float *src = (b == 0) ? a->in_l : a->in_r;
        if (!dst) continue;
        for (i = 0; i < frames; i++) {
            uint32_t p = a->in_pos + i;
            dst[i] = (src && p < a->in_frames) ? src[p] : 0.0f;
        }
    }
    a->in_pos += frames;
    return 0;
}

macau *macau_open(const char *bundle, double samplerate, int blocksize)
{
    macau *a = calloc(1, sizeof *a);
    void *(*factory)(const void *);
    struct { uint32_t type, subtype, manufacturer, flags, flagsmask; } desc;
    const char *entry = NULL;
    OSStatus st;

    if (!a) return NULL;
    g_err[0] = 0;
    a->sr = samplerate > 0 ? samplerate : 44100.0;
    a->maxframes = (uint32_t)(blocksize > 0 ? blocksize : 512);
    a->nchan = 2;

    if (!(a->img = macho_open(bundle))) {
        snprintf(g_err, sizeof g_err, "%s", macho_last_error());
        free(a); return NULL;
    }
    ALOG("run_init");
    macho_run_init(a->img);

    /* Find the factory. Modern AUs export <Something>Factory; older Component
     * Manager ones export <Something>_Entry. */
    /* "Factory" first, deliberately. An export ending in "Entry" is the old
     * Component Manager entry point -- ComponentResult(ComponentParameters *,
     * void *) -- and calling that as a modern factory returns whatever the
     * routine happened to leave in the return register. Dereferencing it crashed
     * the host on Ragnarok's AU. */
    {   static const char *const suffixes[] = { "Factory", "_Entry", NULL };
        int i;
        for (i = 0; suffixes[i] && !entry; i++) {
            entry_scan scan = { suffixes[i], NULL };
            macho_each_export(a->img, note_entry, &scan);
            entry = scan.found;
        }
    }
    if (!entry) {
        snprintf(g_err, sizeof g_err, "no AU factory export found");
        macau_close(a); return NULL;
    }
    {   size_t el = strlen(entry);
        if (el >= 5 && !strcmp(entry + el - 5, "Entry")) {
            snprintf(g_err, sizeof g_err,
                     "%s is a Component Manager entry point, not an "
                     "AudioComponent factory -- that ABI is not implemented",
                     entry);
            macau_close(a); return NULL;
        } }
    factory = (void *(*)(const void *))macho_symbol(a->img, entry[0] == '_' ? entry + 1 : entry);
    if (!factory) {
        snprintf(g_err, sizeof g_err, "%s did not resolve", entry);
        macau_close(a); return NULL;
    }

    memset(&desc, 0, sizeof desc);
    desc.type = 0x61756678u;                    /* 'aufx' -- an effect */
    ALOG("factory");
    a->iface = factory(&desc);
    if (!a->iface || !a->iface->Lookup) {
        snprintf(g_err, sizeof g_err, "factory returned no plug-in interface");
        macau_close(a); return NULL;
    }
    a->self = a->iface;

    a->Initialize      = a->iface->Lookup(SEL_INITIALIZE);
    a->Uninitialize    = a->iface->Lookup(SEL_UNINITIALIZE);
    a->GetPropertyInfo = a->iface->Lookup(SEL_GET_PROPERTY_INFO);
    a->GetProperty     = a->iface->Lookup(SEL_GET_PROPERTY);
    a->SetProperty     = a->iface->Lookup(SEL_SET_PROPERTY);
    a->GetParameter    = a->iface->Lookup(SEL_GET_PARAMETER);
    a->SetParameter    = a->iface->Lookup(SEL_SET_PARAMETER);
    a->Reset           = a->iface->Lookup(SEL_RESET);
    a->Render          = a->iface->Lookup(SEL_RENDER);
    a->MIDIEvent       = a->iface->Lookup(SEL_MIDI_EVENT);
    if (!a->Initialize || !a->SetProperty || !a->Render) {
        snprintf(g_err, sizeof g_err, "plugin does not implement Initialize/SetProperty/Render");
        macau_close(a); return NULL;
    }

    ALOG("Open");
    if (a->iface->Open && (st = a->iface->Open(a->self, a->self)) != 0) {
        snprintf(g_err, sizeof g_err, "Open failed: %d", (int)st);
        macau_close(a); return NULL;
    }
    a->opened = 1;
    return a;
}

int macau_configure(macau *a)
{
    ASBD f;
    OSStatus st;
    uint32_t mf;

    if (!a) return -1;

    /* Ask before telling. An AU advertises a default format on each scope, and
     * adapting that -- changing only the sample rate -- is far more likely to be
     * accepted than a format built from first principles. The canonical layout is
     * only a convention; a particular unit may want interleaved, or mono. */
    if (a->GetProperty) {
        uint32_t sz = sizeof f;
        if (a->GetProperty(a->self, P_STREAM_FORMAT, SCOPE_OUTPUT, 0, &f, &sz) == 0
            && sz >= sizeof f && f.mChannelsPerFrame > 0) {
            ALOG("adapting the unit's own output format");
            if (au_verbose())
                fprintf(stderr, "  [au]   %g Hz, %u ch, flags 0x%x, %u bytes/frame\n",
                        f.mSampleRate, f.mChannelsPerFrame, f.mFormatFlags,
                        f.mBytesPerFrame);
            f.mSampleRate = a->sr;
            a->nchan = f.mChannelsPerFrame > 2 ? 2 : f.mChannelsPerFrame;
            f.mChannelsPerFrame = a->nchan;
            /* Non-interleaved counts bytes per frame per channel; interleaved
             * counts the whole frame. */
            if (f.mFormatFlags & FLAG_NONINTERLEAVED) {
                f.mBytesPerFrame = 4;
                f.mBytesPerPacket = 4;
            } else {
                f.mBytesPerFrame = 4 * a->nchan;
                f.mBytesPerPacket = 4 * a->nchan;
            }
            a->interleaved = !(f.mFormatFlags & FLAG_NONINTERLEAVED);
        } else {
            set_asbd(&f, a->sr, a->nchan);
        }
    } else {
        set_asbd(&f, a->sr, a->nchan);
    }

    /* Output format first: an effect derives its input format from it, and some
     * AUs reject an input format set before the output side is known. */
    ALOG("StreamFormat output");
    if ((st = a->SetProperty(a->self, P_STREAM_FORMAT, SCOPE_OUTPUT, 0,
                             &f, sizeof f)) != 0) {
        snprintf(g_err, sizeof g_err, "output stream format rejected: %d", (int)st);
        return -1;
    }
    /* Input is optional: a generator has none, and refusing here would rule out
     * instruments for no reason. */
    ALOG("StreamFormat input");
    if (a->SetProperty(a->self, P_STREAM_FORMAT, SCOPE_INPUT, 0, &f, sizeof f) == 0)
        a->is_effect = 1;

    mf = a->maxframes;
    a->SetProperty(a->self, P_MAX_FRAMES, SCOPE_GLOBAL, 0, &mf, sizeof mf);
    { double sr = a->sr;
      a->SetProperty(a->self, P_SAMPLE_RATE, SCOPE_OUTPUT, 0, &sr, sizeof sr); }

    if (a->is_effect) {
        AURenderCallbackStruct cb;
        cb.inputProc = pull_input;
        cb.inputProcRefCon = a;
    ALOG("SetRenderCallback");
        if (a->SetProperty(a->self, P_SET_RENDER_CALLBACK, SCOPE_INPUT, 0,
                           &cb, sizeof cb) != 0)
            a->is_effect = 0;            /* it will not pull; render silence in */
    }

    ALOG("Initialize");
    if ((st = a->Initialize(a->self)) != 0) {
        snprintf(g_err, sizeof g_err, "Initialize failed: %d", (int)st);
        return -1;
    }
    a->scratch_n = (size_t)a->maxframes * 2;
    a->scratch = calloc(a->scratch_n, sizeof *a->scratch);
    if (!a->scratch) { snprintf(g_err, sizeof g_err, "out of memory"); return -1; }
    a->initialised = 1;
    return 0;
}

/* How many parameters the unit says it has. */
int macau_param_count(macau *a)
{
    uint32_t size = 0;
    uint8_t writable = 0;

    if (!a || !a->GetPropertyInfo) return 0;
    if (a->GetPropertyInfo(a->self, P_PARAMETER_LIST, SCOPE_GLOBAL, 0,
                           &size, &writable) != 0)
        return 0;
    return (int)(size / sizeof(uint32_t));
}

/* Read the parameter list, writing at most `max` ids.
 *
 * The read goes through a buffer sized to what the unit *declared*, not to what
 * the caller offered, because a unit hands back its whole list regardless of the
 * size passed in -- ModulAir has more than five hundred parameters and smashed a
 * 512-entry array on the caller's stack. Asking for less than a plugin intends to
 * write is not a limit it will respect. */
int macau_num_params(macau *a, unsigned *ids, int max)
{
    uint32_t size;
    int n, want;
    unsigned *tmp;

    if (!a || !a->GetProperty || !ids || max <= 0) return 0;
    if ((want = macau_param_count(a)) <= 0) return 0;
    if (!(tmp = calloc((size_t)want, sizeof *tmp))) return 0;

    size = (uint32_t)want * sizeof(uint32_t);
    if (a->GetProperty(a->self, P_PARAMETER_LIST, SCOPE_GLOBAL, 0, tmp, &size) != 0) {
        free(tmp);
        return 0;
    }
    n = (int)(size / sizeof(uint32_t));
    if (n > want) n = want;              /* never trust the returned size alone */
    if (n > max) n = max;
    memcpy(ids, tmp, (size_t)n * sizeof *ids);
    free(tmp);
    return n;
}

int macau_param_info(macau *a, uint32_t id, char *name, int namelen,
                     float *minv, float *maxv, float *defv)
{
    AUParameterInfo info;
    uint32_t size = sizeof info;
    if (namelen > 0) name[0] = 0;
    if (!a || !a->GetProperty) return -1;
    memset(&info, 0, sizeof info);
    if (a->GetProperty(a->self, P_PARAMETER_INFO, SCOPE_GLOBAL, id,
                       &info, &size) != 0)
        return -1;
    if (namelen > 0) snprintf(name, (size_t)namelen, "%.51s", info.name);
    if (minv) *minv = info.minValue;
    if (maxv) *maxv = info.maxValue;
    if (defv) *defv = info.defaultValue;
    return 0;
}

float macau_get_param(macau *a, uint32_t id)
{
    float v = 0.0f;
    if (a && a->GetParameter) a->GetParameter(a->self, id, SCOPE_GLOBAL, 0, &v);
    return v;
}
void macau_set_param(macau *a, uint32_t id, float v)
{
    if (a && a->SetParameter) a->SetParameter(a->self, id, SCOPE_GLOBAL, 0, v, 0);
}

int macau_render(macau *a, const float *in_l, const float *in_r,
                 float *out_l, float *out_r, int frames, double *time)
{
    AudioTimeStamp ts;
    ABL bl;
    uint32_t flags = 0;
    OSStatus st;

    if (!a || !a->initialised || frames <= 0) return -1;
    if ((uint32_t)frames > a->maxframes) return -1;

    a->in_l = in_l; a->in_r = in_r;
    a->in_frames = (uint32_t)frames;
    a->in_pos = 0;

    memset(&ts, 0, sizeof ts);
    ts.mSampleTime = time ? *time : a->sample_time;
    ts.mRateScalar = 1.0;
    ts.mFlags = TS_SAMPLE_TIME_VALID;

    memset(out_l, 0, (size_t)frames * 4);
    memset(out_r, 0, (size_t)frames * 4);
    memset(&bl, 0, sizeof bl);
    if (a->interleaved) {
        /* One buffer holding both channels; de-interleave after the call. */
        if ((size_t)frames * 2 > a->scratch_n) return -1;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = a->nchan;
        bl.mBuffers[0].mDataByteSize = (uint32_t)frames * 4 * a->nchan;
        bl.mBuffers[0].mData = a->scratch;
        memset(a->scratch, 0, (size_t)frames * 4 * a->nchan);
    } else {
        bl.mNumberBuffers = a->nchan;
        bl.mBuffers[0].mNumberChannels = 1;
        bl.mBuffers[0].mDataByteSize = (uint32_t)frames * 4;
        bl.mBuffers[0].mData = out_l;
        if (a->nchan > 1) {
            bl.mBuffers[1].mNumberChannels = 1;
            bl.mBuffers[1].mDataByteSize = (uint32_t)frames * 4;
            bl.mBuffers[1].mData = out_r;
        }
    }

    st = a->Render(a->self, &flags, &ts, 0, (uint32_t)frames, &bl);
    if (st != 0) {
        snprintf(g_err, sizeof g_err, "Render returned %d", (int)st);
        return -1;
    }
    if (a->interleaved) {
        int i;
        for (i = 0; i < frames; i++) {
            out_l[i] = a->scratch[i * a->nchan];
            out_r[i] = a->nchan > 1 ? a->scratch[i * a->nchan + 1] : out_l[i];
        }
    } else if (a->nchan == 1) {
        memcpy(out_r, out_l, (size_t)frames * 4);
    }
    if (time) *time += frames;
    a->sample_time += frames;
    return 0;
}

int macau_is_effect(const macau *a) { return a ? a->is_effect : 0; }

void macau_describe(const macau *a, const void *addr, char *out, size_t n)
{ macho_describe(a ? a->img : NULL, addr, out, n); }

void macau_close(macau *a)
{
    if (!a) return;
    if (a->initialised && a->Uninitialize) a->Uninitialize(a->self);
    if (a->opened && a->iface && a->iface->Close) a->iface->Close(a->self);
    free(a->scratch);
    /* Same reason as the VST2 side: nothing tied to this plugin may outlive it,
     * and least of all past the point where its image is unmapped. */
    macns_reset_gui();
    macmetal_reset();
    if (a->img) macho_close(a->img);
    free(a);
}

/* Send one MIDI byte triple. Safe to call from the GUI thread between renders,
 * which is the same contract the VST2 side offers. Returns 0 if the unit has no
 * MusicDevice interface -- an effect, or an instrument that only takes events
 * through a property. */
int macau_midi(macau *a, int status, int d1, int d2)
{
    if (!a || !a->MIDIEvent) return 0;
    return a->MIDIEvent(a->self, (uint32_t)(status & 0xff), (uint32_t)(d1 & 0x7f),
                        (uint32_t)(d2 & 0x7f), 0) == 0;
}

/* Does this unit accept MIDI at all? */
int macau_has_midi(const macau *a) { return a && a->MIDIEvent != NULL; }
