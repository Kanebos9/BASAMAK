// MONO LEGATO GLIDE (keysGlide): pressing a new key while the previous is HELD makes the new note
// SLIDE from the old pitch to the new. [1] glide ON: the 2nd note STARTS near the old pitch and ENDS
// near the new, passing through the middle (a real sweep). [2] glide OFF: the 2nd note JUMPS straight
// to the new pitch. [3] poly = fingered portamento (overlap slides, detached jumps) [2026-07-16].
#include "DrumChannel.h"
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

static void setupSine(DrumChannel& ch) {
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    auto& sl = ch.slots[0];
    sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f;
    sl.oscShape = sl.oscShapeB = DrumChannel::WvSine; sl.oscFreq = 261.6256f;   // C3 base (clean single tone)
    sl.atk = 0.002f; sl.dec = 0.4f; sl.sustain = 0.95f; sl.release = 0.05f;     // sustains so the held note keeps ringing
}

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 512;
    const double C3 = 261.63, C4 = 523.25, MID = 261.63 * std::pow(2.0, 6.0/12.0);   // +12 st target, +6 st midpoint

    auto render = [&](DrumChannel& ch, std::vector<float>& out, double sec) {
        juce::AudioBuffer<float> buf(2, bs);
        const int blocks = (int) (sec * SR / bs) + 1;
        for (int b = 0; b < blocks; ++b)
        { buf.clear(); ch.renderInto(buf, 0, bs, false); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
    };
    auto W = [&](const std::vector<float>& o, double t0, double t1, double f){ return goertzel(o, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };

    {   // [1] GLIDE ON: mono, hold C3, then press C4 with glide - it slides up over ~0.4 s
        DrumChannel ch; setupSine(ch); ch.prepareToPlay(SR, bs);
        ch.keysGlide = 1.0f;                       // ~400 ms glide
        ch.keyDown(60, 1.0f, 0, false);            // C3 (mono, held)
        { std::vector<float> est; render(ch, est, 0.12); }
        ch.keyDown(72, 1.0f, 0, false);            // C4 while C3 still held -> legato glide
        std::vector<float> out; render(ch, out, 0.5);
        const double eLo = W(out, 0.00, 0.04, C3),  eHi = W(out, 0.00, 0.04, C4);
        const double mMid = W(out, 0.18, 0.23, MID);
        const double lLo = W(out, 0.44, 0.50, C3),  lHi = W(out, 0.44, 0.50, C4);
        printf("[1] glide: start C3=%.3f>C4=%.3f | mid(+6st)=%.3f | end C4=%.3f>C3=%.3f -> %s\n",
               eLo, eHi, mMid, lHi, lLo,
               CHK(eLo > eHi && mMid > 0.02 && lHi > lLo) ? "SLIDES C3->C4 (OK)" : "FAIL");
    }
    {   // [2] GLIDE OFF: the 2nd note jumps straight to C4
        DrumChannel ch; setupSine(ch); ch.prepareToPlay(SR, bs);
        ch.keysGlide = 0.0f;
        ch.keyDown(60, 1.0f, 0, false);
        { std::vector<float> est; render(ch, est, 0.12); }
        ch.keyDown(72, 1.0f, 0, false);
        std::vector<float> out; render(ch, out, 0.2);
        const double eLo = W(out, 0.00, 0.05, C3), eHi = W(out, 0.00, 0.05, C4);
        printf("[2] no glide: start C3=%.3f C4=%.3f -> %s\n", eLo, eHi,
               CHK(eHi > eLo) ? "JUMPS to C4 (OK)" : "FAIL");
    }
    {   // [3] POLY glide = FINGERED PORTAMENTO ([2026-07-16 round-2] two-axis model: the Glide
        //     knob works in every mode; only OVERLAPPING presses slide). (a) note 2 pressed while
        //     note 1 is HELD -> slides in (C4 NOT there immediately, arrives by the end, C3 keeps
        //     ringing = poly). (b) DETACHED press with glide up -> jumps straight to C4 (no slide).
        DrumChannel ch; setupSine(ch); ch.prepareToPlay(SR, bs);
        ch.keysGlide = 1.0f;
        ch.keyDown(60, 1.0f, 0, true);
        { std::vector<float> est; render(ch, est, 0.12); }
        ch.keyDown(72, 1.0f, 0, true);                       // C3 still held = overlapping
        std::vector<float> out; render(ch, out, 0.45);
        const double eHi = W(out, 0.00, 0.05, C4), lHi = W(out, 0.40, 0.45, C4), lLo = W(out, 0.40, 0.45, C3);
        DrumChannel ch2; setupSine(ch2); ch2.prepareToPlay(SR, bs);
        ch2.keysGlide = 1.0f;
        ch2.keyDown(60, 1.0f, 0, true);
        ch2.keyUp(60);                                        // released = the next press is DETACHED
        { std::vector<float> est; render(ch2, est, 0.12); }
        ch2.keyDown(72, 1.0f, 0, true);
        std::vector<float> out2; render(ch2, out2, 0.2);
        const double dHi = W(out2, 0.00, 0.05, C4);
        printf("[3] poly fingered glide: overlap C4 imm=%.3f end C4=%.3f (C3 rings %.3f) | detached C4 imm=%.3f -> %s\n",
               eHi, lHi, lLo, dHi,
               CHK(eHi < 0.05 && lHi > 0.1 && lLo > 0.05 && dHi > 0.05) ? "OK" : "FAIL");
    }

    {   // [4] RECORDED glide reproduces on PLAYBACK: a mono draw channel with two LEGATO notes
        //     (C3 then C4, butted) + Glide up -> the sequencer slides note 2 from C3 to C4.
        auto* sq = new Sequencer();
        sq->setStandaloneBpm(120.0f);                 // 1 bar = 2 s; DRAW_RES 384 -> beat = 96 cols = 0.5 s
        auto& ch = sq->patterns[0].channels[0];
        setupSine(ch);
        ch.drawMode = true; ch.keysPolyMode = false; ch.keysGlide = 1.0f;   // mono + glide
        ch.addDrawNote(0,  96, 0,  255, 0, 0);        // C3, beat 1
        ch.addDrawNote(96, 96, 12, 255, 0, 1);        // C4, beat 2 (butted -> legato), GLIDE flag set
        for (auto& p : sq->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
        sq->startStandalone();
        std::vector<float> out;
        const int blocks = (int) (1.1 * SR / bs) + 1;
        juce::AudioBuffer<float> buf(2, bs);
        for (int b = 0; b < blocks; ++b) { buf.clear(); sq->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
        const double eLo = W(out, 0.50, 0.54, C3), eHi = W(out, 0.50, 0.54, C4);   // note 2 start -> near C3
        const double lLo = W(out, 0.86, 0.96, C3), lHi = W(out, 0.86, 0.96, C4);   // glide done -> near C4
        printf("[4] recorded playback: note2 start C3=%.3f>C4=%.3f | end C4=%.3f>C3=%.3f -> %s\n",
               eLo, eHi, lHi, lLo, CHK(eLo > eHi && lHi > lLo) ? "SLIDES on playback (RECORDABLE OK)" : "FAIL");
        delete sq;
    }

    printf(fails ? "\n>>> GLIDE FAILURES\n" : "\n>>> GlideTest PASS\n");
    return fails;
}
