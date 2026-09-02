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
| macOS VST2, x86-64 | `peload` | 31 | 31 render, 30 show and drive their own GUI |
| macOS VST3, x86-64 | `peload` | 18 | 18 render, 18 show and drive their own GUI |
| macOS Audio Units, x86-64 | `peload` | 50 | 41 render, 18 of the 18 with a Cocoa view drive it |

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

## Audio input, and what a vocoder needs

Everything an effect was ever fed came from inside the host: silence, a
sawtooth per held key, or noise. That is enough to tell whether a compressor is
working and no use at all for Full Bucket's vocoder, which wants a real voice on
its modulator input. Both windows opened their audio streams `PW_DIRECTION_OUTPUT`
and there was no capture path anywhere.

`pestudio` now opens a second PipeWire stream the other way. The two streams are
not called in lockstep, so a small lock-free ring carries what one writes to the
other; when the reader is behind, the oldest samples are dropped rather than the
newest, because a vocoder wants the current word and not a growing delay. Short
of a whole block the remainder is silence rather than the previous block again --
a stutter of the last five milliseconds of a word is worse than a gap.

The devices come from PipeWire's registry, which is the only thing that knows
about them, through a short-lived connection of its own rather than reaching
into the one the audio thread is using. Audio/Source nodes, listed with their
descriptions, refreshed on demand: plugging in a USB interface is exactly when
the list is wrong, and rescanning the graph on a timer would be rude. Settings
grows an **Audio input** box beside the MIDI one -- a device menu, a Rescan
button and a line saying whether anything is actually arriving, because a device
that is connected and silent and one that is not connected look identical until
you count frames.

`Effect in` gains a fourth choice, `input`, alongside silence, keys and noise.

Verified against the machine's own USB headset: the enumerator finds
`alsa_input.usb-GN_Netcom_A_S_Jabra_EVOLVE_LINK_...` as "Jabra EVOLVE LINK
Mono", selecting it connects the stream to that node, and frames arrive at
exactly 48,000 a second with a peak of 0.225 -- a real signal rather than
digital silence.

A four-input plug-in like FBVC gets the signal on every input: the de-interleave
is `c = k < 2 ? k : k % 2`, so both its carrier and its modulator pairs are fed.
With FBVC's own Keyboard mode on it generates its carrier from MIDI, which is
what the VC-10 it models did, so a USB keyboard for notes and a microphone for
the voice is the whole arrangement.

`dwstudio`, the GTK window, is still output-only.

## No 32-bit editor ever appeared, and the reason was patience

Every 32-bit Windows plug-in reported "editor produced no pixels". Not some --
all of them, every time, while their 64-bit twins drew perfectly through the
same helper protocol. A failure that uniform is usually one thing, and it was
not the editors: `peload32 --editor-png` on the helper alone writes fb-3200's
editor out at 1256x520, 98.8% painted. The pixels were being drawn and were not
arriving.

The buffer is guarded by a sequence lock. The writer publishes a frame and
bumps a generation; the reader takes the generation, copies, and checks it did
not move, treating an odd value as "a write is in progress". Two things were
wrong with that.

`serve32.h` only incremented *after* the copy, where `peserve.c` -- the 64-bit
helper, same protocol -- increments on both sides of it. So the 32-bit
generation was never odd during a write and never said anything at all; that is
now fixed, and it was worth fixing, but it was not the reason.

The reason is that the reader gave up too early. Eight spins with a
`sched_yield` between them is a shorter budget than the writer's critical
section: publishing a 1096x586 editor is a two-and-a-half megabyte `memcpy`.
A reader that arrived inside one saw the same odd generation on all eight
attempts and returned "no frame" -- and on the first read after opening an
editor there is no earlier frame to fall back on, so that reached the host as
"editor produced no pixels". Traced from both ends at once, the helper's
counter went 280, 282, 284 and the host read 285.

A publish takes well under a millisecond and they are sixteen apart, so a
reader that lands in one is never waiting long. It now spins eight times and
then sleeps in tenth-millisecond steps, bounded at about five milliseconds.

35 of the 40 32-bit plug-ins draw their editors now, where none did. Of the
rest, four are the Native Instruments plug-ins, whose 32-bit builds fault inside
the helper while loading -- reported, not fatal, because that is what the helper
is for -- and `brokenmini` refuses its own editor over a font, as it does at 64
bits too.

## Switching plug-ins all session: what ran out

Three things in the Win32 layer counted upward and never came back down, so a
browsing session degraded with use rather than failing outright. All three were
found the same way -- load every plug-in in the corpus, in one process, several
times over, and after each one ask whether its editor still answers the mouse.

**The image registry, at nine.** A base and its resource directory travel
together, because an RVA means nothing without knowing which image it is
relative to. Entries were added on load and never removed on unload, and
`winstubs_add_image` ran off the end of its loop in silence when the table was
full. Eight plug-ins in, the table held eight bases that no longer existed; the
ninth plug-in's image was never registered, and every resource it asked for was
answered from a predecessor's directory in memory that had been handed back to
the kernel. What that looks like is an editor that gradually stops responding --
a control whose bitmap comes back empty draws but does not behave -- and then a
fault. `pe_module_unload` drops the entry now, and a full table says so.

**Thread-local slots, at about eighty.** `TlsFree` accepted its argument and did
nothing, and the allocator only ever counted upward, so every slot a plug-in's
runtime took was gone for the life of the process. There are 120 to spend and a
plug-in spends one or two. Around the eightieth load `TlsAlloc` starts returning
`TLS_OUT_OF_INDEXES`, the Microsoft runtime's `DllMain` fails on that, and from
then on every plug-in is reported as "DllMain failed" -- the host refusing good
plug-ins because of what it had already spent on their predecessors. There is a
free list now, and losing the plug-in's image reclaims whatever it still held.

