# peload — running Windows, macOS and Linux plugins natively, no Wine

Six hosts share one set of shims:

| target | program | plugins | status |
|---|---|---|---|
| Windows VST2, x86-64 | `peload` | 36 | 34 render, 35 show their own GUI |
| Windows VST2, i386 | `peload32` | 36 | 34 render, 35 show their own GUI, live playback |
| Windows VST2, i386, in `pestudio` | bridge | 36 | 34 render, 34 GUIs, out-of-process |
| Linux VST2 (`.so`), x86-64 | `peload` | 39 | 37 render |
| Windows VST3, x86-64 | `peload` | 35 | 35 render, 34 show their own GUI |
| Linux VST3, x86-64 | `peload` | 13 | 7 render, 0 crash, X11-embedded editors |
| macOS VST2, x86-64 | `peload` | 19 | 19 render, 19 show their own GUI |
| macOS Audio Units, x86-64 | `peload` | 42 | 41 render |

The i386 and x86-64 hosts now reach the same result on the same 36 plugins:
35 render, 34 open their editors, and the two that fail are the same two for the
same reasons.

Known failures:
`brokenmini` loads but registers a font that never reaches the text backend, so
its editor is refused rather than painted (identical at both widths).
`brokenmini` and `drumtraqs` load and run but render silence — also at both
widths, so that is plugin behaviour, not the loader.

Six of the thirteen Linux VST3s render silence for reasons they state themselves:
the five gearmulator plugins (`JE8086`, `Osirus`, `OsTIrus`, `Vavra`, `Xenia`)
emulate Access Virus and Waldorf hardware and refuse to make sound without the
original firmware ROM — *"a firmware rom (a single 512k .bin file or multiple
.mid files) is required, but was not found"* — and `Cardinal.vst3` opens an empty
rack by default. Nothing in that list is a loader failure.

### TAL-U-No-62: four host bugs wearing a plugin's clothes

This one was written off as "crashes inside its own code" through three separate
faults, and every one of them was ours:

1. **`audioMasterGetTime` returned NULL.** TAL reads the transport and
   dereferences what it gets. Answering with a real, advancing clock fixed the
   render crash outright — the same missing callback that was crashing four u-he
   plugins.
2. **`effEditGetRect` was dispatched with a NULL `ptr`.** The opcode's contract is
   that the plugin writes an `ERect*` through it. Most plugins check first; TAL
   stores unconditionally, so the host was asking it to write to address zero.
   That was the editor "crash".
3. **`LoadBitmapA` was a `return NULL` stub.** TAL keeps its whole editor in a
   944 KB `.rsrc` and loads it through that call, so it got NULL, stored it, and
   wrote through it. It also could not size a window it had no artwork for, which
   is why it reported an empty rect and the editor was refused. `RT_BITMAP`
   resources are now decoded properly — packed DIBs, 1/4/8/16/24/32 bpp,
   bottom-up unless the height is negative.
4. **`w32_target` returned a pointer to a single function-static.** `BitBlt`
   resolves two DCs, so when both had a bitmap selected the source and
   destination aliased the same struct: the blit copied a bitmap onto itself and
   the editor stayed black. The caller now supplies the scratch.

A fifth kept the mouse from working. TAL imports `GetAsyncKeyState` and
`GetCursorPos` but no capture APIs at all -- it *polls* the button rather than
reading messages -- and `w32_mouse` only ever set `W.keys[]` from the keyboard.
The buttons are now recorded there too, and dragging its controls works.

Result: audio at peak 0.2045 identically at both widths, and a 695x350 editor
at 96.9% painted. The top title band is still drawn flipped, which is the one
thing here that has not been chased down.

## Two hosts, one argument list

The 32-bit bridge spawns `peload32 --serve`, and when `peserve` was added later
the launcher gained `--rate` and `--block`. `peload32`'s option parser never
learned them, so those values fell through to its `argv[i][0] != '-'` arm and
became the plugin path:

```
512: No such file or directory
load failed: the helper died before reporting
```

Every bridged 32-bit load failed that way -- which is to say `pestudio` could not
host a single 32-bit plugin, while `peload32` run directly was fine on 34 of 36.
Two hosts sharing one launcher need to share the argument list too; both now
accept the same options, and the helper uses the rate and block size it is given
rather than assuming 48000/512.

## One plugin should not take the host down

`pestudio` is a browser: it loads a hundred plugins in a session, and any one of
them faulting used to end the session. `TAL-U-No-62` does exactly that, in its own
code, every time it renders. So `pestudio` now hosts plugins out-of-process by
default, through the same `peserve` helper the 32-bit bridge uses:

```
                       in process        isolated
Windows VST2 x86-64    1 host crash      0 host crashes
macOS VST2             1 host crash      0 host crashes
macOS Audio Units      1 host crash      0 host crashes
```

A crash costs a subprocess, and the host reports silence or a failed load instead
of dying. Editors still arrive -- as pixels through shared memory: 34 of 36
Windows VST2 and 18 of 19 macOS VST2 editors capture identically either way.

The exception is native Linux plugins. Their editors are X11 windows embedded into
one of ours, and the bridge carries pixels, not window ids -- so `pehost` keeps
those in process regardless of the isolation setting. `PEHOST_ISOLATE=0` turns the
default off; `=1` forces it on for `peload` too.

## Modal drag loops, and why op 13 hung

TAL's editor polls: `GetAsyncKeyState(VK_LBUTTON)` with `GetCursorPos`, in a loop,
until the button comes up. It imports no capture APIs at all. So once the button
state was reported honestly -- which is what made its controls draggable in the
first place -- pressing one entered a loop that could never end, because *the only
agent that knows the button was released is the host, and the host is inside the
wndproc call that started the loop*. The helper stopped answering and the
five-second socket deadline reported `op 13`, which is `BR_EDITOR_MOUSE`.

Editor input is therefore no longer a request. The host writes mouse events into a
lock-free ring in shared memory, and the helper drains it from two places: its own
loop, and a `pump_input` hook the window layer calls from `PeekMessage` and from
the key-state and cursor queries. That last part is the whole point -- it is what
lets a plugin spinning in its own loop observe input that arrived after the spin
started. Bridged, a press is now answered in 0.0 ms and 15 of TAL's 47 parameters
respond to drags.

Keys had the identical problem and I only fixed the mouse first, which the next
report caught: holding a key while adjusting a control stalled `op 14`
(`BR_EDITOR_KEY`). Both kinds of editor input now share one ring.

That still left everything *else* the host asks during a drag. `pestudio` polls
parameter values to keep its UI live, and those requests waited out the same
deadline -- so the window froze even with input flowing. The helper now answers
read-only requests from inside the spin as well: it peeks at the pending op and
serves parameter, program and geometry queries re-entrantly, leaving anything
structural (opening an editor, changing program, quitting) for the main loop.
A parameter query with the button held went from the full five-second deadline to
**4.9 ms**.

