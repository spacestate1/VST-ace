#!/usr/bin/env python3
"""Hand-tuned menu banks, one entry per machine.

The generated banks map roles onto names and then measure their way out of
trouble. That gets a bank that works; it does not get a bank that sounds like
the machine it is on, because the recipe only ever moves envelopes, a cutoff and
a resonance. Everything that gives a synth its character -- sync, FM, footages,
sections, wave choice -- goes untouched.

So each machine gets its own entry here, written against its own architecture
after reading its parameter list and measuring the scalings that matter. The
rules a machine has to satisfy before it is added:

  * all seven sounds stop on their own, with the key held
  * all seven are audibly distinct **at one key**, since that is how they are
    played -- differing only in the note they are rendered at does not count
  * nothing clips, nothing is inaudible

Values are the plugin's own 0..1. Where a scaling was measured rather than
guessed, the numbers are in the comment above the machine.

Adding a machine: append an entry, run `python3 make_tuned.py <stem>`, read the
table it prints, adjust, repeat. The output carries "handTuned": true so no
generator pass will overwrite it.
"""

# Each machine: dll stem -> dict with
#   plugin   display name, as the plugin reports it
#   program  base program every patch sits on
#   common   parameters shared by all seven
#   patches  name -> {note, description, params}

MACHINES = {}

# ---------------------------------------------------------------------- Kern
#
# 32 parameters, all named plainly and mostly in real units -- the easiest in
# the collection to write for. Measured:
#
#   Osc Wave   Saw below 0.5, Square above        Flt Mode  Smooth / Dirty at 0.5
#   AmpEnv S   Off below 0.5, On above -- a switch, not a level
#   Flt Freq   exponential: 0.25 -> 403 Hz, 0.5 -> 2029, 0.75 -> 7006
#   percentages are linear: Flt Reso 0.77 reads 77%
#   AmpEnv D   0.05 -> 20 ms, 0.15 -> 45, 0.20 -> 70, 0.30 -> 185, 0.40 -> 500,
#              0.55 and up holds until note-off
#
# What makes Kern sound like Kern is the second oscillator: hard sync plus FM
# with the envelope as source. That is the lever the generated bank never
# touched, and it is what separates these seven from each other.
MACHINES["kern64"] = {
    "plugin": "Kern",
    "program": 0,
    "common": {
        "Volume": 0.62, "Monophon": 0.0, "Portamnt": 0.0, "M. Tune": 0.5,
        "Amp Velo": 0.0, "Flt Velo": 0.5, "Flt LFO": 0.5, "Flt Note": 0.5,
        "AmpEnv A": 0.0, "AmpEnv S": 0.0,       # S off: decay to silence
        "FltEnv A": 0.0, "FltEnv S": 0.0,
        "LFO Freq": 0.51, "LFO Wave": 0.0,
        "Cho LFO1": 0.07, "Cho LFO2": 0.13, "Cho Dpt": 0.20,
    },
    "patches": {
        # 20 ms was too short to read as a tick -- at middle C that is about
        # five cycles, which lands as a click with no pitch to it. Tested the
        # three things that would have measured as defects and none was: the
        # transient ratio is 0.78 whatever the envelope, hard sync made the
        # timbre *less* consistent across the keyboard (centroid 3501 Hz at key
        # 60 against 1715 at key 72), and filter key-tracking barely moved it.
        # So it is simply longer now, with the filter fixed rather than tracking
        # and a hair of attack to take the edge off the front.
        "cursor": dict(note=84, description="selection tick -- square, fixed filter, short but pitched",
            params={"Osc Wave": 1.0, "Flt Mode": 0.0, "Sync": 0.0,
                    "FM": 0.0, "FM Src": 1.0, "Osc2Trns": 0.5, "Osc2Tune": 0.5,
                    "Flt Note": 0.0,
                    "Flt Freq": 0.76, "Flt Reso": 0.20, "Flt Env": 0.0,
                    "FltEnv D": 0.10, "AmpEnv A": 0.01, "AmpEnv D": 0.13,
                    "AmpEnv R": 0.06, "Chorus": 0.0}),
        "confirm": dict(note=79, description="accept -- a little envelope FM for bite",
            params={"Osc Wave": 1.0, "Flt Mode": 1.0, "Sync": 0.0,
                    "FM": 0.22, "FM Src": 1.0, "Osc2Trns": 0.5, "Osc2Tune": 0.54,
                    "Flt Freq": 0.68, "Flt Reso": 0.38, "Flt Env": 0.55,
                    "FltEnv D": 0.22, "AmpEnv D": 0.26, "AmpEnv R": 0.08,
                    "Chorus": 0.0}),
        "cancel": dict(note=67, description="back out -- sawtooth, filter kept down",
            params={"Osc Wave": 0.0, "Flt Mode": 0.0, "Sync": 0.0,
                    "FM": 0.0, "FM Src": 1.0, "Osc2Trns": 0.42, "Osc2Tune": 0.5,
                    "Flt Freq": 0.26, "Flt Reso": 0.20, "Flt Env": 0.15,
                    "FltEnv D": 0.16, "AmpEnv D": 0.22, "AmpEnv R": 0.07,
                    "Chorus": 0.0}),
        "error": dict(note=45, description="refused -- sync and heavy FM, filter shut, resonant",
            params={"Osc Wave": 0.0, "Flt Mode": 1.0, "Sync": 1.0,
                    "FM": 0.62, "FM Src": 1.0, "Osc2Trns": 0.62, "Osc2Tune": 0.44,
                    "Flt Freq": 0.30, "Flt Reso": 0.78, "Flt Env": 0.20,
                    "FltEnv D": 0.30, "AmpEnv D": 0.33, "AmpEnv R": 0.08,
                    "Chorus": 0.0}),
        "menu-open": dict(note=76, description="opening -- swells in, filter sweeping up, chorus behind",
            params={"Osc Wave": 1.0, "Flt Mode": 0.0, "Sync": 0.0,
                    "FM": 0.10, "FM Src": 0.0, "Osc2Trns": 0.5, "Osc2Tune": 0.53,
                    "Flt Freq": 0.38, "Flt Reso": 0.42, "Flt Env": 0.88,
                    "FltEnv D": 0.38, "AmpEnv A": 0.12, "AmpEnv D": 0.36,
                    "AmpEnv R": 0.10, "Chorus": 1.0}),
        "menu-close": dict(note=69, description="closing -- the same voice lower and duller",
            params={"Osc Wave": 0.0, "Flt Mode": 0.0, "Sync": 0.0,
                    "FM": 0.08, "FM Src": 0.0, "Osc2Trns": 0.46, "Osc2Tune": 0.47,
                    "Flt Freq": 0.50, "Flt Reso": 0.35, "Flt Env": 0.20,
                    "FltEnv D": 0.24, "AmpEnv A": 0.02, "AmpEnv D": 0.30,
                    "AmpEnv R": 0.09, "Chorus": 1.0}),
        "equip": dict(note=86, description="cha-ching -- sync plus FM into a resonant open filter",
            params={"Osc Wave": 1.0, "Flt Mode": 1.0, "Sync": 1.0,
                    "FM": 0.45, "FM Src": 1.0, "Osc2Trns": 0.66, "Osc2Tune": 0.52,
                    "Flt Freq": 0.78, "Flt Reso": 0.58, "Flt Env": 0.45,
                    "FltEnv D": 0.30, "AmpEnv D": 0.33, "AmpEnv R": 0.12,
                    "Chorus": 0.0}),
    },
}


