# Installing and building vst-ace

The README has the short version of both. This is the rest of it: what the
packages contain, where the Microsoft runtime DLLs go, how to turn on 32-bit
plug-in support, and what the checks cover.

## Installing

Packages are on the
[releases page](https://github.com/spacestate1/VST-ace/releases) — a `.deb` for
Ubuntu 24.04 and Debian 13 or newer, an `.rpm` for Fedora 40 or newer, and a
`.pkg.tar.zst` for Arch:

    sudo apt install ./vst-ace_0.3.0-1_amd64.deb         # Debian, Ubuntu
    sudo dnf install ./vst-ace-0.3.0-1.fc43.x86_64.rpm   # Fedora
    sudo pacman -U ./vst-ace-0.3.0-1-x86_64.pkg.tar.zst  # Arch

`apt install`, not `dpkg -i`. The leading `./` is what makes apt read the
argument as a file rather than a package name, and apt is what pulls in GTK 4,
Qt 6, PipeWire and the rest -- fourteen dependencies, all stock. `dpkg -i`
unpacks the one file and stops, leaving the package unconfigured and printing
the libraries it could not find; `sudo apt install -f` finishes that off.

`dnf install ./` reads a path for the same reason, and resolves the same way.
`pacman -U` is the one that takes a file without being told: it reads a path
either way and pulls the dependencies from the repositories.

That puts `va`, `pestudio` and `dwstudio` on `$PATH`, the patch banks in
`/usr/share/vst-ace/patches`, and both windows in the desktop menu.

### The AppImage

One file, no install, no root, every distribution:

    chmod +x vst-ace-0.3.0-x86_64.AppImage
    ./vst-ace-0.3.0-x86_64.AppImage

It carries GTK 4, Qt 6 and everything under them, and borrows the host's glibc,
which is the one thing an AppImage cannot bundle. glibc is backward compatible
and not forward compatible, so the file runs on the release it was built on and
everything newer: built on Ubuntu 24.04, it wants glibc 2.39 and runs on Ubuntu
24.04, Debian 13, Fedora 40 and Arch alike.

The command line is inside it too. An AppImage takes the name it was invoked
by, so a symlink is how you reach the other programs:

    ln -s vst-ace-0.3.0-x86_64.AppImage peload
    ./peload plug.dll --render out.wav

`peload`, `peserve`, `pestudio`, `dwstudio` and `peload32` all answer to that.

Two things are different from a package. `peload32` is in the AppImage but asks
the host for `/lib/ld-linux.so.2` and the 32-bit libraries beside it before any
of the AppImage's own environment applies, so 32-bit Windows plug-ins work only
where an i386 runtime is installed — and say so plainly where it is not. And
`runtime/` and `runtime32/` are inside a read-only image, so a Microsoft DLL
cannot be dropped in beside them; `PELOAD_DLL_PATH` is the way in:

    PELOAD_DLL_PATH=~/.local/share/vst-ace/runtime32 ./vst-ace-0.3.0-x86_64.AppImage

Every package hosts 64-bit plug-ins. **32-bit Windows VST2** plug-ins are
loaded by `peload32`, the i386 helper the 64-bit hosts bridge to out of
process; the `.deb` and the Arch package carry it. Its runtime libraries are
`Suggests` / `optdepends` rather than hard dependencies, so the package
installs on a machine that has never enabled a foreign architecture and
`peload32` says what is missing rather than failing obscurely. To turn it on:

    # Debian, Ubuntu
    sudo dpkg --add-architecture i386 && sudo apt update
    sudo apt install libx11-6:i386 libfreetype6:i386 \
                     libasound2t64:i386 libpipewire-0.3-0t64:i386

    # Arch -- multilib enabled in /etc/pacman.conf first
    sudo pacman -S lib32-glibc lib32-freetype2 lib32-libx11 \
                   lib32-pipewire lib32-alsa-lib

The Arch package is built with `peload32` in it only if those `lib32-` packages
were present on the machine that built it; `packaging/build-arch.sh` says which
it produced when it finishes. There is no equivalent in the `.rpm`: Fedora
carries too little of a 32-bit development stack to build the helper against,
so on Fedora 32-bit plug-ins still want a build from source.

The Microsoft C++ runtime is reimplemented rather than required at 64-bit.
Every symbol the plug-in corpus imports from `msvcp120.dll` is provided
natively, and the plug-ins that used to need it -- NI Absynth 5, FM8, Kontakt 5
and Massive -- render byte-identical audio with no such DLL present. The
`runtime/` directory can stay empty.

`runtime32/` is a different matter: the reimplementation is 64-bit only, so the
four 32-bit Native Instruments plug-ins still want Microsoft's `msvcp120.dll`
and `msvcr120.dll` there. Everything else in the corpus loads at both widths
with both directories empty.

    /usr/lib/vst-ace/runtime/      x86-64 DLLs -- nothing in the corpus needs these
    /usr/lib/vst-ace/runtime32/    i386 DLLs -- msvcp120.dll and msvcr120.dll,
                                   for the 32-bit NI plug-ins

On Fedora that is `/usr/lib64/vst-ace`, which is where `%{_libdir}` puts the
programs and where `va` was compiled to look for them.

Both are searched next to the installed binaries, so there is nothing to
configure; `PELOAD_DLL_PATH` overrides the search if the DLLs live somewhere
else, and a real DLL is preferred over the built-in implementation wherever one
is found. Wine's builds of the same DLLs are refused rather than loaded: they
are compiled against Wine's own `ntdll` and fault inside their own startup.

Some plug-ins also want their own factory content, which is a separate thing
from a runtime and is not ours to ship. NI Massive is the one in the corpus:
`tables.dat` from its installer goes in
`~/.peload/Program Files (x86)/Common Files/Native Instruments/Massive/` for the
64-bit build and `~/.peload/AppData/Roaming/Native Instruments/Massive/` for the
32-bit one. Without it the plug-in loads nothing and says so in the log.

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

`packaging/build-deb.sh <version>`, `build-rpm.sh`, `build-arch.sh` and
`build-appimage.sh` build them into `release/`, from the recipes in
`packaging/debian/`, `packaging/rpm/`, `packaging/arch/` and
`packaging/appimage/`. Build each on the distribution it is for — a package
built against one release's Qt and GTK will not run on another's, and the
AppImage on the oldest release you mean to support — and publish them on the
releases page rather than committing them, which is why `release/` is not
tracked.

The AppImage build wants four tools that are not distribution packages:
`linuxdeploy`, its Qt and GTK plugins, and `appimagetool`. It fetches them into
`~/.cache/vst-ace/appimage-tools` on first use, or takes them from
`$VSTACE_APPIMAGE_TOOLS` if you would rather supply them yourself.

All four stage a copy of this tree with rsync and build from that, so what the
package is built from is what a source tarball would hold rather than whatever
this working copy happens to have lying in it. The exclusions are one shared
list, `packaging/source-excludes.txt`; `tools/regress.py` checks that every
script reads it, that none has grown an exclusion of its own, and that what
rsync would actually stage holds nothing that is not ours to hand on.

## Using the plug-ins in a DAW

`libvst-ace-bridge.so` is this host with the sides swapped: a Linux VST2 a DAW
loads on a track, which then loads the Windows, macOS or Classic plug-in
behind it. Every Linux DAW already knows how to load a Linux VST2, and none of
them is ever going to load a `.dll`.

`./va build` builds it to `peload/build/libvst-ace-bridge.so`. The packages do
not install it yet -- it is new, and where a distribution should put a bridge
like this is not a decision to make in a hurry -- so for now it is copied out
of the build tree by hand.

Install one copy per plug-in, named after the plug-in, with the real thing
beside it:

    cp peload/build/libvst-ace-bridge.so  ~/.vst/FB-3300.so
    cp /path/to/FB-3300.dll               ~/.vst/

The copy finds its own file name through `dladdr`, drops the `.so`, and looks
for `FB-3300.dll`, `.vst3`, `.vst` or `.component` beside it. A symlink works
as well as a copy, which is the tidier way to install a lot of them.

For a plug-in that cannot move -- one that loads data from its own folder, or
one you would rather leave where its installer put it -- write the path into a
`.vstace` file instead:

    ln -s .../libvst-ace-bridge.so  ~/.vst/TripleCheese.so
    echo /opt/u-he/TripleCheese/TripleCheese.64.so > ~/.vst/TripleCheese.vstace

`VSTACE_TARGET` overrides both and is the quickest way to try one without
installing anything.

What crosses the bridge: audio, MIDI placed to the sample within the block,
parameters, programs, the DAW's tempo and transport, the plug-in's opaque
state -- so a session recalls it -- and the editor, embedded through the X11
window the DAW provides. A bridged plug-in renders bit-identical audio to the
same plug-in loaded directly; that is checked over the whole corpus.

A plug-in that will not load still produces a working effect that passes
silence and names the problem on stderr, rather than failing the load. A DAW
that cannot load a plug-in usually drops it from the track and takes the
routing with it, which is a worse thing to discover in a session than silence.

## Crash containment

A plug-in is hosted in a `peserve` helper process by default. One that faults
then costs a line of text -- `the helper crashed (signal 11)` -- instead of
the host, which matters most while browsing a folder of plug-ins you have
never opened before.

It is not much of a trade: across the corpus the helper renders identical
audio, captures editors at identical size, and adds about 30 ms to a load.
Three plug-ins that abort inside the host process work perfectly in a helper.

    PEHOST_ISOLATE=0    everything in this process, as it used to be
    PEHOST_ISOLATE=1    the helper, always (the default)

Native Linux plug-ins are always in-process: isolating one would cost it its
editor, which is an X11 window in this process.

## Building

Needs a C11 and C++20 compiler, CMake ≥ 3.16, GTK4, Qt6, ALSA, PipeWire, X11,
Cairo and FreeType.

    bash packaging/install-deps.sh

installs all of them through whichever of apt, dnf, yum, pacman or zypper this
machine has — `--dry-run` prints the command instead, `--packaging` adds the
tools that build a `.deb`, an `.rpm` or an Arch package, and `--i386` adds the
32-bit libraries the i386 loader wants. By hand instead:

**Ubuntu / Debian** — 24.04 or newer, which is where Qt 6 and GTK 4 are recent
enough to be worth using:

    sudo apt install build-essential cmake pkg-config \
        libgtk-4-dev qt6-base-dev \
        libasound2-dev libpipewire-0.3-dev \
        libx11-dev libcairo2-dev libfreetype-dev

On 22.04 the same packages exist but GTK 4 is old enough that `dwstudio` is worth
skipping; `pestudio` still builds. If `libfreetype-dev` is not found, it is
`libfreetype6-dev` on the older releases.

**Fedora** — 40 or newer, for the same reason:

    sudo dnf install gcc gcc-c++ make cmake pkgconf-pkg-config \
        gtk4-devel qt6-qtbase-devel \
        alsa-lib-devel pipewire-devel \
        libX11-devel cairo-devel freetype-devel

**Arch:**

    pacman -S base-devel cmake gtk4 qt6-base alsa-lib pipewire libx11 cairo \
              freetype2

Then, on any of them:

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