In process the same trap cannot be escaped: there is no second thread to publish
the release. `pestudio` defaults to isolation so it does not meet this, but
`peload --editor` would, so there is a valve -- a button reported held for half a
second with no input arriving and no pump installed reads as released, with a line
on stderr saying so. A real drag produces events continuously, so it only fires
when nothing can feed the loop at all.

`peserve` also takes `PR_SET_PDEATHSIG` now. The hung helpers were invisible and
each held a plugin open; a helper should not outlive the host that spawned it.

## Classic Mac OS: measuring before building

I first said this was infeasible -- SheepShaver territory, a whole-machine
emulator. That was wrong, and measuring rather than estimating is what corrected
it.

Five Mac OS 9 VST plug-ins came out of Audio Damage's Mayhem bundle
(`MayhemVST_OS9.sit`, which needs `unar`; nothing else here reads StuffIt 5).
Two small readers established what they actually are:

```
tools/rsrcdump.py   AppleDouble -> resource fork -> resource map
tools/pefdump.py    PEF container -> sections, loader, imported symbols
```

They are AppleDouble files whose *entire* content is a resource fork -- no data
fork at all. Inside: `PICT` artwork and an `aEff` resource holding the code, which
turns out to be a PEF container of PowerPC. So a Classic VST is a code fragment in
a resource, not a file the loader could ever open directly.

The number that mattered:

```
Crush mono         pwpc   68 symbols   InterfaceLib MathLib DragLib
Crush stereo       pwpc   68 symbols
Filterpod          pwpc   68 symbols
MasterDestrukto    pwpc   68 symbols
TimeFnk            pwpc   67 symbols
                  union:  70 distinct host functions
```

Seventy, not a Toolbox. Eleven are MathLib and map onto libm; six are drag and
drop and can refuse politely. Of the fifty-one InterfaceLib calls, around twenty
are trivial (NewPtr, HLock, TickCount, SetCursor), which leaves roughly twenty-five
of real work: QuickDraw drawing, GWorlds, `CopyBits`/`CopyMask`, and `DrawPicture`
for PICT. Offscreen bitmap compositing, which this project already does twice over.

What exists so far is `ppc.c`, a 32-bit PowerPC interpreter: integer, branch,
load/store, rotate-and-mask, the condition register, LR and CTR. Guest memory is
big-endian and a guest address is an index into a flat block, so interpreted code
cannot reach the host's address space -- which matters when the code is a
twenty-year-old binary of unknown provenance. Tested on hand-assembled machine
code, deliberately including the two things most likely to be silently wrong:

```
sum 1..10 with bdnz          55        ok
big-endian store, byte load  0x12      ok      (host order must not leak in)
rlwinm r3,r4,8,24,31         0xab      ok      (bit numbering runs the other way)
cmpwi/beq not taken          99        ok
load from 0xffff0000         caught and named, not a host segfault
```

Still to do, in order: the PEF loader (sections, relocations, TVector and TOC
setup), floating point (a DSP needs it), the seventy host functions, the VST 1.0
CFM calling convention -- the plug-in's entry point is a TVector, so the host
callback must be one too -- and PICT decoding for the editor.

## Telling the Mac eras apart

"Cannot map" is the wrong answer for a Classic Mac OS plug-in, because it invites
the reader to look for a bug. A Mac OS 8/9 plug-in is not an old Mach-O at all: it
is a **PEF** container -- the Code Fragment Manager's format -- holding PowerPC
code. No amount of work on this loader would run one. Mac OS X on PowerPC is a
different case again: right format, wrong instruction set. Each now says which it
is, because each implies something different about whether it is worth pursuing:

```
Classic Mac OS / Carbon (CFM/PEF, PowerPC)             'Joy!peff'
Classic Mac OS resource fork (AppleSingle/Double)      0x00051600 / 0x00051607
StuffIt archive (Classic Mac OS) -- unpack it first    'StuffIt' / 'SIT!'
Mac OS X for PowerPC                                   big-endian Mach-O, cpu 18
a PowerPC/i386 binary: Mac OS X of the PowerPC era     fat with no x86-64 slice
an i386-only Mach-O: 32-bit Intel macOS                fat with only cpu 7
arm64-only binary: no x86_64 slice to run              fat with only cpu 0x0100000C
```

The StuffIt case is not hypothetical: Audio Damage's Mayhem ships
`MayhemVST_OS9.sit` beside `MayhemVST_OSX.sit`, and handing the wrong one to the
loader should not look like a failure of the loader. The plug-in list shows the
same text next to the entry, so it is visible before anything is loaded rather
than after.

## Legacy Objective-C dispatch

Ragnarok crashed on load for years' worth of reasons that were all one reason. The
fault was a virtual call on a null bitmap inside `IGraphics::LoadIBitmap`, two
frames after `IGraphicsMac::OSLoadBitmap` returned nothing, and nothing in the
Objective-C trace showed a message being sent -- because as far as the runtime was
concerned, none ever was.

The call sequence gave it away:

```
1a686:  mov  rdi,[0x9c950]        ; receiver
1a68d:  lea  rsi,[0x9bf40]        ; the *address* of a struct
1a694:  call [0x9bf40]            ; through the struct's first field
```

That is not a normal message send. It is the older **fixup dispatch** ABI: the
compiler emits a `struct message_ref { IMP imp; SEL sel; }` per call site, puts
its address in `%rsi`, and calls through `ref->imp`, which the linker initialises
to `objc_msgSend_fixup`. The selector lives at `8(%rsi)`, not in `%rsi`.

Nothing here implemented that, so every Objective-C call the plugin made resolved
to whatever its unresolved import had been bound to -- in this case a no-op Quartz
stub. Its PNG loader quietly returned nothing and the crash surfaced later
somewhere unrelated. Three trampolines (`objc_msgSend_fixup`, the `_stret` and the
`Super2` flavours) fetch the selector from the message_ref and then behave exactly
like the ordinary ones.

Worth noting what this was *not*: no separate framework for one plugin, and no
special case. The missing piece was a general ABI, and the same three entry points
serve any image of that vintage. macOS VST2 went from 18 of 19 rendering to **19 of
19 with no crashes**, and Audio Units from 38 to **41**.

Its editor took three more general gaps, none of them Ragnarok-specific:

- **Nothing turned a dirty view into a draw.** A Cocoa view marks itself with
  `setNeedsDisplayInRect:` and expects `drawRect:` to follow. Ragnarok's editor
  asked twenty times per second and was never once asked to paint, because there
  is no run loop here to do it. `macns_draw_dirty` closes that loop.
- **CoreText's attribute names were registered as a function.**
  `kCTFontAttributeName` and `kCTForegroundColorAttributeName` are *data* --
  CFStringRefs -- so handing over a code address meant drawing text dereferenced
  nothing.
- **CoreFoundation objects were not recognised as Objective-C objects.** CF and NS
  types are interchangeable on macOS, so `[cfString release]` is legal, and WDL's
  compatibility layer relies on it. The runtime read the CF magic word as an isa
  and walked it as a class. It now recognises one and bridges retain, release and
  autorelease.

