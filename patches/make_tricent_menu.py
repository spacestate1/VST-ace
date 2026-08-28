#!/usr/bin/env python3
"""Generate tricent-menu.json -- UI sounds for Tricent MK III (Korg Trident).

A different machine needs a different approach, not the DW-8000 bank transposed.
The Trident has no Auto Bend, so the pitch-sweep trick that made "confirm" and
"cancel" one another inverted is simply unavailable. What it has instead is
three sections that play at once -- Synthe, Brass and Strings, each with its own
filter, envelope and output switch -- so here the sections *are* the vocabulary:

    Synthe    a real subtractive voice: two VCOs, resonant VCF, ADSR. The blips.
    Brass     fast attack, its own filter EG. The confirms.
    Strings   a divide-down ensemble with only Attack and Release. The swells.

Values were measured rather than assumed, by setting a parameter and reading the
plugin's own display back (peload --patch ... --params):

    VCF Cutoff   exponential: 0.0 -> 8 Hz, 0.3 -> 585, 0.5 -> 2030,
                 0.75 -> 7009, 1.0 -> 19912 Hz
    VCO Octave   16' = 0.0, 8' = 0.5, 4' = 1.0
    VCO 1 Wave   Sawtooth = 0.0, Pulse = 0.5, Pulse Width Mod = 1.0
    Range        Lower = 0.0, Both = 0.5, Upper = 1.0
    Output X     Off below 0.5, On above

The envelope parameters are the exception: the plugin renders their display as
an integer, so it reads "0" for everything until it reaches 1. Those were tuned
by rendering and measuring the result instead -- see the table printed by
check_tricent.py.

One trap worth naming: a VCA Attack of exactly 0 produces a click louder than
the note behind it, which is fine for a cursor tick and wrong for anything
tonal. The tonal patches use a small non-zero attack.

Every patch writes the same full key set. A patch only sets the keys it lists,
so a partial one inherits whatever the previous patch left behind -- switch from
a Strings swell to a Synthe blip and the strings would still be sounding.
"""
import json

from bankutil import complete, full_param_base

OFF, ON = 0.0, 1.0
LOWER, BOTH, UPPER = 0.0, 0.5, 1.0
OCT16, OCT8, OCT4 = 0.0, 0.5, 1.0
SAW, PULSE, PWM = 0.0, 0.5, 1.0


