#!/usr/bin/env python3
"""Everything in this tree that can be checked without a plug-in corpus.

The rendering tests need plug-in binaries, which are not ours to ship and are
not here -- packaging/debian/rules says as much where it skips dh_auto_test.
What is left is still worth guarding, and it is where the expensive bugs have
actually been:

  * the i386 ABI surface, where a wrong calling convention or a Windows type
    declared at the wrong width corrupts a guest's stack silently, only at
    32-bit, and shows up later as "that plug-in crashes" rather than as
    anything pointing back here;
  * the packaging recipes, where a list that has drifted out of step fails the
    build on someone else's machine instead of on ours.

Every check below exists because something in one of those two families
actually broke. Each one names what it would have caught.

    python3 tools/regress.py [--no-build] [--keep] [--list] [--only a,b]

--no-build reuses peload/build instead of configuring a fresh tree, which is
much faster but only checks what that tree happens to have been built from.
--keep leaves the temporary build directories behind for inspection. --only
runs just the named checks, which is how a single one is exercised against a
deliberately broken tree to confirm it still catches what it claims to.

A check that needs something this machine does not have -- a -m32 toolchain,
mingw-w64's i686 import libraries, desktop-file-utils -- reports SKIP and says
why, rather than passing quietly. Exit status is the number of failures.
"""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from check_arity import sources          # noqa: E402  (needs sys.path first)

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def rel(*p):
    return os.path.join(ROOT, *p)


def stripped_sources(path):
    """The stub layer's sources, comments removed.

    Comments matter here: they quote the very identifiers these checks look
    for, so leaving them in makes an unregistered stub look registered.
    """
    src = "\n".join(open(p, encoding="utf-8", errors="replace").read()
                    for p in sources(path))
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def stub_table(src):
    """The body of g_stubs[], brace-matched.

    Slicing to the next "};" is not enough -- the table's own entries are
    braced -- and slicing to end of file pulls in later code that mentions
    st_ names for other reasons.
    """
    i = src.index("static const winstub g_stubs[]")
    j = src.index("{", i)
    depth, k = 0, j
    while True:
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
        k += 1


# --------------------------------------------------------------- ABI surface

def check_stub_wiring(ctx):
    """A stub that is defined but never wired up resolves to the generic stub.

    Would have caught: CharNextA, defined here for a long time and never
    registered, so it and its three relatives all returned 0 -- a null pointer
    the caller dereferences on the next instruction. Reached three ways, so all
    three count as wired: the S() macro, which hides the st_ prefix; an
    explicit (void *)st_name in a table entry; and a reference by address from
    elsewhere, which is how the DirectWrite factory and the C++ exception
    vftable get theirs.
    """
    src = stripped_sources(rel("peload", "winstubs.h"))
    tbl = stub_table(src)
    defined = set(re.findall(
        r"static\s+(?:MS|MSCRT)\s+[\w \t\*]*?\bst_(\w+)\s*\(", src))
    by_macro = set(re.findall(r'S\("[^"]+",\s*(\w+)\s*\)', tbl))

    orphans = []
    for name in sorted(defined):
        if name in by_macro:
            continue
        if len(re.findall(r"\bst_%s\b" % re.escape(name), src)) > 1:
            continue                      # referenced somewhere besides itself
        orphans.append(name)
    if orphans:
        return FAIL, "defined but never reachable: " + ", ".join(orphans)
    return PASS, "%d stubs, all reachable" % len(defined)


def check_arity_source(ctx):
    """Prototypes against the real stdcall arities, and the two conventions.

    Would have caught: EnumDisplayMonitors popping 20 bytes where the API takes
    16; wvsprintfA/W declared MSCRT when the import libraries decorate them @12;
    and LPARAM spelled int64_t in the guest-callback typedefs.
    """
    if not os.path.isdir("/usr/i686-w64-mingw32/lib"):
        return SKIP, "no mingw-w64 i686 import libraries to compare against"
    r = run([sys.executable, rel("tools", "check_arity.py"),
             rel("peload", "winstubs.h")])
    tail = [l for l in r.stdout.splitlines() if l.strip()][-2:]
    if r.returncode != 0:
        bad = [l for l in r.stdout.splitlines()
               if l.lstrip().startswith(("ARITY", "CONV", "WIDTH"))]
        return FAIL, "; ".join(l.split(None, 1)[-1].strip() for l in bad)
    return PASS, " / ".join(t.strip() for t in tail)


