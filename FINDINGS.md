# FB-7999 v1.1.7 (64-bit VST2) — reverse engineering notes

Target: `windows/VST2-64/fb799964.dll`, PE32+ x86-64, 8.3 MB, built 2025-01-17,
MSVC linker 14.29 (VS 2019). Not packed, not obfuscated, no DRM.

`windows/VST3/fb7999.vst3` is a separate build of the same source
(`fb7999.vst3_x64.pdb`, 8.4 MB, exports `GetPluginFactory`/`InitDll`/`ExitDll`)
whose **resources are byte-identical** to the VST2's — same `WAVEDST`, same
three preset banks, same GUI art. Everything below applies to both; only the
code differs, so the VST3 is worth importing into Ghidra only if you
specifically want VST3 plumbing rather than DSP.

## 1. It's built on an open-source framework

Leaked PDB path in `.rdata`:

```
F:\Dvlp\iPlug2\FullBucket\FB7999\build-win\pdbs\fb7999.vst2_x64.pdb
```

So the plugin is **iPlug2** (Oli Larkin, MIT-licensed, on GitHub) with **Skia**
for rendering. String tables confirm Skia, HarfBuzz, ICU, zlib, libpng and libjpeg
are all statically linked in.

This is the single most useful fact for the whole effort: the vast majority of the
5.9 MB `.text` is *known open-source code*. Build iPlug2 + Skia with the same MSVC
version and you can fingerprint and discard the library functions (Ghidra's Function
ID / BSim, or Diaphora against your own build), leaving only Björn's own code.

## 2. RTTI is intact — the class list is readable

306 RTTI type descriptors survive. Stripping the library ones leaves the entire
proprietary surface:

| Class | Role |
|---|---|
| `FB7999` | top-level plugin / iPlug2 subclass |
| `Synth` | voice engine |
| `Filter`, `LowPass`, `HighPass` | DSP filter objects |
| `IFBParamControl`, `IFBSliderControl`, `IFBOptionMenu`, `IFBNumberDisplay`, `IFBWavetableSwitch`, `IFBWaveformInfo`, `IFBVCFMGModSource`, `IFBSwitchMidiLearn`, `IFBTapeLoadDialog`, `IFBCornerResizer`, `IFBSizeContextControl`, `IFBUtilityKick` | GUI widgets |
| `mtsclientglobal` | Oddsound MTS-ESP microtuning client (also open source) |

That's **five DSP classes**. The actual synthesis code is a small island in a large
sea of library code, and RTTI gives you the vtables to find it.

Full list: `out/rtti_all.txt`. Demangle with `llvm-undname`.

## 3. Complete parameter map (recovered from the string table)

The full DW-8000 architecture, including its distinctive 6-stage envelopes
(Attack / Decay / Break Point / Slope / Sustain / Release) on both VCF and VCA:

```
OSC1 Waveform/Octave/Level      OSC2 Waveform/Octave/Level/Interval/Detune
Noise Level                     Mode (Poly 1/Poly 2/Unison 1/Unison 2)
Auto Bend Select/Time/Mode/Intensity
VCF Cutoff/Resonance/KBD Track
VCF EG Attack/Decay/Break Point/Slope/Sustain/Release/Intensity/Polarity/Velocity
VCA EG Attack/Decay/Break Point/Slope/Sustain/Release/Velocity
MG Waveform (TRI/SAW/RAMP/RECT)/Frequency/Delay/OSC/VCF
Bend OSC, Bend VCF, Portamento
Delay Time/Factor/Feedback/Level/Mod. Intensity/Mod. Frequency
After Touch OSC MG / VCA / VCF,  Mod.Wheel Osc MG / VCF MG
Tune, Volume, Voices, "Pseudo" Stereo, Wavetable Set, DW Mode
```

Non-original extras: `Wavetable Set`, `DW Mode`, `Voices`, `"Pseudo" Stereo`,
MTS-ESP tuning, and a **cassette tape dump loader** (`DW-8000 Tape Dump Loaded`).

## 4. Resources — the interesting part

`7z x fb799964.dll .rsrc` extracts everything cleanly (see `out/resources/`), and
`c/build/fbextract resources` now does the same without the dependency:

| Resource | Size | Contents |
|---|---|---|
| `DSTDATA/WAVEDST` | 40,832 B | **the wavetable ROM** |
| `BANK_A/PROGINIT` | 37,008 B | factory bank A |
| `BANK_B/PROGINIT` | 37,064 B | factory bank B |
| `PROG6000/PROGINIT` | 37,028 B | DW-6000 factory bank |
| `PNG/*` | 14 files | GUI graphics (BACK.PNG is 1 MB) |
| `TTF/ROBOTO-REGULAR.TTF` | 145 KB | UI font |

