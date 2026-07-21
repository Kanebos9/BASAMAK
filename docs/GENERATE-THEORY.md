# GENERATE v2 — THEORY-DERIVED DESIGN (2026-07-22 research round)
Three research tracks (melody/vocal craft, groove interlock + bass, harmony/comping/sound-aware),
each principle as: RULE (parameterized) / VERIFY / INPUT. Full sources in the session notes.

## DATA REQUIREMENTS (the new gathering spec - what Ctx must carry)
1. Per-drum-channel ROLE classification: kick / snare / hat / other (from bank category + name;
   fallback: register+decay heuristic). Bass listens to KICK; melody phrases around SNARE.
2. Per-hit VELOCITY (accent map) - already hitStr, keep.
3. STEP-channel harmony: real pitches = Freq-knob base + step pitch offsets (stop discarding).
4. Existing-part register map: per roll channel min/median/max pitch (arrangement lanes).
5. Melody-occupancy mask (columns where the paired melody sounds) for comp/counter roles.
6. Selected sound introspection: ADSR (atk/dec/sus/rel), keysPolyMode/legato, category,
   msSet zone-root span, scaleOn (harmonizer active = generate single notes).
7. Shared chordCols[] timeline (harmonic rhythm) driving ALL roles.
8. Tempo (ms-accurate microtiming caps), meter weights.
9. CONTEXT READOUT in the panel: "groove: N hits (K kick / S snare / H hat), key from M channels"
   - starvation must never be silent.

## MELODY / VOCAL (track 1)
M1 Proximity: >=70% intervals <=2 st, <=10% >5 st (resample oversized).
M2 Leap recovery: leap >5 st -> next = opposite-direction step (mandatory); 3-5 st skip -> reverse p=.7.
M3 Same-direction double leap only if all 3 notes are current-chord tones.
M4 Tessitura pull: outside middle 60% of register -> bias next toward median; never 2 edge notes.
M5 Range cap: 12 st Singable (14 else); hook core within 7 st.
M6 Contour: per-phrase arch (peak at 60-70%) or descent template, score candidates against it;
   final phrase ends stepwise DOWN onto cadence tone.
M7 Chord tones on strong beats (p=.85-.95 by Color); NCTs stepwise both sides, <=1 cell long.
M8 Appoggiatura (licensed violation): p=Color*.15, strong beat, leapt-into, resolves down; <=1/phrase.
M9 Tendency tones: 7^->1^, 4^->3^ at phrase ends (unless intentional open end); no augmented intervals.
M10 MOTIF ECONOMY: one 3-5 note cell; >=60% of onsets = transforms (repeat/sequence/inversion/
    extension); vary <=30% of a repeat, never its first two notes.
M11 Question/answer: paired phrases share opening ~50%; Q ends on 2^/5^/7^, A on 1^/3^.
M12 Prosody: longest+highest note per phrase on strength >= beat (or real drum hit); section-wide
    front-heavy (start col 0) vs back-heavy (start +1..+3 cols) choice.
M13 Push: p=.2-.35 (Driving), shift strong-beat note 1 cell early ONLY if next-chord tone; never 2 in a row.
M14 Breath: no run >2 bars; >=1 eighth rest between phrases; <=~12 onsets/bar-pair.

## GROOVE INTERLOCK + BASS (track 2)
G1 Lock the One: bass onsets col 0 when a kick is there (mandatory unless Free).
G2 kickMode {unison, interlock, avoid}: unison >=70% onsets within 1/32 of kicks; interlock 0 onsets
   within 1/16 of kicks + gap-midpoint hits; avoid = note ends >=1/32 before next kick. THE genre lever.
G3 LHL syncopation band: score each bar (16-col weights 0/-1/-2/-3/-4); target Melody/Riff [2,6],
   Bass [1,4], Hum [0,2]; de-syncopate offenders by shifting to next strong col. (Witek inverted-U.)
G4 Anticipation at changes: pushProb (Driving .5 / Flowing .15) move change-note to col -2/-4 with
   NEW chord's tone; may leave col 0 empty.
G5 Pocket microtiming: bass Nudge +0.02..+0.08 step (never ahead of kick), +5% vel on laid-back notes.
G6 Density complementarity: melody onset budget = clamp(K - a*drumDensity); >=60% of Flowing/Hum
   onsets in drum-gap columns.
G7 Accent hierarchy: strength(col) = metricWeight + 0.7*loudestHitVel(col); chord tones/phrase starts
   to top-strength cols.
G8 Snare punctuation: melody phrases end on/1-before snare; no melody onset in post-snare 1/16 shadow;
   bass rests or stays short at snare cols.
G9 Bass grammar: change-downbeat = ROOT (mandatory); vocabulary {root .5, 5th .25, oct .15, 3rd .1};
   last onset before change = approach note (chromatic if Spicy, scale-step if Safe).
