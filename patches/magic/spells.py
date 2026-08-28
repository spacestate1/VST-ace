#!/usr/bin/env python3
"""Hand-written spell banks, one entry per machine.

Seven sounds, and the family is a story rather than a list: the charge gathers,
the cast lets it go, and what arrives is fire, lightning or ice. Heal is the one
that means well. Fizzle is what you get when it does not work.

    charge     the gather -- has to *rise*, and be the longest thing here
    cast       the release, the moment it leaves you
    fire       a roar: noise, low, moving slowly
    lightning  a crack: gone before you have registered it
    ice        glass: bright, ringing, thin
    heal       warm, swelling, no edge on it anywhere
    fizzle     the spell failing -- falls, and dies early

What makes this different from the menu bank is the charge. A menu sound has to
get out of the way; a charge is the one sound in a game you are *waiting* on, so
it is allowed the two seconds the others are not, and it has to spend them
going somewhere. That is the hard part per machine, because a rise over time
needs a modulator that ramps once and stops -- not an LFO. Every machine here
has one, but no two are the same lever:

    FB-7999     Auto Bend, the DW-8000's own pitch ramp
    Ragnarok2   FM depth from the envelope, plus portamento
    MonoFury    the arpeggiator, run fast enough to read as a rise
    Fury800     the Poly-800 sequencer, played from the note
    Bucket ONE  LFO delay into the DCOs, over a slow filter attack
    Fury68      MG delay, same idea with one LFO
    Kern        FM with the envelope as source
    PECS        the modulator section over a slow VCF attack
    MPS         morph, which is a ramp between two points by construction
    Tricent     the Strings section swelling, brass arriving late

Values are the plugin's own 0..1. Where a scaling was measured rather than
guessed, the numbers are in the comment above the machine.

Adding a machine: append an entry, run `python3 make_magic.py <stem>`, read the
table it prints, adjust, repeat. The output carries "handTuned": true so no
generator pass will overwrite it.
"""

SPELLS = {}

# Each machine: dll stem -> dict with
#   plugin   display name, as the plugin reports it
#   program  base program every patch sits on
#   common   parameters shared by all seven
#   patches  name -> {note, description, params}


# ---------------------------------------------------------------- FB-7999
#
# Korg DW-8000. Parameters are front-panel steps written as step/max, and the
# maxima were measured through the plugin's own display rather than assumed:
#
#   0..31  most things          0..63  VCF Cutoff
#   0..15  waveform, delay level        0..7  delay time
#   Auto Bend Select  Off / OSC1 / OSC2 / Both at 0, 1/3, 2/3, 1
#   Auto Bend Mode    Up 0.0 / Down 1.0
#
# Waveform centroids at A4, measured across all sixteen: 16 is the darkest at
# 911 Hz, 7 mellow at 1484, 10 the brightest at 3116.
#
# The charge is what this machine is for. Auto Bend is a one-shot pitch ramp
# with its own time and depth -- exactly the modulator a gathering needs, and
# the thing every other machine here has to fake. Bend Time near the top with
# Both selected is a rise you can hear the whole of.
N31, N63, N15, N7 = 31.0, 63.0, 15.0, 7.0
_OCT16, _OCT8, _OCT4 = 0.0, 0.5, 1.0
_BEND_OFF, _BEND_BOTH = 0.0, 1.0
_UP, _DOWN = 0.0, 1.0


def _w(n):                      # front panel waveform 1..16
    return (n - 1) / N15


def _fb(*, wf, octave, cutoff, res, bend=_BEND_OFF, bend_mode=_UP,
        bend_time=0, bend_int=0, noise=0, vca_a=0, vca_d=4, vca_r=3,
        vcf_a=0, vcf_int=8, vcf_d=6, delay_level=0, delay_time=3,
        delay_fb=0, osc2_level=0, detune2=3, interval=0, mg_delay=0,
        mg_freq=0, mg_osc=0, mg_vcf=0):
    """One FB-7999 patch, in front-panel steps."""
    return {
        "OSC1 Octave": octave, "OSC1 Waveform": _w(wf), "OSC1 Level": 31 / N31,
        "OSC2 Octave": octave, "OSC2 Waveform": _w(wf),
        "OSC2 Level": osc2_level / N31, "OSC2 Interval": interval / 6.0,
        "OSC2 Detune": detune2 / 6.0,
        "Noise Level": noise / N31, "Mode": 0.0,

        "Auto Bend Select": bend, "Auto Bend Mode": bend_mode,
        "Auto Bend Time": bend_time / N31, "Auto Bend Intensity": bend_int / N31,

        "VCF Cutoff": cutoff / N63, "VCF Resonance": res / N31,
        "VCF KBD Track": 2 / 3, "VCF EG Polarity": 0.0,
        "VCF EG Intensity": vcf_int / N31, "VCF EG Attack": vcf_a / N31,
        "VCF EG Decay": vcf_d / N31, "VCF EG Break Point": 0.0,
        "VCF EG Slope": 0.0, "VCF EG Sustain": 0.0, "VCF EG Release": 3 / N31,

        # Break point, slope and sustain at zero is what makes each of these a
        # decay rather than a note that waits for the key.
        "VCA EG Attack": vca_a / N31, "VCA EG Decay": vca_d / N31,
        "VCA EG Break Point": 0.0, "VCA EG Slope": 0.0, "VCA EG Sustain": 0.0,
        "VCA EG Release": vca_r / N31,

        "MG Waveform": 0.0, "MG Frequency": mg_freq / N31,
        "MG Delay": mg_delay / N31, "MG Osc": mg_osc / N31, "MG VCF": mg_vcf / N31,

        "Delay Time": delay_time / N7, "Delay Factor": 0.0,
        "Delay Feedback": delay_fb / N31, "Delay Level": delay_level / N15,
        "Delay Mod. Frequency": 0.0, "Delay Mod. Intensity": 0.0,
        "Portamento": 0.0,
    }


SPELLS["fb799964"] = {
    "plugin": "FB-7999",
    "program": 0,
    "common": {
        "Volume": 0.72, "Tune": 0.5, "Voices": 0.0,
        "After Touch OSC MG": 0.0, "After Touch VCF": 0.0, "After Touch VCA": 0.0,
        "Mod.Wheel Osc MG": 0.0, "Mod.Wheel VCF MG": 0.0,
        "Bend OSC": 0.0, "Bend VCF": 0.0, "VCF EG Velocity": 0.0,
        "VCA EG Velocity": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- Auto Bend running the whole way up, swelling in behind it",
            params=_fb(wf=4, octave=_OCT8, cutoff=30, res=16,
                       bend=_BEND_BOTH, bend_mode=_UP, bend_time=26, bend_int=24,
                       vca_a=12, vca_d=20, vca_r=8,
                       vcf_a=13, vcf_int=24, vcf_d=18,
                       osc2_level=22, detune2=4, delay_level=6, delay_fb=10)),
        "cast": dict(note=72, description="release -- the gathered pitch thrown off downward, bright",
            params=_fb(wf=10, octave=_OCT8, cutoff=46, res=12,
                       bend=_BEND_BOTH, bend_mode=_DOWN, bend_time=4, bend_int=20,
                       vca_d=15, vca_r=6, vcf_int=18, vcf_d=11,
                       osc2_level=18, detune2=4, delay_level=6, delay_fb=8)),
        "fire": dict(note=48, description="a roar -- noise over the darkest wave at 16', filter low and slow",
            params=_fb(wf=16, octave=_OCT16, cutoff=20, res=17,
                       noise=27, vca_a=3, vca_d=18, vca_r=8,
                       vcf_int=14, vcf_d=16, osc2_level=24, detune2=5,
                       delay_level=4, delay_fb=6)),
        "lightning": dict(note=84, description="a crack -- noise and the brightest wave, gone in a moment, echo behind",
            params=_fb(wf=10, octave=_OCT4, cutoff=58, res=24,
                       noise=22, vca_d=7, vca_r=2, vcf_int=20, vcf_d=4,
                       delay_level=9, delay_time=1, delay_fb=14)),
        # Left at cutoff 50 this measured 10865 Hz against lightning's 10302 --
        # two sounds the same brightness, which is the one thing the family
        # cannot have. Ice is the ringing one, so it keeps the resonance and
        # gives up the top: a lower cutoff and the interval detune put it a
        # register below the crack and let the delay carry it instead.
        "ice": dict(note=79, description="glass -- bright and thin, resonant, ringing out through the delay",
            params=_fb(wf=10, octave=_OCT4, cutoff=40, res=27,
                       vca_d=17, vca_r=7, vcf_int=16, vcf_d=13,
                       osc2_level=26, detune2=6, interval=4,
                       delay_level=13, delay_time=5, delay_fb=20)),
        "heal": dict(note=67, description="warm -- the mellow wave swelling in with vibrato arriving late",
            params=_fb(wf=7, octave=_OCT8, cutoff=36, res=6,
                       bend=_BEND_BOTH, bend_mode=_UP, bend_time=8, bend_int=8,
                       vca_a=11, vca_d=17, vca_r=9,
                       vcf_a=8, vcf_int=12, vcf_d=14,
                       osc2_level=20, detune2=4,
                       mg_delay=14, mg_freq=13, mg_osc=4,
                       delay_level=7, delay_fb=10)),
        "fizzle": dict(note=52, description="it fails -- pitch falling away under noise, dead early",
            params=_fb(wf=16, octave=_OCT8, cutoff=22, res=21,
                       bend=_BEND_BOTH, bend_mode=_DOWN, bend_time=14, bend_int=26,
                       noise=15, vca_d=16, vca_r=6,
                       vcf_int=8, vcf_d=14, osc2_level=16, detune2=6,
                       delay_level=3, delay_fb=4)),
    },
}


# ------------------------------------------------------------------ Ragnarok2
#
# A divide-down machine: five waveform groups -- Square, Pulse, Intvl, Multi,
# Saw -- each with 16'/8'/4'/2' as separate on/off switches. Twenty switches
# decide the timbre and the filter is almost an afterthought. Measured on the
# menu bank:
#
#   Flt Mode   Hipass 0.0 / Lowpass 1.0     Flt Pole  6 / 12 / 18 / 24 dB
#   AmpEnv M   ADSR 0.0 / AD 0.67
#   AmpEnv D   0.10 -> 30 ms, 0.20 -> 90, 0.30 -> 225, 0.40 -> 560,
#              0.55 holds until note-off
#   quiet: a single square at Volume 0.7 measured -20 dBFS, so the master is high
#
# The lever no other machine here has is "FM Env" -- FM depth driven by the
# envelope rather than an LFO, bipolar around 0.5. That is a one-shot ramp into
# the timbre, which is what the charge needs. "Drive" is the other one, and it
# is what makes fire sound like burning rather than like a filter sweep.
SPELLS["ragnarok264"] = {
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
        # every footage off by default; each spell switches on what it wants
        "Square16": 0.0, "Square 8": 0.0, "Square 4": 0.0, "Square 2": 0.0,
        "Pulse 16": 0.0, "Pulse 8": 0.0, "Pulse 4": 0.0, "Pulse 2": 0.0,
        "Intvl 16": 0.0, "Intvl 8": 0.0, "Intvl 4": 0.0, "Intvl 2": 0.0,
        "Multi 16": 0.0, "Multi 8": 0.0, "Multi 4": 0.0, "Multi 2": 0.0,
        "Saw 16": 0.0, "Saw 8": 0.0, "Saw 4": 0.0, "Saw 2": 0.0,
        "NoiseMix": 0.0, "Drive": 0.0, "Cho Mix": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- FM depth climbing off the envelope, saw ranks opening behind it",
            params={"Saw 8": 1.0, "Saw 4": 1.0, "Intvl 8": 1.0,
                    "FM Env": 1.0, "Detune": 0.18,
                    "Flt Mode": 1.0, "Flt Pole": 0.67,
                    "Flt Frq": 0.30, "Flt Reso": 0.42, "Flt Env": 0.92,
                    "FltEnv A": 0.30, "FltEnv D": 0.48, "FltEnv R": 0.14,
                    "AmpEnv A": 0.30, "AmpEnv D": 0.50, "AmpEnv R": 0.14,
                    "Cho Mix": 0.35, "Cho Type": 0.0, "Cho Dpt": 0.30}),
        "cast": dict(note=72, description="release -- pulse and saw at 4', driven, filter thrown open",
            params={"Pulse 4": 1.0, "Saw 4": 1.0, "Drive": 0.22,
                    "Flt Mode": 1.0, "Flt Pole": 0.67,
                    "Flt Frq": 0.66, "Flt Reso": 0.34, "Flt Env": 0.60,
                    "FltEnv D": 0.28, "FltEnv R": 0.08,
                    "AmpEnv D": 0.34, "AmpEnv R": 0.09}),
        "fire": dict(note=48, description="a roar -- noise and the multi rank at 16', driven hard and low",
            params={"Multi 16": 1.0, "Square16": 1.0, "NoiseMix": 0.52,
                    "Drive": 0.62, "Flt Mode": 1.0, "Flt Pole": 1.0,
                    "Flt Frq": 0.16, "Flt Reso": 0.38, "Flt Env": 0.20,
                    "FltEnv D": 0.34, "FltEnv R": 0.12,
                    "AmpEnv A": 0.06, "AmpEnv D": 0.42, "AmpEnv R": 0.12}),
        "lightning": dict(note=84, description="a crack -- noise through the highpass, the one timbre nothing else here offers",
            params={"NoiseMix": 0.78, "Square 2": 1.0, "Drive": 0.40,
                    "Flt Mode": 0.0, "Flt Pole": 0.33,
                    "Flt Frq": 0.58, "Flt Reso": 0.52, "Flt Env": 0.35,
                    "FltEnv D": 0.14, "FltEnv R": 0.04,
                    "AmpEnv D": 0.20, "AmpEnv R": 0.04}),
        "ice": dict(note=79, description="glass -- the interval rank up high, synced and resonant",
            params={"Intvl 4": 1.0, "Intvl 2": 1.0, "Pulse 2": 1.0,
                    "Osc Sync": 1.0, "Detune": 0.22,
                    "Flt Mode": 1.0, "Flt Pole": 0.67,
                    "Flt Frq": 0.92, "Flt Reso": 0.64, "Flt Env": 0.30,
                    "FltEnv D": 0.32, "FltEnv R": 0.12,
                    "AmpEnv D": 0.42, "AmpEnv R": 0.14,
                    "Cho Mix": 0.45, "Cho Type": 1.0, "Cho Dpt": 0.25}),
        "heal": dict(note=67, description="warm -- square and saw at 8' swelling through the chorus, nothing sharp",
            params={"Saw 8": 1.0, "Square 8": 1.0,
                    "Flt Mode": 1.0, "Flt Pole": 0.33,
                    "Flt Frq": 0.40, "Flt Reso": 0.16, "Flt Env": 0.50,
                    "FltEnv A": 0.22, "FltEnv D": 0.42, "FltEnv R": 0.16,
                    "AmpEnv A": 0.24, "AmpEnv D": 0.44, "AmpEnv R": 0.18,
                    "Cho Mix": 0.70, "Cho Type": 0.0, "Cho Dpt": 0.45}),
        "fizzle": dict(note=52, description="it fails -- multi rank detuned against noise, driven and shut down",
            params={"Multi 8": 1.0, "Intvl 16": 1.0, "NoiseMix": 0.30,
                    "Drive": 0.38, "Detune": 0.62,
                    "Flt Mode": 1.0, "Flt Pole": 1.0,
                    "Flt Frq": 0.20, "Flt Reso": 0.48, "Flt Env": 0.06,
                    "FltEnv D": 0.18, "FltEnv R": 0.05,
                    "AmpEnv D": 0.38, "AmpEnv R": 0.10}),
    },
}


