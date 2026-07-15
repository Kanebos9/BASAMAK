#pragma once
#include <JuceHeader.h>
#include "Sequencer.h"
#include "MidiLearnManager.h"
#include "../dsp/FDNReverb.h"

//==============================================================================
class DrumSequencerProcessor : public juce::AudioProcessor
{
public:
    DrumSequencerProcessor();
    ~DrumSequencerProcessor() override;

    // Multi-output routing: Main + this many discrete stereo aux outs. Each channel can be
    // routed to Main or to one of these (for separate processing on its own DAW track).
    static constexpr int NUM_AUX_OUTS = 16;   // one discrete stereo out per channel (16 channels)
    static juce::AudioProcessor::BusesProperties makeBuses();

    //-- AudioProcessor overrides
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "BASAMAK"; }
    bool acceptsMidi()  const override { return true;  }
    bool producesMidi() const override { return true;  }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    // Undo/redo capture + restore the state as a ValueTree DIRECTLY (no binary serialize/deserialize
    // roundtrip - that made the undo/redo buttons feel laggy). getStateInformation just wraps these.
    juce::ValueTree captureStateTree();
    void applyStateTree(const juce::ValueTree& state);

    //-- Public state (accessed from editor on message thread — use mutex)
    Sequencer           sequencer;
    MidiLearnManager    midiLearn;

    // Duplicate a whole pattern (sounds + steps + per-pattern settings) into another slot.
    // Used by the pattern-bar drag-copy. Runs on the message thread.
    void copyPattern(int src, int dst);

    // Copy one channel's whole sound + steps onto another channel within a pattern (drag-copy on
    // the strip number). Output routing is preserved on the destination (routing is channel-wide).
    void copyChannel(int pat, int src, int dst);

    // Set true (from audio thread) when an incoming MIDI CC switches the current
    // pattern, so the editor can refresh its UI on the next timer tick.
    std::atomic<bool> patternChangedByMidi { false };

    // Auto-audition: when ON, releasing a slot knob/fader fires a TEST hit so you hear the edit. ON by
    // default (user request); toggled from the top bar. A fresh instance starts ON; saved state restores.
    std::atomic<bool> auditionOnEdit { true };

    // The whole synth engine renders at this internal oversampling factor (anti-aliasing),
    // then the processor downsamples to the host rate. renderInto/channels are unaware (they
    // just run at the higher sr). Reverb/delay/master run at the host rate after downsampling.
    static constexpr int kEngineOS = 2;
    std::unique_ptr<juce::dsp::Oversampling<float>> engineOS;
    double spectrumRate() const { return currentSampleRate * kEngineOS; }   // analysis tap runs at OS rate

    // Master output level meter (post master stage). Audio thread writes, UI reads.
    std::atomic<float> masterMeterL { 0.0f };
    std::atomic<float> masterMeterR { 0.0f };

    // Live spectrum analyser of one channel (the one the editor is inspecting)
    SpectrumTap       spectrumTap;
    std::atomic<int>  analyzeChannel { 0 };
    std::atomic<int>  analysisSlot   { -1 };   // PER-SLOT EQ: spectrum slot (-1 = mix/All)
    int               analysisArmCtr = 0;      // re-arm the spectrum periodically while stopped (audio thread)
    // ONE shared per-slot-analysis scratch (only one channel is ever analysed at a time);
    // the analysed channel's analysisBuf points here per block. Replaced a 32 KB array on
    // every channel of every pattern (~16 MB of idle RAM).
    std::vector<float> analysisScratch;

    // Whether any channel is soloed
    bool anySolo = false;

    // Last channel the editor had selected. Lives on the processor (not the
    // editor) so it survives the editor being destroyed/recreated when the
    // user switches away from the plugin window and back.
    int lastSelectedChannel = 0;
    bool followPlayback = false;   // global (whole-instrument): editor view follows the playing pattern
    int  visibleChannels = 8;      // how many channel rows the editor shows (4/8/12/16); UI-only
    int  visiblePatterns = 32;     // always 32 now (kept persisted for old files); UI-only
    // Keyboard KEY GUIDE (display only): dims out-of-scale keys on the KEYS piano.
    int  kbGuideMode = 0;          // 0 = off, 1 = follow the selected channel's slot SCALE, 2 = custom key+scale
    int  kbGuideKey = 0, kbGuideScale = 0;

    // A fresh STANDALONE must open at FACTORY DEFAULTS, not restore its last session (the JUCE
    // standalone auto-persists + reloads state on launch). We skip that ONE startup restore - it
    // happens before the editor is ever created - while still allowing preset-file loads + undo
    // (which call setStateInformation later, after the editor exists). A VST is untouched: a fresh
    // instance gets no state, a SAVED project restores normally. Set true in createEditor().
    bool uiCreatedOnce = false;

    // Reverb / Delay engines (the FX are shared; their KNOB VALUES are stored
    // per-pattern in Sequencer::Pattern::master, see Sequencer.h).
    FDNReverb fdn;                       // modulated FDN reverb, bus A (smoother than Freeverb)
    FDNReverb fdnB;                      // bus B (its own mode/size/decay - e.g. Hall keys / Room drums)
    float limiterGain    = 1.0f;        // limiter gain-reduction envelope (audio thread state)
    float masterGlueEnv  = 0.0f;        // master "glue" compressor detector envelope (audio thread state)
    float masterWidthLp  = 0.0f;        // [2026-07-15 12:10] master WIDTH: side-signal ~120 Hz LP (bass-safe widening)
    float masterTiltL    = 0.0f;        // master tilt-EQ one-pole low-band state (L/R)
    float masterTiltR    = 0.0f;
    float satDcX[2]      = { 0.0f, 0.0f };  // master saturation DC-blocker state (last in, per channel)
    float satDcY[2]      = { 0.0f, 0.0f };  // (last out) - removes the asymmetric-shaper bias
    // Convenience accessor for the current pattern's master settings.
    Sequencer::MasterFX& masterFX() { return sequencer.current().master; }

    // Latest known tempo + time signature (host when DAW-synced, else the
    // plugin's own values). Used for the synced delay and the bar-length display.
    double currentBpm = 120.0;
    int    arpFireCount = 0;   // alternate-strum stroke counter (reset at startArp)
    int    arpLastStampIdx = -1, arpLastStampPat = -1;   // recording: trim the previous stamp (no overlaps, no snapping)
    TunerTap tunerTap;         // continuous audio ring for the LIVE TUNE strip (editor analyses it)
    int    currentTimeSigNum = 4;
    int    currentTimeSigDen = 4;

    // Called by editor to toggle a step
    void toggleStep(int channel, int step);

    // Called by editor to trigger standalone play/stop. Stop also cuts any
    // ringing voice tails (handled on the audio thread via silenceRequest).
    void standalonePlay()  { sequencer.startStandalone(); }
    void standaloneStop()  { sequencer.stopStandalone(); silenceRequest.store(true); }

    // Audition: fire one channel with its current settings (TEST button).
    std::atomic<int> testTriggerRequest { -1 };
    void requestTestTrigger(int ch) { testTriggerRequest.store(ch); }

    // Stop -> immediately silence all ringing voices (audio-thread acts on it).
    std::atomic<bool> silenceRequest { false };

    // UI-only controls assigned to MIDI: the processor can't touch editor state,
    // so it parks the request here and the editor's timer applies it.
    std::atomic<int> uiMidiEditMode  { -1 };  // 1=Vel 2=Pitch 3=Prob 4=Roll (editor toggles)
    std::atomic<int> uiMidiInfluence { -1 };  // channel index to toggle step-influence on
    // MIDI sound browsing (ui_sound_next/prev = the SELECTED channel's Sound Bank pick): +-1
    // steps, drained + rate-limited by the editor (excess DROPPED, never queued). Knob decodes
    // (absolute / relative / 14-bit fine) were built and REMOVED at the user's order - see
    // docs/HISTORY.md; buttons ARE motion, so they browse without walls.
    std::atomic<int> uiMidiSoundStep { 0 };
    // NEXT/PREV hold-to-repeat: +-1 while a pad is held (editor repeats a step per rate window).
    std::atomic<int> uiSoundHold { 0 };
    std::atomic<juce::uint32> uiSoundHoldMs { 0 };   // press time (repeat starts after ~0.45 s)
    // SELECTED-SCOPE MIDI CONTROLS (ui_sel_*): routeCC pushes (target, value) events here; the
    // editor's timer drains and applies them to the CURRENT selection (pattern/channel/slot)
    // through the same code paths the on-screen controls use. SPSC ring (keyQ pattern): the
    // audio thread writes, the editor reads. Full ring = events dropped (the UI catches up).
    enum SelCC : int { SelFxDrive = 0, SelFxRev, SelFxDel, SelChFxAmtA, SelFxSub, SelFxPunch, SelChFxAmtB,
                       SelEnvA, SelEnvH, SelEnvD, SelEnvS, SelEnvR,
                       SelUniCount, SelUniDet, SelUniVib, SelUniWidth, SelUniDrift,
                       SelStrum, SelMinVel, SelMaxVel, SelGlide, SelSlotOfs,
                       SelRec, SelMute, SelSolo, SelOverlap, SelSlotSel,
                       SelChNext, SelChPrev, SelPatNext, SelPatPrev,
                       SelFollow, SelTest, SelChVol, SelSwing, SelBpm, SelUndo, SelRedo,
                       SelSlotFreq, SelSlotFmAmt, SelSlotWarp,   // the selected slot's Osc faders
                       SelChFxChrA, SelChFxChrB, SelFxRing, SelFxFormant, SelFxRingHz,   // CHANNEL FX A/B Character + slot Ring/Formant/RingHz
                       SelChFxAmtC, SelChFxChrC,                 // CHANNEL FX slot C
                       SelSlotPan,                               // static slot pan (selected slot)
                       SelVolReset, SelKeysView, SelView16, SelEditorToggle, SelOthersVol,   // title-strip controls [2026-07-15 22:30]
                       SelSlotPBase = 1000,    // 1000 + N = the N-th knob of the selected slot's engine grid
                       SelStepBase  = 2000,    // 2000 + N = step N on the selected channel
                       SelModAmtBase = 3000 }; // [2026-07-14 11:10] 3000 + R = mod route R's AMOUNT (selected slot)
    struct SelCCEvt { int t; float v; };
    SelCCEvt selQ[64];
    std::atomic<int> selQHead { 0 }, selQTail { 0 };
    std::atomic<bool> uiMasterCcDirty { false };   // a master CC landed: editor refreshes the knobs
    void pushSelCC(int t, float v)
    {
        const int h = selQHead.load(std::memory_order_relaxed), n = (h + 1) & 63;
        if (n == selQTail.load(std::memory_order_acquire)) return;
        selQ[h] = { t, v }; selQHead.store(n, std::memory_order_release);
    }

    // MIDI-in monitor (drives the on-screen "MIDI" indicator). midiInCount bumps
    // for EVERY incoming message so the UI can tell whether MIDI reaches us at all.
    std::atomic<uint32_t> midiInCount { 0 };
    std::atomic<int> lastCcNum { -1 }, lastCcVal { -1 }, lastCcChan { -1 };
    juce::AudioProcessLoadMeasurer loadMeasurer;                   // [2026-07-14 01:50] CPU readout: our share of the audio budget
    std::atomic<int> lastAtVal { 0 }, lastAtChan { 0 };            // [2026-07-14] aftertouch monitor
    std::atomic<juce::uint32> lastAtMs { 0 };                      // ...expires ~1.5 s after the last AT message

    // ==== KEYS (the on-screen keyboard panel) =====================================
    // Note events from the editor's piano (message thread) -> audio thread via a lock-free SPSC
    // RING (POLY: several presses/releases can land in one block; the old single-atomic handshake
    // dropped all but the last). Producer = message thread ONLY (incoming MIDI on the audio thread
    // calls the handler directly, keeping this strictly single-producer). Audio thread drains all
    // pending events each block, in order, onto the selected channel via keyDown/keyUp(note).
    struct KeyQEvt { uint8_t down, note, vel, chan; };   // vel 1..127 (downs only); chan = MIDI ch 1-16, 0 = on-screen [2026-07-13 21:20 MPE]
    static constexpr int KEYQ = 64;
    KeyQEvt keyQ[KEYQ];
    std::atomic<uint32_t> keyQHead { 0 }, keyQTail { 0 };   // head = write (message), tail = read (audio)
    std::atomic<int>      keysSlot2Down { 0 };     // slot-2 transpose in semitones (+down/-up), persisted
    // [2026-07-13 21:20] MPE / AFTERTOUCH: per-MIDI-channel pitch-bend range (RPN 0 programmable;
    // defaults: ch 1 = 2 st = a normal keyboard / the MPE manager, ch 2-16 = 48 st = MPE members),
    // and the expression sweep that fans pressure / slide / bend onto matching key voices everywhere
    // (the handleKeyUp all-patterns precedent; non-matching voices cost one compare each).
    uint8_t rpnMsb_[16] = { 0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f };
    uint8_t rpnLsb_[16] = { 0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f };
    float   bendRange_[16] = { 2, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48 };
    void expressionSweep(int type, int midiChan, float val, int note = -1)
    {
        for (auto& p : sequencer.patterns)
            for (auto& c : p.channels) c.applyExpression(type, midiChan, val, note);
    }
    void pushKeyDown(int note, float vel01)
    {
        const uint32_t h = keyQHead.load(std::memory_order_relaxed);
        if (h - keyQTail.load(std::memory_order_acquire) >= (uint32_t) KEYQ) return;   // full: drop (never blocks)
        keyQ[h % KEYQ] = { 1, (uint8_t)(note & 0x7f),
                           (uint8_t) juce::jlimit(1, 127, (int) std::lround(vel01 * 127.0f)), 0 };
        keyQHead.store(h + 1, std::memory_order_release);
    }
    void pushKeyUp(int note)
    {
        const uint32_t h = keyQHead.load(std::memory_order_relaxed);
        if (h - keyQTail.load(std::memory_order_acquire) >= (uint32_t) KEYQ) return;
        keyQ[h % KEYQ] = { 0, (uint8_t)(note & 0x7f), 0, 0 };
        keyQHead.store(h + 1, std::memory_order_release);
    }

    // -- KEYS recording (audio thread stamps steps + logs take events; editor assembles takes) --
    // The REFERENCE is FIXED: C3 (MIDI 60) = step pitch 0; the piano's C1..C7 range = +/-36 st.
    std::atomic<bool> keysRecording { false };     // live: key presses are written into steps
    std::atomic<int>  keysRecMode { 0 };           // 0/1 = this pattern (key-start / count-in), 2/3 = chain
    std::atomic<int>  keysArmedPattern { -1 };     // the pattern armed by "this pattern" modes
    // Fixed-capacity take-event log (audio thread appends, editor's timer drains). flags bit0 =
    // MERGE continuation (the held key crossed into this step). pattern 0xFF = a LOOP BOUNDARY
    // marker: in "this pattern" modes each pattern loop is its OWN take, and the channel restarts
    // CLEAN (steps off / pitch 0 / unmerged) for the next pass.
    struct KeyEvt { uint8_t pattern, step; int8_t semis; uint8_t flags; };
    static constexpr int KEYS_EVT_CAP = 8192;
    KeyEvt keysEvts[KEYS_EVT_CAP];
    std::atomic<int> keysEvtCount { 0 };
    // POLY held-note tracking (audio thread only): press-order stack + velocities; the ATOMIC mask
    // mirrors it for the UI (keyboard highlight unions every held note). keysHeldNote stays the
    // MOST RECENT still-held note = the mono projection recording follows.
    int   keysHeldStack[DrumChannel::POLY * 2] = {}; float keysHeldStackVel[DrumChannel::POLY * 2] = {};
    int   keysHeldCount = 0;
    std::atomic<uint64_t> keysHeldMaskLo { 0 }, keysHeldMaskHi { 0 };   // bit n = MIDI note n held
    std::atomic<int> keysHeldNote { -1 };          // audio-thread writes; the held key (for auto-merge)
    std::atomic<float> keysHeldVel { 1.0f };       // velocity of the held key (for DRAW recording of per-column velocity)
    std::atomic<int> keysLastStampStep { -1 };     // last step stamped for the held key (auto-merge chain)
    std::atomic<int> keysDrawLastCol   { -1 };     // DRAW recording: last column written (unquantized lane capture)
    std::atomic<int> keysLoopSeen { -1 };          // loopCount at the last take boundary (-1 = reset)
    std::atomic<int> keysLastPlayPat { -1 };       // playPattern at the last boundary (chain-mode splits)
    // ARP runtime (audio thread only; drives the played channel). See DrumChannel arp fields.
    int    arpRoot = -1;       // MIDI note driving the arp (-1 = inactive)
    float  arpVel  = 0.8f;     // velocity of the held root
    int    arpChan = -1;       // channel the arp runs on
    int    arpStep = 0;        // 0 = root, 1..arpLen-1 = rows
    double arpAcc  = 0.0;      // host samples accumulated since the last arp note
    int    arpSounding = -1;   // MIDI note currently sounding (-1 = none / rest)
    std::atomic<int> arpSoundingUi { -1 };   // UI mirror: the keyboard highlight lights THIS note as it fires
    bool   arpRootHeld = false;  // the root key is PHYSICALLY down (Hold keeps the arp running after release)
    long   arpNoteAge  = 0;      // samples since the last arp fire (drives the Gate cut)
    // Recorded TAKES (message thread only; PERSISTED in the plugin state, so presets keep them).
    // A take = the events of one loop pass (or one whole chain-mode session). Loading = clear the
    // channel in every pattern the take touches, then replay its events.
    struct KeysTake { juce::String name; int channel = 0; std::vector<KeyEvt> evts;
                      bool isDraw = false; int drawPat = 0;
                      std::vector<DrumChannel::DrawNote> drawNotes; };  // piano-roll takes store the NOTE LIST
    std::vector<KeysTake> keysTakes;
    static constexpr int KEYS_TAKES_MAX = 1000;   // hard cap on total takes kept per preset (20 per pattern+channel)
    // PIANO-ROLL take handshake (audio thread snapshots a finished loop's notes -> editor drains into a take).
    std::atomic<bool> keysDrawTakeReady { false };
    std::atomic<int>  keysDrawTakeChan  { -1 };
    std::atomic<int>  keysDrawTakePat   { -1 };
    // Group takes hold the WHOLE pass in CONCAT columns (note start += bar*384; drawPat = the head).
    static constexpr int DRAW_TAKE_MAX = DrumChannel::DRAW_MAX_NOTES * 8;
    DrumChannel::DrawNote keysDrawTakeNotes[DRAW_TAKE_MAX] = {};
    int keysDrawTakeCount = 0;
    // POLY piano-roll recording: each held stack entry may have an OPEN note in the recording
    // channel's list (grown per block until the key releases). -1 = none. Audio thread only.
    // keysHeldOpenPat = the PATTERN the note was opened in: in a merged group later bars keep
    // GROWING the same note (len crosses the bar line) instead of closing + reopening it.
    int keysHeldOpenIdx[DrumChannel::POLY * 2] = {};
    int keysHeldOpenPat[DrumChannel::POLY * 2] = {};
    int keysHeldOpenChan[DrumChannel::POLY * 2] = {};   // MERGE&SPLIT: which channel's roll the note opened in

    // Audio-callback heartbeat: bumped at the top of every processBlock. The editor watches it -
    // if it stops moving, the HOST isn't sending us audio (device off/missing, FX offline), which
    // freezes the whole plugin (transport, TEST, meters). The Play button's tooltip then explains
    // why "nothing plays" instead of leaving the user thinking the plugin is broken (this exact
    // confusion cost a debugging session: Reaper was pointed at an unplugged interface).
    std::atomic<uint32_t> processHeartbeat { 0 };

    // Export sequence as MIDI file for drag-to-DAW
    juce::File exportMidiFile(int channel);   // Drag MIDI: the SELECTED channel only, as a melody

