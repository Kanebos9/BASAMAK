#pragma once
// ================================================================================================
// Adaa.h - ANTIDERIVATIVE ANTI-ALIASING shapers for BASAMAK [written 2026-07-13 20:20,
// extracted to this module 2026-07-13 22:10].
//
// FOR NEW READERS: a naked waveshaper (tanh, clip, fold...) sprays harmonics past Nyquist that
// fold back as ALIASING. First-order ADAA replaces f(x) with the slope of its antiderivative,
//     y = (F(x) - F(x_prev)) / (x - x_prev),
// which integrates that alias energy out (~36 dB measured) for ONE float of state per stream and
// zero latency - the modern alternative to oversampling shaper stages. Every function keeps its
// previous input in a caller-owned float; the 1e9 sentinel means "unprimed" (first call after a
// hit evaluates directly). Consumed by DrumChannel.cpp's render (drive insert, Bass Amp stages,
// oscillator WARP fold). Bitcrush is deliberately NOT here - aliasing IS its sound.
// ================================================================================================
#include <JuceHeader.h>
#include "DrumChannel.h"

static constexpr float kAdaaUnprimed = 1.0e9f;
static inline float lnCoshF(float x)
{ const float ax = std::abs(x); return ax > 12.0f ? ax - 0.6931472f : std::log(std::cosh(ax)); }
// ADAA'd tanh core (shared by SoftClip / Tube / the Bass Amp stages).
static inline float adaaTanh(float u, float& u1)
{
    if (u1 >= kAdaaUnprimed * 0.5f) { u1 = u; return std::tanh(u); }
    const float d = u - u1;
    const float y = std::abs(d) > 1.0e-4f ? (lnCoshF(u) - lnCoshF(u1)) / d : std::tanh(0.5f * (u + u1));
    u1 = u; return y;
}
// Ideal periodic triangle fold (period 4, unity slope) + its primitive - the Foldback shaper.
// (The old loop capped at 4 reflections; the periodic form IS the proper wavefolder - at extreme
// drive the tone is slightly more correct than before, disclosed.)
static inline float triFold(float v)
{ float m = std::fmod(v - 1.0f, 4.0f); if (m < 0.0f) m += 4.0f; return std::abs(m - 2.0f) - 1.0f; }
static inline float triFoldI(float v)   // primitive of the zero-mean triangle = itself periodic
{ float m = std::fmod(v - 1.0f, 4.0f); if (m < 0.0f) m += 4.0f;
  return m <= 2.0f ? m - 0.5f * m * m : 0.5f * m * m - 3.0f * m + 4.0f; }
// The FUZZ transfer (0.6*t + 0.8*|t| - 0.24, t = tanh(1.5u), clamped at +1 above u0) + primitive.
static inline float fuzzF(float u)
{ const float t = std::tanh(1.5f * u);
  return juce::jlimit(-1.0f, 1.0f, t * 0.6f + (std::abs(t) - 0.3f) * 0.8f); }