### WAVEDST decoded

40,832 bytes = **10,208 float32** = **32 waveforms × 319 harmonic amplitudes**.

Not sampled PCM — additive harmonic spectra, one amplitude per harmonic, sign
carrying the 0/π phase flip. Confirmed by resynthesis (`scripts/render_waves.py`),
which cross-correlates the reconstructions against ideal shapes:

| Wave | Identity | Evidence |
|---|---|---|
| 0 | sawtooth | aₙ = a₁/n exactly (ratios 1.0000–1.0019); r = **0.9972** vs ideal ramp |
| 25 | square | a₁ = 0.6366 = 2/π, odd harmonics only at a₁/n; r = **0.9984** |
| 27 | sine | a₁ = 0.4999, all others ≈ 0; r = **0.9989** |
| 22 | impulse/bright | flat ≈0.0215 across all 319 harmonics |
| 6 | pulse/formant | h1–h4 equal, h5 = 0 |

Waves 2↔18, 3↔19, 13↔20 are near-duplicate pairs — two banks of 16, which is what
the `Wavetable Set` parameter switches between.

Reconstruction uses **sine phase** (`-1j * amplitude` into the FFT bin). Cosine
phase gives the right spectrum but the wrong shape — wave 0 comes out as a log
spike rather than a ramp.

Rendered output in `out/waves/`:
- `dw_wave_00.wav` … `dw_wave_31.wav` — single cycles, 2048 samples each
- `dw_wavetable.wav` — all 32 frames concatenated, Serum-layout; **Surge XT, Vital
  and Bitwig import this directly** as a 32-frame wavetable

This is the direct K3 connection: the Kawai K3's user waveform is also defined as a
set of additive harmonic levels, so these are directly comparable to what your
hardware stores.

### Preset banks decoded

The three `PROGINIT` resources are VST2 FXB banks carrying an opaque plugin
chunk. Both layers are plain:

```
'CcnK'  u32be byteSize    'FBCh'  u32be version(2)
'fb79'  u32be fxVersion   u32be numPrograms(64)   byte future[128]
u32be chunkSize   byte chunk[chunkSize]
```

and the chunk is iPlug2's little-endian serialisation:

```
'tffp'  u32le 0x00010000
64 × { u32le nameLen; char name[nameLen];
       u8 version(1); double param[69]; u32le five(5); u32le revision }
```

This parses all three banks with **zero bytes left over**. `fxVersion` varies —
502 / 403 / 10001 for `BANK_A` / `BANK_B` / `PROG6000` — as does the record's
own trailing `revision` word (502 / 259 / 1001), which coincides with
`fxVersion` only for `BANK_A`.

The record body is 561 bytes = `1 + 69*8 + 8`. **The single leading byte
matters:** reading the doubles from offset 9 with no trailer also consumes the
chunk exactly — the sizes coincide — but silently shifts every value into the
neighbouring parameter's slot. The 69 count is independently confirmed by the
registration loop in `FUN_18052f240`, which bounds at `0x45` = 69.

Parameters are stored as `double` but hold the DW-8000's own integer ranges:
1..16 for the two waveform selectors, 0..31 for levels and envelope stages,
0..2 for octave, 0..63 for cutoff, 0..7 for velocity. `Volume` is the only
parameter the factory banks ever store as a fraction.

So the factory presets — DW-8000 banks A and B, plus the DW-6000 set — are
readable and writable without touching a single instruction of the plugin's
code. `c/build/fbextract bank <PROGINIT> -c out.csv` dumps them, labelled.

### Parameter map recovered

All 69 slots are now named. Every `InitParam` call lives in one function,
`FUN_18052f240`, but **the pseudo-C is not usable for this** — Ghidra reorders
independent statements, putting `OSC1 Octave` before `OSC1 Waveform` and
yielding a map that is off by one against the stored data.

The machine code is unambiguous. Each call is preceded by an inlined bounds
check on the parameter array:

```
movslq 0x14(%rbx),%rax          ; count
and    $0xfffffffffffffff8,%rax
cmp    $imm,%rax                ; imm = index * 8
jbe    skip
```

so the index is the immediate over eight (index 0 uses `test` instead of
`cmp`), and the name is the string loaded into `%rdx`.
`scripts/recover_params.py` does this and writes `out/param_map.json`; the
table is also compiled into `c/src/bank.c`.

The trailing slots 60..68 are registered by a loop with a *computed* index, so
they carry no `cmp $imm` of their own — a naive parser misattributes the first
of them to index 59, which is really `VCF MG Mod. Source`.

