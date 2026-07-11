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
    // Pitch grid range: +-48 st around MIDDLE C (displayed C4, scientific pitch) = C0..C8 - SYMMETRIC
    // (same distance up/down, user rule) and fully containing an 88-key piano (A0..C8). MIDI 60 = pitch 0.
    static constexpr int PITCH_RANGE = 48;
    static const int VALID_STEP_COUNTS[];
    static constexpr int NUM_VALID_STEP_COUNTS = 20;

    DrumChannel() { clearStepData(); }

    // Reset every step + all its per-step values to defaults (THE one place - callers used to inline
    // this loop in ~4 spots and could forget a field when a new per-step value was added).
    void clearStepData() {
        for (int i = 0; i < MAX_STEPS; ++i) {
            steps[i] = false; stepVel[i] = 1.0f; stepPitch[i] = 0.0f; stepRoll[i] = 1;
            stepRollDecay[i] = 0.0f; stepNoteLen[i] = 0.0f; stepPan[i] = 0.0f; stepNudge[i] = 0.0f;
            stepSlide[i] = false; stepMerge[i] = false; stepCondLen[i] = 1; stepCondMask[i] = 0;
        }
    }

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
    static constexpr int8_t PAN_INHERIT = 127;   // per-note pan sentinel: use the whole-channel drawPan
    struct DrawNote { int16_t start = 0, len = 1; int8_t semi = 0; uint8_t vel = 255; uint8_t slot = 0;
                      uint8_t glide = 0;      // glide=1: slide INTO this note from the previous (legato) note's pitch
                      uint8_t oneShot = 0;    // 1 = INSTANT TRIGGER (no gate: pure AHD ring, exactly like a bare step);
                                              // 0 = held-key gate (sustain holds for len, then release). Right-click menu.
                      uint8_t strumUp = 0;     // STRUM direction: 0 = down (normal), 1 = UP (alt. strum: reversed
                                               // string order + lighter accent). Arp records it; note menu edits it.
                      uint8_t strumPct = 255;  // per-note STRUM amount OVERRIDE: 0..100 (%); 255 = follow the
                                               // sound's Strum knob (default). Right-click note menu.
                      int8_t  pan = PAN_INHERIT;   // per-note PAN: -100 (L) .. 0 (true centre) .. +100 (R), or
                                                   // PAN_INHERIT (127) = follow the whole-channel pan (the default).

        // ONE serialization format (was smeared across 4 sites; adding a field used to mean editing
        // all of them by hand). "start:len:semi:vel:slot:glide:oneShot:strumUp:strumPct:pan" - the
        // caller appends the ',' separator. unpack() is old-string tolerant (short files fill defaults).
        juce::String pack() const {
            return juce::String((int) start) + ":" + juce::String((int) len) + ":" + juce::String((int) semi)
                 + ":" + juce::String((int) vel) + ":" + juce::String((int) slot) + ":" + juce::String((int) glide)
                 + ":" + juce::String((int) oneShot) + ":" + juce::String((int) strumUp)
                 + ":" + juce::String((int) strumPct) + ":" + juce::String((int) pan);
        }
        static DrawNote unpack(const juce::StringArray& f) {
            DrawNote n;
            if (f.size() < 3) return n;
            n.start   = (int16_t) juce::jlimit(0, DRAW_RES * 8 - 1, f[0].getIntValue());   // loose (concat takes); addDrawNote tightens
            n.len     = (int16_t) juce::jlimit(1, DRAW_RES * 8, f[1].getIntValue());
            n.semi    = (int8_t)  juce::jlimit(-PITCH_RANGE, PITCH_RANGE, f[2].getIntValue());
            n.vel     = (uint8_t) juce::jlimit(0, 255, f.size() > 3 ? f[3].getIntValue() : 255);
            n.slot    = (uint8_t) juce::jlimit(0, 2, f.size() > 4 ? f[4].getIntValue() : 0);
            n.glide   = (uint8_t) (f.size() > 5 && f[5].getIntValue() ? 1 : 0);
            n.oneShot = (uint8_t) (f.size() > 6 && f[6].getIntValue() ? 1 : 0);
            n.strumUp = (uint8_t) (f.size() > 7 && f[7].getIntValue() ? 1 : 0);
            n.strumPct= (uint8_t) juce::jlimit(0, 255, f.size() > 8 ? f[8].getIntValue() : 255);
            n.pan     = (int8_t)  (f.size() > 9 ? juce::jlimit(-128, 127, f[9].getIntValue()) : PAN_INHERIT);
            return n;
        }
    };
    bool     drawMode = false;
    DrawNote drawNotes[DRAW_MAX_NOTES];
    int      drawNoteCount = 0;
    float    drawVel = 1.0f, drawPan = 0.0f;   // drawVel = the DEFAULT velocity for freshly drawn notes; drawPan whole-channel
    float    drawTuneCents = 0.0f;   // PIANO-ROLL TUNE fader (-50..+50 cents): shifts the WHOLE bar - the roll's
                                     // C4 pin becomes C4 + this, the keys target the tuned pitch on this channel,
                                     // and leaving the roll parks the Base Freq knob at the tuned value.
    void clearDrawNotes() { drawNoteCount = 0; }
    // Append a note struct (bounded; clamps to a single bar's start range). Audio + message thread
    // both use this; count is written LAST so a concurrent reader never sees an uninitialised note.
    int addDrawNote(const DrawNote& src)
    {
        if (drawNoteCount >= DRAW_MAX_NOTES) return -1;
        const int i = drawNoteCount;
        DrawNote n = src;
        n.start = (int16_t) juce::jlimit(0, DRAW_RES - 1, (int) src.start);
        n.len   = (int16_t) juce::jlimit(1, DRAW_RES * 8, (int) src.len);   // len may cross bars (merged groups)
        n.semi  = (int8_t)  juce::jlimit(-PITCH_RANGE, PITCH_RANGE, (int) src.semi);
        n.slot  = (uint8_t) juce::jlimit(0, 2, (int) src.slot);
        drawNotes[i] = n;
        drawNoteCount = i + 1;
        return i;
    }
    // Convenience field-wise append (most call sites). strumPct<0 -> 255 (follow knob); pan defaults to inherit.
    int addDrawNote(int start, int len, int semi, int vel, int slot = 0, int glide = 0, int oneShot = 0,
                    int strumUp = 0, int strumPct = -1, int pan = PAN_INHERIT)
    {
        DrawNote n;
        n.start = (int16_t) start; n.len = (int16_t) len; n.semi = (int8_t) semi;
        n.vel = (uint8_t) juce::jlimit(0, 255, vel); n.slot = (uint8_t) slot;
        n.glide = (uint8_t) (glide ? 1 : 0); n.oneShot = (uint8_t) (oneShot ? 1 : 0);
        n.strumUp = (uint8_t) (strumUp ? 1 : 0);
        n.strumPct = (uint8_t) (strumPct < 0 || strumPct > 100 ? 255 : strumPct);
        n.pan = (int8_t) (pan >= PAN_INHERIT ? PAN_INHERIT : juce::jlimit(-100, 100, pan));
        return addDrawNote(n);
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
    // SrcGrain: a SLOT-ONLY GRANULAR engine (v1.3.9, user order). Grains = short windowed reads
    // of a SOURCE: the slot's SAMPLE when one is loaded, else a pre-rendered WAVETABLE JOURNEY
    // of the slot's wave (64 cycles sweeping the Custom frames A->D - so Position scans the
    // timbre; plain waves render a uniform table where Position matters less, disclosed).
    static constexpr int SrcGrain = NUM_SOURCES + 3;   // = 8
    static constexpr int GRAIN_TBL = 65536, GRAIN_CYC = 1024, GRAINS_MAX = 12;
    static constexpr int MODAL_MODES = 16;             // max resonant modes per voice
    static constexpr int MODAL_NOTES = 3;              // Modal unison/chord: up to this many FULL banks (one per note)
    static int         modalMaterialCount();
    static const char* modalMaterialName(int m);
    static int         modalModeCount(int material);              // modes in a material's table
    static float       modalModeGain(int material, int mode);     // base gain of one mode (0 if out of range)
    // Analog+FM oscillator shapes (the single "Wave" fader scans these; 14, incl. Vowel/Formant/etc.).
    static int         oscShapeCount();
    static const char* oscShapeName(int s);
    static float       oscShapeSample(int shape, float phase01, const float* customTbl = nullptr);
    static constexpr int ADD_HARM   = 32;    // drawable additive harmonics per slot
    static constexpr int ADD_FRAMES = 4;     // WAVETABLE frames per slot (A/B/C/D), position scans them
    static constexpr int ADD_TBL    = 1024;  // baked custom-wave table length (power of two)
    void rebuildAddTables();                 // addH frames -> addTbl (MESSAGE THREAD; audio reads floats only)
    void rebuildGrainTables();   // granular source tables (grainTbl below, past NUM_SLOTS)
    // ---- MOD MATRIX (per slot, 6 routes) ---------------------------------------------------------
    // A real modulation matrix replacing the fixed LFO/env destinations: each route maps a SOURCE
    // onto a TARGET by a bipolar amount. Applied BLOCK-RATE (config bake is per block) - the four
    // audio-rate LFO paths still exist unchanged; the matrix extends REACH, not rate. All amounts 0
    // = bit-identical (the render path is byte-for-byte unchanged when no route is active).
    static constexpr int MOD_ROUTES = 6;
    // SOURCES (order persisted - APPEND-ONLY). Values sampled once per block from the newest voice.
    enum ModSrc { MSOff = 0, MSVel, MSNote, MSAmpEnv, MSLfoFilt, MSLfoPitch, MSLfoVol, MSLfoWave,
                  MSRandom, MSModEnv, MSModLfo, MS_COUNT };
    // TARGETS (order persisted - APPEND-ONLY). 0..MT_GRID_BASE-1 = fixed targets; MT_GRID_BASE+i =
    // the engine's own knob i (0..7) via slotParamsFor - the dropdown shows its live name.
    enum ModTgt { MTOff = 0, MTFilt1Cut, MTFilt1Res, MTFilt2Cut, MTFilt2Res, MTDrive, MTRevSend,
                  MTDelSend, MTChorus, MTTone, MTPunch, MTComp, MTAtk, MTDec, MTSus, MTRel, MTPitch,
                  MTWavePos, MTDetune, MTVibrato, MTWidth, MTDrift, MT_GRID_BASE };
    static constexpr int MOD_TGT_GRID = 8;   // grid knobs MT_GRID_BASE .. MT_GRID_BASE+7
    static constexpr int MT_COUNT = MT_GRID_BASE + MOD_TGT_GRID;
    // (GridKnob + the mod-matrix DSP helpers are declared after the Slot struct, below.)
    // DRIFT visual honesty: the newest voice's REAL rolled detunes (cents) for the editor's unison
    // view - the drawn lines move with what actually played. Returns voice count (0 = none active).
    int  getDriftSnapshot(int slot, float* centsOut, int maxN) const;
    // LEGACY encoding: OscShape (0..3) is ONLY for the per-channel layerOscShape field (old
    // projects persist it raw - never renumber). SLOT wave indices use the v5 "Wave" list below;
    // buildSlotsFromLegacy translates between the two.
    enum OscShape { OscSine = 0, OscTriangle, OscSquare, OscSaw };
    enum Wave { WvSine = 0, WvHump, WvTri, WvSquare, WvSaw, WvPulse,     // analytic v5 slot indices (bank follows at 6)
                WvCustom = 14 };   // ADDITIVE: user-drawn harmonics (addHarm) baked into a per-slot table

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
    // GRANULAR source tables (message thread only, like rebuildAddTables): built only for
    // SrcGrain slots (cleared otherwise); the audio thread reads only when fully sized.
    std::vector<float> grainTbl[NUM_SLOTS];
    static constexpr int UNI_MAX   = 16;     // max unison/chord/scale voices (Osc; KS 6 / Modal 3 caps stay - public: key-highlight uses it)
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
        float uniSpread = 0.0f;   // STEREO WIDTH: unison/chord voices pan across the field (0 = mono = bit-identical)
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
        // GRANULAR (SrcGrain) - all 0..1, mapped in the render: Position (where in the source),
        // Size (15..350 ms log), Density (3..50 grains/s log), Spray (random position spread),
        // Pitch Spray (random per-grain detune up to +-12 st).
        float grainPos = 0.25f, grainSize = 0.35f, grainDens = 0.5f, grainSpray = 0.15f, grainPitch = 0.0f;
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
                                         // (steps, piano roll, keys, slide) - always play the Base
                                         // Freq. Pitch env / vibrato / LFO still apply. Factory
                                         // default: ON for drum-type categories, OFF for melodic.
        // -- Per-slot FX (per sound). Drive = an insert on this slot's signal; Reverb/Delay = this slot's
        //    SEND amount into the shared reverb/delay engines (character set in the FX box). --
        int   fxDriveType = 0;                  // DrumChannel::DriveType
        float fxDrive = 0.0f, fxReverbSend = 0.0f, fxDelaySend = 0.0f;
        // -- 3 one-knob per-slot FX (v1.3.5; all 0 = bypass = bit-identical) --
        float fxTone  = 0.0f;                   // tilt EQ -1 dark .. +1 bright (~800 Hz pivot, +/-6 dB)
        float fxPunch = 0.0f;                   // transient shaper -1 soften .. +1 punch (per hit)
        float fxComp  = 0.0f;                   // one-knob compressor 0..1 (squash + makeup, per slot)
        // -- ADDITIVE WAVETABLE (Wave = "Custom"): FOUR user-DRAWN harmonic frames (A/B/C/D), each
        //    baked to a table; addPos (0..1) scans across them (0 = A, 1 = D, linear crossfade of
        //    the two neighbours). addPh = each harmonic's phase (radians) - set by the freehand WAVE
        //    drawing (a drawn shape needs phases to reconstruct); bar drawing leaves phases alone.
        //    PER-SEGMENT GLIDE (user spec): addSeg[k] = the travel time of leg k (0 = A>B,
        //    1 = B>C, 2 = C>D) in seconds; 0 = HOLD = the note STOPS at that leg's left frame.
        //    addSeg[0] > 0 = glide is ON (overrides addPos while the note runs); the old
        //    "morph A -> B then stay" = {time, 0, 0}. The WAVE LFO (dest 3) wobbles the position. --
        float addH [ADD_FRAMES][ADD_HARM] = { { 1.0f }, { 1.0f }, { 1.0f }, { 1.0f } }; // each frame h1=1 = sine
        float addPh[ADD_FRAMES][ADD_HARM] = {};
        float addSeg[ADD_FRAMES - 1] = {};   // per-leg glide seconds (0 = hold); [0] == 0 = glide off
        bool  addLoop = false;               // LOOP the glide: travel out then back (ping-pong), forever
        float addPos = 0.0f;                 // static wavetable position 0..1 (used when glide is off)
        // -- Per-slot LFOs ("wobble"): THREE independent sines, one per destination, each with its
        //    own rate + amount, all RESTARTING on every hit (locked to the groove, no tempo-sync UI
        //    needed). amt 0 = that LFO off (all-default = bit-identical). Dest index: 0 = the slot
        //    FILTER's cutoff (+/-3 oct; needs the slot filter ON), 1 = pitch (+/-1 octave),
        //    2 = volume (tremolo). Any mix can run at once. Edited on the LFO visual (FX box). --
        float lfoRate[4] = { 4.0f, 4.0f, 4.0f, 4.0f };   // dest 3 = WAVE (wavetable position scan)
        float lfoAmt[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
        // LFO SHAPE per dest (0 Sine / 1 Triangle / 2 Saw down / 3 Square / 4 Random steps /
        // 5 Saw up / 6 Random glide / 7 CUSTOM = the drawn curve below) and FREE-RUN per dest
        // (false = RETRIG: the wave restarts at every note = the old behaviour; true = FREE:
        // the LFO runs continuously ANCHORED TO THE TIMELINE - same bar position = same phase
        // on every playback pass; notes join it mid-flight).
        int  lfoShape[4] = { 0, 0, 0, 0 };
        bool lfoFree[4]  = { false, false, false, false };
        // LEGATO retrig (per dest, only meaningful when lfoFree is off): a note that starts
        // while ANOTHER voice of this channel still sounds INHERITS its LFO phase instead of
        // restarting - MiniFreak's "Legato KBD" (user order). Detached notes restart as usual.
        bool lfoLegato[4] = { false, false, false, false };
        // LFO SHAPER (shape 7): one drawn cycle per dest, -1..1, drawn directly ON the LFO wave
        // view. The curve IS what plays (all-zero = no modulation). Persisted "lfCv0..3".
        static constexpr int LFO_CURVE_N = 64;
        float lfoCurve[4][LFO_CURVE_N] = {};
        // TEMPO SYNC per LFO (arp-style TWO faders): lfoSync = the BASE cycles-per-bar (0 = OFF/free Hz,
        // -1 = LOCK TO GRID), lfoSyncRate = a RATE-multiplier index into DrumChannel::arpRateMul (x0.25..x3,
        // default 4 = x1). Effective cycles/bar = base x rateMul, then Hz from the host bar length.
        float lfoSync[4]     = { 0.0f, 0.0f, 0.0f, 0.0f };
        int   lfoSyncRate[4] = { 4, 4, 4, 4 };   // index into arpRateMul (4 = x1) - DORMANT, never applied
        // -- legacy unified-engine (SrcSynth, retired) section extras, still read by nothing but
        //    kept as dormant fields so old-project persistence/migration stays graceful:
        //   oscFold = wavefold amount   oscLevel = osc section level   noiseLevel = noise section level
        float oscFold = 0.0f, oscLevel = 1.0f, noiseLevel = 0.0f;
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
        int   filterType   = FilterOff;   // Off / LowPass / HighPass / BandPass / Notch (Formant = channel-only)
        float filterCutoff = 1000.0f, filterReso = 0.707f, filterEnvAmt = 0.0f;
        float filterKeyTrack = 0.0f;      // 0..1: how much the cutoff FOLLOWS the note pitch (keyboard tracking)
        // FILTER 2: a SECOND independent resonant filter, applied in SERIES after filter 1 (e.g. HP + LP
        // = a band you shape). Same controls; Off by default. Persisted flT2/flC2/flR2/flE2/flK2.
        // DRIFT ("alive"): 0 = today's perfectly repeating notes (bit-identical). Above 0 every note
        // rolls fresh randomness: unison start phases scatter (kills the fixed comb = wide blur),
        // each voice gets a tiny +-cents offset, a slow pitch wander breathes, and the level varies
        // a hair. TRUE random (user choice): passes differ microscopically, like any analog synth.
        float drift = 0.0f;
        // FILTER DRIVE: soft tanh saturation INSIDE both SVFs' state loop (resonance compresses and
        // sings instead of ringing louder). 0 = bypass = bit-identical clean filter. One flavour.
        float filterDrive = 0.0f;
        int   filterType2   = FilterOff;
        float filterCutoff2 = 2500.0f, filterReso2 = 0.707f, filterEnvAmt2 = 0.0f;
        float filterKeyTrack2 = 0.0f;
        // === PER-SLOT FILTER (end) ===
        // === PER-SLOT CHORUS (insert, after the filter/EQ) - lush multi-voice stereo widener.
        //     ONE macro control (user): mix only. Rate/depth are EFFECT CONSTANTS in the DSP (like
        //     the reverb's diffusion); the retired chRt/chDp file keys are ignored on load. ===
        float chorusMix = 0.0f;           // 0 = OFF (dry, bit-identical) .. 1 = full wet
        // === PER-SLOT CHORUS (end) ===
        // === MOD MATRIX (begin) - 6 routes + two matrix-created sources (Mod Env / Mod LFO).
        //     amt 0 (default) on every route = bit-identical (the render path is unchanged). ===
        struct ModRoute { int8_t src = 0; int8_t tgt = 0; float amt = 0.0f; };   // amt bipolar -1..1
        ModRoute mod[MOD_ROUTES];
        float modEnvA = 0.005f, modEnvD = 0.30f;   // Mod Env: attack / decay seconds (per-note AD, stateless)
        float modLfoRate = 1.0f;                    // Mod LFO: free-run rate (Hz 0.05..20, timeline-anchored)
        int   modLfoShape = 0;                      // Mod LFO shape (lfoShapeVal 0..6; no Custom)
        bool  modActive() const                     // any live route? (gates the whole matrix - zero cost when off)
        { for (auto& r : mod) if (r.src != MSOff && r.tgt != MTOff && std::abs(r.amt) > 1.0e-4f) return true; return false; }
        // === MOD MATRIX (end) ===
    };
    Slot slots[NUM_SLOTS];
    // MOD MATRIX (DSP helpers - Slot is now defined). GRID-KNOB mirror: the numeric engine knob at UI
    // index `idx` in slotParamsFor(engine) as a pointer-to-Slot-member + range. field == nullptr = no
    // knob / a stepped choice (skipped; the UI dropdown disables those). MUST mirror slotParamsFor's
    // ordering (the mirror rule, like uiLfoShapeVal <-> lfoShapeVal): UI reads NAMES there, DSP applies here.
    struct GridKnob { float Slot::* field = nullptr; float mn = 0.0f, mx = 1.0f; };
    static GridKnob modGridKnob(int engine, int idx);
    // Sample the 6 mod sources for slot s (block-rate, newest voice + the slot's LFOs + the free clock)
    // into out[MS_COUNT]; then apply the routes onto a scratch Slot before the config bake.
    void computeModSources(int s, const Slot& sl, float* out) const;
    void applyModMatrix(Slot& tmp, const float* srcVals) const;
    // Effective BASE frequency for a pitched slot. In PIANO ROLL every pitched engine plays a
    // C4-ABSOLUTE base (+ the Tune fader), independent of the Freq knob - so the roll is knob-free
    // (the knob is never forced/faded/parked; it stays the STEP-mode base). Slot 2 keeps its
    // Slot-2 transpose. Step mode = the slot's own Freq. Bit-identical to the retired "pin the knob
    // to C4" behaviour, just without mutating the visible control.
    double slotBaseHz(int s, const Slot& sl) const {
        const double own = (sl.engine == SrcPhys) ? (double) sl.physFreq : (double) sl.oscFreq;
        if (! drawMode) return own;
        const double c4 = 261.6255653 * std::pow(2.0, (double) juce::jlimit(-50.0f, 50.0f, drawTuneCents) / 1200.0);
        return (s == 1 && keysSlot2Down != 0) ? c4 * std::pow(2.0, -(double) keysSlot2Down / 12.0) : c4;
    }
    float slotFiltEnv[NUM_SLOTS] = {}; // runtime: per-slot amp-env level from the PREVIOUS block, feeds the per-slot filter's env-follow sweep
    float chDrvLp[2] = {}, chDrvDcX[2] = {}, chDrvDcY[2] = {};   // channel drive post-smoothing + Fuzz DC blocker (legacy multi-slot drive stage)
    // PER-SLOT CHORUS runtime: a stereo delay line + 3 LFO phases per slot (lazy-sized in renderInto);
    // the insert runs on the slot's summed output AFTER the voice loop, so it never touches the other slot.
    std::vector<float> chorusDL[NUM_SLOTS], chorusDR[NUM_SLOTS];
    int    chorusW[NUM_SLOTS]  = {};
    double chorusPh[NUM_SLOTS] = {};
    float  addTbl[NUM_SLOTS][ADD_FRAMES][ADD_TBL] = {};  // baked wavetable frames per slot - see rebuildAddTables()
    juce::Random driftRng { 0x9e3779b9 };    // DRIFT dice (audio thread only)
    double lfoBarPos  = -1.0;   // set by the Sequencer per block while PLAYING: bars into the playing
                                // unit (group bar index + fraction); -1 = not playing (free clock)
    double lfoFreeSec = 0.0;    // free-run LFO clock while NOT playing (accumulated seconds)
    float  compEnv[NUM_SLOTS] = {};           // one-knob compressor envelope per slot
    float  lfoBarSeconds = 2.0f;   // seconds per bar (set by the Sequencer each block) for tempo-synced per-slot LFOs
    int    lfoGridDiv    = 16;      // piano-roll Grid 1/N (set by the Sequencer) - for LFO/arp "Lock to grid" in draw mode
    // Legacy-authoring bridge: factory sounds built via buildSlotsFromLegacy can't set slot fields
    // directly (applyPreset re-runs the build and would wipe them) - this channel-level flag is
    // copied onto the built FM slot instead, so FM sounds keep env-follow inside presets too.
    bool legacyFmEnvFollow = false;
    // UI: the newest active voice's LFO phase for this slot + destination (radians; < 0 = no voice).
    double getLfoPhase(int slot, int dest) const
    {
        const Voice* nv = nullptr;
        for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;
        return nv != nullptr ? nv->sv[juce::jlimit(0, NUM_SLOTS - 1, slot)].lfoPhase[juce::jlimit(0, 3, dest)] : -1.0;
    }
    // UI: the newest active voice's LIVE wavetable position for this slot (0..1 across A..D =
    // handle + glide + WAVE LFO, straight from the render; -1 = nothing playing / not Custom).
    float getWtPos(int slot) const
    {
        const Voice* nv = nullptr;
        for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;
        return nv != nullptr ? nv->sv[juce::jlimit(0, NUM_SLOTS - 1, slot)].wtPosCur : -1.0f;
    }
    // UI: the newest voice's LIVE grain read positions (0..1 across the source) - the preview's
    // dots are real grains, never an animation. Returns the count (0 = nothing sounding).
    int getGrainSnapshot(int s, float* out, int maxN) const
    {
        const Voice* nv = nullptr;
        for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;
        if (nv == nullptr) return 0;
        s = juce::jlimit(0, NUM_SLOTS - 1, s);
        const auto& sv = nv->sv[s];
        double lo = 0.0, span = (double) GRAIN_TBL;
        if (slotSample[s].buf.getNumSamples() > 64)
        {
            const int n2 = slotSample[s].buf.getNumSamples();
            const int l2 = juce::jlimit(0, n2 - 2, (int) (slots[s].smpStart * (float) n2));
            const int h2 = juce::jlimit(l2 + 2, n2, (int) (slots[s].smpEnd * (float) n2));
            lo = (double) l2; span = juce::jmax(1.0, (double) (h2 - l2));
        }
        int n = 0;
        for (const auto& gr : sv.grains)
            if (gr.age < gr.len && n < maxN)
                out[n++] = (float) juce::jlimit(0.0, 1.0, (gr.pos - lo) / span);
        return n;
    }
    // The newest voice's S&H cycle counter (seeded per note) - the editor draws the REAL rolled
    // Random pattern with it, so the picture changes per note exactly like the sound does.
    uint32_t getLfoCycle(int slot, int dest) const
    {
        const Voice* nv = nullptr;
        for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;
        return nv != nullptr ? nv->sv[juce::jlimit(0, NUM_SLOTS - 1, slot)].lfoCyc[juce::jlimit(0, 3, dest)] : 0;
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
    enum DriveType { DriveOff = 0, SoftClip, HardClip, Tube, Foldback, Fuzz, Bitcrush,
                     DriveAmpRetired, DriveBassAmp };   // BASS AMP = the split rig (clean lows +
                     // driven mids/highs). Slot 7 = the retired Guitar/Lead amps (user killed
                     // both; a stray saved 7 plays as Tube). Values are persisted - never renumber.
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
    //   keysGlide = MONO LEGATO portamento: when > 0 and you press a new key while still HOLDING the
    //     previous one, the new note SLIDES from the old pitch to the new over keysGlide*0.4 s. Live keys
    //     only (mono); 0 = off = instant (bit-identical). Poly never glides. Per pattern/channel, keys only.
    float keysGlide   = 0.0f;   // 0..1  (glide time = keysGlide * 400 ms)
    //   MERGE & SPLIT (channel pairing, CHANNEL-WIDE like routing): two ADJACENT channels merge so the
    //   keyboard splits BETWEEN them - keys at/above middle C play the pair's FIRST (lower-numbered)
    //   channel, below it the SECOND. Each half maps onto a 4-octave WINDOW (starts stored on the
    //   FIRST channel; octave-snapped 12..60). Merged channels are PIANO-ROLL only; each records into
    //   its own roll (looper-style while recording, like merged pattern groups). -1 = not merged.
    int   mergeWith   = -1;
    int   keysSplitW1 = 60;     // FIRST channel / RIGHT half window start (C4 = identity)
    int   keysSplitW2 = 12;     // SECOND channel / LEFT half window start (C0 = identity)
    //   ARP = a keyboard RIFF generator: hold ONE key -> it plays the root (the pressed key) then a
    //     programmed list of semitone OFFSETS, ONE PER STEP of this channel's grid (rate-scaled), looping
    //     while held. Each row can be a REST (ARP_REST = empty). The clock is COMPUTED from bpm/time-sig/
    //     numSteps, so it runs whether the transport plays or not; the phase starts at the keypress. Mono;
    //     each note flows through keyDown so chord/scale voicing + glide apply per arp note.
    static constexpr int  ARP_ROWS = 12;      // programmable rows = notes 2..13 (note 1 = the pressed key/root)
    static constexpr int  ARP_REST = -128;    // a row set to this = a rest (nothing plays that step)
    bool   arpOn = false;
    int8_t arpOffset[ARP_ROWS] = { 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // default: root + octave, then
                                   // 0 st rows (user: extending Last note must SOUND, not add rests)
    int    arpLen  = 2;         // pattern length INCLUDING the root (1..1+ARP_ROWS); default root + row 1
    //   arpSync = the arp's BASE grid (the "Notes/bar" fader): how many notes fill one bar at Rate x1.
    //   Its OWN value (not the channel's numSteps) so it stays meaningful in Piano Roll mode. The odd
    //   entries (7/9/11/13) give polyrhythmic grids; 12/16/24 are reached via Rate (e.g. 8 x 2 = 16).
    //   arpRate multiplies it: {1/3, 1/2, 1, 1.5, 2, 3}. Default 8 x 2 = classic 16ths.
    int    arpSync = 8;   // -1 = LOCK TO GRID (Notes/bar follows the channel's grid: piano-roll Grid 1/N, else step count)
    int    arpRate = 4;                       // index into the multiplier table; 4 = x1 (default)
    //   arpAlign = while the TRANSPORT plays, phase-lock the arp's steps to the bar grid (press mid-cell
    //   and the next notes land ON the groove); stopped = free-run from the keypress. Default ON.
    //   arpHold  = LATCH: releasing the key keeps the arp looping; press the same key again (or turn
    //   Hold/Arp off) to stop; a different key re-roots it live.
    //   arpGate  = note length as a fraction of one arp step (0.1..1): 1 = ring until the next note
    //   (the old behaviour, bit-identical); lower = staccato (key-up after that fraction, authored release).
    bool   arpAlign = true;
    bool   arpAltStrum = false;   // alternate strum DIRECTION per arp note (up, down, up... like real strumming)
    bool   strumFlip = false;     // runtime: next trigger strums HIGH->LOW (a downstroke); the arp toggles it
    float  strumOverride = -1.0f; // runtime: next trigger's strum amount (0..1); -1 = the Strum knob. Set by
                                  // piano-roll playback from the note's strumPct; consumed like strumFlip.
    bool   arpHold  = false;
    float  arpGate  = 1.0f;
    static constexpr int ARP_SYNCS[6] = { 7, 8, 9, 10, 11, 13 };   // the fader's detents
    static int arpSnapSync(int v)             // snap to the nearest fader detent (tie -> lower)
    {
        int best = 8, bd = 1 << 30;
        for (int c : ARP_SYNCS) { const int d = std::abs(c - v); if (d < bd) { bd = d; best = c; } }
        return best;
    }
    static constexpr int ARP_RATES = 11;      // decimal rates (drag-fader): x0.25 .. x3
    static double arpRateMul(int idx)         // the Rate fader's multiplier
    { static const double t[ARP_RATES] = { 0.25, 1.0/3.0, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0 };
      return t[juce::jlimit(0, ARP_RATES - 1, idx)]; }
    // The MIDI note for arp step `step` (0 = root, 1.. = the offset rows), looping within `len`.
    // Returns -1 for a REST row. Shared by the engine (processor) and the offline test.
    static int arpNoteAt(const int8_t* offsets, int len, int root, int step)
    {
        const int L = juce::jmax(1, len);
        step = ((step % L) + L) % L;
        if (step == 0) return juce::jlimit(0, 127, root);
        const int off = offsets[juce::jlimit(0, ARP_ROWS - 1, step - 1)];
        return off == ARP_REST ? -1 : juce::jlimit(0, 127, root + off);
    }
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
    TunerTap*    tunerTap    = nullptr;   // continuous ring for the REAL tuner (set with analysisTap)
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
                 bool keyGate = false, bool knobBase = false);   // knobBase: TEST on a roll channel
                 // plays each slot at its own Base Freq knob (the roll config is C4-absolute)
    // KEYS (on-screen keyboard / MIDI in): starts one voice playing the pressed MIDI note on every
    // ELIGIBLE slot (each slot re-tuned from its own base Freq). slot2Down = extra transpose
    // (semitones, +down/-up) applied to slot 2 only. poly=false (MONO, default) fades whatever is
    // ringing first - a new key CUTS the old (classic lead feel, bit-identical to the old keyboard);
    // poly=true stacks held notes like a piano (each voice tagged with its keyNote).
    // keyUp(note) releases ONLY that note's voices into the slot release; keyUp() releases all.
    int  keyDown(int midiNote, float velocity, int slot2Down, bool poly = false, int slotMask = 0);
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
    float vibPhase   = 0.0f;   // shared ~5.5 Hz vibrato LFO (Analog + Physical)

    // One playing note. The channel holds a small pool so overlapping triggers
    // can ring out together (polyphony); in mono mode only voice 0 is used.
    static constexpr int KS_MAX    = 4096;   // Karplus-Strong delay buffer (min ~11 Hz @ 44.1k)
    static constexpr int KS_UNI    = 6;      // Physical unison/chord: up to SIX real strings per voice (guitar voicings)
    // Per-slot synthesis state: each of the 2 slots runs its own engine, so it
    // needs its own oscillator phases / noise colour state / Karplus-Strong line /
    // sample playhead. The KS line (16 KB) is HEAP-allocated lazily by ensureKsBuffers()
    // only when a KS engine (Physical / legacy Synth) is actually assigned - every
    // pattern x channel x voice carrying it inline cost ~130 MB of always-on RAM.
    struct SlotVoice
    {
        double   sinePhase = 0.0;
        double   uniPhase[UNI_MAX + 1] = {};   // +1 for the optional dry/centre voice
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
        float    drvLp[2] = {}, drvDcX[2] = {}, drvDcY[2] = {};   // drive post-smoothing (~8 kHz, harsh types) + Fuzz DC blocker
        float    ampPre[2] = {}, ampLp1[2] = {}, ampLp2[2] = {};  // BASS AMP: low split + 2-pole cabinet state
        float    toneZ[2] = {};                       // per-slot TONE tilt (1-pole split state)
        float    pFast[2] = {}, pSlow[2] = {};        // per-slot PUNCH transient followers (fast/slow)
        double   filtIc1[2][2] = {}, filtIc2[2][2] = {};   // TPT/ZDF SVF integrators [filter 0/1][stereo side]
        double   filtGm[2]  = { -1.0, -1.0 };              // per-sample smoothed cutoff coeff per filter (-1 = snap)
        double   filtGkt[2] = { -1.0, -1.0 };              // per-voice KEYTRACK target per filter (tan coeff; -1 = off)
        // === PER-SLOT FILTER (end) ===
        // Per-step LENGTH: effective decay (seconds) replacing this slot's dec so the note's fall
        // FILLS the note length (attack/hold untouched). 0 = no gate = the authored decay. FROZEN
        // at trigger - a 303 tie (slideTo) extends the voice's life but never reshapes the decay,
        // so tied chains keep falling naturally (re-gating the env mid-decay would step/pop).
        float    gateDec = 0.0f;
        double   lfoPhase[4] = {}; // per-slot LFO phases (radians), one per dest (3 = WAVE)
        uint32_t lfoCyc[4]   = {}; // completed cycles (Random/S&H holds one value per cycle)
        // GRANULAR per-voice state: a small pool of concurrent grains + the spawn accumulator.
        struct Grain { double pos = 0, inc = 0; int age = 0, len = 0; float amp = 0; };
        Grain    grains[GRAINS_MAX];
        float    grAcc = 0.0f;
        float    wtPosCur = -1.0f; // UI: LIVE wavetable position 0..1 the render last played
                                   // (handle + glide + WAVE LFO combined; -1 = not rendering a
                                   // Custom table). Torn-read tolerant like the other UI reads.
        // DRIFT per-note randomness (all 1/neutral when drift = 0 = bit-identical):
        float    driftMul[UNI_MAX + 1] = {}; // per-unison-voice fixed detune multiplier (rolled per note)
        float    driftFiltMul = 1.0f;        // per-note FILTER cutoff variation (needs a filter on)
        float    driftGain   = 1.0f;         // per-note level breath
        float    driftWobMul = 1.0f;         // slow pitch-wander multiplier (advanced per block)
        double   driftWobPh  = 0.0;
        float    driftWobRate = 0.0f;
        // MOD MATRIX: per-note random source (rolled once at trigger from driftRng = the drift
        // precedent; true random per hit, disclosed). 0.5 when the matrix is off (unused).
        float    modRand = 0.5f;
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
    juce::AudioBuffer<float> chorusInBuf; // per-slot dry sum for the CHORUS insert (ch 2s/2s+1 = slot s L/R); only used when a slot's chorus is on

    void applyEQ(juce::AudioBuffer<float>& buf, int numSamples);
};
