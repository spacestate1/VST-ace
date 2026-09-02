# Plug-ins tested

Every plug-in the two windows and the command-line hosts are exercised against:
**270** across seven platform/format combinations.

None of them are in this repository. They live outside the tree, under
`../windows`, `../linux` and `../macos`, and they are their authors' own — nothing
here redistributes any of them. This is the list so that a result can be
reproduced against the same corpus.

The list is generated rather than maintained by hand: `peload --detect` reads each
file's headers and reports the loader it needs, without loading it.

## The three in the README

| plug-in | why |
|---|---|
| **FB-7999** — Full Bucket Music, Windows VST2 | The target the whole effort started on: a Korg DW-8000 simulation whose wavetable and preset banks are what `c/` reimplements. Its editor is drawn by the Win32 layer and blitted. |
| **Cardinal** — DISTRHO, Linux VST3 | VCV Rack as a plug-in. An OpenGL editor embedded as an X11 child window, running at 59 fps inside the host, and the one that leans hardest on the run-loop hooks. |
| **FB-3300** — Full Bucket Music, Windows VST2 | A Korg PS-3300 simulation: three complete synthesizer blocks, 229 parameters, and the widest editor in the corpus. |

## Windows VST2, 64-bit — 40
`windows/VST2-64` — `.dll`, loaded by the PE loader and the Win32 subsystem

| | | |
|---|---|---|
| `blooo64.dll` | `freqshifter64.dll` | `oxid64.dll` |
| `brokenmini64.dll` | `fury6864.dll` | `paralogy64.dll` |
| `bucketone64.dll` | `fury80064.dll` | `pecs64.dll` |
| `bucketpops64.dll` | `grainstrain64.dll` | `qyooo64.dll` |
| `deputy64.dll` | `kern64.dll` | `ragnarok264.dll` |
| `drumtraqs64.dll` | `modulair64.dll` | `scrooo64.dll` |
| `fb0264.dll` | `monofury64.dll` | `sequencair64.dll` |
| `fb310064.dll` | `mps64.dll` | `sixtraq64.dll` |
| `fb320064.dll` | `nabla64.dll` | `stigma64.dll` |
| `fb330064.dll` | `NI Absynth 5.dll` | `TAL-U-No-62.dll` |
| `fb799964.dll` | `NI FM8.dll` | `tricent64.dll` |
| `fbdelay64.dll` | `NI Kontakt 5.dll` | `whispair64.dll` |
| `fbphaser64.dll` | `NI Massive.dll` |  |
| `fbvc64.dll` | `ny64.dll` |  |

## Windows VST3 — 35
`windows/VST3` — `.vst3`, same loader, VST3 plumbing on top

| | | |
|---|---|---|
| `blooo.vst3` | `fbphaser.vst3` | `oxid.vst3` |
| `brokenmini.vst3` | `fbvc.vst3` | `paralogy.vst3` |
| `bucketone.vst3` | `freqshifter.vst3` | `pecs.vst3` |
| `bucketpops.vst3` | `fury68.vst3` | `qyooo.vst3` |
| `deputy.vst3` | `fury800.vst3` | `ragnarok2.vst3` |
| `drumtraqs.vst3` | `grainstrain.vst3` | `scrooo.vst3` |
| `fb02.vst3` | `kern.vst3` | `sequencair.vst3` |
| `fb3100.vst3` | `modulair.vst3` | `sixtraq.vst3` |
| `fb3200.vst3` | `monofury.vst3` | `stigma.vst3` |
| `fb3300.vst3` | `mps.vst3` | `tricent.vst3` |
| `fb7999.vst3` | `nabla.vst3` | `whispair.vst3` |
| `fbdelay.vst3` | `ny.vst3` |  |

## Windows VST2, 32-bit — 40
`windows/VST2-32` — `.dll`, i386 -- run directly by `peload32`, or bridged out-of-process into the 64-bit hosts

