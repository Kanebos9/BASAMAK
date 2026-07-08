// LOCK PITCH verification: a locked slot ignores note pitch from every source.
//  [1] step pitch +12 on a LOCKED slot renders bit-identical to pitch 0
//  [2] the same +12 on an UNLOCKED slot renders an octave up (differs)
//  [3] a locked slot ignores keyDown transposition too
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static std::vector<float> render(bool lock, float stepPitch, int key)
{
    const double SR = 48000.0; const int bs = 512;
    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);
    auto& ch = s->patterns[0].channels[0];
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    auto& sl = ch.slots[0];
    sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
    sl.oscShape = sl.oscShapeB = DrumChannel::WvSaw; sl.oscFreq = 220.0f;
    sl.atk = 0.002f; sl.dec = 0.3f;
    sl.lockPitch = lock;
    ch.numSteps = 16;
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    if (key > 0) ch.keyDown(key, 1.0f, 0, true);
    else { ch.steps[0] = true; ch.stepPitch[0] = stepPitch; s->startStandalone(); }
    for (int b = 0; b < (int)(0.5 * SR / bs); ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
    delete s;
    return out;
}

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    auto diff = [](const std::vector<float>& a, const std::vector<float>& b)
    { double d = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, (double) std::abs(a[i] - b[i])); return d; };

    const double dL = diff(render(true, 12.0f, 0), render(true, 0.0f, 0));
    printf("[1] LOCKED  +12 vs 0 step pitch: maxdiff=%.6f (expect 0)      -> %s\n", dL, CHK(dL < 1e-6) ? "OK" : "FAIL");
    const double dU = diff(render(false, 12.0f, 0), render(false, 0.0f, 0));
    printf("[2] UNLOCKED +12 vs 0 step pitch: maxdiff=%.3f (expect >0.1)  -> %s\n", dU, CHK(dU > 0.1) ? "OK" : "FAIL");
    const double dK = diff(render(true, 0.0f, 72), render(true, 0.0f, 60));
    printf("[3] LOCKED keys C5 vs C4:        maxdiff=%.6f (expect 0)      -> %s\n", dK, CHK(dK < 1e-6) ? "OK" : "FAIL");
    return fails;
}
