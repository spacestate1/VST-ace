#define _GNU_SOURCE
#include "vst3.h"

#include <dirent.h>
#include <dlfcn.h>
#include "peimage.h"
#include "pehost.h"
#include "machoload.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* This file is compiled twice, once per plugin ABI, and the two objects live
 * side by side. Everything inside is static except the entry points, which are
 * renamed through V3N(), so the two copies cannot collide at link time.
 *
 *   native Linux .vst3 : SysV vtables, plain interface-ID byte order
 *   Windows .vst3      : Microsoft x64 vtables, COM interface-ID byte order
 *
 * vst3_dispatch.c sniffs the binary and calls the right set. */
#ifdef V3_MSABI
#  define V3CALL   __attribute__((ms_abi))
#  define V3N(x)   ms_##x
#  define V3_COM   1
#else
#  define V3CALL
#  define V3N(x)   sv_##x
#  define V3_COM   0
#endif

/* Set PELOAD_VERBOSE=1 to trace VST3 setup step by step. */
static int v3_verbose(void)
{ static int v = -1; if (v < 0) { const char *e = getenv("PELOAD_VERBOSE"); v = e && *e != '0'; } return v; }
#define VLOG(...) do { if (v3_verbose()) fprintf(stderr, __VA_ARGS__); } while (0)

static double v3_now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ------------------------------------------------------------ VST3 basics */

typedef int32_t tresult;
#define V3_OK          0
#define V3_NOIFACE     (-1)   /* kNoInterface */
#define V3_NOTIMPL     (-4)   /* kNotImplemented */

typedef char TUID[16];

/* Interface IDs. The SDK lays these out differently on Windows (COM order)
 * than elsewhere; native Linux plugins use the plain big-endian-per-word form,
 * which is what a probe against Surge XT, Odin2 and OB-Xf confirmed. */
#if V3_COM
/* Windows builds lay interface IDs out as a COM GUID: first word little-endian,
 * second word's halves swapped, last two big-endian. */
#define TU(l1,l2,l3,l4) { \
  (char)((l1)&0xFF),(char)(((l1)>>8)&0xFF),(char)(((l1)>>16)&0xFF),(char)(((l1)>>24)&0xFF), \
  (char)(((l2)>>16)&0xFF),(char)(((l2)>>24)&0xFF),(char)((l2)&0xFF),(char)(((l2)>>8)&0xFF), \
  (char)(((l3)>>24)&0xFF),(char)(((l3)>>16)&0xFF),(char)(((l3)>>8)&0xFF),(char)((l3)&0xFF), \
  (char)(((l4)>>24)&0xFF),(char)(((l4)>>16)&0xFF),(char)(((l4)>>8)&0xFF),(char)((l4)&0xFF) }
#else
/* Everywhere else it is simply each 32-bit word big-endian. */
#define TU(l1,l2,l3,l4) { \
  (char)(((l1)>>24)&0xFF),(char)(((l1)>>16)&0xFF),(char)(((l1)>>8)&0xFF),(char)((l1)&0xFF), \
  (char)(((l2)>>24)&0xFF),(char)(((l2)>>16)&0xFF),(char)(((l2)>>8)&0xFF),(char)((l2)&0xFF), \
  (char)(((l3)>>24)&0xFF),(char)(((l3)>>16)&0xFF),(char)(((l3)>>8)&0xFF),(char)((l3)&0xFF), \
  (char)(((l4)>>24)&0xFF),(char)(((l4)>>16)&0xFF),(char)(((l4)>>8)&0xFF),(char)((l4)&0xFF) }
#endif

static const TUID IID_FUnknown         = TU(0x00000000,0x00000000,0xC0000000,0x00000046);
static const TUID IID_IComponent       = TU(0xE831FF31,0xF2D54301,0x928EBBEE,0x25697802);
static const TUID IID_IAudioProcessor  = TU(0x42043F99,0xB7DA453C,0xA569E79D,0x9AAEC33D);
static const TUID IID_IEditController  = TU(0xDCD7BBE3,0x7742448D,0xA874AACC,0x979C759E);
static const TUID IID_IUnitInfo        = TU(0x3D4BD6B5,0x913A4FD2,0xA886E768,0xA5EB92C1);
static const TUID IID_IComponentHandler= TU(0x93A0BEA3,0x0BD045DB,0x8E890B0C,0xC1E46AC6);
static const TUID IID_IHostApplication = TU(0x58E595CC,0xDB2D4969,0x8B6AAF8C,0x36A664E5);
/* Plugins allocate these through the host so both sides share an allocator.
 * DPF asserts and leaves its view half-built when the host refuses. */
static const TUID IID_IMessage        = TU(0x936F033B,0xC6C047DB,0xBB0882F8,0x13C1E613);
static const TUID IID_IAttributeList  = TU(0x1E5F0AEB,0xCC7F4533,0xA2544011,0x38AD5EE4);
static const TUID IID_IEventList       = TU(0x3A2C4214,0x346349FE,0xB2C4F397,0xB9695A44);
static const TUID IID_IParameterChanges= TU(0xA4779663,0x0BB64A56,0xB44384A8,0x466FEB9D);
static const TUID IID_IParamValueQueue = TU(0x01263A18,0xED074F6F,0x98C9D356,0x4686F9BA);
static const TUID IID_IConnectionPoint = TU(0x70A4156F,0x6E6E4026,0x989148BF,0xAA60D8D1);
static const TUID IID_IUnitHandler     = TU(0x4B5147F8,0x4654486B,0x8CDF2A25,0x634BB74C);
/* How a controller says which parameter a MIDI controller drives. VST3 has no
 * event for a pitch bend or a CC: they are parameters, and this is the only
 * thing that knows which. */
static const TUID IID_IMidiMapping     = TU(0xDF0FF9F7,0x49B74669,0xB63AB732,0x7ADBF5E5);
static const TUID IID_IComponentHandler2=TU(0xF040B4B3,0xA36045EC,0xABCDC045,0xB4D5A2CC);
static const TUID IID_IPlugView        = TU(0x5BC32507,0xD06049EA,0xA6151B52,0x2B755B29);
static const TUID IID_IPlugFrame       = TU(0x367FAF01,0xAFA94693,0x8D4DA2A0,0xED0882A3);
/* Steinberg::Linux -- the host owns the event loop and the plugin registers its
 * X11 descriptors and timers with us. JUCE editors will not repaint without it. */
static const TUID IID_IRunLoop         = TU(0x18C35366,0x97764F1A,0x9C5B8385,0x7A871389);
static const TUID IID_IEventHandler    = TU(0x561E65C9,0x13A0496F,0x813A2C35,0x654D7983);
static const TUID IID_ITimerHandler    = TU(0x10BDD94F,0x41424774,0x821FAD8F,0xECA72CA9);

#define PLATFORM_X11 "X11EmbedWindowID"
#define PLATFORM_HWND "HWND"
#define PLATFORM_NSVIEW "NSView"

/* media types / bus directions */
#define MT_AUDIO 0
#define MT_EVENT 1
#define BD_INPUT 0
#define BD_OUTPUT 1

typedef struct { char vendor[64], url[256], email[128]; int32_t flags; } PFactoryInfo;
typedef struct { TUID cid; int32_t cardinality; char category[32], name[64]; } PClassInfo;

typedef struct {
    int32_t  mediaType, direction;
    int32_t  channelCount;
    int16_t  name[128];
    int32_t  busType, flags;
} BusInfo;

typedef struct {
    uint32_t id;
    int16_t  title[128], shortTitle[128], units[128];
    int32_t  stepCount;
    double   defaultNormalizedValue;
    int32_t  unitId, flags;
} ParameterInfo;

typedef struct {
    int32_t processMode, symbolicSampleSize, maxSamplesPerBlock;
    double  sampleRate;
} ProcessSetup;

typedef struct {
    int32_t  numChannels;
    uint64_t silenceFlags;
    float  **channelBuffers32;
} AudioBusBuffers;

typedef struct { int16_t channel, pitch; float tuning, velocity; int32_t length, noteId; } NoteOnEvent;
typedef struct { int16_t channel, pitch; float velocity; int32_t noteId; float tuning; } NoteOffEvent;

typedef struct {
    int32_t  busIndex, sampleOffset;
    double   ppqPosition;
    uint16_t flags, type;
    /* The SDK's event union includes members holding pointers, so it is
     * 8-byte aligned and starts at offset 24 -- not 20, which is where a union
     * of only note events would land it. Without the `align` member the plugin
     * reads pitch and velocity four bytes adrift and ignores the note. */
    union {
        NoteOnEvent  noteOn;
        NoteOffEvent noteOff;
        double       align;
        char         pad[24];
    } u;
} V3Event;
/* 8 + 8 + 4 + (4 pad) + 24 == 48, matching Steinberg's Event. */
_Static_assert(sizeof(V3Event) == 48, "VST3 Event must be 48 bytes");
#define EV_NOTE_ON  0
#define EV_NOTE_OFF 1

typedef struct {
    int32_t processMode, symbolicSampleSize, numSamples, numInputs, numOutputs;
    AudioBusBuffers *inputs, *outputs;
    void *inputParameterChanges, *outputParameterChanges;
    void *inputEvents, *outputEvents;
    void *processContext;
} ProcessData;

/* ------------------------------------------------------------ the vtables */

#define FUNKNOWN_SLOTS \
    tresult (V3CALL *queryInterface)(void *, const TUID, void **); \
    uint32_t (V3CALL *addRef)(void *); \
    uint32_t (V3CALL *release)(void *)

#define PLUGINBASE_SLOTS \
    tresult (V3CALL *initialize)(void *, void *context); \
    tresult (V3CALL *terminate)(void *)

typedef struct { FUNKNOWN_SLOTS; } FUnknownVtbl;
typedef struct { const FUnknownVtbl *vt; } FUnknown;

typedef struct {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *getFactoryInfo)(void *, PFactoryInfo *);
    int32_t (V3CALL *countClasses)(void *);
    tresult (V3CALL *getClassInfo)(void *, int32_t, PClassInfo *);
    tresult (V3CALL *createInstance)(void *, const char *cid, const char *iid, void **obj);
} IPluginFactoryVtbl;
typedef struct { const IPluginFactoryVtbl *vt; } IPluginFactory;