| | | |
|---|---|---|
| `blooo.dll` | `freqshifter.dll` | `oxid.dll` |
| `brokenmini.dll` | `fury68.dll` | `paralogy.dll` |
| `bucketone.dll` | `fury800.dll` | `pecs.dll` |
| `bucketpops.dll` | `grainstrain.dll` | `qyooo.dll` |
| `deputy.dll` | `kern.dll` | `ragnarok2.dll` |
| `drumtraqs.dll` | `modulair.dll` | `scrooo.dll` |
| `fb02.dll` | `monofury.dll` | `sequencair.dll` |
| `fb3100.dll` | `mps.dll` | `sixtraq.dll` |
| `fb3200.dll` | `nabla.dll` | `stigma.dll` |
| `fb3300.dll` | `NI Absynth 5.dll` | `TAL-U-No-62.dll` |
| `fb7999.dll` | `NI FM8.dll` | `tricent.dll` |
| `fbdelay.dll` | `NI Kontakt 5.dll` | `whispair.dll` |
| `fbphaser.dll` | `NI Massive.dll` |  |
| `fbvc.dll` | `ny.dll` |  |

## Linux native — 56
`linux/extracted` — `.so` and `.vst3`, dlopen'd, editors embedded as X11 child windows

| | | |
|---|---|---|
| `ADLplug.so` | `KlangFalter.so` | `Wolpertinger.so` |
| `CardinalFX.vst3` | `LUFSMeterMulti.so` | `EightySix.vst3` |
| `CardinalSynth.vst3` | `LUFSMeter.so` | `JE8086.vst3` |
| `Cardinal.vst` | `Luftikus.so` | `Osirus.vst3` |
| `Cardinal.vst3` | `Obxd.so` | `OsTIrus.vst3` |
| `CardinalFX.so` | `PitchedDelay.so` | `Vavra.vst3` |
| `CardinalSynth.so` | `ReFine.so` | `Xenia.vst3` |
| `Dexed.so` | `StereoSourceSeparation.so` | `OB-Xd.so` |
| `drowaudio-distortionshaper.so` | `TAL-Dub-3.so` | `OB-Xd.vst3` |
| `drowaudio-distortion.so` | `TAL-Filter-2.so` | `OB-Xf.vst3` |
| `drowaudio-flanger.so` | `TAL-Filter.so` | `Odin2.vst3` |
| `drowaudio-reverb.so` | `TAL-NoiseMaker.so` | `OPNplug.so` |
| `drowaudio-tremolo.so` | `TAL-Reverb-2.so` | `Surge XT Effects.vst3` |
| `drumsynth.so` | `TAL-Reverb-3.so` | `Surge XT.vst3` |
| `EasySSP.so` | `TAL-Reverb.so` | `TripleCheese.64.so` |
| `eqinox.so` | `TAL-Vocoder-2.so` | `TyrellN6.64.so` |
| `HiReSam.so` | `TheFunction.so` | `ZebraHZ.64.so` |
| `JuceDemoPlugin.so` | `ThePilgrim.so` | `Zebralette3.64.so` |
| `JuceOPL.so` | `vex.so` |  |

Not loadable by any backend here: `Cardinal.vst`.

## macOS VST2 — 31
`macos/VST2` — `.vst` bundles, loaded by the Mach-O loader with an Objective-C runtime

| | | |
|---|---|---|
| `Automaton.vst` | `Fury800.vst` | `Ragnarok.vst` |
| `Axon.vst` | `karlette.vst` | `RatshackReverb2.vst` |
| `Basic.vst` | `Kern.vst` | `Replicant.vst` |
| `Blooo.vst` | `model-e.vst` | `Scrooo.vst` |
| `Deputy.vst` | `ModulAir.vst` | `Stigma.vst` |
| `DrDevice.vst` | `MonoFury.vst` | `Tattoo.vst` |
| `FB3100.vst` | `MPS.vst` | `Tricent.vst` |
| `FB3200.vst` | `Nabla.vst` | `vb1.vst` |
| `FB3300.vst` | `neon.vst` | `WhispAir.vst` |
| `FB7999.vst` | `Phosphor.vst` |  |
| `FreqShifter.vst` | `Qyooo.vst` |  |

