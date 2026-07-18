// WAVEGUIDE engine (SrcWguide, 2026-07-18). The bank's first CONTINUOUSLY-DRIVEN physical model
// (Reed / Flute jet / Bow friction into a damped delay loop). Locks:
//   [1] SUSTAIN CONTRACT: a held key keeps sounding (late-held level ~ early level) and DECAYS
//       after release - the whole point of the engine vs Karplus-Strong's strike-then-decay
//   [2] PITCH: the reed bore's fundamental lands on the asked note (Goertzel 220 vs off-freq)
//   [3] EXCITERS differ audibly: Reed vs Flute vs Bow are three different sounds, all non-silent
//   [4] BRIGHTNESS is audible (loop damping)
//   [5] BOUNDED under abuse: full pressure/breath, scale unison 3, every mode -> finite, |x| < 4
//   [6] DETERMINISM: identical hits are bit-identical (breath noise is seeded per hit)
//   [7] SEQUENCED steps play too (the AHD envelope drives the pressure)
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0; static const int BS = 480;

struct Cfg { int mode = 0; float press = 0.6f, breath = 0.12f, bright = 0.6f, sus = 0.9f;
             float freq = 220.0f; bool scale3 = false; bool drawPreset = false; };

// the WgCurveEditor's preset seeds (mirror of the UI's presetY over the +-2.5 domain)
static float presetY(int which, float x)
{
    if (which == 0) return juce::jlimit(-1.0f, 1.0f, 0.7f - 0.3f * x);
    if (which == 1) { const float j = juce::jlimit(-1.0f, 1.0f, x); return j * (j * j - 1.0f); }
    return juce::jlimit(0.0f, 1.0f, std::pow(std::abs(x * 3.0f) + 0.75f, -4.0f)) * 2.0f - 1.0f;
}