# ------------------------------------------------------------------- Fury68
#
# Korg Poly-61: two DCOs into one filter, and an unusual split -- EG1 is the
# filter envelope, EG2 the amplifier, selected by "VCA EG Mode" (EG2 at 0.0).
# Setting the wrong one is how a patch ends up ignoring every envelope value it
# was given. Measured:
#
#   DCO1 Octave     16' 0.0 / 8' 0.33 / 4' 1.0     VCF Type   12dB 0.0 / 24dB 0.5
#   DCO1 Waveform   Saw 0.0 / PW 0.33 / PWM 1.0
#   DCO2 Waveform   Off 0.0 / Saw 0.33 / Square 1.0
#   Effects Mode    Off 0.0 / Chorus 0.33 / Phaser 0.5 / Ensemble 1.0
#   envelope steps are 0..15, so a value is n/15
#   EG2 Decay       0.10 -> 15 ms, 0.20 -> 40, 0.30 -> 60, 0.45 -> 115, 0.60 -> 320
#
# The character here is the second DCO and the effects section: switching DCO2
# between off, sawtooth and square, and picking chorus or ensemble behind the
# longer sounds, is what separates these more than the filter does.
MACHINES["fury6864"] = {
    "plugin": "Fury68",
    "program": 0,
    "common": {
        "Volume": 0.62, "Tune": 0.5, "Mode": 0.0, "Spread": 0.0,
        "VCA EG Mode": 0.0,                 # amp follows EG2
        "EG2 Attack": 0.0, "EG2 Sustain": 0.0,
        "EG1 Attack": 0.0, "EG1 Sustain": 0.0,
        "VCF KBD Track": 0.33,
        "MG DCO": 0.0, "MG VCF": 0.0, "MG Delay": 0.0,
        "DCO1 PWM Frequency": 0.0,
    },
    "patches": {
        "cursor": dict(note=84, description="selection tick -- one DCO at 4', pulse, 24 dB filter open",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 0.33, "DCO1 PW/PWM": 0.35,
                    "DCO2 Waveform": 0.0, "DCO2 Octave": 1.0, "DCO2 Detune": 0.5,
                    "DCO2 Interval": 0.0, "VCF Type": 0.5,
                    "VCF Cutoff": 0.82, "VCF Resonance": 0.20, "VCF EG Intensity": 0.30,
                    "EG1 Decay": 0.12, "EG1 Release": 0.03,
                    "EG2 Decay": 0.10, "EG2 Release": 0.03,
                    "Effects Mode": 0.0}),
        "confirm": dict(note=79, description="accept -- square second DCO underneath, 12 dB filter",
            params={"DCO1 Octave": 0.33, "DCO1 Waveform": 0.0, "DCO1 PW/PWM": 0.0,
                    "DCO2 Waveform": 1.0, "DCO2 Octave": 0.33, "DCO2 Detune": 0.62,
                    "DCO2 Interval": 0.0, "VCF Type": 0.0,
                    "VCF Cutoff": 0.70, "VCF Resonance": 0.30, "VCF EG Intensity": 0.55,
                    "EG1 Decay": 0.25, "EG1 Release": 0.06,
                    "EG2 Decay": 0.26, "EG2 Release": 0.06,
                    "Effects Mode": 0.0}),
        "cancel": dict(note=67, description="back out -- sawtooth at 16', filter kept down",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.0, "DCO1 PW/PWM": 0.0,
                    "DCO2 Waveform": 0.0, "DCO2 Octave": 0.0, "DCO2 Detune": 0.5,
                    "DCO2 Interval": 0.0, "VCF Type": 0.0,
                    "VCF Cutoff": 0.46, "VCF Resonance": 0.22, "VCF EG Intensity": 0.25,
                    "EG1 Decay": 0.20, "EG1 Release": 0.05,
                    "EG2 Decay": 0.22, "EG2 Release": 0.05,
                    "Effects Mode": 0.0}),
        "error": dict(note=45, description="refused -- PWM against a detuned square, resonant and shut",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 1.0, "DCO1 PW/PWM": 0.15,
                    "DCO2 Waveform": 1.0, "DCO2 Octave": 0.0, "DCO2 Detune": 0.80,
                    "DCO2 Interval": 0.0, "VCF Type": 0.5,
                    "VCF Cutoff": 0.28, "VCF Resonance": 0.85, "VCF EG Intensity": 0.20,
                    "EG1 Decay": 0.35, "EG1 Release": 0.08,
                    "EG2 Decay": 0.50, "EG2 Release": 0.08,
                    "Effects Mode": 0.0}),
        "menu-open": dict(note=76, description="opening -- swells in through the ensemble, filter sweeping up",
            params={"DCO1 Octave": 0.33, "DCO1 Waveform": 1.0, "DCO1 PW/PWM": 0.5,
                    "DCO2 Waveform": 0.33, "DCO2 Octave": 0.33, "DCO2 Detune": 0.58,
                    "DCO2 Interval": 0.0, "VCF Type": 0.0,
                    "VCF Cutoff": 0.40, "VCF Resonance": 0.40, "VCF EG Intensity": 0.85,
                    "EG1 Attack": 0.10, "EG1 Decay": 0.45, "EG1 Release": 0.10,
                    "EG2 Attack": 0.14, "EG2 Decay": 0.58, "EG2 Release": 0.10,
                    "Effects Mode": 1.0}),
        "menu-close": dict(note=69, description="closing -- the same voice lower, chorus rather than ensemble",
            params={"DCO1 Octave": 0.33, "DCO1 Waveform": 0.0, "DCO1 PW/PWM": 0.0,
                    "DCO2 Waveform": 0.33, "DCO2 Octave": 0.0, "DCO2 Detune": 0.42,
                    "DCO2 Interval": 0.0, "VCF Type": 0.0,
                    "VCF Cutoff": 0.52, "VCF Resonance": 0.32, "VCF EG Intensity": 0.20,
                    "EG1 Decay": 0.30, "EG1 Release": 0.08,
                    "EG2 Attack": 0.03, "EG2 Decay": 0.48, "EG2 Release": 0.09,
                    "Effects Mode": 0.33}),
        "equip": dict(note=86, description="cha-ching -- pulse at 4' with a square a fifth up, resonant",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 0.33, "DCO1 PW/PWM": 0.25,
                    "DCO2 Waveform": 1.0, "DCO2 Octave": 1.0, "DCO2 Detune": 0.55,
                    "DCO2 Interval": 0.85, "VCF Type": 0.5,
                    "VCF Cutoff": 0.80, "VCF Resonance": 0.60, "VCF EG Intensity": 0.45,
                    "EG1 Decay": 0.30, "EG1 Release": 0.10,
                    "EG2 Decay": 0.55, "EG2 Release": 0.14,
                    "Effects Mode": 0.0}),
    },
}


