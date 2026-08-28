# The FB-7999 toolchain in C

`make` builds all of it. The one to run is `../dw`:

| | needs | does |
|---|---|---|
| `../dw`           | + ALSA     | **the launcher** — the engine, the plugin hosts, everything below |
| `build/fbextract` | libc, libm | pulls resources, wavetables and preset banks out of the plugin |
| `build/dwrender`  | libc, libm | renders presets to WAV through a DW-8000 engine |
| `build/dwplay`    | + ALSA     | plays that engine live from a MIDI or computer keyboard |

`dwplay` is skipped automatically if ALSA is not installed, and `dw` is built
without its live and playback commands; the rest still builds.

`dw` is not a wrapper around the three below — it links the same objects, so
`list`, `play`, `demo`, `render` and the live keyboard all happen in its own
process. The single-purpose programs stay because they are useful on their own
and because their arguments are what the notes in this directory quote.

## Playing it

`../dw` finds the plugin and the banks itself, from any working directory —
the wavetable and all three preset banks are read straight out of the plugin's
resources, so nothing has to be extracted first:

```bash
../dw                    # play the keyboard (the default)
../dw demo               # a tour of the factory presets
../dw play "slap bass"   # one preset, by name or index
../dw list               # the bank's 64 programs
../dw render out/        # the whole bank to WAVs
../dw gui                # the GTK window
../dw pe                 # the Qt plugin host, with each plugin's own editor
```

`dwplay -o capture.wav` records everything played. That exists mainly as a test
hook: a live synth is otherwise hard to assert on, and it is what caught the
master bus clipping on held chords.

`BANK=B` or `BANK=6000` switches banks; `NOTE`, `GATE` and `LEN` adjust what
gets rendered.

In `live`, any hardware MIDI keyboard is connected automatically. The computer
keyboard also plays: `zsxdcvgbhnjm` is the lower octave and `q2w3er5t6y7u` the
upper.

| key | does |
|---|---|
| up / down, or `[` `]` | previous / next preset (wraps) |
| left / right, or `-` `=` | octave down / up |
| space | all notes off |
| Ctrl-C or shift-Q | quit |

Quit is deliberately not bound to ESC: arrows, function keys and mouse reports
all arrive as ESC-prefixed sequences, so a bare-ESC binding would end the
session on the first arrow press. Terminal input is buffered rather than read a
byte at a time, because an escape sequence can arrive split across reads — an
arrow is three bytes (`ESC [ A`) and there is no guarantee they land together.
An incomplete sequence is held for the next block and dropped if it never
completes. Audio xruns and sink changes are recovered in place, not fatal.

A terminal reports key-press but not key-release, so QWERTY notes release
themselves after a fixed gate — a real keyboard gets proper note-off and velocity.

## Extraction

```bash
make                # -> build/{fbextract,dwrender,dwplay}
make verify         # re-derive everything and diff against ../out
```

## Usage

```
fbextract all       <plugin.dll|.vst3> <outdir>   resources + spectra + waves + banks
fbextract resources <plugin.dll|.vst3> <outdir>   walk the PE .rsrc tree
fbextract probe     <WAVEDST>                     structural analysis
fbextract spectra   <WAVEDST> [out.json]          per-waveform harmonic report
fbextract waves     <WAVEDST> <outdir>            resynthesis to WAV + verification
fbextract bank      <PROGINIT> [-n names.txt] [-c out.csv] [-v]
```

Works on `fb799964.dll` and `fb7999.vst3` alike — their resources are
byte-identical, so either is a valid input.

## What replaced what

| C | was |
|---|---|
| `pe.c` — PE32+ reader, `.rsrc` walk | `7z x fb799964.dll .rsrc` |
| `wavedst.c` — `wavedst_probe()` | `analyze_wavedst.py` |
| `wavedst.c` — `wavedst_load/report()` | `parse_wavedst.py` |
| `wavedst.c` — `wavedst_synth/match()`, `wav.c` | `render_waves.py` |
| `bank.c` — FXB + iPlug2 chunk decode | *(new — no Python equivalent)* |