**A missing C++ runtime, immediately.** A plug-in importing `MSVCP120.dll` with
no copy to be found had every one of those imports stubbed and was then allowed
to run. A stub is a fair answer for one missing entry point and a terrible one
for a whole C++ library: the plug-in loads with several hundred of them and
faults on the first constructor it calls. That is now a refusal that names what
is missing, which is the difference between one plug-in failing to open and the
session ending.

Two configurations, eight rounds, every 64-bit Windows plug-in in the corpus:

| | loads | crashes | RSS |
|---|---|---|---|
| isolated, as `pestudio` runs | 320 | 0 | 7.4 MB -> 12.3 MB, then flat |
| in process, `PEHOST_ISOLATE=0` | 304 | 0 | flat |

The isolated column is the one that matters for a session: `pestudio` and
`dwstudio` both host out of process by default, so a plug-in that faults in its
own code costs a subprocess and is reported, and the helper exits with the
plug-in it was hosting. The in-process column is what the command-line tools and
the helper itself use, and is where all three of these lived.

One thing is measured and deliberately not fixed. Hosting in process, the
resident set grows about 790 KB a plug-in and does not level off. Mappings stay
flat and the descriptors stay flat; the growth is the plug-in's own `HeapAlloc`
memory, which Windows reclaims by destroying the process heap and which here
outlives the plug-in that asked for it. Reclaiming it means tracking every block
the plug-in allocates, and plug-ins mix `HeapFree` with the C runtime's `free`
on the same pointers -- so a table that misses one path frees a block twice at
unload, on the allocator the audio thread is using. A leak in a configuration
that is not the default is a better outcome than that, and the isolated path
does not have it: 312 loads at 12.8 MB, flat.

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

## Classic editors: what the corpus actually needed

The first Classic editors drew their background and nothing else, and a corpus
capture (`peload <plugin> --editor out.ppm` over all 57 `.vstclassic` files)
showed why. The Destroy FX editors -- the bulk of the corpus -- draw text over
everything: labels, value readouts, the whole informational page the "Food"
plugins show instead of controls. None of the text calls existed, so the Food
editors came out as one flat rectangle of background colour.

The fix, in order of how many plug-ins it unblocked:

- **Text.** `DrawString`/`DrawText`/`StringWidth`/`TextWidth`/`CharWidth`,
  `TextFont`/`TextFace`/`TextSize`, `GetFontInfo`, `c2pstr`/`p2cstr` -- backed by
  a small proportional bitmap font embedded in `macfont.h` (rendered from DejaVu
  Sans at 9px and 13px; the corpus asks for 9-12pt almost everywhere). The font
  is only approximately Geneva, but `StringWidth` and `GetFontInfo` report the
  same metrics the drawing uses, so right-aligned labels land where the plug-in
  intended. Mac Roman's curly quotes and dashes fold to ASCII.
- **Pattern fills.** `FillRect`'s second argument is an 8x8 Pattern, and the
  Classic way to shade a background is a dither between two colours built inline
  with `StuffHex`. Filling solid with the fore colour instead is what flattened
  every shaded panel.
- **Rectangles and regions.** The full `Rect` algebra (`SectRect`, `InsetRect`,
  `OffsetRect`, `PtInRect`, ...), and regions as exactly what a Classic
  rectangular region is on disk: `{ rgnSize = 10; rgnBBox }`. The corpus clips
  only to rectangles, so the clip region is a rect per port, honoured by every
  plotting path.
- **Pen state.** `PenSize`, `PenNormal`, pen modes (copy/or/xor/bic),
  `GetPen`, `Move`/`Line`.
- **TextEdit, single-line.** The dfx numeric-entry fields: a real `TERec`
  layout in guest memory, `TESetText`/`TEGetText`/`TEKey`/`TEClick`/
  `TESetSelect`/`TEUpdate`, drawn with the same text stack, with a caret and an
  inverted-selection highlight.
- **Miscellany.** `tan`/`atan`/`acos`/`asin`/`nan`/`dec2num`/`__fpclassifyf`,
  `GetDateTime`, `GetNextEvent`/`WaitNextEvent` (no event is ever pending, but
  the host gets pumped), `Delay`, `SetOrigin`/`GlobalToLocal`/`LocalToGlobal`,
  `InvalRect` and friends (the host redraws everything anyway), and Internet
  Config declining politely.

What did not get fixed, with reasons:

- **Audio Damage's Mayhem bundle** (Crush, Filterpod, MasterDestrukto,
  TimeFnk) reads a 40-byte `com.audiodamage.mayhem settings` file from the
  support directory at startup and declines to run without a valid one -- the
  registration record the Mayhem installer wrote. Existence is not enough; the
  content is validated. Nothing legitimate to synthesize, so these still refuse
  to load, and the error message says why.
- **mda Looplex and the dfx-dev block tests** import 230-290 Toolbox symbols
  apiece -- a VSTGUI-class surface (Window, Menu and Control Managers, the
  Printing Manager, AppleEvents, Unicode text encoding). Three plug-ins do not
  justify a third of the Toolbox.
- The other mda plug-ins have no editor at all and say so: eight to eleven
  imports, none graphical. "editor: none" is the correct answer for them.

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

## Nothing told the runtime about the plugin's own classes

Eighteen of the nineteen macOS VST2 editors here died on one line, and it was
always the same line:

```
$ tools/macsym.py MPS.vst 0x91c93
0x91c93      _nvgCreateImage+0x73
```

`nvgCreateImage` loads a file and then, at +0x73, dereferences the NVGcontext it
was given for the first time. The context was NULL, because
`nvgCreateContext` had failed, because `renderCreate` found no Metal device,
because `_metalLayer` was nil -- and it was nil because `[view setLayer:]` had
quietly dropped the layer on the floor.

