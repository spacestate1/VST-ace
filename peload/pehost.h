/* pehost -- host a Windows x86-64 VST2 plugin natively on Linux, no Wine.
 *
 * Wraps the PE loader, the Microsoft-ABI bridge and the Win32 stub layer behind
 * a plain C API so both a CLI renderer and a GUI can drive the same plugin.
 *
 * Threading contract, which follows VST2's own:
 *   - pehost_open/close and the parameter/program calls come from one thread
 *     (the GUI thread).
 *   - pehost_render() is called from the audio thread and nothing else.
 *   - Note events are passed through an internal lock-free queue, so
 *     pehost_note_on/off are safe to call from the GUI thread while the audio
 *     thread renders.
 *
 * Every thread that will touch plugin code must call pehost_thread_init()
 * first: MSVC-generated code reads its TEB through %gs, and that register is
 * set up per thread. */
#ifndef PEHOST_H
#define PEHOST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pehost pehost;

/* Install this thread's fake TEB. Call once per thread before any other call
 * that reaches plugin code -- including from the audio thread. */
int pehost_thread_init(void);

pehost *pehost_open(const char *dll_path, double samplerate, int blocksize);
void    pehost_close(pehost *h);

/* ---- what a plugin is, and which loader gets it ------------------------
 *
 * Every format this host knows. pehost_open() sniffs a path and picks one of
 * these on its own; pehost_classify() reports which it would pick without
 * loading anything, and pehost_open_as() lets a caller force one when the
 * sniff is wrong -- a Windows VST3 shipped as a bare .dll, a plugin downloaded
 * into a layout the resolver does not expect. AUTO means "sniff it" and is only
 * an input to pehost_open_as; classify never returns it. UNKNOWN is what
 * classify returns when nothing matched. */
typedef enum {
    PEHOST_KIND_AUTO = 0,      /* pehost_open_as: detect, i.e. behave as pehost_open */
    PEHOST_KIND_UNKNOWN,       /* classify: not a plugin we recognise */
    PEHOST_KIND_WIN_VST2_64,   /* Windows PE32+ .dll, hosted in-process */
    PEHOST_KIND_WIN_VST2_32,   /* Windows PE32 .dll, hosted in the peload32 helper */
    PEHOST_KIND_WIN_VST3,      /* Windows PE .vst3, Microsoft-ABI VST3 */
    PEHOST_KIND_LINUX_VST3,    /* native ELF .vst3 bundle or .so, System-V-ABI VST3 */
    PEHOST_KIND_LINUX_VST2,    /* native ELF64 .so exporting VSTPluginMain */
    PEHOST_KIND_MAC_VST2,      /* .vst Mach-O bundle, run through the macOS shim */
    PEHOST_KIND_MAC_VST3,      /* .vst3 Mach-O bundle */
    PEHOST_KIND_MAC_AU,        /* .component Audio Unit */
    PEHOST_KIND_CLASSIC_MAC,   /* Classic Mac OS PEF/PowerPC, interpreted */
    PEHOST_KIND_COUNT
} pehost_kind;

/* The verdict from pehost_classify(): everything a browser or a downloader
 * needs to label a file and decide whether to offer it, without loading it. */
typedef struct {
    pehost_kind kind;        /* the loader pehost_open would choose */
    int         loadable;    /* 1 if this build can actually run it now */
    char        os[16];      /* "windows" | "linux" | "macos" | "classic" | "" */
    char        arch[16];    /* "x86-64" | "i386" | "ppc" | "" */
    char        format[8];   /* "VST2" | "VST3" | "AU" | "" */
    char        label[40];   /* human tag, e.g. "Windows VST2 32-bit" */
    char        binary[1024];/* the code file inside a bundle, or the path itself */
    char        why[160];    /* when !loadable, why not */
} pehost_info;

/* Sniff `path` and fill *out (which may be NULL to only get the return). Reads
 * file headers and, for bundles, the Contents/ layout; the one exception is a
 * native .so, which is dlopen'd to confirm it exports a VST2 entry point. Never
 * runs plugin init. Returns out->kind. */
