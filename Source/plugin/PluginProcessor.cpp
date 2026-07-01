#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "UserPaths.h"

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

    reverbParams.roomSize   = masterFX().reverbRoom;
    reverbParams.damping    = masterFX().reverbDamp;
    reverbParams.wetLevel   = 0.33f;
    reverbParams.dryLevel   = 0.4f;
    reverbParams.width      = 1.0f;
    reverb.setParameters(reverbParams);

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

// Give fresh channels a default sample drawn from the samples folder (rotating),
// instead of a built-in synth sound. A loaded project overrides this afterwards.
void DrumSequencerProcessor::assignDefaultSamples()
{
    auto folder = UserPaths::samples();
    auto files = folder.findChildFiles(juce::File::findFiles, true,
                                       "*.wav;*.aiff;*.aif;*.mp3;*.flac;*.ogg");
    files.sort();
    if (files.isEmpty()) return;
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
            sequencer.patterns[p].channels[i].loadUserSample(0, files[i % files.size()]);  // slot 0 = default Sample slot
}

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
            // (ch.prepareToPlay already loads the default sound only if no slot has a sample, so the
            //  default samples loaded by assignDefaultSamples are preserved across re-prepare.)
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

    reverb.reset();
    fdn.prepare(sampleRate);
    fdn.reset();

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
    deglitchPrev[0] = deglitchPrev[1] = 0.0f;

    launchpadInitPending.store(true);
}

void DrumSequencerProcessor::releaseResources() {}