# ------------------------------------------------------------------- MonoFury
#
# Korg Mono/Poly: four oscillators, each with its own wave, octave, level and
# tune, and a cross-modulation section that runs them into each other. Measured
# on the menu bank:
#
#   Wave n     Triangle 0.0 / Sawtooth 0.33 / PWM 0.67 / PW 1.0
#   Octave n   16' 0.0 / 8' 0.33 / 4' 0.67 / 2' 1.0
#   Effects    Off below 0.5, On above          FX Type  FM / X-Mod
#   EG2 is the amplifier, EG1 the filter
#   EG2 D      0.10 -> 35 ms, 0.20 -> 90, 0.30 -> 285, 0.40 -> 925, 0.55 holds
#
# Four oscillators at full level clip this plugin -- at Volume 0.5 with all four
# up it rendered flat at 0 dBFS -- so the master sits low and levels are spent
# deliberately. The charge here is the filter envelope climbing under all four
# oscillators at four different footings, which is the Mono/Poly's own way of
# getting bigger; X-Mod on top is what stops it sounding like a filter sweep.
SPELLS["monofury64"] = {
    "plugin": "MonoFury",
    "program": 0,
    "common": {
        "Volume": 0.40, "MstrTune": 0.5, "Trnspose": 0.5, "Portamnt": 0.0,
        "KeyAssgn": 0.0, "Trigger": 0.0, "AutoDamp": 0.0, "Hold": 0.0,
        "EG2 A": 0.0, "EG2 S": 0.0, "EG1 A": 0.0, "EG1 S": 0.0,
        "KeyTrack": 0.0,
        "MWIntens": 0.0, "PBIntens": 0.4, "ARPState": 1.0,
        "VCF Velo": 0.0, "VCA Velo": 0.0, "Noise": 0.0,
        "Tune 2": 0.5, "Tune 3": 0.5, "Tune 4": 0.5,
        "Level 1": 0.0, "Level 2": 0.0, "Level 3": 0.0, "Level 4": 0.0,
        "Effects": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- all four oscillators, one per footing, filter climbing under them",
            params={"Wave 1": 0.33, "Octave 1": 0.0, "Level 1": 0.52,
                    "Wave 2": 0.33, "Octave 2": 0.33, "Level 2": 0.46,
                    "Wave 3": 0.67, "Octave 3": 0.67, "Level 3": 0.40,
                    "Wave 4": 1.0, "Octave 4": 1.0, "Level 4": 0.34,
                    "Tune 2": 0.54, "Tune 3": 0.47, "Tune 4": 0.56,
                    "PW": 0.40, "PWM": 0.30, "PWM Src": 1.0,
                    "Effects": 1.0, "FX Type": 1.0, "FX X-Mod": 0.32, "FX FM": 0.0,
                    "CutOff": 0.20, "Reso": 0.46, "EGIntens": 0.95,
                    "EG1 A": 0.32, "EG1 D": 0.50, "EG1 R": 0.14,
                    "EG2 A": 0.30, "EG2 D": 0.48, "EG2 R": 0.14}),
        "cast": dict(note=72, description="release -- two pulses at 4' and 2' through FM, filter open",
            params={"Wave 1": 1.0, "Octave 1": 0.67, "Level 1": 0.60,
                    "Wave 2": 1.0, "Octave 2": 1.0, "Level 2": 0.42,
                    "Tune 2": 0.58, "PW": 0.30, "PWM": 0.0,
                    "Effects": 1.0, "FX Type": 0.0, "FX FM": 0.40, "FX X-Mod": 0.0,
                    "CutOff": 0.62, "Reso": 0.34, "EGIntens": 0.50,
                    "EG1 D": 0.26, "EG1 R": 0.07,
                    "EG2 D": 0.32, "EG2 R": 0.08}),
        "fire": dict(note=48, description="a roar -- noise over two sawtooths at 16', cross-modulated and low",
            params={"Wave 1": 0.33, "Octave 1": 0.0, "Level 1": 0.44,
                    "Wave 2": 0.33, "Octave 2": 0.0, "Level 2": 0.38,
                    "Tune 2": 0.62, "Noise": 0.62, "PW": 0.5, "PWM": 0.0,
                    "Effects": 1.0, "FX Type": 1.0, "FX X-Mod": 0.48, "FX FM": 0.0,
                    "CutOff": 0.22, "Reso": 0.40, "EGIntens": 0.30,
                    "EG1 D": 0.34, "EG1 R": 0.10,
                    "EG2 A": 0.05, "EG2 D": 0.40, "EG2 R": 0.12}),
        "lightning": dict(note=84, description="a crack -- noise and one pulse at 2', resonant, gone at once",
            params={"Wave 1": 1.0, "Octave 1": 1.0, "Level 1": 0.50,
                    "Noise": 0.70, "PW": 0.22, "PWM": 0.0, "Effects": 0.0,
                    "CutOff": 0.88, "Reso": 0.58, "EGIntens": 0.30,
                    "EG1 D": 0.14, "EG1 R": 0.04,
                    "EG2 D": 0.21, "EG2 R": 0.04}),
        "ice": dict(note=79, description="glass -- three triangles a fifth apart at 2', thin and ringing",
            params={"Wave 1": 0.0, "Octave 1": 1.0, "Level 1": 0.50,
                    "Wave 2": 0.0, "Octave 2": 1.0, "Level 2": 0.38,
                    "Wave 3": 1.0, "Octave 3": 0.67, "Level 3": 0.26,
                    "Tune 2": 0.66, "Tune 3": 0.44,
                    "PW": 0.18, "PWM": 0.0, "Effects": 0.0,
                    "CutOff": 0.94, "Reso": 0.68, "EGIntens": 0.30,
                    "EG1 D": 0.34, "EG1 R": 0.13,
                    "EG2 D": 0.44, "EG2 R": 0.15}),
        "heal": dict(note=67, description="warm -- two triangles at 8' swelling in, lightly detuned, no edge",
            params={"Wave 1": 0.0, "Octave 1": 0.33, "Level 1": 0.55,
                    "Wave 2": 0.0, "Octave 2": 0.67, "Level 2": 0.40,
                    "Tune 2": 0.55, "PW": 0.5, "PWM": 0.0, "Effects": 0.0,
                    "CutOff": 0.48, "Reso": 0.18, "EGIntens": 0.55,
                    "EG1 A": 0.20, "EG1 D": 0.42, "EG1 R": 0.16,
                    "EG2 A": 0.22, "EG2 D": 0.44, "EG2 R": 0.18}),
        "fizzle": dict(note=52, description="it fails -- everything detuned against everything, cross-modulated and shut",
            params={"Wave 1": 0.33, "Octave 1": 0.0, "Level 1": 0.40,
                    "Wave 2": 0.67, "Octave 2": 0.33, "Level 2": 0.34,
                    "Wave 3": 0.33, "Octave 3": 0.0, "Level 3": 0.28,
                    "Tune 2": 0.68, "Tune 3": 0.36, "Noise": 0.24,
                    "PW": 0.5, "PWM": 0.45, "PWM Src": 1.0,
                    "Effects": 1.0, "FX Type": 1.0, "FX X-Mod": 0.55, "FX FM": 0.0,
                    "CutOff": 0.30, "Reso": 0.52, "EGIntens": 0.12,
                    "EG1 D": 0.22, "EG1 R": 0.07,
                    "EG2 D": 0.35, "EG2 R": 0.09}),
    },
}


# ----------------------------------------------------------------------- Kern
#
# 32 parameters, all named plainly -- the easiest here to write for, and the
# hardest to make a fireball on, because it has no noise source at all. What it
# has instead is the second oscillator: hard sync plus FM with the *envelope*
# as source, which is a one-shot ramp into the timbre and does the charge's
# rising for it. Fire and lightning are built from sync and FM depth rather than
# noise, which is the honest way round on this machine. Measured on the menu
# bank:
#
#   Osc Wave  Saw below 0.5, Square above     Flt Mode  Smooth / Dirty at 0.5
#   AmpEnv S  a switch, not a level -- off below 0.5
#   Flt Freq  exponential: 0.25 -> 403 Hz, 0.5 -> 2029, 0.75 -> 7006
#   AmpEnv D  0.05 -> 20 ms, 0.15 -> 45, 0.20 -> 70, 0.30 -> 185, 0.40 -> 500,
#             0.55 and up holds until note-off
#
# That last figure is the constraint the menu bank never hit: the usable decay
# runs out at 0.5, so the charge buys its length with the attack instead.
SPELLS["kern64"] = {
    "plugin": "Kern",
    "program": 0,
    "common": {
        "Volume": 0.62, "Monophon": 0.0, "Portamnt": 0.0, "M. Tune": 0.5,
        "Amp Velo": 0.0, "Flt Velo": 0.5, "Flt LFO": 0.5, "Flt Note": 0.5,
        "AmpEnv A": 0.0, "AmpEnv S": 0.0,       # S off: decay to silence
        "FltEnv A": 0.0, "FltEnv S": 0.0,
        "LFO Freq": 0.51, "LFO Wave": 0.0,
        "Cho LFO1": 0.07, "Cho LFO2": 0.13, "Cho Dpt": 0.20,
        "Chorus": 0.0, "Sync": 0.0, "FM": 0.0, "FM Src": 1.0,
        "Osc2Trns": 0.5, "Osc2Tune": 0.5,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- FM depth climbing off the envelope, filter opening with it",
            params={"Osc Wave": 0.0, "Flt Mode": 0.0,
                    "FM": 0.72, "FM Src": 1.0, "Osc2Trns": 0.58, "Osc2Tune": 0.53,
                    "Sync": 0.0, "Flt Note": 0.0,
                    "Flt Freq": 0.22, "Flt Reso": 0.44, "Flt Env": 0.90,
                    "FltEnv A": 0.34, "FltEnv D": 0.50,
                    "AmpEnv A": 0.36, "AmpEnv D": 0.50, "AmpEnv R": 0.16,
                    "Chorus": 1.0}),
        "cast": dict(note=72, description="release -- square through the dirty filter, a stab of FM on the front",
            params={"Osc Wave": 1.0, "Flt Mode": 1.0,
                    "FM": 0.38, "FM Src": 1.0, "Osc2Trns": 0.62, "Osc2Tune": 0.55,
                    "Sync": 0.0, "Flt Note": 0.0,
                    "Flt Freq": 0.62, "Flt Reso": 0.34, "Flt Env": 0.55,
                    "FltEnv D": 0.28,
                    "AmpEnv D": 0.36, "AmpEnv R": 0.10, "Chorus": 0.0}),
        "fire": dict(note=48, description="a roar -- no noise on this machine, so sync and deep FM stand in for it",
            params={"Osc Wave": 0.0, "Flt Mode": 1.0,
                    "FM": 0.88, "FM Src": 1.0, "Osc2Trns": 0.30, "Osc2Tune": 0.41,
                    "Sync": 1.0, "Flt Note": 0.0,
                    "Flt Freq": 0.30, "Flt Reso": 0.40, "Flt Env": 0.55,
                    "FltEnv D": 0.34,
                    "AmpEnv A": 0.06, "AmpEnv D": 0.44, "AmpEnv R": 0.12,
                    "Chorus": 0.0}),
        "lightning": dict(note=84, description="a crack -- sync wide open at the top, dirty, over before it registers",
            params={"Osc Wave": 1.0, "Flt Mode": 1.0,
                    "FM": 0.62, "FM Src": 1.0, "Osc2Trns": 0.86, "Osc2Tune": 0.62,
                    "Sync": 1.0, "Flt Note": 0.0,
                    "Flt Freq": 0.88, "Flt Reso": 0.52, "Flt Env": 0.30,
                    "FltEnv D": 0.14,
                    "AmpEnv D": 0.25, "AmpEnv R": 0.05, "Chorus": 0.0}),
        "ice": dict(note=79, description="glass -- square high and resonant, the second oscillator a fifth above",
            params={"Osc Wave": 1.0, "Flt Mode": 0.0,
                    "FM": 0.14, "FM Src": 1.0, "Osc2Trns": 0.79, "Osc2Tune": 0.56,
                    "Sync": 0.0, "Flt Note": 0.0,
                    "Flt Freq": 0.84, "Flt Reso": 0.58, "Flt Env": 0.18,
                    "FltEnv D": 0.26,
                    "AmpEnv D": 0.44, "AmpEnv R": 0.16, "Chorus": 1.0}),
        "heal": dict(note=67, description="warm -- sawtooth swelling in through the chorus, no FM anywhere",
            params={"Osc Wave": 0.0, "Flt Mode": 0.0,
                    "FM": 0.0, "FM Src": 1.0, "Osc2Trns": 0.5, "Osc2Tune": 0.54,
                    "Sync": 0.0, "Flt Note": 0.0,
                    "Flt Freq": 0.40, "Flt Reso": 0.18, "Flt Env": 0.50,
                    "FltEnv A": 0.24, "FltEnv D": 0.44,
                    "AmpEnv A": 0.26, "AmpEnv D": 0.46, "AmpEnv R": 0.18,
                    "Chorus": 1.0}),
        "fizzle": dict(note=52, description="it fails -- the second oscillator detuned flat against the first, filter shut",
            params={"Osc Wave": 0.0, "Flt Mode": 1.0,
                    "FM": 0.30, "FM Src": 1.0, "Osc2Trns": 0.44, "Osc2Tune": 0.34,
                    "Sync": 0.0, "Flt Note": 0.0,
                    "Flt Freq": 0.18, "Flt Reso": 0.50, "Flt Env": 0.08,
                    "FltEnv D": 0.22,
                    "AmpEnv D": 0.44, "AmpEnv R": 0.10, "Chorus": 0.0}),
    },
}


