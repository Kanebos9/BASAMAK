// PATTERN MERGE verification [1.5.0 = PER-BAR play modes]. Merged groups are an EDITING unit;
// playback follows each bar's OWN mode. [1] the merge DEFAULTS (bar->next x1, end->head x1)
// reproduce the classic group loop: P0, P1, P0. [2] a bar can REPEAT ITSELF (P0 x2): P0, P0, P1.
// [3] a bar can chain OUT of the group (P1 -> P2): P0, P1, P2.
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

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 1024;
    const double C3 = 261.63, G3 = 392.0, E4 = 659.26;

    auto run = [&](int scen, int expect[3]) {
        auto* s = new Sequencer();
        s->setStandaloneBpm(120.0f);                       // 1 bar = 2.0 s
        mkTone(s->patterns[0].channels[0], 261.6256f);     // P0 = C3
        mkTone(s->patterns[1].channels[0], 392.0f);        // P1 = G3
        mkTone(s->patterns[2].channels[0], 659.26f);       // P2 = E4 (outside the group)
        s->patterns[1].mergeWithPrev = true;               // MERGE P0+P1 (editing unit)
        // the merge DEFAULTS the editor writes: each bar -> next x1, end -> head x1
        chainTo(s->patterns[0], 1, 1);
        chainTo(s->patterns[1], 0, 1);
        if (scen == 1) chainTo(s->patterns[0], 1, 2);      // P0 repeats ITSELF twice first
        if (scen == 2) chainTo(s->patterns[1], 2, 1);      // P1 escapes the group -> P2
        for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
        s->startStandalone();
        std::vector<float> out;
        juce::AudioBuffer<float> buf(2, bs);
        for (int b = 0; b < (int) (6.0 * SR / bs) + 1; ++b)
        { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
        delete s;
        const double fr[3] = { C3, G3, E4 }; const char* nm[3] = { "P0", "P1", "P2" };
        bool ok = true;
        for (int bar = 0; bar < 3; ++bar)
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
    int e0[3] = { 0, 1, 0 }, e1[3] = { 0, 0, 1 }, e2[3] = { 0, 1, 2 };
    printf("[1] merge defaults (group loop):\n");   if (! CHK(run(0, e0))) printf("  FAIL\n");
    printf("[2] middle bar repeats itself x2:\n");  if (! CHK(run(1, e1))) printf("  FAIL\n");
    printf("[3] bar chains OUT of the group:\n");   if (! CHK(run(2, e2))) printf("  FAIL\n");
    printf(fails ? ">>> MergeTest FAIL (%d)\n" : ">>> MergeTest PASS\n", fails);
    return fails;
}
