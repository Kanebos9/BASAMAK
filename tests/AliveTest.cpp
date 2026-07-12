// ENGINE-LIFE batch (v1.3.7): DRIFT (per-note randomness), LFO shapes + FREE-RUN (timeline
// anchored), FILTER DRIVE (in-loop saturation), FDN reverb MODES. Locks:
//   [1] drift 0  = two renders bit-identical (the determinism default survives)
//   [2] drift on = two renders DIFFER (true random) while the fundamental stays in tune
//   [3] filter drive 0 = bit-identical; drive on = audibly different and finite
//   [4] LFO shape Square != Sine (vol dest)
//   [5] FREE-RUN determinism: with a mid-bar note, bar N and bar N+1 render IDENTICALLY
//       (timeline anchor) while FREE != RETRIG (the note joins the wave mid-flight)
//   [6] FDN reverb: Hall deterministic; Room/Plate/Shimmer differ from Hall; all finite
#include "Sequencer.h"
#include "FDNReverb.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0; static const int BS = 480;

struct Cfg { float drift = 0.0f, fDrive = 0.0f; int volShape = -1; bool lfoFree = false; int step = 0; int step2 = -1; int uni = 5; float dec = 0.6f; };

static std::vector<float> render(const Cfg& cfg, double secs)
{
    auto* q = new Sequencer();
    q->setStandaloneBpm(120.0f);
    auto& c = q->patterns[0].channels[0];
    for (auto& sl : c.slots) sl = DrumChannel::Slot();
    auto& s = c.slots[0];
    s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = DrumChannel::WvSaw; s.oscFreq = 220.0f;
    s.oscUnison = cfg.uni; s.oscDetune = cfg.uni > 1 ? 0.2f : 0.0f;
    s.atk = 0.002f; s.hold = 0.3f; s.dec = cfg.dec;
    s.drift = cfg.drift;
    if (cfg.fDrive > 0.0f || cfg.volShape == -2)   // -2 = "filter on" marker for [3]
    { s.filterType = DrumChannel::LowPass; s.filterCutoff = 700.0f; s.filterReso = 6.0f; s.filterDrive = cfg.fDrive; }
    if (cfg.volShape >= 0)
    { s.lfoAmt[2] = 0.9f; s.lfoRate[2] = 3.0f; s.lfoShape[2] = cfg.volShape; s.lfoFree[2] = cfg.lfoFree;
      s.mod[0].src = DrumChannel::MSLfoVol; s.mod[0].tgt = DrumChannel::MTVol; s.mod[0].amt = 1.0f; }  // LFO 3 -> Volume via the matrix
    c.numSteps = 8; c.steps[cfg.step] = true; if (cfg.step2 >= 0) c.steps[cfg.step2] = true;
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
{ float m = 0.0f; for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::abs(a[i] - b[i])); return m; }
static int zc(const std::vector<float>& x, int a, int n)
{ int z = 0; for (int i = a + 1; i < a + n && i < (int) x.size(); ++i) if ((x[i-1] <= 0) != (x[i] <= 0)) ++z; return z; }
static bool finite(const std::vector<float>& x)
{ for (float v : x) if (! std::isfinite(v)) return false; return true; }

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };

    {   // [1]+[2] DRIFT. NOTE: the dice live in ONE instance's RNG - within a session every note
        // differs, so the honest probe = TWO CONSECUTIVE NOTES in one render (steps 0 + 4, short
        // dec so the first tail is gone), not two fresh instances (same seed = same tape).
        Cfg c0; auto a = render(c0, 0.5), b = render(c0, 0.5);
        printf("[1] drift 0: repeat maxdiff=%.6f (expect 0) -> %s\n", maxdiff(a, b),
               CHK(maxdiff(a, b) == 0.0f) ? "OK" : "FAIL");
        Cfg c1; c1.drift = 0.6f; c1.step2 = 4; c1.dec = 0.12f;   // notes at 0 s and 1 s (120 BPM)
        auto d = render(c1, 2.0);
        const int N = (int)(0.4 * SR);
        std::vector<float> n1(d.begin(), d.begin() + N), n2(d.begin() + (int)(1.0 * SR), d.begin() + (int)(1.0 * SR) + N);
        Cfg cp; cp.drift = 0.6f; cp.uni = 1;                      // single voice: zc IS a pitch metric
        Cfg cc; cc.uni = 1;
        auto dp = render(cp, 0.5), dc = render(cc, 0.5);
        const int z1 = zc(dp, 4800, 9600), z2 = zc(dc, 4800, 9600);
        printf("[2] drift 0.6: note1-vs-note2 maxdiff=%.3f (expect >0.01), single-voice zc %d vs clean %d (in tune) -> %s\n",
               maxdiff(n1, n2), z1, z2,
               CHK(maxdiff(n1, n2) > 0.01f && std::abs(z1 - z2) <= juce::jmax(2, z2 / 50) && finite(d)) ? "OK" : "FAIL");
    }
    {   // [3] FILTER DRIVE
        Cfg f0; f0.volShape = -2;                 // filter on, drive 0
        auto a = render(f0, 0.5), b = render(f0, 0.5);
        Cfg f1 = f0; f1.fDrive = 0.6f; auto d = render(f1, 0.5);
        printf("[3] filter drive: 0 repeat maxdiff=%.6f (expect 0), drive 0.6 vs 0 maxdiff=%.3f (expect >0.01) -> %s\n",
               maxdiff(a, b), maxdiff(a, d),
               CHK(maxdiff(a, b) == 0.0f && maxdiff(a, d) > 0.01f && finite(d)) ? "OK" : "FAIL");
    }
    {   // [4] LFO SHAPE
        Cfg sN; sN.volShape = 0; Cfg sQ; sQ.volShape = 3;
        auto a = render(sN, 0.5), b = render(sQ, 0.5);
        printf("[4] LFO shape: Square vs Sine maxdiff=%.3f (expect >0.05) -> %s\n",
               maxdiff(a, b), CHK(maxdiff(a, b) > 0.05f) ? "OK" : "FAIL");
    }
    {   // [5] FREE-RUN: note on step 2 of 8 (mid-bar). 120 BPM -> bar = 2 s.
        Cfg fr; fr.volShape = 0; fr.lfoFree = true; fr.step = 2; fr.dec = 0.15f;   // short: the tail
        Cfg rt = fr; rt.lfoFree = false;                                            // dies inside its bar
        auto oF = render(fr, 4.0), oR = render(rt, 4.0);
        const int bar = (int)(2.0 * SR);
        std::vector<float> b1(oF.begin(), oF.begin() + bar), b2(oF.begin() + bar, oF.begin() + 2 * bar);
        printf("[5] free-run: bar1-vs-bar2 maxdiff=%.6f (expect ~0 = deterministic), free-vs-retrig maxdiff=%.3f (expect >0.01) -> %s\n",
               maxdiff(b1, b2), maxdiff(oF, oR),
               CHK(maxdiff(b1, b2) < 1.0e-4f && maxdiff(oF, oR) > 0.01f) ? "OK" : "FAIL");
    }
    {   // [6] FDN modes: impulse responses
        auto ir = [&](int mode) {
            FDNReverb f; f.prepare(SR);
            std::vector<float> L((size_t)(1.0 * SR), 0.0f), R = L; L[0] = R[0] = 1.0f;
            std::vector<float> oL(L.size()), oR(L.size());
            f.process(L.data(), R.data(), oL.data(), oR.data(), (int) L.size(), 0.5f, 0.6f, 0.4f, 1.0f, mode);
            return oL;
        };
        auto hall = ir(1), hall2 = ir(1), room = ir(0), plate = ir(2), shim = ir(3);
        auto energy = [](const std::vector<float>& x){ double e = 0; for (float v : x) e += (double) v * v; return e; };
        const double er = energy(shim) / juce::jmax(1.0e-9, energy(hall));   // blow-up guard: shimmer must
        printf("[6] reverb: hall repeat=%.6f (0), room/plate/shimmer vs hall = %.3f/%.3f/%.3f, shim/hall energy=%.2f (0.2..4) -> %s\n",
               maxdiff(hall, hall2), maxdiff(room, hall), maxdiff(plate, hall), maxdiff(shim, hall), er,
               CHK(maxdiff(hall, hall2) == 0.0f && maxdiff(room, hall) > 0.001f && maxdiff(plate, hall) > 0.001f
                   && maxdiff(shim, hall) > 0.001f && finite(shim) && er > 0.2 && er < 4.0) ? "OK" : "FAIL");
    }
    printf(fails == 0 ? ">>> AliveTest PASS\n" : ">>> AliveTest FAIL (%d)\n", fails);
    return fails;
}
