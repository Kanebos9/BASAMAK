#include "Sequencer.h"

void Sequencer::reset()
{
    barPosition = 0.0;
    playing     = false;
    isCurrentlyPlaying = false;
    finished = false;
    patternRepeatCount = 0;
    loopCount = 0;
    wasPlaying = false;
    resetStepTracking();
}

void Sequencer::resetStepTracking()
{
    for (int i = 0; i < NUM_CHANNELS; ++i)
        lastStep[i] = -1;
}

//==============================================================================
juce::Array<Sequencer::TriggerEvent> Sequencer::processBlock(
    juce::AudioBuffer<float>& audio,
    double sampleRate,
    int numSamples,
    juce::AudioPlayHead* playHead,
    bool anySolo,
    juce::AudioBuffer<float>* const* auxBuses,
    int numAux,
    juce::AudioBuffer<float>* reverbSendBus,
    juce::AudioBuffer<float>* delaySendBus)
{
    juce::Array<TriggerEvent> events;

    if (dawSync)
        advanceDaw(playHead, sampleRate, numSamples, events);
    else if (playing)
        advanceStandalone(sampleRate, numSamples, events);

    for (auto& e : events)
    {
        auto& c = patterns[playPattern].channels[e.channel];   // steps fire from the PLAYING pattern
        if (c.midiOut) continue;   // MIDI-out channels make no internal sound (they emit notes in the processor)
        // Choke groups: a hit cuts the ringing tails of other channels in the same group (e.g. a
        // closed hi-hat silencing an open one). The new hit masks the cut, so no audible click.
        if (c.chokeGroup > 0)
            for (int o = 0; o < NUM_CHANNELS; ++o)
                if (o != e.channel && patterns[playPattern].channels[o].chokeGroup == c.chokeGroup)
                    patterns[playPattern].channels[o].silenceAllVoices();
        c.trigger(c.stepVel[e.step] * e.velScale, c.stepPitch[e.step], c.stepPan[e.step]);   // velScale = roll-decay ramp
    }

    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
    {
        auto& chan = patterns[playPattern].channels[ch];
        const int ob = chan.outputBus;   // 0 = Main; 1..numAux = a discrete aux out
        const bool toMain = ! (ob >= 1 && ob <= numAux && auxBuses != nullptr && auxBuses[ob - 1] != nullptr);
        juce::AudioBuffer<float>* dest = toMain ? &audio : auxBuses[ob - 1];
        // Sends only apply to Main-routed channels (aux outs are dry, on their own DAW track).
        chan.renderInto(*dest, 0, numSamples, anySolo,
                        toMain ? reverbSendBus : nullptr,
                        toMain ? delaySendBus  : nullptr);
    }
    // A pattern we just switched AWAY from (NextAfterN) keeps rendering its still-ringing voices
    // (tails) into Main until they finish, so the switch doesn't hard-cut them = no click. No new
    // triggers fire on it (it's not playPattern), so the voices just decay out.
    if (fadeOutPattern >= 0 && fadeOutPattern != playPattern && fadeOutPattern != currentPattern)
    {
        bool anyRinging = false;
        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        {
            auto& fc = patterns[fadeOutPattern].channels[ch];
            if (! fc.midiOut) fc.renderInto(audio, 0, numSamples, anySolo);   // dry tail into Main
            if (fc.anyVoiceActive()) anyRinging = true;
        }
        if (! anyRinging) fadeOutPattern = -1;   // all tails finished
    }

    // Also render the VIEWED pattern when it differs, so auditioning a channel there
    // is still heard (always to Main, so it's audible regardless of routing).
    if (currentPattern != playPattern)
        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
            patterns[currentPattern].channels[ch].renderInto(audio, 0, numSamples, anySolo);

    return events;
}

