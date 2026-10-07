# Screenshots

The tracker running, as captured by scripted runs (offscreen Qt, and GTK on the
broadway backend), not mock-ups. All taken from the current code.

**Qt tracker** (`examples/drums.trk`)

| File | Shows |
|---|---|
| 01-qt-tracker-loaded.png | the song open |
| 02-qt-playing.png | playing, the cursor following |
| 03-qt-tracks-added-while-playing.png | **+ Track** pressed three times *while the song plays*: the new tracks appear after the cursor's, the song carries on |
| 04-qt-many-tracks-scrolled.png | sixteen tracks (the most), scrolled to the far end -- headers stay over their columns; "no room" is said in the status line |
| 05-qt-recording.png | a take played in from the keyboard: **● Recording** lit, notes on track 3 with `===` where each key was let go |
| 06-qt-record-options.png | **Rec…**: count-in, metronome, quantizing, MIDI input |
| 07-qt-audio-output.png | **Samples > Audio Output**: system default, PipeWire, JACK, PulseAudio |
| 08-qt-help-columns.png, 09-qt-help-keys.png | **Help > Columns** and **Help > Keys** |
| 10-qt-menu-file / -samples / -help.png | the menus |

**GTK studio** (the tracker tab)

| File | Shows |
|---|---|
| 20-gtk-studio-loaded.png | one menu bar: File / Samples / Help |
| 21-gtk-playing.png, 22-gtk-recording.png | playing; a take being recorded |
| 23-gtk-record-options.png, 24-gtk-audio-output.png | the same windows in GTK |
| 25-gtk-help-columns.png, 26-gtk-help-keys.png | help |
| 27-gtk-tracks-added.png | tracks added with **+ Track** |
| 28-gtk-sample-keymap.png, 11-qt-sample-keymap.png | Samples > Edit Sample Set: the key map (drag a sample onto a key) |
