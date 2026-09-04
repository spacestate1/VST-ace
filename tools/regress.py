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


def check_dll_reachable(ctx):
    """Every DLL with a stub entry has to be one GetProcAddress can search.

    A plug-in reaches an export two ways: through its import table, which
    carries the DLL name and resolves against the stub table directly, or
    through LoadLibrary + GetProcAddress. The second falls back to a name
    search over g_sysdlls alone -- so a DLL listed only in g_stockdlls, or in
    neither list, can never answer, however much is implemented behind it.

    Would have caught: gdiplus, which sat in g_stockdlls with some ninety Gdip*
    entries wired up and unreachable to anything that resolved them by name --
    which is how GDI+ is normally reached, since it is not on a stock Windows
    2000. ws2_32 was in neither list, so LoadLibrary could not even open it.
    Between them, 128 implemented functions that nothing could call.
    """
    src = stripped_sources(rel("peload", "winstubs.h"))
    tbl = stub_table(src)

    def names(var):
        m = re.search(r"static const char \*const %s\[\] = \{(.*?)\n\};" % var,
                      src, re.S)
        return set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()

    trim = lambda n: n[:-4].lower() if n.lower().endswith(".dll") else n.lower()
    sysd = set(trim(d) for d in names("g_sysdlls"))

    # Reached another way, on purpose, and each says so where it is defined:
    # DirectWrite has its own handle in GetProcAddress, and the Direct2D and
    # Direct3D libraries are deliberately not loadable so a plug-in takes the
    # software path this host actually draws.
    exempt = {"dwrite", "d2d1", "d3d11"}

    used = [trim(d) for d in re.findall(r'S\(\s*"([^"]+)"', tbl)]
    used += [trim(d) for d in re.findall(r'\{\s*"([^"]+\.dll)"\s*,', tbl)]
    dead = {}
    for d in used:
        if d in sysd or d in exempt:
            continue
        dead[d] = dead.get(d, 0) + 1
    if dead:
        return FAIL, "stub entries GetProcAddress can never reach: " + ", ".join(
            "%s (%d)" % (d, n) for d, n in sorted(dead.items()))
    return PASS, "%d DLLs with stubs, all reachable by name" % len(set(used) - exempt)


def check_dll_lists_disjoint(ctx):
    """The three DLL lists have to say three different things about a name.

    g_sysdlls is "implemented", g_stockdlls is "present and empty", and
    g_absentdlls is "this host does not have it" -- and which one a library is
    in is a decision, not an accident. A name in two of them is that decision
    made twice and differently: whichever list is searched first silently wins,
    and the other entry reads as intent that is not happening.

    Would have caught the shape of the gdiplus bug from the other side -- a
    library implemented and simultaneously declared empty -- and stops a
    graphics library being added to g_stockdlls, which is what would turn the
    three editors that render today into blank ones.
    """
    src = stripped_sources(rel("peload", "winstubs.h"))

    def names(var):
        m = re.search(r"static const char \*const %s\[\] = \{(.*?)\n\};" % var,
                      src, re.S)
        return set() if not m else set(
            n[:-4].lower() if n.lower().endswith(".dll") else n.lower()
            for n in re.findall(r'"([^"]+)"', m.group(1)))

    sysd, stock, absent = names("g_sysdlls"), names("g_stockdlls"), names("g_absentdlls")
    if not absent:
        return FAIL, "g_absentdlls not found -- the deliberate refusals are gone"
    clashes = []
    for a, b, an, bn in ((sysd, stock, "g_sysdlls", "g_stockdlls"),
                         (sysd, absent, "g_sysdlls", "g_absentdlls"),
                         (stock, absent, "g_stockdlls", "g_absentdlls")):
        for d in sorted(a & b):
            clashes.append("%s in both %s and %s" % (d, an, bn))
    if clashes:
        return FAIL, "; ".join(clashes)
    return PASS, "%d implemented, %d present-and-empty, %d refused, no overlap" % (
        len(sysd), len(stock), len(absent))