def patch(name, *, note_hint, desc,
          # which sections sound
          synthe=OFF, brass=OFF, strings=OFF,
          # Section volumes are a dB scale -- the plugin's own defaults sit
          # around 0.40 for -7 dB. Rendered at 0.80 with Total Volume at 1.0,
          # four of these eight patches clipped flat at 0 dBFS, so the whole
          # bank runs quieter: see the Total Volume note below.
          vol_syn=0.46, vol_brass=0.46, vol_str=0.46,
          # synthe voice
          wave=PULSE, octave=OCT8, oct2=OCT8, detune2=0.53, pw=0.35,
          cutoff=0.60, res=0.0, vcf_eg=0.0,
          vcf_a=0.0, vcf_d=0.20, vcf_s=0.0, vcf_r=0.05,
          vca_a=0.0, vca_d=0.12, vca_s=0.0, vca_r=0.04,
          # brass voice
          br_cut=0.45, br_res=0.0, br_eg=0.55,
          br_a=0.02, br_d=0.25, br_s=0.0, br_r=0.10,
          br16=ON, br8=OFF,
          # strings voice
          st_a=0.05, st_r=0.15, st_ens=OFF, st_vib=0.0,
          st16=OFF, st8=ON, st4=OFF, st_eq_hi=0.5,
          # effects
          flanger=OFF, fl_int=0.5, fl_speed=0.25):
    return {
        "name": name,
        "note": note_hint,
        "description": desc,
        "params": {
            "Trident Mode": ON,
            "Key Assign": 1.0,
            "Total Tune": 0.5,
            "Split Key": 0.4724,
            "Range Synthe": BOTH,
            "Range Brass": BOTH,
            "Range Strings": BOTH,

            "VCO 1 Octave": octave,
            "VCO 1 Wave": wave,
            "VCO 1 PW/PWM": pw,
            "VCO 1 PWM Speed": 0.25,
            "VCO 2 Octave": oct2,
            "VCO 2 Detune": detune2,
            "VCF Cutoff": cutoff,
            "VCF Resonance": res,
            "VCF KBF Track": 0.5,
            "VCF EG Intensity": vcf_eg,
            "VCF Attack": vcf_a,
            "VCF Decay": vcf_d,
            "VCF Sustain": vcf_s,
            "VCF Release": vcf_r,
            "VCA Attack": vca_a,
            "VCA Decay": vca_d,
            "VCA Sustain": vca_s,
            "VCA Release": vca_r,
            "VCA Attenuator": 0.4,
            "VCA Auto Damp": OFF,

            "Brass Cutoff": br_cut,
            "Brass Resonance": br_res,
            "Brass EG Intensity": br_eg,
            "Brass Attack": br_a,
            "Brass Decay": br_d,
            "Brass Sustain": br_s,
            "Brass Release": br_r,
            "Brass 16'": br16,
            "Brass 8'": br8,
            "Brass Multi Trigger": ON,

            "Strings Attack": st_a,
            "Strings Release": st_r,
            "Strings EQ High": st_eq_hi,
            "Strings EQ Low": 0.5,
            "Strings Bowing Level": 0.5,
            "Strings Vibrato Intensity": st_vib,
            "Strings 16'": st16,
            "Strings 8'": st8,
            "Strings 4'": st4,
            "Strings Ensemble": st_ens,

            "Output Synthe": synthe,
            "Output Brass": brass,
            "Output Strings": strings,
            "Volume Synthe": vol_syn,
            "Volume Brass": vol_brass,
            "Volume Strings": vol_str,
            # 0.5 is -6 dB on this plugin's scale. Left with headroom on
            # purpose: these get triggered on top of music and a UI sound that
            # clips is worse than one that is quiet.
            "Total Volume": 0.55,

            "Flanger Synthe": flanger,
            "Flanger Brass": OFF,
            "Flanger Strings": flanger,
            "Flanger Intensity": fl_int,
            "Flanger Speed": fl_speed,
            "Vib. Intensity": 0.0,
        },
    }