# ----------------------------------------------------------------- MonoFury
#
# Korg Mono/Poly: four oscillators, each with its own wave, octave, level and
# tune, and a cross-modulation section that runs them into each other. That is
# the machine -- one filter and one envelope pair is the least of it -- so these
# seven differ mainly in how many oscillators sound and at what footings.
# Measured:
#
#   Wave n     Triangle 0.0 / Sawtooth 0.33 / PWM 0.67 / PW 1.0
#   Octave n   16' 0.0 / 8' 0.33 / 4' 0.67 / 2' 1.0
#   KeyAssgn   Unison 0.0 / RndRobin 0.33 / Chord 0.5 / Unison-Share 0.67 / Poly 1.0
#   Effects    Off below 0.5, On above          FX Type  FM / X-Mod
#   EG2 is the amplifier, EG1 the filter -- with EG1 shortened and EG2 left
#   alone the note ran 2990 ms, which is how that was settled
#   EG2 D      0.10 -> 35 ms, 0.20 -> 90, 0.30 -> 285, 0.40 -> 925, 0.55 -> holds
#
# Four oscillators at full level clip this plugin: at Volume 0.5 with all four
# up it rendered flat at 0 dBFS, so the master sits low and the levels are
# spent deliberately rather than all being opened.
MACHINES["monofury64"] = {
    "plugin": "MonoFury",
    "program": 0,
    "common": {
        "Volume": 0.42, "MstrTune": 0.5, "Trnspose": 0.5, "Portamnt": 0.0,
        "KeyAssgn": 0.0, "Trigger": 0.0, "AutoDamp": 0.0, "Hold": 0.0,
        "EG2 A": 0.0, "EG2 S": 0.0, "EG1 A": 0.0, "EG1 S": 0.0,
        "KeyTrack": 0.0,            # fixed filter: a UI sound should not change
                                    # character with the key it is played at
        "MWIntens": 0.0, "PBIntens": 0.4, "ARPState": 1.0,
        "VCF Velo": 0.0, "VCA Velo": 0.0, "Noise": 0.0,
        "Tune 2": 0.5, "Tune 3": 0.5, "Tune 4": 0.5,
    },
    "patches": {
        "cursor": dict(note=84, description="selection tick -- one pulse oscillator at 2'",
            params={"Wave 1": 1.0, "Octave 1": 1.0, "Level 1": 0.70,
                    "Level 2": 0.0, "Level 3": 0.0, "Level 4": 0.0,
                    "PW": 0.35, "PWM": 0.0, "Effects": 0.0,
                    "CutOff": 0.78, "Reso": 0.20, "EGIntens": 0.45,
                    "EG1 D": 0.15, "EG1 R": 0.04,
                    "EG2 D": 0.12, "EG2 R": 0.04}),
        "confirm": dict(note=79, description="accept -- two pulses an octave apart, lightly detuned",
            params={"Wave 1": 1.0, "Octave 1": 0.67, "Level 1": 0.60,
                    "Wave 2": 1.0, "Octave 2": 1.0, "Level 2": 0.45,
                    "Level 3": 0.0, "Level 4": 0.0, "Tune 2": 0.56,
                    "PW": 0.40, "PWM": 0.0, "Effects": 0.0,
                    "CutOff": 0.68, "Reso": 0.30, "EGIntens": 0.60,
                    "EG1 D": 0.22, "EG1 R": 0.06,
                    "EG2 D": 0.20, "EG2 R": 0.06}),
        "cancel": dict(note=67, description="back out -- a single sawtooth at 16', filter down",
            params={"Wave 1": 0.33, "Octave 1": 0.0, "Level 1": 0.70,
                    "Level 2": 0.0, "Level 3": 0.0, "Level 4": 0.0,
                    "PW": 0.5, "PWM": 0.0, "Effects": 0.0,
                    "CutOff": 0.44, "Reso": 0.22, "EGIntens": 0.30,
                    "EG1 D": 0.20, "EG1 R": 0.05,
                    "EG2 D": 0.17, "EG2 R": 0.05}),
        "error": dict(note=45, description="refused -- all four oscillators detuned into cross-modulation",
            params={"Wave 1": 0.33, "Octave 1": 0.0, "Level 1": 0.45,
                    "Wave 2": 0.33, "Octave 2": 0.0, "Level 2": 0.40,
                    "Wave 3": 1.0, "Octave 3": 0.33, "Level 3": 0.35,
                    "Wave 4": 0.0, "Octave 4": 0.0, "Level 4": 0.30,
                    "Tune 2": 0.62, "Tune 3": 0.38, "Tune 4": 0.66,
                    "PW": 0.20, "PWM": 0.0,
                    "Effects": 1.0, "FX Type": 1.0, "FX X-Mod": 0.55, "FX FM": 0.0,
                    "CutOff": 0.28, "Reso": 0.72, "EGIntens": 0.25,
                    "EG1 D": 0.30, "EG1 R": 0.08,
                    "EG2 D": 0.28, "EG2 R": 0.08}),
        "menu-open": dict(note=76, description="opening -- three oscillators swelling in, filter sweeping up",
            params={"Wave 1": 0.33, "Octave 1": 0.33, "Level 1": 0.45,
                    "Wave 2": 0.67, "Octave 2": 0.67, "Level 2": 0.40,
                    "Wave 3": 0.0, "Octave 3": 1.0, "Level 3": 0.30,
                    "Level 4": 0.0, "Tune 2": 0.54, "Tune 3": 0.47,
                    "PW": 0.5, "PWM": 0.35, "PWM Src": 1.0, "Effects": 0.0,
                    "CutOff": 0.36, "Reso": 0.40, "EGIntens": 0.85,
                    "EG1 A": 0.10, "EG1 D": 0.40, "EG1 R": 0.10,
                    "EG2 A": 0.13, "EG2 D": 0.33, "EG2 R": 0.10}),
        "menu-close": dict(note=69, description="closing -- two low oscillators, filter shutting",
            params={"Wave 1": 0.33, "Octave 1": 0.0, "Level 1": 0.55,
                    "Wave 2": 0.0, "Octave 2": 0.33, "Level 2": 0.40,
                    "Level 3": 0.0, "Level 4": 0.0, "Tune 2": 0.44,
                    "PW": 0.5, "PWM": 0.0, "Effects": 0.0,
                    "CutOff": 0.50, "Reso": 0.32, "EGIntens": 0.20,
                    "EG1 D": 0.26, "EG1 R": 0.08,
                    "EG2 A": 0.03, "EG2 D": 0.27, "EG2 R": 0.09}),
        "equip": dict(note=86, description="cha-ching -- two high pulses through FM, resonant and open",
            params={"Wave 1": 1.0, "Octave 1": 1.0, "Level 1": 0.55,
                    "Wave 2": 1.0, "Octave 2": 0.67, "Level 2": 0.40,
                    "Level 3": 0.0, "Level 4": 0.0, "Tune 2": 0.63,
                    "PW": 0.25, "PWM": 0.0,
                    "Effects": 1.0, "FX Type": 0.0, "FX FM": 0.45, "FX X-Mod": 0.0,
                    "CutOff": 0.78, "Reso": 0.58, "EGIntens": 0.45,
                    "EG1 D": 0.28, "EG1 R": 0.10,
                    "EG2 D": 0.29, "EG2 R": 0.12}),
    },
}