The Foundation methods gate on `macobjc_isa_named(obj, "NSView")`, a question
that has to survive being handed things that are not objects at all, so it
believes only classes the runtime minted. Nothing had ever told it about the
image's own. `macobjc_register_image_classes` existed for exactly this, with a
comment saying so, and had no caller: `machoload.c` never looked for
`__DATA,__objc_classlist`. Every instance of a plugin-defined class therefore
answered "no, I am not an NSView", and every method that gated on it did
nothing and said nothing.

It is registered now, after binding (a class's superclass is an import, so
before `do_bind` it points nowhere useful) and forgotten again when the image is
unmapped. macOS VST2 editors went from 1 of 31 to 19 of 31 -- and Ragnarok, the
one that had been drawing, went from 0 pixels of 427680 to all of them.

Two things this cost that are worth naming. `register_class` used to drop
silently when its table filled, which would have reintroduced exactly this bug
one plugin at a time in a long browsing session; it now says so once. And
`macobjc_class` -- `NSClassFromString`, `objc_getClass` -- searched only the
runtime's own stand-ins, so a plugin asking for a class compiled into itself got
nil. That is what an Audio Unit's Cocoa view is.

## Audio Units build their editor through a factory

An AU has no `effEditOpen`. It answers `kAudioUnitProperty_CocoaUI` with the
*name* of a class inside its own bundle, and the host is expected to find that
class, build it, hand it the unit and take the `NSView` it returns:

```
MPS.component:  -[MPS_View uiViewForAudioUnit:withSize:]
```

Two things had to exist first. The class had to be findable by name, which is
the registration above. And `AudioUnitGetProperty` had to work: it is the call a
view factory makes to find the object behind the opaque `AudioUnit` handle it
was given, and `macshim_set_au_callbacks` -- which fills in that callback and
the two beside it -- had no caller either, so every such call returned "not
initialised". The handle the host hands out is now the `macau` struct itself,
recognised on the way back in by a magic word.

After that the editor is the same machinery a macOS VST2 uses: the plugin draws
into a layer this host owns, the software Metal backend hands back the pixels,
and the host fires the timers because there is no run loop. Eighteen of the
fifty Audio Units here name a Cocoa view; all eighteen draw. The other
thirty-two name none -- the Michael Norris spectral set expects a host to build
a generic parameter interface -- which is not a failure, and is now reported as
`editor: none` rather than as nothing at all.

## The editors drew, and could not be operated

Every macOS editor painted and none of them could be used. A drag moved
nothing, on any of the three backends, where the same gesture on the same
plugin's Windows build moved a slider from 0.843 to 0.802. Three separate
things were wrong, and each hid the next.

**Every click landed at `height - y`.** An NSEvent's `locationInWindow` has its
origin at the bottom left, because that is what Cocoa windows use, and a
*flipped* view -- which every plugin editor here is -- counts from the top.
AppKit does that conversion in `convertPoint:fromView:`, and this host's was the
identity. So the editor was mirrored vertically: a click meant for a slider at
230 arrived at 122, and mostly hit nothing at all. The flip is now the view's
own answer to `isFlipped` rather than an assumption, because a VSTGUI view says
no where an iPlug2 one says yes.

**There was no window.** `[view window]` answered nil, which had seemed
harmless -- nothing is ever shown. It is not harmless: grabbing a control makes
iPlug2 ask its window to convert the cursor's screen position into window
coordinates, and a message to nil returns zero. The mouse-down position was
stored as (0,0), so the first drag moved the control by the entire distance from
the top-left corner. A one-pixel drag took a slider from 0.43 to full scale, in
whichever direction it was dragged. There is a window now, and its origin is
placed to make `locationInWindow` mean what the event already said it meant --
flush with the top of a screen whose height `CGDisplayPixelsHigh`, `NSScreen`
and `[NSEvent mouseLocation]` all now agree on, because a plugin converts
between the two conventions and compares the results.

**The pointer is two positions, not one.** A plugin dragging a locked control
warps the cursor back to where the gesture started after every event, so that
what it reads next is the physical movement since then and nothing else. On a
real Mac the window server performs that warp and the next event arrives from
the new place; nothing warps an X pointer, so the host does it in software.
`CGDisplayMoveCursorToPoint` moves a virtual pointer, and each incoming event
advances it by however far the real one moved. Without that the plugin measured
every event against the point it had warped to, and a drag accelerated away and
reached the end of its range in five pixels -- smooth-looking, and unusable.

Measured on MPS, dragging its Vivid slider 40 pixels, the same gesture the
Windows build answers with an even 0.02325 per step:

| | steps that changed the value | trajectory |
|---|---|---|
| Windows VST2 | 15 of 15 | 0.4533 → 0.8021, evenly |
| macOS VST2 | 15 of 15 | 0.4533 → 0.8951, evenly |
| macOS Audio Unit | 15 of 15 | identical to the VST2 |
| macOS VST3 | 15 of 15 | identical to the VST2 |

The three macOS backends agree to the fourth decimal because they are the same
plugin reaching the same rasterizer by three different roads. The rate matches
Windows per pixel; the endpoints differ only because the two harnesses quantise
the sweep differently.

Three things were then checked rather than assumed, because "the number moved"
is not the same as "it works":

- **The picture follows.** Two clicks at different points on the same slider
  produce framebuffers differing in 2342 bytes, against 2299 for the Windows
  build of the same editor.
- **The sound follows.** A WAV rendered after the drag differs from one rendered
  before it, on all four backends. `peload` puts the render after the gesture
  when both are asked for, because that is the question a gesture is asked to
  answer.
- **It is fast enough to feel like dragging.** A drag frame on MPS's 608x352
  editor -- deliver the event, let the plugin redraw, rasterize -- costs a mean
  of 8.0 ms and a worst of 12.2 ms, inside `pestudio`'s 16 ms pump. Opening the
  editor costs 33 ms, and 200 ms for the largest here.

## The twelve editors that drew nothing

Twelve of the thirty-one macOS VST2 bundles build their interface with VSTGUI 3
rather than iPlug2 -- the whole Audio Damage side of the corpus, and four others
-- and not one of them drew a pixel. They are a single chain of missing pieces,
each one hiding the next, and the first is a long way from anything to do with
drawing.

