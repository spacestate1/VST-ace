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
[ -n "$VSTACE_GIT" ] && echo "building from commit $VSTACE_GIT"


echo "=== Debian/Ubuntu build: vst-ace $VERSION ==="

# 3.0 (quilt) wants an orig tarball beside a source directory named
# <pkg>-<version>. Stage both.
SRC_TOP="vst-ace-$VERSION"
mkdir -p "$WORK_DIR/$SRC_TOP"
rsync -a \
    --exclude='.git' \
    --exclude='build/' \
    --exclude='obj-*/' \
    --exclude='release/' \
    --exclude='debian/' \
    --exclude='__pycache__/' \
    --exclude='*.o' \
    --exclude='*.d' \
    --exclude='/dw' \
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

# --- the i386 add-on package: peload32, packaged by hand --------------------
#
# debhelper only builds the binary packages whose Architecture matches the
# host a plain `dpkg-buildpackage -b` runs on, so an "Architecture: i386"
# stanza in debian/control would just be skipped here rather than cross-built.
# peload32 itself came out fine above regardless: it's an ordinary side-effect
# target of the same peload/CMakeLists.txt build, using the -m32 toolchain and
# 32-bit FreeType/X11 this script's Build-Depends already demanded. So the
# .deb around it is put together directly with dpkg-deb, reusing that binary
# rather than building anything a second time.
PELOAD32="$WORK_DIR/$SRC_TOP/obj-peload/peload32"
if [[ -x "$PELOAD32" ]]; then
    echo
    echo "=== Assembling vst-ace-i386 ==="
    I386_STAGE="$WORK_DIR/i386-stage"
    mkdir -p "$I386_STAGE/DEBIAN" \
             "$I386_STAGE/usr/lib/vst-ace" \
             "$I386_STAGE/usr/share/lintian/overrides" \
             "$I386_STAGE/usr/share/doc/vst-ace-i386"

    # -s: stripped on the way in, which is what dh_strip does for the amd64
    # package above. The unstripped binary stays in obj-peload/ for a backtrace,
    # the same way that package's symbols stay in its .ddeb rather than in the
    # .deb people install.
    install -s -m 0755 "$PELOAD32" "$I386_STAGE/usr/lib/vst-ace/peload32"
    install -m 0644 "$SCRIPT_DIR/debian/copyright" \
        "$I386_STAGE/usr/share/doc/vst-ace-i386/copyright"

    # Debian wants a changelog in every binary package, and this one is
    # assembled too late to get dh_installchangelogs' copy -- so take the same
    # source changelog dpkg-buildpackage just used, under the name and the -9n
    # compression debhelper would have given it. (-n so the gzip header carries
    # no timestamp, which is what keeps the build reproducible.)
    gzip -9n < "$WORK_DIR/$SRC_TOP/debian/changelog" \
        > "$I386_STAGE/usr/share/doc/vst-ace-i386/changelog.Debian.gz"
    chmod 0644 "$I386_STAGE/usr/share/doc/vst-ace-i386/changelog.Debian.gz"

    # Real runtime deps off the binary's own NEEDED list, not a guess.
    # dpkg-shlibdeps insists on a debian/control to run at all, unrelated to
    # the one already staged for dpkg-deb under DEBIAN/ -- give it a throwaway
    # one of its own rather than let it find (and pollute the .deb with) a
    # "debian/" directory sitting inside the package tree itself.
    SHLIBS_TMP="$WORK_DIR/i386-shlibdeps-tmp"
    mkdir -p "$SHLIBS_TMP/debian"
    cat > "$SHLIBS_TMP/debian/control" <<EOF
Source: vst-ace

Package: vst-ace-i386
Architecture: i386
Description: dummy, read by dpkg-shlibdeps only
EOF
    SHLIBS_DEPENDS="$(cd "$SHLIBS_TMP" && dpkg-shlibdeps -O \
        -e "$I386_STAGE/usr/lib/vst-ace/peload32" \
        | sed -n 's/^shlibs:Depends=//p')"
    if [[ -z "$SHLIBS_DEPENDS" ]]; then
        echo "ERROR: dpkg-shlibdeps could not resolve peload32's runtime libraries" >&2
        exit 1
    fi

    cat > "$I386_STAGE/DEBIAN/control" <<EOF
Package: vst-ace-i386
Version: ${VERSION}-1
Section: sound
Priority: optional
Architecture: i386
Maintainer: Connor McRann <cmcrann@protonmail.com>
Depends: vst-ace (= ${VERSION}-1), ${SHLIBS_DEPENDS}
Homepage: https://github.com/spacestate1/VST-ace
Description: 32-bit Windows VST2 loader for vst-ace
 peload32, the i386 PE loader and VST2 host from vst-ace. A process cannot
 host both widths at once, so this is a separate program that vst-ace's
 64-bit hosts run out of process and bridge to when a plug-in turns out to
 be 32-bit.
 .
 Needs vst-ace itself -- the bridge finds peload32 beside the 64-bit
 binaries it already has. Installing this on an amd64 system needs the
 i386 architecture enabled first:
 .
   sudo dpkg --add-architecture i386 && sudo apt update
EOF

    cat > "$I386_STAGE/usr/share/lintian/overrides/vst-ace-i386" <<'EOF'
# Published on the project's own releases page, not the Debian archive -- no
# Debian bug for the initial upload to close.
vst-ace-i386: initial-upload-closes-no-bugs
EOF
    # A heredoc lands under the caller's umask, which on a 002 system is 0664
    # and draws non-standard-file-perm. The installs above all name a mode;
    # this is the one file that has to be told after the fact.
    chmod 0644 "$I386_STAGE/usr/share/lintian/overrides/vst-ace-i386"

    find "$I386_STAGE" -type d -exec chmod 0755 {} +
    dpkg-deb --root-owner-group --build "$I386_STAGE" \
        "$WORK_DIR/vst-ace-i386_${VERSION}-1_i386.deb"
else
    echo "vst-ace-i386 skipped: peload32 was not produced by the build above" >&2
fi

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
echo "release/ is not tracked by git. Publish both on the releases page -- the"
echo "i386 add-on is useless without the amd64 package it depends on, so they"
echo "belong in the same release rather than one trailing the other:"
echo "  gh release create v$VERSION \\"
echo "     $RELEASE_DIR/vst-ace_$VERSION-1_amd64.deb \\"
echo "     $RELEASE_DIR/vst-ace-i386_$VERSION-1_i386.deb \\"
echo "     --title \"vst-ace $VERSION\" --notes \"Ubuntu 24.04 / Debian 13+, amd64."
echo "Install vst-ace-i386 alongside it for 32-bit Windows VST2 plug-ins.\""