# --------------------------------------------------------------- Bucket ONE
#
# Crumar Bit 01. Two DCOs, but the waveforms are not a selector -- Triangle,
# Sawtooth and Pulse are three independent on/off switches per oscillator, so
# they stack. Mixing them is the machine's own way of changing timbre and it is
# what separates these seven more than the filter does. Measured:
#
#   DCO Octave    32' 0.0 / 16' 0.33 / 8' 0.67 / 4' 1.0
#   values run 0..63, so a step is n/63
#   VCA Decay     0.15 -> 30 ms, 0.25 -> 75, 0.35 -> 200, 0.45 -> 555,
#                 0.60 -> 1640 (holds)
#
# It also has "VCF Invert Envelope", which is the tidiest way to get a closing
# sweep out of a filter envelope rather than faking one with a slow attack.
#
# "Lower Volume" and "Upper Volume" each appear twice in the parameter list, so
# they arrive as index keys and cannot be set by name. Nothing here needs them.
MACHINES["bucketone64"] = {
    "plugin": "Bucket ONE",
    "program": 0,
    "common": {
        "Tune": 0.5, "Keyboard Mode": 0.0, "Voices": 0.0,
        "VCA Attack": 0.0, "VCA Sustain": 0.0,
        "VCF Attack": 0.0, "VCF Sustain": 0.0, "VCF Track": 0.0,
        "VCF Attack Dynamic": 0.0, "VCF Envelope Dynamic": 0.0,
        "VCA Attack Dynamic": 0.0, "VCA Amount Dynamic": 0.0,
        "VCA Program Volume": 0.62,
        "LFO 1 to DCO 1": 0.0, "LFO 1 to DCO 2": 0.0,
        "LFO 1 to VCF": 0.0, "LFO 1 to VCA": 0.0, "LFO 1 Depth": 0.0,
        "LFO 2 to DCO 1": 0.0, "LFO 2 to DCO 2": 0.0,
        "LFO 2 to VCF": 0.0, "LFO 2 to VCA": 0.0, "LFO 2 Depth": 0.0,
        "DCO 1 Pulse Width Dynamic": 0.0, "DCO 2 Pulse Width Dynamic": 0.0,
        "DCO 1 Frequency": 0.0, "DCO 2 Frequency": 0.0,
    },
    "patches": {
        "cursor": dict(note=84, description="selection tick -- one narrow pulse at 4'",
            params={"DCO 1 Octave": 1.0, "DCO 1 Pulse": 1.0, "DCO 1 Triangle": 0.0,
                    "DCO 1 Sawtooth": 0.0, "DCO 1 Pulse Width": 0.20, "DCO 1 Noise": 0.0,
                    "DCO 2 Pulse": 0.0, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 0.0,
                    "DCO 2 Detune": 0.0, "VCF Invert Envelope": 0.0,
                    "VCF Cutoff": 0.80, "VCF Resonance": 0.15, "VCF Envelope": 0.30,
                    "VCF Decay": 0.18, "VCF Release": 0.05,
                    "VCA Decay": 0.16, "VCA Release": 0.05}),
        "confirm": dict(note=79, description="accept -- pulse and triangle stacked, a second DCO above",
            params={"DCO 1 Octave": 0.67, "DCO 1 Pulse": 1.0, "DCO 1 Triangle": 1.0,
                    "DCO 1 Sawtooth": 0.0, "DCO 1 Pulse Width": 0.35, "DCO 1 Noise": 0.0,
                    "DCO 2 Octave": 1.0, "DCO 2 Pulse": 1.0, "DCO 2 Triangle": 0.0,
                    "DCO 2 Sawtooth": 0.0, "DCO 2 Pulse Width": 0.30, "DCO 2 Detune": 0.10,
                    "VCF Invert Envelope": 0.0,
                    "VCF Cutoff": 0.68, "VCF Resonance": 0.28, "VCF Envelope": 0.55,
                    "VCF Decay": 0.28, "VCF Release": 0.07, "VCA Program Volume": 0.42,
                    "VCA Decay": 0.27, "VCA Release": 0.07}),
        "cancel": dict(note=67, description="back out -- a lone sawtooth at 16', filter down",
            params={"DCO 1 Octave": 0.33, "DCO 1 Pulse": 0.0, "DCO 1 Triangle": 0.0,
                    "DCO 1 Sawtooth": 1.0, "DCO 1 Pulse Width": 0.5, "DCO 1 Noise": 0.0,
                    "DCO 2 Pulse": 0.0, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 0.0,
                    "DCO 2 Detune": 0.0, "VCF Invert Envelope": 0.0,
                    "VCF Cutoff": 0.42, "VCF Resonance": 0.20, "VCF Envelope": 0.25,
                    "VCF Decay": 0.22, "VCF Release": 0.06,
                    "VCA Decay": 0.23, "VCA Release": 0.06}),
        "error": dict(note=45, description="refused -- saw and pulse at 32' with noise, resonant and shut",
            params={"DCO 1 Octave": 0.0, "DCO 1 Pulse": 1.0, "DCO 1 Triangle": 0.0,
                    "DCO 1 Sawtooth": 1.0, "DCO 1 Pulse Width": 0.12, "DCO 1 Noise": 0.30,
                    "DCO 2 Octave": 0.0, "DCO 2 Pulse": 1.0, "DCO 2 Triangle": 0.0,
                    "DCO 2 Sawtooth": 0.0, "DCO 2 Pulse Width": 0.18, "DCO 2 Detune": 0.35,
                    "VCF Invert Envelope": 0.0,
                    "VCF Cutoff": 0.16, "VCF Resonance": 0.85, "VCF Envelope": 0.14,
                    "VCF Decay": 0.36, "VCF Release": 0.09,
                    "VCA Decay": 0.40, "VCA Release": 0.10}),
        "menu-open": dict(note=76, description="opening -- triangle and saw swelling, filter sweeping up",
            params={"DCO 1 Octave": 0.67, "DCO 1 Pulse": 0.0, "DCO 1 Triangle": 1.0,
                    "DCO 1 Sawtooth": 1.0, "DCO 1 Pulse Width": 0.5, "DCO 1 Noise": 0.0,
                    "DCO 2 Octave": 0.67, "DCO 2 Pulse": 0.0, "DCO 2 Triangle": 1.0,
                    "DCO 2 Sawtooth": 0.0, "DCO 2 Pulse Width": 0.5, "DCO 2 Detune": 0.14,
                    "VCF Invert Envelope": 0.0,
                    # Kept warmer and longer than equip on purpose: at cutoff
                    # 0.34 the two measured 240 ms/6641 Hz against 230 ms/7925 Hz,
                    # which is the same sound twice.
                    "VCF Cutoff": 0.20, "VCF Resonance": 0.30, "VCF Envelope": 0.66,
                    "VCF Attack": 0.18, "VCF Decay": 0.52, "VCF Release": 0.16,
                    "VCA Program Volume": 0.50,
                    "VCA Attack": 0.22, "VCA Decay": 0.54, "VCA Release": 0.18}),
        "menu-close": dict(note=69, description="closing -- the filter envelope inverted, so it shuts rather than opens",
            params={"DCO 1 Octave": 0.33, "DCO 1 Pulse": 0.0, "DCO 1 Triangle": 1.0,
                    "DCO 1 Sawtooth": 1.0, "DCO 1 Pulse Width": 0.5, "DCO 1 Noise": 0.0,
                    "DCO 2 Octave": 0.67, "DCO 2 Pulse": 0.0, "DCO 2 Triangle": 1.0,
                    "DCO 2 Sawtooth": 0.0, "DCO 2 Pulse Width": 0.5, "DCO 2 Detune": 0.08,
                    "VCF Invert Envelope": 1.0,
                    "VCF Cutoff": 0.40, "VCF Resonance": 0.26, "VCF Envelope": 0.62,
                    "VCF Decay": 0.30, "VCF Release": 0.08, "VCA Program Volume": 0.86,
                    "VCA Attack": 0.04, "VCA Decay": 0.34, "VCA Release": 0.10}),
        "equip": dict(note=86, description="cha-ching -- two narrow pulses at 4', detuned, resonant",
            params={"DCO 1 Octave": 1.0, "DCO 1 Pulse": 1.0, "DCO 1 Triangle": 0.0,
                    "DCO 1 Sawtooth": 0.0, "DCO 1 Pulse Width": 0.14, "DCO 1 Noise": 0.0,
                    "DCO 2 Octave": 1.0, "DCO 2 Pulse": 1.0, "DCO 2 Triangle": 0.0,
                    "DCO 2 Sawtooth": 0.0, "DCO 2 Pulse Width": 0.22, "DCO 2 Detune": 0.22,
                    "VCF Invert Envelope": 0.0,
                    "VCF Cutoff": 0.78, "VCF Resonance": 0.58, "VCF Envelope": 0.45,
                    "VCF Decay": 0.28, "VCF Release": 0.09,
                    "VCA Decay": 0.33, "VCA Release": 0.10}),
    },
}


