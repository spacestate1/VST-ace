## vst-ace 0.3.5

Windows plug-ins in Ardour, for real this time (#3).

**Fixed**
- **A relative path in a `.vstace` file** is now read from the `.vstace` file's own folder. `echo FB-3300.dll > ~/.vst/FB-3300.vstace` means `~/.vst/FB-3300.dll`, and `~/` works too. Before, it was read from wherever the DAW's plug-in scanner happened to start, so it usually pointed nowhere.
- **A `.vstace` naming a missing file** now says which path it tried. Before, it said the copy named no plug-in at all.

**Worth knowing**
- **Ardour does not look in `~/.vst`.** Its Linux VST path is system folders only. Add `~/.vst` once in Edit > Preferences > Plugins > VST > Linux VST Path, then rescan. pestudio's *Show in DAWs' FX list* now says so, and the README no longer claims otherwise.
