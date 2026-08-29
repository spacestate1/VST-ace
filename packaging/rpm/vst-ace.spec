# The version is a placeholder; packaging/build-rpm.sh passes the real one with
# --define, the same way build-deb.sh rewrites the changelog.
%global pkglibdir  %{_libdir}/%{name}
%global pkgdatadir %{_datadir}/%{name}

# This tree is not a candidate for link-time optimization, which Fedora turns on
# by default. peload_cxx_throw_c and peload_ms_badjmp are reached only from
# top-level asm() blocks, so a whole-program pass sees nothing calling them and
# drops both -- it shows up as two undefined references while linking peserve.
# A `used` attribute would fix those two and not the general case: the host
# hands function addresses to guest code, which calls them from PE, Mach-O and
# PEF images LTO never sees, and the next symptom need not be a link error.
%global _lto_cflags %{nil}

# Debug symbols are still packaged; what is turned off here is dwz, the DWARF
# compressor rpm runs over them on the way into the -debuginfo subpackage,
# which does not cope with the guest images and hand-written trampolines this
# host carries. %%{nil} skips that pass outright -- the tunable this used to
# set (%%_dwz_low_mem_die_limit) only moved dwz's low-memory threshold and left
# the pass itself running, so it never had the intended effect.
%global _find_debuginfo_dwz_opts %{nil}

Name:           vst-ace
Version:        0.1.0
Release:        1%{?dist}
Summary:        Run Windows, macOS and Linux audio plug-ins natively, without Wine

License:        MIT
URL:            https://github.com/spacestate1/VST-ace
Source0:        %{name}-%{version}.tar.gz

ExclusiveArch:  x86_64

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  cmake >= 3.16
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  pkgconfig(alsa)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  desktop-file-utils

Suggests:       xorg-x11-server-Xwayland

%description
Audio plug-ins built for Windows, macOS and Linux, loaded and run as native
code on Linux. Not emulation and not Wine: a PE loader with a Win32 subsystem
underneath it, a Mach-O loader with an Objective-C runtime and a software Metal
rasteriser, and a CFM/PEF interpreter for Classic Mac OS -- six hosts sharing
one set of shims.

Two windows are included. pestudio (Qt6) is a plug-in browser and player: pick
a folder, pick a plug-in, and get its programs, every exposed parameter, a
playable keyboard, a pitch wheel, patch banks, a recorder, and the plug-in's
own editor -- blitted for Windows plug-ins, embedded as an X11 child window for
native Linux ones. dwstudio (GTK4) drives the reimplemented engines: a Korg
DW-8000, a 4-op FM synth, a Juno-6 and sample kits.

The command line has the same hosts: dw peload inspects and renders a plug-in
without a window, and dw play runs the DW-8000 engine with no plug-in loaded at
all.

This package hosts 64-bit plug-ins. The i386 loader is not built here, as it
needs a 32-bit toolchain and 32-bit FreeType, X11, ALSA and PipeWire; build
from source for 32-bit Windows plug-ins. No plug-in binaries are included --
the plug-ins these hosts load are their authors' own.

%prep
%autosetup

%build
# Three build systems in one tree: c/ is a plain Makefile, peload/ and gui/ are
# separate CMake projects (gui/ pulls peload/ in for the host library). Each is
# driven explicitly rather than through %%cmake, which assumes one per tree.
#
# `dw` is compiled knowing where it was installed, which is what lets it find
# its helpers with no source tree above it -- see locate_tree() in c/src/dw.c.
%make_build -C c \
    CFLAGS="%{build_cflags} -Wno-format-truncation" \
    LDFLAGS="%{build_ldflags}" \
    DW_PKGDEFS='-DDW_PKGLIBDIR=\"%{pkglibdir}\" -DDW_PKGDATADIR=\"%{pkgdatadir}\"'

cmake -B obj-peload -S peload \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_C_FLAGS="%{build_cflags}" \
    -DCMAKE_CXX_FLAGS="%{build_cxxflags}" \
    -DCMAKE_EXE_LINKER_FLAGS="%{build_ldflags}"
