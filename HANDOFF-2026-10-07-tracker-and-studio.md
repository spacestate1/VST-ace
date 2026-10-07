# Handoff — tracker, studio shells, audio backends: where things stand

**Status: everything below is committed (last commit `dbf63fa`) and passes its
tests headless. Nothing has been looked at on a real display or with real
audio hardware beyond PipeWire — that is the one thing still owed.**

## What exists

| area | state |
|---|---|
| Tracker core (`tracker/core`) | up to 16 tracks per song (`ntracks`, 8 by default; added and removed while playing), compact undo/redo, recording with quantize and count-in, MIDI export, Furnace-style keys, note-off flag, sample sets played by the tracker's own audio |
| Tracker UIs | Qt (`tracker/qt/trackerwidget.h`) and GTK (`tracker/gtk/trackerview.c`): same features, separate code. Scroll bar to far tracks, cursor follows, `+ Track` / `− Track`, Help tables for columns and keys |
| Sample key map | **Samples > Edit Sample Set…**: a vertical scrolled keyboard (every note, highest at top) with a drop-down per key, ▶ per key, drag from the list onto a key, **Load a folder…**, right-click menu; the old gain/choke table sits beside it. Writes the set's `kit.txt` |
| Studio shells | `session/qt/main.cpp`, `session/gtk/main.c`: one merged menu bar, plug-in manager, plug-in folders, keep-folder, session approval prompt |
| Synth audio backend | PipeWire (default), JACK, ALSA, from **File > Audio output…** (Qt) / **File > Audio…** (GTK), live switch, saved in `~/.config/vst-ace/audio-backend`, `--backend` / `DW_BACKEND` override, fallback to PipeWire when a backend will not open or dies. Library: `peload/audioout.[ch]` |
| REAPER | MIDI export of a tracker song, REAPER extension (`reaper/`) |
| Security / robustness | audit highs and mediums fixed (trusted paths, safe cpio extraction, sealed bridge memory, hidden plug-in list, session approval); memory audit fixed |
| Screenshots | `screenshots/` (README lists them), headless captures |

## Tests, and how to run them

    tracker/build/trktest                          engine, no window — all pass; TSan: 0 warnings
    tracker/test/run-uitests.sh <abs build dir>    Qt + GTK window tests, self-contained — all pass
    peload/build/audioout-test                     JACK (pipewire-jack) and ALSA "null" — all pass
    tools/regress.py                               27/27 at the time it was last run

The window tests need an ALSA client called `aseqdump` for track 1 and one that
is not running for track 3; `run-uitests.sh` supplies both from
`tracker/test/uitest.trk`. Pass an **absolute** build dir.

## Known gaps — read before trusting it

1. **No real display.** Qt was driven offscreen, GTK under broadway. Real mouse
   drag-and-drop onto key-map rows, the right-click menu, open drop-down lists,
   **Load a folder…**, Learn-style flows and typing-key playback in the key map
   are untested.
2. **JACK and ALSA outputs** were tested against pipewire-jack and ALSA `null`,
   not a real JACK server or sound card. Several synth tabs asking for the same
   exclusive ALSA device means later ones fall back to PipeWire; "System
   default" avoids it. The choice is global, not per tab. Audio *input* for
   effects is still PipeWire only.
3. **Native Linux plug-ins** untested headless; the REAPER extension was only
   run against a mock host; the 32-bit helper change (`peload/serve32.h`) has
   never been compiled (no 32-bit toolchain here).
4. **Two implementations** of the tracker UI and of the key map (Qt, GTK) can
   drift; there is no shared code beyond `tracker/core`.
5. **Key map limits:** a sample cannot sit on a key without being in the set's
   list (taking it off removes it; re-add from the drop-down); wide dialog
   (1180 px) is cramped on small screens; a set with hundreds of WAVs is
   untested (drop-downs fill only when opened).
6. **Volume publication:** the audio thread reads a copy of `song.volume`
   published in `trk_unlock`. Anything that changes the volume while holding
   `e->lock` directly (not through `trk_unlock`) must publish it too. Today only
   UI code changes it.
7. An untracked `song.trk` sits in the repo root; left alone on purpose.

## Where the pieces are

- Engine: `tracker/core/engine.c` (undo `snap_*`, recording `rec_place`,
  routing `trk_route`, audio `audio_open/close`), `song.c`, `editor.c`, `midiexport.c`.
- Set editor: Qt `KeyCombo`/`KeyRow`/`KeyList`/`SetEditor` in `trackerwidget.h`;
  GTK `ed_*` and `kb_*` in `trackerview.c` (state in the static `E`).
- Studio audio: GTK `start_backend` / `engine_start_with` / `act_audio` in
  `session/gtk/main.c`; Qt `Engine::startAudioWith` / `restartAudio` /
  `renderBlock` in `peload/qtgui/hostwindow.h`, dialog `audioSettings` in
  `session/qt/main.cpp`.
- Docs: `tracker/README.md` (Samples, Recording, Tracks, Audio output, Tests),
  top-level `README.md` (synth audio paragraph), `screenshots/README.md`.

## Rules learned this session

- Never copy the scratch-only screenshot/dump hooks (in my scratch copy of
  `session/*/main.*` and `tracker/qt/shots.cpp`) into the repo. The
  `TRACKER_UITEST` code in the repo is test-only, behind a build flag.
- Don't drive the user's live display; screen grabs of `:1` are black (Wayland).
  Use offscreen Qt and broadway for pictures.
- Debian packages are built on the build VMs, never on the Arch host; releases
  and uploads are the user's steps.
- Scratch builds lack the `peserve` helper, so plug-ins run in-process there and
  can crash; real builds isolate them.

## Next, in order

1. Look at the key map and both Audio dialogs on the real desktop; try a drag,
   Load a folder, and each backend.
2. Try JACK against a real server and ALSA against a real card.
3. Decide whether the key-map "taken off = removed from the set" behaviour is
   what you want, or whether unassigned samples should stay listed.
