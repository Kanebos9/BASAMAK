#pragma once
// ============================================================================
// DrumGen [2026-07-22 r20, r22] - the GENERATE feature's DRUM KIT engine (v1.5.6 dev).
//
// FOR NEW READERS: a pure, DETERMINISTIC, style-DNA drum generator. Given a
// STYLE (genre canon), bar count and the panel dials, it emits per-LANE hit
// lists (kick / snare / hat / open hat / perc) in CONCAT columns (384/bar,
// cells = 16ths = 24 cols), plus the style's suggested pattern SWING and
// [r22] per-lane MICROTIMING (mined push/drag ms, Humanize-gated).
// The editor writes each lane to a real drum CHANNEL as STEPS (drums are
// step-native - GENERATE-THEORY "DRUM UI"); trap hat rolls ride out as
// stepRoll ratchets (Hit::roll / rollDec).
//
// [r22] STYLES ARE DATA now (GenStyle.h - the MMA lesson): every genre-specific
// branch this file used to hard-code (trap's roll cells, house/techno's
// all-offbeat open hats, the fill preference, the mined velocity means/sds and
// the swing) reads from the parsed GenStyle::Style instead. Options.dna points
// at the style (the panel's picker, incl. USER .basamakstyle files); the
// legacy Options.style index falls back to the embedded factory table, so the
// engine stays headless-testable with zero registry setup.
//
// THE RULES (docs/GENERATE-THEORY.md D1-D10) + THE CONSTANTS
// (docs/generate-calibration.md, mined from the Groove MIDI Dataset, CC-BY):
//  D1  genre kick canon: immutable cells + <=2 seed-picked optional cells.
//  D2  backbeat law: snare velocity ~100-115 MIDI (0.79..0.91 here).
//  D3  hat language: per-position velocity templates (16ths 105/60/85/60,
//      8ths 100/70), scaled to the genre's mined closed-hat mean; NEVER two
//      consecutive equal velocities (enforced by a final pass).
//  D4  open hat: 8th offbeats only, never a downbeat (only when the caller
//      has a dedicated open-hat channel).
//  D5  trap rolls: 1-2/bar, half-beat before the snare / bar end, ratchet
//      {2,3,4,6}, RISING ramp (stepRollDecay > 0 = build up).
//  D6  ghost snares: genre budget/bar on the 16ths flanking backbeats, at the
//      mined ~25% of backbeat velocity.
//  D8  fills: ONE per 4-bar phrase end - snare crescendo (hats suppressed),
//      subdivision doubling, or (trap) a hat-roll fill.
//  D9/D10 per-genre swing, returned as the pattern-swing field value.
//
// Standing rules: ONE-SHOT dice (message thread), everything from the two
// seeds = deterministic; VARY rerolls the canon-constrained mutations only
// (max 2 kick cells - D1); no JUCE, no editor deps (headless GenTest).
// ============================================================================

#include "PartGen.h"   // Rng + COLS/CELL16 constants (PartGen pulls GenStyle.h in)

