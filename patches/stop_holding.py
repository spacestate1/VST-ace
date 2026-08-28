#!/usr/bin/env python3
"""Find factory programs that stop on their own, for patches that do not.

A UI sound has to end whether or not the key is still down. Plenty of these
patches do not: 13 banks have between one and six that ring until note-off, and
holding a key in pestudio makes that obvious in a way a render at a fixed length
does not.

The cause is not one thing. Some plugins bind no sustain stage at all -- FB-3300
exposes a single "GEG Rel" and nothing else, SequencAir one decay -- so there is
nothing to set to zero. Others bind sustain, have it set to zero, and still
sustain, because a second section is doing it: Nabla's string section has an
attack and no release whatever, so it sounds for as long as the key is held no
matter what the synth envelope says. Chasing that by parameter name across
thirty synths is not tractable.

What is tractable: the factory programs include percussive ones, and a program
that already stops needs nothing done to it. So each holding patch is rendered
against candidate programs and the best one that decays is kept -- scored on
landing near the length the patch wants, not merely on stopping.

    python3 stop_holding.py [menu] [../peload/build/peload]

Only holding patches are touched, so this costs nothing for a bank already
behaving.
"""
import glob, json, math, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bankutil import is_hand_tuned
from check_bank import measure

BANKDIR = sys.argv[1] if len(sys.argv) > 1 else "menu"
PELOAD  = sys.argv[2] if len(sys.argv) > 2 else "../peload/build/peload"

import make_menu_banks as _mmb
_mmb.PELOAD = PELOAD
from make_menu_banks import OVERRIDES, RECIPE, bind, dump

SECS = 3
NOTE_OFF_MS = SECS * 1000 * 2 / 3
HOLDS = NOTE_OFF_MS * 0.95
FLOOR = 0.006
CANDIDATES = 8

# What each sound is aiming for, in milliseconds.
WANT = {
    "cursor": (20, 90), "confirm": (60, 250), "cancel": (50, 220),
    "error": (120, 500), "menu-open": (180, 700), "menu-close": (120, 500),
    "equip": (120, 700),
}


def render(bank, name, note, wav):
    if os.path.exists(wav):
        os.remove(wav)
    try:
        subprocess.run([PELOAD, bank, "--pick", name, "--note", str(note),
                        "--secs", str(SECS), "--render", wav],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
    except subprocess.TimeoutExpired:
        return None
    return measure(wav) if os.path.exists(wav) else None


def build(dll, prog, name, vals, stem, probe):
    d, _, _, _ = dump(dll, probe, prog)
    if d is None:
        return None
    params = dict(d["params"])
    env, single, quiet, _ = bind(list(params), params)
    for st in ("a", "d", "s", "r"):
        for k in env[st]:
            params[k] = vals[st]
    for role in ("flt_c", "flt_q", "noise"):
        if role in single:
            params[single[role]] = vals[role]
    for k in quiet:
        params[k] = 0.0
    # Per-plugin corrections must survive a program swap. Leaving them off is
    # how a repair silently reverts the very fix that made a bank work.
    ov = OVERRIDES.get(stem, {})
    params.update({k: v for k, v in ov.items() if not isinstance(v, dict)})
    if isinstance(ov.get(name), dict):
        params.update(ov[name])
    return params


def score(ms, ct, lo, hi):
    """Stopping matters most, then landing in the window, then brightness."""
    if ms >= HOLDS:
        return -1e6 + ct / 1e6
    miss = 0.0 if lo <= ms <= hi else (lo - ms if ms < lo else ms - hi)
    return -miss / 100.0 + min(ct, 12000) / 24000.0


def main():
    probe = os.path.join(BANKDIR, ".hold.json")
    wav = tempfile.mktemp(suffix=".wav", prefix="hold-")
    vals_by_name = {r[0]: r[3] for r in RECIPE}
    fixed = still = kept = 0

    for f in sorted(glob.glob(os.path.join(BANKDIR, "*.json"))):
        bank = json.load(open(f))
        if is_hand_tuned(bank):
            continue
        stem = os.path.basename(f)[:-10]
        dll = os.path.normpath(os.path.join(os.path.dirname(f),
                                            bank.get("pluginPath", "")))
        nprog = None
        for p in bank.get("patches", []):
            note = p.get("note", 72)
            r = render(f, p["name"], note, wav)
            if r and r[1] < HOLDS and r[0] >= FLOOR:
                kept += 1
                continue
            if p["name"] not in WANT:
                continue
            if nprog is None:
                _, _, nprog, _ = dump(dll, probe, 0)
                nprog = nprog or 1
            lo, hi = WANT[p["name"]]
            best = (score(r[1], r[2], lo, hi) if r else -1e9,
                    p["program"], p["params"], r)
            step = max(1, nprog // CANDIDATES)
            for prog in range(0, nprog, step):
                if prog == p["program"]:
                    continue
                params = build(dll, prog, p["name"], vals_by_name[p["name"]],
                               stem, probe)
                if params is None:
                    continue
                keep = (p["program"], p["params"])
                p["program"], p["params"] = prog, params
                json.dump(bank, open(f, "w"), indent=2)
                rr = render(f, p["name"], note, wav)
                p["program"], p["params"] = keep
                if rr and rr[0] >= FLOOR:
                    sc = score(rr[1], rr[2], lo, hi)
                    if sc > best[0]:
                        best = (sc, prog, params, rr)
            p["program"], p["params"] = best[1], best[2]
            with open(f, "w") as fh:
                json.dump(bank, fh, indent=2)
                fh.write("\n")
            rr = best[3]
            if rr and rr[1] < HOLDS:
                print(f"  {stem:<20} {p['name']:<11} -> program {best[1]:>3}, "
                      f"{rr[1]:>5.0f}ms  stops")
                fixed += 1
            else:
                print(f"  {stem:<20} {p['name']:<11} -> no program stops")
                still += 1

    for x in (probe, wav):
        if os.path.exists(x):
            os.remove(x)
    print(f"\n  {kept} already stop, {fixed} fixed, {still} still hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
