#pragma once
#include "../dsp/Sequencer.h"

//==============================================================================
// Built-in, read-only factory content that ships with DavulSEQ.
//   * Sound mixes  - single-channel sounds built from Osc / Noise / FM only
//                    (no samples), so they always work out of the box.
//   * Presets      - whole-kit grooves including BPM and time signature.
// The user can load and then tweak these, but cannot overwrite them (they live
// in code, not in the user's preset folders). Saving always creates new files.
//==============================================================================
namespace Factory
{
    juce::StringArray mixNames();
    juce::StringArray mixCategories();            // parallel to mixNames(): sound TYPE (Kicks/Snares/...)
    juce::String      mixSourceTag(int index);    // which sound source(s): Analog/FM/Noise/Physical/Sample/Hybrid
    void applyMix(DrumChannel& ch, int index);   // index into mixNames()

    juce::StringArray presetNames();
    void applyPreset(Sequencer& seq, int index);  // index into presetNames()
}
