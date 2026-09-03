# Installing and building vst-ace

The README has the short version of both. This is the rest of it: what the
packages contain, where the Microsoft runtime DLLs go, how to turn on 32-bit
plug-in support, and what the checks cover.

## Installing

Packages are on the
[releases page](https://github.com/spacestate1/VST-ace/releases) — a `.deb` for
Ubuntu 24.04 and Debian 13 or newer, and an `.rpm` for Fedora 40 or newer:

    sudo apt install ./vst-ace_0.2.0-1_amd64.deb        # Debian, Ubuntu
    sudo dnf install ./vst-ace-0.2.0-1.fc43.x86_64.rpm  # Fedora

`apt install`, not `dpkg -i`. The leading `./` is what makes apt read the
argument as a file rather than a package name, and apt is what pulls in GTK 4,
Qt 6, PipeWire and the rest -- fourteen dependencies, all stock. `dpkg -i`
unpacks the one file and stops, leaving the package unconfigured and printing
the libraries it could not find; `sudo apt install -f` finishes that off.

That puts `va`, `pestudio` and `dwstudio` on `$PATH`, the patch banks in
`/usr/share/vst-ace/patches`, and both windows in the desktop menu.

Either package hosts 64-bit plug-ins. **32-bit Windows VST2** plug-ins are
loaded by `peload32`, the i386 helper the 64-bit hosts bridge to out of
process; the `.deb` carries it. Its runtime libraries are `Suggests` rather
than `Depends`, so the package installs on a machine that has never enabled a
foreign architecture and `peload32` says what is missing rather than failing
obscurely. To turn it on:

    sudo dpkg --add-architecture i386 && sudo apt update
    sudo apt install libx11-6:i386 libfreetype6:i386 \
                     libasound2t64:i386 libpipewire-0.3-0t64:i386

There is no equivalent in the `.rpm` yet; on Fedora, 32-bit plug-ins still want
a build from source.

The Microsoft C++ runtime is reimplemented rather than required. Every symbol
the plug-in corpus imports from `msvcp120.dll` is provided natively, and the
plug-ins that used to need it -- NI Absynth 5, FM8 and Kontakt 5 -- render
byte-identical audio with no such DLL present. Nothing in the corpus needs
`runtime/` or `runtime32/` any more.

The two directories still ship, empty, as an escape hatch for a plug-in built
against a runtime that is not yet covered -- `msvcp140.dll` is the known gap:

    /usr/lib/vst-ace/runtime/      x86-64 DLLs, for the 64-bit hosts
    /usr/lib/vst-ace/runtime32/    i386 DLLs, for peload32

Anything dropped there is searched next to the installed binaries, so there is
nothing to configure; `PELOAD_DLL_PATH` overrides the search if the DLLs live
somewhere else. A real DLL is preferred over the built-in implementation
wherever one is found. Wine's builds of the same DLLs are refused rather than
loaded: they are compiled against Wine's own `ntdll` and fault inside their own
startup.

An installed copy has no tree to find the plug-in corpus in. Both windows take
the folders to search under **Settings > Plug-in Folders**, each filed under the
platform it holds; the list is kept in `~/.config/vst-ace/plugin-folders` and is
shared, so a folder added in one window is searched by the other. The system VST
directories and `VST_PATH`/`VST3_PATH` are searched without being asked for.

macOS and Mac OS 9 plug-ins are browsed by default and have their own platform
groups: Mach-O VST2, VST3 and Audio Units on one side, and on the other a
`.vstclassic`, which loads, renders and draws its own editor through the
CFM/PEF loader, the PowerPC interpreter and the QuickDraw path.
`-DPESTUDIO_MAC=0` / `-DPLUGVIEW_MAC=0` take the Mach-O family back out of the
browser and `-DPESTUDIO_CLASSIC=0` / `-DPLUGVIEW_CLASSIC=0` drop the Classic
side; the loaders are compiled in either way, and `va peload` on the command
line has always reached them.

For the command-line tools, point `VST_ROOT` at a directory holding `windows/`,
`linux/` and `macos/`, or keep it at `~/vst`, which is where they look by
default. Paths given on the command line work regardless.

`packaging/build-deb.sh <version>` and `packaging/build-rpm.sh <version>` build
them into `release/`, from the recipes in `packaging/debian/` and
`packaging/rpm/`. Build each on the distribution it is for — a package built
against one release's Qt and GTK will not run on another's — and publish them
on the releases page rather than committing them, which is why `release/` is
not tracked.

## Building

Needs a C11 and C++20 compiler, CMake ≥ 3.16, GTK4, Qt6, ALSA, PipeWire, X11,
Cairo and FreeType.

    bash packaging/install-deps.sh

installs all of them through whichever of apt, dnf, yum, pacman or zypper this
machine has — `--dry-run` prints the command instead, `--packaging` adds the
tools that build a `.deb` or an `.rpm`, and `--i386` adds the 32-bit libraries
the i386 loader wants. By hand instead:

**Ubuntu / Debian** — 24.04 or newer, which is where Qt 6 and GTK 4 are recent
enough to be worth using:

    sudo apt install build-essential cmake pkg-config \
        libgtk-4-dev qt6-base-dev \
        libasound2-dev libpipewire-0.3-dev \
        libx11-dev libcairo2-dev libfreetype-dev

On 22.04 the same packages exist but GTK 4 is old enough that `dwstudio` is worth
skipping; `pestudio` still builds. If `libfreetype-dev` is not found, it is
`libfreetype6-dev` on the older releases.

**Arch:**

    pacman -S base-devel cmake gtk4 qt6-base alsa-lib pipewire libx11 cairo \
              freetype2

Then, on either:

    ./va build          # everything, both windows included

or by hand:

    make -C c                                   # engines + the va launcher
    cmake -S peload -B peload/build && cmake --build peload/build   # peload, pestudio
    cmake -S gui    -B gui/build    && cmake --build gui/build      # dwstudio

### Checking

    python3 tools/regress.py

runs everything that can be checked without a plug-in corpus — the tests that
render need plug-in binaries, which are not ours to ship and are not here. What
is left is the ground the expensive bugs have actually come from: the i386 ABI
surface, where a stub declared with the wrong calling convention or a Windows
type declared at the wrong width corrupts a guest's stack silently and only at
32-bit, and the packaging recipes, where a list that has drifted out of step
fails the build on someone else's machine rather than on this one.

Calling conventions and stdcall arities are checked against mingw-w64's i686
import libraries and again against the compiled code, and `tools/check_arity.py`
does that half on its own. Anything the machine cannot answer — no `-m32`
toolchain, no import libraries — is reported as a skip with the reason rather
than passing quietly. `--no-build` reuses `peload/build` instead of configuring
a fresh tree; `--only <check>` runs one.

    python3 tools/triage.py <dir-of-plug-ins>

is the other half, for when plug-ins fail rather than the tree does. One
plug-in that will not load says little — the backtrace lands in guest code, and
the cause is usually an import that resolved to the generic stub long before.
A corpus says a great deal, because the imports only the *failing* plug-ins
need are a short list. It loads every plug-in in a directory and ranks those
imports by how many failures need them.