**A plugin has to be able to find its own bundle.** VSTGUI's `InitMachOLibrary`
asks dyld which image its own code is in, walks the path up to the `.vst`, and
builds a `CFBundleRef` from it. Nothing here answered for dyld, so `gBundleRef`
stayed null and every resource lookup after it returned nothing. There is one
image, so `_dyld_image_count` is 1 and `dladdr` answers for it -- shadowing the
host's own, which knows nothing about a mapping made with `mmap` and would
happily describe whatever ELF happens to sit at that address.

**Then it has to read its own Info.plist.** The bundle's info dictionary was an
empty one, which is enough for a plugin that only asks whether a key exists and
nothing like enough for one that keeps data there. Audio Damage's do:

```
<key>FontrastInfo_20000</key> <string>tahoma9</string>
```

That names the bitmap font the editor draws its labels with. The parser is a
scan for `<key>` and the element after it, not an XML reader -- these files are
machine-written -- and it skips anything nested, so an `AudioComponents` entry
cannot shadow a top-level key.

**Then it opens that file through Carbon.** `CFURLGetFSRef` and
`FSOpenFork`/`FSGetForkSize`/`FSReadFork` -- the File Manager was already here,
but the door into it was not, and `CFURLGetFSRef` answering false meant the
whole branch was skipped. The field it would have filled is one the constructor
does not zero, so the object carried a garbage pointer that the first call to
`getCharacterInfo` dereferenced. Seven editors died there, in a font routine, a
long way from the question that had gone unanswered. The FSRef also stopped
carrying its path inline while this was being fixed: eighty opaque bytes is not
a path, and a truncated one opens nothing.

**Then it decodes its artwork.** A VSTGUI editor is several hundred images and
nothing else -- background, knob filmstrips, button states -- loaded through
`CGImageSourceCreateWithURL`. Nothing in this host had needed image *decoding*
before: the Metal path is handed pixels by the plugin and the Windows side has
its own DIB reader. So `peload/png_in.h` is a PNG reader, inflate included.

Deflate is implemented rather than linked, for the same reason the writer beside
it emits stored blocks: this host has no zlib dependency, and one added for a
plugin's artwork would have to be carried by every package. It is about two
hundred lines and it is checked rather than trusted --
`tools/regress.py`'s `png-decoder` builds thirty images covering every colour
type, interlaced and progressive, at sizes that leave awkward remainders in the
Adam7 passes, and compares every component against Python's own reader. The 551
PNGs the corpus actually carries were checked the same way. Four of the twelve
keep some artwork as Windows BMP instead, so that is read too, and the caller
sniffs rather than trusting the extension.

**Then it needs somewhere to draw.** AppKit focuses a context on a view before
calling `drawRect:`, and the view fetches it back with `[[NSGraphicsContext
currentContext] graphicsPort]`. There was no such thing here, so every editor
asked for its context, got nil, and dereferenced it a few instructions later.
It is a bitmap this host owns, which is also the editor's framebuffer.

**And the context needs a state.** This was the last one and the largest. The
transform and clip calls accepted their arguments and returned, which is fine
while the only thing drawing is a plugin compositing into a bitmap of its own at
the identity. A VSTGUI editor does not work that way: it clips to a control's
rectangle, translates the origin onto that control, flips the y axis, and draws
the whole filmstrip. Ignoring all of that put every bitmap on top of every other
one at the canvas origin -- the first editor to get this far drew three vertical
bands of stripes. With a real CTM, a real clip and a real save/restore stack, it
draws its interface.

One more piece belongs to the same session and is worth naming separately: the
info dictionary is a singleton this host hands out, and
`CFBundleGetInfoDictionary` is a Get -- the caller does not own it. A plugin
that releases it anyway took the host's only one with it, and the *next* plugin
then found an empty dictionary, looked up nothing, and asked its bundle for a
file called ".png". It is held forever now. That is the difference between one
VSTGUI editor working and two of them working, which is the difference that
matters to a browser.

Two things fell out that were not about VSTGUI at all:

- **An unimplemented selector with out-parameters is not safe.** A missing
  method returns nil and leaves the caller's own locals alone, so
  `getRectsBeingDrawn:count:` handed back a count and an array pointer that were
  whatever the stack happened to hold. With one set of stack contents the count
  came out zero and the loop was skipped; with another -- loading libc++ was
  enough to change it -- the count was large and the pointer was not an array.
  The editor's own drawing was fine either way.
- **An unresolved *data* import fails silently.** A function import binds to a
  stub that reports itself when called; a data import that nothing implements is
  left holding the image's own link-time value, and nothing says so. The plugin
  then puts that value in a dictionary and the fault lands inside `CFRetain`.
  `kCFBooleanTrue` was one. Rather than chase them one at a time, the two type
  tests that were reading through plugin-supplied pointers -- "is this a
  CoreFoundation object?" and "did this runtime allocate this?" -- now answer
  from a hash set of what was actually minted, with the link stored in the
  object's own header so there is nothing extra to allocate.

Result: **30 of the 31 macOS VST2 editors paint**, up from 19. The one that does
not is `model-e`, which is VSTGUI 4 driving a `.uidesc` layout through the
Resource Manager -- a different architecture, and the `Get1Resource` family is
not implemented. Its audio works.

## The editors drew upside down, and it took a dirty rectangle to notice

Eight macOS editors -- six of the eight Audio Damage plug-ins, `neon` and `vb1`
-- painted a picture that looked completely right and could not be operated. Automaton drew
its title, its sequencer grids, its tab strip and its logo, all the right way up
and all legible. Clicking any of them did nothing. Clicking a hundred and six
pixels *below* a button pressed it.

