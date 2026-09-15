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
#include <memory>
#include <algorithm>

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

// Exercise the actual event scanner, not just stepSpan: sample positions, gates,
// ratchets and bar handovers at fractional tempos and different host buffer sizes.
static bool newStepTiming(int scenario)
{
    const int counts[] = {17, 18, 19, 25, 26, 33, 34, 35};
    const double sr = scenario == 0 ? 48000 : scenario == 1 ? 44100 : 96000;
    const int bs = scenario == 0 ? 128 : scenario == 1 ? 257 : 1024;
    const double bpm = scenario == 0 ? 120 : scenario == 1 ? 137 : 83;
    const int numerator = scenario == 0 ? 4 : scenario == 1 ? 7 : 3;
    const int denominator = scenario == 1 ? 8 : 4;
    const double qpb = numerator * 4.0 / denominator;
    const double barSamples = sr * 60.0 / bpm * qpb;
    const bool merged = scenario == 2;
    auto sq = std::make_unique<Sequencer>();
    sq->standaloneBpm = (float)bpm; sq->timeSigNum = numerator; sq->timeSigDen = denominator;
    sq->dawSync = merged;
    for (int p = 0; p < (merged ? 2 : 1); ++p)
    {
        auto& pat = sq->patterns[p];
        pat.swing = scenario == 0 ? 0.0f : 1.0f;
        if (merged) {
            pat.mergeWithPrev = p == 1;
            pat.playMode = Sequencer::Chain; pat.chainLen = 1;
            pat.chainSeq[0] = 1-p; pat.chainLoops[0] = 1;
        }
        for (int ch = 0; ch < 8; ++ch) {
            auto& c = pat.channels[ch];
            c.midiOut = true; c.numSteps = p == 1 ? 17 : counts[ch];
            for (int st = 0; st < c.numSteps; ++st) {
                c.steps[st] = true; c.stepNoteLen[st] = 0.5f;
                if (scenario != 0 && st == c.numSteps / 2) {
                    c.stepRoll[st] = 3; c.stepNudge[st] = 0.4f;
                }
                if (scenario != 0 && st == c.numSteps - 1) c.stepNudge[st] = -0.4f;
            }
        }
    }
    for (auto& pat : sq->patterns) for (auto& ch : pat.channels) ch.prepareToPlay(sr, bs);
    struct Head : juce::AudioPlayHead {
        double ppq = 0, bpm = 120; int num = 4, den = 4;
        juce::Optional<PositionInfo> getPosition() const override {
            PositionInfo p; p.setPpqPosition(ppq); p.setBpm(bpm);
            p.setTimeSignature(TimeSignature{num, den}); p.setIsPlaying(true); return p;
        }
    } head;
    head.bpm = bpm; head.num = numerator; head.den = denominator;
    sq->startStandalone();
    struct Hit { double sample; int step, sub, pattern; long gate; };
    std::vector<Hit> actual[8], expected[8];
    const int frames = (int)(barSamples * 6) / bs * bs;
    juce::AudioBuffer<float> audio(2, bs);
    for (int frame = 0; frame < frames; frame += bs) {
        head.ppq = (double)frame / sr * bpm / 60.0;
        audio.clear();
        for (const auto& e : sq->processBlock(audio, sr, bs, merged ? &head : nullptr))
            if (e.channel < 8)
                actual[e.channel].push_back({double(frame + e.offset), e.step, e.sub, e.pattern, e.gate});
    }
    bool ok = true; double worst = 0; int hits = 0;
    for (int ch = 0; ch < 8; ++ch) {
        for (int bar = 0; bar < 6; ++bar) {
            const int p = merged ? bar % 2 : 0;
            const int n = p == 1 ? 17 : counts[ch];
            for (int st = 0; st < n; ++st) {
                // Analytical straight/swing positions. An odd count's final step stays straight.
                const bool swung = scenario != 0 && !(n % 2 && st == n - 1);
                const double start = swung ? (2.0 * (st / 2) + (st % 2 ? 1.5 : 0.0)) / n : double(st) / n;
                const double width = (swung ? (st % 2 ? 0.5 : 1.5) : 1.0) / n;
                const int roll = scenario != 0 && st == n / 2 ? 3 : 1;
                const double nudge = scenario == 0 ? 0.0 : st == n / 2 ? 0.2 : st == n - 1 ? -0.2 : 0.0;
                for (int sub = 0; sub < roll; ++sub) {
                    const double sample = (bar + start + width * (nudge + double(sub) / roll)) * barSamples;
                    if (sample < frames - 2)
                        expected[ch].push_back({sample, st, sub, p, (long)(width * barSamples * 0.5)});
                }
            }
        }
        std::sort(expected[ch].begin(), expected[ch].end(), [](const Hit& a, const Hit& b){return a.sample < b.sample;});
        if (actual[ch].size() != expected[ch].size()) ok = false;
        for (size_t i = 0; i < std::min(actual[ch].size(), expected[ch].size()); ++i) {
            const auto& a = actual[ch][i]; const auto& e = expected[ch][i];
            worst = std::max(worst, std::abs(a.sample - e.sample)); ++hits;
            if (std::abs(a.sample - e.sample) > 2.0 || a.step != e.step || a.sub != e.sub
                || a.pattern != e.pattern || std::abs(a.gate - e.gate) > 1) ok = false;
        }
    }
    printf("[new counts %d] %d hits across six bars, worst timing error %.3f samples: %s\n",
           scenario, hits, worst, ok ? "PASS" : "FAIL");
    return ok;
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

    for (int scenario = 0; scenario < 3; ++scenario) CHK(newStepTiming(scenario));

    printf(fails ? ">>> PatternCountTest FAIL (%d)\n" : ">>> PatternCountTest PASS\n", fails);
    return fails;
}
