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
                                DrumChannel::MTChFxBAmt, DrumChannel::MTWidth, DrumChannel::MT_GRID_BASE };
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
    {   // [7] CHANNEL FX slots (2 selectable effects on the whole channel): each type renders FINITE +
        //     audibly changes the tone; CHARACTER changes the voicing; all Off = bit-identical ([1]).
        auto dry = render(voice, 0.9f, 0.6);
        auto mk = [&](int type, float amt, float chr) {
            return render([type, amt, chr](DrumChannel& c){ voice(c);
                c.chFxType[0] = type; c.chFxAmt[0] = amt; c.chFxChar[0] = chr; }, 0.9f, 0.6); };
        auto cho = mk(DrumChannel::ChFxChorus,  1.0f, 0.5f);
        auto flg = mk(DrumChannel::ChFxFlanger, 1.0f, 0.5f);
        auto phs = mk(DrumChannel::ChFxPhaser,  1.0f, 0.5f);
        auto cmp = mk(DrumChannel::ChFxComp,    1.0f, 0.5f);
        auto flgC = mk(DrumChannel::ChFxFlanger, 1.0f, 0.95f);   // character = faster/deeper sweep
        auto tap = mk(DrumChannel::ChFxTape,    1.0f, 0.5f);
        auto apn = mk(DrumChannel::ChFxAutoPan, 1.0f, 0.5f);
        auto wid = render([](DrumChannel& c){ voice(c);           // widener needs STEREO content -> unison width
            c.slots[0].oscUnison = 3; c.slots[0].uniSpread = 0.6f;
            c.chFxType[0] = DrumChannel::ChFxWiden; c.chFxAmt[0] = 1.0f; c.chFxChar[0] = 0.5f; }, 0.9f, 0.6);
        auto widOff = render([](DrumChannel& c){ voice(c);
            c.slots[0].oscUnison = 3; c.slots[0].uniSpread = 0.6f; }, 0.9f, 0.6);
        const float cd = maxdiff(dry, cho), fd = maxdiff(dry, flg), pd = maxdiff(dry, phs), md = maxdiff(dry, cmp);
        const float chd = maxdiff(flg, flgC);
        auto ott = mk(DrumChannel::ChFxOtt,       1.0f, 0.5f);
        auto rot = mk(DrumChannel::ChFxRotary,    1.0f, 0.5f);
        auto exc = render([](DrumChannel& c){ voice(c);
            c.slots[0].fxDriveType = DrumChannel::DriveExciter; c.slots[0].fxDrive = 0.7f; }, 0.9f, 0.6);
        const float td = maxdiff(dry, tap), ad = maxdiff(dry, apn), wd2 = maxdiff(widOff, wid);
        const float od = maxdiff(dry, ott), rd2 = maxdiff(dry, rot), ed = maxdiff(dry, exc);
        const bool ok = cd > 0.001f && fd > 0.001f && pd > 0.001f && md > 0.001f && chd > 0.001f
                     && td > 0.001f && ad > 0.001f && wd2 > 0.001f
                     && od > 0.001f && rd2 > 0.001f && ed > 0.001f
                     && finite(cho) && finite(flg) && finite(phs) && finite(cmp) && finite(flgC)
                     && finite(tap) && finite(apn) && finite(wid) && finite(ott) && finite(rot) && finite(exc);
        printf("[7] CHANNEL FX slots: cho=%.2f flg=%.2f phs=%.2f cmp=%.2f chr=%.2f tape=%.2f pan=%.2f wid=%.2f ott=%.2f rot=%.2f exciter=%.2f (all >0.001) -> %s\n",
               cd, fd, pd, md, chd, td, ad, wd2, od, rd2, ed, CHK(ok) ? "OK" : "FAIL");
    }
    {   // [8] CHANNEL FX are MODULATABLE from a slot's matrix (FX A Amount (Channel)): an LFO routed
        //     to it must change the render vs the same sound with the route removed.
        auto off = render([](DrumChannel& c){ voice(c); c.chFxType[0] = DrumChannel::ChFxChorus; c.chFxAmt[0] = 0.5f; }, 0.9f, 0.8);
        auto on  = render([](DrumChannel& c){ voice(c); c.chFxType[0] = DrumChannel::ChFxChorus; c.chFxAmt[0] = 0.5f;
            c.slots[0].lfoRate[0] = 3.0f; c.slots[0].lfoAmt[0] = 1.0f;
            c.slots[0].mod[0].src = DrumChannel::MSLfoFilt;      // LFO 1
            c.slots[0].mod[0].tgt = DrumChannel::MTChFxAAmt;     // -> FX A Amount (Channel)
            c.slots[0].mod[0].amt = 1.0f; }, 0.9f, 0.8);
        const float d = maxdiff(off, on);
        printf("[8] slot matrix -> FX A Amount (Channel): maxdiff=%.4f (>0.002), finite=%d -> %s\n",
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
    {   // [10] SUB + FORMANT are AUDIBLE (user: "make sure the difference can actually be audible"):
        //      fxSub on a 110 Hz saw must add strong 55 Hz energy (Goertzel); fxFormant must reshape
        //      the output; both 0 = bit-identical (covered by [1]'s path being unchanged).
        auto dry = render(voice, 0.9f, 0.6);
        auto sub = render([](DrumChannel& c){ voice(c); c.slots[0].fxSub = 0.9f; }, 0.9f, 0.6);
        auto fmt = render([](DrumChannel& c){ voice(c); c.slots[0].fxFormant = 0.5f; }, 0.9f, 0.6);
        auto goe = [&](const std::vector<float>& x, double f) {
            const double w = 2.0 * 3.14159265358979 * f / SR, cw = 2.0 * std::cos(w);
            double s0 = 0, s1 = 0, s2 = 0;
            for (size_t i = (size_t)(SR * 0.05); i < x.size() && i < (size_t)(SR * 0.5); ++i)
            { s0 = (double) x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::abs(s1 * s1 + s2 * s2 - cw * s1 * s2)); };
        const double d55 = goe(dry, 55.0), s55 = goe(sub, 55.0);
        const float fdif = maxdiff(dry, fmt);
        printf("[10] SUB 55Hz energy %.1f -> %.1f (expect >3x), FORMANT maxdiff=%.4f (>0.02) -> %s\n",
               d55, s55, fdif, CHK(s55 > d55 * 3.0 && fdif > 0.02f && finite(sub) && finite(fmt)) ? "OK" : "FAIL");
    }
    {   // [11] AUDIO-RATE LFO -> PITCH = real FM: a 200 Hz sine LFO on a 400 Hz sine must create
        //      SIDEBANDS at 400 +- 200 Hz (Goertzel) - block-rate application physically cannot.
        auto goe = [&](const std::vector<float>& x, double f) {
            const double w = 2.0 * 3.14159265358979 * f / SR, cw = 2.0 * std::cos(w);
            double s0 = 0, s1 = 0, s2 = 0;
            for (size_t i = (size_t)(SR * 0.10); i < x.size() && i < (size_t)(SR * 0.55); ++i)
            { s0 = (double) x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::abs(s1 * s1 + s2 * s2 - cw * s1 * s2)); };
        auto fmv = render([](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 400.0f;
            s.atk = 0.002f; s.hold = 0.9f; s.dec = 0.5f;
            s.lfoRate[0] = 200.0f; s.lfoAmt[0] = 0.35f;          // audio-rate LFO (the new range)
            s.mod[0].src = DrumChannel::MSLfoFilt; s.mod[0].tgt = DrumChannel::MTPitch; s.mod[0].amt = 1.0f; }, 0.9f, 0.6);
        const double car = goe(fmv, 400.0), lo = goe(fmv, 200.0), hi = goe(fmv, 600.0);
        printf("[11] audio-rate FM: carrier=%.0f sidebands 200Hz=%.0f 600Hz=%.0f (expect sidebands > carrier*0.05) -> %s\n",
               car, lo, hi, CHK(lo > car * 0.05 && hi > car * 0.05 && finite(fmv)) ? "OK" : "FAIL");
    }
    {   // [12] KEY-tracked LFO (Sync = Key, ratio 1): the modulator follows the played pitch, so the
        //      upper sideband sits at 2 x f0 for ANY note (the FM colour is consistent across keys).
        auto goe = [&](const std::vector<float>& x, double f) {
            const double w = 2.0 * 3.14159265358979 * f / SR, cw = 2.0 * std::cos(w);
            double s0 = 0, s1 = 0, s2 = 0;
            for (size_t i = (size_t)(SR * 0.10); i < x.size() && i < (size_t)(SR * 0.5); ++i)
            { s0 = (double) x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::abs(s1 * s1 + s2 * s2 - cw * s1 * s2)); };
        auto mkK = [&](float baseHz) {
            return render([baseHz](DrumChannel& c){
                for (auto& sl : c.slots) sl = Slot();
                auto& s = c.slots[0];
                s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
                s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = baseHz;
                s.atk = 0.002f; s.hold = 0.9f; s.dec = 0.5f;
                s.lfoSync[0] = -2.0f; s.lfoRate[0] = 1.0f; s.lfoAmt[0] = 0.35f;   // KEY mode, ratio x1
                s.mod[0].src = DrumChannel::MSLfoFilt; s.mod[0].tgt = DrumChannel::MTPitch; s.mod[0].amt = 1.0f; }, 0.9f, 0.6); };
        auto a = mkK(300.0f); auto b = mkK(450.0f);
        const double sbA = goe(a, 600.0), carA = goe(a, 300.0);
        const double sbB = goe(b, 900.0), carB = goe(b, 450.0);
        printf("[12] KEY-tracked FM: 300Hz note 2f=%.0f (car %.0f), 450Hz note 2f=%.0f (car %.0f) (2f > car*0.05 both) -> %s\n",
               sbA, carA, sbB, carB, CHK(sbA > carA * 0.05 && sbB > carB * 0.05 && finite(a) && finite(b)) ? "OK" : "FAIL");
    }
    {   // [13] BIPOLAR BELL: +9 dB boost vs -9 dB cut at the same frequency must differ audibly and
        //      both must differ from flat (gain 0 with the filter ON ~ flat response).
        auto mkB = [&](float gainDb) {
            return render([gainDb](DrumChannel& c){ voice(c);
                c.slots[0].filterType2 = DrumChannel::Bell; c.slots[0].filterCutoff2 = 400.0f;
                c.slots[0].filterGain2 = gainDb; c.slots[0].filterReso2 = 1.0f; }, 0.9f, 0.5); };
        auto up = mkB(9.0f); auto dn = mkB(-9.0f); auto fl = mkB(0.0f);
        const float ud = maxdiff(up, dn), uf = maxdiff(up, fl), df = maxdiff(dn, fl);
        printf("[13] bipolar Bell: boost-vs-cut=%.3f boost-vs-flat=%.3f cut-vs-flat=%.3f (all >0.01) -> %s\n",
               ud, uf, df, CHK(ud > 0.01f && uf > 0.01f && df > 0.01f && finite(up) && finite(dn)) ? "OK" : "FAIL");
    }
    {   // [14] FREQUENCY SHIFTER quality: a 400 Hz sine shifted up ~+100 Hz must land at 500 Hz with
        //      the 300 Hz IMAGE rejected (that's what makes it true single-sideband, not ring mod).
        auto goe = [&](const std::vector<float>& x, double f2) {
            const double w = 2.0 * 3.14159265358979 * f2 / SR, cw = 2.0 * std::cos(w);
            double s0 = 0, s1 = 0, s2 = 0;
            for (size_t i = (size_t)(SR * 0.10); i < x.size() && i < (size_t)(SR * 0.5); ++i)
            { s0 = (double) x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::abs(s1 * s1 + s2 * s2 - cw * s1 * s2)); };
        // Character c: shift = 1500^|2c-1| - 1 -> +100 Hz at c = 0.5 + log(101)/log(1500)/2 = 0.8156
        auto sh = render([](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 400.0f;
            s.atk = 0.002f; s.hold = 0.9f; s.dec = 0.5f;
            c.chFxType[0] = DrumChannel::ChFxFreqShift; c.chFxAmt[0] = 1.0f; c.chFxChar[0] = 0.8156f; }, 0.9f, 0.6);
        const double at500 = goe(sh, 500.0), at400 = goe(sh, 400.0), at300 = goe(sh, 300.0);
        printf("[14] FreqShift SSB: 500Hz=%.0f (target), 400Hz=%.0f (dry residue), 300Hz=%.0f (image; expect target > 4x image) -> %s\n",
               at500, at400, at300, CHK(at500 > 4.0 * juce::jmax(1.0, at300) && finite(sh)) ? "OK" : "FAIL");
    }
    {   // [15] ENV-TIME targets are LATCHED PER NOTE (crackle fix): a fast LFO -> Attack must NOT
        //      step the running envelope (max sample delta stays waveform-sized - pre-fix the level
        //      jumped at every block edge), and a FREE slow LFO -> Attack gives each HIT its own
        //      attack (the per-hit-variation semantic the latch buys).
        auto fast = render([](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 110.0f;
            s.atk = 0.15f; s.hold = 0.3f; s.dec = 0.5f;
            s.lfoRate[0] = 200.0f; s.lfoAmt[0] = 1.0f;
            s.mod[0].src = DrumChannel::MSLfoFilt; s.mod[0].tgt = DrumChannel::MTAtk; s.mod[0].amt = 1.0f; }, 0.9f, 0.8);
        float md = 0; for (size_t i = 1; i < fast.size(); ++i) md = std::max(md, std::abs(fast[i] - fast[i - 1]));
        auto two = render([](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 110.0f;
            s.atk = 0.30f; s.hold = 0.1f; s.dec = 0.4f;
            s.lfoRate[0] = 0.7f; s.lfoAmt[0] = 1.0f; s.lfoFree[0] = true;
            s.mod[0].src = DrumChannel::MSLfoFilt; s.mod[0].tgt = DrumChannel::MTAtk; s.mod[0].amt = 0.8f;
            c.steps[4] = true; c.stepVel[4] = 0.9f; }, 0.9f, 1.4);
        auto win = [&](double t0){ double e = 0; int n = 0;
            for (size_t i = (size_t)(t0 * SR); i < (size_t)((t0 + 0.06) * SR) && i < two.size(); ++i)
            { e += (double) two[i] * two[i]; ++n; }
            return std::sqrt(e / std::max(1, n)); };
        const double e1 = win(0.0), e2 = win(1.0);
        printf("[15] env-latch: fast-LFO->Attack max delta=%.4f (<0.02), per-hit attack rms %.4f vs %.4f (differ >1.7x) -> %s\n",
               md, e1, e2, CHK(md < 0.02f && (e2 > e1 * 1.7 || e1 > e2 * 1.7) && finite(fast) && finite(two)) ? "OK" : "FAIL");
    }
    {   // [16] DE-ZIPPER BANK: fast-LFO block-rate modulation of the signal-path amounts (Drive /
        //      Sub / Warp probed) must not STEP the output. The honest bound = each probe's own
        //      STATIC render at the extreme amount the sweep reaches (a hard-driven / folded sine
        //      has legitimately steep edges) - modulated max-delta may not exceed static x1.3.
        auto base = [](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 110.0f;
            s.atk = 0.005f; s.hold = 0.6f; s.dec = 0.3f; };
        auto mkMod = [base](int tgt, float baseDrv) { return [base, tgt, baseDrv](DrumChannel& c){
            base(c); auto& s = c.slots[0];
            s.fxDrive = baseDrv; s.fxDriveType = DrumChannel::Tube;
            s.lfoRate[0] = 90.0f; s.lfoAmt[0] = 1.0f;
            s.mod[0].src = DrumChannel::MSLfoFilt; s.mod[0].tgt = (int8_t) tgt; s.mod[0].amt = 0.8f; }; };
        auto delta = [](const std::vector<float>& x){ float m = 0;
            for (size_t i = 1; i < x.size(); ++i) m = std::max(m, std::abs(x[i] - x[i - 1])); return m; };
        const float dDrv = delta(render(mkMod(DrumChannel::MTDrive, 0.3f), 0.9f, 0.7));
        const float dSub = delta(render(mkMod(DrumChannel::MTSub,   0.0f), 0.9f, 0.7));
        const float dWrp = delta(render(mkMod(DrumChannel::MTWarp,  0.0f), 0.9f, 0.7));
        const float bDrv = delta(render([base](DrumChannel& c){ base(c); c.slots[0].fxDrive = 1.0f; c.slots[0].fxDriveType = DrumChannel::Tube; }, 0.9f, 0.7));
        const float bSub = delta(render([base](DrumChannel& c){ base(c); c.slots[0].fxSub = 0.8f; }, 0.9f, 0.7));
        const float bWrp = delta(render([base](DrumChannel& c){ base(c); c.slots[0].oscWarp = 0.8f; }, 0.9f, 0.7));
        printf("[16] de-zipper: mod-vs-static max deltas drive %.4f/%.4f sub %.4f/%.4f warp %.4f/%.4f (mod < static*1.3+0.005) -> %s\n",
               dDrv, bDrv, dSub, bSub, dWrp, bWrp,
               CHK(dDrv < bDrv * 1.3f + 0.005f && dSub < bSub * 1.3f + 0.005f && dWrp < bWrp * 1.3f + 0.005f) ? "OK" : "FAIL");
    }
    {   // [17] ADAA anti-aliasing: a 5 kHz sine through FULL SoftClip drive. tanh's 19th harmonic
        //      (95 kHz) folds to 1 kHz at the 2x engine rate - the NAIVE shaper (applied in-test to
        //      a clean render) leaks it loudly; the engine's ADAA path must cut that alias hard
        //      while keeping the wanted 15 kHz 3rd harmonic.
        auto tone = [](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 5000.0f;
            s.atk = 0.003f; s.hold = 0.5f; s.dec = 0.2f; };
        auto clean = render(tone, 0.9f, 0.5);
        auto adaa  = render([tone](DrumChannel& c){ tone(c); c.slots[0].fxDrive = 1.0f; c.slots[0].fxDriveType = DrumChannel::SoftClip; }, 0.9f, 0.5);
        std::vector<float> naive(clean.size());
        for (size_t i2 = 0; i2 < clean.size(); ++i2)
            naive[i2] = std::tanh(clean[i2] * 25.0f) * 0.25f;   // the exact pre-ADAA math at HOST rate
        auto goe = [&](const std::vector<float>& x, double f) {
            const double w = 2.0 * 3.14159265358979 * f / SR, cw = 2.0 * std::cos(w);
            double s0 = 0, s1 = 0, s2 = 0;
            for (size_t i2 = (size_t)(SR * 0.08); i2 < x.size() && i2 < (size_t)(SR * 0.45); ++i2)
            { s0 = (double) x[i2] + cw * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::abs(s1 * s1 + s2 * s2 - cw * s1 * s2)); };
        const double aliasN = goe(naive, 1000.0), aliasA = goe(adaa, 1000.0);
        const double harmA  = goe(adaa, 15000.0);
        printf("[17] ADAA: 1kHz alias naive=%.1f adaa=%.1f (expect adaa < naive*0.6), 15k harmonic=%.1f (>alias) -> %s\n",
               aliasN, aliasA, harmA,
               CHK(aliasA < aliasN * 0.6 && harmA > aliasA && finite(adaa)) ? "OK" : "FAIL");
    }
    {   // [18] MPE PRESSURE: a held key voice tagged with MIDI channel 3; pressure fanned onto
        //      channel 3 must move a Pressure->Cutoff route audibly; the SAME pressure on channel 5
        //      must not touch it (per-channel voice matching = the MPE contract).
        auto run = [&](int pressChan) {
            auto* q = new Sequencer();
            q->setStandaloneBpm(120.0f);
            auto& c = q->patterns[0].channels[0];
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSaw; s.oscFreq = 110.0f;
            s.atk = 0.005f; s.hold = 0.1f; s.dec = 0.4f; s.sustain = 0.8f; s.release = 0.2f;
            s.filterType = DrumChannel::LowPass; s.filterCutoff = 350.0f; s.filterReso = 2.0f;
            s.mod[0].src = DrumChannel::MSPressure; s.mod[0].tgt = DrumChannel::MTFilt1Cut; s.mod[0].amt = 1.0f;
            for (auto& p2 : q->patterns) for (auto& c2 : p2.channels) c2.prepareToPlay(SR, BS);
            c.keyDown(60, 0.9f, 0, true, 0, 3);            // held key on MIDI channel 3
            std::vector<float> out;
            juce::AudioBuffer<float> buf(2, BS);
            for (int b = 0; b < (int)(0.5 * SR / BS); ++b)
            {
                if (b == 8 && pressChan > 0) c.applyExpression(0, pressChan, 1.0f);   // full pressure
                buf.clear(); q->processBlock(buf, SR, BS, nullptr);
                for (int i2 = 0; i2 < BS; ++i2) out.push_back(buf.getSample(0, i2));
            }
            delete q; return out;
        };
        auto base = run(0), hit = run(3), miss = run(5);
        const float dHit = maxdiff(base, hit), dMiss = maxdiff(base, miss);
        printf("[18] MPE pressure: ch3 press diff=%.4f (>0.02), wrong-ch diff=%.4f (==0) -> %s\n",
               dHit, dMiss, CHK(dHit > 0.02f && dMiss == 0.0f && finite(hit)) ? "OK" : "FAIL");
    }
    {   // [19] PER-ROUTE REMAP: a GATE curve (0 below half, 1 above) on Velocity -> Cutoff. A soft
        //      hit (vel 0.4) through the gate must be BIT-IDENTICAL to no route at all (the curve
        //      outputs exactly 0); a hard hit (vel 0.95) must still modulate. Locks the remap math
        //      through both the block AND per-sample paths.
        auto mkR = [](bool route, bool gate) { return [route, gate](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSaw; s.oscFreq = 110.0f;
            s.atk = 0.003f; s.hold = 0.3f; s.dec = 0.3f;
            s.filterType = DrumChannel::LowPass; s.filterCutoff = 300.0f; s.filterReso = 2.0f;
            if (route) {
                s.mod[0].src = DrumChannel::MSVel; s.mod[0].tgt = DrumChannel::MTFilt1Cut; s.mod[0].amt = 1.0f;
                if (gate) { s.mod[0].curveOn = 1;
                            for (int k = 0; k < Slot::MOD_CURVE_N; ++k) s.mod[0].curve[k] = k < Slot::MOD_CURVE_N / 2 ? 0 : 255; }
            } }; };
        auto softNo  = render(mkR(false, false), 0.4f, 0.5);
        auto softGt  = render(mkR(true,  true ), 0.4f, 0.5);
        auto hardNo  = render(mkR(false, false), 0.95f, 0.5);
        auto hardGt  = render(mkR(true,  true ), 0.95f, 0.5);
        const float dSoft = maxdiff(softNo, softGt), dHard = maxdiff(hardNo, hardGt);
        printf("[19] remap gate: soft-vel diff=%.6f (==0, curve outputs 0), hard-vel diff=%.4f (>0.02) -> %s\n",
               dSoft, dHard, CHK(dSoft == 0.0f && dHard > 0.02f && finite(softGt) && finite(hardGt)) ? "OK" : "FAIL");
    }
    {   // [20] UNISON-16 CANCELLATION (user find): 16 voices at detune 0 used to phase-cancel to
        //      SILENCE (full-circle even spread sums to zero). The half-cycle spread must keep the
        //      stack at a healthy fraction of a single voice's level.
        auto uniN = [](int n) { return [n](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSaw; s.oscFreq = 110.0f;
            s.atk = 0.003f; s.hold = 0.4f; s.dec = 0.3f;
            s.oscUnison = n; s.oscDetune = 0.0f; }; };
        const double r1 = rms(render(uniN(1), 0.9f, 0.5)), r16 = rms(render(uniN(16), 0.9f, 0.5));
        printf("[20] unison-16 detune-0: rms %.4f vs single %.4f (expect >0.3x, was ~0 = silent) -> %s\n",
               r16, r1, CHK(r16 > r1 * 0.3) ? "OK" : "FAIL");
    }
    {   // [21] CONTINUOUS COUNT-MORPH (supersedes the brief per-hit latch): an LFO swelling the
        //      unison count mid-note must be SMOOTH (per-voice fades - no gain steps/pops), audibly
        //      different from unrouted, and a 2000 Hz LFO abuse case must stay finite + smooth
        //      (block-rate sampling aliases it to flutter; the fades bound every transition).
        auto mk21 = [](float lfoHz, float amt) { return [lfoHz, amt](DrumChannel& c){
            for (auto& sl : c.slots) sl = Slot();
            auto& s = c.slots[0];
            s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
            s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 110.0f;
            s.atk = 0.005f; s.hold = 0.6f; s.dec = 0.3f;
            s.oscUnison = 2; s.oscDetune = 0.25f;
            if (amt > 0.0f) {
                s.lfoRate[0] = lfoHz; s.lfoAmt[0] = 1.0f; s.lfoFree[0] = true;
                s.mod[0].src = DrumChannel::MSLfoFilt; s.mod[0].tgt = DrumChannel::MTUniCount; s.mod[0].amt = amt; } }; };
        auto delta = [](const std::vector<float>& x){ float m = 0;
            for (size_t i = 1; i < x.size(); ++i) m = std::max(m, std::abs(x[i] - x[i - 1])); return m; };
        auto plainR = render(mk21(0.0f, 0.0f), 0.9f, 0.7);
        auto morph  = render(mk21(2.0f, 1.0f), 0.9f, 0.7);
        auto abuse  = render(mk21(2000.0f, 1.0f), 0.9f, 0.7);
        printf("[21] count-morph: audible diff=%.3f (>0.05), smooth delta=%.4f / abuse-2kHz delta=%.4f (both <0.06, no steps) -> %s\n",
               maxdiff(plainR, morph), delta(morph), delta(abuse),
               CHK(maxdiff(plainR, morph) > 0.05f && delta(morph) < 0.06f && delta(abuse) < 0.06f
                   && finite(morph) && finite(abuse)) ? "OK" : "FAIL");
    }
    {   // [22] MONO RETRIGGER HANDOVER (the user's bass-roll crackle, DT770-verified): a 45 Hz
        //      long-decay sine retriggered every step while its tail is still LOUD. The old path
        //      hard-reused voice 0 mid-ring = a discontinuity (~1.4 peak delta); the pitch-aware
        //      handover must keep every sample step waveform-sized.
        auto* q = new Sequencer();
        q->setStandaloneBpm(120.0f);
        auto& c = q->patterns[0].channels[0];
        for (auto& sl : c.slots) sl = Slot();
        auto& s = c.slots[0];
        s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
        s.oscShape = s.oscShapeB = DrumChannel::WvSine; s.oscFreq = 45.0f;
        s.atk = 0.003f; s.hold = 0.0f; s.dec = 2.0f;
        c.numSteps = 8;
        for (int st = 0; st < 8; ++st) { c.steps[st] = true; c.stepVel[st] = 0.9f; }
        for (auto& p2 : q->patterns) for (auto& c2 : p2.channels) c2.prepareToPlay(SR, BS);
        q->startStandalone();
        std::vector<float> out;
        juce::AudioBuffer<float> buf(2, BS);
        for (int b = 0; b < (int)(2.0 * SR / BS); ++b)
        { buf.clear(); q->processBlock(buf, SR, BS, nullptr); for (int i2 = 0; i2 < BS; ++i2) out.push_back(buf.getSample(0, i2)); }
        delete q;
        float md = 0; for (size_t i2 = 1; i2 < out.size(); ++i2) md = std::max(md, std::abs(out[i2] - out[i2 - 1]));
        // CROSS-CHANNEL CHOKE (user: "make sure it works when another channel chokes too"): a loud
        // ringing 45 Hz sub on ch 0 is choked mid-ring by a soft hit on ch 1 (same choke group via
        // the Routing option) - the pitch-aware fade must keep that cut click-free as well.
        auto* q2 = new Sequencer();
        q2->setStandaloneBpm(120.0f);
        auto& ca = q2->patterns[0].channels[0];
        auto& cb = q2->patterns[0].channels[1];
        for (auto& sl : ca.slots) sl = Slot();
        for (auto& sl : cb.slots) sl = Slot();
        auto& sa = ca.slots[0];
        sa.engine = DrumChannel::SrcOsc; sa.weight = 1.0f;
        sa.oscShape = sa.oscShapeB = DrumChannel::WvSine; sa.oscFreq = 45.0f;
        sa.atk = 0.003f; sa.hold = 0.0f; sa.dec = 2.0f;
        auto& sb = cb.slots[0];
        sb.engine = DrumChannel::SrcOsc; sb.weight = 1.0f;
        sb.oscShape = sb.oscShapeB = DrumChannel::WvSine; sb.oscFreq = 200.0f;
        sb.atk = 0.003f; sb.hold = 0.0f; sb.dec = 0.2f;
        ca.chokeGroup = 1; cb.chokeGroup = 1;
        ca.numSteps = 8; ca.steps[0] = true; ca.stepVel[0] = 0.9f;
        cb.numSteps = 8; cb.steps[4] = true; cb.stepVel[4] = 0.3f;   // chokes the sub at 1.0 s, mid-ring
        for (auto& p2 : q2->patterns) for (auto& c2 : p2.channels) c2.prepareToPlay(SR, BS);
        q2->startStandalone();
        std::vector<float> out2;
        for (int b = 0; b < (int)(1.6 * SR / BS); ++b)
        { buf.clear(); q2->processBlock(buf, SR, BS, nullptr); for (int i2 = 0; i2 < BS; ++i2) out2.push_back(buf.getSample(0, i2)); }
        delete q2;
        float md2 = 0; for (size_t i2 = 1; i2 < out2.size(); ++i2) md2 = std::max(md2, std::abs(out2[i2] - out2[i2 - 1]));
        printf("[22] bass retrigger: max delta=%.4f / cross-channel choke delta=%.4f (both <0.03; hard cuts were ~1+), rms=%.3f -> %s\n",
               md, md2, rms(out), CHK(md < 0.03f && md2 < 0.03f && rms(out) > 0.1 && finite(out) && finite(out2)) ? "OK" : "FAIL");
    }
    {   // [23] OTT AUDIBILITY (round-1 shipped inaudibly polite - user: "no difference at all"):
        //      a decaying tone's LATE TAIL must come UP hard at full depth (upward compression =
        //      the thing a downward comp can't do), and the overall tone must clearly change.
        auto mkDecay = [&](bool ott) {
            return render([ott](DrumChannel& c){
                for (auto& sl : c.slots) sl = Slot();
                auto& s = c.slots[0];
                s.engine = DrumChannel::SrcOsc; s.weight = 1.0f;
                s.oscShape = s.oscShapeB = DrumChannel::WvSaw; s.oscFreq = 220.0f;
                s.atk = 0.002f; s.dec = 0.5f;                       // fast natural fade = a quiet tail
                if (ott) { c.chFxType[0] = DrumChannel::ChFxOtt; c.chFxAmt[0] = 1.0f; c.chFxChar[0] = 0.5f; }
            }, 0.9f, 1.2); };
        auto dry3 = mkDecay(false), wet3 = mkDecay(true);
        auto tailRms = [&](const std::vector<float>& x) {           // 0.6..1.1 s = deep into the decay
            double e = 0; size_t n = 0;
            for (size_t i2 = (size_t)(0.6 * SR); i2 < x.size() && i2 < (size_t)(1.1 * SR); ++i2) { e += (double) x[i2] * x[i2]; ++n; }
            return std::sqrt(e / (double) juce::jmax((size_t) 1, n)); };
        const double tDry = tailRms(dry3), tWet = tailRms(wet3);
        const double lift = tWet / juce::jmax(1.0e-9, tDry);
        printf("[23] OTT: tail lift x%.1f (expect >4 = the upward-compression resurrection), tone maxdiff=%.3f -> %s\n",
               lift, maxdiff(dry3, wet3),
               CHK(lift > 4.0 && maxdiff(dry3, wet3) > 0.05f && finite(wet3)) ? "OK" : "FAIL");
    }
    {   // [24] FREE LFO routes work on a LIVE key with the transport STOPPED [2026-07-16]: the
        //     stopped render loop used to feed lfoBarPos = jmax(0, parked barPosition) = "bar 0"
        //     every block, which RESET the free-run clock per block = free LFOs frozen at phase 0
        //     (the "Sequencer Bass does nothing live" user report). Fixed: stopped = the -1
        //     sentinel -> the lfoFreeSec wall clock drives them. Retrig was always fine.
        auto liveVar = [&](bool freeRun) {
            auto* q2 = new Sequencer();
            q2->setStandaloneBpm(120.0f);
            auto& c2 = q2->patterns[0].channels[0];
            for (auto& sl2 : c2.slots) sl2 = DrumChannel::Slot();
            auto& sl2 = c2.slots[0];
            sl2.engine = DrumChannel::SrcOsc; sl2.weight = 1.0f;
            sl2.oscShape = sl2.oscShapeB = DrumChannel::WvSine; sl2.oscFreq = 110.0f;
            sl2.atk = 0.002f; sl2.dec = 0.5f; sl2.sustain = 0.9f; sl2.release = 0.05f;
            sl2.lfoShape[0] = 0; sl2.lfoSync[0] = 1.0f; sl2.lfoAmt[0] = 1.0f; sl2.lfoFree[0] = freeRun;
            sl2.mod[0] = { (int8_t) DrumChannel::MSLfoFilt, (int8_t) DrumChannel::MTVol, 0.9f };
            for (auto& p2 : q2->patterns) for (auto& cc : p2.channels) cc.prepareToPlay(SR, 480);
            c2.keyDown(45, 1.0f, 0, c2.keysPolyMode);           // held live; transport NEVER started
            std::vector<float> o2; juce::AudioBuffer<float> b2(2, 480);
            for (int bl = 0; bl < (int)(2.2 * SR / 480); ++bl)
            { b2.clear(); q2->processBlock(b2, SR, 480, nullptr); for (int i2 = 0; i2 < 480; ++i2) o2.push_back(b2.getSample(0, i2)); }
            delete q2;
            double mn = 1.0e9, mx = 0.0;                        // tremolo depth across 16th windows
            for (int w2 = 0; w2 < 14; ++w2)
            {   const double t0 = 0.15 + w2 * 0.125; double e = 0; int n = 0;
                for (int i2 = (int)(t0 * SR); i2 < (int)((t0 + 0.11) * SR) && i2 < (int) o2.size(); ++i2) { e += (double) o2[(size_t) i2] * o2[(size_t) i2]; ++n; }
                const double r = std::sqrt(e / juce::jmax(1, n)); mn = juce::jmin(mn, r); mx = juce::jmax(mx, r); }
            return mx / juce::jmax(1.0e-9, mn);
        };
        const double fr = liveVar(true), rt = liveVar(false);
        printf("[24] FREE LFO live (stopped transport): tremolo depth free=x%.2f retrig=x%.2f (both >1.8) -> %s\n",
               fr, rt, CHK(fr > 1.8 && rt > 1.8) ? "OK" : "FAIL");
    }
    {   // [25] CHANNEL FILTER/EQ [2026-07-16]: the post-FX pair on the finished channel.
        //     (a) all Off = bit-identical; (b) LP 300 kills a 110 Hz saw's 8th harmonic; (c) a
        //     free-LFO "(Channel)" cutoff route makes the output differ from the static filter.
        auto rendCh = [&](int mode) {   // 0 = off, 1 = LP 300, 2 = LP 300 + LFO->cutoff route
            auto* q2 = new Sequencer(); q2->setStandaloneBpm(120.0f);
            auto& c2 = q2->patterns[0].channels[0];
            for (auto& sl2 : c2.slots) sl2 = DrumChannel::Slot();
            auto& sl2 = c2.slots[0];
            sl2.engine = DrumChannel::SrcOsc; sl2.weight = 1.0f;
            sl2.oscShape = sl2.oscShapeB = DrumChannel::WvSaw; sl2.oscFreq = 110.0f;
            sl2.atk = 0.002f; sl2.dec = 0.5f; sl2.sustain = 0.9f; sl2.release = 0.05f;
            if (mode >= 1) { c2.chFiltType[0] = DrumChannel::LowPass; c2.chFiltCutoff[0] = 300.0f; }
            if (mode == 2)
            {   sl2.lfoShape[0] = 0; sl2.lfoSync[0] = 2.0f; sl2.lfoAmt[0] = 1.0f; sl2.lfoFree[0] = true;
                sl2.mod[0] = { (int8_t) DrumChannel::MSLfoFilt, (int8_t) DrumChannel::MTChFilt1Cut, 0.7f }; }
            for (auto& p2 : q2->patterns) for (auto& cc : p2.channels) cc.prepareToPlay(SR, 480);
            c2.keyDown(45, 1.0f, 0, c2.keysPolyMode);
            std::vector<float> o2; juce::AudioBuffer<float> b2(2, 480);
            for (int bl = 0; bl < (int)(1.2 * SR / 480); ++bl)
            { b2.clear(); q2->processBlock(b2, SR, 480, nullptr); for (int i2 = 0; i2 < 480; ++i2) o2.push_back(b2.getSample(0, i2)); }
            delete q2; return o2;
        };
        auto off1 = rendCh(0), off2 = rendCh(0), lp = rendCh(1), md = rendCh(2);
        auto gzE = [&](const std::vector<float>& x, double f) {
            const double w = 2.0 * M_PI * f / SR, cf = 2.0 * std::cos(w);
            double s1 = 0, s2 = 0; const int a0 = (int)(0.3 * SR), n = (int)(0.5 * SR);
            for (int i2 = a0; i2 < a0 + n && i2 < (int) x.size(); ++i2) { const double s0 = x[(size_t) i2] + cf * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - cf * s1 * s2)) / (0.5 * n); };
        const double h8off = gzE(off1, 880.0), h8lp = gzE(lp, 880.0);
        printf("[25] channel filter: off repeat maxdiff=%.6f | h8 off=%.4f lp300=%.4f (x%.0f down) | LFO route vs static maxdiff=%.3f -> %s\n",
               maxdiff(off1, off2), h8off, h8lp, h8lp > 1e-9 ? h8off / h8lp : 999.0, maxdiff(lp, md),
               CHK(maxdiff(off1, off2) < 1.0e-9f && h8off > 8.0 * (h8lp + 1e-9) && maxdiff(lp, md) > 0.01f && finite(lp)) ? "OK" : "FAIL");
    }
    printf(fails == 0 ? ">>> ModMatrixTest PASS\n" : ">>> ModMatrixTest FAIL (%d)\n", fails);
    return fails;
}
