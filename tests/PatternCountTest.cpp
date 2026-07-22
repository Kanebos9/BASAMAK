// PatternCountTest [r24]: pattern capacity raised 32 -> 64. Locks the TOP of the new range:
// [1] NUM_PATTERNS == 64 (compile-time);
// [2] steps written into pattern 63 actually PLAY (P63 renders its tone);
// [3] a chain 63 -> 0 wraps correctly (the user's "play P63 twice, then jump to P0") and
//     barPlays[63] counts THAT bar's own plays (1 mid-second-pass, 2 after both);
// [4] pattern 63's per-pattern MASTER storage is independent (written values hold, P0 untouched).
// + info line: sizeof(Pattern)/sizeof(Sequencer) = the idle-memory cost of the extra 32 patterns.
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static_assert(Sequencer::NUM_PATTERNS == 64, "r24: pattern capacity must be 64");

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
    const double C3 = 261.63, E4 = 659.26;

    printf("[info] sizeof(Pattern) = %.2f KB, sizeof(Sequencer) = %.2f MB "
           "(the 32 extra patterns ~= %.2f MB of idle struct)\n",
           sizeof(Sequencer::Pattern) / 1024.0, sizeof(Sequencer) / (1024.0 * 1024.0),
           32.0 * sizeof(Sequencer::Pattern) / (1024.0 * 1024.0));

    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);                        // 1 bar = 2.0 s
    mkTone(s->patterns[63].channels[0], 261.6256f);     // P63 = C3 (steps in the LAST pattern)
    mkTone(s->patterns[0].channels[0],  659.26f);       // P0  = E4
    // chain 63 -> 0: play P63 twice, then jump to P0 (wrap across the new top of the range)
    s->patterns[63].playMode = Sequencer::Chain;
    s->patterns[63].chainLen = 1; s->patterns[63].chainStep = 0;
    s->patterns[63].chainSeq[0] = 0; s->patterns[63].chainLoops[0] = 2;
    // per-pattern MASTER written at index 63 (storage + independence check after the run)
    s->patterns[63].master.reverbWet = 0.123f;
    s->patterns[63].master.volume    = 0.42f;
    const float p0Wet = s->patterns[0].master.reverbWet;
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    s->setCurrentPattern(63);                           // stopped: parks playback on P63
    s->startStandalone();

    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    int bpMid = -1;                                     // barPlays[63] sampled mid second pass
    for (int b = 0; b < (int) (2.0 * 3 * SR / bs) + 1; ++b)
    {
        buf.clear(); s->processBlock(buf, SR, bs, nullptr);
        for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i));
        if (bpMid < 0 && (double) out.size() / SR > 3.0) bpMid = s->barPlays[63];
    }
    const int bpEnd = s->barPlays[63];

    // bars: 1 = P63 (C3), 2 = P63 again (C3), 3 = P0 (E4)
    const double expct[3] = { C3, C3, E4 };  const char* nm[3] = { "P63", "P63", "P0" };
    for (int bar = 0; bar < 3; ++bar)
    {
        const double t0 = bar * 2.0 + 0.05, t1 = bar * 2.0 + 0.45;
        const double want  = goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), expct[bar], SR);
        const double other = goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR),
                                      expct[bar] == C3 ? E4 : C3, SR);
        printf("[2/3] bar%d expect %s: got=%.3f other=%.3f\n", bar + 1, nm[bar], want, other);
        if (! CHK(want > 0.02 && other < want * 0.4)) printf("  FAIL\n");
    }
    printf("[3] barPlays[63]: mid-pass-2 = %d (want 1), after both = %d (want 2)\n", bpMid, bpEnd);
    if (! CHK(bpMid == 1 && bpEnd == 2)) printf("  FAIL\n");
    printf("[4] master @63 held: wet=%.3f vol=%.3f, P0 wet untouched=%.3f\n",
           s->patterns[63].master.reverbWet, s->patterns[63].master.volume, s->patterns[0].master.reverbWet);
    if (! CHK(std::abs(s->patterns[63].master.reverbWet - 0.123f) < 1e-6f
           && std::abs(s->patterns[63].master.volume    - 0.42f)  < 1e-6f
           && std::abs(s->patterns[0].master.reverbWet  - p0Wet)  < 1e-6f)) printf("  FAIL\n");
    delete s;

    printf(fails ? ">>> PatternCountTest FAIL (%d)\n" : ">>> PatternCountTest PASS\n", fails);
    return fails;
}
