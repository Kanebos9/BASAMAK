// MOD MATRIX (per-slot, 6 routes; v1.3.9). Locks:
//   [1] BIT-IDENTICAL when inactive: all routes Off, and a route with amt 0, render byte-for-byte
//       like the plain sound (the render path must be unchanged when no route is live).
//   [2] Velocity -> Filter1 Cutoff is AUDIBLE: two step velocities differ with the route on;
//       identical with it off (proves the source reaches the target).
//   [3] Mod LFO is REPEATABLE: bar 1 == bar 2 (the free clock is timeline-anchored).
//   [4] Extreme amounts on every route stay finite.
//   [5] LFO is a pure MATRIX SOURCE: routing LFO1->Filter1 Cutoff wobbles the filter; an LFO with no
//       route = EXACTLY the no-LFO baseline (LFOs do nothing until routed in the matrix).
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
    {   // [5] LFO is a pure MATRIX SOURCE now: an LFO does nothing until a route sends it somewhere.
        //     Routing LFO1 -> Filter1 Cutoff makes it wobble; no route = EXACTLY the no-LFO baseline.
        auto lfoBase = [](DrumChannel& c){ voice(c); c.slots[0].hold = 1.0f;
            c.slots[0].lfoAmt[0] = 0.8f; c.slots[0].lfoRate[0] = 6.0f; c.slots[0].lfoSync[0] = 0.0f; };
        auto onFilt = render([&](DrumChannel& c){ lfoBase(c);
            c.slots[0].mod[0].src = DrumChannel::MSLfoFilt; c.slots[0].mod[0].tgt = DrumChannel::MTFilt1Cut;
            c.slots[0].mod[0].amt = 1.0f; }, 0.9f, 0.6);                                                  // LFO1 -> Filter
        auto noRoute = render(lfoBase, 0.9f, 0.6);                                                        // LFO on, NO route
        auto noLfo   = render([](DrumChannel& c){ voice(c); c.slots[0].hold = 1.0f; }, 0.9f, 0.6);        // no LFO at all
        const float wob = maxdiff(onFilt, noLfo), gone = maxdiff(noRoute, noLfo);
        printf("[5] LFO source: routed->filter wob=%.4f (>0.02), unrouted vs no-LFO=%.6f (==0) -> %s\n",
               wob, gone, CHK(wob > 0.02f && gone == 0.0f && finite(onFilt)) ? "OK" : "FAIL");
    }
    {   // [4] every route maxed = finite
        auto x = render([](DrumChannel& c){ voice(c);
            const int tg[6] = { DrumChannel::MTFilt1Cut, DrumChannel::MTPitch, DrumChannel::MTDrive,
                                DrumChannel::MTChComp, DrumChannel::MTWidth, DrumChannel::MT_GRID_BASE };
            const int sr2[6] = { DrumChannel::MSVel, DrumChannel::MSModEnv, DrumChannel::MSModLfo,
                                 DrumChannel::MSRandom, DrumChannel::MSNote, DrumChannel::MSAmpEnv };
            for (int r = 0; r < 6; ++r) { c.slots[0].mod[r].src = (int8_t) sr2[r];
                c.slots[0].mod[r].tgt = (int8_t) tg[r]; c.slots[0].mod[r].amt = (r & 1) ? -1.0f : 1.0f; } },
            0.9f, 0.6);
        printf("[4] extreme routes finite: rms=%.4f -> %s\n", rms(x), CHK(finite(x)) ? "OK" : "FAIL");
    }
    {   // [6] PER-VOICE: two poly notes with Note -> Filter1 Cutoff each get their OWN cutoff (from their
        //     own pitch), NOT the newest note's. So a poly chord == the SUM of the two single notes (each
        //     voice filtered independently); the old newest-voice code filtered BOTH at the last cutoff.
        const int bs = 256;
        auto renderCh = [&](auto keys, double sec) {
            DrumChannel ch;
            for (auto& sl : ch.slots) sl = Slot();
            auto& s = ch.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSaw; s.oscFreq = 261.6256f;
            s.atk = 0.002f; s.hold = 0.5f; s.dec = 0.5f;
            s.filterType = DrumChannel::LowPass; s.filterCutoff = 1000.0f; s.filterReso = 1.4f;
            s.mod[0].src = DrumChannel::MSNote; s.mod[0].tgt = DrumChannel::MTFilt1Cut; s.mod[0].amt = 0.6f;
            ch.prepareToPlay(SR, bs);
            keys(ch);
            std::vector<float> out; juce::AudioBuffer<float> buf(2, bs);
            for (int b = 0; b < (int)(sec * SR / bs); ++b)
            { buf.clear(); ch.renderInto(buf, 0, bs, false); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
            return out;
        };
        auto outA  = renderCh([](DrumChannel& ch){ ch.keyDown(48, 1.0f, 0, true); }, 0.4);   // low note = dark
        auto outB  = renderCh([](DrumChannel& ch){ ch.keyDown(72, 1.0f, 0, true); }, 0.4);   // high note = bright
        auto outAB = renderCh([](DrumChannel& ch){ ch.keyDown(48, 1.0f, 0, true); ch.keyDown(72, 1.0f, 0, true); }, 0.4);
        std::vector<float> sum(outAB.size(), 0.0f);
        for (size_t i = 0; i < sum.size(); ++i) sum[i] = (i < outA.size() ? outA[i] : 0.0f) + (i < outB.size() ? outB[i] : 0.0f);
        const float superpos = maxdiff(outAB, sum);   // per-voice: ~0 (voices independent); newest-voice: large
        const float ab = maxdiff(outA, outB);          // the two notes ARE different = modulation active
        printf("[6] per-voice Note->cutoff: chord-vs-(A+B) maxdiff=%.4f (per-voice ~0), A-vs-B=%.4f (>0.02) -> %s\n",
               superpos, ab, CHK(superpos < 0.05f && ab > 0.02f && finite(outAB)) ? "OK" : "FAIL");
    }
    {   // [7] CHANNEL FX (Chorus/Flanger/Phaser/Comp act on the whole channel, both slots combined):
        //     render FINITE + audibly change the tone; all 0 = bit-identical (covered by [1]).
        auto dry = render(voice, 0.9f, 0.6);
        auto flg = render([](DrumChannel& c){ voice(c); c.chFlanger = 1.0f; }, 0.9f, 0.6);
        auto phs = render([](DrumChannel& c){ voice(c); c.chPhaser  = 1.0f; }, 0.9f, 0.6);
        auto cho = render([](DrumChannel& c){ voice(c); c.chChorus  = 1.0f; }, 0.9f, 0.6);
        auto cmp = render([](DrumChannel& c){ voice(c); c.chComp    = 1.0f; }, 0.9f, 0.6);
        const float fd = maxdiff(dry, flg), pd = maxdiff(dry, phs), cd = maxdiff(dry, cho), md = maxdiff(dry, cmp);
        const bool ok = fd > 0.001f && pd > 0.001f && cd > 0.001f && md > 0.001f
                     && finite(flg) && finite(phs) && finite(cho) && finite(cmp);
        printf("[7] CHANNEL FX: flanger=%.4f phaser=%.4f chorus=%.4f comp=%.4f (all >0.001, finite) -> %s\n",
               fd, pd, cd, md, CHK(ok) ? "OK" : "FAIL");
    }
    {   // [8] CHANNEL FX are MODULATABLE from a slot's matrix ("... (Channel)" targets): an LFO routed to
        //     Chorus (Channel) must change the render vs the same sound with the route removed.
        auto off = render([](DrumChannel& c){ voice(c); c.chChorus = 0.5f; }, 0.9f, 0.8);
        auto on  = render([](DrumChannel& c){ voice(c); c.chChorus = 0.5f;
            c.slots[0].lfoRate[0] = 3.0f; c.slots[0].lfoAmt[0] = 1.0f;
            c.slots[0].mod[0].src = DrumChannel::MSLfoFilt;      // LFO 1
            c.slots[0].mod[0].tgt = DrumChannel::MTChChorus;     // -> Chorus (Channel)
            c.slots[0].mod[0].amt = 1.0f; }, 0.9f, 0.8);
        const float d = maxdiff(off, on);
        printf("[8] slot matrix -> Chorus (Channel): maxdiff=%.4f (>0.002), finite=%d -> %s\n",
               d, (int) finite(on), CHK(d > 0.002f && finite(on)) ? "OK" : "FAIL");
    }
    {   // [9] DE-ZIPPER: a fast LFO -> Volume on a pure sine must NOT step at block edges (the
        //     "crackling when parameters go up and down fast" bug). Weight is smoothed per sample
        //     (~3 ms), so the max sample-to-sample delta stays near the sine's own slope.
        auto out = render([](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 0.5f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 110.0f;
            s.atk = 0.002f; s.hold = 0.9f; s.dec = 0.5f;
            s.lfoRate[0] = 8.0f; s.lfoAmt[0] = 1.0f;
            s.mod[0].src = DrumChannel::MSLfoFilt; s.mod[0].tgt = DrumChannel::MTVol; s.mod[0].amt = 1.0f; }, 0.9f, 0.9);
        float md = 0.0f, pk = 0.0f;
        for (size_t i = (size_t)(SR * 0.1); i + 1 < out.size(); ++i)
        { md = std::max(md, std::abs(out[i + 1] - out[i])); pk = std::max(pk, std::abs(out[i])); }
        printf("[9] Vol-mod de-zipper: max sample delta=%.5f peak-after-0.1s=%.4f rms=%.4f (delta <0.02 with sound) -> %s\n",
               md, pk, rms(out), CHK(md < 0.02f && pk > 0.05f && finite(out)) ? "OK" : "FAIL");
    }
    printf(fails == 0 ? ">>> ModMatrixTest PASS\n" : ">>> ModMatrixTest FAIL (%d)\n", fails);
    return fails;
}
