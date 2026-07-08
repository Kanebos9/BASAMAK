// LIVE == RECORDING regression for MODAL sus-0 sounds (the Mod Kalimba bug): a piano-roll
// gated note's voice carries its keyOff (gate end) IN ADVANCE from trigger(); the modal sus-0
// key-release branch used decayCurve(t - keyOff) unconditionally, so t < keyOff fed NEGATIVE
// time = exp(+t) = the note EXPLODED (~240,000x, peak 74028) - heard as "recording louder than
// live" through the master soft-clip. Locks: roll playback == live keys (energy + aligned
// waveform), and no blow-up.
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0;
static const int    BS = 480;          // 0.25 s = 25 blocks exactly

static void kalimba(DrumChannel& c)    // == FactoryContent moKalimba (sustain 0 modal)
{
    for (auto& sl : c.slots) sl = DrumChannel::Slot();
    auto& s = c.slots[0];
    s.engine = DrumChannel::SrcModal; s.weight = 1.0f;
    // base = C4: the editor PINS a piano-roll channel's base freq to C4 (refreshDetailPanel),
    // and live keyDown targets the pressed note absolutely - so in the plugin both paths play
    // C4. The headless harness has no editor, so bake the pin here (authored base is 440).
    s.modalMaterial = 6; s.oscFreq = 261.6255653f; s.modalDecay = 0.4f;
    s.modalTone = 0.5f; s.modalStruct = 0.5f;
    c.volume = 0.82f;
}

// live = keyDown 0.25 s then keyUp; roll = the recorded equivalent (gated note, 48 columns)
static std::vector<float> render(bool live)
{
    auto* q = new Sequencer();
    q->setStandaloneBpm(120.0f);       // bar = 2.0 s -> 0.25 s = 48 roll columns
    auto& c = q->patterns[0].channels[0];
    kalimba(c);
    for (auto& p : q->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, BS);
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, BS);
    const int blocks = (int) (1.8 * SR / BS);   // < 1 bar so nothing re-fires
    if (live)
    {
        c.keyDown(60, 1.0f, 0, /*poly*/ true);
        for (int b = 0; b < blocks; ++b)
        {
            if (b == 25) c.keyUp(60);
            buf.clear(); c.renderInto(buf, 0, BS, false);
            for (int i = 0; i < BS; ++i) out.push_back(buf.getSample(0, i));
        }
    }
    else
    {
        c.drawMode = true; c.addDrawNote(0, 48, 0, 255);
        q->startStandalone();
        for (int b = 0; b < blocks; ++b)
        { buf.clear(); q->processBlock(buf, SR, BS, nullptr); for (int i = 0; i < BS; ++i) out.push_back(buf.getSample(0, i)); }
    }
    delete q;
    return out;
}

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const auto lv = render(true), rl = render(false);

    double pkL = 0, pkR = 0, eL = 0, eR = 0;
    for (auto v : lv) { pkL = std::max(pkL, (double) std::abs(v)); eL += (double) v * v; }
    for (auto v : rl) { pkR = std::max(pkR, (double) std::abs(v)); eR += (double) v * v; }
    printf("[1] no blow-up: live peak=%.3f roll peak=%.3f (expect <1)    -> %s\n",
           pkL, pkR, CHK(pkL < 1.0 && pkR < 1.0) ? "OK" : "FAIL");
    const double er = eR / std::max(1e-9, eL);
    printf("[2] roll/live energy ratio=%.4f (expect 0.98-1.02)           -> %s\n",
           er, CHK(er > 0.98 && er < 1.02) ? "OK" : "FAIL");

    // onset-aligned waveform diff (the two clocks start at different offsets)
    auto onset = [](const std::vector<float>& x){ for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > 1e-4f) return (long) i; return (long) 0; };
    const long sh = onset(&rl == &rl ? rl : rl) - onset(lv);
    double d = 0; long n2 = 0;
    for (long i = 0; i + std::max((long) 0, sh) < (long) rl.size() && i + std::max((long) 0, -sh) < (long) lv.size(); ++i)
    { d = std::max(d, (double) std::abs(rl[(size_t)(i + std::max((long) 0, sh))] - lv[(size_t)(i + std::max((long) 0, -sh))])); ++n2; }
    printf("[3] onset-aligned maxdiff=%.4f over %ld samples (expect <0.05) -> %s\n",
           d, n2, CHK(d < 0.05 && n2 > (long) SR) ? "OK" : "FAIL");
    return fails;
}
