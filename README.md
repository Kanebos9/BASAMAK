# BASAMAK

<img width="1440" height="764" alt="Screenshot 2026-07-14 at 09 57 31" src="https://github.com/user-attachments/assets/fd13c511-bac5-48fc-9411-8eac4d3eecb0" />

**BASAMAK** started as a drum sequencer but then I added so much features on it and now it's more like a software groovebox with so much flexibility and ease of use. It's not drum specific anymore, you can synthesize regular keyboard sounds as well.

<img width="1440" height="764" alt="Screenshot 2026-07-14 at 09 58 20" src="https://github.com/user-attachments/assets/bab7d80c-a6cb-47d0-9cfa-e1d312e60874" />

It comes as VST3, AU (macOS), and Standalone for macOS, Windows, and Linux. Its built-in sounds are **synthesized** rather than sampled. But you can load sample as well. It also comes with a CC0 sample library.

There are different sound sources, they mainly come from modal, karplus strong, granular and a very flexible oscillator (it has additive, wavetable, FM features too)

Created by **Oğuzhan Yazıcı**.

Built with [JUCE](https://juce.com). Licensed under the **GNU AGPL v3** (see [`LICENSE`](LICENSE)).

<img width="1440" height="767" alt="Screenshot 2026-07-14 at 09 59 47" src="https://github.com/user-attachments/assets/bc6f08f6-598b-47e3-97f5-def5c472b673" />

<img width="1409" height="760" alt="Screenshot 2026-07-14 at 09 59 57" src="https://github.com/user-attachments/assets/f942d29d-4b24-4650-b254-e80ea8879a0c" />

---

## Formats & DAW support
- **VST3** — Windows, macOS, Linux (works in most DAWs).
- **AU** — macOS only (Logic, GarageBand, …).
- **Standalone** app — all platforms.
  
---

## Download (for regular users)
Go to the [Releases](https://github.com/Kanebos9/BASAMAK/releases) page, download the zip for your OS (windows, mac or linux) from the latest release, unzip it, and run the included installer.

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
