#pragma once
// ============================================================================
// PartGen v2 [2026-07-20] - the GENERATE feature's melody engine (v1.5.4).
//
// FOR NEW READERS: a pure, DETERMINISTIC, rule-based part generator. Given a
// musical context (bars, key/scale, the groove's EXACT drum-hit positions,
// optional per-beat harmony weights) and the user's options, it emits a
// MONOPHONIC line of piano-roll notes in CONCAT columns (DRAW_RES per bar).
//
// v2 (user: "make it musical - real vocal melodies spread through multiple
// bars, they are not too short to repeat every bar"):
//  - PHRASE FORMS instead of copy+mutate: Hook (A a B a - a repeated hook with
//    a contrasting bridge), Call & answer (question phrases answered by phrases
//    that share the opening and resolve), Free (through-composed). Phrases are
//    up to 2 bars; legato notes may cross bar lines = arcs, not bar loops.
//  - CADENCES: phrase endings are OPEN (2nd/5th - "the sentence continues") or
//    RESOLVED (tonic/3rd - "the sentence ends"); the last phrase always resolves.
//  - ECHOES vary RHYTHM too (shift/drop a late onset), never the opening - the
//    opening is the hook's identity; a bit-identical rhythm read as copy-paste.
//  - HIT-EXACT rhythm: Driving locks onsets onto the drums' REAL positions
//    (7/11-step channels land exactly - 384 divides every count); Pockets aims
//    at the real gaps' midpoints; Flowing floats on the METER's beats. The 16th
//    grid survives only as the no-drums fallback.
//  - PER-BEAT harmony: strong-beat chord tones follow the beat's chord, not the
//    bar's average.
//  - Phrase-shaped DYNAMICS: velocity swells toward the phrase peak.
//
// v3 [same day]: the FORM enum surface became LINES + RELATION (user: forms were jargon; he
// thinks in "one/two lines of my lyrics"). lines = how many sentences (0 = Auto, 1 = one arc
// across the WHOLE group - any bar count incl. 3), relation = what later lines do (repeat /
// answer / new / Auto). AABA etc. now EMERGE from Auto instead of being a menu.
//
// v4/r21 [2026-07-22]: THE CONTEXT LATTICE. The r20 rhythm-shaping rules (LHL
// weights, de-syncopation targets, pushes, breaths, prosody strong-cols, motif
// spans) were hardcoded to a 16-col 4/4 bar - a 7-step-kick context's bass came
// out smeared across 16th positions and the step-count authority then honestly
// picked 16. Now Ctx carries latticeN (the gather's LCM of the contributing
// step grids) and EVERY rule moves in lattice cells; strength(cell) = the
// divisor-depth metric weight + 0.7 * the loudest hit at the cell (G7), and
// the LHL weights derive from that map. Onsets never leave the lattice (only
// Humanize jitters off it, deliberately, within the step writer's tolerance).
//
// Standing rules: ONE-SHOT dice on the message thread (playback deterministic);
// TWO seeds - rhythmSeed places every onset, pitchSeed picks every pitch, so
// "Same rhythm, new notes" is exact by construction; varyCount mutates on top;
// the scale is passed in (no third scale-table copy); no editor deps
// (headless-tested by tests/GenTest.cpp).
// ============================================================================

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

#include "GenStyle.h"   // [2026-07-22 r22] styles are DATA - melodic/bass/comp cells + constants

