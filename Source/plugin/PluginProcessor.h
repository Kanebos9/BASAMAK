#pragma once
#include <JuceHeader.h>
#include "Sequencer.h"
#include "LaunchpadController.h"
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

    //-- Public state (accessed from editor on message thread — use mutex)
    Sequencer           sequencer;
    LaunchpadController launchpad;
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

    // The whole synth engine renders at this internal oversampling factor (anti-aliasing),
    // then the processor downsamples to the host rate. renderInto/channels are unaware (they
    // just run at the higher sr). Reverb/delay/master run at the host rate after downsampling.
    // "Keys" mode: incoming MIDI notes play channel sounds (each channel listens on its MIDI note),
    // instead of being read as Launchpad pads. Off by default so the Launchpad keeps working.
    std::atomic<bool> keysMode { false };

    // Auto-audition: when ON, releasing a slot knob/fader fires a TEST hit so you hear the edit. ON by
    // default (user request); toggled from the top bar. A fresh instance starts ON; saved state restores.
    std::atomic<bool> auditionOnEdit { true };

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

    // Launchpad device name (partial match)
    juce::String launchpadDeviceName = "Launchpad Mini MK3";

    // Whether any channel is soloed
    bool anySolo = false;

    // Last channel the editor had selected. Lives on the processor (not the
    // editor) so it survives the editor being destroyed/recreated when the
    // user switches away from the plugin window and back.
    int lastSelectedChannel = 0;
    bool followPlayback = false;   // global (whole-instrument): editor view follows the playing pattern
    int  visibleChannels = 8;      // how many channel rows the editor shows (4/8/12/16); UI-only
    int  visiblePatterns = 16;     // how many patterns the editor shows/uses (16/24/32); UI-only

    // Page B state per channel (which 8 steps are visible on Launchpad)
    bool channelPageB[8] = {};

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

    // Called by editor to trigger a Launchpad refresh
    void requestLaunchpadRefresh() { launchpadRefreshPending.store(true); }

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

    // Export sequence as MIDI file for drag-to-DAW
    juce::File exportMidiFile();

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

    std::atomic<bool> launchpadRefreshPending { true };
    std::atomic<bool> launchpadInitPending    { true };

    // MIDI output buffer to Launchpad (written audio thread, sent by editor)
    juce::CriticalSection launchpadMidiLock;
    juce::MidiBuffer pendingLaunchpadMidi;

    void routeCC(const juce::MidiMessage& msg);
    void updateAnySolo();
    void processDelay(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples);
    void processReverb(juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& send, int numSamples);
    void sendLaunchpadRefresh(const juce::Array<Sequencer::TriggerEvent>& events);
};
