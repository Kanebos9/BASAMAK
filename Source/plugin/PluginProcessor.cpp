#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessor::BusesProperties DrumSequencerProcessor::makeBuses()
{
    // Main stereo out + NUM_AUX_OUTS discrete stereo outs (default-disabled so standalone /
    // simple hosts just use Main; a DAW can enable them for per-channel routing).
    auto b = BusesProperties().withOutput("Main", juce::AudioChannelSet::stereo(), true);
    for (int i = 1; i <= NUM_AUX_OUTS; ++i)
        b = b.withOutput("Out " + juce::String(i), juce::AudioChannelSet::stereo(), false);
    return b;
}

bool DrumSequencerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    for (int i = 1; i < layouts.outputBuses.size(); ++i) {
        const auto& set = layouts.outputBuses.getReference(i);
        if (! set.isDisabled() && set != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

DrumSequencerProcessor::DrumSequencerProcessor()
    : AudioProcessor(makeBuses())
{
    // Default channel names + starting sounds. MIDI notes are WHITE KEYS in order, C2..C3
    // (channel 1 = C2/36 ... channel 8 = C3/48) so MIDI-out lands on a clean ascending scale.
    const char* names[] = { "Kick","Snare","Closed Hat","Open Hat","Clap","Tom","Crash","Rim" };
    const int   notes[] = { 36, 38, 40, 41, 43, 45, 47, 48 };   // C2 D2 E2 F2 G2 A2 B2 C3
    const DrumSoundGenerator::Type types[] = {
        DrumSoundGenerator::Type::Kick808,
        DrumSoundGenerator::Type::Snare808,
        DrumSoundGenerator::Type::HatClosed808,
        DrumSoundGenerator::Type::HatOpen808,
        DrumSoundGenerator::Type::ClapClassic,
        DrumSoundGenerator::Type::TomMid,
        DrumSoundGenerator::Type::Crash,
        DrumSoundGenerator::Type::Rim
    };
    const juce::Colour colours[] = {
        juce::Colours::orangered, juce::Colours::lightblue,
        juce::Colours::yellow,    juce::Colours::lightyellow,
        juce::Colours::pink,      juce::Colours::green,
        juce::Colours::cyan,      juce::Colours::white
    };

    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        auto& pat = sequencer.patterns[p];
        pat.gotoPattern = (p + 1) % Sequencer::NUM_PATTERNS;
        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        {
            auto& ch = pat.channels[i];
            // The default tables cover the first 8 channels; channels 9..16 get generic defaults.
            ch.channelName   = (i < 8) ? juce::String(names[i]) : ("Ch " + juce::String(i + 1));
            ch.channelColour = colours[i % 8];
            ch.midiNote      = (i < 8) ? notes[i] : juce::jmin(127, 49 + (i - 8));   // continue up the scale
            ch.soundType     = types[i % 8];
        }
    }

    // Default sound on every channel: Slot 1 = Analog + FM (SrcOsc), Slot 2 empty (no sample default).
    for (auto& pat : sequencer.patterns)
        for (auto& ch : pat.channels) {
            for (auto& s : ch.slots) s = DrumChannel::Slot();      // both empty
            ch.slots[0].engine = DrumChannel::SrcOsc; ch.slots[0].weight = 1.0f;
            ch.padX = 0.0f; ch.padY = 0.5f;
            ch.restoredSlots = true;   // authored -> prepareToPlay won't rebuild from legacy
        }
}

// NOTE: the curated host-automation parameters (per-channel Volume/Pan/Mute/LP Cutoff/Reverb/Delay +
// Master) were REMOVED - they had drifted out of date (e.g. channel Pan no longer exists; panning is
// per-step) and were low value, so the plugin now exposes NO host-automatable parameters.

DrumSequencerProcessor::~DrumSequencerProcessor() {}

void DrumSequencerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // The synth engine runs at kEngineOS x the host rate (anti-aliasing); channels are prepared
    // at that higher rate + block size. The processor down-samples back to the host rate below.
    for (auto& pat : sequencer.patterns)
        for (auto& ch : pat.channels)
        {
            ch.engineOS = kEngineOS;
            ch.prepareToPlay(sampleRate * kEngineOS, samplesPerBlock * kEngineOS);
            // (ch.prepareToPlay loads the default sound only if no slot has a sample, so
            //  already-loaded samples are preserved across re-prepare.)
        }

    // Whole-engine oversampler (all output channels). Polyphase IIR = low latency, no transient
    // pre-ring (best for drums). Created for the current output channel count.
    {
        const int nCh = juce::jmax(2, getTotalNumOutputChannels());
        engineOS = std::make_unique<juce::dsp::Oversampling<float>>(
            (size_t) nCh, 1 /*=2x*/, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
        engineOS->initProcessing((size_t) juce::jmax(1, samplesPerBlock));
        engineOS->reset();
    }

    // Master limiter lookahead (~1.5 ms). Constant latency = engine OS latency + lookahead.
    limiterLAlen = (int) (0.0015 * sampleRate);
    limiterLA.setSize(2, juce::jmax(1, limiterLAlen));
    limiterLA.clear();
    limiterLAhead = 0;
    setLatencySamples((int) engineOS->getLatencyInSamples() + limiterLAlen);

    fdn.prepare(sampleRate);
    fdn.reset();

    // Shared per-slot spectrum scratch: sized for the biggest engine-rate block (min 8192 for safety).
    analysisScratch.assign((size_t) juce::jmax(8192, samplesPerBlock * kEngineOS), 0.0f);

    // Delay buffer: 2 seconds max
    delayBuffer.setSize(2, (int)(sampleRate * 2.0));
    delayBuffer.clear();
    delayFbLp[0] = delayFbLp[1] = delayFbHp[0] = delayFbHp[1] = 0.0f;
    for (auto& n : activeMidiNote) n = -1;   // no held MIDI-out notes yet

    reverbSendOS.setSize  (2, juce::jmax(1, samplesPerBlock * kEngineOS));
    delaySendOS.setSize   (2, juce::jmax(1, samplesPerBlock * kEngineOS));
    reverbSendBase.setSize(2, juce::jmax(1, samplesPerBlock));
    delaySendBase.setSize (2, juce::jmax(1, samplesPerBlock));
    reverbSendOS.clear(); delaySendOS.clear(); reverbSendBase.clear(); delaySendBase.clear();   // no startup garbage into the FX
    reverbPreBuffer.setSize(2, (int)(sampleRate * 0.130) + 2);   // up to ~120 ms pre-delay
    reverbPreBuffer.clear(); reverbPreHead = 0;
    delayWriteHead = 0;
    limiterGain = 1.0f;
    masterGlueEnv = 0.0f;
}

void DrumSequencerProcessor::releaseResources() {}

void DrumSequencerProcessor::processBlock(juce::AudioBuffer<float>& audio,
                                           juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();
    processHeartbeat.fetch_add(1, std::memory_order_relaxed);   // editor watches this to detect a frozen host

    const int numSamples = audio.getNumSamples();

    // Incoming-MIDI key notes gathered THIS block (handled in the keys block below with full
    // context); local to the audio thread, so the message-thread ring stays single-producer.
    KeyQEvt midiKeyEvts[64]; int nMidiKeyEvts = 0;

    //-- Handle MIDI input (MIDI learn + notes -> keys + CC routing)
    for (auto meta : midi)
    {
        auto msg = meta.getMessage();

        // MIDI-in monitor: count every message + remember the last CC, so the UI
        // can show whether MIDI is reaching the plugin at all.
        midiInCount.fetch_add(1, std::memory_order_relaxed);
        if (msg.isController())
        {
            lastCcNum.store(msg.getControllerNumber(), std::memory_order_relaxed);
            lastCcVal.store(msg.getControllerValue(),  std::memory_order_relaxed);
            lastCcChan.store(msg.getChannel(),         std::memory_order_relaxed);
        }

        // MIDI learn
        if (midiLearn.processMidiMessage(msg))
            continue;

        // Incoming MIDI NOTES play the KEYS engine (selected channel; poly follows the channel's
        // keyboard Mono/Poly toggle) - no setup needed: a MIDI keyboard just works. Pad-style
        // controllers should send CC + MIDI-learn. Collected LOCALLY (we're already on the audio
        // thread) and handled in the keys block below - the ring stays single-producer.
        if (msg.isNoteOnOrOff() && nMidiKeyEvts < (int) (sizeof(midiKeyEvts) / sizeof(midiKeyEvts[0])))
        {
            if (msg.isNoteOn())
                midiKeyEvts[nMidiKeyEvts++] = { 1, (uint8_t) (msg.getNoteNumber() & 0x7f),
                                                (uint8_t) juce::jlimit(1, 127, (int) std::lround(msg.getFloatVelocity() * 127.0f)) };
            else
                midiKeyEvts[nMidiKeyEvts++] = { 0, (uint8_t) (msg.getNoteNumber() & 0x7f), 0 };
        }

        // Route CC messages to assigned parameters/steps
        if (msg.isController())
            routeCC(msg);
    }
    midi.clear();

    //-- Point the spectrum tap at the channel the editor is inspecting. The analysed channel
    //   also gets the SHARED per-slot analysis scratch (one buffer for the whole plugin).
    {
        const int ac = analyzeChannel.load();
        const int as = analysisSlot.load();   // PER-SLOT EQ: which slot to analyse (-1 = mix)
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c) {
            auto& ch = sequencer.channel(c);
            ch.analysisTap    = (c == ac) ? &spectrumTap : nullptr;
            ch.analysisSlot   = (c == ac) ? as : -1;
            ch.analysisBuf    = (c == ac && ! analysisScratch.empty()) ? analysisScratch.data() : nullptr;
            ch.analysisBufLen = (c == ac) ? (int) analysisScratch.size() : 0;
        }
    }

    // Spectrum refresh: a FIXED 20 Hz grid (every 0.05 s) - ALWAYS, playing or stopped (user request:
    // step-rate refresh was too slow at low tempos / long steps; TEST/Auto-Test used 0.1 s). The grid
    // stays PHASE-ALIGNED to hits: DrumChannel::trigger() re-arms the tap at every hit's attack and the
    // TEST handler below resets this counter, so repeated hits are captured at the SAME time offsets ->
    // the display stays consistent (the old free-running-window inconsistency does not come back).
    {
        analysisArmCtr += audio.getNumSamples();
        const int armEvery = juce::jmax(1, (int) (0.05 * currentSampleRate));
        if (analysisArmCtr >= armEvery) { analysisArmCtr = 0; spectrumTap.arm(); }
    }

    //-- Audition trigger from the TEST button
    {
        int tc = testTriggerRequest.exchange(-1);
        if (tc >= 0 && tc < Sequencer::NUM_CHANNELS)
        {
            // Cut this channel's previous voices first so every TEST is a clean, consistent single hit
            // (otherwise overlapping/ringing tails sum and the level seems to vary between taps).
            sequencer.channel(tc).silenceAllVoices();
            sequencer.channel(tc).trigger();   // trigger() arms the spectrum capture aligned to this hit's attack
            // Phase-align the stopped re-arm grid to THIS hit so every tap samples the sound at the same points
            // (attack, +0.1 s, +0.2 s, ...) -> the EQ visual is identical tap-to-tap instead of catching a random
            // point in the decay each time.
            analysisArmCtr = 0;
        }
    }

    //-- KEYS: the on-screen keyboard (mono). Down events play the SELECTED channel via
    //   DrumChannel::keyDown; while RECORDING, notes are stamped into the playing pattern's steps
    //   (nearest-step quantise; pitch = semitones from the FIXED C3 reference) and logged as take
    //   events. In "this pattern" modes every pattern LOOP is its own take: at each loop boundary
    //   a marker is logged and the channel restarts CLEAN for the next pass. A key held across a
    //   step boundary AUTO-MERGES the new step (one long note - see DrumChannel::stepMerge).
    {
        const int  chIdx = juce::jlimit(0, Sequencer::NUM_CHANNELS - 1, lastSelectedChannel);
        const bool rec   = keysRecording.load(std::memory_order_relaxed);
        // Which pattern the keyboard plays (and records into):
        //  - chain record: the PLAYING pattern (follow the chain -> its loaded sound + slot-2 setting);
        //  - this-pattern record: always the ARMED pattern (the chain may play others, but we only
        //    play/record the one we armed);
        //  - not recording: the VIEWED pattern.
        const int  armedP = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, keysArmedPattern.load(std::memory_order_relaxed));
        // A MERGED GROUP records like a chain: the armed "pattern" is really the whole group, so the
        // recorder follows the playing bar and every bar-boundary is a take boundary.
        const bool chain = keysRecMode.load(std::memory_order_relaxed) >= 2 || sequencer.inGroup(armedP);
        const int  keyPat = (rec && sequencer.isCurrentlyPlaying)
                              ? (chain ? juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, sequencer.playPattern) : armedP)
                              : juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, sequencer.currentPattern);
        // RECORDING is ALWAYS a draw lane: if the playing pattern's channel isn't draw yet (a chain
        // may visit a step-mode pattern), force it into draw now. Step data is KEPT (draw ignores it),
        // so switching back to steps later restores it.
        if (rec && sequencer.isCurrentlyPlaying)
        {
            auto& pc = sequencer.patterns[keyPat].channels[chIdx];
            if (! pc.drawMode)
            {
                pc.drawMode = true;
                pc.clearDrawNotes();
                for (auto& oi : keysHeldOpenIdx) oi = -1;
                keysDrawLastCol.store(-1, std::memory_order_relaxed);
            }
        }
        // DRAW mode records into a free unquantized pitch LANE (no steps): the per-block writer below
        // stamps the held note's semitone into the columns as the playhead moves. The step-based
        // stamping / merge / take-boundary logic is skipped for a draw channel.
        const bool drawRec = sequencer.patterns[keyPat].channels[chIdx].drawMode;
        // While recording a piano-roll channel, MUTE its sequenced notes (the live keys monitor it) -
        // playback re-firing the half-recorded note cut the held voice = blips/abrupt cuts.
        // NOT gated on isCurrentlyPlaying: that flag only turns true inside the sequencer's own
        // processBlock, so the "key starts recording" FIRST block slipped through un-suppressed and
        // the just-opened note's trigger cut the first live key (the "first key is silent" bug).
        sequencer.recordSuppressCh.store((rec && drawRec) ? chIdx : -1, std::memory_order_relaxed);
        // "This pattern only" modes (0/1) LOCK playback onto the armed pattern/group while recording -
        // the chain must not pull the transport away from the take (chain modes 2/3 follow it).
        sequencer.recordLoopLock.store(rec && keysRecMode.load(std::memory_order_relaxed) < 2,
                                       std::memory_order_relaxed);
        auto logEvt = [this](int pat, int st, int semis, int flags) {
            const int cnt = keysEvtCount.load(std::memory_order_relaxed);
            if (cnt < KEYS_EVT_CAP)
            {
                keysEvts[cnt] = { (uint8_t) pat, (uint8_t) st, (int8_t) semis, (uint8_t) flags };
                keysEvtCount.store(cnt + 1, std::memory_order_release);
            }
        };

        // Take boundary = EVERY pattern loop AND every chain pattern-switch: close the take and
        // restart the (newly) playing pattern's channel CLEAN, so each pass is its own fresh take
        // (chain example: pattern 1 x2 then pattern 2 x5 = 2 takes for p1 + 5 takes for p2).
        if (rec && sequencer.isCurrentlyPlaying)
        {
            const int lc  = sequencer.loopCount;
            const int pp2 = sequencer.playPattern;
            const int seenL = keysLoopSeen.load(std::memory_order_relaxed);
            const int seenP = keysLastPlayPat.load(std::memory_order_relaxed);
            if (seenL < 0)
            { keysLoopSeen.store(lc, std::memory_order_relaxed); keysLastPlayPat.store(pp2, std::memory_order_relaxed); }
            else if (lc != seenL || pp2 != seenP)
            {
                keysLoopSeen.store(lc, std::memory_order_relaxed);
                keysLastPlayPat.store(pp2, std::memory_order_relaxed);
                const int armed = keysArmedPattern.load(std::memory_order_relaxed);
                if (chain || pp2 == armed || seenP == armed)          // a boundary we record across
                {
                    if (drawRec)
                    {
                        const bool sameGroup = sequencer.inGroup(pp2)
                                               && sequencer.groupHead(pp2) == sequencer.groupHead(seenP);
                        if (sameGroup && pp2 == seenP + 1)
                        {
                            // INTERNAL bar advance of a merged group: nothing to do - held notes keep
                            // growing across the bar line (one continuous note).
                        }
                        else if (sameGroup)
                        {
                            // GROUP WRAP (back to the head): the finished PASS is ONE take. Snapshot
                            // EVERY bar's notes in CONCAT columns (start += bar*384, drawPat = head),
                            // clear ALL bars for the fresh pass, and re-open still-held keys at col 0.
                            const int gh = sequencer.groupHead(pp2), ge = sequencer.groupEnd(pp2);
                            if (! keysDrawTakeReady.load(std::memory_order_acquire))
                            {
                                int nn = 0;
                                for (int b = gh; b <= ge; ++b)
                                {
                                    const auto& rch = sequencer.patterns[b].channels[chIdx];
                                    const int nc = juce::jlimit(0, DrumChannel::DRAW_MAX_NOTES, rch.drawNoteCount);
                                    for (int i = 0; i < nc && nn < DRAW_TAKE_MAX; ++i)
                                    {
                                        keysDrawTakeNotes[nn] = rch.drawNotes[i];
                                        keysDrawTakeNotes[nn].start = (int16_t) (keysDrawTakeNotes[nn].start
                                                                                 + (b - gh) * DrumChannel::DRAW_RES);
                                        ++nn;
                                    }
                                }
                                keysDrawTakeCount = nn;
                                if (nn > 0) { keysDrawTakeChan.store(chIdx, std::memory_order_relaxed);
                                              keysDrawTakePat.store(gh, std::memory_order_relaxed);
                                              keysDrawTakeReady.store(true, std::memory_order_release); }
                            }
                            for (int i = 0; i < keysHeldCount; ++i) keysHeldOpenIdx[i] = -1;   // old handles die with the clear
                            for (int b = gh; b <= ge; ++b) sequencer.patterns[b].channels[chIdx].clearDrawNotes();
                            auto& cch = sequencer.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, pp2)].channels[chIdx];
                            for (int i = 0; i < keysHeldCount; ++i)
                            {
                                keysHeldOpenIdx[i] = cch.addDrawNote(0, 1, keysHeldStack[i] - 60,
                                                                     (int) std::lround(keysHeldStackVel[i] * 255.0f));
                                keysHeldOpenPat[i] = pp2;
                            }
                            keysDrawLastCol.store(-1, std::memory_order_relaxed);
                        }
                        else
                        {
                            // SINGLE pattern (or leaving the unit): classic per-pass takes. Snapshot the
                            // finished pass, clear for the next one, re-open still-held keys at column 0.
                            const int pRec   = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, chain ? seenP : armed);
                            const int pClear = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, chain ? pp2   : armed);
                            auto& rch = sequencer.patterns[pRec].channels[chIdx];
                            if (! keysDrawTakeReady.load(std::memory_order_acquire))
                            {
                                const int nc = juce::jlimit(0, DrumChannel::DRAW_MAX_NOTES, rch.drawNoteCount);
                                for (int i = 0; i < nc; ++i) keysDrawTakeNotes[i] = rch.drawNotes[i];
                                keysDrawTakeCount = nc;
                                if (nc > 0) { keysDrawTakeChan.store(chIdx, std::memory_order_relaxed);
                                              keysDrawTakePat.store(pRec, std::memory_order_relaxed);
                                              keysDrawTakeReady.store(true, std::memory_order_release); }
                            }
                            auto& cch = sequencer.patterns[pClear].channels[chIdx];
                            for (int i = 0; i < keysHeldCount; ++i)   // handles into the cleared pattern die here
                                if (keysHeldOpenPat[i] == pClear) keysHeldOpenIdx[i] = -1;
                            cch.clearDrawNotes();
                            for (int i = 0; i < keysHeldCount; ++i)   // still-held keys start fresh notes at col 0
                            {
                                keysHeldOpenIdx[i] = cch.addDrawNote(0, 1, keysHeldStack[i] - 60,
                                                                     (int) std::lround(keysHeldStackVel[i] * 255.0f));
                                keysHeldOpenPat[i] = pClear;
                            }
                            keysDrawLastCol.store(-1, std::memory_order_relaxed);
                        }
                    }
                    else
                    {
                        const int cnt = keysEvtCount.load(std::memory_order_relaxed);
                        if (cnt > 0 && keysEvts[cnt - 1].pattern != 0xFF)     // only close takes that have content
                            logEvt(0xFF, 0, 0, 0);                            // take boundary marker
                        const int clearPat = chain ? pp2 : armed;             // the pattern this pass records into
                        if (chain || pp2 == armed)
                        {
                            auto& pch = sequencer.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, clearPat)].channels[chIdx];
                            for (int i2 = 0; i2 < DrumChannel::MAX_STEPS; ++i2)   // fresh take: clean slate
                            { pch.steps[i2] = false; pch.stepPitch[i2] = 0.0f; pch.stepMerge[i2] = false; pch.stepNoteLen[i2] = 0.0f; }
                        }
                    }
                    keysLastStampStep.store(-1, std::memory_order_relaxed);
                }
            }
        }

        // ---- POLY key-event handling: drain EVERY pending press/release this block, in order ----
        // (the old single-atomic handshake could only carry one down + one up per block, so chord
        // presses landing together lost all but the last note.)
        auto updateHeldMask = [&] {   // atomic mirror of the held stack, for the UI highlight
            uint64_t lo = 0, hi = 0;
            const bool poly = sequencer.patterns[keyPat].channels[chIdx].keysPolyMode;
            if (poly)
                for (int i = 0; i < keysHeldCount; ++i)
                { const int n = keysHeldStack[i] & 0x7f; (n < 64 ? lo : hi) |= 1ULL << (n & 63); }
            else if (keysHeldCount > 0)   // MONO: only the SOUNDING note lights up (older held keys are silent)
            { const int n = keysHeldStack[keysHeldCount - 1] & 0x7f; (n < 64 ? lo : hi) |= 1ULL << (n & 63); }
            keysHeldMaskLo.store(lo, std::memory_order_relaxed);
            keysHeldMaskHi.store(hi, std::memory_order_relaxed);
        };
        // ===== ARP (keyboard riff generator) =====================================================
        // fireArp(step): play step `step` of the arp (0 = root, 1.. = the offset rows). A REST row
        // silences the current note; else keyDown the (root + offset) note (mono -> cuts the previous,
        // and glide/chord/scale apply since it's the normal key path). While recording a piano-roll
        // channel it also stamps the note so the arp is captured.
        auto& arpKc = sequencer.patterns[keyPat].channels[chIdx];
        bool arpKicked = false;   // this key just STARTED the transport (isCurrentlyPlaying turns true next block)
        auto fireArp = [&](int step)
        {
            const int note = DrumChannel::arpNoteAt(arpKc.arpOffset, arpKc.arpLen, arpRoot, step);
            if (note < 0)   // rest row: silence whatever is ringing, play nothing
            { if (arpSounding >= 0) { arpKc.keyUp(arpSounding); arpSounding = -1; }
              arpSoundingUi.store(-1, std::memory_order_relaxed); return; }
            arpKc.keyDown(note, arpVel, arpKc.keysSlot2Down, false);   // mono arp note
            arpSounding = note;
            arpSoundingUi.store(note, std::memory_order_relaxed);      // the keyboard highlight follows this live
            if (rec && drawRec && (sequencer.isCurrentlyPlaying || arpKicked))   // capture the arp into the piano roll
            {
                const int colLen = juce::jmax(1, (int) ((double) DrumChannel::DRAW_RES
                                                        / ((double) juce::jmax(1, arpKc.arpSync) * DrumChannel::arpRateMul(arpKc.arpRate))));
                const int col = arpKicked ? 0 : juce::jlimit(0, DrumChannel::DRAW_RES - 1, (int) (sequencer.barPos() * DrumChannel::DRAW_RES));
                sequencer.patterns[sequencer.playPattern].channels[chIdx].addDrawNote(col, colLen, note - 60,
                                                                                      (int) std::lround(arpVel * 255.0f));
            }
        };
        auto startArp = [&](int note, float vel)
        {
            arpRoot = note; arpVel = vel; arpChan = chIdx; arpStep = 0; arpAcc = 0.0; arpSounding = -1;
            keysHeldNote.store(note, std::memory_order_relaxed); keysHeldVel.store(vel, std::memory_order_relaxed);
            const int n = note & 0x7f;   // light the root in the held mask so the keyboard highlight shows the arp keys
            keysHeldMaskLo.store(n < 64 ? (1ULL << (n & 63)) : 0ULL, std::memory_order_relaxed);
            keysHeldMaskHi.store(n >= 64 ? (1ULL << (n & 63)) : 0ULL, std::memory_order_relaxed);
            fireArp(0);
        };
        auto stopArp = [&]()
        {
            if (arpSounding >= 0 && arpChan >= 0) sequencer.patterns[keyPat].channels[arpChan].keyUp(arpSounding);
            arpRoot = -1; arpSounding = -1; arpChan = -1;
            arpSoundingUi.store(-1, std::memory_order_relaxed);
            keysHeldNote.store(-1, std::memory_order_relaxed);
            keysHeldMaskLo.store(0, std::memory_order_relaxed); keysHeldMaskHi.store(0, std::memory_order_relaxed);
        };

        auto handleKeyDown = [&](int note, float vel)
        {
            if (arpKc.arpOn)   // ARP owns the note: it generates the riff from this root
            {
                // "First key starts recording" works for the arp too: kick the transport (own transport
                // only), and let fireArp stamp the kicked note at column 0 (isCurrentlyPlaying only turns
                // true NEXT block - same regression the normal path fixed with its `kicked` flag).
                if (rec && ! sequencer.isCurrentlyPlaying && ! sequencer.dawSync)
                { sequencer.startStandalone(); arpKicked = true; }
                const float mv = juce::jlimit(0.0f, 1.0f, arpKc.keysMinVel);
                const float xv = juce::jmax(mv, juce::jlimit(0.0f, 1.0f, arpKc.keysMaxVel));
                startArp(note, juce::jlimit(0.05f, 1.0f, mv + vel * (xv - mv)));
                arpKicked = false;
                return;
            }
            bool kicked = false;   // "key starts recording": isCurrentlyPlaying only turns true on the
                                   // NEXT block, so the FIRST key must record via this flag (regression fix)
            if (rec)
            {
                // "start recording with the first key press": kick the transport if it's stopped
                // (own transport only - with DAW Sync the host owns play).
                if (! sequencer.isCurrentlyPlaying && ! sequencer.dawSync)
                { sequencer.startStandalone(); kicked = true; }
                const int pat = sequencer.playPattern;
                if (! drawRec && (chain || pat == keysArmedPattern.load(std::memory_order_relaxed)))
                {
                    const int semis = note - 60;   // FIXED reference: C3 = 0 (piano range = +/-36)
                    auto& pch = sequencer.patterns[pat].channels[chIdx];
                    const int n  = juce::jmax(1, pch.numSteps);
                    const int st = ((int) std::lround(sequencer.barPos() * n)) % n;
                    logEvt(pat, st, semis, 0);
                    pch.steps[st] = true; pch.stepPitch[st] = (float) semis; pch.stepMerge[st] = false;
                    keysLastStampStep.store(st, std::memory_order_relaxed);
                }
            }
            auto& kc = sequencer.patterns[keyPat].channels[chIdx];
            // MIN/MAX VELOCITY: remap [0..1] -> [keysMinVel..keysMaxVel] (soft still sounds, loud is tamed).
            const float mv = juce::jlimit(0.0f, 1.0f, kc.keysMinVel);
            const float xv = juce::jmax(mv, juce::jlimit(0.0f, 1.0f, kc.keysMaxVel));
            const float kvel = juce::jlimit(0.05f, 1.0f, mv + vel * (xv - mv));
            kc.keyDown(note, kvel, kc.keysSlot2Down, kc.keysPolyMode);   // poly = stack; mono = cut (as before)
            // Held stack: a re-press moves the note to the top (most recent). openIdx/Pat ride along.
            for (int i = 0; i < keysHeldCount; ++i)
                if (keysHeldStack[i] == note)
                { for (int j = i; j < keysHeldCount - 1; ++j) { keysHeldStack[j] = keysHeldStack[j + 1]; keysHeldStackVel[j] = keysHeldStackVel[j + 1]; keysHeldOpenIdx[j] = keysHeldOpenIdx[j + 1]; keysHeldOpenPat[j] = keysHeldOpenPat[j + 1]; }
                  --keysHeldCount; break; }
            if (keysHeldCount < (int) (sizeof(keysHeldStack) / sizeof(keysHeldStack[0])))
            {
                keysHeldStack[keysHeldCount] = note; keysHeldStackVel[keysHeldCount] = kvel;
                // PIANO-ROLL recording: this press OPENS a note at the current column (grown per
                // block below, closed at release) - POLY records real chords now. `kicked` = this key
                // just started the transport, so it opens at column 0 (the bar is about to begin).
                int openIdx = -1, openPat = -1;
                if (rec && drawRec && (sequencer.isCurrentlyPlaying || kicked))
                {
                    openPat = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, sequencer.playPattern);
                    auto& pch = sequencer.patterns[openPat].channels[chIdx];
                    const int cur = kicked ? 0 : juce::jlimit(0, DrumChannel::DRAW_RES - 1,
                                                              (int) (sequencer.barPos() * DrumChannel::DRAW_RES));
                    openIdx = pch.addDrawNote(cur, 1, note - 60, (int) std::lround(kvel * 255.0f));
                    // Legato + mono + Glide up: mark this note to SLIDE from the note already held (so a
                    // recorded portamento performance reproduces; editable by hand in the piano roll).
                    if (openIdx >= 0 && keysHeldCount > 0 && ! pch.keysPolyMode && pch.keysGlide > 0.0001f)
                        pch.drawNotes[openIdx].glide = 1;
                }
                keysHeldOpenIdx[keysHeldCount] = openIdx;
                keysHeldOpenPat[keysHeldCount] = openPat;
                ++keysHeldCount;
            }
            keysHeldNote.store(note, std::memory_order_relaxed);   // mono projection = most recent press
            keysHeldVel.store(kvel, std::memory_order_relaxed);
            updateHeldMask();
        };
        auto handleKeyUp = [&](int note)
        {
            if (arpKc.arpOn) { if (note == arpRoot) stopArp(); return; }   // release the root -> stop the arp
            auto& kc = sequencer.patterns[keyPat].channels[chIdx];
            // Remove from the held stack wherever it sits (poly releases arrive in any order),
            // capturing this key's OPEN piano-roll note (if recording) to close it below.
            bool wasHeld = false; int openIdx = -1, openPat = -1;
            for (int i = 0; i < keysHeldCount; ++i)
                if (keysHeldStack[i] == note)
                { wasHeld = true; openIdx = keysHeldOpenIdx[i]; openPat = keysHeldOpenPat[i];
                  for (int j = i; j < keysHeldCount - 1; ++j) { keysHeldStack[j] = keysHeldStack[j + 1]; keysHeldStackVel[j] = keysHeldStackVel[j + 1]; keysHeldOpenIdx[j] = keysHeldOpenIdx[j + 1]; keysHeldOpenPat[j] = keysHeldOpenPat[j + 1]; }
                  --keysHeldCount; break; }
            updateHeldMask();
            // CLOSE the released key's recorded note: final length = up to the current column. In a
            // merged group the note may have GROWN across bar lines (len spans bars).
            if (openIdx >= 0 && openPat >= 0 && rec && drawRec && sequencer.isCurrentlyPlaying)
            {
                auto& pch = sequencer.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, openPat)].channels[chIdx];
                const int barDelta = sequencer.playPattern - openPat;
                if (openIdx < pch.drawNoteCount
                    && sequencer.groupHead(openPat) == sequencer.groupHead(sequencer.playPattern) && barDelta >= 0)
                {
                    auto& nt = pch.drawNotes[openIdx];
                    const int cur = barDelta * DrumChannel::DRAW_RES
                                  + juce::jlimit(0, DrumChannel::DRAW_RES - 1,
                                                 (int) (sequencer.barPos() * DrumChannel::DRAW_RES));
                    if (cur >= nt.start) nt.len = (int16_t) juce::jlimit(1, DrumChannel::DRAW_RES * 8, cur - nt.start + 1);
                }
            }
            // MONO slide safety (unchanged rule): a stale up (released note != the held one) does
            // nothing - the panel emits up(old) before down(new) and the block processes down first.
            if (! kc.keysPolyMode && note != keysHeldNote.load(std::memory_order_relaxed)) return;
            if (kc.keysPolyMode && ! wasHeld) return;   // stale poly up: nothing to release
            if (note == keysHeldNote.load(std::memory_order_relaxed))
            {
                // RELEASE captures the exact HOLD into the step data: the chain head's Note Length =
                // the fraction of the chain you actually held (so a 2.4-step hold plays back as a
                // 2.4-step gate, not 2.0 - the sequencer reproduces the performance without the keys).
                const int last = keysLastStampStep.load(std::memory_order_relaxed);
                if (rec && ! drawRec && last >= 0 && sequencer.isCurrentlyPlaying)
                {
                    const int pat = sequencer.playPattern;
                    if (chain || pat == keysArmedPattern.load(std::memory_order_relaxed))
                    {
                        auto& pch = sequencer.patterns[pat].channels[chIdx];
                        const int n = juce::jmax(1, pch.numSteps);
                        int head = last; while (head > 0 && pch.stepMerge[head]) --head;
                        const double posSteps = sequencer.barPos() * n;
                        double frac = posSteps - std::floor(posSteps);
                        if ((((int) posSteps) % n) != last) frac = 1.0;   // playhead already left the last step
                        const int chainLen = juce::jmax(1, last - head + 1);
                        const float len = (float) juce::jlimit(0.05, 1.0, ((double)(chainLen - 1) + frac) / (double) chainLen);
                        pch.stepNoteLen[head] = len;
                        logEvt(pat, head, (int) std::lround(len * 100.0f), 2);   // flags bit1 = LENGTH event
                    }
                }
                // Mono projection falls back to the most recent STILL-HELD note (poly), else clears.
                const int nh = keysHeldCount > 0 ? keysHeldStack[keysHeldCount - 1] : -1;
                keysHeldNote.store(nh, std::memory_order_relaxed);
                if (nh >= 0) keysHeldVel.store(keysHeldStackVel[keysHeldCount - 1], std::memory_order_relaxed);
                keysLastStampStep.store(-1, std::memory_order_relaxed);
            }
            // Release this note's voices WHEREVER they live: the voice was created on the channel of
            // the bar that was playing AT PRESS TIME - if the bar advanced mid-hold, releasing only
            // the current bar's channel left the old voice keyed-on forever ("sound after I lift").
            // keyUp(note) is a cheap 16-voice scan, so sweep every pattern's channel.
            for (auto& pat2 : sequencer.patterns)
                pat2.channels[chIdx].keyUp(note);
        };
        // Drain the message-thread ring (SPSC: panel presses), then this block's incoming-MIDI notes.
        for (uint32_t t = keyQTail.load(std::memory_order_relaxed);
             t != keyQHead.load(std::memory_order_acquire); ++t)
        {
            const KeyQEvt e = keyQ[t % KEYQ];
            if (e.down) handleKeyDown((int) e.note, (float) e.vel / 127.0f); else handleKeyUp((int) e.note);
            keyQTail.store(t + 1, std::memory_order_release);
        }
        for (int i = 0; i < nMidiKeyEvts; ++i)
        {
            const KeyQEvt& e = midiKeyEvts[i];
            if (e.down) handleKeyDown((int) e.note, (float) e.vel / 127.0f); else handleKeyUp((int) e.note);
        }

        // ARP CLOCK: while a key is held on an arp channel, fire the next note every time the computed
        // step time elapses. The rate is derived from bpm + time-sig + the channel's step count (so it
        // runs whether the transport plays or not); the phase started at the keypress ("from the top").
        if (arpRoot >= 0 && arpChan == chIdx && arpKc.arpOn)
        {
            const double barSec = (60.0 / juce::jmax(1.0, currentBpm)) * juce::jmax(1, currentTimeSigNum)
                                  * (4.0 / juce::jmax(1, currentTimeSigDen));
            const double noteSec = barSec / ((double) juce::jmax(1, arpKc.arpSync)   // the arp's OWN grid (Notes/bar fader)
                                             * DrumChannel::arpRateMul(arpKc.arpRate));   // x the Rate multiplier
            const double noteSamp = juce::jmax(1.0, noteSec * currentSampleRate);
            arpAcc += (double) numSamples;
            int guard = 0;
            while (arpAcc >= noteSamp && guard++ < 128)
            {
                arpAcc -= noteSamp;
                arpStep = (arpStep + 1) % juce::jmax(1, arpKc.arpLen);
                fireArp(arpStep);
            }
        }
        else if (arpRoot >= 0 && ! arpKc.arpOn) stopArp();   // arp switched off while a key was held

        // AUTO-MERGE: the key is still held when the playhead enters the NEXT step -> that step
        // becomes a merge-continuation of the stamped note (recorded exactly like you played it).
        const int held = keysHeldNote.load(std::memory_order_relaxed);
        if (rec && ! drawRec && held >= 0 && sequencer.isCurrentlyPlaying)
        {
            const int pat = sequencer.playPattern;
            if (chain || pat == keysArmedPattern.load(std::memory_order_relaxed))
            {
                auto& pch = sequencer.patterns[pat].channels[chIdx];
                const int n    = juce::jmax(1, pch.numSteps);
                const int cur  = ((int)(sequencer.barPos() * n)) % n;   // the step we're IN (floor)
                const int last = keysLastStampStep.load(std::memory_order_relaxed);
                if (last >= 0 && cur != last && cur == (last + 1) % n && cur != 0)   // no wrap-merge across the loop
                {
                    logEvt(pat, cur, held - 60, 1);                     // flags bit0 = merge
                    pch.steps[cur] = true; pch.stepPitch[cur] = (float)(held - 60); pch.stepMerge[cur] = true;
                    keysLastStampStep.store(cur, std::memory_order_relaxed);
                }
            }
        }

        // PIANO-ROLL recording: GROW every held key's OPEN note to the column the playhead reached
        // this block. In a MERGED GROUP a note opened in an earlier bar keeps growing ACROSS the bar
        // line (len spans bars - one continuous note, no close/reopen at the boundary).
        if (rec && drawRec && sequencer.isCurrentlyPlaying)
        {
            juce::ignoreUnused(held);
            const int R = DrumChannel::DRAW_RES;
            const int cur = juce::jlimit(0, R - 1, (int) (sequencer.barPos() * R));
            for (int i = 0; i < keysHeldCount; ++i)
            {
                const int oi = keysHeldOpenIdx[i], op = keysHeldOpenPat[i];
                if (oi < 0 || op < 0) continue;
                if (sequencer.groupHead(op) != sequencer.groupHead(sequencer.playPattern)) continue;   // left the unit
                const int barDelta = sequencer.playPattern - op;
                if (barDelta < 0) { keysHeldOpenIdx[i] = -1; continue; }   // wrapped: the boundary reopened it
                auto& pch = sequencer.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, op)].channels[chIdx];
                if (oi >= pch.drawNoteCount) continue;
                auto& nt = pch.drawNotes[oi];
                const int curAbs = barDelta * R + cur;
                if (curAbs >= nt.start)
                    nt.len = (int16_t) juce::jlimit(1, R * 8, curAbs - nt.start + 1);
            }
            keysDrawLastCol.store(cur, std::memory_order_relaxed);
        }
    }

    //-- Stop pressed: cut any ringing tails on every channel of every pattern.
    if (silenceRequest.exchange(false))
        for (auto& pat : sequencer.patterns)
            for (auto& ch : pat.channels)
                ch.silenceAllVoices();

    //-- Run sequencer at the OVERSAMPLED rate, then down-sample. The engine renders into the
    //   oversampler's up-block (kEngineOS x); channels route to Main or an aux bus inside it.
    updateAnySolo();
    juce::dsp::AudioBlock<float> hostBlock(audio);
    auto osBlock = engineOS->processSamplesUp(hostBlock);   // nCh x (numSamples*OS)
    osBlock.clear();                                        // render from silence (ignore up-filter state)
    const int nOS = (int) osBlock.getNumSamples();

    auto busView = [&](int busIdx) -> juce::AudioBuffer<float> {
        const int off = getChannelIndexInProcessBlockBuffer(false, busIdx, 0);
        float* p[2] = { osBlock.getChannelPointer((size_t) off),
                        osBlock.getChannelPointer((size_t) juce::jmin((int) osBlock.getNumChannels() - 1, off + 1)) };
        return juce::AudioBuffer<float>(p, 2, nOS);
    };
    juce::AudioBuffer<float>  osMain = busView(0);
    juce::AudioBuffer<float>  auxViews[NUM_AUX_OUTS];
    juce::AudioBuffer<float>* auxPtrs[NUM_AUX_OUTS] = {};
    const int busCount = getBusCount(false);
    for (int i = 0; i < NUM_AUX_OUTS; ++i)
    {
        const int busIdx = i + 1;
        if (busIdx < busCount && getBus(false, busIdx)->isEnabled())
        {
            auxViews[i] = busView(busIdx);
            auxPtrs[i]  = &auxViews[i];
        }
    }
    reverbSendOS.clear(); delaySendOS.clear();   // per-channel send sums accumulate here (OS rate)
    // (The sequencer computes each rendered pattern's OWN anySolo internally - passing the
    //  VIEWED pattern's used to silence the whole playing pattern when view != playback.)
    auto events = sequencer.processBlock(osMain, currentSampleRate * kEngineOS, nOS,
                                          getPlayHead(), auxPtrs, NUM_AUX_OUTS,
                                          &reverbSendOS, &delaySendOS);
    engineOS->processSamplesDown(hostBlock);   // -> `audio` at the host rate


    // Average-downsample the send buses to the host rate (cheap; reverb/delay are latency-tolerant).
    {
        auto downAvg = [numSamples](juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& dst)
        {
            const float inv = 1.0f / (float) kEngineOS;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* s = src.getReadPointer(ch);
                float* d = dst.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    float acc = 0.0f;
                    for (int k = 0; k < kEngineOS; ++k) acc += s[i * kEngineOS + k];
                    d[i] = acc * inv;
                }
            }
        };
        downAvg(reverbSendOS, reverbSendBase);
        downAvg(delaySendOS,  delaySendBase);
    }

    //-- Emit MIDI for MIDI-out channels (they sequence other plugins). A note is HELD for its
    //   PER-STEP length (set in Vel/Len mode), via a per-channel countdown that fires the note-off in
    //   a later block. A new hit cuts the held note (retrigger); transport-stop or a routing change
    //   flushes everything so nothing hangs in the target plugin.
    {
        // MIDI channel is now per drum-channel (ch.midiOutChannel); activeMidiChan[] remembers which channel a
        // held note went out on so its note-off matches even if the routing changes mid-note.
        bool triggered[Sequencer::NUM_CHANNELS] = {};

        // One step's duration in samples = bar / steps. stepNoteLen: 0 = one full step (default),
        // else the fraction of a step (the same per-step Length that gates internal sounds).
        const double qpb = juce::jmax(1, currentTimeSigNum) * 4.0 / juce::jmax(1, currentTimeSigDen);
        const double barSamples = (qpb * 60.0 / juce::jmax(1.0, currentBpm)) * currentSampleRate;

        for (auto& e : events)
        {
            auto& ch = sequencer.patterns[sequencer.playPattern].channels[e.channel];
            if (! ch.midiOut) continue;
            const int midiCh = juce::jlimit(1, 16, ch.midiOutChannel);   // per-channel MIDI out channel
            const int note  = juce::jlimit(0, 127, ch.midiNote + juce::roundToInt(ch.stepPitch[e.step]));
            const float v   = juce::jlimit(0.0f, 1.0f, ch.stepVel[e.step] * e.velScale);
            const auto  vel = (juce::uint8) juce::jlimit(1, 127, juce::roundToInt(v * 127.0f));
            // SAMPLE-ACCURATE placement: the event offset is at the engine (OS) rate -> host rate.
            const int hostOff = juce::jlimit(0, juce::jmax(0, numSamples - 1), e.offset / kEngineOS);

            if (activeMidiNote[e.channel] >= 0)                          // retrigger -> cut held note (on its own channel)
                midi.addEvent(juce::MidiMessage::noteOff(activeMidiChan[e.channel], activeMidiNote[e.channel]), hostOff);

            midi.addEvent(juce::MidiMessage::noteOn(midiCh, note, vel), hostOff);
            activeMidiChan[e.channel] = midiCh;
            const double samplesPerStep = barSamples / juce::jmax(1, ch.numSteps);
            const float  gl = juce::jlimit(0.0f, 1.0f, ch.stepNoteLen[e.step]);
            const double lenSteps = gl > 0.001f ? (double) gl : 1.0;   // 0 = a full step; else fraction of a step
            const int len = juce::jmax(1, (int)(lenSteps * samplesPerStep));
            if (hostOff + len < numSamples) { midi.addEvent(juce::MidiMessage::noteOff(midiCh, note), hostOff + len); activeMidiNote[e.channel] = -1; }
            else                            { activeMidiNote[e.channel] = note; midiNoteCountdown[e.channel] = len - (numSamples - hostOff); }
            triggered[e.channel] = true;
        }

        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            if (triggered[c] || activeMidiNote[c] < 0) continue;
            const bool stillMidi = sequencer.patterns[sequencer.playPattern].channels[c].midiOut
                                   && sequencer.isCurrentlyPlaying;
            if (! stillMidi || midiNoteCountdown[c] <= numSamples)
            {
                midi.addEvent(juce::MidiMessage::noteOff(activeMidiChan[c], activeMidiNote[c]),
                              stillMidi ? juce::jmax(0, midiNoteCountdown[c]) : 0);
                activeMidiNote[c] = -1;
            }
            else midiNoteCountdown[c] -= numSamples;
        }
    }

    //-- Track tempo for tempo-synced delay. Follow the host tempo only when the
    //   sequencer is DAW-synced; otherwise use the plugin's own (standalone) BPM.
    if (sequencer.dawSync)
    {
        if (auto* ph = getPlayHead())
        {
            if (const auto pos = ph->getPosition())
            {
                if (const auto bpm = pos->getBpm(); bpm.hasValue() && *bpm > 1.0) currentBpm = *bpm;
                if (const auto ts = pos->getTimeSignature(); ts.hasValue())
                {
                    if (ts->numerator   > 0) currentTimeSigNum = ts->numerator;
                    if (ts->denominator > 0) currentTimeSigDen = ts->denominator;
                }
            }
        }
    }
    else
    {
        currentBpm        = sequencer.standaloneBpm;
        currentTimeSigNum = sequencer.timeSigNum;
        currentTimeSigDen = sequencer.timeSigDen;
    }

    //-- Scrub the FX sends BEFORE the delay/reverb: a channel glitch must never enter a feedback line
    //   (it would get trapped + echo forever = the "gunshot that echoes"). Non-finite -> 0, clamp ±4.
    for (auto* buf : { &delaySendBase, &reverbSendBase })
        for (int ch = 0; ch < buf->getNumChannels(); ++ch)
        {
            float* d = buf->getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = std::isfinite(d[i]) ? juce::jlimit(-64.0f, 64.0f, d[i]) : 0.0f;  // only block NaN/absurd into the FX
        }

    //-- Process shared reverb + delay
    processDelay(audio, delaySendBase, numSamples);
    processReverb(audio, reverbSendBase, numSamples);

    //-- Master output stage (final): volume, pan, optional mono, safety limiter
    {
        auto& mfx = masterFX();                       // this pattern's master settings
        const float gL = mfx.volume * (mfx.pan <= 0.0f ? 1.0f : 1.0f - mfx.pan);
        const float gR = mfx.volume * (mfx.pan >= 0.0f ? 1.0f : 1.0f + mfx.pan);
        const bool  mono = mfx.mono;

        // Limiter is OFF at 0; above that it sets a brick-wall ceiling that drops
        // from just below 0 dBFS down to about -12 dB. Even a tiny amount stops a
        // loud EQ/volume boost from making the DAW mute or clip.
        const bool  limOn     = mfx.limit > 0.0001f;
        const float thrDb     = -0.1f - mfx.limit * 11.9f;   // ceiling: lightest -0.1 dB .. heaviest -12 dB
        const float threshold = limOn ? std::pow(10.0f, thrDb / 20.0f) : 1.0f;
        const float atkCoef   = std::exp(-1.0f / (0.001f * (float) currentSampleRate)); // ~1 ms
        const float relCoef   = std::exp(-1.0f / (0.060f * (float) currentSampleRate)); // ~60 ms

        // GLUE = a master bus compressor BEFORE the limiter (musical, single knob). It reacts to the whole
        // mix together (stereo-LINKED detector = same gain on L+R, stable image), with drum-friendly fixed
        // character: medium attack so transients survive, tempo-ish release so it "pumps". The knob lowers the
        // threshold + raises make-up together. The limiter/soft-clip after it still catch any peaks.
        // TILT EQ + SATURATION (drum + bass master colour). Signal order: Tilt -> Sat -> Glue -> Limiter.
        // Tilt = one-knob spectral balance around a ~700 Hz pivot: a one-pole splits low/high bands, then
        // recombines with COMPLEMENTARY gains (+/-6 dB). 0.5 = both gains 1.0 = bit-identical.
        const float tiltT     = (mfx.tilt - 0.5f) * 2.0f;                 // -1 (dark) .. +1 (bright)
        const bool  tiltOn    = std::abs(tiltT) > 0.001f;
        const float tiltDb    = tiltT * 6.0f;
        const float tiltHiG   = std::pow(10.0f,  tiltDb / 20.0f);         // bright: high up / low down
        const float tiltLoG   = std::pow(10.0f, -tiltDb / 20.0f);
        const float tiltK     = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 700.0f / (float) currentSampleRate);
        // Sat = ASYMMETRIC tube-style saturation. Unlike Glue (a compressor that reduces dynamics +
        // pumps), Sat adds HARMONICS - and the asymmetry (positive half shaped harder than the
        // negative) generates EVEN harmonics = a warm, "fat", audibly coloured DRIVE, clearly not
        // compression. Unity small-signal (/drive) so quiet passages stay clean, crossfaded by the
        // amount (0 = dry = bit-identical), driven harder as the master gets louder (analog-ish).
        // A ~5 Hz DC blocker removes the asymmetric bias (won't touch bass).
        const bool  satOn     = mfx.sat > 0.0001f;
        const float satDrive  = 1.0f + mfx.sat * 5.0f;                    // 1 .. 6 (wide = clearly grittier at the top)
        const float satWet    = mfx.sat;
        const float satDcR    = 1.0f - 2.0f * juce::MathConstants<float>::pi * 5.0f / (float) currentSampleRate;
        auto tiltSat = [&](float& x, int chn) {
            if (tiltOn) { float& lp = (chn == 0) ? masterTiltL : masterTiltR;
                          lp += (x - lp) * tiltK; x = lp * tiltLoG + (x - lp) * tiltHiG; }
            if (satOn)  {
                const float y = x * satDrive;
                const float shaped = ((y >= 0.0f) ? std::tanh(y) : std::tanh(y * 0.85f)) / satDrive;  // asymmetric -> even harmonics
                const float wet = x + satWet * (shaped - x);
                float& xz = satDcX[chn]; float& yz = satDcY[chn];   // 1-pole DC blocker (kills the bias)
                const float o = wet - xz + satDcR * yz; xz = wet; yz = o; x = o;
            }
        };

        const bool  glueOn    = mfx.glue > 0.0001f;
        const float gThr      = std::pow(10.0f, (-8.0f - mfx.glue * 16.0f) / 20.0f);   // -8 dB .. -24 dB
        const float gRatio    = 1.0f + mfx.glue * 3.0f;                                // 1:1 .. 4:1
        const float gMakeup   = std::pow(10.0f, (mfx.glue * 7.0f) / 20.0f);            // up to +7 dB make-up
        const float gAtk      = std::exp(-1.0f / (0.015f * (float) currentSampleRate)); // ~15 ms attack
        const float gRel      = std::exp(-1.0f / (0.180f * (float) currentSampleRate)); // ~180 ms release (pump)
        auto glueGain = [&](float l, float r) -> float {                              // stereo-linked gain
            const float det = juce::jmax(std::abs(l), std::abs(r));
            masterGlueEnv = (det > masterGlueEnv) ? (gAtk * masterGlueEnv + (1.0f - gAtk) * det)
                                                  : (gRel * masterGlueEnv + (1.0f - gRel) * det);
            if (! std::isfinite(masterGlueEnv)) masterGlueEnv = 0.0f;
            float gr = 1.0f;
            if (masterGlueEnv > gThr) {                                                // above threshold -> compress
                const float overDb = 20.0f * std::log10(masterGlueEnv / gThr);
                gr = std::pow(10.0f, (overDb * (1.0f / gRatio - 1.0f)) / 20.0f);       // gain reduction (<1)
            }
            return gr * gMakeup;
        };

        if (audio.getNumChannels() >= 2)
        {
            auto* L = audio.getWritePointer(0);
            auto* R = audio.getWritePointer(1);
            float* dL = limiterLA.getWritePointer(0);
            float* dR = limiterLA.getWritePointer(1);
            const int N = limiterLAlen;
            for (int i = 0; i < numSamples; ++i)
            {
                float l = L[i], r = R[i];
                if (mono) { float m = 0.5f * (l + r); l = m; r = m; }
                l *= gL; r *= gR;

                tiltSat(l, 0); tiltSat(r, 1);   // master tone + saturation (pre-glue)
                if (glueOn) { const float gg = glueGain(l, r); l *= gg; r *= gg; }   // bus glue (pre-limiter)

                // Lookahead: output the DELAYED sample while the gain envelope tracks the INCOMING
                // (future) peak - so the gain is already dipped by the time the transient comes out.
                float ol = l, or_ = r;
                if (N > 0) { ol = dL[limiterLAhead]; or_ = dR[limiterLAhead]; dL[limiterLAhead] = l; dR[limiterLAhead] = r; limiterLAhead = (limiterLAhead + 1) % N; }

                if (limOn)
                {
                    const float peak   = juce::jmax(std::abs(l), std::abs(r));   // future peak
                    const float target = (peak > threshold) ? threshold / peak : 1.0f;
                    limiterGain = (target < limiterGain) ? (atkCoef * limiterGain + (1.0f - atkCoef) * target)
                                                         : (relCoef * limiterGain + (1.0f - relCoef) * target);
                    if (! std::isfinite(limiterGain)) limiterGain = 1.0f;   // never trap a NaN in the gain
                    ol *= limiterGain; or_ *= limiterGain;
                }
                else limiterGain = 1.0f;
                L[i] = ol; R[i] = or_;
            }
        }
        else if (audio.getNumChannels() == 1)
        {
            auto* M = audio.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
            {
                float m = M[i] * mfx.volume;
                tiltSat(m, 0);   // master tone + saturation (pre-glue)
                if (glueOn) { const float gg = glueGain(m, m); m *= gg; }   // bus glue (pre-limiter)
                if (limOn)
                {
                    const float peak   = std::abs(m);
                    const float target = (peak > threshold) ? threshold / peak : 1.0f;
                    limiterGain = (target < limiterGain) ? (atkCoef * limiterGain + (1.0f - atkCoef) * target)
                                                         : (relCoef * limiterGain + (1.0f - relCoef) * target);
                    if (! std::isfinite(limiterGain)) limiterGain = 1.0f;   // never trap a NaN in the gain
                    m *= limiterGain;
                }
                else limiterGain = 1.0f;
                M[i] = m;
            }
        }

        //-- FINAL SAFETY CEILING (replaces every threshold-based "de-glitcher" - those were whack-a-mole:
        //   too low = crackled on loud audio, too high = let spikes through "very loud"). We DON'T try to
        //   DETECT glitches anymore. Instead: NaN/Inf -> 0, then a soft-clip so NOTHING ever leaves above
        //   0 dBFS. Any stray spike (10, 50, whatever) is bounded to at most ONE full-scale sample (a faint
        //   tick, never a loud burst); legit signal below 0.85 is untouched; hot mixes saturate musically.
        //   This is the standard "you cannot exceed full-scale" master ceiling every plugin has.
        auto softclip = [](float x) -> float {
            if (! std::isfinite(x)) return 0.0f;
            constexpr float t = 0.85f;                                   // transparent below this
            if (x >  t) return  t + (1.0f - t) * std::tanh((x - t) / (1.0f - t));
            if (x < -t) return -t + (1.0f - t) * std::tanh((x + t) / (1.0f - t));
            return x;
        };
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            float* d = audio.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i) d[i] = softclip(d[i]);
        }

        //-- Master output meter (post everything): peak per channel for the UI master meter.
        if (audio.getNumChannels() >= 2)
        {
            const float* L = audio.getReadPointer(0);
            const float* R = audio.getReadPointer(1);
            float pl = 0.0f, pr = 0.0f;
            for (int i = 0; i < numSamples; ++i) { pl = juce::jmax(pl, std::abs(L[i])); pr = juce::jmax(pr, std::abs(R[i])); }
            // PEAK-HOLD (the UI resets these on read) - otherwise the 24Hz UI samples a random block of the decay.
            masterMeterL.store(juce::jmax(masterMeterL.load(std::memory_order_relaxed), pl), std::memory_order_relaxed);
            masterMeterR.store(juce::jmax(masterMeterR.load(std::memory_order_relaxed), pr), std::memory_order_relaxed);
        }
        else if (audio.getNumChannels() == 1)
        {
            const float* M = audio.getReadPointer(0);
            float pm = 0.0f;
            for (int i = 0; i < numSamples; ++i) pm = juce::jmax(pm, std::abs(M[i]));
            masterMeterL.store(juce::jmax(masterMeterL.load(std::memory_order_relaxed), pm), std::memory_order_relaxed);
            masterMeterR.store(juce::jmax(masterMeterR.load(std::memory_order_relaxed), pm), std::memory_order_relaxed);
        }
    }

}