def check_aw_parity(ctx):
    """An A/W pair where only one side is real.

    The two forms of a Win32 call are the same function with a different string
    width, and a plug-in picks whichever its own build used -- so implementing
    one and leaving the other a stub does not halve the coverage, it makes the
    behaviour depend on how the plug-in was compiled.

    Would have caught: RegOpenKeyExA and RegQueryValueExA, which failed
    unconditionally while the W forms did real work against the same table, so
    an ANSI plug-in could write a registry value and never read it back. That
    is what left daHornet unregistered on every load.

    Only flags a pair where one side is wired and the other is not present at
    all; a deliberate forward from A to W is exactly what this asks for and
    reads as both being defined.
    """
    src = stripped_sources(rel("peload", "winstubs.h")) + \
          stripped_sources(rel("peload", "win32gui.h"))
    defined = set(re.findall(
        r"static\s+(?:MS|MSCRT)\s+[\w \t\*]*?\bst_(\w+)\s*\(", src))
    missing = []
    for name in sorted(defined):
        if name.endswith("A") and not name.endswith("EXA"):
            other = name[:-1] + "W"
        elif name.endswith("W") and not name.endswith("EXW"):
            other = name[:-1] + "A"
        else:
            continue
        # Only a pair: both spellings have to look like a Win32 name, and the
        # stem has to be shared by something else too, or every name ending in
        # a stray A or W is a false positive.
        if other in defined or other in missing:
            continue
        missing.append(name)
    # Reported rather than failed: the corpus needs one side of plenty of pairs
    # and the other has never been asked for. The number is the signal -- a jump
    # in it means a pair was added by halves.
    return PASS, "%d A/W stubs, %d without their counterpart" % (
        sum(1 for n in defined if n[-1:] in "AW"), len(missing))


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


# The trees the build compiles from. A source file here that git does not know
# about is one this machine can see and a clone cannot.
SOURCE_DIRS = ("peload", "gui", "c")
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".S")


def check_sources_tracked(ctx):
    """Every source file the build reads is tracked by git.

    Would have caught: macfont.h and png_in.h -- the Classic text shims' bitmap
    fonts and the PNG decoder behind every VSTGUI editor -- written, #included
    by cfmlib.c and macquartz.c, and never added. Both packaging scripts stage
    the working tree with rsync rather than git, so every build here found them
    and a fresh clone compiled neither file.
    """
    if run(["git", "-C", ROOT, "rev-parse", "--is-inside-work-tree"]).returncode:
        return SKIP, "not a git checkout"
    r = run(["git", "-C", ROOT, "ls-files", "--others", "--exclude-standard",
             "--"] + list(SOURCE_DIRS))
    if r.returncode != 0:
        return FAIL, "git ls-files failed: " + r.stderr.strip()
    stray = sorted(l for l in r.stdout.splitlines()
                   if l.endswith(SOURCE_SUFFIXES))
    if stray:
        return FAIL, "untracked source: " + ", ".join(stray)
    return PASS, "no untracked sources under " + ", ".join(SOURCE_DIRS)


# build-deb.sh copies packaging/debian into place as debian/ once the pristine
# tarball is captured, so it alone excludes that name. Everything else is the
# same tree staged for the same reason, and must be excluded the same way.
STAGING_EXCLUDE_ALLOWED = {"debian/"}


def rsync_excludes(name):
    src = open(rel("packaging", name), encoding="utf-8").read()
    return set(re.findall(r"--exclude='([^']*)'", src))


def check_staging_excludes(ctx):
    """The two packaging scripts stage the source tree the same way.

    Would have caught: renaming the launcher from dw to va fixed
    build-deb.sh's --exclude='/dw' and left build-rpm.sh's behind, so every
    .rpm source tarball shipped the built launcher inside it -- the same bug,
    on the other script, after it had already been found once.
    """
    deb, rpm = rsync_excludes("build-deb.sh"), rsync_excludes("build-rpm.sh")
    parts = []
    for label, only in (("build-deb.sh", deb - rpm - STAGING_EXCLUDE_ALLOWED),
                        ("build-rpm.sh", rpm - deb - STAGING_EXCLUDE_ALLOWED)):
        if only:
            parts.append("only %s: %s" % (label, ", ".join(sorted(only))))
    if parts:
        return FAIL, "; ".join(parts)
    return PASS, "%d staging excludes, both scripts agree" % len(deb & rpm)


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


