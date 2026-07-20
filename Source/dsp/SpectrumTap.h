#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstring>

//==============================================================================
// Lock-free single-producer/single-consumer hand-off of a block of audio
// samples from the audio thread (producer) to the editor (consumer) for FFT.
//
// Capture is TRIGGER-ALIGNED: the audio thread calls arm() the moment the
// analysed channel fires a step, which restarts the window at the hit's attack.
// It then grabs exactly fftSize samples from that point and raises `ready`. The
// editor copies `data`, lowers `ready`, then does the FFT on its own copy.
// Because every hit of the same sound is captured from the same point, the
// resulting spectrum is identical each time instead of catching the hit at a
// random phase (which a free-running window does).
//==============================================================================
struct SpectrumTap
{
    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1 << fftOrder; // 1024

    float fifo[fftSize] = {};
    int   fifoIndex = 0;
    bool  capturing = false;   // audio thread only
    float data[fftSize] = {};
    std::atomic<bool> ready { false };

    // Audio thread: start a fresh ONE-SHOT capture aligned to this moment. Called at EVERY step
    // boundary (Sequencer) AND periodically while stopped (processor), so the window is step-aligned
    // (consistent spectrum) but still refreshes for sustained / long-attack / still-ringing sounds.
    void arm() noexcept { fifoIndex = 0; capturing = true; }

    // Audio thread, once per sample. Ignored unless a capture is in progress.
    void push(float sample) noexcept
    {
        if (! capturing) return;

        fifo[fifoIndex++] = sample;

        if (fifoIndex >= fftSize)
        {
            capturing = false;
            if (! ready.load(std::memory_order_acquire))
            {
                std::memcpy(data, fifo, sizeof(fifo));
                ready.store(true, std::memory_order_release);
            }
        }
    }
};

//==============================================================================
// TUNER TAP: a CONTINUOUS decimated ring of the analysed channel's mono signal for REAL pitch
// detection (the LIVE TUNE strip). The audio thread pushes every sample; every DECIM-th one
// lands in the ring. The editor snapshots the ring ~10x/s and runs NSDF autocorrelation on it -
// so the tuner measures the actual audio, like ReaTune/GTune (user demand), instead of showing
// knob arithmetic. Ring torn-read races only smear one sample at the seam - irrelevant to NSDF.
// NSDF (McLeod-style) pitch detection with parabolic sub-lag interpolation - cents-accurate
// (integer lags alone quantise to ~40 cents at high pitches). Returns Hz, or <= 0 = no pitch.
// ONE implementation, used by the editor's tuner AND the offline TunerTest.
static inline double basamakDetectPitch(const float* x, int W, double fs) noexcept
{
    double rms = 0.0; for (int i = 0; i < W; ++i) rms += (double) x[i] * x[i];
    if (std::sqrt(rms / W) < 1.0e-3) return -1.0;
    const int lagMin = juce::jmax(2, (int) (fs / 1500.0));
    const int lagMax = juce::jmin(W - 2, (int) (fs / 28.0));
    if (lagMax <= lagMin + 2) return -1.0;
    static thread_local std::vector<double> nsdf;
    nsdf.assign((size_t) lagMax + 1, 0.0);
    for (int L = lagMin; L <= lagMax; ++L)
    {
        double ac = 0.0, m0 = 0.0;
        const int n2 = W - L;
        for (int i = 0; i < n2; ++i)
        { ac += (double) x[i] * x[i + L]; m0 += (double) x[i] * x[i] + (double) x[i + L] * x[i + L]; }
        nsdf[(size_t) L] = m0 > 1.0e-12 ? 2.0 * ac / m0 : 0.0;
    }
    // [2026-07-19] TRUE McLeod (MPM) peak picking. The old rule - "first local peak above a
    // FIXED 0.62" - was right for pure tones (TunerTest passed) but wrong for REAL strings: a
    // plucked bass/guitar's strong 2nd harmonic puts a >0.62 NSDF peak at the HALF period, and
    // first-above-fixed picked it = read an octave (or a 5th) off "sometimes" (user vs GTune).
    // MPM: collect ALL local maxima (parabolic-refined), find the GLOBAL best, then take the
    // FIRST peak within 90% of it - the true period's peak is taller than any harmonic's.
    static thread_local std::vector<double> pkLag, pkVal;
    pkLag.clear(); pkVal.clear();
    for (int L = lagMin + 1; L < lagMax; ++L)
    {
        const double a = nsdf[(size_t) L - 1], b = nsdf[(size_t) L], c = nsdf[(size_t) L + 1];
        if (b < 0.30 || b < a || b <= c) continue;                  // 0.30 abs floor = noise reject
        const double den   = a - 2.0 * b + c;
        const double delta = std::abs(den) > 1.0e-12 ? juce::jlimit(-0.5, 0.5, 0.5 * (a - c) / den) : 0.0;
        pkLag.push_back((double) L + delta);
        pkVal.push_back(b - 0.25 * (a - c) * delta);                // refined peak height
    }
    if (pkLag.empty()) return -1.0;
    double best = 0.0;
    for (double v : pkVal) best = juce::jmax(best, v);
    if (best < 0.45) return -1.0;                                   // nothing convincingly periodic
    for (size_t k = 0; k < pkLag.size(); ++k)
        if (pkVal[k] >= 0.90 * best) return fs / pkLag[k];
    return -1.0;
}