# ------------------------------------------------------------------ Fury800
#
# Korg Poly-800. Three envelopes, and which one does what is not guessable:
# DEG1 drives DCO1, DEG2 drives DCO2, DEG3 the filter. Shortening DEG2 or DEG3
# alone left the note running 2270 ms; DEG1 alone cut it to 50 ms. So any patch
# using both oscillators has to shorten both envelopes, not one.
#
# They are six-stage as well -- attack, decay, break point, slope, sustain,
# release -- so break point and slope belong at zero or the envelope stops on
# the way down and never reaches silence. Measured:
#
#   DCO Octave    1 at 0.0 / 2 at 0.33 / 3 at 1.0      Waveform  1 / 2 at 1.0
#   DCO Mode      Whole 0.0 / Double 0.5   Mode  Poly 0.0 / Chord 0.5
#   16'/8'/4'/2'  on-off switches, and stacking them is the Poly-800's own way
#                 of getting bigger -- that is the lever these seven use
#   values run 0..31; VCF Cutoff runs 0..99
#   DEG1 Decay    0.10 -> 20 ms, 0.18 -> 35, 0.25 -> 50, 0.35 -> 70,
#                 0.45 -> 90, 0.60 -> 165. Long tails need the release.
MACHINES["fury80064"] = {
    "plugin": "Fury800",
    "program": 0,
    "common": {
        "Volume": 0.88, "Tune": 0.5, "Mode": 0.0, "Voices": 0.0,
        "DEG1 Attack": 0.0, "DEG1 Break Point": 0.0, "DEG1 Slope": 0.0, "DEG1 Sustain": 0.0,
        "DEG2 Attack": 0.0, "DEG2 Break Point": 0.0, "DEG2 Slope": 0.0, "DEG2 Sustain": 0.0,
        "DEG3 Attack": 0.0, "DEG3 Break Point": 0.0, "DEG3 Slope": 0.0, "DEG3 Sustain": 0.0,
        "VCF KBD Track": 0.0, "VCF EG Polarity": 0.0, "VCF Trigger": 1.0,
        "MG DCO": 0.0, "MG VCF": 0.0, "Velocity": 0.0,
        "Mod.Wheel to DCO": 0.0, "Mod.Wheel to VCF": 0.0,
        "DCO Mode": 0.0, "Seq. Play On Note": 0.0,
    },
    "patches": {
        "cursor": dict(note=84, description="selection tick -- 2' alone, one oscillator",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 1.0, "DCO1 Level": 0.95,
                    "DCO1 16'": 0.0, "DCO1 8'": 0.0, "DCO1 4'": 1.0, "DCO1 2'": 1.0,
                    "DCO2 Level": 0.32, "DCO2 16'": 0.0, "DCO2 8'": 0.0,
                    "DCO2 4'": 0.0, "DCO2 2'": 0.0, "Noise Level": 0.0,
                    "VCF Cutoff": 0.82, "VCF Resonance": 0.15, "VCF EG Intensity": 0.25,
                    "DEG3 Decay": 0.20, "DEG3 Release": 0.05,
                    "DEG1 Decay": 0.12, "DEG1 Release": 0.04, "Chorus": 0.0}),
        "confirm": dict(note=79, description="accept -- 8' and 2' stacked, square",
            params={"DCO1 Octave": 0.33, "DCO1 Waveform": 1.0, "DCO1 Level": 0.90,
                    "DCO1 16'": 0.0, "DCO1 8'": 1.0, "DCO1 4'": 0.0, "DCO1 2'": 1.0,
                    "DCO2 Level": 0.32, "DCO2 16'": 0.0, "DCO2 8'": 0.0,
                    "DCO2 4'": 0.0, "DCO2 2'": 0.0, "Noise Level": 0.0,
                    "VCF Cutoff": 0.70, "VCF Resonance": 0.30, "VCF EG Intensity": 0.50,
                    "DEG3 Decay": 0.28, "DEG3 Release": 0.07,
                    "DEG1 Decay": 0.30, "DEG1 Release": 0.08, "Chorus": 0.0}),
        "cancel": dict(note=67, description="back out -- 16' sawtooth alone, filter down",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.0, "DCO1 Level": 0.94,
                    "DCO1 16'": 1.0, "DCO1 8'": 0.0, "DCO1 4'": 0.0, "DCO1 2'": 0.0,
                    "DCO2 Level": 0.32, "DCO2 16'": 0.0, "DCO2 8'": 0.0,
                    "DCO2 4'": 0.0, "DCO2 2'": 0.0, "Noise Level": 0.0,
                    "VCF Cutoff": 0.44, "VCF Resonance": 0.22, "VCF EG Intensity": 0.20,
                    "DEG3 Decay": 0.22, "DEG3 Release": 0.06,
                    "DEG1 Decay": 0.24, "DEG1 Release": 0.06, "Chorus": 0.0}),
        "error": dict(note=45, description="refused -- both oscillators at 16' and 8' with noise, resonant",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.0, "DCO1 Level": 0.77,
                    "DCO1 16'": 1.0, "DCO1 8'": 1.0, "DCO1 4'": 0.0, "DCO1 2'": 0.0,
                    "DCO2 Octave": 0.0, "DCO2 Waveform": 1.0, "DCO2 Level": 0.74,
                    "DCO2 16'": 1.0, "DCO2 8'": 0.0, "DCO2 4'": 0.0, "DCO2 2'": 0.0,
                    "DCO2 Detune": 0.85, "DCO2 Interval": 0.0, "Noise Level": 0.55,
                    "VCF Cutoff": 0.26, "VCF Resonance": 0.80, "VCF EG Intensity": 0.20,
                    "DEG3 Decay": 0.40, "DEG3 Release": 0.10,
                    "DEG1 Decay": 0.50, "DEG1 Release": 0.14,
                    "DEG2 Decay": 0.50, "DEG2 Release": 0.14, "Chorus": 0.0}),
        "menu-open": dict(note=76, description="opening -- the full 8'+4'+2' stack swelling in, chorus behind",
            params={"DCO1 Octave": 0.33, "DCO1 Waveform": 0.0, "DCO1 Level": 0.95,
                    "DCO1 16'": 0.0, "DCO1 8'": 1.0, "DCO1 4'": 1.0, "DCO1 2'": 1.0,
                    "DCO2 Octave": 0.33, "DCO2 Waveform": 0.0, "DCO2 Level": 0.90,
                    "DCO2 16'": 0.0, "DCO2 8'": 1.0, "DCO2 4'": 1.0, "DCO2 2'": 0.0,
                    "DCO2 Detune": 0.60, "DCO2 Interval": 0.0, "Noise Level": 0.0,
                    "VCF Cutoff": 0.34, "VCF Resonance": 0.38, "VCF EG Intensity": 0.85,
                    "DEG3 Attack": 0.12, "DEG3 Decay": 0.55, "DEG3 Release": 0.14,
                    "DEG1 Attack": 0.16, "DEG1 Decay": 0.70, "DEG1 Release": 0.22,
                    "DEG2 Attack": 0.16, "DEG2 Decay": 0.70, "DEG2 Release": 0.22,
                    "Chorus": 1.0}),
        "menu-close": dict(note=69, description="closing -- 16' and 8' only, shutting rather than opening",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.0, "DCO1 Level": 0.82,
                    "DCO1 16'": 1.0, "DCO1 8'": 1.0, "DCO1 4'": 0.0, "DCO1 2'": 0.0,
                    "DCO2 Octave": 0.33, "DCO2 Waveform": 0.0, "DCO2 Level": 0.67,
                    "DCO2 16'": 0.0, "DCO2 8'": 1.0, "DCO2 4'": 0.0, "DCO2 2'": 0.0,
                    "DCO2 Detune": 0.45, "DCO2 Interval": 0.0, "Noise Level": 0.0,
                    "VCF Cutoff": 0.34, "VCF Resonance": 0.26,
                    "VCF EG Polarity": 1.0,          # inverted: the filter shuts
                    "VCF EG Intensity": 0.55,
                    "DEG3 Decay": 0.40, "DEG3 Release": 0.10,
                    "DEG1 Decay": 0.55, "DEG1 Release": 0.18,
                    "DEG2 Decay": 0.55, "DEG2 Release": 0.18, "Chorus": 1.0}),
        "equip": dict(note=86, description="cha-ching -- 4' and 2' on both oscillators, detuned and resonant",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 1.0, "DCO1 Level": 0.82,
                    "DCO1 16'": 0.0, "DCO1 8'": 0.0, "DCO1 4'": 1.0, "DCO1 2'": 1.0,
                    "DCO2 Octave": 1.0, "DCO2 Waveform": 1.0, "DCO2 Level": 0.72,
                    "DCO2 16'": 0.0, "DCO2 8'": 0.0, "DCO2 4'": 0.0, "DCO2 2'": 1.0,
                    "DCO2 Detune": 0.72, "DCO2 Interval": 0.0, "Noise Level": 0.0,
                    "VCF Cutoff": 0.94, "VCF Resonance": 0.68, "VCF EG Intensity": 0.30,
                    "DEG3 Decay": 0.35, "DEG3 Release": 0.12,
                    "DEG1 Decay": 0.62, "DEG1 Release": 0.20,
                    "DEG2 Decay": 0.62, "DEG2 Release": 0.20, "Chorus": 0.0}),
    },
}


