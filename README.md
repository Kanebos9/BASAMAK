# BASAMAK

<img width="1440" height="787" alt="Screenshot 2026-06-28 at 20 11 13" src="https://github.com/user-attachments/assets/7e98473c-c176-4a22-8f1d-b6945e213aca" />

**BASAMAK** started as a **drum synth / step-sequencer / sampler** but then I added so much features on it and now it's like a mini DAW. It's not drum specific anymore, you can synthesize regular keyboard sounds as well.

It comes as VST3, AU (macOS), and Standalone for macOS, Windows, and Linux. Its built-in sounds are **synthesized** rather than sampled. But you can load sample as well. It also comes with a CC0 sample library.

There are different sound sources, they mainly come from modal, karplus strong and a very flexible oscillator (it has additive, wavetable, FM features too)

I think you should think of BASAMAK as a **sketchpad for a whole track**: because everything is in front of you. You can create your drum sequences, bass lines and play along with them in the same plugin. It has midi recording and midi dragging (in case you wanna drag it on your DAW's track) feature too.

<img width="1440" height="786" alt="Screenshot 2026-06-28 at 20 12 15" src="https://github.com/user-attachments/assets/e6a438b9-ca91-445c-99ef-6864f092997e" />

Created by **Oğuzhan Yazıcı**.

Built with [JUCE](https://juce.com). Licensed under the **GNU AGPL v3** (see [`LICENSE`](LICENSE)).

---

## Features
- **16 channels × up to 32 patterns**, selectable step counts (up to 32) **or a free unquantized Draw lane**, swing (MPC 50–75%), and **pattern chaining** to arrange a whole song (each pattern plays N loops, then jumps).
- **Play & record like an instrument** — perform any pitched channel from a MIDI keyboard or the on-screen keys, and record it into the pattern. Recordings are captured as **takes** (one per loop) — quantized to steps *or* drawn free-hand in the **Draw lane** — kept per channel and saved with your project. **Tie steps into sustained notes** with step-merge (shift-click).
- **Two synth slots per channel**, each any engine with its own params, envelopes, EQ, filter, FX and LFO — blend them (e.g. saw + sub for bass).
- Synth engines: **Analog + FM** (14 band-limited waveshapes + wavefold + unison), **Physical** (Karplus–Strong), **Modal** (struck resonators), **Noise**, plus a per-slot **Sampler** (trim / slice / time-stretch / pitch-shift via SoundTouch).
- **Bass/melodic ready:** per-step **pitch**, **303-style slide/portamento** (glide into the next note), per-step **note length** (decay-rescale, from tight gates to long ring-outs), and a **Hz ↔ note** read-out (click a frequency to dial in real notes). Any channel can be played as a bass or lead. Includes a dedicated **bass sound bank** (Station/Ladder/Rubber/Neuro/Hoover/Reese and more).
- **Per-slot resonant filter with envelope + velocity accent** (drawn right on the EQ display) and a **per-slot LFO** with three independent destinations (filter / pitch / volume) for wobbles, sirens and tremolo.
- **Unison & chords** — thicken any oscillator / physical / modal sound with unison + detune, or switch a voice into a **chord** (5th, Maj, Min, Sus4, Maj7, Min7…) so a single step, a held key, or a drawn line plays the whole chord.
- A full **A-H-D-S-R amp envelope** (sustain + release for held / gated notes; one-shots stay a tight AHD), a 4-point **pitch envelope**, a Strike/Ring envelope for the struck engines, and a Unison/Detune/Vibrato visual — all drawable.
- Per-sound drawable EQ, drive, reverb/delay sends; a master **FDN reverb**, **delay**, one-knob **Tilt** tone, tube **Saturation**, **Glue** bus-compressor, and a look-ahead **limiter**.
- Every control (and every step) is **MIDI-learnable** — works with any MIDI controller sending CC (knobs, faders, or a pad grid such as a Novation Launchpad configured in Components, or **TouchOSC**); **MIDI-out** mode turns any channel into a note generator; per-channel multi-output routing.
- A ready-made **TouchOSC** control surface is bundled (in the download's `TouchOSC/` folder): an 8×8 step grid plus per-channel Mute/Solo/Overlap and Play/Stop. Load the **"Ch1 8x8 + M/S/OV + Play"** map from the MIDI dropdown and it pairs with the template out of the box (see `TouchOSC/TouchOSC-README.txt` for the one-time MIDI-bridge setup).
- Faithful **drag-out MIDI export** of the current pattern (velocity, pitch, rolls, note length, swing, tempo).
- A bundled **CC0 sample library** (see [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md)).

## Formats & DAW support
- **VST3** — Windows, macOS, Linux (works in most DAWs).
- **AU** — macOS only (Logic, GarageBand, …).
- **Standalone** app — all platforms.
- *(AAX / Pro Tools is not built — that needs a paid Avid license.)*

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