The Ghidra scripts (`../scripts/DecompileAll.java`, `DumpRttiClasses.java`) stay
in Java; they run inside Ghidra and have no C form.

## Equivalence with the Python output

`make verify` runs the whole chain against `../out` and reports:

- **resources** — byte-identical, excluding `VERSION`. 7z renders `RT_VERSION`
  as a text listing (`version.txt`); `fbextract` writes the raw
  `VS_VERSIONINFO` blob as `VERSION/1` instead, so the two aren't comparable.
- **waves** — all 33 WAVs byte-identical, including `dw_wavetable.wav`.
- **shape verification** — saw `r=0.9972`, square `0.9984`, sine `0.9989`,
  matching `render_waves.py` exactly.
- **spectra JSON** — numerically identical; only the formatting differs.
  `parse_wavedst.py` rounds to six *decimal places*, `fbextract` prints six
  *significant figures*, so small values keep more precision here
  (`4.41899e-06` vs `4e-06`). Max absolute difference 5e-7.

Two implementation notes, both of which cost real accuracy when got wrong:

- **Sine phase.** `wavedst_synth()` sums `a_k · sin(2πkt/N)` directly rather
  than going through an FFT. This is what numpy's `irfft()` produces when bin
  *k* holds `-1j·a_k`, and it is what makes wave 0 come out as a true ramp;
  cosine phase gives the right spectrum but the wrong shape.
- **`sign()` at zero.** The ideal square used for verification maps an exact
  zero to zero, not to +1, matching `np.sign()`. Getting this wrong moves the
  square correlation from 0.9984 to 0.9981.

## Resource names

iPlug2's `.rc` quotes the image and font resource names and MSVC stores the
quotes as part of the name — the PNG entries really are named `"BACK.PNG"`,
quote characters included (the UTF-16 length prefix counts them: `0a 00` for
`"BACK.PNG"`). `fbextract` strips a matching pair so the extracted tree has
usable filenames, which is also what 7z displays.

## Robustness

`pe.c` bounds-checks every read against the file size and maps RVAs only
through real section spans. The resource walk is capped at three levels, since
a corrupt entry with the subdirectory bit set at the language level would
otherwise recurse forever.

Checked under `-fsanitize=address,undefined` against the real binary, six
truncation points, and 40 seeds of random corruption inside the resource
directory: no sanitizer reports, no hangs, graceful non-zero exits.

```bash
make clean && make CFLAGS="-O1 -g -std=c99 -Wall -Wextra \
    -Wno-format-truncation -fsanitize=address,undefined -fno-omit-frame-pointer"
```

## Preset banks

`bank` decodes the `PROGINIT` resources — see the header comment in `bank.h`
for the layout. All 69 parameters are labelled with their real names, recovered
from the binary by `../scripts/recover_params.py` and compiled into `bank.c`;
`-n names.txt` still overrides them if you want your own.

Two traps in this format, both of which produce output that looks fine:

- **The record body is `1 + 69*8 + 8`, not `9 + 69*8`.** Both consume the chunk
  exactly — the sizes coincide — but the second shifts every value into the
  neighbouring parameter's slot. The giveaway is the column profile: with the
  wrong offset, `OSC1 Waveform`'s 1..16 range shows up under `OSC1 Octave`.
- **The parameter order is not the order the pseudo-C shows.** Ghidra reorders
  independent statements. The real index is the immediate in each call's
  bounds check (`cmp $imm,%rax`, imm = index × 8).

The `--- column profile ---` block is the check that the mapping is right:
every column must fall inside the range its name implies — waveforms 1..16,
levels and envelope stages 0..31, octaves 0..2, cutoff 0..63, velocity 0..7.
`Volume` is the only parameter the factory banks store as a fraction.
