// MixDump: print every property (+ Slot children) of a .basamaksound file, so a user sound can
// be hand-translated into a factory builder. Informational tool - NOT in run.sh.
//   ./MixDump <file.basamaksound> [...]
#include <JuceHeader.h>
#include <cstdio>

static void dumpTree(const juce::ValueTree& t, int indent)
{
    juce::String pad = juce::String::repeatedString("  ", indent);
    std::printf("%s<%s>\n", pad.toRawUTF8(), t.getType().toString().toRawUTF8());
    for (int i = 0; i < t.getNumProperties(); ++i)
    {
        const auto name = t.getPropertyName(i);
        const auto v    = t.getProperty(name);
        std::printf("%s  %-12s = %s\n", pad.toRawUTF8(), name.toString().toRawUTF8(),
                    v.toString().toRawUTF8());
    }
    for (const auto& c : t)
        dumpTree(c, indent + 1);
}

int main(int argc, char** argv)
{
    for (int a = 1; a < argc; ++a)
    {
        juce::File f (juce::String::fromUTF8(argv[a]));
        juce::MemoryBlock mb;
        if (! f.loadFileAsData(mb)) { std::printf("!! cannot read %s\n", argv[a]); continue; }
        auto t = juce::ValueTree::readFromData(mb.getData(), mb.getSize());
        if (! t.isValid()) { std::printf("!! not a ValueTree: %s\n", argv[a]); continue; }
        std::printf("==== %s\n", f.getFileName().toRawUTF8());
        dumpTree(t, 0);
    }
    return 0;
}
