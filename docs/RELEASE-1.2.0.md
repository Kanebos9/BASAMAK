# BASAMAK v1.2.0

The big theme of this release: **BASAMAK becomes a playable, record-able sketchpad.** Any pitched
sound can now be **played live from a keyboard** and **recorded straight into the pattern** — as
stepped notes *or* as a free, unquantized **Draw** lane — with performances kept as **takes**. On top
of that: a full **sustain/release amp envelope**, real **chords** on the synth engines, and a large
round of engine, quality and workflow work.

## Playing & recording (new)
- **KEYS** — play any eligible channel (Analog+FM oscillator, Physical, Modal) from a MIDI keyboard
  or the on-screen keys. Mono, with the previous held key falling back when you release. Middle C
  (C3) = step-pitch 0, and any channel auto-tunes to the note you press, so recorded pitches play
  back exactly as performed. No mode toggle — incoming MIDI notes just play the selected channel.
- **DRAW MODE** — a free, **unquantized** piano-draw lane per channel (centre line = pitch 0,
  ±36 semitones). Left-drag draws a melody, right-drag erases; a magnify overlay with ±6/12/24/36
  ranges lets you place notes precisely, and a live semitone read-out follows the cursor.
- **RECORDING → TAKES** — record a performance and every loop becomes its own **take** (stepped or
  drawn). Held notes capture their **length** and auto-**merge** across step boundaries, so a
  performance reproduces exactly from the steps. Takes are **per channel**, named by time, capped at
  20 per channel, and **saved with the project/preset**. Load a take onto its channel to view/play
  it, overwrite it, or save your edits as a new take.
- **STEP MERGE** — shift-click ties steps into one sustained note (the note's gate extends across the
  whole run); a purple arrow marks the merged run. Recording auto-merges held notes.

## Chords & voicing (new)
- **STD vs CHORD unison** — two independent settings, only one active at a time. STD is classic
  unison + detune; **CHORD** stacks a chord type (Octave, 5th, Maj, Min, Sus4, Maj7, Min7) — the
  voice **count** picks how many chord notes, and the read-out shows the actual stacked intervals
  (e.g. `Maj (+4 st, +3 st)`). A single step, a held key, or a drawn line plays the whole chord.
- **Real chords on every synth engine** — the Oscillator tunes its unison voices to the intervals;
  **Physical** uses genuine multi-string Karplus–Strong (one string per chord note); **Modal** builds
  a full resonator bank per chord note. Single-voice = bit-identical to before.
- **Vibrato** now works on Physical and Modal too.

## Envelope & engine flexibility
- **Unified A-H-D-S-R amp envelope** — Sustain and Release are real, drawable handles again. While a
  note is **gated** (a held key, or a step's Note Length) the decay settles at Sustain and then falls
  with Release; one-shots and TEST hits stay a tight Attack-Hold-Decay. **Sustain 0 (every factory
  sound) = exactly the old behaviour**, verified bit-for-bit.
- **Release is a real handle on Physical & Modal** (Strike | Ring+Sustain | Release) — whatever you
  set is the key-up fade (no hidden floors). The sustaining Keys-bank sounds ship with musical
  releases; factory sounds are unaffected.
- **Physical pluck position redesigned** for an audible difference across the string, and a
  sustaining string can be **bowed** (held key → near-infinite ring) when Sustain is up.
- **Modal** gained working Sustain/Release from the UI and a "bow the bell" hold (a held bell settles
  into a pure singing tone).

## New sounds
- A **Keys** sound-bank category: Keys Bass, E-Piano, Soft Pad, Organ, Square Lead, String Keys —
  authored with real sustain/release for playing and holding.

## Fixes & polish
- **Reverb is audible now** — the FDN return got a make-up gain (it was ~12 dB down and effectively
  silent). The **Wet** knob is the amount (live); the master **Decay** knob shapes the reverb **tail**
  only — it does not touch the dry sound.
- **Step-count / Draw dropdown "click-twice" fixed** — the strip list is now updated change-only, so
  it no longer fights an open popup at the UI refresh rate. Picking a step count while drawing
  quantizes the drawing to the grid.
- **Loading or switching a preset stops the transport.**
- **Smoother playhead** — the editor runs at 60 Hz, with the heavy project-hashing throttled and
  skipped while recording (fixes a record-time stutter).
- **Launchpad note-mode removed** — pad controllers are used purely as **CC + MIDI-learn** (via
  Novation Components / TouchOSC), which is more flexible and fixed a bug where pad parsing ate part
  of the piano range even with nothing connected.
- **EQ target follows the selected slot.**
- Chord interval read-outs show units (`+4 st`), and many tooltips were rewritten.

## Release packaging
- **Download zips now carry the version** in the filename — `BASAMAK-<version>-<os>.zip` — instead of
  every release sharing the same name.

## Known gaps (next)
- Take names use the wall clock (duplicates possible within the same second).
- Unison caps differ by engine (Oscillator 7, Modal/Physical 3), reflected in the UI handle.

---

*Free / open-source (GNU AGPL v3). VST3 · AU (macOS) · Standalone, for macOS / Windows / Linux.
Built-in sounds are synthesized; a CC0 sample library is bundled.*