The editor an older plugin draws is simply the bitmap context it created: one the
size of its window, which it composites everything into. That is what
`macquartz_editor_pixels` hands back, and it is why the same host code shows both
kinds of editor without knowing which it has. Ragnarok paints **427680 of 427680
pixels**.

## Stuck notes

The virtual keyboard sent a note-off from `keyReleaseEvent` and nowhere else, so
any path that swallowed the release left the note sounding. Clicking the editor to
adjust a control is exactly such a path: focus moves to the editor widget and the
key-up is delivered there instead. Hold a note, reach for a knob, and the note
stays down for good.

Releasing on focus loss fixed the sticking and broke the point of holding a note.
The reason to hold one is to go and turn a knob and hear what it does -- and
turning a knob moves focus to the editor, which is exactly what that "fix" treated
as a reason to cut the note off.

So note keys are watched application-wide instead: an event filter routes them to
the keyboard whatever has focus, without consuming them, so a plugin that wants
keyboard input still gets it. A key-up is therefore always seen. Held notes are
given up only when the whole window loses the desktop's focus, which is the one
case where no key-up can ever arrive:

```
piano: released 1 held note(s)
```

Verified by driving the real window with xdotool: hold a note key, click into the
editor and drag a control, and the note keeps sounding; release the key and the
keyboard repaints, so the note-off landed. Both halves measured rather than
reasoned about, because the first version of this looked correct and was not.

Control changes reach the audio in **one block** (~5.3 ms at 256 frames), measured
by moving a control in the plugin's editor and counting blocks until the parameter
took effect. Adjusting a control while a note sounds does what it should.

## The bridge, under a GUI

Three things about the helper only show up with an editor open and a hand on the
controls, which is to say only in `pestudio`:

- **The helper published its framebuffer on every request.** `publish_editor` was
  called from three places in the loop, one of them after every single op -- and a
  publish means a repaint plus a copy of the whole framebuffer, nearly a megabyte
  for a 695x350 editor. Every mouse move the GUI sent therefore paid for a
  redraw. Rate-limiting it to 30 Hz took the round-trip a GUI thread waits on
  from **1.42 ms mean / 2.45 ms worst to 0.02 / 0.16**. TAL is the plugin this
  hurt most, because it is the only one here that repaints on every idle rather
  than only when something changed.
- **The request socket had no timeout.** Parameter displays, editor mouse and
  keys are all synchronous round trips made from the GUI thread. A helper that
  stalled froze the window outright -- and a frozen window never sends the
  note-off for whatever was being played, so the note hangs. There is now a
  five-second deadline: far longer than any real op, and a stalled helper gets
  dropped with a message instead of taking the window with it.
- **A missed deadline left a request in flight.** The helper finishes it and posts
  `done` regardless, so the next wait succeeded instantly on the stale post while
  the helper was still writing the shared buffer. Outstanding requests are now
  collected before another is posted. Worth stating plainly: a control experiment
  with induced misses showed the stream survives either way, so this is a torn-
  audio race rather than the freeze it was first suspected of being.

## X11 windows and Wayland

A plugin editor on Linux is an X11 window: a VST3 is handed a window id it treats
as `kPlatformTypeX11EmbedWindowID`. Under Qt's Wayland backend `winId()` returns
a Wayland surface handle instead, and a plugin given that runs Xlib against a
window that does not exist -- it dies inside its own toolkit, which looks exactly
like a crash on load. It reproduced on KDE Wayland with four different plugins
and never once under X11.

`pestudio` now asks for the xcb backend when it finds itself in a Wayland session
with a `DISPLAY` (XWayland is present on any Wayland desktop that can run these
plugins), says so on stderr, and still honours an explicit `QT_QPA_PLATFORM`.
Attaching an editor on a non-xcb platform is refused with a reason rather than
handed to the plugin.

## Side effects belong outside a log call

Two bugs shared one cause, and it is worth stating plainly because the pattern is
easy to write and invisible afterwards:

```c
VLOG("vst3: setActive -> %d, setProcessing -> %d\n",
        h->comp->vt->setActive(h->comp, 1),           /* never called with */
        h->proc->vt->setProcessing(h->proc, 1));      /* logging switched off */
```

`VLOG` compiles to `if (verbose) fprintf(...)`, so its arguments are only
evaluated when logging is on. **Every VST3 plugin was therefore running
unactivated** unless `PELOAD_VERBOSE` happened to be set. Most tolerated it.
`OB-Xf` did not: JUCE runs `prepareToPlay` from `setActive`, so its MIDI handler
was never constructed, and the first note dereferenced a null pointer inside
`MidiHandler::processMidiPerSample`. The plugin had been written off as
"crashes inside its own code" — it crashed inside its own code because the host
never activated it.

`setBusArrangements` and a `setComponentState(NULL)` probe were hiding in the same
place. Calling them changed two further things:

- `setComponentState` takes an `IBStream`, and NULL is not one. The probe only
  ever looked harmless because it never ran; once it did, every Full Bucket VST3
  went silent, which is a fair response to being told to load a null patch. It is
  gone.
- `setBusArrangements` is allowed to rebuild the bus list, so it has to come
  *before* `activateBus`, not after. Activating first left the output bus
  inactive: the call returned `kResultOk` and the plugin rendered silence.

`grep` for a call inside a log macro's arguments is now part of the checklist. The
remaining hits are field reads, which are safe to lose.

## macOS plugins, and a GPU that is not there

macOS x86-64 *is* System V — the same ABI this host already uses — so a Mach-O
function pointer is directly callable and there is no convention layer at all.
That makes the loader (`machoload.c`) the easy part and the frameworks the whole
job: CoreFoundation, libSystem, Accelerate's vDSP, Core Graphics, Foundation,
AppKit and a small Objective-C runtime (`macobjc.c`), because Apple's classes
have to be *ours* regardless of what the plugin expects to message.

The editors turned out to hinge on one measurement. Of nineteen macOS VST2
bundles here, **eighteen draw through NanoVG's Metal backend** and none has an
OpenGL path compiled in, so on macOS a GUI means Metal or it means nothing:

```
$ tools/macsym.py Qyooo.vst 0x958e3
0x958e3      _nvgCreateImage+0x73
```

Emulating a GPU would be hopeless. What makes it tractable is that the shaders
are not arbitrary: `nanovg_mtl` ships exactly one vertex shader and one fragment
shader, and their behaviour is fixed by nanovg's own source. So `macmetal.c`
ignores the precompiled Metal bitcode the plugin hands to
`newLibraryWithData:`, recognises the pipeline from the state the plugin sets,
and rasterizes the draw calls in C — a Metal object model plus a small software
rasterizer, not a driver.

The vertex shader is what makes it cheap. It is a pure viewport transform, and
since nanovg passes the same size as both `viewSize` and the viewport, a
vertex's pixel coordinate *is* its position attribute. What still has to be real
is the stencil buffer (nanovg fills concave paths by counting winding into
stencil, then covering), premultiplied-alpha blending, and the fragment shader's
gradient, image and scissor maths.

