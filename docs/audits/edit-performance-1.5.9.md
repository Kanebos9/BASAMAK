# BASAMAK 1.5.9 edit and timing audit

2026-09-15, Apple Silicon macOS. Source baseline: `1b0778a` (1.5.8).

## Cause and fix

The Live Drumming update (`74aad33`) put the full undo serializer, whole-project
change scan and merged sound synchronization under the audio callback lock.
A sound edit could serialize 64 patterns x 16 channels while blocking playback.
JUCE's VST3 and standalone wrappers acquire that lock before calling processBlock.

Undo now caches immutable parameter snapshots and shares unchanged channels and
takes. Decimal formatting, ValueTrees and hashing run after parameter capture
releases the audio lock. Timer captures release/yield between patterns and defer
if MIDI/host changes invalidate the scan. Explicit captures remain atomic.
Merged sound updates release the lock between destination bars. Ordinary slot
parameter edits also bypass mix serialization, sample reloads and waveform bakes
when their dependencies are unchanged. This preserves active sample voices in
other merged bars; channel/asset/engine/table changes retain the existing load path.

Snapshot trees are copied at export/restore boundaries to respect JUCE ValueTree's
single-parent ownership and keep old undo entries isolated. Slot capture excludes
voices, sample buffers, effect delay lines and baked tables. The live and saved
channel structures use the same serialization template, so a newly persisted
channel property missing from the snapshot fails compilation.

The audit also fixed an immediate-Undo edge case (an observed edit could be
skipped before the debounce committed it), and missing Step Nudge persistence.
Legacy projects without Nudge values load zero.

## Measured editing workload

`LiveDrumTest --profile-edits [live] [merged] [dense]` sends 12 frequency edits
through the actual Sound Editor fader callback, with editor timer polling between
edits. An independent JUCE real-time audio thread runs 48 kHz / 128-frame blocks
(2.667 ms deadline), MIDI hits and the same callback locking as the JUCE wrapper.
The unchanged old shared library was saved before rebuilding and linked with an
independent copy of this same workload for the baseline measurement. Real-time
scheduling was enabled in all reported runs. No builds ran during the measurements tabulated below.

| Workload | Worst UI timer (ms) | Worst audio callback (ms) | Worst audio lock wait (ms) | Callbacks over 2.667 ms |
|---|---:|---:|---:|---:|
| 1.5.8 regular, one bar | 414.841 | 413.726 | 413.302 | 12 |
| 1.5.8 live, eight merged bars | 414.535 | 414.333 | 411.871 | 27 |
| 1.5.9 regular, one bar | 2.853 | 2.412 | 0.199 | 0 / 791 |
| 1.5.9 live, one bar | 7.106 | 1.417 | 0.191 | 0 / 796 |
| 1.5.9 regular, eight merged bars | 11.096 | 4.558 | 0.151 | 1 / 827 |
| 1.5.9 live, eight merged bars | 11.182 | 3.525 | 0.190 | 1 / 813 |
| 1.5.9 live, 200 saved takes | 6.645 | 1.566 | 0.808 | 0 / 812 |
| 1.5.9 multisample regular, one bar | 4.970 | 1.755 | 0.307 | 0 / 790 |
| 1.5.9 multisample regular, eight merged bars | 17.023 | 6.019 | 1.526 | 1 / 836 |
| 1.5.9 multisample live, eight merged bars | 21.294 | 1.389 | 0.190 | 0 / 820 |

All eight final profiles passed the 50 ms UI-stall regression limit, with no
unexpected silent blocks. The table includes every final low-latency deadline
overrun; these are measurements, not a guarantee against all host scheduling or
DSP load. Full uncached serialization still costs roughly half a second or more;
initial editor capture, explicit full saves/restores and asset loading remain
separate operations from ordinary edits.

Intermediate verification also recorded a 55.672 ms single-bar UI outlier and a
62.217 ms merged-multisample timer with 16.918 ms process CPU time. Investigating
merged propagation found needless sample reloads/voice resets and led to the
parameter-copy optimization described above. A diagnostic pass during final
linking recorded an 85.667 ms UI outlier and one 39.504 ms audio callback, despite
its audio lock wait staying below 0.5 ms. That pass is excluded from the table;
the complete final eight-case measurement was run after compilation/linking
stopped. The harness now logs slow timer CPU time and actual control callback
time to distinguish compute work from waiting/descheduling.

## Functional coverage

- Snapshot cache identity, one-channel invalidation, all 1024 channels on initial
  capture, off-screen pattern/channel edits, slots/curves, master bus B, note/take
  data, immutable prior entries, returned-tree isolation and complete restoration.
- Padding-only changes in note structures cannot cause take/cache churn.
- Concurrent MIDI global edits cannot produce snapshots with mixed old/new bars.
- Actual editor Undo/Redo, an edit seen before debounce, branching after Undo,
  and one undo restoring all eight merged bars. Merged slot edits preserve active
  multisample voices/buffers; channel changes and granular engine/wave changes
  still propagate with their required table rebuilds.
- Existing regular/live MIDI routing, velocity, timing, recording, takes,
  conversion, export, Duck/Overlap/Choke, engines and factory preset regressions.