pehost_kind pehost_classify(const char *path, pehost_info *out);

/* Stable machine name of a kind ("win-vst2-64", "linux-vst3", ...) for CLI
 * flags and settings, and the reverse (PEHOST_KIND_AUTO for an unknown name so
 * a bad --as value falls back to sniffing rather than failing). */
const char *pehost_kind_name(pehost_kind k);
pehost_kind pehost_kind_from_name(const char *name);

/* A short human label for a kind ("Windows VST2 64-bit"), for menus. */
const char *pehost_kind_label(pehost_kind k);

/* Like pehost_open, but forces a loader instead of sniffing. AUTO is exactly
 * pehost_open. Any other kind skips detection and hands the file straight to
 * that backend, so a download the sniffer cannot name still gets its chance --
 * and reports that backend's own error if the guess was wrong. */
pehost *pehost_open_as(const char *path, pehost_kind kind,
                       double samplerate, int blocksize);

/* Last failure from pehost_open(), for reporting. */
const char *pehost_last_error(void);

/* Can this loader run the plugin at `path`? Cheap: reads only the file header.
 * `why` receives a short reason when the answer is no (e.g. a 32-bit build).
 * Lets a browser skip or grey out plugins instead of failing at load time. */
int pehost_can_load(const char *path, char *why, int whyn);

/* True when `path` will be hosted in a helper process rather than in-process --
 * today that means a 32-bit image. Purely informational: every other call
 * behaves identically either way. */
int pehost_is_bridged(const char *path);

/* Whether a path is a native Linux VST2: an ELF64 shared object that exports
 * VSTPluginMain (or the pre-2.4 `main`). The name cannot decide this -- these
 * ship as bare .so files, and an LV2 bundle's inner .so looks identical from the
 * outside -- so the export is what is checked. Exposed because the plugin
 * scanner needs the same test to know a .so is worth offering. */
int pehost_is_native_vst2(const char *path);

/* Whether a path is a Windows plug-in: a PE that exports VSTPluginMain, the
 * pre-2.4 `main`, or VST3's GetPluginFactory. The extension cannot decide it --
 * an installer is a .dll too -- and both browsers need the same test, having
 * previously offered every .dll they found. Reads the export table out of the
 * file; nothing is mapped and nothing runs. */
int pehost_is_windows_vst(const char *path);

/* ---- data a plugin needs but does not carry ---------------------------
 *
 * Several plugins load, open their editor, and then cannot actually work. A
 * Virus or Waldorf emulation with no firmware ROM makes no sound; a u-he synth
 * whose Data folder was never installed draws an editor with no artwork. Both
 * say so only inside their own GUI or on stderr, so from the host's side they
 * look like a clean load and the user is left wondering why nothing happens.
 *
 * Recognition is structural, not a list of plugin names: what is looked for is
 * the folder the vendor's own installer or the plugin itself creates. So the
 * same test covers other products from the same families without knowing them.
 *
 * `where` is the folder the data belongs in. `from` is a copy that already
 * exists on this machine -- a release tree usually ships the data that its
 * installer would have copied -- or empty when there is nowhere to get it. */
typedef struct {
    char product[64];    /* the name the vendor's folders are keyed by */
    char need[160];      /* what is missing, in words fit for a status line */
    char where[1024];    /* the folder it belongs in */
    char from[1024];     /* a copy that exists already, or "" */
    int  repairable;     /* whether `from` can simply be linked into `where` */
} pehost_data_need;

/* 1 when something is missing, 0 when complete or not a plugin this knows.
 * Reads directories only -- nothing is loaded and nothing is written. */
int pehost_data_check(const char *path, pehost_data_need *out);

/* Link what `from` has and `where` lacks, for a need that came back
 * repairable. Returns the number of items linked, or -1 with `err` set.
 * Never overwrites: an entry already present in `where` is left alone. */
int pehost_data_repair(const pehost_data_need *need, char *err, int errn);

/* Run plugins in a helper process even when this one could load them, so a
 * plugin that faults costs a helper rather than the host. Off unless enabled
 * here or by PEHOST_ISOLATE=1. */
void pehost_set_isolation(int on);
int  pehost_isolation(void);