Two bugs found on the way there were not Metal bugs at all, and both were the
same mistake in different places: **storing our own state inside an object the
plugin subclasses.** Objective-C's modern runtime has non-fragile ivars — the
compiler emits a subclass's ivar offsets against whatever superclass size it saw
in the SDK, and the runtime slides them at class realization. Nothing here
realizes classes, so those compile-time offsets stand as written:

- `NSView`'s stand-in kept `frame.size.height` at offset 40, exactly where
  `IGraphicsView` keeps `mTextFieldView`. Sizing the editor stored `648.0` over
  the text field, and tearing it down sent `setDelegate:` to a double.
- the refcount sat at offset 8, where the first ivar of *any* direct NSObject
  subclass goes. `MNVGcontext` keeps its Metal command buffer there, so every
  retain of the context incremented the command-buffer pointer by one and the
  next message went to a misaligned address.

Both now live outside the object — the view state in a side table, the refcount
in front of the allocation behind a magic word. A third was plain ARC semantics:
`objc_retainAutoreleasedReturnValue` was a no-op, and since ARC emits it paired
with a release, `id d = [layer device];` destroyed the plugin's Metal device on
the very next line.

A fourth only showed up once two plugins were loaded in one session, which is
what `pestudio` does. `[NSBundle mainBundle]` was cached forever — reasonable,
since a plugin compares bundle pointers and expects a stable identity — but the
cache outlived the plugin. The second plugin then looked for its artwork in the
first one's `Resources`, got an empty bitmap, and divided by its zero frame
count: a SIGFPE on the second load, in either order. The cache is now keyed on
the bundle path, and closing a plugin drops its timers, its editor view and its
layer, because a timer left behind fires into a torn-down editor on the next
pump.

One diagnostic was worth adding for its own sake: the local libc++ under
`thirdparty/` used to be found by stripping exactly two path components from
`/proc/self/exe`, which works for `build/peload` and silently fails anywhere
else. What it fails at is loading libc++, so the symptom is a plugin dying in a
static destructor or inside `memcpy` on a string that was never constructed —
twice, convincingly, in a scratch test harness built outside the tree. The search
now walks upward looking for `thirdparty/`, and says so plainly when there is no
libc++ to be had.

`tools/macsym.py` maps the `image+0xNNNN` offsets in a fault report back to
symbols, and disassembles around them with `--disasm`. Every diagnosis above
started there.

Cost, measured rather than guessed — `macvst --editor` reports it:

| plugin | first paint | steady state |
|---|---|---|
| MPS (608x352) | 189 triangles, 445k pixels, 25 ms | 0 ms |
| Qyooo (858x648) | 1159 triangles, 1.2M pixels, 69 ms | 0 ms |
| WhispAir (1127x776) | 4783 triangles, 2.0M pixels, 115 ms | 0 ms |
| ModulAir (1280x650) | 2879 triangles, 3.2M pixels, 182 ms | 0 ms |

The steady-state zero is not a rounding artefact: an iPlug2 editor repaints only
what changed, so after the first frame the rasterizer is handed nothing at all
until a control moves. The one-off cost on open is the price, and the pixel
counts show why — ModulAir shades nearly four screens' worth of overdraw for its
layered panels.

## Hosting 32-bit plugins from the 64-bit host

A process cannot execute both widths, so `pestudio` cannot load an i386 plugin
however the loader is written. It runs in a `peload32 --serve` helper instead,
and `pehost_open` dispatches to it — so the GUI, the CLI and the renderer are
unchanged and unaware. `pehost_is_bridged()` exists only so the plugin list can
label it.

Two channels, split by who waits (see `bridge.h`):

- a **UNIX socket** for request/response — names, counts, program changes,
  editor open. All from the GUI thread, so a round trip is fine.
- a **shared mapping** for what a socket cannot carry: the audio block, the
  editor's framebuffer, and lock-free rings for parameter and MIDI writes.

The audio handshake is a counting semaphore built on one 32-bit word and the
futex syscall. It cannot be a `sem_t`: glibc's is 16 bytes on i386 and 32 on
x86-64, so the two processes would not even agree where it ends — the first
attempt failed with *"The futex facility returned an unexpected error code"*.
Nothing in that struct may be width-dependent, and a layout test compares
`offsetof` for every member at both widths.

The host waits with a deadline of two periods; if the helper stalls or dies it
emits silence and says so once, rather than blocking the whole PipeWire graph.
That is worth having: `TAL-U-No-62` faults in its own code, and where the 64-bit
build takes the host down with it, the bridged 32-bit one only loses its helper —
`pestudio` cycles all 36 with no fault.

Rendered through the bridge, **31 of 36 come out bit-identical to running the
same plugin directly in `peload32`**; the rest differ only because the two CLIs
feed different test input.

## What makes this tractable

Two things that sound hard turn out to be free:

- **The Microsoft x64 calling convention.** `__attribute__((ms_abi))` makes GCC
  generate it in full — shadow space, RCX/RDX/R8/R9, the RSI/RDI/XMM6-15
  callee-saved set. No hand-written thunks anywhere.
- **The TEB.** Guest code reads thread-local state through a segment register.
  On x86-64 glibc uses `%fs`, so `%gs` is free and `arch_prctl(ARCH_SET_GS)`
  installs a fake TEB. On i386 this inverts — glibc uses `%gs`, so `%fs` is ours,
  via `set_thread_area`.

What is left is ordinary work: map sections, apply relocations, bind imports,
run TLS callbacks and `DllMain`, and implement enough of Win32 that the plugin's
CRT starts. That last part is 354 registered stubs; a given plugin binds 120–230
of them and calls almost none at runtime — the CLI prints the tally, so it is
measured rather than assumed.

## The i386 port, and what it exposed

A process cannot execute both widths, so `pe32.c` is a separate program rather
than a mode of `peload`. It shares the whole shim layer — `winstubs.h`,
`win32gui.h`, `dwrite_shim.h` — through `winstubs32.h`, which does one thing:
select `stdcall` for the Win32 convention. The only code that had to be written
twice is the JIT'd probe stub in `dwrite_shim.h`, which emits machine code.

Porting to i386 surfaced six bug classes that x86-64 hides completely. Every one
of them was already in the 64-bit code and invisible there:

**1. Argument counts.** An i386 `stdcall` callee pops its own arguments, so a
stub whose prototype has one argument too many corrupts the caller's stack by 4
bytes. On x86-64 the caller always cleans up and a surplus register argument is
simply ignored — so eighteen wrong arities sat there harmlessly. Three were in
`winstubs.h` (`CreateThread`, `EnumResourceNamesA/W`); the other fifteen were
every message-passing stub in `win32gui.h`, where `WPARAM` and `LPARAM` were
declared 64-bit. Those are pointer-sized — 4 bytes on Win32 — so each WndProc
call pushed 8 bytes too many per parameter, and a stdcall window procedure pops
only what its own signature says.

