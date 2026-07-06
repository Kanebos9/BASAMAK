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
// LEGACY sound-type metadata. The old built-in synthesized drum bank is GONE
// (its generate()/gen* synthesis was dead code and was removed); only the Type
// enum + names remain because the "sound" property is persisted in old projects
// and the editor still shows the category names.
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

    // Metadata for building the UI
    static const std::vector<SoundInfo>& all();
    static juce::StringArray            categories();                 // unique, alphabetical
    static juce::Array<Type>            variantsIn(const juce::String& category);
    static juce::String                 categoryOf(Type);
    static juce::String                 variantOf(Type);
};

//==============================================================================
class DrumChannel
{
public:
    static constexpr int MAX_STEPS = 64;
    static const int VALID_STEP_COUNTS[];
    static constexpr int NUM_VALID_STEP_COUNTS = 20;

    DrumChannel() { for (int i = 0; i < MAX_STEPS; ++i) { stepVel[i] = 1.0f; stepPitch[i] = 0.0f; stepRoll[i] = 1; stepRollDecay[i] = 0.0f; stepNoteLen[i] = 0.0f; stepPan[i] = 0.0f; stepNudge[i] = 0.0f; stepSlide[i] = false; stepMerge[i] = false; stepCondLen[i] = 1; stepCondMask[i] = 0; } }

    // ===== PIANO ROLL (was "draw" - internal names kept), an alternative to steps for a channel =====
    // A POLYPHONIC NOTE LIST across the whole bar at fine time resolution (v-poly rework, phase 2):
    // each note = start column + length (columns) + semitone (-36..+36 relative to the slot base /
    // Freq) + its own velocity. Overlapping notes = chords (recorded live or drawn in the editor);
    // a melody is simply non-overlapping notes. FIXED-CAPACITY array (never reallocates), so the
    // audio thread can read while the editor edits - same data-race-tolerant style as the step
    // arrays. Playback triggers each note at its start (overlap-aware: chords don't cut each other)
    // and gates it for its length; the engine's unison/detune/chord/scale apply per note.
    static constexpr int  DRAW_RES = 384;      // columns/bar (divisible by every step count -> clean quantise)
    static constexpr int8_t DRAW_GAP = -128;   // legacy "no note" marker (old-project migration only)
    static constexpr int  DRAW_MAX_NOTES = 256;
    // slot = which sound slot(s) play this note: 0 = both, 1 = slot 1 only, 2 = slot 2 only. Lets a
    // channel draw two independent lines (e.g. a bass on slot 1, a lead on slot 2). Colours: both =
    // orange, slot 1 = yellow, slot 2 = pink (matches the keyboard highlight).
    struct DrawNote { int16_t start = 0, len = 1; int8_t semi = 0; uint8_t vel = 255; uint8_t slot = 0; };
    bool     drawMode = false;
    DrawNote drawNotes[DRAW_MAX_NOTES];
    int      drawNoteCount = 0;
    float    drawVel = 1.0f, drawPan = 0.0f;   // drawVel = the DEFAULT velocity for freshly drawn notes; drawPan whole-channel
    void clearDrawNotes() { drawNoteCount = 0; }
    // Append (bounded); returns the index or -1 when full. Audio + message thread both use this;
    // count is written LAST so a concurrent reader never sees an uninitialised note.
    int addDrawNote(int start, int len, int semi, int vel, int slot = 0)
    {
        if (drawNoteCount >= DRAW_MAX_NOTES) return -1;
        const int i = drawNoteCount;
        // len may exceed the bar: a note recorded/drawn across a MERGED-GROUP bar line keeps
        // sustaining into the following bars (its voice lives in the bar it started in).
        drawNotes[i] = { (int16_t) juce::jlimit(0, DRAW_RES - 1, start),
                         (int16_t) juce::jlimit(1, DRAW_RES * 8, len),
                         (int8_t)  juce::jlimit(-36, 36, semi),
                         (uint8_t) juce::jlimit(0, 255, vel),
                         (uint8_t) juce::jlimit(0, 2, slot) };
        drawNoteCount = i + 1;
        return i;
    }
    void removeDrawNote(int idx)
    {
        if (idx < 0 || idx >= drawNoteCount) return;
        for (int i = idx; i < drawNoteCount - 1; ++i) drawNotes[i] = drawNotes[i + 1];
        --drawNoteCount;
    }
    bool drawHasOverlaps() const   // any two notes sharing time = chords (can't quantise to steps)
    {
        for (int a = 0; a < drawNoteCount; ++a)
            for (int b = a + 1; b < drawNoteCount; ++b)
                if (drawNotes[a].start < drawNotes[b].start + drawNotes[b].len
                    && drawNotes[b].start < drawNotes[a].start + drawNotes[a].len) return true;
        return false;
    }