typedef struct {
    FUNKNOWN_SLOTS;
    PLUGINBASE_SLOTS;
    tresult (V3CALL *getControllerClassId)(void *, TUID);
    tresult (V3CALL *setIoMode)(void *, int32_t);
    int32_t (V3CALL *getBusCount)(void *, int32_t mediaType, int32_t dir);
    tresult (V3CALL *getBusInfo)(void *, int32_t mediaType, int32_t dir, int32_t index, BusInfo *);
    tresult (V3CALL *getRoutingInfo)(void *, void *in, void *out);
    tresult (V3CALL *activateBus)(void *, int32_t mediaType, int32_t dir, int32_t index, uint8_t state);
    tresult (V3CALL *setActive)(void *, uint8_t state);
    tresult (V3CALL *setState)(void *, void *stream);
    tresult (V3CALL *getState)(void *, void *stream);
} IComponentVtbl;
typedef struct { const IComponentVtbl *vt; } IComponent;

typedef struct {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *setBusArrangements)(void *, uint64_t *inputs, int32_t numIns,
                                   uint64_t *outputs, int32_t numOuts);
    tresult (V3CALL *getBusArrangement)(void *, int32_t dir, int32_t index, uint64_t *arr);
    tresult (V3CALL *canProcessSampleSize)(void *, int32_t symbolicSampleSize);
    uint32_t (V3CALL *getLatencySamples)(void *);
    tresult (V3CALL *setupProcessing)(void *, ProcessSetup *);
    tresult (V3CALL *setProcessing)(void *, uint8_t state);
    tresult (V3CALL *process)(void *, ProcessData *);
    uint32_t (V3CALL *getTailSamples)(void *);
} IAudioProcessorVtbl;
typedef struct { const IAudioProcessorVtbl *vt; } IAudioProcessor;

typedef struct {
    FUNKNOWN_SLOTS;
    PLUGINBASE_SLOTS;
    tresult (V3CALL *setComponentState)(void *, void *stream);
    tresult (V3CALL *setState)(void *, void *stream);
    tresult (V3CALL *getState)(void *, void *stream);
    int32_t (V3CALL *getParameterCount)(void *);
    tresult (V3CALL *getParameterInfo)(void *, int32_t index, ParameterInfo *);
    tresult (V3CALL *getParamStringByValue)(void *, uint32_t id, double valueNormalized, int16_t *string);
    tresult (V3CALL *getParamValueByString)(void *, uint32_t id, int16_t *string, double *valueNormalized);
    double (V3CALL *normalizedParamToPlain)(void *, uint32_t id, double valueNormalized);
    double (V3CALL *plainParamToNormalized)(void *, uint32_t id, double plainValue);
    double (V3CALL *getParamNormalized)(void *, uint32_t id);
    tresult (V3CALL *setParamNormalized)(void *, uint32_t id, double value);
    tresult (V3CALL *setComponentHandler)(void *, void *handler);
    void *(V3CALL *createView)(void *, const char *name);
} IEditControllerVtbl;
typedef struct { const IEditControllerVtbl *vt; } IEditController;

/* IMidiMapping. One method: given a bus, a channel and a MIDI controller
 * number, which parameter does it drive. The controller numbers are the CC
 * numbers, with two past the end of them -- 128 is channel aftertouch and 129
 * is the pitch bend. */
enum { V3_CTRL_AFTERTOUCH = 128, V3_CTRL_PITCHBEND = 129 };
typedef struct {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *getMidiControllerAssignment)(void *, int32_t busIndex,
                                                  int16_t channel,
                                                  int16_t midiControllerNumber,
                                                  uint32_t *id);
} IMidiMappingVtbl;
typedef struct { const IMidiMappingVtbl *vt; } IMidiMapping;

/* IConnectionPoint: the component and controller are separate objects and the
 * host is responsible for introducing them. Plugins built on JUCE fetch their
 * processor handle during connect(), so without this the controller comes up
 * with an empty parameter list and no DSP wiring. Connecting the two peers
 * directly to each other avoids needing an IMessage relay. */
typedef struct IConnectionPointVtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *connect)(void *, void *other);
    tresult (V3CALL *disconnect)(void *, void *other);
    tresult (V3CALL *notify)(void *, void *message);
} IConnectionPointVtbl;
typedef struct { const IConnectionPointVtbl *vt; } IConnectionPoint;

typedef struct { int32_t left, top, right, bottom; } ViewRect;

typedef struct {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *isPlatformTypeSupported)(void *, const char *type);
    tresult (V3CALL *attached)(void *, void *parent, const char *type);
    tresult (V3CALL *removed)(void *);
    tresult (V3CALL *onWheel)(void *, float distance);
    tresult (V3CALL *onKeyDown)(void *, int16_t key, int16_t code, int16_t mods);
    tresult (V3CALL *onKeyUp)(void *, int16_t key, int16_t code, int16_t mods);
    tresult (V3CALL *getSize)(void *, ViewRect *);
    tresult (V3CALL *onSize)(void *, ViewRect *);
    tresult (V3CALL *onFocus)(void *, uint8_t state);
    tresult (V3CALL *setFrame)(void *, void *frame);
    tresult (V3CALL *canResize)(void *);
    tresult (V3CALL *checkSizeConstraint)(void *, ViewRect *);
} IPlugViewVtbl;
typedef struct { const IPlugViewVtbl *vt; } IPlugView;

/* The plugin's side of the run loop: we call these back when its descriptor is
 * readable or its timer fires. */
typedef struct {
    FUNKNOWN_SLOTS;
    void (V3CALL *onFDIsSet)(void *, int fd);
} IEventHandlerVtbl;
typedef struct { const IEventHandlerVtbl *vt; } IEventHandler;

typedef struct {
    FUNKNOWN_SLOTS;
    void (V3CALL *onTimer)(void *);
} ITimerHandlerVtbl;
typedef struct { const ITimerHandlerVtbl *vt; } ITimerHandler;

typedef struct { int32_t id; int16_t name[128]; int32_t programCount; } ProgramListInfo;

typedef struct {
    FUNKNOWN_SLOTS;
    int32_t (V3CALL *getUnitCount)(void *);
    tresult (V3CALL *getUnitInfo)(void *, int32_t index, void *info);
    int32_t (V3CALL *getProgramListCount)(void *);
    tresult (V3CALL *getProgramListInfo)(void *, int32_t index, ProgramListInfo *info);
    tresult (V3CALL *getProgramName)(void *, int32_t listId, int32_t programIndex, int16_t *name);
    tresult (V3CALL *getProgramInfo)(void *, int32_t listId, int32_t programIndex,
                                     const char *attributeId, int16_t *value);
    tresult (V3CALL *hasProgramPitchNames)(void *, int32_t listId, int32_t programIndex);
    tresult (V3CALL *getProgramPitchName)(void *, int32_t listId, int32_t programIndex,
                                          int16_t midiPitch, int16_t *name);
    int32_t (V3CALL *getSelectedUnit)(void *);
    tresult (V3CALL *selectUnit)(void *, int32_t unitId);
    tresult (V3CALL *getUnitByBus)(void *, int32_t type, int32_t dir, int32_t busIndex,
                                   int32_t channel, int32_t *unitId);
    tresult (V3CALL *setUnitProgramData)(void *, int32_t listOrUnitId, int32_t programIndex,
                                         void *data);
} IUnitInfoVtbl;
typedef struct { const IUnitInfoVtbl *vt; } IUnitInfo;

/* --------------------------------------------- host-side objects we supply */

/* IParamValueQueue: one parameter's changes within a block. A single point per
 * block is enough for GUI edits; automation curves would need more. */
typedef struct {
    const struct IParamValueQueueVtbl *vt;
    uint32_t id;
    double   value;
} ParamQueue;

typedef struct IParamValueQueueVtbl {
    FUNKNOWN_SLOTS;
    uint32_t (V3CALL *getParameterId)(void *);
    int32_t (V3CALL *getPointCount)(void *);
    tresult (V3CALL *getPoint)(void *, int32_t index, int32_t *sampleOffset, double *value);
    tresult (V3CALL *addPoint)(void *, int32_t sampleOffset, double value, int32_t *index);
} IParamValueQueueVtbl;

static V3CALL tresult  pq_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IParamValueQueue, 16) || !memcmp(iid, IID_FUnknown, 16)) {
        *o = s; return V3_OK;
    }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL uint32_t pq_ref(void *s) { (void)s; return 1; }
static V3CALL uint32_t pq_getid(void *s) { return ((ParamQueue *)s)->id; }
static V3CALL int32_t  pq_count(void *s) { (void)s; return 1; }
static V3CALL tresult  pq_getpoint(void *s, int32_t i, int32_t *off, double *val)
{
    (void)i;
    if (off) *off = 0;
    if (val) *val = ((ParamQueue *)s)->value;
    return V3_OK;
}
static V3CALL tresult  pq_addpoint(void *s, int32_t off, double val, int32_t *idx)
{
    (void)off;
    ((ParamQueue *)s)->value = val;
    if (idx) *idx = 0;
    return V3_OK;
}
static const IParamValueQueueVtbl g_pq_vt = {
    pq_qi, pq_ref, pq_ref, pq_getid, pq_count, pq_getpoint, pq_addpoint
};

/* IParameterChanges: the set of queues handed to process(). */
#define MAX_PARAM_CHANGES 64
typedef struct {
    const struct IParameterChangesVtbl *vt;
    ParamQueue q[MAX_PARAM_CHANGES];
    int32_t    n;
} ParamChanges;

typedef struct IParameterChangesVtbl {
    FUNKNOWN_SLOTS;
    int32_t (V3CALL *getParameterCount)(void *);
    void *(V3CALL *getParameterData)(void *, int32_t index);
    void *(V3CALL *addParameterData)(void *, const uint32_t *id, int32_t *index);
} IParameterChangesVtbl;

static V3CALL tresult pc_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IParameterChanges, 16) || !memcmp(iid, IID_FUnknown, 16)) {
        *o = s; return V3_OK;
    }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL uint32_t pc_ref(void *s) { (void)s; return 1; }
