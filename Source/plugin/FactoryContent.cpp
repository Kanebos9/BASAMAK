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
    for (int i = 0; i < DC::NUM_SOURCES; ++i) { c.srcOn[i] = false; c.srcWeight[i] = 0.0f; }
    for (auto& s : c.slots) s = DC::Slot();   // empty all slots (engine = -1)
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
    c.markDspDirty();
}

static void setSteps(DC& c, int n, std::initializer_list<int> on)
{
    c.numSteps = n;
    for (int i = 0; i < DC::MAX_STEPS; ++i)
    { c.steps[i] = false; c.stepVel[i] = 1.0f; c.stepPitch[i] = 0.0f; c.stepProb[i] = 1.0f; c.stepRoll[i] = 1; c.stepCondLen[i] = 1; c.stepCondMask[i] = 0; }
    for (int s : on) if (s >= 0 && s < n) c.steps[s] = true;
}

//==============================================================================
// Sound builders (Osc / Noise / FM only).  Each starts from a clean channel.
//==============================================================================
static void mSineKick(DC& c)   // deep synth kick with a pitch drop
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 50.0f;
    c.layerSinePEnvAmt = 28.0f; c.layerSinePEnvTime = 0.05f;
    c.srcDec[DC::SrcOsc] = 0.32f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.22f;
    c.volume = 0.92f;
}
static void mFMKick(DC& c)     // tight 808-style sub kick
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -24.0f; c.fmSpread = 0.0f; c.fmDepth = 0.35f;
    c.srcDec[DC::SrcFM] = 0.30f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.20f;
    c.volume = 0.92f;
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
static void mNoiseSnare(DC& c) // noise body + tonal thwack
{
    clearSound(c);
    // Noise (act[0]) + Osc (act[1]). Pad dot biases the blend toward the noise.
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.62f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.38f;
    c.padX = 0.41f; c.padY = 0.5f;  // t = 0.38 -> noise 0.62 / osc 0.38
    c.noiseType = 3; c.layerNoiseCenter = 1900.0f; c.layerNoiseWidth = 0.22f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 185.0f;
    c.srcDec[DC::SrcNoise] = 0.13f; c.srcDec[DC::SrcOsc] = 0.16f;
    c.reverbSend = 0.12f; c.volume = 0.82f;
}
static void mClap(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 1300.0f; c.layerNoiseWidth = 0.28f;
    c.srcAtk[DC::SrcNoise] = 0.004f; c.srcDec[DC::SrcNoise] = 0.12f;
    c.reverbSend = 0.18f; c.volume = 0.80f;
}
static void mClosedHat(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 9000.0f; c.layerNoiseWidth = 0.15f;
    c.srcDec[DC::SrcNoise] = 0.04f; c.volume = 0.62f;
}
static void mOpenHat(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 8200.0f; c.layerNoiseWidth = 0.13f;
    c.srcDec[DC::SrcNoise] = 0.34f; c.volume = 0.6f;
}
static void mFMTom(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -7.0f; c.fmSpread = 0.08f; c.fmDepth = 0.30f;
    c.srcDec[DC::SrcFM] = 0.34f; c.volume = 0.82f;
}
static void mSynthTom(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 130.0f;
    c.layerSinePEnvAmt = 10.0f; c.layerSinePEnvTime = 0.08f;
    c.srcDec[DC::SrcOsc] = 0.30f; c.volume = 0.82f;
}
static void mCowbell(DC& c)    // clangy two-tone FM
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 12.0f; c.fmSpread = 0.42f; c.fmDepth = 0.45f;
    c.srcDec[DC::SrcFM] = 0.22f; c.volume = 0.72f;
}
static void mFMBell(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 19.0f; c.fmSpread = 0.60f; c.fmDepth = 0.60f;
    c.srcDec[DC::SrcFM] = 0.95f;
    c.reverbSend = 0.30f; c.volume = 0.65f;
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
static void mDeepTom(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 90.0f;
    c.layerSinePEnvAmt = 12.0f; c.layerSinePEnvTime = 0.12f;
    c.srcDec[DC::SrcOsc] = 0.45f; c.volume = 0.85f;
}
static void mHiTom(DC& c)
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 180.0f;
    c.layerSinePEnvAmt = 10.0f; c.layerSinePEnvTime = 0.08f;
    c.srcDec[DC::SrcOsc] = 0.30f; c.volume = 0.82f;
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
static void mLaser(DC& c)      // fast descending resonant zap
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 320.0f;
    c.layerSinePEnvAmt = 30.0f; c.layerSinePEnvTime = 0.18f;
    c.srcDec[DC::SrcOsc] = 0.25f;
    c.filterType = DC::LowPass; c.filterCutoff = 5000.0f; c.filterReso = 3.0f; c.filterEnvAmt = 0.4f;
    c.volume = 0.65f;
}

