#!/usr/bin/env python3
"""Render every patch, and repair the ones that come out silent or clipping.

Giving each patch a different factory program as its base is what made the seven
patches on a machine sound like seven sounds instead of one -- the parameters
varying between them went from a mean of 10% to 55%. But it is a blind choice:
programs 0, 10, 21, 32... are whatever the vendor happened to put there, and
some are silent at the note the patch plays while others are far hotter than
program 0. That cost seven banks their clean bill of health.

So the choice stops being blind. Each patch is rendered; a patch that produces
nothing, or that clips, has its base program replaced with another and is
rendered again, until one works or the candidates run out.

Clipping is tried first with the master turned down, because that keeps the
sound the patch was aiming for; only if the plugin exposes no master does the
program get swapped.

    python3 repair_banks.py [menu] [../peload/build/peload]

Only failing patches cost extra renders, so this is roughly one render per patch
plus a handful. Run it after make_menu_banks.py and before make_all_bank.py.
"""
import json, glob, math, os, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bankutil import is_hand_tuned
from check_bank import measure
from make_menu_banks import LOUD, OVERRIDES, QUIET, RECIPE, SINGLE, bind, dump

BANKDIR = sys.argv[1] if len(sys.argv) > 1 else "menu"
PELOAD  = sys.argv[2] if len(sys.argv) > 2 else "../peload/build/peload"

SILENT = 0.006          # -44 dBFS. At the old -60 an oxid patch at
                        # -90 dBFS and a TAL one at -49 both counted as
                        # sounding, which is not what sounding means.
HOT    = 0.95           # at this the render is flat-topped
TARGET = 0.5            # what a trimmed master should come back to
MAX_TRIES = 6

# make_menu_banks reads sys.argv at import time for its own PLUGDIR/PELOAD, so
# importing it from a script with different arguments silently rebinds them --
# here it took "menu" as the peload path and tried to execute it. Point its
# globals back at the real thing rather than relying on argv lining up.
import make_menu_banks as _mmb
_mmb.PELOAD = PELOAD
# Notes to fall back on. The recipe pitches each patch for musical reasons --
# 84 for a cursor tick, 45 for an error -- which assumes every plugin answers
# across the keyboard. A drum machine does not: BucketPops maps drums to roughly
# 36-60 and is silent at 72 and 84, so six of its seven patches produced nothing
# at any program. The pitch is the wrong thing about those patches, not the
# preset under them.
NOTE_TRIES = [60, 48, 55, 43, 38, 36, 67, 72]