static V3CALL int32_t  pc_count(void *s) { return ((ParamChanges *)s)->n; }
static V3CALL void *pc_get(void *s, int32_t i)
{
    ParamChanges *p = s;
    return (i >= 0 && i < p->n) ? &p->q[i] : NULL;
}
static V3CALL void *pc_add(void *s, const uint32_t *id, int32_t *index)
{
    ParamChanges *p = s;
    int32_t i;
    for (i = 0; i < p->n; i++)
        if (p->q[i].id == *id) { if (index) *index = i; return &p->q[i]; }
    if (p->n >= MAX_PARAM_CHANGES) return NULL;
    i = p->n++;
    p->q[i].vt = &g_pq_vt;
    p->q[i].id = *id;
    p->q[i].value = 0.0;
    if (index) *index = i;
    return &p->q[i];
}
static const IParameterChangesVtbl g_pc_vt = {
    pc_qi, pc_ref, pc_ref, pc_count, pc_get, pc_add
};

/* IEventList: the note events for one block. */
#define MAX_EVENTS 64
typedef struct {
    const struct IEventListVtbl *vt;
    V3Event ev[MAX_EVENTS];
    int32_t n;
} EventList;

typedef struct IEventListVtbl {
    FUNKNOWN_SLOTS;
    int32_t (V3CALL *getEventCount)(void *);
    tresult (V3CALL *getEvent)(void *, int32_t index, V3Event *e);
    tresult (V3CALL *addEvent)(void *, V3Event *e);
} IEventListVtbl;

static V3CALL tresult el_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IEventList, 16) || !memcmp(iid, IID_FUnknown, 16)) {
        *o = s; return V3_OK;
    }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL uint32_t el_ref(void *s) { (void)s; return 1; }
static V3CALL int32_t el_count(void *s) { return ((EventList *)s)->n; }
static V3CALL tresult  el_get(void *s, int32_t i, V3Event *e)
{
    EventList *l = s;
    if (i < 0 || i >= l->n || !e) return V3_NOTIMPL;
    *e = l->ev[i];
    return V3_OK;
}
static V3CALL tresult el_add(void *s, V3Event *e)
{
    EventList *l = s;
    if (!e || l->n >= MAX_EVENTS) return V3_NOTIMPL;
    l->ev[l->n++] = *e;
    return V3_OK;
}
static const IEventListVtbl g_el_vt = { el_qi, el_ref, el_ref, el_count, el_get, el_add };

/* IComponentHandler: the controller reports edits through this. We only need it
 * to exist -- a plugin that cannot set a handler often refuses to initialise. */
typedef struct IComponentHandlerVtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *beginEdit)(void *, uint32_t id);
    tresult (V3CALL *performEdit)(void *, uint32_t id, double valueNormalized);
    tresult (V3CALL *endEdit)(void *, uint32_t id);
    tresult (V3CALL *restartComponent)(void *, int32_t flags);
} IComponentHandlerVtbl;

/* Companion handler interfaces. A controller with program lists notifies the
 * host through IUnitHandler when the selection changes, and SDK-based
 * controllers reach for IComponentHandler2 to mark state dirty. Each interface
 * needs its own vtable, so the handler carries one sub-object per interface and
 * queryInterface hands back the matching one. */
typedef struct IUnitHandlerVtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *notifyUnitSelection)(void *, int32_t unitId);
    tresult (V3CALL *notifyProgramListChange)(void *, int32_t listId, int32_t programIndex);
} IUnitHandlerVtbl;

typedef struct IComponentHandler2Vtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *setDirty)(void *, uint8_t state);
    tresult (V3CALL *requestOpenEditor)(void *, const char *name);
    tresult (V3CALL *startGroupEdit)(void *);
    tresult (V3CALL *finishGroupEdit)(void *);
} IComponentHandler2Vtbl;

typedef struct {
    const IComponentHandlerVtbl *vt;                 /* primary interface */
    struct { const IUnitHandlerVtbl        *vt; } unith;
    struct { const IComponentHandler2Vtbl  *vt; } ch2;
} Handler;

static V3CALL uint32_t uh_ref(void *s) { (void)s; return 1; }
static V3CALL tresult  uh_notifysel(void *s, int32_t u) { (void)s;(void)u; return V3_OK; }
static V3CALL tresult  uh_notifyprog(void *s, int32_t l, int32_t p)
{ (void)s;(void)l;(void)p; return V3_OK; }
static V3CALL tresult  uh_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IUnitHandler, 16) || !memcmp(iid, IID_FUnknown, 16)) { *o = s; return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static const IUnitHandlerVtbl g_uh_vt = {
    uh_qi, uh_ref, uh_ref, uh_notifysel, uh_notifyprog
};

static V3CALL tresult ch2_setdirty(void *s, uint8_t st) { (void)s;(void)st; return V3_OK; }
static V3CALL tresult ch2_openeditor(void *s, const char *n) { (void)s;(void)n; return V3_NOTIMPL; }
static V3CALL tresult ch2_group(void *s) { (void)s; return V3_OK; }
static V3CALL tresult ch2_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IComponentHandler2, 16) || !memcmp(iid, IID_FUnknown, 16))
        { *o = s; return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static const IComponentHandler2Vtbl g_ch2_vt = {
    ch2_qi, uh_ref, uh_ref, ch2_setdirty, ch2_openeditor, ch2_group, ch2_group
};

static V3CALL tresult ch_qi(void *s, const TUID iid, void **o)
{
    Handler *h = s;
    if (!memcmp(iid, IID_IComponentHandler, 16) || !memcmp(iid, IID_FUnknown, 16)) {
        *o = s; return V3_OK;
    }
    if (!memcmp(iid, IID_IUnitHandler, 16))      { *o = &h->unith; return V3_OK; }
    if (!memcmp(iid, IID_IComponentHandler2, 16)) { *o = &h->ch2;  return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL uint32_t ch_ref(void *s) { (void)s; return 1; }
static V3CALL tresult  ch_edit(void *s, uint32_t id) { (void)s;(void)id; return V3_OK; }
static V3CALL tresult  ch_perform(void *s, uint32_t id, double v) { (void)s;(void)id;(void)v; return V3_OK; }
static V3CALL tresult  ch_restart(void *s, int32_t f) { (void)s;(void)f; return V3_OK; }
static const IComponentHandlerVtbl g_ch_vt = {
    ch_qi, ch_ref, ch_ref, ch_edit, ch_perform, ch_edit, ch_restart
};

/* IRunLoop. Registrations are forwarded to whatever UI toolkit is driving us
 * through the hooks below, because only it knows how to watch a descriptor or
 * run a timer; this layer just remembers the plugin-side handler to call back. */
typedef struct IRunLoopVtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *registerEventHandler)(void *, void *handler, int fd);
    tresult (V3CALL *unregisterEventHandler)(void *, void *handler);
    tresult (V3CALL *registerTimer)(void *, void *handler, uint64_t ms);
    tresult (V3CALL *unregisterTimer)(void *, void *handler);
} IRunLoopVtbl;

static v3_runloop_hooks g_hooks;

void V3N(v3_set_runloop_hooks)(const v3_runloop_hooks *h)
{ if (h) g_hooks = *h; else memset(&g_hooks, 0, sizeof g_hooks); }

static V3CALL uint32_t rl_ref(void *s) { (void)s; return 1; }
static V3CALL tresult rl_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IRunLoop, 16) || !memcmp(iid, IID_FUnknown, 16)) { *o = s; return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL tresult rl_reg_fd(void *s, void *handler, int fd)
{
    (void)s;
    VLOG("vst3: plugin registered fd %d\n", fd);
    if (!g_hooks.add_fd) return V3_NOTIMPL;
    g_hooks.add_fd(g_hooks.ud, handler, fd);
    return V3_OK;
}
static V3CALL tresult rl_unreg_fd(void *s, void *handler)
{ (void)s; if (g_hooks.del_fd) g_hooks.del_fd(g_hooks.ud, handler); return V3_OK; }
static V3CALL tresult rl_reg_timer(void *s, void *handler, uint64_t ms)
{
    (void)s;
    VLOG("vst3: plugin registered a %llu ms timer\n", (unsigned long long)ms);
    if (!g_hooks.add_timer) return V3_NOTIMPL;
    g_hooks.add_timer(g_hooks.ud, handler, ms ? ms : 16);
    return V3_OK;
}
static V3CALL tresult rl_unreg_timer(void *s, void *handler)
{ (void)s; if (g_hooks.del_timer) g_hooks.del_timer(g_hooks.ud, handler); return V3_OK; }
static const IRunLoopVtbl g_rl_vt = {
    rl_qi, rl_ref, rl_ref, rl_reg_fd, rl_unreg_fd, rl_reg_timer, rl_unreg_timer
};

/* Called by the UI layer when a registered descriptor or timer fires. */
void V3N(v3_runloop_fd)(void *handler, int fd)
{ IEventHandler *h = handler; if (h && h->vt && h->vt->onFDIsSet) h->vt->onFDIsSet(h, fd); }
void V3N(v3_runloop_timer)(void *handler)
{ ITimerHandler *h = handler; if (h && h->vt && h->vt->onTimer) h->vt->onTimer(h); }

/* IPlugFrame: a resizable editor asks the host to change its window size. */
typedef struct IPlugFrameVtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *resizeView)(void *, void *view, ViewRect *newSize);
} IPlugFrameVtbl;

typedef struct {
    const IPlugFrameVtbl *vt;
    struct { const IRunLoopVtbl *vt; } runloop;
} PlugFrame;

static V3CALL tresult pf_qi(void *s, const TUID iid, void **o)
{
    PlugFrame *f = s;
    if (!memcmp(iid, IID_IPlugFrame, 16) || !memcmp(iid, IID_FUnknown, 16)) { *o = s; return V3_OK; }
    /* Editors reach the run loop through the frame as well as the context. */
    if (!memcmp(iid, IID_IRunLoop, 16)) { *o = &f->runloop; return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL tresult pf_resize(void *s, void *view, ViewRect *r)
{
    (void)s; (void)view;
    if (r && g_hooks.resize) g_hooks.resize(g_hooks.ud, r->right - r->left, r->bottom - r->top);
    return V3_OK;
}
static const IPlugFrameVtbl g_pf_vt = { pf_qi, rl_ref, rl_ref, pf_resize };

/* IHostApplication: JUCE- and SDK-based plugins ask for this during
 * initialize() and some refuse to start without it. */
typedef struct IHostApplicationVtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *getName)(void *, int16_t *name);
    tresult (V3CALL *createInstance)(void *, TUID cid, TUID iid, void **obj);
} IHostApplicationVtbl;

