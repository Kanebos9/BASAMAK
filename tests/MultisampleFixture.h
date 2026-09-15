#pragma once
#include "DrumChannel.h"
#include <memory>

// Known pitches with a harmonic marker in low zones, so rendered checks can
// distinguish correct zone selection from simply varispeeding the old C4 zone.
inline juce::File makeMultisampleBaseFixture()
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("BasamakBaseFreqTest", "", false);
    if (!dir.createDirectory()) return {};
    for (int midi : {36, 48, 60, 72}) {
        const double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
        juce::AudioBuffer<float> samples(1, 57600);
        for (int i = 0; i < samples.getNumSamples(); ++i) {
            const double phase = juce::MathConstants<double>::twoPi * hz * i / 48000.0;
            const float edge = juce::jmin(1.0f, (float)juce::jmin(i, samples.getNumSamples()-1-i) / 480.0f);
            samples.setSample(0, i, edge * 0.4f * (float)(std::sin(phase) + (midi < 60 ? 0.3 * std::sin(2*phase) : 0.0)));
        }
        auto out = dir.getChildFile(juce::String(midi) + ".wav").createOutputStream();
        if (!out) return {};
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(out.get(), 48000, 1, 24, {}, 0));
        if (!writer) return {};
        out.release();
        if (!writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples())) return {};
    }
    return dir;
}
