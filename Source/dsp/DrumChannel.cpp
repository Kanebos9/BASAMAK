#include "DrumChannel.h"
#include <vector>
#if BASAMAK_HAVE_SOUNDTOUCH
 #include <SoundTouch.h>
#endif

const int DrumChannel::VALID_STEP_COUNTS[] =
    { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 21, 24, 32 };

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
    { "Tubular Bell", 6,
      { 1.0f, 2.76f, 5.40f, 8.93f, 13.34f, 18.64f },
      { 1.0f, 0.7f, 0.5f, 0.38f, 0.25f, 0.16f },
      { 1.0f, 0.92f, 0.82f, 0.7f, 0.55f, 0.42f } },
    { "Glass", 6,
      { 1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.5f },
      { 1.0f, 0.6f, 0.42f, 0.3f, 0.2f, 0.13f },
      { 1.0f, 0.7f, 0.5f, 0.36f, 0.26f, 0.18f } },
    { "Membrane", 8,    // circular drumhead (Bessel) modes
      { 1.0f, 1.593f, 2.135f, 2.295f, 2.653f, 2.917f, 3.155f, 3.5f },
      { 1.0f, 0.7f, 0.55f, 0.5f, 0.4f, 0.34f, 0.28f, 0.22f },
      { 1.0f, 0.7f, 0.55f, 0.5f, 0.42f, 0.36f, 0.3f, 0.25f } },
    { "Metal Plate", 10,
      { 1.0f, 1.41f, 1.73f, 2.0f, 2.24f, 2.65f, 3.0f, 3.46f, 4.12f, 4.9f },
      { 1.0f, 0.8f, 0.7f, 0.62f, 0.55f, 0.46f, 0.4f, 0.33f, 0.26f, 0.2f },
      { 1.0f, 0.95f, 0.9f, 0.86f, 0.8f, 0.74f, 0.68f, 0.6f, 0.52f, 0.45f } },
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

static float whiteNoise(juce::Random& rng) { return rng.nextFloat() * 2.0f - 1.0f; }

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

// One basic waveform (shape: 0 Sine, 1 Tri, 2 Square, 3 Saw). Shared by the Analog
// and FM carriers (and the FM/Analog Wave A->B morph).
// Oscillator basic shapes. 0 Sine, 1 Tri, 2 Square, 3 Saw, 4 Ramp-down, 5 Pulse, 6 Hump (half-sine).
// Extra (harmonic-rich) oscillator shapes 7..16 - built once as band-limited single cycles (additive).
namespace {
constexpr int OSCT_EXTRA = 10;
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
        add(40, [&](int h){ return g(h,3,1.2f) + 0.6f*g(h,7,2.0f); });                         // 7  Vowel A
        add(40, [&](int h){ return g(h,4,1.2f) + 0.5f*g(h,12,3.0f); });                        // 8  Vowel E
        add(40, [&](int h){ return g(h,2,1.0f) + 0.6f*g(h,5,1.5f); });                         // 9  Vowel O
        add(40, [&](int h){ return g(h,6,1.4f); });                                            // 10 Formant
        add(40, [&](int h){ if ((h&(h-1))!=0) return 0.0f; float o=std::log2((float)h); return 1.0f/(o+1.0f); }); // 11 Organ
        add(40, [&](int h){ static const int p[]={1,2,3,5,7,11}; for(int q:p) if(q==h) return 1.0f/(float)h; return 0.0f; }); // 12 Bell
        add(40, [&](int h){ return (h%2==1)? 1.0f/std::sqrt((float)h) : 0.0f; });              // 13 Glass (bright odd)
        add(24, [&](int h){ return (h%2==1)? 1.0f/(float)h : 0.0f; });                         // 14 Reed (clarinet)
        add(40, [&](int h){ return (1.0f/(float)h) * (1.0f + 1.5f*g(h,5,2.0f)); });            // 15 Brass (saw+formant)
        add(40, [&](int h){ return g(h,3,1.2f)+0.6f*g(h,6,1.5f)+0.4f*g(h,11,2.5f); });         // 16 Voice
    }
};
static const OscShapeBank& oscBank() { static OscShapeBank b; return b; }
} // namespace

