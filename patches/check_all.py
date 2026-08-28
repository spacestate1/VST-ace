#!/usr/bin/env python3
"""Render every patch of every bank and summarise which ones actually work.

make_menu_banks.py maps roles onto names, which tells you a parameter was found
-- not that the result makes a sound. Only rendering does. This is the table to
read before believing any generated bank.

Per bank it reports how many patches produced audio, the loudest peak (so
clipping is visible), the range of lengths, and how many hold rather than decay.
A bank where nothing sounds usually means the plugin needs something the mapping
cannot know about: an oscillator switched on, a program selected, a ROM loaded.

    python3 check_all.py [menu] [../peload/build/peload]
"""
import glob, json, math, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_bank import measure

BANKDIR = sys.argv[1] if len(sys.argv) > 1 else "menu"
PELOAD  = sys.argv[2] if len(sys.argv) > 2 else "../peload/build/peload"
SECS    = 2
NOTE_OFF_MS = SECS * 1000 * 2 / 3

out = tempfile.mkdtemp(prefix="allbanks-")
banks = sorted(glob.glob(os.path.join(BANKDIR, "*.json")))
print(f"{'bank':<24}{'sounding':>9}{'peak dBFS':>11}{'length range':>16}{'held':>6}")
totals = {"ok": 0, "silent": 0, "clip": 0}
worst = []

for b in banks:
    try:
        bank = json.load(open(b))
    except Exception as e:
        print(f"{os.path.basename(b):<24} unreadable: {e}")
        continue
    n_ok = n_held = 0
    peaks, lens = [], []
    for p in bank.get("patches", []):
        wav = os.path.join(out, f"{os.path.basename(b)[:-5]}-{p['name']}.wav")
        try:
            subprocess.run([PELOAD, b, "--pick", p["name"],
                            "--note", str(p.get("note", 72)), "--secs", str(SECS),
                            "--render", wav],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=180)
        except subprocess.TimeoutExpired:
            continue
        r = measure(wav) if os.path.exists(wav) else None
        if not r:
            continue
        pk, ms, _ = r
        # Below about -60 dBFS nothing is being heard over a game's music; that
        # is a failed patch, not a quiet one.
        if pk < 0.001:
            continue
        n_ok += 1
        peaks.append(pk)
        lens.append(ms)
        if ms >= NOTE_OFF_MS * 0.95:
            n_held += 1

    total = len(bank.get("patches", []))
    name = os.path.basename(b)[:-5]
    if not n_ok:
        print(f"{name:<24}{'0/'+str(total):>9}{'-':>11}{'silent':>16}{'':>6}")
        totals["silent"] += 1
        worst.append((name, "no patch produced audio"))
        continue
    pmax = max(peaks)
    db = 20 * math.log10(pmax)
    clip = pmax > 0.99
    if clip:
        totals["clip"] += 1
        worst.append((name, f"clips at {db:+.1f} dBFS"))
    if n_ok == total and not clip:
        totals["ok"] += 1
    rng = f"{min(lens):.0f}-{max(lens):.0f}ms"
    print(f"{name:<24}{str(n_ok)+'/'+str(total):>9}{db:>10.1f}{'!' if clip else ' '}"
          f"{rng:>16}{n_held:>6}")

print(f"\n{totals['ok']} bank(s) fully sounding and clean, "
      f"{totals['clip']} clipping, {totals['silent']} silent, "
      f"of {len(banks)}")
if worst:
    print("\nneeds attention:")
    for n, why in worst:
        print(f"  {n:<24} {why}")
print(f"\nrenders in {out}")