def check_arity_binary(ctx):
    """The same question asked of the compiled code rather than the source.

    The source check reads prototypes; this reads what the compiler actually
    emitted, so it also covers the macro expanding to the convention we meant.
    A stdcall callee pops its arguments, so the ret form is the arity.
    """
    if not ctx.get("peload32"):
        return SKIP, "no peload32 built"
    # A binary older than the stubs it was built from answers about the past.
    # That is the ordinary state of peload/build under --no-build, and
    # reporting it as a failure would point at source that is already fixed.
    newest = max(os.path.getmtime(p) for p in sources(rel("peload", "winstubs.h")))
    if newest > os.path.getmtime(ctx["peload32"]):
        return SKIP, ("peload32 predates the stub sources -- rebuild it, or "
                      "drop --no-build, before trusting this")
    src = stripped_sources(rel("peload", "winstubs.h"))
    tbl = stub_table(src)
    registered = set(re.findall(r'S\("[^"]+",\s*(\w+)\s*\)', tbl))

    arity = {}
    for name, n in re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*\}',
                              open(rel("peload", "win32_arity.h"),
                                   encoding="utf-8", errors="replace").read()):
        if not name.startswith("_imp__"):
            arity.setdefault(name, int(n))

    dis = run(["objdump", "-d", ctx["peload32"]]).stdout
    cur, rets = None, {}
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = m.group(1)
            continue
        if cur and re.search(r"\bret\b", line):
            m2 = re.search(r"ret\s+\$0x([0-9a-f]+)", line)
            rets.setdefault(cur, set()).add(
                int(m2.group(1), 16) if m2 else 0)

    # Exports that do not exist in 32-bit kernel32/user32, or that mingw
    # supplies as an intrinsic with no import thunk, so the table is silent
    # about them and a stdcall stub is still right.
    no_entry_ok = {n for n in registered if n.startswith("Interlocked")} | {
        "RtlLookupFunctionEntry", "RtlVirtualUnwind", "RtlAddFunctionTable",
        "GetWindowLongPtrA", "GetWindowLongPtrW",
        "SetWindowLongPtrA", "SetWindowLongPtrW"}

    bad, checked = [], 0
    for name in sorted(registered):
        got = rets.get("st_" + name)
        if not got or len(got) != 1:
            continue
        got, want = next(iter(got)), arity.get(name)
        if want is None:
            if got != 0 and name not in no_entry_ok:
                bad.append("%s pops %d but has no @N" % (name, got))
        elif got != want:
            bad.append("%s pops %d, table says %d" % (name, got, want))
        else:
            checked += 1
    if bad:
        return FAIL, "; ".join(bad)
    return PASS, "%d compiled stubs match win32_arity.h" % checked


# ------------------------------------------------------------------- the build

def check_build(ctx):
    """peload32 and the 64-bit hosts still compile, at both widths.

    The 32-bit build is the one that matters most and is the one a developer is
    least likely to have exercised: winstubs.h is shared, so a change made for
    x86-64 lands in peload32 too.
    """
    if ctx["no_build"]:
        b = rel("peload", "build")
        if not os.path.isdir(b):
            return SKIP, "--no-build given but peload/build does not exist"
        ctx["build"] = b
        p32 = os.path.join(b, "peload32")
        ctx["peload32"] = p32 if os.path.exists(p32) else None
        return SKIP, "--no-build: reusing %s" % os.path.relpath(b, ROOT)

    b = os.path.join(ctx["tmp"], "build")
    r = run(["cmake", "-S", rel("peload"), "-B", b, "-DCMAKE_BUILD_TYPE=Release"])
    if r.returncode != 0:
        return FAIL, "configure failed: " + r.stderr.strip().splitlines()[-1]
    ctx["build"] = b
    ctx["cmake_log"] = r.stdout

    targets = ["peload", "peserve"]
    if "peload32 skipped" not in r.stdout:
        targets.append("peload32")
    r = run(["cmake", "--build", b, "-j", str(os.cpu_count() or 4),
             "--target"] + targets)
    if r.returncode != 0:
        tail = (r.stderr or r.stdout).strip().splitlines()[-3:]
        return FAIL, "build failed: " + " | ".join(tail)

    p32 = os.path.join(b, "peload32")
    ctx["peload32"] = p32 if os.path.exists(p32) else None
    if not ctx["peload32"]:
        return PASS, "64-bit built; peload32 skipped (no 32-bit toolchain here)"
    return PASS, "built " + ", ".join(targets)


