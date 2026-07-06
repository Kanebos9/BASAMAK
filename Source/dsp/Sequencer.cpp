#include "Sequencer.h"
#include <algorithm>

void Sequencer::reset()
{
    barPosition = 0.0;
    playing     = false;
    isCurrentlyPlaying = false;
    finished = false;
    patternRepeatCount = 0;
    loopCount = 0;
    wasPlaying = false;
    resetTickDedupe();
}

//==============================================================================
juce::Array<Sequencer::TriggerEvent> Sequencer::processBlock(
    juce::AudioBuffer<float>& audio,
    double sampleRate,
    int numSamples,
    juce::AudioPlayHead* playHead,
    juce::AudioBuffer<float>* const* auxBuses,
    int numAux,
    juce::AudioBuffer<float>* reverbSendBus,
    juce::AudioBuffer<float>* delaySendBus)
{
    juce::Array<TriggerEvent> events;
    if (numSamples <= 0) return events;

    if (dawSync)
        advanceDaw(playHead, sampleRate, numSamples, events);
    else if (playing)
        advanceStandalone(sampleRate, numSamples, events);

    // Events now carry a sample-accurate block offset. Sort by offset, then render the
    // PLAYING pattern in segments split at those offsets so each hit starts exactly on
    // the grid (previously everything was quantised to the block start = up to a whole
    // buffer of jitter).
    std::sort(events.begin(), events.end(),
              [](const TriggerEvent& a, const TriggerEvent& b) { return a.offset < b.offset; });

    auto fireEvent = [this, sampleRate](const TriggerEvent& e)
    {
        auto& c = patterns[playPattern].channels[e.channel];   // steps fire from the PLAYING pattern
        // SIDECHAIN DUCK: this hit pushes down every channel set to "Duck by" this channel
        // (Routing popup). Level-only - the ducked sound recovers; nothing is cut like choke.
        for (int o = 0; o < NUM_CHANNELS; ++o)
        {
            auto& d = patterns[playPattern].channels[o];
            if (o != e.channel && d.duckBy == e.channel && d.duckAmt > 0.001f) d.duckPulse();
        }
        if (c.midiOut) return;   // MIDI-out channels make no internal sound (they emit notes in the processor)
        if (e.isDraw) { c.trigger(e.drawVel, e.drawPitch, c.drawPan, e.gate, 0.0f, 0,
                                  e.drawOverlap); return; }   // PIANO-ROLL note (chord tones overlap, melody cuts)
        // Choke groups: a hit FADES OUT (~3 ms) the ringing tails of other channels in the same
        // group (e.g. a closed hi-hat silencing an open one). A hard cut clicked whenever the
        // choking hit was quieter than the tail it cut.
        if (c.chokeGroup > 0)
            for (int o = 0; o < NUM_CHANNELS; ++o)
                if (o != e.channel && patterns[playPattern].channels[o].chokeGroup == c.chokeGroup)
                    patterns[playPattern].channels[o].fadeOutVoices();
        // SLIDE = glide TOWARD THE NEXT STEP: the slid step plays its own attack at its own pitch,
        // then bends across the WHOLE step to land exactly on the next active step's pitch at the
        // boundary (which then takes over seamlessly). Needs different Pitch values on the steps -
        // equal pitches = nothing to glide. (Glide-FROM-previous and the authentic 303 tie were both
        // tried and rejected: from-previous was inaudible in normal use, the tie "skipped the step".)
        c.trigger(c.stepVel[e.step] * e.velScale, c.stepPitch[e.step], c.stepPan[e.step], e.gate,
                  e.slideLen > 0 ? e.slideTo : 0.0f, e.slideLen);   // velScale = roll ramp
    };

    // Each rendered pattern is muted against ITS OWN solo flags (solo is per pattern).
    const bool soloPlay = anySoloIn(patterns[playPattern]);

    int idx = 0, segStart = 0;
    while (segStart < numSamples)
    {
        while (idx < events.size() && events[idx].offset <= segStart) fireEvent(events[idx++]);
        int segEnd = numSamples;
        if (idx < events.size()) segEnd = juce::jlimit(segStart + 1, numSamples, events[idx].offset);

        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        {
            auto& chan = patterns[playPattern].channels[ch];
            const int ob = chan.outputBus;   // 0 = Main; 1..numAux = a discrete aux out
            const bool toMain = ! (ob >= 1 && ob <= numAux && auxBuses != nullptr && auxBuses[ob - 1] != nullptr);
            juce::AudioBuffer<float>* dest = toMain ? &audio : auxBuses[ob - 1];
            // Sends only apply to Main-routed channels (aux outs are dry, on their own DAW track).
            chan.renderInto(*dest, segStart, segEnd - segStart, soloPlay,
                            toMain ? reverbSendBus : nullptr,
                            toMain ? delaySendBus  : nullptr);
        }
        segStart = segEnd;
    }
    while (idx < events.size()) fireEvent(events[idx++]);   // safety: offsets are clamped < numSamples

    // A pattern we just switched AWAY from (NextAfterN) keeps rendering its still-ringing voices
    // (tails) into Main until they finish, so the switch doesn't hard-cut them = no click. No new
    // triggers fire on it (it's not playPattern), so the voices just decay out.
    if (fadeOutPattern >= 0 && fadeOutPattern != playPattern && fadeOutPattern != currentPattern)
    {
        const bool soloFade = anySoloIn(patterns[fadeOutPattern]);
        bool anyRinging = false;
        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        {
            auto& fc = patterns[fadeOutPattern].channels[ch];
            if (! fc.midiOut) fc.renderInto(audio, 0, numSamples, soloFade);   // dry tail into Main
            if (fc.anyVoiceActive()) anyRinging = true;
        }
        if (! anyRinging) fadeOutPattern = -1;   // all tails finished
    }

    // MERGED GROUP: while playing inside a group, EVERY member bar keeps rendering its ringing
    // voices - a note recorded/drawn across a bar line lives in the bar it STARTED in and must keep
    // sounding while later bars play (idle channels render nothing, so this is cheap).
    if (inGroup(playPattern))
    {
        const int gh = groupHead(playPattern), ge = groupEnd(playPattern);
        for (int p = gh; p <= ge; ++p)
        {
            if (p == playPattern || p == fadeOutPattern || p == currentPattern) continue;   // already rendered
            const bool soloG = anySoloIn(patterns[p]);
            for (int ch = 0; ch < NUM_CHANNELS; ++ch)
            {
                auto& gc = patterns[p].channels[ch];
                if (! gc.midiOut && gc.anyVoiceActive()) gc.renderInto(audio, 0, numSamples, soloG);
            }
        }
    }

    // Also render the VIEWED pattern when it differs, so auditioning a channel there
    // is still heard (always to Main, so it's audible regardless of routing).
    if (currentPattern != playPattern)
    {
        const bool soloView = anySoloIn(patterns[currentPattern]);
        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
            patterns[currentPattern].channels[ch].renderInto(audio, 0, numSamples, soloView);
    }

    return events;
}