const char *pehost_name(const pehost *h);
const char *pehost_vendor(const pehost *h);
int         pehost_num_programs(const pehost *h);
int         pehost_num_params(const pehost *h);
int         pehost_num_inputs(const pehost *h);
int         pehost_num_outputs(const pehost *h);
int         pehost_is_synth(const pehost *h);
int         pehost_unique_id(const pehost *h);

void  pehost_program_name(pehost *h, int idx, char *buf, int n);
void  pehost_set_program(pehost *h, int idx);
int   pehost_get_program(pehost *h);

void  pehost_param_name(pehost *h, int i, char *buf, int n);
void  pehost_param_label(pehost *h, int i, char *buf, int n);
void  pehost_param_display(pehost *h, int i, char *buf, int n);
float pehost_get_param(pehost *h, int i);
void  pehost_set_param(pehost *h, int i, float v);   /* queued, applied in render */

/* Apply queued parameter and program writes now, without rendering a block.
 *
 * pehost_set_param defers to the audio thread, which is right when one is
 * running: a value set between two blocks belongs to the second, not to the
 * middle of the first. But a caller that sets values and reads them straight
 * back -- loading a patch and then saving or displaying it -- would otherwise
 * see the state from before, and a caller with no audio thread at all would
 * never see the write take effect.
 *
 * Safe to call while audio runs. The queue entries are marked consumed as they
 * are applied, and setParameter is idempotent, so the worst a race costs is one
 * parameter being written twice with the same value. */
void  pehost_flush_params(pehost *h);

void  pehost_note_on(pehost *h, int note, int vel);
void  pehost_note_off(pehost *h, int note);
void  pehost_all_notes_off(pehost *h);

/* The transport a plugin reads for tempo-synced behaviour -- arpeggiators,
 * synced delays, tempo-locked LFOs. Without this the host reports a fixed 120
 * BPM and every one of them runs at the wrong rate against a sequencer set to
 * anything else. MIDI clock (0xF8) and start/stop/continue (0xFA/0xFB/0xFC) sent
 * to pehost_midi drive these automatically, so a sequencer that sends clock needs
 * no further setup. */
void   pehost_set_tempo(pehost *h, double bpm, int tsig_num, int tsig_den);
double pehost_tempo(const pehost *h);
void   pehost_set_playing(pehost *h, int playing, int rewind);
int    pehost_playing(const pehost *h);

/* Move the transport to a position, in quarter notes from the start. A MIDI song
 * position pointer (0xF2) sent to pehost_midi does this automatically, so a
 * sequencer that locates mid-song lands the plugin on the same bar. */
void   pehost_locate(pehost *h, double ppq);
double pehost_position(const pehost *h);

/* Queue a raw MIDI voice message (status byte plus up to two data bytes).
 * Covers note on/off, CC, pitch bend, program change and aftertouch, so a
 * hardware keyboard's wheels and pedals reach the plugin unchanged. Safe to
 * call from the GUI thread while the audio thread renders. */
void  pehost_midi(pehost *h, int status, int d1, int d2);

/* Whether the plug-in is still running. Only ever false for one hosted out of
 * process whose helper has died -- in process, a plug-in that faults takes this
 * process with it and there is nobody left to ask. A front end wants this so a
 * dead plug-in reads as dead rather than as an editor that stopped repainting
 * and audio that went quiet. */
int   pehost_alive(const pehost *h);

/* Which input channels the signal fed to pehost_render_io reaches, as a bitmask
 * over channels (bit 0 = first input); 0 means all of them, which is the
 * default. A plug-in with a separate modulator and carrier input -- a vocoder --
 * wants the microphone on the modulator only, or the raw voice lands in the
 * output as well as the analysis. Applies across VST2, VST3 and the bridge. */
void  pehost_set_input_mask(pehost *h, unsigned mask);

/* The same, but placed: `frame` is the offset within the next rendered block at
 * which the event should take effect. This is what a sequencer wants -- it knows
 * exactly where a note falls, and passing that through is the difference between
 * a rhythm that lands and one quantised to the block size. Out-of-range offsets
 * are clamped into the block. Use pehost_midi when the time is simply "now". */
