#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cmath>

//==============================================================================
// 8-line modulated Feedback Delay Network reverb - smoother + less metallic than Freeverb.
// The feedback uses a lossless Householder matrix (energy-preserving) multiplied by a decay gain
// that is always < 1, so it is unconditionally stable (cannot self-oscillate). Each feedback path
// has a one-pole damping low-pass, and the delay lengths are slowly modulated for a lush tail.
class FDNReverb
{
public:
    void prepare(double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        static const int primes[N] = { 1129, 1367, 1693, 1979, 2273, 2647, 2957, 3253 };
        for (int i = 0; i < N; ++i)
        {
            base[i] = juce::jmax(8, (int) (primes[i] * sr / 48000.0));
            buf[i].assign((size_t) (base[i] * 2 + 256), 0.0f);
            widx[i] = 0; lp[i] = 0.0f; lfo[i] = (float) i * 0.37f;
        }
        shBuf.assign((size_t) juce::jmax(1024, (int) (4096.0 * sr / 48000.0)), 0.0f);   // shimmer ring
        shW = 0; shPh = 0.0;
        // [2026-07-13 21:05] INPUT DIFFUSION (Dattorro-style): 2 series allpasses per side smear the
        // input before it enters the tank = instant echo density instead of a fluttery early field.
        static const int apP[4] = { 142, 379, 107, 277 };   // L: 0,1  R: 2,3 (mutually prime-ish)
        for (int i = 0; i < 4; ++i)
        { apLen[i] = juce::jmax(8, (int) (apP[i] * sr / 48000.0));
          apBuf[i].assign((size_t) apLen[i], 0.0f); apW[i] = 0; }
        for (int i = 0; i < N; ++i) loCut[i] = 0.0f;
    }

    void reset()
    {
        for (int i = 0; i < N; ++i) { std::fill(buf[i].begin(), buf[i].end(), 0.0f); lp[i] = 0.0f; loCut[i] = 0.0f; }
        for (auto& b : apBuf) std::fill(b.begin(), b.end(), 0.0f);
        std::fill(shBuf.begin(), shBuf.end(), 0.0f);
    }

    // [2026-07-15 02:30] TAIL-LENGTH MATHS, shared by the UI's "~2.1 s" read-out and the synced
    // Decay inverse (Decay-in-bars). Broadband estimate: the tank's mean loop time x how many
    // passes until -60 dB at the mode-adjusted feedback gain. Damping makes highs die sooner, so
    // real tails read slightly shorter - always display with a "~" (honest estimate, not a lie).
    static float modeGain(float decay, int mode)
    {
        float g = 0.5f + 0.43f * juce::jlimit(0.0f, 1.0f, decay);
        if (mode == 0) g *= 0.90f;
        else if (mode == 2) g = juce::jmin(0.95f, g * 1.02f);
        else if (mode == 3) g = juce::jmin(0.95f, g * 1.04f);
        return g;
    }
    static float modeScale(float roomSize, int mode)
    {
        float s = 0.6f + 0.9f * juce::jlimit(0.0f, 1.0f, roomSize);
        if (mode == 0) s *= 0.32f; else if (mode == 2) s *= 0.7f;
        return s;
    }
    static float meanLoopSec(float roomSize, int mode, double sampleRate)
    {   // mean of the 8 base primes (48 kHz domain) = 2162 samples; scaled like the process() read
        return 2162.25f / 48000.0f * modeScale(roomSize, mode) * (float) (sampleRate > 0 ? 1.0 : 1.0);
    }   // (base[] already tracks sr, so the SECONDS figure is sr-independent)
    static float estimateT60(float decay, float roomSize, int mode)
    {
        const float g = juce::jlimit(0.01f, 0.98f, modeGain(decay, mode));
        return meanLoopSec(roomSize, mode, 48000.0) * (-3.0f / std::log10(g));   // -60 dB = 3 decades
    }
    static float decayForT60(float t60, float roomSize, int mode)
    {   // inverse of estimateT60: the DECAY value whose tail lasts ~t60 seconds in this room/mode
        const float loop = meanLoopSec(roomSize, mode, 48000.0);
        float g = std::pow(10.0f, -3.0f * loop / juce::jmax(0.05f, t60));
        if (mode == 0) g /= 0.90f;                       // undo the mode multiplier (caps make the
        else if (mode == 2) g /= 1.02f;                  //  inverse approximate near the top - fine,
        else if (mode == 3) g /= 1.04f;                  //  the whole figure is a ~)
        return juce::jlimit(0.0f, 1.0f, (g - 0.5f) / 0.43f);
    }