void DrumSequencerProcessor::toggleStep(int channel, int step)
{
    if (channel < 0 || channel >= Sequencer::NUM_CHANNELS) return;
    if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
    auto& s = sequencer.channel(channel).steps[step];
    s = !s;
}

//==============================================================================
// Route an incoming CC to its learned parameter. paramId encodes which pattern
// (and channel/step) it targets, so a CC can affect a pattern that isn't current.
void DrumSequencerProcessor::routeCC(const juce::MidiMessage& msg)
{
    const int   cc   = msg.getControllerNumber();
    const int   mch  = msg.getChannel();
    const int   val  = msg.getControllerValue();
    const bool  on   = val >= 64;
    const float norm = val / 127.0f;

    juce::String pid = midiLearn.getParamForCC(cc, mch);
    if (pid.isEmpty()) return;

    // Global controls (play/stop are global; switching patterns won't stop them)
    if (pid == "global_play")       { if (on && !sequencer.dawSync) sequencer.startStandalone(); return; }
    if (pid == "global_stop")       { if (on && !sequencer.dawSync) { sequencer.stopStandalone(); silenceRequest.store(true); } return; }
    if (pid == "global_dawsync")    { if (on) sequencer.dawSync = !sequencer.dawSync;             return; }
    if (pid == "global_reverbRoom") { masterFX().reverbRoom  = norm;                  return; }
    if (pid == "global_reverbDecay"){ masterFX().reverbDamp  = 1.0f - norm;           return; }
    if (pid == "global_reverbWet")  { masterFX().reverbWet   = norm;                  return; }
    if (pid == "global_reverbPre")  { masterFX().reverbPreDelay = norm;               return; }
    if (pid == "global_reverbWidth"){ masterFX().reverbWidth    = norm;               return; }
    if (pid == "global_delayTime")  { masterFX().delayTime   = 0.05f + norm * 1.95f;  return; }
    if (pid == "global_delayFB")    { masterFX().delayFeedback = norm * 0.95f;        return; }
    if (pid == "global_delayWet")   { for (auto& pat : sequencer.patterns) pat.master.delayWet = norm; return; }
    if (pid == "global_masterGlue") { for (auto& pat : sequencer.patterns) pat.master.glue = norm; return; }
    if (pid == "global_masterTilt") { for (auto& pat : sequencer.patterns) pat.master.tilt = norm; return; }
    if (pid == "global_masterSat")  { for (auto& pat : sequencer.patterns) pat.master.sat  = norm; return; }

    // UI-only controls (edit-mode buttons + influence) -> relayed to the editor.
    if (pid == "ui_mode_vel")   { if (on) uiMidiEditMode.store(1); return; }
    if (pid == "ui_mode_pitch") { if (on) uiMidiEditMode.store(2); return; }
    if (pid == "ui_mode_prob")  { if (on) uiMidiEditMode.store(3); return; }
    if (pid == "ui_mode_roll")  { if (on) uiMidiEditMode.store(4); return; }
    if (pid == "ui_mode_pan")   { if (on) uiMidiEditMode.store(5); return; }
    if (pid == "ui_mode_len")   { if (on) uiMidiEditMode.store(6); return; }
    if (pid == "ui_mode_nudge") { if (on) uiMidiEditMode.store(7); return; }
    if (pid.startsWith("ui_influence_ch")) { if (on) uiMidiInfluence.store(pid.substring(15).getIntValue()); return; }

    // Pattern-scoped controls:  "p{P}_..."
    if (!pid.startsWithChar('p')) return;
    juce::StringArray parts = juce::StringArray::fromTokens(pid, "_", "");
    if (parts.size() < 2) return;

    int p = parts[0].substring(1).getIntValue();
    if (p < 0 || p >= Sequencer::NUM_PATTERNS) return;
    auto& pat = sequencer.patterns[p];

    if (parts[1] == "select")
    {
        if (on)
        {
            sequencer.setCurrentPattern(p);
            patternChangedByMidi.store(true);
        }
        return;
    }

    if (parts[1] == "step" && parts.size() == 4)
    {
        int ch = parts[2].getIntValue();
        int step = parts[3].getIntValue();
        if (ch >= 0 && ch < Sequencer::NUM_CHANNELS && step >= 0 && step < DrumChannel::MAX_STEPS)
        {
            pat.channels[ch].steps[step] = on;
        }
        return;
    }

    if (parts[1].startsWith("ch") && parts.size() >= 3)
    {
        int ch = parts[1].substring(2).getIntValue();
        juce::String param = parts[2];
        if (ch < 0 || ch >= Sequencer::NUM_CHANNELS) return;

        auto& dch = pat.channels[ch];
        if      (param == "volume")  dch.volume     = norm;
        else if (param == "pan")     dch.pan        = norm * 2.0f - 1.0f;
        else if (param == "pitch")   dch.pitch      = (norm * 2.0f - 1.0f) * 24.0f;
        else if (param == "bloom")  dch.bloom  = norm;                                 // 0..1 blend character
        else if (param == "drift")  dch.drift  = norm;
        else if (param == "spread") dch.spread = norm;
        else if (param == "punch")  dch.punch  = norm;
        else if (param == "glue")   dch.glue   = norm;
        else if (param.startsWith("atk")) { int s = param.substring(3).getIntValue();
                                            if (s >= 0 && s < DrumChannel::NUM_SOURCES) dch.srcAtk[s]  = norm * 1.0f; }
        else if (param.startsWith("hld")) { int s = param.substring(3).getIntValue();
                                            if (s >= 0 && s < DrumChannel::NUM_SOURCES) dch.srcHold[s] = norm * 2.0f; }
        else if (param.startsWith("dec")) { int s = param.substring(3).getIntValue();
                                            if (s >= 0 && s < DrumChannel::NUM_SOURCES) dch.srcDec[s]  = norm * 4.0f; }
        else if (param.startsWith("eq")) { int bi = param.substring(2).getIntValue();   // CC -> bell gain (3 bells)
                                           if (bi >= 0 && bi < 3)
                                               dch.eqBand[DrumChannel::EQ_B1 + bi].gainDb = (norm * 2.0f - 1.0f) * 18.0f; }
        else if (param == "filterCutoff") dch.filterCutoff = 20.0f * std::pow(1000.0f, norm); // 20..20000 Hz
        else if (param == "filterReso")   dch.filterReso   = 0.3f + norm * 11.7f;             // 0.3..12 Q
        else if (param == "filterEnvAmt") dch.filterEnvAmt = norm * 2.0f - 1.0f;              // -1..1
        else if (param == "drive")        dch.driveAmount  = norm;                            // 0..1
        else if (param == "reverb")  dch.reverbSend = norm;
        else if (param == "delay")   dch.delaySend  = norm;
        else if (param == "mute")    dch.mute       = on;
        else if (param == "solo")    dch.solo       = on;
        else if (param == "overlap") dch.allowOverlap = on;
        else if (param == "phase")   dch.phaseInvert = on;
        dch.markDspDirty();
    }
}

