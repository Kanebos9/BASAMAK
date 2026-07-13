#include "FactoryContent.h"

using DC = DrumChannel;

namespace Factory
{
//==============================================================================
// Helpers
//==============================================================================

// Forward decls: a few early (legacy-section) builders are slot-authored now (Shaker/Whistle/
// the user imports) - the helpers are defined further down with the other slot authoring tools.
static DC::Slot& mkSlot(DC& c, int engine);
static DC::Slot& mkSlot2(DC& c, int engine, float w0);

// Route an LFO (index 0/1/2 = LFO 1/2/3) to a target in the mod matrix (the LFO Amount is the depth,
// so the route amount is full). Uses the first free mod slot. Replaces the old built-in LFO destination.
static void lfoRoute(DC::Slot& s, int lfoIdx, int tgt)
{
    for (auto& r : s.mod)
        if (r.src == DC::MSOff && r.tgt == DC::MTOff)
        { r.src = (int8_t)(DC::MSLfoFilt + lfoIdx); r.tgt = (int8_t) tgt; r.amt = 1.0f; return; }
}
// KEYTRACK as a matrix route (the dedicated filter keytrack was retired): Note -> Filter 1 Cutoff.
// amt = kt/2 reproduces the old keytrack kt exactly within +-24 st (2^(amt*semis/24*4) == 2^(kt*semis/12)).
static void ktRoute(DC::Slot& s, float kt)
{
    for (auto& r : s.mod)
        if (r.src == DC::MSOff && r.tgt == DC::MTOff)
        { r.src = (int8_t) DC::MSNote; r.tgt = (int8_t) DC::MTFilt1Cut; r.amt = kt * 0.5f; return; }
}
// Put an effect into the first free CHANNEL FX slot (A then B). Character 0.5 = the classic voicing.
static void chFx(DC& c, int type, float amt, float chr = 0.5f)
{
    for (int f = 0; f < 2; ++f)
        if (c.chFxType[f] == DC::ChFxOff)
        { c.chFxType[f] = type; c.chFxAmt[f] = amt; c.chFxChar[f] = chr; return; }
}
// Any source -> any target (incl. the CHANNEL FX targets "... (Channel)"), first free mod slot.
static void modRoute(DC::Slot& s, int src, int tgt, float amt)
{
    for (auto& r : s.mod)
        if (r.src == DC::MSOff && r.tgt == DC::MTOff)
        { r.src = (int8_t) src; r.tgt = (int8_t) tgt; r.amt = amt; return; }
}

// Reset only the *sound* parameters of a channel to a clean, sample-free state.
// Leaves the channel's identity (name/colour/MIDI note) and its step pattern
// untouched, so applying a mix swaps the sound without disturbing the groove.
static void clearSound(DC& c)
{
    c.keysPolyMode = true;   // POLY by default (per-sound); a builder may set it off
    for (int i = 0; i < DC::NUM_SOURCES; ++i) { c.srcOn[i] = false; c.srcWeight[i] = 0.0f; }
    for (auto& s : c.slots) s = DC::Slot();   // empty all slots (engine = -1)
    c.legacyFmEnvFollow = false;              // per-sound flag - must not leak between builders
    // ARP IS PER-SOUND (user 2026-07-11, reversing the old keep-on-pick rule): picking ANY sound
    // resets the channel arp; dedicated-arp sounds re-enable it in their builders / mix files.
    c.arpOn = false; c.arpLen = 2; c.arpRate = 4; c.arpSync = 8; c.arpGate = 1.0f;
    c.arpAlign = true; c.arpHold = false; c.arpAltStrum = false;
    for (int i = 0; i < DC::ARP_ROWS; ++i) c.arpOffset[i] = (i == 0 ? 12 : 0);
    c.usingUserSample = false;
    c.padX = c.padY = 0.5f; c.padLayoutB = false;

    c.layerOscShape = DC::OscSine; c.layerSineFreq = 60.0f;
    c.layerSinePEnvAmt = 0.0f;     c.layerSinePEnvTime = 0.04f; c.layerSinePOffset = 0.0f;
    c.oscUnison = 1; c.oscDetune = 0.0f; c.oscSustain = 0.0f; c.fmSustain = 0.0f; c.physSustain = 0.0f;
    c.oscVibrato = 0.0f; c.physVibrato = 0.0f;
    c.noiseType = 0; c.layerNoiseCenter = 3000.0f; c.layerNoiseWidth = 0.0f; c.noiseSustain = 0.0f;
    c.fmPitch = 0.0f; c.fmSpread = 0.0f; c.fmDepth = 0.4f;
    c.fmPitchEnvAmt = 0.0f; c.fmPitchEnvTime = 0.05f; c.fmPitchOffset = 0.0f; c.fmFeedback = 0.0f; c.fmSub = 0.0f;
    c.sampleCrush = 0.0f;
    c.physFreq = 110.0f; c.physTone = 0.5f; c.physMaterial = 0.0f; c.physPosition = 0.0f;
    c.physPitchEnvAmt = 0.0f; c.physPitchEnvTime = 0.05f; c.physPitchOffset = 0.0f;

    c.pitchEnvAmt = 0.0f; c.pitchEnvTime = 0.05f; c.pitchOffset = 0.0f; c.sampleReverse = false;
    // Per-source AHD: fast attack, no hold, source-appropriate decay (Sample/Noise/Osc/FM/Physical).
    const float decDef[DC::NUM_SOURCES] = { 2.0f, 0.08f, 0.20f, 0.30f, 0.80f };
    for (int s = 0; s < DC::NUM_SOURCES; ++s) { c.srcAtk[s] = 0.003f; c.srcHold[s] = 0.0f; c.srcDec[s] = decDef[s]; }
    c.pitch = 0.0f; c.volume = 0.8f; c.pan = 0.0f;
    for (int b = 0; b < DC::NUM_EQ_BANDS; ++b) c.eqBand[b] = DC::defaultEqBand(b);
    c.filterType = DC::FilterOff; c.filterCutoff = 20000.0f; c.filterReso = 0.707f; c.filterEnvAmt = 0.0f;
    c.driveType = DC::DriveOff; c.driveAmount = 0.0f;
    c.reverbSend = 0.0f; c.delaySend = 0.0f;
    c.allowOverlap = false;
    // legacy channel-level SAMPLE fields (they feed buildSlotsFromLegacy for OLD projects - a stale
    // value must not leak into the next legacy-authored sound)
    c.sampleStart = 0.0f; c.sampleEnd = 1.0f; c.useRegion = false; c.playSpeed = 1.0f;
    c.sliceCount = 1; c.stretchAmt = 1.0f;
    c.keysSlot2Down = 0;   // slot-2 pitch is TIED TO THE SOUND now: each sound sets its own (default 0),
                           // so picking a new sound refreshes it instead of leaking the previous value.
    for (int f = 0; f < 2; ++f) { c.chFxType[f] = 0; c.chFxAmt[f] = 0.0f; c.chFxChar[f] = 0.5f; }   // CHANNEL FX ride with the sound
    c.reverbSend = 0.0f; c.delaySend = 0.0f;   // channel sends ride with the sound too
    c.markDspDirty();
}

static void setSteps(DC& c, int n, std::initializer_list<int> on)
{
    c.numSteps = n;
    for (int i = 0; i < DC::MAX_STEPS; ++i)
    { c.steps[i] = false; c.stepVel[i] = 1.0f; c.stepPitch[i] = 0.0f; c.stepRoll[i] = 1;
      c.stepRollDecay[i] = 0.0f; c.stepNoteLen[i] = 0.0f; c.stepPan[i] = 0.0f; c.stepNudge[i] = 0.0f; c.stepSlide[i] = false;
      c.stepMerge[i] = false; c.stepCondLen[i] = 1; c.stepCondMask[i] = 0; }
    for (int s : on) if (s >= 0 && s < n) c.steps[s] = true;
    c.drawMode = false; c.drawVel = 1.0f; c.drawPan = 0.0f; c.drawTuneCents = 0.0f;   // presets/sounds are step-mode
    c.clearDrawNotes();
}

//==============================================================================
// Sound builders (Osc / Noise / FM only).  Each starts from a clean channel.
//==============================================================================
static void mFMKick(DC& c)     // trap/808-style sub kick: FM knock + heavy sub octave
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -24.0f; c.fmSpread = 0.0f; c.fmDepth = 0.30f; c.fmSub = 0.5f;
    c.fmPitchEnvAmt = 24.0f; c.fmPitchEnvTime = 0.03f;   // the knock (it had NO pitch drop before = a dull blip)
    c.legacyFmEnvFollow = true;   // bright FM knock melts into a CLEAN sub tail (user-approved re-voice)
    c.srcDec[DC::SrcFM] = 0.55f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.51f;
    c.volume = 0.95f;
}
static void mSubBass(DC& c)    // round sine sub-bass
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 44.0f;
    c.layerSinePEnvAmt = 6.0f; c.layerSinePEnvTime = 0.03f;
    c.srcAtk[DC::SrcOsc] = 0.004f; c.srcDec[DC::SrcOsc] = 0.55f;
    c.volume = 0.85f;
}
static void mNoiseSnare(DC& c) // classic synth snare: bright noise crack + a knocking tonal body
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.65f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.35f;
    c.padX = 0.14f + 0.35f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 2100.0f; c.layerNoiseWidth = 0.22f;   // WHITE (grey read dull)
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.22f;            // fatter/longer than the 808
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 165.0f;                // lower body = the "fat" snare
    c.layerSinePEnvAmt = 6.0f; c.layerSinePEnvTime = 0.04f;                     // drum-body pitch knock
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.15f;
    c.eqBand[DC::EQ_HP] = { true, 140.0f, 0.0f, 0.707f };                       // keep the low mud out
    c.driveType = DC::Tube; c.driveAmount = 0.346f;
    c.reverbSend = 0.10f; c.volume = 0.86f;
}
static void mClap(DC& c)   // tight modern clap: short, mid-bright, dry-ish
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 1450.0f; c.layerNoiseWidth = 0.32f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.10f;
    c.eqBand[DC::EQ_HP] = { true, 550.0f, 0.0f, 0.707f };   // claps have no low end - cleans the thump away
    c.reverbSend = 0.15f; c.volume = 0.84f;
}
static void mClosedHat(DC& c)   // airy modern closed hat (purple noise = brighter than the 808's white)
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 4; c.layerNoiseCenter = 9500.0f; c.layerNoiseWidth = 0.12f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.045f; c.volume = 0.62f;
}
static void mOpenHat(DC& c)   // airy modern open hat (purple noise pair of mClosedHat)
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 4; c.layerNoiseCenter = 8800.0f; c.layerNoiseWidth = 0.11f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.38f; c.volume = 0.6f;
}
static void mFMTom(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -7.0f; c.fmSpread = 0.08f; c.fmDepth = 0.30f;
    c.legacyFmEnvFollow = true;   // FM bite on the strike, rounder tom body after
    c.srcDec[DC::SrcFM] = 0.34f; c.volume = 0.82f;
}
static void mRimshot(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.6f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.4f;
    c.padX = 0.43f; c.padY = 0.5f;  // t = 0.40 -> noise 0.60 / osc 0.40
    c.noiseType = 0; c.layerNoiseCenter = 2400.0f; c.layerNoiseWidth = 0.35f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 330.0f;
    c.srcDec[DC::SrcNoise] = 0.04f; c.srcDec[DC::SrcOsc] = 0.05f; c.volume = 0.72f;
}
static void mWoodblock(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 1100.0f;
    c.layerSinePEnvAmt = 5.0f; c.layerSinePEnvTime = 0.02f;
    c.srcDec[DC::SrcOsc] = 0.06f; c.volume = 0.7f;
}
static void mZap(DC& c)        // descending saw blip
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 220.0f;
    c.layerSinePEnvAmt = 36.0f; c.layerSinePEnvTime = 0.12f;
    c.srcDec[DC::SrcOsc] = 0.22f;
    c.filterType = DC::LowPass; c.filterCutoff = 4000.0f; c.filterReso = 2.0f; c.filterEnvAmt = 0.3f;
    c.volume = 0.7f;
}
static void m808Bass(DC& c)    // long FM sub-bass with a pitch-drop click (uses FM pitch env)
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -24.0f; c.fmSpread = 0.0f; c.fmDepth = 0.25f;
    c.fmPitchEnvAmt = 20.0f; c.fmPitchEnvTime = 0.04f;          // quick drop = the "click"
    c.srcAtk[DC::SrcFM] = 0.001f; c.srcDec[DC::SrcFM] = 1.6f;   // long sub tail
    c.driveType = DC::SoftClip; c.driveAmount = 0.387f; c.volume = 0.85f;
}
static void mShaker(DC& c)     // the USER'S patch (shaker2.basamaksound): band-filtered noise
{                              // at ~6.5 kHz with a fast VOL-LFO tremolo = the shk-shk grain
    auto& s = mkSlot(c, DC::SrcNoise);
    s.weight = 0.85f;
    s.noiseType = 0; s.noiseCenter = 6532.0f; s.noiseWidth = 0.19f;
    s.atk = 0.001f; s.dec = 0.16f; s.release = 0.06f;
    s.lfoRate[2] = 5.61f; s.lfoAmt[2] = 0.78f; lfoRoute(s, 2, DC::MTVol);   // VOL tremolo (the user's design)
    c.reverbSend = 0.12f;
    c.volume = 0.85f;
}
static void mCrash(DC& c)      // long bright cymbal-ish wash
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 4; c.layerNoiseCenter = 9000.0f; c.layerNoiseWidth = 0.05f;
    c.srcDec[DC::SrcNoise] = 1.7f; c.reverbSend = 0.15f; c.volume = 0.5f;
}

// ---- Newer sounds: pitch-envelope showcases ----
static void mSawBass(DC& c)     // gritty filtered saw bass
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 55.0f;
    c.srcAtk[DC::SrcOsc] = 0.004f; c.srcDec[DC::SrcOsc] = 0.5f;
    c.filterType = DC::LowPass; c.filterCutoff = 1800.0f; c.filterReso = 1.5f; c.filterEnvAmt = 0.25f;
    c.volume = 0.8f;
}
static void mGlassBell(DC& c)   // long shimmering FM bell
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 24.0f; c.fmSpread = 0.72f; c.fmDepth = 0.5f;
    c.fmPitchEnvAmt = 6.0f; c.fmPitchEnvTime = 0.05f;
    c.srcDec[DC::SrcFM] = 1.3f;
    c.reverbSend = 0.35f; c.volume = 0.6f;
}
static void mSubDrop(DC& c)     // sine sub with a delayed pitch drop (P.Offset)
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 48.0f;
    c.layerSinePEnvAmt = 36.0f; c.layerSinePEnvTime = 0.18f; c.layerSinePOffset = 0.12f;
    c.srcDec[DC::SrcOsc] = 0.7f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.346f; c.volume = 0.88f;
}
static void mNoiseSweep(DC& c)  // airy noise with a downward filter sweep
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 1; c.layerNoiseCenter = 6000.0f; c.layerNoiseWidth = 0.15f;
    c.srcAtk[DC::SrcNoise] = 0.01f; c.srcDec[DC::SrcNoise] = 0.6f;
    c.filterType = DC::LowPass; c.filterCutoff = 9000.0f; c.filterReso = 2.0f; c.filterEnvAmt = -0.6f;
    c.volume = 0.55f;
}

//==============================================================================
// Classic drum-machine kit (TR-808 / TR-909 / TR-606) recreated with the synth
// engine. Category "Drum Machines".
//==============================================================================
// -- TR-808 --
static void m808Kick(DC& c) {   // long boomy 808: lower sine, longer tail, warmer saturation
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 45.0f;
    c.layerSinePEnvAmt = 14.0f; c.layerSinePEnvTime = 0.075f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 1.35f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.447f; c.volume = 0.97f;
}
static void m808Clap(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 1200.0f; c.layerNoiseWidth = 0.30f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.14f;
    c.eqBand[DC::EQ_HP] = { true, 480.0f, 0.0f, 0.707f };
    c.reverbSend = 0.22f; c.volume = 0.84f;
}
static void m808ClosedHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 9000.0f; c.layerNoiseWidth = 0.12f; c.srcDec[DC::SrcNoise] = 0.035f; c.volume = 0.6f;
}
static void m808OpenHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 8400.0f; c.layerNoiseWidth = 0.11f; c.srcDec[DC::SrcNoise] = 0.32f; c.volume = 0.58f;
}
static void m808Cowbell(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 12.0f; c.fmSpread = 0.42f; c.fmDepth = 0.40f; c.srcDec[DC::SrcFM] = 0.28f; c.volume = 0.72f;
}
static void m808LowTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 90.0f; c.layerSinePEnvAmt = 8.0f; c.layerSinePEnvTime = 0.10f;
    c.srcDec[DC::SrcOsc] = 0.45f; c.volume = 0.84f;
}
static void m808MidTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 130.0f; c.layerSinePEnvAmt = 8.0f; c.layerSinePEnvTime = 0.09f;
    c.srcDec[DC::SrcOsc] = 0.40f; c.volume = 0.84f;
}
static void m808HiTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 180.0f; c.layerSinePEnvAmt = 8.0f; c.layerSinePEnvTime = 0.08f;
    c.srcDec[DC::SrcOsc] = 0.35f; c.volume = 0.84f;
}
static void m808Rimshot(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 440.0f; c.srcDec[DC::SrcOsc] = 0.05f;
    c.driveType = DC::HardClip; c.driveAmount = 0.5f; c.volume = 0.7f;
}
static void m808Conga(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 260.0f; c.layerSinePEnvAmt = 4.0f; c.layerSinePEnvTime = 0.05f;
    c.srcDec[DC::SrcOsc] = 0.26f; c.volume = 0.8f;
}
static void m808Clave(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 2500.0f; c.srcDec[DC::SrcOsc] = 0.04f; c.volume = 0.66f;
}
// -- TR-909 --
static void m909Kick(DC& c) {   // 909: punchy sine + a real attack click, tube-saturated
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.18f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.82f;
    c.padX = 0.14f + 0.82f * 0.72f; c.padY = 0.5f;        // Noise(1st)/Osc(2nd) slider
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 54.0f; c.layerSinePEnvAmt = 26.0f; c.layerSinePEnvTime = 0.032f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.42f;
    c.noiseType = 0; c.layerNoiseCenter = 4000.0f; c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.018f;
    c.driveType = DC::Tube; c.driveAmount = 0.529f; c.volume = 0.97f;
}
static void m909ClosedHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 28.0f; c.fmSpread = 0.80f; c.fmDepth = 0.50f; c.srcDec[DC::SrcFM] = 0.04f; c.volume = 0.6f;
}
static void m909OpenHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 28.0f; c.fmSpread = 0.80f; c.fmDepth = 0.50f; c.srcDec[DC::SrcFM] = 0.42f; c.volume = 0.58f;
}
static void m909Ride(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 30.0f; c.fmSpread = 0.65f; c.fmDepth = 0.60f; c.srcDec[DC::SrcFM] = 1.40f; c.reverbSend = 0.2f; c.volume = 0.55f;
}
static void m909Crash(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.5f;
    c.srcOn[DC::SrcFM]    = true; c.srcWeight[DC::SrcFM]    = 0.5f;
    c.padX = 0.14f + 0.5f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 7000.0f; c.layerNoiseWidth = 0.05f; c.srcDec[DC::SrcNoise] = 1.5f;
    c.fmPitch = 26.0f; c.fmSpread = 0.7f; c.fmDepth = 0.55f; c.srcDec[DC::SrcFM] = 1.6f;
    c.reverbSend = 0.30f; c.volume = 0.55f;
}
static void m909LowTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 100.0f; c.layerSinePEnvAmt = 10.0f; c.layerSinePEnvTime = 0.09f;
    c.srcDec[DC::SrcOsc] = 0.40f; c.volume = 0.84f;
}
static void m909MidTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 150.0f; c.layerSinePEnvAmt = 10.0f; c.layerSinePEnvTime = 0.08f;
    c.srcDec[DC::SrcOsc] = 0.36f; c.volume = 0.84f;
}
static void m909HiTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 200.0f; c.layerSinePEnvAmt = 10.0f; c.layerSinePEnvTime = 0.07f;
    c.srcDec[DC::SrcOsc] = 0.32f; c.volume = 0.84f;
}
static void m909Rim(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 1700.0f; c.srcDec[DC::SrcOsc] = 0.03f;
    c.driveType = DC::HardClip; c.driveAmount = 0.447f; c.volume = 0.7f;
}
// -- TR-606 --
static void m606Snare(DC& c) {   // 606: short papery snap
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.62f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.38f;
    c.padX = 0.14f + 0.38f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 2800.0f; c.layerNoiseWidth = 0.12f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.11f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 215.0f;
    c.layerSinePEnvAmt = 5.0f; c.layerSinePEnvTime = 0.025f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.08f;
    c.eqBand[DC::EQ_HP] = { true, 300.0f, 0.0f, 0.707f };
    c.volume = 0.84f;
}
static void m606ClosedHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 11000.0f; c.layerNoiseWidth = 0.08f; c.srcDec[DC::SrcNoise] = 0.03f; c.volume = 0.58f;
}
static void mWoodClave(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 800.0f; c.physTone = 0.6f; c.physMaterial = 2.0f; // Wood
    c.srcDec[DC::SrcPhys] = 0.25f; c.volume = 0.8f;
}

static void mPluck(DC& c)       // Physical: bright plucked string
{
    clearSound(c);
    c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 196.0f; c.physTone = 0.7f; c.physMaterial = 0.0f;
    c.srcDec[DC::SrcPhys] = 0.9f; c.volume = 0.85f;
}