    //-- Sequencer state (per step)
    bool   steps[MAX_STEPS] = {};
    float  stepVel[MAX_STEPS];        // 0..1 velocity (volume) per step (default 1)
    float  stepPitch[MAX_STEPS];      // -24..+24 semitone offset per step (default 0)
    int    stepRoll[MAX_STEPS];       // 1..6 ratchet/roll sub-hits per step (default 1)
    float  stepRollDecay[MAX_STEPS];  // 0..1 roll fade: 0 = all hits equal, 1 = last hit silent
    float  stepNoteLen[MAX_STEPS];    // NOTE LENGTH: 0 = off (natural ring / 1 step for MIDI-out); 0..1 = fraction of ONE step the note lasts - the DECAY is rescaled to fill it (attack keeps its punch; long = slow fall, short = tight gate)
    float  stepPan[MAX_STEPS];
    float  stepNudge[MAX_STEPS];      // micro-timing: -1..+1 shifts the hit EARLY/LATE by up to half a step        // -1..+1 stereo pan per step (default 0 = centre; internal sounds only)
    bool   stepSlide[MAX_STEPS];      // SLIDE: this step's pitch glides across the step to land on the NEXT active step's pitch
    bool   stepMerge[MAX_STEPS];      // MERGE: this step CONTINUES the previous step's note (no retrigger; the head's gate extends through it - piano-roll style long notes; head's values apply)
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
    // SIDECHAIN DUCK (channel-wide, set in the Routing popup): when channel `duckBy` fires a hit,
    // THIS channel's level dips by duckAmt and recovers over ~130 ms (classic kick-ducks-bass pump).
    // Unlike CHOKE this never cuts the sound - it only pushes the volume down and lets it back up.
    int   duckBy  = -1;         // -1 = off; else the TRIGGER channel index (0-based)
    float duckAmt = 0.5f;       // dip depth 0..1 (0.5 = -6 dB-ish at the bottom of the dip)
    float duckEnv = 0.0f, duckGainZ = 1.0f;   // audio-thread envelope state (not persisted)
    void  duckPulse() { duckEnv = 1.0f; }     // called by the sequencer when the trigger channel fires
    float stretchAmt = 1.0f;    // time-stretch: output duration = original x this, pitch unchanged (needs SoundTouch)

