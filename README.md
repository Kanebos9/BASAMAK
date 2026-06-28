# BASAMAK

**BASAMAK** is a free, open-source drum & sample **step-sequencer plugin** — VST3, AU (macOS), and Standalone — for macOS, Windows, and Linux. Its built-in sounds are **synthesized** (analog/FM, physical-modelling, modal, noise) rather than sampled, and it ships with a CC0 sample library you can drop into any slot.

Created by **Oğuzhan Yazıcı**.

Built with [JUCE](https://juce.com). Licensed under the **GNU AGPL v3** (see [`LICENSE`](LICENSE)).

---

## Features
- 16 channels × up to 32 patterns, 8/16 steps, swing, per-step velocity/pitch/pan/roll/loop-conditions.
- Synth engines: **Analog + FM**, **Physical** (Karplus–Strong), **Modal** (struck resonators), **Noise**, plus a per-slot **Sampler** (trim / slice / time-stretch / pitch-shift via SoundTouch).
- Per-sound drawable EQ, drive, reverb/delay sends; a master FDN reverb, delay, **Glue** bus-compressor, and limiter.
- Drawable amp/pitch envelopes, a Strike/Ring envelope for the struck engines, and a Unison/Detune/Vibrato visual.
- Per-channel multi-output routing, MIDI-out mode, MIDI-learn on every control, and Novation **Launchpad Mini MK3** grid support.
- A bundled **CC0 sample library** (see [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md)).

## Formats & DAW support
- **VST3** — Windows, macOS, Linux (works in most DAWs).
- **AU** — macOS only (Logic, GarageBand, …).
- **Standalone** app — all platforms.
- *(AAX / Pro Tools is not built — that needs a paid Avid license.)*

---

## Download (pre-built)
If GitHub Actions is enabled, every push produces ready-to-install builds for all three OSes under the repo's **Actions → latest run → Artifacts**. Tagging a commit `v1.0.0` drafts a Release with the three zips attached. Unzip the one for your OS and run the included installer (`Install BASAMAK.command` / `install-windows.bat` / `install-linux.sh`).

## Building from source

### 1. Dependencies
- **CMake ≥ 3.22** and a C++17 compiler (macOS: Xcode CLT · Windows: Visual Studio 2022 · Linux: g++/clang).
- **JUCE 8** — cloned separately (not vendored here): `git clone https://github.com/juce-framework/JUCE`
- **SoundTouch** (time-stretch) is **bundled** in `external/soundtouch` and compiled in automatically.

On **Linux**, install JUCE's build deps first (Debian/Ubuntu):
```
sudo apt install build-essential cmake pkg-config libasound2-dev libjack-jackd2-dev \
  libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libgl1-mesa-dev libcurl4-openssl-dev libwebkit2gtk-4.1-dev
```

### 2. Configure & build (all platforms)
```
cmake -B build -DJUCE_DIR=/path/to/JUCE -DDAVULSEQ_RELEASE=ON
cmake --build build --config Release --parallel
```
(JUCE at `~/JUCE`? You can omit `-DJUCE_DIR`. macOS universal: add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.)
Builds land in `build/DrumSequencer_artefacts/Release/` as `BASAMAK.vst3` / `BASAMAK.component` / the Standalone app.

### 3. Install
Copy the built plugin to your plugin folder, or use the scripts in `Distribution/scripts/`:
- **macOS:** `~/Library/Audio/Plug-Ins/VST3/` and `…/Components/` (AU).
- **Windows:** `C:\Program Files\Common Files\VST3\`.
- **Linux:** `~/.vst3/`.

*(macOS dev loop: `./rebuild.sh` clean-builds + installs VST3/AU/Standalone. macOS-only.)*

### Sample library
The CC0 samples live in `Resources/Samples/` and are installed to `Documents/BASAMAK/Samples/`. Your own samples/presets/saved sounds live in `Documents/BASAMAK/` and are **not** part of this repo.

---

## License
**GNU Affero General Public License v3.0** — see [`LICENSE`](LICENSE). Free to use, modify and share; derivative/networked works must also be open-sourced under the AGPL. Copyright © 2026 **Oğuzhan Yazıcı**.

## Credits
- Sample library: various freesound.org creators, all **CC0 / public domain** — see [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
- [JUCE](https://juce.com) (AGPL / commercial) · [SoundTouch](https://www.surina.net/soundtouch/) (LGPL v2.1).
