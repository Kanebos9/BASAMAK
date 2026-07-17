#include "DrumChannel.h"
#include <vector>

// ================================================================================================
// FILE MAP [added 2026-07-13 22:20 - navigation aid; SEARCH for the quoted strings, line numbers
// drift]. This file is the whole per-channel DSP: sound engines, voices, modulation, persistence.
//   1. "WAVETABLE BANK"            - the oscillator's named single-cycle shape tables
//   2. "MODAL synthesis"           - struck-body material tables (bells/membranes)
//   3. "Sound generation helpers"  - eqProcess biquad, envelopes (ahdsEnv/keyAdsr live in the .h),
//                                    KS stiffness chain, noise, pitch detect
//   4. "DrumChannel implementation" - lifecycle: prepareToPlay, trigger(), keyDown/keyUp (keys +
//                                    MPE expression), chokes, glide
//   5. "SAMPLE FILE CACHE"         - shared decoded-sample store (resampled to host rate)
//   6. "MOD MATRIX (DSP)"          - computeModSources / applyModMatrix / applyHotBlock /
//                                    modGridKnob. HOT targets are per-sample (see 8.)
//   7. #include "Adaa.h" / "SincTable.h" - anti-aliased shapers + sample interpolation (own files)
//   8. "renderInto"                - THE render: per-block bakeSlot config, the voice loop, the
//                                    PER-SAMPLE audio-rate mod eval ("AUDIO-RATE MODULATION"),
//                                    engine switch, filters, FX inserts, CHANNEL FX, sends
//   9. "driveSample"               - the plain (legacy channel-stage) drive shapers
//  10. "writeSlots" / "readSlots" / "writeChannel" / "readChannel" - persistence + migrations
// Dated notes like [2026-07-13 19:57] mark everything added since the audio-rate engine batch.
// ================================================================================================
#if BASAMAK_HAVE_SOUNDTOUCH
 #include <SoundTouch.h>
#endif

const int DrumChannel::VALID_STEP_COUNTS[] =
    { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 21, 24, 32 };   // 48/64 removed (overkill; Draw mode covers fine timing)

//==============================================================================
// WAVETABLE BANK (SrcWave engine). A set of named tables, each = WT_FRAMES single-cycle waveforms
// (WT_LEN samples). The scan Position morphs between frames. Generated ADDITIVELY (sum of sine
// harmonics, capped) so the tables are band-limited. Built once at first use (message thread).
//==============================================================================
namespace {
constexpr int WT_FRAMES = 8;
constexpr int WT_LEN    = 1024;   // power of two

struct WaveBank
{
    std::vector<std::string> names;
    std::vector<float>       data;   // [table][frame][WT_LEN]
    int count = 0;
    const float* frame(int t, int f) const { return data.data() + ((size_t) t * WT_FRAMES + f) * WT_LEN; }

    // amp(h, ff): amplitude of harmonic h (1..maxH) at frame fraction ff (0..1). sign optional.
    void add(const char* nm, int maxH, const std::function<float(int, float)>& amp)
    {
        names.emplace_back(nm);
        const double twoPi = juce::MathConstants<double>::twoPi;
        for (int f = 0; f < WT_FRAMES; ++f)
        {
            const float ff = WT_FRAMES > 1 ? (float) f / (float) (WT_FRAMES - 1) : 0.0f;
            std::vector<float> fr((size_t) WT_LEN, 0.0f);
            for (int h = 1; h <= maxH; ++h)
            {
                const float a = amp(h, ff);
                if (std::abs(a) < 1.0e-5f) continue;
                const double w = twoPi * (double) h / (double) WT_LEN;
                for (int i = 0; i < WT_LEN; ++i) fr[(size_t) i] += a * (float) std::sin(w * i);
            }
            float pk = 1.0e-6f; for (float v : fr) pk = juce::jmax(pk, std::abs(v));
            for (float& v : fr) v /= pk;                       // normalise each frame to +/-1
            data.insert(data.end(), fr.begin(), fr.end());
        }
        ++count;
    }

    WaveBank()
    {
        // sine -> saw (all harmonics ramp in as 1/h)
        add("Saw Sweep", 96, [](int h, float ff){ return h == 1 ? 1.0f : ff / (float) h; });
        // sine -> square (odd harmonics only)
        add("Square Sweep", 96, [](int h, float ff){ return (h % 2 == 1) ? (h == 1 ? 1.0f : ff / (float) h) : 0.0f; });
        // pulse width sweep (PWM): width 0.5 -> 0.05
        add("PWM", 96, [](int h, float ff){ const float w = 0.5f - 0.45f * ff;
                 return (2.0f / (float) (h * juce::MathConstants<float>::pi)) * std::sin((float) h * juce::MathConstants<float>::pi * w); });
        // a single harmonic "formant" peak that climbs the spectrum with the scan
        add("Formant", 110, [](int h, float ff){ const float c = 1.0f + ff * 18.0f, s = 2.5f;
                 return std::exp(-((h - c) * (h - c)) / (2.0f * s * s)); });
        // two vowel-like formant peaks
        add("Vowel", 110, [](int h, float ff){ const float c1 = 2.0f + ff * 4.0f, c2 = 8.0f + ff * 14.0f, s = 1.6f;
                 return std::exp(-((h-c1)*(h-c1))/(2*s*s)) + 0.7f * std::exp(-((h-c2)*(h-c2))/(2*s*s)); });
        // inharmonic-ish bell/metal: a fixed sparse partial set, brightness fades in
        add("Bell", 64, [](int h, float ff){ static const int parts[] = {1,2,3,5,7,11,13,17};
                 for (int p : parts) if (p == h) return (h == 1 ? 1.0f : ff) / std::sqrt((float) h); return 0.0f; });
        // organ: octave stack (powers of two) fading in
        add("Organ", 64, [](int h, float ff){ if ((h & (h - 1)) != 0) return 0.0f;   // h is a power of 2
                 const float oct = std::log2((float) h); return oct == 0 ? 1.0f : ff / (oct + 1.0f); });
        // triangle -> saw morph
        add("Tri-Saw", 96, [](int h, float ff){ const float tri = (h % 2 == 1) ? (1.0f / (float)(h*h)) * ((((h-1)/2) & 1) ? -1.0f : 1.0f) : 0.0f;
                 const float saw = (h == 1) ? 1.0f : 1.0f / (float) h; return tri * (1.0f - ff) + saw * ff; });
        // digital/random: deterministic pseudo-random harmonic amps, count grows with scan
        add("Digital", 96, [](int h, float ff){ unsigned s = (unsigned) h * 2654435761u; float r = ((s >> 9) & 0xffff) / 65535.0f;
                 const float lim = 2.0f + ff * 90.0f; return h <= lim ? (0.2f + 0.8f * r) / std::sqrt((float) h) : 0.0f; });
        // bright buzz: sine that sharpens hard with the scan
        add("Buzz", 110, [](int h, float ff){ return h == 1 ? 1.0f : (ff * ff) / std::sqrt((float) h); });
    }
};

static const WaveBank& waveBank()
{
    static WaveBank b;   // built once, first call (forced on the message thread via prepareToPlay)
    return b;
}
} // namespace

int         DrumChannel::wavetableCount()            { return waveBank().count; }
const char* DrumChannel::wavetableName(int t)        { const auto& b = waveBank();
                                                       return b.names[(size_t) juce::jlimit(0, b.count - 1, t)].c_str(); }
float       DrumChannel::wavetableSample(int table, float pos, float ph01)
{
    const auto& b = waveBank();
    table = juce::jlimit(0, b.count - 1, table);
    const float fp = juce::jlimit(0.0f, 1.0f, pos) * (WT_FRAMES - 1);
    const int   f0 = (int) fp, f1 = juce::jmin(f0 + 1, WT_FRAMES - 1); const float ff = fp - f0;
    ph01 -= std::floor(ph01);
    const float sp = ph01 * WT_LEN; const int i0 = ((int) sp) & (WT_LEN - 1), i1 = (i0 + 1) & (WT_LEN - 1);
    const float si = sp - (float) (int) sp;
    const float* a = b.frame(table, f0); const float* c = b.frame(table, f1);
    const float va = a[i0] + (a[i1] - a[i0]) * si, vc = c[i0] + (c[i1] - c[i0]) * si;
    return va + (vc - va) * ff;
}

//==============================================================================
// MODAL synthesis (SrcModal engine). A struck resonant body = a bank of decaying sine "modes".
// Each Material is a set of modes: a frequency RATIO (vs the base pitch), a GAIN, and a DECAY
// multiplier (higher/inharmonic modes usually die faster). Rendered as a bank of 2-pole
// resonators fed one impulse at the strike (cheap + exact decaying sines). This covers the
// percussion palette KS can't: bars, bells, glass, drumheads, plates, blocks.
//==============================================================================
namespace {
struct ModalMaterial { const char* name; int n; float ratio[16]; float gain[16]; float decayMul[16]; };
// ratios/gains are approximations of real struck-object spectra (bar/bell/membrane/plate modes).
static const ModalMaterial kModalMaterials[] = {
    { "Marimba", 4,
      { 1.0f, 3.93f, 9.61f, 16.8f },
      { 1.0f, 0.45f, 0.22f, 0.10f },
      { 1.0f, 0.55f, 0.30f, 0.18f } },
    { "Tubular Bell", 10,   // extended: 4 more upper partials (continued ~n^2 series, low gain) = shimmer
      { 1.0f, 2.76f, 5.40f, 8.93f, 13.34f, 18.64f, 24.8f, 31.9f, 39.8f, 48.6f },
      { 1.0f, 0.7f, 0.5f, 0.38f, 0.25f, 0.16f, 0.10f, 0.065f, 0.042f, 0.027f },
      { 1.0f, 0.92f, 0.82f, 0.7f, 0.55f, 0.42f, 0.32f, 0.24f, 0.18f, 0.13f } },
    { "Glass", 9,           // extended: 3 more partials continue the authored spacing (quiet sparkle)
      { 1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.5f, 15.95f, 19.75f, 23.9f },
      { 1.0f, 0.6f, 0.42f, 0.3f, 0.2f, 0.13f, 0.085f, 0.055f, 0.035f },
      { 1.0f, 0.7f, 0.5f, 0.36f, 0.26f, 0.18f, 0.13f, 0.09f, 0.06f } },
    { "Membrane", 10,   // circular drumhead (Bessel) modes + 2 more ring modes (subtle skin "air")
      { 1.0f, 1.593f, 2.135f, 2.295f, 2.653f, 2.917f, 3.155f, 3.5f, 3.83f, 4.15f },
      { 1.0f, 0.7f, 0.55f, 0.5f, 0.4f, 0.34f, 0.28f, 0.22f, 0.16f, 0.11f },
      { 1.0f, 0.7f, 0.55f, 0.5f, 0.42f, 0.36f, 0.3f, 0.25f, 0.21f, 0.17f } },
    { "Metal Plate", 14,    // extended: plates get DENSER with frequency - 4 more modes = real clang
      { 1.0f, 1.41f, 1.73f, 2.0f, 2.24f, 2.65f, 3.0f, 3.46f, 4.12f, 4.9f, 5.7f, 6.6f, 7.6f, 8.7f },
      { 1.0f, 0.8f, 0.7f, 0.62f, 0.55f, 0.46f, 0.4f, 0.33f, 0.26f, 0.2f, 0.15f, 0.11f, 0.08f, 0.06f },
      { 1.0f, 0.95f, 0.9f, 0.86f, 0.8f, 0.74f, 0.68f, 0.6f, 0.52f, 0.45f, 0.39f, 0.33f, 0.28f, 0.24f } },
    { "Wood Block", 3,
      { 1.0f, 2.71f, 5.15f },
      { 1.0f, 0.4f, 0.18f },
      { 0.5f, 0.32f, 0.2f } },
    { "Kalimba", 4,
      { 1.0f, 3.0f, 6.2f, 10.5f },
      { 1.0f, 0.35f, 0.16f, 0.08f },
      { 1.0f, 0.6f, 0.4f, 0.28f } },
    { "Cowbell", 5,
      { 1.0f, 1.52f, 2.61f, 3.42f, 4.5f },
      { 1.0f, 0.85f, 0.6f, 0.4f, 0.26f },
      { 1.0f, 0.9f, 0.78f, 0.66f, 0.55f } },
};
static const int kNumModalMaterials = (int) (sizeof(kModalMaterials) / sizeof(kModalMaterials[0]));
} // namespace

int         DrumChannel::modalMaterialCount()      { return kNumModalMaterials; }
const char* DrumChannel::modalMaterialName(int m)  { return kModalMaterials[juce::jlimit(0, kNumModalMaterials - 1, m)].name; }
int         DrumChannel::modalModeCount(int m)     { return kModalMaterials[juce::jlimit(0, kNumModalMaterials - 1, m)].n; }
float       DrumChannel::modalModeGain(int m, int i)
{
    const auto& M = kModalMaterials[juce::jlimit(0, kNumModalMaterials - 1, m)];
    return (i >= 0 && i < M.n) ? M.gain[i] : 0.0f;
}

// USER-STIFFNESS dispersion design (SrcPhys). At the 2x engine rate, POSITIVE-coefficient
// allpasses only bend phase near the (ultrasonic) Nyquist - the audible band sits in their
// flat region, which is why the old Stiffness knob was inaudible no matter the stage count.
// NEGATIVE coefficients near -1 concentrate their group delay at LOW frequencies instead:
// the fundamental is delayed most and upper partials progressively less, so after the loop
// length is COMPENSATED for the fundamental (comp), the upper partials come out SHARP =
// real stiff-string -> bar -> bell inharmonicity (up to ~+3 semitones of partial stretch).
// Shared by the SC setup and trigger so the burst fill + ring agree on the loop length.
static inline void designStiffChain(float st, double baseF, double sr,
                                    float& apC, int& apN, float& comp) noexcept
{
    st   = juce::jlimit(0.0f, 1.0f, st);
    apC  = -(0.80f + 0.15f * st);                        // -0.80 .. -0.95 (dispersion needs |a| near 1)
    apN  = juce::jlimit(1, 6, 1 + juce::roundToInt(st * 5.0f));
    const float tdc = (1.0f - apC) / (1.0f + apC);       // DC group delay per stage (samples)
    const float L0  = (float) (sr / juce::jmax(20.0, baseF));
    comp = (float) apN * tdc;                            // subtracted from the loop so the FUNDAMENTAL stays in tune
    if (comp > 0.45f * L0)                               // short string: only as much dispersion as the period fits
    {
        apN  = juce::jmax(1, (int) std::floor(0.45f * L0 / tdc));
        comp = (float) apN * tdc;
    }
}

#include "SincTable.h"   // [2026-07-13 22:10] polyphase sinc interpolation moved to its own module

//==============================================================================
// Sound generation helpers
//==============================================================================

// === PER-SLOT EQ === process x through a biquad whose COEFFS are in 'c' but whose STATE is
// external (kept per-voice-per-slot in SlotVoice). Lets one set of coeffs serve all voices.
static inline float eqProcess(const Biquad& c, float* z1, float* z2, float x, int ch) noexcept
{
    double y = c.b0 * x + (double) z1[ch];
    z1[ch] = (float)(c.b1 * x - c.a1 * y + (double) z2[ch]);
    z2[ch] = (float)(c.b2 * x - c.a2 * y);
    return (float) y;
}
static constexpr float kPi = juce::MathConstants<float>::pi;

// Oscillator shapes, shared by the Analog and FM carriers - the v5 list: 0 Sine, 1 Hump,
// 2 Tri, 3 Square, 4 Saw, 5 Pulse (25%); 6.. = the additive bank below. v1.1.0 TRIM (user:
// "ramp and saw sound basically the same"): Ramp (reversed saw = identical spectrum), Vowel E
// (between A and O) and Voice (~= Vowel A) were REMOVED; Hump was cut too but the user asked
// for it back "right after sine" (= the v4->v5 renumber). readSlots remaps pre-v4 and v4
// indices so old projects keep their sound. Do not re-add near-duplicate shapes.
// Extra (harmonic-rich) oscillator shapes 6..13 - built once as band-limited single cycles (additive).
namespace {
constexpr int OSCT_EXTRA = 8;
constexpr int OSCT_LEN   = 1024;   // power of two
struct OscShapeBank {
    std::vector<float> d;
    const float* shape(int i) const { return d.data() + (size_t) i * OSCT_LEN; }
    void add(int maxH, const std::function<float(int)>& amp) {
        const double tp = juce::MathConstants<double>::twoPi;
        std::vector<float> f((size_t) OSCT_LEN, 0.0f);
        for (int h = 1; h <= maxH; ++h) { const float a = amp(h); if (std::abs(a) < 1.0e-5f) continue;
            const double w = tp * h / OSCT_LEN;
            for (int i = 0; i < OSCT_LEN; ++i) f[(size_t) i] += a * (float) std::sin(w * i); }
        float pk = 1.0e-6f; for (float v : f) pk = juce::jmax(pk, std::abs(v));
        for (float& v : f) v /= pk;
        d.insert(d.end(), f.begin(), f.end());
    }
    OscShapeBank() {
        auto g = [](int h, float c, float w){ return std::exp(-((h - c) * (h - c)) / (2.0f * w * w)); };
        add(40, [&](int h){ return g(h,3,1.2f) + 0.6f*g(h,7,2.0f); });                         // 6  Vowel A
        add(40, [&](int h){ return g(h,2,1.0f) + 0.6f*g(h,5,1.5f); });                         // 7  Vowel O
        add(40, [&](int h){ return g(h,6,1.4f); });                                            // 8  Formant
        add(40, [&](int h){ if ((h&(h-1))!=0) return 0.0f; float o=std::log2((float)h); return 1.0f/(o+1.0f); }); // 9 Organ
        add(40, [&](int h){ static const int p[]={1,2,3,5,7,11}; for(int q:p) if(q==h) return 1.0f/(float)h; return 0.0f; }); // 10 Bell
        add(40, [&](int h){ return (h%2==1)? 1.0f/std::sqrt((float)h) : 0.0f; });              // 11 Glass (bright odd)
        add(24, [&](int h){ return (h%2==1)? 1.0f/(float)h : 0.0f; });                         // 12 Reed (clarinet)
        add(40, [&](int h){ return (1.0f/(float)h) * (1.0f + 1.5f*g(h,5,2.0f)); });            // 13 Brass (saw+formant)
    }
};
static const OscShapeBank& oscBank() { static OscShapeBank b; return b; }
} // namespace

static constexpr int NUM_OSC_SHAPES = 6 + OSCT_EXTRA;   // 0..5 analytic + 6..13 bank = 14
static inline float waveShape(double phase, int shape) noexcept
{
    const double tp = 2.0 * (double) kPi;
    const float ph = (float)(phase / tp - std::floor(phase / tp));
    if (shape >= 6) {                                    // harmonic-rich bank shape (Vowel/Formant/...)
        const float* w = oscBank().shape(juce::jlimit(0, OSCT_EXTRA - 1, shape - 6));
        const float sp = ph * OSCT_LEN; const int i0 = ((int) sp) & (OSCT_LEN - 1), i1 = (i0 + 1) & (OSCT_LEN - 1);
        return w[i0] + (w[i1] - w[i0]) * (sp - (float) (int) sp);
    }
    switch (shape) {
        case DrumChannel::WvHump:   return 2.0f * std::sin((float) kPi * ph) - 1.0f;  // hump (rectified-sine bump)
        case DrumChannel::WvTri:    return 1.0f - 4.0f * std::abs(ph - 0.5f);   // triangle
        case DrumChannel::WvSquare: return (ph < 0.5f) ? 1.0f : -1.0f;          // square
        case DrumChannel::WvSaw:    return 2.0f * ph - 1.0f;                    // saw (ramp up)
        case DrumChannel::WvPulse:  return (ph < 0.25f) ? 1.0f : -1.0f;         // pulse (25% duty)
        default:                    return (float) std::sin(phase);             // sine
    }
}
// Public accessors (for the UI's Wave fader + visual).
int         DrumChannel::oscShapeCount()          { return NUM_OSC_SHAPES + 1; }   // +1 = Custom (drawn additive)
const char* DrumChannel::oscShapeName(int s) {
    static const char* n[NUM_OSC_SHAPES + 1] = { "Sine","Hump","Tri","Square","Saw","Pulse",
        "Vowel A","Vowel O","Formant","Organ","Bell","Glass","Reed","Brass","Custom" };
    return n[juce::jlimit(0, NUM_OSC_SHAPES, s)];
}
// Pre-v4 slot files used the old 17-shape list (with Ramp/Vowel E/Voice) - map those indices
// onto the closest surviving v5 shape so old projects sound the same.
static inline int remapPreV4Shape(int s)
{
    static const int m[17] = { 0, /*Tri*/2, /*Square*/3, /*Saw*/4, /*Ramp*/4, /*Pulse*/5, /*Hump*/1,
                               /*VowelA*/6, /*VowelE*/6, /*VowelO*/7, /*Formant*/8, /*Organ*/9,
                               /*Bell*/10, /*Glass*/11, /*Reed*/12, /*Brass*/13, /*Voice*/6 };
    return m[juce::jlimit(0, 16, s)];
}
// v4 files (the brief 13-shape list without Hump) -> v5 (Hump re-inserted at index 1).
static inline int remapV4Shape(int s) { return s == 0 ? 0 : juce::jlimit(0, 16, s) + 1; }
float       DrumChannel::oscShapeSample(int shape, float ph01, const float* customTbl) {
    if (shape >= WvCustom && customTbl != nullptr) {              // Custom = the drawn additive table
        ph01 -= std::floor(ph01);
        const float sp = ph01 * (float) ADD_TBL;
        const int i0 = ((int) sp) & (ADD_TBL - 1), i1 = (i0 + 1) & (ADD_TBL - 1);
        return customTbl[i0] + (customTbl[i1] - customTbl[i0]) * (sp - (float) (int) sp);
    }
    return waveShape((double)(ph01 - std::floor(ph01)) * 2.0 * (double) kPi, juce::jmin(shape, NUM_OSC_SHAPES - 1));
}

// DRIFT visual honesty: report the newest playing voice's rolled per-voice detunes in CENTS.
int DrumChannel::getDriftSnapshot(int slot, float* centsOut, int maxN) const
{
    const Voice* nv = nullptr;
    for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;
    if (nv == nullptr) return 0;
    const auto& sv = nv->sv[juce::jlimit(0, NUM_SLOTS - 1, slot)];
    const int n = juce::jmin(maxN, UNI_MAX + 1);
    for (int u = 0; u < n; ++u)
        centsOut[u] = 1200.0f * std::log2(juce::jmax(1.0e-6f, sv.driftMul[u] * sv.driftWobMul));
    return n;
}

// ADDITIVE "Custom" wave: bake each slot's drawn harmonic levels (addHarm) into a wavetable, the
// same construction as the factory OscShapeBank shapes (sum of sines, peak-normalised). MESSAGE
// THREAD only (edits / loads / prepare); the audio thread only reads the float table - a rebuild
// mid-block can tear a few samples (same accepted tolerance as sample loading; never a crash).
void DrumChannel::rebuildAddTables()
{
    auto bake = [](const float* h, const float* ph, float* dst)
    {
        float tmp[ADD_TBL] = {};
        bool any = false;
        const double tp = 2.0 * (double) kPi;
        for (int k = 0; k < ADD_HARM; ++k)
        {
            const float a = h[k];
            if (std::abs(a) < 1.0e-4f) continue;
            any = true;
            const double w = tp * (double)(k + 1) / (double) ADD_TBL;
            for (int i = 0; i < ADD_TBL; ++i) tmp[i] += a * (float) std::sin(w * i + (double) ph[k]);
        }
        if (! any) { const double w = tp / (double) ADD_TBL;      // all-zero drawing = a pure sine (never silence)
                     for (int i = 0; i < ADD_TBL; ++i) tmp[i] = (float) std::sin(w * i); }
        float pk = 1.0e-6f; for (float v : tmp) pk = juce::jmax(pk, std::abs(v));
        for (int i = 0; i < ADD_TBL; ++i) dst[i] = tmp[i] / pk;
    };
    for (int sl = 0; sl < NUM_SLOTS; ++sl)
        for (int f = 0; f < ADD_FRAMES; ++f)
            bake(slots[sl].addH[f], slots[sl].addPh[f], addTbl[sl][f]);
    rebuildGrainTables();   // granular sources ride the same message-thread rebuild sites
}

// GRANULAR source: a 64-cycle "journey" of the slot's wave. Custom waves sweep the 4 drawn
// frames A->D across the table (Position = the timbre scan); plain waves render uniformly
// (Position matters less there - disclosed in the tooltip). MESSAGE THREAD ONLY; audio reads
// only when the vector is fully sized (torn tolerance like the additive tables).
void DrumChannel::rebuildGrainTables()
{
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        auto& tbl = grainTbl[s];
        if (slots[s].engine != SrcGrain) { tbl.clear(); continue; }
        if ((int) tbl.size() != GRAIN_TBL) { tbl.clear(); tbl.resize(GRAIN_TBL, 0.0f); }
        const auto& sl = slots[s];
        const bool custom = sl.oscShape >= WvCustom;
        const int nCyc = GRAIN_TBL / GRAIN_CYC;   // 64
        for (int cyc = 0; cyc < nCyc; ++cyc)
        {
            const float jf = (float) cyc / (float) (nCyc - 1) * (float) (ADD_FRAMES - 1);
            const int   f0 = juce::jmin((int) jf, ADD_FRAMES - 2);
            const float fm = jf - (float) f0;
            for (int i = 0; i < GRAIN_CYC; ++i)
            {
                const float ph = (float) i / (float) GRAIN_CYC;
                float v;
                if (custom)
                {   // sweep the drawn frames across the table = the grain journey
                    const float spx = ph * (float) ADD_TBL;
                    const int j0 = ((int) spx) & (ADD_TBL - 1), j1 = (j0 + 1) & (ADD_TBL - 1);
                    const float fr = spx - (float) (int) spx;
                    const float a0 = addTbl[s][f0][j0] + (addTbl[s][f0][j1] - addTbl[s][f0][j0]) * fr;
                    const float a1 = addTbl[s][f0 + 1][j0] + (addTbl[s][f0 + 1][j1] - addTbl[s][f0 + 1][j0]) * fr;
                    v = a0 + (a1 - a0) * fm;
                }
                else v = oscShapeSample(sl.oscShape, ph, nullptr);
                tbl[(size_t) (cyc * GRAIN_CYC + i)] = v;
            }
        }
    }
}
// LFO wave value for a shape at phase ph (radians). Shape 0 = sine (the old, exact path);
// 1 Triangle / 2 Saw down / 3 Square / 4 Random steps / 5 Saw up / 6 Random glide (interpolated
// steps) / 7 CUSTOM (the drawn curve, linear-interp). Random values are hashed per cycle index.
static inline float lfoRand01(uint32_t cyc) noexcept
{ uint32_t h = cyc * 2654435761u; h ^= h >> 16; h *= 2246822519u; h ^= h >> 13;
  return (float) h / 2147483648.0f - 1.0f; }
static inline float lfoShapeVal(int shape, double ph, uint32_t cyc, const float* curve = nullptr) noexcept
{
    if (shape <= 0) return (float) std::sin(ph);
    const double t = ph / (2.0 * juce::MathConstants<double>::pi);
    const double f = t - std::floor(t);
    switch (shape)
    {
        case 1: return (float) (f < 0.25 ? 4.0 * f : f < 0.75 ? 2.0 - 4.0 * f : 4.0 * f - 4.0); // Triangle
        case 2: return (float) (1.0 - 2.0 * f);                                                  // Saw (falls)
        case 3: return f < 0.5 ? 1.0f : -1.0f;                                                   // Square
        case 4: return lfoRand01(cyc);                                                           // Random steps
        case 5: return (float) (2.0 * f - 1.0);                                                  // Saw up (ramp)
        case 6:                                                                                   // Random glide
        { const float a = lfoRand01(cyc), b = lfoRand01(cyc + 1u);
          const float sm = (float) (f * f * (3.0 - 2.0 * f));                                    // smoothstep
          return a + (b - a) * sm; }
        default:                                                                                  // Custom curve
        { if (curve == nullptr) return 0.0f;
          const double sp = f * (double) DrumChannel::Slot::LFO_CURVE_N;
          const int i0 = juce::jlimit(0, DrumChannel::Slot::LFO_CURVE_N - 1, (int) sp);
          const int i1 = (i0 + 1) % DrumChannel::Slot::LFO_CURVE_N;
          const float fr = (float) (sp - (double) i0);
          return curve[i0] + (curve[i1] - curve[i0]) * fr; }
    }
}

// Continuous morph between the shapes (pos 0..NUM_OSC_SHAPES-1). At an integer pos it is
// identical to waveShape(), so Wave A == Wave B reproduces a plain single shape.
static inline float morphWave(double phase, float pos) noexcept
{
    pos = juce::jlimit(0.0f, (float)(NUM_OSC_SHAPES - 1), pos);
    const int i0 = (int) pos; const int i1 = (i0 < NUM_OSC_SHAPES - 1) ? i0 + 1 : i0;
    const float fr = pos - (float) i0;
    const float a = waveShape(phase, i0), b = waveShape(phase, i1);
    return a + fr * (b - a);
}

// PolyBLEP: band-limited step correction that removes saw/square aliasing at the source
// (the technique virtual-analog synths use). t = phase 0..1, dt = phase increment per sample.
static inline float polyBLEP(float t, float dt) noexcept
{
    if (dt <= 0.0f) return 0.0f;
    if (t < dt)        { t /= dt;            return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
// Band-limited morphWave: same shape crossfade as morphWave(), plus PolyBLEP corrections scaled
// by how much SAW / SQUARE content the morph has. dt = osc freq / sr (cycles per sample).
static inline float morphWaveBL(double phase, float pos, float dt) noexcept
{
    float y = morphWave(phase, pos);
    if (pos > 4.0f) return y;   // Pulse + the additive bank shapes: rely on the 2x oversampling
    pos = juce::jlimit(0.0f, 4.0f, pos);   // v5 indices: ..2 Tri, 3 Square, 4 Saw (Sine/Hump need no BLEP)
    const float wSaw = (pos > 3.0f) ? (pos - 3.0f) : 0.0f;
    const float wSq  = (pos <= 3.0f) ? juce::jmax(0.0f, pos - 2.0f) : (4.0f - pos);
    if (wSaw <= 0.0f && wSq <= 0.0f) return y;   // sine/triangle: no discontinuity to correct
    const double tp = 2.0 * (double) kPi;
    const float ph = (float)(phase / tp - std::floor(phase / tp));
    if (wSaw > 0.0f) y -= wSaw * polyBLEP(ph, dt);
    if (wSq  > 0.0f) { float ph2 = ph + 0.5f; if (ph2 >= 1.0f) ph2 -= 1.0f;
                       y += wSq * (polyBLEP(ph, dt) - polyBLEP(ph2, dt)); }
    return y;
}

// Physical "Material" models. Each reconfigures the Karplus-Strong loop so the
// source sounds like a different struck/plucked object. apC/apStages = dispersion
// (inharmonicity -> metal/bell), briScale = loop brightness, decScale = ring
// length multiplier, exciteBright = how bright the initial pluck noise burst is.
struct PhysModel { float apC; int apStages; float briScale; float decScale; float exciteBright; float pitchDrop; };
// v1.3.9 (user: "steel/metal/glass sound the same, wood/nylon too"): Glass + Metal used
// POSITIVE allpass coefficients, whose dispersion sits ABOVE the audible band at the 2x engine
// rate (the same lesson as the stiffness redesign) - they were literally Steel with extra CPU.
// Now NEGATIVE coefficients = real audible partial stretch (bell/clang), and the material chain
// gets the SAME loop-length compensation as the stiffness chain (or they'd play flat).
static const PhysModel kPhysModels[] = {
    //   apC   stg  bri   dec   excite drop(semis)
    {  0.00f, 0, 0.40f, 1.10f, 0.10f,  0.0f }, // 0 Nylon - round + DARK, soft finger pluck
    {  0.00f, 0, 1.00f, 1.90f, 0.95f,  0.0f }, // 1 Steel - bright harmonic, long sustain, sharp pluck
    {  0.00f, 0, 0.65f, 0.10f, 0.90f,  0.0f }, // 2 Wood  - clicky DRY knock, VERY short (dies instantly)
    { -0.88f, 6, 0.95f, 3.20f, 0.50f,  0.0f }, // 3 Bell (was Glass) - SOFT ping attack + very long ring
                                               //   + stretched shimmer up high. Dispersion alone is
                                               //   pitch-dependent (inaudible on bass strings - physics),
                                               //   so Bell differs by ATTACK + LENGTH at every pitch.
    { -0.92f, 6, 0.80f, 0.90f, 0.85f,  0.0f }, // 4 Metal - heavy stretch, darker, clanky, shorter
    { -0.40f, 2, 0.55f, 0.50f, 0.40f,  9.0f }, // 5 Skin  - boomy tuned drumhead with a tom pitch drop
};
static constexpr int kNumPhysModels = (int) (sizeof(kPhysModels) / sizeof(kPhysModels[0]));

// Vowel formant frequencies (F1,F2,F3 in Hz) for the Formant filter, in order
// A E I O U. The filter's Cutoff sweeps between these (vowel morph), Reso = sharpness.
static const float kVowels[5][3] = {
    { 700.0f, 1220.0f, 2600.0f },  // A (ah)
    { 530.0f, 1840.0f, 2480.0f },  // E (eh)
    { 270.0f, 2290.0f, 3010.0f },  // I (ee)
    { 570.0f,  840.0f, 2410.0f },  // O (oh)
    { 300.0f,  870.0f, 2240.0f },  // U (oo)
};

//-- Metadata table (order defines display order within each category) --------
const std::vector<DrumSoundGenerator::SoundInfo>& DrumSoundGenerator::all()
{
    static const std::vector<SoundInfo> table = {
        { Type::Kick808,      "Kick",       "808" },
        { Type::Kick909,      "Kick",       "909" },
        { Type::KickAcoustic, "Kick",       "Acoustic" },
        { Type::KickSub,      "Kick",       "Sub" },
        { Type::Snare808,     "Snare",      "808" },
        { Type::Snare909,     "Snare",      "909" },
        { Type::SnareAcoustic,"Snare",      "Acoustic" },
        { Type::HatClosed808, "Hi-Hat",     "Closed 808" },
        { Type::HatClosed909, "Hi-Hat",     "Closed 909" },
        { Type::HatOpen808,   "Hi-Hat",     "Open 808" },
        { Type::HatOpen909,   "Hi-Hat",     "Open 909" },
        { Type::ClapClassic,  "Clap",       "Classic" },
        { Type::Clap909,      "Clap",       "909" },
        { Type::TomLow,       "Tom",        "Low" },
        { Type::TomMid,       "Tom",        "Mid" },
        { Type::TomHigh,      "Tom",        "High" },
        { Type::Crash,        "Cymbal",     "Crash" },
        { Type::Ride,         "Cymbal",     "Ride" },
        { Type::Cowbell,      "Percussion", "Cowbell" },
        { Type::Clave,        "Percussion", "Clave" },
        { Type::Rim,          "Percussion", "Rim" }
    };
    return table;
}

juce::StringArray DrumSoundGenerator::categories()
{
    juce::StringArray cats;
    for (auto& s : all())
        cats.addIfNotAlreadyThere(s.category);
    cats.sort(true); // alphabetical
    return cats;
}

juce::Array<DrumSoundGenerator::Type> DrumSoundGenerator::variantsIn(const juce::String& category)
{
    juce::Array<Type> out;
    for (auto& s : all())
        if (s.category == category) out.add(s.type);
    return out;
}

juce::String DrumSoundGenerator::categoryOf(Type t)
{
    for (auto& s : all()) if (s.type == t) return s.category;
    return {};
}

juce::String DrumSoundGenerator::variantOf(Type t)
{
    for (auto& s : all()) if (s.type == t) return s.variant;
    return {};
}

//==============================================================================
// DrumChannel implementation
//==============================================================================

// Build the new slot list from the legacy per-engine fields. Each enabled source
// (srcOn[]) drops into the next free slot with its weight, AHD envelope and that
// engine's parameters. Factory sounds / saved files set the legacy fields, so this
// is the single bridge into the slot model. (No-duplicate by construction here;
// the UI is what creates duplicates later, writing slots[] directly.)
void DrumChannel::buildSlotsFromLegacy()
{
    for (auto& s : slots) { s.engine = -1; s.weight = 0.0f; }
    int si = 0;
    for (int src = 0; src < NUM_SOURCES && si < NUM_SLOTS; ++src)
    {
        if (! srcOn[src]) continue;
        Slot& sl = slots[si++];
        sl.engine = src;
        sl.weight = srcWeight[src];
        sl.atk = srcAtk[src]; sl.hold = srcHold[src]; sl.dec = srcDec[src];
        switch (src)
        {
            case SrcSample:
                sl.smpSpeed = playSpeed; sl.smpCrush = sampleCrush; sl.smpPitch = pitch;
                sl.smpPEnvAmt = pitchEnvAmt; sl.smpPEnvTime = pitchEnvTime; sl.smpPOffset = pitchOffset;
                sl.smpReverse = sampleReverse; sl.smpUseRegion = useRegion;
                sl.smpStart = sampleStart; sl.smpEnd = sampleEnd; sl.smpSlices = sliceCount; sl.smpStretch = stretchAmt;
                break;
            case SrcNoise:
                sl.sustain = noiseSustain;
                sl.noiseType = noiseType; sl.noiseCenter = layerNoiseCenter; sl.noiseWidth = layerNoiseWidth;
                break;
            case SrcOsc:
                sl.sustain = oscSustain; sl.vibrato = oscVibrato;
                { static const int leg2wv[4] = { WvSine, WvTri, WvSquare, WvSaw };   // legacy 0..3 -> v5 slot index
                  sl.oscShape = leg2wv[juce::jlimit(0, 3, layerOscShape)]; }
                sl.oscFreq = layerSineFreq;
                sl.oscPEnvAmt = layerSinePEnvAmt; sl.oscPEnvTime = layerSinePEnvTime; sl.oscPOffset = layerSinePOffset;
                sl.oscUnison = oscUnison; sl.oscDetune = oscDetune;
                sl.fmDepth = 0.0f; sl.fmSpread = 0.0f; sl.fmFeedback = 0.0f; sl.fmSub = 0.0f;   // pure analog (FM section off)
                break;
            case SrcFM:   // FM merged into the Oscillator engine: author as SrcOsc with the FM section ON
                sl.engine = SrcOsc;
                sl.sustain = fmSustain;
                sl.oscShape = 0;   // the FM carrier was a sine (oscShapeB := oscShape below)
                sl.oscFreq  = juce::jlimit(20.0f, 1000.0f, (float)(220.0 * std::pow(2.0, (double) fmPitch / 12.0)));  // Pitch(st) -> Freq(Hz)
                sl.fmSpread = fmSpread; sl.fmDepth = fmDepth; sl.fmFeedback = fmFeedback; sl.fmSub = fmSub;
                sl.oscPEnvAmt = fmPitchEnvAmt; sl.oscPEnvTime = fmPitchEnvTime; sl.oscPOffset = fmPitchOffset;  // FM pitch env -> osc slot
                sl.fmEnvFollow = legacyFmEnvFollow;   // channel-level bridge (survives applyPreset's rebuild)
                break;
            case SrcPhys:
                sl.sustain = physSustain; sl.vibrato = physVibrato;
                sl.physFreq = physFreq; sl.physTone = physTone; sl.physMaterial = physMaterial; sl.physPosition = physPosition;
                sl.physPEnvAmt = physPitchEnvAmt; sl.physPEnvTime = physPitchEnvTime; sl.physPOffset = physPitchOffset;
                break;
            default: break;
        }
        sl.oscShapeB = sl.oscShape;   // legacy/factory sounds: Wave B == Wave A (no morph)

        // Migrate the legacy single-stage pitch env (amt/time/off) into the 4-dot model so
        // it shows in (and is owned by) the new PITCH ENVELOPE visual. Sample the old
        // exponential drop at 4 points; lenSamp mirrors the render-time voiceLenSamp so the
        // timing is preserved. Zero the old field afterwards (no double application).
        {
            float  pa = 0.0f, pt = 0.05f, po = 0.0f; float* legacyAmt = nullptr;
            switch (sl.engine) {
                case SrcOsc:                 pa = sl.oscPEnvAmt;  pt = sl.oscPEnvTime;  po = sl.oscPOffset;  legacyAmt = &sl.oscPEnvAmt;  break;
                case SrcFM:                  pa = sl.fmPEnvAmt;   pt = sl.fmPEnvTime;   po = sl.fmPOffset;   legacyAmt = &sl.fmPEnvAmt;   break;
                case SrcPhys:                pa = sl.physPEnvAmt; pt = sl.physPEnvTime; po = sl.physPOffset; legacyAmt = &sl.physPEnvAmt; break;
                case SrcSample:              pa = sl.smpPEnvAmt;  pt = sl.smpPEnvTime;  po = sl.smpPOffset;  legacyAmt = &sl.smpPEnvAmt;  break;
                default: break;
            }
            if (legacyAmt != nullptr && pa != 0.0f)
            {
                double lenSamp;
                if (sl.engine == SrcSample) {
                    const double frames = (double) getSampleNumFrames();
                    const double frac   = sl.smpUseRegion ? juce::jmax(0.0, (double)(sampleEnd - sampleStart)) : 1.0;
                    const double spd    = juce::jmax(0.05, (double) sl.smpSpeed);
                    lenSamp = (frames > 0.0) ? frames * frac / spd
                                             : (double)((sl.atk + sl.hold + 3.2f * sl.dec) * (float) sr);
                } else {
                    lenSamp = (double)((sl.atk + sl.hold + 3.2f * sl.dec
                                        + (sl.sustain > 0.0f ? 3.2f * sl.release : 0.0f)) * (float) sr);
                }
                lenSamp = juce::jmax(1.0, lenSamp);
                const double offSamp = juce::jlimit(0.0f, 1.0f, po) * 0.3 * (double) sr;
                const double decSamp = 3.2 * juce::jmax(0.005f, pt) * (double) sr;
                const double offFrac = juce::jlimit(0.0, 1.0, offSamp / lenSamp);
                const double endFrac = juce::jlimit(offFrac, 1.0, (offSamp + decSamp) / lenSamp);
                const double fr[4] = { offFrac, offFrac + (endFrac - offFrac) * 0.18,
                                                offFrac + (endFrac - offFrac) * 0.5, endFrac };
                for (int k = 0; k < Slot::NPE; ++k) {
                    sl.pEnvT[k] = (float) juce::jlimit(0.0, 1.0, fr[k]);
                    sl.pEnvP[k] = pa * pitchEnvShape((long)(fr[k] * lenSamp), pt, po);
                }
                *legacyAmt = 0.0f;   // now owned by the 4-dot pitch envelope
            }
        }
    }
    // Per-slot FILTER migration: a legacy sound's CHANNEL LowPass (Acid Bass, Filter Bass, etc.)
    // now lives on each built slot instead. A linear LP on each slot, summed, == the same LP on
    // the mix, so the sound is unchanged - but it's now per-slot (it only filters its own engine,
    // and it shows on the slot's EQ, not "All"). Clear the channel filter so it isn't ALSO applied
    // on the mix (that would filter twice). Only LowPass migrates; Formant/other stay channel-wide.
    if (filterType == LowPass && si > 0)
    {
        for (int k = 0; k < si; ++k)
        {
            slots[k].filterType   = filterType;   slots[k].filterCutoff = filterCutoff;
            slots[k].filterReso   = filterReso;   slots[k].filterEnvAmt = filterEnvAmt;
        }
        filterType = FilterOff;   // no longer on the mix
    }
    // Channel DRIVE migration (SINGLE-slot sounds only): like the filter above, the legacy channel
    // drive is an invisible factory-only stage (no knob) that made the "All" spectrum differ from
    // the slot's with nothing visibly enabled. With ONE slot at weight 1, drive-on-the-slot ==
    // drive-on-the-mix (at full velocity), so move it to the slot's visible/editable FX Drive.
    // MULTI-slot sounds keep the channel stage: a nonlinear drive on the MIX (inter-modulating
    // both slots) cannot be split per slot without changing the sound.
    if (si == 1 && driveType != DriveOff && driveAmount > 0.0f && slots[0].fxDrive <= 0.0001f)
    {
        slots[0].fxDriveType = driveType;  slots[0].fxDrive = driveAmount;
        driveType = DriveOff;  driveAmount = 0.0f;
    }
    ensureKsBuffers();   // a legacy Physical source needs the KS lines (message thread)
}

void DrumChannel::writeSlots(juce::ValueTree& parent) const
{
    for (int b = 0; b < NUM_SLOTS; ++b)
    {
        const Slot& s = slots[b];
        juce::ValueTree st("Slot");
        st.setProperty("eng", s.engine, nullptr); st.setProperty("w", s.weight, nullptr);
        st.setProperty("v2", 1, nullptr);   // merged-engine era marker (Analog now carries an FM section)
        st.setProperty("v3", 1, nullptr);   // Warp = wavefold era (old phase-skew warp is dropped on load)
        st.setProperty("v4", 1, nullptr);   // trimmed wave list era (pre-v4 indices remapped on load)
        st.setProperty("v5", 1, nullptr);   // 14-shape list: Hump back at index 1 (v4 indices shifted)
        st.setProperty("v6", 1, nullptr);   // drive taper era: amounts stored for the SQUARED gain law
        st.setProperty("atk", s.atk, nullptr); st.setProperty("hld", s.hold, nullptr);
        st.setProperty("dec", s.dec, nullptr); st.setProperty("sus", s.sustain, nullptr);
        st.setProperty("rel", s.release, nullptr); st.setProperty("vib", s.vibrato, nullptr);
        st.setProperty("oSh", s.oscShape, nullptr); st.setProperty("oSB", s.oscShapeB, nullptr); st.setProperty("oFr", s.oscFreq, nullptr);
        st.setProperty("oEA", s.oscPEnvAmt, nullptr); st.setProperty("oET", s.oscPEnvTime, nullptr); st.setProperty("oOf", s.oscPOffset, nullptr);
        st.setProperty("oUn", s.oscUnison, nullptr); st.setProperty("oDt", s.oscDetune, nullptr);
        st.setProperty("uniSp", s.uniSpread, nullptr);
        st.setProperty("scOn", s.scaleOn, nullptr); st.setProperty("scTy", s.scaleType, nullptr);
        st.setProperty("scUn", s.scaleUnison, nullptr); st.setProperty("scKy", s.scaleKey, nullptr);   // SCALE mode
        st.setProperty("oUC", s.oscUniCenter, nullptr); st.setProperty("oDM", s.oscDetuneMode, nullptr);
        st.setProperty("fxDt", s.fxDriveType, nullptr); st.setProperty("fxDr", s.fxDrive, nullptr);
        // ("fxRv"/"fxDl" retired - sends are CHANNEL-level now; old keys migrate in readSlots.)
        st.setProperty("nTy", s.noiseType, nullptr); st.setProperty("nCt", s.noiseCenter, nullptr); st.setProperty("nWd", s.noiseWidth, nullptr);
        st.setProperty("nRs", s.noiseRes, nullptr); st.setProperty("nDr", s.noiseDrive, nullptr); st.setProperty("nCk", s.noiseCrackle, nullptr);
        st.setProperty("fPi", s.fmPitch, nullptr); st.setProperty("fSp", s.fmSpread, nullptr); st.setProperty("fDe", s.fmDepth, nullptr);
        st.setProperty("fEA", s.fmPEnvAmt, nullptr); st.setProperty("fET", s.fmPEnvTime, nullptr); st.setProperty("fOf", s.fmPOffset, nullptr);
        st.setProperty("fFb", s.fmFeedback, nullptr); st.setProperty("fSu", s.fmSub, nullptr);
        st.setProperty("fFo", s.fmEnvFollow, nullptr);   // FM Amount follows the amp envelope
        st.setProperty("pFr", s.physFreq, nullptr); st.setProperty("pTo", s.physTone, nullptr); st.setProperty("pMa", s.physMaterial, nullptr); st.setProperty("pPo", s.physPosition, nullptr);
        st.setProperty("pSt", s.physStiff, nullptr); st.setProperty("pEx", s.physExcite, nullptr);
        st.setProperty("grPo", s.grainPos, nullptr); st.setProperty("grSz", s.grainSize, nullptr);
        st.setProperty("grDn", s.grainDens, nullptr); st.setProperty("grSp", s.grainSpray, nullptr);
        st.setProperty("grPi", s.grainPitch, nullptr);   // GRANULAR
        st.setProperty("pEA", s.physPEnvAmt, nullptr); st.setProperty("pET", s.physPEnvTime, nullptr); st.setProperty("pOf", s.physPOffset, nullptr);
        st.setProperty("sSp", s.smpSpeed, nullptr); st.setProperty("sCr", s.smpCrush, nullptr); st.setProperty("sPi", s.smpPitch, nullptr);
        st.setProperty("sEA", s.smpPEnvAmt, nullptr); st.setProperty("sET", s.smpPEnvTime, nullptr); st.setProperty("sOf", s.smpPOffset, nullptr);
        st.setProperty("sRv", s.smpReverse, nullptr); st.setProperty("sRg", s.smpUseRegion, nullptr);
        st.setProperty("sSt", s.smpStart, nullptr); st.setProperty("sEn", s.smpEnd, nullptr);
        st.setProperty("rN", s.smpRegN, nullptr);
        for (int r = 0; r < Slot::MAXREG; ++r) { st.setProperty("rL" + juce::String(r), s.smpRegLo[r], nullptr);
                                                 st.setProperty("rH" + juce::String(r), s.smpRegHi[r], nullptr); }
        st.setProperty("sSl", s.smpSlices, nullptr); st.setProperty("sStr", s.smpStretch, nullptr);
        st.setProperty("sGn", s.smpGain, nullptr);
        st.setProperty("sEnv", s.smpEnvOn, nullptr);   // opt-in sample amp envelope
        st.setProperty("sPP", s.smpPreservePitch, nullptr);   // preserve pitch (ignore step/draw/key pitch)
        st.setProperty("sFile", slotSample[b].file.getFullPathName(), nullptr);   // per-slot sample (reloaded)
        st.setProperty("yFo", s.oscFold, nullptr); st.setProperty("yOL", s.oscLevel, nullptr);
        st.setProperty("yNL", s.noiseLevel, nullptr);
        st.setProperty("wTb", s.waveTable, nullptr); st.setProperty("wPs", s.wavePos, nullptr);  // wavetable
        st.setProperty("oWp", s.oscWarp, nullptr);   // oscillator wave warp
        st.setProperty("mMa", s.modalMaterial, nullptr); st.setProperty("mDe", s.modalDecay, nullptr);   // modal
        st.setProperty("mTo", s.modalTone, nullptr); st.setProperty("mSt", s.modalStruct, nullptr);
        st.setProperty("mHi", s.modalHit, nullptr); st.setProperty("mDp", s.modalDamp, nullptr);
        st.setProperty("mMo", s.modalMorph, nullptr);   // material morph (toward the next material)
        // 4-point pitch envelope (pitch + time fraction per dot)
        st.setProperty("zP0", s.pEnvP[0], nullptr); st.setProperty("zP1", s.pEnvP[1], nullptr); st.setProperty("zP2", s.pEnvP[2], nullptr); st.setProperty("zP3", s.pEnvP[3], nullptr);
        st.setProperty("zT0", s.pEnvT[0], nullptr); st.setProperty("zT1", s.pEnvT[1], nullptr); st.setProperty("zT2", s.pEnvT[2], nullptr); st.setProperty("zT3", s.pEnvT[3], nullptr);
        // (the old per-slot 5-band EQ was DELETED 2026-07-16 - "qe*" keys no longer written)
        // === PER-SLOT FILTER (begin) ===
        st.setProperty("flT", s.filterType, nullptr); st.setProperty("flC", s.filterCutoff, nullptr);
        st.setProperty("flR", s.filterReso, nullptr); st.setProperty("flE", s.filterEnvAmt, nullptr);
        st.setProperty("flT2", s.filterType2, nullptr); st.setProperty("flC2", s.filterCutoff2, nullptr);   // FILTER 2 (series)
        st.setProperty("flG", s.filterGain, nullptr); st.setProperty("flG2", s.filterGain2, nullptr);   // BELL gain (bipolar dB)
        st.setProperty("flR2", s.filterReso2, nullptr); st.setProperty("flE2", s.filterEnvAmt2, nullptr);
        // === PER-SLOT FILTER (end) ===
        // (chM/fxCp/fxFl/fxPh retired - Chorus/Comp/Flanger/Phaser are CHANNEL FX now, saved on the channel.)
        st.setProperty("fxPn", s.fxPunch, nullptr); st.setProperty("fxRg", s.fxRing, nullptr);
        st.setProperty("fxRgH", s.fxRingHz, nullptr);   // Ring carrier (Hz; < 26 = track the note)
        st.setProperty("sPan",  s.pan,      nullptr);   // static slot pan (-1..+1)
        st.setProperty("fxSb", s.fxSub, nullptr);   st.setProperty("fxFm", s.fxFormant, nullptr);
        // ("fxTn" retired - Tone removed 2026-07-13; the Bell filter covers the tilt. Old keys ignored.)
        for (int f = 0; f < ADD_FRAMES; ++f)            // ADDITIVE WAVETABLE: 4 drawn frames (CSV per frame)
        { juce::String hs, ps;
          for (int k = 0; k < ADD_HARM; ++k) { const juce::String c2 = k < ADD_HARM - 1 ? "," : "";
              hs << juce::String(s.addH[f][k], 3) << c2; ps << juce::String(s.addPh[f][k], 3) << c2; }
          st.setProperty("aH" + juce::String(f), hs, nullptr);
          st.setProperty("aP" + juce::String(f), ps, nullptr); }
        st.setProperty("aSg0", s.addSeg[0], nullptr);   // per-leg glide times (0 = hold)
        st.setProperty("aSg1", s.addSeg[1], nullptr);
        st.setProperty("aSg2", s.addSeg[2], nullptr);
        st.setProperty("aLp",  s.addLoop, nullptr);   // glide LOOP (ping-pong)
        st.setProperty("aPos", s.addPos, nullptr);
        // Per-slot LFOs: one rate+amount(+sync+shape+free) PER DESTINATION (0=filter 1=pitch 2=vol 3=wave).
        for (int d2 = 0; d2 < 4; ++d2)
        {
            st.setProperty("lfR" + juce::String(d2), s.lfoRate[d2], nullptr);
            st.setProperty("lfA" + juce::String(d2), s.lfoAmt[d2], nullptr);
            st.setProperty("lfS" + juce::String(d2), s.lfoSync[d2], nullptr);   // tempo sync base (0 = off)
            st.setProperty("lfSh" + juce::String(d2), s.lfoShape[d2], nullptr);  // wave shape (0 sine .. 7 custom)
            st.setProperty("lfFr" + juce::String(d2), s.lfoFree[d2], nullptr);   // free-run (timeline-anchored)
            st.setProperty("lfLg" + juce::String(d2), s.lfoLegato[d2], nullptr); // legato retrig
            if (s.lfoShape[d2] == 7)                                              // LFO SHAPER drawn curve
            { juce::String cv;
              for (int k = 0; k < Slot::LFO_CURVE_N; ++k)
                  cv << juce::String(s.lfoCurve[d2][k], 3) << (k < Slot::LFO_CURVE_N - 1 ? "," : "");
              st.setProperty("lfCv" + juce::String(d2), cv, nullptr);
              st.setProperty("lfCg" + juce::String(d2), (int) s.lfoCurveGrid[d2], nullptr);   // draw-window Grid
              st.setProperty("lfCn" + juce::String(d2), s.lfoCurveSnap[d2], nullptr); }       //  + Snap (saved with the sound)
        }
        st.setProperty("drf", s.drift, nullptr);          // DRIFT (alive) amount
        st.setProperty("flDrv", s.filterDrive, nullptr);  // filter loop saturation
        // MOD MATRIX: routes packed "src:tgt:amt;..." + the two matrix-created source params.
        { juce::String mm;
          for (int r = 0; r < MOD_ROUTES; ++r)
          {
              mm << (int) s.mod[r].src << ":" << (int) s.mod[r].tgt << ":" << juce::String(s.mod[r].amt, 4)
                 << ":" << juce::String(s.mod[r].lagMs, 1);   // [2026-07-14 01:33] LAG ms (4th field)
              if (s.mod[r].curveOn)   // MOD AMOUNT MAP curve = the 5th field (128 hex chars); absent = pass-through
              { mm << ":"; for (int k = 0; k < Slot::MOD_CURVE_N; ++k) mm << juce::String::toHexString(s.mod[r].curve[k]).paddedLeft('0', 2); }
              mm << ";";
          }
          st.setProperty("mmx", mm, nullptr); }
        st.setProperty("mEA", s.modEnvA, nullptr); st.setProperty("mED", s.modEnvD, nullptr);
        st.setProperty("mEH", s.modEnvH, nullptr); st.setProperty("mES", s.modEnvS, nullptr); st.setProperty("mER", s.modEnvR, nullptr);
        st.setProperty("mLR", s.modLfoRate, nullptr); st.setProperty("mLS", s.modLfoShape, nullptr);
        parent.addChild(st, -1, nullptr);
    }
}

bool DrumChannel::readSlots(const juce::ValueTree& parent)
{
    int n = 0;
    const Slot d;   // struct defaults used as per-field fallbacks
    float legacyChorus = 0.0f, legacyComp = 0.0f;   // old per-slot chorus/comp -> CHANNEL FX slots (migration below)
    float legacyRev = 0.0f, legacyDel = 0.0f;       // old per-slot sends -> CHANNEL sends
    for (int j = 0; j < parent.getNumChildren() && n < NUM_SLOTS; ++j)
    {
        auto st = parent.getChild(j);
        if (st.getType() != juce::Identifier("Slot")) continue;
        Slot& s = slots[n];  s = Slot();
        s.engine = (int)  st.getProperty("eng", -1);
        // MIGRATE the retired slot engines (SrcSynth=5 / SrcWave=6, removed v1.2.x) onto Analog+FM so
        // OLD projects play a real sound instead of falling through to silence. SrcModal=7 is a live
        // engine and is left alone. After this, no current data ever holds SrcSynth/SrcWave.
        if (s.engine == SrcSynth || s.engine == SrcWave) s.engine = SrcOsc;
        s.weight = (float)st.getProperty("w", 0.0f);
        s.atk = (float)st.getProperty("atk", d.atk); s.hold = (float)st.getProperty("hld", d.hold);
        s.dec = (float)st.getProperty("dec", d.dec); s.sustain = (float)st.getProperty("sus", d.sustain);
        s.release = (float)st.getProperty("rel", d.release); s.vibrato = (float)st.getProperty("vib", d.vibrato);
        s.oscShape = (int)st.getProperty("oSh", d.oscShape); s.oscShapeB = (int)st.getProperty("oSB", s.oscShape); s.oscFreq = (float)st.getProperty("oFr", d.oscFreq);
        if      (! st.hasProperty("v4")) { s.oscShape = remapPreV4Shape(s.oscShape); s.oscShapeB = remapPreV4Shape(s.oscShapeB); }  // old 17-shape list
        else if (! st.hasProperty("v5")) { s.oscShape = remapV4Shape(s.oscShape);    s.oscShapeB = remapV4Shape(s.oscShapeB); }     // brief 13-shape list
        s.oscPEnvAmt = (float)st.getProperty("oEA", d.oscPEnvAmt); s.oscPEnvTime = (float)st.getProperty("oET", d.oscPEnvTime); s.oscPOffset = (float)st.getProperty("oOf", d.oscPOffset);
        s.oscUnison = (int)st.getProperty("oUn", d.oscUnison); s.oscDetune = (float)st.getProperty("oDt", d.oscDetune);
        s.uniSpread = (float)st.getProperty("uniSp", d.uniSpread);
        // "chd"/"chdU" (the DELETED legacy chord mode, 2026-07-16) are ignored on load.
        s.scaleOn = (bool) st.getProperty("scOn", d.scaleOn); s.scaleType = (int) st.getProperty("scTy", d.scaleType);
        s.scaleUnison = (int) st.getProperty("scUn", d.scaleUnison); s.scaleKey = (int) st.getProperty("scKy", d.scaleKey);
        s.oscUniCenter = (bool)st.getProperty("oUC", d.oscUniCenter);
        s.oscDetuneMode = (int)st.getProperty("oDM", d.oscDetuneMode);
        s.fxDriveType = (int)st.getProperty("fxDt", d.fxDriveType); s.fxDrive = (float)st.getProperty("fxDr", d.fxDrive);
        // v6 drive-taper migration: pre-v6 amounts were stored for the LINEAR gain law; sqrt lands on
        // the identical gain under the new SQUARED law (Bitcrush keeps its own amount semantics).
        if (! st.hasProperty("v6") && s.fxDriveType != Bitcrush && s.fxDrive > 0.0f)
            s.fxDrive = std::sqrt(juce::jlimit(0.0f, 1.0f, s.fxDrive));
        // LEGACY MIGRATION: sends used to be PER SLOT ("fxRv"/"fxDl") - lift the strongest slot's
        // send onto the CHANNEL (readChannel/readChannelMix ran before this and set the channel keys
        // for NEW files; old files have no channel keys, so the max lands cleanly).
        legacyRev = juce::jmax(legacyRev, (float) st.getProperty("fxRv", 0.0f));
        legacyDel = juce::jmax(legacyDel, (float) st.getProperty("fxDl", 0.0f));
        s.noiseType = (int)st.getProperty("nTy", d.noiseType); s.noiseCenter = (float)st.getProperty("nCt", d.noiseCenter); s.noiseWidth = (float)st.getProperty("nWd", d.noiseWidth);
        s.noiseRes = (float)st.getProperty("nRs", d.noiseRes); s.noiseDrive = (float)st.getProperty("nDr", d.noiseDrive); s.noiseCrackle = (float)st.getProperty("nCk", d.noiseCrackle);
        s.fmPitch = (float)st.getProperty("fPi", d.fmPitch); s.fmSpread = (float)st.getProperty("fSp", d.fmSpread); s.fmDepth = (float)st.getProperty("fDe", d.fmDepth);
        s.fmPEnvAmt = (float)st.getProperty("fEA", d.fmPEnvAmt); s.fmPEnvTime = (float)st.getProperty("fET", d.fmPEnvTime); s.fmPOffset = (float)st.getProperty("fOf", d.fmPOffset);
        s.fmFeedback = (float)st.getProperty("fFb", d.fmFeedback); s.fmSub = (float)st.getProperty("fSu", d.fmSub);
        s.fmEnvFollow = (bool)st.getProperty("fFo", d.fmEnvFollow);
        // Pre-merge projects: an Analog slot saved fmDepth=0.4 (then unused). Now Analog HAS an FM
        // section, so zero those fields for old Analog slots or they'd suddenly be FM-modulated.
        if (! st.hasProperty("v2") && s.engine == SrcOsc) { s.fmDepth = 0.0f; s.fmSpread = 0.0f; s.fmFeedback = 0.0f; s.fmSub = 0.0f; }
        s.physFreq = (float)st.getProperty("pFr", d.physFreq); s.physTone = (float)st.getProperty("pTo", d.physTone); s.physMaterial = (float)st.getProperty("pMa", d.physMaterial); s.physPosition = (float)st.getProperty("pPo", d.physPosition);
        s.physStiff = (float)st.getProperty("pSt", d.physStiff); s.physExcite = (int)st.getProperty("pEx", d.physExcite);
        s.grainPos = (float)st.getProperty("grPo", d.grainPos); s.grainSize = (float)st.getProperty("grSz", d.grainSize);
        s.grainDens = (float)st.getProperty("grDn", d.grainDens); s.grainSpray = (float)st.getProperty("grSp", d.grainSpray);
        s.grainPitch = (float)st.getProperty("grPi", d.grainPitch);
        s.physPEnvAmt = (float)st.getProperty("pEA", d.physPEnvAmt); s.physPEnvTime = (float)st.getProperty("pET", d.physPEnvTime); s.physPOffset = (float)st.getProperty("pOf", d.physPOffset);
        s.smpSpeed = (float)st.getProperty("sSp", d.smpSpeed); s.smpCrush = (float)st.getProperty("sCr", d.smpCrush); s.smpPitch = (float)st.getProperty("sPi", d.smpPitch);
        s.smpPEnvAmt = (float)st.getProperty("sEA", d.smpPEnvAmt); s.smpPEnvTime = (float)st.getProperty("sET", d.smpPEnvTime); s.smpPOffset = (float)st.getProperty("sOf", d.smpPOffset);
        s.smpReverse = (bool)st.getProperty("sRv", d.smpReverse); s.smpUseRegion = (bool)st.getProperty("sRg", d.smpUseRegion);
        s.smpStart = (float)st.getProperty("sSt", d.smpStart); s.smpEnd = (float)st.getProperty("sEn", d.smpEnd);
        s.smpRegN = juce::jlimit(0, Slot::MAXREG, (int)st.getProperty("rN", 0));
        for (int r = 0; r < Slot::MAXREG; ++r) { s.smpRegLo[r] = (float)st.getProperty("rL" + juce::String(r), 0.0f);
                                                 s.smpRegHi[r] = (float)st.getProperty("rH" + juce::String(r), 1.0f); }
        // Migrate an old single trim region -> region 0.
        if (s.smpRegN == 0 && s.smpUseRegion && (s.smpStart > 0.001f || s.smpEnd < 0.999f))
        { s.smpRegN = 1; s.smpRegLo[0] = s.smpStart; s.smpRegHi[0] = s.smpEnd; }
        s.smpSlices = (int)st.getProperty("sSl", d.smpSlices); s.smpStretch = (float)st.getProperty("sStr", d.smpStretch);
        s.smpGain = (float)st.getProperty("sGn", d.smpGain);
        s.smpEnvOn = (bool)st.getProperty("sEnv", d.smpEnvOn);
        s.smpPreservePitch = (bool)st.getProperty("sPP", d.smpPreservePitch);   // default true (old projects preserve pitch)
        s.oscFold = (float)st.getProperty("yFo", d.oscFold); s.oscLevel = (float)st.getProperty("yOL", d.oscLevel);
        s.noiseLevel = (float)st.getProperty("yNL", d.noiseLevel);
        s.waveTable = (int)st.getProperty("wTb", d.waveTable); s.wavePos = (float)st.getProperty("wPs", d.wavePos);
        s.oscWarp = (float)st.getProperty("oWp", d.oscWarp);
        if (! st.hasProperty("v3")) s.oscWarp = 0.0f;   // pre-wavefold: drop the old phase-skew warp (different effect)
        s.modalMaterial = (int)st.getProperty("mMa", d.modalMaterial); s.modalDecay = (float)st.getProperty("mDe", d.modalDecay);
        s.modalTone = (float)st.getProperty("mTo", d.modalTone); s.modalStruct = (float)st.getProperty("mSt", d.modalStruct);
        s.modalHit = (float)st.getProperty("mHi", d.modalHit); s.modalDamp = (float)st.getProperty("mDp", d.modalDamp);
        s.modalMorph = (float)st.getProperty("mMo", d.modalMorph);
        s.pEnvP[0] = (float)st.getProperty("zP0", d.pEnvP[0]); s.pEnvP[1] = (float)st.getProperty("zP1", d.pEnvP[1]); s.pEnvP[2] = (float)st.getProperty("zP2", d.pEnvP[2]); s.pEnvP[3] = (float)st.getProperty("zP3", d.pEnvP[3]);
        s.pEnvT[0] = (float)st.getProperty("zT0", d.pEnvT[0]); s.pEnvT[1] = (float)st.getProperty("zT1", d.pEnvT[1]); s.pEnvT[2] = (float)st.getProperty("zT2", d.pEnvT[2]); s.pEnvT[3] = (float)st.getProperty("zT3", d.pEnvT[3]);
        // ("qe*" = the DELETED per-slot 5-band EQ, 2026-07-16 - ignored on load)
        // === PER-SLOT FILTER (begin) ===
        s.filterType = (int)st.getProperty("flT", d.filterType); s.filterCutoff = (float)st.getProperty("flC", d.filterCutoff);
        s.filterReso = (float)st.getProperty("flR", d.filterReso); s.filterEnvAmt = (float)st.getProperty("flE", d.filterEnvAmt);
        const float legacyKt1 = (float) st.getProperty("flK", 0.0f);    // retired keytrack -> migrated to a Note->cutoff route below
        s.filterType2   = (int)  st.getProperty("flT2", d.filterType2);   s.filterCutoff2 = (float)st.getProperty("flC2", d.filterCutoff2);
        s.filterReso2   = (float)st.getProperty("flR2", d.filterReso2);   s.filterEnvAmt2 = (float)st.getProperty("flE2", d.filterEnvAmt2);
        s.filterGain  = (float)st.getProperty("flG",  d.filterGain);
        s.filterGain2 = (float)st.getProperty("flG2", d.filterGain2);
        // MIGRATION (dev-only window): the first Bell stored its BOOST in the reso field with a fixed
        // Q 1.1 - lift it into the gain field so those saves keep their sound.
        if (s.filterType  == Bell && ! st.hasProperty("flG"))  { s.filterGain  = juce::jlimit(-15.0f, 15.0f, s.filterReso);  s.filterReso  = 1.1f; }
        if (s.filterType2 == Bell && ! st.hasProperty("flG2")) { s.filterGain2 = juce::jlimit(-15.0f, 15.0f, s.filterReso2); s.filterReso2 = 1.1f; }
        const float legacyKt2 = (float) st.getProperty("flK2", 0.0f);
        // === PER-SLOT FILTER (end) ===
        s.fxPunch = (float)st.getProperty("fxPn", d.fxPunch);
        s.fxRing  = (float)st.getProperty("fxRg", d.fxRing);
        s.fxRingHz = (float)st.getProperty("fxRgH", d.fxRingHz);
        s.pan      = (float)st.getProperty("sPan",  d.pan);
        s.fxSub     = (float)st.getProperty("fxSb", d.fxSub);
        s.fxFormant = (float)st.getProperty("fxFm", d.fxFormant);
        // LEGACY MIGRATION: chorus + comp used to be PER SLOT ("chM"/"fxCp"). They are CHANNEL FX now -
        // take the strongest slot's amount onto the channel (applied once to both layers combined).
        legacyChorus = juce::jmax(legacyChorus, (float) st.getProperty("chM",  0.0f));
        legacyComp   = juce::jmax(legacyComp,   (float) st.getProperty("fxCp", 0.0f));
        {   // ADDITIVE WAVETABLE frames (aH0..aH3/aP0..aP3 + aMt/aPos). LEGACY 2-spectrum files
            // (aH/aP = A, aHB/aPB = B, aMt = A->B seconds) migrate to frames {A,B,B,B} with
            // morphSec x3: the new glide crosses A->B in the first THIRD of the strip, so
            // 3x the time = the exact same A->B crossfade, then holds B - bit-identical.
            auto rd = [&st](const juce::String& key, float* dst, float dflt) {
                if (! st.hasProperty(key)) return false;
                juce::StringArray a; a.addTokens(st.getProperty(key).toString(), ",", "");
                for (int k = 0; k < ADD_HARM; ++k) dst[k] = k < a.size() ? a[k].getFloatValue() : dflt;
                return true;
            };
            if (st.hasProperty("aH0"))
            {
                for (int f = 0; f < ADD_FRAMES; ++f)
                { rd("aH" + juce::String(f), s.addH[f], 0.0f); rd("aP" + juce::String(f), s.addPh[f], 0.0f); }
                if (st.hasProperty("aSg0"))
                    for (int k = 0; k < ADD_FRAMES - 1; ++k)
                        s.addSeg[k] = juce::jmax(0.0f, (float) st.getProperty("aSg" + juce::String(k), 0.0f));
                else
                {   // brief whole-strip-glide generation (single "aMt" over all 3 legs): even split
                    const float m = (float) st.getProperty("aMt", 0.0f);
                    if (m > 0.001f) s.addSeg[0] = s.addSeg[1] = s.addSeg[2] = m / 3.0f;
                }
                s.addLoop = (bool) st.getProperty("aLp", d.addLoop);
                s.addPos = juce::jlimit(0.0f, 1.0f, (float) st.getProperty("aPos", d.addPos));
            }
            else if (st.hasProperty("aH"))
            {   // ORIGINAL 2-spectrum files (A + B, morph aMt then stay on B) = travel the FIRST
                // leg in aMt then HOLD - exactly {aMt, 0, 0} in the per-segment model.
                rd("aH", s.addH[0], 0.0f); rd("aP", s.addPh[0], 0.0f);
                const bool hadB = rd("aHB", s.addH[1], 0.0f); rd("aPB", s.addPh[1], 0.0f);
                for (int f = 2; f < ADD_FRAMES; ++f)
                    for (int k = 0; k < ADD_HARM; ++k)
                    { s.addH[f][k] = s.addH[hadB ? 1 : 0][k]; s.addPh[f][k] = s.addPh[hadB ? 1 : 0][k]; }
                if (! hadB) for (int k = 0; k < ADD_HARM; ++k) { s.addH[1][k] = s.addH[0][k]; s.addPh[1][k] = s.addPh[0][k]; }
                s.addSeg[0] = (float) st.getProperty("aMt", 0.0f);
            }
        }
        // Per-slot LFOs (per-dest). Legacy single-LFO files (lfR/lfA/lfD) migrate onto their dest's LFO.
        for (int d2 = 0; d2 < 4; ++d2)
        {
            s.lfoRate[d2] = (float)st.getProperty("lfR" + juce::String(d2), d.lfoRate[d2]);
            s.lfoAmt[d2]  = (float)st.getProperty("lfA" + juce::String(d2), d.lfoAmt[d2]);
            s.lfoSync[d2] = (float)st.getProperty("lfS" + juce::String(d2), d.lfoSync[d2]);
            if (s.lfoSync[d2] > -1.5f && s.lfoSync[d2] < 0.0f)   // retired GRID (-1) -> the equivalent bar value [2026-07-15 14:20]
                s.lfoSync[d2] = (float) juce::jmax(1, drawMode ? 16 : numSteps);
            s.lfoShape[d2] = juce::jlimit(0, 7, (int) st.getProperty("lfSh" + juce::String(d2), d.lfoShape[d2]));
            s.lfoFree[d2]  = (bool) st.getProperty("lfFr" + juce::String(d2), d.lfoFree[d2]);
            s.lfoLegato[d2] = (bool) st.getProperty("lfLg" + juce::String(d2), d.lfoLegato[d2]);
            s.lfoCurveGrid[d2] = (uint8_t) juce::jlimit(1, 32, (int) st.getProperty("lfCg" + juce::String(d2), (int) d.lfoCurveGrid[d2]));
            s.lfoCurveSnap[d2] = (bool) st.getProperty("lfCn" + juce::String(d2), false);
            if (st.hasProperty("lfCv" + juce::String(d2)))
            { juce::StringArray a; a.addTokens(st.getProperty("lfCv" + juce::String(d2)).toString(), ",", "");
              for (int k = 0; k < Slot::LFO_CURVE_N; ++k)
                  s.lfoCurve[d2][k] = juce::jlimit(-1.0f, 1.0f, k < a.size() ? a[k].getFloatValue() : 0.0f); }
        }
        s.drift       = juce::jlimit(0.0f, 1.0f, (float) st.getProperty("drf", d.drift));
        s.filterDrive = juce::jlimit(0.0f, 1.0f, (float) st.getProperty("flDrv", d.filterDrive));
        // MOD MATRIX. Old files have no "mmx": their LFOs used the FIXED built-in destination
        // (LFO1->filter, 2->pitch, 3->vol, 4->wave-pos) which is GONE - re-create it as matrix
        // routes so the project still modulates. Files WITH "mmx" carry their explicit routes.
        if (st.hasProperty("mmx"))
        {
            juce::StringArray rows; rows.addTokens(st.getProperty("mmx").toString(), ";", "");
            for (int r = 0; r < MOD_ROUTES && r < rows.size(); ++r)
            {
                juce::StringArray f2; f2.addTokens(rows[r], ":", "");
                if (f2.size() >= 3)
                { s.mod[r].src = (int8_t) juce::jlimit(0, MS_COUNT - 1, f2[0].getIntValue());
                  s.mod[r].tgt = (int8_t) juce::jlimit(0, MT_COUNT - 1, f2[1].getIntValue());
                  s.mod[r].amt = juce::jlimit(-1.0f, 1.0f, f2[2].getFloatValue());
                  // 4th field: LAG ms (numeric) - or the few-hours-old dev format where it was the hex curve.
                  int cvField = -1;
                  if (f2.size() >= 4)
                  { if (f2[3].length() == Slot::MOD_CURVE_N * 2) cvField = 3;
                    else { s.mod[r].lagMs = juce::jlimit(0.0f, 2000.0f, f2[3].getFloatValue());
                           if (f2.size() >= 5 && f2[4].length() == Slot::MOD_CURVE_N * 2) cvField = 4; } }
                  if (cvField > 0)
                  { s.mod[r].curveOn = 1;
                    for (int k = 0; k < Slot::MOD_CURVE_N; ++k)
                        s.mod[r].curve[k] = (uint8_t) f2[cvField].substring(k * 2, k * 2 + 2).getHexValue32(); } }
            }
        }
        else
        {
            const int classicTgt[4] = { MTFilt1Cut, MTPitch, MTVol, MTWavePos };
            int nr = 0;
            for (int d2 = 0; d2 < 4 && nr < MOD_ROUTES; ++d2)
                if (s.lfoAmt[d2] > 0.001f)
                { s.mod[nr].src = (int8_t)(MSLfoFilt + d2); s.mod[nr].tgt = (int8_t) classicTgt[d2]; s.mod[nr].amt = 1.0f; ++nr; }
        }
        // RETIRED FILTER KEYTRACK ("flK"/"flK2"): the dedicated per-filter keytrack is gone - recreate it
        // as a per-voice matrix route. Equivalence: keytrack kt = cutoff x 2^(kt*semis/12); the Note source
        // is semis/24 and the cutoff target applies 2^(amt*src*4) => amt = kt/2 (exact within +-24 st).
        for (int fi = 0; fi < 2; ++fi)
        {
            const float kt = fi == 0 ? legacyKt1 : legacyKt2;
            if (kt < 0.01f) continue;
            bool have = false;   // don't double-add if the file already carries a Note->cutoff route
            const int tgt = fi == 0 ? MTFilt1Cut : MTFilt2Cut;
            for (auto& r : s.mod) have = have || (r.src == MSNote && r.tgt == tgt);
            if (! have)
                for (auto& r : s.mod)
                    if (r.src == MSOff && r.tgt == MTOff)
                    { r.src = MSNote; r.tgt = (int8_t) tgt; r.amt = juce::jlimit(0.0f, 1.0f, kt) * 0.5f; break; }
        }
        s.modEnvA = juce::jlimit(0.001f, 8.0f, (float) st.getProperty("mEA", d.modEnvA));
        s.modEnvD = juce::jlimit(0.01f, 8.0f, (float) st.getProperty("mED", d.modEnvD));
        s.modEnvH = juce::jlimit(0.0f, 4.0f, (float) st.getProperty("mEH", d.modEnvH));
        s.modEnvS = juce::jlimit(0.0f, 1.0f, (float) st.getProperty("mES", d.modEnvS));
        s.modEnvR = juce::jlimit(0.0f, 8.0f, (float) st.getProperty("mER", d.modEnvR));
        s.modLfoRate = juce::jlimit(0.05f, 20.0f, (float) st.getProperty("mLR", d.modLfoRate));
        s.modLfoShape = juce::jlimit(0, 6, (int) st.getProperty("mLS", d.modLfoShape));
        if (st.hasProperty("lfD") && ! st.hasProperty("lfA0"))
        {
            const int ld = juce::jlimit(0, 2, (int)st.getProperty("lfD", 0));
            s.lfoRate[ld] = (float)st.getProperty("lfR", 4.0f);
            s.lfoAmt[ld]  = (float)st.getProperty("lfA", 0.0f);
        }
        // Reload this slot's sample file (smpStretch was just read, so updateStretch uses the right value).
        { juce::String sp = st.getProperty("sFile", "").toString();
          slotSample[n].buf.setSize(1, 0); slotSample[n].original.setSize(1, 0);
          slotSample[n].file = juce::File(); slotSample[n].usingUser = false;
          if (sp.isNotEmpty()) { juce::File sf(sp); if (sf.existsAsFile()) loadUserSample(n, sf); } }
        ++n;
    }
    for (int b = n; b < NUM_SLOTS; ++b) { slots[b] = Slot(); slotSample[b] = SlotSample(); }   // clear unused slots
    // MIGRATION (v1.3.9): a pre-CHANNEL-FX file has per-slot "chM"/"fxCp" and no channel keys - lift the
    // strongest slot's chorus/comp onto the channel. New files write no chM/fxCp, so legacy* stays 0 here
    // and the channel values read from the file (in readChannel, which ran BEFORE this) are left alone.
    {   // old per-slot chorus/comp -> the first free CHANNEL FX slots (character 0.5 = the old voicing)
        auto place = [&](int type, float amt) {
            if (amt <= 0.001f) return;
            for (int f2 = 0; f2 < 3; ++f2)
                if (chFxType[f2] == ChFxOff)
                { chFxType[f2] = type; chFxAmt[f2] = juce::jlimit(0.0f, 1.0f, amt); chFxChar[f2] = 0.5f; return; }
        };
        place(ChFxChorus, legacyChorus);
        place(ChFxComp,   legacyComp);
    }
    if (legacyRev > 0.001f && reverbSend <= 0.001f) reverbSend = juce::jlimit(0.0f, 1.0f, legacyRev);
    if (legacyDel > 0.001f && delaySend  <= 0.001f) delaySend  = juce::jlimit(0.0f, 1.0f, legacyDel);
    if (n > 0) ensureKsBuffers();   // restored slots may use a KS engine (message thread)
    rebuildAddTables();   // drawn harmonics -> tables (message thread; load paths)
    return n > 0;
}

void DrumChannel::prepareToPlay(double sampleRate, int maxBlockSize)
{
    rebuildAddTables();   // custom (additive) waves must exist before audio runs
    sr = sampleRate;
    waveBank();   // force-build the wavetable bank now (message thread), not on the audio thread

    for (auto& b : formantBP) b.reset();

    for (auto& v : voices)
    {
        v.playHead = -1.0;
        v.killing = false; v.killGain = 1.0f;
        for (auto& sv : v.sv) sv.noiseBP.reset();
    }

    renderBuf.setSize(2, maxBlockSize);

    bool anySamp = false;
    for (auto& ss : slotSample) if (ss.buf.getNumSamples() > 0) anySamp = true;
    if (! anySamp) loadDefaultSound();

    // Samples are resampled to the HOST rate at load; if the host rate changed since
    // (or the load happened before the rate was known), reload from the file cache.
    {
        const double hostRate = engineOS > 0 ? sr / (double) engineOS : sr;
        for (int s = 0; s < NUM_SLOTS; ++s)
            if (slotSample[s].usingUser && slotSample[s].file.existsAsFile()
                && std::abs(slotSample[s].loadedAtRate - hostRate) > 0.5)
                loadUserSample(s, slotSample[s].file);
    }

    // Only derive slots from the legacy fields if NONE is set up yet (a brand-new channel had its default slots
    // authored in the processor ctor; a loaded project sets them via readSlots / the post-load buildSlotsFromLegacy).
    // Rebuilding unconditionally here used to wipe the authored default (the first 2 legacy srcOn -> Sample+Noise).
    bool anySlot = false;
    for (auto& s : slots) if (s.engine >= 0) { anySlot = true; break; }
    if (! anySlot) buildSlotsFromLegacy();
    ensureKsBuffers();   // allocate the KS lines if any slot runs a KS engine (message thread)
    updateDSP();
}

// Allocate every voice-slot's Karplus-Strong delay line if any slot uses a KS engine.
// MESSAGE THREAD ONLY. Vectors are allocated once and never resized again; the audio
// thread gates all access on ksReady (acquire), so this is race-free.
void DrumChannel::ensureKsBuffers()
{
    if (ksReady.load(std::memory_order_acquire)) return;
    bool needs = false;
    for (auto& s : slots)
        if (s.engine == SrcPhys) { needs = true; break; }
    if (! needs) return;
    for (auto& v : voices)
        for (auto& sv : v.sv)
            sv.ksBuf.assign((size_t) (KS_UNI * KS_MAX), 0.0f);
    ksReady.store(true, std::memory_order_release);
}

void DrumChannel::loadDefaultSound()
{
    // No built-in synth samples — a Sample slot is silent until a file is loaded into it
    // (the Osc/Noise/FM/Physical engines still make sound).
    const juce::ScopedLock sl(sampleLock);
    for (auto& ss : slotSample) { ss.buf.setSize(1, 0); ss.original.setSize(1, 0); ss.file = juce::File(); ss.usingUser = false; }
    for (auto& v : voices) v.playHead = -1.0;
}

//==============================================================================
// SAMPLE FILE CACHE. A project stores its sounds per pattern, so loading one project can ask
// for the SAME file up to 32 times (once per pattern's copy of the channel). Decode + resample
// once, then hand out copies. Keyed by path + mtime + size + target rate; message thread only.
namespace {
struct SampleFileCache
{
    struct Entry { juce::String key; juce::AudioBuffer<float> buf; };
    std::vector<Entry> entries;
    juce::CriticalSection cs;
    static constexpr int MAX_ENTRIES = 24;   // bound the cache (LRU-ish: oldest dropped first)

    // Load `file` decoded AND resampled to targetRate into `out`. Returns false if unreadable.
    bool get(const juce::File& file, double targetRate, juce::AudioBuffer<float>& out)
    {
        const juce::String key = file.getFullPathName() + "|" + juce::String(file.getLastModificationTime().toMilliseconds())
                               + "|" + juce::String(file.getSize()) + "|" + juce::String(targetRate, 1);
        {
            const juce::ScopedLock sl(cs);
            for (auto& e : entries) if (e.key == key) { out = e.buf; return true; }
        }

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
        if (!reader) return false;
        const int len = (int) reader->lengthInSamples;
        if (len <= 0) return false;
        const int nCh = (int) juce::jmax((unsigned int) 1, reader->numChannels);

        juce::AudioBuffer<float> raw(nCh, len);
        reader->read(&raw, 0, len, 0, true, true);

        // RESAMPLE to the host rate at load. Without this a 48 kHz file in a 44.1 kHz session
        // played ~9% slow/flat (the playback head consumes file frames 1:1 at the host rate).
        const double fileRate = reader->sampleRate > 0.0 ? reader->sampleRate : targetRate;
        if (std::abs(fileRate - targetRate) < 0.5)
            out = std::move(raw);
        else
        {
            const double ratio  = fileRate / targetRate;              // input frames per output frame
            const int    outLen = juce::jmax(1, (int) std::floor((double) len / ratio));
            out.setSize(nCh, outLen);
            for (int c = 0; c < nCh; ++c)
            {
                juce::LagrangeInterpolator interp;
                interp.process(ratio, raw.getReadPointer(c), out.getWritePointer(c), outLen, len, 0);
            }
        }

        const juce::ScopedLock sl(cs);
        if ((int) entries.size() >= MAX_ENTRIES) entries.erase(entries.begin());
        entries.push_back({ key, out });
        return true;
    }
};
static SampleFileCache& sampleFileCache() { static SampleFileCache c; return c; }
} // namespace

void DrumChannel::loadUserSample(int slot, const juce::File& file)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    // Samples play back at the HOST base rate (the engine's 2x oversampling is compensated
    // by the /engineOS head advance), so that's the rate we resample to.
    const double hostRate = engineOS > 0 ? sr / (double) engineOS : sr;

    juce::AudioBuffer<float> newSample;
    if (! sampleFileCache().get(file, hostRate, newSample)) return;

    {
        const juce::ScopedLock sl(sampleLock);
        slotSample[slot].original = std::move(newSample);   // keep the unstretched source
        for (auto& v : voices) v.playHead = -1.0;
    }

    slotSample[slot].usingUser = true;
    slotSample[slot].file = file;
    slotSample[slot].loadedAtRate = hostRate;
    updateStretch(slot);   // builds slotSample[slot].buf from .original (= a copy when stretch == 1)
}

// Rebuild slotSample[slot].buf from its .original at slots[slot].smpStretch (pitch-preserving time-stretch).
// No-op if that slot has no loaded source. Without SoundTouch the stretch is unavailable (buf = original).
void DrumChannel::updateStretch(int slot)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;
    auto& ss = slotSample[slot];
    if (ss.original.getNumSamples() == 0) return;

#if BASAMAK_HAVE_SOUNDTOUCH
    const int nCh = juce::jmax(1, ss.original.getNumChannels());
    const int nFr = ss.original.getNumSamples();
    const float amt = juce::jlimit(0.25f, 4.0f, slots[slot].smpStretch);
    // Pitch-shift is INDEPENDENT of stretch (SoundTouch's pitch function): changes pitch, keeps length.
    const double pitchSemis = (double) pitch + (double) slots[slot].smpPitch;   // channel Pitch knob + slot
    const bool noStretch = (amt > 0.999f && amt < 1.001f);
    const bool noPitch   = (pitchSemis > -0.01 && pitchSemis < 0.01);
    if (noStretch && noPitch) { const juce::ScopedLock sl(sampleLock); ss.buf = ss.original; for (auto& v : voices) v.playHead = -1.0; return; }

    soundtouch::SoundTouch st;
    st.setSampleRate((unsigned int) juce::jmax(8000.0, sr));
    st.setChannels((unsigned int) nCh);
    st.setTempo(1.0 / amt);                  // length: tempo < 1 -> longer (pitch unchanged)
    st.setPitchSemiTones(pitchSemis);        // pitch: shift WITHOUT changing length
    // Highest-quality settings (anti-alias filter on + long, accurate seek - slower but best fidelity).
    st.setSetting(SETTING_USE_AA_FILTER, 1);
    st.setSetting(SETTING_AA_FILTER_LENGTH, 64);
    st.setSetting(SETTING_USE_QUICKSEEK, 0);

    std::vector<float> in((size_t) nFr * (size_t) nCh);
    for (int i = 0; i < nFr; ++i) for (int c = 0; c < nCh; ++c) in[(size_t) i * nCh + c] = ss.original.getSample(c, i);
    st.putSamples(in.data(), (unsigned int) nFr);
    st.flush();

    std::vector<float> out; out.reserve((size_t)((float) nFr * amt + 8192.0f) * (size_t) nCh);
    std::vector<float> chunk((size_t) 4096 * (size_t) nCh);
    unsigned int got;
    while ((got = st.receiveSamples(chunk.data(), 4096)) > 0)
        out.insert(out.end(), chunk.begin(), chunk.begin() + (size_t) got * (size_t) nCh);

    const int outFr = juce::jmax(1, (int)(out.size() / (size_t) nCh));
    juce::AudioBuffer<float> stretched(nCh, outFr);
    stretched.clear();
    for (int i = 0; i < outFr; ++i) for (int c = 0; c < nCh; ++c) stretched.setSample(c, i, out[(size_t) i * nCh + c]);

    const juce::ScopedLock sl(sampleLock);
    ss.buf = std::move(stretched);
    for (auto& v : voices) v.playHead = -1.0;
#else
    const juce::ScopedLock sl(sampleLock);   // no SoundTouch: just play the original
    ss.buf = ss.original;
    for (auto& v : voices) v.playHead = -1.0;
#endif
}


void DrumChannel::getWaveformPeaks(int slot, int numBuckets, std::vector<float>& mins, std::vector<float>& maxs)
{
    mins.assign((size_t) numBuckets, 0.0f);
    maxs.assign((size_t) numBuckets, 0.0f);
    if (slot < 0 || slot >= NUM_SLOTS) return;
    const juce::ScopedTryLock stl(sampleLock);
    if (!stl.isLocked()) return;
    const auto& buf = slotSample[slot].buf;
    const int n = buf.getNumSamples();
    if (n <= 0 || numBuckets <= 0) return;
    const int chs = buf.getNumChannels();
    for (int b = 0; b < numBuckets; ++b)
    {
        int s0 = (int)((int64_t) b * n / numBuckets);
        int s1 = (int)((int64_t)(b + 1) * n / numBuckets);
        if (s1 <= s0) s1 = s0 + 1;
        float mn = 0.0f, mx = 0.0f;
        for (int c = 0; c < chs; ++c)
        {
            const float* d = buf.getReadPointer(c);
            for (int i = s0; i < s1 && i < n; ++i) { mn = juce::jmin(mn, d[i]); mx = juce::jmax(mx, d[i]); }
        }
        mins[(size_t) b] = mn; maxs[(size_t) b] = mx;
    }
}

float DrumChannel::getSamplePlayheadFrac(int slot) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return -1.0f;
    if (slots[slot].engine != SrcSample) return -1.0f;
    const int len = slotSample[slot].buf.getNumSamples();
    if (len <= 0) return -1.0f;
    // Newest active voice = the hit the user is hearing most prominently.
    long newest = -1; double head = -1.0;
    for (const auto& v : voices)
        if (v.active() && (newest < 0 || v.voiceSamples < newest)) { newest = v.voiceSamples; head = v.sv[slot].smpHead; }
    if (head < 0.0) return -1.0f;
    float frac = juce::jlimit(0.0f, 1.0f, (float)(head / (double) len));
    if (slots[slot].smpReverse)   // the head advances forward; the READ position is mirrored in the region
        frac = juce::jlimit(0.0f, 1.0f, slotSample[slot].curRegLo + slotSample[slot].curRegHi - frac);
    return frac;
}

// (The 303-tie slideTo() is GONE: slide is a retriggered portamento now - see Sequencer::fireEvent.)

float DrumChannel::physDecayScale(int material)
{
    return kPhysModels[juce::jlimit(0, kNumPhysModels - 1, material)].decScale;
}

// (kChordTab/chordSemis = the LEGACY CHORD MODE - DELETED 2026-07-16 on user order; SCALE is the one voicing system.)

// SCALE mode (a diatonic HARMONIZER). Each scale = ascending semitone offsets from its root; kScaleLen
// = how many notes it has (5/6/7). Unlike chords these intervals are NOTE-DEPENDENT (they change with the
// played note's scale degree), so scaleSemis() is called PER NOTE and the result stored in
// SlotVoice::uniSemis (not baked into the shared per-block config).
static const int8_t kScaleTab[10][7] = {
    { 0,2,4,5,7,9,11 },   // 0 Major
    { 0,2,3,5,7,8,10 },   // 1 Natural Minor
    { 0,2,3,5,7,8,11 },   // 2 Harmonic Minor
    { 0,2,3,5,7,9,10 },   // 3 Dorian
    { 0,1,3,5,7,8,10 },   // 4 Phrygian
    { 0,2,4,6,7,9,11 },   // 5 Lydian
    { 0,2,4,5,7,9,10 },   // 6 Mixolydian
    { 0,2,4,7,9,0,0 },    // 7 Major Pentatonic (5)
    { 0,3,5,7,10,0,0 },   // 8 Minor Pentatonic (5)
    { 0,3,5,6,7,10,0 },   // 9 Blues (6)
};
static const int kScaleLen[10] = { 7,7,7,7,7,7,7,5,5,6 };
// GUITAR VOICINGS (types 10 = Gtr Major, 11 = Gtr Minor): not diatonic stacks - REAL barre
// shapes from the snapped root, snapping in Major / Natural Minor. The SHAPE (and its STRING
// COUNT) follows the root like on a real neck: roots E..G# = E-shape barre (6 strings,
// R,5,R,3,5,R), everything else = A-shape barre (5 strings, R,5,R,3,5). Unused strings return
// kGtrNone and are SKIPPED by the renders - the Notes count is AUTO for these types (user spec:
// "different chords have different string numbers"). With STRUM up this rakes like a guitar.
static constexpr int8_t kGtrNone = -100;

// Voice k's offset (semitones, relative to the PLAYED note) for the diatonic chord of the note's scale
// degree. Off-scale/between notes SNAP to the nearest scale member (tie -> lower); voice 0 = the snapped
// root (offset 0 on-scale), higher voices stack diatonic thirds within the scale (deg += 2 per voice,
// octave-wrapped). key = 0..11 root pitch class (C = 0), playedMidi = the note in MIDI numbers.
static inline int scaleSemis(int scaleType, int key, int playedMidi, int k)
{
    scaleType = juce::jlimit(0, 12, scaleType);
    if (scaleType == 12)
    {   // POWER (v1.3.9, user): root + 5th + octave stack - quality-neutral, CHROMATIC (no snap,
        // any key is a valid root, like sliding a power chord). Notes count extends the stack.
        static const int kPower[7] = { 0, 7, 12, 19, 24, 31, 36 };
        return kPower[juce::jlimit(0, 6, k)];
    }
    const bool gtr = scaleType >= 10;                                // guitar voicings snap in Maj/Min
    const int  tab = gtr ? scaleType - 10 : scaleType;
    const int8_t* S = kScaleTab[tab];
    const int     N = kScaleLen[tab];
    const int pc = ((playedMidi - key) % 12 + 12) % 12;              // pitch class relative to the key root
    int deg = 0, bestC = (int) S[0], bestD = 100;
    for (int i = 0; i < N; ++i)                                      // nearest member (with octave wraps), tie -> lower
        for (int wrap = -12; wrap <= 12; wrap += 12) {
            const int c = (int) S[i] + wrap, d = std::abs(c - pc);
            if (d < bestD || (d == bestD && c < bestC)) { bestD = d; bestC = c; deg = i; }
        }
    const int snapDelta = bestC - pc;                               // 0 when the note is on-scale
    if (gtr)
    {   // GUITAR voicing, DIATONIC (user round-2: it must follow the KEY, not play only-maj/min):
        // the chord QUALITY comes from the snapped note's scale DEGREE (C major: D plays D minor,
        // B plays B dim...), voiced as a barre pattern R,5,R,3',5',R'' whose STRING COUNT follows
        // the root like a real neck - E..G# = 6-string E-shape, D/D# = 4-string D-shape, the
        // rest = 5-string A-shape. Missing strings = kGtrNone (skipped by the renders).
        auto ivAt = [&](int steps) {                                 // diatonic interval deg -> deg+steps
            const int td = deg + steps, oct = (int) std::floor((double) td / (double) N);
            return (int) S[td - oct * N] + 12 * oct - (int) S[deg];
        };
        const int third = ivAt(2), fifth = ivAt(4);
        const int rootPc = ((playedMidi + snapDelta) % 12 + 12) % 12;
        const int nStrings = (rootPc >= 4 && rootPc <= 8) ? 6 : (rootPc == 2 || rootPc == 3) ? 4 : 5;
        if (k >= nStrings) return kGtrNone;                          // no such string on this chord
        static const int slot[6] = { 0, 1, 2, 3, 4, 5 };            // R,5,R',3'+12,5'+12,R''
        switch (slot[juce::jlimit(0, 5, k)])
        {
            case 0:  return snapDelta;
            case 1:  return snapDelta + fifth;
            case 2:  return snapDelta + 12;
            case 3:  return snapDelta + 12 + third;
            case 4:  return snapDelta + 12 + fifth;
            default: return snapDelta + 24;
        }
    }
    const int td  = deg + 2 * k;
    const int oct = (int) std::floor((double) td / (double) N);
    const int idx = td - oct * N;
    return snapDelta + ((int) S[idx] + 12 * oct - (int) S[deg]);    // root's stack interval + the snap
}

// Public wrappers so the editor's keyboard-highlight can use the exact DSP voicing (single source).
int DrumChannel::scaleNoteOffset(int scaleType, int key, int playedMidi, int voiceIdx)
{ return scaleSemis(scaleType, key, playedMidi, voiceIdx); }

int DrumChannel::trigger(float velocityGain, float pitchSemis, float pan, long gateSamples,
                         float glideToSemis, long glideSamples, bool forceOverlap, int slotMask, bool keyGate,
                         bool knobBase)
{
    // slotMask 0 (or all-bits) = every slot sounds; a piano-roll note may restrict to slot 1 or 2.
    const int mask = (slotMask == 0) ? ~0 : slotMask;
    // [2026-07-16] Per-note LEGATO playback: capture the newest ringing voice's envelope age NOW
    // (before any mono handover fade marks it killing) - applied to the fresh voice at the end.
    long legatoInherit = -1;
    if (legatoNext)
    {
        legatoNext = false;
        for (auto& vv : voices)
            if (vv.active() && vv.voiceSamples > legatoInherit) legatoInherit = vv.voiceSamples;
    }
    // If this is the channel the editor is analysing, start a fresh spectrum
    // capture aligned to this hit's attack so repeats look identical.
    if (analysisTap != nullptr) analysisTap->arm();

    // Pick a voice. Mono: always voice 0 and silence the rest (cuts previous).
    // Overlap: a free voice, or steal the one nearest its end. KEYS always force overlap:
    // on a mono channel the reuse of voice 0 hard-reset the still-sounding note = the slide
    // CRACKLE (the 15 ms handover fade needs the old note on its own voice to fade).
    int vi = 0;
    if (allowOverlap || forceOverlap)
    {
        vi = -1;
        for (int i = 0; i < POLY; ++i) if (!voices[i].active()) { vi = i; break; }
        if (vi < 0) { long best = -1; for (int i = 0; i < POLY; ++i)
                        if (voices[i].voiceSamples > best) { best = voices[i].voiceSamples; vi = i; } }
    }
    else
    {
        // [2026-07-14 10:05] MONO RETRIGGER = PITCH-AWARE HANDOVER (the user's bass-roll crackle,
        // verified on DT 770s): this branch used to HARD-REUSE voice 0 mid-ring (playHead reset,
        // phases restarted) and even zeroed the channel EQ/formant states - a 40 Hz tail still at
        // +-0.8 jumped straight to the new hit's first sample = a discontinuity on EVERY fast
        // retrigger of subby sounds (bright drums hide it: their tails are quiet by the next hit).
        // Now the old voice FADES on its own slot for >= ~1.2 cycles of the sound's bass while the
        // new hit starts clean on a free voice - the exact keys mono-handover precedent. The
        // filter-state resets are gone too (the overlap path never reset them; zeroing a biquad
        // mid-tail was its own step).
        fadeOutVoices(retrigFadeSec());
        vi = -1;
        for (int i = 0; i < POLY; ++i) if (!voices[i].active()) { vi = i; break; }
        if (vi < 0) { long best = -1; for (int i = 0; i < POLY; ++i)
                        if (voices[i].voiceSamples > best) { best = voices[i].voiceSamples; vi = i; } }
    }

    // Reset the free-running modulation LFOs so every hit starts at the same phase - otherwise non-zero Drift
    // (weight wobble) or Vibrato (varispeed on samples) makes the sound vary between identical hits.
    vibPhase   = 0.0f;

    Voice& v = voices[vi];
    v.velGain    = juce::jlimit(0.0f, 1.0f, velocityGain);
    v.voicePitch = pitchSemis;
    v.voicePan   = juce::jlimit(-1.0f, 1.0f, pan);
    v.playHead = 0.0;          // alive (per-slot sample heads do the reading)
    v.voiceSamples = 0;
    v.killing = false; v.killGain = 1.0f; v.killStep = 0.0f;   // fresh hit cancels any choke fade on this voice
    v.gateLen = gateSamples;                // per-step Length gate (0 = play naturally)
    v.isKey = false; v.keyOff = -1;         // sequencer hit by default (keyDown() patches these after)
    if (keyGate && gateSamples > 0)
    {   // PIANO-ROLL note: behave like a key held for the note's length and RELEASED at its end -
        // natural ring while it runs (no decay-rescale), the authored release fade after. This is
        // what makes a recording SOUND like the take (a sustain-0 pluck used to get its whole ring
        // compressed into the note length). keyNote -1 = keyUp(note) never matches it.
        v.isKey = true; v.keyNote = -1; v.keyOff = gateSamples;
        v.keyChan = 0; v.pressTgt = v.pressCur = v.slideTgt = v.slideCur = v.bendTgt = v.bendCur = 0.0f;
    }
    // SLIDE: start at THIS step's own pitch (normal attack) and glide per-sample toward the
    // NEXT step's pitch, landing exactly when the glide time (= the step span) runs out.
    if (glideSamples > 0) { v.glideRemain = glideSamples;
                            v.glideStep = (glideToSemis - pitchSemis) / (float) glideSamples; }
    else                  { v.glideRemain = 0; v.glideStep = 0.0f; }

    // SLOT OFFSET (humanizeAmt, 0..1 = 0..100 ms): slot 2 fires a CONSISTENT (deterministic) amount
    // AFTER slot 1, for a layering flam/thickening. Slot 1 is the on-time anchor. 0 = bit-identical.
    const int slotOffsetSamples = (int) std::lround((double) humanizeAmt * 0.100 * (double) sr);   // 0..100 ms

    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        SlotVoice& sv = v.sv[s];
        const Slot& sl = slots[s];
        // Per-step LENGTH: rescale THIS slot's decay so attack+hold+fall fills the note length.
        // Frozen here (per voice) - ties extend the voice's life but never reshape a running decay.
        sv.gateDec = (gateSamples > 0 && ! keyGate)   // keyGate = natural envelope, never rescaled
            ? juce::jmax(0.01f, (float) gateSamples / (float) sr - sl.atk - sl.hold) : 0.0f;
        sv.lfoPhase[0] = sv.lfoPhase[1] = sv.lfoPhase[2] = sv.lfoPhase[3] = 0.0;  // per-hit LFO restart (locks to the groove)
        for (int d2 = 0; d2 < 4; ++d2)
            sv.lfoCyc[d2] = ((sl.lfoShape[d2] == 4 || sl.lfoShape[d2] == 6) && sl.lfoAmt[d2] > 0.001f && ! sl.lfoFree[d2])
                ? (uint32_t) driftRng.nextInt() : 0;   // Random shapes (Retrig): fresh random pattern per note
        // LEGATO retrig: inherit the phase (and Random pattern) from the newest OTHER sounding
        // voice - overlapping/legato notes ride the same wave; detached notes restart normally.
        for (int d2 = 0; d2 < 4; ++d2)
            if (sl.lfoLegato[d2] && ! sl.lfoFree[d2] && sl.lfoAmt[d2] > 0.001f)
            {
                const Voice* nv = nullptr;
                for (auto& v2 : voices)
                    if (&v2 != &v && v2.active() && (nv == nullptr || v2.voiceSamples < nv->voiceSamples)) nv = &v2;
                if (nv != nullptr)
                { sv.lfoPhase[d2] = nv->sv[s].lfoPhase[d2]; sv.lfoCyc[d2] = nv->sv[s].lfoCyc[d2]; }
            }
        sv.wtPosCur = -1.0f;                       // live-position read-out: nothing rendered yet
        sv.grAcc = 1.0f;                           // GRANULAR: first grain fires immediately;
        sv.grNoteIdx = 0;                          // chord/scale note cycling restarts (identical-hits rule)
        for (auto& gr : sv.grains) { gr.age = 0; gr.len = 0; }   // stale grains from the last note die
        sv.keySemis = 0.0f;
        // TEST on a PIANO-ROLL channel: the block config is C4-absolute (slotBaseHz), so play
        // each pitched slot at its own Base Freq knob via keySemis (user: "TEST should use the
        // Base Freq knob"). Step channels already use the knob - knobBase is a no-op there.
        if (knobBase && drawMode
            && (sl.engine == SrcOsc || sl.engine == SrcPhys || sl.engine == SrcModal))
        {
            const double own = (sl.engine == SrcPhys) ? (double) sl.physFreq : (double) sl.oscFreq;
            const double base = slotBaseHz(s, sl);
            if (own > 1.0 && base > 1.0)
                sv.keySemis = (float) (12.0 * std::log2(own / base));
        }
        sv.keyMute = ! ((mask >> s) & 1);          // per-note slot tag: mute the slots this note doesn't play
        sv.sinePhase = 0.0;
        sv.noiseBP.reset();
        sv.pinkB[0] = sv.pinkB[1] = sv.pinkB[2] = 0.0f;
        sv.brownState = sv.prevWhite = 0.0f;
        sv.greyZ1 = sv.greyZ2 = 0.0f;
        for (int fi = 0; fi < 2; ++fi) {   // both SVFs start clean each hit (no retrigger click); snap the smoothed cutoff
            sv.filtIc1[fi][0] = sv.filtIc1[fi][1] = sv.filtIc2[fi][0] = sv.filtIc2[fi][1] = 0.0; sv.filtGm[fi] = -1.0; sv.filtKm[fi] = -1.0; }
        sv.wSm = -1.0f;   // weight smoother snaps to the (possibly modulated) weight on the first sample
        sv.envModLatched = false;   // env-time mod targets re-latch at THIS hit
        sv.lastEnv = 0.0f; sv.fmtPosSm = -1.0f;    // per-sample Amp Env source + formant glide re-snap at THIS hit
        for (int r2 = 0; r2 < MOD_ROUTES; ++r2) { sv.modLagV[r2] = 0.0f; sv.arLag[r2] = 0.0f; }   // LAG swells from 0 at the hit
        { const int n0 = juce::jlimit(1, UNI_MAX, sl.oscUnison);   // [2026-07-14 02:50] count-morph fades snap to the base stack
          for (int u2 = 0; u2 <= UNI_MAX; ++u2) sv.uniFade[u2] = (u2 < n0 || u2 == UNI_MAX) ? 1.0f : 0.0f; }
        sv.adaaU[0] = sv.adaaU[1] = sv.adaaB1[0] = sv.adaaB1[1] = sv.adaaB2[0] = sv.adaaB2[1]
            = sv.warpU[0] = sv.warpU[1] = sv.warpU[2] = 1.0e9f;   // ADAA states re-prime at THIS hit
        for (int lr = 0; lr < 2; ++lr) {   // drive smoothing/DC + formant + punch state start clean each hit
            sv.drvLp[lr] = sv.drvDcX[lr] = sv.drvDcY[lr] = 0.0f;
            sv.pFast[lr] = sv.pSlow[lr] = 0.0f;
            for (int b2 = 0; b2 < 2; ++b2) { sv.fmtZ1[b2][lr] = 0.0f; sv.fmtZ2[b2][lr] = 0.0f; } }
        sv.ringPh = 0.0; sv.subPh = 0.0;
        sv.fmCarrier = sv.fmMod = sv.fmSubPhase = 0.0; sv.fmFbState = 0.0f;
        sv.wtPhase = 0.0; sv.modalInit = false; sv.modalHold = false;   // re-strike the modal bank on this hit
        for (auto& row : sv.modalY1) for (auto& v2 : row) v2 = 0.0f;   // clean resonator state every hit
        for (auto& row : sv.modalY2) for (auto& v2 : row) v2 = 0.0f;
        sv.noiseState = 0x1234567u + (uint32_t)(vi * NUM_SLOTS + s) * 2654435761u; // distinct per voice+slot
        // [2026-07-14 02:15] HALF-CYCLE phase spread (was a FULL cycle): 16 voices evenly spread
        // around the whole circle sum to EXACTLY ZERO at equal frequency - unison 16 with detune 0
        // was completely silent (user find; the old 7-voice cap only covered a partial arc, which
        // is why it never fully cancelled before UNI_MAX went 7 -> 16). A half-cycle spread keeps
        // the deterministic decorrelation but its phasor sum is >= ~0.64N for EVERY count, so
        // detune-0 unison now sounds like one louder saw (the correct behaviour). Existing detuned
        // unison sounds keep their character; only the beat pattern's starting phase shifts.
        for (int u = 0; u <= UNI_MAX; ++u) sv.uniPhase[u] = kPi * (double) u / (double) UNI_MAX;

        // DRIFT: roll this note's dice (TRUE random - every note breathes differently, like analog
        // hardware; drift 0 = no rolls = bit-identical deterministic hits, the drum default).
        sv.driftGain = 1.0f; sv.driftWobMul = 1.0f; sv.driftWobPh = 0.0; sv.driftWobRate = 0.0f;
        sv.driftFiltMul = 1.0f;
        for (int u = 0; u <= UNI_MAX; ++u) sv.driftMul[u] = 1.0f;
        if (sl.drift > 0.001f)
        {
            const float d = sl.drift;
            const float phScat = juce::jmin(1.0f, d * 2.0f);   // phase blur saturates by ~50% drift
            const float noteCents = (driftRng.nextFloat() - 0.5f) * 16.0f * d;          // the WHOLE note lands
            for (int u = 0; u <= UNI_MAX; ++u)                                          // +-8c off per hit - the
            {                                                                            // clearest audible cue
                sv.uniPhase[u] += (double) phScat * driftRng.nextDouble() * 2.0 * kPi;  // phase scatter (unison blur)
                const float cents = noteCents + (driftRng.nextFloat() - 0.5f) * 30.0f * d;  // +-15c per voice on top
                sv.driftMul[u] = std::pow(2.0f, cents / 1200.0f);
            }
            sv.driftWobPh   = driftRng.nextDouble() * 2.0 * kPi;                        // slow wander start
            sv.driftWobRate = 0.15f + 0.85f * driftRng.nextFloat();                     // 0.15..1.0 Hz
            sv.driftGain    = 1.0f + (driftRng.nextFloat() - 0.5f) * 0.6f * d;          // level breath (+-30%)
            sv.driftFiltMul = std::pow(2.0f, (driftRng.nextFloat() - 0.5f) * 1.2f * d); // filter +-0.6 oct per note
        }
        // MOD MATRIX: roll this note's Random source (0..1). TRUE random per hit (the drift
        // precedent - passes differ microscopically; only recorded NOTES are reproduced, not the
        // live take). Rolled only when the matrix is live, so drums stay bit-identical.
        sv.modRand = sl.modActive() ? driftRng.nextFloat() : 0.5f;

        // SCALE mode (diatonic harmonizer): the chord depends on the played NOTE, so compute this voice's
        // per-note diatonic offsets now (keySemis is 0 for step/draw hits; keyDown recomputes for held keys).
        for (int u = 0; u < UNI_MAX; ++u) sv.uniSemis[u] = 0.0f;
        if (sl.scaleOn && (sl.engine == SrcOsc || sl.engine == SrcModal || sl.engine == SrcPhys || sl.engine == SrcGrain))
        {
            const double baseF = slotBaseHz(s, sl);
            const int playedMidi = (int) std::lround(69.0 + 12.0 * std::log2(juce::jmax(1.0, baseF) / 440.0) + (double) pitchSemis);
            const int nv = juce::jlimit(1, UNI_MAX, sl.scaleUnison);
            for (int u = 0; u < nv; ++u) sv.uniSemis[u] = (float) scaleSemis(sl.scaleType, sl.scaleKey, playedMidi, u);
        }

        // SLOT OFFSET: slot 2 fires slotOffsetSamples (0..100 ms) after slot 1, the same every hit
        // (consistent, not random). Slot 1 stays on time. 0 = bit-identical.
        sv.startDelay = (s == 1) ? slotOffsetSamples : 0;
        sv.velScale = 1.0f;
        for (auto& d : sv.uniDelay) d = 0;
        // STRUM: spread this slot's chord/scale notes low->high in time (deterministic; a strum on
        // every hit). Only when the slot voices distinct pitches (CHORD or SCALE), else notes are
        // identical and a "strum" would just comb-filter. uniDelay[0] stays 0 (the root leads).
        const float sAmt = strumOverride >= 0.0f ? strumOverride : strumAmt;   // per-note override (piano roll)
        if (sAmt > 0.001f && sl.scaleOn
            && (sl.engine == SrcOsc || sl.engine == SrcModal || sl.engine == SrcPhys || sl.engine == SrcGrain))   // grain strums its cloud [2026-07-16]
        {
            const int nStr = juce::jlimit(1, UNI_MAX, sl.scaleType >= 10 ? 6 : sl.scaleUnison);
            if (nStr > 1)
            {
                // The ALTERNATE stroke (strumFlip) is a real UPSTROKE, not just a reversed list:
                // quicker (x0.7 spread) and LIGHTER (x0.82 level). Order alone was proven
                // inaudible in a live log session - the accent pattern is what a strumming hand
                // actually sounds like (down-UP-down-UP = loud-light-loud-light).
                const double spreadMul = strumFlip ? 0.7 : 1.0;
                const double perVoice = sAmt * 0.180 * spreadMul * (double) sr / (double)(nStr - 1);
                for (int u = 0; u < nStr; ++u)
                    sv.uniDelay[u] = (int) std::lround(perVoice * (double) (strumFlip ? nStr - 1 - u : u));
                if (strumFlip) sv.velScale *= 0.82f;
            }
        }
        if (s == NUM_SLOTS - 1) { strumFlip = false; strumOverride = -1.0f; }   // one-shot flags: live sets them
                                                     // per strum; playback must never inherit stale values

        // Sample slot: this hit plays the NEXT slice of THIS slot's own sample, from its slice start
        // (= region start when slicing is off). Each Sample slot has independent slices/region/buffer.
        double head = 0.0;
        if (sl.engine == SrcSample)
        {
            const int n = slotSample[s].buf.getNumSamples();
            if (n > 0)
            {
                float lo = 0.0f, hi = 1.0f;
                if (sl.smpUseRegion)
                {
                    if (sl.smpRegN > 0)   // hand-drawn multi-regions: each hit plays the NEXT one (round-robin)
                    {
                        const int r = slotSample[s].sliceCounter % sl.smpRegN;
                        slotSample[s].sliceCounter = (slotSample[s].sliceCounter + 1) % sl.smpRegN;
                        lo = juce::jmin(sl.smpRegLo[r], sl.smpRegHi[r]);
                        hi = juce::jmax(sl.smpRegLo[r], sl.smpRegHi[r]);
                    }
                    else { lo = sl.smpStart; hi = sl.smpEnd; }   // legacy single region
                }
                lo = juce::jlimit(0.0f, 1.0f, lo); hi = juce::jlimit(0.0f, 1.0f, hi);
                if (hi <= lo + 0.0005f) hi = juce::jmin(1.0f, lo + 0.01f);
                slotSample[s].curRegLo = lo; slotSample[s].curRegHi = hi;   // the per-block region picks this up
                head = (double) lo * n;   // reverse is mirrored within [lo,hi] in the read loop
            }
        }
        sv.smpHead = head;

        // Karplus-Strong: pluck = fill the tuned delay with a noise burst, tone-shaped
        // by the Material model, then comb it by the strike Position. The KS line is
        // lazily heap-allocated (ensureKsBuffers); gate on ksReady + only clear/fill it
        // for engines that actually pluck (a 16 KB memset per slot per hit otherwise).
        // (SrcOsc's retired resonator section no longer plucks - its render never reads KS.)
        for (auto& w : sv.ksWrite) w = 0.0;
        for (auto& l : sv.ksLp) l = 0.0f;
        for (auto& row : sv.ksApSt) for (auto& a : row) a = 0.0f;
        const bool ksPluck = (ksReady.load(std::memory_order_acquire) && sl.engine == SrcPhys);
        if (ksPluck) std::fill(sv.ksBuf.begin(), sv.ksBuf.end(), 0.0f);
        if (ksPluck)
        {
            const float ksFreq0 = (float) slotBaseHz(s, sl);
            // User stiffness adds allpass DC delay to the loop - compensate the burst-fill length the
            // same way the render compensates its read length (designStiffChain), so pitch matches.
            // One comp from the base freq is shared by every unison string (matches the render).
            float apComp = 0.0f;
            if (sl.engine == SrcPhys && sl.physStiff > 0.01f)
            { float ac; int an; designStiffChain(sl.physStiff, slotBaseHz(s, sl), sr, ac, an, apComp); }
            const int mdl = juce::jlimit(0, kNumPhysModels - 1, (int) std::lround(sl.physMaterial));
            if (sl.engine == SrcPhys && kPhysModels[mdl].apC != 0.0f && kPhysModels[mdl].apStages > 0)
            {   // the MATERIAL chain's delay too (it STACKS with stiffness now) - keeps the
                // excitation fill length in tune with what the render will read back
                const float mc = kPhysModels[mdl].apC;
                const float tdc = (1.0f - mc) / (1.0f + mc);
                const float per = (float) (sr / juce::jmax(20.0, slotBaseHz(s, sl)));
                int n2 = kPhysModels[mdl].apStages;
                const float budget = juce::jmax(0.0f, 0.45f * per - apComp);
                while (n2 > 0 && tdc * (float) n2 > budget) --n2;
                apComp += tdc * (float) n2;
            }
            // EXCITATION shapes how the string/bar is set in motion: Pluck = full-length noise burst (rich);
            // Strike = a short sharp burst at the start (percussive mallet hit); Mallet = a soft, rounded burst.
            const int exc = (sl.engine == SrcPhys) ? juce::jlimit(0, 2, sl.physExcite) : 0;
            // v1.3.9 (user: "excite modes sound the same"): each mode fills the string DIFFERENTLY
            // now - Pluck = full rich noise burst; Strike = a SHORT LOUD slap (1/8 of the string,
            // x1.5, brighter); Mallet = a rounded half-cosine PUSH (almost no noise) = soft thump.
            const float eb = (exc == 1) ? juce::jmin(0.98f, kPhysModels[mdl].exciteBright * 1.3f)
                                        :  kPhysModels[mdl].exciteBright;
            const float pos = juce::jlimit(0.0f, 1.0f, sl.physPosition);
            // Physical unison/chord: excite one real string per note, each tuned to its own pitch
            // (chord interval + detune). Single-voice sounds excite string 0 only = bit-identical.
            const int nStr = (sl.engine == SrcPhys) ? juce::jlimit(1, KS_UNI, sl.scaleOn ? (sl.scaleType >= 10 ? 6 : sl.scaleUnison) : sl.oscUnison) : 1;

            auto exciteString = [&](int strIdx, float freqK)
            {
                const int base = strIdx * KS_MAX;
                const int L = juce::jlimit(2, KS_MAX - 2, (int) std::round(sr / juce::jmax(20.0f, freqK) - apComp));
                uint32_t rng = sv.noiseState ^ (0x9e3779b9u + (uint32_t) strIdx * 0x85ebca6bu);
                // PARTIAL excitations live at the END of the fill: the render's read pointer starts
                // at (write - L') where L' SHRINKS with a pitch-drop material (Skin starts ~9 semis
                // high) or heavy Stiffness compensation - a burst at the START sat behind the read
                // pointer and was NEVER read ("Strike is silent on Skin"; Mallet died when Skin +
                // full Stiffness stacked). Content at the end is always consumed before the write.
                if (exc == 2)
                {   // MALLET: a rounded half-cosine push over the LAST half + a whisper of noise -
                    // a padded hammer DISPLACING the string, not scratching it. Dark, thumpy.
                    const int half = juce::jmax(2, L / 2);
                    for (int i = 0; i < L; ++i)
                    {
                        const int j = i - (L - half);   // position inside the tail window
                        const float bump = (j >= 0) ? 0.9f * std::sin((float) kPi * (float) j / (float) half) : 0.0f;
                        sv.ksBuf[base + i] = bump + ((j >= 0) ? 0.15f * eb * whiteNoise(rng) : 0.0f);   // material fingerprint in the whisper
                    }
                }
                else
                {
                    const int burst = (exc == 1) ? juce::jmax(2, L / 8) : L;   // Strike: a short hard SLAP
                    // ~sqrt(L/burst) keeps the injected ENERGY near Pluck's (x1.5 left Strike
                    // noticeably QUIETER while the tooltip said "harder" - user report)
                    const float amp = (exc == 1) ? 2.4f : 1.0f;
                    float lp = 0.0f;
                    for (int i = 0; i < L; ++i) { lp += eb * (whiteNoise(rng) - lp); sv.ksBuf[base + i] = (i >= L - burst) ? lp * amp : 0.0f; }
                }
                // PLUCK POSITION: a DOUBLE full-depth feed-forward comb ((1 - z^-d)^2, d = position along
                // the string) + a position-following tone tilt (middle plucks ROUNDER), with RMS make-up
                // so only the CHARACTER moves. pos 0 = thin/twangy near the edge, pos 1 = hollow mid-string.
                if (pos > 0.01f)
                {
                    double preRms = 0.0; for (int i = 0; i < L; ++i) preRms += (double) sv.ksBuf[base + i] * sv.ksBuf[base + i];
                    const int d = juce::jlimit(1, L - 1, (int) std::round(pos * 0.5f * L));
                    for (int p2 = 0; p2 < 2; ++p2)
                        for (int i = L - 1; i >= d; --i) sv.ksBuf[base + i] -= sv.ksBuf[base + i - d];
                    float sm = 0.0f; const float k = 1.0f - 0.55f * pos;   // darker toward the middle
                    for (int i = 0; i < L; ++i) { sm += (sv.ksBuf[base + i] - sm) * k; sv.ksBuf[base + i] = sm; }
                    double postRms = 0.0; for (int i = 0; i < L; ++i) postRms += (double) sv.ksBuf[base + i] * sv.ksBuf[base + i];
                    if (postRms > 1.0e-12)
                    {
                        const float g = juce::jlimit(0.25f, 6.0f, (float) std::sqrt(preRms / postRms));
                        for (int i = 0; i < L; ++i) sv.ksBuf[base + i] = juce::jlimit(-2.5f, 2.5f, sv.ksBuf[base + i] * g);  // keep the KS write clamp
                    }
                }
                sv.ksWrite[strIdx] = (double) L;
            };

            for (int k = 0; k < nStr; ++k)
            {
                if (sl.scaleOn && sv.uniSemis[k] < -90.0f) continue;   // guitar voicing: missing string
                double uniMul = 1.0;
                if (sl.engine == SrcPhys && nStr > 1)
                {
                    const double sp = 2.0 * (double) k / (double) (nStr - 1) - 1.0;
                    const double interval = sl.scaleOn ? (double) sv.uniSemis[k] : 0.0;
                    uniMul = std::pow(2.0, interval / 12.0
                                          + sp * (double) (sl.oscDetune * 100.0f) / 1200.0);
                }
                exciteString(k, ksFreq0 * (float) uniMul);
            }
        }
    }
    // [2026-07-16] Per-note LEGATO: continue the phrase's envelope exactly like the live legato
    // modes do - the fresh voice starts at the inherited age; its gate end / voice life shift by
    // the same amount so the RELEASE still lands at this note's end (not instantly).
    if (legatoInherit > 0)
    {
        v.voiceSamples = legatoInherit;
        if (v.keyOff  >= 0) v.keyOff  += legatoInherit;
        if (v.gateLen >  0) v.gateLen += legatoInherit;
    }
    return vi;
}

// KEYS (on-screen keyboard): play `midiNote` on the selected channel's ELIGIBLE slots.
// MONO: whatever is ringing gets the 3 ms choke fade, then ONE fresh voice starts. Each eligible
// slot is re-tuned from its OWN base Freq to the pressed note (so both slots sound the same
// musical pitch regardless of their Freq knobs); slot 2 can be transposed DOWN by slot2Down
// semitones (sub-oscillator style). Ineligible slots (Sample/Noise/legacy) stay silent.
int DrumChannel::keyDown(int midiNote, float velocity, int slot2Down, bool poly, int slotMask, int midiChan)
{
    // [2026-07-16 round-2] TWO-AXIS PLAY MODEL (user design): the MODE only decides whether an
    // OVERLAPPING press RESTARTS the envelope (plain Poly/Mono) or CONTINUES it (Legato = the new
    // voice inherits the held voice's envelope position - a slow-attack pad swells ONCE across a
    // whole connected phrase). The GLIDE knob alone decides whether pitch slides between notes
    // (fingered portamento: only overlapping presses slide, in EVERY mode; knob 0 = no slide).
    // Detached presses always restart + never slide. Glide 0 + no held key = bit-identical old path.
    long  glideSamp = 0; float glideFrom = 0.0f;
    long legatoAge = -1;   // the envelope AGE to inherit (Legato modes only). voiceSamples IS the
                           // envelope clock - playHead is just the alive marker (a round-1 bug:
                           // inheriting playHead was a silent no-op, it never advances).
    int prevNote = -1;
    for (auto& vv : voices)
        if (vv.active() && vv.isKey && vv.keyOff < 0 && vv.keyNote >= 0)
        {
            if (prevNote < 0) prevNote = vv.keyNote;
            if (keysLegato && vv.voiceSamples > legatoAge) legatoAge = vv.voiceSamples;   // poly: the furthest-along envelope leads the phrase
            if (! poly) break;                          // mono: the one held voice IS the held note
        }
    if (prevNote >= 0 && keysGlide > 0.0001f)
    {
        // Poly slides from the LAST played note's pitch (the musical "previous note" in a chord
        // world); mono from the held note. Same knob = the slide time everywhere (up to 400 ms).
        const int from = (poly && lastKeyNote >= 0) ? lastKeyNote : prevNote;
        if (from != midiNote)
        {
            glideFrom = (float) (from - midiNote);                              // start this many semitones off target
            glideSamp = (long) (juce::jmax(0.005f, keysGlide * 0.4f) * (float) sr);
        }
    }
    if (! poly)
        fadeOutVoices(0.015f);                         // 15 ms handover (3 ms crackled on slides)
    // KEYBOARD = ABSOLUTE PITCH, KNOB UNTOUCHED (2026-07-08, user spec): the old block here
    // re-based every slot's Freq to C4 on key press - REMOVED. keySemis below already targets the
    // pressed note ABSOLUTELY (12*log2(target/base)), so the keyboard plays real notes no matter
    // where the Freq knob sits, and never rewrites it. Step recordings are stored as FRACTIONAL
    // offsets from the knob (processor stamp), so playback still reproduces the performance
    // exactly. TEST + step pitch 0 keep meaning "whatever the knob says" (the drum contract).
    const int vi = trigger(velocity, glideFrom, 0.0f, 0, /*glideTo*/ 0.0f, glideSamp, /*forceOverlap*/ true, slotMask);
    Voice& v = voices[vi];
    // [2026-07-16] LEGATO (both worlds): overlapping press = the envelope CONTINUES where the
    // held note's was (no re-attack). Phases stay fresh; mono's 15 ms handover masks the seam,
    // poly voices just start mid-envelope. A chord rolled from silence is safe: its notes land
    // ms apart, so inheriting a barely-started envelope ~= a fresh attack. Key voices have
    // keyOff = -1 and no gate, so only the age itself moves (no end-shifts needed).
    if (legatoAge > 0) v.voiceSamples = legatoAge;
    lastKeyNote = midiNote;   // poly glide source for the NEXT note
    v.isKey = true; v.keyOff = -1; v.keyNote = midiNote;   // tag: keyUp(note) releases only this note's voices
    v.keyChan = (int8_t) juce::jlimit(0, 16, midiChan);     // MPE: expression events find this voice by channel
    v.pressTgt = v.pressCur = v.slideTgt = v.slideCur = v.bendTgt = v.bendCur = 0.0f;
    double targetHz = 440.0 * std::pow(2.0, (double)(midiNote - 69) / 12.0);
    // PIANO-ROLL TUNE: a tuned bar's keys target the TUNED pitch, so live play always matches
    // what the recorded notes will play back (the user's never-differ rule survives the fader).
    if (drawMode && std::abs(drawTuneCents) > 0.01f)
        targetHz *= std::pow(2.0, (double) drawTuneCents / 1200.0);
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        SlotVoice& sv = v.sv[s];
        const Slot& sl = slots[s];
        double base = 0.0;
        switch (sl.engine)
        {
            case SrcOsc:   base = (float) slotBaseHz(s, sl);  break;   // Analog+FM
            case SrcGrain: base = (float) slotBaseHz(s, sl);  break;   // grains transpose via keySemis
            case SrcModal: base = (float) slotBaseHz(s, sl);  break;   // Modal's Freq lives on oscFreq too
            case SrcPhys:  base = (float) slotBaseHz(s, sl); break;
            case SrcSample:   // Sample plays on keys: Preserve pitch = its own pitch (keySemis 0);
                              // else varispeed relative to C3 (MIDI 60), slot 2 transposed.
                sv.keySemis = sl.smpPreservePitch ? 0.0f
                            : (float)(midiNote - 60 - (s == 1 ? slot2Down : 0));
                continue;
            case SrcNoise:    sv.keySemis = 0.0f; continue;   // noise is unpitched - just play it
            default: sv.keyMute = true; continue;             // legacy engines: not playable by keys
        }
        double hz = targetHz;
        if (s == 1 && slot2Down != 0) hz *= std::pow(2.0, -(double) slot2Down / 12.0);
        sv.keySemis = (float)(12.0 * std::log2(hz / juce::jmax(1.0, base)));
        // SCALE: trigger() computed uniSemis off the C3-rebased Freq; RE-compute from the pressed note
        // (slot 2 uses its transposed note) so the harmonizer voices the diatonic chord of what you played.
        if (sl.scaleOn)
        {
            const int effNote = midiNote - (s == 1 ? slot2Down : 0);
            for (int u = 0; u < UNI_MAX; ++u) sv.uniSemis[u] = 0.0f;
            const int nv = juce::jlimit(1, UNI_MAX, sl.scaleUnison);
            for (int u = 0; u < nv; ++u) sv.uniSemis[u] = (float) scaleSemis(sl.scaleType, sl.scaleKey, effNote, u);
        }
    }
    return vi;
}

// Release the held key: the voice's env falls with the slot RELEASE from wherever it is
// (see keyAdsr); Physical/Modal also drop their hold clamps back to the authored decay.
// keyUp(note) = POLY: release only that note's voices (others keep sounding); a stale up
// (note not held) touches nothing, which also makes the mono slide race-safe by tag.
void DrumChannel::keyUp(int midiNote)
{
    for (auto& v : voices)
        if (v.active() && v.isKey && v.keyOff < 0 && v.keyNote == midiNote) v.keyOff = v.voiceSamples;
}
void DrumChannel::keyUp()
{
    for (auto& v : voices)
        if (v.active() && v.isKey && v.keyOff < 0) v.keyOff = v.voiceSamples;
}

// ===================== MOD MATRIX (DSP) =========================================================
// The grid-knob mirror: the numeric engine params in slotParamsFor()'s ORDER (choice/stepped params
// occupy an index but return {nullptr} = not modulatable). Keep in sync with slotParamsFor().
DrumChannel::GridKnob DrumChannel::modGridKnob(int engine, int idx)
{
    using S = Slot;
    auto F  = [](float S::* f, float mn, float mx) { GridKnob g; g.field = f; g.mn = mn; g.mx = mx; return g; };
    const GridKnob none;   // field == nullptr
    switch (engine)
    {
        case SrcOsc:   switch (idx) { case 0: return F(&S::fmDepth,0,1); case 1: return F(&S::fmSpread,0,1);
                                      case 2: return F(&S::fmFeedback,0,1); } break;
        case SrcNoise: switch (idx) { case 0: return none;                          /* Type = choice */
                                      case 1: return F(&S::noiseCenter,100,16000); case 2: return F(&S::noiseWidth,0,1);
                                      case 3: return F(&S::noiseRes,0,1); case 4: return F(&S::noiseDrive,0,1);
                                      case 5: return F(&S::noiseCrackle,0,1); } break;
        case SrcFM:    switch (idx) { case 0: return F(&S::fmPitch,-24,24); case 1: return F(&S::fmSpread,0,1);
                                      case 2: return F(&S::fmDepth,0,1); case 3: return F(&S::fmSub,0,1);
                                      case 4: return F(&S::fmFeedback,0,1); } break;
        case SrcPhys:  switch (idx) { case 0: return none;   /* Base Freq: NOT modulatable (use Pitch) */  case 1: return none;   /* Material */
                                      case 2: return F(&S::physStiff,0,1); case 3: return none; } break;   /* Excite */
        case SrcGrain: switch (idx) { case 0: return F(&S::grainPos,0,1); case 1: return F(&S::grainSize,0,1);
                                      case 2: return F(&S::grainDens,0,1); case 3: return F(&S::grainSpray,0,1);
                                      case 4: return F(&S::grainPitch,0,1); } break;
        case SrcSample:switch (idx) { case 0: return F(&S::smpGain,0,4); case 1: return F(&S::smpCrush,0,1);
                                      case 2: return F(&S::smpStretch,0.25f,4); } break;
        case SrcModal: switch (idx) { case 0: return none;   /* Base Freq: NOT modulatable (use Pitch) */  case 1: return none;   /* Material */
                                      case 2: return F(&S::modalMorph,0,1); case 3: return F(&S::modalTone,0,1);
                                      case 4: return F(&S::modalStruct,0,1); } break;
        default: break;
    }
    return none;
}

// Sample the 6 sources once per block for slot s (block-rate = the config bake's rate). All reads come
// from the NEWEST active voice + the slot's per-block state, so a poly stack samples one voice's cues
// (mono-ish semantics, disclosed). Values: Vel/AmpEnv/Random/ModEnv in 0..1, Note/LFOs/ModLFO in -1..1.
void DrumChannel::computeModSources(int s, const Slot& sl, float* out, const Voice* nvIn) const
{
    for (int i = 0; i < MS_COUNT; ++i) out[i] = 0.0f;
    const Voice* nv = nvIn;   // PER-VOICE: sample THIS voice; nullptr = the newest active (block-rate base)
    if (nv == nullptr)
        for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;

    out[MSVel]    = nv != nullptr ? juce::jlimit(0.0f, 1.0f, nv->velGain) : 0.0f;
    { const float note = nv != nullptr ? (nv->keyNote >= 0 ? (float) nv->keyNote : 60.0f + nv->voicePitch) : 60.0f;
      out[MSNote] = juce::jlimit(-1.0f, 1.0f, (note - 60.0f) / 24.0f); }         // +-24 st -> +-1
    out[MSAmpEnv] = juce::jlimit(0.0f, 1.0f, slotFiltEnv[juce::jlimit(0, NUM_SLOTS - 1, s)]);   // prev-block amp level
    out[MSRandom] = nv != nullptr ? nv->sv[juce::jlimit(0, NUM_SLOTS - 1, s)].modRand : 0.5f;

    // Mod Env: a full A-H-D-S-R (same evaluator + gate as the amp env, so Sustain HOLDS while the note
    // is held and Release falls after it ends), read from the newest voice's age.
    if (nv != nullptr)
        out[MSModEnv] = juce::jlimit(0.0f, 1.0f, keyAdsr(nv->voiceSamples, nv->isKey ? nv->keyOff : nv->gateLen,
                                                          sl.modEnvA, sl.modEnvH, sl.modEnvD, sl.modEnvS, sl.modEnvR));

    // The per-slot LFOs, read at the newest voice's block-start phase (retrig) or the timeline-anchored
    // free phase. Value = the LFO Amount (depth) x shape; the matrix route amount scales it further.
    for (int d = 0; d < 4; ++d)
    {
        const float cpb = sl.lfoSync[d];   // GRID mode (-1) is GONE [2026-07-15 14:20] - old files migrate in readSlots
        const float hz = (cpb > 0.0f) ? juce::jlimit(0.005f, 40.0f, cpb / juce::jmax(0.05f, lfoBarSeconds))
                                      : juce::jlimit(0.05f, 30.0f, sl.lfoRate[d]);
        double ph; uint32_t cyc;
        if (sl.lfoFree[d])
        {
            const double sec = (lfoBarPos >= 0.0 ? lfoBarPos * (double) lfoBarSeconds : lfoFreeSec);
            ph  = 2.0 * kPi * (double) hz * sec;
            cyc = (uint32_t) juce::jmax(0.0, ph / (2.0 * kPi));
        }
        else if (nv != nullptr)
        { ph = nv->sv[juce::jlimit(0, NUM_SLOTS - 1, s)].lfoPhase[d]; cyc = nv->sv[juce::jlimit(0, NUM_SLOTS - 1, s)].lfoCyc[d]; }
        else { ph = 0.0; cyc = 0; }
        out[MSLfoFilt + d] = sl.lfoAmt[d] * lfoShapeVal(sl.lfoShape[d], ph, cyc, sl.lfoCurve[d]);   // Amount = the LFO's depth
    }

    // Mod LFO: matrix-created free-run LFO, timeline-anchored so playback passes match.
    {
        const double sec = (lfoBarPos >= 0.0 ? lfoBarPos * (double) lfoBarSeconds : lfoFreeSec);
        const double ph  = 2.0 * kPi * (double) juce::jlimit(0.05f, 20.0f, sl.modLfoRate) * sec;
        out[MSModLfo] = lfoShapeVal(juce::jlimit(0, 6, sl.modLfoShape), ph, (uint32_t) juce::jmax(0.0, ph / (2.0 * kPi)));
    }

    // STEP MOD lanes A / B: the value drawn on the currently-playing step (0..1). modStepPos is set
    // by the Sequencer each block; channel-level (both slots read the same lane). Stepped read for now.
    if (modStepPos >= 0.0f && numSteps > 0)
    {
        const int st = juce::jlimit(0, numSteps - 1, (int) modStepPos);
        out[MSStepModA] = juce::jlimit(0.0f, 1.0f, stepModA[st]);
        out[MSStepModB] = juce::jlimit(0.0f, 1.0f, stepModB[st]);
    }
    out[MSModWheel] = juce::jlimit(0.0f, 1.0f, modWheel);   // live MIDI mod wheel (CC1)
    out[MSPitchWheel] = juce::jlimit(-1.0f, 1.0f, pitchWheel);   // [2026-07-14 03:00] bipolar wheel
    if (nv != nullptr) { out[MSPressure] = nv->pressCur; out[MSSlide] = nv->slideCur; }   // MPE (per-voice)
}

// Apply the 6 routes onto a scratch Slot before the config bake. Cutoffs + pitch are MULTIPLICATIVE
// (octaves); everything else is ADDITIVE over the target's native range, then clamped.
void DrumChannel::applyModMatrix(Slot& tmp, const float* srcVals, SlotVoice* latch, const float* laggedRoute) const
{
    for (int ri2 = 0; ri2 < MOD_ROUTES; ++ri2)
    {
        const auto& r = tmp.mod[ri2];
        if (r.tgt == MTOff || r.src == MSOff || std::abs(r.amt) < 1.0e-4f) continue;
        const float srcV = laggedRoute != nullptr ? laggedRoute[ri2] : srcVals[juce::jlimit(0, MS_COUNT - 1, (int) r.src)];
        const float m = r.amt * modRouteShape(r, srcV);   // depth * (lagged + remapped) source
        auto add = [&](float& f, float mn, float mx) { f = juce::jlimit(mn, mx, f + m * (mx - mn)); };
        switch (r.tgt)
        {
            // [2026-07-13 19:57] HOT (audio-rate) targets: applied PER SAMPLE in the render from ANY
            // source - the block apply must skip them or they'd be applied twice. (applyHotBlock
            // re-applies them onto the display copy so the UI mod rings keep moving.)
            case MTFilt1Cut: case MTFilt2Cut: case MTFilt1Res: case MTFilt2Res:
            case MTDrive: break;
            case MTRevSend: case MTDelSend: break;   // CHANNEL sends now: accumulated separately (channelMod)
            case MTChFilt1Cut: case MTChFilt1Res: case MTChFilt2Cut: case MTChFilt2Res: break;   // CHANNEL filter pair: accumulated separately
            case MTChFxAAmt: case MTChFxBAmt: break;   // CHANNEL FX slots: accumulated separately (channelMod), not on the slot copy
            case MTTone:     break;                  // RETIRED (Tone removed; the Bell filter covers it) - old routes are inert
            case MTPunch: case MTSub: break;   // HOT (per-sample)
            case MTFormant:  add(tmp.fxFormant, 0, 1); break;   // sweep = the slot TALKS
            case MTAtk: case MTDec: case MTSus: case MTRel: break;   // env-TIME targets: LATCHED per note below, never live per block
            case MTPitch: case MTWavePos: break;   // HOT (per-sample; pitch from ANY source is real FM now)
            case MTDetune:   add(tmp.oscDetune, 0, 1); break;
            case MTVibrato:  add(tmp.vibrato, 0, 1); break;
            case MTWidth:    add(tmp.uniSpread, 0, 1); break;
            case MTDrift:    add(tmp.drift, 0, 1); break;
            case MTVol: case MTWarp: break;   // HOT (per-sample AM / wavefold)
            case MTChFxAChr: case MTChFxBChr: case MTChFxCAmt: case MTChFxCChr: break;   // CHANNEL FX C + Characters: accumulated separately (channelMod)
            case MTRing: case MTRingHz: case MTSlotPan: break;   // HOT (per-sample)
            // [2026-07-14 00:30] filter ENV AMOUNTS + UNISON COUNT: block-rate (they feed the bake,
            // per voice) - env-follow is itself block-rate, and a voice COUNT is inherently stepped.
            case MTFilt1Env: add(tmp.filterEnvAmt,  -1, 1); break;
            case MTFilt2Env: add(tmp.filterEnvAmt2, -1, 1); break;
            case MTUniCount: tmp.oscUnison = juce::jlimit(1, 16, tmp.oscUnison + juce::roundToInt(m * 15.0f)); break;   // [2026-07-14 02:50] continuous again - per-voice FADES bridge the steps (Osc only)
            default:
                if (r.tgt >= MT_GRID_BASE && r.tgt < MT_GRID_END)   // engine GRID knobs only (Sub/Formant sit above)
                {
                    const GridKnob gk = modGridKnob(tmp.engine, r.tgt - MT_GRID_BASE);
                    // fmDepth = the one grid param consumed per sample (hot id 12) - skip it here.
                    if (gk.field != nullptr && gk.field != &Slot::fmDepth)
                    { float& f = tmp.*(gk.field); f = juce::jlimit(gk.mn, gk.mx, f + m * (gk.mx - gk.mn)); }
                }
                break;
        }
    }
    // ENVELOPE-TIME targets (Attack/Decay/Sustain/Release) are LATCHED ONCE PER NOTE: our envelopes
    // are stateless env(t, params), so re-baking them per block while a note runs makes the level
    // JUMP at every block edge = crackle on any fast source (user bug, 2026-07-13). Latch-at-the-hit
    // = clean per-hit variation (the Serum/Elektron semantic: a fast LFO scatters the hits, it does
    // not wobble a running envelope - use the Volume target for continuous level movement).
    // latch == nullptr = the block bake: env stays base (the render reads the per-voice config).
    if (latch == nullptr) return;
    if (! latch->envModLatched)
    {
        latch->envModLatched = true;
        for (int k = 0; k < 4; ++k) latch->envModOfs[k] = 0.0f;
        for (int ri2 = 0; ri2 < MOD_ROUTES; ++ri2)
        {
            const auto& r = tmp.mod[ri2];
            if (r.src == MSOff || std::abs(r.amt) < 1.0e-4f) continue;
            const float srcV = laggedRoute != nullptr ? laggedRoute[ri2] : srcVals[juce::jlimit(0, MS_COUNT - 1, (int) r.src)];
            const float m = r.amt * modRouteShape(r, srcV);
            switch (r.tgt) { case MTAtk: latch->envModOfs[0] += m; break; case MTDec: latch->envModOfs[1] += m; break;
                             case MTSus: latch->envModOfs[2] += m; break; case MTRel: latch->envModOfs[3] += m; break;
                             default: break; }
        }
    }
    tmp.atk     = juce::jlimit(0.0f, 6.0f, tmp.atk     + latch->envModOfs[0] * 1.0f);
    tmp.dec     = juce::jlimit(0.0f, 6.0f, tmp.dec     + latch->envModOfs[1] * 2.0f);
    tmp.sustain = juce::jlimit(0.0f, 1.0f, tmp.sustain + latch->envModOfs[2] * 1.0f);
    tmp.release = juce::jlimit(0.0f, 4.0f, tmp.release + latch->envModOfs[3] * 2.0f);

}

#include "Adaa.h"   // [2026-07-13 22:10] ADAA shapers moved to their own module (user: group the code better)

// [2026-07-13 19:57] BLOCK snapshot of the HOT (audio-rate) targets, applied onto the DISPLAY copy
// only (the sc[] bake + the UI mod rings). The real render applies these per sample; without this the
// rings would freeze at the base values. Pitch/Vol have no rings and are skipped (as before).
void DrumChannel::applyHotBlock(Slot& tmp, const float* srcVals, const float* laggedRoute) const
{
    for (int ri2 = 0; ri2 < MOD_ROUTES; ++ri2)
    {
        const auto& r = tmp.mod[ri2];
        if (r.tgt == MTOff || r.src == MSOff || std::abs(r.amt) < 1.0e-4f) continue;
        const float srcV = laggedRoute != nullptr ? laggedRoute[ri2] : srcVals[juce::jlimit(0, MS_COUNT - 1, (int) r.src)];
        const float m = r.amt * modRouteShape(r, srcV);
        auto add = [&](float& f, float mn, float mx) { f = juce::jlimit(mn, mx, f + m * (mx - mn)); };
        switch (r.tgt)
        {
            case MTFilt1Cut: tmp.filterCutoff  = juce::jlimit(20.0f, 20000.0f, tmp.filterCutoff  * std::pow(2.0f, m * 4.0f)); break;
            case MTFilt2Cut: tmp.filterCutoff2 = juce::jlimit(20.0f, 20000.0f, tmp.filterCutoff2 * std::pow(2.0f, m * 4.0f)); break;
            case MTFilt1Res: tmp.filterReso  = juce::jlimit(0.4f, 12.0f, tmp.filterReso  + m * 11.6f); break;
            case MTFilt2Res: tmp.filterReso2 = juce::jlimit(0.4f, 12.0f, tmp.filterReso2 + m * 11.6f); break;
            case MTDrive:    add(tmp.fxDrive, 0, 1); break;
            case MTRing:     add(tmp.fxRing, 0, 1); break;
            case MTSub:      add(tmp.fxSub, 0, 1); break;
            case MTPunch:    add(tmp.fxPunch, -1, 1); break;
            case MTWarp:     add(tmp.oscWarp, 0, 1); break;
            case MTWavePos:  tmp.addPos = juce::jlimit(0.0f, 1.0f, tmp.addPos + m); break;
            case MTRingHz:   tmp.fxRingHz = juce::jlimit(25.0f, 4000.0f, tmp.fxRingHz * std::pow(2.0f, m * 3.0f)); break;
            case MTSlotPan:  add(tmp.pan, -1, 1); break;
            case MTUniCount: tmp.oscUnison = juce::jlimit(1, 16, tmp.oscUnison + juce::roundToInt(m * 15.0f)); break;   // display: what a hit NOW would get
            default:
                if (r.tgt >= MT_GRID_BASE && r.tgt < MT_GRID_END)
                {
                    const GridKnob gk = modGridKnob(tmp.engine, r.tgt - MT_GRID_BASE);
                    if (gk.field == &Slot::fmDepth)
                    { float& f = tmp.*(gk.field); f = juce::jlimit(gk.mn, gk.mx, f + m * (gk.mx - gk.mn)); }
                }
                break;
        }
    }
}

void DrumChannel::renderInto(juce::AudioBuffer<float>& dest, int startSample, int numSamples, bool anySolo,
                             juce::AudioBuffer<float>* reverbSendBus, juce::AudioBuffer<float>* delaySendBus,
                             juce::AudioBuffer<float>* reverbSendBusB, juce::AudioBuffer<float>* delaySendBusB)
{
    // Keep the analyser fed every block (silence between hits included) so its
    // FFT windows stay aligned to the real timeline. Without this, the silent
    // gaps get skipped and the window's phase against each hit drifts randomly,
    // making the spectrum of one repeated sound look different every time.
    // NOTE: do NOT zero meterPeak here - the peak-hold contract is "audio thread only
    // ever raises it, the UI resets it on read". Storing 0 on the first silent block
    // wiped a short one-shot's peak ~6 ms after it ended, long before the ~42 ms UI
    // tick could read it, so rims/claves randomly never registered on the meter.
    auto feedSilence = [this, numSamples]() {
        if (analysisTap != nullptr)
            for (int i = 0; i < numSamples; ++i) analysisTap->push(0.0f);
    };

    if (mute || (anySolo && !solo))
    {
        // [2026-07-15 23:00] KILL any live voices while gated: the output is already silent, so
        // this is click-free - and it guarantees nothing "from the past" survives to play on
        // unmute (belt + braces with the fireEvent skip; also covers voices started BEFORE the
        // mute was toggled, and live-keys voices).
        for (auto& v : voices) v.playHead = -1.0;
        feedSilence(); return;
    }

    // Don't block the audio thread: if a sample swap is in progress, skip this block.
    const juce::ScopedTryLock stl(sampleLock);
    if (!stl.isLocked()) { feedSilence(); return; }

    bool anyActive = false;
    for (auto& v : voices) if (v.active()) { anyActive = true; break; }
    if (!anyActive) { feedSilence(); return; }

    // ===================== SLOT-BASED RENDERING =====================
    // slots[] are now authoritative (the UI edits them directly). They are filled
    // from the legacy fields only on load, via buildSlotsFromLegacy().

    // Block-rate modulation: Drift (slow weight wander) + Vibrato (pitch LFO).
    vibPhase += 2.0f * kPi * 5.5f * (float) numSamples / (float) sr;       // ~5.5 Hz
    if (vibPhase > 2.0f * kPi) vibPhase -= 2.0f * kPi;
    const float vibLfo = std::sin(vibPhase);


    // Sample buffers are PER-SLOT now (slotSample[]); each slot computes its own region in the SC loop.
    const int  destChannels    = dest.getNumChannels();

    // ---- Per-slot render constants (engine specific), computed once -----------
    struct SC {
        int    engine = -1; float weight = 0, atk = 0, hold = 0, dec = 0, sustain = 0, release = 0.06f;
        int    oscShape = 0, oscShapeB = 0; double oscFreq = 0; float oscPEnvAmt = 0, oscPEnvTime = 0.04f, oscPOffset = 0;
        int    uniVoices = 1; float uniCents = 0, uniGain = 1, oscVibFac = 1; bool uniCenter = false; int uniMode = 0;
        bool   scaleOn = false;   // SCALE (diatonic harmonizer): read per-voice sv.uniSemis[] instead of the fixed chord table
        double uniMul[KS_UNI] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };   // per-string pitch multipliers (Physical unison/chord)
        int    noiseType = 0; bool noiseBP = false; double noiseFc = 1000, nQ = 0.7; float noiseDrive = 0, noiseCrackle = 0;
        float  greyB0 = 1, greyB1 = 0, greyB2 = 0, greyA1 = 0, greyA2 = 0;   // grey = white through a mid-scoop peaking biquad
        double fmCarrierF = 220, fmModF = 220; float fmIndex = 0, fmPEnvAmt = 0, fmPEnvTime = 0.05f, fmPOffset = 0, fmFeedback = 0, fmSub = 0;
        float  fmRatio = 1.0f;   // Analog+FM merge: modulator freq = carrier freq * fmRatio (per-sample, tracks vibrato/pitch)
        bool   fmEnvF = false;   // FM Amount follows the amp envelope (bright attack -> mellow decay)
        const PhysModel* pm = &kPhysModels[0]; float ksFb = 0, ksLpC = 0.5f, ksApC = 0; int ksApN = 0;
        float ksApComp = 0;   // loop-length compensation for BOTH allpass chains' DC delay (keeps tuning)
        // GRANULAR config: the source (table or the slot's sample), mapped params, spawn rate.
        const float* grTbl = nullptr; int grTblLen = 0;
        const juce::AudioBuffer<float>* grBuf = nullptr; int grLo = 0, grHi = 0;
        float grPos = 0, grSpray = 0, grPitch = 0; int grLenSamp = 2400;
        float grSpawnPerSamp = 0; double grIncBase = 1.0; float grNorm = 1.0f;
        float ksApC2 = 0; int ksApN2 = 0;   // the MATERIAL's dispersion chain - STACKS with the user
                                            // stiffness chain (it used to be REPLACED by it)
        double physBaseF = 110; float physPEnvAmt = 0, physPEnvTime = 0.05f, physPOffset = 0, physVibFac = 1;
        float  crushStep = 0; double speed = 1; float smpPitch = 0, smpPEnvAmt = 0, smpPEnvTime = 0.04f, smpPOffset = 0; bool reverse = false;
        bool   smpEnv = false;   // opt-in amp envelope on the sample (off = legacy full-length playback)
        bool   smpPreserve = true;   // Sample: ignore step/draw/key/env pitch (play at the sample's own pitch)
        const juce::AudioBuffer<float>* buf = nullptr; int srcLen = 0, regLo = 0, regHi = 0, slices = 1;  // per-slot sample
        float  smpGain = 1.0f;   // sample output boost
        // -- section levels + fold (legacy unified-engine extras) --
        float  oscFold = 0, oscLevel = 1, noiseLevel = 0;
        float  oscWarp = 0.0f;                      // wave WARP = one-way wavefold (0 = off)
        int    fxDriveType = 0; float fxDrive = 0;  // per-slot Drive (insert; sends are CHANNEL-level now)
        int    waveTable = 0; float wavePos = 0;   // wavetable (SrcWave)
        int    modalN = 0; float modalA1[MODAL_MODES] = {}, modalA2[MODAL_MODES] = {}, modalGain[MODAL_MODES] = {};  // modal bank
        float  modalDecaySec = 0.5f;   // base ring length (for voiceEnd)
        // -- 4-point pitch envelope (applies on top of the legacy per-engine env) --
        bool   pEnvOn = false; float pEnvP[Slot::NPE] = { 0, 0, 0, 0 }, pEnvT[Slot::NPE] = { 0.2f, 0.4f, 0.6f, 0.8f };
        double voiceLenSamp = 1.0;   // sound length in samples, for the time-fraction axis
        // === PER-SLOT FILTER (begin) - resonant LP; raw params here, coeffs recomputed per BLOCK
        //     (env-follow) into 'filt' just before the voice loop; state lives per-voice ===
        float  uniSpread = 0.0f; float uniPanL[UNI_MAX + 1] = {}, uniPanR[UNI_MAX + 1] = {};   // stereo WIDTH per voice (equal power)
        // TWO independent resonant filters per slot, applied IN SERIES (filt[0] then filt[1]) - e.g.
        // High-Pass + Low-Pass = a band you shape by hand. Each: LP/HP/BP/Notch + cutoff/reso/env +
        // keytrack. Raw params here; coeffs recomputed per BLOCK (env-follow + LFO); state per-voice.
        struct FiltCfg {
            bool   on = false; int mode = 0;              // on + which SVF output (0 LP / 1 HP / 2 BP / 3 Notch / 4 Bell)
            double cutoff = 1000; float reso = 0.707f, envAmt = 0.0f;
            double cutoffHz = 1000, G = 0.1, K = 1.414;    // block coeffs (env+LFO); per-voice keytrack re-tans cutoffHz
            double bellM1 = 0.0;                           // Bell only: out = in + bellM1*v1 (K carries 1/(Q*A))
            float  resoB = 0.707f; double bellA = 1.0;     // [2026-07-13 19:57] base reso + bell A: per-sample RESO mod recomputes K from these
        } filt[2];
        const float* wtFrm[ADD_FRAMES] = {};  // ADDITIVE WAVETABLE: the slot's 4 baked frame tables (null = not Custom)
        float wtPos = 0.0f;                   // static position 0..1 (addPos)
        float wtPosOfs = 0.0f;                // MOD MATRIX wave-position offset (added in the render so it works even with glide)
        bool  wtGlide = false;                // per-note glide on (addSeg[0] > 0) - overrides wtPos
        bool  wtLoop  = false;                // ping-pong the glide forever (out and back)
        float wtLoopEnd = 1.0f;               // journey end time (samples) = travel to the first hold
        // piecewise position clock (samples): leg k runs [wtT(k-1), wtTk) at slope wtInvk; a HOLD
        // leg has boundary 1e18 + slope 0, so the position parks at that leg's left frame.
        float wtT1 = 0.0f, wtT2 = 0.0f, wtT3 = 0.0f;
        float wtInv0 = 0.0f, wtInv1 = 0.0f, wtInv2 = 0.0f;
        float  subAmt = 0.0f; double subHz = 0.0;     // SUB: half the slot's base pitch (0 = unpitched engine = inert)
        float  panL = 1.0f, panR = 1.0f;              // static SLOT PAN gains (equal-power, unity at centre)
        float  panBase = 0.0f;                        // the raw pan value (-1..1) for per-sample matrix recompute
        float  fmtMix = 0.0f; Biquad fmtBq[2];        // FORMANT: two vowel band-passes + wet mix
        float  punch = 0.0f;   // PUNCH transient shaper (-1 soften .. +1 punch)
        float  ring = 0.0f;   // RING amount (per-voice mod); Chorus/Flanger/Phaser/Comp are CHANNEL FX now (not per-slot)
        double ringHz = 200.0; bool ringTrack = false; double ringBase = 0.0;   // carrier: fixed Hz OR note-tracked (base = slot pitch)
        // =========================================================================================
        // AUDIO-RATE MOD CORE [2026-07-13 19:57] - the COMPILED per-sample route program. Every
        // matrix route whose target is "hot" (pitch, volume, filter cutoff/reso, drive, ring, sub,
        // punch, warp, wave position, FM amount) is applied PER SAMPLE in the render, from ANY
        // source - the sources themselves are per-sample signals (LFO phases, live envelopes,
        // per-note constants, block-linear ramps for the steppy ones). This SUPERSEDES both the
        // 2026-07-13-morning de-zipper bank (smoothing is pointless when the source is smooth) and
        // the LFO-only arP/arV special case (generalised). arN == 0 = zero cost = byte-identical.
        // Hot ids: 0 Pitch 1 Vol 2 F1Cut 3 F2Cut 4 F1Res 5 F2Res 6 Drive 7 Ring 8 Sub 9 Punch
        //          10 Warp 11 WavePos 12 FmIdx(grid fmDepth).
        // =========================================================================================
        int8_t arSrc[MOD_ROUTES] = {}; int8_t arTgt[MOD_ROUTES] = {}; float arAmt[MOD_ROUTES] = {}; int arN = 0;
        const uint8_t* arCv[MOD_ROUTES] = {};   // [2026-07-14 00:03] per-route REMAP curve (null = pass-through); points into slots[] (stable), never the bake copy
        uint8_t arBi[MOD_ROUTES] = {};          // source is bipolar -> curve X maps -1..+1
        float   arLagK[MOD_ROUTES] = {};        // [2026-07-14 01:33] per-sample LAG coefficient (0 = instant)
        uint32_t arMask = 0;                                  // bit per ModSrc that needs a per-sample value
        float  mEA = 0.005f, mEH = 0, mED = 0.3f, mES = 0, mER = 0;   // Mod Env params (per-sample source eval)
        bool   arDrvOn = false;                               // a route targets DRIVE -> the dry-blend ease-in engages
        bool   uniCntMod = false;                             // [2026-07-14 02:50] a route targets UNISON COUNT (Osc) -> per-voice fades engage
        double modLfoPh0 = 0, modLfoInc = 0; int8_t mLfoShape = 0;    // Mod LFO: block-start phase + per-sample inc
        float  lfoKeyRatio[4] = {};         // > 0 = KEY mode: LFO rate = the voice's pitch x ratio (per-sample inc)
        double lfoKeyBase = 0.0;            // the slot's base Hz for KEY-mode LFOs (x noteMul*pe3Mul per sample)
        // === PER-SLOT FILTER (end) ===
        // Per-slot LFOs (3 independent sines, restart each hit). Index: 0 filter cutoff / 1 pitch / 2 volume.
        float  lfoRate[4] = { 4.0f, 4.0f, 4.0f, 4.0f }, lfoAmt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        int    lfoShape[4] = {};            // 0 sine .. 7 custom
        const float* lfoCurve[4] = {};      // shape 7: the slot's drawn cycle
        bool   lfoFreeOn[4] = {};           // FREE-RUN: use the timeline-anchored channel phase
        double lfoFreePh[4] = {}, lfoFreeInc[4] = {};   // block-start phase + per-sample increment
        float  drift = 0.0f;                // slot DRIFT amount (wander depth per block)
        float  filtDrive = 0.0f;            // filter loop saturation 0..1 (0 = clean = bit-identical)
        bool   lfoSrcUsed[4] = {};          // MOD MATRIX: this LFO feeds a route as a SOURCE -> keep its phase advancing
    } sc[NUM_SLOTS];

    bool anySlotActive = false;
    int  domSlot = -1; float domW = -1.0f;
        // PER-VOICE-CAPABLE per-slot config bake (pure: writes only `c` from `sl` + block state).
        auto bakeSlot = [&](int s, const Slot& sl, SC& c)
        {
        c.engine = sl.engine; c.weight = sl.weight;
        c.fxDriveType = sl.fxDriveType; c.fxDrive = sl.fxDrive;   // per-slot FX (sends are CHANNEL-level now)
        c.lfoKeyBase = 0.0;
        for (int d2 = 0; d2 < 4; ++d2) {   // per-slot LFOs (free-Hz / tempo-synced / KEY-tracked)
            // lfoSync: 0 = OFF (free Hz), > 0 = cycles per bar, -2 = KEY (rate follows the played
            // PITCH x ratio; lfoRate = the ratio - the audio-rate FM mode). GRID (-1) was DELETED
            // [2026-07-15 14:20] (user: bar-sync covers it, grid just locked the number) - old
            // files migrate to the equivalent bar value in readSlots.
            c.lfoKeyRatio[d2] = 0.0f;
            float cpb = sl.lfoSync[d2];
            if (cpb <= -1.5f)                                   // KEY mode: per-voice, per-sample rate
            {
                c.lfoKeyRatio[d2] = juce::jlimit(0.05f, 16.0f, sl.lfoRate[d2]);
                if (c.lfoKeyBase <= 0.0) c.lfoKeyBase = slotBaseHz(s, sl);
                c.lfoRate[d2] = juce::jlimit(0.05f, 2000.0f, (float)(c.lfoKeyBase * (double) c.lfoKeyRatio[d2]));  // nominal (UI dot)
            }
            else
            {
                c.lfoRate[d2] = (cpb > 0.0f)
                    ? juce::jlimit(0.005f, 40.0f, cpb / juce::jmax(0.05f, lfoBarSeconds))          // cycles/bar -> Hz
                    : juce::jlimit(0.05f, 2000.0f, sl.lfoRate[d2]);   // free Hz - now reaches AUDIO rates (FM/AM)
            }
            c.lfoAmt[d2]  = juce::jlimit(0.0f, 1.0f, sl.lfoAmt[d2]);
            c.lfoShape[d2]  = juce::jlimit(0, 7, sl.lfoShape[d2]);
            c.lfoCurve[d2]  = sl.lfoCurve[d2];
            c.lfoFreeOn[d2] = sl.lfoFree[d2] && c.lfoKeyRatio[d2] <= 0.0f;   // KEY implies per-voice phase
            if (c.lfoFreeOn[d2])
            {   // FREE-RUN: anchor the phase to the TIMELINE (bars into the playing unit) so every
                // playback pass is identical; when stopped, a free channel clock keeps it moving live.
                const double cyclesNow = (lfoBarPos >= 0.0 ? lfoBarPos * (double) lfoBarSeconds
                                                           : lfoFreeSec) * (double) c.lfoRate[d2];
                c.lfoFreePh[d2]  = 2.0 * kPi * cyclesNow;
                c.lfoFreeInc[d2] = 2.0 * kPi * (double) c.lfoRate[d2] / sr;
            } }
        // AUDIO-RATE ROUTE COMPILER [2026-07-13 19:57]: every route with a HOT target compiles into
        // the per-sample program (applyModMatrix skips those targets; the render evaluates them per
        // sample from any source). Also bakes the per-sample source constants (Mod Env / Mod LFO).
        c.arN = 0; c.arMask = 0; c.arDrvOn = false; c.uniCntMod = false;
        {
            auto hotIdx = [&](int tgt) -> int {
                switch (tgt) {
                    case MTPitch: return 0;    case MTVol: return 1;
                    case MTFilt1Cut: return 2; case MTFilt2Cut: return 3;
                    case MTFilt1Res: return 4; case MTFilt2Res: return 5;
                    case MTDrive: return 6;    case MTRing: return 7;    case MTSub: return 8;
                    case MTPunch: return 9;    case MTWarp: return 10;   case MTWavePos: return 11;
                    case MTRingHz: return 13;  case MTSlotPan: return 14;   // [2026-07-13 22:45]
                    default: break;
                }
                if (tgt >= MT_GRID_BASE && tgt < MT_GRID_END
                    && modGridKnob(sl.engine, tgt - MT_GRID_BASE).field == &Slot::fmDepth)
                    return 12;   // grid "FM Amount" = the one grid param consumed per sample (fmIndex)
                return -1;
            };
            int ri = -1;
            for (auto& r : sl.mod)
            {
                ++ri;
                if (r.src == MSOff || r.tgt == MTOff || std::abs(r.amt) < 1.0e-4f) continue;
                const int h = hotIdx(r.tgt); if (h < 0) continue;
                if (c.arN < MOD_ROUTES)
                { c.arTgt[c.arN] = (int8_t) h; c.arSrc[c.arN] = r.src; c.arAmt[c.arN] = r.amt;
                  // REMAP pointer must reference the PERSISTENT slot (the bake copy dies with the block)
                  c.arCv[c.arN] = (r.curveOn && ri >= 0 && ri < MOD_ROUTES) ? slots[s].mod[ri].curve : nullptr;
                  c.arBi[c.arN] = (uint8_t)(modSrcBipolar(r.src) ? 1 : 0);
                  c.arLagK[c.arN] = r.lagMs > 0.01f ? (float)(1.0 - std::exp(-1.0 / (sr * (double) r.lagMs * 0.001))) : 0.0f;
                  ++c.arN; }
                if (h == 6) c.arDrvOn = true;   // drive routed -> ease the shaper in near zero
                c.arMask |= (1u << juce::jmin(31, (int) r.src));
                if (r.src >= MSLfoFilt && r.src <= MSLfoWave) c.lfoSrcUsed[r.src - MSLfoFilt] = true;
            }
            for (auto& r : sl.mod)   // continuous COUNT-MORPH flag (block target; fades bridge it)
            {
                if (r.src != MSOff && r.tgt == MTUniCount && std::abs(r.amt) > 1.0e-4f && sl.engine == SrcOsc)
                { c.uniCntMod = true; break; }
            }
            c.mEA = sl.modEnvA; c.mEH = sl.modEnvH; c.mED = sl.modEnvD; c.mES = sl.modEnvS; c.mER = sl.modEnvR;
            const double mlSec = (lfoBarPos >= 0.0 ? lfoBarPos * (double) lfoBarSeconds : lfoFreeSec);
            const double mlHz  = (double) juce::jlimit(0.05f, 20.0f, sl.modLfoRate);
            c.modLfoPh0 = 2.0 * kPi * mlHz * mlSec; c.modLfoInc = 2.0 * kPi * mlHz / sr;
            c.mLfoShape = (int8_t) juce::jlimit(0, 6, (int) sl.modLfoShape);
        }
        c.drift     = juce::jlimit(0.0f, 1.0f, sl.drift);
        c.filtDrive = juce::jlimit(0.0f, 1.0f, sl.filterDrive);
        c.atk = sl.atk; c.hold = sl.hold; c.dec = sl.dec; c.sustain = sl.sustain; c.release = sl.release;
        // Release is FAITHFUL now (it has its own handle in the Strike/Ring editor for Phys/Modal
        // too): whatever the user sets IS the key-up fade. No hidden floor - that was overriding
        // the visible value. Factory sounds still author their own release.
        // 4-point pitch envelope + the sound length its time-axis is measured against
        // (mirror the amp-envelope length the UI shows as "ENVELOPE ~X s").
        c.pEnvOn = sl.pEnvOn();
        for (int k = 0; k < Slot::NPE; ++k) { c.pEnvP[k] = sl.pEnvP[k]; c.pEnvT[k] = sl.pEnvT[k]; }
        // Pitch-env time base = the AHD perceptual length the UI shows (atk+hold+dec); the voice itself still
        // renders its full exp tail (voiceEnd uses 3.2*dec) so nothing is cut - this just aligns the pitch timeline.
        c.voiceLenSamp = juce::jmax(1.0, (double)((sl.atk + sl.hold + sl.dec) * (float) sr));
        switch (sl.engine)
        {
            case SrcOsc:
                c.oscShape = sl.oscShape; c.oscShapeB = sl.oscShapeB; c.oscFreq = (float) slotBaseHz(s, sl);
                c.oscPEnvAmt = sl.oscPEnvAmt; c.oscPEnvTime = sl.oscPEnvTime; c.oscPOffset = sl.oscPOffset;
                c.scaleOn   = sl.scaleOn;
                c.uniVoices = juce::jlimit(1, UNI_MAX, sl.scaleOn ? (sl.scaleType >= 10 ? 6 : sl.scaleUnison) : sl.oscUnison);
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;   // up to +/-100 cents (1 semitone) spread
                c.uniCenter = sl.oscUniCenter; c.uniMode = sl.oscDetuneMode;
                c.uniGain   = 1.0f / std::sqrt((float) (c.uniVoices + (c.uniCenter ? 1 : 0)));
                c.oscVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.09f * vibLfo;
                // Merged FM section (Depth 0 = pure analog). Modulator = sine at carrier*ratio.
                c.oscWarp    = sl.oscWarp;
                c.fmRatio    = 1.0f + (float) sl.fmSpread * 5.0f;
                c.fmIndex    = sl.fmDepth * 12.0f;
                c.uniSpread  = juce::jlimit(0.0f, 1.0f, sl.uniSpread);
                for (int u = 0; u <= UNI_MAX; ++u)   // per-voice equal-power pan gains, ALTERNATING pairs
                {
                    float sp = 0.0f; int pairIdx = 0;
                    if (u < c.uniVoices && c.uniVoices > 1)
                    {
                        const float fr = (float) u / (float)(c.uniVoices - 1);
                        sp = (c.uniMode == 1) ? fr : (c.uniMode == 2) ? -fr : (2.0f * fr - 1.0f);
                        pairIdx = juce::jmin(u, c.uniVoices - 1 - u);   // 0 = the outermost +/- pair
                    }
                    // Serum-style: each successive +/- pair lands on the OPPOSITE side, so both ears
                    // get sharp AND flat voices (a straight sp->pan map leans the whole image sharp-right).
                    const float sgn = (pairIdx & 1) ? -1.0f : 1.0f;
                    const float p = juce::jlimit(-1.0f, 1.0f, sgn * sp * c.uniSpread);
                    c.uniPanL[u] = std::sqrt(0.5f * (1.0f - p));
                    c.uniPanR[u] = std::sqrt(0.5f * (1.0f + p));
                }
                c.fmFeedback = sl.fmFeedback; c.fmSub = sl.fmSub;
                c.fmEnvF     = sl.fmEnvFollow;
                break;
            case SrcNoise:
                c.noiseType = sl.noiseType;
                // Resonance sharpens the band-pass (pitched/whistling noise) + forces the filter on.
                c.noiseBP   = (sl.noiseWidth > 0.02f) || (sl.noiseRes > 0.02f);
                c.nQ        = 0.5 + (double)(sl.noiseWidth * sl.noiseWidth) * 15.0
                                  + (double)(sl.noiseRes * sl.noiseRes) * 45.0;
                c.noiseFc   = juce::jlimit(40.0, sr * 0.45, (double) sl.noiseCenter);
                c.noiseDrive = juce::jlimit(0.0f, 1.0f, sl.noiseDrive);
                c.noiseCrackle = juce::jlimit(0.0f, 1.0f, sl.noiseCrackle);
                {   // GREY = white through a peaking DIP at ~2.7 kHz (where the ear is most sensitive) = inverse
                    // equal-loudness -> perceptually flatter, audibly "scooped"/hollow vs flat white. RBJ peaking EQ.
                    const double A  = std::pow(10.0, -15.0 / 40.0);          // -15 dB cut
                    const double w0 = 2.0 * kPi * 2700.0 / sr;
                    const double al = std::sin(w0) / (2.0 * 0.7);            // Q = 0.7
                    const double cw = std::cos(w0);
                    const double a0 = 1.0 + al / A;
                    c.greyB0 = (float)((1.0 + al * A) / a0); c.greyB1 = (float)((-2.0 * cw) / a0); c.greyB2 = (float)((1.0 - al * A) / a0);
                    c.greyA1 = (float)((-2.0 * cw) / a0);    c.greyA2 = (float)((1.0 - al / A) / a0);
                }
                break;
            case SrcFM:
                c.oscShape = sl.oscShape; c.oscShapeB = sl.oscShapeB;   // FM carrier waveform (Wave A->B)
                c.fmCarrierF = 220.0 * std::pow(2.0, (double) sl.fmPitch / 12.0);
                c.fmModF     = c.fmCarrierF * (1.0 + (double) sl.fmSpread * 5.0);
                c.fmIndex    = sl.fmDepth * 12.0f;
                c.fmPEnvAmt  = sl.fmPEnvAmt; c.fmPEnvTime = sl.fmPEnvTime; c.fmPOffset = sl.fmPOffset;
                c.fmFeedback = sl.fmFeedback; c.fmSub = sl.fmSub;
                break;
            case SrcPhys:
                c.pm    = &kPhysModels[juce::jlimit(0, kNumPhysModels - 1, (int) std::lround(sl.physMaterial))];
                c.ksFb  = std::exp(-3.0f / juce::jmax(0.02f, sl.dec * c.pm->decScale) / (float) sr);
                // (Release is the user's own value now - see the note in the amp-env bake above.)
                // UNISON/CHORD: pre-compute each string's pitch multiplier (chord interval + detune spread).
                c.scaleOn   = sl.scaleOn;   // SCALE reads sv.uniSemis per-string in the render (c.uniMul ignored)
                c.uniVoices = juce::jlimit(1, KS_UNI, sl.scaleOn ? (sl.scaleType >= 10 ? 6 : sl.scaleUnison) : sl.oscUnison);
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;
                for (int k = 0; k < KS_UNI; ++k)
                {
                    const double sp = c.uniVoices > 1 ? 2.0 * (double) k / (double)(c.uniVoices - 1) - 1.0 : 0.0;
                    c.uniMul[k] = std::pow(2.0, sp * (double) c.uniCents / 1200.0);
                }
                // Pluck POSITION also shades the LOOP brightness (a mid-string pluck excites fewer
                // highs, and the burst comb alone fades from earshot once the loop filter takes
                // over - the "position does nothing" complaint). pos 0 = the authored brightness
                // (bit-identical), pos 1 = about half the tone coefficient = clearly rounder ring.
                c.ksLpC = juce::jlimit(0.02f, 1.0f, (0.05f + 0.95f * sl.physTone) * c.pm->briScale
                                                     * (1.0f - 0.5f * juce::jlimit(0.0f, 1.0f, sl.physPosition)));
                // STIFFNESS = user dispersion: stretches the partials SHARP (string -> bar -> bell).
                // 0 = the material's own chain (factory bit-identical). >0 = the negative-coefficient
                // design (designStiffChain) - the positive-coefficient approach was inaudible at the
                // 2x engine rate (twice reported by the user; see the function comment for the math).
                { const float st = juce::jlimit(0.0f, 1.0f, sl.physStiff);
                  // CHAIN 1 = user stiffness; CHAIN 2 = the MATERIAL's own dispersion. They STACK
                  // now (stiffness used to REPLACE the material chain, erasing Bell/Metal character
                  // the moment Stiffness moved). Both compensated; the material sheds stages against
                  // whatever loop budget stiffness left (total comp capped at ~45% of the period).
                  float comp1 = 0.0f;
                  if (st > 0.01f) designStiffChain(st, slotBaseHz(s, sl), sr, c.ksApC, c.ksApN, comp1);
                  else { c.ksApC = 0.0f; c.ksApN = 0; }
                  c.ksApC2 = c.pm->apC; c.ksApN2 = c.pm->apStages;
                  float comp2 = 0.0f;
                  if (c.ksApC2 != 0.0f && c.ksApN2 > 0)
                  {
                      const float tdc = (1.0f - c.ksApC2) / (1.0f + c.ksApC2);
                      const float per = (float) (sr / juce::jmax(20.0, slotBaseHz(s, sl)));
                      const float budget = juce::jmax(0.0f, 0.45f * per - comp1);
                      while (c.ksApN2 > 0 && tdc * (float) c.ksApN2 > budget) --c.ksApN2;
                      comp2 = tdc * (float) c.ksApN2;
                  }
                  c.ksApComp = comp1 + comp2; }
                c.physBaseF  = juce::jlimit(20.0, sr * 0.45, slotBaseHz(s, sl));
                c.physPEnvAmt = sl.physPEnvAmt; c.physPEnvTime = sl.physPEnvTime; c.physPOffset = sl.physPOffset;
                c.physVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.07f * vibLfo;
                break;
            case SrcSample: {
                const float crushBits = sl.smpCrush > 0.001f ? (16.0f - juce::jlimit(0.0f, 1.0f, sl.smpCrush) * 14.0f) : 0.0f;
                c.crushStep = crushBits > 0.0f ? std::pow(2.0f, -(crushBits - 1.0f)) : 0.0f;
                c.speed     = juce::jlimit(0.05, 8.0, (double) sl.smpSpeed);
                c.smpPitch  = sl.smpPitch; c.smpPEnvAmt = sl.smpPEnvAmt; c.smpPEnvTime = sl.smpPEnvTime; c.smpPOffset = sl.smpPOffset;
                c.reverse   = sl.smpReverse;
                // This slot's own buffer + the CURRENT region (set per hit by trigger's round-robin).
                c.buf    = &slotSample[s].buf;
                c.srcLen = c.buf->getNumSamples();
                c.regLo  = (sl.smpUseRegion && c.srcLen > 0)
                           ? juce::jlimit(0, c.srcLen - 1, (int)(slotSample[s].curRegLo * c.srcLen)) : 0;
                c.regHi  = (sl.smpUseRegion && c.srcLen > 0)
                           ? juce::jlimit(1, c.srcLen, (int)(slotSample[s].curRegHi * c.srcLen)) : c.srcLen;
                c.slices = 1;   // manual regions replace auto-slicing
                c.smpGain = juce::jlimit(0.0f, 4.0f, sl.smpGain);
                c.smpEnv  = sl.smpEnvOn;   // opt-in amp envelope (off = legacy full-length playback)
                c.smpPreserve = sl.smpPreservePitch;   // ignore step/draw/key/env pitch when on
                c.oscVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.09f * vibLfo;  // vibrato = varispeed wobble
                break; }
            // SrcSynth / SrcWave: legacy slot engines REMOVED (v1.2.x). No factory sound uses them; the
            // enum values stay reserved so saved projects don't renumber, but they no longer bake/render
            // (a very old project's Synth/Wave slot is silent - accepted compat break).
            case SrcGrain: {
                // GRANULAR: source = the slot's SAMPLE when loaded, else the pre-rendered wave
                // journey table. Params mapped here once per block; grains are spawned per voice.
                const auto& gt = grainTbl[s];
                c.grTbl = ((int) gt.size() == GRAIN_TBL) ? gt.data() : nullptr;
                c.grTblLen = GRAIN_TBL;
                if (slotSample[s].buf.getNumSamples() > 64)
                {   // sample source (reuses the Sample engine's per-slot buffer + trim region)
                    c.grBuf = &slotSample[s].buf;
                    const int n = slotSample[s].buf.getNumSamples();
                    c.grLo = juce::jlimit(0, n - 2, (int) (sl.smpStart * (float) n));
                    c.grHi = juce::jlimit(c.grLo + 2, n, (int) (sl.smpEnd * (float) n));
                }
                // CHORD/SCALE on granular [2026-07-16, user: "scale mode cant be used in
                // granular"]: the spawn loop cycles successive grains through the voicing's
                // notes - the CLOUD is the chord (granular has no unison stack to retune).
                c.scaleOn   = sl.scaleOn;
                c.uniVoices = juce::jlimit(1, UNI_MAX, sl.scaleOn ? (sl.scaleType >= 10 ? 6 : sl.scaleUnison) : 1);
                c.grPos   = juce::jlimit(0.0f, 1.0f, sl.grainPos);
                c.grSpray = juce::jlimit(0.0f, 1.0f, sl.grainSpray);
                c.grPitch = juce::jlimit(0.0f, 1.0f, sl.grainPitch);
                c.grLenSamp = (int) ((0.015 * std::pow(350.0 / 15.0, (double) juce::jlimit(0.0f, 1.0f, sl.grainSize)) ) * sr);
                const float dens = 3.0f * std::pow(50.0f / 3.0f, juce::jlimit(0.0f, 1.0f, sl.grainDens));
                c.grSpawnPerSamp = dens / (float) sr;
                c.grNorm = 0.85f / std::sqrt(juce::jmax(1.0f, c.grSpawnPerSamp * (float) c.grLenSamp));
                // table mode: one table CYCLE plays at the slot's base pitch (keys/step transpose ride pe3Mul/keySemis)
                c.grIncBase = (double) GRAIN_CYC * juce::jlimit(20.0, sr * 0.45, slotBaseHz(s, sl)) / sr;
                c.atk = sl.atk; c.hold = sl.hold; c.dec = sl.dec;   // full AHDSR like SrcOsc
                break; }
            case SrcModal: {
                // Build the resonator bank from the Material + base pitch + Decay/Tone/Structure.
                // MORPH crossfades this material's mode table toward the NEXT material in the list
                // (ratios/gains/decays lerped per mode; modes missing on one side fade in/out). 0 = pure.
                const int   mi = juce::jlimit(0, kNumModalMaterials - 1, sl.modalMaterial);
                const auto& A  = kModalMaterials[mi];
                const auto& B  = kModalMaterials[juce::jmin(mi + 1, kNumModalMaterials - 1)];
                const float mo = juce::jlimit(0.0f, 1.0f, sl.modalMorph);
                const int   nModes = (mo > 0.001f) ? juce::jmax(A.n, B.n) : A.n;
                const double baseF = juce::jlimit(20.0, sr * 0.45, slotBaseHz(s, sl));
                const float  decaySec = 0.05f + juce::jlimit(0.0f, 1.0f, sl.modalDecay) * 3.95f;     // 0.05..4 s
                const float  tone   = juce::jlimit(0.0f, 1.0f, sl.modalTone);
                const float  stretch = 0.6f + juce::jlimit(0.0f, 1.0f, sl.modalStruct) * 0.8f;        // 0.6..1.4 (0.5->1.0)
                const float  hit  = juce::jlimit(0.0f, 1.0f, sl.modalHit);    // strike-position comb amount (0 = none)
                const float  damp = juce::jlimit(0.0f, 1.0f, sl.modalDamp);   // extra ring damping (0 = none)
                const float  hitPos = 0.5f * hit;                            // strike point moves edge -> centre
                c.modalN = 0;
                for (int i = 0; i < nModes && i < MODAL_MODES; ++i) {
                    const bool inA = i < A.n, inB = i < B.n;
                    const float rA = inA ? A.ratio[i]    : (inB ? B.ratio[i] : 1.0f);
                    const float rB = inB ? B.ratio[i]    : rA;
                    const float gA = inA ? A.gain[i]     : 0.0f;              // missing on a side -> fades to silent
                    const float gB = inB ? B.gain[i]     : 0.0f;
                    const float dA = inA ? A.decayMul[i] : (inB ? B.decayMul[i] : 1.0f);
                    const float dB = inB ? B.decayMul[i] : dA;
                    const float mRatio = rA + (rB - rA) * mo;
                    const float mGain  = gA + (gB - gA) * mo;
                    const float mDec   = dA + (dB - dA) * mo;
                    double ratio = 1.0 + ((double) mRatio - 1.0) * stretch;                            // inharmonicity
                    double f = baseF * ratio;
                    if (f >= sr * 0.48) continue;                                                      // skip modes above Nyquist
                    const float hi = (float) i / (float) juce::jmax(1, nModes - 1);                    // 0..1 mode height
                    // decay: per-material, shortened for higher modes - more so when dark (low tone), more with Damp.
                    const float dmul = mDec * (1.0f - (1.0f - tone) * 0.6f * hi) * (1.0f - damp * (0.3f + 0.7f * hi));
                    const float dSec = juce::jmax(0.02f, decaySec * dmul);
                    const float r  = std::exp(-1.0f / (dSec * (float) sr));                            // per-sample pole radius
                    const double w = 2.0 * kPi * f / sr;
                    const int k = c.modalN++;
                    c.modalA1[k] = 2.0f * r * (float) std::cos(w);
                    c.modalA2[k] = r * r;
                    // gain: material gain, tilted by Tone (dark damps highs). x sin(w) = the impulse-input
                    // coefficient so each mode's ring is ~gain amplitude regardless of its frequency.
                    float g = mGain * (0.35f + 0.65f * (tone * 0.5f + 0.5f) * (1.0f - 0.5f * hi * (1.0f - tone)));
                    // Hit position: striking at a node of a mode doesn't excite it (comb). hit=0 -> full (no comb).
                    const float comb = std::abs(std::sin((float)(i + 1) * juce::MathConstants<float>::pi * hitPos));
                    g *= (1.0f - hit) + hit * comb;
                    c.modalGain[k] = g * (float) std::sin(w);
                }
                c.oscFreq = baseF;
                c.modalDecaySec = decaySec;
                c.oscVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.07f * vibLfo;   // block-rate pole-angle wobble
                c.scaleOn   = sl.scaleOn;   // SCALE reads sv.uniSemis per bank-note in the render
                c.uniVoices = juce::jlimit(1, MODAL_NOTES, sl.scaleOn ? sl.scaleUnison : sl.oscUnison);   // one FULL bank per note (cap MODAL_NOTES)
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;
                // Pitch-envelope time base = the audible Strike + Ring (matches the editor's axis).
                c.voiceLenSamp = juce::jmax(1.0, (double)((sl.atk + decaySec) * (float) sr));
                break; }
            default: break;
        }
        // For samples, the pitch-envelope time axis is the (trimmed) SAMPLE length, not the
        // amp envelope (samples have no AHDSR now). Mirror the voiceEnd "natural" length.
        if (sl.engine == SrcSample)
            c.voiceLenSamp = juce::jmax(1.0, (double)(c.regHi - c.regLo) * (double) engineOS / juce::jmax(0.05, c.speed) / (double) c.slices);

        // === PER-SLOT FILTERS (begin) - TWO in series; stash raw params, coeffs baked per BLOCK below.
        const int   fTy[2] = { sl.filterType,   sl.filterType2 };
        const float fCu[2] = { sl.filterCutoff, sl.filterCutoff2 };
        const float fRe[2] = { sl.filterReso,   sl.filterReso2 };
        const float fEn[2] = { sl.filterEnvAmt, sl.filterEnvAmt2 };
        for (int fi = 0; fi < 2; ++fi)
        {
            c.filt[fi].on       = (fTy[fi] >= LowPass && fTy[fi] <= Notch) || fTy[fi] == Bell;   // (Formant = legacy, off here)
            c.filt[fi].mode     = fTy[fi] == Bell ? 4 : juce::jlimit(0, 3, fTy[fi] - LowPass);
            c.filt[fi].cutoff   = juce::jlimit(20.0, sr * 0.49, (double) fCu[fi]);
            c.filt[fi].reso     = juce::jlimit(0.3f, 12.0f, fRe[fi]);
            c.filt[fi].envAmt   = juce::jlimit(-1.0f, 1.0f, fEn[fi]);
            // ZDF cutoff coeff + damping (was a separate block-level pass - here so PER-VOICE re-bakes get
            // it too; env-follow uses the PREVIOUS block's per-slot amp level).
            if (c.filt[fi].on)
            {
                double cutoff = c.filt[fi].cutoff;
                if (c.filt[fi].envAmt != 0.0f)
                    cutoff *= std::pow(2.0, (double) c.filt[fi].envAmt * (double) slotFiltEnv[s] * 5.0);
                c.filt[fi].cutoffHz = juce::jlimit(20.0, sr * 0.49, cutoff);   // stash (Hz) so per-voice KEYTRACK re-tans from it
                c.filt[fi].G = std::tan(kPi * c.filt[fi].cutoffHz / sr);       // ZDF prewarped cutoff target
                if (c.filt[fi].mode == 4)
                {   // BELL (bipolar): filterGain = boost/cut dB (Y drag, +-15), reso = the bell's Q
                    // (wheel). Cytomic SVF bell: A = 10^(dB/40), K = 1/(Q*A), out = in + K*(A^2-1)*v1.
                    const double gDb = juce::jlimit(-15.0, 15.0, (double)(fi == 0 ? sl.filterGain : sl.filterGain2));
                    const double A  = std::pow(10.0, gDb / 40.0);
                    const double Q  = juce::jlimit(0.3, 12.0, (double) c.filt[fi].reso);
                    c.filt[fi].K      = 1.0 / (Q * A);
                    c.filt[fi].bellM1 = c.filt[fi].K * (A * A - 1.0);
                    c.filt[fi].bellA  = A;
                }
                else
                {
                    c.filt[fi].K = 1.0 / juce::jmax(0.15, (double) c.filt[fi].reso);
                    c.filt[fi].bellM1 = 0.0;
                    c.filt[fi].bellA  = 1.0;
                }
                c.filt[fi].resoB = c.filt[fi].reso;
            }
        }
        // === PER-SLOT FILTERS (end) ===
        if (sl.oscShape >= WvCustom)                                       // ADDITIVE WAVETABLE (Custom wave)
        {
            for (int f = 0; f < ADD_FRAMES; ++f) c.wtFrm[f] = addTbl[s][f];
            c.wtPos   = juce::jlimit(0.0f, 1.0f, sl.addPos);
            c.wtGlide = sl.addSeg[0] > 0.001f;
            c.wtLoop  = c.wtGlide && sl.addLoop;
            if (c.wtGlide)
            {   // bake the piecewise clock: leg k covers a third of the strip over addSeg[k] sec;
                // a 0 leg = HOLD (boundary pushed to infinity, slope 0 = park at its left frame)
                const float third = 1.0f / 3.0f, INF = 1.0e18f;
                c.wtT1 = juce::jmax(0.02f, sl.addSeg[0]) * (float) sr;
                c.wtInv0 = third / c.wtT1;
                if (sl.addSeg[1] > 0.001f)
                {
                    c.wtT2 = c.wtT1 + juce::jmax(0.02f, sl.addSeg[1]) * (float) sr;
                    c.wtInv1 = third / (c.wtT2 - c.wtT1);
                    if (sl.addSeg[2] > 0.001f)
                    { c.wtT3 = c.wtT2 + juce::jmax(0.02f, sl.addSeg[2]) * (float) sr; c.wtInv2 = third / (c.wtT3 - c.wtT2); }
                    else { c.wtT3 = INF; c.wtInv2 = 0.0f; }
                }
                else { c.wtT2 = INF; c.wtInv1 = 0.0f; c.wtT3 = INF; c.wtInv2 = 0.0f; }
                // journey end = the last FINITE boundary (where a hold parks / the strip ends)
                c.wtLoopEnd = c.wtT3 < INF ? c.wtT3 : (c.wtT2 < INF ? c.wtT2 : c.wtT1);
            }
        }
        else { for (int f = 0; f < ADD_FRAMES; ++f) c.wtFrm[f] = nullptr; c.wtPos = 0.0f; c.wtGlide = false; }
        c.punch   = juce::jlimit(-1.0f, 1.0f, sl.fxPunch);
        c.ring    = juce::jlimit(0.0f, 1.0f, sl.fxRing);
        c.ringTrack = sl.fxRingHz < 26.0f;                                   // hard left of the Ring Hz knob = TRACK the note
        c.ringHz    = juce::jlimit(26.0, 4000.0, (double) sl.fxRingHz);
        c.ringBase  = (c.ring > 0.0f && c.ringTrack) ? slotBaseHz(s, sl) : 0.0;
        // SUB: a clean sine ONE OCTAVE below the slot's ONE base pitch (slotBaseHz = the roll's C4
        // world too). Unpitched engines (Noise / Sample) have no base -> the knob is inert there.
        c.subAmt = juce::jlimit(0.0f, 1.0f, sl.fxSub);
        c.subHz  = 0.0;
        {   // SLOT PAN: equal-power with UNITY at centre (sqrt(1 -/+ p)) - pan 0 = bit-identical.
            const float pp = juce::jlimit(-1.0f, 1.0f, sl.pan);
            c.panL = std::sqrt(1.0f - pp); c.panR = std::sqrt(1.0f + pp); c.panBase = pp;
        }
        if (c.subAmt > 0.0f)
            switch (sl.engine) {
                case SrcOsc: case SrcFM: case SrcGrain: case SrcPhys: case SrcModal:
                    c.subHz = 0.5 * slotBaseHz(s, sl); break;
                default: break;
            }
        {   // FORMANT: the knob = vowel position A -> E -> I -> O -> U; the wet ramps in over the
            // first ~12% so 0 stays bit-identical and low values ease in instead of jumping to "A".
            const float fv = juce::jlimit(0.0f, 1.0f, sl.fxFormant);
            c.fmtMix = fv <= 0.001f ? 0.0f : juce::jmin(1.0f, fv / 0.12f);
            if (c.fmtMix > 0.0f)
            {
                static const float Fm1[5] = { 800.0f, 400.0f, 250.0f, 400.0f, 350.0f };    // F1: A E I O U
                static const float Fm2[5] = { 1150.0f, 2000.0f, 2300.0f, 800.0f, 600.0f }; // F2: A E I O U
                const float pos = juce::jlimit(0.0f, 1.0f, (fv - 0.12f) / 0.88f) * 4.0f;
                const int   vi  = juce::jlimit(0, 3, (int) pos); const float fr = pos - (float) vi;
                auto ip = [&](const float* T) { return (double) (T[vi] * std::pow(T[vi + 1] / T[vi], fr)); };  // log interp
                c.fmtBq[0].bandpass(sr, juce::jlimit(60.0, sr * 0.45, ip(Fm1)), 7.0);
                c.fmtBq[1].bandpass(sr, juce::jlimit(60.0, sr * 0.45, ip(Fm2)), 7.0);
            }
        }
        };
    chFxMod[0] = chFxMod[1] = chFxMod[2] = chFxMod[3] = 0.0f;   // CHANNEL FX mod offset (both slots' routes accumulate below)
    for (int cf = 0; cf < 4; ++cf) { chFiltMod[cf] = 0.0f; chFiltRouted[cf] = false; }   // CHANNEL filter pair [2026-07-16]
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        SC& c = sc[s];
        // MOD MATRIX (block-rate): if any route is live, sample the sources for this slot and apply
        // them onto a scratch Slot copy, then bake from THAT. All routes off/0 = the copy is skipped
        // and `sl` is the real slot = byte-for-byte the old path (bit-identical). The four audio-rate
        // LFO paths still run unchanged; the matrix modulates the block config, not the sample loop.
        Slot modTmp;
        const Slot* slp = &slots[s];
        c.wtPosOfs = 0.0f;
        if (slots[s].modActive())
        {
            modTmp = slots[s];
            float srcVals[MS_COUNT] = {};
            computeModSources(s, modTmp, srcVals);
            float lagged[MOD_ROUTES];   // [2026-07-14 01:33] per-route LAG applied ONCE per block
            lagRouteSources(modTmp, srcVals, modLagBlk[s], (float) numSamples / (float) sr, lagged);
            applyModMatrix(modTmp, srcVals, nullptr, lagged);
            applyHotBlock(modTmp, srcVals, lagged);   // display/sc-only: rings + analysis see the block value
            slp = &modTmp;
            // CHANNEL FX targets can't live on the slot copy - accumulate their offsets here (both slots add).
            for (int ri3 = 0; ri3 < MOD_ROUTES; ++ri3)
            {
                const auto& r = modTmp.mod[ri3];
                if (r.src == MSOff || std::abs(r.amt) < 1.0e-4f) continue;
                const float m = r.amt * modRouteShape(r, lagged[ri3]);
                switch (r.tgt) { case MTChFxAAmt: chFxMod[0] += m; break; case MTChFxAChr: chFxMod[1] += m; break;
                                 case MTChFxBAmt: chFxMod[2] += m; break; case MTChFxBChr: chFxMod[3] += m; break;
                                 case MTRevSend:  chFxMod[4] += m; break; case MTDelSend:  chFxMod[5] += m; break;
                                 case MTChFxCAmt: chFxMod[6] += m; break; case MTChFxCChr: chFxMod[7] += m; break;
                                 case MTChFilt1Cut: chFiltMod[0] += m; chFiltRouted[0] = true; break;
                                 case MTChFilt1Res: chFiltMod[1] += m; chFiltRouted[1] = true; break;
                                 case MTChFilt2Cut: chFiltMod[2] += m; chFiltRouted[2] = true; break;
                                 case MTChFilt2Res: chFiltMod[3] += m; chFiltRouted[3] = true; break;
                                 default: break; }
            }
            // WAVE / grain POSITION modulation is applied in the RENDER as an offset (so it works even
            // when the wavetable is GLIDING, which overrides the static position) - bake the base position.
            if (modTmp.engine == SrcGrain) { c.wtPosOfs = modTmp.grainPos - slots[s].grainPos; modTmp.grainPos = slots[s].grainPos; }
            else                           { c.wtPosOfs = modTmp.addPos   - slots[s].addPos;   modTmp.addPos   = slots[s].addPos; }
            // live snapshot for the editor's mod RINGS (raw modulated FX values). Indices 2/5/18/19 were
            // the per-slot chorus/comp/flanger/phaser - those are CHANNEL FX now (rung via chFxLive).
            slotModLiveFx[s][0] = -1000.0f;            slotModLiveFx[s][1] = -1000.0f;   // (sends = channel faders now)
            slotModLiveFx[s][2] = -1000.0f;            slotModLiveFx[s][3] = modTmp.fxSub;      // [3] = Sub (was Tone)
            slotModLiveFx[s][4] = modTmp.fxPunch;      slotModLiveFx[s][5] = modTmp.fxFormant;  // [5] = Formant (was Comp)
            slotModLiveFx[s][6] = modTmp.fxDrive;
            slotModLiveFx[s][7] = modTmp.filterCutoff; slotModLiveFx[s][8] = modTmp.filterCutoff2;
            for (int gi = 0; gi < MOD_TGT_GRID; ++gi) {   // the 8 engine GRID knobs (FM Amount, Ratio, ...)
                const GridKnob gk = modGridKnob(modTmp.engine, gi);
                slotModLiveFx[s][9 + gi] = (gk.field != nullptr) ? (float)(modTmp.*(gk.field)) : -1000.0f;
            }
            slotModLiveFx[s][17] = (modTmp.engine == SrcOsc) ? modTmp.oscWarp : -1000.0f;   // Warp (wave preview + fader)
            slotModLiveFx[s][18] = -1000.0f; slotModLiveFx[s][19] = -1000.0f; slotModLiveFx[s][20] = modTmp.fxRing;
            // UNISON dots (detune/vib/width/drift): a ring only when a route actually targets them.
            auto uniTgt = [&](int tg) { for (auto& r : modTmp.mod) if (r.tgt == tg && r.src != MSOff && std::abs(r.amt) > 1.0e-4f) return true; return false; };
            slotModLiveFx[s][21] = uniTgt(MTDetune)  ? modTmp.oscDetune : -1000.0f;
            slotModLiveFx[s][22] = uniTgt(MTVibrato) ? modTmp.vibrato   : -1000.0f;
            slotModLiveFx[s][23] = uniTgt(MTWidth)   ? modTmp.uniSpread : -1000.0f;
            slotModLiveFx[s][24] = uniTgt(MTDrift)   ? modTmp.drift     : -1000.0f;
            slotModLiveFx[s][25] = uniTgt(MTRingHz)  ? modTmp.fxRingHz  : -1000.0f;   // [2026-07-13 22:45]
            slotModLiveFx[s][26] = uniTgt(MTSlotPan) ? modTmp.pan       : -1000.0f;
            // [2026-07-14 00:30] live FILTER visuals: modulated RESO + ENV AMOUNT (user: "live visual pls")
            slotModLiveFx[s][27] = uniTgt(MTFilt1Res) ? modTmp.filterReso     : -1000.0f;
            slotModLiveFx[s][28] = uniTgt(MTFilt2Res) ? modTmp.filterReso2    : -1000.0f;
            slotModLiveFx[s][29] = uniTgt(MTFilt1Env) ? modTmp.filterEnvAmt   : -1000.0f;
            slotModLiveFx[s][30] = uniTgt(MTFilt2Env) ? modTmp.filterEnvAmt2  : -1000.0f;
            slotModLiveFx[s][31] = uniTgt(MTUniCount) ? (float) modTmp.oscUnison : -1000.0f;
            // Keep any LFO used as a matrix SOURCE advancing even if its own Amount is 0.
            for (auto& r : modTmp.mod)
                if (r.tgt != MTOff && std::abs(r.amt) > 1.0e-4f
                    && r.src >= MSLfoFilt && r.src <= MSLfoWave)
                    c.lfoSrcUsed[r.src - MSLfoFilt] = true;
        }
        else for (auto& v : slotModLiveFx[s]) v = -1000.0f;   // matrix inactive -> no rings
        const Slot& sl = *slp;
        // Weight-skip decides on the BASE slot when the matrix is live: a route swinging the modulated
        // weight to 0 (LFO -> Volume trough) must KEEP baking/rendering - skipping froze the LFO at the
        // silent phase and tripped !anySlotActive, which HARD-KILLED every voice = "sound dies after the
        // first dip" (+ the kill click). A statically-silent slot (base weight 0, no routes) still skips.
        const bool weightSilent = slots[s].modActive() ? (slots[s].weight <= 0.0f && sl.weight <= 0.0f)
                                                       : (sl.weight <= 0.0f);
        if (sl.engine < 0 || weightSilent) continue;
        if (sl.engine == SrcSample && slotSample[s].buf.getNumSamples() == 0) continue;
        bakeSlot(s, sl, c);
        anySlotActive = true;
        if (sl.weight > domW) { domW = sl.weight; domSlot = s; }
        if (s == 0)   // once per block: advance the free-run clock when the transport isn't driving us
        { if (lfoBarPos >= 0.0) lfoFreeSec = 0.0; else lfoFreeSec += (double) numSamples / sr; }
    }

    if (! anySlotActive)
    { for (auto& v : voices) v.playHead = -1.0; feedSilence(); return; }

    // (Blend character Bloom/Drift/Spread was removed - it was hardcoded to 0 = no effect. Slots mix at
    //  their own weight; per-slot stereo comes from Unison Width, not a slot pan.)

    // Voice length = the longest active slot's tail.
    long voiceEnd = 1;
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        const SC& c = sc[s];
        if (c.engine < 0) continue;
        const long susTail = (long) (3.2f * juce::jmax(0.002f, c.release) * (float) sr);  // per-slot release tail
        const long ahdEnd = (long) ((c.atk + c.hold) * (float) sr) + (long) (3.2f * c.dec * (float) sr) + 8;
        if (c.engine == SrcSample)
        {
            // Samples play the full (trimmed) sample + a short anti-click fade. With the OPT-IN amp
            // envelope on, the voice can end at the envelope's end instead (whichever is shorter).
            long natural = (long) ((double) (c.regHi - c.regLo) * (double) engineOS / juce::jmax(0.05, c.speed) / (double) c.slices) + 8;
            if (c.smpEnv) natural = juce::jmin(natural, ahdEnd);
            voiceEnd = juce::jmax(voiceEnd, natural);
        }
        else if (c.engine == SrcPhys)
            voiceEnd = juce::jmax(voiceEnd, (long) ((c.atk + c.hold + 3.6f * c.dec * c.pm->decScale) * (float) sr) + 8
                                            + (c.sustain > 0.001f ? susTail : 0));
        else if (c.engine == SrcModal)   // the modes carry the decay (Ring); + the Strike (attack) ramp on top
            voiceEnd = juce::jmax(voiceEnd, (long) ((c.atk + 6.0f * c.modalDecaySec) * (float) sr) + 8);
        else
            voiceEnd = juce::jmax(voiceEnd, ahdEnd + (c.sustain > 0.001f ? susTail : 0));
    }
    renderBuf.setSize(2, numSamples, false, false, true);
    renderBuf.clear();
    float* outL = renderBuf.getWritePointer(0);
    float* outR = renderBuf.getWritePointer(1);
    float maxEnvLevel = 0.0f;

    // Per-slot reverb/delay SEND accumulation (each slot adds its signal x its own send amount).

    // (Chorus/Flanger/Phaser/Comp are CHANNEL FX now - they run ONCE on the summed channel output after
    //  the voice loop, so voices always add straight to the mix here.)

    // === PER-SLOT EQ (begin) - spectrum tap for one slot: clear the accumulator ===
    // analysisBuf points into ONE processor-owned scratch (set per block, only for the
    // analysed channel) - not a per-channel array anymore.
    const int tapSlot = (analysisTap != nullptr && analysisBuf != nullptr
                         && analysisSlot >= 0 && analysisSlot < NUM_SLOTS) ? analysisSlot : -1;
    if (tapSlot >= 0) { const int nn = juce::jmin(numSamples, analysisBufLen); for (int i = 0; i < nn; ++i) analysisBuf[i] = 0.0f; }
    // === PER-SLOT EQ (end) ===

    // KS delay lines are lazily allocated; never touch them until ensureKsBuffers() ran.
    const bool ksOk = ksReady.load(std::memory_order_acquire);

    // === PER-SLOT FILTER (begin) - bake this block's coeffs. The env-follow sweep uses the
    //     PREVIOUS block's per-slot amp level (slotFiltEnv, ~one block latency = a few ms), same
    //     spirit as the channel filter's per-block sweep. blockSlotEnv accrues THIS block's level. ===
    float blockSlotEnv[NUM_SLOTS] = {};
    // Drive post-smoothing coefficient (~8 kHz 1-pole at the engine rate) for the HARSH shapers.
    const float drvLpK = 1.0f - std::exp(-2.0f * (float) kPi * 8000.0f / (float) juce::jmax(1.0, sr));
    // BASS AMP voicing (fixed macro constants): 180 Hz clean-low split + ~3.8 kHz 2-pole cab.
    const float drvAmpBassK = 1.0f - std::exp(-2.0f * (float) kPi * 180.0f  / (float) juce::jmax(1.0, sr));
    const float drvAmpCab2K = 1.0f - std::exp(-2.0f * (float) kPi * 3800.0f / (float) juce::jmax(1.0, sr));
    // PUNCH transient followers: ~1.5 ms fast / ~50 ms slow.
    const float punchKf = 1.0f - std::exp(-1.0f / (0.0015f * (float) juce::jmax(1.0, sr)));
    const float punchKs = 1.0f - std::exp(-1.0f / (0.050f  * (float) juce::jmax(1.0, sr)));
    // De-zipper pole (~3 ms): the mod matrix moves params at BLOCK rate; gain-like targets are smoothed
    // per sample toward the block value so fast sources (LFO/Step Mod/wheel) never step audibly.
    const float wSmK = 1.0f - std::exp(-1.0f / (0.003f * (float) juce::jmax(1.0, sr)));
    // [2026-07-13 19:57] AUDIO-RATE steppy sources = block-linear RAMPS: Step Mod A/B jump at step
    // boundaries and the Mod Wheel arrives as quantised CCs - ramping them across the block gives the
    // per-sample route program a smooth signal with no per-voice state (every voice reads start+step*i).
    float arA0, arAStep, arB0, arBStep, arW0, arWStep, arP0, arPStep;
    {
        float tgtA = 0.0f, tgtB = 0.0f;
        if (modStepPos >= 0.0f && numSteps > 0)
        {   const int st = juce::jlimit(0, numSteps - 1, (int) modStepPos);
            tgtA = juce::jlimit(0.0f, 1.0f, stepModA[st]); tgtB = juce::jlimit(0.0f, 1.0f, stepModB[st]); }
        const float tgtW = juce::jlimit(0.0f, 1.0f, modWheel);
        const float tgtP = juce::jlimit(-1.0f, 1.0f, pitchWheel);   // [2026-07-14 03:00]
        if (arStepACur < 0.0f) arStepACur = tgtA;   // first block = snap (no fade-in from silence)
        if (arStepBCur < 0.0f) arStepBCur = tgtB;
        if (arWheelCur < 0.0f) arWheelCur = tgtW;
        if (arPwCur < -1.5f)   arPwCur = tgtP;
        const float inv = 1.0f / (float) juce::jmax(1, numSamples);
        arA0 = arStepACur; arAStep = (tgtA - arStepACur) * inv; arStepACur = tgtA;
        arB0 = arStepBCur; arBStep = (tgtB - arStepBCur) * inv; arStepBCur = tgtB;
        arW0 = arWheelCur; arWStep = (tgtW - arWheelCur) * inv; arWheelCur = tgtW;
        arP0 = arPwCur;    arPStep = (tgtP - arPwCur) * inv;    arPwCur = tgtP;
    }
    // (The per-slot filter cutoff/G/K bake moved INTO bakeSlot so per-voice re-bakes compute it too.)
    // === PER-SLOT FILTER (end) ===

    // ---- Voice loop -----------------------------------------------------------
    for (int vi = 0; vi < POLY; ++vi)
    {
        Voice& v = voices[vi];
        if (! v.active()) continue;

        // PER-VOICE MODULATION: re-bake each slot's config from THIS voice's OWN sources (its velocity,
        // note, amp/mod envelope, per-note random, retrig-LFO phase) so keytrack / velocity->cutoff /
        // per-note pitch etc. are correct on chords - not the newest-voice approximation. Sounds with no
        // live routes reuse the block config (cs -> sc), so unmodulated sounds pay nothing. The block bake
        // (sc) still feeds chorus/comp/analysis; only THIS voice's render reads cs. GLOBAL sources
        // (free-run LFO, mod wheel, step lanes) are shared; the amp-env SOURCE stays the slot's block level.
        SC vsc[NUM_SLOTS];
        const SC* cs[NUM_SLOTS];
        for (int s = 0; s < NUM_SLOTS; ++s)
        {
            if (! slots[s].modActive()) { cs[s] = &sc[s]; continue; }
            Slot vm = slots[s];
            float sv2[MS_COUNT] = {};
            computeModSources(s, vm, sv2, &v);   // sample THIS voice
            float lagV[MOD_ROUTES];              // [2026-07-14 01:33] per-VOICE lag (states reset at the hit = swell-in)
            lagRouteSources(vm, sv2, v.sv[s].modLagV, (float) numSamples / (float) sr, lagV);
            applyModMatrix(vm, sv2, &v.sv[s], lagV);   // env-time targets latch at the hit (crackle fix)
            // FORMANT position GLIDES at block rate (~12 ms): its two Q-7 band-pass coefficients are
            // baked per block, and stepping their centre frequencies on a driven signal warbles =
            // the same crackle class. Smoothing the position also smooths the derived wet mix.
            {   SlotVoice& fsv = v.sv[s];
                if (fsv.fmtPosSm < 0.0f) fsv.fmtPosSm = vm.fxFormant;
                else fsv.fmtPosSm += juce::jlimit(0.0f, 1.0f, (float) numSamples / (0.012f * (float) sr))
                                     * (vm.fxFormant - fsv.fmtPosSm);
                vm.fxFormant = fsv.fmtPosSm; }
            const float wpo = (vm.engine == SrcGrain ? vm.grainPos - slots[s].grainPos : vm.addPos - slots[s].addPos);
            if (vm.engine == SrcGrain) vm.grainPos = slots[s].grainPos; else vm.addPos = slots[s].addPos;
            bakeSlot(s, vm, vsc[s]);
            vsc[s].wtPosOfs = wpo;
            for (int d = 0; d < 4; ++d) vsc[s].lfoSrcUsed[d] = sc[s].lfoSrcUsed[d];   // route-based -> keep phases advancing
            cs[s] = &vsc[s];
        }

        // Set noise band-pass coefficients once per voice (Noise + Synth-noise slots).
        for (int s = 0; s < NUM_SLOTS; ++s)
            if (sc[s].engine == SrcNoise && sc[s].noiseBP)
                v.sv[s].noiseBP.bandpass(sr, sc[s].noiseFc, sc[s].nQ);

        // Channel PITCH (semitones). SYNTHS shift frequency here (vPitchMul). SAMPLES get the channel/slot
        // pitch baked into their buffer by SoundTouch (pitch-shift, no length change) - so they use ONLY
        // the per-step pitch here (vStepMul, still varispeed since it's per-hit). Both include voicePitch.
        // NON-const: a 303 slide updates voicePitch per sample and these track it.
        const double chanMul   = pitch != 0.0f ? std::pow(2.0, (double) pitch / 12.0) : 1.0;
        double vStepMul  = v.voicePitch != 0.0f ? std::pow(2.0, (double) v.voicePitch / 12.0) : 1.0;  // samples
        double vPitchMul = vStepMul * chanMul; // synths
        // Per-step PAN: balance this voice's stereo output before it sums in (rides ON TOP of the
        // channel pan applied later). 0 = centre, -1 = hard left, +1 = hard right.
        const float vPanL = v.voicePan <= 0.0f ? 1.0f : 1.0f - v.voicePan;
        const float vPanR = v.voicePan >= 0.0f ? 1.0f : 1.0f + v.voicePan;
        // Choke fade: ~3 ms ramp to zero instead of a hard cut (a mid-sample discontinuity
        // clicks when the choking hit is quieter than the tail being cut).
        const float killStep = 1.0f / juce::jmax(1.0f, 0.003f * (float) sr);
        // Per-step LENGTH: this voice's end comes from its RESCALED decays (gateDec), not the
        // authored ones - a long note lives to the end of its stretched fall, a short note ends
        // early. Ties may have extended gateLen past the env's tail; keep the voice alive to there.
        long veEnd = voiceEnd;
        if (v.gateLen > 0)
        {
            veEnd = v.gateLen + (long)(0.05f * (float) sr);
            for (int s = 0; s < NUM_SLOTS; ++s)
                if (sc[s].engine >= 0)
                {
                    veEnd = juce::jmax(veEnd, (long)((sc[s].atk + sc[s].hold + 3.2f * v.sv[s].gateDec) * (float) sr) + 8);
                    if (sc[s].sustain > 0.01f)   // gated-sustain slot: add the release tail after the gate
                        veEnd = juce::jmax(veEnd, v.gateLen + (long)(3.2f * juce::jmax(0.005f, sc[s].release) * (float) sr) + 8);
                }
        }
        // KEYS voice: alive for as long as the key is HELD (the env sustains); once released,
        // long enough for the slowest slot RELEASE to ring out (Phys/Modal natural tails already
        // fit inside voiceEnd, so only extend - never shorten).
        if (v.isKey)
        {
            if (v.keyOff < 0) veEnd = std::numeric_limits<long>::max() / 2;
            else
            {
                float relMax = 0.005f;
                for (int s = 0; s < NUM_SLOTS; ++s)
                    if (sc[s].engine >= 0) relMax = juce::jmax(relMax, sc[s].release);
                veEnd = juce::jmax(veEnd, v.keyOff + (long)(3.2f * relMax * (float) sr) + 8);
            }
        }
        // LATCHED env-mod offsets can LENGTHEN this voice (a route adding attack/decay/release) -
        // extend the end estimate or the modulated tail is hard-cut at the base length (a click).
        if (veEnd < std::numeric_limits<long>::max() / 4)
            for (int s = 0; s < NUM_SLOTS; ++s)
                if (v.sv[s].envModLatched)
                {
                    const float* o = v.sv[s].envModOfs;
                    const float ext = juce::jmax(0.0f, o[0]) + 3.2f * juce::jmax(0.0f, o[1] * 2.0f)
                                    + 3.2f * juce::jmax(0.0f, o[3] * 2.0f);
                    if (ext > 0.0f) veEnd += (long)(ext * (float) sr);
                }
        // SAMPLE plays to its TRUE end: a varispeed sample (Keep pitch off) pitched DOWN reads SLOWER than
        // its natural-speed length, and a one-shot sample shouldn't be cut by a key release either. Keep the
        // voice alive long enough for the head to reach the region end (Keep pitch on = natural length).
        for (int s = 0; s < NUM_SLOTS; ++s)
            if (sc[s].engine == SrcSample)
            {
                const double keyMul  = std::pow(2.0, (double) v.sv[s].keySemis / 12.0);
                const double noteMul = sc[s].smpPreserve ? 1.0 : juce::jmax(0.02, vStepMul * keyMul);
                const long   nat = (long)((double)(sc[s].regHi - sc[s].regLo) * (double) engineOS
                                          / juce::jmax(0.05, sc[s].speed) / (double) sc[s].slices);
                veEnd = juce::jmax(veEnd, (long)((double) nat / juce::jmax(0.02, noteMul)) + 8);
            }
        // KS + Length: the string's own feedback decay is rescaled too (same rule as the env),
        // else a held pluck's loop would die at its authored rate under a slower envelope.
        float ksFbGate[NUM_SLOTS];
        for (int s = 0; s < NUM_SLOTS; ++s)
            ksFbGate[s] = (sc[s].engine == SrcPhys && v.sv[s].gateDec > 0.0f)
                ? std::exp(-3.0f / juce::jmax(0.02f, v.sv[s].gateDec * sc[s].pm->decScale) / (float) sr) : -1.0f;
        // DRIFT slow pitch wander: one gentle sine per slot-voice, advanced per block (+-4 cents
        // at full drift; block-rate steps on a sub-Hz wobble are inaudible). driftWobMul = 1 at drift 0.
        for (int s = 0; s < NUM_SLOTS; ++s)
        {
            auto& svd = v.sv[s];
            if (sc[s].drift > 0.001f)
            {
                svd.driftWobMul = (float) std::pow(2.0, std::sin(svd.driftWobPh) * (double) sc[s].drift * 12.0 / 1200.0);
                svd.driftWobPh += 2.0 * kPi * (double) svd.driftWobRate * (double) numSamples / sr;
                if (svd.driftWobPh > 2.0 * kPi) svd.driftWobPh -= 2.0 * kPi;
            }
            else svd.driftWobMul = 1.0f;
        }
        // DRIFT per-note cutoff (the dedicated FILTER KEYTRACK is GONE - keytracking is a per-voice
        // matrix route now: Note -> Filter Cutoff; old flK saves + factory sounds were migrated).
        for (int s = 0; s < NUM_SLOTS; ++s)
            for (int fi = 0; fi < 2; ++fi)
            {
                auto& f = cs[s]->filt[fi];   // per-voice filter config
                if (f.on && std::abs(v.sv[s].driftFiltMul - 1.0f) > 1.0e-4f)
                    v.sv[s].filtGkt[fi] = std::tan(kPi * juce::jlimit(20.0, sr * 0.49,
                                                    f.cutoffHz * (double) v.sv[s].driftFiltMul) / sr);
                else v.sv[s].filtGkt[fi] = -1.0;
            }
        // HUMANIZE/STRUM push a slot's (and its chord notes') onset later, so keep the voice alive
        // long enough for the most-delayed one to finish (else its tail is cut).
        long maxDelay = 0;
        for (int s = 0; s < NUM_SLOTS; ++s)
        {
            long d = v.sv[s].startDelay;
            for (int u = 0; u < UNI_MAX; ++u) d = juce::jmax(d, (long) v.sv[s].startDelay + v.sv[s].uniDelay[u]);
            maxDelay = juce::jmax(maxDelay, d);
        }
        if (! v.isKey || v.keyOff >= 0) veEnd += maxDelay;   // still-held key voices already run open-ended
        bool finished = false;

        for (int i = 0; i < numSamples; ++i)
        {
            const long tv = v.voiceSamples;   // voice age (shared); each slot shadows it below by its own onset delay
            // [2026-07-13 21:20] MPE expression slews (~3 ms) + the per-note BEND multiplier.
            double bendMul = 1.0;
            if (v.isKey)
            {
                v.pressCur += wSmK * (v.pressTgt - v.pressCur);
                v.slideCur += wSmK * (v.slideTgt - v.slideCur);
                v.bendCur  += wSmK * (v.bendTgt  - v.bendCur);
                if (std::abs(v.bendCur) > 1.0e-4f) bendMul = std::exp2((double) v.bendCur / 12.0);
            }
            const float kg = v.killing ? v.killGain : 1.0f;
            if (v.glideRemain > 0)   // 303 slide: per-sample pitch glide toward this step's pitch
            {
                v.voicePitch += v.glideStep; --v.glideRemain;
                vStepMul  = std::pow(2.0, (double) v.voicePitch / 12.0);
                vPitchMul = vStepMul * chanMul;
            }
            float vEnv = 0.0f, mixL = 0.0f, mixR = 0.0f;

            for (int s = 0; s < NUM_SLOTS; ++s)
            {
                const SC& c = *cs[s];   // PER-VOICE modulated config (falls back to the block config sc[s])
                if (c.engine < 0) continue;
                SlotVoice& sv = v.sv[s];
                // HUMANIZE: this slot's whole onset is pushed back by startDelay (0 = on time). Skip it
                // (frozen, silent) until then; afterwards run against a slot-LOCAL age `t` (shadows tv) so
                // its envelope + phases start from zero at the delayed onset. startDelay 0 = bit-identical.
                if (tv < sv.startDelay) continue;
                const long t = tv - sv.startDelay;
                // =====================================================================================
                // AUDIO-RATE MODULATION [2026-07-13 19:57]: evaluate the compiled route program EVERY
                // SAMPLE for the hot targets. Sources: LFOs at THIS sample's phase; Amp Env = the
                // previous sample's env (1-sample delay - no duplication of the engine's env logic,
                // and a per-sample upgrade over the old previous-BLOCK read); Mod Env evaluated live;
                // Vel / Note / Random = per-note constants; Step Mod A/B + Mod Wheel = the channel's
                // block-linear ramps. arN == 0 = zero cost = byte-identical old path.
                // (Supersedes the same-day de-zipper bank: no smoothing needed when the source itself
                // is smooth. The env-time LATCH + FORMANT glide + DRIVE dry-blend remain.)
                // =====================================================================================
                float mDrv = c.fxDrive, mRing = c.ring, mSub = c.subAmt, mPunch = c.punch,
                      mWarp = c.oscWarp, mFm = c.fmIndex, mWt = c.wtPosOfs;
                double mRingHzMul = 1.0; float mPanL = c.panL, mPanR = c.panR;
                double arPitchMul = 1.0; float arVolG = 1.0f;
                double arGmMul[2] = { 1.0, 1.0 }; double arKTgt[2] = { -1.0, -1.0 };
                if (c.arN > 0)
                {
                    float sums[15] = {};
                    float cache[24]; uint32_t got = 0;
                    auto S = [&](int sid) -> float {
                        sid = juce::jlimit(0, 23, sid);
                        if (got & (1u << sid)) return cache[sid];
                        float val = 0.0f;
                        switch (sid) {
                            case MSVel:    val = juce::jlimit(0.0f, 1.0f, v.velGain); break;
                            case MSNote:   val = juce::jlimit(-1.0f, 1.0f,
                                               (((v.keyNote >= 0) ? (float) v.keyNote : 60.0f + v.voicePitch) - 60.0f) / 24.0f); break;
                            case MSAmpEnv: val = sv.lastEnv; break;
                            case MSRandom: val = sv.modRand; break;
                            case MSModEnv: val = juce::jlimit(0.0f, 1.0f,
                                               keyAdsr(tv, v.isKey ? v.keyOff : v.gateLen, c.mEA, c.mEH, c.mED, c.mES, c.mER)); break;
                            case MSModLfo: { const double pt = c.modLfoPh0 + c.modLfoInc * (double) i;
                                             const uint32_t cy = (uint32_t) juce::jmax(0.0, pt / (2.0 * kPi));
                                             val = lfoShapeVal(c.mLfoShape, pt - (double) cy * 2.0 * kPi, cy); } break;
                            case MSPressure: val = v.pressCur; break;
                            case MSSlide:    val = v.slideCur; break;
                            case MSStepModA: val = arA0 + arAStep * (float) i; break;
                            case MSStepModB: val = arB0 + arBStep * (float) i; break;
                            case MSModWheel: val = arW0 + arWStep * (float) i; break;
                            case MSPitchWheel: val = arP0 + arPStep * (float) i; break;
                            default:
                                if (sid >= MSLfoFilt && sid <= MSLfoWave) {
                                    const int d3 = sid - MSLfoFilt;
                                    double ph; uint32_t cyc;
                                    if (c.lfoFreeOn[d3]) { const double pt = c.lfoFreePh[d3] + c.lfoFreeInc[d3] * (double) i;
                                                           cyc = (uint32_t)(pt / (2.0 * kPi)); ph = pt - (double) cyc * 2.0 * kPi; }
                                    else                 { ph = sv.lfoPhase[d3]; cyc = sv.lfoCyc[d3]; }
                                    val = c.lfoAmt[d3] * lfoShapeVal(c.lfoShape[d3], ph, cyc, c.lfoCurve[d3]);
                                }
                                break;
                        }
                        cache[sid] = val; got |= (1u << sid); return val;
                    };
                    for (int r2 = 0; r2 < c.arN; ++r2)
                    {
                        float sval = S(c.arSrc[r2]);
                        if (c.arLagK[r2] > 0.0f)   // [2026-07-14 01:33] per-route LAG (per-sample slew, reset at the hit)
                        { sv.arLag[r2] += c.arLagK[r2] * (sval - sv.arLag[r2]); sval = sv.arLag[r2]; }
                        if (c.arCv[r2] != nullptr)   // [2026-07-14 00:03] per-route REMAP (drawn transfer curve)
                            sval = c.arBi[r2] ? modCurveLut(c.arCv[r2], sval * 0.5f + 0.5f) * 2.0f - 1.0f
                                              : modCurveLut(c.arCv[r2], sval);
                        sums[c.arTgt[r2]] += c.arAmt[r2] * sval;
                    }
                    if (sums[0] != 0.0f) arPitchMul = std::exp2((double) juce::jlimit(-4.0f, 4.0f, sums[0]));   // +-1 oct per full route
                    if (sums[1] != 0.0f) arVolG     = juce::jlimit(0.0f, 2.0f, 1.0f + sums[1]);                 // AM gain
                    if (sums[2] != 0.0f) arGmMul[0] = std::exp2((double) juce::jlimit(-8.0f, 8.0f, 4.0f * sums[2]));   // +-4 oct cutoff
                    if (sums[3] != 0.0f) arGmMul[1] = std::exp2((double) juce::jlimit(-8.0f, 8.0f, 4.0f * sums[3]));
                    for (int fi2 = 0; fi2 < 2; ++fi2)
                        if (sums[4 + fi2] != 0.0f) {   // per-sample RESO: K recomputed from the base reso + bell A
                            const auto& fc2 = c.filt[fi2];
                            const double q2 = juce::jmax(0.15, juce::jlimit(0.4, 12.0, (double) fc2.resoB + (double) sums[4 + fi2] * 11.6));
                            arKTgt[fi2] = 1.0 / (q2 * fc2.bellA);
                        }
                    mDrv   = juce::jlimit(0.0f, 1.0f, mDrv   + sums[6]);
                    mRing  = juce::jlimit(0.0f, 1.0f, mRing  + sums[7]);
                    mSub   = juce::jlimit(0.0f, 1.0f, mSub   + sums[8]);
                    mPunch = juce::jlimit(-1.0f, 1.0f, mPunch + sums[9] * 2.0f);
                    mWarp  = juce::jlimit(0.0f, 1.0f, mWarp  + sums[10]);
                    mWt    = mWt + sums[11];                                       // clamped where consumed
                    mFm    = juce::jlimit(0.0f, 24.0f, mFm + sums[12] * 12.0f);    // fmDepth 0..1 -> index x12
                    if (sums[13] != 0.0f) mRingHzMul = std::exp2((double) juce::jlimit(-6.0f, 6.0f, sums[13] * 3.0f));   // ring carrier +-3 oct
                    if (sums[14] != 0.0f) {   // SLOT PAN: equal-power recomputed per sample (Note/Random/Velocity -> per-hit placement)
                        const float pp2 = juce::jlimit(-1.0f, 1.0f, c.panBase + sums[14] * 2.0f);
                        mPanL = std::sqrt(1.0f - pp2); mPanR = std::sqrt(1.0f + pp2);
                    }
                }
                float sig = 0.0f, sL = 0.0f, sR = 0.0f, env = 0.0f;
                bool  stereo = false;

                // 4-point pitch envelope -> frequency multiplier (1.0 when unused). Applies to every engine.
                double pe3Mul = 1.0;
                if (c.pEnvOn) {
                    const float frac = juce::jlimit(0.0f, 1.0f, (float)((double) t / c.voiceLenSamp));
                    pe3Mul *= std::pow(2.0, (double) pitchEnv4(frac, c.pEnvP, c.pEnvT) / 12.0);
                }
                // KEYS: skip slots the keyboard can't play; re-tune the rest from their own base
                // Freq to the pressed note (keySemis rides pe3Mul, so it reaches every pitched
                // engine including the Modal strike-tuning and the KS varispeed read).
                if (sv.keyMute) continue;
                if (sv.keySemis != 0.0f)
                    pe3Mul *= std::pow(2.0, (double) sv.keySemis / 12.0);
                const double noteMul = vPitchMul;   // per-step/roll/slide/keys pitch (env/vibrato/LFO ride pe3Mul)
                // Pitch multiplier BEFORE the audio-rate LFO (used as the KEY-mode LFO / tracked-Ring
                // reference so pitch-FM doesn't feed its own rate = stays bounded).
                const double pitchPreLfo = noteMul * pe3Mul;
                // === AUDIO-RATE PITCH [2026-07-13 19:57]: the per-sample route program's pitch product.
                //     ANY source -> Pitch is real per-sample FM now (fast LFO = sidebands, Mod Env =
                //     smooth glide, Step Mod = ramped steps) - not just the old LFO-only special case. ===
                if (arPitchMul != 1.0) pe3Mul *= arPitchMul;
                if (bendMul != 1.0)    pe3Mul *= bendMul;   // MPE per-note pitch bend (slewed, semitone-ranged)

                switch (c.engine)
                {
                    case SrcOsc: {
                        // ONE envelope rule (v1.2.0): the decay settles at SUSTAIN while a GATE is
                        // open - a held KEYS note or the step's Note Length - then RELEASE. With
                        // sustain at 0 (all factory sounds) this is exactly the old behaviour:
                        // pure AHD, and Note Length = the decay-rescale (gateDec).
                        if (v.isKey || (v.gateLen > 0 && c.sustain > 0.01f))
                            env = keyAdsr(t, v.isKey ? v.keyOff : v.gateLen, c.atk, c.hold, c.dec, c.sustain, c.release);
                        else
                            env = ahdsEnv(t, c.atk, c.hold, sv.gateDec > 0.0f ? sv.gateDec : c.dec, c.sustain, c.release);
                        double sSemis = (c.oscPEnvAmt != 0.0f) ? (double) c.oscPEnvAmt * pitchEnvShape(t, c.oscPEnvTime, c.oscPOffset) : 0.0;
                        double freq = c.oscFreq * std::pow(2.0, sSemis / 12.0) * noteMul * c.oscVibFac * pe3Mul;
                        // SCALE snaps the played note into the key. The FM modulator + sub-osc are SHARED across the
                        // unison/chord voices, so tune them to the SNAPPED ROOT (voice 0's offset) - else an off-scale
                        // note (C#) keeps its FM/sub at C# while the carriers snap to C-E-G, so C and C# sound DIFFERENT
                        // even though both voice a C chord. (In CHORD the root offset is 0 = no change = old behaviour.)
                        const double fmRootMul = c.scaleOn ? std::pow(2.0, (double) sv.uniSemis[0] / 12.0) : 1.0;
                        // Single "Wave" selector (the From/To over-the-note morph was retired - it sounded harsh).
                        const float pos = (float) c.oscShape;
                        // Merged FM: a sine modulator bends the carrier phase. Depth (fmIndex) 0 => no
                        // modulation => identical to pure (band-limited) analog. Modulator tracks the
                        // carrier freq * ratio so it stays in tune under vibrato / pitch env.
                        float fmAdd = 0.0f;
                        if (mFm > 0.0001f) {
                            const float modOut = (float) std::sin(sv.fmMod + c.fmFeedback * 6.0f * sv.fmFbState);
                            sv.fmFbState = 0.5f * (sv.fmFbState + modOut);
                            // ANTI-ALIAS: FM sidebands ignore Nyquist - roll the index off as the
                            // effective carrier climbs (1x below ~1.8 kHz = drums/bass untouched;
                            // C7/C8 leads lose the inharmonic fizz instead of aliasing).
                            const float fmAtt = (float) juce::jlimit(0.35, 1.0, 1800.0 / juce::jmax(1800.0, freq * fmRootMul));
                            // Env-follow: the FM index rides the amp envelope (classic FM drum -
                            // bright modulated attack that mellows to the plain carrier as it decays).
                            fmAdd = (c.fmEnvF ? mFm * env : mFm) * fmAtt * modOut;
                            sv.fmMod += 2.0 * kPi * freq * fmRootMul * (double) c.fmRatio / sr;
                            if (sv.fmMod > 2.0 * kPi) sv.fmMod -= 2.0 * kPi;
                        }
                        const bool fmActive = mFm > 0.0001f;
                        float wsum = 0.0f, wL = 0.0f, wR = 0.0f;
                        const bool sprd = c.uniSpread > 0.001f && c.uniVoices > 1;   // STEREO WIDTH active
                        const int totalV = c.uniVoices + (c.uniCenter ? 1 : 0);   // +1 dry/centre voice
                        const float strumFade = juce::jmax(1.0f, 0.002f * (float) sr);   // 2 ms click-free fade-in per strummed note
                        // [2026-07-14 02:50] CONTINUOUS COUNT-MORPH: when a route targets Unison Count,
                        // the loop covers ALL possible voices; each fades toward (u < N ? 1 : 0) over
                        // ~3 ms and the makeup gain follows the EFFECTIVE count - joins/leaves cannot
                        // step or pop. Unrouted = the exact old loop (bound totalV, fades untouched).
                        const bool cm = c.uniCntMod;
                        const int loopV = cm ? UNI_MAX + (c.uniCenter ? 1 : 0) : totalV;
                        float effN2 = 0.0f;
                        for (int u = 0; u < loopV; ++u) {
                            const bool centreVoice = (c.uniCenter && u == (cm ? UNI_MAX : c.uniVoices));   // the extra undetuned voice
                            float cmFade = 1.0f;
                            if (cm)
                            {
                                float& fdr = sv.uniFade[juce::jmin(u, UNI_MAX)];
                                const float tgtF = (centreVoice || u < c.uniVoices) ? 1.0f : 0.0f;
                                fdr += wSmK * (tgtF - fdr);
                                if (tgtF <= 0.0f && fdr < 1.0e-3f) continue;   // fully faded out: skip the maths
                                cmFade = fdr; effN2 += fdr * fdr;
                            }
                            // STRUM: chord voice u fades in from its own onset (uniDelay). uniDelay 0 => gate 1 => identical.
                            const float gU = (centreVoice || u >= UNI_MAX || sv.uniDelay[u] <= 0) ? 1.0f
                                            : juce::jlimit(0.0f, 1.0f, (float)(t - sv.uniDelay[u]) / strumFade);
                            float sp = 0.0f;   // mode: 0=symmetric (+/-), 1=up (all sharp), 2=down (all flat)
                            if (! centreVoice && c.uniVoices > 1) {
                                const float frac = (float) u / (float)(c.uniVoices - 1);   // 0..1
                                sp = (c.uniMode == 1) ? frac : (c.uniMode == 2) ? -frac : (2.0f * frac - 1.0f);
                            }
                            double det = std::pow(2.0, (double)(sp * c.uniCents) / 1200.0);
                            // CHORD mode: each unison voice becomes a chord note (root/third/fifth/..);
                            // detune still micro-spreads each note. The centre voice stays on the root.
                            if (c.scaleOn && ! centreVoice)
                            {   // SCALE: per-note diatonic offset; guitar voicings mark missing
                                if (sv.uniSemis[u] < -90.0f) continue;   // strings with a sentinel
                                det *= std::pow(2.0, (double) sv.uniSemis[u] / 12.0);
                            }
                            det *= (double) sv.driftMul[juce::jmin(u, UNI_MAX)] * (double) sv.driftWobMul;   // DRIFT (1 = off)
                            const float dt = (float)(freq * det / sr);   // cycles/sample for PolyBLEP
                            // FM active -> plain morphWave (matches the old FM engine exactly, no BLEP under
                            // phase modulation); FM off -> band-limited morphWaveBL (the clean analog path).
                            const double wph = sv.uniPhase[u];   // (Warp is now an output wavefold, applied below)
                            float vout;
                            if (c.wtFrm[0] != nullptr)
                            {   // ADDITIVE WAVETABLE: read the two frames around the current position and
                                // crossfade (FM phase-mod still applies; band-limiting matches the factory
                                // bank shapes: 32 harmonics + the 2x oversampling).
                                const double phx = fmActive ? wph + fmAdd : wph;
                                const float ph01 = (float)(phx / (2.0 * kPi) - std::floor(phx / (2.0 * kPi)));
                                const float spx = ph01 * (float) ADD_TBL;
                                const int j0 = ((int) spx) & (ADD_TBL - 1), j1 = (j0 + 1) & (ADD_TBL - 1);
                                const float fr = spx - (float) (int) spx;
                                // position: the per-note PIECEWISE glide (per-leg times, 0 = hold)
                                // OR the static addPos, +/- the WAVE LFO (dest 3); clamped.
                                float wtp;
                                if (c.wtGlide)
                                {
                                    float tf = (float) t;
                                    if (c.wtLoop)
                                    {   // LOOP: travel out then back (ping-pong = no snap), forever
                                        const float L2 = 2.0f * c.wtLoopEnd;
                                        tf = std::fmod(tf, L2);
                                        if (tf > c.wtLoopEnd) tf = L2 - tf;
                                    }
                                    if      (tf < c.wtT1) wtp = tf * c.wtInv0;
                                    else if (tf < c.wtT2) wtp = 1.0f / 3.0f + (tf - c.wtT1) * c.wtInv1;
                                    else if (tf < c.wtT3) wtp = 2.0f / 3.0f + (tf - c.wtT2) * c.wtInv2;
                                    else                  wtp = 1.0f;
                                }
                                else wtp = c.wtPos;
                                // matrix WAVE-position modulation is a per-block offset added HERE (works on
                                // top of the glide clock too, not just the static position).
                                wtp = juce::jlimit(0.0f, 1.0f, wtp + mWt);
                                sv.wtPosCur = wtp;   // live position read-out (UI Position strip)
                                wtp *= (float)(ADD_FRAMES - 1);
                                const int   f0 = juce::jmin((int) wtp, ADD_FRAMES - 2);
                                const float fmix = wtp - (float) f0;
                                const float* tA = c.wtFrm[f0];
                                const float* tB = c.wtFrm[f0 + 1];
                                const float vA = tA[j0] + (tA[j1] - tA[j0]) * fr;
                                const float vB = tB[j0] + (tB[j1] - tB[j0]) * fr;
                                vout = gU * (vA + (vB - vA) * fmix);
                            }
                            else
                                vout = gU * (fmActive ? morphWave(wph + fmAdd, pos)
                                                      : morphWaveBL(wph, pos, dt));
                            if (cm) vout *= cmFade;   // count-morph: this voice's fade
                            wsum += vout;
                            if (sprd) { const int pu = juce::jmin(u, UNI_MAX);
                                        wL += vout * c.uniPanL[pu]; wR += vout * c.uniPanR[pu]; }
                            sv.uniPhase[u] += 2.0 * kPi * freq * det / sr;
                            if (sv.uniPhase[u] > 2.0 * kPi) sv.uniPhase[u] -= 2.0 * kPi;
                        }
                        const float ugain = cm ? 1.0f / std::sqrt(juce::jmax(1.0f, effN2)) : c.uniGain;   // gain follows the EFFECTIVE count
                        float oo = wsum * ugain;
                        float ooL = sprd ? wL * ugain : 0.0f, ooR = sprd ? wR * ugain : 0.0f;
                        if (mWarp > 0.001f) {                       // WARP = one-way wavefold (adds harmonics/grit)
                            // [2026-07-13 20:20] ADAA'd fold (the naked sin() fold aliased on bright waves).
                            oo = warpFoldAdaa(oo, mWarp, sv.warpU[0], sv.warpW[0]);
                            if (sprd) { ooL = warpFoldAdaa(ooL, mWarp, sv.warpU[1], sv.warpW[1]); ooR = warpFoldAdaa(ooR, mWarp, sv.warpU[2], sv.warpW[2]); }
                        } else {
                            // keep the ADAA state FRESH while bypassed: a modulated warp sweeping
                            // through zero would otherwise re-enter with a stale x1 = the quotient
                            // averages the fold over half an LFO period = a one-sample dropout.
                            sv.warpU[0] = oo; sv.warpW[0] = mWarp;
                            if (sprd) { sv.warpU[1] = ooL; sv.warpW[1] = mWarp; sv.warpU[2] = ooR; sv.warpW[2] = mWarp; }
                        }
                        if (c.fmSub > 0.001f) {                         // sub-oscillator an octave down (stays CENTRED)
                            const float sub = (float) std::sin(sv.fmSubPhase) * c.fmSub;
                            oo = oo * (1.0f - 0.4f * c.fmSub) + sub;
                            if (sprd) { ooL = ooL * (1.0f - 0.4f * c.fmSub) + sub * 0.7071f;
                                        ooR = ooR * (1.0f - 0.4f * c.fmSub) + sub * 0.7071f; }
                            sv.fmSubPhase += 2.0 * kPi * freq * fmRootMul * 0.5 / sr;
                            if (sv.fmSubPhase > 2.0 * kPi) sv.fmSubPhase -= 2.0 * kPi;
                        }
                        sig = oo * env;   // Analog + FM only (mono path; Width builds sL/sR alongside)
                        if (sprd) { sL = ooL * env; sR = ooR * env; stereo = true; }   // STEREO WIDTH
                        sv.sinePhase = sv.uniPhase[0];
                        break; }
                    case SrcNoise: {
                        // FULL envelope contract, exactly like SrcOsc: a held KEY or a piano-roll
                        // gate sustains + releases (v.isKey was missing here = noise layers could
                        // not sustain or release on keys/roll - the broken "wind" breath).
                        if (v.isKey || (v.gateLen > 0 && c.sustain > 0.01f))
                            env = keyAdsr(t, v.isKey ? v.keyOff : v.gateLen, c.atk, c.hold, c.dec, c.sustain, c.release);
                        else
                            env = ahdsEnv(t, c.atk, c.hold, sv.gateDec > 0.0f ? sv.gateDec : c.dec, c.sustain, c.release);
                        float w = whiteNoise(sv.noiseState), col;
                        switch (c.noiseType) {
                            case 1:
                                sv.pinkB[0] = 0.99765f * sv.pinkB[0] + w * 0.0990460f;
                                sv.pinkB[1] = 0.96300f * sv.pinkB[1] + w * 0.2965164f;
                                sv.pinkB[2] = 0.57000f * sv.pinkB[2] + w * 1.0526913f;
                                col = (sv.pinkB[0] + sv.pinkB[1] + sv.pinkB[2] + w * 0.1848f) * 0.35f; break;
                            case 2:
                                sv.brownState = juce::jlimit(-1.0f, 1.0f, sv.brownState + 0.02f * w);
                                col = sv.brownState * 3.5f; break;
                            case 3: {   // GREY = white through the mid-scoop biquad (inverse equal-loudness)
                                const float y = c.greyB0 * w + sv.greyZ1;
                                sv.greyZ1 = c.greyB1 * w - c.greyA1 * y + sv.greyZ2;
                                sv.greyZ2 = c.greyB2 * w - c.greyA2 * y;
                                col = y * 1.4f; break; }   // makeup for the dip
                            case 4: { float hi = w - sv.prevWhite; sv.prevWhite = w; col = hi * 0.7f; break; }
                            default: col = w; break;
                        }
                        float nOut = c.noiseBP ? sv.noiseBP.process(col, 0) * 3.0f : col;
                        if (c.noiseCrackle > 0.001f) {                 // granular dust / vinyl crackle on top
                            const float r = whiteNoise(sv.noiseState);
                            nOut *= (1.0f - c.noiseCrackle * 0.45f);   // duck the noise bed so the pops stand OUT
                            if (std::abs(r) > 1.0f - c.noiseCrackle * c.noiseCrackle * 0.28f)   // denser as crackle rises
                                nOut += (r < 0.0f ? -1.0f : 1.0f) * (0.7f + 0.7f * std::abs(whiteNoise(sv.noiseState)));
                        }
                        if (c.noiseDrive > 0.001f)                     // saturation -> denser, grittier, more aggressive
                            nOut = std::tanh(nOut * (1.0f + c.noiseDrive * 10.0f)) * (1.0f + c.noiseDrive * 0.5f);
                        sig = nOut * env;
                        break; }
                    case SrcFM: {
                        env = ahdsEnv(t, c.atk, c.hold, sv.gateDec > 0.0f ? sv.gateDec : c.dec, c.sustain, c.release);
                        double fmPMul = (c.fmPEnvAmt != 0.0f) ? std::pow(2.0, (double) c.fmPEnvAmt * pitchEnvShape(t, c.fmPEnvTime, c.fmPOffset) / 12.0) : 1.0;
                        float modOut = (float) std::sin(sv.fmMod + c.fmFeedback * 6.0f * sv.fmFbState);
                        sv.fmFbState = 0.5f * (sv.fmFbState + modOut);
                        // Carrier waveform morphs Wave A -> Wave B over the note (static when equal).
                        float cpos = (float) c.oscShape;
                        if (c.oscShapeB != c.oscShape) cpos += ((float) c.oscShapeB - (float) c.oscShape) * (1.0f - decayCurve(t, juce::jmax(0.02f, c.dec)));
                        float fm = morphWave(sv.fmCarrier + mFm * modOut, cpos);
                        if (c.fmSub > 0.001f) {
                            fm = fm * (1.0f - 0.4f * c.fmSub) + (float) std::sin(sv.fmSubPhase) * c.fmSub;
                            sv.fmSubPhase += 2.0 * kPi * c.fmCarrierF * 0.5 * fmPMul * noteMul * pe3Mul / sr;
                            if (sv.fmSubPhase > 2.0 * kPi) sv.fmSubPhase -= 2.0 * kPi;
                        }
                        sv.fmCarrier += 2.0 * kPi * c.fmCarrierF * fmPMul * noteMul * pe3Mul / sr;
                        sv.fmMod     += 2.0 * kPi * c.fmModF * fmPMul * noteMul * pe3Mul / sr;
                        if (sv.fmCarrier > 2.0 * kPi) sv.fmCarrier -= 2.0 * kPi;
                        if (sv.fmMod     > 2.0 * kPi) sv.fmMod     -= 2.0 * kPi;
                        sig = fm * env;
                        break; }
                    case SrcPhys: {
                        if (! ksOk) break;   // KS line not allocated (engine assigned without ensureKsBuffers - silent, no crash)
                        // Same ONE-rule envelope as Osc/Noise: sustain holds while a gate is open
                        // (held key or Note Length), then release. Sustain 0 = pure AHD (factory).
                        // A KEY always gates (even at sustain 0: held = the same AHD, key-up = the
                        // RELEASE cut - a faithful synth note). Sequencer Length gates need sustain.
                        if (v.isKey || (v.gateLen > 0 && c.sustain > 0.01f))
                            env = keyAdsr(t, v.isKey ? v.keyOff : v.gateLen, c.atk, c.hold, c.dec, c.sustain, c.release);
                        else
                            env = ahdsEnv(t, c.atk, c.hold, sv.gateDec > 0.0f ? sv.gateDec : c.dec, c.sustain, c.release);
                        double pSemis = (c.physPEnvAmt != 0.0f) ? (double) c.physPEnvAmt * pitchEnvShape(t, c.physPEnvTime, c.physPOffset) : 0.0;
                        if (c.pm->pitchDrop != 0.0f) pSemis += (double) c.pm->pitchDrop * std::exp(-(float) t * 3.0f / (0.06f * (float) sr));
                        double fBase = c.physBaseF * std::pow(2.0, pSemis / 12.0) * noteMul * c.physVibFac * pe3Mul;
                        // Gate open (held key / Note Length) + sustain -> HOLD the string: near-lossless
                        // feedback AND a nearly-open loop filter (the LP is what actually drains the loop -
                        // feedback alone still lost ~18 dB in 2 s). Gate closed / sustain 0 = bit-identical.
                        const bool ksHold = c.sustain > 0.01f
                                         && ((v.isKey && v.keyOff < 0) || (v.gateLen > 0 && t < v.gateLen));
                        // HOLD keeps the LEVEL (ksFb below), but only PARTLY opens the loop LP now
                        // (35% of the damping stays) - a held dark material settles into a darker
                        // singing tone instead of every material converging on the same bright ebow.
                        const float lpC = ksHold ? 1.0f - (1.0f - c.ksLpC) * 0.35f : c.ksLpC;
                        // String feedback. Normally the Decay-derived ksFb (the string decays on its own).
                        // ksHold sustains it (bowed/ebow); the STRIKE window swells a slow attack to full.
                        float ksFb = ksFbGate[s] > 0.0f ? ksFbGate[s] : c.ksFb;   // Length rescales the string's own decay too
                        if (ksHold) ksFb = juce::jmax(ksFb, 0.99999f);
                        const long aS = (long) (c.atk * (float) sr), hS = (long) (c.hold * (float) sr);
                        if (t < aS + hS && (c.atk > 0.005f || c.hold > 0.0001f)) ksFb = 0.9997f;   // instant plucks untouched (factory-safe)
                        // UNISON/CHORD: sum nStr real strings, each tuned to its chord/detune pitch and
                        // running its own KS delay line (offset k*KS_MAX). nStr==1 = one string = the
                        // old single-line KS, bit-identical (uniMul[0] == 1). RMS make-up keeps loudness.
                        const int nStr = juce::jlimit(1, KS_UNI, c.uniVoices);
                        const float uniGain = 1.0f / std::sqrt((float) nStr);
                        float acc = 0.0f;
                        for (int k = 0; k < nStr; ++k)
                        {
                            // STRUM: hold string k silent until its onset. Its excitation stays pristine in
                            // the KS line, so it starts as a FRESH pluck, not a decayed one. uniDelay 0 = identical.
                            if (k < UNI_MAX && t < sv.uniDelay[k]) continue;
                            if (c.scaleOn && sv.uniSemis[k] < -90.0f) continue;   // guitar voicing: no such string
                            const int base = k * KS_MAX;
                            const double f = fBase * (c.scaleOn                     // SCALE: per-string diatonic offset + detune spread
                                ? std::pow(2.0, (double) sv.uniSemis[k] / 12.0
                                           + (nStr > 1 ? (2.0 * (double) k / (double)(nStr - 1) - 1.0) : 0.0) * (double) c.uniCents / 1200.0)
                                : c.uniMul[k])
                                * (double) sv.driftMul[juce::jmin(k, UNI_MAX)] * (double) sv.driftWobMul;   // DRIFT (1 = off)
                            double L = juce::jlimit(2.0, (double)(KS_MAX - 2), sr / juce::jmax(20.0, f) - (double) c.ksApComp);
                            double rp = sv.ksWrite[k] - L; while (rp < 0.0) rp += (double) KS_MAX;
                            const int ri = (int) rp; const float fr = (float)(rp - ri);
                            const float k0 = sv.ksBuf[base + ri], k1 = sv.ksBuf[base + ((ri + 1) % KS_MAX)];
                            float y = k0 + fr * (k1 - k0);
                            sv.ksLp[k] += lpC * (y - sv.ksLp[k]);
                            float ss = sv.ksLp[k];
                            for (int st = 0; st < c.ksApN; ++st) { float yy = c.ksApC * ss + sv.ksApSt[k][st]; sv.ksApSt[k][st] = ss - c.ksApC * yy; ss = yy; }
                            for (int st = 0; st < c.ksApN2 && c.ksApN + st < 12; ++st)
                            { const int q = c.ksApN + st;   // MATERIAL chain (stacked after stiffness)
                              float yy = c.ksApC2 * ss + sv.ksApSt[k][q]; sv.ksApSt[k][q] = ss - c.ksApC2 * yy; ss = yy; }
                            const int wi = (int) sv.ksWrite[k] % KS_MAX;
                            sv.ksBuf[base + wi] = juce::jlimit(-2.5f, 2.5f, ss * ksFb); sv.ksWrite[k] = wi + 1;   // KS write clamp (anti-gunshot)
                            acc += ss;
                        }
                        sig = (acc * 1.4f * uniGain) * env;
                        break; }
                    case SrcSample: {
                        // Default: no envelope - play at full level with short anti-click fades at the
                        // very start and just before the (trimmed) end. With the OPT-IN amp envelope on
                        // (smpEnvOn), the AHD env shapes the sample too (fade-in / tame a long tail).
                        // A per-step LENGTH gate applies the gated env even when the opt-in sample env is off,
                        // so Length can shorten a sample too (holds full, then releases at the gate).
                        env = (c.smpEnv || sv.gateDec > 0.0f) ? ahdsEnv(t, c.atk, c.hold, sv.gateDec > 0.0f ? sv.gateDec : c.dec, c.sustain, c.release) : 1.0f;
                        const double head = sv.smpHead;
                        const long fin = (long) (0.003 * sr);
                        if (fin > 0 && t < fin) env *= (float) t / (float) fin;
                        // Fade the last ~10 ms by POSITION (source frames left before the end), so a pitched-DOWN /
                        // varispeed sample reads ALL the way to its end instead of being cut at the natural-speed length.
                        const double foutSrc = juce::jmax(1.0, 0.010 * sr * juce::jmax(0.05, c.speed));
                        const double toEnd   = (double) c.regHi - head;
                        if (toEnd < foutSrc) env *= juce::jmax(0.0f, (float)(toEnd / foutSrc));
                        const int idx = (int) head;
                        // Read from THIS slot's own buffer + trim region (per-slot samples).
                        const juce::AudioBuffer<float>* sbuf = c.buf;
                        if (sbuf == nullptr || sbuf->getNumSamples() == 0) { sv.smpHead += c.speed; break; }
                        const int sLen = sbuf->getNumSamples();
                        const int sCh  = sbuf->getNumChannels();
                        if (idx >= c.regLo && idx < juce::jmin(c.regHi, sLen)) {
                            const float fr = (float)(head - idx);
                            int rIdx = idx;
                            const int dir = c.reverse ? -1 : 1;          // playback direction
                            if (c.reverse) rIdx = juce::jlimit(0, sLen - 1, c.regLo + (c.regHi - 1 - idx));
                            const auto* srcA = sbuf->getReadPointer(0);
                            const auto* srcB = sbuf->getReadPointer(juce::jmin(1, sCh - 1));
                            // [2026-07-13 20:45] 8-tap windowed-SINC interpolation (replaced 4-point Hermite).
                            auto at = [&](const float* s, int k){ return s[juce::jlimit(0, sLen - 1, rIdx + dir * k)]; };
                            const float* hp = gSincTable.t[juce::jlimit(0, SincTable::PHASES,
                                                                        (int) std::lround((double) fr * SincTable::PHASES))];
                            float accL = 0.0f, accR = 0.0f;
                            for (int j = 0; j < SincTable::TAPS; ++j)
                            { const float wj = hp[j]; accL += wj * at(srcA, j - (SincTable::HALF - 1)); accR += wj * at(srcB, j - (SincTable::HALF - 1)); }
                            sL = accL; sR = accR;
                            if (c.crushStep > 0.0f) { sL = std::round(sL / c.crushStep) * c.crushStep; sR = std::round(sR / c.crushStep) * c.crushStep; }
                        }
                        sL *= env * c.smpGain; sR *= env * c.smpGain;   // sample output boost
                        // Static pitch (channel + slot) is baked into the buffer (SoundTouch). The pitch
                        // ENVELOPE (sample smpPEnv + the 4-dot pe3Mul), pitch LFO and vibrato ALWAYS apply.
                        // KEEP PITCH only blocks the NOTE pitch (keys + per-step/draw) - applied AFTER the
                        // pitch envelope - so recording/step pitch can't detune the sample. Off = full varispeed.
                        double smpSemis = 0.0;
                        if (c.smpPEnvAmt != 0.0f) smpSemis += (double) c.smpPEnvAmt * pitchEnvShape(t, c.smpPEnvTime, c.smpPOffset);
                        const double keyMul  = (sv.keySemis != 0.0f) ? std::pow(2.0, (double) sv.keySemis / 12.0) : 1.0;
                        const double envPart = std::pow(2.0, smpSemis / 12.0) * (double) c.oscVibFac * (pe3Mul / keyMul);   // env + vib + LFO (no note pitch)
                        const double advance = c.smpPreserve ? (c.speed * envPart)                        // note pitch blocked
                                                             : (c.speed * envPart * keyMul * vStepMul);   // + keys + step/draw
                        sv.smpHead += advance / (double) engineOS;
                        stereo = true;
                        break; }
                    // SrcSynth / SrcWave render REMOVED (v1.2.x) - engines retired, no factory usage.
                    case SrcGrain: {
                        // GRANULAR: a pool of short windowed reads of the source (the slot's
                        // sample, else the pre-rendered wave-journey table). DETERMINISTIC per
                        // hit: the grain dice roll from sv.noiseState (identical-hits rule).
                        env = (v.isKey || (v.gateLen > 0 && c.sustain > 0.01f))
                            ? keyAdsr(t, v.isKey ? v.keyOff : v.gateLen, c.atk, c.hold, c.dec, c.sustain, c.release)
                            : ahdsEnv(t, c.atk, c.hold, sv.gateDec > 0.0f ? sv.gateDec : c.dec, c.sustain, c.release);
                        const bool smp = c.grBuf != nullptr;
                        if (! smp && c.grTbl == nullptr) break;    // table still building (message thread)
                        sv.grAcc += c.grSpawnPerSamp;              // spawn clock
                        if (sv.grAcc >= 1.0f)
                        {
                            sv.grAcc -= 1.0f;
                            // [2026-07-16] GLIDE JOURNEY: with the draw window's A>B/B>C/C>D times set,
                            // the grain POSITION travels the source over the note's life (Loop =
                            // ping-pong) - the oscillator's exact piecewise clock. The Position knob is
                            // overridden while glide is on (the osc convention); Spray + the WAVE LFO
                            // still add on top. No glide = the knob, bit-identical to before.
                            float grBase = c.grPos;
                            if (c.wtGlide)
                            {
                                float tf = (float) t;
                                if (c.wtLoop) { const float L2 = 2.0f * c.wtLoopEnd;
                                                tf = std::fmod(tf, L2); if (tf > c.wtLoopEnd) tf = L2 - tf; }
                                if      (tf < c.wtT1) grBase = tf * c.wtInv0;
                                else if (tf < c.wtT2) grBase = 1.0f / 3.0f + (tf - c.wtT1) * c.wtInv1;
                                else if (tf < c.wtT3) grBase = 2.0f / 3.0f + (tf - c.wtT2) * c.wtInv2;
                                else                  grBase = 1.0f;
                                // cap just below 1.0: the spray wrap (p01 -= floor) would fold an
                                // EXACT 1.0 journey end back to 0 = frame A (found by GrainTest [7])
                                grBase = juce::jlimit(0.0f, 0.9995f, grBase);
                            }
                            // CHORD/SCALE voicing: successive grains CYCLE the voicing's notes (the
                            // cloud is the chord). uniVoices 1 = plain = bit-identical. STRUM works
                            // too: a note still inside its strum delay is skipped this round, so the
                            // chord tones ENTER the cloud low->high like a real strum.
                            double noteIv = 0.0;
                            if (c.uniVoices > 1)
                                for (int tryN = 0; tryN < c.uniVoices; ++tryN)
                                {
                                    const int k = sv.grNoteIdx % (uint8_t) c.uniVoices;
                                    sv.grNoteIdx = (uint8_t) ((k + 1) % c.uniVoices);
                                    const float iv = sv.uniSemis[k];   // uniVoices > 1 only in SCALE mode now
                                    if (iv < -90.0f) continue;                    // guitar voicing: missing string
                                    if (t < sv.uniDelay[juce::jmin(k, UNI_MAX - 1)]) continue;   // strum: not entered yet
                                    noteIv = (double) iv; break;
                                }
                            for (auto& gr : sv.grains)
                                if (gr.age >= gr.len)              // a free grain slot
                                {
                                    const float r1 = whiteNoise(sv.noiseState);   // position spray
                                    const float r2 = whiteNoise(sv.noiseState);   // pitch spray
                                    const double pmul = (double) noteMul * (double) pe3Mul
                                                      * std::pow(2.0, (double) (r2 * c.grPitch) + noteIv / 12.0);   // spray +-12 st + chord/scale note
                                    gr.len = juce::jmax(64, c.grLenSamp);
                                    gr.age = 0; gr.amp = 1.0f;
                                    const float lfoP = mWt;          // matrix WAVE->grain-position modulation (de-zippered)
                                    if (smp)
                                    {
                                        const double span = (double) (c.grHi - c.grLo);
                                        double p01 = (double) grBase + (double) (r1 * c.grSpray) + (double) lfoP;
                                        p01 -= std::floor(p01);
                                        gr.pos = (double) c.grLo + p01 * juce::jmax(1.0, span - 4.0);
                                        gr.inc = pmul / (double) engineOS;         // varispeed at the file's rate
                                    }
                                    else
                                    {
                                        double p01 = (double) grBase + (double) (r1 * c.grSpray) + (double) lfoP;
                                        p01 -= std::floor(p01);
                                        gr.pos = p01 * (double) GRAIN_TBL;
                                        gr.inc = c.grIncBase * pmul;               // the table cycle at base pitch
                                    }
                                    break;
                                }
                        }
                        float g = 0.0f;
                        for (auto& gr : sv.grains)
                        {
                            if (gr.age >= gr.len) continue;
                            const float w0 = std::sin((float) kPi * (float) gr.age / (float) gr.len);
                            float smpv;
                            if (smp)
                            {
                                if (gr.pos >= (double) c.grHi - 1.0) { gr.age = gr.len; continue; }
                                const int   i0 = (int) gr.pos;
                                const float fr = (float) (gr.pos - i0);
                                const auto* sa = c.grBuf->getReadPointer(0);
                                const auto* sb = c.grBuf->getReadPointer(juce::jmin(1, c.grBuf->getNumChannels() - 1));
                                const float m0 = 0.5f * (sa[i0] + sb[i0]), m1 = 0.5f * (sa[i0 + 1] + sb[i0 + 1]);
                                smpv = m0 + fr * (m1 - m0);
                            }
                            else
                            {
                                double pp = gr.pos; while (pp >= (double) GRAIN_TBL) pp -= (double) GRAIN_TBL;
                                const int   i0 = (int) pp, i1 = (i0 + 1) % GRAIN_TBL;
                                const float fr = (float) (pp - i0);
                                smpv = c.grTbl[i0] + fr * (c.grTbl[i1] - c.grTbl[i0]);
                            }
                            g += smpv * (w0 * w0) * gr.amp;
                            gr.pos += gr.inc; ++gr.age;
                        }
                        sig = g * c.grNorm * env;
                        break; }
                    case SrcModal: {
                        // Strike: feed ONE impulse into the resonator bank (a bank of 2-pole resonators =
                        // decaying sines). Each mode rings + decays on its own from there.
                        // PITCH: the bank is built from the per-block base coeffs at the strike, scaled by
                        // per-step + channel pitch (vPitchMul) AND the 4-dot pitch envelope (pe3Mul). With a
                        // pitch env active the bank is RE-TUNED once per block (block-rate sweep - a
                        // per-sample sweep would be 16 biquad recomputes per sample). Mid-ring only the pole
                        // ANGLE moves (a1); radius + gain stay, so the ring keeps decaying smoothly.
                        const bool strike = ! sv.modalInit;
                        // SUSTAIN-HOLD (keys or a gated Length step): while the gate is open, the mode
                        // radii are clamped toward 1 (the bank stops decaying = the bell is "bowed");
                        // release restores the authored radii = the natural decay rings out. Re-baked
                        // once per block while holding + once on the release transition.
                        const bool modalGate = c.sustain > 0.01f
                                            && ((v.isKey && v.keyOff < 0) || (v.gateLen > 0 && t < v.gateLen));
                        // UNISON/CHORD: each chord note is its OWN FULL resonator bank (like Physical's
                        // real strings), tuned to its chord interval + detune. nNotes==1 = one bank = the
                        // original single bell (bit-identical). RMS make-up keeps the level across notes.
                        const int nNotes = juce::jlimit(1, MODAL_NOTES, c.uniVoices);
                        if (strike || ((c.pEnvOn || c.oscVibFac != 1.0f || modalGate || sv.modalHold) && i == 0)) {
                            sv.modalHold = modalGate;
                            const double pmBase = noteMul * pe3Mul * (double) c.oscVibFac;   // vibrato = block-rate pole-angle wobble
                            if (strike) sv.modalNV = c.modalN;   // bank size fixed at the strike (immune to a mid-ring Material change)
                            double uniMul[MODAL_NOTES];
                            bool noteOn[MODAL_NOTES];
                            for (int j = 0; j < nNotes; ++j) {
                                const double sp = nNotes > 1 ? 2.0 * (double) j / (double)(nNotes - 1) - 1.0 : 0.0;
                                const double iv = c.scaleOn ? (double) sv.uniSemis[j] : 0.0;
                                noteOn[j] = ! (c.scaleOn && sv.uniSemis[j] < -90.0f);   // guitar voicing: missing string
                                uniMul[j] = noteOn[j] ? std::pow(2.0, iv / 12.0 + sp * (double) c.uniCents / 1200.0)
                                                          * (double) sv.driftMul[juce::jmin(j, UNI_MAX)] : 1.0;   // DRIFT
                            }
                            for (int j = 0; j < nNotes; ++j) {
                                if (! noteOn[j]) continue;
                                const double pm = pmBase * uniMul[j];
                                for (int m = 0; m < sv.modalNV; ++m) {
                                    if (strike) { sv.modalY1[j][m] = 0.0f; sv.modalY2[j][m] = 0.0f; }
                                    const float a2 = c.modalA2[m];
                                    float r  = std::sqrt(juce::jmax(0.0f, a2));
                                    if (r <= 1.0e-6f) { if (strike) { sv.modalA1[j][m] = 0.0f; sv.modalA2[j][m] = 0.0f; sv.modalGain[j][m] = 0.0f; } continue; }
                                    const float c0 = juce::jlimit(-1.0f, 1.0f, c.modalA1[m] / (2.0f * r));
                                    const float w0 = std::acos(c0);
                                    const double w = (double) w0 * pm;
                                    if (strike && (w >= kPi * 0.98 || w <= 0.0))   // above Nyquist at the strike -> mute
                                    { sv.modalA1[j][m] = 0.0f; sv.modalA2[j][m] = 0.0f; sv.modalGain[j][m] = 0.0f; continue; }
                                    if (modalGate)   // "bow the bell": low modes freeze, highs keep fading
                                    {
                                        const float fHi = sv.modalNV > 1 ? (float) m / (float)(sv.modalNV - 1) : 0.0f;
                                        r = juce::jmax(r, 1.0f - (1.0e-7f + fHi * fHi * 3.0e-5f));
                                    }
                                    const double wc = juce::jlimit(0.001, (double) kPi * 0.98, w);   // retune mid-ring: clamp, never mute
                                    sv.modalA1[j][m] = 2.0f * r * (float) std::cos(wc);
                                    sv.modalA2[j][m] = r * r;
                                    if (strike) {
                                        const float s0 = std::sin(w0);
                                        const float gg = (s0 > 1.0e-6f) ? c.modalGain[m] / s0 : 0.0f;
                                        sv.modalGain[j][m] = gg * (float) std::sin((float) wc);
                                    }
                                }
                            }
                        }
                        float out = 0.0f;
                        for (int j = 0; j < nNotes; ++j) {
                            // STRUM: strike chord note j at its own onset (uniDelay). Its bank stays zero (silent)
                            // until then, so it rings up fresh. uniDelay 0 => strike at t=0 == the old behaviour.
                            const long strikeAt = (j < UNI_MAX) ? (long) sv.uniDelay[j] : 0;
                            for (int m = 0; m < sv.modalNV; ++m) {
                                const float x = (t == strikeAt) ? sv.modalGain[j][m] : 0.0f;   // per-note impulse
                                const float y = sv.modalA1[j][m] * sv.modalY1[j][m] - sv.modalA2[j][m] * sv.modalY2[j][m] + x;
                                sv.modalY2[j][m] = sv.modalY1[j][m]; sv.modalY1[j][m] = y;
                                out += y;
                            }
                        }
                        out *= 1.0f / std::sqrt((float) nNotes);   // RMS make-up across chord notes
                        sv.modalInit = true;
                        // With SUSTAIN: the held bank doesn't decay, so the gated ADSR shapes the note
                        // (fall to the sustain level over the Ring, release after the gate). Sustain 0 =
                        // the old behaviour exactly: Strike ramp only, the modes carry the decay.
                        if (v.isKey && c.sustain <= 0.01f)
                        {
                            // Sustain 0 on a KEY = a faithful synth note: the bank IS the decay while
                            // the key is held, and LETTING GO releases it (the bell used to ring on
                            // after key-up, which read as broken). TEST/plain steps still ring free.
                            env = (c.atk > 0.005f) ? juce::jmin(1.0f, (float) t / juce::jmax(1.0f, c.atk * (float) sr)) : 1.0f;
                            // keyOff can be set IN ADVANCE (a piano-roll note's voice carries its gate
                            // end from trigger()); the release must not start until the gate actually
                            // ends - decayCurve with NEGATIVE time = exp(+t) = an exploding "louder
                            // than live" note (the Mod Kalimba roll bug).
                            if (v.keyOff >= 0 && t >= v.keyOff)
                                env *= decayCurve(t - v.keyOff, juce::jmax(0.005f, c.release));
                        }
                        else if ((v.isKey || v.gateLen > 0) && c.sustain > 0.01f)
                            env = keyAdsr(t, v.isKey ? v.keyOff : v.gateLen, c.atk, 0.0f,
                                          juce::jmax(0.05f, c.modalDecaySec), c.sustain, c.release);
                        else {
                            // STRIKE (attack ramp) = a soft/swelled onset (the modes self-decay = the RING). Gated so
                            // atk <= 5 ms (every factory Modal sound, default 0.003) keeps env=1 = instant = bit-identical.
                            env = (c.atk > 0.005f) ? juce::jmin(1.0f, (float) t / juce::jmax(1.0f, c.atk * (float) sr)) : 1.0f;
                            // Per-step LENGTH on Modal: the bank self-decays (a ring can't be EXTENDED), but a
                            // SHORTER note is shaped by the same rescaled fall, so Length still tightens bells.
                            if (sv.gateDec > 0.0f)
                                env *= decayCurve(juce::jmax((long) 0, t - (long)((c.atk + c.hold) * (float) sr)), sv.gateDec);
                        }
                        if (! std::isfinite(out)) { for (auto& row : sv.modalY1) for (auto& vv : row) vv = 0.0f; for (auto& row : sv.modalY2) for (auto& vv : row) vv = 0.0f; out = 0.0f; }  // self-heal a runaway
                        sig = juce::jlimit(-4.0f, 4.0f, out) * 0.4f * env;   // bound the bank sum to a musical level, then the Strike ramp
                        break; }
                    default: break;
                }

                // === AUDIO-RATE VOLUME [2026-07-13 19:57]: per-sample AM gain from the route program
                //     (ANY source now - slow = tremolo, fast LFO = sidebands / ring-mod textures). ===
                sv.lastEnv = env;   // the Amp Env SOURCE reads this next sample (1-sample delay, always smooth)
                if (arVolG != 1.0f) { if (stereo) { sL *= arVolG; sR *= arVolG; } else sig *= arVolG; }
                // Advance every ACTIVE LFO's phase once per sample (the filter LFO's phase is read per
                // block; the audio-rate Pitch/Vol routes above read it per sample). KEY mode (lfoKeyRatio
                // > 0): the rate follows THIS VOICE's pitch x ratio - that's what keeps FM colour
                // consistent across the keyboard (pitchPreLfo = pre-FM pitch, so it can't self-feed).
                for (int d2 = 0; d2 < 4; ++d2)
                    if (c.lfoAmt[d2] > 0.001f || c.lfoSrcUsed[d2]) {   // advance also when it drives a mod-matrix route
                        const double rateHz = (c.lfoKeyRatio[d2] > 0.0f)
                            ? juce::jlimit(0.01, sr * 0.45, c.lfoKeyBase * (double) c.lfoKeyRatio[d2] * pitchPreLfo)
                            : (double) c.lfoRate[d2];
                        sv.lfoPhase[d2] += 2.0 * kPi * rateHz / sr;
                        if (sv.lfoPhase[d2] > 2.0 * kPi) { sv.lfoPhase[d2] -= 2.0 * kPi; ++sv.lfoCyc[d2]; }
                    }

                // === PER-SLOT FILTERS (begin) - TWO resonant TPT/ZDF SVFs, SPLIT POSITIONS
                //     [2026-07-17, user design]: FILTER 1 = the INPUT filter, applied HERE (before
                //     Drive/Formant/Punch/Ring/Sub = it shapes what FEEDS the FX - the acid/growl
                //     tool); FILTER 2 = the OUTPUT filter, applied at the END of the slot chain
                //     (after every slot FX + its modulations = it acts on exactly the signal the
                //     slot spectrum draws). State per-voice; cutoff coeff smoothed ~2 ms; keytrack
                //     via matrix Note routes works at either position. ===
                auto applySlotFilt = [&](int fi)
                {
                    const auto& fc = c.filt[fi];
                    if (! fc.on) return;
                    double& gm = sv.filtGm[fi];
                    const double tgt = (sv.filtGkt[fi] > 0.0) ? sv.filtGkt[fi] : fc.G;   // keytrack and/or drift per-note cutoff
                    if (gm < 0.0) gm = tgt;
                    gm += (tgt - gm) * 0.0025;                           // ~2 ms at 2x-OS engine rates
                    double& km = sv.filtKm[fi];                          // damping K smoothed the same way
                    if (km < 0.0) km = fc.K;
                    km += ((arKTgt[fi] >= 0.0 ? arKTgt[fi] : fc.K) - km) * 0.0025;   // per-sample RESO target (audio-rate route)
                    // [2026-07-13 19:57] AUDIO-RATE CUTOFF (real filter-FM): the smoothed base coeff is
                    // MULTIPLIED by the per-sample route product (g ~ tan(pi f/sr) is near-linear in f
                    // below ~5 kHz, so 2^x on g == 2^x on the cutoff; TPT SVF is unconditionally stable).
                    const double gEff = (arGmMul[fi] != 1.0) ? juce::jlimit(1.0e-5, 8.0, gm * arGmMul[fi]) : gm;
                    const double a1 = 1.0 / (1.0 + gEff * (gEff + km)), a2 = gEff * a1, a3 = gEff * a2;
                    const int fm = fc.mode; const double fk = km;
                    // Bell peak gain follows the (possibly per-sample) K so RESO routes modulate the BOOST.
                    const double bellM1s = (fm == 4) ? km * (fc.bellA * fc.bellA - 1.0) : 0.0;
                    // FILTER DRIVE: soft tanh on v3 = INSIDE the state loop, so resonance compresses
                    // and sings instead of ringing louder. drv 0 = the exact old linear path.
                    const double drv = (double) c.filtDrive;
                    // Gain up to 10x with only sqrt make-down = clearly audible colour by ~30%.
                    const double dg = 1.0 + drv * 9.0, dgi = 1.0 / std::sqrt(dg);
                    auto svf = [&](double in, int lr) -> double {
                        double v3 = in - sv.filtIc2[fi][lr];
                        if (drv > 0.0) v3 = std::tanh(v3 * dg) * dgi;
                        const double v1 = a1 * sv.filtIc1[fi][lr] + a2 * v3;
                        const double v2 = sv.filtIc2[fi][lr] + a2 * sv.filtIc1[fi][lr] + a3 * v3;
                        sv.filtIc1[fi][lr] = 2.0 * v1 - sv.filtIc1[fi][lr];
                        sv.filtIc2[fi][lr] = 2.0 * v2 - sv.filtIc2[fi][lr];
                        switch (fm) {                                   // Cytomic SVF outputs
                            case 1:  return in - fk * v1 - v2;          // HIGHPASS
                            case 2:  return v1;                         // BANDPASS
                            case 3:  return in - fk * v1;               // NOTCH
                            case 4:  return in + bellM1s * v1;          // BELL (boosting peak; K live = modulated boost)
                            default: return v2;                         // LOWPASS
                        }
                    };
                    if (stereo) { sL = (float) svf(sL, 0); sR = (float) svf(sR, 1); }
                    else          sig = (float) svf(sig, 0);
                };
                applySlotFilt(0);   // FILTER 1 = the INPUT filter (pre-FX; the acid/growl position)
                // Track this slot's max amp level this block -> next block's env-follow cutoff.
                blockSlotEnv[s] = juce::jmax(blockSlotEnv[s], env * v.velGain);
                // === PER-SLOT FILTER (end) ===


                // Per-slot DRIVE (insert): shape THIS slot's signal with its own drive type/amount.
                // [2026-07-13 19:57] ROUTE-swept drive DRY-BLENDS over the first ~8% (SMOOTHSTEP): the
                // shapers are NOT identity at amount -> 0 (tanh(x) != x), so crossing zero would step
                // ~1 dB. At true per-sample sweep speeds the old 2% linear ramp itself became a
                // 3-sample crossfade (= the step it was hiding) - 8% + smoothstep keeps the blend
                // slope below the waveform's own. Only when a route TARGETS drive; static = dg 1.
                float dDg = 1.0f;
                if (c.arDrvOn)
                { const float t8 = juce::jlimit(0.0f, 1.0f, mDrv * 12.5f); dDg = t8 * t8 * (3.0f - 2.0f * t8); }
                if (mDrv > 0.0001f && c.fxDriveType == DriveBassAmp) {
                    // BASS AMP (the survivor of the amp family - Guitar/Lead were killed by the
                    // user): the SPLIT RIG. Lows below ~180 Hz pass CLEAN and rejoin at the
                    // output; only the mids/highs are driven (2 soft stages + a 3.8 kHz 2-pole
                    // cab + DC block) = fat, never farty. Fixed voicing; the fader = the gain.
                    const float aA = mDrv, a2 = aA * aA;
                    const float g1 = 1.0f + a2 * 22.0f;
                    const float mk = 1.0f / (1.0f + (g1 - 1.0f) / 7.0f);
                    auto amp = [&](float y, int lr) -> float {
                        sv.ampPre[lr] += drvAmpBassK * (y - sv.ampPre[lr]);
                        const float lo = sv.ampPre[lr];                           // the low split
                        float v = (y - lo) * (1.0f + 0.6f * aA);
                        // [2026-07-13 20:20] both amp stages are ADAA'd (aliasing integrated out).
                        v = adaaTanh(v * g1 + 0.12f, sv.adaaB1[lr]) - 0.11943f;   // stage 1 (asym = amp evens; tanh(0.12))
                        v = adaaTanh(v * 1.5f, sv.adaaB2[lr]) * 1.15f;            // stage 2 (glue)
                        sv.ampLp1[lr] += drvAmpCab2K * (v - sv.ampLp1[lr]);       // 2-pole cabinet
                        sv.ampLp2[lr] += drvAmpCab2K * (sv.ampLp1[lr] - sv.ampLp2[lr]);
                        v = sv.ampLp2[lr] * mk + lo;                              // clean lows rejoin
                        const float dc = v - sv.drvDcX[lr] + 0.995f * sv.drvDcY[lr];   // rumble/DC block
                        sv.drvDcX[lr] = v; sv.drvDcY[lr] = dc;
                        return dc;
                    };
                    if (stereo) { const float iL = sL, iR = sR; sL = iL + dDg * (amp(iL, 0) - iL); sR = iR + dDg * (amp(iR, 1) - iR); }
                    else        { const float iM = sig;             sig = iM + dDg * (amp(iM, 0) - iM); }
                }
                else if (mDrv > 0.0001f && c.fxDriveType != DriveOff) {
                    // retired slot 7 (the killed Guitar/Lead amps): stray saves play as Tube
                    const int dTy = c.fxDriveType == DriveAmpRetired ? (int) Tube : c.fxDriveType;
                    const float dryL = sL, dryR = sR, dryM = sig;   // for the matrix-sweep dry-blend below
                    // [2026-07-13 20:20] ADAA path for every shaper except Bitcrush (aliasing = its sound).
                    if (dTy == (int) Bitcrush) {
                        if (stereo) { sL = driveSample(sL, dTy, mDrv); sR = driveSample(sR, dTy, mDrv); }
                        else          sig = driveSample(sig, dTy, mDrv);
                    } else {
                        if (stereo) { sL = driveAdaa(sL, dTy, mDrv, sv.adaaU[0]); sR = driveAdaa(sR, dTy, mDrv, sv.adaaU[1]); }
                        else          sig = driveAdaa(sig, dTy, mDrv, sv.adaaU[0]);
                    }
                    // Musicality pass (v6): HARSH shapers get a gentle ~8 kHz 1-pole after the clip
                    // (naked waveshaping = fizzy top - every synth drive has a post-filter), and FUZZ
                    // gets a DC blocker (its rectified blend adds a constant offset = headroom loss).
                    const bool harsh = c.fxDriveType == HardClip || c.fxDriveType == Foldback || c.fxDriveType == Fuzz;
                    const bool needDc = c.fxDriveType == Fuzz || c.fxDriveType == DriveExciter;   // t*t terms carry DC
                    if (harsh || needDc) {
                        const float k = drvLpK;   // ~8 kHz at the engine rate (baked per block)
                        auto post = [&](float y, int lr) -> float {
                            if (harsh) { sv.drvLp[lr] += k * (y - sv.drvLp[lr]); y = sv.drvLp[lr]; }   // (Exciter stays BRIGHT - no LP)
                            if (needDc) {                          // DC blocker: y = x - x1 + R*y1
                                const float o = y - sv.drvDcX[lr] + 0.9995f * sv.drvDcY[lr];
                                sv.drvDcX[lr] = y; sv.drvDcY[lr] = o; y = o;
                            }
                            return y;
                        };
                        if (stereo) { sL = post(sL, 0); sR = post(sR, 1); }
                        else          sig = post(sig, 0);
                    }
                    if (dDg < 1.0f) {   // matrix sweeping through zero: fade the whole driven result in
                        if (stereo) { sL = dryL + dDg * (sL - dryL); sR = dryR + dDg * (sR - dryR); }
                        else          sig = dryM + dDg * (sig - dryM);
                    }
                }

                // FORMANT (per slot): two vowel band-passes (F1 + F2, interpolated A->E->I->O->U)
                // blended over the dry = vocal colour; sweep it (knob or matrix) and the slot TALKS.
                if (c.fmtMix > 0.0f) {
                    auto fmt = [&](float x, int lr) -> float {
                        const float w1 = eqProcess(c.fmtBq[0], sv.fmtZ1[0], sv.fmtZ2[0], x, lr);
                        const float w2 = eqProcess(c.fmtBq[1], sv.fmtZ1[1], sv.fmtZ2[1], x, lr);
                        return x * (1.0f - c.fmtMix) + (w1 + w2) * 1.9f * c.fmtMix;
                    };
                    if (stereo) { sL = fmt(sL, 0); sR = fmt(sR, 1); } else sig = fmt(sig, 0);
                }
                // PUNCH (transient shaper, per slot/per hit): fast-vs-slow envelope difference finds the
                // attack; positive boosts it (snap), negative softens it (felt/pillowy). Bounded gains.
                if (mPunch != 0.0f) {
                    const float m = stereo ? 0.5f * (std::abs(sL) + std::abs(sR)) : std::abs(sig);
                    sv.pFast[0] += punchKf * (m - sv.pFast[0]);
                    sv.pSlow[0] += punchKs * (m - sv.pSlow[0]);
                    const float tr = juce::jlimit(0.0f, 2.0f, (sv.pFast[0] - sv.pSlow[0]) / (sv.pSlow[0] + 1.0e-4f));
                    const float pg = mPunch > 0.0f ? 1.0f + mPunch * tr * 1.2f
                                                    : 1.0f / (1.0f + (-mPunch) * tr * 1.2f);
                    if (stereo) { sL *= pg; sR *= pg; } else sig *= pg;
                }
                // RING (per slot): ring modulation. Carrier = the Ring Hz knob (default 200 = the old
                // fixed constant) OR, at the knob's hard left, TRACKS the note (carrier = this voice's
                // pitch - classic tuned ring mod, consistent across the keyboard).
                if (mRing > 0.0f) {
                    const double rInc = c.ringTrack
                        ? 2.0 * kPi * juce::jlimit(1.0, sr * 0.45, c.ringBase * pitchPreLfo * mRingHzMul) / sr
                        : 2.0 * kPi * juce::jlimit(1.0, sr * 0.45, c.ringHz * mRingHzMul) / sr;
                    sv.ringPh += rInc; if (sv.ringPh > 2.0 * kPi) sv.ringPh -= 2.0 * kPi;
                    const float m = (float) std::sin(sv.ringPh), rg = mRing;
                    if (stereo) { sL = sL * (1.0f - rg) + sL * m * rg; sR = sR * (1.0f - rg) + sR * m * rg; }
                    else          sig = sig * (1.0f - rg) + sig * m * rg;
                }
                // SUB (per slot, near the END of the chain so it stays clean of Filter 1 + drive;
                // FILTER 2 - the output filter - DOES shape it, like everything the spectrum shows):
                // a sine ONE OCTAVE below the slot's pitch, following note/step/glide (noteMul)
                // + the pitch envelope (pe3Mul) + the slot's own amp env. Unpitched engines = inert.
                if (mSub > 0.0f && c.subHz > 0.0) {
                    sv.subPh += 2.0 * kPi * c.subHz * (double) noteMul * pe3Mul / sr;
                    if (sv.subPh > 2.0 * kPi) sv.subPh -= 2.0 * kPi;
                    const float sub = (float) std::sin(sv.subPh) * env * mSub * 0.9f;
                    if (stereo) { sL += sub * 0.7071f; sR += sub * 0.7071f; } else sig += sub;
                }
                applySlotFilt(1);   // FILTER 2 = the OUTPUT filter (post ALL slot FX incl. Sub - matches the spectrum)
                vEnv = juce::jmax(vEnv, env);
                // HUMANIZE velocity jitter: sv.velScale (~1) loosens this slot's level per hit (1 = identical).
                // Weight is smoothed PER SAMPLE (~3 ms) toward the block value: block-rate volume modulation
                // (MTVol / blend moves) never steps audibly. Constant weight = snap-once = bit-identical.
                if (sv.wSm < 0.0f) sv.wSm = c.weight; else sv.wSm += wSmK * (c.weight - sv.wSm);
                const float wEff = sv.wSm * sv.velScale * sv.driftGain;   // driftGain = per-note breath (1 = off)
                const float cL = (stereo ? sL : sig) * wEff * mPanL;   // SLOT PAN places the layer (matrix = per-sample)
                const float cR = (stereo ? sR : sig) * wEff * mPanR;
                mixL += cL; mixR += cR;   // all slots sum to the mix; CHANNEL FX process the sum after the loop
                // === PER-SLOT EQ (begin) - capture THIS slot's mono output for its spectrum ===
                if (s == tapSlot && i < analysisBufLen) {
                    const float mono = stereo ? 0.5f * wEff * (sL + sR) : wEff * sig;
                    analysisBuf[i] += mono * v.velGain;
                }
                // === PER-SLOT EQ (end) ===
            }

            outL[i] += mixL * v.velGain * vPanL * kg;
            outR[i] += mixR * v.velGain * vPanR * kg;
            maxEnvLevel = juce::jmax(maxEnvLevel, vEnv * v.velGain);

            // (Per-step LENGTH no longer hard-cuts here: it's a HOLD handled in the amp env - the note
            //  sustains until the gate, then the env decays as the release. Only CHOKES set v.killing now.)
            if (v.killing)
            {
                v.killGain -= (v.killStep > 0.0f ? v.killStep : killStep);   // choke fade advances per sample
                if (v.killGain <= 0.0f) { finished = true; break; }       // fade complete -> voice ends
            }
            if (++v.voiceSamples >= veEnd) { finished = true; break; }
        }

        if (finished) v.playHead = -1.0;
    }

    // === CHANNEL FX (v1.3.9 round-2): TWO selectable effect SLOTS run in series A -> B on the summed
    //     channel. Each = TYPE (Off/Chorus/Flanger/Phaser/Comp) + Amount + CHARACTER (the second
    //     dimension: chorus = rate+depth, flanger/phaser = sweep speed + feedback, comp = attack).
    //     Character 0.5 = the OLD fixed constants EXACTLY (migration keeps every sound's voicing).
    //     Amounts smoothed PER SAMPLE (~4 ms, de-zipper); a slot engaging from fully-off starts from
    //     CLEARED state (a paused delay line holds stale audio = a burst otherwise). Type Off /
    //     amount 0 (and no modulation) = the slot is skipped = bit-identical. ===
    {
        const float smK = 1.0f - std::exp(-1.0f / (0.004f * (float) sr));
        const float aTgt[3] = { juce::jlimit(0.0f, 1.0f, chFxAmt[0] + chFxMod[0]),      // slot A/B/C modulated Amount
                                juce::jlimit(0.0f, 1.0f, chFxAmt[1] + chFxMod[2]),
                                juce::jlimit(0.0f, 1.0f, chFxAmt[2] + chFxMod[6]) };
        const float cTgt[3] = { juce::jlimit(0.0f, 1.0f, chFxChar[0] + chFxMod[1]),     // slot A/B/C modulated Character
                                juce::jlimit(0.0f, 1.0f, chFxChar[1] + chFxMod[3]),
                                juce::jlimit(0.0f, 1.0f, chFxChar[2] + chFxMod[7]) };
        for (int fx = 0; fx < 3; ++fx)
        {
            const int type = chFxType[fx];
            float& sm = chFxSm[fx];
            if (sm < 0.0f) sm = aTgt[fx];                       // first use = snap (bit-identical when constant)
            const bool runNow = type != ChFxOff && (sm > 1.0e-4f || aTgt[fx] > 1.0e-4f);
            if (runNow && ! chFxRun[fx])
            {   // engaging from OFF -> start from clean state (no stale-buffer burst)
                std::fill(chFxDL[fx].begin(), chFxDL[fx].end(), 0.0f);
                std::fill(chFxDR[fx].begin(), chFxDR[fx].end(), 0.0f);
                for (int k = 0; k < 6; ++k) { chFxPzL[fx][k] = chFxPzR[fx][k] = 0.0f; }
                for (int k = 0; k < 64; ++k) chFxHil[fx][k] = 0.0f;
                chFxFbL[fx] = chFxFbR[fx] = 0.0f; chFxCompEnv[fx] = 0.0f;
            }
            chFxRun[fx] = runNow;
            if (! runNow) { sm = juce::jmax(0.0f, sm - 0.05f); continue; }
            const float ch1 = cTgt[fx];                          // character (rates re-bake per block)
            // [2026-07-15 13:30] "(sync)" variants: Character = counted CYCLES PER BAR via the
            // shared kChFxCpb stops (rate follows lfoBarSeconds = live tempo); the free Character's
            // depth/feedback half is pinned at its 0.5 default. Free types = the exact old code.
            const bool typeSync = type >= ChFxChorusS;
            const int  baseType = ! typeSync ? type
                                : type == ChFxChorusS  ? ChFxChorus
                                : type == ChFxFlangerS ? ChFxFlanger
                                : type == ChFxPhaserS  ? ChFxPhaser
                                : type == ChFxTapeS    ? ChFxTape
                                : type == ChFxAutoPanS ? ChFxAutoPan : (int) ChFxRotary;
            double syncHz = 0.0;
            if (typeSync)
                syncHz = kChFxCpb[juce::jlimit(0, kChFxCpbN - 1, (int) std::lround(ch1 * (float)(kChFxCpbN - 1)))]
                         / juce::jmax(0.1f, lfoBarSeconds);
            const float chDepth = typeSync ? 0.5f : ch1;         // synced: rate only, depth = the 0.5 default
            switch (baseType)
            {
                case ChFxComp:
                {   // GLUE compressor across both layers; CHARACTER = attack (0.5 = the old 4 ms)
                    const float atkSec = 0.004f * std::pow(4.0f, 1.0f - 2.0f * ch1);   // 16 ms .. 1 ms
                    const float att = 1.0f - std::exp(-1.0f / (atkSec * (float) sr));
                    const float rel = 1.0f - std::exp(-1.0f / (0.120f * (float) sr));
                    float env = chFxCompEnv[fx], a = sm;
                    for (int i = 0; i < numSamples; ++i)
                    {
                        a += smK * (aTgt[fx] - a);
                        const float thr = 0.30f - 0.20f * a;     // higher knob = lower threshold + steeper ratio
                        const float mk  = 1.0f + a * 1.4f;       // makeup so squashing doesn't just get quieter
                        const float lvl = juce::jmax(std::abs(outL[i]), std::abs(outR[i]));
                        env += (lvl > env ? att : rel) * (lvl - env);
                        float gr = 1.0f;
                        if (env > thr) gr = std::pow(env / thr, -0.9f * a);
                        outL[i] *= gr * mk; outR[i] *= gr * mk;
                    }
                    chFxCompEnv[fx] = env; sm = a;
                } break;
                case ChFxFlanger:
                {   // swept short delay + feedback (jet sweep); CHARACTER = speed + bite (0.5 = old 0.20 Hz / 0.7 fb)
                    const int flen = juce::jmax(64, (int) (0.012 * sr));
                    if ((int) chFxDL[fx].size() != flen) { chFxDL[fx].assign((size_t) flen, 0.0f); chFxDR[fx].assign((size_t) flen, 0.0f); chFxW[fx] = 0; chFxPhs[fx] = 0.0; }
                    int w = chFxW[fx]; double ph = chFxPhs[fx];
                    const double dPh   = 2.0 * kPi * (typeSync ? syncHz : 0.20 * std::pow(4.0, 2.0 * (double) ch1 - 1.0)) / sr;   // free: 0.05..0.8 Hz
                    const float  baseS = (float) (0.0010 * sr), depthS = (float) (0.0040 * sr);   // 1..5 ms sweep
                    const float  fbC   = juce::jlimit(0.0f, 0.92f, 0.2f + 1.0f * chDepth);        // 0.5 -> the old 0.7
                    float amt = sm;
                    auto rd = [&](const std::vector<float>& buf, float delay) -> float {
                        float rp = (float) w - delay; while (rp < 0.0f) rp += (float) flen;
                        const int i0 = (int) rp; const float fr = rp - (float) i0;
                        const int i1 = (i0 + 1 < flen) ? i0 + 1 : 0;
                        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr; };
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        const float fb = fbC * amt, mix = amt;
                        const float d = baseS + depthS * (0.5f + 0.5f * (float) std::sin(ph));
                        const float wL = rd(chFxDL[fx], d), wR = rd(chFxDR[fx], d);
                        chFxDL[fx][(size_t) w] = juce::jlimit(-4.0f, 4.0f, outL[i] + wL * fb);   // anti-gunshot clamp
                        chFxDR[fx][(size_t) w] = juce::jlimit(-4.0f, 4.0f, outR[i] + wR * fb);
                        outL[i] = outL[i] * (1.0f - 0.5f * mix) + wL * (0.5f * mix);             // unity-safe blend
                        outR[i] = outR[i] * (1.0f - 0.5f * mix) + wR * (0.5f * mix);
                        if (++w >= flen) w = 0;
                        ph += dPh; if (ph > 2.0 * kPi) ph -= 2.0 * kPi;
                    }
                    chFxW[fx] = w; chFxPhs[fx] = ph; sm = amt;
                } break;
                case ChFxPhaser:
                {   // 6 swept allpasses + feedback (swirl); CHARACTER = speed + resonance (0.5 = old 0.30 Hz / 0.6 fb)
                    double ph = chFxPhs[fx];
                    const double dPh = 2.0 * kPi * (typeSync ? syncHz : 0.30 * std::pow(4.0, 2.0 * (double) ch1 - 1.0)) / sr;   // free: 0.075..1.2 Hz
                    const float fbC = juce::jlimit(0.0f, 0.9f, 0.1f + 1.0f * chDepth);                      // 0.5 -> the old 0.6
                    float* zL = chFxPzL[fx]; float* zR = chFxPzR[fx];
                    float fbL = chFxFbL[fx], fbR = chFxFbR[fx];
                    float amt = sm;
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        const float fb = fbC * amt;
                        const float lfo = 0.5f + 0.5f * (float) std::sin(ph);
                        const float a = -0.2f + 1.1f * lfo;      // allpass coeff sweep (cheap, no tan): notches move
                        float xL = outL[i] + fbL * fb, xR = outR[i] + fbR * fb;
                        for (int k = 0; k < 6; ++k) { const float yL = -a * xL + zL[k]; zL[k] = xL + a * yL; xL = yL;
                                                      const float yR = -a * xR + zR[k]; zR[k] = xR + a * yR; xR = yR; }
                        fbL = xL; fbR = xR;
                        outL[i] = outL[i] * (1.0f - 0.5f * amt) + xL * (0.5f * amt);
                        outR[i] = outR[i] * (1.0f - 0.5f * amt) + xR * (0.5f * amt);
                        ph += dPh; if (ph > 2.0 * kPi) ph -= 2.0 * kPi;
                    }
                    chFxPhs[fx] = ph; chFxFbL[fx] = juce::jlimit(-4.0f, 4.0f, fbL); chFxFbR[fx] = juce::jlimit(-4.0f, 4.0f, fbR); sm = amt;
                } break;
                case ChFxOtt:
                {   // OTT: 3-band UP + DOWN compression (the Xfer-style density). Bands split at ~120 Hz
                    // and ~2.5 kHz (phase-coherent one-pole splits); each band's level is dragged toward
                    // a target from BOTH directions (quiet up, loud down), gains capped +-14 dB, and the
                    // whole effect is blended by Amount (the classic OTT "Depth"). CHARACTER = speed.
                    // [2026-07-15 16:00] RE-TUNED (user: "no difference at all" - round 1 was far
                    // too polite): the pull toward the target is near-TOTAL now (exponents 0.7/0.9,
                    // was 0.55/0.45), the gain window is +-20 dB (was +-14) and the follower works
                    // 40 dB deeper into the quiet - the Xfer-style "everything equals the target"
                    // wall. ModMatrixTest [23] locks the audibility (tail x4+ at full depth).
                    const float spd  = std::pow(4.0f, 1.0f - 2.0f * ch1);                 // time scale (fader up = faster)
                    const float att  = 1.0f - std::exp(-1.0f / (0.004f * spd * (float) sr));
                    const float rel  = 1.0f - std::exp(-1.0f / (0.090f * spd * (float) sr));
                    const float kLo  = 1.0f - std::exp(-2.0f * (float) kPi * 120.0f  / (float) sr);
                    const float kMid = 1.0f - std::exp(-2.0f * (float) kPi * 2500.0f / (float) sr);
                    const float tgt3[3] = { 0.22f, 0.16f, 0.10f };                        // per-band targets
                    float amt = sm;
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        // split L+R with shared coefficients (states: PzL/PzR [0]=lowLP [1]=midLP)
                        chFxPzL[fx][0] += kLo  * (outL[i] - chFxPzL[fx][0]);
                        chFxPzR[fx][0] += kLo  * (outR[i] - chFxPzR[fx][0]);
                        chFxPzL[fx][1] += kMid * (outL[i] - chFxPzL[fx][1]);
                        chFxPzR[fx][1] += kMid * (outR[i] - chFxPzR[fx][1]);
                        const float loL = chFxPzL[fx][0],            loR = chFxPzR[fx][0];
                        const float miL = chFxPzL[fx][1] - loL,      miR = chFxPzR[fx][1] - loR;
                        const float hiL = outL[i] - chFxPzL[fx][1],  hiR = outR[i] - chFxPzR[fx][1];
                        const float bl[3] = { loL, miL, hiL }, br[3] = { loR, miR, hiR };
                        float oL = 0.0f, oR = 0.0f;
                        for (int b2 = 0; b2 < 3; ++b2)
                        {
                            float& env = chFxPzL[fx][2 + b2];                              // band envelope ([2..4])
                            const float lvl = juce::jmax(std::abs(bl[b2]), std::abs(br[b2]));
                            env += (lvl > env ? att : rel) * (lvl - env);
                            float g = 1.0f;
                            if (env > 1.0e-6f)
                                g = std::pow(tgt3[b2] / env, env > tgt3[b2] ? 0.7f : 0.9f);    // down / UP: near-total pull
                            g = juce::jlimit(0.1f, 10.0f, g);                              // +-20 dB window
                            g = 1.0f + (g - 1.0f) * amt;                                   // Depth blend
                            oL += bl[b2] * g; oR += br[b2] * g;
                        }
                        outL[i] = oL * 0.9f; outR[i] = oR * 0.9f;                          // trim (OTT gets LOUD)
                    }
                    sm = amt;
                } break;
                case ChFxFreqShift:
                {   // FREQUENCY SHIFTER: true single-sideband via an IIR Hilbert pair (2 allpass cascades
                    // ~90 deg apart), heterodyned by a quadrature oscillator. NOT a pitch shifter and NOT
                    // ring mod - every frequency moves by the SAME +-Hz, breaking harmonicity: tiny shifts
                    // = barber-pole detune/phasing, big = alien metal. CHARACTER = the shift (centre = 0,
                    // right = up, left = down, log to ~1.5 kHz). Amount = mix.
                    static const float hA[4] = { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
                    static const float hB[4] = { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };
                    const float dch = 2.0f * ch1 - 1.0f;
                    const double shiftHz = (dch >= 0.0f ? 1.0 : -1.0) * (std::pow(1500.0, (double) std::abs(dch)) - 1.0);
                    double ph = chFxPhs[fx];
                    const double dPh = 2.0 * kPi * shiftHz / sr;
                    float amt = sm;
                    // state layout per channel side (32 floats): path A biquads [0..15], path B [16..31]
                    auto ap2 = [](float x, float a, float* st) -> float {
                        // 2nd-order allpass y[n] = a*(x[n] + y[n-2]) - x[n-2]; st = {x1, x2, y1, y2}
                        const float y = a * (x + st[3]) - st[1];
                        st[1] = st[0]; st[0] = x; st[3] = st[2]; st[2] = y;
                        return y; };
                    auto shift1 = [&](float x, float* st, double phase) -> float {
                        float i1 = x, q1 = x;
                        for (int k = 0; k < 4; ++k) i1 = ap2(i1, hA[k] * hA[k], st + k * 4);
                        for (int k = 0; k < 4; ++k) q1 = ap2(q1, hB[k] * hB[k], st + 16 + k * 4);
                        // path A LEADS path B by ~90 deg (verified by test [14]: the minus form picked
                        // the LOWER sideband) - the upper-sideband select is I*cos + Q*sin here.
                        return i1 * (float) std::cos(phase) + q1 * (float) std::sin(phase); };
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        const float wetL = shift1(outL[i], chFxHil[fx],      ph);
                        const float wetR = shift1(outR[i], chFxHil[fx] + 32, ph);
                        outL[i] += (wetL - outL[i]) * amt;
                        outR[i] += (wetR - outR[i]) * amt;
                        ph += dPh; if (ph > 2.0 * kPi) ph -= 2.0 * kPi; else if (ph < 0.0) ph += 2.0 * kPi;
                    }
                    chFxPhs[fx] = ph; sm = amt;
                } break;
                case ChFxRotary:
                {   // ROTARY (Leslie): crossover ~800 Hz; the HORN (highs) spins fast - doppler (modulated
                    // delay) + AM + pan; the ROTOR (lows) spins at 0.34x - gentle AM + slight pan.
                    // CHARACTER = speed (0.7 Hz chorale .. 7 Hz tremolo). Amount = intensity.
                    const int flen = juce::jmax(64, (int) (0.006 * sr));
                    if ((int) chFxDL[fx].size() != flen) { chFxDL[fx].assign((size_t) flen, 0.0f); chFxDR[fx].assign((size_t) flen, 0.0f); chFxW[fx] = 0; chFxPhs[fx] = 0.0; }
                    int w = chFxW[fx]; double ph = chFxPhs[fx]; double phR = chFxPhs2[fx];   // rotor = its OWN accumulator (non-integer 0.34x of a wrapped phase JUMPED at every wrap = clicks)
                    const double hornHz = typeSync ? syncHz : 0.7 * std::pow(10.0, (double) ch1);   // free: 0.7..7 Hz
                    const double dPh = 2.0 * kPi * hornHz / sr;
                    const float  kX  = 1.0f - std::exp(-2.0f * (float) kPi * 800.0f / (float) sr);
                    const float  baseS = (float) (0.0022 * sr);
                    float amt = sm;
                    auto rd = [&](const std::vector<float>& buf, float delay) -> float {
                        float rp = (float) w - delay; while (rp < 0.0f) rp += (float) flen;
                        const int i0 = (int) rp; const float fr = rp - (float) i0;
                        const int i1 = (i0 + 1 < flen) ? i0 + 1 : 0;
                        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr; };
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        const float hs = (float) std::sin(ph),  hc = (float) std::cos(ph);
                        const float rs = (float) std::sin(phR);
                        // split (states [0]/[1] = LP per side); highs into the doppler line
                        chFxPzL[fx][0] += kX * (outL[i] - chFxPzL[fx][0]);
                        chFxPzR[fx][0] += kX * (outR[i] - chFxPzR[fx][0]);
                        const float loL = chFxPzL[fx][0], loR = chFxPzR[fx][0];
                        chFxDL[fx][(size_t) w] = outL[i] - loL; chFxDR[fx][(size_t) w] = outR[i] - loR;
                        const float d = baseS + amt * (float)(0.0011 * sr) * hs;          // horn doppler
                        float hiL = rd(chFxDL[fx], d), hiR = rd(chFxDR[fx], d);
                        const float hAM = 1.0f - amt * 0.35f * (0.5f + 0.5f * hc);        // horn AM
                        const float pn  = amt * 0.7f * hc;                                 // horn pan
                        hiL *= hAM * std::sqrt(juce::jlimit(0.0f, 2.0f, 1.0f - pn));
                        hiR *= hAM * std::sqrt(juce::jlimit(0.0f, 2.0f, 1.0f + pn));
                        const float rAM = 1.0f - amt * 0.18f * (0.5f + 0.5f * rs);        // rotor AM (gentle)
                        const float rpn = amt * 0.25f * rs;
                        const float lL = loL * rAM * std::sqrt(juce::jlimit(0.0f, 2.0f, 1.0f - rpn));
                        const float lR = loR * rAM * std::sqrt(juce::jlimit(0.0f, 2.0f, 1.0f + rpn));
                        outL[i] = lL + hiL; outR[i] = lR + hiR;
                        if (++w >= flen) w = 0;
                        ph  += dPh;        if (ph  > 2.0 * kPi) ph  -= 2.0 * kPi;
                        phR += dPh * 0.34; if (phR > 2.0 * kPi) phR -= 2.0 * kPi;   // wraps ITSELF = continuous
                    }
                    chFxW[fx] = w; chFxPhs[fx] = ph; chFxPhs2[fx] = phR; sm = amt;
                } break;
                case ChFxTape:
                {   // TAPE wow/flutter: the whole channel passes through a slowly-warped delay line =
                    // pitch wobble (wow + a faster flutter) + gentle HF softening. CHARACTER = wobble
                    // speed. NOTE: adds a small constant delay (~3 ms) while engaged (disclosed).
                    const int flen = juce::jmax(64, (int) (0.008 * sr));
                    if ((int) chFxDL[fx].size() != flen) { chFxDL[fx].assign((size_t) flen, 0.0f); chFxDR[fx].assign((size_t) flen, 0.0f); chFxW[fx] = 0; chFxPhs[fx] = 0.0; }
                    int w = chFxW[fx]; double ph = chFxPhs[fx]; double phF = chFxPhs2[fx];   // flutter = its OWN accumulator (see the header note)
                    const double rate = typeSync ? syncHz : 0.8 * std::pow(4.0, 2.0 * (double) ch1 - 1.0);   // free: 0.2..3.2 Hz wow
                    const double dPh  = 2.0 * kPi * rate / sr;
                    const float  baseS = (float) (0.003 * sr);
                    const float  hfK   = 1.0f - std::exp(-2.0f * (float) kPi * 6000.0f / (float) sr);
                    float amt = sm;
                    auto rd = [&](const std::vector<float>& buf, float delay) -> float {
                        float rp = (float) w - delay; while (rp < 0.0f) rp += (float) flen;
                        const int i0 = (int) rp; const float fr = rp - (float) i0;
                        const int i1 = (i0 + 1 < flen) ? i0 + 1 : 0;
                        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr; };
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        const float d = baseS + amt * ((float)(0.0016 * sr) * (float) std::sin(ph)
                                                     + (float)(0.00025 * sr) * (float) std::sin(phF));
                        chFxDL[fx][(size_t) w] = outL[i]; chFxDR[fx][(size_t) w] = outR[i];
                        float xl = rd(chFxDL[fx], d), xr = rd(chFxDR[fx], d);
                        chFxPzL[fx][0] += hfK * (xl - chFxPzL[fx][0]);        // tape HF softening (mixed by amount)
                        chFxPzR[fx][0] += hfK * (xr - chFxPzR[fx][0]);
                        outL[i] = xl + (chFxPzL[fx][0] - xl) * amt * 0.35f;
                        outR[i] = xr + (chFxPzR[fx][0] - xr) * amt * 0.35f;
                        if (++w >= flen) w = 0;
                        ph  += dPh;       if (ph  > 2.0 * kPi) ph  -= 2.0 * kPi;
                        phF += dPh * 6.7; if (phF > 2.0 * kPi) phF -= 2.0 * kPi;   // wraps ITSELF = sin stays continuous
                    }
                    chFxW[fx] = w; chFxPhs[fx] = ph; chFxPhs2[fx] = phF; sm = amt;
                } break;
                case ChFxAutoPan:
                {   // AUTO-PAN: equal-power stereo movement (gL = sqrt(1-p), gR = sqrt(1+p), p = amt*sin).
                    // CHARACTER = speed. Unity at amount 0; nothing else in the plugin MOVES the field.
                    double ph = chFxPhs[fx];
                    const double dPh = 2.0 * kPi * (typeSync ? syncHz : 1.4 * std::pow(4.0, 2.0 * (double) ch1 - 1.0)) / sr;   // free: 0.35..5.6 Hz
                    float amt = sm;
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        const float pn = juce::jlimit(-1.0f, 1.0f, amt * (float) std::sin(ph));
                        outL[i] *= std::sqrt(1.0f - pn);
                        outR[i] *= std::sqrt(1.0f + pn);
                        ph += dPh; if (ph > 2.0 * kPi) ph -= 2.0 * kPi;
                    }
                    chFxPhs[fx] = ph; sm = amt;
                } break;
                case ChFxWiden:
                {   // WIDENER: mid/side - the side ABOVE the bass-mono crossover (CHARACTER, 60..600 Hz)
                    // is boosted up to ~2.6x; side lows stay put = wide top, solid mono low end.
                    // Needs STEREO content (unison Width / Chorus / stereo samples) - honest no-op on mono.
                    const double fc2 = 60.0 * std::pow(10.0, (double) ch1);   // 60..600 Hz crossover
                    const float k2 = 1.0f - std::exp(-2.0f * (float) kPi * (float) fc2 / (float) sr);
                    float amt = sm;
                    for (int i = 0; i < numSamples; ++i)
                    {
                        amt += smK * (aTgt[fx] - amt);
                        const float mid = 0.5f * (outL[i] + outR[i]);
                        float side = 0.5f * (outL[i] - outR[i]);
                        chFxPzL[fx][0] += k2 * (side - chFxPzL[fx][0]);       // side LOWS (kept at unity)
                        const float sLo = chFxPzL[fx][0], sHi = side - sLo;
                        side = sLo + sHi * (1.0f + amt * 1.6f);
                        outL[i] = mid + side; outR[i] = mid - side;
                    }
                    sm = amt;
                } break;
                case ChFxChorus:
                default:
                {   // 3-voice stereo ensemble; CHARACTER = rate + depth (0.5 = old 0.36 Hz / 3.5 ms)
                    const int dlen = juce::jmax(64, (int) (0.06 * sr));
                    if ((int) chFxDL[fx].size() != dlen) { chFxDL[fx].assign((size_t) dlen, 0.0f); chFxDR[fx].assign((size_t) dlen, 0.0f); chFxW[fx] = 0; chFxPhs[fx] = 0.0; }
                    int w = chFxW[fx]; double ph = chFxPhs[fx];
                    const double dPh    = 2.0 * kPi * (typeSync ? syncHz : 0.36 * std::pow(4.0, 2.0 * (double) ch1 - 1.0)) / sr;   // free: 0.09..1.44 Hz
                    const float  baseS  = (float) (0.011 * sr);
                    const float  depthS = (float) (0.0035 * sr) * (0.3f + 1.4f * chDepth);   // 0.5 -> x1.0 = the old depth
                    float mix = sm;
                    auto rd = [&](const std::vector<float>& buf, float delay) -> float {
                        float rp = (float) w - delay; while (rp < 0.0f) rp += (float) dlen;
                        const int i0 = (int) rp; const float fr = rp - (float) i0;
                        const int i1 = (i0 + 1 < dlen) ? i0 + 1 : 0;
                        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr; };
                    for (int i = 0; i < numSamples; ++i)
                    {
                        mix += smK * (aTgt[fx] - mix);
                        chFxDL[fx][(size_t) w] = outL[i]; chFxDR[fx][(size_t) w] = outR[i];
                        const float l0 = (float) std::sin(ph);
                        const float l1 = (float) std::sin(ph + 2.0943951);
                        const float l2 = (float) std::sin(ph + 4.1887902);
                        const float wetL = 0.5f * (rd(chFxDL[fx], baseS + depthS * l0) + rd(chFxDL[fx], baseS + depthS * l2));
                        const float wetR = 0.5f * (rd(chFxDR[fx], baseS + depthS * l1) + rd(chFxDR[fx], baseS - depthS * l0));
                        outL[i] = outL[i] * (1.0f - 0.5f * mix) + wetL * (0.5f * mix);
                        outR[i] = outR[i] * (1.0f - 0.5f * mix) + wetR * (0.5f * mix);
                        if (++w >= dlen) w = 0;
                        ph += dPh; if (ph > 2.0 * kPi) ph -= 2.0 * kPi;
                    }
                    chFxW[fx] = w; chFxPhs[fx] = ph; sm = mix;
                } break;
            }
        }
        // Live values for the CHANNEL FX / send fader RINGS: only when a route targets that control.
        auto chTgt = [&](int tg) {
            for (int s2 = 0; s2 < NUM_SLOTS; ++s2) for (auto& r : slots[s2].mod)
                if (r.tgt == tg && r.src != MSOff && std::abs(r.amt) > 1.0e-4f) return true;
            return false; };
        chFxLive[0] = chTgt(MTChFxAAmt) ? aTgt[0] : -1000.0f;
        chFxLive[1] = chTgt(MTChFxAChr) ? cTgt[0] : -1000.0f;
        chFxLive[2] = chTgt(MTChFxBAmt) ? aTgt[1] : -1000.0f;
        chFxLive[3] = chTgt(MTChFxBChr) ? cTgt[1] : -1000.0f;
        chFxLive[4] = chTgt(MTRevSend)  ? juce::jlimit(0.0f, 1.0f, reverbSend + chFxMod[4]) : -1000.0f;
        chFxLive[5] = chTgt(MTDelSend)  ? juce::jlimit(0.0f, 1.0f, delaySend  + chFxMod[5]) : -1000.0f;
        chFxLive[6] = chTgt(MTChFxCAmt) ? aTgt[2] : -1000.0f;
        chFxLive[7] = chTgt(MTChFxCChr) ? cTgt[2] : -1000.0f;
    }

    // Per-slot filter env-follow: remember this block's per-slot level for next block's sweep.
    for (int s = 0; s < NUM_SLOTS; ++s) slotFiltEnv[s] = blockSlotEnv[s];

    // (Channel Punch/Glue were removed - hardcoded to 0 = no effect. The MASTER Glue knob is separate.)

    // Rebuild EQ coefficients on the audio thread if a knob changed
    if (dspDirty.exchange(false))
        updateDSP();

    // Multimode filter is updated every block so the envelope can sweep cutoff
    updateFilter(maxEnvLevel, 1.0);

    applyEQ(renderBuf, numSamples);

    // [2026-07-16] CHANNEL FILTER/EQ (the FILTER/EQ box's CHANNEL chip): a post-FX resonant pair on
    // the FINISHED channel - after the Channel FX, before Duck, so the sends carry the filtered
    // sound. Same Cytomic SVF + filter-drive recipe as the slot filters (mirror them); Off = the
    // whole block is skipped = bit-identical. Matrix "(Channel)" routes move cutoff (octaves) and
    // reso at block rate; the live values land in chFiltLive for the display's mod rings.
    {
        bool anyOn = false;
        for (int f = 0; f < 2; ++f)
            anyOn = anyOn || (chFiltType[f] >= LowPass && chFiltType[f] <= Notch) || chFiltType[f] == Bell;
        if (anyOn)
        {
            const float drv = juce::jlimit(0.0f, 1.0f, chFiltDrive);
            const double dg = 1.0 + 9.0 * drv, dgi = 1.0 / std::sqrt(dg);
            for (int f = 0; f < 2; ++f)
            {
                const int ft = chFiltType[f];
                const bool on = (ft >= LowPass && ft <= Notch) || ft == Bell;
                const int mCut = f == 0 ? 0 : 2, mRes = f == 0 ? 1 : 3;
                if (! on)
                {   chFiltGm[f] = chFiltKm[f] = -1.0f;
                    chFiltIc1[f][0] = chFiltIc1[f][1] = chFiltIc2[f][0] = chFiltIc2[f][1] = 0.0f;   // stale state = a re-enable transient
                    chFiltLive[mCut] = chFiltLive[mRes] = -1000.0f; continue; }
                const float cutHz = juce::jlimit(20.0f, (float)(sr * 0.45),
                    chFiltCutoff[f] * std::exp2(juce::jlimit(-4.0f, 4.0f, chFiltMod[mCut] * 4.0f)));
                const float reso  = juce::jlimit(0.1f, 12.0f, chFiltReso[f] + chFiltMod[mRes] * 4.0f);
                chFiltLive[mCut] = chFiltRouted[mCut] ? cutHz : -1000.0f;
                chFiltLive[mRes] = chFiltRouted[mRes] ? reso  : -1000.0f;
                const float G = std::tan((float) kPi * cutHz / (float) sr);
                float K; double bellM1 = 0.0;
                int fm = 0;
                switch (ft) { case HighPass: fm = 1; break; case BandPass: fm = 2; break;
                              case Notch: fm = 3; break; case Bell: fm = 4; break; default: fm = 0; break; }
                double bellA = 1.0;
                if (fm == 4)
                {   const float A = std::pow(10.0f, juce::jlimit(-15.0f, 15.0f, chFiltGain[f]) / 40.0f);
                    K = 1.0f / (juce::jmax(0.1f, reso) * A); bellA = (double) A; bellM1 = (double) K * ((double) A * A - 1.0); }
                else K = 1.0f / juce::jmax(0.1f, reso);
                juce::ignoreUnused(bellM1);
                if (chFiltGm[f] < 0.0f) { chFiltGm[f] = G; chFiltKm[f] = K; }
                float* dl = renderBuf.getWritePointer(0);
                float* dr = renderBuf.getWritePointer(1);
                const float smK = 0.0025f;
                for (int i = 0; i < numSamples; ++i)
                {
                    chFiltGm[f] += smK * (G - chFiltGm[f]);
                    chFiltKm[f] += smK * (K - chFiltKm[f]);
                    const double g = chFiltGm[f], fk = chFiltKm[f];
                    const double a1 = 1.0 / (1.0 + g * (g + fk)), a2 = g * a1, a3 = g * a2;
                    const double bellM1s = (fm == 4) ? fk * (bellA * bellA - 1.0) : 0.0;   // K live = smoothed boost (the slot Bell recipe)
                    for (int lr = 0; lr < 2; ++lr)
                    {
                        float& ic1 = chFiltIc1[f][lr]; float& ic2 = chFiltIc2[f][lr];
                        const double in = lr == 0 ? dl[i] : dr[i];
                        double v3 = in - ic2;
                        if (drv > 0.0001f) v3 = std::tanh(v3 * dg) * dgi;
                        const double v1 = a1 * ic1 + a2 * v3;
                        const double v2 = ic2 + a2 * ic1 + a3 * v3;
                        ic1 = (float)(2.0 * v1 - ic1); ic2 = (float)(2.0 * v2 - ic2);
                        double y;
                        switch (fm) { case 1: y = in - fk * v1 - v2; break;   // HIGHPASS
                                      case 2: y = v1; break;                  // BANDPASS
                                      case 3: y = in - fk * v1; break;        // NOTCH
                                      case 4: y = in + bellM1s * v1; break;   // BELL
                                      default: y = v2; break; }               // LOWPASS
                        if (lr == 0) dl[i] = (float) y; else dr[i] = (float) y;
                    }
                }
            }
        }
        else
        {   chFiltGm[0] = chFiltGm[1] = chFiltKm[0] = chFiltKm[1] = -1.0f;
            for (int f = 0; f < 2; ++f) chFiltIc1[f][0] = chFiltIc1[f][1] = chFiltIc2[f][0] = chFiltIc2[f][1] = 0.0f;
            for (int cf = 0; cf < 4; ++cf) chFiltLive[cf] = -1000.0f; }
    }

    // SIDECHAIN DUCK: another channel's hits push this one down (duckBy/duckAmt from the Routing
    // popup; the sequencer pulses duckEnv on the trigger). The envelope releases over ~130 ms and
    // the applied gain is slewed ~2 ms so the dip never clicks. The per-slot FX sends duck too
    // (they were accumulated pre-duck in the voice loop). Unlike CHOKE this only lowers the level.
    if (duckBy >= 0 && duckAmt > 0.001f)
    {
        const float rel  = std::exp(-1.0f / (0.13f  * (float) sr));
        const float slew = 1.0f - std::exp(-1.0f / (0.002f * (float) sr));
        auto* dl = renderBuf.getWritePointer(0);
        auto* dr = renderBuf.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float target = 1.0f - duckAmt * duckEnv;
            duckGainZ += (target - duckGainZ) * slew;
            duckEnv *= rel;
            dl[i] *= duckGainZ; dr[i] *= duckGainZ;   // (channel sends tap renderBuf AFTER this = ducked too)
        }
    }
    else { duckEnv = 0.0f; duckGainZ = 1.0f; }

    // Feed the analyser if this channel is being inspected: the final mix (All), or - when a
    // slot is selected on the EQ - just THAT slot's signal (captured pre-mix above).
    if (analysisTap != nullptr)
    {
        if (tapSlot >= 0)
        {
            const int nn = juce::jmin(numSamples, analysisBufLen);
            for (int i = 0; i < nn; ++i) analysisTap->push(analysisBuf[i]);
        }
        else
        {
            const auto* l = renderBuf.getReadPointer(0);
            const auto* r = renderBuf.getReadPointer(1);
            for (int i = 0; i < numSamples; ++i) analysisTap->push(0.5f * (l[i] + r[i]));
        }
    }
    if (tunerTap != nullptr)   // REAL tuner: continuous decimated mono feed of this channel
    {
        const auto* l = renderBuf.getReadPointer(0);
        const auto* r = renderBuf.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i) tunerTap->push(0.5f * (l[i] + r[i]));
    }

    // Apply volume and pan then mix into dest
    const float effVol = juce::jlimit(0.0f, 1.5f, volume);
    const float effPan = juce::jlimit(-1.0f, 1.0f, pan);
    float gainL = effVol * (effPan <= 0.0f ? 1.0f : 1.0f - effPan);
    float gainR = effVol * (effPan >= 0.0f ? 1.0f : 1.0f + effPan);

    // Post-fader peak for the channel's level meter (UI). Peak amplitude is rate-independent.
    {
        const float* rl = renderBuf.getReadPointer(0);
        const float* rr = renderBuf.getReadPointer(1);
        float pk = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            pk = juce::jmax(pk, std::abs(rl[i]) * gainL, std::abs(rr[i]) * gainR);
        // PEAK-HOLD: keep the MAX since the UI last read it (the UI resets it to 0 on read). Storing only
        // this block's peak made the 24Hz UI sample a random point in the decay -> the meter looked random.
        meterPeak.store(juce::jmax(meterPeak.load(std::memory_order_relaxed), pk), std::memory_order_relaxed);
    }

    // CHANNEL SENDS (post channel-FX / EQ / duck): how much of the FINISHED channel feeds the shared
    // reverb / delay. Gains are matrix-modulatable (Reverb/Delay Send targets -> chFxMod[4]/[5]) and
    // smoothed PER SAMPLE; the REVERB send is HIGH-PASSED ~150 Hz (fixed, disclosed) so subs stay dry
    // - the classic mixing move, and it preserves the old "wet top / dry sub layer" trick without
    // per-slot sends. revBus/delBus pick which shared bus (A or B) this channel feeds.
    {
        const float revTgt = juce::jlimit(0.0f, 1.0f, reverbSend + chFxMod[4]);
        const float delTgt = juce::jlimit(0.0f, 1.0f, delaySend  + chFxMod[5]);
        if (chSendSmR < 0.0f) chSendSmR = revTgt;   // first use = snap (bit-identical when constant)
        if (chSendSmD < 0.0f) chSendSmD = delTgt;
        auto* rBus = (revBus != 0 && reverbSendBusB != nullptr) ? reverbSendBusB : reverbSendBus;
        auto* dBus = (delBus != 0 && delaySendBusB  != nullptr) ? delaySendBusB  : delaySendBus;
        const float smK2 = 1.0f - std::exp(-1.0f / (0.004f * (float) sr));
        const auto* rb0 = renderBuf.getReadPointer(0);
        const auto* rb1 = renderBuf.getReadPointer(1);
        if (rBus != nullptr && (chSendSmR > 1.0e-4f || revTgt > 1.0e-4f))
        {
            const float hpK = 1.0f - std::exp(-2.0f * (float) kPi * 150.0f / (float) sr);
            float* oL = rBus->getWritePointer(0) + startSample;
            float* oR = rBus->getWritePointer(1) + startSample;
            float sm = chSendSmR;
            for (int i = 0; i < numSamples; ++i)
            {
                sm += smK2 * (revTgt - sm);
                const float xl = rb0[i] * gainL, xr = rb1[i] * gainR;
                chSendHpZ[0] += hpK * (xl - chSendHpZ[0]);   // 1-pole split; send the part ABOVE ~150 Hz
                chSendHpZ[1] += hpK * (xr - chSendHpZ[1]);
                oL[i] += (xl - chSendHpZ[0]) * sm;
                oR[i] += (xr - chSendHpZ[1]) * sm;
            }
            chSendSmR = sm;
        }
        else chSendSmR = revTgt;
        if (dBus != nullptr && (chSendSmD > 1.0e-4f || delTgt > 1.0e-4f))
        {
            float* oL = dBus->getWritePointer(0) + startSample;
            float* oR = dBus->getWritePointer(1) + startSample;
            float sm = chSendSmD;
            for (int i = 0; i < numSamples; ++i)
            {
                sm += smK2 * (delTgt - sm);
                oL[i] += rb0[i] * gainL * sm;
                oR[i] += rb1[i] * gainR * sm;
            }
            chSendSmD = sm;
        }
        else chSendSmD = delTgt;
    }

    for (int ch = 0; ch < destChannels; ++ch)
    {
        float g = (ch == 0) ? gainL : gainR;
        dest.addFrom(ch, startSample, renderBuf, ch % 2, 0, numSamples, g);
    }
}

// Signal chain per sample:  multimode filter → drive → 8-band EQ
void DrumChannel::applyEQ(juce::AudioBuffer<float>& buf, int numSamples)
{
    const bool useFormant      = (filterType == Formant);
    const bool useLegacyFilter = (filterType != FilterOff && filterType != Formant);  // factory LP/HP/BP/Notch
    const bool useDrive        = (driveType != DriveOff && driveAmount > 0.0f);

    // -- Stage 1: filter (Formant, or a factory LP/HP/BP/Notch) --
    if (useFormant || useLegacyFilter)
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = buf.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                float x = d[i];
                if (useFormant) x = (formantBP[0].process(x, ch) * 1.0f
                                   + formantBP[1].process(x, ch) * 0.7f
                                   + formantBP[2].process(x, ch) * 0.45f) * 2.2f;
                else            x = filter.process(x, ch);
                d[i] = x;
            }
        }

    // -- Stage 2: DRIVE (runs at the channel's internal oversampled rate, so it's anti-aliased) --
    if (useDrive)
    {
        const bool harsh = driveType == HardClip || driveType == Foldback || driveType == Fuzz;
        const float k = 1.0f - std::exp(-2.0f * kPi * 8000.0f / (float) juce::jmax(1.0, sr));
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = buf.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                float y = applyDrive(d[i]);
                if (harsh)                                            // same v6 musicality pass as the slot drive:
                {
                    chDrvLp[ch] += k * (y - chDrvLp[ch]); y = chDrvLp[ch];   // ~8 kHz anti-fizz
                    if (driveType == Fuzz)                            // DC blocker (rectified blend adds offset)
                    { const float o = y - chDrvDcX[ch] + 0.9995f * chDrvDcY[ch];
                      chDrvDcX[ch] = y; chDrvDcY[ch] = o; y = o; }
                }
                d[i] = y;
            }
        }
    }

    // (Stage 3, the channel 5-band EQ, was DELETED 2026-07-16 with the rest of the old EQ code.)
}

void DrumChannel::updateFilter(float envModLevel, double cutoffMul)
{
    if (sr <= 0.0 || filterType == FilterOff) return;

    const double nyq = sr * 0.49;
    double cutoff = (double) filterCutoff * cutoffMul;   // cutoffMul = Mod-LFO filter sweep (1 = none)
    if (filterEnvAmt != 0.0f)
        cutoff *= std::pow(2.0, (double) filterEnvAmt * (double) envModLevel * 5.0); // ±5 octaves
    cutoff = juce::jlimit(20.0, nyq, cutoff);
    double Q = juce::jlimit(0.3, 12.0, (double) filterReso);

    if (filterType == Formant)
    {
        // Cutoff position (20..20k, log) selects/morphs the vowel A->E->I->O->U.
        const float u = juce::jlimit(0.0f, 1.0f, (float)(std::log(cutoff / 20.0) / std::log(1000.0)));
        const float pos = u * 4.0f;
        const int   v0 = juce::jlimit(0, 4, (int) pos), v1 = juce::jmin(4, v0 + 1);
        const float fr = pos - (float) v0;
        const double fq = juce::jlimit(3.0, 18.0, 4.0 + (double) filterReso * 9.0); // formant sharpness
        for (int k = 0; k < 3; ++k)
        {
            const double f = juce::jlimit(40.0, nyq, (double) (kVowels[v0][k] + fr * (kVowels[v1][k] - kVowels[v0][k])));
            formantBP[k].bandpass(sr, f, fq);
        }
        return;
    }
    // Legacy filter (factory sounds only; not user-selectable - users use the drawable EQ).
    switch (filterType)
    {
        case LowPass:  filter.lowpass (sr, cutoff, Q); break;
        case HighPass: filter.highpass(sr, cutoff, Q); break;
        case BandPass: filter.bandpass(sr, cutoff, Q); break;
        case Notch:    filter.notch   (sr, cutoff, Q); break;
        default: break;
    }
}

float DrumChannel::applyDrive(float x) const { return driveSample(x, driveType, driveAmount); }
float DrumChannel::driveSample(float x, int driveType, float driveAmount)
{
    if (driveType == DriveExciter)
    {   // EXCITER: synthesized 2nd + 3rd harmonics BLENDED IN (Chebyshev-style on a soft-limited
        // copy) - the dry passes untouched, so dynamics survive: "bigger without distorting".
        // (The t*t term carries DC - the render's post stage DC-blocks this type.)
        const float t = std::tanh(x * 1.4f);
        return x + driveAmount * (1.15f * t * t + 0.75f * t * t * t);
    }
    // v6 PERCEPTUAL TAPER: gain rises with amount SQUARED, so the musical range spreads across the
    // whole control instead of hitting near-square-wave by ~20% (the old linear 1+24a did). Factory
    // drive amounts were remapped a -> sqrt(a) (and old user files migrate on load), which lands on
    // the EXACT same gain -> bit-identical playback for existing sounds. Bitcrush keeps its own
    // amount law below (bits/pregain read the raw amount; unchanged + never remapped).
    const float a  = juce::jlimit(0.0f, 1.0f, driveAmount);
    const float g  = 1.0f + a * a * 24.0f;
    const float xg = x * g;
    float y = xg;

    switch (driveType)
    {
        case SoftClip:  // smooth tanh saturation — warm, rounded
            y = std::tanh(xg);
            break;

        case HardClip:  // brick-wall clip — aggressive, buzzy
            y = juce::jlimit(-1.0f, 1.0f, xg);
            break;

        case Tube:      // asymmetric soft clip — adds EVEN harmonics (thicker, "tube")
        {
            // Bias one side so the transfer curve is asymmetric, then remove DC.
            const float bias = 0.35f;
            y = std::tanh(xg + bias) - std::tanh(bias);
            y *= 1.2f;
            break;
        }

        case Foldback:  // wavefolder — reflects past +/-1, very buzzy/metallic
        {
            float v = xg * 0.6f;
            for (int k = 0; k < 4 && (v > 1.0f || v < -1.0f); ++k)
            {
                if (v >  1.0f) v =  2.0f - v;
                if (v < -1.0f) v = -2.0f - v;
            }
            y = v;
            break;
        }

        case Fuzz:      // hard asymmetric + rectify flavour — gnarly, octave-ish
        {
            float a = std::tanh(xg * 1.5f);
            float rect = std::abs(a);                 // even-harmonic rectified blend
            y = juce::jlimit(-1.0f, 1.0f, a * 0.6f + (rect - 0.3f) * 0.8f);
            break;
        }

        case Bitcrush:  // digital sample/bit reduction — lo-fi, crunchy
        {
            // Bit depth shrinks as drive rises (12 bits -> ~3 bits)
            float bits  = juce::jmap(driveAmount, 0.0f, 1.0f, 12.0f, 3.0f);
            float steps = std::pow(2.0f, bits);
            float c = juce::jlimit(-1.0f, 1.0f, x * (1.0f + driveAmount * 2.0f));
            y = std::round(c * steps) / steps;
            return y; // already unity-ish, skip the tanh make-up below
        }

        default: break;
    }

    // Make-up expressed via the GAIN (not the raw amount): 1/(1+(g-1)/8) == the old 1/(1+3a) under
    // the old linear taper, so the remapped factory amounts reproduce the old output EXACTLY.
    return y * (1.0f / (1.0f + (g - 1.0f) * 0.125f));
}

void DrumChannel::updateDSP()
{
    if (sr <= 0.0) return;

    // (the channel 5-band EQ coeff bake was DELETED 2026-07-16 with the old EQ code)
    juce::ignoreUnused(sr);
}
