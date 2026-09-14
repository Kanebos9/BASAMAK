#include "LiveDrumming.h"

namespace
{
juce::String pack(const LiveDrumming::Hit &h)
{
    return juce::String(h.pos, 12) + ":" + juce::String(h.channel) + ":" + juce::String(h.velocity, 8) + ":" +
           juce::String(h.pan, 8);
}
bool unpack(const juce::String &text, LiveDrumming::Hit &h)
{
    auto f = juce::StringArray::fromTokens(text, ":", "");
    if (f.size() != 4)
        return false;
    h = {f[0].getDoubleValue(), f[1].getIntValue(), f[2].getFloatValue(), f[3].getFloatValue()};
    return std::isfinite(h.pos) && h.pos >= 0 && h.pos < 1 && h.channel >= 0 &&
           h.channel < LiveDrumming::CHANNELS && std::isfinite(h.velocity) && h.velocity > 0 &&
           h.velocity <= 1 && std::isfinite(h.pan) && std::abs(h.pan) <= 1;
}
} // namespace
juce::ValueTree LiveDrumming::save() const
{
    juce::ValueTree root("LiveDrumming");
    root.setProperty("enabled", enabled, nullptr);
    for (int c = 0; c < CHANNELS; ++c)
    {
        juce::ValueTree m("Map");
        m.setProperty("ch", c, nullptr);
        m.setProperty("note", notes[(size_t)c], nullptr);
        m.setProperty("midiCh", midiChannels[(size_t)c], nullptr);
        root.addChild(m, -1, nullptr);
    }
    for (int p = 0; p < PATTERNS; ++p)
        if (patterns[(size_t)p].count)
        {
            juce::ValueTree b("Bar");
            b.setProperty("p", p, nullptr);
            juce::String hits;
            const auto &lane = patterns[(size_t)p];
            for (int i = 0; i < lane.count; ++i)
                hits += pack(lane.hits[(size_t)i]) + ",";
            b.setProperty("hits", hits, nullptr);
            root.addChild(b, -1, nullptr);
        }
    for (const auto &take : takes)
    {
        juce::ValueTree t("Take");
        t.setProperty("name", take.name, nullptr);
        t.setProperty("head", take.head, nullptr);
        t.setProperty("bars", take.bars, nullptr);
        juce::String hits;
        for (const auto &h : take.hits)
            hits += juce::String(h.pattern) + "/" + pack(h.hit) + ",";
        t.setProperty("hits", hits, nullptr);
        root.addChild(t, -1, nullptr);
    }
    return root;
}
void LiveDrumming::restore(const juce::ValueTree &root)
{
    enabled = root.isValid() && (bool)root.getProperty("enabled", false);
    clearNotes();
    defaultMap();
    takes.clear();
    pendingTake = {};
    recording = false;
    passHead = -1;
    logCount = inputCount = 0;
    recordOverflow = inputOverflow = false;
    lastNote = -1;
    if (!root.isValid())
        return;
    // Read mappings together: persisted disjoint MIDI channels may share a note.
    for (auto child : root)
        if (child.hasType("Map"))
        {
            int c = (int)child.getProperty("ch", -1);
            if (c < 0 || c >= CHANNELS)
                continue;
            notes[(size_t)c] = juce::jlimit(-1, 127, (int)child.getProperty("note", -1));
            midiChannels[(size_t)c] = juce::jlimit(0, 16, (int)child.getProperty("midiCh", 0));
        }
    for (auto child : root)
    {
        if (child.hasType("Bar"))
        {
            int p = (int)child.getProperty("p", -1);
            if (p < 0 || p >= PATTERNS)
                continue;
            for (const auto &s : juce::StringArray::fromTokens(child.getProperty("hits").toString(), ",", ""))
            {
                Hit h;
                if (unpack(s, h))
                    patterns[(size_t)p].add(h);
            }
        }
        else if (child.hasType("Take") && (int)takes.size() < MAX_TAKES)
        {
            Take t;
            t.head = (int)child.getProperty("head", -1);
            t.bars = (int)child.getProperty("bars", 1);
            if (t.head < 0 || t.head >= PATTERNS || t.bars < 1 || t.bars > 8 || t.head + t.bars > PATTERNS ||
                takesFor(t.head) >= 20)
                continue;
            t.name = child.getProperty("name", "Kit take").toString();
            std::array<int, PATTERNS> counts{};
            for (const auto &s : juce::StringArray::fromTokens(child.getProperty("hits").toString(), ",", ""))
            {
                if (!s.containsChar('/'))
                    continue;
                int p = s.upToFirstOccurrenceOf("/", false, false).getIntValue();
                Hit h;
                if (p >= t.head && p < t.head + t.bars && counts[(size_t)p] < MAX_HITS &&
                    unpack(s.fromFirstOccurrenceOf("/", false, false), h))
                {
                    t.hits.push_back({p, h});
                    ++counts[(size_t)p];
                }
            }
            if (!t.hits.empty())
                takes.push_back(std::move(t));
        }
    }
}
