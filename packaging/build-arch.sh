#!/bin/bash
#
# build-arch.sh VERSION -- build the vst-ace package for Arch Linux.
#
# Arch and its derivatives (EndeavourOS, Manjaro, CachyOS): one rolling target,
# so there is no oldest-supported release to write down the way the .deb and
# .rpm have. Run it from anywhere:
#
#   bash packaging/build-arch.sh 0.3.0
#
# The .pkg.tar.zst lands in release/. Build-dependencies:
#
#   bash packaging/install-deps.sh --packaging
#
# or by name: base-devel, cmake, pkgconf, gtk4, qt6-base, alsa-lib, pipewire,
# libx11, cairo, freetype2, desktop-file-utils, rsync.
#
# 32-bit Windows VST2 plug-ins want multilib enabled in /etc/pacman.conf and
# lib32-glibc, lib32-freetype2, lib32-libx11, lib32-pipewire and lib32-alsa-lib
# installed -- `install-deps.sh --i386` asks for those. Without them peload32 is
# skipped and the package is built without it, which is the only difference:
# everything 64-bit works either way.

set -euo pipefail

VERSION="${1:-0.0.0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RELEASE_DIR="$REPO_ROOT/release"
WORK_DIR="$(mktemp -d -t vst-ace-arch-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

mkdir -p "$RELEASE_DIR"

# makepkg refuses to run as root, and is right to: package() would be building
# in a directory it could damage the running system from.
if [[ $EUID -eq 0 ]]; then
    echo "build-arch.sh: run this as an ordinary user, not root --" >&2
    echo "makepkg drops privileges for package() and refuses to start here." >&2
    exit 1
fi

command -v makepkg >/dev/null || {
    echo "build-arch.sh: no makepkg here. This builds an Arch package and" >&2
    echo "needs pacman's base-devel; on Debian use build-deb.sh, on Fedora" >&2
    echo "build-rpm.sh." >&2
    exit 1
}

# The commit, captured here because the staged source below deliberately has no
# .git in it: makepkg builds from an extracted tarball. Without this the About
# box in a packaged build could name a version and a date but not the commit,
# which is the build where knowing it matters most. Empty outside a checkout,
# which is what a release tarball is.
# ${VSTACE_GIT:-...} rather than a plain assignment: a caller that already
# knows the commit -- a CI job, or anything building from a copy made without
# .git -- passes it in, and asking git unconditionally would overwrite that
# with the empty string exactly when it was most needed.
VSTACE_GIT="${VSTACE_GIT:-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)}"
export VSTACE_GIT
[ -n "$VSTACE_GIT" ] && echo "building from commit $VSTACE_GIT"

echo "=== Arch build: vst-ace $VERSION ==="

# makepkg wants the source beside the PKGBUILD, and the tarball's top directory
# has to be <name>-<version> for the cd in build().
SRC_TOP="vst-ace-$VERSION"
mkdir -p "$WORK_DIR/$SRC_TOP"
rsync -a --exclude-from="$SCRIPT_DIR/source-excludes.txt" \
    "$REPO_ROOT/" "$WORK_DIR/$SRC_TOP/"

( cd "$WORK_DIR" && tar czf "$SRC_TOP.tar.gz" "$SRC_TOP" && rm -rf "$SRC_TOP" )
cp "$SCRIPT_DIR/arch/PKGBUILD" "$WORK_DIR/"

# A PKGBUILD's pkgver is a plain assignment and makepkg has no --define, so the
# placeholder is rewritten in the copy -- the same thing build-deb.sh does to
# the changelog and build-rpm.sh to the spec's Version: tag.
sed -i -e "s|^pkgver=.*|pkgver=$VERSION|" "$WORK_DIR/PKGBUILD"

# PKGDEST asks for the finished package in release/; everything else makepkg
# writes -- src/, pkg/, the extracted tarball -- stays in WORK_DIR and goes
# with it. -f overwrites a package of the same version already sitting there,
# which is what rebuilding one means.
export PKGDEST="$RELEASE_DIR"
cd "$WORK_DIR"
makepkg -f

# Asking makepkg where the package went rather than assuming PKGDEST won.
# makepkg sources /etc/makepkg.conf and then ~/.makepkg.conf after it starts,
# and a PKGDEST in either of those overwrites the one exported above -- so on a
# machine that keeps its packages somewhere else the build succeeds and the
# glob below finds nothing, which reads as a failed build. --packagelist runs
# the same configuration and names the file wherever it actually landed.
pkgs=()
while IFS= read -r built; do
    [[ -f "$built" ]] || continue
    if [[ "$(dirname "$built")" != "$RELEASE_DIR" ]]; then
        echo "makepkg.conf sent the package to $(dirname "$built") -- moving it"
        mv -f "$built" "$RELEASE_DIR/"
        built="$RELEASE_DIR/$(basename "$built")"
    fi
    pkgs+=("$built")
done < <(makepkg --packagelist 2>/dev/null || true)

# A makepkg too old for --packagelist. The glob is what this used to do.
if [[ ${#pkgs[@]} -eq 0 ]]; then
    shopt -s nullglob
    pkgs=("$RELEASE_DIR"/vst-ace-"$VERSION"-*.pkg.tar.*)
    shopt -u nullglob
fi

if [[ ${#pkgs[@]} -eq 0 ]]; then
    echo "ERROR: no package produced by makepkg" >&2
    exit 1
fi

echo
echo "=== Built ==="
ls -lh "${pkgs[@]}"
echo
echo "release/ is not tracked by git. Publish it on the releases page:"
echo "  gh release upload v$VERSION ${pkgs[0]}"
echo
# Same reason as the other two scripts: say how to install it where it is
# downloaded. pacman -U is the whole answer here -- unlike apt and dnf it takes
# a file path directly, and it resolves the dependencies from the repositories
# on its own.
echo "Install with:"
echo "  sudo pacman -U ${pkgs[0]}"
echo
if bsdtar -tf "${pkgs[0]}" usr/lib/vst-ace/peload32 &>/dev/null; then
    echo "peload32 is in this package: 32-bit Windows VST2 plug-ins will load."
else
    echo "peload32 is NOT in this package -- it was built without multilib, so"
    echo "32-bit Windows VST2 plug-ins will not load. To include it, enable"
    echo "multilib in /etc/pacman.conf, then:"
    echo "  bash packaging/install-deps.sh --i386 && bash packaging/build-arch.sh $VERSION"
fi
