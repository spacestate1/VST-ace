#!/usr/bin/env python3
"""Disassemble an exported function out of a Microsoft DLL by name.

The msvcp shim in peload/msvcp_shim.h is written from what the real library
does, not from guesswork, so every layout in it starts here: name the export
and read the prologue. Usage:

    tools/msvcp_dis.py runtime/msvcp120.dll '?exceptions@ios_base@std@@QEAAXH@Z'
    tools/msvcp_dis.py runtime/msvcp120.dll --grep _Getcat
    tools/msvcp_dis.py runtime/msvcp120.dll --rva 0x3ccc4 --bytes 200
"""
import struct, subprocess, sys, tempfile, os, argparse


class PE:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise SystemExit('%s: not a PE file' % path)
        self.machine = struct.unpack_from('<H', d, pe + 4)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        self.pe64 = struct.unpack_from('<H', d, pe + 24)[0] == 0x20b
        self.base = struct.unpack_from('<Q' if self.pe64 else '<I', d,
                                       pe + 24 + (24 if self.pe64 else 28))[0]
        dd = pe + 24 + (112 if self.pe64 else 96)
        self.expva = struct.unpack_from('<I', d, dd)[0]
        self.secs = []
        so = pe + 24 + optsz
        for i in range(nsec):
            b = so + i * 40
            vsz, va, rsz, ptr = struct.unpack_from('<IIII', d, b + 8)
            self.secs.append((va, vsz, ptr, rsz))

    def off(self, rva):
        for va, vsz, ptr, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return ptr + (rva - va)
        return None

    def exports(self):
        d, e = self.d, self.off(self.expva)
        nnam = struct.unpack_from('<I', d, e + 24)[0]
        afun, anam, aord = struct.unpack_from('<III', d, e + 28)
        fo, no, oo = self.off(afun), self.off(anam), self.off(aord)
        out = {}
        for i in range(nnam):
            o = self.off(struct.unpack_from('<I', d, no + i * 4)[0])
            nm = d[o:d.index(b'\0', o)].decode('latin1')
            ordi = struct.unpack_from('<H', d, oo + i * 2)[0]
            out[nm] = struct.unpack_from('<I', d, fo + ordi * 4)[0]
        return out


def disasm(pe, rva, n):
    o = pe.off(rva)
    if o is None:
        raise SystemExit('rva 0x%x is not in any section' % rva)
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(pe.d[o:o + n])
        tmp = f.name
    try:
        arch = 'i386:x86-64' if pe.pe64 else 'i386'
        out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', arch,
                              '-M', 'intel', '--adjust-vma=%d' % rva, tmp],
                             capture_output=True, text=True).stdout
    finally:
        os.unlink(tmp)
    return '\n'.join(out.splitlines()[6:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('name', nargs='?')
    ap.add_argument('--grep')
    ap.add_argument('--rva')
    ap.add_argument('--bytes', type=int, default=120)
    a = ap.parse_args()
    pe = PE(a.dll)
    if a.grep:
        for nm, rva in sorted(pe.exports().items()):
            if a.grep in nm:
                print('0x%06x  %s' % (rva, nm))
        return
    if a.rva:
        rva = int(a.rva, 0)
        print('=== rva 0x%x ===' % rva)
        print(disasm(pe, rva, a.bytes))
        return
    exp = pe.exports()
    if a.name not in exp:
        near = [n for n in exp if a.name in n]
        raise SystemExit('%s not exported.%s' % (
            a.name, ('\nDid you mean:\n  ' + '\n  '.join(near[:10])) if near else ''))
    rva = exp[a.name]
    print('=== %s  rva 0x%x ===' % (a.name, rva))
    print(disasm(pe, rva, a.bytes))


main()
