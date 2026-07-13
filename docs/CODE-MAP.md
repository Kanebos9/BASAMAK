# BASAMAK CODE MAP — a guide for new developers

*Written 2026-07-13 22:20 (the v1.4.0 audio-rate release; supersedes the old ARCHITECTURE.md,
deleted the same day). Line numbers drift, so this map gives
you SEARCH STRINGS instead — copy the quoted text into your editor's project-wide search.*

## What this plugin is

BASAMAK is a JUCE drum/sample step-sequencer **instrument** plugin (VST3 / AU / Standalone).
Sounds are synthesized (samples optional): each of the 16 channels has **two SLOTS**, each slot
picks an engine (Oscillator, Noise, Sample, Karplus-Strong, Modal, Granular) and carries its own
envelope, filters, FX and modulation matrix. 32 patterns, per-step values, a piano roll, a live
keys engine with MPE, and a per-slot **audio-rate modulation matrix**.

## File map (start here)

| File | What lives there |
|---|---|
| `Source/dsp/DrumChannel.h/.cpp` | The whole per-channel DSP: engines, voices, envelopes, filters, the mod matrix, persistence of channel/slot data. The `.cpp` has a FILE MAP banner at the top. |
| `Source/dsp/Adaa.h` | Anti-aliased waveshapers (ADAA). Self-contained; read its header comment for the theory. |
| `Source/dsp/SincTable.h` | Polyphase sinc sample interpolation table. Self-contained. |
| `Source/dsp/FDNReverb.h` | The master reverb (8-line FDN + input diffusion). Self-contained. |
| `Source/dsp/Sequencer.h/.cpp` | Patterns, transport, the trigger scan (which step fires when), swing, chains, merged-pattern groups. |
| `Source/dsp/SpectrumTap.h` | Spectrum + tuner audio taps. |
| `Source/plugin/PluginProcessor.h/.cpp` | The plugin shell: MIDI in (keys, MPE, MIDI-learn), the master chain (reverb/delay buses, Tilt/Sat/Glue/Limiter), whole-project state save/load. FILE MAP banner at the top. |
| `Source/plugin/FactoryContent.cpp` | Every factory sound and song preset, as code ("builders"). |
| `Source/ui/PluginEditor.h/.cpp` | The whole editor. Component classes first, then the editor implementation. FILE MAP banner at the top. |
| `Source/ui/StepGridComponent.cpp` | The step grid + piano roll component (shares `PluginEditor.h`). |
| `Source/midi/MidiLearnManager.*` | CC-to-parameter learning. |

## The five concepts you need before touching DSP

1. **The render**: search `"renderInto"` in DrumChannel.cpp. Per block it *bakes* each slot's
   config (`bakeSlot`) into a `SC` struct, then runs a **voice loop**; inside each voice, a
   **per-sample loop** evaluates the engines. The engine runs at 2x oversampling.
2. **The mod matrix**: 12 routes per slot (`Slot::mod[]`), each SOURCE → TARGET × amount.
   "Hot" targets (pitch, volume, cutoffs, drive, ring, sub, punch, warp, wave position, FM
   amount) are applied **per sample** — search `"AUDIO-RATE MODULATION"`. Everything else is
   applied per block in `applyModMatrix`. Envelope times are latched **once per hit** (search
   `"LATCHED ONCE PER NOTE"` for why — stateless envelopes crackle otherwise).
3. **Bit-identical defaults**: any new feature at its default value must produce *byte-for-byte*
   the old output. The test suite enforces this (`tests/run.sh`, 18 tests).
4. **Persistence checklist**: a new per-sound field must appear in `writeSlots`/`readSlots` (or
   `writeChannel`/`readChannel`), `channelSoundHash`, and reset paths. Search CLAUDE.md for
   "Adding a new per-channel parameter".
5. **Anti-gunshot safety**: the reverb/KS/master clamps and NaN guards are load-bearing. Never
   remove a `jlimit(±...)` on a feedback write.

## Dated notes convention

Everything added since 2026-07-13 carries an inline stamp like `[2026-07-13 19:57]` — search
`[2026-07-` to see every recent touchpoint grouped by feature. Older code predates the
convention; its history lives in `CLAUDE.md` (current state) and `docs/HISTORY.md` (the why).

## Build & test

- Verify a change: `cmake --build build --config Release --target BASAMAK_Standalone`
- Test-install for a DAW: `--target BASAMAK_VST3` (auto-installs; keep exactly ONE bundle).
- Full release build: `./rebuild.sh`. Version lives in ONE place: `project(BASAMAK VERSION x.y.z)`.
- Regression suite: `./tests/run.sh` (must pass before any release); sound-quality audits:
  `BankAudit` (similarity) and `UIAudit` (no hidden parameters) in `build_tests/`.
