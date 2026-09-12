#!/bin/bash
#
# build-appimage.sh VERSION -- build the vst-ace AppImage: one file, no install,
# every distribution.
#
#   bash packaging/build-appimage.sh 0.3.0
#
# It lands in release/, like the other three.
#
# BUILD ON THE OLDEST TARGET.
#
# An AppImage carries its libraries and borrows the host's glibc, which is the
# one thing it cannot bundle. glibc is backward compatible and not forward
# compatible, so the file runs on the build machine's release and everything
# newer, and on nothing older. Built here on Arch it wants glibc 2.43 and runs
# almost nowhere; built on the ubuntu24.04 VM it wants 2.39 and runs on Ubuntu
# 24.04, Debian 13, Fedora 40 and Arch alike -- the same audience as the three
# packages, plus every distribution none of them targets. Build it there.
#
# What it does not carry:
#
#   peload32 is in the AppImage and needs an i386 glibc, X11 and FreeType on
#   the host, which an AppImage cannot supply: a 32-bit ELF asks the host for
#   /lib/ld-linux.so.2 before anything in here gets a say. It works where those
#   are installed and reports itself unavailable where they are not, which is
#   what the .deb does with its Suggests.
#
#   The runtime/ and runtime32/ directories are inside a read-only image, so
#   the Microsoft DLLs a few plug-ins want cannot be dropped in beside them.
#   PELOAD_DLL_PATH is the way in; the README in there says so.
#
# Needs: everything install-deps.sh installs, plus linuxdeploy, its Qt and GTK
# plugins and appimagetool. Those four are fetched from their release pages
# into ~/.cache/vst-ace/appimage-tools on first use, or taken from
# $VSTACE_APPIMAGE_TOOLS if you would rather supply them yourself.

set -euo pipefail

VERSION="${1:-0.0.0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RELEASE_DIR="$REPO_ROOT/release"
WORK_DIR="$(mktemp -d -t vst-ace-appimage-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

TOOLS_DIR="${VSTACE_APPIMAGE_TOOLS:-${XDG_CACHE_HOME:-$HOME/.cache}/vst-ace/appimage-tools}"

mkdir -p "$RELEASE_DIR" "$TOOLS_DIR"

# The tools are themselves AppImages, and running one needs FUSE. Rather than
# require it on a build machine that may be a VM with no /dev/fuse, ask each to
# unpack itself and run from the unpacked copy. linuxdeploy passes this on to
# the plugins it starts.
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=x86_64

# Do not strip anything.
#
# The Qt plugin carries its own binutils, and that strip is older than the
# linker that built this tree: it stops at "unknown type [0x13] section
# `.relr.dyn'" on the first library it is handed and then fails the whole
# plugin with "Failed to execute deferred operations". Relative relocations are
# the default on Ubuntu 24.04 and Fedora alike, so this is not a local
# peculiarity, and it costs nothing to skip -- the distribution's own libraries
# arrive stripped, and these four programs are built -O2 with no debug info to
# remove.
export NO_STRIP=1

# Which GTK, said explicitly, for the same class of reason the Qt plugin needs
# QMAKE. The GTK plugin auto-detects by running ldd over AppDir/usr/bin, and
# the only thing in there is va, which links ALSA and libm and nothing else:
# dwstudio is in usr/lib/vst-ace, where the other helpers are and where the
# plugin does not look. Left to itself it reports "failed to auto-detect GTK
# version" and stops.
export DEPLOY_GTK_VERSION=4

# Same as the other three build scripts: the staged source has no .git, so the
# commit has to be carried in from here or the About box cannot name it.
VSTACE_GIT="${VSTACE_GIT:-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)}"
export VSTACE_GIT
export VSTACE_VERSION="$VERSION"
[ -n "$VSTACE_GIT" ] && echo "building from commit $VSTACE_GIT"

echo "=== AppImage build: vst-ace $VERSION ==="
echo "    glibc here: $(getconf GNU_LIBC_VERSION 2>/dev/null || echo unknown)"
echo "    the AppImage will not run on anything older"

# ------------------------------------------------------------------ the tools