//==============================================================================
// Standalone timing: advance barPosition at BPM rate, honouring the time signature.
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
        checkChannelTriggers(oldPos, newPos, numSamples, 0, events);
        barPosition = newPos;
        return;
    }

    // Ticks in the tail of this bar (oldPos -> 1.0), then decide stop/next BEFORE the new bar.
    const int tailSamples = juce::jlimit(0, numSamples,
                                         (int) std::floor((1.0 - oldPos) / juce::jmax(1.0e-12, barsPerSample)));
    checkChannelTriggers(oldPos, 1.0, juce::jmax(1, tailSamples), 0, events);

    onBarComplete();
    if (finished) { playing = false; barPosition = 0.0; isCurrentlyPlaying = false; return; }

    double rem = newPos - 1.0;
    while (rem >= 1.0) rem -= 1.0;   // pathological giant-buffer safety
    // rem == 0 -> the bar boundary is exactly the block end; the next block (oldPos == 0)
    // fires step 0 itself - calling here too would double-fire it.
    if (rem > 0.0)
        checkChannelTriggers(0.0, rem, juce::jmax(1, numSamples - tailSamples), tailSamples, events);
    barPosition = rem;
}

//==============================================================================
// DAW sync: derive barPosition from AudioPlayHead PPQ position
void Sequencer::advanceDaw(juce::AudioPlayHead* dawHead, double sampleRate, int numSamples,
                            juce::Array<TriggerEvent>& events)
{
    if (!dawHead) { isCurrentlyPlaying = false; return; }

    const auto posInfo = dawHead->getPosition();
    if (! posInfo.hasValue())
    {
        isCurrentlyPlaying = false;
        return;
    }

    if (! posInfo->getIsPlaying() && ! posInfo->getIsRecording())
    {
        isCurrentlyPlaying = false;
        barPosition = 0.0;
        finished = false;            // transport stopped → re-arm
        patternRepeatCount = 0;
        loopCount = 0;
        wasPlaying = false;
        resetTickDedupe();
        return;
    }

    // Transport just (re)started → re-arm the play-mode counters + start from the viewed pattern
    if (!wasPlaying) { finished = false; patternRepeatCount = 0; loopCount = 0; resetChains(); wasPlaying = true;
                       playPattern = currentPattern; resetTickDedupe(); }

    if (finished) { isCurrentlyPlaying = false; return; } // StopAfterN reached

    isCurrentlyPlaying = true;

    // bar position = frac(ppq / quartersPerBar), using the DAW's time signature.
    const auto ts  = posInfo->getTimeSignature();
    const int  num = (ts.hasValue() && ts->numerator   > 0) ? ts->numerator   : 4;
    const int  den = (ts.hasValue() && ts->denominator > 0) ? ts->denominator : 4;
    const double quartersPerBar = num * 4.0 / den;

    const auto ppq = posInfo->getPpqPosition();
    const auto bpm = posInfo->getBpm();
    const double ppqPos  = ppq.hasValue() ? *ppq : 0.0;
    const double hostBpm = (bpm.hasValue() && *bpm > 1.0) ? *bpm : 120.0;

    double oldPos = std::fmod(ppqPos / quartersPerBar, 1.0);
    if (oldPos < 0.0) oldPos += 1.0;

    // Advance by one block (ppq is in quarter notes)
    double beatsPerSample = hostBpm / (60.0 * sampleRate);
    double barsPerSample  = beatsPerSample / quartersPerBar;
    double newPos = oldPos + numSamples * barsPerSample;

    if (newPos < 1.0)
    {
        checkChannelTriggers(oldPos, newPos, numSamples, 0, events);
        barPosition = newPos;
        return;
    }

    const int tailSamples = juce::jlimit(0, numSamples,
                                         (int) std::floor((1.0 - oldPos) / juce::jmax(1.0e-12, barsPerSample)));
    checkChannelTriggers(oldPos, 1.0, juce::jmax(1, tailSamples), 0, events);

    onBarComplete();
    if (finished) { isCurrentlyPlaying = false; return; }

    double rem = newPos - 1.0;
    while (rem >= 1.0) rem -= 1.0;
    if (rem > 0.0)   // rem == 0 -> next block's oldPos == 0 fires step 0 (no double-fire)
        checkChannelTriggers(0.0, rem, juce::jmax(1, numSamples - tailSamples), tailSamples, events);
    barPosition = rem;
}

