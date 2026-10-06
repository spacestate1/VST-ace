# Plan: synths inside the tracker

Status: planned, not started (2026-10-04). Nothing below exists yet.

## Goal

A track can play a plug-in synth itself. The tracker sounds it through the
same output as the sample tracks, in time with everything else, and each synth
has a window of its own -- opened when wanted, to load patches, pick programs
and turn knobs, and closed when not. Closing the window does not stop the
synth.

## 1. What a track can be

Each track plays one of three things:

- a window, over MIDI -- as now;
- a sample set -- as now;
- a plug-in -- new.

A plug-in track saves the plug-in's path, its program, and its whole state
(every knob, through the plug-in's own state call), so opening the song brings
back the same sound:

    track 2 plugin /…/blooo64.dll
    track 2 program 5
    track 2 state <base64 of pehost_get_state>

## 2. Engine (core/engine.c)

- **Loading.** A plug-in is opened and closed on the window thread, as
  `pehost.h`'s threading contract requires (`pehost_open_as`, `pehost_close`,
  program and parameter calls from one thread; `pehost_render` from the audio
  thread only; note events through its lock-free queue). The audio thread calls
  `pehost_thread_init()` once before rendering.
- **Isolation on by default** (`pehost_set_isolation(1)`), so a crashing
  plug-in costs its helper process, not the tracker. The helpers are found
  without setup: `bridge_client.c` looks beside the executable, then walks up
  to `peload/build/` (`peserve`, and `peload32` for 32-bit Windows plug-ins).
  The Windows runtime is found the same way, from `/../../runtime` relative to
  `tracker/build/`.
- **Timing.** Plug-in notes go on the same queue clock as sample hits, through
  the same pending list the audio thread reads. Each period it works out which
  fall inside it and where, hands each to its plug-in with
  `pehost_midi_at(h, status, d1, d2, frame)`, renders every plug-in once
  (`pehost_render`, opened with the audio period as its block size), and mixes
  the result with the samples before the master volume.
- **No stuck notes.** Plug-in tracks have real note-offs, so they take the MIDI
  tracks' guarantees: `held[]` / `pos_mark` snapshots and `rewind_locked` for
  panic and re-routing, a note-off for every note started. Stop and Panic also
  call `pehost_release_all`. Swapping a plug-in while playing removes it from
  the audio thread under `smx` first, and closes it after.
- **Transport.** Play, stop, locate and tempo go to each plug-in
  (`pehost_set_playing`, `pehost_locate`, `pehost_set_tempo`), so synced
  arpeggiators and delays follow the song.

## 3. The synth window

A **Synth…** button on a plug-in track's header opens a top-level window for
that track's synth:

- **Plug-in** -- choose or change it, from the same folders `pestudio` lists
  (`vstdirs.h`).
- **Programs** -- the plug-in's own presets.
- **Patches** -- load and save in the format `pestudio` and `va peload` use
  (`patch.h`: `patch_load`, `patch_save`, `patch_find_for`), so patches made
  there open here.
- **Parameters** -- every knob, as a list.
- **Editor** -- the plug-in's own interface: embedded for native Linux VST3
  (`PEHOST_EDITOR_X11`, `pehost_editor_attach`), drawn from pixels for Windows
  VST2/VST3 (`PEHOST_EDITOR_PIXELS`, `pehost_editor_pixels` and the mouse/key
  calls).

Close it any time and reopen it to change things. The note keys still play
the synth while it is focused.

## 4. Reusing what exists

- **Qt.** `peload/qtgui/main.cpp` has the editor, parameter list and patch
  handling (`EditorHost`, `PixelEditor`, `ParamModel`). Move them into a shared
  file used by both `pestudio` and the tracker rather than copying them; that
  touches `pestudio`, so check it on its own first.
- **GTK.** `gui/plugview.c` (about 3,100 lines) assumes one plug-in for the
  whole program -- its state is the single global `P`. Making it per-window is
  the largest single piece. The GTK tracker can get programs, patches and
  parameters first and the editor second.

## 5. Order of work

1. **Engine:** plug-in tracks that sound, are saved, and leave nothing stuck.
   Tested headless with a synth from the corpus (Blooo, `blooo64.dll`):
   recorded timing checks against the queue, as `trktest` does for samples,
   and the stuck-note checks for stop, panic and re-routing.
2. **Qt synth window:** plug-in choice, programs, patches, parameters, then
   the editor.
3. **GTK synth window:** the same, after `plugview` works per window.
4. **Polish:** bring back a plug-in whose helper died (`pehost_alive`,
   `pehost_recover`); per-track volume; a setting to line up the MIDI window
   tracks with the rest; showing audio load.

## 6. Risks

- **CPU.** Eight synths in one audio thread. Heavy plug-ins may need a longer
  buffer than today's 30 ms -- make it a setting.
- **Missing data.** Some Windows plug-ins need runtime data;
  `pehost_data_check` / `pehost_data_repair` exist and the synth window should
  offer them, as `pestudio` does.
- **Editors.** Plug-in editors behave differently under Qt and GTK; that is
  the part most likely to need fixing plug-in by plug-in.
- **Song size.** Saved state can run from kilobytes to megabytes.

## Until then

The tracker already plays synths in separate windows over MIDI (a track's
window box), and sample sets itself (its sample-set box). This plan replaces
neither: plug-in tracks are a third choice.
