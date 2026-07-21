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
struct GenScore { float lhl = 0, chordStrong = 0, prox = 0, ngram = 0; int leapViol = 0, n = 0; };
static GenScore scoreGen(const std::vector<PartGen::Note>& ns,
                         const PartGen::Options& o, const PartGen::Ctx& cIn)
{
    GenScore sc; sc.n = (int) ns.size();
    if (ns.empty()) return sc;
    for (int b = 0; b < cIn.bars; ++b) sc.lhl += (float) PartGen::lhlBarScore(ns, b);
    sc.lhl /= (float) cIn.bars;
    PartGen::Ctx cc = cIn;
    PartGen::prepareChords(o, cc);          // the SAME chord timeline the generator builds
    int strong = 0, ct = 0, steps = 0, tot = 0;
    for (auto& x : ns)
        if (x.start % 96 == 0)
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
    printf("  SCORE %-28s n=%2d LHL=%.1f chordStrong=%.0f%% proximity=%.0f%% leapViol=%d ngramSim=%.0f%%\n",
           tag, s.n, s.lhl, s.chordStrong * 100.0f, s.prox * 100.0f, s.leapViol, s.ngram * 100.0f);
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

    printf(fails == 0 ? "GenTest: ALL PASS\n" : "GenTest: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
