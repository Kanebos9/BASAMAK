// VoiceMapTest [2026-07-20] - locks the SYLLABLE VOICE multisample model:
//  [1] cyclic pitch->voice mapping (note mod nVoices picks the "voice N" subfolder)
//  [2] sidecar <tune> cents compensation ("auto-tuned by construction": a flat recording plays in tune)
//  [3] voiced instruments default Auto Loop ON and the loops are derived on the NEW set
//      (regression for the rebuild-before-swap bug found 2026-07-20)
#include "Sequencer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static double goertzel(const std::vector<float>& x, size_t a, size_t b, double f, double sr) {
    const double w = 2.0*M_PI*f/sr, c = 2*std::cos(w), sw = std::sin(w), cw = std::cos(w);
    double s1=0,s2=0; size_t n=0;
    for (size_t i=a; i<b && i<x.size(); ++i){ double s0=x[i]+c*s1-s2; s2=s1; s1=s0; ++n; }
    const double re=s1-s2*cw, im=s2*sw; return std::sqrt(re*re+im*im)/(0.5*(double)juce::jmax((size_t)1,n));
}

static bool writeSine(const juce::File& f, double hz, double sec, double sr)
{
    f.getParentDirectory().createDirectory();
    std::vector<float> d((size_t)(sec * sr));
    for (size_t i = 0; i < d.size(); ++i)
    {
        d[i] = 0.5f * (float) std::sin(2.0 * M_PI * hz * (double) i / sr);
        const size_t N = d.size();
        if (i < 480) d[i] *= (float) i / 480.0f;                       // 10 ms edges (no clicks)
        if (i + 480 > N) d[i] *= (float)(N - i) / 480.0f;
    }
    if (auto os = f.createOutputStream())
    {
        juce::WavAudioFormat wav;
        if (std::unique_ptr<juce::AudioFormatWriter> w { wav.createWriterFor(os.get(), sr, 1, 24, {}, 0) })
        { os.release(); const float* chans[1] = { d.data() }; w->writeFromFloatArrays(chans, 1, (int) d.size()); return true; }
    }
    return false;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI ji;
    int fails = 0;
    auto CHK = [&](bool ok){ if (!ok) ++fails; return ok; };
    const double SR = 96000.0; const int bs = 1024;
    const double C4 = 261.6256, Cs4 = 277.1826;   // note 60 / 61
    const double V2C4 = 523.2511;                 // voice 2's C4 file CONTENT (an octave up = distinguishable)
    const double V2at61 = V2C4 * std::pow(2.0, 1.0 / 12.0);   // voice 2 zone varispeeded to note 61

    // build the instrument on disk: voice 1 = C4 recording that is 100 CENTS FLAT (content B3),
    // fixed by the sidecar tune entry; voice 2 = in-tune content an octave up (the marker).
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("BasamakVoiceMapTest");
    dir.deleteRecursively();
    const double flatC4 = C4 * std::pow(2.0, -100.0 / 1200.0);
    bool wrote = writeSine(dir.getChildFile("voice 1").getChildFile("C4.wav"), flatC4, 0.4, 48000.0)
              && writeSine(dir.getChildFile("voice 2").getChildFile("C4.wav"), V2C4,  0.4, 48000.0);
    {
        juce::ValueTree t("instrument");
        juce::ValueTree tu("tune"); tu.setProperty("voice", 0, nullptr);
        tu.setProperty("root", 60, nullptr); tu.setProperty("cents", -100.0f, nullptr);
        t.addChild(tu, -1, nullptr);
        // [2026-07-20] the instrument's OWN envelope (user design: instruments arrive finished)
        t.setProperty("envOn", true, nullptr);
        t.setProperty("envA", 0.02f, nullptr); t.setProperty("envS", 0.75f, nullptr);
        t.setProperty("envR", 0.4f, nullptr);
        dir.getChildFile("instrument.basamakinst").replaceWithText(t.toXmlString());
    }
    printf("[0] fixture written: %s\n", wrote ? "OK" : "FAIL"); if (! wrote) return 1;

    auto* s = new Sequencer();
    s->setStandaloneBpm(120.0f);   // 1 bar = 2.0 s; DRAW_RES 384
    auto& ch = s->patterns[0].channels[0];
    for (auto& sl : ch.slots) sl = DrumChannel::Slot();
    ch.slots[0].engine = DrumChannel::SrcSample; ch.slots[0].weight = 1.0f;
    ch.drawMode = true;
    ch.addDrawNote(0,   88, 0, 255, 0);    // beat 1: note 60 -> 60 % 2 = voice 1 (in tune via cents)
    ch.addDrawNote(96,  88, 1, 255, 0);    // beat 2: note 61 -> voice 2 (octave-up marker, +1 st)
    ch.addDrawNote(192, 176, 0, 255, 0);   // beat 3-4: LONG note on a 0.4 s source = needs the Auto Loop
    for (auto& p : s->patterns) for (auto& c2 : p.channels) c2.prepareToPlay(SR, bs);
    const bool loaded = ch.loadMultisample(0, dir);
    printf("[0b] loadMultisample: %s (nVoices=%d, loopOn=%d)\n",
           loaded ? "OK" : "FAIL", ch.msSet[0] ? ch.msSet[0]->nVoices : 0, (int) ch.slots[0].msLoopOn);
    if (! CHK(loaded && ch.msSet[0] != nullptr && ch.msSet[0]->nVoices == 2 && ch.slots[0].msLoopOn)) { delete s; return 1; }

    s->startStandalone();
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, bs);
    for (int b = 0; b < (int)(2.0 * SR / bs) + 1; ++b)
    { buf.clear(); s->processBlock(buf, SR, bs, nullptr); for (int i = 0; i < bs; ++i) out.push_back(buf.getSample(0, i)); }
    auto W = [&](double t0, double t1, double f){ return goertzel(out, (size_t)(t0*SR), (size_t)(t1*SR), f, SR); };

    // [1] voice mapping: note 60 = voice 1 territory, note 61 = voice 2 territory
    const double a1 = W(0.08, 0.42, C4),     a2 = W(0.08, 0.42, V2C4);
    const double b1 = W(0.58, 0.92, V2at61), b2 = W(0.58, 0.92, Cs4);
    printf("[1] note60: v1(C4)=%.3f v2(oct)=%.3f | note61: v2=%.3f v1(C#4)=%.3f -> %s\n", a1, a2, b1, b2,
           CHK(a1 > 0.03 && a2 < a1 * 0.2 && b1 > 0.03 && b2 < b1 * 0.2) ? "cyclic mapping OK" : "FAIL");

    // [2] cents auto-tune: the flat recording must play at TRUE C4, not its raw B3-ish pitch
    const double raw = W(0.08, 0.42, flatC4);
    printf("[2] tuned C4=%.3f raw(flat)=%.3f -> %s\n", a1, raw,
           CHK(a1 > 0.03 && raw < a1 * 0.25) ? "cents compensated OK" : "FAIL");

    // [3] auto loop on a fresh voiced load: the 0.4 s source must still sound ~0.6-0.85 s into the note
    const double late = W(1.62, 1.86, C4);
    printf("[3] held-note energy late in a short source: %.3f -> %s\n", late,
           CHK(late > 0.02) ? "auto loop sustains (OK)" : "FAIL (loop dead on fresh load)");

    // [4] the sidecar ENVELOPE travelled with the instrument (envOn + the authored values)
    const auto& es = ch.slots[0];
    printf("[4] sidecar env: on=%d A=%.3f S=%.2f R=%.2f -> %s\n",
           (int) es.smpEnvOn, es.atk, es.sustain, es.release,
           CHK(es.smpEnvOn && std::abs(es.atk - 0.02f) < 1e-3 && std::abs(es.sustain - 0.75f) < 1e-3
               && std::abs(es.release - 0.4f) < 1e-3) ? "instrument env applied (OK)" : "FAIL");

    // [5] SHARED DECODE (the RAM fix): the same folder on a second channel shares ONE MsSet
    {
        auto& ch2 = s->patterns[0].channels[1];
        for (auto& sl : ch2.slots) sl = DrumChannel::Slot();
        ch2.slots[0].engine = DrumChannel::SrcSample; ch2.slots[0].weight = 1.0f;
        const bool l2 = ch2.loadMultisample(0, dir);
        printf("[5] shared decode: loaded=%d same-set=%d -> %s\n",
               (int) l2, (int)(ch2.msSet[0] == ch.msSet[0]),
               CHK(l2 && ch2.msSet[0] != nullptr && ch2.msSet[0] == ch.msSet[0])
                   ? "one decode shared (OK)" : "FAIL (duplicate decode)");
    }

    dir.deleteRecursively();
    delete s;
    printf(fails == 0 ? "VoiceMapTest: ALL PASS\n" : "VoiceMapTest: %d FAILURES\n", fails);
    return fails;
}