fetch_tool() {
    local name="$1" url="$2"
    if [ -x "$TOOLS_DIR/$name" ]; then return 0; fi
    if command -v "$name" >/dev/null; then
        ln -sf "$(command -v "$name")" "$TOOLS_DIR/$name"
        return 0
    fi
    echo "fetching $name"
    if ! curl -fsSL -o "$TOOLS_DIR/$name.part" "$url"; then
        echo "build-appimage.sh: could not fetch $name from" >&2
        echo "  $url" >&2
        echo "Put it in $TOOLS_DIR by hand, or point VSTACE_APPIMAGE_TOOLS at" >&2
        echo "a directory holding it, and run this again." >&2
        exit 1
    fi
    mv "$TOOLS_DIR/$name.part" "$TOOLS_DIR/$name"
    chmod +x "$TOOLS_DIR/$name"
}

LD_REL=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous
QT_REL=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous
GTK_RAW=https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master

fetch_tool linuxdeploy-x86_64.AppImage           "$LD_REL/linuxdeploy-x86_64.AppImage"
fetch_tool linuxdeploy-plugin-qt-x86_64.AppImage "$QT_REL/linuxdeploy-plugin-qt-x86_64.AppImage"
fetch_tool linuxdeploy-plugin-gtk.sh             "$GTK_RAW/linuxdeploy-plugin-gtk.sh"
fetch_tool appimagetool-x86_64.AppImage \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

# linuxdeploy finds its plugins on $PATH, by the name linuxdeploy-plugin-<x>.
export PATH="$TOOLS_DIR:$PATH"

# Which Qt, said explicitly.
#
# The Qt plugin picks a qmake off $PATH and believes what it says. On a machine
# with Qt 5 installed beside Qt 6 that is qmake-qt5, and the plugin then goes
# looking for Qt 5 modules in a binary linked against Qt 6, finds none, and
# stops with "Could not find Qt modules to deploy" -- which reads like the Qt
# libraries are missing rather than like it asked the wrong one.
if [ -z "${QMAKE:-}" ]; then
    for q in qmake6 qmake-qt6 /usr/lib/qt6/bin/qmake /usr/lib64/qt6/bin/qmake; do
        if command -v "$q" >/dev/null 2>&1; then QMAKE="$(command -v "$q")"; break; fi
        if [ -x "$q" ]; then QMAKE="$q"; break; fi
    done
fi
if [ -z "${QMAKE:-}" ]; then
    echo "build-appimage.sh: no qmake6 found. The Qt plugin needs it to know" >&2
    echo "which Qt to deploy; it is in qt6-base-dev-tools on Debian and" >&2
    echo "qt6-qtbase-devel on Fedora. Set QMAKE to it if it is somewhere else." >&2
    exit 1
fi
export QMAKE
echo "    qmake: $QMAKE ($("$QMAKE" -query QT_VERSION 2>/dev/null || echo '?'))"

# ----------------------------------------------------------------- the source

SRC="$WORK_DIR/vst-ace-$VERSION"
mkdir -p "$SRC"
rsync -a --exclude-from="$SCRIPT_DIR/source-excludes.txt" "$REPO_ROOT/" "$SRC/"

# ------------------------------------------------------------------ the build

PKGLIBDIR=/usr/lib/vst-ace
PKGDATADIR=/usr/share/vst-ace

cd "$SRC"
# The same defines the packages compile in. They name paths that will not exist
# on the machine that runs this file, and that is fine: locate_tree() looks
# ../lib/vst-ace relative to the running program first and only falls back to
# these. Passing them anyway keeps one code path between the four builds, and
# leaves a sensible answer for a copy extracted into a real prefix.
make -C c -j"$(nproc)" \
    DW_PKGDEFS="-DDW_PKGLIBDIR=\\\"$PKGLIBDIR\\\" -DDW_PKGDATADIR=\\\"$PKGDATADIR\\\""

cmake -B obj-peload -S peload -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build obj-peload --parallel
cmake -B obj-gui -S gui -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build obj-gui --parallel --target dwstudio

# Both windows are skipped rather than failed when their toolkit is missing, so
# insist here, where the missing dependency is still the obvious explanation.
for p in peload peserve pestudio; do test -x "obj-peload/$p"; done
test -x obj-gui/dwstudio

# ----------------------------------------------------------------- the AppDir

APPDIR="$WORK_DIR/AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib/vst-ace" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
         "$APPDIR/usr/share/vst-ace/patches" \
         "$APPDIR/usr/share/doc/vst-ace"

