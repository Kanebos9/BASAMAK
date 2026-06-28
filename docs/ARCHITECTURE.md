# BASAMAK — Architecture & How It Works

A guide for anyone opening the code (e.g. in VS Code) who wants to understand
what the plugin does and where to change things. It assumes basic familiarity
with C++ and the [JUCE](https://juce.com) plugin model (an `AudioProcessor` that
does the audio + state, and an `AudioProcessorEditor` that is the UI).

---

## 1. Big picture

```
        ┌─────────────────────────── PluginProcessor ───────────────────────────┐
        │  processBlock():                                                       │
        │    Sequencer.processBlock()  → fires step triggers, renders 8 channels │
        │        each DrumChannel.renderInto()  (the 4-source voice engine)      │
        │    processDelay() / processReverb()   (shared send FX)                 │
        │    master output: volume · pan · mono · safety limiter                 │
        │  getStateInformation()/setStateInformation(): full ValueTree state     │
        └────────────────────────────────────────────────────────────────────────┘
                                   ▲ params                 ▲ live data
                                   │                        │ (spectrum tap, playhead)
        ┌──────────────────────────┴────────────── PluginEditor ─────────────────┐
        │  Fixed design canvas (DESIGN_W × DESIGN_H) scaled to fit the window.    │
        │  Toolbar · pattern row · 8 channel strips + step grid · detail panel.   │
        │  A 24 Hz timer drives the playhead, meters, spectrum and "modified *".  │
        └────────────────────────────────────────────────────────────────────────┘
```

Source tree:

```
Source/
  dsp/
    DrumChannel.{h,cpp}   one channel: the 4-source voice engine + per-channel FX
    Sequencer.{h,cpp}     16 patterns × 8 channels, timing, swing, play modes
    SpectrumTap.h         lock-free hand-off of a triggered audio block to the UI FFT
  plugin/
    PluginProcessor.{h,cpp}  audio callback, master FX, full state save/load
    FactoryContent.{h,cpp}   read-only factory sounds + groove presets (code, not files)
    UserPaths.h              where the user library lives on disk (+ first-run migration)
  ui/
    PluginEditor.{h,cpp}     ALL the UI: components, layout, presets, sounds
    (SoundPad, WaveformDisplay, FrequencyDisplay, knobs, toggles live in PluginEditor.h)
  midi/
    MidiLearnManager, LaunchpadController
```

---

## 2. The sound engine (`DrumChannel`)

Each channel mixes up to **four sources**, blended by a 2‑D pad, into one mono
signal that then runs through the channel's envelope/EQ/filter/drive:

| Source | What it is | Key params |
|--------|------------|------------|
| **Sample** | a loaded audio file, varispeed + region + pitch envelope | `playSpeed`, `useRegion`, `sampleStart/End` |
| **Noise** | white/pink/brown/grey/purple, *optionally* band‑filtered | `noiseType`, `layerNoiseCenter/Width/Decay` |
| **Oscillator** | sine/triangle/square/saw with its own pitch envelope | `layerOscShape`, `layerSineFreq`, `layerSinePEnv*` |
| **FM** | 2‑operator FM (carrier + ratio‑tuned modulator) | `fmPitch`, `fmSpread` (ratio), `fmDepth` (index), `fmDecay` |

Blend weights `srcWeight[4]` come from the pad position (`padX/padY` + `srcOn[]`);
see `SoundPad::recompute()` for the bilinear/triangle/slider math.

**Voices & polyphony.** A channel holds a small voice pool (`POLY = 8`). With
*Overlap* off it's monophonic (a retrigger restarts voice 0); with it on, voices
ring out together. `trigger()` starts a voice; `renderInto()` sums the active
voices.

**Global pitch.** The Pitch / Pitch‑Env / Pitch‑Time knobs in *Pitch & Level*
apply to **every** source: the static part is folded into the per‑block
constants (FM carrier/mod frequency, noise band centre) and the envelope part is
applied per‑sample to the oscillator and FM phase increments and to the sample
read speed. (See `gPitchMul` / `staticPitchMul` in `renderInto`.)

**Noise filter is optional.** `Width = 0` bypasses the band‑pass entirely so you
get raw white/brown/… noise; the *Center* knob only bites once `Width > 0`.

**Per‑channel chain order:** sources → blend → AHDSR envelope → multimode filter
(`updateFilter`) → drive (`applyDrive`) → 8‑band EQ (`applyEQ`). EQ band gains
are in dB, so big boosts raise loudness — see the master limiter below.

---

## 3. Sequencer & timing (`Sequencer`)

