# BASAMAK style files (.basamakstyle)

A GENERATE **style** is a plain text file describing one genre's DNA: the drum-kit canon, the
mined feel constants, and the melodic vocabulary (bass cells, melody cells, chord-stab
templates, a progression pool). The 8 factory styles are written in exactly this format and
compiled into the plugin - what you can write is what the factory ships.

**Where**: put files in `Documents/BASAMAK/Styles/`, named `Anything.basamakstyle`. They appear
in the Generate panel's Style picker (tagged "(yours)"); the picker's **Refresh styles** entry
rescans the folder. A file whose **name matches a factory style replaces it** (Refresh restores
the factory version if you delete the file). A file that fails to parse is **skipped** and the
reason is shown in the panel's readout line - it can never crash or half-load.

**Determinism stays**: a style only changes WHAT the seeds pick from. The same seeds + the same
style always generate the identical result.

## Format

One statement per line. `#` starts a comment. Unknown keys are ignored (so files stay forward
compatible); malformed values on known keys fail the parse. **Cells are 1-based 16ths of a 4/4
bar**: 1 = the downbeat, 5 and 13 = the backbeats, 3/7/11/15 = the 8th offbeats.

```
# my house variant
style "My House"        # required, first statement (quotes optional for one word)
swing 54                # 50 = straight .. 75 = full triplet swing

# ---- drum kit DNA ----
kick.canon 1 5 9 13     # immutable kick cells (the genre's identity - never mutated)
kick.opt 7 15           # optional cells the seed may add
kick.optn 1             # how many optional cells land (0..2)
snare.cells 5 13        # the backbeat scheme
hat.tier offbeat        # offbeat | 8ths | 16ths
hat.rolls 8 16?         # cells that ratchet (trap rolls); '?' = a 50% chance
openhat all             # all = every 8th offbeat | sparse = 1-2 picked cells
ghosts 2                # ghost snares per bar (0..3, at Medium density)
ghost.ratio 0.25        # ghost velocity as a fraction of the backbeat
fill auto               # auto | crescendo | double | hatroll

# ---- feel constants (velocities in MIDI 0..127; microtiming in ms, negative = early) ----
vel.kick 112 30         # mean + spread (the spread feeds Humanize's jitter)
vel.snare 107 8
vel.hat 64 25
accent 63 40 41 64 89 49 40 50 82 40 41 57 90 68 53 56   # per-16th accent means (melodies too)
micro.kick 0            # NOTE: the kit writer always anchors the KICK to the grid
micro.snare 2.7         # snare drags late...
micro.hat -0.9          # ...hats push early (written as per-step Nudge, gated by Humanize)
micro.ohat -2.4
micro.perc 0
micro.bass 4            # the bassline's laid-back drag on kick-coincident steps

# ---- melodic DNA (weighted cells; a bigger W = picked more often) ----
# bass W : pos dur deg vel | pos dur deg vel | ...
#   deg: R root | 3 third | 5 fifth | 7 seventh | O octave up | L octave below |
#        D fifth below | A approach (slides into the next chord's root)
bass 3 : 3 1 O 98 | 7 1 O 94 | 11 1 O 98 | 15 1 A 92
bass 1 : 1 4 R 104 | 11 3 R 98 | 15 2 5 90

# mel W : <pos><cat> ... ; slope lo hi
#   cat: C chord tone | L color/scale tone | A approach into the next note | R rest
#   slope = the cell's overall pitch drift range in semitones (picks the contour)
mel 2 : 1C 4L 7C 11L ; slope 0 3

# comp W : pos pos ...       (the chord-stab rhythm template)
comp 3 : 3 7 11 15

# prog W : 1 5 6 4           (scale degrees per bar; the pool New idea rerolls from)
prog 3 : 1 6 4 5
prog 1 : 2 5 1 4

# ---- phrasing constants ----
hook 2                  # the hook repeats every N bars
mel.density 1.0         # topline onset-budget multiplier (trap ~0.55, reggaeton ~1.2)
```

## How the engine uses it

- **Drum Kit role**: the drum lines above ARE the pattern (canon + seeded mutations, backbeat
  law, hat language + a seeded ornament layer, ghosts, rolls, fills, swing, microtiming).
- **From scratch** (no drums heard): the bass cells / melody cells / comp templates are placed
  literally (degrees resolved against the chord timeline, categories retuning the motif), and
  the progression pool drives the chords. New idea rerolls the cell picks, the progression,
  the harmonic rhythm and the form - every roll is a different piece.
- **With a real groove**: your context always wins - the style only *biases* where onsets
  prefer to land; the pocket/snare/kick disciplines still move anything that fights your drums.
- A style needs none of these sections: whatever is missing falls back to the engine's generic
  rules. Start by copying a factory style's text from `Source/dsp/GenStyle.h` and editing.
