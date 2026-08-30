#!/bin/bash
#
# build-rpm.sh VERSION -- build the vst-ace .rpm for Fedora.
#
# Targets Fedora 40 and newer, which is where Qt 6 and GTK 4 are recent enough
# for both windows. Run it from anywhere:
#
#   bash packaging/build-rpm.sh 0.1.0
#
# The .rpm lands in release/, which is not tracked by git -- publish it on the
# releases page. Build-dependencies come from packaging/install-deps.sh, or by
# name:
#
#   sudo dnf install gcc gcc-c++ make cmake pkgconf-pkg-config rpm-build \
#       rpmdevtools desktop-file-utils gtk4-devel qt6-qtbase-devel \
#       alsa-lib-devel pipewire-devel libX11-devel cairo-devel freetype-devel

set -euo pipefail

VERSION="${1:-0.0.0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Same as build-deb.sh: rpmbuild works from a tarball with no .git, so the
# commit has to be carried in from here or the About box cannot name it.
VSTACE_GIT="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
export VSTACE_GIT
RELEASE_DIR="$REPO_ROOT/release"
WORK_DIR="$(mktemp -d -t vst-ace-rpm-build-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

mkdir -p "$RELEASE_DIR"

echo "=== Fedora build: vst-ace $VERSION ==="

# rpmbuild wants a tree of its own, and %autosetup wants the tarball's top
# directory to be <name>-<version>.
mkdir -p "$WORK_DIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
SRC_TOP="vst-ace-$VERSION"
mkdir -p "$WORK_DIR/$SRC_TOP"
rsync -a \
    --exclude='.git' \
    --exclude='build/' \
    --exclude='obj-*/' \
    --exclude='release/' \
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

( cd "$WORK_DIR" && tar czf "SOURCES/$SRC_TOP.tar.gz" "$SRC_TOP" )
cp "$SCRIPT_DIR/rpm/vst-ace.spec" "$WORK_DIR/SPECS/"

# A spec's Version: tag is not a macro and --define cannot reach it, so the
# placeholder is rewritten in the copy -- the same thing build-deb.sh does to
# the changelog. The date line is left alone: it is a changelog entry, and
# back-dating it to today would claim a history that did not happen.
sed -i -e "s|^Version:.*|Version:        ${VERSION}|" \
    "$WORK_DIR/SPECS/vst-ace.spec"

rpmbuild -bb --define "_topdir $WORK_DIR" "$WORK_DIR/SPECS/vst-ace.spec"

shopt -s nullglob
rpms=("$WORK_DIR"/RPMS/*/*.rpm)
shopt -u nullglob

if [[ ${#rpms[@]} -eq 0 ]]; then
    echo "ERROR: no .rpm produced by rpmbuild" >&2
    exit 1
fi

cp "${rpms[@]}" "$RELEASE_DIR/"

echo
echo "=== Built ==="
ls -lh "$RELEASE_DIR"/vst-ace*.rpm
echo
echo "release/ is not tracked by git. Publish the .rpm on the releases page:"
echo "  gh release upload v$VERSION $RELEASE_DIR/vst-ace-$VERSION-1*.rpm"