# --------------------------------------------------------------------- Fury68
#
# Korg Poly-61: two DCOs into one filter, and an unusual split -- EG1 is the
# filter envelope, EG2 the amplifier, selected by "VCA EG Mode" (EG2 at 0.0).
# Setting the wrong one is how a patch ends up ignoring every envelope value it
# was given. Measured on the menu bank:
#
#   DCO1 Octave    16' 0.0 / 8' 0.33 / 4' 1.0     VCF Type  12dB 0.0 / 24dB 0.5
#   DCO1 Waveform  Saw 0.0 / PW 0.33 / PWM 1.0
#   DCO2 Waveform  Off 0.0 / Saw 0.33 / Square 1.0
#   Effects Mode   Off 0.0 / Chorus 0.33 / Phaser 0.5 / Ensemble 1.0
#   EG2 Decay      0.10 -> 15 ms, 0.20 -> 40, 0.30 -> 60, 0.45 -> 115, 0.60 -> 320
#
# The envelopes here are the shortest in the collection -- 0.60 buys only 320 ms
# -- so everything long on this machine is bought at the top of the range, and
# the charge needs the attack as well. No noise source either: fire is PWM
# against a detuned square through the phaser, which is as close as a Poly-61
# gets to a roar.
SPELLS["fury6864"] = {
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
        "DCO2 Interval": 0.0, "Effects Mode": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- PWM widening under a filter sweep, vibrato arriving late",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 1.0, "DCO1 PW/PWM": 0.55,
                    "DCO1 PWM Frequency": 0.28,
                    "DCO2 Waveform": 0.33, "DCO2 Octave": 1.0, "DCO2 Detune": 0.58,
                    "VCF Type": 0.0,
                    "VCF Cutoff": 0.30, "VCF Resonance": 0.46, "VCF EG Intensity": 1.0,
                    "MG Frequency": 0.62, "MG Delay": 0.55, "MG DCO": 0.22,
                    "EG1 Attack": 0.40, "EG1 Decay": 0.91, "EG1 Release": 0.30,
                    "EG2 Attack": 0.40, "EG2 Decay": 0.91, "EG2 Release": 0.32,
                    "Effects Mode": 1.0}),
        "cast": dict(note=72, description="release -- pulse at 4' with the filter thrown open, phaser behind it",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 0.33, "DCO1 PW/PWM": 0.30,
                    "DCO2 Waveform": 1.0, "DCO2 Octave": 0.33, "DCO2 Detune": 0.56,
                    "VCF Type": 0.0,
                    "VCF Cutoff": 0.72, "VCF Resonance": 0.34, "VCF EG Intensity": 0.55,
                    "EG1 Decay": 0.55, "EG1 Release": 0.16,
                    "EG2 Decay": 0.62, "EG2 Release": 0.18,
                    "Effects Mode": 0.5}),
        "fire": dict(note=48, description="a roar -- PWM against a detuned square at 16', 24 dB filter kept low",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 1.0, "DCO1 PW/PWM": 0.18,
                    "DCO1 PWM Frequency": 0.45,
                    "DCO2 Waveform": 1.0, "DCO2 Octave": 0.0, "DCO2 Detune": 0.78,
                    "VCF Type": 0.5,
                    "VCF Cutoff": 0.16, "VCF Resonance": 0.58, "VCF EG Intensity": 0.26,
                    "EG1 Decay": 0.92, "EG1 Release": 0.30,
                    "EG2 Attack": 0.18, "EG2 Decay": 1.0, "EG2 Release": 0.34,
                    "Effects Mode": 0.5}),
        "lightning": dict(note=84, description="a crack -- one pulse at 4', filter wide and resonant, gone at once",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 0.33, "DCO1 PW/PWM": 0.12,
                    "DCO2 Waveform": 0.0, "DCO2 Octave": 1.0, "DCO2 Detune": 0.5,
                    "VCF Type": 0.0,
                    "VCF Cutoff": 0.92, "VCF Resonance": 0.62, "VCF EG Intensity": 0.30,
                    "EG1 Decay": 0.22, "EG1 Release": 0.05,
                    "EG2 Decay": 0.52, "EG2 Release": 0.07,
                    "Effects Mode": 0.0}),
        "ice": dict(note=79, description="glass -- square and pulse at 4', resonant, ensemble underneath",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 0.33, "DCO1 PW/PWM": 0.22,
                    "DCO2 Waveform": 1.0, "DCO2 Octave": 1.0, "DCO2 Detune": 0.62,
                    "VCF Type": 0.0,
                    "VCF Cutoff": 0.84, "VCF Resonance": 0.58, "VCF EG Intensity": 0.30,
                    "EG1 Decay": 0.48, "EG1 Release": 0.16,
                    "EG2 Decay": 0.70, "EG2 Release": 0.20,
                    "Effects Mode": 1.0}),
        "heal": dict(note=67, description="warm -- two sawtooths at 8' swelling through the ensemble",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.33, "DCO1 PW/PWM": 0.5,
                    "DCO2 Waveform": 0.33, "DCO2 Octave": 0.0, "DCO2 Detune": 0.55,
                    "VCF Type": 0.5,
                    "VCF Cutoff": 0.50, "VCF Resonance": 0.20, "VCF EG Intensity": 0.30,
                    "EG1 Attack": 0.26, "EG1 Decay": 0.66, "EG1 Release": 0.22,
                    "EG2 Attack": 0.28, "EG2 Decay": 0.72, "EG2 Release": 0.24,
                    "Effects Mode": 1.0}),
        "fizzle": dict(note=52, description="it fails -- sawtooth against a square a semitone out, 24 dB and shut",
            params={"DCO1 Octave": 0.33, "DCO1 Waveform": 0.0, "DCO1 PW/PWM": 0.5,
                    "DCO2 Waveform": 1.0, "DCO2 Octave": 0.0, "DCO2 Detune": 0.86,
                    "VCF Type": 0.5,
                    "VCF Cutoff": 0.26, "VCF Resonance": 0.50, "VCF EG Intensity": 0.12,
                    "EG1 Decay": 0.48, "EG1 Release": 0.12,
                    "EG2 Decay": 0.78, "EG2 Release": 0.15,
                    "Effects Mode": 0.33}),
    },
}


# -------------------------------------------------------------------- Fury800
#
# Korg Poly-800. Three envelopes, and which one does what is not guessable:
# DEG1 drives DCO1, DEG2 drives DCO2, DEG3 the filter. Any patch using both
# oscillators has to shorten both, not one. They are six-stage as well, so break
# point and slope belong at zero or the envelope stops on the way down and never
# reaches silence. Measured on the menu bank:
#
#   DCO Octave   1 at 0.0 / 2 at 0.33 / 3 at 1.0     Waveform  1 / 2 at 1.0
#   16'/8'/4'/2' on-off switches, and stacking them is the Poly-800's own way of
#                getting bigger -- that is the lever these seven use
#   values run 0..31; VCF Cutoff runs 0..99
#   DEG1 Decay   0.10 -> 20 ms, 0.18 -> 35, 0.25 -> 50, 0.35 -> 70, 0.45 -> 90,
#                0.60 -> 165
#
# 0.60 buying only 165 ms is the whole problem here: everything in this family
# is longer than anything the menu bank needed, so all three envelopes run near
# the top of their range and the charge leans on the attack as well.
SPELLS["fury80064"] = {
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
        "DCO Mode": 0.0, "Seq. Play On Note": 0.0, "Noise Level": 0.0, "Chorus": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- the whole 8'+4'+2' stack on both oscillators, filter climbing under it",
            params={"DCO1 Octave": 0.33, "DCO1 Waveform": 0.0, "DCO1 Level": 0.95,
                    "DCO1 16'": 0.0, "DCO1 8'": 1.0, "DCO1 4'": 1.0, "DCO1 2'": 1.0,
                    "DCO2 Octave": 0.33, "DCO2 Waveform": 0.0, "DCO2 Level": 0.88,
                    "DCO2 16'": 0.0, "DCO2 8'": 1.0, "DCO2 4'": 1.0, "DCO2 2'": 1.0,
                    "DCO2 Detune": 0.62, "DCO2 Interval": 0.0,
                    "VCF Cutoff": 0.66, "VCF Resonance": 0.42, "VCF EG Intensity": 0.50,
                    "MG Frequency": 0.55, "MG Delay": 0.60, "MG DCO": 0.14,
                    "DEG3 Attack": 0.14, "DEG3 Decay": 0.80, "DEG3 Release": 0.22,
                    "DEG1 Attack": 0.14, "DEG1 Decay": 0.80, "DEG1 Release": 0.24,
                    "DEG2 Attack": 0.14, "DEG2 Decay": 0.80, "DEG2 Release": 0.24,
                    "Chorus": 1.0}),
        "cast": dict(note=72, description="release -- 4' and 2' thrown open, no chorus to soften it",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 1.0, "DCO1 Level": 0.92,
                    "DCO1 16'": 0.0, "DCO1 8'": 0.0, "DCO1 4'": 1.0, "DCO1 2'": 1.0,
                    "DCO2 Level": 0.30, "DCO2 16'": 0.0, "DCO2 8'": 0.0,
                    "DCO2 4'": 0.0, "DCO2 2'": 0.0,
                    "VCF Cutoff": 0.66, "VCF Resonance": 0.32, "VCF EG Intensity": 0.50,
                    "DEG3 Attack": 0.09, "DEG3 Decay": 0.66, "DEG3 Release": 0.14,
                    "DEG1 Attack": 0.09, "DEG1 Decay": 0.75, "DEG1 Release": 0.16,
                    "Chorus": 0.0}),
        "fire": dict(note=48, description="a roar -- noise over both oscillators at 16', filter kept down",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.0, "DCO1 Level": 0.72,
                    "DCO1 16'": 1.0, "DCO1 8'": 1.0, "DCO1 4'": 0.0, "DCO1 2'": 0.0,
                    "DCO2 Octave": 0.0, "DCO2 Waveform": 1.0, "DCO2 Level": 0.68,
                    "DCO2 16'": 1.0, "DCO2 8'": 0.0, "DCO2 4'": 0.0, "DCO2 2'": 0.0,
                    "DCO2 Detune": 0.80, "DCO2 Interval": 0.0, "Noise Level": 0.72,
                    "VCF Cutoff": 0.30, "VCF Resonance": 0.44, "VCF EG Intensity": 0.25,
                    "DEG3 Decay": 0.68, "DEG3 Release": 0.16,
                    "DEG1 Attack": 0.08, "DEG1 Decay": 0.75, "DEG1 Release": 0.18,
                    "DEG2 Attack": 0.08, "DEG2 Decay": 0.75, "DEG2 Release": 0.18,
                    "Chorus": 0.0}),
        "lightning": dict(note=84, description="a crack -- noise and 2' alone, filter wide, over at once",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 1.0, "DCO1 Level": 0.80,
                    "DCO1 16'": 0.0, "DCO1 8'": 0.0, "DCO1 4'": 0.0, "DCO1 2'": 1.0,
                    "DCO2 Level": 0.0, "DCO2 16'": 0.0, "DCO2 8'": 0.0,
                    "DCO2 4'": 0.0, "DCO2 2'": 0.0, "Noise Level": 0.62,
                    "VCF Cutoff": 0.94, "VCF Resonance": 0.55, "VCF EG Intensity": 0.30,
                    "DEG3 Decay": 0.36, "DEG3 Release": 0.07,
                    "DEG1 Decay": 0.54, "DEG1 Release": 0.09, "Chorus": 0.0}),
        "ice": dict(note=79, description="glass -- 2' on both, detuned a hair, resonant with the chorus behind",
            params={"DCO1 Octave": 1.0, "DCO1 Waveform": 1.0, "DCO1 Level": 0.85,
                    "DCO1 16'": 0.0, "DCO1 8'": 0.0, "DCO1 4'": 1.0, "DCO1 2'": 1.0,
                    "DCO2 Octave": 1.0, "DCO2 Waveform": 1.0, "DCO2 Level": 0.70,
                    "DCO2 16'": 0.0, "DCO2 8'": 0.0, "DCO2 4'": 0.0, "DCO2 2'": 1.0,
                    "DCO2 Detune": 0.70, "DCO2 Interval": 0.0,
                    "VCF Cutoff": 0.88, "VCF Resonance": 0.62, "VCF EG Intensity": 0.25,
                    "DEG3 Decay": 0.60, "DEG3 Release": 0.16,
                    "DEG1 Decay": 0.74, "DEG1 Release": 0.20,
                    "DEG2 Decay": 0.74, "DEG2 Release": 0.20, "Chorus": 1.0}),
        "heal": dict(note=67, description="warm -- 16' and 8' swelling in through the chorus, nothing above",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.0, "DCO1 Level": 0.90,
                    "DCO1 16'": 1.0, "DCO1 8'": 1.0, "DCO1 4'": 0.0, "DCO1 2'": 0.0,
                    "DCO2 Octave": 0.0, "DCO2 Waveform": 0.0, "DCO2 Level": 0.80,
                    "DCO2 16'": 0.0, "DCO2 8'": 1.0, "DCO2 4'": 0.0, "DCO2 2'": 0.0,
                    "DCO2 Detune": 0.58, "DCO2 Interval": 0.0,
                    "VCF Cutoff": 0.44, "VCF Resonance": 0.18, "VCF EG Intensity": 0.40,
                    "DEG3 Attack": 0.12, "DEG3 Decay": 0.74, "DEG3 Release": 0.18,
                    "DEG1 Attack": 0.14, "DEG1 Decay": 0.77, "DEG1 Release": 0.20,
                    "DEG2 Attack": 0.14, "DEG2 Decay": 0.77, "DEG2 Release": 0.20,
                    "Chorus": 1.0}),
        "fizzle": dict(note=52, description="it fails -- the filter envelope inverted so it shuts instead of opening",
            params={"DCO1 Octave": 0.0, "DCO1 Waveform": 0.0, "DCO1 Level": 0.78,
                    "DCO1 16'": 1.0, "DCO1 8'": 1.0, "DCO1 4'": 0.0, "DCO1 2'": 0.0,
                    "DCO2 Octave": 0.33, "DCO2 Waveform": 1.0, "DCO2 Level": 0.62,
                    "DCO2 16'": 0.0, "DCO2 8'": 1.0, "DCO2 4'": 0.0, "DCO2 2'": 0.0,
                    "DCO2 Detune": 0.88, "DCO2 Interval": 0.0, "Noise Level": 0.30,
                    "VCF Cutoff": 0.40, "VCF Resonance": 0.30,
                    "VCF EG Polarity": 1.0,          # inverted: the filter shuts
                    "VCF EG Intensity": 0.60,
                    "DEG3 Decay": 0.50, "DEG3 Release": 0.12,
                    "DEG1 Decay": 0.68, "DEG1 Release": 0.16,
                    "DEG2 Decay": 0.68, "DEG2 Release": 0.16, "Chorus": 0.0}),
    },
}


