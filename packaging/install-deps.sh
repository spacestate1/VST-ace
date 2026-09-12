#!/bin/bash
#
# install-deps.sh -- install everything needed to build this tree, on whichever
# of apt, dnf/yum, pacman or zypper this machine has.
#
#   bash packaging/install-deps.sh            build dependencies
#   bash packaging/install-deps.sh --packaging  ... and the tools that build a
#                                               distribution package
#   bash packaging/install-deps.sh --i386     ... and the 32-bit libraries the
#                                               i386 loader needs
#   bash packaging/install-deps.sh --dry-run  print the command, install nothing
#
# What is being installed, and why:
#
#   a C11 and C++20 compiler, cmake, pkg-config   the build itself
#   GTK 4                     dwstudio
#   Qt 6 (Widgets)            pestudio
#   ALSA                      MIDI in, and audio out where there is no PipeWire
#   PipeWire                  audio out
#   X11                       a plug-in's own editor embeds as an X11 child
#   FreeType                  glyph lookup for the DirectWrite shim
#   Cairo                     dwstudio's keyboard and meters
#
# Ubuntu 24.04 or newer, Debian 13 or newer, Fedora 39 or newer, and Arch are
# what the package names below are drawn from. Older releases mostly work with
# the same names -- see the note about libfreetype6-dev at the end.

set -euo pipefail

WANT_PACKAGING=0
WANT_I386=0
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --packaging) WANT_PACKAGING=1 ;;
        --i386)      WANT_I386=1 ;;
        --dry-run|-n) DRY_RUN=1 ;;
        -h|--help)   sed -n '2,30p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------- the manager

if   command -v apt-get &>/dev/null; then MGR=apt
elif command -v dnf     &>/dev/null; then MGR=dnf
elif command -v yum     &>/dev/null; then MGR=yum
elif command -v pacman  &>/dev/null; then MGR=pacman
elif command -v zypper  &>/dev/null; then MGR=zypper
else
    echo "install-deps.sh: no apt, dnf, yum, pacman or zypper here." >&2
    echo "Install by hand: a C11/C++20 compiler, cmake, pkg-config, GTK 4," >&2
    echo "Qt 6 Widgets, ALSA, PipeWire, X11, FreeType and Cairo development" >&2
    echo "packages." >&2
    exit 1
fi

# Root, without demanding it: sudo when it is there and we are not already
# root, and a clear message when neither is true.
SUDO=""
if [[ $EUID -ne 0 ]]; then
    if command -v sudo &>/dev/null; then SUDO="sudo"
    elif [[ $DRY_RUN -eq 0 ]]; then
        echo "install-deps.sh: not root and no sudo -- run this as root, or" >&2
        echo "with --dry-run to print the command and run it yourself." >&2
        exit 1
    fi
fi

# ------------------------------------------------------------- the package set

case "$MGR" in
apt)
    PKGS=(build-essential cmake pkg-config
          libgtk-4-dev qt6-base-dev
          libasound2-dev libpipewire-0.3-dev
          libx11-dev libcairo2-dev libfreetype-dev)
    # curl for build-appimage.sh, which fetches linuxdeploy and
    # appimagetool itself: they are not distribution packages anywhere.
    PACKAGING=(debhelper devscripts dpkg-dev fakeroot lintian rsync curl)
    # Ubuntu and Debian carry only a small i386 set, and it has to be asked
    # for. This list is also what packaging/debian/control build-depends on to
    # get peload32 -- keep the two in step, or dpkg-checkbuilddeps stops the
    # .deb build before it starts. What each one costs when it is absent is
    # written up there.
    I386=(gcc-multilib g++-multilib libfreetype-dev:i386 libx11-dev:i386
          libasound2-dev:i386 libpipewire-0.3-dev:i386)
    INSTALL=($SUDO apt-get install -y "${PKGS[@]}")
    ;;
dnf|yum)
    PKGS=(gcc gcc-c++ make cmake pkgconf-pkg-config
          gtk4-devel qt6-qtbase-devel
          alsa-lib-devel pipewire-devel
          libX11-devel cairo-devel freetype-devel)
    # desktop-file-utils because the spec BuildRequires it and runs
    # desktop-file-validate in %check -- without it rpmbuild stops after the
    # build, at the last step before the package.
    PACKAGING=(rpm-build rpmdevtools rsync desktop-file-utils curl)
    I386=(glibc-devel.i686 libstdc++-devel.i686 freetype-devel.i686
          libX11-devel.i686 alsa-lib-devel.i686 pipewire-devel.i686)
    INSTALL=($SUDO "$MGR" install -y "${PKGS[@]}")
    ;;
pacman)
    PKGS=(base-devel cmake pkgconf
          gtk4 qt6-base
          alsa-lib pipewire
          libx11 cairo freetype2)
    # makepkg and fakeroot come with base-devel, which is in PKGS already.
    # desktop-file-utils is what the PKGBUILD's check() runs; namcap is
    # optional and says what an Arch reviewer would. dpkg and rpm-tools read a
    # foreign package here -- they cannot build one that would run elsewhere,
    # since it would link against Arch's own glibc, Qt and GTK.
    # curl is what build-appimage.sh downloads appimagetool with. pacman
    # depends on it, so it is always already here -- named anyway, so that
    # the list says what the scripts need rather than what the package
    # manager happens to have dragged in.
    PACKAGING=(rsync desktop-file-utils namcap dpkg rpm-tools curl)
    # multilib has to be enabled in /etc/pacman.conf for these.
    I386=(lib32-glibc lib32-gcc-libs lib32-freetype2 lib32-libx11
          lib32-alsa-lib lib32-pipewire)
    INSTALL=($SUDO pacman -S --needed --noconfirm "${PKGS[@]}")
    ;;
zypper)
    PKGS=(gcc gcc-c++ make cmake pkg-config
          gtk4-devel qt6-base-devel
          alsa-devel pipewire-devel
          libX11-devel cairo-devel freetype2-devel)
    # Same spec as Fedora, so the same two extras: desktop-file-utils for the
    # desktop-file-validate the spec BuildRequires and runs in %check, and
    # curl for build-appimage.sh.
    PACKAGING=(rpm-build rsync desktop-file-utils curl)
    I386=(glibc-devel-32bit freetype2-devel-32bit libX11-devel-32bit
          alsa-devel-32bit)
    INSTALL=($SUDO zypper install -y "${PKGS[@]}")
    ;;
esac

[[ $WANT_PACKAGING -eq 1 ]] && INSTALL+=("${PACKAGING[@]}")
[[ $WANT_I386      -eq 1 ]] && INSTALL+=("${I386[@]}")

# ------------------------------------------------------------------- doing it

echo "=== $MGR ==="
printf '%s ' "${INSTALL[@]}"; echo

if [[ $DRY_RUN -eq 1 ]]; then
    exit 0
fi

case "$MGR" in
apt)
    if [[ $WANT_I386 -eq 1 ]]; then
        $SUDO dpkg --add-architecture i386
    fi
    $SUDO apt-get update
    ;;
pacman)
    $SUDO pacman -Sy
    ;;
esac

"${INSTALL[@]}"

echo
echo "=== Done. Build with: ./va build ==="
if [[ "$MGR" == "apt" ]]; then
    echo "(On releases before Debian 13 / Ubuntu 24.04, libfreetype-dev is"
    echo " named libfreetype6-dev, and GTK 4 is old enough that dwstudio is"
    echo " worth skipping -- pestudio still builds.)"
fi