def render(bank_path, patch_name, note, tmp):
    if os.path.exists(tmp):
        os.remove(tmp)
    try:
        subprocess.run([PELOAD, bank_path, "--pick", patch_name,
                        "--note", str(note), "--secs", "2", "--render", tmp],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
    except subprocess.TimeoutExpired:
        return None
    return measure(tmp) if os.path.exists(tmp) else None


def candidates(current, nprog):
    """Other programs to try, furthest from the ones already spread across."""
    if nprog <= 1:
        return []
    step = max(1, nprog // (MAX_TRIES + 1))
    out = [(current + step * k) % nprog for k in range(1, MAX_TRIES + 1)]
    return [p for p in dict.fromkeys(out) if p != current]


def rebuild_params(dll, prog, recipe_vals, probe, stem="", patch_name=""):
    """The plugin's parameters at `prog`, with this patch's recipe on top."""
    d, _, _, err = dump(dll, probe, prog)
    if d is None:
        return None
    names = list(d.get("params", {}))
    env, single, quiet, loud = bind(names, d.get("params", {}))
    params = dict(d["params"])
    for st in ("a", "d", "s", "r"):
        for n in env[st]:
            params[n] = recipe_vals[st]
    for role in ("flt_c", "flt_q", "noise"):
        if role in single:
            params[single[role]] = recipe_vals[role]
    for n in quiet:
        params[n] = 0.0
    # Per-plugin corrections have to survive a program swap, or repairing a bank
    # silently reverts the fix that made it work in the first place.
    ov = OVERRIDES.get(stem, {})
    params.update({k: v for k, v in ov.items() if not isinstance(v, dict)})
    if isinstance(ov.get(patch_name), dict):
        params.update(ov[patch_name])
    return params


def main():
    probe = os.path.join(BANKDIR, ".repair.json")
    wav = tempfile.mktemp(suffix=".wav", prefix="repair-")
    recipe_by_name = {r[0]: r[3] for r in RECIPE}
    fixed = failed = checked = 0

    for bank_path in sorted(glob.glob(os.path.join(BANKDIR, "*.json"))):
        bank = json.load(open(bank_path))
        if is_hand_tuned(bank):
            continue
        dll = os.path.normpath(os.path.join(os.path.dirname(bank_path),
                                            bank.get("pluginPath", "")))
        # How many programs there are to choose among, from the plugin itself.
        _, _, nprog, _ = dump(dll, probe, 0)
        nprog = nprog or 1
        changed = False
        for p in bank.get("patches", []):
            checked += 1
            note = p.get("note", 72)
            r = render(bank_path, p["name"], note, wav)
            peak = r[0] if r else 0.0
            if SILENT <= peak <= HOT:
                continue

            label = "silent" if peak < SILENT else f"{20*math.log10(peak):+.1f} dBFS"
            print(f"  {os.path.basename(bank_path)[:-5]:<22} {p['name']:<11} {label}", end="")

            # A hot patch first has its master brought down: swapping the program
            # would throw away the sound, and the level is usually the only thing
            # wrong with it.
            if peak > HOT:
                masters = [k for k in p["params"]
                           if any(re.fullmatch(m, k, re.I) for m in LOUD)]
                for _ in range(4):
                    if not masters or peak <= HOT:
                        break
                    for m in masters:
                        p["params"][m] = max(0.05, p["params"][m] * 0.6)
                    json.dump(bank, open(bank_path, "w"), indent=2)
                    r = render(bank_path, p["name"], note, wav)
                    peak = r[0] if r else 0.0
                if peak <= HOT and peak >= SILENT:
                    print(f"  -> trimmed to {20*math.log10(peak):+.1f} dBFS")
                    changed = True
                    fixed += 1
                    continue

            # Otherwise try a different factory program under it.
            ok = False
            for prog in candidates(p.get("program", 0), nprog):
                params = rebuild_params(dll, prog, recipe_by_name[p["name"]], probe,
                                        os.path.basename(bank_path)[:-10], p["name"])
                if params is None:
                    continue
                before = (p["program"], p["params"])
                p["program"], p["params"] = prog, params
                json.dump(bank, open(bank_path, "w"), indent=2)
                r = render(bank_path, p["name"], note, wav)
                peak = r[0] if r else 0.0
                if SILENT <= peak <= HOT:
                    print(f"  -> program {prog}, {20*math.log10(peak):+.1f} dBFS")
                    ok = True
                    changed = True
                    fixed += 1
                    break
                p["program"], p["params"] = before
            # Still nothing: try the patch at other pitches before giving up.
            if not ok:
                for n in NOTE_TRIES:
                    if n == note:
                        continue
                    r = render(bank_path, p["name"], n, wav)
                    peak = r[0] if r else 0.0
                    if SILENT <= peak <= HOT:
                        p["note"] = n
                        json.dump(bank, open(bank_path, "w"), indent=2)
                        print(f"  -> note {n}, {20*math.log10(peak):+.1f} dBFS")
                        ok = True
                        changed = True
                        fixed += 1
                        break
            if not ok:
                json.dump(bank, open(bank_path, "w"), indent=2)
                print("  -> no usable program or note found")
                failed += 1

        if changed:
            json.dump(bank, open(bank_path, "w"), indent=2)
            with open(bank_path, "a") as f:
                f.write("\n")

    for f in (probe, wav):
        if os.path.exists(f):
            os.remove(f)
    print(f"\n  {checked} patches checked, {fixed} repaired, {failed} unfixable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
