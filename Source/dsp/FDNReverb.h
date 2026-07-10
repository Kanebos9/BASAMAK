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
    }

    void reset()
    {
        for (int i = 0; i < N; ++i) { std::fill(buf[i].begin(), buf[i].end(), 0.0f); lp[i] = 0.0f; }
        std::fill(shBuf.begin(), shBuf.end(), 0.0f);
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
            case 0: scale *= 0.45f; g *= 0.92f; cutHz *= 0.7f;  modD = 1.0f; break;               // Room: small, tight, darker
            case 2: scale *= 0.7f;  g = juce::jmin(0.95f, g * 1.02f); modD = 0.8f;                // Plate: dense + bright
                    cutHz = 2400.0f + (1.0f - juce::jlimit(0.0f, 1.0f, damp)) * 9000.0f; break;
            default: break;                                                                        // Hall / Shimmer body
        }
        const float dampC = 1.0f - std::exp(-twoPi * cutHz / (float) sr);
        const float w     = juce::jlimit(0.0f, 1.0f, width);
        const bool  shimmer = (mode == 3) && ! shBuf.empty();
        const int   shN   = (int) shBuf.size();
        const float shWin = (float) juce::jmax(256, shN / 2);              // pitch-shift grain window
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
                hh += 0.4f * shOut;
            }
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
    std::vector<float> shBuf;   // SHIMMER: mono feedback ring for the octave-up pitch shifter
    int    shW  = 0;
    double shPh = 0.0;
    double sr = 44100.0;
};
