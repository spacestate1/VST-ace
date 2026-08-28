#!/usr/bin/env python3
"""Report each DirectWrite interface method's vtable slot and i386 argument size.

The COM shim's methods are called by the plugin through a vtable, and on i386
every one of them is stdcall -- so a method whose C prototype has the wrong
number of parameters pops the wrong number of bytes and drifts the caller's
stack. tools/check_arity.py cannot see these (they are not Win32 exports), so
derive the truth from mingw-w64's dwrite headers, which declare each interface
as a C vtable struct with the methods in slot order.
"""
import os, re, sys

INC = "/usr/i686-w64-mingw32/include"
EIGHT = ("UINT64", "INT64", "ULONGLONG", "LONGLONG", "DOUBLE", "double", "FILETIME")

def parse(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    src = re.sub(r'//[^\n]*', '', src)
    out = {}
    for m in re.finditer(r'typedef struct (\w+)Vtbl\s*\{(.*?)\n\}\s*\1Vtbl;', src, re.S):
        iface, body = m.group(1), m.group(2)
        methods = []
        for mm in re.finditer(r'\(\s*(?:STDMETHODCALLTYPE|WINAPI)\s*\*\s*(\w+)\s*\)\s*\(([^;]*?)\)\s*;',
                              body, re.S):
            name, params = mm.group(1), mm.group(2)
            parts, depth, cur = [], 0, ''
            for ch in params:
                if ch == '(': depth += 1
                if ch == ')': depth -= 1
                if ch == ',' and depth == 0:
                    parts.append(cur); cur = ''
                else:
                    cur += ch
            parts.append(cur)
            nbytes = 0
            for p in parts:
                p = ' '.join(p.split())
                if not p:
                    continue
                if '*' in p or '[' in p:
                    nbytes += 4
                elif any(t in p.split() or p.startswith(t) for t in EIGHT):
                    nbytes += 8
                else:
                    nbytes += 4
            methods.append((name, nbytes))
        if methods:
            out[iface] = methods
    return out

def main():
    tables = {}
    for f in sorted(os.listdir(INC)):
        if f.startswith("dwrite") and f.endswith(".h"):
            tables.update(parse(os.path.join(INC, f)))
    want = sys.argv[1:] or ["IDWriteFactory", "IDWriteFontFace", "IDWriteFontCollection",
                            "IDWriteFontFamily", "IDWriteFont", "IDWriteFontFile"]
    for iface in want:
        ms = tables.get(iface)
        if not ms:
            print("  %s: not found" % iface); continue
        print("%s (%d slots)" % (iface, len(ms)))
        for i, (name, n) in enumerate(ms):
            print("   %2d  %-34s %2d bytes (this + %d args)" % (i, name, n, (n - 4) // 4))

main()
