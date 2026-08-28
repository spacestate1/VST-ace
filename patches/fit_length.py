#!/usr/bin/env python3
"""Tune one patch's decay per plugin until it measures the length it should.

A normalised 0..1 parameter is not the same time on two synths. The equip patch
at decay 0.30 measured 685 ms on ModulAir and 45 ms on Ragnarok 2 -- the same
number, an order of magnitude apart. One value in the recipe cannot be right
everywhere, and picking a different factory program does not help: the program
is the timbre, the decay knob is the length.

So the length is searched for instead. Decay is very nearly monotonic, so a
handful of renders per plugin brackets it: raise the knob while the sound is too
short, lower it while too long, keep the closest to the target window. Release
is carried along at a fixed fraction, because a decay with no release ends in a
click.

    python3 fit_length.py equip 120 700 [menu] [../peload/build/peload]

Only patches outside the window are touched.
"""
import glob, json, math, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bankutil import is_hand_tuned
from check_bank import measure

NAME    = sys.argv[1] if len(sys.argv) > 1 else "equip"
MIN_MS  = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
MAX_MS  = float(sys.argv[3]) if len(sys.argv) > 3 else 700.0
BANKDIR = sys.argv[4] if len(sys.argv) > 4 else "menu"
PELOAD  = sys.argv[5] if len(sys.argv) > 5 else "../peload/build/peload"

import make_menu_banks as _mmb
_mmb.PELOAD = PELOAD
from make_menu_banks import bind

STEPS = 7               # renders per plugin; 2^7 resolution is far finer than
                        # the ear needs on a 120-700 ms window
RELEASE_FRACTION = 0.6  # a decay that ends abruptly clicks


def render(bank, name, note, wav):
    if os.path.exists(wav):
        os.remove(wav)
    try:
        subprocess.run([PELOAD, bank, "--pick", name, "--note", str(note),
                        "--secs", "3", "--render", wav],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
    except subprocess.TimeoutExpired:
        return None
    return measure(wav) if os.path.exists(wav) else None


def main():
    wav = tempfile.mktemp(suffix=".wav", prefix="fit-")
    target = (MIN_MS + MAX_MS) / 2
    fitted = kept = failed = 0

    for f in sorted(glob.glob(os.path.join(BANKDIR, "*.json"))):
        bank = json.load(open(f))
        if is_hand_tuned(bank):
            continue
        p = next((x for x in bank.get("patches", []) if x["name"] == NAME), None)
        if p is None:
            continue
        note = p.get("note", 84)
        r = render(f, NAME, note, wav)
        if r and MIN_MS <= r[1] <= MAX_MS:
            kept += 1
            continue
        if not r:
            failed += 1
            continue

        env, _, _, _ = bind(list(p["params"]), p["params"])
        dk, rk = env["d"], env["r"]
        # The PS-series envelopes are AR: "EM Rel", "GEG Rel", "Over Rel" and no
        # decay at all. There the release *is* the length, so tune that and
        # leave the fraction alone.
        if not dk and rk:
            dk, rk = rk, []
        if not dk:
            print(f"  {os.path.basename(f)[:-10]:<18} no decay or release to "
                  f"tune ({r[1]:.0f}ms)")
            failed += 1
            continue

        original = {k: p["params"][k] for k in dk + rk}
        lo, hi = 0.0, 1.0
        best = (abs(r[1] - target), dict(original), r)
        for _ in range(STEPS):
            mid = (lo + hi) / 2
            for k in dk:
                p["params"][k] = mid
            for k in rk:
                p["params"][k] = mid * RELEASE_FRACTION
            json.dump(bank, open(f, "w"), indent=2)
            rr = render(f, NAME, note, wav)
            if not rr:
                hi = mid            # gone silent: the knob went too far
                continue
            d = abs(rr[1] - target)
            if d < best[0]:
                best = (d, {k: p["params"][k] for k in dk + rk}, rr)
            if rr[1] < MIN_MS:
                lo = mid
            elif rr[1] > MAX_MS:
                hi = mid
            else:
                break

        p["params"].update(best[1])
        with open(f, "w") as fh:
            json.dump(bank, fh, indent=2)
            fh.write("\n")
        ms = best[2][1]
        ok = MIN_MS <= ms <= MAX_MS
        fitted += ok
        failed += not ok
        print(f"  {os.path.basename(f)[:-10]:<18} {r[1]:>5.0f}ms -> {ms:>5.0f}ms  "
              f"decay {best[1][dk[0]]:.3f}  {'fits' if ok else 'closest reachable'}")

    if os.path.exists(wav):
        os.remove(wav)
    print(f"\n  {kept} already in range, {fitted} fitted, {failed} could not be")
    return 0


if __name__ == "__main__":
    sys.exit(main())
