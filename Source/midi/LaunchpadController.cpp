#include "LaunchpadController.h"

juce::MidiBuffer LaunchpadController::buildProgrammerModeInit()
{
    juce::MidiBuffer buf;
    // SysEx: switch Launchpad Mini MK3 to Programmer Mode
    const uint8_t sysex[] = { 0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x01, 0xF7 };
    buf.addEvent(juce::MidiMessage(sysex, sizeof(sysex)), 0);
    return buf;
}

juce::MidiBuffer LaunchpadController::buildFullRefresh(
    const bool steps[8][16],
    const bool muted[8],
    const int  currentPlayStep[8],
    const int  numSteps[8],
    const bool pageB[8])
{
    juce::MidiBuffer buf;
    int ts = 0; // timestamp within buffer

    for (int ch = 0; ch < 8; ++ch)
    {
        int row = channelToRow(ch); // 1-8

        for (int col = 1; col <= 8; ++col)
        {
            int stepIndex = (pageB[ch] ? 8 : 0) + (col - 1);
            int note = padNote(row, col);

            uint8_t color = COLOR_OFF;

            if (stepIndex < numSteps[ch])
            {
                // Is this step the currently playing one?
                int localPlayStep = currentPlayStep[ch];
                bool isCurrentStep = (stepIndex == localPlayStep) && currentPlayStep[ch] >= 0;

                if (isCurrentStep)
                    color = COLOR_AMBER;
                else if (steps[ch][stepIndex])
                    color = muted[ch] ? COLOR_RED : COLOR_GREEN_ON;
                else
                    color = muted[ch] ? COLOR_OFF : COLOR_DIM;
            }
            else
            {
                // Steps beyond the channel's numSteps — show as unavailable
                color = COLOR_OFF;
            }

            buf.addEvent(juce::MidiMessage::noteOn(1, note, (uint8_t)color), ts++);
        }

        // Scene button (col 9) = page indicator
        int sceneN = sceneNote(row);
        uint8_t sceneColor = pageB[ch] ? COLOR_PAGE_B : COLOR_DIM;
        buf.addEvent(juce::MidiMessage::noteOn(1, sceneN, (uint8_t)sceneColor), ts++);
    }

    return buf;
}

bool LaunchpadController::parsePadMessage(const juce::MidiMessage& msg,
                                           int& outChannel, int& outStep,
                                           bool& outPressed,
                                           const bool pageB[8])
{
    if (!msg.isNoteOnOrOff()) return false;

    int note = msg.getNoteNumber();
    outPressed = msg.isNoteOn() && msg.getVelocity() > 0;

    // Decode row and col from note number
    int row = note / 10;
    int col = note % 10;

    // Valid pad area: row 1-8, col 1-9
    if (row < 1 || row > 8 || col < 1 || col > 9) return false;

    int ch = rowToChannel(row); // 0-7

    if (col == 9)
    {
        // Scene button: used externally as page toggle, not a step press
        // Signal with step = -1 so caller knows it's a page button
        outChannel = ch;
        outStep    = -1; // special: page toggle
        return true;
    }

    int stepIndex = (pageB[ch] ? 8 : 0) + (col - 1);
    outChannel = ch;
    outStep    = stepIndex;
    return true;
}
