#include "DrumChannel.h"
#include <vector>
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

// 4-point cubic Hermite (Catmull-Rom) interpolation. fr is 0..1 between y1 and y2.
static inline float hermite4(float fr, float y0, float y1, float y2, float y3) noexcept
{
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 1.5f * (y1 - y2) + 0.5f * (y3 - y0);
    return ((c3 * fr + c2) * fr + c1) * fr + y1;
}

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
        st.setProperty("chd", s.chordMode, nullptr); st.setProperty("chdU", s.chordUnison, nullptr);
        st.setProperty("scOn", s.scaleOn, nullptr); st.setProperty("scTy", s.scaleType, nullptr);
        st.setProperty("scUn", s.scaleUnison, nullptr); st.setProperty("scKy", s.scaleKey, nullptr);   // SCALE mode
        st.setProperty("oUC", s.oscUniCenter, nullptr); st.setProperty("oDM", s.oscDetuneMode, nullptr);
        st.setProperty("fxDt", s.fxDriveType, nullptr); st.setProperty("fxDr", s.fxDrive, nullptr);
        st.setProperty("fxRv", s.fxReverbSend, nullptr); st.setProperty("fxDl", s.fxDelaySend, nullptr);
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
        // === PER-SLOT EQ (begin) ===
        for (int e = 0; e < NUM_EQ_BANDS; ++e) {
            const auto& eb = s.eqBand[e]; const juce::String k = "qe" + juce::String(e);
            st.setProperty(k + "o", eb.on, nullptr); st.setProperty(k + "f", eb.freq, nullptr);
            st.setProperty(k + "g", eb.gainDb, nullptr); st.setProperty(k + "q", eb.q, nullptr);
        }
        // === PER-SLOT EQ (end) ===
        // === PER-SLOT FILTER (begin) ===
        st.setProperty("flT", s.filterType, nullptr); st.setProperty("flC", s.filterCutoff, nullptr);
        st.setProperty("flR", s.filterReso, nullptr); st.setProperty("flE", s.filterEnvAmt, nullptr);
        st.setProperty("flK", s.filterKeyTrack, nullptr);   // filter cutoff keytrack (0 = off)
        st.setProperty("flT2", s.filterType2, nullptr); st.setProperty("flC2", s.filterCutoff2, nullptr);   // FILTER 2 (series)
        st.setProperty("flR2", s.filterReso2, nullptr); st.setProperty("flE2", s.filterEnvAmt2, nullptr);
        st.setProperty("flK2", s.filterKeyTrack2, nullptr);
        // === PER-SLOT FILTER (end) ===
        // === PER-SLOT CHORUS ===
        st.setProperty("chM", s.chorusMix, nullptr);   // (chRt/chDp retired - rate/depth are effect constants)
        st.setProperty("fxTn", s.fxTone, nullptr); st.setProperty("fxPn", s.fxPunch, nullptr);
        st.setProperty("fxCp", s.fxComp, nullptr);
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
            st.setProperty("lfSR" + juce::String(d2), s.lfoSyncRate[d2], nullptr);   // sync rate multiplier index
            st.setProperty("lfSh" + juce::String(d2), s.lfoShape[d2], nullptr);  // wave shape (0 sine .. 7 custom)
            st.setProperty("lfFr" + juce::String(d2), s.lfoFree[d2], nullptr);   // free-run (timeline-anchored)
            st.setProperty("lfLg" + juce::String(d2), s.lfoLegato[d2], nullptr); // legato retrig
            if (s.lfoShape[d2] == 7)                                              // LFO SHAPER drawn curve
            { juce::String cv;
              for (int k = 0; k < Slot::LFO_CURVE_N; ++k)
                  cv << juce::String(s.lfoCurve[d2][k], 3) << (k < Slot::LFO_CURVE_N - 1 ? "," : "");
              st.setProperty("lfCv" + juce::String(d2), cv, nullptr); }
        }
        st.setProperty("drf", s.drift, nullptr);          // DRIFT (alive) amount
        st.setProperty("flDrv", s.filterDrive, nullptr);  // filter loop saturation
        // MOD MATRIX: routes packed "src:tgt:amt;..." + the two matrix-created source params.
        { juce::String mm;
          for (int r = 0; r < MOD_ROUTES; ++r)
              mm << (int) s.mod[r].src << ":" << (int) s.mod[r].tgt << ":" << juce::String(s.mod[r].amt, 4) << ";";
          st.setProperty("mmx", mm, nullptr); }
        st.setProperty("mEA", s.modEnvA, nullptr); st.setProperty("mED", s.modEnvD, nullptr);
        st.setProperty("mLR", s.modLfoRate, nullptr); st.setProperty("mLS", s.modLfoShape, nullptr);
        parent.addChild(st, -1, nullptr);
    }
}

bool DrumChannel::readSlots(const juce::ValueTree& parent)
{
    int n = 0;
    const Slot d;   // struct defaults used as per-field fallbacks
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
        s.chordMode = (int) st.getProperty("chd", d.chordMode); s.chordUnison = (int) st.getProperty("chdU", d.chordUnison);
        s.scaleOn = (bool) st.getProperty("scOn", d.scaleOn); s.scaleType = (int) st.getProperty("scTy", d.scaleType);
        s.scaleUnison = (int) st.getProperty("scUn", d.scaleUnison); s.scaleKey = (int) st.getProperty("scKy", d.scaleKey);
        s.oscUniCenter = (bool)st.getProperty("oUC", d.oscUniCenter);
        s.oscDetuneMode = (int)st.getProperty("oDM", d.oscDetuneMode);
        s.fxDriveType = (int)st.getProperty("fxDt", d.fxDriveType); s.fxDrive = (float)st.getProperty("fxDr", d.fxDrive);
        // v6 drive-taper migration: pre-v6 amounts were stored for the LINEAR gain law; sqrt lands on
        // the identical gain under the new SQUARED law (Bitcrush keeps its own amount semantics).
        if (! st.hasProperty("v6") && s.fxDriveType != Bitcrush && s.fxDrive > 0.0f)
            s.fxDrive = std::sqrt(juce::jlimit(0.0f, 1.0f, s.fxDrive));
        s.fxReverbSend = (float)st.getProperty("fxRv", d.fxReverbSend); s.fxDelaySend = (float)st.getProperty("fxDl", d.fxDelaySend);
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
        // === PER-SLOT EQ (begin) ===
        for (int e = 0; e < NUM_EQ_BANDS; ++e) {
            auto& eb = s.eqBand[e]; const EqBand de = defaultEqBand(e); const juce::String k = "qe" + juce::String(e);
            eb.on = (bool)st.getProperty(k + "o", false); eb.freq = (float)st.getProperty(k + "f", de.freq);
            eb.gainDb = (float)st.getProperty(k + "g", 0.0f); eb.q = (float)st.getProperty(k + "q", de.q);
        }
        // === PER-SLOT EQ (end) ===
        // === PER-SLOT FILTER (begin) ===
        s.filterType = (int)st.getProperty("flT", d.filterType); s.filterCutoff = (float)st.getProperty("flC", d.filterCutoff);
        s.filterReso = (float)st.getProperty("flR", d.filterReso); s.filterEnvAmt = (float)st.getProperty("flE", d.filterEnvAmt);
        s.filterKeyTrack = (float)st.getProperty("flK", d.filterKeyTrack);
        s.filterType2   = (int)  st.getProperty("flT2", d.filterType2);   s.filterCutoff2 = (float)st.getProperty("flC2", d.filterCutoff2);
        s.filterReso2   = (float)st.getProperty("flR2", d.filterReso2);   s.filterEnvAmt2 = (float)st.getProperty("flE2", d.filterEnvAmt2);
        s.filterKeyTrack2 = (float)st.getProperty("flK2", d.filterKeyTrack2);
        // === PER-SLOT FILTER (end) ===
        // === PER-SLOT CHORUS ===
        s.chorusMix   = (float)st.getProperty("chM",  d.chorusMix);   // old files' chRt/chDp ignored (constants now)
        s.fxTone  = (float)st.getProperty("fxTn", d.fxTone);
        s.fxPunch = (float)st.getProperty("fxPn", d.fxPunch);
        s.fxComp  = (float)st.getProperty("fxCp", d.fxComp);
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
            s.lfoSyncRate[d2] = (int)st.getProperty("lfSR" + juce::String(d2), d.lfoSyncRate[d2]);
            s.lfoShape[d2] = juce::jlimit(0, 7, (int) st.getProperty("lfSh" + juce::String(d2), d.lfoShape[d2]));
            s.lfoFree[d2]  = (bool) st.getProperty("lfFr" + juce::String(d2), d.lfoFree[d2]);
            s.lfoLegato[d2] = (bool) st.getProperty("lfLg" + juce::String(d2), d.lfoLegato[d2]);
            if (st.hasProperty("lfCv" + juce::String(d2)))
            { juce::StringArray a; a.addTokens(st.getProperty("lfCv" + juce::String(d2)).toString(), ",", "");
              for (int k = 0; k < Slot::LFO_CURVE_N; ++k)
                  s.lfoCurve[d2][k] = juce::jlimit(-1.0f, 1.0f, k < a.size() ? a[k].getFloatValue() : 0.0f); }
        }
        s.drift       = juce::jlimit(0.0f, 1.0f, (float) st.getProperty("drf", d.drift));
        s.filterDrive = juce::jlimit(0.0f, 1.0f, (float) st.getProperty("flDrv", d.filterDrive));
        // MOD MATRIX (old files have no "mmx" -> all routes stay Off = default = unchanged sound).
        if (st.hasProperty("mmx"))
        {
            juce::StringArray rows; rows.addTokens(st.getProperty("mmx").toString(), ";", "");
            for (int r = 0; r < MOD_ROUTES && r < rows.size(); ++r)
            {
                juce::StringArray f2; f2.addTokens(rows[r], ":", "");
                if (f2.size() >= 3)
                { s.mod[r].src = (int8_t) juce::jlimit(0, MS_COUNT - 1, f2[0].getIntValue());
                  s.mod[r].tgt = (int8_t) juce::jlimit(0, MT_COUNT - 1, f2[1].getIntValue());
                  s.mod[r].amt = juce::jlimit(-1.0f, 1.0f, f2[2].getFloatValue()); }
            }
        }
        s.modEnvA = juce::jlimit(0.001f, 8.0f, (float) st.getProperty("mEA", d.modEnvA));
        s.modEnvD = juce::jlimit(0.01f, 8.0f, (float) st.getProperty("mED", d.modEnvD));
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
    if (n > 0) ensureKsBuffers();   // restored slots may use a KS engine (message thread)
    rebuildAddTables();   // drawn harmonics -> tables (message thread; load paths)
    return n > 0;
}

