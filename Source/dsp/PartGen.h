#pragma once
// ============================================================================
// PartGen [2026-07-20] - the GENERATE feature's melody engine (v1.5.4 headline).
//
// FOR NEW READERS: this is a pure, DETERMINISTIC, rule-based part generator.
// Given a musical context (bars, key/scale, the groove's drum-hit map, optional
// per-beat harmony weights) and a set of user options (role/density/register/
// color/rhythm stance/contour/phrase/singable), it emits a MONOPHONIC line of
// piano-roll notes in CONCAT columns (DRAW_RES per bar, merged groups welcome).
//
// Design rules (agreed with the user, 2026-07-20):
//  - ONE-SHOT generation: dice are rolled here, once, on the message thread.
//    Playback never touches this code - the sequencer stays deterministic.
//  - TWO independent seeds: rhythmSeed places the onsets, pitchSeed picks the
//    pitches. "Same rhythm, new notes" = keep rhythmSeed, reroll pitchSeed
//    (and vice versa) - iteration is a first-class feature, not a slot machine.
//  - varyCount > 0 = "Vary": same seeds, then mutate ~30% of the pitches with
//    a seed derived from the counter (same skeleton, new ornaments).
//  - The SCALE is passed in by the caller (the UI's kUiScaleTab or a test
//    literal) - no third copy of the scale tables (the mirror rule).
//  - No JUCE editor dependencies: testable headlessly (tests/GenTest.cpp).
// ============================================================================

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

