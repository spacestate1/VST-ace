#!/bin/sh
# The two window tests, with what they need: an ALSA client named aseqdump for
# track 1 to point at, a broadway display for the GTK one, a place for pictures.
#   tracker/test/run-uitests.sh [build-dir [output-dir]]
# Exit status is the number of failed checks.
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../build}
out=${2:-$(mktemp -d)}
song=$here/uitest.trk
fail=0
mkdir -p "$out"

aseqdump -p 14:0 >/dev/null 2>&1 &
dump=$!
trap 'kill $dump $bw 2>/dev/null' EXIT
sleep 0.5

if [ -x "$build/tracker-uitest" ]; then
    echo "== Qt"
    QT_QPA_PLATFORM=offscreen timeout 180 "$build/tracker-uitest" "$song" "$out" >"$out/qt.log" 2>&1
    grep -v picture "$out/qt.log"
    fail=$((fail + $(grep -c '^  FAIL' "$out/qt.log")))
fi
if [ -x "$build/tracker-gtk-uitest" ]; then
    echo "== GTK"
    gtk4-broadwayd :7 >/dev/null 2>&1 &
    bw=$!
    sleep 1
    GDK_BACKEND=broadway BROADWAY_DISPLAY=:7 timeout 180 "$build/tracker-gtk-uitest" "$song" "$out" >"$out/gtk.log" 2>&1
    grep -v picture "$out/gtk.log"
    fail=$((fail + $(grep -c '^  FAIL' "$out/gtk.log")))
fi
echo "pictures in $out"
exit $fail