//==============================================================================
// Extra mixes (Unison/Detune, FM Sub & Feedback, Physical Position, long attacks).
//==============================================================================
// -- Bass --
static void mReeseBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 55.0f; c.oscUnison = 5; c.oscDetune = 0.55f;
    c.srcAtk[DC::SrcOsc] = 0.004f; c.srcDec[DC::SrcOsc] = 0.9f;
    c.filterType = DC::LowPass; c.filterCutoff = 1100.0f; c.filterReso = 1.3f; c.volume = 0.8f;
}
static void mSquareBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSquare; c.layerSineFreq = 50.0f; c.srcDec[DC::SrcOsc] = 0.6f;
    c.filterType = DC::LowPass; c.filterCutoff = 900.0f; c.filterReso = 0.9f; c.volume = 0.82f;
}
static void mFMBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -24.0f; c.fmSpread = 0.1f; c.fmDepth = 0.5f; c.fmSub = 0.6f;
    c.legacyFmEnvFollow = true;   // bright pluck -> pure sub tail (classic trap bass behaviour)
    c.srcDec[DC::SrcFM] = 0.7f; c.volume = 0.85f;
}
static void mGrowlBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -12.0f; c.fmSpread = 0.3f; c.fmDepth = 0.6f; c.fmFeedback = 0.6f; c.fmSub = 0.3f;
    c.srcDec[DC::SrcFM] = 0.5f; c.driveType = DC::SoftClip; c.driveAmount = 0.447f; c.volume = 0.75f;
}
static void mAcidBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 60.0f; c.srcDec[DC::SrcOsc] = 0.3f;
    c.filterType = DC::LowPass; c.filterCutoff = 600.0f; c.filterReso = 4.0f; c.filterEnvAmt = 0.6f;
    c.driveType = DC::Tube; c.driveAmount = 0.447f; c.volume = 0.78f;
}
// -- Kicks --
static void mPunchKick(DC& c) {   // HARD short club/rave punch - all attack, no boom.
    // Distinct from 909 Kick (round + click + tube): higher pitch, a much FASTER/bigger knock,
    // a very short body, and HARD clipping instead of tube warmth.
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 62.0f;
    c.layerSinePEnvAmt = 36.0f; c.layerSinePEnvTime = 0.02f;   // ~3-octave snap in 20 ms = the punch IS the attack
    c.srcAtk[DC::SrcOsc] = 0.0005f; c.srcDec[DC::SrcOsc] = 0.22f;
    c.driveType = DC::HardClip; c.driveAmount = 0.566f; c.volume = 0.97f;
}
static void mDistKick(DC& c) {   // gnarly distorted kick with a proper knock
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -20.0f; c.fmSpread = 0.12f; c.fmDepth = 0.5f; c.fmSub = 0.45f;
    c.fmPitchEnvAmt = 20.0f; c.fmPitchEnvTime = 0.035f;
    c.srcDec[DC::SrcFM] = 0.40f; c.driveType = DC::Fuzz; c.driveAmount = 0.592f; c.volume = 0.92f;
}
// ---- Kicks batch 1 (v1.3.4 taxonomy fill). Roles, so the family stays distinct:
// 808 = long boom | 909 = click+tube | Punch = hard rave | Dist = fuzz | FM = trap sub |
// Sub = knockless layer | Break = tight boxy | Rumble = techno noise tail |
// Crunch = foldback grit | Acoustic = modal head + beater. (A round "Deep Kick" and a Bitcrush
// "Lofi Kick" were BOTH cut here: audited 0.99 vs 909/Punch - the short-sine space is FULL.)
static void mSubKick(DC& c) {   // pure 38 Hz layering sub: NO knock at all, soft attack, long-ish
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 38.0f;
    c.srcAtk[DC::SrcOsc] = 0.004f; c.srcDec[DC::SrcOsc] = 0.75f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.316f; c.volume = 0.97f;
}
static void mBreakKick(DC& c) {   // tight boxy breakbeat kick: mid-forward, small knock, very short
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 78.0f;
    c.layerSinePEnvAmt = 8.0f; c.layerSinePEnvTime = 0.025f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.16f;
    c.eqBand[DC::EQ_HP] = { true, 55.0f, 0.0f, 0.707f };   // boxy = no sub weight
    c.driveType = DC::Tube; c.driveAmount = 0.469f; c.volume = 0.93f;
}
static void mRumbleKick(DC& c) {   // hard-techno: clipped knock + a LONG dark noise rumble tail
    clearSound(c);
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.48f;
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.52f;
    c.padX = 0.14f + 0.82f * 0.52f; c.padY = 0.5f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 52.0f;
    c.layerSinePEnvAmt = 24.0f; c.layerSinePEnvTime = 0.022f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.38f;
    c.noiseType = 2; c.layerNoiseCenter = 1400.0f; c.layerNoiseWidth = 0.35f;   // brown = the dark rumble
    c.srcAtk[DC::SrcNoise] = 0.002f; c.srcDec[DC::SrcNoise] = 0.70f;
    c.eqBand[DC::EQ_HP] = { true, 40.0f, 0.0f, 0.707f };
    c.driveType = DC::HardClip; c.driveAmount = 0.616f; c.volume = 0.90f;
}
// -- Snares & Claps --
static void mTrapSnare(DC& c) {   // sharp trap crack
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.68f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.32f;
    c.padX = 0.14f + 0.32f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 3200.0f; c.layerNoiseWidth = 0.14f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.16f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 250.0f;
    c.layerSinePEnvAmt = 6.0f; c.layerSinePEnvTime = 0.028f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.09f;
    c.eqBand[DC::EQ_HP] = { true, 350.0f, 0.0f, 0.707f };
    c.driveType = DC::Tube; c.driveAmount = 0.387f;
    c.reverbSend = 0.12f; c.volume = 0.86f;
}
static void mModSnare(DC& c) {   // realistic snare: Modal membrane body + resonant noise "wires"
    clearSound(c);
    DC::Slot& b = c.slots[0]; b.engine = DC::SrcModal; b.weight = 0.48f;
    b.modalMaterial = 3;               // Membrane (circular drumhead modes)
    b.oscFreq = 185.0f; b.modalDecay = 0.10f; b.modalTone = 0.62f; b.modalHit = 0.35f;
    b.atk = 0.001f;
    DC::Slot& n = c.slots[1]; n.engine = DC::SrcNoise; n.weight = 0.52f;
    n.noiseType = 0; n.noiseCenter = 3000.0f; n.noiseWidth = 0.10f;
    n.noiseRes = 0.20f; n.noiseDrive = 0.15f;   // slightly ringing, saturated "snare wires"
    n.atk = 0.001f; n.dec = 0.17f;
    c.padX = 0.52f; c.padY = 0.5f;
    c.eqBand[DC::EQ_HP] = { true, 160.0f, 0.0f, 0.707f };
    c.reverbSend = 0.10f; c.volume = 0.9f;
}
// -- Hats & Cymbals --
static void mMetalHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 30.0f; c.fmSpread = 0.85f; c.fmDepth = 0.5f; c.fmFeedback = 0.5f;
    c.srcDec[DC::SrcFM] = 0.05f; c.volume = 0.6f;
}
static void mTightHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 4; c.layerNoiseCenter = 12000.0f; c.layerNoiseWidth = 0.1f; c.srcDec[DC::SrcNoise] = 0.025f; c.volume = 0.55f;
}
static void mSizzle(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 33.0f; c.fmSpread = 0.7f; c.fmDepth = 0.6f; c.fmFeedback = 0.4f;
    c.srcDec[DC::SrcFM] = 1.2f; c.reverbSend = 0.2f; c.volume = 0.5f;
}
// -- Toms & Percussion (Physical) --
static void mRotoTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 140.0f; c.physTone = 0.55f; c.physMaterial = 5.0f; // Skin
    c.physPitchEnvAmt = 5.0f; c.physPitchEnvTime = 0.1f; c.srcDec[DC::SrcPhys] = 0.5f; c.volume = 0.85f;
}
static void mTabla(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 180.0f; c.physTone = 0.6f; c.physMaterial = 5.0f; c.physPosition = 0.6f;
    c.srcDec[DC::SrcPhys] = 0.4f; c.volume = 0.85f;
}
static void mLogDrum(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 160.0f; c.physTone = 0.45f; c.physMaterial = 2.0f; c.srcDec[DC::SrcPhys] = 0.35f; c.volume = 0.85f;
}
static void mBongo(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 300.0f; c.physTone = 0.6f; c.physMaterial = 5.0f; c.srcDec[DC::SrcPhys] = 0.25f; c.volume = 0.82f;
}
// -- Bells & Mallets / Plucks --
static void mVibraphone(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 440.0f; c.physTone = 0.7f; c.physMaterial = 4.0f; c.srcDec[DC::SrcPhys] = 1.4f; c.reverbSend = 0.18f; c.volume = 0.78f;
}
static void mHarp(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 330.0f; c.physTone = 0.6f; c.physMaterial = 0.0f; c.physPosition = 0.4f; c.srcDec[DC::SrcPhys] = 0.9f; c.volume = 0.82f;
}
// -- FX & Synth --
static void mRiser(DC& c) {       // long attack swell
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 6000.0f; c.layerNoiseWidth = 0.3f;
    c.srcAtk[DC::SrcNoise] = 0.8f; c.srcDec[DC::SrcNoise] = 0.1f; c.reverbSend = 0.2f; c.volume = 0.6f;
}
static void mBigRiser(DC& c) {    // ~bar-long supersaw riser: long attack swell + rising pitch + opening filter
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 220.0f; c.oscUnison = 5; c.oscDetune = 0.4f;
    c.layerSinePEnvAmt = -24.0f; c.layerSinePEnvTime = 1.3f;        // pitch starts 2 octaves low, rises to base
    c.srcAtk[DC::SrcOsc] = 1.8f; c.srcDec[DC::SrcOsc] = 1.6f; c.oscSustain = 0.0f;   // swells in over a bar, then a long fade-out (was sustain 0.6)
    c.filterType = DC::LowPass; c.filterCutoff = 350.0f; c.filterReso = 2.5f; c.filterEnvAmt = 0.95f; // opens as it swells
    c.reverbSend = 0.28f; c.volume = 0.6f;
}
static void mWind(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 2; c.layerNoiseCenter = 800.0f; c.layerNoiseWidth = 0.4f;
    c.srcAtk[DC::SrcNoise] = 0.3f; c.srcDec[DC::SrcNoise] = 0.6f; c.volume = 0.55f;
}
static void mStab(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 160.0f; c.oscUnison = 4; c.oscDetune = 0.4f;
    c.srcDec[DC::SrcOsc] = 0.22f; c.filterType = DC::LowPass; c.filterCutoff = 2200.0f; c.filterReso = 1.0f; c.volume = 0.72f;
}
// Vox/Talkbox now use the Analog+FM "Vowel"/"Formant" WAVE shapes (the Formant filter box was removed) -
// authored directly as a SrcOsc slot. Shapes (v5 list): 6=Vowel A, 7=Vowel O, 8=Formant.
static void mVox(DC& c) {          // vowel/vocal tone via the Vowel A wave shape + unison + a little vibrato
    clearSound(c);
    DC::Slot& s = c.slots[0]; s.engine = DC::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = 6;            // Vowel A (additive formant bank)
    s.oscFreq = 150.0f; s.oscUnison = 3; s.oscDetune = 0.2f;
    s.dec = 1.2f; s.vibrato = 0.3f;   // long decay (was sustain 0.5 + dec 0.45 -> now a real fade-out); wobble = vocal life
    c.volume = 0.72f;
}
static void mTalkbox(DC& c) {      // FM + the Formant wave shape = a buzzy "talking" vocal tone
    clearSound(c);
    DC::Slot& s = c.slots[0]; s.engine = DC::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = 8;            // Formant wave (additive formant bank)
    s.oscFreq = 120.0f; s.fmDepth = 0.5f; s.fmSpread = 0.2f;   // FM adds the buzz/"talk"
    s.dec = 1.2f; s.vibrato = 0.25f;   // long decay (was sustain 0.4 + dec 0.5 -> now a real fade-out)
    c.volume = 0.72f;
}
static void mWhistle(DC& c) {       // v2 (user: "doesn't sound like a whistle, needs sustain"):
                                    // a NEAR-PURE held tone + strong slow vibrato + a breathy air
                                    // band riding along = human whistling, not a synth voice
    auto& s = mkSlot(c, DC::SrcOsc);
    s.weight = 0.9f;
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 1046.5f;      // C6 - whistle register
    s.atk = 0.03f; s.hold = 0.0f; s.dec = 0.6f; s.sustain = 0.85f; s.release = 0.12f;
    s.vibrato = 0.42f;                                               // the slow human wobble
    s.pEnvP[0] = 1.2f; s.pEnvT[0] = 0.03f;                           // small scoop into the note
    auto& n = mkSlot2(c, DC::SrcNoise, 0.9f);                        // 10% AIR around the tone
    n.noiseType = 0; n.noiseCenter = 3800.0f; n.noiseRes = 0.5f;
    n.atk = 0.03f; n.dec = 0.5f; n.sustain = 0.5f; n.release = 0.12f;
    c.volume = 0.7f;
}

//==============================================================================
// MODAL engine factory sounds (SrcModal authored directly). Material/Decay/Tone/Struct + Freq (= oscFreq).
//==============================================================================
static DC::Slot& mkModal(DC& c)   // clean channel + one Modal slot
{
    clearSound(c);
    DC::Slot& s = c.slots[0];
    s.engine = DC::SrcModal; s.weight = 1.0f;
    return s;
}
static void moTubular(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 1; s.oscFreq = 330.0f; s.modalDecay = 0.85f; s.modalTone = 0.6f;  s.modalStruct = 0.5f; c.reverbSend = 0.25f; c.volume = 0.7f; }
static void moGlass(DC& c)     { auto& s = mkModal(c); s.modalMaterial = 2; s.oscFreq = 520.0f; s.modalDecay = 0.55f; s.modalTone = 0.8f;  s.modalStruct = 0.55f; c.reverbSend = 0.2f; c.volume = 0.72f; }
static void moTomDrum(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 3; s.oscFreq = 110.0f; s.modalDecay = 0.3f;  s.modalTone = 0.5f;  s.modalStruct = 0.5f; c.volume = 0.88f; }   // membrane = tom/drum
static void moMetalPlate(DC& c){ auto& s = mkModal(c); s.modalMaterial = 4; s.oscFreq = 280.0f; s.modalDecay = 0.8f;  s.modalTone = 0.75f; s.modalStruct = 0.6f; c.reverbSend = 0.25f; c.volume = 0.7f; }
static void moWoodBlock(DC& c) { auto& s = mkModal(c); s.modalMaterial = 5; s.oscFreq = 900.0f; s.modalDecay = 0.12f; s.modalTone = 0.5f;  s.modalStruct = 0.5f; c.volume = 0.78f; }
static void moKalimba(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 6; s.oscFreq = 440.0f; s.modalDecay = 0.4f;  s.modalTone = 0.5f;  s.modalStruct = 0.5f; c.volume = 0.82f; }
static void moCowbell(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 7; s.oscFreq = 540.0f; s.modalDecay = 0.3f;  s.modalTone = 0.6f;  s.modalStruct = 0.55f; c.volume = 0.75f; }
   // tuned (low Struct)
static void moGong(DC& c)      { auto& s = mkModal(c); s.modalMaterial = 4; s.oscFreq = 90.0f;  s.modalDecay = 1.0f;  s.modalTone = 0.85f; s.modalStruct = 0.8f; c.reverbSend = 0.35f; c.volume = 0.65f; }   // inharmonic (high Struct)

//==============================================================================
// Filter-envelope showcases. Author a single Analog/FM slot directly (applyMix
// keeps it): channel LowPass + high Env -> the cutoff is swept by the amp
// envelope (the "highs die first" motion).
//==============================================================================
static DC::Slot& mkSlot(DC& c, int engine)   // clean channel + one slot at full weight
{
    clearSound(c);
    DC::Slot& s = c.slots[0];
    s.engine = engine; s.weight = 1.0f;
    return s;
}
// -- Filter envelope (per-SLOT LowPass swept down by the amp env, so it only filters THIS slot) --
static void eFilterBass(DC& c) { auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = s.oscShapeB = DC::WvSaw;    s.oscFreq = 55.0f;  s.dec = 0.8f;  s.filterType = DC::LowPass; s.filterCutoff = 350.0f; s.filterReso = 7.0f; s.filterEnvAmt = 0.9f; c.volume = 0.8f; }
static void eFilterPluck(DC& c){ auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = s.oscShapeB = DC::WvSaw;    s.oscFreq = 220.0f; s.dec = 0.45f; s.filterType = DC::LowPass; s.filterCutoff = 500.0f; s.filterReso = 8.0f; s.filterEnvAmt = 1.0f; c.volume = 0.75f; }
static void eFilterZap(DC& c)  { auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = s.oscShapeB = DC::WvSquare; s.oscFreq = 330.0f; s.dec = 0.3f;  s.filterType = DC::LowPass; s.filterCutoff = 700.0f; s.filterReso = 8.0f; s.filterEnvAmt = 1.0f; c.volume = 0.72f; }

// ---- BASS BANK (v1.1.0, slot-authored, Bass Station II philosophy) ----
// Two-osc / osc+sub architectures. Every sound: per-slot LP filter with an ENVELOPE (so per-step
// velocity = a 303-style accent) and a per-slot drive - all visible and editable on the slot.
// Long decays on purpose: per-step LENGTH stretches/tightens the fall, SLIDE ties notes - these
// are sequencer instruments, not one-shots. Sub slots sit an octave below the root.
static DC::Slot& mkSlot2(DC& c, int engine, float w0)   // add slot 2; weights w0 / 1-w0 (padX = slot-2 share)
{
    c.slots[0].weight = w0; c.padX = 1.0f - w0; c.padY = 0.5f;
    DC::Slot& s = c.slots[1];
    s.engine = engine; s.weight = 1.0f - w0;
    return s;
}
static void bStationBass(DC& c) {   // the flagship: detuned saw + square sub, classic analog mono-synth bass
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.oscUnison = 2; s.oscDetune = 0.22f;
    s.atk = 0.002f; s.dec = 0.5f;
    s.filterType = DC::LowPass; s.filterCutoff = 500.0f; s.filterReso = 1.8f; s.filterEnvAmt = 0.55f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.424f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.58f);
    b.oscShape = b.oscShapeB = DC::WvSquare; b.oscFreq = 27.5f;   // sub: -1 octave, square = BS2 style
    c.keysSlot2Down = 12;   // DECLARE the octave (sub already lives an octave down in the base):
                            // live keys + the roll keep the sub interval instead of collapsing it
    b.atk = 0.002f; b.dec = 0.55f;
    b.filterType = DC::LowPass; b.filterCutoff = 260.0f; b.filterReso = 0.8f; b.filterEnvAmt = 0.2f;
    c.volume = 0.8f;
}
static void bLadderBass(DC& c) {    // warm funk: saw through a low, barely-resonant LP + clean sine sub
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.atk = 0.002f; s.dec = 0.32f;
    s.filterType = DC::LowPass; s.filterCutoff = 420.0f; s.filterReso = 1.1f; s.filterEnvAmt = 0.4f;
    s.fxDriveType = DC::SoftClip; s.fxDrive = 0.346f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.55f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 27.5f;
    c.keysSlot2Down = 12;   // DECLARE the octave (sub already lives an octave down in the base):
                            // live keys + the roll keep the sub interval instead of collapsing it
    b.atk = 0.002f; b.dec = 0.35f;
    c.volume = 0.82f;
}
static void bRubberBass(DC& c) {    // FM electric bass: sine carrier, env-following FM = bright pluck -> mellow tail
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 55.0f;
    s.fmDepth = 0.30f; s.fmSpread = 0.0f; s.fmFeedback = 0.15f;   // ratio 1 = thickens, doesn't detune
    s.fmEnvFollow = true; s.fmSub = 0.5f;                          // built-in sub sine, half the carrier
    s.atk = 0.002f; s.dec = 0.4f;
    s.filterType = DC::LowPass; s.filterCutoff = 1200.0f; s.filterReso = 0.9f; s.filterEnvAmt = 0.3f;
    s.fxDriveType = DC::SoftClip; s.fxDrive = 0.316f;
    c.volume = 0.85f;
}
static void bNeuroBass(DC& c) {     // modern growl: folded+FM'd saw with a swept resonant LP, CLEAN sine sub under it
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.oscWarp = 0.35f;
    s.fmDepth = 0.35f; s.fmSpread = 0.2f; s.fmEnvFollow = true;    // ratio 2, riding the env = "talking" top
    s.atk = 0.002f; s.dec = 0.6f;
    s.filterType = DC::LowPass; s.filterCutoff = 900.0f; s.filterReso = 2.8f; s.filterEnvAmt = 0.65f;
    s.lfoRate[0] = 3.5f; s.lfoAmt[0] = 0.4f; lfoRoute(s, 0, DC::MTFilt1Cut);   // filter-LFO wobble on top of the env sweep = "talking" growl
    s.fxDriveType = DC::Foldback; s.fxDrive = 0.469f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.60f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 27.5f;     // untouched low end - growl stays on top
    c.keysSlot2Down = 12;   // DECLARE the octave (sub already lives an octave down in the base):
                            // live keys + the roll keep the sub interval instead of collapsing it
    b.atk = 0.002f; b.dec = 0.65f;
    c.volume = 0.78f;
}
static void bHooverBass(DC& c) {    // rave hoover: wide 5-voice saw that RISES an octave into its pitch
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 65.41f;     // C2
    s.oscUnison = 5; s.oscDetune = 0.6f;
    s.oscWarp = 0.15f;
    s.oscPEnvAmt = -12.0f; s.oscPEnvTime = 0.09f;                  // starts -1 oct, sweeps up into the note
    s.atk = 0.002f; s.dec = 0.7f;
    s.filterType = DC::LowPass; s.filterCutoff = 2500.0f; s.filterReso = 1.4f; s.filterEnvAmt = 0.3f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.387f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.65f);
    b.oscShape = b.oscShapeB = DC::WvSquare; b.oscFreq = 32.7f;   // C1 sub keeps the bottom solid
    c.keysSlot2Down = 12;   // DECLARE the octave (sub already lives an octave down in the base):
                            // live keys + the roll keep the sub interval instead of collapsing it
    b.atk = 0.002f; b.dec = 0.7f;
    b.filterType = DC::LowPass; b.filterCutoff = 300.0f; b.filterReso = 0.8f;
    c.volume = 0.72f;
}
static void bReedBass(DC& c) {      // hollow house bass: odd-harmonic "reed" wave + sine sub, short organ-ish stab
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 12;                                  // Reed (clarinet-like, odd harmonics)
    s.oscFreq = 65.41f;
    s.atk = 0.002f; s.dec = 0.28f;
    s.filterType = DC::LowPass; s.filterCutoff = 700.0f; s.filterReso = 0.9f; s.filterEnvAmt = 0.35f;
    s.fxDriveType = DC::SoftClip; s.fxDrive = 0.283f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.58f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 32.7f;
    c.keysSlot2Down = 12;   // DECLARE the octave (sub already lives an octave down in the base):
                            // live keys + the roll keep the sub interval instead of collapsing it
    b.atk = 0.002f; b.dec = 0.3f;
    c.volume = 0.85f;
}
// -- LFO showcases (the per-slot LFO restarts on every hit = locked to the groove) --
static void bWobbleBass(DC& c) {    // dubstep wobble: LFO drives the resonant LP; clean sine sub under it
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.fmDepth = 0.2f; s.fmSpread = 0.2f; s.fmEnvFollow = true;      // a little FM dirt on the attack
    s.atk = 0.002f; s.dec = 1.2f;                                    // long body - meant for LONG per-step Lengths
    s.filterType = DC::LowPass; s.filterCutoff = 500.0f; s.filterReso = 3.5f;
    s.lfoRate[0] = 2.0f; s.lfoAmt[0] = 0.8f; lfoRoute(s, 0, DC::MTFilt1Cut);   // THE wobble (~1/8 feel at 120)
    s.fxDriveType = DC::Tube; s.fxDrive = 0.447f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.62f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 27.5f;      // steady sub - the wobble lives on top
    c.keysSlot2Down = 12;   // DECLARE the octave (sub already lives an octave down in the base):
                            // live keys + the roll keep the sub interval instead of collapsing it
    b.atk = 0.002f; b.dec = 1.2f;
    c.volume = 0.78f;
}
static void eSiren(DC& c) {         // classic rave/jungle siren: pitch LFO, +/-1 octave sweep
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSquare; s.oscFreq = 660.0f;
    s.atk = 0.01f; s.dec = 2.5f;
    s.lfoRate[1] = 0.9f; s.lfoAmt[1] = 1.0f; lfoRoute(s, 1, DC::MTPitch);   // slow full-depth pitch swing
    s.filterType = DC::LowPass; s.filterCutoff = 2500.0f; s.filterReso = 1.2f;
    c.reverbSend = 0.15f; c.volume = 0.6f;
}
static void eChopper(DC& c) {       // helicopter: brown noise chopped by a fast full-depth volume LFO
    auto& s = mkSlot(c, DC::SrcNoise);
    s.noiseType = 2;                                                 // brown = deep rumble
    s.atk = 0.05f; s.dec = 2.0f;
    s.lfoRate[2] = 11.0f; s.lfoAmt[2] = 1.0f; lfoRoute(s, 2, DC::MTVol);   // rotor-blade tremolo
    c.volume = 0.85f;
}