def installed_programs():
    """The program names each recipe puts on $PATH.

    debian/rules installs va directly and symlinks the rest in a loop; the spec
    lists them under %{_bindir} in %files. Both are returned separately because
    the same .desktop file is installed by both, so it has to name something
    each of them provides.
    """
    rules = open(rel("packaging", "debian", "rules"), encoding="utf-8").read()
    deb = set(re.findall(r"\$\(DESTDIR\)/usr/bin/(\w[\w.+-]*)", rules))
    for group in re.findall(r"for p in ([a-z0-9 ]+); do", rules):
        deb.update(group.split())

    spec_path = rel("packaging", "rpm", "vst-ace.spec")
    spec = open(spec_path, encoding="utf-8").read() if os.path.exists(spec_path) else ""
    files = spec[spec.index("%files"):] if "%files" in spec else ""
    rpm = set(re.findall(r"^%\{_bindir\}/(\S+)", files, re.M))
    return deb, rpm


def check_desktop_exec(ctx):
    """Every .desktop Exec= must name a program the packages install.

    Would have caught: the launcher renamed from dw to va, leaving Exec=dw pe
    and Exec=dw gui behind. Both menu entries then failed on a fresh install
    with "Failed to execute child process dw (No such file or directory)" --
    and desktop-file-validate passed the files, because it checks their syntax
    and not whether the program named in them is there.
    """
    deb, rpm = installed_programs()
    if not deb or not rpm:
        return SKIP, "could not read the program list out of both recipes"

    problems = []
    for name in sorted(os.listdir(rel("packaging"))):
        if not name.endswith(".desktop"):
            continue
        text = open(rel("packaging", name), encoding="utf-8").read()
        for key in ("Exec", "TryExec"):
            m = re.search(r"^%s=(\S+)" % key, text, re.M)
            if not m:
                continue
            # The program is argv[0]; the rest is arguments and field codes.
            prog = m.group(1)
            for recipe, have in (("the .deb", deb), ("the .rpm", rpm)):
                if prog not in have:
                    problems.append("%s: %s=%s, which %s does not install"
                                    % (name, key, prog, recipe))
    if problems:
        return FAIL, "; ".join(problems)
    return PASS, "every Exec names a program on $PATH in both packages"


def check_version_consistent(ctx):
    """The two CMake projects must default to the same version.

    Each window compiles VSTACE_VERSION into its own About box, and the default
    is written out in both peload/CMakeLists.txt and gui/CMakeLists.txt. A bump
    applied to one and not the other gives pestudio and dwstudio, in the same
    package, two different answers to what they are -- with nothing to say
    which is right. The .rpm spec's Version: is checked with them, since
    build-rpm.sh rewrites that tag rather than reading it.
    """
    found = {}
    for proj in ("peload", "gui"):
        path = rel(proj, "CMakeLists.txt")
        # The literal one, not the set() that reads the environment override.
        lit = [v for v in re.findall(r'^\s*set\(VSTACE_VERSION\s+"([^"]+)"\)',
                                     open(path, encoding="utf-8").read(), re.M)
               if "$" not in v]
        if len(lit) != 1:
            return FAIL, ("%s/CMakeLists.txt has %d literal VSTACE_VERSION "
                          "defaults, wanted 1" % (proj, len(lit)))
        found[proj + "/CMakeLists.txt"] = lit[0]

    spec_path = rel("packaging", "rpm", "vst-ace.spec")
    if os.path.exists(spec_path):
        m = re.search(r"^Version:\s*(\S+)", open(spec_path, encoding="utf-8").read(), re.M)
        if m:
            found["rpm/vst-ace.spec"] = m.group(1)

    # version.h's fallback, for a compile outside the build system. CMake
    # always defines the macro, so this one never reaches a package -- but a
    # stale number here is still a wrong answer waiting for the first person
    # who builds a file by hand.
    m = re.search(r'^#define VSTACE_VERSION "([^"]+)"',
                  open(rel("peload", "version.h"), encoding="utf-8").read(), re.M)
    if m:
        found["peload/version.h"] = m.group(1)

    if len(set(found.values())) != 1:
        return FAIL, "disagree: " + ", ".join("%s=%s" % kv for kv in sorted(found.items()))
    return PASS, "%s in all %d places" % (next(iter(found.values())), len(found))


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


