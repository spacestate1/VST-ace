#!/usr/bin/env python3
"""Load a directory of plug-ins and rank what the failures have in common.

One plug-in that will not load tells you very little: the backtrace lands in
guest code, and the reason is usually an import that quietly resolved to the
generic stub thousands of instructions earlier. A corpus tells you much more,
because the imports that only the *failing* plug-ins need are a short list, and
that list is the work queue.

So this runs the loader over every plug-in it finds, records how each one ended,
and collects every "unknown stdcall arity" line the loader emits -- each of
which names an import that got the generic stub, meaning it returns 0 and, at
i386, pops nothing. It then ranks those imports by how many *failing* plug-ins
import them, which is the order worth fixing them in. An import that fifty
working plug-ins also use is not what broke the other five.

    python3 tools/triage.py <dir-of-plugins> [--loader peload/build/peload32]
                            [--timeout 25] [--top 25] [--csv out.csv]

Written for the 32-bit loader, where a wrong stub is fatal rather than merely
wrong, but --loader takes the 64-bit peload just as well.
"""
import argparse, collections, csv, glob, os, re, subprocess, sys

STUB_RE = re.compile(r"\[stub\]\s+(\S+?)!(\S+?):\s+unknown stdcall arity")
SIG_RE = re.compile(r"\*\*\* (SIG\w+) in (\w+) code")
ADDR_RE = re.compile(r"addr\s+(0x[0-9a-fA-F]+)\s*(<[^>]*>)?")
FRAME_RE = re.compile(r"#0\s+(\S+)")


def load_one(loader, dll, timeout):
    """Run the loader once and boil the result down to a verdict.

    --params is enough: it loads the image, runs DllMain, calls VSTPluginMain
    and reads the AEffect back. Everything that breaks during init breaks here,
    without needing an audio device or a window.
    """
    env = dict(os.environ, PELOAD_VERBOSE="1")
    try:
        r = subprocess.run([loader, dll, "--params"], capture_output=True,
                           text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        return "HANG", "timed out after %gs" % timeout, out

    out = r.stdout + r.stderr
    if r.returncode == 0 and "uniqueID" in out:
        return "OK", "", out

    sig = SIG_RE.search(out)
    if sig:
        why = sig.group(1)
        a = ADDR_RE.search(out)
        if a:
            why += " addr %s%s" % (a.group(1), " " + a.group(2) if a.group(2) else "")
        f = FRAME_RE.search(out)
        if f:
            why += " at " + f.group(1)
        return "CRASH", why, out
    if r.returncode < 0:
        return "CRASH", "signal %d" % -r.returncode, out

    lines = [l.strip() for l in (r.stderr or r.stdout).splitlines() if l.strip()]
    noise = ("[stub]", "[win]", "[res]", "[dwrite]", "[bridge]")
    real = [l for l in lines if not l.startswith(noise)]
    return "FAIL", (real[-1][:60] if real else "exit %d" % r.returncode), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--loader", default="peload/build/peload32")
    ap.add_argument("--timeout", type=float, default=25.0)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--csv")
    a = ap.parse_args()

    if not os.path.exists(a.loader):
        print("no loader at %s -- build it first" % a.loader, file=sys.stderr)
        return 2

    plugins = sorted(glob.glob(os.path.join(a.directory, "*.dll")) +
                     glob.glob(os.path.join(a.directory, "*.so")))
    if not plugins:
        print("no plug-ins under %s" % a.directory, file=sys.stderr)
        return 2

    results, stubs_of = [], {}
    for i, p in enumerate(plugins, 1):
        print("\r  %d/%d %-44s" % (i, len(plugins), os.path.basename(p)[:44]),
              end="", file=sys.stderr, flush=True)
        status, why, out = load_one(a.loader, p, a.timeout)
        results.append((os.path.basename(p), status, why))
        stubs_of[os.path.basename(p)] = {"%s!%s" % m for m in STUB_RE.findall(out)}
    print("\r" + " " * 60 + "\r", end="", file=sys.stderr)

    ok = [r for r in results if r[1] == "OK"]
    bad = [r for r in results if r[1] != "OK"]

    print("%d plug-ins: %d loaded, %d failed\n" % (len(results), len(ok), len(bad)))
    if bad:
        print("failures")
        print("-" * 78)
        for name, status, why in bad:
            print("  %-26s %-6s %s" % (name[:26], status, why))
        print()

    # The ranking. A symbol every plug-in imports says nothing; one that only
    # the failures import is a lead.
    failing = {n for n, s, _ in results if s != "OK"}
    total = collections.Counter()
    among_failing = collections.Counter()
    for name, syms in stubs_of.items():
        for s in syms:
            total[s] += 1
            if name in failing:
                among_failing[s] += 1

    if not total:
        print("no imports fell through to the generic stub -- "
              "the failures are not missing stubs")
        return 0

    print("imports that got the generic stub (returns 0, pops nothing)")
    print("ranked by failing plug-ins that import them")
    print("-" * 78)
    print("  %-5s %-5s %s" % ("fail", "all", "import"))
    ranked = sorted(total, key=lambda s: (-among_failing[s], -total[s], s))
    shown = [s for s in ranked if among_failing[s]][:a.top]
    for s in shown:
        print("  %-5d %-5d %s" % (among_failing[s], total[s], s))
    if not shown:
        print("  (none -- every generic-stub import is also used by a plug-in"
              " that loads, so none of them is the thing that breaks these)")

    only_failing = [s for s in ranked if among_failing[s] and among_failing[s] == total[s]]
    if only_failing:
        print("\n%d of those are imported by failing plug-ins only -- start here:"
              % len(only_failing))
        for s in only_failing[:a.top]:
            print("    %s" % s)

    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["plugin", "status", "detail", "generic_stub_imports"])
            for name, status, why in results:
                w.writerow([name, status, why, " ".join(sorted(stubs_of[name]))])
        print("\nper-plug-in detail written to %s" % a.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
