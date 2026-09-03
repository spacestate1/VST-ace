#!/usr/bin/env python3
"""Which Win32 imports does the corpus ask for that this host only stubs?

A stub returns a failure value and the caller takes its error path -- or, worse,
reads the zero as success. This ranks what is missing by how many plug-ins want
it, so the next thing to implement is the next line of output rather than
whichever one a single plug-in happened to trip over.

    tools/stub_audit.py /path/to/corpus [more paths...]
"""
import os, re, struct, sys, collections


def sections(d, pe):
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    pe64 = struct.unpack_from('<H', d, pe + 24)[0] == 0x20b
    secs = []
    so = pe + 24 + optsz
    for i in range(nsec):
        b = so + i * 40
        vsz, va, rsz, ptr = struct.unpack_from('<IIII', d, b + 8)
        secs.append((va, vsz, ptr, rsz))
    return secs, pe64


def imports(path):
    try:
        d = open(path, 'rb').read()
    except OSError:
        return {}
    if len(d) < 0x40 or d[:2] != b'MZ':
        return {}
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if pe + 24 > len(d) or d[pe:pe + 4] != b'PE\0\0':
        return {}
    secs, pe64 = sections(d, pe)

    def off(rva):
        for va, vsz, ptr, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                return ptr + (rva - va)
        return None

    dd = pe + 24 + (112 if pe64 else 96)
    imp = struct.unpack_from('<I', d, dd + 8)[0]
    if not imp:
        return {}
    o = off(imp)
    out = {}
    w = 8 if pe64 else 4
    hi = 63 if pe64 else 31
    while o is not None and o + 20 <= len(d):
        oft, ts, fc, nm, fta = struct.unpack_from('<IIIII', d, o)
        if not nm:
            break
        no = off(nm)
        if no is None:
            break
        dll = d[no:d.index(b'\0', no)].decode('latin1').lower()
        t = off(oft or fta)
        names = []
        while t is not None:
            v = struct.unpack_from('<Q' if pe64 else '<I', d, t)[0]
            if not v:
                break
            if v >> hi:
                names.append('ordinal#%d' % (v & 0xffff))
            else:
                so_ = off(v & 0x7fffffff)
                if so_ is not None:
                    names.append(d[so_ + 2:d.index(b'\0', so_ + 2)].decode('latin1'))
            t += w
        out.setdefault(dll, []).extend(names)
        o += 20
    return out


def implemented(root):
    s = open(os.path.join(root, 'peload', 'winstubs.h')).read()
    names = set(re.findall(r'\{\s*"[^"]+",\s*"([^"]+)"', s))
    names |= set(re.findall(r'\bS[MW]?\(\s*"[^"]+"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)', s))
    try:
        g = open(os.path.join(root, 'peload', 'win32gui.h')).read()
        names |= set(re.findall(r'\{\s*"[^"]+",\s*"([^"]+)"', g))
        names |= set(re.findall(r'\bS[MW]?\(\s*"[^"]+"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)', g))
    except OSError:
        pass
    return names


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    have = implemented(root)
    want = collections.Counter()
    where = collections.defaultdict(set)
    files = 0
    for base in sys.argv[1:]:
        for dirpath, _dirs, fnames in os.walk(base):
            for f in fnames:
                if not f.lower().endswith(('.dll', '.vst3', '.sem', '.exe')):
                    continue
                p = os.path.join(dirpath, f)
                im = imports(p)
                if im:
                    files += 1
                for dll, names in im.items():
                    for n in names:
                        want[(dll, n)] += 1
                        where[(dll, n)].add(f)
    missing = [(c, dll, n) for (dll, n), c in want.items() if n not in have]
    missing.sort(key=lambda x: (-x[0], x[1], x[2]))
    print("%d binaries scanned, %d distinct imports, %d not implemented\n"
          % (files, len(want), len(missing)))
    for c, dll, n in missing:
        print("%3d  %-18s %s" % (c, dll, n))


main()
