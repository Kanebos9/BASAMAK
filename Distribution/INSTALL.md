# Installing BASAMAK

BASAMAK is a VST3 / Audio Unit drum & sample sequencer. Pick your operating
system below. Installing always replaces any older version and **never deletes
your samples, sounds or presets** — those live in a separate folder
(`Documents/BASAMAK`) that the installer leaves alone.

---

## macOS

1. Unzip **BASAMAK-macOS.zip**.
2. Double-click **`Install BASAMAK.command`**.
   - If macOS says *"cannot be opened because it is from an unidentified
     developer"*, right-click the file → **Open** → **Open**.
3. Restart your DAW and rescan plugins.

Installed to:
- VST3 → `~/Library/Audio/Plug-Ins/VST3/BASAMAK.vst3`
- AU → `~/Library/Audio/Plug-Ins/Components/BASAMAK.component`

> The plugin is not Apple-notarized, so the installer clears the macOS
> "quarantine" flag for you. If a DAW still refuses to load it, run in Terminal:
> `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/BASAMAK.vst3`

## Windows

1. Unzip **BASAMAK-Windows.zip**.
2. Double-click **`install-windows.bat`** (it will ask for administrator rights
   to write into the shared VST3 folder).
3. Restart your DAW and rescan plugins.

Installed to: `C:\Program Files\Common Files\VST3\BASAMAK.vst3`

## Linux

1. Unzip **BASAMAK-Linux.zip**.
2. In a terminal: `bash install-linux.sh`
3. Restart your DAW and rescan plugins.

Installed to: `~/.vst3/BASAMAK.vst3`

---

## Your library (all platforms)

Everything you make or add lives in **`Documents/BASAMAK/`**:

```
Documents/BASAMAK/
    Samples/        your audio samples (factory set is seeded here on install)
    Sound Bank/     sounds you save from a channel   (*.basamaksound)
    Presets/        whole-kit presets you save       (*.basamakpreset)
```

These are kept across updates and uninstalls. To move BASAMAK to another
computer, copy this folder over.

## Uninstalling

Delete the installed plugin file shown above for your OS. To also remove your
library, delete `Documents/BASAMAK` (this erases your saved samples/mixes/
presets, so back it up first if you want to keep them).
