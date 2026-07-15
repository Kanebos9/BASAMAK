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
    static constexpr int NUM_PATTERNS = 32;   // max patterns; the UI shows a user-chosen subset (16/24/32) + scrolls

    enum PlayMode { LoopForever = 0, StopAfterN = 1, NextAfterN = 2, Chain = 3 };
    static constexpr int CHAIN_MAX = 8;   // up to 8 patterns in a per-pattern chain sequence

    // Master FX + Master Output are PATTERN-level: each pattern keeps its own,
    // so switching pattern (or loading a preset) shows that pattern's settings.
    struct MasterFX
    {
        float reverbRoom = 0.5f, reverbDamp = 0.5f, reverbWet = 0.5f;   // wet default 0.4 -> 0.5 [2026-07-15]
        float delayTime  = 0.375f, delayFeedback = 0.3f;
        float delayWet   = 0.5f;        // delay return level (default 0.3 -> 0.5 [2026-07-15], user)
        bool  delaySync  = false;
        int   delayDivision = 4;        // RETIRED index into the old note-division table (kept for old-file migration)
        bool  delayPingPong = false;    // cross-feed L<->R so echoes bounce across the stereo field
        int   delayMode = 0;            // [2026-07-15 00:50] loop character: 0 Tape (= the original
                                        // voicing, default) / 1 Digital / 2 Dub / 3 Analog / 4 Shimmer
        // [2026-07-15 02:30] the SYNC/TRAIL/DUCK/CHARACTER batch (user-designed):
        float delayBarN  = 8.0f;        // synced Time = one bar / N (echoes-per-bar; fader stops 1..21,
                                        // float so old beat-division projects migrate EXACTLY)
        int   delayTrail = 0;           // MAX TRAIL: 0 = unlimited (default = old behaviour); 1..21 =
                                        // hard echo-count cap (independent of Feedback - loud abrupt trails)
        float delayDuck  = 0.0f;        // DUCK: echoes tuck under the dry mix, bloom in the gaps (0 = off)
        float delayChar  = 0.5f;        // CHARACTER: depth of the mode's flavour; 0.5 = the original
                                        // constants = bit-identical (Tape darkening / Dub drive / Analog
                                        // wobble / Shimmer octave blend; Digital = inert)
        float reverbPreDelay = 0.0f;    // 0..1 -> 0..120 ms gap before the reverb tail (drums love this)
        int   reverbMode = 1;           // 0 Room / 1 Hall (= the original voicing, default) / 2 Plate / 3 Shimmer
        float reverbWidth    = 1.0f;    // reverb stereo width (0 = mono/narrow tail, 1 = full wide)
        // BUS B (v1.3.9): a SECOND shared reverb + delay - e.g. A = Hall for keys, B = tight Room
        // for drums. Channels pick their bus per channel (revBus/delBus, right-click the send fader).
        // Same param set as A; edited via the A/B selector in MASTER.
        float reverbRoomB = 0.5f, reverbDampB = 0.5f, reverbWetB = 0.5f;
        float reverbPreDelayB = 0.0f; int reverbModeB = 0;   // B defaults to ROOM (the drum bus)
        float reverbWidthB = 1.0f;
        float delayTimeB = 0.375f, delayFeedbackB = 0.3f, delayWetB = 0.5f;
        bool  delaySyncB = false; int delayDivisionB = 4; bool delayPingPongB = false;
        int   delayModeB = 0;
        float delayBarNB = 8.0f; int delayTrailB = 0; float delayDuckB = 0.0f; float delayCharB = 0.5f;
        // [2026-07-15 02:30] REVERB sync + gate (user-designed): Sync ON = the Decay fader is a
        // musical TAIL LENGTH in bars (feedback computed live from size/mode/tempo) and Pre is a
        // bar fraction. GATE = the 80s gated-verb chop: the wet return cuts dead a bar-fraction
        // after each hit (0 = off; loudness stays decoupled from length, like the delay Trail).
        bool  reverbSync = false;  float reverbDecBars = 1.0f;  float reverbPreBars = 0.03125f;
        float reverbGate = 0.0f;   // 0 = off, else the gate time as a fraction of a bar (1/32..1)
        bool  reverbSyncB = false; float reverbDecBarsB = 1.0f; float reverbPreBarsB = 0.03125f;
        float reverbGateB = 0.0f;
        float volume = 0.9f;
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
        // MERGE: this pattern is glued onto the PREVIOUS one (shift+click its button). A run of merged
        // patterns = ONE multi-bar unit: bars play head..end in sequence, the HEAD's play mode governs
        // what happens after the last bar, and every member mirrors the head's channel SOUNDS (the
        // editor keeps them in sync - one sound editor, no clashing). Steps stay per bar.
        bool  mergeWithPrev = false;
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
    double blockBarSeconds = 2.0; // seconds per bar this block (for tempo-synced per-slot LFOs; DAW overrides)
    int    uiGridDiv       = 16;  // piano-roll Grid 1/N (the editor sets it) - for LFO/arp "Lock to grid"
    float  modWheel        = 0.0f;// live MIDI mod wheel (CC1) 0..1 (set by the processor; a shared mod source, forwarded to channels)
    float pitchWheel = 0.0f;   // [2026-07-14 03:00] live pitch wheel -1..+1 (mod source; forwarded per block like modWheel)

    // Raised when the sequencer auto-changes the current pattern (next/stop),
    // so the editor can re-sync its UI.
    std::atomic<bool> patternChanged { false };

    // offset = SAMPLE-ACCURATE position of the hit within this block (at the engine's rate).
    // The render is split at these offsets so triggers land exactly on the grid instead of
    // being quantised to block starts (which jittered up to a whole buffer, ~12 ms at 512).
    struct TriggerEvent { int channel; int step; float velScale = 1.0f; int sub = 0; int roll = 1; int offset = 0;
                          long gate = 0;      // gate > 0 = cut the hit after this many samples (per-step Length)
                          long slideLen = 0;      // slide glide time in samples (0 = step has no slide)
                          float slideTo = 0.0f;   // slide TARGET pitch (the NEXT active step's pitch, semitones)
                          bool  isDraw = false;   // PIANO-ROLL note: use drawPitch + per-note drawVel + channel drawPan
                          float drawPitch = 0.0f;
                          float drawVel = 1.0f;      // per-note velocity (0..1)
                          int   drawSlot = 0;        // per-note slot tag (0 = both, 1 = slot 1, 2 = slot 2)
                          bool  drawOverlap = false;   // note starts while another sounds (chord) -> don't cut it
                          float drawGlideFrom = -999.0f;   // MONO glide: slide this note FROM this pitch (semis); -999 = no glide
                          bool  drawOneShot = false;       // ONE-SHOT note: instant trigger, natural ring (no gate) - like a bare step
                          bool  drawStrumUp = false;       // per-note STRUM direction (true = up / alt. strum)
                          int   drawStrumPct = -1;         // per-note STRUM amount override (0..100; -1 = Strum knob)
                          float drawNotePan = 0.0f; };     // per-note PAN (-1..+1); overrides the whole-channel drawPan

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

    // "THIS PATTERN ONLY" recording: while set, playback LOOPS the armed pattern (or its merged
    // group) - chains / stop-after / next-after are ignored until recording stops. Without it the
    // chain pulled playback away from the pattern being recorded (user report).
    std::atomic<bool> recordLoopLock { false };

    // While RECORDING keys into a piano-roll channel, its sequenced notes must NOT fire - the live
    // key voices are the monitor (double-triggering re-fired the half-grown note at bar starts and
    // MONO-cut the held voice = blips/cuts). Set per block by the processor; -1 = nothing suppressed.
    std::atomic<int> recordSuppressCh { -1 };
    std::atomic<int> recordSuppressCh2 { -1 };   // the merged PARTNER channel while recording a pair

    // MERGED-GROUP helpers: a group = a head pattern + the following run of mergeWithPrev patterns.
    int groupHead(int p) const { p = juce::jlimit(0, NUM_PATTERNS - 1, p);
                                 while (p > 0 && patterns[p].mergeWithPrev) --p; return p; }
    int groupEnd(int p) const  { p = groupHead(p);
                                 while (p + 1 < NUM_PATTERNS && patterns[p + 1].mergeWithPrev) ++p; return p; }
    bool inGroup(int p) const  { return groupEnd(p) > groupHead(p); }

    juce::Array<TriggerEvent> processBlock(
        juce::AudioBuffer<float>& audio,        // the Main output bus
        double sampleRate,
        int numSamples,
        juce::AudioPlayHead* playHead,
        juce::AudioBuffer<float>* const* auxBuses = nullptr,  // per-aux-out views (nullptr = disabled)
        int numAux = 0,
        juce::AudioBuffer<float>* reverbSendBus  = nullptr,   // per-channel reverb-send sum, bus A (Main-routed only)
        juce::AudioBuffer<float>* delaySendBus   = nullptr,   // per-channel delay-send sum, bus A
        juce::AudioBuffer<float>* reverbSendBusB = nullptr,   // bus B sums (a channel picks its bus via revBus/delBus)
        juce::AudioBuffer<float>* delaySendBusB  = nullptr);

    void reset();
    void resetChains()           { for (auto& p : patterns) p.chainStep = 0; }   // chain positions back to the start
    void startStandalone()       { playing = true; finished = false; patternRepeatCount = 0; loopCount = 0;
                                   resetChains();
                                   // Play from the viewed pattern - unless playPattern was parked on a BAR of the
                                   // viewed merged group (the user clicked a middle bar: start THERE, run on).
                                   if (groupHead(playPattern) != groupHead(currentPattern)) playPattern = currentPattern;
                                   resetTickDedupe(); }
    void stopStandalone()        { playing = false; barPosition = 0.0; finished = false;
                                   patternRepeatCount = 0; loopCount = 0; resetChains(); isCurrentlyPlaying = false;
                                   playPattern = groupHead(playPattern);   // stopping mid-group parks at the HEAD, so the
                                                                           // next Play starts the group from its beginning
                                                                           // (an explicit middle-bar click AFTER stopping
                                                                           // still re-parks it there)
                                   resetTickDedupe(); }
    void setStandaloneBpm(float bpm) { standaloneBpm = bpm; }

    int getChannelStep(int ch) const
    {
        if (!isCurrentlyPlaying || currentPattern != playPattern) return -1;  // only show the cursor on the playing pattern
        int n = patterns[playPattern].channels[ch].numSteps;
        if (n <= 0) return -1;
        return (int)(barPosition * n) % n;
    }

    bool isCurrentlyPlaying = false;
    // Playhead position within the current bar, 0..1 (KEYS recording quantises key presses
    // to the nearest step from this).
    double barPos() const { return barPosition; }

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
                              double samplesPerBar,
                              juce::Array<TriggerEvent>& events);
};