# ---------------------------------------------------------------- Ragnarok 2
#
# A divide-down machine: five waveform groups -- Square, Pulse, Intvl, Multi,
# Saw -- each with 16'/8'/4'/2' as separate on/off switches, so twenty switches
# decide the timbre and the filter is almost an afterthought. That is the lever
# these seven use; picking a different group is a bigger change than any amount
# of cutoff. Measured:
#
#   Flt Mode    Hipass 0.0 / Lowpass 0.67      Flt Pole  6 / 12 / 18 / 24 dB
#   AmpEnv M    ADSR 0.0 / AD 0.67
#   AmpEnv D    0.10 -> 30 ms, 0.20 -> 90, 0.30 -> 225, 0.40 -> 560,
#               0.55 holds until note-off
#
# It is quiet: at Volume 0.7 a single square measured -20 dBFS, so the master
# runs high here. The highpass mode is worth spending on one patch -- it is the
# one timbre no other machine in the collection offers.
MACHINES["ragnarok264"] = {
    "plugin": "Ragnarok2",
    "program": 0,
    "common": {
        "Volume": 0.92, "M. Tune": 0.5, "Voices": 0.0, "Portamnt": 0.0,
        "AmpEnv M": 0.0, "AmpEnv A": 0.0, "AmpEnv S": 0.0,
        "FltEnv M": 0.0, "FltEnv A": 0.0, "FltEnv S": 0.0,
        "Flt Keyb": 0.0, "Flt LFO": 0.0, "FM LFO": 0.0, "FM Env": 0.5,
        "MW > FM": 0.0, "MW > Fc": 0.0, "Osc Sync": 0.0,
        "EQ 100": 0.5, "EQ 200": 0.5, "EQ 400": 0.5, "EQ 1K": 0.5,
        "EQ 2.5K": 0.5, "EQ 5K": 0.5, "EQ 12K": 0.5,
        # every footage off by default; each patch switches on what it wants
        "Square16": 0.0, "Square 8": 0.0, "Square 4": 0.0, "Square 2": 0.0,
        "Pulse 16": 0.0, "Pulse 8": 0.0, "Pulse 4": 0.0, "Pulse 2": 0.0,
        "Intvl 16": 0.0, "Intvl 8": 0.0, "Intvl 4": 0.0, "Intvl 2": 0.0,
        "Multi 16": 0.0, "Multi 8": 0.0, "Multi 4": 0.0, "Multi 2": 0.0,
        "Saw 16": 0.0, "Saw 8": 0.0, "Saw 4": 0.0, "Saw 2": 0.0,
        "NoiseMix": 0.0, "Drive": 0.0,
    },
    "patches": {
        "cursor": dict(note=84, description="selection tick -- a single square at 2', 24 dB lowpass",
            params={"Square 2": 1.0, "Flt Mode": 1.0, "Flt Pole": 1.0,
                    "Flt Frq": 0.80, "Flt Reso": 0.18, "Flt Env": 0.30,
                    "FltEnv D": 0.15, "FltEnv R": 0.04,
                    "AmpEnv D": 0.11, "AmpEnv R": 0.04}),
        "confirm": dict(note=79, description="accept -- pulse at 8' and 2' together",
            params={"Pulse 8": 1.0, "Pulse 2": 1.0, "Flt Mode": 1.0, "Flt Pole": 0.67,
                    "Flt Frq": 0.68, "Flt Reso": 0.30, "Flt Env": 0.55,
                    "FltEnv D": 0.24, "FltEnv R": 0.06,
                    "AmpEnv D": 0.22, "AmpEnv R": 0.06}),
        "cancel": dict(note=67, description="back out -- sawtooth at 16', gentle 12 dB slope",
            params={"Saw 16": 1.0, "Flt Mode": 1.0, "Flt Pole": 0.33,
                    "Flt Frq": 0.30, "Flt Reso": 0.20, "Flt Env": 0.18,
                    "FltEnv D": 0.20, "FltEnv R": 0.05,
                    "AmpEnv D": 0.17, "AmpEnv R": 0.05}),
        "error": dict(note=45, description="refused -- multi and square at 16' with noise, driven hard",
            params={"Multi 16": 1.0, "Square16": 1.0, "NoiseMix": 0.18,
                    "Drive": 0.30, "Flt Mode": 1.0, "Flt Pole": 1.0,
                    "Flt Frq": 0.12, "Flt Reso": 0.45, "Flt Env": 0.12,
                    "FltEnv D": 0.26, "FltEnv R": 0.08,
                    "AmpEnv D": 0.33, "AmpEnv R": 0.10}),
        "menu-open": dict(note=76, description="opening -- saw at 8' and 4' with the interval rank, swelling",
            params={"Saw 8": 1.0, "Saw 4": 1.0, "Intvl 8": 1.0,
                    "Flt Mode": 1.0, "Flt Pole": 0.67,
                    "Flt Frq": 0.30, "Flt Reso": 0.36, "Flt Env": 0.85,
                    "FltEnv A": 0.12, "FltEnv D": 0.40, "FltEnv R": 0.12,
                    "AmpEnv A": 0.16, "AmpEnv D": 0.40, "AmpEnv R": 0.14}),
        "menu-close": dict(note=69, description="closing -- highpass, which nothing else here offers",
            params={"Square16": 1.0, "Pulse 8": 1.0,
                    "Flt Mode": 0.0, "Flt Pole": 0.33,
                    "Flt Frq": 0.52, "Flt Reso": 0.34, "Flt Env": 0.40,
                    "FltEnv D": 0.20, "FltEnv R": 0.06,
                    "AmpEnv A": 0.02, "AmpEnv D": 0.24, "AmpEnv R": 0.07}),
        "equip": dict(note=86, description="cha-ching -- pulse at 4' and 2' over a square, synced and resonant",
            params={"Pulse 4": 1.0, "Pulse 2": 1.0, "Square 2": 1.0,
                    "Osc Sync": 1.0, "Detune": 0.20,
                    "Flt Mode": 1.0, "Flt Pole": 0.67,
                    "Flt Frq": 0.84, "Flt Reso": 0.55, "Flt Env": 0.40,
                    "FltEnv D": 0.26, "FltEnv R": 0.10,
                    "AmpEnv D": 0.31, "AmpEnv R": 0.12}),
    },
}


