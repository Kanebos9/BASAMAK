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
    int dup = 0, tg = 0;                    // motif economy: repeated interval 3-grams
    std::vector<long> seen;
    for (size_t i = 3; i < ns.size(); ++i)
    {
        long key = 0;
        for (int k = 0; k < 3; ++k) key = key * 200 + (long) (ns[i - 2 + k].semi - ns[i - 3 + k].semi + 90);
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

    printf(fails == 0 ? "GenTest: ALL PASS\n" : "GenTest: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
