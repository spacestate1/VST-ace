#!/usr/bin/env python3
"""Build the hand-written spell banks from magic/spells.py, and measure them.

    python3 make_magic.py            every machine defined
    python3 make_magic.py kern64     one, while working on it

Same method as make_tuned.py -- written per machine against its own
architecture, rendered and measured rather than guessed -- but for a different
family of sounds, and the difference matters to the check.

A menu bank is seven blips that all have to be short. A spell bank is not: the
charge is *supposed* to run two seconds and the lightning is supposed to be
gone in a fifth of one, so a single "anything over 1900 ms holds" rule would
condemn the one sound the bank exists for. Each role carries its own length
window instead, and the table flags a sound against its own target.

Rendered at one key like the menu bank, because that is how they are played.
"""
import json, math, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bankutil import complete
from check_bank import measure
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "magic"))
from spells import SPELLS

PLUGDIR = "../../windows/VST2-64"
PELOAD  = "../peload/build/peload"
OUTDIR  = "magic"
RELBASE = "../../../windows/VST2-64"
TEST_KEY = 60          # everything is compared at middle C
SECS     = 4           # peload holds the note for two thirds of this

# name -> (low ms, high ms). The shape of the family, stated once: a charge
# gathers, a bolt is over before you register it, and the rest sit between.
WINDOWS = {
    "charge":    (1200, 2600),
    "cast":      ( 250,  900),
    "fire":      ( 500, 1600),
    "lightning": (  90,  450),
    "ice":       ( 300, 1200),
    "heal":      ( 700, 2000),
    "fizzle":    ( 250, 1000),
}

# A menu sound that runs until you let go is broken. A charge that does is
# correct -- you hold the button, it gathers, you release and it goes. So the
# hold check, which every other role has to pass, does not apply to the charge.
#
# This is not a licence to stop measuring it. Fury800 is the case that forced
# the question: its DCO envelope is effectively binary, 45 ms at decay step 25
# and 2695 ms at step 27, with nothing reachable in between at any break point
# or slope. On that machine a decaying two-second gather does not exist, so the
# choice was a charge that sustains or no charge at all.
MAY_HOLD = {"charge"}


def full_base(dll, program=0):
    """Every parameter of `dll` at `program`, plus the uniqueID it reports."""
    fd, tmp = tempfile.mkstemp(suffix=".json", prefix="magicbase-")
    os.close(fd)
    os.remove(tmp)
    try:
        subprocess.run([PELOAD, dll, "--program", str(program), "--save-patch", tmp],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
    except subprocess.TimeoutExpired:
        return {}, ""
    if not os.path.exists(tmp):
        return {}, ""
    try:
        d = json.load(open(tmp))
        return d.get("params", {}), d.get("uniqueID", "")
    finally:
        os.remove(tmp)


def build(stem, spec):
    dll = os.path.join(PLUGDIR, f"{stem}.dll")
    base, uid = full_base(dll, spec.get("program", 0))
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
        "uniqueID": uid,
        "pluginPath": f"{RELBASE}/{stem}.dll",
        "handTuned": True,
        "description": f"Hand-written spell sounds for {spec['plugin']}.",
        "patches": patches,
    }
    out = os.path.join(OUTDIR, f"{stem}-magic.json")
    with open(out, "w") as f:
        json.dump(bank, f, indent=2)
        f.write("\n")
    return out


def report(path):
    """Render every patch at one key and print length, level and brightness."""
    bank = json.load(open(path))
    tmp = tempfile.mkdtemp(prefix="magic-")
    note_off = SECS * 1000 * 2 / 3
    print(f"\n  {bank['plugin']}  (all at key {TEST_KEY})")
    print(f"    {'spell':<12}{'dBFS':>7}{'length':>9}{'target':>12}"
          f"{'centroid':>10}  note")
    rows = []
    for p in bank["patches"]:
        wav = os.path.join(tmp, p["name"] + ".wav")
        subprocess.run([PELOAD, path, "--pick", p["name"], "--note", str(TEST_KEY),
                        "--secs", str(SECS), "--render", wav],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
        r = measure(wav) if os.path.exists(wav) else None
        if not r:
            print(f"    {p['name']:<12}{'SILENT':>7}")
            rows.append(None)
            continue
        pk, ms, ct = r
        db = 20 * math.log10(pk)
        lo, hi = WINDOWS.get(p["name"], (0, note_off))
        held = ms >= note_off * 0.95
        flag = ""
        if held and p["name"] in MAY_HOLD:
            flag = "  (holds while the key is down)"
        elif held:                flag = "  <- holds"
        elif ms < lo:             flag = f"  <- short of {lo}"
        elif ms > hi:             flag = f"  <- past {hi}"
        elif pk > 0.95:           flag = "  <- clips"
        elif db < -44:            flag = "  <- inaudible"
        print(f"    {p['name']:<12}{db:>7.1f}{ms:>8.0f}ms"
              f"{lo:>7}-{hi:<4}{ct:>9.0f}Hz{flag}")
        rows.append((ms, ct))

    # A patch can render loud enough to measure and still have a centroid of
    # zero -- TAL-U-No-62's ice did, at -78 dBFS, which is silence that did not
    # quite round to silence. Dividing by that took the whole run down, so the
    # comparison floors the centroid rather than trusting it.
    good = [r for r in rows if r and r[1] > 0]
    if len(good) > 1:
        # Distinctness: how close the nearest pair is in length and brightness.
        worst = None
        for i in range(len(good)):
            for j in range(i + 1, len(good)):
                a, b = good[i], good[j]
                d = abs(math.log((a[0] + 5) / (b[0] + 5))) + abs(math.log(a[1] / b[1]))
                if worst is None or d < worst[0]:
                    worst = (d, i, j)
        names = [p["name"] for p, r in zip(bank["patches"], rows) if r and r[1] > 0]
        print(f"    closest pair: {names[worst[1]]} / {names[worst[2]]} "
              f"(separation {worst[0]:.2f}{'  <- too alike' if worst[0] < 0.35 else ''})")
    print(f"    rendered to {tmp}")


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    want = sys.argv[1:] or list(SPELLS)
    for stem in want:
        if stem not in SPELLS:
            print(f"  {stem}: not in magic/spells.py")
            continue
        out = build(stem, SPELLS[stem])
        if out:
            report(out)
    print()
    subprocess.run([sys.executable, "make_magic_bank.py"])
    subprocess.run([sys.executable, "make_magic_bank.py", "--by-plugin"],
                   stdout=subprocess.DEVNULL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