# The four 64-bit programs, in the layout va expects: itself in usr/bin, the
# helpers together in usr/lib/vst-ace, which each of them finds beside the
# running executable.
install -m 0755 obj-peload/peload   "$APPDIR/usr/lib/vst-ace/peload"
install -m 0755 obj-peload/peserve  "$APPDIR/usr/lib/vst-ace/peserve"
install -m 0755 obj-peload/pestudio "$APPDIR/usr/lib/vst-ace/pestudio"
install -m 0755 obj-gui/dwstudio    "$APPDIR/usr/lib/vst-ace/dwstudio"

install -m 0644 "$SCRIPT_DIR/appimage/vst-ace.desktop" \
    "$APPDIR/usr/share/applications/vst-ace.desktop"
install -m 0644 "$SCRIPT_DIR/appimage/vst-ace.png" \
    "$APPDIR/usr/share/icons/hicolor/256x256/apps/vst-ace.png"
install -m 0644 "$SCRIPT_DIR/appimage/vst-ace.svg" \
    "$APPDIR/usr/share/icons/hicolor/scalable/apps/vst-ace.svg"

( cd patches && find . -name '*.json' -type f \
    -exec install -D -m 0644 '{}' "$APPDIR/usr/share/vst-ace/patches/"'{}' \; )
install -m 0644 README.md PLUGINS.md LICENSE "$APPDIR/usr/share/doc/vst-ace/"

# ------------------------------------------------------------- the dependencies

# --deploy-deps-only, not --executable, for the helpers: they have to stay in
# usr/lib/vst-ace, and --executable would move them to usr/bin and break the
# lookup that finds them. va itself is passed as an executable, from the build
# tree rather than the AppDir, so linuxdeploy installs it and resolves it.
#
# peload32 is deliberately not here yet. It is an i386 ELF, and pointing a
# dependency walker at it in a 64-bit AppDir gets 32-bit libraries copied into
# usr/lib on top of their 64-bit namesakes. It goes in after this has run.
"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --executable c/build/va \
    --deploy-deps-only "$APPDIR/usr/lib/vst-ace" \
    --desktop-file "$APPDIR/usr/share/applications/vst-ace.desktop" \
    --icon-file "$SCRIPT_DIR/appimage/vst-ace.png" \
    --custom-apprun "$SCRIPT_DIR/appimage/AppRun" \
    --plugin qt \
    --plugin gtk

# ---------------------------------------------------------- what goes in after

# The i386 loader, if this machine could build one. It asks the host for
# /lib/ld-linux.so.2 and the 32-bit libraries beside it before the AppImage's
# own environment applies, so it works where those are installed and says so
# where they are not.
if [ -x obj-peload/peload32 ]; then
    install -m 0755 obj-peload/peload32 "$APPDIR/usr/lib/vst-ace/peload32"
    echo "  -> peload32 included (needs an i386 runtime on the host to start)"
else
    echo "  -> peload32 skipped: no 32-bit toolchain here, so no 32-bit"
    echo "     Windows VST2 support in this AppImage"
fi

# The drop-in directories, and the README that says what they are for. They are
# read-only in the finished image, which is the whole difference from a
# package: PELOAD_DLL_PATH is how a runtime DLL gets in.
mkdir -p "$APPDIR/usr/lib/vst-ace/runtime" "$APPDIR/usr/lib/vst-ace/runtime32"
{
    cat "$SCRIPT_DIR/runtime-README"
    cat <<'EOF'

In an AppImage this directory is inside a read-only image and nothing can be
put in it. Point PELOAD_DLL_PATH at a directory of your own instead:

  PELOAD_DLL_PATH=~/.local/share/vst-ace/runtime32 ./vst-ace-x86_64.AppImage
EOF
} > "$APPDIR/usr/lib/vst-ace/runtime/README"

# ------------------------------------------------------------------ the image

OUT="$RELEASE_DIR/vst-ace-$VERSION-x86_64.AppImage"
"$TOOLS_DIR/appimagetool-x86_64.AppImage" "$APPDIR" "$OUT"
chmod +x "$OUT"

echo
echo "=== Built ==="
ls -lh "$OUT"
echo
echo "release/ is not tracked by git. Publish it on the releases page:"
echo "  gh release upload v$VERSION $OUT"
echo
echo "Run it with no install, and no root:"
echo "  chmod +x $(basename "$OUT") && ./$(basename "$OUT")"
echo "The command line is in there too, by symlink:"
echo "  ln -s $(basename "$OUT") peload && ./peload plug.dll --render out.wav"
