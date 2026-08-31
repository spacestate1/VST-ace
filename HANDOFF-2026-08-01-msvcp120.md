# Handoff — Native Instruments plugins under peload

**Status: three of the four load and report their AEffect. Massive stops at its
own licence check.** The MSVCP120 blocker this document was originally written
about is resolved.

| plugin | result |
|---|---|
| Absynth 5 | editor **764x687**, **renders audio** (peak 0.198, ~C4) — `0x436c6d35`, 2 in / 6 out, 128 programs, 21 params |
| FM8 | editor **948x562**, **renders audio** (peak 0.072) — `0x4e696638`, 0 in / 2 out, 128 programs, 1094 params |
| Kontakt 5 | editor **916x640**, silent (no library loaded) — `0x4e694f35`, 0 in / 64 out, 1 program, 512 params |
| Massive | `ExitProcess(1)` after looking for the Service Center licence directory |

All four previously stopped at the same symbol, `MSVCP120!operator<<`, with four
static initialisers run. Absynth now runs **1240** initialisers and reaches
`effOpen`, `effGetParamName` and `effGetProgramName`.

Regression, re-run after every change in this document:

```
Full Bucket 64-bit   36/36        Windows VST3     35/35
Full Bucket 32-bit   36/36        Linux VST2       32/32
macOS VST2           23/27  (see below)
Classic Mac          blockfish / spitfish / floorfish, editors 350x240
Windows editors      35 of 36 draw, none blank (one has no editor)
```

The three macOS failures are Audio Damage plugins reaching unimplemented
Objective-C runtime calls (`_class_addIvar`, `_objc_registerClassPair`). That
gap predates this work and is unrelated to it.

## How the MSVCP120 blocker was resolved

Not by implementing MSVC 2013's C++ standard library. `pe_module_load` now
**side-loads a real runtime DLL** and satisfies `msvcp*` imports from its export
table, so the layouts are the authentic ones rather than a reconstruction. The
previous plan in this document — reimplement `basic_streambuf`, `basic_ios`,
`basic_ostream`, `basic_stringbuf` with byte-exact layouts — is unnecessary and
should not be pursued.

`runtime/msvcp120.dll` is Microsoft's own, carved out of `vcredist_x64.exe`
(second attached CAB, member `F_CENTRAL_msvcp120_x64`). Wine's copy is not a
substitute: it expects Wine's host (`__wine_dbg_header`) and corrupts the heap.
Search order is `PELOAD_DLL_PATH`, then beside the plugin, then the usual Wine
directories.

Making it initialise was ordinary C runtime work, not stream work: the
`Concurrency` runtime (MSVC 2013 puts `critical_section` and
`_Condition_variable` in the **C** runtime, and a real msvcp120 fetches them by
`GetProcAddress` at load), `__pctype_func`, `__iob_func`, `setlocale`, the six
locale descriptors, `localeconv`. msvcp120 now runs its 2 CRT initialisers plus
57 static constructors cleanly.

## MSVC C++ exception dispatch — `peload/mscxxeh.h`

This turned out to be the real blocker, and it is not optional: **Absynth carries
a language handler on 12,178 of its 42,803 functions.** Throwing and catching is
how the code is written, so a host that cannot dispatch an exception cannot get
past its first database lookup.

Implemented for x86-64:

- `.pdata` / `.xdata` parsing, binary-searched (42,803 entries — a linear scan
  per frame per throw would dominate the host)
- the full unwind-code set, including `CHAININFO`, so a frame's prolog can be
  undone and the caller's context recovered
- MSVC's own tables: `FuncInfo`, `TryBlockMapEntry`, `HandlerType`, the
  IP-to-state map, `ThrowInfo` and its catchable-type list
- type matching by **mangled name**, not descriptor address: MSVC emits one
  descriptor per type per image, and this host has more than one image mapped
- delivery of the object into the catch's variable, by reference or by value
  (copy constructor when the type has one), then the catch funclet, then a
  register-restoring jump to the continuation it returns

