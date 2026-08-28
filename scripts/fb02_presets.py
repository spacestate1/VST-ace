#!/usr/bin/env python3
"""Decode FB-02's preset banks into readable 4-operator FM voices.

FB-02 stores 12 banks of 336 programs. The FXB container is the same as every
other Full Bucket plugin, but the per-program body is not a run of doubles --
it mirrors the FB-01's own packed voice, one byte per parameter.

Body (158 bytes):

    +0        u8      version (1)
    +1..11    char    "FB-02_1.1.0"
    +16..22   char[7] voice name        (the FB-01's 7-character name field)
    +23       u8      6
    +24       double  master level
    +32..71   mostly zero
    +72..157  86 B    the packed voice, below

Packed voice (86 bytes):

    +0        u8      0x55, constant across all 4032 programs -- a marker
    +1..17    17 B    the global parameters, registration order
    +18..34   17 B    operator 1
    +35..51   17 B    operator 2
    +52..68   17 B    operator 3
    +69..85   17 B    operator 4

Two independent things establish this:

  * The registration function (0x1805355c0) computes each operator's parameter
    index as `imul $0x11` / `sub $0x11` then `lea 0x17(...)` -- 17 parameters
    per operator, first at index 23. The four "OP%i: Wave" parameters are
    registered separately starting at index 2 (`mov $0x2,%ebx`), which fills
    the gap at p02..p05.

  * Every byte's observed range across all 336 factory programs matches what
    its name implies, 17/17 for the globals and 17/17 within each operator:
    Algorithm 0-7, Pitch Bend Range 0-12, Feedback 0-7, Level 0-127,
    Frequency 0-15, Detune 0-7, Keyb. Scaling Rate 0-3, Attack/Decay 0-31,
    Sustain/Release 0-15. Those are the Yamaha 4-op ranges exactly.

Usage:
    fb02_presets.py <PROGINIT> [-n N] [--csv out.csv]
"""
import argparse
import csv
import struct
import sys

FXB_HEADER = 160
BODY = 158
PAYLOAD_AT = 72

# p06..p22, in the order the binary registers them.
GLOBALS = [
    "Algorithm", "Transpose", "Pitch Bend Range", "Portamento", "Feedback",
    "Mode", "PMD Controller", "Output Left", "Output Right", "LFO Enable",
    "LFO Waveform", "LFO Speed", "LFO Sync", "LFO AM Depth",
    "LFO AM Sensitivity", "LFO PM Depth", "LFO PM Sensitivity",
]

# The 17 per-operator parameters, in the order the loop registers them.
OPPARAMS = [
    "Enable", "Level", "Velocity", "Boost", "Frequency", "Inharmonic",
    "Detune", "Keyb. Scaling Type", "Level Adjust", "Keyb. Scaling Depth",
    "Keyb. Scaling Rate", "Attack", "Attack Velocity", "Decay 1", "Decay 2",
    "Sustain", "Release",
]

NOPS = 4
OPSTRIDE = 17

# Distinct abbreviations: several parameters share a first word (Level /
# Level Adjust, three "Keyb. Scaling ...", Attack / Attack Velocity).
SHORT = {
    "Enable": "en", "Level": "lvl", "Velocity": "vel", "Boost": "bst",
    "Frequency": "frq", "Inharmonic": "inh", "Detune": "det",
    "Keyb. Scaling Type": "kst", "Level Adjust": "ladj",
    "Keyb. Scaling Depth": "ksd", "Keyb. Scaling Rate": "ksr",
    "Attack": "atk", "Attack Velocity": "atkv", "Decay 1": "d1",
    "Decay 2": "d2", "Sustain": "sus", "Release": "rel",
}


def parse_bank(data):
    if data[:4] != b"CcnK" or data[8:12] != b"FBCh":
        sys.exit("not an FXB opaque-chunk bank")
    fx_id = data[16:20].decode("latin1")
    nprog = struct.unpack_from(">I", data, 24)[0]
    chunk = data[FXB_HEADER:]
    if chunk[:4] != b"tffp":
        sys.exit("chunk is not 'tffp'")

    progs, off = [], 8
    while off + 4 <= len(chunk) and len(progs) < nprog:
        nlen = struct.unpack_from("<I", chunk, off)[0]
        off += 4
        name = chunk[off:off + nlen].decode("latin1")
        off += nlen
        progs.append((name, chunk[off:off + BODY]))
        off += BODY
    return fx_id, nprog, progs


# Transpose is a signed byte: "The Pad" stores 244, i.e. -12, an octave down.
SIGNED = {"Transpose"}


def decode(body):
    pay = body[PAYLOAD_AT:PAYLOAD_AT + 86]
    v = {"marker": pay[0]}
    for i, n in enumerate(GLOBALS):
        b = pay[1 + i]
        v[n] = b - 256 if (n in SIGNED and b > 127) else b
    for op in range(NOPS):
        base = 18 + op * OPSTRIDE
        for i, n in enumerate(OPPARAMS):
            v[f"OP{op+1} {n}"] = pay[base + i]
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bank")
    ap.add_argument("-n", type=int, default=3, help="programs to print")
    ap.add_argument("--csv", help="write every program to CSV")
    a = ap.parse_args()

    fx_id, nprog, progs = parse_bank(open(a.bank, "rb").read())
    print(f"{a.bank}: id={fx_id!r} {len(progs)}/{nprog} programs\n")

    for name, body in progs[:a.n]:
        v = decode(body)
        print(f"--- {name.strip()!r} ---")
        print("  " + "  ".join(f"{n}={v[n]}" for n in GLOBALS[:6]))
        print("  " + "  ".join(f"{n}={v[n]}" for n in GLOBALS[6:]))
        for op in range(NOPS):
            vals = "  ".join(f"{SHORT[n]}={v[f'OP{op+1} {n}']}"
                             for n in OPPARAMS)
            print(f"  OP{op+1}: {vals}")
        print()

    if a.csv:
        cols = ["index", "name"] + GLOBALS + \
               [f"OP{o+1} {n}" for o in range(NOPS) for n in OPPARAMS]
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(cols)
            for i, (name, body) in enumerate(progs):
                v = decode(body)
                w.writerow([i, name.strip()] + [v[c] for c in cols[2:]])
        print(f"-> {a.csv}  ({len(progs)} programs x {len(cols)-2} parameters)")


if __name__ == "__main__":
    main()
