## vst-ace 0.3.1

The studio is now the main window: a tab per plug-in beside a tracker, with the
standalone `pestudio` and `dwstudio` still there for anyone building from source.
Running the AppImage with no arguments opens the studio (Qt).

**New**
- Studio: record the whole mix to one WAV, Open Recent, and **Connect > REAPER**
  to link each synth to REAPER through PipeWire.
- Synth audio can go to PipeWire, JACK or ALSA, switchable while running.
- Tracker: click-free sample output that recovers if PipeWire restarts, per-track
  volume and meters, up to 16 tracks, and a key-map editor for sample sets.
- GTK studio gains MIDI and effect input.
- A "Loading" bar while a plug-in loads, instead of a frozen window.

**Fixed**
- Qt studio crashed when closing a tab that had an Inputs submenu.
- A sample track's octave now follows its sample set and stays fixed, so the
  keys always land on the pads.

**Install**

    sudo apt install ./vst-ace_0.3.1-1_amd64.deb          # Debian 13+, Ubuntu 24.04+
    sudo pacman -U ./vst-ace-0.3.1-1-x86_64.pkg.tar.zst   # Arch
    chmod +x vst-ace-0.3.1-x86_64.AppImage && ./vst-ace-0.3.1-x86_64.AppImage

Use `apt install ./`, not `dpkg -i`. No Fedora package in this release.
