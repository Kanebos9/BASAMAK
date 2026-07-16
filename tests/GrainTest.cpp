// GRANULAR engine (SrcGrain, v1.3.9). Locks:
//   [1] DETERMINISM: two renders of the same grain sound are bit-identical (identical-hits rule -
//       the grain dice roll from the voice's seeded noiseState, never a free-running RNG)
//   [2] DENSITY is audible: sparse vs dense renders differ
//   [3] SIZE is audible: micro-grains vs big grains differ
//   [4] PITCH: +12 st step pitch ~doubles the zero-crossing rate (table mode, no sprays)
//   [5] everything finite
//   [6] SCALE voicing on granular [2026-07-16]: grains cycle the diatonic triad's notes - the
//       third + fifth are present with scaleOn, absent without (the "cloud is the chord")
//   [7] GLIDE JOURNEY on granular [2026-07-16]: the drawn A>B/B>C/C>D times travel the grain
//       position - a note starts on frame A's tone and ends on frame D's
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0; static const int BS = 480;

struct Cfg { float size = 0.5f, dens = 0.8f, spray = 0.0f, pitch = 0.0f; int stepPitch = 0;
             void (*tweak)(DrumChannel::Slot&) = nullptr; };   // [2026-07-16] extra slot setup (scale/journey tests)

static std::vector<float> render(const Cfg& cfg, double secs)
{
    auto* q = new Sequencer();
    q->setStandaloneBpm(120.0f);
    auto& c = q->patterns[0].channels[0];
    for (auto& sl : c.slots) sl = DrumChannel::Slot();
    auto& s = c.slots[0];
    s.engine = DrumChannel::SrcGrain; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 220.0f;
    s.grainPos = 0.5f; s.grainSize = cfg.size; s.grainDens = cfg.dens;
    s.grainSpray = cfg.spray; s.grainPitch = cfg.pitch;
    s.atk = 0.002f; s.hold = 0.3f; s.dec = 0.4f;
    c.numSteps = 8; c.steps[0] = true; c.stepPitch[0] = (float) cfg.stepPitch;
    if (cfg.tweak) cfg.tweak(s);
    c.rebuildAddTables();                       // bakes the frame tables AND chains rebuildGrainTables (message thread)
    for (auto& p : q->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, BS);
    q->startStandalone();
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, BS);
    for (int b = 0; b < (int)(secs * SR / BS); ++b)
    { buf.clear(); q->processBlock(buf, SR, BS, nullptr); for (int i = 0; i < BS; ++i) out.push_back(buf.getSample(0, i)); }
    delete q;
    return out;
}

