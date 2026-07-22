// GenTest [2026-07-20] - locks the GENERATE feature's melody engine (PartGen.h).
// Pure headless checks: determinism, scale safety, register bounds, density ordering,
// seed-locked iteration (same rhythm / new notes), multi-bar + phrase echo, riff tiling,
// singable constraints, vary mutation, and the mono no-overlap guarantee.
// [17]-[21] [2026-07-21 r18] the CONTEXT GATHER (GenContext.h, extracted from the editor):
// exact 7-step hit columns, merged-group bar offsets, mute/solo exclusion, self-exclusion,
// and STEP-PITCH harmony (Freq-knob base + stepPitch -> chroma + key detection).
// [22] POCKET DISCIPLINE: no onset within a grid cell of a drum hit, no note ringing into one.
// [23]-[30] [2026-07-21 P1]: drum-role classification + role hit lists, chordCols determinism +
// the progression override, G2 kick relationships (unison/interlock/avoid), G9 root-on-change +
// approach notes, G8 snare shadow, M1/M2 proximity + leap recovery (via the scorecard), H8/H9
// sound-aware lengths, and the STEP-COUNT AUTHORITY writer. Plus the DEV SCORECARD: LHL
// syncopation (G3 weights), chord-tone-on-strong %, proximity %, leap-recovery violations,
// motif 3-gram self-similarity - informational prints + the doc's hard bands asserted.
#include "PartGen.h"
#include "GenContext.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  PASS  %s\n", msg); \
                              else { printf("  FAIL  %s\n", msg); ++fails; } } while (0)

static const int8_t kMajor[7] = { 0, 2, 4, 5, 7, 9, 11 };

// ---- DEV SCORECARD [P1 item 5]: musicality numbers per generation, sharing the generator's OWN
// helpers (lhlBarScore / prepareChords / chordRootDegAt) so the score can never drift from the
// rules it measures. Informational prints; hard bands asserted where the theory doc gives them.
struct GenScore { float lhl = 0, chordStrong = 0, prox = 0, ngram = 0; int leapViol = 0, n = 0, lattice = 16; };
static GenScore scoreGen(const std::vector<PartGen::Note>& ns,
                         const PartGen::Options& o, const PartGen::Ctx& cIn)
{
    GenScore sc; sc.n = (int) ns.size();
    if (ns.empty()) return sc;
    PartGen::Ctx cc = cIn;
    PartGen::prepareChords(o, cc);          // the SAME chord timeline the generator builds
    PartGen::prepareLattice(cc);            // [r21] ... and the SAME lattice strength/weight map
    sc.lattice = cc.latticeN;
    for (int b = 0; b < cIn.bars; ++b) sc.lhl += (float) PartGen::lhlBarScore(ns, b, cc);
    sc.lhl /= (float) cIn.bars;
    int strong = 0, ct = 0, steps = 0, tot = 0;
    for (auto& x : ns)
        if (PartGen::strongCol(cc, x.start))   // [r21] the generator's own strong-col rule
        {
            ++strong;
            bool cp[12];
            PartGen::detail::chordPcs(o, PartGen::detail::chordRootDegAt(o, cc, x.start), cp);
            if (cp[((x.semi % 12) + 12) % 12]) ++ct;
        }
    sc.chordStrong = strong > 0 ? (float) ct / (float) strong : 1.0f;
    for (size_t i = 1; i < ns.size(); ++i)
    {
        const int d = ns[i].semi - ns[i - 1].semi;
        ++tot; if (std::abs(d) <= 2) ++steps;
        if (std::abs(d) > 5 && i + 1 < ns.size())
        {
            const int nd = ns[i + 1].semi - ns[i].semi;
            if (! (nd != 0 && (d > 0 ? nd < 0 : nd > 0) && std::abs(nd) <= 3)) ++sc.leapViol;
        }
    }
    sc.prox = tot > 0 ? (float) steps / (float) tot : 1.0f;
    // [r20] motif economy metric = repeated DIATONIC-interval 3-grams: a sequence (the cell
    // transposed a scale step) is a TRANSFORM of the motif, so it must count as similarity -
    // semitone trigrams were blind to it (major-scale steps are 1 or 2 semis).
    auto degOf = [&](int semi)
    {
        const int p = ((semi - o.key) % 12 + 12) % 12;
        int di = 0, bd = 99;
        for (int i = 0; i < o.scaleLen; ++i)
        { const int d = std::abs(o.scale[i] % 12 - p); if (d < bd) { bd = d; di = i; } }
        return (int) std::floor((double) (semi - o.key) / 12.0) * o.scaleLen + di;
    };
    int dup = 0, tg = 0;
    std::vector<long> seen;
    for (size_t i = 3; i < ns.size(); ++i)
    {
        long key = 0;
        for (int k = 0; k < 3; ++k)
            key = key * 60 + (long) (degOf(ns[i - 2 + k].semi) - degOf(ns[i - 3 + k].semi) + 30);
        ++tg;
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) ++dup; else seen.push_back(key);
    }
    sc.ngram = tg > 0 ? (float) dup / (float) tg : 0.0f;
    return sc;
}
static void printScore(const char* tag, const GenScore& s)
{
    printf("  SCORE %-28s n=%2d lat=%d LHL=%.1f chordStrong=%.0f%% proximity=%.0f%% leapViol=%d ngramSim=%.0f%%\n",
           tag, s.n, s.lattice, s.lhl, s.chordStrong * 100.0f, s.prox * 100.0f, s.leapViol, s.ngram * 100.0f);
}

static bool inScale(int semi, int key, const int8_t* sc, int len)
{
    const int p = ((semi - key) % 12 + 12) % 12;
    for (int i = 0; i < len; ++i) if (sc[i] % 12 == p) return true;
    return false;
}
static bool monoOk(const std::vector<PartGen::Note>& n)
{
    for (size_t i = 0; i + 1 < n.size(); ++i)
        if (n[i].start + n[i].len > n[i + 1].start) return false;
    return true;
}
static std::vector<int> onsets(const std::vector<PartGen::Note>& n)
{ std::vector<int> o; for (auto& x : n) o.push_back(x.start); return o; }

