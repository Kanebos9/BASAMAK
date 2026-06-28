#pragma once
#include <JuceHeader.h>
#include "SpectrumTap.h"
#include <complex>

//==============================================================================
// A single biquad (RBJ cookbook). Transposed Direct Form II, stereo state.
// Hand-rolled so the filtering path is fully under our control.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float  z1[2] = { 0, 0 }, z2[2] = { 0, 0 };

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }

    inline float process(float x, int ch) noexcept
    {
        double y = b0 * x + z1[ch];
        z1[ch] = (float)(b1 * x - a1 * y + z2[ch]);
        z2[ch] = (float)(b2 * x - a2 * y);
        return (float)y;
    }

    void setNorm(double B0, double B1, double B2, double A0, double A1, double A2)
    {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }

    void peaking(double sr, double f, double Q, double dB)
    {
        double A = std::pow(10.0, dB / 40.0);
        double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * juce::jmax(0.02, Q));   // clamp Q>0: Q=0 -> divide-by-zero -> NaN coeffs -> dead track
        setNorm(1 + alpha * A, -2 * cw, 1 - alpha * A,
                1 + alpha / A, -2 * cw, 1 - alpha / A);
    }

    void highpass(double sr, double f, double Q)
    {
        double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * juce::jmax(0.02, Q));   // clamp Q>0: Q=0 -> divide-by-zero -> NaN coeffs -> dead track
        setNorm((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void lowpass(double sr, double f, double Q)
    {
        double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * juce::jmax(0.02, Q));   // clamp Q>0: Q=0 -> divide-by-zero -> NaN coeffs -> dead track
        setNorm((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void bandpass(double sr, double f, double Q) // constant 0 dB peak
    {
        double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * juce::jmax(0.02, Q));   // clamp Q>0: Q=0 -> divide-by-zero -> NaN coeffs -> dead track
        setNorm(alpha, 0.0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void notch(double sr, double f, double Q)
    {
        double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2.0 * juce::jmax(0.02, Q));   // clamp Q>0: Q=0 -> divide-by-zero -> NaN coeffs -> dead track
        setNorm(1.0, -2 * cw, 1.0, 1 + alpha, -2 * cw, 1 - alpha);
    }

    // Magnitude response at frequency f (for drawing the EQ curve)
    double magnitudeAt(double f, double sr) const
    {
        double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        std::complex<double> z1c = std::polar(1.0, -w);
        std::complex<double> z2c = std::polar(1.0, -2.0 * w);
        std::complex<double> num = b0 + b1 * z1c + b2 * z2c;
        std::complex<double> den = 1.0 + a1 * z1c + a2 * z2c;
        return std::abs(num / den);
    }
};

//==============================================================================
// AHDSR envelope (Attack, Hold, Decay, Sustain, Release). Hold keeps the peak
// for a time before decaying — useful for one-shot samples where attack is
// instant. Times in seconds.
struct Envelope
{
    enum Stage { Idle, Attack, Hold, Decay, Sustain, Release };

    void setSampleRate(double s) { sr = s > 0 ? s : 44100.0; }
    void setParams(float A, float H, float D, float S, float R) { a = A; h = H; d = D; s = S; r = R; }
    void reset() { stage = Idle; level = 0; holdCtr = 0; }
    void noteOn() { stage = Attack; }
    void noteOff()
    {
        if (stage != Idle && stage != Release)
        {
            relStep = (r > 0.0f) ? level / (float)(r * sr) : level;
            if (relStep <= 0.0f) relStep = 1.0e-5f;
            stage = Release;
        }
    }
    bool isActive() const { return stage != Idle; }

    float getNext() noexcept
    {
        switch (stage)
        {
            case Attack:  { float st = a > 0 ? 1.0f / (float)(a * sr) : 1.0f;
                            level += st; if (level >= 1.0f) { level = 1.0f; holdCtr = 0; stage = h > 0 ? Hold : Decay; } } break;
            case Hold:    { level = 1.0f; if (++holdCtr >= (long)(h * sr)) stage = Decay; } break;
            case Decay:   { float st = d > 0 ? (1.0f - s) / (float)(d * sr) : 1.0f;
                            level -= st; if (level <= s) { level = s; stage = Sustain; } } break;
            case Sustain: level = s; break;
            case Release: { level -= relStep; if (level <= 0.0f) { level = 0.0f; stage = Idle; } } break;
            default:      level = 0.0f; break;
        }
        return level;
    }

    Stage stage = Idle;
    double sr = 44100.0;
    float a = 0.001f, h = 0.0f, d = 0.1f, s = 0.7f, r = 0.1f;
    float level = 0.0f, relStep = 0.0f;
    long  holdCtr = 0;
};

//==============================================================================
// Generates built-in drum sounds synthetically — no external samples needed.
// Each sound has a category (Kick, Snare, ...) and a variant (808, 909, ...).
class DrumSoundGenerator
{
public:
    enum class Type
    {
        Kick808, Kick909, KickAcoustic, KickSub,
        Snare808, Snare909, SnareAcoustic,
        HatClosed808, HatClosed909, HatOpen808, HatOpen909,
        ClapClassic, Clap909,
        TomLow, TomMid, TomHigh,
        Crash, Ride,
        Cowbell, Clave, Rim
    };

    struct SoundInfo { Type type; juce::String category; juce::String variant; };

    static juce::AudioBuffer<float> generate(Type type, double sampleRate);

    // Metadata for building the UI
    static const std::vector<SoundInfo>& all();
    static juce::StringArray            categories();                 // unique, alphabetical
    static juce::Array<Type>            variantsIn(const juce::String& category);
    static juce::String                 categoryOf(Type);
    static juce::String                 variantOf(Type);

private:
    static juce::AudioBuffer<float> genKick (double sr, float f0, float f1, float pitchDecay,
                                             float ampDecay, float click, float len);
    static juce::AudioBuffer<float> genSnare(double sr, float tone, float toneAmt,
                                             float noiseDecay, float len);
    static juce::AudioBuffer<float> genHat  (double sr, float decay, float len, float bright);
    static juce::AudioBuffer<float> genClap (double sr, float len, float decay);
    static juce::AudioBuffer<float> genTom  (double sr, float f0, float decay, float len);
    static juce::AudioBuffer<float> genCymbal(double sr, float len, float decay, float bright);
    static juce::AudioBuffer<float> genCowbell(double sr);
    static juce::AudioBuffer<float> genClave(double sr);
    static juce::AudioBuffer<float> genRim(double sr);
};

//==============================================================================
class DrumChannel
{
public:
    static constexpr int MAX_STEPS = 32;
    static const int VALID_STEP_COUNTS[];
    static constexpr int NUM_VALID_STEP_COUNTS = 20;

    DrumChannel() { for (int i = 0; i < MAX_STEPS; ++i) { stepVel[i] = 1.0f; stepPitch[i] = 0.0f; stepProb[i] = 1.0f; stepRoll[i] = 1; stepRollDecay[i] = 0.0f; stepNoteLen[i] = 0.25f; stepPan[i] = 0.0f; stepCondLen[i] = 1; stepCondMask[i] = 0; } }

    //-- Sequencer state (per step)
    bool   steps[MAX_STEPS] = {};
    float  stepVel[MAX_STEPS];        // 0..1 velocity (volume) per step (default 1)
    float  stepPitch[MAX_STEPS];      // -24..+24 semitone offset per step (default 0)
    float  stepProb[MAX_STEPS];       // 0..1 chance the step fires (default 1) - legacy, superseded by stepCond*
    int    stepRoll[MAX_STEPS];       // 1..6 ratchet/roll sub-hits per step (default 1)
    float  stepRollDecay[MAX_STEPS];  // 0..1 roll fade: 0 = all hits equal, 1 = last hit silent
    float  stepNoteLen[MAX_STEPS];    // 0..1 -> note length 0.1..4 steps (MIDI-out only; ignored for sound)
    float  stepPan[MAX_STEPS];        // -1..+1 stereo pan per step (default 0 = centre; internal sounds only)
    // Per-step LOOP condition (the "Prob" mode): the step fires only on certain pattern loops. stepCondLen = the
    // cycle length N (1 = every loop = default); stepCondMask = bitmask of which loops (0-based) within the cycle fire.
    int    stepCondLen[MAX_STEPS];    // 1..10 (1 = always)
    int    stepCondMask[MAX_STEPS];   // bit b set = fire on loop b of the cycle (0 = no restriction = every loop)
    int    numSteps          = 8;
    int    midiNote          = 36; // default C2, overridden per channel
    int    ccRangeStart      = 1;  // first CC number for this channel

    //-- Channel identity
    juce::String channelName;
    juce::Colour channelColour;

    //-- Selected sound mix (per pattern, per channel) for the strip dropdown.
    //   Empty = none selected. mixModified = parameters edited since selecting it.
    //   mixHash is the editor's "clean" baseline (not persisted; recomputed on load).
    juce::String mixName;
    bool         mixModified = false;
    juce::int64  mixHash     = 0;

    //-- Mix
    float  volume  = 1.0f;   // 0..1.25 (100% default, can boost to 125%)
    float  pan     = 0.0f;
    bool   mute    = false;
    bool   solo    = false;
    bool   phaseInvert = false;

    //-- Sound
    DrumSoundGenerator::Type soundType = DrumSoundGenerator::Type::Kick808;
    bool usingUserSample = false;
    juce::File userSampleFile;
    // Sample playback region + speed (varispeed). Region 0..1 of the sample.
    bool  useRegion   = false;
    float sampleStart = 0.0f;
    float sampleEnd   = 1.0f;
    float playSpeed   = 1.0f;   // 1.0 = 100%; >1 faster/higher, <1 slower/lower
    bool  sampleReverse = false; // play the selected part (or whole sample) backwards
    int   sliceCount  = 1;      // sample slicing: 1 = whole; N = chop the region into N equal slices,
    int   sliceCounter = 0;     // and each consecutive hit plays the next slice (transient runtime state)
    int   chokeGroup = 0;       // 0 = none; channels sharing a group cut each other's tails (e.g. hi-hats)
    float stretchAmt = 1.0f;    // time-stretch: output duration = original x this, pitch unchanged (needs SoundTouch)

    // Fill min/max peaks for a cached waveform display of ONE slot (message thread, lock-free try).
    void getWaveformPeaks(int slot, int numBuckets, std::vector<float>& mins, std::vector<float>& maxs);
    int  getSampleNumFrames(int slot = 0) const
         { return slotSample[juce::jlimit(0, NUM_SLOTS - 1, slot)].buf.getNumSamples(); }
    double getSampleRateHz()  const { return sr; }
    // The loaded file's own sample rate (frames are stored at this rate, not the
    // playback rate), so the real length in seconds = frames / sampleFileRate.
    double getSampleFileRate() const { return sampleFileRate > 0.0 ? sampleFileRate : sr; }

    float sampleCrush = 0.0f;         // 0 = clean .. 1 = heavy bit-crush (lo-fi grit) on the Sample
    //-- Sample source pitch (+/- semitones, applied via resampling) + its own
    //   pitch envelope. These now affect ONLY the Sample source (each source has
    //   its own pitch controls).
    float pitch = 0.0f;
    float pitchEnvAmt  = 0.0f;   // semitones added at note start (-/+)
    float pitchEnvTime = 0.05f;  // seconds to settle to the set pitch
    float pitchOffset  = 0.0f;   // 0..1: delays where the pitch envelope begins

    //-- "Sounds": up to four sources (Sample / Noise / Oscillator / FM) blended
    //   by a 2D pad, then fed as one signal into the global env/EQ/filter/drive.
    enum Source   { SrcSample = 0, SrcNoise, SrcOsc, SrcFM, SrcPhys, NUM_SOURCES };
    // SrcSynth is a SLOT-ONLY unified engine (osc + fold + FM + noise + resonator in
    // one voice). It is NOT one of the NUM_SOURCES legacy sources, so it never enters
    // srcOn[]/buildSlotsFromLegacy(); slots author it directly. Id sits just past the
    // legacy sources so the renderInto/UI switches can add a single extra case.
    static constexpr int SrcSynth = NUM_SOURCES;   // = 5
    // SrcWave: a SLOT-ONLY wavetable engine (scan through a bank of single-cycle waveforms). Like
    // SrcSynth it's not a legacy source; slots author it directly. Id sits just past SrcSynth.
    static constexpr int SrcWave  = NUM_SOURCES + 1;   // = 6
    // ---- Wavetable bank (the SrcWave engine's tables). Built once; read-only at audio rate. ----
    static int         wavetableCount();
    static const char* wavetableName(int table);
    static float       wavetableSample(int table, float pos, float phase01);   // pos 0..1 scan, phase 0..1
    // SrcModal: a SLOT-ONLY MODAL synthesis engine (a struck resonant body = a bank of decaying
    // sine "modes"). Excited by an impulse; each Material picks the mode ratios/gains/decays.
    static constexpr int SrcModal = NUM_SOURCES + 2;   // = 7
    static constexpr int MODAL_MODES = 16;             // max resonant modes per voice
    static int         modalMaterialCount();
    static const char* modalMaterialName(int m);
    // Analog+FM oscillator shapes (the single "Wave" fader scans these; ~17, incl. Vowel/Formant/etc.).
    static int         oscShapeCount();
    static const char* oscShapeName(int s);
    static float       oscShapeSample(int shape, float phase01);
    enum OscShape { OscSine = 0, OscTriangle, OscSquare, OscSaw };

    bool  srcOn[NUM_SOURCES]     = { true, true, true, true, false };       // Physical off by default
    float srcWeight[NUM_SOURCES] = { 0.25f, 0.25f, 0.25f, 0.25f, 0.0f };    // even blend (4 on)
    float padX = 0.5f, padY = 0.5f; // blend-pad dot position (UI restore)
    bool  padLayoutB = false;       // blend pad A/B: swaps the two top corners (4-source square)

    // Per-source AHD amplitude envelope (replaces the old single channel envelope).
    // Indexed by Source (Sample, Noise, Osc, FM). Attack rises, Hold sustains the
    // peak, Decay falls back to silence. All times in seconds.
    float srcAtk[NUM_SOURCES]  = { 0.003f, 0.003f, 0.003f, 0.003f, 0.003f };
    float srcHold[NUM_SOURCES] = { 0.0f,   0.0f,   0.0f,   0.0f,   0.0f   };
    float srcDec[NUM_SOURCES]  = { 2.0f,   0.08f,  0.20f,  0.30f,  0.80f  }; // Sample/Noise/Osc/FM/Physical

    // BLEND character: five clean, non-distorting controls that shape how the
    // blended sources behave together. Each 0..1, 0 = off.
    //   bloom  - per hit, the blend starts focused on the loudest source then opens
    //            up to the full mix (punchy attack -> layered body).
    //   drift  - the blend slowly wanders over time so the timbre keeps evolving.
    //   spread - widens the (synth) sources across the stereo field.
    //   punch  - transient shaper on the combined hit (snap / hardness).
    //   glue   - gentle compression that fuses the blend together.
    float bloom  = 0.0f;
    float drift  = 0.0f;
    float spread = 0.0f;
    float punch  = 0.0f;
    float glue   = 0.0f;

    // Oscillator source
    int   layerOscShape     = OscSine;
    float layerSineFreq     = 60.0f;  // Hz
    float layerSinePEnvAmt  = 0.0f;   // semitones at note start
    float layerSinePEnvTime = 0.04f;  // seconds to settle
    float layerSinePOffset  = 0.0f;   // 0..1: delays where the pitch envelope begins
    // Unison: stack several detuned copies of the oscillator -> fat supersaw / thick bass.
    int   oscUnison = 1;              // 1..7 voices (1 = off)
    float oscDetune = 0.0f;           // 0..1 -> outermost voice is +/-100 cents from the original pitch
    // Sustain: the level the decay settles to (a "floor") instead of fading to silence.
    // A short fixed release then prevents a click at the voice end (no Release knob -
    // drum hits are one-shots with no note-off). 0 = plain AHD. Osc/FM/Physical each
    // have their own; Sample/Noise stay plain AHD.
    float oscSustain = 0.0f;
    float oscVibrato = 0.0f;          // 0..1: pitch vibrato depth (~5.5 Hz) - good for whistles/leads
    // Noise source (coloured then bandpassed; center + width(Q) shape it)
    int   noiseType        = 0;       // 0 white, 1 pink, 2 brown, 3 grey, 4 purple
    float layerNoiseCenter = 3000.0f; // Hz (bandpass center)
    float layerNoiseWidth  = 0.0f;    // 0 = raw (no band-pass) .. 1 = narrow/tuned (Q)
    float noiseSustain     = 0.0f;    // 0..1: decay floor for the Noise source
    // FM source (2-operator FM — carrier modulated by a ratio-tuned modulator)
    float fmPitch  = 0.0f;            // semitones: carrier pitch
    float fmSpread = 0.0f;            // modulator ratio / inharmonicity (0 harmonic .. 1 clangy)
    float fmDepth  = 0.4f;            // modulation depth (index): brightness/timbre
    float fmPitchEnvAmt  = 0.0f;      // semitones at note start (pitch envelope)
    float fmPitchEnvTime = 0.05f;     // seconds to settle
    float fmPitchOffset  = 0.0f;      // 0..1: delays where the pitch envelope begins
    float fmFeedback     = 0.0f;      // 0..1: operator self-feedback (sine -> saw/noise/grit)
    float fmSub          = 0.0f;      // 0..1: sub-octave sine mixed under the FM (body/weight)
    float fmSustain      = 0.0f;      // 0..1: decay floor for the FM source
    // Physical source - Karplus-Strong plucked string / mallet / tuned percussion.
    // A noise burst is recirculated through a tuned delay; a loop low-pass sets the
    // tone and an all-pass adds inharmonic "metal". Ring length follows the AHD Decay.
    float physFreq     = 110.0f;      // Hz (fundamental = delay length)
    float physTone     = 0.5f;        // 0 dark/muted .. 1 bright
    float physMaterial = 0.0f;        // MODEL index 0..5: Nylon/Steel/Wood/Glass/Metal/Skin
    float physPosition = 0.0f;        // 0..1: strike/pluck position (combs the excitation -> hollow)
    float physSustain  = 0.0f;        // 0..1: decay floor for the Physical source
    float physVibrato  = 0.0f;        // 0..1: pitch vibrato depth (~5.5 Hz) on the string
    float physPitchEnvAmt  = 0.0f;    // semitones at note start
    float physPitchEnvTime = 0.05f;   // seconds to settle
    float physPitchOffset  = 0.0f;    // 0..1: delays where the pitch envelope begins

    //======================================================================
    // SOURCE SLOTS  (the new blend model)
    // Up to NUM_SLOTS independent sources. Each slot picks ANY engine (and the
    // same engine may appear in more than one slot, e.g. two detuned Analogs).
    // The legacy per-engine fields above are the simple "authoring" format -
    // factory sounds + saved files still set those, and buildSlotsFromLegacy()
    // converts them into slots[] so nothing downstream needs rewriting.
    //   slot.engine: -1 = none, else a Source value (SrcSample..SrcPhys).
    //======================================================================
    static constexpr int NUM_SLOTS = 2;

    //-- EQ band model (used by BOTH the channel EQ and the per-slot EQ). HP + 3 bells + LP.
    static constexpr int NUM_EQ_BANDS = 5;
    enum { EQ_HP = 0, EQ_B1 = 1, EQ_B2 = 2, EQ_B3 = 3, EQ_LP = 4 };
    struct EqBand { bool on = false; float freq = 1000.0f; float gainDb = 0.0f; float q = 1.0f; };
    static EqBand defaultEqBand(int b) {
        static const EqBand d[NUM_EQ_BANDS] = {
            { false,    30.0f, 0.0f, 0.707f },   // HP
            { false,   180.0f, 0.0f, 1.0f   },   // bell 1 (low)
            { false,  1000.0f, 0.0f, 1.0f   },   // bell 2 (mid)
            { false,  5000.0f, 0.0f, 1.0f   },   // bell 3 (high)
            { false, 16000.0f, 0.0f, 0.707f },   // LP
        };
        return d[juce::jlimit(0, NUM_EQ_BANDS - 1, b)];
    }

    struct Slot
    {
        int   engine  = -1;            // -1 none, else SrcSample/SrcNoise/SrcOsc/SrcFM/SrcPhys
        float weight  = 0.0f;          // blend weight (from the pad)
        // amp envelope (every engine has its own). release = final fade from the
        // sustain floor to 0 (was a fixed constant; now per-slot & user-controlled).
        float atk = 0.003f, hold = 0.0f, dec = 0.2f, sustain = 0.0f, release = 0.06f, vibrato = 0.0f;
        // -- Analog (SrcOsc) -- (also the FM carrier waveform). oscShape = Wave A (start),
        //    oscShapeB = Wave B (end); when they differ the carrier morphs A->B over the
        //    note. Equal (default) = the old static single waveform.
        int   oscShape = OscSine, oscShapeB = OscSine;   // oscShape = the single "Wave" selector (0..16). oscShapeB
                                                         // kept only for legacy/factory From/To morph (now retired).
        float oscWarp = 0.0f;            // wave WARP = one-way wavefold (0 = off/clean, 1 = max fold)
        float oscFreq = 60.0f, oscPEnvAmt = 0.0f, oscPEnvTime = 0.04f, oscPOffset = 0.0f;
        int   oscUnison = 1; float oscDetune = 0.0f; bool oscUniCenter = false;   // dry/centre voice alongside detuned copies
        int   oscDetuneMode = 0;         // detune direction: 0 = symmetric (both ways), 1 = up only (sharp), 2 = down only (flat)
        // -- Wavetable (SrcWave) -- which table + scan position (0..1); pitch reuses oscFreq.
        int   waveTable = 0; float wavePos = 0.0f;
        // -- Modal (SrcModal) -- struck resonant body. Base pitch reuses oscFreq.
        int   modalMaterial = 0;        // which resonant body (mode ratios/gains/decays)
        float modalDecay = 0.5f;        // overall ring length (0..1 -> ~0.05..4s)
        float modalTone  = 0.5f;        // brightness: high-mode level + how fast highs decay
        float modalStruct = 0.5f;       // inharmonicity: stretch/compress the mode ratios (0.5 = native)
        float modalHit = 0.0f;          // strike position: combs which modes get excited (0 = full/no comb)
        float modalDamp = 0.0f;         // extra damping: shortens the ring, highs more (0 = none)
        // -- Noise --
        int   noiseType = 0; float noiseCenter = 3000.0f, noiseWidth = 0.0f;
        float noiseRes = 0.0f, noiseDrive = 0.0f, noiseCrackle = 0.0f;   // resonance (filter Q), saturation, granular dust
        // -- FM (merged into the Oscillator engine; Depth 0 = pure analog, so it defaults OFF) --
        float fmPitch = 0.0f, fmSpread = 0.0f, fmDepth = 0.0f, fmPEnvAmt = 0.0f, fmPEnvTime = 0.05f, fmPOffset = 0.0f, fmFeedback = 0.0f, fmSub = 0.0f;
        // -- Physical --
        float physFreq = 110.0f, physTone = 0.5f, physMaterial = 0.0f, physPosition = 0.0f, physPEnvAmt = 0.0f, physPEnvTime = 0.05f, physPOffset = 0.0f;
        float physStiff = 0.0f;          // Stiffness/inharmonicity: extra dispersion allpass (0 = pure string -> bar/bell)
        int   physExcite = 0;            // excitation: 0 = Pluck (noise burst), 1 = Strike (impulse), 2 = Mallet (soft)
        // -- Sample -- (the buffer lives per-slot in slotSample[]; these are this slot's playback params)
        float smpSpeed = 1.0f, smpCrush = 0.0f, smpPitch = 0.0f, smpPEnvAmt = 0.0f, smpPEnvTime = 0.04f, smpPOffset = 0.0f;
        bool  smpReverse = false, smpUseRegion = false;
        float smpStart = 0.0f, smpEnd = 1.0f;   // legacy single trim region (0..1) - migrated to region 0
        // Multi-region trim: up to 4 hand-drawn regions (0..1); each hit round-robins through them. 0 = none drawn
        // (play the whole sample, or the legacy smpStart/smpEnd region). Cleared when Trim is turned off.
        static constexpr int MAXREG = 4;
        int   smpRegN = 0;
        float smpRegLo[MAXREG] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float smpRegHi[MAXREG] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float smpStretch = 1.0f;                // time-stretch (needs SoundTouch); rebuilds slotSample.buf
        int   smpSlices = 1;                    // 1 = whole; N = chop region into N slices, advance per hit
        float smpGain = 1.0f;                   // sample output boost (samples are quieter than the synth engines)
        // -- Per-slot FX (per sound). Drive = an insert on this slot's signal; Reverb/Delay = this slot's
        //    SEND amount into the shared reverb/delay engines (character set in the FX box). --
        int   fxDriveType = 0;                  // DrumChannel::DriveType
        float fxDrive = 0.0f, fxReverbSend = 0.0f, fxDelaySend = 0.0f;
        // -- Synth (SrcSynth) only -- the unified engine reuses the osc/FM/noise/phys
        // fields above and adds these section controls:
        //   oscFold  = wavefold amount (metallic / FM-ish harmonics from one knob)
        //   oscLevel = oscillator+FM section level   noiseLevel = noise section level
        //   resonAmt = 0 -> resonator OFF; >0 -> Karplus-Strong resonator decay/amount
        float oscFold = 0.0f, oscLevel = 1.0f, noiseLevel = 0.0f, resonAmt = 0.0f;
        // -- Oscillator resonator (the Analog/FM engine's tucked-away PHYSICAL section).
        //    resonAmt doubles as the SrcOsc gate (0 = off -> pure analog/FM, identical to before;
        //    >0 = a Karplus-Strong string is added, tuned to oscFreq, voiced by physMaterial/physTone).
        //    resonDrive = how hard the oscillator(+FM) output EXCITES the string: 0 = only the
        //    trigger noise-burst plucks it (== pure Physical); up = osc/FM-driven string hybrids.
        float resonDrive = 0.0f;
        // -- 4-point PITCH ENVELOPE (per slot, applies on TOP of any legacy per-engine
        //    pitch env). X = time as a fraction (0..1) of the sound's length; Y = pitch in
        //    semitones. The line is ANCHORED at 0 on both edges: (0,0) -> 4 dots -> (1,0),
        //    so the pitch starts and ends at normal. All pitches 0 = no effect (default),
        //    so existing sounds are untouched.
        static constexpr int NPE = 4;
        float pEnvP[NPE] = { 0.0f, 0.0f, 0.0f, 0.0f };       // pitch (semitones) at each dot
        float pEnvT[NPE] = { 0.2f, 0.4f, 0.6f, 0.8f };       // time fraction (0..1) of each dot
        bool  pEnvOn() const { for (int i = 0; i < NPE; ++i) if (pEnvP[i] != 0.0f) return true; return false; }
        // === PER-SLOT EQ (begin) - remove this line + its DSP/persist blocks to revert to
        //     channel-only EQ. Applied to THIS slot's signal before the slots are mixed. ===
        EqBand eqBand[NUM_EQ_BANDS] {
            defaultEqBand(0), defaultEqBand(1), defaultEqBand(2), defaultEqBand(3), defaultEqBand(4) };
        bool  eqOn() const { for (auto& b : eqBand) if (b.on) return true; return false; }
        // === PER-SLOT EQ (end) ===
    };
    Slot slots[NUM_SLOTS];

    // PER-SLOT sample storage: each Sample slot has its OWN buffer + region + speed + reverse, so three
    // slots can hold three different samples (each with its own waveform/trim/reverse). Public so the
    // editor can read a slot's loaded file/name (like it reads slots[] directly).
    struct SlotSample
    {
        juce::AudioBuffer<float> buf;        // the buffer actually played (= original, or time-stretched)
        juce::AudioBuffer<float> original;   // the loaded source (kept so Stretch can be re-derived)
        juce::File file;                     // source path (for persistence + display)
        bool usingUser = false;              // a user file is loaded in this slot
        int  sliceCounter = 0;               // round-robin-of-regions runtime state
        float curRegLo = 0.0f, curRegHi = 1.0f;  // region (0..1) the current voice is playing (set at trigger)
    };
    SlotSample slotSample[NUM_SLOTS];

    // Evaluate the 4-point pitch envelope at time-fraction f (0..1): linear through the
    // anchored points (0,0), (t[i],p[i])..., (1,0). Returns semitones. Static so the
    // editor draws the identical curve.
    static inline float pitchEnv4(float f, const float* p, const float* t) noexcept
    {
        float xs[Slot::NPE + 2], ys[Slot::NPE + 2];
        xs[0] = 0.0f; ys[0] = 0.0f;
        float prev = 0.0f;
        for (int i = 0; i < Slot::NPE; ++i) { prev = juce::jlimit(prev, 1.0f, t[i]); xs[i + 1] = prev; ys[i + 1] = p[i]; }
        xs[Slot::NPE + 1] = 1.0f; ys[Slot::NPE + 1] = 0.0f;
        for (int i = 1; i < Slot::NPE + 2; ++i)
            if (f <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * ((f - xs[i - 1]) / juce::jmax(1.0e-5f, xs[i] - xs[i - 1]));
        return 0.0f;
    }

    // UI (saved per channel): which slot the envelope editor currently edits (1/2/3 = slot
    // 1/2/3). Each slot keeps its own envelope - pick a slot, shape it, pick the next.
    int envEditMode = 1;

    // Rebuild slots[] from the legacy per-engine fields (srcOn/srcWeight + the
    // engine params). Call after any factory/preset/old-file load.
    void buildSlotsFromLegacy();

    // Save/restore the runtime slots[] as "Slot" child trees (so duplicate engines
    // survive save/load + undo). readSlots returns true if any Slot child existed;
    // when false the caller should fall back to buildSlotsFromLegacy().
    void writeSlots(juce::ValueTree& parent) const;
    bool readSlots(const juce::ValueTree& parent);
    bool restoredSlots = false;   // transient: set by readSlots, consumed after load

    //-- Drawable channel EQ (the "ALL" / final EQ on the blended sound): HP + 3 bells + LP.
    //   Edited on the FrequencyDisplay. (Per-slot EQ lives in Slot::eqBand - see PER-SLOT EQ.)
    EqBand eqBand[NUM_EQ_BANDS] {
        defaultEqBand(0), defaultEqBand(1), defaultEqBand(2), defaultEqBand(3), defaultEqBand(4) };

    //-- Filter: only Off + Formant remain (LP/HP now live in the EQ). Enum values kept so
    //   old saved projects still parse; LowPass..Notch are no longer offered or processed.
    enum FilterType { FilterOff = 0, LowPass, HighPass, BandPass, Notch, Formant };
    int   filterType   = FilterOff;
    float filterCutoff = 1000.0f;  // Formant: vowel position
    float filterReso   = 0.707f;   // Formant: sharpness
    float filterEnvAmt = 0.0f;     // Formant: envelope sweep

    //-- Drive / distortion (sits after the filter, before the EQ)
    enum DriveType { DriveOff = 0, SoftClip, HardClip, Tube, Foldback, Fuzz, Bitcrush };
    int   driveType   = DriveOff;
    float driveAmount = 0.0f;      // 0..1

    //-- Effects sends (0..1)
    float reverbSend = 0.0f;
    float delaySend  = 0.0f;

    //-- Multi-output routing: 0 = Main (gets pattern FX + master), 1..NUM_AUX_OUTS = a
    //   discrete stereo aux out (dry, for separate processing on its own DAW track).
    int   outputBus = 0;

    //-- Routing mode: when midiOut is true the channel makes NO internal sound; instead it
    //   emits MIDI notes (its midiNote, transposed by step pitch, velocity from the step,
    //   ratcheted by Roll, faded by Roll decay) so it can sequence another plugin. Mutually
    //   exclusive with internal sound (outputBus is then irrelevant).
    bool  midiOut = false;
    int   midiOutChannel = 1;   // MIDI channel (1-16) the midiOut notes are sent on (channel-wide)

    //-- Polyphony: when true, a new trigger does not cut the previous sound
    //   (voices overlap and ring out); when false the channel is monophonic.
    bool allowOverlap = false;

    //-- Engine internal oversampling factor (set by the processor). Samples read in file-frames
    //   per output sample, so at OS rate their head must advance 1/OS as fast (and their length
    //   x OS) to keep the same playback speed. Other engines are freq-based -> unaffected.
    int engineOS = 1;

    //-- Post-fader output peak for the UI level meter (audio thread writes, UI reads). The editor
    //   applies the meter ballistics (smoothing + peak-hold); this is just the raw block peak.
    std::atomic<float> meterPeak { 0.0f };

    //-- Optional spectrum tap (set by processor for the analysed channel only)
    SpectrumTap* analysisTap = nullptr;
    // === PER-SLOT EQ (begin) - which slot the spectrum analyses: -1 = the final mix (All),
    //     0/1/2 = that slot's signal (pre-mix). Set per block by the processor. ===
    int   analysisSlot = -1;
    float analysisBuf[8192] = {};   // audio-thread scratch: the selected slot's per-sample output
    // === PER-SLOT EQ (end) ===

    //==================================================================
    void prepareToPlay(double sampleRate, int maxBlockSize);
    void loadDefaultSound();
    void loadUserSample(int slot, const juce::File& file);   // load a file into one slot's buffer
    void updateStretch(int slot);   // rebuild slotSample[slot].buf from .original at slots[slot].smpStretch
    void trigger(float velocityGain = 1.0f, float pitchSemis = 0.0f, float pan = 0.0f);
    void renderInto(juce::AudioBuffer<float>& dest, int startSample, int numSamples, bool anySolo,
                    juce::AudioBuffer<float>* reverbSendBus = nullptr,
                    juce::AudioBuffer<float>* delaySendBus  = nullptr);
    void updateDSP();

    // Call (from any thread) after changing eq/filter values; the audio thread
    // rebuilds the filter coefficients on its next block.
    void markDspDirty() { dspDirty.store(true); }

    bool isPlaying() const { for (auto& v : voices) if (v.playHead >= 0.0) return true; return false; }

    // Snapshot the elapsed time (seconds) of each currently-playing voice, for the UI's
    // envelope playhead dots. Read on the message thread (slightly stale is fine - it's
    // only a visual; never blocks the audio thread). Returns the number written.
    int activeVoiceTimes(float* outSec, int maxN) const noexcept
    {
        int n = 0;
        for (const auto& v : voices)
            if (v.playHead >= 0.0 && n < maxN)
                outSec[n++] = (float) ((double) v.voiceSamples / (sr > 0.0 ? sr : 44100.0));
        return n;
    }

    // Immediately silence every voice - used by Stop so ringing tails are cut.
    void silenceAllVoices() { for (auto& v : voices) v.playHead = -1.0; for (auto& ss : slotSample) ss.sliceCounter = 0; }
    bool anyVoiceActive() const { for (auto& v : voices) if (v.active()) return true; return false; }

private:
    double sr = 44100.0;
    double sampleFileRate = 0.0;   // the loaded file's own sample rate (0 = none loaded)
    // Per-channel state for the blend-character processors (Drift/Punch/Glue).
    float driftPhase = 0.0f;
    float vibPhase   = 0.0f;   // shared ~5.5 Hz vibrato LFO (Analog + Physical)
    float punchFast = 0.0f, punchSlow = 0.0f;
    float glueEnv = 0.0f;

    double pitchRatio = 1.0;

    // One playing note. The channel holds a small pool so overlapping triggers
    // can ring out together (polyphony); in mono mode only voice 0 is used.
    static constexpr int POLY      = 8;
    static constexpr int UNI_MAX   = 7;      // max unison voices for the oscillator
    static constexpr int KS_MAX    = 4096;   // Karplus-Strong delay buffer (min ~11 Hz @ 44.1k)
    // Per-slot synthesis state: each of the 3 slots runs its own engine, so it
    // needs its own oscillator phases / noise colour state / Karplus-Strong line /
    // sample playhead. (3x the KS buffer is the only notable memory cost; that's
    // why stacking several Physical slots is the heaviest case - see the tooltip.)
    struct SlotVoice
    {
        double   sinePhase = 0.0;
        double   uniPhase[UNI_MAX + 1] = { 0,0,0,0,0,0,0,0 };   // +1 for the optional dry/centre voice
        double   fmCarrier = 0.0, fmMod = 0.0, fmSubPhase = 0.0;
        double   wtPhase = 0.0;           // wavetable (SrcWave) phase, 0..1
        float    modalY1[MODAL_MODES] = {}, modalY2[MODAL_MODES] = {};   // modal resonator-bank state
        // Per-voice modal coefficients (re-pitched at the strike from the per-block bank by this voice's pitch),
        // so Modal follows per-step + channel pitch. (a2 = pole radius is pitch-independent but stored for speed.)
        float    modalA1[MODAL_MODES] = {}, modalA2[MODAL_MODES] = {}, modalGain[MODAL_MODES] = {};
        int      modalNV = 0;             // mode count captured at the strike (this voice's bank size)
        bool     modalInit = false;       // excitation impulse injected on the first sample
        float    fmFbState = 0.0f;
        Biquad   noiseBP;
        float    pinkB[3] = { 0,0,0 };
        float    brownState = 0.0f, prevWhite = 0.0f;
        float    greyZ1 = 0.0f, greyZ2 = 0.0f;   // grey-noise mid-scoop biquad state (inverse equal-loudness)
        uint32_t noiseState = 0x1234567u;
        float    ksBuf[KS_MAX] = { 0 };
        double   ksWrite = 0.0;
        float    ksLp = 0.0f;
        float    ksApSt[6] = { 0,0,0,0,0,0 };   // dispersion allpass state (up to 6 stages for user Stiffness)
        double   smpHead = 0.0;          // this slot's sample playhead
        // === PER-SLOT EQ (begin) - filter state for HP(2)+bells(3)+LP(2); coeffs live in SC ===
        float    eqZ1[7][2] = {}, eqZ2[7][2] = {};
        // === PER-SLOT EQ (end) ===
    };
    struct Voice
    {
        double   playHead = -1.0;   // -1 = idle; >= 0 = alive (per-slot heads do the reading)
        long     voiceSamples = 0;
        bool     sampleEnded = false;
        float    velGain = 1.0f;    // per-step velocity (volume) for this hit
        float    voicePitch = 0.0f; // per-step pitch offset (semitones) for this hit
        float    voicePan = 0.0f;   // per-step stereo pan (-1..+1) for this hit (0 = centre)
        const juce::AudioBuffer<float>* smpBuf = nullptr;  // velocity-layer buffer chosen at trigger
        SlotVoice sv[NUM_SLOTS];
        bool active() const { return playHead >= 0.0; }
    };
    Voice voices[POLY];

    // Decay coefficient 1 -> ~0.05 over 'timeSeconds' (exp). t in samples.
    inline float decayCurve(long t, float timeSeconds) const noexcept
    {
        if (timeSeconds <= 0.0f) return 0.0f;
        return std::exp(-(float)t * 3.0f / (timeSeconds * (float)sr));
    }
    // Pitch envelope shape with an optional start delay ("pitch offset", 0..1 of
    // up to ~0.3 s): the envelope holds at full until the delay, then decays.
    inline float pitchEnvShape(long t, float timeSeconds, float offset01) const noexcept
    {
        const long d = (long)(juce::jlimit(0.0f, 1.0f, offset01) * 0.3f * (float)sr);
        const long tt = (t > d) ? (t - d) : 0;
        return decayCurve(tt, timeSeconds);
    }
    // Per-source AHD amplitude envelope: linear attack, hold at peak, exp decay.
    inline float ahdEnv(long t, float atk, float hold, float dec) const noexcept
    {
        const float a = atk * (float) sr;
        if ((float) t < a) return a > 1.0f ? (float) t / a : 1.0f;   // attack
        const float ht = (float) t - a;
        const float h  = hold * (float) sr;
        if (ht < h) return 1.0f;                                     // hold
        return decayCurve((long) (ht - h), dec);                     // decay
    }
    // Sample index after which an AHD envelope is effectively silent.
    inline long ahdEndSamples(int s) const noexcept
    {
        return (long) ((srcAtk[s] + srcHold[s]) * (float) sr)
             + (long) (3.2f * srcDec[s] * (float) sr) + 8;
    }
    // PURE AHD: attack -> hold -> exp decay to ZERO. Sustain + Release are RETIRED (the amp-env editor is
    // AHD-only), so they are IGNORED here too - the PLAYED envelope always matches the AHD graph, with no
    // hidden sustain plateau. (Was an AHDSR that settled to a sustain floor + released; that floor was
    // invisible in the AHD UI, so sounds like Vox/Whistle played far longer than the graph showed. Factory
    // sounds that relied on it were retuned to a real decay.) sus/rel kept in the signature for call-site
    // compatibility + persistence, but unused. kSusRelease stays as the field default.
    static constexpr float kSusRelease = 0.06f;   // default release (seconds) - persisted, no longer audible
    inline float ahdsEnv(long t, float atk, float hold, float dec, float /*sus*/, float /*rel*/) const noexcept
    {
        const float a = atk * (float) sr;
        if ((float) t < a) return a > 1.0f ? (float) t / a : 1.0f;       // attack
        const float ht = (float) t - a;
        const float h = hold * (float) sr;
        if (ht < h) return 1.0f;                                          // hold
        return decayCurve((long) (ht - h), dec);                          // decay -> 0
    }
    inline float whiteNoise(uint32_t& st) const noexcept
    {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        return (float)((int32_t)st) * (1.0f / 2147483648.0f);
    }

    // Filters: 8 EQ peaking bands + one multimode filter, applied to the mix
    Biquad eqHP[2], eqBell[3], eqLP[2];   // channel EQ (HP & LP are 24 dB/oct = 2 cascaded)
    Biquad filter;         // legacy multimode filter - kept for factory sounds (LP+env etc.); not user-selectable
    Biquad formantBP[3];   // 3 parallel band-passes for the vowel/Formant filter

    void updateFilter(float envModLevel, double cutoffMul = 1.0); // sets the multimode filter coeffs
    float applyDrive(float x) const;      // distortion shaper (channel-level; calls driveSample)
    static float driveSample(float x, int driveType, float driveAmount);   // per-slot drive (any type/amount)

    // Protects 'sample' buffer against swaps (load) while audio thread reads it
    juce::CriticalSection sampleLock;

    // Set when eq/filter values change; audio thread rebuilds coefficients
    std::atomic<bool> dspDirty { true };

    // Temp render buffer
    juce::AudioBuffer<float> renderBuf;
    juce::AudioBuffer<float> fxSendBuf;   // per-slot reverb/delay send sums (ch 0/1 = reverb L/R, 2/3 = delay L/R)

    void applyEQ(juce::AudioBuffer<float>& buf, int numSamples);
    void rebuildSampleWithPitch();
};
