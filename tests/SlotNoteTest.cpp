// PER-SLOT NOTE tags: slot 1 = a low saw (C3 base), slot 2 = a high saw (C5 base). Draw one note at
// each of three columns tagged both / slot1-only / slot2-only; verify each column sounds the right
// slot(s). slot 0 (both) must be bit-identical to the pre-feature "both slots" behaviour.
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

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 1024;
    const double C3 = 261.63, C5 = 1046.5;

    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);   // 1 bar = 2.0 s; DRAW_RES 384 -> beat = 96 cols
    auto& ch = s->patterns[0].channels[0];
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    ch.slots[0].engine = DrumChannel::SrcOsc; ch.slots[0].weight = 0.5f;
    ch.slots[0].oscShape = ch.slots[0].oscShapeB = DrumChannel::WvSine; ch.slots[0].oscFreq = 261.6256f;   // C3
    ch.slots[0].atk = 0.002f; ch.slots[0].dec = 0.5f;                    // sines: C3 has NO energy at C5
    ch.slots[1].engine = DrumChannel::SrcOsc; ch.slots[1].weight = 0.5f;
    ch.slots[1].oscShape = ch.slots[1].oscShapeB = DrumChannel::WvSine; ch.slots[1].oscFreq = 1046.502f;   // C5
    ch.slots[1].atk = 0.002f; ch.slots[1].dec = 0.5f;
    ch.drawMode = true;
    ch.addDrawNote(0,   80, 0, 255, 0);   // beat 1: BOTH
    ch.addDrawNote(96,  80, 0, 255, 1);   // beat 2: slot 1 only (C3)
    ch.addDrawNote(192, 80, 0, 255, 2);   // beat 3: slot 2 only (C5)
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    s->startStandalone();

    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    const int blocks = (int) (2.0 * SR / bs) + 1;
    for (int b = 0; b < blocks; ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
    auto W = [&](double t0, double t1, double f){ return goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };

    const double b1c3 = W(0.05,0.45,C3), b1c5 = W(0.05,0.45,C5);
    const double b2c3 = W(0.55,0.95,C3), b2c5 = W(0.55,0.95,C5);
    const double b3c3 = W(1.05,1.45,C3), b3c5 = W(1.05,1.45,C5);
    printf("[1] BOTH:      C3=%.3f C5=%.3f -> %s\n", b1c3, b1c5, CHK(b1c3 > 0.02 && b1c5 > 0.02) ? "both slots (OK)" : "FAIL");
    printf("[2] slot1 only C3=%.3f C5=%.3f -> %s\n", b2c3, b2c5, CHK(b2c3 > 0.02 && b2c5 < b2c3 * 0.15) ? "only C3 (OK)" : "FAIL");
    printf("[3] slot2 only C3=%.3f C5=%.3f -> %s\n", b3c3, b3c5, CHK(b3c5 > 0.02 && b3c3 < b3c5 * 0.15) ? "only C5 (OK)" : "FAIL");
    delete s;
    return fails;
}
