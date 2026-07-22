#pragma once
#include <JuceHeader.h>

//==============================================================================
// Where BASAMAK keeps the user's library on disk. One tidy, cross-platform
// location that lives OUTSIDE the plugin binary, so reinstalling/updating the
// plugin never touches the user's samples, sounds or presets.
//
//   macOS / Windows / Linux:  <user Documents>/BASAMAK/
//        Samples/        - audio files (factory + anything the user adds)
//        Sound Bank/     - *.basamaksound  (saved channel sounds)
//        Presets/        - *.basamakpreset (saved whole-instrument presets)
//        MIDI Patterns/  - *.basamakpattern (saved per-pattern note grids)
//
// On first run the folders are seeded (once) by COPYING from the older,
// inconsistently-named locations used by earlier builds (the whole old
// "DavulSEQ" root, plus the old "Sound Mixes" -> "Sound Bank" rename). We only
// ever COPY, so the originals are left untouched.
//==============================================================================
namespace UserPaths
{
    inline juce::File documents()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    inline juce::File root()
    {
        auto r = documents().getChildFile("BASAMAK");
        if (! r.exists())
        {
            auto legacy = documents().getChildFile("DavulSEQ");   // bring the old library over once
            if (legacy.isDirectory()) legacy.copyDirectoryTo(r);
        }
        r.createDirectory();
        return r;
    }

    // Get a sub-folder, one-time-seeding it from an older name inside the root (rename) and/or a
    // legacy folder directly under Documents (pre-1.0 builds). COPY only; never deletes originals.
    inline juce::File sub(const juce::String& name, const juce::String& legacyInRoot,
                          const juce::String& legacyInDocs)
    {
        auto dir = root().getChildFile(name);
        if (! dir.exists())
        {
            dir.createDirectory();
            if (legacyInRoot.isNotEmpty()) { auto l = root().getChildFile(legacyInRoot);
                                             if (l.isDirectory() && l != dir) l.copyDirectoryTo(dir); }
            if (legacyInDocs.isNotEmpty()) { auto l = documents().getChildFile(legacyInDocs);
                                             if (l.isDirectory() && l != dir) l.copyDirectoryTo(dir); }
        }
        return dir;
    }

    inline juce::File samples()    { return sub("Samples",    "Samples",     "DrumSequencer Samples"); }
    inline juce::File presets()    { return sub("Presets",    "Presets",     "DrumSequencer Presets"); }
    // [2026-07-18] MULTISAMPLES: one SUBFOLDER per instrument, plain WAVs NAMED BY NOTE
    // ("E1.wav", "C#3.wav" or a MIDI number "40.wav") - human-editable, shareable as a zip,
    // hand-assemblable from any WAVs by renaming. No custom container (user flexibility call).
    inline juce::File namModels()  { return sub("NAM Models", "", ""); }   // [2026-07-18] .nam captures (incl. A2)
    inline juce::File cabIrs()     { return sub("Cab IRs", "", ""); }      // [2026-07-18] speaker-cabinet impulse WAVs
    inline juce::File multisamples() { return sub("Multisamples", "", ""); }
    inline juce::File styles()     { return sub("Styles", "", ""); }       // [2026-07-22 r22] *.basamakstyle GENERATE styles
    inline juce::File midiPatterns() { return sub("MIDI Patterns", "", ""); }   // saved *.basamakpattern note grids

    inline juce::File soundMixes()
    {
        auto bank = sub("Sound Bank", "Sound Mixes", "DavulSEQ Sound Mixes");
        // Remove the now-redundant in-root "Sound Mixes" left by an earlier migration: move any files it still
        // holds into "Sound Bank" (without overwriting), then delete it. Idempotent (skips once it's gone).
        auto oldMix = root().getChildFile("Sound Mixes");
        if (oldMix.isDirectory() && oldMix != bank)
        {
            for (auto& f : oldMix.findChildFiles(juce::File::findFiles, true))
            {
                auto dest = bank.getChildFile(f.getRelativePathFrom(oldMix));
                if (! dest.existsAsFile()) { dest.getParentDirectory().createDirectory(); f.copyFileTo(dest); }
            }
            oldMix.deleteRecursively();
        }
        return bank;
    }
}