//==============================================================================
// Standalone timing: advance barPosition at BPM rate (4/4 assumed)
void Sequencer::advanceStandalone(double sampleRate, int numSamples,
                                   juce::Array<TriggerEvent>& events)
{
    if (finished) { isCurrentlyPlaying = false; playing = false; return; }

    // Bar length honours the time signature: quarters per bar = num * 4/den.
    const double quartersPerBar = juce::jmax(1, timeSigNum) * 4.0 / juce::jmax(1, timeSigDen);
    double barsPerSample = (standaloneBpm / (60.0 * sampleRate)) / quartersPerBar;
    double oldPos = barPosition;
    double newPos = oldPos + numSamples * barsPerSample;
    isCurrentlyPlaying = true;

    if (newPos < 1.0)
    {
        checkChannelTriggers(oldPos, newPos, events);
        barPosition = newPos;
        return;
    }

    // Bar finished — decide stop/next BEFORE firing the next bar's first step
    onBarComplete();
    if (finished) { playing = false; barPosition = 0.0; isCurrentlyPlaying = false; return; }

    double rem = newPos - 1.0;
    resetStepTracking();
    checkChannelTriggers(0.0, rem, events);  // first step of the new (looped/next) bar
    barPosition = rem;
}

//==============================================================================
// DAW sync: derive barPosition from AudioPlayHead PPQ position
void Sequencer::advanceDaw(juce::AudioPlayHead* dawHead, double sampleRate, int numSamples,
                            juce::Array<TriggerEvent>& events)
{
    if (!dawHead) { isCurrentlyPlaying = false; return; }

    juce::AudioPlayHead::CurrentPositionInfo pos;
    if (!dawHead->getCurrentPosition(pos))
    {
        isCurrentlyPlaying = false;
        return;
    }

    if (!pos.isPlaying && !pos.isRecording)
    {
        isCurrentlyPlaying = false;
        resetStepTracking();
        barPosition = 0.0;
        finished = false;            // transport stopped → re-arm
        patternRepeatCount = 0;
        loopCount = 0;
        wasPlaying = false;
        return;
    }

    // Transport just (re)started → re-arm the play-mode counters + start from the viewed pattern
    if (!wasPlaying) { finished = false; patternRepeatCount = 0; loopCount = 0; resetChains(); wasPlaying = true;
                       playPattern = currentPattern; resetStepTracking(); }

    if (finished) { isCurrentlyPlaying = false; return; } // StopAfterN reached

    isCurrentlyPlaying = true;

    // bar position = frac(ppq / quartersPerBar), using the DAW's time signature.
    const int    num = pos.timeSigNumerator   > 0 ? pos.timeSigNumerator   : 4;
    const int    den = pos.timeSigDenominator > 0 ? pos.timeSigDenominator : 4;
    const double quartersPerBar = num * 4.0 / den;

    double oldPos = std::fmod(pos.ppqPosition / quartersPerBar, 1.0);

    // Advance by one block (ppq is in quarter notes)
    double beatsPerSample = pos.bpm / (60.0 * sampleRate);
    double newPos = oldPos + numSamples * beatsPerSample / quartersPerBar;

    if (newPos < 1.0)
    {
        checkChannelTriggers(oldPos, newPos, events);
        barPosition = newPos;
        return;
    }

    onBarComplete();
    if (finished) { isCurrentlyPlaying = false; return; }

    double rem = newPos - 1.0;
    resetStepTracking();
    checkChannelTriggers(0.0, rem, events);
    barPosition = rem;
}

//==============================================================================
// Apply the current pattern's play mode after a bar finishes.
void Sequencer::onBarComplete()
{
    ++patternRepeatCount;
    ++loopCount;                  // one more pattern playthrough (for per-step loop conditions)
    auto& p = patterns[playPattern];
    const int target = juce::jmax(1, p.repeatTarget);

    if (p.playMode == StopAfterN && patternRepeatCount >= target)
    {
        finished = true;
    }
    else if (p.playMode == NextAfterN && patternRepeatCount >= target)
    {
        fadeOutPattern = playPattern;   // let the outgoing pattern's voices ring out (no hard-cut click)
        playPattern = juce::jlimit(0, NUM_PATTERNS - 1, p.gotoPattern);
        patternRepeatCount = 0;
        finished = false;
        resetStepTracking();
        patternChanged.store(true);   // editor follows playPattern if "Follow" is on
    }
    else if (p.playMode == Chain && p.chainLen > 0
             && patternRepeatCount >= juce::jmax(1, p.chainLoops[p.chainStep % p.chainLen]))
    {
        // Advance through this pattern's chain (cycling). Each entry has its OWN loop count (chainLoops): play this
        // pattern that many loops, then jump to chainSeq[step]. chainStep persists so each visit can differ.
        const int tgt = juce::jlimit(0, NUM_PATTERNS - 1, p.chainSeq[p.chainStep % p.chainLen]);
        p.chainStep = (p.chainStep + 1) % p.chainLen;
        fadeOutPattern = playPattern;
        playPattern = tgt;
        patternRepeatCount = 0;
        finished = false;
        resetStepTracking();
        patternChanged.store(true);
    }
}

