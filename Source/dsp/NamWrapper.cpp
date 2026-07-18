// [2026-07-18] NAM WRAPPER IMPLEMENTATION - compiled at C++20 inside the `nam_core` static lib
// (see CMakeLists). This is the ONLY translation unit in the project that includes NAM headers.
#include "NamWrapper.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "NAM/get_dsp.h"
#include "NAM/container.h"
#include "NAM/convnet.h"
#include "NAM/linear.h"
#include "NAM/lstm.h"
#include "NAM/wavenet/model.h"
#include "NAM/wavenet/slimmable.h"

// STATIC-LIB REGISTRATION TRAP: each architecture registers itself via a file-static
// ConfigParserHelper in its own TU - and a static library only links TUs whose symbols are
// referenced (LTO even strips an unused force-link symbol array - tried, failed). So load()
// registers every built-in parser EXPLICITLY, guarded by has() (registerParser THROWS on
// duplicates, and a non-LTO build's static registrars may have run already).
static void ensureParsersRegistered()
{
    auto& reg = nam::ConfigParserRegistry::instance();
    auto add = [&reg](const char* name, nam::ConfigParserFunction fn)
    { if (! reg.has(name)) reg.registerParser(name, std::move(fn)); };
    add("Linear",             nam::linear::create_config);
    add("ConvNet",            nam::convnet::create_config);
    add("LSTM",               nam::lstm::create_config);
    add("SlimmableContainer", nam::container::create_config);
    add("WaveNet",            nam::wavenet::create_config);
    add("SlimmableWaveNet",   nam::slimmable_wavenet::create_config);
}

struct BasamakNam
{
    std::unique_ptr<nam::DSP> dsp;
    std::vector<double> inBuf, outBuf;   // NAM_SAMPLE is double by default - convert per block
    int maxBlock = 0;
};

namespace basamak_nam
{

BasamakNam* load(const std::string& path, double sampleRate, int maxBlock, std::string& err)
{
    try
    {
        ensureParsersRegistered();
        auto m = std::make_unique<BasamakNam>();
        m->dsp = nam::get_dsp(std::filesystem::path(path));
        if (m->dsp == nullptr) { err = "could not load model"; return nullptr; }
        m->maxBlock = std::max(16, maxBlock);
        m->inBuf.assign((size_t) m->maxBlock, 0.0);
        m->outBuf.assign((size_t) m->maxBlock, 0.0);
        m->dsp->Reset(sampleRate, m->maxBlock);   // prewarm runs here (message thread)
        return m.release();
    }
    catch (const std::exception& e) { err = e.what(); return nullptr; }
    catch (...)                     { err = "unknown error loading model"; return nullptr; }
}

void destroy(BasamakNam* m) { delete m; }

void reset(BasamakNam* m, double sampleRate, int maxBlock)
{
    if (m == nullptr || m->dsp == nullptr) return;
    try
    {
        m->maxBlock = std::max(16, maxBlock);
        m->inBuf.assign((size_t) m->maxBlock, 0.0);
        m->outBuf.assign((size_t) m->maxBlock, 0.0);
        m->dsp->Reset(sampleRate, m->maxBlock);
    }
    catch (...) {}
}

void process(BasamakNam* m, float* mono, int n)
{
    if (m == nullptr || m->dsp == nullptr || mono == nullptr || n <= 0) return;
    n = std::min(n, m->maxBlock);
    double* in  = m->inBuf.data();
    double* out = m->outBuf.data();
    for (int i = 0; i < n; ++i) in[i] = (double) mono[i];
    double* ins[1]  = { in };
    double* outs[1] = { out };
    m->dsp->process(ins, outs, n);
    for (int i = 0; i < n; ++i)
    {
        const double v = out[i];
        mono[i] = (float)(std::isfinite(v) ? std::clamp(v, -8.0, 8.0) : 0.0);   // anti-gunshot bound
    }
}

bool loudnessDb(BasamakNam* m, double& dbOut)
{
    if (m == nullptr || m->dsp == nullptr || ! m->dsp->HasLoudness()) return false;
    try { dbOut = m->dsp->GetLoudness(); return true; } catch (...) { return false; }
}

double expectedRate(BasamakNam* m)
{
    return (m != nullptr && m->dsp != nullptr) ? m->dsp->GetExpectedSampleRate() : -1.0;
}

} // namespace basamak_nam
