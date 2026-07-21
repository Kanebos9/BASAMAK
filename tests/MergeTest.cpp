// PATTERN MERGE verification [1.5.0 FINAL = the user's spec verbatim: "N shows the loops count
// of THAT pattern, period"]. Every bar counts its OWN consecutive plays; unmet = the bar plays
// AGAIN (no group-flow invention). Merge defaults: each bar -> next x1, last -> head x1.
// [1] defaults = the classic group loop: P0, P1, P0. [2] a bar set "next after 2" plays ITSELF
// twice: P0, P0, P1. [3] a bar chains OUT: P0, P1, P2. [4] end bar "to P2 after 2 loops" =
// the END BAR plays twice, then leaves: P0, P1, P1, P2. [5] group-wide mute [1.5.6]: mute on
// all group bars = silent across the whole group (the UI writes every bar now).
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static double goertzel(const std::vector<float>& x, size_t a, size_t b, double f, double sr) {
    const double w = 2.0*M_PI*f/sr, c = 2*std::cos(w), sw = std::sin(w), cw = std::cos(w);
    double s1=0,s2=0; size_t n=0;
    for (size_t i=a; i<b && i<x.size(); ++i){ double s0=x[i]+c*s1-s2; s2=s1; s1=s0; ++n; }
    const double re=s1-s2*cw, im=s2*sw; return std::sqrt(re*re+im*im)/(0.5*(double)juce::jmax((size_t)1,n));
}

static void mkTone(DrumChannel& ch, float hz) {
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    auto& sl = ch.slots[0];
    sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
    sl.oscShape = sl.oscShapeB = DrumChannel::WvSaw; sl.oscFreq = hz;
    sl.atk = 0.002f; sl.dec = 0.5f;
    ch.numSteps = 4; ch.steps[0] = true;   // one hit at the bar start
}
static void chainTo(Sequencer::Pattern& p, int tgt, int loops) {
    p.playMode = Sequencer::Chain; p.chainLen = 1; p.chainStep = 0;
    p.chainSeq[0] = tgt; p.chainLoops[0] = loops;
}
static void chainTo2(Sequencer::Pattern& p, int t0, int l0, int t1, int l1) {
    p.playMode = Sequencer::Chain; p.chainLen = 2; p.chainStep = 0;
    p.chainSeq[0] = t0; p.chainLoops[0] = l0; p.chainSeq[1] = t1; p.chainLoops[1] = l1;
}

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 1024;
    const double C3 = 261.63, G3 = 392.0, E4 = 659.26;

    auto run = [&](int scen, const int* expect, int nbars) {
        auto* s = new Sequencer();
        s->setStandaloneBpm(120.0f);                       // 1 bar = 2.0 s
        mkTone(s->patterns[0].channels[0], 261.6256f);     // P0 = C3
        mkTone(s->patterns[1].channels[0], 392.0f);        // P1 = G3
        mkTone(s->patterns[2].channels[0], 659.26f);       // P2 = E4 (outside the group)
        s->patterns[1].mergeWithPrev = true;               // MERGE P0+P1 (editing unit)
        // the merge DEFAULTS the editor writes: each bar -> next x1, end -> head x1
        chainTo(s->patterns[0], 1, 1);
        chainTo(s->patterns[1], 0, 1);
        if (scen == 1) chainTo(s->patterns[0], 1, 2);   // P0 plays ITSELF twice, then -> P1
        if (scen == 2) chainTo(s->patterns[1], 2, 1);   // P1 escapes the group -> P2
        if (scen == 3) chainTo(s->patterns[1], 2, 2);   // end bar: plays ITSELF twice, then -> P2
        for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
        s->startStandalone();
        std::vector<float> out;
        juce::AudioBuffer<float> buf(2, bs);
        for (int b = 0; b < (int) (2.0 * nbars * SR / bs) + 1; ++b)
        { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
        delete s;
        const double fr[3] = { C3, G3, E4 }; const char* nm[3] = { "P0", "P1", "P2" };
        bool ok = true;
        for (int bar = 0; bar < nbars; ++bar)
        {
            const double t0 = bar * 2.0 + 0.05, t1 = bar * 2.0 + 0.45;
            const double want = goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), fr[expect[bar]], SR);
            double others = 0.0;
            for (int o = 0; o < 3; ++o) if (o != expect[bar])
                others = juce::jmax(others, goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), fr[o], SR));
            ok = ok && want > 0.02 && others < want * 0.4;
            printf("  bar%d expect %s: got=%.3f others=%.3f\n", bar + 1, nm[expect[bar]], want, others);
        }
        return ok;
    };
    int e0[3] = { 0, 1, 0 }, e1[3] = { 0, 0, 1 }, e2[3] = { 0, 1, 2 }, e3[4] = { 0, 1, 1, 2 };
    printf("[1] merge defaults (group loop):\n");            if (! CHK(run(0, e0, 3))) printf("  FAIL\n");
    printf("[2] bar 'next after 2' plays itself twice:\n");  if (! CHK(run(1, e1, 3))) printf("  FAIL\n");
    printf("[3] bar chains OUT of the group:\n");            if (! CHK(run(2, e2, 3))) printf("  FAIL\n");
    printf("[4] end bar 'P2 after 2': plays twice, out:\n"); if (! CHK(run(3, e3, 4))) printf("  FAIL\n");

    // [5] GROUP-WIDE MUTE [1.5.6]: the editor's M/S/OV toggles write EVERY bar of a merged group
    // (head..end). This locks the Sequencer-side contract: mute set on ALL group bars = the channel
    // is silent across the WHOLE group (with mute on only the viewed bar, the other bar still
    // sounded = the shipped bug).
    {
        auto* s = new Sequencer();
        s->setStandaloneBpm(120.0f);
        mkTone(s->patterns[0].channels[0], 261.6256f);     // P0 = C3
        mkTone(s->patterns[1].channels[0], 392.0f);        // P1 = G3
        s->patterns[1].mergeWithPrev = true;
        chainTo(s->patterns[0], 1, 1); chainTo(s->patterns[1], 0, 1);
        // the group-wide write the UI now performs (forGroupChannel over head..end):
        s->patterns[0].channels[0].mute = true;
        s->patterns[1].channels[0].mute = true;
        for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
        s->startStandalone();
        std::vector<float> out;
        juce::AudioBuffer<float> buf(2, bs);
        for (int b = 0; b < (int) (2.0 * 2 * SR / bs) + 1; ++b)
        { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
        delete s;
        const double m0 = goertzel(out, (size_t)(0.05*SR), (size_t)(0.45*SR), C3, SR);   // bar 1 (P0)
        const double m1 = goertzel(out, (size_t)(2.05*SR), (size_t)(2.45*SR), G3, SR);   // bar 2 (P1)
        printf("[5] group-wide mute: bar1 C3=%.5f bar2 G3=%.5f (both must be silent)\n", m0, m1);
        if (! CHK(m0 < 0.002 && m1 < 0.002)) printf("  FAIL\n");
    }
    printf(fails ? ">>> MergeTest FAIL (%d)\n" : ">>> MergeTest PASS\n", fails);
    return fails;
}
