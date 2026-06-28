#pragma once
#include <JuceHeader.h>

//==============================================================================
// Maps MIDI CC messages to parameter indices.
// Right-click a control → "Assign MIDI CC" → wiggle a knob on your controller.
//
// paramId: a unique string for each controllable parameter.
//==============================================================================
class MidiLearnManager
{
public:
    static constexpr int NUM_MIDI_CHANNELS = 16;

    // Start listening for the next CC to assign to paramId.
    // If forcedChannel > 0, the assignment uses that MIDI channel instead of the
    // channel the incoming CC arrived on (used so a control adopts its pattern's
    // channel). Pass -1 to use the incoming message's channel.
    void startLearning(const juce::String& paramId, int forcedChannel = -1);
    void stopLearning();
    bool isLearning() const { return learning.load(std::memory_order_acquire); }
    juce::String getLearningParam() const { return learningParamId; }

    // Call from audio thread on every incoming MIDI message.
    // Returns true if the message was consumed for learning.
    bool processMidiMessage(const juce::MidiMessage& msg);

    // Returns the current CC assignment for a param, or -1 if none.
    int getCCForParam(const juce::String& paramId) const;

    // Returns the MIDI channel assigned to a param, or -1 if none.
    int getChannelForParam(const juce::String& paramId) const;

    // Returns the param assigned to a given CC (and midi channel), or "" if none.
    juce::String getParamForCC(int ccNumber, int midiChannel) const;

    // Clears the assignment for a param
    void clearParam(const juce::String& paramId);

    // Clears EVERY assignment (the "Clear MIDI" button).
    void clearAll();

    // Re-channel every assignment whose paramId starts with `prefix`: keep the
    // CC number, change only the MIDI channel. Used when a pattern's channel
    // changes so all its controls move together without losing their CCs.
    void setChannelForPrefix(const juce::String& prefix, int newChannel);

    // Save / restore
    juce::ValueTree saveState() const;
    void            loadState(const juce::ValueTree& tree);

    // Listener: called on message thread when an assignment changes
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void midiLearnAssignmentChanged(const juce::String& paramId) = 0;
    };
    void addListener(Listener* l)    { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

private:
    struct Assignment
    {
        int ccNumber    = -1;
        int midiChannel = -1;
    };

    juce::HashMap<juce::String, Assignment> paramToCC;
    // Reverse map: cc*16 + (channel-1) -> paramId
    juce::HashMap<int, juce::String> ccToParam;

    // The maps are touched by BOTH the audio thread (getParamForCC + the learn capture in processMidiMessage)
    // and the message thread (assign/clear/load). juce::HashMap is NOT thread-safe, so all access is guarded by
    // this lock. The AUDIO thread only ever TRY-locks (never blocks): if it can't acquire, it skips that one
    // message (a CC just doesn't route / a learn capture retries on the next CC) - never a glitch, never UB.
    mutable juce::CriticalSection mapCS;

    // 'learning' is set on the message thread and read on the audio thread, so it
    // MUST be atomic - a plain bool can be cached in a register (especially under
    // LTO), so the audio thread would never see learning go true and would never
    // capture the incoming CC. learningParamId is only ever WRITTEN on the message
    // thread (startLearning); the audio thread only reads it, which is safe.
    std::atomic<bool> learning { false };
    juce::String      learningParamId;
    int               learningForcedChannel = -1;

    juce::ListenerList<Listener> listeners;

    void assign(const juce::String& paramId, int cc, int midiCh);     // takes mapCS + notifies listeners
    void assignLocked(const juce::String& paramId, int cc, int midiCh); // map mutation only; caller holds mapCS, no notify
    static int key(int cc, int midiCh) { return cc * 16 + (midiCh - 1); }
};
