// PER-STEP NUDGE regression: a hit on step 3-of-4 (bar start + 1.0 s at 120 BPM) nudged fully LATE
// must fire ~0.25 s later (half its 0.5 s span); nudged EARLY, half a span sooner; nudge 0 must be
// bit-identical to a build that never touched the field.
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static std::vector<float> render(float nudgeVal) {
    const double SR = 48000.0; const int bs = 512;
    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);   // 1 bar = 2.0 s; 4 steps -> step 2 starts at 1.0 s, span 0.5 s
    auto& ch = s->patterns[0].channels[0];
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    auto& sl = ch.slots[0];
    sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
    sl.oscShape = sl.oscShapeB = DrumChannel::WvSaw; sl.oscFreq = 220.0f;
    sl.atk = 0.001f; sl.dec = 0.08f;
    ch.numSteps = 4; ch.steps[2] = true; ch.stepNudge[2] = nudgeVal;
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    s->startStandalone();
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    const int blocks = (int) (2.0 * SR / bs);
    for (int b = 0; b < blocks; ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
    delete s;
    return out;
}

static double onsetSec(const std::vector<float>& x, double sr) {
    for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > 0.01f) return (double) i / sr;
    return -1.0;
}

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 48000.0;
    auto base = render(0.0f), late = render(1.0f), early = render(-1.0f);
    const double tb = onsetSec(base, SR), tl = onsetSec(late, SR), te = onsetSec(early, SR);
    printf("[1] onsets: base=%.3fs late=%.3fs early=%.3fs\n", tb, tl, te);
    printf("[2] late  fires +%.3fs (expect ~+0.25) -> %s\n", tl - tb, CHK(std::abs((tl - tb) - 0.25) < 0.02) ? "OK" : "FAIL");
    printf("[3] early fires %.3fs (expect ~-0.25) -> %s\n", te - tb, CHK(std::abs((te - tb) + 0.25) < 0.02) ? "OK" : "FAIL");
    double maxdiff = 0;
    { auto b2 = render(0.0f);
      for (size_t i = 0; i < base.size() && i < b2.size(); ++i) maxdiff = juce::jmax(maxdiff, (double) std::abs(base[i] - b2[i])); }
    printf("[4] nudge 0 repeatable: maxdiff=%.9f -> %s\n", maxdiff, CHK(maxdiff == 0.0) ? "OK" : "FAIL");
    return fails;
}