void DrumSequencerProcessor::updateAnySolo()
{
    anySolo = false;
    for (auto& ch : sequencer.current().channels)
        if (ch.solo) { anySolo = true; break; }
}

void DrumSequencerProcessor::processReverb(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples)
{
    // True send/return: `send` already holds Sum(channel x its reverbSend), so a channel at send=0
    // contributes nothing. We reverb the send (WET ONLY) then add the tail back to Main.
    if (audio.getNumChannels() < 2) return;

    float* sL = send.getWritePointer(0);
    float* sR = send.getWritePointer(1);

    // Pre-delay: push a gap (0..120 ms) before the tail so the dry transient is heard first (drums).
    // The send is fed through a short delay line before the reverb. Always buffered so the line stays
    // warm; only delayed when Pre > 0.
    {
        const int preMax   = reverbPreBuffer.getNumSamples();
        const int preSamps = juce::jlimit(0, preMax - 1, (int)(masterFX().reverbPreDelay * 0.120 * currentSampleRate));
        float* pbL = reverbPreBuffer.getWritePointer(0);
        float* pbR = reverbPreBuffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const int rd = (reverbPreHead + i - preSamps + preMax) % preMax;
            const int wr = (reverbPreHead + i) % preMax;
            const float inL = sL[i], inR = sR[i];
            if (preSamps > 0) { sL[i] = pbL[rd]; sR[i] = pbR[rd]; }
            pbL[wr] = inL; pbR[wr] = inR;
        }
        reverbPreHead = (reverbPreHead + numSamples) % preMax;
    }

    // Modulated FDN reverb (in place: send -> wet tail). "Decay" knob = 1 - reverbDamp (more = longer).
    const float decay01 = 1.0f - juce::jlimit(0.0f, 1.0f, masterFX().reverbDamp);
    fdn.process(sL, sR, sL, sR, numSamples,
                juce::jlimit(0.0f, 1.0f, masterFX().reverbRoom),     // Size -> room
                decay01,                                             // Decay -> feedback time
                0.35f,                                               // damping LP (smooth, musical)
                juce::jlimit(0.0f, 1.0f, masterFX().reverbWidth));   // Width

    // MAKE-UP: the FDN attenuates ~12 dB internally, so the wet was ~ -46 dB at factory sends and
    // still ~ -26 dB fully cranked = inaudible ("reverb does nothing"). Bring it up to a usable range.
    const float wet = juce::jlimit(0.0f, 1.0f, masterFX().reverbWet) * 5.0f;
    audio.addFrom(0, 0, sL, numSamples, wet);   // add the wet tail to Main (scaled by Wet)
    audio.addFrom(1, 0, sR, numSamples, wet);
}

