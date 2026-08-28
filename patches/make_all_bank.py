#!/usr/bin/env python3
"""Merge the per-plugin banks into one file that spans every machine.

Opening thirty-one files to hear the same blip on thirty-one synths is the
wrong shape for the question "what does this sound like on each of them". So
this writes a single bank whose patches each carry their own "pluginPath":
selecting one loads that synth and applies that patch, and stepping down the
list walks the same sound across the whole collection.

Ordered by patch first, plugin second -- "cursor" on every machine, then
"confirm" on every machine -- because that is the order you want for comparing.
Pass --by-plugin to group the other way round, which is the order you want for
auditioning one synth.

    python3 make_all_bank.py [--by-plugin]

Reads menu/*.json, so run make_menu_banks.py first. The two hand-written banks
are included too: they are better than their generated counterparts, so where
one exists it replaces the generated patches for that plugin.
"""
import glob, json, os, sys

BY_PLUGIN = "--by-plugin" in sys.argv
OUT = "all-machines-by-plugin.json" if BY_PLUGIN else "all-machines.json"

# Hand-written banks supersede the generated one for the same plugin: they use
# each machine's own character -- FB-7999's Auto Bend, the Trident's three
# sections -- which a generic envelope shape cannot reach.
HANDWRITTEN = ["rpg-menu.json", "tricent-menu.json"]

entries = []          # (patch_name, plugin_label, patch dict, source)
superseded = set()

def rel_from_here(bank_path, plugin_path):
    """A pluginPath in a sub-bank is relative to that bank; re-base it to here."""
    if plugin_path.startswith("/"):
        return plugin_path
    joined = os.path.join(os.path.dirname(bank_path), plugin_path)
    return os.path.relpath(os.path.normpath(joined), ".")


for src in HANDWRITTEN:
    if not os.path.exists(src):
        continue
    d = json.load(open(src))
    label = d.get("plugin", src)
    superseded.add(label)
    path = rel_from_here(src, d.get("pluginPath", ""))
    for p in d.get("patches", []):
        q = dict(p)
        q["pluginPath"] = path
        q["uniqueID"] = d.get("uniqueID", "")
        q["name"] = f"{p['name']}  [{label}]"
        entries.append((p["name"], label, q, src))

for src in sorted(glob.glob(os.path.join("menu", "*.json"))):
    d = json.load(open(src))
    label = d.get("plugin", os.path.basename(src))
    if label in superseded:
        continue
    path = rel_from_here(src, d.get("pluginPath", ""))
    for p in d.get("patches", []):
        q = dict(p)
        q.pop("description", None)
        q["pluginPath"] = path
        q["uniqueID"] = d.get("uniqueID", "")
        q["name"] = f"{p['name']}  [{label}]"
        entries.append((p["name"], label, q, src))

# "cursor" on every machine, then "confirm" on every machine -- or the reverse.
order = [p["name"] for p in json.load(open(HANDWRITTEN[0]))["patches"]] \
    if os.path.exists(HANDWRITTEN[0]) else []


def rank(e):
    pname, label, _, _ = e
    i = order.index(pname) if pname in order else len(order)
    return (label.lower(), i) if BY_PLUGIN else (i, pname, label.lower())


entries.sort(key=rank)

bank = {
    "plugin": "several",
    "description": "The same menu sounds across every Windows 64-bit synth "
                   "here. Each patch names its own plugin, so selecting one "
                   "loads that synth and applies that patch. "
                   + ("Grouped by plugin." if BY_PLUGIN
                      else "Grouped by sound, so stepping down the list plays "
                           "the same blip on each machine in turn."),
    "patches": [e[2] for e in entries],
}
with open(OUT, "w") as f:
    json.dump(bank, f, indent=2)
    f.write("\n")

machines = sorted({e[1] for e in entries})
print(f"wrote {OUT}: {len(entries)} patches across {len(machines)} machines")
print(f"  hand-written banks used for: {', '.join(sorted(superseded)) or '(none)'}")