namespace PartGen
{
static constexpr int COLS   = 384;   // columns per bar (== DrumChannel::DRAW_RES)
static constexpr int CELL16 = COLS / 16;   // one 16th
static constexpr int BEAT   = COLS / 4;    // one beat
static constexpr int MAX_BARS = 8;

enum Role     { RoleBass = 0, RoleMelody, RoleHum, RoleRiff, RoleChords };   // [r20] Chords = comping
enum Rhythm   { RhFlowing = 0, RhPockets, RhDriving };
enum Contour  { CtAuto = 0, CtArch, CtRising, CtFalling, CtWave };   // internal (Auto-rolled; no UI row)
// [2026-07-20 v3, user design] structure = LINES, not form jargon: how many melodic sentences
// the group holds + what the later lines do. "One line of my lyrics across 3 bars" = lines 1.
enum Relation { RelAuto = 0, RelRepeat, RelAnswer, RelNew };

struct Options
{
    int  role       = RoleMelody;
    int  key        = 0;          // tonic pitch class 0 = C .. 11 = B
    const int8_t* scale = nullptr;
    int  scaleLen   = 7;
    int  density    = 1;          // 0 sparse | 1 medium | 2 busy
    int  registerBand = 1;        // 0 low | 1 mid | 2 high (Bassline caps at mid-low)
    int  color      = 0;          // 0 safe | 1 spicy | 2 colorful
    int  rhythm     = RhPockets;
    int  contour    = CtAuto;     // internal - the panel no longer exposes it (Auto rolls per line)
    int  lines      = 0;          // melodic sentences in the group: 0 = Auto, 1..4 explicit
    int  relation   = RelAuto;    // what lines 2+ do vs line 1: repeat / answer / new / Auto
    bool singable   = false;
    // [2026-07-21 P1] the competitive-surface trio + the harmony override (GENERATE-THEORY v2/v4)
    int  intensity  = 1;          // 0 soft | 1 medium | 2 hard - velocity level + accent depth
    int  humanize   = 0;          // 0 off | 1 subtle | 2 loose - seed-deterministic vel + start jitter
    int  fills      = 0;          // 0 off | 1 last bar | 2 every phrase - end-of-line density bump
    int  progression = 0;         // 0 = Auto (detected/stock) | 1..6 = stock progression override
    // [2026-07-22 r20] style plumbing: the mined per-16th accent means (MIDI 0..127, 16 entries -
    // DrumGen::accentTable) shape melodic velocities; forceMono degrades Chords voicings to an
    // arpeggiated single line (step / mono targets - H9).
    const uint8_t* styleAccent = nullptr;
    bool forceMono = false;
    // [2026-07-22 r22 STAGE 2] MELODIC STYLE DNA: the parsed style (GenStyle registry entry -
    // factory or user .basamakstyle). styleVirtual = the context was EMPTY and the style's canon
    // kit is the virtual groove - the style then PULLS (bass cells placed verbatim, melody motifs
    // born from the style's cells, comp templates literal). With real context the style only
    // BIASES candidate weights - the context lattice / accents / disciplines keep authority
    // (context > style on conflicts, the established hierarchy).
    const GenStyle::Style* styleDna = nullptr;
    bool styleVirtual = false;
    uint32_t rhythmSeed = 1;
    uint32_t pitchSeed  = 2;
    int  varyCount  = 0;
};

struct Ctx
{
    int   bars = 1;
    float grooveHit[MAX_BARS * 16] = {};     // 16th-grid strength map (velocity accents + fallback grid)
    float chroma[MAX_BARS * 4][12] = {};     // per concat beat: pitch-class weights from pitched channels
    bool  chromaValid = false;
    // EXACT drum-hit positions (concat columns, sorted) - 384 divides every step count, so a
    // 7-step channel's hits are exact here. Empty = the 16th-grid fallback drives the rhythm.
    static constexpr int MAX_HITS = 256;
    int   nHits = 0;
    int   hitCol[MAX_HITS] = {};
    float hitStr[MAX_HITS] = {};             // strength 0..1
    // [P1] per-ROLE drum-hit lists beside the combined list (GenContext classifies by bank
    // category + name keywords; perc/generic hits live only in the combined list). The bass
    // listens to the KICK (G2), melody phrases around the SNARE (G8).
    int   nKick = 0, nSnare = 0, nHat = 0;
    int   kickCol[MAX_HITS]  = {};  float kickStr[MAX_HITS]  = {};
    int   snareCol[MAX_HITS] = {};  float snareStr[MAX_HITS] = {};
    int   hatCol[MAX_HITS]   = {};  float hatStr[MAX_HITS]   = {};
    // [P1] register map: each OTHER roll channel's min/median/max pitch (H5's arrangement lanes;
    // data only in P1 - the comp/counter roles consume it in P3). Semis are C4-relative.
    static constexpr int MAX_REG = 16;
    int   nReg = 0;
    int   regMin[MAX_REG] = {}, regMed[MAX_REG] = {}, regMax[MAX_REG] = {};
    // [P1 H4] chordCols[] - the SHARED harmonic-rhythm timeline: change points on STRONG cols
    // (bar starts; Busy density adds the half-bar), each with its scale-degree root. Built by
    // prepareChords() from the chroma / stock progression / the panel's progression override;
    // every role reads chords through it (chordRootDegAt consults it first).
    static constexpr int MAX_CHORDS = MAX_BARS * 4;
    int   nChords = 0;
    int   chordColAt[MAX_CHORDS] = {};
    int   chordDegAt[MAX_CHORDS] = {};
    bool  chordColsValid = false;
    // [r20 G, H6/H7] MELODY OCCUPANCY: the 16th-grid mask of where the highest-register roll
    // channel sounds (comp fills the gaps, counter-lines freeze during runs) + the arrangement
    // lane edges H5 reads (melody median / bass median, C4-relative; -999 = unknown).
    bool  melOcc[MAX_BARS * 16] = {};
    bool  melOccValid = false;
    int   melMed = -999, bassMed = -999;
    // [P1 H8/H9] TARGET-sound introspection: the selected channel's first audible slot (env
    // times gate the length/density rules; mono/scaleOn document the single-line guarantee).
    bool  sndValid = false;
    float sndAtk = 0.0f, sndDec = 0.0f, sndSus = 0.0f, sndRel = 0.0f;
    bool  sndMono = false, sndScaleOn = false;
    int   sndMsLo = -1, sndMsHi = -1;        // multisample zone-root span (MIDI), -1 = none
    // [2026-07-22 r22 H7] THE MELODY LINE per concat 16th cell (gathered from the highest-median
    // roll channel beside melOcc): melPcP1 = the sounding pitch class + 1 (0 = silent), melOnset =
    // a melody note STARTS at this cell, melDir = that onset's motion vs the previous melody
    // onset (-1 down / 0 first-or-repeat / +1 up). The Riff counter-line rules read these.
    uint8_t melPcP1[MAX_BARS * 16] = {};
    bool    melOnset[MAX_BARS * 16] = {};
    int8_t  melDir[MAX_BARS * 16] = {};
    // [2026-07-22 r21] THE CONTEXT LATTICE - the bar grid every GENERATED onset lives on.
    // The gather (GenContext::build) sets latticeN = the LCM of the contributing step
    // channels' grids (hit-weight priority under the 48-cell cap; binary/coarse grids fold
    // back to the classic 16; empty or roll-only context = 16). generate() derives the
    // per-cell STRENGTH map (metric hierarchy by divisor depth + 0.7 * the loudest hit at
    // the cell - the G7 accent hierarchy) and the LHL weights from it (prepareLatticeImpl).
    // Every rhythm-shaping rule (candidates, de-syncopation, pushes, breaths, prosody)
    // moves in LATTICE CELLS - the r20 batch hardcoded these to a 16-col 4/4 bar, which
    // dragged onsets born on a 7/14-step context onto 16th positions (the user's bug).
    static constexpr int LAT_MAX = 48;
    int    latticeN = 16;
    float  latStr[MAX_BARS * LAT_MAX] = {};
    int8_t latW[MAX_BARS * LAT_MAX] = {};
};

struct Note
{
    int start = 0, len = 1, semi = 0, vel = 235;
    bool approach = false;   // [P1 G9] bass approach note INTO a chord change (step writer -> Slide)
    bool core = false;       // [r20 M10] a motif placement's first two notes = the IDENTITY -
                             // later passes may never rewrite their pitches
};

// --- deterministic RNG (xorshift32) --------------------------------------------------------------
struct Rng
{
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9e3779b9u) {}
    uint32_t next()          { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float    uf()            { return (float) (next() >> 8) / 16777216.0f; }
    int      ri(int n)       { return n <= 1 ? 0 : (int) (next() % (uint32_t) n); }
    bool     chance(float p) { return uf() < p; }
};

namespace detail
{
static inline int pc(int semi) { return ((semi % 12) + 12) % 12; }

static inline void buildLadder(const Options& o, int lo, int hi, std::vector<int>& out)
{
    bool member[12] = {};
    for (int i = 0; i < o.scaleLen; ++i) member[pc(o.key + o.scale[i])] = true;
    out.clear();
    for (int s = lo; s <= hi; ++s) if (member[pc(s)]) out.push_back(s);
}

static inline void chordPcs(const Options& o, int rootDeg, bool out[12])
{
    for (int i = 0; i < 12; ++i) out[i] = false;
    const int L = o.scaleLen;
    for (int k = 0; k < 3; ++k) out[pc(o.key + o.scale[(rootDeg + 2 * k) % L])] = true;
}

// chord ROOT scale degree from a chroma weight set (best chord = root x2 + its triad x0.5)
static inline int bestDegFromWeights(const Options& o, const float w[12], float& bestOut)
{
    int bestDeg = 0; float best = 0.0f;
    for (int d = 0; d < o.scaleLen; ++d)
    {
        bool cp[12]; chordPcs(o, d, cp);
        float sc = w[pc(o.key + o.scale[d])] * 2.0f;
        for (int q = 0; q < 12; ++q) if (cp[q]) sc += w[q] * 0.5f;
        if (sc > best) { best = sc; bestDeg = d; }
    }
    bestOut = best;
    return bestDeg;
}

// Stock progression when no harmony is heard (pentatonic/blues stay on the tonic).
static inline int stockDeg(const Options& o, int bars, int bar)
{
    if (o.scaleLen < 6) return 0;
    static const int p2[2] = { 0, 4 };
    static const int p4[4] = { 0, 5, 3, 4 };
    static const int p8[8] = { 0, 0, 5, 5, 3, 3, 4, 4 };
    if (bars >= 8) return p8[bar % 8];
    if (bars >= 4) return p4[bar % 4];
    if (bars >= 2) return p2[bar % 2];
    return 0;
}

// (tiny local clamp so this header stays JUCE-free)
static inline int juceLikeClamp(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

// [r22] shared ladder steppers (free-function versions of the motif engine's lambdas - the
// finesse/counter-line passes need them too): nearest ladder index, then step by scale degrees.
static inline int ladSnapOf(const std::vector<int>& lad, int want)
{
    int best = want, bd = 1 << 20;
    for (int s2 : lad) if (std::abs(s2 - want) < bd) { bd = std::abs(s2 - want); best = s2; }
    return best;
}
static inline int ladStepOf(const std::vector<int>& lad, int semi, int step)
{
    if (lad.empty()) return semi;
    int idx = 0, bd = 1 << 20;
    for (int i = 0; i < (int) lad.size(); ++i)
        if (std::abs(lad[(size_t) i] - semi) < bd) { bd = std::abs(lad[(size_t) i] - semi); idx = i; }
    idx = juceLikeClamp(0, (int) lad.size() - 1, idx + step);
    return lad[(size_t) idx];
}
// [r22 STAGE 2] weighted pick over a style-cell list (deterministic - the caller's Rng)
template <typename CellT>
static inline const CellT* pickWeighted(const std::vector<CellT>& cells, Rng& r)
{
    if (cells.empty()) return nullptr;
    float tot = 0.0f; for (const auto& c : cells) tot += c.w;
    float x = r.uf() * (tot > 0.0f ? tot : 1.0f);
    for (const auto& c : cells) { x -= c.w; if (x <= 0.0f) return &c; }
    return &cells.back();
}

// [P1 H4] stock progressions (the panel's Chords override; 0 = Auto). Degrees are 0-based scale
// degrees - in a minor scale i-VI-III-VII reads {0,5,2,6} against the natural-minor intervals.
static const int kProgLen[7]     = { 0, 4, 4, 4, 3, 3, 12 };
static const int kProgDeg[7][12] = { {},
    { 0, 4, 5, 3 },                          // I-V-vi-IV
    { 0, 5, 3, 4 },                          // I-vi-IV-V
    { 0, 5, 2, 6 },                          // i-VI-III-VII
    { 1, 4, 0 },                             // ii-V-I
    { 0, 3, 4 },                             // I-IV-V
    { 0, 0, 0, 0, 3, 3, 0, 0, 4, 3, 0, 4 } };// 12-bar blues

// [P1 H4] build the SHARED chordCols[] harmonic-rhythm timeline into c: change points on strong
// cols (bar starts; Busy adds the half-bar), degree = the segment's chroma (bestDegFromWeights),
// the stock progression when nothing pitched speaks, or the explicit override when the user
// picked one. Every role reads harmony through this ONE timeline.
static inline void prepareChordsImpl(const Options& o, Ctx& c)
{
    c.nChords = 0;
    // [r22 originality] harmonic-rhythm fan-out WITHIN Density's band (style-era only - callers
    // without a style keep the exact r20 behaviour): Medium may roll 2 changes/bar (p = .25),
    // Busy may relax to 1 (p = .2). Deterministic from the rhythm seed = New idea rerolls it.
    int perBar = (o.density >= 2) ? 2 : 1;         // H4: sparse/medium = 1 chord/bar, busy = 2
    const GenStyle::Style* st = o.styleDna;
    if (st != nullptr && o.density >= 1)
    {
        Rng hrr(o.rhythmSeed ^ 0x4A72B915u);
        if (o.density == 1 && hrr.chance(0.25f)) perBar = 2;
        else if (o.density >= 2 && hrr.chance(0.20f)) perBar = 1;
    }
    // [r22 originality] the style's PROGRESSION POOL replaces the stock fallback: New idea's
    // fresh rhythm seed picks a new pool entry (the user's Chords override + detected chroma
    // still outrank it - context > style).
    const GenStyle::ProgCell* pool = nullptr;
    if (st != nullptr && ! st->progs.empty() && ! (o.progression >= 1 && o.progression <= 6))
    {
        Rng prr(o.rhythmSeed ^ 0x9706D00Du);
        pool = pickWeighted(st->progs, prr);
    }
    for (int b = 0; b < c.bars && c.nChords < Ctx::MAX_CHORDS; ++b)
        for (int h = 0; h < perBar && c.nChords < Ctx::MAX_CHORDS; ++h)
        {
            const int col = b * COLS + h * (COLS / 2);
            int deg = -1;
            if (o.progression >= 1 && o.progression <= 6)
                deg = kProgDeg[o.progression][b % kProgLen[o.progression]];
            else if (c.chromaValid)
            {   // detected: aggregate the chroma of the beats this change point governs
                float w[12] = {}; float tot = 0.0f;
                const int bt0 = b * 4 + (perBar == 2 ? h * 2 : 0);
                const int bt1 = b * 4 + (perBar == 2 ? h * 2 + 2 : 4);
                for (int bt = bt0; bt < bt1 && bt < c.bars * 4; ++bt)
                    for (int p = 0; p < 12; ++p) { w[p] += c.chroma[bt][p]; tot += c.chroma[bt][p]; }
                if (tot > 0.01f)
                {
                    float best = 0.0f;
                    const int d = bestDegFromWeights(o, w, best);
                    if (best > 0.01f) deg = d;
                }
            }
            if (deg < 0 && pool != nullptr && o.scaleLen >= 6)   // [r22] the style pool speaks
                deg = pool->degs[(size_t) (b % (int) pool->degs.size())];
            if (deg < 0) deg = stockDeg(o, c.bars, b);
            c.chordColAt[c.nChords] = col;
            c.chordDegAt[c.nChords] = deg % (o.scaleLen > 0 ? o.scaleLen : 7);
            ++c.nChords;
        }
    c.chordColsValid = c.nChords > 0;
}

// Chord root at a column: the chordCols timeline when built (P1 - the shared harmonic rhythm),
// else the v2 per-beat chroma fallback (kept for direct/legacy callers).
static inline int chordRootDegAt(const Options& o, const Ctx& c, int col)
{
    if (c.chordColsValid)
    {
        int deg = c.chordDegAt[0];
        for (int i = 0; i < c.nChords; ++i)
        { if (c.chordColAt[i] <= col) deg = c.chordDegAt[i]; else break; }
        return deg;
    }
    const int bar  = col / COLS;
    const int beat = juceLikeClamp(0, c.bars * 4 - 1, bar * 4 + (col % COLS) / BEAT);
    float best = 0.0f;
    if (c.chromaValid)
    {
        int deg = bestDegFromWeights(o, c.chroma[beat], best);
        if (best > 0.01f) return deg;
        float w[12] = {};
        for (int bt = bar * 4; bt < bar * 4 + 4 && bt < c.bars * 4; ++bt)
            for (int p = 0; p < 12; ++p) w[p] += c.chroma[bt][p];
        deg = bestDegFromWeights(o, w, best);
        if (best > 0.01f) return deg;
    }
    return stockDeg(o, c.bars, bar);
}

// ============================================================================
// [2026-07-22 r21] LATTICE CELL MATH. Columns use the SAME integer division as the gather's
// hit columns (k * COLS / n, truncated) so a context channel's hits ARE lattice cols by
// construction (a 7-step kick's col 54 = cell 1 of the 7/14 lattice, never "almost 55").
// ============================================================================
static inline int latN(const Ctx& c)
{ return c.latticeN < 1 ? 16 : (c.latticeN > Ctx::LAT_MAX ? Ctx::LAT_MAX : c.latticeN); }
static inline int latCw(const Ctx& c) { return COLS / latN(c); }   // ~cell width (distance unit)
static inline int latColOf(const Ctx& c, int g)                    // global cell -> concat col
{
    const int n = latN(c);
    if (g < 0) g = 0;
    return (g / n) * COLS + (g % n) * COLS / n;
}
static inline int latCellNear(const Ctx& c, int col)               // concat col -> NEAREST cell
{
    const int n = latN(c);
    if (col < 0) col = 0;
    return (col / COLS) * n + ((col % COLS) * n + COLS / 2) / COLS;   // may be the next bar's 0
}
static inline int latSnap(const Ctx& c, int col) { return latColOf(c, latCellNear(c, col)); }
// metric part of the strength: beat hierarchy by DIVISOR DEPTH where the lattice divides in
// halves (n = 16 reproduces the old metricW's 16th values EXACTLY); non-divisible depths are
// uniformly weak - on a 7/14 lattice only the downbeat (and the half, when even) is metric.
static inline float latMetric(int k, int n)
{
    if (k == 0)                         return 1.0f;
    if (n % 2 == 0 && k % (n / 2) == 0) return 0.8f;
    if (n % 4 == 0 && k % (n / 4) == 0) return 0.6f;
    if (n % 8 == 0 && k % (n / 8) == 0) return 0.35f;
    return 0.2f;
}
static inline float latStrAt(const Ctx& c, int col)                // combined strength at a col
{
    const int n = latN(c), cap = c.bars * n - 1;
    const int g = juceLikeClamp(0, cap < 0 ? 0 : cap, latCellNear(c, col));
    return c.latStr[g];
}
// [r21 G7] the strength + LHL weight maps: strength(cell) = metric part + 0.7 * the loudest
// hit AT the cell (hits map by nearest cell, so a grid the 48-cap dropped still accents its
// neighbourhood); LHL weight = the old 0/-1/-2/-3/-4 ladder read off the strength (n = 16
// with no hits reproduces the retired kLhlW table bit for bit).
static inline void prepareLatticeImpl(Ctx& c)
{
    const int n = latN(c);
    c.latticeN = n;
    const int total = juceLikeClamp(1, MAX_BARS * Ctx::LAT_MAX, c.bars * n);
    for (int g = 0; g < total; ++g) c.latStr[g] = latMetric(g % n, n);
    for (int i = 0; i < c.nHits; ++i)
    {
        const int g = latCellNear(c, c.hitCol[i]);
        if (g < 0 || g >= total) continue;               // the LOUDEST hit at a cell wins (G7)
        c.latStr[g] = std::max(c.latStr[g], latMetric(g % n, n) + 0.7f * c.hitStr[i]);
    }
    for (int g = 0; g < total; ++g)
    {
        if (c.latStr[g] > 1.0f) c.latStr[g] = 1.0f;
        const int w = (int) std::lround((c.latStr[g] - 1.0f) * 5.0f);
        c.latW[g] = (int8_t) juceLikeClamp(-4, 0, w);
    }
}
// [r21] the ONE strong-position rule (G7): metric beat-depth cells are strong; 8th-depth
// cells become landings once real hits exist (the old "hit-locked lines treat 8ths as
// landings"); on a lattice WITHOUT binary beat depth (7/14/21...), a cell the groove itself
// accents (combined strength >= 0.6) is strong - the accent hierarchy IS the meter there.
static inline bool latStrong(const Ctx& c, int col, bool hitLandings)
{
    const int n = latN(c);
    const float m = latMetric(latCellNear(c, col) % n, n);
    if (m >= 0.6f) return true;
    if (hitLandings && c.nHits > 0 && m >= 0.35f) return true;
    if (n % 4 != 0 && latStrAt(c, col) >= 0.6f) return true;
    return false;
}

// [P1 G3 -> r21 LATTICE] LHL syncopation scorer: a note on a weak position followed by
// SILENCE on a stronger one is syncopated; its contribution = strongest-silent-weight minus
// the note's weight. Shared by the generator's de-syncopation ceiling AND the GenTest
// scorecard (one implementation, never two). lat == nullptr = the legacy fixed 16-col 4/4
// table (position weights 0/-1/-2/-3/-4) for callers with no context; the generator always
// passes its PREPARED ctx, so the weights follow the context lattice + its hits (G7).
static const int kLhlW16[16] = { 0, -4, -3, -4, -2, -4, -3, -4, -1, -4, -3, -4, -2, -4, -3, -4 };
static inline int lhlBarScoreImpl(const std::vector<Note>& notes, int bar,
                                  int* worstIdx = nullptr, int* worstTargetCol = nullptr,
                                  const Ctx* lat = nullptr)
{
    const int n = lat != nullptr ? latN(*lat) : 16;
    bool on[Ctx::LAT_MAX + 1] = {}, covered[Ctx::LAT_MAX + 1] = {};
    int  noteAt[Ctx::LAT_MAX]; for (int i = 0; i < n; ++i) noteAt[i] = -1;
    auto wAt = [&](int p) -> int
    {
        if (p >= n) return 0;                            // position n = the next downbeat
        if (lat == nullptr) return kLhlW16[p & 15];
        const int cap = lat->bars * n - 1;
        return lat->latW[juceLikeClamp(0, cap < 0 ? 0 : cap, bar * n + p)];
    };
    if (lat != nullptr)
        for (int i = 0; i < lat->nHits; ++i)
        {   // [r21] a cell a REAL DRUM plays is COVERED: the ensemble meets the expectation
            // there, so it ends a melodic note's "air" like an onset does - without this the
            // hit-aware weights condemned Pockets (forbidden from the very cells the scorer
            // pointed at) to unfixable syncopation scores.
            const int g = latCellNear(*lat, lat->hitCol[i]) - bar * n;
            if (g >= 0 && g < n) covered[g] = true;
        }
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const int local = notes[i].start - bar * COLS;
        if (local < 0 || local >= COLS) continue;
        const int p = juceLikeClamp(0, n - 1, (local * n + COLS / 2) / COLS);
        if (! on[p]) { on[p] = true; noteAt[p] = (int) i; }
    }
    int score = 0, worstGain = 0;
    if (worstIdx != nullptr) *worstIdx = -1;
    for (int p = 0; p < n; ++p)
    {
        if (! on[p]) continue;
        int bestW = -99, bestQ = -1;
        for (int q = p + 1; q <= n; ++q)
        {
            if (q < n && (on[q] || covered[q])) break;   // an onset OR a drum hit ends the air
            const int wq = wAt(q);
            if (wq > bestW) { bestW = wq; bestQ = q; }
        }
        if (bestQ >= 0 && bestW > wAt(p))
        {
            const int gain = bestW - wAt(p);
            score += gain;
            if (gain > worstGain && bestQ < n)
            { worstGain = gain;
              if (worstIdx != nullptr)       *worstIdx = noteAt[p];
              if (worstTargetCol != nullptr)
                  *worstTargetCol = lat != nullptr ? latColOf(*lat, bar * n + bestQ)
                                                   : bar * COLS + bestQ * CELL16; }
        }
    }
    return score;
}

// ---- RHYTHM v2: candidates for one PHRASE span, from the groove's REAL positions --------------
struct Cand { int col; float w; };

// [r22 STAGE 2] MELODIC STYLE DNA as candidate BIAS: the style's cells (bass tuples / melody
// cells / comp templates, by role) multiply matching candidates' weights; from scratch (the
// virtual groove) the pull is stronger AND missing style positions join as candidates. The
// context keeps authority: bias never overrides exclusions - pockets/G8/LHL still move onsets.
static inline void styleBiasCands(const Options& o, const Ctx& c, int s, int e, std::vector<Cand>& out)
{
    if (o.styleDna == nullptr) return;
    const auto& st = *o.styleDna;
    bool cellHas[16] = {};
    if (o.role == RoleBass)
        for (const auto& bc : st.bass) for (const auto& ev : bc.ev) cellHas[ev.pos16 & 15] = true;
    else if (o.role == RoleChords)
        for (const auto& cc2 : st.comp) for (int p : cc2.pos16) cellHas[p & 15] = true;
    else
        for (const auto& mc : st.mel) for (const auto& ev : mc.ev) cellHas[ev.pos16 & 15] = true;
    bool any = false; for (bool b2 : cellHas) any = any || b2;
    if (! any) return;
    const float mul = o.styleVirtual ? 2.2f : 1.35f;
    for (auto& cd : out)
        if (cellHas[((cd.col % COLS) / CELL16) & 15]) cd.w *= mul;
    // the style's own positions JOIN as candidates: strongly from scratch, faintly with real
    // context ([r22 originality] - a wider pool = the seeds fan out instead of re-sampling the
    // same few context cells; the disciplines still move/drop whatever violates the context).
    for (int b2 = s / COLS; b2 <= (e - 1) / COLS && b2 < MAX_BARS; ++b2)
        for (int cell = 0; cell < 16; ++cell)
        {
            if (! cellHas[cell]) continue;
            const int col = latSnap(c, b2 * COLS + cell * CELL16);
            if (col < s || col >= e) continue;
            bool have = false;
            for (auto& cd : out) if (cd.col == col) { have = true; break; }
            if (! have) out.push_back({ col, o.styleVirtual ? 0.6f : 0.35f });
        }
}

static inline void phraseCandidates(const Options& o, const Ctx& c, int s, int e, std::vector<Cand>& out)
{
    out.clear();
    const int n = latN(c);
    const int gS = latCellNear(c, s), gE = latCellNear(c, e);   // s/e = bar multiples = exact cells
    auto beatCands = [&](float wMul)
    {   // [r21] the METER's positions ON THE LATTICE (beats strong, half-beats faint; HUM =
        // beats only - its LHL band [0,2] cannot afford a half-beat note syncopating against
        // the next downbeat). A lattice without binary beat depth (7/14...) treats the cells
        // the groove ACCENTS (combined strength >= 0.6) as its beats - the G7 hierarchy.
        for (int g = gS; g < gE; ++g)
        {
            const int col = latColOf(c, g);
            if (col < s || col >= e) continue;
            float m = latMetric(g % n, n);
            if (n % 4 != 0) m = std::max(m, latStrAt(c, col));
            if (m >= 0.6f) out.push_back({ col, m * wMul });          // beats
            else if (m >= 0.35f && o.role != RoleHum)
                out.push_back({ col, 0.14f * wMul });                 // half-beats, rare
        }
    };
    // collect the real hits inside the span
    std::vector<int> hIdx;
    for (int i = 0; i < c.nHits; ++i)
        if (c.hitCol[i] >= s && c.hitCol[i] < e) hIdx.push_back(i);
    // [r20 G, H6/H7] comp + counter roles weigh candidates AGAINST the melody's occupancy:
    // x0.3 under a melody note, x1.6 in its gaps (freeze-during-runs falls out of the weight)
    struct OccW
    {
        const Ctx& c2; const Options& o2;
        void apply(std::vector<Cand>& v) const
        {
            if (! c2.melOccValid || (o2.role != RoleChords && o2.role != RoleRiff)) return;
            for (auto& cd : v)
            {
                const int p = juceLikeClamp(0, MAX_BARS * 16 - 1, cd.col / CELL16);
                cd.w *= c2.melOcc[p] ? 0.3f : 1.6f;
            }
        }
    };
    const OccW occW { c, o };
    struct OccGuard
    {
        const OccW& w; std::vector<Cand>& v;
        ~OccGuard() { w.apply(v); }
    } occGuard { occW, out };
    // [r22 STAGE 2] declared AFTER occGuard = destroyed FIRST: the style bias lands, THEN the
    // occupancy weighting multiplies everything (style adds included) - context stays on top.
    struct StyleGuard
    {
        const Options& o2; const Ctx& c2; int s2, e2; std::vector<Cand>& v;
        ~StyleGuard() { styleBiasCands(o2, c2, s2, e2, v); }
    } styleGuard { o, c, s, e, out };

    if (o.rhythm == RhFlowing || hIdx.empty())
    {
        // Flowing floats on the METER (a vocal line breathes with the bar, not the hats);
        // and every stance falls back here when the groove offers nothing.
        if (o.rhythm == RhDriving && hIdx.empty())      beatCands(1.0f);
        else if (o.rhythm == RhPockets && hIdx.empty())
        {   // no drums to dodge: the offbeat-leaning LATTICE grid [r21]
            for (int g = gS; g < gE; ++g)
            {
                const int col = latColOf(c, g);
                if (col < s || col >= e) continue;
                const float m = latMetric(g % n, n);
                out.push_back({ col, m < 0.5f ? 0.30f + m : 0.25f });
            }
        }
        else beatCands(1.0f);
        return;
    }
    if (o.rhythm == RhDriving)
    {   // lock STRICTLY onto the drums' real positions (the odd-step-count promise:
        // a 7-step kick's hits land exactly - no grid, no rounding)
        for (int i : hIdx) out.push_back({ c.hitCol[i], 0.35f + 0.75f * c.hitStr[i] });
        return;
    }
    // Pockets: the MIDPOINTS of the real gaps between hits (plus the span edges' gaps),
    // SNAPPED to the lattice [r21] - a raw midpoint was an off-any-grid col, which is what
    // made the step-count chooser fall back to its densest grid on odd-step contexts.
    int prev = s;
    for (size_t k = 0; k <= hIdx.size(); ++k)
    {
        const int nxt = k < hIdx.size() ? c.hitCol[hIdx[k]] : e;
        const int gap = nxt - prev;
        if (gap >= latCw(c) * 2)
            out.push_back({ latSnap(c, prev + gap / 2),
                            0.25f + 0.75f * std::min(1.0f, (float) gap / (float) BEAT) });
        prev = nxt;
    }
}

// weighted sample `budget` onsets from the candidates (no replacement; min one lattice cell
// apart - two cells when singable)
static inline void sampleOnsets(const Options& o, const Ctx& c, Rng& rrng, std::vector<Cand>& cands,
                                int budget, int phraseStart, int breathEnd, std::vector<int>& out)
{
    out.clear();
    for (int k = 0; k < budget && ! cands.empty(); ++k)
    {
        float tot = 0.0f; for (auto& cd : cands) tot += cd.w;
        if (tot <= 0.0001f) break;
        float r = rrng.uf() * tot; size_t sel = 0;
        for (size_t i = 0; i < cands.size(); ++i) { r -= cands[i].w; if (r <= 0.0f) { sel = i; break; } }
        const int col = cands[sel].col;
        if (col < breathEnd) out.push_back(col);
        const int minGap = o.singable ? latCw(c) * 2 : latCw(c);
        cands.erase(std::remove_if(cands.begin(), cands.end(),
                    [&](const Cand& cd) { return std::abs(cd.col - col) < minGap; }), cands.end());
    }
    // a phrase must start SOMEWHERE near its head - anchor if the dice left the opening empty
    bool early = false;
    for (int col : out) if (col < phraseStart + BEAT + CELL16) early = true;
    if (! early) out.push_back(phraseStart);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// ---- PITCH v2 ---------------------------------------------------------------------------------
struct PitchState { int prevIdx = -1; };

// [r20 E] ONE velocity law for every melodic note: the mined per-16th accent means (when a style
// is wired) or the metric fallback, softened for vocal/flowing lines, humanized by the pitch dice.
// (The accent table stays a per-16th read - it is a mined SHAPE, sampled at the col's position.)
static inline int velFor(const Options& o, const Ctx& c, Rng& prng, int col, bool strong)
{
    float v;
    if (o.styleAccent != nullptr)
        v = 118.0f + (float) o.styleAccent[((col % COLS) / CELL16) & 15] * 1.30f;
    else
        v = strong ? 232.0f : (latStrAt(c, col) >= 0.35f ? 205.0f : 188.0f);
    if (o.role == RoleHum || o.rhythm == RhFlowing) v = 200.0f + (v - 200.0f) * 0.4f;
    v += (prng.uf() - 0.5f) * 24.0f;
    return std::max(120, std::min(255, (int) std::lround(v)));
}

// [r20 E, M11] cadence degrees said out loud: a QUESTION ends on the 2nd / 5th / 7th (the
// sentence continues), an ANSWER on the tonic / 3rd (bass: tonic only). Shared by the motif
// engine; pitchPhrase keeps its internal snap for the bass path.
static inline void snapCadenceNote(const Options& o, const std::vector<int>& lad, Note& n, bool resolved)
{
    const int L = o.scaleLen;
    bool cad[12] = {};
    if (resolved)
    {
        cad[pc(o.key + o.scale[0])] = true;
        if (o.role != RoleBass) cad[pc(o.key + o.scale[2 % L])] = true;
    }
    else
    {
        cad[pc(o.key + o.scale[4 % L])] = true;
        cad[pc(o.key + o.scale[1 % L])] = true;
        if (L >= 7) cad[pc(o.key + o.scale[6 % L])] = true;   // the leading tone asks a question too
    }
    if (cad[pc(n.semi)]) return;
    int best = n.semi, bd = 1 << 20;
    for (int s2 : lad)
        if (cad[pc(s2)] && std::abs(s2 - n.semi) < bd) { bd = std::abs(s2 - n.semi); best = s2; }
    n.semi = best;
}

static inline int registerCentre(const Options& o)
{
    if (o.role == RoleBass)  return o.registerBand == 0 ? -19 : -14;
    if (o.role == RoleChords) { static const int cc2[3] = { -10, -3, +5 }; return cc2[o.registerBand]; }
    static const int ctr[3] = { -15, -4, +8 };
    return ctr[o.registerBand];
}

// pitch one phrase's notes [from..to): contour-biased ladder walk, chord tones on beats,
// then snap the FINAL note to the phrase's cadence (open = 2nd/5th, resolved = tonic/3rd).
static inline void pitchPhrase(const Options& o, const Ctx& c, Rng& prng, std::vector<Note>& notes,
                               PitchState& st, int from, int to, bool resolvedCadence,
                               int ctOverride, int idealShift)
{
    const int centre = registerCentre(o);
    const int range  = o.singable ? 6 : 8;
    std::vector<int> ladder;
    buildLadder(o, centre - range, centre + range, ladder);
    if (ladder.empty()) buildLadder(o, centre - 12, centre + 12, ladder);
    if (ladder.empty()) return;

    int ct = ctOverride >= 0 ? ctOverride : o.contour;
    if (ct == CtAuto) ct = 1 + prng.ri(4);
    auto target = [&](int col) -> float
    {
        const float t = to > from ? (float) (col - from) / (float) (to - from) : 0.0f;
        float u = 0.5f;
        switch (ct) { case CtArch:    u = std::sin(t * 3.14159265f); break;
                      case CtRising:  u = t; break;
                      case CtFalling: u = 1.0f - t; break;
                      case CtWave:    u = 0.5f + 0.5f * std::sin(t * 6.2831853f); break; }
        return (float) centre + (float) idealShift + (u - 0.5f) * 2.0f * (float) range * 0.8f;
    };

    Note* lastNote = nullptr;
    for (auto& n : notes)
    {
        if (n.start < from || n.start >= to) continue;
        const bool strong = latStrong(c, n.start, true);   // [r21] G7: lattice beats + hit landings
        const int rootDeg = chordRootDegAt(o, c, n.start);                 // [v2] per-BEAT harmony
        bool cp[12]; chordPcs(o, rootDeg, cp);
        const float ideal = st.prevIdx < 0 ? target(n.start)
                          : 0.45f * (float) ladder[st.prevIdx] + 0.55f * target(n.start);
        int idx = st.prevIdx < 0 ? 0 : st.prevIdx;
        if (o.role == RoleBass && strong)
        {
            const int wantPc = prng.chance(0.7f) ? pc(o.key + o.scale[rootDeg])
                             : prng.chance(0.66f) ? pc(o.key + o.scale[(rootDeg + 4) % o.scaleLen])
                                                  : pc(o.key + o.scale[rootDeg]);
            int best = -1; float bd = 1e9f;
            for (int i = 0; i < (int) ladder.size(); ++i)
                if (pc(ladder[i]) == wantPc)
                { const float d = std::fabs((float) ladder[i] - ideal); if (d < bd) { bd = d; best = i; } }
            idx = best >= 0 ? best : idx;
        }
        else if (strong || st.prevIdx < 0)
        {
            int c1 = -1, c2 = -1; float d1 = 1e9f, d2 = 1e9f;
            for (int i = 0; i < (int) ladder.size(); ++i)
            {
                if (! cp[pc(ladder[i])]) continue;
                const float d = std::fabs((float) ladder[i] - ideal);
                if (d < d1)      { d2 = d1; c2 = c1; d1 = d; c1 = i; }
                else if (d < d2) { d2 = d; c2 = i; }
            }
            if (c1 >= 0) idx = (c2 >= 0 && prng.chance(0.3f)) ? c2 : c1;
        }
        else
        {
            int maxStep = o.color == 2 ? 4 : (o.color == 1 ? 3 : 2);
            if (o.singable) maxStep = std::min(maxStep, 2);
            const int dir = ((float) ladder[idx] < ideal) ? 1 : -1;
            int step = 1 + (prng.chance(0.3f) ? prng.ri(maxStep) : 0);
            if (prng.chance(0.25f)) step = -step;
            idx += dir * step;
            if (o.role == RoleBass && o.rhythm == RhDriving && prng.chance(0.5f))
                idx = st.prevIdx;
        }
        idx = std::max(0, std::min((int) ladder.size() - 1, idx));
        int semi = ladder[idx];
        if (! strong && o.color > 0 && prng.chance(o.color == 2 ? 0.25f : 0.10f))
        {
            const int chrom = semi + (prng.chance(0.5f) ? -1 : 1);
            if (std::abs(chrom - centre) <= range + 1) semi = chrom;
        }
        if (o.singable && st.prevIdx >= 0 && std::abs(semi - ladder[st.prevIdx]) > 7)
            semi = ladder[st.prevIdx] + (semi > ladder[st.prevIdx] ? 7 : -7);
        n.semi = std::max(-48, std::min(48, semi));
        st.prevIdx = idx;

        n.vel = velFor(o, c, prng, n.start, strong);   // [r20 E] mined accent shape when styled
        lastNote = &n;
    }
    // CADENCE [v2]: the phrase's final note lands OPEN (2nd/5th) or RESOLVED (tonic/3rd) -
    // this is what makes a question sound unfinished and an answer sound finished.
    if (lastNote != nullptr)
    {
        const int L = o.scaleLen;
        int cadDeg[2];
        if (resolvedCadence) { cadDeg[0] = 0; cadDeg[1] = o.role == RoleBass ? 0 : 2 % L; }
        else                 { cadDeg[0] = 4 % L; cadDeg[1] = 1 % L; }
        bool cadPc[12] = {};
        cadPc[pc(o.key + o.scale[cadDeg[0]])] = true;
        cadPc[pc(o.key + o.scale[cadDeg[1]])] = true;
        if (! cadPc[pc(lastNote->semi)])
        {
            int best = lastNote->semi, bd = 99;
            for (int s2 : ladder)
                if (cadPc[pc(s2)] && std::abs(s2 - lastNote->semi) < bd)
                { bd = std::abs(s2 - lastNote->semi); best = s2; }
            lastNote->semi = best;
        }
    }
}

// ---- POCKET DISCIPLINE [P0 2026-07-21 r18] ----------------------------------------------------
// "In the pockets" must actually stay OUT of the drums' way. Two rules, enforced as a final pass
// (the anchor-at-phrase-start push and the echo nudges could land ON a hit before this existed -
// the user's "no drum awareness" report):
//  (a) ONSET EXCLUSION - no onset within one grid cell (CELL16 = 24 concat cols) of any real drum
//      hit; an offender shifts to the nearest legal column (the candidates already prefer the
//      larger gaps - only anchors/nudges ever violate), or drops when the drums are wall-to-wall.
//  (b) LENGTH CLIPPING - a pocket note ENDS at least 1/32 bar (12 cols) before the next drum hit,
//      so nothing rings through a hit. Driving/Flowing are untouched.
static inline void pocketDiscipline(const Options& o, const Ctx& c, std::vector<Note>& notes, int total)
{
    if (o.rhythm != RhPockets || c.nHits == 0 || notes.empty()) return;
    const int cw = latCw(c);
    auto nearHit = [&](int col)
    {
        for (int i = 0; i < c.nHits; ++i)
            if (std::abs(col - c.hitCol[i]) < cw) return true;
        return false;
    };
    for (auto& n : notes)
    {
        if (! nearHit(n.start)) continue;
        // [r21] offenders shift by LATTICE CELLS to the nearest legal cell (the old +-1-col
        // walk parked them at hit+-24 = an off-any-grid col = the count chooser's poison)
        const int g0 = latCellNear(c, n.start), gMax = c.bars * latN(c);
        int moved = -1;
        for (int d = 1; d < gMax && moved < 0; ++d)
        {
            const int lo = latColOf(c, g0 - d), hi = latColOf(c, g0 + d);
            if (g0 - d >= 0 && ! nearHit(lo))              { moved = lo; break; }
            if (g0 + d < gMax && hi < total && ! nearHit(hi)) { moved = hi; break; }
        }
        if (moved >= 0) n.start = moved;
        else            n.vel = 0;   // wall-to-wall drums: no legal pocket exists - drop the note
    }
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [](const Note& n) { return n.vel == 0; }), notes.end());
    for (auto& n : notes)                            // (b) end >= 12 cols before the NEXT hit
        for (int i = 0; i < c.nHits; ++i)            // (hitCol is sorted - first hit past the start)
            if (c.hitCol[i] > n.start)
            { n.len = std::max(1, std::min(n.len, c.hitCol[i] - COLS / 32 - n.start)); break; }
}

static inline void makeLengths(const Options& o, const Ctx& c, std::vector<Note>& notes, int totalCols)
{
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    const int cw = latCw(c);   // [r21] length floors follow the lattice cell (durations only -
    for (size_t i = 0; i < notes.size(); ++i)   //  lengths never touch the onset grid)
    {
        const int nextStart = i + 1 < notes.size() ? notes[i + 1].start : totalCols;
        int gap = std::max(cw, nextStart - notes[i].start);
        int len;
        if (o.rhythm == RhFlowing || o.role == RoleHum) len = gap;   // legato arcs (may cross bar lines)
        else if (o.rhythm == RhPockets)                 len = std::min(gap, BEAT) * 9 / 10;
        else                                            len = std::min(gap, BEAT / 2) * 9 / 10;
        if (o.role == RoleHum) len = std::max(len, BEAT / 2);
        len = std::min(len, totalCols - notes[i].start);
        notes[i].len = std::max(cw / 2, std::min(len, gap));
    }
}

// ---- FORM PLANNING [v2] -----------------------------------------------------------------------
// Phrases are up to 2 bars; tags: 'A' fresh motif | 'a' echo (opening kept, tail varied) |
// 'n' answer (opening kept ~40%, fresh tail) | 'B' contrast | 'F' free. Last phrase resolves.
struct Phrase { int startBar, lenBars; char tag; bool resolved; };

// [v3] LINES model: split the group into `lines` sentences (earlier lines take the extra bar -
// 3 bars at 2 lines = 2 + 1), then decide what each later line does. AABA and friends EMERGE
// from Auto instead of being a jargon menu. lines 1 = ONE arc across the whole group.
// [r22 originality] fanSeed != 0 (style-era callers): AUTO lines/relations ROLL within their
// sensible band instead of a fixed table, so New idea rerolls the form too. Explicit picks and
// seedless callers (old tests) keep the exact v3 behaviour.
static inline void planForm(int bars, int lines, int relation, std::vector<Phrase>& out,
                            uint32_t fanSeed = 0)
{
    out.clear();
    int L = lines;
    if (L <= 0) L = bars <= 1 ? 1 : (bars >= 6 ? 4 : 2);   // Auto: long arcs by default
    if (lines <= 0 && fanSeed != 0 && bars >= 2)
    {
        Rng fr(fanSeed);
        if (bars >= 6)      L = 3 + fr.ri(2);              // 3 or 4 sentences
        else if (bars >= 4) L = fr.chance(0.3f) ? 1 : 2;   // one long arc sometimes
        else                L = fr.chance(0.25f) ? 1 : 2;
        if (relation <= 0 && fr.chance(0.3f)) relation = 1 + fr.ri(3);   // Repeat/Answer/New roll
    }
    L = std::max(1, std::min(std::min(4, bars), L));
    const int baseB = bars / L, extra = bars % L;
    int b = 0;
    for (int i = 0; i < L; ++i)
    {
        Phrase p; p.startBar = b; p.lenBars = baseB + (i < extra ? 1 : 0); p.resolved = false;
        if (i == 0) p.tag = 'A';
        else if (relation == RelRepeat) p.tag = 'a';
        else if (relation == RelAnswer) p.tag = 'n';
        else if (relation == RelNew)    p.tag = 'F';
        else   // Auto: 2 lines = question/answer; 3 = Q/A/echo; 4 = hook/echo/contrast/echo (AABA)
            p.tag = (L == 2) ? 'n'
                  : (L == 3) ? (i == 1 ? 'n' : 'a')
                             : (i == 1 ? 'a' : i == 2 ? 'B' : 'a');
        b += p.lenBars;
        out.push_back(p);
    }
    if (! out.empty()) out.back().resolved = true;
    for (auto& p : out) if (p.tag == 'n') p.resolved = true;   // answers always resolve
}
} // namespace detail

