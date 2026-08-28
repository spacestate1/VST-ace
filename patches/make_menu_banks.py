#!/usr/bin/env python3
"""Generate a menu-sound bank for every Windows 64-bit synth we can map.

The two hand-written banks (rpg-menu.json, tricent-menu.json) were authored
against one plugin's parameter list each. That does not scale to thirty, and
these plugins agree on almost nothing. The amplifier envelope alone appears as:

    VCA EG Attack   FB-7999          AMPATTACK       TAL-U-No-62
    VCA Attack      Tricent          AmpEnv A        kern, Ragnarok 2
    Loudness Attack brokenmini       Env 2 A         blooo, scrooo
    Out Attack      ModulAir         DEG2 Attack     Fury800
    Synth: Attack   Oxid             P1 EnvA         Stigma
    Envelope: Attack Paralogy        OP1: Attack     FB-02 (six of them)

So this does not try to find *the* amplifier envelope. Guessing which of "Env 1"
and "Env 2" is the amp is exactly the kind of thing that is wrong half the time
and silently produces a bank that sustains forever. Instead it shortens **every**
envelope the plugin exposes, which is what a UI blip wants anyway: a filter
envelope given the same fast shape is no problem, and on an FM synth setting all
six operator envelopes short is precisely correct.

Filter cutoff, resonance and noise are bound separately by name, and anything
that smears a 40 ms sound -- effect tails, LFO depth, portamento -- is forced to
zero.

Values are normalised 0..1, which is what the VST2 API takes. That is an
approximation across thirty synths: 0.1 is not the same decay time on all of
them. So render every bank afterwards and read what actually came out --

    python3 make_menu_banks.py
    python3 check_all.py menu ../peload/build/peload

-- because the table that script prints, not this file, is what says whether a
bank is any good.
"""
import json, os, re, subprocess, sys

from bankutil import complete, is_hand_tuned

PLUGDIR = sys.argv[1] if len(sys.argv) > 1 else "../../windows/VST2-64"
PELOAD  = sys.argv[2] if len(sys.argv) > 2 else "../peload/build/peload"
OUTDIR  = "menu"
# From patches/menu/ up to the vst root, so a bank names its own plugin and
# stays portable as long as the tree moves as a whole.
RELBASE = "../../../windows/VST2-64"

# A name carrying any of these is a *setting about* an envelope stage, not the
# stage itself: "OP1: Attack Velocity" scales the attack, "Env1 Trg" retriggers
# it, "(1) Filter Env Invert" flips it. Shortening those would be meaningless at
# best and would break the sound at worst.
NOT_A_STAGE = re.compile(
    r"velocity|invert|trigger|\btrg\b|key ?scal|\bmode\b|\bamount\b|"
    r"polarity|\bcurve\b|\bdelay\b|"
    r"\bsource\b|\bdest\b|\bloop\b|\bsync\b", re.I)

# The DW-8000's envelope has six stages, not four: attack, decay, *break point*,
# *slope*, sustain, release. Leaving break point and slope alone left FB-7999
# ringing until note-off while its four-stage siblings decayed properly -- the
# note never reached silence because the envelope stopped at the break point.
# Both are levels or times on the way down, so both belong at zero here.
#
# "Slope" only counts next to an envelope: on plenty of plugins it is the
# filter's 12/24 dB slope, which has nothing to do with this.
SIXTH_STAGE = re.compile(r"break ?point|(?=.*\b(eg|env)\b).*\bslope\b", re.I)

# Short form: an envelope name ending in a bare A/D/S/R -- "Env 1 A",
# "AmpEnv A", "P1 EnvS", "Syn EG D".
SHORT = re.compile(r"(env|eg)\s*\d*\s*([adsr])$", re.I)

STAGES = (("attack", "a"), ("decay", "d"), ("sustain", "s"), ("release", "r"))
# The PS-series abbreviates: "EM Sus", "GEG Rel", "Over Rel". Kept to whole
# words so "Dec" cannot come out of "Decimate" or "Rel" out of "Relative".
ABBREV = (("att", "a"), ("dec", "d"), ("sus", "s"), ("rel", "r"))