//==============================================================================
// Fire the step we're now entering, for each channel. The caller guarantees
// newPos <= 1.0 and handles bar boundaries (so the next bar's first step is
// only fired when playback actually continues).
// Maps a bar position to the step we're inside, applying swing: each pair of
// steps splits not at the midpoint but later for the off-step, so odd steps are
// delayed for a groove feel. 'swing' 0 = straight, ~0.7 = heavy.
static int swungStep(double pos, int n, float swing)
{
    if (swing <= 0.0001f) return (int)(pos * n) % n;
    const double stepsPos = pos * (double) n;          // 0..n
    const int    pair     = (int) (stepsPos) / 2;      // which pair of steps
    const double within   = (stepsPos - pair * 2.0) * 0.5; // 0..1 across the pair
    const double boundary = 0.5 + (double) swing * 0.25;   // split point (0.5..0.75)
    int step = pair * 2 + (within < boundary ? 0 : 1);
    if (step >= n) step = n - 1;                        // lone last step (odd n) stays straight
    return step;
}

void Sequencer::checkChannelTriggers(double oldPos, double newPos,
                                      juce::Array<TriggerEvent>& events)
{
    Pattern& pp = patterns[playPattern];   // triggers come from the PLAYING pattern
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
    {
        int n = pp.channels[ch].numSteps;
        if (n <= 0) continue;

        // Which step do we land on now? (with this pattern's swing applied)
        int curStep = swungStep(newPos, n, pp.swing);
        auto& c = pp.channels[ch];

        // Ratchet/roll: subdivide the step into 'roll' evenly-spaced sub-hits. A
        // unique tick per (step, sub-hit) makes the step fire that many times.
        const int roll = juce::jlimit(1, 6, c.stepRoll[curStep]);
        const double posS = newPos * (double) n;
        const double within = posS - std::floor(posS);               // 0..1 within the step slot
        const int sub = juce::jlimit(0, roll - 1, (int)(within * roll));
        const long tick = (long) curStep * 8 + sub;

        // At the exact start of a bar (oldPos == 0.0), always fire step 0
        bool atBarStart = (oldPos == 0.0);

        if (tick != lastStep[ch] || atBarStart)
        {
            lastStep[ch] = tick;
            // Refresh the analysed channel's spectrum at every step/sub boundary so
            // the EQ graph updates. Only the inspected channel has a tap.
            if (auto* tap = c.analysisTap) tap->arm();
            // Per-step LOOP condition: fire only on the chosen loops of an N-loop cycle (N=1 OR no bars chosen = always).
            bool condOk = true;
            const int condN = juce::jlimit(1, 10, c.stepCondLen[curStep]);
            if (condN > 1 && c.stepCondMask[curStep] != 0) {
                const int bar = ((loopCount % condN) + condN) % condN;
                condOk = ((c.stepCondMask[curStep] >> bar) & 1) != 0;
            }
            if (c.steps[curStep] && condOk)
            {
                // Roll ramp: each successive sub-hit ramps in velocity across the ratchet. The amount
                // (stepRollDecay, -1..+1) is set by the Roll cell's X-drag: negative = fade out (each
                // hit quieter), 0 = flat, positive = build up (each hit louder).
                float velScale = 1.0f;
                if (roll > 1)
                {
                    const float rr   = juce::jlimit(-1.0f, 1.0f, c.stepRollDecay[curStep]);
                    const float frac = (float) sub / (float)(roll - 1);   // 0 = first hit, 1 = last
                    velScale = (rr >= 0.0f) ? (1.0f - rr) + rr * frac      // build up
                                            : 1.0f + rr * frac;           // fade out (rr < 0)
                    velScale = juce::jmax(0.0f, velScale);
                }
                events.add({ ch, curStep, velScale, sub, roll });
            }
        }
    }
}
