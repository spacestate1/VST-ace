# tracker

A pattern sequencer. It plays other programs over MIDI -- open a vst-ace
window for each instrument, point each track of the tracker at one of them,
and the tracker plays them all, in time, with MIDI clock so tempo-synced
arpeggiators and delays follow it -- and it plays sample sets itself.

## Build and run

    cmake -S tracker -B tracker/build
    cmake --build tracker/build -j

    tracker/build/tracker [song.trk]        # Qt
    tracker/build/tracker-gtk [song.trk]    # GTK 4

Needs ALSA, and Qt 6 Widgets or GTK 4 (either is enough; each window is
built when its toolkit is found).

## Playing two synths

    ./va                                    # load the bass, e.g. FB-7999
    ./va                                    # load the lead, e.g. Dexed
    tracker/build/tracker tracker/examples/two-synths.trk

The second window calls itself `pestudio 2`, and the example plays `pestudio`
on track 1 and `pestudio 2` on track 2. Under each track's name is the window
it plays; the list shows every program that can be played. A green dot means
connected, a red one means that window is not open -- open it and the tracker
connects within two seconds, without anything being clicked.

## Samples

A track can play a sample set instead of a window, which the tracker sounds
itself -- nothing else to open:

    tracker/build/tracker tracker/examples/drums.trk

Each track has a sample-set box under its window box. It lists every folder
of WAVs under `/storage01/synth_stuff/drums` and `/storage01/synth_stuff/furnace`,
and any folders named in `VA_KITS` (colon separated); pick one and the track
plays it -- its window box greys out. The note picks the sample: every WAV
in the folder sorted by name from C-4 up, or wherever the set's `kit.txt`
puts it. So at the default octave 4, `z` plays the first, and the two rows
of note keys reach the first 29; a set of more than 68 starts lower, so all
of it fits. Typing a note says in the status line which sample it plays --
or that it plays none, and where the set's samples are -- and a note in the
grid that its track's set has no sample on is drawn red. Each track keeps
its own octave -- the "oct" box under its name, or `[ ]` and the toolbar's
octave for the cursor's track -- so a drum track can sit at 4 while a bass
track plays at 2.

**Samples** (beside File) has the rest:

- **Load Sample Set…** -- a folder of WAVs from anywhere, added to every
  track's list.
- **Edit Sample Set…** -- the set the cursor's track plays, or any other:
  which note plays which WAV, each one's gain and choke group, WAVs added
  (from the folder by name, from anywhere else by path) or taken out, and
  ▶ to listen to one. **Save** writes the folder's `kit.txt`, and every
  track playing the set plays the new one at once.

Pads in one choke group cut each other off, as a closed hat stops an open
one -- across tracks too. A hit plays out: a drum has no note-off. Stop fades
what is sounding; Panic cuts it dead. A track plays one note at a time, so
give a kick, a snare and a hat that land on the same row a track each, all
on the same set; the example uses four.

Samples are timed off the same queue as the MIDI tracks and started on
their own sample, within a millisecond of the row. They sound about 40 ms
after it -- the output's buffer -- where a window adds its own latency to
the MIDI tracks. `TRK_PCM` names an ALSA device other than `default`.

## Parts

A song is made of parts -- a part is a pattern of rows, "Intro", "Verse",
"Chorus" -- played in the order the **Parts** list left of the grid shows,
each as often as it appears there. Click a part to edit it; name it in the
box above the list. The box beside the name is how many rows the part has; the list shows each
part's length after its name. Under the list:

| button | does |
|---|---|
| New | a new, empty part after the one picked |
| Copy | a copy of the picked part after it, to change |
| Again | the same part again after it -- change one, and every time it plays changes |
| Remove | take the picked part out of the order (the pattern is kept) |
| ▲ ▼ | move it earlier or later (or drag it, in the Qt window) |

F5 plays the song from the part picked; while it plays, the part playing is
marked ▶ and, with follow on, is the one shown.

