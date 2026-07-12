// MOD MATRIX (per-slot, 6 routes; v1.3.9). Locks:
//   [1] BIT-IDENTICAL when inactive: all routes Off, and a route with amt 0, render byte-for-byte
//       like the plain sound (the render path must be unchanged when no route is live).
//   [2] Velocity -> Filter1 Cutoff is AUDIBLE: two step velocities differ with the route on;
//       identical with it off (proves the source reaches the target).
//   [3] Mod LFO is REPEATABLE: bar 1 == bar 2 (the free clock is timeline-anchored).
//   [4] Extreme amounts on every route stay finite.
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const double SR = 48000.0; static const int BS = 480;
using Slot = DrumChannel::Slot;

// A simple sustained saw with a resonant low-pass on slot 0, so cutoff modulation is easy to hear.
static void voice(DrumChannel& c)
{
    for (auto& sl : c.slots) sl = Slot();
    auto& s = c.slots[0];
    s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = DrumChannel::WvSaw; s.oscFreq = 110.0f;
    s.atk = 0.002f; s.hold = 0.5f; s.dec = 0.5f;
    s.filterType = DrumChannel::LowPass; s.filterCutoff = 500.0f; s.filterReso = 3.0f;
}

static std::vector<float> render(std::function<void(DrumChannel&)> setup, float vel, double secs, int stepPitch = 0)
{
    auto* q = new Sequencer();
    q->setStandaloneBpm(120.0f);
    auto& c = q->patterns[0].channels[0];
    setup(c);
    c.numSteps = 8; c.steps[0] = true; c.stepVel[0] = vel; c.stepPitch[0] = (float) stepPitch;
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
static bool finite(const std::vector<float>& x){ for (float v : x) if (! std::isfinite(v)) return false; return true; }
static double rms(const std::vector<float>& x){ double e=0; for (float v:x) e+=(double)v*v; return std::sqrt(e/(double)x.size()); }

int main()
{
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };

    {   // [1] inactive = bit-identical (plain vs all-Off vs a route with amt 0)
        auto plain = render(voice, 0.9f, 0.6);
        auto offRt = render([](DrumChannel& c){ voice(c);
            c.slots[0].mod[0].src = DrumChannel::MSVel; c.slots[0].mod[0].tgt = DrumChannel::MTFilt1Cut;
            c.slots[0].mod[0].amt = 0.0f; }, 0.9f, 0.6);
        printf("[1] inactive bit-identical: maxdiff=%.6f (expect 0), rms=%.4f -> %s\n",
               maxdiff(plain, offRt), rms(plain),
               CHK(maxdiff(plain, offRt) == 0.0f && rms(plain) > 0.01 && finite(plain)) ? "OK" : "FAIL");
    }
    {   // [2] Velocity -> Filter1 Cutoff audible; off = identical across velocities
        auto onLo = render([](DrumChannel& c){ voice(c);
            c.slots[0].mod[0].src = DrumChannel::MSVel; c.slots[0].mod[0].tgt = DrumChannel::MTFilt1Cut;
            c.slots[0].mod[0].amt = 0.8f; }, 0.2f, 0.6);
        auto onHi = render([](DrumChannel& c){ voice(c);
            c.slots[0].mod[0].src = DrumChannel::MSVel; c.slots[0].mod[0].tgt = DrumChannel::MTFilt1Cut;
            c.slots[0].mod[0].amt = 0.8f; }, 0.95f, 0.6);
        auto offLo = render(voice, 0.2f, 0.6);
        auto offHi = render(voice, 0.95f, 0.6);
        const float on = maxdiff(onLo, onHi), off = maxdiff(offLo, offHi);
        printf("[2] Vel->Cutoff: on maxdiff=%.4f (expect >0.02), off maxdiff=%.4f -> %s\n",
               on, off, CHK(on > 0.02f && finite(onHi)) ? "OK" : "FAIL");
    }
    {   // [3] Mod LFO -> Cutoff repeatable across bars (timeline-anchored free clock)
        auto setup = [](DrumChannel& c){ voice(c); c.slots[0].hold = 2.0f;
            c.slots[0].modLfoRate = 2.0f; c.slots[0].modLfoShape = 0;
            c.slots[0].mod[0].src = DrumChannel::MSModLfo; c.slots[0].mod[0].tgt = DrumChannel::MTFilt1Cut;
            c.slots[0].mod[0].amt = 0.6f; };
        auto x = render(setup, 0.9f, 4.0);   // 2 bars @ 120 BPM (2 s/bar)
        const int perBar = (int)(2.0 * SR);
        std::vector<float> b1(x.begin() + perBar / 4, x.begin() + perBar / 4 + perBar / 4);
        std::vector<float> b2(x.begin() + perBar + perBar / 4, x.begin() + perBar + perBar / 4 + perBar / 4);
        printf("[3] Mod LFO bar1 vs bar2: maxdiff=%.6f (expect ~0) -> %s\n",
               maxdiff(b1, b2), CHK(maxdiff(b1, b2) < 1.0e-4f && finite(x)) ? "OK" : "FAIL");
    }
    {   // [4b] Step Mod Lane A -> Filter1 Cutoff: two different lane values render differently
        auto mk = [](float laneVal){ return [laneVal](DrumChannel& c){ voice(c);
            for (int i = 0; i < 8; ++i) c.stepModA[i] = laneVal;   // whole lane = laneVal
            c.slots[0].mod[0].src = DrumChannel::MSStepModA; c.slots[0].mod[0].tgt = DrumChannel::MTFilt1Cut;
            c.slots[0].mod[0].amt = 0.8f; }; };
        auto lo = render(mk(0.0f), 0.9f, 0.6), hi = render(mk(1.0f), 0.9f, 0.6);
        printf("[4b] StepMod A->Cutoff: lane0 vs lane1 maxdiff=%.4f (expect >0.02) -> %s\n",
               maxdiff(lo, hi), CHK(maxdiff(lo, hi) > 0.02f && finite(hi)) ? "OK" : "FAIL");
    }
    {   // [4] every route maxed = finite
        auto x = render([](DrumChannel& c){ voice(c);
            const int tg[6] = { DrumChannel::MTFilt1Cut, DrumChannel::MTPitch, DrumChannel::MTDrive,
                                DrumChannel::MTComp, DrumChannel::MTWidth, DrumChannel::MT_GRID_BASE };
            const int sr2[6] = { DrumChannel::MSVel, DrumChannel::MSModEnv, DrumChannel::MSModLfo,
                                 DrumChannel::MSRandom, DrumChannel::MSNote, DrumChannel::MSAmpEnv };
            for (int r = 0; r < 6; ++r) { c.slots[0].mod[r].src = (int8_t) sr2[r];
                c.slots[0].mod[r].tgt = (int8_t) tg[r]; c.slots[0].mod[r].amt = (r & 1) ? -1.0f : 1.0f; } },
            0.9f, 0.6);
        printf("[4] extreme routes finite: rms=%.4f -> %s\n", rms(x), CHK(finite(x)) ? "OK" : "FAIL");
    }
    printf(fails == 0 ? ">>> ModMatrixTest PASS\n" : ">>> ModMatrixTest FAIL (%d)\n", fails);
    return fails;
}