cmake --build obj-peload --parallel

cmake -B obj-gui -S gui \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_C_FLAGS="%{build_cflags}" \
    -DCMAKE_EXE_LINKER_FLAGS="%{build_ldflags}"
cmake --build obj-gui --parallel --target dwstudio

# pestudio and dwstudio are skipped rather than failed when their toolkit is
# missing, so without this a package built without Qt6 or GTK4 would ship
# quietly incomplete.
test -x obj-peload/peload
test -x obj-peload/peserve
test -x obj-peload/pestudio
test -x obj-gui/dwstudio

%install
# The real programs go together in one private directory because that is where
# each of them looks for the others: the bridge finds peserve beside the
# running executable, and /proc/self/exe resolves the symlinks below back here.
install -D -m 0755 obj-peload/peload   %{buildroot}%{pkglibdir}/peload
install -D -m 0755 obj-peload/peserve  %{buildroot}%{pkglibdir}/peserve
install -D -m 0755 obj-peload/pestudio %{buildroot}%{pkglibdir}/pestudio
install -D -m 0755 obj-gui/dwstudio    %{buildroot}%{pkglibdir}/dwstudio
%dir %{pkglibdir}/runtime
%dir %{pkglibdir}/runtime32
%{pkglibdir}/runtime/README
install -D -m 0755 c/build/dw          %{buildroot}%{_bindir}/dw

# Where real Microsoft runtime DLLs go. Empty, because the redistributable is
# not ours to ship; owned by the package, because the host searches next to its
# own binaries and the user should not have to read the source to learn that.
install -d %{buildroot}%{pkglibdir}/runtime
install -d %{buildroot}%{pkglibdir}/runtime32
install -D -m 0644 packaging/runtime-README %{buildroot}%{pkglibdir}/runtime/README

# -r, so the links are relative: an absolute one records the buildroot's idea
# of the path and rpm warns about it. /proc/self/exe resolves either kind back
# to pkglibdir, which is what the helper lookup depends on.
for p in peload pestudio dwstudio; do
    ln -sfr %{buildroot}%{pkglibdir}/$p %{buildroot}%{_bindir}/$p
done

# The patch banks. Only the .json banks -- the generators beside them in the
# tree are for producing more, and need the plug-in corpus to run.
install -d %{buildroot}%{pkgdatadir}/patches
cd patches && find . -name '*.json' -type f \
    -exec install -D -m 0644 '{}' %{buildroot}%{pkgdatadir}/patches/'{}' \;
cd ..

install -D -m 0644 packaging/pestudio.desktop \
    %{buildroot}%{_datadir}/applications/pestudio.desktop
install -D -m 0644 packaging/dwstudio.desktop \
    %{buildroot}%{_datadir}/applications/dwstudio.desktop

for m in dw peload pestudio dwstudio; do
    install -D -m 0644 packaging/$m.1 %{buildroot}%{_mandir}/man1/$m.1
done

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/pestudio.desktop
desktop-file-validate %{buildroot}%{_datadir}/applications/dwstudio.desktop

%files
%license LICENSE
%doc README.md PLUGINS.md
%{_bindir}/dw
%{_bindir}/peload
%{_bindir}/pestudio
%{_bindir}/dwstudio
%dir %{pkglibdir}
%{pkglibdir}/peload
%{pkglibdir}/peserve
%{pkglibdir}/pestudio
%{pkglibdir}/dwstudio
%dir %{pkgdatadir}
%{pkgdatadir}/patches
%{_datadir}/applications/pestudio.desktop
%{_datadir}/applications/dwstudio.desktop
%{_mandir}/man1/dw.1*
%{_mandir}/man1/peload.1*
%{_mandir}/man1/pestudio.1*
%{_mandir}/man1/dwstudio.1*

%changelog
* Sat Aug 29 2026 Connor McRann <cmcrann@protonmail.com> - 0.1.0-1
- Initial release.
