#!/usr/bin/env python3
"""Re-apply one patch's recipe across every generated bank, in place.

Changing a sound in RECIPE would otherwise mean regenerating all 31 banks and
losing the per-patch program and note choices repair_banks.py measured its way
to. This rewrites only the named patch, keeping the factory program underneath
it, so a sound can be reworked without discarding the repairs.

    python3 retune_patch.py equip [menu]

Run repair_banks.py afterwards: a shorter, brighter patch can fall below the
silence floor on a plugin where the old one was fine.
"""
from bankutil import is_hand_tuned
import glob, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_menu_banks import OVERRIDES, RECIPE, bind

name    = sys.argv[1] if len(sys.argv) > 1 else "equip"
BANKDIR = sys.argv[2] if len(sys.argv) > 2 else "menu"

entry = next((r for r in RECIPE if r[0] == name), None)
if not entry:
    raise SystemExit(f"no patch called {name!r} in RECIPE")
_, note, desc, vals = entry

n = 0
for f in sorted(glob.glob(os.path.join(BANKDIR, "*.json"))):
    bank = json.load(open(f))
    if is_hand_tuned(bank):
        continue
    for p in bank.get("patches", []):
        if p["name"] != name:
            continue
        env, single, quiet, _ = bind(list(p["params"]), p["params"])
        for st in ("a", "d", "s", "r"):
            for k in env[st]:
                p["params"][k] = vals[st]
        for role in ("flt_c", "flt_q", "noise"):
            if role in single:
                p["params"][single[role]] = vals[role]
        # Per-plugin corrections too, or retuning quietly reverts them: MPS's
        # only length control is its "Envelope" knob, set per patch in
        # OVERRIDES, and a recipe that does not know about it would leave the
        # equip patch at the 460 ms the old weighty version wanted.
        stem = os.path.basename(f)[:-10]
        ov = OVERRIDES.get(stem, {})
        p["params"].update({k: v for k, v in ov.items()
                            if not isinstance(v, dict)})
        if isinstance(ov.get(name), dict):
            p["params"].update(ov[name])
        p["note"], p["description"] = note, desc
        n += 1
    with open(f, "w") as fh:
        json.dump(bank, fh, indent=2)
        fh.write("\n")
print(f"retuned {name!r} in {n} bank(s)")