//==============================================================================
// Apply the current pattern's play mode after a bar finishes.
void Sequencer::onBarComplete()
{
    ++loopCount;                  // one more bar (for per-step loop conditions)
    // MERGED GROUP: bars run head..end in sequence as ONE unit. Mid-group -> just move to the next
    // bar (a group PASS counts as one playthrough, at the end). At the last bar, the HEAD's play
    // mode decides (loop -> back to the head; StopAfterN/NextAfterN/Chain count group passes).
    const int gHead = groupHead(playPattern), gEnd = groupEnd(playPattern);
    if (gEnd > gHead && playPattern < gEnd)
    {
        fadeOutPattern = playPattern;
        playPattern = playPattern + 1;
        finished = false;
        patternChanged.store(true);
        return;
    }
    ++patternRepeatCount;
    auto& p = patterns[gHead];    // group: the HEAD's mode governs (single pattern: gHead == playPattern)
    const int target = juce::jmax(1, p.repeatTarget);
    if (gEnd > gHead && p.playMode == LoopForever)
    {   // loop the whole group: last bar -> back to the head
        fadeOutPattern = playPattern;
        playPattern = gHead;
        finished = false;
        patternChanged.store(true);
        return;
    }

    if (p.playMode == StopAfterN && patternRepeatCount >= target)
    {
        finished = true;
    }
    else if (p.playMode == NextAfterN && patternRepeatCount >= target)
    {
        fadeOutPattern = playPattern;   // let the outgoing pattern's voices ring out (no hard-cut click)
        // Jump to the EXACT target bar - even a middle bar of a merged group: playback starts there
        // and runs on through the rest of the group (user rule; do NOT snap to the group head).
        playPattern = juce::jlimit(0, NUM_PATTERNS - 1, p.gotoPattern);
        patternRepeatCount = 0;
        finished = false;
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
        playPattern = tgt;   // exact target bar - a middle group bar starts there and runs on (user rule)
        patternRepeatCount = 0;
        finished = false;
        patternChanged.store(true);
    }
    else if (gEnd > gHead)
    {
        // Group in StopAfterN / NextAfterN / Chain with N not reached yet: run the group again.
        fadeOutPattern = playPattern;
        playPattern = gHead;
        finished = false;
        patternChanged.store(true);
    }
}