void DrumSequencerProcessor::processBlock(juce::AudioBuffer<float>& audio,
                                           juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();

    const int numSamples = audio.getNumSamples();

    //-- Handle MIDI input (Launchpad presses + MIDI learn + external control)
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

        // "Keys" mode: a MIDI keyboard plays the channel sounds (each channel triggers on its own
        // MIDI note, like hitting its TEST). Bypasses Launchpad pad parsing to avoid a note clash.
        if (msg.isNoteOnOrOff() && keysMode.load(std::memory_order_relaxed))
        {
            if (msg.isNoteOn())
            {
                const int   n   = msg.getNoteNumber();
                const float vel = msg.getFloatVelocity();
                for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
                {
                    auto& chan = sequencer.channel(c);
                    if (! chan.midiOut && chan.midiNote == n)   // sound channels only
                        chan.trigger(vel, 0.0f);
                }
            }
            continue;   // consumed by Keys mode (don't also treat it as a Launchpad pad)
        }

        // Launchpad pad presses
        if (msg.isNoteOnOrOff())
        {
            int ch, step; bool pressed;
            if (launchpad.parsePadMessage(msg, ch, step, pressed, channelPageB))
            {
                if (pressed && step >= 0)
                    toggleStep(ch, step);
                else if (pressed && step == -1)
                {
                    // Scene button = page toggle
                    channelPageB[ch] = !channelPageB[ch];
                    launchpadRefreshPending.store(true);
                }
            }
        }

        // Route CC messages to assigned parameters/steps
        if (msg.isController())
            routeCC(msg);
    }
    midi.clear();

    //-- Point the spectrum tap at the channel the editor is inspecting
    {
        const int ac = analyzeChannel.load();
        const int as = analysisSlot.load();   // PER-SLOT EQ: which slot to analyse (-1 = mix)
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c) {
            sequencer.channel(c).analysisTap  = (c == ac) ? &spectrumTap : nullptr;
            sequencer.channel(c).analysisSlot = (c == ac) ? as : -1;
        }
    }

    // While the transport is NOT advancing steps (stopped / auditioning via TEST), the per-step arm in
    // the Sequencer never fires. Re-arm here on a FIXED ~5 Hz grid (every 0.2 s) so a held / long-attack /
    // still-ringing sound keeps refreshing and switching the analysed slot updates. The grid is PHASE-ALIGNED
    // to the TEST hit (the test handler below resets analysisArmCtr to 0), so every tap captures the sound at
    // the SAME points in time -> a consistent spectrum. Before this alignment the grid was free-running, so a
    // TEST hit landed at a random offset within it and each tap caught the decay at a different phase = the
    // "different wave every time I press TEST" inconsistency. Playback still uses the step-aligned arm.
    if (! sequencer.isCurrentlyPlaying)
    {
        analysisArmCtr += audio.getNumSamples();
        const int armEvery = juce::jmax(1, (int) (0.2 * currentSampleRate));
        if (analysisArmCtr >= armEvery) { analysisArmCtr = 0; spectrumTap.arm(); }
    }
    else analysisArmCtr = 0;

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
    auto events = sequencer.processBlock(osMain, currentSampleRate * kEngineOS, nOS,
                                          getPlayHead(), anySolo, auxPtrs, NUM_AUX_OUTS,
                                          &reverbSendOS, &delaySendOS);
    engineOS->processSamplesDown(hostBlock);   // -> `audio` at the host rate


    // Average-downsample the send buses to the host rate (cheap; reverb/delay are latency-tolerant).
    {
        auto downAvg = [this, numSamples](juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& dst)
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

        // One step's duration in samples = bar / steps. stepNoteLen (0..1) maps to 0.1..4 steps.
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

            if (activeMidiNote[e.channel] >= 0)                          // retrigger -> cut held note (on its own channel)
                midi.addEvent(juce::MidiMessage::noteOff(activeMidiChan[e.channel], activeMidiNote[e.channel]), 0);

            midi.addEvent(juce::MidiMessage::noteOn(midiCh, note, vel), 0);
            activeMidiChan[e.channel] = midiCh;
            const double samplesPerStep = barSamples / juce::jmax(1, ch.numSteps);
            const double lenSteps = 0.1 + juce::jlimit(0.0f, 1.0f, ch.stepNoteLen[e.step]) * 3.9;  // 0.1..4 steps
            const int len = juce::jmax(1, (int)(lenSteps * samplesPerStep));
            if (len <= numSamples) { midi.addEvent(juce::MidiMessage::noteOff(midiCh, note), len); activeMidiNote[e.channel] = -1; }
            else                   { activeMidiNote[e.channel] = note; midiNoteCountdown[e.channel] = len - numSamples; }
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
            juce::AudioPlayHead::CurrentPositionInfo pos;
            if (ph->getCurrentPosition(pos))
            {
                if (pos.bpm > 1.0) currentBpm = pos.bpm;
                if (pos.timeSigNumerator   > 0) currentTimeSigNum = pos.timeSigNumerator;
                if (pos.timeSigDenominator > 0) currentTimeSigDen = pos.timeSigDenominator;
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

    //-- Send Launchpad feedback
    bool needInit = launchpadInitPending.exchange(false);
    if (needInit)
    {
        juce::ScopedLock sl(launchpadMidiLock);
        auto initMsg = LaunchpadController::buildProgrammerModeInit();
        for (auto m : initMsg)
            pendingLaunchpadMidi.addEvent(m.getMessage(), m.samplePosition);
    }

    bool needRefresh = launchpadRefreshPending.exchange(false) || !events.isEmpty();
    if (needRefresh)
        sendLaunchpadRefresh(events);
}

void DrumSequencerProcessor::sendLaunchpadRefresh(const juce::Array<Sequencer::TriggerEvent>&)
{
    bool steps[8][16] = {};
    bool muted[8] = {};
    int numSteps[8] = {};

    // The Launchpad is an 8x8 grid -> only the first LAUNCHPAD_CH channels map to it.
    for (int ch = 0; ch < Sequencer::LAUNCHPAD_CH; ++ch)
    {
        muted[ch]    = sequencer.channel(ch).mute;
        numSteps[ch] = sequencer.channel(ch).numSteps;
        for (int s = 0; s < 16; ++s)
            steps[ch][s] = sequencer.channel(ch).steps[s];
    }

    int curSteps[8] = {};
    for (int ch = 0; ch < Sequencer::LAUNCHPAD_CH; ++ch)
        curSteps[ch] = sequencer.getChannelStep(ch);

    auto refreshMsgs = launchpad.buildFullRefresh(steps, muted, curSteps, numSteps, channelPageB);

    juce::ScopedLock sl(launchpadMidiLock);
    for (auto m : refreshMsgs)
        pendingLaunchpadMidi.addEvent(m.getMessage(), m.samplePosition);
}

void DrumSequencerProcessor::toggleStep(int channel, int step)
{
    if (channel < 0 || channel >= Sequencer::NUM_CHANNELS) return;
    if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
    auto& s = sequencer.channel(channel).steps[step];
    s = !s;
    launchpadRefreshPending.store(true);
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
    if (pid == "global_masterGlue") { for (auto& pat : sequencer.patterns) pat.master.glue = norm; return; }

    // UI-only controls (edit-mode buttons + influence) -> relayed to the editor.
    if (pid == "ui_mode_vel")   { if (on) uiMidiEditMode.store(1); return; }
    if (pid == "ui_mode_pitch") { if (on) uiMidiEditMode.store(2); return; }
    if (pid == "ui_mode_prob")  { if (on) uiMidiEditMode.store(3); return; }
    if (pid == "ui_mode_roll")  { if (on) uiMidiEditMode.store(4); return; }
    if (pid == "ui_mode_pan")   { if (on) uiMidiEditMode.store(5); return; }
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
            launchpadRefreshPending.store(true);
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
            launchpadRefreshPending.store(true);
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
        launchpadRefreshPending.store(true);
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

    const float wet = juce::jlimit(0.0f, 1.0f, masterFX().reverbWet);
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
    const float wet = masterDelayMix;                         // send already applied per channel

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

juce::File DrumSequencerProcessor::exportMidiFile()
{
    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(96);

    juce::MidiMessageSequence seq;

    // Write the current pattern's steps as MIDI notes
    for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch)
    {
        auto& channel = sequencer.channel(ch);
        for (int step = 0; step < channel.numSteps; ++step)
        {
            if (!channel.steps[step]) continue;
            // Each step = 1 16th note = 24 ticks
            double startTick = step * 24.0;
            double endTick   = startTick + 20.0;
            seq.addEvent(juce::MidiMessage::noteOn (10, channel.midiNote, (uint8_t)100), startTick);
            seq.addEvent(juce::MidiMessage::noteOff(10, channel.midiNote),               endTick);
        }
    }

    seq.sort();
    midiFile.addTrack(seq);

    juce::File tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("drum_seq_export.mid");
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
    chState.setProperty("chokeGrp", ch.chokeGroup,     nullptr);   // choke group (channel-wide)
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

    juce::String stepStr, velStr, pitchStr, probStr, rollStr, rollDecStr, noteLenStr, panStr, condLenStr, condMaskStr;
    for (int s = 0; s < DrumChannel::MAX_STEPS; ++s)
    {
        stepStr += (ch.steps[s] ? "1" : "0");
        velStr   += juce::String(ch.stepVel[s],   3) + ",";
        pitchStr += juce::String(ch.stepPitch[s], 2) + ",";
        probStr  += juce::String(ch.stepProb[s],  3) + ",";
        rollStr  += juce::String(ch.stepRoll[s])      + ",";
        rollDecStr += juce::String(ch.stepRollDecay[s], 3) + ",";
        noteLenStr += juce::String(ch.stepNoteLen[s], 3) + ",";
        panStr   += juce::String(ch.stepPan[s],   3) + ",";
        condLenStr  += juce::String(ch.stepCondLen[s])  + ",";
        condMaskStr += juce::String(ch.stepCondMask[s]) + ",";
    }
    chState.setProperty("steps", stepStr, nullptr);
    chState.setProperty("stepVel",   velStr,   nullptr);
    chState.setProperty("stepPitch", pitchStr, nullptr);
    chState.setProperty("stepProb",  probStr,  nullptr);
    chState.setProperty("stepRoll",  rollStr,  nullptr);
    chState.setProperty("stepRollDec", rollDecStr, nullptr);
    chState.setProperty("stepNoteLen", noteLenStr, nullptr);
    chState.setProperty("stepPan", panStr, nullptr);
    chState.setProperty("stepCondLen",  condLenStr,  nullptr);
    chState.setProperty("stepCondMask", condMaskStr, nullptr);

    ch.writeSlots(chState);   // 3-slot model (duplicate engines survive save/load + undo)

}

static void readChannel(const juce::ValueTree& child, DrumChannel& ch)
{
    ch.channelName = child.getProperty("name", ch.channelName).toString();
    ch.volume      = (float)child.getProperty("volume",   0.8f);
    ch.pan         = (float)child.getProperty("pan",      0.0f);
    ch.mute        = (bool)child.getProperty("mute",      false);
    ch.solo        = (bool)child.getProperty("solo",      false);
    ch.phaseInvert = (bool)child.getProperty("phase",     false);
    ch.pitch       = (float)child.getProperty("pitch",    0.0f);
    {
        const float decDef[4] = { 2.0f, 0.08f, 0.20f, 0.30f };
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
    ch.layerNoiseWidth  = (float)child.getProperty("layNWid",  0.4f);
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
    ch.chokeGroup  = (int)  child.getProperty("chokeGrp", 0);
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
        loadArr("stepProb",  ch.stepProb,  1.0f);
        loadArr("stepRollDec", ch.stepRollDecay, 0.0f);
        loadArr("stepNoteLen", ch.stepNoteLen, 0.25f);
        loadArr("stepPan", ch.stepPan, 0.0f);
        {
            juce::String rs = child.getProperty("stepRoll", "").toString();
            auto toks = juce::StringArray::fromTokens(rs, ",", "");
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i)
                ch.stepRoll[i] = (i < toks.size() && toks[i].isNotEmpty())
                               ? juce::jlimit(1, 6, toks[i].getIntValue()) : 1;
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

    // Prefer the saved 3-slot data; only old projects without it fall back to
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

    juce::ValueTree t("ch");
    writeChannel(t, chans[src]);
    readChannel(t, chans[dst]);

    chans[dst].outputBus  = keepBus;
    chans[dst].midiOut    = keepMidi;
    chans[dst].midiOutChannel = keepMidiCh;
    chans[dst].chokeGroup = keepChoke;
}

void DrumSequencerProcessor::getStateInformation(juce::MemoryBlock& dest)
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
    state.setProperty("keysMode",   keysMode.load(), nullptr);
    state.setProperty("audEdit",    auditionOnEdit.load(), nullptr);
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
        // Per-pattern Master FX + Master Output.
        const auto& m = sequencer.patterns[p].master;
        patState.setProperty("revRoom", m.reverbRoom,    nullptr);
        patState.setProperty("revDamp", m.reverbDamp,    nullptr);
        patState.setProperty("revWet",  m.reverbWet,     nullptr);
        patState.setProperty("revPre",  m.reverbPreDelay, nullptr);
        patState.setProperty("revWidth", m.reverbWidth,  nullptr);
        patState.setProperty("delTime", m.delayTime,     nullptr);
        patState.setProperty("delFB",   m.delayFeedback, nullptr);
        patState.setProperty("delSync", m.delaySync,     nullptr);
        patState.setProperty("delDiv",  m.delayDivision, nullptr);
        patState.setProperty("delPP",   m.delayPingPong, nullptr);
        patState.setProperty("mVol",    m.volume,        nullptr);
        patState.setProperty("mPan",    m.pan,           nullptr);
        patState.setProperty("mMono",   m.mono,          nullptr);
        patState.setProperty("mLimit",  m.limit,         nullptr);
        patState.setProperty("mGlue",   m.glue,          nullptr);

        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        {
            juce::ValueTree chState("Ch");
            writeChannel(chState, sequencer.patterns[p].channels[i]);
            patState.appendChild(chState, nullptr);
        }
        state.appendChild(patState, nullptr);
    }

    state.appendChild(midiLearn.saveState(), nullptr);

    juce::MemoryOutputStream mos(dest, false);
    state.writeToStream(mos);
}

void DrumSequencerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::ValueTree state = juce::ValueTree::readFromData(data, (size_t)sizeInBytes);
    if (!state.isValid()) return;

    sequencer.dawSync        = (bool)state.getProperty("dawSync",  false);
    sequencer.standaloneBpm  = (float)state.getProperty("bpm",     120.0f);
    sequencer.timeSigNum     = (int)  state.getProperty("tsNum",   4);
    sequencer.timeSigDen     = (int)  state.getProperty("tsDen",   4);
    sequencer.currentPattern = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1,
                                            (int)state.getProperty("curPattern", 0));
    sequencer.playPattern = sequencer.currentPattern;   // start playback from the restored pattern
    followPlayback = (bool) state.getProperty("followPlay", false);
    visibleChannels = (int) state.getProperty("visChans", 8);
    visiblePatterns = juce::jlimit(16, Sequencer::NUM_PATTERNS, (int) state.getProperty("visPats", 16));
    keysMode.store((bool) state.getProperty("keysMode", false));
    auditionOnEdit.store((bool) state.getProperty("audEdit", false));

    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto child = state.getChild(i);

        if (child.getType() == juce::Identifier("Pattern"))
        {
            int p = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, (int)child.getProperty("index", 0));
            sequencer.patterns[p].swing        = (float)child.getProperty("swing", 0.0f);
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
            m.delaySync     = (bool) child.getProperty("delSync", false);
            m.delayDivision = (int)  child.getProperty("delDiv",  4);
            m.delayPingPong = (bool) child.getProperty("delPP",   false);
            m.volume        = (float)child.getProperty("mVol",    0.9f);
            m.pan           = (float)child.getProperty("mPan",    0.0f);
            m.mono          = (bool) child.getProperty("mMono",   false);
            m.limit         = (float)child.getProperty("mLimit",  0.003f);
            m.glue          = (float)child.getProperty("mGlue",   0.0f);

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

    launchpadRefreshPending.store(true);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumSequencerProcessor();
}