patches = [
    # --- Synthe section: the blips -------------------------------------------
    patch("cursor", note_hint=84, synthe=ON,
          wave=PULSE, octave=OCT4, oct2=OCT4, pw=0.30,
          cutoff=0.72, res=0.15, vcf_eg=0.30, vcf_d=0.10,
          vca_a=0.0, vca_d=0.06, vca_s=0.0, vca_r=0.02,
          desc="selection tick -- Synthe alone, pulse at 4', instant attack "
               "so the click is part of it"),

    patch("confirm", note_hint=79, synthe=ON,
          wave=PULSE, octave=OCT8, oct2=OCT8, detune2=0.56, pw=0.38,
          cutoff=0.66, res=0.20, vcf_eg=0.55, vcf_d=0.22,
          vca_a=0.01, vca_d=0.18, vca_s=0.0, vca_r=0.06,
          desc="accept -- second oscillator detuned, filter envelope opening it"),

    patch("cancel", note_hint=67, synthe=ON,
          wave=SAW, octave=OCT16, oct2=OCT8, detune2=0.47,
          cutoff=0.42, res=0.10, vcf_eg=0.20, vcf_d=0.16,
          vca_a=0.01, vca_d=0.14, vca_s=0.0, vca_r=0.05,
          desc="back out -- sawtooth an octave down, filter kept shut"),

    patch("error", note_hint=45, synthe=ON,
          wave=PULSE, octave=OCT16, oct2=OCT16, detune2=0.62, pw=0.12,
          cutoff=0.30, res=0.72, vcf_eg=0.15, vcf_d=0.30,
          vca_a=0.0, vca_d=0.30, vca_s=0.0, vca_r=0.08,
          desc="refused -- low, narrow pulse, resonance up and the filter shut"),

    # --- Brass section: the confirms that need weight ------------------------
    # The Brass section can be percussive if its envelope is short enough: fast
    # attack, almost no decay, filter well open. At decay 0.30 it measured
    # 265 ms of brass swell, which is a fanfare rather than a clink.
    patch("equip", note_hint=84, brass=ON, synthe=ON, vol_brass=0.46, vol_syn=0.40,
          br_cut=0.62, br_eg=0.80, br_a=0.0, br_d=0.06, br_s=0.0, br_r=0.03,
          br16=OFF, br8=ON,
          wave=PULSE, octave=OCT4, oct2=OCT4, pw=0.30,
          cutoff=0.80, res=0.30, vcf_eg=0.40, vcf_d=0.05,
          vca_a=0.0, vca_d=0.05, vca_s=0.0, vca_r=0.02,
          desc="equip or save -- a short bright clink"),

    patch("fanfare", note_hint=76, brass=ON, synthe=ON,
          vol_brass=0.50, vol_syn=0.34,
          wave=PULSE, octave=OCT8, oct2=OCT8, detune2=0.55,
          cutoff=0.62, res=0.10, vcf_eg=0.50, vcf_d=0.30,
          vca_a=0.02, vca_d=0.35, vca_s=0.0, vca_r=0.16,
          br_cut=0.58, br_eg=0.75, br_a=0.03, br_d=0.38, br_s=0.0, br_r=0.18,
          br16=ON, br8=ON,
          desc="level up -- Brass and Synthe together, the machine's signature "
               "layered sound"),

    # --- Strings section: the swells -----------------------------------------
    patch("menu-open", note_hint=76, strings=ON, vol_str=0.50,
          st_a=0.18, st_r=0.22, st_ens=ON, st_vib=0.15,
          st16=OFF, st8=ON, st4=ON, st_eq_hi=0.68,
          flanger=ON, fl_int=0.55, fl_speed=0.30,
          desc="opening a panel -- Strings swelling in through the ensemble, "
               "flanger behind it"),

    patch("menu-close", note_hint=69, strings=ON, vol_str=0.46,
          st_a=0.06, st_r=0.30, st_ens=ON, st_vib=0.08,
          st16=ON, st8=ON, st4=OFF, st_eq_hi=0.40,
          flanger=ON, fl_int=0.45, fl_speed=0.22,
          desc="closing it -- lower and duller, quicker in and slower out"),
]

bank = {
    "plugin": "Tricent",
    "uniqueID": "0x54726933",
    "pluginPath": "../../../windows/VST2-64/tricent64.dll",
    "description": "UI sounds for Tricent MK III (Korg Trident). Built around "
                   "the three sections: Synthe blips, Brass confirms, Strings "
                   "swells. Play short notes.",
    "handTuned": True,
    "patches": patches,
}

# Every patch is filled out to the plugin's whole parameter set, so selecting
# one overrides the program outright instead of inheriting the parameters it
# does not mention from whatever program happened to be selected.
PELOAD = "../peload/build/peload"
_base = full_param_base(PELOAD, "../../windows/VST2-64/tricent64.dll")
if not _base:
    raise SystemExit("could not read the plugin's parameters -- is peload built?")
for _p in patches:
    _p["program"] = 0
    _p["params"] = complete(_p["params"], _base)

with open("tuned/tricent64-menu.json", "w") as f:
    json.dump(bank, f, indent=2)
    f.write("\n")
print(f"wrote tuned/tricent64-menu.json: {len(patches)} patches, "
      f"{len(patches[0]['params'])} parameters each")