void DrumSequencerProcessor::processDelay(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples)
{
    // True send/return: `send` already holds Sum(channel x its delaySend). It feeds the delay line;
    // the wet echoes return to Main. (We always run so existing echoes ring out naturally.)
    const int delayLen = delayBuffer.getNumSamples();

    // Sync mode: Time selects a note division (in beats) relative to host tempo.
    double delaySecs = masterFX().delayTime;
    if (masterFX().delaySync)
    {
        static const double divBeats[] = { 0.25, 1.0/3.0, 0.5, 0.75, 1.0, 1.5, 2.0 }; // 1/16,1/8T,1/8,1/8.,1/4,1/4.,1/2
        const int n = (int) (sizeof(divBeats) / sizeof(divBeats[0]));
        const double beats = divBeats[juce::jlimit(0, n - 1, masterFX().delayDivision)];
        const double bpm = currentBpm > 1.0 ? currentBpm : 120.0;
        delaySecs = beats * 60.0 / bpm;
    }
    const int delaySamples = juce::jlimit(1, delayLen - 1, (int)(delaySecs * currentSampleRate));
    const float feedback = juce::jlimit(0.0f, 0.98f, masterFX().delayFeedback);

    // Fixed musical feedback tone: each repeat is low-passed (~4 kHz, tape-style darkening) and
    // high-passed (~180 Hz, stops low-end build-up). The wet RETURN stays full-band; only the
    // signal fed back into the line is filtered, so the tail decays naturally instead of ringing.
    const float sr = (float) currentSampleRate;
    const float lpCoef = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 4000.0f / sr);
    const float hpCoef = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 180.0f  / sr);

    // Process L+R together so Ping-Pong can cross-feed the feedback (L's tail feeds R's line
    // and vice-versa, so echoes bounce across the stereo field). Main only; aux outs stay dry.
    const bool stereo = audio.getNumChannels() >= 2;
    const bool ping   = masterFX().delayPingPong && stereo;
    const int  nDly   = delayBuffer.getNumChannels();
    float* outL = audio.getWritePointer(0);
    float* outR = stereo ? audio.getWritePointer(1) : nullptr;
    const float* inL = send.getReadPointer(0);
    const float* inR = send.getReadPointer(stereo ? 1 : 0);
    float* dL   = delayBuffer.getWritePointer(0);
    float* dR   = delayBuffer.getWritePointer(juce::jmin(1, nDly - 1));
    const float wet = juce::jlimit(0.0f, 1.0f, masterFX().delayWet);   // delay return level (MASTER "Wet" knob)

    for (int i = 0; i < numSamples; ++i)
    {
        const int readPos  = (delayWriteHead + i - delaySamples + delayLen) % delayLen;
        const int writePos = (delayWriteHead + i) % delayLen;

        const float dryL = inL[i];                            // delay INPUT = the send sum
        const float dryR = stereo ? inR[i] : inL[i];
        const float delL = dL[readPos];
        const float delR = dR[readPos];

        outL[i] += delL * wet;                                // echoes return to Main
        if (stereo) outR[i] += delR * wet;

        delayFbLp[0] += lpCoef * (delL - delayFbLp[0]);       // band-limit each feedback tap
        delayFbHp[0] += hpCoef * (delayFbLp[0] - delayFbHp[0]);
        const float fbL = delayFbLp[0] - delayFbHp[0];
        delayFbLp[1] += lpCoef * (delR - delayFbLp[1]);
        delayFbHp[1] += hpCoef * (delayFbLp[1] - delayFbHp[1]);
        const float fbR = delayFbLp[1] - delayFbHp[1];

        if (ping) { dL[writePos] = dryL + fbR * feedback; dR[writePos] = dryR + fbL * feedback; }
        else      { dL[writePos] = dryL + fbL * feedback; dR[writePos] = dryR + fbR * feedback; }
    }
    delayWriteHead = (delayWriteHead + numSamples) % delayLen;
}