namespace PartGen
{
static constexpr int COLS   = 384;   // columns per bar (== DrumChannel::DRAW_RES)
static constexpr int CELL16 = COLS / 16;   // one 16th
static constexpr int BEAT   = COLS / 4;    // one beat (4/4 grid; odd meters still get a musical line)
static constexpr int MAX_BARS = 8;

enum Role    { RoleBass = 0, RoleMelody, RoleHum, RoleRiff };
enum Rhythm  { RhFlowing = 0, RhPockets, RhDriving };
enum Contour { CtAuto = 0, CtArch, CtRising, CtFalling, CtWave };

struct Options
{
    int  role       = RoleMelody;
    int  key        = 0;          // tonic pitch class 0 = C .. 11 = B
    const int8_t* scale = nullptr; // semitone offsets from the tonic (caller-owned)
    int  scaleLen   = 7;
    int  density    = 1;          // 0 sparse | 1 medium | 2 busy
    int  registerBand = 1;        // 0 low | 1 mid | 2 high  (Bassline caps at mid-low)
    int  color      = 0;          // 0 safe | 1 spicy | 2 colorful
    int  rhythm     = RhPockets;
    int  contour    = CtAuto;
    int  phrase     = 0;          // 0 = repeat with variation | 1 = evolve (multi-bar only)
    bool singable   = false;      // ~one octave, stepwise, breath gaps at phrase ends
    uint32_t rhythmSeed = 1;
    uint32_t pitchSeed  = 2;
    int  varyCount  = 0;          // > 0 = mutate pitches on top of the same seeds
};

struct Ctx
{
    int   bars = 1;                          // 1..MAX_BARS (merged group size)
    float grooveHit[MAX_BARS * 16] = {};     // per concat 16th: drum-hit strength 0..1 (0 = silence there)
    float chroma[MAX_BARS * 4][12] = {};     // per concat beat: pitch-class weights from the pitched channels
    bool  chromaValid = false;               // false = no pitched material found -> internal progression
};

struct Note { int start = 0, len = 1, semi = 0, vel = 235; };

// --- deterministic RNG (xorshift32) --------------------------------------------------------------
struct Rng
{
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9e3779b9u) {}
    uint32_t next()            { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float    uf()              { return (float) (next() >> 8) / 16777216.0f; }      // [0,1)
    int      ri(int n)         { return n <= 1 ? 0 : (int) (next() % (uint32_t) n); }
    bool     chance(float p)   { return uf() < p; }
};

namespace detail
{
static inline int pc(int semi) { return ((semi % 12) + 12) % 12; }

// The absolute scale LADDER: every in-scale semitone inside [lo, hi], sorted. Stepwise motion =
// walking this ladder; leaps = jumping indices. (Avoids degree/octave arithmetic entirely.)
static inline void buildLadder(const Options& o, int lo, int hi, std::vector<int>& out)
{
    bool member[12] = {};
    for (int i = 0; i < o.scaleLen; ++i) member[pc(o.key + o.scale[i])] = true;
    out.clear();
    for (int s = lo; s <= hi; ++s) if (member[pc(s)]) out.push_back(s);
}

// Diatonic chord (root scale-degree r): pitch-class set of the stacked thirds {r, r+2, r+4}.
static inline void chordPcs(const Options& o, int rootDeg, bool out[12])
{
    for (int i = 0; i < 12; ++i) out[i] = false;
    const int L = o.scaleLen;
    for (int k = 0; k < 3; ++k) out[pc(o.key + o.scale[(rootDeg + 2 * k) % L])] = true;
}

// Bar -> chord ROOT scale degree. With harmony info: the strongest in-scale pitch class of the
// bar's beats. Without: a stock progression per bar count (I / I-V / I-vi-IV-V feel).
static inline int chordRootDeg(const Options& o, const Ctx& c, int bar)
{
    if (c.chromaValid)
    {
        float w[12] = {};
        for (int bt = bar * 4; bt < bar * 4 + 4 && bt < c.bars * 4; ++bt)
            for (int p = 0; p < 12; ++p) w[p] += c.chroma[bt][p];
        int bestDeg = 0; float best = 0.0f;
        for (int d = 0; d < o.scaleLen; ++d)
        {
            const int p = pc(o.key + o.scale[d]);
            // weight the root pc + a little of its third/fifth so a full chord wins over a stray note
            bool cp[12]; chordPcs(o, d, cp);
            float sc = w[p] * 2.0f;
            for (int q = 0; q < 12; ++q) if (cp[q]) sc += w[q] * 0.5f;
            if (sc > best) { best = sc; bestDeg = d; }
        }
        if (best > 0.01f) return bestDeg;
    }
    if (o.scaleLen < 6)                    // pentatonic/blues: stay on the tonic (progressions read odd)
        return 0;
    static const int p1[1] = { 0 };
    static const int p2[2] = { 0, 4 };                    // I  V
    static const int p4[4] = { 0, 5, 3, 4 };              // I  vi IV V
    static const int p8[8] = { 0, 0, 5, 5, 3, 3, 4, 4 };
    const int b = c.bars;
    if (b >= 8) return p8[bar % 8];
    if (b >= 4) return p4[bar % 4];
    if (b >= 2) return p2[bar % 2];
    return p1[0];
}

// Metric strength of a concat column (beat 1 strongest, then beat 3, then 2/4, 8ths, 16ths).
static inline float metricW(int col)
{
    const int inBar = col % COLS;
    if (inBar % COLS  == 0)        return 1.0f;
    if (inBar % (BEAT * 2) == 0)   return 0.8f;
    if (inBar % BEAT == 0)         return 0.6f;
    if (inBar % (BEAT / 2) == 0)   return 0.35f;
    return 0.2f;
}

struct Onset { int col; float strength; };

// ---- RHYTHM: place the onsets for [barFrom, barTo) --------------------------------------------
static inline void makeRhythm(const Options& o, const Ctx& c, Rng& rng,
                              int barFrom, int barTo, std::vector<Onset>& out)
{
    // notes per bar by role x stance, scaled by density
    static const int base[4][3] = { { 2, 4, 5 },   // Bass    flowing/pockets/driving
                                    { 3, 5, 6 },   // Melody
                                    { 2, 3, 4 },   // Hum
                                    { 4, 5, 6 } }; // Riff (per-bar cell)
    static const float dmul[3] = { 0.6f, 1.0f, 1.5f };
    const int perBar = std::max(1, std::min(10,
        (int) std::lround((float) base[o.role][o.rhythm] * dmul[o.density])));

    for (int bar = barFrom; bar < barTo; ++bar)
    {
        // candidate = every 16th of the bar, weighted by metric strength x stance x groove
        float w[16];
        for (int i = 0; i < 16; ++i)
        {
            const int col = bar * COLS + i * CELL16;
            float v = metricW(col);
            const float gh = c.grooveHit[std::min(bar, MAX_BARS - 1) * 16 + i];
            if (o.rhythm == RhFlowing)      v *= (i % 4 == 0) ? 1.0f : 0.25f;        // float on the beats
            else if (o.rhythm == RhPockets) v *= 0.25f + 0.9f * (1.0f - gh);         // land in the gaps
            else                            v *= 0.30f + 0.9f * gh;                  // lock onto the drums
            if (o.role == RoleBass && i == 0) v *= 2.0f;                             // bass anchors beat 1
            w[i] = v;
        }
        // singable: breathe - no fresh onsets in the bar's last 16th (phrase-end gap handled later too)
        if (o.singable) w[15] = 0.0f;

        int picked[16]; int nPicked = 0;
        for (int k = 0; k < perBar; ++k)
        {
            float tot = 0.0f; for (int i = 0; i < 16; ++i) tot += w[i];
            if (tot <= 0.0001f) break;
            float r = rng.uf() * tot; int sel = 0;
            for (int i = 0; i < 16; ++i) { r -= w[i]; if (r <= 0.0f) { sel = i; break; } }
            picked[nPicked++] = sel;
            w[sel] = 0.0f;
            if (o.singable)   // enforce >= one 8th between onsets (no 16th chatter in a hummed line)
            { if (sel > 0) w[sel - 1] = 0.0f; if (sel < 15) w[sel + 1] = 0.0f; }
        }
        // an empty or beat-less first bar gets an anchor on beat 1 (a line must start somewhere)
        bool hasEarly = false;
        for (int k = 0; k < nPicked; ++k) if (picked[k] <= 4) hasEarly = true;
        if (bar == barFrom && (! hasEarly || nPicked == 0)) picked[nPicked++] = 0;

        std::sort(picked, picked + nPicked);
        for (int k = 0; k < nPicked; ++k)
            out.push_back({ bar * COLS + picked[k] * CELL16,
                            c.grooveHit[std::min(bar, MAX_BARS - 1) * 16 + picked[k]] });
    }
    std::sort(out.begin(), out.end(), [](const Onset& a, const Onset& b) { return a.col < b.col; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Onset& a, const Onset& b) { return a.col == b.col; }), out.end());
}

// ---- PITCH: walk the ladder over the onsets ---------------------------------------------------
struct PitchState { int prevIdx = -1; };

static inline int registerCentre(const Options& o)
{
    if (o.role == RoleBass)  return o.registerBand == 0 ? -19 : -14;   // bass ignores High (spec)
    static const int ctr[3] = { -15, -4, +8 };
    return ctr[o.registerBand];
}

static inline void makePitches(const Options& o, const Ctx& c, Rng& rng,
                               std::vector<Note>& notes, PitchState& st, int phraseStartCol, int phraseEndCol)
{
    const int centre = registerCentre(o);
    const int range  = o.singable ? 6 : 8;
    std::vector<int> ladder;
    buildLadder(o, centre - range, centre + range, ladder);
    if (ladder.empty()) { buildLadder(o, centre - 12, centre + 12, ladder); }
    if (ladder.empty()) return;

    // contour curve over THIS phrase
    int ct = o.contour;
    if (ct == CtAuto) ct = 1 + rng.ri(4);
    auto target = [&](int col) -> float
    {
        const float t = phraseEndCol > phraseStartCol
                      ? (float) (col - phraseStartCol) / (float) (phraseEndCol - phraseStartCol) : 0.0f;
        float u = 0.5f;
        switch (ct) { case CtArch:    u = std::sin(t * 3.14159265f); break;
                      case CtRising:  u = t; break;
                      case CtFalling: u = 1.0f - t; break;
                      case CtWave:    u = 0.5f + 0.5f * std::sin(t * 6.2831853f); break; }
        return (float) centre + (u - 0.5f) * 2.0f * (float) range * 0.8f;
    };

    for (auto& n : notes)
    {
        if (n.start < phraseStartCol || n.start >= phraseEndCol) continue;
        const int bar   = n.start / COLS;
        const bool strong = (n.start % BEAT) == 0;   // on a beat
        const int rootDeg = chordRootDeg(o, c, bar);
        bool cp[12]; chordPcs(o, rootDeg, cp);
        const float ideal = st.prevIdx < 0 ? target(n.start)
                          : 0.45f * (float) ladder[st.prevIdx] + 0.55f * target(n.start);

        int idx = st.prevIdx < 0 ? 0 : st.prevIdx;
        if (o.role == RoleBass && strong)
        {
            // bass strong beats: root 70% / fifth 20% / octave-of-root 10%
            const int wantPc = rng.chance(0.7f) ? pc(o.key + o.scale[rootDeg])
                             : rng.chance(0.66f) ? pc(o.key + o.scale[(rootDeg + 4) % o.scaleLen])
                                                 : pc(o.key + o.scale[rootDeg]);
            int best = -1; float bd = 1e9f;
            for (int i = 0; i < (int) ladder.size(); ++i)
                if (pc(ladder[i]) == wantPc)
                { const float d = std::fabs((float) ladder[i] - ideal); if (d < bd) { bd = d; best = i; } }
            idx = best >= 0 ? best : idx;
        }
        else if (strong || st.prevIdx < 0)
        {
            // strong beats (and the first note): a CHORD TONE near the contour target
            int c1 = -1, c2 = -1; float d1 = 1e9f, d2 = 1e9f;
            for (int i = 0; i < (int) ladder.size(); ++i)
            {
                if (! cp[pc(ladder[i])]) continue;
                const float d = std::fabs((float) ladder[i] - ideal);
                if (d < d1)      { d2 = d1; c2 = c1; d1 = d; c1 = i; }
                else if (d < d2) { d2 = d; c2 = i; }
            }
            if (c1 >= 0) idx = (c2 >= 0 && rng.chance(0.3f)) ? c2 : c1;   // best-of-2 keeps it human
        }
        else
        {
            // weak beats: stepwise from the previous note, biased toward the contour
            int maxStep = o.color == 2 ? 4 : (o.color == 1 ? 3 : 2);
            if (o.singable) maxStep = std::min(maxStep, 2);
            const int dir = ((float) ladder[idx] < ideal) ? 1 : -1;
            int step = 1 + (rng.chance(0.3f) ? rng.ri(maxStep) : 0);
            if (rng.chance(0.25f)) step = -step;                       // sometimes against the contour
            idx += dir * step;
            if (o.role == RoleBass && o.rhythm == RhDriving && rng.chance(0.5f))
                idx = st.prevIdx;                                      // pumping bass repeats the note
        }
        idx = std::max(0, std::min((int) ladder.size() - 1, idx));
        int semi = ladder[idx];

        // COLOR: chromatic neighbours on weak beats (spicy 10% / colorful 25%)
        if (! strong && o.color > 0 && rng.chance(o.color == 2 ? 0.25f : 0.10f))
        {
            const int chrom = semi + (rng.chance(0.5f) ? -1 : 1);
            if (std::abs(chrom - centre) <= range + 1) semi = chrom;
        }
        // SINGABLE: cap the leap at a fifth
        if (o.singable && st.prevIdx >= 0 && std::abs(semi - ladder[st.prevIdx]) > 7)
            semi = ladder[st.prevIdx] + (semi > ladder[st.prevIdx] ? 7 : -7);

        n.semi = std::max(-48, std::min(48, semi));
        st.prevIdx = idx;

        // VELOCITY: metric accent + groove accent + a little life
        float v = strong ? 232.0f : ((n.start % (BEAT / 2)) == 0 ? 205.0f : 188.0f);
        if (o.role == RoleHum || o.rhythm == RhFlowing) v = 200.0f + (v - 200.0f) * 0.4f;   // flatter, voice-like
        v += 20.0f * 0.0f;   // (groove strength folded in below by the caller-visible onset)
        v += (rng.uf() - 0.5f) * 24.0f;
        n.vel = std::max(120, std::min(255, (int) std::lround(v)));
    }
}

// note lengths: legato-fill or gated, per stance/role
static inline void makeLengths(const Options& o, std::vector<Note>& notes, int totalCols)
{
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const int nextStart = i + 1 < notes.size() ? notes[i + 1].start : totalCols;
        int gap = std::max(CELL16, nextStart - notes[i].start);
        int len;
        if (o.rhythm == RhFlowing || o.role == RoleHum) len = gap;                          // legato
        else if (o.rhythm == RhPockets)                 len = std::min(gap, BEAT) * 9 / 10;
        else                                            len = std::min(gap, BEAT / 2) * 9 / 10;
        if (o.role == RoleHum) len = std::max(len, BEAT / 2);
        len = std::min(len, totalCols - notes[i].start);
        notes[i].len = std::max(CELL16 / 2, std::min(len, gap));   // mono: never overlap the next onset
    }
    // singable: end each phrase with a breath - trim the last note a 16th short of the phrase end
    if (o.singable && ! notes.empty())
    {
        auto& last = notes.back();
        if (last.start + last.len > totalCols - CELL16)
            last.len = std::max(CELL16 / 2, totalCols - CELL16 - last.start);
    }
}
} // namespace detail

