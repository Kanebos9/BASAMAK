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

    juce::StringArray kickNames, accentNames, transitionNames;
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
        if (s->drums.enabled)
        {
            CHK(!s->drums.hasNotes() && s->drums.takes.empty());
            CHK(s->drums.target(46, 10) == 7);
            kickNames.addIfNotAlreadyThere(s->patterns[0].channels[4].mixName);
            accentNames.addIfNotAlreadyThere(s->patterns[0].channels[1].mixName);
            transitionNames.addIfNotAlreadyThere(s->patterns[0].channels[2].mixName);
            const auto names = Factory::mixNames(), cats = Factory::mixCategories();
            for (const auto& pat : s->patterns)
            {
                int toms = 0;
                for (int ch = 0; ch < 16; ++ch)
                {
                    const auto& c = pat.channels[ch];
                    const int sound = names.indexOf(c.mixName);
                    if (sound >= 0 && cats[sound] == "Toms") { ++toms; CHK(ch == 6); }
                    CHK(c.keysMaxVel == 1);
                    CHK(c.liveChokeBy == (ch == 2 ? 4 : -1));
                }
                CHK(toms == 1);
                CHK(pat.channels[4].keysMinVel == 1); // all kick/bass pads are full velocity
                CHK(pat.channels[6].keysMinVel == 1); // low toms stay audible too
                CHK(pat.channels[5].keysMinVel == 0); // snares/slaps retain expression
            }
            const int physicalNotes[] = {49, 48, 45, 51, 36, 38, 43, 42};
            for (int ch = 0; ch < 8; ++ch) CHK(s->drums.target(physicalNotes[ch], 10) == ch);
            // Check sound identity as well as note routing: moving just the map swaps the pads.
            if (Factory::presetNames()[preset] == "Live Drums - Room Session")
            {
                CHK(s->patterns[0].channels[4].mixName == "Snap Kick");
                CHK(s->patterns[0].channels[5].mixName == "Mod Snare");
            }
            juce::AudioBuffer<float> hit(2, bs);
            for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch)
            {
                CHK(s->drums.notes[(size_t)ch] >= 0);
                CHK(s->drums.target(s->drums.notes[(size_t)ch], 10) == ch);
                auto& c = s->patterns[0].channels[ch];
                c.trigger(0.8f, 0, 0, 0, 0, 0, false, 0, false, true);
                double peak = 0;
                for (int b = 0; b < 12; ++b)
                { hit.clear(); c.renderInto(hit, 0, bs, false); peak = std::max(peak, (double)hit.getMagnitude(0, bs)); }
                printf("    pad %2d %-18s peak %.4f\n", ch + 1, c.mixName.toRawUTF8(), peak);
                CHK(std::isfinite(peak) && peak > 0.001 && peak < 4.0);
            }
            // A live kit has no demo notes: exercise incoming pad routing for the render below.
            s->drums.beginBlock();
            s->drums.noteOn(36, 10, 100, 0);
        }
        else s->startStandalone();
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
    printf("[kit variety] %d kick bodies, %d accents, %d transitions\n", kickNames.size(), accentNames.size(), transitionNames.size());
    CHK(kickNames.size() == 10 && accentNames.size() >= 7 && transitionNames.size() >= 8);
    return fails;
}
