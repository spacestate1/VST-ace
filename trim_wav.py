#!/usr/bin/env python3
"""Trim the silence off a recording, leaving just the sound.

    python3 trim_wav.py renders/confirm.wav              # trims in place
    python3 trim_wav.py in.wav out.wav                   # writes elsewhere
    python3 trim_wav.py in.wav --split                   # one file per hit

A take from the recorder starts whenever you pressed the button and ends
whenever you stopped, so the sound is somewhere in the middle with silence
either side. This finds it by level and cuts to it.

Two details that matter more than they look:

  * The cut is not made at the threshold. Starting exactly where the signal
    crosses it clips the very front of the transient, which is the part that
    makes a UI sound read as a click rather than a tone, so a few milliseconds
    of lead are kept. The tail is kept well past the threshold for the same
    reason -- a decay cut at -60 dB ends audibly.
  * Both ends are faded over a couple of milliseconds. Cutting a waveform
    mid-cycle leaves a step, and a step is a click on every playback.

The original is kept alongside as <name>-untrimmed.wav when trimming in place,
because the one thing this must not do is lose a take.
"""
import os, math, struct, sys, wave

LEAD_MS   = 8      # kept before the sound starts
TAIL_MS   = 60     # kept after it falls below the floor
FADE_MS   = 3      # against clicks at the cut
FLOOR_DB  = -60.0  # relative to the file's peak
GAP_MS    = 180    # silence this long separates two hits, for --split


def read(path):
    w = wave.open(path)
    fr, nch, nf = w.getframerate(), w.getnchannels(), w.getnframes()
    raw = w.readframes(nf)
    w.close()
    s = struct.unpack(f"<{len(raw)//2}h", raw)
    return fr, nch, list(s)


def write(path, fr, nch, samples):
    w = wave.open(path, "wb")
    w.setnchannels(nch)
    w.setsampwidth(2)
    w.setframerate(fr)
    w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    w.close()


def envelope(s, nch):
    return [max(abs(x) for x in s[i:i + nch]) for i in range(0, len(s) - nch + 1, nch)]


def spans(env, fr, floor, gap_frames):
    """Runs of sound, as (first, last) frame indices."""
    on = [i for i, v in enumerate(env) if v >= floor]
    if not on:
        return []
    out, start, prev = [], on[0], on[0]
    for i in on[1:]:
        if i - prev > gap_frames:
            out.append((start, prev))
            start = i
        prev = i
    out.append((start, prev))
    return out


def cut(s, nch, fr, a, b):
    lead, tail = int(fr * LEAD_MS / 1000), int(fr * TAIL_MS / 1000)
    a = max(0, a - lead)
    b = min(len(s) // nch - 1, b + tail)
    seg = s[a * nch:(b + 1) * nch]
    fade = min(int(fr * FADE_MS / 1000), len(seg) // nch // 2)
    for i in range(fade):
        g = i / fade
        for c in range(nch):
            seg[i * nch + c] = int(seg[i * nch + c] * g)
            j = len(seg) - (i + 1) * nch + c
            seg[j] = int(seg[j] * g)
    return seg


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    split = "--split" in sys.argv
    if not args:
        print(__doc__)
        return 2
    src = args[0]
    fr, nch, s = read(src)
    env = envelope(s, nch)
    peak = max(env) or 1
    floor = peak * (10 ** (FLOOR_DB / 20))
    runs = spans(env, fr, floor, int(fr * GAP_MS / 1000))
    if not runs:
        print(f"  {src}: nothing above {FLOOR_DB:g} dB -- left alone")
        return 1
    total = len(env) / fr
    print(f"  {src}: {total:.2f}s, peak {20*math.log10(peak/32768):.1f} dBFS, "
          f"{len(runs)} hit(s)")

    if split:
        stem = os.path.splitext(src)[0]
        for n, (a, b) in enumerate(runs, 1):
            seg = cut(s, nch, fr, a, b)
            out = f"{stem}-{n}.wav"
            write(out, fr, nch, seg)
            print(f"    {out}  {len(seg)//nch/fr:.3f}s")
        return 0

    seg = cut(s, nch, fr, runs[0][0], runs[-1][1])
    dst = args[1] if len(args) > 1 else src
    if dst == src:
        keep = os.path.splitext(src)[0] + "-untrimmed.wav"
        os.replace(src, keep)
        print(f"    original kept as {keep}")
    write(dst, fr, nch, seg)
    print(f"    {dst}  {total:.2f}s -> {len(seg)//nch/fr:.3f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
