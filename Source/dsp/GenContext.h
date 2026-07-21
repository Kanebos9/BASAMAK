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
#include "DrumGen.h"                    // [2026-07-22 r20] the Drum Kit role's style-DNA engine
#include "../plugin/FactoryContent.h"   // [2026-07-21 P1] bank categories name the drum roles

namespace GenContext
{
// what the gather actually saw - feeds the GeneratePanel's context readout line
struct Readout
{
    int hits        = 0;   // distinct drum-hit columns heard
    int grooveChans = 0;   // step channels that contributed >= 1 hit
    int keyChans    = 0;   // channels (roll or pitched step) that contributed chroma
    int kick = 0, snare = 0, hat = 0;   // [P1] classified hit columns per drum role
};

// [P1 item 1a] drum-ROLE classification per step channel: the sound's bank CATEGORY first
// (Kicks / Snares / Claps [snare-family] / Hi-Hats / Cymbals+Percussion [perc]), then mixName
// keywords for user sounds, else a generic hit (combined list only).
enum DrumRole { DrumKick = 0, DrumSnare, DrumHat, DrumPerc, DrumGeneric };
inline int classifyDrumRole(const juce::String& category, const juce::String& mixName)
{
    if (category == "Kicks")                              return DrumKick;
    if (category == "Snares" || category == "Claps")      return DrumSnare;
    if (category == "Hi-Hats")                            return DrumHat;
    if (category == "Cymbals" || category == "Percussion") return DrumPerc;
    const auto n = mixName.toLowerCase();
    if (n.contains("kick") || n.contains("808 boom"))     return DrumKick;
    if (n.contains("snare") || n.contains("clap"))        return DrumSnare;
    if (n.contains("hat"))                                return DrumHat;
    if (n.contains("cymbal") || n.contains("ride") || n.contains("crash") || n.contains("tom")
        || n.contains("shaker") || n.contains("perc") || n.contains("conga") || n.contains("bongo"))
        return DrumPerc;
    return DrumGeneric;
}

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
    // [P1 1a] classify each channel's drum role ONCE (bank category by mixName, else keywords)
    const auto facNames = Factory::mixNames();
    const auto facCats  = Factory::mixCategories();
    int chanRole[Sequencer::NUM_CHANNELS];
    for (int chn = 0; chn < Sequencer::NUM_CHANNELS; ++chn)
    {
        const auto& name = sq.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, head)]
                              .channels[chn].mixName;
        const int fi = facNames.indexOf(name);
        chanRole[chn] = classifyDrumRole(fi >= 0 && fi < facCats.size() ? facCats[fi]
                                                                        : juce::String(), name);
    }
    auto pushRole = [&ctx](int role, int col, float str)
    {
        int* nP; int* colA; float* strA;
        switch (role)
        {
            case DrumKick:  nP = &ctx.nKick;  colA = ctx.kickCol;  strA = ctx.kickStr;  break;
            case DrumSnare: nP = &ctx.nSnare; colA = ctx.snareCol; strA = ctx.snareStr; break;
            case DrumHat:   nP = &ctx.nHat;   colA = ctx.hatCol;   strA = ctx.hatStr;   break;
            default: return;   // perc/generic hits live only in the combined list
        }
        for (int h = 0; h < *nP; ++h)
            if (colA[h] == col) { strA[h] = juce::jmin(1.0f, strA[h] + str * 0.5f); return; }
        if (*nP < PartGen::Ctx::MAX_HITS) { colA[*nP] = col; strA[*nP] = juce::jmin(1.0f, str); ++(*nP); }
    };
    // [P1 1b] register map scratch: per-channel pitch histogram over the roll notes (min/med/max)
    int regLo[Sequencer::NUM_CHANNELS], regHi[Sequencer::NUM_CHANNELS], regCnt[Sequencer::NUM_CHANNELS];
    static_assert(Sequencer::NUM_CHANNELS <= PartGen::Ctx::MAX_REG, "register map sized to channels");
    int regHist[Sequencer::NUM_CHANNELS][97];
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    { regLo[i] = 99; regHi[i] = -99; regCnt[i] = 0; for (int k = 0; k < 97; ++k) regHist[i][k] = 0; }
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
                    const int hb = juce::jlimit(0, 96, (int) n.semi + 48);   // [P1 1b] register map
                    ++regHist[chn][hb]; ++regCnt[chn];
                    regLo[chn] = juce::jmin(regLo[chn], (int) n.semi);
                    regHi[chn] = juce::jmax(regHi[chn], (int) n.semi);
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
                        pushRole(chanRole[chn], col, cc.stepVel[i]);   // [P1 1a] kick/snare/hat lists
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
    // hit lists must be SORTED (Pockets walks the gaps in order); channels interleave, so sort now
    auto sortHits = [](int n, int* cols, float* strs)
    {
        for (int a = 1; a < n; ++a)
            for (int b2 = a; b2 > 0 && cols[b2] < cols[b2 - 1]; --b2)
            { std::swap(cols[b2], cols[b2 - 1]); std::swap(strs[b2], strs[b2 - 1]); }
    };
    sortHits(ctx.nHits,  ctx.hitCol,   ctx.hitStr);
    sortHits(ctx.nKick,  ctx.kickCol,  ctx.kickStr);
    sortHits(ctx.nSnare, ctx.snareCol, ctx.snareStr);
    sortHits(ctx.nHat,   ctx.hatCol,   ctx.hatStr);
    // [P1 1b] register map: each contributing roll channel's min / MEDIAN / max pitch
    int chnMed[Sequencer::NUM_CHANNELS];
    for (int chn = 0; chn < Sequencer::NUM_CHANNELS; ++chn)
    {
        chnMed[chn] = -999;
        if (regCnt[chn] <= 0) continue;
        int seen = 0, med = regLo[chn];
        for (int k = 0; k < 97; ++k)
        { seen += regHist[chn][k]; if (seen * 2 >= regCnt[chn]) { med = k - 48; break; } }
        chnMed[chn] = med;
        if (ctx.nReg < PartGen::Ctx::MAX_REG)
        {
            ctx.regMin[ctx.nReg] = regLo[chn];
            ctx.regMed[ctx.nReg] = med;
            ctx.regMax[ctx.nReg] = regHi[chn];
            ++ctx.nReg;
        }
    }
    {   // [r20 G, H5/H6] arrangement lanes + the MELODY-OCCUPANCY mask: the highest-median
        // contributing roll channel is "the melody"; its note spans mark the 16th grid the
        // comp/counter roles then avoid. Lowest median = the bass lane's ceiling reference.
        int melChn = -1;
        for (int chn = 0; chn < Sequencer::NUM_CHANNELS; ++chn)
        {
            if (chnMed[chn] <= -999) continue;
            if (melChn < 0 || chnMed[chn] > chnMed[melChn]) melChn = chn;
            if (ctx.bassMed <= -999 || chnMed[chn] < ctx.bassMed) ctx.bassMed = chnMed[chn];
        }
        if (melChn >= 0)
        {
            ctx.melMed = chnMed[melChn];
            for (int b = 0; b < bars; ++b)
            {
                const auto& pat = sq.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, head + b)];
                const auto& cc = pat.channels[melChn];
                if (! cc.drawMode) continue;
                for (int i = 0; i < cc.drawNoteCount; ++i)
                {
                    const auto& n = cc.drawNotes[i];
                    const int p0 = juce::jlimit(0, bars * 16 - 1, b * 16 + (int) n.start / 24);
                    const int p1 = juce::jlimit(p0, bars * 16 - 1,
                                                b * 16 + ((int) n.start + (int) n.len - 1) / 24);
                    for (int p = p0; p <= p1; ++p) ctx.melOcc[p] = true;
                    ctx.melOccValid = true;
                }
            }
        }
    }
    {   // [P1 1d] TARGET-sound introspection (H8/H9): the selected channel's first audible slot
        const auto& tc = sq.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, head)].channels[selCh];
        ctx.sndMono = ! tc.keysPolyMode;
        for (int s = 0; s < DrumChannel::NUM_SLOTS; ++s)
        {
            const auto& sl = tc.slots[s];
            if (sl.weight <= 0.001f || sl.engine < 0) continue;
            ctx.sndValid = true;
            ctx.sndAtk = sl.atk; ctx.sndDec = sl.dec; ctx.sndSus = sl.sustain; ctx.sndRel = sl.release;
            ctx.sndScaleOn = sl.scaleOn;
            if (tc.msSet[s] != nullptr && ! tc.msSet[s]->zones.empty())
            {
                int lo = 999, hi = -999;
                for (const auto& z : tc.msSet[s]->zones)
                { lo = juce::jmin(lo, (int) z.root); hi = juce::jmax(hi, (int) z.root); }
                ctx.sndMsLo = lo; ctx.sndMsHi = hi;
            }
            break;
        }
    }
    if (ro != nullptr)
    {
        ro->hits = ctx.nHits;
        ro->kick = ctx.nKick; ro->snare = ctx.nSnare; ro->hat = ctx.nHat;
        ro->grooveChans = ro->keyChans = 0;
        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        { ro->grooveChans += grooveChan[i] ? 1 : 0; ro->keyChans += keyChan[i] ? 1 : 0; }
    }
    return ctx;
}