namespace DrumGen
{
enum Style { StHouse = 0, StTechno, StBoomBap, StTrap, StDnB, StReggaeton, StFunk, StPop, NUM_STYLES };
enum Lane  { LKick = 0, LSnare, LHat, LOpenHat, LPerc, NUM_LANES };

static const char* const kStyleName[NUM_STYLES] =
    { "House", "Techno", "Boom-bap", "Trap", "DnB", "Reggaeton", "Funk", "Pop" };

struct Hit { int col = 0; float vel = 0.8f; int roll = 1; float rollDec = 0.0f; };

struct Options
{
    int style = StHouse, bars = 1;
    int density = 1;          // ghost/perc budget shift (canon grids never densify)
    int intensity = 1;        // 0 soft | 1 medium | 2 hard velocity level
    int humanize = 0;         // 0 off | 1 subtle | 2 loose - mined-sd velocity jitter + [r22] micro
    int fills = 0;            // 0 off | 1 last bar | 2 every 4-bar phrase end
    bool wantOpenHat = false; // a dedicated open-hat channel exists
    bool wantPerc = false;    // a perc channel exists
    uint32_t rhythmSeed = 1;  // pattern dice (canon mutations, ghost cells)
    uint32_t auxSeed = 2;     // flavour dice (velocity jitter, fill variant)
    int varyCount = 0;        // VARY = reroll mutations within the canon
    // [r22] the style DATA (GenStyle registry entry, incl. user styles); nullptr = the
    // embedded factory style at Options.style (headless callers need no registry).
    const GenStyle::Style* dna = nullptr;
};

struct Out
{
    std::vector<Hit> lane[NUM_LANES];
    float swing = 0.0f;       // pattern swing field value (0 = straight; % = 50 + 25 * swing)
    // [r22 STAGE 3] per-lane mined push/drag in ms (negative = early), already Humanize-gated
    // (Off = 0, Subtle = half the mined value, Loose = full). The step writer converts to
    // stepNudge and ANCHORS THE KICK to the grid (G5: the bass may never lead the kick).
    float laneMicroMs[NUM_LANES] = {};
};

namespace detail
{
// hat velocity template per tier (D3), scaled so the tier's mean lands on the genre's mined mean
static inline float hatShape(int tier, int cell, int idx)
{
    if (tier == 2) { static const float t[4] = { 105.0f, 60.0f, 85.0f, 60.0f };
                     return t[cell % 4] / 127.0f / 0.610f; }
    if (tier == 1) return ((cell % 4) == 0 ? 100.0f : 70.0f) / 127.0f / 0.669f;
    return (idx % 2 == 0 ? 88.0f : 78.0f) / 127.0f / 0.654f;   // offbeat 8ths: gentle alternation
}
static inline void tierCells(int tier, std::vector<int>& out)
{
    out.clear();
    if      (tier == 2) for (int c = 0; c < 16; ++c)      out.push_back(c);
    else if (tier == 1) for (int c = 0; c < 16; c += 2)   out.push_back(c);
    else                for (int c = 2; c < 16; c += 4)   out.push_back(c);
}
} // namespace detail

// ================================================================================================
static inline Out generate(const Options& oIn)
{
    using PartGen::Rng;
    Options o = oIn;
    if (o.style < 0 || o.style >= NUM_STYLES) o.style = StHouse;
    o.bars = o.bars < 1 ? 1 : (o.bars > PartGen::MAX_BARS ? PartGen::MAX_BARS : o.bars);
    // [r22] the style is DATA: the caller's registry entry, or the embedded factory fallback
    const GenStyle::Style& d = o.dna != nullptr ? *o.dna : GenStyle::factory(o.style);
    Out out;
    out.swing = (d.swingPct - 50.0f) / 25.0f;
    if (out.swing < 0.0f) out.swing = 0.0f;
    {   // [r22 STAGE 3] mined per-role microtiming, Humanize-gated (Off/half/full)
        const float mg = o.humanize == 0 ? 0.0f : (o.humanize == 1 ? 0.5f : 1.0f);
        out.laneMicroMs[LKick]    = d.microKick  * mg;
        out.laneMicroMs[LSnare]   = d.microSnare * mg;
        out.laneMicroMs[LHat]     = d.microHat   * mg;
        out.laneMicroMs[LOpenHat] = d.microOHat  * mg;
        out.laneMicroMs[LPerc]    = d.microPerc  * mg;
    }

    // pattern dice: rhythm seed + varyCount = "reroll the mutations, keep the canon" (D1)
    Rng mr(o.rhythmSeed ^ (0x9e3779b9u * (uint32_t) (o.varyCount + 1)));
    Rng ar(o.auxSeed ^ 0xD00DFEEDu);   // flavour dice: jitter + fill variant + ghost velocity

    // ---- the ONE-BAR grids (a groove is a loop; fills replace phrase-final bars) -------------
    // KICK = canon + up to 2 optional cells (never more - D1's mutation cap)
    bool kick[16] = {};
    for (int c = 0; c < 16; ++c) kick[c] = (d.kickCanon >> c) & 1;
    {
        std::vector<int> opt;
        for (int c = 0; c < 16; ++c) if ((d.kickOpt >> c) & 1) opt.push_back(c);
        int nAdd = opt.empty() ? 0 : (d.kickOptN >= 2 && (o.density >= 2 || mr.chance(0.6f)) ? 2 : 1);
        if (nAdd > (int) opt.size()) nAdd = (int) opt.size();
        for (int k = 0; k < nAdd && ! opt.empty(); ++k)
        {
            const int pick = mr.ri((int) opt.size());
            kick[opt[(size_t) pick]] = true;
            opt.erase(opt.begin() + pick);
        }
    }
    // SNARE backbeat + GHOSTS (D2 + D6)
    bool snare[16] = {}; float snareV[16] = {};
    for (int c = 0; c < 16; ++c)
        if ((d.snareCells >> c) & 1) { snare[c] = true; snareV[c] = d.snareVel; }
    {
        int budget = d.ghostBudget + (o.density - 1);
        if (budget < 0) budget = 0; if (budget > 3) budget = 3;
        std::vector<int> gc;
        for (int c : { 3, 6, 7, 11, 15 }) if (! snare[c]) gc.push_back(c);
        for (int k = 0; k < budget && ! gc.empty(); ++k)
        {
            const int pick = mr.ri((int) gc.size());
            const int cell = gc[(size_t) pick];
            gc.erase(gc.begin() + pick);
            snare[cell] = true;
            snareV[cell] = d.snareVel * (d.ghostRatio + (ar.uf() - 0.5f) * 0.06f);   // mined ~25%
        }
    }
    // HAT tier cells + template velocities (D3); ratchet ROLLS come from the style data (D5)
    std::vector<int> hatCells; detail::tierCells(d.hatTier, hatCells);
    // OPEN HAT cells (D4): openhat all = every 8th offbeat; sparse = 1-2 seed-picked offbeats
    std::vector<int> openCells;
    if (o.wantOpenHat)
    {
        if (d.openHatAll) openCells = { 2, 6, 10, 14 };
        else { openCells.push_back(6); if (mr.chance(0.5f)) openCells.push_back(14); }
        // the open hat REPLACES the closed hit at its cell (a real hand only plays one)
        for (int oc : openCells)
            hatCells.erase(std::remove(hatCells.begin(), hatCells.end(), oc), hatCells.end());
        // an all-offbeat closed lane would now be EMPTY - play quiet on-beats under the opens
        if (hatCells.empty()) hatCells = { 0, 4, 8, 12 };
    }
    // [r22 originality] the seeded HAT ORNAMENT layer: New idea varies the hat LANGUAGE while
    // the canon core stays identical (the mandate: a house 4-floor never moves - the hats do).
    // Sparse tiers gain 0-2 QUIET push cells (16th 'a' ghosts), the 16th tier DROPS 0-2 weak
    // cells (hat gaps; never the beat anchors, never the style's roll cells). Deterministic
    // from the rhythm seed; the D3 velocity law still runs last.
    std::vector<int> hatQuiet;
    {
        const int nOrn = mr.ri(3);
        if (d.hatTier == 2)
        {
            std::vector<int> weak = { 1, 3, 5, 9, 13 };
            weak.erase(std::remove_if(weak.begin(), weak.end(), [&](int w)
                       { return ((d.hatRollAlways | d.hatRollMaybe) >> w) & 1; }), weak.end());
            for (int k = 0; k < nOrn && ! weak.empty(); ++k)
            {
                const int pick = mr.ri((int) weak.size());
                hatCells.erase(std::remove(hatCells.begin(), hatCells.end(), weak[(size_t) pick]),
                               hatCells.end());
                weak.erase(weak.begin() + pick);
            }
        }
        else
        {
            std::vector<int> extra = { 3, 7, 11, 15 };
            auto drop = [&](int cell)
            { extra.erase(std::remove(extra.begin(), extra.end(), cell), extra.end()); };
            for (int oc : openCells) drop(oc);
            for (int hc : hatCells)  drop(hc);
            for (int k = 0; k < nOrn && ! extra.empty(); ++k)
            {
                const int pick = mr.ri((int) extra.size());
                hatQuiet.push_back(extra[(size_t) pick]);
                hatCells.push_back(extra[(size_t) pick]);
                extra.erase(extra.begin() + pick);
            }
            std::sort(hatCells.begin(), hatCells.end());
        }
    }
    // PERC layer (D7): sparse, never duplicating the kick/snare accent cells
    std::vector<int> percCells;
    if (o.wantPerc)
    {
        std::vector<int> free;
        for (int c = 0; c < 16; ++c) if (! kick[c] && ! snare[c]) free.push_back(c);
        const int nP = 2 + (o.density >= 2 ? 2 : o.density >= 1 ? 1 : 0);
        for (int k = 0; k < nP && ! free.empty(); ++k)
        {
            const int pick = mr.ri((int) free.size());
            percCells.push_back(free[(size_t) pick]);
            free.erase(free.begin() + pick);
        }
        std::sort(percCells.begin(), percCells.end());
    }
    // D5: the roll dice are consumed ONCE per maybe-cell (not per bar) so the bar loop stays
    // a loop; a '?' cell rolls with p = 0.5 for the whole take (the old trap behaviour).
    uint16_t rollMask = d.hatRollAlways;
    for (int c = 0; c < 16; ++c)
        if ((d.hatRollMaybe >> c) & 1 && mr.chance(0.5f)) rollMask = (uint16_t) (rollMask | (1u << c));

    // ---- emit per bar (fill bars get the D8 grammar) ------------------------------------------
    const float iMul = o.intensity == 0 ? 0.78f : (o.intensity == 2 ? 1.18f : 1.0f);
    const float iOfs = o.intensity == 0 ? -0.02f : (o.intensity == 2 ? 0.03f : 0.0f);
    const float jAmt = o.humanize == 0 ? 0.0f : (o.humanize == 1 ? 0.35f : 0.70f);
    auto vel = [&](float base, float sdMidi) -> float
    {   // intensity level + mined-sd humanize jitter (deterministic - ar)
        float v = base * iMul + iOfs;
        if (jAmt > 0.0f) v += (ar.uf() - 0.5f) * 2.0f * (sdMidi / 127.0f) * jAmt;
        return v < 0.05f ? 0.05f : (v > 1.0f ? 1.0f : v);
    };
    // ONE fill per 4-bar phrase end (or the last bar) - D8 / the panel's Fills dial
    auto isFillBar = [&](int b) -> bool
    {
        if (o.fills <= 0) return false;
        if (b == o.bars - 1) return true;
        return o.fills >= 2 && (b % 4) == 3;
    };
    const int fillVariant = d.fillVariant >= 0 ? d.fillVariant
                                               : (ar.chance(0.6f) ? 0 : 1);   // 0 crescendo | 1 doubling | 2 hat rolls

    for (int b = 0; b < o.bars; ++b)
    {
        const int base = b * PartGen::COLS;
        const bool fill = isFillBar(b);
        const bool suppressTail = fill && fillVariant == 0;   // crescendo owns the last beat (D8)
        for (int c = 0; c < 16; ++c)
            if (kick[c]) out.lane[LKick].push_back({ base + c * PartGen::CELL16,
                                                     vel(d.kickVel + (c == 0 ? 0.06f : 0.0f), d.sdKick), 1, 0.0f });
        for (int c = 0; c < 16; ++c)
        {
            if (! snare[c]) continue;
            if (suppressTail && c >= 12) continue;             // the crescendo replaces the tail
            out.lane[LSnare].push_back({ base + c * PartGen::CELL16, vel(snareV[c], d.sdSnare), 1, 0.0f });
        }
        int hi = 0;
        for (int c : hatCells)
        {
            if (suppressTail && c >= 12) { ++hi; continue; }   // hats duck under the crescendo
            const bool quiet = std::find(hatQuiet.begin(), hatQuiet.end(), c) != hatQuiet.end();
            Hit h { base + c * PartGen::CELL16,
                    vel(d.hatVel * detail::hatShape(d.hatTier, c, hi) * (quiet ? 0.55f : 1.0f), d.sdHat), 1, 0.0f };
            // D5: the style's canonical roll cells (trap: half-beat before the snare + bar end)
            if ((rollMask >> c) & 1)
            { h.roll = mr.chance(0.4f) ? 4 : 3; h.rollDec = 0.6f; }
            if (fill && fillVariant == 2 && c >= 12)           // hat-roll fill: bar-end roll ramp
            { h.roll = c >= 14 ? 6 : 4; h.rollDec = 0.7f; }
            out.lane[LHat].push_back(h);
            ++hi;
        }
        if (fill && fillVariant == 0)
            for (int c = 12; c < 16; ++c)                      // snare crescendo 50 -> 120 (D8)
                out.lane[LSnare].push_back({ base + c * PartGen::CELL16,
                                             vel(0.42f + 0.17f * (float) (c - 12), d.sdSnare), 1, 0.0f });
        if (fill && fillVariant == 1)
        {   // subdivision doubling: the last half-bar's hats become 16ths (D8)
            std::vector<int> have;
            for (const auto& h : out.lane[LHat])
                if (h.col >= base + 8 * PartGen::CELL16 && h.col < base + PartGen::COLS)
                    have.push_back((h.col - base) / PartGen::CELL16);
            for (int c = 8; c < 16; ++c)
                if (std::find(have.begin(), have.end(), c) == have.end())
                    out.lane[LHat].push_back({ base + c * PartGen::CELL16,
                                               vel(d.hatVel * detail::hatShape(2, c, c), d.sdHat), 1, 0.0f });
        }
        int oi = 0;
        for (int c : openCells)
        { out.lane[LOpenHat].push_back({ base + c * PartGen::CELL16,
                                         vel(d.hatVel * 1.25f + (oi % 2 == 0 ? 0.02f : -0.02f), 29.0f), 1, 0.0f }); ++oi; }
        for (int c : percCells)
            out.lane[LPerc].push_back({ base + c * PartGen::CELL16,
                                        vel(0.5f + (c % 2 ? -0.04f : 0.04f), 26.0f), 1, 0.0f });
    }

    // D3's law, enforced last: NEVER two consecutive equal velocities in a lane
    for (auto& ln : out.lane)
    {
        std::sort(ln.begin(), ln.end(), [](const Hit& a, const Hit& b) { return a.col < b.col; });
        for (size_t i = 1; i < ln.size(); ++i)
            if (std::fabs(ln[i].vel - ln[i - 1].vel) < 0.005f)
                ln[i].vel = ln[i].vel > 0.10f ? ln[i].vel - 0.03f : ln[i].vel + 0.03f;
    }
    return out;
}

// [r20, E -> r22 data] the mined ACCENT table for melodic velocities lives in the STYLE now
// (docs/generate-calibration.md "Accent structure" rows, mapped per genre in the factory
// texts). These shims keep the factory-index callers (tests) on one line.
static inline const uint8_t* accentTable(int style)
{ return GenStyle::factory(style).accent; }
static inline const GenStyle::Style& dnaFor(int style)
{ return GenStyle::factory(style); }

// [r20, F] STYLE SKELETON for the melodic roles: an empty context pulls the style's CANON kit
// (no mutations, no ghosts - the deterministic identity grid) as the VIRTUAL groove, so
// from-scratch melodies/basslines phrase against the style instead of the bare meter.
static inline void applyStyleSkeleton(const GenStyle::Style& d, PartGen::Ctx& c)
{
    std::vector<int> hats; detail::tierCells(d.hatTier, hats);
    c.nHits = c.nKick = c.nSnare = c.nHat = 0;
    c.latticeN = 16;   // [r21] the style canon IS a 16th grid - the virtual groove's lattice
    auto pushHit = [&](int col, float str, int* n, int* colA, float* strA)
    {
        if (*n < PartGen::Ctx::MAX_HITS) { colA[*n] = col; strA[*n] = str; ++(*n); }
        int f = -1;
        for (int h = 0; h < c.nHits; ++h) if (c.hitCol[h] == col) { f = h; break; }
        if (f >= 0) { if (str > c.hitStr[f]) c.hitStr[f] = str; }
        else if (c.nHits < PartGen::Ctx::MAX_HITS)
        { c.hitCol[c.nHits] = col; c.hitStr[c.nHits] = str; ++c.nHits; }
    };
    for (int b = 0; b < c.bars; ++b)
    {
        const int base = b * PartGen::COLS;
        for (int cell = 0; cell < 16; ++cell)
        {
            if ((d.kickCanon >> cell) & 1)
                pushHit(base + cell * PartGen::CELL16, d.kickVel, &c.nKick, c.kickCol, c.kickStr);
            if ((d.snareCells >> cell) & 1)
                pushHit(base + cell * PartGen::CELL16, d.snareVel, &c.nSnare, c.snareCol, c.snareStr);
            c.grooveHit[b * 16 + cell] = 0.0f;
        }
        int hi = 0;
        for (int cell : hats)
        { pushHit(base + cell * PartGen::CELL16, d.hatVel * detail::hatShape(d.hatTier, cell, hi),
                  &c.nHat, c.hatCol, c.hatStr); ++hi; }
        for (int cell = 0; cell < 16; ++cell)
        {
            float s = 0.0f;
            if ((d.kickCanon >> cell) & 1)  s = d.kickVel;
            if ((d.snareCells >> cell) & 1) s = s > d.snareVel ? s : d.snareVel;
            c.grooveHit[b * 16 + cell] = s;
        }
    }
    auto sortHits = [](int n, int* cols, float* strs)
    {
        for (int a = 1; a < n; ++a)
            for (int b2 = a; b2 > 0 && cols[b2] < cols[b2 - 1]; --b2)
            { std::swap(cols[b2], cols[b2 - 1]); std::swap(strs[b2], strs[b2 - 1]); }
    };
    sortHits(c.nHits, c.hitCol, c.hitStr);
    sortHits(c.nKick, c.kickCol, c.kickStr);
    sortHits(c.nSnare, c.snareCol, c.snareStr);
    sortHits(c.nHat, c.hatCol, c.hatStr);
}
static inline void applyStyleSkeleton(int style, PartGen::Ctx& c)   // factory-index shim (tests)
{ applyStyleSkeleton(GenStyle::factory(style), c); }
} // namespace DrumGen
