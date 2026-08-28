#!/usr/bin/env python3
"""Generate rpg-menu.json -- a bank of UI sounds for FB-7999 (Korg DW-8000).

The DW-8000 exposes its parameters as the front panel's own steps, but a VST2
parameter is a float in 0..1, so every value here is written as step/max. The
maxima were measured by setting a parameter and reading the plugin's own display
back (peload --patch ... --params), not assumed:

    0..31   most things            0..63  VCF Cutoff
    0..15   waveform, delay level  0..7   delay time
    3-way   octave 16'/8'/4'       4-way  auto bend select, mode, kbd track

Every patch writes the same full key set. A patch only sets the keys it lists,
so a partial one would inherit whatever the previous patch left behind -- switch
from "error" to "cursor" and the noise would still be up. Listing all of them
makes clicking through the bank deterministic in any order.
"""
import json

from bankutil import complete, full_param_base

N31, N63, N15, N7 = 31.0, 63.0, 15.0, 7.0

OCT_16, OCT_8, OCT_4 = 0.0, 0.5, 1.0
BEND_OFF, BEND_OSC1, BEND_OSC2, BEND_BOTH = 0.0, 1 / 3, 2 / 3, 1.0
UP, DOWN = 0.0, 1.0
POLY1 = 0.0
KBD_HALF = 2 / 3
POS = 0.0

def wave(n):        # front panel waveform 1..16
    return (n - 1) / N15


def patch(name, *, note_hint, desc, wf, octave, cutoff, res,
          bend=BEND_OFF, bend_mode=UP, bend_time=0, bend_int=0,
          noise=0, vca_decay=4, vca_release=3, vcf_eg_int=8, vcf_eg_decay=6,
          delay_level=0, detune2=3, osc2_level=0):
    """One patch, with every key the bank uses."""
    return {
        "name": name,
        "note": note_hint,          # ignored by the loader; a note for the reader
        "description": desc,
        "params": {
            "OSC1 Octave": octave,
            "OSC1 Waveform": wave(wf),
            "OSC1 Level": 31 / N31,
            "OSC2 Octave": octave,
            "OSC2 Waveform": wave(wf),
            "OSC2 Level": osc2_level / N31,
            "OSC2 Interval": 0.0,
            "OSC2 Detune": detune2 / 6.0,
            "Noise Level": noise / N31,
            "Mode": POLY1,

            "Auto Bend Select": bend,
            "Auto Bend Mode": bend_mode,
            "Auto Bend Time": bend_time / N31,
            "Auto Bend Intensity": bend_int / N31,

            "VCF Cutoff": cutoff / N63,
            "VCF Resonance": res / N31,
            "VCF KBD Track": KBD_HALF,
            "VCF EG Polarity": POS,
            "VCF EG Intensity": vcf_eg_int / N31,
            "VCF EG Attack": 0.0,
            "VCF EG Decay": vcf_eg_decay / N31,
            "VCF EG Break Point": 0.0,
            "VCF EG Slope": 0.0,
            "VCF EG Sustain": 0.0,
            "VCF EG Release": 3 / N31,

            # The blip itself: straight to peak, straight back down, nothing
            # held. Break point, slope and sustain all zero is what makes it a
            # decay rather than a note.
            "VCA EG Attack": 0.0,
            "VCA EG Decay": vca_decay / N31,
            "VCA EG Break Point": 0.0,
            "VCA EG Slope": 0.0,
            "VCA EG Sustain": 0.0,
            "VCA EG Release": vca_release / N31,

            "MG Osc": 0.0,
            "MG VCF": 0.0,
            "Delay Level": delay_level / N15,
            "Delay Time": 2 / N7,
            "Delay Feedback": 4 / N15,
            "Portamento": 0.0,
            # 1.0 is 0 dB on this plugin's own scale; the default 0.5 is -6,
            # which rendered these at -25..-16 dBFS. The patches are left at
            # different levels relative to each other on purpose -- a cursor
            # tick should sit under an equip chime rather than match it.
            "Volume": 1.0,
        },
    }


