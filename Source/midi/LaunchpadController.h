#pragma once
#include <JuceHeader.h>

//==============================================================================
// Manages bidirectional communication with Novation Launchpad Mini MK3.
//
// Layout (Programmer Mode):
//   8 rows  = 8 drum channels  (row 8 = channel 1 at top, row 1 = channel 8 at bottom)
//   8 cols  = up to 8 steps visible (page A = steps 0-7, page B = steps 8-15)
//   Right scene buttons (col 9) = page A/B toggle per row
//
// Note number formula: row*10 + col  (row 1-8 from bottom, col 1-8 from left)
//==============================================================================
class LaunchpadController
{
public:
    static constexpr uint8_t COLOR_OFF       =  0;
    static constexpr uint8_t COLOR_DIM       =  1;
    static constexpr uint8_t COLOR_GREEN_ON  = 21;  // step is active
    static constexpr uint8_t COLOR_GREEN_DIM =  4;  // step is off but channel active
    static constexpr uint8_t COLOR_AMBER     =  9;  // current playing step
    static constexpr uint8_t COLOR_RED       = 11;  // muted channel
    static constexpr uint8_t COLOR_PAGE_B    = 48;  // cyan = page B indicator

    // Returns MIDI messages to send to Launchpad to enter Programmer Mode
    static juce::MidiBuffer buildProgrammerModeInit();

    // Called when a step changes (on/off), or play position moves, or channel muted.
    // Returns MIDI messages to send to Launchpad.
    juce::MidiBuffer buildFullRefresh(
        const bool steps[8][16],        // [channel][step]
        const bool muted[8],
        const int  currentPlayStep[8],  // per-channel current step (-1 if stopped)
        const int  numSteps[8],
        const bool pageB[8]);           // whether each channel is on page B

    // Parse an incoming MIDI message from the Launchpad.
    // Returns true if it was a pad press (and fills channel/step/pressed).
    bool parsePadMessage(const juce::MidiMessage& msg,
                         int& outChannel, int& outStep, bool& outPressed,
                         const bool pageB[8]);

    // Compute the note number for a given (row 1-8, col 1-8) in programmer mode
    static int padNote(int row, int col) { return row * 10 + col; }
    // Right scene button (col 9) for a row
    static int sceneNote(int row) { return row * 10 + 9; }

private:
    // Row 1 = bottom = channel 8, Row 8 = top = channel 1
    static int channelToRow(int ch) { return 8 - ch; }
    static int rowToChannel(int row) { return 8 - row; }
};