**What confirms the mapping:** every column of the factory banks falls inside
the range its recovered name predicts — waveforms 1..16, levels and envelope
stages 0..31, octaves 0..2 (matching the `16'`/`8'`/`4'` strings), cutoff
0..63, EG velocity 0..7, `Mode` 0..3 (Poly 1/Poly 2/Unison 1/Unison 2), `MG
Waveform` 0..3 (TRI/SAW/RAMP/RECT). That agreement across 59 parameters at once
is not something a wrong mapping produces.

### Who authored the programs

The plugin-only parameters — the ones no DW-8000 has — are almost untouched,
and how they are set says where the data came from:

| | `BANK_B` | `BANK_A` | `PROG6000` |
|---|---|---|---|
| `"Pseudo" Stereo`, `Tune`, `VCF MG Mod. Source` | all default | all default | all default |
| `Volume` | all 0.5 | **38 values** (25 still 0.5) | all 0.5 |
| `Voices` | all 0 | all 0 | **all −1** |
| `DW Mode` | all 0 | all 0 | **all 1** |
| `Wavetable Set` | all 0 | all 0 | **all 1** |

`BANK_B` is untouched across all 64 programs. `BANK_A` differs only in per-patch
`Volume` trim. `PROG6000` sets `DW Mode`, `Wavetable Set` and `Voices` to the
*same* value in all 64 — a global "configure the engine as a DW-6000" switch,
not per-patch sound design.

So the sound design is the hardware's: these are **Korg's factory programs** for
the DW-8000 (banks A and B) and the DW-6000, transcribed. The names corroborate
it — `CX-3` is a Korg organ, and `GLIDE BRASS I/II`, `AUTO BRASS (4th)`,
`DIGITAL INVASION`, `SPACE GHOST` are the 1985 factory names. The plugin's
cassette **tape dump loader** (`DW-8000 Tape Dump Loaded`) is a plausible
ingestion route. Nothing in the binary credits preset authorship; the only
author string is `Full Bucket Music`.

That splits the rights two ways, which §6 should be read with: the *plugin* is
Björn Arlt's, but the *program data* is Korg's.

Note also that `PROG6000` storing `Voices = -1` argues against reading that
parameter as a simple offset from 8 — a DW-6000 is 6-voice, not 7. The −4..+3
range is more likely an index into a list of voice counts. `dw_synth.c`
currently assumes `8 + value`, which is wrong for this bank.

## 5. Decompilation

```bash
export GHIDRA_MAXMEM=6G DECOMP_OUT=.../re/out/fb7999_decompiled.c
/opt/ghidra/support/analyzeHeadless re/project fb7999 \
  -import windows/VST2-64/fb799964.dll \
  -scriptPath re/scripts -postScript DecompileAll.java
```

Project is kept at `re/project/` — open it in the Ghidra GUI to browse
interactively, which is far more productive than reading the flat `.c` dump.

**Realistic expectation:** this gives you readable pseudo-C per function, not
buildable source. Value comes from reading the five DSP classes, not from the
30 MB of library noise.

As it stands `out/fb7999_decompiled.c` has 81,829 `FUN_` references and exactly
four named symbols — the `FB7999`, `Synth`, `LowPass` and `HighPass` vtables.
Nothing has been walked from those vtables to their methods yet; that is the
next real step.

## 5b. Tooling

`c/` holds `fbextract`, the whole extraction chain as one dependency-free C99
program (`make && make verify`) — PE resource walking, WAVEDST decoding,
additive resynthesis, and the bank decoder above. It reproduces the Python
output exactly: all 33 WAVs byte-identical, correlations identical to four
decimal places, resources identical. See `c/README.md` for the equivalence
report and the two details that cost accuracy if got wrong (sine phase, and
`sign()` at exactly zero).

The Python scripts remain in `scripts/` as the reference implementation. The
Ghidra scripts stay in Java — they run inside Ghidra.

## 6. Legal position

`fb7999_license.txt`, verbatim:

> You may NOT distribute, sell, rent, or modify this software or any accompanying
> files without permission from the author.

Freeware, all rights reserved, © Björn Arlt. Analysing your own legally-obtained
copy is one thing; the extracted wavetables, preset banks and GUI art are his
copyrighted work, and shipping anything derived from them — a port, a wavetable
pack, a patch set — needs his permission. He's approachable and a one-man shop.

Note also that the underlying **DW-8000 waveforms are Korg's**, not Björn's, and
they're independently documented in the DW-8000 service manual and MIDI
implementation chart. For a clean-room reimplementation, that's the source to work
from — no reversing required, and no licence problem.


