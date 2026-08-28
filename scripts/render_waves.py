#!/usr/bin/env python3
"""Render FB-7999's WAVEDST harmonic tables into single-cycle audio.

Outputs:
  re/out/waves/dw_wave_NN.wav   32 individual single-cycle WAVs (2048 samples)
  re/out/waves/dw_wavetable.wav one 32-frame Serum-style wavetable that
                                Surge XT / Vital / Bitwig import directly
"""
import struct, wave
from pathlib import Path
import numpy as np

FRAME = 2048          # samples per single cycle; Serum/Surge convention
SRC = Path("re/out/resources/.rsrc/DSTDATA/WAVEDST")
OUT = Path("re/out/waves"); OUT.mkdir(parents=True, exist_ok=True)

raw = np.frombuffer(SRC.read_bytes(), dtype="<f4")
NW, NH = 32, len(raw) // 32
tables = raw.reshape(NW, NH)
print(f"{NW} waves x {NH} harmonics")

def synth(amps, n=FRAME, phase="sine"):
    """Additive resynthesis, band-limited to Nyquist for this frame size.

    numpy's irfft reconstructs
        x[t] = (1/n)(... + 2*sum_k (Re X[k] cos - Im X[k] sin))
    so putting -1j*a_k in bin k yields sum a_k*sin(k.theta), i.e. sine phase.
    Sine phase is what makes wave 0 come out as a true ramp; the stored signs
    then encode 0 vs pi flips per harmonic.
    """
    spec = np.zeros(n // 2 + 1, dtype=complex)
    usable = min(len(amps), n // 2)
    a = amps[:usable].astype(float)
    spec[1:usable + 1] = (-1j * a) if phase == "sine" else a
    return np.fft.irfft(spec, n)

def write_wav(path, data, sr=44100):
    peak = np.max(np.abs(data))
    if peak > 0:
        data = data / peak * 0.98
    pcm = (data * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(pcm.tobytes())

cycles = []
for i in range(NW):
    cyc = synth(tables[i])
    cycles.append(cyc)
    write_wav(OUT / f"dw_wave_{i:02d}.wav", cyc)
    rms = float(np.sqrt(np.mean(cyc ** 2)))
    nz = int(np.sum(np.abs(tables[i]) > 1e-6))
    print(f"  wave {i:2d}: {nz:3d} harmonics, rms={rms:.4f} -> dw_wave_{i:02d}.wav")

# one long file: 32 frames back to back = importable wavetable
write_wav(OUT / "dw_wavetable.wav", np.concatenate(cycles))
print(f"\nwrote {NW} single cycles + dw_wavetable.wav "
      f"({NW * FRAME} samples, {NW} frames of {FRAME})")

# --- verification: do the known waves match their ideal shapes? ---
def norm(x):
    x = x - x.mean()
    return x / (np.linalg.norm(x) or 1.0)

t = np.arange(FRAME) / FRAME
ideals = {
    "sawtooth": 2.0 * (t - 0.5),
    "square":   np.sign(np.sin(2 * np.pi * t)),
    "sine":     np.sin(2 * np.pi * t),
}
print("\n--- shape verification (best |correlation| over circular shift) ---")
for wi, expect in ((0, "sawtooth"), (25, "square"), (27, "sine")):
    a = norm(cycles[wi])
    b = norm(ideals[expect])
    # circular cross-correlation via FFT, phase-invariant match
    cc = np.fft.irfft(np.fft.rfft(a) * np.conj(np.fft.rfft(b)), FRAME)
    r = float(np.max(np.abs(cc)))
    print(f"  wave {wi:2d} vs ideal {expect:8s}: r = {r:.4f}"
          f"   {'MATCH' if r > 0.97 else 'no match'}")