# ----------------------------------------------------------------- Bucket ONE
#
# Crumar Bit 01. The waveforms are not a selector -- Triangle, Sawtooth and
# Pulse are three independent on/off switches per oscillator, so they stack, and
# mixing them is this machine's own way of changing timbre. Measured on the menu
# bank:
#
#   DCO Octave  32' 0.0 / 16' 0.33 / 8' 0.67 / 4' 1.0
#   values run 0..63, so a step is n/63
#   VCA Decay   0.15 -> 30 ms, 0.25 -> 75, 0.35 -> 200, 0.45 -> 555, 0.60 -> 1640
#
# It also has "VCF Invert Envelope", which is the tidiest way to get a closing
# sweep rather than faking one with a slow attack -- fizzle uses it. The charge
# lever is "LFO 1 Delay": the LFO fades in rather than starting, so vibrato
# arrives partway through the gather instead of being there from the first
# sample, which is the difference between a rise and a wobble.
#
# "Lower Volume" and "Upper Volume" each appear twice in the parameter list, so
# they arrive as index keys and cannot be set by name. Nothing here needs them.
SPELLS["bucketone64"] = {
    "plugin": "Bucket ONE",
    "program": 0,
    "common": {
        "Tune": 0.5, "Keyboard Mode": 0.0, "Voices": 0.0,
        "VCA Attack": 0.0, "VCA Sustain": 0.0,
        "VCF Attack": 0.0, "VCF Sustain": 0.0, "VCF Track": 0.0,
        "VCF Attack Dynamic": 0.0, "VCF Envelope Dynamic": 0.0,
        "VCA Attack Dynamic": 0.0, "VCA Amount Dynamic": 0.0,
        "VCA Program Volume": 0.52,
        "LFO 1 to DCO 1": 0.0, "LFO 1 to DCO 2": 0.0,
        "LFO 1 to VCF": 0.0, "LFO 1 to VCA": 0.0, "LFO 1 Depth": 0.0,
        "LFO 1 Delay": 0.0,
        "LFO 2 to DCO 1": 0.0, "LFO 2 to DCO 2": 0.0,
        "LFO 2 to VCF": 0.0, "LFO 2 to VCA": 0.0, "LFO 2 Depth": 0.0,
        "DCO 1 Pulse Width Dynamic": 0.0, "DCO 2 Pulse Width Dynamic": 0.0,
        "DCO 1 Frequency": 0.0, "DCO 2 Frequency": 0.0,
        "DCO 1 Noise": 0.0, "VCF Invert Envelope": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- everything stacked at 16' and 8', vibrato fading in late over a slow filter",
            params={"DCO 1 Octave": 0.33, "DCO 1 Triangle": 1.0, "DCO 1 Sawtooth": 1.0,
                    "DCO 1 Pulse": 1.0, "DCO 1 Pulse Width": 0.30,
                    "DCO 2 Octave": 0.67, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 1.0,
                    "DCO 2 Pulse": 1.0, "DCO 2 Pulse Width": 0.40, "DCO 2 Detune": 0.22,
                    "LFO 1 Waveform": 0.0, "LFO 1 Rate": 0.62, "LFO 1 Depth": 0.32,
                    "LFO 1 Delay": 0.58, "LFO 1 to DCO 1": 0.55, "LFO 1 to DCO 2": 0.55,
                    "VCF Cutoff": 0.20, "VCF Resonance": 0.45, "VCF Envelope": 0.95,
                    "VCF Attack": 0.38, "VCF Decay": 0.55, "VCF Release": 0.18,
                    "VCA Attack": 0.50, "VCA Decay": 0.67, "VCA Release": 0.26}),
        "cast": dict(note=72, description="release -- pulse and saw at 4', filter open, nothing modulating",
            params={"DCO 1 Octave": 1.0, "DCO 1 Triangle": 0.0, "DCO 1 Sawtooth": 1.0,
                    "DCO 1 Pulse": 1.0, "DCO 1 Pulse Width": 0.25,
                    "DCO 2 Octave": 1.0, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 0.0,
                    "DCO 2 Pulse": 1.0, "DCO 2 Pulse Width": 0.30, "DCO 2 Detune": 0.12,
                    "VCF Cutoff": 0.74, "VCF Resonance": 0.34, "VCF Envelope": 0.55,
                    "VCF Decay": 0.30, "VCF Release": 0.08,
                    "VCA Decay": 0.43, "VCA Release": 0.11}),
        "fire": dict(note=48, description="a roar -- noise and saw at 32', filter low, the whole thing moving slowly",
            params={"DCO 1 Octave": 0.0, "DCO 1 Triangle": 0.0, "DCO 1 Sawtooth": 1.0,
                    "DCO 1 Pulse": 1.0, "DCO 1 Pulse Width": 0.14, "DCO 1 Noise": 0.62,
                    "DCO 2 Octave": 0.0, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 1.0,
                    "DCO 2 Pulse": 0.0, "DCO 2 Pulse Width": 0.5, "DCO 2 Detune": 0.40,
                    "LFO 1 Waveform": 0.0, "LFO 1 Rate": 0.30, "LFO 1 Depth": 0.20,
                    "LFO 1 Delay": 0.0, "LFO 1 to VCF": 0.45,
                    "VCF Cutoff": 0.28, "VCF Resonance": 0.40, "VCF Envelope": 0.28,
                    "VCF Decay": 0.50, "VCF Release": 0.14,
                    "VCA Attack": 0.06, "VCA Decay": 0.54, "VCA Release": 0.15}),
        "lightning": dict(note=84, description="a crack -- noise and a narrow pulse at 4', wide open and gone",
            params={"DCO 1 Octave": 1.0, "DCO 1 Triangle": 0.0, "DCO 1 Sawtooth": 0.0,
                    "DCO 1 Pulse": 1.0, "DCO 1 Pulse Width": 0.10, "DCO 1 Noise": 0.70,
                    "DCO 2 Octave": 1.0, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 0.0,
                    "DCO 2 Pulse": 0.0, "DCO 2 Detune": 0.0,
                    "VCF Cutoff": 0.92, "VCF Resonance": 0.55, "VCF Envelope": 0.30,
                    "VCF Decay": 0.14, "VCF Release": 0.04,
                    "VCA Decay": 0.33, "VCA Release": 0.06}),
        "ice": dict(note=79, description="glass -- two narrow pulses at 4' a hair apart, resonant and ringing",
            params={"DCO 1 Octave": 1.0, "DCO 1 Triangle": 1.0, "DCO 1 Sawtooth": 0.0,
                    "DCO 1 Pulse": 1.0, "DCO 1 Pulse Width": 0.16,
                    "DCO 2 Octave": 1.0, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 0.0,
                    "DCO 2 Pulse": 1.0, "DCO 2 Pulse Width": 0.20, "DCO 2 Detune": 0.30,
                    "VCF Cutoff": 0.84, "VCF Resonance": 0.62, "VCF Envelope": 0.25,
                    "VCF Decay": 0.30, "VCF Release": 0.12,
                    "VCA Decay": 0.42, "VCA Release": 0.14}),
        "heal": dict(note=67, description="warm -- triangles at 16' and 8' swelling in, no pulse anywhere",
            params={"DCO 1 Octave": 0.33, "DCO 1 Triangle": 1.0, "DCO 1 Sawtooth": 0.0,
                    "DCO 1 Pulse": 0.0, "DCO 1 Pulse Width": 0.5,
                    "DCO 2 Octave": 0.67, "DCO 2 Triangle": 1.0, "DCO 2 Sawtooth": 0.0,
                    "DCO 2 Pulse": 0.0, "DCO 2 Detune": 0.16,
                    "LFO 1 Waveform": 0.0, "LFO 1 Rate": 0.40, "LFO 1 Depth": 0.18,
                    "LFO 1 Delay": 0.45, "LFO 1 to DCO 1": 0.30, "LFO 1 to DCO 2": 0.30,
                    "VCF Cutoff": 0.52, "VCF Resonance": 0.14, "VCF Envelope": 0.45,
                    "VCF Attack": 0.26, "VCF Decay": 0.46, "VCF Release": 0.18,
                    "VCA Attack": 0.24, "VCA Decay": 0.48, "VCA Release": 0.20}),
        "fizzle": dict(note=52, description="it fails -- the filter envelope inverted, so it shuts on you instead of opening",
            params={"DCO 1 Octave": 0.33, "DCO 1 Triangle": 0.0, "DCO 1 Sawtooth": 1.0,
                    "DCO 1 Pulse": 1.0, "DCO 1 Pulse Width": 0.12, "DCO 1 Noise": 0.28,
                    "DCO 2 Octave": 0.33, "DCO 2 Triangle": 0.0, "DCO 2 Sawtooth": 0.0,
                    "DCO 2 Pulse": 1.0, "DCO 2 Pulse Width": 0.18, "DCO 2 Detune": 0.52,
                    "VCF Invert Envelope": 0.0,
                    "VCF Cutoff": 0.58, "VCF Resonance": 0.48, "VCF Envelope": 0.22,
                    "VCF Decay": 0.30, "VCF Release": 0.09,
                    "VCA Program Volume": 0.42,
                    "VCA Decay": 0.44, "VCA Release": 0.11}),
    },
}