// -- KEYS bank (v1.2.0): built for the on-screen keyboard - real SUSTAIN levels + release tails
//    (live for KEY voices only; a sequencer hit still plays the pure AHD graph, so these behave
//    like normal decaying sounds in patterns too). All slot-authored -> never use from presets. --
static void kKeysBass(DC& c) {      // mono-synth keys bass: saw + square sub, holds while a key is down
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.oscUnison = 2; s.oscDetune = 0.18f;
    s.atk = 0.002f; s.dec = 0.5f; s.sustain = 0.55f; s.release = 0.12f;
    s.filterType = DC::LowPass; s.filterCutoff = 520.0f; s.filterReso = 1.6f; s.filterEnvAmt = 0.5f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.387f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.58f);
    b.oscShape = b.oscShapeB = DC::WvSquare; b.oscFreq = 55.0f;      // same base: use Slot-2 pitch in KEYS for the sub
    b.atk = 0.002f; b.dec = 0.55f; b.sustain = 0.55f; b.release = 0.12f;
    b.filterType = DC::LowPass; b.filterCutoff = 300.0f; b.filterReso = 0.8f;
    c.volume = 0.8f;
}
static void kEPiano(DC& c) {        // FM e-piano: sine + env-following FM = bell attack, mellow held tone
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 220.0f;
    s.fmDepth = 0.4f; s.fmPitch = 0.4f;                              // ratio ~3x (snapped range 1-6)
    s.fmEnvFollow = true;                                            // bright strike -> soft sustain
    s.atk = 0.002f; s.dec = 1.2f; s.sustain = 0.3f; s.release = 0.35f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.7f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 220.0f;       // clean body under the FM tine
    b.atk = 0.002f; b.dec = 1.0f; b.sustain = 0.35f; b.release = 0.35f;
    c.reverbSend = 0.28f; c.volume = 0.78f;   // more audible reverb on the E-Piano (was 0.12 = masked by its long tail)
}
static void kSoftPad(DC& c) {       // slow airy pad: wide detuned saws, high sustain, long release
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 220.0f;
    s.oscUnison = 5; s.oscDetune = 0.3f;   // odd unison = a centred voice, all in the UI
    s.atk = 0.18f; s.dec = 1.5f; s.sustain = 0.8f; s.release = 0.7f;
    s.filterType = DC::LowPass; s.filterCutoff = 1400.0f; s.filterReso = 0.9f;
    c.reverbSend = 0.25f; c.volume = 0.68f;
}
static void kSquareLead(DC& c) {    // hollow mono lead: detuned squares, holds while a key is down
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSquare; s.oscFreq = 261.63f;
    s.oscUnison = 2; s.oscDetune = 0.12f;
    s.atk = 0.003f; s.dec = 0.4f; s.sustain = 0.7f; s.release = 0.12f;
    s.filterType = DC::LowPass; s.filterCutoff = 2200.0f; s.filterReso = 1.2f;
    c.volume = 0.72f;
}
static void kStringKeys(DC& c) {    // bowed/ebow string: a HELD key keeps the string energised
    auto& s = mkSlot(c, DC::SrcPhys);                                // (Physical sustain-hold, keys only)
    s.physFreq = 261.63f; s.physMaterial = 1.0f;                     // steel
    s.physTone = 0.6f; s.physPosition = 0.25f;
    s.atk = 0.01f; s.dec = 1.2f; s.sustain = 0.7f; s.release = 1.2f;   // long ring-out on key-up
    c.reverbSend = 0.15f; c.volume = 0.8f;
}
// ---- KEYS bank additions (v1.2.1): 12 more, chosen to be clearly DISTINCT from each other.
//      All base at C3 (261.63 Hz) so KEYS auto-tune + step pitch line up, and carry real
//      sustain/release so they hold + release faithfully on the on-screen keyboard. ----
static void kGrandPiano(DC& c) {    // acoustic-ish piano: FM tine strike + triangle body, percussive, DECAYS while held
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 261.63f;
    s.fmDepth = 0.28f; s.fmPitch = 0.65f; s.fmEnvFollow = true;      // hammered-string overtones, bright attack -> mellow
    s.atk = 0.002f; s.dec = 1.7f; s.sustain = 0.16f; s.release = 0.3f;   // low sustain = keeps falling like a real piano
    auto& b = mkSlot2(c, DC::SrcOsc, 0.58f);
    b.oscShape = b.oscShapeB = DC::WvTri; b.oscFreq = 261.63f;       // warm body under the tine
    b.atk = 0.002f; b.dec = 1.5f; b.sustain = 0.18f; b.release = 0.3f;
    b.filterType = DC::LowPass; b.filterCutoff = 3200.0f; b.filterReso = 0.7f;
    c.reverbSend = 0.14f; c.volume = 0.8f;
}
static void kWurli(DC& c) {         // Wurlitzer EP: barky FM reed + gentle tremolo (distinct from the cleaner E-Piano)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 261.63f;
    s.fmDepth = 0.5f; s.fmPitch = 0.2f; s.fmFeedback = 0.15f; s.fmEnvFollow = true;   // reedy bark
    s.atk = 0.002f; s.dec = 1.1f; s.sustain = 0.4f; s.release = 0.25f;
    s.lfoRate[2] = 5.5f; s.lfoAmt[2] = 0.35f; lfoRoute(s, 2, DC::MTVol);   // VOL tremolo = the Wurli wobble
    c.reverbSend = 0.14f; c.volume = 0.8f;
}
static void kClav(DC& c) {          // funky clavinet: bright pulse + snappy filter, percussive/plucky
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvPulse; s.oscFreq = 261.63f;
    s.atk = 0.001f; s.dec = 0.5f; s.sustain = 0.22f; s.release = 0.08f;
    s.filterType = DC::LowPass; s.filterCutoff = 1600.0f; s.filterReso = 2.2f; s.filterEnvAmt = 0.6f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.374f;
    c.volume = 0.76f;
}
static void kSynthBrass(DC& c) {    // analog synth-brass ensemble: filter-swept detuned saws, swells + sustains
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.16f;
    s.atk = 0.06f; s.dec = 0.7f; s.sustain = 0.75f; s.release = 0.2f; // slow-ish brass swell
    s.filterType = DC::LowPass; s.filterCutoff = 900.0f; s.filterReso = 1.4f; s.filterEnvAmt = 0.55f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.316f;
    c.reverbSend = 0.12f; c.volume = 0.7f;
}
static void kChoir(DC& c) {         // vocal "aah" pad: formant vowel shape, slow swell, lush + a touch of vibrato
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 6;                                    // Vowel A (additive formant)
    s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.2f;
    s.atk = 0.25f; s.dec = 1.5f; s.sustain = 0.85f; s.release = 0.8f;
    s.lfoRate[1] = 5.0f; s.lfoAmt[1] = 0.015f; lfoRoute(s, 1, DC::MTPitch);   // subtle pitch vibrato = choir life
    c.reverbSend = 0.32f; c.volume = 0.64f;
}
static void kWarmPad(DC& c) {       // dark warm pad: mellow reed, very slow + very long release (vs Soft Pad's bright saws)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 12;                                   // Reed (hollow odd harmonics)
    s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.22f;
    s.atk = 0.3f; s.dec = 2.0f; s.sustain = 0.85f; s.release = 1.3f;
    s.filterType = DC::LowPass; s.filterCutoff = 850.0f; s.filterReso = 0.8f;
    c.reverbSend = 0.34f; c.volume = 0.62f;
}
static void kGlassBells(DC& c) {    // shimmering bell keys: bright inharmonic partials, long sparkly release
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 10; s.oscFreq = 261.63f;             // Bell shape (bright from the inharmonic partials)
    s.fmDepth = 0.2f; s.fmPitch = 0.8f; s.fmEnvFollow = true;
    s.atk = 0.002f; s.dec = 2.2f; s.sustain = 0.14f; s.release = 1.0f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.65f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 261.63f;      // pure sine body so it's tonal, not only clang
    b.atk = 0.002f; b.dec = 1.8f; b.sustain = 0.18f; b.release = 1.0f;
    c.reverbSend = 0.35f; c.volume = 0.68f;
}
static void kVibes(DC& c) {         // vibraphone: tuned metal bars with the classic tremolo, rings while held (Modal)
    auto& s = mkModal(c);
    s.modalMaterial = 1;                                            // Tubular Bell = tuned metallic ring
    s.oscFreq = 261.63f; s.modalDecay = 0.7f; s.modalTone = 0.65f; s.modalStruct = 0.44f;
    s.atk = 0.002f; s.dec = 1.4f; s.sustain = 0.45f; s.release = 0.7f;
    s.lfoRate[2] = 5.0f; s.lfoAmt[2] = 0.4f; lfoRoute(s, 2, DC::MTVol);   // the vibraphone tremolo (VOL)
    c.reverbSend = 0.28f; c.volume = 0.7f;
}
static void kMarimbaKeys(DC& c) {   // warm wooden marimba mallet, soft, dark, gentle hold (Modal)
    auto& s = mkModal(c);
    s.modalMaterial = 0;                                            // Marimba bars
    s.oscFreq = 261.63f; s.modalDecay = 0.42f; s.modalTone = 0.38f; s.modalStruct = 0.5f;
    s.atk = 0.002f; s.dec = 0.9f; s.sustain = 0.22f; s.release = 0.4f;
    c.reverbSend = 0.16f; c.volume = 0.82f;
}
static void kSawLead(DC& c) {       // bright cutting saw lead for solos, holds while pressed (vs the hollow Square Lead)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 2; s.oscDetune = 0.1f;
    s.atk = 0.004f; s.dec = 0.5f; s.sustain = 0.8f; s.release = 0.12f;
    s.filterType = DC::LowPass; s.filterCutoff = 2600.0f; s.filterReso = 1.5f; s.filterEnvAmt = 0.3f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.346f;
    c.volume = 0.72f;
}
static void kSubBass(DC& c) {       // clean pure-sine sub bass, holds while a key is down (vs the fuller Keys Bass)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 55.0f;
    s.atk = 0.004f; s.dec = 0.6f; s.sustain = 0.85f; s.release = 0.1f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.316f;                      // a little warmth so it reads on small speakers
    c.volume = 0.85f;
}
static void kSynthPluck(DC& c) {    // bright synth pluck: fast resonant filter decay, snappy + melodic (plucks out even if held)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.atk = 0.001f; s.dec = 0.35f; s.sustain = 0.0f; s.release = 0.15f;
    s.filterType = DC::LowPass; s.filterCutoff = 2200.0f; s.filterReso = 3.0f; s.filterEnvAmt = 0.8f;
    c.reverbSend = 0.16f; c.volume = 0.75f;
}
// -- CHORD / SCALE keyboard sounds (v1.2.3): both slots active, one slot voiced as a chord or diatonic
//    scale so one finger plays a full harmony. "Power Keys" + "Octave Bells" also use the SLOT-2 PITCH
//    transpose (keysSlot2Down) to stack the second slot an octave below / above the key. --
static void kPowerKeys(DC& c) {     // power-chord synth: the FIFTH is now slot 2 (a saw a 4th below = the
    // inverted power dyad) - VISIBLE in the Slot-2 pitch control, no hidden chord mode (chord UI removed).
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 2; s.oscDetune = 0.08f;                            // slight width on the root saw
    s.atk = 0.004f; s.dec = 0.6f; s.sustain = 0.82f; s.release = 0.14f;
    s.filterType = DC::LowPass; s.filterCutoff = 2400.0f; s.filterReso = 1.4f; s.filterEnvAmt = 0.3f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.346f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.62f);
    b.oscShape = b.oscShapeB = DC::WvSaw; b.oscFreq = 261.63f;       // the power FIFTH (4th below = inverted 5th)
    b.filterType = DC::LowPass; b.filterCutoff = 2000.0f; b.filterReso = 0.8f;
    b.atk = 0.004f; b.dec = 0.6f; b.sustain = 0.85f; b.release = 0.12f;
    c.keysSlot2Down = 5;                                             // SLOT-2 PITCH: a fourth below = power-chord dyad
    c.volume = 0.72f;
}
static void kOctaveBells(DC& c) {   // major-triad bells with an OCTAVE-UP glass sparkle layer (slot-2 pitch)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 10; s.oscFreq = 261.63f;             // Bell (inharmonic partials)
    s.fmDepth = 0.2f; s.fmPitch = 0.8f; s.fmEnvFollow = true;
    s.scaleOn = true; s.scaleType = 0; s.scaleUnison = 3; s.scaleKey = 0;   // Major-scale triads (was fixed Maj chord; C plays C-E-G identically, now editable in the SCALE box)
    s.atk = 0.002f; s.dec = 2.0f; s.sustain = 0.16f; s.release = 0.9f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.7f);
    b.oscShape = b.oscShapeB = 11; b.oscFreq = 261.63f;            // Glass shimmer, an octave up (slot-2 pitch)
    b.atk = 0.002f; b.dec = 1.4f; b.sustain = 0.12f; b.release = 0.8f;
    c.keysSlot2Down = -12;                                          // SLOT-2 PITCH: sparkle an octave ABOVE the key
    c.reverbSend = 0.34f; c.volume = 0.64f;
}
static void kDorianPad(DC& c) {     // scale-locked pad (Dorian): every key voices a diatonic chord, always in key
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.scaleOn = true; s.scaleType = 3; s.scaleUnison = 3; s.scaleKey = 0;   // Dorian triads in C
    s.atk = 0.2f; s.dec = 1.6f; s.sustain = 0.85f; s.release = 0.9f;
    s.filterType = DC::LowPass; s.filterCutoff = 1100.0f; s.filterReso = 0.9f; s.filterEnvAmt = 0.3f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.55f);
    b.oscShape = b.oscShapeB = DC::WvTri; b.oscFreq = 261.63f;      // warm detuned body under the harmonized saws
    b.oscUnison = 2; b.oscDetune = 0.18f;
    b.atk = 0.25f; b.dec = 1.8f; b.sustain = 0.85f; b.release = 1.0f;
    c.reverbSend = 0.3f; c.volume = 0.6f;
}
static void kMajorChoir(DC& c) {    // vocal "aah" that harmonizes into the MAJOR scale (diatonic choir) + a reed body
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 6; s.oscFreq = 261.63f;             // Vowel A
    s.scaleOn = true; s.scaleType = 0; s.scaleUnison = 3; s.scaleKey = 0;   // Major triads
    s.atk = 0.22f; s.dec = 1.5f; s.sustain = 0.85f; s.release = 0.8f;
    s.lfoRate[1] = 5.0f; s.lfoAmt[1] = 0.015f; lfoRoute(s, 1, DC::MTPitch);   // subtle vibrato = choir life
    auto& b = mkSlot2(c, DC::SrcOsc, 0.6f);
    b.oscShape = b.oscShapeB = 12; b.oscFreq = 261.63f;            // Reed warmth under the vowels
    b.atk = 0.3f; b.dec = 1.6f; b.sustain = 0.8f; b.release = 0.9f;
    c.reverbSend = 0.34f; c.volume = 0.58f;
}
static void kMinorRhodes(DC& c) {   // moody Rhodes-style EP voiced as a MIN7 chord (jazzy, one-finger) + soft body
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 261.63f;
    s.fmDepth = 0.32f; s.fmPitch = 0.5f; s.fmEnvFollow = true;      // tine bark
    s.scaleOn = true; s.scaleType = 1; s.scaleUnison = 4; s.scaleKey = 0;   // C natural-minor 7ths (C plays C-Eb-G-Bb = Cmin7 identically, now editable in the SCALE box)
    s.atk = 0.003f; s.dec = 1.4f; s.sustain = 0.45f; s.release = 0.35f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.6f);
    b.oscShape = b.oscShapeB = DC::WvTri; b.oscFreq = 261.63f;      // soft body under the tines
    b.atk = 0.003f; b.dec = 1.3f; b.sustain = 0.4f; b.release = 0.3f;
    b.filterType = DC::LowPass; b.filterCutoff = 3000.0f; b.filterReso = 0.6f;
    c.reverbSend = 0.2f; c.volume = 0.62f;
}

// -- New plucked strings + mallets (Physical / Karplus-Strong; material 0=Nylon 1=Steel 2=Wood 3=Glass 4=Metal 5=Skin;
//    physPosition low = plucked near the bridge = brighter/twangier). These are one-shot/decaying = a natural fit. --
static void mNylonGuitar(DC& c){ clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f; c.physFreq = 165.0f; c.physTone = 0.50f; c.physMaterial = 0.0f; c.physPosition = 0.35f; c.srcDec[DC::SrcPhys] = 1.0f; c.volume = 0.84f; }       // warm nylon
static void mKoto(DC& c)       { clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f; c.physFreq = 294.0f; c.physTone = 0.78f; c.physMaterial = 1.0f; c.physPosition = 0.15f; c.srcDec[DC::SrcPhys] = 1.3f; c.reverbSend = 0.12f; c.volume = 0.82f; }  // bright Japanese string
static void mPizzicato(DC& c)  { clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f; c.physFreq = 196.0f; c.physTone = 0.55f; c.physMaterial = 1.0f; c.physPosition = 0.40f; c.srcDec[DC::SrcPhys] = 0.32f; c.volume = 0.85f; }                     // short orchestral pluck
static void mBanjo(DC& c)      { clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f; c.physFreq = 330.0f; c.physTone = 0.88f; c.physMaterial = 1.0f; c.physPosition = 0.10f; c.srcDec[DC::SrcPhys] = 0.6f;  c.volume = 0.80f; }                      // bright twang
static void mSitar(DC& c)      { clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f; c.physFreq = 247.0f; c.physTone = 0.82f; c.physMaterial = 1.0f; c.physPosition = 0.18f; c.srcDec[DC::SrcPhys] = 1.6f; c.reverbSend = 0.15f; c.volume = 0.80f; }  // buzzy drone
static void mGlockenspiel(DC& c){ clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f; c.physFreq = 1047.0f; c.physTone = 0.90f; c.physMaterial = 3.0f; c.srcDec[DC::SrcPhys] = 1.1f; c.reverbSend = 0.20f; c.volume = 0.74f; }                      // high bright bell
static void mCelesta(DC& c)    { clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f; c.physFreq = 523.0f; c.physTone = 0.65f; c.physMaterial = 3.0f; c.srcDec[DC::SrcPhys] = 1.0f; c.reverbSend = 0.15f; c.volume = 0.78f; }                       // soft glassy bell

using Builder = void (*)(DC&);


