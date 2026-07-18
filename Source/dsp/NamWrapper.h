// [2026-07-18] NAM WRAPPER - the C++17-safe firewall around NeuralAmpModelerCore (which needs
// C++20 + Eigen). Our TUs include ONLY this header; the implementation (NamWrapper.cpp) is
// compiled inside the separate `nam_core` static-library target at C++20, so NAM/Eigen headers
// never enter the plugin's own translation units.
//
// Threading contract: load/free/reset are MESSAGE THREAD only (model load = file IO + prewarm,
// tens of ms). process() is audio-thread safe on a loaded model. Ownership: the caller keeps the
// pointer alive while any audio block might still use it (graveyard pattern, like MsSet).
#pragma once
#include <string>

struct BasamakNam;   // opaque: holds the nam::DSP instance + scratch buffers

namespace basamak_nam
{
    // Load a .nam model file and Reset() it for the given rate/block (prewarm included).
    // Returns nullptr on failure with `err` filled. MESSAGE THREAD.
    BasamakNam* load(const std::string& path, double sampleRate, int maxBlock, std::string& err);
    void        destroy(BasamakNam*);                       // MESSAGE THREAD

    // Re-Reset for a new sample rate / block size (prepareToPlay). MESSAGE THREAD.
    void        reset(BasamakNam*, double sampleRate, int maxBlock);

    // In-place MONO processing (amps are mono). n <= the maxBlock given at load/reset.
    void        process(BasamakNam*, float* mono, int n);   // AUDIO THREAD

    // Model metadata: loudness in dB if the file carries it (auto level-match), the rate the
    // model was trained at (-1 = unknown), and the file's display name.
    bool        loudnessDb(BasamakNam*, double& dbOut);
    double      expectedRate(BasamakNam*);
}