# ----------------------------------------------------------------------- PECS
#
# A PE-2000-ish bank machine: three wave banks with their own mix, level and
# transpose, feeding a set of parallel filter paths -- two pre-filters, an
# organ+strings filter, a chorus filter, a raw path and a VCF. Which path
# carries the sound is this machine's own tone control, and it is what separates
# these seven; there is no single cutoff doing the work.
#
# The envelope is easy to miss: "VCA Attack" sits at parameter 5 but the decay,
# sustain and release are at 55-57, well past where the list first looks
# finished. Measured on the menu bank:
#
#   VCA Decay  0.10 -> 30 ms, 0.20 -> 90, 0.30 -> 285, 0.45 -> 1620, 0.60 holds
#
# No noise source, so fire and lightning are built from all three banks detuned
# through a resonant pre-filter. The charge lever is the modulator section --
# source, destination, rate and depth -- run under a slow VCF attack.
SPELLS["pecs64"] = {
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
        "Modulator Depth": 0.0, "Phaser Mode": 0.0,
        "Bank A Level": 0.0, "Bank B Level": 0.0, "C Level": 0.0,
        "Raw Signal Level": 0.0, "VCF Level": 0.0,
        "Pre-Filter 1 Level": 0.0, "Pre-Filter 2 Level": 0.0,
        "Organ+Strings Filter Level": 0.0, "Chorus Filter Level": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- banks A and B swelling through the chorus filter, the modulator climbing under them",
            params={"Bank A Level": 0.80, "Bank B Level": 0.70,
                    "Bank A Wave Mix": 0.40, "Bank B Wave Mix": 0.55,
                    "Bank A Transpose": 0.5, "Bank B Transpose": 0.5,
                    "Bank B Tuning": 0.56,
                    "Raw Signal Level": 0.20, "VCF Level": 0.70,
                    "VCF Type": 0.0, "VCF Cutoff": 0.26, "VCF Peak": 0.45,
                    "VCF EG Intensity": 0.85, "VCF EG Attack": 0.30,
                    "VCF EG Decay": 0.48, "VCF EG Release": 0.16,
                    "Chorus Filter Level": 0.60, "Chorus Filter Frequency": 0.55,
                    "Chorus Filter Q": 0.35,
                    "Modulator Source": 0.5, "Modulator Destination": 0.5,
                    "Modulator Rate": 0.55, "Modulator Depth": 0.35,
                    "VCA Attack": 0.28, "VCA Decay": 0.46, "VCA Release": 0.16}),
        "cast": dict(note=72, description="release -- bank A raw and through a peaked VCF, nothing softening it",
            params={"Bank A Level": 1.0, "Bank B Level": 0.50,
                    "Bank A Wave Mix": 0.15, "Bank B Wave Mix": 0.20,
                    "Bank A Transpose": 0.58, "Bank B Transpose": 0.58,
                    "Bank B Tuning": 0.55,
                    "Raw Signal Level": 0.55, "VCF Level": 0.75,
                    "VCF Type": 0.0, "VCF Cutoff": 0.62, "VCF Peak": 0.50,
                    "VCF EG Intensity": 0.35, "VCF EG Decay": 0.22,
                    "VCF EG Release": 0.09,
                    "VCA Decay": 0.315, "VCA Release": 0.10}),
        "fire": dict(note=48, description="a roar -- all three banks an octave down and out of tune, through the resonant pre-filter",
            params={"Bank A Level": 0.50, "Bank B Level": 0.46, "C Level": 0.44,
                    "Bank A Wave Mix": 1.0, "Bank B Wave Mix": 0.85, "C Wave Mix": 0.70,
                    "Bank A Tuning": 0.60, "Bank B Tuning": 0.40, "C Tuning": 0.57,
                    "Bank A Transpose": 0.28, "Bank B Transpose": 0.28, "C Transpose": 0.28,
                    "Pre-Filter 1 Level": 0.90, "Pre-Filter 1 Shift": 0.14,
                    "Pre-Filter 1 Q": 0.80,
                    "VCA Attack": 0.05, "VCA Decay": 0.38, "VCA Release": 0.12}),
        "lightning": dict(note=84, description="a crack -- bank A raw at the top of its range, peaked hard, over at once",
            params={"Bank A Level": 1.0, "Bank B Level": 0.40,
                    "Bank A Wave Mix": 0.10, "Bank B Wave Mix": 0.10,
                    "Bank A Transpose": 0.85, "Bank B Transpose": 0.85,
                    "Bank B Tuning": 0.58,
                    "Raw Signal Level": 0.70, "VCF Level": 0.60,
                    "VCF Type": 0.0, "VCF Cutoff": 0.95, "VCF Peak": 0.70,
                    "VCF EG Intensity": 0.20, "VCF EG Decay": 0.12,
                    "VCF EG Release": 0.04,
                    "VCA Decay": 0.23, "VCA Release": 0.05}),
        "ice": dict(note=79, description="glass -- banks A and B up an octave through the phaser, thin and moving",
            params={"Bank A Level": 0.90, "Bank B Level": 0.62,
                    "Bank A Wave Mix": 0.22, "Bank B Wave Mix": 0.30,
                    "Bank A Transpose": 0.78, "Bank B Transpose": 0.78,
                    "Bank B Tuning": 0.545,
                    "Raw Signal Level": 0.35, "VCF Level": 0.65,
                    "VCF Type": 0.0, "VCF Cutoff": 0.80, "VCF Peak": 0.62,
                    "VCF EG Intensity": 0.25, "VCF EG Decay": 0.26,
                    "VCF EG Release": 0.12,
                    "Phaser Mode": 1.0, "Phaser Rate": 0.35,
                    "VCA Decay": 0.41, "VCA Release": 0.15}),
        "heal": dict(note=67, description="warm -- banks A and B through the organ+strings filter, swelling, nothing raw",
            params={"Bank A Level": 0.85, "Bank B Level": 0.72,
                    "Bank A Wave Mix": 0.30, "Bank B Wave Mix": 0.35,
                    "Bank A Transpose": 0.5, "Bank B Transpose": 0.38,
                    "Bank B Tuning": 0.54,
                    "Organ+Strings Filter Level": 0.95,
                    "Organ+Strings Filter Frequency": 0.34,
                    "Raw Signal Level": 0.28,
                    "VCA Attack": 0.24, "VCA Decay": 0.42, "VCA Release": 0.18}),
        "fizzle": dict(note=52, description="it fails -- banks A and C a tone apart through the organ filter, low and dull",
            params={"Bank A Level": 0.60, "C Level": 0.62,
                    "Bank A Wave Mix": 0.75, "C Wave Mix": 0.55,
                    "Bank A Tuning": 0.63, "C Tuning": 0.37,
                    "Bank A Transpose": 0.34, "C Transpose": 0.34,
                    "Organ+Strings Filter Level": 0.85,
                    "Organ+Strings Filter Frequency": 0.18,
                    "Pre-Filter 2 Level": 0.40, "Pre-Filter 2 Shift": 0.25,
                    "VCA Decay": 0.30, "VCA Release": 0.09}),
    },
}


# ------------------------------------------------------------------------ MPS
#
# Twelve parameters and no filter, no oscillators, no envelope in the usual
# sense -- a morphing pad with two variation axes. Which makes it, of the ten,
# the machine best suited to a charge and worst suited to everything else: the
# morph *is* a ramp between two points, so the gather writes itself, while fire
# and lightning have to be told apart on length alone.
#
#   Envelope   the length control, and higher is *shorter*. The menu bank only
#              ever needed the short end; measured down the long end for this:
#              0.30 -> 2700 ms (holds), 0.65 -> 2670, 0.72 -> 1800, 0.80 -> 555,
#              0.85 -> 310, 0.90 -> 180, 0.96 -> 95, 1.00 -> 60
#   Vivid/Air  dampers -- turning them *down* makes it louder and brighter
#   Variation X1/Y1  the timbre, and the only timbre control there is:
#              X1 0 / Y1 1 measured 3809 Hz, X1 1 / Y1 0 measured 3214,
#              centre 2695 -- about an octave of range, total
#
# So brightness barely moves here and the seven are separated by length and by
# how damped they are. The charge morphs from the dark corner to the bright one
# over its whole two seconds, which is the one thing this machine does better
# than anything else in the collection.
SPELLS["mps64"] = {
    "plugin": "MPS",
    "program": 0,
    "common": {
        "Volume": 1.0, "Morph Mode": 0.0, "Morph Rate": 0.30,
        "Variation X2": 0.5, "Variation Y2": 0.5,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- morphing from the dark corner to the bright one, slowly, the whole way",
            params={"Envelope": 0.72, "Release": 0.26,
                    "Vivid": 0.0, "Air": 0.0,
                    "Variation X1": 0.90, "Variation Y1": 0.10,
                    "Variation X2": 0.05, "Variation Y2": 0.95,
                    "Morph Mode": 1.0, "Morph Rate": 0.12}),
        "cast": dict(note=72, description="release -- the bright corner alone, no morph, gone in half a second",
            params={"Envelope": 0.775, "Release": 0.12,
                    "Vivid": 0.0, "Air": 0.05,
                    "Variation X1": 0.05, "Variation Y1": 0.95}),
        "fire": dict(note=48, description="a roar -- both variations wide open, morphing under itself",
            params={"Envelope": 0.725, "Release": 0.24,
                    "Vivid": 0.75, "Air": 0.60,
                    "Variation X1": 0.55, "Variation Y1": 0.45,
                    "Variation X2": 0.95, "Variation Y2": 0.10,
                    "Morph Mode": 1.0, "Morph Rate": 0.62}),
        "lightning": dict(note=84, description="a crack -- brightest corner, the shortest the envelope goes",
            params={"Envelope": 0.935, "Release": 0.05,
                    "Vivid": 0.0, "Air": 0.0,
                    "Variation X1": 0.0, "Variation Y1": 1.0}),
        "ice": dict(note=79, description="glass -- bright and undamped, ringing a little past the cast",
            params={"Envelope": 0.810, "Release": 0.18,
                    "Vivid": 0.0, "Air": 0.15,
                    "Variation X1": 0.15, "Variation Y1": 0.90}),
        "heal": dict(note=67, description="warm -- damped toward the middle, morphing gently rather than travelling",
            params={"Envelope": 0.755, "Release": 0.24,
                    "Vivid": 0.30, "Air": 0.35,
                    "Variation X1": 0.40, "Variation Y1": 0.55,
                    "Variation X2": 0.60, "Variation Y2": 0.45,
                    "Morph Mode": 1.0, "Morph Rate": 0.20}),
        "fizzle": dict(note=52, description="it fails -- damped hard toward the dark corner, which on this synth is all there is",
            params={"Envelope": 0.838, "Release": 0.14,
                    "Vivid": 0.55, "Air": 0.45,
                    "Variation X1": 0.85, "Variation Y1": 0.20}),
    },
}


# -------------------------------------------------------------------- Tricent
#
# Korg Trident MK III: three sections that play at once, each with its own
# filter, envelope and output switch, and the sections *are* the vocabulary --
#
#   Synthe    a real subtractive voice: two VCOs, resonant VCF, ADSR
#   Brass     fast attack, its own filter EG
#   Strings   a divide-down ensemble with only Attack and Release
#
# Measured by reading the plugin's own display back:
#
#   VCF Cutoff  exponential: 0.0 -> 8 Hz, 0.3 -> 585, 0.5 -> 2030,
#               0.75 -> 7009, 1.0 -> 19912 Hz
#   VCO Octave  16' 0.0 / 8' 0.5 / 4' 1.0     VCO 1 Wave  Saw / Pulse / PWM
#   Range       Lower 0.0 / Both 0.5 / Upper 1.0     Output X  off below 0.5
#   the envelope parameters render their display as an integer, so it reads "0"
#   for everything under 1 -- those are tuned by measuring, not by reading
#
# Two things follow from the Strings section having no sustain stage: it is the
# obvious voice for a gather, because it swells for exactly as long as you hold
# it -- and it is unusable for anything else here, because it never stops on its
# own. So charge is the only spell with Strings switched on. Heal, which wants
# the same swelling quality, gets it from the Synthe VCA attack instead.
#
# A VCA Attack of exactly 0 produces a click louder than the note behind it, so
# everything tonal here keeps a small non-zero attack.
_T_OFF, _T_ON = 0.0, 1.0
_T_BOTH = 0.5
_T_OCT16, _T_OCT8, _T_OCT4 = 0.0, 0.5, 1.0
_T_SAW, _T_PULSE, _T_PWM = 0.0, 0.5, 1.0


def _tri(*, synthe=_T_OFF, brass=_T_OFF, strings=_T_OFF,
         vol_syn=0.46, vol_brass=0.46, vol_str=0.46,
         wave=_T_PULSE, octave=_T_OCT8, oct2=_T_OCT8, detune2=0.53, pw=0.35,
         pwm_speed=0.25, cutoff=0.60, res=0.0, vcf_eg=0.0,
         vcf_a=0.0, vcf_d=0.20, vcf_s=0.0, vcf_r=0.05,
         vca_a=0.02, vca_d=0.12, vca_s=0.0, vca_r=0.04,
         br_cut=0.45, br_res=0.0, br_eg=0.55,
         br_a=0.02, br_d=0.25, br_s=0.0, br_r=0.10, br16=_T_ON, br8=_T_OFF,
         st_a=0.05, st_r=0.15, st_ens=_T_OFF, st_vib=0.0, st_vib_delay=0.0,
         st16=_T_OFF, st8=_T_ON, st4=_T_OFF, st_eq_hi=0.5,
         flanger=_T_OFF, fl_int=0.5, fl_speed=0.25):
    """One Tricent patch, with every key the bank uses."""
    return {
        "Trident Mode": _T_ON, "Key Assign": 1.0, "Total Tune": 0.5,
        "Split Key": 0.4724,
        "Range Synthe": _T_BOTH, "Range Brass": _T_BOTH, "Range Strings": _T_BOTH,

        "VCO 1 Octave": octave, "VCO 1 Wave": wave, "VCO 1 PW/PWM": pw,
        "VCO 1 PWM Speed": pwm_speed,
        "VCO 2 Octave": oct2, "VCO 2 Detune": detune2,
        "VCF Cutoff": cutoff, "VCF Resonance": res, "VCF KBF Track": 0.5,
        "VCF EG Intensity": vcf_eg,
        "VCF Attack": vcf_a, "VCF Decay": vcf_d,
        "VCF Sustain": vcf_s, "VCF Release": vcf_r,
        "VCA Attack": vca_a, "VCA Decay": vca_d,
        "VCA Sustain": vca_s, "VCA Release": vca_r,
        "VCA Attenuator": 0.4, "VCA Auto Damp": _T_OFF,

        "Brass Cutoff": br_cut, "Brass Resonance": br_res,
        "Brass EG Intensity": br_eg,
        "Brass Attack": br_a, "Brass Decay": br_d,
        "Brass Sustain": br_s, "Brass Release": br_r,
        "Brass 16'": br16, "Brass 8'": br8, "Brass Multi Trigger": _T_ON,

        "Strings Attack": st_a, "Strings Release": st_r,
        "Strings EQ High": st_eq_hi, "Strings EQ Low": 0.5,
        "Strings Bowing Level": 0.5,
        "Strings Vibrato Intensity": st_vib, "Strings Vibrato Delay": st_vib_delay,
        "Strings Vibrato Speed": 0.45,
        "Strings 16'": st16, "Strings 8'": st8, "Strings 4'": st4,
        "Strings Ensemble": st_ens,

        "Output Synthe": synthe, "Output Brass": brass, "Output Strings": strings,
        "Volume Synthe": vol_syn, "Volume Brass": vol_brass,
        "Volume Strings": vol_str,
        "Total Volume": 0.55,

        "Flanger Synthe": flanger, "Flanger Brass": _T_OFF,
        "Flanger Strings": flanger,
        "Flanger Intensity": fl_int, "Flanger Speed": fl_speed,
        "Vib. Intensity": 0.0,
    }