static inline float fuzzFI(float u)
{
    constexpr float u0 = 0.93445f, F0 = 0.49200f;   // clamp knee (f(u0) = 1) + primitive there
    if (u > u0) return F0 + (u - u0);
    const float lc = lnCoshF(1.5f * u);   // (1/k)*lnCosh(ku) terms with k = 1.5 folded into the factors
    return lc * (0.4f + 0.533333f * (u >= 0.0f ? 1.0f : -1.0f)) - 0.24f * u;
}
// The oscillator WARP fold: f = (1-w)x + w*sin(qx), q = (1+4w)*pi/2; F = (1-w)x^2/2 - (w/q)cos(qx).
// F depends on w EXPLICITLY (unlike the drive shapers, where the amount only scales the input), so
// with per-sample-modulated w both endpoints MUST use one midpoint w - mixing epochs puts a
// parameter error over a tiny denominator (measured: a 0.84 spike on a swept warp; test [16]).
static inline float warpFoldAdaa(float x, float w, float& x1, float& w1)
{
    if (x1 >= kAdaaUnprimed * 0.5f)
    { x1 = x; w1 = w; const float q0 = (1.0f + w * 4.0f) * 1.5707963f;
      return x + w * (std::sin(x * q0) - x); }
    const float wm = 0.5f * (w + w1);                       // ONE parameter epoch for both endpoints
    const float q  = (1.0f + wm * 4.0f) * 1.5707963f;
    const float d  = x - x1; float y;
    if (std::abs(d) > 1.0e-4f)
    { auto F = [&](float v) { return 0.5f * (1.0f - wm) * v * v - (wm / q) * std::cos(v * q); };
      y = (F(x) - F(x1)) / d; }
    else y = 0.5f * (x + x1) + wm * (std::sin(0.5f * (x + x1) * q) - 0.5f * (x + x1));
    x1 = x; w1 = w; return y;
}
// ADAA drive router: same gain taper / makeup / voicings as driveSample, aliasing integrated out.
static float driveAdaa(float x, int driveType, float driveAmount, float& u1)
{
    using DC = DrumChannel;
    if (driveType == DC::DriveExciter)
    {   // ADAA on the synthesized-harmonics part only (the dry passes untouched, as designed).
        auto h = [](float v) { const float t = std::tanh(v * 1.4f); return 1.15f * t * t + 0.75f * t * t * t; };
        if (u1 >= kAdaaUnprimed * 0.5f) { u1 = x; return x + driveAmount * h(x); }
        const float d = x - u1; float hv;
        if (std::abs(d) > 1.0e-4f)
        { auto H = [](float v) { const float k = 1.4f, t = std::tanh(k * v);
                                 return 1.15f * (v - t / k) + (0.75f / k) * (lnCoshF(k * v) + 0.5f * (1.0f - t * t)); };
          hv = (H(x) - H(u1)) / d; }
        else hv = h(0.5f * (x + u1));
        u1 = x; return x + driveAmount * hv;
    }
    const float a  = juce::jlimit(0.0f, 1.0f, driveAmount);
    const float g  = 1.0f + a * a * 24.0f;
    const float u  = x * g;
    const float mk = 1.0f / (1.0f + (g - 1.0f) * 0.125f);
    float y;
    switch (driveType)
    {
        case DC::SoftClip: y = adaaTanh(u, u1); break;
        case DC::Tube:
        {   constexpr float b = 0.35f, tb = 0.33638f;   // tanh(0.35)
            if (u1 >= kAdaaUnprimed * 0.5f) { u1 = u; y = 1.2f * (std::tanh(u + b) - tb); break; }
            const float d = u - u1;
            y = std::abs(d) > 1.0e-4f
                ? 1.2f * ((lnCoshF(u + b) - lnCoshF(u1 + b)) / d - tb)
                : 1.2f * (std::tanh(0.5f * (u + u1) + b) - tb);
            u1 = u; break;
        }
        case DC::HardClip:
        {
            auto F = [](float v) { const float av = std::abs(v); return av <= 1.0f ? 0.5f * v * v : av - 0.5f; };
            if (u1 >= kAdaaUnprimed * 0.5f) { u1 = u; y = juce::jlimit(-1.0f, 1.0f, u); break; }
            const float d = u - u1;
            y = std::abs(d) > 1.0e-4f ? (F(u) - F(u1)) / d
                                      : juce::jlimit(-1.0f, 1.0f, 0.5f * (u + u1));
            u1 = u; break;
        }
        case DC::Foldback:
        {
            if (u1 >= kAdaaUnprimed * 0.5f) { u1 = u; y = triFold(0.6f * u); break; }
            const float d = u - u1;
            y = std::abs(d) > 1.0e-4f ? (triFoldI(0.6f * u) - triFoldI(0.6f * u1)) / (0.6f * d)
                                      : triFold(0.6f * 0.5f * (u + u1));
            u1 = u; break;
        }
        case DC::Fuzz:
        {
            if (u1 >= kAdaaUnprimed * 0.5f) { u1 = u; y = fuzzF(u); break; }
            const float d = u - u1;
            y = std::abs(d) > 1.0e-4f ? (fuzzFI(u) - fuzzFI(u1)) / d : fuzzF(0.5f * (u + u1));
            u1 = u; break;
        }
        default: u1 = u; y = u; break;   // (Bitcrush is routed to driveSample by the caller)
    }
    return y * mk;
}

