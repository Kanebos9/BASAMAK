#pragma once
#include "PluginProcessor.h"

// One common time axis; each row is an instrument, not a MIDI pitch.
class DrumGridComponent : public juce::Component, public juce::SettableTooltipClient
{
  public:
    explicit DrumGridComponent(DrumSequencerProcessor &p) : proc(p)
    {
        setWantsKeyboardFocus(true);
    }
    std::function<void()> beforeEdit;
    std::function<void(int)> selectChannel;
    int firstRow = 0, rows = 8, rowHeight = 44, snap = 16;
    void update();
    void quantize();
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool keyPressed(const juce::KeyPress &) override;
    juce::String getTooltip() override;

  private:
    DrumSequencerProcessor &proc;
    struct VisibleHit
    {
        int pattern, index;
        LiveDrumming::Hit hit;
    };
    std::vector<VisibleHit> mirror;
    int head = 0, bars = 1, playBar = -1, dragPattern = -1, dragIndex = -1;
    double playPos = 0, grabX = 0;
    float grabVelocity = 1;
    bool recording = false, velocityDrag = false;
    juce::Point<int> down;
    int hitAt(juce::Point<int>) const;
    double timeAt(int x) const;
    float xFor(const VisibleHit &) const;
    void editMenu(int i);
};

class DrumModePrompt : public juce::Component
{
  public:
    juce::TextButton keep{"Keep and convert"}, fresh{"Start fresh"}, cancel{"Cancel"};
    std::function<void(int)> onChoice;
    bool destinationDrums = true;
    juce::String error;
    DrumModePrompt()
    {
        for (auto *b : {&keep, &fresh, &cancel})
            addAndMakeVisible(b);
        keep.setTooltip("Convert notes and saved takes across ALL patterns. To drums: keep rhythm, velocity "
                        "and pan; pitches, held lengths, per-slot choices, step modulation and loop conditions become natural "
                        "drum hits. Simultaneous chord notes become one hit. To regular: create one-shot "
                        "piano-roll notes using each channel's tuning; timing rounds to the piano-roll "
                        "resolution. Undo restores the previous mode.");
        fresh.setTooltip(
            "Delete sequence notes and saved takes in ALL patterns. Keep every sound, effect, mixer setting, "
            "pattern chain and MIDI assignment. Undo restores the previous mode.");
        cancel.setTooltip("Close this message without changing modes or any notes, takes or settings.");
        keep.onClick = [this]
        {
            if (onChoice)
                onChoice(1);
        };
        fresh.onClick = [this]
        {
            if (onChoice)
                onChoice(2);
        };
        cancel.onClick = [this]
        {
            if (onChoice)
                onChoice(0);
        };
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colour(0xff1d2438));
        g.setColour(juce::Colour(0xffaebcdf));
        g.drawRect(getLocalBounds(), 2);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(18, juce::Font::bold));
        g.drawText(destinationDrums ? "Switch to Live Drumming?" : "Switch to regular mode?", 18, 14,
                   getWidth() - 36, 26, juce::Justification::centredLeft);
        g.setFont(14);
        g.drawFittedText(error.isNotEmpty()
                             ? error
                             : (destinationDrums
                                    ? "Keep and convert preserves rhythm, velocity and pan. Melodies become "
                                      "fixed-pitch drum hits; chords become one hit. Note lengths, slot "
                                      "choices, glide, step modulation and loop conditions do not carry over."
                                    : "Keep and convert puts each drum on its channel's regular piano roll, "
                                      "using natural decay and the channel's tuning. Timing rounds to the "
                                      "piano roll's resolution. Kit takes become separate channel takes within each pattern group."),
                         18, 52, getWidth() - 36, 78, juce::Justification::topLeft, 4);
        g.setColour(juce::Colour(0xffc2c9d8));
        g.setFont(13);
        g.drawFittedText("Applies to ALL patterns and saved takes. Sounds, effects, mixer settings and MIDI "
                         "assignments stay intact. Hover a choice for details. You can Undo the switch.",
                         18, 138, getWidth() - 36, 48, juce::Justification::topLeft, 3);
    }
    void resized() override
    {
        keep.setBounds(18, getHeight() - 46, 190, 30);
        fresh.setBounds(220, getHeight() - 46, 170, 30);
        cancel.setBounds(getWidth() - 138, getHeight() - 46, 120, 30);
    }
};
