#!/usr/bin/env python3
"""Merge tuned/*.json into all-machines.json.

Run after every machine is added, so the file the window opens always contains
exactly the machines that have been finished. make_tuned.py calls this itself.

Grouped by sound rather than by machine -- all the cursors, then all the
confirms -- so stepping down the list plays the same UI sound on each machine in
turn, which is what the collection is for. --by-plugin writes the other order.
"""
import glob, json, os, sys

BY_PLUGIN = "--by-plugin" in sys.argv
OUT = "all-machines-by-plugin.json" if BY_PLUGIN else "all-machines.json"
ORDER = ["cursor", "confirm", "cancel", "error", "menu-open", "menu-close", "equip"]

entries = []
for src in sorted(glob.glob(os.path.join("tuned", "*.json"))):
    d = json.load(open(src))
    label = d.get("plugin", os.path.basename(src))
    path = d.get("pluginPath", "")
    if path and not path.startswith("/"):
        path = os.path.relpath(
            os.path.normpath(os.path.join(os.path.dirname(src), path)), ".")
    for p in d.get("patches", []):
        q = dict(p)
        q["pluginPath"] = path
        q["name"] = f"{p['name']}  [{label}]"
        entries.append((p["name"], label, q))

entries.sort(key=lambda e: (e[1].lower(), ORDER.index(e[0]) if e[0] in ORDER else 99)
             if BY_PLUGIN else
             (ORDER.index(e[0]) if e[0] in ORDER else 99, e[1].lower()))

machines = sorted({e[1] for e in entries})
bank = {
    "plugin": "several",
    "description": "Hand-tuned menu sounds. A machine appears here only once its "
                   "seven sounds have been written against that synth's own "
                   "architecture and measured at one key: all seven stop on their "
                   "own and are audibly distinct. "
                   + ("Grouped by machine." if BY_PLUGIN else
                      "Grouped by sound, so stepping down the list plays the same "
                      "one on each machine in turn."),
    "machines": machines,
    "patches": [e[2] for e in entries],
}

if not entries:
    # An empty bank is not loadable, and writing one is how the window came to
    # report "no patches, no params and no program". Say so here instead.
    print(f"  nothing in tuned/ yet -- {OUT} not written")
    sys.exit(1)

with open(OUT, "w") as f:
    json.dump(bank, f, indent=2)
    f.write("\n")
print(f"  {OUT}: {len(entries)} patches across {len(machines)} machine(s) "
      f"({', '.join(machines)})")