typedef struct {
    const IHostApplicationVtbl *vt;
    struct { const IRunLoopVtbl *vt; } runloop;
} HostApp;

static V3CALL tresult ha_qi(void *s, const TUID iid, void **o)
{
    HostApp *h = s;
    if (!memcmp(iid, IID_IHostApplication, 16) || !memcmp(iid, IID_FUnknown, 16)) {
        *o = s; return V3_OK;
    }
    if (!memcmp(iid, IID_IRunLoop, 16)) { *o = &h->runloop; return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL uint32_t ha_ref(void *s) { (void)s; return 1; }
static V3CALL tresult  ha_getname(void *s, int16_t *name)
{
    static const char *n = "pestudio";
    int i;
    (void)s;
    if (!name) return V3_NOTIMPL;
    for (i = 0; n[i]; i++) name[i] = (int16_t)n[i];
    name[i] = 0;
    return V3_OK;
}
/* ---- IAttributeList and IMessage ---------------------------------------
 *
 * Small host-provided objects a plugin uses to talk between its processor and
 * its editor. Only the storage has to be real; nothing here interprets it. */
#define ATTR_MAX 32
typedef struct {
    char     key[64];
    int      kind;                 /* 1 int, 2 float, 3 string, 4 binary */
    int64_t  i;
    double   f;
    void    *blob;
    uint32_t blobsz;
    int16_t  str[256];
} attr_entry;

typedef struct IAttributeListVtbl IAttributeListVtbl;
typedef struct {
    const IAttributeListVtbl *vt;
    int32_t     refs;
    attr_entry  e[ATTR_MAX];
    int         n;
} AttrList;

static attr_entry *attr_find(AttrList *a, const char *id, int create)
{
    int i;
    if (!a || !id) return NULL;
    for (i = 0; i < a->n; i++)
        if (!strcmp(a->e[i].key, id)) return &a->e[i];
    if (!create || a->n >= ATTR_MAX) return NULL;
    memset(&a->e[a->n], 0, sizeof a->e[0]);
    snprintf(a->e[a->n].key, sizeof a->e[0].key, "%s", id);
    return &a->e[a->n++];
}

static V3CALL tresult al_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IAttributeList, 16) || !memcmp(iid, IID_FUnknown, 16))
        { *o = s; return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL uint32_t al_addref(void *s) { return (uint32_t)(++((AttrList *)s)->refs); }
static V3CALL uint32_t al_release(void *s)
{
    AttrList *a = s;
    int i;
    if (--a->refs > 0) return (uint32_t)a->refs;
    for (i = 0; i < a->n; i++) free(a->e[i].blob);
    free(a);
    return 0;
}
static V3CALL tresult al_setInt(void *s, const char *id, int64_t v)
{ attr_entry *e = attr_find(s, id, 1); if (!e) return V3_NOTIMPL; e->kind = 1; e->i = v; return V3_OK; }
static V3CALL tresult al_getInt(void *s, const char *id, int64_t *v)
{ attr_entry *e = attr_find(s, id, 0); if (!e || !v) return V3_NOTIMPL; *v = e->i; return V3_OK; }
static V3CALL tresult al_setFloat(void *s, const char *id, double v)
{ attr_entry *e = attr_find(s, id, 1); if (!e) return V3_NOTIMPL; e->kind = 2; e->f = v; return V3_OK; }
static V3CALL tresult al_getFloat(void *s, const char *id, double *v)
{ attr_entry *e = attr_find(s, id, 0); if (!e || !v) return V3_NOTIMPL; *v = e->f; return V3_OK; }
static V3CALL tresult al_setString(void *s, const char *id, const int16_t *v)
{
    attr_entry *e = attr_find(s, id, 1);
    int i = 0;
    if (!e) return V3_NOTIMPL;
    e->kind = 3;
    if (v) for (; v[i] && i < 255; i++) e->str[i] = v[i];
    e->str[i] = 0;
    return V3_OK;
}
static V3CALL tresult al_getString(void *s, const char *id, int16_t *v, uint32_t bytes)
{
    attr_entry *e = attr_find(s, id, 0);
    uint32_t n = bytes / 2, i;
    if (!e || !v || !n) return V3_NOTIMPL;
    for (i = 0; i + 1 < n && e->str[i]; i++) v[i] = e->str[i];
    v[i] = 0;
    return V3_OK;
}
static V3CALL tresult al_setBinary(void *s, const char *id, const void *d, uint32_t sz)
{
    attr_entry *e = attr_find(s, id, 1);
    if (!e) return V3_NOTIMPL;
    free(e->blob);
    e->blob = malloc(sz ? sz : 1);
    if (e->blob && d) memcpy(e->blob, d, sz);
    e->blobsz = e->blob ? sz : 0;
    e->kind = 4;
    return V3_OK;
}
static V3CALL tresult al_getBinary(void *s, const char *id, const void **d, uint32_t *sz)
{
    attr_entry *e = attr_find(s, id, 0);
    if (!e || !d || !sz) return V3_NOTIMPL;
    *d = e->blob; *sz = e->blobsz;
    return V3_OK;
}
struct IAttributeListVtbl {
    FUNKNOWN_SLOTS;
    tresult (V3CALL *setInt)(void *, const char *, int64_t);
    tresult (V3CALL *getInt)(void *, const char *, int64_t *);
    tresult (V3CALL *setFloat)(void *, const char *, double);
    tresult (V3CALL *getFloat)(void *, const char *, double *);
    tresult (V3CALL *setString)(void *, const char *, const int16_t *);
    tresult (V3CALL *getString)(void *, const char *, int16_t *, uint32_t);
    tresult (V3CALL *setBinary)(void *, const char *, const void *, uint32_t);
    tresult (V3CALL *getBinary)(void *, const char *, const void **, uint32_t *);
};
static const IAttributeListVtbl g_al_vt = {
    al_qi, al_addref, al_release,
    al_setInt, al_getInt, al_setFloat, al_getFloat,
    al_setString, al_getString, al_setBinary, al_getBinary
};

static AttrList *attrlist_new(void)
{
    AttrList *a = calloc(1, sizeof *a);
    if (a) { a->vt = &g_al_vt; a->refs = 1; }
    return a;
}

typedef struct IMessageVtbl IMessageVtbl;
typedef struct {
    const IMessageVtbl *vt;
    int32_t   refs;
    char      id[128];
    AttrList *attrs;
} Message;

static V3CALL tresult msg_qi(void *s, const TUID iid, void **o)
{
    if (!memcmp(iid, IID_IMessage, 16) || !memcmp(iid, IID_FUnknown, 16))
        { *o = s; return V3_OK; }
    *o = NULL; return V3_NOIFACE;
}
static V3CALL uint32_t msg_addref(void *s) { return (uint32_t)(++((Message *)s)->refs); }
static V3CALL uint32_t msg_release(void *s)
{
    Message *m = s;
    if (--m->refs > 0) return (uint32_t)m->refs;
    if (m->attrs) al_release(m->attrs);
    free(m);
    return 0;
}
static V3CALL const char *msg_getMessageID(void *s) { return ((Message *)s)->id; }
static V3CALL void msg_setMessageID(void *s, const char *id)
{ snprintf(((Message *)s)->id, sizeof ((Message *)s)->id, "%s", id ? id : ""); }
static V3CALL void *msg_getAttributes(void *s) { return ((Message *)s)->attrs; }

struct IMessageVtbl {
    FUNKNOWN_SLOTS;
    const char *(V3CALL *getMessageID)(void *);
    void        (V3CALL *setMessageID)(void *, const char *);
    void       *(V3CALL *getAttributes)(void *);
};
static const IMessageVtbl g_msg_vt = {
    msg_qi, msg_addref, msg_release, msg_getMessageID, msg_setMessageID, msg_getAttributes
};

static V3CALL tresult ha_create(void *s, TUID cid, TUID iid, void **obj)
{
    (void)s; (void)cid;
    if (!obj) return V3_NOTIMPL;
    *obj = NULL;
    if (!memcmp(iid, IID_IMessage, 16)) {
        Message *m = calloc(1, sizeof *m);
        if (!m) return V3_NOTIMPL;
        m->vt = &g_msg_vt;
        m->refs = 1;
        m->attrs = attrlist_new();
        *obj = m;
        VLOG("vst3: host created an IMessage\n");
        return V3_OK;
    }
    if (!memcmp(iid, IID_IAttributeList, 16)) {
        *obj = attrlist_new();
        VLOG("vst3: host created an IAttributeList\n");
        return V3_OK;
    }
    VLOG("vst3: IHostApplication::createInstance for an unknown interface\n");
    return V3_NOTIMPL;
}
static const IHostApplicationVtbl g_ha_vt = { ha_qi, ha_ref, ha_ref, ha_getname, ha_create };

/* Entry points, forward declared: v3_open's failure path calls v3_close, and
 * v3_set_program writes through v3_set_param. */
v3host *V3N(v3_open)(const char *, double, int, char *, int);
void    V3N(v3_close)(v3host *);
void    V3N(v3_set_param)(v3host *, int, float);

/* ------------------------------------------------------------------- host */

#define EVQ 512
enum { Q_MIDI = 1, Q_PARAM };
typedef struct { unsigned char type, a, b, c; float v; } qev_t;

struct v3host {
    void            *dl;      /* ELF path */
    pe_module        pe;      /* PE path  */
    macho           *img;     /* Mach-O path (macOS bundles) */
    IPluginFactory  *factory;
    IComponent      *comp;
    IAudioProcessor *proc;
    IEditController *ctrl;
    IMidiMapping    *midimap;      /* NULL when the plug-in offers none */
    unsigned         in_mask;      /* input channels fed; 0 = all */
    int              ctrl_is_comp;  /* single-component plugin: same object */

    Handler   handler;
    HostApp   hostapp;
    PlugFrame frame;
    IPlugView *view;
    int        view_w, view_h;
    int        view_attached;   /* removed() must be called exactly once */

    char   name[64], vendor[64];
    int    nparams, nin, nout, synth;
    double sr;
    int    bs;

    /* Scratch buffers. VST3 plugins commonly expose several buses (Surge XT
     * has three outputs), and process() must describe all of them, so every
     * bus gets its own channel array even though only bus 0 is played. */
#define MAXBUS 8
#define MAXCH  16
    int             nInBus, nOutBus;
    int             inCh[MAXBUS], outCh[MAXBUS];
    float          *inbuf[MAXBUS][MAXCH], *outbuf[MAXBUS][MAXCH];
    float          *inptr[MAXBUS][MAXCH], *outptr[MAXBUS][MAXCH];
    AudioBusBuffers inbus[MAXBUS], outbus[MAXBUS];
    int             cap;

    uint32_t *pid;          /* parameter index -> VST3 ParamID */

    /* Programs live behind IUnitInfo, and switching one means writing the
     * parameter the plugin flagged kIsProgramChange -- there is no dedicated
     * "set program" call in VST3. */
    IUnitInfo *units;
    int32_t    plist_id, nprograms;
    int        prog_param;      /* index into pid[], or -1 */
    int32_t    prog_steps;
    int        program;

    qev_t             q[EVQ];
    _Atomic unsigned  head, tail;
};

static void u16_to_ascii(const int16_t *w, char *out, int n)
{
    int i = 0;
    if (!w) { if (n) out[0] = 0; return; }
    for (; w[i] && i < n - 1; i++) out[i] = (w[i] > 0 && w[i] < 128) ? (char)w[i] : '?';
    out[i] = 0;
}

static void qpush(v3host *h, unsigned char t, unsigned char a, unsigned char b,
                  unsigned char c, float v)
{
    unsigned hd = atomic_load_explicit(&h->head, memory_order_relaxed);
    unsigned tl = atomic_load_explicit(&h->tail, memory_order_acquire);
    if (((hd + 1) & (EVQ - 1)) == (tl & (EVQ - 1))) return;
    h->q[hd & (EVQ - 1)] = (qev_t){ t, a, b, c, v };
    atomic_store_explicit(&h->head, hd + 1, memory_order_release);
}

/* A .vst3 bundle is a directory; the code lives under Contents/<arch>/. */
/* A macOS .vst3 is a bundle whose binary sits in Contents/MacOS with no
 * extension at all, so the name cannot be the test. Recognise the layout, and
 * hand the *bundle* over rather than the binary: the Mach-O loader resolves
 * Contents/MacOS itself, reads Info.plist for the bundle identifier, and finds
 * the plugin's own artwork relative to the bundle. Also answers for a bare
 * Mach-O file, which is what --as mac-vst3 on a loose binary gives.
 *
 * Sets *out to what macho_open should be given. */
static int macos_bundle(const char *path, char *out, size_t n)
{
    struct stat st;
    char p[1024];

    if (stat(path, &st)) return 0;
    if (S_ISDIR(st.st_mode)) {
        snprintf(p, sizeof p, "%s/Contents/MacOS", path);
        if (stat(p, &st) || !S_ISDIR(st.st_mode)) return 0;
        snprintf(out, n, "%s", path);
        return 1;
    }
    {   unsigned char m[4] = { 0, 0, 0, 0 };
        FILE *f = fopen(path, "rb");
        size_t got = f ? fread(m, 1, 4, f) : 0;
        if (f) fclose(f);
        if (got < 4) return 0;
        /* MH_MAGIC_64 little-endian, or either width of universal binary. */
        if (!(m[0] == 0xcf && m[1] == 0xfa && m[2] == 0xed && m[3] == 0xfe) &&
            !(m[0] == 0xca && m[1] == 0xfe && m[2] == 0xba &&
              (m[3] == 0xbe || m[3] == 0xbf)))
            return 0;
    }
    snprintf(out, n, "%s", path);
    return 1;
}

static int find_binary(const char *path, char *out, size_t n)
{
    struct stat st;
    static const char *arch[] = { "x86_64-linux", "x86_64-win", NULL };
    int i;

    if (stat(path, &st)) return -1;
    if (S_ISREG(st.st_mode)) { snprintf(out, n, "%s", path); return 0; }

    for (i = 0; arch[i]; i++) {
        char dir[1024];
        DIR *d;
        struct dirent *e;
        snprintf(dir, sizeof dir, "%s/Contents/%s", path, arch[i]);
        if (!(d = opendir(dir))) continue;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 3 && (!strcmp(e->d_name + l - 3, ".so") ||
                          (l > 5 && !strcmp(e->d_name + l - 5, ".vst3")))) {
                snprintf(out, n, "%s/%s", dir, e->d_name);
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }
    return -1;
}

static void free_bufs(v3host *h)
{
    int b, c;
    for (b = 0; b < MAXBUS; b++)
        for (c = 0; c < MAXCH; c++) {
            free(h->inbuf[b][c]);  h->inbuf[b][c]  = NULL;
            free(h->outbuf[b][c]); h->outbuf[b][c] = NULL;
        }
    h->cap = 0;
}

static int alloc_bufs(v3host *h, int frames)
{
    int b, c;
    if (frames <= h->cap) return 0;
    free_bufs(h);
    for (b = 0; b < h->nInBus; b++)
        for (c = 0; c < h->inCh[b]; c++)
            if (!(h->inbuf[b][c] = calloc((size_t)frames, sizeof ***h->inbuf))) return -1;
    for (b = 0; b < h->nOutBus; b++)
        for (c = 0; c < h->outCh[b]; c++)
            if (!(h->outbuf[b][c] = calloc((size_t)frames, sizeof ***h->outbuf))) return -1;
    h->cap = frames;
    return 0;
}

/* Channel-count -> VST3 SpeakerArrangement bitmask for the common layouts. */
static uint64_t arrangement_for(int ch)
{
    switch (ch) {
    case 0:  return 0;
    case 1:  return 0x1;                    /* kMono   */
    case 2:  return 0x3;                    /* kStereo */
    case 4:  return 0xF;
    case 6:  return 0x3F;
    default: return (ch >= 64) ? ~0ull : ((1ull << ch) - 1);
    }
}

v3host *V3N(v3_open)(const char *path, double samplerate, int blocksize,
                char *err, int errlen)
{
    char bin[1024];
    v3host *h;
    IPluginFactory *(V3CALL *getfac)(void);
#if !V3_MSABI
    int (*modentry)(void *);
#endif
    PFactoryInfo fi;
    int is_macho = 0;
    int32_t nclasses, i, chosen = -1;
    PClassInfo ci;
    TUID ctrlcid;
    double t0 = v3_now(), tLoad = t0, tCreate = t0, tInit = t0;

#define FAIL(msg) do { snprintf(err, errlen, "%s", msg); V3N(v3_close)(h); return NULL; } while (0)

    is_macho = macos_bundle(path, bin, sizeof bin);
    if (!is_macho && find_binary(path, bin, sizeof bin)) {
        snprintf(err, errlen, "no VST3 binary inside %s", path);
        return NULL;
    }
    if (!(h = calloc(1, sizeof *h))) { snprintf(err, errlen, "oom"); return NULL; }
    h->sr = samplerate;
    h->bs = blocksize;
    h->handler.vt = &g_ch_vt;
    h->handler.unith.vt = &g_uh_vt;
    h->handler.ch2.vt = &g_ch2_vt;
    h->hostapp.vt = &g_ha_vt;
    h->hostapp.runloop.vt = &g_rl_vt;
    h->frame.vt = &g_pf_vt;
    h->frame.runloop.vt = &g_rl_vt;

#if V3_MSABI
    /* Windows VST3: the same PE loader the VST2 path uses, then the module's
     * own InitDll before the factory is asked for. */
    if (pehost_thread_init()) { snprintf(err, errlen, "cannot install TEB"); free(h); return NULL; }
    if (pe_module_load(bin, &h->pe, err, errlen)) { free(h); return NULL; }
    {
        V3CALL int32_t (*initdll)(void) =
            (V3CALL int32_t (*)(void))pe_module_export(&h->pe, "InitDll");
        if (initdll) initdll();
    }
    getfac = (IPluginFactory *(V3CALL *)(void))pe_module_export(&h->pe, "GetPluginFactory");
    if (!getfac) FAIL("no GetPluginFactory export");
#else
    if (is_macho) {
        /* macOS x86-64 is System V, so this is the same code that drives a
         * native Linux bundle -- only the loader underneath differs. What VST3
         * calls ModuleEntry on Linux is bundleEntry here, and it takes the
         * bundle rather than a dlopen handle. */
        int (*bentry)(void *);
        if (!(h->img = macho_open(bin))) {
            snprintf(err, errlen, "%s", macho_last_error());
            free(h);
            return NULL;
        }
        macho_run_init(h->img);
        if ((bentry = (int (*)(void *))macho_symbol(h->img, "bundleEntry")))
            bentry(h->img);
        if (!(getfac = (IPluginFactory *(*)(void))macho_symbol(h->img, "GetPluginFactory")))
            FAIL("no GetPluginFactory export");
    } else {
    if (!(h->dl = dlopen(bin, RTLD_NOW | RTLD_LOCAL))) {
        snprintf(err, errlen, "dlopen: %s", dlerror());
        free(h);
        return NULL;
    }
    /* Linux VST3 requires ModuleEntry before the factory is usable. */
    if ((modentry = (int (*)(void *))dlsym(h->dl, "ModuleEntry"))) modentry(h->dl);

    if (!(getfac = (IPluginFactory *(*)(void))dlsym(h->dl, "GetPluginFactory")))
        FAIL("no GetPluginFactory export");
    }
#endif
    if (!(h->factory = getfac())) FAIL("GetPluginFactory returned NULL");

    tLoad = v3_now();
    memset(&fi, 0, sizeof fi);
    if (h->factory->vt->getFactoryInfo(h->factory, &fi) == V3_OK)
        snprintf(h->vendor, sizeof h->vendor, "%s", fi.vendor);

    /* Pick the first audio module class. */
    nclasses = h->factory->vt->countClasses(h->factory);
    for (i = 0; i < nclasses; i++) {
        memset(&ci, 0, sizeof ci);
        if (h->factory->vt->getClassInfo(h->factory, i, &ci) != V3_OK) continue;
        if (!strcmp(ci.category, "Audio Module Class")) { chosen = i; break; }
    }
    VLOG("vst3: %d class(es), audio module = %d\n", nclasses, chosen);
    if (chosen < 0) FAIL("no Audio Module Class in factory");
    h->factory->vt->getClassInfo(h->factory, chosen, &ci);
    snprintf(h->name, sizeof h->name, "%s", ci.name);

    if (h->factory->vt->createInstance(h->factory, ci.cid, IID_IComponent,
                                       (void **)&h->comp) != V3_OK || !h->comp)
        FAIL("createInstance(IComponent) failed");
    tCreate = v3_now();

    {
        tresult r = h->comp->vt->initialize(h->comp, &h->hostapp);
        VLOG("vst3: IComponent::initialize -> %d\n", r);
        if (r != V3_OK) FAIL("IComponent::initialize failed");
    }
    tInit = v3_now();

    if (h->comp->vt->queryInterface(h->comp, IID_IAudioProcessor,
                                    (void **)&h->proc) != V3_OK || !h->proc)
        FAIL("no IAudioProcessor");

    /* The controller is usually a separate class; fall back to the component
     * implementing it directly (the "single component effect" pattern). */
    {
        tresult rc = h->comp->vt->getControllerClassId(h->comp, ctrlcid);
        VLOG("vst3: getControllerClassId -> %d\n", rc);
        if (rc == V3_OK) {
            tresult r = h->factory->vt->createInstance(h->factory, ctrlcid,
                                                       IID_IEditController, (void **)&h->ctrl);
            VLOG("vst3: createInstance(IEditController) -> %d ctrl=%p\n",
                    r, (void *)h->ctrl);
            if (r != V3_OK) h->ctrl = NULL;
            if (h->ctrl) {
                r = h->ctrl->vt->initialize(h->ctrl, &h->hostapp);
                VLOG("vst3: IEditController::initialize -> %d\n", r);
            }
        }
    }
    if (!h->ctrl) {
        tresult r = h->comp->vt->queryInterface(h->comp, IID_IEditController, (void **)&h->ctrl);
        fprintf(stderr, "vst3: component as controller -> %d ctrl=%p\n", r, (void *)h->ctrl);
    }
    if (h->ctrl) h->ctrl->vt->setComponentHandler(h->ctrl, &h->handler);

    /* Introduce component and controller to each other. */
    if (h->ctrl) {
        IConnectionPoint *cpComp = NULL, *cpCtrl = NULL;
        h->comp->vt->queryInterface(h->comp, IID_IConnectionPoint, (void **)&cpComp);
        h->ctrl->vt->queryInterface(h->ctrl, IID_IConnectionPoint, (void **)&cpCtrl);
        if (cpComp && cpCtrl) {
            tresult r1 = cpComp->vt->connect(cpComp, cpCtrl);
            tresult r2 = cpCtrl->vt->connect(cpCtrl, cpComp);
            VLOG("vst3: connect component<->controller -> %d / %d\n", r1, r2);
        } else {
            VLOG("vst3: no IConnectionPoint (comp=%p ctrl=%p)\n",
                    (void *)cpComp, (void *)cpCtrl);
        }
    }

    /* Bus layout: enumerate every bus, activate them all, and hand the plugin
     * an arrangement for each. Declaring only the first output bus is what made
     * setBusArrangements fail on plugins with aux outputs. */
    {
        uint64_t inArr[MAXBUS], outArr[MAXBUS];
        int32_t  nEventIn;
        int      b;

        h->nInBus  = h->comp->vt->getBusCount(h->comp, MT_AUDIO, BD_INPUT);
        h->nOutBus = h->comp->vt->getBusCount(h->comp, MT_AUDIO, BD_OUTPUT);
        nEventIn   = h->comp->vt->getBusCount(h->comp, MT_EVENT, BD_INPUT);
        if (h->nInBus  > MAXBUS) h->nInBus  = MAXBUS;
        if (h->nOutBus > MAXBUS) h->nOutBus = MAXBUS;
        if (h->nOutBus < 1) { h->nOutBus = 1; h->outCh[0] = 2; }

        /* Channel counts first, then the arrangement, and only then activate.
         *
         * That order is the SDK's and it matters: setBusArrangements is allowed
         * to rebuild the bus list, which discards whatever activateBus had
         * already set. Activating first made every Full Bucket VST3 render
         * silence -- the call returned kResultOk, and the output bus it left
         * behind was simply inactive. (The bug was invisible for as long as
         * setBusArrangements sat inside a log call and never ran at all.) */
        for (b = 0; b < h->nInBus; b++) {
            BusInfo bi;
            memset(&bi, 0, sizeof bi);
            h->inCh[b] = 2;
            if (h->comp->vt->getBusInfo(h->comp, MT_AUDIO, BD_INPUT, b, &bi) == V3_OK)
                h->inCh[b] = bi.channelCount > MAXCH ? MAXCH : bi.channelCount;
            inArr[b] = arrangement_for(h->inCh[b]);
        }
        for (b = 0; b < h->nOutBus; b++) {
            BusInfo bi;
            memset(&bi, 0, sizeof bi);
            if (!h->outCh[b]) h->outCh[b] = 2;
            if (h->comp->vt->getBusInfo(h->comp, MT_AUDIO, BD_OUTPUT, b, &bi) == V3_OK)
                h->outCh[b] = bi.channelCount > MAXCH ? MAXCH : bi.channelCount;
            outArr[b] = arrangement_for(h->outCh[b]);
        }
        h->synth = nEventIn > 0;
        h->nin  = h->nInBus  ? h->inCh[0]  : 0;
        h->nout = h->outCh[0];

        VLOG("vst3: audio buses in %d out %d, event in %d; bus0 ch in %d out %d\n",
                h->nInBus, h->nOutBus, nEventIn, h->nin, h->nout);
        {
            tresult r = h->proc->vt->setBusArrangements(h->proc,
                        h->nInBus ? inArr : NULL, h->nInBus, outArr, h->nOutBus);
            VLOG("vst3: setBusArrangements -> %d\n", (int)r);
        }
        for (b = 0; b < h->nInBus; b++)
            h->comp->vt->activateBus(h->comp, MT_AUDIO, BD_INPUT, b, 1);
        for (b = 0; b < h->nOutBus; b++)
            h->comp->vt->activateBus(h->comp, MT_AUDIO, BD_OUTPUT, b, 1);
        for (b = 0; b < nEventIn; b++)
            h->comp->vt->activateBus(h->comp, MT_EVENT, BD_INPUT, b, 1);
    }

    {
        ProcessSetup su;
        su.processMode = 0;              /* kRealtime */
        su.symbolicSampleSize = 0;       /* kSample32 */
        su.maxSamplesPerBlock = blocksize;
        su.sampleRate = samplerate;
        if (h->proc->vt->setupProcessing(h->proc, &su) != V3_OK)
            FAIL("setupProcessing rejected 32-bit realtime");
    }

    if (alloc_bufs(h, blocksize)) FAIL("buffer allocation failed");

    /* Called for their effect, then logged -- never from inside the log call.
     * VLOG only evaluates its arguments when logging is enabled, so with these
     * two as arguments the plugin was activated only when PELOAD_VERBOSE was
     * set. Unactivated, JUCE never runs prepareToPlay, and OB-Xf's MIDI handler
     * stayed unconstructed until the first note dereferenced it. */
    {
        tresult act = h->comp->vt->setActive(h->comp, 1);
        tresult pro = h->proc->vt->setProcessing(h->proc, 1);
        VLOG("vst3: setActive -> %d, setProcessing -> %d\n", (int)act, (int)pro);
    }

    /* Validate the controller vtable before trusting counts from it: if slot 0
     * (queryInterface) behaves, the offsets after it are almost certainly right,
     * and a zero parameter count means something else. */
    if (h->ctrl) {
        void *self = NULL;
        tresult r = h->ctrl->vt->queryInterface(h->ctrl, IID_IEditController, &self);
        VLOG("vst3: ctrl->queryInterface(IEditController) -> %d self=%p (ctrl=%p)\n",
                r, self, (void *)h->ctrl);
        {
            ParameterInfo pi;
            memset(&pi, 0, sizeof pi);
            r = h->ctrl->vt->getParameterInfo(h->ctrl, 0, &pi);
            VLOG("vst3: getParameterInfo(0) -> %d id=%u step=%d\n",
                    r, pi.id, pi.stepCount);
        }
        /* No setComponentState here. It takes an IBStream, and NULL is not one;
         * it only ever sat in this probe because the probe never ran -- it was an
         * argument to a log call that is compiled out when logging is off. Once
         * it did run, every Full Bucket VST3 went silent, which is a fair
         * response to being told to load a null patch. */
        VLOG("vst3: getParameterCount -> %d\n",
             (int)h->ctrl->vt->getParameterCount(h->ctrl));
    }

    /* Cache the parameter ID list: VST3 addresses parameters by ID, not index. */
    if (h->ctrl) {
        h->nparams = h->ctrl->vt->getParameterCount(h->ctrl);
        VLOG("vst3: getParameterCount -> %d\n", h->nparams);
        if (h->nparams > 0 && (h->pid = calloc((size_t)h->nparams, sizeof *h->pid))) {
            for (i = 0; i < h->nparams; i++) {
                ParameterInfo pi;
                memset(&pi, 0, sizeof pi);
                if (h->ctrl->vt->getParameterInfo(h->ctrl, i, &pi) == V3_OK)
                    h->pid[i] = pi.id;
            }
        } else if (h->nparams > 0) {
            h->nparams = 0;
        }
    }
    if (v3_verbose())
        fprintf(stderr, "vst3: load %.0f ms, createInstance %.0f ms, initialize %.0f ms, "
                        "buses+params %.0f ms\n",
                (tLoad - t0) * 1e3, (tCreate - tLoad) * 1e3,
                (tInit - tCreate) * 1e3, (v3_now() - tInit) * 1e3);
    fprintf(stderr, "vst3: %s -- %s, %d param(s), in %d out %d%s\n",
            h->name, h->vendor, h->nparams, h->nin, h->nout,
            h->synth ? ", synth" : "");
    /* Which parameter the wheel and the controllers drive, if the plug-in says.
     *
     * VST3 has no event for a pitch bend or a control change: they are
     * parameters, and IMidiMapping is the only thing that knows which. Without
     * asking, a wheel or a mod wheel simply does nothing -- which is what every
     * VST3 here did, while the same plug-in's VST2 build bent perfectly. */
    if (h->ctrl &&
        h->ctrl->vt->queryInterface(h->ctrl, IID_IMidiMapping,
                                    (void **)&h->midimap) != V3_OK)
        h->midimap = NULL;
    VLOG("  midi mapping: %s\n", h->midimap ? "yes" : "not offered");

    /* Program list, if the plugin publishes one. */
    h->prog_param = -1;
    if (h->ctrl &&
        h->ctrl->vt->queryInterface(h->ctrl, IID_IUnitInfo, (void **)&h->units) == V3_OK &&
        h->units) {
        if (h->units->vt->getProgramListCount(h->units) > 0) {
            ProgramListInfo pl;
            memset(&pl, 0, sizeof pl);
            if (h->units->vt->getProgramListInfo(h->units, 0, &pl) == V3_OK) {
                h->plist_id  = pl.id;
                h->nprograms = pl.programCount;
            }
        }
        /* Find the parameter that acts as the program selector. */
        for (i = 0; i < h->nparams && h->prog_param < 0; i++) {
            ParameterInfo pi;
            memset(&pi, 0, sizeof pi);
            if (h->ctrl->vt->getParameterInfo(h->ctrl, i, &pi) != V3_OK) continue;
            if (pi.flags & 0x8000) {          /* kIsProgramChange */
                h->prog_param = i;
                h->prog_steps = pi.stepCount;
            }
        }
        VLOG("vst3: %d program(s) in list %d, selector param %d (steps %d)\n",
             h->nprograms, h->plist_id, h->prog_param, h->prog_steps);
    }

    return h;
#undef FAIL
}

void V3N(v3_close)(v3host *h)
{
    if (!h) return;

    if (h->view) {
        if (h->view_attached) { h->view->vt->removed(h->view); h->view_attached = 0; }
        h->view->vt->release(h->view);
        h->view = NULL;
    }

    /* Silence it first -- this part is required and safe. */
    if (h->proc) h->proc->vt->setProcessing(h->proc, 0);
    if (h->comp) h->comp->vt->setActive(h->comp, 0);

    /* Stop there by default. Releasing the objects runs the plugin's own
     * destructor chain, and some large plugins (Cardinal) fault deep inside
     * theirs, taking the host with them. Deactivated-but-not-freed leaks one
     * instance per load, which is a far better trade than crashing on every
     * plugin switch. Set PELOAD_V3_TEARDOWN=1 to do the full release. */
    {
        const char *e = getenv("PELOAD_V3_TEARDOWN");
        if (e && *e != '0') {
            if (h->ctrl) {
                if (!h->ctrl_is_comp) h->ctrl->vt->terminate(h->ctrl);
                h->ctrl->vt->release(h->ctrl);
            }
            if (h->proc) h->proc->vt->release(h->proc);
            if (h->comp) { h->comp->vt->terminate(h->comp); h->comp->vt->release(h->comp); }
        }
    }

    free_bufs(h);
    free(h->pid);
#if V3_MSABI
    pe_module_unload(&h->pe);
#else
    /* ModuleEntry has a partner, and not calling it is what makes Cardinal fault
     * at process exit.
     *
     * The Linux VST3 contract is ModuleEntry -> use -> ModuleExit. A module
     * never told to exit still owns everything it built, so its globals are torn
     * down by the C++ runtime at process exit instead -- in link order, from an
     * atexit handler, against state the host dismantled long before. Cardinal's
     * Rack engine is the one here that does not survive that: it asserts on its
     * own module list and then dereferences what it just failed to find. Told to
     * exit while the host is still standing, it unwinds in the right order.
     *
     * The factory goes first, because ModuleExit releases the module's own
     * reference and the one taken by GetPluginFactory is ours to drop. */
    if (h->factory) { h->factory->vt->release(h->factory); h->factory = NULL; }
    if (h->dl) {
        int (*modexit)(void) = (int (*)(void))dlsym(h->dl, "ModuleExit");
        if (modexit) modexit();
    }
    if (h->img) {
        /* bundleExit is bundleEntry's partner, for the same reason ModuleExit is
         * ModuleEntry's. Unlike the ELF path the image really is unmapped after
         * it, so a module that skipped its own teardown would be torn down by
         * nothing at all. */
        int (*bexit)(void) = (int (*)(void))macho_symbol(h->img, "bundleExit");
        if (bexit) bexit();
        macho_close(h->img);
        h->img = NULL;
    }
    /* Still no dlclose. Measured: it reclaimed nothing (the plugins hold their
     * own references) and crashed Odin2 and CardinalSynth outright. */
#endif
    free(h);
}

const char *V3N(v3_name)(const v3host *h)   { return h ? h->name : ""; }
const char *V3N(v3_vendor)(const v3host *h) { return h ? h->vendor : ""; }
int V3N(v3_num_params)(const v3host *h)  { return h ? h->nparams : 0; }
int V3N(v3_num_inputs)(const v3host *h)  { return h ? h->nin : 0; }
int V3N(v3_num_outputs)(const v3host *h) { return h ? h->nout : 0; }
int V3N(v3_is_synth)(const v3host *h)    { return h ? h->synth : 0; }

/* Per-plugin quirk list. FB-02's VST3 controller accepts a program-change write
 * and then its processor faults on the very next block -- verified by bisecting
 * the write. Every other plugin here needs that write for programs to work at
 * all, so the narrow fix is to opt this one out rather than give up program
 * changes everywhere. Its VST2 build handles all 336 programs correctly. */
static int programs_unsafe(const v3host *h)
{
    if (!h) return 0;
    if (!strcmp(h->name, "FB-02")) {
        static int said = 0;
        if (!said) {
            said = 1;
            fprintf(stderr, "vst3: %s -- program switching disabled (its processor "
                            "faults after a program write); use the VST2 build\n", h->name);
        }
        return 1;
    }
    return 0;
}

int V3N(v3_num_programs)(const v3host *h)
{ return (h && !programs_unsafe(h)) ? h->nprograms : 0; }
int V3N(v3_get_program)(const v3host *h) { return h ? h->program : 0; }

void V3N(v3_program_name)(v3host *h, int i, char *b, int n)
{
    int16_t nm[128];
    if (n) b[0] = 0;
    if (!h || !h->units || i < 0 || i >= h->nprograms) return;
    memset(nm, 0, sizeof nm);
    if (h->units->vt->getProgramName(h->units, h->plist_id, i, nm) == V3_OK)
        u16_to_ascii(nm, b, n);
    if (n && !b[0]) snprintf(b, (size_t)n, "Program %d", i);
}

void V3N(v3_set_program)(v3host *h, int i)
{
    if (!h || i < 0 || i >= h->nprograms || programs_unsafe(h)) return;
    h->program = i;
    /* VST3 has no setProgram: write the kIsProgramChange parameter instead, so
     * the change reaches both the controller and the processor. */
    if (h->prog_param >= 0) {
        double v = h->prog_steps > 0 ? (double)i / (double)h->prog_steps : 0.0;
        V3N(v3_set_param)(h, h->prog_param, (float)v);
    }
}

void V3N(v3_param_name)(v3host *h, int i, char *buf, int n)
{
    ParameterInfo pi;
    if (n) buf[0] = 0;
    if (!h || !h->ctrl || i < 0 || i >= h->nparams) return;
    memset(&pi, 0, sizeof pi);
    if (h->ctrl->vt->getParameterInfo(h->ctrl, i, &pi) == V3_OK)
        u16_to_ascii(pi.title, buf, n);
}

void V3N(v3_param_display)(v3host *h, int i, char *buf, int n)
{
    int16_t s[128];
    if (n) buf[0] = 0;
    if (!h || !h->ctrl || i < 0 || i >= h->nparams) return;
    memset(s, 0, sizeof s);
    if (h->ctrl->vt->getParamStringByValue(
            h->ctrl, h->pid[i],
            h->ctrl->vt->getParamNormalized(h->ctrl, h->pid[i]), s) == V3_OK)
        u16_to_ascii(s, buf, n);
}

float V3N(v3_get_param)(v3host *h, int i)
{
    if (!h || !h->ctrl || i < 0 || i >= h->nparams) return 0.0f;
    return (float)h->ctrl->vt->getParamNormalized(h->ctrl, h->pid[i]);
}

void V3N(v3_set_param)(v3host *h, int i, float v)
{
    if (!h || i < 0 || i >= h->nparams) return;
    /* Keep the controller in step for the display, and queue the change so the
     * processor hears it too -- in VST3 those are two separate objects. */
    /* The controller copy is not cosmetic: for the program-change parameter it
     * is what actually swaps the patch -- the processor alone ignores it. */
    if (h->ctrl)
        h->ctrl->vt->setParamNormalized(h->ctrl, h->pid[i], v);

    /* Ordinary parameters also go to the processor, which is what changes the
     * audio. The program-change parameter deliberately does not: the controller
     * has already swapped the patch, and FB-02's processor faults on the next
     * block if it is handed the program parameter as well. */
    if (i != h->prog_param)
        qpush(h, Q_PARAM, (unsigned char)(i & 0xff), (unsigned char)(i >> 8), 0, v);
}

void V3N(v3_set_input_mask)(v3host *h, unsigned mask) { if (h) h->in_mask = mask; }

void V3N(v3_midi)(v3host *h, int status, int d1, int d2)
{
    if (h) qpush(h, Q_MIDI, (unsigned char)status,
                 (unsigned char)(d1 & 0x7f), (unsigned char)(d2 & 0x7f), 0.0f);
}

/* ------------------------------------------------------------- editor ---- */

/* Created lazily: a plugin that is never shown should not pay for its GUI. */
static IPlugView *view_of(v3host *h)
{
    if (!h || !h->ctrl) return NULL;
    if (h->view) return h->view;
    h->view = (IPlugView *)h->ctrl->vt->createView(h->ctrl, "editor");
    if (h->view) {
        ViewRect r = { 0, 0, 0, 0 };
        if (h->view->vt->getSize(h->view, &r) == V3_OK) {
            h->view_w = r.right - r.left;
            h->view_h = r.bottom - r.top;
        }
        h->view->vt->setFrame(h->view, &h->frame);
    }
    return h->view;
}

int V3N(v3_has_editor)(v3host *h)
{
    IPlugView *v = view_of(h);
    if (!v) return 0;
    return v->vt->isPlatformTypeSupported(v, PLATFORM_X11) == V3_OK;
}

void V3N(v3_editor_size)(v3host *h, int *w, int *hh)
{
    IPlugView *v = view_of(h);
    if (w) *w = 0;
    if (hh) *hh = 0;
    if (!v) return;
    if (w)  *w  = h->view_w;
    if (hh) *hh = h->view_h;
}

int V3N(v3_editor_can_resize)(v3host *h)
{
    IPlugView *v = view_of(h);
    return v ? (v->vt->canResize(v) == V3_OK) : 0;
}

int V3N(v3_editor_is_hwnd)(v3host *h)
{
    IPlugView *v = view_of(h);
    return v ? (v->vt->isPlatformTypeSupported(v, PLATFORM_HWND) == V3_OK) : 0;
}

/* Was this module loaded by the Mach-O loader? Callers use it to decide which
 * editor road to take before asking the plugin anything, because the answer
 * also decides where the pixels come from. */
int V3N(v3_is_macho)(v3host *h) { return (h && h->img) ? 1 : 0; }

int V3N(v3_editor_is_nsview)(v3host *h)
{
    IPlugView *v = view_of(h);
    return v ? (v->vt->isPlatformTypeSupported(v, PLATFORM_NSVIEW) == V3_OK) : 0;
}

/* Attach to a host-owned NSView. Unlike VST2 on macOS, which is handed NULL and
 * makes its own top-level view, VST3 has no such convention -- a plugin given no
 * parent refuses. The view is one of the runtime's own stand-ins; the plugin
 * adds its own as a subview and draws into the layer, which is what the software
 * Metal backend hands back. */
int V3N(v3_editor_attach_nsview)(v3host *h, void *nsview)
{
    IPlugView *v = view_of(h);
    tresult r;
    if (!v || !nsview) return -1;
    if (v->vt->isPlatformTypeSupported(v, PLATFORM_NSVIEW) != V3_OK) return -1;
    r = v->vt->attached(v, nsview, PLATFORM_NSVIEW);
    VLOG("vst3: attached(NSView %p) -> %d\n", nsview, r);
    if (r == V3_OK) h->view_attached = 1;
    return r == V3_OK ? 0 : -1;
}

int V3N(v3_editor_attach_hwnd)(v3host *h, void *hwnd)
{
    IPlugView *v = view_of(h);
    tresult r;
    if (!v) return -1;
    if (v->vt->isPlatformTypeSupported(v, PLATFORM_HWND) != V3_OK) return -1;
    r = v->vt->attached(v, hwnd, PLATFORM_HWND);
    VLOG("vst3: attached(HWND %p) -> %d\n", hwnd, r);
    if (r == V3_OK) h->view_attached = 1;
    return r == V3_OK ? 0 : -1;
}

/* Does this id name a live X11 window? Checked through dlopen so libX11 remains
 * an optional dependency of the loader. */
int V3N(v3_editor_attach)(v3host *h, unsigned long xid)
{
    /* A window id that is not an X11 window is worse than none: the plugin will
     * run Xlib against it and fault inside its own toolkit. Qt on Wayland hands
     * out surface handles that look like plausible ids, which is exactly how
     * that happened. */
    IPlugView *v = view_of(h);
    tresult r;
    if (!v) return -1;
    if (v->vt->isPlatformTypeSupported(v, PLATFORM_X11) != V3_OK) return -1;
    /* The window id is passed by value in a pointer-sized slot, not as a
     * pointer to it -- a detail that silently yields a blank editor if wrong. */
    r = v->vt->attached(v, (void *)(uintptr_t)xid, PLATFORM_X11);
    VLOG("vst3: attached(0x%lx) -> %d\n", xid, r);
    if (r == V3_OK) h->view_attached = 1;
    return r == V3_OK ? 0 : -1;
}

void V3N(v3_editor_detach)(v3host *h)
{
    /* Exactly once: DPF tears its UI down inside removed() and asserts if the
     * host calls it again, which is what a detach followed by close did. */
    if (h && h->view && h->view_attached) {
        h->view->vt->removed(h->view);
        h->view_attached = 0;
    }
}

void V3N(v3_editor_resized)(v3host *h, int w, int hp)
{
    ViewRect r;
    if (!h || !h->view) return;
    r.left = 0; r.top = 0; r.right = w; r.bottom = hp;
    h->view->vt->onSize(h->view, &r);
}

void V3N(v3_render)(v3host *h, const float *src, float *out, int frames)
{
    EventList     events;
    ParamChanges  changes;
    ParamChanges  outchanges;
    EventList     outevents;
    ProcessData   pd;
    unsigned      hd, tl;
    int           i;

    if (!h || !h->proc) { memset(out, 0, (size_t)frames * 2 * sizeof *out); return; }
    if (frames > h->cap && alloc_bufs(h, frames)) {
        memset(out, 0, (size_t)frames * 2 * sizeof *out); return;
    }

    events.vt = &g_el_vt;  events.n = 0;
    changes.vt = &g_pc_vt; changes.n = 0;
    /* Always supply the output collections. They are optional in the spec but
     * DPF-based plugins assert on a null outputParameterChanges, and a host that
     * never reads them still has to provide somewhere to write. */
    outchanges.vt = &g_pc_vt; outchanges.n = 0;
    outevents.vt = &g_el_vt;  outevents.n = 0;

    hd = atomic_load_explicit(&h->head, memory_order_acquire);
    tl = atomic_load_explicit(&h->tail, memory_order_relaxed);
    for (; tl != hd; tl++) {
        qev_t e = h->q[tl & (EVQ - 1)];
        if (e.type == Q_PARAM) {
            int idx = e.a | (e.b << 8);
            if (idx >= 0 && idx < h->nparams) {
                uint32_t id = h->pid[idx];
                int32_t at = 0;
                ParamQueue *q = pc_add(&changes, &id, &at);
                if (q) q->value = e.v;
            }
        } else if (e.type == Q_MIDI && events.n < MAX_EVENTS) {
            V3Event *ev = &events.ev[events.n];
            int cmd = e.a & 0xf0, ch = e.a & 0x0f;
            memset(ev, 0, sizeof *ev);
            ev->busIndex = 0;
            ev->sampleOffset = 0;
            if (cmd == 0x90 && e.c > 0) {
                ev->type = EV_NOTE_ON;
                ev->u.noteOn.channel  = (int16_t)ch;
                ev->u.noteOn.pitch    = (int16_t)e.b;
                ev->u.noteOn.velocity = e.c / 127.0f;
                ev->u.noteOn.noteId   = -1;
                events.n++;
            } else if (cmd == 0x80 || (cmd == 0x90 && e.c == 0)) {
                ev->type = EV_NOTE_OFF;
                ev->u.noteOff.channel  = (int16_t)ch;
                ev->u.noteOff.pitch    = (int16_t)e.b;
                ev->u.noteOff.velocity = 0.0f;
                ev->u.noteOff.noteId   = -1;
                events.n++;
            }
            /* A control change, a channel aftertouch or a pitch bend is a
             * parameter in VST3, not an event. Ask the plug-in which one and
             * queue a change on it; with no IMidiMapping there is nothing
             * sensible to send and it is dropped, as before. */
            else if (h->midimap &&
                     (cmd == 0xB0 || cmd == 0xD0 || cmd == 0xE0)) {
                int32_t ctrl = cmd == 0xE0 ? V3_CTRL_PITCHBEND
                             : cmd == 0xD0 ? V3_CTRL_AFTERTOUCH
                                           : (e.b & 0x7f);
                uint32_t id = 0;
                if (h->midimap->vt->getMidiControllerAssignment(
                        h->midimap, 0, (int16_t)ch, (int16_t)ctrl, &id) == V3_OK) {
                    int32_t at = 0;
                    ParamQueue *q = pc_add(&changes, &id, &at);
                    if (q) {
                        /* Bend is fourteen bits across both data bytes and sits
                         * at 0.5 when the wheel is centred; the other two are
                         * one byte. */
                        q->value = cmd == 0xE0
                            ? (double)((e.b & 0x7f) | ((e.c & 0x7f) << 7)) / 16383.0
                            : (double)(e.c & 0x7f) / 127.0;
                    }
                }
            }
        }
    }
    atomic_store_explicit(&h->tail, tl, memory_order_release);

    {
        int b, c;
        for (b = 0; b < h->nInBus; b++) {
            for (c = 0; c < h->inCh[b]; c++) {
                if (src && b == 0) {
                    int ic = c < 2 ? c : c % 2;
                    for (i = 0; i < frames; i++) h->inbuf[b][c][i] = src[2 * i + ic];
                } else {
                    memset(h->inbuf[b][c], 0, (size_t)frames * sizeof ***h->inbuf);
                }
                h->inptr[b][c] = h->inbuf[b][c];
            }
            h->inbus[b].numChannels = h->inCh[b];
            h->inbus[b].silenceFlags = 0;
            h->inbus[b].channelBuffers32 = h->inptr[b];
        }
        for (b = 0; b < h->nOutBus; b++) {
            for (c = 0; c < h->outCh[b]; c++) {
                memset(h->outbuf[b][c], 0, (size_t)frames * sizeof ***h->outbuf);
                h->outptr[b][c] = h->outbuf[b][c];
            }
            h->outbus[b].numChannels = h->outCh[b];
            h->outbus[b].silenceFlags = 0;
            h->outbus[b].channelBuffers32 = h->outptr[b];
        }
    }

    memset(&pd, 0, sizeof pd);
    pd.processMode = 0;
    pd.symbolicSampleSize = 0;
    pd.numSamples = frames;
    pd.numInputs  = h->nInBus;
    pd.numOutputs = h->nOutBus;
    pd.inputs  = h->nInBus ? h->inbus : NULL;
    pd.outputs = h->outbus;
    pd.inputParameterChanges  = changes.n ? (void *)&changes : NULL;
    pd.outputParameterChanges = (void *)&outchanges;
    pd.inputEvents  = events.n ? (void *)&events : NULL;
    pd.outputEvents = (void *)&outevents;

    h->proc->vt->process(h->proc, &pd);

    for (i = 0; i < frames; i++) {
        out[2 * i]     = h->outbuf[0][0][i];
        out[2 * i + 1] = h->outCh[0] >= 2 ? h->outbuf[0][1][i] : h->outbuf[0][0][i];
    }
}
