# Handoff — click-free sample output, studio mix, Connect > REAPER: where things stand

Picks up from `HANDOFF-2026-10-07-tracker-and-studio.md`. Everything below is
committed (five commits on `main`, none pushed): `e680969`, `906bd16`,
`d238605`, `31e6825`, `097dc84`. Tests pass headless (see the end). What has
*not* been done is listed under "Not verified" -- read it before trusting any of
this on a real desktop.

## What changed since the last handoff

| area | state |
|---|---|
| Sample output (`tracker/core/engine.c`, `rt.c`) | Default output is a native PipeWire stream on the synth tabs' clock; ALSA for named devices and `TRK_PCM`. Reopens itself when the PipeWire server goes away (20 tries, then says so in Audio output). Realtime priority for the audio thread (scheduler, then RealtimeKit over D-Bus loaded at run time); self-demotes if rendering falls behind. Buffer setting (4 levels) in Audio output, kept in `audio-output`. One look-ahead limiter on the whole mix instead of the kits' per-sample limiter (`drumkit_set_raw`). Open/close serialised by `amx`. Dropouts counted and logged (first three, then one per 10 s). |
| Engine API additions | `trk_set_tap` (output tap for a recorder), `trk_poll` (one lock take for a window's timer), `trk_sink_set_gain_cb` (per-track fader drives a synth's output gain; handed back at full on `trk_close`), `trk_audio_buffer*`. |
| Engine locking | `trk_list_dests` no longer holds the engine lock across ALSA queries. Kit folder scan is not under the engine lock; the list has its own mutex (`kmx`); a missing set rescans at most every 5 s; a set that failed to load is not retried at once. |
| Song files (`song.c`) | `fsync` before rename, through symlinks, keeps mode; order list past its limit is refused; lines of a newer build are skipped and reported in `err`; names with leading spaces round-trip. |
| Tracker UIs | GTK (standalone): Export take, Ctrl+N/O/S/Shift+S/Q, View menu (meters, pitch colours), panic from any widget, full-name tooltips, Save As keeps the folder, status line no longer overwritten every 2 s. Qt: cheaper grid paint (full repaint 16 -> 7 ms). Both: playback timer 30 Hz moving / 10 Hz idle / slower hidden. |
| Studio shells | **Record studio mix** (both): one WAV of every synth tab + the tracker's sample tracks, on a bus (`session/mixbus.c` for GTK, `MixBus` in `peload/qtgui/hostwindow.h` for Qt -- same design, two implementations). `--record-mix <wav>` and `--play` for scripts. **Open Recent** (both; list in `~/.config/vst-ace/recent`, shared). GTK: MIDI input (`studiogtk:in` plus a port per synth tab), Effect input (PipeWire capture, off until asked). Qt: audio falls back PipeWire -> ALSA, `--backend`. |
| **Connect > REAPER** (both) | `session/reaperlink.[ch]` watches the PipeWire graph and links synth k's stereo output to REAPER inputs 2k+1/2k+2 and REAPER's k-th MIDI output to synth k's MIDI input. Only its own links are removed. Choice kept in `~/.config/vst-ace/connect`. In the GTK shell each synth gets its own stream (`studiogtk synth NN`) and MIDI port while this is on; the shared mix leaves it out. |
| Idle cost | Qt studio with a Windows VST3 loaded, 8 s: 19.8% / 19.5% -> 12.0% / 11.6% CPU (parameter list, pixel editor, piano). |

Design notes worth knowing before touching these:

- **Mix bus.** Sources keep their own cursors (start at the wall-clock position
  of their first block, advance by exactly what they feed); the writer drains up
  to what every *active* source has gone past, minus 2048 frames. Not the wall
  clock -- an audio device's clock drifts from it and a slow source would arrive
  too late. The ring is allocated once and never freed (a feeding thread may
  still be in it when a take ends). Disk-full is reported (`mixbus_failed`).
- **Parking (GTK).** `engine_park` now also waits for `g_direct_busy == 0`, the
  per-synth streams' in-callback count. A direct stream counts itself in before
  it reads `g_park_req`.
- **REAPER linker.** Audio goes to the lowest-numbered REAPER node that has audio
  inputs. MIDI sources are JACK-MIDI ports of a node called REAPER or Midi-Bridge
  ports starting `reaper:`. Tables grow; nothing is capped.

## Bugs found by testing this session (all fixed)

- libdbus was `dlclose`d after every realtime request: leaked its global state
  each time the audio thread started (`RTLD_NODELETE` now). Found by ASan.
- GTK sample-set editor leaked the array from `g_ptr_array_free(a, FALSE)`.
- **Crash when closing the Qt studio with a tab open**: the `setFrontCheck`
  lambda asked the half-destroyed tab bar which tab was in front. Only showed
  with a real plug-in loaded. `SessionShell::closing_` guards it.
- Use-after-free in the REAPER linker (a `spa_hook` removed after its proxy was
  freed). Found by ASan.
- Mix bus: first design drained by the wall clock and clicked under clock drift.
  Replaced by the cursor design above.

## Not verified -- do these on a real desktop

1. A **synth tab through the studio mix recorder, MIDI input and effect input**
   with a live plug-in on your own session. Tested here: tracker audio through
   the bus on both shells; the bus's summing and drift with synthetic sources;
   MIDI and effect input only with stand-ins.
2. **REAPER's own MIDI outputs.** Audio linking was checked against the real
   REAPER (see below); MIDI against a stand-in ALSA client named REAPER. By
   default REAPER on pipewire-jack shows no MIDI ports in the graph; its MIDI
   device preferences should list the studio's ports for enabling as outputs --
   not tried.