Two explanations fit that, and they are hard to tell apart from the outside:
either the clicks arrive mirrored, or the picture is drawn mirrored and the
clicks are fine. The picture argued for the first -- text was upright, panels had
their titles at the top, the brand was at the bottom, and a vertically mirrored
editor should have looked like none of those things. That reading was wrong, and
what settled it was asking the plug-in rather than looking at its output:

```
[invalidate] rect 33,293 37x38   (view height 523)
```

Automaton says the button it just changed is at y=293. It was appearing at
y=192, and 523 - 293 - 38 = 192. The plug-in's own account of where its controls
are is the one thing in the system that cannot be a matter of opinion, and it
said the drawing was mirrored. Text stayed upright through the mirror because
these editors draw text from a bitmap font -- each glyph is an image placed by
its own rectangle, so a flipped canvas moves the glyphs and does not turn them
over.

The cause is one line of AppKit that was not there. A Core Graphics bitmap
context has its origin at the bottom left. A *flipped* view -- `isFlipped`
returning YES, which is what these plug-ins' view classes do -- draws with y
counting down from the top, and AppKit arranges that by concatenating a flip
onto the context before it calls `drawRect:`. This host handed over the context
unflipped, so every rectangle the plug-in drew landed at `height - y` while
every click it received was measured honestly from the top.

`macquartz_begin_draw` now puts the context into the state AppKit hands
`drawRect:`, flip included, and resets the transform and clip while it is there
-- a plug-in that leaves a translate behind at the end of one `drawRect:` would
otherwise begin the next one inside it.

The check is mechanical, and it is worth having because "the editor looks right"
demonstrably is not: click a grid over every macOS editor, and for each click
that changes the picture, ask whether the pixels that changed contain the point
clicked or its mirror image. Across all ninety-nine, before: fourteen answering
only at the mirror -- eight distinct plug-ins, six of which are in the corpus as
Audio Units as well. After: 872 clicks landing where they were aimed, and none
at the mirror.

## Nothing was drawn with a path, and that is where the dials were

Fifty-eight Core Graphics entry points were accepting their arguments and
returning. Most of them deserve to -- line dash, miter limit, font smoothing
hints -- but among them were `CGContextMoveToPoint`, `AddLineToPoint`, `AddArc`,
`FillPath`, `StrokePath`, `FillEllipseInRect`, `SetLineWidth`,
`SetRGBStrokeColor` and `RotateCTM`. Every path call in the API. Nothing was
rasterized from a path at all.

This hid unusually well. An editor's fixed furniture is bitmaps -- the panel, the
labels, the knob body -- and bitmaps drew correctly, so the editors looked
finished. What is drawn with a path is the part that *moves*: the pointer on a
knob, the needle on a meter, the curve in an envelope, the highlight on the
selected step. Tattoo's knobs were plain discs, its two envelope displays were
empty boxes, and its mod sequencer was a blank strip. Dragging a knob moved the
parameter on all sixty frames of a drag and changed the picture on none of them
-- the sound followed the mouse and the knob sat still, which is the worst way
for a control to be broken.

There is now a path rasterizer: subpaths flattened to device-space points as
they are added, filled by scanline with four sub-scanlines a row and exact
horizontal coverage, nonzero or even-odd, and stroked by turning each segment
into a quad and each joint into a small polygon and filling the union. Curves
and arcs are flattened; `CGContextClip` reduces a path to its bounding box,
which is the same approximation the rest of the clip handling makes, and
consumes the path as Core Graphics does.

Fifty-five of the seventy-five editors that paint changed as a result, and the
comparison that matters is that none of them got worse -- no picture lost its
ink or its colours. Tattoo's knobs have pointers, its envelopes have curves, its
sequencer has bars, and its dials now repaint on every frame of a drag that
changes their value.

## An Audio Unit has no size until it has a view

WhispAir and Tricent showed a corner of themselves and a pair of scrollbars --
but only as `.component`, never as `.vst`, which is the clue that says where to
look. The editors are identical; the two are the same plug-in in different
wrappers, and both render pixel-for-pixel the same picture at the same size.

What differs is when that size can be asked for:

```
WhispAir.vst        before open 1127x776    after open 1127x776
WhispAir.component  before open 0x0         after open 1127x776
```

An Audio Unit's editor size comes from its Cocoa view, and there is no view
until the editor is opened. `pestudio` asked first and opened second, and its
whole editor-sizing block is guarded on the answer being non-zero -- so for
every AU it was skipped entirely. The window then believed there was no editor:
no size, no zoom applied, no automatic fit, and the Fit button disabled because
a window with no editor has nothing to fit. Meanwhile the editor widget grew to
the size the plug-in was actually drawing, inside a page still sized 0x0. That
is the crop, and the scrollbars.

Measured with the fix taken back out again, which is the only way to be sure
that is what it was:

```
without:  editor 0x0        zoom 1.000   page 0x0
with:     editor 1127x776   zoom 0.486   page 548x377   (pane 601x377)
```

`plugview` opens the editor first and asks afterwards, so it never had this;
the two windows had drifted apart on the one ordering that matters. `pestudio`
now asks again once the editor exists, and failing that takes the framebuffer's
own dimensions -- whatever the plug-in is drawing into is the truth.

Both windows also keep the editor fitted now, rather than fitting it once on
opening: the automatic fit is retried until the pane can actually be measured,
and re-run when the pane changes size, until the zoom controls are touched. A
chosen zoom is a decision and the window stops arguing with it from then on.

## A control that polls for the pointer instead of waiting for it

Dr. Device's XY pad could not be moved. Not "moved the wrong amount" -- grabbing
a handle and dragging did nothing at all to the parameters, while the picture
changed on fifty-eight frames out of fifty-nine, so the pad was clearly alive
and clearly not listening.

It was listening, to a different question. Most controls take the position out
of the `mouseDragged:` event they are handed. This one asks the window instead
-- `mouseLocationOutsideOfEventStream`, four times across a drag -- which is
what a control tracks with when it wants the pointer *now* rather than where it
was when the event was queued. That selector was unimplemented, so every poll
returned the zero point and the pad put its handle in the corner and left it
there.