//==============================================================================
// 2026-07 CONTENT FILL (batches 2-12, one overnight pass; see docs/FEATURES.md plan).
// Every category authored to its target; BankAudit-checked before shipping to the user.
//==============================================================================
// ---- Snares (+4) ----
static void mBrushSnare(DC& c) {   // soft brush hit: PINK noise, slow-ish attack, NO tonal body
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 1; c.layerNoiseCenter = 2400.0f; c.layerNoiseWidth = 0.35f;
    c.srcAtk[DC::SrcNoise] = 0.006f; c.srcDec[DC::SrcNoise] = 0.28f;
    c.eqBand[DC::EQ_HP] = { true, 300.0f, 0.0f, 0.707f };
    c.reverbSend = 0.06f; c.volume = 0.8f;
}
static void mSnapSnare(DC& c) {    // ultra-tight snap: bright white crack + a whisper of 240 Hz body
    clearSound(c);
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.2f;
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.8f;
    c.padX = 0.14f + 0.82f * 0.8f; c.padY = 0.5f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 240.0f;
    c.layerSinePEnvAmt = 5.0f; c.layerSinePEnvTime = 0.015f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.06f;
    c.noiseType = 0; c.layerNoiseCenter = 3800.0f; c.layerNoiseWidth = 0.3f;
    c.srcAtk[DC::SrcNoise] = 0.0005f; c.srcDec[DC::SrcNoise] = 0.09f;
    c.eqBand[DC::EQ_HP] = { true, 350.0f, 0.0f, 0.707f };
    c.volume = 0.9f;
}
static void mRoomSnare(DC& c) {    // big wet room: mid body + darker noise, heavy reverb send
    clearSound(c);
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.35f;
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.65f;
    c.padX = 0.14f + 0.82f * 0.65f; c.padY = 0.5f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 190.0f;
    c.layerSinePEnvAmt = 6.0f; c.layerSinePEnvTime = 0.02f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.2f;
    c.noiseType = 0; c.layerNoiseCenter = 1700.0f; c.layerNoiseWidth = 0.3f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.3f;
    c.eqBand[DC::EQ_HP] = { true, 140.0f, 0.0f, 0.707f };
    c.driveType = DC::Tube; c.driveAmount = 0.387f;
    c.reverbSend = 0.45f; c.volume = 0.85f;
}
static void mClackSnare(DC& c) {   // woody clack: HIGH-tuned membrane + a pinch of noise (timbale-ish snare)
    clearSound(c);
    DC::Slot& b = c.slots[0]; b.engine = DC::SrcModal; b.weight = 0.8f;
    b.modalMaterial = 3; b.oscFreq = 330.0f; b.modalDecay = 0.12f; b.modalTone = 0.7f; b.modalHit = 0.5f;
    b.atk = 0.001f;
    DC::Slot& n = c.slots[1]; n.engine = DC::SrcNoise; n.weight = 0.2f;
    n.noiseType = 0; n.noiseCenter = 3200.0f; n.noiseWidth = 0.2f;
    n.atk = 0.0005f; n.dec = 0.05f;
    c.padX = 0.2f; c.padY = 0.5f;
    c.eqBand[DC::EQ_HP] = { true, 200.0f, 0.0f, 0.707f };
    c.volume = 0.88f;
}
// ---- Claps (+4) ----
static void mSnapClap(DC& c) {     // shortest clap in the family: one bright crack, bone dry
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 2600.0f; c.layerNoiseWidth = 0.18f;
    c.srcAtk[DC::SrcNoise] = 0.0005f; c.srcDec[DC::SrcNoise] = 0.07f;
    c.eqBand[DC::EQ_HP] = { true, 600.0f, 0.0f, 0.707f };
    c.volume = 0.9f;
}
static void mBigClap(DC& c) {      // stadium clap: long, very wet, darker centre than the 909
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 1; c.layerNoiseCenter = 1100.0f; c.layerNoiseWidth = 0.32f;
    c.srcAtk[DC::SrcNoise] = 0.002f; c.srcDec[DC::SrcNoise] = 0.38f;
    c.eqBand[DC::EQ_HP] = { true, 400.0f, 0.0f, 0.707f };
    c.reverbSend = 0.5f; c.volume = 0.82f;
}
// ---- Hi-Hats (+1) ----
static void mFootHat(DC& c) {      // pedal "chick": the shortest, dullest hat (foot close)
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 6500.0f; c.layerNoiseWidth = 0.15f;
    c.srcAtk[DC::SrcNoise] = 0.0005f; c.srcDec[DC::SrcNoise] = 0.012f;
    c.eqBand[DC::EQ_HP] = { true, 400.0f, 0.0f, 0.707f };
    c.volume = 0.6f;
}
// ---- Cymbals (+2) ----
static void mChina(DC& c) {        // trashy china: wide fuzzy purple noise, explosive then gone
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 4; c.layerNoiseCenter = 8800.0f; c.layerNoiseWidth = 0.5f;
    c.srcAtk[DC::SrcNoise] = 0.0005f; c.srcDec[DC::SrcNoise] = 0.85f;
    c.eqBand[DC::EQ_HP] = { true, 800.0f, 0.0f, 0.707f };
    c.driveType = DC::Fuzz; c.driveAmount = 0.469f; c.volume = 0.62f;
}
static void mBellRide(DC& c) {     // ride BELL ping: inharmonic struck metal, none of the noise wash
    auto& s = mkModal(c);
    s.modalMaterial = 1; s.oscFreq = 1320.0f; s.modalDecay = 0.45f;
    s.modalTone = 0.75f; s.modalStruct = 0.7f; s.modalHit = 0.3f;
    c.volume = 0.62f;
}
// ---- Electro Perc (+6) ----
static void mBlip(DC& c) {         // tiny sonar blip: pure high sine dot
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 1400.0f;
    c.layerSinePEnvAmt = 6.0f; c.layerSinePEnvTime = 0.01f;
    c.srcAtk[DC::SrcOsc] = 0.0005f; c.srcDec[DC::SrcOsc] = 0.05f;
    c.volume = 0.7f;
}
static void mLaserZap(DC& c) {     // long TONAL laser fall (Zap stays the short noisy one)
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSquare; c.layerSineFreq = 180.0f;
    c.layerSinePEnvAmt = 42.0f; c.layerSinePEnvTime = 0.16f;
    c.srcAtk[DC::SrcOsc] = 0.0005f; c.srcDec[DC::SrcOsc] = 0.3f;
    c.delaySend = 0.18f; c.volume = 0.7f;
}
static void mChirp(DC& c) {        // fast UP-chirp (negative env = rises into the note)
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 900.0f;
    c.layerSinePEnvAmt = -30.0f; c.layerSinePEnvTime = 0.05f;
    c.srcAtk[DC::SrcOsc] = 0.0005f; c.srcDec[DC::SrcOsc] = 0.08f;
    c.volume = 0.72f;
}
static void mStaticHit(DC& c) {    // burst of crackle static (slot noise: crackle needs the slot engine)
    clearSound(c);
    DC::Slot& n = c.slots[0]; n.engine = DC::SrcNoise; n.weight = 1.0f;
    n.noiseType = 0; n.noiseCenter = 3000.0f; n.noiseWidth = 0.4f;
    n.noiseCrackle = 0.85f; n.noiseDrive = 0.3f;
    n.atk = 0.0005f; n.dec = 0.12f;
    c.volume = 0.78f;
}
static void mBuzzHit(DC& c) {      // 8-bit buzz stab: low square driven hard, no pitch motion
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSquare; c.layerSineFreq = 110.0f;
    c.srcAtk[DC::SrcOsc] = 0.0005f; c.srcDec[DC::SrcOsc] = 0.1f;
    c.driveType = DC::HardClip; c.driveAmount = 0.671f; c.volume = 0.78f;
}
static void mGlitchTick(DC& c) {   // 4 ms digital tick (way shorter + lower than Tight Hat)
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 5200.0f; c.layerNoiseWidth = 0.12f;
    c.srcAtk[DC::SrcNoise] = 0.0002f; c.srcDec[DC::SrcNoise] = 0.004f;
    c.driveType = DC::Bitcrush; c.driveAmount = 0.35f; c.volume = 0.75f;
}
// ---- Keys (+5) ----
static void kToyPiano(DC& c) {     // short dark toy piano: dull glass strike, fast die-away
    auto& s = mkModal(c);
    s.modalMaterial = 2; s.oscFreq = 523.25f; s.modalDecay = 0.32f;
    s.modalTone = 0.35f; s.modalStruct = 0.4f; s.modalHit = 0.3f;
    s.atk = 0.001f;
    c.volume = 0.8f;
}
static void kHarpsichord(DC& c) {  // plucked quill: bright stiff string, no sustain (true to the instrument)
    auto& s = mkSlot(c, DC::SrcPhys);
    s.physFreq = 261.63f; s.physMaterial = 1.0f;
    s.physTone = 0.85f; s.physPosition = 0.12f; s.physStiff = 0.08f;
    s.atk = 0.001f; s.dec = 0.8f; s.sustain = 0.0f; s.release = 0.1f;
    c.volume = 0.8f;
}
static void kPipeOrgan(DC& c) {    // church organ: octave stack + a 16-foot sub rank, wet stone room
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 9; s.oscFreq = 261.63f;               // Organ additive stack
    s.atk = 0.03f; s.dec = 0.3f; s.sustain = 1.0f; s.release = 0.35f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.66f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 261.63f;      // the sub rank (dropped an octave below)
    b.atk = 0.05f; b.dec = 0.3f; b.sustain = 1.0f; b.release = 0.4f;
    c.keysSlot2Down = 12;
    c.reverbSend = 0.35f; c.volume = 0.66f;
}
static void kAccordion(DC& c) {    // musette accordion: detuned reed pair, breath attack, slight wobble
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 12; s.oscFreq = 261.63f;              // Reed
    s.oscUnison = 2; s.oscDetune = 0.3f;                             // the musette detune IS the sound
    s.vibrato = 0.12f;
    s.atk = 0.06f; s.dec = 0.4f; s.sustain = 1.0f; s.release = 0.12f;
    c.volume = 0.7f;
}
static void kHonkyTonk(DC& c) {    // saloon piano: detuned "string pairs" beating (barroom shimmer)
    auto& s = mkSlot(c, DC::SrcOsc);                                 // ONE slot now - the old 2nd slot was
    s.oscShape = s.oscShapeB = 10; s.oscFreq = 261.63f;              // near-identical (only its detune differed),
    s.fmDepth = 0.12f; s.fmPitch = 0.5f; s.fmEnvFollow = true;       // so the honky beat comes from 4 detuned
    s.atk = 0.002f; s.dec = 1.3f; s.sustain = 0.2f; s.release = 0.25f;   // unison voices in a single slot instead.
    s.oscUnison = 4; s.oscDetune = 0.10f;
    c.volume = 0.72f;
}
// ---- Pads & Choirs (+5) ----
static void kChoirOoh(DC& c) {     // darker "ooh" choir (Vowel O vs Choir Aah's bright A)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 7; s.oscFreq = 261.63f;               // Vowel O
    s.oscUnison = 3; s.oscDetune = 0.14f;
    s.atk = 0.22f; s.dec = 1.6f; s.sustain = 0.85f; s.release = 0.9f;
    c.reverbSend = 0.3f; c.volume = 0.66f;
}
static void kDarkPad(DC& c) {      // subterranean pad: low square through a nearly-shut filter
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSquare; s.oscFreq = 130.81f;    // C3
    s.oscUnison = 2; s.oscDetune = 0.18f;
    s.atk = 0.5f; s.dec = 2.2f; s.sustain = 0.9f; s.release = 1.4f;
    s.filterType = DC::LowPass; s.filterCutoff = 420.0f; s.filterReso = 1.1f;
    c.reverbSend = 0.25f; c.volume = 0.7f;
}
static void kShimmerPad(DC& c) {   // airy glass shimmer an octave up, drowned in reverb
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 11; s.oscFreq = 523.25f;              // Glass, C5
    s.oscUnison = 3; s.oscDetune = 0.2f; s.uniSpread = 0.6f;         // wide stereo sparkle
    s.atk = 0.35f; s.dec = 2.5f; s.sustain = 0.75f; s.release = 1.8f;
    c.reverbSend = 0.5f; c.volume = 0.6f;
}
static void kMotionPad(DC& c) {    // slow filter-breathing pad (the LFO IS the character)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 196.0f;        // G3
    s.oscUnison = 3; s.oscDetune = 0.24f;
    s.atk = 0.3f; s.dec = 2.0f; s.sustain = 0.85f; s.release = 1.0f;
    s.filterType = DC::LowPass; s.filterCutoff = 900.0f; s.filterReso = 1.3f;
    s.lfoRate[0] = 0.3f; s.lfoAmt[0] = 0.55f; lfoRoute(s, 0, DC::MTFilt1Cut);   // ~3 s filter breathing
    c.reverbSend = 0.2f; c.volume = 0.66f;
}
static void kBrassPad(DC& c) {     // soft brass-section swell (slow vs Synth Brass's stab attack)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 13; s.oscFreq = 349.23f;              // Brass, F4 register
    s.oscUnison = 2; s.oscDetune = 0.12f;
    s.atk = 0.25f; s.dec = 1.8f; s.sustain = 0.85f; s.release = 0.6f;
    s.filterType = DC::LowPass; s.filterCutoff = 2600.0f; s.filterReso = 0.7f;
    c.reverbSend = 0.22f; c.volume = 0.66f;
}
// ---- Leads (+2) ----
static void kPulseLead(DC& c) {    // hollow pulse lead: thin PWM-flavoured pair
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvPulse; s.oscFreq = 261.63f;
    s.oscUnison = 2; s.oscDetune = 0.16f;
    s.atk = 0.004f; s.dec = 0.5f; s.sustain = 0.85f; s.release = 0.1f;
    s.filterType = DC::LowPass; s.filterCutoff = 3200.0f; s.filterReso = 1.0f;
    c.volume = 0.72f;
}
static void kFMLead(DC& c) {       // glassy FM lead: 2x modulator that mellows as the note fades
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 261.63f;
    s.fmDepth = 0.5f; s.fmPitch = 0.2f; s.fmEnvFollow = true;        // ratio snaps 2x
    s.atk = 0.003f; s.dec = 0.7f; s.sustain = 0.8f; s.release = 0.12f;
    c.delaySend = 0.12f; c.volume = 0.7f;
}
// ---- Bells & Mallets (+2) ----
static void moSteelDrum(DC& c) {   // Trinidad steel pan: tuned metal with the "bloom" wobble
    auto& s = mkModal(c);
    s.modalMaterial = 1; s.oscFreq = 349.23f; s.modalDecay = 0.5f;
    s.modalTone = 0.7f; s.modalStruct = 0.3f; s.modalHit = 0.55f;
    s.vibrato = 0.16f;                                               // the pan's pitch bloom
    c.reverbSend = 0.15f; c.volume = 0.78f;
}
// ---- Chords & Arps (+5): every one SHIPS voiced or with a dedicated arp ----
static void kHouseStab(DC& c) {    // classic house piano-organ stab: Min7 voicing baked in
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 9; s.oscFreq = 261.63f;               // Organ
    s.scaleOn = true; s.scaleType = 1; s.scaleUnison = 4; s.scaleKey = 0;   // natural-minor 7ths:
    // C plays Cm7 exactly like the old fixed Min7 chord - but SCALE mode has UI (chordMode has
    // none since the redesign), so the voicing is fully user-replicable + editable (user rule).
    s.atk = 0.002f; s.dec = 0.4f; s.sustain = 0.0f; s.release = 0.08f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.316f;
    c.eqBand[DC::EQ_HP] = { true, 160.0f, 0.0f, 0.707f };
    c.reverbSend = 0.15f; c.volume = 0.78f;
}
static void kRaveStab(DC& c) {     // hoover-adjacent rave stab: detuned saws voiced as a MAJOR triad
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.scaleOn = true; s.scaleType = 0; s.scaleUnison = 3; s.scaleKey = 0;   // Major triads (C = C-E-G,
    // same as the old fixed Maj chord on the root; SCALE has UI, chordMode does not - user rule)
    s.oscDetune = 0.2f; s.uniSpread = 0.4f;
    s.atk = 0.002f; s.dec = 0.5f; s.sustain = 0.0f; s.release = 0.1f;
    s.fxDriveType = DC::Foldback; s.fxDrive = 0.5f;
    c.eqBand[DC::EQ_HP] = { true, 120.0f, 0.0f, 0.707f };
    c.volume = 0.72f;
}
static void kTranceArp(DC& c) {    // DEDICATED ARP: octave-bounce 16ths on a bright pluck
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.atk = 0.001f; s.dec = 0.22f; s.sustain = 0.0f; s.release = 0.06f;
    s.filterType = DC::LowPass; s.filterCutoff = 2600.0f; s.filterReso = 1.5f; s.filterEnvAmt = 0.45f;
    c.arpOn = true; c.arpLen = 4; c.arpSync = 8; c.arpRate = 8;      // 8 x2 = 16ths
    c.arpOffset[0] = 12; c.arpOffset[1] = 0; c.arpOffset[2] = 12;    // root/+oct/root/+oct
    c.arpGate = 0.75f;
    c.delaySend = 0.2f; c.volume = 0.74f;
}
static void kAcidArp(DC& c) {      // DEDICATED ARP: mono acid line hopping octaves, squelchy filter
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 130.81f;       // C3 bass register
    s.atk = 0.001f; s.dec = 0.3f; s.sustain = 0.0f; s.release = 0.05f;
    s.filterType = DC::LowPass; s.filterCutoff = 700.0f; s.filterReso = 2.6f; s.filterEnvAmt = 0.6f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.447f;
    c.keysPolyMode = false;                                          // acid is a MONO instrument
    c.arpOn = true; c.arpLen = 4; c.arpSync = 8; c.arpRate = 8;
    c.arpOffset[0] = 0; c.arpOffset[1] = 12; c.arpOffset[2] = -12;   // root/root/+oct/-oct
    c.arpGate = 0.55f;                                               // staccato squelch
    c.volume = 0.8f;
}
static void kDreamArp(DC& c) {     // DEDICATED ARP: slow broken major chord on a glass pluck
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 11; s.oscFreq = 261.63f;              // Glass
    s.atk = 0.002f; s.dec = 0.6f; s.sustain = 0.0f; s.release = 0.3f;
    c.arpOn = true; c.arpLen = 6; c.arpSync = 8; c.arpRate = 4;      // 8 x1 = 8ths, unhurried
    c.arpOffset[0] = 4; c.arpOffset[1] = 7; c.arpOffset[2] = 12;     // root-3rd-5th-oct-5th-3rd
    c.arpOffset[3] = 7; c.arpOffset[4] = 4;
    c.arpGate = 1.0f;                                                // notes ring into each other
    c.reverbSend = 0.35f; c.delaySend = 0.15f; c.volume = 0.7f;
}
// ---- Risers & Falls (+4) ----
static void mUplifter(DC& c) {     // trance uplifter: noise swell + a tone RISING two octaves under it
    clearSound(c);
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.45f;
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.55f;
    c.padX = 0.14f + 0.82f * 0.55f; c.padY = 0.5f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 220.0f;
    c.layerSinePEnvAmt = -24.0f; c.layerSinePEnvTime = 1.4f;         // negative = rises INTO the note
    c.srcAtk[DC::SrcOsc] = 0.6f; c.srcDec[DC::SrcOsc] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 5000.0f; c.layerNoiseWidth = 0.35f;
    c.srcAtk[DC::SrcNoise] = 1.2f; c.srcDec[DC::SrcNoise] = 0.3f;
    c.reverbSend = 0.4f; c.volume = 0.6f;
}
static void mTapeStop(DC& c) {     // tape-stop fall: a held tone that suddenly winds down
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 330.0f;
    c.layerSinePEnvAmt = 36.0f; c.layerSinePEnvTime = 0.4f; c.layerSinePOffset = 0.08f;
    c.srcAtk[DC::SrcOsc] = 0.002f; c.srcDec[DC::SrcOsc] = 0.55f;
    c.driveType = DC::Tube; c.driveAmount = 0.424f; c.volume = 0.72f;
}
static void mDive(DC& c) {         // long whistling dive-bomb: high square falling for a full second
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSquare; c.layerSineFreq = 880.0f;
    c.layerSinePEnvAmt = 24.0f; c.layerSinePEnvTime = 0.9f;
    c.srcAtk[DC::SrcOsc] = 0.005f; c.srcDec[DC::SrcOsc] = 1.1f;
    c.reverbSend = 0.2f; c.delaySend = 0.15f; c.volume = 0.62f;
}
static void mSubRise(DC& c) {      // felt-not-heard sub swell rising an octave (transition weight)
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 40.0f;
    c.layerSinePEnvAmt = -12.0f; c.layerSinePEnvTime = 1.3f;
    c.srcAtk[DC::SrcOsc] = 0.4f; c.srcDec[DC::SrcOsc] = 1.3f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.316f; c.volume = 0.85f;
}
// ---- Impacts & Booms (+5) ----
static void mSlam(DC& c) {         // door slam: broadband smack + a low thump, clipped
    clearSound(c);
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.45f;
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.55f;
    c.padX = 0.14f + 0.82f * 0.55f; c.padY = 0.5f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 60.0f;
    c.layerSinePEnvAmt = 30.0f; c.layerSinePEnvTime = 0.02f;
    c.srcAtk[DC::SrcOsc] = 0.0005f; c.srcDec[DC::SrcOsc] = 0.3f;
    c.noiseType = 0; c.layerNoiseCenter = 900.0f; c.layerNoiseWidth = 0.45f;
    c.srcAtk[DC::SrcNoise] = 0.0005f; c.srcDec[DC::SrcNoise] = 0.25f;
    c.driveType = DC::HardClip; c.driveAmount = 0.592f;
    c.reverbSend = 0.3f; c.volume = 0.85f;
}
static void mThud(DC& c) {         // dead body-blow thud: low knock, zero ring, bone dry
    clearSound(c);
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.9f;
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.1f;
    c.padX = 0.14f + 0.82f * 0.1f; c.padY = 0.5f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 55.0f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.12f;
    c.noiseType = 1; c.layerNoiseCenter = 500.0f; c.layerNoiseWidth = 0.2f;
    c.srcAtk[DC::SrcNoise] = 0.0005f; c.srcDec[DC::SrcNoise] = 0.03f;
    c.volume = 0.9f;
}
static void mBlast(DC& c) {        // distorted FM blast: mid-band explosion, fuzz all over
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -12.0f; c.fmSpread = 0.3f; c.fmDepth = 0.7f; c.fmSub = 0.3f;
    c.fmPitchEnvAmt = 26.0f; c.fmPitchEnvTime = 0.05f;
    c.srcDec[DC::SrcFM] = 0.6f;
    c.driveType = DC::Fuzz; c.driveAmount = 0.707f;
    c.reverbSend = 0.25f; c.volume = 0.8f;
}
static void mBraam(DC& c) {        // trailer BRAAM: detuned saw wall, foldback, slow bloom
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 65.41f;        // C2
    s.oscUnison = 5; s.oscDetune = 0.5f; s.uniSpread = 0.5f;
    s.atk = 0.02f; s.dec = 1.6f; s.sustain = 0.0f; s.release = 0.4f;
    s.filterType = DC::LowPass; s.filterCutoff = 950.0f; s.filterReso = 1.2f;
    s.fxDriveType = DC::Foldback; s.fxDrive = 0.632f;
    c.reverbSend = 0.35f; c.volume = 0.72f;
}
// ---- Noise & Texture (+2) ----
static void mRain(DC& c) {         // steady rain bed: pink noise + dense crackle droplets
    clearSound(c);
    DC::Slot& n = c.slots[0]; n.engine = DC::SrcNoise; n.weight = 1.0f;
    n.noiseType = 1; n.noiseCenter = 4500.0f; n.noiseWidth = 0.4f;
    n.noiseCrackle = 0.55f;
    n.atk = 0.3f; n.dec = 3.0f;
    c.volume = 0.5f;
}
static void mOcean(DC& c) {        // ocean swell: brown noise breathing at wave speed (vol LFO)
    clearSound(c);
    DC::Slot& n = c.slots[0]; n.engine = DC::SrcNoise; n.weight = 1.0f;
    n.noiseType = 2; n.noiseCenter = 420.0f; n.noiseWidth = 0.6f;
    n.lfoRate[2] = 0.16f; n.lfoAmt[2] = 0.9f; lfoRoute(n, 2, DC::MTVol);   // ~6 s near-full wave swell
    n.atk = 0.4f; n.dec = 3.5f;
    c.volume = 0.6f;
}


// ---- Guitar batch (2026-07-08, user order: bass guitars + guitars + strum-ready chords). All
// Karplus-Strong (THE plucked-string engine), all C-based, no hidden state (UIAudit-checked). ----
static DC::Slot& mkGtr(DC& c, float hz, float tone, float pos, float dec)
{
    auto& s = mkSlot(c, DC::SrcPhys);
    s.physFreq = hz; s.physMaterial = 1.0f;              // steel string
    s.physTone = tone; s.physPosition = pos;
    s.physStiff = 0.03f;                                 // a hint of real-string inharmonicity
    s.oscDetune = 0.05f;                                 // strings never PERFECTLY agree (chord shimmer)
    s.atk = 0.001f; s.dec = dec;
    s.release = 0.5f;                                    // strings RING on release (arp gate / note ends
                                                         // used to chop them with the 60 ms default)
    // (Removed the +1 dB "guitar body" EQ bells: too subtle to see on the EQ curve = felt hidden.
    //  The guitars now use ZERO EQ - fully visible. Imperceptible sound change.)
    return s;
}
static void gFingerBass(DC& c) {   // fingered electric bass: warm, round, played near the neck
    auto& s = mkGtr(c, 65.41f, 0.35f, 0.32f, 0.9f);      // C2
    s.filterType = DC::LowPass; s.filterCutoff = 1100.0f; s.filterReso = 0.7f;
    c.volume = 0.88f;
}
static void gPickBass(DC& c) {     // picked bass: brighter attack, close to the bridge = twang
    auto& s = mkGtr(c, 65.41f, 0.72f, 0.12f, 0.8f);
    s.fxDriveType = DC::Tube; s.fxDrive = 0.316f;
    c.volume = 0.85f;
}
static void gMutedBass(DC& c) {    // palm-muted bass: short thump, all fundamental
    auto& s = mkGtr(c, 65.41f, 0.4f, 0.25f, 0.18f);
    s.fxDriveType = DC::SoftClip; s.fxDrive = 0.387f;
    c.volume = 0.9f;
}
static void gFuzzBass(DC& c) {     // fuzz bass: KS string into heavy fuzz - gnarly sustain
    auto& s = mkGtr(c, 65.41f, 0.6f, 0.2f, 0.7f);
    s.fxDriveType = DC::Fuzz; s.fxDrive = 0.671f;
    s.filterType = DC::LowPass; s.filterCutoff = 1600.0f; s.filterReso = 0.8f;
    c.volume = 0.78f;
}
static void gSteelGuitar(DC& c) {  // steel-string acoustic: bright, singing, a touch of room.
    auto& s = mkGtr(c, 130.81f, 0.68f, 0.2f, 1.2f);      // C3. Ships IN the guitar voicing (user
    s.scaleOn = true; s.scaleType = 10; s.scaleKey = 0;  // order) - string count is AUTO per chord.
    c.reverbSend = 0.12f;
    c.strumAmt = 0.6f;                                   // strummed by default (strum is stepped 0/20/40/60/80/100%)
    c.volume = 0.8f;
}
static void gAmpBass(DC& c) {      // finger bass through the BASS AMP split rig: fat lows, driven mids
    auto& s = mkGtr(c, 65.41f, 0.45f, 0.3f, 0.85f);
    s.fxDriveType = DC::DriveBassAmp; s.fxDrive = 0.55f;
    s.filterType = DC::LowPass; s.filterCutoff = 2200.0f; s.filterReso = 0.75f;
    c.volume = 0.85f;
}
static void gMutedGuitar(DC& c) {  // funk mute: the percussive "chick" scratch
    auto& s = mkGtr(c, 130.81f, 0.55f, 0.15f, 0.12f);
    c.volume = 0.85f;
}
static void gStrumAcoustic(DC& c) {   // GUITAR-VOICED chords (E-shape barre, 6 notes): turn the KEYS
    auto& s = mkGtr(c, 130.81f, 0.66f, 0.2f, 1.2f);      // rings like a real strummed acoustic -
    s.scaleOn = true; s.scaleType = 10; s.scaleKey = 0;  // Gtr Major, AUTO string count
    c.reverbSend = 0.14f;
    c.strumAmt = 0.6f;                                   // ships STRUMMED (strum rides with sounds now)
    c.volume = 0.72f;
}
static void gStrumElectric(DC& c) {   // minor-voiced electric strum (same shape, minor third)
    auto& s = mkGtr(c, 130.81f, 0.5f, 0.28f, 1.3f);
    s.scaleOn = true; s.scaleType = 11; s.scaleKey = 0;  // Gtr Minor, AUTO string count
    s.fxDriveType = DC::Tube; s.fxDrive = 0.283f;
    c.strumAmt = 0.6f;
    c.volume = 0.72f;
}