- **destructors**, via the unwind map, for every frame the exception passes
  through and for the catch frame down to the try's state. These were skipped at
  first, on the theory that a leak beats the complexity. It is not a leak that
  bites: with them skipped, FM8 and Absynth dispatched their startup exception and
  then their audio thread missed every deadline and the out-of-process helper died
  — which is what a mutex nobody will now release looks like from outside.

One thing it does **not** do:

- **`RaiseException` has no dispatch.** The single exception is `0x406D1388`,
  thread naming, which is raised inside a `__try` whose `__except` swallows it —
  the `__try` contains nothing else, so returning normally lands exactly where
  the `__except` would continue. The name is passed to `pthread_setname_np`. Any
  other code aborts with a diagnostic.

The `_CxxThrowException` entry point is assembly, because the unwinder has to
start from the frame that threw and C code only ever sees its own.

## Bugs found, worth knowing about

**A second mapped image overwrote the first's resources.** `winstubs_init` set one
global resource base, so side-loading msvcp120 replaced the plugin's — and a
stock Microsoft DLL carries nothing but a version resource. Every
`FindResource` afterwards searched a directory holding one entry, which is why
Absynth could not find its own `PM`, `FM`, `FNT` and `PICTURE` resources. There
is now an image table, and the side-load path restores the plugin as primary.

**Window class names can be atoms.** `RegisterClass` correctly returned
`0xC000+i`, but `CreateWindowEx` only ever treated the name as a string. NI keeps
the atom, so this dereferenced a small integer.

**Path separators.** A plugin rebuilds a path it was handed using backslashes —
Absynth turned `/home/user/...` into `\home\user\...` — and every open, mkdir and
directory scan then failed on a spelling the filesystem cannot use. Normalised at
each entry point and at `file_open`, the one place every open passes through.

**Windows directory-enumeration semantics.** `FindFirstFile` returns `.` and
`..`, so an empty directory is not "not found"; and `*.*` matches names with no
dot at all. Both were wrong at first, and both made Absynth conclude its library
folders were missing when they were merely empty.

**Waits that do not wait.** `GetQueuedCompletionStatus` returned `WAIT_TIMEOUT`
immediately for `INFINITE`, and Absynth's three worker threads each burned a core
parking-polling. There is now a real completion port — queue, mutex, condition
variable — so a posted completion is also delivered rather than dropped. Same
class of fault as a stubbed `Sleep`, and just as invisible: every thread was
making progress through its loop as fast as it could.

**`VerifyVersionInfo` always returned 1.** The standard way to discover the OS
version raises a candidate while the API still agrees, so a function that always
agrees never terminates. It now compares against what `GetVersionExW` reports.

**Stubs that answer nothing.** `_errno` returns a *pointer* the caller writes
through; `_beginthreadex` returning garbage makes `std::thread` throw
`std::system_error`; `isspace`/`tolower`/`isalpha`/`isdigit`/`memchr` were never
registered at all, so string parsing took an arbitrary branch per character.
`IsWow64Process`, `GetLogicalProcessorInformation`, `CallNtPowerInformation` and
`_wsplitpath` all report through caller buffers and wrote nothing.

## Also new

- **The printf family**, narrow and wide, with `__builtin_ms_va_list`: a
  Microsoft va_list cannot be handed to glibc's `vsnprintf`, so each conversion
  is pulled off the argument list by hand. Full Bucket plugins link the CRT
  statically and carry their own, which is why this was missing until a
  dynamically-linked plugin turned up. 20 cases pass at both widths under
  ASan/UBSan. Known limit: output is assembled narrow, so a non-ASCII argument to
  a wide format loses anything outside Latin-1.
- **`setjmp`/`longjmp`** in assembly, saving the Microsoft callee-saved set —
  `rdi`, `rsi` and `xmm6`-`xmm15` included, none of which System V preserves. The
  buffer carries a magic word, and a `longjmp` through one this pair did not write
  is refused rather than jumped through. 9 cases pass at both widths.
- **`std::exception`** with a real vtable, so a constructor leaves something
  callable behind.
- **Floating-point control** (`_control87`, `_controlfp`, `_clearfp`, `_statusfp`,
  `fegetenv`/`fesetenv`, rounding) acting on MXCSR. These are reached during
  *render*: a synth that believes denormals are being flushed when they are not
  loses a lot of CPU in its filter tails.