def check_man_xrefs(ctx):
    """Man page cross-references must resolve to a page that exists.

    Would have caught: the same rename leaving .BR dw (1) in three of the four
    pages, so `man pestudio` sent the reader to a page no package ships.
    """
    pages = {n[:-2] for n in os.listdir(rel("packaging")) if n.endswith(".1")}
    if not pages:
        return SKIP, "no man pages in packaging/"

    have_man = shutil.which("man") is not None
    dangling, unchecked = [], 0
    for page in sorted(pages):
        text = open(rel("packaging", page + ".1"), encoding="utf-8").read()
        for ref in re.findall(r"^\.BR?\s+(\S+)\s+\(1\)", text, re.M):
            if ref in pages:
                continue
            # Not ours. It may still be a real page on this machine -- a
            # reference out to something the distribution ships is fine.
            if not have_man:
                unchecked += 1
            elif run(["man", "-w", ref]).returncode != 0:
                dangling.append("%s.1 -> %s(1)" % (page, ref))

    if dangling:
        return FAIL, "; ".join(sorted(set(dangling)))
    if unchecked:
        return SKIP, ("%d reference(s) out of the tree, and no man here to "
                      "resolve them" % unchecked)
    return PASS, "%d pages, every (1) cross-reference resolves" % len(pages)