//==============================================================================
// v1.3.5 SHOWCASE batch - the new per-slot FILTER MODES (HP/BP/Notch) + cutoff KEYTRACK, the
// multi-voice STEREO CHORUS, and TEMPO-SYNCED LFOs (incl. Lock-to-grid). Slot-authored -> never
// referenced from presets. At least half also use the shared reverb/delay.
//==============================================================================
static void nChorusPad(DC& c) {     // lush wide pad: detuned saws under a soft LP, drenched in stereo chorus + reverb
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 5; s.oscDetune = 0.28f; s.uniSpread = 0.5f;
    s.atk = 0.22f; s.dec = 1.6f; s.sustain = 0.85f; s.release = 0.9f;
    s.filterType = DC::LowPass; s.filterCutoff = 1600.0f; s.filterReso = 0.8f;
    chFx(c, DC::ChFxChorus, 0.7f);   // slow, deep ensemble
    c.reverbSend = 0.3f; c.volume = 0.58f;
}
static void nChorusEP(DC& c) {      // chorused electric piano: FM tine + a sine body, classic chorus + slap-back delay
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 261.63f;
    s.fmDepth = 0.55f; s.fmPitch = 14.0f; s.fmEnvFollow = true;       // bell-ish tine attack
    s.atk = 0.003f; s.dec = 1.4f; s.sustain = 0.35f; s.release = 0.5f;
    chFx(c, DC::ChFxChorus, 0.55f);
    auto& b = mkSlot2(c, DC::SrcOsc, 0.5f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 261.63f;
    b.atk = 0.003f; b.dec = 1.6f; b.sustain = 0.3f; b.release = 0.5f;
    c.delaySend = 0.18f; c.reverbSend = 0.1f; c.volume = 0.64f;
}
static void nHyperSaw(DC& c) {      // huge trance lead: 7-voice saw, a tempo-synced 1/8 filter pulse, chorus + delay
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 7; s.oscDetune = 0.35f; s.uniSpread = 0.7f;
    s.atk = 0.01f; s.dec = 1.0f; s.sustain = 0.8f; s.release = 0.3f;
    s.filterType = DC::LowPass; s.filterCutoff = 2200.0f; s.filterReso = 1.4f;
    s.lfoAmt[0] = 0.5f; s.lfoSync[0] = 8.0f; lfoRoute(s, 0, DC::MTFilt1Cut);   // 8 cycles/bar = 1/8-note filter pulse (tempo-synced)
    chFx(c, DC::ChFxChorus, 0.5f);
    c.delaySend = 0.2f; c.volume = 0.56f;
}
static void nDreamPluck(DC& c) {    // bright pluck that stays even across the keyboard (KEYTRACK), chorus + reverb
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.atk = 0.002f; s.dec = 0.5f; s.sustain = 0.0f; s.release = 0.35f;
    s.filterType = DC::LowPass; s.filterCutoff = 1200.0f; s.filterReso = 2.5f; s.filterEnvAmt = 0.5f;
    ktRoute(s, 0.8f);                    // cutoff tracks the note -> high notes stay bright, lows stay warm
    chFx(c, DC::ChFxChorus, 0.6f);
    c.reverbSend = 0.28f; c.volume = 0.66f;
}
// ---- CHANNEL FX showcases (v1.3.9): the Flanger/Phaser act on the WHOLE instrument, and a slot's mod
//      matrix drives them via the "... (Channel)" targets - so the effect DEPTH itself is modulated. ----
static void nJetPad(DC& c) {        // wide saw pad through a slow-breathing FLANGER (LFO -> Flanger (Channel))
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 5; s.oscDetune = 0.22f; s.uniSpread = 0.45f;
    s.atk = 0.25f; s.dec = 1.8f; s.sustain = 0.85f; s.release = 1.0f;
    s.filterType = DC::LowPass; s.filterCutoff = 2400.0f; s.filterReso = 0.7f;
    s.lfoRate[0] = 0.12f; s.lfoAmt[0] = 0.55f; s.lfoFree[0] = true;   // very slow, free-running
    modRoute(s, DC::MSLfoFilt, DC::MTChFxAAmt, 0.6f);                 // LFO 1 -> FX A Amount (the flanger) - channel
    chFx(c, DC::ChFxFlanger, 0.4f);                                               // base sweep; the LFO breathes it
    c.reverbSend = 0.3f; c.volume = 0.56f;
}
static void nPhaseKeys(DC& c) {     // swirling electric piano: slow LFO -> Phaser (Channel), a touch of chorus
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 261.63f;
    s.fmDepth = 0.5f; s.fmPitch = 14.0f; s.fmEnvFollow = true;        // tine attack
    s.atk = 0.003f; s.dec = 1.5f; s.sustain = 0.4f; s.release = 0.6f;
    s.lfoRate[1] = 0.22f; s.lfoAmt[1] = 0.5f; s.lfoFree[1] = true;
    modRoute(s, DC::MSLfoPitch, DC::MTChFxAAmt, 0.5f);                // LFO 2 -> FX A Amount (the phaser) - channel
    auto& b = mkSlot2(c, DC::SrcOsc, 0.55f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 261.63f;
    b.atk = 0.004f; b.dec = 1.7f; b.sustain = 0.35f; b.release = 0.6f;
    chFx(c, DC::ChFxPhaser, 0.5f); chFx(c, DC::ChFxComp, 0.3f);       // swirl, glued (2 slots; the old 0.25 chorus dropped - disclosed)
    c.reverbSend = 0.16f; c.volume = 0.6f;
}
static void nSwirlLead(DC& c) {     // saw lead where HARDER notes swirl more (Velocity -> Phaser (Channel))
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.14f; s.uniSpread = 0.3f;
    s.atk = 0.008f; s.dec = 0.9f; s.sustain = 0.7f; s.release = 0.3f;
    s.filterType = DC::LowPass; s.filterCutoff = 2800.0f; s.filterReso = 1.2f;
    modRoute(s, DC::MSVel, DC::MTChFxAAmt, 0.55f);                    // velocity -> FX A Amount (the phaser) - channel
    chFx(c, DC::ChFxPhaser, 0.25f); chFx(c, DC::ChFxComp, 0.35f);
    c.delaySend = 0.18f; c.volume = 0.58f;
}
// ---- SUB + FORMANT showcases (v1.3.9): the two new FX knobs doing what nothing else can - a clean
//      tracked octave-down under the sound, and a vowel filter that TALKS when the matrix sweeps it. ----
static void nTalkingBass(DC& c) {  // FORMANT swept by a free LFO = the bass literally talks; big SUB underneath
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 110.0f;
    s.atk = 0.004f; s.dec = 0.8f; s.sustain = 0.8f; s.release = 0.15f;
    s.fxFormant = 0.4f;                                   // parked mid-vowel; the LFO does the talking
    s.fxSub = 0.7f;                                       // clean octave-down = the weight
    s.lfoRate[0] = 0.35f; s.lfoAmt[0] = 0.8f; s.lfoFree[0] = true;
    modRoute(s, DC::MSLfoFilt, DC::MTFormant, 0.5f);      // LFO 1 -> Formant = ah-oh-ee vowels
    chFx(c, DC::ChFxComp, 0.35f); c.volume = 0.8f;
}
static void nVowelPad(DC& c) {     // slow vowel morph across a wide saw pad = a choir-ish "aah-ooh" drift
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 5; s.oscDetune = 0.2f; s.uniSpread = 0.5f;
    s.atk = 0.3f; s.dec = 1.6f; s.sustain = 0.85f; s.release = 0.9f;
    s.fxFormant = 0.5f;
    s.lfoRate[0] = 0.08f; s.lfoAmt[0] = 0.7f; s.lfoFree[0] = true;
    modRoute(s, DC::MSLfoFilt, DC::MTFormant, 0.45f);     // very slow free vowel drift
    chFx(c, DC::ChFxChorus, 0.3f); c.reverbSend = 0.3f; c.volume = 0.6f;
}
static void nSubPluck(DC& c) {     // bright resonant pluck with a BIG clean octave-down = instant bass hook
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 130.81f;   // C3 - the sub lands at C2
    s.atk = 0.002f; s.dec = 0.5f; s.sustain = 0.0f; s.release = 0.3f;
    s.filterType = DC::LowPass; s.filterCutoff = 2500.0f; s.filterReso = 1.8f; s.filterEnvAmt = 0.4f;
    s.fxSub = 0.85f;                                      // the sub bypasses the filter = always clean
    c.volume = 0.78f;
}
// ---- AUDIO-RATE MODULATION showcases (v1.3.9): LFO -> Pitch/Volume run PER SAMPLE now, and the
//      KEY sync mode locks the LFO rate to the played pitch x a ratio = real, playable FM/AM. ----
static void nFmGrowl(DC& c) {      // THE audio-rate demo: KEY-tracked LFO -> Pitch = an FM growl bass
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 110.0f;
    s.atk = 0.004f; s.dec = 0.7f; s.sustain = 0.8f; s.release = 0.12f;
    s.filterType = DC::LowPass; s.filterCutoff = 1400.0f; s.filterReso = 1.6f; s.filterEnvAmt = 0.35f;
    s.lfoSync[0] = -2.0f; s.lfoRate[0] = 1.0f; s.lfoAmt[0] = 0.5f;   // KEY mode, ratio x1
    modRoute(s, DC::MSLfoFilt, DC::MTPitch, 0.5f);                   // audio-rate FM (per sample)
    s.fxSub = 0.5f;
    chFx(c, DC::ChFxComp, 0.4f); c.volume = 0.78f;
}
static void nWolfLead(DC& c) {     // aggressive FM lead: KEY ratio x2 -> Pitch + drive = snarling harmonics
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 2; s.oscDetune = 0.08f;
    s.atk = 0.006f; s.dec = 0.8f; s.sustain = 0.75f; s.release = 0.2f;
    s.lfoSync[0] = -2.0f; s.lfoRate[0] = 2.0f; s.lfoAmt[0] = 0.4f;   // KEY, ratio x2 = bright FM bite
    modRoute(s, DC::MSLfoFilt, DC::MTPitch, 0.4f);
    s.fxDriveType = DC::Tube; s.fxDrive = 0.3f;
    c.delaySend = 0.15f; c.volume = 0.6f;
}
static void nMetalEP(DC& c) {      // DX-style metallic e-piano: KEY ratio x3.5 (inharmonic) -> Pitch on a sine
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 261.63f;
    s.atk = 0.003f; s.dec = 1.4f; s.sustain = 0.3f; s.release = 0.5f;
    s.lfoSync[0] = -2.0f; s.lfoRate[0] = 3.5f; s.lfoAmt[0] = 0.35f;  // inharmonic ratio = the metal
    modRoute(s, DC::MSLfoFilt, DC::MTPitch, 0.35f);
    chFx(c, DC::ChFxComp, 0.3f); c.reverbSend = 0.15f; c.volume = 0.66f;
}
static void nFmBellTone(DC& c) {   // classic FM bell: KEY ratio x3 -> Pitch, struck and ringing
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 523.25f;   // C5
    s.atk = 0.002f; s.dec = 1.8f; s.sustain = 0.0f; s.release = 0.6f;
    s.lfoSync[0] = -2.0f; s.lfoRate[0] = 3.0f; s.lfoAmt[0] = 0.45f;
    modRoute(s, DC::MSLfoFilt, DC::MTPitch, 0.45f);
    c.reverbSend = 0.3f; c.volume = 0.7f;
}
static void nAmShimmer(DC& c) {    // octave AM shimmer pad: KEY ratio x2 -> VOLUME (per-sample AM = new partials)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 4; s.oscDetune = 0.18f; s.uniSpread = 0.5f;
    s.atk = 0.4f; s.dec = 1.8f; s.sustain = 0.85f; s.release = 1.1f;
    s.filterType = DC::LowPass; s.filterCutoff = 2600.0f; s.filterReso = 0.7f;
    s.lfoSync[0] = -2.0f; s.lfoRate[0] = 2.0f; s.lfoAmt[0] = 0.5f;   // KEY x2 -> AM at the octave
    modRoute(s, DC::MSLfoFilt, DC::MTVol, 0.6f);                     // audio-rate AM (per sample)
    chFx(c, DC::ChFxChorus, 0.35f);
    c.reverbSend = 0.32f; c.volume = 0.56f;
}
static void nRobotDrone(DC& c) {   // robotic vocal drone: deep audio-rate AM + a parked vowel
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 130.81f;   // C3
    s.atk = 0.15f; s.dec = 1.2f; s.sustain = 0.9f; s.release = 0.5f;
    s.fxFormant = 0.3f;                                          // vowel body
    s.lfoSync[0] = -2.0f; s.lfoRate[0] = 1.0f; s.lfoAmt[0] = 0.8f;
    modRoute(s, DC::MSLfoFilt, DC::MTVol, 0.9f);                 // near-full AM = robot buzz
    chFx(c, DC::ChFxComp, 0.35f); c.volume = 0.68f;
}
static void nFmClang(DC& c) {      // metallic clang hit: FIXED 780 Hz LFO -> Pitch on a short sine strike
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 320.0f;
    s.atk = 0.001f; s.dec = 0.35f; s.sustain = 0.0f; s.release = 0.08f;
    s.lfoRate[0] = 780.0f; s.lfoAmt[0] = 0.5f;                   // fixed audio-rate = inharmonic clang
    modRoute(s, DC::MSLfoFilt, DC::MTPitch, 0.55f);
    s.fxPunch = 0.4f; c.reverbSend = 0.18f; c.volume = 0.8f;
}
static void nRingChime(DC& c) {    // TUNED ring mod: Ring at full with the carrier TRACKING the note
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 523.25f;
    s.atk = 0.002f; s.dec = 1.4f; s.sustain = 0.0f; s.release = 0.5f;
    s.fxRing = 0.65f; s.fxRingHz = 25.0f;                        // hard left = TRACK the note (tuned ring)
    c.reverbSend = 0.28f; c.volume = 0.72f;
}
static void nJetRiser(DC& c) {     // CHARACTER showcase: a noise riser through a SCREAMING flanger (char 0.92)
    auto& s = mkSlot(c, DC::SrcNoise);
    s.noiseType = 0; s.atk = 1.2f; s.hold = 0.8f; s.dec = 0.8f; s.sustain = 0.0f;
    s.filterType = DC::BandPass; s.filterCutoff = 900.0f; s.filterReso = 1.2f; s.filterEnvAmt = 0.8f;
    chFx(c, DC::ChFxFlanger, 0.8f, 0.92f);                       // character up = fast deep jet
    c.reverbSend = 0.2f; c.volume = 0.7f;
}
static void nScoopedBass(DC& c) {  // BIPOLAR BELL showcase: mids CUT -9 dB = the scooped metal bass
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 65.41f;    // C2
    s.atk = 0.004f; s.dec = 0.7f; s.sustain = 0.75f; s.release = 0.15f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.35f;
    s.filterType2 = DC::Bell; s.filterCutoff2 = 750.0f; s.filterGain2 = -9.0f; s.filterReso2 = 1.0f;   // the SCOOP (bell CUT)
    s.fxSub = 0.4f;
    chFx(c, DC::ChFxComp, 0.4f); c.volume = 0.8f;
}
static void nSidebandPad(DC& c) {  // hazy inharmonic pad: a FIXED 150 Hz LFO -> Pitch at low depth = sideband fog
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvTri; s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.12f; s.uniSpread = 0.4f;
    s.atk = 0.5f; s.dec = 2.0f; s.sustain = 0.85f; s.release = 1.2f;
    s.lfoRate[0] = 150.0f; s.lfoAmt[0] = 0.18f;                  // fixed audio rate = a fog that shifts per note
    modRoute(s, DC::MSLfoFilt, DC::MTPitch, 0.25f);
    chFx(c, DC::ChFxChorus, 0.3f);
    c.reverbSend = 0.35f; c.volume = 0.55f;
}
static void nNotchBass(DC& c) {     // phaser-ish bass: a swept NOTCH + keytrack, tempo-synced 1/2-note notch motion
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 65.41f;         // C2
    s.atk = 0.004f; s.dec = 0.8f; s.sustain = 0.7f; s.release = 0.2f;
    s.filterType = DC::Notch; s.filterCutoff = 500.0f; s.filterReso = 3.0f;
    ktRoute(s, 0.5f);
    s.lfoAmt[0] = 0.6f; s.lfoSync[0] = 2.0f; lfoRoute(s, 0, DC::MTFilt1Cut);   // 1/2-note notch sweep (tempo-synced) = phaser motion
    c.volume = 0.82f;
}
static void nAcidHP(DC& c) {        // thin, buzzy acid lead: HIGH-PASS + resonance, cutoff tracks the note
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSquare; s.oscFreq = 130.81f;     // C3
    s.atk = 0.003f; s.dec = 0.4f; s.sustain = 0.5f; s.release = 0.15f;
    s.filterType = DC::HighPass; s.filterCutoff = 300.0f; s.filterReso = 5.0f; s.filterEnvAmt = 0.6f;
    ktRoute(s, 0.7f);
    c.delaySend = 0.15f; c.volume = 0.72f;
}
static void nVoxBP(DC& c) {         // vocal-ish tone via a resonant BAND-PASS + full keytrack, softened with chorus
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.atk = 0.05f; s.dec = 1.0f; s.sustain = 0.7f; s.release = 0.4f;
    s.filterType = DC::BandPass; s.filterCutoff = 900.0f; s.filterReso = 4.0f;
    ktRoute(s, 1.0f);                    // the formant band tracks the note = a consistent 'vowel'
    chFx(c, DC::ChFxChorus, 0.5f);
    c.reverbSend = 0.2f; c.volume = 0.62f;
}
static void nSyncSweep(DC& c) {     // rhythmic band-pass sweep pad: LFO LOCKED TO THE GRID, big reverb
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.2f;
    s.atk = 0.12f; s.dec = 1.4f; s.sustain = 0.85f; s.release = 0.8f;
    s.filterType = DC::BandPass; s.filterCutoff = 800.0f; s.filterReso = 2.2f;
    s.lfoAmt[0] = 0.85f; s.lfoSync[0] = -1.0f; lfoRoute(s, 0, DC::MTFilt1Cut);   // LOCK TO GRID: one sweep per grid cell (step count / piano-roll grid)
    c.reverbSend = 0.35f; c.volume = 0.6f;
}
static void nChorusBells(DC& c) {   // shimmering tuned bells: modal body + chorus, long reverb + delay tail
    auto& s = mkModal(c); s.modalMaterial = 1; s.oscFreq = 523.25f;  // C5, Tubular
    s.modalDecay = 0.8f; s.modalTone = 0.7f; s.modalStruct = 0.45f;
    chFx(c, DC::ChFxChorus, 0.45f);
    c.reverbSend = 0.35f; c.delaySend = 0.2f; c.volume = 0.58f;
}
static void nAmbientWash(DC& c) {   // evolving ambient bed: soft saw pad + a filtered noise texture, huge chorus,
    auto& s = mkSlot(c, DC::SrcOsc);                                 // slow tempo-synced volume swell + long verb/delay
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 5; s.oscDetune = 0.25f;
    s.atk = 0.4f; s.dec = 2.0f; s.sustain = 0.9f; s.release = 1.4f;
    s.filterType = DC::LowPass; s.filterCutoff = 1000.0f; s.filterReso = 0.7f;
    chFx(c, DC::ChFxChorus, 0.75f);
    s.lfoAmt[2] = 0.4f; s.lfoSync[2] = 1.0f; lfoRoute(s, 2, DC::MTVol);   // 1 cycle/bar volume swell (tempo-synced)
    auto& b = mkSlot2(c, DC::SrcNoise, 0.22f);  // airy noise bed (texture) under the pad
    b.noiseType = 1;                            // pink
    b.atk = 0.6f; b.dec = 2.0f; b.sustain = 0.8f; b.release = 1.4f;
    b.filterType = DC::BandPass; b.filterCutoff = 2500.0f; b.filterReso = 1.2f;
    c.reverbSend = 0.4f; c.delaySend = 0.25f; c.volume = 0.54f;
}


//==============================================================================
// v1.3.5 ADDITIVE batch - 10 sounds whose Wave = Custom (drawn harmonics, addH). Each recipe is
// deliberately different from the factory bank shapes (Reed = pure odd 1/h, Organ = octaves, ...).
// + 6 sounds showcasing the new per-slot Tone / Punch / Comp knobs. All slot-authored.
//==============================================================================
static DC::Slot& mkAdd(DC& c, std::initializer_list<std::pair<int,float>> hs)
{   // one SrcOsc slot playing a DRAWN additive wave (harmonic number, level)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvCustom;
    s.oscFreq = 261.63f;                             // C4 base so STEP pitch 0 = middle C (basses override)
    for (auto& h : hs) if (h.first >= 1 && h.first <= DC::ADD_HARM) s.addH[0][h.first - 1] = h.second;
    s.addH[0][0] = juce::jmax(s.addH[0][0], 0.0f);   // (h1 default 1 unless the recipe overrides)
    for (int f = 1; f < DC::ADD_FRAMES; ++f)         // static sound: all 4 frames identical (position = no-op)
        for (int k = 0; k < DC::ADD_HARM; ++k) { s.addH[f][k] = s.addH[0][k]; s.addPh[f][k] = s.addPh[0][k]; }
    return s;
}
static void wtFrame(DC::Slot& s, int f, std::initializer_list<std::pair<int,float>> hs);   // defined with setB below

// WINDS v4 = v3 finally AUDIBLE (round-12): the round-11 bug was windBreath calling mkSlot, which
// CLEARS the whole channel - every wind's wavetable tone was erased one line after being authored
// and only the quiet noise layer survived ("they're all just noise" - the user heard the bug, not
// the design). The breath is SLOT 2 via mkSlot2 now. Design per wind: a 3-frame wavetable tone
// (hollow/pure start > body > overblown bright), an attack BLOOM (glide A>B, park), a slow FREE
// WAVE LFO breathing around the body, sustaining band-passed air, drift + vibrato.
static void windMotion(DC::Slot& s, float bloomSec, float lfoAmt, float lfoHz)
{
    s.addSeg[0] = bloomSec; s.addSeg[1] = 0.0f; s.addSeg[2] = 0.0f;   // bloom A>B, park on the body
    s.lfoAmt[0] = lfoAmt; s.lfoRate[0] = lfoHz; s.lfoFree[0] = true; lfoRoute(s, 0, DC::MTWavePos);  // LFO 1 -> WAVE scan (breath)
}
static DC::Slot& windBreath(DC& c, float w, float centerHz, float atk, float dec, float sus, float rel)
{   // the BREATH layer = SLOT 2 (mkSlot would CLEAR the channel - the round-11 wind killer)
    auto& n = mkSlot2(c, DC::SrcNoise, 1.0f - w);
    n.noiseType = 0; n.noiseCenter = centerHz; n.noiseRes = 0.55f;
    n.atk = atk; n.dec = dec; n.sustain = sus; n.release = rel;
    return n;
}
static void aClarinet(DC& c) {     // woody odd pipe: hollow start blooming into the reedy body
    auto& s = mkAdd(c, {{1,1.0f},{3,0.35f},{5,0.12f}});                              // A: hollow start
    wtFrame(s, 1, {{1,1.0f},{3,0.75f},{5,0.5f},{7,0.2f},{9,0.35f},{11,0.12f}});      // B: the reedy body
    wtFrame(s, 2, {{1,0.9f},{2,0.08f},{3,0.85f},{5,0.65f},{7,0.35f},{9,0.45f},{11,0.2f}}); // C: overblown bright
    wtFrame(s, 3, {{1,0.9f},{2,0.08f},{3,0.85f},{5,0.65f},{7,0.35f},{9,0.45f},{11,0.2f}});
    s.atk = 0.07f; s.dec = 0.5f; s.sustain = 0.85f; s.release = 0.22f;
    s.filterType = DC::LowPass; s.filterCutoff = 2400.0f; s.filterReso = 0.8f; ktRoute(s, 0.5f);
    s.filterType2 = DC::Bell; s.filterCutoff2 = 250.0f; s.filterGain2 = 2.0f; s.filterReso2 = 1.1f;   // warm low-mid lift (was Tone -0.25)
    s.drift = 0.2f; s.vibrato = 0.10f;
    windMotion(s, 0.09f, 0.14f, 0.6f);
    s.addLoop = true;                     // user: loop the glide (the tone breathes out AND back)
    windBreath(c, 0.12f, 2600.0f, 0.06f, 0.5f, 0.6f, 0.25f);
    c.volume = 0.74f;
}
static void aFlute(DC& c) {        // soft pure start blooming into a singing body; the tone breathes
    auto& s = mkAdd(c, {{1,1.0f}});                                                  // A: pure start
    wtFrame(s, 1, {{1,1.0f},{2,0.3f},{3,0.08f}});                                    // B: singing body
    wtFrame(s, 2, {{1,1.0f},{2,0.5f},{3,0.16f},{4,0.07f}});                          // C: blown brighter
    wtFrame(s, 3, {{1,1.0f},{2,0.5f},{3,0.16f},{4,0.07f}});
    s.atk = 0.09f; s.dec = 0.6f; s.sustain = 0.9f; s.release = 0.25f;
    s.drift = 0.3f; s.vibrato = 0.16f;
    s.filterType = DC::Bell; s.filterCutoff = 3200.0f; s.filterGain = 1.5f; s.filterReso = 1.1f;   // airy lift (was Tone 0.1)
    windMotion(s, 0.11f, 0.16f, 0.7f);
    s.addLoop = true;                     // user: loop the glide (the tone breathes out AND back)
    windBreath(c, 0.18f, 3400.0f, 0.07f, 0.6f, 0.75f, 0.28f);
    c.volume = 0.72f;
}
static void aOboe(DC& c) {         // "Reed Piano" (user: it reads as a piano, not an oboe - renamed):
                                   // warm start snapping into a nasal reedy body; formant BP + LP pair
    auto& s = mkAdd(c, {{1,0.7f},{2,0.6f},{3,0.5f},{4,0.25f}});                      // A: warm pre-bite
    wtFrame(s, 1, {{1,0.55f},{2,0.85f},{3,1.0f},{4,0.7f},{5,0.45f},{6,0.3f},{8,0.18f}});   // B: the reed
    wtFrame(s, 2, {{1,0.45f},{2,0.8f},{3,1.0f},{4,0.85f},{5,0.6f},{6,0.45f},{8,0.3f},{10,0.15f}}); // C: pressed
    wtFrame(s, 3, {{1,0.45f},{2,0.8f},{3,1.0f},{4,0.85f},{5,0.6f},{6,0.45f},{8,0.3f},{10,0.15f}});
    s.atk = 0.05f; s.dec = 0.5f; s.sustain = 0.85f; s.release = 0.2f;
    s.filterType = DC::BandPass; s.filterCutoff = 1100.0f; s.filterReso = 1.3f; ktRoute(s, 0.35f);
    s.filterType2 = DC::LowPass; s.filterCutoff2 = 5200.0f; s.filterReso2 = 0.7f;
    s.drift = 0.22f; s.vibrato = 0.12f;
    windMotion(s, 0.07f, 0.12f, 0.8f);
    windBreath(c, 0.1f, 3000.0f, 0.05f, 0.5f, 0.6f, 0.22f);
    c.volume = 0.7f;
}
static void aMusicBox(DC& c) {     // tiny sparkling box: sparse high partials, quick ping
    auto& s = mkAdd(c, {{1,1.0f},{4,0.55f},{9,0.3f},{16,0.12f}});
    s.atk = 0.002f; s.dec = 1.6f; s.sustain = 0.0f; s.release = 0.8f;
    s.fxPunch = 0.2f; c.reverbSend = 0.3f; c.volume = 0.7f;
}
static void aHollowBass(DC& c) {   // fundamental + a lonely 3rd = hollow, woody bass
    auto& s = mkAdd(c, {{1,1.0f},{3,0.45f},{5,0.12f}});
    s.atk = 0.004f; s.dec = 0.7f; s.sustain = 0.7f; s.release = 0.12f;
    s.oscFreq = 65.41f;
    s.filterType = DC::LowPass; s.filterCutoff = 900.0f; s.filterReso = 1.2f; ktRoute(s, 0.6f);
    chFx(c, DC::ChFxComp, 0.4f); c.volume = 0.82f;
}
static void aQuintLead(DC& c) {    // strong 3rd harmonic = a built-in fifth colour (organ-quint trick)
    auto& s = mkAdd(c, {{1,1.0f},{2,0.6f},{3,0.85f},{4,0.25f},{6,0.35f}});
    s.atk = 0.006f; s.dec = 0.5f; s.sustain = 0.75f; s.release = 0.2f;
    c.delaySend = 0.18f; c.volume = 0.7f;
}
static void aCombPad(DC& c) {      // every 3rd harmonic only = a comb-like, airy pad spectrum
    auto& s = mkAdd(c, {{1,1.0f},{4,0.7f},{7,0.5f},{10,0.35f},{13,0.25f},{16,0.18f}});
    s.atk = 0.25f; s.dec = 1.6f; s.sustain = 0.85f; s.release = 1.0f;
    chFx(c, DC::ChFxChorus, 0.6f); c.reverbSend = 0.3f; c.volume = 0.6f;
}
static void aNasalPluck(DC& c) {   // formant bump at h5-7, weak fundamental = kalimba-meets-banjo honk
    auto& s = mkAdd(c, {{1,0.5f},{2,0.2f},{5,1.0f},{6,0.8f},{7,0.6f}});
    s.atk = 0.002f; s.dec = 0.35f; s.sustain = 0.0f; s.release = 0.25f;
    s.fxPunch = 0.3f; c.delaySend = 0.15f; c.volume = 0.76f;
}
static void aGlassHarp(DC& c) {    // octave-dominant, slow-bowed glass rim (h2 > h1)
    auto& s = mkAdd(c, {{1,0.7f},{2,1.0f},{5,0.5f},{7,0.25f},{12,0.1f}});
    s.atk = 0.05f; s.dec = 1.4f; s.sustain = 0.5f; s.release = 1.2f;
    chFx(c, DC::ChFxChorus, 0.3f); c.reverbSend = 0.35f; c.volume = 0.62f;
}
static void aDulcimer(DC& c) {     // full 1/h series with every 4th harmonic REMOVED = hammered zither
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvCustom;
    for (int h = 1; h <= 16; ++h) s.addH[0][h - 1] = (h % 4 == 0) ? 0.0f : 1.0f / (float) h;
    for (int f = 1; f < DC::ADD_FRAMES; ++f)         // uniform strip (static sound, like mkAdd)
        for (int k = 0; k < DC::ADD_HARM; ++k) s.addH[f][k] = s.addH[0][k];
    s.atk = 0.002f; s.dec = 0.8f; s.sustain = 0.0f; s.release = 0.4f;
    s.filterType = DC::Bell; s.filterCutoff = 3500.0f; s.filterGain = 2.0f; s.filterReso = 1.1f;   // hammered sparkle (was Tone 0.2)
    c.delaySend = 0.1f; c.volume = 0.75f;
}
static void aOctaveSub(DC& c) {    // sine + half-strength octave = a sub that reads on small speakers
    auto& s = mkAdd(c, {{1,1.0f},{2,0.5f}});
    s.atk = 0.004f; s.dec = 0.8f; s.sustain = 0.9f; s.release = 0.15f;
    s.oscFreq = 55.0f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.35f; chFx(c, DC::ChFxComp, 0.3f); c.volume = 0.84f;
}
static void aBellPad(DC& c) {      // sparse inharmonic-feel partials swelling slowly = bells-as-a-pad
    auto& s = mkAdd(c, {{1,0.8f},{3,0.5f},{8,0.4f},{13,0.25f}});
    s.atk = 0.35f; s.dec = 1.8f; s.sustain = 0.8f; s.release = 1.3f;
    chFx(c, DC::ChFxChorus, 0.5f); c.reverbSend = 0.4f; c.volume = 0.58f;
}
// ---- Tone / Punch / Comp showcases ----
static void xSnapKick(DC& c) {     // the transient IS the click: Punch shapes a clean sine knock
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 52.0f;
    s.pEnvP[0] = 26.0f; s.pEnvT[0] = 0.012f; s.pEnvP[1] = 8.0f; s.pEnvT[1] = 0.05f;   // fast knock
    s.atk = 0.001f; s.dec = 0.42f; s.sustain = 0.0f; s.release = 0.05f;
    s.fxPunch = 0.7f; chFx(c, DC::ChFxComp, 0.4f); c.volume = 0.95f;
}
static void xSquashClap(DC& c) {   // compressor slammed on a clap = long sizzling sustain tail
    auto& s = mkSlot(c, DC::SrcNoise);
    s.noiseType = 0; s.atk = 0.002f; s.hold = 0.02f; s.dec = 0.35f; s.sustain = 0.0f;
    s.filterType = DC::BandPass; s.filterCutoff = 1400.0f; s.filterReso = 1.4f;
    s.filterType2 = DC::Bell; s.filterCutoff2 = 4000.0f; s.filterGain2 = 2.5f; s.filterReso2 = 1.1f;   // sizzle lift (was Tone 0.3)
    chFx(c, DC::ChFxComp, 0.8f); c.volume = 0.8f;
}
static void xFeltPiano(DC& c) {    // softened attack + dark tilt = the felt-piano intimacy
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 10; s.oscFreq = 261.63f;              // Bell hammer tone
    s.fmDepth = 0.1f; s.fmPitch = 0.5f; s.fmEnvFollow = true;
    s.atk = 0.004f; s.dec = 1.6f; s.sustain = 0.25f; s.release = 0.4f;
    s.fxPunch = -0.6f; chFx(c, DC::ChFxComp, 0.2f);
    s.filterType  = DC::LowPass; s.filterCutoff  = 4500.0f; s.filterReso  = 0.7f;   // highs down +
    s.filterType2 = DC::Bell;    s.filterCutoff2 = 220.0f;  s.filterGain2 = 3.5f; s.filterReso2 = 1.1f;   // lows up = the felt tilt (was Tone -0.5)
    c.reverbSend = 0.15f; c.volume = 0.78f;
}
static void xTightSnare(DC& c) {   // punch + comp = a dry, in-your-face snare crack
    auto& s = mkSlot(c, DC::SrcNoise);
    s.noiseType = 0; s.atk = 0.001f; s.dec = 0.16f; s.sustain = 0.0f;
    s.filterType = DC::HighPass; s.filterCutoff = 250.0f; s.filterReso = 0.8f;
    s.fxPunch = 0.5f; chFx(c, DC::ChFxComp, 0.5f);
    auto& b = mkSlot2(c, DC::SrcOsc, 0.62f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 185.0f;
    b.pEnvP[0] = 6.0f; b.pEnvT[0] = 0.03f;
    b.atk = 0.001f; b.dec = 0.12f; b.sustain = 0.0f;
    c.volume = 0.9f;
}
static void xGlueBass(DC& c) {     // heavy one-knob comp holding a saw+sub together
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.atk = 0.004f; s.dec = 0.6f; s.sustain = 0.75f; s.release = 0.12f;
    s.filterType = DC::LowPass; s.filterCutoff = 800.0f; s.filterReso = 1.0f; ktRoute(s, 0.5f);
    chFx(c, DC::ChFxComp, 0.7f);
    s.filterType2 = DC::Bell; s.filterCutoff2 = 120.0f; s.filterGain2 = 2.0f; s.filterReso2 = 1.1f;   // low warmth (was Tone -0.2)
    auto& b = mkSlot2(c, DC::SrcOsc, 0.6f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 55.0f;
    b.atk = 0.004f; b.dec = 0.6f; b.sustain = 0.8f; b.release = 0.12f;
    c.volume = 0.8f;
}
static void xCrispHat(DC& c) {     // bright tilt + snap on purple noise = pristine tight hat
    auto& s = mkSlot(c, DC::SrcNoise);
    s.noiseType = 3; s.atk = 0.001f; s.dec = 0.06f; s.sustain = 0.0f;
    s.filterType = DC::HighPass; s.filterCutoff = 5000.0f; s.filterReso = 0.8f;
    s.filterType2 = DC::Bell; s.filterCutoff2 = 8000.0f; s.filterGain2 = 4.5f; s.filterReso2 = 1.1f;   // pristine top lift (was Tone 0.6)
    s.fxPunch = 0.4f; c.volume = 0.72f;
}


// ---- SPECTRUM MOTION showcases (A -> B morph over the note; the additive-native movement) ----
static void wtFrame(DC::Slot& s, int f, std::initializer_list<std::pair<int,float>> hs)
{   // author ONE wavetable frame (0..3) from a harmonic list
    for (int k = 0; k < DC::ADD_HARM; ++k) { s.addH[f][k] = 0.0f; s.addPh[f][k] = 0.0f; }
    for (auto& h : hs) if (h.first >= 1 && h.first <= DC::ADD_HARM) s.addH[f][h.first - 1] = h.second;
}
static void setB(DC::Slot& s, std::initializer_list<std::pair<int,float>> hs, float morphSec)
{   // the ORIGINAL 2-spectrum morph: frames B..D = the target, travel the FIRST leg in the
    // authored time, then HOLD = the per-segment model's {morphSec, 0, 0} - audibly identical.
    for (int f = 1; f < DC::ADD_FRAMES; ++f) wtFrame(s, f, hs);
    s.addSeg[0] = morphSec; s.addSeg[1] = 0.0f; s.addSeg[2] = 0.0f;
}
static void sMorphPad(DC& c) {     // dark swell that blossoms into an airy comb over ~2.5 s
    auto& s = mkAdd(c, {{1,1.0f},{2,0.5f}});
    setB(s, {{1,0.6f},{4,0.8f},{7,0.6f},{10,0.45f},{13,0.3f}}, 2.5f);
    s.atk = 0.3f; s.dec = 1.8f; s.sustain = 0.85f; s.release = 1.0f;
    chFx(c, DC::ChFxChorus, 0.5f); c.reverbSend = 0.35f; c.volume = 0.6f;
}
static void sBloomLead(DC& c) {    // starts a pure sine, BLOOMS into a full saw-bright spectrum
    auto& s = mkAdd(c, {{1,1.0f}});
    setB(s, {{1,1.0f},{2,0.5f},{3,0.33f},{4,0.25f},{5,0.2f},{6,0.17f},{7,0.14f},{8,0.12f},{10,0.1f},{12,0.08f}}, 1.2f);
    s.atk = 0.005f; s.dec = 0.6f; s.sustain = 0.8f; s.release = 0.2f;
    c.delaySend = 0.18f; c.volume = 0.7f;
}
static void sMorphKeys(DC& c) {    // bright hammer spectrum melting into a soft body (~0.6 s) = living keys
    auto& s = mkAdd(c, {{1,1.0f},{2,0.6f},{3,0.8f},{5,0.5f},{7,0.3f}});
    setB(s, {{1,1.0f},{3,0.25f}}, 0.6f);
    s.atk = 0.003f; s.dec = 1.5f; s.sustain = 0.3f; s.release = 0.35f;
    chFx(c, DC::ChFxComp, 0.25f); c.reverbSend = 0.12f; c.volume = 0.72f;
}
static void sAuroraBells(DC& c) {  // one sparse bell colour drifting into ANOTHER (inharmonic-feel shimmer)
    auto& s = mkAdd(c, {{1,0.8f},{5,0.6f},{12,0.35f}});
    setB(s, {{2,1.0f},{8,0.5f},{13,0.3f}}, 1.6f);
    s.atk = 0.004f; s.dec = 2.0f; s.sustain = 0.3f; s.release = 1.2f;
    c.reverbSend = 0.4f; c.volume = 0.62f;
}
static void sEvolver(DC& c) {      // slow drone: hollow odd start evolving into a full bright series (~3.5 s)
    auto& s = mkAdd(c, {{1,1.0f},{3,0.6f},{5,0.4f},{7,0.25f}});
    setB(s, {{1,1.0f},{2,0.7f},{3,0.55f},{4,0.45f},{5,0.38f},{6,0.32f},{8,0.25f},{10,0.2f},{12,0.16f},{16,0.12f}}, 3.5f);
    s.atk = 0.5f; s.dec = 2.0f; s.sustain = 0.9f; s.release = 1.5f;
    chFx(c, DC::ChFxChorus, 0.6f); c.reverbSend = 0.4f; c.volume = 0.56f;
}

// ---- 4-FRAME WAVETABLE showcases (w*): four DISTINCT frames + per-leg glide times (0 = hold),
//      the static Position, and the WAVE LFO scanning - the full v1.3.5 wavetable feature set. ----
static void wOdysseyPad(DC& c) {   // the full A>D journey: dark -> hollow -> organ -> bright, ~5.5 s
    auto& s = mkAdd(c, {{1,1.0f},{2,0.35f}});                               // A: dark warmth
    wtFrame(s, 1, {{1,1.0f},{3,0.6f},{5,0.4f},{7,0.25f}});                  // B: hollow odd
    wtFrame(s, 2, {{1,1.0f},{2,0.7f},{4,0.5f},{8,0.35f},{16,0.15f}});       // C: organ octaves
    wtFrame(s, 3, {{1,1.0f},{2,0.6f},{3,0.45f},{4,0.35f},{5,0.3f},{6,0.25f},{8,0.2f},{10,0.15f},{13,0.1f}}); // D: full bloom
    s.addSeg[0] = 1.2f; s.addSeg[1] = 1.8f; s.addSeg[2] = 2.5f;
    s.atk = 0.4f; s.dec = 2.0f; s.sustain = 0.9f; s.release = 1.4f;
    chFx(c, DC::ChFxChorus, 0.45f); c.reverbSend = 0.35f; c.volume = 0.58f;
}
static void wTalkingPad(DC& c) {   // vowel frames + a slow synced WAVE LFO scanning them = it TALKS
    auto& s = mkAdd(c, {{2,0.7f},{3,1.0f},{4,0.8f},{7,0.3f}});              // A: "ah" (low formant)
    wtFrame(s, 1, {{1,0.6f},{2,1.0f},{3,0.5f},{5,0.7f},{6,0.4f}});          // B: "oh" (round)
    wtFrame(s, 2, {{1,0.5f},{6,0.9f},{7,1.0f},{8,0.6f},{12,0.25f}});        // C: "ee" (high formant)
    wtFrame(s, 3, {{2,0.8f},{4,1.0f},{5,0.7f},{9,0.5f},{11,0.3f}});         // D: nasal "ay"
    s.addPos = 0.5f;                                                        // park mid-strip...
    s.lfoAmt[0] = 0.65f; s.lfoSync[0] = 0.5f; lfoRoute(s, 0, DC::MTWavePos);             // LFO 1 -> WAVE sweeps the mouth (2-bar cycle)
    s.atk = 0.15f; s.dec = 1.5f; s.sustain = 0.85f; s.release = 0.8f;
    c.reverbSend = 0.3f; c.volume = 0.6f;
}
static void wCascadeKeys(DC& c) {  // 3-STAGE piano-ish hit: bright strike > warm bloom > hollow tail (holds C)
    auto& s = mkAdd(c, {{1,1.0f},{2,0.7f},{3,0.9f},{5,0.6f},{8,0.45f},{12,0.3f}}); // A: strike
    wtFrame(s, 1, {{1,1.0f},{2,0.5f},{3,0.3f},{5,0.15f}});                  // B: warm body
    wtFrame(s, 2, {{1,1.0f},{3,0.2f},{7,0.08f}});                           // C: hollow tail
    wtFrame(s, 3, {{1,1.0f},{3,0.2f},{7,0.08f}});                           // D = C (journey ends at C)
    s.addSeg[0] = 0.06f; s.addSeg[1] = 0.5f; s.addSeg[2] = 0.0f;            // fast strike leg, slow bloom, HOLD
    s.atk = 0.002f; s.dec = 1.8f; s.sustain = 0.25f; s.release = 0.4f;
    chFx(c, DC::ChFxComp, 0.2f); c.reverbSend = 0.14f; c.volume = 0.7f;
}
static void wScanBass(DC& c) {     // the WAVE LFO scans sub > nasal > hollow per bar = spectral wobble bass
    auto& s = mkAdd(c, {{1,1.0f},{2,0.4f}});                                // A: sub weight
    wtFrame(s, 1, {{1,0.8f},{3,1.0f},{4,0.6f},{6,0.3f}});                   // B: nasal push
    wtFrame(s, 2, {{1,1.0f},{5,0.8f},{7,0.5f}});                            // C: hollow growl
    wtFrame(s, 3, {{1,0.9f},{2,0.6f},{3,0.5f},{4,0.45f},{6,0.3f}});         // D: gritty top
    s.oscFreq = 55.0f;                                                      // A1 bass register
    s.addPos = 0.35f;
    s.lfoAmt[0] = 0.6f; s.lfoSync[0] = 1.0f; lfoRoute(s, 0, DC::MTWavePos);               // LFO 1 -> WAVE, one scan per bar, on the groove
    s.atk = 0.003f; s.dec = 0.8f; s.sustain = 0.7f; s.release = 0.12f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.35f; c.volume = 0.72f;
}
static void wDriftLead(DC& c) {    // parked mid-strip, a SLOW free-run WAVE drift = a lead that never sits still
    auto& s = mkAdd(c, {{1,1.0f},{2,0.5f},{3,0.35f},{4,0.2f}});             // A: soft saw-ish
    wtFrame(s, 1, {{1,1.0f},{3,0.6f},{5,0.35f},{9,0.15f}});                 // B: square-ish
    wtFrame(s, 2, {{1,1.0f},{2,0.25f},{5,0.5f},{8,0.3f}});                  // C: glassy
    wtFrame(s, 3, {{1,0.9f},{2,0.7f},{4,0.5f},{7,0.35f},{11,0.2f}});        // D: brassy
    s.addPos = 0.5f;
    s.lfoAmt[0] = 0.18f; s.lfoRate[0] = 0.3f; lfoRoute(s, 0, DC::MTWavePos);              // LFO 1 -> WAVE, small slow drift (free Hz)
    s.atk = 0.01f; s.dec = 0.7f; s.sustain = 0.8f; s.release = 0.18f;
    c.delaySend = 0.2f; c.volume = 0.68f;
}
static void wSwellOrgan(DC& c) {   // drawbars pulled WHILE you hold: octaves > +quint > full mixture (holds C)
    auto& s = mkAdd(c, {{1,1.0f},{2,0.6f},{4,0.3f}});                       // A: flutes 8'+4'
    wtFrame(s, 1, {{1,1.0f},{2,0.7f},{3,0.55f},{4,0.4f},{6,0.3f}});         // B: + the quint rank
    wtFrame(s, 2, {{1,1.0f},{2,0.75f},{3,0.6f},{4,0.55f},{6,0.45f},{8,0.4f},{12,0.3f},{16,0.2f}}); // C: full mixture
    wtFrame(s, 3, {{1,1.0f},{2,0.75f},{3,0.6f},{4,0.55f},{6,0.45f},{8,0.4f},{12,0.3f},{16,0.2f}}); // D = C
    s.addSeg[0] = 0.8f; s.addSeg[1] = 2.0f; s.addSeg[2] = 0.0f;             // swell in two stages, then HOLD
    s.atk = 0.05f; s.dec = 1.2f; s.sustain = 1.0f; s.release = 0.25f;
    chFx(c, DC::ChFxChorus, 0.35f); c.volume = 0.62f;
}

//==============================================================================
// USER IMPORTS (2026-07-11): the user's own patches, translated 1:1 from his
// .basamaksound files (all pure synth - see docs/HISTORY.md for the batch).
//==============================================================================
static void uSteelKick(DC& c)   // "kick1": a Modal metal hit through a 35 Hz high-pass = steely knock
{
    auto& s = mkSlot(c, DC::SrcModal);
    s.weight = 0.5f;
    s.oscFreq = 69.19f;                     // modal base (~C#2)
    s.modalMaterial = 4; s.modalDecay = 0.0713f; s.modalTone = 0.554f;
    s.modalStruct = 0.2905f; s.modalMorph = 0.2162f;
    s.atk = 0.0024f; s.dec = 0.2f; s.release = 0.005f;
    s.filterType = DC::HighPass; s.filterCutoff = 35.0f; s.filterReso = 0.707f;
}
static void uHungryBass(DC& c)  // USER IMPORT "my growl": KS Steel bass, resonant 370 Hz LP sweeping
{                               // DOWN into the BASS AMP split rig = a feral, chewing growl
    auto& s = mkSlot(c, DC::SrcPhys);
    s.physFreq = 65.41f; s.physTone = 0.55f; s.physMaterial = 1.0f;   // Steel
    s.physPosition = 0.22f; s.physStiff = 0.03f;
    s.atk = 0.001f; s.dec = 1.07f; s.sustain = 0.19f; s.release = 0.5f;
    s.oscDetune = 0.05f;
    s.filterType = DC::LowPass; s.filterCutoff = 370.0f; s.filterReso = 3.57f; s.filterEnvAmt = -0.35f;
    s.fxDriveType = DC::DriveBassAmp; s.fxDrive = 0.6f;
    c.strumAmt = 1.0f;
    c.volume = 0.82f;
}
static void nProwlBass(DC& c) {  // filter-forward bass (Hungry Bass's cousin, ORIGINAL recipe):
                                 // detuned saws + FM sub into a resonant LP that BREATHES on a slow
                                 // FREE-RUN filter LFO (timeline-anchored - notes join it mid-sweep,
                                 // it never restarts per key: the user's bass-LFO complaint answered)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;   // A1
    s.oscUnison = 2; s.oscDetune = 0.06f; s.fmSub = 0.45f;     // body + clean sub under it
    s.atk = 0.004f; s.dec = 0.9f; s.sustain = 0.65f; s.release = 0.25f;
    s.drift = 0.15f;
    s.filterType = DC::LowPass; s.filterCutoff = 480.0f; s.filterReso = 2.6f;
    s.filterEnvAmt = 0.45f; ktRoute(s, 0.35f);
    s.lfoAmt[0] = 0.3f; s.lfoSync[0] = 0.5f; s.lfoFree[0] = true; lfoRoute(s, 0, DC::MTFilt1Cut);   // half a cycle per bar, free-run
    s.fxDriveType = DC::DriveBassAmp; s.fxDrive = 0.4f;
    c.volume = 0.85f;
}
// ---- GRANULAR bank (v1.3.9, user order: "at least 10"). All wave-sourced = self-contained;
// load a sample onto the slot (drag-drop keeps the Granular engine) to granulate audio instead.
static DC::Slot& grSlot(DC& c, int shape) { auto& s = mkSlot(c, DC::SrcGrain); s.oscShape = s.oscShapeB = shape; return s; }
static void grGrainPad(DC& c) {      // custom dark->bright journey; Position = the timbre scan
    auto& s = grSlot(c, 14);         // Custom wave
    wtFrame(s, 0, {{1,1.0f}});  wtFrame(s, 1, {{1,1.0f},{2,0.4f},{3,0.25f}});
    wtFrame(s, 2, {{1,0.9f},{3,0.6f},{5,0.4f},{7,0.25f}});
    wtFrame(s, 3, {{1,0.8f},{2,0.6f},{3,0.55f},{4,0.4f},{5,0.35f},{6,0.3f},{7,0.25f},{8,0.2f}});
    s.grainPos = 0.3f; s.grainSize = 0.7f; s.grainDens = 0.8f; s.grainSpray = 0.25f;
    s.atk = 0.4f; s.dec = 1.5f; s.sustain = 0.8f; s.release = 0.7f;
    c.reverbSend = 0.3f; c.volume = 0.8f;
}
static void grCloudChoir(DC& c) {    // vowel grains, dense + wide = a breathing choir cloud
    auto& s = grSlot(c, 6);          // Vowel A
    s.grainPos = 0.5f; s.grainSize = 0.35f; s.grainDens = 0.85f; s.grainSpray = 0.4f; s.grainPitch = 0.06f;
    s.atk = 0.3f; s.dec = 1.2f; s.sustain = 0.85f; s.release = 0.6f;
    chFx(c, DC::ChFxChorus, 0.35f); c.reverbSend = 0.35f; c.volume = 0.78f;
}
static void grShimmerDrone(DC& c) {  // octave glitter over a slow bright journey
    auto& s = grSlot(c, 14);
    wtFrame(s, 0, {{1,1.0f},{2,0.3f}}); wtFrame(s, 1, {{1,0.8f},{2,0.5f},{4,0.4f}});
    wtFrame(s, 2, {{1,0.7f},{3,0.5f},{6,0.4f},{8,0.3f}}); wtFrame(s, 3, {{1,0.6f},{4,0.5f},{8,0.45f},{12,0.35f}});
    s.grainPos = 0.45f; s.grainSize = 0.8f; s.grainDens = 0.75f; s.grainSpray = 0.3f; s.grainPitch = 0.3f;
    s.atk = 0.9f; s.dec = 2.0f; s.sustain = 0.9f; s.release = 1.0f;
    c.reverbSend = 0.4f; c.volume = 0.7f;
}
static void grGrainKeys(DC& c) {     // bell-grain keys: struck, glassy, a little dusty
    auto& s = grSlot(c, 10);         // Bell shape
    s.grainPos = 0.4f; s.grainSize = 0.4f; s.grainDens = 0.7f; s.grainSpray = 0.15f; s.grainPitch = 0.04f;
    s.atk = 0.003f; s.dec = 1.2f; s.sustain = 0.4f; s.release = 0.35f;
    c.delaySend = 0.18f; c.volume = 0.8f;
}
static void grScatterLead(DC& c) {   // gritty scattered saw lead
    auto& s = grSlot(c, 4);          // Saw
    s.grainPos = 0.5f; s.grainSize = 0.25f; s.grainDens = 0.7f; s.grainSpray = 0.1f; s.grainPitch = 0.08f;
    s.atk = 0.004f; s.dec = 0.8f; s.sustain = 0.7f; s.release = 0.2f;
    s.filterType = DC::LowPass; s.filterCutoff = 2600.0f; s.filterReso = 1.2f; ktRoute(s, 0.5f);
    c.volume = 0.8f;
}
static void grParticlePerc(DC& c) {  // sparse micro-grains = pitched particle percussion
    auto& s = grSlot(c, 5);          // Pulse
    s.grainPos = 0.5f; s.grainSize = 0.05f; s.grainDens = 0.45f; s.grainSpray = 0.6f; s.grainPitch = 0.5f;
    s.atk = 0.001f; s.dec = 0.25f; s.sustain = 0.0f; s.release = 0.05f;
    c.delaySend = 0.15f; c.volume = 0.85f;
}
static void grGraniteBass(DC& c) {   // thick granulated saw bass - dense grains read as texture
    auto& s = grSlot(c, 4);
    s.oscFreq = 55.0f;
    s.grainPos = 0.5f; s.grainSize = 0.5f; s.grainDens = 0.9f; s.grainSpray = 0.05f; s.grainPitch = 0.03f;
    s.atk = 0.004f; s.dec = 0.9f; s.sustain = 0.6f; s.release = 0.2f;
    s.filterType = DC::LowPass; s.filterCutoff = 520.0f; s.filterReso = 1.6f;
    s.fxDriveType = DC::DriveBassAmp; s.fxDrive = 0.3f;
    c.volume = 0.85f;
}
static void grGrainRiser(DC& c) {    // pitch-sprayed cloud swelling in = an organic riser
    auto& s = grSlot(c, 9);          // Organ
    s.grainPos = 0.4f; s.grainSize = 0.55f; s.grainDens = 0.8f; s.grainSpray = 0.5f; s.grainPitch = 0.4f;
    s.atk = 2.2f; s.hold = 0.4f; s.dec = 0.8f; s.sustain = 0.9f; s.release = 0.5f;
    c.reverbSend = 0.35f; c.volume = 0.75f;
}
static void grFrozenOrgan(DC& c) {   // big smooth grains of an organ mixture - sustained, hazy
    auto& s = grSlot(c, 9);
    s.grainPos = 0.5f; s.grainSize = 0.9f; s.grainDens = 0.85f; s.grainSpray = 0.2f;
    s.atk = 0.08f; s.dec = 1.4f; s.sustain = 0.9f; s.release = 0.5f;
    chFx(c, DC::ChFxChorus, 0.25f); c.volume = 0.78f;
}
static void grDustMotes(DC& c) {     // barely-there micro-particles drifting in a delay
    auto& s = grSlot(c, 0);          // Sine
    s.grainPos = 0.5f; s.grainSize = 0.03f; s.grainDens = 0.3f; s.grainSpray = 0.8f; s.grainPitch = 0.7f;
    s.atk = 0.02f; s.dec = 1.0f; s.sustain = 0.6f; s.release = 0.4f;
    c.delaySend = 0.3f; c.reverbSend = 0.2f; c.volume = 0.62f;
}
static void uEvilLaugh(DC& c)   // "evil laugh": pulsing resonant pink noise + a falling filter sweep
{
    auto& s = mkSlot(c, DC::SrcNoise);
    s.weight = 0.5f;
    s.noiseType = 1; s.noiseCenter = 102.6f; s.noiseWidth = 0.197f; s.noiseDrive = 0.338f;
    s.atk = 0.001f; s.dec = 1.02f; s.release = 0.06f;
    s.filterType = DC::LowPass; s.filterCutoff = 1186.0f; s.filterReso = 4.73f; s.filterEnvAmt = -1.0f;
    s.lfoRate[2] = 5.4f; s.lfoAmt[2] = 0.457f; lfoRoute(s, 2, DC::MTVol);   // the ha-ha-ha pulse (VOL LFO; halved - matrix MTVol is bipolar around the 0.5 weight, 0.914 drove it to silence)
}

static const struct { const char* name; Builder build; const char* cat; } kMixes[] = {
    // Categories are by INSTRUMENT (never by engine). Grouped + ordered like catOrder[] in
    // rebuildSoundMixMenu() - a category missing there is silently dropped from the menu.
    // ---- Kicks ----
    { "FM Kick", mFMKick, "Kicks" },
    { "808 Kick", m808Kick, "Kicks" },
    { "909 Kick", m909Kick, "Kicks" },
    { "Punch Kick", mPunchKick, "Kicks" },
    { "Dist Kick", mDistKick, "Kicks" },
    { "Sub Kick", mSubKick, "Kicks" },
    { "Steel Kick", uSteelKick, "Kicks" },        // USER IMPORT (kick1)
    { "Break Kick", mBreakKick, "Kicks" },
    { "Rumble Kick", mRumbleKick, "Kicks" },
    { "Snap Kick", xSnapKick, "Kicks" },
    // ---- Snares ----
    { "Noise Snare", mNoiseSnare, "Snares" },
    { "606 Snare", m606Snare, "Snares" },
    { "Trap Snare", mTrapSnare, "Snares" },
    { "Mod Snare", mModSnare, "Snares" },
    { "Brush Snare", mBrushSnare, "Snares" },
    { "Snap Snare", mSnapSnare, "Snares" },
    { "Room Snare", mRoomSnare, "Snares" },
    { "Clack Snare", mClackSnare, "Snares" },
    { "Tight Snare", xTightSnare, "Snares" },
    // ---- Claps ----
    { "Clap", mClap, "Claps" },
    { "808 Clap", m808Clap, "Claps" },
    { "Snap Clap", mSnapClap, "Claps" },
    { "Big Clap", mBigClap, "Claps" },
    { "Squash Clap", xSquashClap, "Claps" },
    // ---- Hi-Hats ----
    { "Closed Hat", mClosedHat, "Hi-Hats" },
    { "Open Hat", mOpenHat, "Hi-Hats" },
    { "808 CH", m808ClosedHat, "Hi-Hats" },
    { "808 OH", m808OpenHat, "Hi-Hats" },
    { "909 CH", m909ClosedHat, "Hi-Hats" },
    { "909 OH", m909OpenHat, "Hi-Hats" },
    { "606 CH", m606ClosedHat, "Hi-Hats" },
    { "Metal Hat", mMetalHat, "Hi-Hats" },
    { "Tight Hat", mTightHat, "Hi-Hats" },
    { "Foot Hat", mFootHat, "Hi-Hats" },
    { "Crisp Hat", xCrispHat, "Hi-Hats" },
    // ---- Cymbals ----
    { "Crash", mCrash, "Cymbals" },
    { "Bell Cymbal", m909Ride, "Cymbals" },      // was "909 Ride" (user: it sounds like a bell)
    { "909 Crash", m909Crash, "Cymbals" },
    { "Sizzle", mSizzle, "Cymbals" },
    { "Mod Gong", moGong, "Cymbals" },
    { "China", mChina, "Cymbals" },
    { "Bell Ride", mBellRide, "Cymbals" },
    // ---- Toms ----
    { "FM Tom", mFMTom, "Toms" },
    { "808 Low Tom", m808LowTom, "Toms" },
    { "808 Mid Tom", m808MidTom, "Toms" },
    { "808 Hi Tom", m808HiTom, "Toms" },
    { "909 Low Tom", m909LowTom, "Toms" },
    { "909 Mid Tom", m909MidTom, "Toms" },
    { "909 Hi Tom", m909HiTom, "Toms" },
    { "Roto Tom", mRotoTom, "Toms" },
    { "Mod Tom", moTomDrum, "Toms" },
    // ---- Percussion ----
    { "Rimshot", mRimshot, "Percussion" },
    { "Woodblock", mWoodblock, "Percussion" },
    { "Shaker", mShaker, "Percussion" },
    { "Wood Clave", mWoodClave, "Percussion" },
    { "808 Cowbell", m808Cowbell, "Percussion" },
    { "808 Rimshot", m808Rimshot, "Percussion" },
    { "808 Conga", m808Conga, "Percussion" },
    { "808 Clave", m808Clave, "Percussion" },
    { "909 Rimshot", m909Rim, "Percussion" },
    { "Tabla", mTabla, "Percussion" },
    { "Log Drum", mLogDrum, "Percussion" },
    { "Bongo", mBongo, "Percussion" },
    { "Mod Metal Plate", moMetalPlate, "Percussion" },
    { "Mod Wood Block", moWoodBlock, "Percussion" },
    { "Mod Cowbell", moCowbell, "Percussion" },
    // ---- Electro Perc ----
    { "Zap", mZap, "Electro Perc" },
    { "Particle Perc", grParticlePerc, "Electro Perc" },   // GRANULAR
    { "FM Clang", nFmClang, "Electro Perc" },        // AUDIO-RATE: fixed 780 Hz -> Pitch = metal clang
    { "Filter Zap", eFilterZap, "Electro Perc" },
    { "Blip", mBlip, "Electro Perc" },
    { "Laser Zap", mLaserZap, "Electro Perc" },
    { "Mouth Pop", mChirp, "Electro Perc" },     // was "Chirp" (user rename)
    { "Static Hit", mStaticHit, "Electro Perc" },
    { "Buzz Hit", mBuzzHit, "Electro Perc" },
    { "Glitch Tick", mGlitchTick, "Electro Perc" },
    // ---- Bass ----
    { "Sub Bass", mSubBass, "Bass" },
    { "808 Bass", m808Bass, "Bass" },
    { "Saw Bass", mSawBass, "Bass" },
    { "Reese Bass", mReeseBass, "Bass" },
    { "Square Bass", mSquareBass, "Bass" },
    { "FM Bass", mFMBass, "Bass" },
    { "Growl Bass", mGrowlBass, "Bass" },
    { "Acid Bass", mAcidBass, "Bass" },
    { "Filter Bass", eFilterBass, "Bass" },
    { "Station Bass", bStationBass, "Bass" },
    { "Ladder Bass", bLadderBass, "Bass" },
    { "Rubber Bass", bRubberBass, "Bass" },
    { "Neuro Bass", bNeuroBass, "Bass" },
    { "Hoover Bass", bHooverBass, "Bass" },
    { "Reed Bass", bReedBass, "Bass" },
    { "Wobble Bass", bWobbleBass, "Bass" },
    { "Finger Bass", gFingerBass, "Bass" },
    { "Pick Bass", gPickBass, "Bass" },
    { "Muted Bass", gMutedBass, "Bass" },
    { "Fuzz Bass", gFuzzBass, "Bass" },
    { "Amp Bass", gAmpBass, "Bass" },            // KS bass through the BASS AMP split rig
    { "Hungry Bass", uHungryBass, "Bass" },          // USER IMPORT (my growl)
    { "Talking Bass", nTalkingBass, "Bass" },        // FORMANT + SUB showcase (LFO -> Formant = it talks)
    { "FM Growl", nFmGrowl, "Bass" },                // AUDIO-RATE showcase: KEY LFO -> Pitch = playable FM growl
    { "Scooped Bass", nScoopedBass, "Bass" },        // BIPOLAR BELL showcase: mids CUT -9 dB
    { "Sub Pluck", nSubPluck, "Bass" },              // SUB showcase: clean octave-down under a bright pluck
    { "Granite Bass", grGraniteBass, "Bass" },   // GRANULAR
    { "Prowl Bass", nProwlBass, "Bass" },        // free-run filter LFO showcase
    { "Keys Bass", kKeysBass, "Bass" },
    { "Deep Sub", kSubBass, "Bass" },
    { "Notch Bass", nNotchBass, "Bass" },
    { "Hollow Bass", aHollowBass, "Bass" },
    { "Scan Bass", wScanBass, "Bass" },
    { "Octave Sub", aOctaveSub, "Bass" },
    { "Glue Bass", xGlueBass, "Bass" },
    // ---- Keys ----
    { "E-Piano", kEPiano, "Keys" },
    { "Grain Keys", grGrainKeys, "Keys" },   // GRANULAR
    { "String Keys", kStringKeys, "Keys" },
    { "Toy Piano", kToyPiano, "Keys" },
    { "Harpsichord", kHarpsichord, "Keys" },
    { "Pipe Organ", kPipeOrgan, "Keys" },
    { "Accordion", kAccordion, "Keys" },
    { "Honky Tonk", kHonkyTonk, "Keys" },
    { "Grand Piano", kGrandPiano, "Keys" },
    { "Wurli", kWurli, "Keys" },
    { "Clavinet", kClav, "Keys" },
    { "Chorus EP", nChorusEP, "Keys" },
    { "Phase Keys", nPhaseKeys, "Keys" },            // CHANNEL FX showcase: LFO -> Phaser (Channel)
    { "Metal EP", nMetalEP, "Keys" },                // AUDIO-RATE: inharmonic KEY x3.5 = DX metal
    { "Felt Piano", xFeltPiano, "Keys" },
    { "Morph Keys", sMorphKeys, "Keys" },
    { "Cascade Keys", wCascadeKeys, "Keys" },
    { "Swell Organ", wSwellOrgan, "Keys" },
    // ---- Pads & Choirs ----
    { "Soft Pad", kSoftPad, "Pads & Choirs" },
    { "Choir Aah", kChoir, "Pads & Choirs" },
    { "Warm Pad", kWarmPad, "Pads & Choirs" },
    { "Choir Ooh", kChoirOoh, "Pads & Choirs" },
    { "Dark Pad", kDarkPad, "Pads & Choirs" },
    { "Shimmer Pad", kShimmerPad, "Pads & Choirs" },
    { "Motion Pad", kMotionPad, "Pads & Choirs" },
    { "Brass Pad", kBrassPad, "Pads & Choirs" },
    { "Chorus Pad", nChorusPad, "Pads & Choirs" },
    { "Jet Pad", nJetPad, "Pads & Choirs" },          // CHANNEL FX showcase: LFO -> Flanger (Channel)
    { "Vowel Pad", nVowelPad, "Pads & Choirs" },      // FORMANT showcase: slow LFO -> vowel morph
    { "AM Shimmer", nAmShimmer, "Pads & Choirs" },    // AUDIO-RATE AM at the octave (KEY x2 -> Volume)
    { "Sideband Pad", nSidebandPad, "Pads & Choirs" },// AUDIO-RATE: fixed 150 Hz -> Pitch = inharmonic fog
    { "Vox Pad", nVoxBP, "Pads & Choirs" },
    { "Sync Sweep", nSyncSweep, "Pads & Choirs" },
    { "Comb Pad", aCombPad, "Pads & Choirs" },
    { "Morph Pad", sMorphPad, "Pads & Choirs" },
    { "Odyssey Pad", wOdysseyPad, "Pads & Choirs" },
    { "Grain Pad", grGrainPad, "Pads & Choirs" },        // GRANULAR bank (v1.3.9)
    { "Cloud Choir", grCloudChoir, "Pads & Choirs" },
    { "Frozen Organ", grFrozenOrgan, "Pads & Choirs" },
    { "Talking Pad", wTalkingPad, "Pads & Choirs" },
    { "Bell Pad", aBellPad, "Pads & Choirs" },
    // ---- Leads ----
    { "Vox", mVox, "Leads" },
    { "Scatter Lead", grScatterLead, "Leads" },   // GRANULAR
    { "Talkbox", mTalkbox, "Leads" },
    { "Whistle", mWhistle, "Leads" },
    { "Pulse Lead", kPulseLead, "Leads" },
    { "FM Lead", kFMLead, "Leads" },
    { "Square Lead", kSquareLead, "Leads" },
    { "Synth Brass", kSynthBrass, "Leads" },
    { "Saw Lead", kSawLead, "Leads" },
    { "Hyper Saw", nHyperSaw, "Leads" },
    { "Swirl Lead", nSwirlLead, "Leads" },           // CHANNEL FX showcase: Velocity -> Phaser (Channel)
    { "Wolf Lead", nWolfLead, "Leads" },             // AUDIO-RATE: KEY x2 FM bite
    { "Acid HP", nAcidHP, "Leads" },
    { "Quint Lead", aQuintLead, "Leads" },
    { "Bloom Lead", sBloomLead, "Leads" },
    { "Drift Lead", wDriftLead, "Leads" },
    { "Clarinet", aClarinet, "Leads" },
    { "Flute", aFlute, "Leads" },
    { "Reed Piano", aOboe, "Keys" },
    // ---- Plucks & Strings ----
    { "Pluck", mPluck, "Plucks & Strings" },
    { "Harp", mHarp, "Plucks & Strings" },
    { "Filter Pluck", eFilterPluck, "Plucks & Strings" },
    { "Synth Pluck", kSynthPluck, "Plucks & Strings" },
    { "Stab", mStab, "Plucks & Strings" },
    { "Steel Guitar", gSteelGuitar, "Plucks & Strings" },
    { "Muted Guitar", gMutedGuitar, "Plucks & Strings" },
    { "Nylon Guitar", mNylonGuitar, "Plucks & Strings" },
    { "Koto", mKoto, "Plucks & Strings" },
    { "Pizzicato", mPizzicato, "Plucks & Strings" },
    { "Banjo", mBanjo, "Plucks & Strings" },
    { "Sitar", mSitar, "Plucks & Strings" },
    { "Dream Pluck", nDreamPluck, "Plucks & Strings" },
    { "Nasal Pluck", aNasalPluck, "Plucks & Strings" },
    { "Dulcimer", aDulcimer, "Plucks & Strings" },
    // ---- Bells & Mallets ----
    { "Glass Bell", mGlassBell, "Bells & Mallets" },
    { "Vibraphone", mVibraphone, "Bells & Mallets" },
    { "Glass Bells", kGlassBells, "Bells & Mallets" },
    { "Vibes", kVibes, "Bells & Mallets" },
    { "Marimba Keys", kMarimbaKeys, "Bells & Mallets" },
    { "Steel Drum", moSteelDrum, "Bells & Mallets" },
    { "FM Bell", nFmBellTone, "Bells & Mallets" },   // AUDIO-RATE: KEY x3 = classic FM bell
    { "Ring Chime", nRingChime, "Bells & Mallets" }, // RING carrier TRACKS the note = tuned ring mod
    { "Mod Tubular Bell", moTubular, "Bells & Mallets" },
    { "Mod Glass", moGlass, "Bells & Mallets" },
    { "Mod Kalimba", moKalimba, "Bells & Mallets" },
    { "Glockenspiel", mGlockenspiel, "Bells & Mallets" },
    { "Celesta", mCelesta, "Bells & Mallets" },
    { "Chorus Bells", nChorusBells, "Bells & Mallets" },
    { "Music Box", aMusicBox, "Bells & Mallets" },
    { "Glass Harp", aGlassHarp, "Bells & Mallets" },
    { "Aurora Bells", sAuroraBells, "Bells & Mallets" },
    // ---- Chords & Arps ----
    { "Power Keys", kPowerKeys, "Chords & Arps" },
    { "Octave Bells", kOctaveBells, "Chords & Arps" },
    { "Dorian Pad", kDorianPad, "Chords & Arps" },
    { "Major Choir", kMajorChoir, "Chords & Arps" },
    { "Minor Rhodes", kMinorRhodes, "Chords & Arps" },
    { "House Stab", kHouseStab, "Chords & Arps" },
    { "Rave Stab", kRaveStab, "Chords & Arps" },
    { "Trance Arp", kTranceArp, "Chords & Arps" },
    { "Acid Arp", kAcidArp, "Chords & Arps" },
    { "Dream Arp", kDreamArp, "Chords & Arps" },
    { "Strum Acoustic", gStrumAcoustic, "Chords & Arps" },
    { "Strum Electric", gStrumElectric, "Chords & Arps" },
    // ---- Risers & Falls ----
    { "Noise Sweep", mNoiseSweep, "Risers & Falls" },
    { "Grain Riser", grGrainRiser, "Risers & Falls" },   // GRANULAR
    { "Uplifter", mUplifter, "Risers & Falls" },
    { "Jet Riser", nJetRiser, "Risers & Falls" },    // CHANNEL FX CHARACTER: flanger char 0.92 = screaming jet
    { "Tape Stop", mTapeStop, "Risers & Falls" },
    { "Dive", mDive, "Risers & Falls" },
    { "Sub Rise", mSubRise, "Risers & Falls" },
    { "Riser", mRiser, "Risers & Falls" },
    { "Big Riser", mBigRiser, "Risers & Falls" },
    // ---- Impacts & Booms ----
    { "Sub Drop", mSubDrop, "Impacts & Booms" },
    { "Slam", mSlam, "Impacts & Booms" },
    { "Thud", mThud, "Impacts & Booms" },
    { "Blast", mBlast, "Impacts & Booms" },
    { "Braam", mBraam, "Impacts & Booms" },
    // ---- Noise & Texture ----
    { "Wind", mWind, "Noise & Texture" },
    { "Dust Motes", grDustMotes, "Noise & Texture" },   // GRANULAR
    { "Shimmer Drone", grShimmerDrone, "Noise & Texture" },   // GRANULAR
    { "Siren", eSiren, "Noise & Texture" },
    { "Chopper", eChopper, "Noise & Texture" },
    { "Rain", mRain, "Noise & Texture" },
    { "Evil Laugh", uEvilLaugh, "Noise & Texture" },   // USER IMPORT (evil laugh)
    { "Robot Drone", nRobotDrone, "Noise & Texture" },// AUDIO-RATE AM (near-full) + vowel = robot buzz
    { "Ocean", mOcean, "Noise & Texture" },
    { "Ambient Wash", nAmbientWash, "Noise & Texture" },
    { "Evolver", sEvolver, "Noise & Texture" },
};
static constexpr int kNumMixes = (int) (sizeof(kMixes) / sizeof(kMixes[0]));

juce::StringArray mixNames()
{
    juce::StringArray a;
    for (auto& m : kMixes) a.add(m.name);
    return a;
}

juce::StringArray mixCategories()
{
    juce::StringArray a;
    for (auto& m : kMixes) a.add(m.cat);
    return a;
}

juce::String mixSourceTag(int index)
{
    if (index < 0 || index >= kNumMixes) return {};
    DC tmp; kMixes[index].build(tmp);
    // Sounds that author slots[] directly (Modal / Physical / Analog+FM / Synth ...) -> derive the tag from
    // the slot ENGINE, not the legacy srcOn[] flags (which are empty for them -> they all showed "(Synth)").
    // Oscillator sounds are tagged by what they USE (engine renamed "Oscillator", 2026-07-10):
    // a slot playing the drawn Custom wave = "Additive"; any FM engaged = "Oscillator+FM"; pure =
    // "Oscillator". Two OSC slots (e.g. lead + sub) are still one engine family, NOT "Hybrid".
    static const char* eng[] = { "Sample", "Noise", "Oscillator", "FM", "Karplus-Strong", "Synth", "Wave", "Modal", "Granular" };
    int firstEng = -1; bool sameEng = true, anyEng = false, allOsc = true, anyFM = false, anyAdd = false;
    for (auto& sl : tmp.slots)
        if (sl.engine >= 0 && sl.weight > 0.001f)
        {
            anyEng = true;
            if (sl.engine != DC::SrcOsc) allOsc = false;
            else { if (sl.fmDepth > 0.001f) anyFM = true;
                   if (sl.oscShape >= DC::WvCustom) anyAdd = true; }
            if (firstEng < 0) firstEng = sl.engine; else if (sl.engine != firstEng) sameEng = false;
        }
    if (anyEng) {
        if (allOsc) return anyAdd ? "Additive" : (anyFM ? "Oscillator+FM" : "Oscillator");
        if (sameEng && firstEng >= 0 && firstEng < (int) (sizeof(eng) / sizeof(eng[0]))) return eng[firstEng];
        return "Hybrid";
    }
    // Legacy per-engine authoring (factory drum sounds): read the srcOn[] flags. Legacy "FM"
    // sounds run on the merged Analog+FM engine after conversion, so tag them that way.
    static const char* names[DC::NUM_SOURCES] = { "Sample", "Noise", "Oscillator", "Oscillator+FM", "Karplus-Strong" };
    int count = 0, last = -1;
    for (int s = 0; s < DC::NUM_SOURCES; ++s)
        if (tmp.srcOn[s] && tmp.srcWeight[s] > 0.001f) { ++count; last = s; }
    if (count == 1) return names[last];
    if (count > 1) return "Hybrid";
    return "Synth";
}

// THE MODERN INVARIANT: a channel's slots[] ARE its sound the moment any builder finishes.
// Old-style builders describe the sound via per-engine channel fields and leave slots empty -
// finishSound() converts them to slots RIGHT HERE, once, so nothing downstream ever needs a
// legacy pass again (applyPreset's old blanket buildSlotsFromLegacy wiped slot-authored sounds).
// Message thread only (allocates KS lines).
static void finishSound(DC& ch)
{
    bool authored = false;
    for (auto& s : ch.slots) if (s.engine >= 0) { authored = true; break; }
    if (! authored) ch.buildSlotsFromLegacy();
    // UI-REPLICABILITY (2026-07-08, user rule: every factory sound must be reproducible from the
    // UI alone). Channel-level DRIVE has no UI (the visible Drive is PER-SLOT) - migrate it onto the
    // audible slots. Channel SENDS are the CANONICAL, VISIBLE controls now (CHANNEL FX box faders,
    // 2026-07-13) so they stay right where they are.
    {
        const bool anyDrive = ch.driveType != DC::DriveOff && ch.driveAmount > 0.001f;
        if (anyDrive)
        {
            for (auto& sl : ch.slots)
            {
                if (sl.engine < 0 || sl.weight <= 0.001f) continue;
                if (sl.fxDrive <= 0.0001f) { sl.fxDriveType = ch.driveType; sl.fxDrive = ch.driveAmount; }
            }
            ch.driveType = DC::DriveOff; ch.driveAmount = 0.0f;
        }
    }
    // === NO HIDDEN PARAMETERS (user rule, EMPHATIC) ===================================
    // A factory sound's tone MUST be fully VISIBLE + editable in the UI - never applied in the
    // background. The old drum "clean the low mud" high-pass lived on the CHANNEL EQ, which only
    // showed as a faint gain-0 handle under the "All" tab = effectively invisible. Move it onto
    // each audible slot's VISIBLE resonant FILTER (High-Pass mode), then clear the channel EQ.
    // The slot SVF is 12 dB/oct vs the EQ's 24, so the mud-cut is a touch gentler (accepted).
    for (int b = 0; b < DC::NUM_EQ_BANDS; ++b)
        if (ch.eqBand[b].on && b == DC::EQ_HP)
        {
            for (auto& sl : ch.slots)
                if (sl.engine >= 0 && sl.weight > 0.001f && sl.filterType == DC::FilterOff)
                { sl.filterType = DC::HighPass; sl.filterCutoff = ch.eqBand[b].freq; sl.filterReso = 0.707f; }
            ch.eqBand[b] = DC::defaultEqBand(b);   // off the (now removed) channel EQ
        }
    // Slot-authored KS sounds (e.g. String Keys = Physical) never hit buildSlotsFromLegacy, so
    // allocate the lazy KS delay lines here too - else the audio thread gates them to SILENCE
    // (ksReady == false) until a later prepareToPlay happens to allocate them.
    ch.ensureKsBuffers();
    ch.rebuildAddTables();   // additive (Custom-wave) sounds: bake the drawn harmonics (message thread)
}

void applyMix(DC& ch, int index)
{
    if (index < 0 || index >= kNumMixes) return;
    kMixes[index].build(ch);
    // Keyboard-friendly sounds ship with the TRANSPOSE LOCK on (Freq faders disabled), so nothing can
    // sneakily shift their pitch reference. Category-based: the whole Keys bank. (clearSound reset it.)
    const juce::String cat(kMixes[index].cat);   // keyboard-first categories open in PIANO ROLL by
    if (cat == "Keys" || cat == "Pads & Choirs" || cat == "Leads" || cat == "Chords & Arps")
        ch.drawMode = true;   // default (user; step data is kept underneath)
    finishSound(ch);
}

//==============================================================================
// Presets: whole-kit grooves (channels 0..7) + BPM + time signature.
//==============================================================================
// The factory-sound name that matches a builder, so a preset's channels show
// the sound they use in the strip dropdown (e.g. "Sine Kick").
static juce::String nameForBuilder(Builder b)
{
    for (auto& m : kMixes) if (m.build == b) return juce::String(m.name);
    return {};
}

// Build channel `i` of pattern `pat` with a sound + step pattern (for multi-pattern presets).
static void buildChP(Sequencer& s, int pat, int i, Builder build, int n, std::initializer_list<int> on)
{
    auto& c = s.patterns[pat].channels[i];
    build(c);
    // Same finishing as applyMix: slots become the sound HERE (works for old-style AND
    // slot-authored builders - presets may reference ANY factory sound now), Keys bank locks.
    finishSound(c);
    c.mixName = nameForBuilder(build); c.mixModified = false;
    setSteps(c, n, on);
}

// WHOLE-SONG preset via pattern CHAINING: Beethoven's "Ode to Joy" (public domain = royalty-free).
// The recognizable 8-bar main theme = EIGHT one-bar patterns, CHAINED 0->1->...->7->0 so it plays
// end to end and loops. A pattern is always ONE BAR (numSteps just sets the grid), so 16 steps =
// a 16th grid and QUARTER notes land every 4 steps (0,4,8,12). Melody on a soft celesta; a beat
// that builds (kick throughout, hats join for the 2nd half). No bass: the bass sounds' base
// pitches don't land on a clean C, and a detuned sub under a bell melody is not something to guess.
static void pOdeToJoy(Sequencer& s)
{
    s.standaloneBpm = 120.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    // Per-step PITCH = semitones from the tonic C: C=0 D=+2 E=+4 F=+5 G=+7. The dotted phrase-
    // endings ("E. D D" / "D. C C") keep the real rhythm: dotted-quarter (step 0, 6 sixteenths),
    // eighth (step 6), half (step 8).
    struct Note { int step, semi; };
    auto melody = [&](int pat, std::initializer_list<Note> notes) {
        buildChP(s, pat, 0, mCelesta, 16, {});
        auto& c = s.patterns[pat].channels[0];
        for (auto n : notes) { c.steps[n.step] = true; c.stepPitch[n.step] = (float) n.semi; }
    };
    melody(0, { {0,4},{4,4},{8,5},{12,7} });   // bar 1:  E E F G
    melody(1, { {0,7},{4,5},{8,4},{12,2} });   // bar 2:  G F E D
    melody(2, { {0,0},{4,0},{8,2},{12,4} });   // bar 3:  C C D E
    melody(3, { {0,4},{6,2},{8,2} });          // bar 4:  E. D D
    melody(4, { {0,4},{4,4},{8,5},{12,7} });   // bar 5:  E E F G
    melody(5, { {0,7},{4,5},{8,4},{12,2} });   // bar 6:  G F E D
    melody(6, { {0,0},{4,0},{8,2},{12,4} });   // bar 7:  C C D E
    melody(7, { {0,2},{6,0},{8,0} });          // bar 8:  D. C C  (resolves to C)
    // Building beat: soft kick on beats 1 & 3 of every bar; hats join for the second half (bars 5-8).
    for (int p = 0; p < 8; ++p) buildChP(s, p, 1, m808Kick, 16, { 0, 8 });   // (Sine Kick removed - 0.995 twin)
    for (int p = 4; p < 8; ++p) buildChP(s, p, 2, mClosedHat, 16, { 0,2,4,6,8,10,12,14 });
    // Chain the eight bars into one continuous, looping playthrough.
    for (int p = 0; p < 8; ++p) {
        auto& P = s.patterns[p];
        P.playMode = Sequencer::Chain;
        P.chainLen = 1; P.chainSeq[0] = (p + 1) % 8; P.chainLoops[0] = 1;
    }
    auto& m = s.current().master;
    m.reverbWet = 0.28f; m.reverbRoom = 0.6f; m.tilt = 0.54f;   // a little concert-hall air
}

// Multi-pattern preset: pattern 1 is a riser intro that auto-jumps to the drop in
// pattern 2 (which then loops). Shows off pattern-switching + the long-attack riser.
static void pRiserDrop(Sequencer& s)
{
    s.standaloneBpm = 128.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    // -- Pattern 1 (index 0): the riser build-up, then jump to the drop. --
    buildChP(s, 0, 0, mBigRiser,  16, { 0 });             // one long swell across the bar
    buildChP(s, 0, 1, mPunchKick, 16, { 8, 12, 14, 15 }); // accelerating kick build
    buildChP(s, 0, 2, mClosedHat, 16, { 0,2,4,6,8,10,12,14 });
    s.patterns[0].playMode = Sequencer::NextAfterN;       // after 1 loop -> pattern 2
    s.patterns[0].gotoPattern = 1;
    s.patterns[0].repeatTarget = 1;
    // -- Pattern 2 (index 1): the drop / main groove (loops). --
    buildChP(s, 1, 0, mPunchKick,  16, { 0,4,8,12 });
    buildChP(s, 1, 1, mClap,      16, { 4, 12 });   // (909 Clap removed - the tight Clap stands in)
    buildChP(s, 1, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildChP(s, 1, 3, mReeseBass,  16, { 0,3,6,8,11,14 });
    buildChP(s, 1, 4, mOpenHat,    16, { 2,6,10,14 });
    s.patterns[1].playMode = Sequencer::LoopForever;
    s.current().master.reverbWet = 0.18f;
}



//==============================================================================
// PUBLIC-DOMAIN SONG PRESETS (2026-07-08, user order: "at least 5", multi-pattern chains).
// Same recipe as Ode to Joy: one pattern = ONE BAR, per-step PITCH = semitones from the
// sound's BASE (keys/bells sounds sit on a clean C; "Deep Sub" sits on A1 = 55 Hz, so its
// offsets are computed FROM A). Chained 0->1->...->last->0 = a looping playthrough.
// SUSTAINED sounds (EP/brass/pads/sub, sustain > 0) need a GATE to hold their written note
// length: each note's `len` (in steps) becomes a STEP-MERGE run - the head fires, the merged
// steps hold the gate, then the authored release. len 1 = a plain step (percussive sounds).
//==============================================================================
struct SongNote { int step, semi, len; };
static void songNotes(Sequencer& s, int pat, int ch, Builder snd, int n, std::initializer_list<SongNote> notes)
{
    auto& c = s.patterns[pat].channels[ch];
    if (c.mixName.isEmpty()) buildChP(s, pat, ch, snd, n, {});
    for (auto nn : notes)
    {
        c.steps[nn.step] = true; c.stepPitch[nn.step] = (float) nn.semi;
        for (int i = nn.step + 1; i < nn.step + nn.len && i < n; ++i) c.stepMerge[i] = true;
    }
}
static void songChain(Sequencer& s, int bars, float verb)
{
    for (int p = 0; p < bars; ++p)
    {
        auto& P = s.patterns[p];
        P.playMode = Sequencer::Chain;
        P.chainLen = 1; P.chainSeq[0] = (p + 1) % bars; P.chainLoops[0] = 1;
        P.master.reverbWet = verb;                    // every CHAINED bar gets the room (not just bar 1)
    }
}

// "Twinkle Twinkle Little Star" (trad.) - 12 bars: A A B B A A form on a high glockenspiel
// (sustain-0 bell = natural ring, no gates needed).
static void pTwinkle(Sequencer& s)
{
    s.standaloneBpm = 100.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    auto A1 = { SongNote{0,0,1},{4,0,1},{8,7,1},{12,7,1} };   // C C G G
    auto A2 = { SongNote{0,9,1},{4,9,1},{8,7,1} };            // A A G .
    auto A3 = { SongNote{0,5,1},{4,5,1},{8,4,1},{12,4,1} };   // F F E E
    auto A4 = { SongNote{0,2,1},{4,2,1},{8,0,1} };            // D D C .
    auto B1 = { SongNote{0,7,1},{4,7,1},{8,5,1},{12,5,1} };   // G G F F
    auto B2 = { SongNote{0,4,1},{4,4,1},{8,2,1} };            // E E D .
    const std::initializer_list<SongNote> bars[12] = { A1,A2,A3,A4, B1,B2, B1,B2, A1,A2,A3,A4 };
    for (int p = 0; p < 12; ++p) songNotes(s, p, 0, mGlockenspiel, 16, bars[p]);
    for (int p = 0; p < 12; ++p) buildChP(s, p, 1, m808Kick, 16, { 0, 8 });
    for (int p = 4; p < 12; ++p) buildChP(s, p, 2, mShaker, 16, { 0,2,4,6,8,10,12,14 });
    songChain(s, 12, 0.24f);
}

// "Fur Elise" (Beethoven) - the A section as a music box, on a CONTINUOUS 16th timeline
// (v1 left a quarter-note hole after the run - user caught it against the sheet music).
// Each 12-step 3/4 bar = TWO 3/8 bars of the score. P0 = pickup + the E-D# run (intro only);
// P1 = the A bar + B bar; P2 = the C bar + the run again; the loop bounces P1 <-> P2 so the
// run always lands straight onto A with no gap. Toy Piano offsets from C5; sub from A1.
static void pFurElise(Sequencer& s)
{
    s.standaloneBpm = 72.0f; s.timeSigNum = 3; s.timeSigDen = 4;
    // P0: [. . . . E D# | E D# E B D C]  (pickup + run, 16ths)
    songNotes(s, 0, 0, kToyPiano, 12, { {4,4,1},{5,3,1},{6,4,1},{7,3,1},{8,4,1},{9,-1,1},{10,2,1},{11,0,1} });
    // P1: [A . . C E A | B . . E G# B]  (melody A + low pickup; then B + its pickup)
    songNotes(s, 1, 0, kToyPiano, 12, { {0,-3,1},{3,-12,1},{4,-8,1},{5,-3,1},{6,-1,1},{9,-8,1},{10,-4,1},{11,-1,1} });
    // P2: [C . . . E D# | E D# E B D C]  (resolution + the run straight back in)
    songNotes(s, 2, 0, kToyPiano, 12, { {0,0,1},{4,4,1},{5,3,1},{6,4,1},{7,3,1},{8,4,1},{9,-1,1},{10,2,1},{11,0,1} });
    // left hand: A-minor / E-major broken chords under the downbeats (natural sub ring)
    buildChP(s, 0, 1, kSubBass, 12, {});
    songNotes(s, 1, 1, kSubBass, 12, { {0,12,1},{1,19,1},{2,24,1},{6,7,1},{7,19,1},{8,23,1} });   // A2 E3 A3 | E2 E3 G#3
    songNotes(s, 2, 1, kSubBass, 12, { {0,12,1},{1,19,1},{2,24,1} });                             // A2 E3 A3
    songChain(s, 3, 0.3f);
    s.patterns[2].chainSeq[0] = 1;   // P2 -> P1 (not back to the intro pickup)
}

// "Amazing Grace" (trad. 1779) - 3/4 hymn: gated e-piano melody, bar-long merged pad chords,
// sub roots. Quarter = 4 steps, half = 8, dotted half = the whole 12-step bar.
static void pAmazingGrace(Sequencer& s)
{
    s.standaloneBpm = 84.0f; s.timeSigNum = 3; s.timeSigDen = 4;
    songNotes(s, 0, 0, kEPiano, 12, { {8,-5,4} });                    // pickup: "A-" (G3)
    songNotes(s, 1, 0, kEPiano, 12, { {0,0,8},{8,4,2},{10,0,2} });    // "MA-"(C4) + zing (E-C)
    songNotes(s, 2, 0, kEPiano, 12, { {0,4,8},{8,2,4} });             // "grace"(E) "how"(D)
    songNotes(s, 3, 0, kEPiano, 12, { {0,0,8},{8,-3,4} });            // "sweet"(C) "the"(A3)
    songNotes(s, 4, 0, kEPiano, 12, { {0,-5,8},{8,-5,4} });           // "sound"(G3) + pickup "that"
    songNotes(s, 5, 0, kEPiano, 12, { {0,0,8},{8,4,2},{10,0,2} });    // "saved a"
    songNotes(s, 6, 0, kEPiano, 12, { {0,4,8},{8,2,4} });             // "wretch like"
    songNotes(s, 7, 0, kEPiano, 12, { {0,7,12} });                    // "me"(G4 rings the bar)
    static const int pad[8]  = { 0, 0, 0, -7, 0, 0, 0, -5 };          // C C C F C | C C G
    static const int root[8] = { 3, 3, 3, 8, 3, 3, 3, 10 };           // sub roots (from A1): C2 F2 G2
    for (int p = 0; p < 8; ++p)
    {
        songNotes(s, p, 1, kWarmPad, 12, { {0, pad[p], 12} });        // whole-bar merged pad note
        songNotes(s, p, 2, kSubBass, 12, { {0, root[p], 12} });
    }
    songChain(s, 8, 0.32f);
}

// "When the Saints Go Marching In" (trad.) - gated brass lead, marching kit, two-beat bass,
// light swing. Ends on the half cadence (D over G) so the loop pulls back to the top.
static void pSaints(Sequencer& s)
{
    s.standaloneBpm = 112.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    songNotes(s, 0, 0, kSynthBrass, 16, { {4,0,4},{8,4,4},{12,5,4} });   // "oh when the" C E F
    songNotes(s, 1, 0, kSynthBrass, 16, { {0,7,16} });                   // "saints" G (whole)
    songNotes(s, 2, 0, kSynthBrass, 16, { {4,0,4},{8,4,4},{12,5,4} });
    songNotes(s, 3, 0, kSynthBrass, 16, { {0,7,16} });
    songNotes(s, 4, 0, kSynthBrass, 16, { {4,0,4},{8,4,4},{12,5,4} });
    songNotes(s, 5, 0, kSynthBrass, 16, { {0,7,8},{8,4,8} });            // "saints(G) go(E)"
    songNotes(s, 6, 0, kSynthBrass, 16, { {0,0,8},{8,4,8} });            // "march(C)-ing(E)"
    songNotes(s, 7, 0, kSynthBrass, 16, { {0,2,16} });                   // "in"(D, whole)
    for (int p = 0; p < 8; ++p)
    {
        buildChP(s, p, 1, m909Kick,   16, { 0, 8 });
        buildChP(s, p, 2, mSnapSnare, 16, { 4, 12 });
        songNotes(s, p, 3, kSubBass, 16, { {0,3,8},{8,-2,8} });          // two-beat C2 / G1
        if (p >= 4) buildChP(s, p, 4, mClosedHat, 16, { 0,2,4,6,8,10,12,14 });
        s.patterns[p].swing = 0.35f;                                     // light dixieland lilt (~59%)
    }
    songChain(s, 8, 0.2f);
}

// "Canon" (Pachelbel, in C) - the ground: sub roots + SCALE-voiced bell triads (Octave Bells
// harmonizes each root with its correct diatonic chord - the SCALE-mode showcase); the violin
// descent + shakers join for the second half. Two chords per bar (half notes).
static void pCanon(Sequencer& s)
{
    s.standaloneBpm = 72.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    static const int roots[8]  = { 0, 7, 9, 4, 5, 0, 5, 7 };       // C G Am Em F C F G (rel C4)
    static const int subs[8]   = { 3, -2, 0, -5, -4, 3, -4, -2 };  // C2 G1 A1 E1 F1 C2 F1 G1 (rel A1)
    static const int violin[8] = { 16, 14, 12, 11, 9, 7, 9, 11 };  // E5 D5 C5 B4 A4 G4 A4 B4
    for (int p = 0; p < 8; ++p)
    {
        const int a = (p * 2) % 8, b = (p * 2 + 1) % 8;
        songNotes(s, p, 0, kSubBass,     16, { {0, subs[a], 8},  {8, subs[b], 8} });
        songNotes(s, p, 1, kOctaveBells, 16, { {0, roots[a], 8}, {8, roots[b], 8} });
        if (p >= 4)
        {
            songNotes(s, p, 2, kEPiano, 16, { {0, violin[a], 8}, {8, violin[b], 8} });
            buildChP(s, p, 3, mShaker, 16, { 2,6,10,14 });
        }
    }
    songChain(s, 8, 0.3f);
}

// PIANO-ROLL presets: these channels are authored as DrawNotes (drawMode), not steps - held
// voices under moving 16ths, and real CHORDS (overlapping notes), which step mode cannot hold.
struct RollNote { int start, len, semi, vel; };
static void rollNotes(Sequencer& s, int pat, int ch, Builder snd, std::initializer_list<RollNote> notes)
{
    buildChP(s, pat, ch, snd, 16, {});
    auto& c = s.patterns[pat].channels[ch];
    c.drawMode = true;
    for (auto n : notes) c.addDrawNote(n.start, n.len, n.semi, n.vel);
}

// "Prelude in C" (Bach, WTC I) - the figuration: a HELD bass + held alto under running 16ths,
// exactly what the piano roll can express and steps can't. 4 bars (C / Dm7 / G7 / C), looped.
static void pPreludeC(Sequencer& s)
{
    s.standaloneBpm = 66.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    static const int bass[4] = { -12, -12, -13, -12 };            // C3 C3 B2 C3
    static const int alto[4] = { 4, 2, 2, 4 };                    // E4 D4 D4 E4
    static const int fig[4][3] = { {7,12,16}, {9,14,17}, {7,14,17}, {7,12,16} };   // G C' E' etc.
    for (int p = 0; p < 4; ++p)
    {
        juce::Array<RollNote> n;                                  // one 16th = 24 columns
        for (int half = 0; half < 2; ++half)
        {
            const int o = half * 192;
            n.add({ o,      184, bass[p], 105 });                 // held bass (half note)
            n.add({ o + 24, 160, alto[p], 95 });                  // held alto, a 16th later
            for (int r = 0; r < 2; ++r)                           // the G-C-E figure, twice
                for (int k = 0; k < 3; ++k)
                    n.add({ o + 48 + (r * 3 + k) * 24, 24, fig[p][k], 100 });
        }
        buildChP(s, p, 0, kGrandPiano, 16, {});
        auto& c = s.patterns[p].channels[0];
        c.drawMode = true;
        for (auto& rn : n) c.addDrawNote(rn.start, rn.len, rn.semi, rn.vel);
    }
    songChain(s, 4, 0.28f);
}

// "Lofi Chill" (groove) - piano-roll CHORD showcase: whole-bar e-piano 7th chords (4 overlapping
// notes = impossible in step mode) over a dusty swung kit and vinyl crackle.
static void pLofiChill(Sequencer& s)
{
    s.standaloneBpm = 75.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    static const int chords[4][4] = { { 0, 4, 7, 11 },     // Cmaj7
                                      { -3, 0, 4, 7 },     // Am7
                                      { -7, -3, 0, 4 },    // Fmaj7
                                      { -5, -1, 2, 5 } };  // G7
    for (int p = 0; p < 4; ++p)
    {
        rollNotes(s, p, 0, kEPiano, { { 0, 336, chords[p][0], 88 }, { 0, 336, chords[p][1], 84 },
                                      { 0, 336, chords[p][2], 84 }, { 0, 336, chords[p][3], 90 } });
        buildChP(s, p, 1, mBreakKick,  16, { 0, 10 });
        buildChP(s, p, 2, mBrushSnare, 16, { 4, 12 });
        buildChP(s, p, 3, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
        buildChP(s, p, 4, mRain,       16, { 0,2,4,6,8,10,12,14 });   // Vinyl removed (user) - Rain = the crackle bed
        s.patterns[p].swing = 0.45f;
    }
    songChain(s, 4, 0.22f);
}

using PresetFn = void (*)(Sequencer&);
static const struct { const char* name; PresetFn build; } kPresets[] = {
    { "Riser + Drop",       pRiserDrop },
    { "Ode to Joy (Beethoven)", pOdeToJoy },
    { "Fur Elise (Beethoven)",  pFurElise },
    { "Canon (Pachelbel)",      pCanon },
    { "Twinkle Twinkle (trad.)", pTwinkle },
    { "Amazing Grace (trad.)",  pAmazingGrace },
    { "When the Saints (trad.)", pSaints },
    { "Prelude in C (Bach)",    pPreludeC },
    { "Lofi Chill (groove)",    pLofiChill },
};
static constexpr int kNumPresets = (int) (sizeof(kPresets) / sizeof(kPresets[0]));

juce::StringArray presetNames()
{
    juce::StringArray a;
    for (auto& p : kPresets) a.add(p.name);
    return a;
}

// A preset defines the WHOLE instrument, so loading one must wipe every pattern
// back to a clean state first (no leftover steps, mute/solo, or master FX from
// the previous preset). Then the chosen groove is laid onto pattern 0.
static void resetAll(Sequencer& s)
{
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        auto& P = s.patterns[p];
        P.swing = 0.0f; P.playMode = Sequencer::LoopForever; P.repeatTarget = 2;
        P.gotoPattern = (p + 1) % Sequencer::NUM_PATTERNS;
        P.chainLen = 0; P.chainStep = 0;
        P.mergeWithPrev = false;                   // merged groups are preset-level -> reset
        P.master = Sequencer::MasterFX();          // default master FX + output
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            auto& ch = P.channels[c];
            clearSound(ch);
            finishSound(ch);   // even an EMPTY channel carries its (init) sound as slots - the
                               // modern invariant; applyPreset's blanket legacy pass is GONE
            ch.mute = false; ch.solo = false;
            ch.chokeGroup = 0; ch.outputBus = 0; ch.midiOut = false; ch.midiOutChannel = 1;   // routing/choke are preset-level -> reset them
            ch.revBus = 0; ch.delBus = 0;   // reverb/delay bus assignment is routing too
            ch.duckBy = -1; ch.duckAmt = 0.5f;   // sidechain duck is routing-like -> preset-level too
            ch.keysSlot2Down = 0;   // KEYS slot-2 transpose (channel-wide) is preset-level too
            ch.humanizeAmt = 0.0f; ch.strumAmt = 0.0f; ch.keysMinVel = 0.0f; ch.keysMaxVel = 1.0f; ch.keysGlide = 0.0f;   // HUMANIZE / STRUM / vel range / GLIDE reset
            for (int f2 = 0; f2 < 2; ++f2) { ch.chFxType[f2] = 0; ch.chFxAmt[f2] = 0.0f; ch.chFxChar[f2] = 0.5f; }   // CHANNEL FX reset
            ch.reverbSend = 0.0f; ch.delaySend = 0.0f;
            ch.mergeWith = -1; ch.keysSplitW1 = 60; ch.keysSplitW2 = 12;   // MERGE&SPLIT reset (per-pattern pairing + split windows)
            { DC d; ch.arpOn = d.arpOn; ch.arpLen = d.arpLen; ch.arpSync = d.arpSync; ch.arpRate = d.arpRate;
              ch.arpAlign = d.arpAlign; ch.arpHold = d.arpHold; ch.arpGate = d.arpGate;
              for (int ai = 0; ai < DC::ARP_ROWS; ++ai) ch.arpOffset[ai] = d.arpOffset[ai]; }   // ARP reset
            ch.keysPolyMode = true;    // keys POLY by default
            ch.mixName = {}; ch.mixModified = false;
            setSteps(ch, 8, {});
        }
    }
}

void applyPreset(Sequencer& seq, int index)
{
    if (index < 0 || index >= kNumPresets) return;
    resetAll(seq);                 // fresh slate - same result every time (channels finished there)
    seq.currentPattern = 0;
    kPresets[index].build(seq);    // buildChP finishes each sound as it lands - NO legacy pass
}

} // namespace Factory
