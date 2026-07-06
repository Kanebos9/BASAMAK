// PRESET MODERNIZATION regression: applyPreset no longer runs the blanket buildSlotsFromLegacy pass -
// every channel's slots[] must BE its sound the moment the preset lands (the modern invariant).
//  [1] every channel of every pattern has at least one authored slot (engine >= 0) after applyPreset
//  [2] the preset actually SOUNDS (renders non-silence) - both factory presets
//  [3] a slot-authored sound applied onto a preset-loaded kit KEEPS its authored slots
#include "Sequencer.h"
#include "FactoryContent.h"
#include <cstdio>
#include <cmath>
#include <vector>

int main() {
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 48000.0; const int bs = 512;

    for (int preset = 0; preset < Factory::presetNames().size(); ++preset)
    {
        auto* s = new Sequencer();
        Factory::applyPreset(*s, preset);
        // [1] modern invariant: every channel WITH a sound has authored slots the moment the
        // preset lands (unbuilt/empty channels have no sound and stay slot-less, as always).
        bool allAuthored = true; int sounds = 0;
        for (auto& pat : s->patterns)
            for (auto& ch : pat.channels)
            {
                if (ch.mixName.isEmpty()) continue;
                ++sounds;
                bool a = false;
                for (auto& sl : ch.slots) if (sl.engine >= 0) { a = true; break; }
                allAuthored = allAuthored && a;
            }
        printf("[1] preset %d '%s': %d sound channels, all slot-authored -> %s\n", preset,
               Factory::presetNames()[preset].toRawUTF8(), sounds, CHK(allAuthored && sounds > 0) ? "OK" : "FAIL");
        // [2] it makes sound
        for (auto& p : s->patterns) for (auto& c : p.channels) c.prepareToPlay(SR, bs);
        s->startStandalone();
        double acc = 0.0;
        juce::AudioBuffer<float> buf(2, bs);
        const int blocks = (int) (1.0 * SR / bs);
        for (int b = 0; b < blocks; ++b)
        { buf.clear(); s->processBlock(buf, SR, bs, nullptr);
          for (int i = 0; i < bs; ++i) acc += std::abs(buf.getSample(0, i)); }
        const double mean = acc / (blocks * bs);
        printf("[2] preset %d renders: mean |x| = %.5f -> %s\n", preset, mean, CHK(mean > 1.0e-4) ? "OK" : "FAIL");
        delete s;
    }
    {   // [3] slot-authored sound survives on a preset-loaded kit
        auto* s = new Sequencer();
        Factory::applyPreset(*s, 0);
        auto names = Factory::mixNames();
        int idx = -1;
        for (int i = 0; i < names.size(); ++i) if (names[i] == "E-Piano") { idx = i; break; }
        if (idx < 0) { printf("[3] E-Piano not found in mixNames -> FAIL\n"); ++fails; }
        else
        {
            auto& ch = s->patterns[0].channels[5];
            Factory::applyMix(ch, idx);
            bool authored = false;
            for (auto& sl : ch.slots) if (sl.engine >= 0) { authored = true; break; }
            printf("[3] E-Piano onto preset kit keeps authored slots -> %s\n", CHK(authored) ? "OK" : "FAIL");
        }
        delete s;
    }
    return fails;
}
