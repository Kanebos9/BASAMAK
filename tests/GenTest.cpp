// GenTest [2026-07-20] - locks the GENERATE feature's melody engine (PartGen.h).
// Pure headless checks: determinism, scale safety, register bounds, density ordering,
// seed-locked iteration (same rhythm / new notes), multi-bar + phrase echo, riff tiling,
// singable constraints, vary mutation, and the mono no-overlap guarantee.
// [17]-[21] [2026-07-21 r18] the CONTEXT GATHER (GenContext.h, extracted from the editor):
// exact 7-step hit columns, merged-group bar offsets, mute/solo exclusion, self-exclusion,
// and STEP-PITCH harmony (Freq-knob base + stepPitch -> chroma + key detection).
// [22] POCKET DISCIPLINE: no onset within a grid cell of a drum hit, no note ringing into one.
#include "PartGen.h"
#include "GenContext.h"
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  PASS  %s\n", msg); \
                              else { printf("  FAIL  %s\n", msg); ++fails; } } while (0)

static const int8_t kMajor[7] = { 0, 2, 4, 5, 7, 9, 11 };

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

    printf(fails == 0 ? "GenTest: ALL PASS\n" : "GenTest: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
