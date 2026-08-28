/* The slice of the VST 2.4 ABI a headless host needs.
 *
 * Layout is fixed by the published SDK header and must match byte for byte,
 * including the reserved/padding fields -- the plugin was compiled against it.
 * Only the members this host touches are named; the rest hold their offsets. */
#ifndef PELOAD_VST2_H
#define PELOAD_VST2_H

#include <stdint.h>

/* A Windows VST2 uses the Microsoft x64 convention; a macOS one uses System V,
 * which is also the host's. The layout is identical either way -- only the
 * function pointers' convention differs -- so the same header serves both, with
 * VST2_SYSV selecting the macOS flavour. */
#ifdef VST2_SYSV
#define MS
#else
#define MS __attribute__((ms_abi))
#endif

typedef struct AEffect AEffect;

typedef MS intptr_t (*audioMasterCb)(AEffect *, int32_t, int32_t, intptr_t, void *, float);

struct AEffect {
    int32_t   magic;                 /* 'VstP' */
    MS intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    MS void   (*process)(AEffect *, float **, float **, int32_t);
    MS void   (*setParameter)(AEffect *, int32_t, float);
    MS float  (*getParameter)(AEffect *, int32_t);
    int32_t   numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t  resvd1, resvd2;
    int32_t   initialDelay, realQualities, offQualities;
    float     ioRatio;
    void     *object, *user;
    int32_t   uniqueID, version;
    MS void   (*processReplacing)(AEffect *, float **, float **, int32_t);
    MS void   (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char      future[56];
};

/* effect opcodes (host -> plugin) */
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

/* MIDI / event structures for note input */
typedef struct {
    int32_t type;          /* 1 = kVstMidiType */
    int32_t byteSize;      /* 24 */
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength, noteOffset;
    char    midiData[4];
    char    detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;

typedef struct {
    int32_t numEvents;
    intptr_t reserved;
    void   *events[2];
} VstEvents;


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
/* kVstTransportPlaying is bit 1; the rest are the "this field is valid" bits. */
#define VST_TIME_PLAYING (1 << 1)

/* The transport a plugin reads through audioMasterGetTime.
 *
 * `tempo` and `playing` are the host's, not assumptions: an arpeggiator, a synced
 * delay and a tempo-locked LFO all take their rate from here, so a fixed 120 BPM
 * means every one of them runs at the wrong speed against a sequencer set to
 * anything else. */
static inline void vst_time_set_full(VstTimeInfo *t, double samples, double rate,
                                     double tempo, int num, int den, int playing)
{
    double beats;
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->samplePos  = samples;
    t->sampleRate = rate > 0.0 ? rate : 48000.0;
    t->tempo      = tempo > 0.0 ? tempo : 120.0;
    beats         = samples / t->sampleRate * (t->tempo / 60.0);
    t->ppqPos     = beats;
    t->timeSigNumerator   = num > 0 ? num : 4;
    t->timeSigDenominator = den > 0 ? den : 4;
    {   /* Bar length in beats, so a 6/8 bar is not assumed to be four beats. */
        double bar = (double)t->timeSigNumerator * 4.0 / (double)t->timeSigDenominator;
        if (bar <= 0.0) bar = 4.0;
        t->barStartPos = ((double)(long)(beats / bar)) * bar;
    }
    t->nanoSeconds = samples / t->sampleRate * 1e9;
    t->flags = VST_TIME_FLAGS;
    if (!playing) t->flags &= ~VST_TIME_PLAYING;
}

static inline void vst_time_set(VstTimeInfo *t, double samples, double rate)
{ vst_time_set_full(t, samples, rate, 120.0, 4, 4, 1); }

#endif /* PELOAD_VST2_H */
