/* VST 2.4 ABI as it appears in a 32-bit Windows build.
 *
 * Same field order as the 64-bit form, but every pointer and intptr_t is 4
 * bytes, so the struct is exactly half the size. The function pointers are
 * __cdecl -- VSTCALLBACK on Win32 -- not stdcall, which is the one place the
 * two conventions in this loader differ from each other. */
#ifndef PELOAD_VST2_32_H
#define PELOAD_VST2_32_H

#include <stddef.h>
#include <string.h>

#define VSTCALL_ __attribute__((cdecl))

typedef struct AEffect32 AEffect32;

typedef intptr_t VSTCALL_ (*host_cb32)(AEffect32 *, int32_t, int32_t, intptr_t, void *, float);

struct AEffect32 {
    int32_t   magic;                 /* 'VstP' */
    intptr_t  VSTCALL_ (*dispatcher)(AEffect32 *, int32_t, int32_t, intptr_t, void *, float);
    void      VSTCALL_ (*process)(AEffect32 *, float **, float **, int32_t);
    void      VSTCALL_ (*setParameter)(AEffect32 *, int32_t, float);
    float     VSTCALL_ (*getParameter)(AEffect32 *, int32_t);
    int32_t   numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t  resvd1, resvd2;
    int32_t   initialDelay, realQualities, offQualities;
    float     ioRatio;
    void     *object, *user;
    int32_t   uniqueID, version;
    void      VSTCALL_ (*processReplacing)(AEffect32 *, float **, float **, int32_t);
    void      VSTCALL_ (*processDoubleReplacing)(AEffect32 *, double **, double **, int32_t);
    char      future[56];
};

enum {
    effOpen = 0, effClose, effSetProgram, effGetProgram, effSetProgramName,
    effGetProgramName, effGetParamLabel, effGetParamDisplay, effGetParamName,
    effGetVu, effSetSampleRate, effSetBlockSize, effMainsChanged,
    effEditGetRect, effEditOpen, effEditClose, effEditDraw, effEditMouse,
    effEditKey, effEditIdle, effEditTop, effEditSleep, effIdentify,
    effGetChunk, effSetChunk, effProcessEvents,
    effCanBeAutomated, effString2Parameter, effGetNumProgramCategories,
    effGetProgramNameIndexed, effCopyProgram, effConnectInput, effConnectOutput,
    effGetInputProperties, effGetOutputProperties, effGetPlugCategory,
    effGetCurrentPosition, effGetDestinationBuffer, effOfflineNotify,
    effOfflinePrepare, effOfflineRun, effProcessVarIo, effSetSpeakerArrangement,
    effSetBlockSizeAndSampleRate, effSetBypass, effGetEffectName,
    effGetErrorText, effGetVendorString, effGetProductString,
    effGetVendorVersion, effVendorSpecific, effCanDo, effGetTailSize
};

typedef struct {
    int32_t type, byteSize, deltaFrames, flags;
    int32_t noteLength, noteOffset;
    char    midiData[4];
    char    detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent32;

/* VstEvents declares events[2] but is really variable-length: the pointer array
 * has to be contiguous and numEvents long. A struct with `VstEvents32 ev;`
 * followed by a second pointer array does NOT work -- that array begins after
 * events[2], so events[0] and events[1] stay NULL and the plugin sees two empty
 * slots at the front. Allocate VSTEVENTS32_BYTES(n) and fill from events[0]. */
typedef struct {
    int32_t  numEvents;
    intptr_t reserved;
    void    *events[2];
} VstEvents32;

#define VSTEVENTS32_BYTES(n) \
    (offsetof(VstEvents32, events) + (size_t)(n) * sizeof(void *))
static inline void **vstevents32_array(void *buf)
{ return (void **)((char *)buf + offsetof(VstEvents32, events)); }


/* Transport state, for audioMasterGetTime.
 *
 * All doubles and int32s -- no pointers -- so the layout is the same at both
 * widths and this one definition serves the 64-bit and i386 hosts alike.
 *
 * Returning NULL here is legal but not safe in practice: a plugin that reads
 * tempo or ppqPos dereferences whatever it is handed, and four u-he plugins
 * crashed inside processReplacing for exactly that reason. Answering with a
 * plausible, advancing transport costs nothing and is what a real host does. */
typedef struct {
    double  samplePos;
    double  sampleRate;
    double  nanoSeconds;
    double  ppqPos;
    double  tempo;
    double  barStartPos;
    double  cycleStartPos;
    double  cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t smpteOffset;
    int32_t smpteFrameRate;
    int32_t samplesToNextClock;
    int32_t flags;
} VstTimeInfo;

/* kVstTransportPlaying | kVstPpqPosValid | kVstTempoValid | kVstBarsValid
 * | kVstTimeSigValid */
#define VST_TIME_FLAGS ((1 << 1) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 13))

/* Fill in a transport at `samples` into a 120 bpm 4/4 timeline. */
static inline void vst_time_set(VstTimeInfo *t, double samples, double rate)
{
    double beats;
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->samplePos  = samples;
    t->sampleRate = rate > 0.0 ? rate : 48000.0;
    t->tempo      = 120.0;
    beats         = samples / t->sampleRate * (t->tempo / 60.0);
    t->ppqPos     = beats;
    t->timeSigNumerator   = 4;
    t->timeSigDenominator = 4;
    t->barStartPos = ((double)(long)(beats / 4.0)) * 4.0;
    t->nanoSeconds = samples / t->sampleRate * 1e9;
    t->flags      = VST_TIME_FLAGS;
}

#endif /* PELOAD_VST2_32_H */
