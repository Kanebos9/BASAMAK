// ARP note-selection + rate + pitch. The arp CLOCK lives in the processor (needs bpm/time-sig/keys),
// but its two failure-prone pieces are shared, pure, and tested here: [1] arpNoteAt (root/offset/rest/
// wrap), [2] arpRateMul (the 1/3..3 table), [3] a rendered arp note plays the right PITCH via keyDown.
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

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 512;
    using DC = DrumChannel;

    {   // [1] note selection: root C3(60), pattern = +7, REST, +12, length 4 (root + 3 rows), looping
        int8_t off[DC::ARP_ROWS]; for (auto& o : off) o = DC::ARP_REST;
        off[0] = 7; off[1] = DC::ARP_REST; off[2] = 12;
        const int len = 4;   // steps: 0=root, 1=+7, 2=rest, 3=+12
        const int s0 = DC::arpNoteAt(off, len, 60, 0);
        const int s1 = DC::arpNoteAt(off, len, 60, 1);
        const int s2 = DC::arpNoteAt(off, len, 60, 2);
        const int s3 = DC::arpNoteAt(off, len, 60, 3);
        const int s4 = DC::arpNoteAt(off, len, 60, 4);   // wraps to step 0 = root
        printf("[1] steps: %d %d %d %d (wrap %d) -> %s\n", s0, s1, s2, s3, s4,
               CHK(s0==60 && s1==67 && s2==-1 && s3==72 && s4==60) ? "root/offset/REST/wrap OK" : "FAIL");
    }
    {   // [2] Notes/bar fader snap onto {7,8,9,10,11,13} (nearest; tie -> lower) + the Rate table
        printf("[2] snap: 8->%d 12->%d 6->%d 100->%d | rates x%.2f..x%.1f, x1.5=%.1f -> %s\n",
               DC::arpSnapSync(8), DC::arpSnapSync(12), DC::arpSnapSync(6), DC::arpSnapSync(100),
               DC::arpRateMul(0), DC::arpRateMul(10), DC::arpRateMul(6),
               CHK(DC::arpSnapSync(8)==8 && DC::arpSnapSync(12)==11 && DC::arpSnapSync(6)==7
                   && DC::arpSnapSync(100)==13 && std::abs(DC::arpRateMul(0)-0.25)<1e-9
                   && std::abs(DC::arpRateMul(6)-1.5)<1e-9 && std::abs(DC::arpRateMul(10)-3.0)<1e-9) ? "OK" : "FAIL");
    }
    {   // [3] an arp note (root + offset) rendered via keyDown plays the right pitch: root C3 + 7 = G3
        DrumChannel ch;
        for (auto& sl : ch.slots) sl = DrumChannel::Slot();
        ch.slots[0].engine = DrumChannel::SrcOsc; ch.slots[0].weight = 1.0f;
        ch.slots[0].oscShape = ch.slots[0].oscShapeB = DrumChannel::WvSine; ch.slots[0].oscFreq = 261.6256f;
        ch.slots[0].atk = 0.002f; ch.slots[0].dec = 0.4f; ch.slots[0].sustain = 0.9f;
        ch.prepareToPlay(SR, bs);
        const int note = DC::arpNoteAt(nullptr ? nullptr : ch.arpOffset, 2, 60, 1);   // default pattern: +12 -> C4
        ch.keyDown(note, 1.0f, 0, false);
        std::vector<float> out; juce::AudioBuffer<float> buf(2, bs);
        for (int b = 0; b < (int)(0.3*SR/bs)+1; ++b) { buf.clear(); ch.renderInto(buf,0,bs,false); for (int i=0;i<bs;++i) out.push_back(buf.getSample(0,i)); }
        const double C4 = 523.25, C3 = 261.63;
        const double hi = goertzel(out,(size_t)(0.05*SR),(size_t)(0.25*SR),C4,SR);
        const double lo = goertzel(out,(size_t)(0.05*SR),(size_t)(0.25*SR),C3,SR);
        printf("[3] default arp row1 note=%d -> C4=%.3f C3=%.3f -> %s\n", note, hi, lo,
               CHK(note==72 && hi>0.1 && hi>lo*4) ? "plays root+octave (OK)" : "FAIL");
    }

    printf(fails ? "\n>>> ARP FAILURES\n" : "\n>>> ArpTest PASS\n");
    return fails;
}