# Waveform choices come from a measured spectral centroid at A4 across all 16:
# 16 is the darkest at 911 Hz, 7 mellow at 1484, 10 the brightest at 3116.
patches = [
    patch("cursor", note_hint=84, wf=10, octave=OCT_4, cutoff=45, res=6,
          vca_decay=3, vca_release=2, vcf_eg_int=10, vcf_eg_decay=4,
          desc="the tick as the selection moves -- brightest wave, shortest decay"),

    patch("confirm", note_hint=79, wf=7, octave=OCT_8, cutoff=40, res=8,
          bend=BEND_BOTH, bend_mode=UP, bend_time=4, bend_int=12,
          vca_decay=8, vca_release=5, vcf_eg_int=12, vcf_eg_decay=7,
          desc="accept -- same blip with the pitch bending up into it"),

    patch("cancel", note_hint=72, wf=16, octave=OCT_8, cutoff=30, res=5,
          bend=BEND_BOTH, bend_mode=DOWN, bend_time=5, bend_int=14,
          vca_decay=7, vca_release=4, vcf_eg_int=8, vcf_eg_decay=6,
          desc="back out -- the darkest wave, pitch falling away"),

    patch("menu-open", note_hint=76, wf=4, octave=OCT_8, cutoff=35, res=12,
          bend=BEND_BOTH, bend_mode=UP, bend_time=10, bend_int=20,
          vca_decay=12, vca_release=8, vcf_eg_int=20, vcf_eg_decay=10,
          delay_level=5,
          desc="opening a panel -- longer sweep up, a little delay behind it"),

    patch("menu-close", note_hint=67, wf=4, octave=OCT_8, cutoff=32, res=12,
          bend=BEND_BOTH, bend_mode=DOWN, bend_time=9, bend_int=20,
          vca_decay=10, vca_release=6, vcf_eg_int=16, vcf_eg_decay=9,
          delay_level=4,
          desc="closing it -- the same gesture inverted"),

    patch("error", note_hint=48, wf=10, octave=OCT_16, cutoff=18, res=20,
          noise=10, vca_decay=16, vca_release=6, vcf_eg_int=6, vcf_eg_decay=12,
          desc="refused -- low, noisy and resonant, deliberately unpleasant"),

    # A clink, not a chord. Wave 10 is the brightest of the sixteen at 3116 Hz,
    # the octave is up, and the decay is short enough that the tail is gone
    # before it registers -- the character is all in the transient. Written as a
    # "heavier confirm" first, it measured 640 ms, which is a chime.
    patch("equip", note_hint=84, wf=10, octave=OCT_4, cutoff=52, res=14,
          bend=BEND_BOTH, bend_mode=UP, bend_time=1, bend_int=5,
          vca_decay=2, vca_release=1, vcf_eg_int=18, vcf_eg_decay=3,
          osc2_level=20, detune2=5, delay_level=0,
          desc="equip or save -- a short bright clink"),
]

bank = {
    "plugin": "FB-7999",
    "uniqueID": "0x66623739",
    "pluginPath": "../../../windows/VST2-64/fb799964.dll",
    "description": "RPG menu sounds for FB-7999 (Korg DW-8000). "
                   "Play short notes -- these are all decay, no sustain.",
    "handTuned": True,
    "patches": patches,
}

# Every patch is filled out to the plugin's whole parameter set, so selecting
# one overrides the program outright instead of inheriting the parameters it
# does not mention from whatever program happened to be selected.
PELOAD = "../peload/build/peload"
_base = full_param_base(PELOAD, "../../windows/VST2-64/fb799964.dll")
if not _base:
    raise SystemExit("could not read the plugin's parameters -- is peload built?")
for _p in patches:
    _p["program"] = 0
    _p["params"] = complete(_p["params"], _base)

with open("tuned/fb799964-menu.json", "w") as f:
    json.dump(bank, f, indent=2)
    f.write("\n")
print(f"wrote tuned/fb799964-menu.json: {len(patches)} patches, "
      f"{len(patches[0]['params'])} parameters each")
