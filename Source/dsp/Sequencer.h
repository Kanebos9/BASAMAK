#pragma once
#include <JuceHeader.h>
#include "DrumChannel.h"

//==============================================================================
// Timing is tracked as a fractional position within a bar (0..1).
// The sequencer holds NUM_PATTERNS independent patterns; only the "current"
// one is rendered. Each pattern has a play mode controlling what happens after
// it completes a number of bars: loop forever, stop, or jump to the next.
//==============================================================================
class Sequencer
{
public:
    static constexpr int NUM_CHANNELS = 16;   // max channels; the UI shows a user-chosen subset (4/8/12/16)
    static constexpr int LAUNCHPAD_CH = 8;    // the Launchpad is an 8x8 grid -> it only maps the first 8 channels
    static constexpr int NUM_PATTERNS = 32;   // max patterns; the UI shows a user-chosen subset (16/24/32) + scrolls

    enum PlayMode { LoopForever = 0, StopAfterN = 1, NextAfterN = 2, Chain = 3 };
    static constexpr int CHAIN_MAX = 8;   // up to 8 patterns in a per-pattern chain sequence

    // Master FX + Master Output are PATTERN-level: each pattern keeps its own,
    // so switching pattern (or loading a preset) shows that pattern's settings.
    struct MasterFX
    {
        float reverbRoom = 0.5f, reverbDamp = 0.5f, reverbWet = 0.4f;
        float delayTime  = 0.375f, delayFeedback = 0.3f;
        bool  delaySync  = false;
        int   delayDivision = 4;        // index into the note-division table
        bool  delayPingPong = false;    // cross-feed L<->R so echoes bounce across the stereo field
        float reverbPreDelay = 0.0f;    // 0..1 -> 0..120 ms gap before the reverb tail (drums love this)
        float reverbWidth    = 1.0f;    // reverb stereo width (0 = mono/narrow tail, 1 = full wide)
        float volume = 0.9f, pan = 0.0f;
        bool  mono   = false;
        float limit  = 0.003f;          // 0 = limiter off; default ~-0.1 dB ceiling (light/transparent)
        float glue   = 0.0f;            // 0 = off; master bus "glue" compressor amount (before the limiter)
    };

    struct Pattern
    {
        DrumChannel channels[NUM_CHANNELS];
        int   playMode     = LoopForever;
        int   repeatTarget = 2;   // N (used by StopAfterN / NextAfterN / Chain)
        int   gotoPattern  = 0;   // target for NextAfterN (0-based)
        // Chain mode: a list of (target pattern, loops). Play this pattern chainLoops[i] loops, then jump to
        // chainSeq[i], advancing through the list (cycling) - so each visit can go somewhere different, each with
        // its own loop count. chainStep is runtime (reset on transport start).
        int   chainSeq[CHAIN_MAX]   = { -1,-1,-1,-1,-1,-1,-1,-1 };
        int   chainLoops[CHAIN_MAX] = {  2, 2, 2, 2, 2, 2, 2, 2 };
        int   chainLen = 0;
        int   chainStep = 0;
        float swing        = 0.0f; // 0 = straight .. ~0.7 delays the off-steps
        MasterFX master;          // per-pattern master FX + output
    };

    Pattern patterns[NUM_PATTERNS];
    int     currentPattern = 0;   // the VIEWED / edited pattern (what the editor shows)
    int     playPattern    = 0;   // the pattern the transport is actually playing (advances on its own)
    int     fadeOutPattern = -1;  // a just-switched-away pattern still ringing its tails out (-1 = none)

    bool  dawSync        = false;  // off by default (use the plugin's own play/BPM)
    float standaloneBpm  = 120.0f;
    int   timeSigNum     = 4;     // standalone time signature (used when not DAW-synced)
    int   timeSigDen     = 4;
    bool  playing        = false; // standalone mode only

    // Raised when the sequencer auto-changes the current pattern (next/stop),
    // so the editor can re-sync its UI.
    std::atomic<bool> patternChanged { false };

    struct TriggerEvent { int channel; int step; float velScale = 1.0f; int sub = 0; int roll = 1; };

    //-- Accessors for the VIEWED pattern (what the editor edits/auditions).
    Pattern&       current()        { return patterns[currentPattern]; }
    const Pattern& current() const  { return patterns[currentPattern]; }
    DrumChannel&       channel(int i)       { return patterns[currentPattern].channels[i]; }
    const DrumChannel& channel(int i) const { return patterns[currentPattern].channels[i]; }
    //-- Accessor for the PLAYING pattern (transport).
    Pattern&       playing_()       { return patterns[playPattern]; }

    // Set the VIEWED pattern. While stopped the view is also where playback will start,
    // so it moves playPattern too; while PLAYING it only changes the view (playback
    // continues on its own pattern - clicking a pattern never hijacks playback).
    void setCurrentPattern(int p)
    {
        if (p < 0 || p >= NUM_PATTERNS) return;
        currentPattern = p;
        if (! isCurrentlyPlaying)
        {
            playPattern = p;
            patternRepeatCount = 0;
            finished = false;
            resetStepTracking();
        }
    }

    juce::Array<TriggerEvent> processBlock(
        juce::AudioBuffer<float>& audio,        // the Main output bus
        double sampleRate,
        int numSamples,
        juce::AudioPlayHead* playHead,
        bool anySolo,
        juce::AudioBuffer<float>* const* auxBuses = nullptr,  // per-aux-out views (nullptr = disabled)
        int numAux = 0,
        juce::AudioBuffer<float>* reverbSendBus = nullptr,    // per-channel reverb-send sum (Main-routed only)
        juce::AudioBuffer<float>* delaySendBus  = nullptr);   // per-channel delay-send sum

    void reset();
    void resetChains()           { for (auto& p : patterns) p.chainStep = 0; }   // chain positions back to the start
    void startStandalone()       { playing = true; finished = false; patternRepeatCount = 0; loopCount = 0;
                                   resetChains(); playPattern = currentPattern; resetStepTracking(); }   // play from the viewed pattern
    void stopStandalone()        { playing = false; barPosition = 0.0; finished = false;
                                   patternRepeatCount = 0; loopCount = 0; resetChains(); isCurrentlyPlaying = false; resetStepTracking(); }
    void setStandaloneBpm(float bpm) { standaloneBpm = bpm; }

    int getChannelStep(int ch) const
    {
        if (!isCurrentlyPlaying || currentPattern != playPattern) return -1;  // only show the cursor on the playing pattern
        int n = patterns[playPattern].channels[ch].numSteps;
        if (n <= 0) return -1;
        return (int)(barPosition * n) % n;
    }

    bool isCurrentlyPlaying = false;

private:
    double barPosition = 0.0;      // 0..1 fraction within current bar
    int    lastStep[NUM_CHANNELS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
    juce::Random stepRng;          // per-step probability rolls

    int    patternRepeatCount = 0; // bars completed in current pattern
public:
    int    loopCount = 0;          // pattern playthroughs since playback started (per-step loop conditions)
private:
    bool   finished = false;       // StopAfterN reached → suppress triggers
    bool   wasPlaying = false;     // DAW transport edge detection

    void resetStepTracking();
    void onBarComplete();          // applies the current pattern's play mode
    void advanceStandalone(double sampleRate, int numSamples, juce::Array<TriggerEvent>& events);
    void advanceDaw(juce::AudioPlayHead* playHead, double sampleRate, int numSamples,
                    juce::Array<TriggerEvent>& events);
    void checkChannelTriggers(double oldPos, double newPos, juce::Array<TriggerEvent>& events);
};