// [P1] public faces of the shared helpers (the GenTest scorecard uses the SAME implementations
// the generator runs - one chord timeline, one LHL scorer, one lattice, never two).
static inline void prepareChords(const Options& o, Ctx& c) { detail::prepareChordsImpl(o, c); }
static inline void prepareLattice(Ctx& c)                  { detail::prepareLatticeImpl(c); }
static inline int  lhlBarScore(const std::vector<Note>& notes, int bar)
{ return detail::lhlBarScoreImpl(notes, bar); }            // legacy fixed-16 weights (no ctx)
// [r21] lattice-aware scorer + strong-col test: pass a ctx that went through prepareLattice
static inline int  lhlBarScore(const std::vector<Note>& notes, int bar, const Ctx& preparedCtx)
{ return detail::lhlBarScoreImpl(notes, bar, nullptr, nullptr, &preparedCtx); }
static inline bool strongCol(const Ctx& preparedCtx, int col)
{ return detail::latStrong(preparedCtx, col, true); }

// ================================================================================================
static inline std::vector<Note> generate(const Options& oIn, const Ctx& cIn)
{
    using namespace detail;
    Options o = oIn;
    if (o.scale == nullptr || o.scaleLen < 5)
    { static const int8_t maj[7] = { 0, 2, 4, 5, 7, 9, 11 }; o.scale = maj; o.scaleLen = 7; }
    Ctx c = cIn;   // [P1] local working copy: the chord timeline + the bass's kick view live here
    const int bars = std::max(1, std::min(MAX_BARS, c.bars));
    c.bars = bars;
    const int total = bars * COLS;
    prepareChordsImpl(o, c);   // [P1 H4] the ONE harmonic-rhythm timeline every role reads
    // [r20 G] poly comping emits SAME-START chord tones: the mono passes below must stand aside
    const bool polyChords = o.role == RoleChords && ! o.forceMono;

    // [P1 G2] BASSLINE listens to the KICK: the stance is the relationship. Driving = unison
    // (lock onto the kick's real columns), Pockets = interlock (live in the kick gaps, stay out
    // of the kick windows) - both come free by making the KICK list THE hit list for the bass;
    // Flowing = avoid (meter-floating, but every note ENDS before the next kick - clipped below).
    if (o.role == RoleBass && c.nKick > 0 && o.rhythm != RhFlowing)
    {
        c.nHits = c.nKick;
        for (int i = 0; i < c.nKick; ++i) { c.hitCol[i] = c.kickCol[i]; c.hitStr[i] = c.kickStr[i]; }
    }
    prepareLatticeImpl(c);   // [r21] strength + LHL weights on the CONTEXT lattice (after the
                             // bass kick-swap - the maps must see the hit list the rules see)

    Rng rrng(o.rhythmSeed), prng(o.pitchSeed);
    std::vector<Note> notes;

    static const int base[5][3] = { { 2, 4, 5 }, { 3, 5, 6 }, { 2, 3, 4 }, { 4, 5, 6 },
                                    { 2, 3, 4 } };   // [r20] RoleChords: comp-sparse budgets
    static const float dmul[3] = { 0.6f, 1.0f, 1.5f };
    // [P1 H8] a slow-attack sound (atk >= 120 ms) can't articulate runs - halve the onset budget
    const bool sndSlowAtk = c.sndValid && c.sndAtk >= 0.12f;
    // [r22 STAGE 2] the style's topline-density constant scales melody/hum/riff budgets
    const float styleMul = (o.styleDna != nullptr && o.role != RoleBass && o.role != RoleChords)
                         ? o.styleDna->melDensity : 1.0f;
    // [r22 G6] DENSITY COMPLEMENTARITY: the drum bed's own busyness SHRINKS the melodic onset
    // budget (budget = clamp(K - a * drumDensity) - a busy kit leaves less room to sing over).
    // Melody/Hum only; the bass tracks the kick by design, the riff tiles its identity cell.
    const int g6sub = (o.role == RoleMelody || o.role == RoleHum) && c.nHits > 0
                    ? (int) std::lround(0.3f * std::max(0.0f, (float) c.nHits / (float) bars - 4.0f))
                    : 0;
    auto budgetFor = [&](int lenBars)
    { const float sm = sndSlowAtk ? 0.5f : 1.0f;
      const int b2 = (int) std::lround((float) base[o.role][o.rhythm] * dmul[o.density] * sm
                                       * styleMul * (float) lenBars) - g6sub * lenBars;
      return std::max(1, std::min(14, b2)); };
    // [r20 E, M14] MELODIES breathe too now, not just vocal lines - phrase ends leave air
    const bool breathe = o.singable || o.role == RoleHum || o.role == RoleMelody;

    std::vector<Phrase> plan;   // [P1] hoisted: the FILLS pass reads line ends (empty for Riff)
    if (o.role == RoleRiff)
    {
        // RIFF: one bar's cell tiled every bar, transposed with the harmony (unchanged from v1;
        // tiling shifts by whole bars, so lattice cols stay lattice cols).
        std::vector<Cand> cands; std::vector<int> ons;
        phraseCandidates(o, c, 0, COLS, cands);
        sampleOnsets(o, c, rrng, cands, budgetFor(1), 0, COLS, ons);
        for (int col : ons) notes.push_back({ col, 1, 0, 235 });
        makeLengths(o, c, notes, COLS);
        PitchState st;
        pitchPhrase(o, c, prng, notes, st, 0, COLS, true, -1, 0);
        const int root0 = o.key + o.scale[chordRootDegAt(o, c, 0)];
        std::vector<Note> tiled;
        for (int b = 0; b < bars; ++b)
        {
            int shift = (o.key + o.scale[chordRootDegAt(o, c, b * COLS)]) - root0;
            while (shift > 6)  shift -= 12;
            while (shift < -6) shift += 12;
            for (auto n : notes)
            { n.start += b * COLS; n.semi = std::max(-48, std::min(48, n.semi + shift)); tiled.push_back(n); }
        }
        notes.swap(tiled);
    }
    else if (o.role == RoleChords)
    {
        // ============================================================================
        // [r20 G] COMPING (H1-H7, H9): block voicings on the H3 rhythm templates, voice-led
        // with minimal motion (common tones locked), inside the H5 register lanes, weighted
        // into the melody's gaps (H6 - via phraseCandidates' occupancy hook where sampled).
        // forceMono (step / mono targets) = the SAME voicings ARPEGGIATED as a single line.
        // ============================================================================
        const int centre = registerCentre(o);
        int laneLo = centre - 8, laneHi = centre + 9;
        if (c.melMed > -999) laneHi = std::min(laneHi, c.melMed - 3);    // H5: below the melody
        if (c.bassMed > -999 && c.bassMed < centre)
            laneLo = std::max(laneLo, c.bassMed + 7);                    // H5: above the bass
        if (laneHi - laneLo < 10) { laneLo = centre - 8; laneHi = centre + 9; }
        const int nv = (c.sndValid && c.sndSus < 0.05f) || o.density == 0 ? 3
                     : (o.density >= 2 ? 4 : 3);                         // H9: pads 3-4, keys 2-3
        // ---- H3 rhythm template per stance (Charleston / tresillo / offbeat 8ths) ----
        std::vector<int> onsCols;
        Rng cr(o.rhythmSeed ^ 0xC0117D5u ^ (0x9e3779b9u * (uint32_t) o.varyCount));
        for (int b = 0; b < bars; ++b)
        {
            const int base2 = b * COLS;
            // [r21] the H3 templates are written in 16th cells (Charleston / tresillo are 4/4
            // vocabulary) - each template col SNAPS to the context lattice on the way out
            auto add = [&](int cell) { onsCols.push_back(latSnap(c, base2 + cell * CELL16)); };
            if (o.rhythm == RhFlowing)
            {   // one hit per chord change inside this bar
                for (int ci = 0; ci < c.nChords; ++ci)
                    if (c.chordColAt[ci] >= base2 && c.chordColAt[ci] < base2 + COLS)
                        onsCols.push_back(latSnap(c, c.chordColAt[ci]));
            }
            else if (o.rhythm == RhPockets)
            {   // [r22 STAGE 2] the style's comp TEMPLATE when one ships (weighted pick per bar -
                // New idea rerolls it; H6's occupancy drop still runs after = context authority);
                // no style = the classic Charleston / tresillo pair.
                const GenStyle::CompCell* tc = o.styleDna != nullptr && ! o.styleDna->comp.empty()
                                             ? pickWeighted(o.styleDna->comp, cr) : nullptr;
                if (tc != nullptr)                     for (int p : tc->pos16) add(p);
                else if (o.density >= 2 || cr.chance(0.4f)) { add(0); add(6); add(12); }
                else                                        { add(0); add(6); }
            }
            else
            {   // Driving: offbeat 8ths with an ONBEAT anchor (never > 60% offbeat unanchored)
                add(0);
                for (int cell : { 2, 6, 10, 14 })
                    if (! cr.chance(0.25f)) add(cell);
            }
        }
        std::sort(onsCols.begin(), onsCols.end());
        onsCols.erase(std::unique(onsCols.begin(), onsCols.end()), onsCols.end());
        if (c.melOccValid)
        {   // H6 [r22]: a stab under a melody note first tries to SHIFT into a nearby GAP
            // (the comp literally fills the melody's gaps - the GENERATE ALL interlock);
            // no free gap within 2 cells = mostly drop (the old x0.3 thinning). Anchors stay.
            auto occAt = [&](int col)
            { return c.melOcc[juceLikeClamp(0, MAX_BARS * 16 - 1, col / CELL16)]; };
            for (size_t oi = 0; oi < onsCols.size(); ++oi)
            {
                int& col = onsCols[oi];
                if ((col % COLS) == 0 || ! occAt(col)) continue;
                bool moved = false;
                for (int d2 = 1; d2 <= 2 && ! moved; ++d2)
                    for (int sgn : { 1, -1 })
                    {
                        const int cand = latColOf(c, latCellNear(c, col) + sgn * d2);
                        if (cand <= 0 || cand >= bars * COLS || occAt(cand)) continue;
                        bool taken = false;
                        for (int oc2 : onsCols)
                            if (oc2 >= 0 && oc2 != col && std::abs(oc2 - cand) < latCw(c)) taken = true;
                        if (taken) continue;
                        col = cand; moved = true; break;
                    }
                if (! moved && cr.chance(0.7f)) col = -1;
            }
            onsCols.erase(std::remove(onsCols.begin(), onsCols.end(), -1), onsCols.end());
            std::sort(onsCols.begin(), onsCols.end());
            onsCols.erase(std::unique(onsCols.begin(), onsCols.end()), onsCols.end());
        }
        // ---- voicing per onset: H2 minimal-motion voice leading, H1 low-interval limits ----
        std::vector<int> prevV;
        auto chordToneUp = [&](bool cp[12], int from) -> int
        { for (int p2 = from; p2 <= laneHi; ++p2) if (cp[pc(p2)]) return p2; return -999; };
        for (size_t oi = 0; oi < onsCols.size(); ++oi)
        {
            const int col = onsCols[oi];
            bool cp[12]; chordPcs(o, chordRootDegAt(o, c, col), cp);
            std::vector<int> v;
            if (prevV.empty())
            {   // first voicing: stack chord tones upward from the lane floor, spaced >= 3 st
                int p2 = laneLo;
                while ((int) v.size() < nv)
                {
                    const int t = chordToneUp(cp, p2);
                    if (t <= -999) break;
                    v.push_back(t); p2 = t + 3;
                }
            }
            else
            {   // H2: common tones LOCK; every other voice moves to the nearest chord tone
                for (int pv : prevV)
                {
                    if (cp[pc(pv)]) { v.push_back(pv); continue; }        // common tone locked
                    int best = pv, bd = 1 << 20;
                    for (int t = std::max(laneLo, pv - 5); t <= std::min(laneHi, pv + 5); ++t)
                        if (cp[pc(t)] && std::abs(t - pv) < bd) { bd = std::abs(t - pv); best = t; }
                    v.push_back(best);
                }
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end()), v.end());
                while ((int) v.size() < nv)                              // top up a lost voice
                {
                    const int t = chordToneUp(cp, v.empty() ? laneLo : v.back() + 3);
                    if (t <= -999) break;
                    v.push_back(t);
                }
                while ((int) v.size() > nv) v.pop_back();
            }
            std::sort(v.begin(), v.end());
            for (size_t i = 1; i < v.size(); ++i)
            {   // H1 low-interval limits: muddy close intervals rise an octave (or vanish)
                const bool tooLow  = v[i - 1] < -12 && v[i] - v[i - 1] <= 4;
                const bool tooLow2 = v[i - 1] < -5  && v[i] - v[i - 1] <= 2;
                if (tooLow || tooLow2)
                {
                    if (v[i] + 12 <= laneHi + 3) v[i] += 12;
                    else { v.erase(v.begin() + (long) i); --i; continue; }
                    std::sort(v.begin(), v.end());
                    v.erase(std::unique(v.begin(), v.end()), v.end());
                }
            }
            prevV = v;
            const int nextCol = oi + 1 < onsCols.size() ? onsCols[oi + 1] : total;
            int len = o.rhythm == RhFlowing ? std::max(CELL16, nextCol - col - CELL16 / 2)
                                            : std::min(std::max(CELL16, (nextCol - col) * 9 / 10), BEAT);
            len = std::min(len, total - col);
            const int vel = velFor(o, c, prng, col, latStrong(c, col, false));
            if (o.forceMono)
            {   // H9 degrade: the voicing ARPEGGIATED - successive stabs cycle its tones
                if (! v.empty())
                {
                    Note nn; nn.start = col; nn.len = std::min(len, BEAT / 2);
                    nn.semi = std::max(-48, std::min(48, v[oi % v.size()])); nn.vel = vel;
                    notes.push_back(nn);
                }
            }
            else
                for (int t : v)
                {
                    Note nn; nn.start = col; nn.len = len;
                    nn.semi = std::max(-48, std::min(48, t)); nn.vel = vel;
                    notes.push_back(nn);
                }
        }
    }
    else if (o.role == RoleBass && o.styleDna != nullptr && o.styleVirtual
             && ! o.styleDna->bass.empty())
    {
        // ============================================================================
        // [r22 STAGE 2] FROM-SCRATCH BASS = the STYLE'S CELL (house offbeat octaves, reggaeton
        // tresillo 3-3-2, funk ghosted 16ths, trap 808 sustains...): ONE seed-picked weighted
        // cell tiles every bar; degrees resolve against the chord-of-the-moment; durations come
        // from the tuples. Every later pass (G1/G9 grammar, LHL band, pockets, G10, H10) still
        // runs over it - the context keeps authority, the style supplies the vocabulary.
        // ============================================================================
        const auto& st2 = *o.styleDna;
        const GenStyle::BassCell* cell = pickWeighted(st2.bass, rrng);
        const int centre = registerCentre(o);
        std::vector<int> lad;
        buildLadder(o, centre - 10, centre + 10, lad);
        if (cell != nullptr && ! lad.empty())
        {
            auto nearestPc = [&](int wantPc, int ref)
            {
                int best = ref, bd = 1 << 20;
                for (int s2 : lad)
                    if (pc(s2) == wantPc && std::abs(s2 - ref) < bd) { bd = std::abs(s2 - ref); best = s2; }
                return best;
            };
            for (int b = 0; b < bars; ++b)
                for (const auto& ev : cell->ev)
                {
                    const int col = latSnap(c, b * COLS + ev.pos16 * CELL16);
                    if (col >= total) continue;
                    const int rootDeg = chordRootDegAt(o, c, col);
                    const int rootPc  = pc(o.key + o.scale[rootDeg]);
                    const int rootRef = nearestPc(rootPc, centre - 3);
                    int semi = rootRef;
                    bool approach = false;
                    switch (ev.code)
                    {
                        case GenStyle::DegRoot: break;
                        case GenStyle::DegThird:
                        case GenStyle::DegFifth:
                        case GenStyle::DegSeventh:
                            semi = nearestPc(pc(o.key + o.scale[(rootDeg + ev.code) % o.scaleLen]),
                                             rootRef + 3);
                            break;
                        case GenStyle::DegOct:       semi = nearestPc(rootPc, rootRef + 12); break;
                        case GenStyle::DegOctDown:   semi = nearestPc(rootPc, rootRef - 12); break;
                        case GenStyle::DegFifthDown:
                            semi = nearestPc(pc(o.key + o.scale[(rootDeg + 4) % o.scaleLen]),
                                             rootRef - 5);
                            break;
                        default: approach = true; break;   // DegApproach: the G9 pass re-aims it
                    }
                    Note nn;
                    nn.start = col;
                    nn.len   = std::max(latCw(c) / 2, std::min(ev.dur16 * CELL16 * 9 / 10, total - col));
                    nn.semi  = std::max(-48, std::min(48, semi));
                    nn.vel   = juceLikeClamp(120, 255, (int) std::lround((double) ev.vel / 127.0 * 255.0));
                    nn.approach = approach;
                    notes.push_back(nn);
                }
        }
        if (notes.empty())
        {   // an unusable cell: fall back to the generic sampler over the whole span
            std::vector<Cand> cands; std::vector<int> ons;
            phraseCandidates(o, c, 0, total, cands);
            sampleOnsets(o, c, rrng, cands, budgetFor(bars), 0, total, ons);
            for (int col : ons) notes.push_back({ col, 1, 0, 235 });
            makeLengths(o, c, notes, total);
            PitchState stFb;
            pitchPhrase(o, c, prng, notes, stFb, 0, total, true, -1, 0);
        }
    }
    else if (o.role == RoleMelody || o.role == RoleHum)
    {
        // ============================================================================
        // [r20 E] THE MOTIF ENGINE (M10-M12): one 3-5 note rhythm CELL is the part's identity.
        // Phrases place it at half-bar anchors; >= 60% of onsets are its transforms (repeat /
        // diatonic sequence / inversion / extension), a repeat varies only LATE material (never
        // the first two notes), echoes/answers reuse the cell verbatim at their opening, the
        // bridge gets a contrasting cell. Cadences per M11 (Q = 2/5/7, A = 1/3); prosody per
        // M12 (per-generation front/back-heavy start, phrase peak on the strongest onset).
        // ============================================================================
        planForm(bars, o.lines, o.relation, plan,
                 o.styleDna != nullptr ? (o.rhythmSeed ^ 0xF04D5EEDu) : 0u);   // [r22] form fan-out
        // [r21] the motif's onsets + spans live in LATTICE CELLS (indices, not columns): a
        // placement = anchor cell + relative cell, converted to a col only at the very end -
        // so every copy of the cell is on-lattice by construction (half-bar spans on an odd
        // lattice have no half cell: the span becomes the whole bar there).
        // [r22 STAGE 2] cats/slope: a STYLE-BORN cell carries its category string (C chord tone /
        // L color / A approach) and pitch slope - the cats retune the captured identity, the
        // slope picks the motif contour.
        struct MotifCell { std::vector<int> ons; std::vector<int> semis, vels; int spanCells = 8;
                           std::vector<char> cats; int slopeLo = 0, slopeHi = 0; bool styled = false; };
        MotifCell motif;                 // 'A's cell = the identity every echo keeps
        int motifCt = -1;
        PitchState st;
        const int nLat = latN(c);
        // M12 section choice: back-heavy starts are a MELODY device (hum stays front-anchored -
        // its LHL band [0,2] cannot afford an off-beat opening the ceiling pass may never move)
        const bool backHeavy = o.role == RoleMelody && rrng.chance(0.35f);
        const int  startOfs  = backHeavy ? 1 + rrng.ri(3) : 0;   // [r21] lattice CELLS now

        auto buildCell = [&](int s, int budget) -> MotifCell
        {
            MotifCell mc;
            // [r22 STAGE 2] FROM SCRATCH: the style's melody cells ARE the motif vocabulary -
            // a seed-picked weighted cell (bar-length span; positions are 16th vocabulary,
            // latSnapped; the virtual groove's lattice is 16 so they land exactly). Budget
            // trims from the TAIL (the opening is the identity). Real context keeps the
            // sampled path (the style only biases its candidate weights).
            if (o.styleDna != nullptr && o.styleVirtual && ! o.styleDna->mel.empty())
            {
                const GenStyle::MelCell* sc2 = pickWeighted(o.styleDna->mel, rrng);
                if (sc2 != nullptr && ! sc2->ev.empty())
                {
                    mc.styled = true;
                    mc.spanCells = nLat;
                    mc.slopeLo = sc2->slopeLo; mc.slopeHi = sc2->slopeHi;
                    const int sCell0 = latCellNear(c, s);
                    for (const auto& ev : sc2->ev)
                    {
                        const int rc = latCellNear(c, ev.pos16 * CELL16);
                        if (rc >= 0 && rc < mc.spanCells)
                        { mc.ons.push_back(rc); mc.cats.push_back(ev.cat); }
                    }
                    (void) sCell0;
                    const int keep = std::max(2, std::min((int) mc.ons.size(),
                                                          budget >= 3 ? std::min(6, budget) : 2));
                    while ((int) mc.ons.size() > keep) { mc.ons.pop_back(); mc.cats.pop_back(); }
                    if (! mc.ons.empty()) return mc;
                    mc = MotifCell();   // empty after trimming: fall through to the sampler
                }
            }
            mc.spanCells = (nLat % 2 == 0) ? nLat / 2 : nLat;
            const int sCell   = latCellNear(c, s);
            const int spanEnd = latColOf(c, sCell + mc.spanCells);
            std::vector<Cand> cands; std::vector<int> ons;
            phraseCandidates(o, c, s, spanEnd, cands);
            const int cellN = budget >= 3 ? std::min(5, 3 + (budget - 3) / 2) : 2;
            sampleOnsets(o, c, rrng, cands, cellN, s, spanEnd, ons);
            for (int col : ons)
            {
                const int rc = latCellNear(c, col) - sCell;
                if (rc >= 0 && rc < mc.spanCells) mc.ons.push_back(rc);
            }
            std::sort(mc.ons.begin(), mc.ons.end());
            mc.ons.erase(std::unique(mc.ons.begin(), mc.ons.end()), mc.ons.end());
            if (mc.ons.empty()) mc.ons.push_back(0);
            if (startOfs > 0 && mc.ons[0] == 0
                && (mc.ons.size() < 2 || startOfs < mc.ons[1])) mc.ons[0] = startOfs;   // back-heavy
            return mc;
        };
        auto ladderOf = [&]
        {
            const int centre = registerCentre(o);
            const int range  = o.singable ? 6 : 8;
            std::vector<int> lad; buildLadder(o, centre - range, centre + range, lad);
            if (lad.empty()) buildLadder(o, centre - 12, centre + 12, lad);
            return lad;
        };
        auto snapLad = [&](const std::vector<int>& lad, int want)
        {
            int best = want, bd = 1 << 20;
            for (int s2 : lad) if (std::abs(s2 - want) < bd) { bd = std::abs(s2 - want); best = s2; }
            return best;
        };
        auto ladShift = [&](const std::vector<int>& lad, int semi, int step)
        {
            if (lad.empty()) return semi;
            int idx = 0, bd = 1 << 20;
            for (int i2 = 0; i2 < (int) lad.size(); ++i2)
                if (std::abs(lad[(size_t) i2] - semi) < bd) { bd = std::abs(lad[(size_t) i2] - semi); idx = i2; }
            idx = std::max(0, std::min((int) lad.size() - 1, idx + step));
            return lad[(size_t) idx];
        };

        for (auto& ph : plan)
        {
            const int s = ph.startBar * COLS, e = (ph.startBar + ph.lenBars) * COLS;
            const int budget = budgetFor(ph.lenBars);
            const bool fresh = ph.tag == 'A' || ph.tag == 'B' || ph.tag == 'F' || motif.ons.empty();
            MotifCell local;
            if (ph.tag == 'A' && motif.ons.empty()) { motif = buildCell(s, budget); local = motif; }
            else if (fresh)                           local = buildCell(s, budget);   // bridge/free = own cell
            else                                      local = motif;                  // echoes reuse the identity
            const int cellN = (int) local.ons.size();
            if (cellN == 0) continue;
            const int sCell = latCellNear(c, s);
            const int phraseCells = std::max(1, (latCellNear(c, e) - sCell) / local.spanCells);
            const int nPlace = std::max(1, std::min(phraseCells, (budget + cellN - 1) / cellN));
            std::vector<int> anchors;
            for (int k = 0; k < phraseCells && (int) anchors.size() < nPlace; ++k) anchors.push_back(k);
            if ((int) anchors.size() >= 3 && rrng.chance(0.30f))            // a mid-phrase breath (M14)
                anchors.erase(anchors.begin() + 1 + rrng.ri((int) anchors.size() - 2));
            const bool varyLast = anchors.size() >= 2 && rrng.chance(0.4f); // M10: vary LATE, never the opening
            const int  varyDir  = rrng.chance(0.5f) ? 1 : -1;               // [r21] one lattice CELL
            const int  breathEnd = breathe ? latColOf(c, latCellNear(c, e) - 2) : e;
            const bool extend   = o.role == RoleMelody && rrng.chance(0.35f);   // hum stays spare

            struct PN { int col, k, ci; };            // placement k, cell-onset ci (-1 = extension)
            std::vector<PN> pns;
            for (int a2 = 0; a2 < (int) anchors.size(); ++a2)
            {
                const int baseCell = sCell + anchors[(size_t) a2] * local.spanCells;
                for (int ci = 0; ci < cellN; ++ci)
                {
                    int g = baseCell + local.ons[(size_t) ci];
                    if (varyLast && a2 == (int) anchors.size() - 1 && ci == cellN - 1 && ci >= 2)
                    { const int ng = g + varyDir;
                      const int nc = latColOf(c, ng);
                      if (nc > latColOf(c, baseCell) && nc < breathEnd) g = ng; }
                    const int col = latColOf(c, g);
                    if (col >= breathEnd || col >= e) continue;
                    pns.push_back({ col, a2, ci });
                }
                if (extend && a2 == (int) anchors.size() - 1 && cellN > 0)
                {   // EXTENSION transform: one extra tail onset two cells past the cell
                    const int col = latColOf(c, baseCell + local.ons.back() + 2);
                    if (col < breathEnd) pns.push_back({ col, a2, -1 });
                }
            }
            if (o.rhythm == RhDriving && c.nHits > 0)
                for (auto& pn : pns)
                {   // DRIVING's hit-exact promise outranks the anchor grid: every placement
                    // column SNAPS to the nearest real hit inside the phrase (GenTest [14])
                    int best = -1, bd = 1 << 20;
                    for (int h = 0; h < c.nHits; ++h)
                        if (c.hitCol[h] >= s && c.hitCol[h] < e
                            && std::abs(c.hitCol[h] - pn.col) < bd)
                        { bd = std::abs(c.hitCol[h] - pn.col); best = c.hitCol[h]; }
                    if (best >= 0) pn.col = best;
                }
            std::sort(pns.begin(), pns.end(), [](const PN& a2, const PN& b2) { return a2.col < b2.col; });
            pns.erase(std::unique(pns.begin(), pns.end(),
                                  [](const PN& a2, const PN& b2) { return a2.col == b2.col; }), pns.end());
            if (pns.empty()) continue;
            const size_t noteBase = notes.size();
            for (auto& pn : pns) { Note nn; nn.start = pn.col; notes.push_back(nn); }

            // contour: 'A' rolls it once, echoes reuse it, the bridge contrasts it
            // [r22 STAGE 2] a style-born cell's SLOPE picks the contour (up = rising, down =
            // falling, straddling zero = arch) - the cell's drift is part of the vocabulary
            if (ph.tag == 'A' && motifCt < 0 && local.styled)
                motifCt = local.slopeLo >= 0 && local.slopeHi > 0 ? CtRising
                        : local.slopeHi <= 0 && local.slopeLo < 0 ? CtFalling : CtArch;
            if (ph.tag == 'A' && motifCt < 0) motifCt = o.contour != CtAuto ? o.contour : 1 + prng.ri(4);
            const int ctOv = ph.tag == 'B' ? (motifCt == CtRising ? CtFalling : CtRising) : motifCt;
            auto lad = ladderOf();
            {   // the ladder walk pitches EVERY note (velocities included); transforms overwrite below
                std::vector<Note> tmp(notes.begin() + (long) noteBase, notes.end());
                PitchState st2 = st;
                pitchPhrase(o, c, prng, tmp, st2, s, e, ph.resolved, ctOv, ph.tag == 'B' ? 3 : 0);
                for (size_t i = noteBase; i < notes.size(); ++i) notes[i] = tmp[i - noteBase];
                st = st2;
            }
            if (fresh)
            {   // [r20 M1-AT-BIRTH] the cell is the IDENTITY - born stepwise-leaning (one skip
                // <= a 3rd licensed), so every repeat/echo inherits proximity instead of the
                // floor fighting locked copies later. Strong cols still prefer chord-tone steps.
                bool skipUsed = false;
                for (size_t i = noteBase + 1; i < notes.size(); ++i)
                {
                    const int d = notes[i].semi - notes[i - 1].semi;
                    if (std::abs(d) <= 2) continue;
                    if (! skipUsed && std::abs(d) <= 4) { skipUsed = true; continue; }
                    const int dir2 = d > 0 ? 1 : -1;
                    const bool strong2 = latStrong(c, notes[i].start, false);   // [r21] lattice beats
                    bool cp2[12] = {};
                    if (strong2) chordPcs(o, chordRootDegAt(o, c, notes[i].start), cp2);
                    int best2 = notes[i].semi, bd2 = 1 << 20; bool bc2 = false;
                    for (int s2 : lad)
                    {
                        const int dd = (s2 - notes[i - 1].semi) * dir2;
                        if (dd < 1 || dd > 2) continue;
                        const bool ct2 = strong2 && cp2[pc(s2)];
                        const int dist = std::abs(s2 - notes[i].semi);
                        if ((ct2 && ! bc2) || (ct2 == bc2 && dist < bd2)) { bd2 = dist; best2 = s2; bc2 = ct2; }
                    }
                    if (best2 != notes[i].semi && std::abs(best2 - notes[i - 1].semi) <= 2)
                        notes[i].semi = best2;
                }
            }
            // [r22 STAGE 2] the style cell's CATEGORY string retunes placement 0 BEFORE the
            // capture (the identity carries it into every echo/transform): C = snap to the
            // chord-of-the-moment's nearest tone, A = a scale-step approach INTO the next
            // note (processed in reverse so "next" is final), L = the walked color tone stays.
            if (fresh && local.styled && ! local.cats.empty())
            {
                for (int i = (int) notes.size() - 1; i >= (int) noteBase; --i)
                {
                    const auto& pn = pns[(size_t) i - noteBase];
                    if (pn.k != 0 || pn.ci < 0 || pn.ci >= (int) local.cats.size()) continue;
                    const char cat = local.cats[(size_t) pn.ci];
                    if (cat == 'C')
                    {
                        bool cp2[12]; chordPcs(o, chordRootDegAt(o, c, notes[(size_t) i].start), cp2);
                        if (! cp2[pc(notes[(size_t) i].semi)])
                        {
                            int best = notes[(size_t) i].semi, bd = 1 << 20;
                            for (int s2 : lad)
                                if (cp2[pc(s2)] && std::abs(s2 - notes[(size_t) i].semi) < bd)
                                { bd = std::abs(s2 - notes[(size_t) i].semi); best = s2; }
                            notes[(size_t) i].semi = best;
                        }
                    }
                    else if (cat == 'A' && (size_t) i + 1 < notes.size())
                    {
                        const int nxt = notes[(size_t) i + 1].semi;
                        const int dir = notes[(size_t) i].semi <= nxt ? -1 : 1;   // step INTO it
                        notes[(size_t) i].semi = ladStepOf(lad, nxt, dir);
                    }
                }
            }
            if (ph.tag == 'A' && motif.semis.empty())
            {   // capture the identity: placement 0's pitches + velocities. These notes are CORE
                // too - the original must keep matching the echoes that copy it.
                motif.semis.assign((size_t) cellN, 999); motif.vels.assign((size_t) cellN, 220);
                for (size_t i = noteBase; i < notes.size(); ++i)
                {
                    const auto& pn = pns[i - noteBase];
                    if (pn.k == 0 && pn.ci >= 0 && pn.ci < cellN)
                    {
                        motif.semis[(size_t) pn.ci] = notes[i].semi;
                        motif.vels[(size_t) pn.ci]  = notes[i].vel;
                        notes[i].core = pn.ci <= 1;   // M10 verbatim: the FIRST TWO notes
                    }
                }
                for (auto& sm : motif.semis) if (sm == 999) sm = registerCentre(o);
            }
            if (! fresh && ! motif.semis.empty())
                for (size_t i = noteBase; i < notes.size(); ++i)
                {   // echo/answer OPENING = the identity verbatim (M11's shared opening) - these
                    // notes are CORE: the cross-phrase recognition later passes must not rewrite
                    const auto& pn = pns[i - noteBase];
                    if (pn.k == 0 && pn.ci >= 0 && pn.ci < (int) motif.semis.size())
                    {
                        notes[i].semi = motif.semis[(size_t) pn.ci];
                        notes[i].vel  = motif.vels[(size_t) pn.ci];
                        notes[i].core = pn.ci <= 1;   // M10 verbatim: the FIRST TWO notes
                    }
                }
            {   // M10 TRANSFORMS on placements 1+: repeat / diatonic sequence / inversion / free
                std::vector<int> ref((size_t) cellN, 999);
                for (size_t i = noteBase; i < notes.size(); ++i)
                {
                    const auto& pn = pns[i - noteBase];
                    if (pn.k == 0 && pn.ci >= 0) ref[(size_t) pn.ci] = notes[i].semi;
                }
                bool haveRef = true;
                for (int ci = 0; ci < cellN; ++ci) if (ref[(size_t) ci] == 999) haveRef = false;
                const int seqDir = motifCt == CtFalling ? -1 : 1;
                if (haveRef)
                    for (int k = 1; k < (int) anchors.size(); ++k)
                    {
                        const float r2 = prng.uf();
                        const int mode = r2 < 0.5f ? 0 : (r2 < 0.75f ? 1 : (r2 < 0.85f ? 2 : 3));
                        for (size_t i = noteBase; i < notes.size(); ++i)
                        {
                            const auto& pn = pns[i - noteBase];
                            if (pn.k != k || pn.ci < 0) continue;
                            if (mode == 3) continue;               // free: keep the walked pitch
                            int semi = ref[(size_t) pn.ci];
                            if      (mode == 1) semi = ladShift(lad, semi, seqDir);          // sequence
                            else if (mode == 2) semi = snapLad(lad, 2 * ref[0] - semi);      // inversion
                            if (pn.ci >= 2)
                            {   // M7 correction is licensed only PAST the first two notes (M10)
                                const bool strong = latStrong(c, notes[i].start, true);
                                if (strong)
                                {
                                    bool cp[12]; chordPcs(o, chordRootDegAt(o, c, notes[i].start), cp);
                                    if (! cp[pc(semi)])
                                    {
                                        int best = semi, bd = 1 << 20;
                                        for (int s2 : lad)
                                            if (cp[pc(s2)] && std::abs(s2 - semi) < bd)
                                            { bd = std::abs(s2 - semi); best = s2; }
                                        semi = best;
                                    }
                                }
                            }
                            notes[i].semi = std::max(-48, std::min(48, semi));
                        }
                    }
            }
            // [r20 M1 SEAMS] a within-phrase placement whose SEAM leaps gets TRANSPOSED whole
            // (a true sequence): the cell's interval identity survives, the join becomes a step.
            for (int k = 1; k < (int) anchors.size(); ++k)
            {
                size_t first = (size_t) -1, prev = (size_t) -1;
                for (size_t i = noteBase; i < notes.size(); ++i)
                {
                    const auto& pn = pns[i - noteBase];
                    if (pn.k == k && first == (size_t) -1) first = i;
                    if (pn.k < k) prev = i;
                }
                if (first == (size_t) -1 || prev == (size_t) -1) continue;
                const int seam = notes[first].semi - notes[prev].semi;
                if (std::abs(seam) <= 4) continue;
                const int shift = seam > 0 ? -(std::abs(seam) - 2) : (std::abs(seam) - 2);
                for (size_t i = noteBase; i < notes.size(); ++i)
                    if (pns[i - noteBase].k == k && ! notes[i].core)
                        notes[i].semi = snapLad(lad, notes[i].semi + shift);
            }
            if (notes.size() > noteBase)
            {   // M11 cadence degrees - the cadence pitch is LAW: core, so no later pass undoes it
                snapCadenceNote(o, lad, notes.back(), ph.resolved);
                notes.back().core = true;
            }
            {   // M12 prosody: the phrase PEAK moves onto the strongest onset (pitch swap only;
                // [r21] strength = the lattice's combined metric + hit map - the G7 hierarchy)
                size_t peak = noteBase, strongI = noteBase; float bestW = -1.0f;
                auto strengthAt = [&](int col) { return latStrAt(c, col); };
                for (size_t i = noteBase; i < notes.size(); ++i)
                {
                    if (notes[i].semi > notes[peak].semi) peak = i;
                    const float w = strengthAt(notes[i].start);
                    if (w > bestW) { bestW = w; strongI = i; }
                }
                if (peak != strongI && bestW >= 0.6f && strengthAt(notes[peak].start) < 0.6f
                    && peak + 1 < notes.size() && strongI + 1 < notes.size()    // never the cadence note
                    && ! notes[peak].core && ! notes[strongI].core)             // never the identity
                    std::swap(notes[peak].semi, notes[strongI].semi);
            }
        }
        makeLengths(o, c, notes, total);
        {   // M14: <= ~12 onsets per bar-pair - the quietest non-opening notes yield
            for (int w = 0; w * 2 * COLS < total; ++w)
                for (;;)
                {
                    int cnt = 0, wv = 999; size_t weakest = (size_t) -1;
                    for (size_t i = 0; i < notes.size(); ++i)
                        if (notes[i].start >= w * 2 * COLS && notes[i].start < (w + 1) * 2 * COLS)
                        {
                            ++cnt;
                            if ((notes[i].start % COLS) != 0 && notes[i].vel < wv)
                            { wv = notes[i].vel; weakest = i; }
                        }
                    if (cnt <= 12 || weakest == (size_t) -1) break;
                    notes.erase(notes.begin() + (long) weakest);
                }
            makeLengths(o, c, notes, total);
        }
        // breath caps: a vocal/melody phrase ends a LATTICE CELL before the next one begins (M14)
        if (breathe)
            for (auto& ph : plan)
            {
                const int e = (ph.startBar + ph.lenBars) * COLS;
                const int air = latColOf(c, latCellNear(c, e) - 1);
                for (auto& n : notes)
                    if (n.start < e && n.start + n.len > air)
                        n.len = std::max(latCw(c) / 2, air - n.start);
            }
    }
    else
    {
        planForm(bars, o.lines, o.relation, plan);
        // the MOTIF = phrase A's material (onsets + pitches), the identity every echo/answer keeps
        std::vector<int>  motifOns;    // bar-local (relative to its phrase start)
        std::vector<Note> motifNotes;  // pitched A notes (phrase-relative starts)
        int motifCt = -1;              // A's rolled contour (echoes reuse it; B contrasts it)
        PitchState st;

        for (auto& ph : plan)
        {
            const int s = ph.startBar * COLS, e = (ph.startBar + ph.lenBars) * COLS;
            const int breathEnd = breathe ? latColOf(c, latCellNear(c, e) - 2) : e;
            std::vector<int> ons;

            if ((ph.tag == 'a' || ph.tag == 'n') && ! motifOns.empty())
            {
                // keep the OPENING (the recognizable part): echoes keep ~70%, answers ~40%
                // (motifOns are BAR-RELATIVE col offsets; phrases start on bar lines, so a
                // re-anchored offset stays on the lattice - each bar's cells repeat exactly)
                const float keep = ph.tag == 'a' ? 0.7f : 0.4f;
                const int span = ph.lenBars * COLS;
                for (int col : motifOns)
                    if (col < (int) ((float) span * keep)) ons.push_back(s + col);
                // fresh tail rhythm from the remaining span (rhythm stream only)
                std::vector<Cand> cands;
                phraseCandidates(o, c, s + (int) ((float) span * keep), e, cands);
                std::vector<int> tail;
                const int want = std::max(1, budgetFor(ph.lenBars) - (int) ons.size());
                sampleOnsets(o, c, rrng, cands, want, s + (int) ((float) span * keep), breathEnd, tail);
                for (int col : tail) ons.push_back(col);
                // ECHO RHYTHM VARIATION [v2]: nudge the last kept onset sometimes - a bit-identical
                // rhythm reads as copy-paste (the user's complaint); the opening itself never moves
                if (ph.tag == 'a' && ons.size() >= 3 && rrng.chance(0.4f))
                {
                    std::sort(ons.begin(), ons.end());
                    int& mv = ons[ons.size() - 2];   // [r21] one lattice CELL either way
                    const int shifted = latColOf(c, latCellNear(c, mv) + (rrng.chance(0.5f) ? 1 : -1));
                    if (shifted > s && shifted < breathEnd) mv = shifted;
                }
            }
            else
            {
                std::vector<Cand> cands;
                phraseCandidates(o, c, s, e, cands);
                sampleOnsets(o, c, rrng, cands, budgetFor(ph.lenBars), s, breathEnd, ons);
            }
            std::sort(ons.begin(), ons.end());
            ons.erase(std::unique(ons.begin(), ons.end()), ons.end());

            const size_t noteBase = notes.size();
            for (int col : ons) notes.push_back({ col, 1, 0, 235 });

            // pitches: echoes/answers REUSE the motif's opening pitches (identity), tails walk fresh
            int reused = 0;
            if ((ph.tag == 'a' || ph.tag == 'n') && ! motifNotes.empty())
            {
                const float keep = ph.tag == 'a' ? 0.7f : 0.4f;
                for (size_t i = noteBase; i < notes.size(); ++i)
                {
                    const int rel = notes[i].start - s;
                    for (auto& m : motifNotes)
                        if (m.start == rel && rel < (int) ((float) (ph.lenBars * COLS) * keep))
                        { notes[i].semi = m.semi; notes[i].vel = m.vel; ++reused; break; }
                }
            }
            const int ctOv   = ph.tag == 'B' ? (motifCt == CtRising ? CtFalling : CtRising)
                             : (ph.tag == 'a' || ph.tag == 'n') ? motifCt : -1;
            const int idealS = ph.tag == 'B' ? 3 : 0;    // the bridge sits a step higher (contrast)
            // roll A's contour ONCE (stable across its echoes)
            if (ph.tag == 'A' && motifCt < 0)
            { motifCt = o.contour != CtAuto ? o.contour : 1 + prng.ri(4); }
            {
                // pitch only the notes the motif didn't already supply: temporarily mark reused ones
                std::vector<Note> tmp;
                for (size_t i = noteBase; i < notes.size(); ++i) tmp.push_back(notes[i]);
                PitchState st2 = st;
                pitchPhrase(o, c, prng, tmp, st2,  s, e, ph.resolved,
                            ph.tag == 'A' ? motifCt : ctOv, idealS);
                for (size_t i = noteBase, j = 0; i < notes.size(); ++i, ++j)
                {
                    const int rel = notes[i].start - s;
                    const bool keptPitch = (ph.tag == 'a' || ph.tag == 'n')
                        && rel < (int) ((float) (ph.lenBars * COLS) * (ph.tag == 'a' ? 0.7f : 0.4f))
                        && [&]{ for (auto& m : motifNotes) if (m.start == rel) return true; return false; }();
                    if (! keptPitch || &notes[i] == &notes.back()) notes[i] = tmp[j];   // cadence wins on the final note
                }
                st = st2;
            }
            (void) reused;
            if (ph.tag == 'A' && motifOns.empty())
            {   // capture the motif for the echoes/answers
                for (int col : ons) motifOns.push_back(col - s);
                for (size_t i = noteBase; i < notes.size(); ++i)
                { Note m = notes[i]; m.start -= s; motifNotes.push_back(m); }
            }
        }
        makeLengths(o, c, notes, total);
        // breath caps: a vocal phrase ends a LATTICE CELL before the next one begins
        if (breathe)
            for (auto& ph : plan)
            {
                const int e = (ph.startBar + ph.lenBars) * COLS;
                const int air = latColOf(c, latCellNear(c, e) - 1);
                for (auto& n : notes)
                    if (n.start < e && n.start + n.len > air)
                        n.len = std::max(latCw(c) / 2, air - n.start);
            }
    }

    // [P1] FILLS: an end-of-line density bump (calibration: fill bars run ~1.1-1.5x the groove -
    // docs/generate-calibration.md fill mult). Extra in-scale onsets in the last quarter of the
    // chosen bars, pitched stepwise from the nearest existing note. Seed-deterministic.
    if (o.fills > 0 && ! notes.empty() && o.role != RoleChords)
    {
        Rng fr(o.rhythmSeed ^ 0xF111C0DEu);
        bool fillBar[MAX_BARS] = {};
        if (o.fills == 1 || plan.empty()) fillBar[bars - 1] = true;   // Last bar (Riff always here)
        else for (auto& ph : plan)
            fillBar[std::max(0, std::min(bars - 1, ph.startBar + ph.lenBars - 1))] = true;
        for (int b = 0; b < bars; ++b)
        {
            if (! fillBar[b]) continue;
            const int fs = b * COLS + 3 * BEAT, fe = (b + 1) * COLS;
            std::vector<Cand> cands; std::vector<int> ons;
            phraseCandidates(o, c, fs, fe, cands);
            sampleOnsets(o, c, fr, cands, std::max(1, budgetFor(1) / 2), fs, fe, ons);
            for (int col : ons)
            {
                bool dup = false;
                for (auto& n : notes) if (n.start == col) { dup = true; break; }
                if (dup) continue;
                int ref = 0, bd = 1 << 20;   // pitch: a scale step off the nearest existing note
                for (auto& n : notes)
                    if (std::abs(n.start - col) < bd) { bd = std::abs(n.start - col); ref = n.semi; }
                int semi = ref;
                std::vector<int> lad; buildLadder(o, ref - 4, ref + 4, lad);
                if (! lad.empty())
                {
                    int ni = 0, nd = 1 << 20;
                    for (int i = 0; i < (int) lad.size(); ++i)
                        if (std::abs(lad[i] - ref) < nd) { nd = std::abs(lad[i] - ref); ni = i; }
                    semi = lad[std::max(0, std::min((int) lad.size() - 1, ni + (fr.chance(0.5f) ? 1 : -1)))];
                }
                notes.push_back({ col, 1, semi, 225 });
            }
        }
        makeLengths(o, c, notes, total);
    }

    // VARY: same skeleton, new ornaments (Chords reroll their template dice instead - the
    // comping Rng folds varyCount in; a per-note pitch mutation would break the voicings)
    if (o.varyCount > 0 && o.role != RoleChords)
    {
        Rng vr(0xC0FFEEu ^ (o.pitchSeed + 0x9e3779b9u * (uint32_t) o.varyCount));
        for (auto& n : notes)
            if (vr.chance(0.3f))
            {
                std::vector<int> lad;
                buildLadder(o, n.semi - 5, n.semi + 5, lad);
                if (! lad.empty()) n.semi = lad[vr.ri((int) lad.size())];
            }
    }

    // SINGABLE enforcement (whatever the echoes/vary did): one-octave ladder, leaps <= a fifth
    if (o.singable && ! notes.empty() && o.role != RoleChords)
    {
        const int centre = registerCentre(o);
        std::vector<int> lad;
        buildLadder(o, centre - 6, centre + 6, lad);
        if (! lad.empty())
        {
            int prev = 999;
            for (auto& n : notes)
            {
                int best = lad[0]; int bd = 1 << 20;
                for (int s2 : lad)
                {
                    if (prev != 999 && std::abs(s2 - prev) > 7) continue;
                    const int d = std::abs(s2 - n.semi);
                    if (d < bd) { bd = d; best = s2; }
                }
                n.semi = best; prev = best;
            }
        }
    }

    auto byStart = [](const Note& a, const Note& b) { return a.start < b.start; };

    // [P1 G9 + G1] BASS GRAMMAR: the change-downbeat plays the ROOT; the last onset in the beat
    // BEFORE a change becomes an APPROACH note walking into the next root (scale-step at Safe,
    // chromatic from Spicy up - flagged for the step writer's Slide band). G1: when a kick holds
    // the One and the bass missed it, the first note moves onto it (non-Flowing stances).
    if (o.role == RoleBass && ! notes.empty() && c.chordColsValid)
    {
        std::sort(notes.begin(), notes.end(), byStart);
        const int centre = registerCentre(o);
        const int range  = o.singable ? 6 : 8;   // pitchPhrase's own register window - stay in it
        std::vector<int> lad;
        buildLadder(o, centre - range, centre + range, lad);
        auto rootSemi = [&](int deg, int ref)
        {
            const int want = pc(o.key + o.scale[deg % o.scaleLen]);
            int best = ref, bd = 1 << 20;
            for (int s2 : lad)
                if (pc(s2) == want && std::abs(s2 - ref) < bd) { bd = std::abs(s2 - ref); best = s2; }
            return best;
        };
        if (o.rhythm != RhFlowing && c.nKick > 0 && c.kickCol[0] == 0 && notes.front().start != 0)
            notes.front().start = 0;
        for (int ci = 0; ci < c.nChords && ! lad.empty(); ++ci)
        {
            const int cc = c.chordColAt[ci];
            Note* atC = nullptr;                        // root ON the change (first onset in a beat)
            for (auto& n : notes)
                if (n.start >= cc && n.start < cc + BEAT) { atC = &n; break; }
            if (atC != nullptr) atC->semi = rootSemi(c.chordDegAt[ci], atC->semi);
            if (ci == 0) continue;                      // nothing approaches the very first chord
            Note* ap = nullptr;                         // approach INTO the change (last onset before)
            for (auto& n : notes)
                if (n.start >= cc - BEAT && n.start < cc) ap = &n;
            if (ap == nullptr || ap == atC) continue;
            const int prevCc = c.chordColAt[ci - 1];
            if (ap->start >= prevCc && ap->start < prevCc + BEAT) continue;   // it IS the previous root
            const int nextRoot = rootSemi(c.chordDegAt[ci], ap->semi);
            if (o.color >= 1)
                ap->semi = nextRoot + (ap->semi >= nextRoot ? 1 : -1);   // chromatic slide-in
            else
            {   // scale-step neighbour of the next root, on the side the line already sits
                int best = nextRoot, bd = 1 << 20;
                for (int s2 : lad)
                {
                    if (s2 == nextRoot || std::abs(s2 - nextRoot) > 2) continue;
                    if (std::abs(s2 - ap->semi) < bd) { bd = std::abs(s2 - ap->semi); best = s2; }
                }
                ap->semi = best;
            }
            ap->approach = true;
        }
        for (auto& n : notes) n.semi = std::max(-48, std::min(48, n.semi));
    }

    // [r20 E, G4 + M13] ANTICIPATION at chord changes: the change note may arrive ONE CELL EARLY
    // carrying the NEW chord's tone (it already does - chordRootDegAt named it at the change col),
    // with the stance's push probability; NEVER two consecutive changes pushed. Deterministic
    // from the rhythm seed, dice consumed at EVERY change so the shape is seed-stable.
    if ((o.role == RoleBass || o.role == RoleMelody) && c.chordColsValid && c.nChords > 1
        && ! notes.empty())
    {
        std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
        const float pushP = o.rhythm == RhDriving ? 0.35f : (o.rhythm == RhPockets ? 0.20f : 0.12f);
        const int cw = latCw(c);
        Rng gr(o.rhythmSeed ^ 0xA17C1BA7u);
        bool prevPushed = false;
        for (int ci = 1; ci < c.nChords; ++ci)
        {
            const int cc2 = c.chordColAt[ci];
            const bool roll2 = gr.chance(pushP);        // consume EVERY change (deterministic)
            Note* at = nullptr;
            for (auto& n : notes) if (n.start >= cc2 && n.start < cc2 + cw) { at = &n; break; }
            if (at == nullptr || prevPushed || ! roll2) { prevPushed = false; continue; }
            const int tgt = latColOf(c, latCellNear(c, cc2) - 1);   // [r21] one lattice CELL early
            bool free2 = tgt > 0 && tgt < cc2;
            for (auto& n : notes) if (&n != at && std::abs(n.start - tgt) < cw) free2 = false;
            if (o.rhythm == RhPockets)                  // the push respects the pocket exclusions
                for (int h = 0; h < c.nHits; ++h)
                    if (std::abs(tgt - c.hitCol[h]) < cw) free2 = false;
            if (! free2) { prevPushed = false; continue; }
            at->start = tgt;
            prevPushed = true;
        }
    }

    // [r20 E, M7 GUARD] the prosody swap / transforms / pushes may have dragged strong-beat
    // notes off their chord - re-snap every NON-CORE strong note to the nearest chord tone
    // within a 3rd BEFORE the proximity/leap passes (they smooth whatever the snap disturbs;
    // their floor below also PREFERS chord tones on strong cols so the snap survives them).
    if ((o.role == RoleMelody || o.role == RoleHum) && ! notes.empty())
    {
        const int centreG = registerCentre(o);
        std::vector<int> ladG;
        buildLadder(o, centreG - (o.singable ? 6 : 8) - 2, centreG + (o.singable ? 6 : 8) + 2, ladG);
        for (auto& n : notes)
        {
            if (n.core || ! latStrong(c, n.start, false)) continue;   // [r21] lattice beats
            bool cp[12]; chordPcs(o, chordRootDegAt(o, c, n.start), cp);
            if (cp[pc(n.semi)]) continue;
            int best = n.semi, bd = 4;                     // within a 3rd, else leave it (an NCT)
            for (int s2 : ladG)
                if (cp[pc(s2)] && std::abs(s2 - n.semi) < bd) { bd = std::abs(s2 - n.semi); best = s2; }
            n.semi = std::max(-48, std::min(48, best));
        }
    }

    // ============================================================================
    // [r22 STAGE 4a] EXPRESSIVE SHAPE (M3 / M4 / M6) - BEFORE the M1/M2 floor, which then
    // smooths whatever these passes disturb (shape first, polish after - the M7-guard order).
    // ============================================================================
    if ((o.role == RoleMelody || o.role == RoleHum) && notes.size() >= 3)
    {
        std::sort(notes.begin(), notes.end(), byStart);
        const int centreS = registerCentre(o);
        std::vector<int> ladS;
        buildLadder(o, centreS - (o.singable ? 6 : 8), centreS + (o.singable ? 6 : 8), ladS);
        const int L = o.scaleLen;
        // ---- M3: a same-direction DOUBLE LEAP is licensed only on a chord outline ----
        for (size_t i = 2; i < notes.size(); ++i)
        {
            const int d1 = notes[i - 1].semi - notes[i - 2].semi;
            const int d2 = notes[i].semi - notes[i - 1].semi;
            if (! ((d1 > 2 && d2 > 2) || (d1 < -2 && d2 < -2))) continue;
            bool cp[12]; chordPcs(o, chordRootDegAt(o, c, notes[i - 1].start), cp);
            if (cp[pc(notes[i - 2].semi)] && cp[pc(notes[i - 1].semi)] && cp[pc(notes[i].semi)])
                continue;                              // an arpeggiated chord outline - licensed
            if (notes[i].core) continue;
            notes[i].semi = ladStepOf(ladS, notes[i - 1].semi, d2 > 0 ? 1 : -1);   // shrink to a step
        }
        // ---- M4: TESSITURA pull - never two consecutive notes parked on a register edge ----
        if ((int) ladS.size() >= 5)
        {
            const int lo = ladS.front(), hi = ladS.back();
            const int edge = std::max(1, (hi - lo) / 5);   // the outer ~20% of the ladder
            auto isEdge = [&](int s2) { return s2 <= lo + edge || s2 >= hi - edge; };
            for (size_t i = 1; i < notes.size(); ++i)
            {
                if (! isEdge(notes[i - 1].semi) || ! isEdge(notes[i].semi) || notes[i].core) continue;
                const int dir = notes[i].semi >= hi - edge ? -1 : 1;   // toward the median
                for (int guard = 0; guard < 4 && isEdge(notes[i].semi); ++guard)
                    notes[i].semi = ladStepOf(ladS, notes[i].semi, dir);
            }
        }
        // ---- M6: CONTOUR template scoring - an arch's peak belongs near the 2/3 point ----
        for (auto& ph : plan)
        {
            const int ps = ph.startBar * COLS, pe = (ph.startBar + ph.lenBars) * COLS;
            size_t first = notes.size(), last = 0, peak = notes.size();
            int cnt = 0;
            for (size_t i = 0; i < notes.size(); ++i)
            {
                if (notes[i].start < ps || notes[i].start >= pe) continue;
                if (first == notes.size()) first = i;
                last = i; ++cnt;
                if (peak == notes.size() || notes[i].semi > notes[peak].semi) peak = i;
            }
            if (cnt < 4 || first == notes.size() || peak == notes.size()) continue;
            const float peakPos = (float) (notes[peak].start - ps) / (float) (pe - ps);
            const bool descentShape = notes[first].semi >= notes[peak].semi - 2;   // starts on top
            if (peakPos < 0.3f && ! descentShape && ! notes[peak].core)
            {   // an early parked peak that is NOT a descent: swap it toward the 65% point
                const int want = ps + (int) ((float) (pe - ps) * 0.65f);
                size_t tgt = notes.size(); int bd = 1 << 20;
                for (size_t i = first; i <= last; ++i)
                {
                    if (notes[i].core || i + 1 > last) continue;
                    const int d = std::abs(notes[i].start - want);
                    if (d < bd) { bd = d; tgt = i; }
                }
                if (tgt < notes.size() && tgt != peak)
                    std::swap(notes[peak].semi, notes[tgt].semi);
            }
            // the FINAL phrase prefers a stepwise DESCENT onto its cadence tone (M6's ending
            // rule; M9's leading-tone case deliberately overrides it upward later)
            if (&ph == &plan.back() && last > first && ! notes[last - 1].core)
            {
                const int cad = notes[last].semi;
                const int lead = pc(o.key + o.scale[(L - 1) % L]);
                if (pc(notes[last - 1].semi) != lead && notes[last - 1].semi <= cad)
                    notes[last - 1].semi = ladStepOf(ladS, cad, 1);   // one step ABOVE = falls home
            }
        }
    }

    // [P1 M1 + M2] PROXIMITY + LEAP RECOVERY (the vocal-craft rules, melody/hum): a leap > 5 st
    // must be answered by an opposite-direction step; then the line is pulled toward >= 70%
    // stepwise motion (the widest remaining skip shrinks to a step until the floor holds).
    if ((o.role == RoleMelody || o.role == RoleHum) && notes.size() >= 3)
    {
        std::sort(notes.begin(), notes.end(), byStart);
        const int centre = registerCentre(o);
        const int range  = o.singable ? 6 : 8;
        std::vector<int> lad;
        buildLadder(o, centre - range, centre + range, lad);
        auto recoverLeaps = [&]
        {
            for (size_t i = 1; i + 1 < notes.size(); ++i)
            {
                const int d = notes[i].semi - notes[i - 1].semi;
                if (std::abs(d) <= 5) continue;
                const int nd = notes[i + 1].semi - notes[i].semi;
                if (nd != 0 && (d > 0 ? nd < 0 : nd > 0) && std::abs(nd) <= 3) continue;
                const int dir = d > 0 ? -1 : 1;         // step back INTO the leap's hole
                int best = notes[i].semi, bd = 1 << 20;
                for (int s2 : lad)
                {
                    const int dd = (s2 - notes[i].semi) * dir;
                    if (dd < 1 || dd > 3) continue;     // a scale step (pentatonic gaps reach 3)
                    if (dd < bd) { bd = dd; best = s2; }
                }
                if (best != notes[i].semi) notes[i + 1].semi = best;
            }
        };
        recoverLeaps();
        for (int iter = 0; iter < 16; ++iter)           // M1: the 70% stepwise floor
        {
            int steps = 0, tot = 0, worst = -1, worstD = 0;
            for (size_t i = 1; i < notes.size(); ++i)
            {
                const int d = std::abs(notes[i].semi - notes[i - 1].semi);
                ++tot; if (d <= 2) ++steps;
                // [r20] a skip is fixable from EITHER end, but the motif identity (core) never
                // rewrites - a skip locked between two core notes is licensed and stays
                if (d > 2 && d > worstD && (! notes[i].core || ! notes[i - 1].core))
                { worstD = d; worst = (int) i; }
            }
            if (tot == 0 || worst < 0 || (float) steps / (float) tot >= 0.7f) break;
            const size_t tgtI  = ! notes[(size_t) worst].core ? (size_t) worst : (size_t) worst - 1;
            const int   anchor = tgtI == (size_t) worst ? notes[(size_t) worst - 1].semi
                                                        : notes[(size_t) worst].semi;
            const int cur = notes[tgtI].semi;
            const int dir = cur > anchor ? 1 : -1;
            // [r20] a STRONG-col note prefers a CHORD-TONE step (M7 survives the floor)
            const bool strongW = latStrong(c, notes[tgtI].start, false);
            bool cpW[12] = {};
            if (strongW) chordPcs(o, chordRootDegAt(o, c, notes[tgtI].start), cpW);
            int best = anchor, bd = 1 << 20; bool bestCt = false;
            for (int s2 : lad)
            {
                const int dd = (s2 - anchor) * dir;
                if (dd < 1 || dd > 2) continue;
                const bool ct = strongW && cpW[pc(s2)];
                const int d2 = std::abs(s2 - cur);
                if ((ct && ! bestCt) || (ct == bestCt && d2 < bd))
                { bd = d2; best = s2; bestCt = ct; }
            }
            if (best == anchor || best == cur) break;
            notes[tgtI].semi = best;
        }
        recoverLeaps();   // the floor pass may have re-shaped a recovery - re-guarantee M2
    }

    // ============================================================================
    // [r22 STAGE 4b] LICENSED DISSONANCE + CADENTIAL REFINEMENT (M8 / M9), AFTER the M1/M2
    // floor: the appoggiatura's leap is a LICENSED violation (both its notes go core so no
    // later pass smooths it away), and the tendency-tone resolutions refine the cadences the
    // earlier passes landed. The SHAPE passes (M3/M4/M6) run BEFORE the floor - see above.
    // Strong-col checks ride the lattice (latStrong) - no 16-grid constants (the r21 rule).
    // ============================================================================
    if ((o.role == RoleMelody || o.role == RoleHum) && notes.size() >= 3)
    {
        std::sort(notes.begin(), notes.end(), byStart);
        const int centreF = registerCentre(o);
        const int rangeF  = o.singable ? 6 : 8;
        std::vector<int> ladF;
        buildLadder(o, centreF - rangeF, centreF + rangeF, ladF);
        const int L = o.scaleLen;
        // ---- M8: APPOGGIATURA (the licensed violation) - Color-gated, <= 1 per phrase ----
        // A strong-beat chord tone lifts ONE scale step (a non-chord dissonance), leapt into,
        // and the NEXT note takes the abandoned chord tone = the classic resolve-down-by-step.
        if (o.color > 0 && ! plan.empty() && ! ladF.empty())
        {
            Rng apr(o.pitchSeed ^ 0xA9906A2Au);
            for (auto& ph : plan)
            {
                const bool roll = apr.chance(0.15f * (float) o.color);   // consume EVERY phrase
                if (! roll) continue;
                const int ps = ph.startBar * COLS, pe = (ph.startBar + ph.lenBars) * COLS;
                for (size_t i = 1; i + 1 < notes.size(); ++i)
                {
                    auto& n = notes[i];
                    if (n.start < ps || n.start >= pe) continue;
                    if (notes[i + 1].start >= pe) continue;          // never the phrase's final note
                    if (n.core || notes[i + 1].core) continue;
                    if (! latStrong(c, n.start, false)) continue;    // strong beat only
                    bool cp[12]; chordPcs(o, chordRootDegAt(o, c, n.start), cp);
                    if (! cp[pc(n.semi)]) continue;                  // must START as a chord tone
                    const int up = ladStepOf(ladF, n.semi, 1);
                    if (up == n.semi || cp[pc(up)]) continue;        // the neighbour must dissonate
                    if (std::abs(up - notes[i - 1].semi) < 4) continue;   // must be LEAPT into
                    const int resolved = n.semi;                     // the tone it resolves onto
                    n.semi = up;             n.core = true;          // protect the dissonance
                    notes[i + 1].semi = resolved; notes[i + 1].core = true;   // ...and its resolution
                    break;                                           // max ONE per phrase (M8)
                }
            }
        }
        // ---- M9: TENDENCY TONES at phrase ends (7^ -> 1^ up, 4^ -> 3^ down) + aug-2nd guard ----
        for (auto& ph : plan)
        {
            const int ps = ph.startBar * COLS, pe = (ph.startBar + ph.lenBars) * COLS;
            size_t last = notes.size(), penult = notes.size();
            for (size_t i = 0; i < notes.size(); ++i)
                if (notes[i].start >= ps && notes[i].start < pe) { penult = last; last = i; }
            if (last >= notes.size() || penult >= notes.size()) continue;
            const int leadPc  = pc(o.key + o.scale[(L - 1) % L]);
            const int fourPc  = pc(o.key + o.scale[3 % L]);
            const int tonicPc = pc(o.key);
            const int thirdPc = pc(o.key + o.scale[2 % L]);
            if (ph.resolved && pc(notes[penult].semi) == leadPc)
            {   // the leading tone RESOLVES UP to the tonic just above it
                int best = notes[last].semi, bd = 1 << 20;
                for (int s2 : ladF)
                    if (pc(s2) == tonicPc && s2 > notes[penult].semi && s2 - notes[penult].semi < bd)
                    { bd = s2 - notes[penult].semi; best = s2; }
                notes[last].semi = best;
            }
            else if (pc(notes[penult].semi) == fourPc && o.role != RoleBass)
            {   // 4^ falls to 3^ below it
                int best = notes[last].semi, bd = 1 << 20;
                for (int s2 : ladF)
                    if (pc(s2) == thirdPc && s2 < notes[penult].semi && notes[penult].semi - s2 < bd)
                    { bd = notes[penult].semi - s2; best = s2; }
                notes[last].semi = best;
            }
            // no AUGMENTED 2nd into the cadence (harmonic minor's 3-semitone step): respell the
            // penult one scale step toward the last note
            if (std::abs(notes[last].semi - notes[penult].semi) == 3 && ! notes[penult].core)
            {
                bool mem[12] = {};
                for (int k2 = 0; k2 < L; ++k2) mem[pc(o.key + o.scale[k2])] = true;
                if (mem[pc(notes[last].semi)] && mem[pc(notes[penult].semi)])
                    notes[penult].semi = ladStepOf(ladF, notes[penult].semi,
                                                   notes[last].semi > notes[penult].semi ? 1 : -1);
            }
        }
    }

    // [P1 G3-lite -> r20 E FULL BAND -> r21 LATTICE] LHL SYNCOPATION BAND per role (the doc's
    // targets: Melody/Riff [2,6], Bass [1,4], Hum [0,2]) on the CONTEXT lattice's weight map.
    // CEILING: de-syncopate offenders by shifting the worst off-onset to a stronger silent
    // LATTICE CELL - when the scorer's own target is illegal (a pocket exclusion / taken) the
    // pass walks DOWN the silent cells' strength order instead of giving up. FLOOR [r20]: a
    // bar UNDER the band pushes its LAST movable onset 1-2 lattice cells LATER (off-position
    // followed by air = honest syncopation), rhythm-seed deterministic. Skipped for
    // Driving-with-drums (locked to the groove's own syncopation = intentional) and for Riff
    // (the tiled cell is the identity); bar-opening onsets never move.
    if (o.role != RoleRiff && o.role != RoleChords
        && ! (o.rhythm == RhDriving && c.nHits > 0) && ! notes.empty())
    {
        const int maxBand = o.role == RoleBass ? 4 : (o.role == RoleHum ? 2 : 6);
        const int minBand = o.role == RoleBass ? 1 : (o.role == RoleHum ? 0 : 2);
        const int cw = latCw(c), nL = latN(c);
        auto nearAnyHit = [&](int col)
        {
            for (int i = 0; i < c.nHits; ++i)
                if (std::abs(col - c.hitCol[i]) < cw) return true;
            return false;
        };
        auto legalTarget = [&](int col)
        {
            if (o.rhythm == RhPockets && nearAnyHit(col)) return false;
            for (auto& n : notes) if (std::abs(n.start - col) < cw) return false;
            return true;
        };
        for (int b = 0; b < bars; ++b)
            for (int iter = 0; iter < 6; ++iter)
            {
                std::sort(notes.begin(), notes.end(), byStart);
                int wi = -1, tcol = -1;
                if (lhlBarScoreImpl(notes, b, &wi, &tcol, &c) <= maxBand || wi < 0 || tcol < 0) break;
                bool barFirst = true;                    // never move a bar's opening onset
                for (auto& n : notes)
                    if (n.start >= b * COLS && n.start < notes[(size_t) wi].start) { barFirst = false; break; }
                if (barFirst) break;
                if (! legalTarget(tcol))
                {   // [r21] the named cell is blocked: the strongest LEGAL silent cell between
                    // the offender and the next onset takes its place (the ceiling must HOLD)
                    const int gW = latCellNear(c, notes[(size_t) wi].start);
                    int gNext = (b + 1) * nL;
                    for (auto& n : notes)
                        if (n.start > notes[(size_t) wi].start)
                        { gNext = std::min(gNext, latCellNear(c, n.start)); break; }
                    tcol = -1; int bestW = -99;
                    for (int g = gW + 1; g < gNext; ++g)
                    {
                        const int col2 = latColOf(c, g);
                        if (nearAnyHit(col2)) break;   // a drum hit ends the air (scorer's rule)
                        const int wq = c.latW[juceLikeClamp(0, c.bars * nL - 1, g)];
                        if (wq > bestW && legalTarget(col2)) { bestW = wq; tcol = col2; }
                    }
                    if (tcol < 0) break;
                }
                notes[(size_t) wi].start = tcol;
            }
        Rng ir(o.rhythmSeed ^ 0x5C0FFEE5u);
        for (int b = 0; b < bars && minBand > 0; ++b)
            for (int iter = 0; iter < 3; ++iter)
            {
                std::sort(notes.begin(), notes.end(), byStart);
                if (lhlBarScoreImpl(notes, b, nullptr, nullptr, &c) >= minBand) break;
                Note* last = nullptr; int cnt = 0;       // the bar's LAST onset, never its first
                for (auto& n : notes)
                    if (n.start >= b * COLS && n.start < (b + 1) * COLS) { last = &n; ++cnt; }
                if (last == nullptr || cnt < 2) break;
                const int tgt = latColOf(c, latCellNear(c, last->start) + 1 + ir.ri(2));
                if (tgt >= (b + 1) * COLS) break;
                bool free2 = true;
                for (auto& n : notes) if (&n != last && std::abs(n.start - tgt) < cw) free2 = false;
                if (o.rhythm == RhPockets && nearAnyHit(tgt)) break;
                if (! free2) break;
                last->start = tgt;
            }
    }

    // [P1] INTENSITY: level + accent depth around the 200 mean. Constants informed by the Groove
    // MIDI mining (docs/generate-calibration.md): soft playing sits ~45-60 MIDI vel with shallow
    // accents, hard ~90-120 with ~2x accent ratios - mapped into our 120..255 space.
    if (o.intensity != 1)
    {
        const float mul = o.intensity == 0 ? 0.55f : 1.5f;
        const float ofs = o.intensity == 0 ? -26.0f : 10.0f;
        for (auto& n : notes)
            n.vel = std::max(120, std::min(255,
                        (int) std::lround(200.0f + ((float) n.vel - 200.0f) * mul + ofs)));
    }

    // PHRASE DYNAMICS [v2]: a gentle swell toward the two-thirds point of each bar-pair
    for (auto& n : notes)
    {
        const float t = (float) (n.start % (2 * COLS)) / (float) (2 * COLS);
        const float sw = 0.92f + 0.14f * std::sin(std::min(1.0f, t / 0.66f) * 3.14159265f);
        n.vel = std::max(120, std::min(255, (int) std::lround((float) n.vel * sw)));
    }

    // [P1] HUMANIZE: seed-deterministic velocity + micro-start jitter (Groove MIDI micro IQR is
    // ~30 ms -> Loose ~ +-6 cols ~ 30 ms at 120 BPM, Subtle ~ +-2 cols; vel jitter scaled down
    // from the mined +-30..45 MIDI sd). Runs BEFORE the disciplines so they still hold after.
    if (o.humanize > 0)
    {
        Rng hr(o.rhythmSeed ^ (o.pitchSeed * 0x9e3779b9u) ^ 0x48554D41u);
        const int vj = o.humanize == 1 ? 10 : 22;
        const int sj = o.humanize == 1 ? 2  : 6;
        for (auto& n : notes)
        {
            n.vel = std::max(120, std::min(255, n.vel + (int) std::lround((hr.uf() - 0.5f) * 2.0f * (float) vj)));
            if (! polyChords)   // chord tones jitter TOGETHER or not at all (no accidental strum)
                n.start = std::max(0, std::min(total - 1, n.start + (int) std::lround((hr.uf() - 0.5f) * 2.0f * (float) sj)));
        }
    }

    // [P1 G8] SNARE PUNCTUATION (Melody): the snare's crack owns the LATTICE CELL after it -
    // offending onsets slide onto the snare col (ending ON the snare is idiomatic; Pockets
    // slides past the shadow instead, its exclusion window forbids the hit itself), and the
    // take's FINAL note snaps to the nearest snare col / one cell before it when a snare sits
    // within half a beat. [r21] all targets are lattice cells (the snare col itself is one).
    if (o.role == RoleMelody && c.nSnare > 0 && ! notes.empty())
    {
        std::sort(notes.begin(), notes.end(), byStart);
        const int cw = latCw(c);
        auto onNote = [&](int col)
        { for (auto& n : notes) if (n.start == col) return true; return false; };
        auto cellAfter  = [&](int col) { return latColOf(c, latCellNear(c, col) + 1); };
        auto cellBefore = [&](int col) { return latColOf(c, latCellNear(c, col) - 1); };
        for (auto& n : notes)
            for (int i = 0; i < c.nSnare; ++i)
            {
                const int sc = c.snareCol[i];
                if (n.start > sc && n.start < sc + cw)
                {
                    n.start = (o.rhythm != RhPockets && ! onNote(sc)) ? sc : cellAfter(sc);
                    break;
                }
            }
        auto& last = notes.back();
        int bestC = -1, bd = BEAT / 2 + 1;
        for (int i = 0; i < c.nSnare; ++i)
        {
            const int prefer[2] = { o.rhythm == RhPockets ? cellBefore(c.snareCol[i]) : c.snareCol[i],
                                    o.rhythm == RhPockets ? c.snareCol[i] : cellBefore(c.snareCol[i]) };
            for (int cand : prefer)
                if (cand >= 0 && cand < total && std::abs(cand - last.start) < bd
                    && (cand == last.start || ! onNote(cand)))
                { bd = std::abs(cand - last.start); bestC = cand; }
        }
        if (bestC >= 0) last.start = bestC;
    }

    // POCKET DISCIPLINE [P0 r18]: runs before the safety pass (which re-sorts and tidies any
    // dedupe/mono fallout of the shifts); the later len-clips only ever SHORTEN, so (b) survives.
    if (! polyChords) pocketDiscipline(o, c, notes, total);   // H3/H6 place the comp already

    // [P1 G2] FLOWING bass = AVOID: every note ENDS >= 1/32 bar before the next kick, so nothing
    // rings into the kick's punch (the third stance of the kick relationship).
    if (o.role == RoleBass && o.rhythm == RhFlowing && c.nKick > 0)
        for (auto& n : notes)
            for (int i = 0; i < c.nKick; ++i)
                if (c.kickCol[i] > n.start && n.start + n.len > c.kickCol[i] - COLS / 32)
                { n.len = std::max(1, c.kickCol[i] - COLS / 32 - n.start); break; }

    // ============================================================================
    // [r22 STAGE 5, H7] FULL COUNTER-LINE: a Riff written NEXT TO an existing melody plays
    // AGAINST it - (a) FREEZE during melody runs (>= 3 melody onsets inside a 5-cell window:
    // the riff yields, bar anchors survive as the tile's identity), (b) no strong-beat
    // unisons/octaves with the melody's sounding pitch class, (c) contrary/oblique motion
    // >= 60% among co-moving pairs (offenders retune opposite the melody's direction).
    // ============================================================================
    if (o.role == RoleRiff && c.melOccValid && ! notes.empty())
    {
        std::sort(notes.begin(), notes.end(), byStart);
        auto cellOf = [&](int col) { return juceLikeClamp(0, MAX_BARS * 16 - 1, col / CELL16); };
        notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n)
        {
            if ((n.start % COLS) == 0) return false;             // the tile's anchor survives
            const int g = cellOf(n.start);
            int cnt = 0;
            for (int q = g - 2; q <= g + 2; ++q)
                if (q >= 0 && q < MAX_BARS * 16 && c.melOnset[q]) ++cnt;
            return cnt >= 3;                                     // a melody RUN owns this ground
        }), notes.end());
        const int centreR = registerCentre(o);
        std::vector<int> ladR;
        buildLadder(o, centreR - 8, centreR + 8, ladR);
        if (! ladR.empty() && ! notes.empty())
        {
            auto unisonBan = [&]                                 // (b) strong-beat unison/octave ban
            {
                for (auto& n : notes)
                {
                    if (! latStrong(c, n.start, false)) continue;
                    const int g = cellOf(n.start);
                    if (c.melPcP1[g] == 0) continue;
                    if (pc(n.semi) == c.melPcP1[g] - 1)
                        n.semi = std::max(-48, std::min(48, ladStepOf(ladR, n.semi, -1)));
                }
            };
            unisonBan();
            for (int iter = 0; iter < 8; ++iter)                 // (c) the contrary/oblique census
            {
                int simN = 0, tot = 0; size_t worst = notes.size();
                for (size_t i = 1; i < notes.size(); ++i)
                {
                    const int g = cellOf(notes[i].start);
                    if (! c.melOnset[g] || c.melDir[g] == 0) continue;   // melody holds = oblique
                    const int rd = notes[i].semi - notes[i - 1].semi;
                    if (rd == 0) continue;                               // the riff holds = oblique
                    ++tot;
                    if ((rd > 0) == (c.melDir[g] > 0))
                    { ++simN; if (worst >= notes.size()) worst = i; }
                }
                if (tot == 0 || worst >= notes.size()
                    || (float) (tot - simN) / (float) tot >= 0.6f) break;
                const int g = cellOf(notes[worst].start);        // flip: move OPPOSITE the melody
                int flip = ladStepOf(ladR, notes[worst - 1].semi, c.melDir[g] > 0 ? -1 : 1);
                if (pc(flip) == (c.melPcP1[cellOf(notes[worst].start)] - 1) % 12)   // never onto a unison
                    flip = ladStepOf(ladR, flip, c.melDir[g] > 0 ? -1 : 1);
                notes[worst].semi = std::max(-48, std::min(48, flip));
            }
            unisonBan();   // the census flips must not have re-created a strong-beat unison
        }
    }

    // safety: sorted, clamped; MONO dedupe/trim only for the single-line roles (poly comping
    // deliberately stacks same-start chord tones - the roll write handles them natively)
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    if (! polyChords)
    {
        notes.erase(std::unique(notes.begin(), notes.end(),
                                [](const Note& a, const Note& b) { return a.start == b.start; }), notes.end());
        for (size_t i = 0; i + 1 < notes.size(); ++i)
            notes[i].len = std::max(1, std::min(notes[i].len, notes[i + 1].start - notes[i].start));
    }
    if (! notes.empty()) notes.back().len = std::max(1, std::min(notes.back().len, total - notes.back().start));

    // [P1 H8/H9] SOUND-AWARE lengths (the target sound's envelope, gathered by GenContext):
    // a slow-attack pad (atk >= 120 ms) needs room to bloom - min half-note holds, up to the
    // next note (mono); a pluck (sustain ~0, decay < 300 ms) is a one-cell voice - long gates
    // only gate its silence. Mono / scaleOn sounds need single-note lines: the whole output is
    // monophonic by construction (the safety pass above), so H9 holds for every role. The
    // pocket / kick-avoid clips are re-applied after (they always win over a length extension).
    if (c.sndValid && ! notes.empty() && ! polyChords)
    {
        const bool pluck = c.sndSus < 0.05f && c.sndDec < 0.3f && c.sndAtk < 0.05f;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const int nextStart = i + 1 < notes.size() ? notes[i + 1].start : total;
            if (sndSlowAtk)
                notes[i].len = std::max(notes[i].len,
                                        std::min(COLS / 2, nextStart - notes[i].start));
            else if (pluck)
                notes[i].len = std::min(notes[i].len, CELL16);
            notes[i].len = std::max(1, notes[i].len);
        }
        if (o.rhythm == RhPockets && c.nHits > 0)         // re-clip: nothing rings into a hit
            for (auto& n : notes)
                for (int i = 0; i < c.nHits; ++i)
                    if (c.hitCol[i] > n.start && n.start + n.len > c.hitCol[i] - COLS / 32)
                    { n.len = std::max(1, c.hitCol[i] - COLS / 32 - n.start); break; }
        if (o.role == RoleBass && o.rhythm == RhFlowing && c.nKick > 0)
            for (auto& n : notes)
                for (int i = 0; i < c.nKick; ++i)
                    if (c.kickCol[i] > n.start && n.start + n.len > c.kickCol[i] - COLS / 32)
                    { n.len = std::max(1, c.kickCol[i] - COLS / 32 - n.start); break; }
    }

    // ============================================================================
    // [r22 STAGE 6, G10] SUSTAIN vs STAB (bass lengths follow the DRUM BED's density):
    // kicks present = a note rides ~80% of the mean inter-kick gap; busy hats (>= 8/bar) =
    // every other note stabs (>= 50% staccato); a SPARSE bed (< 2 hits/bar) = notes sustain
    // to the next onset or chord change. The pocket / kick-avoid clips re-run after (they
    // always outrank a length extension).
    // ============================================================================
    if (o.role == RoleBass && ! notes.empty() && ! polyChords)
    {
        std::sort(notes.begin(), notes.end(), byStart);
        if (c.nKick >= 2)
        {
            long gapSum = 0;
            for (int i = 1; i < c.nKick; ++i) gapSum += c.kickCol[i] - c.kickCol[i - 1];
            const int cap = std::max(latCw(c) / 2, (int) (gapSum / (c.nKick - 1) * 8 / 10));
            for (auto& n : notes) n.len = std::min(n.len, cap);
        }
        if (c.nHat / std::max(1, bars) >= 8)
            for (size_t i = 0; i < notes.size(); i += 2)
                notes[i].len = std::max(latCw(c) / 2, std::min(notes[i].len, BEAT / 2));
        if (c.nHits < 2 * bars)
            for (size_t i = 0; i < notes.size(); ++i)
            {
                int lim = i + 1 < notes.size() ? notes[i + 1].start : total;
                if (c.chordColsValid)
                    for (int ci = 0; ci < c.nChords; ++ci)
                        if (c.chordColAt[ci] > notes[i].start) { lim = std::min(lim, c.chordColAt[ci]); break; }
                notes[i].len = std::max(notes[i].len, std::max(latCw(c) / 2, lim - notes[i].start));
            }
        if (o.rhythm == RhPockets && c.nHits > 0)          // re-clip: nothing rings into a hit
            for (auto& n : notes)
                for (int i = 0; i < c.nHits; ++i)
                    if (c.hitCol[i] > n.start && n.start + n.len > c.hitCol[i] - COLS / 32)
                    { n.len = std::max(1, c.hitCol[i] - COLS / 32 - n.start); break; }
        if (o.rhythm == RhFlowing && c.nKick > 0)
            for (auto& n : notes)
                for (int i = 0; i < c.nKick; ++i)
                    if (c.kickCol[i] > n.start && n.start + n.len > c.kickCol[i] - COLS / 32)
                    { n.len = std::max(1, c.kickCol[i] - COLS / 32 - n.start); break; }
    }

    // FINAL CADENCE GUARD [v3]: the vary/singable passes run AFTER the per-phrase cadence snap
    // and can move the last note - re-land it home (the "last phrase resolves" promise must
    // survive every later pass). Riff excluded (its cell tiles verbatim, no cadence promise).
    if (o.role != RoleRiff && ! polyChords && ! notes.empty())
    {
        auto& last = notes.back();
        bool cad[12] = {};
        cad[pc(o.key + o.scale[0])] = true;
        if (o.role != RoleBass) cad[pc(o.key + o.scale[2 % o.scaleLen])] = true;
        if (! cad[pc(last.semi)])
        {
            const int prev = notes.size() >= 2 ? notes[notes.size() - 2].semi : 999;
            const int centre = registerCentre(o);
            std::vector<int> lad;
            buildLadder(o, centre - (o.singable ? 6 : 8), centre + (o.singable ? 6 : 8), lad);
            int best = last.semi, bd = 1 << 20;
            for (int s2 : lad)
            {
                if (! cad[pc(s2)]) continue;
                if (o.singable && prev != 999 && std::abs(s2 - prev) > 7) continue;
                const int d = std::abs(s2 - last.semi);
                if (d < bd) { bd = d; best = s2; }
            }
            last.semi = best;
        }
    }

    // ============================================================================
    // [r22 STAGE 6, H10] MULTISAMPLE REACH: every note stays within 5 st of the target
    // instrument's nearest ZONE (the varispeed cap - past that a stretched zone reads as
    // chipmunk/mud). Octave folds first (pitch-class preserving); a range narrower than an
    // octave clamps to its bound - the hardware limit outranks theory, so this runs LAST.
    // ============================================================================
    if (c.sndValid && c.sndMsLo >= 0 && c.sndMsHi >= c.sndMsLo && ! notes.empty())
    {
        const int lo = std::max(-48, c.sndMsLo - 60 - 5);
        const int hi = std::min(48, c.sndMsHi - 60 + 5);
        if (lo <= hi)
            for (auto& n : notes)
            {
                int s2 = n.semi;
                while (s2 < lo && s2 + 12 <= hi) s2 += 12;
                while (s2 > hi && s2 - 12 >= lo) s2 -= 12;
                n.semi = juceLikeClamp(lo, hi, s2);
            }
    }
    return notes;
}