`tools/check_arity.py` compares every stub against the real export, using the
fact that mingw-w64's i686 import libraries encode the byte count in the
decorated name (`_CreateFileW@28`). It follows `#include "..."`, because the
first version only read the file it was given and silently skipped the entire
window layer — which is where those fifteen were hiding. Run it after touching a
prototype:

```sh
python3 tools/check_arity.py peload/winstubs.h
```

The same data drives `win32_arity.h` (`tools/gen_arity.py`, 22484 exports), so
a JIT stub for an *unimplemented* import can emit `ret N` instead of `ret` —
otherwise the first call to any stubbed function drifts the stack until some
later `ret` jumps to address 0.

**2. Hardcoded structure sizes.** `GetStartupInfoW` zeroed 104 bytes, which is
`STARTUPINFOW` on x86-64 and 36 bytes too many on i386 — enough to erase the
caller's own return address. `CRITICAL_SECTION` (40 vs 24), `SYSTEM_INFO`
(48 vs 36), `SLIST_HEADER` (16 vs 8) and `CONTEXT` (1232 vs 716) were all the
same shape of mistake. These are now declared as structs so `sizeof` produces
the right number per ABI, which is also simply more readable.

**3. Stubs that must not be stubs.** The arity warning above pointed at
`InterlockedIncrement`/`Decrement`, which mingw provides as intrinsics so the
table cannot cover them. They were being stubbed to return 0 — and callers use
them for reference counts, so every release looked like the last one. Arity was
the least of it. The whole family is now implemented with GCC atomics.

The same warning trail led to `GetVersionEx`, which was stubbed to return 0
(failure). A plugin that version-checks in `DllMain` reads that as "cannot run
here" and refuses to initialise, which presents as a bare `DllMain failed` with
nothing to suggest a version check. Both fixes apply to the 64-bit host too.

**4. Oversized writes into caller buffers.** `dwfa_GetMetrics` zeroed 24 bytes
of a `DWRITE_FONT_METRICS`, which is ten 16-bit fields — 20 bytes. The extra 4
landed on whatever followed, and on i386 that is exactly where MSVC keeps the
`/GS` stack cookie, so the plugin killed itself with `__fastfail` on the way out
of the function that asked for the metrics. Same shape as the `STARTUPINFO` bug
above, and it was in the 64-bit build too.

**5. Truncated channel counts.** Both hosts clamped the channel count to a
fixed maximum and then passed the plugin an array of that length. A drum machine
declaring 16 outputs writes to all 16 regardless, so anything past the cap was
an out-of-bounds write. The arrays are now sized to whatever the plugin
declares.

**6. Variable-length arrays at the end of a struct.** `VstEvents` declares
`events[2]` but is really variable-length, and the pointer array must be
contiguous with the header. Writing them into a second array declared after the
struct puts the first entry at `events[2]`, so `events[0]` and `events[1]` stay
NULL and the plugin sees two empty slots — no notes. Both 32-bit users had this;
`VSTEVENTS32_BYTES()` now sizes one contiguous block, as the 64-bit path always
did. Nothing to do with width, just a bug in new code.

**7. Stack alignment.** The i386 Windows ABI guarantees only a 4-byte aligned
stack; GCC assumes 16 and freely emits `movaps` against stack slots. So the
first time guest code called a stub that spilled an XMM register — ours, or
FreeType's deep inside `FT_New_Memory_Face` — it took a general-protection
fault. `-mincoming-stack-boundary=2` tells GCC the truth and it realigns where
needed. x86-64 needs nothing here: both ABIs already guarantee 16.

None of these were i386 bugs. i386 just charges for them immediately — which
made it a decent auditor of code that had only ever run at one width.

The COM shim has the same hazard and no import library to check against, so
`tools/dwrite_slots.py` derives each DirectWrite method's slot and i386
argument size from mingw's `dwrite*.h` C vtable structs. All 33 implemented
methods check out.

### Cross-checking the two loaders against each other

Every 32-bit plugin here has a 64-bit twin, which makes a useful test: render the
same note through both and compare.

**31 of 35 come out bit-identical.** The four that do not are `fbphaser` (max
sample delta 19/32768), `freqshifter` (127), `modulair` (158) and `grainstrain`
(16360). All four are deterministic run-to-run at a fixed width, so this is not
randomness — it is the two DLLs being separate compilations, where the i386 build
carries x87 intermediates the x86-64 build does none of. The loader has no say in
a plugin's own arithmetic; if it were feeding bad data, far more than four would
diverge.

## Diagnosing a guest fault

`pe32.c` installs a handler that reports faults as `image+offset (section)`,
walks the `%ebp` chain, and — when the frame chain is gone — sweeps the stack
for words pointing into the image. Feed the offsets straight to objdump:

```sh
objdump -d --start-address=$((0x10000000+0x4e0975)) \
           --stop-address=$((0x10000000+0x4e09a0)) plugin.dll
```

`eip 0` with `ebp 0` means a `leave; ret` off a zeroed frame — look for
something that overwrote the guest's stack, not for a null function pointer.
`PELOAD_VERBOSE=1` logs image layout, import resolution, TLS, and every
`LoadLibrary`/`GetProcAddress`.

One trap worth knowing: returning NULL from `LoadLibrary` is not the safe
default it looks like. The MSVC CRT probes for optional APIs with
`LoadLibraryExW(L"api-ms-win-core-fibers-l1-1-1")`, falls back to
`LoadLibraryExW(L"kernel32")`, and then calls `GetProcAddress` on the result
*without checking* — because on Windows that second call cannot fail. Synthetic
per-DLL handles plus a `GetProcAddress` that answers from the stub table turn
those probes into hits.

## Building

```sh
cmake -S peload -B peload/build -DCMAKE_BUILD_TYPE=Release && cmake --build peload/build
```

`peload32` needs a `-m32` toolchain (`lib32-glibc`) and `lib32-freetype2`; the
editor window additionally needs `lib32-libx11`, and live playback
`lib32-pipewire` with `lib32-alsa-lib`. Each is detected and the feature is
dropped rather than breaking the build. `pestudio` needs Qt6, PipeWire and ALSA.

```sh
./dw pe                      # Qt6 window: browse, play, plugin GUIs
./dw peload   <plug> --render out.wav
./dw peload32 <plug> --play --editor   # 32-bit: GUI, audio, USB keyboard
./dw peload32 <plug> --editor-png out.png   # capture the GUI, no display
```

## Patches

A session was only reproducible by clicking it back together: `pestudio` took a
directory to browse and nothing else, so there was no way to say *open this
plugin, in this state*. A bank now carries both — the patches and the plugin
they are for — so handing someone a JSON file is the whole instruction:

```sh
pestudio patches/rpg-menu.json                    # FB-7999, 7 patches listed
pestudio patches/rpg-menu.json --pick menu-open   # ... starting on that one
peload   patches/rpg-menu.json --list-patches
peload   patches/rpg-menu.json --pick cancel --note 72 --render cancel.wav
peload   fb799964.dll --save-patch mine.json      # author one
```