void DrumChannel::prepareToPlay(double sampleRate, int maxBlockSize)
{
    rebuildAddTables();   // custom (additive) waves must exist before audio runs
    sr = sampleRate;
    waveBank();   // force-build the wavetable bank now (message thread), not on the audio thread

    for (auto& b : eqHP) b.reset();  for (auto& b : eqBell) b.reset();  for (auto& b : eqLP) b.reset();
    for (auto& b : formantBP) b.reset();

    for (auto& v : voices)
    {
        v.playHead = -1.0;
        v.killing = false; v.killGain = 1.0f;
        for (auto& sv : v.sv) sv.noiseBP.reset();
    }

    renderBuf.setSize(2, maxBlockSize);
    fxSendBuf.setSize(4, maxBlockSize);   // per-slot reverb/delay send accumulation (revL,revR,delL,delR)

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

// Chord-mode intervals (semitones): unison voice u plays triad note u%3, +1 octave per wrap.
// Major = root / +4 (major third) / +7 (perfect fifth); minor = root / +3 (minor third) / +7.
// Chord tables: semitone offset from the root for unison voice k, octave-extended past the
// core so any voice count works. chordMode: 0 = STD (plain detune), 1..7 = these. Shared by
// Osc / Modal / Physical so all three engines voice a chord the same way.
static const int8_t kChordTab[7][6] = {
    { 0, 12, 24, 36, 48, 60 },   // 1 Oct   (octaves)
    { 0,  7, 12, 19, 24, 31 },   // 2 5th   (power chord)
    { 0,  4,  7, 12, 16, 19 },   // 3 Maj
    { 0,  3,  7, 12, 15, 19 },   // 4 Min
    { 0,  5,  7, 12, 17, 19 },   // 5 Sus4
    { 0,  4,  7, 11, 12, 16 },   // 6 Maj7
    { 0,  3,  7, 10, 12, 15 },   // 7 Min7
};
static inline int chordSemis(int chordMode, int k)
{ return (chordMode >= 1 && chordMode <= 7) ? (int) kChordTab[chordMode - 1][juce::jlimit(0, 5, k)] : 0; }

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
int DrumChannel::chordNoteOffset(int chordMode, int k) { return chordSemis(chordMode, k); }

int DrumChannel::trigger(float velocityGain, float pitchSemis, float pan, long gateSamples,
                         float glideToSemis, long glideSamples, bool forceOverlap, int slotMask, bool keyGate,
                         bool knobBase)
{
    // slotMask 0 (or all-bits) = every slot sounds; a piano-roll note may restrict to slot 1 or 2.
    const int mask = (slotMask == 0) ? ~0 : slotMask;
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
        for (int i = 1; i < POLY; ++i) voices[i].playHead = -1.0;
        for (auto& b : eqHP) b.reset();  for (auto& b : eqBell) b.reset();  for (auto& b : eqLP) b.reset();
        for (auto& b : formantBP) b.reset();
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
            sv.filtIc1[fi][0] = sv.filtIc1[fi][1] = sv.filtIc2[fi][0] = sv.filtIc2[fi][1] = 0.0; sv.filtGm[fi] = -1.0; }
        for (int lr = 0; lr < 2; ++lr) {   // drive smoothing/DC + tone + punch state start clean each hit
            sv.drvLp[lr] = sv.drvDcX[lr] = sv.drvDcY[lr] = 0.0f;
            sv.toneZ[lr] = 0.0f; sv.pFast[lr] = sv.pSlow[lr] = 0.0f; }
        sv.fmCarrier = sv.fmMod = sv.fmSubPhase = 0.0; sv.fmFbState = 0.0f;
        sv.wtPhase = 0.0; sv.modalInit = false; sv.modalHold = false;   // re-strike the modal bank on this hit
        for (auto& row : sv.modalY1) for (auto& v2 : row) v2 = 0.0f;   // clean resonator state every hit
        for (auto& row : sv.modalY2) for (auto& v2 : row) v2 = 0.0f;
        sv.noiseState = 0x1234567u + (uint32_t)(vi * NUM_SLOTS + s) * 2654435761u; // distinct per voice+slot
        for (int u = 0; u <= UNI_MAX; ++u) sv.uniPhase[u] = (2.0 * kPi) * (double) u / (double) UNI_MAX;

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
        if (sl.scaleOn && (sl.engine == SrcOsc || sl.engine == SrcModal || sl.engine == SrcPhys))
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
        if (sAmt > 0.001f && (sl.scaleOn || sl.chordMode > 0)
            && (sl.engine == SrcOsc || sl.engine == SrcModal || sl.engine == SrcPhys))
        {
            const int nStr = juce::jlimit(1, UNI_MAX, sl.scaleOn ? (sl.scaleType >= 10 ? 6 : sl.scaleUnison)
                                                    : sl.chordMode > 0 ? sl.chordUnison : sl.oscUnison);
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
                    const double interval = sl.scaleOn ? (double) sv.uniSemis[k] : (double) chordSemis(sl.chordMode, k);
                    uniMul = std::pow(2.0, interval / 12.0
                                          + sp * (double) (sl.oscDetune * 100.0f) / 1200.0);
                }
                exciteString(k, ksFreq0 * (float) uniMul);
            }
        }
    }
    return vi;
}

// KEYS (on-screen keyboard): play `midiNote` on the selected channel's ELIGIBLE slots.
// MONO: whatever is ringing gets the 3 ms choke fade, then ONE fresh voice starts. Each eligible
// slot is re-tuned from its OWN base Freq to the pressed note (so both slots sound the same
// musical pitch regardless of their Freq knobs); slot 2 can be transposed DOWN by slot2Down
// semitones (sub-oscillator style). Ineligible slots (Sample/Noise/legacy) stay silent.
int DrumChannel::keyDown(int midiNote, float velocity, int slot2Down, bool poly, int slotMask)
{
    // MONO LEGATO GLIDE (portamento): if a key is still HELD when this new one is pressed and Glide > 0,
    // the new note SLIDES from the held note's pitch to the new pitch. Poly never glides. Glide 0 = the
    // old instant behaviour (glideFrom/glideSamp both 0 -> the trigger() call below is bit-identical).
    long  glideSamp = 0; float glideFrom = 0.0f;
    if (! poly)
    {
        if (keysGlide > 0.0001f)
        {
            int prevNote = -1;
            for (auto& vv : voices)
                if (vv.active() && vv.isKey && vv.keyOff < 0 && vv.keyNote >= 0) { prevNote = vv.keyNote; break; }
            if (prevNote >= 0 && prevNote != midiNote)
            {
                glideFrom = (float) (prevNote - midiNote);                              // start this many semitones off target
                glideSamp = (long) (juce::jmax(0.005f, keysGlide * 0.4f) * (float) sr); // up to 400 ms
            }
        }
        fadeOutVoices(0.015f);                         // 15 ms handover (3 ms crackled on slides)
    }
    // KEYBOARD = ABSOLUTE PITCH, KNOB UNTOUCHED (2026-07-08, user spec): the old block here
    // re-based every slot's Freq to C4 on key press - REMOVED. keySemis below already targets the
    // pressed note ABSOLUTELY (12*log2(target/base)), so the keyboard plays real notes no matter
    // where the Freq knob sits, and never rewrites it. Step recordings are stored as FRACTIONAL
    // offsets from the knob (processor stamp), so playback still reproduces the performance
    // exactly. TEST + step pitch 0 keep meaning "whatever the knob says" (the drum contract).
    const int vi = trigger(velocity, glideFrom, 0.0f, 0, /*glideTo*/ 0.0f, glideSamp, /*forceOverlap*/ true, slotMask);
    Voice& v = voices[vi];
    v.isKey = true; v.keyOff = -1; v.keyNote = midiNote;   // tag: keyUp(note) releases only this note's voices
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
        case SrcPhys:  switch (idx) { case 0: return F(&S::physFreq,20,4186); case 1: return none;   /* Material */
                                      case 2: return F(&S::physStiff,0,1); case 3: return none; } break;   /* Excite */
        case SrcGrain: switch (idx) { case 0: return F(&S::grainPos,0,1); case 1: return F(&S::grainSize,0,1);
                                      case 2: return F(&S::grainDens,0,1); case 3: return F(&S::grainSpray,0,1);
                                      case 4: return F(&S::grainPitch,0,1); } break;
        case SrcSample:switch (idx) { case 0: return F(&S::smpGain,0,4); case 1: return F(&S::smpCrush,0,1);
                                      case 2: return F(&S::smpStretch,0.25f,4); } break;
        case SrcModal: switch (idx) { case 0: return F(&S::oscFreq,20,4186); case 1: return none;   /* Material */
                                      case 2: return F(&S::modalMorph,0,1); case 3: return F(&S::modalTone,0,1);
                                      case 4: return F(&S::modalStruct,0,1); } break;
        default: break;
    }
    return none;
}

