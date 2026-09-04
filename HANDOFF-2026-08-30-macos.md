# Handoff — the macOS side, measured

Where the three macOS formats actually stand, taken by loading every bundle in
the corpus and rendering two seconds of audio from each. Measured at commit
`dcbb905`; the sweep is a dozen lines of shell around `peload --render` and is
worth re-running rather than trusting this table.

| corpus | bundles | rendered audio | silent | failed |
|---|---|---|---|---|
| `macos/VST2` (`.vst`) | 27 | **25** | 0 | 2 |
| `macos/AU` (`.component`) | 50 | **38** | 2 | 10 |
| `macos/VST3` (`.vst3`) | 18 | **0** | 0 | 18 |

> **Written 2026-08-30 and left on a branch until 2026-09-04.** The table
> below is that day's measurement at `dcbb905` and is kept as it was taken.
> The macOS side has moved since: both windows offer macOS plug-ins and drive
> them rather than only loading them, and the current corpus figure is 90 of
> 99 rendering. Re-run the sweep before trusting any number here.

`tools/triage.py` cannot do this: it globs `*.dll` and `*.so`, and a macOS
plug-in is a directory.

## VST3: nothing loads, for two reasons stacked

Every one of the eighteen fails at the same place, and it is not where
`peload/README.md` says the work is. The README describes the halves as
existing separately -- a Mach-O loader on one side, a VST3 driver on the other
-- which is true, but the first failure comes earlier than that.

**`find_binary` never looks in a Mac bundle.** `vst3.c:891` searches
`Contents/x86_64-linux` and `Contents/x86_64-win`, the Windows and Linux
convention. A macOS VST3 uses `Contents/MacOS/<name>` with no extension,
exactly as a `.vst` or `.component` does:

    Blooo.vst3/Contents/MacOS/Blooo
    Blooo.vst3/Contents/Info.plist
    Blooo.vst3/Contents/Resources/...

so the search finds nothing and reports "no VST3 binary inside", which is what
all eighteen say.

**And then there would be nowhere to load it.** `v3_open` has exactly two load
paths: `pe_module_load` under `V3_MSABI`, and `dlopen` otherwise. A Mach-O is
neither. `dlopen` on Linux will not take one.

Both are tractable, and the second is smaller than it sounds because the
loader already exists and exports what is needed:

    macho_open(path)          map the image
    macho_run_init(m)         run its initialisers
    macho_symbol(m, name)     look up an export
    macho_close(m)

The shape of the fix, in the `#else` branch of `v3_open`, choosing on the
image's magic rather than on the path:

    if (macho_magic(bin)) {
        h->mh = macho_open(bin);
        macho_run_init(h->mh);
        /* bundleEntry is the macOS spelling of ModuleEntry. It is handed the
         * CFBundleRef the host would have made; there is no CoreFoundation
         * here, and the plug-ins in this corpus do not dereference it. */
        if ((be = macho_symbol(h->mh, "bundleEntry"))) be(NULL);
        getfac = macho_symbol(h->mh, "GetPluginFactory");
    } else {
        ... the existing dlopen path ...
    }

`h` needs a `macho *mh` beside its `dl` and `pe`, and `v3_close` needs a
`macho_close`. **No build wiring is required**: `machoload.c` and the
`vst3_sysv` object library are both linked into `pehost`, so the symbols are
already in the same archive. macOS x86-64 is System V with the Itanium C++
ABI, the same as Linux, so the sysv build is the right one to extend and no
ABI thunking is involved.

There is also a `macho_magic()` helper to write -- `machoload.h` does not
currently expose one -- or the two-byte check can be inlined at the call site.
`0xFEEDFACF` is 64-bit Mach-O; a fat binary starts `0xCAFEBABE` and the corpus
here has none, but a check that ignores fat images should say so.

Detection declines separately from all this. `pehost_classify` recognises
`Contents/MacOS` inside a `.vst3` and sets `PEHOST_KIND_MAC_VST3` not-loadable
with "macOS VST3 bundles are not hosted yet" (`pehost.c:2486`); that decline
comes out once the above works. Routing is already in place --
`pehost_open_as` sends `PEHOST_KIND_MAC_VST3` to `open_vst3`.

## Audio Units: nine of the ten failures are one thing, and it is by design

The count looks worse than the situation.

    8x  load failed: no AU factory export found
        Automaton, Axon, Basic, DrDevice, Phosphor,
        RatshackReverb2, Replicant, Tattoo
    1x  Ragnarok:  _Ragnarok_Entry is a Component Manager entry point
    1x  Qyooo:     dumped core

The eight are not a missing feature in the loader. An AudioComponent AU
declares its entry point in the bundle's `Info.plist`:

    <key>factoryFunction</key>
    <string>Blooo_Factory</string>

`Blooo.component`, which works, has that. `Automaton.component`, which does
not, has **no `AudioComponents` array and no `factoryFunction` at all** -- its
plist stops at the `CFBundle*` keys. That is a Component Manager-era AU, whose
registration lives in a `thng` resource rather than the plist, and it is the
same pre-AudioComponent ABI `peload/README.md` already documents declining for
Ragnarok. So nine of the ten are one known and deliberate gap, not nine
separate bugs.

`nm -g` is no help investigating these on Linux: modern Mach-O keeps exports
in the `LC_DYLD_INFO` trie rather than the symbol table, and GNU `nm` reports
nothing for working and broken bundles alike. `Info.plist` is the reliable
place to look, and `macho_each_export` is the reliable tool.

Whether to implement the Component Manager entry point is a real decision
rather than an oversight. It would bring nine `.component` bundles in, and
seven of those nine ship a `.vst` build of the same plug-in that already
renders -- so the audio is mostly reachable already, and what would be gained
is the AU path specifically.

The two silent ones, `IdeeFixer` and `MNSpectralWeave`, load and run and
produce nothing. Not yet chased; on the Windows side that pattern was usually
the plug-in rather than the host.

## Qyooo crashes in both formats

`Qyooo.vst` and `Qyooo.component` both dump core, and `Tattoo.vst` times out.
Qyooo failing identically through two different bundle types points at the
shared Mach-O path rather than at either host, which makes it the most
informative crash on this side and the one worth a backtrace first. Nothing
has been done to it.

## What this handoff is not

No code was changed for any of the above. The VST3 fix is sketched from
reading, and the sketch has not been compiled, let alone run against a bundle.
Treat the two root causes as established -- they were read off the failing
paths and confirmed against the bundles on disk -- and the fix as a proposal.
