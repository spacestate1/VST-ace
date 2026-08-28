# FB-02 v1.1.1 (Yamaha FB-01) — what transfers from the FB-7999 work

Target: `windows/VST2-64/fb0264.dll`, PE32+ x86-64, 10.8 MB, released 16 Jan 2026.
Same iPlug2 + Skia build as the rest of the catalogue
(`F:\Dvlp\iPlug2\FullBucket\fb02\build-win\pdbs\`).

## Acquisition

The download was not dead. `fetch_fullbucket.sh` matched
`dl.php?file=[A-Za-z0-9_]+_winvst`, but the current link is
`fb02_1.1.1_winvst` — **dots in the version string**, outside that character
class, so the script reported "no windows download link found". Fixed by adding
`.` to the class.

## Preset banks

**12 banks × 336 programs = 4,032 presets**, far more than FB-7999's 3 × 64:
`FACTORY`, `KRAFTRAUM`, `EMPTY`, and `COLLECTION1`..`COLLECTION9`.

The FXB container is byte-identical to FB-7999's — `CcnK` / `FBCh` / `tffp`,
id `fb02`. Only the program body differs:

```
+0        u8      version (always 1)
+1..11    char    "FB-02_1.1.0"     (+12 is padding, uninitialised)
+16..22   char[7] voice name        (matches the record name in 336/336)
+23       u8      always 6
+24       double  1.0 / 1.975 / 2.0
+32..71   40 B    zero in 38 of 40 positions
+72..157  86 B    the FM voice payload, 83 of 86 vary per program
```

Total body 158 bytes. Note the 7-character name field: that is the FB-01's own
voice-name length, so this record is close to the hardware's voice dump rather
than a parameter-per-slot layout. The payload begins `0x55` in every program
seen.

This is why `bank.c` had to grow a detector: FB-7999's body is *N* doubles,
FB-02's is packed bytes. The container generalises, the body does not.

## Parameter map — complete

**91 parameters**, laid out as:

| index | what |
|---|---|
| p00–p01 | Master Volume, Master Tune |
| p02–p05 | OP1–OP4 Wave |
| p06–p22 | 17 globals: Algorithm, Transpose, Pitch Bend Range, Portamento, Feedback, Mode, PMD Controller, Output L/R, and the 8 LFO parameters |
| p23–p39 | operator 1, 17 parameters |
| p40–p56 | operator 2 |
| p57–p73 | operator 3 |
| p74–p90 | operator 4 |

The operator block comes from the loop at `0x1805363d2`:

```
movzbl %r12b,%eax
imul   $0x11,%eax,%ecx      ; n * 17
sub    $0x11,%cl            ; (n-1) * 17
...
lea    0x17(%r13),%rcx      ; index = (n-1)*17 + 23
```

so **17 parameters per operator, first at index 23**. The four `OP%i: Wave`
parameters are registered by a separate loop at `0x18053572e` starting from
`mov $0x2,%ebx` — index 2 — which is exactly the p02–p05 gap left by the
literal-string pass.

The 18 `OP%i: <name>` format strings load into `%r8` (third argument of a
formatting helper), not `%rdx`, which is why the first scan for name arguments
found none of them.

## Preset payload — decoded

```
+72       u8      0x55, constant across all 4032 programs
+73..89   17 B    the globals, in registration order
+90..106  17 B    operator 1
+107..123 17 B    operator 2
+124..140 17 B    operator 3
+141..157 17 B    operator 4
```

`scripts/fb02_presets.py` decodes this; all 12 banks parse, 4,032 programs.

**What confirms it.** Every byte's observed range across the 336 factory
programs matches what its parameter name implies — 17/17 for the globals and
17/17 within each operator, and these are the Yamaha 4-operator ranges exactly:

| | |
|---|---|
| Algorithm 0–7 | the eight 4-op algorithms |
| Pitch Bend Range 0–12 | semitones |
| Feedback 0–7, LFO Waveform 0–3, LFO PM Sensitivity 0–7 | |
| Level 0–127, Frequency 0–15, Detune 0–7 | total level, multiple, detune |
| Keyb. Scaling Rate 0–3, Attack/Decay 0–31, Sustain/Release 0–15 | |

Output Left and Output Right are constant 1 in every program.

It also holds up musically, which a wrong mapping would not: `The Pad` decodes
to Attack 7 (slow) where `Marple` gets 31 (instant), and `The Pad`'s Transpose
byte 244 reads as **−12**, an octave down — Transpose is signed. Across the
factory bank the transposes are 0, ±12, ±24, ±36, +48: octaves, as expected.
(One outlier stores 124, which does not fit that reading and is unexplained.)

## Earlier note: why the automatic locator needed help

`scripts/recover_params_any.py` locates the registration function without
needing a Ghidra decompile, and is validated against FB-7999: it finds
`0x18052f240` and all 60 parameters, matching the Ghidra-derived map exactly.

On FB-02 it needs `--at`, because the automatic ranking fails here for a real
reason. FB-02 registers its **per-operator** parameters in a loop with names
built at runtime (`OP1_...`, `OP4_...`), so the function carries only 19 inline
string references and does not top a ranking by string density. Three other
functions in the binary carry more.

```bash
python3 scripts/recover_params_any.py windows/VST2-64/fb0264.dll --at 0x180535936
```

recovers the 19 global parameters at `0x1805355c0`:

| | |
|---|---|
| p00–p01 | Master Volume, Master Tune |
| p06–p14 | Algorithm, Transpose, Pitch Bend Range, Portamento, Feedback, Mode, PMD Controller, Output Left, Output Right |
| p15–p22 | LFO Enable / Waveform / Speed / Sync / AM Depth / AM Sensitivity / PM Depth / PM Sensitivity |

Gaps at p02–p05 and everything past p22 are the loop-registered operator
parameters. Recovering those means reading the loop, not the call sites — the
literal-string pairing that worked on FB-7999 cannot reach them.

## What does not transfer

The synthesis engine. FB-7999 is subtractive — wavetable oscillators, ladder
filter, six-stage envelopes. The FB-01 is **4-operator FM**: operators,
algorithms, feedback, no filter at all. `dw_synth.c` shares nothing with it;
playing these presets means a new engine.

## Status

| | |
|---|---|
| resource extraction | works, unmodified |
| preset banks parsed | 12/12, all 4,032 programs |
| body structure | fully mapped, all 86 payload bytes named |
| parameter map | complete: 91 parameters |
| preset decoder | `scripts/fb02_presets.py`, 12 banks / 4,032 programs, CSV export |
| audio engine | not started, and not shared with the DW-8000 engine |