juce::File DrumSequencerProcessor::exportMidiFile(int channel)
{
    // Drag MIDI (v1.2.0): export the SELECTED channel only, as a MELODY. The note of each step =
    // a BASE note + that step's pitch value, so a pitched line (keys recording, acid bassline)
    // exports as real notes; all-same pitches = one repeated note. The base:
    //   - MIDI-out channels: their configured midiNote (unchanged behaviour);
    //   - sound channels: the nearest MIDI note of SLOT 1's Freq (after a keys session that IS
    //     C3, since touching the piano re-tunes the Freq - so exports match what you played;
    //     if the knob was moved later, the export transposes with it, as expected).
    // MERGED chains export as ONE long note spanning the whole chain. Velocity / rolls / swing /
    // Length / tempo + time-sig meta all carry over.
    juce::MidiFile midiFile;
    constexpr int tpq = 96;   // ticks per quarter note
    midiFile.setTicksPerQuarterNote(tpq);

    juce::MidiMessageSequence seq;

    const double qpb      = juce::jmax(1, currentTimeSigNum) * 4.0 / juce::jmax(1, currentTimeSigDen);
    const double barTicks = qpb * (double) tpq;

    seq.addEvent(juce::MidiMessage::tempoMetaEvent((int) (60000000.0 / juce::jmax(1.0, currentBpm))), 0.0);
    seq.addEvent(juce::MidiMessage::timeSignatureMetaEvent(juce::jmax(1, currentTimeSigNum),
                                                           juce::jmax(1, currentTimeSigDen)), 0.0);

    const int ch = juce::jlimit(0, Sequencer::NUM_CHANNELS - 1, channel);
    // MERGED GROUP: export every bar of the group back to back (bar b at tick offset b * barTicks).
    const int gHead = sequencer.groupHead(sequencer.currentPattern);
    const int gEnd  = sequencer.groupEnd(sequencer.currentPattern);
    for (int gBar = gHead; gBar <= gEnd; ++gBar)
    {
        const auto& pat = sequencer.patterns[gBar];
        const double tickOff = (double) (gBar - gHead) * barTicks;
        const auto& chn = pat.channels[ch];
        const int n = chn.numSteps;
        const int midiCh = chn.midiOut ? juce::jlimit(1, 16, chn.midiOutChannel) : 1;
        const double stepTicks = barTicks / (double) juce::jmax(1, n);

        // PER-SLOT VOICED export (v1.3.0): each PITCHED slot (Osc/Modal/Phys, audible) exports its
        // OWN notes from its OWN Freq-knob base (rounded to the nearest MIDI note - the 0-point);
        // a slot in CHORD/SCALE mode exports its FULL voicing (every chord note), so slot 1 in a
        // 3-note chord + slot 2 in a 5-note scale = up to 8 notes stacked per hit (duplicates
        // de-duped). Sample/Noise slots have NO Freq base, so they don't add per-slot notes; but a
        // channel with NO pitched slot (pure Sample/Noise) still exports its step/draw PITCH contour
        // on the channel's own note (`midiNote` + pitch) - NO fixed C3 anchor.
        struct PSlot { int slotIdx; int base; bool scaleOn; int scaleType, scaleKey, scaleUni, chordMode, chordUni; };
        juce::Array<PSlot> pslots;
        if (! chn.midiOut)
            for (int si = 0; si < DrumChannel::NUM_SLOTS; ++si)
            {
                const auto& sl = chn.slots[si];
                if (sl.weight <= 0.001f) continue;
                double hz = 0.0;
                if      (sl.engine == DrumChannel::SrcOsc || sl.engine == DrumChannel::SrcModal) hz = sl.oscFreq;
                else if (sl.engine == DrumChannel::SrcPhys)                                      hz = sl.physFreq;
                else continue;   // Sample / Noise: unpitched -> contributes no notes
                PSlot p; p.slotIdx = si;
                p.base = juce::jlimit(0, 127, (int) std::lround(69.0 + 12.0 * std::log2(juce::jmax(20.0, hz) / 440.0)));
                p.scaleOn = sl.scaleOn; p.scaleType = sl.scaleType; p.scaleKey = sl.scaleKey;
                p.scaleUni = sl.scaleUnison; p.chordMode = sl.chordMode; p.chordUni = sl.chordUnison;
                pslots.add(p);
            }
        // Emit one hit: every slot's voiced notes (or the MIDI-out / drum fallback), de-duped per hit.
        auto emitNotes = [&](int semis, juce::uint8 vel, double tOn, double tOff, int noteSlot = 0)
        {
            bool used[128] = {};
            auto add = [&](int note) {
                if (note < 0 || note > 127 || used[note]) return;
                used[note] = true;
                seq.addEvent(juce::MidiMessage::noteOn (midiCh, note, vel), tickOff + tOn);
                seq.addEvent(juce::MidiMessage::noteOff(midiCh, note),      tickOff + tOff);
            };
            if (chn.midiOut)        { add(juce::jlimit(0, 127, chn.midiNote + semis)); return; }   // unchanged behaviour
            if (pslots.isEmpty())   { add(juce::jlimit(0, 127, chn.midiNote + semis)); return; }   // Sample/Noise: the channel's own note + step/draw pitch (no C3 anchor)
            for (const auto& p : pslots)
            {
                if (noteSlot != 0 && (noteSlot - 1) != p.slotIdx) continue;   // per-note slot tag: only that slot's voicing
                const int played = p.base + semis;
                if (p.scaleOn)          { const int nv = juce::jlimit(1, DrumChannel::UNI_MAX, p.scaleUni);
                    for (int k = 0; k < nv; ++k) add(played + DrumChannel::scaleNoteOffset(p.scaleType, p.scaleKey, played, k)); }
                else if (p.chordMode > 0) { const int nv = juce::jlimit(1, DrumChannel::UNI_MAX, p.chordUni);
                    for (int k = 0; k < nv; ++k) add(played + DrumChannel::chordNoteOffset(p.chordMode, k)); }
                else add(played);
            }
        };

        // DRAW mode: the melody is the drawn lane (per-column semitone), not the steps. Each
        // contiguous run of the SAME semitone = one note; DRAW_GAP = a rest. Freely exported at
        // the lane's resolution (no need to quantise to steps first).
        if (chn.drawMode)
        {
            const double colTicks = barTicks / (double) DrumChannel::DRAW_RES;
            for (int ni = 0; ni < chn.drawNoteCount; ++ni)   // note list: recorded/drawn chords export as chords
            {
                const auto& nt = chn.drawNotes[ni];
                const auto vel = (juce::uint8) juce::jlimit(1, 127, (int) nt.vel >> 1);
                emitNotes((int) nt.semi, vel, (double) nt.start * colTicks,
                          (double) (nt.start + nt.len) * colTicks, (int) nt.slot);   // per-slot voiced + slot-tag aware
            }
        }
        else
        for (int step = 0; step < n; ++step)
        {
            if (! chn.steps[step] || chn.stepMerge[step]) continue;   // merged steps ride their head
            double st, en;
            Sequencer::stepSpan(step, n, pat.swing, st, en);   // swung span -> the exported groove == the played one
            // MERGE chain: extend this note through every following merged step.
            double chainEn = en;
            for (int k = step + 1; k < n && chn.stepMerge[k]; ++k)
            { double st2, en2; Sequencer::stepSpan(k, n, pat.swing, st2, en2); chainEn = en2; }
            const bool mergedChain = chainEn > en + 1.0e-9;

            // Rolls play on merged chains too (matches the engine): the sub-hits ratchet inside
            // the head step and the LAST one holds through the rest of the chain.
            const int roll = juce::jlimit(1, 6, chn.stepRoll[step]);
            const int semis = juce::roundToInt(chn.stepPitch[step]);
            const float  gl = juce::jlimit(0.0f, 1.0f, chn.stepNoteLen[step]);
            const double lenSteps = gl > 0.001f ? (double) gl : (chn.midiOut ? 1.0 : 0.8 / (double) roll);
            // Length = fraction of the WHOLE chain, measured from the chain start.
            const double chainGateEnd = st + (chainEn - st) * (gl > 0.001f ? (double) gl : 1.0);

            // NUDGE: the exported note shifts early/late exactly like the engine plays it.
            const double nud = (double) juce::jlimit(-1.0f, 1.0f, chn.stepNudge[step]) * 0.5 * (en - st);
            for (int j = 0; j < roll; ++j)
            {
                const double pos = juce::jlimit(0.0, 0.9999995, st + nud + (en - st) * (double) j / (double) roll);   // bar fraction
                float velScale = 1.0f;
                if (roll > 1)
                {
                    const float rr   = juce::jlimit(-1.0f, 1.0f, chn.stepRollDecay[step]);
                    const float frac = (float) j / (float) (roll - 1);
                    velScale = (rr >= 0.0f) ? (1.0f - rr) + rr * frac : 1.0f + rr * frac;
                    velScale = juce::jmax(0.0f, velScale);
                }
                const float v   = juce::jlimit(0.0f, 1.0f, chn.stepVel[step] * velScale);
                const auto  vel = (juce::uint8) juce::jlimit(1, 127, juce::roundToInt(v * 127.0f));
                const double startTick = pos * barTicks;
                double endTick = startTick + juce::jmax(4.0, lenSteps * stepTicks);
                if (mergedChain && j == roll - 1)   // the last sub-hit rings through the merged chain
                    endTick = juce::jmax(startTick + 4.0, chainGateEnd * barTicks);
                emitNotes(semis, vel, startTick, endTick);   // per-slot voiced (chord/scale aware)
            }
        }
    }

    seq.updateMatchedPairs();
    seq.sort();
    midiFile.addTrack(seq);

    juce::File tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("drum_seq_export.mid");
    tmpFile.deleteFile();
    juce::FileOutputStream fos(tmpFile);
    if (fos.openedOk())
        midiFile.writeTo(fos);

    return tmpFile;
}

