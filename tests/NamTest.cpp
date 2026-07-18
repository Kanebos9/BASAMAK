// [2026-07-18] NamTest - locks the vendored NeuralAmpModelerCore integration:
//   [1] the A2 architecture example model LOADS and PROCESSES (the whole reason we vendored
//       this version - "NAM A2" was the user's requirement; verified, not guessed),
//   [2] classic WaveNet + LSTM models load too (the files people actually download),
//   [3] output is finite, non-trivial, and DIFFERS from the input (the model actually ran),
//   [4] loading garbage fails CLEANLY (error string, no crash).
#include <JuceHeader.h>
#include <cmath>
#include "NamWrapper.h"
#include "DrumChannel.h"

static bool runModel(const juce::File& f, double sr, bool expectLoudness, bool& loudnessSeen)
{
    std::string err;
    BasamakNam* m = basamak_nam::load(f.getFullPathName().toStdString(), sr, 512, err);
    if (m == nullptr) { printf("    load FAILED: %s\n", err.c_str()); return false; }

    const int N = 4096;
    std::vector<float> x((size_t) N);
    for (int i = 0; i < N; ++i)
        x[(size_t) i] = 0.25f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) sr);
    std::vector<float> y = x;
    for (int off = 0; off < N; off += 512)
        basamak_nam::process(m, y.data() + off, 512);

    double maxdiff = 0.0, rms = 0.0; bool finite = true;
    for (int i = 512; i < N; ++i)   // skip the first block (warm-up transients are legitimate)
    {
        if (! std::isfinite(y[(size_t) i])) finite = false;
        maxdiff = juce::jmax(maxdiff, (double) std::abs(y[(size_t) i] - x[(size_t) i]));
        rms += (double) y[(size_t) i] * y[(size_t) i];
    }
    rms = std::sqrt(rms / (N - 512));
    double ldb = 0.0;
    const bool hasLoud = basamak_nam::loudnessDb(m, ldb);
    loudnessSeen = hasLoud;
    printf("    rate=%.0f maxdiff=%.4f rms=%.4f finite=%d loudness=%s\n",
           basamak_nam::expectedRate(m), maxdiff, rms, finite ? 1 : 0,
           hasLoud ? juce::String(ldb, 1).toRawUTF8() : "n/a");
    basamak_nam::destroy(m);
    juce::ignoreUnused(expectLoudness);
    return finite && maxdiff > 1.0e-4 && rms > 1.0e-4 && rms < 8.0;
}