def stage_of(name):
    """Which envelope stage this parameter is, or None.

    Break point and slope are reported as "s": they are the extra stages of a
    six-stage envelope and want the same zero the sustain does. """
    if SIXTH_STAGE.search(name):
        return "s"
    if NOT_A_STAGE.search(name):
        return None
    for word, s in STAGES:
        if re.search(rf"\b{word}\b", name, re.I):
            return s
    # No word boundary: TAL-U-No-62 names them AMPATTACK, AMPDECAY, AMPSUSTAIN,
    # AMPRELEASE, so the stage is a suffix with nothing separating it.
    for word, s in STAGES:
        if re.search(rf"{word}$", name, re.I):
            return s
    for word, s in ABBREV:
        if re.search(rf"\b{word}\b", name, re.I):
            return s
    m = SHORT.search(name)
    return m.group(2).lower() if m else None


# Bound by name, one each, most specific first.
SINGLE = {
    "flt_c": [r"^VCF Cutoff$", r"^Filter Cutoff$", r"^VCF Freq(uency)?$",
              r"^Filter Freq(uency)?$", r"^Cutoff$", r"^VCF$", r"^Flt Freq$",
              r"^Filter$", r".*\bcutoff\b.*"],
    "flt_q": [r"^VCF Resonance$", r"^Filter Resonance$", r"^Resonance$",
              r"^Emphasis$", r"^VCF Q$", r"^Flt Res$", r".*\bresonance\b.*"],
    "noise": [r"^Noise Level$", r"^Noise$", r"^VCO Noise$", r"^Noise Volume$"],
}

# Forced to zero in every patch. These are what a plugin's default program tends
# to leave switched on, and every one of them smears a sound meant to last 40 ms.
# The effect word has to lead the name. Matching it anywhere silenced NY
# outright: the Korg Lambda is a preset machine whose sections are called
# "Volume Ensemble" and "Volume Chorus", so a pattern ending in "ensemble" or
# "chorus" turned off the very things that make it sound.
QUIET = [
    r"^(delay|reverb|chorus|ensemble|flanger|phaser)\b.*"
    r"(level|mix|amount|intensity|depth|volume)?$",
    r"^portamento\b", r"^glide\b",
    r"^(mg|lfo)\b.*\b(osc|vcf|vca|pitch|amount|depth|int|intensity)",
    r"^vibrato\s*(intensity|depth|amount)",
]

# Raised where present, so a bank is audible without hunting for the master --
# but only ever *up to* this, never past it, and never above where the plugin
# already sits. At 0.9 four banks clipped flat at 0 dBFS; at 0.65 MonoFury still
# did, because its Volume defaults to exactly 0.5 and raising it was the whole
# problem. A UI sound that clips over a game's music is worse than a quiet one,
# so this only rescues a master that was left low.
LOUD_VALUE = 0.5
LOUD = [r"^volume$", r"^total volume$", r"^master volume$", r"^level$",
        r"^output level$", r"^gain$", r"^master$"]

