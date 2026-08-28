#!/usr/bin/env bash
# Run the FB-7999 extractor across Full Bucket's whole catalogue.
#
# Every plugin in windows/VST2-64 is the same iPlug2 + Skia build by the same
# developer, so fbextract works on all of them unmodified: the resource layout,
# the FXB preset banks and the 'tffp' chunk are identical throughout. Only the
# four-character plugin id and the parameter count differ.
#
#   ./sweep_catalogue.sh [outdir]
#
# Prints one line per data resource with its detected format.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
vst="$(cd "$here/../.." && pwd)"
out="${1:-${TMPDIR:-/tmp}/fb_sweep}"
fbextract="$vst/re/c/build/fbextract"

[[ -x "$fbextract" ]] || make -C "$vst/re/c" --no-print-directory >/dev/null

rm -rf "$out"; mkdir -p "$out"

echo "=== framework check ==="
for f in "$vst"/windows/VST2-64/*.dll; do
  pdb=$(strings -a "$f" | grep -m1 -oE '[A-Z]:\\[^ ]*\.pdb' || echo "-")
  printf "%-22s %s\n" "$(basename "$f")" "$(basename "$pdb")"
done

echo
echo "=== extracting ==="
n=0
for f in "$vst"/windows/VST2-64/*.dll; do
  b=$(basename "$f" 64.dll)
  if "$fbextract" resources "$f" "$out/$b" >/dev/null 2>&1; then n=$((n+1))
  else echo "  FAILED: $b"; fi
done
echo "  $n plugins extracted -> $out"

echo
echo "=== data resources ==="
python3 - "$out" <<'PY'
import os, struct, sys
import numpy as np

root = sys.argv[1]
SKIP = {"PNG","TTF","JPG","VERSION","MANIFEST","ICON","GROUP_ICON",
        "DIALOG","MENU","ACCELERATOR"}

print(f"{'plugin':<14} {'resource':<22} {'bytes':>9}  format")
for plug in sorted(os.listdir(root)):
    for typ in sorted(os.listdir(os.path.join(root, plug))):
        if typ in SKIP:
            continue
        for name in sorted(os.listdir(os.path.join(root, plug, typ))):
            b = open(os.path.join(root, plug, typ, name), "rb").read()
            if b[:4] == b"CcnK":
                pid  = b[16:20].decode("latin1")
                npr  = struct.unpack_from(">I", b, 24)[0]
                fmt  = f"FXB id={pid!r} nprog={npr} chunk={b[160:164].decode('latin1')!r}"
            else:
                # Vintage ROMs and wavetables here are int16 PCM, not float32;
                # a float32 read produces absurd exponents, which is the tell.
                f32 = np.frombuffer(b[:len(b)//4*4], dtype="<f4")
                if len(f32) and np.isfinite(f32).all() and np.abs(f32).max() < 10:
                    fmt = f"raw float32 x{len(f32)}"
                else:
                    i16 = np.frombuffer(b[:len(b)//2*2], dtype="<i2")
                    fmt = f"raw int16 x{len(i16)}"
                    if len(i16) % 2048 == 0:
                        fmt += f"  = {len(i16)//2048} x 2048-sample cycles"
            print(f"{plug:<14} {typ+'/'+name:<22} {len(b):>9}  {fmt}")
PY
