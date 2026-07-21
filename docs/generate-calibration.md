# GENERATE calibration constants - mined from the Groove MIDI Dataset

Derived from the Groove MIDI Dataset, Gillick et al., CC-BY 4.0
(https://magenta.tensorflow.org/datasets/groove, groove-v1.0.0-midionly).

**Method.** All 1,138 4/4 performances (323,234 mapped drum notes, 10 drummers) were
quantized to the 16th grid of each file's session tempo. Roles use the standard map
(36 kick; 38/40 snare; 42/44 closed hat; 46 open hat; 43/45/47/48/50 toms). Microtiming
is each hit's ms offset from its nearest 16th gridline, centered on the file's own median
offset (removes recording-alignment bias), so values are push/drag relative to the kit:
negative = early, positive = late. Groove stats (velocity, accent, ghost, micro, swing)
use beat-labelled files only; fill stats compare fill takes against them.

## Per-genre overview

Swing % = median placement of off-beat hat hits inside the 8th pair (50 = straight,
66.7 = triplet); computed only from files with enough off-beat hats, so straight-8th
genres (rock/pop) are biased toward their shuffled subset - n given. Ghost frac = share
of snare hits below 0.5x that file's snare p90 velocity; ghost ratio = their mean
velocity / p90. Fill mult = densest bar of a fill take / mean bar of groove takes
(notes per bar). Genres with few beat files (marked *) have thin groove stats.

| Genre | Files (beat) | Swing % (n) | Ghost frac | Ghost ratio | Fill mult | Fill share |
|---|---|---|---|---|---|---|
| rock | 334 (204) | 65 (77) | 0.19 | 0.30 | 1.14 | 0.39 |
| funk | 160 (53) | 54 (38) | 0.45 | 0.27 | 0.90 | 0.67 |
| jazz | 99 (48) | 77 (31) | 0.36 | 0.32 | 1.36 | 0.52 |
| hiphop | 95 (34) | 48 (18) | 0.30 | 0.22 | 1.50 | 0.64 |
| latin | 94 (48) | 80 (43) | 0.41 | 0.31 | 0.87 | 0.49 |
| afrobeat | 13 (13) | 80 (11) | 0.32 | 0.29 | - | 0.00 |
| afrocuban* | 60 (7) | 76 (7) | 0.60 | 0.27 | 0.68 | 0.88 |
| soul | 63 (28) | 59 (18) | 0.35 | 0.21 | 1.05 | 0.56 |
| neworleans | 53 (13) | 80 (11) | 0.55 | 0.28 | 0.87 | 0.75 |
| pop | 27 (15) | 80 (8) | 0.27 | 0.25 | 1.39 | 0.44 |
| punk* | 58 (7) | - | 0.17 | 0.20 | 1.00 | 0.88 |
| reggae* | 20 (4) | - | 0.56 | 0.24 | 0.85 | 0.80 |
| country* | 29 (2) | - | 0.63 | 0.26 | 0.76 | 0.93 |
| gospel* | 19 (1) | - | 0.30 | 0.29 | 0.75 | 0.95 |

## Velocity + microtiming per genre x role

Velocity = MIDI 0..127 mean +- sd. Micro = median ms vs the 16th grid (IQR in
parentheses); negative = plays early (push), positive = late (drag).

| Genre | Kick vel | Kick micro ms | Snare vel | Snare micro ms | CHat vel | CHat micro ms | OHat vel | OHat micro ms | Tom vel | Tom micro ms |
|---|---|---|---|---|---|---|---|---|---|---|
| rock | 71+-33 | -0.0 (31) | 77+-42 | +2.6 (33) | 67+-33 | -3.6 (34) | 32+-34 | -8.6 (41) | 82+-29 | +2.3 (28) |
| funk | 60+-30 | -1.2 (31) | 67+-44 | +3.3 (37) | 48+-23 | -2.6 (27) | 62+-28 | -3.4 (28) | 104+-27 | -6.9 (40) |
| jazz | 69+-31 | -0.6 (34) | 59+-35 | +1.4 (34) | 64+-25 | -4.9 (36) | 51+-16 | -6.0 (30) | 86+-33 | +2.9 (35) |
| hiphop | 63+-27 | -1.2 (31) | 82+-46 | +4.2 (38) | 51+-29 | -1.7 (27) | 57+-29 | +0.0 (28) | 102+-26 | -3.3 (43) |
| latin | 40+-26 | +5.2 (36) | 57+-36 | +0.0 (38) | 62+-24 | -4.3 (32) | 40+-23 | +4.2 (27) | 89+-26 | -3.9 (29) |
| afrobeat | 80+-33 | +1.1 (22) | 88+-42 | +5.6 (30) | 58+-23 | -7.9 (27) | 56+-31 | +3.6 (37) | 110+-24 | -1.5 (28) |
| afrocuban | 43+-27 | +4.0 (36) | 59+-36 | +1.0 (31) | 62+-22 | -3.7 (30) | 55+-28 | -11.4 (20) | 90+-25 | +1.1 (35) |
| soul | 45+-19 | -2.4 (28) | 71+-44 | +2.4 (32) | 64+-30 | +0.0 (23) | 68+-34 | -1.3 (39) | 79+-33 | -3.6 (26) |
| neworleans | 46+-27 | +1.2 (40) | 64+-39 | +2.4 (42) | 56+-27 | -6.0 (37) | 52+-33 | +6.2 (25) | 104+-26 | -5.5 (30) |
| pop | 52+-21 | +0.0 (27) | 86+-43 | +2.7 (28) | 55+-32 | -0.9 (26) | 42+-22 | -2.4 (18) | 91+-34 | -4.5 (30) |
| punk | 53+-20 | +0.0 (30) | 89+-47 | -2.9 (26) | 44+-13 | -1.0 (23) | - | - | 99+-23 | +4.9 (21) |
| reggae | 37+-17 | +1.1 (25) | 51+-40 | +0.5 (45) | 45+-23 | -1.6 (37) | 38+-16 | -1.9 (27) | 92+-30 | -5.9 (57) |
| country | 57+-16 | +0.0 (18) | 56+-34 | +3.3 (26) | 69+-24 | -15.3 (20) | - | - | 100+-27 | -8.7 (32) |
| gospel | 50+-10 | +1.0 (27) | 79+-33 | +7.3 (26) | 55+-21 | -14.6 (40) | - | - | 109+-19 | +10.4 (23) |

## Accent structure - mean velocity by 16th position (0..15)

All roles pooled per position (beat files). Read: backbeats (4, 12) and downbeat (0)
carry the accents; 'e'/'a' positions are the quiet inner 16ths.

| Genre | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rock | 74 | 44 | 65 | 58 | 92 | 52 | 69 | 56 | 78 | 60 | 70 | 66 | 93 | 63 | 72 | 59 |
| funk | 62 | 41 | 56 | 46 | 93 | 51 | 48 | 51 | 58 | 50 | 62 | 54 | 87 | 54 | 66 | 46 |
| jazz | 73 | 48 | 62 | 58 | 77 | 51 | 66 | 57 | 67 | 54 | 70 | 64 | 77 | 57 | 66 | 60 |
| hiphop | 76 | 44 | 61 | 47 | 104 | 48 | 53 | 44 | 72 | 47 | 66 | 52 | 97 | 58 | 70 | 49 |
| latin | 57 | 45 | 62 | 55 | 58 | 48 | 59 | 54 | 53 | 59 | 62 | 55 | 65 | 55 | 61 | 57 |
| afrobeat | 83 | 51 | 76 | 83 | 82 | 47 | 71 | 81 | 71 | 63 | 81 | 92 | 88 | 78 | 84 | 75 |
| afrocuban | 54 | 38 | 61 | 56 | 54 | 43 | 70 | 69 | 67 | 56 | 61 | 66 | 62 | 52 | 69 | 51 |
| soul | 59 | 31 | 51 | 48 | 90 | 37 | 58 | 46 | 61 | 40 | 52 | 53 | 89 | 54 | 61 | 42 |
| neworleans | 61 | 39 | 59 | 64 | 56 | 55 | 71 | 45 | 55 | 69 | 60 | 53 | 79 | 61 | 67 | 46 |
| pop | 63 | 40 | 41 | 64 | 89 | 49 | 40 | 50 | 82 | 40 | 41 | 57 | 90 | 68 | 53 | 56 |
| punk | 64 | 40 | 107 | 46 | 78 | 42 | 105 | 48 | 63 | 70 | 100 | 53 | 89 | 62 | 107 | 62 |
| reggae | 41 | 40 | 60 | 39 | 46 | 27 | 45 | 50 | 48 | 38 | 50 | 39 | 51 | 47 | 72 | 52 |
| country | 50 | 27 | 84 | 39 | 65 | 56 | 80 | 35 | 49 | 32 | 84 | 45 | 66 | 70 | 83 | 32 |
| gospel | 48 | - | 80 | 41 | 46 | 57 | 79 | 37 | 51 | - | 79 | 48 | 51 | 69 | 81 | 53 |

Snare-only accent rows for the four biggest genres (ghost-note geography):

| Genre | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rock | 57 | 44 | 59 | 56 | 107 | 54 | 69 | 52 | 80 | 56 | 70 | 68 | 107 | 63 | 75 | 57 |
| funk | 33 | 43 | 60 | 60 | 113 | 50 | 50 | 54 | 54 | 54 | 66 | 70 | 110 | 59 | 84 | 47 |
| jazz | 70 | 42 | 47 | 50 | 80 | 44 | 58 | 52 | 64 | 47 | 59 | 56 | 82 | 49 | 59 | 55 |
| hiphop | 67 | 49 | 71 | 46 | 120 | 54 | 49 | 47 | 75 | 52 | 81 | 53 | 116 | 66 | 99 | 56 |

## Suggested plugin-genre mapping

Rock->rock, Funk->funk, Jazz->jazz, HipHop->hiphop, Latin->latin (afrocuban as a
denser sibling), Afrobeat->afrobeat, Pop->pop (soul for a softer variant),
Reggae->reggae, Punk->punk (rock played harder/straighter, near-zero ghosts).

