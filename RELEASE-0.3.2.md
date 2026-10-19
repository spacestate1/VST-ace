## vst-ace 0.3.2

Plug-ins from vst-ace in REAPER, two ways, from pestudio's new **Connect** menu.

**New**
- **Show in DAWs' FX list**: puts the loaded plug-in in REAPER's plug-in list
  (or any DAW's), to add to a track like any other. Works for everything
  pestudio loads: Windows 32- and 64-bit, macOS and Linux; VST2, VST3 and AU.
  Inside REAPER a bridged plug-in runs in a helper process, so a crash does not
  take REAPER down.
- **Connect to REAPER** in pestudio: the synth's audio into REAPER's inputs and
  a REAPER MIDI output into the synth. Several pestudio windows can be
  connected at once, each on its own channels.
- **Show: Instruments / Effects** boxes above the plug-in list.

**Fixed**
- Connect > REAPER never linked REAPER's MIDI output to the synth when the
  session manager had already wired that output to a keyboard.
- 32-bit plug-ins behind the bridge played silence inside a DAW.
- Narrowing the plug-in list no longer replaces the plug-in that is playing.

**Not yet**: a bridged plug-in's own window inside REAPER (its parameters show
as sliders). The FX-list option is not in the AppImage.
