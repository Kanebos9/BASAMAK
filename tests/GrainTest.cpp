// GRANULAR engine (SrcGrain, v1.3.9). Locks:
//   [1] DETERMINISM: two renders of the same grain sound are bit-identical (identical-hits rule -
//       the grain dice roll from the voice's seeded noiseState, never a free-running RNG)
//   [2] DENSITY is audible: sparse vs dense renders differ
//   [3] SIZE is audible: micro-grains vs big grains differ
//   [4] PITCH: +12 st step pitch ~doubles the zero-crossing rate (table mode, no sprays)
//   [5] everything finite
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0; static const int BS = 480;

struct Cfg { float size = 0.5f, dens = 0.8f, spray = 0.0f, pitch = 0.0f; int stepPitch = 0; };

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
    c.rebuildGrainTables();                     // message-thread bake (the harness IS the message thread)
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
    printf(fails == 0 ? ">>> GrainTest PASS\n" : ">>> GrainTest FAIL (%d)\n", fails);
    return fails;
}
