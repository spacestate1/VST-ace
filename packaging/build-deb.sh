#!/bin/bash
#
# build-deb.sh VERSION -- build the vst-ace .deb for Debian/Ubuntu.
#
# Targets Ubuntu 24.04 and Debian 13 (Trixie) and newer, which is where Qt 6
# and GTK 4 are recent enough for both windows. One package serves both: the
# Qt dependencies are declared as alternatives in debian/shlibs.local, because
# the two distributions do not agree on which runtime packages carry Ubuntu's
# "t64" suffix and dpkg-shlibdeps can only see the one it builds on.
# Run it from anywhere:
#
#   bash packaging/build-deb.sh 0.1.0
#
# The .deb lands in release/. Build-dependencies are listed in
# packaging/debian/control; `sudo apt build-dep .` installs them from a
# checkout, or install them by name:
#
#   sudo apt install build-essential debhelper devscripts dpkg-dev cmake \
#       pkg-config libgtk-4-dev qt6-base-dev libasound2-dev \
#       libpipewire-0.3-dev libx11-dev libcairo2-dev libfreetype-dev

set -euo pipefail

VERSION="${1:-0.0.0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RELEASE_DIR="$REPO_ROOT/release"
WORK_DIR="$(mktemp -d -t vst-ace-deb-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

mkdir -p "$RELEASE_DIR"

# The commit, captured here because the staged source below deliberately has no
# .git in it: dpkg-buildpackage wants an exported tree. Without this the About
# box in a packaged build could name a version and a date but not the commit,
# which is the build where knowing it matters most. Empty outside a checkout,
# which is what a release tarball is.
# ${VSTACE_GIT:-...} rather than a plain assignment: a caller that already
# knows the commit -- a CI job, or anything building from a copy made without
# .git -- passes it in, and asking git unconditionally would overwrite that
# with the empty string exactly when it was most needed.
VSTACE_GIT="${VSTACE_GIT:-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)}"
export VSTACE_GIT

# The About boxes are compiled with this, so the number the package is named
# after and the number it reports are the same one.
export VSTACE_VERSION="$VERSION"
[ -n "$VSTACE_GIT" ] && echo "building from commit $VSTACE_GIT"


echo "=== Debian/Ubuntu build: vst-ace $VERSION ==="

# 3.0 (quilt) wants an orig tarball beside a source directory named
# <pkg>-<version>. Stage both.
SRC_TOP="vst-ace-$VERSION"
mkdir -p "$WORK_DIR/$SRC_TOP"
rsync -a \
    --exclude='.git' \
    --exclude='.claude/' \
    --exclude='build/' \
    --exclude='obj-*/' \
    --exclude='release/' \
    --exclude='debian/' \
    --exclude='__pycache__/' \
    --exclude='*.o' \
    --exclude='*.d' \
    --exclude='/va' \
    --exclude='/out/' \
    --exclude='/project/' \
    --exclude='/renders/' \
    --exclude='/runtime/' \
    --exclude='thirdparty/' \
    "$REPO_ROOT/" "$WORK_DIR/$SRC_TOP/"

# The pristine tarball, captured before debian/ goes in.
( cd "$WORK_DIR" && tar czf "vst-ace_$VERSION.orig.tar.gz" "$SRC_TOP" )

# dpkg-buildpackage only ever looks for a directory literally named "debian",
# so the recipe is copied into place rather than kept there.
cp -r "$SCRIPT_DIR/debian" "$WORK_DIR/$SRC_TOP/debian"

# Replace the placeholder version and date. dch when it is available, since it
# gets the formatting right; sed otherwise.
if command -v dch &>/dev/null; then
    ( cd "$WORK_DIR/$SRC_TOP" && \
        EDITOR=true DEBEMAIL="cmcrann@protonmail.com" DEBFULLNAME="Connor McRann" \
        dch -v "${VERSION}-1" -D "stable" "Release ${VERSION}." )
else
    sed -i \
        -e "s|^vst-ace (0\\.0\\.0-1)|vst-ace (${VERSION}-1)|" \
        -e "s|UNRELEASED;|stable;|" \
        -e "s|Sat, 29 Aug 2026 12:00:00 +0000|$(date -R)|" \
        "$WORK_DIR/$SRC_TOP/debian/changelog"
fi

cd "$WORK_DIR/$SRC_TOP"
dpkg-buildpackage -b -us -uc

# peload32 is no longer a package of its own.
#
# It was one because dpkg's architecture model says an i386 binary belongs in
# an i386 package, and debhelper will not cross-build one from an amd64 host,
# so this script assembled it by hand. Correct, and it meant 32-bit support
# only arrived for users who knew to install a second thing.
#
# debian/rules installs it into the main package instead. The cost to a
# 64-bit-only user is 250 KB that never executes; the i386 libraries are
# Suggests rather than Depends, so the package still installs on a host with
# no foreign architecture enabled, and peload32 decides at run time whether it
# can actually start rather than assuming it can.

# The .deb, .changes and .buildinfo land in the parent directory.
shopt -s nullglob
debs=("$WORK_DIR"/*.deb)
shopt -u nullglob

if [[ ${#debs[@]} -eq 0 ]]; then
    echo "ERROR: no .deb produced by dpkg-buildpackage" >&2
    exit 1
fi

mv "$WORK_DIR"/*.deb "$RELEASE_DIR/"
# The detached debug symbols, which debhelper names .ddeb rather than .deb and
# which the glob above therefore misses. Worth keeping: without them a
# backtrace out of an installed copy is addresses and nothing else.
mv "$WORK_DIR"/*.ddeb "$RELEASE_DIR/" 2>/dev/null || true
mv "$WORK_DIR"/*.buildinfo "$RELEASE_DIR/" 2>/dev/null || true
mv "$WORK_DIR"/*.changes "$RELEASE_DIR/" 2>/dev/null || true

echo
echo "=== Built ==="
ls -lh "$RELEASE_DIR"/vst-ace*.deb
echo
echo "release/ is not tracked by git. Publish it on the releases page:"
echo "  gh release create v$VERSION \\"
echo "     $RELEASE_DIR/vst-ace_$VERSION-1_amd64.deb \\"
echo "     --title \"vst-ace $VERSION\" --notes \"Ubuntu 24.04 / Debian 13+, amd64."
echo ""
# The install line goes first, and says which tool. Someone landing on the
# releases page reaches for dpkg -i, which unpacks the file and stops --
# fourteen dependencies unmet and a package left unconfigured. apt is what
# resolves them, and the ./ is what makes it read a path rather than a name.
echo "Install with apt, not dpkg -i -- apt pulls in GTK 4, Qt 6 and the rest:"
echo "  sudo apt install ./vst-ace_$VERSION-1_amd64.deb"
echo "If dpkg -i was already run, sudo apt install -f finishes it."
echo ""
echo "32-bit Windows VST2 plug-ins work once the i386 runtime is present:"
echo "  sudo dpkg --add-architecture i386 && sudo apt update"
echo "  sudo apt install libx11-6:i386 libfreetype6:i386 libasound2t64:i386 \\"
echo "                   libpipewire-0.3-0t64:i386\""
