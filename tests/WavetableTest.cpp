// ADDITIVE WAVETABLE (v1.3.5): 4 drawn frames (A..D) + a POSITION that scans them, PER-LEG glide
// times (addSeg[3]; 0 = HOLD) and the WAVE LFO (dest 3) scanning live. Locks:
//   [1] position 0 = frame A only, position 1 = frame D only (Goertzel per harmonic)
//   [2] a mid-leg position = a real crossfade of its two frames
//   [3] LEGACY migrations: 2-spectrum files (aH/aHB/aMt) -> frames {A,B,B,B} + seg {aMt,0,0}
//       (travel then hold = the exact original behaviour); the brief whole-strip-glide
//       generation (aH0..3 + aMt, no aSg) -> an even 3-way split
//   [4] the WAVE LFO changes the output; amount 0 = bit-identical to a static render
//   [5] full glide {t,t,t}: the note STARTS as A and ENDS as D
//   [6] HOLD: seg {t,0,0} parks the note on frame B forever (no C/D content)
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0; static const int BS = 480;
static const double C4 = 261.6255653;

// Frames A/B/C/D = pure h1/h2/h3/h4: every leg's content is separable by Goertzel.
static void authFrames(DrumChannel& c, float pos, const float* segs, float waveLfoAmt)
{
    for (auto& sl : c.slots) sl = DrumChannel::Slot();
    auto& s = c.slots[0];
    s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = DrumChannel::WvCustom;
    s.oscFreq = (float) C4;
    for (int f = 0; f < DrumChannel::ADD_FRAMES; ++f)
        for (int k = 0; k < DrumChannel::ADD_HARM; ++k) { s.addH[f][k] = 0.0f; s.addPh[f][k] = 0.0f; }
    for (int f = 0; f < DrumChannel::ADD_FRAMES; ++f)  // A=h1, B=h2, C=h3, D=h4
        s.addH[f][f] = 1.0f;
    s.addPos = pos;
    for (int k = 0; k < DrumChannel::ADD_FRAMES - 1; ++k) s.addSeg[k] = segs ? segs[k] : 0.0f;
    s.lfoAmt[3] = waveLfoAmt; s.lfoRate[3] = 6.0f;
    s.atk = 0.002f; s.hold = 1.0f; s.dec = 1.5f;       // long flat body for clean windows
    c.rebuildAddTables();
}

static std::vector<float> renderStep(float pos, const float* segs, float waveLfoAmt, double secs)
{
    auto* q = new Sequencer();
    q->setStandaloneBpm(120.0f);
    auto& c = q->patterns[0].channels[0];
    authFrames(c, pos, segs, waveLfoAmt);
    c.numSteps = 4; c.steps[0] = true;
    for (auto& p : q->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, BS);
    q->startStandalone();
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, BS);
    for (int b = 0; b < (int)(secs * SR / BS); ++b)
    { buf.clear(); q->processBlock(buf, SR, BS, nullptr); for (int i = 0; i < BS; ++i) out.push_back(buf.getSample(0, i)); }
    delete q;
    return out;
}