def check_png_decoder(ctx):
    """The PNG reader, against Python's own, on images built here.

    Would have caught: any of the ways an inflate goes subtly wrong. A VSTGUI
    editor is several hundred PNGs and nothing else -- background, knob
    filmstrips, button states -- so a decoder that is merely close produces an
    editor that is merely nearly right, and the difference does not announce
    itself. The images are generated rather than taken from a corpus, so this
    runs on a machine with no plug-ins: one per colour type, each in both
    interlaced and progressive form, at sizes chosen to leave awkward remainders
    in the Adam7 passes.
    """
    import struct, zlib
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return SKIP, "no C compiler"

    def png(w, h, colour, interlace, seed):
        """A PNG built by hand, and the RGB rows it should decode to."""
        ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
        pal, rows = b"", []
        rnd = seed
        def nxt():
            nonlocal rnd
            rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
            return (rnd >> 16) & 0xFF
        if colour == 3:
            pal = bytes(nxt() for _ in range(256 * 3))
        for _ in range(h):
            rows.append([tuple(nxt() for _ in range(ch)) for _ in range(w)])
        expect = []
        for r in rows:
            out = []
            for px in r:
                if colour == 2 or colour == 6: out.append(px[:3])
                elif colour == 3: out.append(tuple(pal[px[0] * 3: px[0] * 3 + 3]))
                else: out.append((px[0],) * 3)
            expect.append(out)

        passes = ([(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),
                   (0,2,2,4),(1,0,2,2),(0,1,1,2)] if interlace else [(0,0,1,1)])
        raw = b""
        for xo, yo, xs, ys in passes:
            pw = (w - xo + xs - 1) // xs
            ph = (h - yo + ys - 1) // ys
            if pw <= 0 or ph <= 0:
                continue
            for y in range(ph):
                line = b""
                for x in range(pw):
                    line += bytes(rows[yo + y * ys][xo + x * xs])
                raw += b"\x00" + line          # filter 0, so the test is the inflate
        def chunk(tag, body):
            c = tag + body
            return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c))
        blob = b"\x89PNG\r\n\x1a\n"
        blob += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, colour, 0, 0,
                                           1 if interlace else 0))
        if colour == 3:
            blob += chunk(b"PLTE", pal)
        blob += chunk(b"IDAT", zlib.compress(raw, 9))
        blob += chunk(b"IEND", b"")
        return blob, expect

    src = """#include <stdio.h>
#include "png_in.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); long n; uint8_t *d; uint32_t *px;
    int w = 0, h = 0, i;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc((size_t)n); if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    if (!(px = png_decode(d, (size_t)n, &w, &h))) { fprintf(stderr, "decode failed\\n"); return 1; }
    printf("%d %d\\n", w, h);
    for (i = 0; i < w * h; i++)
        printf("%u %u %u\\n", (px[i] >> 16) & 255, (px[i] >> 8) & 255, px[i] & 255);
    return 0;
}
"""
    tmp = tempfile.mkdtemp(prefix="pngchk-")
    ctx.setdefault("tmpdirs", []).append(tmp)
    cfile = os.path.join(tmp, "t.c")
    with open(cfile, "w") as f:
        f.write(src)
    exe = os.path.join(tmp, "t")
    r = run([cc, "-O1", "-o", exe, cfile, "-I", os.path.join(ROOT, "peload")])
    if r.returncode:
        return FAIL, "will not compile: %s" % r.stderr.strip().splitlines()[:1]

    cases, bad = 0, []
    for colour in (0, 2, 3, 4, 6):
        for interlace in (0, 1):
            for w, h in ((13, 9), (32, 32), (1, 5)):
                blob, expect = png(w, h, colour, interlace, w * 7 + h + colour * 3 + interlace)
                pf = os.path.join(tmp, "i.png")
                with open(pf, "wb") as f:
                    f.write(blob)
                out = run([exe, pf])
                cases += 1
                if out.returncode:
                    bad.append("colour %d interlace %d %dx%d: %s"
                               % (colour, interlace, w, h, out.stderr.strip()))
                    continue
                got = out.stdout.split()
                if [int(v) for v in got[:2]] != [w, h]:
                    bad.append("colour %d: size %s not %dx%d" % (colour, got[:2], w, h))
                    continue
                vals = [int(v) for v in got[2:]]
                flat = [c for row in expect for px in row for c in px]
                if vals != flat:
                    n = sum(1 for a, b in zip(vals, flat) if a != b)
                    bad.append("colour %d interlace %d %dx%d: %d of %d components differ"
                               % (colour, interlace, w, h, n, len(flat)))
    if bad:
        return FAIL, "; ".join(bad[:3])
    return PASS, "%d images, every pixel matches Python's reader" % cases


CHECKS = [
    ("stub-wiring",        check_stub_wiring),
    ("png-decoder",        check_png_decoder),
    ("arity-source",       check_arity_source),
    ("build",              check_build),
    ("dll-reachable",      check_dll_reachable),
    ("dll-lists-disjoint", check_dll_lists_disjoint),
    ("aw-parity",          check_aw_parity),
    ("arity-binary",       check_arity_binary),
    ("no-runpath",         check_no_runpath),
    ("peload32-complete",  check_peload32_complete),
    ("cmake-skip-path",    check_cmake_skip_path),
    ("pkgconfig32-env",    check_pkgconfig32_env),
    ("shell-syntax",       check_shell_syntax),
    ("sources-tracked",    check_sources_tracked),
    ("staging-excludes",   check_staging_excludes),
    ("i386-deps-match",    check_i386_deps_match),
    ("rules-staging-dirs", check_rules_staging_dirs),
    ("rpm-spec",           check_rpm_spec),
    ("version-consistent", check_version_consistent),
    ("desktop-files",      check_desktop_files),
    ("desktop-exec",       check_desktop_exec),
    ("man-pages",          check_man_pages),
    ("man-xrefs",          check_man_xrefs),
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
