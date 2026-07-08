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
| 1 | Steps->roll IMPORT runs only when the roll is EMPTY; a roll with notes is left untouched and the Base Freq knob is re-pinned (knob experiments in step mode don't carry back) | after a quantize round trip both sides hold the same melody; importing again would double every note | EXPLAINED (2026-07-09; option B — absorbing the knob offset into notes+Tune on return — offered, no verdict yet) |
| 2 | Quantizing roll->steps KEEPS the roll notes underneath (only the chord-overlap path deletes them) | so switching back shows your roll again instead of a lossy re-import | OPEN (this is what makes #1 bite) |
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
| 12 | The upstroke accent (x0.82) is baked into recorded VELOCITY (plus the per-note Strum UP flag since 2026-07-09) | live == recording for the audible accent | EXPLAINED |
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

## Engine / infrastructure (behaviour-affecting internals)

| # | Decision | Why | Status |
|---|---|---|---|
| 23 | Loop-wrap fix internals: sub-1/1000-sample remainders snap to 0; bar-start check tolerates a quarter sample | the gate-explosion bugfix needed a tolerance; values chosen conservatively | EXPLAINED (bugfix session) |
| 24 | Take names use the wall clock (same-second duplicates possible) | known gap, never prioritised | OPEN |
| 25 | Old projects with removed legacy engines (SrcSynth/SrcWave) load SILENT on those slots instead of erroring | graceful compat break | OPEN |

> Older user-approved semantics (per-step Length = decay-rescale, slide-toward-next, one term
> per concept, no probability, master preset-wide, etc.) are DESIGN, recorded in CLAUDE.md /
> HISTORY.md — not repeated here.
