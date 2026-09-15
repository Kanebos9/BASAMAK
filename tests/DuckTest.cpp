// SIDECHAIN DUCK regression:
//   [1] channel 1 = a sustained tone (long decay), channel 0 = a kick-style hit at beat 1 + 3
//       with ch1 set to "duck by ch0" - the tone's level must DIP right after each hit
//   [2] ... and RECOVER between hits
//   [3] a duck that NEVER FIRES is bit-identical to duck off: duckBy = -1 vs duckBy = a channel
//       with no steps ([1]/[2] stay the positive control - the old check compared two OFF
//       renders, which only proved determinism) [2026-08-01 r26]
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

// mode: 0 = duck off (duckBy -1), 1 = duck by ch0 (fires), 2 = duck by ch2 (NO steps = never
// fires - the armed-but-silent path must be bit-identical to off) [2026-08-01 r26]
static std::vector<float> render(int mode, bool live = false, bool otherPattern = false) {
    const double SR = 48000.0; const int bs = 512;
    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);   // 1 bar = 2.0 s
    auto& kick = s->patterns[0].channels[0];
    auto& tone = s->patterns[0].channels[1];
    mkTone(kick, 60.0f, 0.05f);  kick.numSteps = 4; kick.steps[1] = true; kick.steps[3] = true;  // hits at 0.5s + 1.5s
    mkTone(tone, 330.0f, 4.0f);  tone.numSteps = 4; tone.steps[0] = true;                        // long tone from 0
    if (mode == 1) { tone.duckBy = 0; tone.duckAmt = 0.9f; }
    if (mode == 2) { tone.duckBy = 2; tone.duckAmt = 0.9f; }   // ch2 has no steps = never pulses
    if (mode == 3) { tone.duckBy = 0; tone.duckAmt = 0; }
    if (mode == 4) { tone.duckBy = 0; tone.duckAmt = 0.9f; }
    if (mode == 5) tone.liveChokeBy = 0; // Live-only setting cannot change regular playback
    auto& nextKick = s->patterns[1].channels[0];
    auto& nextTone = s->patterns[1].channels[1];
    mkTone(nextKick, 60.0f, 0.05f); mkTone(nextTone, 330.0f, 4.0f);
    nextTone.duckBy = tone.duckBy; nextTone.duckAmt = tone.duckAmt;
    for (auto& p : s->patterns) for (auto& c : p.channels) c.prepareToPlay(SR, bs);
    if (live) { s->drums.enabled = true; s->drums.assign(0, 60, 10); s->drums.assign(1, 61, 10); }
    else s->startStandalone();
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    const int blocks = (int) (2.0 * SR / bs);
    for (int b = 0; b < blocks; ++b)
    {
        if (live)
        {
            s->drums.beginBlock();
            if (b == 0) s->drums.noteOn(61, 10, 127, 0);
            for (int hit : {24000, 72000})
                if (hit >= b * bs && hit < (b + 1) * bs)
                {
                    if (otherPattern) s->currentPattern = 1;
                    s->drums.noteOn(60, 10, 127, hit - b * bs);
                }
            if (mode == 4 && b * bs >= 32000) tone.duckBy = nextTone.duckBy = -1;
        }
        buf.clear(); s->processBlock(buf, SR, bs, nullptr);
        for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i));
    }
    delete s;
    return out;
}

