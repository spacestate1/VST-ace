# reaper_tracker

The tracker's songs in REAPER, two ways.

## File > Export MIDI (both tracker windows and both studios)

Writes the song as a type 1 standard MIDI file: a tempo track, then one track
per playing track, named as the song names it and on its own channel. Muted
tracks and tracks that play nothing are left out. A track plays one note at a
time, as in the tracker: a note lasts until the next note or a note-off (or
the end of the song); a track playing a sample set plays each hit for one
row. In REAPER: Insert > Media file..., or drag the `.mid` onto the arrange.

The walk lives in `tracker/core/midiexport.c` (`trk_song_events`), which is
what the tracker's engine schedules, written out as events.

## The extension

`reaper_tracker.so` adds an action, "vst-ace: Import tracker song (.trk)...",
which reads a `.trk` and creates the tracks with MIDI items in the open
project, as one undo step. The tempo is set to the song's when the project is
empty, and asked about when it is not.

    ./fetch-sdk.sh                        # the REAPER SDK and WDL headers, once
    cmake -S . -B build && cmake --build build
    cmake --install build                 # ~/.config/REAPER/UserPlugins

Restart REAPER, then find the action in the Action list (`?`) and bind it to
a key or a toolbar button. It uses only REAPER's own API
(`InsertTrackAtIndex`, `CreateNewMIDIItemInProj`, `MIDI_InsertNote`, ...);
if REAPER lacks one of those the extension does not load.

Tested against a stand-in host that supplies those functions, not against
REAPER itself.