static void writeChannel(juce::ValueTree& chState, const DrumChannel& ch)
{
    chState.setProperty("name",     ch.channelName,    nullptr);
    chState.setProperty("volume",   ch.volume,         nullptr);
    chState.setProperty("pan",      ch.pan,            nullptr);
    chState.setProperty("mute",     ch.mute,           nullptr);
    chState.setProperty("solo",     ch.solo,           nullptr);
    chState.setProperty("phase",    ch.phaseInvert,    nullptr);
    chState.setProperty("pitch",    ch.pitch,          nullptr);
    for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s) {
        chState.setProperty("atk" + juce::String(s), ch.srcAtk[s],  nullptr);
        chState.setProperty("hld" + juce::String(s), ch.srcHold[s], nullptr);
        chState.setProperty("dec" + juce::String(s), ch.srcDec[s],  nullptr);
    }
    for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) {
        const auto& eb = ch.eqBand[b]; const juce::String k = "eb" + juce::String(b);
        chState.setProperty(k + "on", eb.on, nullptr); chState.setProperty(k + "f", eb.freq, nullptr);
        chState.setProperty(k + "g", eb.gainDb, nullptr); chState.setProperty(k + "q", eb.q, nullptr);
    }
    chState.setProperty("fType",    ch.filterType,     nullptr);
    chState.setProperty("fCutoff",  ch.filterCutoff,   nullptr);
    chState.setProperty("fReso",    ch.filterReso,     nullptr);
    chState.setProperty("fEnvAmt",  ch.filterEnvAmt,   nullptr);
    chState.setProperty("drvType",  ch.driveType,      nullptr);
    chState.setProperty("drvAmt",   ch.driveAmount,    nullptr);
    chState.setProperty("pEnvAmt",  ch.pitchEnvAmt,    nullptr);
    chState.setProperty("pEnvTime", ch.pitchEnvTime,   nullptr);
    chState.setProperty("pEnvOff",  ch.pitchOffset,    nullptr);
    chState.setProperty("smpRev",   ch.sampleReverse,  nullptr);
    chState.setProperty("bloom",  ch.bloom,  nullptr);
    chState.setProperty("drift",  ch.drift,  nullptr);
    chState.setProperty("spread", ch.spread, nullptr);
    chState.setProperty("punch",  ch.punch,  nullptr);
    chState.setProperty("glue",   ch.glue,   nullptr);
    for (int i = 0; i < DrumChannel::NUM_SOURCES; ++i) {
        chState.setProperty("srcOn" + juce::String(i), ch.srcOn[i],     nullptr);
        chState.setProperty("srcW"  + juce::String(i), ch.srcWeight[i], nullptr);
    }
    chState.setProperty("padX", ch.padX, nullptr);
    chState.setProperty("padY", ch.padY, nullptr);
    chState.setProperty("padB", ch.padLayoutB, nullptr);
    chState.setProperty("layOscSh", ch.layerOscShape,     nullptr);
    chState.setProperty("laySFreq", ch.layerSineFreq,     nullptr);
    chState.setProperty("laySPEA",  ch.layerSinePEnvAmt,  nullptr);
    chState.setProperty("laySPET",  ch.layerSinePEnvTime, nullptr);
    chState.setProperty("laySPOff", ch.layerSinePOffset,  nullptr);
    chState.setProperty("oscUni",   ch.oscUnison,         nullptr);
    chState.setProperty("oscDet",   ch.oscDetune,         nullptr);
    chState.setProperty("oscSus",   ch.oscSustain,        nullptr);
    chState.setProperty("oscVib",   ch.oscVibrato,        nullptr);
    chState.setProperty("fmSus",    ch.fmSustain,         nullptr);
    chState.setProperty("phySus",   ch.physSustain,       nullptr);
    chState.setProperty("phyVib",   ch.physVibrato,       nullptr);
    chState.setProperty("nSus",     ch.noiseSustain,      nullptr);
    chState.setProperty("phF",      ch.physFreq,          nullptr);
    chState.setProperty("phTone",   ch.physTone,          nullptr);
    chState.setProperty("phMat",    ch.physMaterial,      nullptr);
    chState.setProperty("phPEA",    ch.physPitchEnvAmt,   nullptr);
    chState.setProperty("phPET",    ch.physPitchEnvTime,  nullptr);
    chState.setProperty("phPOff",   ch.physPitchOffset,   nullptr);
    chState.setProperty("phPos",    ch.physPosition,      nullptr);
    chState.setProperty("nType",    ch.noiseType,         nullptr);
    chState.setProperty("layNCtr",  ch.layerNoiseCenter,  nullptr);
    chState.setProperty("layNWid",  ch.layerNoiseWidth,   nullptr);
    chState.setProperty("fmPit",    ch.fmPitch,           nullptr);
    chState.setProperty("fmSpr",    ch.fmSpread,          nullptr);
    chState.setProperty("fmDep",    ch.fmDepth,           nullptr);
    chState.setProperty("fmPEA",    ch.fmPitchEnvAmt,     nullptr);
    chState.setProperty("fmPET",    ch.fmPitchEnvTime,    nullptr);
    chState.setProperty("fmPOff",   ch.fmPitchOffset,     nullptr);
    chState.setProperty("fmFb",     ch.fmFeedback,        nullptr);
    chState.setProperty("fmSub",    ch.fmSub,             nullptr);
    chState.setProperty("smpCrush", ch.sampleCrush,       nullptr);
    chState.setProperty("reverb",   ch.reverbSend,     nullptr);
    chState.setProperty("delay",    ch.delaySend,      nullptr);
    chState.setProperty("outBus",   ch.outputBus,      nullptr);   // multi-out routing (channel-wide)
    chState.setProperty("midiOut",  ch.midiOut,        nullptr);   // route to MIDI out instead of sound
    chState.setProperty("midiCh",   ch.midiOutChannel, nullptr);   // MIDI out channel (1-16)
    chState.setProperty("keys2Dn",  ch.keysSlot2Down,  nullptr);   // KEYS slot-2 transpose (per pattern/channel)
    chState.setProperty("humanize", ch.humanizeAmt,    nullptr);   // HUMANIZE: between-slot timing/velocity jitter
    chState.setProperty("strum",    ch.strumAmt,       nullptr);   // STRUM: chord/scale note time-spread
    chState.setProperty("keysMinVel", ch.keysMinVel,   nullptr);   // KEYS: minimum played velocity floor
    chState.setProperty("keysMaxVel", ch.keysMaxVel,   nullptr);   // KEYS: maximum played velocity ceiling
    chState.setProperty("keysGlide",  ch.keysGlide,    nullptr);   // KEYS: mono legato glide (portamento) time
    chState.setProperty("arpOn",   ch.arpOn,   nullptr);            // ARP: on/off
    chState.setProperty("arpLen",  ch.arpLen,  nullptr);            // ARP: pattern length incl. root
    chState.setProperty("arpSync", ch.arpSync, nullptr);            // ARP: base notes-per-bar (fader 7..13)
    chState.setProperty("arpRate", ch.arpRate, nullptr);            // ARP: rate multiplier index (1/3..3)
    { juce::String ao; for (int i = 0; i < DrumChannel::ARP_ROWS; ++i) ao << (int) ch.arpOffset[i] << ',';
      chState.setProperty("arpOff", ao, nullptr); }                 // ARP: 12 row offsets (ARP_REST = rest)
    chState.setProperty("keysPoly",   ch.keysPolyMode, nullptr);   // KEYS: poly (held keys stack) vs mono (new key cuts)
    chState.setProperty("chokeGrp", ch.chokeGroup,     nullptr);   // choke group (channel-wide)
    chState.setProperty("duckBy",   ch.duckBy,         nullptr);   // sidechain duck (channel-wide)
    chState.setProperty("duckAmt",  ch.duckAmt,        nullptr);
    chState.setProperty("numSteps", ch.numSteps,       nullptr);
    chState.setProperty("sound",    (int)ch.soundType, nullptr);
    chState.setProperty("userSample", ch.userSampleFile.getFullPathName(), nullptr);
    chState.setProperty("useRegion", ch.useRegion,  nullptr);
    chState.setProperty("smpStart",  ch.sampleStart, nullptr);
    chState.setProperty("smpEnd",    ch.sampleEnd,   nullptr);
    chState.setProperty("slices",    ch.sliceCount,  nullptr);
    chState.setProperty("stretch",   ch.stretchAmt,    nullptr);
    chState.setProperty("smpSpeed",  ch.playSpeed,   nullptr);
    chState.setProperty("overlap",   ch.allowOverlap, nullptr);
    chState.setProperty("mixName",   ch.mixName,     nullptr);
    chState.setProperty("mixMod",    ch.mixModified, nullptr);
    chState.setProperty("envMode",   ch.envEditMode, nullptr);   // UI: envelope-target dropdown (per channel)

    juce::String stepStr, velStr, pitchStr, rollStr, rollDecStr, noteLenStr, panStr, nudgeStr, condLenStr, condMaskStr;
    for (int s = 0; s < DrumChannel::MAX_STEPS; ++s)
    {
        stepStr += (ch.steps[s] ? "1" : "0");
        velStr   += juce::String(ch.stepVel[s],   3) + ",";
        pitchStr += juce::String(ch.stepPitch[s], 2) + ",";
        rollStr  += juce::String(ch.stepRoll[s])      + ",";
        rollDecStr += juce::String(ch.stepRollDecay[s], 3) + ",";
        noteLenStr += juce::String(ch.stepNoteLen[s], 3) + ",";
        panStr   += juce::String(ch.stepPan[s],   3) + ",";
        nudgeStr += juce::String(ch.stepNudge[s], 3) + ",";
        condLenStr  += juce::String(ch.stepCondLen[s])  + ",";
        condMaskStr += juce::String(ch.stepCondMask[s]) + ",";
    }
    chState.setProperty("steps", stepStr, nullptr);
    chState.setProperty("stepVel",   velStr,   nullptr);
    chState.setProperty("stepPitch", pitchStr, nullptr);
    chState.setProperty("stepRoll",  rollStr,  nullptr);
    chState.setProperty("stepRollDec", rollDecStr, nullptr);
    chState.setProperty("stepNoteLen", noteLenStr, nullptr);
    chState.setProperty("lenV", 2, nullptr);   // v2 = Length is a 0..1 GATE (0 = off); v1 mapped 0..1 -> 0.1..4 steps
    chState.setProperty("stepPan", panStr, nullptr);
    chState.setProperty("stepCondLen",  condLenStr,  nullptr);
    chState.setProperty("stepCondMask", condMaskStr, nullptr);
    { juce::String sl; for (int s = 0; s < DrumChannel::MAX_STEPS; ++s) sl += ch.stepSlide[s] ? "1" : "0";
      chState.setProperty("stepSlide", sl, nullptr); }
    { juce::String mg; for (int s = 0; s < DrumChannel::MAX_STEPS; ++s) mg += ch.stepMerge[s] ? "1" : "0";
      chState.setProperty("stepMerge", mg, nullptr); }
    // PIANO ROLL (only when active, to keep normal channels lean): the NOTE LIST packed
    // "start:len:semi:vel," per note, plus the default Vel + whole-channel Pan.
    chState.setProperty("drawMode", ch.drawMode, nullptr);
    if (ch.drawMode)
    {
        juce::String ns; ns.preallocateBytes((size_t) ch.drawNoteCount * 14);
        for (int i = 0; i < ch.drawNoteCount; ++i)
            ns << (int) ch.drawNotes[i].start << ':' << (int) ch.drawNotes[i].len << ':'
               << (int) ch.drawNotes[i].semi  << ':' << (int) ch.drawNotes[i].vel  << ':'
               << (int) ch.drawNotes[i].slot  << ':'
               << (int) ch.drawNotes[i].glide << ',';
        chState.setProperty("drawNotes", ns, nullptr);
        chState.setProperty("drawVel", ch.drawVel, nullptr);
        chState.setProperty("drawPan", ch.drawPan, nullptr);
    }

    ch.writeSlots(chState);   // 2-slot model (duplicate engines survive save/load + undo)

}