Opened this way the window grows a **Patches** list beside Programs; clicking a
name applies it. Without a bank the window is exactly as it was. `--pick` takes
a name or an index in both front ends, because a bank is read by people and
driven by scripts and those want different handles on the same thing; an
unmatched name prints the ones that do exist rather than falling back to the
first.

Whether the positional argument is a bank, a plugin or a tree to browse is
decided by the file: `.json` is a bank, and for the rest `pehost_can_load`
answers rather than a flag or an `isdir` test — a macOS plug-in is a bundle and
so is a `.vst3`, so being a directory settles nothing.

A patch is JSON keyed on the plugin's own parameter *names*, which is what makes
it diffable, hand-editable and robust against the plugin being rebuilt with a
parameter inserted in the middle:

```json
{
  "plugin":     "FB-7999",
  "uniqueID":   "0x66623739",
  "pluginPath": "../../windows/VST2-64/fb799964.dll",
  "patches": [
    { "name": "cursor",  "params": { "VCF Cutoff": 0.714, "VCA EG Decay": 0.097 } },
    { "name": "confirm", "params": { "VCF Cutoff": 0.635, "VCA EG Decay": 0.258 } }
  ]
}
```

`pluginPath` is resolved relative to the bank file, so a bank kept beside its
plugin travels with it. `--save-patch` nonetheless writes it absolute: what it
is handed is relative to the *caller's* working directory, and writing that
verbatim read back against a different base — saving a patch into another
directory produced a path pointing nowhere, which is exactly the case that
motivated recording the path at all.

A file holding a single patch writes `params` at the top level with no `patches`
array and reads back as a bank of one, so both shapes go through one code path.

Four things this format had to get right, each of which was a real case rather
than a hypothetical one:

- **The program comes first.** Selecting a program makes the plugin overwrite
  every parameter with that program's values, so applying one after the
  parameters silently discards the entire patch. Both front ends order it that
  way, and `pestudio` additionally has to move the program list's selection with
  the signal blocked, because that handler dispatches `effSetProgram` again.
- **Names are not unique.** FB-3300 gives 183 of its 233 parameters a name that
  some other parameter also has; `whispair` 61 of 157, `qyooo` 40 of 164. Those
  are written as `"15": 0.19` — the index — which is unambiguous in both
  directions. Names are still resolved first, so a plugin with a parameter
  genuinely called `12` stays addressable.
- **A patch loaded into the wrong plugin is reported, not refused.** Moving one
  between two builds of a plugin is reasonable and the unmatched names are
  simply skipped, but `uniqueID` is recorded so it can say
  *"patch is for 0x66623739 -- FB-7999, this plugin is 0x66383030 (Fury800)"*
  instead of quietly scattering values across whatever happened to share a name.
- **Queued writes have to be flushed before they are read back.**
  `pehost_set_param` defers to the audio thread, which is right when one is
  running. But `--patch X --save-patch Y` has no audio thread at all, so the
  values would never land, and the 32-bit bridge is worse: the helper applies
  its ring only as it renders. `pehost_flush_params` covers the in-process
  queue; the CLI renders one block after a patch, which is the one mechanism
  that works across a process boundary too and is safe precisely because the CLI
  is single-threaded.

- **A patch that sets only some parameters inherits the rest.** Switching from a
  patch with the noise generator up to one that never mentions it leaves the
  noise up, so a bank meant to be clicked through in any order has to write the
  same full key set in every entry. `patches/make_rpg_menu.py` does that and says
  why; it is the difference between a bank that is browsable and one that is
  only correct read top to bottom.

Verified by round-trip: save, load, save again, and compare. Identical for
FB-7999 (69 parameters, 9 of them index-keyed) and for Surge XT as a native
Linux VST3 (2855 parameters). A bridged 32-bit `fb7999.dll` reads back the same
four values as the 64-bit build of the same plugin.

### patches/all-machines.json — one file, every synth

A bank normally names one plugin. Opening thirty-one files to hear the same blip
on thirty-one synths is the wrong shape for the question, so a **patch** may name
its own `pluginPath` too — and then selecting it means *load that synth, then
apply this*:

```sh
pestudio patches/all-machines.json        # 218 patches across 31 machines
```

Stepping down the list plays `cursor` on every machine in turn, then `confirm`
on every machine, because it is grouped by sound. `make_all_bank.py --by-plugin`
writes the other ordering, which is what you want for auditioning one synth
rather than comparing many. Where a hand-written bank exists it supersedes the
generated one, so FB-7999 and Tricent contribute their better patches.

`patch_bank_is_multi_plugin()` tells a one-machine bank from a cross-machine one,
and the two orderings are generated rather than hand-maintained.

### Patches override programs completely

A program and a patch are not the same thing and they do collide. A **program**
is the plugin's own preset, baked into the DLL — FB-7999 ships 64 of them — and
selecting one makes the plugin overwrite *every* parameter it has. A **patch** is
a JSON file that sets parameters by name afterwards. Three conflicts, all real:

- **Order decides who wins.** A program change applied after a patch discards
  the patch entirely. `patch_bank_apply` therefore applies `program` first and
  the parameters second, and the GUI moves the Programs selection with its
  signal blocked so it follows rather than re-firing `effSetProgram`.
- **Clicking a Program still discards the patch**, by design — you asked for the
  program.
- **A partial patch inherits the rest from whatever program is selected.** This
  was the subtle one: FB-7999's `cursor` rendered peak 0.1137 from program 0 and
  0.1294 from program 40 — different files, same patch. A patch listing 38 of 69
  parameters is not a sound, it is a modification of whichever program you
  happened to be on.

So every patch is now filled out to the plugin's **whole** parameter set and
pins `"program": 0`. Selecting one overrides the program outright. Verified on
Stigma (235 parameters): the same patch from programs 0, 20 and 55 renders
byte-identical.

FB-7999 keeps one residual worth naming honestly: starting from program 0 versus
any other still differs over the first 45 ms, because a program *change* resets
oscillator phase and re-selecting the same program does not. It is phase only —
peak 0.1136, length 35 ms, RMS 0.00610 and centroid within 2% either way, so the
same sound with a different first cycle. No parameter can reach it.

The cost is size: a bank is now the full parameter set per patch, so
`patches/menu/` is 1.0 MB and `all-machines.json` 930 KB. That is the price of a
patch meaning one sound rather than one diff.

### The patch list follows the plugin

Switching synth in the Plugins list switches the Patches list with it, so what
is offered always belongs to what is loaded. Three rules, in order:

1. **The opened bank wins for its own plugin.** Opening `rpg-menu.json` and
   landing on FB-7999 gives its 38-parameter hand-written patches, not the
   17-key generated ones.
2. **A cross-machine bank is filtered.** `all-machines.json` holds patches for
   all 31, and only the loaded plugin's are listed — otherwise selecting one
   silently reloads a different synth and the list stops describing what is on
   screen. A patch naming no plugin belongs to whatever is open, which is what
   keeps an ordinary single-plugin bank working unchanged.