Implemented against the same virtual pointer the events carry, so a plug-in that
mixes the two sees one pointer and not two. Both handles now follow the mouse on
both axes, and the parameters they drive move in step: dragging the left handle
up 86 pixels takes `Left Y` from 0.292 to 0.754 in even increments, with
`FiltResn` following it.

`makeFirstResponder:` went in beside it. It was returning nil, which is a "no",
and a control that checks gives up whatever it was starting.

## One repaint per mouse event

The Win32 path posts `WM_MOUSEMOVE` and lets the pump turn the invalid region
into a `WM_PAINT`: however many mouse events arrive between frames, one frame is
drawn. All three macOS backends painted inside the mouse handler instead.

A mouse reports far faster than a software rasterizer can draw an editor. At
Qyooo's thirty-one milliseconds a frame and a pointer reporting every four, the
queue backed up and the dial arrived in lurches -- which is exactly what "janky"
describes. The three `*_editor_mouse` functions now post the event and nothing
else; the pump draws. Delivering four events per displayed frame instead of one
costs the same as delivering one, where before it cost four times as much.

## An Audio Unit that is a VST in a wrapper

Nine of the fifty `.component` bundles refused to load with "no AU factory
export found". Eight of them are Audio Damage's, and they are Symbiosis
wrappers: one binary exporting both `SymbiosisEntry` -- the pre-AudioComponent
Component Manager entry point -- and `VSTPluginMain`, with no `AudioComponents`
key in the Info.plist for the modern path to read. The VST2 inside is the whole
plug-in, editor included, and this host already knows how to run it.

So when the AU path has failed and the bundle turns out to export a VST entry,
the VST2 is loaded instead. All eight now load, render, and show their editors,
and each renders a byte-identical WAV to its own `.vst` build -- which is the
check worth making, because it is the same code either way. The ninth,
Ragnarok's, is a Component Manager unit with no VST entry at all and still
declines.

## Splitting a frame across cores

An iPlug2 editor renders its whole interface every frame: NanoVG has no
partial-frame mode, so honouring the dirty rectangle buys nothing there -- it was
tried, and the shaded-pixel count did not move. The work is one pass over
independent pixels, which is the shape of thing that splits.

It splits by scanline. Each band gets a horizontal strip of the target and walks
the whole stream of triangles, drawing only its own rows. A pixel is written by
exactly one band, and within a band the triangles are applied in the order the
plug-in issued them, so the blending, the stencil read-modify-write and the
depth of overdraw are all unchanged. That is a claim worth testing rather than
asserting: every macOS editor captured twice, once banded and once not, compared
byte for byte.

The first attempt was slower than no threading at all. Deciding whether to split
by the size of the *target* meant splitting every call, and FB-7999 issues
twenty-three hundred draw calls a frame at eight triangles each -- two thousand
barriers, and a frame that went from 59 to 113 milliseconds. The decision has to
be made on the work in the call, which is a cheap pass over the triangles'
bounding boxes that gives up as soon as the answer is past the threshold. Two to
four calls a frame are split now, and they are the ones that matter.

| dragging a knob | before | after |
|---|---|---|
| MPS, 608x352 | 11.9 ms | **3.9 ms** |
| Qyooo, 858x648 | 31.2 ms | **6.2 ms** |
| FB-7999, 1150x718 | 58.7 ms | **34.8 ms** |

FB-7999 is the largest editor here and is also, since its display timer started
firing, drawing twice what it was: 1.93 million shaded pixels a frame against
960,000. Per unit of work it is three times quicker; in wall-clock it is not yet
at sixty frames a second, and it is the only one that is not.

## One plugin after another, in one process

A browser loads a plugin, shows it, closes it and loads the next, and this fell
over on the third. Two lifetimes were wrong.

**`dispatch_async` did not copy the block.** A block literal lives on the stack,
and the contract is that whoever takes it beyond the enclosing call copies it to
the heap first -- that is the whole meaning of `__NSConcreteStackBlock`. Keeping
the stack pointer and running it on another thread reads a frame that has since
returned, which survives for exactly as long as nothing reuses that stack. It is
copied now, with the compiler's own copy helper for whatever it captured, and
released when the job finishes.

**An image was unmapped while a dispatched block was still running.** The block's
code is *in* that image. Unmapping under it jumps into nothing -- not there, but
later, on a thread with no connection to whatever the host was doing.
`macho_close` waits for the outstanding jobs; if one will not finish, the mapping
is retired rather than unmapped, because leaking an image is a cost that stops
growing and pulling the ground out from under a running thread is a crash in the
host.

**And a view outlived its plugin.** Closing a macOS VST3 left the Cocoa and Metal
state pointing at a view whose image had gone, so the next plugin's first mouse
event walked a class structure that was no longer there. That close path now
resets them like the other two -- and, so that a close path which forgets again
costs nothing, the input and drawing paths refuse a view whose class the runtime
no longer knows. That test is free: image classes are unregistered when the image
is unmapped, which is exactly the question worth asking.

The last of these needed one more thing. Every Apple stand-in was minted as a
direct `NSObject` subclass, which is enough for a class a plugin only messages
and wrong the moment it *subclasses* one: an `NSTextField` is an `NSView`, so a
plugin's own text field expected `[super initWithFrame:]` to find `NSView`'s and
found nothing. The relationships a plugin actually leans on are declared now --
control, text field, cell, scroll and clip view, button, popup, slider -- and no
others, because inventing a hierarchy nothing needs is a way to move a method
resolution somewhere surprising.

Six plugins, mixed across all three macOS backends, opened, painted, poked and
closed twice each in one process:

