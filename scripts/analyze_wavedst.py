#!/usr/bin/env python3
"""Probe the structure of FB-7999's WAVEDST resource (the DW wavetable ROM)."""
import struct, sys, math
from pathlib import Path

p = Path(sys.argv[1] if len(sys.argv) > 1 else
         "re/out/resources/.rsrc/DSTDATA/WAVEDST")
d = p.read_bytes()
n = len(d)
print(f"file: {p}  size: {n} bytes")

# --- factorisation, to guess table geometry ---
facs = [k for k in range(1, 4097) if n % k == 0]
print(f"divisors <=4096: {facs}")

print("\n--- first 64 bytes ---")
print(" ".join(f"{b:02x}" for b in d[:64]))
print("--- last 32 bytes ---")
print(" ".join(f"{b:02x}" for b in d[-32:]))

# --- plausible header? read a few leading words both endians ---
print("\n--- leading words ---")
print("u16 LE:", struct.unpack_from("<8H", d, 0))
print("u32 LE:", struct.unpack_from("<4I", d, 0))

# --- byte-value histogram: signed 8-bit audio clusters near 0x80/0x00 ---
hist = [0] * 256
for b in d:
    hist[b] += 1
top = sorted(range(256), key=lambda i: -hist[i])[:8]
print("\nmost common bytes:", [(hex(i), hist[i]) for i in top])
nz = sum(1 for b in d if b)
print(f"nonzero bytes: {nz}/{n} ({100*nz/n:.1f}%)")

def stats(vals):
    lo, hi = min(vals), max(vals)
    mean = sum(vals) / len(vals)
    return lo, hi, mean

for name, fmt, size in (("int8", "b", 1), ("uint8", "B", 1),
                        ("int16le", "<h", 2), ("uint16le", "<H", 2)):
    cnt = n // size
    vals = list(struct.unpack("<" + ("b" if name == "int8" else
                                     "B" if name == "uint8" else
                                     "h" if name == "int16le" else "H") * cnt,
                              d[:cnt * size]))
    lo, hi, mean = stats(vals)
    # zero crossings are a decent "is this audio" signal
    zc = sum(1 for a, b in zip(vals, vals[1:])
             if (a - mean) * (b - mean) < 0)
    print(f"{name:9s} count={cnt:6d} min={lo:7d} max={hi:7d} "
          f"mean={mean:9.2f} zerocross={zc}")

# --- try splitting into equal waves and test for self-similarity ---
print("\n--- candidate geometries (int16) ---")
for nwaves in (8, 16, 32, 64, 128):
    if n % nwaves:
        continue
    chunk = n // nwaves
    print(f"  {nwaves:3d} waves x {chunk:6d} bytes = {chunk//2:5d} int16 samples each")