# Per-plugin corrections, applied last. The mapping is generic by design, and
# these are the cases where generic is not enough. Each was measured, not
# guessed -- if you add one, render it and say what the number was.
#
# A value may be a number, applied to every patch, or a dict keyed by patch
# name when the correction has to differ per sound.
OVERRIDES = {
    # Four oscillators at full Level plus an opened filter takes MonoFury past
    # 0 dBFS even with Volume left at the 0.5 it ships with, so raising nothing
    # was not enough and the master has to come down.
    "monofury64": {"Volume": 0.28},

    # MPS is a morphing pad with twelve macro knobs and exactly one envelope
    # stage -- "Release". The generic recipe therefore had a single control to
    # act on, and all seven patches came out the same 1345 ms, which read as a
    # plugin that had not loaded. It had; there was simply nothing to shorten.
    #
    # Its shaping control is the "Envelope" knob, which is an amount rather than
    # a stage, so nothing matching stage names could find it. Measured at note
    # 84, higher is shorter and the useful range is all at the top:
    #
    #   0.00-0.50 -> ~1345 ms   0.80 -> 500 ms   0.90 -> 180 ms   0.96 -> 95 ms
    #   0.75      -> 1070 ms    0.85 -> 310 ms   0.93 -> 120 ms   1.00 -> 60 ms
    # Nabla is a Korg Delta: a synth section and a string section side by side.
    # The strings have an attack and nothing else -- no decay, no sustain, no
    # release -- so they sound for as long as the key is down whatever the
    # envelope knobs say, and six of seven patches came out as the same held
    # chord at 1340-1990 ms. Turning the string section down is what makes the
    # synth envelope audible: measured, the same patch goes 1990 ms -> 25 ms.
    #
    # Its release is unreachable by name: two parameters are both called
    # "Syn EG R" (indices 23 and 33), so they are saved as index keys and no
    # amount of name matching finds them. They are addressed by index here,
    # which is exactly what index keys are for.
    #
    # VCA Mode is set to EG rather than Gate for the same reason -- on Gate the
    # amplitude follows the key rather than the envelope.
    "nabla64": {
        "VCA Mode": 1.0,
        #
        # Turning the strings off also took away the only thing that differed
        # between the factory programs, so all seven became short bright clicks:
        # measured at one key they sat inside 25-90 ms and 6589-8062 Hz, which is
        # the same sound seven times. The synth section's footages are plain
        # on/off switches (0 below half, 1 above), so giving each patch its own
        # combination is what puts the timbre back -- 2' alone is thin and high,
        # 16' alone is an octave down and dull.
        "cursor":     {"Str Vol": 0.00, "23": 0.03, "33": 0.03,
                       "SG 16'": 0.0, "SG 8'": 0.0, "SG 4'": 0.0, "SG 2'": 1.0,
                       "VCF Freq": 0.85, "SG Noise": 0.0},
        "confirm":    {"Str Vol": 0.00, "23": 0.08, "33": 0.08,
                       "SG 16'": 0.0, "SG 8'": 1.0, "SG 4'": 0.0, "SG 2'": 1.0,
                       "VCF Freq": 0.70, "SG Noise": 0.0},
        "cancel":     {"Str Vol": 0.00, "23": 0.06, "33": 0.06,
                       "SG 16'": 1.0, "SG 8'": 0.0, "SG 4'": 0.0, "SG 2'": 0.0,
                       "VCF Freq": 0.40, "SG Noise": 0.0},
        "error":      {"Str Vol": 0.00, "23": 0.10, "33": 0.10,
                       "SG 16'": 1.0, "SG 8'": 1.0, "SG 4'": 0.0, "SG 2'": 0.0,
                       "VCF Freq": 0.25, "SG Noise": 1.0},
        # The swells cannot keep any strings either. With no release stage there
        # is no such thing as a quiet string tail -- any level at all sustains
        # until the key comes up, which left both of these at ~1390 ms and
        # indistinguishable from each other. The swell comes from the synth
        # envelope's attack instead.
        "menu-open":  {"Str Vol": 0.00, "23": 0.18, "33": 0.18,
                       "SG 16'": 0.0, "SG 8'": 1.0, "SG 4'": 1.0, "SG 2'": 1.0,
                       "VCF Freq": 0.60, "SG Noise": 0.0},
        "menu-close": {"Str Vol": 0.00, "23": 0.22, "33": 0.22,
                       "SG 16'": 1.0, "SG 8'": 1.0, "SG 4'": 0.0, "SG 2'": 0.0,
                       "VCF Freq": 0.45, "SG Noise": 0.0},
        "equip":      {"Str Vol": 0.00, "23": 0.18, "33": 0.18,
                       "SG 16'": 0.0, "SG 8'": 0.0, "SG 4'": 1.0, "SG 2'": 1.0,
                       "VCF Freq": 0.90, "SG Noise": 0.0},
    },

    "mps64": {
        "cursor":     {"Envelope": 1.00},   #  60 ms
        "confirm":    {"Envelope": 0.90},   # 180 ms
        "cancel":     {"Envelope": 0.92},   # ~130 ms
        "error":      {"Envelope": 0.85},   # 310 ms
        "menu-open":  {"Envelope": 0.80},   # 500 ms
        "menu-close": {"Envelope": 0.84},   # ~340 ms
        "equip":      {"Envelope": 0.86},   # ~290 ms -- rings, not a click
    },
}

# Loads and runs, renders silence whatever it is sent. Verified here at both
# widths, with Power, Fuse, Voltage and Warm-Up all switched on and a 15-second
# render in case the warm-up simulation needed time. It did not. This matches
# what peload/README.md already records about the plugin, so a bank for it would
# only be a file that never makes a sound.
KNOWN_SILENT = {
    "brokenmini64": "renders silence at both widths even powered on "
                    "(see peload/README.md)",
}