//==============================================================================
// Scan every step / ratchet-sub-hit boundary in (oldPos, newPos] and fire the ones that
// are on. Each fired event gets a sample-accurate offset within the block:
// baseOffset + (its position within the scanned span) * spanSamples.
//   - Scanning the RANGE means a huge host buffer that crosses several boundaries fires
//     them ALL (the old "which step are we in now" check silently skipped steps).
//   - Sub-hits subdivide the SWUNG step span, so ratchets inside a swung step groove too.
//   - The exact bar start (position 0) fires only when the scan starts at 0.
void Sequencer::checkChannelTriggers(double oldPos, double newPos, int spanSamples, int baseOffset,
                                      juce::Array<TriggerEvent>& events)
{
    if (newPos < oldPos) return;
    const double span = juce::jmax(1.0e-12, newPos - oldPos);
    Pattern& pp = patterns[playPattern];   // triggers come from the PLAYING pattern

    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
    {
        auto& c = pp.channels[ch];

        // PIANO ROLL: a polyphonic NOTE LIST. Trigger every note whose start column is crossed this
        // block, gated for its own length. OVERLAP-AWARE: a note starting while another is still
        // sounding (a chord, or a later chord tone) triggers with forceOverlap so it doesn't cut it;
        // a plain sequential melody keeps the old mono-cut feel exactly.
        if (c.drawMode)
        {
            // Channel being RECORDED: its notes don't fire (the live keys are the monitor).
            if (ch == recordSuppressCh.load(std::memory_order_relaxed)) continue;
            const int R = DrumChannel::DRAW_RES;
            const int nN = juce::jlimit(0, DrumChannel::DRAW_MAX_NOTES, c.drawNoteCount);
            // Seam dedupe: compare against the PREVIOUS pass's value and set AFTER the loop, so a
            // chord (several notes at the SAME start col, fired in one pass) is never self-deduped.
            const int prevTick = lastTick[ch]; const int prevTickLoop = lastTickLoop[ch];
            int firedCol = -1;
            for (int ni = 0; ni < nN; ++ni)
            {
                const auto nt = c.drawNotes[ni];                      // copy (editor may edit concurrently)
                const double colPos = (double) nt.start / (double) R; // bar-fraction of the note's start
                const bool atZero = (oldPos == 0.0 && colPos == 0.0);
                if (! atZero && ! (colPos > oldPos && colPos <= newPos)) continue;
                if (prevTick == 100000 + (int) nt.start && prevTickLoop == loopCount) continue;   // seam re-crossing
                firedCol = nt.start;
                if (auto* tap = c.analysisTap) tap->arm();
                // overlap = ANY other note is sounding at this note's start (equal starts: earlier
                // list entries count as already sounding, so chord tone #2+ overlap tone #1)
                bool overlap = false;
                for (int mj = 0; mj < nN && ! overlap; ++mj)
                {
                    if (mj == ni) continue;
                    const auto& m = c.drawNotes[mj];
                    if (m.start < nt.start && nt.start < m.start + m.len) overlap = true;
                    else if (m.start == nt.start && mj < ni) overlap = true;
                }
                const double segBars = (double) nt.len / (double) R;
                const long gate = (long) juce::jmax(64.0, segBars / span * (double) spanSamples);
                const int off = baseOffset + (int) juce::jlimit(0.0, (double) spanSamples - 1.0,
                                                (colPos - oldPos) / span * (double) spanSamples);
                TriggerEvent e; e.channel = ch; e.step = 0; e.offset = off; e.gate = gate;
                e.isDraw = true; e.drawPitch = (float) nt.semi;
                e.drawVel = (float) nt.vel / 255.0f;                  // per-note velocity
                e.drawOverlap = overlap;
                events.add(e);
            }
            if (firedCol >= 0) { lastTick[ch] = 100000 + firedCol; lastTickLoop[ch] = loopCount; }
            continue;
        }

        const int n = c.numSteps;
        if (n <= 0) continue;

        for (int s = 0; s < n; ++s)
        {
            double st, en;
            stepSpan(s, n, pp.swing, st, en);
            const int roll = juce::jlimit(1, 6, c.stepRoll[s]);

            // NUDGE (micro-timing): shift this step's hit early/late by up to HALF its span.
            // Clamped inside the bar (an early-nudged step 1 fires AT the bar start; a late-nudged
            // last step can't spill into the next bar).
            const double nud = (double) juce::jlimit(-1.0f, 1.0f, c.stepNudge[s]) * 0.5 * (en - st);
            for (int j = 0; j < roll; ++j)
            {
                const double pos = juce::jlimit(0.0, 0.9999995, st + nud + (en - st) * (double) j / (double) roll);
                const bool atZero = (oldPos == 0.0 && pos == 0.0);           // bar start fires inclusively
                if (! atZero && ! (pos > oldPos && pos <= newPos)) continue;

                // Seam dedupe: never fire the same tick twice within one loop iteration
                // (protects against block-seam floating-point re-crossings in DAW sync).
                const int tickId = s * 8 + j;
                if (lastTick[ch] == tickId && lastTickLoop[ch] == loopCount) continue;
                lastTick[ch] = tickId; lastTickLoop[ch] = loopCount;

                // Refresh the analysed channel's spectrum at every step/sub boundary so
                // the EQ graph updates. Only the inspected channel has a tap.
                if (auto* tap = c.analysisTap) tap->arm();

                // Per-step LOOP condition: fire only on the chosen loops of an N-loop cycle
                // (N=1 OR no bars chosen = always).
                bool condOk = true;
                const int condN = juce::jlimit(1, 10, c.stepCondLen[s]);
                if (condN > 1 && c.stepCondMask[s] != 0) {
                    const int bar = ((loopCount % condN) + condN) % condN;
                    condOk = ((c.stepCondMask[s] >> bar) & 1) != 0;
                }
                if (! c.steps[s] || ! condOk) continue;
                if (c.stepMerge[s]) continue;   // MERGE: a continuation step never re-fires - the
                                                // chain HEAD's gate (below) rings through it.

                // Roll ramp: each successive sub-hit ramps in velocity across the ratchet. The amount
                // (stepRollDecay, -1..+1) is set by the Roll cell's X-drag: negative = fade out (each
                // hit quieter), 0 = flat, positive = build up (each hit louder).
                float velScale = 1.0f;
                if (roll > 1)
                {
                    const float rr   = juce::jlimit(-1.0f, 1.0f, c.stepRollDecay[s]);
                    const float frac = (float) j / (float)(roll - 1);   // 0 = first hit, 1 = last
                    velScale = (rr >= 0.0f) ? (1.0f - rr) + rr * frac    // build up
                                            : 1.0f + rr * frac;         // fade out (rr < 0)
                    velScale = juce::jmax(0.0f, velScale);
                }

                const int off = baseOffset + (int) juce::jlimit(0.0, (double) spanSamples - 1.0,
                                                (pos - oldPos) / span * (double) spanSamples);
                // Per-step LENGTH: gate the hit after (Length x this step's duration). 0 = off (natural).
                // MERGE chains: following merged steps EXTEND this head's duration (one long note,
                // piano-roll style), so the gate covers the whole chain; Length (if set) is a
                // fraction of the WHOLE chain. A merged chain always gates (even Length 0) so the
                // note actually spans/holds through the merged steps (sustain holds, AHD rescales).
                long gate = 0;
                const float gl = juce::jlimit(0.0f, 1.0f, c.stepNoteLen[s]);
                double chainFrac = (en - st) / span;                  // this step's share of the bar span
                for (int k = s + 1; k < n && c.stepMerge[k]; ++k)
                {
                    double st2, en2; stepSpan(k, n, pp.swing, st2, en2);
                    chainFrac += (en2 - st2) / span;
                }
                const bool mergedChain = chainFrac > (en - st) / span + 1.0e-9;
                if (gl > 0.001f || mergedChain)
                    gate = (long) juce::jmax(64.0, chainFrac * (double) spanSamples * (double)(gl > 0.001f ? gl : 1.0f));
                // SLIDE: glide across the FULL step so the pitch lands on the NEXT active step's
                // pitch exactly at the boundary (that step then continues at the landed pitch).
                long slideLen = 0; float slideTo = 0.0f;
                if (c.stepSlide[s])
                {
                    slideTo = c.stepPitch[s];             // fallback: no other active step = no bend
                    for (int k = 1; k <= c.numSteps; ++k) // next ACTIVE step, wrapping round the bar
                    {
                        const int ns = (s + k) % juce::jmax(1, c.numSteps);
                        if (c.steps[ns]) { slideTo = c.stepPitch[ns]; break; }
                    }
                    slideLen = (long) juce::jmax(256.0, (en - st) / span * (double) spanSamples);
                }
                events.add({ ch, s, velScale, j, roll, off, gate, slideLen, slideTo });
            }
        }
    }
}