# ------------------------------------------------------------------- PECS
#
# Korg PE-2000, a preset ensemble. Three oscillator banks (A, B and C), each
# with its own wave mix, level, transpose and vibrato, feeding a set of *parallel
# filter paths* whose levels are mixed: pre-filter 1 and 2, the VCF, the raw
# signal, an organ+strings filter and a chorus filter. Choosing which path
# carries the sound is the machine's own tone control and it is what separates
# these seven -- there is no single cutoff doing the work.
#
# The envelope is easy to miss: "VCA Attack" sits at parameter 5 but the decay,
# sustain and release are at 55-57, well past where the list first looks
# finished. Reading only the first page suggests a string machine that cannot
# stop, and it can. Measured:
#
#   VCA Decay   0.10 -> 30 ms, 0.20 -> 90, 0.30 -> 285, 0.45 -> 1620,
#               0.60 -> holds
MACHINES["pecs64"] = {
    "plugin": "PECS",
    "program": 0,
    "common": {
        "Master Volume": 0.68, "Master Tune": 0.5,
        "Bank A Tuning": 0.5, "Bank B Tuning": 0.5, "C Tuning": 0.5,
        "VCA Attack": 0.0, "VCA Sustain": 0.0,
        "VCF EG Attack": 0.0, "VCF EG Sustain": 0.0,
        "Vibrato A Depth": 0.0, "Vibrato B Depth": 0.0, "Vibrato C Depth": 0.0,
        "Velocity to VCA EG": 0.0, "Velocity to VCF EG": 0.0,
        "Bank A Pan": 0.5, "Bank B Pan": 0.5, "C Pan": 0.5,
        "Bass": 0.5, "Treble": 0.5,
    },
    "patches": {
        "cursor": dict(note=84, description="selection tick -- bank A raw, no filter path at all",
            params={"Bank A Level": 1.0, "Bank B Level": 0.0, "C Level": 0.0,
                    "Bank A Wave Mix": 0.0, "Bank A Transpose": 0.5,
                    "Raw Signal Level": 0.85, "VCF Level": 0.0,
                    "Pre-Filter 1 Level": 0.0, "Pre-Filter 2 Level": 0.0,
                    "Organ+Strings Filter Level": 0.0, "Chorus Filter Level": 0.0,
                    "VCA Decay": 0.11, "VCA Release": 0.04}),
        "confirm": dict(note=79, description="accept -- banks A and B through the second pre-filter",
            params={"Bank A Level": 0.85, "Bank B Level": 0.70, "C Level": 0.0,
                    "Bank A Wave Mix": 0.30, "Bank B Wave Mix": 0.70,
                    "Bank A Transpose": 0.5, "Bank B Transpose": 0.62,
                    "Raw Signal Level": 0.35, "VCF Level": 0.0,
                    "Pre-Filter 1 Level": 0.0, "Pre-Filter 2 Level": 0.75,
                    "Pre-Filter 2 Shift": 0.80,
                    "Organ+Strings Filter Level": 0.0, "Chorus Filter Level": 0.0,
                    "VCA Decay": 0.21, "VCA Release": 0.06}),
        "cancel": dict(note=67, description="back out -- bank C an octave down, organ+strings path",
            params={"Bank A Level": 0.0, "Bank B Level": 0.0, "C Level": 1.0,
                    "C Wave Mix": 0.0, "C Transpose": 0.20,
                    "Raw Signal Level": 0.22, "VCF Level": 0.0,
                    "Pre-Filter 1 Level": 0.0, "Pre-Filter 2 Level": 0.0,
                    "Organ+Strings Filter Level": 0.95,
                    "Organ+Strings Filter Frequency": 0.16,
                    "Chorus Filter Level": 0.0,
                    "VCA Decay": 0.15, "VCA Release": 0.05}),
        "error": dict(note=45, description="refused -- all three banks detuned through a resonant pre-filter",
            params={"Bank A Level": 0.45, "Bank B Level": 0.42, "C Level": 0.40,
                    "Bank A Wave Mix": 1.0, "Bank B Wave Mix": 0.85, "C Wave Mix": 0.60,
                    "Bank A Tuning": 0.62, "Bank B Tuning": 0.38, "C Tuning": 0.58,
                    "Bank A Transpose": 0.30, "Bank B Transpose": 0.30, "C Transpose": 0.30,
                    "Raw Signal Level": 0.0, "VCF Level": 0.0,
                    "Pre-Filter 1 Level": 0.90, "Pre-Filter 1 Shift": 0.12,
                    "Pre-Filter 1 Q": 0.85,
                    "Pre-Filter 2 Level": 0.0,
                    "Organ+Strings Filter Level": 0.0, "Chorus Filter Level": 0.0,
                    "VCA Decay": 0.28, "VCA Release": 0.08}),
        "menu-open": dict(note=76, description="opening -- banks A and B swelling through the chorus filter",
            params={"Bank A Level": 0.75, "Bank B Level": 0.65, "C Level": 0.0,
                    "Bank A Wave Mix": 0.45, "Bank B Wave Mix": 0.55,
                    "Bank A Transpose": 0.5, "Bank B Transpose": 0.5,
                    "Bank B Tuning": 0.55,
                    "Raw Signal Level": 0.0, "VCF Level": 0.0,
                    "Pre-Filter 1 Level": 0.0, "Pre-Filter 2 Level": 0.0,
                    "Raw Signal Level": 0.40,
                    "Organ+Strings Filter Level": 0.0,
                    "Chorus Filter Level": 0.90, "Chorus Filter Frequency": 0.62,
                    "Chorus Filter Q": 0.30,
                    "VCA Attack": 0.16, "VCA Decay": 0.32, "VCA Release": 0.12}),
        "menu-close": dict(note=69, description="closing -- bank C through the VCF, shutting as it goes",
            params={"Bank A Level": 0.0, "Bank B Level": 0.60, "C Level": 0.80,
                    "Bank B Wave Mix": 0.25, "C Wave Mix": 0.35,
                    "Bank B Transpose": 0.38, "C Transpose": 0.38,
                    "Raw Signal Level": 0.0, "VCF Level": 0.88,
                    "VCF Type": 0.0, "VCF Cutoff": 0.55, "VCF Peak": 0.30,
                    "VCF EG Intensity": 0.30, "VCF EG Decay": 0.22,
                    "VCF EG Release": 0.08,
                    "Pre-Filter 1 Level": 0.0, "Pre-Filter 2 Level": 0.0,
                    "Organ+Strings Filter Level": 0.0, "Chorus Filter Level": 0.0,
                    "VCA Attack": 0.03, "VCA Decay": 0.25, "VCA Release": 0.09}),
        "equip": dict(note=86, description="cha-ching -- bank A up an octave, raw and through a peaked VCF",
            params={"Bank A Level": 1.0, "Bank B Level": 0.55, "C Level": 0.0,
                    "Bank A Wave Mix": 0.15, "Bank B Wave Mix": 0.15,
                    "Bank A Transpose": 0.70, "Bank B Transpose": 0.70,
                    "Bank B Tuning": 0.56,
                    "Raw Signal Level": 0.55, "VCF Level": 0.75,
                    "VCF Type": 0.0, "VCF Cutoff": 0.92, "VCF Peak": 0.55,
                    "VCF EG Intensity": 0.25, "VCF EG Decay": 0.24,
                    "VCF EG Release": 0.10,
                    "Pre-Filter 1 Level": 0.0, "Pre-Filter 2 Level": 0.0,
                    "Organ+Strings Filter Level": 0.0, "Chorus Filter Level": 0.0,
                    "VCA Decay": 0.27, "VCA Release": 0.11}),
    },
}