// Actual MIDI-triggered tails: mode 0 = repeated same pad, 1/2 = another pad with choke on/off.
static int liveTailCount(bool overlap, int mode, bool otherPattern, bool& audible)
{
    auto* s = new Sequencer();
    s->drums.enabled = true; s->drums.assign(0, 60, 10); s->drums.assign(1, 61, 10);
    for (int p = 0; p < 2; ++p)
        for (int ch = 0; ch < 16; ++ch)
        {
            auto& c = s->patterns[p].channels[ch];
            if (ch < 2) mkTone(c, 330.0f + ch * 110, 4.0f);
            c.allowOverlap = true;
            c.chokeGroup = mode == 1 && ch < 2 ? 1 : 0;
            c.liveChokeBy = (mode == 3 || mode == 5) && ch == 0 ? 1 : mode == 4 && ch == 1 ? 0 : -1;
            c.mute = mode == 5 && ch == 1;
            c.prepareToPlay(48000, 512);
        }
    juce::AudioBuffer<float> b(2, 512);
    for (int block = 0; block < 20; ++block)
    {
        s->drums.beginBlock();
        if (block == 0) s->drums.noteOn(60, 10, 100, 0);
        if (block == 10)
        {
            if (otherPattern) s->currentPattern = 1;
            for (int p = 0; p < 2; ++p) s->patterns[p].channels[0].allowOverlap = overlap;
            s->drums.noteOn(mode == 0 ? 60 : 61, 10, 100, 123);
        }
        b.clear(); s->processBlock(b, 48000, 512, nullptr);
    }
    audible = b.getMagnitude(0, 512) > 0.001f;
    float times[DrumChannel::POLY]; int tails = 0;
    for (int p = 0; p < 2; ++p) tails += s->patterns[p].channels[0].activeVoiceTimes(times, DrumChannel::POLY);
    delete s;
    return tails;
}

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 48000.0;
    auto dry = render(0), wet = render(1);
    auto R = [&](const std::vector<float>& x, double t0, double t1){ return rms(x, (size_t)(t0*SR), (size_t)(t1*SR)); };
    // windows: pre-hit (0.35-0.48), in-dip (0.52-0.60), recovered (1.05-1.35)
    const double pre = R(wet, 0.35, 0.48), dip = R(wet, 0.52, 0.60), rec = R(wet, 1.05, 1.35);
    const double dryDip = R(dry, 0.52, 0.60), dryRec = R(dry, 1.05, 1.35);
    printf("[1] duck dips:      pre=%.4f dip=%.4f (dry dip=%.4f) -> %s\n", pre, dip, dryDip,
           CHK(dip < dryDip * 0.55) ? "level pushed down (DUCK OK)" : "FAIL");
    printf("[2] duck recovers:  rec=%.4f (dry=%.4f) -> %s\n", rec, dryRec,
           CHK(rec > dryRec * 0.7) ? "level back up (RELEASE OK)" : "FAIL");
    // [3] [2026-08-01 r26] duck ARMED but never fired = bit-identical to duck off (the old
    // check rendered off twice = a determinism test wearing the wrong label)
    double maxdiff = 0;
    { auto armed = render(2);
      for (size_t i = 0; i < dry.size() && i < armed.size(); ++i) maxdiff = juce::jmax(maxdiff, (double) std::abs(dry[i] - armed[i])); }
    printf("[3] duck armed-but-silent = bit-identical to off: maxdiff=%.9f -> %s\n", maxdiff, CHK(maxdiff == 0.0) ? "OK" : "FAIL");
    {
        auto regularWithLiveChoke = render(5);
        double delta = 0;
        for (size_t i = 0; i < dry.size(); ++i) delta = std::max(delta, (double)std::abs(dry[i] - regularWithLiveChoke[i]));
        printf("[regular] Live-only choke leaves regular playback unchanged: %.9f -> %s\n", delta, CHK(delta == 0) ? "OK" : "FAIL");
    }
    for (bool across : {false, true})
    {
        auto liveDry = render(0, true, across), liveWet = render(1, true, across);
        const double ratio = R(liveWet, 0.52, 0.60) / R(liveDry, 0.52, 0.60);
        const double recovery = R(liveWet, 1.05, 1.35) / R(liveDry, 1.05, 1.35);
        printf("[live %s] duck dip=%.3f recovery=%.3f -> %s\n", across ? "old-bar tails" : "same bar",
               ratio, recovery, CHK(ratio < 0.55 && recovery > 0.7) ? "OK" : "FAIL");
        for (int mode : {2, 3, 4})
        {
            auto disabled = render(mode, true, across); double delta = 0;
            const size_t start = mode == 4 ? 48000 : 0;
            for (size_t i = start; i < liveDry.size(); ++i)
                delta = std::max(delta, (double)std::abs(liveDry[i] - disabled[i]));
            printf("[live %s] duck disabled/silent mode %d delta=%.9f -> %s\n",
                   across ? "old-bar tails" : "same bar", mode, delta, CHK(delta == 0) ? "OK" : "FAIL");
        }
        for (bool overlap : {false, true})
            for (int mode : {0, 1, 2, 3, 4, 5})
            {
                bool audible = false;
                const int tails = liveTailCount(overlap, mode, across, audible);
                const int expected = mode == 0 ? (overlap ? 2 : 1) : (mode == 1 || mode == 3) ? 0 : 1;
                printf("[live %s] overlap=%d choke-mode=%d old-row voices=%d (want %d) -> %s\n",
                       across ? "old-bar tails" : "same bar", overlap, mode, tails, expected,
                       CHK(audible && tails == expected) ? "OK" : "FAIL");
            }
    }
    return fails;
}