static void readChannel(const juce::ValueTree& child, DrumChannel& ch)
{
    ch.channelName = child.getProperty("name", ch.channelName).toString();
    ch.volume      = (float)child.getProperty("volume",   1.0f);   // matches the field default
    ch.pan         = (float)child.getProperty("pan",      0.0f);
    ch.mute        = (bool)child.getProperty("mute",      false);
    ch.solo        = (bool)child.getProperty("solo",      false);
    ch.phaseInvert = (bool)child.getProperty("phase",     false);
    ch.pitch       = (float)child.getProperty("pitch",    0.0f);
    {
        // One default per source (Sample/Noise/Osc/FM/Physical) - was 4 entries for 5 sources = OOB read.
        const float decDef[DrumChannel::NUM_SOURCES] = { 2.0f, 0.08f, 0.20f, 0.30f, 0.80f };
        for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s) {
            ch.srcAtk[s]  = (float)child.getProperty("atk" + juce::String(s), 0.003f);
            ch.srcHold[s] = (float)child.getProperty("hld" + juce::String(s), 0.0f);
            ch.srcDec[s]  = (float)child.getProperty("dec" + juce::String(s), decDef[s]);
        }
    }
    for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) {
        auto& eb = ch.eqBand[b]; const DrumChannel::EqBand d; const juce::String k = "eb" + juce::String(b);
        eb.on = (bool)child.getProperty(k + "on", false); eb.freq = (float)child.getProperty(k + "f", d.freq);
        eb.gainDb = (float)child.getProperty(k + "g", 0.0f); eb.q = (float)child.getProperty(k + "q", 1.0f);
    }
    ch.filterType   = (int)  child.getProperty("fType",   0);
    ch.filterCutoff = (float)child.getProperty("fCutoff", 20000.0f);
    ch.filterReso   = (float)child.getProperty("fReso",   0.707f);
    ch.filterEnvAmt = (float)child.getProperty("fEnvAmt", 0.0f);
    ch.driveType    = (int)  child.getProperty("drvType", 0);
    ch.driveAmount  = (float)child.getProperty("drvAmt",  0.0f);
    ch.pitchEnvAmt      = (float)child.getProperty("pEnvAmt",  0.0f);
    ch.pitchEnvTime     = (float)child.getProperty("pEnvTime", 0.05f);
    ch.pitchOffset      = (float)child.getProperty("pEnvOff",  0.0f);
    ch.sampleReverse    = (bool) child.getProperty("smpRev",   false);
    ch.bloom  = (float)child.getProperty("bloom",  0.0f);
    ch.drift  = (float)child.getProperty("drift",  0.0f);
    ch.spread = (float)child.getProperty("spread", 0.0f);
    ch.punch  = (float)child.getProperty("punch",  0.0f);
    ch.glue   = (float)child.getProperty("glue",   0.0f);
    for (int i = 0; i < DrumChannel::NUM_SOURCES; ++i) {
        ch.srcOn[i]     = (bool) child.getProperty("srcOn" + juce::String(i), i == 0);
        ch.srcWeight[i] = (float)child.getProperty("srcW"  + juce::String(i), i == 0 ? 1.0f : 0.0f);
    }
    ch.padX = (float)child.getProperty("padX", 0.5f);
    ch.padY = (float)child.getProperty("padY", 0.5f);
    ch.padLayoutB = (bool)child.getProperty("padB", false);
    ch.layerOscShape    = (int)  child.getProperty("layOscSh", 0);
    ch.layerSineFreq    = (float)child.getProperty("laySFreq", 60.0f);
    ch.layerSinePEnvAmt = (float)child.getProperty("laySPEA",  0.0f);
    ch.layerSinePEnvTime= (float)child.getProperty("laySPET",  0.04f);
    ch.layerSinePOffset = (float)child.getProperty("laySPOff", 0.0f);
    ch.oscUnison        = (int)  child.getProperty("oscUni",   1);
    ch.oscDetune        = (float)child.getProperty("oscDet",   0.0f);
    ch.oscSustain       = (float)child.getProperty("oscSus",   0.0f);
    ch.oscVibrato       = (float)child.getProperty("oscVib",   0.0f);
    ch.fmSustain        = (float)child.getProperty("fmSus",    0.0f);
    ch.physSustain      = (float)child.getProperty("phySus",   0.0f);
    ch.physVibrato      = (float)child.getProperty("phyVib",   0.0f);
    ch.noiseSustain     = (float)child.getProperty("nSus",     0.0f);
    ch.physFreq         = (float)child.getProperty("phF",      110.0f);
    ch.physTone         = (float)child.getProperty("phTone",   0.5f);
    ch.physMaterial     = (float)child.getProperty("phMat",    0.0f);
    ch.physPitchEnvAmt  = (float)child.getProperty("phPEA",    0.0f);
    ch.physPitchEnvTime = (float)child.getProperty("phPET",    0.05f);
    ch.physPitchOffset  = (float)child.getProperty("phPOff",   0.0f);
    ch.physPosition     = (float)child.getProperty("phPos",    0.0f);
    ch.noiseType        = (int)  child.getProperty("nType",    0);
    ch.layerNoiseCenter = (float)child.getProperty("layNCtr",  3000.0f);
    ch.layerNoiseWidth  = (float)child.getProperty("layNWid",  0.0f);   // matches the field default
    ch.fmPitch          = (float)child.getProperty("fmPit",    0.0f);
    ch.fmSpread         = (float)child.getProperty("fmSpr",    0.0f);
    ch.fmDepth          = (float)child.getProperty("fmDep",    0.4f);
    ch.fmPitchEnvAmt    = (float)child.getProperty("fmPEA",    0.0f);
    ch.fmPitchEnvTime   = (float)child.getProperty("fmPET",    0.05f);
    ch.fmPitchOffset    = (float)child.getProperty("fmPOff",   0.0f);
    ch.fmFeedback       = (float)child.getProperty("fmFb",     0.0f);
    ch.fmSub            = (float)child.getProperty("fmSub",    0.0f);
    ch.sampleCrush      = (float)child.getProperty("smpCrush", 0.0f);
    ch.reverbSend   = (float)child.getProperty("reverb",  0.0f);
    ch.delaySend   = (float)child.getProperty("delay",   0.0f);
    ch.outputBus   = (int)  child.getProperty("outBus",  0);
    ch.midiOut     = (bool) child.getProperty("midiOut", false);
    ch.midiOutChannel = juce::jlimit(1, 16, (int) child.getProperty("midiCh", 1));
    ch.keysSlot2Down  = juce::jlimit(-24, 24, (int) child.getProperty("keys2Dn", 0));   // KEYS slot-2 transpose (per channel; +down/-up)
    ch.humanizeAmt = juce::jlimit(0.0f, 1.0f, (float) child.getProperty("humanize", 0.0f));   // HUMANIZE
    ch.strumAmt    = juce::jlimit(0.0f, 1.0f, (float) child.getProperty("strum",    0.0f));   // STRUM
    ch.keysMinVel  = juce::jlimit(0.0f, 1.0f, (float) child.getProperty("keysMinVel", 0.0f)); // KEYS min velocity
    ch.keysMaxVel  = juce::jlimit(0.0f, 1.0f, (float) child.getProperty("keysMaxVel", 1.0f)); // KEYS max velocity
    ch.keysGlide   = juce::jlimit(0.0f, 1.0f, (float) child.getProperty("keysGlide",  0.0f)); // KEYS mono glide
    ch.arpOn   = (bool) child.getProperty("arpOn", false);
    ch.arpLen  = juce::jlimit(1, 1 + DrumChannel::ARP_ROWS, (int) child.getProperty("arpLen", 2));
    ch.arpSync = DrumChannel::arpSnapSync((int) child.getProperty("arpSync", 8));
    ch.arpRate = juce::jlimit(0, DrumChannel::ARP_RATES - 1, (int) child.getProperty("arpRate", 8));
    { const juce::String ao = child.getProperty("arpOff", "").toString();
      if (ao.isNotEmpty()) { auto f = juce::StringArray::fromTokens(ao, ",", "");
        for (int i = 0; i < DrumChannel::ARP_ROWS && i < f.size(); ++i)
          ch.arpOffset[i] = (int8_t) juce::jlimit(-128, 127, f[i].getIntValue()); } }
    ch.keysPolyMode = (bool) child.getProperty("keysPoly", true);    // KEYS poly/mono (poly default)
    ch.chokeGroup  = (int)  child.getProperty("chokeGrp", 0);
    ch.duckBy      = juce::jlimit(-1, Sequencer::NUM_CHANNELS - 1, (int) child.getProperty("duckBy", -1));
    ch.duckAmt     = juce::jlimit(0.0f, 1.0f, (float) child.getProperty("duckAmt", 0.5f));
    ch.numSteps    = (int)child.getProperty("numSteps",   8);
    ch.soundType   = (DrumSoundGenerator::Type)(int)child.getProperty("sound", 0);
    ch.useRegion   = (bool) child.getProperty("useRegion", false);
    ch.sampleStart = (float)child.getProperty("smpStart",  0.0f);
    ch.sampleEnd   = (float)child.getProperty("smpEnd",    1.0f);
    ch.sliceCount  = juce::jlimit(1, 16, (int) child.getProperty("slices", 1));
    ch.stretchAmt    = (float) child.getProperty("stretch",  1.0f);   // set before loadUserSample so updateStretch uses it
    ch.playSpeed   = (float)child.getProperty("smpSpeed",  1.0f);
    ch.allowOverlap= (bool) child.getProperty("overlap",   false);
    ch.mixName     =        child.getProperty("mixName",   juce::String()).toString();
    ch.mixModified = (bool) child.getProperty("mixMod",    false);
    ch.envEditMode = (int)  child.getProperty("envMode",   1);

    juce::String stepStr = child.getProperty("steps", "").toString();
    for (int s = 0; s < DrumChannel::MAX_STEPS && s < stepStr.length(); ++s)
        ch.steps[s] = (stepStr[s] == '1');
    {
        auto loadArr = [&](const char* key, float* dst, float def) {
            juce::String s = child.getProperty(key, "").toString();
            if (s.isEmpty()) { for (int i = 0; i < DrumChannel::MAX_STEPS; ++i) dst[i] = def; return; }
            auto toks = juce::StringArray::fromTokens(s, ",", "");
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i)
                dst[i] = (i < toks.size() && toks[i].isNotEmpty()) ? toks[i].getFloatValue() : def;
        };
        loadArr("stepVel",   ch.stepVel,   1.0f);
        loadArr("stepPitch", ch.stepPitch, 0.0f);
        loadArr("stepRollDec", ch.stepRollDecay, 0.0f);
        loadArr("stepNoteLen", ch.stepNoteLen, 0.0f);
        // MIGRATE v1 note lengths (MIDI-out only; 0..1 -> 0.1..4 steps, default 0.25 ~= 1 step):
        // the old default becomes "off" (0 = one full step for MIDI-out = same behaviour), other
        // values clamp into the new one-step gate range.
        if ((int) child.getProperty("lenV", 1) < 2)
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i) {
                const float v = ch.stepNoteLen[i];
                ch.stepNoteLen[i] = (std::abs(v - 0.25f) < 0.005f) ? 0.0f
                                  : juce::jlimit(0.0f, 1.0f, 0.1f + 3.9f * v);
            }
        loadArr("stepPan", ch.stepPan, 0.0f);
        {
            juce::String rs = child.getProperty("stepRoll", "").toString();
            auto toks = juce::StringArray::fromTokens(rs, ",", "");
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i)
                ch.stepRoll[i] = (i < toks.size() && toks[i].isNotEmpty())
                               ? juce::jlimit(1, 6, toks[i].getIntValue()) : 1;
        }
        {
            juce::String sl = child.getProperty("stepSlide", "").toString();
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i)
                ch.stepSlide[i] = (i < sl.length() && sl[i] == '1');
            juce::String mg = child.getProperty("stepMerge", "").toString();
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i)
                ch.stepMerge[i] = (i < mg.length() && mg[i] == '1');
        }
        {
            ch.drawMode = (bool) child.getProperty("drawMode", false);
            ch.drawVel  = (float) child.getProperty("drawVel", 1.0f);
            ch.drawPan  = (float) child.getProperty("drawPan", 0.0f);
            ch.clearDrawNotes();
            const juce::String ns = child.getProperty("drawNotes", "").toString();
            if (ns.isNotEmpty())   // new format: "start:len:semi:vel," per note
            {
                for (auto& tok : juce::StringArray::fromTokens(ns, ",", ""))
                {
                    auto f = juce::StringArray::fromTokens(tok, ":", "");
                    if (f.size() >= 4)
                        ch.addDrawNote(f[0].getIntValue(), f[1].getIntValue(), f[2].getIntValue(), f[3].getIntValue(),
                                       f.size() >= 5 ? f[4].getIntValue() : 0,
                                       f.size() >= 6 ? f[5].getIntValue() : 0);
                }
            }
            else   // MIGRATION: old mono column lane ("drawSemi"/"drawVelC") -> same-semi runs become notes
            {
                const juce::String ds = child.getProperty("drawSemi", "").toString();
                const juce::String vs = child.getProperty("drawVelC", "").toString();
                int i = 0;
                while (i < ds.length() && i < DrumChannel::DRAW_RES)
                {
                    const int cch = (int) ds[i];
                    if (cch == 33) { ++i; continue; }               // gap
                    const int semi = juce::jlimit(-36, 36, cch - 70);
                    int e = i + 1;
                    while (e < ds.length() && e < DrumChannel::DRAW_RES && (int) ds[e] == cch) ++e;
                    const int vc = (i < vs.length()) ? juce::jlimit(0, 127, (int) vs[i] - 35) : 127;
                    ch.addDrawNote(i, e - i, semi, vc << 1);
                    i = e;
                }
            }
        }
        {
            auto cl = juce::StringArray::fromTokens(child.getProperty("stepCondLen",  "").toString(), ",", "");
            auto cm = juce::StringArray::fromTokens(child.getProperty("stepCondMask", "").toString(), ",", "");
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i) {
                ch.stepCondLen[i]  = (i < cl.size() && cl[i].isNotEmpty()) ? juce::jlimit(1, 10, cl[i].getIntValue()) : 1;
                ch.stepCondMask[i] = (i < cm.size() && cm[i].isNotEmpty()) ? cm[i].getIntValue() : 0;
            }
        }
    }

    ch.loadDefaultSound();   // clear slot sample buffers; readSlots/migration below load per-slot

    // Prefer the saved 2-slot data; only old projects without it fall back to
    // buildSlotsFromLegacy() (done in the post-load loop when restoredSlots stays false).
    ch.restoredSlots = ch.readSlots(child);
    // MIGRATION: old projects stored ONE per-channel sample ("userSample"); if no slot loaded its own
    // sample, put it in slot 0 (where buildSlotsFromLegacy places the Sample source).
    {
        bool anyLoaded = false;
        for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) if (ch.slotSample[b].usingUser) anyLoaded = true;
        juce::String sp = child.getProperty("userSample", "").toString();
        if (! anyLoaded && sp.isNotEmpty()) { juce::File f(sp); if (f.existsAsFile()) ch.loadUserSample(0, f); }
    }

    ch.markDspDirty(); // apply restored EQ/filter values
}

// Duplicate a whole pattern into another slot: each channel round-trips through writeChannel/
// readChannel (so sounds, EQ, FX, steps + routing all copy), plus the per-pattern settings.
void DrumSequencerProcessor::copyPattern(int src, int dst)
{
    if (src == dst || src < 0 || dst < 0
        || src >= Sequencer::NUM_PATTERNS || dst >= Sequencer::NUM_PATTERNS) return;

    auto& S = sequencer.patterns[src];
    auto& D = sequencer.patterns[dst];
    D.playMode = S.playMode; D.repeatTarget = S.repeatTarget; D.gotoPattern = S.gotoPattern;
    D.chainLen = S.chainLen; for (int k = 0; k < Sequencer::CHAIN_MAX; ++k) { D.chainSeq[k] = S.chainSeq[k]; D.chainLoops[k] = S.chainLoops[k]; }
    D.swing = S.swing; D.master = S.master;

    for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
    {
        juce::ValueTree t("ch");
        writeChannel(t, S.channels[c]);
        readChannel(t, D.channels[c]);
    }
}

