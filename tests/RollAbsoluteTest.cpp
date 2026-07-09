// PIANO ROLL plays ABSOLUTE, C4-referenced pitch, IGNORING the Freq knob (user: the roll must not
// care about the knob). A draw note at semi 0 plays C4 (~261.6 Hz) even when the slot's Freq knob
// sits at A1 (55 Hz). STEP mode still uses the knob. The Tune fader shifts BOTH the roll and live.
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0; static const int BS = 480;

// Render a channel (draw note at semi 0) or a step (pitch 0) and return the fundamental Hz via
// zero-crossing rate of a sine.
static double fundamental(bool draw, float knobHz, float tuneCents) {
    auto* q = new Sequencer();
    q->setStandaloneBpm(120.0f);
    auto& c = q->patterns[0].channels[0];
    for (auto& sl : c.slots) sl = DrumChannel::Slot();
    auto& s = c.slots[0];
    s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = knobHz;
    s.atk = 0.002f; s.dec = 0.6f;
    if (draw) { c.drawMode = true; c.drawTuneCents = tuneCents; c.addDrawNote(0, 96, 0, 255); }
    else      { c.numSteps = 4; c.steps[0] = true; }
    for (auto& p : q->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, BS);
    q->startStandalone();
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, BS);
    for (int b = 0; b < (int)(0.35 * SR / BS); ++b)
    { buf.clear(); q->processBlock(buf, SR, BS, nullptr); for (int i = 0; i < BS; ++i) out.push_back(buf.getSample(0, i)); }
    delete q;
    // count zero-crossings over the steady portion (skip attack)
    int zc = 0; const int a = (int)(0.05 * SR), bnd = (int) out.size();
    for (int i = a + 1; i < bnd; ++i) if ((out[i - 1] <= 0.0f) != (out[i] <= 0.0f)) ++zc;
    const double secs = (double)(bnd - a) / SR;
    return (double) zc / (2.0 * secs);
}

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double C4 = 261.6255653;

    const double rollA1 = fundamental(true, 55.0f, 0.0f);   // draw, knob at A1 -> should be C4
    printf("[1] roll note, knob=A1(55Hz): got %.1f Hz (expect ~%.1f = C4) -> %s\n",
           rollA1, C4, CHK(std::abs(rollA1 - C4) < 6.0) ? "OK" : "FAIL");

    const double rollBass = fundamental(true, 110.0f, 0.0f);  // knob at A2 -> still C4 (knob ignored)
    printf("[2] roll note, knob=110Hz: got %.1f Hz (expect ~%.1f = C4, knob ignored) -> %s\n",
           rollBass, C4, CHK(std::abs(rollBass - C4) < 6.0) ? "OK" : "FAIL");

    const double stepA1 = fundamental(false, 55.0f, 0.0f);    // STEP mode still uses the knob
    printf("[3] step note, knob=A1(55Hz): got %.1f Hz (expect ~55 = knob) -> %s\n",
           stepA1, CHK(std::abs(stepA1 - 55.0) < 3.0) ? "OK" : "FAIL");

    const double tuneUp = fundamental(true, 55.0f, 50.0f);    // Tune +50c shifts the roll up
    const double want = C4 * std::pow(2.0, 50.0 / 1200.0);
    printf("[4] roll note + Tune +50c: got %.1f Hz (expect ~%.1f) -> %s\n",
           tuneUp, want, CHK(std::abs(tuneUp - want) < 6.0) ? "OK" : "FAIL");
    return fails;
}
