#!/usr/bin/env python3
"""Read a PEF container -- the executable format of Classic Mac OS and Carbon.

Written to answer one question before any emulator is contemplated: what does a
Classic plug-in actually import? The imported-library table names the Toolbox
surface that would have to exist, and the symbol list gives its size. Everything
else about running one of these (a PowerPC interpreter, resource forks, TVectors)
is wasted effort if that surface turns out to be unreasonable.

All PEF fields are big-endian regardless of the host.

    tools/pefdump.py <file>            summary, libraries, symbol counts
    tools/pefdump.py <file> --symbols  every imported symbol
"""
import struct, sys

SECTION_KIND = {
    0: "code", 1: "unpacked data", 2: "pattern-initialised data",
    3: "constant", 4: "loader", 5: "debug",
    6: "executable data", 7: "exception", 8: "traceback",
}
# The top 8 bits of an imported symbol's entry say what kind of thing it is.
SYM_CLASS = {0: "code", 1: "data", 2: "TVector", 3: "TOC", 4: "glue"}


def be(fmt, buf, off):
    return struct.unpack_from(">" + fmt, buf, off)


def cstr(buf, off):
    end = buf.index(b"\0", off)
    return buf[off:end].decode("mac-roman", "replace")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    want_syms = "--symbols" in sys.argv

    if d[:8] != b"Joy!peff":
        raise SystemExit("not a PEF container (no 'Joy!peff' tag)")
    arch = d[8:12].decode("ascii", "replace")
    ver, stamp = be("II", d, 12)
    nsec, ninst = be("HH", d, 32)
    print("PEF container: architecture %r, format version %d" % (arch, ver))
    print("  %d section(s), %d instantiated" % (nsec, ninst))
    if arch != "pwpc":
        print("  NOTE: %r is not PowerPC -- 68k code needs a different interpreter"
              % arch)

    loader = None
    for i in range(nsec):
        o = 40 + i * 28
        (nameoff, addr, total, unpacked, packed, coff) = be("iIIIII", d, o)
        kind, share, align, _r = be("BBBB", d, o + 24)
        print("  [%d] %-26s vm 0x%08x total %-8d packed %-8d at 0x%x"
              % (i, SECTION_KIND.get(kind, "kind %d" % kind), addr, total,
                 packed, coff))
        if kind == 4:
            loader = (coff, packed)

    if not loader:
        print("  no loader section: nothing to import")
        return
    lo, llen = loader
    (mainSec, mainOff, initSec, initOff, termSec, termOff,
     nlibs, nsyms, nrelsec, relOff, strOff, hashOff, hashPow,
     nexp) = be("iIiIiIIIIIIIII", d, lo)
    print("\nloader section: %d imported librar(ies), %d imported symbol(s), "
          "%d export(s)" % (nlibs, nsyms, nexp))
    print("  main at section %d offset 0x%x, init %d, term %d"
          % (mainSec, mainOff, initSec, termSec))

    strings = lo + strOff
    libs = []
    for i in range(nlibs):
        o = lo + 56 + i * 24
        nameoff, oldv, curv, cnt, first = be("IIIII", d, o)
        libs.append((cstr(d, strings + nameoff), cnt, first))

    symtab = lo + 56 + nlibs * 24
    syms = []
    for i in range(nsyms):
        v, = be("I", d, symtab + i * 4)
        syms.append((SYM_CLASS.get(v >> 24, "class %d" % (v >> 24)),
                     cstr(d, strings + (v & 0xFFFFFF))))

    print("\nimported libraries -- this is the API surface that would need writing:")
    for name, cnt, first in libs:
        print("  %-24s %4d symbol(s)" % (name, cnt))
        if want_syms:
            for cls, nm in syms[first:first + cnt]:
                print("        %-8s %s" % (cls, nm))
    if not want_syms and syms:
        print("\n  (--symbols to list all %d)" % len(syms))


main()