3. **GTK shortcuts in standalone `tracker-gtk`** (Ctrl+S etc.): wired to actions
   the tests exercise; no real key presses were synthesised.
4. **PipeWire output under real load** was measured only against a silent sink
   (0 dropouts at 256/512/1024 frames with 24 CPU hogs on 12 cores). Nobody has
   listened to it. The old code was not run under the same load, so there is no
   before/after.
5. REAPER with the **bridge** (`libvst-ace-bridge.so`) and with Mac plug-ins
   behind it: untested. The README's "In a DAW" section describes VST2 `.dll`
   only.

## Known gaps and fragile spots

- **PipeWire library leak, about 1-3 KB per stream open**, inside PipeWire's own
  `pw_context_new` module loading (our frames are only callers). Hit on every
  output open, buffer change, device switch, recovery. Related upstream reports
  (not confirmed to be the same bug): pipewire issues #2220, #327. Fix on our side would be
  one PipeWire context per engine instead of `pw_stream_new_simple` each time --
  touches the recovery logic, not done. `pw_init` is called once and never
  `pw_deinit`ed in the engine (the GTK shell does).
- **Unverified thread-safety assumptions:** ALSA seq calls from two threads on one
  handle (GTK creates a port while the MIDI thread reads; `trk_list_dests` runs
  without the engine lock). Neither is documented as safe.
- `RLIMIT_RTTIME` is set process-wide on the RealtimeKit path and cannot be
  undone; only matters if another realtime thread in the process blocks 200 ms.
- "Is the song dirty?" is a 1.6 MB `memcmp` under the engine lock on file actions.
  A change counter does not work (the lock is also taken for plain reads).
- GTK pattern grid redraws whole on each playhead step (~2 ms): measured small,
  left alone.
- Two implementations of the mix bus and of each tracker UI feature (Qt, GTK) can
  drift.
- The GTK shell needs X11 (plug-in editors embed); it cannot be run under broadway
  with plug-ins.
- Strict items left from earlier handoffs still stand (see the 10-07 file).

## How the tests were run (reuse this)

    tracker/build/trktest                          engine, no window -- all pass
    tracker/test/run-uitests.sh $PWD/tracker/build Qt + GTK window tests -- all pass
    python3 tools/regress.py                       27 passed, 0 failed
    cmake -S tracker -B <dir> -DCMAKE_C_FLAGS="-fsanitize=thread|address,undefined -g -O1" \
          -DCMAKE_EXE_LINKER_FLAGS=<same>          then build target trktest and run it

**Plug-ins and REAPER without touching your desktop.** The studio's plug-in
helper needs an X display; REAPER needs one too. Both were run inside a private
nested compositor, which gives them their own `DISPLAY` and Wayland socket:

    dbus-run-session -- kwin_wayland --virtual --xwayland --no-lockscreen \
        --no-global-shortcuts --exit-with-session=<script>

The script sets `GDK_BACKEND=x11` for `studiogtk`; Qt can use `offscreen` for the
studio itself. Send output to a silent sink so nothing is audible:

    pactl load-module module-null-sink sink_name=trkbench media.class=Audio/Sink \
          sink_properties=node.name=trkbench       # then PIPEWIRE_NODE=trkbench for the studio
    pactl unload-module <id>                       # when done

REAPER is unpacked at `~/Documents/reaper_linux_x86_64/REAPER/reaper`. Run it as
`pw-jack ./reaper -cfgfile <scratch>/reaper.ini -nosplash -newinst` so it uses a
scratch config and not `~/.config/REAPER`; it then shows up in the graph as node
`REAPER` with ports `in1 in2 out1 out2`. A Windows plug-in that loads without X
trouble in that session: `/storage01/synth_stuff/vst/windows/VST3/fb3300.vst3`.

PipeWire stand-ins used for checks (all in the scratch area, none needed in the
repo): `pw-loopback` with `node.name=REAPER media.class=Audio/Sink` for a REAPER
audio node; `pw-cat -p -P node.name=<n> long.wav` for a synth output; a tiny ALSA
client for a REAPER MIDI output. Do not leave test nodes in the live graph --
`pw-cli destroy <id>` removes strays.

## Rules learned this session

- `pkill -f <word>` matches the command line of the shell running it; use
  `pkill -x <name>`.
- A plug-in loaded with a real UI finds bugs a headless test does not (the
  close-time crash above). Test shell changes with one loaded.
- Do not run a mixed pacing test that adds jitter into the schedule: it makes the
  source slower than real time and looks like a bug in the recorder.
- Keep REAPER's real config out of tests (`-cfgfile`); keep test streams off the
  speakers (silent sink); remove test nodes afterwards.
- Commits are local; pushing, the build VMs and uploads stay the user's steps.

## Next, in order

1. On the real desktop: a synth tab + the tracker through **Record studio mix**,
   a MIDI keyboard into the GTK shell, **Connect > REAPER** with a real REAPER
   project (add input channels in its audio preferences; try enabling the studio's
   MIDI port as an output).
2. Decide on the **one-context-per-engine** change to stop the PipeWire leak, or
   file a minimal repro upstream (open/close `pw_stream_new_simple` in a loop
   under ASan).
3. The bridge for REAPER: shell plug-in or installer so every Windows/Mac plug-in
   appears without per-plug-in copies (the earlier discussion's ranked list), and
   check VST3/Mac behind the bridge.
4. MIDI import into the tracker (export exists, import does not).
