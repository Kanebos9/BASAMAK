// LOOP-WRAP GATE regression (the "first step rings much longer from the second loop on" bug).
// At 120 BPM / 48 kHz a bar (96000 samples) is an EXACT multiple of common block sizes, so the
// loop wrap lands on a block edge and leaves a ~1e-16 floating-point remainder. That dust
// segment (spanSamples clamped to 1, span clamped to 1e-12) fired column-0/step-0 notes with a
// gate computed from the SEGMENT RATIO (bars/span*spanSamples) = ~3.4e11 samples = rings
// "forever" - and the seam dedupe then blocked the correct fire in the next block. The fix
// converts gates with the ABSOLUTE samples-per-bar and snaps dust remainders to 0.
// This test locks: every pass fires step 0 / note col 0 EXACTLY ONCE with the SAME sane gate.
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <map>

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 48000.0; const int bs = 500;      // bar = 96000 = 192 blocks EXACTLY
    const double barSamp = 2.0 * SR;

    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);
    // ch 0 = the user's step repro: 6 steps, step 0 + step 2 gated 75%
    auto& c0 = s->patterns[0].channels[0];
    for (auto& sl : c0.slots) sl = DrumChannel::Slot();
    c0.slots[0].engine = DrumChannel::SrcOsc; c0.slots[0].weight = 1.0f; c0.slots[0].oscFreq = 220.0f;
    c0.numSteps = 6; c0.steps[0] = c0.steps[2] = true;
    c0.stepNoteLen[0] = c0.stepNoteLen[2] = 0.75f;
    // ch 1 = the roll repro from the log: a gated note at column 0, len 132 columns
    auto& c1 = s->patterns[0].channels[1];
    for (auto& sl : c1.slots) sl = DrumChannel::Slot();
    c1.slots[0].engine = DrumChannel::SrcOsc; c1.slots[0].weight = 1.0f; c1.slots[0].oscFreq = 220.0f;
    c1.drawMode = true; c1.addDrawNote(0, 132, 0, 255);

    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    s->startStandalone();

    const int loops = 4, blocksPerBar = (int) (barSamp / bs);
    // per loop pass: fire count + gate of (ch0 step0) and (ch1 col-0 note)
    std::vector<int>  n0(loops, 0), n1(loops, 0);
    std::vector<long> g0(loops, -1), g1(loops, -1);
    juce::AudioBuffer<float> buf(2, bs);
    for (int b = 0; b < loops * blocksPerBar; ++b)
    {
        const int pass = b / blocksPerBar;
        buf.clear();
        auto evts = s->processBlock(buf, SR, bs, nullptr);
        for (const auto& e : evts)
        {
            if (e.channel == 0 && ! e.isDraw && e.step == 0) { ++n0[pass]; g0[pass] = e.gate; }
            if (e.channel == 1 && e.isDraw && e.offset >= 0 && e.gate > 0
                && b % blocksPerBar == 0)                     { ++n1[pass]; g1[pass] = e.gate; }
        }
    }
    delete s;

    const long exp0 = (long) (barSamp / 6.0 * 0.75);          // 12000
    const long exp1 = (long) (barSamp * 132.0 / 384.0);       // 33000
    bool ok = true;
    for (int p = 0; p < loops; ++p)
    {
        printf("[pass %d] step0 fires=%d gate=%ld (expect %ld) | roll col0 fires=%d gate=%ld (expect %ld)\n",
               p, n0[p], g0[p], exp0, n1[p], g1[p], exp1);
        ok = ok && n0[p] == 1 && n1[p] == 1
                && std::labs(g0[p] - exp0) <= 2 && std::labs(g1[p] - exp1) <= 2;
    }
    printf("[1] every pass fires ONCE with the same sane gate -> %s\n", CHK(ok) ? "OK" : "FAIL");
    return fails;
}