## macOS VST3 — 18
`macos/VST3` — `.vst3` bundles, the same Mach-O loader under the VST3 host

| | | |
|---|---|---|
| `Blooo.vst3` | `FreqShifter.vst3` | `Nabla.vst3` |
| `Deputy.vst3` | `Fury800.vst3` | `Qyooo.vst3` |
| `FB3100.vst3` | `Kern.vst3` | `Scrooo.vst3` |
| `FB3200.vst3` | `ModulAir.vst3` | `Stigma.vst3` |
| `FB3300.vst3` | `MonoFury.vst3` | `Tricent.vst3` |
| `FB7999.vst3` | `MPS.vst3` | `WhispAir.vst3` |

## macOS Audio Units — 50
`macos/AU` — `.component` bundles, same loader

| | | |
|---|---|---|
| `Automaton.component` | `MNSpectralBinShift.component` | `MNSpectralWeave.component` |
| `Axon.component` | `MNSpectralBlurring.component` | `MNSuperFilterBank.component` |
| `Basic.component` | `MNSpectralDroneMaker.component` | `ModulAir.component` |
| `Blooo.component` | `MNSpectralEmergence.component` | `MonoFury.component` |
| `Chorus.component` | `MNSpectralFilterbank.component` | `MPS.component` |
| `Deputy.component` | `MNSpectralFreezing.component` | `Nabla.component` |
| `DrDevice.component` | `MNSpectralGateAndHold.component` | `Phosphor.component` |
| `FB3100.component` | `MNSpectralGlidingFilters.component` | `Qyooo.component` |
| `FB3200.component` | `MNSpectralGranulation.component` | `Ragnarok.component` |
| `FB3300.component` | `MNSpectralHarmonizer.component` | `RatshackReverb2.component` |
| `FB7999.component` | `MNSpectralPartialGlide.component` | `Replicant.component` |
| `FreqShifter.component` | `MNSpectralPitchShift.component` | `Scrooo.component` |
| `Fury800.component` | `MNSpectralPulsing.component` | `Stigma.component` |
| `GrainStreamer.component` | `MNSpectralShimmer.component` | `Tattoo.component` |
| `IdeeFixer.component` | `MNSpectralShuffle.component` | `Tricent.component` |
| `Kern.component` | `MNSpectralStretch.component` | `WhispAir.component` |
| `MNSpectralAveraging.component` | `MNSpectralTracing.component` |  |

## Known non-renderers

Not loader failures -- these are the plug-ins saying so themselves, and they are
documented at length in `peload/README.md`:

- The five gearmulator plug-ins (`JE8086`, `Osirus`, `OsTIrus`, `Vavra`, `Xenia`)
  emulate Access Virus and Waldorf hardware and refuse to make a sound without the
  original firmware ROM, which is in no download.
- `Cardinal.vst3` opens an empty rack by default, so it is silent until patched.
- `brokenmini` and `drumtraqs` load and run but render silence at both widths --
  plug-in behaviour, not the host's.
- `brokenmini`'s editor is refused rather than painted: it registers a font that
  never reaches the text backend.
- `NI Massive` calls `ExitProcess(1)` while loading. It is reported as a failed
  load rather than taking the window down with it, which is why plug-ins are
  hosted out of process by default.

On the macOS side, and unlike the above, these are ours rather than the
plug-ins' -- they are counted against the host in `peload/README.md`:

- `model-e` is the one macOS VST2 whose editor still draws nothing: it is
  VSTGUI 4 driving a `.uidesc` layout through the Resource Manager, which is not
  implemented. Its audio works.
- One of the fifty Audio Units, `Ragnarok.component`, exports the
  pre-AudioComponent Component Manager entry point and nothing else, which the
  AU host does not speak; it declines rather than crashing. Its VST2 and VST3
  builds load. Eight others used to fail the same way -- every Audio Damage
  Audio Unit -- and they turned out to be Symbiosis wrappers holding a complete
  VST2, which is what the host loads now: all eight render, show their editors,
  and produce a byte-identical WAV to their own `.vst` build.

