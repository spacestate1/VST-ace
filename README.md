# Running plug-ins natively, without Wine

Audio plug-ins built for Windows, macOS and Mac OS 9, loaded and run as native
code on Linux. Not emulation and not Wine: a PE loader with a Win32 subsystem
underneath it, a Mach-O loader with an Objective-C runtime and a software Metal
rasteriser, and a CFM/PEF interpreter for Classic — six hosts sharing one set of
shims.

| target | plug-ins | status |
|---|---|---|
| Windows VST2, x86-64 | 40 | 39 load, 36 render, 39 draw their editor |
| Windows VST2, i386 | 40 | 36 load, 34 render, 36 draw their editor |
| Windows VST3, x86-64 | 35 | 35 load, 33 render, 35 draw their editor |
| Linux VST2 (`.so`), x86-64 | 39 | 37 render |
| Linux VST3, x86-64 | 13 | 7 render, X11-embedded editors |
| macOS VST2 / VST3 / AU | 99 | 90 render, editors open and are driven |

`peload/README.md` has the long version, including what the remaining failures
are and why most turned out to be host bugs wearing a plug-in's clothes.
`PLUGINS.md` lists all 270 plug-ins the hosts are tested against.

![FB-7999 in pestudio](docs/fb-7999.png)

*FB-7999, a Korg DW-8000 simulation and the plug-in this started on: a Windows
VST2 running as native code, its editor drawn by the Win32 layer and blitted.*

![Cardinal in pestudio](docs/cardinal.png)

*Cardinal — VCV Rack as a Linux VST3, an OpenGL editor embedded as an X11 child
at 59 fps.*

## What is in here

    peload/     the loaders and the shims, plus pestudio (Qt6)
    gui/        dwstudio (GTK4)
    c/          the reimplemented engines and the command-line tools
    patches/    patch banks the hosts and the engines share
    scripts/    the analysis that produced them — Ghidra, Z80, wavetables
    tools/      resource and symbol dumps, arity tables, the checks
    packaging/  the Debian and Fedora packages, and the dependency installer

**pestudio** (Qt6) and **dwstudio** (GTK4) are the same host in two toolkits:
pick a plug-in, get its programs, parameters, a playable keyboard, patch banks,
a recorder, and the plug-in's own editor. Both go through one `pehost` API, and
differ only in toolkit — which is deliberate, because a plug-in that misbehaves
in one and not the other is worth investigating.

`c/` is a from-scratch implementation of the synths rather than a wrapper:
`dwrender` renders a preset bank to WAV with no plug-in loaded at all.

## Installing

Packages are on the
[releases page](https://github.com/spacestate1/VST-ace/releases):

    sudo apt install ./vst-ace_0.2.0-1_amd64.deb        # Debian 13+, Ubuntu 24.04+
    sudo dnf install ./vst-ace-0.2.0-1.fc43.x86_64.rpm  # Fedora 40+

`apt install ./`, not `dpkg -i` — apt is what pulls in GTK 4, Qt 6 and the rest.

That puts `va`, `pestudio` and `dwstudio` on `$PATH` and both windows in the
desktop menu. 32-bit Windows plug-ins, the Microsoft runtime DLLs a few plug-ins
need, and where the browsers look for plug-ins are all in
[`docs/INSTALL.md`](docs/INSTALL.md).

## Building

    bash packaging/install-deps.sh     # apt, dnf, yum, pacman or zypper
    ./va build                         # everything, both windows included

By hand, the dependencies are a C11 and C++20 compiler, CMake ≥ 3.16, GTK4,
Qt6, ALSA, PipeWire, X11, Cairo and FreeType:

    sudo apt install build-essential cmake pkg-config libgtk-4-dev qt6-base-dev \
        libasound2-dev libpipewire-0.3-dev libx11-dev libcairo2-dev libfreetype-dev
    sudo dnf install gcc gcc-c++ cmake pkgconf gtk4-devel qt6-qtbase-devel \
        alsa-lib-devel pipewire-devel libX11-devel cairo-devel freetype-devel
    pacman -S base-devel cmake gtk4 qt6-base alsa-lib pipewire libx11 cairo freetype2

    python3 tools/regress.py           # everything checkable without plug-ins

## Running

    dw                       open a window — whichever matches the desktop
    va pe <dir|bank.json>    the Qt window, on a folder or a patch bank
    va gui                   the GTK window
    va peload <plug-in>      the same hosts from the command line:
                             --params, --render out.wav, --editor shot.ppm
    va play <preset>         the reimplemented engine, no plug-in involved

Editors embed through an X11 window id, so both windows ask for the X11 backend
under Wayland; XWayland is enough. Plug-ins are hosted out of process by
default: a browser loads a hundred in a session and one that faults should not
end it.

## Licence

MIT — see `LICENSE`. No plug-in binaries are included; the corpora the windows
browse live outside this tree and are their authors' own.