// ============================================================================
// [2026-07-21 P1 item 3] STEP-COUNT AUTHORITY + STEP OUTPUT (GENERATE-THEORY v4, user:
// "definitely add"). The generator composes in absolute 384-col time; when the TARGET channel is
// in STEP mode the writer picks the SMALLEST valid step count whose grid holds every onset
// DISTINCTLY (an exact-fit pass first, then a quarter-cell tolerance pass), sets numSteps on
// every group bar (capped so count x bars <= the 64-concat-cell row), and writes steps +
// per-step velocity + stepPitch (vs the channel's Freq-knob base) + SLIDE on bass approach
// notes + gate lengths via stepNoteLen. Roll-mode targets keep the roll write (editor side).
// ============================================================================
struct StepWrite { int count = 0; int written = 0; bool switched = false; };

inline int chooseStepCount(const std::vector<PartGen::Note>& notes, int bars)
{
    const int cap = juce::jmax(1, 64 / juce::jmax(1, bars));   // the 64-concat-cell group cap
    auto fits = [&](int n, int tolCols) -> bool
    {
        bool used[PartGen::MAX_BARS * DrumChannel::MAX_STEPS] = {};
        for (const auto& note : notes)
        {
            const int b     = note.start / DrumChannel::DRAW_RES;
            const int local = note.start - b * DrumChannel::DRAW_RES;
            const int k     = juce::jlimit(0, n - 1, (int) std::lround((double) local * n / 384.0));
            const int grid  = (int) std::lround((double) k * 384.0 / n);
            if (std::abs(local - grid) > tolCols) return false;
            const int key = juce::jlimit(0, PartGen::MAX_BARS * DrumChannel::MAX_STEPS - 1, b * n + k);
            if (used[key]) return false;    // two onsets on one step: this grid can't hold them
            used[key] = true;
        }
        return true;
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int ci = 0; ci < DrumChannel::NUM_VALID_STEP_COUNTS; ++ci)
        {
            const int n = DrumChannel::VALID_STEP_COUNTS[ci];
            if (n > cap) break;   // the list is ascending
            // tolerance = a quarter of the cell, capped at 12 cols (1/32 bar) so a sparse line
            // can never get "held" by a coarse grid that would smear its timing audibly
            if (fits(n, pass == 0 ? 0 : juce::jmax(2, juce::jmin(12, 384 / n / 4)))) return n;
        }
    int best = 1;   // nothing holds every onset distinctly: the densest allowed grid (collisions
    for (int ci = 0; ci < DrumChannel::NUM_VALID_STEP_COUNTS; ++ci)   //  merge on write, loudest wins)
        if (DrumChannel::VALID_STEP_COUNTS[ci] <= cap) best = DrumChannel::VALID_STEP_COUNTS[ci];
    return best;
}