3. **Otherwise one is looked for on disk**, beside the bank that was opened:
   `<stem>-menu.json` next to it, or under `menu/`. So opening the FB-7999 bank
   and clicking TAL-U-No-62 brings up `TAL-U-No-62-menu.json` — a plugin with no
   bank anywhere shows *"Patches — none for this plugin"* and nothing is applied.

The selected **row** is kept across the switch, not the patch name. Every bank
here carries the same sounds in the same order, so staying on row 2 means staying
on `cancel`, and stepping down the Plugins list plays the same sound on each
machine in turn — which is the comparison this was all for. Matching by name
would have needed the `"cursor  [Blooo]"` suffix parsed back off, and the row
already carries the meaning.

Two things this needed that a single-plugin bank did not:

- **Which patch was picked decides which plugin to open.** `--pick` was resolved
  *after* the plugin, which is harmless when a bank names one plugin and wrong
  the moment it names several -- every pick opened whatever patch 0 wanted.
- **Paths have to be compared canonically.** A bank's path is resolved against
  the bank file and arrives full of `..`, while the loaded path is already
  absolute, so a plain string compare said "different" for the plugin that was
  already open and the startup patch reloaded the synth it had just loaded.

### patches/menu/ — the same seven sounds on 31 synths

`patches/make_menu_banks.py` generates one bank per Windows 64-bit synth, all
carrying the same patch names, so the same UI sound can be heard on every
machine in the collection:

```sh
pestudio patches/menu/fury80064-menu.json
peload   patches/menu/stigma64-menu.json --pick error --render error.wav
```

Hand-authoring thirty of these is not on, and the plugins agree on almost
nothing — the amplifier envelope alone is `VCA EG Attack` (FB-7999), `AMPATTACK`
(TAL-U-No-62), `AmpEnv A` (Kern, Ragnarok 2), `Loudness Attack` (brokenmini),
`Out Attack` (ModulAir), `DEG2 Attack` (Fury800), `P1 EnvA` (Stigma) and
`OP1: Attack` six times over (FB-02). So the generator maps *roles* onto each
plugin's own names.

The one decision that made it work: **it does not try to find the amplifier
envelope.** Guessing which of `Env 1` and `Env 2` is the amp is wrong about half
the time and fails silently, as a bank that sustains forever. It shortens *every*
envelope the plugin exposes instead — which is what a blip wants anyway, and on
an FM synth setting all six operator envelopes short is exactly right.

`check_all.py` renders all 217 patches and reports what actually came out; that
table, not the mapping, is what says whether a bank is any good. It found four
real faults, and each one is now a comment in the generator:

- **Six-stage envelopes need more than ADSR.** The DW-8000's has attack, decay,
  *break point*, *slope*, sustain, release. Leaving the two extra stages alone
  left FB-7999 ringing until note-off — 7/7 patches held at ~1400 ms — while its
  four-stage siblings decayed properly. Zeroing them dropped it to 30–485 ms and
  fixed Fury800 the same way (1340 ms → 20–95 ms).
- **Raising the master clipped four banks.** At 0.9 they hit 0 dBFS flat; at 0.65
  MonoFury still did, because its `Volume` ships at exactly 0.5 and raising it
  was the whole problem. A master is now only ever lifted *to* 0.5 and only if
  the plugin left it lower.
- **An effect name has to lead, not merely appear.** NY is a preset machine whose
  sections are called `Volume Ensemble` and `Volume Chorus`, so a pattern ending
  in "ensemble" or "chorus" switched off the very things that make it sound —
  the whole plugin went silent.
- **The checker only read the left channel.** NY pans its sections across the
  stereo field, so a patch whose audio sat in the right measured as silence and
  a working bank was written off as dead.

#### A different program under every patch

Filling each patch out to the plugin's whole parameter set made them
deterministic, and identical. The recipe only moves envelopes, cutoff and
resonance — measured, that is **0–25% of a plugin's parameters**, 1 of 229 on
FB-3300 and 3 of 591 on ModulAir — so seven patches came out as one sound with
seven envelopes. They read as the plugin's default, because that is what they
were.

Adding oscillator roles would not fix it: only 7 of 39 plugins expose an octave
by a matchable name and 8 a waveform. So each patch instead takes a **different
factory program as its base**, spread across whatever the plugin ships, and the
recipe shortens that into a blip. No per-plugin knowledge required — the
programs are the vendor's own sound design. Parameters varying between the seven
patches went from a mean of **10% to 55%**; FB-3300 from 1 of 229 to 144.

That is a blind choice, though, and it cost seven banks their clean bill of
health: some presets are silent at the note the patch plays, others far hotter
than program 0. `repair_banks.py` renders every patch and fixes the failures —
trimming the master when one clips, swapping the program or re-pitching it when
one is silent. Of nine failures, **seven turned out to be pitch rather than
preset**: BucketPops maps drums to roughly 36–60 and the recipe was playing it at
67–84, so six of its seven patches produced nothing under *any* program.

#### Making a patch sound like its name

`repair_banks.py` only asks *does this make a sound*. That keeps a bank honest
and says nothing about whether `equip` is the clink it claims to be. Measured
across all 31, it was not: written as "the weighty one" at decay 0.40 and note
72, it ran **300–2000 ms on 27 of 31 machines** — a chime, or a pad.

Redefined as what a clink is (instant attack, decay 0.10, filter open at 0.82,
resonance 0.38 to ring, up an octave) and re-measured, three passes were needed
and each caught something the last missed:

1. Retuning got 23. But the verdict only tested length and brightness, so an
   Oxid patch at **−90 dBFS** passed as a clink. It was inaudible.
2. Adding a level test, and raising the silence floor from −60 to −44 dBFS
   (a patch at −49 is not audible over music), got 24.
3. `optimize_patch.py` got 27. Where the recipe cannot reach an amplifier
   envelope, shortening does nothing — but the *factory programs* include
   percussive ones, and a program that is already short and bright needs no
   shortening. Scoring candidates on length, brightness and audibility fixed
   Nabla (1340 ms → 30 ms at 7699 Hz), Ragnarok 2 (198 Hz → 3924 Hz) and
   BucketPops.

Both hand-written banks too: FB-7999's equip went 640 ms → 25 ms at 5023 Hz,
Tricent's 265 ms → 20 ms. **29 of 33 are now a clink**, and the four that are not
say why: FB-3300 at 270 ms is over an arbitrary line rather than wrong, Oxid is a
string machine with no percussive preset to find, FM8 exposes macro slots rather
than parameters, and FB-02's six operator envelopes leave no carrier when zeroed.

`retune_patch.py` reworks one sound across every bank without discarding the
program and note choices the repair passes measured their way to.

