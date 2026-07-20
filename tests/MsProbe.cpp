// MsProbe [2026-07-20] - INFORMATIONAL tool (not in run.sh): load a real multisample folder and
// print the MEASURED pitch for a few played notes. Usage: MsProbe "<folder>" [semi semi ...]
// (semis are roll offsets from C4; default probes A#2, C3, D3, E3, G3.)
#include "Sequencer.h"
#include "SpectrumTap.h"
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI ji;
    if (argc < 2) { printf("usage: MsProbe <folder> [semi ...]\n"); return 1; }
    juce::File dir { juce::String(argv[1]) };
    if (dir.existsAsFile())   // audio-file mode: print the file's own pitch over time
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(dir));
        if (! r) { printf("unreadable\n"); return 1; }
        juce::AudioBuffer<float> b((int) r->numChannels, (int) r->lengthInSamples);
        r->read(&b, 0, (int) r->lengthInSamples, 0, true, true);
        const int W = 8192;
        for (double t = 0.2; t + (double) W / r->sampleRate < r->lengthInSamples / r->sampleRate; t += 0.4)
        {
            const double hz = basamakDetectPitch(b.getReadPointer(0) + (int)(t * r->sampleRate), W, r->sampleRate);
            const double m = hz > 0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0.0;
            printf("t=%.1fs  %.1f Hz  midi %.2f\n", t, hz, m);
        }
        return 0;
    }
    std::vector<int> semis;
    for (int i = 2; i < argc; ++i) semis.push_back(atoi(argv[i]));
    if (semis.empty()) semis = { -14, -12, -10, -8, -5 };   // A#2 C3 D3 E3 G3

    const double SR = 48000.0; const int bs = 512;
    auto* s = new Sequencer();
    s->setStandaloneBpm(60.0f);   // 1 bar = 4 s -> roomy per-note windows
    auto& ch = s->patterns[0].channels[0];
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    ch.slots[0].engine = DrumChannel::SrcSample; ch.slots[0].weight = 1.0f;
    ch.drawMode = true;
    for (size_t i = 0; i < semis.size(); ++i)
        ch.addDrawNote((int)(i * 64), 56, semis[i], 255, 0);
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    if (! ch.loadMultisample(0, dir)) { printf("load FAILED\n"); return 1; }
    printf("loaded: nVoices=%d zones=%d loop=%d preserve=%d\n",
           ch.msSet[0]->nVoices, (int) ch.msSet[0]->zones.size(),
           (int) ch.slots[0].msLoopOn, (int) ch.slots[0].smpPreservePitch);
    for (auto& z : ch.msSet[0]->zones)
        printf("  zone: voice=%d root=%d cents=%.1f layers=%d\n", z.voice, z.root, z.cents, (int) z.layers.size());
    s->startStandalone();

    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    const double total = (double) semis.size() * (64.0 / 384.0) * 4.0 + 0.5;
    for (int b = 0; b < (int)(total * SR / bs) + 1; ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }

    for (size_t i = 0; i < semis.size(); ++i)
    {
        const int W = 8192;
        double mm[2] = { 0.0, 0.0 };
        for (int k = 0; k < 2; ++k)
        {
            const double t0 = (double) i * (64.0 / 384.0) * 4.0 + (k == 0 ? 0.10 : 0.42);
            const size_t o = (size_t)(t0 * SR);
            double hz = 0.0;
            if (o + (size_t) W < out.size()) hz = basamakDetectPitch(out.data() + o, W, SR);
            mm[k] = hz > 0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0.0;
        }
        printf("semi %+3d (expect midi %3d)  ->  early midi %.2f (%+.2f st)   late midi %.2f (%+.2f st)\n",
               semis[i], 60 + semis[i], mm[0], mm[0] - (60 + semis[i]), mm[1], mm[1] - (60 + semis[i]));
    }
    delete s;
    return 0;
}