G10 Sustain vs stab: bass len ~ interKickGap*0.8; hats >=8/bar -> >=50% staccato; sparse drums -> sustains to next change.

## HARMONY / COMPING / SOUND-AWARE (track 3)
H1 Low-interval limits: reject interval <=4 st below C3 and <=2 st below G3; below C3 only root/5th/oct.
H2 Voice leading: minimal total motion, common tones locked, others <=2 st preferred (cap 5), tie-break
   to register center. The biggest "professional" jump for chord parts.
H3 Comp rhythm templates per stance: Flowing = 1 hit/chord; Pockets = Charleston/tresillo; Driving =
   offbeat 8ths; never >60% offbeat without onbeat anchor.
H4 Harmonic rhythm: shared chordCols[] (Density: 1/bar sparse, 2/bar dense, turnaround on line ends);
   comp re-voices at every change; changes only on strong cols.
H5 Register lanes: new part goes in widest free >=1-octave gap; comp top >=3 st below melody median;
   comp bottom >=7 st above bass median; <20% range overlap.
H6 Comp fills melody gaps: onset prob x0.3 under melody, x2.0 in gaps >= quarter.
H7 Counter-lines: contrary/oblique >=60%; freeze during melody runs; no strong-beat unisons/octaves.
H8 Envelope-aware: atk>=120ms -> min len half-note, sparse, allow -1 cell early start; pluck (sus~0,
   dec<300ms) -> 1-cell notes; rel>800ms -> no adjacent different-pitch repeats.
H9 Mono/poly: mono or Bass/Leads = single line (comp role degrades to arpeggio); poly pads = 3-4 note
   voicings, keys 2-3; scaleOn slot = generate single notes, harmonizer voices them.
H10 Register sweet spots per category; multisample notes <=5 st from nearest zone (varispeed cap).

## UNIFIED TOP 10 (implementation priority)
1. G2 kickMode + kick/snare/hat classification (the enabling input)
2. M7 chord-tones-on-strong-beats + stepwise NCTs
3. M10 motif economy
4. H4 shared chordCols harmonic-rhythm timeline
5. G3 LHL syncopation band targeting
6. M1+M2 proximity + leap recovery
7. G9 bass root-on-change + approach notes
8. H2 minimal-motion voice leading (comp)
9. H8/H9 sound-aware length/density/poly
10. M11+G8 question-answer ends + snare-aware phrase breathing

## PHASES
P0 (plumbing, no theory): gather test coverage, context readout, step-pitch harmony via Freq knob,
   pocket length-clipping + exclusion zones. + the swing merged-group fix rides along.
P1: data model (roles, registers, chordCols, sound introspection) + G2/M7/M1/M2/G9.
P2: motif engine (M10, M11) + LHL scorer (G3) + prosody/breath (M12/M14/G8).
P3: comp/counter roles (H2-H7) + sound-aware (H8-H10) + microtiming (G5).
Each phase gated by GenTest additions + the user's ear.

# ============ V2 ADDENDUM (competitive surfaces + drums + from-scratch, 2026-07-22) ============

## COMPETITIVE PARAMETER SURFACE (Logic Session Players, EZbass/EZD3, Scaler, Captain, Orb, Live 12, BiaB)
Recurring standard: style preset | Complexity(=our Density) | INTENSITY (dynamics - we lack) |
Humanize (vel+timing - we lack) | Swing/Feel | FILLS (we lack) | register | follow-drums |
reroll-with-locks | editable output. Praised uniques to steal: Logic FEEL push/pull (-> our Nudge),
Live 12 SPLIT ratchet-probability (-> our stepRoll), Toontrack TAP2FIND (tap a rhythm as the seed ->
our keys capture), Captain TENSION (-> our Color), EZD3 POWER HAND. Market's loudest gap: no tool
AUGMENTS THE USER'S OWN NOTES (Logic overwrites) -> our "Keep my notes" mode (Vary on hand-written
material, rhythm-only or pitch-only) = a differentiator built on existing Vary machinery.
PANEL v1-ESSENTIAL ADDS: Intensity (Soft/Med/Hard) | Humanize (Off/Subtle/Loose) | Fills
(Off/Last bar/Every phrase) | context readout line | "Keep my notes" action.
LATER: Feel push/pull | Ratchet chance | Strum/Glide amount (Riff) | Tap-a-rhythm seed |
Accent-every-N | named style presets (bundles - only once dials stabilize).

## DRUM GENERATION (STEP-mode, multi-channel - NOT the roll; NOT via channel-merge)
D1 Genre kick canon (mutate <=2 steps): House/Techno 1,5,9,13 (immutable) | Boom-bap 1+{7|8|11}(+15)
   | Trap 1+sparse{4,7,8,11,12,15}, never 9 | DnB 1,11 | Reggaeton 4-floor + tresillo 4,7,12,15.
