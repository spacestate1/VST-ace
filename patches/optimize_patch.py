#!/usr/bin/env python3
"""Search factory programs for the one that best fits a patch's character.

repair_banks.py only asks "does this make a sound" -- enough to keep a bank
honest, but not enough to make a patch sound like what it is called. "equip" is
meant to be a clink: a short bright transient. On plugins where the recipe
cannot reach an amplifier envelope -- Nabla, FB-3300, FM8 -- shortening does
nothing and the patch rings until note-off however the envelope knobs are set.

But the factory programs include percussive ones, and a program that is already
short and bright needs no shortening. So this scores candidate programs against
the character wanted and keeps the best, which is the same measure-don't-guess
move as everything else here, applied to preset choice.

    python3 optimize_patch.py equip [menu] [../peload/build/peload]

Only patches that currently miss the target are touched, so a bank already
sounding right is left alone.
"""
import glob, json, math, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bankutil import is_hand_tuned
from check_bank import measure
from make_menu_banks import OVERRIDES, RECIPE, bind, dump

NAME    = sys.argv[1] if len(sys.argv) > 1 else "equip"
BANKDIR = sys.argv[2] if len(sys.argv) > 2 else "menu"
PELOAD  = sys.argv[3] if len(sys.argv) > 3 else "../peload/build/peload"

# What "a clink" means, measured: audible, over before it registers, and bright
# enough to read as metal rather than wood.
FLOOR_DB, MAX_MS, MIN_HZ = -44.0, 250.0, 2500.0
CANDIDATES = 10

# make_menu_banks reads sys.argv at import time for its own PLUGDIR/PELOAD, so
# importing it from a script with different arguments silently rebinds them --
# here it took "menu" as the peload path and tried to execute it. Point its
# globals back at the real thing rather than relying on argv lining up.
import make_menu_banks as _mmb
_mmb.PELOAD = PELOAD


def fits(pk, ms, ct):
    return 20 * math.log10(pk) >= FLOOR_DB and ms <= MAX_MS and ct >= MIN_HZ


def score(pk, ms, ct):
    """Higher is better. Length dominates, brightness breaks ties, and anything
    inaudible is worthless however short and bright it measures."""
    db = 20 * math.log10(pk) if pk > 0 else -99
    if db < FLOOR_DB:
        return -1e9
    return -(ms / MAX_MS) * 2.0 + min(ct, 12000) / 12000.0


def render(bank, name, note, wav):
    if os.path.exists(wav):
        os.remove(wav)
    try:
        subprocess.run([PELOAD, bank, "--pick", name, "--note", str(note),
                        "--secs", "2", "--render", wav],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
    except subprocess.TimeoutExpired:
        return None
    return measure(wav) if os.path.exists(wav) else None


def build(dll, prog, vals, ov, probe):
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
    params.update({k: v for k, v in ov.items() if not isinstance(v, dict)})
    if isinstance(ov.get(NAME), dict):
        params.update(ov[NAME])
    return params


def main():
    entry = next((r for r in RECIPE if r[0] == NAME), None)
    if not entry:
        raise SystemExit(f"no patch called {NAME!r} in RECIPE")
    _, note, _, vals = entry
    probe = os.path.join(BANKDIR, ".opt.json")
    wav = tempfile.mktemp(suffix=".wav", prefix="opt-")
    improved = kept = 0

    for f in sorted(glob.glob(os.path.join(BANKDIR, "*.json"))):
        bank = json.load(open(f))
        if is_hand_tuned(bank):
            continue
        p = next((x for x in bank.get("patches", []) if x["name"] == NAME), None)
        if p is None:
            continue
        note_now = p.get("note", note)
        r = render(f, NAME, note_now, wav)
        if r and fits(*r):
            kept += 1
            continue

        stem = os.path.basename(f)[:-10]
        dll = os.path.normpath(os.path.join(os.path.dirname(f),
                                            bank.get("pluginPath", "")))
        _, _, nprog, _ = dump(dll, probe, 0)
        nprog = nprog or 1
        best = (score(*r) if r else -1e9, p["program"], p["params"])
        before = best[0]

        step = max(1, nprog // CANDIDATES)
        for prog in range(0, nprog, step):
            if prog == p["program"]:
                continue
            params = build(dll, prog, vals, OVERRIDES.get(stem, {}), probe)
            if params is None:
                continue
            keep = (p["program"], p["params"])
            p["program"], p["params"] = prog, params
            json.dump(bank, open(f, "w"), indent=2)
            rr = render(f, NAME, note_now, wav)
            p["program"], p["params"] = keep
            if rr and score(*rr) > best[0]:
                best = (score(*rr), prog, params)

        p["program"], p["params"] = best[1], best[2]
        with open(f, "w") as fh:
            json.dump(bank, fh, indent=2)
            fh.write("\n")
        rr = render(f, NAME, note_now, wav)
        if rr:
            db, ms, ct = 20 * math.log10(rr[0]), rr[1], rr[2]
            verdict = "fits" if fits(*rr) else "still short of it"
            print(f"  {stem:<18} program {best[1]:>3}  "
                  f"{db:>6.1f} dBFS {ms:>5.0f}ms {ct:>6.0f}Hz  {verdict}")
        if best[0] > before:
            improved += 1

    for x in (probe, wav):
        if os.path.exists(x):
            os.remove(x)
    print(f"\n  {kept} already fitted, {improved} improved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