# --------------------------------------------------------------------- recipe
#
# Every envelope in the plugin gets these four values; cutoff, resonance and
# noise are set where they were found. Missing roles are simply skipped, so a
# plugin with no filter still gets the amplitude shape -- which is most of what
# makes these read as UI sounds.
RECIPE = [
    ("cursor",     84, "selection tick -- shortest blip in the set",
     dict(a=0.00, d=0.08, s=0.0, r=0.04, flt_c=0.75, flt_q=0.12, noise=0.0)),
    ("confirm",    79, "accept -- brighter and slightly longer",
     dict(a=0.01, d=0.18, s=0.0, r=0.08, flt_c=0.68, flt_q=0.20, noise=0.0)),
    ("cancel",     67, "back out -- lower and duller",
     dict(a=0.01, d=0.15, s=0.0, r=0.06, flt_c=0.42, flt_q=0.10, noise=0.0)),
    ("error",      45, "refused -- low, resonant, a little noise",
     dict(a=0.00, d=0.30, s=0.0, r=0.10, flt_c=0.28, flt_q=0.70, noise=0.30)),
    ("menu-open",  76, "opening a panel -- swells in",
     dict(a=0.14, d=0.35, s=0.0, r=0.18, flt_c=0.55, flt_q=0.30, noise=0.0)),
    ("menu-close", 69, "closing it -- quicker in, slower out",
     dict(a=0.04, d=0.28, s=0.0, r=0.22, flt_c=0.45, flt_q=0.28, noise=0.0)),
    # A cha-ching: the transient is the "cha", the ring after it is the "ching".
    # One note cannot be two hits, so the second half has to come from a tail --
    # instant attack for the strike, a decay long enough to hear it ring rather
    # than click, resonance high so the ring is metallic, and the filter wide
    # open. A clink at decay 0.10 was too curt for this; the weighty version
    # before that, at 0.40 and note 72, was a pad.
    ("equip",      86, "equip or save -- a bright metallic cha-ching",
     dict(a=0.00, d=0.30, s=0.0, r=0.18, flt_c=0.88, flt_q=0.58, noise=0.0)),
]


def bind(names, current):
    """Return ({stage: [names]}, {role: name}, [quiet], [loud])."""
    env = {"a": [], "d": [], "s": [], "r": []}
    for n in names:
        st = stage_of(n)
        if st:
            env[st].append(n)

    single, taken = {}, set()
    for role, patterns in SINGLE.items():
        for pat in patterns:                       # priority order
            hit = next((n for n in names
                        if n not in taken and re.fullmatch(pat, n, re.I)), None)
            if hit:
                single[role] = hit
                taken.add(hit)
                break

    quiet = [n for n in names
             if any(re.search(p, n, re.I) for p in QUIET) and not stage_of(n)]
    # Only masters the plugin left below LOUD_VALUE: raising one that already
    # sits at or above it is how MonoFury got pushed into clipping.
    loud = [n for n in names
            if any(re.fullmatch(p, n, re.I) for p in LOUD)
            and current.get(n, 1.0) < LOUD_VALUE]
    return env, single, quiet, loud