static constexpr int NUM_OSC_SHAPES = 7 + OSCT_EXTRA;   // 0..6 analytic + 7..16 bank = 17
static inline float waveShape(double phase, int shape) noexcept
{
    const double tp = 2.0 * (double) kPi;
    const float ph = (float)(phase / tp - std::floor(phase / tp));
    if (shape >= 7) {                                    // harmonic-rich bank shape (Vowel/Formant/...)
        const float* w = oscBank().shape(juce::jlimit(0, OSCT_EXTRA - 1, shape - 7));
        const float sp = ph * OSCT_LEN; const int i0 = ((int) sp) & (OSCT_LEN - 1), i1 = (i0 + 1) & (OSCT_LEN - 1);
        return w[i0] + (w[i1] - w[i0]) * (sp - (float) (int) sp);
    }
    switch (shape) {
        case 1:  return 1.0f - 4.0f * std::abs(ph - 0.5f);   // triangle
        case 2:  return (ph < 0.5f) ? 1.0f : -1.0f;          // square
        case 3:  return 2.0f * ph - 1.0f;                    // saw (ramp up)
        case 4:  return 1.0f - 2.0f * ph;                    // ramp down (reverse saw)
        case 5:  return (ph < 0.25f) ? 1.0f : -1.0f;         // pulse (25% duty)
        case 6:  return 2.0f * std::sin((float) kPi * ph) - 1.0f;  // hump (rectified-sine bump)
        default: return (float) std::sin(phase);             // sine
    }
}
// Public accessors (for the UI's Wave fader + visual).
int         DrumChannel::oscShapeCount()          { return NUM_OSC_SHAPES; }
const char* DrumChannel::oscShapeName(int s) {
    static const char* n[NUM_OSC_SHAPES] = { "Sine","Tri","Square","Saw","Ramp","Pulse","Hump",
        "Vowel A","Vowel E","Vowel O","Formant","Organ","Bell","Glass","Reed","Brass","Voice" };
    return n[juce::jlimit(0, NUM_OSC_SHAPES - 1, s)];
}
float       DrumChannel::oscShapeSample(int shape, float ph01) {
    return waveShape((double)(ph01 - std::floor(ph01)) * 2.0 * (double) kPi, shape);
}
// Skew/warp the phase (PWM on square, skew toward saw on tri, bend on sine). warp 0.5 = neutral.
static inline double skewPhase(double phase, float warp) noexcept
{
    if (warp > 0.49f && warp < 0.51f) return phase;   // neutral -> untouched (and keeps BLEP exact)
    const double tp = 2.0 * (double) kPi;
    double ph = phase / tp - std::floor(phase / tp);  // 0..1
    const double k = (double) juce::jlimit(0.03f, 0.97f, warp);
    ph = (ph < k) ? (ph * 0.5 / k) : (0.5 + (ph - k) * 0.5 / (1.0 - k));
    return ph * tp;
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
    if (pos > 3.0f) return y;   // new shapes (ramp-down/pulse/hump): rely on the 2x oversampling
    pos = juce::jlimit(0.0f, 3.0f, pos);
    const float wSaw = (pos > 2.0f) ? (pos - 2.0f) : 0.0f;
    const float wSq  = (pos <= 2.0f) ? juce::jmax(0.0f, pos - 1.0f) : (3.0f - pos);
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
static const PhysModel kPhysModels[] = {
    //   apC   stg  bri   dec   excite drop(semis)
    {  0.00f, 0, 0.60f, 1.20f, 0.25f,  0.0f }, // 0 Nylon - warm harmonic string, medium ring, soft pluck
    {  0.00f, 0, 1.00f, 1.90f, 0.95f,  0.0f }, // 1 Steel - very bright harmonic, long sustain, sharp pluck
    {  0.00f, 0, 0.50f, 0.18f, 0.60f,  0.0f }, // 2 Wood  - bright-ish, very short dry thunk (woodblock)
    {  0.60f, 4, 1.00f, 2.50f, 0.90f,  0.0f }, // 3 Glass - inharmonic shimmering bell, very long
    {  0.78f, 4, 0.85f, 1.10f, 0.85f,  0.0f }, // 4 Metal - extreme clang, very inharmonic, medium ring
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

//-- Parameterized synthesis helpers -----------------------------------------
juce::AudioBuffer<float> DrumSoundGenerator::genKick(double sr, float f0, float f1,
                                                     float pitchDecay, float ampDecay,
                                                     float click, float len)
{
    const int n = (int)(sr * len);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float freq = f1 + (f0 - f1) * std::exp(-pitchDecay * t);
        float env = std::exp(-ampDecay * t);
        float cl = (t < 0.003f) ? (1.0f - t / 0.003f) * click : 0.0f;
        d[i] = (std::sin(2.0f * kPi * freq * t) * env + cl) * 0.9f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genSnare(double sr, float tone, float toneAmt,
                                                      float noiseDecay, float len)
{
    const int n = (int)(sr * len);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    juce::Random rng;
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float toneEnv = std::exp(-22.0f * t);
        float noiseEnv = std::exp(-noiseDecay * t);
        float toneSig = std::sin(2.0f * kPi * tone * t) * toneEnv * toneAmt;
        float noise = whiteNoise(rng) * noiseEnv * (1.0f - toneAmt);
        d[i] = (toneSig + noise) * 0.85f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genHat(double sr, float decay, float len, float bright)
{
    const int n = (int)(sr * len);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    const float base[] = { 287.0f, 314.0f, 365.0f, 404.0f, 451.0f, 501.0f };
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float env = std::exp(-decay * t);
        float sig = 0.0f;
        for (float f : base)
            sig += (std::fmod(f * bright * t, 1.0f) < 0.5f ? 1.0f : -1.0f) / 6.0f;
        d[i] = sig * env * 0.6f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genClap(double sr, float len, float decay)
{
    const int n = (int)(sr * len);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    juce::Random rng;
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float b1 = (t < 0.006f) ? std::exp(-200.0f * t) : 0.0f;
        float b2 = (t >= 0.006f && t < 0.012f) ? std::exp(-200.0f * (t - 0.006f)) : 0.0f;
        float b3 = (t >= 0.012f && t < 0.018f) ? std::exp(-200.0f * (t - 0.012f)) : 0.0f;
        float tail = (t >= 0.018f) ? std::exp(-decay * (t - 0.018f)) : 0.0f;
        float env = b1 + b2 * 0.8f + b3 * 0.6f + tail * 0.5f;
        d[i] = whiteNoise(rng) * env * 0.8f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genTom(double sr, float f0, float decay, float len)
{
    const int n = (int)(sr * len);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float freq = f0 * 1.5f * std::exp(-12.0f * t) + f0;
        float env = std::exp(-decay * t);
        d[i] = std::sin(2.0f * kPi * freq * t) * env * 0.85f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genCymbal(double sr, float len, float decay, float bright)
{
    const int n = (int)(sr * len);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    const float base[] = { 287.0f, 314.0f, 365.0f, 404.0f, 451.0f, 501.0f, 550.0f, 630.0f };
    juce::Random rng;
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float env = std::exp(-decay * t);
        float sig = 0.0f;
        for (float f : base)
            sig += (std::fmod(f * bright * t, 1.0f) < 0.5f ? 1.0f : -1.0f) / 8.0f;
        sig += whiteNoise(rng) * 0.25f;
        d[i] = sig * env * 0.55f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genCowbell(double sr)
{
    const int n = (int)(sr * 0.4);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float env = std::exp(-8.0f * t);
        float a = (std::fmod(540.0f * t, 1.0f) < 0.5f ? 1.0f : -1.0f);
        float b = (std::fmod(800.0f * t, 1.0f) < 0.5f ? 1.0f : -1.0f);
        d[i] = (a + b) * 0.5f * env * 0.7f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genClave(double sr)
{
    const int n = (int)(sr * 0.08);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float env = std::exp(-90.0f * t);
        d[i] = std::sin(2.0f * kPi * 1200.0f * t) * env * 0.85f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::genRim(double sr)
{
    const int n = (int)(sr * 0.06);
    juce::AudioBuffer<float> buf(1, n);
    auto* d = buf.getWritePointer(0);
    for (int i = 0; i < n; ++i)
    {
        float t = (float)i / (float)sr;
        float toneEnv = std::exp(-80.0f * t);
        float tone = std::sin(2.0f * kPi * 1700.0f * t) * toneEnv;
        float click = (i < 5) ? 1.0f : 0.0f;
        d[i] = (tone * 0.6f + click * 0.4f) * 0.8f;
    }
    return buf;
}

juce::AudioBuffer<float> DrumSoundGenerator::generate(Type type, double sr)
{
    switch (type)
    {
        case Type::Kick808:      return genKick(sr, 150.0f, 45.0f, 20.0f, 6.0f, 0.4f, 0.6f);
        case Type::Kick909:      return genKick(sr, 180.0f, 50.0f, 35.0f, 9.0f, 0.6f, 0.45f);
        case Type::KickAcoustic: return genKick(sr, 120.0f, 60.0f, 14.0f, 7.0f, 0.5f, 0.5f);
        case Type::KickSub:      return genKick(sr, 90.0f,  35.0f, 8.0f,  3.5f, 0.1f, 0.9f);

        case Type::Snare808:     return genSnare(sr, 200.0f, 0.45f, 14.0f, 0.28f);
        case Type::Snare909:     return genSnare(sr, 180.0f, 0.30f, 18.0f, 0.22f);
        case Type::SnareAcoustic:return genSnare(sr, 220.0f, 0.35f, 10.0f, 0.35f);

        case Type::HatClosed808: return genHat(sr, 60.0f, 0.08f, 1.0f);
        case Type::HatClosed909: return genHat(sr, 75.0f, 0.06f, 1.25f);
        case Type::HatOpen808:   return genHat(sr, 8.0f,  0.4f,  1.0f);
        case Type::HatOpen909:   return genHat(sr, 7.0f,  0.5f,  1.25f);

        case Type::ClapClassic:  return genClap(sr, 0.25f, 25.0f);
        case Type::Clap909:      return genClap(sr, 0.3f,  18.0f);

        case Type::TomLow:       return genTom(sr, 80.0f,  9.0f,  0.4f);
        case Type::TomMid:       return genTom(sr, 120.0f, 10.0f, 0.35f);
        case Type::TomHigh:      return genTom(sr, 180.0f, 12.0f, 0.3f);

        case Type::Crash:        return genCymbal(sr, 1.4f, 3.5f, 1.0f);
        case Type::Ride:         return genCymbal(sr, 1.0f, 5.0f, 1.6f);

        case Type::Cowbell:      return genCowbell(sr);
        case Type::Clave:        return genClave(sr);
        case Type::Rim:          return genRim(sr);
        default:                 return genKick(sr, 150.0f, 45.0f, 20.0f, 6.0f, 0.4f, 0.6f);
    }
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
                sl.oscShape = layerOscShape; sl.oscFreq = layerSineFreq;
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
                case SrcOsc:  case SrcSynth: pa = sl.oscPEnvAmt;  pt = sl.oscPEnvTime;  po = sl.oscPOffset;  legacyAmt = &sl.oscPEnvAmt;  break;
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
        st.setProperty("atk", s.atk, nullptr); st.setProperty("hld", s.hold, nullptr);
        st.setProperty("dec", s.dec, nullptr); st.setProperty("sus", s.sustain, nullptr);
        st.setProperty("rel", s.release, nullptr); st.setProperty("vib", s.vibrato, nullptr);
        st.setProperty("oSh", s.oscShape, nullptr); st.setProperty("oSB", s.oscShapeB, nullptr); st.setProperty("oFr", s.oscFreq, nullptr);
        st.setProperty("oEA", s.oscPEnvAmt, nullptr); st.setProperty("oET", s.oscPEnvTime, nullptr); st.setProperty("oOf", s.oscPOffset, nullptr);
        st.setProperty("oUn", s.oscUnison, nullptr); st.setProperty("oDt", s.oscDetune, nullptr);
        st.setProperty("oUC", s.oscUniCenter, nullptr); st.setProperty("oDM", s.oscDetuneMode, nullptr);
        st.setProperty("fxDt", s.fxDriveType, nullptr); st.setProperty("fxDr", s.fxDrive, nullptr);
        st.setProperty("fxRv", s.fxReverbSend, nullptr); st.setProperty("fxDl", s.fxDelaySend, nullptr);
        st.setProperty("nTy", s.noiseType, nullptr); st.setProperty("nCt", s.noiseCenter, nullptr); st.setProperty("nWd", s.noiseWidth, nullptr);
        st.setProperty("nRs", s.noiseRes, nullptr); st.setProperty("nDr", s.noiseDrive, nullptr); st.setProperty("nCk", s.noiseCrackle, nullptr);
        st.setProperty("fPi", s.fmPitch, nullptr); st.setProperty("fSp", s.fmSpread, nullptr); st.setProperty("fDe", s.fmDepth, nullptr);
        st.setProperty("fEA", s.fmPEnvAmt, nullptr); st.setProperty("fET", s.fmPEnvTime, nullptr); st.setProperty("fOf", s.fmPOffset, nullptr);
        st.setProperty("fFb", s.fmFeedback, nullptr); st.setProperty("fSu", s.fmSub, nullptr);
        st.setProperty("pFr", s.physFreq, nullptr); st.setProperty("pTo", s.physTone, nullptr); st.setProperty("pMa", s.physMaterial, nullptr); st.setProperty("pPo", s.physPosition, nullptr);
        st.setProperty("pSt", s.physStiff, nullptr); st.setProperty("pEx", s.physExcite, nullptr);
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
        st.setProperty("sFile", slotSample[b].file.getFullPathName(), nullptr);   // per-slot sample (reloaded)
        st.setProperty("yFo", s.oscFold, nullptr); st.setProperty("yOL", s.oscLevel, nullptr);
        st.setProperty("yNL", s.noiseLevel, nullptr); st.setProperty("yRs", s.resonAmt, nullptr);
        st.setProperty("yRD", s.resonDrive, nullptr);   // Osc-resonator exciter drive
        st.setProperty("wTb", s.waveTable, nullptr); st.setProperty("wPs", s.wavePos, nullptr);  // wavetable
        st.setProperty("oWp", s.oscWarp, nullptr);   // oscillator wave warp
        st.setProperty("mMa", s.modalMaterial, nullptr); st.setProperty("mDe", s.modalDecay, nullptr);   // modal
        st.setProperty("mTo", s.modalTone, nullptr); st.setProperty("mSt", s.modalStruct, nullptr);
        st.setProperty("mHi", s.modalHit, nullptr); st.setProperty("mDp", s.modalDamp, nullptr);
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
        s.weight = (float)st.getProperty("w", 0.0f);
        s.atk = (float)st.getProperty("atk", d.atk); s.hold = (float)st.getProperty("hld", d.hold);
        s.dec = (float)st.getProperty("dec", d.dec); s.sustain = (float)st.getProperty("sus", d.sustain);
        s.release = (float)st.getProperty("rel", d.release); s.vibrato = (float)st.getProperty("vib", d.vibrato);
        s.oscShape = (int)st.getProperty("oSh", d.oscShape); s.oscShapeB = (int)st.getProperty("oSB", s.oscShape); s.oscFreq = (float)st.getProperty("oFr", d.oscFreq);
        s.oscPEnvAmt = (float)st.getProperty("oEA", d.oscPEnvAmt); s.oscPEnvTime = (float)st.getProperty("oET", d.oscPEnvTime); s.oscPOffset = (float)st.getProperty("oOf", d.oscPOffset);
        s.oscUnison = (int)st.getProperty("oUn", d.oscUnison); s.oscDetune = (float)st.getProperty("oDt", d.oscDetune);
        s.oscUniCenter = (bool)st.getProperty("oUC", d.oscUniCenter);
        s.oscDetuneMode = (int)st.getProperty("oDM", d.oscDetuneMode);
        s.fxDriveType = (int)st.getProperty("fxDt", d.fxDriveType); s.fxDrive = (float)st.getProperty("fxDr", d.fxDrive);
        s.fxReverbSend = (float)st.getProperty("fxRv", d.fxReverbSend); s.fxDelaySend = (float)st.getProperty("fxDl", d.fxDelaySend);
        s.noiseType = (int)st.getProperty("nTy", d.noiseType); s.noiseCenter = (float)st.getProperty("nCt", d.noiseCenter); s.noiseWidth = (float)st.getProperty("nWd", d.noiseWidth);
        s.noiseRes = (float)st.getProperty("nRs", d.noiseRes); s.noiseDrive = (float)st.getProperty("nDr", d.noiseDrive); s.noiseCrackle = (float)st.getProperty("nCk", d.noiseCrackle);
        s.fmPitch = (float)st.getProperty("fPi", d.fmPitch); s.fmSpread = (float)st.getProperty("fSp", d.fmSpread); s.fmDepth = (float)st.getProperty("fDe", d.fmDepth);
        s.fmPEnvAmt = (float)st.getProperty("fEA", d.fmPEnvAmt); s.fmPEnvTime = (float)st.getProperty("fET", d.fmPEnvTime); s.fmPOffset = (float)st.getProperty("fOf", d.fmPOffset);
        s.fmFeedback = (float)st.getProperty("fFb", d.fmFeedback); s.fmSub = (float)st.getProperty("fSu", d.fmSub);
        // Pre-merge projects: an Analog slot saved fmDepth=0.4 (then unused). Now Analog HAS an FM
        // section, so zero those fields for old Analog slots or they'd suddenly be FM-modulated.
        if (! st.hasProperty("v2") && s.engine == SrcOsc) { s.fmDepth = 0.0f; s.fmSpread = 0.0f; s.fmFeedback = 0.0f; s.fmSub = 0.0f; }
        s.physFreq = (float)st.getProperty("pFr", d.physFreq); s.physTone = (float)st.getProperty("pTo", d.physTone); s.physMaterial = (float)st.getProperty("pMa", d.physMaterial); s.physPosition = (float)st.getProperty("pPo", d.physPosition);
        s.physStiff = (float)st.getProperty("pSt", d.physStiff); s.physExcite = (int)st.getProperty("pEx", d.physExcite);
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
        s.oscFold = (float)st.getProperty("yFo", d.oscFold); s.oscLevel = (float)st.getProperty("yOL", d.oscLevel);
        s.noiseLevel = (float)st.getProperty("yNL", d.noiseLevel); s.resonAmt = (float)st.getProperty("yRs", d.resonAmt);
        s.resonDrive = (float)st.getProperty("yRD", d.resonDrive);
        s.waveTable = (int)st.getProperty("wTb", d.waveTable); s.wavePos = (float)st.getProperty("wPs", d.wavePos);
        s.oscWarp = (float)st.getProperty("oWp", d.oscWarp);
        if (! st.hasProperty("v3")) s.oscWarp = 0.0f;   // pre-wavefold: drop the old phase-skew warp (different effect)
        s.modalMaterial = (int)st.getProperty("mMa", d.modalMaterial); s.modalDecay = (float)st.getProperty("mDe", d.modalDecay);
        s.modalTone = (float)st.getProperty("mTo", d.modalTone); s.modalStruct = (float)st.getProperty("mSt", d.modalStruct);
        s.modalHit = (float)st.getProperty("mHi", d.modalHit); s.modalDamp = (float)st.getProperty("mDp", d.modalDamp);
        s.pEnvP[0] = (float)st.getProperty("zP0", d.pEnvP[0]); s.pEnvP[1] = (float)st.getProperty("zP1", d.pEnvP[1]); s.pEnvP[2] = (float)st.getProperty("zP2", d.pEnvP[2]); s.pEnvP[3] = (float)st.getProperty("zP3", d.pEnvP[3]);
        s.pEnvT[0] = (float)st.getProperty("zT0", d.pEnvT[0]); s.pEnvT[1] = (float)st.getProperty("zT1", d.pEnvT[1]); s.pEnvT[2] = (float)st.getProperty("zT2", d.pEnvT[2]); s.pEnvT[3] = (float)st.getProperty("zT3", d.pEnvT[3]);
        // === PER-SLOT EQ (begin) ===
        for (int e = 0; e < NUM_EQ_BANDS; ++e) {
            auto& eb = s.eqBand[e]; const EqBand de = defaultEqBand(e); const juce::String k = "qe" + juce::String(e);
            eb.on = (bool)st.getProperty(k + "o", false); eb.freq = (float)st.getProperty(k + "f", de.freq);
            eb.gainDb = (float)st.getProperty(k + "g", 0.0f); eb.q = (float)st.getProperty(k + "q", de.q);
        }
        // === PER-SLOT EQ (end) ===
        // Reload this slot's sample file (smpStretch was just read, so updateStretch uses the right value).
        { juce::String sp = st.getProperty("sFile", "").toString();
          slotSample[n].buf.setSize(1, 0); slotSample[n].original.setSize(1, 0);
          slotSample[n].file = juce::File(); slotSample[n].usingUser = false;
          if (sp.isNotEmpty()) { juce::File sf(sp); if (sf.existsAsFile()) loadUserSample(n, sf); } }
        ++n;
    }
    for (int b = n; b < NUM_SLOTS; ++b) { slots[b] = Slot(); slotSample[b] = SlotSample(); }   // clear unused slots
    return n > 0;
}

void DrumChannel::prepareToPlay(double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    waveBank();   // force-build the wavetable bank now (message thread), not on the audio thread

    for (auto& b : eqHP) b.reset();  for (auto& b : eqBell) b.reset();  for (auto& b : eqLP) b.reset();
    for (auto& b : formantBP) b.reset();
    driftPhase = 0.0f; punchFast = punchSlow = glueEnv = 0.0f;

    for (auto& v : voices)
    {
        v.playHead = -1.0;
        for (auto& sv : v.sv) sv.noiseBP.reset();
    }

    renderBuf.setSize(2, maxBlockSize);
    fxSendBuf.setSize(4, maxBlockSize);   // per-slot reverb/delay send accumulation (revL,revR,delL,delR)

    bool anySamp = false;
    for (auto& ss : slotSample) if (ss.buf.getNumSamples() > 0) anySamp = true;
    if (! anySamp) loadDefaultSound();

    // Only derive slots from the legacy fields if NONE is set up yet (a brand-new channel had its default slots
    // authored in the processor ctor; a loaded project sets them via readSlots / the post-load buildSlotsFromLegacy).
    // Rebuilding unconditionally here used to wipe the authored default (the first 2 legacy srcOn -> Sample+Noise).
    bool anySlot = false;
    for (auto& s : slots) if (s.engine >= 0) { anySlot = true; break; }
    if (! anySlot) buildSlotsFromLegacy();
    updateDSP();
}

void DrumChannel::loadDefaultSound()
{
    // No built-in synth samples — a Sample slot is silent until a file is loaded into it
    // (the Osc/Noise/FM/Physical engines still make sound).
    {
        const juce::ScopedLock sl(sampleLock);
        for (auto& ss : slotSample) { ss.buf.setSize(1, 0); ss.original.setSize(1, 0); ss.file = juce::File(); ss.usingUser = false; }
        sampleFileRate = 0.0;
        for (auto& v : voices) v.playHead = -1.0;
    }
    rebuildSampleWithPitch();
}

void DrumChannel::loadUserSample(int slot, const juce::File& file)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (!reader) return;

    const int len = (int)reader->lengthInSamples;
    if (len <= 0) return;

    const double fileRate = reader->sampleRate;

    juce::AudioBuffer<float> newSample((int)juce::jmax((unsigned int)1, reader->numChannels), len);
    reader->read(&newSample, 0, len, 0, true, true);

    {
        const juce::ScopedLock sl(sampleLock);
        slotSample[slot].original = std::move(newSample);   // keep the unstretched source
        sampleFileRate = fileRate > 0.0 ? fileRate : sr;
        for (auto& v : voices) v.playHead = -1.0;
    }

    slotSample[slot].usingUser = true;
    slotSample[slot].file = file;
    rebuildSampleWithPitch();
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


void DrumChannel::rebuildSampleWithPitch()
{
    pitchRatio = std::pow(2.0, pitch / 12.0);
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

void DrumChannel::trigger(float velocityGain, float pitchSemis, float pan)
{
    // If this is the channel the editor is analysing, start a fresh spectrum
    // capture aligned to this hit's attack so repeats look identical.
    if (analysisTap != nullptr) analysisTap->arm();

    // Pick a voice. Mono: always voice 0 and silence the rest (cuts previous).
    // Overlap: a free voice, or steal the one nearest its end.
    int vi = 0;
    if (allowOverlap)
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
    driftPhase = 0.0f;
    vibPhase   = 0.0f;

    Voice& v = voices[vi];
    v.velGain    = juce::jlimit(0.0f, 1.0f, velocityGain);
    v.voicePitch = pitchSemis;
    v.voicePan   = juce::jlimit(-1.0f, 1.0f, pan);
    v.playHead = 0.0;          // alive (per-slot sample heads do the reading)
    v.voiceSamples = 0;

    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        SlotVoice& sv = v.sv[s];
        const Slot& sl = slots[s];
        sv.sinePhase = 0.0;
        sv.noiseBP.reset();
        sv.pinkB[0] = sv.pinkB[1] = sv.pinkB[2] = 0.0f;
        sv.brownState = sv.prevWhite = 0.0f;
        sv.greyZ1 = sv.greyZ2 = 0.0f;
        sv.fmCarrier = sv.fmMod = sv.fmSubPhase = 0.0; sv.fmFbState = 0.0f;
        sv.wtPhase = 0.0; sv.modalInit = false;   // re-strike the modal bank on this hit
        for (int m = 0; m < MODAL_MODES; ++m) { sv.modalY1[m] = 0.0f; sv.modalY2[m] = 0.0f; }  // clean state every hit
        sv.noiseState = 0x1234567u + (uint32_t)(vi * NUM_SLOTS + s) * 2654435761u; // distinct per voice+slot
        for (int u = 0; u <= UNI_MAX; ++u) sv.uniPhase[u] = (2.0 * kPi) * (double) u / (double) UNI_MAX;

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
        // by the Material model, then comb it by the strike Position.
        sv.ksWrite = 0.0; sv.ksLp = 0.0f;
        for (auto& a : sv.ksApSt) a = 0.0f;
        std::fill(std::begin(sv.ksBuf), std::end(sv.ksBuf), 0.0f);
        const bool ksPluck = (sl.engine == SrcPhys)
                           || (sl.engine == SrcSynth && sl.resonAmt > 0.001f)
                           || (sl.engine == SrcOsc   && sl.resonAmt > 0.001f);   // Osc engine's resonator section
        if (ksPluck)
        {
            const float ksFreq = (sl.engine == SrcPhys) ? sl.physFreq : sl.oscFreq;
            const int L = juce::jlimit(2, KS_MAX - 2, (int) std::round(sr / juce::jmax(20.0f, ksFreq)));
            const int mdl = juce::jlimit(0, kNumPhysModels - 1, (int) std::lround(sl.physMaterial));
            // EXCITATION shapes how the string/bar is set in motion: Pluck = full-length noise burst (rich);
            // Strike = a short sharp burst at the start (percussive mallet hit); Mallet = a soft, rounded burst.
            const int exc = (sl.engine == SrcPhys) ? juce::jlimit(0, 2, sl.physExcite) : 0;
            const float eb = (exc == 2) ? kPhysModels[mdl].exciteBright * 0.35f   // Mallet: softer (darker burst)
                                        :  kPhysModels[mdl].exciteBright;
            const int burst = (exc == 1) ? juce::jmax(2, L / 5) : L;              // Strike: only the first ~1/5 excited
            uint32_t rng = sv.noiseState ^ 0x9e3779b9u;
            float lp = 0.0f;
            for (int i = 0; i < L; ++i) { lp += eb * (whiteNoise(rng) - lp); sv.ksBuf[i] = (i < burst) ? lp : 0.0f; }
            const float pos = juce::jlimit(0.0f, 1.0f, sl.physPosition);
            if (pos > 0.01f)
            {
                const int d = juce::jlimit(1, L - 1, (int) std::round(pos * 0.5f * L));
                for (int i = L - 1; i >= d; --i) sv.ksBuf[i] -= 0.85f * sv.ksBuf[i - d];
            }
            sv.ksWrite = (double) L;
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
    auto feedSilence = [this, numSamples]() {
        meterPeak.store(0.0f, std::memory_order_relaxed);   // nothing rendered -> meter falls
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
    driftPhase += 2.0f * kPi * 0.25f * (float) numSamples / (float) sr;   // ~0.25 Hz
    if (driftPhase > 2.0f * kPi) driftPhase -= 2.0f * kPi;
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
        int    noiseType = 0; bool noiseBP = false; double noiseFc = 1000, nQ = 0.7; float noiseDrive = 0, noiseCrackle = 0;
        float  greyB0 = 1, greyB1 = 0, greyB2 = 0, greyA1 = 0, greyA2 = 0;   // grey = white through a mid-scoop peaking biquad
        double fmCarrierF = 220, fmModF = 220; float fmIndex = 0, fmPEnvAmt = 0, fmPEnvTime = 0.05f, fmPOffset = 0, fmFeedback = 0, fmSub = 0;
        float  fmRatio = 1.0f;   // Analog+FM merge: modulator freq = carrier freq * fmRatio (per-sample, tracks vibrato/pitch)
        const PhysModel* pm = &kPhysModels[0]; float ksFb = 0, ksLpC = 0.5f, ksApC = 0; int ksApN = 0;
        double physBaseF = 110; float physPEnvAmt = 0, physPEnvTime = 0.05f, physPOffset = 0, physVibFac = 1;
        float  crushStep = 0; double speed = 1; float smpPitch = 0, smpPEnvAmt = 0, smpPEnvTime = 0.04f, smpPOffset = 0; bool reverse = false;
        const juce::AudioBuffer<float>* buf = nullptr; int srcLen = 0, regLo = 0, regHi = 0, slices = 1;  // per-slot sample
        float  smpGain = 1.0f;   // sample output boost
        // -- Synth (unified) extras: section levels + fold + resonator on --
        // (reson + resonDrive are ALSO used by the SrcOsc resonator section.)
        float  oscFold = 0, oscLevel = 1, noiseLevel = 0; bool reson = false; float resonDrive = 0; float resonMix = 1;
        float  oscWarp = 0.0f;                      // wave WARP = one-way wavefold (0 = off)
        int    fxDriveType = 0; float fxDrive = 0;  // per-slot Drive (insert); fxRevSend/fxDelSend = per-slot send amounts
        float  fxRevSend = 0, fxDelSend = 0;
        int    waveTable = 0; float wavePos = 0;   // wavetable (SrcWave)
        int    modalN = 0; float modalA1[MODAL_MODES] = {}, modalA2[MODAL_MODES] = {}, modalGain[MODAL_MODES] = {};  // modal bank
        float  modalDecaySec = 0.5f;   // base ring length (for voiceEnd)
        float  gL = 1, gR = 1;
        // -- 4-point pitch envelope (applies on top of the legacy per-engine env) --
        bool   pEnvOn = false; float pEnvP[Slot::NPE] = { 0, 0, 0, 0 }, pEnvT[Slot::NPE] = { 0.2f, 0.4f, 0.6f, 0.8f };
        double voiceLenSamp = 1.0;   // sound length in samples, for the time-fraction axis
        // === PER-SLOT EQ (begin) - coeffs (state lives per-voice in SlotVoice) ===
        bool   eqAny = false; bool eqUse[7] = {}; Biquad eqBq[7];
        // === PER-SLOT EQ (end) ===
    } sc[NUM_SLOTS];

    bool anySlotActive = false;
    int  domSlot = -1; float domW = -1.0f;
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        const Slot& sl = slots[s];
        SC& c = sc[s];
        if (sl.engine < 0 || sl.weight <= 0.0f) continue;
        if (sl.engine == SrcSample && slotSample[s].buf.getNumSamples() == 0) continue;
        c.engine = sl.engine; c.weight = sl.weight;
        c.fxDriveType = sl.fxDriveType; c.fxDrive = sl.fxDrive; c.fxRevSend = sl.fxReverbSend; c.fxDelSend = sl.fxDelaySend;  // per-slot FX
        c.atk = sl.atk; c.hold = sl.hold; c.dec = sl.dec; c.sustain = sl.sustain; c.release = sl.release;
        // 3-point pitch envelope + the sound length its time-axis is measured against
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
                c.oscShape = sl.oscShape; c.oscShapeB = sl.oscShapeB; c.oscFreq = sl.oscFreq;
                c.oscPEnvAmt = sl.oscPEnvAmt; c.oscPEnvTime = sl.oscPEnvTime; c.oscPOffset = sl.oscPOffset;
                c.uniVoices = juce::jlimit(1, UNI_MAX, sl.oscUnison);
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;   // up to +/-100 cents (1 semitone) spread
                c.uniCenter = sl.oscUniCenter; c.uniMode = sl.oscDetuneMode;
                c.uniGain   = 1.0f / std::sqrt((float) (c.uniVoices + (c.uniCenter ? 1 : 0)));
                c.oscVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.09f * vibLfo;
                // Merged FM section (Depth 0 = pure analog). Modulator = sine at carrier*ratio.
                c.oscWarp    = sl.oscWarp;
                c.fmRatio    = 1.0f + (float) sl.fmSpread * 5.0f;
                c.fmIndex    = sl.fmDepth * 12.0f;
                c.fmFeedback = sl.fmFeedback; c.fmSub = sl.fmSub;
                c.reson = false;   // resonator REMOVED from Analog+FM (use the standalone Physical engine instead)
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
                c.ksLpC = juce::jlimit(0.02f, 1.0f, (0.05f + 0.95f * sl.physTone) * c.pm->briScale);
                // STIFFNESS = user dispersion: stretches the partials inharmonic (string -> bar -> bell). 0 = the
                // material's own value (factory unchanged); raising it adds allpass coefficient + stages.
                { const float st = juce::jlimit(0.0f, 1.0f, sl.physStiff);
                  c.ksApC = juce::jlimit(-0.92f, 0.92f, c.pm->apC + st * 0.85f * (c.pm->apC < 0.0f ? -1.0f : 1.0f));
                  c.ksApN = (st > 0.01f) ? juce::jlimit(0, 6, juce::jmax(c.pm->apStages, juce::roundToInt(st * 6.0f)))
                                         : c.pm->apStages; }
                c.physBaseF  = juce::jlimit(20.0, sr * 0.45, (double) sl.physFreq);
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
                c.oscVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.09f * vibLfo;  // vibrato = varispeed wobble
                break; }
            case SrcSynth: {
                // Unified voice: oscillator(+FM+fold) and noise sections, optionally fed
                // into a Karplus-Strong resonator. Coefficients mirror the dedicated
                // engines exactly so a synth in "osc only" / "noise only" / etc. is
                // bit-identical to Analog / Noise / FM / Physical.
                c.oscLevel   = juce::jlimit(0.0f, 1.0f, sl.oscLevel);
                c.noiseLevel = juce::jlimit(0.0f, 1.0f, sl.noiseLevel);
                c.oscFold    = juce::jlimit(0.0f, 1.0f, sl.oscFold);
                // --- oscillator + FM (carrier = osc freq) ---
                c.oscShape  = sl.oscShape; c.oscFreq = juce::jlimit(8.0, sr * 0.45, (double) sl.oscFreq);
                c.oscPEnvAmt = sl.oscPEnvAmt; c.oscPEnvTime = sl.oscPEnvTime; c.oscPOffset = sl.oscPOffset;
                c.uniVoices = juce::jlimit(1, UNI_MAX, sl.oscUnison);
                c.uniCents  = juce::jlimit(0.0f, 1.0f, sl.oscDetune) * 100.0f;   // up to +/-100 cents (1 semitone) spread
                c.uniCenter = sl.oscUniCenter; c.uniMode = sl.oscDetuneMode;
                c.uniGain   = 1.0f / std::sqrt((float) (c.uniVoices + (c.uniCenter ? 1 : 0)));
                c.oscVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.09f * vibLfo;
                c.fmModF    = c.oscFreq * (1.0 + (double) sl.fmSpread * 5.0);
                c.fmIndex   = sl.fmDepth * 12.0f; c.fmFeedback = sl.fmFeedback;
                // --- noise section ---
                c.noiseType = sl.noiseType;
                c.noiseBP   = (sl.noiseWidth > 0.02f);
                c.nQ        = 0.5 + (double)(sl.noiseWidth * sl.noiseWidth) * 15.0;
                c.noiseFc   = juce::jlimit(40.0, sr * 0.45, (double) sl.noiseCenter);
                // --- resonator (KS) ---
                c.reson = (sl.resonAmt > 0.001f);
                if (c.reson) {
                    c.pm    = &kPhysModels[juce::jlimit(0, kNumPhysModels - 1, (int) std::lround(sl.physMaterial))];
                    const float rd = juce::jlimit(0.02f, 1.0f, sl.resonAmt) * 4.0f;   // resonAmt -> ring time
                    c.ksFb  = std::exp(-3.0f / juce::jmax(0.02f, rd * c.pm->decScale) / (float) sr);
                    c.ksLpC = juce::jlimit(0.02f, 1.0f, (0.05f + 0.95f * sl.physTone) * c.pm->briScale);
                    c.ksApC = c.pm->apC; c.ksApN = c.pm->apStages;
                    c.physBaseF = c.oscFreq;
                }
                break; }
            case SrcWave:
                c.oscFreq   = juce::jlimit(8.0, sr * 0.45, (double) sl.oscFreq);
                c.waveTable = sl.waveTable;
                c.wavePos   = juce::jlimit(0.0f, 1.0f, sl.wavePos);
                c.oscVibFac = 1.0f + juce::jlimit(0.0f, 1.0f, sl.vibrato) * 0.09f * vibLfo;
                break;
            case SrcModal: {
                // Build the resonator bank from the Material + base pitch + Decay/Tone/Structure.
                const auto& M = kModalMaterials[juce::jlimit(0, kNumModalMaterials - 1, sl.modalMaterial)];
                const double baseF = juce::jlimit(20.0, sr * 0.45, (double) sl.oscFreq);
                const float  decaySec = 0.05f + juce::jlimit(0.0f, 1.0f, sl.modalDecay) * 3.95f;     // 0.05..4 s
                const float  tone   = juce::jlimit(0.0f, 1.0f, sl.modalTone);
                const float  stretch = 0.6f + juce::jlimit(0.0f, 1.0f, sl.modalStruct) * 0.8f;        // 0.6..1.4 (0.5->1.0)
                const float  hit  = juce::jlimit(0.0f, 1.0f, sl.modalHit);    // strike-position comb amount (0 = none)
                const float  damp = juce::jlimit(0.0f, 1.0f, sl.modalDamp);   // extra ring damping (0 = none)
                const float  hitPos = 0.5f * hit;                            // strike point moves edge -> centre
                c.modalN = 0;
                for (int i = 0; i < M.n && i < MODAL_MODES; ++i) {
                    double ratio = 1.0 + (M.ratio[i] - 1.0) * stretch;                                 // inharmonicity
                    double f = baseF * ratio;
                    if (f >= sr * 0.48) continue;                                                      // skip modes above Nyquist
                    const float hi = (float) i / (float) juce::jmax(1, M.n - 1);                       // 0..1 mode height
                    // decay: per-material, shortened for higher modes - more so when dark (low tone), more with Damp.
                    const float dmul = M.decayMul[i] * (1.0f - (1.0f - tone) * 0.6f * hi) * (1.0f - damp * (0.3f + 0.7f * hi));
                    const float dSec = juce::jmax(0.02f, decaySec * dmul);
                    const float r  = std::exp(-1.0f / (dSec * (float) sr));                            // per-sample pole radius
                    const double w = 2.0 * kPi * f / sr;
                    const int k = c.modalN++;
                    c.modalA1[k] = 2.0f * r * (float) std::cos(w);
                    c.modalA2[k] = r * r;
                    // gain: material gain, tilted by Tone (dark damps highs). x sin(w) = the impulse-input
                    // coefficient so each mode's ring is ~gain amplitude regardless of its frequency.
                    float g = M.gain[i] * (0.35f + 0.65f * (tone * 0.5f + 0.5f) * (1.0f - 0.5f * hi * (1.0f - tone)));
                    // Hit position: striking at a node of a mode doesn't excite it (comb). hit=0 -> full (no comb).
                    const float comb = std::abs(std::sin((float)(i + 1) * juce::MathConstants<float>::pi * hitPos));
                    g *= (1.0f - hit) + hit * comb;
                    c.modalGain[k] = g * (float) std::sin(w);
                }
                c.oscFreq = baseF;
                c.modalDecaySec = decaySec;
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
    }

    if (! anySlotActive)
    { for (auto& v : voices) v.playHead = -1.0; feedSilence(); return; }

    // Blend character (Bloom/Drift/Spread/Punch/Glue) was REMOVED from the UI - force every amount to 0 so it has
    // ZERO effect (no phantom drift/spread/etc). The math below stays but is neutral. (Fields are now vestigial.)
    const float spreadAmt = 0.0f;
    static const float panBaseSlot[NUM_SLOTS] = { -0.6f, 0.6f };   // slot 0 left, slot 1 right at full spread
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        const float p = spreadAmt * panBaseSlot[s];
        sc[s].gL = juce::jmin(1.0f, 1.0f - p);
        sc[s].gR = juce::jmin(1.0f, 1.0f + p);
    }

    // Drift: slow per-slot weight wander. (REMOVED -> 0.)
    const float driftAmt = 0.0f;
    float driftMod[NUM_SLOTS];
    for (int s = 0; s < NUM_SLOTS; ++s)
        driftMod[s] = juce::jmax(0.0f, 1.0f + driftAmt * 1.4f * std::sin(driftPhase + (float) s * 1.7f));

    // Bloom: at the attack only the loudest slot plays; the rest swell in. (REMOVED -> 0.)
    const float bloomAmt  = 0.0f;
    const float bloomRise = 1.0f / ((0.04f + 0.20f * bloomAmt) * (float) sr);

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
            // Samples ignore AHDSR - play the full (trimmed) sample, then a short anti-click fade.
            const long natural = (long) ((double) (c.regHi - c.regLo) * (double) engineOS / juce::jmax(0.05, c.speed) / (double) c.slices) + 8;
            voiceEnd = juce::jmax(voiceEnd, natural);
        }
        else if (c.engine == SrcPhys)
            voiceEnd = juce::jmax(voiceEnd, (long) ((c.atk + c.hold + 3.6f * c.dec * c.pm->decScale) * (float) sr) + 8
                                            + (c.sustain > 0.001f ? susTail : 0));
        else if (c.engine == SrcSynth || (c.engine == SrcOsc && c.reson))
        {   // amp tail, plus the resonator's ring when it is on (Synth + Osc-resonator)
            long e = ahdEnd + (c.sustain > 0.001f ? susTail : 0);
            if (c.reson) e = juce::jmax(e, (long) ((c.atk + c.hold + 3.6f * c.dec * c.pm->decScale) * (float) sr) + 8);
            voiceEnd = juce::jmax(voiceEnd, e);
        }
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

    // === PER-SLOT EQ (begin) - spectrum tap for one slot: clear the accumulator ===
    const int tapSlot = (analysisTap != nullptr && analysisSlot >= 0 && analysisSlot < NUM_SLOTS) ? analysisSlot : -1;
    if (tapSlot >= 0) { const int nn = juce::jmin(numSamples, (int) std::size(analysisBuf)); for (int i = 0; i < nn; ++i) analysisBuf[i] = 0.0f; }
    // === PER-SLOT EQ (end) ===

    // ---- Voice loop -----------------------------------------------------------
    for (int vi = 0; vi < POLY; ++vi)
    {
        Voice& v = voices[vi];
        if (! v.active()) continue;

        // Set noise band-pass coefficients once per voice (Noise + Synth-noise slots).
        for (int s = 0; s < NUM_SLOTS; ++s)
            if ((sc[s].engine == SrcNoise || sc[s].engine == SrcSynth) && sc[s].noiseBP)
                v.sv[s].noiseBP.bandpass(sr, sc[s].noiseFc, sc[s].nQ);

        // Channel PITCH (semitones). SYNTHS shift frequency here (vPitchMul). SAMPLES get the channel/slot
        // pitch baked into their buffer by SoundTouch (pitch-shift, no length change) - so they use ONLY
        // the per-step pitch here (vStepMul, still varispeed since it's per-hit). Both include voicePitch.
        const double vStepMul  = v.voicePitch != 0.0f ? std::pow(2.0, (double) v.voicePitch / 12.0) : 1.0;  // samples
        const double vPitchMul = pitch != 0.0f ? vStepMul * std::pow(2.0, (double) pitch / 12.0) : vStepMul; // synths
        // Per-step PAN: balance this voice's stereo output before it sums in (rides ON TOP of the
        // channel pan applied later). 0 = centre, -1 = hard left, +1 = hard right.
        const float vPanL = v.voicePan <= 0.0f ? 1.0f : 1.0f - v.voicePan;
        const float vPanR = v.voicePan >= 0.0f ? 1.0f : 1.0f + v.voicePan;
        bool finished = false;

        for (int i = 0; i < numSamples; ++i)
        {
            const long t = v.voiceSamples;
            float vEnv = 0.0f, mixL = 0.0f, mixR = 0.0f;
            const float be = bloomAmt > 0.0001f ? (1.0f - std::exp(-(float) t * bloomRise)) : 1.0f;
            const float bGate = (1.0f - bloomAmt) + bloomAmt * be;

            for (int s = 0; s < NUM_SLOTS; ++s)
            {
                const SC& c = sc[s];
                if (c.engine < 0) continue;
                SlotVoice& sv = v.sv[s];
                float sig = 0.0f, sL = 0.0f, sR = 0.0f, env = 0.0f;
                bool  stereo = false;

                // 3-point pitch envelope -> frequency multiplier (1.0 when unused). Applies to every engine.
                double pe3Mul = 1.0;
                if (c.pEnvOn) {
                    const float frac = juce::jlimit(0.0f, 1.0f, (float)((double) t / c.voiceLenSamp));
                    pe3Mul *= std::pow(2.0, (double) pitchEnv4(frac, c.pEnvP, c.pEnvT) / 12.0);
                }

                switch (c.engine)
                {
                    case SrcOsc: {
                        env = ahdsEnv(t, c.atk, c.hold, c.dec, c.sustain, c.release);
                        double sSemis = (c.oscPEnvAmt != 0.0f) ? (double) c.oscPEnvAmt * pitchEnvShape(t, c.oscPEnvTime, c.oscPOffset) : 0.0;
                        double freq = c.oscFreq * std::pow(2.0, sSemis / 12.0) * vPitchMul * c.oscVibFac * pe3Mul;
                        // Single "Wave" selector (the From/To over-the-note morph was retired - it sounded harsh).
                        const float pos = (float) c.oscShape;
                        // Merged FM: a sine modulator bends the carrier phase. Depth (fmIndex) 0 => no
                        // modulation => identical to pure (band-limited) analog. Modulator tracks the
                        // carrier freq * ratio so it stays in tune under vibrato / pitch env.
                        float fmAdd = 0.0f;
                        if (c.fmIndex > 0.0001f) {
                            const float modOut = (float) std::sin(sv.fmMod + c.fmFeedback * 6.0f * sv.fmFbState);
                            sv.fmFbState = 0.5f * (sv.fmFbState + modOut);
                            fmAdd = c.fmIndex * modOut;
                            sv.fmMod += 2.0 * kPi * freq * (double) c.fmRatio / sr;
                            if (sv.fmMod > 2.0 * kPi) sv.fmMod -= 2.0 * kPi;
                        }
                        const bool fmActive = c.fmIndex > 0.0001f;
                        float wsum = 0.0f;
                        const int totalV = c.uniVoices + (c.uniCenter ? 1 : 0);   // +1 dry/centre voice
                        for (int u = 0; u < totalV; ++u) {
                            const bool centreVoice = (c.uniCenter && u == c.uniVoices);   // the extra undetuned voice
                            float sp = 0.0f;   // mode: 0=symmetric (+/-), 1=up (all sharp), 2=down (all flat)
                            if (! centreVoice && c.uniVoices > 1) {
                                const float frac = (float) u / (float)(c.uniVoices - 1);   // 0..1
                                sp = (c.uniMode == 1) ? frac : (c.uniMode == 2) ? -frac : (2.0f * frac - 1.0f);
                            }
                            const double det = std::pow(2.0, (double)(sp * c.uniCents) / 1200.0);
                            const float dt = (float)(freq * det / sr);   // cycles/sample for PolyBLEP
                            // FM active -> plain morphWave (matches the old FM engine exactly, no BLEP under
                            // phase modulation); FM off -> band-limited morphWaveBL (the clean analog path).
                            const double wph = sv.uniPhase[u];   // (Warp is now an output wavefold, applied below)
                            wsum += fmActive ? morphWave(wph + fmAdd, pos)
                                             : morphWaveBL(wph, pos, dt);
                            sv.uniPhase[u] += 2.0 * kPi * freq * det / sr;
                            if (sv.uniPhase[u] > 2.0 * kPi) sv.uniPhase[u] -= 2.0 * kPi;
                        }
                        float oo = wsum * c.uniGain;
                        if (c.oscWarp > 0.001f) {                       // WARP = one-way wavefold (adds harmonics/grit)
                            const float folded = std::sin(oo * (1.0f + c.oscWarp * 4.0f) * 1.5707963f);
                            oo += c.oscWarp * (folded - oo);
                        }
                        if (c.fmSub > 0.001f) {                         // sub-oscillator an octave down
                            oo = oo * (1.0f - 0.4f * c.fmSub) + (float) std::sin(sv.fmSubPhase) * c.fmSub;
                            sv.fmSubPhase += 2.0 * kPi * freq * 0.5 / sr;
                            if (sv.fmSubPhase > 2.0 * kPi) sv.fmSubPhase -= 2.0 * kPi;
                        }
                        sig = oo * env;   // Analog + FM only (the resonator was removed from this engine -> use Physical)
                        sv.sinePhase = sv.uniPhase[0];
                        break; }
                    case SrcNoise: {
                        env = ahdsEnv(t, c.atk, c.hold, c.dec, c.sustain, c.release);
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
                        env = ahdsEnv(t, c.atk, c.hold, c.dec, c.sustain, c.release);
                        double fmPMul = (c.fmPEnvAmt != 0.0f) ? std::pow(2.0, (double) c.fmPEnvAmt * pitchEnvShape(t, c.fmPEnvTime, c.fmPOffset) / 12.0) : 1.0;
                        float modOut = (float) std::sin(sv.fmMod + c.fmFeedback * 6.0f * sv.fmFbState);
                        sv.fmFbState = 0.5f * (sv.fmFbState + modOut);
                        // Carrier waveform morphs Wave A -> Wave B over the note (static when equal).
                        float cpos = (float) c.oscShape;
                        if (c.oscShapeB != c.oscShape) cpos += ((float) c.oscShapeB - (float) c.oscShape) * (1.0f - decayCurve(t, juce::jmax(0.02f, c.dec)));
                        float fm = morphWave(sv.fmCarrier + c.fmIndex * modOut, cpos);
                        if (c.fmSub > 0.001f) {
                            fm = fm * (1.0f - 0.4f * c.fmSub) + (float) std::sin(sv.fmSubPhase) * c.fmSub;
                            sv.fmSubPhase += 2.0 * kPi * c.fmCarrierF * 0.5 * fmPMul * vPitchMul * pe3Mul / sr;
                            if (sv.fmSubPhase > 2.0 * kPi) sv.fmSubPhase -= 2.0 * kPi;
                        }
                        sv.fmCarrier += 2.0 * kPi * c.fmCarrierF * fmPMul * vPitchMul * pe3Mul / sr;
                        sv.fmMod     += 2.0 * kPi * c.fmModF * fmPMul * vPitchMul * pe3Mul / sr;
                        if (sv.fmCarrier > 2.0 * kPi) sv.fmCarrier -= 2.0 * kPi;
                        if (sv.fmMod     > 2.0 * kPi) sv.fmMod     -= 2.0 * kPi;
                        sig = fm * env;
                        break; }
                    case SrcPhys: {
                        env = ahdsEnv(t, c.atk, c.hold, c.dec, c.sustain, c.release);
                        double pSemis = (c.physPEnvAmt != 0.0f) ? (double) c.physPEnvAmt * pitchEnvShape(t, c.physPEnvTime, c.physPOffset) : 0.0;
                        if (c.pm->pitchDrop != 0.0f) pSemis += (double) c.pm->pitchDrop * std::exp(-(float) t * 3.0f / (0.06f * (float) sr));
                        double f = c.physBaseF * std::pow(2.0, pSemis / 12.0) * vPitchMul * c.physVibFac * pe3Mul;
                        double L = juce::jlimit(2.0, (double)(KS_MAX - 2), sr / juce::jmax(20.0, f));
                        double rp = sv.ksWrite - L; while (rp < 0.0) rp += (double) KS_MAX;
                        const int ri = (int) rp; const float fr = (float)(rp - ri);
                        const float k0 = sv.ksBuf[ri], k1 = sv.ksBuf[(ri + 1) % KS_MAX];
                        float y = k0 + fr * (k1 - k0);
                        sv.ksLp += c.ksLpC * (y - sv.ksLp);
                        float ss = sv.ksLp;
                        for (int st = 0; st < c.ksApN; ++st) { float yy = c.ksApC * ss + sv.ksApSt[st]; sv.ksApSt[st] = ss - c.ksApC * yy; ss = yy; }
                        const int wi = (int) sv.ksWrite % KS_MAX;
                        // String feedback. Normally the Decay-derived ksFb (the string decays on its own). But a
                        // plucked string can't be "held", so DURING the amp-env HOLD window we sustain it (near-
                        // lossless ring) so amp-env Hold actually holds. Hold=0 -> the window is empty -> this is a
                        // no-op (bit-identical to a pure pluck); only sounds with Hold>0 ring through the hold.
                        float ksFb = c.ksFb;
                        const long aS = (long) (c.atk * (float) sr), hS = (long) (c.hold * (float) sr);
                        // Sustain the string through the STRIKE (a slow attack) + any hold, so the amp env swells the
                        // pluck to FULL instead of fading in an already-decaying string (the weak-peak with slow attacks).
                        // Instant plucks (atk<=20ms, hold=0) are untouched -> factory Physical sounds stay bit-identical.
                        if (t < aS + hS && (c.atk > 0.005f || c.hold > 0.0001f)) ksFb = 0.9997f;   // (the loop LP still dulls it)
                        sv.ksBuf[wi] = juce::jlimit(-2.5f, 2.5f, ss * ksFb); sv.ksWrite = wi + 1;   // safety bound (pure pluck never reaches it)
                        sig = (ss * 1.4f) * env;
                        break; }
                    case SrcSample: {
                        // No AHDSR for samples - play at full level with short anti-click
                        // fades at the very start and just before the (trimmed) end.
                        env = 1.0f;
                        const long fin = (long) (0.003 * sr), fout = (long) (0.010 * sr), endN = (long) c.voiceLenSamp;
                        if (fin  > 0 && t < fin)          env  = (float) t / (float) fin;
                        if (fout > 0 && t > endN - fout)  env *= juce::jmax(0.0f, (float)(endN - t) / (float) fout);
                        const double head = sv.smpHead;
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
                            if (phaseInvert) { sL = -sL; sR = -sR; }
                        }
                        sL *= env * c.smpGain; sR *= env * c.smpGain;   // sample output boost
                        // Static pitch (channel + slot) is baked into the buffer (SoundTouch) so it doesn't
                        // change length; here only the TIME-VARYING pitches remain (legacy env + per-step),
                        // which are varispeed by nature. The 4-dot pitch ENVELOPE (pe3Mul) is varispeed too.
                        double smpSemis = 0.0;
                        if (c.smpPEnvAmt != 0.0f) smpSemis += (double) c.smpPEnvAmt * pitchEnvShape(t, c.smpPEnvTime, c.smpPOffset);
                        sv.smpHead += c.speed * std::pow(2.0, smpSemis / 12.0) * vStepMul * c.oscVibFac * pe3Mul / (double) engineOS;
                        stereo = true;
                        break; }
                    case SrcSynth: {
                        // ---- Unified voice: (osc+FM+fold) + noise -> optional resonator ----
                        env = ahdsEnv(t, c.atk, c.hold, c.dec, c.sustain, c.release);
                        const double pMul = (c.oscPEnvAmt != 0.0f)
                            ? std::pow(2.0, (double) c.oscPEnvAmt * pitchEnvShape(t, c.oscPEnvTime, c.oscPOffset) / 12.0) : 1.0;

                        float ex = 0.0f;

                        // -- oscillator + FM + fold (only when the osc section is up) --
                        if (c.oscLevel > 0.0001f)
                        {
                            const double freq = c.oscFreq * pMul * vPitchMul * c.oscVibFac * pe3Mul;
                            // FM modulator (one operator, with feedback), shared by unison voices.
                            float modOut = (float) std::sin(sv.fmMod + c.fmFeedback * 6.0f * sv.fmFbState);
                            sv.fmFbState = 0.5f * (sv.fmFbState + modOut);
                            const float phaseMod = c.fmIndex * modOut;
                            sv.fmMod += 2.0 * kPi * c.fmModF * pMul * vPitchMul * pe3Mul / sr;
                            if (sv.fmMod > 2.0 * kPi) sv.fmMod -= 2.0 * kPi;
                            auto shape = [&](double phase) -> float {
                                const float ph = (float)(phase / (2.0 * kPi) - std::floor(phase / (2.0 * kPi)));
                                switch (c.oscShape) {
                                    case OscTriangle: return 1.0f - 4.0f * std::abs(ph - 0.5f);
                                    case OscSquare:   return (ph < 0.5f) ? 1.0f : -1.0f;
                                    case OscSaw:      return 2.0f * ph - 1.0f;
                                    default:          return (float) std::sin(phase);
                                }
                            };
                            float wsum = 0.0f;
                            const int totalV = c.uniVoices + (c.uniCenter ? 1 : 0);   // +1 dry/centre voice
                            for (int u = 0; u < totalV; ++u) {
                                const bool centreVoice = (c.uniCenter && u == c.uniVoices);
                                float sp = 0.0f;   // mode: 0=symmetric, 1=up, 2=down
                                if (! centreVoice && c.uniVoices > 1) {
                                    const float frac = (float) u / (float)(c.uniVoices - 1);
                                    sp = (c.uniMode == 1) ? frac : (c.uniMode == 2) ? -frac : (2.0f * frac - 1.0f);
                                }
                                const double det = std::pow(2.0, (double)(sp * c.uniCents) / 1200.0);
                                float w = shape(sv.uniPhase[u] + phaseMod);
                                if (c.oscFold > 0.0001f) {                 // wavefold (fold=0 stays identity)
                                    const float folded = std::sin(w * (1.0f + c.oscFold * 5.0f) * 1.5707963f);
                                    w += c.oscFold * (folded - w);
                                }
                                wsum += w;
                                sv.uniPhase[u] += 2.0 * kPi * freq * det / sr;
                                if (sv.uniPhase[u] > 2.0 * kPi) sv.uniPhase[u] -= 2.0 * kPi;
                            }
                            ex += c.oscLevel * wsum * c.uniGain;
                            sv.sinePhase = sv.uniPhase[0];
                        }

                        // -- noise section --
                        if (c.noiseLevel > 0.0001f)
                        {
                            float wN = whiteNoise(sv.noiseState), col;
                            switch (c.noiseType) {
                                case 1:
                                    sv.pinkB[0] = 0.99765f * sv.pinkB[0] + wN * 0.0990460f;
                                    sv.pinkB[1] = 0.96300f * sv.pinkB[1] + wN * 0.2965164f;
                                    sv.pinkB[2] = 0.57000f * sv.pinkB[2] + wN * 1.0526913f;
                                    col = (sv.pinkB[0] + sv.pinkB[1] + sv.pinkB[2] + wN * 0.1848f) * 0.35f; break;
                                case 2:
                                    sv.brownState = juce::jlimit(-1.0f, 1.0f, sv.brownState + 0.02f * wN);
                                    col = sv.brownState * 3.5f; break;
                                case 3: {   // GREY = mid-scoop biquad (same as the Noise engine)
                                    const float y = c.greyB0 * wN + sv.greyZ1;
                                    sv.greyZ1 = c.greyB1 * wN - c.greyA1 * y + sv.greyZ2;
                                    sv.greyZ2 = c.greyB2 * wN - c.greyA2 * y;
                                    col = y * 1.4f; break; }
                                case 4: { float hi = wN - sv.prevWhite; sv.prevWhite = wN; col = hi * 0.7f; break; }
                                default: col = wN; break;
                            }
                            float nOut = c.noiseBP ? sv.noiseBP.process(col, 0) * 3.0f : col;
                            ex += c.noiseLevel * nOut;
                        }

                        // -- optional Karplus-Strong resonator (ex injected; pure pluck when ex==0) --
                        if (c.reson)
                        {
                            const double f = c.physBaseF * pMul * vPitchMul * c.oscVibFac * pe3Mul;
                            const double L = juce::jlimit(2.0, (double)(KS_MAX - 2), sr / juce::jmax(20.0, f));
                            double rp = sv.ksWrite - L; while (rp < 0.0) rp += (double) KS_MAX;
                            const int ri = (int) rp; const float fr = (float)(rp - ri);
                            const float k0 = sv.ksBuf[ri], k1 = sv.ksBuf[(ri + 1) % KS_MAX];
                            float y = k0 + fr * (k1 - k0);
                            sv.ksLp += c.ksLpC * (y - sv.ksLp);
                            float ss = sv.ksLp;
                            for (int st = 0; st < c.ksApN; ++st) { float yy = c.ksApC * ss + sv.ksApSt[st]; sv.ksApSt[st] = ss - c.ksApC * yy; ss = yy; }
                            const int wi = (int) sv.ksWrite % KS_MAX;
                            // Cap the feedback line: a resonator driven continuously by `ex` (Drive) with
                            // feedback ~1 would build up without bound -> a huge spike -> "gunshot" that
                            // echoes via the FX sends. Real resonators saturate; so do we (bounded, no explosion).
                            sv.ksBuf[wi] = juce::jlimit(-2.5f, 2.5f, ss * c.ksFb + ex * 0.5f); sv.ksWrite = wi + 1;
                            sig = (ss * 1.4f) * env;
                        }
                        else
                            sig = ex * env;
                        break; }
                    case SrcWave: {
                        env = ahdsEnv(t, c.atk, c.hold, c.dec, c.sustain, c.release);
                        const double freq = c.oscFreq * vPitchMul * c.oscVibFac * pe3Mul;
                        sv.wtPhase += freq / sr;                          // 0..1 per cycle
                        if (sv.wtPhase >= 1.0) sv.wtPhase -= std::floor(sv.wtPhase);
                        sig = wavetableSample(c.waveTable, c.wavePos, (float) sv.wtPhase) * env;
                        break; }
                    case SrcModal: {
                        // Strike: feed ONE impulse into the resonator bank (a bank of 2-pole resonators =
                        // decaying sines). Each mode rings + decays on its own from there.
                        const bool strike = ! sv.modalInit;
                        if (strike) {
                            // Re-pitch the bank for THIS voice (per-step + channel pitch = vPitchMul), once at the
                            // strike. Each mode's angle w scales with frequency; reconstruct r/w0/g from the per-block
                            // coeffs (a1=2r*cos w0, a2=r^2, gain=g*sin w0) and rebuild at w = w0 * vPitchMul.
                            const double pm = vPitchMul;
                            sv.modalNV = c.modalN;        // this voice's bank size (immune to a mid-ring Material change)
                            for (int m = 0; m < c.modalN; ++m) {
                                sv.modalY1[m] = 0.0f; sv.modalY2[m] = 0.0f;
                                const float a2 = c.modalA2[m];
                                const float r  = std::sqrt(juce::jmax(0.0f, a2));
                                const float c0 = (r > 1.0e-6f) ? juce::jlimit(-1.0f, 1.0f, c.modalA1[m] / (2.0f * r)) : 1.0f;
                                const float w0 = std::acos(c0);
                                const float s0 = std::sin(w0);
                                const float g  = (s0 > 1.0e-6f) ? c.modalGain[m] / s0 : 0.0f;
                                const double w = (double) w0 * pm;
                                if (w >= kPi * 0.98 || w <= 0.0) { sv.modalA1[m] = 0.0f; sv.modalA2[m] = 0.0f; sv.modalGain[m] = 0.0f; }  // above Nyquist -> mute
                                else { sv.modalA1[m] = 2.0f * r * (float) std::cos(w); sv.modalA2[m] = a2; sv.modalGain[m] = g * (float) std::sin(w); }
                            }
                        }
                        float out = 0.0f;
                        for (int m = 0; m < sv.modalNV; ++m) {
                            const float x = strike ? sv.modalGain[m] : 0.0f;   // impulse only on the first sample
                            const float y = sv.modalA1[m] * sv.modalY1[m] - sv.modalA2[m] * sv.modalY2[m] + x;
                            sv.modalY2[m] = sv.modalY1[m]; sv.modalY1[m] = y;
                            out += y;
                        }
                        sv.modalInit = true;
                        // STRIKE (attack ramp) = a soft/swelled onset (the modes self-decay = the RING). Gated so
                        // atk<=20ms (every factory Modal sound, default 0.003) keeps env=1 = instant = bit-identical.
                        env = (c.atk > 0.005f) ? juce::jmin(1.0f, (float) t / juce::jmax(1.0f, c.atk * (float) sr)) : 1.0f;
                        if (! std::isfinite(out)) { for (int m = 0; m < sv.modalNV; ++m) { sv.modalY1[m] = sv.modalY2[m] = 0.0f; } out = 0.0f; }  // self-heal a runaway
                        sig = juce::jlimit(-4.0f, 4.0f, out) * 0.4f * env;   // bound the bank sum to a musical level, then the Strike ramp
                        break; }
                    default: break;
                }

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
                if (c.fxDrive > 0.0001f && c.fxDriveType != DriveOff) {
                    if (stereo) { sL = driveSample(sL, c.fxDriveType, c.fxDrive); sR = driveSample(sR, c.fxDriveType, c.fxDrive); }
                    else          sig = driveSample(sig, c.fxDriveType, c.fxDrive);
                }

                vEnv = juce::jmax(vEnv, env);
                const float wEff = c.weight * driftMod[s] * (s == domSlot ? 1.0f : bGate);
                const float cL = (stereo ? sL : sig) * wEff * c.gL;
                const float cR = (stereo ? sR : sig) * wEff * c.gR;
                mixL += cL; mixR += cR;
                // Per-slot reverb/delay SEND: this slot's signal x its own send amount (pre channel-gain; gain added later).
                if (c.fxRevSend > 0.0001f) { fxRevL[i] += cL * v.velGain * vPanL * c.fxRevSend; fxRevR[i] += cR * v.velGain * vPanR * c.fxRevSend; }
                if (c.fxDelSend > 0.0001f) { fxDelL[i] += cL * v.velGain * vPanL * c.fxDelSend; fxDelR[i] += cR * v.velGain * vPanR * c.fxDelSend; }
                // === PER-SLOT EQ (begin) - capture THIS slot's mono output for its spectrum ===
                if (s == tapSlot && i < (int) std::size(analysisBuf)) {
                    const float mono = stereo ? 0.5f * (wEff * sL * c.gL + wEff * sR * c.gR)
                                              : 0.5f * wEff * sig * (c.gL + c.gR);
                    analysisBuf[i] += mono * v.velGain;
                }
                // === PER-SLOT EQ (end) ===
            }

            outL[i] += mixL * v.velGain * vPanL;
            outR[i] += mixR * v.velGain * vPanR;
            maxEnvLevel = juce::jmax(maxEnvLevel, vEnv * v.velGain);

            if (++v.voiceSamples >= voiceEnd) { finished = true; break; }
        }

        if (finished) v.playHead = -1.0;
    }

    // ---- PUNCH (transient shaper) + GLUE (gentle compression) on the mix - both REMOVED (-> 0). -----
    const float punchAmt = 0.0f;
    const float glueAmt  = 0.0f;
    if (punchAmt > 0.0001f || glueAmt > 0.0001f)
    {
        const float fastC = 1.0f - std::exp(-1.0f / (0.0012f * (float) sr)); // ~1.2 ms
        const float slowC = 1.0f - std::exp(-1.0f / (0.035f  * (float) sr)); // ~35 ms
        const float glAtt = 1.0f - std::exp(-1.0f / (0.003f  * (float) sr)); // glue attack
        const float glRel = 1.0f - std::exp(-1.0f / (0.090f  * (float) sr)); // glue release (pumps)
        const float glThr = 0.12f, glMakeup = 1.0f + glueAmt * 1.3f;          // hard squash + big make-up
        for (int i = 0; i < numSamples; ++i)
        {
            const float a = 0.5f * (std::fabs(outL[i]) + std::fabs(outR[i]));
            float g = 1.0f;
            if (punchAmt > 0.0001f)   // strongly boost the attack transient
            {
                punchFast += fastC * (a - punchFast);
                punchSlow += slowC * (a - punchSlow);
                const float trans = juce::jmax(0.0f, punchFast - punchSlow);
                g *= 1.0f + punchAmt * 14.0f * trans;
            }
            if (glueAmt > 0.0001f)    // hard ~5:1 compression above threshold + make-up (pumps)
            {
                glueEnv += (a > glueEnv ? glAtt : glRel) * (a - glueEnv);
                if (glueEnv > glThr)
                {
                    const float gr = std::pow(glueEnv / glThr, -0.8f); // ~5:1
                    g *= (1.0f - glueAmt) + glueAmt * gr;
                }
                g *= glMakeup;
            }
            outL[i] *= g; outR[i] *= g;
        }
    }

    // Rebuild EQ coefficients on the audio thread if a knob changed
    if (dspDirty.exchange(false))
        updateDSP();

    // Multimode filter is updated every block so the envelope can sweep cutoff
    updateFilter(maxEnvLevel, 1.0);

    applyEQ(renderBuf, numSamples);

    // Feed the analyser if this channel is being inspected: the final mix (All), or - when a
    // slot is selected on the EQ - just THAT slot's signal (captured pre-mix above).
    if (analysisTap != nullptr)
    {
        if (tapSlot >= 0)
        {
            const int nn = juce::jmin(numSamples, (int) std::size(analysisBuf));
            for (int i = 0; i < nn; ++i) analysisTap->push(analysisBuf[i]);
        }
        else
        {
            const auto* l = renderBuf.getReadPointer(0);
            const auto* r = renderBuf.getReadPointer(1);
            for (int i = 0; i < numSamples; ++i) analysisTap->push(0.5f * (l[i] + r[i]));
        }
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
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = buf.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i) d[i] = applyDrive(d[i]);
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
    const float g  = 1.0f + driveAmount * 24.0f;
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

    // rough make-up so louder drive doesn't just get louder
    return y * (1.0f / (1.0f + driveAmount * 3.0f));
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