def check_no_runpath(ctx):
    """peload32 must carry no RUNPATH.

    The 32-bit .pc files answer with -L/usr/lib32 or -L/usr/lib/i386-linux-gnu,
    and CMake writes any link directory it does not recognise as a system one
    into the binary. Both are already in ld.so's own search path, so the entry
    buys nothing -- and lintian rejects the .deb over it.
    """
    if not ctx.get("peload32"):
        return SKIP, "no peload32 built"
    d = run(["readelf", "-d", ctx["peload32"]]).stdout
    hits = [l.strip() for l in d.splitlines()
            if "RUNPATH" in l or "RPATH" in l]
    if hits:
        return FAIL, "; ".join(hits)
    return PASS, "no RUNPATH or RPATH"


def check_peload32_complete(ctx):
    """A peload32 with its editor window and its audio path compiled in.

    Both are optional in peload/CMakeLists.txt, so a machine missing 32-bit X11
    or PipeWire still produces a binary -- just a lesser one. The .deb asserts
    this too, in packaging/debian/rules, where the i386 Build-Depends make it a
    real requirement rather than a preference.
    """
    if not ctx.get("peload32"):
        return SKIP, "no peload32 built"
    needed = run(["readelf", "-d", ctx["peload32"]]).stdout
    missing = [lib for lib in ("libX11", "libpipewire") if lib not in needed]
    if missing:
        return SKIP, "render-only build here (no 32-bit %s)" % ", ".join(missing)
    return PASS, "editor window and audio path both linked"


def check_cmake_skip_path(ctx):
    """No 32-bit FreeType must skip peload32, not fail the whole configure.

    Would have caught: the FATAL_ERROR that used to be here. A machine with a
    -m32 toolchain and no 32-bit FreeType -- a Fedora box with glibc-devel.i686
    pulled in by something else is the ordinary way to get one -- could not
    configure pestudio or the 64-bit loader either.
    """
    if ctx["no_build"]:
        return SKIP, "--no-build"
    shim_dir = os.path.join(ctx["tmp"], "shim")
    os.makedirs(shim_dir, exist_ok=True)
    shim = os.path.join(shim_dir, "pkg-config")
    real = shutil.which("pkg-config")
    if not real:
        return SKIP, "no pkg-config on this machine"
    with open(shim, "w") as f:
        f.write("#!/bin/sh\n"
                "# Pretend this machine carries no 32-bit .pc files at all.\n"
                "case \"$PKG_CONFIG_LIBDIR\" in\n"
                "  /usr/lib32/pkgconfig|/usr/lib/i386-linux-gnu/pkgconfig)"
                " exit 1 ;;\n"
                "esac\n"
                "exec %s \"$@\"\n" % real)
    os.chmod(shim, 0o755)

    env = dict(os.environ, PATH=shim_dir + os.pathsep + os.environ["PATH"])
    b = os.path.join(ctx["tmp"], "build-skip")
    r = run(["cmake", "-S", rel("peload"), "-B", b,
             "-DCMAKE_BUILD_TYPE=Release"], env=env)
    if r.returncode != 0:
        return FAIL, "configure failed instead of skipping peload32"
    if "peload32 skipped" not in r.stdout:
        return FAIL, "no skip message; peload32 was configured anyway"
    if r.stdout.count("peload32 skipped") != 1:
        return FAIL, "more than one skip reason printed"
    helps = run(["cmake", "--build", b, "--target", "help"]).stdout
    if re.search(r"^\.\.\. peload32$", helps, re.M):
        return FAIL, "peload32 target exists despite the skip"
    if not re.search(r"^\.\.\. arity$", helps, re.M):
        return FAIL, "the arity target went with it; it needs no 32-bit toolchain"
    return PASS, "skipped cleanly, one reason, arity still reachable"