# --------------------------------------------------------------------- MPS
#
# Twelve parameters and no filter, no oscillators, no envelope in the usual
# sense -- a morphing pad with two variation axes. The generated bank found one
# envelope stage ("Release") and nothing else, so all seven came out the same
# 1345 ms. What actually shapes it:
#
#   Envelope   the length control, and higher is *shorter*:
#              0.80 -> 500 ms, 0.85 -> 310, 0.90 -> 180, 0.96 -> 95, 1.00 -> 60
#   Vivid/Air  dampers -- turning them *down* makes it louder and brighter.
#              At the defaults MPS peaked -12.3 dBFS with Volume at maximum,
#              which is 8 dB under the rest of the collection; at Vivid 0 and
#              Air 0 it reaches -6.
#   Variation X1/Y1   the timbre, and the only timbre control there is. X1 0 /
#              Y1 1 measured 3809 Hz, X1 1 / Y1 0 measured 3214, centre 2695.
#
# So level and brightness come from the same two places here, which is why the
# quiet patches are also the dull ones -- there is no filter to separate them.
MACHINES["mps64"] = {
    "plugin": "MPS",
    "program": 0,
    "common": {
        "Volume": 1.0, "Morph Mode": 0.0, "Morph Rate": 0.30,
        "Variation X2": 0.5, "Variation Y2": 0.5,
    },
    "patches": {
        "cursor": dict(note=84, description="selection tick -- brightest corner of the variation plane",
            params={"Envelope": 1.00, "Release": 0.04,
                    "Vivid": 0.0, "Air": 0.0,
                    "Variation X1": 0.0, "Variation Y1": 1.0}),
        "confirm": dict(note=79, description="accept -- the opposite corner, a little longer",
            params={"Envelope": 0.955, "Release": 0.07,
                    "Vivid": 0.0, "Air": 0.10,
                    "Variation X1": 1.0, "Variation Y1": 0.0}),
        "cancel": dict(note=67, description="back out -- damped, which on this synth also means darker",
            params={"Envelope": 0.915, "Release": 0.09,
                    "Vivid": 0.35, "Air": 0.20,
                    "Variation X1": 0.30, "Variation Y1": 0.15}),
        "error": dict(note=45, description="refused -- both variations wide open, morphing under it",
            params={"Envelope": 0.830, "Release": 0.18,
                    "Vivid": 0.05, "Air": 0.0,
                    "Variation X1": 1.0, "Variation Y1": 1.0,
                    "Morph Mode": 1.0, "Morph Rate": 0.75}),
        "menu-open": dict(note=76, description="opening -- the longest the envelope reaches, morphing slowly",
            params={"Envelope": 0.800, "Release": 0.22,
                    "Vivid": 0.0, "Air": 0.20,
                    "Variation X1": 0.50, "Variation Y1": 0.60,
                    "Morph Mode": 1.0, "Morph Rate": 0.18}),
        "menu-close": dict(note=69, description="closing -- mid-length, damped toward the dark corner",
            params={"Envelope": 0.855, "Release": 0.15,
                    "Vivid": 0.50, "Air": 0.40,
                    "Variation X1": 0.20, "Variation Y1": 0.40}),
        "equip": dict(note=86, description="cha-ching -- bright corner held a little longer, morph off",
            params={"Envelope": 0.875, "Release": 0.13,
                    "Vivid": 0.0, "Air": 0.0,
                    "Variation X1": 0.05, "Variation Y1": 0.95}),
    },
}
