#!/usr/bin/env python3
"""Read a Classic Mac OS resource fork, via AppleDouble if need be.

A Classic plug-in keeps everything in its resource fork -- code included -- and a
fork does not survive on a Linux filesystem, so it arrives wrapped in AppleDouble.
This unwraps that, walks the resource map, and lists what is inside.

The point is one question: which processor? A 'cfrg' resource means PowerPC code
fragments; bare 'CODE' resources mean 68k. That decides whether a PowerPC
interpreter is the right machine or the wrong one.

    tools/rsrcdump.py <file> [--dump TYPE,ID outfile]
"""
import struct, sys

def be(fmt, b, o):
    return struct.unpack_from(">" + fmt, b, o)

def appledouble_rsrc(d):
    """Return the resource fork bytes, unwrapping AppleDouble/AppleSingle."""
    if len(d) < 26:
        return d
    magic, = be("I", d, 0)
    if magic not in (0x00051600, 0x00051607):
        return d                                   # already a bare fork
    n, = be("H", d, 24)
    for i in range(n):
        eid, off, ln = be("III", d, 26 + i * 12)
        if eid == 2:                               # 2 = resource fork
            return d[off:off + ln]
    return b""

def resources(fork):
    """{type: {id: (name, bytes)}} from a resource fork."""
    if len(fork) < 16:
        return {}
    dataOff, mapOff, dataLen, mapLen = be("IIII", fork, 0)
    if mapOff + 30 > len(fork):
        return {}
    tlOff, nlOff = be("HH", fork, mapOff + 24)
    tl = mapOff + tlOff
    ntypes, = be("H", fork, tl)
    out = {}
    for i in range(ntypes + 1):
        o = tl + 2 + i * 8
        rtype = fork[o:o + 4].decode("mac-roman", "replace")
        nrefs, rlOff = be("HH", fork, o + 4)
        out[rtype] = {}
        for j in range(nrefs + 1):
            r = tl + rlOff + j * 12
            rid, nameOff = be("hH", fork, r)
            attrs = fork[r + 4]
            doff = (fork[r + 5] << 16) | (fork[r + 6] << 8) | fork[r + 7]
            start = dataOff + doff
            ln, = be("I", fork, start)
            name = ""
            if nameOff != 0xFFFF:
                p = mapOff + nlOff + nameOff
                name = fork[p + 1:p + 1 + fork[p]].decode("mac-roman", "replace")
            out[rtype][rid] = (name, fork[start + 4:start + 4 + ln])
    return out

def show_cfrg(blob):
    """A 'cfrg' 0 resource: where the code fragments live and for which CPU."""
    if len(blob) < 32:
        return
    n, = be("I", blob, 28)
    print("      %d fragment(s):" % n)
    o = 32
    for i in range(n):
        if o + 43 > len(blob):
            break
        arch = blob[o:o + 4].decode("ascii", "replace")
        updatelevel, curver, oldver, stacksize, libdir = be("IIIII", blob, o + 4)
        fgkind, fgloc, offset, length = blob[o + 24], blob[o + 25], *be("II", blob, o + 26)
        memb, = be("H", blob, o + 34)
        nlen = blob[o + 36]
        name = blob[o + 37:o + 37 + nlen].decode("mac-roman", "replace")
        where = {0: "in this file's data fork", 1: "in this resource fork",
                 2: "by name", 3: "in memory"}.get(fgloc, "location %d" % fgloc)
        print("        %-6s %-22s %s, offset 0x%x, length %s"
              % (arch, name or "(unnamed)", where, offset,
                 "to end" if length == 0 else "0x%x" % length))
        o += memb if memb else 43

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    fork = appledouble_rsrc(d)
    if "--dump" in sys.argv:
        spec = sys.argv[sys.argv.index("--dump") + 1]
        rtype, rid = spec.split(",")
        blob = resources(fork)[rtype][int(rid)][1]
        out = sys.argv[sys.argv.index("--dump") + 2]
        open(out, "wb").write(blob)
        print("wrote %s (%d bytes) -- first 16: %s" % (out, len(blob), blob[:16].hex()))
        return
    print("%s: %d bytes of resource fork" % (sys.argv[1], len(fork)))
    res = resources(fork)
    if not res:
        print("  no resource map found")
        return
    for rtype in sorted(res):
        ids = sorted(res[rtype])
        total = sum(len(res[rtype][i][1]) for i in ids)
        print("  %-6s %3d resource(s), %7d bytes   ids %s"
              % (rtype, len(ids), total,
                 ",".join(str(i) for i in ids[:8]) + (" ..." if len(ids) > 8 else "")))
        if rtype == "cfrg":
            for i in ids:
                show_cfrg(res[rtype][i][1])
    print()
    if "cfrg" in res:
        print("  -> has 'cfrg': PowerPC code fragments (CFM/PEF)")
    if "CODE" in res:
        print("  -> has 'CODE': 68k code")
    if "aEff" in res or "fxbk" in res:
        print("  -> VST resources present")

main()