- **Aligned allocation**, directory enumeration, a registry that round-trips what
  a plugin writes and honestly reports `ERROR_FILE_NOT_FOUND` for everything else,
  real per-user shell folders under `~/.peload`, monitor geometry, CRT locks.

## The editors

**FM8 and Kontakt 5 both render their real interfaces** — 948x562 at 97% non-black
and 916x640 at 82%. Getting there took three fixes, each the same shape: a stub
that answered nothing, where the honest answer was cheap.

- **`IsWindow`** was unimplemented. A plugin handed a parent window checks it
  before building into it, and a stub answering from a leftover register decides
  that at random. FM8 registered all four of its window classes and then created
  none of them: it had asked whether our container was real, been told no, and
  given up. 80 `WM_PAINT`s produced a black image because there was nothing to
  paint.
- **`GetDialogBaseUnits`** returned nothing. FM8 computes
  `(width * 4) / LOWORD(GetDialogBaseUnits())`, so zero is not a wrong layout, it
  is `idiv` by zero — `SIGFPE`. Windows returns 8 and 16 for the default font.
- **`SetDIBitsToDevice`** was the actual blit. Both plugins render their interface
  into a DIB section in software and hand it over in one call; FM8's entire GDI
  surface is ten functions. Until this existed they painted correctly into
  nothing. Note it differs from `StretchDIBits`: no scaling, and `startScan` /
  `scanLines` describe which band of a taller image the buffer covers — ignoring
  that draws the wrong rows.

`GetClipBox` went in alongside: reporting an empty box tells a renderer everything
is clipped away and it draws nothing.

Regression on the same change: 35 of 36 Full Bucket editors draw, none blank, one
has no editor.

**Absynth's editor renders too** — 764x687 at 94.6% non-black. Getting there was a
chain of four file-API faults, none of which announced itself:

1. **`GetFullPathName` returned zero for a size query.** The API has a two-call
   contract: with no buffer, return the size *required* including the terminator.
   The old code's copy loop tested `i + 1 < len` against a length of nought and
   its terminator write was guarded the same way, so it returned 0 -- which in
   that API means *failure*. SQLite's `winFullPathname` asks exactly that way, read
   the zero as failure, and reported `SQLITE_CANTOPEN: unable to open database
   file` for a file it had never tried to open. Absynth's editor then dereferenced
   the database object it expects always to exist.
2. **`GetFileAttributesEx` was a stub** that left the caller reading uninitialised
   stack as a file's size and timestamps.
3. **`LockFileEx` was a stub.** SQLite's concurrency protocol is byte-range locks
   at fixed offsets, and a failed lock means "someone else holds it" --
   `SQLITE_BUSY` on every query. Now implemented with fcntl locks, which are real
   and cross-process.
4. **`ReadFile`/`WriteFile` ignored the OVERLAPPED offset.** SQLite reads and
   writes every page through it. With the offset dropped, the database was
   *written* correctly -- those writes happened to run in order -- and then read
   back from wherever the file pointer sat, so the header came out as noise and
   SQLite said `file is encrypted or is not a database`. A real sqlite3 opened the
   same file and listed `DBSystemTable` without complaint, which is what pointed at
   the reader rather than the writer.

Absynth now builds its own content database: it ships a full SQLite and its own
`CREATE TABLE` statements, so nothing had to be supplied but working file calls
and the directories an installer would have made
(`AppData/Local/Native Instruments/Absynth 5/`,
`Documents/Native Instruments/Service Center/`). Empty directories only -- no
content, no licence data.

Two `SQLITE_IOERR` exceptions still occur during startup. They are caught by
Absynth's own handlers and it carries on to a full editor, so they are a loose end
rather than a blocker.

How the object was identified, since it is a reusable technique: the throw
dispatcher now prints the throw site as an image offset and scans the exception
object's first few words for a readable string, which is how
`"SQLITE_CANTOPEN[14]: unable to open database file"` surfaced. Those words are
guesses, so they are read through `process_vm_readv` -- dereferencing them
directly crashed the dispatcher on the first object whose members were not
pointers, and diagnostics that can kill the process are worse than none.