- New 17/18/19/25/26/33/34/35 step counts: actual event timing over six bars at
  48 kHz/128 frames/120 BPM/4-4; 44.1 kHz/257 frames/137 BPM/7-8 with maximum
  Swing, Nudge, ratchets and gates; 96 kHz/1024 frames/83 BPM/3-4 DAW sync with
  differently sized merged bars. Exact event count and step/sub-hit/pattern
  identity are checked, with a two-sample timing tolerance and one-sample gate
  tolerance. Menu selection, merged 64-cell guards, state reload and 9600 PPQ
  MIDI export are exercised through the processor/editor. Step menu descriptions
  also check seconds per step for standalone BPM/meter changes, host 7/8 meter
  and per-bar merged options; compact closed captions and existing cap guards stay intact.

## Multisample base frequency

Each multisample slot now exposes Base Freq below Amp/Cab, reusing the existing
Hz/Note readout, semitone snapping, Shift free tuning and C4 double-click reset.
The C4 default retains legacy single-note steps. Steps, TEST and Live Drumming
use the base and choose the matching recorded zone; KEYS and normal rolls remain
absolute. Scale steps now use the visible base instead of the hidden oscillator
frequency. Original instrument files are not modified by this setting.

VoiceMapTest measures rendered fundamentals and distinct zone timbres at C3,
C4, D3, a C3 major voicing and fractional tuning. LiveDrumTest checks both slot
controls, independent bases, actual learned MIDI CC/readout refresh, step
recording, export, Undo, copy and state persistence. Missing old-file values load
C4. The UI snapshot was visually checked for fit in both slots.

## Pattern handover and Overlap Off

A read-only replay of the user's saved hayalet slot/step settings reproduced two
bass voices at the P1 → P2 switch. Both patterns' actual Overlap flags were zero
before playback, at the switch and 100 ms afterward, while one outgoing and one
incoming voice remained active. The setting was not turning itself on: regular
step retriggers had applied Off only within the current pattern instance.

New step hits also fade the same channel's old step voices in rendered previous
patterns when the incoming channel has Overlap Off. Tails rendered later in the
block receive a fade scheduled for the actual hit sample; a later request cannot
postpone an earlier fade. Normal piano-roll voices retain note-length/Poly rules.
No changes were made to the user's preset. Replaying the original full processor
fixture after the fix leaves the outgoing bass inactive; its second 100 ms
window has RMS 0 (previously 0.258), after the intended short handover fade.

DuckTest covers 16 handovers: Off/On, a gap, muted or solo-excluded target, source
or destination in Piano Roll, merged bars and following the playing pattern.
A separate audio comparison checks exact samples before the scheduled fade and
complete cutoff afterward. Existing Live Duck/Overlap/Choke cases also run.

## Menu placement

The Live Drumming note menu now uses
`withTargetComponent(this).withMousePosition()`: the component supplies scaling,
and the cursor supplies the position, with JUCE screen-edge fitting. Delete,
Velocity, Pan, keyboard deletion and recording guards keep their existing paths.
Actual popup placement in the user's running REAPER session needs their visual
check; the session was never closed or restarted by the update.

## Final build validation

All 22 native regression programs passed with the controls, step choices,
duration labels and handover fix:
PolyTest, PRollTest, MergeTest, PresetTest, DuckTest, NudgeTest, SlotNoteTest,
GlideTest, ArpTest, TunerTest, GateWrapTest, LiveRollTest, RollAbsoluteTest,
WavetableTest, AliveTest, GrainTest, ModMatrixTest, NamTest, GenTest, VoiceMapTest,
PatternCountTest and LiveDrumTest (`--ui`). New count timing cases checked
1242 / 1338 / 1125 events; maximum error was 1.000 / 0.998 / 1.000 samples.

An independent serializer fixture populated all 64 x 16 channels plus slot and
curve settings, step/roll notes, regular and kit takes, maps, master bus B and UI
state. Old 1.5.8 and final 1.5.9 shared libraries produced byte-identical
14,967,731-byte saved files for the existing schema. Nonzero Step Nudge adds its
new property and is covered separately by restore/undo tests.

The universal shared library passed LiveDrumTest (`--ui`), PatternCountTest
and VoiceMapTest on arm64, plus those same programs and DuckTest under x86_64
Rosetta. After the final merged-slot optimization, LiveDrumTest (`--ui`) was
rebuilt and passed again on native arm64 and universal x86_64/Rosetta, including
active multisample voice preservation and engine/table fallback checks.
Release VST3, AU and Standalone bundles all report 1.5.9, contain arm64
and x86_64 binaries, and pass strict code-signature verification.

Signed release bundles were installed under their normal BASAMAK names in the
user VST3/AU folders and /Applications. Each destination contains exactly one
BASAMAK bundle. Previous installs are preserved in
`backups/20260915-1.5.9-edit-stutter-installed`. The existing REAPER session was
left running. Apple `auval -v aumu Bsmk OZ95` passed.

## Windows compiler follow-up

The Windows build for `e854020` (MSVC 19.51.36256.0, downloaded run
94796825618) reported three C2668 errors. Both `copySavedField` templates
matched fixed arrays: float[5] in saved channel capture and int[8] in pattern
chain capture. The fix uses one C++17 template: trivially copyable values,
including arrays, use the same byte copy; non-trivial arrays recurse by extent;
other objects use assignment. Call sites and the saved-state format are unchanged.

An extracted before/after C++17 fixture checks scalar, float/int/bool arrays,
nested arrays, padded structs and non-trivial string arrays. All copies match.
The actual processor and LiveDrumTest were rebuilt locally, and LiveDrumTest
(`--ui`, including snapshot/undo regressions) passed with zero failures. Windows MSVC is not
available on this Mac; confirmation on that compiler requires a fresh CI run
from the fix commit. Version remains 1.5.9.