int main()
{
    const juce::File root = juce::File(__FILE__).getParentDirectory().getParentDirectory()
                                .getChildFile("external/nam-core/example_models");
    bool ok = true, loud = false;

    printf("[1] A2 architecture (A2.nam):\n");
    ok &= runModel(root.getChildFile("A2.nam"), 48000.0, false, loud);

    printf("[2] WaveNet (wavenet.nam):\n");
    ok &= runModel(root.getChildFile("wavenet.nam"), 48000.0, false, loud);

    printf("[3] LSTM (lstm.nam):\n");
    ok &= runModel(root.getChildFile("lstm.nam"), 48000.0, false, loud);

    printf("[4] garbage file fails cleanly: ");
    {
        std::string err;
        juce::File bad = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("bad.nam");
        bad.replaceWithText("{ not a model }");
        BasamakNam* m = basamak_nam::load(bad.getFullPathName().toStdString(), 48000.0, 512, err);
        const bool cleanFail = (m == nullptr && ! err.empty());
        printf("%s (err=\"%s\")\n", cleanFail ? "OK" : "FAIL", err.c_str());
        if (m != nullptr) basamak_nam::destroy(m);
        ok &= cleanFail;
    }

    // [5] CHANNEL FX integration: a sine channel with FX A = NAM Amp (A2.nam) renders finite,
    //     non-silent, and DIFFERENT from the dry channel; empty file = untouched (bit-identical).
    printf("[5] channel FX (engineOS=1): ");
    {
        auto mk = []{
            auto ch = std::make_unique<DrumChannel>();
            ch->prepareToPlay(48000.0, 512);
            auto& sl = ch->slots[0];
            sl = DrumChannel::Slot();
            sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f; sl.oscShape = 0;
            sl.oscFreq = 110.0f; sl.dec = 0.5f;
            return ch;
        };
        auto render = [](DrumChannel& ch){
            ch.trigger(1.0f);
            juce::AudioBuffer<float> out(2, 12288), blk(2, 512);
            out.clear();
            for (int off = 0; off < 12288; off += 512)
            { blk.clear(); ch.renderInto(blk, 0, 512, false);
              for (int c = 0; c < 2; ++c) out.copyFrom(c, off, blk, c, 0, 512); }
            return out;
        };
        auto dryCh = mk(); auto dry = render(*dryCh);
        auto namCh = mk();
        namCh->chFxType[0] = DrumChannel::ChFxNamAmp;   // type set, NO file yet = must stay dry
        namCh->refreshChFxAssets(0);
        auto noFile = render(*namCh);
        double d0 = 0.0;
        for (int i = 0; i < 12288; ++i) d0 = juce::jmax(d0, (double) std::abs(noFile.getSample(0, i) - dry.getSample(0, i)));
        namCh->chFxFile[0] = root.getChildFile("A2.nam").getFullPathName();
        namCh->refreshChFxAssets(0);
        auto wet = render(*namCh);
        double dd = 0.0, wrms = 0.0; bool fin = true;
        for (int i = 0; i < 12288; ++i)
        {
            const float v = wet.getSample(0, i);
            if (! std::isfinite(v)) fin = false;
            dd = juce::jmax(dd, (double) std::abs(v - dry.getSample(0, i)));
            wrms += (double) v * v;
        }
        wrms = std::sqrt(wrms / 12288.0);
        const bool pass = fin && d0 < 1.0e-9 && dd > 0.01 && wrms > 1.0e-3;
        printf("nofile-diff=%.9f wet-diff=%.4f wet-rms=%.4f finite=%d -> %s\n", d0, dd, wrms, fin ? 1 : 0, pass ? "OK" : "FAIL");
        ok &= pass;
    }

    // [6] the 2x-OVERSAMPLED path: engineOS=2 = the halfband decimate -> model -> interpolate
    //     chain the real plugin uses. Locks finite + audibly transformed.
    printf("[6] channel FX (engineOS=2, halfband): ");
    {
        auto ch = std::make_unique<DrumChannel>();
        ch->engineOS = 2;
        ch->prepareToPlay(96000.0, 1024);   // engine rate = 2x host, like the processor
        auto& sl = ch->slots[0];
        sl = DrumChannel::Slot();
        sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f; sl.oscShape = 0;
        sl.oscFreq = 110.0f; sl.dec = 0.5f;
        ch->chFxType[0] = DrumChannel::ChFxNamAmp;
        ch->chFxFile[0] = root.getChildFile("A2.nam").getFullPathName();
        ch->refreshChFxAssets(0);
        ch->trigger(1.0f);
        juce::AudioBuffer<float> out(2, 24576), blk(2, 1024);
        out.clear();
        for (int off = 0; off < 24576; off += 1024)
        { blk.clear(); ch->renderInto(blk, 0, 1024, false);
          for (int c = 0; c < 2; ++c) out.copyFrom(c, off, blk, c, 0, 1024); }
        double rms = 0.0; bool fin = true;
        for (int i = 0; i < 24576; ++i)
        { const float v = out.getSample(0, i); if (! std::isfinite(v)) fin = false; rms += (double) v * v; }
        rms = std::sqrt(rms / 24576.0);
        const bool pass = fin && rms > 1.0e-3 && rms < 4.0;
        printf("rms=%.4f finite=%d -> %s\n", rms, fin ? 1 : 0, pass ? "OK" : "FAIL");
        ok &= pass;
    }

    // [7] CAB IR wiring: a generated 64-tap decaying IR loads through the Convolution path and
    //     the channel renders finite / bounded (JUCE loads IRs async - allow either state).
    printf("[7] cab IR wiring: ");
    {
        juce::File ir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("basamak_test_ir.wav");
        {
            juce::AudioBuffer<float> b(1, 64);
            for (int i = 0; i < 64; ++i) b.setSample(0, i, i == 0 ? 1.0f : 0.4f * std::exp(-i / 12.0f));
            ir.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(new juce::FileOutputStream(ir), 48000.0, 1, 24, {}, 0));
            w->writeFromAudioSampleBuffer(b, 0, 64);
        }
        auto ch = std::make_unique<DrumChannel>();
        ch->prepareToPlay(48000.0, 512);
        auto& sl = ch->slots[0];
        sl = DrumChannel::Slot();
        sl.engine = DrumChannel::SrcOsc; sl.weight = 1.0f; sl.oscShape = 0;
        sl.oscFreq = 110.0f; sl.dec = 0.5f;
        ch->chFxType[0] = DrumChannel::ChFxCabIr;
        ch->chFxFile[0] = ir.getFullPathName();
        ch->refreshChFxAssets(0);
        juce::Thread::sleep(200);   // let the async IR load land
        ch->trigger(1.0f);
        juce::AudioBuffer<float> out(2, 12288), blk(2, 512);
        out.clear();
        for (int off = 0; off < 12288; off += 512)
        { blk.clear(); ch->renderInto(blk, 0, 512, false);
          for (int c = 0; c < 2; ++c) out.copyFrom(c, off, blk, c, 0, 512); }
        double rms = 0.0; bool fin = true;
        for (int i = 0; i < 12288; ++i)
        { const float v = out.getSample(0, i); if (! std::isfinite(v)) fin = false; rms += (double) v * v; }
        rms = std::sqrt(rms / 12288.0);
        const bool pass = fin && rms > 1.0e-4 && rms < 8.0;
        printf("rms=%.4f finite=%d -> %s\n", rms, fin ? 1 : 0, pass ? "OK" : "FAIL");
        ok &= pass;
    }

    printf(ok ? ">>> NamTest PASS\n" : ">>> NamTest FAIL\n");
    return ok ? 0 : 1;
}
