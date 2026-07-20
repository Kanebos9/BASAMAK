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

namespace PartGen
{
static constexpr int COLS   = 384;   // columns per bar (== DrumChannel::DRAW_RES)
static constexpr int CELL16 = COLS / 16;   // one 16th
static constexpr int BEAT   = COLS / 4;    // one beat
static constexpr int MAX_BARS = 8;

enum Role     { RoleBass = 0, RoleMelody, RoleHum, RoleRiff };
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
};

struct Note { int start = 0, len = 1, semi = 0, vel = 235; };

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

// PER-BEAT chord root [v2]: the beat's own chroma when it speaks, the bar's aggregate when that
// beat is silent, the stock progression when nothing pitched exists at all.
static inline int chordRootDegAt(const Options& o, const Ctx& c, int col)
{
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

static inline float metricW(int col)
{
    const int inBar = col % COLS;
    if (inBar == 0)                return 1.0f;
    if (inBar % (BEAT * 2) == 0)   return 0.8f;
    if (inBar % BEAT == 0)         return 0.6f;
    if (inBar % (BEAT / 2) == 0)   return 0.35f;
    return 0.2f;
}

// ---- RHYTHM v2: candidates for one PHRASE span, from the groove's REAL positions --------------
struct Cand { int col; float w; };

static inline void phraseCandidates(const Options& o, const Ctx& c, int s, int e, std::vector<Cand>& out)
{
    out.clear();
    auto beatCands = [&](float wMul)
    {   // the METER's positions (beats strong, half-beats faint)
        for (int col = s - (s % (BEAT / 2)); col < e; col += BEAT / 2)
        {
            if (col < s) continue;
            const float m = metricW(col);
            if (m >= 0.6f) out.push_back({ col, m * wMul });          // beats
            else if (m >= 0.35f) out.push_back({ col, 0.14f * wMul });// half-beats, rare
        }
    };
    // collect the real hits inside the span
    std::vector<int> hIdx;
    for (int i = 0; i < c.nHits; ++i)
        if (c.hitCol[i] >= s && c.hitCol[i] < e) hIdx.push_back(i);

    if (o.rhythm == RhFlowing || hIdx.empty())
    {
        // Flowing floats on the METER (a vocal line breathes with the bar, not the hats);
        // and every stance falls back here when the groove offers nothing.
        if (o.rhythm == RhDriving && hIdx.empty())      beatCands(1.0f);
        else if (o.rhythm == RhPockets && hIdx.empty())
        {   // no drums to dodge: the old offbeat-leaning 16th grid
            for (int col = s; col < e; col += CELL16)
            {
                const float m = metricW(col);
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
    // Pockets: the MIDPOINTS of the real gaps between hits (plus the span edges' gaps)
    int prev = s;
    for (size_t k = 0; k <= hIdx.size(); ++k)
    {
        const int nxt = k < hIdx.size() ? c.hitCol[hIdx[k]] : e;
        const int gap = nxt - prev;
        if (gap >= CELL16 * 2)
            out.push_back({ prev + gap / 2, 0.25f + 0.75f * std::min(1.0f, (float) gap / (float) BEAT) });
        prev = nxt;
    }
}

// weighted sample `budget` onsets from the candidates (no replacement; singable = min 1/8 apart)
static inline void sampleOnsets(const Options& o, Rng& rrng, std::vector<Cand>& cands,
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
        const int minGap = o.singable ? CELL16 * 2 : CELL16;
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

static inline int registerCentre(const Options& o)
{
    if (o.role == RoleBass)  return o.registerBand == 0 ? -19 : -14;
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
        const bool strong = (n.start % BEAT) == 0
                         || (c.nHits > 0 && n.start % (BEAT / 2) == 0);   // hit-locked lines treat 8ths as landings
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

        float v = strong ? 232.0f : ((n.start % (BEAT / 2)) == 0 ? 205.0f : 188.0f);
        if (o.role == RoleHum || o.rhythm == RhFlowing) v = 200.0f + (v - 200.0f) * 0.4f;
        v += (prng.uf() - 0.5f) * 24.0f;
        n.vel = std::max(120, std::min(255, (int) std::lround(v)));
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

static inline void makeLengths(const Options& o, std::vector<Note>& notes, int totalCols)
{
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const int nextStart = i + 1 < notes.size() ? notes[i + 1].start : totalCols;
        int gap = std::max(CELL16, nextStart - notes[i].start);
        int len;
        if (o.rhythm == RhFlowing || o.role == RoleHum) len = gap;   // legato arcs (may cross bar lines)
        else if (o.rhythm == RhPockets)                 len = std::min(gap, BEAT) * 9 / 10;
        else                                            len = std::min(gap, BEAT / 2) * 9 / 10;
        if (o.role == RoleHum) len = std::max(len, BEAT / 2);
        len = std::min(len, totalCols - notes[i].start);
        notes[i].len = std::max(CELL16 / 2, std::min(len, gap));
    }
}

// ---- FORM PLANNING [v2] -----------------------------------------------------------------------
// Phrases are up to 2 bars; tags: 'A' fresh motif | 'a' echo (opening kept, tail varied) |
// 'n' answer (opening kept ~40%, fresh tail) | 'B' contrast | 'F' free. Last phrase resolves.
struct Phrase { int startBar, lenBars; char tag; bool resolved; };

// [v3] LINES model: split the group into `lines` sentences (earlier lines take the extra bar -
// 3 bars at 2 lines = 2 + 1), then decide what each later line does. AABA and friends EMERGE
// from Auto instead of being a jargon menu. lines 1 = ONE arc across the whole group.
static inline void planForm(int bars, int lines, int relation, std::vector<Phrase>& out)
{
    out.clear();
    int L = lines;
    if (L <= 0) L = bars <= 1 ? 1 : (bars >= 6 ? 4 : 2);   // Auto: long arcs by default
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

    static const int base[4][3] = { { 2, 4, 5 }, { 3, 5, 6 }, { 2, 3, 4 }, { 4, 5, 6 } };
    static const float dmul[3] = { 0.6f, 1.0f, 1.5f };
    auto budgetFor = [&](int lenBars)
    { return std::max(1, std::min(14, (int) std::lround((float) base[o.role][o.rhythm] * dmul[o.density] * (float) lenBars))); };
    const bool breathe = o.singable || o.role == RoleHum;   // vocal lines breathe at phrase ends

    if (o.role == RoleRiff)
    {
        // RIFF: one bar's cell tiled every bar, transposed with the harmony (unchanged from v1).
        std::vector<Cand> cands; std::vector<int> ons;
        phraseCandidates(o, c, 0, COLS, cands);
        sampleOnsets(o, rrng, cands, budgetFor(1), 0, COLS, ons);
        for (int col : ons) notes.push_back({ col, 1, 0, 235 });
        makeLengths(o, notes, COLS);
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
    else
    {
        std::vector<Phrase> plan;
        planForm(bars, o.lines, o.relation, plan);
        // the MOTIF = phrase A's material (onsets + pitches), the identity every echo/answer keeps
        std::vector<int>  motifOns;    // bar-local (relative to its phrase start)
        std::vector<Note> motifNotes;  // pitched A notes (phrase-relative starts)
        int motifCt = -1;              // A's rolled contour (echoes reuse it; B contrasts it)
        PitchState st;

        for (auto& ph : plan)
        {
            const int s = ph.startBar * COLS, e = (ph.startBar + ph.lenBars) * COLS;
            const int breathEnd = breathe ? e - CELL16 * 2 : e;
            std::vector<int> ons;

            if ((ph.tag == 'a' || ph.tag == 'n') && ! motifOns.empty())
            {
                // keep the OPENING (the recognizable part): echoes keep ~70%, answers ~40%
                const float keep = ph.tag == 'a' ? 0.7f : 0.4f;
                const int span = ph.lenBars * COLS;
                for (int col : motifOns)
                    if (col < (int) ((float) span * keep)) ons.push_back(s + col);
                // fresh tail rhythm from the remaining span (rhythm stream only)
                std::vector<Cand> cands;
                phraseCandidates(o, c, s + (int) ((float) span * keep), e, cands);
                std::vector<int> tail;
                const int want = std::max(1, budgetFor(ph.lenBars) - (int) ons.size());
                sampleOnsets(o, rrng, cands, want, s + (int) ((float) span * keep), breathEnd, tail);
                for (int col : tail) ons.push_back(col);
                // ECHO RHYTHM VARIATION [v2]: nudge the last kept onset sometimes - a bit-identical
                // rhythm reads as copy-paste (the user's complaint); the opening itself never moves
                if (ph.tag == 'a' && ons.size() >= 3 && rrng.chance(0.4f))
                {
                    std::sort(ons.begin(), ons.end());
                    int& mv = ons[ons.size() - 2];
                    const int shifted = mv + (rrng.chance(0.5f) ? CELL16 : -CELL16);
                    if (shifted > s && shifted < breathEnd) mv = shifted;
                }
            }
            else
            {
                std::vector<Cand> cands;
                phraseCandidates(o, c, s, e, cands);
                sampleOnsets(o, rrng, cands, budgetFor(ph.lenBars), s, breathEnd, ons);
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
        makeLengths(o, notes, total);
        // breath caps: a vocal phrase ends a 16th before the next one begins
        if (breathe)
            for (auto& ph : plan)
            {
                const int e = (ph.startBar + ph.lenBars) * COLS;
                for (auto& n : notes)
                    if (n.start < e && n.start + n.len > e - CELL16)
                        n.len = std::max(CELL16 / 2, e - CELL16 - n.start);
            }
    }

    // VARY: same skeleton, new ornaments
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

    // SINGABLE enforcement (whatever the echoes/vary did): one-octave ladder, leaps <= a fifth
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

    // PHRASE DYNAMICS [v2]: a gentle swell toward the two-thirds point of each bar-pair
    for (auto& n : notes)
    {
        const float t = (float) (n.start % (2 * COLS)) / (float) (2 * COLS);
        const float sw = 0.92f + 0.14f * std::sin(std::min(1.0f, t / 0.66f) * 3.14159265f);
        n.vel = std::max(120, std::min(255, (int) std::lround((float) n.vel * sw)));
    }

    // safety: sorted, deduped, clamped, mono
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    notes.erase(std::unique(notes.begin(), notes.end(),
                            [](const Note& a, const Note& b) { return a.start == b.start; }), notes.end());
    for (size_t i = 0; i + 1 < notes.size(); ++i)
        notes[i].len = std::max(1, std::min(notes[i].len, notes[i + 1].start - notes[i].start));
    if (! notes.empty()) notes.back().len = std::max(1, std::min(notes.back().len, total - notes.back().start));

    // FINAL CADENCE GUARD [v3]: the vary/singable passes run AFTER the per-phrase cadence snap
    // and can move the last note - re-land it home (the "last phrase resolves" promise must
    // survive every later pass). Riff excluded (its cell tiles verbatim, no cadence promise).
    if (o.role != RoleRiff && ! notes.empty())
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
    return notes;
}
} // namespace PartGen
