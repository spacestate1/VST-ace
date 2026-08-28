#!/usr/bin/env python3
"""Merge magic/*.json into all-magic.json.

Run after every machine is added, so the file the window opens always holds
exactly the machines that have been finished. make_magic.py calls this itself.

Grouped by spell rather than by machine -- all the charges, then all the
fireballs -- so stepping down the list plays the same spell on each machine in
turn. --by-plugin writes the other order, for auditioning one synth.

Two things the menu merge drops and this one keeps:

  * each patch's "uniqueID", so peload can tell you when a patch has been
    applied to the wrong plugin instead of quietly setting whatever matched
  * "handTuned" on the merged file, so a pass pointed at this directory leaves
    it alone the way it leaves the per-machine banks alone
"""
import glob, json, os, sys

BY_PLUGIN = "--by-plugin" in sys.argv
OUT = "all-magic-by-plugin.json" if BY_PLUGIN else "all-magic.json"
ORDER = ["charge", "cast", "fire", "lightning", "ice", "heal", "fizzle"]

entries = []
for src in sorted(glob.glob(os.path.join("magic", "*.json"))):
    d = json.load(open(src))
    label = d.get("plugin", os.path.basename(src))
    path = d.get("pluginPath", "")
    if path and not path.startswith("/"):
        path = os.path.relpath(
            os.path.normpath(os.path.join(os.path.dirname(src), path)), ".")
    for p in d.get("patches", []):
        q = dict(p)
        q["pluginPath"] = path
        q["uniqueID"] = d.get("uniqueID", "")
        q["name"] = f"{p['name']}  [{label}]"
        entries.append((p["name"], label, q))

# Anything not in ORDER sorts to the end rather than into the middle, and is
# named when it happens -- a spell that quietly lands after all the fizzles
# because of a typo in its name is a thing you should be told about.
unknown = sorted({e[0] for e in entries if e[0] not in ORDER})
if unknown:
    print(f"  not in the spell order, so appended last: {', '.join(unknown)}")

entries.sort(key=lambda e: (e[1].lower(), ORDER.index(e[0]) if e[0] in ORDER else 99)
             if BY_PLUGIN else
             (ORDER.index(e[0]) if e[0] in ORDER else 99, e[1].lower()))

machines = sorted({e[1] for e in entries})
bank = {
    "plugin": "several",
    "handTuned": True,
    "description": "Spell sounds for an RPG -- a charge that gathers, the cast "
                   "that releases it, fire, lightning and ice, a heal, and the "
                   "fizzle when it fails. Written per machine against that "
                   "synth's own architecture and measured at one key. "
                   + ("Grouped by machine." if BY_PLUGIN else
                      "Grouped by spell, so stepping down the list plays the "
                      "same one on each machine in turn."),
    "machines": machines,
    "patches": [e[2] for e in entries],
}

if not entries:
    # An empty bank is not loadable, and writing one is how the window came to
    # report "no patches, no params and no program". Say so here instead.
    print(f"  nothing in magic/ yet -- {OUT} not written")
    sys.exit(1)

with open(OUT, "w") as f:
    json.dump(bank, f, indent=2)
    f.write("\n")
print(f"  {OUT}: {len(entries)} patches across {len(machines)} machine(s) "
      f"({', '.join(machines)})")