**Massive's editor still does not open**, because Massive still exits at its
licence check before reaching one.


## Two faults that killed the out-of-process helper

Both showed up as the same thing from the host: `bridge: helper missed its
deadline; the helper is gone`, and therefore no editor. Both are on the audio
path, which is why the editor looked like the broken part when it was not.

**The event block was freed before `processReplacing` ran.** `effProcessEvents`
was handed a `calloc`'d `VstEvents` that was released as soon as the dispatch
returned. A plugin is entitled to read those events during the following
`processReplacing` rather than copying them out, and FM8 does: it walked released
memory and took an event pointer out of whatever had reused it. The block and the
`VstMidiEvent`s now live on the host handle until the next call replaces them,
which also takes a `malloc` and a `free` off the audio path where neither belongs.

**`_aligned_offset_malloc` ignored its offset.** The call means "return p such
that `p + off` is aligned", not "return an aligned p". FM8 uses it with an offset
of 8 for a block laid out as an 8-byte count followed by 16-byte-aligned data, and
writes that data with `movaps` — which faults rather than running slowly when the
address is wrong. The whole `_aligned_*` family now shares one scheme that records
the original allocation in the word before the returned pointer, so
`_aligned_free` and `_aligned_msize` can recover it.

Audio buffers are also allocated 32-byte aligned and rounded to a whole number of
vectors now. That was not the cause here, but a plugin is entitled to assume the
buffers a host gives it suit aligned vector access.

## Plugin data directories are created on demand

Each NI product wants a different tree (`Native Instruments/<product>/`), and
creating them by hand does not scale. `file_open` now creates the missing parents
when a create fails with ENOENT — **only inside the directory tree this host hands
out through `SHGetFolderPath`**, never for a path the plugin chose elsewhere.
`CreateDirectory` does the same after its plain attempt fails.

Verified by deleting `~/.peload` entirely: Absynth rebuilt its
`Libraries/{Channel,Effect,Envelope,Filter,Mod,Oscil,Tuning,Wave,Waveshape}` tree,
Kontakt its `Db` and `QuickLoad/{Bank,Instr,Multi}`, and all three recreated their
databases and `Service Center/pal.db` unaided, with every editor rendering as
before.

## On what the audio proves

Absynth plays the requested note for **14 seconds and then cuts to exactly zero** —
a hard cut, not an envelope. Every program is still named `"<disabled>"`. That is
an unlicensed plugin muting itself, and it is the correct outcome: the licensing
runs, and it is not satisfied.


## The bridged editors were showing torn frames

Reported as choppy, blinking, and parts of the interface drawn twice. All three
were one fault: the host read the shared framebuffer **while the helper was
memcpy'ing the next frame into it**. `bridge_editor_pixels` handed back a pointer
straight into shared memory, so what got painted was the top of one frame and the
bottom of the next — controls drawn at two positions at once, and a flicker
whenever a half-copied frame landed.

There was already an `ed_gen` counter, but nothing used it to exclude a torn read.
It is now a sequence lock: the helper bumps it to odd before the copy and even
after, and the host copies into a buffer of its own, rechecking the counter and
retrying if it moved. If every attempt races it repaints the last whole frame
rather than a torn one — a repeated frame reads as a pause, a torn one as a
glitch.

Measured over a run of each editor in the host, before the fix would have shown
every one of these as a visible artefact:

```
Absynth   761 reads, 178 caught the helper mid-frame   (23%)
FM8       729 reads, 195                               (27%)
Kontakt   530 reads, 219                               (41%)
```

A quarter to two-fifths of every frame painted was a mixture of two. The counter
is kept and reported under `PELOAD_VERBOSE`, because it is the only way to tell a
smooth editor from a lucky one.

Bridged editors regress clean at both widths: 35 of 36 draw, none blank, one has
no editor.

## Where each remaining plugin stops

**Absynth** loads and renders, but **silently**, and every program is named
`"<disabled>"`. Its licence check ran and found no licence. That is the intended
outcome, not a defect to be worked around.