private:
    double currentSampleRate  = 44100.0;
    int    currentBlockSize   = 512;

    // Delay line. The feedback path is filtered (one-pole LP + HP per channel) so each repeat
    // darkens + sheds low-end mud, like an analog/tape delay - the biggest "cheap -> musical" upgrade.
    juce::AudioBuffer<float> delayBuffer;
    int delayWriteHead = 0;
    float delayFbLp[2] = { 0.0f, 0.0f };   // LP state per channel (feedback darkening)
    float delayFbHp[2] = { 0.0f, 0.0f };   // HP state per channel (feedback mud removal)
    juce::AudioBuffer<float> delayBufferB;  // BUS B delay line + state
    int delayWriteHeadB = 0;
    float delayFbLpB[2] = { 0.0f, 0.0f }, delayFbHpB[2] = { 0.0f, 0.0f };
    // [2026-07-15 16:30] STOP BUS KILL: Stop ramps the Main output to zero over ~100 ms (only wet
    // tails remain by then) and wipes the reverb/delay states while silent - one press = clickless
    // full silence (user; supersedes the ring-out-on-stop behaviour).
    int busKillRemain = 0, busKillTotal = 0;
    void wipeFxBuses();
    // [2026-07-15 13:30] SMOOTHED time/size: the delay read-offset GLIDES to a changed target
    // (fader drag or live tempo change) = tape-style pitch swoop instead of a crackling jump;
    // the reverb size slews ~50 ms. -1 = snap on first block. Converged = bit-identical paths.
    float delayTimeSm = -1.0f, delayTimeSmB = -1.0f;   // in samples
    float revSizeSm   = -1.0f, revSizeSmB   = -1.0f;   // 0..1 room size
    // [2026-07-15 00:50] DELAY MODES per-bus state: Analog's wobble LFO phase + Shimmer's octave-up
    // ring per channel (the FDN reverb's 2-tap crossfaded varispeed shifter, one per bus/channel).
    float delayWobPh = 0.0f, delayWobPhB = 0.0f;
    std::vector<float> delayShRing[2], delayShRingB[2];    // sized in prepareToPlay (~85 ms)
    int    delayShW[2] = { 0, 0 },   delayShWB[2] = { 0, 0 };
    double delayShPh[2] = { 0.0, 0.0 }, delayShPhB[2] = { 0.0, 0.0 };

    // True per-channel FX sends. Channels sum their post-fader signal x send into these at the
    // OVERSAMPLED rate (rendered alongside the engine); the processor averages them down to the
    // host rate, then reverb/delay process the down-rate sum and add the wet back to Main.
    juce::AudioBuffer<float> reverbSendOS, delaySendOS;     // accumulation buffers (host rate x kEngineOS)
    juce::AudioBuffer<float> reverbSendBase, delaySendBase; // down-sampled to host rate
    juce::AudioBuffer<float> reverbSendOSB, delaySendOSB;   // BUS B accumulation (OS rate)
    juce::AudioBuffer<float> reverbSendBaseB, delaySendBaseB;
    // [2026-07-15 02:30] one bar in seconds from the CURRENT clock (host tempo+sig when DAW-synced,
    // the toolbar values standalone) - the sync reference for delay Per-bar / reverb bars / gate.
    double barSeconds() const
    {
        const double bpm   = currentBpm > 1.0 ? (double) currentBpm : 120.0;
        const double beats = (double) juce::jmax(1, currentTimeSigNum) * 4.0 / (double) juce::jmax(1, currentTimeSigDen);
        return beats * 60.0 / bpm;
    }
    // Reverb GATE (per bus): samples the gate stays open after a hit + the smoothed gain (5 ms fades).
    int   revGateHold = 0,  revGateHoldB = 0;
    float revGateSm   = 0.0f, revGateSmB  = 0.0f;
    std::vector<uint8_t> revGateTrig;                       // per-sample "a hit fed the reverb" scratch
    // Delay DUCK: the dry-mix key envelope (captured in processBlock BEFORE any wet returns) + the
    // per-bus smoothed duck gain.
    std::vector<float> duckKeyEnv;                          // per-sample key level (attack ~10 ms / release ~200 ms)
    float duckKeyState = 0.0f;
    // Delay TRAIL: a parallel "echo age" line per bus (age 1 = first repeat; the feedback write
    // stores age+1, and a repeat older than the cap is not fed back = hard echo count).
    juce::AudioBuffer<float> delayAgeBuf, delayAgeBufB;
    juce::AudioBuffer<float> reverbPreBuffer;               // reverb pre-delay line (0..120 ms free / up to 1/8 bar synced)
    int reverbPreHead = 0;
    juce::AudioBuffer<float> reverbPreBufferB;              // bus B pre-delay line
    int reverbPreHeadB = 0;

    // Master limiter lookahead: a short delay so the gain dips BEFORE a transient (cleaner catch).
    // Always delays by limiterLAlen samples (reported as latency) so the latency stays constant.
    juce::AudioBuffer<float> limiterLA;
    int limiterLAhead = 0, limiterLAlen = 0;

    // MIDI-out note tracking (audio thread). A held note per channel + the samples left until its
    // note-off, so MIDI-out notes sustain for the channel's amp-envelope length, not one block.
    int activeMidiNote   [Sequencer::NUM_CHANNELS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
    int activeMidiChan   [Sequencer::NUM_CHANNELS] = { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };  // channel each held note went out on
    int midiNoteCountdown[Sequencer::NUM_CHANNELS] = {};

    void routeCC(const juce::MidiMessage& msg);
    void updateAnySolo();
    void processDelay(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples, bool busB = false);
    void processReverb(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples, bool busB = false);
};
