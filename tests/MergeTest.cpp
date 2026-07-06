// PATTERN MERGE verification: pattern 0 (C3 hit) + pattern 1 (G3 hit) merged -> playback must run
// bar1 = P0 (C), bar2 = P1 (G), bar3 = P0 again (loop through the group).
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

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 1024;
    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);                       // 1 bar = 2.0 s
    mkTone(s->patterns[0].channels[0], 261.6256f);     // P0 = C3
    mkTone(s->patterns[1].channels[0], 392.0f);        // P1 = G3
    s->patterns[1].mergeWithPrev = true;               // MERGE: P0+P1 = one 2-bar unit
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    s->startStandalone();

    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    const int blocks = (int) (6.0 * SR / bs) + 1;      // 3 bars
    for (int b = 0; b < blocks; ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }

    auto W = [&](double t0, double t1, double f){ return goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };
    const double C3 = 261.63, G3 = 392.0;
    const double b1C = W(0.05,0.45,C3), b1G = W(0.05,0.45,G3);
    const double b2C = W(2.05,2.45,C3), b2G = W(2.05,2.45,G3);
    const double b3C = W(4.05,4.45,C3), b3G = W(4.05,4.45,G3);
    printf("bar1: C=%.3f G=%.3f -> %s\n", b1C, b1G, CHK(b1C > 0.02 && b1G < b1C*0.2) ? "P0 plays (OK)" : "FAIL");
    printf("bar2: C=%.3f G=%.3f -> %s\n", b2C, b2G, CHK(b2G > 0.02 && b2C < b2G*0.4) ? "P1 plays (MERGE ADVANCE OK)" : "FAIL");
    printf("bar3: C=%.3f G=%.3f -> %s\n", b3C, b3G, CHK(b3C > 0.02 && b3G < b3C*0.4) ? "back to P0 (GROUP LOOP OK)" : "FAIL");
    delete s;
    return fails;
}