static float maxdiff(const std::vector<float>& a, const std::vector<float>& b)
{ float m = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::abs(a[i] - b[i])); return m; }
static int zc(const std::vector<float>& x, int a, int n)
{ int z = 0; for (int i = a + 1; i < a + n && i < (int) x.size(); ++i) if ((x[i-1] <= 0) != (x[i] <= 0)) ++z; return z; }
static bool finite(const std::vector<float>& x)
{ for (float v : x) if (! std::isfinite(v)) return false; return true; }
static double rms(const std::vector<float>& x)
{ double e = 0; for (float v : x) e += (double) v * v; return std::sqrt(e / (double) x.size()); }

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };

    {   // [1] determinism + [5] finite + audible at all
        Cfg c0; auto a = render(c0, 0.6), b = render(c0, 0.6);
        printf("[1] determinism: repeat maxdiff=%.6f (expect 0), rms=%.4f (expect >0.01) -> %s\n",
               maxdiff(a, b), rms(a), CHK(maxdiff(a, b) == 0.0f && rms(a) > 0.01 && finite(a)) ? "OK" : "FAIL");
    }
    {   // [2] density
        Cfg lo; lo.dens = 0.1f; Cfg hi; hi.dens = 0.95f;
        auto a = render(lo, 0.6), b = render(hi, 0.6);
        printf("[2] density: sparse-vs-dense maxdiff=%.3f (expect >0.02) -> %s\n",
               maxdiff(a, b), CHK(maxdiff(a, b) > 0.02f) ? "OK" : "FAIL");
    }
    {   // [3] size
        Cfg sm; sm.size = 0.02f; Cfg bg; bg.size = 0.95f;
        auto a = render(sm, 0.6), b = render(bg, 0.6);
        printf("[3] size: micro-vs-big maxdiff=%.3f (expect >0.02) -> %s\n",
               maxdiff(a, b), CHK(maxdiff(a, b) > 0.02f) ? "OK" : "FAIL");
    }
    {   // [4] pitch: +12 st ~doubles the zero-crossing count (steady sine grains, no sprays)
        Cfg p0; p0.size = 0.9f; p0.dens = 0.9f;
        Cfg p1 = p0; p1.stepPitch = 12;
        auto a = render(p0, 0.5), b = render(p1, 0.5);
        const int z0 = zc(a, 2400, 14400), z1 = zc(b, 2400, 14400);
        const double ratio = (double) z1 / juce::jmax(1, z0);
        printf("[4] pitch: zc %d -> %d, ratio %.2f (expect ~2 within 15%%) -> %s\n",
               z0, z1, ratio, CHK(ratio > 1.7 && ratio < 2.3) ? "OK" : "FAIL");
    }
    {   // [6] scale voicing: A3 base in C major -> A minor triad (A + C + E). Third/fifth
        //     energy appears only with scaleOn (successive grains cycle the chord tones).
        auto gz = [](const std::vector<float>& x, double f) {
            const double w = 2.0 * M_PI * f / SR, cf = 2.0 * std::cos(w);
            double s1 = 0, s2 = 0; const int a0 = 4800, n = 19200;
            for (int i = a0; i < a0 + n && i < (int) x.size(); ++i) { const double s0 = x[(size_t) i] + cf * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - cf * s1 * s2)) / (0.5 * n);
        };
        Cfg on; on.size = 0.9f; on.dens = 0.9f;
        on.tweak = [](DrumChannel::Slot& sl) { sl.scaleOn = true; sl.scaleType = 0; sl.scaleKey = 0; sl.scaleUnison = 3; };
        Cfg off = on; off.tweak = nullptr;
        auto a = render(on, 0.6), b = render(off, 0.6);
        const double third = 220.0 * std::pow(2.0, 3.0 / 12.0), fifth = 220.0 * std::pow(2.0, 7.0 / 12.0);
        const double t1 = gz(a, third), f1 = gz(a, fifth), t0 = gz(b, third), f0 = gz(b, fifth);
        printf("[6] grain scale: third on=%.4f off=%.4f | fifth on=%.4f off=%.4f -> %s\n",
               t1, t0, f1, f0, CHK(t1 > 4.0 * (t0 + 1e-6) && f1 > 4.0 * (f0 + 1e-6) && t1 > 0.005) ? "TRIAD IN THE CLOUD (OK)" : "FAIL");
    }
    {   // [7] glide journey: frame A = pure h1, frame D = pure h4 (2 octaves up); addSeg
        //     {0.15, 0.1, 0.1} = full travel in 0.35 s. Early window = fundamental dominates,
        //     late window (journey done) = the 4th harmonic dominates.
        auto gzw = [](const std::vector<float>& x, double f, int a0, int n) {
            const double w = 2.0 * M_PI * f / SR, cf = 2.0 * std::cos(w);
            double s1 = 0, s2 = 0;
            for (int i = a0; i < a0 + n && i < (int) x.size(); ++i) { const double s0 = x[(size_t) i] + cf * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - cf * s1 * s2)) / (0.5 * n);
        };
        // SMALL grains + a HOLD leg: giant grains wrap the circular table and smear the journey
        // (physics); parking at frame B (pos 1/3 = mid-table) keeps the reads wrap-free.
        Cfg j; j.size = 0.2f; j.dens = 0.9f;
        j.tweak = [](DrumChannel::Slot& sl) {
            sl.oscShape = DrumChannel::WvCustom;
            for (int f = 0; f < 4; ++f) for (int h = 0; h < 32; ++h) sl.addH[f][h] = 0.0f;
            sl.addH[0][0] = 1.0f;                                               // A = pure h1
            for (int f = 1; f < 4; ++f) sl.addH[f][3] = 1.0f;                   // B/C/D = pure h4
            sl.addSeg[0] = 0.15f;                                               // A>B in 0.15 s, then HOLD at B
        };
        auto a = render(j, 0.7);
        const int w0a = (int) (0.02 * SR), w1a = (int) (0.30 * SR), wn = (int) (0.12 * SR);
        const double e1 = gzw(a, 220.0, w0a, wn), e4 = gzw(a, 880.0, w0a, wn);
        const double l1 = gzw(a, 220.0, w1a, wn), l4 = gzw(a, 880.0, w1a, wn);
        printf("[7] grain journey: early h1=%.4f>h4=%.4f | late h4=%.4f>h1=%.4f -> %s\n",
               e1, e4, l4, l1, CHK(e1 > 2.0 * e4 && l4 > 2.0 * l1) ? "TRAVELS A->D (OK)" : "FAIL");
    }
    printf(fails == 0 ? ">>> GrainTest PASS\n" : ">>> GrainTest FAIL (%d)\n", fails);
    return fails;
}
