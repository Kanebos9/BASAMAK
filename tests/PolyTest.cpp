// POLY KEYS regression: [1] a held 3-note chord rings ALL THREE tones (poly keyDown = no cut),
// [2] keyUp(one note) releases ONLY that note's voice while the others sustain,
// [3] MONO keyDown still CUTS the previous note (the classic lead/slide feel is preserved).
#include "DrumChannel.h"
#include <cstdio>
#include <cmath>
#include <vector>

static double goertzel(const std::vector<float>& x, size_t a, size_t b, double f, double sr) {
    const double w = 2.0*M_PI*f/sr, c = 2*std::cos(w), sw = std::sin(w), cw = std::cos(w);
    double s1=0,s2=0; size_t n=0;
    for (size_t i=a; i<b && i<x.size(); ++i){ double s0=x[i]+c*s1-s2; s2=s1; s1=s0; ++n; }
    const double re=s1-s2*cw, im=s2*sw; return std::sqrt(re*re+im*im)/(0.5*(double)juce::jmax((size_t)1,n));
}

static void setupKeys(DrumChannel& ch) {
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    auto& sl = ch.slots[0];
    sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
    sl.oscShape = sl.oscShapeB = DrumChannel::WvSaw; sl.oscFreq = 261.6256f;   // C3 base
    sl.atk = 0.002f; sl.dec = 0.4f; sl.sustain = 0.9f; sl.release = 0.05f;
}

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 512;
    const double C3 = 261.63, E3 = 329.63, G3 = 392.0;

    auto render = [&](DrumChannel& ch, std::vector<float>& out, double sec) {
        juce::AudioBuffer<float> buf(2, bs);
        const int blocks = (int) (sec * SR / bs) + 1;
        for (int b = 0; b < blocks; ++b)
        { buf.clear(); ch.renderInto(buf, 0, bs, false); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
    };

    {   // [1] + [2]: POLY chord + per-note release
        DrumChannel ch; setupKeys(ch); ch.prepareToPlay(SR, bs);
        ch.keyDown(60, 1.0f, 0, true);   // C
        ch.keyDown(64, 1.0f, 0, true);   // E
        ch.keyDown(67, 1.0f, 0, true);   // G
        std::vector<float> out; render(ch, out, 0.5);
        auto W = [&](double t0, double t1, double f){ return goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };
        const double c1 = W(0.15,0.45,C3), e1 = W(0.15,0.45,E3), g1 = W(0.15,0.45,G3);
        printf("[1] held chord:   C=%.3f E=%.3f G=%.3f -> %s\n", c1, e1, g1,
               CHK(c1 > 0.05 && e1 > 0.05 && g1 > 0.05) ? "ALL RING (POLY OK)" : "FAIL");
        ch.keyUp(64);                    // release ONLY the E
        std::vector<float> out2; render(ch, out2, 0.5);
        auto W2 = [&](double t0, double t1, double f){ return goertzel(out2, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };
        const double c2 = W2(0.2,0.45,C3), e2 = W2(0.2,0.45,E3), g2 = W2(0.2,0.45,G3);
        printf("[2] keyUp(E):     C=%.3f E=%.3f G=%.3f -> %s\n", c2, e2, g2,
               CHK(c2 > 0.05 && g2 > 0.05 && e2 < c2 * 0.2) ? "E released, C+G sustain (OK)" : "FAIL");
    }
    {   // [3]: MONO second key cuts the first
        DrumChannel ch; setupKeys(ch); ch.prepareToPlay(SR, bs);
        ch.keyDown(60, 1.0f, 0, false);  // C (mono)
        { std::vector<float> pre; render(ch, pre, 0.1); }   // let it establish
        ch.keyDown(67, 1.0f, 0, false);  // G cuts it
        std::vector<float> out; render(ch, out, 0.4);
        auto W = [&](double t0, double t1, double f){ return goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };
        const double c3 = W(0.15,0.35,C3), g3 = W(0.15,0.35,G3);
        printf("[3] mono handover: C=%.3f G=%.3f -> %s\n", c3, g3,
               CHK(g3 > 0.05 && c3 < g3 * 0.15) ? "old note cut (MONO OK)" : "FAIL");
    }
    return fails;
}
