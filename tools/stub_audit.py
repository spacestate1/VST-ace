#!/usr/bin/env python3
"""List the stubs that answer without doing anything, and say which kind.

Every fault worth chasing in this host so far has been the same shape: a stub
that is registered, returns successfully, and leaves the caller with nothing.
Not a missing import -- those announce themselves -- but an implemented one
whose body is `return 0`.

The three kinds, in the order they hurt:

  handle      the return value is a pointer or a handle and the body returns
              NULL. On Windows several of these can never be null, so nothing
              checks them: GetDesktopWindow and GetFocus were both this, and
              both crashed a plug-in at a fixed offset off address zero.

  out-handle  a pointer-to-pointer argument the body never writes, while the
              return says the call worked. The caller then calls through
              whatever was on its stack. CreateDXGIFactory and GdipCreateRegion
              were this, and both took a plug-in down a vtable that was never
              filled in.

  out-buffer  the same for a plain pointer, on a function whose name says it
              produces something -- Get, Query, Create, Enum and their like.
              _localtime64_s was this: a structure reported as filled and never
              touched. A pointer passed to Free, Close or Set is an input and is
              not counted.

  silent      returns the value that means success and does nothing else. Safe
              until a caller loops until the answer changes -- wcsftime
              returning 0 for "buffer too small" is an endless loop -- or until
              the missing effect is the whole point, as with a draw.

This is a static reading, so it over-reports: a stub that genuinely has nothing
to do is listed too, and the fourth column says whether the corpus has ever
reached it. Judgement stays with the reader; what this removes is the searching.

    python3 tools/stub_audit.py [--kind handle|out-handle|out-buffer|silent]
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
HEADERS = ["peload/winstubs.h", "peload/win32gui.h", "peload/gdiplus_shim.h",
           "peload/d3d_shim.h", "peload/dwrite_shim.h", "peload/msvcp_shim.h"]

POINTER_RET = re.compile(r"\b(void|uint16_t|char|int32_t)\s*\*\s*$")


def read(path):
    return open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read()


def registered():
    """The (dll, name) pairs in g_stubs."""
    src = read("peload/winstubs.h")
    tbl = src[src.index("static const winstub g_stubs[]"):]
    tbl = tbl[:tbl.index("\n};")]
    out = {}
    for dll, name in re.findall(r'S\(\s*"([^"]+)"\s*,\s*([A-Za-z0-9_]+)\s*\)', tbl):
        out[name] = dll
    for dll, name in re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"', tbl):
        out.setdefault(name, dll)
    return out


def definitions():
    """name -> (return type, parameter list, body) for every st_ function."""
    defs = {}
    sig = re.compile(
        r"^static\s+(?:MS|MSCRT|MSTHIS)?\s*([A-Za-z_][A-Za-z0-9_ ]*?[ *])"
        r"st_([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*\{", re.M)
    for h in HEADERS:
        if not os.path.exists(os.path.join(ROOT, h)):
            continue
        src = read(h)
        for m in sig.finditer(src):
            ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
            # the body, to the matching brace
            i, depth = m.end() - 1, 0
            while i < len(src):
                if src[i] == "{":
                    depth += 1
                elif src[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            defs[name] = (ret, params, src[m.end():i])
    return defs


def classify(name, ret, params, body):
    name_of = (name,)
    """What kind of nothing this does, or None if it does something."""
    stripped = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    stripped = re.sub(r"\(void\)\s*[A-Za-z0-9_]+\s*;", " ", stripped)
    stripped = " ".join(stripped.split())

    trivial = re.fullmatch(r"(return\s+(0|NULL|1|-1|S_OK)\s*;)?", stripped) is not None
    if not trivial:
        # a body that only ever returns a constant still counts
        if not re.fullmatch(r"return\s+\(?[A-Za-z_0-9 ()]*\)?\s*(0|NULL|1)\s*;", stripped):
            return None

    if ret.rstrip().endswith("*"):
        return "handle"

    # A pointer argument the body never mentions except to discard it. Whether
    # that matters depends on which way the pointer points, and the name of the
    # call is the only evidence available here: Get and Create produce, Free and
    # Close consume. A pointer-to-pointer is an output either way -- there is
    # nothing else it could be.
    produces = re.match(r"(Get|Query|Create|Enum|Open|Read|Alloc|Load|Make|Find|"
                        r"Retrieve|Init|Lookup|Convert|Format|Copy|Duplicate|To|"
                        r"_localtime|_gmtime|_ftime|_stat|_dupenv)", name_of[0])
    for p in params.split(","):
        p = p.strip()
        if "*" not in p or p.startswith("const "):
            continue
        stars = p.count("*")
        pname = p.split("*")[-1].strip().strip("[]")
        if not pname or not pname.isidentifier():
            continue
        if re.search(r"\b%s\b" % re.escape(pname), body.replace("(void)%s" % pname, "")):
            continue
        if stars >= 2:
            return "out-handle"
        if produces:
            return "out-buffer"
    return "silent"


def main(argv):
    want = None
    for i, a in enumerate(argv):
        if a == "--kind" and i + 1 < len(argv):
            want = argv[i + 1]

    reg, defs = registered(), definitions()
    rows = []
    for name, dll in sorted(reg.items()):
        if name not in defs:
            continue
        ret, params, body = defs[name]
        kind = classify(name, ret, params, body)
        if kind and (want is None or kind == want):
            rows.append((kind, dll, name, ret.strip()))

    order = {"handle": 0, "out-handle": 1, "out-buffer": 2, "silent": 3}
    rows.sort(key=lambda r: (order[r[0]], r[1], r[2]))
    print("%-10s %-22s %-34s %s" % ("kind", "library", "export", "returns"))
    print("-" * 88)
    for kind, dll, name, ret in rows:
        print("%-10s %-22s %-34s %s" % (kind, dll, name, ret))
    print("-" * 88)
    for k in ("handle", "out-handle", "out-buffer", "silent"):
        n = sum(1 for r in rows if r[0] == k)
        if n:
            print("%d %s" % (n, k))
    print("%d of %d registered stubs answer without doing anything"
          % (len(rows), len(reg)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
