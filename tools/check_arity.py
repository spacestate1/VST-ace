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

# argument sizes as pushed on the i386 stack. W32POINT is there because a POINT
# is passed *by value* to WindowFromPoint and ChildWindowFromPoint -- two dwords
# on i386, and one register on x86-64, which is why it cannot be spelled as two
# int parameters instead. The pointer test above this runs first, so a
# W32POINT * still counts as four.
EIGHT = ('uint64_t', 'int64_t', 'double', 'long long', 'W32POINT')
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

def our_cdecl_stubs(path):
    """The stubs declared MSCRT -- __cdecl at i386, where MS is stdcall.

    A stub carries no arity to compare, but the choice between the two macros
    is itself checkable: if the import libraries decorate the name @N, the real
    export is stdcall and MSCRT is wrong. That is not a cosmetic difference at
    i386 -- a cdecl stub returns without popping, so the caller is left N bytes
    low on every call. wvsprintfA and wvsprintfW were declared MSCRT next to
    wsprintfA/W, which genuinely are __cdecl (WINAPIV) because they are
    variadic; the va_list forms are not, and are ordinary WINAPI.
    """
    src = "\n".join(open(p).read() for p in sources(path))
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    pat = re.compile(r'static\s+MSCRT\s+[\w \t\*]*?\bst_(\w+)\s*\(')
    return [m.group(1) for m in pat.finditer(src)]


# Calls that run the other way -- host into guest -- are declared as function
# pointer typedefs, not as st_ prototypes, so the stub loop below never sees
# them. There is no import-library arity to check them against either: their
# shape comes from the Windows API contract rather than from an export. What
# can be checked is the convention winstubs32.h states for this whole tree --
# every Windows type that changes width is spelled size_t/intptr_t and never a
# fixed 64-bit type -- because the cost of getting it wrong is the same as a
# wrong stub arity, and lands in the same place. An LPARAM declared int64_t
# pushes 8 bytes into a guest callback that pops 4, so the stack drifts four
# bytes per call: EnumDisplayMonitors and the EnumResourceNames callbacks both
# had exactly that.
FIXED64 = ('int64_t', 'uint64_t', 'long long')

def strip_x64_only(src):
    """Drop #if defined(__x86_64__) regions, nesting included.

    The width rule only bites at i386, so code that never compiles there is not
    a finding. mscxxeh.h's MSVC exception funclets are the case that matters:
    they take a genuine 64-bit frame pointer, in RDX, and the whole file is
    guarded out of the 32-bit build.
    """
    out, depth, skip_at = [], 0, None
    for line in src.splitlines(True):
        t = line.lstrip()
        if re.match(r'#\s*if', t):
            depth += 1
            if skip_at is None and re.match(
                    r'#\s*(ifdef\s+__x86_64__\b'
                    r'|if\s+defined\s*\(\s*__x86_64__\s*\)\s*$)', t):
                skip_at = depth
        if skip_at is None:
            out.append(line)
        if re.match(r'#\s*endif', t):
            if skip_at == depth:
                skip_at = None
            depth -= 1
    return "".join(out)


def width_exempt(path):
    """Typedefs whose 64-bit parameter is 64-bit on both architectures.

    The width rule assumes a fixed 64-bit parameter is a pointer or handle
    written with the wrong type, which is the usual mistake. A few interfaces
    really do take a 64-bit value at i386 too -- IStream::Seek's LARGE_INTEGER
    is the one here -- so the declaration says so with a WIDTH-OK marker and
    this reads the markers back out.
    """
    names = set()
    pat = re.compile(r'WIDTH-OK:\s*([\w, ]+)')
    for f in sources(path):
        for m in pat.finditer(open(f).read()):
            names.update(n.strip() for n in m.group(1).split(',') if n.strip())
    return names

def callback_typedefs(path):
    src = "\n".join(open(p).read() for p in sources(path))
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = strip_x64_only(src)
    pat = re.compile(r'typedef\s+MS\s+[\w \t\*]*?\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)')
    exempt = width_exempt(path)
    return [(m.group(1), m.group(2)) for m in pat.finditer(src)
            if m.group(1) not in exempt]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "peload/winstubs.h"
    real = real_arities()
    bad = unknown = ok = cdecl = 0
    undecorated = []
    for name, mine in our_stubs(path):
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
    miscalled = [n for n in our_cdecl_stubs(path) if real.get(n) is not None]
    for n in miscalled:
        print(f"  CONV    {n:34} declared MSCRT (__cdecl), but the import libs"
              f" decorate it @{real[n]} -- it is stdcall, so it needs MS")

    cbs = callback_typedefs(path)
    wide = [(n, a) for n, a in cbs if any(t in a for t in FIXED64)]
    for n, a in wide:
        print(f"  WIDTH   {n:34} fixed 64-bit parameter in a guest callback"
              f" -- use intptr_t/uintptr_t: ({' '.join(a.split())})")

    print(f"\n{ok} match, {bad} wrong arity, {cdecl} undecorated, "
          f"{unknown} not in the import libs at all")
    print(f"{len(cbs) - len(wide)}/{len(cbs)} guest callbacks pointer-width clean")
    return 1 if bad or wide or miscalled else 0

# Importable, so tools/regress.py can reuse sources() rather than re-deriving
# which headers the stub layer is spread across.
if __name__ == "__main__":
    sys.exit(main())
