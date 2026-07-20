# BASAMAK — FULL FEATURE INVENTORY + "BIG SYNTH" COMPARISON (2026-07-07, v1.3.3-dev)

> Written on the user's order after the unison-width pan-law miss: ONE complete audit of every
> feature we have, checked against what the big plugins do, so nothing gets "discovered" to be
> naive later. Keep this file CURRENT when features are added. Verdict legend:
> **[=] matches industry standard  [+] ahead of class  [~] simpler BY USER DECISION (on record)
> [GAP] genuine gap worth considering  [x] deliberately rejected (do not re-add)**

Compared against: **Serum 2 / Vital** (synth engines, unison, mod), **NI Battery 4 / Maschine**
(drum sampler), **Elektron Digitakt/Analog Rytm** (step sequencing), **Roland TR-8S** (drum
machine), **Arturia Pigments** (hybrid), **Scaler 2 / Komplete Kontrol** (scale/chord tools).

---

## 1. SEQUENCER

| Feature | Ours | Big-box reference | Verdict |
|---|---|---|---|
| Patterns | 32, one bar each, chainable | Elektron 128+, TR-8S 128 | [~] fewer but with merge-groups (below) |
| Channels | 16 | Battery 128 cells, Rytm 12 | [=] |
| Steps | up to 64/channel, per-channel count (polymeter) | Elektron 64 + per-track length | [=] |
| Per-step velocity | yes | all | [=] |
| Per-step pitch | +/-48 st | Elektron p-lock, TR-8S sub-steps | [=] |
| Per-step note length | decay-rescale semantics | Elektron | [=] |
| Ratchets/rolls | 1-6 sub-hits + velocity ramp | Elektron retrig, TR-8S | [=] |
| Per-step pan | true per-voice | rare (p-lock only) | [+] |
| Micro-timing | per-step Nudge +/-half step, ms read-out | Elektron micro-timing | [=] |
| Loop conditions | 1..10 cycle masks | Elektron trig conditions | [=] (theirs also have %, we removed probability [x]) |
| Slide/tie | per-step 303 slide toward next | TB-303, Elektron | [=] |
| Step merge | chain steps into one gated note | rare | [+] |
| Swing | full MPC 50-75% | all | [=] |
| Humanize | slot-2 delay + vel jitter knob | Maschine humanize | [=] |
| Pattern chain | per-pattern (target, loops) list | song modes | [~] no full song timeline (arrangement lives in the DAW) |
| Pattern merge groups | up to 8 bars as one unit, multi-bar view | Elektron chains | [+] (visual concat editing is unusual) |
| **Parameter locks (per-step SOUND automation)** | NO - per-step is vel/pitch/len/roll/pan/nudge only | **Elektron's signature feature** | **[GAP]** the one sequencer feature class we lack: automating e.g. cutoff per step. Nearest workaround: MIDI-learn + DAW automation of CCs. Big surgery (per-step param snapshots), flag only if wanted |
| Probability | REMOVED | Elektron/TR-8S have it | [x] user hates it |

## 2. PIANO ROLL / RECORDING / KEYS

| Feature | Ours | Reference | Verdict |
|---|---|---|---|
| Poly note-list roll | 256 notes/bar, per-note vel/len/slot/glide | DAW rolls | [=] for in-plugin rolls (most drum plugins have NONE) [+] |
| Per-note SLOT targeting | orange/yellow/pink = both/1/2 | none | [+] unique |
| Ghost voicing lines | shows real chord/scale voicing per note | none | [+] |
| Multi-select, move, resize, grid 1-64 | yes | DAW standard | [=] |
| Draw-audition | hear pitch while drawing | DAW standard | [=] |
| Recording | loop takes, count-in, auto-merge, hold-length, unquantized draw | Maschine/MPC loop record | [=] |
| Takes | 20/channel-pattern, persisted, save-edits | rare in plugins | [+] |
| Poly keyboard + per-note release | 16 voices | all synths | [=] |
| Mono glide/portamento + recordable per-note glide flag | yes | synth standard | [=] |
| Arpeggiator | SH-101-style offset riff, step-synced, align/hold/gate | synth arps arp held chords | [~] user-designed; different animal, documented |
| Scale harmonizer | per-slot diatonic chords, 10 scales, snap | Scaler 2 / Komplete Kontrol | [=] genuinely competitive |
| Part generator (v1.5.4) | GENERATE in the roll: role-based (bassline/melody/hum/riff), reads the groove + key symbolically, seed-locked iteration (same-rhythm/same-notes/vary), one-shot dice, singable mode | Captain Melody / Scaler 2 phrases / Orb Producer | [=] rule-based like theirs; ours reads the user's OWN groove + writes editable roll notes; vocal-guide (hum + record-your-own-hum multisample) combo is unique |
| Live chord naming | 3 readouts, full dictionary to 13ths | Scaler 2 | [=] |
| Key guide (dim non-scale keys) | yes | Komplete Kontrol light guide | [=] |
| Split keyboard | channel pairing, per-half 4-oct windows, per-half recording | workstation splits | [=] |
| MPE / poly aftertouch | no | Serum 2, Vital have MPE | [GAP] niche for a drum plugin; note-on velocity only |

## 3. SOUND ENGINE (per slot; 2 slots per channel)