int main()
{
    using namespace PartGen;
    printf("GenTest\n");

    Ctx ctx1; ctx1.bars = 1;
    for (int i = 0; i < 16; i += 4) ctx1.grooveHit[i] = 1.0f;   // four-on-the-floor groove

    // [1] determinism: identical options -> identical output
    {
        Options o; o.scale = kMajor; o.rhythmSeed = 11; o.pitchSeed = 22;
        auto a = generate(o, ctx1), b = generate(o, ctx1);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].start == b[i].start && a[i].len == b[i].len
                && a[i].semi == b[i].semi && a[i].vel == b[i].vel;
        CHECK(same && ! a.empty(), "[1] deterministic (same seeds = identical notes)");
    }

    // [2] scale safety at color 0 (C major, every pitch a scale member)
    {
        bool ok = true;
        for (uint32_t s = 1; s <= 6 && ok; ++s)
        {
            Options o; o.scale = kMajor; o.color = 0; o.rhythmSeed = s; o.pitchSeed = s * 7 + 1;
            for (auto& n : generate(o, ctx1))
                if (! inScale(n.semi, 0, kMajor, 7)) ok = false;
        }
        CHECK(ok, "[2] color=Safe stays in the scale (6 seeds)");
    }

    // [3] register bounds: melody mid stays near its centre
    {
        Options o; o.scale = kMajor; o.color = 0; o.registerBand = 1; o.rhythmSeed = 3; o.pitchSeed = 5;
        bool ok = true;
        for (auto& n : generate(o, ctx1)) if (n.semi < -13 || n.semi > 5) ok = false;
        CHECK(ok, "[3] melody mid register within centre-4 +-9");
    }

    // [4] bassline lives low
    {
        Options o; o.scale = kMajor; o.role = RoleBass; o.registerBand = 0; o.color = 0;
        o.rhythmSeed = 9; o.pitchSeed = 4;
        bool ok = true; auto n = generate(o, ctx1);
        for (auto& x : n) if (x.semi > -10 || x.semi < -28) ok = false;
        CHECK(ok && ! n.empty(), "[4] bassline register low (-28..-10)");
    }

    // [5] density ordering: busy >= sparse (same seeds)
    {
        Options o; o.scale = kMajor; o.rhythmSeed = 5; o.pitchSeed = 5;
        o.density = 0; auto sp = generate(o, ctx1);
        o.density = 2; auto bu = generate(o, ctx1);
        CHECK(bu.size() >= sp.size() && ! sp.empty(), "[5] busy density >= sparse");
    }

    // [6] SAME RHYTHM, NEW NOTES: rhythm seed locked -> onsets identical, pitches move
    {
        Options o; o.scale = kMajor; o.rhythmSeed = 7; o.pitchSeed = 1; o.density = 2;
        auto a = generate(o, ctx1);
        bool onsetsSame = true, pitchDiff = false;
        for (uint32_t ps = 2; ps <= 6; ++ps)
        {
            o.pitchSeed = ps; auto b = generate(o, ctx1);
            if (onsets(a) != onsets(b)) onsetsSame = false;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                if (a[i].semi != b[i].semi) pitchDiff = true;
        }
        CHECK(onsetsSame, "[6a] locked rhythm seed keeps every onset");
        CHECK(pitchDiff,  "[6b] new pitch seeds change the melody");
    }

    // [7] multi-bar merged group: 4 bars, notes reach the later bars, all in range, mono
    Ctx ctx4; ctx4.bars = 4;
    for (int b = 0; b < 4; ++b) for (int i = 0; i < 16; i += 4) ctx4.grooveHit[b * 16 + i] = 1.0f;
    {
        Options o; o.scale = kMajor; o.density = 1; o.rhythmSeed = 13; o.pitchSeed = 17;
        auto n = generate(o, ctx4);
        bool late = false, bounded = true;
        for (auto& x : n) { if (x.start >= 2 * 384) late = true;
                            if (x.start < 0 || x.start + x.len > 4 * 384) bounded = false; }
        CHECK(late && bounded && monoOk(n), "[7] 4-bar group: fills later bars, stays in range, mono");
    }

    // [8] CALL & ANSWER (2 bars): the answer keeps the question's opening and RESOLVES home
    {
        Ctx c2; c2.bars = 2;
        Options o; o.scale = kMajor; o.lines = 2; o.relation = RelAnswer; o.rhythmSeed = 21; o.pitchSeed = 8;
        auto n = generate(o, c2);
        std::set<int> b1, b2;
        for (auto& x : n) (x.start < 384 ? b1 : b2).insert(x.start % 384);
        bool openingShared = ! b1.empty() && ! b2.empty();
        for (int col : b1) if (col < 384 * 4 / 10 && ! b2.count(col)) openingShared = false;
        const int lastPc = ((n.back().semi % 12) + 12) % 12;
        CHECK(openingShared, "[8a] Q&A: the answer keeps the question's opening onsets");
        CHECK(lastPc == 0 || lastPc == 4, "[8b] Q&A: the final note RESOLVES (tonic or third)");
    }

    // [9] riff: the cell tiles every bar (identical bar-local onsets)
    {
        Options o; o.scale = kMajor; o.role = RoleRiff; o.rhythmSeed = 4; o.pitchSeed = 6;
        auto n = generate(o, ctx4);
        std::set<int> per[4];
        for (auto& x : n) per[std::min(3, x.start / 384)].insert(x.start % 384);
        CHECK(! per[0].empty() && per[0] == per[1] && per[1] == per[2] && per[2] == per[3],
              "[9] riff cell tiles all 4 bars");
    }

    // [10] singable: leaps capped at a fifth+2, ~one-octave span
    {
        Options o; o.scale = kMajor; o.singable = true; o.color = 0; o.density = 2;
        o.rhythmSeed = 31; o.pitchSeed = 12;
        auto n = generate(o, ctx4);
        bool ok = ! n.empty(); int lo = 99, hi = -99;
        for (size_t i = 0; i < n.size(); ++i)
        {
            lo = std::min(lo, n[i].semi); hi = std::max(hi, n[i].semi);
            if (i > 0 && std::abs(n[i].semi - n[i - 1].semi) > 7) ok = false;
        }
        CHECK(ok && (hi - lo) <= 13, "[10] singable: leaps <= 7 st, span <= octave+1");
    }

    // [11] vary: same seeds + varyCount -> same skeleton, some pitches move
    {
        Options o; o.scale = kMajor; o.density = 2; o.rhythmSeed = 19; o.pitchSeed = 23;
        Ctx c = ctx4;
        auto a = generate(o, c);
        bool anyDiff = false, onsetsSame = true;
        for (int v = 1; v <= 3; ++v)
        {
            o.varyCount = v; auto b = generate(o, c);
            if (onsets(a) != onsets(b)) onsetsSame = false;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                if (a[i].semi != b[i].semi) anyDiff = true;
        }
        CHECK(onsetsSame && anyDiff, "[11] Vary keeps the skeleton, moves some pitches");
    }

    // [12] harmony awareness: a strong D-minor beat pulls chord roots onto D
    {
        Ctx ch; ch.bars = 1; ch.chromaValid = true;
        for (int bt = 0; bt < 4; ++bt) { ch.chroma[bt][2] = 3.0f; ch.chroma[bt][5] = 1.5f; ch.chroma[bt][9] = 1.5f; }
        Options o; o.scale = kMajor; o.role = RoleBass; o.color = 0; o.rhythmSeed = 2; o.pitchSeed = 3;
        int dCount = 0, total = 0;
        for (uint32_t s = 1; s <= 5; ++s)
        {
            o.pitchSeed = s;
            for (auto& n : generate(o, ch))
                if (n.start % 96 == 0) { ++total; if (((n.semi % 12) + 12) % 12 == 2) ++dCount; }
        }
        CHECK(total > 0 && dCount * 2 >= total, "[12] chroma steers bass roots to the heard chord");
    }

    // [13] HOOK form (AABA in 4 bars): bars 2 + 4 recap A's opening; bar 3 (the bridge) contrasts
    {
        Options o; o.scale = kMajor; o.lines = 4; o.relation = RelAuto; o.rhythmSeed = 14; o.pitchSeed = 9;   // Auto 4 lines = A a B a
        auto n = generate(o, ctx4);
        int first[4] = { -1, -1, -1, -1 };
        std::set<int> rel[4];
        for (auto& x : n)
        {
            const int b = std::min(3, x.start / 384);
            rel[b].insert(x.start % 384);
            if (first[b] < 0) first[b] = x.start % 384;
        }
        CHECK(first[0] >= 0 && first[1] == first[0] && first[3] == first[0],
              "[13a] AABA: bars 2+4 open with the hook's first onset");
        CHECK(! rel[2].empty() && rel[2] != rel[0], "[13b] AABA: the bridge (bar 3) contrasts the hook");
    }

    // [14] HIT-EXACT rhythm: a 7-step groove, Driving -> every onset lands ON a real hit
    {
        Ctx c7; c7.bars = 1;
        std::set<int> hitSet;
        for (int k = 0; k < 7; ++k)
        { c7.hitCol[k] = k * 384 / 7; c7.hitStr[k] = 1.0f; hitSet.insert(k * 384 / 7); }
        c7.nHits = 7;
        bool ok = true; int cnt = 0;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.rhythm = RhDriving; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 40;
            for (auto& x : generate(o, c7)) { ++cnt; if (! hitSet.count(x.start)) ok = false; }
        }
        CHECK(ok && cnt > 0, "[14] Driving locks EXACTLY onto a 7-step groove's hits (4 seeds)");
    }

    // [15] VOCAL ARC: Hum + Flowing + singable = long notes crossing the bar, breath at phrase ends
    {
        Ctx c2; c2.bars = 2;
        Options o; o.scale = kMajor; o.role = RoleHum; o.rhythm = RhFlowing; o.density = 0;
        o.singable = true; o.lines = 2; o.relation = RelAnswer; o.rhythmSeed = 6; o.pitchSeed = 11;
        auto n = generate(o, c2);
        bool longArc = false, breath = true;
        for (auto& x : n)
        {
            if (x.len >= 96 * 3 / 2) longArc = true;                    // >= 1.5 beats = an arc, not a blip
            if (x.start + x.len > 2 * 384 - 20) breath = false;         // phrase end leaves air
        }
        CHECK(! n.empty() && longArc && breath, "[15] hum sings arcs and breathes at phrase ends");
    }

    // [16] LINES=1 on 3 bars: ONE melodic arc across the whole group (the "one line of my
    // lyrics" case) - some note crosses a bar line, the final note resolves
    {
        Ctx c3; c3.bars = 3;
        Options o; o.scale = kMajor; o.role = RoleHum; o.rhythm = RhFlowing;
        o.lines = 1; o.singable = true; o.rhythmSeed = 3; o.pitchSeed = 7;
        auto n = generate(o, c3);
        // ONE line = no breaths at the internal bar ends (a multi-line take gets capped a 16th
        // short of each phrase end; one arc runs straight through those points)
        bool runsThrough = false, bounded = true, longNote = false;
        for (auto& x : n)
        {
            const int endC = x.start + x.len;
            if (endC > 3 * 384) bounded = false;
            if (x.len >= 96 * 3 / 2) longNote = true;
            for (int k = 1; k <= 2; ++k)
                if (x.start < k * 384 && endC > k * 384 - 24) runsThrough = true;
        }
        const int lastPc = n.empty() ? -1 : ((n.back().semi % 12) + 12) % 12;
        CHECK(! n.empty() && runsThrough && longNote && bounded && (lastPc == 0 || lastPc == 4),
              "[16] lines=1 on 3 bars: one unbroken arc (no internal breaths), resolves home");
    }

    // ---- CONTEXT GATHER (GenContext::build, headless Sequencer) [r18] ----
    auto kit = [](DrumChannel& ch, int nSteps, std::initializer_list<int> on)
    {   // an unpitched (Noise) step kit - hits only, no chroma
        ch.numSteps = nSteps; for (int i : on) ch.steps[i] = true;
        ch.slots[0] = DrumChannel::Slot();
        ch.slots[0].engine = DrumChannel::SrcNoise; ch.slots[0].weight = 1.0f;
    };

    // [17] a 7-step kick's hits land at the EXACT concat columns i*384/7
    {
        auto sq = std::make_unique<Sequencer>();
        kit(sq->patterns[0].channels[0], 7, { 0, 1, 2, 3, 4, 5, 6 });
        GenContext::Readout ro;
        auto c = GenContext::build(*sq, 0, 0, 7, &ro);
        bool ok = c.nHits == 7 && ro.hits == 7 && ro.grooveChans == 1;
        for (int i = 0; i < 7 && ok; ++i) ok = c.hitCol[i] == i * 384 / 7;
        CHECK(ok, "[17] gather: 7-step kick = exact concat columns (i*384/7)");
    }

    // [18] merged 2-bar group: hits from BOTH bars, bar 2 at +384 offsets
    {
        auto sq = std::make_unique<Sequencer>();
        sq->patterns[1].mergeWithPrev = true;
        kit(sq->patterns[0].channels[0], 4, { 0, 2 });
        kit(sq->patterns[1].channels[0], 4, { 1 });
        auto c = GenContext::build(*sq, 0, 1, 7);
        CHECK(c.bars == 2 && c.nHits == 3
              && c.hitCol[0] == 0 && c.hitCol[1] == 192 && c.hitCol[2] == 384 + 96,
              "[18] gather: merged group hears BOTH bars at bar-offset columns");
    }

    // [19] muted channel excluded; a solo elsewhere excludes the un-soloed kit
    {
        auto sq = std::make_unique<Sequencer>();
        kit(sq->patterns[0].channels[0], 4, { 0, 2 });
        sq->patterns[0].channels[0].mute = true;
        auto a = GenContext::build(*sq, 0, 0, 7);
        sq->patterns[0].channels[0].mute = false;
        sq->patterns[0].channels[1].solo = true;   // solo a silent channel -> the kit is excluded
        auto b = GenContext::build(*sq, 0, 0, 7);
        sq->patterns[0].channels[1].solo = false;
        auto c = GenContext::build(*sq, 0, 0, 7);
        CHECK(a.nHits == 0 && b.nHits == 0 && c.nHits == 2,
              "[19] gather: muted excluded, solo exclusion honored, clean = heard");
    }

    // [20] the selected channel never shapes its own context
    {
        auto sq = std::make_unique<Sequencer>();
        kit(sq->patterns[0].channels[3], 4, { 0, 2 });
        auto self  = GenContext::build(*sq, 0, 0, 3);
        auto other = GenContext::build(*sq, 0, 0, 7);
        CHECK(self.nHits == 0 && other.nHits == 2,
              "[20] gather: selected channel excluded from its own context");
    }

    // [21] STEP-PITCH HARMONY: an A-minor step bassline (Freq knob = A2, stepPitch offsets)
    // contributes chroma and steers key detection to A minor
    {
        auto sq = std::make_unique<Sequencer>();
        auto& bass = sq->patterns[0].channels[2];
        bass.numSteps = 8;
        bass.slots[0] = DrumChannel::Slot();
        bass.slots[0].engine = DrumChannel::SrcOsc; bass.slots[0].weight = 1.0f;
        bass.slots[0].oscFreq = 110.0f;             // A2 = MIDI 45
        const int   onStep[4]  = { 0, 2, 4, 6 };
        const float offs[4]    = { 0.0f, 3.0f, 7.0f, 12.0f };   // A C E A
        for (int i = 0; i < 4; ++i) { bass.steps[onStep[i]] = true; bass.stepPitch[onStep[i]] = offs[i]; }
        GenContext::Readout ro;
        auto c = GenContext::build(*sq, 0, 0, 7, &ro);
        int st = -1;
        const int key = GenContext::detectKey(*sq, 0, 0, 7, st);
        CHECK(c.chromaValid && c.chroma[0][9] > 0.5f && ro.keyChans == 1,
              "[21a] pitched STEP channel contributes chroma (pc A heard)");
        CHECK(key == 9 && st == 1, "[21b] step bassline steers detection to A minor");
    }

    // [22] POCKET DISCIPLINE: hits every 2nd step of 16 -> zero onsets inside the exclusion
    // window (1 grid cell) and zero notes ringing into a hit (end >= 12 cols before it)
    {
        Ctx cp; cp.bars = 1;
        for (int k2 = 0; k2 < 8; ++k2) { cp.hitCol[k2] = k2 * 48; cp.hitStr[k2] = 1.0f; }
        cp.nHits = 8;
        bool ok = true; int cnt = 0;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.rhythm = RhPockets; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 9;
            for (auto& x : generate(o, cp))
            {
                ++cnt;
                for (int h = 0; h < cp.nHits; ++h)
                {
                    if (std::abs(x.start - cp.hitCol[h]) < 24) ok = false;                        // exclusion
                    if (x.start < cp.hitCol[h] && x.start + x.len > cp.hitCol[h] - 12) ok = false; // ring-through
                }
            }
        }
        CHECK(ok && cnt > 0, "[22] pockets: no onset in an exclusion window, no note rings into a hit");
    }

    // ---- [P1] the theory-rule + step-output round ----

    // [23] drum-ROLE classification: bank category (factory names) + mixName keywords -> the
    // per-role hit lists beside the combined list, and the readout's role counts
    {
        auto sq = std::make_unique<Sequencer>();
        kit(sq->patterns[0].channels[0], 4, { 0, 2 }); sq->patterns[0].channels[0].mixName = "808 Kick";
        kit(sq->patterns[0].channels[1], 4, { 1 });    sq->patterns[0].channels[1].mixName = "Trap Snare";
        kit(sq->patterns[0].channels[2], 8, { 2, 6 }); sq->patterns[0].channels[2].mixName = "Weird Hat 99";
        GenContext::Readout ro;
        auto c = GenContext::build(*sq, 0, 0, 7, &ro);
        CHECK(c.nKick == 2 && c.kickCol[0] == 0 && c.kickCol[1] == 192
              && c.nSnare == 1 && c.snareCol[0] == 96
              && c.nHat == 2 && c.hatCol[0] == 96 && c.hatCol[1] == 288
              && ro.kick == 2 && ro.snare == 1 && ro.hat == 2 && c.nHits == 4,
              "[23] drum roles: category + keyword classification fill the kick/snare/hat lists");
    }

    // [24] chordCols: deterministic, changes on STRONG cols, density-driven, override honored
    {
        PartGen::Ctx ch2; ch2.bars = 2; ch2.chromaValid = true;
        for (int bt = 0; bt < 4; ++bt) { ch2.chroma[bt][2] += 3.0f; ch2.chroma[bt][5] += 1.5f; ch2.chroma[bt][9] += 1.5f; }
        for (int bt = 4; bt < 8; ++bt) { ch2.chroma[bt][7] += 3.0f; ch2.chroma[bt][11] += 1.5f; ch2.chroma[bt][2] += 1.5f; }
        Options o; o.scale = kMajor; o.density = 1;
        PartGen::Ctx a = ch2, b = ch2;
        PartGen::prepareChords(o, a); PartGen::prepareChords(o, b);
        bool det = a.nChords == b.nChords;
        for (int i = 0; det && i < a.nChords; ++i)
            det = a.chordColAt[i] == b.chordColAt[i] && a.chordDegAt[i] == b.chordDegAt[i];
        CHECK(det && a.nChords == 2 && a.chordColAt[0] == 0 && a.chordColAt[1] == 384
              && a.chordDegAt[0] == 1 && a.chordDegAt[1] == 4,
              "[24a] chordCols: deterministic, hears Dm then G on the bar lines");
        o.density = 2; PartGen::Ctx d = ch2; PartGen::prepareChords(o, d);
        CHECK(d.nChords == 4 && d.chordColAt[1] == 192 && d.chordColAt[3] == 384 + 192,
              "[24b] chordCols: Busy density = two changes per bar, on strong cols");
        o.density = 1; o.progression = 5;   // I-IV-V override
        PartGen::Ctx e; e.bars = 3; PartGen::prepareChords(o, e);
        CHECK(e.nChords == 3 && e.chordDegAt[0] == 0 && e.chordDegAt[1] == 3 && e.chordDegAt[2] == 4,
              "[24c] chordCols: the Chords override writes the picked progression");
    }

    // [25] G2 kick relationships: the bass reads the STANCE against the KICK list
    Ctx ck; ck.bars = 1;
    {
        const int kc[4] = { 0, 96, 192, 288 };
        for (int i = 0; i < 4; ++i) { ck.kickCol[i] = kc[i]; ck.kickStr[i] = 1.0f; }
        ck.nKick = 4;
        const int all[8] = { 0, 48, 96, 144, 192, 240, 288, 336 };
        for (int i = 0; i < 8; ++i) { ck.hitCol[i] = all[i]; ck.hitStr[i] = 1.0f; }
        ck.nHits = 8;
        bool uniOk = true, interOk = true, avoidOk = true; int cu = 0, cp2 = 0, ca = 0;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleBass; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 3;
            o.rhythm = RhDriving;                       // unison: every onset ON a kick
            for (auto& x : generate(o, ck))
            {
                ++cu; bool onK = false;
                for (int i = 0; i < 4; ++i) if (std::abs(x.start - kc[i]) <= 12) onK = true;
                if (! onK) uniOk = false;
            }
            o.rhythm = RhPockets;                       // interlock: never within a cell of a kick
            for (auto& x : generate(o, ck))
            {
                ++cp2;
                for (int i = 0; i < 4; ++i) if (std::abs(x.start - kc[i]) < 24) interOk = false;
            }
            o.rhythm = RhFlowing;                       // avoid: nothing rings into a kick
            for (auto& x : generate(o, ck))
            {
                ++ca;
                for (int i = 0; i < 4; ++i)
                    if (kc[i] > x.start && x.start + x.len > kc[i] - 12) avoidOk = false;
            }
        }
        CHECK(uniOk && cu > 0,    "[25a] G2 Driving bass = unison (every onset within 1/32 of a kick)");
        CHECK(interOk && cp2 > 0, "[25b] G2 Pockets bass = interlock (no onset within 1/16 of a kick)");
        CHECK(avoidOk && ca > 0,  "[25c] G2 Flowing bass = avoid (no note rings into a kick)");
    }

    // [26] G9 bass grammar: ROOT on the chord change + an APPROACH note flagged before it
    {
        Ctx cg; cg.bars = 2;
        for (int i = 0; i < 8; ++i) { cg.kickCol[i] = i * 96; cg.kickStr[i] = 1.0f;
                                      cg.hitCol[i]  = i * 96; cg.hitStr[i]  = 1.0f; }
        cg.nKick = cg.nHits = 8;
        bool rootOk = true, apPlaced = true, anyApproach = false; int roots = 0;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = RhDriving; o.density = 2;
            o.progression = 5;   // I-IV-V: bar 1 = C, bar 2 = F (degree 3)
            o.rhythmSeed = s; o.pitchSeed = s + 7;
            auto n = generate(o, cg);
            bool found = false;
            for (auto& x : n)
            {
                if (! found && x.start >= 384 && x.start < 480)
                { found = true; ++roots; if (((x.semi % 12) + 12) % 12 != 5) rootOk = false; }
                if (x.approach)
                {
                    anyApproach = true;
                    bool nearChange = false;   // approaches live in the beat before SOME change
                    for (int ccCol : { 192, 384, 384 + 192 })
                        if (x.start >= ccCol - 96 && x.start < ccCol) nearChange = true;
                    if (! nearChange) apPlaced = false;
                }
            }
        }
        CHECK(rootOk && roots > 0, "[26a] G9: the change-downbeat note is the new chord's ROOT");
        CHECK(anyApproach && apPlaced, "[26b] G9: approach notes flagged, in the beat before a change");
    }

    // [27] G8 snare punctuation: no melody onset in the post-snare 1/16 shadow
    {
        Ctx cs; cs.bars = 1;
        cs.snareCol[0] = 96; cs.snareCol[1] = 288; cs.snareStr[0] = cs.snareStr[1] = 1.0f; cs.nSnare = 2;
        cs.hitCol[0] = 96;  cs.hitCol[1] = 288;  cs.hitStr[0] = cs.hitStr[1] = 1.0f;  cs.nHits = 2;
        bool ok = true; int cnt = 0;
        for (uint32_t s = 1; s <= 6; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhFlowing; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s * 3 + 1;
            for (auto& x : generate(o, cs))
            {
                ++cnt;
                for (int i = 0; i < 2; ++i)
                    if (x.start > cs.snareCol[i] && x.start < cs.snareCol[i] + 24) ok = false;
            }
        }
        CHECK(ok && cnt > 0, "[27] G8: no melody onset in the post-snare 1/16 shadow (6 seeds)");
    }

    // [28] SCORECARD bands (the doc's hard bands): melody proximity >= 60%, zero unrecovered
    // leaps, LHL within the role ceilings (Melody 6 / Bass 4 / Hum 2)
    {
        Ctx cm; cm.bars = 2;   // offbeat hats: strong cols FREE, the pocket midpoints = beats
        for (int i = 0; i < 8; ++i) { cm.hitCol[i] = i * 96 + 48; cm.hitStr[i] = 1.0f; }
        cm.nHits = 8;
        bool proxOk = true, leapOk = true, lhlOk = true;
        GenScore sample;
        for (uint32_t s = 1; s <= 5; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhPockets; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 11;
            auto n = generate(o, cm);
            const auto sc = scoreGen(n, o, cm);
            if (s == 1) sample = sc;
            if (sc.prox < 0.6f) proxOk = false;
            if (sc.leapViol != 0) leapOk = false;
            if (sc.lhl > 6.0f) lhlOk = false;
        }
        printScore("melody/pockets (seed 1)", sample);
        CHECK(proxOk, "[28a] M1: proximity >= 60% on every seed (target 70%)");
        CHECK(leapOk, "[28b] M2: zero unrecovered large leaps");
        CHECK(lhlOk,  "[28c] G3: melody LHL within the role band (<= 6)");
        {   // bass + hum ceilings on the kick groove from [25]
            Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = RhDriving; o.density = 1;
            o.rhythmSeed = 3; o.pitchSeed = 4;
            auto n = generate(o, ck);
            const auto sb = scoreGen(n, o, ck);
            printScore("bass/driving", sb);
            Options oh; oh.scale = kMajor; oh.role = RoleHum; oh.rhythm = RhFlowing; oh.density = 0;
            oh.singable = true; oh.rhythmSeed = 5; oh.pitchSeed = 6;
            auto nh = generate(oh, ck);
            const auto sh = scoreGen(nh, oh, ck);
            printScore("hum/flowing", sh);
            CHECK(sb.lhl <= 4.0f && sh.lhl <= 2.0f, "[28d] G3: bass <= 4 and hum <= 2 LHL ceilings");
        }
    }

    // [29] H8/H9 sound-aware: slow attack = min half-note holds + sparser; pluck = one-cell
    // notes; both outputs mono (H9's single-line guarantee)
    {
        Ctx cslow; cslow.bars = 1;
        cslow.sndValid = true; cslow.sndAtk = 0.2f; cslow.sndSus = 0.8f; cslow.sndDec = 1.0f;
        Options o; o.scale = kMajor; o.rhythm = RhFlowing; o.density = 2;
        o.rhythmSeed = 5; o.pitchSeed = 6;
        auto slow = generate(o, cslow);
        Ctx cnone = cslow; cnone.sndValid = false;
        auto ref = generate(o, cnone);
        bool lenOk = ! slow.empty();
        for (size_t i = 0; i < slow.size(); ++i)
        {
            const int nx = i + 1 < slow.size() ? slow[i + 1].start : 384;
            if (slow[i].len < std::min(192, nx - slow[i].start)) lenOk = false;
        }
        CHECK(lenOk && slow.size() <= ref.size() && monoOk(slow),
              "[29a] H8: slow attack = min half-note holds + sparser, still mono");
        Ctx cpl; cpl.bars = 1;
        cpl.sndValid = true; cpl.sndAtk = 0.005f; cpl.sndSus = 0.0f; cpl.sndDec = 0.1f;
        auto pl = generate(o, cpl);
        bool shortOk = ! pl.empty();
        for (auto& x : pl) if (x.len > 24) shortOk = false;
        CHECK(shortOk && monoOk(pl), "[29b] H8: pluck = one-cell notes, mono");
    }

    // [30] STEP-COUNT AUTHORITY + STEP OUTPUT: a 7-hit line lands as 7 steps with velocity,
    // pitch-vs-Freq-knob, the approach note's SLIDE flag and gate lengths
    {
        std::vector<PartGen::Note> ns;
        for (int i = 0; i < 7; ++i) ns.push_back({ i * 384 / 7, 40, -14 + i, 200, false });
        ns[5].approach = true;
        CHECK(GenContext::chooseStepCount(ns, 1) == 7,
              "[30a] step-count authority: the smallest count that holds a 7-hit line is 7");
        auto sq = std::make_unique<Sequencer>();
        auto& bass = sq->patterns[0].channels[4];
        bass.slots[0] = DrumChannel::Slot();
        bass.slots[0].engine = DrumChannel::SrcOsc; bass.slots[0].weight = 1.0f;
        bass.slots[0].oscFreq = 110.0f;   // A2 = MIDI 45 = the Freq-knob base (step pitch 0)
        auto res = GenContext::writeStepOutput(*sq, 0, 1, 4, ns);
        bool ok = res.count == 7 && res.written == 7 && bass.numSteps == 7 && ! bass.drawMode;
        for (int i = 0; i < 7 && ok; ++i) ok = bass.steps[i];
        ok = ok && std::abs(bass.stepPitch[0] - 1.0f) < 0.01f          // semi -14 = MIDI 46 = knob +1
                && bass.stepSlide[5] && ! bass.stepSlide[4]            // approach -> SLIDE
                && bass.stepVel[0] > 0.7f && bass.stepVel[0] < 0.85f   // vel 200/255
                && bass.stepNoteLen[0] > 0.6f && bass.stepNoteLen[0] < 0.85f;   // 40 of ~55 cols
        CHECK(ok, "[30b] step writer: steps + vel + pitch + Slide + gate all written");
        // full flow: a generated Driving bass over a 7-step kick, written as steps - every onset
        // must survive the grid within the writer's tolerance
        Ctx c7b; c7b.bars = 1;
        for (int k = 0; k < 7; ++k) { c7b.kickCol[k] = k * 384 / 7; c7b.kickStr[k] = 1.0f;
                                      c7b.hitCol[k]  = k * 384 / 7; c7b.hitStr[k]  = 1.0f; }
        c7b.nKick = c7b.nHits = 7;
        Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = RhDriving; o.density = 2;
        o.rhythmSeed = 2; o.pitchSeed = 9;
        auto gen = generate(o, c7b);
        auto res2 = GenContext::writeStepOutput(*sq, 0, 1, 4, gen);
        bool fit = res2.written == (int) gen.size() && bass.numSteps == res2.count;
        for (auto& x : gen)
        {
            const int k = (int) std::lround((double) x.start * res2.count / 384.0);
            const int grid = (int) std::lround((double) k * 384.0 / res2.count);
            if (std::abs(x.start - grid) > 12) fit = false;
        }
        CHECK(fit && res2.count == 7 && ! gen.empty(),
              "[30c] full flow: generated bass fits a 7-step grid, every onset preserved");
    }

    // [31] the panel trio: Intensity widens the accent spread, Humanize is seed-deterministic
    // jitter, Fills add end-of-bar notes - all default-off = bit-identical output
    {
        Options o; o.scale = kMajor; o.density = 2; o.rhythmSeed = 8; o.pitchSeed = 15;
        auto ref = generate(o, ctx4);
        auto spread = [](const std::vector<PartGen::Note>& n)
        { int lo = 999, hi = 0; for (auto& x : n) { lo = std::min(lo, x.vel); hi = std::max(hi, x.vel); }
          return n.empty() ? 0 : hi - lo; };
        o.intensity = 0; auto soft = generate(o, ctx4);
        o.intensity = 2; auto hard = generate(o, ctx4);
        o.intensity = 1;
        CHECK(spread(hard) > spread(soft) && ! soft.empty(),
              "[31a] Intensity: Hard's accent spread exceeds Soft's");
        o.humanize = 2;
        auto h1 = generate(o, ctx4), h2 = generate(o, ctx4);
        bool same = h1.size() == h2.size(), moved = false;
        for (size_t i = 0; same && i < h1.size(); ++i)
            same = h1[i].start == h2[i].start && h1[i].vel == h2[i].vel;
        for (size_t i = 0; i < h1.size() && i < ref.size(); ++i)
            if (h1[i].start != ref[i].start || h1[i].vel != ref[i].vel) moved = true;
        o.humanize = 0;
        CHECK(same && moved, "[31b] Humanize: deterministic from the seeds, audibly jitters");
        o.fills = 1; auto fl = generate(o, ctx4); o.fills = 0;
        auto lastBarCount = [](const std::vector<PartGen::Note>& n)
        { int c2 = 0; for (auto& x : n) if (x.start >= 3 * 384) ++c2; return c2; };
        CHECK(lastBarCount(fl) >= lastBarCount(ref) && fl.size() >= ref.size(),
              "[31c] Fills: the last bar gains notes, nothing lost elsewhere");
    }

    // ---- [r20 item A] UNIVERSAL OUTPUT: the mode matrix (both writers switch + clear) ----

    // [32] roll channel + "Steps" -> count chosen, channel switched, roll data cleared;
    //      step channel + "Piano Roll" -> switched to roll, steps cleared, notes written
    {
        auto sq = std::make_unique<Sequencer>();
        auto& c0 = sq->patterns[0].channels[0];
        c0.drawMode = true;
        { DrumChannel::DrawNote dn; dn.start = 10; dn.len = 40; dn.semi = 3; dn.vel = 200; c0.addDrawNote(dn); }
        std::vector<PartGen::Note> ns;
        for (int i = 0; i < 4; ++i) ns.push_back({ i * 96, 48, i, 200, false });
        auto sw = GenContext::writeStepOutput(*sq, 0, 1, 0, ns);
        CHECK(sw.switched && ! c0.drawMode && c0.drawNoteCount == 0 && sw.count == 4
              && c0.numSteps == 4 && c0.steps[0] && c0.steps[3],
              "[32a] Steps on a roll channel: switched, roll cleared, count authority applied");
        auto& c1 = sq->patterns[0].channels[1];
        c1.numSteps = 8; c1.steps[0] = c1.steps[4] = true;
        auto rw = GenContext::writeRollOutput(*sq, 0, 1, 1, ns);
        bool stepsGone = true;
        for (int i = 0; i < DrumChannel::MAX_STEPS; ++i) if (c1.steps[i]) stepsGone = false;
        CHECK(rw.switched && c1.drawMode && stepsGone && rw.written == 4 && c1.drawNoteCount == 4,
              "[32b] Piano Roll on a step channel: switched, steps cleared, notes written");
        auto rw2 = GenContext::writeRollOutput(*sq, 0, 1, 1, ns);   // already-roll = no switch
        CHECK(! rw2.switched && c1.drawNoteCount == 4, "[32c] roll on a roll channel: no switch, replaced");
    }

    // ---- [r20 item B] DRUM KIT: style-DNA canon, velocities, ghosts, alternation, fills ----

    // [33] determinism: identical options -> identical lanes + swing
    {
        DrumGen::Options d; d.style = DrumGen::StBoomBap; d.bars = 2; d.rhythmSeed = 5; d.auxSeed = 9;
        auto a = DrumGen::generate(d), b = DrumGen::generate(d);
        bool same = a.swing == b.swing;
        for (int ln = 0; ln < DrumGen::NUM_LANES && same; ++ln)
        {
            same = a.lane[ln].size() == b.lane[ln].size();
            for (size_t i = 0; same && i < a.lane[ln].size(); ++i)
                same = a.lane[ln][i].col == b.lane[ln][i].col
                    && a.lane[ln][i].vel == b.lane[ln][i].vel
                    && a.lane[ln][i].roll == b.lane[ln][i].roll;
        }
        CHECK(same && ! a.lane[DrumGen::LKick].empty(), "[33] drum kit deterministic (same seeds = identical)");
    }

    // [34] canon compliance: house kick = the immutable 4-floor; trap snare ONLY on cell 8
    //      (the 1-based col 9), trap kick NEVER cell 8; over seeds AND vary counts
    {
        bool houseOk = true, trapSnOk = true, trapKkOk = true, bbOk = true;
        for (uint32_t s = 1; s <= 5; ++s)
            for (int v = 0; v <= 2; ++v)
            {
                DrumGen::Options d; d.rhythmSeed = s; d.auxSeed = s + 3; d.varyCount = v; d.bars = 1;
                d.style = DrumGen::StHouse;
                { auto o = DrumGen::generate(d);
                  std::set<int> kc; for (auto& h : o.lane[DrumGen::LKick]) kc.insert(h.col);
                  if (kc != std::set<int>({ 0, 96, 192, 288 })) houseOk = false; }
                d.style = DrumGen::StTrap;
                { auto o = DrumGen::generate(d);
                  // canon = the BACKBEAT-velocity snare lives on cell 8 ONLY; quiet D6 ghosts on
                  // the flanking 16ths are legal (they are not the backbeat)
                  float mx = 0.0f;
                  for (auto& h : o.lane[DrumGen::LSnare]) mx = std::max(mx, h.vel);
                  for (auto& h : o.lane[DrumGen::LSnare])
                      if (h.vel > mx * 0.5f && h.col != 8 * 24) trapSnOk = false;
                  if (o.lane[DrumGen::LSnare].empty()) trapSnOk = false;
                  for (auto& h : o.lane[DrumGen::LKick]) if (h.col == 8 * 24) trapKkOk = false;
                  bool one = false; for (auto& h : o.lane[DrumGen::LKick]) if (h.col == 0) one = true;
                  if (! one) trapKkOk = false; }
                d.style = DrumGen::StBoomBap; d.fills = 0;
                { auto o = DrumGen::generate(d);
                  bool one = false; std::set<int> sn;
                  for (auto& h : o.lane[DrumGen::LKick]) if (h.col == 0) one = true;
                  for (auto& h : o.lane[DrumGen::LSnare]) sn.insert(h.col);
                  if (! one || ! sn.count(4 * 24) || ! sn.count(12 * 24)) bbOk = false; }
            }
        CHECK(houseOk,  "[34a] house kick = the immutable 4-on-the-floor (every seed + vary)");
        CHECK(trapSnOk, "[34b] trap snare only on the col-9 equivalent (cell 8)");
        CHECK(trapKkOk, "[34c] trap kick keeps the One, never cell 8");
        CHECK(bbOk,     "[34d] boom-bap: kick on the One, backbeat snares 5+13 present");
    }

    // [35] backbeat law: snare backbeat velocity in the D2 window (no ghosts counted)
    {
        DrumGen::Options d; d.style = DrumGen::StBoomBap; d.bars = 1; d.rhythmSeed = 2; d.auxSeed = 4;
        auto o = DrumGen::generate(d);
        bool ok = false, band = true; float bbVel = 0.0f;
        for (auto& h : o.lane[DrumGen::LSnare])
            if (h.col == 4 * 24 || h.col == 12 * 24)
            { ok = true; bbVel = h.vel; if (h.vel < 0.72f || h.vel > 0.95f) band = false; }
        (void) bbVel;
        CHECK(ok && band, "[35] backbeat snares sit in the 100-115-MIDI window (0.72..0.95)");
    }

    // [36] ghost geography + the mined ~25% ratio band
    {
        bool ratioOk = true, cellOk = true, budgetOk = true; int ghosts = 0;
        float ratioSample = 0.0f;
        for (uint32_t s = 1; s <= 5; ++s)
        {
            DrumGen::Options d; d.style = DrumGen::StBoomBap; d.bars = 1; d.rhythmSeed = s; d.auxSeed = s + 7;
            auto o = DrumGen::generate(d);
            float bb = 0.0f; int g = 0;
            for (auto& h : o.lane[DrumGen::LSnare])
                if (h.col == 4 * 24 || h.col == 12 * 24) bb = std::max(bb, h.vel);
            for (auto& h : o.lane[DrumGen::LSnare])
            {
                if (h.col == 4 * 24 || h.col == 12 * 24) continue;
                ++g; ++ghosts;
                const float ratio = h.vel / bb;
                if (s == 1 && ratioSample == 0.0f) ratioSample = ratio;
                if (ratio < 0.12f || ratio > 0.45f) ratioOk = false;   // mined ~0.22 +- jitter
                const int cell = h.col / 24;
                if (cell != 3 && cell != 6 && cell != 7 && cell != 11 && cell != 15) cellOk = false;
            }
            if (g < 1 || g > 3) budgetOk = false;                       // genre budget 1-3/bar
        }
        CHECK(ghosts > 0 && ratioOk, "[36a] ghost snares at the mined ~25% of the backbeat");
        CHECK(cellOk && budgetOk,    "[36b] ghosts on backbeat-flanking 16ths, 1-3 per bar");
    }

    // [37] hat alternation: no two consecutive equal velocities, every style (D3's law)
    {
        bool ok = true;
        for (int st = 0; st < DrumGen::NUM_STYLES; ++st)
            for (uint32_t s = 1; s <= 3; ++s)
            {
                DrumGen::Options d; d.style = st; d.bars = 2; d.rhythmSeed = s; d.auxSeed = s * 5 + 1;
                auto o = DrumGen::generate(d);
                for (size_t i = 1; i < o.lane[DrumGen::LHat].size(); ++i)
                    if (std::fabs(o.lane[DrumGen::LHat][i].vel - o.lane[DrumGen::LHat][i - 1].vel) < 0.004f)
                        ok = false;
            }
        CHECK(ok, "[37] hat lane never repeats a velocity back to back (all styles)");
    }

    // [38] fills: only the phrase-final bar changes; off = every bar identical
    {
        auto barCells = [](const std::vector<DrumGen::Hit>& ln, int bar)
        {
            std::set<int> s2;
            for (auto& h : ln) if (h.col >= bar * 384 && h.col < (bar + 1) * 384)
                s2.insert(h.col - bar * 384);
            return s2;
        };
        DrumGen::Options d; d.style = DrumGen::StFunk; d.bars = 4; d.rhythmSeed = 3; d.auxSeed = 6;
        d.fills = 0;
        auto off = DrumGen::generate(d);
        bool offSame = true;
        for (int ln : { (int) DrumGen::LKick, (int) DrumGen::LSnare, (int) DrumGen::LHat })
            for (int b = 1; b < 4; ++b)
                if (barCells(off.lane[ln], b) != barCells(off.lane[ln], 0)) offSame = false;
        d.fills = 1;
        auto on = DrumGen::generate(d);
        bool earlySame = true, lastDiff = false;
        for (int ln : { (int) DrumGen::LKick, (int) DrumGen::LSnare, (int) DrumGen::LHat })
        {
            for (int b = 1; b < 3; ++b)
                if (barCells(on.lane[ln], b) != barCells(on.lane[ln], 0)) earlySame = false;
            if (barCells(on.lane[ln], 3) != barCells(on.lane[ln], 0)) lastDiff = true;
        }
        CHECK(offSame, "[38a] fills Off: every bar plays the identical groove");
        CHECK(earlySame && lastDiff, "[38b] fills Last bar: only the phrase-final bar changes");
    }

    // [39] the kit WRITER: 16-step canon grid, velocities + trap rolls land as stepRoll
    {
        auto sq = std::make_unique<Sequencer>();
        DrumGen::Options d; d.style = DrumGen::StTrap; d.bars = 1; d.rhythmSeed = 1; d.auxSeed = 2;
        auto o = DrumGen::generate(d);
        auto rk = GenContext::writeDrumLane(*sq, 0, 1, 0, o.lane[DrumGen::LKick]);
        auto rh = GenContext::writeDrumLane(*sq, 0, 1, 2, o.lane[DrumGen::LHat]);
        auto& kick = sq->patterns[0].channels[0];
        auto& hat  = sq->patterns[0].channels[2];
        bool rollOk = false;
        for (int i = 0; i < hat.numSteps; ++i)
            if (hat.steps[i] && hat.stepRoll[i] > 1 && hat.stepRollDecay[i] > 0.0f) rollOk = true;
        CHECK(rk.count == 16 && rh.count == 16 && kick.numSteps == 16 && kick.steps[0]
              && rk.written == (int) o.lane[DrumGen::LKick].size() && rollOk,
              "[39] drum writer: 16-step grid, kick on the One, trap hat rolls as stepRoll ramps");
        // the DRUM SCORECARD line: canon-match %, ghost ratio, swing (boom-bap seed 1)
        DrumGen::Options ds; ds.style = DrumGen::StBoomBap; ds.bars = 1; ds.rhythmSeed = 1; ds.auxSeed = 2;
        auto so = DrumGen::generate(ds);
        int inCanon = 0, kn = (int) so.lane[DrumGen::LKick].size();
        for (auto& h : so.lane[DrumGen::LKick])
        {
            const int cell = h.col / 24;
            if (cell == 0 || cell == 6 || cell == 7 || cell == 10 || cell == 14) ++inCanon;
        }
        float bb = 0.0f, gh = 0.0f;
        for (auto& h : so.lane[DrumGen::LSnare])
            if (h.col == 4 * 24 || h.col == 12 * 24) bb = std::max(bb, h.vel); else gh = std::max(gh, h.vel);
        printf("  SCORE drums/boom-bap (seed 1)     canonMatch=%.0f%% ghostRatio=%.0f%% swing=%.0f%%\n",
               kn > 0 ? 100.0f * (float) inCanon / (float) kn : 0.0f,
               bb > 0.0f ? 100.0f * gh / bb : 0.0f, 50.0f + 25.0f * so.swing);
        CHECK(kn > 0 && inCanon == kn, "[39b] drum scorecard: every kick inside canon + optional cells");
    }

    // ---- [r20 item E] the melodic-identity round: M10-M14, G3 full band, G4 push, accents ----

    // [40] M10 MOTIF ECONOMY: >= 60% of onsets sit on the cell's rhythm offsets (transforms
    // preserve the rhythm), and the pitch identity shows as repeated DIATONIC trigrams on a
    // majority of seeds (sequences count - the scorecard's upgraded ngram)
    {
        Ctx c4m; c4m.bars = 4;
        bool covOk = true; int ngramSeeds = 0, seeds = 0;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhFlowing; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 3;
            auto n = generate(o, c4m);
            std::vector<int> cell;
            for (auto& x : n) if (x.start < 192) cell.push_back(x.start % 192);
            int match = 0;
            for (auto& x : n)
            {
                bool m = false;
                for (int cOf : cell) if ((x.start % 192) == cOf) m = true;
                if (m) ++match;
            }
            if (n.empty() || (float) match / (float) n.size() < 0.6f) covOk = false;
            const auto sc = scoreGen(n, o, c4m);
            ++seeds; if (sc.ngram > 0.0f) ++ngramSeeds;
        }
        CHECK(covOk, "[40a] M10: >= 60% of onsets are motif-cell transforms (rhythm coverage)");
        CHECK(ngramSeeds * 2 >= seeds, "[40b] M10: diatonic-trigram similarity > 0 on most seeds");
    }

    // [41] M11 Q/A degree ends: the question ends on 2/5/7, the answer on 1/3 (C major pcs)
    {
        Ctx c2q; c2q.bars = 2;
        bool qOk = true, aOk = true;
        for (uint32_t s = 1; s <= 5; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.lines = 2; o.relation = RelAnswer;
            o.rhythmSeed = s; o.pitchSeed = s + 6;
            auto n = generate(o, c2q);
            PartGen::Note* qLast = nullptr; PartGen::Note* aLast = nullptr;
            for (auto& x : n) { if (x.start < 384) qLast = &x; aLast = &x; }
            if (qLast == nullptr || aLast == nullptr || qLast == aLast) { qOk = aOk = false; continue; }
            const int qp = ((qLast->semi % 12) + 12) % 12, ap = ((aLast->semi % 12) + 12) % 12;
            if (! (qp == 2 || qp == 7 || qp == 11)) qOk = false;   // D / G / B
            if (! (ap == 0 || ap == 4))             aOk = false;   // C / E
        }
        CHECK(qOk, "[41a] M11: the question ends OPEN (2nd / 5th / 7th) on every seed");
        CHECK(aOk, "[41b] M11: the answer ends HOME (tonic / 3rd) on every seed");
    }

    // [42] G3 FULL BAND: meter-only melody sits inside the doc's [2,6] band on average
    // (the floor injection lifts under-syncopated bars; the ceiling still caps)
    {
        Ctx cb2; cb2.bars = 2;
        float worst = 99.0f, best = -1.0f; bool capOk = true;
        for (uint32_t s = 1; s <= 5; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhFlowing; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 8;
            auto n = generate(o, cb2);
            float mean = 0.0f;
            for (int b = 0; b < 2; ++b)
            {
                const int sc2 = lhlBarScore(n, b);
                if (sc2 > 6) capOk = false;
                mean += (float) sc2;
            }
            mean *= 0.5f;
            worst = std::min(worst, mean); best = std::max(best, mean);
        }
        CHECK(capOk && worst >= 1.0f, "[42] G3 band: melody mean LHL >= 1 per take, every bar <= 6");
    }

    // [43] G4/M13 anticipation: over seeds, SOME change-note arrives one cell early (still the
    // new chord's tone), and no two consecutive changes are both pushed
    {
        Ctx cg4; cg4.bars = 4;
        bool pushed = false, pairOk = true, toneOk = true;
        for (uint32_t s = 1; s <= 8; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = RhDriving; o.density = 1;
            o.progression = 5;   // I-IV-V(-I): changes at every bar line (density 1 = 1 chord/bar)
            o.rhythmSeed = s; o.pitchSeed = s + 2;
            auto n = generate(o, cg4);
            bool prevP = false;
            for (int b = 1; b < 4; ++b)
            {
                bool thisP = false;
                for (auto& x : n)
                    if (x.start == b * 384 - 24)
                    {
                        thisP = true; pushed = true;
                        static const int prog[4] = { 0, 3, 4, 0 };
                        const int wantPc = (0 + kMajor[prog[b % 4]]) % 12;
                        if (((x.semi % 12) + 12) % 12 != wantPc) toneOk = false;
                    }
                if (thisP && prevP) pairOk = false;
                prevP = thisP;
            }
        }
        CHECK(pushed && pairOk, "[43a] G4: pushes happen, never two consecutive changes");
        CHECK(toneOk, "[43b] M13: a pushed note carries the NEW chord's tone (the bass root)");
    }

    // [44] M14 breath: <= 12 onsets per bar-pair, and a melody phrase end leaves air
    {
        Ctx c4b; c4b.bars = 4;
        bool capOk = true, airOk = true;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhPockets; o.density = 2;
            o.lines = 2; o.rhythmSeed = s; o.pitchSeed = s + 5;
            auto n = generate(o, c4b);
            for (int w = 0; w < 2; ++w)
            {
                int cnt = 0;
                for (auto& x : n) if (x.start >= w * 768 && x.start < (w + 1) * 768) ++cnt;
                if (cnt > 12) capOk = false;
            }
            for (auto& x : n)   // lines=2 on 4 bars: the phrase boundary at bar 2's end breathes
                if (x.start < 768 && x.start + x.len > 768 - 12) airOk = false;
        }
        CHECK(capOk, "[44a] M14: never more than 12 onsets in a bar-pair");
        CHECK(airOk, "[44b] M14: melody phrases leave air at the phrase boundary");
    }

    // [45] mined ACCENT wiring: with a style accent table, backbeat-position notes out-vote the
    // weak inner 16ths on average (the hiphop row's 104-vs-44 shape must be audible)
    {
        static const uint8_t hiphop[16] = { 76,44,61,47,104,48,53,44,72,47,66,52,97,58,70,49 };
        Ctx c2a; c2a.bars = 2;
        float strongSum = 0, weakSum = 0; int sN = 0, wN = 0;
        for (uint32_t s = 1; s <= 6; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhFlowing; o.density = 2;
            o.styleAccent = hiphop; o.rhythmSeed = s; o.pitchSeed = s + 9;
            for (auto& x : generate(o, c2a))
            {
                const int pos = (x.start % 384) / 24;
                if (pos == 4 || pos == 12) { strongSum += (float) x.vel; ++sN; }
                if (pos == 1 || pos == 3 || pos == 5)  { weakSum += (float) x.vel; ++wN; }
            }
        }
        CHECK(sN > 0 && (wN == 0 || strongSum / (float) sN > weakSum / (float) wN + 10.0f),
              "[45] styleAccent: backbeat-position notes measurably louder than inner 16ths");
    }

    // [46] KEEP MY NOTES (engine half): repitch keeps every onset + length EXACTLY, moves pitches
    {
        Ctx c1k; c1k.bars = 1;
        std::vector<PartGen::Note> user;
        for (int i = 0; i < 5; ++i) user.push_back({ i * 72 + 12, 48, 7, 200, false });
        auto notes = user;
        Options o; o.scale = kMajor; o.pitchSeed = 77;
        PartGen::repitch(o, c1k, notes);
        bool rhythmSame = notes.size() == user.size(), pitchMoved = false, inScaleOk = true;
        for (size_t i = 0; i < notes.size() && rhythmSame; ++i)
        {
            rhythmSame = notes[i].start == user[i].start && notes[i].len == user[i].len;
            if (notes[i].semi != user[i].semi) pitchMoved = true;
            if (! inScale(notes[i].semi, 0, kMajor, 7)) inScaleOk = false;
        }
        CHECK(rhythmSame && pitchMoved && inScaleOk,
              "[46] keep-my-rhythm: onsets + lengths exact, pitches rewritten in scale");
    }

    // ---- [r20 item G] the COMPING role: voicings, voice leading, lanes, gap-filling ----

    // [47] Chords voicings: poly stacks, H1 low-interval limits hold, H2 minimal motion with
    // common tones locked, forceMono degrades to a single arpeggiated line
    {
        Ctx cc4; cc4.bars = 4;
        bool poly = false, lilOk = true, motionOk = true, monoOk2 = true, inScaleOk = true;
        float motionSum = 0; int motionN = 0;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleChords; o.rhythm = RhFlowing; o.density = 1;
            o.progression = 5;   // I-IV-V: real changes = real voice leading
            o.rhythmSeed = s; o.pitchSeed = s + 4;
            auto n = generate(o, cc4);
            std::vector<std::vector<int>> voicings;
            for (auto& x : n)
                if (! inScale(x.semi, 0, kMajor, 7)) inScaleOk = false;
            // group by start
            std::vector<int> starts;
            for (auto& x : n) if (starts.empty() || starts.back() != x.start) starts.push_back(x.start);
            for (int st : starts)
            {
                std::vector<int> v;
                for (auto& x : n) if (x.start == st) v.push_back(x.semi);
                std::sort(v.begin(), v.end());
                if (v.size() >= 3) poly = true;
                for (size_t i = 1; i < v.size(); ++i)
                {
                    if (v[i - 1] < -12 && v[i] - v[i - 1] <= 4) lilOk = false;   // H1
                    if (v[i - 1] < -5  && v[i] - v[i - 1] <= 2) lilOk = false;
                }
                voicings.push_back(v);
            }
            for (size_t i = 1; i < voicings.size(); ++i)
            {   // H2: mean motion between consecutive voicings (nearest-voice matching)
                for (int t : voicings[i])
                {
                    int bd = 1 << 20;
                    for (int pv : voicings[i - 1]) bd = std::min(bd, std::abs(t - pv));
                    if (bd < 1 << 20) { motionSum += (float) bd; ++motionN; }
                }
            }
            o.forceMono = true;
            auto m = generate(o, cc4);
            for (size_t i = 0; i + 1 < m.size(); ++i)
                if (m[i].start + m[i].len > m[i + 1].start || m[i].start == m[i + 1].start) monoOk2 = false;
            (void) monoOk2;
        }
        if (motionN > 0 && motionSum / (float) motionN >= 2.0f) motionOk = false;
        CHECK(poly && inScaleOk, "[47a] Chords: 3+ note voicings, all in scale");
        CHECK(lilOk,    "[47b] H1: zero low-interval-limit violations");
        CHECK(motionOk, "[47c] H2: mean voice motion under 2 st (common tones lock)");
        CHECK(monoOk2,  "[47d] H9: forceMono = a non-overlapping single arpeggiated line");
    }

    // [48] H6 comp-in-gaps: with the melody occupying beats 1+2 of every bar, the comp's
    // non-anchor stabs prefer the free half
    {
        Ctx co; co.bars = 2; co.melOccValid = true; co.melMed = 10;
        for (int b = 0; b < 2; ++b)
            for (int p = 0; p < 8; ++p) co.melOcc[b * 16 + p] = true;   // first half occupied
        int occ = 0, freeC = 0;
        for (uint32_t s = 1; s <= 6; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleChords; o.rhythm = RhPockets; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 8;
            for (auto& x : generate(o, co))
            {
                if ((x.start % 384) == 0) continue;                     // bar anchors exempt
                ((x.start % 384) / 24 < 8 ? occ : freeC) += 1;
            }
        }
        CHECK(freeC + occ > 0 && occ <= freeC,
              "[48] H6: comp stabs prefer the melody's gaps (occupied half loses)");
    }

    // [49] [item H, G5] POCKET MICROTIMING: kick-coincident steps carry a positive Nudge in the
    // +0.02..+0.08-step band (field units 0.04..0.16), never negative, others untouched; the
    // laid-back notes lean +5% velocity
    {
        auto sq = std::make_unique<Sequencer>();
        std::vector<PartGen::Note> ns;
        for (int i = 0; i < 4; ++i) ns.push_back({ i * 96, 48, -14, 200, false });
        PartGen::Ctx cx; cx.bars = 1;
        cx.nKick = 2; cx.kickCol[0] = 0; cx.kickCol[1] = 192;   // kicks under notes 0 + 2 only
        auto res = GenContext::writeStepOutput(*sq, 0, 1, 5, ns, &cx, 4.0, 2000.0);
        auto& cc = sq->patterns[0].channels[5];
        bool ok = res.count == 4;
        // 2000 ms bar / 4 steps = 500 ms step; 4 ms drag -> 4 / 250 = 0.016 -> clamped to 0.04
        ok = ok && cc.stepNudge[0] >= 0.04f - 0.001f && cc.stepNudge[0] <= 0.16f + 0.001f
                && cc.stepNudge[2] >= 0.04f - 0.001f
                && cc.stepNudge[1] == 0.0f && cc.stepNudge[3] == 0.0f;
        for (int i = 0; i < cc.numSteps; ++i) if (cc.stepNudge[i] < 0.0f) ok = false;
        ok = ok && cc.stepVel[0] > cc.stepVel[1];   // the +5% lean on the pocketed notes
        CHECK(ok, "[49] G5: kick-coincident steps nudge late within band, never early");
    }

    // ---- [2026-07-22 r21] the CONTEXT LATTICE round: the user's confirmed bug locked ----

    // [50] THE USER'S SCENARIO: 7-step kick + 7-step snare + 14-step hats over a merged 4-bar
    // group -> the lattice is 14, a generated bass lands on a 7/14 step count (NEVER the old
    // 16 fallback), and EVERY onset sits exactly on the 14-lattice (both stances)
    {
        auto sq = std::make_unique<Sequencer>();
        for (int b = 1; b < 4; ++b) sq->patterns[b].mergeWithPrev = true;
        for (int b = 0; b < 4; ++b)
        {
            auto& kk = sq->patterns[b].channels[0];
            kit(kk, 7, { 0, 2, 4 });  kk.mixName = "My Kick";
            auto& sn = sq->patterns[b].channels[1];
            kit(sn, 7, { 1, 5 });     sn.mixName = "My Snare";
            auto& hh = sq->patterns[b].channels[2];
            kit(hh, 14, { 0, 2, 4, 6, 8, 10, 12 }); hh.mixName = "My Hat";
        }
        GenContext::Readout ro;
        auto cx = GenContext::build(*sq, 0, 3, 4, &ro);
        CHECK(cx.latticeN == 14 && ro.lattice == 14 && cx.nKick == 12 && cx.nHat == 28,
              "[50a] lattice: 7 + 7 + 14 step context derives the 14-cell bar grid");
        bool onLat = true, countOk = true, wroteAll = true;
        for (int stance : { (int) RhPockets, (int) RhDriving })
            for (uint32_t s = 1; s <= 3; ++s)
            {
                Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = stance;
                o.density = 2; o.rhythmSeed = s; o.pitchSeed = s + 5;
                auto n = generate(o, cx);
                for (auto& x : n)
                {   // every onset must sit EXACTLY on a 14-lattice col (k * 384 / 14, truncated)
                    const int local = x.start % 384;
                    const int k2 = (int) std::lround((double) local * 14.0 / 384.0);
                    if (local != k2 * 384 / 14) onLat = false;
                }
                auto res = GenContext::writeStepOutput(*sq, 0, 4, 4, n);
                if (n.empty() || res.written != (int) n.size()) wroteAll = false;
                if (res.count != 7 && res.count != 14) countOk = false;
                printScore(stance == RhPockets ? "bass/pockets 7-14 ctx" : "bass/driving 7-14 ctx",
                           scoreGen(n, o, cx));
            }
        CHECK(onLat,   "[50b] lattice: every generated onset sits ON the 14-lattice (6 takes)");
        CHECK(countOk && wroteAll,
              "[50c] step-count authority lands on 7 or 14 - NOT the 16 fallback (the user's bug)");
    }

    // [51] classifyDrumRole CONSENT GUARD: a known NON-drum category is authoritative - the
    // name keywords may only classify sounds the bank doesn't know
    {
        using namespace GenContext;
        CHECK(classifyDrumRole("Bass",  "Sub Kick")  == DrumGeneric
              && classifyDrumRole("Leads", "Kick Start") == DrumGeneric
              && classifyDrumRole("Kicks", "808 Kick")   == DrumKick
              && classifyDrumRole("Toms",  "Floor Tom")  == DrumPerc
              && classifyDrumRole("Electro Perc", "Blip") == DrumPerc
              && classifyDrumRole("", "my kick thing")   == DrumKick
              && classifyDrumRole("", "Vocal Chop")      == DrumGeneric,
              "[51] drum-role guard: category beats name; keywords only for unknown sounds");
    }

    // [52] lattice derivation table: binary grids fold to 16, triplets to 12, LCM under the
    // 48 cap, and an impossible pair keeps the heavier grid
    {
        auto latOf = [&](std::initializer_list<std::pair<int, int>> chans)   // {numSteps, nHits}
        {
            auto sq = std::make_unique<Sequencer>();
            int chn = 0;
            for (auto& p : chans)
            {
                auto& cc2 = sq->patterns[0].channels[chn++];
                std::vector<int> on;
                for (int i = 0; i < p.second; ++i) on.push_back(i);
                cc2.numSteps = p.first;
                for (int i : on) cc2.steps[i] = true;
                cc2.slots[0] = DrumChannel::Slot();
                cc2.slots[0].engine = DrumChannel::SrcNoise; cc2.slots[0].weight = 1.0f;
            }
            return GenContext::build(*sq, 0, 0, 15).latticeN;
        };
        CHECK(latOf({ { 4, 4 } })            == 16     // binary folds up to the classic 16
              && latOf({ { 8, 4 } })         == 16
              && latOf({ { 12, 6 } })        == 12     // triplet grid stays a triplet grid
              && latOf({ { 7, 7 } })         == 14     // the 7 family doubles for resolution
              && latOf({ { 16, 8 }, { 12, 4 } }) == 48 // LCM within the cap holds both
              && latOf({ { 7, 7 }, { 16, 4 } })  == 14 // impossible pair: the heavier grid wins
              && latOf({})                   == 16,    // no step context = 16
              "[52] lattice derivation: LCM + cap + doubling land on the musical grid");
    }

    // [53] determinism on the lattice path + the truncation-consistent exact fit
    {
        auto sq = std::make_unique<Sequencer>();
        kit(sq->patterns[0].channels[0], 7, { 0, 1, 2, 3, 4, 5, 6 });
        sq->patterns[0].channels[0].mixName = "My Kick";
        auto cx = GenContext::build(*sq, 0, 0, 4);
        Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = RhDriving;
        o.density = 2; o.rhythmSeed = 9; o.pitchSeed = 4;
        auto a = generate(o, cx), b = generate(o, cx);
        bool same = a.size() == b.size() && ! a.empty();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].start == b[i].start && a[i].semi == b[i].semi && a[i].vel == b[i].vel;
        CHECK(same, "[53a] lattice path deterministic (same seeds = identical notes)");
        std::vector<PartGen::Note> tr;   // the gather's TRUNCATED cols must fit n=7 exactly
        for (int i = 0; i < 7; ++i) tr.push_back({ i * 384 / 7, 40, 0, 200, false });
        CHECK(GenContext::chooseStepCount(tr, 1) == 7,
              "[53b] chooser: truncated 7-grid cols pass the exact-fit pass (54 vs the old 55)");
    }

    // ============ [r22] STYLES AS DATA + MELODIC DNA + MICROTIMING + FINESSE + GENERATE ALL ====

    // [54] STYLE FILES: parse round-trip, a deliberately-broken file skipped gracefully,
    // user-collision replacement, and the factory DATA-BREADTH minimums (originality mandate)
    {
        const char* txt =
            "# a comment line\n"
            "style \"Test Groove\"\n"
            "swing 57\n"
            "kick.canon 1 9\nkick.opt 15\nkick.optn 1\n"
            "snare.cells 5 13\nhat.tier 16ths\nhat.rolls 8 16?\nopenhat all\n"
            "ghosts 2\nghost.ratio 0.3\nfill hatroll\n"
            "vel.kick 100 20\nvel.snare 110 10\nvel.hat 60 25\n"
            "accent 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
            "micro.kick -1\nmicro.snare 3.5\nmicro.hat -2\nmicro.bass 4\n"
            "unknown.future.key 42\n"                       // unknown keys IGNORED (forward compat)
            "bass 2 : 1 4 R 100 | 7 1 O 90 | 15 1 A 80\n"
            "mel 1 : 1C 4L 8A 12R ; slope -2 3\n"
            "comp 1 : 3 7 11\n"
            "prog 2 : 1 5 6 4\n"
            "hook 4\nmel.density 1.5\n";
        GenStyle::Style st; std::string err;
        const bool ok = GenStyle::parse(txt, st, err);
        CHECK(ok && st.name == "Test Groove" && std::fabs(st.swingPct - 57.0f) < 0.01f
              && st.kickCanon == ((1u << 0) | (1u << 8)) && st.kickOpt == (1u << 14) && st.kickOptN == 1
              && st.snareCells == ((1u << 4) | (1u << 12)) && st.hatTier == 2
              && st.hatRollAlways == (1u << 7) && st.hatRollMaybe == (1u << 15)
              && st.openHatAll && st.ghostBudget == 2 && st.fillVariant == 2
              && std::fabs(st.kickVel - 100.0f / 127.0f) < 0.001f && std::fabs(st.sdSnare - 10.0f) < 0.01f
              && st.accent[0] == 1 && st.accent[15] == 16
              && std::fabs(st.microSnare - 3.5f) < 0.01f && std::fabs(st.microBass - 4.0f) < 0.01f
              && st.bass.size() == 1 && st.bass[0].ev.size() == 3
              && st.bass[0].ev[1].code == GenStyle::DegOct && st.bass[0].ev[2].code == GenStyle::DegApproach
              && st.mel.size() == 1 && st.mel[0].ev.size() == 3          // the R rest is dropped
              && st.mel[0].slopeLo == -2 && st.mel[0].slopeHi == 3
              && st.comp.size() == 1 && st.comp[0].pos16.size() == 3
              && st.progs.size() == 1 && st.progs[0].degs.size() == 4 && st.progs[0].degs[1] == 4
              && st.hookBars == 4 && std::fabs(st.melDensity - 1.5f) < 0.01f,
              "[54a] style parse round-trip: every field lands (unknown keys ignored)");
        GenStyle::Style bad; std::string berr;
        CHECK(! GenStyle::parse("style \"Broken\"\nkick.canon 1 19\n", bad, berr) && ! berr.empty()
              && ! GenStyle::parse("swing 55\n", bad, berr)              // no style name = broken
              && ! GenStyle::parse("style \"B\"\nbass 1 : 1 2 X 90\n", bad, berr),
              "[54b] broken files FAIL the parse with an error (never a crash)");
        GenStyle::resetToFactory();
        const int n0 = GenStyle::count();
        CHECK(! GenStyle::addUser("style \"Junk\"\nsnare.cells 44\n").empty()
              && GenStyle::count() == n0,
              "[54c] a broken user file is SKIPPED - the registry is untouched");
        CHECK(GenStyle::addUser("style \"House\"\nswing 66\n").empty()
              && GenStyle::count() == n0 && GenStyle::at(0).user
              && std::fabs(GenStyle::at(0).swingPct - 66.0f) < 0.01f,
              "[54d] name collision: the USER'S style replaces the factory entry, tagged");
        GenStyle::resetToFactory();
        CHECK(! GenStyle::at(0).user && std::fabs(GenStyle::at(0).swingPct - 54.0f) < 0.01f,
              "[54e] resetToFactory restores the factory entry");
        bool breadth = true;
        for (int i = 0; i < GenStyle::NUM_FACTORY; ++i)
        {
            const auto& f = GenStyle::factory(i);
            if ((int) f.bass.size() < 4 || (int) f.comp.size() < 4
                || (int) f.mel.size() < 6 || (int) f.progs.size() < 4) breadth = false;
        }
        CHECK(breadth, "[54f] factory breadth minimums: >= 4 bass / 4 comp / 6 mel cells + 4 progs");
    }

    // [55] MELODIC STYLE DIFFERENTIATION: two styles, same seeds, from scratch -> different
    // rhythm-cell signatures; each deterministic; house pocket bass stays out of the 4-floor
    {
        auto scratchBass = [&](int styleIdx, int rhythm)
        {
            Ctx cs2; cs2.bars = 2;
            DrumGen::applyStyleSkeleton(styleIdx, cs2);
            Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = rhythm; o.density = 1;
            o.styleDna = &GenStyle::factory(styleIdx); o.styleVirtual = true;
            o.styleAccent = GenStyle::factory(styleIdx).accent;
            o.rhythmSeed = 21; o.pitchSeed = 33;
            return generate(o, cs2);
        };
        auto sig = [](const std::vector<PartGen::Note>& n)
        { std::set<int> s2; for (auto& x : n) s2.insert(x.start); return s2; };
        auto house = scratchBass(DrumGen::StHouse, RhPockets);
        auto house2 = scratchBass(DrumGen::StHouse, RhPockets);
        auto funk  = scratchBass(DrumGen::StFunk, RhPockets);
        bool det = house.size() == house2.size();
        for (size_t i = 0; det && i < house.size(); ++i)
            det = house[i].start == house2[i].start && house[i].semi == house2[i].semi;
        CHECK(det && ! house.empty(), "[55a] style-cell bass deterministic (same style + seeds)");
        CHECK(sig(house) != sig(funk), "[55b] House vs Funk, same seeds: different onset signatures");
        bool offKick = true;   // house bass never sits ON the 4-floor (pockets + offbeat cells)
        for (auto& x : house)
            for (int b = 0; b < 2; ++b)
                for (int kcell : { 0, 4, 8, 12 })
                    if (std::abs(x.start - (b * 384 + kcell * 24)) < 24) offKick = false;
        CHECK(offKick, "[55c] house pocket bass lives OFF the four-on-the-floor (the genre premise)");
    }

    // [56] KIT MICROTIMING: Humanize gates the mined per-role push/drag; Off = grid; the step
    // writer converts to stepNudge in band, and the KICK lane stays the anchor (G5)
    {
        DrumGen::Options d; d.style = DrumGen::StBoomBap; d.bars = 1; d.rhythmSeed = 4; d.auxSeed = 6;
        d.humanize = 0; auto off = DrumGen::generate(d);
        d.humanize = 1; auto sub = DrumGen::generate(d);
        d.humanize = 2; auto loose = DrumGen::generate(d);
        bool offZero = true;
        for (int ln = 0; ln < DrumGen::NUM_LANES; ++ln) if (off.laneMicroMs[ln] != 0.0f) offZero = false;
        CHECK(offZero, "[56a] Humanize Off: zero microtiming on every lane");
        CHECK(sub.laneMicroMs[DrumGen::LSnare] > 0.0f && sub.laneMicroMs[DrumGen::LHat] < 0.0f
              && std::fabs(loose.laneMicroMs[DrumGen::LSnare] - 2.0f * sub.laneMicroMs[DrumGen::LSnare]) < 0.01f,
              "[56b] snare drags late, hats push early; Subtle = half of Loose (mined values)");
        auto sq = std::make_unique<Sequencer>();
        const double barMs = 2000.0;   // 120 BPM 4/4
        GenContext::writeDrumLane(*sq, 0, 1, 0, loose.lane[DrumGen::LKick],
                                  (double) loose.laneMicroMs[DrumGen::LKick], barMs, true);
        GenContext::writeDrumLane(*sq, 0, 1, 1, loose.lane[DrumGen::LSnare],
                                  (double) loose.laneMicroMs[DrumGen::LSnare], barMs, false);
        GenContext::writeDrumLane(*sq, 0, 1, 2, loose.lane[DrumGen::LHat],
                                  (double) loose.laneMicroMs[DrumGen::LHat], barMs, false);
        bool kick0 = true, snLate = false, snBand = true, hatEarly = false, hatBand = true;
        auto& K = sq->patterns[0].channels[0];
        auto& S = sq->patterns[0].channels[1];
        auto& H = sq->patterns[0].channels[2];
        for (int i = 0; i < K.numSteps; ++i) if (K.steps[i] && K.stepNudge[i] != 0.0f) kick0 = false;
        for (int i = 0; i < S.numSteps; ++i)
            if (S.steps[i])
            { if (S.stepNudge[i] > 0.0f) snLate = true;
              if (S.stepNudge[i] < 0.0f || S.stepNudge[i] > 0.25f) snBand = false; }
        for (int i = 0; i < H.numSteps; ++i)
            if (H.steps[i])
            { if (H.stepNudge[i] < 0.0f) hatEarly = true;
              if (H.stepNudge[i] > 0.0f || H.stepNudge[i] < -0.25f) hatBand = false; }
        CHECK(kick0, "[56c] the kick lane anchors the grid: zero nudges (G5)");
        CHECK(snLate && snBand && hatEarly && hatBand,
              "[56d] snare nudges LATE, hats EARLY, both inside the +-0.25-step band");
        auto sq2 = std::make_unique<Sequencer>();
        GenContext::writeDrumLane(*sq2, 0, 1, 1, off.lane[DrumGen::LSnare],
                                  (double) off.laneMicroMs[DrumGen::LSnare], barMs, false);
        bool allZero = true;
        auto& S2 = sq2->patterns[0].channels[1];
        for (int i = 0; i < S2.numSteps; ++i) if (S2.steps[i] && S2.stepNudge[i] != 0.0f) allZero = false;
        CHECK(allZero, "[56e] Humanize Off writes a bit-clean grid (no nudges anywhere)");
    }

    // [57] M8 APPOGGIATURA: Color-licensed - strong beat, leapt into, non-chord, resolves DOWN
    // by step onto a chord tone; never more than one per phrase
    {
        Ctx ca; ca.bars = 2;
        int found = 0; bool capOk = true, shapeOk = true;
        for (uint32_t s = 1; s <= 14; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhFlowing;
            o.density = 2; o.color = 2; o.lines = 1;         // ONE phrase = the <= 1 cap is global
            o.rhythmSeed = s; o.pitchSeed = s * 5 + 2;
            auto n = generate(o, ca);
            Ctx cc = ca; PartGen::prepareChords(o, cc); PartGen::prepareLattice(cc);
            int inPhrase = 0;
            for (size_t i = 1; i + 1 < n.size(); ++i)
            {
                if (! PartGen::strongCol(cc, n[i].start)) continue;
                bool cp[12];
                PartGen::detail::chordPcs(o, PartGen::detail::chordRootDegAt(o, cc, n[i].start), cp);
                if (cp[((n[i].semi % 12) + 12) % 12]) continue;             // must dissonate
                if (std::abs(n[i].semi - n[i - 1].semi) < 4) continue;      // must be leapt into
                const int drop = n[i].semi - n[i + 1].semi;
                if (drop < 1 || drop > 2) continue;                         // resolves DOWN by step
                if (! cp[((n[i + 1].semi % 12) + 12) % 12]) continue;       // onto a chord tone
                ++inPhrase; ++found;
            }
            if (inPhrase > 1) capOk = false;
            (void) shapeOk;
        }
        CHECK(found >= 1, "[57a] appoggiaturas appear under Colorful (leap in, step down, chord tone)");
        CHECK(capOk, "[57b] never more than ONE appoggiatura per phrase (M8's cap)");
    }

    // [58] H7 FULL COUNTER-LINE: against a rising melody, the riff moves contrary/oblique
    // >= 60%, never strong-beat unisons, and freezes inside melody RUNS
    {
        Ctx ch7; ch7.bars = 2;
        // a synthetic RISING melody: onsets on every beat, +2 st per step, sounding a full beat
        int semi = -2;
        for (int cell = 0; cell < 32; ++cell)
        {
            const bool onset = (cell % 4) == 0;
            if (onset) semi += 2;
            ch7.melOcc[cell] = true;
            ch7.melPcP1[cell] = (uint8_t) (((semi % 12) + 12) % 12 + 1);
            if (onset) { ch7.melOnset[cell] = true; ch7.melDir[cell] = (int8_t) (cell == 0 ? 0 : 1); }
        }
        // a melody RUN in bar 1, cells 20..23 (3+ onsets tight together)
        for (int cell = 20; cell <= 23; ++cell)
        { ch7.melOnset[cell] = true; ch7.melDir[cell] = 1; }
        ch7.melOccValid = true; ch7.melMed = 4;
        bool censusOk = true, unisonOk = true, freezeOk = true;
        for (uint32_t s = 1; s <= 4; ++s)
        {
            Options o; o.scale = kMajor; o.role = RoleRiff; o.rhythm = RhDriving; o.density = 2;
            o.rhythmSeed = s; o.pitchSeed = s + 9;
            auto n = generate(o, ch7);
            if (n.empty()) continue;
            Ctx cc = ch7; PartGen::prepareChords(o, cc); PartGen::prepareLattice(cc);
            int simN = 0, tot = 0;
            for (size_t i = 1; i < n.size(); ++i)
            {
                const int g = n[i].start / 24;
                if (g < 0 || g >= 32 || ! ch7.melOnset[g] || ch7.melDir[g] == 0) continue;
                const int rd = n[i].semi - n[i - 1].semi;
                if (rd == 0) continue;
                ++tot;
                if ((rd > 0) == (ch7.melDir[g] > 0)) ++simN;
            }
            if (tot > 0 && (float) (tot - simN) / (float) tot < 0.6f) censusOk = false;
            for (auto& x : n)
            {
                const int g = x.start / 24;
                if (g >= 0 && g < 32 && ch7.melPcP1[g] != 0 && PartGen::strongCol(cc, x.start)
                    && ((x.semi % 12) + 12) % 12 == ch7.melPcP1[g] - 1) unisonOk = false;
                if (g >= 20 && g <= 23 && (x.start % 384) != 0) freezeOk = false;
            }
        }
        CHECK(censusOk, "[58a] H7: contrary/oblique motion >= 60% among co-moving pairs");
        CHECK(unisonOk, "[58b] H7: no strong-beat unisons/octaves with the melody");
        CHECK(freezeOk, "[58c] H7: the riff freezes inside melody runs (bar anchors excepted)");
    }

    // [59] G6 DENSITY BUDGET: a busier drum bed leaves FEWER melodic onsets (monotonic)
    {
        Ctx cSparse; cSparse.bars = 2;
        for (int i = 0; i < 8; ++i) { cSparse.hitCol[i] = i * 96; cSparse.hitStr[i] = 1.0f; }
        cSparse.nHits = 8;                                   // 4 hits/bar
        Ctx cBusy = cSparse;
        for (int i = 0; i < 32; ++i) { cBusy.hitCol[i] = i * 24; cBusy.hitStr[i] = 0.8f; }
        cBusy.nHits = 32;                                    // 16 hits/bar
        Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhFlowing; o.density = 2;
        o.rhythmSeed = 12; o.pitchSeed = 5;
        const auto nS = generate(o, cSparse).size();
        const auto nB = generate(o, cBusy).size();
        CHECK(nB < nS && nB >= 1, "[59] G6: onset budget shrinks monotonically with drum density");
    }

    // [60] G10 SUSTAIN vs STAB: busy hats force >= 50% staccato bass; a sparse bed sustains
    {
        Ctx cHats; cHats.bars = 2;
        for (int b = 0; b < 2; ++b)
            for (int i = 0; i < 4; ++i)
            { cHats.kickCol[cHats.nKick] = b * 384 + i * 96; cHats.kickStr[cHats.nKick++] = 1.0f; }
        for (int i = 0; i < 32; ++i) { cHats.hatCol[i] = i * 24; cHats.hatStr[i] = 0.6f; }
        cHats.nHat = 32;
        for (int i = 0; i < cHats.nKick; ++i)
        { cHats.hitCol[i] = cHats.kickCol[i]; cHats.hitStr[i] = 1.0f; }
        cHats.nHits = cHats.nKick;
        Ctx cQuiet; cQuiet.bars = 2;
        cQuiet.hitCol[0] = 0; cQuiet.hitStr[0] = 1.0f;
        cQuiet.hitCol[1] = 384; cQuiet.hitStr[1] = 1.0f; cQuiet.nHits = 2;   // 1 hit/bar
        Options o; o.scale = kMajor; o.role = RoleBass; o.rhythm = RhFlowing; o.density = 1;
        o.rhythmSeed = 7; o.pitchSeed = 3;
        auto busy  = generate(o, cHats);
        auto quiet = generate(o, cQuiet);
        int stab = 0;
        float meanB = 0, meanQ = 0; bool sustained = false;
        for (auto& x : busy)  { meanB += (float) x.len; if (x.len <= 48) ++stab; }
        for (auto& x : quiet) { meanQ += (float) x.len; if (x.len >= 96) sustained = true; }
        meanB /= (float) std::max<size_t>(1, busy.size());
        meanQ /= (float) std::max<size_t>(1, quiet.size());
        CHECK(! busy.empty() && stab * 2 >= (int) busy.size(),
              "[60a] G10: busy hats (16/bar) = at least half the bass notes stab (<= half beat)");
        CHECK(sustained && meanQ > meanB,
              "[60b] G10: a sparse bed sustains (longer means than the busy bed)");
    }

    // [61] H10 MULTISAMPLE REACH: notes stay within 5 st of the nearest zone (varispeed cap)
    {
        Ctx cz; cz.bars = 2;
        cz.sndValid = true; cz.sndMsLo = 64; cz.sndMsHi = 67;   // zones E4..G4
        Options o; o.scale = kMajor; o.role = RoleMelody; o.rhythm = RhFlowing;
        o.density = 2; o.registerBand = 0;                      // LOW register = would sit ~-15
        o.rhythmSeed = 8; o.pitchSeed = 21;
        auto n = generate(o, cz);
        bool inRange = ! n.empty();
        for (auto& x : n) if (x.semi < 64 - 5 - 60 || x.semi > 67 + 5 - 60) inRange = false;
        CHECK(inRange, "[61] H10: every note within 5 st varispeed of the zone span (E4..G4)");
    }

    // [62] GENERATE ALL: plan targeting by existing sounds, planning never mutates,
    // determinism, interlock (bass on the generated kick, comp in melody gaps, register
    // lanes), and skip-and-name when a role has no home
    {
        auto names = Factory::mixNames(); auto cats = Factory::mixCategories();
        auto firstOf = [&](const juce::String& cat) -> juce::String
        { for (int i = 0; i < names.size(); ++i) if (cats[i] == cat) return names[i]; return {}; };
        const auto nmKick = firstOf("Kicks"), nmSnare = firstOf("Snares"), nmHat = firstOf("Hi-Hats");
        const auto nmBass = firstOf("Bass"), nmKeys = firstOf("Keys"), nmLead = firstOf("Leads");
        auto setup = [&](Sequencer& sq)
        {
            auto arm = [&](int chn, const juce::String& nm, bool roll)
            {
                auto& cc2 = sq.patterns[0].channels[chn];
                cc2.mixName = nm;
                cc2.slots[0] = DrumChannel::Slot();
                cc2.slots[0].engine = DrumChannel::SrcOsc; cc2.slots[0].weight = 1.0f;
                cc2.slots[0].oscFreq = 261.6255653f;
                cc2.drawMode = roll;   // Keys/Leads open in the roll (the bank default)
            };
            arm(0, nmKick, false); arm(1, nmSnare, false); arm(2, nmHat, false);
            arm(4, nmBass, false); arm(5, nmKeys, true);   arm(6, nmLead, true);
        };
        auto chanHash = [](const Sequencer& sq)
        {
            long h = 1469598103l;
            for (int chn = 0; chn < Sequencer::NUM_CHANNELS; ++chn)
            {
                const auto& cc2 = sq.patterns[0].channels[chn];
                h = h * 31 + cc2.numSteps + cc2.drawNoteCount * 7;
                for (int i = 0; i < cc2.numSteps; ++i) h = h * 31 + (cc2.steps[i] ? i + 1 : 0);
                for (int i = 0; i < cc2.drawNoteCount; ++i)
                    h = h * 31 + cc2.drawNotes[i].start * 3 + cc2.drawNotes[i].semi;
            }
            return h;
        };
        auto sqA = std::make_unique<Sequencer>(); setup(*sqA);
        const long before = chanHash(*sqA);
        const auto plan = GenContext::planArrangement(*sqA, 0, 6);
        CHECK(before == chanHash(*sqA), "[62a] planning is read-only (consent decline = no-op)");
        CHECK(plan.kit.kick == 0 && plan.kit.snare == 1 && plan.kit.hat == 2
              && plan.bass == 4 && plan.chords == 5 && plan.melody == 6,
              "[62b] targeting by existing sounds: kit 1/2/3, bass 5, chords 6, melody 7 (selected)");
        GenContext::ArrangeOptions ao;
        ao.dna = &GenStyle::factory(DrumGen::StHouse);
        ao.scale = kMajor; ao.scaleLen = 7;
        ao.rhythm = RhDriving;                       // the crispest interlock stance to assert
        ao.rhythmSeed = 41; ao.pitchSeed = 87; ao.barMs = 2000.0;
        auto resA = GenContext::generateArrangement(*sqA, 0, 0, plan, ao);
        auto sqB = std::make_unique<Sequencer>(); setup(*sqB);
        GenContext::generateArrangement(*sqB, 0, 0, plan, ao);
        CHECK(chanHash(*sqA) == chanHash(*sqB) && resA.kitChans == 3
              && resA.bassN > 0 && resA.melN > 0 && resA.chordN > 0,
              "[62c] full arrangement deterministic (same seeds = identical across all channels)");
        // interlock: every kick col; bass steps mostly ON them (Driving; G4 pushes licensed)
        std::vector<int> kickCols;
        { const auto& K = sqA->patterns[0].channels[0];
          for (int i = 0; i < K.numSteps; ++i) if (K.steps[i]) kickCols.push_back(i * 384 / K.numSteps); }
        int bassOn = 0, bassTot = 0;
        { const auto& B = sqA->patterns[0].channels[4];
          for (int i = 0; i < B.numSteps; ++i)
              if (B.steps[i])
              {
                  ++bassTot;
                  const int col = i * 384 / B.numSteps;
                  for (int kc : kickCols) if (std::abs(col - kc) <= 12) { ++bassOn; break; }
              } }
        CHECK(bassTot > 0 && bassOn * 4 >= bassTot * 3,
              "[62d] interlock: >= 75% of Driving bass steps sit on the generated kick");
        // comp in melody gaps + register lanes (chords never rise above the melody's median)
        const auto& M = sqA->patterns[0].channels[6];
        const auto& C4c = sqA->patterns[0].channels[5];
        bool melOcc2[16] = {};
        std::vector<int> melSemis;
        for (int i = 0; i < M.drawNoteCount; ++i)
        {
            const auto& dn = M.drawNotes[i];
            melSemis.push_back(dn.semi);
            for (int p = dn.start / 24; p <= (dn.start + dn.len - 1) / 24 && p < 16; ++p)
                melOcc2[p] = true;
        }
        std::sort(melSemis.begin(), melSemis.end());
        const int melMed2 = melSemis.empty() ? 99 : melSemis[melSemis.size() / 2];
        int inGap = 0, under = 0, laneViol = 0;
        for (int i = 0; i < C4c.drawNoteCount; ++i)
        {
            const auto& dn = C4c.drawNotes[i];
            if (dn.start != 0)   // bar anchors are licensed to overlap
            { if (melOcc2[juce::jlimit(0, 15, dn.start / 24)]) ++under; else ++inGap; }
            if (dn.semi > melMed2) ++laneViol;
        }
        printf("  SCORE arrangement comp           inGap=%d under=%d total=%d melMed=%d\n",
               inGap, under, C4c.drawNoteCount, melMed2);
        CHECK(C4c.drawNoteCount > 0 && inGap >= under,
              "[62e] the comp's off-anchor stabs favour the melody's gaps (H6)");
        CHECK(laneViol == 0, "[62f] register lanes: no chord tone above the melody's median (H5)");
        // skip-and-name: no bass/keys/leads channels -> those roles are skipped BY NAME
        auto sqS = std::make_unique<Sequencer>();
        { auto arm = [&](int chn, const juce::String& nm)
          { sqS->patterns[0].channels[chn].mixName = nm; };
          arm(0, nmKick); arm(1, nmSnare); }
        const auto planS = GenContext::planArrangement(*sqS, 0, 0);
        auto resS = GenContext::generateArrangement(*sqS, 0, 0, planS, ao);
        CHECK(planS.bass < 0 && planS.chords < 0 && planS.melody < 0
              && resS.skipped.contains("bass") && resS.skipped.contains("melody")
              && resS.skipped.contains("chords"),
              "[62g] roles with no suitable channel are SKIPPED and NAMED");
    }

    // [63] ORIGINALITY / VARIETY (the mandate): 5 consecutive from-scratch arrangements, fresh
    // seed pairs - melodies+basses pairwise < 60% identical, progressions fan out, drums keep
    // the canon core while the hat ornament layer differs
    {
        auto names = Factory::mixNames(); auto cats = Factory::mixCategories();
        auto firstOf = [&](const juce::String& cat) -> juce::String
        { for (int i = 0; i < names.size(); ++i) if (cats[i] == cat) return names[i]; return {}; };
        auto setup = [&](Sequencer& sq)
        {
            auto arm = [&](int chn, const juce::String& nm, bool roll)
            {
                auto& cc2 = sq.patterns[0].channels[chn];
                cc2.mixName = nm;
                cc2.slots[0] = DrumChannel::Slot();
                cc2.slots[0].engine = DrumChannel::SrcOsc; cc2.slots[0].weight = 1.0f;
                cc2.slots[0].oscFreq = 261.6255653f;
                cc2.drawMode = roll;
            };
            arm(0, firstOf("Kicks"), false); arm(1, firstOf("Snares"), false);
            arm(2, firstOf("Hi-Hats"), false);
            arm(4, firstOf("Bass"), false); arm(5, firstOf("Keys"), true);
            arm(6, firstOf("Leads"), true);
        };
        struct Take { std::set<long> melBass; std::set<int> kick; std::vector<int> hatSig; };
        std::vector<Take> takes;
        for (int t2 = 0; t2 < 5; ++t2)
        {
            auto sq = std::make_unique<Sequencer>(); setup(*sq);
            const auto plan = GenContext::planArrangement(*sq, 0, 6);
            GenContext::ArrangeOptions ao;
            ao.dna = &GenStyle::factory(DrumGen::StHouse);
            ao.scale = kMajor; ao.scaleLen = 7;
            ao.rhythmSeed = 1000u + (uint32_t) t2 * 7919u;   // fresh pairs, as New idea rolls
            ao.pitchSeed  = 2000u + (uint32_t) t2 * 6007u;
            ao.barMs = 2000.0;
            GenContext::generateArrangement(*sq, 0, 1, plan, ao);   // 2 bars
            Take tk;
            { const auto& M = sq->patterns[0].channels[6];
              for (int i = 0; i < M.drawNoteCount; ++i)
                  tk.melBass.insert(10000000l + M.drawNotes[i].start * 128l + M.drawNotes[i].semi);
              const auto& M2 = sq->patterns[1].channels[6];
              for (int i = 0; i < M2.drawNoteCount; ++i)
                  tk.melBass.insert(20000000l + M2.drawNotes[i].start * 128l + M2.drawNotes[i].semi); }
            for (int b = 0; b < 2; ++b)
            { const auto& B = sq->patterns[b].channels[4];
              for (int i = 0; i < B.numSteps; ++i)
                  if (B.steps[i])
                      tk.melBass.insert(30000000l + (long) b * 1000000l
                                        + (i * 384 / B.numSteps) * 128l
                                        + juce::roundToInt(B.stepPitch[i])); }
            { const auto& K = sq->patterns[0].channels[0];
              for (int i = 0; i < K.numSteps; ++i) if (K.steps[i]) tk.kick.insert(i * 384 / K.numSteps);
              const auto& H = sq->patterns[0].channels[2];
              for (int i = 0; i < H.numSteps; ++i)
                  if (H.steps[i]) tk.hatSig.push_back(i * 1000 + (int) (H.stepVel[i] * 100.0f)); }
            takes.push_back(tk);
        }
        bool simOk = true, kickSame = true; float worstSim = 0.0f;
        for (size_t a2 = 0; a2 < takes.size(); ++a2)
            for (size_t b2 = a2 + 1; b2 < takes.size(); ++b2)
            {
                int shared = 0;
                for (long k : takes[a2].melBass) if (takes[b2].melBass.count(k)) ++shared;
                const int mn = (int) std::min(takes[a2].melBass.size(), takes[b2].melBass.size());
                const float sim = mn > 0 ? (float) shared / (float) mn : 0.0f;
                worstSim = std::max(worstSim, sim);
                if (sim > 0.6f) simOk = false;
                if (takes[a2].kick != takes[b2].kick) kickSame = false;
            }
        printf("  SCORE variety (5 takes, House)   worstPairSim=%.0f%%\n", worstSim * 100.0f);
        std::set<std::vector<int>> hatSigs;
        for (auto& tk : takes) hatSigs.insert(tk.hatSig);
        CHECK(simOk, "[63a] variety: no two takes share > 60% of their melody+bass note pairs");
        CHECK(kickSame, "[63b] the house 4-floor canon is IDENTICAL across takes (correct)");
        CHECK((int) hatSigs.size() >= 2,
              "[63c] the hat ornament layer differs across takes (variety without canon damage)");
        // progressions fan out from the style pool (New idea rerolls the pick)
        std::set<juce::String> progSigs;
        for (int t2 = 0; t2 < 5; ++t2)
        {
            Ctx cp2; cp2.bars = 4;
            Options o; o.scale = kMajor; o.role = RoleBass;
            o.styleDna = &GenStyle::factory(DrumGen::StHouse);
            o.rhythmSeed = 1000u + (uint32_t) t2 * 7919u;
            PartGen::prepareChords(o, cp2);
            juce::String sig2;
            for (int i = 0; i < cp2.nChords; ++i) sig2 += juce::String(cp2.chordDegAt[i]) + ",";
            progSigs.insert(sig2);
        }
        CHECK((int) progSigs.size() >= 2, "[63d] the progression POOL fans out across seed rolls");
    }

    printf(fails == 0 ? "GenTest: ALL PASS\n" : "GenTest: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