// [r20 item H, G5] optional POCKET MICROTIMING: when the caller passes the gathered ctx + the
// mined drag (ms) + the bar length, every KICK-COINCIDENT step gets a positive Nudge of
// +0.02..+0.08 step (never early - the bass may never lead the kick) and a +5% velocity lean.
// Roll outputs skip this honestly: the roll has no nudge lane.
inline StepWrite writeStepOutput(Sequencer& sq, int head, int bars, int ch,
                                 const std::vector<PartGen::Note>& notes,
                                 const PartGen::Ctx* ctx = nullptr,
                                 double nudgeMs = 0.0, double barMs = 0.0)
{
    StepWrite r;
    r.count = chooseStepCount(notes, bars);
    auto patOf = [&](int b) -> Sequencer::Pattern&
    { return sq.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, head + b)]; };
    const int baseMidi = stepChannelBaseMidi(patOf(0).channels[ch]);   // step pitch 0 = the Freq knob
    r.switched = patOf(0).channels[ch].drawMode;   // [r20] the writer IS the roll->steps switch
    for (int b = 0; b < bars; ++b)
    {
        auto& cc = patOf(b).channels[ch];
        cc.clearStepData();
        // [r20] clear-on-switch convention (the comboSteps handler's clearRoll): a channel that
        // was in Piano Roll loses its roll data when it becomes a step channel - undo restores.
        cc.clearDrawNotes(); cc.drawVel = 1.0f; cc.drawPan = 0.0f; cc.drawTuneCents = 0.0f;
        cc.drawMode = false;
        cc.numSteps = r.count;
    }
    const double span = 384.0 / (double) r.count;   // concat cols per step
    for (const auto& note : notes)
    {
        const int b     = juce::jlimit(0, bars - 1, note.start / DrumChannel::DRAW_RES);
        auto& cc        = patOf(b).channels[ch];
        const int local = note.start - b * DrumChannel::DRAW_RES;
        const int k     = juce::jlimit(0, r.count - 1, (int) std::lround((double) local * r.count / 384.0));
        const float vel = juce::jlimit(0.05f, 1.0f, (float) note.vel / 255.0f);
        if (cc.steps[k] && cc.stepVel[k] >= vel) continue;   // collision: the louder hit names the step
        cc.steps[k]    = true;
        cc.stepVel[k]  = vel;
        cc.stepPitch[k] = (float) juce::jlimit(-DrumChannel::PITCH_RANGE, DrumChannel::PITCH_RANGE,
                              baseMidi >= 0 ? (60 + note.semi) - baseMidi : note.semi);
        cc.stepSlide[k] = note.approach;                     // bass approach -> the SLIDE band
        const float frac = (float) ((double) note.len / span);
        cc.stepNoteLen[k] = frac >= 0.98f ? 0.0f : juce::jlimit(0.08f, 1.0f, frac);   // 0 = natural ring
        if (ctx != nullptr && nudgeMs > 0.0 && barMs > 0.0)
        {   // [G5] laid-back pocket on kick-coincident notes only, always LATE, never early
            bool onKick = false;
            for (int i = 0; i < ctx->nKick; ++i)
                if (std::abs(ctx->kickCol[i] - note.start) <= 6) { onKick = true; break; }
            if (onKick)
            {
                const double stepMs = barMs / (double) r.count;
                cc.stepNudge[k] = (float) juce::jlimit(0.04, 0.16, nudgeMs / (stepMs * 0.5));
                cc.stepVel[k]   = juce::jmin(1.0f, cc.stepVel[k] * 1.05f);
            }
        }
        ++r.written;
    }
    for (int b = 0; b < bars; ++b) patOf(b).channels[ch].markDspDirty();
    return r;
}

