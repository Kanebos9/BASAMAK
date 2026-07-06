// MONO LEGATO GLIDE (keysGlide): pressing a new key while the previous is HELD makes the new note
// SLIDE from the old pitch to the new. [1] glide ON: the 2nd note STARTS near the old pitch and ENDS
// near the new, passing through the middle (a real sweep). [2] glide OFF: the 2nd note JUMPS straight
// to the new pitch. [3] poly never glides (jumps even with glide up).
#include "DrumChannel.h"
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
    {   // [3] POLY never glides even with glide up (both notes just ring)
        DrumChannel ch; setupSine(ch); ch.prepareToPlay(SR, bs);
        ch.keysGlide = 1.0f;
        ch.keyDown(60, 1.0f, 0, true);
        { std::vector<float> est; render(ch, est, 0.12); }
        ch.keyDown(72, 1.0f, 0, true);
        std::vector<float> out; render(ch, out, 0.2);
        const double eHi = W(out, 0.00, 0.05, C4);
        printf("[3] poly: C4 present immediately=%.3f -> %s\n", eHi,
               CHK(eHi > 0.05) ? "no glide in poly (OK)" : "FAIL");
    }

    printf(fails ? "\n>>> GLIDE FAILURES\n" : "\n>>> GlideTest PASS\n");
    return fails;
}