    // Fill min/max peaks for a cached waveform display of ONE slot (message thread, lock-free try).
    void getWaveformPeaks(int slot, int numBuckets, std::vector<float>& mins, std::vector<float>& maxs);
    // The newest playing voice's sample playhead for a slot, as a fraction of the buffer (-1 = none).
    // Display-only (message thread; a torn double read is harmless). Mirrors within the region for reverse.
    float getSamplePlayheadFrac(int slot) const;
    int  getSampleNumFrames(int slot = 0) const
         { return slotSample[juce::jlimit(0, NUM_SLOTS - 1, slot)].buf.getNumSamples(); }
    double getSampleRateHz()  const { return sr; }
    // Sample buffers are RESAMPLED to the host base rate at load (loadUserSample), so
    // their frames are always at hostRate = sr / engineOS. Length in seconds = frames / this.
    double getSampleFileRate() const { return engineOS > 0 ? sr / (double) engineOS : sr; }

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
    static constexpr int MODAL_NOTES = 3;              // Modal unison/chord: up to this many FULL banks (one per note)
    static int         modalMaterialCount();
    static const char* modalMaterialName(int m);
    static int         modalModeCount(int material);              // modes in a material's table
    static float       modalModeGain(int material, int mode);     // base gain of one mode (0 if out of range)
    // Analog+FM oscillator shapes (the single "Wave" fader scans these; 14, incl. Vowel/Formant/etc.).
    static int         oscShapeCount();
    static const char* oscShapeName(int s);
    static float       oscShapeSample(int shape, float phase01);
    // LEGACY encoding: OscShape (0..3) is ONLY for the per-channel layerOscShape field (old
    // projects persist it raw - never renumber). SLOT wave indices use the v5 "Wave" list below;
    // buildSlotsFromLegacy translates between the two.
    enum OscShape { OscSine = 0, OscTriangle, OscSquare, OscSaw };
    enum Wave { WvSine = 0, WvHump, WvTri, WvSquare, WvSaw, WvPulse };   // analytic v5 slot indices (bank follows at 6)

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
    static constexpr int UNI_MAX   = 7;      // max unison/chord/scale voices (public: the editor's key-highlight uses it)
    static constexpr int POLY      = 16;     // simultaneous note events (chords live INSIDE a voice); public: the processor's held stack sizes off it