// Sample the 6 sources once per block for slot s (block-rate = the config bake's rate). All reads come
// from the NEWEST active voice + the slot's per-block state, so a poly stack samples one voice's cues
// (mono-ish semantics, disclosed). Values: Vel/AmpEnv/Random/ModEnv in 0..1, Note/LFOs/ModLFO in -1..1.
void DrumChannel::computeModSources(int s, const Slot& sl, float* out) const
{
    for (int i = 0; i < MS_COUNT; ++i) out[i] = 0.0f;
    const Voice* nv = nullptr;
    for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;

    out[MSVel]    = nv != nullptr ? juce::jlimit(0.0f, 1.0f, nv->velGain) : 0.0f;
    { const float note = nv != nullptr ? (nv->keyNote >= 0 ? (float) nv->keyNote : 60.0f + nv->voicePitch) : 60.0f;
      out[MSNote] = juce::jlimit(-1.0f, 1.0f, (note - 60.0f) / 24.0f); }         // +-24 st -> +-1
    out[MSAmpEnv] = juce::jlimit(0.0f, 1.0f, slotFiltEnv[juce::jlimit(0, NUM_SLOTS - 1, s)]);   // prev-block amp level
    out[MSRandom] = nv != nullptr ? nv->sv[juce::jlimit(0, NUM_SLOTS - 1, s)].modRand : 0.5f;

    // Mod Env: stateless AD from the newest voice's age.
    if (nv != nullptr)
    {
        const float t = (float) nv->voiceSamples / (float) juce::jmax(1.0, sr);
        const float A = juce::jmax(0.001f, sl.modEnvA), D = juce::jmax(0.01f, sl.modEnvD);
        out[MSModEnv] = t < A ? t / A : std::exp(-(t - A) * 3.0f / D);
    }

    // The four existing per-slot LFOs, read at the newest voice's block-start phase (retrig) or the
    // timeline-anchored free phase. Value is the raw shape (-1..1); the route amount is the depth.
    for (int d = 0; d < 4; ++d)
    {
        float cpb = sl.lfoSync[d];
        if (cpb < 0.0f) cpb = (float) juce::jmax(1, drawMode ? lfoGridDiv : numSteps);
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
        out[MSLfoFilt + d] = lfoShapeVal(sl.lfoShape[d], ph, cyc, sl.lfoCurve[d]);
    }

    // Mod LFO: matrix-created free-run LFO, timeline-anchored so playback passes match.
    {
        const double sec = (lfoBarPos >= 0.0 ? lfoBarPos * (double) lfoBarSeconds : lfoFreeSec);
        const double ph  = 2.0 * kPi * (double) juce::jlimit(0.05f, 20.0f, sl.modLfoRate) * sec;
        out[MSModLfo] = lfoShapeVal(juce::jlimit(0, 6, sl.modLfoShape), ph, (uint32_t) juce::jmax(0.0, ph / (2.0 * kPi)));
    }
}

// Apply the 6 routes onto a scratch Slot before the config bake. Cutoffs + pitch are MULTIPLICATIVE
// (octaves); everything else is ADDITIVE over the target's native range, then clamped.
void DrumChannel::applyModMatrix(Slot& tmp, const float* srcVals) const
{
    for (auto& r : tmp.mod)
    {
        if (r.tgt == MTOff || r.src == MSOff || std::abs(r.amt) < 1.0e-4f) continue;
        const float m = r.amt * srcVals[juce::jlimit(0, MS_COUNT - 1, (int) r.src)];   // depth * source
        auto add = [&](float& f, float mn, float mx) { f = juce::jlimit(mn, mx, f + m * (mx - mn)); };
        switch (r.tgt)
        {
            case MTFilt1Cut: tmp.filterCutoff  = juce::jlimit(20.0f, 20000.0f, tmp.filterCutoff  * std::pow(2.0f, m * 4.0f)); break;
            case MTFilt2Cut: tmp.filterCutoff2 = juce::jlimit(20.0f, 20000.0f, tmp.filterCutoff2 * std::pow(2.0f, m * 4.0f)); break;
            case MTFilt1Res: tmp.filterReso  = juce::jlimit(0.4f, 12.0f, tmp.filterReso  + m * 11.6f); break;
            case MTFilt2Res: tmp.filterReso2 = juce::jlimit(0.4f, 12.0f, tmp.filterReso2 + m * 11.6f); break;
            case MTDrive:    add(tmp.fxDrive, 0, 1); break;
            case MTRevSend:  add(tmp.fxReverbSend, 0, 1); break;
            case MTDelSend:  add(tmp.fxDelaySend, 0, 1); break;
            case MTChorus:   add(tmp.chorusMix, 0, 1); break;
            case MTTone:     add(tmp.fxTone, -1, 1); break;
            case MTPunch:    add(tmp.fxPunch, -1, 1); break;
            case MTComp:     add(tmp.fxComp, 0, 1); break;
            case MTAtk:      tmp.atk = juce::jlimit(0.0f, 6.0f, tmp.atk + m * 1.0f); break;
            case MTDec:      tmp.dec = juce::jlimit(0.0f, 6.0f, tmp.dec + m * 2.0f); break;
            case MTSus:      add(tmp.sustain, 0, 1); break;
            case MTRel:      tmp.release = juce::jlimit(0.0f, 4.0f, tmp.release + m * 2.0f); break;
            case MTPitch:    { float* bp = (tmp.engine == SrcPhys) ? &tmp.physFreq : &tmp.oscFreq;
                               *bp = juce::jlimit(1.0f, 20000.0f, *bp * std::pow(2.0f, m)); } break;   // +-1 octave
            case MTWavePos:  { float& f = (tmp.engine == SrcGrain) ? tmp.grainPos : tmp.addPos; f = juce::jlimit(0.0f, 1.0f, f + m); } break;
            case MTDetune:   add(tmp.oscDetune, 0, 1); break;
            case MTVibrato:  add(tmp.vibrato, 0, 1); break;
            case MTWidth:    add(tmp.uniSpread, 0, 1); break;
            case MTDrift:    add(tmp.drift, 0, 1); break;
            default:
                if (r.tgt >= MT_GRID_BASE && r.tgt < MT_COUNT)
                {
                    const GridKnob gk = modGridKnob(tmp.engine, r.tgt - MT_GRID_BASE);
                    if (gk.field != nullptr) { float& f = tmp.*(gk.field); f = juce::jlimit(gk.mn, gk.mx, f + m * (gk.mx - gk.mn)); }
                }
                break;
        }
    }
}

