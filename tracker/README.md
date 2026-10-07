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

In studio and studiogtk (the session shells, `session/`), the synth tabs
appear in the same
list as `this window: <plug-in>`, after the ALSA windows. Picking one plays
the tab directly, in-process -- no trip through the sequencer -- with the
same sample-accurate timing the kernel's queue gives an ALSA window: the
engine's delivery thread turns each event's queue tick into wall-clock time
exactly as the queue's timestamps mean it and hands the tab a block at a
time, and the tab's audio engine places every event on its own sample. One
destination per track, as with a window: the track's window names wait in the
song, and routing back reconnects them. The pick is saved with the song, as a
`sink` line naming the tab, and the shells open that synth again -- at its
default program; sounds are the session file's business -- when the song is
opened. A routed tab closing sends the track back to its window.

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
track plays at 2. Changing a track's octave moves the notes already typed
on it with it, clamped at C-0 and G-9, so a part keeps sounding as it did.

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
| Delete or `.` | clear the field, advance |
| Insert / Backspace | push the track down / pull it up a row |
| `0-9 a-f` | hex, in the velocity and controller fields |
| `[` `]` | octave down / up, for the cursor's track -- each track has its own ("oct" under its name), and its notes move with it |
| Shift + arrows | select a block of cells; a drag selects too, and the row numbers select whole rows |
| Ctrl+C, Ctrl+X, Ctrl+V | copy, cut, paste -- paste puts the block down at the cursor, in any part |
| Ctrl+A, Delete | select the whole pattern; clear what is selected |
| Ctrl+Z | undo -- the last 50 edits: entry, clearing, insert/backspace, octave moves, cut/paste/clear, parts-list changes |
| `-` `=` | previous / next pattern |
| F5, F6, F8 | play song, play pattern, stop |
| Space (or `` ` ``) | edit mode on / off -- off, the note keys only play and nothing is written |
| Enter | play pattern / stop |
| F7 | record -- see Recording below |
| Shift+Enter | play the pattern from the cursor row |
| Escape or F12 | panic |
| keypad `*` `/` | octave up / down (as `[` `]`); with Ctrl, the edit step |
| Ctrl+Y or Ctrl+Shift+Z | redo |
| Ctrl+Shift+V | paste mix: only what the clipboard has, over what is there |
| Ctrl+F1 / F2, Ctrl+F3 / F4 | transpose the selection (or the cell) down / up a semitone, an octave |
| Shift+PgUp / PgDn | select a page of rows |
| Alt+F9, Alt+F10, Alt+Shift+F9 | mute, solo, unmute all -- the cursor's track |
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
    track 3 sink this window: FB-7999   an in-process synth, as the shell named it
    order 0 0 1 1                     the parts, in order
    pattern 0 name Verse
    pattern 0 rows 16
    cell 0 0 1 C-2 .. .. ..          pattern row track note vel cc val

Tracks name their window by ALSA client and port name, not number, so a song
finds its windows again however many times they have been reopened. A track
routed to an in-process synth saves the tab's name the same way, and the
studio shells open that synth again -- at its default program; sounds are the
session file's business -- when the song is opened. A file carrying `sink`
lines is refused by builds from before they were first saved.

## Tests

    tracker/build/trktest                                      # engine, no window
    QT_QPA_PLATFORM=offscreen tracker/build/tracker-uitest song.trk outdir
    gtk4-broadwayd :7 &
    GDK_BACKEND=broadway BROADWAY_DISPLAY=:7 tracker/build/tracker-gtk-uitest song.trk outdir

`trktest` runs two ALSA receivers standing in for two windows and checks
timing, the clock, routing, that the ports refuse other subscribers, and that
stop, panic and quitting leave nothing sounding -- plus the in-process sinks:
delivery timing against the sink's own clock, clock and transport once per
sink, preview, moving a track between a window and a sink, and a sink removed
mid-song -- and that a sink pick is written into the song file and parses
back. The UI tests drive each window with real key events and save a
picture at each step.

## Recording

**F7** (or the **● Rec** button) plays from the cursor row and writes what you
play into the pattern, on the cursor's track. Press it again, or Stop, to end
the take. The notes come from the computer keyboard, or from a MIDI keyboard
connected to the tracker's **Record In** port (pick one under **Rec…**, or
`aconnect <keyboard> "tracker:Record In"`).

Timing is by the clock the song plays on. A MIDI event is stamped with the
sequencer's own tick the moment it arrives, so where it lands does not depend
on when a thread got to it; a computer key is stamped when the window hands it
over, and the **keyboard timing offset** moves those earlier or later to make
up for a slow keyboard or screen. A note goes to the row it falls on -- with
**Nearest row**, a note struck a little before a row rounds up to it; **Row
that is sounding** leaves it where it was struck. A key let go writes `===`
on its row (a note is at least a row long), unless that is turned off.

**Rec…** also sets a count-in (0-4 bars) and a metronome click on every beat
(the bar's first beat higher), played through the tracker's own audio output;
a machine without one records silently. One undo removes the whole take.

**File > Export recorded take as MIDI** writes the take as it was played, at
its exact times -- not rounded onto rows -- as a standard MIDI file at the
song's tempo.

## Tracks

A song starts with eight tracks and can have up to sixteen (`TRK_TRACKS` in
`core/trk.h` is the most; a song's own count is `trk_song.ntracks`). **+ Track**
adds an empty one after the cursor's; **- Track** takes the cursor's away, notes
and all -- asked about first when it holds any, and Undo (Ctrl+Z) brings it
back. The windows, sample sets and synth routes follow their tracks, and
playback carries on (what was sounding is released). The grid scrolls sideways with its headers
(the scroll bar under the grid), and the cursor keeps itself in view.

A song of eight tracks is written exactly as before. Any other count adds a
`tracks N` line, which builds from before this change refuse; a file that uses a
track past its count has that many tracks. The tracker's ALSA client has one
output port per possible track (sixteen), whatever the song uses.

Undo keeps, for each step, everything of the song except the patterns, and only
the patterns that hold something -- tens of kilobytes where the whole song is
over a megabyte now that it has room for sixteen tracks.

## Audio output

Sample-set tracks are played by the tracker's own audio. **Samples > Audio
Output** lists where it can go -- the system default, PipeWire, JACK,
PulseAudio (each where its ALSA plug-in is installed) and sound cards -- and
clicking one switches to it, live, and remembers it
(`~/.config/vst-ace/audio-output`; `TRK_PCM` still names a device when none is
chosen). Synth tabs in the studio play through PipeWire, a separate path; it
serves JACK programs through pipewire-jack and ALSA programs through
pipewire-alsa.
