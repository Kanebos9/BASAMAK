# BASAMAK v1.1.0

The big theme of this release: **BASAMAK is now a drum *and bass* instrument.** Any channel can
be played as a bass line or a lead, with per-step notes, glide and note-length — and there's a new
master tone section, a resonant filter and an LFO per sound, plus a large round of quality,
performance and workflow fixes.

## Bass & melodic playing (new)
- **Per-step Slide** — 303-style portamento: a slid step plays its own attack and glides across the
  step to land on the next step's pitch (draw notes in Pitch mode, then toggle the "slide" band at
  the bottom of a cell). Great for acid/303 basslines.
- **Per-step Note Length** — one control from tight gated hits to long ring-outs; the note's decay
  is rescaled to fill the length (long = slow fall like a synth note, short = tight gate).
- **Per-slot resonant Low-Pass Filter with envelope + velocity accent**, drawn right on the EQ
  display (the orange "F" diamond — drag for cutoff, wheel for resonance, an arrow for the sweep).
- **Per-slot LFO** with three independent destinations at once — filter cutoff, pitch, and volume —
  for wobble bass, sirens, vibrato and tremolo. Interactive visual with live recipes in the tooltip.
- **Hz ↔ note read-out** — click a frequency value to switch to note names (A1, F#2…) with
  semitone snapping, so you can dial in real notes.
- **Bass sound bank** — Station, Ladder, Rubber, Neuro, Hoover, Reese and Reed basses, plus a
  Wobble Bass, built for the new Length/Slide/filter/LFO controls. New FX toys: Siren, Chopper.
- **Keys mode pitch-tracks** — an incoming note that matches no drum plays the selected channel
  transposed, so any sound is playable from a keyboard.

## Master bus (new)
- **Tilt** — one-knob tone for the whole mix (dark/warm ↔ bright) around a ~700 Hz pivot.
- **Saturation** — warm, tube-style harmonic drive (distinct from Glue's compression).
- **Delay Wet** knob (the return level is a real control now).

## Sound engines & quality
- **Analog + FM merged engine** — 14 band-limited waveshapes (near-duplicates removed), one-way
  wavefold "Warp", FM amount env-follow, FM ratio snapping to integers.
- **Interactive engine controllers** — the Physical string pluck-triangle and the Modal
  standing-wave are the parameters (drag them), not decorations.
- **Modal pitch envelope now works**; **Physical stiffness** redesigned for an audible
  string→bar→bell partial stretch.
- **Samples resampled to the host rate on load** (fixes a 48 kHz file playing flat in a 44.1 kHz
  session), with a decode cache; **live waveform playhead**; **drag-and-drop audio** onto any slot;
  **opt-in sample amp envelope** (double-click the graph).
- **Factory bank refined** — punchier kicks (real pitch knock + click layers), brighter/cleaner
  snares, a distinct kick lineup, and ~22 weak/duplicate sounds removed (quality over quantity).

## Sequencer & workflow
- **Sample-accurate triggering** — steps land exactly on the grid instead of snapping to buffer
  starts (audible on large buffers / rolls).
- **Swing** extended to the full MPC 50–75% range ("Off" at 50%).
- **Step magnifier** — hold a step in any value mode and it zooms **2×** under your cursor for
  precise edits (and floats above the toolbar).
- Clearer **active-step marking** in Pitch/Pan modes.
- **Faithful drag-out MIDI export** — velocity, per-step pitch, rolls + ramps, note length, swing,
  tempo and time-signature.

## Fixes & polish
- **Cross-pattern solo** fixed; **meter peak-hold** fixed; **choke** is now a 3 ms fade (no click).
- **Rotary knobs jump to the clicked position**; **double-click resets to factory default**.
- **A fresh Standalone / newly-added plugin opens at factory defaults** — only a saved DAW project
  restores its saved settings.
- **Auto Test on by default** (auditions your edits).
- Dropdowns dismiss on outside clicks; **lazy Karplus-Strong buffers** cut ~130 MB of idle RAM;
  various CPU/memory trims and anti-glitch safety hardening.

## Presets
- Trimmed to two curated presets: **Riser + Drop**, and **Ode to Joy (Beethoven)** — a full
  public-domain melody arranged across eight chained patterns to show off pattern chaining and
  melodic playing.

---

*Free / open-source (GNU AGPL v3). VST3 · AU (macOS) · Standalone, for macOS / Windows / Linux.
Built-in sounds are synthesized; a CC0 sample library is bundled.*