    // Processes `n` samples IN-PLACE allowed (in==out). roomSize/decay/damp/width are 0..1.
    // mode: 0 Room / 1 Hall (= the ORIGINAL voicing, bit-identical) / 2 Plate / 3 Shimmer
    // (Hall + an octave-up pitch shifter in the feedback = the glowing ambient halo).
    void process(const float* inL, const float* inR, float* outL, float* outR, int n,
                 float roomSize, float decay, float damp, float width, int mode = 1) noexcept
    {
        const float twoPi = juce::MathConstants<float>::twoPi;
        float g     = 0.5f + 0.43f * juce::jlimit(0.0f, 1.0f, decay);      // 0.5..0.93 feedback (extra stability margin)
        float scale = 0.6f + 0.9f  * juce::jlimit(0.0f, 1.0f, roomSize);   // delay-length scale
        float cutHz = 1200.0f + (1.0f - juce::jlimit(0.0f, 1.0f, damp)) * 8000.0f;  // damping LP
        float modD  = 1.5f;                                                // delay-line modulation depth
        switch (mode)
        {   // re-voicings of the same safe network; Hall (1) = the untouched original numbers
            case 0: scale *= 0.32f; g *= 0.90f; cutHz *= 0.55f; modD = 0.6f; break;               // Room: small, boxy, dark
            case 2: scale *= 0.7f;  g = juce::jmin(0.95f, g * 1.02f); modD = 0.8f;                // Plate: dense + bright
                    cutHz = 2400.0f + (1.0f - juce::jlimit(0.0f, 1.0f, damp)) * 9000.0f; break;
            case 3: cutHz = juce::jmax(cutHz * 1.4f, 4500.0f);          // Shimmer: open the damping so the
                    g = juce::jmin(0.95f, g * 1.04f); break;            // octave-up passes survive + bloom
            default: break;                                                                        // Hall body
        }
        const float dampC = 1.0f - std::exp(-twoPi * cutHz / (float) sr);
        const float w     = juce::jlimit(0.0f, 1.0f, width);
        const bool  shimmer = (mode == 3) && ! shBuf.empty();
        const int   shN   = (int) shBuf.size();
        const float shWin = (float) juce::jmax(256, shN / 2);              // pitch-shift grain window
        static const float sgn[N] = { 1,-1,1,-1,1,-1,1,-1 };

        if (buf[0].empty()) { for (int s = 0; s < n; ++s) { outL[s] = 0.0f; outR[s] = 0.0f; } return; }  // not prepared

        bool bad = false;   // any non-finite this block -> reset the whole network at the end (self-heal)
        // loop LOW-CUT tracker (~55 Hz): stops sub energy accumulating in the tail = tighter, less
        // muddy long decays (the frequency-dependent-decay idea, minimal form). [2026-07-13 21:05]
        const float loK = 1.0f - std::exp(-twoPi * 55.0f / (float) sr);
        for (int s = 0; s < n; ++s)
        {
            // [2026-07-13 21:05] TRUE STEREO INPUT: L/R are DIFFUSED separately (2 allpasses each)
            // and feed alternate lines - the old path collapsed the input to mono first.
            float xin[2] = { std::isfinite(inL[s]) ? inL[s] : 0.0f,
                             std::isfinite(inR[s]) ? inR[s] : 0.0f };
            for (int side = 0; side < 2; ++side)
                for (int a = side * 2; a < side * 2 + 2; ++a)
                {   const float z = apBuf[a][(size_t) apW[a]];
                    const float y = z - 0.68f * xin[side];                     // allpass: unity magnitude
                    const float wv = xin[side] + 0.68f * y;
                    apBuf[a][(size_t) apW[a]] = std::isfinite(wv) ? juce::jlimit(-8.0f, 8.0f, wv) : 0.0f;
                    apW[a] = (apW[a] + 1) % apLen[a];
                    xin[side] = y;
                }
            float dout[N];
            for (int i = 0; i < N; ++i)
            {
                lfo[i] += twoPi * (0.25f + 0.05f * (float) i) / (float) sr;   // slow, per-line
                if (lfo[i] > twoPi) lfo[i] -= twoPi;
                const int sz = (int) buf[i].size();
                float len = (float) base[i] * scale + 1.5f * std::sin(lfo[i]);   // gentle modulation
                len = juce::jlimit(2.0f, (float) (sz - 2), len);
                float rp = (float) widx[i] - len; while (rp < 0.0f) rp += (float) sz;
                const int ri = juce::jlimit(0, sz - 1, (int) rp); const float fr = rp - (float) ri;
                const int ri2 = (ri + 1) % sz;
                dout[i] = buf[i][(size_t) ri] + fr * (buf[i][(size_t) ri2] - buf[i][(size_t) ri]);
            }
            float sum = 0.0f; for (int i = 0; i < N; ++i) sum += dout[i];
            float hh = (2.0f / (float) N) * sum;           // Householder reflection
            if (shimmer)
            {   // SHIMMER: blend an OCTAVE-UP copy of the feedback into itself (2-tap crossfaded
                // varispeed read = classic cheap +12 shifter). Bounded by the same ±8 state clamp.
                shBuf[(size_t) shW] = juce::jlimit(-8.0f, 8.0f, std::isfinite(hh) ? hh : 0.0f);
                shPh += 1.0 / (double) shWin;                              // rate 2 => offset shrinks 1/sample
                if (shPh >= 1.0) shPh -= 1.0;
                float shOut = 0.0f;
                for (int t = 0; t < 2; ++t)
                {
                    const double fr2 = shPh + (t == 0 ? 0.0 : 0.5);
                    const double d2  = (fr2 - std::floor(fr2)) * (double) shWin;
                    int rp2 = shW - (int) d2; while (rp2 < 0) rp2 += shN;
                    const float win = std::sin((float) (fr2 - std::floor(fr2)) * juce::MathConstants<float>::pi);
                    shOut += shBuf[(size_t) rp2] * win * win;
                }
                shW = (shW + 1) % shN;
                // ENERGY-NEUTRAL: crossfade toward the shifted copy (never add on top - adding pushed
                // the loop past unity, the +-8 clamps pegged, and the huge wet made the master
                // soft-clip crush the whole mix = "shimmer breaks everything").
                hh += 0.6f * (shOut - hh);
            }
            for (int i = 0; i < N; ++i)
            {
                float fb = (hh - dout[i]) * g;
                lp[i] += dampC * (fb - lp[i]);
                if (! std::isfinite(lp[i])) { lp[i] = 0.0f; bad = true; }   // guard the damping state
                fb = lp[i];
                loCut[i] += loK * (fb - loCut[i]);                          // loop LOW-CUT (~55 Hz HP)
                if (! std::isfinite(loCut[i])) { loCut[i] = 0.0f; bad = true; }
                fb -= loCut[i];
                const int sz = (int) buf[i].size();
                // HARD SAFETY: bound the stored state + kill any NaN/Inf so the network can NEVER run away.
                // Room mode: SPARSE input (only half the lines are fed, with the MID) = boxy, fluttery
                // early field - a real character change, not just "small hall" (Size does small).
                float inFeed = (i & 1) ? xin[1] : xin[0];                   // stereo: L = even, R = odd lines
                float inG = 0.5f;
                if (mode == 0) { if (i & 1) inG = 0.0f; inFeed = 0.5f * (xin[0] + xin[1]); }
                const float v = inFeed * sgn[i] * inG + fb;
                buf[i][(size_t) widx[i]] = std::isfinite(v) ? juce::jlimit(-8.0f, 8.0f, v) : 0.0f;
                widx[i] = (widx[i] + 1) % sz;
            }
            float wl = (dout[0] + dout[2] + dout[4] + dout[6]) * 0.35f;
            float wr = (dout[1] + dout[3] + dout[5] + dout[7]) * 0.35f;
            const float mid = 0.5f * (wl + wr);
            float oL = mid + w * (wl - mid);   // width: 1 = full stereo, 0 = mono
            float oR = mid + w * (wr - mid);
            if (! std::isfinite(oL)) { oL = 0.0f; bad = true; }
            if (! std::isfinite(oR)) { oR = 0.0f; bad = true; }
            outL[s] = oL; outR[s] = oR;
        }
        if (bad) reset();   // a non-finite slipped in somewhere -> flush all delay lines + states clean
    }

private:
    static constexpr int N = 8;
    std::vector<float> buf[N];
    int   widx[N] = {};
    int   base[N] = {};
    float lp[N]   = {};
    float lfo[N]  = {};
    std::vector<float> apBuf[4];   // [2026-07-13 21:05] input diffusion allpasses (2 per side)
    int   apW[4] = {}, apLen[4] = { 8, 8, 8, 8 };
    float loCut[N] = {};           // loop low-cut trackers (~55 Hz)
    std::vector<float> shBuf;   // SHIMMER: mono feedback ring for the octave-up pitch shifter
    int    shW  = 0;
    double shPh = 0.0;
    double sr = 44100.0;
};
