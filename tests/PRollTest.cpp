// PIANO-ROLL rework verification (phases 2-5):
//  [1] melody notes play at their columns (C3 beat 1, G3 beat 2)
//  [2] an overlapping 3-note chord (beat 3) rings ALL THREE tones (overlap-aware trigger)
//  [3] quantize-to-steps helper: overlap detection flags chords, clean melodies pass
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
    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);   // 1 bar 4/4 = 2.0 s ; DRAW_RES 384 -> beat = 96 columns
    auto& ch = s->patterns[0].channels[0];
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    { auto& sl = ch.slots[0];
      sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
      sl.oscShape = sl.oscShapeB = DrumChannel::WvSaw; sl.oscFreq = 261.6256f;
      sl.atk = 0.002f; sl.dec = 0.4f; sl.sustain = 0.9f; sl.release = 0.05f; }
    ch.drawMode = true;
    ch.addDrawNote(0,   90, 0,  255);   // beat 1: C3
    ch.addDrawNote(96,  90, 7,  255);   // beat 2: G3
    ch.addDrawNote(192, 90, 0,  255);   // beat 3: chord C-E-G (three overlapping notes)
    ch.addDrawNote(192, 90, 4,  255);
    ch.addDrawNote(192, 90, 7,  255);
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    s->startStandalone();

    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    const int blocks = (int) (2.0 * SR / bs) + 1;
    for (int b = 0; b < blocks; ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }

    auto W = [&](double t0, double t1, double f){ return goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };
    const double C3 = 261.63, E3 = 329.63, G3 = 392.0;
    printf("[1] beat1 (C3 solo):  C=%.3f G=%.3f  -> %s\n", W(0.10,0.40,C3), W(0.10,0.40,G3),
           CHK(W(0.10,0.40,C3) > 0.02 && W(0.10,0.40,G3) < W(0.10,0.40,C3)*0.2) ? "OK" : "FAIL");
    printf("    beat2 (G3 solo):  C=%.3f G=%.3f  -> %s\n", W(0.60,0.90,C3), W(0.60,0.90,G3),
           CHK(W(0.60,0.90,G3) > 0.02 && W(0.60,0.90,C3) < W(0.60,0.90,G3)*0.35) ? "OK" : "FAIL");
    printf("[2] beat3 (CHORD):    C=%.3f E=%.3f G=%.3f  -> %s\n", W(1.10,1.40,C3), W(1.10,1.40,E3), W(1.10,1.40,G3),
           CHK(W(1.10,1.40,C3) > 0.02 && W(1.10,1.40,E3) > 0.02 && W(1.10,1.40,G3) > 0.02) ? "ALL RING (POLY ROLL OK)" : "FAIL");
    printf("[3] overlap detect:   hasOverlaps=%d (expect 1); after clearing chord: ", (int) ch.drawHasOverlaps());
    ch.drawNoteCount = 2;   // keep just the two melody notes
    printf("hasOverlaps=%d (expect 0)  -> %s\n", (int) ch.drawHasOverlaps(), CHK(! ch.drawHasOverlaps()) ? "OK" : "FAIL");
    delete s;

    // [4] ONE-SHOT notes = the STEP contract: on a SUSTAINED sound, a one-shot roll note must
    // render (near) bit-identical to the same bare step, while a gated note must differ
    // (sustain holds at 0.9 instead of decaying to zero).
    auto render = [&](bool draw, int oneShot) {
        auto* q = new Sequencer();
        q->setStandaloneBpm(120.0f);
        auto& c2 = q->patterns[0].channels[0];
        for (auto& sl : c2.slots) sl = DrumChannel::Slot();
        { auto& sl = c2.slots[0];
          sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
          sl.oscShape = sl.oscShapeB = DrumChannel::WvSaw; sl.oscFreq = 261.6255653f;   // exact C4 (roll base is C4-absolute now)
          sl.atk = 0.002f; sl.dec = 0.4f; sl.sustain = 0.9f; sl.release = 0.05f; }
        if (draw) { c2.drawMode = true; c2.addDrawNote(0, 96, 0, 255, 0, 0, oneShot); }
        else      { c2.numSteps = 16; c2.steps[0] = true; }
        for (auto& pp : q->patterns) for (auto& cc : pp.channels) cc.prepareToPlay(SR, bs);
        q->startStandalone();
        std::vector<float> o;
        juce::AudioBuffer<float> b2(2, bs);
        for (int b = 0; b < (int)(1.5 * SR / bs) + 1; ++b)
        { b2.clear(); q->processBlock(b2, SR, bs, nullptr); for (int i = 0; i < bs; ++i) o.push_back(b2.getSample(0, i)); }
        delete q;
        return o;
    };
    auto stepO = render(false, 0), oneO = render(true, 1), gateO = render(true, 0);
    double dOne = 0, dGate = 0;
    for (size_t i = 0; i < stepO.size() && i < oneO.size(); ++i) dOne  = std::max(dOne,  (double) std::abs(stepO[i] - oneO[i]));
    for (size_t i = 0; i < stepO.size() && i < gateO.size(); ++i) dGate = std::max(dGate, (double) std::abs(stepO[i] - gateO[i]));
    printf("[4] one-shot == step: maxdiff=%.6f (expect ~0); gated != step: maxdiff=%.3f (expect >0.05) -> %s\n",
           dOne, dGate, CHK(dOne < 1e-4 && dGate > 0.05) ? "OK" : "FAIL");
    return fails;
}
