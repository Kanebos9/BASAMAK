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
    int  visiblePatterns = 16;     // how many patterns the editor shows/uses (16/24/32); UI-only

    // A fresh STANDALONE must open at FACTORY DEFAULTS, not restore its last session (the JUCE
    // standalone auto-persists + reloads state on launch). We skip that ONE startup restore - it
    // happens before the editor is ever created - while still allowing preset-file loads + undo
    // (which call setStateInformation later, after the editor exists). A VST is untouched: a fresh
    // instance gets no state, a SAVED project restores normally. Set true in createEditor().
    bool uiCreatedOnce = false;

    // Reverb / Delay engines (the FX are shared; their KNOB VALUES are stored
    // per-pattern in Sequencer::Pattern::master, see Sequencer.h).
    FDNReverb fdn;                       // modulated FDN reverb (smoother than Freeverb)
    float limiterGain    = 1.0f;        // limiter gain-reduction envelope (audio thread state)
    float masterGlueEnv  = 0.0f;        // master "glue" compressor detector envelope (audio thread state)
    float masterTiltL    = 0.0f;        // master tilt-EQ one-pole low-band state (L/R)
    float masterTiltR    = 0.0f;
    float satDcX[2]      = { 0.0f, 0.0f };  // master saturation DC-blocker state (last in, per channel)
    float satDcY[2]      = { 0.0f, 0.0f };  // (last out) - removes the asymmetric-shaper bias
    // Convenience accessor for the current pattern's master settings.
    Sequencer::MasterFX& masterFX() { return sequencer.current().master; }

    // Latest known tempo + time signature (host when DAW-synced, else the
    // plugin's own values). Used for the synced delay and the bar-length display.
    double currentBpm = 120.0;
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

    // MIDI-in monitor (drives the on-screen "MIDI" indicator). midiInCount bumps
    // for EVERY incoming message so the UI can tell whether MIDI reaches us at all.
    std::atomic<uint32_t> midiInCount { 0 };
    std::atomic<int> lastCcNum { -1 }, lastCcVal { -1 }, lastCcChan { -1 };

    // ==== KEYS (the on-screen keyboard panel) =====================================
    // Note events from the editor's piano (message thread) -> audio thread, MONO. Packed
    // (gen << 16) | (midiNote << 8) | velocity7bit; gen bumps every event so re-pressing the
    // same note retriggers. keysUpEvt carries just a gen. The audio thread plays them on the
    // SELECTED channel via DrumChannel::keyDown/keyUp.
    std::atomic<uint32_t> keysDownEvt { 0 }, keysUpEvt { 0 };
    std::atomic<int>      keysSlot2Down { 0 };     // slot-2 transpose DOWN in semitones (0..24), persisted
    void pushKeyDown(int note, float vel01)
    {
        const uint32_t gen = ((keysDownEvt.load(std::memory_order_relaxed) >> 16) + 1) & 0xffff;
        keysDownEvt.store((gen << 16) | ((uint32_t)(note & 0x7f) << 8)
                          | (uint32_t) juce::jlimit(1, 127, (int) std::lround(vel01 * 127.0f)),
                          std::memory_order_release);
    }
    // Key-up carries WHICH note was released ((gen << 8) | note). The audio thread only
    // applies it if that note is still the held one: a keyboard SLIDE emits up(old) before
    // down(new) on the message thread, but both land in the same audio block and the block
    // processes down first - an unchecked up would kill the freshly slid-to note (heard as
    // "Analog+FM only plays the first key"; Phys/Modal masked it with their natural tails).
    void pushKeyUp(int note)
    {
        const uint32_t gen = ((keysUpEvt.load(std::memory_order_relaxed) >> 8) + 1) & 0xffffff;
        keysUpEvt.store((gen << 8) | (uint32_t)(note & 0x7f), std::memory_order_release);
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
    uint32_t keysDownSeen = 0, keysUpSeen = 0;     // audio-thread only: last processed event gens
    std::atomic<int> keysHeldNote { -1 };          // audio-thread writes; the held key (for auto-merge)
    std::atomic<int> keysLastStampStep { -1 };     // last step stamped for the held key (auto-merge chain)
    std::atomic<int> keysDrawLastCol   { -1 };     // DRAW recording: last column written (unquantized lane capture)
    std::atomic<int> keysLoopSeen { -1 };          // loopCount at the last take boundary (-1 = reset)
    std::atomic<int> keysLastPlayPat { -1 };       // playPattern at the last boundary (chain-mode splits)
    // Recorded TAKES (message thread only; PERSISTED in the plugin state, so presets keep them).
    // A take = the events of one loop pass (or one whole chain-mode session). Loading = clear the
    // channel in every pattern the take touches, then replay its events.
    struct KeysTake { juce::String name; int channel = 0; std::vector<KeyEvt> evts;
                      bool isDraw = false; int drawPat = 0; std::vector<int8_t> drawLane; };  // draw takes store the lane
    std::vector<KeysTake> keysTakes;
    static constexpr int KEYS_TAKES_MAX = 1000;   // hard cap on total takes kept per preset (20 per pattern+channel)
    // DRAW-take handshake (audio thread snapshots a finished loop's lane -> editor drains into a take).
    std::atomic<bool> keysDrawTakeReady { false };
    std::atomic<int>  keysDrawTakeChan  { -1 };
    std::atomic<int>  keysDrawTakePat   { -1 };
    int8_t keysDrawTakeBuf[DrumChannel::DRAW_RES] = {};

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

    // True per-channel FX sends. Channels sum their post-fader signal x send into these at the
    // OVERSAMPLED rate (rendered alongside the engine); the processor averages them down to the
    // host rate, then reverb/delay process the down-rate sum and add the wet back to Main.
    juce::AudioBuffer<float> reverbSendOS, delaySendOS;     // accumulation buffers (host rate x kEngineOS)
    juce::AudioBuffer<float> reverbSendBase, delaySendBase; // down-sampled to host rate
    juce::AudioBuffer<float> reverbPreBuffer;               // reverb pre-delay line (0..120 ms)
    int reverbPreHead = 0;

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
    void processDelay(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples);
    void processReverb(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples);
};