def check_pkgconfig32_env(ctx):
    """pkgconfig32() must put PKG_CONFIG_LIBDIR back the way it found it.

    It is process-wide, not scoped to the function. Restoring an unset variable
    as empty is not the same thing: pkg-config reads an empty PKG_CONFIG_LIBDIR
    as a search path with nothing in it, which fails every later query in the
    tree rather than falling back to the default.
    """
    cml = open(rel("peload", "CMakeLists.txt"), encoding="utf-8").read()
    i = cml.index("function(pkgconfig32 prefix)")
    fn = cml[i:cml.index("endfunction()", i) + len("endfunction()")]
    d = os.path.join(ctx["tmp"], "pkgfn")
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "fn.cmake"), "w").write(fn + "\n")
    open(os.path.join(d, "t.cmake"), "w").write("""
set(PKG_CONFIG_EXECUTABLE pkg-config)
include(${CMAKE_CURRENT_LIST_DIR}/fn.cmake)
pkgconfig32(A freetype2)
if(DEFINED ENV{PKG_CONFIG_LIBDIR})
  message(FATAL_ERROR "leaked PKG_CONFIG_LIBDIR after a successful query")
endif()
pkgconfig32(B no-such-module-xyz)
if(B_OK EQUAL 0)
  message(FATAL_ERROR "a missing module reported success")
endif()
if(DEFINED ENV{PKG_CONFIG_LIBDIR})
  message(FATAL_ERROR "leaked PKG_CONFIG_LIBDIR after a failed query")
endif()
execute_process(COMMAND pkg-config --modversion freetype2
                OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "a later 64-bit query was poisoned")
endif()
set(ENV{PKG_CONFIG_LIBDIR} "/sentinel")
pkgconfig32(C freetype2)
if(NOT "$ENV{PKG_CONFIG_LIBDIR}" STREQUAL "/sentinel")
  message(FATAL_ERROR "did not restore a pre-set value")
endif()
message(STATUS "OK")
""")
    r = run(["cmake", "-P", os.path.join(d, "t.cmake")])
    if r.returncode != 0:
        last = (r.stderr or r.stdout).strip().splitlines()
        return FAIL, last[-1] if last else "cmake -P failed"
    return PASS, "restores unset, restores a set value, survives a miss"


# --------------------------------------------------------------- the packaging

def check_shell_syntax(ctx):
    """The packaging scripts still parse."""
    bad = []
    for name in sorted(os.listdir(rel("packaging"))):
        if not name.endswith(".sh"):
            continue
        r = run(["bash", "-n", rel("packaging", name)])
        if r.returncode != 0:
            bad.append("%s: %s" % (name, r.stderr.strip().splitlines()[-1]))
    if bad:
        return FAIL, "; ".join(bad)
    return PASS, "all packaging/*.sh parse"


def apt_i386_from_install_deps():
    s = open(rel("packaging", "install-deps.sh"), encoding="utf-8").read()
    i = s.index("    I386=(gcc-multilib")
    return set(s[i:s.index(")", i)].split("=(", 1)[1].split())


def i386_from_control():
    s = open(rel("packaging", "debian", "control"), encoding="utf-8").read()
    bd = s[s.index("Build-Depends:"):s.index("Standards-Version:")]
    bd = "\n".join(l for l in bd.splitlines() if not l.lstrip().startswith("#"))
    got = {p.strip().rstrip(",") for p in re.findall(r"^\s+(\S+),?\s*$", bd, re.M)}
    return {p for p in got if p.endswith(":i386") or "multilib" in p}