- 16 independent **patterns**, each with 8 `DrumChannel`s, its own **swing**, a
  **play mode** (loop forever / stop after N bars / jump to another pattern), and
  per‑channel **step counts** (any of a set including odd values like 7, 9, 24…).
- Timing is a fractional **bar position 0..1**. In **standalone** mode it
  advances from the plugin's own BPM and time signature; in **DAW‑sync** mode it
  follows the host transport/PPQ (then BPM + time‑sig are locked to the host).
- `swungStep()` maps bar position → step, delaying off‑steps for groove.
- On every step boundary the inspected channel's `SpectrumTap` is armed so the
  EQ/spectrum view refreshes once per step.

---

## 4. Master FX & output (`PluginProcessor`)

- **Reverb** (Size/Decay/Wet) and **Delay** (Time/Feedback/Sync) are **send**
  effects: a channel is heard through them only when its *Sends → Reverb/Delay*
  knob is up (`maxSend` gates them). Delay can be tempo‑synced.
- **Master output:** Volume · Pan · **Limit** · Mono. The **Limiter** is a
  brick‑wall on the final mix; even at 0 it holds the ceiling just below 0 dBFS
  so a heavy EQ/volume boost can't spike loud enough to make a DAW clip or
  auto‑mute the track. Turn it up to squash peaks harder.

---

## 5. UI (`PluginEditor`)

The editor draws onto a **fixed design canvas** (`DESIGN_W × DESIGN_H`) and
scales it with an `AffineTransform` to whatever size the window is, so the layout
is resolution‑independent. `layoutContent()` positions everything; `paintContent()`
draws the frame, the `davulSEQ` logo + version badge, the group boxes, etc.

Layout regions: **toolbar** (logo, DAW‑sync, BPM, transport, preset menu, time
signature) · **pattern row** (16 pattern buttons, play‑mode, loop count, swing) ·
**8 channel strips + step grid** · **detail panel** for the selected channel
(Sounds pad + per‑source knobs on top; Envelope/EQ/Pitch&Level/Sends/Filter/Drive
below; Master FX/Output on the right).

A `startTimerHz(24)` callback syncs the playhead, strip states, the live spectrum
(FFT of the tapped block), and the **modified `*`** detection (see §7).

---

## 6. Factory content (`FactoryContent`)

Factory **sounds** (14 Osc/Noise/FM sounds) and **presets** (6 grooves with
BPM + time signature + steps) are defined in **code**, so users can load and tweak
them but never overwrite them — *Save* always writes a new file to the user
library. The strip dropdown shows `Factory sounds` vs `Your sounds`; the preset
menu shows `Initialize new preset`, factory grooves, then the user's presets.

Each pattern/channel remembers its own selected mix (`DrumChannel::mixName`), so
switching patterns shows the right name per lane.

---

## 7. State, presets & the "modified *" marker

- **Whole‑plugin state** (used by the DAW *and* by `*.basamakpreset` presets) is a
  JUCE `ValueTree` written in `getStateInformation` / read in
  `setStateInformation`: every channel param, all 16 patterns, BPM, time
  signature, master FX/output, etc.
- **Saved sounds** (`*.basamaksound`) are single‑channel ValueTrees (`writeChannelMix`
  / `readChannelMix`). Samples are referenced by path, not embedded.
- The **`*` indicator**: a cheap rolling hash of the channel's sound params
  (`channelSoundHash`) and of the whole instrument (`stateHash`) is compared each
  timer tick against a baseline captured when a mix/preset was loaded or saved.
  When they differ, the dropdown name is prefixed with `*` (or just `*` if nothing
  was selected).

---

## 8. Where user data lives (`UserPaths`)

One cross‑platform folder, **outside** the plugin binary, so updates/uninstalls
never touch it:

```
<Documents>/BASAMAK/Samples       Sound Bank       Presets
```

On first run each sub‑folder is seeded once by **copying** from the older,
inconsistently‑named pre‑1.0 folders (the originals are left untouched).

---

## 9. Building & distribution

`CMakeLists.txt` defines the plugin via `juce_add_plugin` (VST3 + AU +
Standalone). `project(... VERSION 1.0.0)` is the single source of truth for the
version (exposed to the code as `DAVULSEQ_VERSION`). Pass `-DDAVULSEQ_RELEASE=ON`
for a clean, no‑auto‑install build. See `README.md` and `Distribution/` for the
per‑OS build/install scripts and the GitHub Actions workflow that builds all
three platforms.

> Heads‑up for packaging/CI: the factory sample library is **not** committed to
> the repo. `package-macos.sh` pulls it from `~/Documents/DrumSequencer Samples`
> by default (override with `SAMPLES_SRC=…`). To have CI bundle samples, add them
> to the repo (or attach them to the release) and copy them in the workflow.
```
