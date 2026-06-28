#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstring>

//==============================================================================
// Lock-free single-producer/single-consumer hand-off of a block of audio
// samples from the audio thread (producer) to the editor (consumer) for FFT.
//
// Capture is TRIGGER-ALIGNED: the audio thread calls arm() the moment the
// analysed channel fires a step, which restarts the window at the hit's attack.
// It then grabs exactly fftSize samples from that point and raises `ready`. The
// editor copies `data`, lowers `ready`, then does the FFT on its own copy.
// Because every hit of the same sound is captured from the same point, the
// resulting spectrum is identical each time instead of catching the hit at a
// random phase (which a free-running window does).
//==============================================================================
struct SpectrumTap
{
    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1 << fftOrder; // 1024

    float fifo[fftSize] = {};
    int   fifoIndex = 0;
    bool  capturing = false;   // audio thread only
    float data[fftSize] = {};
    std::atomic<bool> ready { false };

    // Audio thread: start a fresh ONE-SHOT capture aligned to this moment. Called at EVERY step
    // boundary (Sequencer) AND periodically while stopped (processor), so the window is step-aligned
    // (consistent spectrum) but still refreshes for sustained / long-attack / still-ringing sounds.
    void arm() noexcept { fifoIndex = 0; capturing = true; }

    // Audio thread, once per sample. Ignored unless a capture is in progress.
    void push(float sample) noexcept
    {
        if (! capturing) return;

        fifo[fifoIndex++] = sample;

        if (fifoIndex >= fftSize)
        {
            capturing = false;
            if (! ready.load(std::memory_order_acquire))
            {
                std::memcpy(data, fifo, sizeof(fifo));
                ready.store(true, std::memory_order_release);
            }
        }
    }
};