void DrumChannel::renderInto(juce::AudioBuffer<float>& dest, int startSample, int numSamples, bool anySolo,
                             juce::AudioBuffer<float>* reverbSendBus, juce::AudioBuffer<float>* delaySendBus)
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

    if (mute || (anySolo && !solo)) { feedSilence(); return; }

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
        int    uniVoices = 1; float uniCents = 0, uniGain = 1, oscVibFac = 1; bool uniCenter = false; int uniMode = 0; int chord = 0;
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
        int    fxDriveType = 0; float fxDrive = 0;  // per-slot Drive (insert); fxRevSend/fxDelSend = per-slot send amounts
        float  fxRevSend = 0, fxDelSend = 0;
        int    waveTable = 0; float wavePos = 0;   // wavetable (SrcWave)
        int    modalN = 0; float modalA1[MODAL_MODES] = {}, modalA2[MODAL_MODES] = {}, modalGain[MODAL_MODES] = {};  // modal bank
        float  modalDecaySec = 0.5f;   // base ring length (for voiceEnd)
        // -- 4-point pitch envelope (applies on top of the legacy per-engine env) --
        bool   pEnvOn = false; float pEnvP[Slot::NPE] = { 0, 0, 0, 0 }, pEnvT[Slot::NPE] = { 0.2f, 0.4f, 0.6f, 0.8f };
        double voiceLenSamp = 1.0;   // sound length in samples, for the time-fraction axis
        // === PER-SLOT EQ (begin) - coeffs (state lives per-voice in SlotVoice) ===
        bool   eqAny = false; bool eqUse[7] = {}; Biquad eqBq[7];
        // === PER-SLOT EQ (end) ===
        // === PER-SLOT FILTER (begin) - resonant LP; raw params here, coeffs recomputed per BLOCK
        //     (env-follow) into 'filt' just before the voice loop; state lives per-voice ===
        float  uniSpread = 0.0f; float uniPanL[UNI_MAX + 1] = {}, uniPanR[UNI_MAX + 1] = {};   // stereo WIDTH per voice (equal power)
        // TWO independent resonant filters per slot, applied IN SERIES (filt[0] then filt[1]) - e.g.
        // High-Pass + Low-Pass = a band you shape by hand. Each: LP/HP/BP/Notch + cutoff/reso/env +
        // keytrack. Raw params here; coeffs recomputed per BLOCK (env-follow + LFO); state per-voice.
        struct FiltCfg {
            bool   on = false; int mode = 0;              // on + which SVF output (0 LP / 1 HP / 2 BP / 3 Notch)
            float  keyTrack = 0.0f;                        // 0..1 = cutoff follows the note pitch
            double cutoff = 1000; float reso = 0.707f, envAmt = 0.0f;
            double cutoffHz = 1000, G = 0.1, K = 1.414;    // block coeffs (env+LFO); per-voice keytrack re-tans cutoffHz
        } filt[2];
        float  chMix = 0.0f;   // per-slot CHORUS insert (0 mix = off; rate/depth = effect constants)
        const float* wtFrm[ADD_FRAMES] = {};  // ADDITIVE WAVETABLE: the slot's 4 baked frame tables (null = not Custom)
        float wtPos = 0.0f;                   // static position 0..1 (addPos)
        bool  wtGlide = false;                // per-note glide on (addSeg[0] > 0) - overrides wtPos
        bool  wtLoop  = false;                // ping-pong the glide forever (out and back)
        float wtLoopEnd = 1.0f;               // journey end time (samples) = travel to the first hold
        // piecewise position clock (samples): leg k runs [wtT(k-1), wtTk) at slope wtInvk; a HOLD
        // leg has boundary 1e18 + slope 0, so the position parks at that leg's left frame.
        float wtT1 = 0.0f, wtT2 = 0.0f, wtT3 = 0.0f;
        float wtInv0 = 0.0f, wtInv1 = 0.0f, wtInv2 = 0.0f;
        float  toneK = 0.0f, toneGL = 1.0f, toneGH = 1.0f;   // TONE tilt (1-pole split + complementary gains)
        float  punch = 0.0f;   // PUNCH transient shaper (-1 soften .. +1 punch)
        float  comp  = 0.0f;   // one-knob COMPRESSOR amount (applied on the slot bus post-loop)
        // === PER-SLOT FILTER (end) ===
        // Per-slot LFOs (3 independent sines, restart each hit). Index: 0 filter cutoff / 1 pitch / 2 volume.
        float  lfoRate[4] = { 4.0f, 4.0f, 4.0f, 4.0f }, lfoAmt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        int    lfoShape[4] = {};            // 0 sine .. 7 custom
        const float* lfoCurve[4] = {};      // shape 7: the slot's drawn cycle
        bool   lfoFreeOn[4] = {};           // FREE-RUN: use the timeline-anchored channel phase
        double lfoFreePh[4] = {}, lfoFreeInc[4] = {};   // block-start phase + per-sample increment
        float  drift = 0.0f;                // slot DRIFT amount (wander depth per block)
        float  filtDrive = 0.0f;            // filter loop saturation 0..1 (0 = clean = bit-identical)
        bool   lfoSrcUsed[4] = {};          // MOD MATRIX: this LFO feeds a route as a SOURCE -> keep its phase advancing even at built-in amount 0
    } sc[NUM_SLOTS];

    bool anySlotActive = false;
    int  domSlot = -1; float domW = -1.0f;
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        SC& c = sc[s];
        // MOD MATRIX (block-rate): if any route is live, sample the sources for this slot and apply
        // them onto a scratch Slot copy, then bake from THAT. All routes off/0 = the copy is skipped
        // and `sl` is the real slot = byte-for-byte the old path (bit-identical). The four audio-rate
        // LFO paths still run unchanged; the matrix modulates the block config, not the sample loop.
        Slot modTmp;
        const Slot* slp = &slots[s];
        if (slots[s].modActive())
        {
            modTmp = slots[s];
            float srcVals[MS_COUNT] = {};
            computeModSources(s, modTmp, srcVals);
            applyModMatrix(modTmp, srcVals);
            slp = &modTmp;
            // Keep any LFO used as a matrix SOURCE advancing even if its own Amount is 0.
            for (auto& r : modTmp.mod)
                if (r.tgt != MTOff && std::abs(r.amt) > 1.0e-4f
                    && r.src >= MSLfoFilt && r.src <= MSLfoWave)
                    c.lfoSrcUsed[r.src - MSLfoFilt] = true;
        }
        const Slot& sl = *slp;
        if (sl.engine < 0 || sl.weight <= 0.0f) continue;
        if (sl.engine == SrcSample && slotSample[s].buf.getNumSamples() == 0) continue;
        c.engine = sl.engine; c.weight = sl.weight;
        c.fxDriveType = sl.fxDriveType; c.fxDrive = sl.fxDrive; c.fxRevSend = sl.fxReverbSend; c.fxDelSend = sl.fxDelaySend;  // per-slot FX
        for (int d2 = 0; d2 < 4; ++d2) {   // per-slot LFOs (free-Hz OR tempo-synced cycles/bar)
            // lfoSync: 0 = OFF (free Hz), > 0 = cycles per bar (edited by dragging the LFO wave while
            // synced), < 0 = LOCK TO GRID (draw = Grid 1/N, else step count). NOTE: lfoSyncRate is a
            // DORMANT persisted field (a rejected base-x-rate design) - never apply it (hidden-param rule).
            float cpb = sl.lfoSync[d2];
            if (cpb < 0.0f) cpb = (float) juce::jmax(1, drawMode ? lfoGridDiv : numSteps);   // grid: one cycle per cell
            c.lfoRate[d2] = (cpb > 0.0f)
                ? juce::jlimit(0.005f, 40.0f, cpb / juce::jmax(0.05f, lfoBarSeconds))          // cycles/bar -> Hz
                : juce::jlimit(0.05f, 30.0f, sl.lfoRate[d2]);
            c.lfoAmt[d2]  = juce::jlimit(0.0f, 1.0f, sl.lfoAmt[d2]);
            c.lfoShape[d2]  = juce::jlimit(0, 7, sl.lfoShape[d2]);
            c.lfoCurve[d2]  = sl.lfoCurve[d2];
            c.lfoFreeOn[d2] = sl.lfoFree[d2];
            if (c.lfoFreeOn[d2])
            {   // FREE-RUN: anchor the phase to the TIMELINE (bars into the playing unit) so every
                // playback pass is identical; when stopped, a free channel clock keeps it moving live.
                const double cyclesNow = (lfoBarPos >= 0.0 ? lfoBarPos * (double) lfoBarSeconds
                                                           : lfoFreeSec) * (double) c.lfoRate[d2];
                c.lfoFreePh[d2]  = 2.0 * kPi * cyclesNow;
                c.lfoFreeInc[d2] = 2.0 * kPi * (double) c.lfoRate[d2] / sr;
            } }
        c.drift     = juce::jlimit(0.0f, 1.0f, sl.drift);
        c.filtDrive = juce::jlimit(0.0f, 1.0f, sl.filterDrive);
        if (s == 0)   // once per block: advance the free-run clock when the transport isn't driving us
        { if (lfoBarPos >= 0.0) lfoFreeSec = 0.0; else lfoFreeSec += (double) numSamples / sr; }
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
        anySlotActive = true;
        if (sl.weight > domW) { domW = sl.weight; domSlot = s; }
        switch (sl.engine)
        {
            case SrcOsc:
                c.oscShape = sl.oscShape; c.oscShapeB = sl.oscShapeB; c.oscFreq = (float) slotBaseHz(s, sl);
                c.oscPEnvAmt = sl.oscPEnvAmt; c.oscPEnvTime = sl.oscPEnvTime; c.oscPOffset = sl.oscPOffset;
                c.scaleOn   = sl.scaleOn;
                c.uniVoices = juce::jlimit(1, UNI_MAX, sl.scaleOn ? (sl.scaleType >= 10 ? 6 : sl.scaleUnison) : sl.chordMode > 0 ? sl.chordUnison : sl.oscUnison);
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;   // up to +/-100 cents (1 semitone) spread
                c.uniCenter = sl.oscUniCenter; c.uniMode = sl.oscDetuneMode;
                c.uniGain   = 1.0f / std::sqrt((float) (c.uniVoices + (c.uniCenter ? 1 : 0)));
                c.chord     = sl.scaleOn ? 0 : juce::jlimit(0, 7, sl.chordMode);   // SCALE forces chord off; render reads sv.uniSemis
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
                c.uniVoices = juce::jlimit(1, KS_UNI, sl.scaleOn ? (sl.scaleType >= 10 ? 6 : sl.scaleUnison) : sl.chordMode > 0 ? sl.chordUnison : sl.oscUnison);
                c.chord     = juce::jlimit(0, 7, sl.chordMode);
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;
                for (int k = 0; k < KS_UNI; ++k)
                {
                    const double sp = c.uniVoices > 1 ? 2.0 * (double) k / (double)(c.uniVoices - 1) - 1.0 : 0.0;
                    c.uniMul[k] = std::pow(2.0, (double) chordSemis(c.chord, k) / 12.0 + sp * (double) c.uniCents / 1200.0);
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
                c.uniVoices = juce::jlimit(1, MODAL_NOTES, sl.scaleOn ? sl.scaleUnison : sl.chordMode > 0 ? sl.chordUnison : sl.oscUnison);   // one FULL bank per note (cap MODAL_NOTES)
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;
                c.chord     = sl.scaleOn ? 0 : juce::jlimit(0, 7, sl.chordMode);
                // Pitch-envelope time base = the audible Strike + Ring (matches the editor's axis).
                c.voiceLenSamp = juce::jmax(1.0, (double)((sl.atk + decaySec) * (float) sr));
                break; }
            default: break;
        }
        // For samples, the pitch-envelope time axis is the (trimmed) SAMPLE length, not the
        // amp envelope (samples have no AHDSR now). Mirror the voiceEnd "natural" length.
        if (sl.engine == SrcSample)
            c.voiceLenSamp = juce::jmax(1.0, (double)(c.regHi - c.regLo) * (double) engineOS / juce::jmax(0.05, c.speed) / (double) c.slices);

        // === PER-SLOT EQ (begin) - coeffs for this slot's HP(2)+bells(3)+LP(2) ===
        {
            const double nyq = sr * 0.49;
            const auto& E = sl.eqBand;
            if (E[EQ_HP].on) { double f = juce::jlimit(20.0, nyq, (double) E[EQ_HP].freq);
                               c.eqBq[0].highpass(sr, f, 0.5412); c.eqBq[1].highpass(sr, f, 1.3066); c.eqUse[0] = c.eqUse[1] = true; }
            for (int k = 0; k < 3; ++k) if (E[EQ_B1 + k].on) {
                c.eqBq[2 + k].peaking(sr, juce::jlimit(20.0, nyq, (double) E[EQ_B1 + k].freq),
                                      juce::jlimit(0.2, 12.0, (double) E[EQ_B1 + k].q), (double) E[EQ_B1 + k].gainDb);
                c.eqUse[2 + k] = true; }
            if (E[EQ_LP].on) { double f = juce::jlimit(20.0, nyq, (double) E[EQ_LP].freq);
                               c.eqBq[5].lowpass(sr, f, 0.5412); c.eqBq[6].lowpass(sr, f, 1.3066); c.eqUse[5] = c.eqUse[6] = true; }
            for (bool u : c.eqUse) c.eqAny |= u;
        }
        // === PER-SLOT EQ (end) ===
        // === PER-SLOT FILTERS (begin) - TWO in series; stash raw params, coeffs baked per BLOCK below.
        const int   fTy[2] = { sl.filterType,   sl.filterType2 };
        const float fCu[2] = { sl.filterCutoff, sl.filterCutoff2 };
        const float fRe[2] = { sl.filterReso,   sl.filterReso2 };
        const float fEn[2] = { sl.filterEnvAmt, sl.filterEnvAmt2 };
        const float fKt[2] = { sl.filterKeyTrack, sl.filterKeyTrack2 };
        for (int fi = 0; fi < 2; ++fi)
        {
            c.filt[fi].on       = (fTy[fi] >= LowPass && fTy[fi] <= Notch);       // LP/HP/BP/Notch (Formant = legacy, off here)
            c.filt[fi].mode     = juce::jlimit(0, 3, fTy[fi] - LowPass);
            c.filt[fi].keyTrack = juce::jlimit(0.0f, 1.0f, fKt[fi]);
            c.filt[fi].cutoff   = juce::jlimit(20.0, sr * 0.49, (double) fCu[fi]);
            c.filt[fi].reso     = juce::jlimit(0.3f, 12.0f, fRe[fi]);
            c.filt[fi].envAmt   = juce::jlimit(-1.0f, 1.0f, fEn[fi]);
        }
        // === PER-SLOT FILTERS (end) ===
        // === PER-SLOT CHORUS - lush multi-voice stereo widener (0 mix = off = bit-identical) ===
        c.chMix   = juce::jlimit(0.0f, 1.0f, sl.chorusMix);
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
        c.comp    = juce::jlimit(0.0f, 1.0f, sl.fxComp);
        { const float t = juce::jlimit(-1.0f, 1.0f, sl.fxTone);          // TONE tilt: +/-6 dB around ~800 Hz
          if (t != 0.0f) { c.toneK  = 1.0f - std::exp(-2.0f * (float) kPi * 800.0f / (float) juce::jmax(1.0, sr));
                           c.toneGL = juce::Decibels::decibelsToGain(-t * 6.0f);
                           c.toneGH = juce::Decibels::decibelsToGain( t * 6.0f); }
          else { c.toneK = 0.0f; c.toneGL = c.toneGH = 1.0f; } }
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
    fxSendBuf.clear();
    float* fxRevL = fxSendBuf.getWritePointer(0); float* fxRevR = fxSendBuf.getWritePointer(1);
    float* fxDelL = fxSendBuf.getWritePointer(2); float* fxDelR = fxSendBuf.getWritePointer(3);

    // CHORUS: when a slot's chorus is on, its (fully panned/gained) output is summed into a per-slot
    // block buffer here instead of into the main mix; after the voice loop the insert runs on that
    // buffer and adds to the output. All-off = the buffer is never touched = bit-identical.
    bool slotChorus[NUM_SLOTS], slotComp[NUM_SLOTS], slotBus[NUM_SLOTS];
    bool anyChorus = false;
    for (int s = 0; s < NUM_SLOTS; ++s) { slotChorus[s] = sc[s].chMix > 0.001f;
                                          slotComp[s]   = sc[s].comp  > 0.001f;
                                          slotBus[s]    = slotChorus[s] || slotComp[s];
                                          anyChorus |= slotBus[s]; }
    float* chorInL[NUM_SLOTS] = {}; float* chorInR[NUM_SLOTS] = {};
    if (anyChorus)
    {
        chorusInBuf.setSize(NUM_SLOTS * 2, numSamples, false, false, true);
        chorusInBuf.clear();
        for (int s = 0; s < NUM_SLOTS; ++s)
        { chorInL[s] = chorusInBuf.getWritePointer(s * 2); chorInR[s] = chorusInBuf.getWritePointer(s * 2 + 1); }
    }

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
    {
        const double nyq = sr * 0.49;
        for (int s = 0; s < NUM_SLOTS; ++s)
        {
            SC& c = sc[s];
            // The per-slot LFO -> FILTER cutoff (dest 0) modulates BOTH filters (sampled per block from
            // the NEWEST voice's phase); env-follow uses the PREVIOUS block's per-slot amp level.
            double lfoMul = 1.0;
            if (c.lfoAmt[0] > 0.001f)
            {
                const Voice* nv = nullptr;
                for (auto& v : voices) if (v.active() && (nv == nullptr || v.voiceSamples < nv->voiceSamples)) nv = &v;
                if (nv != nullptr)
                {
                    const double ph0 = c.lfoFreeOn[0] ? c.lfoFreePh[0] : nv->sv[s].lfoPhase[0];
                    const uint32_t cy0 = c.lfoFreeOn[0] ? (uint32_t) juce::jmax(0.0, ph0 / (2.0 * kPi)) : nv->sv[s].lfoCyc[0];
                    lfoMul = std::pow(2.0, (double) lfoShapeVal(c.lfoShape[0], ph0, cy0, c.lfoCurve[0]) * (double) c.lfoAmt[0] * 3.0);
                }
            }
            for (int fi = 0; fi < 2; ++fi)
            {
                auto& f = c.filt[fi];
                if (! f.on) continue;
                double cutoff = f.cutoff * lfoMul;
                if (f.envAmt != 0.0f)
                    cutoff *= std::pow(2.0, (double) f.envAmt * (double) slotFiltEnv[s] * 5.0);   // +/-5 octaves
                f.cutoffHz = juce::jlimit(20.0, nyq, cutoff);                  // stash (Hz) so per-voice KEYTRACK re-tans from it
                f.G = std::tan(kPi * f.cutoffHz / sr);                         // ZDF prewarped cutoff target
                f.K = 1.0 / juce::jmax(0.15, (double) f.reso);                 // damping (higher reso = lower k)
            }
        }
    }
    // === PER-SLOT FILTER (end) ===

    // ---- Voice loop -----------------------------------------------------------
    for (int vi = 0; vi < POLY; ++vi)
    {
        Voice& v = voices[vi];
        if (! v.active()) continue;

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
        // FILTER KEYTRACK (per voice, per block): shift the cutoff TARGET with the note's pitch offset
        // from the slot base (voicePitch = step/roll pitch, keySemis = keyboard). keytrack 1 = cutoff
        // tracks the note 1:1 (doubles per octave). Rides on top of filtCutoffHz (env-follow + LFO).
        for (int s = 0; s < NUM_SLOTS; ++s)
            for (int fi = 0; fi < 2; ++fi)
            {
                auto& f = sc[s].filt[fi];
                const bool kt  = f.on && f.keyTrack > 0.0f;
                const bool dfm = f.on && std::abs(v.sv[s].driftFiltMul - 1.0f) > 1.0e-4f;   // DRIFT per-note cutoff
                if (kt || dfm)
                {
                    double hz = f.cutoffHz * (double) v.sv[s].driftFiltMul;
                    if (kt)
                    {
                        const double noteSemis = (double) v.voicePitch + (double) v.sv[s].keySemis;
                        hz *= std::pow(2.0, (double) f.keyTrack * noteSemis / 12.0);
                    }
                    v.sv[s].filtGkt[fi] = std::tan(kPi * juce::jlimit(20.0, sr * 0.49, hz) / sr);
                }
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
                const SC& c = sc[s];
                if (c.engine < 0) continue;
                SlotVoice& sv = v.sv[s];
                // HUMANIZE: this slot's whole onset is pushed back by startDelay (0 = on time). Skip it
                // (frozen, silent) until then; afterwards run against a slot-LOCAL age `t` (shadows tv) so
                // its envelope + phases start from zero at the delayed onset. startDelay 0 = bit-identical.
                if (tv < sv.startDelay) continue;
                const long t = tv - sv.startDelay;
                float sig = 0.0f, sL = 0.0f, sR = 0.0f, env = 0.0f;
                bool  stereo = false;

                // 4-point pitch envelope -> frequency multiplier (1.0 when unused). Applies to every engine.
                double pe3Mul = 1.0;
                if (c.pEnvOn) {
                    const float frac = juce::jlimit(0.0f, 1.0f, (float)((double) t / c.voiceLenSamp));
                    pe3Mul *= std::pow(2.0, (double) pitchEnv4(frac, c.pEnvP, c.pEnvT) / 12.0);
                }
                // Per-slot PITCH LFO: sine bend, +/-1 octave at full Amount (sirens, dive wobble,
                // vibrato at small amounts). Phase advances after the engine renders.
                if (c.lfoAmt[1] > 0.001f)
                {
                    const double ph1 = c.lfoFreeOn[1] ? c.lfoFreePh[1] + (double) i * c.lfoFreeInc[1] : sv.lfoPhase[1];
                    const uint32_t cy1 = c.lfoFreeOn[1] ? (uint32_t) juce::jmax(0.0, ph1 / (2.0 * kPi)) : sv.lfoCyc[1];
                    pe3Mul *= std::pow(2.0, (double) lfoShapeVal(c.lfoShape[1], ph1, cy1, c.lfoCurve[1]) * (double) c.lfoAmt[1]);
                }
                // KEYS: skip slots the keyboard can't play; re-tune the rest from their own base
                // Freq to the pressed note (keySemis rides pe3Mul, so it reaches every pitched
                // engine including the Modal strike-tuning and the KS varispeed read).
                if (sv.keyMute) continue;
                if (sv.keySemis != 0.0f)
                    pe3Mul *= std::pow(2.0, (double) sv.keySemis / 12.0);
                const double noteMul = vPitchMul;   // per-step/roll/slide/keys pitch (env/vibrato/LFO ride pe3Mul)

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
                        if (c.fmIndex > 0.0001f) {
                            const float modOut = (float) std::sin(sv.fmMod + c.fmFeedback * 6.0f * sv.fmFbState);
                            sv.fmFbState = 0.5f * (sv.fmFbState + modOut);
                            // ANTI-ALIAS: FM sidebands ignore Nyquist - roll the index off as the
                            // effective carrier climbs (1x below ~1.8 kHz = drums/bass untouched;
                            // C7/C8 leads lose the inharmonic fizz instead of aliasing).
                            const float fmAtt = (float) juce::jlimit(0.35, 1.0, 1800.0 / juce::jmax(1800.0, freq * fmRootMul));
                            // Env-follow: the FM index rides the amp envelope (classic FM drum -
                            // bright modulated attack that mellows to the plain carrier as it decays).
                            fmAdd = (c.fmEnvF ? c.fmIndex * env : c.fmIndex) * fmAtt * modOut;
                            sv.fmMod += 2.0 * kPi * freq * fmRootMul * (double) c.fmRatio / sr;
                            if (sv.fmMod > 2.0 * kPi) sv.fmMod -= 2.0 * kPi;
                        }
                        const bool fmActive = c.fmIndex > 0.0001f;
                        float wsum = 0.0f, wL = 0.0f, wR = 0.0f;
                        const bool sprd = c.uniSpread > 0.001f && c.uniVoices > 1;   // STEREO WIDTH active
                        const int totalV = c.uniVoices + (c.uniCenter ? 1 : 0);   // +1 dry/centre voice
                        const float strumFade = juce::jmax(1.0f, 0.002f * (float) sr);   // 2 ms click-free fade-in per strummed note
                        for (int u = 0; u < totalV; ++u) {
                            const bool centreVoice = (c.uniCenter && u == c.uniVoices);   // the extra undetuned voice
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
                            else if (c.chord > 0 && ! centreVoice)
                                det *= std::pow(2.0, (double) chordSemis(c.chord, u) / 12.0);
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
                                if (c.lfoAmt[3] > 0.001f)
                                {
                                    const double ph3 = c.lfoFreeOn[3] ? c.lfoFreePh[3] + (double) i * c.lfoFreeInc[3] : sv.lfoPhase[3];
                                    const uint32_t cy3 = c.lfoFreeOn[3] ? (uint32_t) juce::jmax(0.0, ph3 / (2.0 * kPi)) : sv.lfoCyc[3];
                                    wtp += lfoShapeVal(c.lfoShape[3], ph3, cy3, c.lfoCurve[3]) * c.lfoAmt[3] * 0.5f;
                                }
                                wtp = juce::jlimit(0.0f, 1.0f, wtp);
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
                            wsum += vout;
                            if (sprd) { const int pu = juce::jmin(u, UNI_MAX);
                                        wL += vout * c.uniPanL[pu]; wR += vout * c.uniPanR[pu]; }
                            sv.uniPhase[u] += 2.0 * kPi * freq * det / sr;
                            if (sv.uniPhase[u] > 2.0 * kPi) sv.uniPhase[u] -= 2.0 * kPi;
                        }
                        float oo = wsum * c.uniGain;
                        float ooL = sprd ? wL * c.uniGain : 0.0f, ooR = sprd ? wR * c.uniGain : 0.0f;
                        auto fold1 = [&](float x) { const float fd = std::sin(x * (1.0f + c.oscWarp * 4.0f) * 1.5707963f);
                                                    return x + c.oscWarp * (fd - x); };
                        if (c.oscWarp > 0.001f) {                       // WARP = one-way wavefold (adds harmonics/grit)
                            oo = fold1(oo);
                            if (sprd) { ooL = fold1(ooL); ooR = fold1(ooR); }
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
                        float fm = morphWave(sv.fmCarrier + c.fmIndex * modOut, cpos);
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
                            // 4-point Hermite (cubic) interpolation - cleaner than linear on pitched/stretched playback.
                            auto at = [&](const float* s, int k){ return s[juce::jlimit(0, sLen - 1, rIdx + dir * k)]; };
                            sL = hermite4(fr, at(srcA, -1), at(srcA, 0), at(srcA, 1), at(srcA, 2));
                            sR = hermite4(fr, at(srcB, -1), at(srcB, 0), at(srcB, 1), at(srcB, 2));
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
                            for (auto& gr : sv.grains)
                                if (gr.age >= gr.len)              // a free grain slot
                                {
                                    const float r1 = whiteNoise(sv.noiseState);   // position spray
                                    const float r2 = whiteNoise(sv.noiseState);   // pitch spray
                                    const double pmul = (double) noteMul * (double) pe3Mul
                                                      * std::pow(2.0, (double) (r2 * c.grPitch));   // +-12 st max
                                    gr.len = juce::jmax(64, c.grLenSamp);
                                    gr.age = 0; gr.amp = 1.0f;
                                    float lfoP = 0.0f;   // WAVE LFO scans grain Position (same meaning
                                    if (c.lfoAmt[3] > 0.001f)                      // as the osc wavetable)
                                    {
                                        const double ph3 = c.lfoFreeOn[3] ? c.lfoFreePh[3] + (double) i * c.lfoFreeInc[3] : sv.lfoPhase[3];
                                        const uint32_t cy3 = c.lfoFreeOn[3] ? (uint32_t) juce::jmax(0.0, ph3 / (2.0 * kPi)) : sv.lfoCyc[3];
                                        lfoP = lfoShapeVal(c.lfoShape[3], ph3, cy3, c.lfoCurve[3]) * c.lfoAmt[3] * 0.5f;
                                    }
                                    if (smp)
                                    {
                                        const double span = (double) (c.grHi - c.grLo);
                                        double p01 = (double) c.grPos + (double) (r1 * c.grSpray) + (double) lfoP;
                                        p01 -= std::floor(p01);
                                        gr.pos = (double) c.grLo + p01 * juce::jmax(1.0, span - 4.0);
                                        gr.inc = pmul / (double) engineOS;         // varispeed at the file's rate
                                    }
                                    else
                                    {
                                        double p01 = (double) c.grPos + (double) (r1 * c.grSpray) + (double) lfoP;
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
                                const double iv = c.scaleOn ? (double) sv.uniSemis[j] : (double) chordSemis(c.chord, j);   // SCALE per-note offset
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

                // Per-slot VOLUME LFO: tremolo on this slot's signal (full Amount dips to silence -
                // helicopter/chopper at fast rates, pumping at slow). Then advance every ACTIVE
                // LFO's phase once per sample (the filter LFO's phase is read per block).
                if (c.lfoAmt[2] > 0.001f) {
                    const double ph2 = c.lfoFreeOn[2] ? c.lfoFreePh[2] + (double) i * c.lfoFreeInc[2] : sv.lfoPhase[2];
                    const uint32_t cy2 = c.lfoFreeOn[2] ? (uint32_t) juce::jmax(0.0, ph2 / (2.0 * kPi)) : sv.lfoCyc[2];
                    const float g = 1.0f - c.lfoAmt[2] * 0.5f * (1.0f + lfoShapeVal(c.lfoShape[2], ph2, cy2, c.lfoCurve[2]));
                    if (stereo) { sL *= g; sR *= g; } else sig *= g;
                }
                for (int d2 = 0; d2 < 4; ++d2)
                    if (c.lfoAmt[d2] > 0.001f || c.lfoSrcUsed[d2]) {   // advance also when it drives a mod-matrix route
                        sv.lfoPhase[d2] += 2.0 * kPi * (double) c.lfoRate[d2] / sr;
                        if (sv.lfoPhase[d2] > 2.0 * kPi) { sv.lfoPhase[d2] -= 2.0 * kPi; ++sv.lfoCyc[d2]; }
                    }

                // === PER-SLOT FILTERS (begin) - TWO resonant TPT/ZDF SVFs IN SERIES on THIS slot's source
                //     (before its EQ), so they never touch the other slot's engine. State is per-voice.
                //     Each SVF's cutoff coeff is smoothed PER SAMPLE (~2 ms) so env/LFO sweeps glide; KEYTRACK
                //     (per voice) shifts the target cutoff with the note pitch (sv.filtGkt from voice setup). ===
                for (int fi = 0; fi < 2; ++fi)
                {
                    const auto& fc = c.filt[fi];
                    if (! fc.on) continue;
                    double& gm = sv.filtGm[fi];
                    const double tgt = (sv.filtGkt[fi] > 0.0) ? sv.filtGkt[fi] : fc.G;   // keytrack and/or drift per-note cutoff
                    if (gm < 0.0) gm = tgt;
                    gm += (tgt - gm) * 0.0025;                           // ~2 ms at 2x-OS engine rates
                    const double a1 = 1.0 / (1.0 + gm * (gm + fc.K)), a2 = gm * a1, a3 = gm * a2;
                    const int fm = fc.mode; const double fk = fc.K;
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
                            default: return v2;                         // LOWPASS
                        }
                    };
                    if (stereo) { sL = (float) svf(sL, 0); sR = (float) svf(sR, 1); }
                    else          sig = (float) svf(sig, 0);
                }
                // Track this slot's max amp level this block -> next block's env-follow cutoff.
                blockSlotEnv[s] = juce::jmax(blockSlotEnv[s], env * v.velGain);
                // === PER-SLOT FILTER (end) ===

                // === PER-SLOT EQ (begin) - filter THIS slot's signal before it's mixed ===
                if (c.eqAny) {
                    if (stereo) {
                        for (int k = 0; k < 7; ++k) if (c.eqUse[k]) {
                            sL = eqProcess(c.eqBq[k], sv.eqZ1[k], sv.eqZ2[k], sL, 0);
                            sR = eqProcess(c.eqBq[k], sv.eqZ1[k], sv.eqZ2[k], sR, 1); }
                    } else {
                        for (int k = 0; k < 7; ++k) if (c.eqUse[k])
                            sig = eqProcess(c.eqBq[k], sv.eqZ1[k], sv.eqZ2[k], sig, 0);
                    }
                }
                // === PER-SLOT EQ (end) ===

                // Per-slot DRIVE (insert): shape THIS slot's signal with its own drive type/amount.
                if (c.fxDrive > 0.0001f && c.fxDriveType == DriveBassAmp) {
                    // BASS AMP (the survivor of the amp family - Guitar/Lead were killed by the
                    // user): the SPLIT RIG. Lows below ~180 Hz pass CLEAN and rejoin at the
                    // output; only the mids/highs are driven (2 soft stages + a 3.8 kHz 2-pole
                    // cab + DC block) = fat, never farty. Fixed voicing; the fader = the gain.
                    const float aA = c.fxDrive, a2 = aA * aA;
                    const float g1 = 1.0f + a2 * 22.0f;
                    const float mk = 1.0f / (1.0f + (g1 - 1.0f) / 7.0f);
                    auto amp = [&](float y, int lr) -> float {
                        sv.ampPre[lr] += drvAmpBassK * (y - sv.ampPre[lr]);
                        const float lo = sv.ampPre[lr];                           // the low split
                        float v = (y - lo) * (1.0f + 0.6f * aA);
                        v = std::tanh(v * g1 + 0.12f) - std::tanh(0.12f);         // stage 1 (asym = amp evens)
                        v = std::tanh(v * 1.5f) * 1.15f;                          // stage 2 (glue)
                        sv.ampLp1[lr] += drvAmpCab2K * (v - sv.ampLp1[lr]);       // 2-pole cabinet
                        sv.ampLp2[lr] += drvAmpCab2K * (sv.ampLp1[lr] - sv.ampLp2[lr]);
                        v = sv.ampLp2[lr] * mk + lo;                              // clean lows rejoin
                        const float dc = v - sv.drvDcX[lr] + 0.995f * sv.drvDcY[lr];   // rumble/DC block
                        sv.drvDcX[lr] = v; sv.drvDcY[lr] = dc;
                        return dc;
                    };
                    if (stereo) { sL = amp(sL, 0); sR = amp(sR, 1); }
                    else          sig = amp(sig, 0);
                }
                else if (c.fxDrive > 0.0001f && c.fxDriveType != DriveOff) {
                    // retired slot 7 (the killed Guitar/Lead amps): stray saves play as Tube
                    const int dTy = c.fxDriveType == DriveAmpRetired ? (int) Tube : c.fxDriveType;
                    if (stereo) { sL = driveSample(sL, dTy, c.fxDrive); sR = driveSample(sR, dTy, c.fxDrive); }
                    else          sig = driveSample(sig, dTy, c.fxDrive);
                    // Musicality pass (v6): HARSH shapers get a gentle ~8 kHz 1-pole after the clip
                    // (naked waveshaping = fizzy top - every synth drive has a post-filter), and FUZZ
                    // gets a DC blocker (its rectified blend adds a constant offset = headroom loss).
                    const bool harsh = c.fxDriveType == HardClip || c.fxDriveType == Foldback || c.fxDriveType == Fuzz;
                    if (harsh) {
                        const float k = drvLpK;   // ~8 kHz at the engine rate (baked per block)
                        auto post = [&](float y, int lr) -> float {
                            sv.drvLp[lr] += k * (y - sv.drvLp[lr]); y = sv.drvLp[lr];
                            if (c.fxDriveType == Fuzz) {           // DC blocker: y = x - x1 + R*y1
                                const float o = y - sv.drvDcX[lr] + 0.9995f * sv.drvDcY[lr];
                                sv.drvDcX[lr] = y; sv.drvDcY[lr] = o; y = o;
                            }
                            return y;
                        };
                        if (stereo) { sL = post(sL, 0); sR = post(sR, 1); }
                        else          sig = post(sig, 0);
                    }
                }

                // TONE (tilt EQ, per slot): 1-pole split at ~800 Hz + complementary +/-6 dB gains.
                if (c.toneK > 0.0f) {
                    auto tone = [&](float x, int lr) -> float {
                        sv.toneZ[lr] += c.toneK * (x - sv.toneZ[lr]);
                        return sv.toneZ[lr] * c.toneGL + (x - sv.toneZ[lr]) * c.toneGH;
                    };
                    if (stereo) { sL = tone(sL, 0); sR = tone(sR, 1); } else sig = tone(sig, 0);
                }
                // PUNCH (transient shaper, per slot/per hit): fast-vs-slow envelope difference finds the
                // attack; positive boosts it (snap), negative softens it (felt/pillowy). Bounded gains.
                if (c.punch != 0.0f) {
                    const float m = stereo ? 0.5f * (std::abs(sL) + std::abs(sR)) : std::abs(sig);
                    sv.pFast[0] += punchKf * (m - sv.pFast[0]);
                    sv.pSlow[0] += punchKs * (m - sv.pSlow[0]);
                    const float tr = juce::jlimit(0.0f, 2.0f, (sv.pFast[0] - sv.pSlow[0]) / (sv.pSlow[0] + 1.0e-4f));
                    const float pg = c.punch > 0.0f ? 1.0f + c.punch * tr * 1.2f
                                                    : 1.0f / (1.0f + (-c.punch) * tr * 1.2f);
                    if (stereo) { sL *= pg; sR *= pg; } else sig *= pg;
                }
                vEnv = juce::jmax(vEnv, env);
                // HUMANIZE velocity jitter: sv.velScale (~1) loosens this slot's level per hit (1 = identical).
                const float wEff = c.weight * sv.velScale * sv.driftGain;   // driftGain = per-note breath (1 = off)
                const float cL = (stereo ? sL : sig) * wEff;
                const float cR = (stereo ? sR : sig) * wEff;
                // BUS slots (chorus and/or comp on) are pulled OUT of the main mix (fully gained here)
                // into their own block buffer; the inserts run post-loop. Others take the untouched path.
                if (slotBus[s]) { chorInL[s][i] += cL * v.velGain * vPanL * kg; chorInR[s][i] += cR * v.velGain * vPanR * kg; }
                else            { mixL += cL; mixR += cR; }
                // Per-slot reverb/delay SEND: this slot's signal x its own send amount (pre channel-gain; gain added later).
                if (c.fxRevSend > 0.0001f) { fxRevL[i] += cL * v.velGain * vPanL * c.fxRevSend * kg; fxRevR[i] += cR * v.velGain * vPanR * c.fxRevSend * kg; }
                if (c.fxDelSend > 0.0001f) { fxDelL[i] += cL * v.velGain * vPanL * c.fxDelSend * kg; fxDelR[i] += cR * v.velGain * vPanR * c.fxDelSend * kg; }
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

    // === PER-SLOT CHORUS (post-mix insert) - a lush 3-voice modulated-delay ensemble in true stereo.
    //     Each chorus slot's summed DRY output was pulled out of the main mix above; here it runs
    //     through the delay line and the wet+dry result is added to the channel output (pre channel-EQ).
    //     Off (chMix 0) = the slot never left the mix = bit-identical. ===
    if (anyChorus)
    {
        const int dlen = juce::jmax(64, (int) (0.06 * sr));      // ~60 ms line (max tap ~17 ms + margin)
        for (int s = 0; s < NUM_SLOTS; ++s)
        {
            if (! slotBus[s]) continue;
            // One-knob COMPRESSOR (per slot, on the slot's summed bus - real bus compression, not
            // per-voice): peak follower -> soft squash above a fixed threshold + automatic makeup.
            if (slotComp[s])
            {
                const float a   = sc[s].comp;
                const float att = 1.0f - std::exp(-1.0f / (0.004f * (float) sr));
                const float rel = 1.0f - std::exp(-1.0f / (0.120f * (float) sr));
                // v2 (user: "barely doing anything"): the knob now ALSO lowers the threshold
                // (0.30 -> 0.10) while the ratio steepens - high settings genuinely squash.
                // Factory Tone/Punch/Comp showcases get audibly tighter (disclosed).
                const float thr = 0.30f - 0.20f * a;
                const float mk  = 1.0f + a * 1.4f;               // makeup so squashing doesn't just get quieter
                float* iL = chorInL[s]; float* iR = chorInR[s];
                float env = compEnv[s];
                for (int i = 0; i < numSamples; ++i)
                {
                    const float lvl = juce::jmax(std::abs(iL[i]), std::abs(iR[i]));
                    env += (lvl > env ? att : rel) * (lvl - env);
                    float gr = 1.0f;
                    if (env > thr) gr = std::pow(env / thr, -0.9f * a);   // ratio rises with the knob
                    const float g2 = gr * mk;
                    iL[i] *= g2; iR[i] *= g2;
                }
                compEnv[s] = env;
            }
            if (! slotChorus[s])                                  // comp only: the bus goes straight to the mix
            {
                float* iL = chorInL[s]; float* iR = chorInR[s];
                for (int i = 0; i < numSamples; ++i) { outL[i] += iL[i]; outR[i] += iR[i]; }
                continue;
            }
            std::vector<float>& dL = chorusDL[s]; std::vector<float>& dR = chorusDR[s];
            if ((int) dL.size() != dlen) { dL.assign((size_t) dlen, 0.0f); dR.assign((size_t) dlen, 0.0f); chorusW[s] = 0; chorusPh[s] = 0.0; }
            int    w  = chorusW[s];
            double ph = chorusPh[s];
            const double dPh    = 2.0 * kPi * 0.36 / sr;                          // EFFECT CONSTANT rate ~0.36 Hz (the sweet spot)
            const float  baseS  = (float) (0.011 * sr);                           // 11 ms centre delay
            const float  depthS = (float) (0.0035 * sr);                          // EFFECT CONSTANT depth (+/- 3.5 ms swing)
            const float  mix    = sc[s].chMix;
            float* iL = chorInL[s]; float* iR = chorInR[s];
            auto rd = [&](const std::vector<float>& buf, float delay) -> float {
                float rp = (float) w - delay; while (rp < 0.0f) rp += (float) dlen;
                const int i0 = (int) rp; const float fr = rp - (float) i0;
                const int i1 = (i0 + 1 < dlen) ? i0 + 1 : 0;
                return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr;
            };
            for (int i = 0; i < numSamples; ++i)
            {
                dL[(size_t) w] = iL[i]; dR[(size_t) w] = iR[i];
                const float l0 = (float) std::sin(ph);
                const float l1 = (float) std::sin(ph + 2.0943951);   // +120 deg
                const float l2 = (float) std::sin(ph + 4.1887902);   // +240 deg
                const float wetL = 0.5f * (rd(dL, baseS + depthS * l0) + rd(dL, baseS + depthS * l2));
                const float wetR = 0.5f * (rd(dR, baseS + depthS * l1) + rd(dR, baseS - depthS * l0));
                // Unity-safe blend: dry*(1-0.5mix) + wet*(0.5mix). Worst case (taps fully aligned with
                // the dry) sums to EXACTLY the dry level - never a boost (the old wet*mix peaked ~1.5x =
                // the clipping the user heard). The stereo width still comes from L/R using different LFO phases.
                outL[i] += iL[i] * (1.0f - 0.5f * mix) + wetL * (0.5f * mix);
                outR[i] += iR[i] * (1.0f - 0.5f * mix) + wetR * (0.5f * mix);
                if (++w >= dlen) w = 0;
                ph += dPh; if (ph > 2.0 * kPi) ph -= 2.0 * kPi;
            }
            chorusW[s] = w; chorusPh[s] = ph;
        }
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
        float* sb[4] = { fxSendBuf.getWritePointer(0), fxSendBuf.getWritePointer(1),
                         fxSendBuf.getWritePointer(2), fxSendBuf.getWritePointer(3) };
        for (int i = 0; i < numSamples; ++i)
        {
            const float target = 1.0f - duckAmt * duckEnv;
            duckGainZ += (target - duckGainZ) * slew;
            duckEnv *= rel;
            dl[i] *= duckGainZ; dr[i] *= duckGainZ;
            sb[0][i] *= duckGainZ; sb[1][i] *= duckGainZ; sb[2][i] *= duckGainZ; sb[3][i] *= duckGainZ;
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

    // PER-SLOT FX sends: each slot already added (its signal x its own send amount) into fxSendBuf
    // (reverb = ch 0/1, delay = ch 2/3). Apply the channel gain + route into the shared reverb/delay buses.
    if (reverbSendBus != nullptr)
        for (int ch = 0; ch < 2; ++ch) {
            reverbSendBus->addFrom(ch, startSample, fxSendBuf, ch, 0, numSamples, (ch == 0 ? gainL : gainR));
            if (reverbSend > 0.0001f)   // legacy channel-wide send (factory sounds)
                reverbSendBus->addFrom(ch, startSample, renderBuf, ch, 0, numSamples, (ch == 0 ? gainL : gainR) * reverbSend);
        }
    if (delaySendBus != nullptr)
        for (int ch = 0; ch < 2; ++ch) {
            delaySendBus->addFrom(ch, startSample, fxSendBuf, ch + 2, 0, numSamples, (ch == 0 ? gainL : gainR));
            if (delaySend > 0.0001f)
                delaySendBus->addFrom(ch, startSample, renderBuf, ch, 0, numSamples, (ch == 0 ? gainL : gainR) * delaySend);
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
    const bool anyEq           = eqBand[EQ_HP].on || eqBand[EQ_B1].on || eqBand[EQ_B2].on
                              || eqBand[EQ_B3].on || eqBand[EQ_LP].on;

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

    // -- Stage 3: channel EQ: HP (24 dB/oct) -> 3 bells -> LP (24 dB/oct) --
    if (anyEq)
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = buf.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                float x = d[i];
                if (eqBand[EQ_HP].on) { x = eqHP[0].process(x, ch); x = eqHP[1].process(x, ch); }
                for (int b = 0; b < 3; ++b)
                    if (eqBand[EQ_B1 + b].on) x = eqBell[b].process(x, ch);
                if (eqBand[EQ_LP].on) { x = eqLP[0].process(x, ch); x = eqLP[1].process(x, ch); }
                d[i] = x;
            }
        }
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

    const double nyq = sr * 0.49;
    // HP / LP: two cascaded biquads with the Butterworth 4th-order Q pair => 24 dB/oct.
    if (eqBand[EQ_HP].on) {
        const double f = juce::jlimit(20.0, nyq, (double) eqBand[EQ_HP].freq);
        eqHP[0].highpass(sr, f, 0.54119610); eqHP[1].highpass(sr, f, 1.30656296);
    }
    if (eqBand[EQ_LP].on) {
        const double f = juce::jlimit(20.0, nyq, (double) eqBand[EQ_LP].freq);
        eqLP[0].lowpass(sr, f, 0.54119610); eqLP[1].lowpass(sr, f, 1.30656296);
    }
    for (int b = 0; b < 3; ++b) {
        const auto& bd = eqBand[EQ_B1 + b];
        eqBell[b].peaking(sr, juce::jlimit(20.0, nyq, (double) bd.freq),
                          juce::jlimit(0.2, 12.0, (double) bd.q), (double) bd.gainDb);
    }
}
