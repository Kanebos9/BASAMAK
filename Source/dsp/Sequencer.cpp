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
    for (auto& b2 : barPlays) b2 = 0;
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
    juce::AudioBuffer<float>* delaySendBus,
    juce::AudioBuffer<float>* reverbSendBusB,
    juce::AudioBuffer<float>* delaySendBusB)
{
    juce::Array<TriggerEvent> events;
    if (numSamples <= 0) return events;

    // Bar length for tempo-synced per-slot LFOs: default from the standalone tempo (also covers
    // stopped/DAW-not-playing); advanceDaw overrides it with the live host BPM while playing.
    blockBarSeconds = (juce::jmax(1, timeSigNum) * 4.0 / juce::jmax(1, timeSigDen))
                      * 60.0 / juce::jmax(1.0f, standaloneBpm);

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
        // [2026-07-15 23:00] MUTED (or solo-excluded) channels DON'T FIRE AT ALL. They used to
        // trigger silently (renderInto only gated the OUTPUT), so voices accumulated frozen while
        // muted and all became audible at once on unmute ("really loud suddenly" + the roll note
        // the playhead had already passed playing after unmute - both user reports, same root).
        // Skipping here also stops a muted channel's duck pulses, chokes and MIDI-out notes -
        // a silent channel shouldn't push the mix around.
        if (c.mute || (anySoloIn(patterns[playPattern]) && ! c.solo)) return;
        // SIDECHAIN DUCK: this hit pushes down every channel set to "Duck by" this channel
        // (Routing popup). Level-only - the ducked sound recovers; nothing is cut like choke.
        for (int o = 0; o < NUM_CHANNELS; ++o)
        {
            auto& d = patterns[playPattern].channels[o];
            if (o != e.channel && d.duckBy == e.channel && d.duckAmt > 0.001f) d.duckPulse();
        }
        if (c.midiOut) return;   // MIDI-out channels make no internal sound (they emit notes in the processor)
        if (e.isDraw) {   // PIANO-ROLL note (chord tones overlap, melody cuts). drawSlot 0=both, 1/2=one slot.
            const int mask = e.drawSlot == 1 ? 0b01 : e.drawSlot == 2 ? 0b10 : 0b11;
            // ONE-SHOT notes = the STEP contract: instant trigger, no gate, pure AHD natural ring
            // (bit-identical to a bare step) - the importer marks them; right-click menu toggles.
            const long g = e.drawOneShot ? 0 : e.gate;
            const bool kg = ! e.drawOneShot;
            c.strumFlip = e.drawStrumUp;   // per-note strum direction + amount (trigger() consumes both)
            c.strumOverride = e.drawStrumPct >= 0 ? (float) e.drawStrumPct * 0.01f : -1.0f;
            c.legatoNext = e.drawLegato;   // [2026-07-16] per-note LEGATO: trigger() inherits the ringing envelope (consumed)
            if (e.drawGlideFrom > -900.0f) {   // GLIDE: slide from the legato predecessor's pitch to this one's
                const long gs = (long) (c.keysGlide * 0.4 * sampleRate);   // same 0..400 ms as live keys
                // Mono keeps the live 15 ms handover; POLY must NOT fade (it would cut ringing
                // chord tones - live poly glide never fades either) [2026-07-16 round-5].
                if (! c.keysPolyMode) c.fadeOutVoices(0.015f);
                c.trigger(e.drawVel, e.drawGlideFrom, e.drawNotePan, g, /*glideTo*/ e.drawPitch, gs,
                          /*forceOverlap*/ true, mask, kg);
            } else
                // POLY channels: release tails ring over the next note in PLAYBACK exactly as
                // they do live (mono channels keep the classic mono-cut - also like live).
                c.trigger(e.drawVel, e.drawPitch, e.drawNotePan, g, 0.0f, 0, e.drawOverlap || c.keysPolyMode, mask, kg);
            return; }
        // Choke groups: a hit FADES OUT (~3 ms) the ringing tails of other channels in the same
        // group (e.g. a closed hi-hat silencing an open one). A hard cut clicked whenever the
        // choking hit was quieter than the tail it cut.
        if (c.chokeGroup > 0)
            for (int o = 0; o < NUM_CHANNELS; ++o)
                if (o != e.channel && patterns[playPattern].channels[o].chokeGroup == c.chokeGroup)
                {   // [2026-07-14 10:05] choke fade is PITCH-AWARE now (3 ms on a sub tail = a click;
                    // hats keep their tight ~3-5 ms feel automatically via their higher base).
                    auto& oc = patterns[playPattern].channels[o];
                    oc.fadeOutVoices(oc.retrigFadeSec());
                }
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
            chan.lfoBarSeconds = (float) blockBarSeconds;   // tempo-synced per-slot LFOs
            chan.modWheel      = modWheel;                   // live mod wheel (shared mod source)
            // FREE-RUN LFO anchor: bars into the playing unit at THIS segment's start (group bar
            // index + fraction). Same bar position = same phase on every pass (deterministic).
            // [2026-07-16] ONLY while the transport actually plays: this loop also runs STOPPED
            // (live keys/tails render through it), and the jmax(0.0, ...) turned the parked
            // barPosition into "bar position 0" every block - which RESET the free-run clock
            // per block = FREE LFOs frozen at phase 0 on live keys (the Sequencer Bass bug).
            // Stopped = the -1 sentinel -> renderInto's lfoFreeSec wall clock keeps them moving.
            chan.lfoBarPos = isCurrentlyPlaying
                ? juce::jmax(0.0,
                    (double)(playPattern - groupHead(playPattern)) + barPosition
                    - (double)(numSamples - segStart) / juce::jmax(1.0, blockBarSeconds * sampleRate))
                : -1.0;
            // STEP MOD lanes: the current step position (within-bar fraction * numSteps) at this
            // segment; stopped = -1 (no step is playing - the lanes read as silent, not "step 0").
            if (isCurrentlyPlaying)
            { double bf = barPosition - (double)(numSamples - segStart) / juce::jmax(1.0, blockBarSeconds * sampleRate);
              bf -= std::floor(bf);   // wrap into 0..1 of the bar
              chan.modStepPos = (float) (bf * (double) juce::jmax(1, chan.numSteps)); }
            else chan.modStepPos = -1.0f;
            const int ob = chan.outputBus;   // 0 = Main; 1..numAux = a discrete aux out
            const bool toMain = ! (ob >= 1 && ob <= numAux && auxBuses != nullptr && auxBuses[ob - 1] != nullptr);
            juce::AudioBuffer<float>* dest = toMain ? &audio : auxBuses[ob - 1];
            // Sends only apply to Main-routed channels (aux outs are dry, on their own DAW track).
            chan.renderInto(*dest, segStart, segEnd - segStart, soloPlay,
                            toMain ? reverbSendBus  : nullptr,
                            toMain ? delaySendBus   : nullptr,
                            toMain ? reverbSendBusB : nullptr,   // channel revBus/delBus pick A or B
                            toMain ? delaySendBusB  : nullptr);
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
            fc.lfoBarSeconds = (float) blockBarSeconds; fc.modWheel = modWheel; fc.pitchWheel = pitchWheel;
            if (! fc.midiOut) { fc.lfoBarPos = -1.0; fc.modStepPos = -1.0f; fc.renderInto(audio, 0, numSamples, soloFade); }   // dry tail into Main
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
                gc.lfoBarSeconds = (float) blockBarSeconds; gc.modWheel = modWheel; gc.pitchWheel = pitchWheel;
                if (! gc.midiOut && gc.anyVoiceActive())
                {   // ringing group members share the playing unit's timeline anchor
                    gc.lfoBarPos = juce::jmax(0.0, (double)(playPattern - groupHead(playPattern)) + barPosition
                                                   - (double) numSamples / juce::jmax(1.0, blockBarSeconds * sampleRate));
                    { double bf = barPosition - (double) numSamples / juce::jmax(1.0, blockBarSeconds * sampleRate);
                      bf -= std::floor(bf); gc.modStepPos = (float) (bf * (double) juce::jmax(1, gc.numSteps)); }
                    gc.renderInto(audio, 0, numSamples, soloG);
                }
            }
        }
    }

    // Also render the VIEWED pattern when it differs, so auditioning a channel there
    // is still heard (always to Main, so it's audible regardless of routing).
    if (currentPattern != playPattern)
    {
        const bool soloView = anySoloIn(patterns[currentPattern]);
        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        {
            patterns[currentPattern].channels[ch].modWheel = modWheel; patterns[currentPattern].channels[ch].pitchWheel = pitchWheel;
            patterns[currentPattern].channels[ch].lfoBarSeconds = (float) blockBarSeconds;
            { auto& vc = patterns[currentPattern].channels[ch];
              vc.lfoBarPos = -1.0; vc.modStepPos = -1.0f;   // viewed-but-not-playing audition
              vc.renderInto(audio, 0, numSamples, soloView); }
        }
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

    const double samplesPerBar = 1.0 / juce::jmax(1.0e-12, barsPerSample);
    if (newPos < 1.0)
    {
        checkChannelTriggers(oldPos, newPos, numSamples, 0, samplesPerBar, events);
        barPosition = newPos;
        return;
    }

    // Ticks in the tail of this bar (oldPos -> 1.0), then decide stop/next BEFORE the new bar.
    const int tailSamples = juce::jlimit(0, numSamples,
                                         (int) std::floor((1.0 - oldPos) / juce::jmax(1.0e-12, barsPerSample)));
    checkChannelTriggers(oldPos, 1.0, juce::jmax(1, tailSamples), 0, samplesPerBar, events);

    onBarComplete();
    if (finished) { playing = false; barPosition = 0.0; isCurrentlyPlaying = false; return; }

    double rem = newPos - 1.0;
    while (rem >= 1.0) rem -= 1.0;   // pathological giant-buffer safety
    // Floating-point DUST remainder: the bar boundary IS the block end (at 120 BPM the bar
    // is an exact multiple of common block sizes, so this happens every loop) and rem is a
    // ~1e-16 rounding leftover. Snap to 0 and let the next block (oldPos == 0) fire step 0
    // with a real segment - the dust segment used to fire it itself with a 1-sample span.
    if (rem < barsPerSample * 1.0e-3) rem = 0.0;
    if (rem > 0.0)
        checkChannelTriggers(0.0, rem, juce::jmax(1, numSamples - tailSamples), tailSamples, samplesPerBar, events);
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
        playPattern = groupHead(playPattern);   // [1.5.0] park at the group HEAD like the standalone
                                                // Stop (a mid-group stop used to leave playback parked
                                                // on bar 2 = "starts from the second pattern sometimes")
        resetTickDedupe();
        return;
    }

    // Transport just (re)started → re-arm the play-mode counters + start from the viewed pattern
    if (!wasPlaying) { finished = false; patternRepeatCount = 0; loopCount = 0; resetChains(); wasPlaying = true;
                       for (auto& b2 : barPlays) b2 = 0;
                       // Start from the viewed pattern - but if the view merely FOLLOWED playback onto
                       // a middle bar of the same merged group (Follow on), keep the parked head instead
                       // of restarting mid-group (the standalone guard, now shared by the DAW path).
                       if (groupHead(playPattern) != groupHead(currentPattern)) playPattern = currentPattern;
                       resetTickDedupe(); }

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
    blockBarSeconds = quartersPerBar * 60.0 / hostBpm;   // live host tempo -> synced LFOs

    double oldPos = std::fmod(ppqPos / quartersPerBar, 1.0);
    if (oldPos < 0.0) oldPos += 1.0;

    // Advance by one block (ppq is in quarter notes)
    double beatsPerSample = hostBpm / (60.0 * sampleRate);
    double barsPerSample  = beatsPerSample / quartersPerBar;
    double newPos = oldPos + numSamples * barsPerSample;

    const double samplesPerBar = 1.0 / juce::jmax(1.0e-12, barsPerSample);
    if (newPos < 1.0)
    {
        checkChannelTriggers(oldPos, newPos, numSamples, 0, samplesPerBar, events);
        barPosition = newPos;
        return;
    }

    const int tailSamples = juce::jlimit(0, numSamples,
                                         (int) std::floor((1.0 - oldPos) / juce::jmax(1.0e-12, barsPerSample)));
    checkChannelTriggers(oldPos, 1.0, juce::jmax(1, tailSamples), 0, samplesPerBar, events);

    onBarComplete();
    if (finished) { isCurrentlyPlaying = false; return; }

    double rem = newPos - 1.0;
    while (rem >= 1.0) rem -= 1.0;
    if (rem < barsPerSample * 1.0e-3) rem = 0.0;   // floating-point dust at an exact block-edge
                                                   // wrap (see advanceStandalone)
    if (rem > 0.0)   // rem == 0 -> next block's oldPos == 0 fires step 0 (no double-fire)
        checkChannelTriggers(0.0, rem, juce::jmax(1, numSamples - tailSamples), tailSamples, samplesPerBar, events);
    barPosition = rem;
}

