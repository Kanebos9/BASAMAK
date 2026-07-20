// GenTest [2026-07-20] - locks the GENERATE feature's melody engine (PartGen.h).
// Pure headless checks: determinism, scale safety, register bounds, density ordering,
// seed-locked iteration (same rhythm / new notes), multi-bar + phrase echo, riff tiling,
// singable constraints, vary mutation, and the mono no-overlap guarantee.
#include "PartGen.h"
#include <cstdio>
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

    printf(fails == 0 ? "GenTest: ALL PASS\n" : "GenTest: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