**Massive** enumerates
`~/.peload/AppData/Roaming/Native Instruments/Service Center/*.*` and calls
`ExitProcess(1)`. Creating the directory empty does not change it: it wants
licence content. Left alone deliberately.

**Kontakt** loads, and separately throws a boost `runtime_error` wrapped in
`error_info_injector` that escapes 10 frames with no handler — worth a look, since
it may be our type matching being too strict rather than a genuine escape.

**FM8** loads. Not yet rendered.

Absynth also expects an installed content layout that no installer has created
here. It looks for `Libraries/{Wave,Tuning,Channel,Notescale,Oscil,Filter,Mod,
Waveshape,Effect,Envelope}` under `~/.peload/AppData/Roaming/Native Instruments/
Absynth 5/`, and a SQLite database `NIAbsynth 5Database2_ul` under `AppData/
Local/...`. Empty directories are enough to get past enumeration; the database
throws `CppSQLite3Exception`, which its own code now catches twice and continues
past. There is no `LOCA` resource anywhere in the DLL or the installer payload —
its types are `FM`, `FNT`, `FRM`, `PICTURE`, `PM`, `TTFD`, `#3`, `#14`, `#16`,
`#24` — so localisation lives in files an installer would place.

## Reproducing

```sh
cd /storage01/code/c_things/vst-ace
cmake --build peload/build -j"$(nproc)"

cd peload/build
PELOAD_VERBOSE=1 ./peload "/storage01/synth_stuff/vsti/NI_VSTI_PACK_extracted/\
NI_Absynth_5_V5.3.0/data/data/OFFLINE/74D3EC57/C7D7176/Absynth 5.dll" --params \
  2>&1 | grep -vE 'unknown stdcall arity'
```

No environment variable is needed. `find_real_dll` locates `runtime/` relative to
the running executable, via `/proc/self/exe`, so it works whoever launched it and
from any working directory. That was not true at first, and the failure mode was
bad: the plugin loaded from the command line (where `PELOAD_DLL_PATH` had been set
by hand) and failed from `pestudio`, which does not set it — the same plugin,
apparently the same action, different result. `PELOAD_DLL_PATH` still wins when
set, for deliberately pointing at a different runtime.

## Known-imperfect, deliberately

Carried forward, still true: `_msize` reports glibc's usable size (`>=` what was
asked); `_recalloc` does not zero the new tail; `getenv` and `_wgetenv` return
NULL for everything; `clock` is rescaled for Windows' 1000 `CLOCKS_PER_SEC`;
`EncodePointer`/`DecodePointer` are identity. Added here:

- **`DuplicateHandle` returns the same value.** Handles are indices into one
  table, so `CloseHandle` on either copy releases the object. Windows would keep
  it alive until both closed.
- **Destructors are not run when unwinding**, as above.
- **`_aligned_realloc`** copies `malloc_usable_size` bytes, which is `>=` the old
  request — there is no portable aligned realloc.
- **Absynth's render exited cleanly once and dumped core once** on otherwise
  identical runs. Something at teardown is racy; not yet chased.

## On licensing

Unchanged, and it governed every decision above. These plugins require a genuine
Native Instruments licence and the point of this work is that they continue to.
The R2R tooling that shipped on the ISO is not involved and must not become
involved.

`GetVolumeInformationW` returns a real, stable, machine-specific serial, and
`GetComputerNameW` the machine's own hostname, because a licence is issued against
the fingerprint the machine reports and that fingerprint has to be honest for a
genuine licence to validate. Implementing these APIs faithfully is the work.
Making them report an identity that is not this machine's, or short-circuiting the
check that consumes them, is not — and would defeat the purpose, since the result
would be a plugin running *without* a licence rather than one running *with* one.

That Absynth loads and names every program `"<disabled>"`, and that Massive
refuses to start without Service Center content, are both the correct outcome.

Worth repeating: **B4 II, Pro-53 and FM7 cannot be licensed by anyone any more.**
NI switched off Service Center in May 2020 and lists them as unactivatable in
Native Access. The four above are the whole realistic target set.