//==============================================================================
// Apply the current pattern's play mode after a bar finishes.
void Sequencer::onBarComplete()
{
    ++loopCount;                  // one more BAR COMPLETION (tick-dedupe epochs)
    // [1.5.0] per-BAR play count: "one loop" = one play of THIS bar (drives the step/note LOOP
    // CONDITIONS now that a merged bar can repeat itself - a group pass is no longer well-defined).
    barPlays[playPattern] = juce::jmin(1 << 24, barPlays[playPattern] + 1);
    ++patternRepeatCount;
    if (recordLoopLock.load(std::memory_order_relaxed))
    {
        // Recording: LOOP the armed unit LINEARLY head -> end -> head. Per-bar play modes are
        // IGNORED while the take rolls - the looper-style group recording (clear once, spanning
        // notes, per-pass reopen) needs strictly ordered bars.
        const int gHead = groupHead(playPattern), gEnd = groupEnd(playPattern);
        patterns[playPattern].visitCount = 0;   // modes are ignored while recording - keep counts fresh
        fadeOutPattern = playPattern;
        playPattern = (playPattern < gEnd) ? playPattern + 1 : gHead;
        finished = false;
        patternChanged.store(true);
        return;
    }
    // [1.5.0 FINAL, the user's spec verbatim: "N shows the loops count of THAT pattern, period.
    // not the merged whole thing"] EVERY bar follows its OWN play mode with its OWN count:
    //   - N counts CONSECUTIVE plays of THIS bar (per-bar visitCount, reset when its jump fires);
    //     an unmet count means the bar simply PLAYS AGAIN - merged or not, no special group flow.
    //   - merging only writes DEFAULTS: every bar = Chain -> next x1, the last = Chain -> head x1
    //     (so an untouched group loops exactly like before);
    //   - LoopForever = repeat this bar forever; Stop/Next work the same per-bar way.
    // ("group passes" as a counting unit was an invention of mine and is gone - it made an unmet
    //  count ADVANCE the group = "just plays the next merged pattern no matter what".)
    auto& p = patterns[playPattern];
    ++p.visitCount;
    const int target = juce::jmax(1, p.repeatTarget);
    if (p.playMode == StopAfterN && p.visitCount >= target)
    {
        finished = true;
        p.visitCount = 0;
        fadeOutPattern = playPattern;           // the stopping bar's tails still ring out
        playPattern = groupHead(playPattern);   // park at the head so the next Play starts the group
    }                                           // from its beginning (the Stop-button convention)
    else if (p.playMode == NextAfterN && p.visitCount >= target)
    {
        fadeOutPattern = playPattern;   // let the outgoing pattern's voices ring out (no hard-cut click)
        playPattern = juce::jlimit(0, NUM_PATTERNS - 1, p.gotoPattern);   // exact bar, groups included
        p.visitCount = 0;
        finished = false;
        patternChanged.store(true);   // editor follows playPattern if "Follow" is on
    }
    else if (p.playMode == Chain && p.chainLen > 0
             && p.visitCount >= juce::jmax(1, p.chainLoops[p.chainStep % p.chainLen]))
    {
        // This bar has played its count - jump to the entry's target (cycling through the chain).
        const int tgt = juce::jlimit(0, NUM_PATTERNS - 1, p.chainSeq[p.chainStep % p.chainLen]);
        p.chainStep = (p.chainStep + 1) % p.chainLen;
        p.visitCount = 0;
        fadeOutPattern = playPattern;
        playPattern = tgt;   // exact target bar - a middle group bar starts there and runs on (user rule)
        finished = false;
        patternChanged.store(true);
    }
    // LoopForever (or a count not yet reached): the bar plays again - no pattern change.
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
                                      double samplesPerBar,
                                      juce::Array<TriggerEvent>& events)
{
    if (newPos < oldPos) return;
    const double span = juce::jmax(1.0e-12, newPos - oldPos);
    // Bar-start tolerance: in DAW sync the block-start position comes from the host's ppq and can
    // carry a sub-sample epsilon after a wrap; treat anything within a quarter sample of 0 as the
    // bar start so column-0/step-0 hits are never dropped (the dedupe stops double-fires).
    const double zeroTol = 0.25 / juce::jmax(1.0, samplesPerBar);
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
            if (ch == recordSuppressCh.load(std::memory_order_relaxed)
                || ch == recordSuppressCh2.load(std::memory_order_relaxed)) continue;
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
                const bool atZero = (oldPos <= zeroTol && colPos == 0.0);
                if (! atZero && ! (colPos > oldPos && colPos <= newPos)) continue;
                if (prevTick == 100000 + (int) nt.start && prevTickLoop == loopCount) continue;   // seam re-crossing
                firedCol = nt.start;
                // [2026-07-15 23:00] per-NOTE loop condition (the step Loop system, per note):
                // fire only on the chosen loops of an N-loop cycle. Len 1 / mask 0 = every loop.
                if (nt.condLen > 1 && nt.condMask != 0)
                {
                    const int bar = ((barPlays[playPattern] % (int) nt.condLen) + (int) nt.condLen) % (int) nt.condLen;   // [1.5.0] per-BAR count
                    if (((nt.condMask >> bar) & 1) == 0) continue;
                }
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
                // GATE from the ABSOLUTE bar length, NEVER the segment ratio: at a loop wrap the
                // post-wrap segment can be a floating-point sliver (rem ~ 1e-16 bars, spanSamples
                // clamped to 1) - segBars/span*spanSamples exploded to ~1e11 samples there, which
                // was the "first note rings much longer from the second loop on" bug.
                const double segBars = (double) nt.len / (double) R;
                const long gate = (long) juce::jmax(64.0, segBars * samplesPerBar);
                const int off = baseOffset + (int) juce::jlimit(0.0, (double) spanSamples - 1.0,
                                                (colPos - oldPos) / span * (double) spanSamples);
                // GEOMETRY-DRIVEN LEGATO + GLIDE [2026-07-16 round-5, user design - replaced the
                // per-note flags]: the notes THEMSELVES say what was played. A note with a legato
                // PREDECESSOR (the most recent earlier note that overlaps or butts up to it, ~1/64
                // bar tolerance) follows the sound's CURRENT settings: Glide knob up = slide from
                // the predecessor's pitch (different pitches only); a Legato mode = continue the
                // ringing envelope (no re-attack). Detached notes always restart. One source of
                // truth (grid + Mode + knob), nothing stamped = nothing to go stale.
                float glideFrom = -999.0f; bool legato = false;
                if (c.keysLegato || c.keysGlide > 0.0001f)
                {
                    const int adjGap = R / 64;
                    int bestStart = -1; int bestSemi = 0;
                    for (int mj = 0; mj < nN; ++mj)
                    {
                        if (mj == ni) continue;
                        const auto& m = c.drawNotes[mj];
                        if (m.start < nt.start && (m.start + m.len) >= (nt.start - adjGap)
                            && m.start > bestStart)
                        { bestStart = m.start; bestSemi = (int) m.semi; }
                    }
                    if (bestStart >= 0)
                    {
                        legato = c.keysLegato;
                        if (c.keysGlide > 0.0001f && bestSemi != (int) nt.semi)
                            glideFrom = (float) bestSemi;
                    }
                }
                TriggerEvent e; e.channel = ch; e.step = 0; e.offset = off; e.gate = gate;
                e.isDraw = true; e.drawPitch = (float) nt.semi;
                e.drawVel = (float) nt.vel / 255.0f;                  // per-note velocity
                e.drawSlot = nt.slot;                                 // per-note slot tag
                e.drawOneShot = nt.oneShot != 0;
                e.drawStrumUp = nt.strumUp != 0;
                e.drawLegato = legato;   // geometry + Mode (see above), not a stored flag
                e.drawStrumPct = nt.strumPct > 100 ? -1 : (int) nt.strumPct;
                e.drawNotePan = nt.pan == DrumChannel::PAN_INHERIT ? c.drawPan : (float) nt.pan * 0.01f;   // 127 = inherit channel pan; else explicit (0 = true centre)
                e.drawOverlap = overlap;
                e.drawGlideFrom = glideFrom;
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
                const bool atZero = (oldPos <= zeroTol && pos == 0.0);       // bar start fires inclusively
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
                    const int bar = ((barPlays[playPattern] % condN) + condN) % condN;   // [1.5.0] per-BAR count
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
                double chainBars = en - st;                           // chain length in BARS (absolute units -
                for (int k = s + 1; k < n && c.stepMerge[k]; ++k)     // never the segment ratio, see the draw
                {                                                     // branch's wrap-sliver note)
                    double st2, en2; stepSpan(k, n, pp.swing, st2, en2);
                    chainBars += en2 - st2;
                }
                const bool mergedChain = chainBars > (en - st) + 1.0e-9;
                if (gl > 0.001f || mergedChain)
                    gate = (long) juce::jmax(64.0, chainBars * samplesPerBar * (double)(gl > 0.001f ? gl : 1.0f));
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
                    slideLen = (long) juce::jmax(256.0, (en - st) * samplesPerBar);
                }
                events.add({ ch, s, velScale, j, roll, off, gate, slideLen, slideTo });
            }
        }
    }
}