D2 Backbeat law: snare 5,13 vel 100-115 (trap: 9 only; dembow: tresillo); one displaced backbeat
   max per 2 bars (breakbeat).
D3 Hat language: tier 8ths/16ths; velocity template per beat 105/60/85/60 (16ths), 100/70 (8ths);
   house = offbeat-only hats. Never two consecutive equal velocities.
D4 Open hat: 8th offbeats only, choked by next closed (choke group!); never downbeat.
D5 Trap rolls: 1-2/bar, half-beat before snare/bar-end, ratchet {2,3,4,6}, vel ramp 60->110 (= stepRoll!).
D6 Ghost notes: 25-40% of backbeat vel, on 16ths flanking backbeats {4,7,8,12,16}, 1-3/bar (genre budget).
D7 Percussion layers add clap->shaker->rim->tom; clap doubles snare (small negative Nudge); a new
   layer's onsets must not duplicate an existing channel's accent grid.
D8 Fill grammar: last bar of each 4/8-bar phrase; start col 13/9/1; subdivision doubling OR tom run
   (hi->mid->low) OR snare crescendo 50->120; suppress hats during full fills; crash+kick on next downbeat.
D9 Swing per genre: techno 50 / house 52-56 / boom-bap 58-62 / garage-funk <=66%; quarters unswung;
   +-3-8 vel jitter.
D10 FROM-SCRATCH = style-DNA template (never a blank grid): BPM range + swing + kick grid + hat tier +
   snare scheme per genre (House 120-128/52-56%/4-floor/8th-offbeat/clap 5,13; Techno 125-135/50%;
   Boom-bap 85-95/58-62%; Trap 130-160 half-time; DnB 170-176; Reggaeton 90-100/dembow). Instantiate
   verbatim, one mutation per regenerate.
DRUM UI: generation writes STEPS (velocity, ratchets via stepRoll, Nudge, choke groups) across one or
several drum channels; entry point needed outside the roll header. Channel-merge is NOT the vehicle.

## AGREED SCOPE DECISIONS (user, 2026-07-22)
- From-scratch generation: yes (style DNA when context empty; readout says so).
- Drum roles: yes, step-native (single channel first, kit-level later).
- Sound-editing toggle: OPT-IN, performance-feel params ONLY (env times, glide, strum, poly/mono,
  maybe cutoff) - never engine/wave/layers; undoable; default off.
- "Keep my notes" augmentation mode: build (market gap).
- BASAMAK idiom: generated parts must use ratchets/nudge/strum/glide/humanize where idiomatic.

# ============ V3 ADDENDUM (open-source architecture survey + user decisions, 2026-07-22) ============
## ARCHITECTURE (from Impro-Visor, GPL - idea only, no code):
TWO-STAGE PIPELINE: (1) rhythm skeleton -> ABSTRACT MELODY string of note CATEGORIES + durations
(C=chord tone, L=color, A=approach, R=rest) + contour SLOPES ("ascend 2-4 st across these cells");
(2) resolver turns categories into concrete pitches against the chord-of-the-moment (our ladder walk
IS the resolver half). Styles become KEY-INDEPENDENT DATA, not code. Flat weighted cell lists, no CFG.
## STYLE-AS-DATA FORMAT (from MMA, GPL - format shape only): text/constexpr tables:
style "funk" swing/vel-stats/push-per-role; rhythm cells (pos strength); melody cells (category
strings + slopes); bass tuples (pos dur degree vel); fill specs. Weighted, seeded-RNG picks =
deterministic. Named grooves with variation tiers (intensity families).
## CALIBRATION: Groove MIDI Dataset (CC-BY 4.0 = shippable constants + credit): one offline mining
session -> per-genre/role velocity mean+sd, accent ratios, microtiming push/drag ms, swing ratios,
ghost floors, fill stats. Ship rules + mined constants; NEVER embed GPL/unclear content (Hydrogen/
MMA data = read for structure only). Stochas/CP-solvers = skip (playback dice / wrong weight class).
## USER DECISIONS (2026-07-22 round 2):
- Panel may be BIG; NO ratchet-probability dial (drum grammar may still write Roll sub-hits where
  genre-canonical, e.g. trap hat rolls - content, not a knob).
- ENTRY POINT: a Generate button LEFT OF "Clear" in the pattern row = the universal door (works in
  step + roll modes); the roll-header button stays, same panel.
- DRUM KIT ROLE = MULTI-CHANNEL: writes kick+snare+hats+perc as one interlocking groove across
  several channels (auto-detect drum channels by category + confirm list in panel; ONE undo).
- SOUND-EDIT TOGGLE: competence-gated authority - v1 minimal (env lengths only) or none; expands
  only after passing the user's ear. "If it's not competent, it edits nothing."
- Aug mode "Keep my notes" confirmed.
