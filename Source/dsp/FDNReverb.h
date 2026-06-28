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
    }

    void reset()
    {
        for (int i = 0; i < N; ++i) { std::fill(buf[i].begin(), buf[i].end(), 0.0f); lp[i] = 0.0f; }
    }

    // Processes `n` samples IN-PLACE allowed (in==out). roomSize/decay/damp/width are 0..1.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n,
                 float roomSize, float decay, float damp, float width) noexcept
    {
        const float twoPi = juce::MathConstants<float>::twoPi;
        const float g     = 0.5f + 0.43f * juce::jlimit(0.0f, 1.0f, decay);      // 0.5..0.93 feedback (extra stability margin)
        const float scale = 0.6f + 0.9f  * juce::jlimit(0.0f, 1.0f, roomSize);   // delay-length scale
        const float cutHz = 1200.0f + (1.0f - juce::jlimit(0.0f, 1.0f, damp)) * 8000.0f;  // damping LP
        const float dampC = 1.0f - std::exp(-twoPi * cutHz / (float) sr);
        const float w     = juce::jlimit(0.0f, 1.0f, width);
        static const float sgn[N] = { 1,-1,1,-1,1,-1,1,-1 };

        if (buf[0].empty()) { for (int s = 0; s < n; ++s) { outL[s] = 0.0f; outR[s] = 0.0f; } return; }  // not prepared

        bool bad = false;   // any non-finite this block -> reset the whole network at the end (self-heal)
        for (int s = 0; s < n; ++s)
        {
            const float inMix = 0.5f * (inL[s] + inR[s]);
            const float in = std::isfinite(inMix) ? inMix : 0.0f;   // never feed a NaN into the network
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
            const float hh = (2.0f / (float) N) * sum;     // Householder reflection
            for (int i = 0; i < N; ++i)
            {
                float fb = (hh - dout[i]) * g;
                lp[i] += dampC * (fb - lp[i]);
                if (! std::isfinite(lp[i])) { lp[i] = 0.0f; bad = true; }   // guard the damping state
                fb = lp[i];
                const int sz = (int) buf[i].size();
                // HARD SAFETY: bound the stored state + kill any NaN/Inf so the network can NEVER run away.
                const float v = in * sgn[i] * 0.5f + fb;
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
    double sr = 44100.0;
};
