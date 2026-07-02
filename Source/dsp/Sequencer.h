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
        float delayWet   = 0.3f;        // delay return level (was a fixed hidden 0.3; now a MASTER knob like reverb Wet)
        bool  delaySync  = false;
        int   delayDivision = 4;        // index into the note-division table
        bool  delayPingPong = false;    // cross-feed L<->R so echoes bounce across the stereo field
        float reverbPreDelay = 0.0f;    // 0..1 -> 0..120 ms gap before the reverb tail (drums love this)
        float reverbWidth    = 1.0f;    // reverb stereo width (0 = mono/narrow tail, 1 = full wide)
        float volume = 0.9f, pan = 0.0f;
        bool  mono   = false;
        float limit  = 0.003f;          // 0 = limiter off; default ~-0.1 dB ceiling (light/transparent)
        float glue   = 0.0f;            // 0 = off; master bus "glue" compressor amount (before the limiter)
        // Master TONE + colour (drum + bass bus). Signal order: Tilt -> Sat -> Glue -> Limiter.
        float tilt   = 0.5f;            // one-knob tilt EQ around ~700 Hz: 0 = dark (bass up/treble down),
                                        // 0.5 = flat (bit-identical), 1 = bright (treble up/bass down). +/-6 dB.
        float sat    = 0.0f;            // 0 = off (bit-identical); master saturation (tanh warmth/drive), 0..1.
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
        float swing        = 0.0f; // 0 = straight .. 1 = max (MPC 50%..75%: off-step at 0.5+swing*0.25 of the pair)
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

    // offset = SAMPLE-ACCURATE position of the hit within this block (at the engine's rate).
    // The render is split at these offsets so triggers land exactly on the grid instead of
    // being quantised to block starts (which jittered up to a whole buffer, ~12 ms at 512).
    struct TriggerEvent { int channel; int step; float velScale = 1.0f; int sub = 0; int roll = 1; int offset = 0;
                          long gate = 0;      // gate > 0 = cut the hit after this many samples (per-step Length)
                          long slideLen = 0;      // slide glide time in samples (0 = step has no slide)
                          float slideTo = 0.0f; };// slide TARGET pitch (the NEXT active step's pitch, semitones)

    // [start, end) of step `s` (bar fraction 0..1) with this pattern's swing applied. The
    // MIDI exporter reuses it so exported clips carry the same groove the engine plays.
    static void stepSpan(int s, int n, float swing, double& start, double& end)
    {
        const double stepW = 1.0 / juce::jmax(1, n);
        if (swing <= 0.0001f || (n % 2 != 0 && s >= n - 1))   // straight, or the lone last step of an odd count
        { start = s * stepW; end = start + stepW; return; }
        const int    pair      = s / 2;
        const double pairStart = pair * 2.0 * stepW;
        const double boundary  = pairStart + (0.5 + (double) swing * 0.25) * 2.0 * stepW;  // swung split point
        if ((s & 1) == 0) { start = pairStart; end = boundary; }
        else              { start = boundary;  end = pairStart + 2.0 * stepW; }
    }

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
        }
    }

    // Solo state is PER PATTERN, so each rendered pattern must be muted against ITS OWN
    // solo flags (using the viewed pattern's solo for the playing pattern silenced
    // everything when view != playback - the old anySolo-parameter bug).
    static bool anySoloIn(const Pattern& p)
    { for (auto& c : p.channels) if (c.solo) return true; return false; }

    juce::Array<TriggerEvent> processBlock(
        juce::AudioBuffer<float>& audio,        // the Main output bus
        double sampleRate,
        int numSamples,
        juce::AudioPlayHead* playHead,
        juce::AudioBuffer<float>* const* auxBuses = nullptr,  // per-aux-out views (nullptr = disabled)
        int numAux = 0,
        juce::AudioBuffer<float>* reverbSendBus = nullptr,    // per-channel reverb-send sum (Main-routed only)
        juce::AudioBuffer<float>* delaySendBus  = nullptr);   // per-channel delay-send sum

    void reset();
    void resetChains()           { for (auto& p : patterns) p.chainStep = 0; }   // chain positions back to the start
    void startStandalone()       { playing = true; finished = false; patternRepeatCount = 0; loopCount = 0;
                                   resetChains(); playPattern = currentPattern; resetTickDedupe(); }   // play from the viewed pattern
    void stopStandalone()        { playing = false; barPosition = 0.0; finished = false;
                                   patternRepeatCount = 0; loopCount = 0; resetChains(); isCurrentlyPlaying = false; resetTickDedupe(); }
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
    juce::Random stepRng;          // per-step probability rolls
    // Seam dedupe: the DAW path recomputes oldPos from the host ppq each block; a floating-point
    // mismatch at the block seam could re-cross (double-fire) a tick. Remember the last tick id
    // + the loop it fired on per channel and skip exact repeats.
    int lastTick[NUM_CHANNELS]     = {};
    int lastTickLoop[NUM_CHANNELS] = {};
    void resetTickDedupe() { for (int i = 0; i < NUM_CHANNELS; ++i) { lastTick[i] = -1; lastTickLoop[i] = -1; } }

    int    patternRepeatCount = 0; // bars completed in current pattern
public:
    int    loopCount = 0;          // pattern playthroughs since playback started (per-step loop conditions)
private:
    bool   finished = false;       // StopAfterN reached → suppress triggers
    bool   wasPlaying = false;     // DAW transport edge detection

    void onBarComplete();          // applies the current pattern's play mode
    void advanceStandalone(double sampleRate, int numSamples, juce::Array<TriggerEvent>& events);
    void advanceDaw(juce::AudioPlayHead* playHead, double sampleRate, int numSamples,
                    juce::Array<TriggerEvent>& events);
    // Scan every step/sub-hit boundary in (oldPos, newPos] (bar fractions) and fire the ones
    // that are on, with a sample-accurate block offset = baseOffset + position within the span.
    // Scanning the RANGE (not "which step are we in now") also fires steps a huge host buffer
    // would previously have skipped, and puts ratchet sub-hits inside the SWUNG step span.
    void checkChannelTriggers(double oldPos, double newPos, int spanSamples, int baseOffset,
                              juce::Array<TriggerEvent>& events);
};