void DrumSequencerProcessor::copyChannel(int pat, int src, int dst)
{
    if (src == dst || pat < 0 || pat >= Sequencer::NUM_PATTERNS
        || src < 0 || dst < 0 || src >= Sequencer::NUM_CHANNELS || dst >= Sequencer::NUM_CHANNELS) return;

    auto& chans = sequencer.patterns[pat].channels;
    const int  keepBus   = chans[dst].outputBus;   // routing is channel-wide -> keep the destination's
    const bool keepMidi  = chans[dst].midiOut;
    const int  keepMidiCh= chans[dst].midiOutChannel;
    const int  keepChoke = chans[dst].chokeGroup;  // choke is channel-wide too
    const int  keepDuckBy = chans[dst].duckBy; const float keepDuckAmt = chans[dst].duckAmt;

    juce::ValueTree t("ch");
    writeChannel(t, chans[src]);
    readChannel(t, chans[dst]);

    chans[dst].outputBus  = keepBus;
    chans[dst].midiOut    = keepMidi;
    chans[dst].midiOutChannel = keepMidiCh;
    chans[dst].chokeGroup = keepChoke;
    chans[dst].duckBy = keepDuckBy; chans[dst].duckAmt = keepDuckAmt;

    // Takes are channel-specific: copying a channel carries its takes to the destination (the user's
    // way to "copy takes between channels"). Replace the destination's existing takes for this channel.
    keysTakes.erase(std::remove_if(keysTakes.begin(), keysTakes.end(),
                                   [dst](const KeysTake& t){ return t.channel == dst; }), keysTakes.end());
    { std::vector<KeysTake> add;
      for (auto& t : keysTakes) if (t.channel == src) { KeysTake c = t; c.channel = dst; add.push_back(std::move(c)); }
      for (auto& c : add) keysTakes.push_back(std::move(c)); }
}

juce::ValueTree DrumSequencerProcessor::captureStateTree()
{
    juce::ValueTree state("DrumSeqState");

    state.setProperty("dawSync",  sequencer.dawSync,         nullptr);
    state.setProperty("bpm",      sequencer.standaloneBpm,   nullptr);
    state.setProperty("tsNum",    sequencer.timeSigNum,      nullptr);
    state.setProperty("tsDen",    sequencer.timeSigDen,      nullptr);
    state.setProperty("curPattern", sequencer.currentPattern, nullptr);
    state.setProperty("followPlay", followPlayback, nullptr);
    state.setProperty("visChans",   visibleChannels, nullptr);
    state.setProperty("visPats",    visiblePatterns, nullptr);
    state.setProperty("audEdit",    auditionOnEdit.load(), nullptr);
    state.setProperty("keys2Down",  keysSlot2Down.load(), nullptr);   // KEYS: slot-2 transpose (semitones down)
    // KEYS takes ride with the state/preset: one child per take, events packed "pat:step:semis:flags,".
    {
        juce::ValueTree kt("KeysTakes");
        for (auto& t : keysTakes)
        {
            juce::ValueTree tt("Take");
            tt.setProperty("name", t.name, nullptr);
            tt.setProperty("ch",   t.channel, nullptr);
            if (t.isDraw)
            {
                tt.setProperty("draw", true, nullptr);
                tt.setProperty("drawPat", t.drawPat, nullptr);
                juce::String ns; ns.preallocateBytes(t.drawNotes.size() * 14);
                for (const auto& nt : t.drawNotes)
                    ns << (int) nt.start << ':' << (int) nt.len << ':' << (int) nt.semi << ':' << (int) nt.vel
                       << ':' << (int) nt.slot << ':' << (int) nt.glide << ',';
                tt.setProperty("notes", ns, nullptr);   // piano-roll take = the note list
            }
            else
            {
                juce::String ev;
                for (auto& e : t.evts)
                    ev << (int) e.pattern << ':' << (int) e.step << ':' << (int) e.semis << ':' << (int) e.flags << ',';
                tt.setProperty("evts", ev, nullptr);
            }
            kt.addChild(tt, -1, nullptr);
        }
        state.addChild(kt, -1, nullptr);
    }
    // Master FX/Output are saved per-pattern inside the pattern loop below.

    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        juce::ValueTree patState("Pattern");
        patState.setProperty("index", p, nullptr);
        patState.setProperty("playMode",     sequencer.patterns[p].playMode,     nullptr);
        patState.setProperty("repeatTarget", sequencer.patterns[p].repeatTarget, nullptr);
        patState.setProperty("gotoPattern",  sequencer.patterns[p].gotoPattern,  nullptr);
        { juce::String cs, cl; for (int k = 0; k < sequencer.patterns[p].chainLen; ++k) {
              cs += juce::String(sequencer.patterns[p].chainSeq[k])   + ",";
              cl += juce::String(sequencer.patterns[p].chainLoops[k]) + ","; }
          patState.setProperty("chain", cs, nullptr); patState.setProperty("chainL", cl, nullptr); }
        patState.setProperty("swing",        sequencer.patterns[p].swing,        nullptr);
        patState.setProperty("mergePrev",    sequencer.patterns[p].mergeWithPrev, nullptr);   // merged-group glue
        // Per-pattern Master FX + Master Output.
        const auto& m = sequencer.patterns[p].master;
        patState.setProperty("revRoom", m.reverbRoom,    nullptr);
        patState.setProperty("revDamp", m.reverbDamp,    nullptr);
        patState.setProperty("revWet",  m.reverbWet,     nullptr);
        patState.setProperty("revPre",  m.reverbPreDelay, nullptr);
        patState.setProperty("revWidth", m.reverbWidth,  nullptr);
        patState.setProperty("delTime", m.delayTime,     nullptr);
        patState.setProperty("delFB",   m.delayFeedback, nullptr);
        patState.setProperty("delWet",  m.delayWet,      nullptr);
        patState.setProperty("delSync", m.delaySync,     nullptr);
        patState.setProperty("delDiv",  m.delayDivision, nullptr);
        patState.setProperty("delPP",   m.delayPingPong, nullptr);
        patState.setProperty("mVol",    m.volume,        nullptr);
        patState.setProperty("mPan",    m.pan,           nullptr);
        patState.setProperty("mMono",   m.mono,          nullptr);
        patState.setProperty("mLimit",  m.limit,         nullptr);
        patState.setProperty("mGlue",   m.glue,          nullptr);
        patState.setProperty("mTilt",   m.tilt,          nullptr);
        patState.setProperty("mSat",    m.sat,           nullptr);

        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        {
            juce::ValueTree chState("Ch");
            writeChannel(chState, sequencer.patterns[p].channels[i]);
            patState.appendChild(chState, nullptr);
        }
        state.appendChild(patState, nullptr);
    }

    state.appendChild(midiLearn.saveState(), nullptr);

    return state;
}

void DrumSequencerProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = captureStateTree();
    juce::MemoryOutputStream mos(dest, false);
    state.writeToStream(mos);
}

void DrumSequencerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // Fresh STANDALONE = factory defaults: ignore the JUCE standalone's ONE startup auto-restore
    // (it fires before the editor exists). Preset-file loads + undo call this AFTER createEditor,
    // so they still work; VST hosts are untouched (a saved project restores as normal).
    if (wrapperType == wrapperType_Standalone && ! uiCreatedOnce) return;

    juce::ValueTree state = juce::ValueTree::readFromData(data, (size_t)sizeInBytes);
    if (!state.isValid()) return;
    applyStateTree(state);
}

void DrumSequencerProcessor::applyStateTree(const juce::ValueTree& state)
{
    sequencer.dawSync        = (bool)state.getProperty("dawSync",  false);
    sequencer.standaloneBpm  = (float)state.getProperty("bpm",     120.0f);
    sequencer.timeSigNum     = (int)  state.getProperty("tsNum",   4);
    sequencer.timeSigDen     = (int)  state.getProperty("tsDen",   4);
    sequencer.currentPattern = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1,
                                            (int)state.getProperty("curPattern", 0));
    sequencer.playPattern = sequencer.currentPattern;   // start playback from the restored pattern
    followPlayback = (bool) state.getProperty("followPlay", false);
    visibleChannels = (int) state.getProperty("visChans", 8);
    visiblePatterns = Sequencer::NUM_PATTERNS;   // always 32 (old files' 16 is ignored)
    auditionOnEdit.store((bool) state.getProperty("audEdit", true));   // default ON (matches a fresh instance)
    keysSlot2Down.store(juce::jlimit(-24, 24, (int) state.getProperty("keys2Down", 0)));
    keysTakes.clear();
    if (auto kt = state.getChildWithName("KeysTakes"); kt.isValid())
        for (int i = 0; i < kt.getNumChildren(); ++i)
        {
            auto tt = kt.getChild(i);
            if (tt.getType() != juce::Identifier("Take")) continue;
            KeysTake t;
            t.name    = tt.getProperty("name", "take").toString();
            t.channel = juce::jlimit(0, Sequencer::NUM_CHANNELS - 1, (int) tt.getProperty("ch", 0));
            if ((bool) tt.getProperty("draw", false))
            {
                t.isDraw = true;
                t.drawPat = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, (int) tt.getProperty("drawPat", 0));
                const juce::String ns = tt.getProperty("notes", "").toString();
                if (ns.isNotEmpty())   // new piano-roll take: "start:len:semi:vel," per note
                {
                    for (auto& tok : juce::StringArray::fromTokens(ns, ",", ""))
                    {
                        auto f = juce::StringArray::fromTokens(tok, ":", "");
                        if (f.size() >= 4 && (int) t.drawNotes.size() < DRAW_TAKE_MAX)   // group takes = concat columns
                            t.drawNotes.push_back({ (int16_t) juce::jlimit(0, DrumChannel::DRAW_RES * 8 - 1, f[0].getIntValue()),
                                                    (int16_t) juce::jlimit(1, DrumChannel::DRAW_RES * 8, f[1].getIntValue()),
                                                    (int8_t)  juce::jlimit(-36, 36, f[2].getIntValue()),
                                                    (uint8_t) juce::jlimit(0, 255, f[3].getIntValue()),
                                                    (uint8_t) juce::jlimit(0, 2, f.size() >= 5 ? f[4].getIntValue() : 0),
                                                    (uint8_t) (f.size() >= 6 && f[5].getIntValue() ? 1 : 0) });
                    }
                }
                else   // MIGRATION: old lane take -> same-semi runs become notes
                {
                    const juce::String ds = tt.getProperty("lane", "").toString();
                    const juce::String vs = tt.getProperty("laneVel", "").toString();
                    int i = 0;
                    while (i < ds.length() && i < DrumChannel::DRAW_RES)
                    {
                        const int cch = (int) ds[i];
                        if (cch == 33) { ++i; continue; }
                        int e = i + 1;
                        while (e < ds.length() && e < DrumChannel::DRAW_RES && (int) ds[e] == cch) ++e;
                        const int vc = (i < vs.length()) ? juce::jlimit(0, 127, (int) vs[i] - 35) : 127;
                        if ((int) t.drawNotes.size() < DrumChannel::DRAW_MAX_NOTES)
                            t.drawNotes.push_back({ (int16_t) i, (int16_t) (e - i),
                                                    (int8_t) juce::jlimit(-36, 36, cch - 70), (uint8_t) (vc << 1) });
                        i = e;
                    }
                }
                if (keysTakes.size() < KEYS_TAKES_MAX) keysTakes.push_back(std::move(t));
                continue;
            }
            juce::StringArray evs = juce::StringArray::fromTokens(tt.getProperty("evts", "").toString(), ",", "");
            for (auto& evStr : evs)
            {
                juce::StringArray f = juce::StringArray::fromTokens(evStr, ":", "");
                if (f.size() < 4) continue;
                t.evts.push_back({ (uint8_t) f[0].getIntValue(), (uint8_t) f[1].getIntValue(),
                                   (int8_t)  f[2].getIntValue(), (uint8_t) f[3].getIntValue() });
            }
            if (! t.evts.empty() && keysTakes.size() < KEYS_TAKES_MAX) keysTakes.push_back(std::move(t));
        }

    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto child = state.getChild(i);

        if (child.getType() == juce::Identifier("Pattern"))
        {
            int p = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, (int)child.getProperty("index", 0));
            sequencer.patterns[p].swing        = (float)child.getProperty("swing", 0.0f);
            sequencer.patterns[p].mergeWithPrev = (bool)child.getProperty("mergePrev", false);
            sequencer.patterns[p].playMode     = (int)child.getProperty("playMode", 0);
            sequencer.patterns[p].repeatTarget = juce::jmax(1, (int)child.getProperty("repeatTarget", 2));
            sequencer.patterns[p].gotoPattern  = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1,
                                                              (int)child.getProperty("gotoPattern", (p + 1) % Sequencer::NUM_PATTERNS));
            { auto toks  = juce::StringArray::fromTokens(child.getProperty("chain",  "").toString(), ",", "");
              auto ltoks = juce::StringArray::fromTokens(child.getProperty("chainL", "").toString(), ",", "");
              int cl = 0; for (int k = 0; k < toks.size(); ++k) { if (toks[k].isEmpty() || cl >= Sequencer::CHAIN_MAX) continue;
                  sequencer.patterns[p].chainSeq[cl]   = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, toks[k].getIntValue());
                  sequencer.patterns[p].chainLoops[cl] = (k < ltoks.size() && ltoks[k].isNotEmpty()) ? juce::jmax(1, ltoks[k].getIntValue()) : 2;
                  ++cl; }
              sequencer.patterns[p].chainLen = cl; sequencer.patterns[p].chainStep = 0; }
            auto& m = sequencer.patterns[p].master;
            m.reverbRoom    = (float)child.getProperty("revRoom", 0.5f);
            m.reverbDamp    = (float)child.getProperty("revDamp", 0.5f);
            m.reverbWet     = (float)child.getProperty("revWet",  0.4f);
            m.reverbPreDelay = (float)child.getProperty("revPre",   0.0f);
            m.reverbWidth    = (float)child.getProperty("revWidth", 1.0f);
            m.delayTime     = (float)child.getProperty("delTime", 0.375f);
            m.delayFeedback = (float)child.getProperty("delFB",   0.3f);
            m.delayWet      = (float)child.getProperty("delWet",  0.3f);   // 0.3 = the old fixed return level
            m.delaySync     = (bool) child.getProperty("delSync", false);
            m.delayDivision = (int)  child.getProperty("delDiv",  4);
            m.delayPingPong = (bool) child.getProperty("delPP",   false);
            m.volume        = (float)child.getProperty("mVol",    0.9f);
            m.pan           = (float)child.getProperty("mPan",    0.0f);
            m.mono          = (bool) child.getProperty("mMono",   false);
            m.limit         = (float)child.getProperty("mLimit",  0.003f);
            m.glue          = (float)child.getProperty("mGlue",   0.0f);
            m.tilt          = (float)child.getProperty("mTilt",   0.5f);
            m.sat           = (float)child.getProperty("mSat",    0.0f);

            int chIdx = 0;
            for (int j = 0; j < child.getNumChildren() && chIdx < Sequencer::NUM_CHANNELS; ++j)
            {
                auto chChild = child.getChild(j);
                if (chChild.getType() == juce::Identifier("Ch"))
                    readChannel(chChild, sequencer.patterns[p].channels[chIdx++]);
            }
        }
        else if (child.getType() == juce::Identifier("MidiLearn"))
        {
            midiLearn.loadState(child);
        }
    }

    for (auto& pat : sequencer.patterns)
        for (auto& ch : pat.channels)
        {
            if (! ch.restoredSlots) ch.buildSlotsFromLegacy();  // old project: derive from legacy fields
            ch.restoredSlots = false;                           // consume the transient flag
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumSequencerProcessor();
}
