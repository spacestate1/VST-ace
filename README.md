# Running plug-ins natively, without Wine

Audio plug-ins built for Windows, macOS and Linux, loaded and run as native code
on Linux. Not emulation and not Wine: a PE loader with a Win32 subsystem
underneath it, a Mach-O loader with an Objective-C runtime and a software Metal
rasteriser, and a CFM/PEF interpreter for Classic Mac OS — six hosts sharing one
set of shims.

| target | plug-ins | status |
|---|---|---|
| Windows VST2, x86-64 | 36 | 34 render, 35 open their own GUI |
| Windows VST2, i386 | 36 | 34 render, 35 open their own GUI, live playback |
| Windows VST2, i386, bridged | 36 | 34 render, 34 GUIs, out-of-process |
| Windows VST3, x86-64 | 35 | 35 render, 34 open their own GUI |
| Linux VST2 (`.so`), x86-64 | 39 | 37 render |
| Linux VST3, x86-64 | 13 | 7 render, X11-embedded editors |
| macOS VST2, x86-64 | 19 | 19 render, 19 open their own GUI |
| macOS Audio Units, x86-64 | 42 | 41 render |

`peload/README.md` has the long version, including what the remaining failures
are and why most of them turned out to be host bugs wearing a plug-in's clothes.

## What is in here

    peload/     the loaders and the shims, plus pestudio (Qt6)
    gui/        dwstudio (GTK4)
    c/          the reimplemented engines and the command-line tools
    patches/    patch banks the hosts and the engines share
    scripts/    the analysis that produced them — Ghidra, Z80, wavetables
    tools/      resource and symbol dumps, arity tables

### Two windows

**pestudio** (`peload/qtgui`, Qt6) is a plug-in browser and player: pick a
folder, pick a plug-in, and get its programs, every exposed parameter, a
playable keyboard, a pitch wheel, patch banks, a recorder, and the plug-in's own
editor — blitted for Windows plug-ins, embedded as an X11 child window for
native Linux ones.

**dwstudio** (`gui/`, GTK4) is the same host beside the engines in `c/src`: a
DW-8000, a 4-op FM synth, a Juno-6 and sample kits, voiced from banks read
straight out of the plug-in binaries rather than extracted to disk first. Both
halves share one audio graph, one keyboard and one MIDI input.

Both host through the same `pehost` API and differ only in toolkit, which is
deliberate: it is what makes a plug-in that misbehaves in one and not the other
worth investigating.

### The engines

`c/` is a from-scratch implementation of the synths, not a wrapper: `dwrender`
renders a preset bank to WAV with no plug-in loaded at all, and `dwplay` plays
one live. `c/README.md` covers them.

## Building

Needs a C11 and C++20 compiler, CMake ≥ 3.16, GTK4, Qt6, ALSA, PipeWire, X11,
Cairo and FreeType. On Arch:

    pacman -S base-devel cmake gtk4 qt6-base alsa-lib pipewire libx11 cairo \
              freetype2

Then:

    ./dw build          # everything, both windows included

or by hand:

    make -C c                                   # engines + the dw launcher
    cmake -S peload -B peload/build && cmake --build peload/build   # peload, pestudio
    cmake -S gui    -B gui/build    && cmake --build gui/build      # dwstudio

## Running

    dw                       open a window — pestudio or dwstudio, whichever
                             matches the desktop (--qt / --gtk to force one)
    dw pe <dir|bank.json>    the Qt window, on a folder or a patch bank
    dw gui                   the GTK window
    dw peload <plug-in>      the same hosts from the command line:
                             --params, --render out.wav, --patch/--pick
    dw play <preset>         the reimplemented engine, no plug-in involved

Plug-in editors embed through an X11 window id, so both windows ask for the X11
backend under Wayland; XWayland is enough. Plug-ins are hosted out of process by
default — a browser loads a hundred of them in a session and one that faults
should not end it.

## What is not in the tree

Deliberately, and listed with reasons in `.gitignore`:

- **The Ghidra workspace and the analysis output.** 150 MB of database,
  decompiler dumps, disassembly and extracted resources, all regenerable by the
  scripts in `scripts/` and `tools/` — and all derived from commercial binaries,
  which is the better reason not to publish it. The written-up conclusions are
  in `FINDINGS.md` and `FINDINGS-FB02.md`.
- **Rendered audio**, reproducible from `patches/`.
- **`runtime/msvcp120.dll`**, a Microsoft redistributable some plug-ins need at
  runtime. `HANDOFF-2026-08-01-msvcp120.md` explains how to put it back.
- **A vendored libc++**, referenced by no build file here.
- **Build directories**, which is where every binary lands.

No plug-in binaries are included. The corpora the windows browse live outside
this tree, under `../windows`, `../linux` and `../macos`.

## Licence

MIT — see `LICENSE`. The plug-ins it loads are their authors' own, and nothing
here redistributes any of them.