    // SCALE diatonic harmonizer: the semitone offset (from the played note) of chord voice `voiceIdx`
    // in `scaleType`/`key` for a note `playedMidi` - off-scale notes snap to the nearest member. Public
    // so the editor can light up the played keys. (Wraps the DSP's own scaleSemis - single source.)
    static int scaleNoteOffset(int scaleType, int key, int playedMidi, int voiceIdx);
    // CHORD interval (note-independent) for voice k of chord type `chordMode`. Same table the DSP uses.
    static int chordNoteOffset(int chordMode, int k);

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
        int   chordMode = 0;             // 0 = STD (detuned copies); 1-7 = chord types (Oct/5th/Maj/Min/Sus4/Maj7/Min7) - Osc/Modal/Physical
        int   chordUnison = 3;           // unison count used in CHORD mode (SEPARATE from oscUnison so STD + CHORD don't share)
        // -- SCALE mode (a per-slot diatonic HARMONIZER; precedence scaleOn ? SCALE : chordMode>0 ? CHORD : STD).
        //    Each played note is voiced with the diatonic chord for its scale degree in scaleKey/scaleType;
        //    off-scale/between notes SNAP to the nearest scale note at play time. Unlike CHORD the intervals
        //    depend on the note, so they're computed per-note into SlotVoice::uniSemis (not baked in SC). --
        bool  scaleOn = false;
        int   scaleType = 0;             // 0..9: Major, Nat/Har Minor, Dorian, Phrygian, Lydian, Mixolydian, Maj/Min Pentatonic, Blues
        int   scaleUnison = 3;           // voice/chord-size count in SCALE mode (SEPARATE, like chordUnison)
        int   scaleKey = 0;              // 0..11 root pitch class (C = 0)
        // -- Wavetable (SrcWave) -- which table + scan position (0..1); pitch reuses oscFreq.
        int   waveTable = 0; float wavePos = 0.0f;
        // -- Modal (SrcModal) -- struck resonant body. Base pitch reuses oscFreq.
        int   modalMaterial = 0;        // which resonant body (mode ratios/gains/decays)
        float modalDecay = 0.5f;        // overall ring length (0..1 -> ~0.05..4s)
        float modalTone  = 0.5f;        // brightness: high-mode level + how fast highs decay
        float modalStruct = 0.5f;       // inharmonicity: stretch/compress the mode ratios (0.5 = native)
        float modalHit = 0.0f;          // strike position: combs which modes get excited (0 = full/no comb)
        float modalDamp = 0.0f;         // extra damping: shortens the ring, highs more (0 = none)
        float modalMorph = 0.0f;        // crossfade this Material toward the NEXT one in the list (0 = pure)
        // -- Noise --
        int   noiseType = 0; float noiseCenter = 3000.0f, noiseWidth = 0.0f;
        float noiseRes = 0.0f, noiseDrive = 0.0f, noiseCrackle = 0.0f;   // resonance (filter Q), saturation, granular dust
        // -- FM (merged into the Oscillator engine; Depth 0 = pure analog, so it defaults OFF) --
        float fmPitch = 0.0f, fmSpread = 0.0f, fmDepth = 0.0f, fmPEnvAmt = 0.0f, fmPEnvTime = 0.05f, fmPOffset = 0.0f, fmFeedback = 0.0f, fmSub = 0.0f;
        bool  fmEnvFollow = false;      // FM Amount follows the amp envelope (classic FM drum: bright attack -> mellow decay)
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
        bool  smpEnvOn = false;                 // OPT-IN amp envelope on the sample (off = play full length, legacy-identical)
        bool  smpPreservePitch = true;          // Sample: IGNORE step/draw/key/env pitch (play at the sample's own pitch). Default ON.
        // -- Per-slot FX (per sound). Drive = an insert on this slot's signal; Reverb/Delay = this slot's
        //    SEND amount into the shared reverb/delay engines (character set in the FX box). --
        int   fxDriveType = 0;                  // DrumChannel::DriveType
        float fxDrive = 0.0f, fxReverbSend = 0.0f, fxDelaySend = 0.0f;
        // -- Per-slot LFOs ("wobble"): THREE independent sines, one per destination, each with its
        //    own rate + amount, all RESTARTING on every hit (locked to the groove, no tempo-sync UI
        //    needed). amt 0 = that LFO off (all-default = bit-identical). Dest index: 0 = the slot
        //    FILTER's cutoff (+/-3 oct; needs the slot filter ON), 1 = pitch (+/-1 octave),
        //    2 = volume (tremolo). Any mix can run at once. Edited on the LFO visual (FX box). --
        float lfoRate[3] = { 4.0f, 4.0f, 4.0f };
        float lfoAmt[3]  = { 0.0f, 0.0f, 0.0f };
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
        // === PER-SLOT FILTER (begin) - a resonant LowPass on THIS slot's signal (before its EQ),
        //     so a filtered sound (e.g. Acid Bass) doesn't filter the OTHER slot's engine. Edited on
        //     the slot's EQ display (F diamond). Off by default = identical to before. ===
        int   filterType   = FilterOff;   // Off / LowPass (Formant reserved for the channel filter)
        float filterCutoff = 1000.0f, filterReso = 0.707f, filterEnvAmt = 0.0f;
        // === PER-SLOT FILTER (end) ===
    };
    Slot slots[NUM_SLOTS];
    float slotFiltEnv[NUM_SLOTS] = {}; // runtime: per-slot amp-env level from the PREVIOUS block, feeds the per-slot filter's env-follow sweep
    // Legacy-authoring bridge: factory sounds built via buildSlotsFromLegacy can't set slot fields
    // directly (applyPreset re-runs the build and would wipe them) - this channel-level flag is
    // copied onto the built FM slot instead, so FM sounds keep env-follow inside presets too.
    bool legacyFmEnvFollow = false;
    // UI: the newest active voice's LFO phase for this slot + destination (radians; < 0 = no voice).
    double getLfoPhase(int slot, int dest) const
    {
        const Voice* nv = nullptr;
        for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;
        return nv != nullptr ? nv->sv[juce::jlimit(0, NUM_SLOTS - 1, slot)].lfoPhase[juce::jlimit(0, 2, dest)] : -1.0;
    }

    // PER-SLOT sample storage: each Sample slot has its OWN buffer + region + speed + reverse, so both
    // slots can hold different samples (each with its own waveform/trim/reverse). Public so the
    // editor can read a slot's loaded file/name (like it reads slots[] directly).
    struct SlotSample
    {
        juce::AudioBuffer<float> buf;        // the buffer actually played (= original, or time-stretched)
        juce::AudioBuffer<float> original;   // the loaded source (kept so Stretch can be re-derived)
        juce::File file;                     // source path (for persistence + display)
        bool usingUser = false;              // a user file is loaded in this slot
        double loadedAtRate = 0.0;           // host rate the file was resampled to at load (0 = none);
                                             // prepareToPlay reloads the slot if the host rate changed
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

    // UI (saved per channel): which slot the envelope editor currently edits (1/2 = slot
    // 1/2). Each slot keeps its own envelope - pick a slot, shape it, pick the next.
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
    int   keysSlot2Down = 0;    // KEYS: extra transpose DOWN (0-24 st) applied to slot 2 only, PER pattern/channel

    //-- HUMANIZE (per-channel feel, all patterns). Two independent axes, both bit-identical at 0:
    //   humanizeAmt = per-hit RANDOM timing offset between slot 1 & slot 2 (+ small velocity jitter),
    //     so two layered sounds don't stack machine-perfectly (only meaningful with 2 audible slots).
    //   strumAmt    = deterministic low->high TIME SPREAD across a slot's chord/scale notes (a strum;
    //     only meaningful when that slot is in CHORD or SCALE mode).
    float humanizeAmt = 0.0f;   // 0..1
    float strumAmt    = 0.0f;   // 0..1
    uint32_t humRng   = 0x9e3779b9u;   // per-channel RNG advanced each trigger (per-hit humanize variation)
    //   keysMinVel / keysMaxVel = the FLOOR + CEILING for keyboard velocity: a played key's velocity is
    //     remapped [0..1] -> [keysMinVel..keysMaxVel], so soft playing still sounds and loud is tamed.
    //     Per pattern/channel, keys only. Defaults 0/1 = raw velocity.
    float keysMinVel  = 0.0f;   // 0..1
    float keysMaxVel  = 1.0f;   // 0..1
    //   keysPolyMode = keyboard POLY: held keys stack like a piano (up to POLY notes); off = MONO,
    //     a new key cuts the previous one (classic lead/slide feel). Per pattern/channel, keys only.
    bool  keysPolyMode = true;    // POLY by default (per-sound; saved in mix files)
    //-- TRANSPOSE LOCK (PER-SOUND, rides with Sound Bank mixes like the engines do): when true the
    //   Freq knobs/faders of BOTH slots are UI-disabled, so nothing can sneakily transpose the sound's
    //   pitch reference (keys / steps / MIDI export stay anchored). UI-only lock - the engine's keys
    //   C3 re-base still applies (it IS the consistency mechanism). Factory Keys-bank sounds ship ON.

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
    //     0/1 = that slot's signal (pre-mix). Set per block by the processor. ===
    int   analysisSlot = -1;
    // Audio-thread scratch for the selected slot's per-sample output. Points into ONE
    // processor-owned buffer (only one channel is ever analysed at a time) instead of a
    // 32 KB array per channel x pattern. Set per block by the processor with analysisTap.
    float* analysisBuf   = nullptr;
    int    analysisBufLen = 0;
    // === PER-SLOT EQ (end) ===

    //==================================================================
    void prepareToPlay(double sampleRate, int maxBlockSize);
    void loadDefaultSound();
    void loadUserSample(int slot, const juce::File& file);   // load a file into one slot's buffer
    void updateStretch(int slot);   // rebuild slotSample[slot].buf from .original at slots[slot].smpStretch
    // Allocate the Karplus-Strong delay lines (16 KB per voice-slot) if any slot uses a KS
    // engine (Physical / legacy Synth). MESSAGE THREAD ONLY (it allocates); the audio thread
    // gates every KS read/write on ksReady. Called from prepareToPlay / readSlots /
    // buildSlotsFromLegacy / the editor's engine-change path. Lazily allocating these cut the
    // idle memory of 32 patterns x 16 channels x 8 voices x 2 slots from ~130 MB to ~0.
    void ensureKsBuffers();
    // gateSamples > 0 = cut this hit after that many samples (soft 3 ms fade - the per-step Length).
    // glideSamples > 0 = SLIDE: the pitch starts at pitchSemis and glides to glideToSemis over that time.
    // Returns the voice index it used (keyDown patches key data onto it; other callers ignore it).
    // slotMask = which slots this hit sounds (bit 0 = slot 1, bit 1 = slot 2); 0 or 3 = both (default).
    // Piano-roll notes carry a per-note slot tag so slot 1 + slot 2 can play different lines.
    // keyGate = play the gate like a HELD KEY (natural envelope while the note runs, RELEASE at its
    // end) instead of the per-step Length decay-rescale - piano-roll notes reproduce the performance.
    int  trigger(float velocityGain = 1.0f, float pitchSemis = 0.0f, float pan = 0.0f, long gateSamples = 0,
                 float glideToSemis = 0.0f, long glideSamples = 0, bool forceOverlap = false, int slotMask = 0,
                 bool keyGate = false);
    // KEYS (on-screen keyboard / MIDI in): starts one voice playing the pressed MIDI note on every
    // ELIGIBLE slot (each slot re-tuned from its own base Freq). slot2Down = extra transpose
    // (semitones, +down/-up) applied to slot 2 only. poly=false (MONO, default) fades whatever is
    // ringing first - a new key CUTS the old (classic lead feel, bit-identical to the old keyboard);
    // poly=true stacks held notes like a piano (each voice tagged with its keyNote).
    // keyUp(note) releases ONLY that note's voices into the slot release; keyUp() releases all.
    int  keyDown(int midiNote, float velocity, int slot2Down, bool poly = false);
    void keyUp(int midiNote);
    void keyUp();
    static float physDecayScale(int material);   // material ring-length multiplier (for the UI's tail read-out)
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
    // CHOKE-group cut: fade every ringing voice out over ~3 ms instead of a hard cut
    // (a mid-sample discontinuity clicks when the choking hit is quieter than the tail).
    // Fade every active voice out. Chokes keep the default 3 ms; the KEYS mono handover uses
    // ~15 ms (a loud sustained tone cut in 3 ms reads as a CRACKLE when sliding across keys).
    void fadeOutVoices(float sec = 0.0f)
    { for (auto& v : voices) if (v.active()) { v.killing = true; if (sec > 0.0f) v.killStep = 1.0f / juce::jmax(1.0f, sec * (float) sr); } }
    bool anyVoiceActive() const { for (auto& v : voices) if (v.active()) return true; return false; }

private:
    double sr = 44100.0;
    // Per-channel state for the blend-character processors (Drift/Punch/Glue).
    float driftPhase = 0.0f;
    float vibPhase   = 0.0f;   // shared ~5.5 Hz vibrato LFO (Analog + Physical)
    float punchFast = 0.0f, punchSlow = 0.0f;
    float glueEnv = 0.0f;

    // One playing note. The channel holds a small pool so overlapping triggers
    // can ring out together (polyphony); in mono mode only voice 0 is used.
    static constexpr int KS_MAX    = 4096;   // Karplus-Strong delay buffer (min ~11 Hz @ 44.1k)
    static constexpr int KS_UNI    = 3;      // Physical unison/chord: up to this many real strings per voice
    // Per-slot synthesis state: each of the 2 slots runs its own engine, so it
    // needs its own oscillator phases / noise colour state / Karplus-Strong line /
    // sample playhead. The KS line (16 KB) is HEAP-allocated lazily by ensureKsBuffers()
    // only when a KS engine (Physical / legacy Synth) is actually assigned - every
    // pattern x channel x voice carrying it inline cost ~130 MB of always-on RAM.
    struct SlotVoice
    {
        double   sinePhase = 0.0;
        double   uniPhase[UNI_MAX + 1] = { 0,0,0,0,0,0,0,0 };   // +1 for the optional dry/centre voice
        float    uniSemis[UNI_MAX] = {};   // SCALE: per-note diatonic offset (semitones) for each unison/string/bank voice; only read in SCALE mode
        double   fmCarrier = 0.0, fmMod = 0.0, fmSubPhase = 0.0;
        double   wtPhase = 0.0;           // wavetable (SrcWave) phase, 0..1
        float    modalY1[MODAL_NOTES][MODAL_MODES] = {}, modalY2[MODAL_NOTES][MODAL_MODES] = {};   // resonator state, one FULL bank per chord note
        // Per-voice modal coefficients (re-pitched at the strike from the per-block bank by this voice's pitch),
        // so Modal follows per-step + channel pitch. (a2 = pole radius is pitch-independent but stored for speed.)
        float    modalA1[MODAL_NOTES][MODAL_MODES] = {}, modalA2[MODAL_NOTES][MODAL_MODES] = {}, modalGain[MODAL_NOTES][MODAL_MODES] = {};
        int      modalNV = 0;             // mode count captured at the strike (this voice's bank size)
        bool     modalInit = false;       // excitation impulse injected on the first sample
        bool     modalHold = false;       // KEYS/gate sustain: bank radius clamped ~1 last block (re-bake on change)
        float    fmFbState = 0.0f;
        Biquad   noiseBP;
        float    pinkB[3] = { 0,0,0 };
        float    brownState = 0.0f, prevWhite = 0.0f;
        float    greyZ1 = 0.0f, greyZ2 = 0.0f;   // grey-noise mid-scoop biquad state (inverse equal-loudness)
        uint32_t noiseState = 0x1234567u;
        // KS delay lines: empty until ensureKsBuffers(), then KS_UNI * KS_MAX floats (one region
        // per unison STRING, string k at offset k*KS_MAX). Physical unison/chord plays several
        // real strings; single-voice sounds use string 0 only (bit-identical to the old 1-line KS).
        std::vector<float> ksBuf;
        double   ksWrite[KS_UNI] = {};
        float    ksLp[KS_UNI]    = {};
        float    ksApSt[KS_UNI][12] = {};   // dispersion allpass state per string (up to 12 stages for Stiffness)
        double   smpHead = 0.0;          // this slot's sample playhead
        // === PER-SLOT EQ (begin) - filter state for HP(2)+bells(3)+LP(2); coeffs live in SC ===
        float    eqZ1[7][2] = {}, eqZ2[7][2] = {};
        // === PER-SLOT EQ (end) ===
        // === PER-SLOT FILTER (begin) - resonant LP state (stereo); coeffs live in SC ===
        float    filtZ1[2] = {}, filtZ2[2] = {};
        // === PER-SLOT FILTER (end) ===
        // Per-step LENGTH: effective decay (seconds) replacing this slot's dec so the note's fall
        // FILLS the note length (attack/hold untouched). 0 = no gate = the authored decay. FROZEN
        // at trigger - a 303 tie (slideTo) extends the voice's life but never reshapes the decay,
        // so tied chains keep falling naturally (re-gating the env mid-decay would step/pop).
        float    gateDec = 0.0f;
        double   lfoPhase[3] = {}; // per-slot LFO phases (radians), one per dest, reset at trigger (per-hit restart)
        // KEYS (on-screen keyboard): this slot re-tuned from its own base freq to the pressed
        // note, in semitones (multiplies pe3Mul, so it reaches every pitched engine incl. the
        // Modal strike-tuning). keyMute = the slot's engine can't be played by keys (Sample/
        // Noise/legacy) -> silent for this key voice only.
        float    keySemis = 0.0f;
        bool     keyMute  = false;
        // HUMANIZE / STRUM (all 0 = bit-identical). startDelay = samples this whole slot's onset is
        // pushed back (between-slot humanize); velScale = this slot's per-hit velocity jitter (~1).
        // uniDelay[u] = extra samples chord/scale voice u waits before it sounds (strum, low->high).
        int      startDelay = 0;
        float    velScale   = 1.0f;
        int      uniDelay[UNI_MAX] = {};
    };
    struct Voice
    {
        double   playHead = -1.0;   // -1 = idle; >= 0 = alive (per-slot heads do the reading)
        long     voiceSamples = 0;
        bool     sampleEnded = false;
        float    velGain = 1.0f;    // per-step velocity (volume) for this hit
        float    voicePitch = 0.0f; // per-step pitch offset (semitones) for this hit
        float    voicePan = 0.0f;   // per-step stereo pan (-1..+1) for this hit (0 = centre)
        bool     killing  = false;  // choke/gate: fade this voice out (~3 ms) then stop - no hard-cut click
        float    killGain = 1.0f;   // current fade gain while killing
        float    killStep = 0.0f;   // per-sample fade rate override (0 = the default 3 ms choke)
        long     gateLen  = 0;      // per-step Length: the note's length in samples (0 = off). The audible shaping
                                    // lives in SlotVoice::gateDec (rescaled decay); this also keeps a tied voice alive.
        float    glideStep  = 0.0f; // 303 slide: semitones added to voicePitch per sample while gliding
        long     glideRemain = 0;   // samples of glide left (0 = not gliding)
        bool     isKey  = false;    // KEYS voice: held-note ADSR (sustain/release live) + per-slot keySemis
        long     keyOff = -1;       // voiceSamples when the key was RELEASED (-1 = still held / not a key voice)
        int      keyNote = -1;      // POLY keys: the MIDI note this voice is playing (keyUp(note) releases only its own voices)
        const juce::AudioBuffer<float>* smpBuf = nullptr;  // velocity-layer buffer chosen at trigger
        SlotVoice sv[NUM_SLOTS];
        bool active() const { return playHead >= 0.0; }
    };
    Voice voices[POLY];
    // True once every voice's KS lines are allocated (ensureKsBuffers). The audio thread
    // gates all ksBuf access on this (acquire) so allocation on the message thread is safe.
    std::atomic<bool> ksReady { false };

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
    // KEYS (held-note) envelope: attack/hold identical to AHD, but the decay settles at the
    // SUSTAIN level while the key is held, then falls with RELEASE from wherever it was when
    // the key was let go. ONLY key voices use this - sequencer hits keep the pure AHD above,
    // so factory sounds + the amp-env graph are untouched (sus/rel are live on the keyboard only).
    inline float keyAdsr(long t, long tOff, float atk, float hold, float dec, float sus, float rel) const noexcept
    {
        sus = juce::jlimit(0.0f, 1.0f, sus);
        auto held = [&](long tt) -> float {
            const float a = atk * (float) sr;
            if ((float) tt < a) return a > 1.0f ? (float) tt / a : 1.0f;  // attack
            const float ht = (float) tt - a, h = hold * (float) sr;
            if (ht < h) return 1.0f;                                       // hold
            return sus + (1.0f - sus) * decayCurve((long)(ht - h), dec);   // decay -> sustain floor
        };
        if (tOff < 0 || t < tOff) return held(t);                          // key held
        return held(tOff) * decayCurve(t - tOff, juce::jmax(0.005f, rel)); // released -> release tail
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
};
