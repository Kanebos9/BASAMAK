#include "FactoryContent.h"

using DC = DrumChannel;

namespace Factory
{
//==============================================================================
// Helpers
//==============================================================================

// Reset only the *sound* parameters of a channel to a clean, sample-free state.
// Leaves the channel's identity (name/colour/MIDI note) and its step pattern
// untouched, so applying a mix swaps the sound without disturbing the groove.
static void clearSound(DC& c)
{
    c.keysPolyMode = true;   // POLY by default (per-sound); a builder may set it off
    for (int i = 0; i < DC::NUM_SOURCES; ++i) { c.srcOn[i] = false; c.srcWeight[i] = 0.0f; }
    for (auto& s : c.slots) s = DC::Slot();   // empty all slots (engine = -1)
    c.legacyFmEnvFollow = false;              // per-sound flag - must not leak between builders
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
    c.bloom = 0.0f; c.drift = 0.0f; c.spread = 0.0f; c.punch = 0.0f; c.glue = 0.0f;
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
    c.drawMode = false; c.drawVel = 1.0f; c.drawPan = 0.0f;   // presets/sounds are step-mode
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
    c.driveType = DC::SoftClip; c.driveAmount = 0.26f;
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
    c.driveType = DC::Tube; c.driveAmount = 0.12f;
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
    c.driveType = DC::SoftClip; c.driveAmount = 0.15f; c.volume = 0.85f;
}
static void mShaker(DC& c)     // short bright noise tick
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 7000.0f; c.layerNoiseWidth = 0.20f;
    c.srcAtk[DC::SrcNoise] = 0.004f; c.srcDec[DC::SrcNoise] = 0.06f; c.volume = 0.55f;
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
    c.driveType = DC::SoftClip; c.driveAmount = 0.12f; c.volume = 0.88f;
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
    c.driveType = DC::SoftClip; c.driveAmount = 0.20f; c.volume = 0.97f;
}
static void m808Snare(DC& c) {   // 808: snappy noise + tuned body knock
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.58f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.42f;
    c.padX = 0.14f + 0.42f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 2000.0f; c.layerNoiseWidth = 0.15f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.14f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 180.0f;
    c.layerSinePEnvAmt = 4.0f; c.layerSinePEnvTime = 0.035f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.11f;
    c.eqBand[DC::EQ_HP] = { true, 150.0f, 0.0f, 0.707f };
    c.driveType = DC::SoftClip; c.driveAmount = 0.10f;
    c.reverbSend = 0.10f; c.volume = 0.86f;
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
    c.driveType = DC::HardClip; c.driveAmount = 0.25f; c.volume = 0.7f;
}
static void m808Conga(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 260.0f; c.layerSinePEnvAmt = 4.0f; c.layerSinePEnvTime = 0.05f;
    c.srcDec[DC::SrcOsc] = 0.26f; c.volume = 0.8f;
}
static void m808Maracas(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 10000.0f; c.layerNoiseWidth = 0.05f; c.srcDec[DC::SrcNoise] = 0.03f; c.volume = 0.55f;
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
    c.driveType = DC::Tube; c.driveAmount = 0.28f; c.volume = 0.97f;
}
static void m909Snare(DC& c) {   // 909: big crack, short tuned knock, tube edge
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.70f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.30f;
    c.padX = 0.14f + 0.30f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 2600.0f; c.layerNoiseWidth = 0.10f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.20f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 230.0f;
    c.layerSinePEnvAmt = 7.0f; c.layerSinePEnvTime = 0.03f;
    c.srcAtk[DC::SrcOsc] = 0.001f; c.srcDec[DC::SrcOsc] = 0.10f;
    c.eqBand[DC::EQ_HP] = { true, 250.0f, 0.0f, 0.707f };
    c.driveType = DC::Tube; c.driveAmount = 0.15f;
    c.reverbSend = 0.12f; c.volume = 0.86f;
}
static void m909Clap(DC& c) {   // big roomy 909 clap: longer + wetter than the tight Clap / darker 808
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 1500.0f; c.layerNoiseWidth = 0.24f;
    c.srcAtk[DC::SrcNoise] = 0.001f; c.srcDec[DC::SrcNoise] = 0.19f;
    c.eqBand[DC::EQ_HP] = { true, 500.0f, 0.0f, 0.707f };
    c.reverbSend = 0.30f; c.volume = 0.84f;
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
    c.driveType = DC::HardClip; c.driveAmount = 0.2f; c.volume = 0.7f;
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
    c.srcDec[DC::SrcFM] = 0.5f; c.driveType = DC::SoftClip; c.driveAmount = 0.2f; c.volume = 0.75f;
}
static void mAcidBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 60.0f; c.srcDec[DC::SrcOsc] = 0.3f;
    c.filterType = DC::LowPass; c.filterCutoff = 600.0f; c.filterReso = 4.0f; c.filterEnvAmt = 0.6f;
    c.driveType = DC::Tube; c.driveAmount = 0.2f; c.volume = 0.78f;
}
// -- Kicks --
static void mPunchKick(DC& c) {   // HARD short club/rave punch - all attack, no boom.
    // Distinct from 909 Kick (round + click + tube): higher pitch, a much FASTER/bigger knock,
    // a very short body, and HARD clipping instead of tube warmth.
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 62.0f;
    c.layerSinePEnvAmt = 36.0f; c.layerSinePEnvTime = 0.02f;   // ~3-octave snap in 20 ms = the punch IS the attack
    c.srcAtk[DC::SrcOsc] = 0.0005f; c.srcDec[DC::SrcOsc] = 0.22f;
    c.driveType = DC::HardClip; c.driveAmount = 0.32f; c.volume = 0.97f;
}
static void mDistKick(DC& c) {   // gnarly distorted kick with a proper knock
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -20.0f; c.fmSpread = 0.12f; c.fmDepth = 0.5f; c.fmSub = 0.45f;
    c.fmPitchEnvAmt = 20.0f; c.fmPitchEnvTime = 0.035f;
    c.srcDec[DC::SrcFM] = 0.40f; c.driveType = DC::Fuzz; c.driveAmount = 0.35f; c.volume = 0.92f;
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
    c.driveType = DC::Tube; c.driveAmount = 0.15f;
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
static void mPluckSynth(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 330.0f; c.srcDec[DC::SrcOsc] = 0.18f;
    c.filterType = DC::LowPass; c.filterCutoff = 3000.0f; c.filterReso = 1.5f; c.filterEnvAmt = 0.5f; c.volume = 0.78f;
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
static void mVinyl(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 2; c.layerNoiseCenter = 400.0f; c.layerNoiseWidth = 0.0f; c.srcDec[DC::SrcNoise] = 0.05f; c.volume = 0.42f;
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
static void mWhistle(DC& c) {       // pure sine + vibrato + soft attack = a whistle/ocarina
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 1000.0f;
    c.srcAtk[DC::SrcOsc] = 0.025f; c.srcDec[DC::SrcOsc] = 1.8f; c.oscSustain = 0.0f;   // long ring-out (was sustain 0.85 -> now a real fade-out)
    c.oscVibrato = 0.5f;                                  // the wobble that sells it
    c.layerSinePEnvAmt = 1.5f; c.layerSinePEnvTime = 0.06f; // tiny pitch scoop up on the attack
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
static void moMarimba(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 0; s.oscFreq = 262.0f; s.modalDecay = 0.32f; s.modalTone = 0.45f; s.modalStruct = 0.5f; c.volume = 0.9f; }
static void moTubular(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 1; s.oscFreq = 330.0f; s.modalDecay = 0.85f; s.modalTone = 0.6f;  s.modalStruct = 0.5f; c.reverbSend = 0.25f; c.volume = 0.7f; }
static void moGlass(DC& c)     { auto& s = mkModal(c); s.modalMaterial = 2; s.oscFreq = 520.0f; s.modalDecay = 0.55f; s.modalTone = 0.8f;  s.modalStruct = 0.55f; c.reverbSend = 0.2f; c.volume = 0.72f; }
static void moTomDrum(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 3; s.oscFreq = 110.0f; s.modalDecay = 0.3f;  s.modalTone = 0.5f;  s.modalStruct = 0.5f; c.volume = 0.88f; }   // membrane = tom/drum
static void moMetalPlate(DC& c){ auto& s = mkModal(c); s.modalMaterial = 4; s.oscFreq = 280.0f; s.modalDecay = 0.8f;  s.modalTone = 0.75f; s.modalStruct = 0.6f; c.reverbSend = 0.25f; c.volume = 0.7f; }
static void moWoodBlock(DC& c) { auto& s = mkModal(c); s.modalMaterial = 5; s.oscFreq = 900.0f; s.modalDecay = 0.12f; s.modalTone = 0.5f;  s.modalStruct = 0.5f; c.volume = 0.78f; }
static void moKalimba(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 6; s.oscFreq = 440.0f; s.modalDecay = 0.4f;  s.modalTone = 0.5f;  s.modalStruct = 0.5f; c.volume = 0.82f; }
static void moCowbell(DC& c)   { auto& s = mkModal(c); s.modalMaterial = 7; s.oscFreq = 540.0f; s.modalDecay = 0.3f;  s.modalTone = 0.6f;  s.modalStruct = 0.55f; c.volume = 0.75f; }
static void moBellTuned(DC& c) { auto& s = mkModal(c); s.modalMaterial = 1; s.oscFreq = 660.0f; s.modalDecay = 0.95f; s.modalTone = 0.7f;  s.modalStruct = 0.42f; c.reverbSend = 0.3f;  c.volume = 0.65f; }   // tuned (low Struct)
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
    s.fxDriveType = DC::Tube; s.fxDrive = 0.18f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.58f);
    b.oscShape = b.oscShapeB = DC::WvSquare; b.oscFreq = 27.5f;   // sub: -1 octave, square = BS2 style
    b.atk = 0.002f; b.dec = 0.55f;
    b.filterType = DC::LowPass; b.filterCutoff = 260.0f; b.filterReso = 0.8f; b.filterEnvAmt = 0.2f;
    c.volume = 0.8f;
}
static void bLadderBass(DC& c) {    // warm funk: saw through a low, barely-resonant LP + clean sine sub
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.atk = 0.002f; s.dec = 0.32f;
    s.filterType = DC::LowPass; s.filterCutoff = 420.0f; s.filterReso = 1.1f; s.filterEnvAmt = 0.4f;
    s.fxDriveType = DC::SoftClip; s.fxDrive = 0.12f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.55f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 27.5f;
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
    s.fxDriveType = DC::SoftClip; s.fxDrive = 0.10f;
    c.volume = 0.85f;
}
static void bNeuroBass(DC& c) {     // modern growl: folded+FM'd saw with a swept resonant LP, CLEAN sine sub under it
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 55.0f;
    s.oscWarp = 0.35f;
    s.fmDepth = 0.35f; s.fmSpread = 0.2f; s.fmEnvFollow = true;    // ratio 2, riding the env = "talking" top
    s.atk = 0.002f; s.dec = 0.6f;
    s.filterType = DC::LowPass; s.filterCutoff = 900.0f; s.filterReso = 2.8f; s.filterEnvAmt = 0.65f;
    s.lfoRate[0] = 3.5f; s.lfoAmt[0] = 0.4f;   // filter-LFO wobble on top of the env sweep = "talking" growl
    s.fxDriveType = DC::Foldback; s.fxDrive = 0.22f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.60f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 27.5f;     // untouched low end - growl stays on top
    b.atk = 0.002f; b.dec = 0.65f;
    c.volume = 0.78f;
}
static void bHooverBass(DC& c) {    // rave hoover: wide 5-voice saw that RISES an octave into its pitch
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 65.41f;     // C2
    s.oscUnison = 5; s.oscDetune = 0.6f; s.oscUniCenter = true;
    s.oscWarp = 0.15f;
    s.oscPEnvAmt = -12.0f; s.oscPEnvTime = 0.09f;                  // starts -1 oct, sweeps up into the note
    s.atk = 0.002f; s.dec = 0.7f;
    s.filterType = DC::LowPass; s.filterCutoff = 2500.0f; s.filterReso = 1.4f; s.filterEnvAmt = 0.3f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.15f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.65f);
    b.oscShape = b.oscShapeB = DC::WvSquare; b.oscFreq = 32.7f;   // C1 sub keeps the bottom solid
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
    s.fxDriveType = DC::SoftClip; s.fxDrive = 0.08f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.58f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 32.7f;
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
    s.lfoRate[0] = 2.0f; s.lfoAmt[0] = 0.8f;                         // THE wobble (~1/8 feel at 120)
    s.fxDriveType = DC::Tube; s.fxDrive = 0.2f;
    auto& b = mkSlot2(c, DC::SrcOsc, 0.62f);
    b.oscShape = b.oscShapeB = DC::WvSine; b.oscFreq = 27.5f;      // steady sub - the wobble lives on top
    b.atk = 0.002f; b.dec = 1.2f;
    c.volume = 0.78f;
}
static void eSiren(DC& c) {         // classic rave/jungle siren: pitch LFO, +/-1 octave sweep
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSquare; s.oscFreq = 660.0f;
    s.atk = 0.01f; s.dec = 2.5f;
    s.lfoRate[1] = 0.9f; s.lfoAmt[1] = 1.0f;                         // slow full-depth pitch swing
    s.filterType = DC::LowPass; s.filterCutoff = 2500.0f; s.filterReso = 1.2f;
    c.reverbSend = 0.15f; c.volume = 0.6f;
}
static void eChopper(DC& c) {       // helicopter: brown noise chopped by a fast full-depth volume LFO
    auto& s = mkSlot(c, DC::SrcNoise);
    s.noiseType = 2;                                                 // brown = deep rumble
    s.atk = 0.05f; s.dec = 2.0f;
    s.lfoRate[2] = 11.0f; s.lfoAmt[2] = 1.0f;                        // rotor-blade tremolo
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
    s.fxDriveType = DC::Tube; s.fxDrive = 0.15f;
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
    s.oscUnison = 4; s.oscDetune = 0.3f; s.oscUniCenter = true;
    s.atk = 0.18f; s.dec = 1.5f; s.sustain = 0.8f; s.release = 0.7f;
    s.filterType = DC::LowPass; s.filterCutoff = 1400.0f; s.filterReso = 0.9f;
    c.reverbSend = 0.25f; c.volume = 0.68f;
}
static void kOrgan(DC& c) {         // drawbar-ish organ: octave-stack wave at FULL sustain (gate on/off)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 9;                                    // Organ (additive octave stack)
    s.oscFreq = 261.63f;
    s.atk = 0.005f; s.dec = 0.3f; s.sustain = 1.0f; s.release = 0.08f;
    c.reverbSend = 0.10f; c.volume = 0.72f;
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
    s.lfoRate[2] = 5.5f; s.lfoAmt[2] = 0.35f;                        // VOL tremolo = the Wurli wobble
    c.reverbSend = 0.14f; c.volume = 0.8f;
}
static void kClav(DC& c) {          // funky clavinet: bright pulse + snappy filter, percussive/plucky
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvPulse; s.oscFreq = 261.63f;
    s.atk = 0.001f; s.dec = 0.5f; s.sustain = 0.22f; s.release = 0.08f;
    s.filterType = DC::LowPass; s.filterCutoff = 1600.0f; s.filterReso = 2.2f; s.filterEnvAmt = 0.6f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.14f;
    c.volume = 0.76f;
}
static void kSynthBrass(DC& c) {    // analog synth-brass ensemble: filter-swept detuned saws, swells + sustains
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSaw; s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.16f; s.oscUniCenter = true;
    s.atk = 0.06f; s.dec = 0.7f; s.sustain = 0.75f; s.release = 0.2f; // slow-ish brass swell
    s.filterType = DC::LowPass; s.filterCutoff = 900.0f; s.filterReso = 1.4f; s.filterEnvAmt = 0.55f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.1f;
    c.reverbSend = 0.12f; c.volume = 0.7f;
}
static void kChoir(DC& c) {         // vocal "aah" pad: formant vowel shape, slow swell, lush + a touch of vibrato
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 6;                                    // Vowel A (additive formant)
    s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.2f; s.oscUniCenter = true;
    s.atk = 0.25f; s.dec = 1.5f; s.sustain = 0.85f; s.release = 0.8f;
    s.lfoRate[1] = 5.0f; s.lfoAmt[1] = 0.015f;                       // subtle pitch vibrato = choir life
    c.reverbSend = 0.32f; c.volume = 0.64f;
}
static void kWarmPad(DC& c) {       // dark warm pad: mellow reed, very slow + very long release (vs Soft Pad's bright saws)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = 12;                                   // Reed (hollow odd harmonics)
    s.oscFreq = 261.63f;
    s.oscUnison = 3; s.oscDetune = 0.22f; s.oscUniCenter = true;
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
    s.lfoRate[2] = 5.0f; s.lfoAmt[2] = 0.4f;                        // the vibraphone tremolo (VOL)
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
    s.fxDriveType = DC::Tube; s.fxDrive = 0.12f;
    c.volume = 0.72f;
}
static void kSubBass(DC& c) {       // clean pure-sine sub bass, holds while a key is down (vs the fuller Keys Bass)
    auto& s = mkSlot(c, DC::SrcOsc);
    s.oscShape = s.oscShapeB = DC::WvSine; s.oscFreq = 55.0f;
    s.atk = 0.004f; s.dec = 0.6f; s.sustain = 0.85f; s.release = 0.1f;
    s.fxDriveType = DC::Tube; s.fxDrive = 0.1f;                      // a little warmth so it reads on small speakers
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
    s.fxDriveType = DC::Tube; s.fxDrive = 0.12f;
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
    s.lfoRate[1] = 5.0f; s.lfoAmt[1] = 0.015f;                      // subtle vibrato = choir life
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

static const struct { const char* name; Builder build; const char* cat; } kMixes[] = {
    // Categories are by SOUND TYPE (not synthesis source).
    { "FM Kick",     mFMKick,    "Kicks" },
    { "Sub Bass",    mSubBass,    "Bass" },           { "Noise Snare", mNoiseSnare,"Snares & Claps" },
    { "Clap",        mClap,       "Snares & Claps" }, { "Closed Hat",  mClosedHat, "Hats & Cymbals" },
    { "Open Hat",    mOpenHat,    "Hats & Cymbals" }, { "FM Tom",      mFMTom,     "Toms" },

    { "Rimshot",     mRimshot,   "Percussion" },
    { "Woodblock",   mWoodblock,  "Percussion" },     { "Zap",         mZap,       "FX & Synth" },
    { "808 Bass",    m808Bass,    "Bass" },           
               { "Shaker",      mShaker,    "Percussion" },
    { "Crash",       mCrash,      "Hats & Cymbals" }, 
    { "Saw Bass",    mSawBass,    "Bass" },           { "Glass Bell",  mGlassBell, "Bells & Mallets" },
    { "Sub Drop",    mSubDrop,    "FX & Synth" },     { "Noise Sweep", mNoiseSweep,"FX & Synth" },

    { "Pluck",       mPluck,      "Plucks & Strings" },
    
               { "Wood Clave",  mWoodClave, "Percussion" },
     
    // Classic drum machines
    { "808 Kick",     m808Kick,      "Kicks" },          { "808 Snare",    m808Snare,     "Snares & Claps" },
    { "808 Clap",     m808Clap,      "Snares & Claps" }, { "808 CH",       m808ClosedHat, "Hats & Cymbals" },
    { "808 OH",       m808OpenHat,   "Hats & Cymbals" }, { "808 Cowbell",  m808Cowbell,   "Percussion" },
    { "808 Low Tom",  m808LowTom,    "Toms" },           { "808 Mid Tom",  m808MidTom,    "Toms" },
    { "808 Hi Tom",   m808HiTom,     "Toms" },           { "808 Rimshot",  m808Rimshot,   "Percussion" },
    { "808 Conga",    m808Conga,     "Percussion" },     { "808 Maracas",  m808Maracas,   "Percussion" },
    { "808 Clave",    m808Clave,     "Percussion" },
    { "909 Kick",     m909Kick,      "Kicks" },          { "909 Snare",    m909Snare,     "Snares & Claps" },
    { "909 Clap",     m909Clap,      "Snares & Claps" }, { "909 CH",       m909ClosedHat, "Hats & Cymbals" },
    { "909 OH",       m909OpenHat,   "Hats & Cymbals" }, { "909 Ride",     m909Ride,      "Hats & Cymbals" },
    { "909 Crash",    m909Crash,     "Hats & Cymbals" }, { "909 Low Tom",  m909LowTom,    "Toms" },
    { "909 Mid Tom",  m909MidTom,    "Toms" },           { "909 Hi Tom",   m909HiTom,     "Toms" },
    { "909 Rimshot",  m909Rim,       "Percussion" },
    { "606 Snare",    m606Snare,     "Snares & Claps" },
    { "606 CH",       m606ClosedHat, "Hats & Cymbals" },

    // Extra mixes (round 8)
    { "Reese Bass",   mReeseBass,   "Bass" },           
    { "Square Bass",  mSquareBass,  "Bass" },           { "FM Bass",      mFMBass,      "Bass" },
    { "Growl Bass",   mGrowlBass,   "Bass" },           { "Acid Bass",    mAcidBass,    "Bass" },
    { "Punch Kick",   mPunchKick,   "Kicks" },          { "Dist Kick",    mDistKick,    "Kicks" },
    { "Trap Snare",   mTrapSnare,   "Snares & Claps" },
    { "Mod Snare",    mModSnare,    "Snares & Claps" },
    { "Metal Hat",    mMetalHat,    "Hats & Cymbals" }, { "Tight Hat",    mTightHat,    "Hats & Cymbals" },
    { "Sizzle",       mSizzle,      "Hats & Cymbals" }, 
    { "Roto Tom",     mRotoTom,     "Toms" },           { "Tabla",        mTabla,       "Percussion" },
    { "Log Drum",     mLogDrum,     "Percussion" },     { "Bongo",        mBongo,       "Percussion" },
    { "Vibraphone",   mVibraphone,  "Bells & Mallets" },
    { "Harp",         mHarp,        "Plucks & Strings" },{ "Pluck Synth", mPluckSynth,  "Plucks & Strings" },
     { "Riser",       mRiser,       "FX & Synth" },
    { "Wind",         mWind,        "FX & Synth" },      { "Vinyl",       mVinyl,       "FX & Synth" },
    { "Stab",         mStab,        "FX & Synth" },       { "Big Riser",   mBigRiser,    "FX & Synth" },
    { "Vox",          mVox,         "FX & Synth" }, { "Talkbox",      mTalkbox,     "FX & Synth" },
    { "Whistle",      mWhistle,     "FX & Synth" },
    // ---- Filter-envelope sounds, filed by sound TYPE ----

    { "Filter Bass",  eFilterBass, "Bass" },            { "Filter Pluck", eFilterPluck,"Plucks & Strings" },
    { "Filter Zap",   eFilterZap,  "FX & Synth" },
    // ---- BASS BANK (v1.1.0): two-osc / osc+sub, per-slot filter env + drive ----
    { "Station Bass", bStationBass, "Bass" },           { "Ladder Bass",  bLadderBass, "Bass" },
    { "Rubber Bass",  bRubberBass,  "Bass" },           { "Neuro Bass",   bNeuroBass,  "Bass" },
    { "Hoover Bass",  bHooverBass,  "Bass" },           { "Reed Bass",    bReedBass,   "Bass" },
    // ---- LFO showcases ----
    { "Wobble Bass",  bWobbleBass,  "Bass" },
    { "Siren",        eSiren,       "FX & Synth" },     { "Chopper",      eChopper,    "FX & Synth" },
    // ---- KEYS bank (v1.2.0): sustain/release live on the on-screen keyboard ----
    { "Keys Bass",    kKeysBass,    "Keys" },           { "E-Piano",      kEPiano,     "Keys" },
    { "Soft Pad",     kSoftPad,     "Keys" },           { "Organ",        kOrgan,      "Keys" },
    { "Square Lead",  kSquareLead,  "Keys" },           { "String Keys",  kStringKeys, "Keys" },
    // ---- KEYS bank additions (v1.2.1): 12 more, all distinct ----
    { "Grand Piano",  kGrandPiano,  "Keys" },           { "Wurli",        kWurli,      "Keys" },
    { "Clavinet",     kClav,        "Keys" },           { "Synth Brass",  kSynthBrass, "Keys" },
    { "Choir Aah",    kChoir,       "Keys" },           { "Warm Pad",     kWarmPad,    "Keys" },
    { "Glass Bells",  kGlassBells,  "Keys" },           { "Vibraphone",   kVibes,      "Keys" },
    { "Marimba Keys", kMarimbaKeys, "Keys" },           { "Saw Lead",     kSawLead,    "Keys" },
    { "Sub Bass",     kSubBass,     "Keys" },           { "Synth Pluck",  kSynthPluck, "Keys" },
    // ---- KEYS chord/scale sounds (v1.2.3): both slots, a slot in chord/scale; Power/Octave use slot-2 pitch ----
    { "Power Keys",   kPowerKeys,   "Keys" },           { "Octave Bells", kOctaveBells, "Keys" },
    { "Dorian Pad",   kDorianPad,   "Keys" },           { "Major Choir",  kMajorChoir,  "Keys" },
    { "Minor Rhodes", kMinorRhodes, "Keys" },
    // ---- MODAL engine (struck resonant bodies). Names carry no "(Modal)" - the tag adds it. ----
    { "Mod Marimba",   moMarimba,    "Modal" }, { "Mod Tubular Bell", moTubular,   "Modal" },
    { "Mod Glass",     moGlass,      "Modal" }, { "Mod Tom",          moTomDrum,   "Modal" },
    { "Mod Metal Plate", moMetalPlate, "Modal" }, { "Mod Wood Block", moWoodBlock, "Modal" },
    { "Mod Kalimba",   moKalimba,    "Modal" }, { "Mod Cowbell",      moCowbell,   "Modal" },
    { "Mod Tuned Bell", moBellTuned, "Modal" }, { "Mod Gong",         moGong,      "Modal" },
    // ---- New plucked strings + mallets (Physical) ----
    { "Nylon Guitar", mNylonGuitar, "Plucks & Strings" }, { "Koto",     mKoto,      "Plucks & Strings" },
    { "Pizzicato",    mPizzicato,   "Plucks & Strings" }, { "Banjo",    mBanjo,     "Plucks & Strings" },
    { "Sitar",        mSitar,       "Plucks & Strings" },
    { "Glockenspiel", mGlockenspiel,"Bells & Mallets" },  { "Celesta",  mCelesta,   "Bells & Mallets" },
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
    // Oscillator sounds are tagged by what they USE: any slot with the FM section engaged
    // (fmDepth > 0) = "Analog+FM", pure = "Analog" (user: sounds that don't touch FM shouldn't
    // say "+FM"). Two OSC slots (e.g. lead + sub) are still one engine family, NOT "Hybrid".
    static const char* eng[] = { "Sample", "Noise", "Analog", "FM", "Karplus-Strong", "Synth", "Wave", "Modal" };
    int firstEng = -1; bool sameEng = true, anyEng = false, allOsc = true, anyFM = false;
    for (auto& sl : tmp.slots)
        if (sl.engine >= 0 && sl.weight > 0.001f)
        {
            anyEng = true;
            if (sl.engine != DC::SrcOsc) allOsc = false;
            else if (sl.fmDepth > 0.001f) anyFM = true;
            if (firstEng < 0) firstEng = sl.engine; else if (sl.engine != firstEng) sameEng = false;
        }
    if (anyEng) {
        if (allOsc) return anyFM ? "Analog+FM" : "Analog";
        if (sameEng && firstEng >= 0 && firstEng < (int) (sizeof(eng) / sizeof(eng[0]))) return eng[firstEng];
        return "Hybrid";
    }
    // Legacy per-engine authoring (factory drum sounds): read the srcOn[] flags. Legacy "FM"
    // sounds run on the merged Analog+FM engine after conversion, so tag them that way.
    static const char* names[DC::NUM_SOURCES] = { "Sample", "Noise", "Analog", "Analog+FM", "Karplus-Strong" };
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
    // Slot-authored KS sounds (e.g. String Keys = Physical) never hit buildSlotsFromLegacy, so
    // allocate the lazy KS delay lines here too - else the audio thread gates them to SILENCE
    // (ksReady == false) until a later prepareToPlay happens to allocate them.
    ch.ensureKsBuffers();
}

void applyMix(DC& ch, int index)
{
    if (index < 0 || index >= kNumMixes) return;
    kMixes[index].build(ch);
    // Keyboard-friendly sounds ship with the TRANSPOSE LOCK on (Freq faders disabled), so nothing can
    // sneakily shift their pitch reference. Category-based: the whole Keys bank. (clearSound reset it.)
    if (juce::String(kMixes[index].cat) == "Keys")
        ch.drawMode = true;   // KEYS sounds open in PIANO ROLL by default (user; step data is kept underneath)
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
    buildChP(s, 1, 1, m909Clap,   16, { 4, 12 });   // (Clap Snare removed - 0.995 twin)
    buildChP(s, 1, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildChP(s, 1, 3, mReeseBass,  16, { 0,3,6,8,11,14 });
    buildChP(s, 1, 4, mOpenHat,    16, { 2,6,10,14 });
    s.patterns[1].playMode = Sequencer::LoopForever;
    s.current().master.reverbWet = 0.18f;
}

using PresetFn = void (*)(Sequencer&);
static const struct { const char* name; PresetFn build; } kPresets[] = {
    { "Riser + Drop",       pRiserDrop },
    { "Ode to Joy (Beethoven)", pOdeToJoy },
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
            ch.duckBy = -1; ch.duckAmt = 0.5f;   // sidechain duck is routing-like -> preset-level too
            ch.keysSlot2Down = 0;   // KEYS slot-2 transpose (channel-wide) is preset-level too
            ch.humanizeAmt = 0.0f; ch.strumAmt = 0.0f; ch.keysMinVel = 0.0f; ch.keysMaxVel = 1.0f; ch.keysGlide = 0.0f;   // HUMANIZE / STRUM / vel range / GLIDE reset
            ch.mergeWith = -1; ch.keysSplitW1 = 60; ch.keysSplitW2 = 12;   // MERGE&SPLIT reset (channel-wide)
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
