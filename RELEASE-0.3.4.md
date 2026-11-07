## vst-ace 0.3.4

Volume sliders that do what they say.

**Fixed**
- **Master volume** now controls synth tabs as well as sample tracks.
- **Track sliders** on sample tracks are exact: 10% is now 10%, where it used to play at 37%.
- **Reopening a song** no longer replaces some VST3 synths' sound with default settings (Tricent's VST3, for one).

**New**
- **Tracks sharing a synth**: they have one output between them, so a slider barely changed the quieter track. The Qt studio now offers that track its own copy of the synth, with the same sound.

**Worth knowing**
- A song saved with its master above 100% plays its synths louder than before.
- A song using an affected VST3 may sound different on reopening: it now plays its real program, not the defaults.