// ============================================================================
// [2026-07-22 r20, item A] UNIVERSAL OUTPUT: the ROLL writer as a sibling of the step writer, so
// the panel's "Write as" row can send ANY role either way (Auto = the channel's current mode).
// A step-mode channel switching to the roll follows the clear-on-switch convention (clearStepData
// + drawTuneCents = 0 - the comboSteps handler's model); the caller's ONE commitUndoNow covers it.
// ============================================================================
struct RollWrite { int written = 0; bool switched = false; };

inline RollWrite writeRollOutput(Sequencer& sq, int head, int bars, int ch,
                                 const std::vector<PartGen::Note>& notes)
{
    RollWrite r;
    auto patOf = [&](int b) -> Sequencer::Pattern&
    { return sq.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, head + b)]; };
    r.switched = ! patOf(0).channels[ch].drawMode;
    for (int b = 0; b < bars; ++b)
    {
        auto& cc = patOf(b).channels[ch];
        if (! cc.drawMode) { cc.clearStepData(); cc.drawTuneCents = 0.0f; cc.drawMode = true; }
        cc.clearDrawNotes();
    }
    for (const auto& n : notes)
    {
        const int b = juce::jlimit(0, bars - 1, n.start / DrumChannel::DRAW_RES);
        DrumChannel::DrawNote dn;                       // whole-struct push (the field-drop lesson)
        dn.start = (int16_t) (n.start - b * DrumChannel::DRAW_RES);
        dn.len   = (int16_t) juce::jmax(1, n.len);
        dn.semi  = (int8_t)  juce::jlimit(-48, 48, n.semi);
        dn.vel   = (uint8_t) juce::jlimit(0, 255, n.vel);
        patOf(b).channels[ch].addDrawNote(dn);
        ++r.written;
    }
    for (int b = 0; b < bars; ++b) patOf(b).channels[ch].markDspDirty();
    return r;
}