// ================================================================================================
// [2026-07-22 r20, item I] KEEP MY NOTES - the augmentation half that keeps the USER'S RHYTHM:
// the caller passes the channel's existing notes (starts + lens untouched, the user's identity)
// and this REPITCHES them with the full pitching rules (chord tones on strong beats, ladder
// proximity, resolved cadence). The other half (keep pitches, new rhythm) needs no engine: the
// editor generates fresh rhythm and pours the user's pitch SEQUENCE over it.
// ================================================================================================
static inline void repitch(const Options& oIn, const Ctx& cIn, std::vector<Note>& notes)
{
    using namespace detail;
    if (notes.empty()) return;
    Options o = oIn;
    if (o.scale == nullptr || o.scaleLen < 5)
    { static const int8_t maj[7] = { 0, 2, 4, 5, 7, 9, 11 }; o.scale = maj; o.scaleLen = 7; }
    Ctx c = cIn;
    c.bars = std::max(1, std::min(MAX_BARS, c.bars));
    prepareChordsImpl(o, c);
    prepareLatticeImpl(c);   // [r21] the strong-col rule reads the lattice strength map
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    Rng prng(o.pitchSeed);
    PitchState st;
    pitchPhrase(o, c, prng, notes, st, 0, c.bars * COLS, true, -1, 0);
    for (auto& n : notes) n.semi = std::max(-48, std::min(48, n.semi));
}
} // namespace PartGen
