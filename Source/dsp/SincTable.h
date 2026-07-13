#pragma once
// ================================================================================================
// SincTable.h - polyphase windowed-sinc SAMPLE INTERPOLATION for BASAMAK [written 2026-07-13
// 20:45, extracted to this module 2026-07-13 22:10].
//
// FOR NEW READERS: reading a sample buffer at a fractional position needs interpolation; cheap
// methods (linear, Hermite) leave audible "images" when the sample is pitched. This table holds
// 2048 pre-computed 8-tap Kaiser-windowed sinc kernels (one per fractional phase, each row
// normalised to sum exactly 1). The reader picks the row nearest its fraction and does 8 MACs.
// Built once at plugin load (~64 KB); exact passthrough at integer positions. Consumed by the
// SrcSample read in DrumChannel.cpp's render.
// ================================================================================================
#include <JuceHeader.h>

struct SincTable
{
    static constexpr int TAPS = 8, HALF = 4, PHASES = 2048;
    float t[PHASES + 1][TAPS];                 // +1 guard row = fr exactly 1.0
    SincTable()
    {
        auto besselI0 = [](double x) { double s = 1.0, term = 1.0; const double h = 0.5 * x;
                                       for (int k = 1; k < 64; ++k) { term *= (h / k) * (h / k); s += term;
                                                                      if (term < 1.0e-12 * s) break; } return s; };
        const double beta = 8.0, i0b = besselI0(beta);
        for (int p = 0; p <= PHASES; ++p)
        {
            const double fr = (double) p / (double) PHASES;
            double sum = 0.0;
            for (int j = 0; j < TAPS; ++j)
            {
                const double x = (double)(j - (HALF - 1)) - fr;              // tap offsets -3..+4
                const double s = x == 0.0 ? 1.0 : std::sin(juce::MathConstants<double>::pi * x)
                                                  / (juce::MathConstants<double>::pi * x);
                const double r = x / (double) HALF;                          // window argument -1..1
                const double w = std::abs(r) <= 1.0 ? besselI0(beta * std::sqrt(1.0 - r * r)) / i0b : 0.0;
                t[p][j] = (float)(s * w); sum += s * w;
            }
            if (sum > 1.0e-9) for (int j = 0; j < TAPS; ++j) t[p][j] = (float)((double) t[p][j] / sum);
        }
    }
};
static const SincTable gSincTable;