## Keys

Right-click the grid for Copy, Cut, Paste, Clear and selecting a track's
column or the whole pattern. Cut, paste and clear follow edit mode: with it
off, nothing changes. **vol** in the toolbar is the master volume of the
sample tracks; the windows playing MIDI tracks have their own.

**Help > Cheat Sheet** (F1; in the GTK window, the Help button) lists, for
each sample set the tracks play, every sample with its note and the key that
types it at the octave set now, then the note keys for the tracks that play
windows. It keeps up as the octave or a track's set changes. The **edit**
box beside **follow** shows edit mode; with it off the cursor's row is grey.

| key | does |
|---|---|
| arrows | move; left/right step through a cell's fields |
| Tab, Shift+Tab | next / previous track |
| PgUp, PgDn, Home, End | 16 rows, first and last row |
| `z s x d c v g b h n j m` | notes, one octave |
| `q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 p` | the octave above |
| `1` | note-off |
| `` ` `` | edit mode on / off: off, note keys only play -- try keys out without writing anything |
| Delete or `.` | clear the field, advance |
| Insert / Backspace | push the track down / pull it up a row |
| `0-9 a-f` | hex, in the velocity and controller fields |
| `[` `]` | octave down / up, for the cursor's track -- each track has its own ("oct" under its name) |
| Shift + arrows | select a block of cells; a drag selects too, and the row numbers select whole rows |
| Ctrl+C, Ctrl+X, Ctrl+V | copy, cut, paste -- paste puts the block down at the cursor, in any part |
| Ctrl+A, Delete | select the whole pattern; clear what is selected |
| `-` `=` | previous / next pattern |
| F5, F6, F8 | play song, play pattern, stop |
| Space | play pattern / stop |
| Escape | panic: release every note everywhere |

A cell is `note vel cc val`: a note (or `===` to release), its velocity in
hex (empty uses the track's), and a controller number and value sent on the
same row -- `4A 20` is CC 74, the usual filter cutoff, at 32. A track plays
one note at a time; a new note releases the last.

## What it guarantees

- **Timing.** Rows go out timestamped on an ALSA queue running on the
  high-resolution timer, a short way ahead, so the kernel does the timing and
  a busy window cannot drag it. Measured spacing is within 0.3 ms.
- **No stuck notes.** Every note started is remembered until a release has
  been sent for it directly. Stop, changing a track's window, Panic and
  quitting all send those, plus all-notes-off on every channel used.
- **One track, one window.** The tracker's ports refuse subscriptions from
  anything but the tracker, because vst-ace's windows subscribe to every port
  that allows it -- and would otherwise each play all eight tracks.
- **Clock once per window**, however many tracks play it.

## Song files

Plain text, one fact per line, written only for what is set:

    tracker 1
    bpm 120
    lpb 4
    track 1 channel 1 velocity 100 mute 0
    track 1 name Bass
    track 1 client pestudio
    track 1 port pestudio in
    track 2 samples drum-singles
    order 0 0 1 1                     the parts, in order
    pattern 0 name Verse
    pattern 0 rows 16
    cell 0 0 1 C-2 .. .. ..          pattern row track note vel cc val

Tracks name their window by ALSA client and port name, not number, so a song
finds its windows again however many times they have been reopened.

## Tests

    tracker/build/trktest                                      # engine, no window
    QT_QPA_PLATFORM=offscreen tracker/build/tracker-uitest song.trk outdir
    gtk4-broadwayd :7 &
    GDK_BACKEND=broadway BROADWAY_DISPLAY=:7 tracker/build/tracker-gtk-uitest song.trk outdir

`trktest` runs two ALSA receivers standing in for two windows and checks
timing, the clock, routing, that the ports refuse other subscribers, and that
stop, panic and quitting leave nothing sounding. The UI tests drive each
window with real key events and save a picture at each step.