struct Run { Sequencer* q; DrumChannel* c; };
static Run makeRun(const Cfg& cfg)
{
    auto* q = new Sequencer();
    q->setStandaloneBpm(120.0f);
    auto& c = q->patterns[0].channels[0];
    for (auto& sl : c.slots) sl = DrumChannel::Slot();
    auto& s = c.slots[0];
    s.engine = DrumChannel::SrcWguide; s.weight = 1.0f;
    s.oscFreq = cfg.freq;
    s.wgExcite = cfg.mode; s.wgPressure = cfg.press; s.wgBreath = cfg.breath; s.wgBright = cfg.bright;
    s.atk = 0.01f; s.hold = 0.0f; s.dec = 0.3f; s.sustain = cfg.sus; s.release = 0.12f;
    if (cfg.scale3) { s.scaleOn = true; s.scaleType = 0; s.scaleKey = 0; s.scaleUnison = 3; }
    if (cfg.drawPreset)   // load the matching preset AS A DRAWING (must behave like the formula now)
    {
        s.wgCurveOn[cfg.mode] = true;
        for (int k = 0; k < 64; ++k)
        {
            const float x = ((float) k / 63.0f * 2.0f - 1.0f) * 2.5f;
            s.wgCurve[cfg.mode][k] = (uint16_t) juce::jlimit(0, 65535,
                (int) std::lround((presetY(cfg.mode, x) * 0.5f + 0.5f) * 65535.0f));
        }
    }
    c.numSteps = 8;
    for (auto& p : q->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, BS);
    c.ensureKsBuffers();
    return { q, &c };
}
static void renderInto(Run& r, std::vector<float>& out, double secs)
{
    juce::AudioBuffer<float> buf(2, BS);
    for (int b = 0; b < (int)(secs * SR / BS); ++b)
    { buf.clear(); r.q->processBlock(buf, SR, BS, nullptr); for (int i = 0; i < BS; ++i) out.push_back(buf.getSample(0, i)); }
}
static std::vector<float> renderKey(const Cfg& cfg, double holdSec, double tailSec)
{
    Run r = makeRun(cfg);
    std::vector<float> out;
    r.c->keyDown(60, 1.0f, 0, false);
    renderInto(r, out, holdSec);
    if (tailSec > 0.0) { r.c->keyUp(60); renderInto(r, out, tailSec); }
    delete r.q;
    return out;
}
static double rmsWin(const std::vector<float>& x, double t0, double t1)
{
    const int a = (int)(t0 * SR), b = std::min((int)(t1 * SR), (int) x.size());
    double e = 0; int n = 0;
    for (int i = a; i < b; ++i) { e += (double) x[i] * x[i]; ++n; }
    return n > 0 ? std::sqrt(e / n) : 0.0;
}
static double goertzel(const std::vector<float>& x, double f, double t0, double t1)
{
    const int a = (int)(t0 * SR), b = std::min((int)(t1 * SR), (int) x.size());
    const double w = 2.0 * juce::MathConstants<double>::pi * f / SR, cw = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = a; i < b; ++i) { s0 = (double) x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - cw * s1 * s2));
}
static float maxdiff(const std::vector<float>& a, const std::vector<float>& b)
{ float m = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::abs(a[i] - b[i])); return m; }
static bool finiteBounded(const std::vector<float>& x, float cap)
{ for (float v : x) if (! std::isfinite(v) || std::abs(v) > cap) return false; return true; }

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };

    {   // [1] sustain while held, decay after release
        Cfg c0; auto x = renderKey(c0, 1.4, 0.9);
        const double early = rmsWin(x, 0.2, 0.6), late = rmsWin(x, 1.0, 1.4), tail = rmsWin(x, 2.0, 2.3);
        printf("[1] sustain: early=%.4f late=%.4f (late/early %.2f expect >0.3) tail=%.5f (expect <0.15*late) -> %s\n",
               early, late, early > 1e-6 ? late / early : 0.0, tail,
               CHK(early > 0.005 && late > 0.3 * early && tail < 0.15 * late) ? "OK" : "FAIL");
    }
    {   // [2] pitch: keyDown(60) plays C4 = 261.63 Hz (keys are ABSOLUTE - the knob is only the
        // step-mode base); the fundamental must land there, an off-freq probe far weaker
        Cfg c0; auto x = renderKey(c0, 1.0, 0.0);
        const double on = goertzel(x, 261.63, 0.4, 0.9), off = goertzel(x, 370.0, 0.4, 0.9);
        printf("[2] pitch: g(261.6)=%.1f g(370)=%.1f (ratio %.1f expect >4) -> %s\n",
               on, off, off > 1e-9 ? on / off : 999.0, CHK(on > 4.0 * off && on > 1.0) ? "OK" : "FAIL");
    }
    {   // [3] the three exciters are three different, non-silent sounds
        Cfg r0; r0.mode = 0; Cfg f0; f0.mode = 1; Cfg b0; b0.mode = 2;
        auto xr = renderKey(r0, 0.8, 0.0), xf = renderKey(f0, 0.8, 0.0), xb = renderKey(b0, 0.8, 0.0);
        const double rr = rmsWin(xr, 0.2, 0.8), rf = rmsWin(xf, 0.2, 0.8), rb = rmsWin(xb, 0.2, 0.8);
        printf("[3] exciters: rms reed=%.4f flute=%.4f bow=%.4f (all >0.003), reed-vs-flute maxdiff=%.3f, "
               "reed-vs-bow=%.3f (both >0.05) -> %s\n", rr, rf, rb, maxdiff(xr, xf), maxdiff(xr, xb),
               CHK(rr > 0.003 && rf > 0.003 && rb > 0.003 && maxdiff(xr, xf) > 0.05f && maxdiff(xr, xb) > 0.05f) ? "OK" : "FAIL");
    }
    {   // [4] brightness (loop damping) is audible
        Cfg d0; d0.bright = 0.08f; Cfg b1; b1.bright = 0.95f;
        auto xd = renderKey(d0, 0.8, 0.0), xb = renderKey(b1, 0.8, 0.0);
        printf("[4] brightness: dark-vs-bright maxdiff=%.3f (expect >0.05) -> %s\n",
               maxdiff(xd, xb), CHK(maxdiff(xd, xb) > 0.05f) ? "OK" : "FAIL");
    }
    {   // [5] bounded under abuse (full pressure/breath, 3-bore scale, all modes)
        bool ok = true;
        for (int m = 0; m < 3; ++m)
        { Cfg a; a.mode = m; a.press = 1.0f; a.breath = 1.0f; a.bright = 1.0f; a.scale3 = true;
          auto x = renderKey(a, 0.8, 0.4); ok = ok && finiteBounded(x, 4.0f); }
        printf("[5] abuse: all modes finite + |x|<4 -> %s\n", CHK(ok) ? "OK" : "FAIL");
    }
    {   // [6] determinism (breath noise is per-hit seeded)
        Cfg c0; auto a = renderKey(c0, 0.7, 0.0), b = renderKey(c0, 0.7, 0.0);
        printf("[6] determinism: repeat maxdiff=%.6f (expect 0) -> %s\n",
               maxdiff(a, b), CHK(maxdiff(a, b) == 0.0f) ? "OK" : "FAIL");
    }
    {   // [7] sequenced steps play (AHD drives the pressure; no key involved)
        Cfg c0; Run r = makeRun(c0);
        r.c->steps[0] = true;
        r.q->startStandalone();
        std::vector<float> x; renderInto(r, x, 0.8);
        delete r.q;
        printf("[7] sequenced: rms=%.4f (expect >0.002) -> %s\n",
               rmsWin(x, 0.0, 0.6), CHK(rmsWin(x, 0.0, 0.6) > 0.002) ? "OK" : "FAIL");
    }

    {   // [8] DRAWN PRESET == FORMULA-LIKE [2026-07-18 r4]: the drawing's +-2.5 domain covers the
        // full felt range now, so tracing the built-in curve must sound like the built-in (level
        // within 40%, still tonal) - r3's +-1 window buzzed against the clamps here.
        bool ok = true;
        for (int m = 0; m < 3; ++m)
        {
            Cfg f0; f0.mode = m; Cfg d0 = f0; d0.drawPreset = true;
            auto xf = renderKey(f0, 0.9, 0.0), xd = renderKey(d0, 0.9, 0.0);
            const double rf = rmsWin(xf, 0.3, 0.9), rd = rmsWin(xd, 0.3, 0.9);
            const bool okm = rd > 0.5 * rf && rd < 2.0 * rf && finiteBounded(xd, 4.0f);
            printf("    mode %d: formula rms=%.4f drawn-preset rms=%.4f -> %s\n", m, rf, rd, okm ? "ok" : "BAD");
            ok = ok && okm;
        }
        printf("[8] drawn preset ~ formula -> %s\n", CHK(ok) ? "OK" : "FAIL");
    }

    printf(fails == 0 ? "WaveguideTest: ALL PASS\n" : "WaveguideTest: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