void  pehost_midi_at(pehost *h, int status, int d1, int d2, int frame);

/* MIDI that did not reach the plugin: events lost to a full queue, and events
 * deferred to a later block because one block's batch was full. The first is a
 * fault worth reporting -- a lost note-off hangs a note -- the second is normal
 * under load. Both are cumulative. */
void  pehost_midi_stats(const pehost *h, unsigned *dropped, unsigned *spilled);

/* Render `frames` of interleaved stereo. Drains queued events first.
 * Safe to call with h == NULL, which fills silence. */
void  pehost_render(pehost *h, float *interleaved, int frames);

/* Same, but feeds `in` (interleaved stereo, may be NULL for silence) to the
 * plugin's inputs. Effects need this: with silent inputs they correctly
 * produce silence, which looks like a failure but is not. */
void  pehost_render_io(pehost *h, const float *in, float *out, int frames);

/* ---- plugin editor (VST3 only for now) ---------------------------------
 *
 * A VST3 plugin draws its own GUI into a native window the host provides. On
 * Linux that is an X11 window id, so the caller passes a widget's window handle
 * and the plugin does the rest. VST2 editors need a Win32 window layer that does
 * not exist yet, so these report "no editor" for VST2. */
/* How the editor is delivered:
 *   X11    -- the plugin draws into a window you give it (native Linux VST3)
 *   PIXELS -- the plugin renders into a buffer we own (Windows VST2 and VST3) */
enum { PEHOST_EDITOR_NONE = 0, PEHOST_EDITOR_X11, PEHOST_EDITOR_PIXELS };
int  pehost_editor_kind(pehost *h);
/* Compare msvcp_shim.h's implementations against a real msvcp120.dll, field by
 * field. Returns 0 if they agree, 1 if they differ, 2 if there is no DLL. */
int  pehost_msvcp_selftest(void);

/* Open a PIXELS editor. Pump it from a UI timer, then read the pixels with
 * pehost_editor_pixels(). Returns 0 on success. */
int  pehost_editor_open(pehost *h);
void pehost_editor_pump(pehost *h);
int  pehost_editor_pixels(pehost *h, const unsigned int **px, int *w, int *height);

/* Loaded from a Mach-O bundle, or interpreted from a Classic one. For labelling
 * only. */
int  pehost_is_macos(pehost *h);

/* Interpreted from a Classic (CFM/PEF, PowerPC) bundle. For labelling only. */
int  pehost_is_classic(pehost *h);

/* Is this file a Classic Mac OS (PEF/PowerPC) plug-in? True for a bare PEF and
 * for a resource fork carrying an 'aEff' resource. Such a plug-in is interpreted
 * rather than loaded, so callers that enumerate plug-ins need to recognise it as
 * a candidate even though it is neither PE, ELF nor Mach-O. */
int  pehost_is_classic_mac(const char *path);

/* The one host directory a Classic plug-in can see, as its Application Support
 * folder. Settable with PELOAD_CLASSIC_SUPPORT. */
const char *pehost_classic_support_dir(void);

/* Install a callback the window layer invokes when a plugin polls for input from
 * inside its own message loop. Needed for modal drag loops -- see win32host.h. */
void pehost_set_input_pump(void (*fn)(void *), void *ud);
void pehost_editor_mouse(pehost *h, int x, int y, int msg, int buttons, int wheel);
void pehost_editor_key(pehost *h, int vk, int down, int ch);

int  pehost_has_editor(pehost *h);
void pehost_editor_size(pehost *h, int *w, int *height);
int  pehost_editor_can_resize(pehost *h);
int  pehost_editor_attach(pehost *h, unsigned long x11_window);
void pehost_editor_detach(pehost *h);
void pehost_editor_resized(pehost *h, int w, int height);

/* Diagnostics: how many imported symbols were implemented vs stubbed, and how
 * many stubs the plugin actually reached. */
void  pehost_import_stats(int *implemented, int *stubbed, int *stubs_called);

#ifdef __cplusplus
}
#endif

#endif /* PEHOST_H */
