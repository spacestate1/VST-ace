#!/usr/bin/env python3
"""Check the winstubs.h prototypes against the real Win32 stdcall arities.

On x86-64 a wrong argument count in a stub is silently harmless -- the caller
cleans the stack and surplus register args are ignored. On i386 stdcall the
callee pops, so every wrong count shifts the stack by 4 bytes per argument and
the guest eventually 'ret's into nothing.

mingw-w64's i686 import libraries decorate each stdcall export as _Name@bytes,
which is the authoritative byte count. Compare it with what our prototypes say.
"""
import glob, os, re, subprocess, sys

LIBDIR = "/usr/i686-w64-mingw32/lib"

def real_arities():
    out = {}
    for lib in glob.glob(os.path.join(LIBDIR, "lib*.a")):
        try:
            s = subprocess.run(["i686-w64-mingw32-nm", "--defined-only", lib],
                               capture_output=True, text=True).stdout
        except FileNotFoundError:
            s = subprocess.run(["nm", "--defined-only", lib],
                               capture_output=True, text=True).stdout
        for m in re.finditer(r'\b_?([A-Za-z_][A-Za-z0-9_]*)@(\d+)\b', s):
            out.setdefault(m.group(1), int(m.group(2)))
        # cdecl exports carry no @N; record them so we can flag convention
        for m in re.finditer(r'^\S+ T _([A-Za-z_][A-Za-z0-9_]*)$', s, re.M):
            out.setdefault(m.group(1), None)
    return out

# argument sizes as pushed on the i386 stack
EIGHT = ('uint64_t', 'int64_t', 'double', 'long long')
def argbytes(arglist):
    a = arglist.strip()
    if a in ('', 'void'):
        return 0
    n = 0
    depth = 0
    cur = ''
    parts = []
    for ch in a:                       # split on commas outside parens
        if ch == '(':  depth += 1
        if ch == ')':  depth -= 1
        if ch == ',' and depth == 0: parts.append(cur); cur = ''
        else: cur += ch
    parts.append(cur)
    for p in parts:
        p = p.strip()
        if '*' in p or '[' in p:  n += 4
        elif any(t in p for t in EIGHT): n += 8
        else: n += 4               # int32_t, uint32_t, int, float, size_t...
    return n

def sources(path, seen=None):
    """The file and every local header it includes.

    The stubs are spread across winstubs.h, win32gui.h and dwrite_shim.h, and
    checking only the file named on the command line silently skipped the whole
    window layer -- where fifteen prototypes had WPARAM and LPARAM declared as
    64-bit, so every message stub popped 8 bytes too many per argument on i386.
    Follow the includes so a layer cannot go unchecked again."""
    seen = seen if seen is not None else set()
    path = os.path.normpath(path)
    if path in seen or not os.path.exists(path):
        return []
    seen.add(path)
    out = [path]
    base = os.path.dirname(path)
    for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', open(path).read(), re.M):
        out += sources(os.path.join(base, m.group(1)), seen)
    return out


def our_stubs(path):
    src = "\n".join(open(p).read() for p in sources(path))
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    pat = re.compile(r'static\s+MS\s+[A-Za-z_][\w \t\*]*?\bst_(\w+)\s*\(([^)]*)\)')
    return [(m.group(1), argbytes(m.group(2))) for m in pat.finditer(src)]

def main():
    real = real_arities()
    bad = unknown = ok = cdecl = 0
    undecorated = []
    for name, mine in our_stubs(sys.argv[1] if len(sys.argv) > 1 else "peload/winstubs.h"):
        if name not in real:
            unknown += 1
            continue
        want = real[name]
        if want is None:
            # No @N anywhere in the import libraries. That means either a real
            # __cdecl export (wsprintfA and friends) or a compiler intrinsic
            # that mingw never emits an import thunk for (the Interlocked
            # family on i386). The two are indistinguishable here, so list it
            # for a human rather than calling it a failure.
            undecorated.append(name)
            cdecl += 1
        elif want != mine:
            print(f"  ARITY   {name:34} ours {mine:3}  real {want:3}"
                  f"   ({(want-mine)//4:+d} args)")
            bad += 1
        else:
            ok += 1
    if undecorated:
        print("  no @N in the import libs (real __cdecl, or an intrinsic with no"
              " import thunk):\n    " + ", ".join(sorted(undecorated)))
    print(f"\n{ok} match, {bad} wrong arity, {cdecl} undecorated, "
          f"{unknown} not in the import libs at all")
    return 1 if bad else 0

sys.exit(main())