def dump(dll, tmp, program=0):
    """The plugin's parameters at `program`, via a saved patch.

    Also returns how many programs it has, because the base program is spread
    across them: a patch whose every parameter came from program 0 is program 0
    with a shorter envelope, and seven of those are seven of the same sound.
    """
    if os.path.exists(tmp):
        os.remove(tmp)
    try:
        r = subprocess.run([PELOAD, dll, "--program", str(program),
                            "--save-patch", tmp],
                           capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return None, None, "timed out loading"
    if not os.path.exists(tmp):
        last = (r.stdout + r.stderr).strip().splitlines()
        return None, None, None, last[-1] if last else "no output"
    m = re.search(r"programs (\d+)", r.stdout)
    return (json.load(open(tmp)),
            "synth" if re.search(r"\bsynth\b", r.stdout) else "effect",
            int(m.group(1)) if m else 1,
            None)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    tmp = os.path.join(OUTDIR, ".probe.json")
    made, skipped = [], []

    for f in sorted(x for x in os.listdir(PLUGDIR) if x.lower().endswith(".dll")):
        stem = f[:-4]
        if stem in KNOWN_SILENT:
            skipped.append((stem, KNOWN_SILENT[stem]))
            continue
        dll = os.path.join(PLUGDIR, f)
        out_path = os.path.join(OUTDIR, f"{stem}-menu.json")
        if os.path.exists(out_path):
            try:
                if is_hand_tuned(json.load(open(out_path))):
                    skipped.append((stem, "hand-tuned -- left alone"))
                    continue
            except Exception:
                pass
        d, kind, nprog, err = dump(dll, tmp)
        if d is None:
            skipped.append((stem, f"will not load ({err})"))
            continue
        if kind != "synth":
            skipped.append((stem, "an effect, not a synth"))
            continue

        names = list(d.get("params", {}))
        env, single, quiet, loud = bind(names, d.get("params", {}))
        nenv = sum(len(v) for v in env.values())
        # Something has to make the sound stop. With no decay and no release
        # there is no shape to impose and the bank would just be the plugin's
        # current program with the filter moved.
        if not env["d"] and not env["r"]:
            skipped.append((stem, f"no envelope decay or release among "
                                  f"{len(names)} parameters"))
            continue

        # A different factory program under each patch, spread across whatever
        # the plugin ships. Filling every patch out from program 0 made them
        # deterministic but identical in timbre -- the recipe only moves the
        # envelope and the filter, which is 0-25% of a plugin's parameters, so
        # seven patches came out as one sound with seven envelopes. Starting
        # each from different sound design is what makes them differ, and it
        # needs no per-plugin knowledge: the programs are the vendor's own work.
        bases = []
        for i in range(len(RECIPE)):
            prog = round(i * (nprog - 1) / max(1, len(RECIPE) - 1)) if nprog > 1 else 0
            pd, _, _, _ = dump(dll, tmp, prog) if prog else (d, None, None, None)
            bases.append((prog, dict((pd or d).get("params", {}))))

        patches = []
        for name, note, desc, vals in RECIPE:
            params = {}
            for st in ("a", "d", "s", "r"):
                for n in env[st]:
                    params[n] = vals[st]
            for role in ("flt_c", "flt_q", "noise"):
                if role in single:
                    params[single[role]] = vals[role]
            for n in quiet:
                params[n] = 0.0
            for n in loud:
                params[n] = LOUD_VALUE
            # A flat override applies to every patch; a dict keyed by patch
            # name applies only to that one.
            ov = OVERRIDES.get(stem, {})
            params.update({k: v for k, v in ov.items()
                           if not isinstance(v, dict)})
            if isinstance(ov.get(name), dict):
                params.update(ov[name])
            prog, base = bases[len(patches)]
            patches.append({"name": name, "note": note, "description": desc,
                            # Pinned as well as complete: a program change also
                            # resets state a parameter list cannot reach.
                            "program": prog,
                            "params": complete(params, base)})

        bank = {
            "plugin": d.get("plugin", stem),
            "uniqueID": d.get("uniqueID", ""),
            "pluginPath": f"{RELBASE}/{f}",
            "description": f"Menu sounds for {d.get('plugin', stem)}, generated "
                           "by make_menu_banks.py. Play short notes.",
            # Recorded so the mapping is auditable: if a bank sounds wrong, this
            # is the first place to look, and hand-editing it is expected.
            "mapping": {
                "envelopeStages": {k: v for k, v in env.items() if v},
                "named": single,
                "silenced": quiet,
            },
            "patches": patches,
        }
        with open(os.path.join(OUTDIR, f"{stem}-menu.json"), "w") as fh:
            json.dump(bank, fh, indent=2)
            fh.write("\n")
        made.append((stem, d.get("plugin", stem), nenv,
                     len(single), len(patches[0]["params"])))

    if os.path.exists(tmp):
        os.remove(tmp)

    print(f"{'file':<18}{'plugin':<20}{'env':>5}{'named':>7}{'keys':>6}")
    for stem, plug, ne, ns, nk in made:
        print(f"{stem:<18}{plug[:19]:<20}{ne:>5}{ns:>7}{nk:>6}")
    print(f"\n{len(made)} bank(s) written to {OUTDIR}/")
    if skipped:
        print(f"\n{len(skipped)} skipped:")
        for stem, why in skipped:
            print(f"  {stem:<18} {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