// ================================================================================================
// generate(): the one entry point. Returns the note list (concat columns, mono, no overlaps).
// ================================================================================================
static inline std::vector<Note> generate(const Options& oIn, const Ctx& c)
{
    using namespace detail;
    Options o = oIn;
    if (o.scale == nullptr || o.scaleLen < 5)
    { static const int8_t maj[7] = { 0, 2, 4, 5, 7, 9, 11 }; o.scale = maj; o.scaleLen = 7; }
    const int bars = std::max(1, std::min(MAX_BARS, c.bars));
    const int total = bars * COLS;

    Rng rrng(o.rhythmSeed), prng(o.pitchSeed);
    std::vector<Note> notes;

    if (o.role == RoleRiff)
    {
        // RIFF: ONE bar's cell, tiled across every bar, transposed to each bar's chord root.
        std::vector<Onset> ons;
        makeRhythm(o, c, rrng, 0, 1, ons);
        for (auto& on : ons) notes.push_back({ on.col, 1, 0, 235 });
        PitchState st;
        makePitches(o, c, prng, notes, st, 0, COLS);
        makeLengths(o, notes, COLS);
        const int rootDeg0 = chordRootDeg(o, c, 0);
        const int root0 = o.key + o.scale[rootDeg0];
        std::vector<Note> tiled;
        for (int b = 0; b < bars; ++b)
        {
            const int rd = chordRootDeg(o, c, b);
            int shift = (o.key + o.scale[rd]) - root0;          // transpose with the harmony
            while (shift > 6)  shift -= 12;                      // nearest direction
            while (shift < -6) shift += 12;
            for (auto n : notes)
            { n.start += b * COLS; n.semi = std::max(-48, std::min(48, n.semi + shift)); tiled.push_back(n); }
        }
        notes.swap(tiled);
    }
    else
    {
        // phrase model: <4 bars -> 1-bar phrases; >= 4 -> 2-bar phrases
        const int phraseBars = bars >= 4 ? 2 : 1;
        if (bars == 1 || o.phrase == 1)
        {
            // EVOLVE (or a single bar): one continuous line, contour per phrase
            std::vector<Onset> ons;
            makeRhythm(o, c, rrng, 0, bars, ons);
            for (auto& on : ons) notes.push_back({ on.col, 1, 0, 235 });
            PitchState st;
            for (int p = 0; p * phraseBars < bars; ++p)
                makePitches(o, c, prng, notes, st,
                            p * phraseBars * COLS, std::min(total, (p + 1) * phraseBars * COLS));
            makeLengths(o, notes, total);
        }
        else
        {
            // REPEAT WITH VARIATION: generate the first phrase, echo it with pitch mutations.
            std::vector<Onset> ons;
            makeRhythm(o, c, rrng, 0, phraseBars, ons);
            std::vector<Note> phrase;
            for (auto& on : ons) phrase.push_back({ on.col, 1, 0, 235 });
            PitchState st;
            makePitches(o, c, prng, phrase, st, 0, phraseBars * COLS);
            makeLengths(o, phrase, phraseBars * COLS);
            for (int p = 0; p * phraseBars < bars; ++p)
            {
                Rng vr(o.pitchSeed ^ (0x85ebca6bu * (uint32_t) (p + 1)));
                const int off = p * phraseBars * COLS;
                for (auto n : phrase)
                {
                    if (p > 0 && vr.chance(0.3f))    // the echo answers: ~30% of pitches move a step
                    {
                        std::vector<int> lad;
                        buildLadder(o, n.semi - 4, n.semi + 4, lad);
                        if (! lad.empty()) n.semi = lad[vr.ri((int) lad.size())];
                    }
                    n.start += off;
                    if (n.start < total) { n.len = std::min(n.len, total - n.start); notes.push_back(n); }
                }
            }
            // cadence: the very last note resolves to a tonic chord tone (root or third)
            if (! notes.empty())
            {
                bool cp[12]; chordPcs(o, 0, cp);
                auto& last = notes.back();
                if (! cp[pc(last.semi)])
                {
                    for (int d = 1; d <= 6; ++d)
                    { if (cp[pc(last.semi - d)]) { last.semi -= d; break; }
                      if (cp[pc(last.semi + d)]) { last.semi += d; break; } }
                }
            }
        }
    }

    // VARY: same skeleton, new ornaments - mutate ~30% of the pitches within the scale.
    if (o.varyCount > 0)
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

    // SINGABLE enforcement pass: whatever the phrase-echo/vary mutations did, the final line
    // must stay hummable - every pitch on the one-octave ladder, every leap capped at a fifth.
    if (o.singable && ! notes.empty())
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
                for (int s : lad)
                {
                    if (prev != 999 && std::abs(s - prev) > 7) continue;
                    const int d = std::abs(s - n.semi);
                    if (d < bd) { bd = d; best = s; }
                }
                n.semi = best; prev = best;
            }
        }
    }

    // safety: sorted, deduped, clamped, mono (never overlap the next onset)
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    notes.erase(std::unique(notes.begin(), notes.end(),
                            [](const Note& a, const Note& b) { return a.start == b.start; }), notes.end());
    for (size_t i = 0; i + 1 < notes.size(); ++i)
        notes[i].len = std::max(1, std::min(notes[i].len, notes[i + 1].start - notes[i].start));
    if (! notes.empty()) notes.back().len = std::max(1, std::min(notes.back().len, total - notes.back().start));
    return notes;
}
} // namespace PartGen