// ---- Newer sounds: pitch-envelope showcases + Fusion (interaction) hybrids ----
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
static void mFusionKick(DC& c)  // Hybrid: sine body + FM, fused for harmonic bite
{
    clearSound(c);
    c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 0.68f;
    c.srcOn[DC::SrcFM]  = true; c.srcWeight[DC::SrcFM]  = 0.32f;
    c.padX = 0.14f + 0.32f * 0.72f; c.padY = 0.5f;     // Osc(1st)/FM(2nd) slider, t = FM share
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 52.0f;
    c.layerSinePEnvAmt = 22.0f; c.layerSinePEnvTime = 0.05f; c.srcDec[DC::SrcOsc] = 0.34f;
    c.fmPitch = -12.0f; c.fmSpread = 0.12f; c.fmDepth = 0.4f; c.srcDec[DC::SrcFM] = 0.18f;
    c.punch = 0.3f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.18f; c.volume = 0.9f;
}
static void mMetalTick(DC& c)   // Hybrid: noise x FM ring-mod -> metallic tick
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.45f;
    c.srcOn[DC::SrcFM]    = true; c.srcWeight[DC::SrcFM]    = 0.55f;
    c.padX = 0.14f + 0.55f * 0.72f; c.padY = 0.5f;     // Noise(1st)/FM(2nd) slider
    c.noiseType = 0; c.layerNoiseCenter = 7000.0f; c.layerNoiseWidth = 0.25f; c.srcDec[DC::SrcNoise] = 0.08f;
    c.fmPitch = 19.0f; c.fmSpread = 0.55f; c.fmDepth = 0.5f; c.srcDec[DC::SrcFM] = 0.10f;
    c.spread = 0.3f; c.volume = 0.6f;
}
static void mRingSnare(DC& c)   // Hybrid: noise body x tonal osc, fused
{
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.58f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.42f;
    c.padX = 0.14f + 0.42f * 0.72f; c.padY = 0.5f;     // Noise(1st)/Osc(2nd) slider
    c.noiseType = 0; c.layerNoiseCenter = 1800.0f; c.layerNoiseWidth = 0.2f; c.srcDec[DC::SrcNoise] = 0.14f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 190.0f; c.srcDec[DC::SrcOsc] = 0.16f;
    c.bloom = 0.4f;   // noise attack blooms into the tonal body
    c.reverbSend = 0.12f; c.volume = 0.8f;
}

