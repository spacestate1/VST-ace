#!/usr/bin/env python3
"""Recover a Full Bucket plugin's parameter index -> name map from its binary.

Generalises recover_params.py, which needed a Ghidra decompile to locate the
registration function. This finds it unaided, so it works on any plugin in the
catalogue -- including ones never imported into Ghidra.

How it locates the function: every InitParam call loads the parameter name into
%rdx with a rip-relative LEA. Scanning .text for those and keeping the ones
that land on a plausible NUL-terminated string in .rdata gives a scatter of
addresses; the registration function is the densest cluster by far. .pdata then
gives that function's exact bounds.

How it reads the index: each call is preceded by an inlined bounds check on the
parameter array,

    and $0xfffffffffffffff8,%rax
    cmp $imm,%rax               ; imm = index * 8
    jbe skip

so the index is the immediate over eight (index 0 uses `test` instead of `cmp`).

Usage:  python3 recover_params_any.py <plugin.dll> [-v]
"""
import re
import struct
import subprocess
import sys

LEA_OPCODES = {
    b"\x48\x8d\x15": "rdx",   # the name argument
}


def sections(img):
    e = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, e + 6)[0]
    optsz = struct.unpack_from("<H", img, e + 20)[0]
    base = struct.unpack_from("<Q", img, e + 24 + 24)[0]
    out = {}
    for i in range(nsec):
        p = e + 24 + optsz + i * 40
        name = img[p:p + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, rp = struct.unpack_from("<IIII", img, p + 8)
        out[name] = (va, max(vs, rs), rp, rs)
    return base, out


def cstring_at(img, secs, base, va, maxlen=64):
    """Return the NUL-terminated printable string at a virtual address."""
    rva = va - base
    for _, (sva, span, rp, rs) in secs.items():
        if sva <= rva < sva + span and rva - sva < rs:
            o = rp + (rva - sva)
            end = img.find(b"\0", o, o + maxlen)
            if end < 0:
                return None
            s = img[o:end]
            if not s or len(s) < 2:
                return None
            if any(c < 32 or c > 126 for c in s):
                return None
            return s.decode("latin1")
    return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        sys.exit(__doc__)
    verbose = "-v" in sys.argv

    img = open(path, "rb").read()
    base, secs = sections(img)
    tva, tspan, trp, trs = secs[".text"]
    text = img[trp:trp + trs]

    # --- locate by the inlined bounds check, not by string density ---
    # Every InitParam call is guarded by
    #     and $0xfffffffffffffff8,%rax
    #     cmp $imm,%rax
    # and that adjacent pair is rare enough elsewhere to be a reliable marker.
    # Ranking by "most string references" instead picks some unrelated
    # string-heavy function -- on FB-7999 it lands 750 KB away from the right
    # one.
    SIG = b"\x48\x83\xe0\xf8"          # and $0xfffffffffffffff8,%rax
    marks = []
    start = 0
    while True:
        i = text.find(SIG, start)
        if i < 0:
            break
        start = i + 1
        nxt = text[i + 4:i + 6]
        if nxt == b"\x48\x83" or nxt[:1] == b"\x48" and text[i + 5:i + 6] == b"\x3d":
            marks.append(base + tva + i)
    if not marks:
        sys.exit("no parameter bounds-check pattern found")

    # Score whole functions rather than a sliding byte window: iPlug2 uses this
    # same accessor all over the binary, so an arbitrary window lands on
    # whichever unrelated region happens to be dense. .pdata gives the real
    # function boundaries, and the registration function is the one containing
    # the most bounds checks by a wide margin.
    import bisect
    marks.sort()
    pva, pspan, prp, prs = secs[".pdata"]

    # --at <hex> pins the function explicitly. Needed when a plugin registers
    # parameters in a loop rather than as a straight run of literal calls --
    # FB-02 builds its per-operator names ("OP1_...", "OP4_...") at runtime, so
    # it carries far fewer inline string references and does not top the
    # ranking. Seed it from a known parameter name instead.
    fn, best_n = None, 0
    if "--at" in sys.argv:
        at = int(sys.argv[sys.argv.index("--at") + 1], 16)
        for off in range(0, prs, 12):
            b, e2, _u = struct.unpack_from("<III", img, prp + off)
            if b <= at - base < e2:
                fn = (base + b, base + e2)
                break
        if not fn:
            sys.exit(f"{at:#x} is not inside any .pdata function")
    else:
        for off in range(0, prs, 12):
            b, e2, _u = struct.unpack_from("<III", img, prp + off)
            if e2 <= b:
                continue
            n = (bisect.bisect_left(marks, base + e2)
                 - bisect.bisect_left(marks, base + b))
            if n > best_n:
                best_n, fn = n, (base + b, base + e2)
        if not fn:
            sys.exit("could not find a registration function")
        if verbose:
            print(f"best function holds {best_n} bounds checks")
    print(f"registration function: {fn[0]:#x} .. {fn[1]:#x} ({fn[1]-fn[0]} bytes)\n")

    # --- disassemble and pair index with name ---
    dis = subprocess.run(
        ["objdump", "-d", f"--start-address={hex(fn[0])}",
         f"--stop-address={hex(fn[1])}", path],
        capture_output=True, text=True).stdout

    LINE = re.compile(r"^\s+[0-9a-f]+:\s+(?:[0-9a-f]{2} )+\s*(.*)$")
    CMP = re.compile(r"cmp\s+\$0x([0-9a-f]+),%rax")
    TEST = re.compile(r"test\s+\$0xfffffffffffffff8,%rax")
    LEA = re.compile(r"lea\s+0x[0-9a-f]+\(%rip\),%rdx\s+#\s+0x([0-9a-f]+)")
    CALL = re.compile(r"call\s+0x([0-9a-f]+)")

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
            name = cstring_at(img, secs, base, int(l.group(1), 16))
        elif CALL.search(t) and idx is not None and name:
            # Trailing "reserved" slots are registered by a loop with a
            # computed index and carry no cmp of their own.
            if name != "reserved":
                params[idx] = name
            idx = name = None

    print(f"recovered {len(params)} parameters\n")
    for i in sorted(params):
        print(f"  p{i:02d}  {params[i]}")


if __name__ == "__main__":
    main()
