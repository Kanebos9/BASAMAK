// REAL-TUNER accuracy lock: basamakDetectPitch (the editor's tuner) must read known tones to
// within a couple of cents, like any third-party tuner (ReaTune/GTune - user comparison).
#include "Sequencer.h"
#include <cstdio>
#include <cmath>

static double centsErr(double f, double ref) { return 1200.0 * std::log2(f / ref); }

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double fs = 24000.0;   // matches TunerTap::DECIM 4 at a 96k engine rate
    float x[2048];
    auto testTone = [&](double hz, const char* name)
    {
        double ph = 0.0;
        for (int i = 0; i < 2048; ++i)   // saw with a few harmonics (guitar-ish, not a lab sine)
        {
            ph += hz / fs;
            const double t = ph - std::floor(ph);
            x[i] = (float) (0.6 * (2.0 * t - 1.0) + 0.2 * std::sin(4.0 * juce::MathConstants<double>::pi * ph));
        }
        const double f = basamakDetectPitch(x, 2048, fs);
        const double err = f > 0 ? centsErr(f, hz) : 999.0;
        printf("[tuner] %-14s expect %7.2f Hz got %7.2f Hz  err %+5.1f cents -> %s\n",
               name, hz, f, err, CHK(std::abs(err) < 3.0) ? "OK" : "FAIL");
    };
    testTone(110.0,   "A2 (bass)");
    testTone(261.626, "C4 (middle C)");
    testTone(261.626 * std::pow(2.0, 0.30 / 12.0), "C4 +30 cents");
    testTone(440.0,   "A4");
    testTone(65.406,  "C2 (low bass)");
    // silence must report NO pitch (the strip shows "-")
    for (auto& v : x) v = 0.0f;
    printf("[tuner] silence -> %s\n", CHK(basamakDetectPitch(x, 2048, fs) <= 0.0) ? "OK (no pitch)" : "FAIL");
    return fails;
}