def check_i386_deps_match(ctx):
    """install-deps.sh --i386 must satisfy debian/control's Build-Depends.

    Would have caught: libpipewire-0.3-dev:i386 added to Build-Depends for
    peload32 and never added to the installer, so the documented
    install-deps.sh then build-deb.sh route stopped at dpkg-checkbuilddeps.
    """
    script, control = apt_i386_from_install_deps(), i386_from_control()
    missing, extra = control - script, script - control
    if missing or extra:
        parts = []
        if missing:
            parts.append("install-deps.sh is missing " + ", ".join(sorted(missing)))
        if extra:
            parts.append("install-deps.sh has extra " + ", ".join(sorted(extra)))
        return FAIL, "; ".join(parts)
    return PASS, "%d i386 build-dependencies, both lists agree" % len(control)


# debian/ entries that are recipe files rather than a package staging directory
DEBIAN_NOT_A_PACKAGE = {
    "rules", "control", "changelog", "copyright", "compat", "docs",
    "source", "tmp", "files", "watch", "install", "clean",
}


def check_rules_staging_dirs(ctx):
    """Every debian/<name> that rules stages into must be a declared package.

    Would have caught: DESTDIR_I386 := $(CURDIR)/debian/vst-ace-i386, which
    debhelper never turned into anything because no such Package: stanza exists
    -- so the install into it was silently dead work, and its comment claimed
    the opposite of the note in debian/control.
    """
    rules = open(rel("packaging", "debian", "rules"), encoding="utf-8").read()
    control = open(rel("packaging", "debian", "control"), encoding="utf-8").read()
    declared = set(re.findall(r"^Package:\s*(\S+)", control, re.M))

    referenced = set()
    for m in re.finditer(r"debian/([A-Za-z0-9][A-Za-z0-9.+-]*)", rules):
        name = m.group(1)
        if name in DEBIAN_NOT_A_PACKAGE or "." in name:
            continue
        referenced.add(name)
    stray = sorted(referenced - declared)
    if stray:
        return FAIL, ("staged into but not declared in control: "
                      + ", ".join(stray))
    return PASS, "staging dirs match the %d declared package(s)" % len(declared)


def check_rpm_spec(ctx):
    """The spec has its required tags, and its macros do what they claim.

    Would have caught: _dwz_low_mem_die_limit set to 0 under a comment saying
    debuginfo extraction was being relaxed. That tunable only moves dwz's
    low-memory threshold; it never skipped the pass, so the comment described
    something the spec was not doing.
    """
    path = rel("packaging", "rpm", "vst-ace.spec")
    if not os.path.exists(path):
        return SKIP, "no packaging/rpm/vst-ace.spec"
    s = open(path, encoding="utf-8").read()
    problems = []

    for tag in ("Name:", "Version:", "Release:", "Summary:", "License:",
                "URL:", "Source0:", "%description", "%prep", "%build",
                "%install", "%files", "%changelog"):
        if not re.search(r"^%s" % re.escape(tag), s, re.M):
            problems.append("no %s" % tag.rstrip(":"))

    # rpm expands macros inside comments, so a single % in one is a live
    # macro reference and has to be doubled.
    for n, line in enumerate(s.splitlines(), 1):
        if line.lstrip().startswith("#") and "%" in line.replace("%%", ""):
            problems.append("unescaped %% in the comment on line %d" % n)

    # Definitions only -- the comment above the replacement names the old
    # macro to explain why it went, and that is not a use of it.
    if re.search(r"^%(?:global|define)\s+_dwz_low_mem_die_limit", s, re.M):
        problems.append("_dwz_low_mem_die_limit only moves dwz's threshold; "
                        "use _find_debuginfo_dwz_opts to skip the pass")
    if not re.search(r"^%(?:global|define)\s+_find_debuginfo_dwz_opts", s, re.M):
        problems.append("no _find_debuginfo_dwz_opts")
    if not re.search(r"^%global\s+_lto_cflags\s+%\{nil\}", s, re.M):
        problems.append("LTO not disabled; the asm-only entry points get "
                        "dropped and peserve fails to link")

    if problems:
        return FAIL, "; ".join(problems)
    return PASS, "tags, comment escaping and build macros all present"


