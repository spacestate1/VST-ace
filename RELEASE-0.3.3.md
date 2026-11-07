## vst-ace 0.3.3

Steadier realtime audio in the studio, and songs and USB keyboards that behave.

**Audio**
- Fewer pops and pauses when many synths play, above all under load. A plug-in
  that runs late no longer holds up the others: the audio thread waits a bounded
  time, and live hosts render one block ahead (one block of added latency) so a
  late block is still heard.
- Helpers and the ALSA output ask RealtimeKit for realtime priority, with a limit
  that demotes a runaway thread instead of letting the system kill the process.
- Much less CPU: macOS editors draw on half the cores below the audio thread with
  a faster fill, tabs in the back stop drawing, and an audio input stays closed
  until it is chosen as the source, which leaves realtime threads for the synths.

**Songs**
- Opening a song closes the synths it does not use, and asks first if one of them
  has changed since it was saved.
- **File > New song** closes the current song and opens a blank tracker.

**USB keyboards** (Qt studio)
- A keyboard plays the tab in front, not every synth at once. The tracker, REAPER,
  MIDI clock and tabs set to one MIDI channel still reach tabs in the back.
- With the tracker in front it plays the cursor's track, connected for you. With
  the song stopped and **edit** ticked, its notes go in at the cursor as typed ones
  do; while the song plays, only a Rec take writes.

**Fixed**
- A crash opening the editor while a search or the Show boxes hid the loaded
  plug-in.
- The tracker grid showing the old part's notes after playback moved to a new
  part, most visibly in full screen.