```
round 1
  MPS.vst           608x352   99.7% lit   click: a control answered
  Qyooo.vst         858x648   99.5% lit   click: a control answered
  Blooo.component  1096x586   99.9% lit   click: a control answered
  Stigma.vst3      1104x488   98.7% lit   click: a control answered
  Ragnarok.vst     1080x396  100.0% lit   click: no control answered
  MPS.component     608x352   99.7% lit   click: a control answered
round 2
  ... the same
every plug-in opened, painted and closed
```

Ragnarok's line is the probe missing rather than the editor failing -- its grid
lands on artwork. Clicked where a control actually is, it answers, and the
pixels that change are the ones under the pointer: a click at 800,330 repaints
busiest at 816,288 and 768,312, and the same click mirrored to 800,66 changes
nothing. That is the check worth running on both editor backends, because it
catches the mirror above without needing to know what the plugin's controls are
called -- MPS, on the Metal path, repaints at 288..336 x 192..240 for a click at
307,230.

Across the whole corpus, every macOS editor -- 30 VST2, 18 Audio Unit, 18 VST3
-- survives a fifteen-point grid of clicks, each in its own process.

Under AddressSanitizer the host itself is clean -- three VSTGUI editors, two
rounds each, no error from any of this code. Two findings remain and both are
in the plugins:

- One allocates with `operator new` and releases with `free`. That is fine
  wherever both reach the same allocator, which is true on macOS and true here;
  ASan is stricter than either, and `alloc_dealloc_mismatch=0` is the honest
  setting for running foreign binaries.
- An Audio Unit's own teardown frees a pointer sixty bytes past a four-byte
  allocation. That is a real defect in the plugin, in its destructor, and it
  only ever surfaces under a sanitizer: with the ordinary allocator the address
  lands somewhere `free` accepts and the sequence runs, repeatedly, without
  complaint.

## What a browsing session costs

An editor that opens once and is measured once looks fine. Opening one after
another for an afternoon is the thing a browser actually does, and it was
leaking megabytes a time -- six and a half of them per open for MPS, three and a
half for Dr. Device. None of it was subtle once measured: resident memory
against opens, and whether it was heap or mappings.

Two allocations were never released:

- **Every Metal texture and buffer payload.** A texture is an Objective-C object
  and objects here are retired rather than freed, which is documented and
  deliberate -- but their *pixels* are a plain allocation, and a font atlas plus
  a drawable plus the vertex and uniform buffers is megabytes. `macmetal_reset`
  already freed the layer's back buffer when a plugin closed; it now frees the
  rest of what it handed out.
- **Every decoded image, and the framebuffer they composite into.** A VSTGUI
  editor is hundreds of PNGs, and a plugin does not reliably release them --
  `CGImageSourceRef` has no typed release at all, so it goes through `CFRelease`,
  which knew nothing about these objects. `CFRelease` reaches them now, and
  whatever is still outstanding when the editor closes is swept.

| opened repeatedly | before | after |
|---|---|---|
| MPS (iPlug2, Metal) | 6.63 MB each | 0.86 MB |
| Automaton (VSTGUI) | 3.67 MB each | 0.70 MB |
| Dr. Device (VSTGUI) | 3.50 MB each | 0.28 MB |

All thirty-one macOS VST2 editors opened in one process now come to 2.73 MB
apiece, and every one of them still renders byte-for-byte what it did before --
which is the check that made the sweep safe to make at all.

## What a frame costs, and what it is not worth optimising

An idle editor costs nothing: zero milliseconds a frame, on every backend here.
That is the number that matters most, because an editor spends almost all its
time not being touched.

Dragging a control costs what the software rasterizer costs, and nothing else:

| | frame while dragging | of which the rasterizer |
|---|---|---|
| MPS, 608x352 | 7.2 ms | 7.3 ms |
| Qyooo, 858x648 | 18.3 ms | 18.8 ms |
| Dr. Device, 728x431 (Core Graphics) | 0.21 ms | none |

The Core Graphics path -- the one the VSTGUI editors use -- does not register.
Nor does the image decoder: 563 images, 53 megapixels, in 1.3 seconds, which is
why a VSTGUI editor opens in under eight milliseconds with all its artwork.
Nor do the three pointer registries: about ten thousand lookups in a session.

The Metal rasterizer is therefore the whole cost, and three attempts to make it
cheaper are *not* in this tree. Replacing `p[i] / 255.0f` with a lookup table,
hoisting the reciprocal of the triangle area out of the pixel loop, and lifting
the channel-order branch out of the sampler all looked like wins on a single
run. Benchmarked properly -- best of five, against the same code without them --
they were noise: 7.17 against 7.19 ms, 18.26 against 18.26. The reciprocal was
the only one with a real effect, about five percent, and it changed the output
of fifty-four of the sixty-seven editors by a few hundred bytes each. Five
percent is not worth giving up "the renderer produces exactly what it produced
before" as a test, so all three came back out.

What would actually make a difference is not a micro-optimisation: Qyooo shades
384,005 pixels for one knob moving, on an editor of 555,984. The plugin is
redrawing two thirds of its interface per frame, and no amount of tightening the
pixel loop changes that.

Changing the *shape* of the work does. Those pixels are independent, so the
rasterizer now splits a large draw call by scanline across cores -- see
"Splitting a frame across cores" above. Qyooo drags at 6.2 ms rather than 31.2,
and the three micro-optimisations above are still not in this tree, because they
were still noise.

## macOS VST3: the same host, a different loader

The two halves had existed separately for a while: `vst3.c` compiled for System V
vtables, and a Mach-O loader. macOS x86-64 *is* System V, so the only thing
between them was the twenty lines that decide how the module gets into memory --
`dlopen` for a Linux bundle, `macho_open` for a macOS one, and `bundleEntry`
where Linux has `ModuleEntry`. The bundle is handed over whole rather than the
binary inside it, because the plugin's own artwork is found relative to the
bundle.

