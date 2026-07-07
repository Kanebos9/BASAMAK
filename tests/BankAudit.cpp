// FACTORY BANK SIMILARITY AUDIT (informational; always exits 0 - run on demand, not in run.sh).
// Renders every factory sound, fingerprints it (24 log-spaced spectral bands for the ATTACK and the
// BODY window + a decay time), and prints the most similar same-category pairs - candidates for the
// "no near-duplicate sounds" rule. High score = the two sound almost the same.
#include "Sequencer.h"
#include "FactoryContent.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static double goertzel(const std::vector<float>& x, size_t a, size_t b, double f, double sr) {
    const double w = 2.0*M_PI*f/sr, c = 2*std::cos(w), sw = std::sin(w), cw = std::cos(w);
    double s1=0,s2=0; size_t n=0;
    for (size_t i=a; i<b && i<x.size(); ++i){ double s0=x[i]+c*s1-s2; s2=s1; s1=s0; ++n; }
    const double re=s1-s2*cw, im=s2*sw; return std::sqrt(re*re+im*im)/(0.5*(double)juce::jmax((size_t)1,n));
}

int main() {
    const double SR = 48000.0; const int bs = 512;
    const auto names = Factory::mixNames();
    const auto cats  = Factory::mixCategories();
    const int N = names.size();
    constexpr int NB = 24;
    double bandF[NB];
    for (int b = 0; b < NB; ++b) bandF[b] = 45.0 * std::pow(14000.0 / 45.0, (double) b / (NB - 1));

    std::vector<std::array<double, NB>> fA(N), fB(N);
    std::vector<double> dec(N);
    for (int i = 0; i < N; ++i)
    {
        auto* ch = new DrumChannel();
        Factory::applyMix(*ch, i);
        ch->prepareToPlay(SR, bs);
        ch->trigger(1.0f);
        std::vector<float> out; out.reserve((size_t) SR);
        juce::AudioBuffer<float> buf(2, bs);
        for (int blk = 0; blk < (int)(1.0 * SR / bs) + 1; ++blk)
        { buf.clear(); ch->renderInto(buf, 0, bs, false); for (int k = 0; k < bs; ++k) out.push_back(buf.getSample(0, k)); }
        // decay: time until a 10ms RMS falls below -30 dB of the peak RMS
        double pk = 1e-12; std::vector<double> rms;
        for (size_t p = 0; p + 480 < out.size(); p += 480)
        { double e = 0; for (size_t k = p; k < p + 480; ++k) e += (double) out[k] * out[k];
          e = std::sqrt(e / 480.0); rms.push_back(e); pk = std::max(pk, e); }
        dec[i] = 1.0;
        for (size_t r = 0; r < rms.size(); ++r) if (rms[r] > pk * 0.0316) dec[i] = (r + 1) * 0.01;
        double na = 1e-12, nb2 = 1e-12;
        for (int b = 0; b < NB; ++b)
        {
            fA[i][b] = goertzel(out, (size_t)(0.005*SR), (size_t)(0.15*SR), bandF[b], SR);
            fB[i][b] = goertzel(out, (size_t)(0.15*SR),  (size_t)(0.55*SR), bandF[b], SR);
            na += fA[i][b]*fA[i][b]; nb2 += fB[i][b]*fB[i][b];
        }
        na = std::sqrt(na); nb2 = std::sqrt(nb2);
        for (int b = 0; b < NB; ++b) { fA[i][b] /= na; fB[i][b] /= nb2; }
        delete ch;
    }
    struct Pair { double sim; int a, b; };
    std::vector<Pair> pairs;
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
        {
            if (cats[i] != cats[j]) continue;   // only within a family
            double dA = 0, dB = 0;
            for (int b = 0; b < NB; ++b) { dA += fA[i][b]*fA[j][b]; dB += fB[i][b]*fB[j][b]; }
            const double dd = std::fabs(std::log(std::max(0.02, dec[i]) / std::max(0.02, dec[j])));
            const double sim = 0.55 * dA + 0.35 * dB + 0.10 * std::max(0.0, 1.0 - dd);
            pairs.push_back({ sim, i, j });
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y){ return x.sim > y.sim; });
    juce::StringArray seen;
    printf("== top similar pairs PER CATEGORY (metric saturates on sustained Keys - judge those by ear) ==\n");
    for (int c = 0; c < cats.size(); ++c)
    {
        if (seen.contains(cats[c])) continue;
        seen.add(cats[c]);
        printf("-- %s --\n", cats[c].toRawUTF8());
        int shown = 0;
        for (const auto& p : pairs)
        {
            if (cats[p.a] != cats[c] || shown >= 5) continue;
            printf("  %.3f  %-16s ~  %-16s (dec %.2fs/%.2fs)\n", p.sim,
                   names[p.a].toRawUTF8(), names[p.b].toRawUTF8(), dec[p.a], dec[p.b]);
            ++shown;
        }
    }
    return 0;
}