Result: **30 of 31 banks fully sounding and clean, none clipping, none silent.**
The two partials are honest ones — `bucketpops` is a drum machine (1/7: only the
note that lands on a drum sounds) and `fb0264` is FM (3/7). Nine plugins get no
bank: five are effects, NI Massive will not load, NI Kontakt 5 exposes only
`#000`-style slots, drumtraqs has no envelope at all, and brokenmini renders
silence at both widths even with Power, Fuse and Warm-Up on and a 15-second
render — which is what this README already said about it.

Every bank records its own `mapping` — which parameter each role bound to, and
what was silenced — so when one sounds wrong that is the first place to look.
Hand-editing is expected; `rpg-menu.json` and `tricent-menu.json` are what the
hand-written version of the same idea looks like, and both are better than their
generated counterparts.

### patches/tricent-menu.json

Eight UI sounds for **Tricent MK III** (Korg Trident, uniqueID `Tri3`), from
`patches/make_tricent_menu.py`.

Not the FB-7999 bank transposed. The Trident has no Auto Bend, so the pitch
sweep that made `confirm` and `cancel` one another inverted is unavailable; what
it has instead is three sections sounding at once, each with its own filter,
envelope and output switch — so the sections became the vocabulary. **Synthe**
(two VCOs, resonant VCF, ADSR) gives `cursor`, `confirm`, `cancel`, `error`;
**Brass** gives `equip`; both together give `fanfare`; **Strings** gives
`menu-open` and `menu-close`.

Two things this plugin taught that the DW-8000 did not:

- **Its envelope displays are useless for tuning.** They render as an integer,
  so every value below 1 reads `0`. The times had to be found by rendering and
  measuring, which is what `patches/check_tricent.py` does — it works on any
  bank, not just this one.
- **A held note sits over a rising low-level residue** that reaches about
  −52 dB. A "−40 dB below peak" length metric latches onto that and reports the
  note-off time instead of the decay, which is how the same patch measured 41 ms
  at `--secs 2` and 2667 ms at `--secs 4`. The check script uses −30 dB below
  peak over 5 ms windows and says so.

A third caught four patches: at the section volumes that suited FB-7999 this
plugin **clipped flat at 0 dBFS**. The bank runs at `Total Volume` 0.55 (−6 dB)
with sections near 0.46, landing at −18 to −6.5 dBFS. `menu-open` and
`menu-close` are reported as *holds while key down* rather than a length,
because the Strings section has no sustain stage — it sounds while the key is
held, which is correct for a string machine and would otherwise read as a patch
that forgot to stop.

### patches/rpg-menu.json

Seven UI sounds for FB-7999 — `cursor`, `confirm`, `cancel`, `menu-open`,
`menu-close`, `error`, `equip` — generated by `patches/make_rpg_menu.py`, which
is the file to edit; the JSON is its output.

The DW-8000's **Auto Bend** is why this synth suits the job: a per-note pitch
sweep as four parameters, so "confirm" and "cancel" are one another with
`Auto Bend Mode` flipped rather than two separate sound designs. Every value is
written as `step/max`, and the maxima were measured by setting a parameter and
reading the plugin's own display back rather than assumed — 0..31 for most,
0..63 for cutoff, 0..15 for waveform and delay level, 0..7 for delay time.

Waveform choices came from measuring the spectral centroid of all 16 at A4:
16 is the darkest at 911 Hz and gets `cancel`, 10 the brightest at 3116 Hz and
gets `cursor` and `error`, 7 sits mid at 1484 Hz for `confirm`. Rendered, the
set runs 35 ms (`cursor`) to 770 ms (`equip`) to −40 dB, peaking −19 to −10
dBFS — left at different levels on purpose, since a cursor tick should sit under
an equip chime rather than match it.

`peload32` has no `--patch`: `pe32.c` is a self-contained loader with its own
host rather than a user of `pehost.h`, so the shared `patch.c` does not reach
it. 32-bit plugins loaded *through `pestudio`* go over the bridge and are
patched like any other, so this only costs the standalone i386 CLI.

## Hardening the loaders

Both loaders took every size straight from the file. That is fine for a
well-formed plugin and an overflow for anything else, and a truncated download is
the realistic case:

- **PE**: `e_lfanew`, `SizeOfOptionalHeader`, `NumberOfSections`, `SizeOfImage`,
  `SizeOfHeaders` and every section's `PointerToRawData`/`VirtualAddress` +
  `SizeOfRawData` are now range-checked against both the file and the mapped
  image. A truncated DLL used to `memcpy` past the end of the mapping; it now
  reports `section 0 raw data runs past the end of the file`.
- **Mach-O**: the source bound was already checked; the destination was not.
  `filesize` is independent of `vmsize` in the file, so a segment claiming more
  file bytes than virtual space wrote into whatever followed it. `vmaddr +
  vmsize` can also wrap, which would produce a span too small for the addresses
  it was computed from.

## Not done

- **macOS Audio Unit editors.** An AU builds its view through a separate
  factory (`kAudioUnitProperty_CocoaUI`) rather than `effEditOpen`, so the same
  Metal backend is not wired to them yet. Their audio works.
- **Component Manager Audio Units.** Ragnarok's `.component` exports
  `_Ragnarok_Entry`, the pre-AudioComponent entry point --
  `ComponentResult(ComponentParameters *, void *)` -- which is a different ABI
  from the factory the AU host speaks. It now says so and declines instead of
  calling it as a factory and crashing on the garbage it returns. The VST2 build
  of the same plugin works, so nothing is actually lost.
- **One macOS editor at a time.** `macmetal.c` keeps a single CAMetalLayer, so
  the host can find the framebuffer without being handed a pointer. `pestudio`
  hosts one plugin at a time, so this costs nothing today; hosting two at once
  would need the layer keyed per plugin.
- **Objects are retired, not freed.** With no autorelease pools and no weak
  references, a plugin's retain/release traffic cannot be balanced faithfully,
  and an over-release hands the allocator a block that is still live. Reaching
  zero therefore poisons the header and leaks the memory, which turns an
  unbalanced release into a no-op instead of a corrupted heap. The cost is that
  a plugin's objects accumulate while its editor is open.
- **Out-of-process hosting for 64-bit plugins.** The bridge above gives 32-bit
  plugins crash isolation as a side effect of needing another process at all.
  A 64-bit plugin still runs in-process, so `TAL-U-No-62` still takes the host
  down. Routing those through the same helper would fix it, and the protocol
  already fits — it just needs a 64-bit build of the server side.
- **CLAP (5 present) and LV2 (43 present) are not implemented.** CLAP is a clean
  C ABI and would be a small host; LV2 needs Turtle parsing, so it wants lilv.
- **macOS VST3 (18 bundles) is not hosted.** They bind, but nothing drives the
  VST3 API against a Mach-O image yet -- the two halves exist separately.
- **SEH.** `__try` regions work only insofar as nothing throws; real unwinding
  means parsing `.pdata`/`.xdata`.

Plugins that write settings do so through the file stubs without path
translation, which is why `\FullBucketMusic\*.ini` appears in the working
directory — backslashes and all — rather than under a mapped `%APPDATA%`.
