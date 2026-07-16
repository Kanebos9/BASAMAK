// FACTORY-SOUND UI-REPLICABILITY AUDIT (informational; exits 0). Lists every sound that relies
// on HIDDEN state a user cannot set from the UI: channel drive (no knob), channel reverb/delay
// sends (the visible Reverb/Delay knobs are PER-SLOT sends), the channel Formant filter (combo
// hidden), "Replicate from the UI" must be possible for all.
#include "Sequencer.h"
#include "FactoryContent.h"
#include <cstdio>

int main() {
    const auto names = Factory::mixNames();
    int bad = 0;
    for (int i = 0; i < names.size(); ++i)
    {
        auto* ch = new DrumChannel();
        Factory::applyMix(*ch, i);
        juce::StringArray v;
        if (ch->driveType != DrumChannel::DriveOff && ch->driveAmount > 0.001f)
            v.add("chanDrive(" + juce::String(ch->driveType) + "," + juce::String(ch->driveAmount, 2) + ")");
        // (channel reverbSend/delaySend are VISIBLE controls now - the CHANNEL FX box send faders,
        //  2026-07-13 - so they are no longer hidden state.)
        if (ch->filterType != 0)     v.add("chanFilter(" + juce::String(ch->filterType) + ")");
        for (int s = 0; s < DrumChannel::NUM_SLOTS; ++s)
        {
            if (ch->slots[s].engine < 0 || ch->slots[s].weight <= 0.001f) continue;
            if (ch->slots[s].oscUniCenter)   // the centre-voice toggle's UI was removed with the chord chips
                v.add("slot" + juce::String(s + 1) + "uniCenter");
        }
        if (! v.isEmpty()) { ++bad; printf("%-16s %s\n", names[i].toRawUTF8(), v.joinIntoString(" ").toRawUTF8()); }
        delete ch;
    }
    printf("== %d of %d sounds rely on hidden state ==\n", bad, names.size());
    return 0;
}