SPELLS["tricent64"] = {
    "plugin": "Tricent",
    "program": 0,
    "common": {},
    "patches": {
        "charge": dict(note=60, description="the gather -- Strings swelling under a Synthe filter sweep, vibrato arriving late, flanged",
            params=_tri(synthe=_T_ON, strings=_T_ON,
                        vol_syn=0.38, vol_str=0.50,
                        wave=_T_PWM, octave=_T_OCT8, oct2=_T_OCT8, detune2=0.56,
                        pw=0.5, pwm_speed=0.30,
                        cutoff=0.26, res=0.45, vcf_eg=0.85,
                        vcf_a=0.30, vcf_d=0.55, vcf_s=0.30, vcf_r=0.20,
                        vca_a=0.30, vca_d=0.60, vca_s=0.25, vca_r=0.20,
                        st_a=0.45, st_r=0.30, st_ens=_T_ON,
                        st_vib=0.40, st_vib_delay=0.55,
                        st16=_T_ON, st8=_T_ON, st4=_T_OFF,
                        flanger=_T_ON, fl_int=0.55, fl_speed=0.18)),
        "cast": dict(note=72, description="release -- the Brass section alone, which is the one thing here with a fast attack",
            params=_tri(brass=_T_ON, vol_brass=0.55,
                        br_cut=0.70, br_res=0.30, br_eg=0.75,
                        br_a=0.01, br_d=0.34, br_s=0.0, br_r=0.12,
                        br16=_T_OFF, br8=_T_ON)),
        "fire": dict(note=48, description="a roar -- PWM at 16' against the brass, filter low and resonant, flanged",
            params=_tri(synthe=_T_ON, brass=_T_ON,
                        vol_syn=0.40, vol_brass=0.34,
                        wave=_T_PWM, octave=_T_OCT16, oct2=_T_OCT16, detune2=0.62,
                        pw=0.5, pwm_speed=0.45,
                        cutoff=0.18, res=0.55, vcf_eg=0.30,
                        vcf_a=0.02, vcf_d=0.40, vcf_r=0.14,
                        vca_a=0.04, vca_d=0.44, vca_r=0.16,
                        br_cut=0.30, br_res=0.20, br_eg=0.40,
                        br_a=0.03, br_d=0.40, br_r=0.14, br16=_T_ON,
                        flanger=_T_ON, fl_int=0.65, fl_speed=0.35)),
        "lightning": dict(note=84, description="a crack -- one pulse at 4', filter wide and resonant, gone at once",
            params=_tri(synthe=_T_ON, vol_syn=0.55,
                        wave=_T_PULSE, octave=_T_OCT4, oct2=_T_OCT4, detune2=0.5,
                        pw=0.15,
                        cutoff=0.90, res=0.60, vcf_eg=0.25,
                        vcf_a=0.0, vcf_d=0.18, vcf_r=0.05,
                        vca_a=0.005, vca_d=0.24, vca_r=0.06)),
        "ice": dict(note=79, description="glass -- pulse at 4' detuned against itself, resonant, flanged rather than ensembled",
            params=_tri(synthe=_T_ON, vol_syn=0.34,
                        wave=_T_PULSE, octave=_T_OCT4, oct2=_T_OCT4, detune2=0.585,
                        pw=0.30,
                        cutoff=0.74, res=0.36, vcf_eg=0.25,
                        vcf_a=0.01, vcf_d=0.34, vcf_r=0.13,
                        vca_a=0.01, vca_d=0.38, vca_r=0.15,
                        flanger=_T_ON, fl_int=0.45, fl_speed=0.40)),
        "heal": dict(note=67, description="warm -- Synthe sawtooths swelling in, no strings, because strings would never stop",
            params=_tri(synthe=_T_ON, brass=_T_ON,
                        vol_syn=0.48, vol_brass=0.36,
                        wave=_T_SAW, octave=_T_OCT8, oct2=_T_OCT8, detune2=0.545,
                        cutoff=0.58, res=0.12, vcf_eg=0.40,
                        vcf_a=0.20, vcf_d=0.38, vcf_r=0.16,
                        vca_a=0.22, vca_d=0.42, vca_r=0.17,
                        br_cut=0.38, br_res=0.0, br_eg=0.30,
                        br_a=0.16, br_d=0.30, br_r=0.12, br16=_T_OFF, br8=_T_ON,
                        flanger=_T_ON, fl_int=0.35, fl_speed=0.12)),
        "fizzle": dict(note=52, description="it fails -- the two oscillators a tone apart, filter shut down, brass dragging behind",
            params=_tri(synthe=_T_ON, brass=_T_ON,
                        vol_syn=0.46, vol_brass=0.40,
                        wave=_T_SAW, octave=_T_OCT8, oct2=_T_OCT16, detune2=0.71,
                        cutoff=0.32, res=0.42, vcf_eg=0.10,
                        vcf_a=0.0, vcf_d=0.26, vcf_r=0.09,
                        vca_a=0.01, vca_d=0.32, vca_r=0.10,
                        br_cut=0.26, br_res=0.15, br_eg=0.20,
                        br_a=0.06, br_d=0.30, br_r=0.10, br16=_T_ON)),
    },
}


# ----------------------------------------------------------------- TAL-U-No-62
#
# A Juno-60: one DCO with saw and sub, noise, and a chorus that is half the
# reason anyone wants the machine. The charge lever is "LFO1DELAY" with
# "LFO1TRIGGER", so the LFO arrives rather than being already running.
# "FILTERCONTOURBIAS" is the other one worth knowing: it shifts the envelope's
# effect negative, which is a filter that shuts rather than opens, and it is
# what fizzle is built on. Probed rather than assumed:
#
#   SAW and SUBOSC are switches, not levels -- 0.5 and 1.0 render identically,
#              and SUBOSC needs SUBOSCVOLUME above zero to be heard at all
#   "7"        the pulse oscillator's switch. It has no name, which is why the
#              first pass had an ice patch with no oscillator on and rendered
#              -78 dBFS. SQUAREPWINTENSITY is its width and does nothing alone.
#   AMPDECAY   0.10 -> 15 ms, 0.20 -> 30, 0.30 -> 80, 0.40 -> 290, 0.50 -> 1310,
#              0.62 -> 2000. Steep between 0.40 and 0.50, which is where most of
#              this family has to sit.
SPELLS["TAL-U-No-62"] = {
    "plugin": "TAL-U-No-62",
    "program": 0,
    "common": {
        "VOLUME": 0.80, "MASTERPITCH": 0.5, "TRANSPOSE": 0.5,
        "ENVGATE": 0.0, "ENVVOLUME": 1.0, "ENVVELOCITY": 0.0,
        "KEYFOLLOW": 0.0, "CUTOFFSMOOTH": 0.20,
        "MODDCO": 0.0, "MODVCF": 0.0, "VOICEMODE": 0.0,
        "LFO1CLOCK": 0.0, "LFO1WAVEFORM": 0.0, "LFO1TRIGGER": 1.0,
        "OSCLFOINTENSITY": 0.0, "FILTERLFO": 0.0,
        "SAW": 0.0, "7": 0.0, "SUBOSC": 0.0, "SUBOSCVOLUME": 0.0,
        "NOISEVOLUME": 0.0, "SQUAREPWMODE": 0.0, "SQUAREPWINTENSITY": 0.0,
        "CHORUS1": 0.0, "CHORUS2": 0.0,
        "HPCUTOFF": 0.0, "FILTERCONTOURBIAS": 0.5, "AMPSUSTAIN": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- filter climbing from shut, the LFO delayed so the shimmer arrives late",
            params={"SAW": 1.0, "7": 1.0, "SQUAREPWINTENSITY": 0.45,
                    "SUBOSC": 1.0, "SUBOSCVOLUME": 0.45,
                    "CUTOFF": 0.54, "RESONANCE": 0.45,
                    "FILTERCONTOUR": 0.50,
                    "LFO1RATE": 0.58, "LFO1DELAY": 0.70,
                    "OSCLFOINTENSITY": 0.20, "FILTERLFO": 0.28,
                    "AMPATTACK": 0.32, "AMPDECAY": 0.545, "AMPRELEASE": 0.24,
                    "CHORUS1": 1.0}),
        "cast": dict(note=72, description="release -- saw and sub wide open, chorus off so it keeps an edge",
            params={"SAW": 1.0, "SUBOSC": 1.0, "SUBOSCVOLUME": 0.65,
                    "CUTOFF": 0.74, "RESONANCE": 0.35,
                    "FILTERCONTOUR": 0.45,
                    "AMPATTACK": 0.02, "AMPDECAY": 0.425, "AMPRELEASE": 0.12}),
        "fire": dict(note=48, description="a roar -- noise over the sub, filter low, the LFO stirring it slowly",
            params={"SAW": 1.0, "SUBOSC": 1.0, "SUBOSCVOLUME": 0.75,
                    "NOISEVOLUME": 0.60,
                    "CUTOFF": 0.50, "RESONANCE": 0.40,
                    "FILTERCONTOUR": 0.15,
                    "LFO1RATE": 0.30, "FILTERLFO": 0.30,
                    "AMPATTACK": 0.08, "AMPDECAY": 0.508, "AMPRELEASE": 0.18}),
        "lightning": dict(note=84, description="a crack -- noise alone through a wide filter with the highpass up, gone at once",
            params={"NOISEVOLUME": 0.90, "7": 1.0, "SQUAREPWINTENSITY": 0.20,
                    "CUTOFF": 0.92, "RESONANCE": 0.40, "HPCUTOFF": 0.18,
                    "FILTERCONTOUR": 0.15,
                    "AMPATTACK": 0.0, "AMPDECAY": 0.345, "AMPRELEASE": 0.05}),
        "ice": dict(note=79, description="glass -- a narrow pulse, resonant, both choruses on",
            params={"7": 1.0, "SQUAREPWINTENSITY": 0.88,
                    "CUTOFF": 0.82, "RESONANCE": 0.50,
                    "FILTERCONTOUR": 0.15,
                    "AMPATTACK": 0.01, "AMPDECAY": 0.472, "AMPRELEASE": 0.20,
                    "CHORUS1": 1.0, "CHORUS2": 1.0}),
        "heal": dict(note=67, description="warm -- saw and sub swelling in through the chorus, no resonance anywhere",
            params={"SAW": 1.0, "SUBOSC": 1.0, "SUBOSCVOLUME": 0.55,
                    "CUTOFF": 0.58, "RESONANCE": 0.08,
                    "FILTERCONTOUR": 0.40,
                    "AMPATTACK": 0.26, "AMPDECAY": 0.50, "AMPRELEASE": 0.24,
                    "CHORUS1": 1.0}),
        "fizzle": dict(note=52, description="it fails -- the contour biased negative, so the filter shuts instead of opening",
            params={"SAW": 1.0, "7": 1.0, "SQUAREPWINTENSITY": 0.30,
                    "NOISEVOLUME": 0.25,
                    "CUTOFF": 0.60, "RESONANCE": 0.45,
                    "FILTERCONTOUR": 0.55, "FILTERCONTOURBIAS": 0.0,
                    "AMPATTACK": 0.01, "AMPDECAY": 0.43, "AMPRELEASE": 0.10}),
    },
}


