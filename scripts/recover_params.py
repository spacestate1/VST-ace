#!/usr/bin/env python3
"""Recover FB-7999's parameter index -> name mapping from the binary.

The mapping is what makes the extracted preset banks usable: without it the
192 factory programs are 70 anonymous doubles each.

All the InitParam calls live in one function, FUN_18052f240. Reading them out
of the Ghidra pseudo-C does *not* work -- the decompiler reorders independent
statements, so the call order there is not the parameter order (it puts
"OSC1 Octave" before "OSC1 Waveform", and the resulting map is off by one
against the stored preset data).

The reliable source is the machine code. Each call is preceded by an inlined
bounds check on the parameter array:

    movslq 0x14(%rbx),%rax        ; count
    and    $0xfffffffffffffff8,%rax
    cmp    $imm,%rax              ; imm = index * 8
    jbe    skip

so the index is the immediate divided by eight (index 0 uses a `test` instead
of a `cmp`). The name is the string pointer loaded into %rdx.

Cross-check: every column of BANK_A then falls inside the range its recovered
name predicts -- waveforms 1..16, levels and envelope stages 0..31, octaves
0..2, cutoff 0..63, velocity 0..7, Mode 0..3. That agreement across 59
parameters is what confirms the result.

Usage:  python3 recover_params.py [plugin.dll] [decompiled.c]
"""
import json
import re
import struct
import subprocess
import sys

DLL = sys.argv[1] if len(sys.argv) > 1 else "windows/VST2-64/fb799964.dll"
DEC = sys.argv[2] if len(sys.argv) > 2 else "re/out/fb7999_decompiled.c"
FUNC = "FUN_18052f240"
IMAGE_BASE = 0x180000000

# ---- locate the function in the decompiled dump (for its address and size) ----
with open(DEC, errors="replace") as f:
    hdr = re.search(rf"{FUNC}\s+@ ([0-9a-f]+)\s+\((\d+) bytes\)", f.read())
if not hdr:
    sys.exit(f"{FUNC} not found in {DEC}")
start, size = int(hdr.group(1), 16), int(hdr.group(2))

# ---- resolve RVAs so string pointers can be dereferenced ----
img = open(DLL, "rb").read()
e = struct.unpack_from("<I", img, 0x3C)[0]
nsec = struct.unpack_from("<H", img, e + 6)[0]
optsz = struct.unpack_from("<H", img, e + 20)[0]
secs = []
for i in range(nsec):
    o = e + 24 + optsz + i * 40
    vs, va, rs, rp = struct.unpack_from("<IIII", img, o + 8)
    secs.append((va, max(vs, rs), rp, rs))


def cstr(addr):
    rva = addr - IMAGE_BASE
    for va, span, rp, rs in secs:
        if va <= rva < va + span and rva - va < rs:
            o = rp + (rva - va)
            return img[o:img.index(b"\0", o)].decode("latin1")
    return None


# ---- walk the disassembly ----
dis = subprocess.run(
    ["objdump", "-d", f"--start-address={hex(start)}",
     f"--stop-address={hex(start + size)}", DLL],
    capture_output=True, text=True).stdout

LINE = re.compile(r"^\s+[0-9a-f]+:\s+(?:[0-9a-f]{2} )+\s*(.*)$")
CMP = re.compile(r"cmp\s+\$0x([0-9a-f]+),%rax")
TEST = re.compile(r"test\s+\$0xfffffffffffffff8,%rax")
LEA = re.compile(r"lea\s+0x[0-9a-f]+\(%rip\),%rdx\s+#\s+0x([0-9a-f]+)")
CALL = re.compile(r"call\s+0x(1805129a0|180512b00)")   # InitEnum / InitDouble

params, idx, name = {}, None, None
for line in dis.splitlines():
    m = LINE.match(line)
    if not m:
        continue
    t = m.group(1)
    if (c := CMP.search(t)):
        idx = int(c.group(1), 16) // 8
    elif TEST.search(t):
        idx = 0
    elif (l := LEA.search(t)):
        name = cstr(int(l.group(1), 16))
    elif (k := CALL.search(t)) and idx is not None and name:
        kind = "enum" if k.group(1) == "1805129a0" else "double"
        # The trailing "reserved" slots are registered by a loop with a
        # computed index, so they carry no `cmp $imm` of their own and would
        # otherwise be misattributed to whatever index was seen last.
        if name == "reserved":
            idx = None
            name = None
            continue
        params[idx] = (name, kind)
        idx = None
        name = None

print(f"recovered {len(params)} parameters\n")
for i in sorted(params):
    print(f"  p{i:02d}  {params[i][0]:<24} [{params[i][1]}]")

json.dump({str(i): params[i] for i in sorted(params)},
          open("re/out/param_map.json", "w"), indent=1)
print("\n-> re/out/param_map.json")
