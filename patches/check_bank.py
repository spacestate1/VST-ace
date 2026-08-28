#!/usr/bin/env python3
"""Render every patch in a bank and measure what actually came out.

The Tricent renders its envelope parameters as an integer, so the display reads
"0" for everything below 1 and the times cannot be read off it. This renders
each patch instead and reports peak, length and brightness -- which is the only
honest way to tune a bank on this plugin.

    python3 check_tricent.py tricent-menu.json ../peload/build/peload

Length is measured to -30 dB below the patch's own peak, using 5 ms windows and
searching forward from the peak. A plain "-40 dB below peak" threshold is not
safe here: a held note on this plugin sits over a rising low-level residue that
reaches about -52 dB, and a threshold under that latches onto the residue and
reports the note-off time instead of the decay.
"""
import json, math, os, struct, subprocess, sys, tempfile, wave


def measure(path):
    w = wave.open(path)
    fr = w.getframerate()
    raw = w.readframes(w.getnframes())
    w.close()
    both = struct.unpack(f"<{len(raw) // 2}h", raw)
    # Both channels, not just the left. NY pans its sections across the stereo
    # field, so a patch whose audio sat entirely in the right channel measured
    # as silence and the whole bank was written off as dead when it was fine.
    s = [max(abs(l), abs(r)) for l, r in zip(both[0::2], both[1::2])]
    win = fr // 200                                    # 5 ms
    env = [max(abs(x) for x in s[i:i + win]) / 32768
           for i in range(0, len(s) - win, win)]
    if not env or max(env) == 0:
        return None
    pk = max(env)
    pi = env.index(pk)
    thr = pk * 10 ** (-30 / 20)
    # The end is where it stays below the threshold, not the first window that
    # dips under it. Two oscillators beating against each other put a notch in
    # the attack -- Fury800's equip drops to -43 dB at 20 ms and comes back to
    # -28 -- and stopping at the first dip reported that 200 ms sound as 15 ms,
    # which sent me tuning a patch that was not wrong.
    hold = max(1, int(0.06 * fr / win))          # 60 ms below, to call it over
    end = len(env) - 1
    for i in range(pi, len(env)):
        if env[i] >= thr:
            continue
        if all(env[j] < thr for j in range(i, min(i + hold, len(env)))):
            end = i
            break
    ms = end * win / fr * 1000
    # Spectral centroid taken *after* the attack, not at the peak.
    #
    # The peak of a percussive sound is its transient, which is broadband, so a
    # window starting there measures the click rather than the timbre. That made
    # brightness look nearly constant no matter what the filter did: on
    # Ragnarok 2 a cutoff of 96 Hz and one of 13351 Hz both read about 3460 Hz,
    # which cannot be true of a lowpass and was the measurement, not the synth.
    #
    # Starting a fifth of the way in, capped at 25 ms, clears the transient
    # while still landing inside even a 30 ms blip.
    skip = min(int(0.025 * fr), max(win, int((end - pi) * win * 0.2)))
    start = pi * win + skip
    if start + 512 > len(s):                      # too short to skip: take what
        start = max(0, len(s) - 2048)             # is there rather than nothing
    seg = list(s[start: start + 2048])
    seg.extend([0] * (2048 - len(seg)))
    m = sum(seg) / len(seg)
    seg = [x - m for x in seg]

    def fft(a):
        n = len(a)
        if n == 1:
            return [complex(a[0])]
        ev, od = fft(a[0::2]), fft(a[1::2])
        out = [0] * n
        for k in range(n // 2):
            t = complex(math.cos(-2 * math.pi * k / n),
                        math.sin(-2 * math.pi * k / n)) * od[k]
            out[k] = ev[k] + t
            out[k + n // 2] = ev[k] - t
        return out

    mag = [abs(v) for v in fft(seg)[:1024]]
    cent = sum(v * (i * fr / 2048) for i, v in enumerate(mag)) / (sum(mag) or 1)
    return pk, ms, cent


def main():
    bank_path = sys.argv[1] if len(sys.argv) > 1 else "tricent-menu.json"
    peload = sys.argv[2] if len(sys.argv) > 2 else "../peload/build/peload"
    bank = json.load(open(bank_path))
    out = tempfile.mkdtemp(prefix="bankcheck-")
    SECS = 2
    # peload holds the note for two thirds of the render, so a patch still
    # sounding at that point is one that holds rather than decays. Worth telling
    # apart: the Strings section has no sustain stage and simply sounds while the
    # key is down, which is correct for a string machine and would otherwise read
    # as a patch that forgot to stop.
    note_off_ms = SECS * 1000 * 2 / 3
    print(f"{'patch':<12}{'note':>5}{'peak':>8}{'dBFS':>7}{'length':>9}"
          f"{'centroid':>10}  behaviour")
    bad = 0
    for p in bank["patches"]:
        wav = os.path.join(out, p["name"] + ".wav")
        subprocess.run([peload, bank_path, "--pick", p["name"],
                        "--note", str(p.get("note", 72)), "--secs", str(SECS),
                        "--render", wav],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
        r = measure(wav) if os.path.exists(wav) else None
        if not r:
            print(f"{p['name']:<12}{p.get('note',72):>5}{'SILENT':>8}")
            bad += 1
            continue
        pk, ms, cent = r
        held = ms >= note_off_ms * 0.95
        how = "holds while key down" if held else "decays on its own"
        print(f"{p['name']:<12}{p.get('note',72):>5}{pk:>8.3f}"
              f"{20*math.log10(pk):>7.1f}{ms:>8.0f}ms{cent:>9.0f}Hz  {how}")
        if pk > 0.99:
            print(f"{'':<12}  ^ CLIPPING")
            bad += 1
    print(f"\nrendered to {out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
