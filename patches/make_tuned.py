#!/usr/bin/env python3
"""Build the hand-tuned banks from tuned/machines.py, and measure them.

    python3 make_tuned.py            every machine defined
    python3 make_tuned.py kern64     one, while working on it

For each machine it writes tuned/<stem>-menu.json, renders all seven patches
**at one key**, and prints what came out. One key matters: the patches carry a
note for rendering, but in the window you play your own, so seven sounds that
differ only in pitch are seven of the same sound. The table is the check --
lengths and centroids that cluster mean the machine is not done.

Every patch is filled out to the plugin's whole parameter set, so selecting one
overrides the program outright, and the file is marked "handTuned" so no
generator pass will touch it.
"""
import json, math, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bankutil import complete, full_param_base
from check_bank import measure
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tuned"))
from machines import MACHINES

PLUGDIR = "../../windows/VST2-64"
PELOAD  = "../peload/build/peload"
OUTDIR  = "tuned"
RELBASE = "../../../windows/VST2-64"
TEST_KEY = 60          # everything is compared at middle C


def build(stem, spec):
    dll = os.path.join(PLUGDIR, f"{stem}.dll")
    base = full_param_base(PELOAD, dll, spec.get("program", 0))
    if not base:
        print(f"  {stem}: will not load")
        return None
    patches = []
    for name, p in spec["patches"].items():
        params = dict(spec.get("common", {}))
        params.update(p["params"])
        unknown = [k for k in params if k not in base]
        if unknown:
            print(f"  {stem}/{name}: not parameters of this plugin: {unknown}")
        patches.append({
            "name": name,
            "note": p.get("note", 72),
            "description": p.get("description", ""),
            "program": spec.get("program", 0),
            "params": complete(params, base),
        })
    bank = {
        "plugin": spec["plugin"],
        "pluginPath": f"{RELBASE}/{stem}.dll",
        "handTuned": True,
        "description": f"Hand-tuned menu sounds for {spec['plugin']}.",
        "patches": patches,
    }
    out = os.path.join(OUTDIR, f"{stem}-menu.json")
    with open(out, "w") as f:
        json.dump(bank, f, indent=2)
        f.write("\n")
    return out


def report(path):
    """Render every patch at one key and print length, level and brightness."""
    bank = json.load(open(path))
    tmp = tempfile.mkdtemp(prefix="tuned-")
    print(f"\n  {bank['plugin']}  (all at key {TEST_KEY})")
    print(f"    {'patch':<12}{'dBFS':>7}{'length':>9}{'centroid':>10}  note")
    rows = []
    for p in bank["patches"]:
        wav = os.path.join(tmp, p["name"] + ".wav")
        subprocess.run([PELOAD, path, "--pick", p["name"], "--note", str(TEST_KEY),
                        "--secs", "3", "--render", wav],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
        r = measure(wav) if os.path.exists(wav) else None
        if not r:
            print(f"    {p['name']:<12}{'SILENT':>7}")
            rows.append(None)
            continue
        pk, ms, ct = r
        db = 20 * math.log10(pk)
        flag = ""
        if ms >= 1900:  flag = "  <- holds"
        elif pk > 0.95: flag = "  <- clips"
        elif db < -44:  flag = "  <- inaudible"
        print(f"    {p['name']:<12}{db:>7.1f}{ms:>8.0f}ms{ct:>9.0f}Hz{flag}")
        rows.append((ms, ct))

    good = [r for r in rows if r]
    if len(good) > 1:
        # Distinctness: how close the nearest pair is in length and brightness.
        worst = None
        for i in range(len(good)):
            for j in range(i + 1, len(good)):
                a, b = good[i], good[j]
                d = abs(math.log((a[0] + 5) / (b[0] + 5))) + abs(math.log(a[1] / b[1]))
                if worst is None or d < worst[0]:
                    worst = (d, i, j)
        names = [p["name"] for p, r in zip(bank["patches"], rows) if r]
        print(f"    closest pair: {names[worst[1]]} / {names[worst[2]]} "
              f"(separation {worst[0]:.2f}{'  <- too alike' if worst[0] < 0.35 else ''})")


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    want = sys.argv[1:] or list(MACHINES)
    for stem in want:
        if stem not in MACHINES:
            print(f"  {stem}: not in tuned/machines.py")
            continue
        out = build(stem, MACHINES[stem])
        if out:
            report(out)
    # Always rebuild the merged bank, so the file the window opens is current
    # the moment a machine is finished rather than one manual step later.
    print()
    subprocess.run([sys.executable, "make_tuned_bank.py"])
    subprocess.run([sys.executable, "make_tuned_bank.py", "--by-plugin"],
                   stdout=subprocess.DEVNULL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