// ============================================================================
// [2026-07-22 r20, item B] DRUM-LANE writer: one DrumGen lane onto one step channel. Drums are
// STEP-NATIVE (GENERATE-THEORY "DRUM UI") - the count is 16 (the canon grid) whenever the group
// cap allows it, else the smallest valid grid that holds the cells (the item-A authority). Trap
// hat rolls ride out as stepRoll ratchets with a RISING ramp (stepRollDecay > 0 = build up).
// ============================================================================
inline StepWrite writeDrumLane(Sequencer& sq, int head, int bars, int ch,
                               const std::vector<DrumGen::Hit>& hits)
{
    StepWrite r;
    std::vector<PartGen::Note> ns;
    for (const auto& h : hits) ns.push_back({ h.col, 1, 0, 200, false });
    r.count = 16 * bars <= 64 ? 16 : chooseStepCount(ns, bars);   // 16 = the style-DNA default
    auto patOf = [&](int b) -> Sequencer::Pattern&
    { return sq.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, head + b)]; };
    r.switched = patOf(0).channels[ch].drawMode;
    for (int b = 0; b < bars; ++b)
    {
        auto& cc = patOf(b).channels[ch];
        cc.clearStepData();
        cc.clearDrawNotes(); cc.drawVel = 1.0f; cc.drawPan = 0.0f; cc.drawTuneCents = 0.0f;
        cc.drawMode = false;
        cc.numSteps = r.count;
    }
    for (const auto& h : hits)
    {
        const int b     = juce::jlimit(0, bars - 1, h.col / DrumChannel::DRAW_RES);
        auto& cc        = patOf(b).channels[ch];
        const int local = h.col - b * DrumChannel::DRAW_RES;
        const int k     = juce::jlimit(0, r.count - 1, (int) std::lround((double) local * r.count / 384.0));
        const float vel = juce::jlimit(0.05f, 1.0f, h.vel);
        if (cc.steps[k] && cc.stepVel[k] >= vel) continue;   // collision (coarse grid): louder wins
        cc.steps[k]   = true;
        cc.stepVel[k] = vel;
        if (h.roll > 1)
        {
            cc.stepRoll[k]      = juce::jlimit(1, 6, h.roll);
            cc.stepRollDecay[k] = juce::jlimit(-1.0f, 1.0f, h.rollDec);
        }
        ++r.written;
    }
    for (int b = 0; b < bars; ++b) patOf(b).channels[ch].markDspDirty();
    return r;
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