# ------------------------------------------------------------------ Nabla
#
# A Korg Delta: a divide-down Synth section with 16'/8'/4'/2' plus noise as
# separate switches, a String section beside it, one filter, and a phaser,
# delay and ensemble on the end. Two things shape everything here:
#
#   the Synth EG has Attack, Decay and Sustain but no release of its own, and
#   "VCA Mode" decides whether the amplifier follows that EG at all
#   the String section has only an attack -- it holds while the key is down, so
#   like the Trident it is the charge's voice and nothing else's
#
# "SG Noise" is what makes fire and lightning possible on a divide-down machine.
SPELLS["nabla64"] = {
    "plugin": "Nabla",
    "program": 0,
    "common": {
        "Volume": 0.86, "Tune": 0.5, "Octave": 0.5, "GOD Mode": 0.0,
        "Syn Vol": 0.70, "Syn Pan": 0.5, "Str Vol": 0.0, "Str Pan": 0.5,
        "MW Src": 0.0, "MG Vib": 0.0, "VCA Mode": 1.0, "Syn Trig": 0.0,
        "EQ Low": 0.5, "EQ High": 0.5,
        "Phaser": 0.0, "Delay": 0.0, "VCF KbdF": 0.0,
        "SG 16'": 0.0, "SG 8'": 0.0, "SG 4'": 0.0, "SG 2'": 0.0, "SG Noise": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- the String section swelling under a filter sweep, phased",
            params={"Syn Vol": 0.45, "Str Vol": 0.72, "Str Oct": 0.5,
                    "Str EG A": 0.62, "Str Trig": 0.0,
                    "SG 16'": 1.0, "SG 8'": 1.0,
                    "VCF Freq": 0.18, "VCF Reso": 0.50, "VCF EG": 0.90,
                    "VCF Mode": 0.0,
                    "Syn EG A": 0.40, "Syn EG D": 0.62, "Syn EG S": 0.30,
                    "Phaser": 1.0, "Ph Speed": 0.18, "Ph Mix": 0.55,
                    "Ph Feed": 0.40, "Ph Mod": 0.35}),
        "cast": dict(note=72, description="release -- 4' and 2' together, filter open, nothing behind it",
            params={"SG 4'": 1.0, "SG 2'": 1.0,
                    "VCF Freq": 0.70, "VCF Reso": 0.34, "VCF EG": 0.50,
                    "VCF Mode": 0.0,
                    "Syn EG A": 0.0, "Syn EG D": 0.40, "Syn EG S": 0.0}),
        "fire": dict(note=48, description="a roar -- noise over 16', filter down, the phaser turning slowly under it",
            params={"SG 16'": 1.0, "SG 8'": 1.0, "SG Noise": 0.72,
                    "VCF Freq": 0.36, "VCF Reso": 0.40, "VCF EG": 0.22,
                    "VCF Mode": 0.0,
                    "Syn EG A": 0.06, "Syn EG D": 0.42, "Syn EG S": 0.0,
                    "Phaser": 1.0, "Ph Speed": 0.30, "Ph Mix": 0.40,
                    "Ph Feed": 0.55, "Ph Mod": 0.45}),
        "lightning": dict(note=84, description="a crack -- noise and 2' alone, wide open, over before it registers",
            params={"SG 2'": 1.0, "SG Noise": 0.85,
                    "VCF Freq": 0.90, "VCF Reso": 0.48, "VCF EG": 0.25,
                    "VCF Mode": 0.0,
                    "Syn EG A": 0.0, "Syn EG D": 0.27, "Syn EG S": 0.0}),
        "ice": dict(note=79, description="glass -- 4' and 2' resonant, ringing out through the delay",
            params={"SG 4'": 1.0, "SG 2'": 1.0,
                    "VCF Freq": 0.80, "VCF Reso": 0.62, "VCF EG": 0.25,
                    "VCF Mode": 0.0,
                    "Syn EG A": 0.01, "Syn EG D": 0.46, "Syn EG S": 0.0,
                    "Delay": 1.0, "Dly Time": 0.22, "Dly Feed": 0.45,
                    "Dly Mix": 0.40}),
        "heal": dict(note=67, description="warm -- 16' and 8' swelling in, no resonance, ensemble underneath",
            params={"SG 16'": 1.0, "SG 8'": 1.0,
                    "VCF Freq": 0.44, "VCF Reso": 0.12, "VCF EG": 0.45,
                    "VCF Mode": 0.0,
                    "Syn EG A": 0.22, "Syn EG D": 0.45, "Syn EG S": 0.0,
                    "Ens Spd1": 0.25, "Ens Spd2": 0.35,
                    "Ens 1to1": 0.55, "Ens 2to2": 0.55}),
        "fizzle": dict(note=52, description="it fails -- highpass mode with noise under it, thin and going nowhere",
            params={"SG 16'": 1.0, "SG 4'": 1.0, "SG Noise": 0.35,
                    "VCF Freq": 0.42, "VCF Reso": 0.52, "VCF EG": 0.08,
                    "VCF Mode": 1.0,
                    "Syn EG A": 0.0, "Syn EG D": 0.44, "Syn EG S": 0.0}),
    },
}


# --------------------------------------------------------------------- Paralogy
#
# Three sections again -- Synth, Organ, String -- but unlike the Trident the
# Synth section here has a full ADSR *and* the machine has "Modulation: Delay",
# an LFO that arrives late. So the charge can be built without the String
# section holding the key down: the envelope swells, the modulation fades in
# behind it, and it still stops on its own.
#
# "Osc.: Sync" and "Waveform Selection: Alternate" are the timbre levers that
# nothing else in this collection has in the same shape.
SPELLS["paralogy64"] = {
    "plugin": "Paralogy",
    "program": 0,
    "common": {
        "Master Volume": 0.78, "Tweak: Master Tune": 0.5,
        "Pitch Bend Amount": 0.2, "Mod.Wheel Amount": 0.0,
        "Synth: Volume": 0.0, "Organ: Volume": 0.0, "String: Volume": 0.0,
        "Synth: Panorama": 0.5, "Organ: Panorama": 0.5, "String: Panoram": 0.5,
        "Organ: 16'": 0.0, "Organ: 8'": 0.0, "Organ: 4'": 0.0, "Organ: 2'": 0.0,
        "Glide: Enable": 0.0, "Glide: Amount": 0.0,
        "Osc.: Sync": 0.0, "Modulation: Depth": 0.0, "Modulation: Delay": 0.0,
        "Phaser: Synth": 0.0, "Phaser: Organ": 0.0, "Phaser: String": 0.0,
        "Delay: Mix": 0.0, "Envelope: Sustain": 0.0,
        "Osc.1: Fine Tune": 0.5, "Osc.2: Coarse Tune": 0.5,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- the envelope swelling while the modulation fades in behind it, phased",
            params={"Synth: Volume": 0.85,
                    "Osc.1: Octave": 0.5, "Osc.2: Octave": 0.5,
                    "Osc.2: Coarse Tune": 0.53,
                    "Waveform Selection: Enable": 1.0,
                    "Waveform Selection: Waveform": 0.30,
                    "VCF: Cutoff": 0.18, "VCF: Resonance": 0.48,
                    "VCF: Envelope": 0.92,
                    "Envelope: Attack": 0.34, "Envelope: Decay": 0.50,
                    "Envelope: Release": 0.24,
                    "Modulation: Speed": 0.55, "Modulation: Delay": 0.70,
                    "Modulation: Depth": 0.30, "LFO: Routing": 0.5,
                    "Phaser: Synth": 1.0, "Phaser: Speed": 0.18,
                    "Phaser: Feedback": 0.45}),
        "cast": dict(note=72, description="release -- both oscillators synced, filter thrown open",
            params={"Synth: Volume": 0.88,
                    "Osc.1: Octave": 0.75, "Osc.2: Octave": 0.75,
                    "Osc.2: Coarse Tune": 0.62, "Osc.: Sync": 1.0,
                    "Waveform Selection: Enable": 1.0,
                    "Waveform Selection: Waveform": 0.15,
                    "VCF: Cutoff": 0.70, "VCF: Resonance": 0.34,
                    "VCF: Envelope": 0.50,
                    "Envelope: Attack": 0.02, "Envelope: Decay": 0.30,
                    "Envelope: Release": 0.12}),
        "fire": dict(note=48, description="a roar -- oscillators a tone apart low down, filter shut, phaser turning under it",
            params={"Synth: Volume": 0.88,
                    "Osc.1: Octave": 0.25, "Osc.2: Octave": 0.25,
                    "Osc.2: Coarse Tune": 0.40,
                    "Waveform Selection: Enable": 1.0,
                    "Waveform Selection: Waveform": 0.55,
                    "VCF: Cutoff": 0.36, "VCF: Resonance": 0.44,
                    "VCF: Envelope": 0.28,
                    "Envelope: Attack": 0.07, "Envelope: Decay": 0.38,
                    "Envelope: Release": 0.18,
                    "Phaser: Synth": 1.0, "Phaser: Speed": 0.34,
                    "Phaser: Feedback": 0.60}),
        "lightning": dict(note=84, description="a crack -- synced high and resonant, over before it registers",
            params={"Synth: Volume": 0.90,
                    "Osc.1: Octave": 1.0, "Osc.2: Octave": 1.0,
                    "Osc.2: Coarse Tune": 0.78, "Osc.: Sync": 1.0,
                    "Waveform Selection: Enable": 1.0,
                    "Waveform Selection: Waveform": 0.10,
                    "VCF: Cutoff": 0.90, "VCF: Resonance": 0.52,
                    "VCF: Envelope": 0.25,
                    "Envelope: Attack": 0.0, "Envelope: Decay": 0.24,
                    "Envelope: Release": 0.05}),
        "ice": dict(note=79, description="glass -- the organ ranks over a resonant synth, ringing out through the delay",
            params={"Synth: Volume": 0.88,
                    "Osc.1: Octave": 1.0, "Osc.2: Octave": 1.0,
                    "Osc.2: Coarse Tune": 0.545,
                    "Waveform Selection: Enable": 1.0,
                    "Waveform Selection: Waveform": 0.20,
                    "VCF: Cutoff": 0.80, "VCF: Resonance": 0.60,
                    "VCF: Envelope": 0.25,
                    "Envelope: Attack": 0.01, "Envelope: Decay": 0.33,
                    "Envelope: Release": 0.18,
                    "Delay: Mix": 0.40, "Delay: Time": 0.22,
                    "Delay: Feedback": 0.45}),
        "heal": dict(note=67, description="warm -- the String section over a soft synth, swelling, nothing sharp",
            params={"Synth: Volume": 0.85,
                    "Osc.1: Octave": 0.5, "Osc.2: Octave": 0.5,
                    "Osc.2: Coarse Tune": 0.52,
                    "Waveform Selection: Enable": 1.0,
                    "Waveform Selection: Waveform": 0.40,
                    "VCF: Cutoff": 0.46, "VCF: Resonance": 0.12,
                    "VCF: Envelope": 0.42,
                    "Envelope: Attack": 0.26, "Envelope: Decay": 0.37,
                    "Envelope: Release": 0.22}),
        "fizzle": dict(note=52, description="it fails -- the two oscillators badly out with each other, filter closing down",
            params={"Synth: Volume": 0.80,
                    "Osc.1: Octave": 0.5, "Osc.2: Octave": 0.25,
                    "Osc.2: Coarse Tune": 0.71,
                    "Waveform Selection: Enable": 1.0,
                    "Waveform Selection: Waveform": 0.65,
                    "Waveform Selection: Alternate": 1.0,
                    "VCF: Cutoff": 0.34, "VCF: Resonance": 0.50,
                    "VCF: Envelope": 0.10,
                    "Envelope: Attack": 0.0, "Envelope: Decay": 0.285,
                    "Envelope: Release": 0.10}),
    },
}


# ---------------------------------------------------------------------- Oxid
#
# A Siel Orchestra: three sections again -- String (Bass/Cello/Viola/Violin as
# separate switches), Bass, and a Synth section that is the only one with a real
# ADSR. "Synth: ADSR to VCA" is the switch that decides whether that envelope
# reaches the amplifier at all, and without it on, everything here holds.
#
# The String section has one global Attack and Release and sounds while the key
# is down, so as on the Trident it is the charge's voice and nobody else's.
#
# "Mix String/Synth" runs the way its name reads: 1.0 is *all string*, so a
# synth-only patch that sets it to 1.0 renders silence. Four spells did, in the
# first pass. The synth end is 0.0. Measured from there:
#
#   Synth: Decay  0.15 -> 40 ms (and -68 dBFS, which is too quiet to use),
#                 0.30 -> 435, 0.45 -> 2525, 0.60 and up holds
#
# Level tracks the decay closely on this machine, so the short spells are also
# the quiet ones and the shortest usable decay is about 0.25.
# Phaser, chorus and a delay sit on the end and are most of this machine's
# character -- the filter barely is.
SPELLS["oxid64"] = {
    "plugin": "Oxid",
    "program": 0,
    "common": {
        "Master Volume": 0.72, "Tune": 0.5, "Tone": 0.5,
        "String: Bass": 0.0, "String: Cello": 0.0,
        "String: Viola": 0.0, "String: Violin": 0.0,
        "Bass Volume": 0.0, "Bass: 16'": 0.0, "Bass: 8'": 0.0,
        "Bass: Staccato": 0.0,
        "Synth: 8'": 0.0, "Synth: 4'": 0.0, "Hollow Waveform": 0.0,
        "Synth: ADSR to VCA": 1.0, "Synth: Sustain": 0.0,
        "Single Trigger": 0.0, "VCF Wheel": 0.0, "VCF LFO": 0.0,
        "Chorus: Synth": 0.0, "Phaser: String": 0.0, "Phaser: Synth": 0.0,
        "Phaser: Bass": 0.0, "Delay: Mix": 0.0,
        "Pan String": 0.5, "Pan Synth": 0.5, "Pan Bass": 0.5,
        "Mix String/Synth": 0.5,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- the whole string section swelling in behind a synth filter sweep, phased",
            params={"String: Viola": 1.0, "String: Violin": 1.0,
                    "String: Cello": 1.0,
                    "Attack": 0.62, "Release": 0.35,
                    "Mix String/Synth": 0.40,
                    "Synth: 8'": 1.0, "Hollow Waveform": 1.0,
                    "VCF Cutoff": 0.18, "VCF Resonance": 0.48, "VCF ADSR": 0.90,
                    "Synth: Attack": 0.34, "Synth: Decay": 0.62,
                    "Synth: Release": 0.24,
                    "Phaser: String": 1.0, "Phaser: Synth": 1.0,
                    "Phaser: Speed": 0.18, "Phaser: Feedback": 0.45}),
        "cast": dict(note=72, description="release -- the synth section at 4', filter open, nothing behind it",
            params={"Mix String/Synth": 0.0,
                    "Synth: 8'": 1.0, "Synth: 4'": 1.0,
                    "VCF Cutoff": 0.70, "VCF Resonance": 0.35, "VCF ADSR": 0.50,
                    "Synth: Attack": 0.02, "Synth: Decay": 0.305,
                    "Synth: Release": 0.09, "Tone": 0.62}),
        "fire": dict(note=48, description="a roar -- synth and bass together low down, filter shut, phaser turning slowly",
            params={"Mix String/Synth": 0.0,
                    "Synth: 8'": 1.0, "Hollow Waveform": 1.0,
                    "Bass Volume": 0.70, "Bass: 16'": 1.0,
                    "VCF Cutoff": 0.26, "VCF Resonance": 0.45, "VCF ADSR": 0.28,
                    "Synth: Attack": 0.05, "Synth: Decay": 0.375,
                    "Synth: Release": 0.16, "Tone": 0.18,
                    "Phaser: Synth": 1.0, "Phaser: Bass": 1.0,
                    "Phaser: Speed": 0.35, "Phaser: Feedback": 0.60}),
        "lightning": dict(note=84, description="a crack -- synth at 4' wide open and resonant, gone at once",
            params={"Mix String/Synth": 0.0,
                    "Synth: 4'": 1.0,
                    "VCF Cutoff": 0.92, "VCF Resonance": 0.45, "VCF ADSR": 0.55,
                    "Synth: Attack": 0.0, "Synth: Decay": 0.272,
                    "Synth: Release": 0.04, "Tone": 0.88,
                    "Master Volume": 1.0}),
        "ice": dict(note=79, description="glass -- the hollow waveform at 4', resonant, ringing out through the delay",
            params={"Mix String/Synth": 0.0,
                    "Synth: 4'": 1.0, "Hollow Waveform": 1.0,
                    "VCF Cutoff": 0.82, "VCF Resonance": 0.62, "VCF ADSR": 0.25,
                    "Synth: Attack": 0.01, "Synth: Decay": 0.345,
                    "Synth: Release": 0.16, "Tone": 0.72,
                    "Delay: Mix": 0.40, "Delay: Time": 0.22,
                    "Delay: Feedback": 0.45, "Chorus: Synth": 1.0}),
        "heal": dict(note=67, description="warm -- synth at 8' swelling in through the chorus, no resonance",
            params={"Mix String/Synth": 0.0,
                    "Synth: 8'": 1.0,
                    "VCF Cutoff": 0.48, "VCF Resonance": 0.10, "VCF ADSR": 0.42,
                    "Synth: Attack": 0.26, "Synth: Decay": 0.405,
                    "Synth: Release": 0.20, "Tone": 0.40,
                    "Chorus: Synth": 1.0, "Chorus: Slow": 1.0}),
        "fizzle": dict(note=52, description="it fails -- synth and bass a register apart, filter closing, the staccato bass cutting out under it",
            params={"Mix String/Synth": 0.0,
                    "Synth: 8'": 1.0, "Hollow Waveform": 1.0,
                    "Bass Volume": 0.55, "Bass: 8'": 1.0, "Bass: Staccato": 1.0,
                    "VCF Cutoff": 0.32, "VCF Resonance": 0.50, "VCF ADSR": 0.10,
                    "Synth: Attack": 0.0, "Synth: Decay": 0.335,
                    "Synth: Release": 0.10, "Tone": 0.12}),
    },
}


