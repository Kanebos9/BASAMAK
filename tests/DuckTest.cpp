// SIDECHAIN DUCK regression: channel 1 = a sustained tone (long decay), channel 0 = a kick-style
// hit at beat 1 + 3 with ch1 set to "duck by ch0". The tone's level must DIP right after each hit
// and RECOVER between hits. duckBy = -1 must be bit-identical to no duck at all.
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static double rms(const std::vector<float>& x, size_t a, size_t b) {
    double acc = 0; size_t n = 0;
    for (size_t i = a; i < b && i < x.size(); ++i) { acc += (double) x[i] * x[i]; ++n; }
    return std::sqrt(acc / (double) juce::jmax((size_t) 1, n));
}

static void mkTone(DrumChannel& ch, float hz, float dec) {
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    auto& sl = ch.slots[0];
    sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
    sl.oscShape = sl.oscShapeB = DrumChannel::WvSaw; sl.oscFreq = hz;
    sl.atk = 0.002f; sl.dec = dec;
}

static std::vector<float> render(bool duck) {
    const double SR = 48000.0; const int bs = 512;
    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);   // 1 bar = 2.0 s
    auto& kick = s->patterns[0].channels[0];
    auto& tone = s->patterns[0].channels[1];
    mkTone(kick, 60.0f, 0.05f);  kick.numSteps = 4; kick.steps[1] = true; kick.steps[3] = true;  // hits at 0.5s + 1.5s
    mkTone(tone, 330.0f, 4.0f);  tone.numSteps = 4; tone.steps[0] = true;                        // long tone from 0
    if (duck) { tone.duckBy = 0; tone.duckAmt = 0.9f; }
    for (auto& p : s->patterns) for (auto& c : p.channels) c.prepareToPlay(SR, bs);
    s->startStandalone();
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    const int blocks = (int) (2.0 * SR / bs);
    for (int b = 0; b < blocks; ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
    delete s;
    return out;
}

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 48000.0;
    auto dry = render(false), wet = render(true);
    auto R = [&](const std::vector<float>& x, double t0, double t1){ return rms(x, (size_t)(t0*SR), (size_t)(t1*SR)); };
    // windows: pre-hit (0.35-0.48), in-dip (0.52-0.60), recovered (1.05-1.35)
    const double pre = R(wet, 0.35, 0.48), dip = R(wet, 0.52, 0.60), rec = R(wet, 1.05, 1.35);
    const double dryDip = R(dry, 0.52, 0.60), dryRec = R(dry, 1.05, 1.35);
    printf("[1] duck dips:      pre=%.4f dip=%.4f (dry dip=%.4f) -> %s\n", pre, dip, dryDip,
           CHK(dip < dryDip * 0.55) ? "level pushed down (DUCK OK)" : "FAIL");
    printf("[2] duck recovers:  rec=%.4f (dry=%.4f) -> %s\n", rec, dryRec,
           CHK(rec > dryRec * 0.7) ? "level back up (RELEASE OK)" : "FAIL");
    // [3] off = identical
    double maxdiff = 0;
    { auto off = render(false);
      for (size_t i = 0; i < dry.size() && i < off.size(); ++i) maxdiff = juce::jmax(maxdiff, (double) std::abs(dry[i] - off[i])); }
    printf("[3] duck off = bit-identical: maxdiff=%.9f -> %s\n", maxdiff, CHK(maxdiff == 0.0) ? "OK" : "FAIL");
    return fails;
}