//==============================================================================
// Classic drum-machine kit (TR-808 / TR-909 / TR-606) recreated with the synth
// engine. Category "Drum Machines".
//==============================================================================
// -- TR-808 --
static void m808Kick(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 48.0f;
    c.layerSinePEnvAmt = 16.0f; c.layerSinePEnvTime = 0.06f;
    c.srcDec[DC::SrcOsc] = 1.10f; c.driveType = DC::SoftClip; c.driveAmount = 0.14f; c.volume = 0.96f;
}
static void m808Snare(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.55f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.45f;
    c.padX = 0.14f + 0.45f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 1800.0f; c.layerNoiseWidth = 0.18f; c.srcDec[DC::SrcNoise] = 0.12f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 190.0f; c.srcDec[DC::SrcOsc] = 0.13f;
    c.reverbSend = 0.10f; c.volume = 0.82f;
}
static void m808Clap(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 1200.0f; c.layerNoiseWidth = 0.30f;
    c.srcAtk[DC::SrcNoise] = 0.002f; c.srcDec[DC::SrcNoise] = 0.14f; c.reverbSend = 0.22f; c.volume = 0.80f;
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
static void m909Kick(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.85f;
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.15f;
    c.padX = 0.14f + 0.85f * 0.72f; c.padY = 0.5f;        // Noise(1st)/Osc(2nd) slider
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 55.0f; c.layerSinePEnvAmt = 22.0f; c.layerSinePEnvTime = 0.04f;
    c.srcDec[DC::SrcOsc] = 0.40f;
    c.noiseType = 0; c.layerNoiseCenter = 3000.0f; c.srcDec[DC::SrcNoise] = 0.02f;
    c.driveType = DC::SoftClip; c.driveAmount = 0.20f; c.volume = 0.95f;
}
static void m909Snare(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.70f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.30f;
    c.padX = 0.14f + 0.30f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 2200.0f; c.layerNoiseWidth = 0.14f; c.srcDec[DC::SrcNoise] = 0.18f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 220.0f; c.srcDec[DC::SrcOsc] = 0.10f;
    c.reverbSend = 0.12f; c.volume = 0.82f;
}
static void m909Clap(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 1400.0f; c.layerNoiseWidth = 0.26f;
    c.srcAtk[DC::SrcNoise] = 0.002f; c.srcDec[DC::SrcNoise] = 0.16f; c.reverbSend = 0.25f; c.volume = 0.8f;
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
static void m606Kick(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 60.0f; c.layerSinePEnvAmt = 14.0f; c.layerSinePEnvTime = 0.03f;
    c.srcDec[DC::SrcOsc] = 0.26f; c.driveType = DC::SoftClip; c.driveAmount = 0.18f; c.volume = 0.92f;
}
static void m606Snare(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.6f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.4f;
    c.padX = 0.14f + 0.4f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 2600.0f; c.layerNoiseWidth = 0.12f; c.srcDec[DC::SrcNoise] = 0.10f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 210.0f; c.srcDec[DC::SrcOsc] = 0.08f; c.volume = 0.8f;
}
static void m606ClosedHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 11000.0f; c.layerNoiseWidth = 0.08f; c.srcDec[DC::SrcNoise] = 0.03f; c.volume = 0.58f;
}
static void m606OpenHat(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 10000.0f; c.layerNoiseWidth = 0.07f; c.srcDec[DC::SrcNoise] = 0.26f; c.volume = 0.56f;
}
static void m606Cymbal(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.55f;
    c.srcOn[DC::SrcFM]    = true; c.srcWeight[DC::SrcFM]    = 0.45f;
    c.padX = 0.14f + 0.45f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 8000.0f; c.layerNoiseWidth = 0.05f; c.srcDec[DC::SrcNoise] = 0.8f;
    c.fmPitch = 24.0f; c.fmSpread = 0.7f; c.fmDepth = 0.5f; c.srcDec[DC::SrcFM] = 0.8f;
    c.reverbSend = 0.2f; c.volume = 0.55f;
}
// -- Physical-modelled percussion (category "Physical") --
static void mKalimba(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 440.0f; c.physTone = 0.7f; c.physMaterial = 1.0f; // Steel
    c.srcDec[DC::SrcPhys] = 0.55f; c.volume = 0.85f;
}
static void mSteelTom(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 120.0f; c.physTone = 0.55f; c.physMaterial = 5.0f; // Membrane
    c.physPitchEnvAmt = 6.0f; c.physPitchEnvTime = 0.08f;
    c.srcDec[DC::SrcPhys] = 0.6f; c.volume = 0.85f;
}
static void mWoodClave(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 800.0f; c.physTone = 0.6f; c.physMaterial = 2.0f; // Wood
    c.srcDec[DC::SrcPhys] = 0.25f; c.volume = 0.8f;
}
static void mGong(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 180.0f; c.physTone = 0.75f; c.physMaterial = 4.0f; // Metal
    c.srcDec[DC::SrcPhys] = 2.4f; c.reverbSend = 0.3f; c.volume = 0.78f;
}
static void mBellPluck(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 520.0f; c.physTone = 0.8f; c.physMaterial = 3.0f; // Glass
    c.srcDec[DC::SrcPhys] = 1.4f; c.reverbSend = 0.18f; c.volume = 0.78f;
}

static void mPluck(DC& c)       // Physical: bright plucked string
{
    clearSound(c);
    c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 196.0f; c.physTone = 0.7f; c.physMaterial = 0.0f;
    c.srcDec[DC::SrcPhys] = 0.9f; c.volume = 0.85f;
}
static void mMarimba(DC& c)     // Physical: soft mallet / wooden bar
{
    clearSound(c);
    c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 262.0f; c.physTone = 0.45f; c.physMaterial = 2.0f; // Wood
    c.srcDec[DC::SrcPhys] = 0.5f; c.volume = 0.85f;
}
static void mMetalDrum(DC& c)   // Physical: inharmonic metal / steel-drum bell
{
    clearSound(c);
    c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 330.0f; c.physTone = 0.8f; c.physMaterial = 4.0f; // Metal
    c.srcDec[DC::SrcPhys] = 1.6f; c.reverbSend = 0.15f; c.volume = 0.8f;
}

//==============================================================================
// Extra mixes (showcasing Unison/Detune, FM Sub & Feedback, Physical Position,
// long attacks, and the blend FX). No overlap with the sounds above.
//==============================================================================
// -- Bass --
static void mReeseBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 55.0f; c.oscUnison = 5; c.oscDetune = 0.55f;
    c.srcAtk[DC::SrcOsc] = 0.004f; c.srcDec[DC::SrcOsc] = 0.9f;
    c.filterType = DC::LowPass; c.filterCutoff = 1100.0f; c.filterReso = 1.3f; c.volume = 0.8f;
}
static void mSuperSaw(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSaw; c.layerSineFreq = 220.0f; c.oscUnison = 7; c.oscDetune = 0.7f;
    c.srcAtk[DC::SrcOsc] = 0.005f; c.srcDec[DC::SrcOsc] = 0.5f; c.volume = 0.7f;
}
static void mSquareBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSquare; c.layerSineFreq = 50.0f; c.srcDec[DC::SrcOsc] = 0.6f;
    c.filterType = DC::LowPass; c.filterCutoff = 900.0f; c.filterReso = 0.9f; c.volume = 0.82f;
}
static void mFMBass(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -24.0f; c.fmSpread = 0.1f; c.fmDepth = 0.5f; c.fmSub = 0.6f;
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
static void mPunchKick(DC& c) {
    clearSound(c); c.srcOn[DC::SrcOsc] = true; c.srcWeight[DC::SrcOsc] = 1.0f;
    c.layerOscShape = DC::OscSine; c.layerSineFreq = 52.0f; c.layerSinePEnvAmt = 20.0f; c.layerSinePEnvTime = 0.04f;
    c.srcDec[DC::SrcOsc] = 0.5f; c.punch = 0.5f; c.driveType = DC::SoftClip; c.driveAmount = 0.15f; c.volume = 0.95f;
}
static void mDistKick(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = -20.0f; c.fmSpread = 0.15f; c.fmDepth = 0.45f; c.fmSub = 0.4f;
    c.srcDec[DC::SrcFM] = 0.35f; c.driveType = DC::Fuzz; c.driveAmount = 0.3f; c.volume = 0.9f;
}
// -- Snares & Claps --
static void mTrapSnare(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.65f;
    c.srcOn[DC::SrcOsc]   = true; c.srcWeight[DC::SrcOsc]   = 0.35f;
    c.padX = 0.14f + 0.35f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 2800.0f; c.layerNoiseWidth = 0.12f; c.srcDec[DC::SrcNoise] = 0.14f;
    c.layerOscShape = DC::OscTriangle; c.layerSineFreq = 240.0f; c.srcDec[DC::SrcOsc] = 0.1f;
    c.bloom = 0.4f; c.reverbSend = 0.1f; c.volume = 0.82f;
}
static void mClapSnare(DC& c) {
    clearSound(c); c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 1.0f;
    c.noiseType = 0; c.layerNoiseCenter = 1600.0f; c.layerNoiseWidth = 0.2f;
    c.srcAtk[DC::SrcNoise] = 0.002f; c.srcDec[DC::SrcNoise] = 0.16f; c.spread = 0.4f; c.reverbSend = 0.2f; c.volume = 0.8f;
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
static void mSplash(DC& c) {
    clearSound(c);
    c.srcOn[DC::SrcNoise] = true; c.srcWeight[DC::SrcNoise] = 0.5f;
    c.srcOn[DC::SrcFM]    = true; c.srcWeight[DC::SrcFM]    = 0.5f;
    c.padX = 0.14f + 0.5f * 0.72f; c.padY = 0.5f;
    c.noiseType = 0; c.layerNoiseCenter = 9000.0f; c.layerNoiseWidth = 0.05f; c.srcDec[DC::SrcNoise] = 0.5f;
    c.fmPitch = 28.0f; c.fmSpread = 0.8f; c.fmDepth = 0.5f; c.srcDec[DC::SrcFM] = 0.5f;
    c.reverbSend = 0.25f; c.volume = 0.55f;
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
static void mMusicBox(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 880.0f; c.physTone = 0.85f; c.physMaterial = 3.0f; c.srcDec[DC::SrcPhys] = 1.6f; c.reverbSend = 0.2f; c.volume = 0.78f;
}
static void mVibraphone(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 440.0f; c.physTone = 0.7f; c.physMaterial = 4.0f; c.srcDec[DC::SrcPhys] = 1.4f; c.reverbSend = 0.18f; c.volume = 0.78f;
}
static void mHarp(DC& c) {
    clearSound(c); c.srcOn[DC::SrcPhys] = true; c.srcWeight[DC::SrcPhys] = 1.0f;
    c.physFreq = 330.0f; c.physTone = 0.6f; c.physMaterial = 0.0f; c.physPosition = 0.4f; c.srcDec[DC::SrcPhys] = 0.9f; c.volume = 0.82f;
}
static void mTubularBell(DC& c) {
    clearSound(c); c.srcOn[DC::SrcFM] = true; c.srcWeight[DC::SrcFM] = 1.0f;
    c.fmPitch = 12.0f; c.fmSpread = 0.55f; c.fmDepth = 0.55f; c.srcDec[DC::SrcFM] = 1.8f; c.reverbSend = 0.3f; c.volume = 0.6f;
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
// authored directly as a SrcOsc slot. Shapes: 7=Vowel A, 8=Vowel E, 9=Vowel O, 10=Formant, 16=Voice.
static void mVox(DC& c) {          // vowel/vocal tone via the Vowel A wave shape + unison + a little vibrato
    clearSound(c);
    DC::Slot& s = c.slots[0]; s.engine = DC::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = 7;            // Vowel A (additive formant bank)
    s.oscFreq = 150.0f; s.oscUnison = 3; s.oscDetune = 0.2f;
    s.dec = 1.2f; s.vibrato = 0.3f;   // long decay (was sustain 0.5 + dec 0.45 -> now a real fade-out); wobble = vocal life
    c.volume = 0.72f;
}
static void mTalkbox(DC& c) {      // FM + the Formant wave shape = a buzzy "talking" vocal tone
    clearSound(c);
    DC::Slot& s = c.slots[0]; s.engine = DC::SrcOsc; s.weight = 1.0f;
    s.oscShape = s.oscShapeB = 10;           // Formant wave (additive formant bank)
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
// EVOLVING showcases. Author a single Analog/FM slot directly (applyMix keeps it).
//   - Wave A != Wave B  -> the carrier waveform morphs A->B over the note.
//   - Channel LowPass + high Env -> the cutoff is swept by the amp envelope (the
//     "highs die first" motion), tuned LOUD here (low cutoff, high reso) so it's obvious.
//==============================================================================
static DC::Slot& mkSlot(DC& c, int engine)   // clean channel + one slot at full weight
{
    clearSound(c);
    DC::Slot& s = c.slots[0];
    s.engine = engine; s.weight = 1.0f;
    return s;
}
// -- Wave A -> Wave B morph --
static void eSawFall(DC& c)    { auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = DC::OscSaw;    s.oscShapeB = DC::OscSine;     s.oscFreq = 110.0f; s.dec = 0.7f; c.volume = 0.8f; }
static void eSquareSoft(DC& c) { auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = DC::OscSquare; s.oscShapeB = DC::OscTriangle; s.oscFreq = 90.0f;  s.dec = 0.6f; c.volume = 0.8f; }
static void eRiseBuzz(DC& c)   { auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = DC::OscSine;   s.oscShapeB = DC::OscSaw;      s.oscFreq = 180.0f; s.dec = 0.5f; s.oscPEnvAmt = 12.0f; s.oscPEnvTime = 0.3f; c.volume = 0.78f; }
static void eFMMorph(DC& c)    { auto& s = mkSlot(c, DC::SrcFM);  s.oscShape = DC::OscSaw;    s.oscShapeB = DC::OscSine;     s.fmPitch = 7.0f; s.fmSpread = 0.3f; s.fmDepth = 0.5f; s.dec = 0.8f; c.volume = 0.72f; }
// -- Filter envelope (channel LowPass swept down by the amp env) --
static void eFilterBass(DC& c) { auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = s.oscShapeB = DC::OscSaw;    s.oscFreq = 55.0f;  s.dec = 0.8f;  c.filterType = DC::LowPass; c.filterCutoff = 350.0f; c.filterReso = 7.0f; c.filterEnvAmt = 0.9f; c.volume = 0.8f; }
static void eFilterPluck(DC& c){ auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = s.oscShapeB = DC::OscSaw;    s.oscFreq = 220.0f; s.dec = 0.45f; c.filterType = DC::LowPass; c.filterCutoff = 500.0f; c.filterReso = 8.0f; c.filterEnvAmt = 1.0f; c.volume = 0.75f; }
static void eFilterZap(DC& c)  { auto& s = mkSlot(c, DC::SrcOsc); s.oscShape = s.oscShapeB = DC::OscSquare; s.oscFreq = 330.0f; s.dec = 0.3f;  c.filterType = DC::LowPass; c.filterCutoff = 700.0f; c.filterReso = 8.0f; c.filterEnvAmt = 1.0f; c.volume = 0.72f; }

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
    { "Sine Kick",   mSineKick,   "Kicks" },          { "FM Kick",     mFMKick,    "Kicks" },
    { "Sub Bass",    mSubBass,    "Bass" },           { "Noise Snare", mNoiseSnare,"Snares & Claps" },
    { "Clap",        mClap,       "Snares & Claps" }, { "Closed Hat",  mClosedHat, "Hats & Cymbals" },
    { "Open Hat",    mOpenHat,    "Hats & Cymbals" }, { "FM Tom",      mFMTom,     "Toms" },
    { "Synth Tom",   mSynthTom,   "Toms" },           { "Cowbell",     mCowbell,   "Percussion" },
    { "FM Bell",     mFMBell,     "Bells & Mallets" },{ "Rimshot",     mRimshot,   "Percussion" },
    { "Woodblock",   mWoodblock,  "Percussion" },     { "Zap",         mZap,       "FX & Synth" },
    { "808 Bass",    m808Bass,    "Bass" },           { "Deep Tom",    mDeepTom,   "Toms" },
    { "Hi Tom",      mHiTom,      "Toms" },           { "Shaker",      mShaker,    "Percussion" },
    { "Crash",       mCrash,      "Hats & Cymbals" }, { "Laser",       mLaser,     "FX & Synth" },
    { "Saw Bass",    mSawBass,    "Bass" },           { "Glass Bell",  mGlassBell, "Bells & Mallets" },
    { "Sub Drop",    mSubDrop,    "FX & Synth" },     { "Noise Sweep", mNoiseSweep,"FX & Synth" },
    { "Fusion Kick", mFusionKick, "Kicks" },          { "Metal Tick",  mMetalTick, "Percussion" },
    { "Ring Snare",  mRingSnare,  "Snares & Claps" },
    { "Pluck",       mPluck,      "Plucks & Strings" },{ "Marimba",    mMarimba,   "Bells & Mallets" },
    { "Metal Drum",  mMetalDrum,  "Bells & Mallets" },{ "Kalimba",     mKalimba,   "Bells & Mallets" },
    { "Steel Tom",   mSteelTom,   "Toms" },           { "Wood Clave",  mWoodClave, "Percussion" },
    { "Gong",        mGong,       "Hats & Cymbals" }, { "Bell Pluck",  mBellPluck, "Bells & Mallets" },
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
    { "606 Kick",     m606Kick,      "Kicks" },          { "606 Snare",    m606Snare,     "Snares & Claps" },
    { "606 CH",       m606ClosedHat, "Hats & Cymbals" }, { "606 OH",       m606OpenHat,   "Hats & Cymbals" },
    { "606 Cymbal",   m606Cymbal,    "Hats & Cymbals" },
    // Extra mixes (round 8)
    { "Reese Bass",   mReeseBass,   "Bass" },           { "Super Saw",    mSuperSaw,    "Bass" },
    { "Square Bass",  mSquareBass,  "Bass" },           { "FM Bass",      mFMBass,      "Bass" },
    { "Growl Bass",   mGrowlBass,   "Bass" },           { "Acid Bass",    mAcidBass,    "Bass" },
    { "Punch Kick",   mPunchKick,   "Kicks" },          { "Dist Kick",    mDistKick,    "Kicks" },
    { "Trap Snare",   mTrapSnare,   "Snares & Claps" }, { "Clap Snare",   mClapSnare,   "Snares & Claps" },
    { "Metal Hat",    mMetalHat,    "Hats & Cymbals" }, { "Tight Hat",    mTightHat,    "Hats & Cymbals" },
    { "Sizzle",       mSizzle,      "Hats & Cymbals" }, { "Splash",       mSplash,      "Hats & Cymbals" },
    { "Roto Tom",     mRotoTom,     "Toms" },           { "Tabla",        mTabla,       "Percussion" },
    { "Log Drum",     mLogDrum,     "Percussion" },     { "Bongo",        mBongo,       "Percussion" },
    { "Music Box",    mMusicBox,    "Bells & Mallets" },{ "Vibraphone",   mVibraphone,  "Bells & Mallets" },
    { "Harp",         mHarp,        "Plucks & Strings" },{ "Pluck Synth", mPluckSynth,  "Plucks & Strings" },
    { "Tubular Bell", mTubularBell, "Bells & Mallets" }, { "Riser",       mRiser,       "FX & Synth" },
    { "Wind",         mWind,        "FX & Synth" },      { "Vinyl",       mVinyl,       "FX & Synth" },
    { "Stab",         mStab,        "FX & Synth" },       { "Big Riser",   mBigRiser,    "FX & Synth" },
    { "Vox",          mVox,         "FX & Synth" }, { "Talkbox",      mTalkbox,     "FX & Synth" },
    { "Whistle",      mWhistle,     "FX & Synth" },
    // ---- Wave-morph + filter-envelope sounds, filed by sound TYPE ----
    { "Saw Fall",     eSawFall,    "Bass" },            { "Square Soft",  eSquareSoft, "Bass" },
    { "Rise Buzz",    eRiseBuzz,   "FX & Synth" },      { "FM Morph",     eFMMorph,    "FX & Synth" },
    { "Filter Bass",  eFilterBass, "Bass" },            { "Filter Pluck", eFilterPluck,"Plucks & Strings" },
    { "Filter Zap",   eFilterZap,  "FX & Synth" },
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
    static const char* eng[] = { "Sample", "Noise", "Analog+FM", "FM", "Physical", "Synth", "Wave", "Modal" };
    int firstEng = -1; bool sameEng = true, anyEng = false;
    for (auto& sl : tmp.slots)
        if (sl.engine >= 0 && sl.weight > 0.001f)
        { anyEng = true; if (firstEng < 0) firstEng = sl.engine; else if (sl.engine != firstEng) sameEng = false; }
    if (anyEng) {
        if (sameEng && firstEng >= 0 && firstEng < (int) (sizeof(eng) / sizeof(eng[0]))) return eng[firstEng];
        return "Hybrid";
    }
    // Legacy per-engine authoring (factory drum sounds): read the srcOn[] flags.
    static const char* names[DC::NUM_SOURCES] = { "Sample", "Noise", "Analog", "FM", "Physical" };
    int count = 0, last = -1;
    for (int s = 0; s < DC::NUM_SOURCES; ++s)
        if (tmp.srcOn[s] && tmp.srcWeight[s] > 0.001f) { ++count; last = s; }
    if (count == 1) return names[last];
    if (count > 1) return "Hybrid";
    return "Synth";
}

void applyMix(DC& ch, int index)
{
    if (index < 0 || index >= kNumMixes) return;
    kMixes[index].build(ch);
    // Synth mixes author slots[] directly; legacy mixes leave them empty and are
    // converted from the per-engine fields. Only rebuild when nothing was authored.
    bool authored = false;
    for (auto& s : ch.slots) if (s.engine >= 0) { authored = true; break; }
    if (! authored) ch.buildSlotsFromLegacy();
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

static void buildCh(Sequencer& s, int i, Builder build, int n, std::initializer_list<int> on)
{
    auto& c = s.patterns[0].channels[i];
    build(c);
    c.mixName = nameForBuilder(build); c.mixModified = false;
    setSteps(c, n, on);
}

// Same as buildCh but targets a chosen pattern (for multi-pattern presets).
static void buildChP(Sequencer& s, int pat, int i, Builder build, int n, std::initializer_list<int> on)
{
    auto& c = s.patterns[pat].channels[i];
    build(c);
    c.mixName = nameForBuilder(build); c.mixModified = false;
    setSteps(c, n, on);
}

// Quiet, empty channel (so unused rows are silent but still playable).
static void emptyCh(Sequencer& s, int i)
{
    auto& c = s.patterns[0].channels[i];
    mClosedHat(c);
    c.mixName = nameForBuilder(mClosedHat); c.mixModified = false;
    setSteps(c, 16, {});
}

static void pHouse(Sequencer& s)
{
    s.standaloneBpm = 124.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mSineKick,  16, { 0,4,8,12 });
    buildCh(s, 1, mClap,      16, { 4,12 });
    buildCh(s, 2, mClosedHat, 16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mOpenHat,   16, { 2,6,10,14 });
    buildCh(s, 4, mSubBass,   16, { 0,3,6,8,11,14 });
    buildCh(s, 5, mCowbell,   16, {});
    emptyCh(s, 6); emptyCh(s, 7);
}
static void pBoomBap(Sequencer& s)
{
    s.standaloneBpm = 88.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mSineKick,   16, { 0,7,10 });
    buildCh(s, 1, mNoiseSnare, 16, { 4,12 });
    buildCh(s, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mOpenHat,    16, { 14 });
    buildCh(s, 4, mRimshot,    16, {});
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pTrap(Sequencer& s)
{
    s.standaloneBpm = 140.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mFMKick,    16, { 0,7,10 });
    buildCh(s, 1, mClap,      16, { 8 });
    // Fast hi-hats with a little roll, on a 32-step lane.
    buildCh(s, 2, mClosedHat, 32, { 0,2,4,6,8,10,12,14,16,18,20,21,22,24,26,28,29,30,31 });
    buildCh(s, 3, mSubBass,   16, { 0,7,10 });
    buildCh(s, 4, mCowbell,   16, {});
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pTechno(Sequencer& s)
{
    s.standaloneBpm = 130.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mSineKick,  16, { 0,4,8,12 });
    buildCh(s, 1, mClap,      16, { 4,12 });
    buildCh(s, 2, mClosedHat, 16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mOpenHat,   16, { 2,6,10,14 });
    buildCh(s, 4, mCowbell,   16, { 0,3,6,9,12,15 });
    buildCh(s, 5, mZap,       16, {});
    emptyCh(s, 6); emptyCh(s, 7);
}
static void pPopRock(Sequencer& s)
{
    s.standaloneBpm = 120.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mSineKick,   16, { 0,8,10 });
    buildCh(s, 1, mNoiseSnare, 16, { 4,12 });
    buildCh(s, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mOpenHat,    16, {});
    emptyCh(s, 4); emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pBreakbeat(Sequencer& s)
{
    s.standaloneBpm = 170.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mSineKick,   16, { 0,10 });
    buildCh(s, 1, mNoiseSnare, 16, { 4,12 });
    buildCh(s, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mOpenHat,    16, { 7,15 });
    buildCh(s, 4, mSubBass,    16, { 0,10 });
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}

static void pHalfTime(Sequencer& s)   // moody half-time, shows off the Fusion hybrids
{
    s.standaloneBpm = 140.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mFusionKick, 16, { 0, 10 });
    buildCh(s, 1, mRingSnare,  16, { 8 });
    buildCh(s, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mMetalTick,  16, { 3,7,11,15 });
    buildCh(s, 4, mSubDrop,    16, { 0,10 });
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pFootwork(Sequencer& s)   // fast, busy kick + saw bass
{
    s.standaloneBpm = 160.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mSineKick,  16, { 0,3,6,8,11 });
    buildCh(s, 1, mClap,      16, { 4,12 });
    buildCh(s, 2, mClosedHat, 16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mSawBass,   16, { 0,6,8,14 });
    buildCh(s, 4, mMetalTick, 16, { 2,5,9,13 });
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}

static void pDnB(Sequencer& s)        // drum & bass: rolling break + reese
{
    s.standaloneBpm = 174.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mSineKick,   16, { 0, 10 });
    buildCh(s, 1, mTrapSnare,  16, { 4, 12 });
    buildCh(s, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mReeseBass,  16, { 0,6,8,14 });
    buildCh(s, 4, mTightHat,   16, { 3,7,11,15 });
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pReggaeton(Sequencer& s)  // dembow
{
    s.standaloneBpm = 95.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mPunchKick,  16, { 0, 8 });
    buildCh(s, 1, mClapSnare,  16, { 3,6,10,13 });
    buildCh(s, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mSquareBass, 16, { 0, 8 });
    buildCh(s, 4, mBongo,      16, { 7, 15 });
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pGarage(Sequencer& s)     // UK garage shuffle
{
    s.standaloneBpm = 134.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    s.current().swing = 0.4f;
    buildCh(s, 0, mPunchKick,  16, { 0, 10 });
    buildCh(s, 1, mClapSnare,  16, { 4, 12 });
    buildCh(s, 2, mTightHat,   16, { 2,5,7,10,13,15 });
    buildCh(s, 3, mFMBass,     16, { 0,7,10 });
    emptyCh(s, 4); emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pMinimal(Sequencer& s)    // minimal/dub techno
{
    s.standaloneBpm = 126.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mPunchKick,  16, { 0,4,8,12 });
    buildCh(s, 1, mTightHat,   16, { 2,6,10,14 });
    buildCh(s, 2, mAcidBass,   16, { 3,6,11,14 });
    buildCh(s, 3, mWoodblock,  16, { 7 });
    buildCh(s, 4, mVinyl,      16, { 0, 8 });
    emptyCh(s, 5); emptyCh(s, 6); emptyCh(s, 7);
}
static void pAfrobeat(Sequencer& s)   // afrobeat / latin percussion
{
    s.standaloneBpm = 110.0f; s.timeSigNum = 4; s.timeSigDen = 4;
    buildCh(s, 0, mPunchKick,  16, { 0,6,10 });
    buildCh(s, 1, mClapSnare,  16, { 4, 12 });
    buildCh(s, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildCh(s, 3, mBongo,      16, { 2,5,9,13 });
    buildCh(s, 4, mLogDrum,    16, { 0,7,11 });
    buildCh(s, 5, mShaker,     16, { 1,3,5,7,9,11,13,15 });
    emptyCh(s, 6); emptyCh(s, 7);
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
    buildChP(s, 1, 1, mClapSnare,  16, { 4, 12 });
    buildChP(s, 1, 2, mClosedHat,  16, { 0,2,4,6,8,10,12,14 });
    buildChP(s, 1, 3, mReeseBass,  16, { 0,3,6,8,11,14 });
    buildChP(s, 1, 4, mOpenHat,    16, { 2,6,10,14 });
    s.patterns[1].playMode = Sequencer::LoopForever;
    s.current().master.reverbWet = 0.18f;
}

using PresetFn = void (*)(Sequencer&);
static const struct { const char* name; PresetFn build; } kPresets[] = {
    { "Riser + Drop",    pRiserDrop },
    { "House 4x4",       pHouse     },
    { "Boom Bap",        pBoomBap   },
    { "Trap",            pTrap      },
    { "Techno",          pTechno    },
    { "Pop / Rock",      pPopRock   },
    { "Breakbeat",       pBreakbeat },
    { "Half-Time",       pHalfTime  },
    { "Footwork",        pFootwork  },
    { "Drum & Bass",     pDnB       },
    { "Reggaeton",       pReggaeton },
    { "UK Garage",       pGarage    },
    { "Minimal",         pMinimal   },
    { "Afrobeat",        pAfrobeat  },
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
        P.master = Sequencer::MasterFX();          // default master FX + output
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            auto& ch = P.channels[c];
            clearSound(ch);
            ch.mute = false; ch.solo = false;
            ch.chokeGroup = 0; ch.outputBus = 0; ch.midiOut = false; ch.midiOutChannel = 1;   // routing/choke are preset-level -> reset them
            ch.mixName = {}; ch.mixModified = false;
            setSteps(ch, 8, {});
        }
    }
}

void applyPreset(Sequencer& seq, int index)
{
    if (index < 0 || index >= kNumPresets) return;
    resetAll(seq);                 // fresh slate - same result every time
    seq.currentPattern = 0;
    kPresets[index].build(seq);
    for (auto& pat : seq.patterns) for (auto& ch : pat.channels) ch.buildSlotsFromLegacy();
}

} // namespace Factory