// [2026-07-20] SUSTAIN pitch = the MEDIAN of several mid-body windows. A single loudest-window
// measurement lands on the ATTACK, and sung/plucked onsets run audibly SHARP of the sustain
// (the Da Loo Ne incident: the wizard named a vocal take ~2.5 st above the note it settles on).
// Windows probe 30..78% of the length; near-silent windows (< 10% of the strongest) are
// skipped; the median throws away the outliers a scoop or wobble leaves behind.
static inline double basamakMeasureSustainPitch(const float* x, int N, double fs) noexcept
{
    const int W = (N / 2 < 8192) ? N / 2 : 8192;
    if (W < 2048 || fs <= 0) return 0.0;
    constexpr double frac[5] = { 0.30, 0.42, 0.54, 0.66, 0.78 };
    double eng[5]; double eMax = 0.0;
    int offs[5];
    for (int k = 0; k < 5; ++k)
    {
        offs[k] = (int)(frac[k] * (double)(N - W));
        double e2 = 0.0;
        for (int i = 0; i < W; i += 4) e2 += (double) x[offs[k] + i] * x[offs[k] + i];
        eng[k] = e2; if (e2 > eMax) eMax = e2;
    }
    double cand[5]; int nc = 0;
    for (int k = 0; k < 5; ++k)
    {
        if (eng[k] < eMax * 0.1) continue;
        const double hz = basamakDetectPitch(x + offs[k], W, fs);
        if (hz > 0.0) cand[nc++] = hz;
    }
    if (nc == 0) return 0.0;
    for (int i = 1; i < nc; ++i)             // insertion sort - 5 elements, no <algorithm> dep
        for (int j = i; j > 0 && cand[j] < cand[j - 1]; --j)
        { const double t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t; }
    return cand[nc / 2];
}

struct TunerTap
{
    static constexpr int DECIM = 4;      // engine rate / 4 (e.g. 96k -> 24k): high notes need the lag
                                         // resolution (DECIM 8 read A4 ~6 cents sharp - TunerTest)
    static constexpr int N     = 4096;   // ~170 ms at 24k: still >4 periods of a 30 Hz bass

    float ring[N] = {};
    std::atomic<int> widx { 0 };
    int dec = 0;

    void push(float x) noexcept
    {
        if (++dec < DECIM) return;
        dec = 0;
        const int w = widx.load(std::memory_order_relaxed);
        ring[w] = x;
        widx.store((w + 1) % N, std::memory_order_release);
    }
};
