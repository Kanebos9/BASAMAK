#pragma once
// ============================================================================
// GenContext [2026-07-21 r18 / v1.5.7 - GENERATE P0 plumbing]
//
// FOR NEW READERS: the GENERATE feature's CONTEXT GATHERING, extracted out of
// the editor (genAction used to inline this) so tests/GenTest.cpp can drive it
// HEADLESSLY against a real Sequencer. The editor resolves the merged group
// (groupHead/groupEnd) and passes head..end; everything musical happens here:
//
//  - GROOVE: every un-muted, solo-included STEP channel's active steps become
//    EXACT concat hit columns (i * 384 / numSteps - 384 divides every step
//    count, so a 7-step kick lands exactly) + the 16th-grid accent map.
//  - HARMONY: piano-roll channels contribute per-beat pitch-class CHROMA
//    (the roll is C4-absolute = pcs are exact), and [P0 item 4] STEP channels
//    with a PITCHED first audible slot now contribute too: real pitch = the
//    slot's authored Freq-knob base (nearest MIDI note) + each step's
//    stepPitch offset, weighted by step velocity. Sample/Noise-only channels
//    stay out (their pitch is unknowable).
//  - KEY DETECTION: Krumhansl-lite over the SUMMED chroma (so the step-pitch
//    harmony steers detection too, by construction - one histogram source).
//  - READOUT: hit/channel counts for the panel's honest context line
//    ("starvation must never be silent" - GENERATE-THEORY data spec #9).
//
// The selected channel is ALWAYS excluded from its own context. Muted and
// solo-excluded channels are silent, so they shape nothing (key detection
// honors this too - it used to listen to muted channels).
// ============================================================================

#include "Sequencer.h"
#include "PartGen.h"

namespace GenContext
{
// what the gather actually saw - feeds the GeneratePanel's context readout line
struct Readout
{
    int hits        = 0;   // distinct drum-hit columns heard
    int grooveChans = 0;   // step channels that contributed >= 1 hit
    int keyChans    = 0;   // channels (roll or pitched step) that contributed chroma
};

// The first PITCHED audible slot's base as a MIDI note, or -1 for an unpitched
// (Sample/Noise-only) channel. Step mode = the Freq knob is the base (slotBaseHz's
// !drawMode branch); Phys reads physFreq, the rest oscFreq.
inline int stepChannelBaseMidi(const DrumChannel& cc)
{
    for (int s = 0; s < DrumChannel::NUM_SLOTS; ++s)
    {
        const auto& sl = cc.slots[s];
        if (sl.weight <= 0.001f) continue;
        const int e = sl.engine;
        const bool pitched = e == DrumChannel::SrcOsc || e == DrumChannel::SrcFM
                          || e == DrumChannel::SrcPhys || e == DrumChannel::SrcModal
                          || e == DrumChannel::SrcGrain;
        if (! pitched) continue;
        const double f = (e == DrumChannel::SrcPhys) ? (double) sl.physFreq : (double) sl.oscFreq;
        if (f <= 0.0) return -1;
        return juce::roundToInt(69.0 + 12.0 * std::log2(f / 440.0));
    }
    return -1;
}

// Gather the musical context for generating onto channel selCh, from group bars head..end.
inline PartGen::Ctx build(const Sequencer& sq, int head, int end, int selCh, Readout* ro = nullptr)
{
    PartGen::Ctx ctx;
    const int bars = juce::jlimit(1, PartGen::MAX_BARS, end - head + 1);
    ctx.bars = bars;
    float grooveMax = 0.0f;
    bool grooveChan[Sequencer::NUM_CHANNELS] = {}, keyChan[Sequencer::NUM_CHANNELS] = {};
    for (int b = 0; b < bars; ++b)
    {
        const auto& pat = sq.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, head + b)];
        const bool solos = Sequencer::anySoloIn(pat);
        for (int chn = 0; chn < Sequencer::NUM_CHANNELS; ++chn)
        {
            if (chn == selCh) continue;
            const auto& cc = pat.channels[chn];
            if (cc.mute || (solos && ! cc.solo)) continue;   // silent channels don't shape the part
            if (cc.drawMode)
            {   // pitched material -> per-beat chroma (the roll is C4-absolute, so pcs are exact)
                for (int i = 0; i < cc.drawNoteCount; ++i)
                {
                    const auto& n = cc.drawNotes[i];
                    const int p = ((n.semi % 12) + 12) % 12;
                    const int bt0 = b * 4 + juce::jlimit(0, 3, (int) n.start / 96);
                    const int bt1 = juce::jlimit(bt0, bars * 4 - 1,
                                                 b * 4 + juce::jlimit(0, 3, ((int) n.start + (int) n.len - 1) / 96));
                    for (int bt = bt0; bt <= bt1; ++bt)
                    { ctx.chroma[bt][p] += (float) n.vel / 255.0f; ctx.chromaValid = true; keyChan[chn] = true; }
                }
            }
            else
            {   // drum/step material -> the 16th-grid accent map AND the EXACT hit list [v2]
                // [P0 item 4] a PITCHED step channel (bassline on steps) also speaks harmony:
                // base = the Freq knob's nearest MIDI note, + the step's own pitch offset.
                const int baseMidi = stepChannelBaseMidi(cc);
                for (int i = 0; i < cc.numSteps; ++i)
                    if (cc.steps[i])
                    {
                        const int pos16 = juce::jlimit(0, 15, i * 16 / juce::jmax(1, cc.numSteps));
                        auto& gcell = ctx.grooveHit[b * 16 + pos16];
                        gcell += cc.stepVel[i]; grooveMax = juce::jmax(grooveMax, gcell);
                        // exact position: 384 divides every step count, so a 7-step channel's hits
                        // land EXACTLY here - Driving/Pockets read these, never a rounded grid
                        const int col = b * DrumChannel::DRAW_RES
                                      + i * DrumChannel::DRAW_RES / juce::jmax(1, cc.numSteps);
                        grooveChan[chn] = true;   // counted even when every hit doubles another channel's column
                        int f = -1;
                        for (int h = 0; h < ctx.nHits; ++h) if (ctx.hitCol[h] == col) { f = h; break; }
                        if (f >= 0) ctx.hitStr[f] = juce::jmin(1.0f, ctx.hitStr[f] + cc.stepVel[i] * 0.5f);
                        else if (ctx.nHits < PartGen::Ctx::MAX_HITS)
                        { ctx.hitCol[ctx.nHits] = col; ctx.hitStr[ctx.nHits] = juce::jmin(1.0f, cc.stepVel[i]);
                          ++ctx.nHits; }
                        if (baseMidi >= 0)
                        {
                            const int note = baseMidi + juce::roundToInt(cc.stepPitch[i]);
                            const int p    = ((note % 12) + 12) % 12;
                            const int bt   = b * 4 + juce::jlimit(0, 3, i * 4 / juce::jmax(1, cc.numSteps));
                            ctx.chroma[bt][p] += juce::jmax(0.05f, cc.stepVel[i]);
                            ctx.chromaValid = true; keyChan[chn] = true;
                        }
                    }
            }
        }
    }
    if (grooveMax > 0.0f)
        for (int i = 0; i < bars * 16; ++i) ctx.grooveHit[i] = juce::jmin(1.0f, ctx.grooveHit[i] / grooveMax);
    // hit list must be SORTED (Pockets walks the gaps in order); channels interleave, so sort now
    for (int a = 1; a < ctx.nHits; ++a)
        for (int b2 = a; b2 > 0 && ctx.hitCol[b2] < ctx.hitCol[b2 - 1]; --b2)
        { std::swap(ctx.hitCol[b2], ctx.hitCol[b2 - 1]); std::swap(ctx.hitStr[b2], ctx.hitStr[b2 - 1]); }
    if (ro != nullptr)
    {
        ro->hits = ctx.nHits;
        ro->grooveChans = ro->keyChans = 0;
        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        { ro->grooveChans += grooveChan[i] ? 1 : 0; ro->keyChans += keyChan[i] ? 1 : 0; }
    }
    return ctx;
}

