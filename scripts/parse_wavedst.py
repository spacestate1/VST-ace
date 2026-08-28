#!/usr/bin/env python3
"""WAVEDST is an array of float32 harmonic amplitudes. Recover the geometry
and dump each waveform's spectrum."""
import struct, json
from pathlib import Path

src = Path("re/out/resources/.rsrc/DSTDATA/WAVEDST")
d = src.read_bytes()
f = list(struct.unpack("<%df" % (len(d) // 4), d))
n = len(f)
print(f"{n} float32 values")
print("first 12:", [round(x, 5) for x in f[:12]])

# A fresh waveform starts where amplitude jumps back up to a large value.
peaks = [i for i, x in enumerate(f) if abs(x) > 0.05]
print(f"\nvalues with |amp| > 0.05: {len(peaks)}")
print("first 20 such indices:", peaks[:20])

# Test each candidate wave-length for "does index 0 of every block hold the
# largest magnitude in that block" -- true for harmonic tables.
print("\n--- geometry test ---")
for nw in (8, 16, 22, 29, 32, 44, 58, 64, 116, 128):
    if n % nw:
        continue
    L = n // nw
    hits = sum(1 for w in range(nw)
               if abs(f[w * L]) == max(abs(v) for v in f[w * L:(w + 1) * L]))
    print(f"  {nw:3d} waves x {L:5d} harmonics -> "
          f"{hits}/{nw} blocks start with their own peak")

# Lock in the winner and dump.
NW, L = 32, n // 32
print(f"\n--- assuming {NW} waves x {L} harmonics ---")
waves = []
for w in range(NW):
    blk = f[w * L:(w + 1) * L]
    nz = [i + 1 for i, v in enumerate(blk) if abs(v) > 1e-6]
    peak = max(abs(v) for v in blk)
    waves.append({"index": w, "harmonics_present": len(nz),
                  "highest_harmonic": nz[-1] if nz else 0,
                  "peak": round(peak, 6),
                  "first8": [round(v, 6) for v in blk[:8]]})
    print(f"wave {w:2d}: {len(nz):4d} nonzero harmonics, "
          f"highest={nz[-1] if nz else 0:4d}, peak={peak:.4f}, "
          f"h1..h6={[round(v,4) for v in blk[:6]]}")

Path("re/out/wavedst_spectra.json").write_text(json.dumps(waves, indent=1))
print("\n-> re/out/wavedst_spectra.json")

# Ratio check on wave 0: is it a sawtooth (a_n = a_1/n)?
blk = f[:L]
print("\nwave 0 harmonic ratios a1/(n*an):")
print([round(blk[0] / ((i + 1) * blk[i]), 4) if abs(blk[i]) > 1e-9 else None
       for i in range(8)])