# ------------------------------------------------------------------------ NY
#
# A Korg Lambda: two sections and no filter worth the name. The Percussive
# section (Electric Piano, Clavi, Piano, Harmonics as separate switches, each
# with its own volume) decays by itself and is where six of these seven live.
# The Ensemble section (Brass, Organ, Strings I and II, Chorus) holds while the
# key is down, so it belongs to the charge alone.
#
# "Perc: Sustain" on and the Ensemble section at all both hold to note-off --
# the brass has a filter envelope but not an amplitude one -- so the six spells
# that must stop are all Percussive with Sustain off. Measured:
#
#   Perc: Decay  0.0 -> 20 ms, 0.30 -> 300, 0.42 -> 960
#
# The only filter on the machine is the brass one -- "Brass fc", "Brass Peak",
# "Brass EG Intensity" -- so what would be a filter sweep anywhere else has to
# be the brass section here, and everything else is chosen by which voice is
# switched on.
SPELLS["ny64"] = {
    "plugin": "NY",
    "program": 0,
    "common": {
        "Volume Percussive": 0.0, "Volume Ensemble": 0.0,
        "Pan Percussive": 0.5, "Pan Ensemble": 0.5, "Total Tune": 0.5,
        "Tune A": 0.5, "Tune B": 0.5, "Octave Up": 0.0,
        "Perc: Electric Piano": 0.0, "Perc: Clavi": 0.0,
        "Perc: Piano": 0.0, "Perc: Harmonics": 0.0,
        "Perc: Tremolo": 0.0, "Perc: Sustain": 0.0,
        "Ens: Brass": 0.0, "Ens: Organ": 0.0,
        "Ens: Strings I": 0.0, "Ens: Strings II": 0.0, "Ens: Chorus": 0.0,
        "Ens: Vibrato Off": 1.0, "Ens: A/R Variable": 0.0,
        "Tone Percussive": 0.5, "Tone Ensemble": 0.5,
        "Velocity": 0.0, "Mod. Wheel": 0.0,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- both string ranks swelling in with the chorus, vibrato under them",
            params={"Volume Ensemble": 0.82, "Volume Percussive": 0.0,
                    "Ens: Strings I": 1.0, "Ens: Strings II": 1.0,
                    "Ens: Chorus": 1.0,
                    "Volume Strings 1": 0.80, "Volume Strings 2": 0.70,
                    "Volume Chorus": 0.45,
                    "Ens: A/R Variable": 1.0,
                    "Ens: Attack": 0.62, "Ens: Release": 0.35,
                    "Ens: Vibrato Off": 0.0, "Vibrato Speed": 0.40,
                    "Vibrato Strings Depth": 0.45,
                    "Chorus Phase Ensemble": 1.0, "Chorus Phase Speed": 0.20,
                    "Chorus Phase Depth": 0.55, "Tone Ensemble": 0.40}),
        "cast": dict(note=72, description="release -- clavi struck hard with the key-click on the front",
            params={"Volume Percussive": 0.85,
                    "Perc: Clavi": 1.0, "Volume Clavi": 0.85,
                    "Perc: Decay": 0.36, "Perc: Sustain": 0.0,
                    "Electric Piano Key-Click": 1.0,
                    "Tone Percussive": 0.70}),
        "fire": dict(note=48, description="a roar -- piano and clavi together with the tone right off, ringing on",
            params={"Volume Percussive": 0.88,
                    "Perc: Piano": 1.0, "Perc: Clavi": 1.0,
                    "Volume Piano": 0.85, "Volume Clavi": 0.60,
                    "Tune A": 0.56, "Tune B": 0.44,
                    "Perc: Decay": 0.435, "Perc: Sustain": 0.0,
                    "Tone Percussive": 0.10}),
        "lightning": dict(note=84, description="a crack -- clavi and harmonics with the shortest decay the section has",
            params={"Volume Percussive": 0.88, "Octave Up": 1.0,
                    "Perc: Clavi": 1.0, "Perc: Harmonics": 1.0,
                    "Volume Clavi": 0.75, "Volume Harmonics": 0.85,
                    "Perc: Decay": 0.22, "Perc: Sustain": 0.0,
                    "Electric Piano Key-Click": 1.0,
                    "Tone Percussive": 0.85}),
        "ice": dict(note=79, description="glass -- harmonics and electric piano up an octave, key-click on the front",
            params={"Volume Percussive": 0.82, "Octave Up": 1.0,
                    "Perc: Harmonics": 1.0, "Perc: Electric Piano": 1.0,
                    "Volume Harmonics": 0.80, "Volume Electric Piano": 0.55,
                    "Perc: Decay": 0.42, "Perc: Sustain": 0.0,
                    "Electric Piano Key-Click": 1.0,
                    "Tone Percussive": 0.75}),
        "heal": dict(note=67, description="warm -- piano and electric piano together, tremolo turning slowly under them",
            params={"Volume Percussive": 0.80,
                    "Perc: Piano": 1.0, "Perc: Electric Piano": 1.0,
                    "Volume Piano": 0.70, "Volume Electric Piano": 0.70,
                    "Perc: Decay": 0.48, "Perc: Sustain": 0.0,
                    "Perc: Tremolo": 1.0, "Perc: Tremolo Speed": 0.30,
                    "Tone Percussive": 0.40}),
        "fizzle": dict(note=52, description="it fails -- clavi alone with the two tunings pulled apart, dull and short",
            params={"Volume Percussive": 0.78,
                    "Perc: Clavi": 1.0, "Volume Clavi": 0.80,
                    "Tune A": 0.62, "Tune B": 0.38,
                    "Perc: Decay": 0.30, "Perc: Sustain": 0.0,
                    "Tone Percussive": 0.20}),
    },
}


# ------------------------------------------------------------------ Sequencair
#
# A simple synth voice bolted to a 16-step sequencer, and the sequencer is the
# point: "Note->Play" starts it from the key you hold, "Note->Transpose" moves
# it with that key, and Banks 1-16 are the steps. So the charge here is not an
# envelope at all -- it is a run of steps climbing, which is the one gather in
# this collection you can count.
#
# The voice itself is thin by comparison: one waveform, one filter, and "S: Env.
# Decay" is the only envelope stage there is -- no attack, no release. So the
# other six spells are separated by filter and waveform, and nothing here can
# swell except by sequencing it. Measured:
#
#   S: Amp Mode  1.0 is gate -- the amplifier ignores the envelope and holds to
#                note-off, and every decay value renders identically. 0.0 is the
#                envelope. Getting this backwards is what made the first pass
#                look like a broken decay curve when it was not being used.
#   S: Env. Decay (at Amp Mode 0.0)  0.2 -> 45 ms, 0.4 -> 285, 0.6 -> 2240,
#                0.8 and up holds
#   S: Amp Velocity  defaults to 0.5 and must stay there. Pinned to 0.0 -- which
#                looked like the tidy thing to do for a bank that is not played
#                by hand -- every spell collapsed to 60-90 ms whatever the decay
#                said, because the amplifier is scaled by velocity from there.
SPELLS["sequencair64"] = {
    "plugin": "Sequencair",
    "program": 0,
    "common": {
        "S: On/Off": 1.0, "S: Volume": 0.50, "S: Master Tune": 0.5,
        "S: Portamento": 0.0, "S: Porta. Legato": 0.0,
        "S: Filter Track": 0.5, "S: Filter Velo.": 1.0,
        "S: Env. Trigger": 0.0, "S: Amp Mode": 0.0, "S: Amp Velocity": 0.5,
        "Note->Play": 0.0, "Note->Stop": 0.0, "Note->Transpose": 0.0,
        "Sync": 0.0, "Loop": 0.0, "Swing": 0.5, "Reset on Play": 1.0,
        "Mode": 0.0, "Pivot Key": 0.5,
    },
    "patches": {
        "charge": dict(note=60, description="the gather -- sixteen steps climbing from the key you played, which is a rise you can count",
            params={"S: Octave": 0.5, "S: Waveform": 0.35,
                    "S: Cutoff": 0.70, "S: Resonance": 0.42,
                    "S: Filter Env.": 0.40, "S: Env. Decay": 0.58,
                    "S: Portamento": 0.18,
                    "Note->Play": 1.0, "Note->Transpose": 1.0,
                    "Tempo": 0.78, "Division": 0.75, "Loop": 0.0,
                    "Bank 1": 0.30, "Bank 2": 0.36, "Bank 3": 0.42, "Bank 4": 0.47,
                    "Bank 5": 0.52, "Bank 6": 0.56, "Bank 7": 0.60, "Bank 8": 0.64,
                    "Bank 9": 0.68, "Bank 10": 0.72, "Bank 11": 0.76, "Bank 12": 0.80,
                    "Bank 13": 0.84, "Bank 14": 0.88, "Bank 15": 0.92, "Bank 16": 1.0}),
        "cast": dict(note=72, description="release -- one note an octave up, the narrowest waveform, sequencer out of the way",
            params={"S: Octave": 0.75, "S: Waveform": 0.12,
                    "S: Cutoff": 0.70, "S: Resonance": 0.32,
                    "S: Filter Env.": 0.40, "S: Env. Decay": 0.42}),
        "fire": dict(note=48, description="a roar -- the widest waveform an octave down, resonant, the filter envelope barely helping",
            params={"S: Octave": 0.25, "S: Waveform": 0.88, "S: Volume": 0.34,
                    "S: Cutoff": 0.70, "S: Resonance": 0.55,
                    "S: Filter Env.": 0.30, "S: Env. Decay": 0.50}),
        "lightning": dict(note=84, description="a crack -- top octave, narrow, the shortest decay that is still audible",
            params={"S: Octave": 1.0, "S: Waveform": 0.08,
                    "S: Cutoff": 0.70, "S: Resonance": 0.48,
                    "S: Filter Env.": 0.35, "S: Env. Decay": 0.28}),
        "ice": dict(note=79, description="glass -- high and resonant, ringing rather than striking",
            params={"S: Octave": 0.75, "S: Waveform": 0.20,
                    "S: Cutoff": 0.70, "S: Resonance": 0.68,
                    "S: Filter Env.": 0.20, "S: Env. Decay": 0.45}),
        "heal": dict(note=67, description="warm -- middle octave, wide waveform, no resonance and the longest decay",
            params={"S: Octave": 0.5, "S: Waveform": 0.62,
                    "S: Cutoff": 0.70, "S: Resonance": 0.12,
                    "S: Filter Env.": 0.42, "S: Env. Decay": 0.55}),
        "fizzle": dict(note=52, description="it fails -- portamento dragging it flat, filter envelope doing nothing at all",
            params={"S: Octave": 0.25, "S: Waveform": 0.70,
                    "S: Cutoff": 0.46, "S: Resonance": 0.36,
                    "S: Filter Env.": 0.05, "S: Env. Decay": 0.47,
                    "S: Portamento": 0.45}),
    },
}