// Key detection = Krumhansl-lite over the SUMMED per-beat chroma. ONE histogram source means the
// step-pitch harmony steers detection by construction. Returns the tonic pc (0 = C); scaleTypeOut
// 0 = Major, 1 = Natural Minor (kUiScaleTab order). Nothing pitched heard -> C major default.
inline int detectKeyFromCtx(const PartGen::Ctx& ctx, int& scaleTypeOut)
{
    float hist[12] = {}; float total = 0.0f;
    for (int bt = 0; bt < ctx.bars * 4; ++bt)
        for (int p = 0; p < 12; ++p) { hist[p] += ctx.chroma[bt][p]; total += ctx.chroma[bt][p]; }
    scaleTypeOut = 0;
    if (total < 0.01f) return 0;   // nothing pitched -> C major default
    // Krumhansl-lite templates: tonic + fifth dominate, chord tones next, scale members last.
    static const float majT[12] = { 3.0f, 0.0f, 1.0f, 0.0f, 2.0f, 1.0f, 0.0f, 2.5f, 0.0f, 1.0f, 0.0f, 1.0f };
    static const float minT[12] = { 3.0f, 0.0f, 1.0f, 2.0f, 0.0f, 1.0f, 0.0f, 2.5f, 1.0f, 0.0f, 1.0f, 0.0f };
    int bestKey = 0, bestMode = 0; float best = -1.0f;
    for (int k = 0; k < 12; ++k)
        for (int m = 0; m < 2; ++m)
        {
            const float* T = m == 0 ? majT : minT;
            float sc = 0.0f;
            for (int p = 0; p < 12; ++p) sc += hist[p] * T[((p - k) % 12 + 12) % 12];
            if (sc > best) { best = sc; bestKey = k; bestMode = m; }
        }
    scaleTypeOut = bestMode;
    return bestKey;
}

inline int detectKey(const Sequencer& sq, int head, int end, int selCh, int& scaleTypeOut)
{
    return detectKeyFromCtx(build(sq, head, end, selCh), scaleTypeOut);
}
} // namespace GenContext