static double goertzel(const std::vector<float>& x, int a, int n, double freq)
{
    double S = 0.0, Cc = 0.0; const double w = 2.0 * 3.14159265358979 * freq / SR;
    for (int i = 0; i < n && a + i < (int) x.size(); ++i)
    { S += x[(size_t)(a + i)] * std::sin(w * i); Cc += x[(size_t)(a + i)] * std::cos(w * i); }
    return std::sqrt(S * S + Cc * Cc) / n;
}

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const int a0 = (int)(0.05 * SR), W = (int)(0.20 * SR);

    {   // [1] position endpoints
        auto oA = renderStep(0.0f, nullptr, 0.0f, 0.4);
        auto oD = renderStep(1.0f, nullptr, 0.0f, 0.4);
        const double a_h1 = goertzel(oA, a0, W, C4),       a_h4 = goertzel(oA, a0, W, C4 * 4.0);
        const double d_h1 = goertzel(oD, a0, W, C4),       d_h4 = goertzel(oD, a0, W, C4 * 4.0);
        printf("[1] pos 0: h1=%.3f h4=%.3f | pos 1: h1=%.3f h4=%.3f (endpoints pure) -> %s\n",
               a_h1, a_h4, d_h1, d_h4,
               CHK(a_h1 > 10.0 * a_h4 && d_h4 > 10.0 * d_h1) ? "OK" : "FAIL");
    }
    {   // [2] mid-leg position = a crossfade of its two frames (pos 1/6 = 50/50 A/B = h1+h2)
        auto oM = renderStep(1.0f / 6.0f, nullptr, 0.0f, 0.4);
        const double m_h1 = goertzel(oM, a0, W, C4), m_h2 = goertzel(oM, a0, W, C4 * 2.0);
        const double m_h3 = goertzel(oM, a0, W, C4 * 3.0);
        printf("[2] pos 1/6 (mid A-B): h1=%.3f h2=%.3f h3=%.3f (A+B only) -> %s\n",
               m_h1, m_h2, m_h3, CHK(m_h1 > 0.1 && m_h2 > 0.1 && m_h3 < 0.02) ? "OK" : "FAIL");
    }
    {   // [3] legacy migrations
        DrumChannel c;
        juce::ValueTree parent("Slots"); juce::ValueTree st("Slot");
        st.setProperty("eng", (int) DrumChannel::SrcOsc, nullptr);
        st.setProperty("w", 1.0f, nullptr);
        juce::String hA, hB;
        for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
        { hA << (k == 0 ? "1.0" : "0.0") << (k < DrumChannel::ADD_HARM - 1 ? "," : "");
          hB << (k == 3 ? "1.0" : "0.0") << (k < DrumChannel::ADD_HARM - 1 ? "," : ""); }
        st.setProperty("aH", hA, nullptr); st.setProperty("aHB", hB, nullptr);
        st.setProperty("aMt", 0.8f, nullptr);
        parent.addChild(st, -1, nullptr);
        c.readSlots(parent);
        const auto& s = c.slots[0];
        const bool ok = std::abs(s.addSeg[0] - 0.8f) < 1.0e-4f && s.addSeg[1] == 0.0f && s.addSeg[2] == 0.0f
                     && s.addH[0][0] == 1.0f && s.addH[0][3] == 0.0f
                     && s.addH[1][3] == 1.0f && s.addH[2][3] == 1.0f && s.addH[3][3] == 1.0f
                     && s.addH[1][0] == 0.0f;
        printf("[3a] legacy aH/aHB/aMt=0.8 -> frames {A,B,B,B} + seg {%.2f,%.2f,%.2f} (expect 0.80,0,0) -> %s\n",
               s.addSeg[0], s.addSeg[1], s.addSeg[2], CHK(ok) ? "OK" : "FAIL");

        DrumChannel c2;                             // whole-strip-glide generation: aH0..3 + aMt, no aSg
        juce::ValueTree p2("Slots"); juce::ValueTree s2("Slot");
        s2.setProperty("eng", (int) DrumChannel::SrcOsc, nullptr);
        s2.setProperty("w", 1.0f, nullptr);
        for (int f = 0; f < DrumChannel::ADD_FRAMES; ++f)
        { s2.setProperty("aH" + juce::String(f), hA, nullptr); }
        s2.setProperty("aMt", 0.9f, nullptr);
        p2.addChild(s2, -1, nullptr);
        c2.readSlots(p2);
        const auto& sl2 = c2.slots[0];
        const bool ok2 = std::abs(sl2.addSeg[0] - 0.3f) < 1.0e-4f
                      && std::abs(sl2.addSeg[1] - 0.3f) < 1.0e-4f
                      && std::abs(sl2.addSeg[2] - 0.3f) < 1.0e-4f;
        printf("[3b] whole-strip gen (aH0..3 + aMt=0.9) -> seg thirds {%.2f,%.2f,%.2f} -> %s\n",
               sl2.addSeg[0], sl2.addSeg[1], sl2.addSeg[2], CHK(ok2) ? "OK" : "FAIL");
    }
    {   // [4] WAVE LFO: amt 0 = bit-identical, amt 0.8 = audibly different
        auto ref  = renderStep(0.5f, nullptr, 0.0f, 0.4);
        auto ref2 = renderStep(0.5f, nullptr, 0.0f, 0.4);
        auto lfo  = renderStep(0.5f, nullptr, 0.8f, 0.4);
        float md0 = 0.0f, md1 = 0.0f;
        for (size_t i = 0; i < ref.size(); ++i)
        { md0 = std::max(md0, std::abs(ref[i] - ref2[i])); md1 = std::max(md1, std::abs(ref[i] - lfo[i])); }
        printf("[4] WAVE LFO: repeat maxdiff=%.6f (expect 0), lfo-vs-static maxdiff=%.3f (expect >0.05) -> %s\n",
               md0, md1, CHK(md0 == 0.0f && md1 > 0.05f) ? "OK" : "FAIL");
    }
    {   // [5] full glide {0.2,0.2,0.2}: starts as A (h1), ends as D (h4)
        const float segs[3] = { 0.2f, 0.2f, 0.2f };
        auto o = renderStep(0.0f, segs, 0.0f, 1.0);
        const int We = (int)(0.04 * SR);   // early window ends at 50 ms = still ~pure A
        const double e_h1 = goertzel(o, (int)(0.01 * SR), We, C4);
        const double e_h4 = goertzel(o, (int)(0.01 * SR), We, C4 * 4.0);
        const double l_h1 = goertzel(o, (int)(0.75 * SR), We, C4);
        const double l_h4 = goertzel(o, (int)(0.75 * SR), We, C4 * 4.0);
        printf("[5] glide {0.2,0.2,0.2}: start h1=%.3f h4=%.3f | end h1=%.3f h4=%.3f -> %s\n",
               e_h1, e_h4, l_h1, l_h4,
               CHK(e_h1 > 3.0 * e_h4 && l_h4 > 3.0 * l_h1) ? "OK" : "FAIL");
    }
    {   // [6] HOLD: seg {0.15, 0, 0} = travel A>B then PARK ON B (h2) - no C/D content ever
        const float segs[3] = { 0.15f, 0.0f, 0.0f };
        auto o = renderStep(0.0f, segs, 0.0f, 1.0);
        const int We = (int)(0.15 * SR);
        const double l_h2 = goertzel(o, (int)(0.60 * SR), We, C4 * 2.0);
        const double l_h3 = goertzel(o, (int)(0.60 * SR), We, C4 * 3.0);
        const double l_h4 = goertzel(o, (int)(0.60 * SR), We, C4 * 4.0);
        printf("[6] hold {0.15,0,0}: late h2=%.3f h3=%.3f h4=%.3f (parked on B) -> %s\n",
               l_h2, l_h3, l_h4, CHK(l_h2 > 0.2 && l_h3 < 0.02 && l_h4 < 0.02) ? "OK" : "FAIL");
    }
    printf(fails == 0 ? ">>> WavetableTest PASS\n" : ">>> WavetableTest FAIL (%d)\n", fails);
    return fails;
}