| Feature | Ours | Reference | Verdict |
|---|---|---|---|
| Engines | Sample, Noise, Analog+FM osc, Karplus-Strong, Modal | Pigments (VA/wavetable/granular/sample), Serum (WT) | [~] no wavetable/granular - we are synthesis-first drums, not a WT synth |
| Osc shapes | 14 band-limited (PolyBLEP + additive bank) | Serum: arbitrary wavetables | [~] user pruned duplicates; wavetable import = different product |
| FM | ratio-snapped 2-op + feedback + sub, env-follow, anti-aliased | Serum 2 FM engines | [=] for 2-op scope |
| Warp | one-way wavefold | Serum wavetable warps (30+) | [~] one good one vs many |
| Unison | 7 voices, directional detune, ALTERNATING-pair stereo width, chord/scale stacking | Serum 16 voices, random phase, blend curves | [~] count 7 vs 16 fine for drums; deterministic phase = identical-hits rule (user rule); width law NOW matches Serum-style alternation |
| Amp env | A-H-D-S-R, gated semantics, per-engine adaptations | ADSR standard | [=] |
| Pitch env | 4-dot bipolar, all engines incl. modal | drum-synth standard | [=] |
| Filter | per-slot ZDF/TPT SVF LP, per-sample smoothed, env + LFO | Serum/Vital ZDF multi-mode | [GAP-small] ours is LP-only per slot (channel Formant exists). HP/BP/notch per slot = cheap to add if ever wanted; same SVF gives all outputs |
| LFOs | 3 per slot (filter/pitch/vol), retrigger, sine | Serum 10 shapeable, tempo-sync, mod matrix | [~] fixed-dest by design (no mod-matrix UI wanted); no tempo-sync = deliberate (locked to hit) |
| Mod matrix | none - direct wired | Serum/Vital/Pigments huge matrices | [~] explicit design: drums want immediacy; MIDI-learn covers control |
| Karplus-Strong | multi-string (3) chords, stiffness dispersion (audible), sustain-hold/ebow, position comb | rare in this class | [+] |
| Modal | 16 modes, 10 materials + morph, bow-the-bell sustain, full banks per chord note | Chromaphone is the benchmark (deeper) | [=] for a built-in engine |
| Noise | 5 colours, reso, drive, crackle | standard | [=] |
| Sample | host-rate resample at load, Hermite, 4 round-robin regions, reverse, crush, stretch/pitch bake (SoundTouch), keep-pitch | Battery: velocity layers, humanize round-robin | [GAP-small] no velocity layers/multisample per cell; regions cover round-robin. Battery-style layering = a sampler product's core, ours is a synth-first box |
| Per-slot EQ | HP + 3 bells + LP (24 dB Butterworth ends) | Battery per-cell EQ | [=] |
| Per-slot FX | drive insert + reverb/delay sends | Battery per-cell inserts | [~] fewer insert types (no chorus/phaser per slot) |
| Oversampling | whole engine 2x | Serum 2 up to 8x per-note | [~] fixed 2x is a deliberate CPU/quality point for drums; no user toggle |

## 4. CHANNEL / MIXER / MASTER

| Feature | Ours | Reference | Verdict |
|---|---|---|---|
| Choke groups | 8, mutual, 3 ms fade | all drum machines | [=] |
| Sidechain duck | per-channel duck-by + amount, zero-UI popup | Rytm/live mixers | [+] built-in duck is not standard in this class |
| Routing | main + 16 aux stereo outs | Battery/Maschine multi-out | [=] |
| MIDI out per channel | notes w/ length, sample-accurate | rare in drum plugins | [+] |
| Master FX | FDN reverb, synced delay w/ ping-pong, tilt EQ, tube sat, glue comp, lookahead limiter, mono | TR-8S master FX | [=] |
| Per-channel inserts (comp/transient shaper) | no (drive/EQ per slot only) | Battery has per-cell comp/transient master | [GAP-small] a transient shaper is THE drum-plugin insert we lack; per-slot drive + env attack covers much of it |
| Metering | master + per-channel peak-hold, spectrum analyzer | standard | [=] |

## 5. CONTROL / IO / UX

| Feature | Ours | Reference | Verdict |
|---|---|---|---|
| MIDI-learn | EVERY control + 4096 step targets, maps as files, TouchOSC template | rare at this depth | [+] |
| Host automation | REMOVED (empty param list) | all big plugins expose params | [x] user decision on record; MIDI-learn is the path. This is the biggest "reviewer would flag it" divergence |
| Drag-MIDI export | voiced per-slot chords/scales, merged notes, rolls, swing | none do voiced export | [+] |
| Audio export | no | some render stems | [x] rejected ("DAWs render") |
| Undo | 24 whole-state snapshots + Cmd/Ctrl+Z/+Shift+Z | standard | [=] |
| Presets | 2 factory presets + factory sound bank + user library folders | hundreds of presets, tag browsers | [GAP-content, not code] the browser is fine; the CONTENT count is small. More factory sounds/presets = authoring work, not architecture |
| Tooltips | structured, plugin-wide, toggleable | rare | [+] |
| Resizable/HiDPI UI | fixed 1510 design scaled | Serum 2 fully resizable | [~] scales with host zoom; no free resize handle |

---

## THE HONEST GAP SHORTLIST (everything above folded down)

Things a "big synth" reviewer would name, in rough order of real-world value to THIS plugin:
1. **Per-step parameter locks** (Elektron-style) - the one missing sequencer feature class. Big surgery.
2. **Transient shaper / per-channel compressor insert** - the classic drum-bus tool we don't have.
3. **Per-slot filter types beyond LP** (HP/BP/notch from the same SVF) - cheap, low risk.
4. **Velocity layers on samples** (Battery-style) - only matters if sample-first use grows.
5. **MPE** - niche here.
6. Factory content volume (authoring time, not code).

Everything else is either at standard, ahead of it, or simpler BY A DECISION THE USER MADE
(each marked [~]/[x] above with the decision noted). The unison width pan law was the one place
we shipped a naive version of a solved problem - fixed 2026-07-07 (alternating pairs), and this
document is the standing check so that class of miss doesn't repeat: **when building a NEW
feature, check this file's reference column / the industry standard FIRST, then build.**