Their editors want an `NSView`, which is a third platform type beside the X11
and HWND ones already there. The runtime can mint one, so the host makes a
container, hands it to `IPlugView::attached`, and reads the pixels back through
the same software Metal backend. All eighteen render; all eighteen draw.

Getting there turned up two bugs that had nothing to do with VST3:

- **`CFUUIDCreate` returned a CFData.** A `cfdata`'s first field after the header
  is a pointer to its bytes; a `cfuuid`'s *is* the bytes. Tagged as data,
  releasing one handed the first eight of sixteen random bytes to `free()`.
  "free(): invalid pointer", from a plugin that had done nothing wrong.
- **`CFStringGetBytes` took its `CFRange` by pointer.** It is passed by value,
  and on System V a 16-byte struct of two integers goes in two registers -- so
  every argument after it was read one register early: `buf` arrived as `cap`,
  and the write-back pointer was a stack address the callee then stored through.
  Deputy's VST3 asks for all 2191 of its parameter names this way and faulted on
  the first. This is the same class of mistake the i386 checks in
  `tools/regress.py` exist to catch on the Windows side, and there is no
  equivalent check here.

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

- **`model-e`'s editor**, the one macOS VST2 that still draws nothing. It is
  VSTGUI 4 driving a `.uidesc` layout rather than compiled controls, and it
  reads that description through the Resource Manager -- `Get1Resource` and the
  handle calls around it, none of which are implemented. Its twelve images load
  and its audio works; nothing is drawn with them.
- **Typing a value into an iPlug2 control.** Clicking a control that offers text
  entry builds the field -- an `NSTextField` finally being an `NSView` was what
  that took -- and the field does not accept anything typed into it, because
  nothing here shapes text and there would be no glyph to show for a keystroke.
  Knobs, sliders and menus are unaffected.

  Getting that far turned out to be worth more than the crash it removed. Five
  CoreText calls -- `CTFontCreateWithGraphicsFont`, `CTFontCopyFontDescriptor`
  and the three `CTFontDescriptor` ones -- returned NULL, and iPlug2 keeps what
  they hand back. Two things followed from that. A control asking for text entry
  read a size out of the null descriptor, so clicking MPS's program-name field
  read a double from address 8 and took the host down -- a crash, on a click, on
  a control, which a twelve-by-twelve grid of drags reaches on four editors
  where the five-by-three grid this used to be checked with did not. And the
  font the editor loads from its own bundle never registered, so NanoVG had no
  glyphs and every string an iPlug2 editor draws at run time came out blank.

  They return real objects now, carrying a face and a size. The click does
  nothing instead of ending the process, and fifty-four editors gained the text
  they draw through their own font: MPS shows its program name, its program
  number and its value readout where it showed empty boxes.
- **Text in a VSTGUI editor is drawn by its own bitmap font, not by ours.** That
  is how those plugins work and it is why their labels appear. What this host
  still does not rasterize is CoreGraphics *text* -- `CTLineDraw` and the fill
  and stroke calls are accepted and dropped -- so an editor that draws a string
  through Core Graphics rather than from a glyph sheet leaves a gap where it
  should be. Nothing in this corpus depends on it for more than a caption.
- **Component Manager Audio Units.** One of the fifty `.component` bundles still
  exports only the pre-AudioComponent entry point -- Ragnarok's `_Ragnarok_Entry`,
  `ComponentResult(ComponentParameters *, void *)` -- which is a different ABI
  from the factory the AU host speaks. It says so and declines rather than
  calling it as a factory and crashing on the garbage it returns. Its VST2 and
  VST3 builds load, so nothing is actually lost; what is lost is that
  `pehost_classify` cannot tell in advance, so the browser offers it and the
  failure only arrives on the click. The other eight that used to fail this way
  are handled -- see "An Audio Unit that is a VST in a wrapper" above.
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
- **SEH.** `__try` regions work only insofar as nothing throws; real unwinding
  means parsing `.pdata`/`.xdata`.
- **A 32-bit C++ runtime, to go with the 32-bit loader.** `peload32` can load
  real dependency DLLs now -- it could not before, which is the whole reason
  the four Native Instruments plugins crashed on i386 while the same four
  loaded on x86-64. They import several hundred `MSVCP120` iostream and locale
  symbols, and there is no stubbing those: the objects have vtables and
  internal state. With a runtime present the loader binds 190 of them and the
  imports left on the generic stub drop from 313 to 16.

  What is still needed is the file. `runtime/` holds Microsoft's own x86-64
  `msvcp120.dll`; `runtime32/` is where the i386 pair goes, from the same
  place -- the Visual C++ 2013 redistributable, x86. Wine's builds of those
  DLLs are *not* a substitute: they are compiled against Wine's own `ntdll`
  and reach for far more of it than the four `__wine_dbg_*` entry points this
  host now answers. They load, bind, and then fault inside their own startup.

## Settings, and the paths they are written through

Plugins that keep settings in an `.ini` file now get a real
`GetPrivateProfileString`/`WritePrivateProfileString` and the section calls
beside them, anchored under this host's own tree rather than resolved against a
Windows directory that does not exist. Before that the whole family was
unimplemented, so every read returned the generic stub's 0 and every write went
nowhere: `stigma64` and `sixtraq64` started from defaults every session and
nothing said why.

The paths themselves survive a round trip through a guest's own path handling
now, which they did not. The host hands out real POSIX paths, and a plugin that
runs one through Windows path rules -- where a leading `/` means "the root of
the current drive" -- hands back something the filesystem cannot use. Both
directions turned up in this corpus, from the one cause:

    Kontakt        C:/home/you/.peload/AppData/Local/Native Instruments/...
    FM8, Absynth     home/you/.peload/Documents/Native Instruments/FM8/Sounds

The first is the path re-anchored against a drive that does not exist; the
second is the same path with its root marker eaten, which is why a second
settings tree used to appear in the working directory beside the real one.
`path_norm_n` undoes both, and only for this host's own data root -- a relative
path a plugin chose for itself is still its own business.