def check_desktop_files(ctx):
    """The .desktop files both packages install must validate.

    The spec runs this in %check, so a broken one fails the rpm build rather
    than shipping; the .deb has no equivalent gate.
    """
    if not shutil.which("desktop-file-validate"):
        return SKIP, "desktop-file-utils not installed"
    bad = []
    for name in ("pestudio.desktop", "dwstudio.desktop"):
        p = rel("packaging", name)
        if not os.path.exists(p):
            bad.append("%s missing" % name)
            continue
        r = run(["desktop-file-validate", p])
        if r.returncode != 0:
            bad.append("%s: %s" % (name, r.stdout.strip().splitlines()[0]))
    if bad:
        return FAIL, "; ".join(bad)
    return PASS, "both .desktop files validate"


def check_man_pages(ctx):
    """Every program the packages put on $PATH has a man page beside it.

    Both recipes install the same four by name, so a fifth program added to one
    of them without a page is a lintian complaint waiting to happen.
    """
    rules = open(rel("packaging", "debian", "rules"), encoding="utf-8").read()
    spec_path = rel("packaging", "rpm", "vst-ace.spec")
    spec = open(spec_path, encoding="utf-8").read() if os.path.exists(spec_path) else ""
    wanted = set(re.findall(r"for m in ([a-z0-9 ]+); do", rules + spec))
    names = sorted({n for group in wanted for n in group.split()})
    if not names:
        return SKIP, "no man page loop found in either recipe"
    missing = [n for n in names if not os.path.exists(rel("packaging", n + ".1"))]
    if missing:
        return FAIL, "no page for " + ", ".join(missing)
    return PASS, "%d man pages present" % len(names)


CHECKS = [
    ("stub-wiring",        check_stub_wiring),
    ("arity-source",       check_arity_source),
    ("build",              check_build),
    ("arity-binary",       check_arity_binary),
    ("no-runpath",         check_no_runpath),
    ("peload32-complete",  check_peload32_complete),
    ("cmake-skip-path",    check_cmake_skip_path),
    ("pkgconfig32-env",    check_pkgconfig32_env),
    ("shell-syntax",       check_shell_syntax),
    ("i386-deps-match",    check_i386_deps_match),
    ("rules-staging-dirs", check_rules_staging_dirs),
    ("rpm-spec",           check_rpm_spec),
    ("desktop-files",      check_desktop_files),
    ("man-pages",          check_man_pages),
]


def main(argv):
    if "--list" in argv:
        for name, fn in CHECKS:
            print("%-20s %s" % (name, (fn.__doc__ or "").strip().split("\n")[0]))
        return 0

    checks = CHECKS
    for i, a in enumerate(argv):
        if a == "--only" and i + 1 < len(argv):
            want = set(argv[i + 1].split(","))
            unknown = want - {n for n, _ in CHECKS}
            if unknown:
                print("no such check: %s" % ", ".join(sorted(unknown)),
                      file=sys.stderr)
                return 1
            checks = [(n, f) for n, f in CHECKS if n in want]

    ctx = {"no_build": "--no-build" in argv,
           "tmp": tempfile.mkdtemp(prefix="vst-ace-regress-")}
    keep = "--keep" in argv

    print("regression checks, no plug-in corpus needed")
    print("=" * 72)
    results = []
    try:
        for name, fn in checks:
            try:
                status, detail = fn(ctx)
            except Exception as e:                       # a broken check is a
                status, detail = FAIL, "%s: %s" % (type(e).__name__, e)
            results.append((name, status, detail))
            print("%-4s %-20s %s" % (status, name, detail), flush=True)
    finally:
        if keep:
            print("\ntemporary build tree left at %s" % ctx["tmp"])
        else:
            shutil.rmtree(ctx["tmp"], ignore_errors=True)

    print("=" * 72)
    n = {s: sum(1 for _, st, _ in results if st == s) for s in (PASS, FAIL, SKIP)}
    print("%d passed, %d failed, %d skipped" % (n[PASS], n[FAIL], n[SKIP]))
    return n[FAIL]


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
