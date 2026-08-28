#!/usr/bin/env python3
"""Map image-relative offsets in a Mach-O to the nearest preceding symbol.

peload's fault reporter prints frames as `image+0xNNNN` because it has no
symbolizer of its own -- the offsets are relative to the slice's __TEXT vmaddr,
which is what this resolves. Usage:

    tools/macsym.py <mach-o or .vst bundle> 0x958e3 [0x...]

Demangles C++ names when c++filt is available, since every plugin here is C++.
"""
import os, struct, subprocess, sys

FAT, FAT64 = 0xcafebabe, 0xcafebabf
MH64 = 0xfeedfacf
CPU_X86_64 = 0x01000007


def slices(data):
    magic, = struct.unpack_from(">I", data, 0)
    if magic in (FAT, FAT64):
        n, = struct.unpack_from(">I", data, 4)
        step = 32 if magic == FAT64 else 20
        for i in range(n):
            off = 8 + i * step
            cpu, = struct.unpack_from(">i", data, off)
            if magic == FAT64:
                o, sz = struct.unpack_from(">QQ", data, off + 8)
            else:
                o, sz = struct.unpack_from(">II", data, off + 8)
            yield cpu, o, sz
    else:
        yield CPU_X86_64, 0, len(data)


def symbols(data, base):
    """Return (value, name) for every defined symbol in the slice at `base`."""
    magic, = struct.unpack_from("<I", data, base)
    if magic != MH64:
        raise SystemExit("not a 64-bit Mach-O slice")
    ncmds, = struct.unpack_from("<I", data, base + 16)
    off = base + 32
    out, text = [], 0
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", data, off)
        if cmd == 0x19:                                  # LC_SEGMENT_64
            name = data[off + 8:off + 24].rstrip(b"\0")
            if name == b"__TEXT":
                text, = struct.unpack_from("<Q", data, off + 24)
        elif cmd == 0x02:                                # LC_SYMTAB
            symoff, nsyms, stroff, _ = struct.unpack_from("<IIII", data, off + 8)
            for i in range(nsyms):
                e = base + symoff + i * 16
                strx, typ, _sect, _d = struct.unpack_from("<IBBH", data, e)
                val, = struct.unpack_from("<Q", data, e + 8)
                if not val or (typ & 0x0e) != 0x0e:      # N_SECT only
                    continue
                end = data.index(b"\0", base + stroff + strx)
                out.append((val, data[base + stroff + strx:end].decode("utf8", "replace")))
        off += sz
    out.sort()
    return out, text


def demangle(names):
    try:
        p = subprocess.run(["c++filt"], input="\n".join(names), text=True,
                           capture_output=True)
        if p.returncode == 0:
            return p.stdout.splitlines()
    except FileNotFoundError:
        pass
    return names


def disasm(data, base, syms, text, addr, count):
    """Disassemble `count` bytes at an image offset, via objdump on raw bytes."""
    import tempfile
    ncmds, = struct.unpack_from("<I", data, base + 16)
    off, segs = base + 32, []
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", data, off)
        if cmd == 0x19:
            nsects, = struct.unpack_from("<I", data, off + 64)
            so = off + 72
            for _ in range(nsects):
                a, size = struct.unpack_from("<QQ", data, so + 32)
                fo, = struct.unpack_from("<I", data, so + 48)
                segs.append((a, size, fo))
                so += 80
        off += sz
    vm = addr + text
    for a, size, fo in segs:
        if a <= vm < a + size:
            fpos = base + fo + (vm - a)
            with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
                f.write(data[fpos:fpos + count])
                tmp = f.name
            out = subprocess.run(["objdump", "-D", "-b", "binary",
                                  "-m", "i386:x86-64", "-M", "intel",
                                  "--adjust-vma=0x%x" % addr, tmp],
                                 capture_output=True, text=True).stdout
            os.unlink(tmp)
            return out
    return "address not in any section"


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    if os.path.isdir(path):                              # a bundle: find the binary
        mac = os.path.join(path, "Contents", "MacOS")
        path = os.path.join(mac, os.listdir(mac)[0])
    data = open(path, "rb").read()
    base = next((o for cpu, o, _ in slices(data) if cpu == CPU_X86_64), None)
    if base is None:
        raise SystemExit("no x86_64 slice")
    syms, text = symbols(data, base)

    if sys.argv[2] == "--disasm":
        addr, n = int(sys.argv[3], 0), int(sys.argv[4], 0) if len(sys.argv) > 4 else 64
        print(disasm(data, base, syms, text, addr, n))
        return

    hits = []
    for a in sys.argv[2:]:
        addr = int(a, 0) + text
        lo, hi = 0, len(syms)
        while lo < hi:                                   # last symbol <= addr
            mid = (lo + hi) // 2
            if syms[mid][0] <= addr: lo = mid + 1
            else: hi = mid
        hits.append((a, syms[lo - 1] if lo else (0, "?")))
    for (a, (val, name)), pretty in zip(hits, demangle([h[1][1] for h in hits])):
        print("%-12s %s+0x%x" % (a, pretty, int(a, 0) + text - val))


main()
