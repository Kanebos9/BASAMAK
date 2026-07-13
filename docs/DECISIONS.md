# BASAMAK — UNDER-THE-HOOD DECISION REGISTER (started 2026-07-09)

> Written on the user's order after the "empty-roll import guard" surprise: EVERY behaviour-level
> decision Claude made without an explicit user choice, in one place, so nothing has to be
> "discovered" in use. Companion to docs/FEATURES.md (which covers the same for industry-standard
> comparisons). **Standing rule: any new decision of this kind gets a line here IN THE SAME
> COMMIT that introduces it.** Items the user explicitly specced are NOT listed (that's the
> plugin's design, not a hidden call) — except where a spec left a gap that was filled silently;
> the filled gap is what's listed.
>
> Status column: **OPEN** = never explicitly reviewed by the user; **EXPLAINED** = surfaced in a
> session and the user replied/moved on; **APPROVED** = user said yes after the fact.
> Challenge any line and it gets rebuilt to your spec.

## Piano roll <-> steps round trip

| # | Decision | Why | Status |
|---|---|---|---|
| 1 | ~~Import only when the roll is empty~~ **REBUILT to user spec (2026-07-09): quantizing roll->steps FORGETS the roll, so switching back ALWAYS re-imports the steps (with compensation + warnings)** | user: "it should forget how piano roll was. So it should just be converted again" | RESOLVED |
| 2 | ~~Roll notes kept underneath after quantize~~ **REBUILT (2026-07-09): quantize clears them; the round trip is a fresh conversion each way** | user: "I prefer lossy re-import. There is an undo button anyway" | RESOLVED |
| 3 | Quantize: several same-pitch notes starting in one step become a step ROLL; the ramp is recovered by inverting the engine's velocity law; step Vel = the louder end | round-trips ratchets instead of dropping them | OPEN |
| 4 | Quantize: a mixed-pitch cluster inside one step keeps only the largest-overlap note | a step holds ONE pitch; something had to win | OPEN |
| 5 | Quantize: gate fractions >= 97% round to full; a merged chain's fractional tail is written on the HEAD step | avoids 99%-gate noise; merged cells have no own Gate | OPEN |
| 6 | Quantize: a merge whose gate never reaches past the head step is COLLAPSED to one unmerged step | your question "what's the use of merging two steps?" — the combo was meaningless | EXPLAINED (2026-07-09) |
| 7 | Roll->steps DROPS per-note strum direction/amount (the arp accent survives in velocity) | steps have no per-step strum vocabulary | EXPLAINED (disclosed when built) |
| 8 | Import marks bare steps + ratchet sub-hits Gate OFF, merge runs + Length steps gated | keeps the mode switch bit-identical in sound | APPROVED (part of the one-shot plan you approved) |
| 9 | Import base compensation: notes shift by the knob's whole-semitone distance from C4, leftover cents go to the Tune fader | keeps the ABSOLUTE pitch identical across the mode switch (60 Hz stays 60 Hz) | EXPLAINED (2026-07-09) |
| 10 | The two import warnings fire only for real losses: fractional per-step pitches (rounded) or notes clamped at the C0-C8 edge; a between-notes KNOB alone warns nothing (it's lossless via Tune) | don't warn when nothing audible changes | EXPLAINED (2026-07-09) |

## Recording

| # | Decision | Why | Status |
|---|---|---|---|
| 11 | Arp recording: when jitter makes stamps overlap, the PREVIOUS note's length is trimmed (recording itself stays unquantized) | replaced the grid-snap you rightly rejected; free timing is the hard rule | EXPLAINED |
| 12 | ~~Upstroke accent baked into recorded velocity~~ **REBUILT (2026-07-09): velocity is stamped clean; the Strum UP flag alone reproduces the accent through the same DSP gate as live.** (The bake + flag combo was double-applying = recorded upstrokes 0.67x live - found by the user's challenge) | user: "Why baked in velocity? Why not to strum or alt strum?" - exactly right | RESOLVED |
| 13 | Recording SKIPS notes beyond +-48 st instead of storing a clamped-wrong pitch (they still play live) | a wrong pitch in the take is worse than a missing note | OPEN |
| 14 | Step recording stamps FRACTIONAL knob-relative pitches (e.g. +12.3) | your "create your own solution... just do" — playback always reproduces the performance | EXPLAINED (you dislike the look; PARKED for a better idea) |
| 15 | Group (multi-bar) recording is looper-style overdub; per-pass draw takes for single bars; a group take can truncate a note still ringing at the pass end | per-pass clearing was wiping bar 1 mid-hold | OPEN (truncation part) |

## Arp / strum / keys

| # | Decision | Why | Status |
|---|---|---|---|
| 16 | Per-note "Strum amount" menu offers Sound's-knob / 0 / 25 / 50 / 75 / 100% (not a free field) | menu = one click; a text field for cents-level strum felt heavy | OPEN (granularity is arbitrary — say the word for a finer control) |
| 17 | Upstroke recipe = x0.7 spread + x0.82 level | order alone measured inaudible in the log session; the accent is the audible cue | EXPLAINED |
| 18 | The arp block is NOT in the sound-modified hash (tweaking a feel-arp doesn't flag the sound `*`) | arp = channel feel unless the sound is a dedicated-arp sound | OPEN |
| 19 | Saving a sound while the channel arp is ON bakes the arp into that sound file | side effect of "arp travels only with dedicated-arp sounds" (your rule); the bake direction was my fill | EXPLAINED (called a trade-off when built) |
| 20 | Live keys play every slot at the target pitch, flattening inter-slot detune (Honky Tonk's 12 cents) while sequenced playback keeps each slot's base | absolute-pitch keys (your spec) can't preserve unequal bases; authoring rule now forbids unequal bases | EXPLAINED (caveat noted when built) |

## Factory sounds / authoring

| # | Decision | Why | Status |
|---|---|---|---|
| 21 | finishSound migrates channel-level sends/drive onto the slots; split drive colours very low velocities slightly differently | your UI-replicability order; sends are linear-equivalent, drive nearly so | EXPLAINED (accepted at the time) |
| 22 | Which categories ship Lock Pitch ON (all drums/FX) vs OFF (Bass/Keys/Pads/Leads/Plucks/Bells/Chords) | you approved the toggle + "drums locked by default"; the exact category split was my fill | OPEN (list in applyMix, trivially editable) |

## Steps<->roll SIMPLIFICATION batch (2026-07-09) — decisions inside a user-directed redesign

> The batch itself (clear-on-switch, remove Lock Pitch, per-note pan/velocity/strum in the roll,
> disable step edit-modes in the roll, per-pattern channel merge, ch 7/8 roll on init, quantize
> box, merged-pattern warnings) was user-directed = design. The rows below are the gap-fills I
> made WITHIN it.

| # | Decision | Why | Status |
|---|---|---|---|
| 26 | Roll ModeVel per-note velocity DRAG + whole-channel ModePan editing were DELETED (not just disabled) | user: "delete their roll-only code" - all step edit-modes are off in the roll; per-note velocity/pan now come from the right-click menu | EXPLAINED (the fine velocity DRAG is gone; right-click velocity is 10% steps - say if you want the drag back as a roll-only tool) |
| 27 | ~~Right-click Velocity = 10% steps~~ **FIXED (2026-07-09, user caught it): right-click Velocity now has "Type exact %..." = FULL 1% resolution (matches the min/max vel knobs), plus 10% quick presets** | the first version shipped the exact coarse/fine MISMATCH the user warned against (coarse menu + continuous knobs); a typed dialog delivers option #1 ("just as sensitive as the knobs") without coarsening anything | RESOLVED |
| 28 | ~~Per-note Pan==0 falls back to channel pan~~ **RESOLVED by #38: pan now uses a 127 sentinel for inherit, so 0 = a true centre** | the 0-means-both ambiguity is gone | RESOLVED |
| 29 | Strum stepped to 0/20/40/60/80/100%; factory strum values rounded (0.45->0.40, 0.55->0.60) | match the knob to the right-click override exactly (no live-vs-record drift) - your request; the rounding keeps factory sounds on the new grid | EXPLAINED |
| 30 | Channel Merge & Split now CLEARS the paired channels' steps (forced roll) | consistency with the new "switching clears" model; it warns first + is undoable | EXPLAINED |
| 31 | Unmerge SPLITS a note that spanned the bar line into a truncated head + a gated continuation clamped to ONE bar | your "it should end and start again where the pattern ends"; clamping to one bar because the freed pattern loops alone | EXPLAINED |
| 32 | Overlap (strip button) FADES in the roll; Duck + Choke stay live in the roll | Overlap is a step-retrigger concept (roll uses per-note length + the keyboard Poly toggle); Duck/Choke are level/gate effects that apply to any trigger incl. roll notes | EXPLAINED (my opinion on your ask) |
| 33 | Fresh instance + Init default channels 7 + 8 to Piano Roll (set in the processor ctor AND initPreset; a loaded project overrides via its saved drawMode) | your request; the ctor covers a brand-new plugin, initPreset covers the Init button | EXPLAINED |
| 34 | Quantize (roll header) snaps note STARTS to 1/N and clamps length to >= one cell, on the selected channel across a merged-pattern group | "quantizing is a one-time thing... do whatever is smart" - starts are what matter for timing; length-clamp avoids zero-length notes | EXPLAINED |

## Code-review cleanup (2026-07-09) — user asked for items 1-12 + 16 of the review

| # | Decision | Why | Status |
|---|---|---|---|
| 35 | Blend-character (bloom/drift/spread/punch/glue) removal is BIT-IDENTICAL (all amounts were hardcoded 0, so every multiply resolved to identity) | remove dead DSP without changing any sound; tests confirm | RESOLVED |
| 36 | Old SrcSynth/SrcWave slots MIGRATE to SrcOsc on load (were rendering silent) | user: "old projects must use current settings, no reliance on obsolete code" - a plain oscillator beats silence | RESOLVED |
| 37 | SrcWave display scaffolding (waveView + waveTable/wavePos fields) KEPT as inert dormant compat, not removed | it never shows after migration (zero runtime effect); removing it = disproportionate layout surgery vs. reward | OPEN (flag if you want it fully stripped) |
| 38 | Per-note pan sentinel = 127 (PAN_INHERIT); explicit -100..100 (0 = true centre) | fixes #28 - "centre" and "inherit" no longer collide; the field is brand-new so no migration risk | RESOLVED |
| 39 | Monolith split = extracted StepGridComponent (1571 lines) to its own .cpp; did NOT split the rest | it's the one large, cleanly-separable, callback-driven unit; splitting DrumSequencerEditor's 100+ methods needs a shared internal header for ~30 file-statics = a bigger, riskier pass | PARTIAL (more splitting possible later) |
| 40 | #13 (dropdown) HARDENED: the per-tick combo refresh now also skips while the popup is OPEN (isPopupActive), not just change-only - can't fight your selection even if the change-only test is ever dropped | structural, not conventional, safety for the recurring "dropdown ignores my pick" bug | RESOLVED |
| 41 | #14 (undo window) CLOSED: doUndo() force-commits any uncommitted current state FIRST, so the stack top always equals the screen - undoing right after an edit steps back exactly one edit | removes the ~0.1 s "undo skips a step" window at its source (no more scattered patches needed) | RESOLVED |
| 43 | Roll is KNOB-INDEPENDENT (plays C4-absolute via slotBaseHz) - you approved it; the Freq knob is no longer forced/faded/parked in the roll | matches "piano roll doesnt care about freq knob"; bit-identical playback, knob free | RESOLVED |
| 44 | RIGHT-DRAG = marquee select (was SHIFT+drag); right-click no-drag = note menu | your request; SHIFT freed | RESOLVED |
| 42 | #15 readability = already handled (labels squeeze, custom + default buttons auto-fit, no "..." strings); the magic-number LAYOUT refactor NOT done | can't visually verify blind layout changes = exactly the "dont break it" risk; will fix specific truncated spots if screenshotted | OPEN (maintainability only; no current readability issue) |

## Engine / infrastructure (behaviour-affecting internals)

| # | Decision | Why | Status |
|---|---|---|---|
| 23 | Loop-wrap fix internals: sub-1/1000-sample remainders snap to 0; bar-start check tolerates a quarter sample | the gate-explosion bugfix needed a tolerance; values chosen conservatively | EXPLAINED (bugfix session) |
| 24 | Take names use the wall clock (same-second duplicates possible) | known gap, never prioritised | OPEN |
| 25 | ~~Old SrcSynth/SrcWave load silent~~ **FIXED (#36): they now migrate to SrcOsc on load** | no reliance on obsolete code | RESOLVED |

| 45 | ADDITIVE = a "Custom" wave INSIDE the Oscillator engine (drawn 32-harmonic table), not a new Source engine | every existing control (envs/filters/unison/FM/FX) works on it for free; band-limiting matches the factory bank shapes | overnight batch (user: "go on, dont ask") |
| 46 | Harmonic editor = in-editor overlay, 32 bars, presets Sine/Saw/Odd/Clear, opened by clicking the Custom wave preview | popup rule (never an OS window); presets give starting points | overnight batch |
| 47 | The 3 new FX params = Tone (tilt +-6 dB @800 Hz) / Punch (transient shaper) / Comp (one-knob, on the slot BUS not per voice) | most useful gaps after the EQ removal (tone fix, drum snap, glue); industry-standard per-cell trio (Battery/TR-8S) | overnight batch |
| 48 | Comp runs on the slot's summed bus (the chorus pull-out buffer), fixed 0.30 threshold / 4 ms / 120 ms, makeup 1+0.8a | true bus compression, not per-voice; constants = effect design like chorus rate | overnight batch |
| 49 | Chorus effect constants: rate 0.36 Hz, depth +-3.5 ms; drive anti-fizz LP = 8 kHz 1-pole on Hard/Fold/Fuzz; punch followers 1.5/50 ms | picked by ear/convention; not exposed (macro-knob philosophy) | overnight batch |
| 50 | Additive factory sounds base = C4 (basses 55/65.41) | step pitch 0 = middle C like the keys/bells families | overnight batch |

| 51 | Spectrum motion = per-note A->B crossfade driven by TIME (log 0.05-4 s), not env/LFO | simplest musical model; env/LFO motion already exists via filter/FM; per-voice restart keeps notes consistent | user-ordered feature, driver choice mine |

| 52 | Wavetable = 4 frames INSIDE the Custom wave (not a new engine); legacy A/B files migrate to {A,B,B,B} + morph x3 (bit-identical crossfade) | user picked 4 frames + in-place growth; the x3 keeps every authored/saved A->B time exact | user-approved plan |
| 53 | WAVE (4th LFO dest) scans +/- half the strip around the base position; per-note GLIDE overrides the static Position while it runs | LFO scan = the classic wavetable wobble; "glide wins" is the simplest audible precedence | my call inside the approved feature |
| 54 | Analyze sample: window starts 25% in (captures the SUSTAIN, skips the attack), Goertzel mag+phase at k*f0, unpitched files report "no clear pitch found" | a frame holds a steady spectrum; report-not-guess = the honesty rule | my call |
| 55 | Factory mkAdd copies frame 0 into all 4 frames (uniform strip) | moving Position on a factory additive sound must be a predictable no-op, not a surprise sine | my call |
| 56 | Per-leg glide (round-2, user spec): addSeg[3], 0 = HOLD at that leg's left frame; seg[0]=0 = glide off entirely | user: "choose morphing time individually"; Hold makes the old morph-then-stay a natural case (no x3 migration trick) | user-approved |
| 57 | Unreachable glide boxes draw dimmed but stay draggable | signal without blocking (user may set legs out of order) | my call |
| 58 | Drift = TRUE random (not seeded): playback passes differ microscopically | user picked it after full explanation (live take is never reproducible either way - only notes are recorded) | user-approved |
| 59 | Free-run LFO = TIMELINE-anchored (bar position -> phase), free clock only when stopped | keeps LIVE==RECORDING determinism: every playback pass identical; "free" wall-clock would differ per pass | my call, disclosed in tooltip |
| 60 | Filter drive = one soft-tanh flavour on v3 inside the SVF loop; no type menu | user: one type like discussed; in-loop = resonance compression (the analog character), types deferred | user-approved |
| 61 | Reverb mode default = Hall = the exact original FDN numbers; modes are re-voicings of the SAME safe network | factory/old projects sound identical; anti-gunshot clamps stay on every path incl. shimmer | my call |
| 62 | Reverb stays ONE shared engine (mode is preset-wide); per-slot reverb rejected | 2 slots x 16 channels = 32 reverb instances = CPU cliff; send/return is the industry model | explained, user accepted |
| 63 | Live Position marker (amber, both strip modes) shows ONLY while a voice renders a Custom table; silent = no marker | honest display: no voice = no position being played (same rule as the LFO playhead); amber = the plugin's playhead colour | my call |
| 64 | MIDI sound browsing = NEXT/PREV buttons ONLY (hold-to-repeat: 450 ms arm, one step/220 ms, 15 s toggle-pad cap, excess dropped); ALL knob decodes (relative / absolute+pawl / 14-bit fine) built then REMOVED at user order | the Launchkey MK4's custom modes are absolute-only, silent at the pegs, and its 14-bit mode rescales the same fixed physical sweep - every knob mode ended in a wall or a recovery gesture the user hated; buttons ARE motion (full saga: docs/HISTORY.md) | user decision |
| 65 | Selected-scope MIDI (ui_sel_*) COEXISTS with the addressed p{P}_* vocabulary - NO global mode toggle; env CCs use a v*v curve; strum snaps to the knob's 0.2 grid; min/max vel push each other; Master Mono not routed (knobs only) | a mode toggle = hidden global state changing what a controller does behind the user's back; two id vocabularies can't collide (one param per CC); curves/snaps mirror the on-screen controls' feel | user approved coexistence; details my call |
| 66 | ALL master CCs write ALL patterns now (were single-pattern; the knobs are preset-wide) + set uiMasterCcDirty so the knobs refresh; ui_selstep_N SETS from value >= 64 (not toggle) | a single-pattern CC write is silently clobbered by the next preset-wide knob move; SET-from-value matches the addressed steps + TouchOSC toggle pads (a toggle-on-press would desync pad LEDs) | my call, disclosed |
| 67 | Prune picks within "remove one" orders: kept Trap Snare (removed 909 Snare), kept Synth Pluck (removed Pluck Synth), kept Vibes (removed Hand Bell + Mod Tuned Bell); renames "Bell Cymbal" + "Mouth Pop" | user said remove/rename but left the pick to me: Trap = the modern search term (909 Kick keeps the 909 flavour), Vibes = the classic instrument name | my call within user order |
| 68 | Pitch-blind BankAudit = render in drawMode (the C4 keys world), not a retune sweep | drawMode IS the existing pitch-normalized path (slotBaseHz) - zero new machinery, exactly matches "what I hear on my keyboard"; side-effect disclosed: drums score higher against each other (tuning no longer counts) | my call, disclosed |
| 69 | MOD MATRIX is applied BLOCK-RATE (a scratch Slot copy modulated before the per-block config bake), not per-sample; sources sampled from the NEWEST active voice | the config bake is already per-block; a per-sample matrix would be a second modulation engine. The four audio-rate LFOs still run smoothly on their own dests - the matrix extends REACH. Newest-voice sampling = poly stacks get one voice's cues (mono-ish, disclosed in the tooltip: "can sound stepped on very fast sources") | Fable's plan, user-approved scope (6 routes, Baseline+ModEnv+ModLfo) |
| 70 | Grid-knob targets ("Engine knob 1..8") applied via a DSP-side mirror (modGridKnob) of slotParamsFor's numeric-param order; choice/stepped params skipped | the DSP translation unit can't link the UI's slotParamsFor (not in the test build); mirror is the established convention (uiLfoShapeVal<->lfoShapeVal, uiScaleSemis<->scaleSemis). UI shows live names, DSP applies by index - a sync-warning comment sits on both | my call inside the approved feature |
| 71 | Per-note Random source = TRUE random per hit (rolled from driftRng, the drift precedent), only when the matrix is live | matches the DRIFT decision (58): passes differ microscopically, only recorded NOTES reproduce; rolled lazily so drum sounds stay bit-identical when no route uses it | my call, mirrors #58 |
| 72 | FX row (final): Crush + Air REMOVED, replaced by FLANGER + PHASER; RING kept | user: Crush == the Drive dropdown's Bitcrush, Air ~= Tone (both redundant); Flanger/Phaser are the two classic modulation FX the plugin lacked (Chorus exists) - distinct from Drive/Tone/Chorus, one-knob, bus inserts, matrix-targetable | user removed Crush/Air; Flanger/Phaser = my pick from the user's Pigments list |
| 74 | Wave Position matrix target = Oscillator-only (modulates addPos); granular grain position = its "Position" grid knob | they modulated the SAME field (grainPos) for granular = a confusing double-listing; splitting them = one clear target per engine + fixes the cancel-on-engine-switch | user flagged the redundancy; split = my call |
| 75 | FX SCOPE SPLIT: per-slot = Drive/Tone/Punch/Ring/Rev+Del sends (the layer's raw voice); per-CHANNEL = Chorus/Flanger/Phaser/Comp (the instrument's space/motion/glue), in a new CHANNEL FX box | the test is "would you routinely want it DIFFERENT between the two layers?" - yes for tone/drive, no for spatial FX (one per layer fights itself: two independent sweeps = phasey mush, 2x CPU, no musical gain). Comp across both layers IS the glue. My earlier "keep it all per-slot" was work-avoidance (factory re-authoring), and the user called it | user decision; the per-effect classification = my call, user approved |
| 76 | Channel-FX signal order = Comp -> Flanger -> Phaser -> Chorus | comp FIRST glues the raw layers; a comp AFTER the modulation FX would pump on their sweeps | my call |
| 77 | Channel FX are modulatable from EITHER slot's matrix ("... (Channel)" targets); both slots' routes ADD into one offset (chFxMod), sampled block-rate from each slot's newest voice | the matrix is per-slot by design but the FX is one instance - summing the two slots' offsets is the only scope-crossing rule that stays predictable (and per-voice is meaningless for a single shared effect) | my call inside the user's "wire it that way" order |
| 78 | Old files' per-slot chorus/comp migrate to the channel by taking the STRONGEST slot's amount | one instance now serves both layers; max preserves the effect's presence (averaging would silently weaken every migrated sound) | my call, disclosed |
| 79 | BELL filter: Y (the reso field) = BOOST in dB with a FIXED musical Q (1.1), not a separate gain field | keeps the diamond's one-gesture drag (X = freq, Y = amount) + zero new persistence; a Q control would need a third axis for marginal gain - revisit only if asked | user asked for a boosting type; the one-knob mapping = my call |
| 80 | De-zipper poles are EFFECT CONSTANTS (weight ~3 ms, filter K ~2 ms, channel-FX mix ~4 ms), always on | short enough to be inaudible as smoothing, long enough to kill block-rate steps; constant values snap-init so unmodulated sounds stay bit-identical | my call (fix for the user's crackle report) |
| 81 | A modulated slot is NEVER liveness-skipped on its modulated weight (base weight decides); pitch/drive/send matrix stepping stays block-rate, disclosed | skipping on the modulated value froze the LFO at the silent phase and hard-killed voices (the deadlock); the remaining stepping is mild - smooth later only if audible | my call (bug fix) |
| 82 | Tone removed; MTTone = a retired reserved enum index (inert routes, never offered); old "fxTn" keys ignored (never in a release) | the Bell filter is the movable superset of the fixed tilt; reserving the index keeps this week's dev saves loading sanely | user order; retire-in-place = my call |
| 83 | SUB is added LAST in the slot chain (bypasses the slot filter/EQ/drive) and follows noteMul*pe3Mul from slotBaseHz/2; pitched engines only | the point of a sub is clean weight - filtering/driving it defeats it; slotBaseHz = the one base that's also correct in the piano roll's C4 world; Noise/Sample have no pitch (tooltip discloses) | my call inside user order |
| 84 | FORMANT = one knob sweeping A-E-I-O-U at fixed intensity (two Q-7 band-passes, wet eases in over the first ~12%) | one gesture = the musical use (vowel morph, LFO -> talks); a separate intensity would need a second control for marginal gain; the ease-in avoids an off->"A" jump at 1% | my call inside user order |
| 85 | New fixed mod targets append ABOVE the grid block (MTSub = MT_GRID_BASE+8, MTFormant; grid checks became range checks) | inserting before MT_GRID_BASE would shift every saved grid route (the user has live dev saves now); above-grid append keeps append-only honest forever | my call |
| 86 | Audio-rate modulation = SCOPED: only LFO -> Pitch and LFO -> Volume run per-sample; every other route stays block-rate + smoothed | those are the only routes whose targets can swing fast enough to make sidebands (FM/AM); a full per-sample matrix = an engine rewrite spent on inaudible routes (user approved this scoping after the full Vital discussion) | user-approved scope |
| 87 | KEY-mode LFO reuses lfoRate as the RATIO (lfoSync = -2); the rate reference is the PRE-FM pitch (pitchPreLfo) so pitch-FM cannot feed its own rate | one field, no new persistence; self-feeding rate = unbounded chaos, the pre-FM reference keeps classic FM behaviour | my call |
| 88 | Channel FX Character 0.5 = the old fixed effect constants EXACTLY; all migrations land at 0.5 | every existing sound keeps its voicing through the 2-slot redesign; the knob then widens BOTH ways (slower/softer left, faster/wilder right) | my call (migration-safety rule) |
| 89 | Channel FX slot combos START an effect at Amount 0.5 when picked with amount 0 | "picked Chorus, hear nothing" is worse than a disclosed default; the tooltip says so | my call, disclosed |
| 90 | Reverb send is HIGH-PASSED ~150 Hz (fixed); delay send is full-band | subs in a reverb = mud (the classic mixing move), and it preserves the old wet-top/dry-sub layering without per-slot sends; delay echoes on bass are a legitimate effect so the delay stays full-band | my call, disclosed in the send tooltip |
| 91 | Bus assignment (revBus/delBus) is ROUTING (channel-wide, all patterns, reset with routing); MASTER edits one bus at a time via a header A/B switch | which physical space a channel sits in = routing, not sound; duplicating the whole master row for B would not fit the column | my call |
| 92 | Old per-slot send migration takes the STRONGEST slot's send; the 4-fixed-channel-FX migration fills slots A/B strongest-first | same max rule as the chorus/comp migration (presence preserved); strongest-first keeps the old matrix Amount targets pointing at the audibly-dominant effect | my call, disclosed |
| 93 | Bell became BIPOLAR (gain field, diamond Y = gain on the dB axis, wheel = Q); NOTCH kept as a separate type | user's design for the bell; a true notch is an infinite-rejection zero (the phaser character) that a finite bell cut can't reproduce, and removing a persisted type would lossily migrate factory + user notches | user-approved (bell); keep-notch explained + accepted |
| 94 | New channel FX types = Tape / Auto-Pan / Widener (not gate/OTT/lo-fi/rotary) | picked for ZERO overlap: nothing else moves the stereo field (auto-pan), wobbles the transport (tape), or does M/S width on arbitrary content (widener); gating = Step Mod -> Volume already, lo-fi = the Drive dropdown, OTT overlaps Comp | my picks inside "if u have any ideas, do it" |
| 95 | Tape is 100% wet with ~3 ms constant latency while engaged | wow/flutter must warp the WHOLE signal (a dry blend combs like a chorus); 3 ms = the modulation headroom, disclosed in the tooltip | my call, disclosed |
| 96 | OTT band targets 0.22/0.16/0.10 + exponents 0.55 down / 0.45 up, +-14 dB caps, 0.9 output trim | tuned so Depth 100% is dramatic but never runaway (OTT gets LOUD); the caps are the anti-gunshot habit applied to gain | my call - re-voice on listening |
| 97 | FreqShift Character curve = 1500^|2c-1| - 1 Hz (centre exactly 0, ~3 Hz at 60%, ~1.5 kHz at the ends) | the musical zone (barber-pole 0.5..50 Hz) needs most of the throw; a linear map would waste it | my call |
| 98 | Rotary = ratio-locked rotor (0.34x the horn phase), no speed-ramp inertia | true Leslie inertia (accel/decel on speed switch) needs a slewed rate state; ratio-lock keeps it deterministic + cheap - add inertia only if the user asks | my call, disclosed |
| 73 | Base Freq is NOT a matrix target (modGridKnob returns none for Phys/Modal grid index 0) | user: pitch control already exists (MTPitch); a second per-slot pitch route would be redundant + confusing | user instruction |
| 99 | Slot Pan (the 6th FX knob) is NOT a matrix target and has no motion | division of labour: placement = static knobs (slot pan / step + note pan), stereo MOVEMENT = Channel FX Auto-Pan exclusively - avoids a 4th overlapping pan mechanism | user-approved split |
| 100 | Dropdown per-item tooltips = a custom TipList panel (TipCombo showPopup override), not PopupMenu custom items | juce::ComboBox/PopupMenu can't tooltip items; the content-child panel reuses the proven PickerCombo latch + Closer + focus-watchdog machinery (closes on outside click AND host-focus loss, per the user's spec) | user allowed "make a new dropdown" |
| 101 | MonoToggle rows = STEREO on top / MONO below, active row lit amber | reads as a state display + button in one; top = the default state | my call within the user's two-row-button spec |

> Older user-approved semantics (per-step Length = decay-rescale, slide-toward-next, one term
> per concept, no probability, master preset-wide, etc.) are DESIGN, recorded in CLAUDE.md /
> HISTORY.md — not repeated here.
