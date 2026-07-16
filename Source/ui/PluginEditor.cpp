#include "PluginEditor.h"
#include "FactoryMidiMaps.h"   // [2026-07-14 23:20] embedded "Launchpad + Launchkey" MIDI-learn map
#include "../plugin/FactoryContent.h"
#include "../plugin/UserPaths.h"
#include <cstring>

// ================================================================================================
// SYSTEM STATS [2026-07-14 01:50] - whole-machine CPU % (delta-sampled between UI polls) and this
// PROCESS's resident memory. Message-thread only (the editor timer polls ~2x/sec). In a DAW the
// process = the whole host, so the RAM figure is "BASAMAK + your DAW" - the tooltip says so.
// ================================================================================================
#if JUCE_MAC
 #include <mach/mach.h>
static float systemCpuPercent()
{
    static uint64_t lastUsed = 0, lastTotal = 0;
    host_cpu_load_info_data_t info; mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t) &info, &cnt) != KERN_SUCCESS) return -1.0f;
    uint64_t used = (uint64_t) info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM] + info.cpu_ticks[CPU_STATE_NICE];
    uint64_t total = used + info.cpu_ticks[CPU_STATE_IDLE];
    const uint64_t dU = used - lastUsed, dT = total - lastTotal;
    lastUsed = used; lastTotal = total;
    return dT > 0 ? 100.0f * (float) dU / (float) dT : -1.0f;
}
static float processRamMB()
{
    mach_task_basic_info info; mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &cnt) != KERN_SUCCESS) return -1.0f;
    return (float)(info.resident_size / (1024.0 * 1024.0));
}
#elif JUCE_WINDOWS
 // [2026-07-14 09:30] psapi.h needs windows.h FIRST (it uses BOOL/DWORD/WINAPI without defining
 // them) - the first Windows release build failed right here with hundreds of "unknown type"
 // errors. WIN32_LEAN_AND_MEAN + NOMINMAX keep windows.h from breaking the rest of this file
 // (its min/max macros would wreck std::/juce:: calls below).
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #pragma comment(lib, "psapi.lib")
static float systemCpuPercent()
{
    static ULONGLONG lastIdle = 0, lastTotal = 0;
    FILETIME idleF, kernF, userF;
    if (! GetSystemTimes(&idleF, &kernF, &userF)) return -1.0f;
    auto q = [](const FILETIME& f) { return ((ULONGLONG) f.dwHighDateTime << 32) | f.dwLowDateTime; };
    const ULONGLONG idle = q(idleF), total = q(kernF) + q(userF);   // kernel time INCLUDES idle
    const ULONGLONG dI = idle - lastIdle, dT = total - lastTotal;
    lastIdle = idle; lastTotal = total;
    return dT > 0 ? 100.0f * (float)(dT - dI) / (float) dT : -1.0f;
}
static float processRamMB()
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (! GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1.0f;
    return (float)(pmc.WorkingSetSize / (1024.0 * 1024.0));
}
#else   // Linux
static float systemCpuPercent()
{
    static long long lastIdle = 0, lastTotal = 0;
    juce::File f("/proc/stat");
    auto line = f.loadFileAsString().upToFirstOccurrenceOf("\n", false, false);
    juce::StringArray tok; tok.addTokens(line, " ", "");
    if (tok.size() < 5 || tok[0] != "cpu") return -1.0f;
    long long total = 0, idle = tok[4].getLargeIntValue();
    for (int i = 1; i < tok.size(); ++i) total += tok[i].getLargeIntValue();
    const long long dI = idle - lastIdle, dT = total - lastTotal;
    lastIdle = idle; lastTotal = total;
    return dT > 0 ? 100.0f * (float)(dT - dI) / (float) dT : -1.0f;
}
static float processRamMB()
{
    juce::StringArray tok; tok.addTokens(juce::File("/proc/self/statm").loadFileAsString(), " ", "");
    if (tok.size() < 2) return -1.0f;
    return (float)(tok[1].getLargeIntValue() * 4096ll / (1024.0 * 1024.0));   // resident pages (4 KB assumed)
}
#endif

// ================================================================================================
// FILE MAP [added 2026-07-13 22:20 - navigation aid; SEARCH for the quoted strings]. This file is
// the whole editor: component classes first, then the DrumSequencerEditor implementation.
//   1. Component classes (top ~half): "ADSRDisplay", "PitchEnvDisplay", "VoiceModDisplay",
//      "FrequencyDisplay" (filters), "LfoDisplay" (MOD visual + Mod Env tab), "ModFaderMatrix" +
//      "RoutePicker" (the 12-route matrix), "HarmonicEditor" (draw window), "WaveMorphDisplay",
//      "SoundPickerPanel" + "TipList"/"TipCombo" (dropdowns), "StepGridComponent" lives in its
//      OWN file (StepGridComponent.cpp shares PluginEditor.h)
//   2. "kModSrcName"            - mod source/target display names (append-only enums)
//   3. "setupComponents"        - every control's wiring + tooltips (one giant function)
//   4. "layoutContent"          - every control's geometry (DESIGN_W canvas)
//   5. "refreshDetailPanel"     - pushes channel state into the controls (change-only rules!)
//   6. "timerCallback"          - 60 Hz UI tick: meters, playhead, mod rings, watchdogs
//   7. "channelSoundHash"       - the modified-star hash (EVERY new sound field joins it)
//   8. "writeChannelMix" / "readChannelMix" - .basamaksound (Sound Bank) persistence
// ================================================================================================

// Design height grows with the visible channel-row count. Keep these magic numbers in sync with the
// layout constants GRID_TOP(84)/ROW_H(38) + the 24px gap + the 366px detail-panel block (see below).
// At 8 channels this returns 778 == DESIGN_H.
// The channel area shows at most 8 rows; more channels scroll (see channelBar). 44 = ROW_H (keep in sync).
static int contentHeightFor(int visCh, bool detail = true)
{
    const int rows = juce::jmin(visCh, detail ? 8 : 16);     // editor hidden -> room for all channels
    const int grid = 84 + rows * 44 + 24;                    // top bar + pattern row + channel grid
    return detail ? grid + 366 : grid + 22;                  // + the editing panel, or just the "Show Editor" strip
}

//==============================================================================
// Top-left brand logo (vector). Parsed once into logoDrawable + drawn scaled in paintContent.
static const char* kBasamakLogoSvg = R"LOGO(<svg xmlns="http://www.w3.org/2000/svg" viewBox="200 46 880 128"><g transform="translate(206.543,170.000) scale(0.17143,-0.17143)" fill="#F3F3F5"><path transform="translate(0.000,0)" d="M65 700H504L604 600V404L564 363L635 291V108L527 0H65ZM434 407 470 443V551L435 586H198V407ZM458 114 501 157V255L458 298H198V114Z"/><path transform="translate(730.000,0)" d="M261 700H385L641 0H503L446 155H200L143 0H5ZM416 267 323 533H321L231 267Z"/><path transform="translate(1436.000,0)" d="M50 108V208H184V149L217 116H424L458 150V266L425 299H160L52 407V592L160 700H476L584 592V491H450V551L417 584H219L186 551V448L219 415H484L592 307V110L482 0H158Z"/><path transform="translate(2133.000,0)" d="M261 700H385L641 0H503L446 155H200L143 0H5ZM416 267 323 533H321L231 267Z"/><path transform="translate(2839.000,0)" d="M65 700H191L399 248H401L610 700H736V0H605V426H603L442 98H358L198 426H196V0H65Z"/><path transform="translate(3700.000,0)" d="M261 700H385L641 0H503L446 155H200L143 0H5ZM416 267 323 533H321L231 267Z"/><path transform="translate(4406.000,0)" d="M65 700H201V407H307L476 700H629L424 351L646 0H490L306 291H201V0H65Z"/></g></svg>)LOGO";

//==============================================================================
// SlotEditor: a data-driven per-slot knob panel (engine-specific knobs on top,
// shared envelope below). Three of these make up the new SOUND BLEND row.
//==============================================================================
// Pitched-frequency controls have TWO read-out modes, toggled by CLICKING the value text
// (session-wide): Hz (default, free drag - the original behaviour) or NOTE ("A1"; dragging
// snaps to semitones, SHIFT = free, and the text turns green on an exact note).
static bool gFreqNotesMode = false;

static juce::String noteNameFor(double hz)
{
    const double midi  = 69.0 + 12.0 * std::log2(juce::jmax(1.0, hz) / 440.0);
    const int    n     = (int) std::lround(midi);
    const int    cents = (int) std::lround((midi - (double) n) * 100.0);
    static const char* nm[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    juce::String note = juce::String(nm[((n % 12) + 12) % 12]) + juce::String(n / 12 - 1);   // SCIENTIFIC pitch: C4 = middle C (MIDI 60)
    if (std::abs(cents) >= 10) note += (cents > 0 ? "+" : "") + juce::String(cents);
    return note;
}

// (The LFO Sync/Rate FADERS + their fader-mapping helpers were removed - tempo sync lives INSIDE the
//  LfoDisplay now: kLfoSyncCPB + the Sync button + the snapped wave drag, see the LfoDisplay section.)

static juce::String fmtSlot(const SlotParam& p, double v)
{
    if (p.choices.size() > 0)
        return p.choices[juce::jlimit(0, p.choices.size() - 1, (int) std::lround(v))];
    if (p.suffix == "fmr")   // FM Ratio: show the REAL modulator ratio (1..6x), not a %
        return juce::String(1.0 + v * 5.0, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + "x";
    if (p.isPct) return juce::String(juce::roundToInt(v * 100.0)) + "%";
    if (p.suffix == "st") return (v > 0 ? "+" : "") + juce::String(juce::roundToInt(v)) + "st";
    if (p.suffix == "Hz") return v >= 1000.0 ? juce::String(v / 1000.0, 1) + "k"
                                             : juce::String(juce::roundToInt(v)) + "";
    if (p.suffix == "nHz")   // PITCHED-source frequency: Hz by default; the NOTE ("A1") in note mode
        return gFreqNotesMode ? noteNameFor(v)
                              : (v >= 1000.0 ? juce::String(v / 1000.0, 1) + "k"
                                             : juce::String(juce::roundToInt(v)) + "");
    if (p.suffix == "x")  return juce::String(v, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + "x";
    if (p.suffix == "sl") return (v < 1.5) ? juce::String("Off") : juce::String((int) std::lround(v));  // slices (int)
    if (p.suffix == "ms") return v < 1.0 ? juce::String(juce::roundToInt(v * 1000.0)) + "ms"
                                         : juce::String(v, 2) + "s";
    return juce::String(v, 2) + p.suffix;
}

// NOTE: the old green "on an exact note" tint was REMOVED. Setting a slider's text colour makes
// juce::Slider REBUILD its internal text-box Label (colourChanged -> lookAndFeelChanged), which
// silently destroyed the MouseListener that drives the Hz<->note CLICK toggle - so after one tint
// the value text went deaf and could never switch back. No colour is ever set on these read-outs
// now (they stay the default), so the click listener lives forever and the toggle always works.

juce::Array<SlotParam> slotParamsFor(int engine)
{
    using S = DrumChannel::Slot;
    juce::Array<SlotParam> p;
    auto F = [](juce::String lab, double mn, double mx, float S::* f, juce::String suf = "", bool pct = false, juce::String tip = {}) {
        SlotParam sp; sp.label = lab; sp.min = mn; sp.max = mx; sp.suffix = suf; sp.isPct = pct; sp.tooltip = tip;
        sp.get = [f](const S& s) { return (double)(s.*f); };
        sp.set = [f](S& s, double v) { s.*f = (float) v; };
        return sp;
    };
    auto Ic = [](juce::String lab, double mn, double mx, int S::* f, juce::StringArray ch, juce::String tip = {}) {
        SlotParam sp; sp.label = lab; sp.min = mn; sp.max = mx; sp.choices = ch; sp.tooltip = tip;
        sp.get = [f](const S& s) { return (double)(s.*f); };
        sp.set = [f](S& s, double v) { s.*f = (int) std::lround(v); };
        return sp;
    };
    auto Fc = [](juce::String lab, double mn, double mx, float S::* f, juce::StringArray ch, juce::String tip = {}) {  // choice on a float field
        SlotParam sp; sp.label = lab; sp.min = mn; sp.max = mx; sp.choices = ch; sp.tooltip = tip;
        sp.get = [f](const S& s) { return (double)(s.*f); };
        sp.set = [f](S& s, double v) { s.*f = (float) std::lround(v); };
        return sp;
    };
    switch (engine)
    {
        case DrumChannel::SrcOsc:
            // "Analog + FM" engine. Sectioned layout (placeOsc): a single WAVE fader + the wave visual,
            // then Freq + Warp faders, then the FM row = 3 KNOBS [Amount(fmDepth), Ratio, Feedback].
            // Wave/Freq/Warp are FADERS (placeOsc draws them); the FM controls are knobs in the grid.
            // (The resonator was REMOVED from this engine - use the standalone Physical/Modal engines instead.)
            p.add(F ("Amount", 0, 1, &S::fmDepth, "", true,
                     "FM Amount: 0 = pure analog oscillator (no FM). Raise it to add FM harmonics / brightness; "
                     "the Ratio + Feedback knobs then shape that FM tone (the wave display shows it live). "
                     "The small Env switch makes this Amount follow the amp envelope (classic FM drums)."));
            { SlotParam r = F("Ratio", 0, 1, &S::fmSpread, "fmr", false,
                     "FM Ratio = the FM CHARACTER: the modulator's frequency relative to the carrier. SNAPS to the "
                     "harmonic ratios 1x-6x (bell-like/tuned); hold SHIFT while dragging for free in-between values "
                     "(metallic/clangy). Only audible when FM Amount is up.");
              r.snapRatio = true;   // integer ratios by default; Shift = free
              p.add(r); }
            p.add(F ("Feedback", 0, 1, &S::fmFeedback, "", true,
                     "FM Feedback: the modulator modulates itself, adding grit / harshness / noise to the tone. "
                     "Only audible when the FM (Depth) fader is up."));
            break;   // Unison/Detune/Pitch-env/Vibrato moved to the shared shape groups
        case DrumChannel::SrcNoise:
            p.add(Ic("Type", 0, 4, &S::noiseType, { "White","Pink","Brown","Grey","Purple" }));
            p.add(F ("Center", 100, 16000, &S::noiseCenter, "Hz"));
            p.add(F ("Width", 0, 1, &S::noiseWidth, "", true, "Band-pass width around Center (0 = full-band noise)."));
            p.add(F ("Reso", 0, 1, &S::noiseRes, "", true,
                     "Resonance: sharpens the band-pass into a ringing, pitched/whistling tone (great for tonal hats/zaps). "
                     "Also makes Drive + Crackle far more obvious, since it shapes the noise they sit on."));
            p.add(F ("Drive", 0, 1, &S::noiseDrive, "", true,
                     "Saturation - denser, grittier, more aggressive. Most audible once the noise is shaped (raise Reso, "
                     "or narrow Width) - on full-band white noise the effect is subtle."));
            p.add(F ("Crackle", 0, 1, &S::noiseCrackle, "", true,
                     "Granular dust / vinyl crackle - random pops over a ducked noise bed (texture, lo-fi, rain/fire). "
                     "Clearest on shaped/quieter noise (raise Reso or narrow Width); on full white noise it's masked."));
            break;
        case DrumChannel::SrcFM:
            // Wave A/Wave B (the FM carrier morph) are edited on the WaveMorphDisplay (morphView),
            // which in FM mode also shows the live FM result as Ratio/Depth change.
            p.add(F("Pitch", -24, 24, &S::fmPitch, "st"));
            p.add(F("Ratio", 0, 1, &S::fmSpread, "", true));
            p.add(F("Depth", 0, 1, &S::fmDepth, "", true));
            p.add(F("Sub", 0, 1, &S::fmSub, "", true));
            p.add(F("Feedback", 0, 1, &S::fmFeedback, "", true));
            break;   // pitch-env moved to the shared shape groups
        case DrumChannel::SrcPhys:
            // Position + Tone are edited on the interactive STRING visual (drag the dot: X = Position,
            // Y = Tone) - they're no longer knobs here.
            p.add(F ("Base Freq", 20, 4186, &S::physFreq, "nHz", false,
                     "Base pitch of the plucked/struck string. CLICK the value read-out to switch Hz <-> NOTE mode "
                     "(shows A1/C2...; dragging snaps to semitones, SHIFT = free). Also follows per-step pitch. "
                     "KEYS: touching the piano snaps this to C4 (middle C) so recordings play back as performed - transpose "
                     "with it afterwards."));
            p.add(Fc("Material", 0, 5, &S::physMaterial, { "Nylon","Steel","Wood","Bell","Metal","Skin" },
                     "The string/body material.\n\n"
                     "- Nylon: round + dark, soft finger pluck.\n- Steel: bright, long, sharp pick.\n"
                     "- Wood: clicky DRY knock, very short.\n- Bell: SOFT ping attack, very long ring, "
                     "shimmering stretched partials on higher notes.\n"
                     "- Metal: heavy stretch, darker clank, shorter.\n- Skin: boomy drumhead with a pitch drop.\n\n"
                     "Ring length differences need a longer Ring to show; materials now KEEP their "
                     "character while a key is held (and they stack with Stiffness)."));
            p.add(F ("Stiffness", 0, 1, &S::physStiff, "", true,
                     "Inharmonicity: bends the overtones progressively SHARP - pure string -> stiff bar -> bell "
                     "(the fundamental stays in tune). Most obvious with a bright Tone and a longer Ring."));
            p.add(Ic("Excite", 0, 2, &S::physExcite, { "Pluck","Strike","Mallet" },
                     "How the string is set in motion.\n\n"
                     "- Pluck: the full rich burst - bright, sustaining.\n"
                     "- Strike: a short sharp SLAP - snappier attack, similar loudness.\n"
                     "- Mallet: a soft padded push - dark and thumpy. A smooth push has few overtones, "
                     "so materials naturally sound closer together with it (real mallets do this too)."));
            break;   // pitch-env/Vibrato moved to the shared shape groups (4 params = knob grid)
        case DrumChannel::SrcGrain:
            p.add(F ("Position", 0, 1, &S::grainPos, "", true,
                     "WHERE in the source the grains read.\n\n"
                     "- Sample source: the spot in the file.\n"
                     "- Wave source: the spot in the wave JOURNEY (Custom's frames A->D sweep across it - "
                     "plain waves are uniform, so Position matters less there)."));
            p.add(F ("Size", 0, 1, &S::grainSize, "", true,
                     "Grain length (15..350 ms). Small = grainy/AM texture; big = smooth, overlapping washes."));
            p.add(F ("Density", 0, 1, &S::grainDens, "", true,
                     "How many grains per second (3..50). Sparse = particles and stutters; dense = a solid cloud."));
            p.add(F ("Spray", 0, 1, &S::grainSpray, "", true,
                     "Random position scatter per grain. 0 = all grains read the same spot (frozen); "
                     "up = the cloud spreads across the source."));
            p.add(F ("Pitch Spray", 0, 1, &S::grainPitch, "", true,
                     "Random per-grain detune, up to +-12 semitones. A little = shimmer/chorus; "
                     "a lot = glittering pitch clouds."));
            break;
        case DrumChannel::SrcSample:
            // NO Pitch knob here (user call: removed completely - per-step pitch + the pitch envelope
            // cover melodies; the legacy channel pitch / smpPitch fields stay dormant for old projects).
            { SlotParam sg = F("Gain", 0.0, 4.0, &S::smpGain, "x");
              sg.tooltip = "Sample output boost - samples are usually quieter than the synth engines (1x = unchanged).";
              p.add(sg); }
            p.add(F("Crush", 0, 1, &S::smpCrush, "", true,
                    "Bit-crush / downsample - lo-fi grit + aliasing (0 = clean, up = crunchier/dirtier)."));
            // (Auto "Slices" knob removed - the Trim multi-region drawing on the waveform replaces it.)
            { SlotParam sp = F("Stretch", 0.25, 4.0, &S::smpStretch, "x");
              sp.reBake = true;   // time-stretch needs a SoundTouch re-bake (done on drag-end)
              sp.tooltip = "Time-stretch (SoundTouch): change the sample's length without changing its pitch. 1x = off.";
              p.add(sp); }
            break;   // pitch-env moved to the shared shape groups
        // (SrcSynth + SrcWave param lists removed - those slot engines are retired and old projects
        //  migrate to Analog+FM on load, so slotParamsFor is never called with them.)
        case DrumChannel::SrcModal: {
            juce::StringArray mats;
            for (int i = 0; i < DrumChannel::modalMaterialCount(); ++i) mats.add(DrumChannel::modalMaterialName(i));
            p.add(F ("Base Freq", 20, 4186, &S::oscFreq, "nHz", false, "Base pitch of the struck body - the 0-point of step Pitch mode. CLICK the value read-out to switch Hz <-> NOTE mode (snaps to semitones there, SHIFT = free). Follows per-step pitch + the pitch envelope. Double-click = C4. The keyboard plays real notes and never changes this."));
            p.add(Ic("Material", 0, juce::jmax(0, DrumChannel::modalMaterialCount() - 1), &S::modalMaterial, mats,
                     "The struck body - sets each mode's frequency, gain + decay (Marimba/Tubular Bell/Glass/Membrane/"
                     "Metal Plate/Wood Block/Kalimba/Cowbell). The starting point Decay/Tone/Struct then shape."));
            // Decay moved OUT to the shared amp-env editor's RING handle (Strike/Ring, like Physical).
            // Hit Pos + Damp are edited on the interactive struck-body visual (drag the hammer dot).
            p.add(F ("Morph", 0, 1, &S::modalMorph, "", true,
                     "Material morph: crossfades this Material's modes toward the NEXT material in the list - "
                     "in-between bodies (marimba-into-bell, glass-into-membrane...). 0 = pure."));
            p.add(F ("Tone", 0, 1, &S::modalTone, "", true, "Brightness: dark (highs damped, quick) <-> bright (highs ring)."));
            p.add(F ("Struct", 0, 1, &S::modalStruct, "", true,
                     "Structure: stretches/compresses the mode pitches - harmonic/tuned <-> inharmonic/metallic."));
            return p; }
        default: return p;
    }
    // Unison/Detune/Vibrato + the pitch envelope are now edited in the shared shape
    // groups (per slot), so nothing is appended here.
    return p;
}

//==============================================================================
// WaveMorphDisplay
//==============================================================================
float WaveMorphDisplay::basicWave(int shape, float ph01)
{
    ph01 -= std::floor(ph01);
    switch (shape) {
        case 1:  return 1.0f - 4.0f * std::abs(ph01 - 0.5f);                      // triangle
        case 2:  return ph01 < 0.5f ? 1.0f : -1.0f;                               // square
        case 3:  return 2.0f * ph01 - 1.0f;                                       // saw (ramp up)
        case 4:  return 1.0f - 2.0f * ph01;                                       // ramp down
        case 5:  return ph01 < 0.25f ? 1.0f : -1.0f;                              // pulse (25%)
        case 6:  return 2.0f * std::sin(juce::MathConstants<float>::pi * ph01) - 1.0f;  // hump
        default: return std::sin(ph01 * juce::MathConstants<float>::twoPi);        // sine
    }
}

void WaveMorphDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
    auto full = b.reduced(6.0f, compact ? 2.0f : 4.0f);
    // Compact form (resonator open, box is tight): drop the A/B label strip, use it all for the wave.
    auto labelRow = compact ? juce::Rectangle<float>() : full.removeFromBottom(13.0f);
    auto in = full;
    const float cy = in.getCentreY();

    auto* s = getSlot ? getSlot() : nullptr;
    const int   wave  = s ? juce::jlimit(0, DrumChannel::oscShapeCount() - 1, s->oscShape) : 0;  // the single Wave
    // Live matrix modulation overrides the drawn FM/Warp (the preview moves with the modulated sound).
    const float ratio = modRatio > -900.0f ? juce::jlimit(0.0f, 1.0f, modRatio) : (s ? juce::jlimit(0.0f, 1.0f, s->fmSpread) : 0.0f);
    const float depth = modFm    > -900.0f ? juce::jlimit(0.0f, 1.0f, modFm)    : (s ? juce::jlimit(0.0f, 1.0f, s->fmDepth)  : 0.0f);

    g.setColour(juce::Colour(0xff2a2a44)); g.drawHorizontalLine((int) cy, in.getX(), in.getRight());

    // A few cycles across the width of the SELECTED wave. FM phase-modulates it (Ratio/Depth); Warp skews it.

    const float twoPi   = juce::MathConstants<float>::twoPi;
    const float fmRatio = 1.0f + ratio * 5.0f;     // matches DSP fmModF = f*(1+spread*5)
    const float fmIndex = depth * 6.0f;            // radians of phase modulation (visual)
    const float warp = modWarp > -900.0f ? juce::jlimit(0.0f, 1.0f, modWarp)
                                         : (s ? juce::jlimit(0.0f, 1.0f, s->oscWarp) : 0.0f);   // Warp = one-way WAVEFOLD (0 = off)
    auto fold = [warp](float v) {                   // same fold as the DSP (adds harmonics as warp rises)
        if (warp < 0.001f) return v;
        const float folded = std::sin(v * (1.0f + warp * 4.0f) * 1.5707963f);
        return v + warp * (folded - v);
    };
    const bool custom = wave >= DrumChannel::WvCustom;
    const bool morphOn = custom && s != nullptr && getCustomFrame
                      && (s->addSeg[0] > 0.001f || s->lfoAmt[3] > 0.001f);   // in MOTION: glide or WAVE LFO
    const float CYC = morphOn ? 1.0f : 2.0f;   // 2 cycles; 1 per panel in the split view (4 was too dense - user)
    auto drawWave = [&](juce::Rectangle<float> r, const float* tbl, juce::Colour col) {
        juce::Path path; const int NP = 200;
        const float rcy = r.getCentreY(), rhh = r.getHeight() * 0.5f - 1.0f;
        for (int i = 0; i < NP; ++i) {
            const float fx = (float) i / (float)(NP - 1);
            float ph = fx * CYC;
            if (fmMode) ph += (fmIndex / twoPi) * std::sin(ph * fmRatio * twoPi);
            const float v = fold(DrumChannel::oscShapeSample(wave, ph, tbl));
            const float x = r.getX() + fx * r.getWidth();
            const float y = rcy - juce::jlimit(-1.0f, 1.0f, v) * rhh;
            if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
        }
        g.setColour(col); g.strokePath(path, juce::PathStrokeType(1.6f));
    };
    if (morphOn)
    {   // WAVETABLE MOTION preview: EVERY frame the journey/LFO can actually reach, side by side
        // with little arrows (user: "i should be able to see all enabled ones"). Glide = frames
        // A..the first HOLD; a WAVE-LFO scan = the frames inside pos +/- half the LFO depth.
        int f0 = 0, f1 = 3;
        if (s->addSeg[0] > 0.001f)
            f1 = s->addSeg[1] <= 0.001f ? 1 : (s->addSeg[2] <= 0.001f ? 2 : 3);
        else
        {   const float lo = juce::jlimit(0.0f, 1.0f, s->addPos - s->lfoAmt[3] * 0.5f);
            const float hi = juce::jlimit(0.0f, 1.0f, s->addPos + s->lfoAmt[3] * 0.5f);
            f0 = (int) std::floor(lo * 3.0f); f1 = (int) std::ceil(hi * 3.0f);
            f1 = juce::jmax(f1, f0 + 1);   // always at least a 2-frame span while scanning
        }
        static const juce::Colour fc[4] = { juce::Colour(0xff35c0ff), juce::Colour(0xff7fd4ff),
                                            juce::Colour(0xffb46bff), juce::Colour(0xff35b56a) };
        const int   n   = f1 - f0 + 1;
        const float gap = 11.0f;
        const float ww  = (in.getWidth() - gap * (float)(n - 1)) / (float) n;
        for (int f = f0; f <= f1; ++f)
        {
            auto rf = juce::Rectangle<float>(in.getX() + (float)(f - f0) * (ww + gap), in.getY(), ww, in.getHeight());
            drawWave(rf, getCustomFrame(f), fc[f]);
            if (f < f1)
            {   const float ax = rf.getRight() + gap * 0.5f, ay = in.getCentreY();   // drawn arrow, never unicode
                juce::Path ar; ar.addTriangle(ax + 4.0f, ay, ax - 2.5f, ay - 3.5f, ax - 2.5f, ay + 3.5f);
                g.setColour(juce::Colour(0xffe8bf4d)); g.fillPath(ar);
                g.drawLine(ax - 5.5f, ay, ax - 2.5f, ay, 1.2f);
            }
        }
    }
    else
        drawWave(in, (custom && getCustomTbl) ? getCustomTbl() : nullptr, juce::Colour(0xff35c0ff));

    // Wave name under it - for Custom the INVITATION lives here too, so nothing ever covers the
    // wave drawing itself (the old floating pill blocked the B wave in the morph split view).
    if (! compact) {
        g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.setColour(custom ? juce::Colour(0xffe8bf4d) : juce::Colour(0xff8aa0c8));
        g.drawText(custom ? juce::String(wave + 1) + "-Custom - CLICK to draw harmonics"
                          : juce::String(wave + 1) + "-" + DrumChannel::oscShapeName(wave), labelRow,
                   juce::Justification::centred, false);
    }
    if (custom && hoverEd) {   // hover = the whole preview reads as a button (accent frame)
        g.setColour(juce::Colour(0xffe8bf4d).withAlpha(0.8f));
        g.drawRoundedRectangle(b.reduced(1.0f), 4.0f, 1.6f);
    }
    if (grainN >= 0)
    {   // GRANULAR: the dots are the REAL positions the engine's grains are reading right now
        // (fed from getGrainSnapshot - honest visual, never an animation). A darker underlay strip
        // along the bottom so the cyan dots read against bright waves.
        g.setColour(juce::Colour(0x66000000));
        g.fillRoundedRectangle(b.getX() + 2.0f, b.getBottom() - 9.0f, b.getWidth() - 4.0f, 8.0f, 2.0f);
        g.setColour(juce::Colour(0xff35c0ff));
        for (int i = 0; i < grainN; ++i)
        {
            const float x = b.getX() + 4.0f + grainPos[i] * (b.getWidth() - 8.0f);
            g.fillEllipse(x - 2.0f, b.getBottom() - 7.0f, 4.0f, 4.0f);
        }
        if (srcName.isNotEmpty())
        {   // the source caption on a PILL so it never smears over the waveform (user: text overlap)
            const juce::Font capFont(9.0f, juce::Font::bold);
            g.setFont(capFont);
            const int tw = juce::jmin((int) b.getWidth() - 10, juce::GlyphArrangement::getStringWidthInt(capFont, srcName) + 8);
            const juce::Rectangle<float> pill((float) ((int) b.getX() + 4), (float) ((int) b.getY() + 2), (float) tw, 12.0f);
            g.setColour(juce::Colour(0xcc101022)); g.fillRoundedRectangle(pill, 3.0f);
            g.setColour(juce::Colour(0xff9fd1ff));
            g.drawText(srcName, pill.reduced(4.0f, 0.0f), juce::Justification::centredLeft, false);
        }
    }
}

void WaveMorphDisplay::mouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    // Clicking the drawing ALWAYS opens the harmonic editor now - if the Wave isn't Custom yet, the
    // handler switches it (seeding the frames from the outgoing shape so the tone doesn't jump).
    if (getSlot && getSlot() != nullptr && onOpenCustom) onOpenCustom();
}

//==============================================================================
// HarmonicEditor (ADDITIVE Custom wave) - SIDE-BY-SIDE spectra: A (left, the note's start) and
// B (right, where Morph travels to). Each half: presets | freehand WAVE strip | 32 harmonic bars
// with interval watermarks. The Morph fader lives in the header between the title and the X.
//==============================================================================
void HarmonicEditor::strokeToHarmonics(int f)
{
    float* vals = HV(f); float* phs = HP(f); const float* w0 = WP(f);
    float mx = 1.0e-6f;
    float mag[DrumChannel::ADD_HARM];
    for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
    {
        double S = 0.0, C = 0.0;
        const double w = 2.0 * juce::MathConstants<double>::pi * (double)(k + 1) / (double) WPTS;
        for (int n = 0; n < WPTS; ++n) { S += w0[n] * std::sin(w * n); C += w0[n] * std::cos(w * n); }
        mag[k] = (float)(2.0 / WPTS * std::sqrt(S * S + C * C));
        phs[k] = (float) std::atan2(C, S);
        mx = juce::jmax(mx, mag[k]);
    }
    for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
    {
        vals[k] = juce::jlimit(0.0f, 1.0f, mag[k] / mx);
        if (vals[k] < 0.02f) { vals[k] = 0.0f; phs[k] = 0.0f; }   // tidy the bar view (inaudible bins)
    }
}

void HarmonicEditor::rebuildStrokeFromHarmonics(int f)
{
    float* vals = HV(f); float* phs = HP(f); float* wp = WP(f);
    float pk = 1.0e-6f;
    for (int n = 0; n < WPTS; ++n)
    {
        double v = 0.0;
        for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
            if (vals[k] > 0.001f)
                v += vals[k] * std::sin(2.0 * juce::MathConstants<double>::pi * (double)(k + 1) * n / (double) WPTS + phs[k]);
        wp[n] = (float) v; pk = juce::jmax(pk, std::abs(wp[n]));
    }
    for (int n = 0; n < WPTS; ++n) wp[n] /= pk;
}

void HarmonicEditor::paint(juce::Graphics& g)
{
    auto bb = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xf0141428)); g.fillRoundedRectangle(bb, 6.0f);
    g.setColour(accent); g.drawRoundedRectangle(bb.reduced(0.5f), 6.0f, 1.4f);
    g.setFont(juce::Font(12.5f, juce::Font::bold)); g.setColour(accent);
    g.drawText("ADDITIVE WAVETABLE - SLOT " + juce::String(slotIdx + 1), 12, 2, 260, 16,
               juce::Justification::centredLeft, false);
    g.setColour(juce::Colour(0xffc05050)); g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.drawText("X", closeRect(), juce::Justification::centred, false);

    static const char* iv[12] = { "Root", "b2", "2nd", "m3", "3rd", "4th", "b5", "5th", "m6", "6th", "b7", "7th" };
    static const char* fl[NF] = { "A", "B", "C", "D" };
    for (int f = 0; f < NF; ++f)
    {
        float* vals = HV(f);
        const juce::Colour fc = frameCol(f);
        g.setColour(fc); g.setFont(juce::Font(13.0f, juce::Font::bold));
        g.drawText(fl[f], (int) frameRect(f).getX(), (int) frameRect(f).getY(), 14, 15,
                   juce::Justification::centred, false);
        {   // shape DROPDOWN: what this frame holds ("Bell", an analyzed file's name, or "Custom")
            auto r = shapeBtnRect(f);
            g.setColour(juce::Colour(0xff223046)); g.fillRoundedRectangle(r, 3.0f);
            g.setColour(juce::Colour(0xff9fd1ff)); g.setFont(juce::Font(10.5f, juce::Font::bold));
            const juce::String nm = frameLabel[f].isNotEmpty() ? frameLabel[f]
                                  : (loadedShape[f] >= 0 ? juce::String(DrumChannel::oscShapeName(loadedShape[f]))
                                                         : juce::String("Custom"));
            g.drawText(nm, r.reduced(8.0f, 0.0f), juce::Justification::centredLeft, false);
            juce::Path tri; const float tx = r.getRight() - 12.0f, ty = r.getCentreY();   // drawn triangle, never unicode
            tri.addTriangle(tx - 4.0f, ty - 2.5f, tx + 4.0f, ty - 2.5f, tx, ty + 3.5f);
            g.fillPath(tri);
        }
        {   // ANALYZE SAMPLE = a command cell (blue, like the picker's command rows)
            auto r = smpRect(f);
            g.setColour(juce::Colour(0xff223046)); g.fillRoundedRectangle(r, 3.0f);
            g.setColour(juce::Colour(0xff9fd1ff)); g.setFont(juce::Font(10.0f, juce::Font::bold));
            g.drawText("Analyze sample", r, juce::Justification::centred, false);
        }
        {   // Clear = an ACTION (red), wipes this frame
            auto r = clrRect(f);
            g.setColour(juce::Colour(0xff4a2430)); g.fillRoundedRectangle(r, 3.0f);
            g.setColour(juce::Colour(0xffe08a8a)); g.setFont(juce::Font(10.5f, juce::Font::bold));
            g.drawText("Clear", r, juce::Justification::centred, false);
        }
        {   // freehand WAVE strip (the actual playable cycle, band-limited)
            auto wr = waveStripRect(f);
            g.setColour(juce::Colour(0xff10101f)); g.fillRoundedRectangle(wr, 3.0f);
            g.setColour(fc.withAlpha(0.35f)); g.drawRoundedRectangle(wr, 3.0f, 1.0f);
            g.setColour(juce::Colour(0xff262640)); g.drawHorizontalLine((int) wr.getCentreY(), wr.getX(), wr.getRight());
            const float* wp = WP(f);
            juce::Path path;
            for (int n = 0; n < WPTS; ++n)
            {
                const float x = wr.getX() + 2.0f + (wr.getWidth() - 4.0f) * (float) n / (float)(WPTS - 1);
                const float y = wr.getCentreY() - juce::jlimit(-1.0f, 1.0f, wp[n]) * (wr.getHeight() * 0.5f - 3.0f);
                if (n == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
            }
            g.setColour(fc); g.strokePath(path, juce::PathStrokeType(1.5f));
            g.setColour(juce::Colour(0xff6a7290)); g.setFont(juce::Font(8.5f, juce::Font::bold));
            g.drawText("draw the WAVE", wr.reduced(4.0f, 1.0f), juce::Justification::topRight, false);
            if (frameMsg[f].isNotEmpty())
            { g.setColour(juce::Colour(0xffff9a3c)); g.setFont(juce::Font(9.5f, juce::Font::bold));
              g.drawText(frameMsg[f], wr.reduced(4.0f, 1.0f), juce::Justification::bottomLeft, false); }
        }
        auto a = barArea(f);
        const float bw = a.getWidth() / (float) DrumChannel::ADD_HARM;
        for (int i = 0; i < DrumChannel::ADD_HARM; ++i)
        {
            auto br = juce::Rectangle<float>(a.getX() + i * bw + 0.5f, a.getY(), bw - 1.0f, a.getHeight());
            g.setColour(juce::Colour(0xff1c1c34)); g.fillRect(br);
            if (i == 0) { g.setColour(fc.withAlpha(0.5f)); g.drawRect(br, 1.0f); }   // Root = the anchor bar
            const float v = juce::jlimit(0.0f, 1.0f, vals[i]);
            if (v > 0.001f) { g.setColour(fc.withAlpha(0.85f));
                              g.fillRect(br.withTop(br.getBottom() - v * br.getHeight())); }
            {   // rotated interval watermark (what this bar ADDS to the played note)
                const int semis = juce::roundToInt(12.0 * std::log2((double)(i + 1)));
                const juce::String lbl = i == 0 ? juce::String("Root")
                                       : (semis % 12 == 0 ? "Oct" + juce::String(semis / 12)
                                                          : juce::String(iv[semis % 12]));
                juce::Graphics::ScopedSaveState ss(g);
                g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi,
                                                               br.getCentreX(), br.getCentreY()));
                g.setColour(v > 0.4f ? juce::Colours::black.withAlpha(0.55f) : juce::Colour(0x60aeb8d4));
                g.setFont(juce::Font(7.5f, juce::Font::bold));
                g.drawText(lbl, (int)(br.getCentreX() - br.getHeight() * 0.5f), (int)(br.getCentreY() - 5.0f),
                           (int) br.getHeight(), 10, juce::Justification::centred, false);
            }
            if ((i % 8) == 0 || i == DrumChannel::ADD_HARM - 1)
            { g.setColour(juce::Colour(0xff7d86a8)); g.setFont(juce::Font(8.0f, juce::Font::bold));
              g.drawText(juce::String(i + 1), (int) br.getX() - 6, (int) a.getBottom() + 1, (int) bw + 12, 10,
                         juce::Justification::centred, false); }
        }
        {   // pitch warning per frame: the heard pitch = the lowest STRONG bar
            int lowIdx = -1; float mx2 = 0.0f;
            for (int i = 0; i < DrumChannel::ADD_HARM; ++i) { mx2 = juce::jmax(mx2, vals[i]);
                if (lowIdx < 0 && vals[i] > 0.05f) lowIdx = i; }
            if (mx2 > 0.05f && lowIdx > 0)
            {
                g.setColour(juce::Colour(0xffff9a3c)); g.setFont(juce::Font(9.0f, juce::Font::bold));
                g.drawText("Root empty - pitch = bar " + juce::String(lowIdx + 1),
                           (int) a.getX(), (int) a.getY() + 1, (int) a.getWidth(), 11, juce::Justification::centred, false);
            }
        }
    }
    {   // ---- POSITION band (between the rows): where in A..D the sound sits ----
        auto pr = posRect();
        g.setColour(juce::Colour(0xffd8e0f0)); g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.drawText("Position", 8, (int) pr.getY(), 66, (int) pr.getHeight(), juce::Justification::centredLeft, false);
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(pr, 3.0f);
        const bool gliding = seg[0] > 0.001f;
        for (int f = 0; f < NF; ++f)   // A/B/C/D ticks: short STUBS top+bottom, the letter clear between
        {
            const float tx = pr.getX() + 6.0f + (pr.getWidth() - 12.0f) * (float) f / (float)(NF - 1);
            g.setColour(frameCol(f).withAlpha(gliding ? 0.4f : 0.9f));
            g.drawVerticalLine((int) tx, pr.getY() + 1.0f, pr.getY() + 4.0f);
            g.drawVerticalLine((int) tx, pr.getBottom() - 4.0f, pr.getBottom() - 1.0f);
            g.setFont(juce::Font(9.0f, juce::Font::bold));
            g.drawText(fl[f], (int) tx - 6, (int) pr.getY() + 1, 12, (int) pr.getHeight() - 2,
                       juce::Justification::centred, false);
        }
        if (gliding)
        {   const int endF = seg[1] <= 0.001f ? 1 : (seg[2] <= 0.001f ? 2 : 3);   // where the journey stops
            const juce::String eF = juce::String::charToString((juce::juce_wchar)('A' + endF));
            g.setColour(juce::Colour(0xffaeb8d4)); g.setFont(juce::Font(9.5f, juce::Font::bold));
            g.drawText(loopOn ? "glide loops A <> " + eF + " forever" : "glide runs A > " + eF + " each note",
                       pr, juce::Justification::centred, false); }
        else
        {   const float hx = pr.getX() + 6.0f + (pr.getWidth() - 12.0f) * juce::jlimit(0.0f, 1.0f, wtPos);
            g.setColour(accent.withAlpha(0.30f));
            g.fillRoundedRectangle(pr.withRight(hx), 3.0f);
            g.setColour(juce::Colours::white); g.fillEllipse(hx - 4.0f, pr.getCentreY() - 4.0f, 8.0f, 8.0f); }
        if (livePos >= 0.0f)
        {   // LIVE marker (amber, the playhead colour): the position the engine is PLAYING right
            // now - handle + glide + WAVE LFO combined - straight from the newest voice's render.
            const float lx = pr.getX() + 6.0f + (pr.getWidth() - 12.0f) * juce::jlimit(0.0f, 1.0f, livePos);
            g.setColour(juce::Colour(0xffffc169));
            g.drawLine(lx, pr.getY() + 1.0f, lx, pr.getBottom() - 1.0f, 2.0f);
            g.fillEllipse(lx - 2.6f, pr.getCentreY() - 2.6f, 5.2f, 5.2f);
        }
    }
    for (int k = 0; k < NF - 1; ++k)
    {   // THREE per-leg GLIDE boxes (A>B / B>C / C>D): drag = travel time, hard left = HOLD.
        // A leg the journey can't reach (an earlier leg holds) draws dimmed.
        auto mr = segRect(k);
        const bool reachable = k == 0 || (seg[0] > 0.001f && (k == 1 || seg[1] > 0.001f));
        const float al = reachable ? 1.0f : 0.4f;
        g.setColour(juce::Colour(0xff26264a).withAlpha(al)); g.fillRoundedRectangle(mr, 3.0f);
        if (seg[k] > 0.001f)
        {   const float p = juce::jlimit(0.0f, 1.0f, std::log(seg[k] / 0.05f) / std::log(8.0f / 0.05f));
            g.setColour(frameCol(k + 1).withAlpha(0.30f * al));
            g.fillRoundedRectangle(mr.withWidth(juce::jmax(5.0f, (0.12f + 0.88f * p) * mr.getWidth())), 3.0f); }
        g.setColour(juce::Colour(0xffd8e0f0).withAlpha(al)); g.setFont(juce::Font(10.0f, juce::Font::bold));
        const juce::String leg = juce::String::charToString((juce::juce_wchar)('A' + k)) + ">"
                               + juce::String::charToString((juce::juce_wchar)('B' + k));
        g.drawText(seg[k] > 0.001f ? leg + " " + juce::String(seg[k], 2) + " s"
                                   : leg + (k == 0 ? " Off" : " Hold"),
                   mr, juce::Justification::centred, false);
    }
    {   // LOOP toggle: the glide travels out and BACK, forever (ping-pong - no snap)
        auto lr = loopRect();
        g.setColour(loopOn ? accent : juce::Colour(0xff26264a)); g.fillRoundedRectangle(lr, 3.0f);
        g.setColour(loopOn ? juce::Colours::black : juce::Colour(0xff9aa6c0)); g.setFont(juce::Font(10.0f, juce::Font::bold));
        g.drawText("Loop", lr, juce::Justification::centred, false);
    }
    g.setColour(juce::Colour(0xff6a7290)); g.setFont(juce::Font(9.0f, juce::Font::bold));
    g.drawText("4 frames A > D - Position picks the mix; the A>B / B>C / C>D boxes = per-leg glide times "
               "(left = hold); Loop = out and back forever - LEFT-drag = draw, RIGHT-drag = erase",
               12, getHeight() - 13, getWidth() - 24, 11, juce::Justification::centredLeft, false);
}

void HarmonicEditor::applyAt(const juce::MouseEvent& e, bool erase, int f)
{
    float* vals = HV(f); float* phs = HP(f);
    auto a = barArea(f);
    const int i = juce::jlimit(0, DrumChannel::ADD_HARM - 1,
                               (int) ((e.position.x - a.getX()) / (a.getWidth() / (float) DrumChannel::ADD_HARM)));
    const float lvl = erase ? 0.0f : juce::jlimit(0.0f, 1.0f, (a.getBottom() - e.position.y) / a.getHeight());
    if (std::abs(lvl - vals[i]) < 1.0e-3f) return;
    vals[i] = lvl;
    if (lvl <= 0.001f) phs[i] = 0.0f;             // an erased bar forgets its phase
    rebuildStrokeFromHarmonics(f);                 // the wave strip follows the bars
    if (onChange) onChange();
    repaint();
}

void HarmonicEditor::mouseDown(const juce::MouseEvent& e)
{
    if (closeRect().contains(e.position)) { if (onClose) onClose(); return; }
    if (loopRect().contains(e.position))
    { loopOn = ! loopOn; if (onChange) onChange(); if (onDragEnd) onDragEnd(); repaint(); return; }
    if (posRect().contains(e.position))   { posDragging = true; mouseDrag(e); return; }
    for (int k = 0; k < NF - 1; ++k)
        if (segRect(k).contains(e.position)) { segDragging = k; mouseDrag(e); return; }
    const int f = frameAt(e.position);
    if (f < 0) return;
    if (shapeBtnRect(f).contains(e.position))
    {   // shape dropdown: every entry is a RECIPE (built from sine harmonics, exact by construction)
        juce::PopupMenu m;
        for (int i = 0; i < DrumChannel::oscShapeCount() - 1; ++i)   // 0..13 (Custom itself excluded)
            m.addItem(i + 1, juce::String(DrumChannel::oscShapeName(i)), true, loadedShape[f] == i);
        menuOpen = true;
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
                            localAreaToGlobal(shapeBtnRect(f).getSmallestIntegerContainer())),
            [this, f](int r) {
                menuOpen = false;
                if (r > 0) { frameLabel[f].clear(); frameMsg[f].clear(); loadShapeSpectrum(f, r - 1); }
            });
        return;
    }
    if (smpRect(f).contains(e.position)) { analyzeSample(f); return; }
    if (clrRect(f).contains(e.position))
    {   // Clear this frame (bars to zero; phases forgotten)
        float* vals = HV(f); float* phs = HP(f);
        for (int k = 0; k < DrumChannel::ADD_HARM; ++k) { vals[k] = 0.0f; phs[k] = 0.0f; }
        loadedShape[f] = -1; frameLabel[f].clear(); frameMsg[f].clear();
        rebuildStrokeFromHarmonics(f);
        if (onChange) onChange();
        if (onDragEnd) onDragEnd();
        repaint(); return;
    }
    if (waveStripRect(f).contains(e.position))
    { waveDrawing = true; dragFrame = f; waveLastI = -1; loadedShape[f] = -1; frameLabel[f].clear(); mouseDrag(e); return; }
    if (barArea(f).contains(e.position))
    { drawing = true; dragFrame = f; loadedShape[f] = -1; frameLabel[f].clear();
      applyAt(e, e.mods.isRightButtonDown() || e.mods.isPopupMenu(), f); }
}

void HarmonicEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (posDragging)
    {   // POSITION: where in the A..D strip the (static) sound sits. SNAPS onto the exact frame
        // ticks when close (a letter = purely that frame - there is no per-frame on/off, the
        // position IS the selector); hold SHIFT for free placement (the FM-Ratio convention).
        auto pr = posRect();
        float p = juce::jlimit(0.0f, 1.0f, (e.position.x - pr.getX() - 6.0f) / juce::jmax(1.0f, pr.getWidth() - 12.0f));
        if (! e.mods.isShiftDown())
            for (int f = 0; f < NF; ++f)
            {   const float tick = (float) f / (float)(NF - 1);
                if (std::abs(p - tick) < 0.035f) { p = tick; break; }
            }
        wtPos = p;
        if (onChange) onChange();
        repaint(); return;
    }
    if (segDragging >= 0)
    {   // per-leg glide time: hard left = HOLD (leg 0 = glide Off), then log 0.05 .. 8 s
        auto mr = segRect(segDragging);
        const float f = juce::jlimit(0.0f, 1.0f, (e.position.x - mr.getX()) / juce::jmax(1.0f, mr.getWidth()));
        seg[segDragging] = f < 0.06f ? 0.0f : 0.05f * std::pow(8.0f / 0.05f, (f - 0.06f) / 0.94f);
        if (onChange) onChange();
        repaint(); return;
    }
    if (waveDrawing)
    {
        const int f = dragFrame;
        float* wp = WP(f);
        auto wr = waveStripRect(f);
        const int   i = juce::jlimit(0, WPTS - 1,
                        (int) ((e.position.x - wr.getX() - 2.0f) / (wr.getWidth() - 4.0f) * (float)(WPTS - 1)));
        const float v = juce::jlimit(-1.0f, 1.0f, (wr.getCentreY() - e.position.y) / (wr.getHeight() * 0.5f - 3.0f));
        // Smooth BRUSH, never a needle: a single click used to write a 1-sample impulse - which
        // contains EVERY frequency (all 32 bars jumped to full; correct Fourier, terrible UX).
        auto stamp = [&](int centre, float val, int radius)
        {
            for (int off = -radius; off <= radius; ++off)
            {
                const int n = centre + off;
                if (n < 0 || n >= WPTS) continue;
                const float w = 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * (float) off / (float)(radius + 1)));
                wp[n] += (val - wp[n]) * w;
            }
        };
        if (waveLastI < 0) stamp(i, v, 6);                       // first touch = a soft bump
        else
        {
            for (int n = juce::jmin(waveLastI, i); n <= juce::jmax(waveLastI, i); ++n)   // connect the swept span
            {
                const float t = waveLastI == i ? 1.0f : (float)(n - waveLastI) / (float)(i - waveLastI);
                wp[n] = wp[waveLastI] + (v - wp[waveLastI]) * (waveLastI <= i ? t : 1.0f - t);
            }
            stamp(i, v, 3);                                      // soften the stroke's leading edge
        }
        waveLastI = i;
        strokeToHarmonics(f);                      // stroke -> bars + phases, live
        if (onChange) onChange();
        repaint();
        return;
    }
    if (drawing) applyAt(e, e.mods.isRightButtonDown() || e.mods.isPopupMenu(), dragFrame);
}

// RECIPES (user direction B): every factory shape expressed as WHAT IT IS - a stack of sine
// harmonics (magnitude + phase per bar). The 8 bank shapes mirror OscShapeBank's authored recipes
// EXACTLY (keep in sync with DrumChannel.cpp - the mirror rule, like kUiScaleTab); the analytic
// shapes use their textbook Fourier series. Identity is BY CONSTRUCTION: the dropdown names what
// it just placed - no measuring, no tolerance-matching, no untruthful visuals.
static void addShapeRecipe(int shape, float* mag, float* ph)
{
    constexpr int N = DrumChannel::ADD_HARM;
    for (int k = 0; k < N; ++k) { mag[k] = 0.0f; ph[k] = 0.0f; }
    auto g = [](int h, float c, float w){ return std::exp(-((h - c) * (h - c)) / (2.0f * w * w)); };
    for (int k = 1; k <= N; ++k)
    {
        float m = 0.0f, p = 0.0f;
        switch (shape)
        {
            case 0: m = (k == 1) ? 1.0f : 0.0f; break;                                   // Sine
            case 1: m = 1.0f / (float)(4 * k * k - 1); p = -juce::MathConstants<float>::halfPi; break;   // Hump (half-sine bump)
            case 2: if (k & 1) { m = 1.0f / (float)(k * k);                              // Triangle
                                 p = (((k - 1) / 2) & 1) ? juce::MathConstants<float>::pi : 0.0f; } break;
            case 3: if (k & 1) m = 1.0f / (float) k; break;                              // Square (odd 1/k)
            case 4: m = 1.0f / (float) k; p = juce::MathConstants<float>::pi; break;     // Saw (ramp up)
            case 5: {                                                                     // Pulse (25% duty)
                const float d = 0.25f, kk = (float) k, tp = juce::MathConstants<float>::twoPi;
                const float A = (1.0f - std::cos(tp * kk * d)) / kk;                     // sin component
                const float B = std::sin(tp * kk * d) / kk;                              // cos component
                m = std::sqrt(A * A + B * B); p = std::atan2(B, A); break; }
            case 6:  m = g(k, 3, 1.2f) + 0.6f * g(k, 7, 2.0f); break;                    // Vowel A
            case 7:  m = g(k, 2, 1.0f) + 0.6f * g(k, 5, 1.5f); break;                    // Vowel O
            case 8:  m = g(k, 6, 1.4f); break;                                           // Formant
            case 9:  if ((k & (k - 1)) == 0) m = 1.0f / (std::log2((float) k) + 1.0f); break;   // Organ (octaves)
            case 10: { static const int pr[] = { 1, 2, 3, 5, 7, 11 };                    // Bell (prime partials)
                       for (int q : pr) if (q == k) m = 1.0f / (float) k; break; }
            case 11: if (k & 1) m = 1.0f / std::sqrt((float) k); break;                  // Glass (bright odd)
            case 12: if ((k & 1) && k <= 24) m = 1.0f / (float) k; break;                // Reed (clarinet)
            case 13: m = (1.0f / (float) k) * (1.0f + 1.5f * g(k, 5, 2.0f)); break;      // Brass (saw + formant)
            default: break;
        }
        mag[k - 1] = m; ph[k - 1] = p;
    }
    float mx = 1.0e-6f; for (int k = 0; k < N; ++k) mx = juce::jmax(mx, mag[k]);
    for (int k = 0; k < N; ++k) { mag[k] /= mx; if (mag[k] < 0.005f) { mag[k] = 0.0f; ph[k] = 0.0f; } }
}

void HarmonicEditor::loadShapeSpectrum(int f, int shape)
{
    addShapeRecipe(shape, HV(f), HP(f));           // the recipe IS the spectrum (exact, deterministic)
    loadedShape[f] = shape;                        // identity by construction, not by matching
    rebuildStrokeFromHarmonics(f);                 // show the reconstructed cycle in the strip
    if (onChange) onChange();
    if (onDragEnd) onDragEnd();
    repaint();
}

// ANALYZE SAMPLE: pick an audio file, pitch-detect it (the tuner's shared NSDF detector), then
// measure its 32 harmonics (Goertzel at k*f0, magnitude + phase) into this frame's bars. The
// analysis window skips the attack (starts 25% in) so the SUSTAIN's spectrum is captured.
// MESSAGE THREAD (async chooser callback); unpitched material reports instead of guessing.
void HarmonicEditor::analyzeSample(int f)
{
    if (menuOpen) return;   // a chooser/menu is already up - never stack two
    menuOpen = true;   // the OS file dialog must not trip the outside-click closer
    chooser = std::make_unique<juce::FileChooser>(
        "Analyze an audio file into frame " + juce::String::charToString((juce::juce_wchar)('A' + f)),
        UserPaths::samples(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, f](const juce::FileChooser& fc)
        {
            menuOpen = false;
            const juce::File file = fc.getResult();
            if (file == juce::File()) return;
            juce::AudioFormatManager afm; afm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor(file));
            if (rd == nullptr || rd->lengthInSamples < 512 || rd->numChannels < 1)
            { frameMsg[f] = "can't read that file"; repaint(); return; }
            const int len = (int) juce::jmin(rd->lengthInSamples, (juce::int64) (1 << 21));   // no explicit jmin<int64> (Linux GCC pulls the SIMD overload in)
            juce::AudioBuffer<float> tmp((int) rd->numChannels, len);
            rd->read(&tmp, 0, len, 0, true, true);
            std::vector<float> mono((size_t) len, 0.0f);
            for (int c2 = 0; c2 < (int) rd->numChannels; ++c2)
            {   const float* src = tmp.getReadPointer(c2);
                for (int n = 0; n < len; ++n) mono[(size_t) n] += src[n] / (float) rd->numChannels; }
            const int start = juce::jlimit(0, len - 256, len / 4);       // skip the attack
            const int W = juce::jmin(16384, len - start);
            const double fs = rd->sampleRate;
            double f0 = basamakDetectPitch(mono.data() + start, W, fs);
            if (f0 <= 0.0)
            {   // NO CLEAR PITCH (drums/noise/chords): fall back to the STRONGEST SPECTRAL PEAK as
                // the "fundamental" - the analysis always produces SOMETHING editable (disclosed).
                double bestF = 110.0, bestM = -1.0;
                for (int q = 0; q < 60; ++q)
                {
                    const double fq = 55.0 * std::pow(2.0, (double) q / 10.0);   // 55 Hz .. ~3.3 kHz, 10/oct
                    if (fq > fs * 0.45) break;
                    double S = 0.0, Co = 0.0; const double w = 2.0 * juce::MathConstants<double>::pi * fq / fs;
                    for (int n = 0; n < W; n += 2) { const double s2 = (double) mono[(size_t)(start + n)];
                        S += s2 * std::sin(w * n); Co += s2 * std::cos(w * n); }
                    const double m2 = S * S + Co * Co;
                    if (m2 > bestM) { bestM = m2; bestF = fq; }
                }
                f0 = bestF;
                frameMsg[f] = "no clear pitch - used the strongest peak (" + juce::String((int) f0) + " Hz)";
            }
            float* vals = HV(f); float* phsF = HP(f);
            float mag[DrumChannel::ADD_HARM]; float mx = 1.0e-6f;
            for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
            {
                const double fk = f0 * (double)(k + 1);
                mag[k] = 0.0f; phsF[k] = 0.0f;
                if (fk > fs * 0.45) continue;                            // above the file's usable band
                double S = 0.0, Co = 0.0; const double w = 2.0 * juce::MathConstants<double>::pi * fk / fs;
                for (int n = 0; n < W; ++n)
                { const double s2 = (double) mono[(size_t)(start + n)];
                  S += s2 * std::sin(w * n); Co += s2 * std::cos(w * n); }
                mag[k] = (float) std::sqrt(S * S + Co * Co);
                phsF[k] = (float) std::atan2(Co, S);
                mx = juce::jmax(mx, mag[k]);
            }
            for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
            {
                vals[k] = juce::jlimit(0.0f, 1.0f, mag[k] / mx);
                if (vals[k] < 0.02f) { vals[k] = 0.0f; phsF[k] = 0.0f; }
            }
            loadedShape[f] = -1;
            frameLabel[f] = file.getFileNameWithoutExtension();
            if (! frameMsg[f].startsWith("no clear pitch")) frameMsg[f].clear();
            rebuildStrokeFromHarmonics(f);
            if (onChange) onChange();
            if (onDragEnd) onDragEnd();
            repaint();
        });
}

// Name a half on OPEN if its saved bars equal a factory recipe (so a sound saved as "Bell" reopens
// saying Bell). Hand-drawn spectra match nothing -> "Custom".
void HarmonicEditor::detectLoadedShape(int f)
{
    const float* vals = HV(f); const float* phs = HP(f);
    loadedShape[f] = -1;
    float rm[DrumChannel::ADD_HARM], rp[DrumChannel::ADD_HARM];
    for (int sh = 0; sh < DrumChannel::oscShapeCount() - 1; ++sh)
    {
        addShapeRecipe(sh, rm, rp);
        bool ok = true;
        for (int k = 0; k < DrumChannel::ADD_HARM && ok; ++k)
        {
            if (std::abs(vals[k] - rm[k]) > 0.01f) ok = false;
            else if (rm[k] > 0.05f)
            {   float dp = std::abs(phs[k] - rp[k]);
                while (dp > juce::MathConstants<float>::pi) dp = std::abs(dp - juce::MathConstants<float>::twoPi);
                if (dp > 0.2f) ok = false; }
        }
        if (ok) { loadedShape[f] = sh; return; }
    }
}

juce::String HarmonicEditor::getTooltip()
{
    const auto p = getMouseXYRelative().toFloat();
    if (posRect().contains(p))
        return "POSITION: where in the A > D strip the sound sits.\n\n"
               "- The two neighbouring frames crossfade; the handle SNAPS onto the letters "
               "(a letter = purely that frame - hold SHIFT for free placement).\n"
               "- Greyed while glide is on (the note travels instead).\n"
               "- The AMBER line = the position actually PLAYING right now (handle + glide + "
               "WAVE LFO combined) - watch it move while a note sounds.";
    if (loopRect().contains(p))
        return "LOOP: the glide travels its legs OUT and then BACK, forever (ping-pong, so there is "
               "no snap) - a breathing, evolving tone instead of settling on the last frame.";
    for (int k = 0; k < NF - 1; ++k)
        if (segRect(k).contains(p))
        {
            const juce::String from = juce::String::charToString((juce::juce_wchar)('A' + k));
            const juce::String to   = juce::String::charToString((juce::juce_wchar)('B' + k));
            return "GLIDE " + from + ">" + to + ": how long each note takes to travel this leg of the "
                   "strip. Hard left = " + (k == 0 ? juce::String("OFF (no glide - the sound sits at Position)")
                                                   : "HOLD (the journey stops at " + from + " and stays)") + ". "
                   "Example: A>B 0.1 s + B>C 2 s + C>D Hold = a fast strike colour, a slow bloom, then it sits on C.";
        }
    const int f = frameAt(p);
    if (f >= 0)
    {
        const juce::String fn = juce::String::charToString((juce::juce_wchar)('A' + f));
        if (shapeBtnRect(f).contains(p))
            return "Load a ready-made wave into frame " + fn + ". Every entry is placed as its "
                   "EXACT harmonic recipe (a stack of sines) - edit any bar afterwards and it becomes Custom.";
        if (smpRect(f).contains(p))
            return "ANALYZE SAMPLE: pick an audio file - its harmonics become frame " + fn + "'s bars. "
                   "The RULES:\n\n"
                   "- Works best on a SUSTAINED, single-pitch sound (one note of a flute, a pad, a string). "
                   "Chords, drums and noise have no single pitch - those fall back to the strongest "
                   "frequency peak (the message under the wave says so).\n"
                   "- Any length from ~0.1 s works; the analysis listens to the SUSTAIN (it skips the first "
                   "quarter of the file to avoid the attack) for up to ~0.3 s.\n"
                   "- Stereo files are mixed to mono; re-analyzing the same frame simply replaces it.\n\n"
                   "Analyze different files into different frames, then Glide or the WAVE LFO morphs "
                   "between the real sounds.";
        if (clrRect(f).contains(p))
            return "Clear frame " + fn + " (all bars to zero).";
        if (waveStripRect(f).contains(p))
            return "Draw ONE CYCLE of frame " + fn + "'s wave freehand - it is analysed into the "
                   "bars below automatically.";
        if (barArea(f).contains(p))
            return "Frame " + fn + ": each bar = one harmonic of the note you play; its label "
                   "says what it adds (Root, Oct, 5th, 3rd...). LEFT-drag = draw, RIGHT-drag = erase. "
                   "The lowest strong bar sets the pitch you hear.";
    }
    return "Additive WAVETABLE: four drawable frames (A > D). Position picks the mix, Glide travels "
           "it each note, the WAVE LFO (LFO visual) scans it live.";
}

void LfoCurveEditor::paint(juce::Graphics& g)
{
    auto bb = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xf0141428)); g.fillRoundedRectangle(bb, 6.0f);
    g.setColour(accent); g.drawRoundedRectangle(bb.reduced(0.5f), 6.0f, 1.4f);
    g.setFont(juce::Font(12.5f, juce::Font::bold)); g.setColour(accent);
    g.drawText("DRAW LFO SHAPE", 12, 4, 200, 16, juce::Justification::centredLeft, false);
    g.setColour(juce::Colour(0xffc05050)); g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.drawText("X", closeRect(), juce::Justification::centred, false);
    const auto a = strip();
    g.setColour(juce::Colour(0xff10101f)); g.fillRoundedRectangle(a, 3.0f);
    g.setColour(accent.withAlpha(0.35f)); g.drawRoundedRectangle(a, 3.0f, 1.0f);
    g.setColour(juce::Colour(0xff262640));
    g.drawHorizontalLine((int) a.getCentreY(), a.getX(), a.getRight());
    for (int q = 1; q < 4; ++q)   // quarter-cycle guides
        g.drawVerticalLine((int) (a.getX() + a.getWidth() * (float) q / 4.0f), a.getY() + 2.0f, a.getBottom() - 2.0f);
    juce::Path p;
    for (int k = 0; k < CVN; ++k)
    {
        const float x = a.getX() + 2.0f + (a.getWidth() - 4.0f) * (float) k / (float)(CVN - 1);
        const float y = a.getCentreY() - juce::jlimit(-1.0f, 1.0f, curve[k]) * (a.getHeight() * 0.5f - 3.0f);
        if (k == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
    }
    g.setColour(accent); g.strokePath(p, juce::PathStrokeType(1.8f));
    g.setColour(juce::Colour(0xff6a7290)); g.setFont(juce::Font(9.0f, juce::Font::bold));
    g.drawText("LEFT-drag = draw - one cycle - Amount scales it - click outside to close",
               12, getHeight() - 13, getWidth() - 24, 11, juce::Justification::centredLeft, false);
}
void LfoCurveEditor::mouseDown(const juce::MouseEvent& e)
{
    if (closeRect().contains(e.position)) { setVisible(false); if (onClose) onClose(); return; }
    lastI = -1; mouseDrag(e);
}
void LfoCurveEditor::mouseDrag(const juce::MouseEvent& e)
{
    const auto a = strip();
    const int   i = juce::jlimit(0, CVN - 1, (int) ((e.position.x - a.getX() - 2.0f) / juce::jmax(1.0f, a.getWidth() - 4.0f) * (float)(CVN - 1)));
    const float v = juce::jlimit(-1.0f, 1.0f, (a.getCentreY() - e.position.y) / juce::jmax(1.0f, a.getHeight() * 0.5f - 3.0f));
    if (lastI < 0) curve[i] = v;
    else for (int n = juce::jmin(lastI, i); n <= juce::jmax(lastI, i); ++n)
    {   const float t = lastI == i ? 1.0f : (float)(n - lastI) / (float)(i - lastI);
        curve[n] = curve[lastI] + (v - curve[lastI]) * (lastI <= i ? t : 1.0f - t); }
    lastI = i;
    if (onChange) onChange(curve);
    repaint();
}

//==============================================================================
// MOD MATRIX shared name tables (used by ModFaderMatrix + RoutePicker)
//==============================================================================
static const char* kModSrcName[DrumChannel::MS_COUNT] =   // the LFOs are GENERIC now (index, not destination)
{ "Off", "Velocity", "Note", "Amp Env", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
  "Random", "Mod Env", "Mod LFO", "Step Mod A", "Step Mod B", "Mod Wheel",
  "Aftertouch", "Slide",
  "Pitch Wheel" };   // [2026-07-14 03:00] the wheel as a source (bipolar; still bends pitch normally)
// "(Channel)" = a CHANNEL FX target: it acts on the whole instrument (both slots combined), not this slot.
// "Tone (retired)" = the removed Tone knob's reserved index - never offered, old routes inert.
// Sub/Formant sit ABOVE the grid block (MTSub/MTFormant) so their names come from kModTgtHiName.
static const char* kModTgtFixedName[DrumChannel::MT_GRID_BASE] =
{ "Off", "Filter 1 Cutoff", "Filter 1 Reso", "Filter 2 Cutoff", "Filter 2 Reso", "Drive",
  "Reverb Send", "Delay Send", "FX A Amount (Channel)", "Tone (retired)", "Punch", "FX B Amount (Channel)", "Attack", "Decay", "Sustain",
  "Release", "Pitch", "Wave Position", "Detune", "Vibrato", "Width", "Drift", "Volume", "Warp",
  "FX A Character (Channel)", "FX B Character (Channel)", "Ring" };
static juce::String kModTgtHiName(int t)   // fixed targets ABOVE the grid range
{ return t == DrumChannel::MTSub ? "Sub" : t == DrumChannel::MTFormant ? "Formant"
       : t == DrumChannel::MTChFxCAmt ? "FX C Amount (Channel)" : t == DrumChannel::MTChFxCChr ? "FX C Character (Channel)"
       : t == DrumChannel::MTRingHz ? "Ring Hz" : t == DrumChannel::MTSlotPan ? "Slot Pan"
       : t == DrumChannel::MTFilt1Env ? "Filter 1 Env Amount" : t == DrumChannel::MTFilt2Env ? "Filter 2 Env Amount"
       : t == DrumChannel::MTUniCount ? "Unison Count" : juce::String(); }
// ================================================================================================
// MOD SOURCE / TARGET TOOLTIPS [2026-07-13 22:50] (user: "we need tips like that as tooltips") -
// shown per ROW while hovering the RoutePicker lists. The env-target tips carry the crucial
// latch guidance; the zero-at-the-hit sources say so explicitly.
// ================================================================================================
static juce::String modSrcTip(int s)
{
    using DC = DrumChannel;
    switch (s)
    {
        case DC::MSOff:      return "No source - the route is off.";
        case DC::MSVel:      return "How hard the hit was (step velocity / key velocity). Constant for the whole note - great for envelope targets (Velocity -> Decay = the classic accent).";
        case DC::MSNote:     return "The played pitch vs middle C (+-24 st = +-100%). Constant per note - keytracking (Note -> Cutoff) or keyboard spread (Note -> Slot Pan).";
        case DC::MSAmpEnv:   return "This slot's own amp envelope, live. NOTE: it reads 0 at the very start of a hit, so it does nothing on envelope-time targets.";
        case DC::MSLfoFilt: case DC::MSLfoPitch: case DC::MSLfoVol: case DC::MSLfoWave:
            return juce::String("LFO ") + juce::String(s - DC::MSLfoFilt + 1) + " (draw/set it on the MOD visual; its Amount = depth). "
                   "AUDIO-RATE on pitch/volume/cutoff etc. - crank the speed for FM/AM timbres. "
                   "For ENVELOPE targets use FREE-RUN or the Random shape: a retriggered sine restarts at 0 every hit = latches nothing.";
        case DC::MSRandom:   return "A fresh random value per hit (0..1). The humanizer source: Random -> Decay on hats, Random -> Slot Pan for hit scatter.";
        case DC::MSModEnv:   return "The drawable A-H-D-S-R on the MOD visual's 4th tab. Reads 0 at the hit itself, so use it for movement DURING the note (filter sweeps), not for envelope-time targets.";
        case DC::MSStepModA: return "The drawable per-step lane (Mod A edit mode on the step grid). Each step holds its drawn value - parameter-locks, Elektron style.";
        case DC::MSStepModB: return "The second drawable per-step lane (Mod B edit mode).";
        case DC::MSModWheel: return "Your MIDI mod wheel (CC1; learnable to any CC via the MIDI dropdown). Holds its last position.";
        case DC::MSPressure: return "AFTERTOUCH: press INTO a held key. Per note on MPE controllers, whole-channel on normal keyboards. "
                                     "Reads 0 at the hit, so aim it at movement targets (Cutoff / Volume / Vibrato / Formant). "
                                     "Not moving? Your keyboard must SEND aftertouch (and have it enabled) - the top-bar MIDI monitor shows \"AT\" when it arrives.";
        case DC::MSPitchWheel: return "PITCH WHEEL, as a mod source (bipolar: centre = 0, up = +, down = -). The wheel STILL does its normal pitch bend - this route lets it drive anything else at the same time. Wire any CC to it via the MIDI dropdown -> selection controls.";
        case DC::MSSlide:    return "SLIDE: the MPE timbre dimension (vertical finger position; CC74 by default). Per note on MPE controllers. "
                                     "Wire ANY other CC to it via the MIDI dropdown -> \"MIDI-learn: selection controls...\" -> Slide.";
        default:             return {};
    }
}
static juce::String modTgtTip(int t, int engine)
{
    using DC = DrumChannel;
    const juce::String envNote = "ENVELOPE-TIME target: the source is sampled ONCE at the hit. "
        "Use Velocity / Random / Note / Step Mod / Mod Wheel / a FREE-RUN LFO - a retriggered LFO or "
        "an envelope source reads 0 at the hit and will do NOTHING.";
    switch (t)
    {
        case DC::MTOff:      return "No target - the route is off.";
        case DC::MTFilt1Cut: return "Filter 1 cutoff, +-4 octaves. AUDIO-RATE: fast LFOs = real filter-FM.";
        case DC::MTFilt2Cut: return "Filter 2 cutoff, +-4 octaves. AUDIO-RATE.";
        case DC::MTFilt1Res: return "Filter 1 resonance (on a Bell filter: its boost/Q).";
        case DC::MTFilt2Res: return "Filter 2 resonance (on a Bell filter: its boost/Q).";
        case DC::MTDrive:    return "Drive amount (the slot's distortion). AUDIO-RATE; eases to dry near zero.";
        case DC::MTRevSend:  return "This channel's reverb send (block-rate, smoothed).";
        case DC::MTDelSend:  return "This channel's delay send (block-rate, smoothed).";
        case DC::MTPunch:    return "Transient shaper: + snaps the attack, - softens it. Per hit.";
        case DC::MTSub:      return "The octave-down sub sine's level. AUDIO-RATE.";
        case DC::MTFormant:  return "Vowel position (A-E-I-O-U). Sweep it and the slot talks. Glided per block.";
        case DC::MTAtk:      return "Attack time. " + envNote;
        case DC::MTDec:      return "Decay time. Velocity -> Decay = the drum-machine accent. " + envNote;
        case DC::MTSus:      return "Sustain level (audible only on held keys / gated notes). " + envNote;
        case DC::MTRel:      return "Release time (audible at note END on held/gated notes). " + envNote;
        case DC::MTPitch:    return "Pitch, +-1 octave per full route. AUDIO-RATE: audio-speed LFOs fuse into real FM sidebands; KEY-synced LFOs keep the FM colour constant across the keyboard.";
        case DC::MTWavePos:  return "The Custom wave's frame position (A..D). AUDIO-RATE wavetable scanning." + juce::String(engine == DC::SrcOsc ? "" : " (Oscillator slots only.)");
        case DC::MTDetune:   return "Unison detune spread (block-rate).";
        case DC::MTVibrato:  return "Vibrato depth (block-rate).";
        case DC::MTWidth:    return "Unison stereo width (block-rate).";
        case DC::MTDrift:    return "Drift amount - the per-hit analog looseness (block-rate).";
        case DC::MTVol:      return "This slot's level. AUDIO-RATE: slow = tremolo, audio-speed = AM / ring-mod sidebands.";
        case DC::MTWarp:     return "The oscillator's wavefold amount. AUDIO-RATE, anti-aliased.";
        case DC::MTRing:     return "Ring modulator amount (dry -> ring-modulated). AUDIO-RATE.";
        case DC::MTRingHz:   return "The ring modulator's CARRIER frequency, +-3 octaves. AUDIO-RATE: sweeping it = sci-fi sideband sweeps; audio-rate sources = chaos (the good kind).";
        case DC::MTSlotPan:  return "This layer's pan. Note -> Pan = keyboard spread, Random -> Pan = per-hit scatter, Velocity -> Pan = accent lean. (Whole-channel MOTION = Channel FX Auto-Pan.)";
        case DC::MTFilt1Env: return "Filter 1's ENVELOPE AMOUNT (the dashed sweep arrow). Velocity -> Env Amount = harder hits sweep further (the 303 accent, squared). Live-shown on the filter display.";
        case DC::MTFilt2Env: return "Filter 2's ENVELOPE AMOUNT (the dashed sweep arrow).";
        case DC::MTUniCount: return "Unison VOICE COUNT (1..16). Fully CONTINUOUS: voices FADE in/out (~3 ms) as the count moves, and the stack's gain follows the effective count - swell a pad from 2 to 16 voices mid-note. Oscillator slots only (a Karplus-Strong string or Modal strike only exists if it was plucked - physics).";
        case DC::MTChFxAAmt: return "Channel FX slot A's Amount fader (block-rate, smoothed).";
        case DC::MTChFxBAmt: return "Channel FX slot B's Amount fader (block-rate, smoothed).";
        case DC::MTChFxCAmt: return "Channel FX slot C's Amount fader (block-rate, smoothed).";
        case DC::MTChFxAChr: return "Channel FX slot A's Character fader (block-rate, smoothed).";
        case DC::MTChFxBChr: return "Channel FX slot B's Character fader (block-rate, smoothed).";
        case DC::MTChFxCChr: return "Channel FX slot C's Character fader (block-rate, smoothed).";
        default:
            if (t >= DC::MT_GRID_BASE && t < DC::MT_GRID_END)
                return "One of this engine's own knobs (follows the engine, like the on-screen knob). Block-rate - except FM Amount, which is audio-rate.";
            return {};
    }
}

// Engine display name (matches the slot engine dropdown) for the mod-target parentheses "(Oscillator)" etc.
static juce::String engineDisplayName(int engine)
{
    static const char* eng[9] = { "Sample", "Noise", "Oscillator", "FM", "Karplus-Strong", "Synth", "Wavetable", "Modal", "Granular" };
    return (engine >= 0 && engine < 9) ? juce::String(eng[engine]) : juce::String();
}
// Which of the 4 LFOs (index 0..3) feed at least one mod-matrix route (for the LFO "not routed" hint).
static void lfoRoutedMask(const DrumChannel::Slot& sl, bool routed[4])
{
    for (int d = 0; d < 4; ++d) routed[d] = false;
    for (const auto& r : sl.mod)
        if (r.tgt != DrumChannel::MTOff && r.src >= DrumChannel::MSLfoFilt && r.src <= DrumChannel::MSLfoWave)
            routed[r.src - DrumChannel::MSLfoFilt] = true;
}

//==============================================================================
// RemapEditor [2026-07-14 00:03] - the per-route REMAP curve overlay implementation.
//==============================================================================
void RemapEditor::applyPreset(int i)
{
    if (i == 0) { on = false; for (int k = 0; k < N; ++k) curve[k] = (float) k / (float)(N - 1); }
    else
    {
        on = true;
        for (int k = 0; k < N; ++k)
        {
            const float x = (float) k / (float)(N - 1);
            switch (i)
            {
                case 1: curve[k] = x * x * x; break;                                    // SOFT: late response
                case 2: curve[k] = 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); break;  // HARD: early response
                case 3: curve[k] = x * x * (3.0f - 2.0f * x); break;                    // S-CURVE: smoothstep
                // [2026-07-14 01:20] the second row (user: "more ready made drawings"):
                case 4: curve[k] = x < 0.5f ? 0.0f : (x - 0.5f) * 2.0f; break;          // THRESHOLD: dead until half, then rise
                case 5: curve[k] = 1.0f - x; break;                                     // INVERT: high source = low output
                case 6: curve[k] = std::floor(x * 5.0f) / 4.0f; break;                  // STEPS: quantise into 5 stairs
                default: { const float b = 1.0f - std::abs(2.0f * x - 1.0f);            // BUMP: zero ends, full middle
                           curve[k] = b * b * (3.0f - 2.0f * b); } break;
            }
        }
    }
    if (onChange) onChange(curve, on);
    repaint();
}
void RemapEditor::drawAt(juce::Point<float> pos, bool connect)
{
    const auto r = strip();
    const int   i = juce::jlimit(0, N - 1, (int) std::floor((pos.x - r.getX()) / r.getWidth() * (float) N));
    const float v = juce::jlimit(0.0f, 1.0f, 1.0f - (pos.y - r.getY()) / r.getHeight());
    if (! on) on = true;   // drawing anything switches the remap ON
    if (connect && lastI >= 0 && lastI != i)
    {   const int lo = juce::jmin(lastI, i), hi = juce::jmax(lastI, i);
        const float v0 = curve[lastI];
        for (int k = lo; k <= hi; ++k)
            curve[k] = juce::jmap((float)(k - lo) / juce::jmax(1.0f, (float)(hi - lo)), lastI < i ? v0 : v, lastI < i ? v : v0);
    }
    else curve[i] = v;
    lastI = i;
    if (onChange) onChange(curve, on);
    repaint();
}
void RemapEditor::mouseDown(const juce::MouseEvent& e)
{
    if (closeRect().contains(e.position)) { setVisible(false); if (onClose) onClose(); return; }
    for (int i = 0; i < 8; ++i) if (presetRect(i).contains(e.position)) { applyPreset(i); return; }
    if (strip().contains(e.position)) drawAt(e.position, false);
}
void RemapEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (strip().contains(e.position) || lastI >= 0) drawAt(strip().getConstrainedPoint(e.position), true);
}
void RemapEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xf0141426));
    g.setColour(juce::Colour(0xff777fa8)); g.drawRect(getLocalBounds(), 1);
    g.setColour(juce::Colours::white); g.setFont(juce::Font(12.5f, juce::Font::bold));
    g.drawText(title, 10, 4, getWidth() - 40, 16, juce::Justification::centredLeft, false);
    g.setColour(juce::Colour(0xffaebada)); g.setFont(juce::Font(12.0f, juce::Font::bold));
    g.drawText("X", closeRect(), juce::Justification::centred, false);
    static const char* pn[8] = { "Linear", "Soft", "Hard", "S-Curve", "Threshold", "Invert", "Steps", "Bump" };
    for (int i = 0; i < 8; ++i)
    {   const auto pr = presetRect(i);
        const bool lit = (i == 0 && ! on);
        g.setColour(lit ? accent.withAlpha(0.30f) : juce::Colour(0xff20203a)); g.fillRoundedRectangle(pr, 3.0f);
        g.setColour(i == 0 ? juce::Colour(0xff9fd1ff) : juce::Colours::white);
        g.setFont(juce::Font(10.0f, lit ? juce::Font::bold : juce::Font::plain));
        g.drawText(pn[i], pr, juce::Justification::centred, false);
    }
    const auto r = strip();
    g.setColour(juce::Colour(0xff181830)); g.fillRoundedRectangle(r, 4.0f);
    // [2026-07-14 01:20] VALUE GRID (user idea): faint lines every 10%, brighter every 50%, numeric
    // labels at 0/50/100 only (per-10% numbers would be unreadable at this size).
    for (int i = 1; i < 10; ++i)
    {
        const float fx = r.getX() + r.getWidth() * (float) i / 10.0f;
        const float fy = r.getBottom() - r.getHeight() * (float) i / 10.0f;
        g.setColour(juce::Colour(i == 5 ? 0x30ffffff : 0x12ffffff));
        g.drawVerticalLine((int) fx, r.getY(), r.getBottom());
        g.drawHorizontalLine((int) fy, r.getX(), r.getRight());
    }
    g.setColour(juce::Colour(0xff707890)); g.setFont(juce::Font(8.5f));
    for (int i = 0; i <= 2; ++i)
    {
        const juce::String lb = juce::String(i * 50) + "%";
        g.drawText(lb, (int)(r.getX() + r.getWidth() * (float) i * 0.5f) - 12, (int) r.getBottom() + 1, 24, 9,
                   juce::Justification::centred, false);
        g.drawText(lb, (int) r.getX() - 1, (int)(r.getBottom() - r.getHeight() * (float) i * 0.5f) - 5, 24, 9,
                   juce::Justification::centredLeft, false);
    }
    // AXIS WATERMARKS (the PitchEnv convention, user request): drawn arrows + captions.
    g.setColour(juce::Colour(0x33ffffff));
    { juce::Path ax; ax.startNewSubPath(r.getX() + 6.0f, r.getBottom() - 6.0f);
      ax.lineTo(r.getX() + 78.0f, r.getBottom() - 6.0f);
      ax.lineTo(r.getX() + 73.0f, r.getBottom() - 9.0f); ax.startNewSubPath(r.getX() + 78.0f, r.getBottom() - 6.0f);
      ax.lineTo(r.getX() + 73.0f, r.getBottom() - 3.0f);
      ax.startNewSubPath(r.getX() + 6.0f, r.getBottom() - 6.0f); ax.lineTo(r.getX() + 6.0f, r.getY() + 66.0f);
      ax.lineTo(r.getX() + 3.0f, r.getY() + 71.0f); ax.startNewSubPath(r.getX() + 6.0f, r.getY() + 66.0f);
      ax.lineTo(r.getX() + 9.0f, r.getY() + 71.0f);
      g.strokePath(ax, juce::PathStrokeType(1.2f)); }
    g.setFont(juce::Font(9.0f, juce::Font::bold));
    g.drawText("Mod Source Amount", (int) r.getX() + 12, (int) r.getBottom() - 18, 130, 10, juce::Justification::centredLeft, false);
    { juce::Graphics::ScopedSaveState ss(g);
      g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, r.getX() + 16.0f, r.getY() + 80.0f));
      g.drawText("What The Target Receives", (int) r.getX() - 46, (int) r.getY() + 74, 150, 10, juce::Justification::centredLeft, false); }
    g.setColour(juce::Colour(0x22ffffff));
    g.drawLine(r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);            // pass-through diagonal
    if (bipolarSrc) g.drawVerticalLine((int) r.getCentreX(), r.getY(), r.getBottom());   // source-at-rest marker
    juce::Path pth;
    for (int k = 0; k < N; ++k)
    {   const float x = r.getX() + r.getWidth() * (float) k / (float)(N - 1);
        const float y = r.getBottom() - r.getHeight() * juce::jlimit(0.0f, 1.0f, curve[k]);
        if (k == 0) pth.startNewSubPath(x, y); else pth.lineTo(x, y);
    }
    g.setColour(on ? accent : accent.withAlpha(0.35f));
    g.strokePath(pth, juce::PathStrokeType(on ? 2.0f : 1.4f));
    if (! on)
    { g.setColour(juce::Colour(0xff8890b0)); g.setFont(juce::Font(11.0f));
      g.drawText("pass-through - draw to remap", r, juce::Justification::centred, false); }
}

//==============================================================================
// RoutePicker: two-column SOURCE | TARGET chooser for ONE mod route (right-click a matrix fader).
//==============================================================================
void RoutePicker::Col::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= (int) rows.size()) return;
    const bool cur = (isSrc ? owner.curSrc : owner.curTgt) == rows[(size_t) row].first;
    g.setColour(cur ? owner.accent.withAlpha(0.30f) : (sel ? juce::Colour(0xff262640) : juce::Colour(0xff181828)));
    g.fillRect(0, 0, w, h);
    g.setColour(cur ? owner.accent : juce::Colours::white);
    g.setFont(juce::Font(12.0f, cur ? juce::Font::bold : juce::Font::plain));
    g.drawText(rows[(size_t) row].second, 8, 0, w - 10, h, juce::Justification::centredLeft, true);
}
void RoutePicker::Col::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= (int) rows.size()) return;
    (isSrc ? owner.curSrc : owner.curTgt) = rows[(size_t) row].first;
    if (owner.onPicked) owner.onPicked(owner.curSrc, owner.curTgt);
    owner.srcList.repaint(); owner.tgtList.repaint();
}
RoutePicker::RoutePicker()
{
    addAndMakeVisible(srcList); addAndMakeVisible(tgtList);
    srcList.setModel(&srcModel); tgtList.setModel(&tgtModel);
    srcList.setRowHeight(17); tgtList.setRowHeight(17);
    for (auto* l : { &srcList, &tgtList })
    { l->setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff181828));
      l->setColour(juce::ListBox::outlineColourId, juce::Colour(0xff33335a)); l->setOutlineThickness(1); }
    lblSrc.setText("SOURCE", juce::dontSendNotification); lblTgt.setText("TARGET", juce::dontSendNotification);
    for (auto* lb : { &lblSrc, &lblTgt })
    { lb->setJustificationType(juce::Justification::centred); lb->setFont(juce::Font(11.0f, juce::Font::bold));
      lb->setColour(juce::Label::textColourId, juce::Colour(0xffaeb8d4)); addAndMakeVisible(lb); }
}
void RoutePicker::visibilityChanged()
{
    if (isVisible() && ! hooked)      { juce::Desktop::getInstance().addGlobalMouseListener(&closer); hooked = true; }
    else if (! isVisible() && hooked) { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); hooked = false; }
}
void RoutePicker::openFor(int cs, int ct, float amt, float lagMs)
{
    curSrc = cs; curTgt = ct; curAmt = juce::jlimit(-1.0f, 1.0f, amt); curLag = juce::jlimit(0.0f, 2000.0f, lagMs);
    srcModel.rows.clear();
    for (int i = 0; i < DrumChannel::MS_COUNT; ++i)
    { if (i == DrumChannel::MSLfoWave || i == DrumChannel::MSModLfo) continue;   // LFO 4 slot is the Mod Env tab now; Mod LFO dropped
      srcModel.rows.push_back({ i, juce::String(kModSrcName[i]) }); }
    tgtModel.rows.clear();
    for (int t = 0; t < DrumChannel::MT_GRID_BASE; ++t)
    {   // ENGINE-GATED targets: only offer what this slot's engine can actually do (no dead routes).
        if (t == DrumChannel::MTTone) continue;                                       // RETIRED (Tone removed)
        if (t == DrumChannel::MTWarp    && engine != DrumChannel::SrcOsc) continue;   // wavefold = Oscillator only
        if (t == DrumChannel::MTWavePos && engine != DrumChannel::SrcOsc) continue;   // Osc wavetable frame only (granular position = the "Position" grid knob)
        if (t == DrumChannel::MTUniCount && engine != DrumChannel::SrcOsc) continue;  // [2026-07-14 02:50] count-morph = Osc only (a KS string / Modal strike can't join mid-note; Grain/Noise/Sample have no unison)
        juce::String nm = kModTgtFixedName[t];
        if (t == DrumChannel::MTWarp || t == DrumChannel::MTWavePos) nm << " (Oscillator)";   // engine-specific fixed target = say which engine (user)
        tgtModel.rows.push_back({ t, nm });
    }
    // GRID knobs = the current engine's OWN params. Label them "<param> (<Engine>)", not "Knob N" (user).
    const juce::String eng = engineDisplayName(engine);
    for (int i = 0; i < DrumChannel::MOD_TGT_GRID; ++i)
    { const juce::String nm = gridKnobName ? gridKnobName(i) : juce::String();
      if (nm.isNotEmpty()) tgtModel.rows.push_back({ DrumChannel::MT_GRID_BASE + i,
                              eng.isNotEmpty() ? (nm + " (" + eng + ")") : nm }); }
    // Fixed targets ABOVE the grid (appended so saved grid routes never shift): Sub + Formant.
    tgtModel.rows.push_back({ DrumChannel::MTSub,     "Sub" });
    tgtModel.rows.push_back({ DrumChannel::MTFormant, "Formant" });
    tgtModel.rows.push_back({ DrumChannel::MTChFxCAmt, "FX C Amount (Channel)" });
    tgtModel.rows.push_back({ DrumChannel::MTChFxCChr, "FX C Character (Channel)" });
    tgtModel.rows.push_back({ DrumChannel::MTRingHz,  "Ring Hz" });    // [2026-07-13 22:45] user request
    tgtModel.rows.push_back({ DrumChannel::MTSlotPan, "Slot Pan" });
    tgtModel.rows.push_back({ DrumChannel::MTFilt1Env, "Filter 1 Env Amount" });   // [2026-07-14 00:30]
    tgtModel.rows.push_back({ DrumChannel::MTFilt2Env, "Filter 2 Env Amount" });
    tgtModel.rows.push_back({ DrumChannel::MTUniCount, "Unison Count" });
    // ALPHABETICAL, "Off" pinned first (user) - one flat A..Z list per column, so the three
    // FX A/B/C pairs cluster together instead of FX C hiding under the engine-knob rows.
    auto alphabetize = [](std::vector<std::pair<int, juce::String>>& rows) {
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.first == 0) return true; if (b.first == 0) return false;   // Off stays on top
            return a.second.compareIgnoreCase(b.second) < 0; });
    };
    alphabetize(srcModel.rows); alphabetize(tgtModel.rows);
    srcList.updateContent(); tgtList.updateContent();
    selectCurrentRows();
    setVisible(true); toFront(true);
}
juce::String RoutePicker::getTooltip()
{   // rows serve their own tips via Col::getTooltipForRow; the picker itself tips the fader + remap row
    const auto p = getMouseXYRelative().toFloat();
    if (lagRect().contains(p))
        return "LAG: the source GLIDES toward its value over this time instead of jumping (0 = instant).\n\n"
               "- The states reset at every hit, so the source SWELLS IN from zero - the \"aftertouch "
               "shouldn't fire on the strike\" delay, done properly in the time domain.\n"
               "- Composes with the map below: LAG = how fast it responds, MAP = how hard you must push.\n"
               "- NOTE: envelope-time targets sample at the hit, so a lagged source reads ~0 there.";
    if (remapRect().contains(p))
        return "MODULATION AMOUNT MAP: draw a transfer curve for THIS route.\n\n"
               "- X = the source's value (bipolar sources like LFOs/Note span -1..+1, centre = at rest); "
               "Y = what actually reaches the target.\n"
               "- Presets inside: Linear = off (pass-through), Soft = late response (tame aftertouch), "
               "Hard = early, S-Curve = smooth switch.\n"
               "- Each route remembers its own curve - the same source can act differently per target.";
    if (midiRect().contains(p))
        return "MIDI: assign a CC (knob/fader) to THIS route's AMOUNT.\n\n"
               "- Acts on the CURRENT selection (pattern / channel / slot) - it always drives this "
               "route number on whatever slot is selected.\n"
               "- Bipolar: the CC's centre = 0, above = positive, below = negative (same cubic "
               "response as the on-screen fader).";
    if (amtRect().contains(p))
        return "Route AMOUNT: centre = 0, right = positive, left = negative (cubic response - fine control near zero). Double-click = 0.";
    return {};
}

juce::String RoutePicker::Col::getTooltipForRow(int row)
{
    if (row < 0 || row >= (int) rows.size()) return {};
    return isSrc ? modSrcTip(rows[(size_t) row].first)
                 : modTgtTip(rows[(size_t) row].first, owner.engine);
}

void RoutePicker::selectCurrentRows()
{
    for (int r = 0; r < (int) srcModel.rows.size(); ++r) if (srcModel.rows[(size_t) r].first == curSrc) { srcList.selectRow(r); break; }
    for (int r = 0; r < (int) tgtModel.rows.size(); ++r) if (tgtModel.rows[(size_t) r].first == curTgt) { tgtList.selectRow(r); break; }
}
void RoutePicker::resized()
{
    auto b = getLocalBounds().reduced(6);
    auto hdr = b.removeFromTop(15);
    lblSrc.setBounds(hdr.removeFromLeft(hdr.getWidth() / 2));
    lblTgt.setBounds(hdr);
    b.removeFromBottom(26 + 6 + 24);   // reserve the AMOUNT fader + the REMAP row above it [2026-07-14]
    auto left = b.removeFromLeft(b.getWidth() / 2).reduced(2, 0);
    srcList.setBounds(left);
    tgtList.setBounds(b.reduced(2, 0));
}
// Amount fader response = CUBIC (user): the fader starts slow (fine sub-1% control near the
// centre) and accelerates toward the ends. pos = the linear finger position -1..1; amt = pos^3
// (sign-preserving). Drawing inverts it (cbrt) so the handle tracks the finger, not the value.
static float modAmtFromPos(float p) { p = juce::jlimit(-1.0f, 1.0f, p); return p * p * p; }
static float modPosFromAmt(float a) { return std::cbrt(juce::jlimit(-1.0f, 1.0f, a)); }
void RoutePicker::setAmtFromX(float x)
{
    auto fr = amtRect();
    const float cx = fr.getCentreX(), half = fr.getWidth() * 0.5f - 4.0f;
    float p = juce::jlimit(-1.0f, 1.0f, (x - cx) / juce::jmax(1.0f, half));
    if (std::abs(p) < 0.02f) p = 0.0f;             // snap the finger to dead-centre = exactly 0
    curAmt = modAmtFromPos(p); if (onAmt) onAmt(curAmt); repaint();
}
void RoutePicker::mouseDown(const juce::MouseEvent& e)
{
    if (remapRect().contains(e.position)) { if (onRemap) onRemap(); return; }   // open the MOD AMOUNT MAP editor
    if (midiRect().contains(e.position))  { if (onMidi)  onMidi();  return; }   // MIDI-assign this route's amount [2026-07-14 11:10]
    lagDrag = lagRect().contains(e.position); if (lagDrag) { setLagFromX(e.position.x); return; }
    amtDrag = amtRect().contains(e.position); if (amtDrag) setAmtFromX(e.position.x);
}
void RoutePicker::setLagFromX(float x)
{   // [2026-07-14 01:33] LAG fader: hard left = instant (0), then log 5..1000 ms across the travel
    const auto r = lagRect();
    const float p = juce::jlimit(0.0f, 1.0f, (x - r.getX()) / juce::jmax(1.0f, r.getWidth()));
    curLag = p <= 0.02f ? 0.0f : 5.0f * std::pow(200.0f, (p - 0.02f) / 0.98f);
    if (onLag) onLag(curLag);
    repaint();
}
void RoutePicker::mouseDrag(const juce::MouseEvent& e) { if (lagDrag) setLagFromX(e.position.x); else if (amtDrag) setAmtFromX(e.position.x); }
void RoutePicker::mouseUp(const juce::MouseEvent&) { if ((amtDrag || lagDrag) && onAmtDragEnd) onAmtDragEnd(); amtDrag = lagDrag = false; }
void RoutePicker::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff11111c)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(accent.withAlpha(0.6f)); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.75f), 6.0f, 1.5f);
    // [2026-07-14 01:33] LAG fader (left half, under SOURCES): 0 = instant, log 5..1000 ms.
    {
        const auto lr2 = lagRect();
        g.setColour(juce::Colour(0xff181828)); g.fillRoundedRectangle(lr2, 4.0f);
        const float p = curLag <= 0.01f ? 0.0f : 0.02f + 0.98f * std::log(curLag / 5.0f) / std::log(200.0f);
        if (p > 0.001f)
        { g.setColour(juce::Colour(0xff35c0ff).withAlpha(0.30f));
          g.fillRoundedRectangle(lr2.withWidth(juce::jmax(3.0f, lr2.getWidth() * juce::jlimit(0.0f, 1.0f, p))).reduced(1.5f), 3.0f);
          g.setColour(juce::Colour(0xff35c0ff));
          g.fillRect(lr2.getX() + lr2.getWidth() * juce::jlimit(0.0f, 1.0f, p) - 1.2f, lr2.getY() + 3.0f, 2.4f, lr2.getHeight() - 6.0f); }
        g.setColour(juce::Colours::white); g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText(curLag <= 0.01f ? juce::String("LAG  Off")
                                   : "LAG  " + juce::String(juce::roundToInt(curLag)) + " ms",
                   lr2, juce::Justification::centred, false);
    }
    // AMOUNT fader (bipolar, centre = 0; right half, under TARGETS) with the exact % written IN it.
    auto fr = amtRect();
    g.setColour(juce::Colour(0xff181828)); g.fillRoundedRectangle(fr, 4.0f);
    const float cx = fr.getCentreX(), half = fr.getWidth() * 0.5f - 4.0f, px = cx + modPosFromAmt(curAmt) * half;
    if (std::abs(curAmt) > 1.0e-4f)
    { g.setColour(accent.withAlpha(0.32f)); g.fillRect(juce::Rectangle<float>(juce::jmin(cx, px), fr.getY() + 3.0f, std::abs(px - cx), fr.getHeight() - 6.0f));
      g.setColour(accent); g.fillRect(px - 1.2f, fr.getY() + 3.0f, 2.4f, fr.getHeight() - 6.0f); }
    g.setColour(juce::Colour(0xff4a4a66)); g.fillRect(cx - 0.5f, fr.getY() + 4.0f, 1.0f, fr.getHeight() - 8.0f);   // 0 tick
    g.setColour(juce::Colours::white); g.setFont(juce::Font(11.0f, juce::Font::bold));
    const float pct = curAmt * 100.0f;   // sub-10% shows one decimal so the fine sub-1% range is readable
    const juce::String pctS = std::abs(pct) < 9.95f ? juce::String(pct, 1) : juce::String(juce::roundToInt(pct));
    g.drawText("AMOUNT  " + juce::String(curAmt >= 0 ? "+" : "") + pctS + "%", fr, juce::Justification::centred, false);
    // REMAP row [2026-07-14]: click = draw this route's transfer curve (Vital-style).
    const auto rr = remapRect();
    g.setColour(remapOn ? accent.withAlpha(0.22f) : juce::Colour(0xff181828)); g.fillRoundedRectangle(rr, 4.0f);
    g.setColour(remapOn ? accent : juce::Colour(0xff777fa8)); g.drawRoundedRectangle(rr.reduced(0.5f), 4.0f, 1.0f);
    { juce::Path curveGlyph;   // a small S-mark so the row reads as "curve", never unicode
      const float gx = rr.getX() + 8.0f, gy = rr.getCentreY();
      curveGlyph.startNewSubPath(gx, gy + 4.0f);
      curveGlyph.cubicTo(gx + 5.0f, gy + 4.0f, gx + 5.0f, gy - 4.0f, gx + 10.0f, gy - 4.0f);
      g.strokePath(curveGlyph, juce::PathStrokeType(1.4f)); }
    g.setFont(juce::Font(11.0f, remapOn ? juce::Font::bold : juce::Font::plain));
    g.setColour(remapOn ? juce::Colours::white : juce::Colour(0xffaebada));
    g.drawText(remapOn ? "Amount map: ON - click to edit" : "Amount map: off - click to draw",
               rr.withTrimmedLeft(22.0f), juce::Justification::centredLeft, false);
    // MIDI-assign button [2026-07-14 11:10]: right of the map row - learn a CC to this route's AMOUNT.
    const auto mr = midiRect();
    const bool assigned = midiTag.length() > 4;   // "MIDI cc44" = assigned, plain "MIDI" = not
    g.setColour(assigned ? juce::Colour(0xff223046) : juce::Colour(0xff181828)); g.fillRoundedRectangle(mr, 4.0f);
    g.setColour(assigned ? juce::Colour(0xff9fd1ff) : juce::Colour(0xff777fa8)); g.drawRoundedRectangle(mr.reduced(0.5f), 4.0f, 1.0f);
    g.setFont(juce::Font(11.0f, assigned ? juce::Font::bold : juce::Font::plain));
    g.setColour(assigned ? juce::Colour(0xff9fd1ff) : juce::Colour(0xffaebada));
    g.drawText(midiTag, mr, juce::Justification::centred, false);
}

//==============================================================================
// ModFaderMatrix: 12 route faders (6 rows x 2 cols) - the whole matrix, inline.
//==============================================================================
juce::String ModFaderMatrix::routeSrcName(int r) const
{ const int s = src[r]; return (s > 0 && s < DrumChannel::MS_COUNT) ? juce::String(kModSrcName[s]) : juce::String(); }
juce::String ModFaderMatrix::routeTgtName(int r) const
{
    const int t = tgt[r];
    if (t <= 0) return {};
    if (t < DrumChannel::MT_GRID_BASE) return juce::String(kModTgtFixedName[t]);   // fixed targets = short name (no engine parens on the fader)
    if (t >= DrumChannel::MT_GRID_END) return kModTgtHiName(t);                    // Sub / Formant (above the grid)
    const juce::String nm = gridKnobName ? gridKnobName(t - DrumChannel::MT_GRID_BASE) : juce::String();   // grid = the param's own name
    return nm.isNotEmpty() ? nm : ("Knob " + juce::String(t - DrumChannel::MT_GRID_BASE + 1));
}
juce::Rectangle<float> ModFaderMatrix::faderRect(int i) const
{
    const int col = i / 6, row = i % 6;               // routes 0-5 = left column, 6-11 = right
    const float pad = 4.0f, top = 15.0f;
    const float cw = (getWidth() - pad * 3.0f) * 0.5f;
    const float ch = (getHeight() - top - pad) / 6.0f;
    return { pad + col * (cw + pad), top + row * ch, cw, ch - 2.0f };
}
int ModFaderMatrix::faderAt(juce::Point<float> p) const
{ for (int i = 0; i < NR; ++i) if (faderRect(i).contains(p)) return i; return -1; }
void ModFaderMatrix::setAmtFromX(int r, float x)
{
    auto fr = faderRect(r);
    const float cx = fr.getCentreX(), half = fr.getWidth() * 0.5f - 4.0f;
    float p = juce::jlimit(-1.0f, 1.0f, (x - cx) / juce::jmax(1.0f, half));
    if (std::abs(p) < 0.02f) p = 0.0f;                // snap the finger to dead-centre (0)
    amt[r] = modAmtFromPos(p); if (onChange) onChange(); repaint();
}
void ModFaderMatrix::setValues(const DrumChannel::Slot& sl)
{
    for (int r = 0; r < NR; ++r)
    { src[r] = juce::jlimit(0, DrumChannel::MS_COUNT - 1, (int) sl.mod[r].src);
      tgt[r] = juce::jlimit(0, DrumChannel::MT_COUNT - 1, (int) sl.mod[r].tgt);
      amt[r] = juce::jlimit(-1.0f, 1.0f, sl.mod[r].amt);
      cvOn[r] = sl.mod[r].curveOn != 0; }
    repaint();
}
void ModFaderMatrix::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff11111c)); g.fillRoundedRectangle(b, 5.0f);
    g.setColour(accent.withAlpha(0.4f)); g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);
    g.setColour(accent); g.setFont(juce::Font(9.5f, juce::Font::bold));
    g.drawText("MATRIX - SLOT " + juce::String(slotIdx + 1), 8, 1, getWidth() - 16, 12, juce::Justification::centredLeft, false);

    for (int i = 0; i < NR; ++i)
    {
        auto fr = faderRect(i);
        const bool active = (src[i] != DrumChannel::MSOff && tgt[i] != DrumChannel::MTOff);
        g.setColour(juce::Colour(0xff181828)); g.fillRoundedRectangle(fr, 3.0f);
        const float cx = fr.getCentreX(), half = fr.getWidth() * 0.5f - 4.0f;
        if (active && std::abs(amt[i]) > 1.0e-4f)
        {   // bipolar fill from the centre + a bright thumb (handle in cube-root space = tracks the finger)
            const float px = cx + modPosFromAmt(amt[i]) * half;
            g.setColour(accent.withAlpha(0.32f));
            g.fillRect(juce::Rectangle<float>(juce::jmin(cx, px), fr.getY() + 2.0f, std::abs(px - cx), fr.getHeight() - 4.0f));
            g.setColour(accent); g.fillRect(px - 1.0f, fr.getY() + 2.0f, 2.0f, fr.getHeight() - 4.0f);
        }
        g.setColour(juce::Colour(0xff4a4a66)); g.fillRect(cx - 0.5f, fr.getY() + 3.0f, 1.0f, fr.getHeight() - 6.0f);  // 0 tick
        if (active)
        {   // "Source  [->]  Target" with a drawn arrow (never a unicode glyph)
            g.setFont(juce::Font(9.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            auto ti = fr.reduced(5.0f, 0.0f);
            const float aw = 10.0f, hw = ti.getWidth() * 0.5f;
            g.drawText(routeSrcName(i), ti.withWidth(hw - aw * 0.5f), juce::Justification::centredRight, true);
            g.drawText(routeTgtName(i), ti.withTrimmedLeft(hw + aw * 0.5f), juce::Justification::centredLeft, true);
            const float ax = fr.getCentreX(), ay = fr.getCentreY();
            juce::Path ar; ar.addTriangle(ax - 3.0f, ay - 3.0f, ax + 3.0f, ay, ax - 3.0f, ay + 3.0f);
            g.setColour(accent); g.fillPath(ar);
            if (cvOn[i])
            {   // [2026-07-14] REMAP glyph: a tiny S-curve mark bottom-right = this route is remapped
                juce::Path cg; const float gx = fr.getRight() - 12.0f, gy = fr.getBottom() - 5.0f;
                cg.startNewSubPath(gx, gy);
                cg.cubicTo(gx + 3.0f, gy, gx + 3.0f, gy - 5.0f, gx + 6.0f, gy - 5.0f);
                g.setColour(juce::Colour(0xff35c0ff)); g.strokePath(cg, juce::PathStrokeType(1.3f));
            }
        }
        else
        {
            g.setColour(juce::Colour(0xff5a5f74)); g.setFont(juce::Font(8.5f));
            g.drawText("right-click to route", fr, juce::Justification::centred, false);
        }
    }
}
void ModFaderMatrix::mouseDown(const juce::MouseEvent& e)
{
    const int i = faderAt(e.position);
    if (i < 0) return;
    if (e.mods.isPopupMenu()) { if (onPickRoute) onPickRoute(i); return; }
    dragRoute = i;
    if (src[i] != DrumChannel::MSOff && tgt[i] != DrumChannel::MTOff) setAmtFromX(i, e.position.x);
}
void ModFaderMatrix::mouseDrag(const juce::MouseEvent& e)
{ if (dragRoute >= 0 && src[dragRoute] != DrumChannel::MSOff && tgt[dragRoute] != DrumChannel::MTOff) setAmtFromX(dragRoute, e.position.x); }
void ModFaderMatrix::mouseUp(const juce::MouseEvent&)
{ const bool dragged = (dragRoute >= 0); dragRoute = -1; if (dragged && onDragEnd) onDragEnd(); }   // auto-audition on release
void ModFaderMatrix::mouseDoubleClick(const juce::MouseEvent& e)
{ const int i = faderAt(e.position); if (i >= 0) { amt[i] = 0.0f; if (onChange) onChange(); if (onDragEnd) onDragEnd(); repaint(); } }

juce::String WaveMorphDisplay::getTooltip()
{
    return "The oscillator's waveform (the picture is the real tone). Pick the wave with the WAVE fader above; "
           "the FM Depth/Ratio/Feedback + Warp reshape it live here.";
}

void WavetableDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
    auto in = b.reduced(6.0f, 4.0f);
    const float cy = in.getCentreY(), hh = in.getHeight() * 0.5f - 1.0f;
    g.setColour(juce::Colour(0xff2a2a44)); g.drawHorizontalLine((int) cy, in.getX(), in.getRight());

    auto* s = getSlot ? getSlot() : nullptr;
    if (s == nullptr) return;
    const int   table = s->waveTable;
    const float pos   = juce::jlimit(0.0f, 1.0f, s->wavePos);
    const int   NP    = 220;
    auto drawWave = [&](float scanPos, juce::Colour col, float thick) {
        juce::Path path;
        for (int i = 0; i < NP; ++i) {
            const float ph = (float) i / (float) (NP - 1);
            const float v  = DrumChannel::wavetableSample(table, scanPos, ph);
            const float x  = in.getX() + ph * in.getWidth();
            const float y  = cy - juce::jlimit(-1.0f, 1.0f, v) * hh;
            if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
        }
        g.setColour(col); g.strokePath(path, juce::PathStrokeType(thick));
    };
    // faint neighbour frames behind, then the current one bright
    drawWave(juce::jlimit(0.0f, 1.0f, pos - 0.16f), juce::Colour(0x3344c08a), 1.0f);
    drawWave(juce::jlimit(0.0f, 1.0f, pos + 0.16f), juce::Colour(0x33ffc23a), 1.0f);
    drawWave(pos, juce::Colour(0xff35c0ff), 1.8f);

    // table name + scan readout under the wave
    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.setColour(juce::Colour(0xff8aa0c8));
    g.drawText(DrumChannel::wavetableName(table), in.removeFromBottom(12.0f),
               juce::Justification::centredLeft, false);
}

//==============================================================================
// StringDisplay - the Physical engine's interactive string. The drawn PLUCK SHAPE is the
// parameters: apex X = strike Position (a real plucked string displaces as a triangle with
// its apex at the pluck point), apex height = Tone (brightness), jaggedness = Stiffness.
// Drag the dot (X = Position, Y = Tone). Static unless a control changes.
//==============================================================================
void StringDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022));      g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
    auto in = b.reduced(10.0f, 5.0f);

    auto* s = getSlot ? getSlot() : nullptr;
    const float pos   = s ? juce::jlimit(0.02f, 0.98f, s->physPosition < 0.02f ? 0.02f : s->physPosition) : 0.25f;
    const float tone  = s ? juce::jlimit(0.0f, 1.0f, s->physTone)  : 0.5f;
    const float stiff = s ? juce::jlimit(0.0f, 1.0f, s->physStiff) : 0.0f;

    const float baseY = in.getBottom() - 4.0f;               // resting string line
    const float apexH = (0.25f + 0.75f * tone) * (in.getHeight() - 10.0f);   // Tone = how high the pluck sits

    // Bridge posts + resting string (dim).
    g.setColour(juce::Colour(0xff2a2a44));
    g.fillRect(in.getX() - 3.0f, baseY - 8.0f, 3.0f, 12.0f);
    g.fillRect(in.getRight(),    baseY - 8.0f, 3.0f, 12.0f);
    g.drawLine(in.getX(), baseY, in.getRight(), baseY, 1.0f);

    // The plucked string: a triangle apexed at Position, roughened by Stiffness.
    juce::Colour col = juce::Colour(0xff35c0ff);
    if (auto* pa = findParentComponentOfClass<SlotEditor>()) col = pa->accent;   // slot identity colour
    col = col.withMultipliedBrightness(0.55f + 0.45f * tone);                    // Tone also brightens it
    juce::Path p; const int NP = 90;
    for (int i = 0; i < NP; ++i)
    {
        const float fx = (float) i / (float) (NP - 1);
        float disp = apexH * (fx < pos ? fx / pos : (1.0f - fx) / (1.0f - pos));
        if (stiff > 0.001f && i > 0 && i < NP - 1)                               // stiffness = jagged/bar-like
            disp += stiff * 2.6f * std::sin(fx * 55.0f) * (disp / juce::jmax(1.0f, apexH));
        const float x = in.getX() + fx * in.getWidth();
        const float y = baseY - disp;
        if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
    }
    g.setColour(col); g.strokePath(p, juce::PathStrokeType(1.8f));

    // Drag handle at the apex.
    const float hx = in.getX() + pos * in.getWidth(), hy = baseY - apexH;
    g.setColour(col);                 g.fillEllipse(hx - 4.0f, hy - 4.0f, 8.0f, 8.0f);
    g.setColour(juce::Colours::white); g.drawEllipse(hx - 4.0f, hy - 4.0f, 8.0f, 8.0f, 1.2f);

    // Read-out (brighter while dragging).
    g.setFont(juce::Font(9.5f, juce::Font::bold));
    g.setColour(dragging ? juce::Colours::white : juce::Colour(0xff8aa0c8));
    g.drawText("Pos " + juce::String(juce::roundToInt((s ? s->physPosition : 0.0f) * 100)) + "%  Tone "
                   + juce::String(juce::roundToInt(tone * 100)) + "%",
               getLocalBounds().reduced(5, 2), juce::Justification::topRight, false);
}

void StringDisplay::mouseDown(const juce::MouseEvent& e) { dragging = true; mouseDrag(e); }
void StringDisplay::mouseDrag(const juce::MouseEvent& e)
{
    auto* s = getSlot ? getSlot() : nullptr;
    if (! s) return;
    auto in = getLocalBounds().toFloat().reduced(10.0f, 5.0f);
    s->physPosition = juce::jlimit(0.0f, 1.0f, (e.position.x - in.getX()) / juce::jmax(1.0f, in.getWidth()));
    s->physTone     = juce::jlimit(0.0f, 1.0f, 1.0f - (e.position.y - in.getY()) / juce::jmax(1.0f, in.getHeight()));
    if (onEdit) onEdit();
    repaint();
}
void StringDisplay::mouseUp(const juce::MouseEvent&)
{ if (dragging) { dragging = false; repaint(); if (onDragEnd) onDragEnd(); } }

juce::String StringDisplay::getTooltip()
{
    return "The string, drawn as its real pluck shape. DRAG the dot: LEFT/RIGHT = strike Position "
           "(plucking near the edge = full/bright, near a harmonic node = hollow/nasal), UP/DOWN = Tone "
           "(how much high end rings). Jaggedness shows Stiffness.";
}

//==============================================================================
// ModalDisplay - the Modal engine's interactive controller, same philosophy as the
// Physical string: THE DRAWN SHAPE IS THE SOUND. It plots the struck body's actual
// standing wave - the sum of the REAL mode shapes (sin((i+1)*pi*x)) weighted by the
// REAL gains the DSP uses (material gains x Tone tilt x the Hit-position comb x Morph).
// So Material/Tone/Morph change the curve itself, Hit visibly combs modes out of it,
// and Damp flattens it (a muted body barely moves). ONE consistent picture for every
// material - no per-material cartoon outlines.
// Drag the mallet dot: X = Hit Position over the FULL width (a body is symmetric, so
// both edges = edge strike, the middle = centre strike), Y = Damp (down = muted).
//==============================================================================
void ModalDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
    auto in = b.reduced(10.0f, 6.0f);

    auto* s = getSlot ? getSlot() : nullptr;
    const int   mat  = s ? juce::jlimit(0, DrumChannel::modalMaterialCount() - 1, s->modalMaterial) : 0;
    const float hit  = s ? juce::jlimit(0.0f, 1.0f, s->modalHit)   : 0.0f;
    const float damp = s ? juce::jlimit(0.0f, 1.0f, s->modalDamp)  : 0.0f;
    const float tone = s ? juce::jlimit(0.0f, 1.0f, s->modalTone)  : 0.5f;
    const float morph= s ? juce::jlimit(0.0f, 1.0f, s->modalMorph) : 0.0f;

    juce::Colour col = juce::Colour(0xff35c0ff);
    if (auto* pa = findParentComponentOfClass<SlotEditor>()) col = pa->accent;

    // Reconcile the mallet position with the slot's hit (symmetric mapping keeps the side
    // the user last dragged; only reset if the value changed elsewhere).
    if (dotX < 0.0f || std::abs((1.0f - std::abs(2.0f * dotX - 1.0f)) - hit) > 0.02f)
        dotX = hit * 0.5f;

    // Per-mode gains, EXACTLY like the DSP: material gain (morph-blended), Tone tilt,
    // Hit-position comb. These weight the mode shapes below.
    const int nA = DrumChannel::modalModeCount(mat);
    const int nxt = juce::jmin(mat + 1, DrumChannel::modalMaterialCount() - 1);
    const int nB = DrumChannel::modalModeCount(nxt);
    const int n  = (morph > 0.001f) ? juce::jmax(nA, nB) : nA;
    float gains[DrumChannel::MODAL_MODES] = {};
    const float hitPos = 0.5f * hit;
    for (int i = 0; i < n && i < DrumChannel::MODAL_MODES; ++i)
    {
        const float gA = DrumChannel::modalModeGain(mat, i);
        const float gB = DrumChannel::modalModeGain(nxt, i);
        float gv = gA + (gB - gA) * morph;
        const float hi = (float) i / (float) juce::jmax(1, n - 1);
        gv *= (0.35f + 0.65f * (tone * 0.5f + 0.5f) * (1.0f - 0.5f * hi * (1.0f - tone)));   // Tone tilt (DSP formula)
        const float comb = std::abs(std::sin((float) (i + 1) * juce::MathConstants<float>::pi * hitPos));
        gv *= (1.0f - hit) + hit * comb;                                                     // Hit comb (DSP formula)
        gains[i] = gv;
    }

    // The standing wave: u(x) = sum_i gains[i] * sin((i+1) * pi * x), pinned at both ends
    // like a real bar/membrane cross-section. Damp scales it flat.
    const float cy = in.getCentreY() + 2.0f;
    const float amp = (in.getHeight() * 0.5f - 8.0f) * (1.0f - 0.85f * damp);
    const int NP = 110;
    float u[NP]; float pk = 1.0e-4f;
    for (int k = 0; k < NP; ++k)
    {
        const float x = (float) k / (float) (NP - 1);
        float v = 0.0f;
        for (int i = 0; i < n && i < DrumChannel::MODAL_MODES; ++i)
            v += gains[i] * std::sin((float) (i + 1) * juce::MathConstants<float>::pi * x);
        u[k] = v; pk = juce::jmax(pk, std::abs(v));
    }
    // Anchor posts + rest line (the un-struck body).
    g.setColour(juce::Colour(0xff2a2a44));
    g.drawLine(in.getX(), cy, in.getRight(), cy, 1.0f);
    g.fillRect(in.getX() - 3.0f, cy - 7.0f, 3.0f, 14.0f);
    g.fillRect(in.getRight(),    cy - 7.0f, 3.0f, 14.0f);

    juce::Path wave;
    for (int k = 0; k < NP; ++k)
    {
        const float x = in.getX() + (float) k / (float) (NP - 1) * in.getWidth();
        const float y = cy - (u[k] / pk) * amp;
        if (k == 0) wave.startNewSubPath(x, y); else wave.lineTo(x, y);
    }
    // Mirror ghost (the other half-cycle of the vibration) so it reads as a vibrating body,
    // then the main curve on top.
    {
        juce::Path mirror;
        for (int k = 0; k < NP; ++k)
        {
            const float x = in.getX() + (float) k / (float) (NP - 1) * in.getWidth();
            const float y = cy + (u[k] / pk) * amp;
            if (k == 0) mirror.startNewSubPath(x, y); else mirror.lineTo(x, y);
        }
        g.setColour(col.withAlpha(0.22f)); g.strokePath(mirror, juce::PathStrokeType(1.2f));
    }
    g.setColour(col.withAlpha(0.9f)); g.strokePath(wave, juce::PathStrokeType(1.8f));

    // The mallet handle rides the curve at the strike point.
    {
        const int   ki = juce::jlimit(0, NP - 1, (int) std::lround(dotX * (NP - 1)));
        const float hx = in.getX() + dotX * in.getWidth();
        const float hy = cy - (u[ki] / pk) * amp;
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.drawLine(hx + 3.0f, hy - 3.0f, hx + 14.0f, hy - 14.0f, 2.0f);          // mallet stick
        g.setColour(col);                  g.fillEllipse(hx - 4.5f, hy - 4.5f, 9.0f, 9.0f);
        g.setColour(juce::Colours::white); g.drawEllipse(hx - 4.5f, hy - 4.5f, 9.0f, 9.0f, 1.2f);
    }

    if (morph > 0.01f)
    {
        g.setFont(juce::Font(9.0f, juce::Font::bold));
        g.setColour(juce::Colour(0xff8aa0c8));
        g.drawText("-> " + juce::String(DrumChannel::modalMaterialName(nxt)) + " "
                       + juce::String(juce::roundToInt(morph * 100)) + "%",
                   getLocalBounds().reduced(5, 2), juce::Justification::bottomLeft, false);
    }
    g.setFont(juce::Font(9.5f, juce::Font::bold));
    g.setColour(dragging ? juce::Colours::white : juce::Colour(0xff8aa0c8));
    g.drawText("Hit " + juce::String(juce::roundToInt(hit * 100)) + "%  Damp "
                   + juce::String(juce::roundToInt(damp * 100)) + "%",
               getLocalBounds().reduced(5, 2), juce::Justification::topRight, false);
}

void ModalDisplay::mouseDown(const juce::MouseEvent& e) { dragging = true; mouseDrag(e); }
void ModalDisplay::mouseDrag(const juce::MouseEvent& e)
{
    auto* s = getSlot ? getSlot() : nullptr;
    if (! s) return;
    auto in = getLocalBounds().toFloat().reduced(10.0f, 6.0f);
    dotX = juce::jlimit(0.0f, 1.0f, (e.position.x - in.getX()) / juce::jmax(1.0f, in.getWidth()));
    s->modalHit  = 1.0f - std::abs(2.0f * dotX - 1.0f);      // symmetric: edges = 0, middle = 1
    s->modalDamp = juce::jlimit(0.0f, 1.0f, (e.position.y - in.getY()) / juce::jmax(1.0f, in.getHeight()));
    if (onEdit) onEdit();
    repaint();
}
void ModalDisplay::mouseUp(const juce::MouseEvent&)
{ if (dragging) { dragging = false; repaint(); if (onDragEnd) onDragEnd(); } }

juce::String ModalDisplay::getTooltip()
{
    return "The struck body's REAL vibration shape (its ringing modes - Material/Tone/Morph reshape it live).\n\n"
           "- Drag the mallet LEFT/RIGHT = where you strike: edges = full ring, middle = combed/hollow "
           "(watch modes vanish from the curve).\n"
           "- Drag DOWN = Damp (the curve flattens as the body is muted).";
}

void SlotEditor::init(int idx, MidiLearnManager& mlm, juce::LookAndFeel* knobLNF,
                      std::function<DrumChannel::Slot*()> slotFn, std::function<void()> editFn)
{
    index = idx; getSlot = slotFn; onEdit = editFn;
    // Drag-copy: watch presses on the box + all its children; a drag from a bare (non-control) area
    // starts a slot copy (see BoxDragger). No visible handle - drag the box wherever there's no control.
    boxDragger.ownerComp = this; boxDragger.slotIndex = idx;
    addMouseListener(&boxDragger, true);
    for (int i = 0; i < MAXK; ++i)
    {
        // "ui_sel_p{N}" = the N-th knob of the SELECTED slot's engine grid (selected-scope MIDI;
        // the old "slot{idx}_k{i}" pids were learnable but routed NOWHERE = dead assignments)
        auto k = std::make_unique<LearnableKnob>("ui_sel_p" + juce::String(i + 1), mlm);
        k->setSliderStyle(juce::Slider::RotaryVerticalDrag);
        k->setLookAndFeel(knobLNF);
        k->setTextBoxStyle(juce::Slider::TextBoxBelow, true, 46, 13);
        k->setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
        k->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k->setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        const int kid = i;
        k->onValueChange = [this, kid] {
            if (kid < params.size() && getSlot)
                if (auto* s = getSlot()) {
                    double v = knobs[kid]->getValue();
                    // FM Ratio: snap the mapped 1..6x ratio to integers (musical/harmonic); SHIFT = free.
                    if (params[kid].snapRatio
                        && ! juce::ModifierKeys::getCurrentModifiers().isShiftDown()) {
                        const double ratio = std::round(1.0 + v * 5.0);
                        v = juce::jlimit(0.0, 1.0, (ratio - 1.0) / 5.0);
                        knobs[kid]->setValue(v, juce::dontSendNotification);
                    }
                    // Pitched Freq in NOTE mode (click the read-out to toggle): PARK on the nearest
                    // semitone while dragging; hold SHIFT for free/inharmonic Hz. Hz mode = free drag.
                    if (params[kid].suffix == "nHz") {
                        if (gFreqNotesMode && ! juce::ModifierKeys::getCurrentModifiers().isShiftDown()) {
                            const double midi = std::round(69.0 + 12.0 * std::log2(juce::jmax(1.0, v) / 440.0));
                            v = juce::jlimit(params[kid].min, params[kid].max, 440.0 * std::pow(2.0, (midi - 69.0) / 12.0));
                            knobs[kid]->setValue(v, juce::dontSendNotification);
                        }
                    }
                    params[kid].set(*s, v);
                    if (params[kid].reBake) pendingRebake = true;   // Stretch -> needs a re-bake on release
                    if (onEdit) onEdit();
                    morphView.repaint(); waveView.repaint(); physView.repaint(); modalView.repaint();
                    if (activeParamCount() != lastActiveCount) relayoutSelf();
                }
        };
        k->onDragEnd = [this] {
            if (pendingRebake && onSampleEdit) { onSampleEdit(index); pendingRebake = false; }  // re-bake stretch/pitch once
            if (onAudition) onAudition();     // hear the edit ("Auto" toggle) - ALL engines (was gated to osc/sample only)
        };
        addAndMakeVisible(*k);
        auto l = std::make_unique<juce::Label>();
        l->setFont(juce::Font(11.5f)); l->setJustificationType(juce::Justification::centred);
        l->setMinimumHorizontalScale(0.62f); l->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(*l);
        knobs.push_back(std::move(k));
        labels.push_back(std::move(l));
    }
    addChildComponent(morphView);                 // shown only for Analog/FM (setEngine)
    morphView.getSlot = slotFn;
    morphView.onEdit  = [this] { if (onEdit) onEdit(); };
    addChildComponent(waveView);                  // shown only for Wavetable (setEngine)
    waveView.getSlot = slotFn;
    addChildComponent(physView);                  // Physical: interactive string (Position/Tone)
    physView.getSlot   = slotFn;
    physView.onEdit    = [this] { if (onEdit) onEdit(); };
    physView.onDragEnd = [this] { if (onAudition) onAudition(); };
    addChildComponent(modalView);                 // Modal: interactive struck body (Hit Pos/Damp)
    modalView.getSlot   = slotFn;
    modalView.onEdit    = [this] { if (onEdit) onEdit(); };
    modalView.onDragEnd = [this] { if (onAudition) onAudition(); };
    addChildComponent(fmEnvSw);                   // SrcOsc: FM Amount follows the amp envelope
    fmEnvSw.setTooltip("ENV: ties the FM brightness to the sound's volume envelope.\n\n"
                       "- ON = each hit starts buzzy/metallic and smooths to the plain wave as it fades "
                       "(the classic FM drum/bass trick: bright attack, warm tail).\n"
                       "- OFF = the FM buzz stays constant start to finish.\n"
                       "- Only matters when FM Amount is up.");
    fmEnvSw.onClick = [this] {
        if (auto* s = getSlot ? getSlot() : nullptr) {
            s->fmEnvFollow = fmEnvSw.getToggleState();
            if (onEdit) onEdit();
            if (onAudition) onAudition();
        }
    };

    // SrcOsc faders: Freq (shared base pitch, under the wave) + Reson (resonator gate). Horizontal so
    // each leaves the FM / Physical knobs a full clean row. MIDI-learnable like the knobs.
    auto mkFader = [&](const juce::String& id) {
        auto f = std::make_unique<LearnableKnob>("slot" + juce::String(idx) + "_" + id, mlm);
        f->setSliderStyle(juce::Slider::LinearHorizontal);
        f->setLookAndFeel(knobLNF);
        f->setTextBoxStyle(juce::Slider::TextBoxRight, true, 50, 16);
        f->setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
        f->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        f->setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        f->setColour(juce::Slider::trackColourId, juce::Colour(0xff35c0ff));
        addChildComponent(*f);
        return f;
    };
    freqFader  = mkFader("freq");   depthFader = mkFader("fmdepth");
    freqFader->paramId  = "ui_sel_slotFreq";    // selected-scope MIDI (log-mapped 20..4186 Hz)
    depthFader->paramId = "ui_sel_slotFmAmt";
    // DOUBLE-CLICK -> the Analog+FM faders' FACTORY DEFAULTS (fresh Slot), like the other knobs.
    { DrumChannel::Slot d;
      freqFader->setDoubleClickReturnValue (true, 261.6255653);   // dbl-click = C4 (the roll's pin; user spec)
      depthFader->setDoubleClickReturnValue(true, d.fmDepth); }
    freqFader->setRange(20.0, 4186.0, 0.0);   // reach C7 (MIDI 108 = 4186 Hz) - covers an 88-key top octave for sound design
    freqFader->setSkewFactorFromMidPoint(500.0);   // middle of the fader ~= 500 Hz (gently slow, not too slow)
    freqFader->textFromValueFunction = [](double v) {   // Hz by default; the NOTE in note mode (click the read-out)
        return gFreqNotesMode ? noteNameFor(v) : juce::String(juce::roundToInt(v)) + " Hz";
    };
    freqFader->setTooltip("Base Freq: the oscillator's base pitch.\n\n"
                          "- CLICK the value read-out = NOTE mode (shows A1, C2...; drags snap to semitones, "
                          "SHIFT = free). Click again = Hz. Double-click = C4.\n"
                          "- The keyboard plays REAL notes wherever this sits, and never changes it.\n"
                          "- Step recordings are stored RELATIVE to it (step pitch 0 = this frequency) - "
                          "moving it afterwards transposes the whole pattern.");
    freqFader->onValueChange = [this] {
        if (auto* s = getSlot ? getSlot() : nullptr) {
            double v = freqFader->getValue();
            if (gFreqNotesMode && ! juce::ModifierKeys::getCurrentModifiers().isShiftDown()) {   // note mode: park on a semitone
                const double midi = std::round(69.0 + 12.0 * std::log2(juce::jmax(1.0, v) / 440.0));
                v = juce::jlimit(20.0, 4186.0, 440.0 * std::pow(2.0, (midi - 69.0) / 12.0));
                freqFader->setValue(v, juce::dontSendNotification);
            }
            s->oscFreq = (float) v; if (onEdit) onEdit(); morphView.repaint();
        }
    };
    freqFader->onDragEnd = [this] { if (onAudition) onAudition(); };
    depthFader->setRange(0.0, 1.0, 0.0);
    depthFader->textFromValueFunction = [](double v) { return v <= 0.0005 ? juce::String("OFF")
                                                        : juce::String(juce::roundToInt(v * 100.0)) + "%"; };
    depthFader->setTooltip("FM amount: 0 = pure analog oscillator (no FM). Raise it to add FM harmonics / "
                           "brightness - the Ratio + Feedback knobs shape that FM tone (and the wave display "
                           "shows it live).");
    depthFader->onValueChange = [this] {
        if (auto* s = getSlot ? getSlot() : nullptr) { s->fmDepth = (float) depthFader->getValue(); if (onEdit) onEdit(); morphView.repaint(); }
    };
    depthFader->onDragEnd = [this] { if (onAudition) onAudition(); };

    // Warp (ANALOG section): phase skew / PWM. 0.5 = neutral. Distinct from FM (this reshapes the cycle).
    warpFader = mkFader("warp");
    warpFader->paramId = "ui_sel_slotWarp";     // selected-scope MIDI
    warpFader->setRange(0.0, 1.0, 0.0);
    warpFader->setColour(juce::Slider::trackColourId, juce::Colour(0xff8a7adf));
    warpFader->textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
    warpFader->setTooltip("Warp: a one-way WAVEFOLD - folds the wave back on itself, adding harmonics + grit as you "
                          "raise it (0 = off, clean). Audible on every wave. Distinct from FM (which adds sidebands).");
    warpFader->onValueChange = [this] {
        if (auto* s = getSlot ? getSlot() : nullptr) { s->oscWarp = (float) warpFader->getValue(); if (onEdit) onEdit(); morphView.repaint(); }
    };
    warpFader->onDragEnd = [this] { if (onAudition) onAudition(); };

    // Single "Wave" selector: ONE horizontal fader ABOVE the visual that scans every shape (snaps to each;
    // shows "n-Name"). Replaces the old From/To vertical faders + the over-the-note morph (which sounded harsh).
    fromFader = mkFader("wave");   // reuse the fromFader slot as the single Wave fader (toFader retired)
    fromFader->paramId = {};       // shape picking by CC = jumpy nonsense - not learnable
    fromFader->setTextBoxStyle(juce::Slider::TextBoxRight, true, 150, 16);  // WIDE read-out (track shrinks) - Custom explains itself (user)
    fromFader->setColour(juce::Slider::trackColourId, juce::Colour(0xff35c0ff));
    fromFader->setRange(0.0, (double) (DrumChannel::oscShapeCount() - 1), 1.0);
    fromFader->textFromValueFunction = [](double v) { const int i = juce::roundToInt(v);
        if (i >= DrumChannel::WvCustom) return juce::String("Custom (draw on wave)");   // the invitation (user)
        return juce::String(i + 1) + "-" + juce::String(DrumChannel::oscShapeName(i)); };
    fromFader->setTooltip("Wave: the oscillator's waveform - slide through all shapes (Sine, Hump, Tri, Square, Saw, "
                          "Pulse, Vowel A/O, Formant, Organ, Bell, Glass, Reed, Brass, Custom).\n\n"
                          "- CUSTOM (last) = ADDITIVE: click the wave drawing to open DRAW HARMONICS and paint "
                          "your own harmonic levels. The drawn wave is saved with the sound and works with every "
                          "control (envelopes, filters, unison, FM, Warp, FX).");
    fromFader->onValueChange = [this] {
        if (auto* s = getSlot ? getSlot() : nullptr) {
            s->oscShape = (int) std::lround(fromFader->getValue()); s->oscShapeB = s->oscShape;   // single wave (no morph)
            if (onEdit) onEdit(); morphView.repaint();
        }
    };
    fromFader->onDragEnd = [this] { if (onAudition) onAudition(); };

    setEngine(-1);
}

void SlotEditor::setEngine(int eng)
{
    engine = eng;
    params = slotParamsFor(eng);
    const bool grain = (eng == DrumChannel::SrcGrain);
    const bool morph = (eng == DrumChannel::SrcOsc || eng == DrumChannel::SrcFM || grain);
    morphView.setVisible(morph);   // GRAIN reuses the wave preview: it shows the grain SOURCE
    waveView.setVisible(eng == DrumChannel::SrcWave);   // wavetable visual
    physView.setVisible(eng == DrumChannel::SrcPhys);   // interactive string (Position/Tone)
    modalView.setVisible(eng == DrumChannel::SrcModal); // interactive struck body (Hit Pos/Damp)
    fmEnvSw.setVisible(eng == DrumChannel::SrcOsc);     // FM Amount env-follow (placed by placeOsc)
    morphView.fmMode = morph && ! grain;   // grain preview = the raw source (no FM applied to grains)
    oscLayout = (eng == DrumChannel::SrcOsc);   // sectioned Analog/FM/Resonator layout with Freq/Depth/Reson faders
    freqFader->setVisible(oscLayout);
    depthFader->setVisible(false);          // FM amount is now the "Amount" KNOB (params[0]), not a fader
    warpFader->setVisible(oscLayout);
    fromFader->setVisible(oscLayout || grain);   // the "Wave" fader = the grain SOURCE picker too
    if (toFader) toFader->setVisible(false);   // retired
    for (int i = 0; i < MAXK; ++i)
    {
        const bool has = i < params.size();
        if (! has) { knobs[i]->setVisible(false); labels[i]->setVisible(false); continue; }
        const auto& p = params[i];
        // choices snap to whole indices; "x" (speed) snaps to fine 0.05 steps (so 0.25/0.5 are reachable
        // and the readout shows decimals, not a misleading rounded "0x"); everything else is continuous.
        knobs[i]->setRange(p.min, p.max, p.choices.size() > 0 ? 1.0 : (p.suffix == "x" ? 0.05 : (p.suffix == "sl" ? 1.0 : 0.0)));
        // Frequency faders are LOG-skewed (low freqs get most of the travel = a slow, musical start); knobs are
        // reused across engines, so reset to linear for everything else.
        if (p.suffix == "nHz")   // pitched freq: log-ish skew but with a FASTER low end (pure sqrt midpoint
                                 // made the first octaves crawl - "stays at 20 Hz for too long")
            knobs[i]->setSkewFactorFromMidPoint(juce::jmin(p.max * 0.5, std::sqrt(p.min * p.max) * 2.0));
        else if (p.suffix == "Hz" && p.max > p.min * 4.0) knobs[i]->setSkewFactorFromMidPoint(std::sqrt(p.min * p.max));
        else                                              knobs[i]->setSkewFactor(1.0);
        knobs[i]->textFromValueFunction = [p](double v) { return fmtSlot(p, v); };
        knobs[i]->setTooltip(p.tooltip);
        labels[i]->setText(p.label, juce::dontSendNotification);
        // DOUBLE-CLICK = reset to the parameter's FACTORY DEFAULT (the value on a fresh Slot).
        // Slot knobs are REUSED across engines and never had a return value set, so double-click did
        // nothing on them (user: some knobs "went to a different value" = stayed on the edited value).
        { DrumChannel::Slot dflt;
          // pitched Base Freq knobs double-click to C4 (practical reset - user spec); everything
          // else keeps the fresh-Slot default.
          knobs[i]->setDoubleClickReturnValue(true, p.suffix == "nHz" ? 261.6255653 : p.get(dflt)); }
    }
    updateKnobVisibility();   // hide the resonator reveal knobs unless this slot's Reson is up
    pushValues();
}

int SlotEditor::activeParamCount() const
{
    // Physical knobs (Drive/Material/Tone/Position) are ALWAYS shown now - no Reson>0 reveal gate.
    // (The slots got a full-height column, so there's room to show them all the time.)
    return params.size();
}

void SlotEditor::updateKnobVisibility()
{
    const int n = activeParamCount();
    for (int i = 0; i < MAXK; ++i)
    {
        const bool on = i < n;
        knobs[i]->setVisible(on); labels[i]->setVisible(on);
    }
    lastActiveCount = n;
}

void SlotEditor::relayoutSelf()
{
    if (lastBoxW > 0) place(lastBoxX, lastYTop, lastBoxW, lastBoxH);
}

void SlotEditor::pushValues()
{
    auto* s = getSlot ? getSlot() : nullptr;
    if (! s) return;
    for (int i = 0; i < params.size() && i < MAXK; ++i)
    {
        knobs[i]->setValue(params[i].get(*s), juce::dontSendNotification);
        knobs[i]->updateText();   // refresh the read-out: textFromValueFunction may have
                                  // changed when the engine switched even if the value didn't.
    }
    if (oscLayout) {              // Analog+FM faders (not in the params list)
        freqFader->setValue (s->oscFreq,  juce::dontSendNotification); freqFader->updateText();
        depthFader->setValue(s->fmDepth,  juce::dontSendNotification); depthFader->updateText();
        warpFader->setValue (s->oscWarp,  juce::dontSendNotification); warpFader->updateText();
        fmEnvSw.setToggleState(s->fmEnvFollow, juce::dontSendNotification);
    }
    if (oscLayout || engine == DrumChannel::SrcGrain) {   // the Wave fader is shown for Osc AND Granular (the grain source)
        fromFader->setValue (s->oscShape, juce::dontSendNotification); fromFader->updateText();
    }
    morphView.repaint();          // reflect this slot's wave (+ warp + FM)
    physView.repaint(); modalView.repaint();   // reflect Position/Tone / Hit Pos/Damp + material looks
    applyFreqLock();              // transpose lock follows the channel on every refresh
}

void SlotEditor::place(int boxX, int yTop, int boxW, int boxH)
{
    lastBoxX = boxX; lastYTop = yTop; lastBoxW = boxW; lastBoxH = boxH;
    updateKnobVisibility();                               // resonator reveal may have toggled
    setBounds(boxX, yTop, boxW, boxH);
    // Reset all generic knobs to rotary + value-below; placeGeneric re-styles some as faders for 5+-param engines.
    for (auto& k : knobs) { k->setSliderStyle(juce::Slider::RotaryVerticalDrag);
                            k->setTextBoxStyle(juce::Slider::TextBoxBelow, true, 46, 13); }
    fmLineY = resLineY = -1;
    if (oscLayout) placeOsc(boxW); else placeGeneric(boxW);
    hookFreqReadouts();   // setTextBoxStyle recreates the value Labels -> re-attach the Hz<->note click
    repaint();                                            // section divider lines / labels (SrcOsc)
}

// Attach the Hz <-> NOTE toggle click to every pitched-Freq control's value Label. The Label is
// the slider's internal child (recreated whenever the text-box style changes), so this runs
// after every place(). addMouseListener de-dupes, so repeat calls are safe.
void SlotEditor::hookFreqReadouts()
{
    freqReadoutClick.onClick = [this] {
        gFreqNotesMode = ! gFreqNotesMode;
        pushValues();                        // refresh this slot's read-outs + tints
        if (onFreqModeToggled) onFreqModeToggled();   // and the sibling slot's
    };
    auto hook = [this](juce::Slider& sl) {
        for (auto* ch : sl.getChildren())
            if (auto* l = dynamic_cast<juce::Label*>(ch)) l->addMouseListener(&freqReadoutClick, false);
    };
    for (int i = 0; i < params.size() && i < (int) knobs.size(); ++i)
        if (params[i].suffix == "nHz") hook(*knobs[i]);
    if (freqFader != nullptr) hook(*freqFader);
    applyFreqLock();   // the lock state must survive every re-place/refresh
}

// (freqDisabled is always false now - PIANO ROLL is knob-INDEPENDENT: it plays a C4-absolute base in
// the DSP and never touches this knob. The lock path stays inert for safety.)
void SlotEditor::applyFreqLock()
{
    static const juce::String lockMsg;   // unused (freqDisabled never set)
    static const juce::String freqFaderTip =
        "Frequency: the oscillator's base pitch (Hz). CLICK the value read-out to switch to NOTE "
        "mode (shows A1, C2...; dragging snaps to semitones, SHIFT = free). Click again for Hz.\n\n"
        "KEYS: the keyboard plays REAL notes no matter where this knob sits, and never changes it. "
        "Keyboard recordings are stored RELATIVE to this knob (step pitch 0 = this frequency), so they "
        "play back exactly as performed.\n\n"
        "PIANO ROLL ignores this knob entirely - the roll always plays C4-referenced notes (row 0 = C4), "
        "so the knob stays free and untouched. It only matters in step mode.";
    auto apply = [this](juce::Slider& sl, const juce::String& openTip) {
        sl.setEnabled(! freqDisabled);
        sl.setAlpha(freqDisabled ? 0.45f : 1.0f);
        sl.setTooltip(freqDisabled ? lockMsg : openTip);
    };
    // Reset EVERY param knob, then lock only the CURRENT nHz ones: the knob objects are REUSED
    // across engines, so a knob disabled as "Freq" on Physical must come back to life when the same
    // knob becomes "FM Amount" on Oscillator (stale disable = the faded/uncontrollable FM knob bug).
    for (int i = 0; i < params.size() && i < (int) knobs.size(); ++i)
    {
        const bool lockThis = freqDisabled && params[i].suffix == "nHz";
        knobs[i]->setEnabled(! lockThis);
        knobs[i]->setAlpha(lockThis ? 0.45f : 1.0f);
        knobs[i]->setTooltip(lockThis ? lockMsg : params[i].tooltip);
    }
    if (freqFader != nullptr) apply(*freqFader, freqFaderTip);
}

// Knobs in up to TWO balanced rows that FILL the box height (so the knobs are as big as fit, no empty
// space below). 1 row for <=4 params; 2 rows for 5+ (Physical/Modal/Noise).
void SlotEditor::placeGeneric(int boxW)
{
    const int n = activeParamCount();                     // only the currently-revealed knobs
    const int mL = 6, innerW = boxW - 2 * mL;
    int yTop = 4;
    // GRANULAR shows BOTH the source preview AND the Wave fader (5 param faders below). That stack
    // overflowed a 140px box, so its preview + fader rows are tighter than the legacy Analog/FM morph.
    const bool grainSrc = (fromFader != nullptr && fromFader->isVisible() && ! oscLayout);
    if (morphView.isVisible()) {                          // Analog/FM legacy + GRANULAR: source preview
        const int mw = juce::jmin(boxW - 8, rowWidth(MAX_ROW) + 12);
        const int mh = grainSrc ? 34 : 40;
        morphView.compact = true;
        morphView.setBounds((boxW - mw) / 2, yTop, mw, mh);
        morphView.repaint();
        yTop += mh + (grainSrc ? 3 : 4);
    }
    if (grainSrc) {
        // GRANULAR: the Wave fader right under the preview = the grain SOURCE, fully visible
        // (it was silently read from oscShape with no control = a hidden param - user caught it).
        // A "Waves" tag on the left (like the Oscillator group's "Wave" tag) - drawn in paint().
        const int tag = 42;
        fromFader->setBounds(mL + tag, yTop, innerW - tag, 16);
        yTop += 19;
    }
    if (waveView.isVisible()) {                           // Wavetable: the current waveform across the top
        const int mw = juce::jmin(boxW - 8, rowWidth(MAX_ROW) + 12);
        const int mh = 56;
        waveView.setBounds((boxW - mw) / 2, yTop, mw, mh);
        waveView.repaint();
        yTop += mh + 4;
    }
    if (physView.isVisible()) {                           // Physical: interactive string across the top
        const int mh = 52;
        physView.setBounds(mL, yTop, innerW, mh);
        physView.repaint();
        yTop += mh + 4;
    }
    if (modalView.isVisible()) {                          // Modal: interactive struck body across the top
        const int mh = 52;
        modalView.setBounds(mL, yTop, innerW, mh);
        modalView.repaint();
        yTop += mh + 4;
    }
    if (n == 0) return;

    // 5+ params (Physical / Modal / Noise): stacked FULL-WIDTH faders - readable + uses the height well
    // (a cramped 2-row knob grid was wasting space). Name on the left, value on the right.
    const int reserve = 0;
    if (n >= 5)
    {
        const int availH = juce::jmax(20, getHeight() - yTop - 2);
        const int rowH = juce::jlimit(13, 30, availH / n);   // floor 13 so 5 faders fit under the grain source strip (was 16 = overflow)
        // Wider name + value columns (so Modal material names etc. are readable) -> the fader track itself is shorter.
        // Same dimensions for Noise/Physical/Modal (they all share this path) so the boxes look consistent.
        const int tag = 62;                          // name column (left)
        const int valW = juce::jlimit(60, 90, innerW - tag - reserve - 60);  // value column (right) - room for long choice names
        for (int i = 0; i < n; ++i) {
            const int y = yTop + i * rowH;
            knobs[i]->setSliderStyle(juce::Slider::LinearHorizontal);
            knobs[i]->setTextBoxStyle(juce::Slider::TextBoxRight, true, valW, juce::jmax(12, rowH - 4));
            knobs[i]->setBounds(mL + tag, y, innerW - tag - reserve, rowH - 2);
            labels[i]->setJustificationType(juce::Justification::centredLeft);
            labels[i]->setBounds(mL, y, tag - 2, rowH - 2);
        }
        return;
    }

    // <= 4 params: one row of rotary knobs, as big as fits (inside innerW minus the reserved column).
    const int cols = n;
    const int availH = juce::jmax(50, getHeight() - yTop);
    int KS = juce::jmin((innerW - reserve - (cols - 1) * GAP) / cols, availH - 24);
    KS = juce::jlimit(30, 64, KS);
    const int kh = KS + 13;
    const int rowW = cols * KS + (cols - 1) * GAP;
    int kx = mL + (innerW - reserve - rowW) / 2;          // centre the row in the remaining width
    const int yy = yTop + juce::jmax(0, (availH - (kh + 11)) / 2);
    for (int i = 0; i < n; ++i) {
        knobs[i]->setBounds(kx, yy, KS, kh);
        labels[i]->setJustificationType(juce::Justification::centred);
        labels[i]->setBounds(kx - 8, yy + kh, KS + 16, 11);
        kx += KS + GAP;
    }
}

// SrcOsc sectioned layout: ANALOG (wave + Freq fader) | FM (4 knobs) | RESONATOR (Reson fader +
// the 4 Physical knobs, revealed when Reson > 0). Freq + Reson are horizontal faders so each knob
// group is one clean labelled row. Divider lines + section names are drawn in paint().
void SlotEditor::placeOsc(int boxW)
{
    const int n = activeParamCount();                     // 2 (FM only) or 6 (FM + Physical)
    const bool resonOn = (n > 2);
    const int KS = KNOB, step = KS + GAP, kh = KS + 16;
    const int KS_FM = 44, kh_FM = KS_FM + 16; juce::ignoreUnused(KS_FM, kh_FM);   // (knob row retired - FM is stacked faders now)
    const int mL = 6, innerW = boxW - 2 * mL;
    juce::ignoreUnused(step, kh);

    juce::ignoreUnused(resonOn);
    resLineY = -1;   // no PHYSICAL section in Analog+FM anymore
    int y = 3;
    // -- "Wave" fader on top. --
    { const int tag = 38;                                  // "Wave" tag drawn at oscLabelR (paint)
      oscLabelR = juce::Rectangle<int>(mL, y, tag - 2, 16);
      fromFader->setBounds(mL + tag, y, innerW - tag, 16);
      y += 16 + 2; }
    // -- Wave visual: as TALL as possible (FM names sit BESIDE the knobs, so no name row below them). --
    {
        const int reserve = 18 /*Freq+Warp row*/ + 56 /*FM = 3 stacked fader rows*/ + 4;
        const int mh = juce::jlimit(24, 110, getHeight() - y - reserve);
        morphView.compact = (mh < 50);
        morphView.setBounds(mL, y, innerW, mh);
        morphView.repaint();
        y += mh + 3;
    }
    // -- Freq + Warp faders share ONE row (each half-width with its tag). --
    { const int tag = 34, halfW = innerW / 2;
      freqLabelR = juce::Rectangle<int>(mL, y, tag - 2, 16);
      freqFader->setBounds(mL + tag, y, halfW - tag - 4, 16);
      warpLabelR = juce::Rectangle<int>(mL + halfW, y, tag - 2, 16);
      warpFader->setBounds(mL + halfW + tag, y, halfW - tag, 16);
      y += 16 + 2; }
    // -- FM: "FM" tag (with the Env-follow switch under it) on the left, then the 3 FM KNOBS
    //    (Amount, Ratio, Feedback), each with its NAME BESIDE it. --
    fmLineY = y;
    { const int tag = 30;                                  // "FM" tag drawn at fmLabelR (paint); Env switch under it
      fmLabelR = juce::Rectangle<int>(mL, y + 2, tag, 14);
      fmEnvSw.setBounds(mL, y + 22, 26, 13);               // FM Amount follows the amp env (tooltip explains)
      // The 3 FM controls are STACKED HORIZONTAL FADERS (user order - the shrunken knobs were
      // unreadable): name on the left, value read-out on the right, same row recipe as the
      // Modal/Noise boxes.
      const int fx0 = mL + tag;
      const int nfm = juce::jmin(3, (int) params.size());  // Amount, Ratio, Feedback
      const int reserve = 0;
      const int nameW = 60, rowH = 18, valW = 46;
      for (int gi = 0; gi < nfm; ++gi) {
          const int yy = y + gi * rowH;
          knobs[gi]->setSliderStyle(juce::Slider::LinearHorizontal);
          knobs[gi]->setTextBoxStyle(juce::Slider::TextBoxRight, true, valW, rowH - 4);
          knobs[gi]->setBounds(fx0 + nameW, yy, innerW - tag - nameW - reserve, rowH - 2);
          labels[gi]->setJustificationType(juce::Justification::centredLeft);
          labels[gi]->setBounds(fx0, yy, nameW - 2, rowH - 2);
      }
      y += nfm * rowH + 2; }
}

// Section divider lines + names for the SrcOsc sectioned layout (drawn behind the children).
void SlotEditor::paint(juce::Graphics& g)
{
    // Audio-file drop highlight (any engine): the whole box is a drop target.
    if (fileDragOver) {
        g.setColour(juce::Colour(0x332ec46a));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(juce::Colour(0xff2ec46a));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 2.0f);
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText("drop to load sample", getLocalBounds(), juce::Justification::centred, false);
    }
    // Slot drag-copy hover: a green outline shows this box will receive the drop (no text - user).
    if (slotDropOver) {
        g.setColour(juce::Colour(0x2635d07a)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(juce::Colour(0xff35d07a)); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 2.0f);
    }
    // GRANULAR: label the grain-source Wave fader "Waves" on its left (like the Oscillator group).
    if (! oscLayout && fromFader != nullptr && fromFader->isVisible() && morphView.isVisible())
    {
        g.setFont(juce::Font(10.0f, juce::Font::bold));
        g.setColour(juce::Colour(0xff9a9ac0));
        auto fb = fromFader->getBounds();
        g.drawText("Waves", 6, fb.getY(), juce::jmax(1, fb.getX() - 8), fb.getHeight(), juce::Justification::centredLeft, false);
    }
    if (! oscLayout) return;
    const float right = (float) getWidth() - 6.0f;
    const auto  lineCol = juce::Colour(0xff33335a);
    g.setFont(juce::Font(10.0f, juce::Font::bold));

    // ANALOG: "Wave" (above the visual) + "Freq" + "Warp" tags to the left of their faders.
    g.setColour(juce::Colour(0xff9a9ac0));
    g.drawText("Wave", oscLabelR,  juce::Justification::centredLeft, false);
    g.drawText("Base Freq", freqLabelR, juce::Justification::centredLeft, false);
    g.drawText("Warp", warpLabelR, juce::Justification::centredLeft, false);

    // FM: a rule, then an "FM" tag + the Env-follow switch's caption.
    if (fmLineY >= 0) {
        g.setColour(lineCol);
        g.drawHorizontalLine(fmLineY - 3, 6.0f, right);
        g.setColour(juce::Colour(0xff35c0ff));
        g.drawText("FM", fmLabelR, juce::Justification::centredLeft, false);
        if (fmEnvSw.isVisible()) {
            g.setFont(juce::Font(8.5f, juce::Font::bold));
            g.setColour(juce::Colour(0xff9a9ac0));
            g.drawText("ENV", fmEnvSw.getX(), fmEnvSw.getBottom() + 1, 26, 9, juce::Justification::centred, false);
        }
    }
}

//==============================================================================
// Shared MIDI-learn popup (shows current assignment, lets you assign/clear)
//==============================================================================
void showMidiLearnMenu(juce::Component* target, MidiLearnManager& mlm,
                       const juce::String& paramId, int forcedChannel,
                       const juce::String& altPid, const juce::String& altLabel,
                       const juce::String& alt2Pid, const juce::String& alt2Label)
{
    juce::PopupMenu menu;
    int cc = mlm.getCCForParam(paramId);
    int ch = mlm.getChannelForParam(paramId);
    const bool learningThis  = mlm.isLearning() && mlm.getLearningParam() == paramId;
    const bool learningOther = mlm.isLearning() && ! learningThis;

    if (paramId.startsWith("ui_sel"))   // ui_sel_* + ui_selstep_*: they follow the selection
        menu.addSectionHeader("Acts on the CURRENT selection (pattern / channel / slot).");
    if (learningThis)
    {
        menu.addSectionHeader("Listening... move a knob/fader on your MIDI device");
        menu.addItem(3, "Cancel MIDI learn");
    }
    else
    {
        menu.addSectionHeader(cc >= 0 ? ("Assigned: ch" + juce::String(ch) + " cc" + juce::String(cc))
                                      : "Not assigned");
        menu.addItem(2, cc >= 0 ? "Reassign MIDI CC..." : "Assign MIDI CC...");
        if (cc >= 0)
            menu.addItem(1, "Reset (clear) MIDI assignment");
        if (altPid.isNotEmpty())   // the SELECTED-channel variant (follows selection)
        {
            const int acc = mlm.getCCForParam(altPid);
            menu.addSeparator();
            menu.addItem(4, "Assign: " + altLabel + "..."
                            + (acc >= 0 ? "  [ch" + juce::String(mlm.getChannelForParam(altPid))
                                           + " cc" + juce::String(acc) + "]" : juce::String()));
            if (acc >= 0) menu.addItem(5, "Clear the " + altLabel + " assignment");
        }
        if (alt2Pid.isNotEmpty())  // [2026-07-14 11:10] a third variant (steps: pinned cell, SELECTED pattern)
        {
            const int acc = mlm.getCCForParam(alt2Pid);
            menu.addSeparator();
            menu.addItem(6, "Assign: " + alt2Label + "..."
                            + (acc >= 0 ? "  [ch" + juce::String(mlm.getChannelForParam(alt2Pid))
                                           + " cc" + juce::String(acc) + "]" : juce::String()));
            if (acc >= 0) menu.addItem(7, "Clear the " + alt2Label + " assignment");
        }
        if (learningOther)
            menu.addItem(3, "Cancel the pending MIDI learn");
    }

    auto* mlmPtr = &mlm;
    // Drop the menu from where the user clicked, not from the control's anchor.
    auto mp = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
    juce::ignoreUnused(target);
    menu.showMenuAsync(juce::PopupMenu::Options{}
                           .withTargetScreenArea({ mp.x, mp.y, 1, 1 })
                           .withMinimumWidth(240),
        [mlmPtr, paramId, forcedChannel, altPid, alt2Pid](int result) {
            if      (result == 1) mlmPtr->clearParam(paramId);
            else if (result == 2) mlmPtr->startLearning(paramId, forcedChannel);
            else if (result == 3) mlmPtr->stopLearning();
            else if (result == 4) mlmPtr->startLearning(altPid);
            else if (result == 5) mlmPtr->clearParam(altPid);
            else if (result == 6) mlmPtr->startLearning(alt2Pid);
            else if (result == 7) mlmPtr->clearParam(alt2Pid);
        });
}

// (StepGridComponent method definitions live in StepGridComponent.cpp)

//==============================================================================
// Sound Bank combo right-click: MIDI-learn SOUND BROWSING for the SELECTED channel.
// NEXT/PREV buttons only (tap = one sound, hold = scroll). The knob decodes (absolute /
// relative / 14-bit fine) were built and REMOVED at the user's order: his controller's
// custom modes are absolute-only with fixed physical travel, so every knob mode ended in
// a wall or a recovery gesture. Buttons ARE motion - no walls. (docs/HISTORY.md has the saga.)
//==============================================================================
void DrumSequencerEditor::PickerCombo::showCcMenu()
{
    auto tag = [this](const char* pid) {
        const int cc = mlm->getCCForParam(pid);
        return cc < 0 ? juce::String()
                      : "   [ch" + juce::String(mlm->getChannelForParam(pid)) + " cc" + juce::String(cc) + "]";
    };
    juce::PopupMenu m;
    m.addSectionHeader("MIDI: browse sounds on the SELECTED channel");
    if (mlm->isLearning())
    {
        m.addSectionHeader("Listening... press a pad/button on your MIDI device");
        m.addItem(4, "Cancel MIDI learn");
    }
    m.addItem(1, "Learn NEXT sound (tap = one, hold = scroll)..." + tag("ui_sound_next"));
    m.addItem(2, "Learn PREV sound (tap = one, hold = scroll)..." + tag("ui_sound_prev"));
    if (mlm->getCCForParam("ui_sound_next") >= 0 || mlm->getCCForParam("ui_sound_prev") >= 0)
    { m.addSeparator(); m.addItem(3, "Clear these assignments"); }
    auto mp = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
    auto* L = mlm;
    m.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({ mp.x, mp.y, 1, 1 }).withMinimumWidth(280),
        [L](int r) {
            if      (r == 1) L->startLearning("ui_sound_next");
            else if (r == 2) L->startLearning("ui_sound_prev");
            else if (r == 3) { L->clearParam("ui_sound_next"); L->clearParam("ui_sound_prev");
                               // retired knob targets: scrub stale assignments from old sessions
                               L->clearParam("ui_sound_knobA"); L->clearParam("ui_sound_knob");
                               L->clearParam("ui_sound_knob14"); }
            else if (r == 4) L->stopLearning();
        });
}

//==============================================================================
// DragMidiSource
//==============================================================================

void DragMidiSource::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff334455));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1), 5.0f);
    g.setColour(juce::Colours::lightblue);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1), 5.0f, 1.0f);
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.drawText("DRAG MIDI", getLocalBounds(), juce::Justification::centred);   // [2026-07-14] shortened (user) - frees room for the CPU/RAM readout
}

void DragMidiSource::mouseDrag(const juce::MouseEvent& e)
{
    if (dragStarted) return;
    if (e.getDistanceFromDragStart() < 5) return;

    dragStarted = true;
    if (getMidiFile)
    {
        auto file = getMidiFile();
        if (file.existsAsFile())
        {
            auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
            if (container)
                container->performExternalDragDropOfFiles({ file.getFullPathName() }, false,
                    this, [this]() { dragStarted = false; });
        }
        else dragStarted = false;
    }
}

//==============================================================================
// LearnableKnob / LearnableButton / PatternButton
//==============================================================================

LearnableKnob::LearnableKnob(const juce::String& pid, MidiLearnManager& mlm)
    : juce::Slider(juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox),
      paramId(pid), midiLearn(mlm)
{}

void LearnableKnob::mouseDown(const juce::MouseEvent& e)
{
    if ((e.mods.isRightButtonDown() || e.mods.isPopupMenu()) && paramId.isNotEmpty())
    {
        int forced = learnChannelProvider ? learnChannelProvider() : -1;
        showMidiLearnMenu(this, midiLearn, paramId, forced);
        return;
    }
    // ROTARY knobs: a CLICK jumps the value to the clicked ANGLE (user: "should work when I click a
    // knob position, not only after dragging"). The drag then continues relatively from there.
    // Clicks in the text-box area (below the dial) or the dead centre are left alone.
    if ((getSliderStyle() == juce::Slider::RotaryVerticalDrag || getSliderStyle() == juce::Slider::Rotary
         || getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag)
        && ! isTwoValue() && ! isThreeValue())
    {
        const auto r   = getLocalBounds().toFloat();
        const float sz = juce::jmin(r.getWidth(), r.getHeight());
        const auto dial = juce::Rectangle<float>(r.getX() + (r.getWidth() - sz) * 0.5f, r.getY(), sz, sz);
        const float dx = e.position.x - dial.getCentreX(), dy = e.position.y - dial.getCentreY();
        if (dial.contains(e.position) && std::sqrt(dx * dx + dy * dy) > sz * 0.12f)
        {
            const auto rp = getRotaryParameters();
            float ang = std::atan2(dx, -dy);                  // 0 = 12 o'clock, clockwise positive
            while (ang < rp.startAngleRadians) ang += juce::MathConstants<float>::twoPi;
            if (ang <= rp.endAngleRadians)
            {
                const double prop = (ang - rp.startAngleRadians) / (rp.endAngleRadians - rp.startAngleRadians);
                setValue(proportionOfLengthToValue(juce::jlimit(0.0, 1.0, prop)), juce::sendNotificationSync);
            }
        }
    }
    juce::Slider::mouseDown(e);
}

LearnableButton::LearnableButton(const juce::String& text, const juce::String& pid, MidiLearnManager& mlm)
    : juce::TextButton(text), paramId(pid), midiLearn(&mlm)
{}

void LearnableButton::mouseDown(const juce::MouseEvent& e)
{
    if ((e.mods.isRightButtonDown() || e.mods.isPopupMenu()) && midiLearn != nullptr)
    {
        int forced = learnChannelProvider ? learnChannelProvider() : -1;
        showMidiLearnMenu(this, *midiLearn, paramId, forced, altPid, altLabel);
        return;
    }
    juce::TextButton::mouseDown(e);
}

void PatternButton::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(2.0f);

    g.setColour(isCurrent ? juce::Colour(0xff3a66bb) : juce::Colour(0xff20203a));
    g.fillRoundedRectangle(b, 5.0f);
    g.setColour(isCurrent ? juce::Colours::white : juce::Colour(0xff556699));
    g.drawRoundedRectangle(b, 5.0f, isCurrent ? 1.6f : 1.0f);

    g.setColour(isCurrent ? juce::Colours::white : juce::Colours::lightgrey);
    g.setFont(juce::Font(16.0f, juce::Font::bold));
    g.drawText(juce::String(index + 1), b, juce::Justification::centred, false);

    // Playing pattern marker: a small green "play" triangle in the top-left corner,
    // so you can always see what's sounding even when viewing a different pattern.
    if (isPlaying)
    {
        g.setColour(juce::Colour(0xff35d07a));
        juce::Path tri; tri.addTriangle(b.getX() + 4, b.getY() + 3, b.getX() + 4, b.getY() + 11, b.getX() + 11, b.getY() + 7);
        g.fillPath(tri);
    }

    if (midiLearn && midiLearn->isLearning() && midiLearn->getLearningParam() == paramId())
    {
        g.setColour(juce::Colour(0xffffd23b));
        g.drawRoundedRectangle(b, 5.0f, 2.2f);
    }

    // Drop highlight: a pattern is being dragged onto this one (it will be copied here).
    if (dragOver)
    {
        g.setColour(juce::Colour(0xff35d07a));
        g.drawRoundedRectangle(b, 5.0f, 2.4f);
    }
}

void PatternButton::mouseDrag(const juce::MouseEvent& e)
{
    // Drag a pattern's number onto another slot to copy this pattern there (ghost = this button).
    if (e.getDistanceFromDragStart() < 6) return;
    if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this))
        if (! c->isDragAndDropActive())
            c->startDragging("pat" + juce::String(index), this);
}

void PatternButton::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
    {
        if (midiLearn != nullptr)
            showMidiLearnMenu(this, *midiLearn, paramId(), -1); // learn captures its own channel
        return;
    }
    if (e.mods.isShiftDown()) { if (onShiftClick) onShiftClick(); return; }   // MERGE with the previous pattern
    if (onSelect) onSelect();
}

//==============================================================================
// ContentComponent
//==============================================================================

void ContentComponent::paint(juce::Graphics& g) { owner.paintContent(g); }
void ContentComponent::paintOverChildren(juce::Graphics& g) { owner.paintStripOutline(g); }
void ContentComponent::resized()                { owner.layoutContent(); }

//==============================================================================
// ADSRDisplay
//==============================================================================
void ADSRDisplay::setValues(float a, float h, float d, float s, float r)
{
    if (a == atk && h == hld && d == dcy && s == sus && r == rel) return;
    atk = a; hld = h; dcy = d; sus = s; rel = r;
    repaint();
}

// Per-handle band widths (fractions of the inner width minus the fixed Hold gap).
static void adsrBands(float W, float& wA, float& wH, float& wD, float& susW, float& wR)
{
    const float Wu = juce::jmax(1.0f, W - ADSRDisplay::kMinHold);
    // A-H-D-S-R (v1.2.0: Sustain + Release are BACK - the decay settles at the sustain level while
    // a GATE is open: a held KEYS note or a step's Note Length; gate closes -> release. Sustain 0 =
    // exactly the old AHD, so factory sounds draw + play identically). A/H/D share ~72% of the
    // width, a fixed plateau shows the sustain level, release gets the rest.
    wA = Wu * 0.24f; wH = Wu * 0.24f; wD = Wu * 0.24f; susW = Wu * 0.10f; wR = Wu * 0.18f;
}

// Breakpoint positions from the current values. Each time stage has a FIXED skewed
// pixel band (no reflow). The Hold handle always sits >= kMinHold right of Attack so
// it stays visible/grabbable even when Hold == 0.
ADSRDisplay::Geo ADSRDisplay::geom() const
{
    auto in = getLocalBounds().toFloat().reduced(6.0f);
    Geo g;
    g.left = in.getX(); g.top = in.getY() + 5.0f; g.bottom = in.getBottom() - 2.0f;
    g.h = g.bottom - g.top;
    float wA, wH, wD, susW, wR; adsrBands(in.getWidth(), wA, wH, wD, susW, wR);
    if (strikeRing) { wA = in.getWidth() * 0.28f; wD = in.getWidth() * 0.34f; wH = 0.0f;   // 3 bands: Strike | Ring | Release
                     susW = in.getWidth() * 0.06f; wR = in.getWidth() * 0.24f; }
    // Strike (attack) is a SHORT 0..50ms range on a LINEAR axis (spreads evenly); the generic AHD attack keeps its
    // ms-skewed 0..6s axis.
    g.xA = g.left + wA * (strikeRing ? juce::jlimit(0.0f, 1.0f, atk / maxAStrike) : skew(atk / maxA));
    g.xH = strikeRing ? g.xA : (g.xA + kMinHold + wH * skew(hld / maxH));   // Strike/Ring: peak is a single point (no Hold)
    g.xD = g.xH   + wD * skew(dcy / maxD);
    if (strikeRing)
    {   // Strike | Ring(+Sustain on Y) | Release. Physical & Modal both gate now, so Release is a
        // real handle (how fast the note fades when you let go of a key / a Length step ends).
        g.xS = g.xD   + susW;
        g.xR = g.xS   + wR * skew(rel / maxR);
        g.susY = srSus ? g.bottom - g.h * juce::jlimit(0.0f, 1.0f, sus) : g.bottom;
        return g;
    }
    if (noSusRel) { g.xS = g.xD; g.xR = g.xD; g.susY = g.bottom; return g; }   // samples: AHD only
    g.xS = g.xD   + susW;                                        // sustain plateau (fixed width)
    g.xR = g.xS   + wR * skew(rel / maxR);                       // release tail
    g.susY = g.bottom - g.h * juce::jlimit(0.0f, 1.0f, sus);     // decay lands ON the sustain level
    return g;
}

void ADSRDisplay::handlePts(juce::Point<float> out[4]) const
{
    const Geo q = geom();    // 4 handles: Attack, Hold, Decay(x)+Sustain(y), Release.
    out[0] = { q.xA, q.top }; out[1] = { q.xH, q.top }; out[2] = { q.xD, q.susY }; out[3] = { q.xR, q.bottom };
}

int ADSRDisplay::nearestHandle(juce::Point<float> p) const
{
    juce::Point<float> h[4]; handlePts(h);
    int best = -1; float bd = 20.0f * 20.0f;   // only grab/hover within ~20px of a handle (else -1 = none), like the pitch env
    for (int i = 0; i < 4; ++i) { if ((strikeRing && i == 1) || (noSusRel && i == 3)) continue;
        float d = p.getDistanceSquaredFrom(h[i]); if (d < bd) { bd = d; best = i; } }
    return best;
}

// Distinct colour per handle so they're easy to tell apart.
static const juce::Colour kEnvCols[4] = { juce::Colour(0xff35c0ff),   // Attack  - cyan
                                          juce::Colour(0xff5ad17a),   // Hold    - green
                                          juce::Colour(0xffffc23a),   // Decay/S - amber
                                          juce::Colour(0xffff6ec7) }; // Release - pink
static juce::String envTimeStr(float s) { return s < 1.0f ? juce::String(juce::roundToInt(s * 1000.0f)) + " ms"
                                                          : juce::String(s, 2) + " s"; }

void ADSRDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022));    g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a));    g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);

    if (! enabledLook)   // sample slots have no amp envelope - the sample plays naturally
    {
        g.setColour(juce::Colour(0xff60708a)); g.setFont(juce::Font(10.0f, juce::Font::bold));
        g.drawText(naMain, getLocalBounds().reduced(5), juce::Justification::topLeft, false);
        g.setColour(juce::Colour(0xff556070)); g.setFont(juce::Font(11.0f));
        g.drawText(naSub, getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    const Geo q = geom();
    juce::Path p;
    p.startNewSubPath(q.left, q.bottom);
    p.lineTo(q.xA, q.top); p.lineTo(q.xH, q.top);
    p.lineTo(q.xD, q.susY);                                    // decay lands on the SUSTAIN level
    if (! noSusRel) { p.lineTo(q.xS, q.susY); p.lineTo(q.xR, q.bottom); }   // sustain plateau + release tail (Phys/Modal too)
    juce::Path fill = p; fill.lineTo(q.left, q.bottom); fill.closeSubPath();
    g.setColour(juce::Colour(0x3340cc88)); g.fillPath(fill);
    g.setColour(juce::Colour(0xff55ddaa)); g.strokePath(p, juce::PathStrokeType(1.6f));

    juce::Point<float> h[4]; handlePts(h);
    const int active = drag >= 0 ? drag : hover;
    for (int i = 0; i < 4; ++i) {
        if ((strikeRing && i == 1) || (noSusRel && i == 3)) continue;   // no Hold in Strike/Ring; no Release on samples
        const float r = (i == active) ? 5.5f : 4.0f;
        if (i == active) { g.setColour(juce::Colours::white); g.drawEllipse(h[i].x - r - 2, h[i].y - r - 2, (r + 2) * 2, (r + 2) * 2, 1.2f); }
        g.setColour(kEnvCols[i]); g.fillEllipse(h[i].x - r, h[i].y - r, r * 2, r * 2);
        g.setColour(juce::Colours::black); g.drawEllipse(h[i].x - r, h[i].y - r, r * 2, r * 2, 1.0f);
    }
    // Live playhead dots: where each currently-playing voice is in this envelope.
    for (int i = 0; i < numHeads; ++i)
    {
        auto pt = playheadXY(heads[i]);
        g.setColour(juce::Colours::white);                 g.fillEllipse(pt.x - 3.0f, pt.y - 3.0f, 6.0f, 6.0f);
        g.setColour(juce::Colour(0xff35c0ff));             g.drawEllipse(pt.x - 3.5f, pt.y - 3.5f, 7.0f, 7.0f, 1.3f);
    }

    // Total envelope length (top-right). AHD perceptual length: the decay curve is already ~5% (-26 dB) at t=dec,
    // so dec IS the audible decay time - matches the handle read-out (no 3.2x inflation, no release).
    const float total = atk + (strikeRing ? 0.0f : hld) + dcy;
    g.setColour(juce::Colour(0xffcdd8ec)); g.setFont(juce::Font(10.5f, juce::Font::bold));
    // Release shown SEPARATELY: it only plays at a note's END (key-up / Note Length), so it is not
    // part of the one-shot length - but hiding it entirely read as "release does nothing" (user).
    g.drawText("length ~" + envTimeStr(total) + (rel > 0.05f ? "  +rel " + envTimeStr(rel) : juce::String()),
               getLocalBounds().reduced(5).withTrimmedTop(14), juce::Justification::topRight, false);

    if (strikeRing) {   // make it obvious this is the Physical-specific envelope (cue at top-left)
        g.setColour(juce::Colour(0xff7a8aa8)); g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText("STRIKE / RING / REL", getLocalBounds().reduced(6), juce::Justification::topLeft, false);
    }

    // [2026-07-14 23:20] ALWAYS-ON stage read-out (user): every A-H-D-S-R value stacked top-right,
    // each row in its handle's colour; the hovered/dragged stage's row lights WHITE. (The old
    // hover-only single line is gone - the times are readable without grabbing a handle.)
    {
        struct Row { juce::String txt; int handle; };
        Row rows[5]; int n = 0;
        rows[n++] = { (strikeRing ? "Strike " : "A ") + envTimeStr(atk), 0 };
        if (! strikeRing) rows[n++] = { "H " + envTimeStr(hld), 1 };
        rows[n++] = { (strikeRing ? "Ring " : "D ") + envTimeStr(dcy), 2 };
        if (! noSusRel && (! strikeRing || srSus))
            rows[n++] = { (strikeRing ? "Sus " : "S ") + juce::String(juce::roundToInt(sus * 100.0f)) + "%", 2 };
        if (! noSusRel) rows[n++] = { (strikeRing ? "Rel " : "R ") + envTimeStr(rel), 3 };
        auto tr = getLocalBounds().reduced(5).withTrimmedTop(26);
        g.setFont(juce::Font(10.0f, juce::Font::bold));
        for (int i = 0; i < n; ++i)
        {
            g.setColour(rows[i].handle == active ? juce::Colours::white
                                                 : kEnvCols[rows[i].handle].withAlpha(0.92f));
            g.drawText(rows[i].txt, tr.removeFromTop(12), juce::Justification::topRight, false);
        }
    }
}

// Map an elapsed time (seconds) to the matching point ON the drawn A-H-D-S-R polyline.
juce::Point<float> ADSRDisplay::playheadXY(float ts) const
{
    const Geo q = geom();
    const float hldEff = strikeRing ? 0.0f : hld;
    const float aEnd = atk, hEnd = atk + hldEff, dWin = juce::jmax(1.0e-4f, dcy), dEnd = hEnd + dWin;
    auto lp = [](float a, float b, float t) { return a + (b - a) * juce::jlimit(0.0f, 1.0f, t); };
    if (ts <= aEnd) { float p = aEnd > 1.0e-5f ? ts / aEnd : 1.0f;           return { lp(q.left, q.xA, p), lp(q.bottom, q.top, p) }; }
    if (ts <= hEnd) { float p = hldEff > 1.0e-5f ? (ts - aEnd) / hldEff : 1.0f; return { lp(q.xA, q.xH, p), q.top }; }
    if (ts <= dEnd) { float p = (ts - hEnd) / dWin;                       return { lp(q.xH, q.xD, p), lp(q.top, q.susY, p) }; }
    // Sustaining sound: the decay settles at the SUSTAIN level, so the dot parks ON the plateau
    // (release timing isn't knowable from elapsed time alone). Sustain 0 = silence at the end.
    if (q.susY < q.bottom - 0.5f) return { q.xS, q.susY };
    return { q.xD, q.bottom };   // decayed to silence -> rest at the end
}

void ADSRDisplay::setPlayheads(const float* sec, int n)
{
    n = juce::jlimit(0, kMaxHeads, n);
    if (n == 0 && numHeads == 0) return;          // already empty - skip needless repaints
    numHeads = n;
    for (int i = 0; i < n; ++i) heads[i] = sec[i];
    repaint();
}

void ADSRDisplay::mouseMove(const juce::MouseEvent& e)
{
    int h = nearestHandle(e.position);
    if (h != hover) { hover = h; repaint(); }
}

juce::String ADSRDisplay::getTooltip()
{
    // The "length ~X s" read-out is the AMP envelope's own length. A PITCH envelope can change the REAL audible
    // length: on samples the pitch env is VARISPEED, so raising the pitch makes the sound END SOONER and lowering
    // it makes it last LONGER than this envelope shows. Same caveat the pitch-env editor carries - mirror it here.
    const juce::String warn = "  NOTE: a PITCH envelope can change the REAL length - on samples it's varispeed, so "
                              "raising the pitch ends the sound sooner and lowering it lasts longer than the 'length' here.";
    // This graph is the CHANNEL's sound. The sequencer's per-step Len is separate: it RESCALES the
    // decay so a step's note falls across its whole length - so the graph "lies" for those steps
    // (they can last longer OR shorter than drawn here). That's by design; note it for the user.
    const juce::String gateNote = "  Per-step Len (Len mode) is separate: it stretches or tightens the DECAY so that "
                                  "step's note falls across its own length - those steps last longer or shorter than this graph shows.";
    const juce::String tailNote = "  The decay is exponential - at the shown length the sound is at ~5% level, so a "
                                  "quiet tail rings slightly past the moving dot.";
    // SUSTAIN/RELEASE are GATE-driven; nothing gates a one-shot. Spell that out (user request:
    // "why does TEST sometimes ring longer than a key I let go of").
    const juce::String susNote = "  SUSTAIN and RELEASE only act on GATED notes: a HELD KEY (Keys view / MIDI keyboard) "
                                 "or a step with a Length (Len edit mode). One-shots - TEST and plain steps - have no "
                                 "gate, so they ignore Sustain/Release and ring their FULL natural decay. That is why "
                                 "TEST can ring LONGER than a key you let go of: releasing a key fades it with Release, "
                                 "a one-shot just rings out.";
    const int i = drag >= 0 ? drag : hover;
    if (strikeRing) {   // Physical/Modal: Strike(attack) + Ring(decay + sustain on Y), no Hold
        switch (i) {
            case 0: return "Strike (" + envTimeStr(atk) + ") - how soft the pluck/strike is: 0 = sharp pluck, higher = "
                           "a slow swelled strike (the string is held up so it still reaches full volume)";
            case 2: return srSus
                        ? "Ring (" + envTimeStr(dcy) + ") - drag LEFT/RIGHT: how long the string/body rings on its own. "
                          "Drag UP/DOWN: SUSTAIN (" + juce::String((int) std::lround(sus * 100.0f)) + "%) - a HELD key "
                          "(or a step with Length) keeps the sound alive at that level instead of letting it die; "
                          "letting go rings out naturally. Sustain only matters on gated notes - TEST/plain steps ignore it."
                        : "Ring (" + envTimeStr(dcy) + ") - how long the string/body rings after the strike (drag left/right)";
            default: return "Strike/Ring envelope. WHY it's different here: a plucked string / struck body carries its "
                            "OWN natural decay - Strike sets the onset softness, Ring sets that natural decay (and its "
                            "height sets SUSTAIN: a held key keeps the body energised at that level)." + susNote
                            + tailNote + gateNote + warn;
        }
    }
    if (toggleable) {   // sample slot with the opt-in envelope
        if (! enabledLook)
            return "Samples play their full (trimmed) length by default. DOUBLE-CLICK to enable an amp envelope on "
                   "this sample (fade the attack in, tame a long tail) - old sounds are untouched until you do.";
        switch (i) {
            case 0: return "Attack (" + envTimeStr(atk) + ") - fade the sample in";
            case 1: return "Hold (" + envTimeStr(hld) + ") - time held at full before the decay";
            case 2: return "Decay (" + envTimeStr(dcy) + ") - fade the sample's tail out (the sample still ends at its "
                           "own length if that's shorter)." + warn;
            default: return "SAMPLE amp envelope (optional): shapes the sample's level over time. Double-click empty "
                            "space to turn it back OFF (= play the full sample untouched)." + warn;
        }
    }
    switch (i) {
        case 0: return "Attack (" + envTimeStr(atk) + ") - time to rise from silence to full level";
        case 1: return "Hold (" + envTimeStr(hld) + ") - time held at full before the decay";
        case 2: return "Decay (" + envTimeStr(dcy) + ") - drag LEFT/RIGHT: fall time. Drag UP/DOWN: SUSTAIN ("
                       + juce::String((int) std::lround(sus * 100.0f)) + "%) - on a GATED note (held key / step with "
                       "Length) the fall settles there instead of at silence. One-shots ignore Sustain." + tailNote + warn;
        case 3: return "Release (" + envTimeStr(rel) + ") - fade-out time AFTER the gate ends (you let go of a key, or "
                       "a step's Length runs out). One-shots (TEST, plain steps) never reach it.";
        default: return "Amp envelope: drag the coloured handles to shape Attack / Hold / Decay+Sustain / Release. The "
                        "'length ~X s' is this envelope's length." + susNote + tailNote + gateNote + warn;
    }
}

void ADSRDisplay::mouseDown(const juce::MouseEvent& e)
{
    if ((e.mods.isRightButtonDown() || e.mods.isPopupMenu()) && mlm != nullptr)
    {   // MIDI-learn: the 5 envelope parameters, applied to the SELECTED slot (ui_sel_env*)
        auto tag = [this](const char* pid) {
            const int cc = mlm->getCCForParam(pid);
            return cc < 0 ? juce::String()
                          : "   [ch" + juce::String(mlm->getChannelForParam(pid)) + " cc" + juce::String(cc) + "]";
        };
        juce::PopupMenu m;
        m.addSectionHeader("MIDI-learn (acts on the SELECTED slot)");
        m.addItem(1, "Attack..."  + tag("ui_sel_envA"));
        m.addItem(2, "Hold..."    + tag("ui_sel_envH"));
        m.addItem(3, "Decay..."   + tag("ui_sel_envD"));
        m.addItem(4, "Sustain..." + tag("ui_sel_envS"));
        m.addItem(5, "Release..." + tag("ui_sel_envR"));
        m.addSeparator(); m.addItem(6, "Clear these assignments");
        auto mp = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
        auto* L = mlm;
        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({ mp.x, mp.y, 1, 1 }).withMinimumWidth(240),
            [L](int r) {
                static const char* pids[5] = { "ui_sel_envA", "ui_sel_envH", "ui_sel_envD", "ui_sel_envS", "ui_sel_envR" };
                if (r >= 1 && r <= 5) L->startLearning(pids[r - 1]);
                else if (r == 6) for (auto* p : pids) L->clearParam(p);
            });
        return;
    }
    if (! enabledLook) return;   // sample slots: no amp envelope to edit
    drag = nearestHandle(e.position);
    mouseDrag(e);
}

// Sample slots: the amp envelope is OPT-IN - double-click the graph to enable/disable it.
// When enabled, double-clicking EMPTY space (not a handle) turns it back off.
void ADSRDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (! toggleable || ! onToggleRequest) return;
    if (enabledLook && nearestHandle(e.position) >= 0) return;   // double-click on a handle = not a toggle
    onToggleRequest();
}

void ADSRDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (drag < 0) return;
    const Geo q = geom();
    const float bw = getLocalBounds().toFloat().reduced(6.0f).getWidth();
    float wA, wH, wD, susW, wR; adsrBands(bw, wA, wH, wD, susW, wR);
    if (strikeRing) { wA = bw * 0.28f; wD = bw * 0.34f; susW = bw * 0.06f; wR = bw * 0.24f; }   // match geom() Strike|Ring|Release
    switch (drag)
    {
        case 0: atk = strikeRing ? maxAStrike * juce::jlimit(0.0f, 1.0f, (e.position.x - q.left) / wA)
                                 : maxA * invSkew((e.position.x - q.left) / wA); break;
        case 1: hld = maxH * invSkew((e.position.x - q.xA - kMinHold) / wH); break;
        case 2: dcy = maxD * invSkew((e.position.x - q.xH) / wD);          // x = decay time
                if ((! strikeRing && ! noSusRel) || (strikeRing && srSus))  // y = SUSTAIN level (0 = classic AHD)
                    sus = juce::jlimit(0.0f, 1.0f, (q.bottom - e.position.y) / juce::jmax(1.0f, q.h));
                break;
        case 3: if (! noSusRel) rel = maxR * invSkew((e.position.x - q.xS) / wR); break;   // x = release time
        default: break;
    }
    atk = juce::jlimit(0.0005f, strikeRing ? maxAStrike : maxA, atk); hld = juce::jlimit(0.0f, maxH, hld);
    dcy = juce::jlimit(0.002f, maxD, dcy);  rel = juce::jlimit(0.005f, maxR, rel);
    if (strikeRing) hld = 0.0f;   // Strike/Ring has no Hold stage
    if (onChange) onChange(atk, hld, dcy, sus, rel);
    repaint();
}

//==============================================================================
// SlotSelector
//==============================================================================
void SlotSelector::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const int n = juce::jmax(1, labels.size());
    const float w = b.getWidth() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        // Per-slot accent: "1" = yellow, "2" = pink, anything else (e.g. "All") = blue.
        const juce::String& t = labels[i];
        const juce::Colour accent = (t == "1") ? juce::Colour(0xffffd24a)
                                  : (t == "2") ? juce::Colour(0xffff5fa6)
                                               : juce::Colour(0xff35c0ff);
        auto r = juce::Rectangle<float>(b.getX() + i * w, b.getY(), w, b.getHeight()).reduced(1.5f);
        const bool seld = (i == sel);
        g.setColour(juce::Colour(0xff1b1b2c));                 // opaque base so the colour never mixes with the bg
        g.fillRoundedRectangle(r, 3.0f);
        if (seld) { g.setColour(accent); g.fillRoundedRectangle(r, 3.0f); }          // selected = filled accent
        g.setColour(accent.withAlpha(seld ? 1.0f : 0.85f)); g.drawRoundedRectangle(r, 3.0f, seld ? 1.4f : 1.1f);
        g.setColour(seld ? juce::Colours::black : accent);    // text in the accent colour when idle
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText(t, r, juce::Justification::centred, false);
    }
}

void SlotSelector::mouseDown(const juce::MouseEvent& e)
{
    const int n = juce::jmax(1, labels.size());
    int i = juce::jlimit(0, n - 1, (int) (e.position.x / juce::jmax(1.0f, getWidth() / (float) n)));
    if (onSelect) onSelect(i);
}

//==============================================================================
// LevelMeter
//==============================================================================
void LevelMeter::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0c0c14));
    g.fillRoundedRectangle(r, 1.5f);

    const float lv = juce::jlimit(0.0f, 1.0f, level);
    const float pk = juce::jlimit(0.0f, 1.0f, peak);
    const juce::Colour green (0xff35d873), amber (0xfff2b134), red (0xffff3b30);

    if (horizontal)
    {
        // Strips: a real fader TRACK (so the handle lives IN something instead of floating),
        // with the level bar drawn slim inside it and the full-height cyan volume handle.
        if (onVolume)
        {
            g.setColour (juce::Colour (0xff20203a)); g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colour (0xff33335a)); g.drawRoundedRectangle (r.reduced (0.4f), 3.0f, 0.8f);
        }
        const auto bar = onVolume ? r.reduced (1.0f).withTop (r.getBottom() - 5.0f) : r;
        // The VOLUME HANDLE is the level bar's CEILING on the strips (user design): the fill can
        // never pass the handle - pull the handle down and the whole bar squeezes with it.
        const float capW = onVolume ? r.getWidth() * gainToPos(vol) : r.getWidth();   // dB-taper position [2026-07-14]
        // green->amber->red gradient laid over the capped span, clipped to the filled portion.
        juce::ColourGradient grad (green, bar.getX(), 0.0f, red, bar.getX() + juce::jmax (8.0f, capW), 0.0f, false);
        grad.addColour (0.78, amber);
        g.setGradientFill (grad);
        g.fillRect (bar.withWidth (capW * lv));
        if (pk > 0.01f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            const float px = bar.getX() + capW * pk;
            g.fillRect (juce::Rectangle<float> (px - 0.75f, bar.getY(), 1.5f, bar.getHeight()));
        }
        if (onVolume)   // strips only (the logo master meter has no volume handle)
        {
            // UNITY (100%) marker: a clearly visible white tick (user: the old one was too faint).
            const float ux = r.getX() + r.getWidth() * 0.75f;   // unity notch = the 0 dB point of the taper
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (ux - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 3.0f));
            const float vx = r.getX() + r.getWidth() * gainToPos(vol);
            g.setColour (juce::Colour (0xff35c0ff));
            g.fillRoundedRectangle (vx - 2.2f, r.getY(), 4.4f, r.getHeight(), 1.6f);   // full-height, wide = grabbable
        }
    }
    else
    {
        juce::ColourGradient grad (red, 0.0f, r.getY(), green, 0.0f, r.getBottom(), false);
        grad.addColour (0.22, amber);
        g.setGradientFill (grad);
        const float h = r.getHeight() * lv;
        g.fillRect (r.withTop (r.getBottom() - h));
        if (pk > 0.01f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            const float py = r.getBottom() - r.getHeight() * pk;
            g.fillRect (juce::Rectangle<float> (r.getX(), py - 0.75f, r.getWidth(), 1.5f));
        }
    }
}

//==============================================================================
// PitchEnvDisplay
//==============================================================================
void PitchEnvDisplay::setDots(const float* pitch, const float* timeFrac)
{
    bool same = true;
    for (int i = 0; i < NDOT; ++i) if (p[i] != pitch[i] || t[i] != timeFrac[i]) same = false;
    if (same) return;
    for (int i = 0; i < NDOT; ++i) { p[i] = pitch[i]; t[i] = timeFrac[i]; }
    repaint();
}

void PitchEnvDisplay::setPlayheads(const float* secs, int n)
{
    n = juce::jlimit(0, kMaxHeads, n);
    if (n == 0 && nHeads == 0) return;
    nHeads = n;
    for (int i = 0; i < n; ++i) heads[i] = secs[i];
    repaint();
}

PitchEnvDisplay::Geo PitchEnvDisplay::geom() const
{
    auto in = getLocalBounds().toFloat().reduced(6.0f);
    Geo g;
    g.left = in.getX(); g.right = in.getRight(); g.top = in.getY() + 4.0f; g.bottom = in.getBottom() - 4.0f;
    g.cy = (g.top + g.bottom) * 0.5f; g.hh = (g.bottom - g.top) * 0.5f;
    return g;
}

// Polyline anchored at 0 on both edges: (left,0) -> the 4 dots -> (right,0).
void PitchEnvDisplay::buildCurve(juce::Path& path, const Geo& q) const
{
    path.startNewSubPath(q.left, yForP(q, 0.0f));
    for (int i = 0; i < NDOT; ++i) path.lineTo(xForT(q, t[i]), yForP(q, p[i]));
    path.lineTo(q.right, yForP(q, 0.0f));
}

int PitchEnvDisplay::nearestHandle(juce::Point<float> pos) const
{
    const Geo q = geom();
    int best = -1; float bd = 18.0f * 18.0f;   // only grab within ~18px
    for (int i = 0; i < NDOT; ++i) {
        const juce::Point<float> h(xForT(q, t[i]), yForP(q, p[i]));
        const float d = pos.getDistanceSquaredFrom(h);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void PitchEnvDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
    const Geo q = geom();
    const float a = enabledLook ? 1.0f : 0.30f;

    // 0-semitone centre line + faint vertical grid at 25/50/75% of the sound length.
    g.setColour(juce::Colour(0xff2a2a44)); g.drawHorizontalLine((int) q.cy, q.left, q.right);
    g.setColour(juce::Colour(0xff20203a));
    for (int k = 1; k <= 3; ++k) { float x = xForT(q, 0.25f * k); g.drawVerticalLine((int) x, q.top, q.bottom); }

    // Beginner axis hint (faint watermark in the bottom-left corner): which way is time, which way is pitch.
    // Drawn BEFORE the curve so the dots/line sit on top, and kept low + low-alpha so it doesn't fight them.
    {
        const float ox = q.left + 7.0f, oy = q.bottom - 5.0f, hLen = 30.0f, vLen = 22.0f;
        g.setColour(juce::Colour(0xff6678a0).withAlpha(0.5f * a));
        g.drawArrow(juce::Line<float>(ox, oy, ox + hLen, oy), 1.0f, 5.0f, 4.0f);   // time  ->
        g.drawArrow(juce::Line<float>(ox, oy, ox, oy - vLen), 1.0f, 5.0f, 4.0f);   // pitch  ^
        g.setFont(juce::Font(8.5f, juce::Font::bold));
        g.drawText("time",  juce::Rectangle<float>(ox + 5, oy - 12,        30, 10), juce::Justification::centredLeft, false);
        g.drawText("pitch", juce::Rectangle<float>(ox + 5, oy - vLen - 3,  34, 10), juce::Justification::centredLeft, false);
    }

    juce::Path path; buildCurve(path, q);
    g.setColour(juce::Colour(0xff7aa0ff).withAlpha(a)); g.strokePath(path, juce::PathStrokeType(1.7f));

    // Playhead dots riding the curve (one per voice while the sound plays).
    if (enabledLook)
        for (int i = 0; i < nHeads; ++i) {
            const float f = juce::jlimit(0.0f, 1.0f, heads[i] / lenSec);
            const float x = xForT(q, f), y = yForP(q, DrumChannel::pitchEnv4(f, p, t));
            g.setColour(juce::Colours::white);        g.fillEllipse(x - 2.6f, y - 2.6f, 5.2f, 5.2f);
            g.setColour(juce::Colour(0xff35c0ff));    g.drawEllipse(x - 3.6f, y - 3.6f, 7.2f, 7.2f, 1.2f);
        }

    const juce::Colour cols[NDOT] = { juce::Colour(0xff35c0ff), juce::Colour(0xff5ad17a),
                                      juce::Colour(0xffffc23a), juce::Colour(0xffff7ab0) };
    const int active = drag >= 0 ? drag : hover;
    for (int i = 0; i < NDOT; ++i) {
        const juce::Point<float> h(xForT(q, t[i]), yForP(q, p[i]));
        const float r = (i == active) ? 5.5f : 4.0f;
        if (i == active) { g.setColour(juce::Colours::white); g.drawEllipse(h.x - r - 2, h.y - r - 2, (r + 2) * 2, (r + 2) * 2, 1.2f); }
        g.setColour(cols[i].withAlpha(a)); g.fillEllipse(h.x - r, h.y - r, r * 2, r * 2);
        g.setColour(juce::Colours::black);  g.drawEllipse(h.x - r, h.y - r, r * 2, r * 2, 1.0f);
    }
    g.setColour(juce::Colour(0xffcdd8ec)); g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.drawText(enabledLook ? "PITCH" : "PITCH (n/a)", getLocalBounds().reduced(5), juce::Justification::topLeft, false);
    if (active >= 0 && enabledLook) {
        const float ms = t[active] * lenSec * 1000.0f;
        juce::String s = juce::String(juce::roundToInt(p[active])) + " st  @ "
                       + juce::String(juce::roundToInt(t[active] * 100.0f)) + "%"
                       + (ms < 1000.0f ? " (" + juce::String(juce::roundToInt(ms)) + "ms)"
                                       : " (" + juce::String(ms / 1000.0f, 2) + "s)");
        g.setColour(cols[active]); g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.drawText(s, getLocalBounds().reduced(5), juce::Justification::topRight, false);
    }
}

void PitchEnvDisplay::mouseMove(const juce::MouseEvent& e) { int h = nearestHandle(e.position); if (h != hover) { hover = h; repaint(); } }

juce::String PitchEnvDisplay::getTooltip()
{
    // On SAMPLES the pitch envelope is VARISPEED: it changes pitch AND speed/length together (a rise plays
    // faster, a drop slower - that's what makes 808 pitch-drops work). Synth engines just change frequency.
    // For pitch WITHOUT a length change, use the channel Pitch knob (SoundTouch pitch-shift) instead.
    const juce::String warn = "  NOTE: on samples this is VARISPEED - pitch & speed/length move together "
                              "(use the Pitch knob for pitch without a length change).";
    const int i = drag >= 0 ? drag : hover;
    if (! enabledLook) return "This slot's engine has no pitch (e.g. Noise).";
    if (i >= 0) return "Dot " + juce::String(i + 1) + ": drag UP/DOWN for pitch (semitones), LEFT/RIGHT for WHEN it happens "
                       "(% of the sound's length). The line starts and ends at normal (0)." + warn;
    return "Pitch over time. LEFT-to-RIGHT = WHEN (0-100% of the sound's length, auto-scales with the amp envelope); "
           "UP/DOWN = pitch in semitones. The line begins and ends on the centre (0) line; drag the 4 dots to shape a "
           "drop, rise, or wobble. The white dot shows playback position." + warn;
}

void PitchEnvDisplay::mouseDown(const juce::MouseEvent& e) { drag = nearestHandle(e.position); if (drag >= 0) mouseDrag(e); }

// Double-click empty space (not on a dot) -> reset the pitch envelope to flat (no pitch movement).
void PitchEnvDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (! enabledLook || nearestHandle(e.position) >= 0) return;   // ignore when n/a or when on a dot
    for (int i = 0; i < NDOT; ++i) { p[i] = 0.0f; t[i] = 0.2f * (float)(i + 1); }
    if (onChange) onChange(p, t);
    if (onDragEnd) onDragEnd();
    repaint();
}

void PitchEnvDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (drag < 0) return;
    const Geo q = geom();
    p[drag] = juce::jlimit(-maxSt, maxSt, -(e.position.y - q.cy) / q.hh * maxSt);
    float f = juce::jlimit(0.0f, 1.0f, (e.position.x - q.left) / juce::jmax(1.0f, q.right - q.left));
    // keep the dots ordered left->right (the 4 dots split the timeline into 5 spaces)
    const float lo = (drag > 0)        ? t[drag - 1] : 0.0f;
    const float hi = (drag < NDOT - 1) ? t[drag + 1] : 1.0f;
    t[drag] = juce::jlimit(lo, hi, f);
    if (onChange) onChange(p, t);
    repaint();
}

//==============================================================================
// VoiceModDisplay - unison / detune / vibrato as an interactive picture
//==============================================================================
// Mirror of the DSP chord table (DrumChannel.cpp kChordTab) for the display + interval read-out.
static const int8_t kUiChordTab[7][6] = {
    { 0,12,24,36,48,60 }, { 0,7,12,19,24,31 }, { 0,4,7,12,16,19 }, { 0,3,7,12,15,19 },
    { 0,5,7,12,17,19 }, { 0,4,7,11,12,16 }, { 0,3,7,10,12,15 } };
static const char* kUiChordName[8] = { "STD", "Oct", "5th", "Maj", "Min", "Sus4", "Maj7", "Min7" };
static inline int uiChordSemis(int chord, int k)
{ return (chord >= 1 && chord <= 7) ? (int) kUiChordTab[chord - 1][juce::jlimit(0, 5, k)] : 0; }

// SCALE mode (diatonic harmonizer) display mirror of the DSP kScaleTab. kNumScales must match.
static const int8_t kUiScaleTab[10][7] = {
    { 0,2,4,5,7,9,11 },{ 0,2,3,5,7,8,10 },{ 0,2,3,5,7,8,11 },{ 0,2,3,5,7,9,10 },{ 0,1,3,5,7,8,10 },
    { 0,2,4,6,7,9,11 },{ 0,2,4,5,7,9,10 },{ 0,2,4,7,9,0,0 },{ 0,3,5,7,10,0,0 },{ 0,3,5,6,7,10,0 } };
static const int   kUiScaleLen[10]   = { 7,7,7,7,7,7,7,5,5,6 };
static const char* kUiScaleName[13]  = { "Major","Minor","Har Min","Dorian","Phrygian","Lydian","Mixolyd","Maj Pent","Min Pent","Blues","Gtr Major","Gtr Minor","Power" };
// (guitar voicings are DIATONIC now - the display cluster shows the TONIC chord: see uiScaleSemis)
static const char* kUiNoteName[12]   = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
static constexpr int kNumScales = 13;   // 10 scales + 2 GUITAR VOICINGS + POWER (root+5th+oct, chromatic)
// The TONIC diatonic chord's k-th tone (semitones from the tonic) - a REPRESENTATIVE voicing for the
// display cluster + read-out (the real per-note voicing is note-dependent - see DrumChannel scaleSemis).
static inline int uiScaleSemis(int scaleType, int k) {
    scaleType = juce::jlimit(0, kNumScales - 1, scaleType);
    if (scaleType == 12)
    {   // POWER: root + 5th + octave stack (chromatic - mirror of the DSP kPower table)
        static const int kPower[7] = { 0, 7, 12, 19, 24, 31, 36 };
        return kPower[juce::jlimit(0, 6, k)];
    }
    if (scaleType >= 10)
    {   // guitar voicing display = the TONIC barre (diatonic 3rd/5th of the parent scale)
        const int8_t* S = kUiScaleTab[scaleType - 10];
        const int third = S[2], fifth = S[4];
        static const int base[6] = { 0, 0, 12, 12, 12, 24 };
        const int kk = juce::jlimit(0, 5, k);
        return base[kk] + (kk == 1 ? fifth : kk == 3 ? third : kk == 4 ? fifth : 0);
    }
    const int8_t* S = kUiScaleTab[scaleType]; const int N = kUiScaleLen[scaleType];
    const int td = 2 * k, oct = td / N, idx = td - oct * N;
    return (int) S[idx] + 12 * oct;
}

void VoiceModDisplay::setValues(int unison, int chordUnison, int scaleUnison, float detune, float vibrato, bool centreOn, int detuneMode, int chordMode, bool scaleOnIn, int scaleTypeIn, int scaleKeyIn, float uniSpreadIn, float driftIn)
{
    uni = juce::jlimit(1, maxUni, unison);
    uniChord = juce::jlimit(1, maxUni, chordUnison);
    uniScale = juce::jlimit(1, maxUni, scaleUnison);
    det = juce::jlimit(0.0f, 1.0f, detune);
    uniWidth = juce::jlimit(0.0f, 1.0f, uniSpreadIn);
    driftAmt = juce::jlimit(0.0f, 1.0f, driftIn);
    vib = juce::jlimit(0.0f, 1.0f, vibrato);
    centre = centreOn;
    mode = juce::jlimit(0, 2, detuneMode);
    // The visual is UNISON-only now (chord/scale editing moved to the SCALE box above the keyboard).
    // Chord/scale values are STASHED for emit() pass-through so editing detune never clobbers them.
    emitChord = juce::jlimit(0, 7, chordMode);
    emitScaleOn = scaleOnIn;
    emitScaleType = juce::jlimit(0, kNumScales - 1, scaleTypeIn);
    emitScaleKey = ((scaleKeyIn % 12) + 12) % 12;
    chord = 0; scaleOn = false;
    scaleType = emitScaleType; scaleKey = emitScaleKey;
    repaint();
}
void VoiceModDisplay::setDriftLive(const float* cents, int n)
{
    n = juce::jlimit(0, 17, n);
    bool ch = (n != driftLiveN);
    for (int i = 0; i < n && ! ch; ++i) ch = std::abs(cents[i] - driftLive[i]) > 0.05f;
    if (! ch) return;
    driftLiveN = n;
    for (int i = 0; i < n; ++i) driftLive[i] = cents[i];
    if (driftAmt > 0.001f) repaint();   // only worth repainting when drift is audible
}
void VoiceModDisplay::setModLive(float det, float vib, float width, float drift, float uniCnt)
{
    const float v[4] = { det, vib, width, drift };
    bool ch = false;
    for (int i = 0; i < 4; ++i) { const float a = (v[i] < 0.0f ? -1.0f : v[i]);
                                  if (std::abs(a - modLive[i]) > 0.002f) { modLive[i] = a; ch = true; } }
    if (std::abs(uniCnt - modUniCnt) > 0.01f) { modUniCnt = uniCnt; ch = true; }   // [2026-07-14] Unison Count target
    if (ch) repaint();
}
void VoiceModDisplay::setSupport(bool uniSupported, bool vibSupported, juce::String naReason)
{
    if (uniSupported == uniOn && vibSupported == vibOn && naReason == reason) return;
    uniOn = uniSupported; vibOn = vibSupported; reason = naReason; repaint();
}
VoiceModDisplay::Geo VoiceModDisplay::geom() const
{
    auto b = getLocalBounds().toFloat().reduced(8.0f, 6.0f);
    Geo q;
    // q.top sits BELOW the mode chips (chip band = b.top+3 .. b.top+16) so a dot dragged all the way up
    // (and its label ~20 px above it) can't overlap the UNISON/CHORD/SCALE chips.
    q.left = b.getX(); q.right = b.getRight(); q.top = b.getY() + 22.0f; q.bottom = b.getBottom() - 2.0f;
    q.uniTop = q.top + 8.0f;   // the unison dot (left) tops out a bit LOWER so it clears the chips too
    q.cy = (q.top + q.bottom) * 0.5f; q.hh = (q.bottom - q.top) * 0.5f - 2.0f;
    const float w = q.right - q.left;
    q.uX = q.left + w * 0.10f;   // Unison handle (left)
    q.dX = q.left + w * 0.30f;   // Detune handle home X
    q.vX = q.left + w * 0.52f;   // Vibrato handle
    q.wX = q.left + w * 0.72f;   // WIDTH handle (stereo spread of the unison voices)
    q.xX = q.left + w * 0.92f;   // DRIFT handle (right; per-note randomness = "alive")
    q.rangeX = (q.vX - q.dX) * 0.7f;
    q.rangeY = q.hh * 0.85f;
    // In CHORD/SCALE the root line drops LOW so the value dots (chord/scale type) + the vibrato dot start
    // there and get dragged UP - more travel = easier picking. In STD the root line is centred (old feel).
    const bool stacked = (scaleOn || chord > 0);
    q.rootY   = stacked ? q.cy + q.hh * 0.6f : q.cy;
    q.upRange = juce::jmax(10.0f, q.rootY - q.top - 6.0f);
    q.dPtX = q.dX;
    q.dPtY = scaleOn ? q.rootY - ((float) scaleType / (float)(kNumScales - 1)) * q.upRange   // SCALE: dot height = scale type
           : (chord > 0) ? q.rootY - ((float)(chord - 1) / 6.0f) * q.upRange                 // CHORD: dot height = chord type
                         : q.rootY - det * q.upRange;                                          // STD: dot height = cents
    return q;
}
int VoiceModDisplay::nearestHandle(juce::Point<float> p) const
{
    const Geo q = geom();
    juce::Point<float> pts[5] = {
        { q.uX, q.bottom - (float)(curUni() - 1) / (float)(juce::jmax(1, maxUni - 1)) * (q.bottom - q.uniTop) },  // 0 Unison
        { q.dPtX, q.dPtY },                                                                 // 1 Detune (mode-aware)
        { q.vX, q.rootY - vib * q.upRange },                                                // 2 Vibrato
        { q.wX, q.rootY - uniWidth * q.upRange },                                           // 3 Width (stereo)
        { q.xX, q.rootY - driftAmt * q.upRange } };                                         // 4 Drift (alive)
    int best = -1; float bd = 22.0f * 22.0f;
    for (int i = 0; i < 5; ++i) {
        if (((i == 0 || i == 1 || i == 3 || i == 4) && !uniOn) || (i == 2 && !vibOn)) continue;
        const float dx = p.x - pts[i].x, dy = p.y - pts[i].y, d2 = dx * dx + dy * dy;
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best;
}
juce::String VoiceModDisplay::getTooltip()
{
    if (!uniOn && !vibOn) return "Voice: " + reason;
    // Per-CHIP tooltips (the three mode buttons + the Key chip each explain themselves).
    if (chipHover == 0)
        return "UNISON mode: stack detuned copies of the note for a thicker / wider sound. The count (drag the "
               "cyan dot) = how many voices; Detune spreads them apart. No chord - just the one note, fattened.";
    if (chipHover == 1)
        return "CHORD mode: the stacked voices play a FIXED chord shape on EVERY note. Drag the detune dot to pick "
               "the TYPE (Oct / 5th / Maj / Min / Sus4 / Maj7 / Min7); the unison count = how many chord notes "
               "(3 = triad, 4 = 7th). Note: it's the SAME chord quality on every note you play.";
    if (chipHover == 2)
        return "SCALE mode (diatonic HARMONIZER): set a Key + a scale, and each note is voiced with the chord of "
               "ITS scale degree - so a melody auto-harmonises in key (in C major: C -> major, D -> minor, "
               "E -> minor, ...). Off-key notes snap into the scale. Count = chord size (3 triads / 4 sevenths). "
               "Drag the dot to pick the scale:\n"
               "- Major / Minor: the everyday happy / sad scales.\n"
               "- Har Min: minor with a raised 7th (classical / exotic, Middle-Eastern flavour).\n"
               "- Dorian: minor but brighter (jazzy, folky, Santana-ish).\n"
               "- Phrygian: dark, Spanish / flamenco / metal.\n"
               "- Lydian: major but dreamy / floaty (film-score).\n"
               "- Mixolyd(ian): major but bluesy (rock / funk).\n"
               "- Maj / Min Pent: 5-note scales that (almost) never clash - safe + simple.\n"
               "- Blues: the 6-note bluesy scale.";
    if (chipHover == 3)
        return "KEY: the root note of the scale (the 'home' note - e.g. C for C major). Notes you play snap into "
               "this key. Click to change it.";
    // Per-handle tooltips.
    if (hover == 0 && uniOn)
        return "Count (cyan dot): drag up/down for how many voices are stacked (up to 7 / 3). More = thicker/wider. "
               "In CHORD/SCALE mode this = how many chord notes.";
    if (hover == 1 && uniOn)
        return scaleOn ? "Scale (green dot): drag up/down to choose the scale (Major ... Blues). See the SCALE chip "
                         "tooltip for what each one sounds like."
             : (chord > 0) ? "Chord type (violet dot): drag up/down to choose the chord (Oct/5th/Maj/Min/Sus4/Maj7/Min7)."
             : "Detune (amber dot): drag up to spread the unison voices apart (both sharp + flat). The cents value "
               "is how far the outermost voice sits from the original, each way (100c = 1 semitone).";
    if (hover == 2 && vibOn)
        return "Vibrato (pink dot): drag up for more pitch wobble at ~5.5 Hz. Shown in semitones (up to ~1.5 st).";
    if (hover == 3 && uniOn)
        return "Width (teal dot): STEREO spread of the stacked voices - drag up and the unison/chord notes fan "
               "out across the stereo field in ALTERNATING +/- pairs (like the big synths), so both sides get "
               "sharp AND flat voices and the image never leans. Equal-power panning; the voice lines show "
               "their real side (BLUE = left, ORANGE = right). 0% = mono, exactly the old sound. The "
               "sub-oscillator always stays centred so the bass keeps its punch.\n\n"
               "Needs more than one voice. Width DOES work with Detune at 0 (the voices' fixed phase "
               "offsets give a subtle, static width), but it really opens up with some Detune - or "
               "Chord/Scale notes - which is also when the lines here visibly fan apart.";
    if (hover == 4 && uniOn)
        return "DRIFT (orange dot): every note rolls its own dice, like analog hardware - 0% = perfectly "
               "repeating hits (the drum default). The recipe at 100% (all scale with the dot):\n\n"
               "- The whole note lands up to +-8 cents off per hit (the clearest cue) + each unison voice "
               "another +-15 cents + a +-12 cent slow wander.\n"
               "- Unison start phases scatter (full blur from ~50%).\n"
               "- The FILTER cutoff moves up to +-0.6 octave per note (needs a filter on).\n"
               "- The level breathes up to +-30% per note.\n\n"
               "HOW TO HEAR IT: put Drift at 60%+, press TEST repeatedly - each hit differs. Strongest "
               "with unison 3+ (blur) and a filter on (tone changes per note). One dot = the whole "
               "effect; the numbers are its fixed design, like the chorus knob's rate.";
    juce::String s = "Voice controls for the selected slot. Hover the UNISON / CHORD / SCALE chips for what each mode "
                     "does. ";
    if (vibOn) s += "Vibrato = ~5.5 Hz pitch wobble (works on every engine here).";
    return s;
}
void VoiceModDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);

    if (!uniOn && !vibOn) {   // engine has no pitch / not supported -> say so (like pitch-env n/a)
        g.setColour(juce::Colour(0xff6a6a86)); g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText(reason, b, juce::Justification::centred, false);   // just the reason (the header already names the controls)
        return;
    }
    const Geo q = geom();
    // In CHORD/SCALE the voices only go UP from the root, so drop the root/original line LOW to give the
    // chord notes room to spread above it. In STD (unison) the spread is symmetric, so it stays centred.
    const bool  stacked = (scaleOn || chord > 0);
    const float rootY   = q.rootY;
    g.setColour(juce::Colour(0x14ffffff)); g.drawHorizontalLine((int) rootY, q.left, q.right);  // root/original pitch line

    // The voice cluster: `uni` lines spread symmetrically by detune, each wobbling by vibrato.
    const int n = uniOn ? curUni() : 1;
    const float spread  = uniOn ? det * q.hh * 0.85f : 0.0f;
    const float vibAmp  = vibOn ? vib * q.hh * 0.30f : 0.0f;
    // pan: -1 (hard left) .. +1 (hard right). The line's HORIZONTAL SPAN moves with it: a panned
    // voice occupies only its side of the box (full width at pan 0 = the old look), so at high Width
    // the lines visibly FAN out across the stereo field instead of just changing colour.
    auto drawVoice = [&](float off, juce::Colour col, float thick, float pan = 0.0f) {
        const float boxW = q.right - q.left;
        const float spanW = boxW * (1.0f - 0.45f * std::abs(pan));          // panned = shorter line...
        const float x0 = q.left + juce::jmax(0.0f, pan) * (boxW - spanW);   // ...pushed to its side
        juce::Path path;
        const int Wv = juce::jmax(2, (int) spanW);
        for (int xi = 0; xi <= Wv; xi += 2) {
            const float x = x0 + (float) xi;
            const float ph = (float) xi / (float) Wv * juce::MathConstants<float>::twoPi * 3.0f;
            const float y = rootY + off + std::sin(ph) * vibAmp;
            if (xi == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
        }
        g.setColour(col); g.strokePath(path, juce::PathStrokeType(thick));
    };
    // Stacked (chord/scale): scale the notes so the TOP voice reaches near the top of the box and the
    // root sits on the low line - so the intervals fill the space and read clearly whatever the chord.
    int maxSemi = 1;
    if (stacked) for (int k = 0; k < n; ++k) maxSemi = juce::jmax(maxSemi, scaleOn ? uiScaleSemis(scaleType, k) : uiChordSemis(chord, k));
    const float availUp = q.upRange;
    for (int k = 0; k < n; ++k) {
        // sp = this voice's REAL detune position (the DSP's law: 0 = symmetric, 1 = all sharp,
        // 2 = all flat). It drives BOTH the line height (up = sharper) and the stereo pan below,
        // so the picture matches the sound: flat voices sit low-LEFT, sharp voices high-RIGHT.
        const float fr = (n > 1) ? (float) k / (float)(n - 1) : 0.0f;
        const float sp = (n > 1) ? ((mode == 1) ? fr : (mode == 2) ? -fr : (2.0f * fr - 1.0f)) : 0.0f;
        float off = -sp * spread;
        const int voiceSemi = scaleOn ? uiScaleSemis(scaleType, k) : (chord > 0 ? uiChordSemis(chord, k) : 0);
        if (stacked)   // chord/scale: the lines sit at their chord notes (root on the low line, going up)
            off = -(float) voiceSemi / (float) maxSemi * availUp + off * 0.15f;
        const bool isRoot = stacked ? (voiceSemi % 12 == 0) : (n % 2 == 1 && k == n / 2);
        // DRIFT honesty: nudge each line by the DSP's REAL rolled cents for this voice (the last
        // hit's dice) - the picture moves per hit exactly as much as the sound does. 100c = the
        // same pixel scale the Detune spread uses.
        if (driftAmt > 0.001f && k < driftLiveN)
            off -= driftLive[k] * (q.hh * 0.85f / 100.0f);
        // WIDTH visual: the voice's PAN moves its line to its side of the box (geometry, not just
        // colour) with a hue shift as reinforcement. Width 0 = full-width plain cyan = the old look.
        float p = 0.0f;
        juce::Colour vc = juce::Colour(isRoot ? 0xff35c0ff : 0x9935c0ff);
        if (uniWidth > 0.001f && n > 1) {
            const int pairIdx = juce::jmin(k, n - 1 - k);        // alternate +/- pairs across the
            p = ((pairIdx & 1) ? -sp : sp) * uniWidth;           // sides, exactly like the DSP bake
            vc = vc.interpolatedWith(p < 0.0f ? juce::Colour(0xff4a7fff) : juce::Colour(0xffff9f43),
                                     juce::jlimit(0.0f, 1.0f, std::abs(p)) * 0.7f);
        }
        drawVoice(off, vc.withMultipliedAlpha(isRoot ? 1.0f : 0.55f), isRoot ? 1.6f : 1.0f, p);
    }
    // Mode chips: STD = detuned copies; CHORD = the unison voices become chord notes. When CHORD
    // is on, a third chip cycles the chord TYPE and shows the actual stacked intervals for the
    // current voice count, e.g. "Maj (+4,+3)".
    // (The UNISON/CHORD/SCALE chips are GONE - the visual is UNISON-only; the SCALE harmonizer is
    // edited in the SCALE box above the keyboard. chip[] stay empty so old hit-tests never fire.)
    for (auto& cp : chip) cp = {};
    if (uniOn && centre)   // the extra DRY/original voice (double-click Detune) - drawn white so it stands out
        drawVoice(0.0f, juce::Colour(0xffffffff).withMultipliedAlpha(0.95f), 1.8f);

    auto handle = [&](float x, float y, juce::Colour c, int idx, const juce::String& lbl) {
        const float r = (hover == idx || drag == idx) ? 6.0f : 4.5f;
        g.setColour(c); g.fillEllipse(x - r, y - r, r * 2, r * 2);
        g.setColour(juce::Colours::black.withAlpha(0.5f)); g.drawEllipse(x - r, y - r, r * 2, r * 2, 1.0f);
        g.setColour(c.withMultipliedAlpha(0.9f)); g.setFont(juce::Font(9.0f, juce::Font::bold));
        g.drawText(lbl, juce::Rectangle<float>(x - 24, y - 18, 48, 12), juce::Justification::centred, false);
    };
    if (uniOn) {
        const float uy = q.bottom - (float)(curUni() - 1) / (float)(juce::jmax(1, maxUni - 1)) * (q.bottom - q.uniTop);
        handle(q.uX, uy, juce::Colour(0xff35c0ff), 0, juce::String(curUni()) + "x");
        if (scaleOn) {
            // SCALE: the detune dot picks the SCALE TYPE. Show just the scale name + key - NOT fixed
            // intervals (a scale voices a DIFFERENT chord per note, so one interval list would mislead).
            handle(q.dPtX, q.dPtY, juce::Colour(0xff5ad17a), 1, juce::String());
            g.setColour(juce::Colour(0xffb6e8c6)); g.setFont(juce::Font(10.0f, juce::Font::bold));
            g.drawText(juce::String(kUiScaleName[scaleType]) + juce::String::fromUTF8("  \xC2\xB7  ") + kUiNoteName[scaleKey],
                       juce::Rectangle<float>(q.dPtX - 80.0f, juce::jmax(q.top - 2.0f, q.dPtY - 20.0f), 160.0f, 11.0f),
                       juce::Justification::centred, false);
        } else if (chord > 0) {
            // DETUNE dot = the chord-TYPE selector: its height picks the type. The name + the
            // stacked intervals for the current voice count are drawn just ABOVE the dot.
            handle(q.dPtX, q.dPtY, juce::Colour(0xffb98cff), 1, juce::String());
            juce::String iv; int prev = 0;
            for (int k = 1; k < curUni(); ++k) { const int sMi = uiChordSemis(chord, k);
                iv += (iv.isEmpty() ? "" : ", ") + ("+" + juce::String(sMi - prev) + " st"); prev = sMi; }
            g.setColour(juce::Colour(0xffcdb4ff)); g.setFont(juce::Font(9.5f, juce::Font::bold));
            g.drawText(juce::String(kUiChordName[chord]) + (iv.isNotEmpty() ? " (" + iv + ")" : ""),
                       juce::Rectangle<float>(q.dPtX - 80.0f, juce::jmax(q.top - 2.0f, q.dPtY - 20.0f), 160.0f, 11.0f),
                       juce::Justification::centred, false);
        } else {
            // DETUNE (STD): real cents spread (how far the outermost voice sits from the original, each way).
            handle(q.dPtX, q.dPtY, juce::Colour(0xffffc23a), 1,
                   juce::String::fromUTF8("±") + juce::String(juce::roundToInt(det * 100.0f)) + "c");
        }
    }
    if (vibOn) {
        // VIBRATO shown as the real peak pitch deviation in semitones (~1.5 st at full).
        const float vibSt = 12.0f * std::log2(1.0f + 0.09f * vib);
        handle(q.vX, q.rootY - vib * q.upRange, juce::Colour(0xffff7ab0), 2, juce::String(vibSt, 2) + "st");
    if (uniOn)   // WIDTH dot: stereo spread of the unison/chord voices (0 = mono = the old sound)
        handle(q.wX, q.rootY - uniWidth * q.upRange, juce::Colour(0xff3ec6a8), 3,
               juce::String(juce::roundToInt(uniWidth * 100.0f)) + "%");
    if (uniOn)   // DRIFT dot: per-note randomness ("alive"); 0 = perfectly repeating = the old sound
        handle(q.xX, q.rootY - driftAmt * q.upRange, juce::Colour(0xffff9a3c), 4,
               juce::String(juce::roundToInt(driftAmt * 100.0f)) + "%");
    // LIVE MODULATION rings: a cyan hollow ring at each dot's modulated position (matrix targets moving,
    // like the FX-knob rings). Detune only in STD mode (chord/scale re-purpose that dot's height).
    {
        const float mx[4] = { q.dX, q.vX, q.wX, q.xX };
        const bool  on[4] = { uniOn && chord == 0 && ! scaleOn, vibOn, uniOn, uniOn };
        for (int i = 0; i < 4; ++i)
        {
            if (! on[i] || modLive[i] < 0.0f) continue;
            const float my = q.rootY - juce::jlimit(0.0f, 1.0f, modLive[i]) * q.upRange;
            g.setColour(juce::Colour(0xff35c0ff).withAlpha(0.9f));
            g.drawEllipse(mx[i] - 5.5f, my - 5.5f, 11.0f, 11.0f, 1.6f);
        }
        // [2026-07-14 00:30] Unison COUNT target: ring at the count handle's modulated position.
        if (uniOn && modUniCnt > -900.0f)
        {
            const float cy = q.bottom - (juce::jlimit(1.0f, (float) maxUni, modUniCnt) - 1.0f)
                                        / (float) juce::jmax(1, maxUni - 1) * (q.bottom - q.uniTop);
            g.setColour(juce::Colour(0xff35c0ff).withAlpha(0.9f));
            g.drawEllipse(q.uX - 5.5f, cy - 5.5f, 11.0f, 11.0f, 1.6f);
        }
    }
    }

    // tiny per-handle captions (which dot is which), since the box title names all three
    g.setColour(juce::Colour(0x559a9ac0)); g.setFont(juce::Font(8.0f, juce::Font::plain));
    if (uniOn) {
        g.drawText("uni",    juce::Rectangle<float>(q.uX - 18, q.bottom - 1, 36, 9), juce::Justification::centred, false);
        g.drawText(scaleOn ? "scale" : chord > 0 ? "chord" : "detune", juce::Rectangle<float>(q.dX - 24, q.bottom - 1, 48, 9), juce::Justification::centred, false);
    }
    if (vibOn) g.drawText("vib", juce::Rectangle<float>(q.vX - 18, q.bottom - 1, 36, 9), juce::Justification::centred, false);
    if (uniOn) g.drawText("width", juce::Rectangle<float>(q.wX - 18, q.bottom - 1, 36, 9), juce::Justification::centred, false);
    if (uniOn) g.drawText("drift", juce::Rectangle<float>(q.xX - 18, q.bottom - 1, 36, 9), juce::Justification::centred, false);
}
void VoiceModDisplay::mouseMove(const juce::MouseEvent& e) {
    int h = nearestHandle(e.position);
    int ch = -1; if (uniOn) for (int i = 0; i < 4; ++i) if (chip[i].contains(e.position)) { ch = i; break; }
    if (h != hover || ch != chipHover) { hover = h; chipHover = ch; repaint(); }
}
void VoiceModDisplay::mouseDown(const juce::MouseEvent& e)
{
    if ((e.mods.isRightButtonDown() || e.mods.isPopupMenu()) && mlm != nullptr)
    {   // MIDI-learn: the 5 unison-visual parameters, applied to the SELECTED slot (ui_sel_uni*)
        auto tag = [this](const char* pid) {
            const int cc = mlm->getCCForParam(pid);
            return cc < 0 ? juce::String()
                          : "   [ch" + juce::String(mlm->getChannelForParam(pid)) + " cc" + juce::String(cc) + "]";
        };
        juce::PopupMenu m;
        m.addSectionHeader("MIDI-learn (acts on the SELECTED slot)");
        m.addItem(1, "Unison count..." + tag("ui_sel_uniCount"));
        m.addItem(2, "Detune..."       + tag("ui_sel_uniDet"));
        m.addItem(3, "Vibrato..."      + tag("ui_sel_uniVib"));
        m.addItem(4, "Width..."        + tag("ui_sel_uniWidth"));
        m.addItem(5, "Drift..."        + tag("ui_sel_uniDrift"));
        m.addSeparator(); m.addItem(6, "Clear these assignments");
        auto mp = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
        auto* L = mlm;
        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({ mp.x, mp.y, 1, 1 }).withMinimumWidth(240),
            [L](int r) {
                static const char* pids[5] = { "ui_sel_uniCount", "ui_sel_uniDet", "ui_sel_uniVib", "ui_sel_uniWidth", "ui_sel_uniDrift" };
                if (r >= 1 && r <= 5) L->startLearning(pids[r - 1]);
                else if (r == 6) for (auto* p : pids) L->clearParam(p);
            });
        return;
    }
    if (uniOn) {
        if (chip[0].contains(e.position)) { const bool ch = scaleOn || chord != 0; scaleOn = false; chord = 0; if (ch) { emit(); repaint(); if (onDragEnd) onDragEnd(); } return; }
        if (chip[1].contains(e.position)) { const bool ch = scaleOn || chord == 0; scaleOn = false; if (chord == 0) chord = 3; if (ch) { emit(); repaint(); if (onDragEnd) onDragEnd(); } return; }  // CHORD (Maj); type via the detune dot
        if (chip[2].contains(e.position)) { if (! scaleOn) { scaleOn = true; emit(); repaint(); if (onDragEnd) onDragEnd(); } return; }   // SCALE (diatonic harmonizer)
        if (scaleOn && chip[3].contains(e.position)) {   // KEY picker (root note)
            juce::PopupMenu m;
            for (int nk = 0; nk < 12; ++nk) m.addItem(nk + 1, kUiNoteName[nk], true, nk == scaleKey);
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                [this](int r){ if (r >= 1) { scaleKey = r - 1; emit(); repaint(); if (onDragEnd) onDragEnd(); } });
            return;
        }
    }
    drag = nearestHandle(e.position); if (drag >= 0) mouseDrag(e);
}
void VoiceModDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (drag < 0) return;
    const Geo q = geom();
    if (drag == 0) { int& tgt = scaleOn ? uniScale : (chord > 0) ? uniChord : uni; tgt = juce::jlimit(1, maxUni, 1 + juce::roundToInt((q.bottom - e.position.y) / juce::jmax(1.0f, q.bottom - q.uniTop) * (float)(maxUni - 1))); }
    else if (drag == 1) {
        const float t = juce::jlimit(0.0f, 1.0f, (q.rootY - e.position.y) / juce::jmax(1.0f, q.upRange));   // 0 at the root line -> 1 near the top
        if (scaleOn)          scaleType = juce::jlimit(0, kNumScales - 1, juce::roundToInt(t * (float)(kNumScales - 1)));   // SCALE type
        else if (chord > 0)   chord     = juce::jlimit(1, 7, 1 + juce::roundToInt(t * 6.0f));                              // CHORD type
        else { mode = 0;      det       = t; }                                                                            // STD cents
    }
    else if (drag == 2) vib = juce::jlimit(0.0f, 1.0f, (q.rootY - e.position.y) / juce::jmax(1.0f, q.upRange));
    else if (drag == 3) uniWidth = juce::jlimit(0.0f, 1.0f, (q.rootY - e.position.y) / juce::jmax(1.0f, q.upRange));
    else if (drag == 4) driftAmt = juce::jlimit(0.0f, 1.0f, (q.rootY - e.position.y) / juce::jmax(1.0f, q.upRange));
    emit();
    repaint();
}
void VoiceModDisplay::mouseDoubleClick(const juce::MouseEvent&)
{
    // (Removed: double-clicking Detune used to toggle an extra "original/dry" voice - the user found it
    // confusing. No double-click action now. Factory sounds that authored a centre voice still play it.)
}

//==============================================================================
// FrequencyDisplay
//==============================================================================
void FrequencyDisplay::setFilters(int t0, float c0, float r0, float e0, float g0,
                                  int t1, float c1, float r1, float e1, float g1, double sr)
{
    fType[0] = t0; fCutoff[0] = c0; fReso[0] = r0; fEnvAmt[0] = e0; fGain[0] = g0;
    fType[1] = t1; fCutoff[1] = c1; fReso[1] = r1; fEnvAmt[1] = e1; fGain[1] = g1;
    sampleRate = sr; showFilter = true;
    repaint();
}

void FrequencyDisplay::pushSpectrum(const float* mags, int n)
{
    n = juce::jmin(n, scopeSize);
    for (int i = 0; i < n; ++i)
        scope[i] = juce::jmax(scope[i], mags[i]); // outline rises instantly to peaks
    hasSpectrum = true;
}

void FrequencyDisplay::decayTick()
{
    for (int i = 0; i < scopeSize; ++i)
        scope[i] *= 0.80f;   // peak-hold fall (capture is now timeline-aligned)
    repaint();
}


int FrequencyDisplay::nearestBand(juce::Point<float> p) const
{
    // TWO filters, each with a cutoff/reso DIAMOND + an envelope end handle (grabbable when the filter
    // is ON). Pick the NEAREST handle across both filters; the active filter's diamond wins close ties.
    // [2026-07-15 17:00] grab radius 16 -> 13 (user: too easy to grab from far away).
    const auto a = plotArea();
    int best = -1; float bd = 13.0f * 13.0f;
    for (int fi = 0; fi < 2; ++fi)
    {
        const bool fOn = (fType[fi] >= DrumChannel::LowPass && fType[fi] <= DrumChannel::Notch) || fType[fi] == DrumChannel::Bell;
        if (fOn) { const float de = p.getDistanceSquaredFrom(filtEnvPos(a, fi)); if (de < bd) { bd = de; best = kFiltEnv(fi); } }
        const float dm = p.getDistanceSquaredFrom(filtPos(a, fi));
        const float bias = (fi == activeFilt) ? 6.0f : 0.0f;   // small pull toward the active filter's diamond
        if (dm - bias < bd) { bd = dm - bias; best = kFilt(fi); }
    }
    if (best >= 0) return best;
    if (a.contains(p)) return kFilt(activeFilt);   // empty graph area = drag the active filter's cutoff/reso
    return -1;
}

// [2026-07-15 17:00] HOVER hit-test: tighter (11 px) and NO empty-area fallback - hovering the open
// graph shows the GENERAL tooltip + no highlight ring, instead of always reading as "a filter"
// (the fallback exists for DRAGGING - drag-anywhere = cutoff/reso stays - not for hovering).
int FrequencyDisplay::hoverBand(juce::Point<float> p) const
{
    const auto a = plotArea();
    int best = -1; float bd = 11.0f * 11.0f;
    for (int fi = 0; fi < 2; ++fi)
    {
        const bool fOn = (fType[fi] >= DrumChannel::LowPass && fType[fi] <= DrumChannel::Notch) || fType[fi] == DrumChannel::Bell;
        if (fOn) { const float de = p.getDistanceSquaredFrom(filtEnvPos(a, fi)); if (de < bd) { bd = de; best = kFiltEnv(fi); } }
        const float dm = p.getDistanceSquaredFrom(filtPos(a, fi));
        if (dm < bd) { bd = dm; best = kFilt(fi); }
    }
    return best;
}

void FrequencyDisplay::paint(juce::Graphics& g)
{
    auto bb = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(bb, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(bb.reduced(0.5f), 4.0f, 1.0f);
    {   // FILTER DRIVE drag-box (top-right): saturation INSIDE both filter loops, one amount.
        // DIMMED when no filter is on (it would do nothing - honesty rule).
        auto dr = driveRect();
        const bool anyF = fType[0] > 0 || fType[1] > 0;
        const float al = anyF ? 1.0f : 0.4f;
        g.setColour(juce::Colour(0xff26264a).withAlpha(al)); g.fillRoundedRectangle(dr, 3.0f);
        if (fDrive > 0.001f)
        { g.setColour(juce::Colour(0xffff7a4a).withAlpha(0.30f * al));
          g.fillRoundedRectangle(dr.withWidth(juce::jmax(5.0f, fDrive * dr.getWidth())), 3.0f); }
        g.setColour(juce::Colour(0xffd8e0f0).withAlpha(al)); g.setFont(juce::Font(10.0f, juce::Font::bold));
        g.drawText(fDrive > 0.001f ? "Drive " + juce::String(juce::roundToInt(fDrive * 100.0f)) + "%"
                                   : "Drive: Off", dr, juce::Justification::centred, false);
    }

    const auto a = plotArea();
    const float left = a.getX(), right = a.getRight(), top = a.getY(), bottom = a.getBottom(), w = a.getWidth(), h = a.getHeight();

    // 0 dB centre line + frequency grid. Gridlines are drawn for every decade tick, but only a SPARSE
    // set is LABELLED (the full set overlapped + was unreadable - user).
    g.setColour(juce::Colour(0xff242440)); g.drawHorizontalLine((int) a.getCentreY(), left, right);
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = xForFreq(a, f);
        g.setColour(juce::Colour(0xff20203a)); g.drawVerticalLine((int) x, top, bottom);
        const bool labelled = (f == 50.0f || f == 200.0f || f == 1000.0f || f == 5000.0f || f == 20000.0f);
        if (! labelled) continue;
        g.setColour(juce::Colour(0xff7d86a8)); g.setFont(juce::Font(10.0f, juce::Font::bold));
        juce::String lbl = f >= 1000.0f ? juce::String((int)(f / 1000)) + "k" : juce::String((int) f);
        const bool nearEdge = (x > right - 26.0f);
        g.drawText(lbl, (int) x - (nearEdge ? 28 : -3), (int) bottom - 14, 26, 12,
                   nearEdge ? juce::Justification::right : juce::Justification::left, false);
    }

    // Spectrum (filled cyan body + outline).
    if (hasSpectrum)
    {
        auto xAt = [&](int i) { return left + w * (float) i / (float)(scopeSize - 1); };
        juce::Path sp; sp.startNewSubPath(left, bottom);
        for (int i = 0; i < scopeSize; ++i) sp.lineTo(xAt(i), bottom - scope[i] * h);
        sp.lineTo(right, bottom); sp.closeSubPath();
        g.setColour(juce::Colour(0x3300ccff)); g.fillPath(sp);
        juce::Path ol;
        for (int i = 0; i < scopeSize; ++i) { float y = bottom - scope[i] * h; if (i == 0) ol.startNewSubPath(xAt(i), y); else ol.lineTo(xAt(i), y); }
        g.setColour(juce::Colour(0xff33ddff)); g.strokePath(ol, juce::PathStrokeType(1.4f));
    }

    // === TWO per-slot resonant FILTERS in series (F1 = orange, F2 = cyan). Each is just a DIAMOND
    //     (drag = cutoff X / reso Y). DOUBLE-CLICK a diamond = enable/disable it; RIGHT-CLICK = change
    //     its type (LP -> HP -> BP -> Notch -> Bell). The type name sits on the diamond. No buttons. ===
    static const char* tn[7] = { "OFF", "LP", "HP", "BP", "NOTCH", "FORMANT", "BELL" };
    if (sampleRate > 0.0)
    {
        const int act = drag >= 0 ? drag : hover;
        for (int fi = 0; fi < 2; ++fi)
        {
            const auto fcol = filtColour(fi);
            const bool fOn  = (fType[fi] >= DrumChannel::LowPass && fType[fi] <= DrumChannel::Notch) || fType[fi] == DrumChannel::Bell;
            const auto mp   = filtPos(a, fi);
            {   // response curve for this filter's type (dim when off)
                Biquad bq; const double fc = juce::jlimit(20.0, sampleRate * 0.49, (double) fCutoff[fi]);
                const double q = juce::jlimit(0.3, 12.0, (double) fReso[fi]);
                switch (fType[fi]) {
                    case DrumChannel::HighPass: bq.highpass(sampleRate, fc, q); break;
                    case DrumChannel::BandPass: bq.bandpass(sampleRate, fc, q); break;
                    case DrumChannel::Notch:    bq.notch   (sampleRate, fc, q); break;
                    case DrumChannel::Bell:     bq.peaking (sampleRate, fc, q, juce::jlimit(-15.0f, 15.0f, fGain[fi])); break;   // bipolar gain + Q (wheel)
                    default:                    bq.lowpass (sampleRate, fc, q); break;
                }
                juce::Path resp; bool started = false;
                for (float px = 0; px <= w; px += 2.0f) {
                    const float db = fOn ? juce::Decibels::gainToDecibels((float) bq.magnitudeAt(normToFreq(px / w), sampleRate)) : 0.0f;
                    const float y = yForDb(a, juce::jlimit(-kMaxDb, kMaxDb, db));
                    if (! started) { resp.startNewSubPath(left + px, y); started = true; } else resp.lineTo(left + px, y);
                }
                if (fOn) { g.setColour(fcol.withAlpha(0.9f)); g.strokePath(resp, juce::PathStrokeType(2.0f)); }
                // live MODULATED cutoff marker (a bright vertical line where modulation pushes the cutoff)
                if (fOn && modCutoff[fi] > 0.0f && std::abs(modCutoff[fi] - fCutoff[fi]) > fCutoff[fi] * 0.02f)
                {
                    const float mx = xForFreq(a, juce::jlimit(20.0f, 20000.0f, modCutoff[fi]));
                    g.setColour(fcol.brighter(0.5f).withAlpha(0.9f));
                    g.drawVerticalLine((int) mx, top, bottom);
                    g.fillEllipse(mx - 2.5f, top + 1.0f, 5.0f, 5.0f);
                }
                // [2026-07-14 00:30] live MODULATED RESO: a hollow ring at the modulated (cutoff, reso)
                // spot - the diamond stays at your base setting (the mod-ring convention).
                if (fOn && modReso[fi] > -900.0f)
                {
                    const float mx2 = xForFreq(a, juce::jlimit(20.0f, 20000.0f, modCutoff[fi] > 0.0f ? modCutoff[fi] : fCutoff[fi]));
                    const float my2 = a.getBottom() - resoToNorm(modReso[fi]) * a.getHeight() * 0.85f - a.getHeight() * 0.06f;
                    g.setColour(fcol.brighter(0.6f));
                    g.drawEllipse(mx2 - 4.5f, my2 - 4.5f, 9.0f, 9.0f, 1.6f);
                }
                // live MODULATED ENV AMOUNT: a bright dot where the modulated sweep would END.
                if (fOn && modEnvA[fi] > -900.0f)
                {
                    const float endHz = juce::jlimit(20.0f, 20000.0f,
                        fCutoff[fi] * std::pow(2.0f, juce::jlimit(-1.0f, 1.0f, modEnvA[fi]) * 5.0f));
                    const auto mp2 = filtPos(a, fi);
                    g.setColour(fcol.brighter(0.7f).withAlpha(0.95f));
                    g.fillEllipse(xForFreq(a, endHz) - 3.0f, mp2.y - 3.0f, 6.0f, 6.0f);
                }
            }
            if (fOn && std::abs(fEnvAmt[fi]) > 0.02f)   // envelope sweep arrow
            {
                const auto ep = filtEnvPos(a, fi);
                const float dash[2] = { 4.0f, 3.0f };
                g.setColour(fcol.withAlpha(0.8f));
                g.drawDashedLine(juce::Line<float>(mp, ep), dash, 2, 1.4f);
                const float dir = (ep.x >= mp.x) ? 1.0f : -1.0f;
                juce::Path tri; tri.addTriangle(ep.x + dir * 5.0f, ep.y, ep.x - dir * 2.0f, ep.y - 4.0f, ep.x - dir * 2.0f, ep.y + 4.0f);
                g.fillPath(tri);
            }
            if (fOn)   // env end handle (parks beside the diamond at env 0)
            {
                const auto ep = filtEnvPos(a, fi);
                const bool parked = std::abs(fEnvAmt[fi]) <= 0.02f;
                if (parked) { const float dash[2] = { 2.0f, 2.0f }; g.setColour(fcol.withAlpha(0.4f));
                              g.drawDashedLine(juce::Line<float>(mp, ep), dash, 2, 1.0f); }
                const float er = (act == kFiltEnv(fi)) ? 5.0f : 3.6f;
                juce::Path d2; d2.addQuadrilateral(ep.x, ep.y - er, ep.x + er, ep.y, ep.x, ep.y + er, ep.x - er, ep.y);
                g.setColour(fcol.withAlpha(act == kFiltEnv(fi) ? 1.0f : parked ? 0.4f : 0.65f)); g.fillPath(d2);
                g.setColour(juce::Colours::white.withAlpha(parked ? 0.5f : 0.8f)); g.strokePath(d2, juce::PathStrokeType(1.0f));
            }
            // The DIAMOND: filled + type name when ON, hollow/dim when OFF (still there to double-click).
            const float r = (act == kFilt(fi)) ? 8.0f : 6.5f;
            if (fi == activeFilt && fOn) { g.setColour(juce::Colours::white.withAlpha(0.6f)); g.drawEllipse(mp.x - r - 2, mp.y - r - 2, (r + 2) * 2, (r + 2) * 2, 1.2f); }
            juce::Path dia; dia.addQuadrilateral(mp.x, mp.y - r, mp.x + r, mp.y, mp.x, mp.y + r, mp.x - r, mp.y);
            if (fOn) { g.setColour(fcol); g.fillPath(dia); g.setColour(juce::Colours::black); g.strokePath(dia, juce::PathStrokeType(1.0f)); }
            else     { g.setColour(fcol.withAlpha(0.45f)); g.strokePath(dia, juce::PathStrokeType(1.5f)); }
            // Type name ON the diamond (a small tag just above it): the user always sees F1/F2's type.
            g.setColour(fOn ? fcol : fcol.withAlpha(0.6f)); g.setFont(juce::Font(9.5f, juce::Font::bold));
            g.drawText(juce::String("F") + juce::String(fi + 1) + " " + tn[juce::jlimit(0, 6, fType[fi])],
                       juce::jlimit(left, right - 52.0f, mp.x - 26.0f), mp.y - r - 13.0f, 52.0f, 11.0f, juce::Justification::centred, false);
        }
        // one-line gesture hint (no buttons to discover from)
        g.setColour(juce::Colour(0xff6a7290)); g.setFont(juce::Font(9.0f, juce::Font::bold));
        g.drawText("drag = cutoff/reso - double-click = on/off - right-click = type",
                   juce::Rectangle<int>((int) left, (int) top - 1, (int) w, 11), juce::Justification::topLeft, false);
    }
}

void FrequencyDisplay::mouseMove(const juce::MouseEvent& e) { int b = hoverBand(e.position); if (b != hover) { hover = b; repaint(); } }   // tight hover [2026-07-15 17:00]

void FrequencyDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (driveRect().contains(e.position)) { driveDrag = true; mouseDrag(e); return; }
    const int b = nearestBand(e.position);
    if (b < 0) return;
    const int fi = b % 2;
    activeFilt = fi;   // grabbing a handle selects its filter (Keytrack fader follows it)
    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
    {   // RIGHT-CLICK a diamond = cycle its type: Off -> LP -> HP -> BP -> Notch -> Bell -> Off (no menu)
        int nt = fType[fi] + 1;
        if (nt == DrumChannel::Formant)   nt = DrumChannel::Bell;        // skip Formant (legacy)
        else if (nt > DrumChannel::Bell)  nt = DrumChannel::FilterOff;
        fType[fi] = nt;
        if (onFilterEdit) onFilterEdit(fi, fType[fi], fCutoff[fi], fReso[fi], fEnvAmt[fi], fGain[fi]);
        if (onEdit) onEdit(); if (onDragEnd) onDragEnd();
        repaint(); return;
    }
    drag = b; mouseDrag(e);
}

void FrequencyDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (driveDrag)
    {   // FILTER DRIVE: absolute horizontal drag across the box; hard left = Off (clean)
        auto dr = driveRect();
        const float f2 = juce::jlimit(0.0f, 1.0f, (e.position.x - dr.getX()) / juce::jmax(1.0f, dr.getWidth()));
        fDrive = f2 < 0.04f ? 0.0f : f2;
        if (onFilterDriveEdit) onFilterDriveEdit(fDrive);
        repaint(); return;
    }
    if (drag < 0) return;
    const auto a = plotArea();
    const int fi = drag % 2;   // handle id encodes the filter (kFilt/kFiltEnv = 100/102 + fi)
    if (drag == kFiltEnv(fi))
    {   // arrow end = where the envelope sweeps the cutoff to: amt = log2(end/cutoff)/5 (+/-1 = +/-5 oct)
        const float endHz = juce::jlimit(20.0f, 20000.0f, freqForX(a, e.position.x));
        float amt = std::log2(juce::jmax(1.0e-3f, endHz / juce::jmax(20.0f, fCutoff[fi]))) / 5.0f;
        if (std::abs(amt) < 0.04f) amt = 0.0f;
        fEnvAmt[fi] = juce::jlimit(-1.0f, 1.0f, amt);
        if (onFilterEdit) onFilterEdit(fi, fType[fi], fCutoff[fi], fReso[fi], fEnvAmt[fi], fGain[fi]);
        repaint(); return;
    }
    // diamond: X = cutoff; Y = resonance - except BELL, where Y = bipolar GAIN on the dB axis
    // (boost above the centre line, CUT below; wheel = the bell's Q). Dragging an OFF filter turns it on.
    if ((fType[fi] < DrumChannel::LowPass || fType[fi] > DrumChannel::Notch) && fType[fi] != DrumChannel::Bell)
        fType[fi] = DrumChannel::LowPass;
    fCutoff[fi] = juce::jlimit(20.0f, 20000.0f, freqForX(a, e.position.x));
    if (fType[fi] == DrumChannel::Bell)
    {
        float g = juce::jlimit(-15.0f, 15.0f, dbForY(a, e.position.y));
        if (std::abs(g) < 0.6f) g = 0.0f;   // snap-to-flat near the centre line
        fGain[fi] = g;
    }
    else
        fReso[fi] = normToReso((a.getBottom() - a.getHeight() * 0.06f - e.position.y) / (a.getHeight() * 0.85f));
    if (onFilterEdit) onFilterEdit(fi, fType[fi], fCutoff[fi], fReso[fi], fEnvAmt[fi], fGain[fi]);
    repaint();
}

void FrequencyDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    const int b = nearestBand(e.position);
    if (b < 0) return;
    const int fi = b % 2;
    if (b == kFiltEnv(fi)) { fEnvAmt[fi] = 0.0f; if (onFilterEdit) onFilterEdit(fi, fType[fi], fCutoff[fi], fReso[fi], fEnvAmt[fi], fGain[fi]); repaint(); return; }
    activeFilt = fi;   // toggle that filter on/off
    const bool wasOn = (fType[fi] >= DrumChannel::LowPass && fType[fi] <= DrumChannel::Notch) || fType[fi] == DrumChannel::Bell;
    fType[fi] = wasOn ? DrumChannel::FilterOff : DrumChannel::LowPass;
    if (onFilterEdit) onFilterEdit(fi, fType[fi], fCutoff[fi], fReso[fi], fEnvAmt[fi], fGain[fi]);
    repaint();
}

void FrequencyDisplay::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wd)
{   // wheel over a diamond = its resonance
    const int b = nearestBand(e.position); if (b < 0) return;
    const int fi = b % 2;
    fReso[fi] = juce::jlimit(0.3f, 12.0f, fReso[fi] * (wd.deltaY > 0 ? 1.12f : 1.0f / 1.12f));   // Bell: this is the bell's Q
    if (onFilterEdit) onFilterEdit(fi, fType[fi], fCutoff[fi], fReso[fi], fEnvAmt[fi], fGain[fi]);
    repaint();
}

juce::String FrequencyDisplay::getTooltip()
{
    if (driveRect().contains(getMouseXYRelative().toFloat()))
        return "FILTER DRIVE (drag left/right): warms and saturates the filter itself - not the output.\n\n"
               "- The distortion happens INSIDE the filter loop, so the resonance compresses and SINGS "
               "instead of just ringing louder.\n"
               "- One amount drives both filters (they run in series).\n"
               "- NEEDS A FILTER ON (a diamond enabled) - it is dimmed and silent otherwise.\n"
               "- Off = the exact clean filter you had before.\n\n"
               "Tip: turn resonance up and Drive to ~40% on a bass - it growls instead of whistling.";
    static const char* tn[7] = { "Off", "Low-pass", "High-pass", "Band-pass", "Notch", "Formant", "Bell" };
    const int b = drag >= 0 ? drag : hover;
    if (b == kFiltEnv(0) || b == kFiltEnv(1))
    { const int fi = b % 2; return juce::String("Filter ") + juce::String(fi + 1) + " ENVELOPE: the cutoff opens/closes by "
           + juce::String(fEnvAmt[fi] * 5.0f, 1) + " octaves each hit (right of the diamond = sweep DOWN, left = UP). Double-click resets."; }
    if (b == kFilt(0) || b == kFilt(1))
    { const int fi = b % 2;
      if (fType[fi] == DrumChannel::Bell)
          return juce::String("Filter ") + juce::String(fi + 1) + " (Bell): a BIPOLAR peak - boost OR cut a band (a real EQ bell). "
                 "DRAG = frequency (" + juce::String(juce::roundToInt(fCutoff[fi])) + " Hz) + gain ("
                 + juce::String(fGain[fi] >= 0 ? "+" : "") + juce::String(fGain[fi], 1) + " dB - the diamond sits AT its gain; "
                 "centre line = flat). WHEEL = Q (" + juce::String(fReso[fi], 2) + "). RIGHT-CLICK = type, DOUBLE-CLICK = on/off.";
      return juce::String("Filter ") + juce::String(fi + 1) + " (" + tn[juce::jlimit(0, 6, fType[fi])] + "): DRAG the diamond = cutoff ("
           + juce::String(juce::roundToInt(fCutoff[fi])) + " Hz) + reso (Q " + juce::String(fReso[fi], 2) + "). RIGHT-CLICK = change type, DOUBLE-CLICK = on/off, wheel = reso."; }
    return "Live spectrum of THIS channel + TWO resonant filters in series (F1 orange, F2 cyan).\n\n"
           "- Each filter is a DIAMOND: drag = cutoff/reso, double-click = on/off, right-click = type "
           "(LP / HP / BP / Notch / BELL - Bell BOOSTS a band, Y = +dB). Stack High-Pass + Low-Pass = a band.\n"
           "- REVERB/DELAY tails never appear in the spectrum: they live on the shared MASTER bus - this "
           "shows only this channel's own output, measured before the sends leave.\n"
           "- Dragging the empty graph moves the ACTIVE filter's cutoff/reso.";
}

//==============================================================================
// LfoDisplay
//==============================================================================
static float lfoCyclesShown(float rate)   // rate (log 0.1 Hz..2 kHz now) -> how many sine cycles the strip draws
{
    const float t = juce::jlimit(0.0f, 1.0f, std::log(rate / 0.1f) / std::log(2000.0f / 0.1f));
    return 0.75f + t * 7.25f;   // slow = under one cycle, audio-rate = 8 cycles (a dense band = honest)
}

// Fixed per-destination hues so the three INDEPENDENT LFOs read apart at a glance
// (FILT = the EQ diamond's orange, PITCH = the pitch/slide violet, VOL = the level green).
static const juce::Colour kLfoDestCol[4] = { juce::Colour(0xffff9a3c), juce::Colour(0xffc77dff), juce::Colour(0xff35d07a), juce::Colour(0xff35c0ff) };
juce::Colour LfoDisplay::destCol(int lfoIdx) const
{ return kLfoDestCol[juce::jlimit(0, 3, lfoIdx)]; }   // fixed per-LFO identity hue
static const char* kLfoShapeName[8] = { "Sine", "Tri", "Saw Dn", "Square", "Rnd Step", "Saw Up", "Rnd Glide", "Custom" };
// UI mirror of the DSP's lfoShapeVal (DrumChannel.cpp) - keep in sync (the mirror rule): the drawn
// wave must be exactly what plays, including each Random cycle's held value + the Custom curve.
static inline float uiLfoRand01(uint32_t cyc)
{ uint32_t h = cyc * 2654435761u; h ^= h >> 16; h *= 2246822519u; h ^= h >> 13;
  return (float) h / 2147483648.0f - 1.0f; }
static inline float uiLfoShapeVal(int shape, double ph, uint32_t cyc, const float* curve = nullptr)
{
    if (shape <= 0) return (float) std::sin(ph);
    const double t = ph / (2.0 * juce::MathConstants<double>::pi);
    const double f2 = t - std::floor(t);
    switch (shape)
    {
        case 1: return (float) (f2 < 0.25 ? 4.0 * f2 : f2 < 0.75 ? 2.0 - 4.0 * f2 : 4.0 * f2 - 4.0);
        case 2: return (float) (1.0 - 2.0 * f2);
        case 3: return f2 < 0.5 ? 1.0f : -1.0f;
        case 4: return uiLfoRand01(cyc);
        case 5: return (float) (2.0 * f2 - 1.0);
        case 6: { const float a = uiLfoRand01(cyc), b = uiLfoRand01(cyc + 1u);
                  const float sm = (float) (f2 * f2 * (3.0 - 2.0 * f2));
                  return a + (b - a) * sm; }
        default:
        { if (curve == nullptr) return 0.0f;
          const double sp = f2 * 64.0;
          const int i0 = juce::jlimit(0, 63, (int) sp), i1 = (i0 + 1) % 64;
          return curve[i0] + (curve[i1] - curve[i0]) * (float) (sp - (double) i0); }
    }
}

// Tempo-sync detents in CYCLES PER BAR, stepped through by DRAGGING THE WAVE while Sync is on
// (the wave is the ONE rate control - the old Sync/Rate faders duplicated it and were removed).
// Spans slow sweeps (0.25 = one cycle per 4 bars) to 16th wobble, incl. ODD values for odd meters.
// [2026-07-15 14:20] the LFO Bar-sync detents = THE shared cycles-per-bar ladder (every whole
// number to 21; the old hand-picked list jumped 13 -> 16 - user). One source: DrumChannel.
static const float* kLfoSyncCPB = DrumChannel::kChFxCpb;
static constexpr int kLfoSyncN  = DrumChannel::kChFxCpbN;
static int lfoSyncNearestIdx(float cpb)
{ int b = 0; float bd = 1.0e9f;
  for (int i = 0; i < kLfoSyncN; ++i) { const float d = std::abs(kLfoSyncCPB[i] - cpb); if (d < bd) { bd = d; b = i; } }
  return b; }
static juce::String lfoCpbText(float cpb)   // "8 /bar", "0.25 /bar"
{ return (cpb == (int) cpb ? juce::String((int) cpb) : juce::String(cpb, 2)) + " /bar"; }

void LfoDisplay::paint(juce::Graphics& g)
{
    auto bb = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(bb, 4.0f);
    g.setColour(accent_.withAlpha(0.5f));  g.drawRoundedRectangle(bb.reduced(0.5f), 4.0f, 1.0f);

    // Top strip: title + the 3 LFO tabs (selected = bright; a dot marks the ones that are ON).
    g.setFont(juce::Font(9.0f, juce::Font::bold));
    g.setColour(juce::Colour(0xff8090b0));
    g.drawText("MOD", 6, 3, 40, 12, juce::Justification::centredLeft, false);
    // Tabs: LFO 1 / LFO 2 / LFO 3 + Mod Env. An LFO tab's hue follows the destination it drives
    // (RIGHT-click to reassign); the Mod Env tab is violet. Left-click selects.
    const float bw = bb.getWidth() * 0.2075f;
    for (int d = 0; d < 4; ++d)
    {
        juce::Rectangle<float> br(bb.getWidth() * 0.17f + d * bw + 1.0f, 2.0f, bw - 2.0f, 13.0f);
        const bool isEnv = (d == 3), sel = (d == dest_), on = ! isEnv && amt_[d] > 0.001f;
        const juce::Colour hue = isEnv ? juce::Colour(0xffb46bff) : destCol(d);
        g.setColour(sel ? hue : juce::Colour(0xff2a2a4a)); g.fillRoundedRectangle(br, 3.0f);
        g.setColour(sel ? juce::Colours::black : (on || isEnv ? hue : juce::Colours::lightgrey));
        g.drawText(isEnv ? "Mod Env" : "LFO " + juce::String(d + 1), br, juce::Justification::centred, false);
        if (on && ! sel) { g.setColour(hue); g.fillEllipse(br.getRight() - 5.0f, br.getY() + 2.0f, 3.0f, 3.0f); }
    }
    if (dest_ == 3) { paintModEnv(g); return; }   // TAB 4 = the Mod Env graph (no LFO wave / buttons)

    const auto a  = waveArea();
    const float cy = a.getCentreY();
    g.setColour(juce::Colour(0xff242440)); g.drawHorizontalLine((int) cy, a.getX(), a.getRight());

    // GHOST waves first: the other destinations that are running (dim, their own hue) - makes it
    // obvious the three LFOs are independent and can stack.
    const uint32_t cycBase = liveCyc_;   // Random: draw the PLAYING note's rolled pattern (changes per note)
    auto wavePath = [&a, cy, cycBase](float rate, float amt, int shape, const float* cv) {
        const float cyc = lfoCyclesShown(rate);
        const float amp = amt * (a.getHeight() * 0.5f - 2.0f);
        juce::Path w;
        for (float px = 0; px <= a.getWidth(); px += 1.5f)
        {
            const double ph = (double) px / (double) a.getWidth() * (double) cyc * 2.0 * juce::MathConstants<double>::pi;
            const float y = cy - amp * uiLfoShapeVal(shape, ph, cycBase + (uint32_t)(ph / (2.0 * juce::MathConstants<double>::pi)), cv);
            if (px == 0) w.startNewSubPath(a.getX(), y); else w.lineTo(a.getX() + px, y);
        }
        return w;
    };
    // Ghost + selected waves draw at effHz() - the TRUE speed the DSP plays (synced/grid rates
    // included), never the ignored free-Hz value (the honesty rule).
    for (int d = 0; d < 4; ++d)
        if (d != dest_ && amt_[d] > 0.001f)
        {
            g.setColour(destCol(d).withAlpha(0.30f));
            g.strokePath(wavePath(effHz(d), amt_[d], shape_[d], curve_[d]), juce::PathStrokeType(1.2f));
        }

    if (amt_[dest_] <= 0.001f)
    {   // the SELECTED LFO is off: flat dim line + how to wake it
        g.setColour(destCol(dest_).withAlpha(0.35f));
        g.drawLine(a.getX() + 3, cy, a.getRight() - 3, cy, 1.2f);
        g.setColour(juce::Colour(0xff8090b0)); g.setFont(juce::Font(10.0f));
        g.drawText("LFO " + juce::String(dest_ + 1) + " off - drag up", a, juce::Justification::centred, false);
    }
    else
    {
        // The selected LFO's wave: N cycles wide (true speed), amplitude = Amount, in its own hue.
        const float cyc = lfoCyclesShown(effHz(dest_));
        const float amp = amt_[dest_] * (a.getHeight() * 0.5f - 2.0f);
        g.setColour(destCol(dest_)); g.strokePath(wavePath(effHz(dest_), amt_[dest_], shape_[dest_], curve_[dest_]), juce::PathStrokeType(1.8f));

        // LIVE PLAYHEAD (user: "i wanna see where it is when i play live"): a vertical scan line +
        // a glowing dot riding the drawn wave. RETRIG = the newest voice's real phase (nothing
        // sounding = no LFO running = no playhead, honest); FREE = the continuous timeline clock
        // (it keeps travelling between notes - that IS what Free means). The playhead sweeps ALL
        // drawn cycles left-to-right (wraps at the right edge), and its y uses the SAME per-cycle
        // formula as the drawn path, so the dot always sits ON the wave (incl. Random's steps).
        double dotFrac = -1.0;
        if (free_[dest_])      dotFrac = freeSec_ * (double) effHz(dest_);
        else if (phase_ >= 0.0) dotFrac = phase_ / (2.0 * juce::MathConstants<double>::pi);
        if (dotFrac >= 0.0)
        {
            const double vf = std::fmod(dotFrac, (double) cyc);           // position across the VIEW
            const double ph = vf * 2.0 * juce::MathConstants<double>::pi;
            const float px = a.getX() + (float) (vf / (double) cyc) * a.getWidth();
            const float py = cy - amp * uiLfoShapeVal(shape_[dest_], ph, cycBase + (uint32_t) vf, curve_[dest_]);
            g.setColour(destCol(dest_).withAlpha(0.40f));
            g.drawLine(px, a.getY() + 1.0f, px, a.getBottom() - 1.0f, 1.2f);
            g.setColour(destCol(dest_).withAlpha(0.35f)); g.fillEllipse(px - 6.0f, py - 6.0f, 12.0f, 12.0f);
            g.setColour(juce::Colours::white);                g.fillEllipse(px - 3.2f, py - 3.2f, 6.4f, 6.4f);
        }
    }
    if (! routed_[dest_] && amt_[dest_] > 0.001f)
    {   // the LFO has a level but no matrix route = it does nothing: tell the user where routing lives
        g.setColour(juce::Colour(0xffff7a4a)); g.setFont(juce::Font(9.0f));
        g.drawText("not routed - use the MATRIX faders below", a.reduced(3.0f, 1.0f),
                   juce::Justification::bottomLeft, false);
    }
    // Bottom row: the SYNC button (click = Off -> Sync -> Key) + the honest speed read-out.
    // Off = drag the wave for free Hz; Sync = dragging SNAPS through cycles/bar (the shared ladder).
    {
        auto shb = shapeBtnRect();   // SHAPE: cycles Sine -> Tri -> Saw -> Square -> Random
        const bool shOn = shape_[dest_] > 0;
        g.setColour(shOn ? destCol(dest_) : juce::Colour(0xff2a2a4a)); g.fillRoundedRectangle(shb, 3.0f);
        g.setColour(shOn ? juce::Colours::black : juce::Colour(0xff9aa6c0)); g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText(kLfoShapeName[juce::jlimit(0, 7, shape_[dest_])], shb.withTrimmedRight(10.0f), juce::Justification::centred, false);
        { juce::Path tri; const float tx = shb.getRight() - 8.0f, ty = shb.getCentreY();   // drawn triangle = dropdown
          tri.addTriangle(tx - 3.5f, ty - 2.0f, tx + 3.5f, ty - 2.0f, tx, ty + 3.0f);
          g.fillPath(tri); }
        auto fb = freeBtnRect();     // RETRIG -> FREE -> LEGATO (click cycles)
        const bool fOn2 = free_[dest_] || leg_[dest_];
        g.setColour(fOn2 ? destCol(dest_) : juce::Colour(0xff2a2a4a)); g.fillRoundedRectangle(fb, 3.0f);
        g.setColour(fOn2 ? juce::Colours::black : juce::Colour(0xff9aa6c0)); g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText(free_[dest_] ? "Free" : leg_[dest_] ? "Legato" : "Retrig", fb, juce::Justification::centred, false);
        auto sb = syncBtnRect();
        const float s = sync_[dest_];
        const bool on = s != 0.0f;
        g.setColour(on ? destCol(dest_) : juce::Colour(0xff2a2a4a)); g.fillRoundedRectangle(sb, 3.0f);
        g.setColour(on ? juce::Colours::black : juce::Colour(0xff9aa6c0)); g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText(s == 0.0f ? "Sync: Off" : (s < 0.0f ? "Sync: Key" : "Sync: Bar"), sb, juce::Justification::centred, false);
        g.setColour(juce::Colour(0xffaeb8d4)); g.setFont(juce::Font(9.5f, juce::Font::bold));
        const juce::String rd = s == 0.0f
            ? "Speed: " + juce::String(rate_[dest_], rate_[dest_] < 3.0f ? 1 : (rate_[dest_] > 99.0f ? 0 : 0)) + " Hz"
            : (s <= -1.5f ? "Speed: Key x" + juce::String(rate_[dest_], rate_[dest_] < 3.0f ? 2 : 1)   // rate = the RATIO in Key mode
               : "Speed: " + lfoCpbText(s));
        g.drawText(rd, 6, getHeight() - 15, getWidth() - 12, 12, juce::Justification::centredLeft, false);
    }
}
// TAB 4 = the Mod Env: a small A/D envelope graph (drag the PEAK handle = attack, END = decay).
void LfoDisplay::paintModEnv(juce::Graphics& g)
{
    const auto r = envRect();
    const juce::Colour hue (0xffb46bff);
    g.setColour(juce::Colour(0xff242440)); g.drawHorizontalLine((int) r.getBottom(), r.getX(), r.getRight());
    juce::Point<float> pk, hd, dc, rl; float peX = 0.0f;
    modEnvHandles(pk, hd, dc, rl, peX);
    // A-H-D-S-R polyline: rise to peak, hold flat, decay to sustain, sustain plateau, release to 0.
    juce::Path p; p.startNewSubPath(r.getX(), r.getBottom());
    p.lineTo(pk); p.lineTo(hd); p.lineTo(dc); p.lineTo(peX, dc.y); p.lineTo(rl);
    g.setColour(hue.withAlpha(0.22f)); g.fillPath(juce::Path(p));
    g.setColour(hue); g.strokePath(p, juce::PathStrokeType(1.8f));
    g.setColour(juce::Colours::white);
    for (auto& h : { pk, hd, dc, rl }) g.fillEllipse(h.x - 3.3f, h.y - 3.3f, 6.6f, 6.6f);
    g.setColour(juce::Colour(0xffaeb8d4)); g.setFont(juce::Font(9.0f, juce::Font::bold));
    auto secTxt = [](float s){ return s < 1.0f ? juce::String((int) std::round(s * 1000.0f)) + "ms"
                                               : juce::String(s, 2) + "s"; };
    g.drawText("A " + secTxt(modEnvA_) + " H " + secTxt(modEnvH_) + " D " + secTxt(modEnvD_)
                 + " S " + juce::String((int) std::round(modEnvS_ * 100.0f)) + "% R " + secTxt(modEnvR_),
               6, getHeight() - 15, getWidth() - 12, 12, juce::Justification::centredLeft, false);
}

void LfoDisplay::mouseDown(const juce::MouseEvent& e)
{
    const int d = destAt(e.position);
    if (d >= 0)
    {   // a tab click just SELECTS it (LFO routing lives in the matrix now, not a per-tab destination)
        if (d != dest_) { dest_ = d; repaint(); if (onDestChange) onDestChange(d); } return;
    }
    if (dest_ == 3)
    {   // TAB 4 = Mod Env: grab the NEAREST of the 4 handles (attack / hold / decay+sustain / release).
        juce::Point<float> pk, hd, dc, rl; float peX;
        modEnvHandles(pk, hd, dc, rl, peX);
        const juce::Point<float> hs[4] = { pk, hd, dc, rl };
        int best = 0; float bd = 1.0e9f;
        for (int i = 0; i < 4; ++i) { const float dd = e.position.getDistanceFrom(hs[i]); if (dd < bd) { bd = dd; best = i; } }
        envDrag_ = best;
        mouseDrag(e);   // apply immediately
        return;
    }
    if (shapeBtnRect().contains(e.position))
    {   // shape DROPDOWN (user: a chip-cycle hid the list); "Custom" = draw the cycle right on the wave view
        juce::PopupMenu m;
        for (int i = 0; i < 8; ++i) m.addItem(i + 1, kLfoShapeName[i], true, shape_[dest_] == i);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
                            localAreaToGlobal(shapeBtnRect().getSmallestIntegerContainer())),
            [this](int r) {
                if (r <= 0) return;
                shape_[dest_] = r - 1;
                if (onShapeChange) onShapeChange(dest_, shape_[dest_]);
                if (onDragEnd) onDragEnd();
                repaint();
            });
        return;
    }
    if (freeBtnRect().contains(e.position))
    {   // cycle Retrig -> Free -> Legato -> Retrig
        if      (! free_[dest_] && ! leg_[dest_]) { free_[dest_] = true; }
        else if (free_[dest_])                     { free_[dest_] = false; leg_[dest_] = true; }
        else                                       { leg_[dest_] = false; }
        if (onTrigChange) onTrigChange(dest_, free_[dest_], leg_[dest_]);
        if (onDragEnd) onDragEnd();
        repaint(); return;
    }
    if (syncBtnRect().contains(e.position))
    {   // cycle the selected LFO's sync mode: Off -> Sync (bar) -> KEY -> Off (Grid deleted).
        // KEY = the rate FOLLOWS THE PLAYED PITCH x a ratio (the audio-rate FM mode: the timbre stays
        // consistent across the keyboard). While in Key, dragging the wave sets the RATIO.
        // NO reference here + the MODE commits BEFORE the Key ratio's onChange: onChange's handler
        // re-reads the slot into setValues, and with the old order that clobbered a sync_ REFERENCE
        // back to the slot's stale Grid value = "stuck at Sync: Grid" (user bug).
        const float cur = sync_[dest_];
        float ns;
        if (cur == 0.0f)       ns = kLfoSyncCPB[lfoSyncNearestIdx(rate_[dest_] * barSec_)];   // keep ~the same speed
        else if (cur > 0.0f)   ns = -2.0f;    // key (GRID was deleted [2026-07-15 14:20] - bar sync covers it)
        else                   ns = 0.0f;     // back to free Hz
        sync_[dest_] = ns;
        if (onSyncChange) onSyncChange(dest_, ns);
        if (ns <= -1.5f && onChange) onChange(dest_, 1.0f, amt_[dest_]);   // key: start at ratio x1 (slot sync already written)
        if (onDragEnd) onDragEnd();
        repaint(); return;
    }
    dnPos_ = e.position; dnRate_ = rate_[dest_]; dnAmt_ = amt_[dest_]; dragging_ = true;
    dnMoved_ = false;
    dnSyncIdx_ = sync_[dest_] > 0.0f ? lfoSyncNearestIdx(sync_[dest_]) : 0;
}

void LfoDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (dest_ == 3 && envDrag_ >= 0)
    {   // Mod Env A-H-D-S-R: each handle maps its X into its band (decay handle's Y = sustain level).
        const auto r = envRect();
        const float x0 = r.getX(), w = juce::jmax(1.0f, r.getWidth());
        const float fx = (e.position.x - x0) / w;   // 0..1 across the whole graph
        switch (envDrag_)
        {
            case 0: modEnvA_ = f2A(fx / bA_); break;
            case 1: modEnvH_ = juce::jlimit(0.0f, 2.0f, (fx - bA_) / bH_ * 2.0f); break;
            case 2: modEnvD_ = f2D((fx - bA_ - bH_) / bD_);
                    modEnvS_ = juce::jlimit(0.0f, 1.0f, (r.getBottom() - e.position.y) / juce::jmax(1.0f, r.getHeight())); break;
            case 3: modEnvR_ = f2R((fx - (bA_ + bH_ + bD_ + bS_)) / (1.0f - (bA_ + bH_ + bD_ + bS_))); break;
        }
        if (onModEnvChange) onModEnvChange(modEnvA_, modEnvH_, modEnvD_, modEnvS_, modEnvR_);
        repaint(); return;
    }
    if (! dragging_) return;
    dnMoved_ = dnMoved_ || e.position.getDistanceFrom(dnPos_) > 3.0f;
    const float dx = e.position.x - dnPos_.x, dy = e.position.y - dnPos_.y;
    const float amt = juce::jlimit(0.0f, 1.0f, dnAmt_ - dy / 70.0f);
    if (amt > 0.001f) lastAmt_[dest_] = amt;
    const float s = sync_[dest_];
    if (s > 0.0f)
    {   // SYNC mode: the X-drag SNAPS through the musical cycles/bar detents (the wave IS the rate control)
        const int idx = juce::jlimit(0, kLfoSyncN - 1, dnSyncIdx_ + juce::roundToInt(dx / 16.0f));
        if (kLfoSyncCPB[idx] != sync_[dest_])
        {   // repaint HERE: the editor's setValues below is change-gated and sync_ is already updated
            // locally, so without this the wave + read-out froze while the value moved ("always 9/bar").
            sync_[dest_] = kLfoSyncCPB[idx];
            if (onSyncChange) onSyncChange(dest_, sync_[dest_]);
            repaint();
        }
        if (onChange) onChange(dest_, rate_[dest_], amt);   // Y still edits Amount
        return;
    }
    if (s <= -1.5f)
    {   // KEY mode: the X-drag sets the RATIO (played pitch x ratio = the LFO rate). Snaps to musical
        // ratios; SHIFT = free (the FM-Ratio convention).
        static const float kRatios[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 6.0f, 8.0f };
        float ratio = juce::jlimit(0.05f, 16.0f, dnRate_ * std::exp(dx * 0.01f));
        if (! e.mods.isShiftDown())
        {   int bi = 0; float bd = 1.0e9f;
            for (int k = 0; k < (int)(sizeof(kRatios) / sizeof(float)); ++k)
            { const float d2 = std::abs(std::log(ratio / kRatios[k])); if (d2 < bd) { bd = d2; bi = k; } }
            ratio = kRatios[bi]; }
        if (onChange) onChange(dest_, ratio, amt);
        return;
    }
    if (s < 0.0f)                                            // GRID mode: rate is automatic; only Amount drags
    { if (onChange) onChange(dest_, rate_[dest_], amt); return; }
    // free Hz: log drag - the top now reaches AUDIO RATES (FM/AM territory once routed to Pitch/Volume)
    const float rate = juce::jlimit(0.1f, 2000.0f, dnRate_ * std::exp(dx * 0.02f));
    if (onChange) onChange(dest_, rate, amt);   // edits ONLY the selected LFO
}

void LfoDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (destAt(e.position) >= 0) return;
    if (onChange) onChange(dest_, rate_[dest_], amt_[dest_] > 0.001f ? 0.0f : lastAmt_[dest_]);   // off <-> restore
}

juce::String LfoDisplay::getTooltip()
{
    if (shapeBtnRect().contains(getMouseXYRelative().toFloat()))
        return "SHAPE (click = list): the pattern this LFO wobbles in.\n\n"
               "- Sine / Tri: smooth back-and-forth.\n"
               "- Saw Dn / Saw Up: ramps - falling or rising rhythmic wobbles.\n"
               "- Square: jumps between two values - trills and gates.\n"
               "- Rnd Step: holds a NEW random value each cycle (NOT a random shape pick); every note "
               "rolls a fresh pattern. Rnd Glide: the same but sliding smoothly between values.\n"
               "- Custom: DRAW your own cycle right on the wave view (left-drag = draw, right-drag = "
               "the usual speed/amount). It starts as the shape you were on.\n\n"
               "The drawn wave is exactly what plays.";
    if (freeBtnRect().contains(getMouseXYRelative().toFloat()))
        return "RETRIG / FREE / LEGATO (click to cycle): where the wave starts on each note.\n\n"
               "- Retrig: the wave restarts at every note - predictable and punchy (drums, rhythmic wobbles).\n"
               "- Free: the LFO runs continuously and each note joins it MID-FLIGHT (living pads). "
               "Timeline-anchored: playback is identical every pass.\n"
               "- Legato: a note that starts while another is still SOUNDING inherits its wave mid-flight; "
               "detached notes restart. The mono-lead feel: the wobble survives a legato line.";
    if (syncBtnRect().contains(getMouseXYRelative().toFloat()))
        return "SYNC (click to cycle): Off -> Sync -> KEY, for the selected LFO tab.\n\n"
               "- Off = free speed in Hz (drag the wave left/right - now reaches AUDIO rates, 2 kHz: route a "
               "fast LFO to Pitch = real FM growl, to Volume = ring-mod textures).\n"
               "- Sync = tempo-synced: dragging the wave SNAPS through cycles-per-bar (every value to 21).\n"
               "- KEY = the speed FOLLOWS THE PLAYED PITCH x a ratio (drag the wave = the ratio, snaps to "
               "musical ratios, SHIFT = free). THE FM mode: the timbre stays consistent across the keyboard.\n"
               "The wave always draws the TRUE speed; the read-out beside this button shows it.";
    // Per-TAB tooltips (tabs: LFO 1/2/3 with an assignable audio-rate destination + a Mod Env).
    {
        const int tab = destAt(getMouseXYRelative().toFloat());
        if (tab == 3)
            return "Mod Env - a full A-H-D-S-R envelope you can route as a SOURCE in the matrix below.\n\n"
                   "- Drag the 4 handles: ATTACK peak, HOLD, DECAY (its height = SUSTAIN level), RELEASE.\n"
                   "- Like the amp envelope, it HOLDS at Sustain while the note is held and falls on Release.\n"
                   "- Route it (right-click a matrix fader) to sweep a filter, pitch, wave position, anything.";
        if (tab >= 0)
            return "LFO " + juce::String(tab + 1) + " - click to edit its wave, rate and shape.\n\n"
                   "- An LFO does NOTHING on its own: route it in the MATRIX faders below "
                   "(right-click a fader, pick LFO " + juce::String(tab + 1) + " as the source and a target).\n"
                   "- Drag the wave LEFT/RIGHT = speed, UP/DOWN = the LFO's amount (its depth).\n"
                   "- All 3 LFOs run at once (the 4th tab is the Mod Env).";
    }
    return "The selected LFO's wave - the drawing is exactly what plays.\n\n"
           "- Drag LEFT/RIGHT = speed - UP/DOWN = amount (wave height; 0 = off).\n"
           "- Route it in the MATRIX faders below (right-click a fader) to make it affect the sound.\n"
           "- The vertical line + white dot = the LIVE playhead while sound plays.\n\n"
           "Every tab and button here has its own tooltip - hover them.";
}

//==============================================================================
// KeysPanel (the on-screen piano)
//==============================================================================
// TintKeyboard: draw the note normally, then overlay the slot tint if this key is highlighted.
void TintKeyboard::drawWhiteNote(int n, juce::Graphics& g, juce::Rectangle<float> area,
                                 bool isDown, bool isOver, juce::Colour line, juce::Colour text)
{
    juce::MidiKeyboardComponent::drawWhiteNote(n, g, area, isDown, isOver, line, text);
    const auto c = tint[n];
    if (splitMark && n == 60 && c.getAlpha() == 0) {   // MERGE&SPLIT boundary: the WHOLE C4 key gets a
        const auto r = area.reduced(0.5f);             // VIOLET shade (the merge colour - reddish is taken
        g.setColour(juce::Colour(0x8ab46bff)); g.fillRect(r);   // by the C landmarks, tints by held keys)
        g.setColour(juce::Colour(0xffb46bff)); g.drawRect(area, 1.5f);
    }
    if (c.getAlpha() != 0) {                             // near-solid wash reads clearly on the white key
        g.setColour(c.withMultipliedAlpha(0.62f));
        g.fillRect(area.reduced(0.5f));
        g.setColour(c.darker(0.4f)); g.drawRect(area, 1.0f);
    }
    else if (dim[n]) {                                   // KEY GUIDE: out-of-scale white key = dark wash
        g.setColour(juce::Colour(0x63131320)); g.fillRect(area.reduced(0.5f));
    }
    { // EVERY key gets its note name: BLACK on white keys (C keys bold as landmarks); faded when dimmed
        const bool isC = (n % 12) == 0;
        if (isC && c.getAlpha() == 0 && ! (splitMark && n == 60))   // shade C keys softly (octave landmarks);
        {                               // tinted/held keys keep their highlight colour instead
            g.setColour(juce::Colour(dim[n] ? 0x0fb02020 : 0x1ab02020));
            g.fillRect(area.reduced(0.5f));
        }
        g.setColour(dim[n] && c.getAlpha() == 0 ? juce::Colour(isC ? 0x66c01818 : 0x59101018)
                                                : (isC ? juce::Colour(0xffd21f1f)              // C = bold RED landmarks
                                                       : juce::Colour(0xcc10141c)));
        g.setFont(juce::Font(juce::jmin(10.5f, area.getWidth() * 0.5f), isC ? juce::Font::bold : juce::Font::plain));
        g.drawFittedText(juce::MidiMessage::getMidiNoteName(n, true, true, 4),
                         area.reduced(1.0f, 2.0f).removeFromBottom(12.0f).toNearestInt(),
                         juce::Justification::centred, 1, 0.7f);
    }
}
void TintKeyboard::drawBlackNote(int n, juce::Graphics& g, juce::Rectangle<float> area,
                                 bool isDown, bool isOver, juce::Colour fill)
{
    const auto c = tint[n];
    // A translucent wash vanishes on black - fill the black key OPAQUE with a brightened tint instead,
    // keep a dark border so it still reads as a raised black key.
    juce::MidiKeyboardComponent::drawBlackNote(n, g, area, isDown, isOver,
                                               c.getAlpha() != 0 ? c.darker(0.15f)
                                             : dim[n]            ? juce::Colour(0xff9aa0ae)   // guide: dimmed = the SAME grey as faded whites
                                                                 : fill);
    if (c.getAlpha() != 0) { g.setColour(juce::Colour(0xff101018)); g.drawRect(area, 1.0f); }
    { // black keys get their name too (dark text on a dimmed/tinted key, light on plain black)
        const bool lightBg = dim[n] || c.getAlpha() != 0;   // dimmed = light-grey key, tinted = coloured key
        g.setColour(lightBg ? juce::Colour(0xdd14161e) : juce::Colour(0xf2ffffff));   // dark on light bg, WHITE on black keys
        g.setFont(juce::Font(8.0f, juce::Font::bold));
        g.drawFittedText(juce::MidiMessage::getMidiNoteName(n, true, true, 4),
                         area.reduced(0.5f, 2.0f).removeFromBottom(11.0f).toNearestInt(),
                         juce::Justification::centred, 1, 0.55f);
    }
}

//==============================================================================
// ScaleBox
static const juce::Colour kSlotCols[2] = { juce::Colour(0xffe8bf4d), juce::Colour(0xffe86aa8) };

void ScaleBox::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff16162a));
    g.setColour(juce::Colour(0xff33335a)); g.drawRect(getLocalBounds(), 1);
    const auto& s = v[juce::jlimit(0, 1, slot)];
    const auto acc = kSlotCols[juce::jlimit(0, 1, slot)];
    g.setColour(juce::Colour(0xff9aa4c0)); g.setFont(juce::Font(11.5f, juce::Font::bold));
    g.drawText("SCALE", 6, 1, 44, 14, juce::Justification::centredLeft, false);
    for (int i = 0; i < 2; ++i)   // slot chips (1 yellow / 2 pink, like the sound editor)
    {
        auto r = chipRect(i).toFloat();
        g.setColour(i == slot ? kSlotCols[i] : juce::Colour(0xff26264a)); g.fillRoundedRectangle(r, 3.0f);
        g.setColour(i == slot ? juce::Colours::black : kSlotCols[i]); g.setFont(juce::Font(10.0f, juce::Font::bold));
        g.drawText(juce::String(i + 1), chipRect(i), juce::Justification::centred, false);
    }
    { // KEY button (popup C..B)
        auto r = keyRect().toFloat();
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(r, 3.0f);
        g.setColour(s.on ? juce::Colour(0xffd8e0f0) : juce::Colour(0xff6a7690)); g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.drawText(juce::String("Key ") + kUiNoteName[juce::jlimit(0, 11, s.key)], keyRect(), juce::Justification::centred, false);
    }
    auto fader = [&](juce::Rectangle<int> r, float pos, const juce::String& t, bool active) {
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(r.toFloat(), 4.0f);
        g.setColour(acc.withAlpha(active ? 0.28f : 0.10f));
        g.fillRoundedRectangle(r.toFloat().withWidth(juce::jmax(6.0f, pos * (float) r.getWidth())), 4.0f);
        g.setColour(juce::Colour(0xffd8e0f0).withAlpha(active ? 0.9f : 0.55f)); g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText(t, r, juce::Justification::centred, false);
    };
    const bool gtrType = s.on && s.type >= 10;   // guitar voicings pick their own string count
    fader(notesRect(), gtrType ? 1.0f : (float) s.count / 7.0f,
          gtrType ? juce::String("Notes: Auto") : "Notes x" + juce::String(s.count), s.on && ! gtrType);
    fader(scaleRect(), s.on ? (float)(s.type + 2) / (float)(kNumScales + 1) : 1.0f / (float)(kNumScales + 1),
          s.on ? juce::String(kUiScaleName[juce::jlimit(0, kNumScales - 1, s.type)]) : juce::String("Off"), true);
}

void TunerBox::paint(juce::Graphics& g)
{   // "<note> <+/-cents>" with tuner-style dots: left = flat, right = sharp; green ring = in tune.
    g.fillAll(juce::Colour(0xff16162a));
    const auto r = getLocalBounds();
    const int cy = r.getCentreY();
    const bool inTune = std::abs(cents) <= 3;
    // LED-meter behaviour (user: the growing bar was "just wrong"): ONE dot lights at the
    // deviation's POSITION - dot i covers ~(i*12..i*12+12) cents on its side, like a clip tuner.
    const int pos = juce::jlimit(1, 4, (std::abs(cents) + 5) / 12 + (std::abs(cents) > 3 ? 1 : 0));
    for (int i = 0; i < 4; ++i)
    {
        auto dot = [&](int x, bool on) {
            g.setColour(on ? juce::Colour(0xffffb03a) : juce::Colour(0xff33335a));
            g.fillEllipse((float) x - 2.5f, (float) cy - 2.5f, 5.0f, 5.0f);
        };
        dot(r.getCentreX() - 40 - i * 12, ! inTune && cents < 0 && pos == i + 1);
        dot(r.getCentreX() + 40 + i * 12, ! inTune && cents > 0 && pos == i + 1);
    }
    g.setColour(inTune ? juce::Colour(0xff2f9e57) : juce::Colour(0xff777fa8));
    g.drawEllipse((float) r.getCentreX() - 28.0f, (float) cy - 4.5f, 9.0f, 9.0f, 1.5f);
    g.setColour(inTune ? juce::Colour(0xff7ae0a0) : juce::Colour(0xffd8e0f0));
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    g.drawText(note.isEmpty() ? juce::String("-")
                 : note + " " + (cents > 0 ? "+" : "") + juce::String(cents) + "c",
               r.getCentreX() - 14, r.getY(), 80, r.getHeight(), juce::Justification::centredLeft, false);
}

void ScaleBox::mouseDown(const juce::MouseEvent& e)
{
    dragNotes = dragScale = false;
    const auto p = e.getPosition();
    if ((e.mods.isPopupMenu() || e.mods.isRightButtonDown()) && mlm != nullptr)
    {   // [2026-07-16] right-click = MIDI-learn. SELECTION-scope by design: the CC always edits
        // whatever pattern/channel is selected AND whichever slot chip is active in THIS box.
        if (notesRect().contains(p)) { showMidiLearnMenu(this, *mlm, "ui_sel_scaleNotes", -1); return; }
        if (scaleRect().contains(p)) { showMidiLearnMenu(this, *mlm, "ui_sel_scaleType",  -1); return; }
        if (keyRect().contains(p))   { showMidiLearnMenu(this, *mlm, "ui_sel_scaleKey",   -1); return; }
        return;
    }
    for (int i = 0; i < 2; ++i) if (chipRect(i).contains(p))
    {   // [2026-07-16] the chip is a face of the editor's ONE slot selection (setShapeSlot syncs
        // the sound editor, filter/env targets and this box together - they must never diverge).
        if (onSlotSelect) onSlotSelect(i);
        else if (slot != i) { slot = i; repaint(); }
        return;
    }
    if (keyRect().contains(p))
    {
        juce::PopupMenu m;
        for (int k = 0; k < 12; ++k) m.addItem(k + 1, kUiNoteName[k], true, v[slot].key == k);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                            .withTargetScreenArea(localAreaToGlobal(keyRect())),   // drop from the Key button itself
            [this](int r) { if (r >= 1) { v[slot].key = r - 1; if (onChange) onChange(slot); repaint(); } });
        return;
    }
    if (notesRect().contains(p)) { dragNotes = true; mouseDrag(e); return; }
    if (scaleRect().contains(p)) { dragScale = true; mouseDrag(e); return; }
}

void ScaleBox::mouseDrag(const juce::MouseEvent& e)
{
    auto& s = v[juce::jlimit(0, 1, slot)];
    if (dragNotes)
    {
        if (s.on && s.type >= 10) return;   // guitar voicings: string count is AUTO (per chord shape)
        auto r = notesRect();
        const float f = juce::jlimit(0.0f, 1.0f, (float)(e.getPosition().x - r.getX()) / (float) juce::jmax(1, r.getWidth()));
        const int c = juce::jlimit(1, 7, 1 + (int) (f * 7.0f));
        if (c != s.count) { s.count = c; if (onChange) onChange(slot); repaint(); }
        return;
    }
    if (dragScale)
    {   // Off + every scale (incl. the 2 GUITAR voicings) = kNumScales + 1 positions
        auto r = scaleRect();
        const float f = juce::jlimit(0.0f, 1.0f, (float)(e.getPosition().x - r.getX()) / (float) juce::jmax(1, r.getWidth()));
        const int pos = juce::jlimit(0, kNumScales, (int) (f * (float)(kNumScales + 1)));
        const bool nOn = pos > 0; const int nType = juce::jmax(0, pos - 1);
        if (nOn != s.on || (nOn && nType != s.type)) { s.on = nOn; if (nOn) s.type = nType; if (onChange) onChange(slot); repaint(); }
        return;
    }
}

juce::String ScaleBox::getTooltip()
{
    const auto p = getMouseXYRelative();
    if (chipRect(0).contains(p) || chipRect(1).contains(p))
        return "Which SOUND SLOT these scale controls edit: 1 = yellow, 2 = pink (each slot has its own scale). "
               "This is the SAME slot selection as the sound editor's - switching here switches there too "
               "(and the ui_sel_slotSel MIDI target flips both).";
    if (keyRect().contains(p))
        return "KEY: the scale's root note (C..B). Right-click = assign a MIDI knob (acts on the selected channel + this box's slot chip).";
    if (notesRect().contains(p))
        return "NOTES (drag): the chord size the harmonizer plays per key - x1 = just the (snapped) note, "
               "x3 = triads, x4 = 7th chords. Right-click = assign a MIDI knob.";
    if (scaleRect().contains(p))
        return "SCALE (drag): Off, or the scale the slot plays in. With a scale ON, every key you press plays "
               "a full chord built from that key inside the scale; off-scale keys snap to the nearest scale note. "
               "Right-click = assign a MIDI knob.";
    return "SCALE harmonizer for the selected channel's sound slots: one key = a full in-scale chord.\n\n"
           "- Pick the slot (1/2), a Key, a scale and the chord size (Notes).\n"
           "- The read-outs above the keys name what sounds LIVE: yellow = slot 1, pink = slot 2, "
           "white ALL = both combined (no matching chord = the note list).";
}

//==============================================================================
// KeySplitBox (Merge & Split)
static const juce::Colour kPairCols[2] = { juce::Colour(0xffff9f43),    // FIRST channel / RIGHT half = orange
                                           juce::Colour(0xff3ec6a8) };  // SECOND channel / LEFT half = teal

void KeySplitBox::paint(juce::Graphics& g)
{
    const bool on = merged();
    auto ob = onRect().toFloat();
    g.setColour(on ? juce::Colour(0xff35b56a) : juce::Colour(0xff33335a)); g.fillRoundedRectangle(ob, 4.0f);
    g.setColour(on ? juce::Colours::black : juce::Colour(0xffb8b8d0)); g.setFont(juce::Font(12.0f, juce::Font::bold));
    g.drawFittedText("Merge\n& Split", onRect().reduced(2), juce::Justification::centred, 2, 0.8f);
    for (int i = 0; i < 2; ++i)
    {
        const int  ws  = (i == 0) ? w1 : w2;
        const auto acc = kPairCols[i];
        auto r = boxRect(i);
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(r.toFloat(), 4.0f);
        const float x0 = (float) r.getX() + (float)(ws - 12) / 96.0f * (float) r.getWidth();
        const float ww = 48.0f / 96.0f * (float) r.getWidth();
        juce::Rectangle<float> win(x0, (float) r.getY() + 1.5f, ww, (float) r.getHeight() - 3.0f);
        g.setColour(acc.withAlpha(on ? 0.30f : 0.10f)); g.fillRoundedRectangle(win, 3.0f);
        g.setColour(acc.withAlpha(on ? 0.95f : 0.35f)); g.drawRoundedRectangle(win, 3.0f, 1.2f);
        g.setColour(on ? juce::Colours::white : juce::Colour(0xff6a7690)); g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawFittedText(on ? juce::MidiMessage::getMidiNoteName(ws, true, true, 4) + "-"
                              + juce::MidiMessage::getMidiNoteName(ws + 48, true, true, 4)
                            : juce::String("off"),
                         win.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        // corner tag = the pair's CHANNEL NUMBER for this half (R = right hand, L = left hand)
        g.setColour(acc.withAlpha(on ? 0.95f : 0.4f)); g.setFont(juce::Font(9.0f, juce::Font::bold));
        const juce::String tag = on ? (i == 0 ? "R " + juce::String(chFirst + 1) : "L " + juce::String(chSecond + 1))
                                    : juce::String(i == 0 ? "R" : "L");
        g.drawText(tag, r.getX() + 3, r.getY() + 1, 26, 10, juce::Justification::topLeft, false);
    }
}
void KeySplitBox::dragTo(int i, int x)
{
    auto r = boxRect(i);
    const float frac = juce::jlimit(0.0f, 1.0f, (float)(x - r.getX()) / (float) juce::jmax(1, r.getWidth()));
    const int start = juce::jlimit(12, 60, 12 + 12 * (int) std::lround((frac * 96.0f - 24.0f) / 12.0f));   // octave snap
    int& tgt = (i == 0) ? w1 : w2;
    if (start != tgt) { tgt = start; if (onChange) onChange(); repaint(); }
}
void KeySplitBox::mouseDown(const juce::MouseEvent& e)
{
    dragBox = -1;
    if (onRect().contains(e.getPosition())) { if (onMergeClick) onMergeClick(); return; }
    if (! merged()) return;
    for (int i = 0; i < 2; ++i)
        if (boxRect(i).contains(e.getPosition())) { dragBox = i; dragTo(i, e.getPosition().x); return; }
}
void KeySplitBox::mouseDrag(const juce::MouseEvent& e) { if (dragBox >= 0) dragTo(dragBox, e.getPosition().x); }
juce::String KeySplitBox::getTooltip()
{
    const auto p = getMouseXYRelative();
    if (onRect().contains(p))
        return "MERGE & SPLIT: pairs this channel with the PREVIOUS or NEXT one (click for the popup; "
               "SHIFT+click a channel number does the same with its previous channel).\n\n"
               "- Pairs are PER PATTERN (they don't spread to every pattern; if the PATTERNS are merged "
               "into a group, the pairing follows the whole group).\n"
               "- The keyboard splits BETWEEN the pair: keys from middle C up play the FIRST (lower-numbered) "
               "channel, keys below it the SECOND - two full sounds, one keyboard.\n"
               "- Merged channels are PIANO-ROLL only, so pairing a step channel CLEARS its steps (it warns "
               "first; you can Undo). Each half RECORDS into its own channel's roll (looper-style).\n"
               "- Select either channel to edit its sound - selection never blocks the pairing.";
    if (boxRect(0).contains(p) || boxRect(1).contains(p))
        return "This half's 4-OCTAVE WINDOW (drag; snaps to octaves; needs a merged pair). The whole box = "
               "C0..C8; the coloured window = the pitches this half of the keyboard actually plays. Orange = "
               "the FIRST channel (right hand), teal = the SECOND (left hand); the corner tag shows WHICH "
               "channel number that is.";
    return "Merge & Split: pair two adjacent channels and split the keyboard between them.";
}

KeysPanel::KeysPanel(MidiLearnManager& mlm)
    : humanKnob("ui_sel_slotOfs", mlm), strumKnob("ui_sel_strum", mlm),
      minVelKnob("ui_sel_minVel", mlm), maxVelKnob("ui_sel_maxVel", mlm),
      glideKnob("ui_sel_glide", mlm)
{
    btnRec.paramId = "ui_sel_rec"; btnRec.midiLearn = &mlm;   // REC is MIDI-learnable too
    kbState.addListener(this);
    addAndMakeVisible(kb);
    kb.setAvailableRange(12, 108);                      // C0..C8 (scientific) = +-48 st around middle C - contains a full 88-key piano
    kb.setLowestVisibleKey(36);                         // open around C2 (bass-friendly)
    kb.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, juce::Colour(0xaae8bf4d));
    kb.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, juce::Colour(0x44e8bf4d));

    auto combo = [this](juce::ComboBox& c) { addAndMakeVisible(c); };
    combo(comboRecMode);
    addAndMakeVisible(btnSlot2);
    btnSlot2.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    auto knob = [this](juce::Slider& s) {
        addAndMakeVisible(s);
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 12);   // LIVE % read-out under the dial
        s.setRange(0.0, 1.0, 0.01); s.setDoubleClickReturnValue(true, 0.0);
        s.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        s.valueFromTextFunction = [](const juce::String& t) { return t.getDoubleValue() / 100.0; };
        s.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffe8bf4d));
        s.setColour(juce::Slider::textBoxTextColourId,    juce::Colour(0xffc8d0e0));
        s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    };
    knob(humanKnob); knob(strumKnob); knob(minVelKnob); knob(maxVelKnob); knob(glideKnob);
    // STRUM is STEPPED to 0/20/40/60/80/100% (user: match the piano-roll right-click's fixed
    // options so live playing and a per-note override can never subtly differ). The other feel
    // knobs stay continuous (they're amounts/times, not a value the roll also edits per-note).
    strumKnob.setRange(0.0, 1.0, 0.2);
    // SLOT OFFSET reads in MILLISECONDS (0..100 ms), CONSISTENT per hit (user: "use ms, not percentage").
    humanKnob.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " ms"; };
    humanKnob.valueFromTextFunction = [](const juce::String& t) { return t.getDoubleValue() / 100.0; };
    maxVelKnob.setValue(1.0);   // default full
    addAndMakeVisible(btnRec);
    btnRec.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    addAndMakeVisible(btnTakes);
    btnTakes.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));

    auto lab = [this](juce::Label& l, const juce::String& t) {
        addAndMakeVisible(l); l.setText(t, juce::dontSendNotification);
        l.setFont(juce::Font(12.0f, juce::Font::bold)); l.setJustificationType(juce::Justification::centred);
        l.setMinimumHorizontalScale(0.72f);   // squeeze long words instead of "..." (user: must read the FULL text)
        l.setColour(juce::Label::textColourId, juce::Colour(0xffaebadA));
    };
    lab(lblRecMode, "Record mode"); lab(lblSlot2, "Slot 2 pitch");
    lab(lblHuman, "Slot offset"); lab(lblStrum, "Strum"); lab(lblMinVel, "Min vel"); lab(lblMaxVel, "Max vel");
    lab(lblPoly, "Mode"); lab(lblGlide, "Glide");
    addAndMakeVisible(btnPlayMode);
    btnPlayMode.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    addAndMakeVisible(btnArp);
    btnArp.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    addAndMakeVisible(btnGuide);
    addAndMakeVisible(lblGuideCur);
    lblGuideCur.setJustificationType(juce::Justification::centred);
    lblGuideCur.setFont(juce::Font(11.0f, juce::Font::bold));
    lblGuideCur.setMinimumHorizontalScale(0.6f);
    lblGuideCur.setColour(juce::Label::textColourId, juce::Colour(0xffaebada));
    lblGuideCur.setInterceptsMouseClicks(false, false);
    btnGuide.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    addAndMakeVisible(scaleBox);
    addAndMakeVisible(tunerBox);
    addAndMakeVisible(splitBox);
    { static const juce::Colour cc[3] = { juce::Colour(0xffe8bf4d), juce::Colour(0xffe86aa8), juce::Colour(0xffd8e0f0) };
      for (int i = 0; i < 3; ++i)
      {
          addAndMakeVisible(lblChord[i]);
          lblChord[i].setFont(juce::Font(20.0f, juce::Font::bold));
          lblChord[i].setColour(juce::Label::textColourId, cc[i]);
          lblChord[i].setJustificationType(juce::Justification::centredLeft);
          lblChord[i].setMinimumHorizontalScale(0.55f);   // squeeze, never "..." (user)
          lblChord[i].setInterceptsMouseClicks(false, false);
      } }
    btnArp.setTooltip("ARP: hold ONE key and it plays a programmed riff, locked to the beat. Click to open "
        "the editor - every control in it has its OWN tooltip. Works stopped or playing; records into the "
        "piano roll; mono, and chord/scale/glide apply per note.");
    btnArp.onClick = [this] { arpEditor.setVisible(! arpEditor.isVisible()); arpEditor.toFront(false); };
    arpEditor.clickIgnore = &btnArp;   // the button's own click toggles; anything else outside closes
    addChildComponent(arpEditor);      // hidden until the Arp button (costs no permanent space)
}

//==============================================================================
// ARP EDITOR (a dropdown-style popup under the Arp button)

void ArpEditor::visibilityChanged()
{
    // Hook/unhook the global click-outside closer (a real dropdown closes when you click elsewhere).
    if (isVisible() && ! closerHooked)      { juce::Desktop::getInstance().addGlobalMouseListener(&closer); closerHooked = true; }
    else if (! isVisible() && closerHooked) { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); closerHooked = false; }
}

ArpEditor::~ArpEditor()
{
    if (closerHooked) juce::Desktop::getInstance().removeGlobalMouseListener(&closer);
}

static const char* kArpRateName11(int r)
{ static const char* n[11] = { "x0.25", "x0.33", "x0.5", "x0.75", "x1", "x1.25", "x1.5", "x1.75", "x2", "x2.5", "x3" };
  return n[juce::jlimit(0, 10, r)]; }

void ArpEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a2c));
    g.setColour(juce::Colour(0xff35c0ff)); g.drawRect(getLocalBounds(), 1);
    // --- top bar: On | Rate | Notes/bar FADER | Notes(length) ---
    auto ob = onRect().toFloat();
    g.setColour(on ? juce::Colour(0xff35b56a) : juce::Colour(0xff33335a)); g.fillRoundedRectangle(ob, 4.0f);
    g.setColour(on ? juce::Colours::black : juce::Colour(0xffb8b8d0)); g.setFont(juce::Font(12.5f, juce::Font::bold));
    g.drawText(on ? "ON" : "OFF", onRect(), juce::Justification::centred, false);
    { // RATE: a click-and-drag FADER inside the button shape (subtle fill = position; label semi-transparent)
        auto rr = rateRect();
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(rr.toFloat(), 4.0f);
        const float pos = (float)(rate + 1) / (float) DrumChannel::ARP_RATES;
        g.setColour(juce::Colour(0xff35c0ff).withAlpha(0.22f));
        g.fillRoundedRectangle(rr.toFloat().withWidth(juce::jmax(6.0f, pos * (float) rr.getWidth())), 4.0f);
        g.setColour(juce::Colour(0xffc8d0e0).withAlpha(0.85f)); g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText(juce::String("Rate ") + kArpRateName11(rate), rr, juce::Justification::centred, false);
    }
    { // NOTES/BAR: a drag-fader box like Rate ("Notes/bar x8"; fill = position among the 6 detents +
      //           the MAX = "Grid" = LOCK TO GRID, following the channel's grid division)
        auto f = faderRect();
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(f.toFloat(), 4.0f);
        int selIdx = 6; for (int i = 0; i < 6; ++i) if (DrumChannel::ARP_SYNCS[i] == sync) selIdx = i;   // 6 = grid lock
        g.setColour(juce::Colour(0xffe8bf4d).withAlpha(0.22f));
        g.fillRoundedRectangle(f.toFloat().withWidth(juce::jmax(6.0f, (float)(selIdx + 1) / 7.0f * (float) f.getWidth())), 4.0f);
        g.setColour(juce::Colour(0xffd8e0f0).withAlpha(0.85f)); g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText(sync < 0 ? juce::String("Notes/bar Grid") : ("Notes/bar x" + juce::String(sync)), f, juce::Justification::centred, false);
    }
    auto stepBtn = [&](juce::Rectangle<int> rr, const juce::String& t) {
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(rr.toFloat(), 4.0f);
        g.setColour(juce::Colour(0xffc8d0e0)); g.setFont(juce::Font(13.0f, juce::Font::bold));
        g.drawText(t, rr, juce::Justification::centred, false); };
    stepBtn(lenDnRect(), "<");
    g.setColour(juce::Colour(0xffc8d0e0)); g.setFont(juce::Font(12.5f, juce::Font::bold));
    g.drawText("Last note " + juce::String(len), lenValRect(), juce::Justification::centred, false);   // = the note ordinal the pattern ends on
    stepBtn(lenUpRect(), ">");
    // --- row 2: Align / Hold toggles + the Gate drag-fader ---
    auto togBtn = [&](juce::Rectangle<int> rr, bool state, const juce::String& t) {
        g.setColour(state ? juce::Colour(0xff35b56a) : juce::Colour(0xff33335a)); g.fillRoundedRectangle(rr.toFloat(), 4.0f);
        g.setColour(state ? juce::Colours::black : juce::Colour(0xffb8b8d0)); g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText(t, rr, juce::Justification::centred, false); };
    togBtn(alignRect(), align, "Align bar");
    togBtn(holdRect(),  hold,  "Hold");
    togBtn(altRect(),   altStrum, "Alt strum");   // up, down, up... (needs the channel's STRUM up)
    { // GATE drag-fader (same style as Rate): note length as % of one arp step
        auto gr = gateRect();
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(gr.toFloat(), 4.0f);
        const float pos = juce::jlimit(0.0f, 1.0f, (gate - 0.1f) / 0.9f);
        g.setColour(juce::Colour(0xff35c0ff).withAlpha(0.22f));
        g.fillRoundedRectangle(gr.toFloat().withWidth(juce::jmax(6.0f, pos * (float) gr.getWidth())), 4.0f);
        g.setColour(juce::Colour(0xffc8d0e0).withAlpha(0.75f)); g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText("Gate " + juce::String(juce::roundToInt(gate * 100.0f)) + "%", gr, juce::Justification::centred, false);
    }

    // --- 2 x 6 cells (notes 2..13): big "Note N" header + big value ("+5 st" / "0 st" / "REST") ---
    for (int i = 0; i < DrumChannel::ARP_ROWS; ++i)
    {
        auto c = cellRect(i).reduced(2);
        const bool active = on && i < len - 1;                 // row i (= note i+2) plays only within the length
        const bool rest = off[i] == DrumChannel::ARP_REST;
        const bool isLast = (i == len - 2);
        g.setColour(juce::Colour(active ? 0xff232342 : 0xff16162a)); g.fillRect(c);
        g.setColour(isLast ? juce::Colour(0xffe8bf4d) : juce::Colour(active ? 0xff35406a : 0xff262636)); g.drawRect(c, isLast ? 2 : 1);
        // header: the note ordinal, BIG + explicit (click it = make this the LAST note)
        g.setColour(isLast ? juce::Colour(0xffe8bf4d) : (active ? juce::Colour(0xffb8c4dc) : juce::Colour(0xff53607a)));
        g.setFont(juce::Font(13.0f, juce::Font::bold));
        g.drawText("Note " + juce::String(i + 2), c.withHeight(18), juce::Justification::centred, false);
        // + / - strips (click = one semitone; scroll and drag also work)
        g.setColour(juce::Colour(active ? 0x66ffffff : 0x33ffffff)); g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText("+", c.withY(c.getY() + 18).withHeight(14), juce::Justification::centred, false);
        g.drawText(juce::String::fromUTF8("\xe2\x88\x92"), c.withTop(c.getBottom() - 16), juce::Justification::centred, false);
        // the VALUE, big: semitones with the unit ("+5 st"); REST is distinct from "0 st"
        g.setColour(rest ? juce::Colour(active ? 0xff8a94ac : 0xff4a5068)
                         : (active ? juce::Colours::white : juce::Colour(0xff70809a)));
        g.setFont(juce::Font(rest ? 14.0f : 17.0f, juce::Font::bold));
        g.drawText(rest ? "REST" : ((off[i] > 0 ? "+" : "") + juce::String((int) off[i]) + " st"),
                   c.reduced(0, 20), juce::Justification::centred, false);
    }
}

juce::String ArpEditor::getTooltip()
{
    const auto p = getMouseXYRelative();
    if (onRect().contains(p))
        return "Arp ON/OFF for this channel. Hold ONE key on the keyboard and it plays your programmed riff, "
               "looping while held.";
    if (rateRect().contains(p))
        return "RATE (drag inside the box, x0.25..x3): multiplies Notes/bar. Example: Notes/bar 8 x Rate 2 = "
               "16 notes per bar (classic 16ths).";
    if (faderRect().contains(p))
        return "NOTES/BAR (drag, 7-13): the arp's base grid - how many notes fill one bar at Rate x1. "
               "Odd values (7/9/11/13) give polyrhythms; get 12/16/24 via Rate (8 x 1.5 = 12, 8 x 2 = 16).\n\n"
               "- All the way RIGHT = LOCK TO GRID: Notes/bar follows the channel's grid automatically "
               "(piano-roll Grid 1/N, else the step count) - so odd grids like 1/23 just work.";
    if (alignRect().contains(p))
        return "ALIGN BAR: while the beat plays, the riff plays as if you had pressed the key EXACTLY on the "
               "bar start - notes land on the arp's OWN grid (Notes/bar x Rate), in time with the pattern.\n\n"
               "It does NOT use the channel's step count; only Notes/bar and Rate set the spacing. When the "
               "transport is stopped it free-runs from your keypress.";
    if (holdRect().contains(p))
        return "HOLD (latch): release the key and the arp keeps looping on its own. Press the SAME key again "
               "to stop it; a DIFFERENT key re-roots the riff. The plugin's STOP button also ends it.";
    if (altRect().contains(p))
        return "ALT STRUM: every other arp note strums the chord in the OPPOSITE direction - up, down, up... "
               "like a real strumming hand. Needs the channel's STRUM knob (KEYS panel) up and a Chord/Scale "
               "voicing on the sound to hear anything.";
    if (gateRect().contains(p))
        return "GATE (drag, 10-100%): each note's length as a fraction of its step. 100% = a note rings until "
               "the next one (legato). Lower = staccato: the note is released early with the sound's own "
               "release fade. The keyboard highlight follows it.";
    if (lenDnRect().contains(p) || lenValRect().contains(p) || lenUpRect().contains(p))
        return "LAST NOTE: where the pattern ends before looping, counting the root as note 1. 'Last note 4' = "
               "root + notes 2-4, then back to the root. Clicking a cell's 'Note N' header sets this too.";
    if (p.y >= TOPH)
        return "One riff note (offset from the pressed key). Click the top/bottom strip or SCROLL = one "
               "semitone (-24..+24 st; 0 st = the root pitch again). DRAG the middle = fast set. "
               "DOUBLE-CLICK = 0 st; RIGHT-CLICK = REST (a silent step). Click the 'Note N' header = "
               "make it the LAST note.";
    return {};
}

void ArpEditor::mouseDown(const juce::MouseEvent& e)
{
    dragCell = -1; dragFader = false; dragRate = false; dragGate = false;
    const auto p = e.getPosition();
    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())   // RIGHT-click a cell = REST (user spec)
    {
        const int i = cellAt(p);
        if (i >= 0) { off[i] = DrumChannel::ARP_REST; if (i + 2 > len) len = i + 2;
                      if (onChange) onChange(); repaint(); }
        return;
    }
    if (onRect().contains(p))    { on = ! on; if (onChange) onChange(); repaint(); return; }
    if (rateRect().contains(p))  { dragRate = true; mouseDrag(e); return; }   // drag-fader inside the box
    if (alignRect().contains(p)) { align = ! align; if (onChange) onChange(); repaint(); return; }
    if (holdRect().contains(p))  { hold = ! hold; if (onChange) onChange(); repaint(); return; }
    if (altRect().contains(p))   { altStrum = ! altStrum; if (onChange) onChange(); repaint(); return; }
    if (gateRect().contains(p))  { dragGate = true; mouseDrag(e); return; }
    if (faderRect().contains(p)) { dragFader = true; mouseDrag(e); return; }
    if (lenDnRect().contains(p)) { len = juce::jmax(1, len - 1); if (onChange) onChange(); repaint(); return; }
    if (lenUpRect().contains(p)) { len = juce::jmin(1 + DrumChannel::ARP_ROWS, len + 1); if (onChange) onChange(); repaint(); return; }
    const int i = cellAt(p);
    if (i < 0) return;
    auto cell = cellRect(i).reduced(2);
    if (p.y <= cell.getY() + 18)              { len = juce::jlimit(1, 1 + DrumChannel::ARP_ROWS, i + 2);   // header = make LAST
                                                if (onChange) onChange(); repaint(); return; }
    if (p.y <= cell.getY() + 34)              { bump(i, +1); return; }   // "+" strip
    if (p.y >= cell.getBottom() - 16)         { bump(i, -1); return; }   // "-" strip
    dragCell = i;                                                        // middle = fast drag
    mouseDrag(e);
}

void ArpEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (dragGate)
    {   // GATE fader: absolute position -> 10..100% (5% steps)
        auto gr = gateRect();
        const float f = juce::jlimit(0.0f, 1.0f, (float)(e.getPosition().x - gr.getX()) / (float) juce::jmax(1, gr.getWidth()));
        const float ng = 0.1f + std::round(f * 18.0f) / 18.0f * 0.9f;
        if (std::abs(ng - gate) > 0.001f) { gate = ng; if (onChange) onChange(); repaint(); }
        return;
    }
    if (dragRate)
    {   // RATE fader: absolute position across the box picks one of the 11 rates
        auto rr = rateRect();
        const float f = juce::jlimit(0.0f, 1.0f, (float)(e.getPosition().x - rr.getX()) / (float) juce::jmax(1, rr.getWidth()));
        const int idx = juce::jlimit(0, DrumChannel::ARP_RATES - 1, (int) (f * (float) DrumChannel::ARP_RATES));
        if (idx != rate) { rate = idx; if (onChange) onChange(); repaint(); }
        return;
    }
    if (dragFader)
    {   // Notes/bar box: absolute position picks one of the 6 detents, or the MAX = LOCK TO GRID (-1)
        auto tr = faderRect();
        const float f = juce::jlimit(0.0f, 1.0f, (float) (e.getPosition().x - tr.getX()) / (float) juce::jmax(1, tr.getWidth()));
        const int idx = juce::jlimit(0, 6, (int) (f * 7.0f));
        const int ns  = idx >= 6 ? -1 : DrumChannel::ARP_SYNCS[idx];
        if (ns != sync) { sync = ns; if (onChange) onChange(); repaint(); }
        return;
    }
    if (dragCell < 0) return;
    auto cell = cellRect(dragCell).reduced(2);
    const int top = cell.getY() + 34, bot = cell.getBottom() - 16;
    const float f = 1.0f - 2.0f * (float) (e.getPosition().y - top) / (float) juce::jmax(1, bot - top);
    off[dragCell] = (int8_t) juce::jlimit(-24, 24, (int) std::lround(f * 24.0f));
    if (dragCell + 2 > len) len = dragCell + 2;
    if (onChange) onChange(); repaint();
}

void ArpEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    const int i = cellAt(e.getPosition());
    if (i < 0) return;
    off[i] = 0;   // double-click = 0 st (the root pitch); RIGHT-click = REST (user spec)
    if (i + 2 > len) len = i + 2;
    if (onChange) onChange(); repaint();
}

void ArpEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int i = cellAt(e.getPosition());
    if (i >= 0) bump(i, w.deltaY >= 0 ? +1 : -1);   // precise +/-1 (includes 0)
}

void KeysPanel::resized()
{
    auto r = getLocalBounds().reduced(8);
    // ONE aligned control row (label over control), the hint fills the right side, the piano
    // takes the rest (slightly shorter than v1 so the strip isn't cramped).
    auto strip = r.removeFromTop(50);
    auto place = [&strip](juce::Component& c, juce::Label* l, int w) {
        auto col = strip.removeFromLeft(w);
        if (l) { l->setBounds(col.removeFromTop(15)); }
        else    col.removeFromTop(15);
        c.setBounds(col.reduced(2, 1));
    };
    place(btnRec,       nullptr,      78);
    strip.removeFromLeft(6);
    place(comboRecMode, &lblRecMode, 224);
    strip.removeFromLeft(6);
    place(btnTakes,     nullptr,      96);   // smaller (user: shrink Takes/Slot2 to fit the knobs, not the knobs)
    strip.removeFromLeft(6);
    place(btnSlot2,     &lblSlot2,    74);   // smaller ("+12 st" still fits)
    strip.removeFromLeft(10);
    // HUMANIZE / STRUM / MIN / MAX VEL: full-size dials (their cell borrows ~16 px below the strip; the
    // keyboard is pushed down to match) - kept big on purpose; the Takes/Slot2 dropdowns shrank instead.
    auto placeKnob = [](juce::Slider& s, juce::Label& l, juce::Rectangle<int> cell) {
        l.setBounds(cell.removeFromTop(14));
        s.setBounds(cell.reduced(3, 0));
    };
    auto knobArea = strip.removeFromLeft(5 * 62);
    knobArea.setBottom(knobArea.getBottom() + 42);          // taller cell (uses the free band below): bigger dial + read-out
    placeKnob(humanKnob,  lblHuman,  knobArea.removeFromLeft(62));
    placeKnob(strumKnob,  lblStrum,  knobArea.removeFromLeft(62));
    placeKnob(minVelKnob, lblMinVel, knobArea.removeFromLeft(62));
    placeKnob(maxVelKnob, lblMaxVel, knobArea.removeFromLeft(62));
    placeKnob(glideKnob,  lblGlide,  knobArea);
    strip.removeFromLeft(12);
    { auto col = strip.removeFromLeft(94);                                   // Mode / Arp / Guide: ONE column
      // [2026-07-16] three SAME-SIZE stacked popup buttons (user: they should look alike, and
      // "Mono Legato" needs the width) - the play-mode dropdown replaced the Poly toggle.
      lblPoly.setBounds(col.removeFromTop(13));
      btnPlayMode.setBounds(col.getCentreX() - 43, col.getY(),      86, 16);
      btnArp.setBounds     (col.getCentreX() - 43, col.getY() + 19, 86, 16);
      btnGuide.setBounds   (col.getCentreX() - 43, col.getY() + 38, 86, 16);
      lblGuideCur.setBounds(col.getX() - 3, col.getY() + 55, col.getWidth() + 6, 15); }   // its key+scale
    strip.removeFromLeft(8);
    // SCALE box rides in the SAME strip row, now FULL-HEIGHT down to the band bottom (user: use the space)
    scaleBox.setBounds(strip.getX(), strip.getY(), juce::jmax(150, strip.getWidth() - 2), 76);
    tunerBox.setBounds(strip.getX(), strip.getY() + 78, juce::jmax(150, strip.getWidth() - 2), 16);

    r.removeFromTop(4);
    { // the BAND under the REC/mode/Takes/Slot2 row: SPLIT control + the three live chord names
      // (the knobs / Poly column / SCALE box overhang into this band further right - widths never collide)
        auto band = r.removeFromTop(44);
        splitBox.setBounds(band.getX(), band.getY(), 216, 42);   // toggle + the two STACKED window boxes
        // LIVE CHORD NAMES: one big row spread across the rest of the free line (up to the knob columns)
        auto nb = juce::Rectangle<int>(band.getX() + 224, band.getY(), 500 - 224, 44);
        lblChord[0].setBounds(nb.removeFromLeft(78));   // slot 1 (yellow)
        lblChord[1].setBounds(nb.removeFromLeft(78));   // slot 2 (pink)
        lblChord[2].setBounds(nb);                      // ALL (white, widest - inversions are long)
    }
    r.removeFromTop(2);
    kb.setBounds(r);
    // ARP editor = a DROPDOWN under the Arp button: anchored to it (right-aligned so it stays inside
    // the panel), as tall as the keyboard area allows (2x the first version - the cells are big now).
    { const int pw = juce::jmin(780, getWidth() - 16);   // 780: Alt-strum needs its own slot in the header row
      const int px = juce::jlimit(8, getWidth() - pw - 8, btnArp.getRight() - pw);
      const int py = btnArp.getBottom() + 4;
      arpEditor.setBounds(px, py, pw, juce::jmax(120, getHeight() - py - 10)); }
    // white-key width so the full C0..C8 range fits the panel (57 white keys in 8 octaves)
    kb.setKeyWidth(juce::jmax(7.0f, (float) r.getWidth() / 57.0f));
}

void KeysPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff12121f));
    g.setColour(juce::Colour(0xff33335a));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.2f);
    g.setColour(juce::Colour(0xff7799cc));
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    g.drawText("KEYS", 12, 2, 60, 14, juce::Justification::centredLeft, false);
    // (The count-in "3-2-1-GO!" is drawn full-canvas by CountdownOverlay, not here.)
}

void KeysPanel::handleNoteOn(juce::MidiKeyboardState*, int, int note, float vel)
{
    held.removeFirstMatchingValue(note);
    held.add(note);
    lastVel = vel;
    if (onKeyDown) onKeyDown(note, vel);
}

void KeysPanel::handleNoteOff(juce::MidiKeyboardState*, int, int note, float)
{
    const bool wasTop = ! held.isEmpty() && held.getLast() == note;
    held.removeFirstMatchingValue(note);
    if (polyMode)                                                  // POLY: every release is its own note-off
    {
        if (onKeyUp) onKeyUp(note);
        return;
    }
    if (wasTop)
    {
        if (held.isEmpty()) { if (onKeyUp) onKeyUp(note); }
        else if (onKeyDown) onKeyDown(held.getLast(), lastVel);   // mono fall-back to the previous held key
    }
}

//==============================================================================
// SoundPad
//==============================================================================
static constexpr float kPadMargin = 0.14f; // inset of the shape inside the unit square

int SoundPad::activeCount() const
{
    int n = 0; for (bool a : active) if (a) ++n; return n;
}

juce::Point<float> SoundPad::toPixels(juce::Point<float> n) const
{
    // Inset the drawable square so the corner labels have a little room outside it.
    // Kept small so the blend shape fills most of the (now larger) pad.
    auto b = getLocalBounds().toFloat().reduced(20.0f, 15.0f);
    return { b.getX() + n.x * b.getWidth(), b.getY() + n.y * b.getHeight() };
}

void SoundPad::vertices(juce::Array<juce::Point<float>>& out) const
{
    out.clear();
    const float M = kPadMargin;
    switch (activeCount())
    {
        case 1: out.add({ 0.5f, 0.5f }); break;
        case 2: out.add({ M, 0.5f }); out.add({ 1 - M, 0.5f }); break;
        case 3: out.add({ 0.5f, M }); out.add({ M, 1 - M }); out.add({ 1 - M, 1 - M }); break;
        default: break;   // at most NUM_SLOTS (3) corners now
    }
}

void SoundPad::centroid(float& nx, float& ny) const
{
    if (activeCount() == 3) { nx = 0.5f; ny = (2.0f - kPadMargin) / 3.0f; }
    else                    { nx = 0.5f; ny = 0.5f; }
}

void SoundPad::setActiveMask(const bool a[NS])
{
    for (int i = 0; i < NS; ++i) active[i] = a[i];
    centroid(dotX, dotY);   // reposition the dot to the new shape's centre
    recompute();
}

void SoundPad::setDot(float nx, float ny)
{
    dotX = juce::jlimit(0.0f, 1.0f, nx);
    dotY = juce::jlimit(0.0f, 1.0f, ny);
    recompute();
}

void SoundPad::applyLayout(juce::Array<int>& act) const
{
    if (! layoutB) return;
    const int n = act.size();
    if (n == 4) act.swap(0, 1);                 // square: swap the two TOP corners
    else if (n == 5)                            // pentagon: pentagram (every-other) order
    { juce::Array<int> r { act[0], act[2], act[4], act[1], act[3] }; act = r; }
}

void SoundPad::recompute()
{
    for (int i = 0; i < NS; ++i) weights[i] = 0.0f;

    juce::Array<int> act;
    for (int i = 0; i < NS; ++i) if (active[i]) act.add(i);
    applyLayout(act);                           // A/B corner remap (square + pentagon)
    const int n = act.size();
    const float M = kPadMargin;

    if (n == 1) { weights[act[0]] = 1.0f; dotX = dotY = 0.5f; }
    else if (n == 2)
    {
        float t = juce::jlimit(0.0f, 1.0f, (dotX - M) / (1 - 2 * M));
        weights[act[0]] = 1.0f - t;
        weights[act[1]] = t;
        dotX = M + t * (1 - 2 * M); dotY = 0.5f;
    }
    else if (n == 3)
    {
        // Barycentric weights - exact 1/3 each at the centroid, any triangle.
        juce::Point<float> V0(0.5f, M), V1(M, 1 - M), V2(1 - M, 1 - M), P(dotX, dotY);
        float det = (V1.y - V2.y) * (V0.x - V2.x) + (V2.x - V1.x) * (V0.y - V2.y);
        float b0 = ((V1.y - V2.y) * (P.x - V2.x) + (V2.x - V1.x) * (P.y - V2.y)) / det;
        float b1 = ((V2.y - V0.y) * (P.x - V2.x) + (V0.x - V2.x) * (P.y - V2.y)) / det;
        float b2 = 1.0f - b0 - b1;
        b0 = juce::jmax(0.0f, b0); b1 = juce::jmax(0.0f, b1); b2 = juce::jmax(0.0f, b2);
        float s = b0 + b1 + b2; if (s <= 0.0f) { b0 = b1 = b2 = 1.0f / 3.0f; s = 1.0f; }
        b0 /= s; b1 /= s; b2 /= s;
        weights[act[0]] = b0; weights[act[1]] = b1; weights[act[2]] = b2;
        auto proj = V0 * b0 + V1 * b1 + V2 * b2; // keep the dot inside the triangle
        dotX = proj.x; dotY = proj.y;
    }

    if (onChange) onChange();
}

void SoundPad::mouseDown(const juce::MouseEvent& e) { mouseDrag(e); }

void SoundPad::mouseDrag(const juce::MouseEvent& e)
{
    auto b = getLocalBounds().toFloat().reduced(2.0f);
    if (b.getWidth() <= 0 || b.getHeight() <= 0) return;
    dotX = juce::jlimit(0.0f, 1.0f, (e.position.x - b.getX()) / b.getWidth());
    dotY = juce::jlimit(0.0f, 1.0f, (e.position.y - b.getY()) / b.getHeight());
    recompute();
    repaint();
}

void SoundPad::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);

    const int n = activeCount();
    if (n == 0)
    {
        g.setColour(juce::Colours::grey); g.setFont(juce::Font(10.0f));
        g.drawText("no sources", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    juce::Array<int> act; for (int i = 0; i < NS; ++i) if (active[i]) act.add(i);
    applyLayout(act);                          // corners follow the A/B layout
    juce::Array<juce::Point<float>> nv; vertices(nv);
    juce::Array<juce::Point<float>> pv; for (auto& p : nv) pv.add(toPixels(p));

    if (n >= 2)
    {
        juce::Path p; p.startNewSubPath(pv[0]);
        for (int i = 1; i < pv.size(); ++i) p.lineTo(pv[i]);
        if (n >= 3) p.closeSubPath();
        g.setColour(juce::Colour(0xff44557a));
        g.strokePath(p, juce::PathStrokeType(1.2f));
    }

    g.setFont(juce::Font(9.5f));
    const float cy = getLocalBounds().toFloat().getCentreY();
    const int W = getWidth();
    for (int i = 0; i < n; ++i)
    {
        const int src = act[i];
        juce::String txt = names[src] + " " + juce::String(juce::roundToInt(weights[src] * 100.0f)) + "%";
        // Just outside the corner: above for upper vertices, below for lower ones.
        const bool below = pv[i].y > cy + 1.0f;
        int ly = below ? (int) pv[i].y + 3 : (int) pv[i].y - 15;
        int lx = juce::jlimit(0, W - 66, (int) pv[i].x - 33);
        g.setColour(juce::Colour(0xffcfe0ff));
        g.drawText(txt, lx, ly, 66, 12, juce::Justification::centred, false);
    }

    if (n >= 2)
    {
        auto d = toPixels({ dotX, dotY });
        g.setColour(juce::Colour(0xffffcc33)); g.fillEllipse(d.x - 5, d.y - 5, 10, 10);
        g.setColour(juce::Colours::black);     g.drawEllipse(d.x - 5, d.y - 5, 10, 10, 1.0f);
    }
}

//==============================================================================
// WaveformDisplay
//==============================================================================
void WaveformDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(b, 3.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);

    auto in = b.reduced(2.0f);
    const float mid = in.getCentreY(), halfH = in.getHeight() * 0.5f, W = in.getWidth();

    if (pMax.empty())
    {
        g.setColour(juce::Colours::grey); g.setFont(juce::Font(9.0f));
        g.drawText("no sample", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    // Cached peaks, bucketed to the box width => fits any length.
    const int n = (int) pMax.size();
    g.setColour(juce::Colour(0xff5fb0ff));
    for (int px = 0; px < (int) W; ++px)
    {
        int bk = juce::jlimit(0, n - 1, (int)((int64_t) px * n / (int) W));
        float yMax = mid - juce::jlimit(-1.0f, 1.0f, pMax[(size_t) bk]) * halfH;
        float yMin = mid - juce::jlimit(-1.0f, 1.0f, pMin[(size_t) bk]) * halfH;
        g.drawVerticalLine((int) in.getX() + px, juce::jmin(yMax, yMin), juce::jmax(yMax, yMin) + 1.0f);
    }

    // Hand-drawn regions (Trim on): up to 4, each its own colour + play-order number. They can overlap.
    static const juce::Colour kRegCols[MAXREG] = { juce::Colour(0xff2ec46a), juce::Colour(0xffe7c33c),
                                                   juce::Colour(0xffff5fa6), juce::Colour(0xff35c0ff) };
    if (selEnabled)
        for (int i = 0; i < regN; ++i)
        {
            const float x0 = in.getX() + juce::jmin(regLo[i], regHi[i]) * W;
            const float x1 = in.getX() + juce::jmax(regLo[i], regHi[i]) * W;
            const juce::Colour c = kRegCols[i % MAXREG];
            g.setColour(c.withAlpha(0.22f)); g.fillRect(juce::Rectangle<float>(x0, in.getY(), x1 - x0, in.getHeight()));
            g.setColour(c); g.drawVerticalLine((int) x0, in.getY(), in.getBottom()); g.drawVerticalLine((int) x1, in.getY(), in.getBottom());
            g.setFont(juce::Font(9.5f, juce::Font::bold));
            g.drawText(juce::String(i + 1), (int) x0 + 2, (int) in.getY() + 1, 14, 12, juce::Justification::topLeft, false);
        }

    // Length watermark (bottom-right corner).
    if (lengthSec > 0.0f)
    {
        const juce::String t = lengthSec < 1.0f ? juce::String(juce::roundToInt(lengthSec * 1000.0f)) + " ms"
                                                : juce::String(lengthSec, 2) + " s";
        g.setColour(juce::Colour(0x99cfe0ff)); g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText(t, getLocalBounds().reduced(4), juce::Justification::bottomRight, false);
    }

    // Reverse indicator (top-left): a left-pointing arrow + "REV".
    if (reversed)
    {
        const float ax = in.getX() + 5.0f, ay = in.getY() + 7.0f;
        juce::Path arr; arr.addTriangle(ax, ay, ax + 7.0f, ay - 4.0f, ax + 7.0f, ay + 4.0f);  // ◀
        g.setColour(juce::Colour(0xffff7a4a)); g.fillPath(arr);
        g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText("REV", (int) ax + 10, (int) ay - 7, 30, 14, juce::Justification::centredLeft, false);
    }

    // LIVE playhead: the actual read position of the newest playing voice (not an animation).
    if (playhead >= 0.0f)
    {
        const float px = in.getX() + juce::jlimit(0.0f, 1.0f, playhead) * W;
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawVerticalLine((int) px, in.getY(), in.getBottom());
    }

    // Audio-file drop highlight.
    if (fileDragOver)
    {
        g.setColour(juce::Colour(0x332ec46a)); g.fillRoundedRectangle(b, 3.0f);
        g.setColour(juce::Colour(0xff2ec46a)); g.drawRoundedRectangle(b.reduced(1.0f), 3.0f, 2.0f);
    }
}

bool WaveformDisplay::isInterestedInFileDrag(const juce::StringArray& files)
{
    return onFileDropped != nullptr && files.size() > 0 && SlotEditor::isAudioFile(files[0]);
}

void WaveformDisplay::filesDropped(const juce::StringArray& files, int, int)
{
    fileDragOver = false; repaint();
    if (onFileDropped && files.size() > 0) onFileDropped(juce::File(files[0]));
}

void WaveformDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (!selEnabled) return;
    auto in = getLocalBounds().toFloat().reduced(2.0f);
    const float x = juce::jlimit(0.0f, 1.0f, (e.position.x - in.getX()) / in.getWidth());
    if (regN >= MAXREG) { dragIdx = -1; return; }   // already 4; double-click to clear them
    dragIdx = regN; dragAnchor = x; regLo[regN] = regHi[regN] = x; ++regN;   // start a new region
    repaint();
}

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (!selEnabled || dragIdx < 0) return;
    auto in = getLocalBounds().toFloat().reduced(2.0f);
    const float x = juce::jlimit(0.0f, 1.0f, (e.position.x - in.getX()) / in.getWidth());
    regLo[dragIdx] = juce::jmin(dragAnchor, x);
    regHi[dragIdx] = juce::jmax(dragAnchor, x);
    repaint();
}

void WaveformDisplay::mouseUp(const juce::MouseEvent&)
{
    if (dragIdx < 0) return;
    if (regHi[dragIdx] - regLo[dragIdx] < 0.01f) regN = juce::jmax(0, regN - 1);   // discard a tiny region
    dragIdx = -1;
    emitRegions();
}

void WaveformDisplay::mouseDoubleClick(const juce::MouseEvent&)
{
    if (!selEnabled) return;
    regN = 0; dragIdx = -1;   // clear all regions
    emitRegions(); repaint();
}

//==============================================================================
// DrumSequencerEditor
//==============================================================================

// Sound Bank menu category order (2026-07 taxonomy: INSTRUMENT roles - the old engine-named
// "Modal" category was dissolved into Bells/Percussion/Toms/Cymbals; "FX & Synth" split into
// Electro Perc + the three FX shelves). A category missing here is silently dropped!
static const char* kSoundCatOrder[] = { "Kicks", "Snares", "Claps", "Hi-Hats", "Cymbals", "Toms",
                                        "Percussion", "Electro Perc", "Bass", "Keys", "Pads & Choirs",
                                        "Leads", "Plucks & Strings", "Bells & Mallets", "Chords & Arps",
                                        "Risers & Falls", "Impacts & Booms", "Noise & Texture" };
// Factory indices in CATEGORY cat, alphabetical; a non-empty query keeps only matches
// (against the sound NAME or the category title - the search feature).
// SPACE-INSENSITIVE match: hosts (Reaper) often reserve the SPACE key for their transport, so
// the search field may never receive it - matching ignores spaces on BOTH sides ("subkick"
// finds "Sub Kick") and the search works whether or not the host lets the space through.
static bool pickerMatch(const juce::String& name, const juce::String& query)
{
    return name.removeCharacters(" ").containsIgnoreCase(query.removeCharacters(" "));
}

static juce::Array<int> factoryIndicesFor(const char* cat, const juce::StringArray& names,
                                          const juce::StringArray& cats, const juce::String& query)
{
    juce::Array<int> idx;
    for (int i = 0; i < names.size(); ++i)
        if (cats[i] == cat && (query.isEmpty() || pickerMatch(names[i], query)
                               || pickerMatch(cat, query)))
            idx.add(i);
    std::sort(idx.begin(), idx.end(),
              [&](int a, int b) { return names[a].compareIgnoreCase(names[b]) < 0; });
    return idx;
}

static constexpr int SAMPLE_ID_BASE   = 1000;
static constexpr int FACTORY_MIX_BASE = 5000;   // factory sound mixes: 5000 + index
static constexpr int FACTORY_PST_BASE = 6000;   // factory presets:     6000 + index
static constexpr int ID_INIT_PRESET   = 9003;
static constexpr int ID_INIT_MIX      = 9996;
static constexpr int ID_REFRESH_SAMPLES = 9990;  // rescan the Samples folder so newly-added files appear in the dropdown
static constexpr int ID_SHOW_SAMPLES    = 9989;  // reveal the Samples folder in Finder/Explorer
static constexpr int ID_REFRESH_BANK    = 9995;  // rescan the Sound Bank folder
static constexpr int ID_SHOW_BANK       = 9993;  // reveal the Sound Bank folder
static constexpr int ID_BROWSE        = 10001;   // open the sound-browser window (outside the sample-id range)


// THE Sound Bank dropdown (user spec, round-3: ONE dropdown, no extra pop-ups - a search field
// with a magnifier lives at its top and the list below filters LIVE while typing). Shown as a
// child of the editor content (like the Arp dropdown - no OS popup window, so the unfocused-menu
// watchdog can never kill it); clicking anywhere outside closes it. Rows carry the SAME ids the
// combo used, dispatched through DrumSequencerEditor::applySoundPickId.
class SoundPickerPanel : public juce::Component,
                         private juce::TextEditor::Listener, private juce::ListBoxModel
{
public:
    std::function<void(int id)> onPick;
    std::function<void()> onClosed;           // clears the opening combo's menuActive latch
    juce::Component* clickIgnore = nullptr;   // the combo that opened us (its click toggles)

    SoundPickerPanel()
    {
        addAndMakeVisible(ed);
        ed.setTextToShowWhenEmpty("Search names + categories...", juce::Colour(0xff7d86a0));
        ed.setFont(juce::Font(14.0f));
        ed.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff17172c));
        ed.addListener(this);
        addAndMakeVisible(list);
        list.setModel(this);
        list.setRowHeight(25);   // x1.25 with the fonts (user)
        list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff101020));
    }
    ~SoundPickerPanel() override { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); }

    std::function<void(const juce::String&)> onQueryChanged;   // editor remembers it per channel
    void openWith(const juce::Array<juce::File>& userFiles, const juce::File& userRoot, int currentId,
                  const juce::String& rememberedQuery)
    {
        files = userFiles; root = userRoot; curId = currentId;
        // The channel's last search survives open/close until the user clears it (user request),
        // and it re-opens SELECTED: typing replaces it, one Delete clears it (user request round-2).
        ed.setText(rememberedQuery, juce::dontSendNotification);
        rebuild();
        setVisible(true);
        toFront(false);
        ed.grabKeyboardFocus();
        ed.selectAll();                            // remembered query = pre-selected (type-over/one-key delete)
        scrollToCur();                             // open ON the channel's current sound
    }
    void close() { setVisible(false); giveAwayKeyboardFocus(); if (onClosed) onClosed(); }
    // Live re-highlight (MIDI knob browsing with the panel open follows along).
    void setCurrent(int id) { if (id != curId) { curId = id; scrollToCur(); list.repaint(); } }

    void resized() override
    {
        auto b = getLocalBounds().reduced(6);
        auto top = b.removeFromTop(24);
        top.removeFromLeft(24);              // the drawn magnifier's spot
        ed.setBounds(top);
        b.removeFromTop(5);
        list.setBounds(b);
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff181830));
        g.setColour(juce::Colour(0xff777fa8)); g.drawRect(getLocalBounds(), 1);
        g.setColour(juce::Colour(0xffaebada));                       // the magnifier glyph, drawn
        const float cx = 16.0f, cy = 17.0f;                          // (no unicode - mojibake rule)
        g.drawEllipse(cx - 9.0f, cy - 9.0f, 11.0f, 11.0f, 1.8f);
        g.drawLine(cx + 1.5f, cy + 1.5f, cx + 6.5f, cy + 6.5f, 2.2f);
    }
    void visibilityChanged() override
    {
        auto& d = juce::Desktop::getInstance();
        if (isVisible()) d.addGlobalMouseListener(&closer);
        else             d.removeGlobalMouseListener(&closer);
    }

private:
    struct Entry { juce::String text; int id = 0; bool action = false; };   // action = command row (tinted)
    struct Row   { juce::String header; Entry e[3]; int n = 0; bool isHeader = false; };   // 3 columns (user)
    juce::Array<Row> rows;
    juce::Array<juce::File> files;
    juce::File root;
    juce::TextEditor ed;
    juce::ListBox list;
    int curId = 0;                            // the channel's CURRENT sound (amber highlight; 0 = none)
    void scrollToCur()
    {
        if (curId == 0) return;
        for (int i = 0; i < rows.size(); ++i)
            if (! rows[i].isHeader)
                for (int k = 0; k < rows[i].n; ++k)
                    if (rows[i].e[k].id == curId) { list.scrollToEnsureRowIsOnscreen(i); return; }
    }

    struct Closer : juce::MouseListener   // click anywhere OUTSIDE = close, like a real dropdown
    {
        SoundPickerPanel& p; explicit Closer(SoundPickerPanel& pp) : p(pp) {}
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (! p.isVisible()) return;
            const auto sp = e.getScreenPosition();
            if (p.getScreenBounds().contains(sp)) return;
            if (p.clickIgnore != nullptr && p.clickIgnore->getScreenBounds().contains(sp)) return;
            p.close();
        }
    };
    Closer closer { *this };

    void addHeader(const juce::String& text) { Row r; r.header = text; r.isHeader = true; rows.add(r); }
    void addSection(const juce::Array<Entry>& items)   // pack 3 per row: the section FLOWS across
    {                                                  // the columns (user spec, round-2: 3 cols)
        for (int i = 0; i < items.size(); i += 3)
        {
            Row r;
            for (int k = 0; k < 3 && i + k < items.size(); ++k) { r.e[k] = items[i + k]; ++r.n; }
            rows.add(r);
        }
    }
    void rebuild()
    {
        rows.clear();
        const juce::String q = ed.getText().trim();
        auto names = Factory::mixNames();
        auto cats  = Factory::mixCategories();
        if (q.isEmpty())
        {
            juce::Array<Entry> init; init.add({ "Initialize new sound mix", ID_INIT_MIX, true });
            addSection(init);
        }
        for (auto* cat : kSoundCatOrder)
        {
            auto idx = factoryIndicesFor(cat, names, cats, q);
            if (idx.isEmpty()) continue;
            addHeader(cat);
            juce::Array<Entry> items;
            for (int i : idx)
                items.add({ names[i] + "  (" + Factory::mixSourceTag(i) + ")", FACTORY_MIX_BASE + i });
            addSection(items);
        }
        juce::Array<Entry> user;
        for (int i = 0; i < files.size(); ++i)
        {
            const bool inRoot = files[i].getParentDirectory() == root;
            const juce::String label = inRoot ? files[i].getFileNameWithoutExtension()
                : files[i].getParentDirectory().getFileName() + " / " + files[i].getFileNameWithoutExtension();
            if (q.isEmpty() || pickerMatch(label, q)) user.add({ label, i + 1 });
        }
        if (! user.isEmpty()) { addHeader("Your Sound Bank"); addSection(user); }
        if (q.isEmpty())
        {
            addHeader("");
            juce::Array<Entry> acts;
            acts.add({ "Refresh sound bank folder", ID_REFRESH_BANK, true });
            acts.add({ "Show Folder", ID_SHOW_BANK, true });
            addSection(acts);
        }
        else if (rows.isEmpty()) addHeader("(no sounds match)");
        list.updateContent();
        list.repaint();
    }
    void pick(const Entry& en)
    {
        if (en.id == 0) return;
        close();
        if (onPick) onPick(en.id);
    }
    // TextEditor::Listener
    void textEditorTextChanged(juce::TextEditor&) override
    { rebuild(); if (onQueryChanged) onQueryChanged(ed.getText()); }
    void textEditorReturnKeyPressed(juce::TextEditor&) override
    { for (const auto& r : rows) if (! r.isHeader && r.n > 0) { pick(r.e[0]); return; } }
    void textEditorEscapeKeyPressed(juce::TextEditor&) override { close(); }
    // ListBoxModel
    int getNumRows() override { return rows.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool over) override
    {
        if (row < 0 || row >= rows.size()) return;
        const auto& r = rows[row];
        if (r.isHeader)
        {
            g.setColour(juce::Colour(0xffcf9a2a));
            g.setFont(juce::Font(19.0f, juce::Font::bold));   // titles BIGGER than the items (user)
            g.drawFittedText(r.header, 4, 0, w - 8, h, juce::Justification::centredLeft, 1, 0.8f);
            return;
        }
        const int colW = juce::jmax(1, w / 3);
        int mx = -1;   // hover: highlight only the column under the cursor
        if (over) mx = list.getLocalPoint(nullptr, juce::Desktop::getInstance()
                                                       .getMainMouseSource().getScreenPosition().toInt()).x;
        for (int k = 0; k < r.n; ++k)
        {
            const auto& en = r.e[k];
            const int x0 = k * colW;
            const bool hov = over && mx >= x0 && mx < x0 + colW;
            if (en.action)   // command rows (Initialize / Refresh / Show Folder) - tinted so they
            {                // never read as sounds
                g.setColour(juce::Colour(hov ? 0xff2e4468 : 0xff223046));
                g.fillRoundedRectangle((float) x0 + 2.0f, 1.0f, (float) colW - 6.0f, (float) h - 2.0f, 4.0f);
                g.setColour(juce::Colour(0xff9fd1ff));
            }
            else
            {
                const bool sel = curId != 0 && en.id == curId;   // the channel's CURRENT sound
                if (hov) { g.setColour(juce::Colour(0xff2a2a4a)); g.fillRect(x0, 0, colW, h); }
                if (sel)
                {
                    g.setColour(juce::Colour(0xff3a3320));       // amber wash + outline (brand gold)
                    g.fillRoundedRectangle((float) x0 + 2.0f, 1.0f, (float) colW - 6.0f, (float) h - 2.0f, 4.0f);
                    g.setColour(juce::Colour(0xffcf9a2a));
                    g.drawRoundedRectangle((float) x0 + 2.5f, 1.5f, (float) colW - 7.0f, (float) h - 3.0f, 4.0f, 1.4f);
                }
                g.setColour(sel ? juce::Colour(0xffffd98a) : juce::Colours::white);
                if (sel) { g.setFont(juce::Font(17.0f, juce::Font::bold));
                           g.drawFittedText(en.text, x0 + 12, 0, colW - 16, h, juce::Justification::centredLeft, 1, 0.75f);
                           continue; }
            }
            g.setFont(juce::Font(17.0f, en.action ? juce::Font::bold : juce::Font::plain));   // x1.25 (user)
            g.drawFittedText(en.text, x0 + 12, 0, colW - 16, h, juce::Justification::centredLeft, 1, 0.75f);
        }
    }
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override
    {
        if (row < 0 || row >= rows.size() || rows[row].isHeader) return;
        const auto& r = rows[row];
        const int colW = juce::jmax(1, list.getVisibleRowWidth() / 3);
        const int k = juce::jlimit(0, 2, e.x / colW);
        if (k < r.n) pick(r.e[k]);
    }
};

// On-brand extension for a saved per-pattern note grid (the "MIDI pattern" menu).
static const juce::String kPatternExt  = "basamakpattern";
static const juce::String kPatternWild = "*.basamakpattern";

// On-brand file extensions. We WRITE the new ones and READ both (so older .davulmix/.drumseq files still load).
static const juce::String kSoundExt   = "basamaksound";                 // saved channel sound (Sound Bank)
static const juce::String kSoundWild  = "*.basamaksound;*.davulmix";    // + legacy
static const juce::String kPresetExt  = "basamakpreset";                // saved whole-instrument preset
static const juce::String kPresetWild = "*.basamakpreset;*.drumseq";    // + legacy

DrumSequencerEditor::DrumSequencerEditor(DrumSequencerProcessor& p)
    : AudioProcessorEditor(&p), proc(p),
      btnDawSync("DAW Sync", "global_dawsync", p.midiLearn),
      btnPlay   ("Play",     "global_play",    p.midiLearn),
      btnStop   ("Stop",     "global_stop",    p.midiLearn)
{
    addAndMakeVisible(content);

    // A "Zoom" button beside each group title (row 1 + row 2). Each lifts only its
    // group's controls into the floating zoom panel.
    {
        const juce::Component* hdrs[NUM_ZOOM] = {
            &hdrSounds, &hdrSamplerG, &hdrOscG, &hdrNoiseG, &hdrFmG, &hdrPhysG,
            &hdrEq, &hdrSend, &hdrFilter, &hdrChan, &hdrMasterFX, &hdrMasterOut,
            &hdrAmpEnv, &hdrPitch, &hdrModulation };   // amp/eq + pitch + MODULATION columns
        for (int i = 0; i < NUM_ZOOM; ++i)
        {
            content.addAndMakeVisible(zoomBtns[i]);
            zoomBtns[i].setTooltip("Pop just this group out bigger for easier tweaking (the rest stays as-is). Click outside it or Close to return.");
            auto* h = hdrs[i];
            zoomBtns[i].onClick = [this, h, i] {
                auto b = h->getBounds();
                const int bh = zoomBoxH[i] > 0 ? zoomBoxH[i] : b.getHeight();
                zoomToGroup({ b.getX() - 5, b.getY() - 3, b.getWidth() + 10, bh });
            };
        }
    }
    addChildComponent(zoomCatcher);    // transparent; a click outside the panel closes
    zoomCatcher.onClick = [this] { unzoom(); };
    addChildComponent(zoomPanel);
    addChildComponent(zoomCloseBtn);
    zoomCloseBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff35c0ff));
    zoomCloseBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    zoomCloseBtn.onClick = [this] { unzoom(); };

    categoryList = DrumSoundGenerator::categories();

    setupComponents();
    for (auto& fo : srcFade) content.addAndMakeVisible(fo);  // above the knobs (dims off sources)
    for (auto& b : zoomBtns) b.toFront(false);   // keep the zoom "+" clickable on top
    rescanSamples();
    rescanSoundMixes();
    rebuildPresetMenu();
    rebuildSampleMenu();
    for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch)
        rebuildSoundMixMenu(ch);

    selectPattern(proc.sequencer.currentPattern);
    selectChannel(juce::jlimit(0, Sequencer::NUM_CHANNELS - 1, proc.lastSelectedChannel));
    rebaselinePreset(juce::String());  // baseline edit-tracking against the loaded state

    visibleChannels = (proc.visibleChannels <= 8) ? 8 : 16;   // only 8 or 16 now
    contentHeightPx = contentHeightFor(visibleChannels, detailShown);
    firstChannelRow = 0;
    stepGrid.visibleRows = viewRows();
    stepGrid.firstRow    = 0;
    visiblePatterns = Sequencer::NUM_PATTERNS;   // always 32
    firstPatternCol = 0;
    refreshCountButtons();

    setResizable(true, true);
    setResizeLimits(DESIGN_W / 2, contentHeightPx / 2, DESIGN_W * 2, contentHeightPx * 2);

    // Open at a size that fits the user's screen (the content scales to fit), so
    // the whole plugin is visible on first open instead of being cut off.
    double scale = 1.0;
    if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        auto area = disp->userArea;
        scale = juce::jmin(1.0, (area.getWidth()  * 0.96) / (double) DESIGN_W,
                                (area.getHeight() * 0.90) / (double) contentHeightPx);
    }
    setSize(juce::roundToInt(DESIGN_W * scale), juce::roundToInt(contentHeightPx * scale));
    startTimerHz(60);   // smooth playhead motion (the grid/meters update every tick; heavy hashing is throttled below)
    proc.midiLearn.addListener(this);
}

DrumSequencerEditor::~DrumSequencerEditor()
{
    proc.midiLearn.removeListener(this);
    stopTimer();
    comboSampleSel.setLookAndFeel(nullptr);
    for (auto& c : slotCombo) c.setLookAndFeel(nullptr);
    for (auto& c : comboChFx) c.setLookAndFeel(nullptr);   // TinyComboLNF
    swDelaySync.setLookAndFeel(nullptr); swDelayPingPong.setLookAndFeel(nullptr);   // [2026-07-15] lit buttons (tinyBtnLNF)
    swReverbSync.setLookAndFeel(nullptr);
    for (auto* k : allKnobs) k->setLookAndFeel(nullptr);
    blendFader.setLookAndFeel(nullptr);   // had a custom two-tone LNF
    patModeBtn.setLookAndFeel(nullptr);
    for (juce::Button* b : { (juce::Button*)&btnPlay, (juce::Button*)&btnStop, (juce::Button*)&btnUndo,
                             (juce::Button*)&btnRedo, (juce::Button*)&btnRoute, (juce::Button*)&btnSaveMix,
                             (juce::Button*)&btnToggleDetail, (juce::Button*)&btnKeysView, (juce::Button*)&btnClearPat,
                             (juce::Button*)&btnInfluenceTop,
                             (juce::Button*)&btnTooltips,
                             (juce::Button*)&btn16View }) b->setLookAndFeel(nullptr);
    keysPanel.btnSlot2.setLookAndFeel(nullptr);   // dropBtnLNF
    keysPanel.btnArp.setLookAndFeel(nullptr);     // dropBtnLNF
    keysPanel.btnGuide.setLookAndFeel(nullptr);   // dropBtnLNF
    tooltipWindow.setLookAndFeel(nullptr);        // tipLNF
    keysPanel.btnTakes.setLookAndFeel(nullptr);   // dropBtnLNF
    for (auto& s : strips)
    {
        s.btnPoly.setLookAndFeel(nullptr);
        s.numBtn.setLookAndFeel(nullptr);
        s.comboSound.setLookAndFeel(nullptr);
        if (s.btnMute)  s.btnMute->setLookAndFeel(nullptr);
        if (s.btnSolo)  s.btnSolo->setLookAndFeel(nullptr);
    }
}

//==============================================================================
// Samples / sounds
//==============================================================================

juce::File DrumSequencerEditor::getSamplesFolder()
{
    return UserPaths::samples();
}

static const juce::String kAudioWildcard = "*.wav;*.aiff;*.aif;*.mp3;*.flac;*.ogg";

void DrumSequencerEditor::rescanSamples()
{
    // Fresh, recursive scan of real audio files only (subfolders included).
    sampleFiles.clear();
    auto files = getSamplesFolder().findChildFiles(juce::File::findFiles, true, kAudioWildcard);
    files.sort();
    for (auto& f : files) sampleFiles.add(f);
}

// Recursively build the sample menu mirroring the folder tree (subfolders ->
// submenus). Each file's id is its index in the flat sampleFiles array.
void DrumSequencerEditor::addFolderToMenu(juce::PopupMenu& menu, const juce::File& folder)
{
    auto dirs = folder.findChildFiles(juce::File::findDirectories, false);
    dirs.sort();
    for (auto& d : dirs)
    {
        juce::PopupMenu sub;
        addFolderToMenu(sub, d);
        if (sub.getNumItems() > 0) menu.addSubMenu(d.getFileName(), sub);
    }
    auto files = folder.findChildFiles(juce::File::findFiles, false, kAudioWildcard);
    files.sort();
    for (auto& f : files)
    {
        int idx = sampleFiles.indexOf(f);
        if (idx >= 0) menu.addItem(SAMPLE_ID_BASE + idx, f.getFileNameWithoutExtension());
    }
}

juce::Array<DrumSoundGenerator::Type>
DrumSequencerEditor::sortedVariants(const juce::String& category) const
{
    auto v = DrumSoundGenerator::variantsIn(category);
    std::sort(v.begin(), v.end(), [](DrumSoundGenerator::Type a, DrumSoundGenerator::Type b) {
        return DrumSoundGenerator::variantOf(a).compareIgnoreCase(DrumSoundGenerator::variantOf(b)) < 0;
    });
    return v;
}

// e.g. "808 Kick", "Closed 808 Hi-Hat", "Cowbell"
juce::String DrumSequencerEditor::soundDisplayName(DrumSoundGenerator::Type t)
{
    auto cat = DrumSoundGenerator::categoryOf(t);
    auto var = DrumSoundGenerator::variantOf(t);
    if (cat == "Percussion") return var;
    return var + " " + cat;
}

// Sample chooser combo (in the Sounds section, acts on the selected channel).
void DrumSequencerEditor::rebuildSampleMenu()
{
    comboSampleSel.clear(juce::dontSendNotification);
    auto* root = comboSampleSel.getRootMenu();
    root->clear();

    addFolderToMenu(*root, getSamplesFolder()); // only what's actually in the folder

    root->addSeparator();
    root->addItem(ID_REFRESH_SAMPLES, "Refresh samples folder");   // rescan so newly-added files show up
    root->addItem(ID_SHOW_SAMPLES,    "Show Folder");              // open the Samples folder to drop files in

    refreshSampleSel();
    rebuildSlotMenus();   // the slot dropdowns embed the same sample list as a submenu
}

void DrumSequencerEditor::refreshSampleSel()
{
    auto& dch = proc.sequencer.channel(selectedChannel);
    const auto& ss = dch.slotSample[envTargetSlot()];   // the selected slot's sample
    if (ss.usingUser)
    {
        int sel = -1;
        for (int i = 0; i < sampleFiles.size(); ++i)
            if (sampleFiles[i] == ss.file) { sel = SAMPLE_ID_BASE + i; break; }
        if (sel > 0) comboSampleSel.setSelectedId(sel, juce::dontSendNotification);
        else { comboSampleSel.setSelectedId(0, juce::dontSendNotification);
               comboSampleSel.setTextWhenNothingSelected(ss.file.getFileNameWithoutExtension()); }
    }
    else
    {
        comboSampleSel.setSelectedId(0, juce::dontSendNotification);
        comboSampleSel.setTextWhenNothingSelected("(no sample)");
    }
}

void DrumSequencerEditor::handleSampleSelChange()
{
    int id = comboSampleSel.getSelectedId();
    auto& dch = proc.sequencer.channel(selectedChannel);

    if (id >= SAMPLE_ID_BASE && id < ID_INIT_MIX)
    {
        int idx = id - SAMPLE_ID_BASE;
        if (idx >= 0 && idx < sampleFiles.size())
        {
            dch.slots[envTargetSlot()].engine = DrumChannel::SrcSample;
            dch.loadUserSample(envTargetSlot(), sampleFiles[idx]);
            cacheWaveform(selectedChannel);
        }
    }
    else if (id == ID_REFRESH_SAMPLES)   // re-scan the folder so files added on disk appear in the list
    {
        rescanSamples();
        rebuildSampleMenu();
    }
    else if (id == ID_SHOW_SAMPLES)      // open the Samples folder so the user can drop files in
    {
        getSamplesFolder().revealToUser();
    }
}

void DrumSequencerEditor::cacheWaveform(int ch)
{
    auto& dch = proc.sequencer.channel(ch);
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)   // each slot shows its OWN sample
    {
        std::vector<float> mn, mx;
        dch.getWaveformPeaks(b, 480, mn, mx);
        waveform[b].setPeaks(mn, mx);
    }
    if (ch == selectedChannel) updateSampleLengthLabel();
}

void DrumSequencerEditor::updateSampleLengthLabel()
{
    auto& ch = proc.sequencer.channel(selectedChannel);
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    {
        const auto& sl = ch.slots[b];
        const int frames = ch.getSampleNumFrames(b);
        if (frames <= 0) { waveform[b].setLength(0.0f); continue; }
        double sr = ch.getSampleFileRate(); if (sr <= 0.0) sr = 44100.0;
        const double speed = juce::jmax(0.05, (double) sl.smpSpeed);  // pitch is baked (length-preserving); buffer already stretched
        const double secs  = frames / sr / speed;              // whole-sample length watermark
        waveform[b].setLength((float) secs);
    }
}

void DrumSequencerEditor::updateBarLength()
{
    const double bpm = proc.currentBpm > 1.0 ? proc.currentBpm : 120.0;
    const int num = proc.currentTimeSigNum, den = juce::jmax(1, proc.currentTimeSigDen);
    const double secs = (60.0 / bpm) * (double) num * (4.0 / (double) den);
    lblBarResult.setText("Bar: " + juce::String(secs, 2) + " s", juce::dontSendNotification);
}

//==============================================================================
// Sound mixes - per-channel full presets (the channel strip dropdown)
//==============================================================================
juce::File DrumSequencerEditor::getSoundMixFolder()
{
    return UserPaths::soundMixes();
}

void DrumSequencerEditor::rescanSoundMixes()
{
    soundMixFiles.clear();
    // Recursive so subfolders are picked up; read the new extension AND the legacy one.
    auto files = getSoundMixFolder().findChildFiles(juce::File::findFiles, true, kSoundWild);
    files.sort();
    for (auto& f : files) soundMixFiles.add(f);
}

// Mirror the Sound Bank folder structure into the menu: subfolders -> submenus, files -> items
// (id = index into soundMixFiles + 1). Parallels addFolderToMenu for samples.
void DrumSequencerEditor::addSoundFolderToMenu(juce::PopupMenu& menu, const juce::File& folder)
{
    auto dirs = folder.findChildFiles(juce::File::findDirectories, false);
    dirs.sort();
    for (auto& d : dirs)
    {
        juce::PopupMenu sub;
        addSoundFolderToMenu(sub, d);
        if (sub.getNumItems() > 0) menu.addSubMenu(d.getFileName(), sub);
    }
    auto files = folder.findChildFiles(juce::File::findFiles, false, kSoundWild);
    files.sort();
    for (auto& f : files) { int idx = soundMixFiles.indexOf(f); if (idx >= 0) menu.addItem(idx + 1, f.getFileNameWithoutExtension()); }
}

void DrumSequencerEditor::rebuildSoundMixMenu(int ch)
{
    auto& combo = strips[ch].comboSound;
    const int keep = combo.getSelectedId();
    combo.clear(juce::dontSendNotification);
    combo.setTextWhenNothingSelected("Sound Bank");
    auto* root = combo.getRootMenu();
    root->clear();
    root->addItem(ID_INIT_MIX, "Initialize new sound mix");
    // Built-in factory sounds (read-only). Categories are SECTION HEADERS in one
    // flat list (no nested submenus); when the list is taller than the screen JUCE
    // automatically flows it into extra columns to the side. ALPHABETICAL inside
    // each category (the table itself is grouped by authoring batch, not by name).
    auto facNames = Factory::mixNames();
    auto facCats  = Factory::mixCategories();
    for (auto* cat : kSoundCatOrder)
    {
        auto idx = factoryIndicesFor(cat, facNames, facCats, {});
        if (idx.isEmpty()) continue;
        root->addSectionHeader(cat);
        for (int i : idx)
            root->addItem(FACTORY_MIX_BASE + i, facNames[i] + "  (" + Factory::mixSourceTag(i) + ")");
    }
    // The user's own saved sounds (Your Sound Bank), mirroring any subfolders as submenus.
    root->addSectionHeader("Your Sound Bank");
    if (soundMixFiles.isEmpty())
        root->addItem(-1, "(none saved yet)", false, false);
    else
        addSoundFolderToMenu(*root, getSoundMixFolder());
    root->addSeparator();
    root->addItem(ID_REFRESH_BANK, "Refresh sound bank folder");   // rescan so newly-saved sounds show up
    root->addItem(ID_SHOW_BANK,    "Show Folder");                 // open the Sound Bank folder
    juce::ignoreUnused(keep);
    // clear() above reset the combo's displayed text, so invalidate the cache to
    // force updateStripMixLabel to re-apply this channel's mix name.
    stripMixShown[ch] = juce::String();
    // Show this pattern/channel's own selected mix name (+ * if edited).
    updateStripMixLabel(ch);
}

void DrumSequencerEditor::handleSoundMixChange(int ch)
{
    applySoundPickId(ch, strips[ch].comboSound.getSelectedId());
}

void DrumSequencerEditor::openSoundPicker(int ch)
{
    if (soundPicker == nullptr)
    {
        soundPicker = std::make_unique<SoundPickerPanel>();
        content.addChildComponent(*soundPicker);
    }
    auto& panel = static_cast<SoundPickerPanel&>(*soundPicker);
    auto& combo = strips[ch].comboSound;
    if (panel.isVisible() && panel.clickIgnore == &combo) { panel.close(); return; }   // toggle
    if (panel.isVisible()) panel.close();          // switching combos: release the OLD combo's latch
    panel.clickIgnore = &combo;
    panel.onClosed = [&combo] { combo.hidePopup(); };   // free the menuActive latch for the next click
    panel.onPick = [this, ch](int id) { applySoundPickId(ch, id); selectChannel(ch); };
    panel.onQueryChanged = [this, ch](const juce::String& q) { pickerQuery[ch] = q; };
    const auto r = content.getLocalArea(&combo, combo.getLocalBounds());
    int y = r.getBottom() + 2;
    int h = content.getHeight() - y - 6;
    if (h < 300) { h = juce::jmin(620, content.getHeight() - 12); y = content.getHeight() - h - 6; }
    h = juce::jmin(h, 620);
    panel.setBounds(juce::jlimit(2, juce::jmax(2, content.getWidth() - 902), r.getX()), y, 900, h);   // 3 columns
    panel.openWith(soundMixFiles, getSoundMixFolder(), currentSoundPickId(ch), pickerQuery[ch]);   // highlight + remembered search
}

void DrumSequencerEditor::applySoundPickId(int ch, int id)
{
    auto& c = proc.sequencer.channel(ch);
    // [2026-07-13 22:40] CHANNEL VOLUME = the USER'S mixer handle: factory builders bake their own
    // loudness (c.volume) for kit balance, which made the strip handle JUMP on every pick (user:
    // "why is it deciding itself"). Sound picks now PRESERVE the channel volume; presets (whole
    // kits) still set it - kit balance is a preset-level decision.
    const float keepVol = c.volume;
    const bool mutating = id == ID_INIT_MIX
                       || (id >= FACTORY_MIX_BASE && id < FACTORY_MIX_BASE + Factory::mixNames().size())
                       || (id >= 1 && id <= soundMixFiles.size());
    // [2026-07-15 22:10] FADE any ringing/stuck voice BEFORE the slots change under it: a live
    // voice rendering brand-new slot data (new engine, stale phases) screamed continuously when
    // a sound was picked mid-playback (user report). Pitch-aware fade = clickless.
    if (mutating) c.stopVoicesFaded();
    if (id == ID_REFRESH_BANK)      // re-scan so sounds saved on disk appear in every channel's dropdown
    {
        rescanSoundMixes();
        for (int k = 0; k < Sequencer::NUM_CHANNELS; ++k) rebuildSoundMixMenu(k);
    }
    else if (id == ID_SHOW_BANK)    // open the Sound Bank folder
    {
        getSoundMixFolder().revealToUser();
        strips[ch].comboSound.setSelectedId(0, juce::dontSendNotification);
    }
    else if (id == ID_INIT_MIX)
    {
        initChannelMix(ch);                       // clears mixName, rebuilds the menu/label
    }
    else if (id >= FACTORY_MIX_BASE && id < FACTORY_MIX_BASE + Factory::mixNames().size())
    {
        const int fi = id - FACTORY_MIX_BASE;
        Factory::applyMix(c, fi);
        c.markDspDirty();
        c.mixName = Factory::mixNames()[fi]; c.mixModified = false; c.mixHash = channelSoundHash(c);
        if (ch == selectedChannel) { refreshDetailPanel(); refreshSampleSel(); updateVisuals(); }
        updateStripMixLabel(ch);
    }
    else if (id >= 1 && id <= soundMixFiles.size())
    {
        loadSoundMix(ch, soundMixFiles[id - 1]);   // restores slots (3-slot or legacy)
        c.mixName = soundMixFiles[id - 1].getFileNameWithoutExtension();
        c.mixModified = false; c.mixHash = channelSoundHash(c);
        updateStripMixLabel(ch);
    }
    c.volume = keepVol;   // the user's mixer level survives every pick (incl. Initialize)
    // [2026-07-15 22:10] FULL slot-box resync on every pick (user: "the sound slot doesn't
    // update... especially via MIDI next/prev"). refreshDetailPanel alone never re-synced the
    // boxEngine[] mirror or re-placed the boxes (the drag-copy round-2 disease) - only
    // selectChannel did, which is why combo picks "sometimes" looked right and MIDI browsing
    // (no channel switch, no picker close) almost never did.
    if (mutating && ch == selectedChannel)
    {
        syncBoxesFromSrcOn();                          // boxEngine[] <- the new slots' engines
        keepPickerOnLayout = true;                     // browsing must not close the picker
        layoutContent();                               // re-place the slot boxes for the new engines
        keepPickerOnLayout = false;
        refreshDetailPanel(); refreshSampleSel();
        cacheWaveform(ch); syncPadFromSlots(false);
        updateVisuals();
    }
    // A visible picker for this channel follows the pick (MIDI knob browsing highlights live).
    if (soundPicker != nullptr && soundPicker->isVisible())
    {
        auto& pp = static_cast<SoundPickerPanel&>(*soundPicker);
        if (pp.clickIgnore == &strips[ch].comboSound) pp.setCurrent(currentSoundPickId(ch));
    }
}

//==============================================================================
// Cheap rolling hashes used to detect "modified since saved" for the * marker.
juce::int64 DrumSequencerEditor::channelSoundHash(const DrumChannel& c) const
{
    auto mix = [](juce::int64 h, juce::int64 v) { return h * 1000003LL ^ v; };
    auto f   = [](float x) { juce::int64 b = 0; std::memcpy(&b, &x, sizeof(float)); return b; };
    juce::int64 h = 146959810934665603LL;
    h = mix(h, c.keysSlot2Down);   // slot-2 pitch is part of the SOUND now (rides + refreshes with mixes)
    h = mix(h, f(c.strumAmt));     // STRUM is per-sound too (user order: guitar sounds ship strummed)
    // CHANNEL FX (both slots combined) are part of the SOUND: chorus / flanger / phaser / comp.
    for (int fx = 0; fx < 3; ++fx)
    { h = mix(h, c.chFxType[fx]); h = mix(h, f(c.chFxAmt[fx])); h = mix(h, f(c.chFxChar[fx])); }
    h = mix(h, f(c.reverbSend)); h = mix(h, f(c.delaySend));   // channel sends are part of the SOUND
    h = mix(h, c.keysPolyMode ? 1 : 0);   // keys Poly/Mono is per-sound too
    h = mix(h, c.keysLegato ? 1 : 0);     // [2026-07-16] mode dropdown's legato/glide axis (per-sound)
    // ARP is per-sound now (user 2026-07-11): editing it marks the sound modified like any param.
    h = mix(h, c.arpOn ? 1 : 0); h = mix(h, c.arpLen); h = mix(h, c.arpRate); h = mix(h, c.arpSync);
    h = mix(h, f(c.arpGate)); h = mix(h, c.arpAlign ? 1 : 0); h = mix(h, c.arpHold ? 1 : 0);
    h = mix(h, c.arpAltStrum ? 1 : 0);
    for (int i = 0; i < DrumChannel::ARP_ROWS; ++i) h = mix(h, c.arpOffset[i]);
    for (int i = 0; i < DrumChannel::NUM_SOURCES; ++i) { h = mix(h, c.srcOn[i] ? 1 : 0); h = mix(h, f(c.srcWeight[i])); }
    h = mix(h, f(c.padX)); h = mix(h, f(c.padY)); h = mix(h, c.padLayoutB ? 1 : 0);
    // Slots are the runtime source of truth (incl. duplicate engines) - hash them too.
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) {
        const auto& sl = c.slots[b];
        h = mix(h, sl.engine); h = mix(h, f(sl.weight));
        h = mix(h, f(sl.atk)); h = mix(h, f(sl.hold)); h = mix(h, f(sl.dec)); h = mix(h, f(sl.sustain)); h = mix(h, f(sl.release)); h = mix(h, f(sl.vibrato));
        h = mix(h, sl.oscShape); h = mix(h, sl.oscShapeB); h = mix(h, f(sl.oscFreq)); h = mix(h, f(sl.oscPEnvAmt)); h = mix(h, f(sl.oscPEnvTime)); h = mix(h, f(sl.oscPOffset));
        h = mix(h, sl.oscUnison); h = mix(h, f(sl.oscDetune)); h = mix(h, f(sl.uniSpread)); h = mix(h, sl.oscUniCenter ? 1 : 0); h = mix(h, sl.oscDetuneMode); h = mix(h, sl.chordMode); h = mix(h, sl.chordUnison);
        h = mix(h, sl.scaleOn ? 1 : 0); h = mix(h, sl.scaleType); h = mix(h, sl.scaleUnison); h = mix(h, sl.scaleKey);   // SCALE mode
        h = mix(h, sl.fxDriveType); h = mix(h, f(sl.fxDrive));   // (slot sends retired - the channel sends are hashed above)
        h = mix(h, sl.noiseType); h = mix(h, f(sl.noiseCenter)); h = mix(h, f(sl.noiseWidth)); h = mix(h, f(sl.noiseRes)); h = mix(h, f(sl.noiseDrive)); h = mix(h, f(sl.noiseCrackle));
        h = mix(h, f(sl.fmPitch)); h = mix(h, f(sl.fmSpread)); h = mix(h, f(sl.fmDepth)); h = mix(h, f(sl.fmPEnvAmt)); h = mix(h, f(sl.fmPEnvTime)); h = mix(h, f(sl.fmPOffset)); h = mix(h, f(sl.fmFeedback)); h = mix(h, f(sl.fmSub));
        h = mix(h, f(sl.physFreq)); h = mix(h, f(sl.physTone)); h = mix(h, f(sl.physMaterial)); h = mix(h, f(sl.physPosition)); h = mix(h, f(sl.physPEnvAmt)); h = mix(h, f(sl.physPEnvTime)); h = mix(h, f(sl.physPOffset)); h = mix(h, f(sl.physStiff)); h = mix(h, sl.physExcite); h = mix(h, f(sl.grainPos)); h = mix(h, f(sl.grainSize)); h = mix(h, f(sl.grainDens)); h = mix(h, f(sl.grainSpray)); h = mix(h, f(sl.grainPitch));
        h = mix(h, f(sl.smpSpeed)); h = mix(h, f(sl.smpCrush)); h = mix(h, f(sl.smpPitch)); h = mix(h, f(sl.smpPEnvAmt)); h = mix(h, f(sl.smpPEnvTime)); h = mix(h, f(sl.smpPOffset)); h = mix(h, sl.smpReverse ? 1 : 0); h = mix(h, sl.smpUseRegion ? 1 : 0);
        h = mix(h, f(sl.smpStart)); h = mix(h, f(sl.smpEnd)); h = mix(h, sl.smpSlices); h = mix(h, f(sl.smpStretch)); h = mix(h, f(sl.smpGain));
        h = mix(h, sl.smpEnvOn ? 1 : 0); h = mix(h, sl.smpPreservePitch ? 1 : 0); h = mix(h, sl.fmEnvFollow ? 1 : 0); h = mix(h, f(sl.modalMorph));
        h = mix(h, sl.smpRegN); for (int r = 0; r < DrumChannel::Slot::MAXREG; ++r) { h = mix(h, f(sl.smpRegLo[r])); h = mix(h, f(sl.smpRegHi[r])); }
        h = mix(h, (juce::int64) c.slotSample[b].file.getFullPathName().hashCode64());   // this slot's sample
        h = mix(h, f(sl.oscFold)); h = mix(h, f(sl.oscLevel)); h = mix(h, f(sl.noiseLevel));
        h = mix(h, sl.waveTable); h = mix(h, f(sl.wavePos)); h = mix(h, f(sl.oscWarp));
        h = mix(h, sl.modalMaterial); h = mix(h, f(sl.modalDecay)); h = mix(h, f(sl.modalTone)); h = mix(h, f(sl.modalStruct)); h = mix(h, f(sl.modalHit)); h = mix(h, f(sl.modalDamp));
        for (int k = 0; k < DrumChannel::Slot::NPE; ++k) { h = mix(h, f(sl.pEnvP[k])); h = mix(h, f(sl.pEnvT[k])); }
        for (int e = 0; e < DrumChannel::NUM_EQ_BANDS; ++e) { const auto& eb = sl.eqBand[e]; h = mix(h, eb.on ? 1 : 0); h = mix(h, f(eb.freq)); h = mix(h, f(eb.gainDb)); h = mix(h, f(eb.q)); }
        h = mix(h, sl.filterType); h = mix(h, f(sl.filterCutoff)); h = mix(h, f(sl.filterReso)); h = mix(h, f(sl.filterEnvAmt));   // per-slot filter 1
        h = mix(h, sl.filterType2); h = mix(h, f(sl.filterCutoff2)); h = mix(h, f(sl.filterReso2)); h = mix(h, f(sl.filterEnvAmt2));   // filter 2
        h = mix(h, f(sl.filterGain)); h = mix(h, f(sl.filterGain2));   // BELL bipolar gains
        h = mix(h, f(sl.fxPunch)); h = mix(h, f(sl.fxRing)); h = mix(h, f(sl.fxRingHz)); h = mix(h, f(sl.pan));   // Punch/Ring/RingHz/Pan (per slot)
        h = mix(h, f(sl.fxSub)); h = mix(h, f(sl.fxFormant));                      // Sub/Formant (per slot)
        for (int fr = 0; fr < DrumChannel::ADD_FRAMES; ++fr)
            for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
            { h = mix(h, f(sl.addH[fr][k])); h = mix(h, f(sl.addPh[fr][k])); }   // wavetable frames
        for (int sg = 0; sg < DrumChannel::ADD_FRAMES - 1; ++sg) h = mix(h, f(sl.addSeg[sg]));
        h = mix(h, sl.addLoop ? 1 : 0); h = mix(h, f(sl.addPos));
        for (int d2 = 0; d2 < 4; ++d2) { h = mix(h, f(sl.lfoRate[d2])); h = mix(h, f(sl.lfoAmt[d2])); h = mix(h, f(sl.lfoSync[d2]));
                                         h = mix(h, sl.lfoShape[d2]); h = mix(h, sl.lfoFree[d2] ? 1 : 0); h = mix(h, sl.lfoLegato[d2] ? 1 : 0);
                                         if (sl.lfoShape[d2] == 7)   // the drawn LFO cycle IS the sound
                                             for (int k = 0; k < DrumChannel::Slot::LFO_CURVE_N; ++k)
                                                 h = mix(h, f(sl.lfoCurve[d2][k])); }   // per-slot LFOs
        h = mix(h, f(sl.drift)); h = mix(h, f(sl.filterDrive));   // DRIFT (alive) + filter loop drive
        for (int r = 0; r < DrumChannel::MOD_ROUTES; ++r)   // MOD MATRIX routes + created sources
        { h = mix(h, sl.mod[r].src); h = mix(h, sl.mod[r].tgt); h = mix(h, f(sl.mod[r].amt));
          h = mix(h, f(sl.mod[r].lagMs));   // [2026-07-14] LAG + MOD AMOUNT MAP ride the sound hash
          h = mix(h, sl.mod[r].curveOn);
          if (sl.mod[r].curveOn) for (int k = 0; k < DrumChannel::Slot::MOD_CURVE_N; k += 4) h = mix(h, sl.mod[r].curve[k]); }
        h = mix(h, f(sl.modEnvA)); h = mix(h, f(sl.modEnvD)); h = mix(h, f(sl.modEnvH)); h = mix(h, f(sl.modEnvS)); h = mix(h, f(sl.modEnvR)); h = mix(h, f(sl.modLfoRate)); h = mix(h, sl.modLfoShape);
    }
    h = mix(h, c.layerOscShape); h = mix(h, f(c.layerSineFreq)); h = mix(h, f(c.layerSinePEnvAmt)); h = mix(h, f(c.layerSinePEnvTime)); h = mix(h, f(c.layerSinePOffset));
    h = mix(h, c.oscUnison); h = mix(h, f(c.oscDetune)); h = mix(h, f(c.oscSustain)); h = mix(h, f(c.fmSustain)); h = mix(h, f(c.physSustain));
    h = mix(h, f(c.oscVibrato)); h = mix(h, f(c.physVibrato));
    h = mix(h, f(c.physFreq)); h = mix(h, f(c.physTone)); h = mix(h, f(c.physMaterial));
    h = mix(h, f(c.physPitchEnvAmt)); h = mix(h, f(c.physPitchEnvTime)); h = mix(h, f(c.physPitchOffset));
    h = mix(h, c.noiseType); h = mix(h, f(c.layerNoiseCenter)); h = mix(h, f(c.layerNoiseWidth)); h = mix(h, f(c.noiseSustain));
    h = mix(h, f(c.fmPitch)); h = mix(h, f(c.fmSpread)); h = mix(h, f(c.fmDepth));
    h = mix(h, f(c.fmPitchEnvAmt)); h = mix(h, f(c.fmPitchEnvTime)); h = mix(h, f(c.fmPitchOffset)); h = mix(h, f(c.fmFeedback)); h = mix(h, f(c.fmSub));
    h = mix(h, f(c.physPosition)); h = mix(h, f(c.sampleCrush));
    h = mix(h, f(c.pitchEnvAmt)); h = mix(h, f(c.pitchEnvTime)); h = mix(h, f(c.pitchOffset)); h = mix(h, c.sampleReverse ? 1 : 0);
    for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s) { h = mix(h, f(c.srcAtk[s])); h = mix(h, f(c.srcHold[s])); h = mix(h, f(c.srcDec[s])); }
    h = mix(h, f(c.pitch)); h = mix(h, f(c.volume)); h = mix(h, f(c.pan));
    for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) {
        const auto& eb = c.eqBand[b]; h = mix(h, eb.on ? 1 : 0); h = mix(h, f(eb.freq)); h = mix(h, f(eb.gainDb)); h = mix(h, f(eb.q));
    }
    h = mix(h, c.filterType); h = mix(h, f(c.filterCutoff)); h = mix(h, f(c.filterReso)); h = mix(h, f(c.filterEnvAmt));
    h = mix(h, c.driveType); h = mix(h, f(c.driveAmount));
    h = mix(h, f(c.reverbSend)); h = mix(h, f(c.delaySend)); h = mix(h, c.allowOverlap ? 1 : 0);
    h = mix(h, c.usingUserSample ? (juce::int64) c.userSampleFile.getFullPathName().hashCode64() : (juce::int64) c.soundType);
    h = mix(h, f(c.playSpeed)); h = mix(h, c.useRegion ? 1 : 0); h = mix(h, f(c.sampleStart)); h = mix(h, f(c.sampleEnd));
    return h;
}

juce::int64 DrumSequencerEditor::stateHash() const
{
    auto mix = [](juce::int64 h, juce::int64 v) { return h * 1000003LL ^ v; };
    auto f   = [](float x) { juce::int64 b = 0; std::memcpy(&b, &x, sizeof(float)); return b; };
    auto& s = proc.sequencer;
    juce::int64 h = 1125899906842597LL;
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        auto& P = s.patterns[p];
        h = mix(h, P.playMode); h = mix(h, P.repeatTarget); h = mix(h, P.gotoPattern); h = mix(h, f(P.swing));
        h = mix(h, P.mergeWithPrev ? 1 : 0);   // merged-group glue (undoable)
        h = mix(h, P.chainLen); for (int k = 0; k < P.chainLen; ++k) { h = mix(h, P.chainSeq[k]); h = mix(h, P.chainLoops[k]); }
        const auto& m = P.master;        // per-pattern master FX + output
        h = mix(h, f(m.reverbRoom)); h = mix(h, f(m.reverbDamp)); h = mix(h, f(m.reverbWet));
        h = mix(h, f(m.reverbPreDelay)); h = mix(h, f(m.reverbWidth));
        h = mix(h, f(m.delayTime)); h = mix(h, f(m.delayFeedback)); h = mix(h, f(m.delayWet)); h = mix(h, m.delaySync ? 1 : 0); h = mix(h, m.delayDivision); h = mix(h, m.delayPingPong ? 1 : 0);
        h = mix(h, m.reverbMode); h = mix(h, m.delayMode); h = mix(h, m.delayModeB);   // [2026-07-15] delay loop character (undoable)
        h = mix(h, f(m.delayBarN)); h = mix(h, m.delayTrail); h = mix(h, f(m.delayDuck)); h = mix(h, f(m.delayChar));       // [2026-07-15 02:30]
        h = mix(h, f(m.delayBarNB)); h = mix(h, m.delayTrailB); h = mix(h, f(m.delayDuckB)); h = mix(h, f(m.delayCharB));
        h = mix(h, m.reverbSync ? 1 : 0); h = mix(h, f(m.reverbDecBars)); h = mix(h, f(m.reverbPreBars)); h = mix(h, f(m.reverbGate));
        h = mix(h, m.reverbSyncB ? 1 : 0); h = mix(h, f(m.reverbDecBarsB)); h = mix(h, f(m.reverbPreBarsB)); h = mix(h, f(m.reverbGateB));
        h = mix(h, f(m.reverbGateMs)); h = mix(h, f(m.reverbGateMsB)); h = mix(h, f(m.masterWidth));   // [2026-07-15 12:10]
        h = mix(h, f(m.volume)); h = mix(h, m.mono ? 1 : 0); h = mix(h, f(m.limit)); h = mix(h, f(m.glue));
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            auto& ch = P.channels[c];
            h = mix(h, channelSoundHash(ch));
            h = mix(h, ch.numSteps);
            h = mix(h, f(ch.humanizeAmt)); h = mix(h, f(ch.strumAmt)); h = mix(h, f(ch.keysMinVel)); h = mix(h, f(ch.keysMaxVel)); h = mix(h, f(ch.keysGlide));   // HUMANIZE / STRUM / min+max-vel / GLIDE (undoable)
            for (int fx = 0; fx < 3; ++fx) { h = mix(h, ch.chFxType[fx]); h = mix(h, f(ch.chFxAmt[fx])); h = mix(h, f(ch.chFxChar[fx])); }   // CHANNEL FX (undoable)
            h = mix(h, f(ch.reverbSend)); h = mix(h, f(ch.delaySend)); h = mix(h, ch.revBus); h = mix(h, ch.delBus);
            h = mix(h, ch.mergeWith + 1); h = mix(h, ch.keysSplitW1); h = mix(h, ch.keysSplitW2);   // MERGE&SPLIT pair + windows
            h = mix(h, ch.arpOn ? 1 : 0); h = mix(h, ch.arpLen); h = mix(h, ch.arpSync); h = mix(h, ch.arpRate);
            h = mix(h, ch.arpAlign ? 1 : 0); h = mix(h, ch.arpHold ? 1 : 0); h = mix(h, f(ch.arpGate));
            for (int ai = 0; ai < DrumChannel::ARP_ROWS; ++ai) h = mix(h, (int) ch.arpOffset[ai] + 128);   // ARP (undoable)
            h = mix(h, ch.keysPolyMode ? 1 : 0);   // KEYS poly/mono toggle (undoable)
            h = mix(h, ch.keysLegato ? 1 : 0);
            h = mix(h, ch.duckBy + 2); h = mix(h, f(ch.duckAmt));   // sidechain duck (undoable)
            juce::int64 st = 0; for (int i = 0; i < DrumChannel::MAX_STEPS; ++i) st = (st << 1) | (ch.steps[i] ? 1 : 0);
            h = mix(h, st); h = mix(h, ch.mute ? 1 : 0); h = mix(h, ch.solo ? 2 : 0);
            for (int i = 0; i < ch.numSteps; ++i) { h = mix(h, f(ch.stepVel[i])); h = mix(h, f(ch.stepPitch[i])); h = mix(h, f(ch.stepNoteLen[i])); h = mix(h, ch.stepSlide[i] ? 1 : 0); h = mix(h, ch.stepMerge[i] ? 1 : 0); h = mix(h, ch.stepRoll[i]); h = mix(h, f(ch.stepRollDecay[i])); h = mix(h, f(ch.stepPan[i])); h = mix(h, f(ch.stepNudge[i])); h = mix(h, f(ch.stepModA[i])); h = mix(h, f(ch.stepModB[i])); h = mix(h, ch.stepCondLen[i]); h = mix(h, ch.stepCondMask[i]); }
            h = mix(h, ch.drawMode ? 1 : 0);
            if (ch.drawMode) { h = mix(h, f(ch.drawVel)); h = mix(h, f(ch.drawPan)); h = mix(h, f(ch.drawTuneCents));
                for (int i = 0; i < ch.drawNoteCount; ++i) { const auto& nt = ch.drawNotes[i];
                    h = mix(h, nt.start); h = mix(h, nt.len); h = mix(h, (int) nt.semi + 128); h = mix(h, (int) nt.vel); h = mix(h, (int) nt.slot); h = mix(h, (int) nt.glide); h = mix(h, (int) nt.oneShot); h = mix(h, (int) nt.strumUp); h = mix(h, (int) nt.strumPct); h = mix(h, (int) nt.pan); h = mix(h, (int) nt.condLen); h = mix(h, (int) nt.condMask); } }
        }
    }
    h = mix(h, f(s.standaloneBpm)); h = mix(h, s.timeSigNum); h = mix(h, s.timeSigDen);
    return h;
}

void DrumSequencerEditor::updateStripMixLabel(int ch)
{
    const auto& c = proc.sequencer.channel(ch);
    juce::String txt = c.mixName.isEmpty() ? (c.mixModified ? juce::String("*") : juce::String("Sound Bank"))
                                           : (c.mixModified ? "*" + c.mixName : c.mixName);
    if (txt == stripMixShown[ch]) return;          // unchanged - avoid per-tick repaint
    stripMixShown[ch] = txt;
    auto& combo = strips[ch].comboSound;
    combo.setSelectedId(0, juce::dontSendNotification);
    combo.setTextWhenNothingSelected(txt);
    combo.repaint();
}

void DrumSequencerEditor::updatePresetLabel()
{
    juce::String txt = presetName.isEmpty() ? (presetModified ? juce::String("*") : juce::String("Presets"))
                                            : (presetModified ? "*" + presetName : presetName);
    if (txt == presetShown) return;
    presetShown = txt;
    comboPreset.setSelectedId(0, juce::dontSendNotification);
    comboPreset.setTextWhenNothingSelected(txt);
    comboPreset.repaint();
}

void DrumSequencerEditor::rebaselinePreset(const juce::String& name)
{
    presetName = name; presetModified = false; presetBaselineHash = stateHash();
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            auto& ch = proc.sequencer.patterns[p].channels[c];
            ch.mixHash = channelSoundHash(ch);   // re-baseline so loaded edits aren't re-flagged
        }
    updatePresetLabel();
}

// Reset one channel's sound parameters to the clean factory-fresh default.
// Does NOT touch the step pattern or the UI (callers handle those).
void DrumSequencerEditor::resetChannelToDefault(DrumChannel& c, int ch)
{
    // A fresh, untouched channel: the four classic sources enabled, evenly blended
    // (Physical starts off so the default blend is unchanged).
    for (int i = 0; i < 4; ++i) { c.srcOn[i] = true; c.srcWeight[i] = 0.25f; }
    c.srcOn[DrumChannel::SrcPhys] = false; c.srcWeight[DrumChannel::SrcPhys] = 0.0f;
    c.drawMode = false; c.drawVel = 1.0f; c.drawPan = 0.0f; c.drawTuneCents = 0.0f;   // fresh channel = step mode
    c.keysSlot2Down = 0;                                      // KEYS slot-2 transpose (channel-wide) resets too
    c.humanizeAmt = 0.0f; c.strumAmt = 0.0f; c.keysMinVel = 0.0f; c.keysMaxVel = 1.0f; c.keysGlide = 0.0f;   // HUMANIZE / STRUM / vel range / GLIDE default
    for (int fx = 0; fx < 3; ++fx) { c.chFxType[fx] = 0; c.chFxAmt[fx] = 0.0f; c.chFxChar[fx] = 0.5f; }   // CHANNEL FX off
    c.reverbSend = 0.0f; c.delaySend = 0.0f;   // channel sends off (the sound sets its own)
    c.mergeWith = -1; c.keysSplitW1 = 60; c.keysSplitW2 = 12;   // MERGE&SPLIT off / identity windows
    { DrumChannel d; c.arpOn = d.arpOn; c.arpLen = d.arpLen; c.arpSync = d.arpSync; c.arpRate = d.arpRate;
      c.arpAlign = d.arpAlign; c.arpHold = d.arpHold; c.arpGate = d.arpGate;   // ARP defaults
      for (int ai = 0; ai < DrumChannel::ARP_ROWS; ++ai) c.arpOffset[ai] = d.arpOffset[ai]; }
    c.keysPolyMode = true;                                    // keys POLY by default on Init
    c.clearDrawNotes();
    c.padX = c.padY = 0.5f; c.padLayoutB = false;
    c.layerOscShape = 0; c.layerSineFreq = 60.0f; c.layerSinePEnvAmt = 0.0f; c.layerSinePEnvTime = 0.04f; c.layerSinePOffset = 0.0f;
    c.oscUnison = 1; c.oscDetune = 0.0f; c.oscSustain = 0.0f; c.fmSustain = 0.0f; c.physSustain = 0.0f;
    c.oscVibrato = 0.0f; c.physVibrato = 0.0f;
    c.noiseType = 0; c.layerNoiseCenter = 3000.0f; c.layerNoiseWidth = 0.0f; c.noiseSustain = 0.0f;
    c.fmPitch = 0.0f; c.fmSpread = 0.0f; c.fmDepth = 0.4f;
    c.fmPitchEnvAmt = 0.0f; c.fmPitchEnvTime = 0.05f; c.fmPitchOffset = 0.0f; c.fmFeedback = 0.0f; c.fmSub = 0.0f;
    c.sampleCrush = 0.0f;
    c.physFreq = 110.0f; c.physTone = 0.5f; c.physMaterial = 0.0f; c.physPosition = 0.0f;
    c.physPitchEnvAmt = 0.0f; c.physPitchEnvTime = 0.05f; c.physPitchOffset = 0.0f;
    c.pitchEnvAmt = 0.0f; c.pitchEnvTime = 0.05f; c.pitchOffset = 0.0f; c.sampleReverse = false;
    { const float decDef[DrumChannel::NUM_SOURCES] = { 2.0f, 0.08f, 0.20f, 0.30f, 0.80f };
      for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s) { c.srcAtk[s] = 0.003f; c.srcHold[s] = 0.0f; c.srcDec[s] = decDef[s]; } }
    c.pitch = 0.0f; c.volume = 1.0f; c.pan = 0.0f;
    for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) c.eqBand[b] = DrumChannel::defaultEqBand(b);
    c.filterType = 0; c.filterCutoff = 1000.0f; c.filterReso = 0.707f; c.filterEnvAmt = 0.0f;
    c.driveType = 0; c.driveAmount = 0.0f;
    c.reverbSend = 0.0f; c.delaySend = 0.0f;
    c.usingUserSample = false;
    c.soundType = DrumSoundGenerator::Type::Kick808;
    c.sampleStart = 0.0f; c.sampleEnd = 1.0f; c.useRegion = false; c.playSpeed = 1.0f;
    c.sliceCount = 1; c.stretchAmt = 1.0f;   // (were missing - stale slices/stretch survived Init)
    c.allowOverlap = false;
    c.mixName = juce::String(); c.mixModified = false;   // no sound mix selected
    c.loadDefaultSound();
    juce::ignoreUnused(ch);
    // Default slot layout: Slot 1 = Analog + FM (SrcOsc), Slot 2 = empty.
    for (auto& s : c.slots) s = DrumChannel::Slot();      // both empty (engine = -1)
    c.slots[0].engine = DrumChannel::SrcOsc; c.slots[0].weight = 1.0f;
    c.padX = 0.0f; c.padY = 0.5f;                          // blend pinned to Slot 1 (the only occupied slot)
    c.restoredSlots = true;   // these slots are authored -> don't rebuild from legacy
    c.markDspDirty();
}

// Reset a channel to a clean default sound mix (keeps its steps).
void DrumSequencerEditor::initChannelMix(int ch)
{
    resetChannelToDefault(proc.sequencer.channel(ch), ch);
    if (ch == selectedChannel) { refreshDetailPanel(); refreshSampleSel(); updateVisuals(); }
    rebuildSoundMixMenu(ch);
}

void DrumSequencerEditor::writeChannelMix(juce::ValueTree& t, const DrumChannel& ch) const
{
    t.setProperty("sound",    (int) ch.soundType,    nullptr);
    t.setProperty("keysPoly", ch.keysPolyMode, nullptr);
    t.setProperty("keysLegato", ch.keysLegato, nullptr);   // [2026-07-16]
    if (ch.arpOn)   // DEDICATED-ARP sounds: the pattern is part of the sound and travels with it.
    {               // arp OFF = the sound carries NO arp - loading it leaves the channel's arp alone
                    // (setting an arp up takes long; a plain sound swap must never wipe it - user rule).
        t.setProperty("arpOn",   true,        nullptr);
        t.setProperty("arpLen",  ch.arpLen,   nullptr);
        t.setProperty("arpSync", ch.arpSync,  nullptr);
        t.setProperty("arpRate", ch.arpRate,  nullptr);
        t.setProperty("arpAlign", ch.arpAlign, nullptr);
        t.setProperty("arpHold", ch.arpHold,  nullptr);
        t.setProperty("arpGate", ch.arpGate,  nullptr);
        t.setProperty("arpAltS", ch.arpAltStrum, nullptr);
        juce::String ao; for (int i = 0; i < DrumChannel::ARP_ROWS; ++i) ao << (int) ch.arpOffset[i] << ',';
        t.setProperty("arpOff", ao, nullptr);
    }
    t.setProperty("keys2Dn",  ch.keysSlot2Down,      nullptr);   // slot-2 pitch rides with the sound mix
    t.setProperty("strum",    ch.strumAmt,           nullptr);   // STRUM rides with the sound (user order)
    for (int fx = 0; fx < 3; ++fx)   // CHANNEL FX slots ride with the sound
    { const juce::String k(fx);
      t.setProperty("cfxT" + k, ch.chFxType[fx], nullptr);
      t.setProperty("cfxA" + k, ch.chFxAmt[fx],  nullptr);
      t.setProperty("cfxC" + k, ch.chFxChar[fx], nullptr); }
    t.setProperty("chRev", ch.reverbSend, nullptr);   // channel sends ride with the sound
    t.setProperty("chDel", ch.delaySend,  nullptr);
    t.setProperty("userSample", ch.usingUserSample ? ch.userSampleFile.getFullPathName() : juce::String(), nullptr);
    for (int i = 0; i < DrumChannel::NUM_SOURCES; ++i) { t.setProperty("srcOn" + juce::String(i), ch.srcOn[i], nullptr);
                                  t.setProperty("srcW"  + juce::String(i), ch.srcWeight[i], nullptr); }
    t.setProperty("padX", ch.padX, nullptr);
    t.setProperty("padY", ch.padY, nullptr);
    t.setProperty("padB", ch.padLayoutB, nullptr);
    t.setProperty("oscShape", ch.layerOscShape,    nullptr);
    t.setProperty("oscFreq",  ch.layerSineFreq,    nullptr);
    t.setProperty("oscPEA",   ch.layerSinePEnvAmt, nullptr);
    t.setProperty("oscPET",   ch.layerSinePEnvTime,nullptr);
    t.setProperty("oscPOff",  ch.layerSinePOffset, nullptr);
    t.setProperty("oscUni",   ch.oscUnison,        nullptr);
    t.setProperty("oscDet",   ch.oscDetune,        nullptr);
    t.setProperty("oscSus",   ch.oscSustain,       nullptr);
    t.setProperty("oscVib",   ch.oscVibrato,       nullptr);
    t.setProperty("fmSus",    ch.fmSustain,        nullptr);
    t.setProperty("phySus",   ch.physSustain,      nullptr);
    t.setProperty("phyVib",   ch.physVibrato,      nullptr);
    t.setProperty("nSus",     ch.noiseSustain,     nullptr);
    t.setProperty("phF",      ch.physFreq,         nullptr);
    t.setProperty("phTone",   ch.physTone,         nullptr);
    t.setProperty("phMat",    ch.physMaterial,     nullptr);
    t.setProperty("phPEA",    ch.physPitchEnvAmt,  nullptr);
    t.setProperty("phPET",    ch.physPitchEnvTime, nullptr);
    t.setProperty("phPOff",   ch.physPitchOffset,  nullptr);
    t.setProperty("phPos",    ch.physPosition,     nullptr);
    t.setProperty("nType",    ch.noiseType,        nullptr);
    t.setProperty("nCtr",     ch.layerNoiseCenter, nullptr);
    t.setProperty("nWid",     ch.layerNoiseWidth,  nullptr);
    t.setProperty("fmPit",    ch.fmPitch,          nullptr);
    t.setProperty("fmSpr",    ch.fmSpread,         nullptr);
    t.setProperty("fmDep",    ch.fmDepth,          nullptr);
    t.setProperty("fmPEA",    ch.fmPitchEnvAmt,    nullptr);
    t.setProperty("fmPET",    ch.fmPitchEnvTime,   nullptr);
    t.setProperty("fmPOff",   ch.fmPitchOffset,    nullptr);
    t.setProperty("fmFb",     ch.fmFeedback,       nullptr);
    t.setProperty("fmSub",    ch.fmSub,            nullptr);
    t.setProperty("smpCrush", ch.sampleCrush,      nullptr);
    t.setProperty("useRegion", ch.useRegion,  nullptr);
    t.setProperty("smpStart",  ch.sampleStart, nullptr);
    t.setProperty("smpEnd",    ch.sampleEnd,   nullptr);
    t.setProperty("smpSpeed",  ch.playSpeed,   nullptr);
    t.setProperty("smpRev",    ch.sampleReverse, nullptr);
    t.setProperty("overlap",   ch.allowOverlap, nullptr);
    t.setProperty("pEnvA",    ch.pitchEnvAmt,      nullptr);
    t.setProperty("pEnvT",    ch.pitchEnvTime,     nullptr);
    t.setProperty("pEnvOff",  ch.pitchOffset,      nullptr);
    for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s) {
        t.setProperty("atk" + juce::String(s), ch.srcAtk[s],  nullptr);
        t.setProperty("hld" + juce::String(s), ch.srcHold[s], nullptr);
        t.setProperty("dec" + juce::String(s), ch.srcDec[s],  nullptr);
    }
    t.setProperty("pitch", ch.pitch, nullptr); t.setProperty("vol", ch.volume, nullptr);
    t.setProperty("pan", ch.pan, nullptr);
    for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) {
        const auto& eb = ch.eqBand[b]; const juce::String k = "eb" + juce::String(b);
        t.setProperty(k + "on", eb.on, nullptr); t.setProperty(k + "f", eb.freq, nullptr);
        t.setProperty(k + "g", eb.gainDb, nullptr); t.setProperty(k + "q", eb.q, nullptr);
    }
    t.setProperty("fType", ch.filterType, nullptr); t.setProperty("fCut", ch.filterCutoff, nullptr);
    t.setProperty("fReso", ch.filterReso, nullptr); t.setProperty("fEnv", ch.filterEnvAmt, nullptr);
    t.setProperty("drvT", ch.driveType, nullptr); t.setProperty("drvA", ch.driveAmount, nullptr);
    t.setProperty("v6ch", 1, nullptr);   // drive taper era (channel drvA stored for the squared law)
    t.setProperty("rev", ch.reverbSend, nullptr); t.setProperty("dly", ch.delaySend, nullptr);
    ch.writeSlots(t);   // 3-slot model (duplicate engines survive Save/Load)
}

void DrumSequencerEditor::readChannelMix(const juce::ValueTree& t, DrumChannel& ch, juce::String& missingSample) const
{
    ch.keysPolyMode = (bool) t.getProperty("keysPoly", true);
    ch.keysLegato   = (bool) t.getProperty("keysLegato", false);
    if (t.hasProperty("arpOn") && (bool) t.getProperty("arpOn", false))
    {   // the sound brings its OWN arp -> apply it; sounds without one keep the channel's arp
        ch.arpOn   = true;
        ch.arpLen  = juce::jlimit(1, 1 + DrumChannel::ARP_ROWS, (int) t.getProperty("arpLen", 2));
        { const int rawSync = (int) t.getProperty("arpSync", 8);   // -1 = LOCK TO GRID (preserved)
          ch.arpSync = rawSync < 0 ? -1 : DrumChannel::arpSnapSync(rawSync); }
        ch.arpRate = juce::jlimit(0, DrumChannel::ARP_RATES - 1, (int) t.getProperty("arpRate", 4));
        ch.arpAlign = (bool) t.getProperty("arpAlign", true);
        ch.arpHold  = (bool) t.getProperty("arpHold", false);
        ch.arpGate  = juce::jlimit(0.1f, 1.0f, (float) t.getProperty("arpGate", 1.0f));
        ch.arpAltStrum = (bool) t.getProperty("arpAltS", false);
        const juce::String ao = t.getProperty("arpOff", "").toString();
        if (ao.isNotEmpty()) { auto f = juce::StringArray::fromTokens(ao, ",", "");
            for (int i = 0; i < DrumChannel::ARP_ROWS && i < f.size(); ++i)
                ch.arpOffset[i] = (int8_t) juce::jlimit(-128, 127, f[i].getIntValue()); }
    }
    ch.keysSlot2Down = juce::jlimit(-24, 24, (int) t.getProperty("keys2Dn", 0));   // slot-2 pitch (0 for old mix files)
    ch.strumAmt = juce::jlimit(0.0f, 1.0f, (float) t.getProperty("strum", 0.0f));   // per-sound strum (0 for old files)
    for (int fx = 0; fx < 3; ++fx)   // CHANNEL FX slots (readSlots migrates pre-slot-system files after this)
    { const juce::String k(fx);
      ch.chFxType[fx] = juce::jlimit(0, 16, (int) t.getProperty("cfxT" + k, 0));
      ch.chFxAmt[fx]  = juce::jlimit(0.0f, 1.0f, (float) t.getProperty("cfxA" + k, 0.0f));
      ch.chFxChar[fx] = juce::jlimit(0.0f, 1.0f, (float) t.getProperty("cfxC" + k, 0.5f)); }
    {   // MIGRATION: last week's 4-fixed-effect mix files -> the strongest two FX slots
        struct OldFx { int type; float amt; };
        OldFx v[4] = { { DrumChannel::ChFxChorus,  juce::jlimit(0.0f, 1.0f, (float) t.getProperty("chFxCho", 0.0f)) },
                       { DrumChannel::ChFxFlanger, juce::jlimit(0.0f, 1.0f, (float) t.getProperty("chFxFlg", 0.0f)) },
                       { DrumChannel::ChFxPhaser,  juce::jlimit(0.0f, 1.0f, (float) t.getProperty("chFxPhs", 0.0f)) },
                       { DrumChannel::ChFxComp,    juce::jlimit(0.0f, 1.0f, (float) t.getProperty("chFxCmp", 0.0f)) } };
        for (int fx = 0; fx < 3; ++fx)
        {
            if (ch.chFxType[fx] != DrumChannel::ChFxOff) continue;
            int best = -1; for (int j = 0; j < 4; ++j) if (v[j].amt > 0.001f && (best < 0 || v[j].amt > v[best].amt)) best = j;
            if (best < 0) break;
            ch.chFxType[fx] = v[best].type; ch.chFxAmt[fx] = v[best].amt; ch.chFxChar[fx] = 0.5f;
            v[best].amt = 0.0f;
        }
    }
    ch.reverbSend = juce::jlimit(0.0f, 1.0f, (float) t.getProperty("chRev", (float) ch.reverbSend));   // channel sends
    ch.delaySend  = juce::jlimit(0.0f, 1.0f, (float) t.getProperty("chDel", (float) ch.delaySend));
    for (int i = 0; i < DrumChannel::NUM_SOURCES; ++i) { ch.srcOn[i]     = (bool)  t.getProperty("srcOn" + juce::String(i), i == 0);
                                  ch.srcWeight[i] = (float) t.getProperty("srcW"  + juce::String(i), i == 0 ? 1.0f : 0.0f); }
    ch.padX = (float) t.getProperty("padX", 0.5f);
    ch.padY = (float) t.getProperty("padY", 0.5f);
    ch.padLayoutB = (bool) t.getProperty("padB", false);
    ch.layerOscShape     = (int)  t.getProperty("oscShape", 0);
    ch.layerSineFreq     = (float)t.getProperty("oscFreq", 60.0f);
    ch.layerSinePEnvAmt  = (float)t.getProperty("oscPEA", 0.0f);
    ch.layerSinePEnvTime = (float)t.getProperty("oscPET", 0.04f);
    ch.layerSinePOffset  = (float)t.getProperty("oscPOff", 0.0f);
    ch.oscUnison         = (int)  t.getProperty("oscUni", 1);
    ch.oscDetune         = (float)t.getProperty("oscDet", 0.0f);
    ch.oscSustain        = (float)t.getProperty("oscSus", 0.0f);
    ch.oscVibrato        = (float)t.getProperty("oscVib", 0.0f);
    ch.fmSustain         = (float)t.getProperty("fmSus", 0.0f);
    ch.physSustain       = (float)t.getProperty("phySus", 0.0f);
    ch.physVibrato       = (float)t.getProperty("phyVib", 0.0f);
    ch.noiseSustain      = (float)t.getProperty("nSus", 0.0f);
    ch.physFreq          = (float)t.getProperty("phF", 110.0f);
    ch.physTone          = (float)t.getProperty("phTone", 0.5f);
    ch.physMaterial      = (float)t.getProperty("phMat", 0.0f);
    ch.physPitchEnvAmt   = (float)t.getProperty("phPEA", 0.0f);
    ch.physPitchEnvTime  = (float)t.getProperty("phPET", 0.05f);
    ch.physPitchOffset   = (float)t.getProperty("phPOff", 0.0f);
    ch.physPosition      = (float)t.getProperty("phPos", 0.0f);
    ch.noiseType         = (int)  t.getProperty("nType", 0);
    ch.layerNoiseCenter  = (float)t.getProperty("nCtr", 3000.0f);
    ch.layerNoiseWidth   = (float)t.getProperty("nWid", 0.0f);
    ch.fmPitch           = (float)t.getProperty("fmPit", 0.0f);
    ch.fmSpread          = (float)t.getProperty("fmSpr", 0.0f);
    ch.fmDepth           = (float)t.getProperty("fmDep", 0.4f);
    ch.fmPitchEnvAmt     = (float)t.getProperty("fmPEA", 0.0f);
    ch.fmPitchEnvTime    = (float)t.getProperty("fmPET", 0.05f);
    ch.fmPitchOffset     = (float)t.getProperty("fmPOff", 0.0f);
    ch.fmFeedback        = (float)t.getProperty("fmFb", 0.0f);
    ch.fmSub             = (float)t.getProperty("fmSub", 0.0f);
    ch.sampleCrush       = (float)t.getProperty("smpCrush", 0.0f);
    ch.useRegion         = (bool) t.getProperty("useRegion", false);
    ch.sampleStart       = (float)t.getProperty("smpStart", 0.0f);
    ch.sampleEnd         = (float)t.getProperty("smpEnd", 1.0f);
    ch.playSpeed         = (float)t.getProperty("smpSpeed", 1.0f);
    ch.sampleReverse     = (bool) t.getProperty("smpRev", false);
    ch.allowOverlap      = (bool) t.getProperty("overlap", false);
    ch.pitchEnvAmt       = (float)t.getProperty("pEnvA", 0.0f);
    ch.pitchEnvTime      = (float)t.getProperty("pEnvT", 0.05f);
    ch.pitchOffset       = (float)t.getProperty("pEnvOff", 0.0f);
    { const float decDef[4] = { 2.0f, 0.08f, 0.20f, 0.30f };
      for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s) {
          ch.srcAtk[s]  = (float)t.getProperty("atk" + juce::String(s), 0.003f);
          ch.srcHold[s] = (float)t.getProperty("hld" + juce::String(s), 0.0f);
          ch.srcDec[s]  = (float)t.getProperty("dec" + juce::String(s), decDef[s]); } }
    ch.pitch = (float)t.getProperty("pitch", 0.0f); ch.volume = (float)t.getProperty("vol", 1.0f);
    ch.pan = (float)t.getProperty("pan", 0.0f);
    for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) {
        auto& eb = ch.eqBand[b]; const DrumChannel::EqBand d = DrumChannel::EqBand(); const juce::String k = "eb" + juce::String(b);
        eb.on = (bool)t.getProperty(k + "on", false); eb.freq = (float)t.getProperty(k + "f", d.freq);
        eb.gainDb = (float)t.getProperty(k + "g", 0.0f); eb.q = (float)t.getProperty(k + "q", 1.0f);
    }
    ch.filterType = (int)t.getProperty("fType", 0); ch.filterCutoff = (float)t.getProperty("fCut", 20000.0f);
    ch.filterReso = (float)t.getProperty("fReso", 0.707f); ch.filterEnvAmt = (float)t.getProperty("fEnv", 0.0f);
    ch.driveType = (int)t.getProperty("drvT", 0); ch.driveAmount = (float)t.getProperty("drvA", 0.0f);
    if (! t.hasProperty("v6ch") && ch.driveType != DrumChannel::Bitcrush && ch.driveAmount > 0.0f)   // v6 taper migration
        ch.driveAmount = std::sqrt(juce::jlimit(0.0f, 1.0f, ch.driveAmount));
    ch.reverbSend = (float)t.getProperty("rev", 0.0f); ch.delaySend = (float)t.getProperty("dly", 0.0f);

    ch.loadDefaultSound();   // clear all slot sample buffers
    ch.soundType = (DrumSoundGenerator::Type)(int)t.getProperty("sound", 0);

    // Prefer saved 3-slot data (readSlots reloads each slot's OWN sample); else legacy.
    if (! ch.readSlots(t)) ch.buildSlotsFromLegacy();

    // MIGRATION: old files stored ONE per-channel sample ("userSample") with no per-slot file.
    // If nothing loaded into a slot, put it in the first Sample slot.
    bool anyLoaded = false;
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) if (ch.slotSample[b].usingUser) anyLoaded = true;
    juce::String path = t.getProperty("userSample", "").toString();
    if (! anyLoaded && path.isNotEmpty())
    {
        juce::File f(path);
        if (f.existsAsFile())
        {
            int tgt = 0; for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
                if (ch.slots[b].engine == DrumChannel::SrcSample) { tgt = b; break; }
            ch.loadUserSample(tgt, f);
        }
        else missingSample = path;
    }
    ch.markDspDirty();
}

void DrumSequencerEditor::loadSoundMix(int ch, const juce::File& file)
{
    juce::FileInputStream in(file);
    if (!in.openedOk()) return;
    auto t = juce::ValueTree::readFromStream(in);
    if (!t.isValid()) return;

    juce::String missing;
    readChannelMix(t, proc.sequencer.channel(ch), missing);

    if (ch == selectedChannel) { refreshDetailPanel(); refreshSampleSel(); updateVisuals(); }

    if (missing.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
            "Missing sample",
            "This mix references a sample that could not be found:\n\n" + missing
            + "\n\nThe built-in sound was loaded instead. Re-load the sample to restore it.");
}

void DrumSequencerEditor::saveSoundMix()
{
    auto* aw = new juce::AlertWindow("Save Sound Mix",
                                     "Name this sound mix:", juce::AlertWindow::NoIcon);
    aw->addTextEditor("name", "My Mix");
    aw->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int r)
    {
        if (r == 1)
        {
            auto name = aw->getTextEditorContents("name").trim();
            if (name.isNotEmpty())
            {
                auto doSave = [this, name]
                {
                    juce::ValueTree t("SoundMix");
                    t.setProperty("name", name, nullptr);
                    auto& sel = proc.sequencer.channel(selectedChannel);
                    writeChannelMix(t, sel);
                    auto file = getSoundMixFolder().getChildFile(name + "." + kSoundExt);
                    file.deleteFile();
                    juce::FileOutputStream os(file);
                    if (os.openedOk()) t.writeToStream(os);
                    // The selected channel now matches this saved mix -> clean baseline.
                    sel.mixName = name; sel.mixModified = false; sel.mixHash = channelSoundHash(sel);
                    rescanSoundMixes();
                    for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c) rebuildSoundMixMenu(c);
                };
                // A sound with this name already exists: ASK before replacing it (PopupMenu,
                // never a message box - the user picks Replace or goes back to rename).
                if (getSoundMixFolder().getChildFile(name + "." + kSoundExt).existsAsFile())
                {
                    juce::PopupMenu wm;
                    wm.addSectionHeader("'" + name + "' already exists in your Sound Bank.");
                    wm.addItem(1, "Replace it");
                    wm.addItem(2, "Cancel (pick another name)");
                    wm.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&btnSaveMix),
                                     [this, doSave](int c) { if (c == 1) doSave(); else saveSoundMix(); });
                }
                else doSave();
            }
        }
        delete aw;
    }), false);
}

//==============================================================================
// Presets (saved as files, also usable via Reaper's own preset bar)
//==============================================================================
juce::File DrumSequencerEditor::getPresetsFolder()
{
    return UserPaths::presets();
}

void DrumSequencerEditor::rebuildPresetMenu()
{
    presetFiles.clear();
    auto files = getPresetsFolder().findChildFiles(juce::File::findFiles, false, kPresetWild);
    files.sort();
    for (auto& f : files) presetFiles.add(f);

    comboPreset.clear(juce::dontSendNotification);
    comboPreset.addItem("Initialize new preset", ID_INIT_PRESET);
    // Built-in factory grooves (read-only).
    comboPreset.addSectionHeading("Factory presets");
    auto facNames = Factory::presetNames();
    for (int i = 0; i < facNames.size(); ++i)
        comboPreset.addItem(facNames[i], FACTORY_PST_BASE + i);
    // The user's own saved presets.
    comboPreset.addSectionHeading("Your presets");
    if (presetFiles.isEmpty())
        comboPreset.addItem("(none saved yet)", -1);
    for (int i = 0; i < presetFiles.size(); ++i)
        comboPreset.addItem(presetFiles[i].getFileNameWithoutExtension(), i + 1);
    comboPreset.addSeparator();
    comboPreset.addItem("Save Preset...",         9001);
    comboPreset.addItem("Refresh presets folder", 9002);   // rescan so newly-saved presets show up
    comboPreset.addItem("Show Folder",            9004);   // open the Presets folder
    comboPreset.setItemEnabled(-1, false);
    comboPreset.setTextWhenNothingSelected("Presets");
    comboPreset.setSelectedId(0, juce::dontSendNotification);
}

void DrumSequencerEditor::handlePresetChange()
{
    int id = comboPreset.getSelectedId();

    // Loading a preset stops the transport (a new song shouldn't keep the old one playing).
    const bool isLoad = (id >= FACTORY_PST_BASE && id < FACTORY_PST_BASE + Factory::presetNames().size())
                        || id == ID_INIT_PRESET || (id >= 1 && id <= presetFiles.size());
    if (isLoad && proc.sequencer.isCurrentlyPlaying && ! proc.sequencer.dawSync) proc.standaloneStop();

    if (id >= FACTORY_PST_BASE && id < FACTORY_PST_BASE + Factory::presetNames().size())
    {
        const int pi = id - FACTORY_PST_BASE;
        Factory::applyPreset(proc.sequencer, pi);
        proc.keysTakes.clear(); keysLoadedTakeIdx = -1; keysLoadedTakeHash = 0;   // takes are preset-level (applyPreset/resetAll only reset the sequencer)
        syncAfterStateChange();
        rebaselinePreset(Factory::presetNames()[pi]);   // shows name, clean baseline
    }
    else if (id == ID_INIT_PRESET)
    {
        initPreset();
    }
    else if (id >= 1 && id <= presetFiles.size())
    {
        juce::MemoryBlock mb;
        if (presetFiles[id - 1].loadFileAsData(mb))
        {
            proc.setStateInformation(mb.getData(), (int) mb.getSize());
            keysLoadedTakeIdx = -1; keysLoadedTakeHash = 0;   // takes were reloaded from the file
            fullRefresh();
            rebaselinePreset(presetFiles[id - 1].getFileNameWithoutExtension());
        }
    }
    else if (id == 9001) // Save
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Save Preset", getPresetsFolder().getChildFile("My Preset." + kPresetExt), "*." + kPresetExt);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f != juce::File())
                {
                    if (f.getFileExtension() != "." + kPresetExt) f = f.withFileExtension(kPresetExt);
                    juce::MemoryBlock mb;
                    proc.getStateInformation(mb);
                    f.replaceWithData(mb.getData(), mb.getSize());
                    rebaselinePreset(f.getFileNameWithoutExtension()); // now the clean baseline
                }
                rebuildPresetMenu();
            });
        comboPreset.setSelectedId(0, juce::dontSendNotification);
    }
    else if (id == 9002) // Refresh presets folder - rescan so presets saved on disk appear in the list
    {
        rebuildPresetMenu();   // (also resets the combo selection to 0)
    }
    else if (id == 9004) // Show Folder - open the Presets folder
    {
        getPresetsFolder().revealToUser();
        comboPreset.setSelectedId(0, juce::dontSendNotification);
    }
}

//==============================================================================
// MIDI menu (top-bar dropdown; replaces the old "Clear MIDI" button)
//==============================================================================
juce::File DrumSequencerEditor::getMidiPatternsFolder() { return UserPaths::midiPatterns(); }

void DrumSequencerEditor::rebuildMidiMenu()
{
    midiPatternFiles.clear();
    auto files = getMidiPatternsFolder().findChildFiles(juce::File::findFiles, false, kPatternWild);
    files.sort();
    for (auto& f : files) midiPatternFiles.add(f);

    comboMidi.clear(juce::dontSendNotification);
    comboMidi.addItem("Save MIDI pattern...", 9101);           // saves the CURRENT pattern's note grid
    comboMidi.addSectionHeading("Factory MIDI patterns");
    comboMidi.addItem("Ch1 8x8 + M/S/OV + Play", 9105);        // MIDI-learn map: steps CC1-64, M/S/OV 65-88, Play/Stop 89/90 (MIDI ch 1)
    comboMidi.addItem("Launchpad + Launchkey", 9107);          // [2026-07-14 23:20] pairs with the shipped custom controller modes (.syx)
    comboMidi.addSectionHeading("Saved MIDI patterns");
    if (midiPatternFiles.isEmpty())
        comboMidi.addItem("(none saved yet)", -1);
    for (int i = 0; i < midiPatternFiles.size(); ++i)
        comboMidi.addItem(midiPatternFiles[i].getFileNameWithoutExtension(), i + 1);   // ids 1..N = load
    comboMidi.addSeparator();
    comboMidi.addItem("Refresh MIDI pattern folder", 9102);
    comboMidi.addItem("Show Folder",                 9103);
    comboMidi.addSeparator();
    comboMidi.addItem("MIDI-learn: selection controls...", 9106);   // next/prev channel + pattern, slot 1<->2
    comboMidi.addItem("Clear MIDI (learn)",          9104);    // clears MIDI-learn CC assignments (the old button)
    comboMidi.setItemEnabled(-1, false);
    comboMidi.setTextWhenNothingSelected("MIDI");
    comboMidi.setSelectedId(0, juce::dontSendNotification);
}

void DrumSequencerEditor::handleMidiMenuChange()
{
    const int id = comboMidi.getSelectedId();
    if (id == 9101)          // Save MIDI pattern...
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Save MIDI pattern", getMidiPatternsFolder().getChildFile("My Pattern." + kPatternExt), "*." + kPatternExt);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f != juce::File())
                {
                    if (f.getFileExtension() != "." + kPatternExt) f = f.withFileExtension(kPatternExt);
                    saveMidiPattern(f);
                }
                rebuildMidiMenu();
            });
    }
    else if (id == 9105)     // Factory example map (the TouchOSC template matches this). All on MIDI ch 1:
    {                        //   CC 1-64 = 8x8 steps, 65-72 Mute, 73-80 Solo, 81-88 Overlap, 89 Play, 90 Stop.
        const int p = currentPattern();
        const juce::String pp = "p" + juce::String(p) + "_";
        for (int ch = 0; ch < 8; ++ch)
        {
            for (int st = 0; st < 8; ++st)
                // paramId MUST match StepGridComponent::stepParamId: "p{P}_step_{ch}_{step}".
                proc.midiLearn.assign(pp + "step_" + juce::String(ch) + "_" + juce::String(st), ch * 8 + st + 1, 1);
            // Per-channel Mute / Solo / Overlap (paramIds match updateStripParamIds()).
            proc.midiLearn.assign(pp + "ch" + juce::String(ch) + "_mute",    65 + ch, 1);
            proc.midiLearn.assign(pp + "ch" + juce::String(ch) + "_solo",    73 + ch, 1);
            proc.midiLearn.assign(pp + "ch" + juce::String(ch) + "_overlap", 81 + ch, 1);
        }
        proc.midiLearn.assign("global_play", 89, 1);   // transport (global, pattern-independent)
        proc.midiLearn.assign("global_stop", 90, 1);
        stepGrid.repaint(); content.repaint();   // the grid draws each step's assigned CC
    }
    else if (id == 9107)     // [2026-07-14 23:20] Factory "Launchpad + Launchkey" map (embedded bytes).
    {                        //   Pairs with the download's "launchpad+launchkey custom midi modes" .syx files.
        juce::MemoryInputStream in(kLaunchMapBytes, (size_t) kLaunchMapSize, false);
        auto tree = juce::ValueTree::readFromStream(in);
        if (tree.isValid())
        {
            proc.midiLearn.loadState(tree);   // replaces the current assignments, like loading a file
            stepGrid.repaint(); content.repaint();
        }
    }
    else if (id == 9106)     // MIDI-learn: SELECTION controls (they act on whatever is selected)
    {
        auto* L = &proc.midiLearn;
        auto tag = [L](const char* pid) {
            const int cc = L->getCCForParam(pid);
            return cc < 0 ? juce::String()
                          : "   [ch" + juce::String(L->getChannelForParam(pid)) + " cc" + juce::String(cc) + "]";
        };
        juce::PopupMenu m;
        m.addSectionHeader("Act on the SELECTION (learn + press a pad)");
        m.addItem(1, "Next channel..."  + tag("ui_sel_chNext"));
        m.addItem(2, "Prev channel..."  + tag("ui_sel_chPrev"));
        m.addItem(3, "Next pattern..."  + tag("ui_sel_patNext"));
        m.addItem(4, "Prev pattern..."  + tag("ui_sel_patPrev"));
        m.addItem(5, "Slot 1 <-> 2..."  + tag("ui_sel_slotSel"));
        m.addItem(6, "TEST the selected channel..." + tag("ui_sel_test"));
        m.addSectionHeader("Global (knobs / pads)");
        m.addItem(7, "BPM (knob)..."   + tag("ui_sel_bpm"));
        m.addItem(8, "Swing (knob)..." + tag("ui_sel_swing"));
        m.addItem(9, "Undo (pad)..."   + tag("ui_sel_undo"));
        m.addItem(10, "Redo (pad)..."  + tag("ui_sel_redo"));
        m.addItem(12, "Mod Wheel (knob/CC)..." + tag("ui_sel_modWheel"));   // the MOD WHEEL modulation source (also CC1 by default)
        m.addItem(13, "Slide (knob/CC)..."     + tag("ui_sel_slide"));      // the SLIDE source (MPE CC74 by default)
        m.addItem(14, "Pitch Wheel (knob/CC)..." + tag("ui_sel_pitchWheel"));  // the wheel source (the real wheel by default)
        m.addSeparator(); m.addItem(11, "Clear these assignments");
        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&comboMidi).withMinimumWidth(260),
            [L](int r) {
                static const char* pids[13] = { "ui_sel_chNext", "ui_sel_chPrev",
                                                "ui_sel_patNext", "ui_sel_patPrev",
                                                "ui_sel_slotSel", "ui_sel_test",
                                                "ui_sel_bpm", "ui_sel_swing",
                                                "ui_sel_undo", "ui_sel_redo", "ui_sel_modWheel",
                                                "ui_sel_slide", "ui_sel_pitchWheel" };
                if (r >= 1 && r <= 10) L->startLearning(pids[r - 1]);
                else if (r == 12) L->startLearning(pids[10]);
                else if (r == 13) L->startLearning(pids[11]);
                else if (r == 14) L->startLearning(pids[12]);
                else if (r == 11) for (auto* p : pids) L->clearParam(p);
            });
    }
    else if (id == 9102)     // Refresh MIDI pattern folder
        rebuildMidiMenu();
    else if (id == 9103)     // Show Folder
        getMidiPatternsFolder().revealToUser();
    else if (id == 9104)     // Clear MIDI-learn assignments (the old "Clear MIDI")
        juce::NativeMessageBox::showOkCancelBox(juce::AlertWindow::QuestionIcon,
            "Clear all MIDI assignments?",
            "This removes every MIDI CC assignment in the whole plugin. This cannot be undone "
            "(but a saved preset keeps its own assignments - reloading it restores them).",
            this, juce::ModalCallbackFunction::create([this](int ok) {
                if (ok) { proc.midiLearn.clearAll(); content.repaint(); } }));
    else if (id >= 1 && id <= midiPatternFiles.size())   // load a saved pattern onto the current pattern
        loadMidiPattern(midiPatternFiles[id - 1]);

    comboMidi.setSelectedId(0, juce::dontSendNotification);
}

// A "MIDI pattern" is the MIDI-LEARN MAP: which MIDI channel + CC number is assigned to each parameter and
// step (exactly what MidiLearnManager stores). It does NOT save any parameter/step VALUES (velocity, pitch,
// which steps are on, etc.) - just the CC assignments - so it's a portable controller map. (The "Drag MIDI"
// button separately exports a standard .mid note clip.)
void DrumSequencerEditor::saveMidiPattern(const juce::File& file)
{
    auto tree = proc.midiLearn.saveState();   // <MidiLearn><Assign param cc channel/>...</MidiLearn>
    file.deleteFile();
    juce::FileOutputStream os(file);
    if (os.openedOk()) tree.writeToStream(os);
}

// Load a saved MIDI-learn map. Replaces the current CC assignments (MidiLearnManager::loadState clears first).
void DrumSequencerEditor::loadMidiPattern(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (! in.openedOk()) return;
    auto tree = juce::ValueTree::readFromStream(in);
    if (! tree.isValid()) return;
    proc.midiLearn.loadState(tree);
    stepGrid.repaint();   // steps redraw their assigned-CC labels
    content.repaint();    // knobs/buttons repaint their assignment rings
}

// Push BPM / time signature from the sequencer into the toolbar and refresh
// everything (used after loading a factory preset or initializing a new one).
void DrumSequencerEditor::syncAfterStateChange()
{
    auto& s = proc.sequencer;
    proc.currentBpm        = s.standaloneBpm;
    proc.currentTimeSigNum = s.timeSigNum;
    proc.currentTimeSigDen = s.timeSigDen;
    barTimeSigX = s.timeSigNum;
    barTimeSigY = s.timeSigDen;
    sliderBpm.setValue(s.standaloneBpm, juce::dontSendNotification);
    barSigX.setText(juce::String(s.timeSigNum), juce::dontSendNotification);
    barSigY.setText(juce::String(s.timeSigDen), juce::dontSendNotification);
    updateBarLength();
    fullRefresh();
}

// "Initialize new preset": everything back to a clean default state - 8 steps
// per channel, no steps active, all knobs at default, 120 BPM 4/4, and no
// leftover parameter changes from any previous pattern.
void DrumSequencerEditor::initPreset()
{
    auto& s = proc.sequencer;
    s.standaloneBpm = 120.0f; s.timeSigNum = 4; s.timeSigDen = 4; s.currentPattern = 0;
    // Recorded takes are preset-level (like the factory + file-load paths) - a fresh preset has none.
    proc.keysTakes.clear(); keysLoadedTakeIdx = -1; keysLoadedTakeHash = 0;
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        auto& P = s.patterns[p];
        P.swing = 0.0f; P.playMode = Sequencer::LoopForever; P.repeatTarget = 2; P.gotoPattern = 0;
        P.chainLen = 0; P.chainStep = 0;
        P.mergeWithPrev = false;            // merged groups reset on Init too
        P.master = Sequencer::MasterFX();   // reset reverb/delay/tilt/sat/vol/limiter - was LEAKING from the old preset
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            auto& ch = P.channels[c];
            resetChannelToDefault(ch, c);
            ch.chokeGroup = 0; ch.outputBus = 0; ch.midiOut = false; ch.midiOutChannel = 1;   // routing is preset-level -> reset on Init too
            ch.revBus = 0; ch.delBus = 0;   // reverb/delay bus assignment is routing too
            ch.mute = false; ch.solo = false;   // mixer state resets with the preset too (audit)
            ch.duckBy = -1; ch.duckAmt = 0.5f;
            ch.numSteps = 8;
            ch.clearStepData();   // wipe every per-step value (a fresh init starts clean, like Clear)
            // Channels 7 + 8 (0-indexed 6 + 7) open in PIANO ROLL on a fresh init (user: a starting
            // point for melodic/chord parts). The rest start in step mode.
            ch.drawMode = (c == 6 || c == 7);
        }
    }
    syncAfterStateChange();
    rebaselinePreset(juce::String());   // a fresh init has no preset name, clean baseline
}

void DrumSequencerEditor::refreshPatternOptions()
{
    // MERGED GROUP: the play-mode/chain settings live on (and are edited on) the LAST bar - the
    // bar the playback LEAVES from (user rule). Single pattern: groupEnd == itself.
    auto& p = proc.sequencer.patterns[proc.sequencer.groupEnd(currentPattern())];
    juce::String inf   = juce::String(juce::CharPointer_UTF8("\xE2\x88\x9E"));
    juce::String arrow = juce::String(juce::CharPointer_UTF8("\xE2\x86\x92"));

    juce::ignoreUnused(arrow);
    juce::String t;
    if (p.playMode == Sequencer::LoopForever) t = "Loop " + inf;
    else if (p.playMode == Sequencer::StopAfterN) t = "Stop after " + juce::String(p.repeatTarget);
    else if (p.playMode == Sequencer::Chain && p.chainLen > 0) {
        juce::String cs; for (int k = 0; k < p.chainLen; ++k)
            cs += "P" + juce::String(p.chainSeq[k] + 1) + "(" + juce::String(p.chainLoops[k]) + ")" + (k < p.chainLen - 1 ? ">" : "");
        t = "Chain " + cs;
    }
    else t = "Go to P" + juce::String(p.gotoPattern + 1) + " after " + juce::String(p.repeatTarget);   // legacy NextAfterN

    patModeBtn.setButtonText(t);   // the down-triangle is drawn by DropButtonLNF (cleaner than a unicode glyph)
}

// The swing KNOB caption shows the live value (Swing 66% / Swing Off) - the knob has no textbox
// (the old label+fader+readout trio ate a third of the pattern row).
void DrumSequencerEditor::refreshSwingLabel()
{
    const double v = sliderSwing.getValue();
    lblSwing.setText("Swing " + (sliderSwing.textFromValueFunction ? sliderSwing.textFromValueFunction(v)
                                                                   : juce::String(v, 2)),
                     juce::dontSendNotification);
}

// Small modal dialog to TYPE a loop count (so the user can enter any number, e.g. 128, not pick from a list).
void DrumSequencerEditor::askLoopCount(const juce::String& title, int defVal, std::function<void(int)> onResult)
{
    auto* aw = new juce::AlertWindow(title, "Number of loops (1-999):", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor("n", juce::String(defVal));
    aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true, juce::ModalCallbackFunction::create(
        [aw, onResult](int r) {
            if (r == 1 && onResult) onResult(juce::jlimit(1, 999, aw->getTextEditorContents("n").getIntValue()));
        }), true);   // delete the window when dismissed (after this callback runs)
}

void DrumSequencerEditor::fullRefresh()
{
    barTimeSigX = proc.sequencer.timeSigNum;
    barTimeSigY = proc.sequencer.timeSigDen;
    rescanSoundMixes();
    rebuildSampleMenu();
    for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch)
        rebuildSoundMixMenu(ch);
    selectPattern(proc.sequencer.currentPattern);
    selectChannel(selectedChannel);
    refreshPatternOptions();
    refreshFollowButton();
    refreshAuditionButton();
    // Routing (choke group / aux Out / MIDI Out) is CHANNEL-WIDE - it must be identical on every pattern. A load can
    // leave it inconsistent (old projects, or patterns 16-31 that weren't in the saved file), which made it "sometimes"
    // show stale routing. Force every pattern's copy to match pattern 0 (the preset/reset authority).
    for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c) {
        const auto& s0 = proc.sequencer.patterns[0].channels[c];
        for (int p = 1; p < Sequencer::NUM_PATTERNS; ++p) {
            auto& cc = proc.sequencer.patterns[p].channels[c];
            cc.chokeGroup = s0.chokeGroup; cc.outputBus = s0.outputBus; cc.midiOut = s0.midiOut; cc.midiOutChannel = s0.midiOutChannel;
            cc.duckBy = s0.duckBy; cc.duckAmt = s0.duckAmt;
        }
    }
    refreshRouting();   // routing/choke are preset-level now -> recolour the strips after a preset/state change
    visiblePatterns = Sequencer::NUM_PATTERNS;   // always 32 (the 16/32 toggle is gone)
    firstPatternCol = juce::jlimit(0, juce::jmax(0, visiblePatterns - patShown()), firstPatternCol);
    refreshCountButtons();
}

//== Undo / redo (whole-instrument state snapshots) ===========================
void DrumSequencerEditor::pushUndoSnapshot()
{
    UndoEntry e;
    e.state = proc.captureStateTree();          // the TREE directly - no binary roundtrip (fast)
    e.presetName         = presetName;          // remember the preset label as it is now
    e.presetBaselineHash = presetBaselineHash;
    e.presetModified     = presetModified;
    undoStack.push_back(std::move(e));
    if ((int) undoStack.size() > kUndoMax) undoStack.erase(undoStack.begin());
    redoStack.clear();
    updateUndoRedoEnabled();
}

// Force-commit the current state as an undo entry NOW, so a destructive action that follows
// (Clear, a mode-switch clear) is undoable even when the passive settle hasn't fired yet - e.g.
// notes that were just RECORDED (undo capture is disabled during recording) have no committed
// pre-state otherwise. A no-op when the current state already matches the last committed entry.
void DrumSequencerEditor::commitUndoNow()
{
    if (applyingUndo) return;
    const juce::int64 h = stateHash();
    if (undoStack.empty()) { pushUndoSnapshot(); lastUndoHash = h; return; }
    if (h != lastUndoHash) { pushUndoSnapshot(); lastUndoHash = h; undoDirty = false; undoStableTicks = 0; }
}

void DrumSequencerEditor::applyUndoState(const UndoEntry& e)
{
    applyingUndo = true;
    proc.applyStateTree(e.state);               // apply the TREE directly (no deserialize)
    fullRefresh();
    presetName         = e.presetName;          // restore the preset label too
    presetBaselineHash = e.presetBaselineHash;
    presetModified     = e.presetModified;
    updatePresetLabel();
    lastUndoHash    = stateHash();
    undoDirty       = false;
    undoStableTicks = 0;
    applyingUndo    = false;
    updateUndoRedoEnabled();
}

// Cmd+Z (mac) / Ctrl+Z (win) = undo; Cmd+Shift+Z or Ctrl+Y = redo. commandModifier abstracts the
// platform key. Best-effort: some hosts eat plugin key events before we see them.
// [2026-07-15 19:45] STANDALONE: advertise the window as MAXIMIZABLE so the OS gestures work
// (Windows: double-click the title bar / the square button; macOS: the green zoom; Linux: WM
// maximize). JUCE's standalone wrapper ships without the maximise style flag; adding the button
// requirement here recreates the native title bar with it. Free resizing is untouched.
void DrumSequencerEditor::parentHierarchyChanged()
{
    if (proc.wrapperType != juce::AudioProcessor::wrapperType_Standalone) return;
    static bool applied = false;   // once per process is plenty (no getter exists to check)
    if (applied) return;
    if (auto* dw = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
    { dw->setTitleBarButtonsRequired(juce::DocumentWindow::allButtons, false); applied = true; }
}

bool DrumSequencerEditor::keyPressed(const juce::KeyPress& k)
{
    using MK = juce::ModifierKeys;
    if (k == juce::KeyPress('z', MK::commandModifier, 0))                       { doUndo(); return true; }
    if (k == juce::KeyPress('z', MK::commandModifier | MK::shiftModifier, 0)
        || k == juce::KeyPress('y', MK::commandModifier, 0))                    { doRedo(); return true; }
    return false;
}

// The channel's CURRENT sound as a picker id, located by its mixName - the one thing every pick
// path records (the combo's selectedId is NOT set by the picker panel, so it can't be trusted).
int DrumSequencerEditor::currentSoundPickId(int ch) const
{
    const auto& name = proc.sequencer.channel(ch).mixName;
    if (name.isEmpty()) return 0;
    auto facNames = Factory::mixNames();
    for (int i = 0; i < facNames.size(); ++i)
        if (name == facNames[i]) return FACTORY_MIX_BASE + i;
    for (int i = 0; i < soundMixFiles.size(); ++i)
        if (name == soundMixFiles[i].getFileNameWithoutExtension()) return i + 1;
    return 0;
}

// Apply ONE selected-scope MIDI CC (ui_sel_*) to the current selection. Knob targets write the
// SELECTED slot/channel fields through the same paths the on-screen controls use; button targets
// trigger the actual buttons (all their side-effects included). Runs on the message thread.
void DrumSequencerEditor::applySelCC(int t, float v, bool& slotDirty, bool& keysDirty)
{
    using P = DrumSequencerProcessor;
    auto& ch = proc.sequencer.channel(selectedChannel);
    auto& sl = ch.slots[envTargetSlot()];
    if (t >= P::SelStepBase && t < P::SelStepBase + DrumChannel::MAX_STEPS)
    {   // step N of the SELECTED channel: SET from the pad's value (127 = on, 0 = off)
        ch.steps[t - P::SelStepBase] = v >= 0.5f;
        return;
    }
    if (t >= P::SelModAmtBase && t < P::SelModAmtBase + DrumChannel::MOD_ROUTES)
    {   // [2026-07-14 11:10] mod route R's AMOUNT on the SELECTED slot (the picker's MIDI button).
        // Same cubic law as the on-screen fader position: CC centre = 0, fine control near zero.
        const float p = v * 2.0f - 1.0f;
        sl.mod[t - P::SelModAmtBase].amt = p * p * p;
        ch.markDspDirty(); slotDirty = true;   // refreshDetailPanel pushes modFaders.setValues
        return;
    }
    if (t >= P::SelSlotPBase && t < P::SelSlotPBase + 8)
    {   // the N-th knob of the SELECTED slot's engine grid - mirrors the on-screen knob, so what
        // it controls follows the engine (FM Amount on Oscillator, Base Freq on Karplus-Strong...)
        auto params = slotParamsFor(sl.engine);
        const int i = t - P::SelSlotPBase;
        if (i < params.size())
        {
            auto& sp = params.getReference(i);
            double v2 = sp.min + (sp.max - sp.min) * (double) v;
            if (! sp.choices.isEmpty()) v2 = std::round(v2);
            sp.set(sl, v2);
            ch.markDspDirty(); slotDirty = true;
        }
        return;
    }
    switch (t)
    {
        case P::SelFxDrive: sl.fxDrive = v; break;
        case P::SelFxRev:   ch.reverbSend = v; slotDirty = true; break;   // channel sends now
        case P::SelFxDel:   ch.delaySend  = v; slotDirty = true; break;
        case P::SelFxSub:     sl.fxSub = v; break;
        case P::SelFxFormant: sl.fxFormant = v; break;
        case P::SelFxPunch: sl.fxPunch = v * 2.0f - 1.0f; break;
        case P::SelFxRing:  sl.fxRing = v; break;
        case P::SelFxRingHz: sl.fxRingHz = v < 0.02f ? 25.0f : 25.0f * std::pow(160.0f, v); break;   // 25..4000 log; hard left = Track
        case P::SelSlotPan:  sl.pan      = v * 2.0f - 1.0f; break;   // static slot pan L..R
        // CHANNEL FX (whole instrument, both slots) - these write the CHANNEL, not the slot.
        case P::SelChFxAmtA: ch.chFxAmt[0]  = v; slotDirty = true; break;
        case P::SelChFxChrA: ch.chFxChar[0] = v; slotDirty = true; break;
        case P::SelChFxAmtB: ch.chFxAmt[1]  = v; slotDirty = true; break;
        case P::SelChFxChrB: ch.chFxChar[1] = v; slotDirty = true; break;
        case P::SelChFxAmtC: ch.chFxAmt[2]  = v; slotDirty = true; break;
        case P::SelChFxChrC: ch.chFxChar[2] = v; slotDirty = true; break;
        case P::SelEnvA: case P::SelEnvH: case P::SelEnvD: case P::SelEnvS: case P::SelEnvR:
        {   // read the current 5 (Modal mirrors the Ring read-back), replace ONE, write via the
            // ONE apply path so the per-engine mapping stays identical to dragging the editor
            float a = sl.atk, h = sl.hold,
                  d = sl.engine == DrumChannel::SrcModal ? 0.05f + sl.modalDecay * 3.95f : sl.dec,
                  s = sl.sustain, r = sl.release;
            const float t2 = v * v;   // musical curve (bottom-weighted, like the editor's skew)
            if      (t == P::SelEnvA) a = t2 * 6.0f;
            else if (t == P::SelEnvH) h = t2 * 6.0f;
            else if (t == P::SelEnvD) d = juce::jmax(0.01f, t2 * 6.0f);
            else if (t == P::SelEnvS) s = v;
            else                      r = t2 * 4.0f;
            applyEnvToTargets(a, h, d, s, r);
            break;
        }
        case P::SelUniCount:
        {   // per-engine cap, mirroring setMaxUni (Modal 3 / KS 6 / Osc 16)
            const int cap = sl.engine == DrumChannel::SrcModal ? 3
                          : sl.engine == DrumChannel::SrcPhys  ? 6 : 16;
            sl.oscUnison = 1 + juce::roundToInt(v * (float) (cap - 1));
            break;
        }
        case P::SelUniDet:   sl.oscDetune = v; break;
        case P::SelUniVib:   sl.vibrato = v; break;
        case P::SelUniWidth: sl.uniSpread = v; break;
        case P::SelUniDrift: sl.drift = v; break;
        case P::SelStrum:   ch.strumAmt = std::round(v * 5.0f) / 5.0f; keysDirty = true; break;   // 0.2 steps = the knob's grid
        case P::SelMinVel:  ch.keysMinVel = v; if (ch.keysMaxVel < v) ch.keysMaxVel = v; keysDirty = true; break;
        case P::SelMaxVel:  ch.keysMaxVel = v; if (ch.keysMinVel > v) ch.keysMinVel = v; keysDirty = true; break;
        case P::SelGlide:   ch.keysGlide = v; keysDirty = true; break;
        case P::SelSlotOfs: ch.humanizeAmt = v; keysDirty = true; break;
        case P::SelRec:     keysPanel.btnRec.triggerClick(); return;
        // ScaleBox [2026-07-16]: the CC edits the SELECTED channel's slot = whichever chip (1/2)
        // is active in the box - "whatever pattern/channel/slot I have selected" (user spec).
        case P::SelScaleNotes:
        {
            auto& s2 = ch.slots[juce::jlimit(0, 1, keysPanel.scaleBox.slot)];
            if (! (s2.scaleOn && s2.scaleType >= 10))   // guitar voicings: string count is AUTO
                s2.scaleUnison = 1 + juce::roundToInt(v * 6.0f);
            ch.markDspDirty(); slotDirty = true; keysDirty = true; return;
        }
        case P::SelScaleType:
        {   // 0 = Off, 1..kNumScales = the scale list (mirrors the ScaleBox fader positions)
            auto& s2 = ch.slots[juce::jlimit(0, 1, keysPanel.scaleBox.slot)];
            const int t2 = juce::roundToInt(v * (float) kNumScales);
            s2.scaleOn = t2 > 0; if (t2 > 0) s2.scaleType = juce::jlimit(0, kNumScales - 1, t2 - 1);
            ch.markDspDirty(); slotDirty = true; keysDirty = true; return;
        }
        case P::SelScaleKey:
        {
            auto& s2 = ch.slots[juce::jlimit(0, 1, keysPanel.scaleBox.slot)];
            s2.scaleKey = juce::jlimit(0, 11, juce::roundToInt(v * 11.0f));
            ch.markDspDirty(); slotDirty = true; keysDirty = true; return;
        }
        // Title-strip controls [2026-07-15 22:30] - the real buttons fire so all side-effects run.
        case P::SelVolReset:     btnVolReset.triggerClick(); return;
        case P::SelKeysView:     btnKeysView.triggerClick(); return;
        case P::SelView16:       btn16View.triggerClick(); return;
        case P::SelEditorToggle: btnToggleDetail.triggerClick(); return;
        case P::SelOthersVol:
        {   // Others trim via CC: same relative gesture as the fader, but a CC has no "release" -
            // a >0.5 s pause in messages re-captures the base volumes (so a new sweep starts fresh).
            const juce::uint32 now = juce::Time::getMillisecondCounter();
            if (! othersVolActive || now - othersCcLastMs > 500)
            {
                othersVolActive = true;
                for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
                    othersVolBase[i] = proc.sequencer.channel(i).volume;
            }
            othersCcLastMs = now;
            const float g = std::pow(10.0f, (v - 0.5f) * 12.0f / 20.0f);   // +-6 dB
            for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
                if (i != selectedChannel)
                {
                    auto& c2 = proc.sequencer.channel(i);
                    c2.volume = juce::jlimit(0.0f, LevelMeter::VOL_MAX, othersVolBase[i] * g);
                    c2.markDspDirty();
                }
            othersVolF.setValue01(v);   // the pill mirrors the CC while it moves
            return;
        }
        // [2026-07-14 23:20] M/S/OV SET directly from the pad value (127 = on, 0 = off) - the old
        // triggerClick() only toggled on press (dead every other press on a toggle pad) and its
        // async button flash drew a faint half-state. The tick refresh syncs the button faces.
        case P::SelMute:    ch.mute = v >= 0.5f;         ch.markDspDirty(); return;
        case P::SelSolo:    ch.solo = v >= 0.5f;         ch.markDspDirty(); return;
        case P::SelOverlap: ch.allowOverlap = v >= 0.5f; return;
        case P::SelSlotSel: setShapeSlot(envTargetSlot() == 0 ? 1 : 0); return;
        case P::SelChNext:  selectChannel((selectedChannel + 1) % Sequencer::NUM_CHANNELS); return;
        case P::SelChPrev:  selectChannel((selectedChannel + Sequencer::NUM_CHANNELS - 1) % Sequencer::NUM_CHANNELS); return;
        case P::SelPatNext: selectPattern((currentPattern() + 1) % Sequencer::NUM_PATTERNS); return;
        case P::SelPatPrev: selectPattern((currentPattern() + Sequencer::NUM_PATTERNS - 1) % Sequencer::NUM_PATTERNS); return;
        case P::SelFollow:  btnFollow.triggerClick(); return;
        case P::SelTest:    proc.requestTestTrigger(selectedChannel); return;
        case P::SelChVol:   ch.volume = LevelMeter::posToGain(v); return;   // dB taper; handle follows next tick
        case P::SelSwing:   sliderSwing.setValue(sliderSwing.getMinimum()
                              + v * (sliderSwing.getMaximum() - sliderSwing.getMinimum()),
                              juce::sendNotificationSync); return;
        case P::SelBpm:     sliderBpm.setValue(sliderBpm.getMinimum()
                              + v * (sliderBpm.getMaximum() - sliderBpm.getMinimum()),
                              juce::sendNotificationSync); return;
        case P::SelUndo:    doUndo(); return;
        case P::SelRedo:    doRedo(); return;
        case P::SelSlotFreq:  sl.oscFreq = (float)(20.0 * std::pow(4186.0 / 20.0, (double) v)); break;   // log 20..4186 Hz
        case P::SelSlotFmAmt: sl.fmDepth = v; break;
        case P::SelSlotWarp:  sl.oscWarp = v; break;
        default: return;
    }
    ch.markDspDirty();
    if (t <= P::SelUniDrift) slotDirty = true;   // slot-level edits refresh the detail panel
}

// Step the SELECTED channel's sound through the bank in the PICKER's order (factory categories in
// kSoundCatOrder, then Your Sound Bank; wraps at the ends). Driven by the ui_sound_* MIDI CCs.
void DrumSequencerEditor::stepSoundBank(int dir)
{
    juce::Array<int> order;                                    // the full bank in PICKER order
    auto facNames = Factory::mixNames();
    auto facCats  = Factory::mixCategories();
    for (auto* cat : kSoundCatOrder)
        for (int i : factoryIndicesFor(cat, facNames, facCats, {}))
            order.add(FACTORY_MIX_BASE + i);
    for (int i = 0; i < soundMixFiles.size(); ++i) order.add(i + 1);   // Your Sound Bank (file ids = idx+1)
    if (order.isEmpty()) return;
    int pos = order.indexOf(currentSoundPickId(selectedChannel));
    pos = pos < 0 ? (dir > 0 ? 0 : order.size() - 1)           // Init / unknown: start at an end
                  : (pos + dir + order.size()) % order.size(); // wrap
    applySoundPickId(selectedChannel, order[pos]);
}

void DrumSequencerEditor::doUndo()
{
    // The passive timer commits a snapshot ~0.1 s AFTER each edit, so for a split-second the top
    // of the stack can be STALE (the pre-edit state) while the screen shows a newer change. Undoing
    // in that window used to skip a step. Force-commit any uncommitted current state FIRST, so the
    // top ALWAYS equals what's on screen and undo steps back exactly one edit. (No-op if already
    // committed; a real uncommitted change correctly clears redo, like any new edit.)
    commitUndoNow();
    if (undoStack.size() < 2) return;          // top is the current state; need a prior one
    redoStack.push_back(undoStack.back());
    undoStack.pop_back();
    applyUndoState(undoStack.back());
}

void DrumSequencerEditor::doRedo()
{
    if (redoStack.empty()) return;
    undoStack.push_back(redoStack.back());
    redoStack.pop_back();
    applyUndoState(undoStack.back());
}

void DrumSequencerEditor::updateUndoRedoEnabled()
{
    btnUndo.setEnabled(undoStack.size() >= 2);
    btnRedo.setEnabled(! redoStack.empty());
}

//==============================================================================
void DrumSequencerEditor::setupComponents()
{
    // Title is drawn as a logo in paintContent (titleLabel is hidden).
    titleLabel.setVisible(false);
    if (auto xml = juce::XmlDocument::parse(juce::String(juce::CharPointer_UTF8(kBasamakLogoSvg))))
        logoDrawable = juce::Drawable::createFromSVG(*xml);

    // DAW sync
    content.addAndMakeVisible(btnDawSync);
    btnDawSync.setClickingTogglesState(true);
    btnDawSync.setToggleState(proc.sequencer.dawSync, juce::dontSendNotification);
    btnDawSync.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff226688));
    btnDawSync.onClick = [this] {
        proc.sequencer.dawSync = btnDawSync.getToggleState();
        btnPlay.setEnabled(!proc.sequencer.dawSync);
        btnStop.setEnabled(!proc.sequencer.dawSync);
        sliderBpm.setEnabled(!proc.sequencer.dawSync);
    };

    content.addAndMakeVisible(btnPlay);
    btnPlay.setEnabled(!proc.sequencer.dawSync);
    btnPlay.onClick = [this] { proc.standalonePlay(); };
    btnPlay.getProperties().set("icon", "play"); btnPlay.setLookAndFeel(&iconBtnLNF);

    content.addAndMakeVisible(btnStop);
    btnStop.setEnabled(!proc.sequencer.dawSync);
    btnStop.onClick = [this] { proc.standaloneStop(); };
    btnStop.getProperties().set("icon", "stop"); btnStop.setLookAndFeel(&iconBtnLNF);

    content.addAndMakeVisible(lblBpm);
    lblBpm.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    lblBpm.setFont(juce::Font(12.0f)); lblBpm.setMinimumHorizontalScale(0.7f);   // so "BPM:" fits (no "BP...")

    content.addAndMakeVisible(lblMidiIn);
    lblMidiIn.setFont(juce::Font(11.0f, juce::Font::bold));
    lblMidiIn.setJustificationType(juce::Justification::centredLeft);
    lblMidiIn.setColour(juce::Label::textColourId, juce::Colour(0xff556688));
    lblMidiIn.setText("MIDI: waiting", juce::dontSendNotification);
    lblMidiIn.setTooltip("Live MIDI-in monitor: turns green and shows the last CC (number / value / "
                         "channel) when MIDI reaches the plugin.\n\n"
                         "If it stays grey while you move a control, MIDI isn't getting here yet - check:\n"
                         "- the track is record-armed (or input-monitoring) in your host\n"
                         "- your controller is enabled as a MIDI input device in the host\n"
                         "- you're moving a control that sends CC (some pads/keys send NOTES instead, "
                         "which toggle steps rather than being MIDI-learnable)");

    // Clickable version next to the logo -> opens the GitHub Releases page (check for updates).
    // ONE tall click-area (empty HyperlinkButton) covers the version + the "Check/Updates" caption; two
    // Labels on top (non-intercepting) draw the text at FIXED rows so they never overlap (a topLeft-
    // justified HyperlinkButton still vertically-centred its text -> it collided with the caption).
    content.addAndMakeVisible(verLink);
    verLink.setButtonText({});
    verLink.setURL(juce::URL("https://github.com/Kanebos9/BASAMAK/releases"));   // ALL releases (latest + history), not /latest
    verLink.setTooltip("BASAMAK v" BASAMAK_VERSION " - click to check for updates: opens GitHub Releases (the latest "
                       "version plus every previous release).\n\n"
                       "Installed a newer version but this still shows the old number? Rescan your plugins "
                       "in your DAW/host and reopen the project - the DAW may have cached the old build.");
    content.addAndMakeVisible(lblVersion);
    lblVersion.setText("v" BASAMAK_VERSION, juce::dontSendNotification);
    lblVersion.setFont(juce::Font(11.5f, juce::Font::bold));
    lblVersion.setJustificationType(juce::Justification::centred);   // share the caption's centre axis (tidy stack)
    lblVersion.setColour(juce::Label::textColourId, juce::Colour(0xffe8bf4d));   // brand gold
    lblVersion.setInterceptsMouseClicks(false, false);
    lblVersion.setBorderSize(juce::BorderSize<int>(0));
    content.addAndMakeVisible(lblCheckUpd);
    lblCheckUpd.setText("Check\nUpdates", juce::dontSendNotification);
    lblCheckUpd.setFont(juce::Font(8.5f, juce::Font::bold));
    lblCheckUpd.setJustificationType(juce::Justification::centredTop);
    lblCheckUpd.setColour(juce::Label::textColourId, juce::Colour(0xffb98a2e));   // dimmer gold (reads as part of the link)
    lblCheckUpd.setMinimumHorizontalScale(0.8f);
    lblCheckUpd.setInterceptsMouseClicks(false, false);
    lblCheckUpd.setBorderSize(juce::BorderSize<int>(0));

    // MIDI menu (dropdown): Save / load the current pattern's note grid + clear MIDI-learn.
    content.addAndMakeVisible(comboMidi);
    comboMidi.setTooltip("MIDI: save/load a MIDI-LEARN MAP - which CC controls which parameter (never their values).\n\n"
                         "- Saved as *.basamakpattern in your MIDI Patterns folder (refresh/open it here too).\n"
                         "- 'Clear MIDI' wipes all assignments.\n"
                         "- The 'Ch1 8x8 + M/S/OV + Play' factory map matches the TouchOSC file in your download.\n"
                         "- The 'Launchpad + Launchkey' factory map matches the custom controller modes included "
                         "in your download (the 'launchpad+launchkey custom midi modes' folder - load those .syx "
                         "modes onto a Launchkey MK4 / Launchpad Mini via Novation Components first).");
    rebuildMidiMenu();
    comboMidi.onChange = [this] { handleMidiMenuChange(); };

    content.addAndMakeVisible(btnUndo);
    btnUndo.setTooltip("Undo the last change (sounds, steps, FX, patterns - up to a couple dozen steps back).");
    btnUndo.onClick = [this] { doUndo(); };
    setWantsKeyboardFocus(true);   // so Cmd/Ctrl+Z reaches keyPressed (children bubble unconsumed keys up)
    btnUndo.getProperties().set("icon", "undo"); btnUndo.setLookAndFeel(&iconBtnLNF);
    content.addAndMakeVisible(btnRedo);
    btnRedo.setTooltip("Redo a change you just undid.");
    btnRedo.onClick = [this] { doRedo(); };
    btnRedo.getProperties().set("icon", "redo"); btnRedo.setLookAndFeel(&iconBtnLNF);
    btnUndo.setEnabled(false); btnRedo.setEnabled(false);

    content.addAndMakeVisible(sliderBpm);
    sliderBpm.setRange(40.0, 240.0, 0.5);
    sliderBpm.setValue(proc.sequencer.standaloneBpm, juce::dontSendNotification);
    sliderBpm.setSliderStyle(juce::Slider::LinearHorizontal);
    sliderBpm.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 20);
    sliderBpm.setEnabled(!proc.sequencer.dawSync);
    sliderBpm.onValueChange = [this] { proc.sequencer.standaloneBpm = (float)sliderBpm.getValue(); };

    content.addAndMakeVisible(lblSwing);
    lblSwing.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    lblSwing.setFont(juce::Font(10.0f));
    lblSwing.setJustificationType(juce::Justification::centred);
    lblSwing.setMinimumHorizontalScale(0.8f);
    content.addAndMakeVisible(sliderSwing);
    // Full classic (MPC-style) swing range: internal 0..1 -> the off-step of each pair lands at
    // 50%..75% of the pair (stepSpan: boundary = 0.5 + swing*0.25). Read-out uses the standard
    // 50-75% notation (50% = straight, 66% = triplet feel, 75% = maximum). Old projects stored
    // 0..0.7 and play EXACTLY as before - the boundary math is unchanged, only the cap/label moved.
    sliderSwing.setRange(0.0, 1.0, 0.01);
    sliderSwing.setValue(proc.sequencer.current().swing, juce::dontSendNotification);
    sliderSwing.setSliderStyle(juce::Slider::LinearHorizontal);   // the fader, stacked over its caption
    sliderSwing.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sliderSwing.setColour(juce::Slider::trackColourId,      juce::Colour(0xff35c0ff));   // filled LEFT of the thumb = blue (was dark-on-dark)
    sliderSwing.setColour(juce::Slider::thumbColourId,      juce::Colour(0xffd8e0f0));
    sliderSwing.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff26263c));
    sliderSwing.textFromValueFunction = [](double v){ return v < 0.005 ? juce::String("Off")
                                                             : juce::String(juce::roundToInt(50.0 + v * 25.0)) + "%"; };
    sliderSwing.onValueChange = [this] { proc.sequencer.current().swing = (float)sliderSwing.getValue();
                                         refreshSwingLabel(); };

    // Step-grid edit-mode radio buttons.
    content.addAndMakeVisible(lblEditMode);
    lblEditMode.setText("Edit:", juce::dontSendNotification);
    lblEditMode.setFont(juce::Font(11.0f, juce::Font::bold));
    lblEditMode.setColour(juce::Label::textColourId, juce::Colour(0xff7799cc));
    lblEditMode.setJustificationType(juce::Justification::centredRight);
    lblEditMode.setMinimumHorizontalScale(0.7f);   // squeeze "Edit:" rather than clip it ("Ed...") on wider fonts
    for (auto* b : { &btnModeVel, &btnModeLen, &btnModePitch, &btnModeProb, &btnModeRoll, &btnModePan, &btnModeNudge, &btnModeModA, &btnModeModB })
    {
        content.addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2a4a));
        b->setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    }
    btnModeModA.onClick  = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeModA  ? 0 : StepGridComponent::ModeModA);  };
    btnModeModB.onClick  = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeModB  ? 0 : StepGridComponent::ModeModB);  };
    btnModeModA.setTooltip("Step Mod A: draw a value (0-100%) per step. It's a MODULATION SOURCE - "
                           "route it in the MODULATION box (source \"Step Mod A\") to move any target in "
                           "time with the pattern. Separate from Vel/Pitch/Pan; drawing it changes nothing else.");
    btnModeModB.setTooltip("Step Mod B: a second drawable per-step modulation lane (route it as \"Step Mod B\").");
    btnModeVel.onClick   = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeVel   ? 0 : StepGridComponent::ModeVel);   };
    btnModeLen.onClick   = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeLen   ? 0 : StepGridComponent::ModeLen);   };
    btnModePitch.onClick = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModePitch ? 0 : StepGridComponent::ModePitch); };
    btnModeProb.onClick  = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeProb  ? 0 : StepGridComponent::ModeProb);  };
    btnModeRoll.onClick  = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeRoll  ? 0 : StepGridComponent::ModeRoll);  };
    btnModePan.onClick   = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModePan   ? 0 : StepGridComponent::ModePan);   };
    btnModeNudge.onClick = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeNudge ? 0 : StepGridComponent::ModeNudge); };
    // Make the edit-mode buttons MIDI-learnable (right-click). They drive UI state,
    // so the processor relays the CC back to the editor (see uiMidiEditMode).
    btnModeVel.midiLearn   = &proc.midiLearn; btnModeVel.paramId   = "ui_mode_vel";
    btnModeLen.midiLearn   = &proc.midiLearn; btnModeLen.paramId   = "ui_mode_len";
    btnModePitch.midiLearn = &proc.midiLearn; btnModePitch.paramId = "ui_mode_pitch";
    btnModeProb.midiLearn  = &proc.midiLearn; btnModeProb.paramId  = "ui_mode_prob";
    btnModeRoll.midiLearn  = &proc.midiLearn; btnModeRoll.paramId  = "ui_mode_roll";
    btnModePan.midiLearn   = &proc.midiLearn; btnModePan.paramId   = "ui_mode_pan";
    btnModeNudge.midiLearn = &proc.midiLearn; btnModeNudge.paramId = "ui_mode_nudge";
    btnModeVel.setTooltip("Velocity mode: drag a step up/down for how HARD that hit plays (0-100%).\n\n"
                          "- More than volume: a harder hit also sweeps a filter envelope further (303 accent).\n"
                          "- MIDI Out channels send it as real MIDI velocity.");
    btnModeLen.setTooltip("Gate mode (note length): drag a step LEFT/RIGHT to set how long the note is held - "
                          "the same idea as a piano-roll note's length.\n\n"
                          "- The note keeps its attack/punch, then its DECAY is stretched (or tightened) so the fall "
                          "fills exactly that much of the step - long notes ring down across their whole length (like "
                          "a synth/303 note), short notes get a tight gated feel. On a sound with SUSTAIN up, Length "
                          "HOLDS the note instead (the same gate a held key uses).\n"
                          "- 'Off' (default) = the sound's own natural length; for MIDI Out channels 'Off' = a "
                          "one-step note. Because it's a FRACTION of the step, changing the step count keeps the feel.\n"
                          "- MERGE (SHIFT+click a step) = a note LONGER than one step: the merged cells continue "
                          "the previous step's note (their own on/off state doesn't matter - they never re-fire). "
                          "On a merged chain, Gate = a fraction of the WHOLE chain (75% of 2 merged steps = 1.5 "
                          "steps). A long piano-roll note quantises to exactly this: a head step + merged cells.\n"
                          "- PIANO ROLL round trips use Gate too: a Gate step becomes a gated note of that length, "
                          "and a gated note's duration comes back into Gate (Gate-OFF notes = plain steps).\n\n"
                          "Double-click a step resets it to Off.");
    btnModePitch.setTooltip("Pitch mode: drag a step up/down to transpose that hit (semitones; centre = +0).\n\n"
                            "- The SLIDE band at each cell's bottom: click it and the step's pitch GLIDES across "
                            "the step into the NEXT step's pitch (303 portamento).\n"
                            "- Slide is only audible when the two steps have DIFFERENT pitches - draw the pitch "
                            "line first, then slide the notes that should flow together.\n\n"
                            "TIP: the glide is clearest with FEW steps + a long Gate (a slow bass line).");
    btnModeProb.setTooltip("Loop-condition mode: make a step fire only on certain passes of the loop.\n\n"
                           "- DRAG a step left/right = the cycle length (N bars).\n"
                           "- CLICK the bars = pick which passes it fires on (N=6, bars 3+6 = only the 3rd "
                           "and 6th time around). None picked = every pass.\n"
                           "- The step must also be ON in normal mode. Double-click resets.");
    btnModeRoll.setTooltip("Roll / ratchet mode: each step is a 2D cell.\n\n"
                           "- Drag UP/DOWN = how many times the step re-fires (1-6).\n"
                           "- Drag LEFT/RIGHT = the velocity RAMP across those hits: centre = flat, left = fade OUT, "
                           "right = build UP. The bars show the ramp shape.\n"
                           "- PIANO ROLL round trips keep rolls: switching to the roll expands a ratchet into its real "
                           "sub-notes (ramp included), and quantising back turns several same-pitch notes inside one "
                           "step into a roll again - count, ramp and loudness survive both ways.\n\n"
                           "Great for drum-roll fades, crescendo buzzes and stutters.");
    btnModePan.setTooltip("Pan edit mode: each step becomes a bipolar bar - drag LEFT/RIGHT to place that hit in the "
                          "stereo field (centre = middle). Per-step pan rides on top of the channel Pan. For built-in "
                          "sounds only (a MIDI-Out channel sends notes, not audio). Click again to leave.");
    btnModeNudge.setTooltip("Nudge mode (micro-timing): shift ONE hit off the grid line.\n\n"
                            "- Drag LEFT = fires EARLY (before the grid line), RIGHT = LATE - up to half a step; "
                            "the read-out is real milliseconds at the current tempo.\n"
                            "- Unlike Swing (one knob shifting every off-beat), Nudge moves one chosen hit: a "
                            "lazy snare, a rushed hat, a hand-humanized fill.\n"
                            "- Snaps to on-the-grid near centre; double-click resets.\n"
                            "- Rolls, MIDI-out notes and Drag-MIDI all follow the shifted timing.");

    // Time signature + bar-length calculator. X/Y editable.
    auto styleStatic = [this](juce::Label& l, const juce::String& t, float fs) {
        content.addAndMakeVisible(l); l.setText(t, juce::dontSendNotification);
        l.setFont(juce::Font(fs)); l.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        l.setJustificationType(juce::Justification::centredLeft);
    };
    styleStatic(lblBarPre, "TIME SIG", 9.0f);   // small label that sits ABOVE the X/Y numbers (saves width)
    lblBarPre.setJustificationType(juce::Justification::centred);
    styleStatic(lblBarSlash, "/", 14.0f);
    styleStatic(lblBarResult, "Bar length = 2.00 s", 13.0f);
    lblBarSlash.setJustificationType(juce::Justification::centred);
    auto styleEditable = [this](juce::Label& l) {
        content.addAndMakeVisible(l);
        l.setFont(juce::Font(14.0f, juce::Font::bold));
        l.setColour(juce::Label::textColourId, juce::Colour(0xffffd24a)); // yellow => editable
        l.setJustificationType(juce::Justification::centred);
        l.setEditable(true);
    };
    styleEditable(barSigX); styleEditable(barSigY);
    barSigX.onTextChange = [this] { barTimeSigX = juce::jlimit(1, 32, barSigX.getText().getIntValue());
                                    barSigX.setText(juce::String(barTimeSigX), juce::dontSendNotification);
                                    proc.sequencer.timeSigNum = barTimeSigX; updateBarLength(); };
    barSigY.onTextChange = [this] { int v = barSigY.getText().getIntValue(); v = (v==1||v==2||v==4||v==8||v==16) ? v : 4;
                                    barTimeSigY = v; barSigY.setText(juce::String(barTimeSigY), juce::dontSendNotification);
                                    proc.sequencer.timeSigDen = barTimeSigY; updateBarLength(); };
    updateBarLength();

    content.addAndMakeVisible(dragMidi);
    dragMidi.getMidiFile = [this] { return proc.exportMidiFile(selectedChannel); };
    dragMidi.setTooltip(
        "Drag this onto a DAW track to export the SELECTED channel as a MIDI clip.\n\n"
        "- Notes carry their VELOCITY and length; rolls become sub-hits; swing is kept; merged step "
        "chains come out as one long note.\n"
        "- Piano-roll channels export the roll exactly as drawn (C4-based, Base Freq independent). "
        "Step channels export with each pitched slot's Base Freq as the pitch 0-point.\n"
        "- Slots in Chord/Scale mode export their FULL voicing; both slots export together. Pure "
        "Sample/Noise channels export their step/draw pitch on the channel's own MIDI note.");


    // Preset menu
    content.addAndMakeVisible(lblPreset);
    lblPreset.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    content.addAndMakeVisible(comboPreset);
    comboPreset.onChange = [this] { handlePresetChange(); };

    // Pattern row
    content.addAndMakeVisible(lblPatterns);
    lblPatterns.setFont(juce::Font(11.0f, juce::Font::bold));
    lblPatterns.setColour(juce::Label::textColourId, juce::Colour(0xff7799cc));
    content.addAndMakeVisible(lblPatternsBars);
    lblPatternsBars.setFont(juce::Font(11.0f, juce::Font::bold));
    lblPatternsBars.setColour(juce::Label::textColourId, juce::Colour(0xff7799cc));
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        auto& pb = patternBtns[p];
        pb.index = p;
        pb.midiLearn = &proc.midiLearn;
        // Clicking a pattern is a manual choice -> stop auto-following so the view sticks
        // (and, while playing, it only changes the view; playback keeps going on its own).
        pb.onSelect = [this, p] { selectPattern(p); };   // Follow is a global toggle - clicking a pattern doesn't change it
        pb.onCopyFrom = [this, p] (int src) { copyPatternContent(src, p); selectPattern(p); };
        // SHIFT+CLICK = MERGE this pattern onto the previous one (toggle). Merged patterns play as ONE
        // multi-bar unit and mirror the HEAD's channel sounds (one sound editor, no clashing).
        pb.onShiftClick = [this, p] {
            if (p == 0) return;   // nothing before pattern 1 to merge with
            auto& sq = proc.sequencer;
            const bool turnOn = ! sq.patterns[p].mergeWithPrev;
            if (turnOn)
            {   // LIMITS so the merged view always FITS: max 8 bars, and - since a channel's step
                // count is UNIFORM across the group (merging normalizes every bar to the HEAD's
                // count, below) - the head's worst count x bars must fit the 64-cell row
                // (15 steps x 5 bars = 75 -> refused; max 12 steps for 5 bars).
                const int head = sq.groupHead(p - 1);
                const int pEnd = sq.groupEnd(p);          // p may already head its own run
                const int bars = pEnd - head + 1;
                int worst = 1;
                for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
                    worst = juce::jmax(worst, sq.patterns[head].channels[c].numSteps);
                if (bars > StepGridComponent::GRP_MAX || worst * bars > DrumChannel::MAX_STEPS)
                {
                    juce::PopupMenu mm;
                    mm.addSectionHeader(bars > StepGridComponent::GRP_MAX
                        ? "Can't merge: at most " + juce::String(StepGridComponent::GRP_MAX) + " patterns can be merged."
                        : "Can't merge: a channel has " + juce::String(worst) + " steps, and " + juce::String(bars)
                          + " bars of that need " + juce::String(worst * bars) + " cells - the row shows at most "
                          + juce::String(DrumChannel::MAX_STEPS) + ". Lower the step count first (max "
                          + juce::String(DrumChannel::MAX_STEPS / bars) + " steps for " + juce::String(bars) + " bars).");
                    mm.addItem(1, "OK");
                    mm.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&patternBtns[p]), [](int){});
                    return;
                }
            }
            if (! turnOn)   // UN-merge: free the pattern, and SPLIT any note that spanned the boundary
            {   // A note held across the (former internal) bar line was stored as one long note in the
                // earlier bar. Un-merged, that bar loops alone, so the note must END at the bar line and
                // RESTART at the next bar (user). Split it: truncate the head, add a gated continuation.
                commitUndoNow();
                const int RES = DrumChannel::DRAW_RES;
                auto& prev = sq.patterns[p - 1];   // the bar the spanning note lived in
                for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
                {
                    auto& pc = prev.channels[c]; auto& nc = sq.patterns[p].channels[c];
                    const int cnt = pc.drawNoteCount;
                    for (int i = 0; i < cnt; ++i)
                    {
                        auto& n = pc.drawNotes[i];
                        if (n.start + n.len > RES)   // spills past this bar
                        {
                            const int overflow = juce::jmin(RES, n.start + n.len - RES);
                            nc.addDrawNote(0, overflow, n.semi, n.vel, n.slot, 0, /*oneShot*/ 0,
                                           n.strumUp, n.strumPct == 255 ? -1 : n.strumPct, n.pan);
                            n.len = (int16_t) (RES - n.start);   // head ends at the bar line
                        }
                    }
                }
                sq.patterns[p].mergeWithPrev = false; selectPattern(p); refreshDetailPanel(); repaint(); return;
            }
            // MERGE: the group adopts the HEAD's sounds, step counts, roll/step MODE and Tune. Warn
            // first (user) - naming the channels whose mode will FLIP (roll<->step) in the later bars,
            // plus the reminder that every channel inherits the head's sound.
            // The head = the PREVIOUS pattern's group head (like the fit-check above): the clicked
            // pattern isn't merged YET, so groupHead(p) was just p = the dialog said "2-2" and the
            // merge normalized to the CLICKED bar's sounds instead of pattern 1's (user report).
            const int mHead = sq.groupHead(juce::jmax(0, p - 1)), mEnd = sq.groupEnd(p);
            juce::StringArray flips;
            for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c) {
                const bool headRoll = sq.patterns[mHead].channels[c].drawMode;
                for (int m = mHead + 1; m <= mEnd; ++m)
                    if (sq.patterns[m].channels[c].drawMode != headRoll)
                    { flips.add("Ch " + juce::String(c + 1) + " -> " + (headRoll ? "Piano Roll" : "steps")); break; }
            }
            auto doMerge = [this, p, mHead, mEnd]() {
                auto& sq2 = proc.sequencer;
                commitUndoNow();
                sq2.patterns[p].mergeWithPrev = true;
                for (int m = mHead + 1; m <= mEnd; ++m)
                    for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c) {
                        copyChannelSound(mHead, m, c);
                        auto& hc = sq2.patterns[mHead].channels[c]; auto& mc = sq2.patterns[m].channels[c];
                        mc.numSteps = hc.numSteps; mc.drawMode = hc.drawMode;
                        mc.drawTuneCents = hc.drawTuneCents;   // Tune uniform across the group (head's value)
                    }
                selectPattern(mHead); refreshDetailPanel(); repaint();
            };
            juce::PopupMenu wm;
            wm.addSectionHeader("Merge patterns " + juce::String(mHead + 1) + "-" + juce::String(mEnd + 1) + "?");
            if (! flips.isEmpty()) {
                wm.addSectionHeader("These channels differ and will switch to Pattern " + juce::String(mHead + 1) + "'s mode:");
                for (int i = 0; i < juce::jmin(6, flips.size()); ++i) wm.addSectionHeader("   " + flips[i]);
                if (flips.size() > 6) wm.addSectionHeader("   ...and " + juce::String(flips.size() - 6) + " more");
            }
            wm.addSectionHeader("All merged channels will use Pattern " + juce::String(mHead + 1) + "'s SOUND settings.");
            wm.addItem(1, "Merge");
            wm.addItem(2, "Cancel");
            wm.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&patternBtns[p]),
                             [doMerge](int r) { if (r == 1) doMerge(); });
        };
        pb.setTooltip("Pattern " + juce::String(p + 1) + " - click to view + edit it.\n\n"
                      "- DRAG onto another pattern = copy its steps, swing, play-mode + FX there.\n"
                      "- SHIFT+CLICK = MERGE with the pattern before it: merged patterns play back to back "
                      "as one longer piece (sounds + step counts mirror the FIRST bar; play mode/chain "
                      "come from the LAST). Shift+click again = un-merge.\n"
                      "- Right-click = MIDI-learn.");
        content.addAndMakeVisible(pb);
    }
    content.addAndMakeVisible(btnFollow);
    btnFollow.setClickingTogglesState(false);
    btnFollow.paramId = "ui_sel_follow"; btnFollow.midiLearn = &proc.midiLearn;   // right-click = learn
    btnFollow.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnFollow.setTooltip("Follow: when ON, the editor view jumps to whatever pattern is playing. "
                         "Turn it OFF (or just click a pattern) to look at one pattern while another keeps playing. "
                         "The playing pattern always shows a small green play triangle.");
    btnFollow.onClick = [this] {
        proc.followPlayback = ! proc.followPlayback;
        refreshFollowButton();
        if (proc.followPlayback) selectPattern(proc.sequencer.playPattern);   // snap the view to what's playing
    };
    refreshFollowButton();

    // [2026-07-14 01:50] CPU / RAM readout (user design: stacked small, right of DRAG MIDI).
    for (auto* l : { &lblCpu, &lblRam })
    {
        content.addAndMakeVisible(*l);
        l->setFont(juce::Font(9.5f, juce::Font::bold));
        l->setColour(juce::Label::textColourId, juce::Colour(0xff8fa0c0));
        l->setJustificationType(juce::Justification::centredLeft);
        l->setTooltip("Live resource readout.\n\n"
                      "- CPU first number = how much of the AUDIO TIME BUDGET this plugin uses (the "
                      "number that predicts crackles - near 100% = dropouts).\n"
                      "- CPU second number = the whole machine, all apps, averaged across cores. A low "
                      "total can still glitch if ONE core is pegged.\n"
                      "- RAM = this process's memory. In a DAW that means BASAMAK + the DAW together "
                      "(a plugin's own share can't be isolated); in the standalone it is just BASAMAK.");
    }
    lblCpu.setText("CPU --", juce::dontSendNotification);
    lblRam.setText("RAM --", juce::dontSendNotification);
    content.addAndMakeVisible(btnTooltips);
    btnTooltips.setLookAndFeel(&tinyBtnLNF);
    btnTooltips.setClickingTogglesState(false);
    btnTooltips.setTooltip("Tooltips: turn these hover help bubbles ON or OFF everywhere. On by default; "
                           "turn OFF once you know your way around.");
    btnTooltips.onClick = [this] { tooltipsOn = ! tooltipsOn; applyTooltipsSetting(); };
    applyTooltipsSetting();

    // INFLUENCE moved off the channel strips (they were too cramped to read the step-count dropdown)
    // to ONE button here, purple-outlined so it reads as a different KIND of action. It arms influence
    // for the SELECTED channel; then in a value edit mode you touch one step to copy it across the row.
    content.addAndMakeVisible(btnInfluenceTop);
    btnInfluenceTop.setClickingTogglesState(true);
    btnInfluenceTop.setLookAndFeel(&purpleOutlineLNF);
    btnInfluenceTop.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xff20203a));
    btnInfluenceTop.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb96bff));
    btnInfluenceTop.setTooltip(
        "Influence: copy ONE step's value onto the whole SELECTED channel.\n\n"
        "- Arm this, then in a value edit mode (Vel/Len/Pitch/Loop/Roll/Pan/Nudge) touch the step to "
        "copy FROM.\n"
        "- It un-arms after one use; re-arm to copy a different step.\n"
        "- In Pitch mode, touching a step's SLIDE strip copies just the slide flag.\n\n"
        "The purple outline marks it as a copy-across action, not per-step editing. Right-click to "
        "assign a MIDI control.");
    btnInfluenceTop.midiLearn = &proc.midiLearn;
    btnInfluenceTop.paramId   = "ui_influence";   // single UI control now (selected channel)
    btnInfluenceTop.onClick = [this] {
        stepGrid.influenceArmed[selectedChannel] = btnInfluenceTop.getToggleState();
    };
    content.addAndMakeVisible(btnClearPat);
    btnClearPat.setLookAndFeel(&tinyBtnLNF);
    btnClearPat.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a2030));
    btnClearPat.setTooltip("Clear the SELECTED channel in this pattern.\n\n"
                           "- Steps: disables them + resets every per-step value (vel, pan, pitch, loop, roll).\n"
                           "- Piano Roll: deletes all notes.\n"
                           "- Other channels untouched. Undoable.");
    btnClearPat.onClick = [this] {
        // Clear the SELECTED channel only - across EVERY bar of a merged group (the whole visible row).
        commitUndoNow();   // so this Clear is undoable even right after recording (user request)
        auto& sq = proc.sequencer;
        const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
        for (int b = head; b <= end; ++b)
        {
            auto& ch = sq.patterns[b].channels[selectedChannel];
            ch.clearStepData();
            // Piano-roll content too: wipe every note and reset the whole-channel default vel/pan.
            // (drawMode itself is left as-is.)
            ch.clearDrawNotes();
            ch.drawVel = 1.0f; ch.drawPan = 0.0f;
        }
        stepGrid.update(proc.sequencer, proc.anySolo);
        refreshPatternButtons();
        content.repaint();
    };

    // All 16 channels + 32 patterns are ALWAYS active (the old count toggles are gone). This button
    // (placed next to HIDE SOUND EDITOR/KEYS) switches the VIEW between 8 rows (default) and all 16.
    // [2026-07-15 19:45] OTHERS trim + VOL RESET (user design), left of the view toggle.
    content.addAndMakeVisible(othersVolF);
    othersVolF.mlm = &proc.midiLearn; othersVolF.learnPid = "ui_sel_others";   // right-click = learn [2026-07-15 22:30]
    othersVolF.setAccent(juce::Colour(0xff35c0ff));
    othersVolF.setLabel({}); othersVolF.setDefault(0.5f); othersVolF.setValue01(0.5f);
    othersVolF.format = [](float v) { const float db = (v - 0.5f) * 12.0f;
        return juce::String("Others ") + (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB"; };
    othersVolF.onChange = [this](float v) {
        if (ignoreKnobCallbacks) return;
        if (! othersVolActive)
        {   // capture the volumes at grab time - the whole drag is relative to THESE
            othersVolActive = true;
            for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
                othersVolBase[i] = proc.sequencer.channel(i).volume;
        }
        const float g = std::pow(10.0f, (v - 0.5f) * 12.0f / 20.0f);   // +-6 dB
        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
            if (i != selectedChannel)
            {
                auto& c = proc.sequencer.channel(i);
                c.volume = juce::jlimit(0.0f, LevelMeter::VOL_MAX, othersVolBase[i] * g);
                c.markDspDirty();
            }
    };
    othersVolF.onDragEnd = [this] { othersVolActive = false; othersVolF.setValue01(0.5f); };   // spring back
    othersVolF.setTooltip("Others: trim EVERY channel except the SELECTED one by up to +-6 dB "
        "(this pattern).\n\n"
        "- It EDITS their volume handles - watch them slide as you drag. Undo restores them.\n"
        "- Each drag is relative to where the volumes were when you grabbed it; the pill "
        "re-centres on release (it's a gesture, not a stored setting).\n"
        "- The quick way to make the selected sound stand out, or to pull it back into the mix.\n"
        "- Trimming UP can push the mix over full scale = clipping; the master LIMITER is the "
        "safeguard (it catches only the peaks that cross its ceiling).\n"
        "- Right-click = assign a MIDI knob (a >0.5 s pause in CC messages starts a fresh grab).");
    content.addAndMakeVisible(btnVolReset);
    btnVolReset.paramId = "ui_sel_volReset"; btnVolReset.midiLearn = &proc.midiLearn;   // right-click = learn
    btnVolReset.setLookAndFeel(&tinyBtnLNF);
    btnVolReset.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnVolReset.onClick = [this] {
        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        { auto& c = proc.sequencer.channel(i); c.volume = 1.0f; c.markDspDirty(); }
    };
    btnVolReset.setTooltip("Set every channel's volume handle in this pattern back to 100% (0 dB). Undoable.\n\nRight-click = assign a MIDI pad.");

    content.addAndMakeVisible(btn16View);
    btn16View.paramId = "ui_sel_view16"; btn16View.midiLearn = &proc.midiLearn;         // right-click = learn
    btn16View.setLookAndFeel(&tinyBtnLNF);
    btn16View.setClickingTogglesState(false);
    btn16View.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btn16View.setTooltip(
        "Show 8 or 16 channel rows. Opening 16 hides the sound editor/keys panel (they don't fit "
        "together); SHOW SOUND EDITOR/KEYS brings it back with 8 rows.\n\n"
        "All 16 channels are ALWAYS active - this only changes how many you SEE. Scroll the rest with "
        "the yellow bar or the mouse wheel over the strips.");
    btn16View.onClick = [this] {
        // The view toggle is about SEEING channel rows, so it always hides the sound editor/keys (both
        // 8 and 16 rows show with the editor down). Bringing the editor back is the Show Editor button's
        // job (it caps the view at 8 rows). This keeps the two buttons genuinely different (user).
        const int target = (visibleChannels == 16) ? 8 : 16;
        if (detailShown) btnToggleDetail.onClick();
        setVisibleChannels(target);
    };

    // Vertical scrollbar for the channel area - BRIGHT so new users notice there are more channels.
    content.addAndMakeVisible(channelBar);
    channelBar.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xffe8bf4d));
    channelBar.setColour(juce::ScrollBar::trackColourId, juce::Colour(0xff26263c));
    channelBar.setAutoHide(false);
    channelBar.addListener(this);
    // Horizontal scrollbar for the pattern row - BRIGHT so the extra patterns are discoverable.
    content.addAndMakeVisible(patternBar);
    patternBar.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xffe8bf4d));
    patternBar.setColour(juce::ScrollBar::trackColourId, juce::Colour(0xff26263c));
    patternBar.setAutoHide(false);
    patternBar.addListener(this);

    content.addAndMakeVisible(btnToggleDetail);
    btnToggleDetail.paramId = "ui_sel_editor"; btnToggleDetail.midiLearn = &proc.midiLearn;   // right-click = learn [2026-07-15 22:30]
    btnToggleDetail.setLookAndFeel(&tinyBtnLNF);   // smaller font so the text fits + reads
    btnToggleDetail.setClickingTogglesState(false);
    btnToggleDetail.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnToggleDetail.setTooltip("Show/hide the sound-editing panel (or the KEYS panel, whichever is open). Hide it to "
                               "give the step sequencer the whole window.");
    btnToggleDetail.onClick = [this] {
        detailShown = ! detailShown;
        btnToggleDetail.setButtonText(detailShown ? "HIDE SOUND EDITOR/KEYS" : "SHOW SOUND EDITOR/KEYS");
        btnToggleDetail.setColour(juce::TextButton::buttonColourId, detailShown ? juce::Colour(0xff20203a) : juce::Colour(0xff35c0ff));
        btnToggleDetail.setColour(juce::TextButton::textColourOffId, detailShown ? juce::Colours::lightgrey : juce::Colours::black);
        contentHeightPx = contentHeightFor(visibleChannels, detailShown);
        setResizeLimits(DESIGN_W / 2, contentHeightPx / 2, DESIGN_W * 2, contentHeightPx * 2);
        const double sc = juce::jmax(0.1, (double) getWidth() / (double) DESIGN_W);
        layoutContent();
        setSize(getWidth(), juce::roundToInt(contentHeightPx * sc));
        repaint();
    };

    content.addAndMakeVisible(btnAudition);
    btnAudition.setClickingTogglesState(false);
    btnAudition.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnAudition.setTooltip("Auto-audition: when ON, releasing a sound-slot knob/fader plays a TEST hit so you hear "
                           "the edit straight away. Turn OFF if you don't want a sound every time you tweak a knob.");
    btnAudition.onClick = [this] { proc.auditionOnEdit.store(! proc.auditionOnEdit.load()); refreshAuditionButton(); };
    refreshAuditionButton();

    // ==== KEYS view: the on-screen piano (radio with the sound editor) =======================
    content.addAndMakeVisible(btnKeysView);
    btnKeysView.paramId = "ui_sel_keysView"; btnKeysView.midiLearn = &proc.midiLearn;         // right-click = learn
    btnKeysView.setLookAndFeel(&tinyBtnLNF);
    btnKeysView.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnKeysView.setTooltip("Switch between the SOUND EDITOR and the on-screen KEYS piano.\n\n"
                           "- Keys play the SELECTED channel at the pressed pitch (Freq knobs are never changed).\n"
                           "- Every engine plays; 'Keep pitch' samples just don't transpose.\n"
                           "- REC records what you play into the channel as piano-roll notes.");
    btnKeysView.onClick = [this] { keysView = ! keysView; applyKeysView(); };

    content.addChildComponent(keysPanel);   // hidden until KEYS is opened
    keysPanel.onKeyDown = [this](int note, float vel) { proc.pushKeyDown(note, vel); };
    keysPanel.onKeyUp   = [this](int note) { proc.pushKeyUp(note); };
    keysPanel.btnRec.setTooltip(
        "Record what you play on the keys into the SELECTED channel (as piano-roll notes).\n\n"
        "- The mode box picks WHERE (this pattern only, or follow the chain) and WHEN (your first key, "
        "or a 3-second count-in).\n"
        "- Every pattern loop while recording becomes a TAKE - find them in the Takes list.\n"
        "- The pitch reference is fixed: C4 (middle C) = pitch 0.\n\n"
        "Press again to stop; the last take stays loaded on the channel.");
    keysPanel.btnRec.onClick = [this] {
        if (proc.keysRecording.load() || keysCountdownTicks > 0) { keysStopRecord(true); return; }
        // Recording writes into PIANO ROLL. If the armed channel (or its merge partner) is in step
        // mode WITH steps, warn - starting switches it to the roll and CLEARS those steps (user).
        auto& sq = proc.sequencer;
        const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
        auto stepDataIn = [&](int c) -> bool {
            if (c < 0) return false;
            for (int b = head; b <= end; ++b) { auto& cc = sq.patterns[b].channels[c];
                if (cc.drawMode) continue; for (int s = 0; s < DrumChannel::MAX_STEPS; ++s) if (cc.steps[s]) return true; }
            return false; };
        const int partner = sq.channel(selectedChannel).mergeWith;
        if (stepDataIn(selectedChannel) || stepDataIn(partner)) {
            juce::PopupMenu w;
            w.addSectionHeader("Record into Channel " + juce::String(selectedChannel + 1) + "?");
            w.addSectionHeader("It's in step mode - recording switches it to Piano Roll and CLEARS its steps (you can Undo).");
            w.addItem(1, "Clear steps and record");
            w.addItem(2, "Cancel");
            w.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&keysPanel.btnRec),
                [this](int r) { if (r == 1) { commitUndoNow(); keysStartRecord(); } });
            return;
        }
        keysStartRecord();
    };
    keysPanel.comboRecMode.addItem("This pattern only - key starts",  1);
    keysPanel.comboRecMode.addItem("This pattern only - 3s count-in", 2);
    keysPanel.comboRecMode.addItem("Follow chain, record each - key", 3);
    keysPanel.comboRecMode.addItem("Follow chain, record each - 3s",  4);
    keysPanel.comboRecMode.setSelectedId(1, juce::dontSendNotification);
    keysPanel.comboRecMode.setTooltip("WHERE recording writes:\n"
                                      "- This pattern only: playback LOOPS the pattern you're on for the whole recording "
                                      "(chains are ignored until you stop) and each loop = one take.\n"
                                      "- Follow chain, record each: the chain runs and you record into EVERY pattern it "
                                      "visits - each pattern+channel keeps its own separate takes.\n"
                                      "'key' = recording starts on your first key press; '3s' = a 3-second count-in.");
    // Slot-2 pitch: a 3-COLUMN popup (no scrolling) covering the full -24..+24 st transpose. + = UP,
    // - = DOWN. Stored as keysSlot2Down (positive = DOWN, preserving old projects), so trans = -down.
    keysPanel.btnSlot2.setTooltip("Slot 2 pitch: transpose slot 2 relative to the key you press (-24..+24 "
                                  "semitones; slot 1 always plays the pressed pitch). -12 = a classic sub-oscillator "
                                  "an octave down, +12 = an octave up. 0 = both slots at the pressed note.");
    keysPanel.btnSlot2.onClick = [this] {
        juce::PopupMenu m;
        const int cur = -proc.sequencer.channel(selectedChannel).keysSlot2Down;   // musical transpose (+ up)
        for (int t = 24; t >= -24; --t) {
            const juce::String lab = t == 0 ? "0 st" : (t > 0 ? "+" + juce::String(t) + " st"
                                                              : juce::String(t) + " st");
            m.addItem(t + 100, lab, true, t == cur);   // id = transpose + 100 (never 0)
        }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&keysPanel.btnSlot2).withMinimumNumColumns(3),
            [this](int r) {
                if (r == 0) return;
                const int down = -(r - 100);   // stored value is "down" (positive = down)
                proc.sequencer.channel(selectedChannel).keysSlot2Down = down;
                proc.keysSlot2Down.store(down);
                refreshKeysPanel();            // update the button label + the key highlight
            });
    };
    // HUMANIZE / STRUM knobs (per pattern/channel feel). Both greyed + untouchable when they'd do
    // nothing (Humanize needs 2 audible slots; Strum needs a slot in Chord or Scale mode).
    keysPanel.humanKnob.setTooltip("SLOT OFFSET (0-100 ms, needs 2 sound slots): fires SLOT 2 a fixed amount "
        "AFTER slot 1 on every hit - a consistent flam/layering delay (thicken a stack, add a soft doubling).\n\n"
        "- Slot 1 stays exactly on time; slot 2 trails by the set milliseconds.\n"
        "- The SAME every hit (not random). 0 = both slots together. Faded when only one slot is used.");
    keysPanel.strumKnob.setTooltip("STRUM (needs Chord or Scale on): spreads a chord's notes in time, low to high, "
        "like strumming - every hit (keys, steps or drawn) fans the chord out instead of a block. Stepped in fixed "
        "amounts (0/20/40/60/80/100%) so it matches the piano-roll right-click override exactly (no drift between "
        "live playing and a recorded note). 0 = all notes together. Faded when neither slot is in Chord or Scale "
        "mode. Works on Oscillator, Modal and Physical.\n\n"
        "- Per note (in the piano roll): right-click a note for Strum UP/DOWN + a per-note amount. Those options "
        "only apply to notes on the SAME pitch and SAME length (a chord voiced in Scale mode) - they fade otherwise.");
    keysPanel.strumKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffb46bff));   // violet (chord/scale hue)
    keysPanel.humanKnob.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        proc.sequencer.channel(selectedChannel).humanizeAmt = (float) keysPanel.humanKnob.getValue();
    };
    keysPanel.strumKnob.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        proc.sequencer.channel(selectedChannel).strumAmt = (float) keysPanel.strumKnob.getValue();
    };
    keysPanel.minVelKnob.setTooltip("MIN VELOCITY: the softest a played key can sound. Your key velocity is remapped "
        "so the quietest press = this level and the hardest = full. Turn up if soft playing (or a light "
        "controller) gets too quiet or drops out. 0 = off (raw velocity).");
    keysPanel.minVelKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff5ad17a));
    keysPanel.minVelKnob.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.keysMinVel = (float) keysPanel.minVelKnob.getValue();
        if (ch.keysMaxVel < ch.keysMinVel) {   // Min pushes Max up (they can't cross)
            ch.keysMaxVel = ch.keysMinVel;
            keysPanel.maxVelKnob.setValue(ch.keysMaxVel, juce::dontSendNotification);
        }
    };
    keysPanel.maxVelKnob.setTooltip("MAX VELOCITY: the loudest a played key can sound. Your key velocity is remapped "
        "so the hardest press = this level. Turn down to tame a heavy hand or an aggressive controller. "
        "1 = off (full). Min/Max together map [soft..hard] -> [min..max].");
    keysPanel.maxVelKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff5ad17a));
    keysPanel.glideKnob.setTooltip(
        "GLIDE: pitch slide time between notes (up to ~0.4 s; 0% = no slide). Works in EVERY "
        "play mode:\n\n"
        "- Only OVERLAPPING presses slide (fingered portamento) - play detached = no slide.\n"
        "- Mono modes slide from the held note; Poly modes from the last played note.\n"
        "- Whether the ENVELOPE restarts or continues is the Mode dropdown's job, not this knob's.\n\n"
        "Live playing only - recorded notes use the piano roll's per-note glide flag instead "
        "(CMD/CTRL+click a note).");
    keysPanel.glideKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff35c0ff));   // cyan = pitch/glide
    keysPanel.glideKnob.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        proc.sequencer.channel(selectedChannel).keysGlide = (float) keysPanel.glideKnob.getValue();
    };
    keysPanel.maxVelKnob.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.keysMaxVel = (float) keysPanel.maxVelKnob.getValue();
        if (ch.keysMinVel > ch.keysMaxVel) {   // Max pushes Min down (they can't cross)
            ch.keysMinVel = ch.keysMaxVel;
            keysPanel.minVelKnob.setValue(ch.keysMinVel, juce::dontSendNotification);
        }
    };
    // [2026-07-16] PLAY MODE dropdown (replaced the Poly toggle): 4 modes, per-sound.
    keysPanel.btnPlayMode.setTooltip(
        "Play mode (per sound). The mode decides ONE thing: does an overlapping press RESTART the "
        "envelope, or CONTINUE it (Legato)? Pitch sliding is the separate GLIDE knob (works in "
        "every mode).\n\n"
        "- Poly: held keys stack like a piano; every note gets its own attack.\n"
        "- Poly Legato: notes played while others are held CONTINUE the phrase's envelope (no "
        "re-attack) - a slow-attack pad swells ONCE under a whole held phrase. Fresh chords from "
        "silence still attack normally.\n"
        "- Mono: a new key cuts the previous one; the envelope restarts each note.\n"
        "- Mono Legato: overlapping presses retune the sounding note - the envelope rides on.\n\n"
        "Mono modes do NOT block Chord/Scale - one key still plays its full voicing. Keyboard only; "
        "the sequencer's per-channel OV button is separate.");
    keysPanel.btnPlayMode.onClick = [this] {
        auto& c0 = proc.sequencer.channel(selectedChannel);
        const int cur = (c0.keysPolyMode ? 0 : 2) + (c0.keysLegato ? 1 : 0);
        juce::PopupMenu m;
        m.addItem(1, "Poly",        true, cur == 0);
        m.addItem(2, "Poly Legato", true, cur == 1);
        m.addItem(3, "Mono",        true, cur == 2);
        m.addItem(4, "Mono Legato", true, cur == 3);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&keysPanel.btnPlayMode),
            [this](int r) {
                if (r == 0) return;
                auto& c = proc.sequencer.channel(selectedChannel);
                c.keysPolyMode = (r <= 2);
                c.keysLegato   = (r == 2 || r == 4);
                keysPanel.polyMode = c.keysPolyMode;   // routes the panel's note-offs (per-sound)
                refreshKeysPanel();                    // button text + Glide enable follow
            });
    };
    keysPanel.scaleBox.mlm = &proc.midiLearn;   // right-click a ScaleBox control = MIDI-learn [2026-07-16]
    // [2026-07-16] ONE slot selection everywhere: the ScaleBox chip drives setShapeSlot (which
    // syncs the sound editor's selectors, filter/env targets and, via refreshKeysPanel, this chip).
    keysPanel.scaleBox.onSlotSelect = [this](int i) { if (! ignoreKnobCallbacks) setShapeSlot(i); };
    keysPanel.btnSlot2.setLookAndFeel(&dropBtnLNF);   // dropdown look (triangle) - it's a popup button now
    keysPanel.btnArp.setLookAndFeel(&dropBtnLNF);     // same: the Arp button opens a dropdown
    keysPanel.btnGuide.setLookAndFeel(&dropBtnLNF);   // Key-guide popup button
    keysPanel.btnGuide.setTooltip("KEY GUIDE (display only): dims the keys OUTSIDE a scale. Lit keys = the "
        "notes of the scale - ALL of them, not just chord roots.\n\n"
        "- MELODIES: stay on lit keys and you stay in key.\n"
        "- CHORDS BY HAND (Poly on): press several lit keys together. Classic recipe: press a lit key, SKIP "
        "the next lit one, press the one after, skip, press again - root / third / fifth, that key's chord "
        "in the scale.\n"
        "- If a slot's SCALE (the box above the keys) is on, one key already plays a full chord - the lit "
        "keys are then the valid ROOTS, and a dimmed key snaps to the nearest lit one.");
    keysPanel.btnGuide.onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Off",                        true, proc.kbGuideMode == 0);
        m.addSeparator();
        for (int k = 0; k < 12; ++k)
        {
            juce::PopupMenu sub;
            for (int t = 0; t < kNumScales; ++t)
                sub.addItem(1000 + k * 16 + t, kUiScaleName[t], true,
                            proc.kbGuideMode == 2 && proc.kbGuideKey == k && proc.kbGuideScale == t);
            m.addSubMenu(juce::String(kUiNoteName[k]), sub);   // (the "..." suffix confused users)
        }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&keysPanel.btnGuide),
            [this](int r) {
                if (r == 0) return;
                if (r == 1)      proc.kbGuideMode = 0;
                else { proc.kbGuideMode = 2; proc.kbGuideKey = (r - 1000) / 16; proc.kbGuideScale = (r - 1000) % 16; }
                updateKeyboardGuide();
            });
    };
    keysPanel.btnTakes.setLookAndFeel(&dropBtnLNF);   // proper dropdown look (like the sound bank)
    keysPanel.btnTakes.setTooltip(
        "Recorded takes for the CURRENT pattern + selected channel. One pattern loop while recording = "
        "one take (max 20 per pattern+channel, 1000 per preset; saved with the preset).\n\n"
        "- CLICK a take to LOAD it onto the channel (see it and play it).\n"
        "- Deleting lives in the 'Delete a take' submenu.\n"
        "- Edited a loaded take? 'Save changes' options appear at the top of this menu.");
    keysPanel.btnTakes.onClick = [this] {
        juce::PopupMenu m, delSub;
        int shown = 0;
        // Save options for the loaded take, only when it's been hand-edited (dirty).
        const bool haveDirty = keysLoadedTakeIdx >= 0 && keysLoadedTakeIdx < (int) proc.keysTakes.size()
                               && keysTakeDirty(keysLoadedTakeIdx);
        if (haveDirty) { m.addItem(600, "Save edits to the loaded take");
                         m.addItem(601, "Save edits as a NEW take"); m.addSeparator(); }
        for (int i = 0; i < (int) proc.keysTakes.size(); ++i)
        {
            const auto& t = proc.keysTakes[(size_t) i];
            if (t.channel != selectedChannel || takePatternOf(t) != currentPattern()) continue;   // per PATTERN + channel
            ++shown;
            const juce::String kind = t.isDraw ? "draw" : (juce::String((int) t.evts.size()) + " notes");
            const bool loaded = (i == keysLoadedTakeIdx);
            // CLICKING the take LOADS it (no more Load/Delete sub-dropdown). A tick marks the loaded one.
            m.addItem(1000 + i, (loaded ? juce::String::fromUTF8("\xE2\x97\x8F ") : "") + t.name + "  (" + kind
                                + (loaded && keysTakeDirty(i) ? ", edited*" : "") + ")", true, false);
            delSub.addItem(2000 + i, t.name + "  (" + kind + ")");
        }
        const juce::String where = "Pattern " + juce::String(currentPattern() + 1)
                                   + " Channel " + juce::String(selectedChannel + 1);
        if (shown == 0) m.addItem(-1, "(no takes in " + where + " yet)", false);
        else { m.addSeparator();
               delSub.addSeparator(); delSub.addItem(500, "Delete ALL in " + where);
               m.addSubMenu("Delete a take", delSub); }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&keysPanel.btnTakes),
                        [this](int r) {
                            if (r <= 0) return;
                            if (r == 500)   // delete every take in this pattern+channel
                            {
                                auto& v = proc.keysTakes;
                                v.erase(std::remove_if(v.begin(), v.end(),
                                            [this](const DrumSequencerProcessor::KeysTake& t)
                                            { return t.channel == selectedChannel && takePatternOf(t) == currentPattern(); }),
                                        v.end());
                                keysLoadedTakeIdx = -1;   // indices shifted
                                refreshKeysPanel();
                                return;
                            }
                            if (r == 600 || r == 601)   // save edits of the LOADED take (overwrite / as new)
                            {
                                const int idx = keysLoadedTakeIdx;
                                if (idx < 0 || idx >= (int) proc.keysTakes.size()) return;
                                const auto& ot = proc.keysTakes[(size_t) idx];
                                const int pat = ot.isDraw ? ot.drawPat : currentPattern();
                                auto nt = captureTakeFromChannel(ot.channel, pat);
                                if (r == 600) { nt.name = ot.name; proc.keysTakes[(size_t) idx] = std::move(nt);
                                                keysLoadedTakeHash = takeDataHash(proc.keysTakes[(size_t) idx]); }
                                else if (takesForPatChan(takePatternOf(ot), ot.channel) < 20
                                         && (int) proc.keysTakes.size() < DrumSequencerProcessor::KEYS_TAKES_MAX) {
                                    nt.name = juce::Time::getCurrentTime().toString(true, true, true, true)
                                              + "  #" + juce::String((int) proc.keysTakes.size() + 1);
                                    proc.keysTakes.push_back(std::move(nt));
                                    keysLoadedTakeIdx = (int) proc.keysTakes.size() - 1;
                                    keysLoadedTakeHash = takeDataHash(proc.keysTakes.back());
                                }
                                refreshKeysPanel();
                                return;
                            }
                            if (r >= 2000) { keysDeleteTake(r - 2000); return; }   // Delete-a-take submenu
                            if (r >= 1000) { keysLoadTake(r - 1000); return; }     // CLICK a take = LOAD it
                        });
    };

    // Per-pattern play options (apply to the current pattern)
    content.addAndMakeVisible(patModeBtn);
    patModeBtn.setLookAndFeel(&dropBtnLNF);   // draws a clean down-triangle on the right
    patModeBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    patModeBtn.onClick = [this] {
        // Merged group: edit the LAST bar's settings (the bar playback leaves from).
        auto& p = proc.sequencer.patterns[proc.sequencer.groupEnd(currentPattern())];
        juce::String inf = juce::String(juce::CharPointer_UTF8("\xE2\x88\x9E"));
        juce::PopupMenu m;
        m.addItem(1, "Loop " + inf, true, p.playMode == Sequencer::LoopForever);
        m.addItem(2, "Stop after... (" + juce::String(p.repeatTarget) + ")", true, p.playMode == Sequencer::StopAfterN);

        // CHAIN: each entry is (pattern, loops). Pick a pattern -> a dialog asks how many loops (type any number) ->
        // appended. At play time the pattern plays that many loops, then jumps to that pattern, advancing the chain.
        juce::PopupMenu chainAdd;
        for (int i = 0; i < visiblePatterns; ++i) chainAdd.addItem(220000 + i, "Pattern " + juce::String(i + 1));
        m.addSubMenu("Chain: add pattern", chainAdd, p.chainLen < Sequencer::CHAIN_MAX);
        if (p.chainLen > 0) {
            juce::String cs;
            for (int k = 0; k < p.chainLen; ++k)
                cs += "P" + juce::String(p.chainSeq[k] + 1) + "(" + juce::String(p.chainLoops[k])
                      + ")" + (k < p.chainLen - 1 ? " > " : "");
            m.addSectionHeader("Chain: " + cs);
            m.addItem(3, "Delete last chain");
        }

        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&patModeBtn),
            [this](int r) {
                if (r <= 0) return;
                auto& pp = proc.sequencer.patterns[proc.sequencer.groupEnd(currentPattern())];
                if      (r == 1)        { pp.playMode = Sequencer::LoopForever; refreshPatternOptions(); }
                else if (r == 2)        askLoopCount("Stop after", pp.repeatTarget, [this](int n) {
                                            auto& q = proc.sequencer.patterns[proc.sequencer.groupEnd(currentPattern())];
                                            q.playMode = Sequencer::StopAfterN; q.repeatTarget = n; refreshPatternOptions(); });
                else if (r == 3)        { if (pp.chainLen > 0) --pp.chainLen;          // delete the LAST chain entry
                                          if (pp.chainLen == 0) pp.playMode = Sequencer::LoopForever; refreshPatternOptions(); }
                else if (r >= 220000)   { const int pat = r - 220000;
                                          askLoopCount("Play Pattern " + juce::String(pat + 1) + " after how many loops", 2,
                                            [this, pat](int n) {
                                                auto& q = proc.sequencer.patterns[proc.sequencer.groupEnd(currentPattern())];
                                                if (q.chainLen < Sequencer::CHAIN_MAX) {
                                                    q.chainSeq[q.chainLen] = pat; q.chainLoops[q.chainLen] = n;
                                                    ++q.chainLen; q.playMode = Sequencer::Chain; }
                                                refreshPatternOptions(); }); }
            });
    };


    // Step grid
    content.addAndMakeVisible(stepGrid);
    // Magnifier overlay: added AFTER the grid + all top-bar/strip controls exist so it sits on
    // TOP of them (toFront in layout keeps it there); mouse-transparent, so it never steals input.
    content.addAndMakeVisible(stepMagOverlay);
    content.addAndMakeVisible(countdownOverlay);   // big 3-2-1-GO! count-in, over everything
    stepMagOverlay.grid = &stepGrid;
    stepGrid.magOverlay = &stepMagOverlay;
    stepGrid.midiLearn = &proc.midiLearn;
    stepGrid.onStepClicked     = [this](int ch, int step) {
        if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
        int ls = step; auto& c = groupStepChannel(ch, ls);   // merged group: concat -> the right bar
        if (ls >= 0 && ls < DrumChannel::MAX_STEPS) c.steps[ls] = ! c.steps[ls];
    };
    stepGrid.onChannelSelected = [this](int ch) { selectChannel(ch); };
    stepGrid.onStepValueChanged = [this](int ch, int step, int mode, float value) {
        if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
        const int concat = step;
        int ls = step; auto& c = groupStepChannel(ch, ls);   // merged group: concat -> the right bar
        if (ls < 0 || ls >= DrumChannel::MAX_STEPS) return;
        if (mode == StepGridComponent::ModeVel)        c.stepVel[ls] = value;      // Y = velocity
        else if (mode == StepGridComponent::ModeLen)   c.stepNoteLen[ls] = value;   // X = gate length (all channels)
        else if (mode == StepGridComponent::ModePitch) c.stepPitch[ls] = value;
        else if (mode == StepGridComponent::ModePan)   c.stepPan[ls]   = value;   // X = pan -1..+1
        else if (mode == StepGridComponent::ModeNudge) c.stepNudge[ls] = value;   // X = micro-timing -1..+1
        else if (mode == StepGridComponent::ModeModA)  c.stepModA[ls]  = value;   // Y = step mod lane A 0..1
        else if (mode == StepGridComponent::ModeModB)  c.stepModB[ls]  = value;
        else if (mode == StepGridComponent::ModeRoll) {
            c.stepRoll[ls]      = juce::jlimit(1, 6, (int) value);       // Y = ratchet count
            c.stepRollDecay[ls] = stepGrid.getRollDec(ch, concat);       // X = per-hit ramp (grid mirrors concat)
        }
    };
    stepGrid.onStepSlideChanged = [this](int ch, int step, bool on) {
        if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
        int ls = step; auto& c = groupStepChannel(ch, ls);
        if (ls >= 0 && ls < DrumChannel::MAX_STEPS) c.stepSlide[ls] = on;
    };
    stepGrid.onStepMergeChanged = [this](int ch, int step, bool on) {
        if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
        int ls = step; auto& c = groupStepChannel(ch, ls);
        if (ls >= 0 && ls < DrumChannel::MAX_STEPS) c.stepMerge[ls] = on;
    };
    // PIANO-ROLL snap grid: type any value 1-64 (0 = off). Reuses the non-blocking AlertWindow
    // pattern (askLoopCount) - a modal message box would hang the standalone.
    // GHOST LINES: the actual pitches slot 'slot' sounds for a drawn note (chord/scale voicing +
    // slot-2 transpose), from the REAL channel data - same source as the keyboard highlight.
    stepGrid.getSlotVoicing = [this](int ch, int slot, int semi, int* out) -> int {
        const auto& c  = proc.sequencer.patterns[proc.sequencer.currentPattern].channels[ch];
        const auto& sl = c.slots[juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, slot)];
        const bool pitched = sl.engine == DrumChannel::SrcOsc || sl.engine == DrumChannel::SrcModal
                          || sl.engine == DrumChannel::SrcPhys;
        if (! pitched || sl.weight <= 0.001f) return 0;
        const int down = (slot == 1) ? c.keysSlot2Down : 0;   // positive = semitones DOWN (baked into slot 2's Freq)
        const int base = semi - down;
        int n = 0;
        if (sl.scaleOn)
        {
            const int nv = juce::jlimit(1, 8, sl.scaleType >= 10 ? 6 : sl.scaleUnison);   // guitar = AUTO
            for (int k = 0; k < nv && n < 8; ++k)
            {
                const int off = DrumChannel::scaleNoteOffset(sl.scaleType, sl.scaleKey, 60 + base, k);
                if (off < -90) continue;   // guitar voicing: missing string
                out[n++] = base + off;
            }
        }
        else if (sl.chordMode > 0)
        {
            const int nv = juce::jlimit(1, 8, sl.chordUnison);
            for (int k = 0; k < nv && n < 8; ++k)
                out[n++] = base + DrumChannel::chordNoteOffset(sl.chordMode, k);
        }
        else out[n++] = base;   // plain slot: one line (differs from the drawn bar only when transposed)
        return n;
    };
    // AUDITION WHILE DRAWING: hear the note you're creating/dragging in the roll, played through the
    // normal keys path (so slot-2/chord/scale voicing is what you hear). Skipped while recording (the
    // live keys are the monitor) and when the ARP owns the keyboard.
    stepGrid.onRollPreview = [this](int semi) {
        auto& c = proc.sequencer.channel(selectedChannel);
        if (proc.keysRecording.load() || c.arpOn || ! c.drawMode) return;
        const int note = juce::jlimit(0, 127, 60 + semi);
        if (note == rollPreviewNote) return;
        if (rollPreviewNote >= 0) proc.pushKeyUp(rollPreviewNote);
        proc.pushKeyDown(note, 0.8f);
        rollPreviewNote = note;
    };
    stepGrid.onRollPreviewEnd = [this] {
        if (rollPreviewNote >= 0) { proc.pushKeyUp(rollPreviewNote); rollPreviewNote = -1; }
    };
    // MERGE&SPLIT box: the two 4-octave WINDOWS write to the pair's FIRST channel across ALL patterns
    // (pairing is channel-wide, like routing). The big button opens the merge/un-merge popup.
    keysPanel.splitBox.onChange = [this] {
        if (ignoreKnobCallbacks) return;
        const int p0 = proc.sequencer.channel(selectedChannel).mergeWith;
        const int first = (p0 >= 0) ? juce::jmin(selectedChannel, p0) : selectedChannel;
        for (auto& pat : proc.sequencer.patterns)
        {
            pat.channels[first].keysSplitW1 = juce::jlimit(12, 60, keysPanel.splitBox.w1);
            pat.channels[first].keysSplitW2 = juce::jlimit(12, 60, keysPanel.splitBox.w2);
        }
        keysHighlightMaskLo = keysHighlightMaskHi = ~0ULL;   // mapping changed -> recompute highlight/names
    };
    keysPanel.splitBox.onMergeClick = [this] {
        const int ch = selectedChannel;
        const int cur = proc.sequencer.channel(ch).mergeWith;
        juce::PopupMenu m;
        if (cur >= 0)
            m.addItem(3, "Un-merge channels " + juce::String(juce::jmin(ch, cur) + 1) + " & "
                          + juce::String(juce::jmax(ch, cur) + 1));
        else
        {
            m.addItem(1, "Merge & Split with PREVIOUS (channel " + juce::String(ch) + ")", ch > 0);
            m.addItem(2, "Merge & Split with NEXT (channel " + juce::String(ch + 2) + ")",
                      ch < Sequencer::NUM_CHANNELS - 1);
        }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&keysPanel.splitBox),
            [this, ch, cur](int r) {
                if (r == 1) setChannelMerge(ch, ch - 1);
                else if (r == 2) setChannelMerge(ch, ch + 1);
                else if (r == 3) setChannelMerge(ch, cur);   // toggles OFF
            });
    };
    // SCALE box: write the edited slot's harmonizer fields straight onto the channel (they're part of
    // the SOUND - channelSoundHash picks them up), then refresh so the guide/highlight/detail follow.
    keysPanel.scaleBox.onChange = [this](int si) {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[juce::jlimit(0, 1, si)];
        const bool wasVoiced = sl.scaleOn || sl.chordMode > 0;
        const auto& nv = keysPanel.scaleBox.v[juce::jlimit(0, 1, si)];
        sl.scaleOn = nv.on; sl.scaleType = nv.type; sl.scaleKey = nv.key; sl.scaleUnison = nv.count;
        // FORGET per-note strum when a note loses ALL its chord voicing (user: turning scale/chord
        // OFF forgets the strum; just CHANGING the scale keeps it). Applies across a merged group.
        if (wasVoiced && ! (sl.scaleOn || sl.chordMode > 0)) {
            auto slotVoiced = [&](int s){ return ch.slots[s].scaleOn || ch.slots[s].chordMode > 0; };
            auto& sq = proc.sequencer; const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
            for (int b = head; b <= end; ++b) { auto& cc = sq.patterns[b].channels[selectedChannel];
                for (int i = 0; i < cc.drawNoteCount; ++i) { auto& nt = cc.drawNotes[i];
                    const bool sh = (nt.slot == 0 && (slotVoiced(0) || slotVoiced(1)))
                                 || (nt.slot == 1 && slotVoiced(0)) || (nt.slot == 2 && slotVoiced(1));
                    if (! sh) { nt.strumUp = 0; nt.strumPct = 255; } } }
        }
        ch.markDspDirty();
        kbGuideApplied = -1;
        keysHighlightMaskLo = keysHighlightMaskHi = ~0ULL;   // voicing changed -> recompute highlight + chord name
        refreshDetailPanel();                                 // re-syncs the UNISON visual's pass-through stash too
    };
    stepGrid.onGridDivEdit = [this] {
        auto* aw = new juce::AlertWindow("Piano-roll grid", "Snap to 1/N of the bar (1-64, or 0 for off):",
                                         juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor("n", juce::String(stepGrid.gridDiv()));
        aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, aw](int r) {
                if (r == 1) { stepGrid.setGridDiv(aw->getTextEditorContents("n").getIntValue());
                              proc.sequencer.uiGridDiv = juce::jmax(1, stepGrid.gridDiv()); }   // feed LFO/arp "Lock to grid"
            }), true);
    };
    proc.sequencer.uiGridDiv = juce::jmax(1, stepGrid.gridDiv());   // keep the DSP's grid divisor in sync from the start
    // QUANTIZE (one-time): type 1/N, snap every note's START (and clamp length to >= one cell) to
    // that grid for the channel shown in the big editor. Independent of the live snap grid; undoable.
    stepGrid.onQuantizeEdit = [this] {
        auto* aw = new juce::AlertWindow("Quantize notes", "Snap note starts to 1/N of the bar (1-64):",
                                         juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor("n", "16");
        aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, aw](int r) {
                if (r != 1) return;
                const int div = juce::jlimit(1, 64, aw->getTextEditorContents("n").getIntValue());
                const int ch  = selectedChannel;   // the big editor always shows the selected channel
                auto& sq = proc.sequencer;
                const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
                const int cell = juce::jmax(1, DrumChannel::DRAW_RES / div);
                commitUndoNow();
                for (int b = head; b <= end; ++b) {
                    auto& cc = sq.patterns[b].channels[ch];
                    for (int i = 0; i < cc.drawNoteCount; ++i) {
                        auto& n = cc.drawNotes[i];
                        const int snapped = ((int) (n.start + cell / 2) / cell) * cell;   // nearest grid line
                        n.start = (int16_t) juce::jlimit(0, DrumChannel::DRAW_RES - 1, snapped);
                        n.len   = (int16_t) juce::jmax(cell, (int) n.len);                // at least one cell long
                    }
                }
                stepGrid.update(proc.sequencer, proc.anySolo);
            }), true);
    };
    stepGrid.onDrawNotesChanged = [this](int ch, const DrumChannel::DrawNote* notes, int count) {
        // PIANO ROLL: the grid pushes its whole mirror list (CONCAT columns in a merged group) -
        // split it back into per-bar note lists (a note belongs to the bar its start column is in).
        auto& sq = proc.sequencer;
        const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
        const int bars = juce::jlimit(1, StepGridComponent::GRP_MAX, end - head + 1);
        const int RES  = DrumChannel::DRAW_RES;
        for (int b = 0; b < bars; ++b) sq.patterns[head + b].channels[ch].clearDrawNotes();
        for (int i = 0; i < count; ++i)
        {
            const auto& nt = notes[i];
            const int b = juce::jlimit(0, bars - 1, (int) nt.start / RES);
            // [2026-07-15 23:40] push the WHOLE STRUCT (the parameter-list overload silently DROPPED
            // any field it didn't name - the brand-new loop condition reverted on every push).
            DrumChannel::DrawNote nn = nt;
            nn.start = (int16_t) juce::jlimit(0, RES - 1, (int) nt.start - b * RES);
            sq.patterns[head + b].channels[ch].addDrawNote(nn);
        }
    };
    stepGrid.onDrawTuneChanged = [this](int ch, float cents) {   // the roll's TUNE fader: whole-bar
        auto& sq = proc.sequencer;                                // transpose, uniform across a merged group
        const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
        for (int b = head; b <= end; ++b)
            sq.patterns[b].channels[ch].drawTuneCents = juce::jlimit(-50.0f, 50.0f, cents);
        if (ch == selectedChannel) refreshDetailPanel();          // re-pins the base to C4 + tune NOW
    };
    stepGrid.onStepCondChanged = [this](int ch, int step, int len, int mask) {
        if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
        int ls = step; auto& c = groupStepChannel(ch, ls);
        if (ls < 0 || ls >= DrumChannel::MAX_STEPS) return;
        c.stepCondLen[ls]  = juce::jlimit(1, 5, len);
        c.stepCondMask[ls] = mask;
    };
    stepGrid.onInfluenceApply = [this](int ch, int srcStep) {
        int lsrc = srcStep; auto& c = groupStepChannel(ch, lsrc);   // influence stays within the source BAR
        srcStep = lsrc;
        if (srcStep < 0 || srcStep >= DrumChannel::MAX_STEPS) return;
        const int mode = stepGrid.editMode;   // copy ONLY the parameter being edited
        for (int s = 0; s < DrumChannel::MAX_STEPS; ++s)
        {
            if (mode == StepGridComponent::ModeVel)        c.stepVel[s] = c.stepVel[srcStep];
            else if (mode == StepGridComponent::ModeLen)   c.stepNoteLen[s] = c.stepNoteLen[srcStep];
            else if (mode == StepGridComponent::ModePitch) { c.stepPitch[s] = c.stepPitch[srcStep]; c.stepSlide[s] = c.stepSlide[srcStep]; }
            else if (mode == StepGridComponent::ModeProb)  { c.stepCondLen[s] = c.stepCondLen[srcStep]; c.stepCondMask[s] = c.stepCondMask[srcStep]; }
            else if (mode == StepGridComponent::ModePan)   c.stepPan[s]   = c.stepPan[srcStep];
            else if (mode == StepGridComponent::ModeNudge) c.stepNudge[s] = c.stepNudge[srcStep];
            else if (mode == StepGridComponent::ModeModA)  c.stepModA[s]  = c.stepModA[srcStep];
            else if (mode == StepGridComponent::ModeModB)  c.stepModB[s]  = c.stepModB[srcStep];
            else if (mode == StepGridComponent::ModeRoll)  { c.stepRoll[s] = c.stepRoll[srcStep]; c.stepRollDecay[s] = c.stepRollDecay[srcStep]; }
        }
    };
    stepGrid.onInfluenceDisarm = [this](int ch) {
        if (ch == selectedChannel) btnInfluenceTop.setToggleState(false, juce::dontSendNotification);
    };

    // Channel strips:  [#] [sound mixes ▾] [TEST] [M] [S] [OV] [steps ▾]

    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        auto& strip = strips[i];
        int ci = i;

        stripMeter[i].horizontal = true;
        stripMeter[i].onVolume   = [this, i](float v) { proc.sequencer.channel(i).volume = v; };
        stripMeter[i].onVolLearn = [this, i] {
            showMidiLearnMenu(&stripMeter[i], proc.midiLearn,
                              "p" + juce::String(currentPattern()) + "_ch" + juce::String(i) + "_volume", -1,
                              "ui_sel_chVol", "SELECTED-channel Volume");
        };
        stripMeter[i].setTooltip("Channel meter + VOLUME: the cyan handle is this channel's volume, and it is "
                                 "also the level bar's CEILING - the bar fills up to the handle, never past it.\n\n"
                                 "- Drag left/right to set it - MIXER-STYLE dB travel (most of the fader = -54..0 dB, so quiet settings are actually reachable); the white tick at 3/4 = 100%/0 dB, right of it = BOOST up to +6 dB (200%).\n"
                                 "- Boosting past the tick can push the MIX over full scale = clipping; the master LIMITER is the safeguard (it catches only the peaks that cross its ceiling).\n"
                                 "- Double-click = back to 100%.\n"
                                 "- Right-click = MIDI-learn (this exact channel, or the SELECTED channel).");
        content.addAndMakeVisible(stripMeter[i]);

        content.addAndMakeVisible(strip.numBtn);
        strip.numBtn.setLookAndFeel(&tinyBtnLNF);   // small bold font so 2-digit numbers (10-16) fit
        strip.numBtn.setButtonText(juce::String(i + 1));
        strip.numBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
        strip.numBtn.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff20203a));
        strip.numBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3355aa));
        strip.numBtn.onClick = [this, ci] {
            if (juce::ModifierKeys::getCurrentModifiers().isShiftDown() && ci > 0)
            { selectChannel(ci); setChannelMerge(ci, ci - 1); return; }   // SHIFT+click = Merge&Split with previous
            selectChannel(ci);
        };
        strip.numBtn.chIndex = i;
        strip.numBtn.onCopyFrom = [this, ci] (int src) { proc.copyChannel(currentPattern(), src, ci); selectChannel(ci); fullRefresh(); };

        strip.btnMute  = std::make_unique<LearnableButton>("M", "p0_ch" + juce::String(i) + "_mute",  proc.midiLearn);
        strip.btnSolo  = std::make_unique<LearnableButton>("S", "p0_ch" + juce::String(i) + "_solo",  proc.midiLearn);
        // Each M/S/OV right-click ALSO offers the SELECTED-channel variant (follows selection).
        strip.btnMute->altPid = "ui_sel_mute";    strip.btnMute->altLabel = "SELECTED-channel Mute";
        strip.btnSolo->altPid = "ui_sel_solo";    strip.btnSolo->altLabel = "SELECTED-channel Solo";
        strip.btnPoly.altPid  = "ui_sel_overlap"; strip.btnPoly.altLabel  = "SELECTED-channel Overlap";

        content.addAndMakeVisible(*strip.btnMute);
        content.addAndMakeVisible(*strip.btnSolo);

        strip.btnMute->setClickingTogglesState(true);
        strip.btnMute->setColour(juce::TextButton::buttonOnColourId, juce::Colours::orange);
        strip.btnMute->onClick = [this, ci] {
            selectChannel(ci);
            proc.sequencer.channel(ci).mute = strips[ci].btnMute->getToggleState();
        };

        strip.btnSolo->setClickingTogglesState(true);
        strip.btnSolo->setColour(juce::TextButton::buttonOnColourId, juce::Colours::yellow);
        strip.btnSolo->onClick = [this, ci] {
            selectChannel(ci);
            proc.sequencer.channel(ci).solo = strips[ci].btnSolo->getToggleState();
        };

        content.addAndMakeVisible(strip.comboSound);  // now the "Sound Bank" selector
        strip.comboSound.setLookAndFeel(&wideMenuLNF); // 3-column popup (no tall scroll)
        strip.comboSound.mlm      = &proc.midiLearn;   // right-click = MIDI-learn sound browsing
        strip.comboSound.onChange = [this, ci] { handleSoundMixChange(ci); selectChannel(ci); };
        strip.comboSound.onOpen   = [this, ci] { selectChannel(ci); openSoundPicker(ci); };   // the searchable dropdown
        strip.comboSound.panelOpen = [this, ci]
        { return soundPicker != nullptr && soundPicker->isVisible()
                 && static_cast<SoundPickerPanel&>(*soundPicker).clickIgnore == &strips[ci].comboSound; };
        strip.comboSound.onToggleClose = [this]
        { if (soundPicker != nullptr) static_cast<SoundPickerPanel&>(*soundPicker).close(); };

        content.addAndMakeVisible(strip.btnTest);
        strip.btnTest.onClick = [this, ci] { selectChannel(ci); proc.requestTestTrigger(ci); };

        content.addAndMakeVisible(strip.btnPoly);
        strip.btnPoly.setClickingTogglesState(true);
        strip.btnPoly.setLookAndFeel(&tinyBtnLNF);
        strip.btnMute->setLookAndFeel(&tinyBtnLNF);
        strip.btnSolo->setLookAndFeel(&tinyBtnLNF);
        strip.btnPoly.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff35b56a));
        strip.btnPoly.setTooltip("Overlap: lets a sound keep ringing into the next step instead of being cut off - but only if the sound is actually long enough to ring (a short sound still ends on its own). Off = each trigger restarts the sound.\n\nThis is a STEP-mode control - it fades out in Piano Roll, where each note has its own length and Poly is the keyboard's Poly toggle.\n\nNOTE: cutting a sound never cuts its already-sent REVERB/DELAY - those tails live on the master bus and finish on their own (use the reverb Gate / delay Trail for tight wet).\n\nRight-click to assign a MIDI control.");
        strip.btnPoly.midiLearn = &proc.midiLearn;   // paramId set per-pattern in updateStripParamIds()
        strip.btnPoly.onClick = [this, ci] {
            selectChannel(ci);
            proc.sequencer.channel(ci).allowOverlap = strips[ci].btnPoly.getToggleState();
        };

        content.addAndMakeVisible(strip.comboSteps);
        strip.comboSteps.addItem("Piano Roll", StepGridComponent::DRAW_ITEM_ID);   // free poly note lane - TOP of the list (user)
        for (int si = 0; si < DrumChannel::NUM_VALID_STEP_COUNTS; ++si)
        {
            int s = DrumChannel::VALID_STEP_COUNTS[si];
            strip.comboSteps.addItem(juce::String(s) + (s == 1 ? " step" : " steps"), s);
        }
        { auto& c0 = proc.sequencer.channel(i);
          strip.comboSteps.setSelectedId(c0.drawMode ? StepGridComponent::DRAW_ITEM_ID : c0.numSteps, juce::dontSendNotification); }
        // RULES (regressed many times - keep them): read getSelectedId() FIRST; NEVER call setSelectedId()
        // in here (re-entrant, corrupts the selection - the timer's CHANGE-ONLY refresh syncs the combo);
        // the CONFIRM uses a PopupMenu (JUCE/native message boxes HANG in the standalone). Draw->steps with
        // a drawn lane asks first; an empty lane or step->step is immediate.
        strip.comboSteps.onChange = [this, ci] {
            const int id = strips[ci].comboSteps.getSelectedId();
            auto& sq = proc.sequencer;
            const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
            // guard: a step count that can't fit the 64-cell group view is ignored (items are also disabled)
            if (id != StepGridComponent::DRAW_ITEM_ID && id * (end - head + 1) > DrumChannel::MAX_STEPS) return;
            auto& c = sq.patterns[head].channels[ci];
            auto finish = [this, ci] { selectChannel(ci); refreshDrawModeButtons(); stepGrid.update(proc.sequencer, proc.anySolo); };
            // MERGED GROUP: the dropdown applies to EVERY bar individually (8 steps x 3 bars = a
            // 24-cell row in front of you); the roll spans all bars likewise.
            auto eachBar = [&](auto&& f) { for (int b = head; b <= end; ++b) f(sq.patterns[b].channels[ci]); };
            // SIMPLE SWITCH MODEL (user redesign 2026-07-09): steps and piano roll are SEPARATE
            // worlds. Switching between them CLEARS the destination channel's data (no import, no
            // quantize, no pitch compensation - those were confusing and lossy). If there's data to
            // lose we WARN first (a plain PopupMenu, never a message box); an already-empty side just
            // switches silently. Undo restores the previous side (the clear mutates hashed data).
            auto clearSteps = [](DrumChannel& cc) { cc.clearStepData(); };
            auto clearRoll = [](DrumChannel& cc) { cc.clearDrawNotes(); cc.drawVel = 1.0f;
                                                   cc.drawPan = 0.0f; cc.drawTuneCents = 0.0f; };
            if (id == StepGridComponent::DRAW_ITEM_ID)
            {   // -> PIANO ROLL. Clear any step data (fresh roll); warn if steps exist.
                bool hasSteps = false;
                for (int b = head; b <= end && ! hasSteps; ++b) {
                    const auto& cb = sq.patterns[b].channels[ci];
                    for (int s = 0; s < DrumChannel::MAX_STEPS; ++s) if (cb.steps[s]) { hasSteps = true; break; }
                }
                auto go = [this, ci, head, end, finish, clearSteps]() {
                    commitUndoNow();   // the clear-on-switch is undoable
                    auto& sq2 = proc.sequencer;
                    for (int b = head; b <= end; ++b) { auto& cc = sq2.patterns[b].channels[ci];
                                                        clearSteps(cc); cc.drawTuneCents = 0.0f; cc.drawMode = true; }
                    finish();
                };
                if (! hasSteps) { eachBar([](DrumChannel& cc){ cc.drawTuneCents = 0.0f; cc.drawMode = true; }); finish(); return; }
                juce::PopupMenu wm;
                wm.addSectionHeader("Switch to Piano Roll?");
                wm.addSectionHeader("This CLEARS this channel's steps (you can Undo).");
                wm.addItem(1, "Clear steps and switch to Piano Roll");
                wm.addItem(2, "Cancel - stay in step mode");
                wm.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&strips[ci].comboSteps),
                                 [go](int r) { if (r == 1) go(); });
                return;
            }
            // -> N STEPS.
            if (! c.drawMode) { eachBar([id](DrumChannel& cc){ cc.numSteps = id; }); finish(); return; }   // step -> step
            bool hasNotes = false;
            for (int b = head; b <= end; ++b) hasNotes |= sq.patterns[b].channels[ci].drawNoteCount > 0;
            auto go = [this, ci, head, end, id, finish, clearRoll]() {
                commitUndoNow();   // the clear-on-switch is undoable
                auto& sq2 = proc.sequencer;
                for (int b = head; b <= end; ++b) { auto& cc = sq2.patterns[b].channels[ci];
                                                    clearRoll(cc); cc.drawMode = false; cc.numSteps = id; }
                finish();
            };
            if (! hasNotes) { eachBar([id, clearRoll](DrumChannel& cc){ clearRoll(cc); cc.drawMode = false; cc.numSteps = id; }); finish(); return; }
            juce::PopupMenu mm;
            mm.addSectionHeader("Switch to " + juce::String(id) + " steps?");
            mm.addSectionHeader("This CLEARS this channel's piano-roll notes (you can Undo).");
            mm.addItem(1, "Clear notes and switch to " + juce::String(id) + " steps");
            mm.addItem(2, "Cancel - stay in Piano Roll");
            mm.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&strips[ci].comboSteps),
                             [go](int r) { if (r == 1) go(); });
        };

        strip.numBtn.setTooltip("Channel number - click to select + edit its sound below.\n\n"
                                "- DRAG onto another channel's number = copy this channel's sound + steps there "
                                "(the target keeps its own routing).\n"
                                "- SHIFT+CLICK = Merge & Split with the previous channel (split keyboard).\n"
                                "- The COLOUR = routing: dark = Main mix, TEAL = own aux Output, PURPLE = MIDI Out "
                                "(no sound, sequences another plugin).");
        strip.comboSound.setTooltip("Sound Bank: pick this channel's sound (opens the searchable picker; the "
                                    "current sound is highlighted amber).\n\n"
                                    "- RIGHT-CLICK = MIDI-learn sound browsing on the SELECTED channel: "
                                    "NEXT/PREV buttons - tap = one sound, HOLD = keeps scrolling until released.\n"
                                    "- Set the pads to MOMENTARY (not Toggle) in your controller's editor.\n"
                                    "- Works while this window is open (browsing is an editor action).");
        strip.btnTest.setTooltip("Play this channel once with its current settings, to hear it without running the sequencer.");
        strip.btnMute->setTooltip("Mute: silence this channel.");
        strip.btnSolo->setTooltip("Solo: play only this channel (and other soloed ones), muting the rest.");
        strip.comboSteps.setTooltip("Number of steps in this channel's pattern, or PIANO ROLL for a free note lane "
                                    "(draw/record melodies and chords; the lens opens the full editor).\n\n"
                                    "- Steps and Piano Roll are SEPARATE: switching CLEARS this channel's content and "
                                    "starts the other side fresh (it warns first; Undo restores).\n"
                                    "- To quantise notes WITHOUT leaving the roll, use the Quantize button in the "
                                    "roll editor's header.");
    }

    // Detail panel
    content.addAndMakeVisible(lblSelected);
    lblSelected.setFont(juce::Font(13.0f, juce::Font::bold));
    lblSelected.setColour(juce::Label::textColourId, juce::Colours::white);

    content.addAndMakeVisible(freqDisplay);

    setupGroupHeader(hdrEq,     "CHANNEL EQ  (drag the bands on the display below)");
    setupGroupHeader(hdrFilter, "CHANNEL FILTER");
    setupGroupHeader(hdrDrive,  "DRIVE");
    setupGroupHeader(hdrChan,   "CHANNEL");
    setupGroupHeader(hdrSend,   "FX");
    setupGroupHeader(hdrMasterFX,  "PATTERN FX");
    setupGroupHeader(hdrReverb,    "Reverb");
    // Click the REVERB header = cycle the reverb MODE (Room/Hall/Plate/Shimmer). Preset-wide like
    // Size/Decay (ONE shared reverb engine - every slot's Rev Send feeds it).
    hdrReverb.setInterceptsMouseClicks(true, false);
    hdrReverb.addMouseListener(&revModeClick, false);
    revModeClick.fnE = [this](const juce::MouseEvent& e) {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {   // RIGHT-CLICK = switch which BUS the master rows edit (A <-> B)
            masterBusB = ! masterBusB; refreshReverbModeHeader(); refreshDetailPanel(); return;
        }
        auto& m0 = proc.masterFX();
        const int next = (juce::jlimit(0, 3, masterBusB ? m0.reverbModeB : m0.reverbMode) + 1) % 4;
        for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.reverbModeB : p.master.reverbMode) = next;   // flavour = all patterns
        refreshReverbModeHeader();
        if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel);   // hear the new mode
    };
    hdrDelayG.setInterceptsMouseClicks(true, false);
    hdrDelayG.addMouseListener(&delayBusClick, false);
    // [2026-07-15 00:50] the DELAY header mirrors the REVERB header exactly: LEFT-click = cycle
    // this bus's MODE (Tape/Digital/Dub/Analog/Shimmer, all patterns), RIGHT-click = switch A/B.
    delayBusClick.fnE = [this](const juce::MouseEvent& e) {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        { masterBusB = ! masterBusB; refreshReverbModeHeader(); refreshDetailPanel(); return; }
        auto& m0 = proc.masterFX();
        const int next = (juce::jlimit(0, 4, masterBusB ? m0.delayModeB : m0.delayMode) + 1) % 5;
        for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.delayModeB : p.master.delayMode) = next;   // flavour = all patterns
        refreshReverbModeHeader();
        refreshMasterSyncFaders();   // Character dims on Digital
        if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel);   // hear the new mode
    };
    hdrDelayG.setTooltip(juce::String("DELAY (click the title = cycle the MODE; RIGHT-CLICK = switch bus A/B).\n\n"
        "TWO delay buses exist - A and B, each with its own time/feedback/wet/mode. The faders below "
        "edit the bus named in this title; each channel picks its bus by right-clicking its Dly send "
        "fader. The MODE is the loop's character - what happens to each repeat as it recirculates:\n\n")
        + "- Tape: repeats darken gently, low mud trimmed (the original sound - the default).\n"
        + "- Digital: pristine full-band repeats, no colouring.\n"
        + "- Dub: each repeat gets darker AND softly saturated - the reggae/lo-fi smear.\n"
        + "- Analog: dark repeats with a slow BBD-style pitch wobble.\n"
        + "- Shimmer: every repeat is pitched UP an octave - a climbing sparkle trail.\n\n"
        + "One mode per bus for the whole preset; Ping-Pong bounces the repeats left/right in any mode.");
    hdrReverb.setTooltip(juce::String("REVERB (click the title = cycle the MODE; RIGHT-CLICK = switch bus A/B).\n\n"
        "TWO reverb buses exist - A and B, each with its own size/decay/mode (e.g. A = Hall for keys, "
        "B = tight Room for drums). The faders below edit the bus named in this title; each channel picks "
        "its bus by right-clicking its Rev send fader.\n\n")
        + "SIZE says how BIG the room is; the MODE says what the WALLS are made of:\n"
        + "- Room: boxy and fluttery, dark walls (it also naturally plays smaller - part of the character) - drums.\n"
        + "- Hall: smooth and balanced, the neutral one (the original sound - the default).\n"
        + "- Plate: dense, bright, metallic sheen (vintage studios used literal steel plates) - snares, keys.\n"
        + "- Shimmer: every echo pass is pitched UP an octave - a glowing halo for pads and ambient.\n\n"
        + "One mode for the whole preset (all sends share one reverb engine); each sound picks how much "
        + "goes in with its Rev Send knob.");
    setupGroupHeader(hdrDelayG,    "Delay");
    refreshReverbModeHeader();   // AFTER the group-header setup (it sets the real "DELAY A/B" text)
    setupGroupHeader(hdrMasterOut, "MASTER");   // now a sub-header inside the SOUND BLEND box (Pattern Output group removed)

    // Compact value formatters (shown always under each knob)
    auto fmtMs   = [](double v){ return v < 1.0 ? juce::String(juce::roundToInt(v * 1000.0)) + "ms"
                                                : juce::String(v, 2) + "s"; };
    auto fmtPct  = [](double v){ return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
    auto fmtHz   = [](double v){ if (v >= 1000.0) { bool whole = std::fmod(v, 1000.0) < 1.0;
                                     return juce::String(v / 1000.0, whole ? 0 : 1) + "k"; }
                                 return juce::String(juce::roundToInt(v)); };
    auto fmtSemi = [](double v){ bool whole = std::abs(v - std::round(v)) < 0.05;
                                 juce::String s = whole ? juce::String((int) std::round(v)) : juce::String(v, 1);
                                 return (v >= 0 ? "+" : "") + s + "st"; };
    auto fmtPan  = [](double v){ int p = juce::roundToInt(std::abs(v) * 100.0);
                                 return v == 0 ? juce::String("C")
                                       : (v < 0 ? "L" + juce::String(p) : "R" + juce::String(p)); };

    // Per-source AHD envelopes (Sample/Noise/Analog/FM). Time knobs get a
    // logarithmic taper - fine at the fast/low end where drums live.
    {
        const float decDef[5] = { 2.0f, 0.08f, 0.20f, 0.30f, 0.80f };
        for (int s = 0; s < 5; ++s)
        {
            setupKnob(knobSrcAtk[s],  lblSrcAtk[s],  "Attack", 0.0005, 1.0, 0.003, 1.0, fmtMs);
            knobSrcAtk[s].setSkewFactorFromMidPoint(0.03);
            setupKnob(knobSrcHold[s], lblSrcHold[s], "Hold",   0.0, 2.0, 0.0, 1.0, fmtMs);
            knobSrcHold[s].setSkewFactorFromMidPoint(0.15);
            setupKnob(knobSrcDec[s],  lblSrcDec[s],  "Decay",  0.002, 4.0, decDef[s], 1.0, fmtMs);
            knobSrcDec[s].setSkewFactorFromMidPoint(0.15);
            const int i = s;
            knobSrcAtk[s].onValueChange  = [this, i] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).srcAtk[i]  = (float)knobSrcAtk[i].getValue(); };
            knobSrcHold[s].onValueChange = [this, i] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).srcHold[i] = (float)knobSrcHold[i].getValue(); };
            knobSrcDec[s].onValueChange  = [this, i] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).srcDec[i]  = (float)knobSrcDec[i].getValue(); };
            knobSrcAtk[s].setTooltip("Attack: how gently this source fades in when triggered. Small = punchy, larger = a soft swell.");
            knobSrcHold[s].setTooltip("Hold: how long this source stays at full level before it starts to fall.");
            knobSrcDec[s].setTooltip(s == DrumChannel::SrcSample
                ? "Decay: how long this source takes to fall to silence - this sets the length of the sound."
                : "Decay: how long this source takes to fall after the attack/hold. With Sustain at 0% it falls to silence (setting the length); with Sustain up, it falls to that floor instead.");
        }
    }
    setupKnob(knobPitch,   lblPit, "Crush", 0.0, 1.0, 0.0, 1.0, fmtPct);  // sample bit-crush (was Pitch)
    setupKnob(knobVolume,  lblVol, "Volume",  0.0,   2.0,   1.0,   1.0, fmtPct);  // up to 200% (+6 dB) [2026-07-15], default 100%
    setupKnob(knobPan,     lblPan, "Pan",    -1.0,   1.0,   0.0,   1.0, fmtPan);
    setupKnob(knobSlices,  lblSlices, "Slices", 1.0, 16.0, 1.0, 1.0,
              [](double v){ int n=(int)v; return n<=1 ? juce::String("Off") : juce::String(n); });
    knobSlices.setTooltip("Sample SLICING: chop the sample (or its trimmed region) into N equal slices; "
                          "each consecutive hit plays the NEXT slice (wrapping). 1 = Off (whole sample). "
                          "Great for re-sequencing a chopped loop across the steps. Sound-engine sounds ignore it.");
    setupKnob(knobStretch, lblStretch, "Stretch", 0.25, 4.0, 1.0, 1.0,
              [](double v){ return juce::String(v, 2) + "x"; });
    knobStretch.setSkewFactorFromMidPoint(1.0);   // 1x centred; 0.25x (shorter) <-> 4x (longer)
    knobStretch.setTooltip("Time-STRETCH a loaded sample: change its LENGTH WITHOUT changing pitch (1.00x = off, "
                           "up to 4x longer / 0.25x shorter), high-quality SoundTouch. Sample slots only. "
                           "To change PITCH without changing length, use the Pitch knob.");
    knobStretch.onValueChange = [this] { if (!ignoreKnobCallbacks) {
        auto& c = proc.sequencer.channel(selectedChannel); c.slots[envTargetSlot()].smpStretch = (float) knobStretch.getValue(); c.updateStretch(envTargetSlot()); cacheWaveform(selectedChannel); } };

    // Drawable EQ (HP + 3 bells + LP) replaces the old EQ knobs: dragging on the frequency
    // display edits the selected channel's bands directly.
    freqDisplay.onEdit = [this] {
        if (ignoreKnobCallbacks) return;
        proc.sequencer.channel(selectedChannel).markDspDirty();
        };
    freqDisplay.onDragEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    // The resonant FILTER is PER SLOT with TWO in series (filterIdx 0/1). eqEditTarget = 1 or 2 picks
    // the slot; the display edits that slot's filter 1 or 2 (never the other slot's engine).
    freqDisplay.onFilterEdit = [this](int filterIdx, int type, float cut, float reso, float env, float gainDb) {
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, eqEditTarget - 1)];
        if (filterIdx == 0) { sl.filterType = type;  sl.filterCutoff = cut;  sl.filterReso = reso;  sl.filterEnvAmt = env;  sl.filterGain = gainDb; }
        else                { sl.filterType2 = type; sl.filterCutoff2 = cut; sl.filterReso2 = reso; sl.filterEnvAmt2 = env; sl.filterGain2 = gainDb; }
        ch.markDspDirty();
        // The LFO's "filter is off" warning must update the INSTANT a filter is toggled here (a full
        // refreshDetailPanel doesn't run on a filter edit). Recompute from the LFO's own slot.
        auto& lsl = ch.slots[envTargetSlot()];
        auto on = [](int t){ return (t >= DrumChannel::LowPass && t <= DrumChannel::Notch) || t == DrumChannel::Bell; };
        lfoDisplay.setFilterOn(on(lsl.filterType) || on(lsl.filterType2));
    };

    // Filter (multimode) + its type selector
    setupKnob(knobCutoff, lblCutoff, "Cutoff", 20.0, 20000.0, 20000.0, 1.0, fmtHz);
    knobCutoff.setSkewFactorFromMidPoint(800.0);
    setupKnob(knobReso,   lblReso,   "Reso",   0.3,  12.0,    0.707,   1.0,
              [](double v){ return juce::String(v, 1); });
    knobReso.setSkewFactorFromMidPoint(2.0);
    setupKnob(knobEnvAmt, lblEnvAmt, "Env",   -1.0,  1.0,     0.0,     1.0,
              [](double v){ return juce::String(juce::roundToInt(v * 100.0)) + "%"; });

    content.addAndMakeVisible(comboFilterType);
    content.addAndMakeVisible(lblFiltType);
    lblFiltType.setText("Type", juce::dontSendNotification);
    lblFiltType.setFont(juce::Font(9.0f)); lblFiltType.setJustificationType(juce::Justification::centred);
    lblFiltType.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    // Only Off + Formant remain - HP/LP/bells live in the drawable EQ now. (Item ids still
    // map id = filterType+1, so Formant stays id 6 and old projects keep parsing.)
    comboFilterType.addItem("Off",             1);
    comboFilterType.addItem("Formant (Vowel)", 6);
    comboFilterType.onChange = [this] {
        proc.sequencer.channel(selectedChannel).filterType = comboFilterType.getSelectedId() - 1;
    };

    // Drive / distortion + its type selector
    setupKnob(knobDrive, lblDrive, "Drive", 0.0, 1.0, 0.0, 1.0, fmtPct);

    content.addAndMakeVisible(comboDriveType);
    content.addAndMakeVisible(lblDrvType);
    lblDrvType.setText("Drive Type", juce::dontSendNotification);
    lblDrvType.setFont(juce::Font(9.0f)); lblDrvType.setJustificationType(juce::Justification::centred);
    lblDrvType.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    comboDriveType.addItem("Drive: Off", 1);   // labelled so the dropdown reads as DRIVE (the "Drive" text label was removed)
    comboDriveType.addItem("Soft Clip", 2);
    comboDriveType.addItem("Hard Clip", 3);
    comboDriveType.addItem("Tube",      4);
    comboDriveType.addItem("Foldback",  5);
    comboDriveType.addItem("Fuzz",      6);
    comboDriveType.addItem("Bitcrush",  7);
    comboDriveType.addItem("Bass Amp",   9);    // SPLIT rig: clean lows + driven mids/highs
    comboDriveType.addItem("Exciter",    10);   // synthesized 2nd+3rd harmonics blended in (keeps dynamics)
    comboDriveType.setTooltip("Drive TYPE for this slot (the fader beside it = the amount).\n\n"
                              "- Soft Clip / Tube: warm, rounded saturation (Tube adds even harmonics).\n"
                              "- Hard Clip / Fuzz / Foldback: aggressive, buzzy, metallic.\n"
                              "- Bitcrush: digital lo-fi grit.\n"
                              "- BASS AMP: the split rig - lows stay CLEAN, only the mids/highs distort "
                              "= fat, never farty (fixed voicing; the amount is the amp GAIN).\n"
                              "- EXCITER: synthesizes 2nd + 3rd harmonics and blends them IN - the dry signal "
                              "passes untouched, so dynamics survive: bigger and brighter WITHOUT clipping.");
    comboDriveType.onChange = [this] {   // per-slot drive type
        if (ignoreKnobCallbacks) return;
        proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxDriveType = comboDriveType.getSelectedId() - 1;
    };
    comboDriveType.onOpenList = [this] {   // per-item tooltips (hover a type = what it does)
        fxTypeList.openFor(&comboDriveType, {
            { 1,  "Drive: Off", "No drive." },
            { 2,  "Soft Clip",  "Rounded tanh saturation - warm, stays gentle even when pushed hard." },
            { 3,  "Hard Clip",  "Flat-top clipping - raw, buzzy, aggressive." },
            { 4,  "Tube",       "Asymmetric valve-style warmth - adds even harmonics, keeps dynamics." },
            { 5,  "Foldback",   "Wavefolder: peaks fold back DOWN instead of clipping - metallic, synthy." },
            { 6,  "Fuzz",       "Rectified broken-speaker fuzz - splatty, thick, unstable." },
            { 7,  "Bitcrush",   "Digital lo-fi: bit-depth + sample-rate reduction grit." },
            { 9,  "Bass Amp",   "The split rig: LOWS pass clean, only mids/highs drive - fat bass that never farts out. Amount = amp gain." },
            { 10, "Exciter",    "Synthesizes fresh 2nd + 3rd harmonics and blends them IN - the dry signal passes untouched, so it gets bigger and brighter WITHOUT clipping." } },
            comboDriveType.getSelectedId(),
            [this](int id) { comboDriveType.setSelectedId(id, juce::sendNotificationSync); });
    };
    comboDriveType.listOpen      = [this] { return fxTypeList.openForCombo(&comboDriveType); };
    comboDriveType.onToggleClose = [this] { fxTypeList.close(); };

    // Per-channel output routing: Main, or one of the discrete aux outs (process each drum
    // on its own DAW track). Aux outs must be enabled in the host; standalone = Main only.
    content.addAndMakeVisible(comboOutput);
    content.addAndMakeVisible(lblOutput);
    lblOutput.setText("Output", juce::dontSendNotification);
    lblOutput.setFont(juce::Font(9.0f)); lblOutput.setJustificationType(juce::Justification::centred);
    lblOutput.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    comboOutput.addItem("Main out (ch 1/2)", 1);
    for (int i = 1; i <= DrumSequencerProcessor::NUM_AUX_OUTS; ++i)
        comboOutput.addItem("Out " + juce::String(i) + " (ch " + juce::String(i * 2 + 1) + "/" + juce::String(i * 2 + 2) + ")", i + 1);
    comboOutput.addItem("MIDI Out", kMidiOutId);   // route to MIDI (sequence another plugin) - no sound
    comboOutput.setTooltip("Where this channel goes (CHANNEL-WIDE - applies to ALL patterns):\n"
                           "- Main = the normal mix (Pattern FX + master).\n"
                           "- Out 1-" + juce::String(DrumSequencerProcessor::NUM_AUX_OUTS) +
                           " = a discrete stereo out, DRY, so you can mix/process this drum on its OWN DAW track. "
                           "The DAW does the routing: in your DAW, enable this plugin's extra outputs and send 'Out N' to a "
                           "separate track (Reaper: the plugin's pin connector / track routing). Standalone has only Main.\n"
                           "- MIDI Out = makes NO sound; instead sends MIDI notes (the channel's MIDI note below, transposed by "
                           "step Pitch, velocity from the step, ratcheted by Roll) on MIDI channel 1, out the plugin's MIDI output. "
                           "YOUR DAW routes that MIDI to the instrument you want (Reaper: add a MIDI send from this track to the "
                           "synth/sampler track). Mutually exclusive with sound. Strip turns purple (MIDI) / teal (aux out).");
    comboOutput.onChange = [this] {
        if (ignoreKnobCallbacks) return;
        const int id   = comboOutput.getSelectedId();
        const bool midi = (id == kMidiOutId);
        const int  ob   = midi ? 0 : (id - 1);
        // Routing is CHANNEL-WIDE: set it on this channel in EVERY pattern. MIDI XOR sound.
        for (auto& pat : proc.sequencer.patterns) {
            pat.channels[selectedChannel].midiOut   = midi;
            if (! midi) pat.channels[selectedChannel].outputBus = ob;
        }
        refreshRouting();
    };
    // (The old in-panel MIDI-note ComboBox was a zero-size dead widget - the MIDI Out note is set
    //  from the Routing popup's "Change MIDI Out note" submenu now. Removed.)

    // Top-bar "Routing" button: an at-a-glance overview to wire every channel (1-8) to Main,
    // an aux Out, or MIDI Out - all in one popup. Highlights purple when any channel is MIDI.
    content.addAndMakeVisible(btnRoute);
    btnRoute.setLookAndFeel(&dropBtnLNF);   // draws a down-triangle (it's a dropdown)
    btnRoute.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnRoute.setTooltip("Routing overview: wire every channel at once (channel-wide, all patterns).\n\n"
                        "- Main = the normal mix.\n"
                        "- Out N = its own aux output - route it to a separate DAW track.\n"
                        "- MIDI Out = no internal sound; the channel sequences another plugin on a note "
                        "('Change MIDI Out note' picks it; note length = this channel's amp-envelope length).\n"
                        "- Strip colours: purple = MIDI, teal = aux out.");
    btnRoute.onClick = [this] {
        juce::PopupMenu menu;
        for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch) {
            auto& c = proc.sequencer.channel(ch);
            juce::PopupMenu sub;
            sub.addItem(500000 + ch, "Sound -> Main out (ch 1/2)", true, !c.midiOut && c.outputBus == 0);   // 500000+ch (NOT 0 - id 0 = "no selection")
            for (int o = 1; o <= DrumSequencerProcessor::NUM_AUX_OUTS; ++o)
                sub.addItem(ch * 100 + o, "Sound -> Out " + juce::String(o)
                            + " (ch " + juce::String(o * 2 + 1) + "/" + juce::String(o * 2 + 2) + ")",
                            true, !c.midiOut && c.outputBus == o);
            sub.addSeparator();
            // MIDI Out. In the PIANO ROLL the notes come from the grid (C4-absolute), so the base-note
            // picker is redundant -> the item shows that and the "Change note" submenu is disabled (user).
            sub.addItem(ch * 100 + 50,
                        c.drawMode ? "MIDI Out (notes from the Piano Roll)"
                                   : "MIDI Out (" + juce::MidiMessage::getMidiNoteName(c.midiNote, true, true, 4) + ")",
                        true, c.midiOut);
            juce::PopupMenu notes;
            for (int n = 0; n <= 127; ++n)
                notes.addItem(100000 + ch * 200 + n,
                              juce::MidiMessage::getMidiNoteName(n, true, true, 4) + " (" + juce::String(n) + ")",
                              true, c.midiOut && c.midiNote == n);
            sub.addSubMenu("Change MIDI Out note", notes, ! c.drawMode);   // disabled in Piano Roll (pitch comes from the grid)
            // MIDI Out channel (1-16) - so different drums can drive different instruments / DAW MIDI tracks.
            juce::PopupMenu mchan;
            for (int mc = 1; mc <= 16; ++mc)
                mchan.addItem(400000 + ch * 100 + mc, "Channel " + juce::String(mc), true, c.midiOut && c.midiOutChannel == mc);
            sub.addSubMenu("MIDI Out channel" + juce::String(c.midiOut ? " (" + juce::String(c.midiOutChannel) + ")" : ""), mchan);
            sub.addSeparator();
            // Choke group: channels in the SAME group cut each other off (e.g. closed hat cuts open hat). Channel-wide.
            juce::PopupMenu choke;
            choke.addSectionHeader("Channels sharing a group cut each other off");
            choke.addSectionHeader("(e.g. open-hat channel + closed-hat channel)");
            choke.addSectionHeader("(excluding reverb/delay tails - already-sent wet finishes on the master bus)");
            choke.addItem(300000 + ch * 100 + 0, "Off (no choke)", true, c.chokeGroup == 0);
            for (int g = 1; g <= 8; ++g)
                choke.addItem(300000 + ch * 100 + g, "Group " + juce::String(g), true, c.chokeGroup == g);
            sub.addSubMenu("Choke group" + juce::String(c.chokeGroup > 0 ? " (" + juce::String(c.chokeGroup) + ")" : ""), choke);
            // SIDECHAIN DUCK: when the picked channel fires, THIS channel dips and recovers (~130 ms) -
            // the classic kick-ducks-bass pump. Unlike choke, nothing is cut - only the level dips.
            juce::PopupMenu duck;
            duck.addSectionHeader("When that channel HITS, this one dips + recovers");
            duck.addSectionHeader("(volume pump - unlike choke, nothing is cut)");
            duck.addItem(600000 + ch * 100 + 0, "Off (no duck)", true, c.duckBy < 0);
            for (int t = 0; t < Sequencer::NUM_CHANNELS; ++t)
                if (t != ch)
                    duck.addItem(600000 + ch * 100 + t + 1, "Duck by channel " + juce::String(t + 1), true, c.duckBy == t);
            duck.addSeparator();
            static const int amts[4] = { 25, 50, 75, 100 };
            for (int a = 0; a < 4; ++a)
                duck.addItem(700000 + ch * 100 + a + 1, "Dip amount: " + juce::String(amts[a]) + "%",
                             c.duckBy >= 0, std::abs(c.duckAmt - amts[a] / 100.0f) < 0.01f);
            sub.addSubMenu("Duck" + juce::String(c.duckBy >= 0
                               ? " (by ch " + juce::String(c.duckBy + 1) + ", " + juce::String((int) std::lround(c.duckAmt * 100)) + "%)" : ""),
                           duck);
            const juce::String tag = c.midiOut ? "  [MIDI " + juce::MidiMessage::getMidiNoteName(c.midiNote, true, true, 4) + " ch" + juce::String(c.midiOutChannel) + "]"
                                               : (c.outputBus > 0 ? "  [Out " + juce::String(c.outputBus)
                                                     + ": ch " + juce::String(c.outputBus * 2 + 1) + "/" + juce::String(c.outputBus * 2 + 2) + "]" : "");
            menu.addSubMenu("Channel " + juce::String(ch + 1) + tag, sub);
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(btnRoute), [this](int r) {
            if (r <= 0) return;
            if (r >= 700000) {                         // "Duck amount" -> channel-wide
                const int x = r - 700000, ch = x / 100, a = (x % 100) - 1;
                static const float amts[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
                for (auto& pat : proc.sequencer.patterns) pat.channels[ch].duckAmt = amts[juce::jlimit(0, 3, a)];
            } else if (r >= 600000) {                  // "Duck by" -> channel-wide (0 = off)
                const int x = r - 600000, ch = x / 100, t = (x % 100) - 1;
                for (auto& pat : proc.sequencer.patterns) pat.channels[ch].duckBy = t;
            } else if (r >= 500000) {                  // "Sound -> Main" (dedicated id so it's never 0/unclickable)
                const int ch = r - 500000;
                for (auto& pat : proc.sequencer.patterns) { pat.channels[ch].midiOut = false; pat.channels[ch].outputBus = 0; }
            } else if (r >= 400000) {                  // "MIDI Out channel" -> set channel-wide (also enables MIDI out)
                const int x = r - 400000, ch = x / 100, mc = x % 100;
                for (auto& pat : proc.sequencer.patterns) { pat.channels[ch].midiOut = true; pat.channels[ch].midiOutChannel = mc; }
            } else if (r >= 300000) {                  // "Choke group" -> set channel-wide
                const int x = r - 300000, ch = x / 100, grp = x % 100;
                for (auto& pat : proc.sequencer.patterns) pat.channels[ch].chokeGroup = grp;
            } else if (r >= 100000) {                  // "Change MIDI Out note" -> MIDI Out + chosen note
                const int x = r - 100000, ch = x / 200, note = x % 200;
                for (auto& pat : proc.sequencer.patterns) { pat.channels[ch].midiOut = true; pat.channels[ch].midiNote = note; }
            } else if (r % 100 == 50) {                 // "MIDI Out (current note)" -> MIDI, keep note
                const int ch = r / 100;
                for (auto& pat : proc.sequencer.patterns) pat.channels[ch].midiOut = true;
            } else {                                   // Sound -> Main / Out N
                const int ch = r / 100, dest = r % 100;
                for (auto& pat : proc.sequencer.patterns) { pat.channels[ch].midiOut = false; pat.channels[ch].outputBus = dest; }
            }
            refreshRouting(); refreshDetailPanel();
        });
    };

    // (Rev/Dly send knobs retired - the sends are CHANNEL FX box faders now.)
    setupKnob(knobPunch, lblPunch, "Punch", -1.0, 1.0, 0.0, 1.0,
              [](double v){ const int t = juce::roundToInt(v * 100.0);
                            return t == 0 ? juce::String("Off") : (t < 0 ? "Soft " + juce::String(-t) : "+" + juce::String(t)); });
    setupKnob(knobRing,  lblRing,  "Ring",   0.0, 1.0, 0.0, 1.0,
              [](double v){ return v <= 0.001 ? juce::String("Off") : juce::String(juce::roundToInt(v * 100.0)) + "%"; });
    setupKnob(knobSub,   lblSub,   "Sub",    0.0, 1.0, 0.0, 1.0,
              [](double v){ return v <= 0.001 ? juce::String("Off") : juce::String(juce::roundToInt(v * 100.0)) + "%"; });
    setupKnob(knobFormant, lblFormant, "Formant", 0.0, 1.0, 0.0, 1.0,
              [](double v){ if (v <= 0.001) return juce::String("Off");
                            static const char* vw[5] = { "A", "E", "I", "O", "U" };   // the vowel the knob sits on
                            const float pos = juce::jlimit(0.0f, 1.0f, ((float) v - 0.12f) / 0.88f) * 4.0f;
                            return juce::String(vw[juce::jlimit(0, 4, juce::roundToInt(pos))]) + " " + juce::String(juce::roundToInt(v * 100.0)) + "%"; });
    knobPunch.onValueChange = [this]{ if(!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxPunch = (float)knobPunch.getValue(); };
    knobRing.onValueChange  = [this]{ if(!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxRing  = (float)knobRing.getValue(); };
    knobSub.onValueChange   = [this]{ if(!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxSub   = (float)knobSub.getValue(); };
    knobFormant.onValueChange = [this]{ if(!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxFormant = (float)knobFormant.getValue(); };
    for (auto* k : { &knobPunch, &knobRing, &knobSub, &knobFormant })
        k->onDragEnd = [this]{ if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    knobPunch.setTooltip("PUNCH (per slot): transient shaper - reshapes each hit's ATTACK.\n\n"
        "- Right = more snap/click (drums cut through), left = softened attack (felt-piano, pillowy pads).\n"
        "- Works per hit on this slot only; 0 = untouched.");
    knobRing.setTooltip("RING (per slot): ring modulator - blends the tone against a sine carrier.\n\n"
        "- Up = metallic/bell/clangorous sidebands blended over the dry tone.\n"
        "- The CARRIER is the Ring Hz knob beside this (or note-tracked at its hard left).\n"
        "- Great on mallets, FX and toms for inharmonic colour; 0 = dry.");
    knobSub.setTooltip("SUB (per slot): a clean sine ONE OCTAVE below this slot's pitch, mixed underneath.\n\n"
        "- Follows the note (keys, steps, glide) and the pitch envelope; shaped by the slot's amp envelope.\n"
        "- Added AFTER the slot filter/EQ/drive, so it stays clean however hard you mangle the top.\n"
        "- Pitched engines only (Oscillator / Karplus-Strong / Modal / Granular) - inert on Noise and Sample.\n\n"
        "Tip: instant weight on basses and plucks; also fattens a thin lead without touching its character.");
    knobFormant.setTooltip("FORMANT (per slot): vowel filter - turning the knob morphs A - E - I - O - U.\n\n"
        "- Two vocal-tract resonances (F1 + F2) blended over the dry tone at a fixed intensity.\n"
        "- Route an LFO (or Mod Wheel / Step Mod) to the matrix target \"Formant\" and the slot TALKS.\n"
        "- 0 = off (bit-identical); the effect eases in over the first ~12% of the knob.");
    setupKnob(knobRingHz, lblRingHz, "Ring Hz", 25.0, 4000.0, 200.0, 0.35,
              [](double v){ return v < 26.0 ? juce::String("Track")
                                            : v >= 1000.0 ? juce::String(v / 1000.0, 2) + " kHz"
                                                          : juce::String(juce::roundToInt(v)) + " Hz"; });
    knobRingHz.onValueChange = [this]{ if(!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxRingHz = (float)knobRingHz.getValue(); };
    knobRingHz.onDragEnd     = [this]{ if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    knobRingHz.setTooltip("RING Hz: the ring modulator's CARRIER.\n\n"
        "- Default 200 Hz = the classic fixed carrier; higher = brighter/glassier sidebands.\n"
        "- HARD LEFT = \"Track\": the carrier FOLLOWS THE NOTE (tuned ring mod - the colour stays consistent across the keyboard).\n"
        "- Only audible while the Ring knob is up.");

    setupKnob(knobSlotPan, lblSlotPan, "Pan", -1.0, 1.0, 0.0, 1.0,
              [](double v){ return std::abs(v) < 0.01 ? juce::String("C")
                                                      : (v < 0 ? "L" : "R") + juce::String(juce::roundToInt(std::abs(v) * 100.0)); });
    knobSlotPan.onValueChange = [this]{ if(!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].pan = (float)knobSlotPan.getValue(); };
    knobSlotPan.onDragEnd     = [this]{ if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    knobSlotPan.setTooltip("SLOT PAN: place THIS LAYER left or right (static, equal-power).\n\n"
        "- Pan the two slots apart = instant instrument WIDTH, even on mono engines (KS / samples / noise) - and real stereo for the Widener to work on.\n"
        "- Composes with per-step / per-note pan (those move the whole hit; this places the layer inside it).\n"
        "- Deliberately NOT a matrix target: placement is static - stereo MOVEMENT is Channel FX Auto-Pan's job.\n"
        "- Double-click = centre.");

    // ---- CHANNEL FX: TWO selectable effect SLOTS (A -> B in series) on the WHOLE instrument, plus
    //      the channel REVERB/DELAY SEND faders. None of this follows the slot selector - one set per
    //      channel, saved with the sound. ----
    setupGroupHeader(hdrChannelFx, "CHANNEL FX");
    content.addChildComponent(fxTypeList);   // shared per-item-tooltip dropdown (channel FX + drive types)
    {
        // Per-ITEM tooltips (user: "i wanna see individual tooltips when i hover on them") - a plain
        // ComboBox popup can't do that, so TipCombo opens the TipList panel instead.
        auto chFxTipItems = [] {
            return std::vector<TipList::Item> {
                { 1,  "Off",       "No effect in this slot." },
                { 2,  "Chorus",    "3-voice stereo thickener / widener - the classic detuned shimmer.\n\nCharacter = sweep speed + depth." },
                { 12, "Chorus (sync)",  "Chorus locked to the tempo: Character = sweeps per bar (1/4 /bar ... 16 /bar), follows tempo changes live. Depth stays at the classic voicing." },
                { 3,  "Flanger",   "Swept comb filter - the metallic \"jet\" whoosh.\n\nCharacter = sweep speed + feedback bite." },
                { 13, "Flanger (sync)", "Flanger locked to the tempo: Character = sweeps per bar. Feedback stays at the classic voicing." },
                { 4,  "Phaser",    "6-stage all-pass swirl - softer and hollower than the flanger.\n\nCharacter = sweep speed + resonance." },
                { 14, "Phaser (sync)",  "Phaser locked to the tempo: Character = sweeps per bar. Resonance stays at the classic voicing." },
                { 5,  "Comp",      "Glue compressor across the whole instrument (both slots together) - transparent: the tone stays, the dynamics tighten.\n\nCharacter = attack in ms: short = smashed flat, long = the transient punches through first (4 ms = the classic)." },
                { 6,  "Tape",      "Tape wobble: wow + flutter pitch drift + softened highs. Adds ~3 ms latency while on.\n\nCharacter = wobble speed." },
                { 15, "Tape (sync)",    "Tape wobble locked to the tempo: Character = wobbles per bar." },
                { 7,  "Auto-Pan",  "Moves the WHOLE instrument left-right. It drives itself (internal LFO) - no matrix routing needed.\n\nCharacter = pan speed." },
                { 16, "Auto-Pan (sync)","Auto-Pan locked to the tempo: Character = sweeps per bar - \"1 /bar\" = exactly one left-right swing per bar, forever in the pocket." },
                { 8,  "Widener",   "Mid/Side width boost with a bass-mono floor - needs STEREO content first (Chorus, unison Width, slot pans).\n\nCharacter = the bass-mono crossover in Hz (60..600: higher = more of the low end stays centred)." },
                { 9,  "OTT",       "3-band UP + DOWN compression: quiet details and tails are dragged UP per band = the hyped modern wall (tone-changing, unlike Comp). Amount = depth.\n\nCharacter = compression speed (0.25x..4x; 1x = the classic)." },
                { 10, "FreqShift", "Single-sideband frequency shifter - moves every harmonic by the SAME Hz, so the sound turns inharmonic.\n\nCharacter = the shift in Hz: centre = 0 Hz, right = up, left = down. A few Hz = barber-pole detune, hundreds = alien metal." },
                { 11, "Rotary",    "Leslie speaker: horn doppler + tremolo + rotor pan.\n\nCharacter = rotor speed in Hz (0.7 = slow chorale, 7 = fast tremolo)." },
                { 17, "Rotary (sync)",  "Leslie locked to the tempo: Character = rotations per bar." } };
        };
        for (int i = 0; i < 3; ++i)
        {
            auto& cb = comboChFx[i];
            content.addAndMakeVisible(cb);
            cb.setLookAndFeel(&tinyComboLNF);   // compact skin: tiny chevron + squeezed text (no "...")
            cb.addItem("Off", 1);   // SHORT names: the 66px combo must read without "..." (user)
            cb.addItem("Chorus", 2); cb.addItem("Chorus (sync)", 12);      // [2026-07-15] each "(sync)"
            cb.addItem("Flanger", 3); cb.addItem("Flanger (sync)", 13);   // sits under its free sibling
            cb.addItem("Phaser", 4); cb.addItem("Phaser (sync)", 14);
            cb.addItem("Comp", 5);
            cb.addItem("Tape", 6); cb.addItem("Tape (sync)", 15);
            cb.addItem("Auto-Pan", 7); cb.addItem("Auto-Pan (sync)", 16);
            cb.addItem("Widener", 8); cb.addItem("OTT", 9); cb.addItem("FreqShift", 10);
            cb.addItem("Rotary", 11); cb.addItem("Rotary (sync)", 17);
            cb.setSelectedId(1, juce::dontSendNotification);
            cb.setTooltip(juce::String("CHANNEL FX slot ") + (i == 0 ? "A" : i == 1 ? "B" : "C") + ": pick an effect for the WHOLE instrument "
                          "(both sound slots combined; runs A -> B -> C in series).\n\n"
                          "- Hover each entry in the open list for what it does.\n"
                          "- AMOUNT = how much. CHARACTER = the effect's second dimension, shown in its REAL unit "
                          "(sweep Hz or /bar, attack ms, speed x, shift Hz, crossover Hz - see each entry's tooltip).\n"
                          "- Both faders are matrix targets: \"FX " + juce::String(i == 0 ? "A" : i == 1 ? "B" : "C") + " Amount/Character (Channel)\".");
            cb.onChange = [this, i] {
                if (ignoreKnobCallbacks) return;
                auto& ch = proc.sequencer.channel(selectedChannel);
                ch.chFxType[i] = juce::jlimit(0, 16, comboChFx[i].getSelectedId() - 1);
                if (ch.chFxType[i] != DrumChannel::ChFxOff && ch.chFxAmt[i] <= 0.001f)
                    ch.chFxAmt[i] = 0.5f;   // picking an effect with amount 0 would be silent - start audible (disclosed)
                ch.markDspDirty(); refreshDetailPanel();
                if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel);
            };
            cb.onOpenList = [this, i, chFxTipItems] {
                fxTypeList.openFor(&comboChFx[i], chFxTipItems(), comboChFx[i].getSelectedId(),
                                   [this, i](int id) { comboChFx[i].setSelectedId(id, juce::sendNotificationSync); });
            };
            cb.listOpen      = [this, i] { return fxTypeList.openForCombo(&comboChFx[i]); };
            cb.onToggleClose = [this]    { fxTypeList.close(); };
            auto setupF = [this](SlotDragFader& f, const char* nm, const juce::String& tip) {
                content.addAndMakeVisible(f);
                f.setAccent(juce::Colour(0xff35c0ff));   // channel scope = cyan (not a slot colour)
                f.setLabel(nm);
                f.format = [](float v){ return juce::String(juce::roundToInt(v * 100.0f)) + "%"; };
                f.setDefault(0.5f);
                f.mlm = &proc.midiLearn;
                f.setTooltip(tip);
                f.onDragEnd = [this]{ if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
            };
            const juce::String sl9 = i == 0 ? "A" : i == 1 ? "B" : "C";
            setupF(chFxAmtF[i], "Amt", "FX " + sl9 + " AMOUNT: how much of the effect (0 = bypass). Matrix target \"FX " + sl9 + " Amount (Channel)\".");
            setupF(chFxChrF[i], "Chr", "FX " + sl9 + " CHARACTER: the effect's second dimension, shown in its real unit - sweep speed (Hz or /bar), Comp attack (ms), OTT speed (x), FreqShift (+-Hz), Widener crossover (Hz). See the type dropdown's per-entry tooltips. Matrix target \"FX " + sl9 + " Character (Channel)\".");
            chFxAmtF[i].learnPid = i == 0 ? "ui_sel_cfxAmtA" : i == 1 ? "ui_sel_cfxAmtB" : "ui_sel_cfxAmtC";
            chFxChrF[i].learnPid = i == 0 ? "ui_sel_cfxChrA" : i == 1 ? "ui_sel_cfxChrB" : "ui_sel_cfxChrC";
            chFxAmtF[i].setDefault(0.0f);
            chFxAmtF[i].onChange = [this, i](float v) {
                if (ignoreKnobCallbacks) return;
                auto& ch = proc.sequencer.channel(selectedChannel);
                ch.chFxAmt[i] = juce::jlimit(0.0f, 1.0f, v); ch.markDspDirty();
            };
            // [2026-07-15 15:00] Character reads in each type's HONEST unit (user: "%" said nothing).
            // Display-only - the formulas MIRROR the DSP constants in DrumChannel.cpp's chFx cases
            // (mirror rule: change one = change both). Value-only (no "Chr" prefix - it truncated).
            chFxChrF[i].format = [this, i](float v) {
                using DC = DrumChannel;
                const int t = proc.sequencer.channel(selectedChannel).chFxType[i];
                auto hz = [](double h) { return h < 9.95 ? juce::String(h, 2) + " Hz" : juce::String(juce::roundToInt(h)) + " Hz"; };
                switch (t)
                {
                    case DC::ChFxChorusS: case DC::ChFxFlangerS: case DC::ChFxPhaserS:
                    case DC::ChFxTapeS:   case DC::ChFxAutoPanS: case DC::ChFxRotaryS:
                    { const float c = DC::kChFxCpb[juce::jlimit(0, DC::kChFxCpbN - 1, (int) std::lround(v * (float)(DC::kChFxCpbN - 1)))];
                      return (c < 1.0f ? juce::String(c, 2) : juce::String(juce::roundToInt(c))) + " /bar"; }
                    case DC::ChFxChorus:  return hz(0.36 * std::pow(4.0, 2.0 * v - 1.0));   // sweep speed
                    case DC::ChFxFlanger: return hz(0.20 * std::pow(4.0, 2.0 * v - 1.0));
                    case DC::ChFxPhaser:  return hz(0.30 * std::pow(4.0, 2.0 * v - 1.0));
                    case DC::ChFxTape:    return hz(0.80 * std::pow(4.0, 2.0 * v - 1.0));
                    case DC::ChFxAutoPan: return hz(1.40 * std::pow(4.0, 2.0 * v - 1.0));
                    case DC::ChFxRotary:  return hz(0.70 * std::pow(10.0, (double) v));
                    case DC::ChFxComp:                                                       // attack time
                    { const float ms = 4.0f * std::pow(4.0f, 1.0f - 2.0f * v);
                      return (ms < 3.0f ? juce::String(ms, 1) : juce::String(juce::roundToInt(ms))) + " ms"; }
                    case DC::ChFxOtt:                                                        // envelope SPEED
                    { const float x = std::pow(4.0f, 2.0f * v - 1.0f);   // rises with the fader (the DSP's
                      return juce::String(x, x < 1.0f ? 2 : 1) + "x"; }  //  time scale is its inverse - user
                                                                         //  caught the backwards number)
                    case DC::ChFxFreqShift:                                                  // +-Hz shift
                    { const float d = 2.0f * v - 1.0f;
                      const int s9 = juce::roundToInt((d >= 0.0f ? 1.0 : -1.0) * (std::pow(1500.0, (double) std::abs(d)) - 1.0));
                      return s9 == 0 ? juce::String("0 Hz") : (s9 > 0 ? "+" : "") + juce::String(s9) + " Hz"; }
                    case DC::ChFxWiden:                                                      // bass-mono crossover
                      return juce::String(juce::roundToInt(60.0 * std::pow(10.0, (double) v))) + " Hz";
                    default: return juce::String(juce::roundToInt(v * 100.0f)) + "%";
                }
            };
            chFxChrF[i].setLabel({});   // value-only: "Chr 2 /bar" didn't fit (user); the tooltip names it
            chFxChrF[i].onChange = [this, i](float v) {
                if (ignoreKnobCallbacks) return;
                auto& ch = proc.sequencer.channel(selectedChannel);
                ch.chFxChar[i] = juce::jlimit(0.0f, 1.0f, v); ch.markDspDirty();
            };
        }
        // Channel SEND faders (the sends moved here from the per-slot FX box). Right-click = bus pick + learn.
        auto setupSend = [this](SlotDragFader& f, const char* nm, bool isRev) {
            content.addAndMakeVisible(f);
            f.setAccent(juce::Colour(0xff35c0ff));
            f.setLabel(nm);
            f.format = [this, isRev](float v){   // value + BUS name, e.g. "73% Bus A" (user request)
                auto& ch = proc.sequencer.channel(selectedChannel);
                const juce::String bus = juce::String(" Bus ") + ((isRev ? ch.revBus : ch.delBus) != 0 ? "B" : "A");
                return (v <= 0.001f ? juce::String("Off") : juce::String(juce::roundToInt(v * 100.0f)) + "%") + bus; };
            f.setDefault(0.0f);
            f.mlm = &proc.midiLearn; f.learnPid = isRev ? "ui_sel_fxRev" : "ui_sel_fxDel";
            f.setTooltip(juce::String(isRev ? "REVERB" : "DELAY") + " SEND (channel): how much of the finished channel feeds the shared "
                         + (isRev ? juce::String("reverb") : juce::String("delay")) + ".\n\n"
                         "- Post CHANNEL FX (the chorused/compressed sound is what reverberates).\n"
                         + (isRev ? juce::String("- The send is high-passed ~150 Hz (fixed): subs stay DRY - the classic mixing move.\n") : juce::String())
                         + "- RIGHT-CLICK: pick which BUS (A or B - each has its own " + (isRev ? juce::String("size/decay/mode") : juce::String("time/feedback")) + " in MASTER) + MIDI-learn.\n"
                         "- Matrix target: \"" + (isRev ? juce::String("Reverb Send") : juce::String("Delay Send")) + "\".");
            f.onChange = [this, isRev](float v) {
                if (ignoreKnobCallbacks) return;
                auto& ch = proc.sequencer.channel(selectedChannel);
                (isRev ? ch.reverbSend : ch.delaySend) = juce::jlimit(0.0f, 1.0f, v); ch.markDspDirty();
            };
            f.onDragEnd = [this]{ if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
            f.onRightClick = [this, isRev, &f]() -> bool {
                auto& ch = proc.sequencer.channel(selectedChannel);
                const int cur = isRev ? ch.revBus : ch.delBus;
                juce::PopupMenu m;
                m.addSectionHeader(isRev ? "Reverb bus (this channel)" : "Delay bus (this channel)");
                m.addItem(1, "Bus A", true, cur == 0);
                m.addItem(2, "Bus B", true, cur == 1);
                m.addSeparator();
                m.addItem(3, "MIDI-learn this send...");
                m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&f), [this, isRev, &f](int r) {
                    if (r == 0) return;
                    auto& ch2 = proc.sequencer.channel(selectedChannel);
                    if (r == 3) { showMidiLearnMenu(&f, proc.midiLearn, f.learnPid, -1); return; }
                    const int8_t bus = (int8_t)(r - 1);
                    // bus assignment is ROUTING (channel-wide): write it to this channel in EVERY pattern
                    const int chIdx = selectedChannel;
                    for (auto& pat : proc.sequencer.patterns)
                        (isRev ? pat.channels[chIdx].revBus : pat.channels[chIdx].delBus) = bus;
                    juce::ignoreUnused(ch2);
                    f.repaint();   // the fader shows the bus name - update it instantly
                });
                return true;
            };
        };
        setupSend(sendRevF, "Rev", true);
        setupSend(sendDelF, "Dly", false);
    }

    // Per-slot LFO visual (FX box bottom): the drawn sine IS the parameters. Three independent
    // LFOs per slot - the drag edits ONLY the tab-selected one.
    content.addAndMakeVisible(lfoDisplay);
    lfoDisplay.onChange = [this](int dest, float rate, float amt) {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[envTargetSlot()];
        sl.lfoRate[dest] = rate; sl.lfoAmt[dest] = amt;
        ch.markDspDirty();
        bool lfoRt[4]; lfoRoutedMask(sl, lfoRt);
        lfoDisplay.setValues(sl.lfoRate, sl.lfoAmt, sl.lfoSync, sl.lfoShape, sl.lfoFree, sl.lfoLegato, sl.lfoCurve, lfoRt, ((sl.filterType >= DrumChannel::LowPass && sl.filterType <= DrumChannel::Notch) || sl.filterType == DrumChannel::Bell || (sl.filterType2 >= DrumChannel::LowPass && sl.filterType2 <= DrumChannel::Notch) || sl.filterType2 == DrumChannel::Bell), (sl.oscShape >= DrumChannel::WvCustom || sl.engine == DrumChannel::SrcGrain),
                             envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8));
    };
    // Tempo sync is edited IN the visual (Sync button + snapped wave drag) - write it straight back.
    lfoDisplay.onSyncChange = [this](int dest, float sync) {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.slots[envTargetSlot()].lfoSync[juce::jlimit(0, 3, dest)] = sync;
        ch.markDspDirty();
    };
    lfoDisplay.onShapeChange = [this](int dest, int shape) {   // Shape dropdown (Sine..Custom)
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[envTargetSlot()];
        const int d = juce::jlimit(0, 3, dest);
        const int prev = sl.lfoShape[d];
        sl.lfoShape[d] = juce::jlimit(0, 7, shape);
        if (sl.lfoShape[d] == 7)
        {   // picking Custom with an EMPTY curve seeds it from the shape you were on - you start
            // editing what you heard, never a dead flat line
            bool empty = true;
            for (int k = 0; k < DrumChannel::Slot::LFO_CURVE_N && empty; ++k)
                empty = std::abs(sl.lfoCurve[d][k]) < 1.0e-4f;
            if (empty)
                for (int k = 0; k < DrumChannel::Slot::LFO_CURVE_N; ++k)
                    sl.lfoCurve[d][k] = uiLfoShapeVal(prev, 2.0 * juce::MathConstants<double>::pi
                                                            * (double) k / (double) DrumChannel::Slot::LFO_CURVE_N, 0);
        }
        ch.markDspDirty();
        refreshDetailPanel();   // push the seeded curve into the display immediately
        if (sl.lfoShape[d] == 7) openLfoCurveEditor(d);   // picking Custom opens the draw window
    };
    lfoDisplay.onOpenCurveEditor = [this](int dest) { openLfoCurveEditor(dest); };   // click the wave in Custom
    content.addChildComponent(lfoCurveEd);
    lfoCurveEd.clickIgnore = &lfoDisplay;
    lfoCurveEd.onChange = [this](const float* cv) {
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[envTargetSlot()];
        const int d = juce::jlimit(0, 3, lfoCurveEdDest);
        for (int k = 0; k < DrumChannel::Slot::LFO_CURVE_N; ++k) sl.lfoCurve[d][k] = cv[k];
        ch.markDspDirty();
        // Push the LFO wave view IMMEDIATELY: there is NO timer push for it (the old comment
        // claimed one) - the drawn curve only appeared whenever something ELSE refreshed the
        // panel = "takes some time and sometimes never shows" (user report).
        bool lfoRt[4]; lfoRoutedMask(sl, lfoRt);
        lfoDisplay.setValues(sl.lfoRate, sl.lfoAmt, sl.lfoSync, sl.lfoShape, sl.lfoFree, sl.lfoLegato, sl.lfoCurve, lfoRt,
                             ((sl.filterType >= DrumChannel::LowPass && sl.filterType <= DrumChannel::Notch) || sl.filterType == DrumChannel::Bell
                              || (sl.filterType2 >= DrumChannel::LowPass && sl.filterType2 <= DrumChannel::Notch) || sl.filterType2 == DrumChannel::Bell),
                             (sl.oscShape >= DrumChannel::WvCustom || sl.engine == DrumChannel::SrcGrain),
                             envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8));
    };
    lfoDisplay.onTrigChange = [this](int dest, bool freeRun, bool legato) {  // Retrig/Free/Legato
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.slots[envTargetSlot()].lfoFree[juce::jlimit(0, 3, dest)]   = freeRun;
        ch.slots[envTargetSlot()].lfoLegato[juce::jlimit(0, 3, dest)] = legato;
        ch.markDspDirty();
    };
    lfoDisplay.onDragEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };

    // TAB 4 of the LFO visual = the Mod Env: dragging its A-H-D-S-R writes the selected slot.
    lfoDisplay.onModEnvChange = [this](float a, float h, float d, float s, float r) {
        if (ignoreKnobCallbacks) return;
        auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        sl.modEnvA = a; sl.modEnvH = h; sl.modEnvD = d; sl.modEnvS = s; sl.modEnvR = r;
        proc.sequencer.channel(selectedChannel).markDspDirty();
    };

    // 12-FADER MOD MATRIX (inline in the MODULATION box). Each fader = one route; right-click opens the
    // two-column source|target picker; drag sets the bipolar amount. Writes the SELECTED slot's mod[].
    content.addAndMakeVisible(modFaders);
    modFaders.gridKnobName = [this](int gridIdx) -> juce::String {
        auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        if (DrumChannel::modGridKnob(sl.engine, gridIdx).field == nullptr) return {};   // choice/none = skip
        auto params = slotParamsFor(sl.engine);
        return (gridIdx < params.size()) ? params[gridIdx].label : juce::String();
    };
    modFaders.onChange = [this] {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[envTargetSlot()];
        for (int r = 0; r < DrumChannel::MOD_ROUTES; ++r)
        { sl.mod[r].src = (int8_t) modFaders.src[r]; sl.mod[r].tgt = (int8_t) modFaders.tgt[r];
          sl.mod[r].amt = modFaders.amt[r]; }
        ch.markDspDirty();
    };
    modFaders.onDragEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };   // hear the route (auto-test)
    // Right-click a fader -> the source|target picker (deferred open so the opening click doesn't
    // reach the picker's own outside-click watcher and instantly re-close it).
    modFaders.onPickRoute = [this](int route) { juce::MessageManager::callAsync([this, route] { openRoutePicker(route); }); };

    content.addChildComponent(routePicker);
    routePicker.gridKnobName = [this](int gridIdx) -> juce::String {
        auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        if (DrumChannel::modGridKnob(sl.engine, gridIdx).field == nullptr) return {};
        auto params = slotParamsFor(sl.engine);
        return (gridIdx < params.size()) ? params[gridIdx].label : juce::String();
    };
    routePicker.onClose = [this] { routePicker.setVisible(false); };
    routePicker.onLag = [this](float ms) {   // [2026-07-14 01:33] per-route LAG
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.slots[envTargetSlot()].mod[juce::jlimit(0, DrumChannel::MOD_ROUTES - 1, routePickRoute)].lagMs = ms;
        ch.markDspDirty();
    };
    // MIDI [2026-07-14 11:10]: the picker's MIDI button = learn a CC to THIS route's amount
    // (ui_sel_modAmt{R} - selected-scope, so it drives route R on whatever slot is selected).
    routePicker.onMidi = [this] {
        const int r = juce::jlimit(0, DrumChannel::MOD_ROUTES - 1, routePickRoute);
        showMidiLearnMenu(&routePicker, proc.midiLearn, "ui_sel_modAmt" + juce::String(r), -1);
    };
    // REMAP [2026-07-14]: the picker's remap row opens the curve overlay for the CURRENT route.
    content.addChildComponent(remapEd);
    routePicker.onRemap = [this] {
        juce::MessageManager::callAsync([this] {   // let the opening click finish (Closer precedent)
            const int r = juce::jlimit(0, DrumChannel::MOD_ROUTES - 1, routePickRoute);
            auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
            float cv[RemapEditor::N];
            for (int k = 0; k < RemapEditor::N; ++k) cv[k] = (float) sl.mod[r].curve[k] / 255.0f;
            auto b = routePicker.getBounds();
            remapEd.setBounds(b.getX(), juce::jmax(0, b.getCentreY() - 130), juce::jmin(440, b.getWidth()), 260);
            remapEd.openFor("MODULATION AMOUNT MAP   " + modFaders.routeSrcName(r) + "  >  " + modFaders.routeTgtName(r),
                            cv, sl.mod[r].curveOn != 0, DrumChannel::modSrcBipolar(sl.mod[r].src),
                            envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8));
        });
    };
    remapEd.onChange = [this](const float* cv, bool on) {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& rt = ch.slots[envTargetSlot()].mod[juce::jlimit(0, DrumChannel::MOD_ROUTES - 1, routePickRoute)];
        rt.curveOn = on ? 1 : 0;
        for (int k = 0; k < RemapEditor::N; ++k)
            rt.curve[k] = (uint8_t) juce::jlimit(0, 255, juce::roundToInt(cv[k] * 255.0f));
        ch.markDspDirty();
        routePicker.remapOn = on; routePicker.repaint();
        modFaders.setValues(ch.slots[envTargetSlot()]);   // curve glyph follows
    };
    remapEd.onClose = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    routePicker.onPicked = [this](int src, int tgt) {
        if (routePickRoute < 0 || routePickRoute >= DrumChannel::MOD_ROUTES) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[envTargetSlot()];
        sl.mod[routePickRoute].src = (int8_t) src; sl.mod[routePickRoute].tgt = (int8_t) tgt;
        // The LFO's OWN amount is its depth (source = amount x shape). LFOs are off (amount 0) by default,
        // so routing one that's still at 0 would do nothing - turn it on so the route works immediately.
        if (src >= DrumChannel::MSLfoFilt && src <= DrumChannel::MSLfoWave)
        { const int d = src - DrumChannel::MSLfoFilt; if (sl.lfoAmt[d] < 0.05f) sl.lfoAmt[d] = 1.0f; }
        ch.markDspDirty();
        modFaders.setValues(sl);   // reflect the new source/target on the fader immediately
        refreshDetailPanel();      // the LFO visual shows its now-active wave
    };
    routePicker.onAmt = [this](float amt) {   // the picker's own AMOUNT fader (big, with a % read-out)
        if (routePickRoute < 0 || routePickRoute >= DrumChannel::MOD_ROUTES) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.slots[envTargetSlot()].mod[routePickRoute].amt = juce::jlimit(-1.0f, 1.0f, amt);
        ch.markDspDirty();
        modFaders.setValues(ch.slots[envTargetSlot()]);   // the 12-fader matrix reflects it too
    };
    routePicker.onAmtDragEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };   // hear the route

    // ---- Per-slot FX + Chorus + Keytrack + LFO-sync DRAG-FADERS (Arp Rate-fader style, slot-coloured).
    //      All write the SELECTED slot; the accent follows the FX slot selector (yellow 1 / pink 2).
    auto pctFader = [](float v){ return juce::String(juce::roundToInt(v * 100.0f)) + "%"; };
    auto setupFxFader = [this](SlotDragFader& fd, const juce::String& name, float dflt,
                               std::function<juce::String(float)> fmt,
                               std::function<void(float)> write, const juce::String& tip) {
        content.addAndMakeVisible(fd);
        fd.setLabel(name); fd.setDefault(dflt); fd.format = std::move(fmt);
        fd.setTooltip(tip);
        fd.onChange = [this, write = std::move(write)](float v) { if (! ignoreKnobCallbacks) write(v); };
        fd.onDragEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    };
    setupFxFader(fxDriveFader, "Drive", 0.0f, pctFader,
        [this](float v){ proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxDrive = v; },
        "How hard THIS slot is pushed into distortion (the Drive TYPE is the dropdown beside it).\n\n"
        "- Per slot: slot 1 and slot 2 distort independently.\n- Drag left/right; double-click = off.\n"
        "- Right-click = MIDI-learn (acts on the selected slot).");
    fxDriveFader.mlm = &proc.midiLearn; fxDriveFader.learnPid = "ui_sel_fxDrive";
    // (The F1/F2 KEYTRACK faders are GONE - keytracking is a per-voice MATRIX route now: Note -> Filter
    //  Cutoff. Factory sounds + old "flK" saves were migrated to routes; the filter visual got the space.)
    setupKnob(knobReverbRoom,  lblRevRoom,  "Size",  0.0, 1.0,  0.5,   1.0, fmtPct);
    setupKnob(knobReverbDecay, lblRevDecay, "Decay", 0.0, 1.0,  0.5,   1.0, fmtPct);
    setupKnob(knobReverbWet,   lblRevWet,   "Wet",   0.0, 1.0,  0.4,   1.0, fmtPct);
    setupKnob(knobReverbPre,   lblRevPre,   "Pre",   0.0, 1.0,  0.0,   1.0,
              [](double v){ return juce::String(juce::roundToInt(v * 120.0)) + " ms"; });   // pre-delay 0..120 ms (not %)
    setupKnob(knobReverbWidth, lblRevWidth, "Width", 0.0, 1.0,  1.0,   1.0, fmtPct);
    setupKnob(knobDelayTime,  lblDelTime, "Time", 0.05, 2.0, 0.375, 1.0, fmtMs);
    setupKnob(knobDelayFB,    lblDelFB,   "Feedback", 0.0, 0.95, 0.3, 1.0, fmtPct);
    setupKnob(knobDelayWet,   lblDelWet,  "Wet",  0.0, 1.0, 0.3, 1.0, fmtPct);   // return level (mirrors reverb Wet)
    knobDelayTime.setSkewFactorFromMidPoint(0.3);
    setupKnob(knobMasterVol,   lblMasterVol,   "Volume", 0.0, 1.0,  0.9, 1.0, fmtPct);
    // Master VOLUME is a horizontal FADER now (lives in the SOUND BLEND box; Pattern Output group removed).
    knobMasterVol.setSliderStyle(juce::Slider::LinearHorizontal);
    knobMasterVol.setTextBoxStyle(juce::Slider::TextBoxRight, true, 38, 16);
    knobMasterVol.setColour(juce::Slider::trackColourId,      juce::Colour(0xffe8947a)); // filled = BLEND orange (slot1+slot2 mix = "everything") [2026-07-15]
    knobMasterVol.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff2a2a44)); // empty (right) = dark
    // Limiter read-out = the output CEILING in dB (concrete) instead of a meaningless %. "Off" at 0.
    setupKnob(knobMasterLimit, lblMasterLimit, "Limit",  0.0, 1.0,  0.003, 1.0,
              [](double v){ return v <= 0.0005 ? juce::String("Off") : juce::String(-0.1 - v * 11.9, 1) + " dB"; });
    knobMasterLimit.setSkewFactorFromMidPoint(0.12);   // start slower at the light end so -0.1..-1.5 dB is easy to dial
    setupKnob(knobMasterGlue,  lblMasterGlue,  "Glue",   0.0, 1.0,  0.0,  1.0, fmtPct);  // 0 = off; master bus compressor
    // Tilt: one-knob tone, read-out shows dark<->bright with a "Flat" centre detent. Sat: 0 = off.
    setupKnob(knobMasterTilt,  lblMasterTilt,  "Tilt",   0.0, 1.0,  0.5,  1.0,
              [](double v){ const int d = juce::roundToInt((v - 0.5) * 12.0);   // +/-6 "clicks"
                            return d == 0 ? juce::String("Flat")
                                          : (d > 0 ? "Br +" + juce::String(d) : "Dk " + juce::String(d)); });
    setupKnob(knobMasterSat,   lblMasterSat,   "Sat",    0.0, 1.0,  0.0,  1.0, fmtPct);   // 0 = off; master saturation


    knobPitch.onValueChange   = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).sampleCrush = (float)knobPitch.getValue(); };
    knobVolume.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).volume  = (float)knobVolume.getValue(); };
    knobPan.onValueChange     = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).pan     = (float)knobPan.getValue(); };
    knobSlices.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].smpSlices = juce::jlimit(1, 16, (int)knobSlices.getValue()); };
    knobCutoff.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).filterCutoff = (float)knobCutoff.getValue(); };
    knobReso.onValueChange    = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).filterReso   = (float)knobReso.getValue(); };
    knobEnvAmt.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).filterEnvAmt = (float)knobEnvAmt.getValue(); };
    knobDrive.onValueChange   = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxDrive       = (float)knobDrive.getValue(); };
    // Auto Test: the per-sound FX knobs (Drive/Reverb/Delay) are editor-level LearnableKnobs, not SlotEditor
    // knobs, so they need their own onDragEnd to play a test hit when "Auto Test" is on (they did nothing before).
    knobDrive.onDragEnd  = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    // Reverb/Delay FLAVOUR is MASTER-level now (write ALL patterns), so it's one shared sound for the whole kit.
    auto allM = [this](std::function<void(Sequencer::MasterFX&)> fn) {
        if (ignoreKnobCallbacks) return; for (auto& p : proc.sequencer.patterns) fn(p.master); };
    // The reverb/delay rows edit the SELECTED bus (masterBusB; the header shows which - right-click switches).
    knobReverbRoom.onValueChange  = [this, allM] { allM([this](Sequencer::MasterFX& m){ (masterBusB ? m.reverbRoomB : m.reverbRoom) = (float)knobReverbRoom.getValue(); }); };
    knobReverbDecay.onValueChange = [this, allM] { allM([this](Sequencer::MasterFX& m){ (masterBusB ? m.reverbDampB : m.reverbDamp) = 1.0f - (float)knobReverbDecay.getValue(); }); };
    knobReverbWet.onValueChange   = [this, allM] { allM([this](Sequencer::MasterFX& m){ (masterBusB ? m.reverbWetB : m.reverbWet) = (float)knobReverbWet.getValue(); }); };
    knobReverbPre.onValueChange   = [this, allM] { allM([this](Sequencer::MasterFX& m){ (masterBusB ? m.reverbPreDelayB : m.reverbPreDelay) = (float)knobReverbPre.getValue(); }); };
    knobReverbWidth.onValueChange = [this, allM] { allM([this](Sequencer::MasterFX& m){ (masterBusB ? m.reverbWidthB : m.reverbWidth) = (float)knobReverbWidth.getValue(); }); };
    knobDelayFB.onValueChange     = [this, allM] { allM([this](Sequencer::MasterFX& m){ (masterBusB ? m.delayFeedbackB : m.delayFeedback) = (float)knobDelayFB.getValue(); }); };
    knobDelayWet.onValueChange    = [this, allM] { allM([this](Sequencer::MasterFX& m){ (masterBusB ? m.delayWetB : m.delayWet) = (float)knobDelayWet.getValue(); }); };
    // AUTO TEST for the master REVERB + DELAY rows: fire a test hit on release so send/tail edits are
    // heard immediately (the per-slot knobs always did this; the master rows were silent - user).
    {
        auto masterAudition = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
        for (juce::Slider* k : { (juce::Slider*) &knobReverbRoom, (juce::Slider*) &knobReverbDecay,
                                 (juce::Slider*) &knobReverbWet,  (juce::Slider*) &knobReverbPre,
                                 (juce::Slider*) &knobReverbWidth,
                                 (juce::Slider*) &knobDelayTime,  (juce::Slider*) &knobDelayFB,
                                 (juce::Slider*) &knobDelayWet })
            k->onDragEnd = masterAudition;
    }
    // Volume / Pan / Limiter are MASTER-level too now (write ALL patterns) - the whole master section
    // is preset-wide, so it doesn't change as the chain moves between patterns.
    knobMasterVol.onValueChange   = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.volume = (float)knobMasterVol.getValue(); }); };
    knobMasterLimit.onValueChange = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.limit  = (float)knobMasterLimit.getValue(); }); };
    // Glue is a MASTER bus compressor (one shared setting for the whole kit) -> write ALL patterns, like the FX flavour.
    knobMasterGlue.onValueChange  = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.glue = (float)knobMasterGlue.getValue(); }); };
    // Tilt + Sat = master TONE/colour (shared kit sound) -> also write ALL patterns.
    knobMasterTilt.onValueChange  = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.tilt = (float)knobMasterTilt.getValue(); }); };
    knobMasterSat.onValueChange   = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.sat  = (float)knobMasterSat.getValue(); }); };

    // Delay Time: free ms, or (when Sync is on) a tempo note-division.
    static const char* divNames[] = { "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2" };
    knobDelayTime.textFromValueFunction = [this](double v) {
        if (proc.masterFX().delaySync)
            return juce::String(divNames[juce::jlimit(0, 6, (int) juce::jmap(v, 0.05, 2.0, 0.0, 6.999))]);
        return v < 1.0 ? juce::String(juce::roundToInt(v * 1000.0)) + "ms" : juce::String(v, 2) + "s";
    };
    knobDelayTime.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        double v = knobDelayTime.getValue();
        const int div = juce::jlimit(0, 6, (int) juce::jmap(v, 0.05, 2.0, 0.0, 6.999));
        for (auto& p : proc.sequencer.patterns)
        {
            auto& m = p.master;
            if (masterBusB) { m.delayTimeB = (float) v; if (m.delaySyncB) m.delayDivisionB = div; }
            else            { m.delayTime  = (float) v; if (m.delaySync)  m.delayDivision  = div; }
        }
    };

    // MASTER vertical drag-faders (the narrow 3/5 strip): visual PROXIES over the 12 knobs above.
    // Each reuses the knob's range/skew/format/default/MIDI-learn (proportionOfLengthToValue), so no
    // master DSP/persistence/learn changed - dragging the fader just drives its hidden knob.
    {
        struct MV { LearnableKnob* k; juce::Label* l; };
        const MV mv[NMVF] = {
            { &knobMasterTilt,  &lblMasterTilt  }, { &knobMasterSat,   &lblMasterSat   },
            { &knobMasterGlue,  &lblMasterGlue  }, { &knobMasterLimit, &lblMasterLimit },
            { &knobReverbRoom,  &lblRevRoom     }, { &knobReverbDecay, &lblRevDecay    },
            { &knobReverbWet,   &lblRevWet      }, { &knobReverbPre,   &lblRevPre      },
            { &knobReverbWidth, &lblRevWidth    }, { &knobDelayTime,   &lblDelTime     },
            { &knobDelayFB,     &lblDelFB       }, { &knobDelayWet,    &lblDelWet      } };
        auto masterAud = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
        for (int i = 0; i < NMVF; ++i)
        {
            auto* k = mv[i].k;  auto& f = masterVF[i];
            f.setVertical(true);
            f.setAccent(juce::Colour(0xffe8947a));   // BLEND orange = master ("both slots" colour, not slot-1 yellow) [2026-07-15]
            f.setLabel({});                          // name goes in the Label BELOW; the fader shows only the value
            f.format    = [k](float v01) { return k->getTextFromValue(k->proportionOfLengthToValue((double) v01)); };
            f.onChange  = [this, k](float v01) { if (! ignoreKnobCallbacks) k->setValue(k->proportionOfLengthToValue((double) v01), juce::sendNotificationSync); };
            f.onDragEnd = masterAud;
            f.setDefault((float) k->valueToProportionOfLength(k->getDoubleClickReturnValue()));
            f.mlm = &proc.midiLearn;  f.learnPid = k->paramId;
            f.setTooltip(k->getTooltip());
            content.addAndMakeVisible(f);
        }
    }
    // [2026-07-15 02:30] SYNC/TRAIL/DUCK/CHARACTER batch: 7 DIRECT master faders + the reverb Sync
    // toggle. Each writes its MasterFX field bus-aware into ALL patterns (flavour = preset-wide).
    {
        auto masterAud = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
        auto initF = [&](SlotDragFader& f, float def) {
            f.setVertical(true); f.setAccent(juce::Colour(0xffe8947a)); f.setLabel({});
            f.setDefault(def); f.onDragEnd = masterAud; content.addAndMakeVisible(f); f.setVisible(false);
        };
        // [2026-07-15 12:10] COUNTED UNITS (user design): every synced fader = a whole number of a
        // FIXED unit (the delay-Time model - the denominator never moves). Normal drag counts the
        // unit; SHIFT-drag = free (the FM-Ratio convention), displayed as an honest DECIMAL.
        // Formats read the STORED field (not the drag position) so snap vs free displays truthfully.
        // Delay: synced Time = ECHOES PER BAR (count 1..21; Shift = fractional, e.g. "5.3 /bar").
        initF(delayBarNF, 7.0f / 20.0f);
        delayBarNF.format   = [this](float) { auto& m = proc.masterFX();
            const float N = masterBusB ? m.delayBarNB : m.delayBarN;
            return (std::abs(N - std::round(N)) < 0.02f ? juce::String(juce::roundToInt(N))
                                                        : juce::String(N, 1)) + " /bar"; };
        delayBarNF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            const float N = std::round(1.0f + v * 20.0f);   // whole echoes-per-bar (SHIFT = fine drag, still counted)
            for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.delayBarNB : p.master.delayBarN) = N; };
        delayBarNF.setTooltip("Echoes per bar (Sync is on).\n\n"
            "- The delay time = one bar divided by this number - 7 lands every echo on a 7-step grid, "
            "14 on a 14-step grid, and so on up to 21.\n"
            "- This is the delay's 'room size': the reverb's Size fader plays the same role in meters.\n"
            "- SHIFT-drag = fine control.\n"
            "- Follows the tempo AND time signature (the host's when DAW Sync is on, the toolbar's otherwise).\n"
            "- Turn Sync off for free milliseconds instead.");
        // MAX TRAIL: hard echo count (top = unlimited = the classic endless-fade delay).
        initF(delayTrailF, 1.0f);
        delayTrailF.format   = [](float v) { const int i = juce::roundToInt(v * 21.0f);
                                             return i >= 21 ? juce::String("No limit") : juce::String(i + 1) + (i == 0 ? " echo" : " echoes"); };
        delayTrailF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            const int i = juce::roundToInt(v * 21.0f); const int t = i >= 21 ? 0 : i + 1;
            for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.delayTrailB : p.master.delayTrail) = t; };
        delayTrailF.setTooltip("Max trail: a HARD cap on how many echoes play.\n\n"
            "- Independent of Feedback: crank Feedback for LOUD near-equal echoes, cap the trail at 4 = "
            "a fat trail that just STOPS (impossible with feedback alone, which only fades gradually).\n"
            "- Set it to your echoes-per-bar and the trail ends exactly at the bar line.\n"
            "- Top = no limit (the classic delay, default).\n"
            "- The reverb's GATE does the same job for the wash - length capped, loudness untouched.");
        // DUCK: echoes tuck under the dry mix, bloom in the gaps.
        initF(delayDuckF, 0.0f);
        delayDuckF.format   = [](float v) { return v < 0.005f ? juce::String("Off") : juce::String(juce::roundToInt(v * 100.0f)) + "%"; };
        delayDuckF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.delayDuckB : p.master.delayDuck) = v; };
        delayDuckF.setTooltip("Duck: the echoes get QUIETER while the dry drums are playing and bloom "
            "up in the gaps between hits.\n\n"
            "- Big audible trail + a clean, punchy pattern at the same time (masking alone can't do this - "
            "the echo energy is genuinely removed under the hits, not just blurred).\n"
            "- Listens to the AUDIO of the whole dry mix, so steps, piano roll, live keys and the arp all "
            "duck it - nothing to wire up.\n"
            "- Fixed feel: ~10 ms dip, ~200 ms swell-back. 0 = off.");
        // CHARACTER: the depth of the delay MODE's flavour (title picks the mode; this is how much).
        initF(delayCharF, 0.5f);
        delayCharF.format   = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; };
        delayCharF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.delayCharB : p.master.delayChar) = v; };
        delayCharF.setTooltip("Character: HOW MUCH of the delay mode's flavour (the title picks WHICH flavour).\n\n"
            "- Tape: how fast repeats darken.\n- Dub: how hard each repeat saturates.\n"
            "- Analog: how deep the pitch wobble.\n- Shimmer: how much octave-up per pass.\n"
            "- Digital has no flavour - this fader is inert there.\n- 50% = the mode's standard voicing.");
        // Reverb synced variants [2026-07-15 12:10 counted units]: Decay counts QUARTER-BARS
        // (1.5-bar tails exist now), Pre counts 64THS. Shift = free decimal.
        initF(revDecBarsF, (12.0f - 3.0f) / 45.0f);   // default 1 bar = 12 twelfths
        revDecBarsF.format   = [this](float) { auto& m = proc.masterFX();
            const float b = masterBusB ? m.reverbDecBarsB : m.reverbDecBars;
            const float q = b * 4.0f;   // quarters display clean ("1.5 bars"); thirds as decimals ("0.33 bars")
            if (std::abs(q - std::round(q)) < 0.02f)
            { const float br = std::round(q) / 4.0f;
              const juce::String num = std::abs(br - std::round(br)) < 0.01f ? juce::String(juce::roundToInt(br))
                                     : juce::String(br, std::abs(br * 2.0f - std::round(br * 2.0f)) < 0.01f ? 1 : 2);
              return num + (br > 1.01f ? " bars" : " bar"); }
            return juce::String(b, 2) + (b > 1.01f ? " bars" : " bar"); };
        revDecBarsF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            // [2026-07-15 13:30] counts TWELFTHS of a bar, 3..48 = 0.25..4 bars (twelfths express BOTH
            // the quarter family and the 0.33/0.66 triplet family the user asked for; 8 bars was
            // ambient overkill + made the stops twitchy). SHIFT = fine drag, still counted.
            const float t = std::round(3.0f + v * 45.0f);
            const float b = juce::jlimit(0.25f, 4.0f, t / 12.0f);
            for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.reverbDecBarsB : p.master.reverbDecBars) = b; };
        revDecBarsF.setTooltip("Decay, synced: the tail dies (~-60 dB) this many bars after a hit.\n\n"
            "- Counts 12ths of a bar: quarters (0.25, 0.5, 1.5...) AND thirds (0.33, 0.66...) up to 4 bars. "
            "SHIFT-drag = fine control.\n"
            "- The reverb feedback is recomputed LIVE from Size, Mode and the tempo, so the tail keeps "
            "landing on the bar line when any of them change.\n"
            "- Plays the role FEEDBACK plays on the delay (how long until silence).\n"
            "- Approximate by nature (~): damping makes highs die a little sooner.");
        initF(revPreBarsF, 2.0f / 8.0f);
        revPreBarsF.format   = [this](float) { auto& m = proc.masterFX();
            const float b = masterBusB ? m.reverbPreBarsB : m.reverbPreBars;
            const float n = b * 64.0f;
            if (std::abs(n - std::round(n)) < 0.02f)
            { const int ni = juce::roundToInt(n);
              return ni == 0 ? juce::String("Off") : juce::String(ni) + "/64 bar"; }
            return juce::String(b, 3) + " bar"; };
        revPreBarsF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            const float b = std::round(v * 0.125f * 64.0f) / 64.0f;  // 0..8 sixty-fourths (SHIFT = fine drag)
            for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.reverbPreBarsB : p.master.reverbPreBars) = b; };
        revPreBarsF.setTooltip("Pre-delay, synced: the silence between a sound's TRIGGER (not its end) and "
            "the moment its reverb tail STARTS, counted in 64THS of a bar (1/64 = ~30 ms at 120 BPM - "
            "classic drum pre-delay).\n\n"
            "- Hit -> gap -> wash: the transient stays punchy and the reverb sits BEHIND the sound "
            "like a room, instead of on top of it.\n- SHIFT-drag = fine control.");
        // GATE [2026-07-15 12:10]: follows the row's Sync - counted 16THS of a bar when synced,
        // free MILLISECONDS when not. Each mode keeps its OWN stored value.
        initF(revGateF, 0.0f);
        revGateF.format   = [this](float) { auto& m = proc.masterFX();
            if (masterBusB ? m.reverbSyncB : m.reverbSync)
            { const float b = masterBusB ? m.reverbGateB : m.reverbGate;
              return b < 0.005f ? juce::String("Off") : juce::String(b, 2) + " bar"; }
            const float ms = masterBusB ? m.reverbGateMsB : m.reverbGateMs;
            return ms < 1.0f ? juce::String("Off") : juce::String(juce::roundToInt(ms)) + " ms"; };
        revGateF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            // [2026-07-15 13:30] plain DECIMAL bar fraction (user: no /16 counting; SHIFT = fine drag)
            if (masterBusB ? proc.masterFX().reverbSyncB : proc.masterFX().reverbSync)
            { const float b = v < 0.005f ? 0.0f : v;
              for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.reverbGateB : p.master.reverbGate) = b; }
            else
            { const float ms = v < 0.005f ? 0.0f : v * 1000.0f;      // free 0..1000 ms
              for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.reverbGateMsB : p.master.reverbGateMs) = ms; } };
        revGateF.setTooltip("Gate: the wet tail is CUT DEAD this long after each hit - the 80s gated-reverb "
            "chop (think huge 'In the Air Tonight' snares).\n\n"
            "- The reverb's version of the delay's MAX TRAIL: length capped, loudness untouched - crank "
            "Decay for a dense loud wash, gate it at 2/16 = thick burst, then silence.\n"
            "- Sync on = a bar fraction (\"0.25 bar\"); Sync off = free milliseconds. Each mode "
            "remembers its own setting. SHIFT-drag = fine control.\n"
            "- Closes with a ~5 ms fade so it never clicks.\n- Bottom = off.");
        // Reverb SYNC = a single-row lit button [2026-07-15 12:10] (styled at the Sync/Ping site).
        swReverbSync.setTooltip("Sync the reverb to the tempo: Decay becomes a tail length in BARS, "
            "Pre-delay a bar fraction, and Gate counts 16ths.\n\n- Off = the classic free knobs "
            "(percent / milliseconds).");
        swReverbSync.onClick = [this] { if (ignoreKnobCallbacks) return;
            const bool on = swReverbSync.getToggleState();
            for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.reverbSyncB : p.master.reverbSync) = on;
            refreshMasterSyncFaders(); refreshDetailPanel(); };
        // Master WIDTH [2026-07-15 12:10] (the 5th top-row fader): bass-safe M/S stereo width.
        initF(masterWidthF, 1.0f / 1.5f);
        masterWidthF.format   = [](float v) { return juce::String(juce::roundToInt(v * 150.0f)) + "%"; };
        masterWidthF.onChange = [this](float v) { if (ignoreKnobCallbacks) return;
            for (auto& p : proc.sequencer.patterns) p.master.masterWidth = v * 1.5f; };
        masterWidthF.setTooltip("Master stereo width.\n\n"
            "- 100% = natural (off). Below = narrower (0% = mono); above = wider, up to 150%.\n"
            "- Widening is BASS-SAFE: side content below ~120 Hz stays centred, so kick and sub "
            "keep their mono punch (club-safe).\n"
            "- The reverb row's Width is the same idea applied to the reverb tail only.");
        content.addAndMakeVisible(lblMasterWidth);
        // attach the new faders' name labels [2026-07-15 12:10] - they were styled+positioned but
        // never PARENTED, so they didn't draw (the user's "some faders have no text" report)
        content.addAndMakeVisible(lblDelTrail); content.addAndMakeVisible(lblDelDuck);
        content.addAndMakeVisible(lblDelChar);  content.addAndMakeVisible(lblRevGate);
        // The proxy Wet faders become header PILLS (same objects, horizontal, placed in layoutContent).
        masterVF[6].setLabel("Wet");  masterVF[11].setLabel("Wet");
        // Live read-outs on the proxies: Decay shows the ESTIMATED tail (~seconds, follows Size/Mode/
        // tempo in real time); Feedback shows the ~audible-echo count beside the %.
        masterVF[5].format = [this](float) {
            auto& m = proc.masterFX();
            const float t = FDNReverb::estimateT60(1.0f - (masterBusB ? m.reverbDampB : m.reverbDamp),
                                                   masterBusB ? m.reverbRoomB : m.reverbRoom,
                                                   juce::jlimit(0, 3, masterBusB ? m.reverbModeB : m.reverbMode));
            return "~" + (t < 9.95f ? juce::String(t, 1) : juce::String(juce::roundToInt(t))) + " s";
        };
        masterVF[10].format = [this](float v01) {   // [2026-07-15 16:00] % + the EXACT dB step per echo
            const float g = juce::jlimit(0.0f, 0.98f, (float) knobDelayFB.proportionOfLengthToValue((double) v01));
            juce::String pct = juce::String(juce::roundToInt(g * 100.0f)) + "%";
            if (g <= 0.005f) return pct;
            const float db = 20.0f * std::log10(g);   // >= 10 dB -> whole number (JUCE String(x, 0) prints RAW floats - the 4-decimals bug)
            return pct + " " + (std::abs(db) < 9.95f ? juce::String(db, 1) : juce::String(juce::roundToInt(db))) + "dB";
        };
        // [2026-07-15 02:30] TOOLTIPS on the 12 proxy faders (user: "master faders have no tooltips" -
        // the proxy loop copied each knob's tooltip BEFORE the knob tooltips were assigned = empty).
        masterVF[0].setTooltip("Tilt: one-knob master tone around ~700 Hz.\n\n- Below 50% = darker (bass up, "
            "treble down), above = brighter. 50% = flat (off).\n- +-6 dB at the extremes.");
        masterVF[1].setTooltip("Sat: master tube-style saturation.\n\n- Adds warm EVEN harmonics = fatter, "
            "louder-feeling mix without clipping.\n- Distinct from Glue (a compressor - dynamics, no harmonics).\n- 0 = off.");
        masterVF[2].setTooltip("Glue: master bus compressor.\n\n- Gently squeezes the loudest moments so the "
            "kit sounds like ONE instrument (the classic bus-glue trick).\n- Adds no harmonics - that's Sat's job.\n- 0 = off.");
        masterVF[3].setTooltip("Limiter: a brick-wall PEAK ceiling on the whole mix.\n\n"
            "- Below the ceiling it does NOTHING (unity gain) - it is not a volume cut.\n"
            "- The instant a peak would cross, the WHOLE mix ducks just enough for that peak to land "
            "exactly at the ceiling (~1 ms lookahead attack), then recovers in ~60 ms. Ceiling -5 dB "
            "+ a peak at +1 dB = that moment plays at -5; quiet parts stay untouched.\n"
            "- A mix CONSTANTLY above the ceiling = constant reduction = audible squash/pump.\n"
            "- This is the safeguard for channel-volume boosts above 0 dB.\n"
            "- -0.1 dB = transparent protection (default); lower = deliberate loudness squash.");
        // Size shows an honest ROOM DIMENSION (~meters, from the tank's loop time) instead of a
        // meaningless % [2026-07-15 12:10] - the delay's Time ("N /bar") and this are the same
        // knob in two kinds of space.
        masterVF[4].format = [this](float) {
            auto& m = proc.masterFX();
            const float mtr = FDNReverb::meanLoopSec(masterBusB ? m.reverbRoomB : m.reverbRoom,
                                                     juce::jlimit(0, 3, masterBusB ? m.reverbModeB : m.reverbMode),
                                                     48000.0) * 343.0f;
            return "~" + juce::String(juce::roundToInt(mtr)) + " m";
        };
        masterVF[4].setTooltip("Size: how BIG the reverb's room is - shown as an approximate room "
            "dimension in meters.\n\n- ~4 m = a booth, ~12 m = a club, ~20 m+ = a hall.\n"
            "- Size = the room's dimensions; the MODE (title click) = what its walls are made of - "
            "two different things.\n"
            "- The Decay fader's ~seconds estimate follows Size live.\n"
            "- Same job as the delay's Time fader: that one measures its 'room' in echoes-per-bar, "
            "this one in meters.");
        masterVF[5].setTooltip("Decay: how long the tail rings.\n\n- The value shows the ESTIMATED tail length "
            "(~seconds, to about -60 dB) - it updates live when Size, Mode or the tempo change.\n"
            "- Approximate by nature (~): damping makes highs die a little sooner.\n"
            "- Turn the row's Sync on to set it in BARS instead.");
        masterVF[6].setTooltip("Reverb Wet: the return level of the whole reverb bus.\n\n- Per-sound amounts "
            "live on each channel's Rev send fader; this scales what comes back.\n- Right-click = MIDI-learn.");
        masterVF[7].setTooltip("Pre-delay: the silence between a sound's TRIGGER (not its end) and the moment "
            "its reverb tail STARTS (0-120 ms).\n\n- Hit -> gap -> wash: the transient stays punchy and the "
            "reverb sits BEHIND the sound like a room, instead of on top of it. Drums love 10-40 ms.\n"
            "- Turn the row's Sync on to set it as a bar fraction instead.");
        masterVF[8].setTooltip("Width: the reverb's stereo spread.\n\n- 100% = full wide tail, 0 = mono/narrow "
            "(tight, centred).");
        masterVF[9].setTooltip("Delay time in milliseconds (free).\n\n- Turn Sync on to set it as ECHOES PER BAR "
            "instead (locks to any step grid, incl. 7/11/13).\n"
            "- Same job as the reverb's Size: this is the delay's 'room', measured in echo spacing.");
        masterVF[10].setTooltip("Feedback: how much of each echo survives into the next.\n\n"
            "- The dB figure is EXACT: every echo is that many dB quieter than the one before "
            "(50% = -6.0 dB per echo, always - a constant ratio IS a constant dB step).\n"
            "- Echo counts are a CEILING, not a promise: 50% ~ 10 echoes down to -60 dB, soloed in "
            "silence. In a busy mix you HEAR fewer (masking), the Wet level lowers the whole trail, "
            "and Tape/Dub modes shave extra energy per pass.\n"
            "- Want a LOUD trail that ends abruptly? That's the Trail fader.\n"
            "- Plays the role DECAY plays on the reverb (how long until silence).");
        masterVF[11].setTooltip("Delay Wet: the return level of the whole delay bus.\n\n- Per-sound amounts "
            "live on each channel's Dly send fader; this scales what comes back.\n- Right-click = MIDI-learn.");
    }
    setupGroupHeader(hdrModulation, "MODULATION");   // new group (holds the MOD/LFO visual for now)

    //-- Sample pitch envelope (knobs live in the "Pitch & Level" group now)
    setupKnob(knobPEnvAmt,  lblPEnvAmt,  "P.Env",  -48.0, 48.0, 0.0,  1.0, fmtSemi);
    setupKnob(knobPEnvTime, lblPEnvTime, "P.Time", 0.001, 1.0,  0.05, 0.3, fmtMs);
    knobPEnvAmt.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).pitchEnvAmt  = (float)knobPEnvAmt.getValue(); };
    knobPEnvTime.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).pitchEnvTime = (float)knobPEnvTime.getValue(); };

    //-- Sounds section: 4 source toggle-switches + 2D blend pad + per-source knobs
    setupGroupHeader(hdrSounds, "MASTER");
    hdrSounds.setColour(juce::Label::textColourId, juce::Colour(0xffe8947a));   // BLEND orange = global/master (the "both slots" mix colour) [2026-07-15]
    for (auto& e : boxEngine) e = -1;
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    {
        content.addAndMakeVisible(slotCombo[b]);
        slotCombo[b].setLookAndFeel(&bigComboLNF);
        slotCombo[b].setTooltip("Pick this slot's sound engine (or None). 'Sample' opens a submenu of your "
                                "samples - pick one directly. You can choose the same engine in more than one "
                                "slot for layered / detuned sounds. NOTE: Physical is the heaviest engine.");
        slotCombo[b].onChange = [this, b] { if (!ignoreKnobCallbacks) onSlotEngineChange(b); };
        content.addAndMakeVisible(slotEd[b]);
        slotEd[b].init(b, proc.midiLearn, &knobLNF,
                       [this, b]() -> DrumChannel::Slot* { return &proc.sequencer.channel(selectedChannel).slots[b]; },
                       [this, b]() {
                           auto& ch = proc.sequencer.channel(selectedChannel);
                           if (ch.slots[b].engine == DrumChannel::SrcGrain)
                               ch.rebuildGrainTables();   // the grain SOURCE follows wave edits live
                           ch.markDspDirty();
                       });
        // "Auto" toggle ON: releasing a slot knob fires a TEST hit so you hear the edit (default OFF).
        slotEd[b].onAudition = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
        // DRAG one slot's box onto the other = copy its whole setup (engine, params, env, EQ,
        // filters, FX, sample) - the slot version of the channel-number drag-copy (user order).
        slotEd[b].onCopyFromSlot = [this, b](int srcIdx) {
            if (srcIdx == b || srcIdx < 0 || srcIdx >= DrumChannel::NUM_SLOTS) return;
            auto& ch = proc.sequencer.channel(selectedChannel);
            commitUndoNow();
            ch.silenceAllVoices();                 // nothing reads the old buffers mid-copy
            ch.slots[b] = ch.slots[srcIdx];        // copies the ENGINE + every param
            ch.slotSample[b] = ch.slotSample[srcIdx];
            ch.ensureKsBuffers();
            ch.rebuildAddTables();                 // chains the grain-source rebuild too
            ch.rebuildGrainTables();
            ch.markDspDirty();
            // The DATA is copied, but the UI's engine mirror (boxEngine[]) must RE-SYNC or the layout
            // rebuilds the destination editor with the STALE engine (the "it stays empty/Modal" bug).
            syncBoxesFromSrcOn();                  // boxEngine[] <- the copied slots[].engine
            slotEd[b].setEngine(ch.slots[b].engine);
            slotEd[b].pushValues();
            cacheWaveform(selectedChannel);
            syncPadFromSlots(false);
            layoutContent(); refreshDetailPanel();
        };
        // Sample Stretch was changed -> re-bake that slot's buffer (SoundTouch) + refresh the waveform.
        slotEd[b].onSampleEdit = [this](int box) {
            auto& ch = proc.sequencer.channel(selectedChannel);
            ch.updateStretch(box); cacheWaveform(selectedChannel);
        };
        // Hz <-> note read-out mode is session-wide: when one slot toggles it, refresh the other too.
        slotEd[b].onFreqModeToggled = [this, b] { slotEd[1 - b].pushValues(); };
        // Clicking a slot box (bare area) selects it as the edit target (user: "clicking the slot
        // should switch to it"). Child knobs/faders/combos consume their own clicks, so this only
        // fires on the box background - a harmless convenience on top of the 1/2 slot selectors.
        slotEd[b].onSelect = [this, b] { if (! ignoreKnobCallbacks) setShapeSlot(b); };
        // Per-slot accent: Slot 1 = yellow, Slot 2 = pink (knobs + faders).
        slotEd[b].setAccent(b == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8));
        // Drop an audio file on the box (any engine) or on its waveform -> this slot becomes a
        // Sample playing that file (same path as picking it from the menu).
        auto dropLoad = [this, b](const juce::File& f) {
            auto& ch = proc.sequencer.channel(selectedChannel);
            // A GRANULAR slot keeps its engine: the dropped file becomes the GRAIN SOURCE
            // (that is how you feed the granulator a sample). Any other engine -> Sample.
            if (ch.slots[b].engine != DrumChannel::SrcGrain)
            { boxEngine[b] = DrumChannel::SrcSample; ch.slots[b].engine = DrumChannel::SrcSample; }
            ch.loadUserSample(b, f); cacheWaveform(selectedChannel);
            syncPadFromSlots(true); ch.markDspDirty();
            layoutContent(); refreshDetailPanel();
        };
        slotEd[b].onFileDropped   = dropLoad;
        waveform[b].onFileDropped = dropLoad;
        // ADDITIVE Custom wave: the preview reads the baked table; clicking it opens the harmonic editor.
        slotEd[b].morphView.getCustomTbl = [this, b]() -> const float* {
            auto& ch = proc.sequencer.channel(selectedChannel);
            const auto& sl = ch.slots[b];
            // in MOTION (glide / WAVE LFO) the split view starts at frame A; a STATIC position
            // shows the EXACT two-frame blend the DSP plays (message-thread scratch, read
            // immediately by the paint that asked for it).
            if (sl.addSeg[0] > 0.001f || sl.lfoAmt[3] > 0.001f || sl.addPos <= 0.001f)
                return ch.addTbl[b][0];
            static float blend[DrumChannel::ADD_TBL];
            const float wtp = juce::jlimit(0.0f, 1.0f, sl.addPos) * (float)(DrumChannel::ADD_FRAMES - 1);
            const int   f0 = juce::jmin((int) wtp, DrumChannel::ADD_FRAMES - 2);
            const float fm = wtp - (float) f0;
            for (int i = 0; i < DrumChannel::ADD_TBL; ++i)
                blend[i] = ch.addTbl[b][f0][i] + (ch.addTbl[b][f0 + 1][i] - ch.addTbl[b][f0][i]) * fm;
            return blend; };
        slotEd[b].morphView.getCustomFrame = [this, b](int f) -> const float* {
            return proc.sequencer.channel(selectedChannel)
                       .addTbl[b][juce::jlimit(0, DrumChannel::ADD_FRAMES - 1, f)]; };
        slotEd[b].morphView.onOpenCustom = [this, b] {
            {   // not on Custom yet? switch to it, seeding the 4 frames from the CURRENT shape's
                // recipe (only when the frames are still the untouched default) - same tone, now drawable
                auto& ch = proc.sequencer.channel(selectedChannel);
                auto& sl = ch.slots[b];
                if (sl.oscShape < DrumChannel::WvCustom)
                {
                    bool untouched = true;
                    for (int f = 0; f < DrumChannel::ADD_FRAMES && untouched; ++f)
                        for (int k = 0; k < DrumChannel::ADD_HARM && untouched; ++k)
                            untouched = std::abs(sl.addH[f][k] - (k == 0 ? 1.0f : 0.0f)) < 1.0e-4f
                                     && std::abs(sl.addPh[f][k]) < 1.0e-4f;
                    if (untouched)
                    {   float mg[DrumChannel::ADD_HARM], phr[DrumChannel::ADD_HARM];
                        addShapeRecipe(juce::jlimit(0, DrumChannel::WvCustom - 1, sl.oscShape), mg, phr);
                        for (int f = 0; f < DrumChannel::ADD_FRAMES; ++f)
                            for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
                            { sl.addH[f][k] = mg[k]; sl.addPh[f][k] = phr[k]; }
                    }
                    sl.oscShape = sl.oscShapeB = DrumChannel::WvCustom;
                    ch.rebuildAddTables(); ch.markDspDirty();
                    refreshDetailPanel();
                    slotEd[b].pushValues();   // the Wave fader itself (refreshDetailPanel alone left it stale)
                }
            }
            harmEdSlot = b;
            harmEd.slotIdx = b;
            harmEd.clickIgnore = &slotEd[b].morphView;   // the preview's own click re-opens, never toggle-fights
            harmEd.accent = b == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8);
            { auto& sl = proc.sequencer.channel(selectedChannel).slots[b];
              harmEd.setValues(sl.addH, sl.addPh, sl.addSeg, sl.addPos, sl.addLoop); }
            harmEd.setVisible(true); harmEd.toFront(false);
        };
    }
    // The DRAW HARMONICS overlay (shared by both slots; a content child, closed by X / layoutContent).
    content.addChildComponent(harmEd);
    harmEd.onChange = [this] {
        auto& ch = proc.sequencer.channel(selectedChannel);
        auto& sl = ch.slots[juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, harmEdSlot)];
        for (int k = 0; k < DrumChannel::ADD_HARM; ++k)
            for (int f = 0; f < DrumChannel::ADD_FRAMES; ++f)
            { sl.addH[f][k] = harmEd.vals[f][k]; sl.addPh[f][k] = harmEd.phs[f][k]; }
        for (int k = 0; k < DrumChannel::ADD_FRAMES - 1; ++k) sl.addSeg[k] = harmEd.seg[k];
        sl.addLoop = harmEd.loopOn;
        sl.addPos = harmEd.wtPos;
        ch.rebuildAddTables(); ch.markDspDirty();
        slotEd[harmEdSlot].morphView.repaint();
    };
    harmEd.onDragEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    harmEd.onClose   = [this] { harmEd.setVisible(false); };
    content.addAndMakeVisible(lblPadHint);
    lblPadHint.setText("Drag the yellow dot to\nadjust sound levels.", juce::dontSendNotification);
    lblPadHint.setFont(juce::Font(12.5f)); lblPadHint.setJustificationType(juce::Justification::centred);
    lblPadHint.setColour(juce::Label::textColourId, juce::Colour(0xff90a0c0));

    // Channel A-H-D-S-R envelope editor + which-slots dropdown (in the SOUND BLEND box).
    // Shared shape editors (amp env + pitch + voice), all driven by one selected slot.
    content.addAndMakeVisible(envEditor);
    // Auto-audition also for the VISUAL editors (amp/pitch/voice) - fire a TEST hit on drag-release, like knobs do.
    auto auditionEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    envEditor.mlm = &proc.midiLearn;   // right-click = MIDI-learn the 5 env params (selected slot)
    envEditor.onChange = [this](float a, float h, float d, float s, float r) {
        if (ignoreKnobCallbacks) return; applyEnvToTargets(a, h, d, s, r); };
    envEditor.onDragEnd = auditionEnd;
    // Sample slots: double-click the amp-env graph = toggle the OPT-IN sample envelope on/off.
    envEditor.onToggleRequest = [this] {
        auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        if (sl.engine != DrumChannel::SrcSample) return;
        sl.smpEnvOn = ! sl.smpEnvOn;
        proc.sequencer.channel(selectedChannel).markDspDirty();
        loadEnvIntoEditor();
        if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel);
    };
    content.addAndMakeVisible(pitchEditor);   // its varispeed warning is in PitchEnvDisplay::getTooltip()
    pitchEditor.onChange = [this](const float* pp, const float* tt) {
        if (ignoreKnobCallbacks) return;
        auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        for (int i = 0; i < DrumChannel::Slot::NPE; ++i) { sl.pEnvP[i] = pp[i]; sl.pEnvT[i] = tt[i]; }
        proc.sequencer.channel(selectedChannel).markDspDirty();
    };
    pitchEditor.onDragEnd = auditionEnd;
    voiceMod.onDragEnd = auditionEnd;

    auto pickSlot = [this](int s) { if (!ignoreKnobCallbacks) setShapeSlot(s); };
    content.addAndMakeVisible(slotSelAmp);   slotSelAmp.onSelect   = pickSlot;
    content.addAndMakeVisible(slotSelPitch); slotSelPitch.onSelect = pickSlot;
    content.addAndMakeVisible(slotSelVoice); slotSelVoice.onSelect = pickSlot;   // under UNISON/DETUNE/VIBRATO
    content.addAndMakeVisible(slotSelFx);    slotSelFx.onSelect    = pickSlot;
    content.addAndMakeVisible(slotSelMod);   slotSelMod.onSelect   = pickSlot;   // MODULATION box 1/2 picker
    setupGroupHeader(hdrPitch, "PITCH ENVELOPE");
    setupGroupHeader(hdrVoice, "UNISON");   // sub-title above the voice visual (detune/vibrato; SCALE moved above the keyboard)
    setupGroupHeader(hdrAmpEnv, "AMP ENVELOPE");
    setupGroupHeader(hdrEqBox,  "FILTER");
    // The FILTER is PER SLOT (the "All" channel target + the static EQ were removed, v1.3.5). Pick which
    // slot's filter you edit. s = slot index (0/1); eqEditTarget = s + 1 (1 or 2, never 0/All). Labels
    // stay "1"/"2" so the SlotSelector keeps its yellow/pink slot colours.
    slotSelEq.labels = { "1", "2" };
    content.addAndMakeVisible(slotSelEq);
    slotSelEq.onSelect = [this](int s) { if (ignoreKnobCallbacks) return; setShapeSlot(s); };   // syncs ALL groups (like the others)
    freqDisplay.onFilterDriveEdit = [this](float v) {   // FILTER DRIVE (selected slot; drives both its SVFs)
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.slots[juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, eqEditTarget - 1)].filterDrive = v;
        ch.markDspDirty();
    };

    // Unison / Detune / Vibrato as an interactive VISUAL (like the amp/pitch env editors), editing the
    // selected slot. The 1/2/3 selector (slotSelPitch) above it chooses which slot.
    content.addAndMakeVisible(voiceMod);
    voiceMod.mlm = &proc.midiLearn;   // right-click = MIDI-learn the 5 unison params (selected slot)
    voiceMod.onChange = [this](int u, float d, float v, bool centre, int detuneMode, int chordMode, bool scaleOn, int scaleType, int scaleKey, float uniSpread, float driftV) {
        if (ignoreKnobCallbacks) return;
        auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        sl.oscUnison = u;   // the visual edits the STD count only now (the Scale count lives in the SCALE box)
        sl.oscDetune = d; sl.vibrato = v; sl.oscUniCenter = centre; sl.oscDetuneMode = detuneMode;
        sl.uniSpread = uniSpread;   // STEREO WIDTH (0 = mono, bit-identical)
        sl.drift = driftV;          // DRIFT (0 = perfectly repeating = bit-identical)
        sl.chordMode = chordMode;
        sl.scaleOn = scaleOn; sl.scaleType = scaleType; sl.scaleKey = scaleKey;   // SCALE (diatonic harmonizer)
        proc.sequencer.channel(selectedChannel).markDspDirty();
    };

    static const char* srcNames[5] = { "Sample", "Noise", "Oscillator", "FM", "Karplus-Strong" };
    for (int i = 0; i < 5; ++i)
    {
        content.addAndMakeVisible(lblSrc[i]);
        lblSrc[i].setText(srcNames[i], juce::dontSendNotification);
        lblSrc[i].setFont(juce::Font(12.5f)); lblSrc[i].setColour(juce::Label::textColourId, juce::Colours::white);
        content.addAndMakeVisible(srcSwitch[i]);
        srcSwitch[i].onClick = [this] { if (!ignoreKnobCallbacks) onSoundToggle(); };
    }

    content.addAndMakeVisible(soundPad);
    soundPad.onChange = [this] {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        // The pad now has one corner per SLOT, so each slot (even two of the same
        // engine) gets its own independent weight straight from the pad.
        for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) ch.slots[b].weight = soundPad.weights[b];
        ch.padX = soundPad.dotX; ch.padY = soundPad.dotY;
    };
    // BLEND fader (replaces the vector pad now that there are 2 slots): left = Slot 1, right = Slot 2.
    content.addAndMakeVisible(blendFader);
    blendFader.setSliderStyle(juce::Slider::LinearVertical);   // stacked slots -> vertical (top = Slot 1)
    blendFader.setRange(0.0, 1.0, 0.0);
    blendFader.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    blendFader.setLookAndFeel(&blendLNF);                       // pink (top/Slot 1) / yellow (bottom/Slot 2)
    blendFader.setTooltip("Blend between the two stacked sound slots: top = Slot 1 only, bottom = Slot 2 only, centre = 50/50.");
    blendFader.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        const float x = 1.0f - (float) blendFader.getValue();   // top of the fader = Slot 1 (padX 0)
        const bool o0 = ch.slots[0].engine >= 0, o1 = ch.slots[1].engine >= 0;
        if (o0 && o1) { ch.slots[0].weight = 1.0f - x; ch.slots[1].weight = x; }
        else          { ch.slots[0].weight = o0 ? 1.0f : 0.0f; ch.slots[1].weight = o1 ? 1.0f : 0.0f; }
        ch.padX = x;   // persist the blend
        const int s2pct = juce::roundToInt(x * 100.0f);
        lblBlendTitle.setText(juce::String(100 - s2pct) + "%", juce::dontSendNotification);
        lblBlendBot.setText(juce::String(s2pct) + "%", juce::dontSendNotification);
        ch.markDspDirty();
    };
    blendFader.onDragEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };

    // Live master-volume meter built into the logo step ramp (display only). Sits over the SVG steps.
    content.addAndMakeVisible(logoMeter);
    logoMeter.setBounds(8, 24, 150, 12);   // under the wordmark, same width

    // Blend-pad A/B layout: only meaningful with all 4 sources on, where it swaps
    // the two TOP corners so a pair that was on opposing corners (capped at 25%
    // each) becomes adjacent and can be pushed much higher.
    content.addAndMakeVisible(btnPadLayout);
    btnPadLayout.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2a4a));
    btnPadLayout.onClick = [this] {
        if (ignoreKnobCallbacks) return;
        auto& ch = proc.sequencer.channel(selectedChannel);
        ch.padLayoutB = ! ch.padLayoutB;
        soundPad.layoutB = ch.padLayoutB;
        soundPad.recompute();   // re-derive weights for the new corner map (writes back via onChange)
        btnPadLayout.setButtonText(ch.padLayoutB ? "B" : "A");
        soundPad.repaint();
    };
    btnPadLayout.setTooltip(
        "Blend layout A / B (with 4-5 sources on): re-arranges which sources are NEIGHBOURS on the pad.\n\n"
        "- Sources on OPPOSITE corners can never both be loud at once.\n"
        "- If the pair you want to push up together is opposite in A, switch to B (it becomes adjacent).\n"
        "- Changes nothing by itself - only which blends are reachable.");

    // Sample chooser (moved here from the channel strip) — "Sample" as a group header
    setupGroupHeader(lblSampleSel, "SAMPLE");
    content.addAndMakeVisible(comboSampleSel);
    comboSampleSel.setLookAndFeel(&bigComboLNF);   // larger text for the sample name
    comboSampleSel.setTextWhenNothingSelected("(load a sample)");
    comboSampleSel.onChange = [this] { if (!ignoreKnobCallbacks) handleSampleSelChange(); };

    // Per-slot cached waveform + region (trim) toggle + length read-out (one set per Sample slot).
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    {
        content.addAndMakeVisible(waveform[b]);
        waveform[b].onRegionsChange = [this, b](int n, const float* lo, const float* hi) {
            auto& sl = proc.sequencer.channel(selectedChannel).slots[b];
            sl.smpRegN = juce::jlimit(0, DrumChannel::Slot::MAXREG, n);
            for (int r = 0; r < sl.smpRegN; ++r) { sl.smpRegLo[r] = lo[r]; sl.smpRegHi[r] = hi[r]; }
            proc.sequencer.channel(selectedChannel).markDspDirty();
            updateSampleLengthLabel();
        };
        waveform[b].setTooltip("This slot's sample. With 'Trim' ON, DRAG to draw up to 4 play regions (green, yellow, "
                               "pink, cyan) - they can overlap, and each hit plays the NEXT one in turn. Double-click to "
                               "clear them. Turning Trim off clears them too (plays the whole sample).");
        content.addAndMakeVisible(lblUseRegion[b]);
        lblUseRegion[b].setText("Trim", juce::dontSendNotification);
        lblUseRegion[b].setFont(juce::Font(11.5f)); lblUseRegion[b].setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        lblUseRegion[b].setMinimumHorizontalScale(0.8f);
        content.addAndMakeVisible(swUseRegion[b]);
        swUseRegion[b].onClick = [this, b] {
            if (ignoreKnobCallbacks) return;
            bool on = swUseRegion[b].getToggleState();
            auto& sl = proc.sequencer.channel(selectedChannel).slots[b];
            sl.smpUseRegion = on;
            if (! on) sl.smpRegN = 0;   // Trim off -> clean slate (clears the drawn regions)
            waveform[b].setSelectionEnabled(on);
            waveform[b].setRegions(sl.smpRegN, sl.smpRegLo, sl.smpRegHi);
            proc.sequencer.channel(selectedChannel).markDspDirty();
            updateSampleLengthLabel();
        };
        swUseRegion[b].setTooltip("Trim: draw up to 4 play regions on the waveform (each hit plays the next). Off = whole sample.");
        content.addAndMakeVisible(lblSampleLen[b]);
        lblSampleLen[b].setFont(juce::Font(11.0f)); lblSampleLen[b].setJustificationType(juce::Justification::centred);
        lblSampleLen[b].setColour(juce::Label::textColourId, juce::Colour(0xff8fb0d0));
        content.addAndMakeVisible(lblSampleReverse[b]);
        lblSampleReverse[b].setText("Reverse", juce::dontSendNotification);
        lblSampleReverse[b].setFont(juce::Font(11.0f)); lblSampleReverse[b].setJustificationType(juce::Justification::centred);
        lblSampleReverse[b].setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        content.addAndMakeVisible(swSampleReverse[b]);
        swSampleReverse[b].onClick = [this, b] { if (!ignoreKnobCallbacks) {
            const bool on = swSampleReverse[b].getToggleState();
            proc.sequencer.channel(selectedChannel).slots[b].smpReverse = on;
            waveform[b].setReversed(on); } };
        swSampleReverse[b].setTooltip("Play this slot's sample (or the selected part) backwards.");
        content.addAndMakeVisible(lblSmpPreserve[b]);
        lblSmpPreserve[b].setText("Keep pitch", juce::dontSendNotification);
        lblSmpPreserve[b].setFont(juce::Font(11.0f)); lblSmpPreserve[b].setJustificationType(juce::Justification::centred);
        lblSmpPreserve[b].setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        content.addAndMakeVisible(swSmpPreserve[b]);
        swSmpPreserve[b].onClick = [this, b] { if (!ignoreKnobCallbacks) {
            proc.sequencer.channel(selectedChannel).slots[b].smpPreservePitch = swSmpPreserve[b].getToggleState();
            proc.sequencer.channel(selectedChannel).markDspDirty(); } };
        swSmpPreserve[b].setTooltip("Keep pitch (ON by default): played NOTES never transpose this sample.\n\n"
            "- The keyboard and per-step/roll pitch leave it alone - recording a melody can't detune it.\n"
            "- The pitch envelope, vibrato and pitch LFO STILL work (applied separately).\n"
            "- Turn OFF to play it as a pitched sampler: notes below C4 play slower (and always "
            "finish the whole sample).");
    }
    // PITCH (semitones) = transpose the WHOLE channel - works for every engine (synth freq + sample
    // varispeed), applied via vPitchMul in the render. Same unit as the pitch envelope. Per-channel.
    setupKnob(knobSpeed, lblSpeed, "Tune", -24.0, 24.0, 0.0, 1.0,
              [](double v){ return (v > 0 ? "+" : "") + juce::String(juce::roundToInt(v)) + " st"; });
    // Channel pitch/Tune is REMOVED from the UI entirely (user call, third round: per-step pitch +
    // the pitch envelope cover melodies; synths tune on Freq). The knob object stays (hidden) so
    // ch.pitch from OLD projects still loads/applies and MIDI-learn maps don't break.
    knobSpeed.setSliderStyle(juce::Slider::LinearHorizontal);
    knobSpeed.setTextBoxStyle(juce::Slider::TextBoxRight, true, 44, 16);
    knobSpeed.setVisible(false); lblSpeed.setVisible(false);
    knobSpeed.onValueChange = [this] {
        if (ignoreKnobCallbacks) return;
        proc.sequencer.channel(selectedChannel).pitch = (float)knobSpeed.getValue();  // synths retune live
        updateSampleLengthLabel();
    };
    // Samples are pitch-SHIFTED (length kept) via SoundTouch - re-bake on drag end (not every tick).
    knobSpeed.onDragEnd = [this] {
        auto& c = proc.sequencer.channel(selectedChannel);
        for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
            if (c.slots[b].engine == DrumChannel::SrcSample) c.updateStretch(b);
        cacheWaveform(selectedChannel);
    };

    auto fmtShape = [](double v) {
        switch (juce::roundToInt(v)) { case 1: return juce::String("Triangle");
                                       case 2: return juce::String("Square");
                                       case 3: return juce::String("Saw");
                                       default: return juce::String("Sine"); } };
    setupKnob(knobLayOscShape, lblLayOscShape, "Shape", 0.0, 3.0, 0.0, 1.0, fmtShape);
    knobLayOscShape.setRange(0.0, 3.0, 1.0); // 4 discrete shapes
    setupKnob(knobLaySineFreq, lblLaySineFreq, "Freq",  20.0, 1000.0, 60.0, 1.0, fmtHz);
    knobLaySineFreq.setSkewFactorFromMidPoint(120.0);
    setupKnob(knobLaySinePEA,  lblLaySinePEA,  "P.Env",  -48.0, 48.0,  0.0,  1.0, fmtSemi);
    setupKnob(knobLaySinePET,  lblLaySinePET,  "P.Time", 0.001, 1.0,  0.04, 0.3, fmtMs);
    auto fmtNoiseType = [](double v) {
        switch (juce::roundToInt(v)) { case 1: return juce::String("Pink");  case 2: return juce::String("Brown");
                                       case 3: return juce::String("Grey");  case 4: return juce::String("Purple");
                                       default: return juce::String("White"); } };
    setupKnob(knobLayNoiseType, lblLayNoiseTypeK, "Type", 0.0, 4.0, 0.0, 1.0, fmtNoiseType);
    knobLayNoiseType.setRange(0.0, 4.0, 1.0); // 5 noise colours
    setupKnob(knobLayNoiseCtr, lblLayNoiseCtr, "Center", 100.0, 16000.0, 3000.0, 1.0, fmtHz);
    knobLayNoiseCtr.setSkewFactorFromMidPoint(1500.0);
    setupKnob(knobLayNoiseWid, lblLayNoiseWid, "Width", 0.0, 1.0, 0.0, 1.0, fmtPct);  // 0 = raw noise (no filter)
    setupKnob(knobLaySinePOff, lblLaySinePOff, "P.Offset", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobFmPitch,  lblFmPitch,  "Pitch",  -24.0, 24.0, 0.0, 1.0, fmtSemi);
    setupKnob(knobFmSub,    lblFmSub,    "Sub",    0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobFmSpread, lblFmSpread, "Ratio",  0.0,  1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobFmDepth,  lblFmDepth,  "Depth",  0.0,  1.0, 0.4, 1.0, fmtPct);
    setupKnob(knobFmPEnv,   lblFmPEnv,   "P.Env",  -48.0, 48.0, 0.0,  1.0, fmtSemi);
    setupKnob(knobFmPTime,  lblFmPTime,  "P.Time", 0.001, 1.0,  0.05, 0.3, fmtMs);
    setupKnob(knobFmPOff,   lblFmPOff,   "P.Offset",   0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobSmpPOff,  lblSmpPOff,  "P.Offset",   0.0, 1.0, 0.0, 1.0, fmtPct);
    // Analog Unison: stack detuned copies (fat supersaw / thick bass).
    auto fmtVoices = [](double v){ return juce::String(juce::roundToInt(v)) + "x"; };
    setupKnob(knobOscUnison, lblOscUnison, "Unison", 1.0, 7.0, 1.0, 1.0, fmtVoices);
    knobOscUnison.setRange(1.0, 7.0, 1.0); // 1..7 stacked voices
    setupKnob(knobOscDetune, lblOscDetune, "Detune", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobOscSustain, lblOscSustain, "Sustain", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobOscVib,     lblOscVib,     "Vibrato", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobFmSustain,  lblFmSustain,  "Sustain", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobPhysSus,    lblPhysSus,    "Sustain", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobPhysVib,    lblPhysVib,    "Vibrato", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobNoiseSus,   lblNoiseSus,   "Sustain", 0.0, 1.0, 0.0, 1.0, fmtPct);
    // Physical (Karplus-Strong) source.
    setupKnob(knobPhysFreq, lblPhysFreq, "Freq", 20.0, 2000.0, 110.0, 1.0, fmtHz);
    knobPhysFreq.setSkewFactorFromMidPoint(220.0);
    setupKnob(knobPhysTone, lblPhysTone, "Tone", 0.0, 1.0, 0.5, 1.0, fmtPct);
    auto fmtPhysModel = [](double v) {
        switch (juce::roundToInt(v)) { case 1: return juce::String("Steel"); case 2: return juce::String("Wood");
                                       case 3: return juce::String("Glass"); case 4: return juce::String("Metal");
                                       case 5: return juce::String("Skin");  default: return juce::String("Nylon"); } };
    setupKnob(knobPhysMat,  lblPhysMat,  "Material", 0.0, 5.0, 0.0, 1.0, fmtPhysModel);
    knobPhysMat.setRange(0.0, 5.0, 1.0); // 6 object models
    setupKnob(knobPhysPEnv, lblPhysPEnv, "P.Env",  -48.0, 48.0, 0.0,  1.0, fmtSemi);
    setupKnob(knobPhysPTime,lblPhysPTime,"P.Time", 0.001, 1.0,  0.05, 0.3, fmtMs);
    setupKnob(knobPhysPOff, lblPhysPOff, "P.Offset",   0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobPhysPos,  lblPhysPos,  "Position",   0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobFmFeedback, lblFmFeedback, "Feedback", 0.0, 1.0, 0.0, 1.0, fmtPct);

    knobLayOscShape.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).layerOscShape     = juce::roundToInt(knobLayOscShape.getValue()); };
    knobLaySineFreq.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).layerSineFreq     = (float)knobLaySineFreq.getValue(); };
    knobLaySinePEA.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).layerSinePEnvAmt  = (float)knobLaySinePEA.getValue(); };
    knobLaySinePET.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).layerSinePEnvTime = (float)knobLaySinePET.getValue(); };
    knobLaySinePOff.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).layerSinePOffset  = (float)knobLaySinePOff.getValue(); };
    knobLayNoiseType.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).noiseType       = juce::roundToInt(knobLayNoiseType.getValue()); };
    knobLayNoiseCtr.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).layerNoiseCenter = (float)knobLayNoiseCtr.getValue(); };
    knobLayNoiseWid.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).layerNoiseWidth  = (float)knobLayNoiseWid.getValue(); };
    knobFmPitch.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmPitch          = (float)knobFmPitch.getValue(); };
    knobFmSub.onValueChange       = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmSub            = (float)knobFmSub.getValue(); };
    knobFmSpread.onValueChange    = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmSpread         = (float)knobFmSpread.getValue(); };
    knobFmDepth.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmDepth          = (float)knobFmDepth.getValue(); };
    knobFmPEnv.onValueChange      = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmPitchEnvAmt    = (float)knobFmPEnv.getValue(); };
    knobFmPTime.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmPitchEnvTime   = (float)knobFmPTime.getValue(); };
    knobFmPOff.onValueChange      = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmPitchOffset    = (float)knobFmPOff.getValue(); };
    knobSmpPOff.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).pitchOffset      = (float)knobSmpPOff.getValue(); };
    knobOscUnison.onValueChange   = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).oscUnison        = juce::roundToInt(knobOscUnison.getValue()); };
    knobOscDetune.onValueChange   = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).oscDetune        = (float)knobOscDetune.getValue(); };
    knobOscSustain.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).oscSustain       = (float)knobOscSustain.getValue(); };
    knobOscVib.onValueChange      = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).oscVibrato       = (float)knobOscVib.getValue(); };
    knobFmSustain.onValueChange   = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmSustain        = (float)knobFmSustain.getValue(); };
    knobPhysSus.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physSustain      = (float)knobPhysSus.getValue(); };
    knobPhysVib.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physVibrato      = (float)knobPhysVib.getValue(); };
    knobNoiseSus.onValueChange    = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).noiseSustain     = (float)knobNoiseSus.getValue(); };
    knobPhysFreq.onValueChange    = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physFreq         = (float)knobPhysFreq.getValue(); };
    knobPhysTone.onValueChange    = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physTone         = (float)knobPhysTone.getValue(); };
    knobPhysMat.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physMaterial     = (float)knobPhysMat.getValue(); };
    knobPhysPEnv.onValueChange    = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physPitchEnvAmt  = (float)knobPhysPEnv.getValue(); };
    knobPhysPTime.onValueChange   = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physPitchEnvTime = (float)knobPhysPTime.getValue(); };
    knobPhysPOff.onValueChange    = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physPitchOffset  = (float)knobPhysPOff.getValue(); };
    knobPhysPos.onValueChange     = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).physPosition     = (float)knobPhysPos.getValue(); };
    knobFmFeedback.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).fmFeedback       = (float)knobFmFeedback.getValue(); };

    setupGroupHeader(hdrSamplerG, "SAMPLE");
    setupGroupHeader(hdrOscG,   "OSCILLATOR");
    setupGroupHeader(hdrNoiseG, "NOISE");
    setupGroupHeader(hdrFmG,    "FM");
    setupGroupHeader(hdrPhysG,  "KARPLUS-STRONG");

    // Reverse toggle for the Sample source (plays the region/sample backwards)

    // Blend character: Bloom / Drift / Spread / Punch / Glue (clean, non-distorting).
    content.addAndMakeVisible(lblBlendTitle);   // Slot 1 % (top of the blend fader, yellow)
    lblBlendTitle.setJustificationType(juce::Justification::centred);
    lblBlendTitle.setFont(juce::Font(10.0f, juce::Font::bold));
    lblBlendTitle.setColour(juce::Label::textColourId, juce::Colour(0xffffd24a));
    content.addAndMakeVisible(lblBlendBot);     // Slot 2 % (bottom of the blend fader, pink)
    lblBlendBot.setJustificationType(juce::Justification::centred);
    lblBlendBot.setFont(juce::Font(10.0f, juce::Font::bold));
    lblBlendBot.setColour(juce::Label::textColourId, juce::Colour(0xffff5fa6));
    // (Blend-character knobs Bloom/Drift/Spread/Punch/Glue removed - the DSP was dead.)

    // Master FX: Delay Sync/Ping = SINGLE-ROW lit buttons now [2026-07-15 12:10] (user: the old
    // label + tiny switch pairs cost four rows and shrank the text; a lit button carries its own
    // word). The old labels stay as members but are never placed.
    auto setupLitBtn = [this](juce::TextButton& b, const char* txt) {
        b.setButtonText(txt); b.setClickingTogglesState(true);
        b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff35b56a));
        b.setLookAndFeel(&tinyBtnLNF);
        content.addAndMakeVisible(b);
    };
    setupLitBtn(swDelaySync, "Sync");
    swDelaySync.onClick = [this] { if (!ignoreKnobCallbacks) { const bool on = swDelaySync.getToggleState();
        for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.delaySyncB : p.master.delaySync) = on; knobDelayTime.updateText();
        refreshMasterSyncFaders(); refreshDetailPanel(); } };   // Time fader <-> echoes-per-bar fader swap
    setupLitBtn(swReverbSync, "Sync");   // reverb row (onClick + tooltip set at the fader block)
    setupLitBtn(swDelayPingPong, "Ping");
    swDelayPingPong.setTooltip("Ping-Pong: each echo bounces to the opposite side (echo 1 left, echo 2 right...). "
                               "Great for width on hats/percussion. Needs the channel's Delay send up.");
    swDelayPingPong.onClick = [this] { if (!ignoreKnobCallbacks) { const bool on = swDelayPingPong.getToggleState();
        for (auto& p : proc.sequencer.patterns) (masterBusB ? p.master.delayPingPongB : p.master.delayPingPong) = on; } };

    content.addAndMakeVisible(monoToggle);   // two-row STEREO/MONO button (the old label + tiny switch was unreadable)
    monoToggle.onChange = [this](bool on) { if (ignoreKnobCallbacks) return;   // Mono is preset-wide too -> write ALL patterns
        for (auto& p : proc.sequencer.patterns) p.master.mono = on; };

    content.addAndMakeVisible(btnSaveMix);
    btnSaveMix.setLookAndFeel(&tinyBtnLNF);   // smaller font so the long text fits + reads
    btnSaveMix.onClick = [this] { saveSoundMix(); };
    btnSaveMix.setTooltip("Save this channel's current sound as a new, reusable entry in the Sound Bank.");

    //-- Beginner-friendly tooltips on hover (a TooltipWindow shows them).
    tooltipWindow.setLookAndFeel(&tipLNF);   // bigger font + wider wrap + paragraph-friendly
    btnDawSync.setTooltip("When sync is enabled, BPM and time signature are taken from the project. "
                          "And play/stop functions will also be controlled by the DAW.");
    sliderBpm.setTooltip("Tempo in beats per minute. Sets how fast the pattern plays (only editable when DAW Sync is off).");
    sliderSwing.setTooltip("Swing (per pattern) delays every other step, MPC-style: 50% = straight (off), ~66% = "
                           "triplet shuffle, 75% = maximum drag. Roll sub-hits and the MIDI export follow the same groove.");
    barSigX.setTooltip("Top number of the time signature: how many beats are in one bar. Click to type a value.");
    barSigY.setTooltip("Bottom number of the time signature: which note value counts as one beat. Click to type a value.");
    lblBarResult.setTooltip("How many seconds one full bar lasts, from the BPM and time signature. One pattern = one bar.");
    btnPlay.setTooltip("Start playback (used when DAW Sync is off, so the plugin runs on its own).");
    btnStop.setTooltip("Stop playback (used when DAW Sync is off).");
    comboPreset.setTooltip("Save or load a whole-kit preset: all channels, patterns and settings at once.");
    dragMidi.setTooltip("Drag onto a DAW track to export the SELECTED channel as a MIDI clip.\n\n"
                        "- Notes carry their VELOCITY and length; rolls, swing and merged chains are kept.\n"
                        "- Piano-roll channels export exactly as drawn (C4-based); step channels use each "
                        "pitched slot's Base Freq as the 0-point; Chord/Scale slots export their full voicing.");
    patModeBtn.setTooltip("What happens after this pattern finishes: loop forever, stop after N loops, or jump to another pattern.");

    // (freqDisplay's tooltip lives in FrequencyDisplay::getTooltip() - the override means a static
    //  setTooltip here is NEVER shown. A dead decoy text sat here until [2026-07-15 17:00].)
    soundPad.setTooltip("Drag the yellow dot to blend the enabled sound sources. Closer to a corner = more of that source.");
    comboSampleSel.setTooltip("Pick a sample for this channel from your samples folder (or load a new one).");
    knobSpeed.setTooltip("TUNE: transpose the WHOLE channel in semitones.\n\n"
                         "- Tune sets the root/key once; per-step Pitch plays the melody RELATIVE to it.\n"
                         "- On samples Tune is a length-keeping pitch-shift (per-step pitch is varispeed).\n"
                         "- Length without pitch change = Stretch.");
    knobLayNoiseType.setTooltip("Noise colour: White (bright/hissy), Pink (softer), Brown (deep/rumbly), Grey (balanced), Purple (very bright/airy).");
    swDelaySync.setTooltip("Lock the delay time to the tempo (note values like 1/8, 1/4) instead of free milliseconds.");
    monoToggle.setTooltip("Switch the final output between STEREO and MONO (both speakers identical). The lit row = the current mode; click to toggle.");
    for (int i = 0; i < 4; ++i)
        srcSwitch[i].setTooltip("Turn this sound source on or off. Active sources are blended by the pad.");

    knobPitch.setTooltip("Crush: bit-crushes the Sample for lo-fi, gritty, retro-sampler character. 0% = clean. (Tune per step now in the grid's Pitch mode.)");
    knobPEnvAmt.setTooltip("A quick pitch bend at the very start of the sound. Great for punchy kicks (drops down to the set pitch).");
    knobPEnvTime.setTooltip("How long that starting pitch bend takes to settle to the normal pitch.");
    knobVolume.setTooltip("Loudness of this channel.");
    knobPan.setTooltip("Position in the stereo field: left, center, or right.");
    knobCutoff.setTooltip("Filter cutoff frequency - where the filter starts cutting the sound. In Formant mode it morphs the vowel from A (low) to U (high); use the filter Env to make it 'talk'.");
    knobReso.setTooltip("Filter resonance - emphasises the sound right at the cutoff for a sharper, more vocal tone.");
    knobEnvAmt.setTooltip("Lets the envelope sweep the filter cutoff over time for a moving, dynamic tone.");
    knobDrive.setTooltip("How hard the sound is pushed into distortion. More drive = grittier, louder, more aggressive.");
    knobReverbRoom.setTooltip("Reverb size - small room to large hall.");
    knobReverbDecay.setTooltip("How long the reverb tail lasts before fading away.");
    knobReverbWet.setTooltip("Overall reverb amount (how loud the reverb is in the mix).");
    knobReverbPre.setTooltip("Pre-delay: a short gap (0-120 ms) before the reverb tail starts, so the dry hit is heard "
                             "first and the tail blooms after. Adds clarity + size to drums. 0 = no gap.");
    knobReverbWidth.setTooltip("Stereo width of the reverb tail. 1 = full wide, lower = narrower (toward mono).");
    knobDelayTime.setTooltip("Time between echoes. With Sync on it snaps to note values; otherwise it's free milliseconds.");
    knobDelayFB.setTooltip("Delay feedback - how many times the echo repeats. Higher = more repeats.");
    knobDelayWet.setTooltip("Overall delay amount (how loud the echoes are in the mix) - like the reverb Wet.");
    knobMasterVol.setTooltip("MASTER output volume (the final fader, per pattern).");
    knobMasterLimit.setTooltip("MASTER output limiter - the read-out is the output CEILING in dB.\n\n"
                               "- Peaks are held just below the ceiling, so boosts can't clip your DAW.\n"
                               "- Light (-0.1 to -1 dB) = transparently catches strays; low (toward -12 dB) = "
                               "squashes hard + pushes the level up.\n"
                               "- 'Off' = no limiting.");
    knobMasterGlue.setTooltip("GLUE: a master compressor - squeezes loud and quiet hits closer together so the kit "
                              "plays as one punchy groove.\n\n"
                              "- Adds a subtle 'pump'; no harmonics/dirt (that's SAT).\n"
                              "- GLUE = tightness + punch; SAT = warmth + colour - they stack.\n"
                              "- 0% = OFF. Sits before the Limiter; keeps the stereo image put.");
    knobMasterTilt.setTooltip("TILT: one knob for the tone of the WHOLE mix, tilting the balance around ~700 Hz.\n\n"
                              "- LEFT (Dk) = darker/warmer - tames harsh hats, fattens bass.\n"
                              "- RIGHT (Br) = brighter/crisper - more attack + air.\n"
                              "- Centre = Flat = off. Range about +/-6 dB.");
    knobMasterSat.setTooltip("SAT: master saturation - a tube-style warmer that ADDS harmonics.\n\n"
                             "- Low = subtle analog warmth; high = obvious grit/dirt.\n"
                             "- Keeps bass audible on small speakers.\n"
                             "- SAT = warmth + colour; GLUE = tightness + pump (no harmonics) - they stack.\n"
                             "- 0% = OFF (clean). Sits before Glue + the Limiter.");
    knobLayOscShape.setTooltip("Oscillator waveform: Sine (pure), Triangle (soft), Square (hollow), Saw (bright/buzzy).");
    knobLaySineFreq.setTooltip("Oscillator pitch in Hz. Low values give sub-bass for kicks; higher for tones.");
    knobLaySinePEA.setTooltip("Oscillator pitch bend amount at the start of the note.");
    knobLaySinePET.setTooltip("How long the oscillator's starting pitch bend takes to settle.");
    knobLayNoiseCtr.setTooltip("Centre frequency of the noise tone-filter. Only has an effect when Width is above 0.");
    knobLayNoiseWid.setTooltip("Tone-filter amount. At 0 the noise is raw/unfiltered (plain white, brown, etc.). "
                               "Turn it up to focus the noise into a band around the Centre frequency (narrower = more pitched).");
    knobFmPitch.setTooltip("Pitch: transposes the FM tone up or down in semitones (sets the carrier base pitch).");
    knobFmSub.setTooltip("Sub: mixes a sub-octave sine under the FM tone for extra body and weight - great for FM kicks and basses. 0% = none.");
    knobFmSpread.setTooltip("How far apart the FM tones are detuned - more spread = more clangy/metallic.");
    knobFmDepth.setTooltip("FM modulation depth (index): how strongly the modulator bends the carrier. More = brighter, richer, more metallic.");
    knobFmSpread.setTooltip("FM ratio: how the modulator is tuned vs the carrier. Higher = more inharmonic / clangy / bell-like.");
    // New per-source pitch knobs
    const juce::String pOffTip = "Pitch Offset: delays where the pitch envelope kicks in. 0% = right at the start of the sound; higher holds the starting pitch longer before it sweeps.";
    knobSmpPOff.setTooltip(pOffTip);
    knobLaySinePOff.setTooltip(pOffTip);
    knobFmPOff.setTooltip(pOffTip);
    knobLaySinePOff.setTooltip("Pitch Offset (Analog): delays where the oscillator's pitch bend begins. 0% = at the start of the sound.");
    knobFmPEnv.setTooltip("FM pitch bend amount at the start of the note (in semitones).");
    knobFmPTime.setTooltip("How long the FM starting pitch bend takes to settle.");
    knobFmPOff.setTooltip("Pitch Offset (FM): delays where the FM pitch bend begins. 0% = at the start of the sound.");
    knobSmpPOff.setTooltip("Pitch Offset (Sample): delays where the sample's pitch bend begins. 0% = at the start of the sound.");
    knobOscUnison.setTooltip("Unison: stack this many detuned copies of the Analog oscillator. 1 = off. Higher = fatter (supersaw / thick analog bass).");
    knobOscDetune.setTooltip("Detune: how far apart the stacked unison copies are spread (up to ~50 cents). More = wider, more shimmering / chorused.");
    const juce::String susTip = ": the level the decay settles to instead of fading right out, holding a 'floor' so the sound has body / drone. 0% = normal one-shot decay; a short auto fade-out stops it clicking.";
    knobOscSustain.setTooltip("Sustain (Analog)" + susTip);
    knobOscVib.setTooltip("Vibrato: a gentle ~5.5 Hz pitch wobble on the Analog source - great for whistles, leads and flutes. 0% = off.");
    knobFmSustain.setTooltip("Sustain (FM)" + susTip);
    knobPhysSus.setTooltip("Sustain (Physical)" + susTip);
    knobPhysVib.setTooltip("Vibrato: a gentle ~5.5 Hz pitch wobble on the Physical string. 0% = off.");
    knobNoiseSus.setTooltip("Sustain (Noise)" + susTip);
    knobPhysFreq.setTooltip("Physical pitch: the fundamental of the plucked-string / mallet model.");
    knobPhysTone.setTooltip("Tone: brightness of the physical model. Low = dark/muted, high = bright/ringing.");
    knobPhysMat.setTooltip("Material: what the object is made of - Nylon (warm soft string), Steel (bright ringing string), Wood (short dry woodblock), Glass (long shimmering bell), Metal (clangy gong), Skin (boomy drumhead with a tom-like pitch drop).");
    knobPhysPEnv.setTooltip("Physical pitch bend amount at the start of the note (in semitones).");
    knobPhysPTime.setTooltip("How long the physical model's starting pitch bend takes to settle.");
    knobPhysPOff.setTooltip("Pitch Offset (Physical): delays where the physical pitch bend begins. 0% = at the start of the sound.");
    knobPhysPos.setTooltip("Position: where the object is struck/plucked. 0% = full/centred; higher combs out harmonics for a more hollow, nasal, bridge-like tone.");
    knobFmFeedback.setTooltip("Feedback: the FM operator modulates itself. Low adds bite/edge; high morphs the sine toward a saw, then into noisy, gritty textures.");
    freqDisplay.setTooltip("Channel EQ: drag a band to move it, mouse-wheel a bell for width (Q), double-click to enable/disable. H = high-pass, L = low-pass (24 dB/oct), 1/2/3 = bells.");

    comboFilterType.setTooltip("Filter: Off, or Formant (vowel/vocal - Cutoff sweeps A-E-I-O-U, Reso = how vocal; works best on Analog saw or FM). Low/High/Band/Notch are now done on the EQ display above.");
    comboDriveType.setTooltip("Distortion flavour - each shapes the grit differently (soft, hard, tube, fold, fuzz, bitcrush).");
}

// Legacy per-source on/off toggles are hidden in the 3-slot UI; the dropdowns
// drive everything now. Kept as a no-op so old call sites stay valid.
void DrumSequencerEditor::onSoundToggle() {}

// boxEngine[] reflects each slot's engine (the slots are the source of truth).
// One-line description of the currently-selected engine, shown as the dropdown's tooltip
// (the menu is where a new user first meets these names).
static juce::String engineDescription(int eng)
{
    switch (eng)
    {
        case DrumChannel::SrcSample: return "SAMPLE - plays an audio file (drag one onto the box, or pick from the menu). "
                                            "Trim regions, reverse, crush, stretch + an optional amp envelope.";
        case DrumChannel::SrcNoise:  return "NOISE - coloured noise (White/Pink/Brown/Grey/Purple) through a band-pass. "
                                            "Hats, claps, texture; Reso makes it pitched/whistling.";
        case DrumChannel::SrcOsc:    return "OSCILLATOR - a band-limited oscillator (analog waves + additive shapes + a "
                                            "drawable Custom wave + Warp wavefold), with an "
                                            "FM section on top (Amount 0 = pure analog). Kicks, basses, bells, leads.";
        case DrumChannel::SrcFM:     return "FM (legacy) - an old project's 2-operator FM slot. New sounds: use "
                                            "Oscillator (its FM section is the same maths).";
        case DrumChannel::SrcPhys:   return "KARPLUS-STRONG - a plucked/struck string synthesis: materials, stiffness, "
                                            "strike position. Drag the string visual; Ring = its natural decay.";
        case DrumChannel::SrcSynth:  return "SYNTH (legacy) - an old project's unified-synth slot (osc + noise + resonator).";
        case DrumChannel::SrcWave:   return "WAVETABLE (legacy) - an old project's wavetable slot. New sounds: the "
                                            "Oscillator waves + Warp cover this ground.";
        case DrumChannel::SrcModal:  return "MODAL - a struck resonant body (marimba, bell, glass, membrane, plate...): "
                                            "a bank of ringing modes. Drag the hammer visual; Ring = the mode decay.";
        case DrumChannel::SrcGrain:  return "GRANULAR - clouds of tiny windowed grains. Source = this slot's SAMPLE when "
                                            "one is loaded, else a rendered journey of the slot's wave (Custom's 4 frames "
                                            "= the Position scan). Pads, textures, stutters, frozen moments.";
        default:                     return "Pick this slot's sound engine (or a sample). Two slots blend into one sound.";
    }
}

void DrumSequencerEditor::syncBoxesFromSrcOn()
{
    auto& ch = proc.sequencer.channel(selectedChannel);
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    {
        boxEngine[b] = ch.slots[b].engine;
        slotCombo[b].setTooltip(engineDescription(boxEngine[b]));   // describe the current engine on hover
        if (boxEngine[b] == DrumChannel::SrcSample)   // Sample has no root item (it's a submenu) - show the name
        {
            slotCombo[b].setSelectedId(0, juce::dontSendNotification);
            juce::String nm = ch.slotSample[b].usingUser ? ch.slotSample[b].file.getFileNameWithoutExtension() : "Sample";
            slotCombo[b].setTextWhenNothingSelected(nm);
            slotCombo[b].repaint();
        }
        else if (boxEngine[b] == DrumChannel::SrcFM)  // legacy FM (merged into Analog/FM) - no menu item now
        {
            slotCombo[b].setSelectedId(0, juce::dontSendNotification);
            slotCombo[b].setTextWhenNothingSelected("FM (legacy)");
            slotCombo[b].repaint();
        }
        else if (boxEngine[b] == DrumChannel::SrcWave) // wavetable retired from the menu (Oscillator covers it)
        {
            slotCombo[b].setSelectedId(0, juce::dontSendNotification);
            slotCombo[b].setTextWhenNothingSelected("Wavetable (legacy)");
            slotCombo[b].repaint();
        }
        else
            slotCombo[b].setSelectedId(boxEngine[b] < 0 ? 1 : boxEngine[b] + 2, juce::dontSendNotification);
    }
}

// (Re)build each slot dropdown: None, a "Sample" SUBMENU of the samples folder, then
// the synth engines. Called whenever the sample list changes.
void DrumSequencerEditor::rebuildSlotMenus()
{
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    {
        slotCombo[b].clear(juce::dontSendNotification);
        auto* root = slotCombo[b].getRootMenu();
        root->clear();
        root->addItem(1, "None");
        juce::PopupMenu sampleSub;
        addFolderToMenu(sampleSub, getSamplesFolder());
        sampleSub.addSeparator();
        sampleSub.addItem(ID_REFRESH_SAMPLES, "Refresh samples folder");   // rescan so newly-added files show up
        sampleSub.addItem(ID_SHOW_SAMPLES,    "Show Folder");              // open the Samples folder to drop files in
        root->addSubMenu("Sample", sampleSub);
        root->addItem(3, "Noise"); root->addItem(4, "Oscillator");   // analog waves + FM + drawable additive Custom
        root->addItem(6, "Karplus-Strong");   // (was "Physical") - the honest algorithm name; "FM"/"Synth"/"Wavetable" retired (kept parseable for old
                                        // projects); the Oscillator now covers wavetable-style shaping (more shapes + Warp).
        root->addItem(9, "Modal");      // id = engine+2 -> SrcModal (7); maps automatically (struck resonant body)
        root->addItem(10, "Granular");  // id = engine+2 -> SrcGrain (8): grains of a wave journey or the slot's sample
    }
    syncBoxesFromSrcOn();   // restore the current channel's displayed text
}

// Mirror the channel's slots onto the blend pad: one corner per occupied slot,
// each labelled with its engine (numbered when an engine repeats, e.g. two
// Analog slots -> "Analog 1" / "Analog 2"). recenter=true re-balances the blend
// (used when a slot is added/removed); recenter=false restores the saved dot
// (used when (re)selecting a channel).
void DrumSequencerEditor::syncPadFromSlots(bool recenter)
{
    auto& ch = proc.sequencer.channel(selectedChannel);
    static const char* eng[DrumChannel::NUM_SOURCES + 4] = { "Sample", "Noise", "Oscillator", "FM", "Karplus-Strong", "Synth", "Wavetable", "Modal", "Granular" };
    bool a[DrumChannel::NUM_SLOTS];
    int total[DrumChannel::NUM_SOURCES + 4] = {}, seen[DrumChannel::NUM_SOURCES + 4] = {};
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    { a[b] = (ch.slots[b].engine >= 0); if (a[b]) total[ch.slots[b].engine]++; }

    juce::StringArray nm;
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    {
        const int e = ch.slots[b].engine;
        if (e < 0) { nm.add (juce::String()); continue; }
        juce::String s (eng[e]);
        if (total[e] > 1) s << " " << (++seen[e]);   // disambiguate duplicate engines
        nm.add (s);
    }
    soundPad.names   = nm;
    soundPad.layoutB = ch.padLayoutB;

    {   // don't let setActiveMask/setDot re-enter onChange while we copy back below
        const juce::ScopedValueSetter<bool> guard (ignoreKnobCallbacks, true);
        if (recenter)
            soundPad.setActiveMask (a);                       // recenter dot + recompute (balanced)
        else
        {
            for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) soundPad.active[b] = a[b];
            soundPad.setDot (ch.padX, ch.padY);               // recompute weights from the saved dot
        }
    }
    // READ-ONLY refresh: the dormant pad used to write its recomputed weights + dot BACK into
    // the channel here - every refresh (TEST click, channel select...) clobbered the fader-set
    // weights and drifted padX through the pad's geometry = "the blend fader doesn't sit still".
    // The blend fader's onValueChange is the ONE writer now; the pad only displays.
    btnPadLayout.setEnabled (false);   // A/B only matters at 4+ corners; max 3 slots now
    soundPad.repaint();
    { const juce::ScopedValueSetter<bool> g(ignoreKnobCallbacks, true);   // reflect the blend + lock on the fader
      const bool o0 = ch.slots[0].engine >= 0, o1 = ch.slots[1].engine >= 0;
      blendFader.setEnabled(o0 && o1);
      blendFader.setValue((o0 && o1) ? 1.0f - ch.padX : (o1 ? 0.0f : 1.0f), juce::dontSendNotification);
      const int s2pct = (o0 && o1) ? juce::roundToInt(ch.padX * 100.0f) : (o1 ? 100 : 0);
      lblBlendTitle.setText(juce::String(100 - s2pct) + "%", juce::dontSendNotification);
      lblBlendBot.setText(juce::String(s2pct) + "%", juce::dontSendNotification); }
}

// Pointers to a slot's pitch-envelope fields - which physical fields differ by engine
// (so a factory sound's pitch env shows up correctly whatever engine the slot uses).
// Returns nulls for engines with no pitch env (Noise).
struct PEnvRef { float *amt = nullptr, *time = nullptr, *off = nullptr; };
static PEnvRef slotPEnv(DrumChannel::Slot& s)
{
    switch (s.engine) {
        case DrumChannel::SrcOsc:
        case DrumChannel::SrcSynth:  return { &s.oscPEnvAmt,  &s.oscPEnvTime,  &s.oscPOffset  };
        case DrumChannel::SrcFM:     return { &s.fmPEnvAmt,   &s.fmPEnvTime,   &s.fmPOffset   };
        case DrumChannel::SrcPhys:   return { &s.physPEnvAmt, &s.physPEnvTime, &s.physPOffset };
        case DrumChannel::SrcSample: return { &s.smpPEnvAmt,  &s.smpPEnvTime,  &s.smpPOffset  };
        default:                     return {};   // Noise / none
    }
}

// The shared selected slot (0/1/2) that all shape editors act on.
int DrumSequencerEditor::envTargetSlot() const { return juce::jlimit(0, 2, slotSelAmp.sel); }

void DrumSequencerEditor::setShapeSlot(int s)
{
    s = juce::jlimit(0, 2, s);
    slotSelAmp.sel = slotSelPitch.sel = slotSelVoice.sel = slotSelFx.sel = slotSelMod.sel = s;
    proc.sequencer.channel(selectedChannel).envEditMode = s + 1;   // remembered per channel
    slotSelAmp.repaint(); slotSelPitch.repaint(); slotSelVoice.repaint(); slotSelFx.repaint(); slotSelMod.repaint();
    content.repaint();   // redraw the slot-box highlight (the selected slot is emphasised)
    loadEnvIntoEditor();
    loadPitchAndVoice();
    // The FX box (Drive / Reverb / Delay / LFO) + Sample Slices/Stretch ALSO follow the selected
    // slot - reload them too, or switching slots left them showing the OTHER slot's settings (the
    // LFO is per-slot in the DSP; only the display wasn't refreshing).
    auto& ch = proc.sequencer.channel(selectedChannel);
    auto& sl = ch.slots[envTargetSlot()];
    knobSlices.setValue (sl.smpSlices,   juce::dontSendNotification);
    knobStretch.setValue(sl.smpStretch,  juce::dontSendNotification);
    { const bool smp = (sl.engine == DrumChannel::SrcSample);
      knobSlices.setEnabled(smp);  knobSlices.setAlpha(smp ? 1.0f : 0.4f);  lblSlices.setAlpha(smp ? 1.0f : 0.4f);
      knobStretch.setEnabled(smp); knobStretch.setAlpha(smp ? 1.0f : 0.4f); lblStretch.setAlpha(smp ? 1.0f : 0.4f); }
    knobDrive.setValue (sl.fxDrive,       juce::dontSendNotification);
    comboDriveType.setSelectedId(sl.fxDriveType + 1, juce::dontSendNotification);
    updateFxFaders(sl);
    bool lfoRt[4]; lfoRoutedMask(sl, lfoRt);
        lfoDisplay.setValues(sl.lfoRate, sl.lfoAmt, sl.lfoSync, sl.lfoShape, sl.lfoFree, sl.lfoLegato, sl.lfoCurve, lfoRt, ((sl.filterType >= DrumChannel::LowPass && sl.filterType <= DrumChannel::Notch) || sl.filterType == DrumChannel::Bell || (sl.filterType2 >= DrumChannel::LowPass && sl.filterType2 <= DrumChannel::Notch) || sl.filterType2 == DrumChannel::Bell), (sl.oscShape >= DrumChannel::WvCustom || sl.engine == DrumChannel::SrcGrain),
                         envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8));
    // The 12-fader matrix + the Mod Env follow the selected slot too (yellow slot 1 / pink slot 2).
    modFaders.slotIdx = envTargetSlot();
    modFaders.accent = envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8);
    modFaders.setValues(sl);
    lfoDisplay.setModEnv(sl.modEnvA, sl.modEnvH, sl.modEnvD, sl.modEnvS, sl.modEnvR);
    // The EQ target follows the selected slot too (user: picking a slot shouldn't leave EQ on
    // "All"). Picking "All" on the EQ selector afterward still works - it just isn't the default.
    eqEditTarget = s + 1;
    refreshEqTarget();
    refreshKeysPanel();   // sus/rel knobs + hint follow the selected slot too
}

//==============================================================================
// KEYS view + recording (see KeysPanel in the header for the widget layout)
//==============================================================================
void DrumSequencerEditor::applyKeysView()
{
    btnKeysView.setButtonText(keysView ? "SOUND EDITOR" : "KEYS");
    // [2026-07-15 19:45] no highlight in either state (user: the yellow "SOUND EDITOR" while the
    // keys were open meant nothing - the button is a plain view switch, like its neighbours).
    btnKeysView.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnKeysView.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    // Opening KEYS no longer touches the channel's step/roll mode (user: "clicking Keys alone
    // shouldn't switch to Piano Roll" - the old force flipped a STEP channel onto its empty
    // roll, silencing its pattern). RECORDING still forces Piano Roll (keysStartRecord + the
    // processor's chain branch) - that's where the roll is actually needed.
    if (! keysView && (proc.keysRecording.load() || keysCountdownTicks > 0))
        keysStopRecord(true);   // leaving the panel ends the take
    refreshKeysPanel();
    layoutContent();
    content.repaint();
}

void DrumSequencerEditor::keysStartRecord()
{
    if (takesForPatChan(currentPattern(), selectedChannel) >= 20
        || (int) proc.keysTakes.size() >= DrumSequencerProcessor::KEYS_TAKES_MAX)
    { refreshKeysPanel(); return; }   // this pattern+channel (or the whole preset) is full
    proc.keysEvtCount.store(0);
    keysEvtCursor = 0; keysPendingEvts.clear();
    proc.keysArmedPattern.store(currentPattern());
    keysLoadedTakeIdx = -1;   // a fresh recording is not an edit of a loaded take
    keysRecStartTakeCount = (int) proc.keysTakes.size();   // to auto-load the last NEW take when we stop
    proc.keysRecMode.store(keysPanel.comboRecMode.getSelectedId() - 1);
    proc.keysLoopSeen.store(-1); proc.keysLastStampStep.store(-1); proc.keysLastPlayPat.store(-1);
    keysRecWasPlaying = false;
    // Recording auto-enables FOLLOW so the view tracks the playing pattern (essential in chain mode:
    // you record into whatever pattern is playing, so you want to see it).
    proc.followPlayback = true; refreshFollowButton();
    if (proc.sequencer.isCurrentlyPlaying) selectPattern(proc.sequencer.playPattern);
    // A recording ALWAYS goes into DRAW mode (user rule): if a step count was set, switch to draw.
    // Then start from a CLEAN slate - the draw lane cleared, and the step data too (both modes).
    proc.keysDrawLastCol.store(-1);
    {
        // Clean slate across EVERY bar of a merged group (group recording is a continuous overdub:
        // wraps never clear, so the fresh start happens HERE, once).
        auto& sq = proc.sequencer;
        const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
        const int recPartner = sq.channel(selectedChannel).mergeWith;   // MERGE&SPLIT: both rolls start clean
        for (int b = head; b <= end; ++b)
        {
            auto& pch = sq.patterns[b].channels[selectedChannel];
            pch.drawMode = true;
            pch.clearDrawNotes();
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i)
            { pch.steps[i] = false; pch.stepPitch[i] = 0.0f; pch.stepMerge[i] = false; pch.stepNoteLen[i] = 0.0f; }
            if (recPartner >= 0)
            {
                auto& qch = sq.patterns[b].channels[recPartner];
                qch.drawMode = true;
                qch.clearDrawNotes();
            }
        }
        strips[selectedChannel].comboSteps.setSelectedId(StepGridComponent::DRAW_ITEM_ID, juce::dontSendNotification);
        refreshDrawModeButtons();
        stepGrid.update(proc.sequencer, proc.anySolo);
    }
    const int m = proc.keysRecMode.load();
    if (m == 1 || m == 3) keysCountdownTicks = 180;        // 3 s count-in @ the 60 Hz UI timer
    else                  proc.keysRecording.store(true);  // live now; 1st key starts the transport
    refreshKeysPanel();
}

// Drain the audio thread's event log into TAKES. A 0xFF marker = loop boundary: the events since
// the previous marker become one take; the tail (no marker yet) stays pending until stop.
void DrumSequencerEditor::parseKeysEvents()
{
    const int cnt = juce::jmin(proc.keysEvtCount.load(), DrumSequencerProcessor::KEYS_EVT_CAP);
    while (keysEvtCursor < cnt)
    {
        const auto& e = proc.keysEvts[keysEvtCursor++];
        if (e.pattern != 0xFF) { keysPendingEvts.push_back(e); continue; }
        if (! keysPendingEvts.empty())
        {
            const int tp = (int) keysPendingEvts[0].pattern;   // this take lives in one pattern
            if (takesForPatChan(tp, selectedChannel) < 20 && (int) proc.keysTakes.size() < DrumSequencerProcessor::KEYS_TAKES_MAX)
            {
                DrumSequencerProcessor::KeysTake t;
                t.channel = selectedChannel;
                t.name    = juce::Time::getCurrentTime().toString(true, true, true, true)
                            + "  #" + juce::String((int) proc.keysTakes.size() + 1);
                t.evts    = std::move(keysPendingEvts);
                proc.keysTakes.push_back(std::move(t));
            }
        }
        keysPendingEvts.clear();
        if ((takesForPatChan(currentPattern(), selectedChannel) >= 20
             || (int) proc.keysTakes.size() >= DrumSequencerProcessor::KEYS_TAKES_MAX) && proc.keysRecording.load())
            keysStopRecord(true);   // this pattern+channel (or the whole preset) is full -> stop until a delete
    }
}

void DrumSequencerEditor::keysStopRecord(bool finalize)
{
    proc.keysDrawLastCol.store(-1);
    const bool wasPlaying = proc.sequencer.isCurrentlyPlaying;
    proc.keysRecording.store(false);
    keysCountdownTicks = 0; keysGoTicks = 0; keysPanel.countdown = 0;
    if (countdownOverlay.label.isNotEmpty()) { countdownOverlay.label.clear(); countdownOverlay.repaint(); }
    parseKeysEvents();
    drainDrawTake();   // grab any loop-boundary draw take waiting in the handshake
    auto& drawCh = proc.sequencer.channel(selectedChannel);
    if (finalize && drawCh.drawMode)
    {
        // save the final (partial) pass as a take too - for a MERGED GROUP the take holds every
        // bar's notes in CONCAT columns (drawPat = the head; keysLoadTake splits them back).
        auto& sq = proc.sequencer;
        const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
        bool any = false;
        for (int b = head; b <= end; ++b) any |= sq.patterns[b].channels[selectedChannel].drawNoteCount > 0;
        if (any && takesForPatChan(head, selectedChannel) < 20
                && (int) proc.keysTakes.size() < DrumSequencerProcessor::KEYS_TAKES_MAX)
        {
            DrumSequencerProcessor::KeysTake t;
            t.isDraw = true; t.channel = selectedChannel; t.drawPat = head;
            for (int b = head; b <= end; ++b)
            {
                const auto& cb = sq.patterns[b].channels[selectedChannel];
                for (int i = 0; i < cb.drawNoteCount && (int) t.drawNotes.size() < DrumSequencerProcessor::DRAW_TAKE_MAX; ++i)
                {
                    auto nt = cb.drawNotes[i];
                    nt.start = (int16_t) (nt.start + (b - head) * DrumChannel::DRAW_RES);
                    t.drawNotes.push_back(nt);
                }
            }
            t.name = juce::Time::getCurrentTime().toString(true, true, true, true) + "  #" + juce::String((int) proc.keysTakes.size() + 1);
            proc.keysTakes.push_back(std::move(t));
        }
    }
    else if (finalize && ! keysPendingEvts.empty()
             && takesForPatChan((int) keysPendingEvts[0].pattern, selectedChannel) < 20
             && (int) proc.keysTakes.size() < DrumSequencerProcessor::KEYS_TAKES_MAX)
    {
        DrumSequencerProcessor::KeysTake t;
        t.channel = selectedChannel;
        t.name    = juce::Time::getCurrentTime().toString(true, true, true, true)
                    + "  #" + juce::String((int) proc.keysTakes.size() + 1);
        t.evts    = std::move(keysPendingEvts);
        proc.keysTakes.push_back(std::move(t));
    }
    keysPendingEvts.clear();
    proc.keysEvtCount.store(0); keysEvtCursor = 0;
    if (finalize && wasPlaying && ! proc.sequencer.dawSync) proc.standaloneStop();   // stopping REC stops the sequencer too
    // Load the LAST take recorded THIS session onto the channel, so stopping REC leaves the
    // recording visible/playable (not the cleaned live pass, which read as empty).
    if (finalize && (int) proc.keysTakes.size() > keysRecStartTakeCount)
        keysLoadTake((int) proc.keysTakes.size() - 1);
    refreshKeysPanel();
    stepGrid.update(proc.sequencer, proc.anySolo);   // show the recording immediately
}

// Drain a loop-boundary draw take from the audio-thread handshake into the takes list.
void DrumSequencerEditor::drainDrawTake()
{
    if (! proc.keysDrawTakeReady.load(std::memory_order_acquire)) return;
    const int dChan = juce::jlimit(0, Sequencer::NUM_CHANNELS - 1, proc.keysDrawTakeChan.load());
    const int dPat  = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, proc.keysDrawTakePat.load());
    if (takesForPatChan(dPat, dChan) < 20 && (int) proc.keysTakes.size() < DrumSequencerProcessor::KEYS_TAKES_MAX)
    {
        DrumSequencerProcessor::KeysTake t;
        t.isDraw = true;
        t.channel = dChan;
        t.drawPat = dPat;
        t.drawNotes.assign(proc.keysDrawTakeNotes,
                           proc.keysDrawTakeNotes + juce::jlimit(0, DrumSequencerProcessor::DRAW_TAKE_MAX, proc.keysDrawTakeCount));
        t.name = juce::Time::getCurrentTime().toString(true, true, true, true) + "  #" + juce::String((int) proc.keysTakes.size() + 1);
        proc.keysTakes.push_back(std::move(t));
    }
    proc.keysDrawTakeReady.store(false, std::memory_order_release);
    // If this pattern+channel just filled up (or the preset hit the cap), stop recording.
    if ((takesForPatChan(dPat, dChan) >= 20 || (int) proc.keysTakes.size() >= DrumSequencerProcessor::KEYS_TAKES_MAX)
        && proc.keysRecording.load())
        keysStopRecord(true);
    refreshKeysPanel();
}

// LOAD a take: clear its channel in every pattern the take touches, then replay its notes -
// the grid shows the take's pitches and the pattern plays it.
void DrumSequencerEditor::keysLoadTake(int idx)
{
    if (idx < 0 || idx >= (int) proc.keysTakes.size()) return;
    const auto& t = proc.keysTakes[(size_t) idx];
    if (t.isDraw)
    {
        // Take notes are in CONCAT columns for a group take (drawPat = the head) - split per bar.
        auto& sq = proc.sequencer;
        const int head = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, t.drawPat);
        const int end  = sq.groupEnd(head);
        for (int b = head; b <= end; ++b)
        { auto& cb = sq.patterns[b].channels[t.channel]; cb.drawMode = true; cb.clearDrawNotes(); }
        for (const auto& nt : t.drawNotes)
        {
            const int b = juce::jlimit(0, end - head, (int) nt.start / DrumChannel::DRAW_RES);
            sq.patterns[head + b].channels[t.channel].addDrawNote((int) nt.start - b * DrumChannel::DRAW_RES,
                                                                  nt.len, nt.semi, nt.vel, nt.slot, nt.glide, nt.oneShot,
                                                                  nt.strumUp, nt.strumPct == 255 ? -1 : nt.strumPct, nt.pan);
        }
        selectChannel(t.channel);
        strips[t.channel].comboSteps.setSelectedId(StepGridComponent::DRAW_ITEM_ID, juce::dontSendNotification);
        refreshDrawModeButtons();
        stepGrid.update(proc.sequencer, proc.anySolo);
        keysLoadedTakeIdx = idx; keysLoadedTakeHash = takeDataHash(t);   // track for save-to / save-as-new
        return;
    }
    bool touched[Sequencer::NUM_PATTERNS] = {};
    for (const auto& e : t.evts) if (e.pattern < Sequencer::NUM_PATTERNS) touched[e.pattern] = true;
    for (int pp = 0; pp < Sequencer::NUM_PATTERNS; ++pp)
        if (touched[pp])
        {
            auto& c = proc.sequencer.patterns[pp].channels[t.channel];
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i)
            { c.steps[i] = false; c.stepPitch[i] = 0.0f; c.stepMerge[i] = false; c.stepNoteLen[i] = 0.0f; }
        }
    for (const auto& e : t.evts)
        if (e.pattern < Sequencer::NUM_PATTERNS && e.step < DrumChannel::MAX_STEPS)
        {
            auto& c = proc.sequencer.patterns[e.pattern].channels[t.channel];
            if (e.flags & 2) { c.stepNoteLen[e.step] = juce::jlimit(0, 100, (int) e.semis) / 100.0f; continue; }
            c.steps[e.step] = true; c.stepPitch[e.step] = (float) e.semis;
            c.stepMerge[e.step] = (e.flags & 1) != 0;
        }
    selectChannel(t.channel);
    stepGrid.update(proc.sequencer, proc.anySolo);
    keysLoadedTakeIdx = idx; keysLoadedTakeHash = takeDataHash(t);   // track for save-to / save-as-new
}

void DrumSequencerEditor::keysDeleteTake(int idx)
{
    if (idx < 0 || idx >= (int) proc.keysTakes.size()) return;
    proc.keysTakes.erase(proc.keysTakes.begin() + idx);   // snapshots only - the channel keeps what's loaded
    keysLoadedTakeIdx = -1;   // indices shifted - drop the loaded-take link
    refreshKeysPanel();
}

// Fingerprint a take's data (draw lane or step events) - used to tell if a loaded take was edited.
juce::int64 DrumSequencerEditor::takeDataHash(const DrumSequencerProcessor::KeysTake& t) const
{
    juce::int64 h = 5381;
    auto mix = [&](juce::int64 v) { h = h * 33 ^ v; };
    mix(t.channel); mix(t.isDraw ? 1 : 0);
    if (t.isDraw) { mix(t.drawPat); for (const auto& nt : t.drawNotes)
                    { mix(nt.start); mix(nt.len); mix((int) nt.semi + 128); mix((int) nt.vel); mix((int) nt.slot); mix((int) nt.glide); mix((int) nt.oneShot); mix((int) nt.strumUp); mix((int) nt.strumPct); mix((int) nt.pan); mix((int) nt.condLen); mix((int) nt.condMask); } }
    else for (auto& e : t.evts) { mix(e.pattern); mix(e.step); mix((int) e.semis + 128); mix(e.flags); }
    return h;
}

// Snapshot the live channel (in pattern `pat`) as a take - the reverse of keysLoadTake.
DrumSequencerProcessor::KeysTake DrumSequencerEditor::captureTakeFromChannel(int ch, int pat) const
{
    DrumSequencerProcessor::KeysTake t; t.channel = juce::jlimit(0, Sequencer::NUM_CHANNELS - 1, ch);
    auto& c = proc.sequencer.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, pat)].channels[t.channel];
    if (c.drawMode)
    {
        // Group channels snapshot the WHOLE group in concat columns (drawPat = the head).
        auto& sq = proc.sequencer;
        const int head = sq.groupHead(pat), end = sq.groupEnd(pat);
        t.isDraw = true; t.drawPat = head;
        for (int b = head; b <= end; ++b)
        {
            const auto& cb = sq.patterns[b].channels[t.channel];
            for (int i = 0; i < cb.drawNoteCount && (int) t.drawNotes.size() < DrumSequencerProcessor::DRAW_TAKE_MAX; ++i)
            {
                auto nt = cb.drawNotes[i];
                nt.start = (int16_t) (nt.start + (b - head) * DrumChannel::DRAW_RES);
                t.drawNotes.push_back(nt);
            }
        }
    }
    else for (int s = 0; s < c.numSteps; ++s) if (c.steps[s])
    {
        t.evts.push_back({ (uint8_t) pat, (uint8_t) s, (int8_t) juce::roundToInt(c.stepPitch[s]), (uint8_t) (c.stepMerge[s] ? 1 : 0) });
        if (c.stepNoteLen[s] > 0.001f)
            t.evts.push_back({ (uint8_t) pat, (uint8_t) s, (int8_t) juce::jlimit(0, 100, juce::roundToInt(c.stepNoteLen[s] * 100.0f)), (uint8_t) 2 });
    }
    return t;
}

int DrumSequencerEditor::takePatternOf(const DrumSequencerProcessor::KeysTake& t) const
{
    if (t.isDraw) return juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, t.drawPat);
    return t.evts.empty() ? 0 : juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, (int) t.evts[0].pattern);
}
int DrumSequencerEditor::takesForPatChan(int pat, int ch) const
{ int n = 0; for (auto& t : proc.keysTakes) if (t.channel == ch && takePatternOf(t) == pat) ++n; return n; }

bool DrumSequencerEditor::keysTakeDirty(int idx) const
{
    if (idx < 0 || idx != keysLoadedTakeIdx || idx >= (int) proc.keysTakes.size()) return false;
    const auto& t = proc.keysTakes[(size_t) idx];
    const int pat = t.isDraw ? t.drawPat : currentPattern();
    return takeDataHash(captureTakeFromChannel(t.channel, pat)) != keysLoadedTakeHash;
}

void DrumSequencerEditor::refreshKeysPanel()
{
    keysHighlightMaskLo = keysHighlightMaskHi = ~0ULL;   // channel/slot settings may have changed -> recompute the key highlight next tick
    keysHighlightArpNote = -2;
    const bool recLive = proc.keysRecording.load() || keysCountdownTicks > 0;
    keysPanel.btnRec.setButtonText(recLive ? juce::String::charToString(0x25A0) + " STOP"    // filled square
                                           : juce::String::charToString(0x25CF) + " REC");   // filled circle
    keysPanel.btnRec.setColour(juce::TextButton::buttonColourId,
                               recLive ? juce::Colour(0xffcc2222) : juce::Colour(0xff20203a));
    keysPanel.btnRec.setColour(juce::TextButton::textColourOffId,
                               recLive ? juce::Colours::white : juce::Colour(0xffff5548));   // red dot + text when idle
    const int patTakes = takesForPatChan(currentPattern(), selectedChannel);   // takes for THIS pattern+channel
    const bool presetFull = (int) proc.keysTakes.size() >= DrumSequencerProcessor::KEYS_TAKES_MAX;
    keysPanel.btnRec.setEnabled(recLive || (patTakes < 20 && ! presetFull));
    keysPanel.btnTakes.setButtonText("Takes (" + juce::String(patTakes)   // per pattern+channel count, matches the filtered menu
                                     + (patTakes >= 20 ? "/20 FULL)" : (presetFull ? " - preset full)" : ")")));
    const bool prevIgnore = ignoreKnobCallbacks;
    ignoreKnobCallbacks = true;
    auto& kch = proc.sequencer.channel(selectedChannel);
    { const int t = -kch.keysSlot2Down;   // musical transpose (+ up / - down)
      keysPanel.btnSlot2.setButtonText(t == 0 ? "0 st" : (t > 0 ? "+" + juce::String(t) + " st"
                                                                : juce::String(t) + " st")); }
    keysPanel.humanKnob.setValue(kch.humanizeAmt, juce::dontSendNotification);
    keysPanel.strumKnob.setValue(kch.strumAmt,    juce::dontSendNotification);
    keysPanel.minVelKnob.setValue(kch.keysMinVel, juce::dontSendNotification);
    keysPanel.maxVelKnob.setValue(kch.keysMaxVel, juce::dontSendNotification);
    keysPanel.glideKnob.setValue(kch.keysGlide,   juce::dontSendNotification);
    // [2026-07-16 round-2] Glide = the pitch-slide time in EVERY mode now (two-axis model: the
    // Mode dropdown owns the envelope question, this knob owns the slide question) - never faded.
    keysPanel.glideKnob.setEnabled(true); keysPanel.glideKnob.setAlpha(1.0f);
    // ARP editor: mirror the channel's arp state in, and push edits back out (+ mark modified / undoable).
    for (int si = 0; si < 2; ++si)
    {
        const auto& sl = kch.slots[si];
        keysPanel.scaleBox.v[si] = { sl.scaleOn, juce::jlimit(0, kNumScales - 1, sl.scaleType),
                                     ((sl.scaleKey % 12) + 12) % 12, juce::jlimit(1, 7, sl.scaleUnison) };
    }
    keysPanel.scaleBox.slot = juce::jmin(1, envTargetSlot());   // [2026-07-16] the chip mirrors the ONE slot selection
    keysPanel.scaleBox.repaint();
    { const int p0 = kch.mergeWith;
      const int first  = (p0 >= 0) ? juce::jmin(selectedChannel, p0) : -1;
      const int second = (p0 >= 0) ? juce::jmax(selectedChannel, p0) : -1;
      keysPanel.splitBox.chFirst = first; keysPanel.splitBox.chSecond = second;
      const auto& fc = proc.sequencer.channel(first >= 0 ? first : selectedChannel);
      keysPanel.splitBox.w1 = fc.keysSplitW1;
      keysPanel.splitBox.w2 = fc.keysSplitW2;
      keysPanel.splitBox.repaint(); }
    keysPanel.arpEditor.on   = kch.arpOn;
    keysPanel.arpEditor.len  = kch.arpLen;
    keysPanel.arpEditor.sync = kch.arpSync;
    keysPanel.arpEditor.rate = kch.arpRate;
    keysPanel.arpEditor.align = kch.arpAlign;
    keysPanel.arpEditor.hold  = kch.arpHold;
    keysPanel.arpEditor.gate  = kch.arpGate;
    keysPanel.arpEditor.altStrum = kch.arpAltStrum;
    for (int ai = 0; ai < DrumChannel::ARP_ROWS; ++ai) keysPanel.arpEditor.off[ai] = kch.arpOffset[ai];
    keysPanel.arpEditor.repaint();
    kbGuideApplied = -1;        // channel/slot scale may have changed -> re-evaluate the key guide
    updateKeyboardGuide();
    keysPanel.btnArp.setColour(juce::TextButton::buttonColourId, kch.arpOn ? juce::Colour(0xff35b56a) : juce::Colour(0xff20203a));
    keysPanel.btnArp.setColour(juce::TextButton::textColourOffId, kch.arpOn ? juce::Colours::black : juce::Colours::lightgrey);
    keysPanel.btnArp.repaint();
    keysPanel.arpEditor.onChange = [this] {
        if (ignoreKnobCallbacks) return;
        auto& c = proc.sequencer.channel(selectedChannel);
        c.arpOn = keysPanel.arpEditor.on; c.arpLen = keysPanel.arpEditor.len;
        c.arpSync = keysPanel.arpEditor.sync; c.arpRate = keysPanel.arpEditor.rate;
        c.arpAlign = keysPanel.arpEditor.align; c.arpHold = keysPanel.arpEditor.hold; c.arpGate = keysPanel.arpEditor.gate;
        c.arpAltStrum = keysPanel.arpEditor.altStrum;
        for (int ai = 0; ai < DrumChannel::ARP_ROWS; ++ai) c.arpOffset[ai] = keysPanel.arpEditor.off[ai];
        // The Arp button's green face must follow the popup's ON/OFF LIVE (it used to wait for the
        // next refreshKeysPanel = a key press - "I need to press a key to see if it's on").
        keysPanel.btnArp.setColour(juce::TextButton::buttonColourId, c.arpOn ? juce::Colour(0xff35b56a) : juce::Colour(0xff20203a));
        keysPanel.btnArp.setColour(juce::TextButton::textColourOffId, c.arpOn ? juce::Colours::black : juce::Colours::lightgrey);
        keysPanel.btnArp.repaint();
    };
    keysPanel.btnPlayMode.setButtonText(kch.keysPolyMode ? (kch.keysLegato ? "Poly Legato" : "Poly")
                                                          : (kch.keysLegato ? "Mono Legato" : "Mono"));
    keysPanel.polyMode = kch.keysPolyMode;
    ignoreKnobCallbacks = prevIgnore;
    // SLOT OFFSET needs BOTH slots (it delays slot 2 behind slot 1); STRUM only when a slot is in Chord/Scale.
    auto slotAudible = [&](int s){ return kch.slots[s].engine >= 0 && kch.slots[s].weight > 0.001f; };
    const bool humOk   = slotAudible(0) && slotAudible(1);
    const bool strumOk = (kch.slots[0].scaleOn || kch.slots[0].chordMode > 0
                          || kch.slots[1].scaleOn || kch.slots[1].chordMode > 0);
    keysPanel.humanKnob.setEnabled(humOk); keysPanel.humanKnob.setAlpha(humOk ? 1.0f : 0.4f);
    keysPanel.lblHuman.setAlpha(humOk ? 1.0f : 0.4f);
    keysPanel.strumKnob.setEnabled(strumOk); keysPanel.strumKnob.setAlpha(strumOk ? 1.0f : 0.4f);
    keysPanel.lblStrum.setAlpha(strumOk ? 1.0f : 0.4f);
    // (the hint text above the piano was removed - the Poly + Lock-transpose toggles live there now;
    //  the colour legend moved into the keyboard's tooltip)
}

// Light up the piano keys that the currently-held note actually voices: for each slot, the chord/
// scale notes (from its own settings + slot-2 transpose) - slot 1 keys yellow, slot 2 keys pink,
// keys used by BOTH a yellow-pink blend. Cleared when no key is held. Driven by the editor timer so
// it works for the on-screen keyboard AND external MIDI (both set proc.keysHeldNote).
// Name the chord formed by a set of MIDI notes (lowest note first). Root candidates: the BASS first,
// then the other pitch classes (a non-bass match shows as a slash chord, e.g. "A min / C").
static juce::String chordNameFor(const juce::Array<int>& notes)
{
    if (notes.isEmpty()) return {};
    if (notes.size() == 1) return juce::MidiMessage::getMidiNoteName(notes[0], true, true, 4);
    bool pcSet[12] = {}; for (int n : notes) pcSet[n % 12] = true;
    juce::Array<int> pcs; for (int i = 0; i < 12; ++i) if (pcSet[i]) pcs.add(i);
    const int bassPC = notes[0] % 12;
    if (pcs.size() == 1)   // octave doublings of one pitch class = just the note
        return juce::MidiMessage::getMidiNoteName(notes[0], true, true, 4);
    struct ChordDef { const char* name; int n; int iv[7]; };
    static const ChordDef tab[] = {
        // -- intervals (2 notes; standard names: m/M 2nd 3rd 6th 7th, P4, tritone, P5) --
        { "m2", 2, {0,1} },  { "M2", 2, {0,2} },  { "m3", 2, {0,3} },  { "M3", 2, {0,4} },
        { "P4", 2, {0,5} },  { "TT", 2, {0,6} },  { "5", 2, {0,7} },   { "m6", 2, {0,8} },
        { "M6", 2, {0,9} },  { "m7", 2, {0,10} }, { "M7", 2, {0,11} },
        // -- triads --
        { "maj", 3, {0,4,7} },  { "min", 3, {0,3,7} },  { "dim", 3, {0,3,6} },  { "aug", 3, {0,4,8} },
        { "sus2", 3, {0,2,7} }, { "sus4", 3, {0,5,7} },
        // -- 7th family (4 notes) --
        { "7", 4, {0,4,7,10} },    { "maj7", 4, {0,4,7,11} }, { "min7", 4, {0,3,7,10} },
        { "minMaj7", 4, {0,3,7,11} }, { "m7b5", 4, {0,3,6,10} }, { "dim7", 4, {0,3,6,9} },
        { "6", 4, {0,4,7,9} },     { "min6", 4, {0,3,7,9} },  { "add9", 4, {0,2,4,7} },
        { "minAdd9", 4, {0,2,3,7} }, { "7sus4", 4, {0,5,7,10} }, { "7b5", 4, {0,4,6,10} },
        { "aug7", 4, {0,4,8,10} }, { "augMaj7", 4, {0,4,8,11} },
        // -- 9th family (5 notes; a 5-note SCALE stack lands here, e.g. Notes x5 Major = maj9) --
        { "9", 5, {0,2,4,7,10} },     { "maj9", 5, {0,2,4,7,11} },  { "min9", 5, {0,2,3,7,10} },
        { "minMaj9", 5, {0,2,3,7,11} }, { "6/9", 5, {0,2,4,7,9} },  { "min6/9", 5, {0,2,3,7,9} },
        { "7b9", 5, {0,1,4,7,10} },   { "7#9", 5, {0,3,4,7,10} },   { "min7b9", 5, {0,1,3,7,10} },
        { "m7b5(b9)", 5, {0,1,3,6,10} }, { "9sus4", 5, {0,2,5,7,10} },
        // -- 11th family (6 notes) --
        { "11", 6, {0,2,4,5,7,10} },  { "maj11", 6, {0,2,4,5,7,11} }, { "min11", 6, {0,2,3,5,7,10} },
        { "maj9#11", 6, {0,2,4,6,7,11} },
        // -- 13th family (7 notes = a full scale stacked) --
        { "13", 7, {0,2,4,5,7,9,10} }, { "maj13", 7, {0,2,4,5,7,9,11} }, { "min13", 7, {0,2,3,5,7,9,10} }
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int root : pcs)
        {
            if ((pass == 0) != (root == bassPC)) continue;
            bool rel[12] = {};
            for (int p : pcs) rel[(p - root + 12) % 12] = true;
            for (const auto& cd : tab)
            {
                if (cd.n != pcs.size()) continue;
                bool ok = true;
                for (int k = 0; k < cd.n && ok; ++k) ok = rel[cd.iv[k]];
                if (ok)
                {
                    juce::String out = juce::String(kUiNoteName[root]) + " " + cd.name;
                    if (root != bassPC) out << " / " << kUiNoteName[bassPC];
                    return out;
                }
            }
        }
    juce::String out;   // no dictionary match (fingers everywhere?): list the notes, capped at 6
    int shown = 0;
    for (int p : pcs) { if (shown++ == 6) { out << "..."; break; } out << kUiNoteName[p] << " "; }
    return out.trim();
}

void DrumSequencerEditor::updateKeyboardHighlight()
{
    // POLY: light the UNION of every held note's voicing. Change-gated on the held-note MASK
    // (the processor mirrors its held stack into two atomics).
    const bool active = keysView && detailShown;
    const uint64_t mLo = active ? proc.keysHeldMaskLo.load(std::memory_order_relaxed) : 0;
    const uint64_t mHi = active ? proc.keysHeldMaskHi.load(std::memory_order_relaxed) : 0;
    const int arpNow = active ? proc.arpSoundingUi.load(std::memory_order_relaxed) : -1;
    if (mLo == keysHighlightMaskLo && mHi == keysHighlightMaskHi && arpNow == keysHighlightArpNote) return;   // nothing changed
    keysHighlightMaskLo = mLo; keysHighlightMaskHi = mHi; keysHighlightArpNote = arpNow;
    keysPanel.clearKeyTints();
    if (mLo != 0 || mHi != 0)
    {
        const auto& c = proc.sequencer.channel(selectedChannel);
        bool used[2][128] = {};
        // A per-(held, slot) light: chord/scale-expand the note and honour slot-2 transpose.
        auto lightNote = [&](int held, int s) {
            const auto& sl = c.slots[s];
            if (sl.engine < 0 || sl.weight <= 0.001f) return;   // empty/muted slot
            const bool pitched = (sl.engine == DrumChannel::SrcOsc || sl.engine == DrumChannel::SrcModal
                                  || sl.engine == DrumChannel::SrcPhys);
            const bool transposes = pitched || (sl.engine == DrumChannel::SrcSample && ! sl.smpPreservePitch);
            const int base = held - (s == 1 && transposes ? c.keysSlot2Down : 0);
            auto add = [&](int off) { if (off < -90) return;   // guitar voicing: missing string
                                      const int m = base + off; if (m >= 0 && m < 128) used[s][m] = true; };
            if (pitched && sl.scaleOn) { const int nv = juce::jlimit(1, DrumChannel::UNI_MAX, sl.scaleType >= 10 ? 6 : sl.scaleUnison);
                for (int u = 0; u < nv; ++u) add(DrumChannel::scaleNoteOffset(sl.scaleType, sl.scaleKey, base, u)); }
            else if (pitched && sl.chordMode > 0) { const int nv = juce::jlimit(1, DrumChannel::UNI_MAX, sl.chordUnison);
                for (int u = 0; u < nv; ++u) add(DrumChannel::chordNoteOffset(sl.chordMode, u)); }
            else add(0);                                        // plain note (Osc/Modal/Phys single, or Sample/Noise)
        };
        int root = -1;
        for (int i = 0; i < 128; ++i) if ((i < 64 ? mLo : mHi) & (1ULL << (i & 63))) { root = i; break; }
        if (c.arpOn && root >= 0)   // ARP: the highlight FOLLOWS the riff - only the note sounding RIGHT NOW
        {                           // lights up (rest steps go dark); each key waits its turn (user request)
            if (arpNow >= 0)
                for (int s = 0; s < DrumChannel::NUM_SLOTS; ++s) lightNote(arpNow, s);
        }
        else
        for (int held = 0; held < 128; ++held)
        {
            if (! ((held < 64 ? mLo : mHi) & (1ULL << (held & 63)))) continue;   // not held
            if (c.mergeWith >= 0)
            {   // MERGE&SPLIT: the pressed half picks a CHANNEL. The SELECTED channel's half lights in
                // its slot colours; the PARTNER's half lights BLUE (it is another channel's sound).
                const int first = juce::jmin(selectedChannel, (int) c.mergeWith);
                const auto& fc = proc.sequencer.channel(first);
                const bool left = held < 60;
                const int mapped = juce::jlimit(0, 127, left ? fc.keysSplitW2 + (held - 12)
                                                             : fc.keysSplitW1 + (held - 60));
                const int tgt = left ? juce::jmax(selectedChannel, (int) c.mergeWith) : first;
                if (tgt == selectedChannel)
                    for (int s = 0; s < DrumChannel::NUM_SLOTS; ++s) lightNote(mapped, s);
                else
                    keysPanel.setKeyTint(mapped, juce::Colour(0xff4a7fff));   // the OTHER channel = blue
            }
            else
                for (int s = 0; s < DrumChannel::NUM_SLOTS; ++s) lightNote(held, s);
        }
        const juce::Colour y(0xffe8bf4d), p(0xffe86aa8);   // slot 1 yellow / slot 2 pink (match the sound editor)
        for (int m = 0; m < 128; ++m)
            if (used[0][m] && used[1][m]) keysPanel.setKeyTint(m, y.interpolatedWith(p, 0.5f));
            else if (used[0][m])          keysPanel.setKeyTint(m, y);
            else if (used[1][m])          keysPanel.setKeyTint(m, p);
        keysPanel.applyKeyTints();
        // LIVE CHORD NAMES: slot 1, slot 2, and ALL (the UNION of both slots - every key sounding,
        // whichever slot makes it). Random hand-fulls that match nothing show the note list instead.
        juce::Array<int> s1, s2, all;
        for (int m = 0; m < 128; ++m)
        {
            if (used[0][m]) s1.add(m);
            if (used[1][m]) s2.add(m);
            if (used[0][m] || used[1][m]) all.add(m);
        }
        keysPanel.lblChord[0].setText(s1.isEmpty() ? juce::String() : "1: " + chordNameFor(s1), juce::dontSendNotification);
        keysPanel.lblChord[1].setText(s2.isEmpty() ? juce::String() : "2: " + chordNameFor(s2), juce::dontSendNotification);
        keysPanel.lblChord[2].setText(all.isEmpty() ? juce::String() : "All: " + chordNameFor(all), juce::dontSendNotification);
    }
    else for (auto& l : keysPanel.lblChord) l.setText({}, juce::dontSendNotification);
}

// KEY GUIDE: dim the keyboard keys OUTSIDE the chosen scale (display only; change-gated).
// MERGE & SPLIT a pair of ADJACENT channels (toggle). Pairing is CHANNEL-WIDE (like routing):
// written to BOTH channels in EVERY pattern; merging forces both into PIANO ROLL everywhere
// (pairs are roll-only). Any existing pairing of either party is dissolved first.
void DrumSequencerEditor::setChannelMerge(int a, int b)
{
    if (a == b || b < 0 || b >= Sequencer::NUM_CHANNELS || std::abs(a - b) != 1) return;
    auto& sq = proc.sequencer;
    // Channel Merge & Split is PER PATTERN now (user: it must not spread to every pattern). It DOES
    // span a MERGED-PATTERN group (head..end) - those bars are one unit, so the pairing follows.
    const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
    const bool unmerge = sq.patterns[head].channels[a].mergeWith == b;
    auto clearSteps = [](DrumChannel& cc) { cc.clearStepData(); };
    auto doIt = [this, a, b, head, end, unmerge, clearSteps]() {
        auto& sq2 = proc.sequencer;
        auto clearPairOf = [&](int ch) {   // dissolve any existing pairing of ch, across this group only
            const int p = sq2.patterns[head].channels[ch].mergeWith;
            if (p < 0) return;
            for (int bp = head; bp <= end; ++bp) { sq2.patterns[bp].channels[ch].mergeWith = -1;
                                                   sq2.patterns[bp].channels[p].mergeWith = -1; } };
        clearPairOf(a); clearPairOf(b);
        if (! unmerge)
            for (int bp = head; bp <= end; ++bp) {
                auto& ca = sq2.patterns[bp].channels[a]; auto& cb = sq2.patterns[bp].channels[b];
                ca.mergeWith = b; cb.mergeWith = a;
                ca.drawMode = true; cb.drawMode = true;   // pairs are PIANO-ROLL only -> clear any steps
                clearSteps(ca); clearSteps(cb);
            }
        keysHighlightMaskLo = keysHighlightMaskHi = ~0ULL;
        refreshKeysPanel(); refreshChannelStrips(); stepGrid.repaint();
    };
    if (! unmerge) {
        auto stepDataIn = [&](int c) -> bool {
            for (int bp = head; bp <= end; ++bp) { auto& cc = sq.patterns[bp].channels[c];
                if (cc.drawMode) continue; for (int s = 0; s < DrumChannel::MAX_STEPS; ++s) if (cc.steps[s]) return true; }
            return false; };
        if (stepDataIn(a) || stepDataIn(b)) {
            juce::PopupMenu w;
            w.addSectionHeader("Merge & Split channels " + juce::String(a + 1) + " + " + juce::String(b + 1) + "?");
            w.addSectionHeader("One or both are in step mode - merging switches them to Piano Roll and CLEARS their steps (you can Undo).");
            w.addItem(1, "Clear steps and merge");
            w.addItem(2, "Cancel");
            w.showMenuAsync(juce::PopupMenu::Options(), [this, doIt](int r) { if (r == 1) { commitUndoNow(); doIt(); } });
            return;
        }
    }
    commitUndoNow();   // merge/unmerge is undoable
    doIt();
}

void DrumSequencerEditor::updateKeyboardGuide()
{
    // Build a 12-bit pitch-class mask of the keys that stay LIT (-1 = guide off / nothing to dim).
    auto scaleMask = [](int key, int type) {
        type = juce::jlimit(0, kNumScales - 1, type);
        if (type == 12) return 0xFFF;   // POWER is chromatic: every key is a valid root - nothing dims
        if (type >= 10) type -= 10;   // guitar voicings snap in Major / Natural Minor - same lit keys
        int m = 0;
        for (int k = 0; k < kUiScaleLen[type]; ++k) m |= 1 << ((key + kUiScaleTab[type][k]) % 12);
        return m;
    };
    int mask = -1;   // ("Follow sound" was removed - it just complicated things; pick a key+scale directly)
    if (proc.kbGuideMode == 2) mask = scaleMask(proc.kbGuideKey, proc.kbGuideScale);
    keysPanel.lblGuideCur.setText(proc.kbGuideMode == 2                       // caption under the button;
        ? juce::String(kUiNoteName[proc.kbGuideKey]) + " " + kUiScaleName[proc.kbGuideScale]
        : juce::String("Off"), juce::dontSendNotification);                   // setText no-ops if unchanged
    if (mask == kbGuideApplied) return;
    kbGuideApplied = mask;
    keysPanel.clearKeyDims();
    if (mask >= 0)
        for (int m = 0; m < 128; ++m) if (! ((mask >> (m % 12)) & 1)) keysPanel.setKeyDim(m, true);
    keysPanel.applyKeyTints();   // repaint
}

// Show the selected slot's amp envelope in the graph.
void DrumSequencerEditor::loadEnvIntoEditor()
{
    const auto& s = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    const bool isModal  = (s.engine == DrumChannel::SrcModal);
    const bool isSample = (s.engine == DrumChannel::SrcSample);
    // Modal: the Ring handle IS the modal decay (modalDecay 0..1 -> 0.05..4 s, the DSP's mapping), shown in seconds.
    if (isModal) envEditor.setValues(s.atk, 0.0f, 0.05f + s.modalDecay * 3.95f, 0.0f, 0.0f);
    else         envEditor.setValues(s.atk, s.hold, s.dec, s.sustain, s.release);
    // Samples: the amp envelope is OPT-IN (smpEnvOn; double-click the graph to enable). Off = the sample
    // plays its full (trimmed) length untouched - exactly the legacy behaviour.
    const bool ampApplies = ! isSample || s.smpEnvOn;
    envEditor.setNa("SAMPLE ENVELOPE (off)", "double-click to enable a fade-in/out envelope");
    envEditor.setEnabledLook(ampApplies);
    envEditor.setToggleable(isSample);
    // Physical + Modal (both struck/plucked) get a tailored 2-handle Strike(onset softness)/Ring(decay) editor - no
    // Hold/Sustain (they don't fit a struck body). For Physical the Strike sustains the string so a slow strike hits full.
    envEditor.setStrikeRing(s.engine == DrumChannel::SrcPhys || isModal,
                            /*allowSus: both bodies can be HELD now (string ebow / bell bow)*/ true);
    envEditor.setSusRelVisible(! isSample);   // samples don't gate - hide the meaningless S/R
}

// The pitch-envelope X-axis length. For samples it's the (trimmed) SAMPLE length taken
// straight from the sample group (samples have no AHDSR); otherwise the amp envelope.
float DrumSequencerEditor::pitchEnvLenSec(int slotIdx)
{
    auto& ch = proc.sequencer.channel(selectedChannel);
    const auto& sl = ch.slots[slotIdx];
    if (sl.engine == DrumChannel::SrcSample)
    {
        const double frames = (double) ch.getSampleNumFrames(slotIdx);
        if (frames > 0.0)
        {
            const double frac = sl.smpUseRegion ? juce::jmax(0.0, (double)(sl.smpEnd - sl.smpStart)) : 1.0;
            const double spd  = juce::jmax(0.05, (double) sl.smpSpeed);  // pitch is baked (length-preserving)
            double sr = proc.getSampleRate(); if (sr <= 0.0) sr = 44100.0;
            return (float) (frames * frac / spd / sr);   // actual playback duration -> playhead stays in sync
        }
    }
    if (sl.engine == DrumChannel::SrcModal)
        return sl.atk + 0.05f + sl.modalDecay * 3.95f;   // Strike + Ring (matches the DSP's modal time base)
    return sl.atk + sl.hold + sl.dec;   // AHD perceptual length (matches the amp-env "length" read-out)
}

// Show the selected slot's pitch envelope + Unison/Detune/Vibrato in their controls.
void DrumSequencerEditor::loadPitchAndVoice()
{
    auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    // Pitch env applies to every engine except Noise (no pitch) and empty slots. Modal is swept at
    // BLOCK rate (its resonator bank is re-tuned once per block - a per-sample sweep is too heavy).
    const bool hasPitch = (sl.engine >= 0 && sl.engine != DrumChannel::SrcNoise);
    pitchEditor.setEnabledLook(hasPitch);
    pitchEditor.setDots(sl.pEnvP, sl.pEnvT);
    // X-axis = the sound's length (sample length for samples, else amp env) so the dots/playhead stay synced.
    pitchEditor.setLengthSec(pitchEnvLenSec(envTargetSlot()));
    const juce::ScopedValueSetter<bool> guard(ignoreKnobCallbacks, true);
    // Voice visual: Unison/Detune/Chord + Vibrato work on the pitched engines - Oscillator, Modal
    // (bank-split notes) AND Physical (real multi-string KS, cap 3). Sample gets Vibrato only
    // (varispeed wobble); Noise has no pitch.
    const int e = sl.engine;
    const bool uniOn = (e == DrumChannel::SrcOsc || e == DrumChannel::SrcFM || e == DrumChannel::SrcSynth
                        || e == DrumChannel::SrcModal || e == DrumChannel::SrcPhys);
    const bool vibOn = uniOn || e == DrumChannel::SrcSample;
    juce::String naReason;
    if (!uniOn && !vibOn)
        naReason = (e == DrumChannel::SrcNoise) ? "(n/a - noise has no pitch)" : "(no engine)";
    // Per-engine unison cap so you can't select more voices than the engine builds: Physical = 3
    // real strings, Modal = 4 banks, Oscillator = 7. (User: "up to 3 for physical but I can select
    // more" - now the handle stops at the real max.)
    voiceMod.setMaxUni(e == DrumChannel::SrcModal ? 3 : e == DrumChannel::SrcPhys ? 6 : 16);   // KS = 6 real strings; Osc = 16 (supersaw)
    voiceMod.setValues((int) sl.oscUnison, sl.chordUnison, sl.scaleUnison, sl.oscDetune, sl.vibrato, sl.oscUniCenter, sl.oscDetuneMode, sl.chordMode, sl.scaleOn, sl.scaleType, sl.scaleKey, sl.uniSpread, sl.drift);
    voiceMod.setSupport(uniOn, vibOn, naReason);
}

// Apply the dragged amp envelope to the selected slot (live; no coeff rebuild needed).
void DrumSequencerEditor::applyEnvToTargets(float a, float h, float d, float s, float r)
{
    auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    if (sl.engine == DrumChannel::SrcModal) {   // Ring handle (seconds) -> modalDecay (0..1; inverse of 0.05..4 s)
        // Sustain (Ring handle Y) + Release (Release handle) MUST be written here too - they were
        // missing, so dragging them did nothing on Modal (the DSP honoured them, the UI never set them).
        sl.atk = a; sl.modalDecay = juce::jlimit(0.0f, 1.0f, (d - 0.05f) / 3.95f);
        sl.sustain = s; sl.release = r;
    } else {
        sl.atk = a; sl.hold = h; sl.dec = d; sl.sustain = s; sl.release = r;
    }
}

// Apply the dragged pitch envelope to the selected slot's per-engine fields.
void DrumSequencerEditor::applyPitch(float amt, float time, float off)
{
    auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    PEnvRef pe = slotPEnv(sl);
    if (pe.amt) { *pe.amt = amt; *pe.time = time; *pe.off = off; }
}

// A slot dropdown changed. Besides the engine items, the "Sample" submenu yields
// sample-file ids (SAMPLE_ID_BASE+) and the Load/Open actions - those pick the Sample
// engine AND load a sample (so there's no second stacked dropdown).
void DrumSequencerEditor::onSlotEngineChange(int box)
{
    const int id = slotCombo[box].getSelectedId();
    auto& ch = proc.sequencer.channel(selectedChannel);
    const int oldEng = boxEngine[box];   // to detect a real engine change (cancel now-meaningless sound-engine mod routes)

    if (id == ID_BROWSE)
    {
        openSoundBrowser(box);
        syncBoxesFromSrcOn();   // restore this combo's display (the action isn't an engine)
        return;
    }
    if (id == ID_REFRESH_SAMPLES)   // re-scan the folder so files added on disk appear in the list
    {
        rescanSamples(); rebuildSampleMenu();
        syncBoxesFromSrcOn();   // restore this combo's display (the action isn't an engine)
        return;
    }
    if (id == ID_SHOW_SAMPLES)      // open the Samples folder so the user can drop files in
    {
        getSamplesFolder().revealToUser();
        syncBoxesFromSrcOn();   // restore this combo's display (the action isn't an engine)
        return;
    }
    if (id >= SAMPLE_ID_BASE && id < ID_INIT_MIX)   // a specific sample file
    {
        boxEngine[box] = DrumChannel::SrcSample; ch.slots[box].engine = DrumChannel::SrcSample;
        const int idx = id - SAMPLE_ID_BASE;
        if (idx >= 0 && idx < sampleFiles.size()) { ch.loadUserSample(box, sampleFiles[idx]); cacheWaveform(selectedChannel); }
    }
    else
    {
        boxEngine[box] = (id <= 1) ? -1 : id - 2;   // None / Noise / Analog+FM / FM / Physical / Synth / Wave / Modal / Granular
        ch.slots[box].engine = boxEngine[box];
        ch.silenceAllVoices();   // don't let a voice ringing on the OLD engine get re-read as the new one (= noise)
        ch.ensureKsBuffers();    // KS lines are lazily allocated; a KS engine just got assigned (message thread)
        ch.rebuildGrainTables(); // a slot may have just become (or left) Granular (message thread)
    }
    // ENGINE CHANGED -> cancel this slot's mod routes that targeted the OLD engine's own params (they'd
    // otherwise silently modulate a DIFFERENT param on the new engine, or show a stale "Knob N"). Only the
    // sound-engine-specific targets: the grid knobs + Warp (Osc-only) + Wave/Grain Position (Osc/Grain).
    if (boxEngine[box] != oldEng)
    {
        const int ne = boxEngine[box];
        for (auto& r : ch.slots[box].mod)
        {
            const bool gridTgt = (r.tgt >= DrumChannel::MT_GRID_BASE && r.tgt < DrumChannel::MT_GRID_END);   // engine knobs only (Sub/Formant sit above + survive)
            const bool warpTgt = (r.tgt == DrumChannel::MTWarp    && ne != DrumChannel::SrcOsc);
            const bool wposTgt = (r.tgt == DrumChannel::MTWavePos && ne != DrumChannel::SrcOsc);   // Wave Position = Osc-only now
            const bool cntTgt  = (r.tgt == DrumChannel::MTUniCount && ne != DrumChannel::SrcOsc);   // [2026-07-14 02:50] count-morph = Osc only
            if (gridTgt || warpTgt || wposTgt || cntTgt) { r.src = DrumChannel::MSOff; r.tgt = DrumChannel::MTOff; r.amt = 0.0f; }
        }
        // (layoutContent() -> refreshDetailPanel() below re-reads modFaders from the selected slot.)
    }
    syncPadFromSlots (true);   // adding/removing a slot rebalances the blend
    ch.markDspDirty();
    layoutContent();
    refreshDetailPanel();
}

// Sound browser: a FileBrowserComponent window rooted at the samples folder. Double-click a file
// to load it into the slot the menu was opened from. Window self-nulls on close (deferred, safe).
void DrumSequencerEditor::openSoundBrowser(int box)
{
    browseTargetBox = juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, box);
    if (browserWin != nullptr) { browserWin->toFront(true); return; }

    auto* fb = new juce::FileBrowserComponent(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        getSamplesFolder(), &sampleFilter, nullptr);
    fb->setSize(560, 420);
    fb->addListener(this);

    struct BrowserWin : juce::DialogWindow
    {
        DrumSequencerEditor& owner;
        BrowserWin(DrumSequencerEditor& e)
            : juce::DialogWindow("Sound Browser  -  double-click a sample to load it into this slot",
                                 juce::Colour(0xff14141f), true, true), owner(e) {}
        void closeButtonPressed() override
        {
            juce::Component::SafePointer<DrumSequencerEditor> sp(&owner);
            juce::MessageManager::callAsync([sp]() mutable { if (sp) sp->browserWin.reset(); });
        }
    };
    auto w = std::make_unique<BrowserWin>(*this);
    w->setContentOwned(fb, true);
    w->setUsingNativeTitleBar(true);
    w->setResizable(true, false);
    w->centreWithSize(580, 470);
    w->setVisible(true);
    browserWin = std::move(w);
}

void DrumSequencerEditor::fileDoubleClicked(const juce::File& f)
{
    if (! f.existsAsFile()) return;
    auto& ch = proc.sequencer.channel(selectedChannel);
    const int slot = juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, browseTargetBox);
    boxEngine[slot] = DrumChannel::SrcSample;
    ch.slots[slot].engine = DrumChannel::SrcSample;
    ch.loadUserSample(slot, f);
    cacheWaveform(selectedChannel);
    ch.markDspDirty();
    rescanSamples(); rebuildSampleMenu();
    syncPadFromSlots(true); layoutContent(); refreshDetailPanel();
}

// Per-source knobs stay visible regardless of on/off state (user preference).
void DrumSequencerEditor::updateSoundsVisibility() {}

void DrumSequencerEditor::setStepEditMode(int mode)
{
    stepGrid.editMode = mode;
    auto hl = [&](juce::TextButton& b, bool on) {
        b.setColour(juce::TextButton::buttonColourId, on ? juce::Colour(0xff35c0ff) : juce::Colour(0xff2a2a4a));
        b.setColour(juce::TextButton::textColourOffId, on ? juce::Colours::black : juce::Colours::lightgrey);
    };
    hl(btnModeVel,   mode == StepGridComponent::ModeVel);
    hl(btnModeLen,   mode == StepGridComponent::ModeLen);
    hl(btnModePitch, mode == StepGridComponent::ModePitch);
    hl(btnModeProb,  mode == StepGridComponent::ModeProb);
    hl(btnModeRoll,  mode == StepGridComponent::ModeRoll);
    hl(btnModePan,   mode == StepGridComponent::ModePan);
    hl(btnModeNudge, mode == StepGridComponent::ModeNudge);
    hl(btnModeModA,  mode == StepGridComponent::ModeModA);
    hl(btnModeModB,  mode == StepGridComponent::ModeModB);
    stepGrid.repaint();
}

void DrumSequencerEditor::setupGroupHeader(juce::Label& lbl, const char* txt)
{
    content.addAndMakeVisible(lbl);
    lbl.setFont(juce::Font(11.0f, juce::Font::bold));
    lbl.setJustificationType(juce::Justification::centred);
    lbl.setColour(juce::Label::textColourId, juce::Colour(0xff7799cc));
    lbl.setText(txt, juce::dontSendNotification);
}

void DrumSequencerEditor::setupKnob(LearnableKnob& k, juce::Label& lbl, const juce::String& txt,
                                     double lo, double hi, double def, double skew,
                                     std::function<juce::String(double)> fmt)
{
    content.addAndMakeVisible(k);
    k.setRange(lo, hi);
    k.setValue(def, juce::dontSendNotification);
    if (skew != 1.0) k.setSkewFactor(skew);
    k.setDoubleClickReturnValue(true, def);

    if (fmt)
        k.textFromValueFunction = [fmt](double v) { return fmt(v); };

    // Always-visible value read-out below the knob
    k.setLookAndFeel(&knobLNF);
    k.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 46, 13);
    k.setColour(juce::Slider::textBoxTextColourId,      juce::Colours::white);
    k.setColour(juce::Slider::textBoxOutlineColourId,   juce::Colours::transparentBlack);
    k.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    allKnobs.push_back(&k);

    content.addAndMakeVisible(lbl);
    lbl.setFont(juce::Font(11.5f));
    lbl.setJustificationType(juce::Justification::centred);
    lbl.setMinimumHorizontalScale(0.85f);
    lbl.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    lbl.setText(txt, juce::dontSendNotification);
}


// MERGED GROUPS: copy one channel's SOUND from pattern src -> dst via the mix serializer (slots,
// env, EQ, FX, filter, LFO, slot-2 pitch, transpose lock - NOT steps/routing/mute). Message thread.
void DrumSequencerEditor::copyChannelSound(int srcPat, int dstPat, int ch)
{
    if (srcPat == dstPat) return;
    auto& src = proc.sequencer.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, srcPat)].channels[ch];
    auto& dst = proc.sequencer.patterns[juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, dstPat)].channels[ch];
    juce::ValueTree t("Mix");
    juce::String missing;
    writeChannelMix(t, src);
    readChannelMix(t, dst, missing);   // sample reloads hit the SampleFileCache (no re-decode)
    dst.mixName = src.mixName; dst.mixModified = src.mixModified; dst.mixHash = src.mixHash;
    dst.markDspDirty();
}

// MERGED-GROUP view: concat step index -> the bar's channel + bar-local step.
DrumChannel& DrumSequencerEditor::groupStepChannel(int ch, int& step)
{
    auto& sq = proc.sequencer;
    const int head = sq.groupHead(currentPattern()), end = sq.groupEnd(currentPattern());
    if (end <= head) return sq.channel(ch);
    int b = head;
    for (; b < end; ++b)
    {
        const int n = juce::jmax(1, sq.patterns[b].channels[ch].numSteps);
        if (step < n) break;
        step -= n;
    }
    return sq.patterns[b].channels[ch];
}

// Keep every merged-group member's sounds MIRRORING the pattern being edited (the visible one).
// Called from the timer's throttled tick; change-only via channelSoundHash, so it's a no-op while
// nothing is edited. Edits always happen on the CURRENT pattern -> sync flows current -> members.
void DrumSequencerEditor::syncMergedGroupSounds()
{
    auto& sq = proc.sequencer;
    const int cp = currentPattern();
    const int head = sq.groupHead(cp), end = sq.groupEnd(cp);
    if (end <= head) return;   // not in a group
    for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
    {
        const auto hsrc = channelSoundHash(sq.patterns[cp].channels[c]);
        for (int m = head; m <= end; ++m)
            if (m != cp && channelSoundHash(sq.patterns[m].channels[c]) != hsrc)
                copyChannelSound(cp, m, c);
    }
}

// In DRAW mode the pitch line IS the pitch, drawing sets the timing/length, and rolls make no
// sense - so grey + disable Len/Pitch/Roll for a draw channel. Vel/Pan stay (whole-channel).
void DrumSequencerEditor::refreshDrawModeButtons()
{
    const bool draw = proc.sequencer.channel(selectedChannel).drawMode;
    // PIANO ROLL: every per-note property (velocity, pan, gate, pitch, glide, strum, slot) is edited
    // INSIDE the roll - by pointer gestures and the right-click note menu. So ALL step edit-mode
    // buttons AND Influence are disabled/faded here (user); only Clear stays live. Step mode keeps them.
    for (auto* b : { &btnModeVel, &btnModeLen, &btnModePitch, &btnModeProb, &btnModeRoll, &btnModePan, &btnModeNudge, &btnModeModA, &btnModeModB })
    { b->setEnabled(! draw); b->setAlpha(draw ? 0.4f : 1.0f); }
    btnInfluenceTop.setEnabled(! draw); btnInfluenceTop.setAlpha(draw ? 0.4f : 1.0f);
    if (draw && stepGrid.editMode != StepGridComponent::ModeSteps)
        setStepEditMode(0);   // the roll shows the note view; no step edit mode is active on it
}

void DrumSequencerEditor::selectChannel(int ch)
{
    // LOCKED while recording: the audio thread stamps onto the selected channel, so switching
    // mid-take would split the take across channels.
    if (proc.keysRecording.load() || keysCountdownTicks > 0) return;
    selectedChannel = ch;
    proc.lastSelectedChannel = ch; // remember across editor open/close
    stepGrid.selectedRow = ch;     // red row outline in the grid
    stepGrid.closeDrawEditorIfNot(ch);   // don't leave another channel's piano-roll editor covering the grid
    proc.analyzeChannel.store(ch); // analyse the channel we're inspecting
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        strips[i].numBtn.setToggleState(i == ch, juce::dontSendNotification);
    btnInfluenceTop.setToggleState(stepGrid.influenceArmed[ch], juce::dontSendNotification);
    updateKnobParamIds();
    refreshDrawModeButtons();   // grey Len/Pitch/Roll when this channel is a draw lane
    syncBoxesFromSrcOn();   // set boxEngine[] from this channel's active sources
    refreshDetailPanel();
    layoutContent();        // re-place the slot boxes for this channel's engines
    updateVisuals();
    content.repaint();
}

void DrumSequencerEditor::selectPattern(int p)
{
    const int clicked = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, p);
    p = proc.sequencer.groupHead(p);   // a merged group is viewed as ONE unit - always at its head
    proc.sequencer.setCurrentPattern(p);
    // Clicking a MIDDLE bar of a group: playback (when it next starts) begins from THAT bar and runs
    // on through the rest of the group (setCurrentPattern parked playPattern at the head).
    if (! proc.sequencer.isCurrentlyPlaying && clicked != p)
        proc.sequencer.playPattern = clicked;
    stepGrid.currentPattern = p;
    updateStripParamIds();
    updateKnobParamIds();
    refreshDetailPanel();
    refreshChannelStrips();
    refreshPatternButtons();
    refreshPatternOptions();
    content.repaint();
}

// Duplicate one pattern into another (drag-copy on the pattern bar). Delegates to the processor,
// which round-trips each channel through its serializer so the SOUNDS copy too (not just steps).
void DrumSequencerEditor::copyPatternContent(int src, int dst)
{
    proc.copyPattern(src, dst);
}

void DrumSequencerEditor::updateKnobParamIds()
{
    // NO-HIDDEN-PARAMS RULE: every legacy pid here routed to a UI-less field (pan, pitch,
    // channel filter/drive, layer atk/hld/dec) - all REMOVED with their routeCC branches.
    // Channel VOLUME's learn surface = the strip METER's drag handle (addressed + ui_sel_chVol);
    // the FX row keeps FIXED ui_sel_* ids. Nothing is re-addressed per selection any more.
    // (The invisible legacy knob OBJECTS stay - v1.2.2 lesson: invisible != dead - but they
    // carry no MIDI pids now, so learning them is impossible.)
}

void DrumSequencerEditor::updateStripParamIds()
{
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        juce::String pre = "p" + juce::String(currentPattern()) + "_ch" + juce::String(i) + "_";
        strips[i].btnMute->paramId  = pre + "mute";
        strips[i].btnSolo->paramId  = pre + "solo";
        strips[i].btnPoly.paramId   = pre + "overlap";   // overlap is per-pattern channel state
    }
}

// Push the per-slot FX / Chorus / Keytrack / LFO-Sync drag-fader values for the edited slot, and
// tint every fader with the slot's colour (yellow slot 1 / pink slot 2). setValue01 never fires
// onChange, so this is safe to call from refresh without guarding ignoreKnobCallbacks.
void DrumSequencerEditor::updateFxFaders(const DrumChannel::Slot& sl)
{
    const juce::Colour acc = envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8);
    fxDriveFader.setAccent(acc);   // keytrack faders keep their F1/F2 filter colours
    fxDriveFader.setValue01(sl.fxDrive);
    knobPunch.setValue  (sl.fxPunch,      juce::dontSendNotification);
    knobRing.setValue   (sl.fxRing,       juce::dontSendNotification);
    knobSub.setValue    (sl.fxSub,        juce::dontSendNotification);
    knobFormant.setValue(sl.fxFormant,    juce::dontSendNotification);
    knobRingHz.setValue (sl.fxRingHz,     juce::dontSendNotification);
    knobSlotPan.setValue(sl.pan,          juce::dontSendNotification);
    // CHANNEL FX + sends read the CHANNEL (both slots combined) - they never follow the slot selector.
    { const auto& ch = proc.sequencer.channel(selectedChannel);
      for (int i = 0; i < 3; ++i)
      { comboChFx[i].setSelectedId(juce::jlimit(0, 16, ch.chFxType[i]) + 1, juce::dontSendNotification);
        chFxChrF[i].repaint();   // the Character read-out depends on the TYPE - repaint even when the value didn't move [2026-07-15 15:00]
        chFxAmtF[i].setValue01(ch.chFxAmt[i]);
        chFxChrF[i].setValue01(ch.chFxChar[i]);
        const bool on = ch.chFxType[i] != DrumChannel::ChFxOff;
        chFxAmtF[i].setAlpha(on ? 1.0f : 0.4f); chFxChrF[i].setAlpha(on ? 1.0f : 0.4f); }
      sendRevF.setValue01(ch.reverbSend); sendRevF.repaint();   // repaint: the bus name in the value may have changed
      sendDelF.setValue01(ch.delaySend); sendDelF.repaint(); }
    // (LFO tempo sync lives INSIDE the LfoDisplay now - setValues carries sl.lfoSync.)
}

// LFO SHAPER: open the draw overlay for one LFO tab, loaded with that tab's current curve.
void DrumSequencerEditor::openLfoCurveEditor(int dest)
{
    lfoCurveEdDest = juce::jlimit(0, 3, dest);
    auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    lfoCurveEd.setCurve(sl.lfoCurve[lfoCurveEdDest]);
    lfoCurveEd.accent = kLfoDestCol[lfoCurveEdDest];
    lfoCurveEd.setVisible(true); lfoCurveEd.toFront(false);
}

void DrumSequencerEditor::openRoutePicker(int route)
{
    routePickRoute = route;
    const int si = envTargetSlot();
    routePicker.accent = (si == 0) ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8);
    routePicker.engine = proc.sequencer.channel(selectedChannel).slots[si].engine;   // gates Warp/WavePos targets
    routePicker.remapOn = proc.sequencer.channel(selectedChannel).slots[si].mod[juce::jlimit(0, DrumChannel::MOD_ROUTES - 1, route)].curveOn != 0;
    { // [2026-07-14 11:10] the MIDI button shows this route's current assignment
      const int mcc = proc.midiLearn.getCCForParam("ui_sel_modAmt" + juce::String(route));
      routePicker.midiTag = mcc >= 0 ? "MIDI cc" + juce::String(mcc) : "MIDI"; }
    routePicker.openFor(modFaders.src[route], modFaders.tgt[route], modFaders.amt[route],
                        proc.sequencer.channel(selectedChannel).slots[si].mod[juce::jlimit(0, DrumChannel::MOD_ROUTES - 1, route)].lagMs);
    routePicker.toFront(false);
}

void DrumSequencerEditor::refreshDetailPanel()
{
    ignoreKnobCallbacks = true;
    auto& ch = proc.sequencer.channel(selectedChannel);

    // PIANO ROLL is knob-INDEPENDENT now (user): the roll plays a C4-absolute base regardless of the
    // Freq knob (handled in the DSP via DrumChannel::slotBaseHz), so the knob is NOT forced to C4, NOT
    // faded, and NOT parked on exit - it just stays the step-mode base. Nothing to do here.
    slotEd[0].freqDisabled = false; slotEd[1].freqDisabled = false;

    lblSelected.setText("Editing: Pattern " + juce::String(currentPattern() + 1)
                        + " / Channel " + juce::String(selectedChannel + 1),
                        juce::dontSendNotification);

    for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s)
    {
        knobSrcAtk[s].setValue (ch.srcAtk[s],  juce::dontSendNotification);
        knobSrcHold[s].setValue(ch.srcHold[s], juce::dontSendNotification);
        knobSrcDec[s].setValue (ch.srcDec[s],  juce::dontSendNotification);
    }
    knobPitch.setValue  (ch.sampleCrush, juce::dontSendNotification);  // "Crush" (repurposed)
    knobVolume.setValue (ch.volume,     juce::dontSendNotification);
    knobPan.setValue    (ch.pan,        juce::dontSendNotification);
    knobSlices.setValue (ch.slots[envTargetSlot()].smpSlices, juce::dontSendNotification);
    knobStretch.setValue(ch.slots[envTargetSlot()].smpStretch, juce::dontSendNotification);
    // Slices & Stretch only affect SAMPLE slots - grey them (knob + label) on any other engine.
    { const bool smp = (ch.slots[envTargetSlot()].engine == DrumChannel::SrcSample);
      knobSlices.setEnabled(smp);  knobSlices.setAlpha(smp ? 1.0f : 0.4f);  lblSlices.setAlpha(smp ? 1.0f : 0.4f);
      knobStretch.setEnabled(smp); knobStretch.setAlpha(smp ? 1.0f : 0.4f); lblStretch.setAlpha(smp ? 1.0f : 0.4f); }
    knobCutoff.setValue (ch.filterCutoff, juce::dontSendNotification);
    knobReso.setValue   (ch.filterReso,   juce::dontSendNotification);
    knobEnvAmt.setValue (ch.filterEnvAmt, juce::dontSendNotification);
    comboFilterType.setSelectedId(ch.filterType + 1, juce::dontSendNotification);
    comboOutput.setSelectedId    (ch.midiOut ? kMidiOutId : ch.outputBus + 1, juce::dontSendNotification);
    // Per-slot FX (Drive + Reverb/Delay send + LFO) for the selected slot:
    { auto& sl = ch.slots[envTargetSlot()];
      knobDrive.setValue (sl.fxDrive,       juce::dontSendNotification);
      comboDriveType.setSelectedId(sl.fxDriveType + 1, juce::dontSendNotification);
      updateFxFaders(sl);
      bool lfoRt[4]; lfoRoutedMask(sl, lfoRt);
        lfoDisplay.setValues(sl.lfoRate, sl.lfoAmt, sl.lfoSync, sl.lfoShape, sl.lfoFree, sl.lfoLegato, sl.lfoCurve, lfoRt, ((sl.filterType >= DrumChannel::LowPass && sl.filterType <= DrumChannel::Notch) || sl.filterType == DrumChannel::Bell || (sl.filterType2 >= DrumChannel::LowPass && sl.filterType2 <= DrumChannel::Notch) || sl.filterType2 == DrumChannel::Bell), (sl.oscShape >= DrumChannel::WvCustom || sl.engine == DrumChannel::SrcGrain),
                           envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8));
      modFaders.slotIdx = envTargetSlot();
      modFaders.accent = envTargetSlot() == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8);
      modFaders.setValues(sl);
      lfoDisplay.setModEnv(sl.modEnvA, sl.modEnvH, sl.modEnvD, sl.modEnvS, sl.modEnvR); }   // 12-fader matrix + Mod Env follow the selected slot
    refreshKeysPanel();   // the KEYS panel follows the selected channel/slot too

    // Double-click on any master knob returns it to its FACTORY DEFAULT (set once in setupKnob) -
    // which is exactly what a freshly-added VST / a fresh standalone shows (the standalone no longer
    // restores its last session; a SAVED DAW project still restores + displays its saved values).
    { auto& m = proc.masterFX();   // the reverb/delay rows show the SELECTED bus (masterBusB)
      knobReverbRoom.setValue (masterBusB ? m.reverbRoomB : m.reverbRoom,               juce::dontSendNotification);
      knobReverbDecay.setValue(1.0f - (masterBusB ? m.reverbDampB : m.reverbDamp),      juce::dontSendNotification);
      knobReverbWet.setValue  (masterBusB ? m.reverbWetB : m.reverbWet,                 juce::dontSendNotification);
      knobReverbPre.setValue  (masterBusB ? m.reverbPreDelayB : m.reverbPreDelay,       juce::dontSendNotification);
      knobReverbWidth.setValue(masterBusB ? m.reverbWidthB : m.reverbWidth,             juce::dontSendNotification);
      knobDelayTime.setValue  (masterBusB ? m.delayTimeB : m.delayTime,                 juce::dontSendNotification);
      knobDelayFB.setValue    (masterBusB ? m.delayFeedbackB : m.delayFeedback,         juce::dontSendNotification);
      knobDelayWet.setValue   (masterBusB ? m.delayWetB : m.delayWet,                   juce::dontSendNotification);
      swDelaySync.setToggleState(masterBusB ? m.delaySyncB : m.delaySync,               juce::dontSendNotification);
      swDelayPingPong.setToggleState(masterBusB ? m.delayPingPongB : m.delayPingPong,   juce::dontSendNotification);
      // [2026-07-15 02:30] the direct sync/trail/duck/character faders (bus-aware)
      swReverbSync.setToggleState(masterBusB ? m.reverbSyncB : m.reverbSync,            juce::dontSendNotification);
      delayBarNF.setValue01(((masterBusB ? m.delayBarNB : m.delayBarN) - 1.0f) / 20.0f);
      { const int t = masterBusB ? m.delayTrailB : m.delayTrail;
        delayTrailF.setValue01(t <= 0 ? 1.0f : (float) (t - 1) / 21.0f); }
      delayDuckF.setValue01(masterBusB ? m.delayDuckB : m.delayDuck);
      delayCharF.setValue01(masterBusB ? m.delayCharB : m.delayChar);
      // counted-unit inverses [2026-07-15 12:10]
      revDecBarsF.setValue01(((masterBusB ? m.reverbDecBarsB : m.reverbDecBars) * 12.0f - 3.0f) / 45.0f);
      revPreBarsF.setValue01((masterBusB ? m.reverbPreBarsB : m.reverbPreBars) / 0.125f);
      revGateF.setValue01((masterBusB ? m.reverbSyncB : m.reverbSync)
                              ? (masterBusB ? m.reverbGateB : m.reverbGate)
                              : (masterBusB ? m.reverbGateMsB : m.reverbGateMs) / 1000.0f);
      masterWidthF.setValue01(m.masterWidth / 1.5f);
      refreshMasterSyncFaders(); }
    knobDelayTime.updateText();
    knobMasterVol.setValue  (proc.masterFX().volume,            juce::dontSendNotification);
    knobMasterLimit.setValue(proc.masterFX().limit,             juce::dontSendNotification);
    knobMasterGlue.setValue (proc.masterFX().glue,              juce::dontSendNotification);
    knobMasterTilt.setValue (proc.masterFX().tilt,             juce::dontSendNotification);
    knobMasterSat.setValue  (proc.masterFX().sat,             juce::dontSendNotification);
    if (monoToggle.mono != proc.masterFX().mono) { monoToggle.mono = proc.masterFX().mono; monoToggle.repaint(); }
    // Sync the MASTER vertical faders from their (just-updated) proxy knobs.
    { LearnableKnob* mvk[NMVF] = { &knobMasterTilt, &knobMasterSat, &knobMasterGlue, &knobMasterLimit,
        &knobReverbRoom, &knobReverbDecay, &knobReverbWet, &knobReverbPre, &knobReverbWidth,
        &knobDelayTime, &knobDelayFB, &knobDelayWet };
      for (int i = 0; i < NMVF; ++i) masterVF[i].setValue01((float) mvk[i]->valueToProportionOfLength(mvk[i]->getValue())); }

    knobPEnvAmt.setValue  (ch.pitchEnvAmt,  juce::dontSendNotification);
    knobPEnvTime.setValue (ch.pitchEnvTime, juce::dontSendNotification);

    // Sounds section - the blend pad mirrors the channel's slots (one corner each,
    // duplicate engines numbered). Restore the saved dot without rebalancing.
    syncPadFromSlots(false);
    slotSelAmp.sel = slotSelPitch.sel = slotSelVoice.sel = juce::jlimit(1, 3, ch.envEditMode) - 1;  // per-channel shared slot
    slotSelAmp.repaint(); slotSelPitch.repaint(); slotSelVoice.repaint();
    loadEnvIntoEditor();   // amp env -> graph
    loadPitchAndVoice();   // pitch env + unison/detune/vibrato -> their controls
    knobLayOscShape.setValue (ch.layerOscShape,     juce::dontSendNotification);
    knobLaySineFreq.setValue (ch.layerSineFreq,     juce::dontSendNotification);
    knobLaySinePEA.setValue  (ch.layerSinePEnvAmt,  juce::dontSendNotification);
    knobLaySinePET.setValue  (ch.layerSinePEnvTime, juce::dontSendNotification);
    knobLaySinePOff.setValue (ch.layerSinePOffset,  juce::dontSendNotification);
    knobOscUnison.setValue   (ch.oscUnison,         juce::dontSendNotification);
    knobOscDetune.setValue   (ch.oscDetune,         juce::dontSendNotification);
    knobOscSustain.setValue  (ch.oscSustain,        juce::dontSendNotification);
    knobOscVib.setValue      (ch.oscVibrato,        juce::dontSendNotification);
    knobFmSustain.setValue   (ch.fmSustain,         juce::dontSendNotification);
    knobPhysSus.setValue     (ch.physSustain,       juce::dontSendNotification);
    knobPhysVib.setValue     (ch.physVibrato,       juce::dontSendNotification);
    knobNoiseSus.setValue    (ch.noiseSustain,      juce::dontSendNotification);
    knobPhysFreq.setValue    (ch.physFreq,          juce::dontSendNotification);
    knobPhysTone.setValue    (ch.physTone,          juce::dontSendNotification);
    knobPhysMat.setValue     (ch.physMaterial,      juce::dontSendNotification);
    knobPhysPEnv.setValue    (ch.physPitchEnvAmt,   juce::dontSendNotification);
    knobPhysPTime.setValue   (ch.physPitchEnvTime,  juce::dontSendNotification);
    knobPhysPOff.setValue    (ch.physPitchOffset,   juce::dontSendNotification);
    knobLayNoiseType.setValue(ch.noiseType,         juce::dontSendNotification);
    knobLayNoiseCtr.setValue (ch.layerNoiseCenter,  juce::dontSendNotification);
    knobLayNoiseWid.setValue (ch.layerNoiseWidth,   juce::dontSendNotification);
    knobFmPitch.setValue     (ch.fmPitch,           juce::dontSendNotification);
    knobFmSub.setValue       (ch.fmSub,             juce::dontSendNotification);
    knobFmSpread.setValue    (ch.fmSpread,          juce::dontSendNotification);
    knobFmDepth.setValue     (ch.fmDepth,           juce::dontSendNotification);
    knobFmPEnv.setValue      (ch.fmPitchEnvAmt,     juce::dontSendNotification);
    knobFmPTime.setValue     (ch.fmPitchEnvTime,    juce::dontSendNotification);
    knobFmPOff.setValue      (ch.fmPitchOffset,     juce::dontSendNotification);
    knobFmFeedback.setValue  (ch.fmFeedback,        juce::dontSendNotification);
    knobPhysPos.setValue     (ch.physPosition,      juce::dontSendNotification);
    knobSmpPOff.setValue     (ch.pitchOffset,       juce::dontSendNotification);
    knobSpeed.setValue(ch.pitch, juce::dontSendNotification);  // legacy channel pitch (no UI - see below)
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)   // each slot's own trim/reverse/region
    {
        const auto& sl = ch.slots[b];
        swSampleReverse[b].setToggleState(sl.smpReverse, juce::dontSendNotification);
        swSmpPreserve[b].setToggleState(sl.smpPreservePitch, juce::dontSendNotification);
        swUseRegion[b].setToggleState(sl.smpUseRegion, juce::dontSendNotification);
        waveform[b].setSelectionEnabled(sl.smpUseRegion);
        waveform[b].setRegions(sl.smpRegN, sl.smpRegLo, sl.smpRegHi);
        waveform[b].setReversed(sl.smpReverse);
    }
    cacheWaveform(selectedChannel);
    refreshSampleSel();
    updateSampleLengthLabel();

    ignoreKnobCallbacks = false;
}

// Colour-code each channel strip by routing (purple = MIDI Out, teal = aux out, dark = Main)
// + light up the Routing button when any channel is on MIDI. Routing is channel-wide.
void DrumSequencerEditor::refreshRouting()
{
    bool anyMidi = false;
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        auto& c = proc.sequencer.channel(i);
        juce::Colour col(0xff20203a), sel(0xff3355aa);   // (off, selected) colours
        if (c.midiOut)            { col = juce::Colour(0xff7a30d8); sel = juce::Colour(0xffa45cff); anyMidi = true; }  // purple = MIDI
        else if (c.outputBus > 0) { col = juce::Colour(0xff138a7a); sel = juce::Colour(0xff27c2a8); }                  // teal = aux out
        strips[i].numBtn.setColour(juce::TextButton::buttonColourId,   col);
        strips[i].numBtn.setColour(juce::TextButton::buttonOnColourId, sel);   // routing shows even when selected
        strips[i].numBtn.repaint();
    }
    btnRoute.setColour(juce::TextButton::buttonColourId,  anyMidi ? juce::Colour(0xff8a3df0) : juce::Colour(0xff20203a));
    btnRoute.setColour(juce::TextButton::textColourOffId, anyMidi ? juce::Colours::white : juce::Colours::lightgrey);
    btnRoute.repaint();
}

void DrumSequencerEditor::refreshChannelStrips()
{
    // MERGED GROUP: only offer step counts that FIT the 64-cell concat row (8 steps max for 8 bars,
    // 13 steps would break a 5-bar view -> disabled). Change-only (item churn fights open dropdowns).
    {
        auto& sq = proc.sequencer;
        const int bars = sq.groupEnd(currentPattern()) - sq.groupHead(currentPattern()) + 1;
        const int cap  = DrumChannel::MAX_STEPS / juce::jmax(1, bars);
        if (cap != lastStepCap)
        {
            lastStepCap = cap;
            for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
                for (int si = 0; si < DrumChannel::NUM_VALID_STEP_COUNTS; ++si)
                {
                    const int s = DrumChannel::VALID_STEP_COUNTS[si];
                    strips[i].comboSteps.setItemEnabled(s, s <= cap);
                }
        }
    }
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        strips[i].btnMute->setToggleState (proc.sequencer.channel(i).mute,        juce::dontSendNotification);
        strips[i].btnSolo->setToggleState (proc.sequencer.channel(i).solo,        juce::dontSendNotification);
        strips[i].btnPoly.setToggleState  (proc.sequencer.channel(i).allowOverlap, juce::dontSendNotification);
        // OVERLAP is a STEP concept (a step ringing into the next). In the piano roll each note has
        // its own length + poly is the keyboard Poly toggle, so it's inert here -> fade it (user review).
        { const bool roll = proc.sequencer.channel(i).drawMode;
          if (strips[i].btnPoly.isEnabled() == roll) { strips[i].btnPoly.setEnabled(! roll);
                                                       strips[i].btnPoly.setAlpha(roll ? 0.4f : 1.0f); } }
        { auto& cc = proc.sequencer.channel(i);
          const int tgt = cc.drawMode ? StepGridComponent::DRAW_ITEM_ID : cc.numSteps;
          // NEVER touch the combo while its popup is OPEN (that fought the user's selection = the
          // recurring "dropdown ignores my pick" bug) - structural guard, on top of the change-only
          // check, so it can't regress even if someone drops the change-only test later.
          if (! strips[i].comboSteps.isPopupActive() && strips[i].comboSteps.getSelectedId() != tgt)
              strips[i].comboSteps.setSelectedId(tgt, juce::dontSendNotification);
          const bool lockRoll = proc.sequencer.channel(i).mergeWith >= 0;   // merged pairs are PIANO-ROLL only
          if (strips[i].comboSteps.isEnabled() == lockRoll)
              strips[i].comboSteps.setEnabled(! lockRoll); }
        updateStripMixLabel(i);   // per-pattern sound-mix name (+ * if edited)
    }
    refreshRouting();   // recolour strips by MIDI/aux routing
    btnDawSync.setToggleState(proc.sequencer.dawSync,       juce::dontSendNotification);
    sliderSwing.setValue     (proc.sequencer.current().swing, juce::dontSendNotification);
    refreshSwingLabel();

    // DAW Sync ON -> BPM + time signature are locked and follow the host.
    const bool sync = proc.sequencer.dawSync;
    sliderBpm.setEnabled(!sync);
    if (barSigX.isEditable() == sync) { barSigX.setEditable(!sync); barSigY.setEditable(!sync); }
    const juce::Colour sigCol = sync ? juce::Colours::grey : juce::Colour(0xffffd24a);
    barSigX.setColour(juce::Label::textColourId, sigCol);
    barSigY.setColour(juce::Label::textColourId, sigCol);
    // Don't overwrite the text while the user is typing in it.
    if (sync)
    {
        sliderBpm.setValue(proc.currentBpm, juce::dontSendNotification);
        if (!barSigX.isBeingEdited()) barSigX.setText(juce::String(proc.currentTimeSigNum), juce::dontSendNotification);
        if (!barSigY.isBeingEdited()) barSigY.setText(juce::String(proc.currentTimeSigDen), juce::dontSendNotification);
    }
    else
    {
        sliderBpm.setValue(proc.sequencer.standaloneBpm, juce::dontSendNotification);
        if (!barSigX.isBeingEdited()) barSigX.setText(juce::String(barTimeSigX), juce::dontSendNotification);
        if (!barSigY.isBeingEdited()) barSigY.setText(juce::String(barTimeSigY), juce::dontSendNotification);
    }
    updateBarLength();
}

void DrumSequencerEditor::refreshPatternButtons()
{
    const int cur  = currentPattern();
    const int play = proc.sequencer.playPattern;
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        patternBtns[p].isCurrent = (p == cur);
        patternBtns[p].isPlaying = (p == play) && proc.sequencer.isCurrentlyPlaying;
        patternBtns[p].repaint();
    }
}

void DrumSequencerEditor::refreshFollowButton()
{
    const bool on = proc.followPlayback;
    btnFollow.setColour(juce::TextButton::buttonColourId, on ? juce::Colour(0xff35c0ff) : juce::Colour(0xff20203a));
    btnFollow.setColour(juce::TextButton::textColourOffId, on ? juce::Colours::black : juce::Colours::lightgrey);
    btnFollow.setButtonText(on ? "Follow: On" : "Follow: Off");
    btnFollow.repaint();
}

void DrumSequencerEditor::applyTooltipsSetting()
{
    tooltipWindow.gate = &tooltipsOn;   // OFF = getTipFor returns nothing (the delay trick overflowed - see the header note)
    btnTooltips.setColour(juce::TextButton::buttonColourId,  tooltipsOn ? juce::Colour(0xff35c0ff) : juce::Colour(0xff20203a));
    btnTooltips.setColour(juce::TextButton::textColourOffId, tooltipsOn ? juce::Colours::black : juce::Colours::lightgrey);
    btnTooltips.setButtonText(tooltipsOn ? "Tooltips" : "Tooltips: Off");
    btnTooltips.repaint();
}


void DrumSequencerEditor::refreshAuditionButton()
{
    const bool on = proc.auditionOnEdit.load();
    btnAudition.setColour(juce::TextButton::buttonColourId, on ? juce::Colour(0xff35c0ff) : juce::Colour(0xff20203a));
    btnAudition.setColour(juce::TextButton::textColourOffId, on ? juce::Colours::black : juce::Colours::lightgrey);
    btnAudition.setButtonText("Auto Test");
    btnAudition.repaint();
}

void DrumSequencerEditor::updateVisuals()
{
    refreshEqTarget();
}

// Point the FILTER display at the chosen SLOT (1 or 2). The "All" channel-EQ target was REMOVED
// (v1.3.5, user): the filter is PER SLOT now - it only touches that slot's own engine, and it's the
// only spectral tool (the static EQ is gone). eqEditTarget is 1 or 2 (slot index + 1).
void DrumSequencerEditor::refreshReverbModeHeader()
{
    static const char* mn[4] = { "ROOM", "HALL", "PLATE", "SHIMMER" };
    static const char* dn[5] = { "TAPE", "DIGITAL", "DUB", "ANALOG", "SHIMMER" };   // [2026-07-15] delay modes
    auto& m = proc.masterFX();
    const int mode  = juce::jlimit(0, 3, masterBusB ? m.reverbModeB : m.reverbMode);
    const int dMode = juce::jlimit(0, 4, masterBusB ? m.delayModeB  : m.delayMode);
    hdrReverb.setText(juce::String("REVERB ") + (masterBusB ? "B" : "A") + " - " + mn[mode], juce::dontSendNotification);
    hdrDelayG.setText(juce::String("DELAY ")  + (masterBusB ? "B" : "A") + " - " + dn[dMode], juce::dontSendNotification);
}

// [2026-07-15 02:30] Free proxy vs synced-variant faders (they share bounds; sync state picks which
// shows) + the Character dim on Digital. Called from layoutContent, refreshDetailPanel and the
// sync-toggle / mode-click handlers.
void DrumSequencerEditor::refreshMasterSyncFaders()
{
    auto& m = proc.masterFX();
    const bool vis = hdrDelayG.isVisible();          // the master column is on screen at all
    const bool dS  = masterBusB ? m.delaySyncB  : m.delaySync;
    const bool rS  = masterBusB ? m.reverbSyncB : m.reverbSync;
    masterVF[9].setVisible(vis && ! dS);  delayBarNF.setVisible(vis && dS);     // Time <-> echoes/bar
    masterVF[5].setVisible(vis && ! rS);  revDecBarsF.setVisible(vis && rS);    // Decay <-> bars
    masterVF[7].setVisible(vis && ! rS);  revPreBarsF.setVisible(vis && rS);    // Pre <-> fraction
    const bool charOn = juce::jlimit(0, 4, masterBusB ? m.delayModeB : m.delayMode) != 1;   // Digital = no flavour
    delayCharF.setAlpha(charOn ? 1.0f : 0.35f);
    delayCharF.setInterceptsMouseClicks(charOn, charOn);
}

void DrumSequencerEditor::refreshEqTarget()
{
    auto& ch = proc.sequencer.channel(selectedChannel);
    const int si = juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, eqEditTarget - 1);
    auto& fsl = ch.slots[si];
    freqDisplay.setFilters(fsl.filterType,  fsl.filterCutoff,  fsl.filterReso,  fsl.filterEnvAmt,  fsl.filterGain,
                           fsl.filterType2, fsl.filterCutoff2, fsl.filterReso2, fsl.filterEnvAmt2, fsl.filterGain2, proc.spectrumRate());
    freqDisplay.setFilterDrive(fsl.filterDrive);
    proc.analysisSlot.store(si);   // spectrum follows the selected slot
    slotSelEq.sel = si; slotSelEq.repaint();
}

void DrumSequencerEditor::timerCallback()
{
    ++timerCounter;   // 60 Hz; heavy refreshes are throttled to every 3rd tick (~20 Hz) below
    // HOST-FROZEN detector: if the audio callback stops (audio device off/missing/misconfigured,
    // or the host took the FX offline), the WHOLE plugin freezes - no sound, sequencer doesn't
    // move, TEST does nothing - and it looks broken. Watch the processBlock heartbeat and swap
    // the Play button's tooltip for a "Not playing?" explanation while frozen (~1 s of silence
    // from the host = frozen; one moving block = instantly healthy again).
    {
        const uint32_t hb = proc.processHeartbeat.load(std::memory_order_relaxed);
        if (hb != lastHeartbeat) { lastHeartbeat = hb; heartbeatStaleTicks = 0; }
        else if (heartbeatStaleTicks <= 80) ++heartbeatStaleTicks;   // timer runs at 60 Hz
        const bool frozen = heartbeatStaleTicks > 60;
        if (frozen != hostFrozen)
        {
            hostFrozen = frozen;
            btnPlay.setTooltip(frozen
                ? juce::String("NOT PLAYING? BASAMAK is not receiving audio from the host - everything is "
                               "paused (no sound, no sequencer movement, TEST does nothing).\n\n"
                               "- Almost always the AUDIO DEVICE: off, unplugged or misconfigured.\n"
                               "- In a DAW: check its audio device settings + that this FX is not offline/bypassed.\n"
                               "- Standalone: check Options > Audio Settings.\n\n"
                               "BASAMAK unfreezes by itself as soon as audio is back.")
                : juce::String("Start playback (used when DAW Sync is off, so the plugin runs on its own)."));
        }
    }

    // KEYS: 3s count-in, auto-finalise the take when the transport stops, the +/-24 reference
    // warning flash, and cheap UI refresh whenever the recording state / ref / take count moves.
    if (keysView)
    {
        if (keysCountdownTicks > 0)
        {
            if (--keysCountdownTicks == 0)
            {
                keysGoTicks = 42;                 // ~0.7 s "GO!" flash once the count-in ends
                proc.keysRecording.store(true);
                if (! proc.sequencer.isCurrentlyPlaying && ! proc.sequencer.dawSync)
                    proc.standalonePlay();
            }
        }
        else if (keysGoTicks > 0)
            --keysGoTicks;
        // Big half-transparent 3-2-1-GO! count-in overlay (change-only repaint).
        {
            juce::String cd;
            if (keysCountdownTicks > 0) cd = juce::String((keysCountdownTicks + 59) / 60);  // 3,2,1
            else if (keysGoTicks > 0)   cd = "GO!";
            if (countdownOverlay.label != cd) { countdownOverlay.label = cd; countdownOverlay.repaint(); }
        }
        updateKeyboardHighlight();   // light up the chord/scale/slot notes of the held key (cheap: change-gated)
        keysPanel.setSplitMark(keysView && detailShown
                               && proc.sequencer.channel(selectedChannel).mergeWith >= 0);
        if (++tunerTick >= 6)   // REAL TUNER, ~10 Hz: NSDF autocorrelation on the analysed channel's
        {                       // continuous ring (TunerTap) - measures the ACTUAL audio like
            tunerTick = 0;      // ReaTune/GTune (user demand), not knob arithmetic.
            juce::String tn; int cents = 0;
            constexpr int NR = TunerTap::N;
            static float snap[NR];
            const int w = proc.tunerTap.widx.load(std::memory_order_acquire);
            for (int i = 0; i < NR; ++i) snap[i] = proc.tunerTap.ring[(w + i) % NR];   // oldest..newest
            const double fs = proc.spectrumRate() / (double) TunerTap::DECIM;
            const int W = 2048;                        // analysis window = the newest 2048 samples
            const double f = basamakDetectPitch(snap + (NR - W), W, fs);   // the SHARED detector
            if (f > 0.0)                                                   // (TunerTest locks its accuracy)
            {
                const double m = 69.0 + 12.0 * std::log2(f / 440.0);
                const int    n = (int) std::lround(m);
                if (n >= 0 && n <= 127)
                {
                    cents = juce::jlimit(-50, 50, (int) std::lround((m - (double) n) * 100.0));
                    tn = juce::MidiMessage::getMidiNoteName(n, true, true, 4);
                }
            }
            if (tn.isNotEmpty() || tunerSilence++ > 6)   // hold the last reading briefly through gaps
            {
                if (tn.isNotEmpty()) tunerSilence = 0;
                if (tn != keysPanel.tunerBox.note || cents != keysPanel.tunerBox.cents)
                { keysPanel.tunerBox.note = tn; keysPanel.tunerBox.cents = cents; keysPanel.tunerBox.repaint(); }
            }
        }
        if (proc.keysRecording.load())
        {
            parseKeysEvents();   // live: each finished loop becomes a take while you keep playing
            drainDrawTake();     // draw mode: each finished loop's lane -> a take
            if (proc.sequencer.isCurrentlyPlaying) keysRecWasPlaying = true;
            else if (keysRecWasPlaying) keysStopRecord(true);   // transport stopped -> take is done
        }
        const int uiHash = (proc.keysRecording.load() ? 1 : 0) + (keysCountdownTicks > 0 ? 2 : 0)
                         + (int) proc.keysTakes.size() * 1024
                         + (int)(proc.keyQHead.load(std::memory_order_relaxed) & 0xffff) * 32768;   // key presses bump the ring head
        if (uiHash != keysUiHash)
        {
            keysUiHash = uiHash;
            refreshKeysPanel();
            slotEd[0].pushValues(); slotEd[1].pushValues();   // key touch re-tunes Freq to C3 -> show it
        }
    }

    // Close any open dropdown / popup menu when the user clicks OUTSIDE the plugin - into the
    // host's own UI or another application. JUCE auto-dismisses only for clicks inside OUR
    // windows, so a combo menu left open while the user switched to the DAW hung around.
    // Signals: the app deactivated (clicked another app), or a popup is open but no window of
    // ours has OS focus (clicked the host window in the same process). Debounced 2 ticks
    // (~80 ms) so focus flicker while a menu is opening can never dismiss it.
    // The sound picker is a CONTENT CHILD (never modal), so it gets the same treatment - the
    // old gate on getNumModalComponents() > 0 meant the focus check NEVER RAN with only the
    // picker open = "doesn't close when I click outside the plugin" (round-2 report).
    const bool pickerOpen = soundPicker != nullptr && soundPicker->isVisible();
    const bool tipsOpen   = fxTypeList.isVisible();   // the per-item-tooltip type dropdown joins the watchdog
    if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0 || pickerOpen || tipsOpen)
    {
        bool anyFocused = juce::Process::isForegroundProcess();
        if (anyFocused)
        {
            anyFocused = false;
            if (auto* ownPeer = getPeer(); ownPeer != nullptr && ownPeer->isFocused()) anyFocused = true;
            auto& desktop = juce::Desktop::getInstance();
            for (int i = desktop.getNumComponents(); ! anyFocused && --i >= 0;)
                if (auto* dc = desktop.getComponent(i))
                    if (auto* peer = dc->getPeer())
                        if (peer->isFocused()) anyFocused = true;
        }
        // GRACE on menu TRANSITIONS: when one menu closes and another opens in the same gesture
        // (the step-count -> quantize-confirm chain), OS focus is briefly nowhere - without this
        // the fresh menu was dismissed ~100 ms in ("clicks don't register" on step counts).
        auto* mm = juce::ModalComponentManager::getInstance();
        const int mc = mm->getNumModalComponents();
        auto* mtop = mm->getModalComponent(0);   // identity only - a swapped menu can keep count 1
        if (mc != lastModalCount || mtop != lastModalComp)
        { lastModalCount = mc; lastModalComp = mtop; outsideFocusTicks = -30; }   // ~0.5 s grace
        if (! anyFocused && ++outsideFocusTicks >= 6)
        {   // ~100ms at 60Hz (was 2 = 33ms, dismissed dropdowns as they opened)
            outsideFocusTicks = 0;
            juce::PopupMenu::dismissAllActiveMenus();
            if (pickerOpen) static_cast<SoundPickerPanel&>(*soundPicker).close();
            if (tipsOpen)   fxTypeList.close();
        }
        else if (anyFocused) outsideFocusTicks = 0;
    }
    else { outsideFocusTicks = 0; lastModalCount = 0; lastModalComp = nullptr; }

    // The playing pattern auto-advanced (or a MIDI CC switched it). Follow it only if
    // "Follow" is on; otherwise just keep the green playing-marker in sync.
    if (proc.patternChangedByMidi.exchange(false)
        || proc.sequencer.patternChanged.exchange(false))
    {
        if (proc.followPlayback) selectPattern(proc.sequencer.playPattern);
        else                     refreshPatternButtons();
    }
    // Keep the playing-marker current when playback starts/stops (no patternChanged then).
    {
        const bool nowPlaying = proc.sequencer.isCurrentlyPlaying;
        const int  nowPlayPat = proc.sequencer.playPattern;
        if (nowPlaying != lastPlayingState || nowPlayPat != lastPlayPattern)
        {
            lastPlayingState = nowPlaying; lastPlayPattern = nowPlayPat;
            if (proc.followPlayback && nowPlaying && nowPlayPat != currentPattern()) selectPattern(nowPlayPat);
            else refreshPatternButtons();
        }
    }

    // --- Level meters: read the PLAYING pattern's per-channel peaks + master peak, dB-scale,
    //     apply ballistics (instant attack, smooth release, ~0.7s peak-hold), push to the meters.
    {
        auto toDisp = [](float lin) -> float {
            if (lin < 1.0e-5f) return 0.0f;
            const float db = 20.0f * std::log10(lin);
            return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);   // -60..0 dBFS -> 0..1
        };
        auto ballistic = [](float d, float& val, float& pk, int& hold) {
            if (d >= val) val = d; else val += (d - val) * 0.35f;          // fast up, slow down
            if (d >= pk) { pk = d; hold = 0; }
            else if (++hold > 16) pk += (d - pk) * 0.25f;                  // hold then fall
        };
        const int pp = juce::jlimit(0, Sequencer::NUM_PATTERNS - 1, proc.sequencer.playPattern);
        auto& pat = proc.sequencer.patterns[pp];
        for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        {
            const float raw = pat.channels[i].meterPeak.load(std::memory_order_relaxed);
            pat.channels[i].meterPeak.store(0.0f, std::memory_order_relaxed);   // consume the held peak (peak-hold reset)
            const float d = toDisp(raw);
            ballistic(d, meterVal[i], meterPk[i], meterHold[i]);
            stripMeter[i].setLevel(meterVal[i], meterPk[i]);
            stripMeter[i].setVol(proc.sequencer.channel(i).volume);   // the volume handle follows CC/state
        }
        const float dm[2] = { toDisp(proc.masterMeterL.load(std::memory_order_relaxed)),
                              toDisp(proc.masterMeterR.load(std::memory_order_relaxed)) };
        proc.masterMeterL.store(0.0f, std::memory_order_relaxed);   // consume the held peaks
        proc.masterMeterR.store(0.0f, std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
        {
            ballistic(dm[c], mMeterVal[c], mMeterPk[c], mMeterHold[c]);
        }
        logoMeter.setLevels(mMeterVal[0], mMeterVal[1]);   // live stereo volume in the logo ramp (L | R)
    }

    // MIDI-learn highlight: repaint when learning starts / moves to another
    // control / finishes, so the amber ring appears and clears promptly.
    {
        bool nowLearn = proc.midiLearn.isLearning();
        juce::String lp = proc.midiLearn.getLearningParam();
        if (nowLearn != lastLearnActive || lp != lastLearnParam)
        {
            lastLearnActive = nowLearn;
            lastLearnParam  = lp;
            // Repaint each control DIRECTLY - knobs are buffered-to-image, so a
            // parent content.repaint() would just blit their stale cache.
            for (auto* k : allKnobs) k->repaint();
            for (auto& s : strips)
            {
                s.btnPoly.repaint(); s.btnInfluence.repaint();
                if (s.btnMute) s.btnMute->repaint();
                if (s.btnSolo) s.btnSolo->repaint();
            }
            btnModeVel.repaint();  btnModeLen.repaint();  btnModePitch.repaint();
            btnModeProb.repaint(); btnModeRoll.repaint(); btnModePan.repaint();
            btnModeNudge.repaint(); btnModeModA.repaint(); btnModeModB.repaint();
            stepGrid.repaint();    // steps are learnable too
            content.repaint();
        }
    }

    // UI-only controls assigned to MIDI: apply the parked request (see routeCC).
    if (int m = proc.uiMidiEditMode.exchange(-1); m >= 1)
        setStepEditMode(stepGrid.editMode == m ? 0 : m);   // toggle, mirroring a button click
    if (int ic = proc.uiMidiInfluence.exchange(-1); ic >= 0 && ic < Sequencer::NUM_CHANNELS)
    {
        bool ns = ! stepGrid.influenceArmed[ic];
        stepGrid.influenceArmed[ic] = ns;
        if (ic == selectedChannel) btnInfluenceTop.setToggleState(ns, juce::dontSendNotification);
    }
    // MIDI sound browsing (NEXT/PREV): at most ONE step per ~220 ms; excess presses are
    // DROPPED, never queued - the moment the hand stops, the browsing stops (no overshoot).
    if (int t = proc.uiMidiSoundStep.exchange(0); t != 0)
    {
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        if (now - lastSoundStepMs >= 220) { lastSoundStepMs = now; stepSoundBank(t > 0 ? 1 : -1); }
    }
    // NEXT/PREV hold-to-repeat: a held pad keeps stepping at the browse rate. Starts after
    // ~0.45 s so a tap stays ONE step; a 15 s cap stops a stuck toggle-mode pad from scrolling
    // forever (set pads to MOMENTARY in the controller's editor).
    if (int hd = proc.uiSoundHold.load(); hd != 0)
    {
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        const juce::uint32 t0  = proc.uiSoundHoldMs.load();
        if      (now - t0 > 15000) proc.uiSoundHold.store(0);
        else if (now - t0 > 450 && now - lastSoundStepMs >= 220)
        { lastSoundStepMs = now; stepSoundBank(hd); }
    }
    {   // SELECTED-SCOPE MIDI controls (ui_sel_*): apply queued CCs to the current selection
        // through the same paths the on-screen controls use (undo + the modified-* follow).
        bool slotDirty = false, keysDirty = false;
        int tail = proc.selQTail.load(std::memory_order_relaxed);
        const int head = proc.selQHead.load(std::memory_order_acquire);
        while (tail != head)
        {
            const auto ev = proc.selQ[tail]; tail = (tail + 1) & 63;
            applySelCC(ev.t, ev.v, slotDirty, keysDirty);
        }
        proc.selQTail.store(tail, std::memory_order_release);
        if (proc.uiMasterCcDirty.exchange(false)) slotDirty = true;   // master CC landed: refresh knobs
        if (slotDirty) refreshDetailPanel();
        if (keysDirty) refreshKeysPanel();
    }

    stepGrid.update(proc.sequencer, proc.anySolo);      // 60 Hz: smooth playhead
    // One bar in ms at the current tempo - the Nudge cells read out real milliseconds.
    stepGrid.barMs = 60000.0 / juce::jmax(1.0, proc.currentBpm)
                   * (juce::jmax(1, proc.currentTimeSigNum) * 4.0 / juce::jmax(1, proc.currentTimeSigDen));
    if (timerCounter % 3 == 0) refreshChannelStrips();  // 20 Hz: strips/combo/labels (was hammering the UI at 60 Hz -> laggy dropdowns)
    updateVisuals();

    // MIDI-in monitor: flash green + show the last CC whenever MIDI arrives.
    {
        // [2026-07-15 02:30] the Decay fader's "~2.1 s" is COMPUTED from Size/Mode/tempo - repaint it
        // ~4x/sec so the figure follows those live even while the Decay handle isn't touched.
        if (++masterEstTick >= 16)
        { masterEstTick = 0; if (masterVF[5].isVisible()) masterVF[5].repaint(); }
        if (++sysStatTicks >= 30)   // [2026-07-14 01:50] ~2x/sec: CPU (plugin | machine) + RAM
        {
            sysStatTicks = 0;
            const int plug = juce::roundToInt(proc.loadMeasurer.getLoadAsProportion() * 100.0);
            const float sys = systemCpuPercent(), ram = processRamMB();
            lblCpu.setText("CPU " + juce::String(juce::jlimit(0, 999, plug)) + "%"
                           + (sys >= 0.0f ? " | " + juce::String(juce::roundToInt(sys)) + "%" : juce::String()),
                           juce::dontSendNotification);
            lblRam.setText(ram >= 0.0f ? "RAM " + juce::String(juce::roundToInt(ram)) + " MB" : juce::String("RAM --"),
                           juce::dontSendNotification);
        }
        uint32_t mc = proc.midiInCount.load(std::memory_order_relaxed);
        if (mc != lastMidiInSeen) { lastMidiInSeen = mc; midiFlash = 8; }
        int cn = proc.lastCcNum.load(std::memory_order_relaxed);
        juce::String t = "MIDI: ";
        const juce::uint32 atMs = proc.lastAtMs.load(std::memory_order_relaxed);
        if (atMs != 0 && juce::Time::getMillisecondCounter() - atMs < 1500)   // fresh AFTERTOUCH wins; expires ~1.5 s
            t << "AT v" << proc.lastAtVal.load(std::memory_order_relaxed)
              << " ch" << proc.lastAtChan.load(std::memory_order_relaxed);
        else if (cn >= 0)
            t << "cc" << cn << " v" << proc.lastCcVal.load(std::memory_order_relaxed)
              << " ch" << proc.lastCcChan.load(std::memory_order_relaxed);
        else
            t << (mc > 0 ? "in (notes)" : "waiting");
        lblMidiIn.setText(t, juce::dontSendNotification);
        lblMidiIn.setColour(juce::Label::textColourId,
                            midiFlash > 0 ? juce::Colour(0xff44ff66) : juce::Colour(0xff556688));
        if (midiFlash > 0) --midiFlash;
    }

    // Undo history + "modified" detection hash the WHOLE project, so run them at ~20 Hz (every 3rd
    // of the 60 Hz ticks), not on every frame - the heavy per-tick hashing was dropping the playhead
    // to a stutter while recording. The playhead/grid/meters still update at the full 60 Hz below.
    // (per-action snapshots: settled + no mouse button held -> one undo step; a drag = one step.)
    if (! applyingUndo && ! proc.keysRecording.load() && timerCounter % 3 == 0)
    {
        syncMergedGroupSounds();   // merged patterns mirror the edited pattern's sounds (change-only)
        juce::int64 h = stateHash();
        undoTickHash = h;   // reuse for the "modified since saved" check below (avoid a 2nd full hash)
        const bool mouseHeld = juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown();
        if (undoStack.empty()) { pushUndoSnapshot(); lastUndoHash = h; }   // baseline
        else
        {
            if (h != lastUndoHash) { lastUndoHash = h; undoDirty = true; undoStableTicks = 0; }
            if (undoDirty && ! mouseHeld && ++undoStableTicks >= 2) { pushUndoSnapshot(); undoDirty = false; }
        }
        // Detect "modified since saved" for the * markers (same throttle - it reuses undoTickHash).
        auto& sc = proc.sequencer.channel(selectedChannel);
        if (!sc.mixModified && channelSoundHash(sc) != sc.mixHash)
        { sc.mixModified = true; updateStripMixLabel(selectedChannel); }
        if (!presetModified && undoTickHash != presetBaselineHash)
        { presetModified = true; updatePresetLabel(); }
    }

    // Live spectrum: if the audio thread handed us a block, FFT it here.
    // Bins are mapped onto the same 20 Hz-20 kHz log axis as the EQ curve.
    if (proc.spectrumTap.ready.load(std::memory_order_acquire))
    {
        constexpr int N = SpectrumTap::fftSize;
        juce::zeromem(fftBuf, sizeof(fftBuf));
        std::memcpy(fftBuf, proc.spectrumTap.data, sizeof(float) * N);
        proc.spectrumTap.ready.store(false, std::memory_order_release);

        fftWindow.multiplyWithWindowingTable(fftBuf, N);
        fft.performFrequencyOnlyForwardTransform(fftBuf);

        const double sr = proc.spectrumRate() > 0 ? proc.spectrumRate() : 44100.0;
        const float mindB = -90.0f, maxdB = 0.0f;
        const float ref = juce::Decibels::gainToDecibels((float)N);
        for (int i = 0; i < FrequencyDisplay::scopeSize; ++i)
        {
            float prop = (float)i / (float)(FrequencyDisplay::scopeSize - 1);
            double freq = FrequencyDisplay::normToFreq(prop);       // 20..20000 Hz (skewed axis)
            int idx = juce::jlimit(0, N / 2, (int)(freq / (sr * 0.5) * (N / 2)));
            float dB = juce::Decibels::gainToDecibels(fftBuf[idx]) - ref;
            scopeData[i] = juce::jmap(juce::jlimit(mindB, maxdB, dB), mindB, maxdB, 0.0f, 1.0f);
        }
        freqDisplay.pushSpectrum(scopeData, FrequencyDisplay::scopeSize);
    }

    freqDisplay.decayTick(); // smooth fade so transients stay visible
    updateBarLength();       // keep the bar-length read-out in sync with tempo

    // Live envelope playheads: where each playing voice on the selected channel is.
    float heads[8]; int nh = proc.sequencer.channel(selectedChannel).activeVoiceTimes(heads, 8);
    envEditor.setPlayheads(heads, nh);
    pitchEditor.setPlayheads(heads, nh);
    // Live sample playhead on the visible waveforms (the real read position, not an animation).
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
        if (waveform[b].isVisible())
            waveform[b].setPlayhead(proc.sequencer.channel(selectedChannel).getSamplePlayheadFrac(b));
    // Keep the pitch X-axis = the sound's current length live (so editing the amp envelope -
    // or the sample trim - rescales the pitch time immediately and the playhead stays synced).
    pitchEditor.setLengthSec(pitchEnvLenSec(envTargetSlot()));
    // Live LFO dot: the REAL phase of the newest playing voice on the selected channel/slot.
    lfoDisplay.setPhase(proc.sequencer.channel(selectedChannel).getLfoPhase(envTargetSlot(), lfoDisplay.selDest()));
    lfoDisplay.setLiveCycle(proc.sequencer.channel(selectedChannel).getLfoCycle(envTargetSlot(), lfoDisplay.selDest()));
    // MOD RINGS: the FX knobs show a cyan arc at the live modulated value (Vital-style; the knob
    // itself stays at your base setting). Fed from the DSP's per-block modLiveFx snapshot.
    {
        const auto& mc = proc.sequencer.channel(selectedChannel);
        const int ms = envTargetSlot();
        // A slot with NO active routes clears its rings IMMEDIATELY (the DSP snapshot only updates while
        // the channel renders, so a new preset would otherwise leave a stale blue ring - user bug).
        auto liveFx = [&mc](int s, int i) -> float
        { return mc.slots[s].modActive() ? mc.slotModLiveFx[s][i] : -1000.0f; };
        struct RK { LearnableKnob* k; int idx; };
        const RK rk[6] = { { &knobSub, 3 }, { &knobPunch, 4 }, { &knobFormant, 5 }, { &knobRing, 20 },
                           { &knobRingHz, 25 }, { &knobSlotPan, 26 } };
        for (auto& r : rk)
        { const float raw = liveFx(ms, r.idx); r.k->setModRing(raw < -900.0f ? -1.0f : (float) r.k->valueToProportionOfLength(raw)); }
        // CHANNEL FX faders: rung from the channel-level live values (both slots' routes feed them).
        {   // CHANNEL FX + send faders: [A-Amt, A-Chr, B-Amt, B-Chr, RevSend, DelSend, C-Amt, C-Chr]
            SlotDragFader* cf[8] = { &chFxAmtF[0], &chFxChrF[0], &chFxAmtF[1], &chFxChrF[1],
                                     &sendRevF, &sendDelF, &chFxAmtF[2], &chFxChrF[2] };
            for (int i = 0; i < 8; ++i)
            { const float raw = mc.chFxLive[i]; cf[i]->setModRing(raw < -900.0f ? -1.0f : juce::jlimit(0.0f, 1.0f, raw)); }
        }
        const float drv = liveFx(ms, 6);   // Drive is a fader (0..1)
        fxDriveFader.setModRing(drv < -900.0f ? -1.0f : juce::jlimit(0.0f, 1.0f, drv));
        freqDisplay.setModCutoff(0, liveFx(ms, 7) < -900.0f ? -1.0f : liveFx(ms, 7));   // filter 1/2 cutoff (Hz)
        freqDisplay.setModCutoff(1, liveFx(ms, 8) < -900.0f ? -1.0f : liveFx(ms, 8));
        freqDisplay.setModReso  (0, liveFx(ms, 27)); freqDisplay.setModReso  (1, liveFx(ms, 28));   // [2026-07-14] live reso + env-amount
        freqDisplay.setModEnvAmt(0, liveFx(ms, 29)); freqDisplay.setModEnvAmt(1, liveFx(ms, 30));
        // GRID knobs (the engine's own params - FM Amount, Ratio, ...): ring both slot boxes from [9..16].
        // Osc slots also ring the Warp fader + push the modulated FM/Warp into the WAVE PREVIEW so the
        // drawing itself moves with the modulation (user: "changing the sound wave too").
        for (int s = 0; s < DrumChannel::NUM_SLOTS; ++s)
        {
            float grid[DrumChannel::MOD_TGT_GRID];
            for (int i = 0; i < DrumChannel::MOD_TGT_GRID; ++i) grid[i] = liveFx(s, 9 + i);
            slotEd[s].setGridModRings(grid, DrumChannel::MOD_TGT_GRID);
            slotEd[s].setOscModLive(liveFx(s, 17), liveFx(s, 9), liveFx(s, 10));
        }
        // UNISON visual: rings on detune/vib/width/drift when a route targets them (-1000 -> no ring).
        auto uf = [&](int i) { const float r = liveFx(ms, i); return r < -900.0f ? -1.0f : r; };
        voiceMod.setModLive(uf(21), uf(22), uf(23), uf(24), liveFx(ms, 31));   // + Unison Count ring
    }
    {   // DRIFT visual honesty: push the last hit's REAL rolled detunes into the unison view
        auto& dc = proc.sequencer.channel(selectedChannel);
        float dcents[DrumChannel::UNI_MAX + 1];
        voiceMod.setDriftLive(dcents, dc.getDriftSnapshot(envTargetSlot(), dcents, DrumChannel::UNI_MAX + 1));
        // FREE-RUN LFOs: the continuous clock the white dot rides between notes
        lfoDisplay.setFreeClockSec(dc.lfoBarPos >= 0.0 ? dc.lfoBarPos * (double) dc.lfoBarSeconds
                                                       : dc.lfoFreeSec);
        // LIVE wavetable position (amber marker on the draw window's Position strip)
        if (harmEd.isVisible())
            harmEd.setLivePos(dc.getWtPos(harmEd.slotIdx));
        // LIVE grain markers on the source preview (real grain positions, honest-visual rule)
        for (int b2 = 0; b2 < DrumChannel::NUM_SLOTS; ++b2)
        {
            auto& mv = slotEd[b2].morphView;
            if (! mv.isVisible()) continue;
            if (dc.slots[b2].engine == DrumChannel::SrcGrain)
            {
                float gp[DrumChannel::GRAINS_MAX];
                const int n2 = dc.getGrainSnapshot(b2, gp, DrumChannel::GRAINS_MAX);
                mv.setGrainLive(gp, n2, dc.slotSample[b2].buf.getNumSamples() > 64
                                          ? "Sample: " + dc.slotSample[b2].file.getFileName()
                                          : juce::String());
            }
            else if (mv.grainN >= 0) mv.setGrainLive(nullptr, -1, {});
        }
    }
    // live tempo so the wave/read-out always show the TRUE synced speed (change-gated)
    lfoDisplay.setTempoInfo((float) proc.sequencer.blockBarSeconds);
    { // keep the LFO's "filter is off" warning correct regardless of how the filter was toggled (change-gated = cheap)
        auto& lsl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        auto fOn = [](int t){ return (t >= DrumChannel::LowPass && t <= DrumChannel::Notch) || t == DrumChannel::Bell; };
        lfoDisplay.setFilterOn(fOn(lsl.filterType) || fOn(lsl.filterType2));
    }
}

//==============================================================================
void DrumSequencerEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void DrumSequencerEditor::resized()
{
    const float DH = (float) contentHeightPx;
    float s = juce::jmin((float)getWidth()  / (float)DESIGN_W,
                         (float)getHeight() / DH);
    float offX = ((float)getWidth()  - DESIGN_W * s) * 0.5f;
    float offY = ((float)getHeight() - DH * s) * 0.5f;

    content.setBounds(0, 0, DESIGN_W, contentHeightPx);
    content.setTransform(juce::AffineTransform::scale(s).translated(offX, offY));
    if (zoomed) positionZoomPanel();
}

// Margin of solid panel background around the re-parented group.
static constexpr int kZoomMargin = 8;

void DrumSequencerEditor::positionZoomPanel()
{
    const float s = juce::jmin((float) getWidth()  / (float) DESIGN_W,
                               (float) getHeight() / (float) contentHeightPx);
    const float offX = ((float) getWidth()  - DESIGN_W * s) * 0.5f;
    const float offY = ((float) getHeight() - contentHeightPx * s) * 0.5f;
    const float Z = 2.4f;                         // 2.4x the group's normal on-screen size

    const int   m = kZoomMargin;
    const int   localW = zoomRect.getWidth()  + 2 * m;
    const int   localH = zoomRect.getHeight() + 2 * m;
    zoomPanel.setBounds(0, 0, localW, localH);

    const float onW = localW * s * Z, onH = localH * s * Z;
    const float gcx = (float) zoomRect.getCentreX() * s + offX;  // group's normal centre on screen
    const float gcy = (float) zoomRect.getCentreY() * s + offY;
    const float px = juce::jlimit(8.0f, juce::jmax(8.0f, getWidth()  - onW - 8.0f), gcx - onW * 0.5f);
    const float py = juce::jlimit(8.0f, juce::jmax(8.0f, getHeight() - onH - 8.0f), gcy - onH * 0.5f);
    zoomPanel.setTransform(juce::AffineTransform::scale(s * Z).translated(px, py));

    zoomCatcher.setBounds(getLocalBounds());
    zoomCatcher.toFront(false);
    zoomPanel.toFront(false);
    zoomCloseBtn.setBounds((int) (px + onW) - 60, (int) py + 4, 54, 22);
    zoomCloseBtn.toFront(false);
}

void DrumSequencerEditor::zoomToGroup(juce::Rectangle<int> designRect)
{
    if (zoomed) unzoom();
    zoomRect = designRect;

    // Close the floating overlays FIRST (same rule as layoutContent): they park over multiple
    // columns, so the lift below would otherwise drag them into the zoom - and addAndMakeVisible
    // would force-SHOW even a hidden one (user: "wavetable menu in the background / modulation
    // list covering the screen" when magnifying).
    if (soundPicker != nullptr && soundPicker->isVisible())
        static_cast<SoundPickerPanel&>(*soundPicker).close();
    harmEd.setVisible(false);
    lfoCurveEd.setVisible(false);
    routePicker.setVisible(false);
    remapEd.setVisible(false);
    fxTypeList.close();   // anchored under a combo that just moved (close() frees the combo latch)

    // Lift every control whose centre is inside the group box into the panel,
    // re-positioned relative to the group (the panel's transform enlarges them).
    const auto origin = designRect.getTopLeft();
    juce::Array<juce::Component*> toMove;
    for (auto* c : content.getChildren())
    {
        bool skip = ! c->isVisible();   // NEVER lift hidden components (addAndMakeVisible would show them)
        for (auto& zb : zoomBtns) if (c == &zb) skip = true;       // keep "+" buttons in place
        for (auto& fo : srcFade)  if (c == &fo) skip = true;       // and the fade overlays
        if (! skip && designRect.contains(c->getBounds().getCentre()))
            toMove.add(c);
    }
    for (auto* c : toMove)
    {
        auto rel = c->getBounds().translated(-origin.x + kZoomMargin, -origin.y + kZoomMargin);
        zoomPanel.addAndMakeVisible(c);
        c->setBounds(rel);
        zoomMoved.add(c);
    }

    zoomed = true;
    keysPanel.setVisible(false);   // the KEYS keyboard must not show behind/through the zoom (unzoom's layoutContent restores it)
    zoomCatcher.setVisible(true);
    zoomPanel.setVisible(true);
    zoomCloseBtn.setVisible(true);
    positionZoomPanel();
}

void DrumSequencerEditor::unzoom()
{
    for (auto* c : zoomMoved) content.addAndMakeVisible(c);  // re-parent back
    zoomMoved.clear();
    zoomed = false;
    zoomCatcher.setVisible(false);
    zoomPanel.setVisible(false);
    zoomCloseBtn.setVisible(false);
    layoutContent();                                        // restore original positions
}

//==============================================================================
static constexpr int TOP_H      = 40;
static constexpr int PAT_Y      = 44;
static constexpr int PAT_H      = 34;
static constexpr int GRID_TOP   = 84;
static constexpr int ROW_H      = 44;   // taller step rows (drag-lock removed the mis-click concern)
static constexpr int STRIP_W    = 414;   // wider so the sound-mix dropdown fits ~1.5x
// The detail panel's top Y is computed at layout time = GRID_TOP + visibleChannels*ROW_H + 24,
// so the panel sits just below however many channel rows are currently shown.

// Drawn ABOVE the content's children (paintOverChildren): the strip METER sits right on the row's
// bottom edge and was covering a background-drawn outline's bottom line.
void DrumSequencerEditor::paintStripOutline(juce::Graphics& g)
{
    if (soundPicker != nullptr && soundPicker->isVisible())   // the picker dropdown stays ON TOP
        g.excludeClipRegion(soundPicker->getBounds());        // (paintOverChildren covers children)
    const float x0 = channelBar.isVisible() ? 17.0f : 2.0f;   // start AFTER the yellow scrollbar (x 2..14)
    // MERGED CHANNEL PAIRS: ONE clean rounded VIOLET box around the two paired rows - the same visual
    // language as the amber pattern-merge box (no busy/crossing lines, user's OCD note). Per pattern;
    // pairs are adjacent channels. Inset INSIDE the red selected outline so the two never sit on top
    // of each other (concentric with a gap when a paired row is also selected). Drawn regardless of
    // which channel is selected (must show even when the selected channel is scrolled off-view).
    {
        auto& sq = proc.sequencer;
        g.setColour(juce::Colour(0xffb46bff));   // merge violet (matches the C4 split marker + Merge UI)
        for (int c = 0; c + 1 < Sequencer::NUM_CHANNELS; ++c)
        {
            if (sq.channel(c).mergeWith != c + 1) continue;   // draw once, from the LOWER channel of the pair
            const int rA = c - firstChannelRow, rB = rA + 1;
            if (rB < 0 || rA >= viewRows()) continue;         // pair fully scrolled out of view
            const int top = GRID_TOP + juce::jmax(0, rA) * ROW_H;
            const int bot = GRID_TOP + juce::jmin(viewRows(), rB + 1) * ROW_H;
            g.drawRoundedRectangle(juce::Rectangle<float>(x0 + 3.0f, (float) top + 2.0f,
                                   (float) STRIP_W - 9.0f - x0, (float) (bot - top) - 4.0f), 6.0f, 2.0f);
        }
    }
    const int selRow = selectedChannel - firstChannelRow;
    if (selRow < 0 || selRow >= viewRows()) return;           // selected channel scrolled off -> no red outline
    g.setColour(juce::Colour(0xffff3b30));
    g.drawRoundedRectangle(juce::Rectangle<float>(x0, (float) (GRID_TOP + selRow * ROW_H) + 1.0f,
                                                  (float) STRIP_W - 3.0f - x0, (float) ROW_H - 1.5f), 5.0f, 2.0f);   // bottom edge BELOW the level meter
}

void DrumSequencerEditor::paintContent(juce::Graphics& g)
{
    const int nCh = viewRows();               // rows currently in the viewport (<= 8)
    const int detailY = GRID_TOP + nCh * ROW_H + 24;
    const int gridRightP = DESIGN_W - 12;     // step grid now spans the full width

    g.fillAll(juce::Colour(0xff14142a));

    // ---- Brand logo (top-left): the BASAMAK vector badge + version ------
    {
        // Wordmark only now (the ascending steps were cropped out of the SVG); the live volume meter
        // (logoMeter) sits just below it. viewBox is the wordmark bounds (~6.875:1).
        const juce::Rectangle<float> logoRect(8.0f, 1.0f, 150.0f, 22.0f);
        if (logoDrawable != nullptr)
            logoDrawable->drawWithin(g, logoRect, juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid, 1.0f);
        // version is now the clickable `verLink` HyperlinkButton (set up in setupComponents / positioned in layoutContent)
    }
    // (The Channels/Patterns count toggles moved to the pattern row, by the loop dropdown - no top-bar boxes now.)

    // Detail panel: 5 full-height columns, edge to edge (no split). Keep in sync with layoutContent.
    const int colH = (detailY + 360) - (detailY + 22);
    const int slotVGap = 6, slotH = (colH - slotVGap) / 2;

    g.setColour(juce::Colour(0xff2a2a4a));
    g.drawHorizontalLine(TOP_H, 0.0f, (float)DESIGN_W);
    g.drawHorizontalLine(GRID_TOP - 4, 0.0f, (float)DESIGN_W);
    g.drawHorizontalLine(detailY - 6, 0.0f, (float)DESIGN_W);
    g.drawVerticalLine(STRIP_W, (float)GRID_TOP, (float)(GRID_TOP + nCh * ROW_H));

    // MERGED pattern groups: one amber box around each run of glued pattern buttons.
    {
        auto& sq = proc.sequencer;
        for (int p = 0; p < proc.visiblePatterns; )
        {
            const int end = sq.groupEnd(p);
            if (end > p)
            {
                juce::Rectangle<int> uni;
                for (int m = p; m <= end && m < Sequencer::NUM_PATTERNS; ++m)
                    if (patternBtns[m].isVisible())
                        uni = uni.isEmpty() ? patternBtns[m].getBounds() : uni.getUnion(patternBtns[m].getBounds());
                if (! uni.isEmpty())
                {
                    g.setColour(juce::Colour(0xffd9c46a));
                    g.drawRoundedRectangle(uni.expanded(3).toFloat(), 6.0f, 1.6f);
                }
            }
            p = end + 1;
        }
    }

    g.setColour(juce::Colour(0x18ffffff));
    const int selRow = selectedChannel - firstChannelRow;        // selection highlight at its on-screen row
    if (selRow >= 0 && selRow < nCh)
    {
        g.fillRect(0, GRID_TOP + selRow * ROW_H, gridRightP, ROW_H);
        // (The strip's red outline is drawn in ContentComponent::paintOverChildren - the strip METER
        //  child sits on the row's bottom edge and painted over a background-drawn outline.)
    }

    if (! detailShown) return;   // editing panel collapsed -> draw nothing below the grid

    // Outline each knob group so they're easy to tell apart.
    g.setColour(juce::Colour(0xff353560));
    auto box = [&](float bx, float by, float bw, float bh) {
        g.drawRoundedRectangle({ bx, by, bw, bh }, 4.0f, 1.0f);
    };
    auto boxGroup = [&](const juce::Component& hdr, float h) {
        if (hdr.getWidth() <= 0) return;
        auto b = hdr.getBounds().toFloat();
        box(b.getX() - 3.0f, b.getY() - 3.0f, b.getWidth() + 6.0f, h);
    };
    // Full-height columns: AMP ENV+EQ | PITCH ENV | MODULATION. The FX column is SPLIT: the per-slot FX
    // box on top + the CHANNEL FX box (whole instrument) under it.
    boxGroup(hdrAmpEnv, (float) colH); boxGroup(hdrModulation, (float) colH);
    boxGroup(hdrSend, (float) FX_BOX_H); boxGroup(hdrChannelFx, (float) CHFX_BOX_H);
    // PITCH box outline: from the column top (the "PITCH ENVELOPE" title) down the full column height.
    if (hdrPitch.getWidth() > 0) {
        auto pb = hdrPitch.getBounds().toFloat();
        box(pb.getX() - 3.0f, (float) (detailY + 22) - 3.0f, pb.getWidth() + 6.0f, (float) colH);
    }
    // Two stacked HALF-height slot boxes on the left edge (hdrSamplerG = slot 1, hdrOscG = slot 2).
    // Subtle tints so you can tell them apart (Slot 1 = yellow, Slot 2 = pink) - kept faint so knobs stay readable.
    const int selSlot = envTargetSlot();              // which slot the 1/2 control selectors point at
    auto tintBox = [&](const juce::Component& hdr, juce::Colour c) {
        if (hdr.getWidth() <= 0) return;
        auto b = hdr.getBounds().toFloat();
        g.setColour(c);
        g.fillRoundedRectangle(b.getX() - 3.0f, b.getY() - 3.0f, b.getWidth() + 6.0f, (float) slotH, 4.0f);
    };
    // Slot 1 = yellow family, Slot 2 = pink family. The SELECTED slot is BRIGHTER + gets an accent border
    // so the whole group reads as "active"; the other is dim/near-neutral. (Knobs carry the slot colour too.)
    tintBox(hdrSamplerG, selSlot == 0 ? juce::Colour(0xff45391b) : juce::Colour(0xff232017));
    tintBox(hdrOscG,     selSlot == 1 ? juce::Colour(0xff43203a) : juce::Colour(0xff231820));
    g.setColour(juce::Colour(0xff353560));
    boxGroup(hdrSamplerG, (float) slotH); boxGroup(hdrOscG, (float) slotH);
    auto accentBorder = [&](const juce::Component& hdr, juce::Colour c) {
        if (hdr.getWidth() <= 0) return;
        auto b = hdr.getBounds().toFloat();
        g.setColour(c);
        g.drawRoundedRectangle(juce::Rectangle<float>(b.getX() - 3.0f, b.getY() - 3.0f, b.getWidth() + 6.0f, (float) slotH).reduced(0.5f), 4.0f, 2.2f);
    };
    if (selSlot == 0)      accentBorder(hdrSamplerG, juce::Colour(0xffe8bf4d));
    else if (selSlot == 1) accentBorder(hdrOscG,     juce::Colour(0xffe86aa8));
    // MASTER box (hdrSounds, right edge) - distinct: amber tint + a bolder amber border (it's GLOBAL).
    if (hdrSounds.getWidth() > 0) {
        auto b = hdrSounds.getBounds().toFloat();
        juce::Rectangle<float> mb(b.getX() - 3.0f, b.getY() - 3.0f, b.getWidth() + 6.0f, (float) colH);
        g.setColour(juce::Colour(0x18ffc24a)); g.fillRoundedRectangle(mb, 4.0f);
        g.setColour(juce::Colour(0xffe8947a)); g.drawRoundedRectangle(mb.reduced(0.5f), 4.0f, 1.8f);   // blend orange [2026-07-15]
        g.setColour(juce::Colour(0xff353560));
    }
}

void DrumSequencerEditor::setVisibleChannels(int n)
{
    n = (n <= 8) ? 8 : 16;   // VIEW rows preference; all 16 channels stay active + scrollable either way
    visibleChannels      = n;
    contentHeightPx      = contentHeightFor(n, detailShown);
    proc.visibleChannels = n;
    firstChannelRow      = juce::jlimit(0, juce::jmax(0, Sequencer::NUM_CHANNELS - viewRows()), firstChannelRow);
    stepGrid.visibleRows = viewRows();
    stepGrid.firstRow    = firstChannelRow;
    channelBar.setRangeLimits(0.0, (double) Sequencer::NUM_CHANNELS, juce::dontSendNotification);
    channelBar.setCurrentRange((double) firstChannelRow, (double) viewRows(), juce::dontSendNotification);
    refreshCountButtons();
    setResizeLimits(DESIGN_W / 2, contentHeightPx / 2, DESIGN_W * 2, contentHeightPx * 2);
    const double s = juce::jmax(0.1, (double) getWidth() / (double) DESIGN_W);  // keep the current width-scale
    layoutContent();
    setSize(getWidth(), juce::roundToInt(contentHeightPx * s));   // adjust height -> triggers resized()
    repaint();
}

void DrumSequencerEditor::setNumPatterns(int n)
{
    n = Sequencer::NUM_PATTERNS;   // always 32 now (kept for the ctor call path)
    visiblePatterns  = n;
    proc.visiblePatterns = n;
    firstPatternCol  = juce::jlimit(0, juce::jmax(0, n - patShown()), firstPatternCol);
    patternBar.setRangeLimits(0.0, (double) n, juce::dontSendNotification);
    patternBar.setCurrentRange((double) firstPatternCol, (double) patShown(), juce::dontSendNotification);
    refreshCountButtons();
    layoutContent();
    refreshPatternButtons();
    content.repaint();
}

void DrumSequencerEditor::refreshCountButtons()
{
    // Just the 16-CHANNELS-VIEW toggle's face now (counts are fixed at 16 ch / 32 patterns).
    const bool on = visibleChannels == 16;
    btn16View.setButtonText(on ? "8 CHANNELS VIEW" : "16 CHANNELS VIEW");
    btn16View.setColour(juce::TextButton::buttonColourId, on ? juce::Colour(0xffe8bf4d) : juce::Colour(0xff20203a));
    btn16View.setColour(juce::TextButton::textColourOffId, on ? juce::Colours::black : juce::Colours::lightgrey);
    btn16View.repaint();
}

// Mouse WHEEL over the channel strips scrolls the channel window; over the pattern row it walks
// the pattern window sideways (same clamps as the scrollbars; the bars stay the visual truth).
void ContentComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    owner.contentWheel(e.getEventRelativeTo(this).getPosition(), w.deltaY != 0.0f ? w.deltaY : -w.deltaX);
}

void DrumSequencerEditor::contentWheel(juce::Point<int> pos, float deltaY)
{
    if (deltaY == 0.0f) return;
    // Rate-limit: a wheel/trackpad can fire many events per physical notch - without this each event
    // moved a whole row, so the list rocketed. ~70 ms min between steps = smooth, still responsive.
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastContentWheelMs < 70.0) return;
    lastContentWheelMs = now;
    const int dir = deltaY < 0.0f ? 1 : -1;
    const int vr  = viewRows();
    if (pos.x < STRIP_W && pos.y >= GRID_TOP && pos.y < GRID_TOP + vr * ROW_H)   // channel strips
    {
        const int fr = juce::jlimit(0, juce::jmax(0, Sequencer::NUM_CHANNELS - vr), firstChannelRow + dir);
        if (fr != firstChannelRow) channelBar.setCurrentRange((double) fr, (double) vr, juce::sendNotificationSync);
        return;
    }
    if (pos.y >= PAT_Y && pos.y < PAT_Y + 40)                                    // pattern row
    {
        const int fc = juce::jlimit(0, juce::jmax(0, visiblePatterns - patShown()), firstPatternCol + dir);
        if (fc != firstPatternCol) patternBar.setCurrentRange((double) fc, (double) patShown(), juce::sendNotificationSync);
    }
}

void DrumSequencerEditor::scrollBarMoved(juce::ScrollBar* sb, double newRangeStart)
{
    if (sb == &patternBar)
    {
        const int fc = juce::jlimit(0, juce::jmax(0, visiblePatterns - patShown()), (int) (newRangeStart + 0.5));
        if (fc == firstPatternCol) return;
        firstPatternCol = fc;
        layoutContent();
        content.repaint();
        return;
    }
    if (sb != &channelBar) return;
    const int fr = juce::jlimit(0, juce::jmax(0, Sequencer::NUM_CHANNELS - viewRows()), (int) (newRangeStart + 0.5));
    if (fr == firstChannelRow) return;
    firstChannelRow   = fr;
    stepGrid.firstRow = fr;
    layoutContent();   // reposition the strips into the new window
    content.repaint();
}

void DrumSequencerEditor::layoutContent()
{
    // A view change (KEYS toggle, 8/16 rows, hide editor, resize) must never leave the sound
    // picker floating over the NEW layout (user screenshot: "it all just scrambled").
    // [2026-07-15 22:10] EXCEPT when the relayout was caused by a SOUND PICK - browsing (MIDI
    // next/prev or the picker itself) must keep the picker open while the slot boxes re-place.
    if (! keepPickerOnLayout && soundPicker != nullptr && soundPicker->isVisible())
        static_cast<SoundPickerPanel&>(*soundPicker).close();
    harmEd.setVisible(false);   // same rule for the DRAW HARMONICS overlay
    lfoCurveEd.setVisible(false);   // and the LFO SHAPER overlay
    routePicker.setVisible(false);  // and the route source|target picker
    remapEd.setVisible(false);      // and its REMAP curve overlay
    fxTypeList.close();             // and the per-item-tooltip type dropdown
    const int W = DESIGN_W;
    const int gridLeft = STRIP_W + 4;
    const int gridW = W - gridLeft - 12;            // step grid now spans the full width
    // (Master FX/Output used to sit to the right of the grid; they now live in the
    //  detail panel's first row, so the grid reclaims that space - longer steps.)

    // Top toolbar (logo + version are painted in paintContent; leave room for them)
    titleLabel.setBounds  (0, 0, 0, 0); // logo painted directly, not a label
    verLink.setBounds     (150, 2, 56, 40);    // TALL click area covering the whole stack (one link)
    lblVersion.setBounds  (150, 3, 56, 14);    // v1.3.0  - centred
    lblCheckUpd.setBounds (150, 18, 56, 22);   // Check / Updates - centred on the SAME axis (tidy stack)
    btnDawSync.setBounds  (210, 7, 72,  26);
    btnPlay.setBounds     (288, 7, 30,  26);   // ▶ icon
    btnStop.setBounds     (320, 7, 30,  26);   // ■ icon
    // BPM + time signature kept TOGETHER.
    lblBpm.setBounds      (358, 8, 30,  24);
    sliderBpm.setBounds   (388, 7, 92,  26);
    lblBarPre.setBounds   (486, 0, 56,  10);   // "TIME SIG" label ABOVE the numbers
    barSigX.setBounds     (492, 12, 20, 21);
    lblBarSlash.setBounds (512, 12, 8,  21);
    barSigY.setBounds     (520, 12, 20, 21);
    lblBarResult.setBounds(546, 8,  66, 24);   // bar length (seconds)
    lblPreset.setBounds   (0, 0, 0, 0);          // empty caption removed - the combo's "Presets" placeholder is the label
    comboPreset.setBounds (614, 7, 152, 26);     // wider so "Presets" never clips (Windows renders fonts wider than macOS)
    btnRoute.setBounds    (772, 7, 80,  26);   // routing dropdown (▼ drawn by DropButtonLNF)
    btnUndo.setBounds     (858, 7, 28,  26);   // ↶ icon
    btnRedo.setBounds     (888, 7, 28,  26);   // ↷ icon
    lblMidiIn.setBounds   (922, 8, 92, 24);
    comboMidi.setBounds(1018, 7, 72, 26);         // ends at 1090; "MIDI" is short, so this leaves an 8px gap
    btnAudition.setBounds (1104, 7, 76, 26);      // "Auto Test" (right-side group spread into the old Keys button's gap)
    // Where the Channels/Patterns count boxes used to be: Tooltips toggle + the (global) Follow toggle.
    btnTooltips.setBounds(1188, 7, 74, 26);
    btnFollow.setBounds  (1270, 7, 74, 26);   // moved up from the pattern row (it's a GLOBAL setting)
    dragMidi.setBounds    (W - 156, 7, 84, 26);    // "DRAG MIDI" (shortened; the CPU/RAM readout sits right of it)
    lblCpu.setVisible(true); lblCpu.setBounds(W - 68, 4, 62, 15);
    lblRam.setVisible(true); lblRam.setBounds(W - 68, 20, 62, 15);

    // Pattern row: a window of the pattern buttons (16 visible; 24/32 scroll via patternBar).
    lblPatterns.setBounds(6, PAT_Y + 2, 60, 15);
    lblPatternsBars.setBounds(6, PAT_Y + 17, 60, 15);   // "(Bars)" under "Patterns", same size/colour
    {
        const int px0 = 70, pw = 33, pg = 3, step = pw + pg, shown = patShown();
        firstPatternCol = juce::jlimit(0, juce::jmax(0, visiblePatterns - shown), firstPatternCol);
        for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p) {
            const int col = p - firstPatternCol;
            const bool vis = (p < visiblePatterns) && col >= 0 && col < shown;
            patternBtns[p].setVisible(vis);
            if (vis) patternBtns[p].setBounds(px0 + col * step, PAT_Y, pw, PAT_H);
        }
        const bool patScroll = visiblePatterns > shown;
        patternBar.setVisible(patScroll);
        if (patScroll) {
            patternBar.setBounds(px0, PAT_Y + PAT_H, shown * step - pg, 7);   // bright bar just below the patterns
            patternBar.setRangeLimits(0.0, (double) visiblePatterns, juce::dontSendNotification);
            patternBar.setCurrentRange((double) firstPatternCol, (double) shown, juce::dontSendNotification);
        }
    }
    patModeBtn.setBounds(664, PAT_Y + 8, 160, 26);   // shortened to make room for the wider edit-mode buttons (user)
    // Channel-count (8/16) + pattern-count (16/32) toggles, right next to the loop dropdown (Follow moved to the top bar).

    sliderSwing.setBounds(836, PAT_Y + 3, 92, 20);    // shifted left (Loop dropdown shortened) so the edit buttons fit
    lblSwing.setBounds   (836, PAT_Y + 24, 92, 12);   // ...live caption under it, e.g. Swing 66%
    // Step edit-mode radio buttons (+ the two STEP MOD lanes), then Influence, then Clear (flush right).
    // Widths sized so the full word fits (no "G..." / "L..." truncation - user).
    lblEditMode.setBounds (942,  PAT_Y + 8, 28, 24);   // "Edit:"
    btnModeVel.setBounds  (974,  PAT_Y + 8, 32, 24);   // (Slide has no button - it lives in Pitch mode's bottom band)
    btnModeLen.setBounds  (1010, PAT_Y + 8, 40, 24);
    btnModePitch.setBounds(1054, PAT_Y + 8, 42, 24);
    btnModeProb.setBounds (1100, PAT_Y + 8, 40, 24);
    btnModeRoll.setBounds (1144, PAT_Y + 8, 36, 24);
    btnModePan.setBounds  (1184, PAT_Y + 8, 34, 24);
    btnModeNudge.setBounds(1222, PAT_Y + 8, 46, 24);
    btnModeModA.setBounds (1272, PAT_Y + 8, 48, 24);   // STEP MOD lanes (drawable modulation sources)
    btnModeModB.setBounds (1324, PAT_Y + 8, 48, 24);
    btnInfluenceTop.setBounds(1384, PAT_Y + 8, 44, 24);// purple-outlined; left of Clear
    btnClearPat.setBounds (1434, PAT_Y + 8, 70, 24);   // Clear - flush near the right edge

    // Channel strips:  [#] [sound ▸ sub-menu] [M] [S] [Ø] [steps]
    // Only the channels in the scroll window [firstChannelRow, +viewRows) are shown, mapped to on-screen
    // rows. The rest are hidden (the engine still runs them). When scrolling is active a scrollbar sits at
    // the far left, so the strips shift right by `sbPad` to make room for it.
    juce::ignoreUnused(W);
    const int vr     = viewRows();
    firstChannelRow  = juce::jlimit(0, juce::jmax(0, Sequencer::NUM_CHANNELS - vr), firstChannelRow);  // keep valid as the view grows/shrinks
    const bool canScroll = Sequencer::NUM_CHANNELS > vr;
    const int sbPad  = canScroll ? 16 : 0;
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        const int rrow = i - firstChannelRow;                 // on-screen row of this channel
        const bool vis = rrow >= 0 && rrow < vr;
        auto& st = strips[i];
        st.numBtn.setVisible(vis);  st.comboSound.setVisible(vis);  st.btnTest.setVisible(vis);
        st.btnMute->setVisible(vis); st.btnSolo->setVisible(vis);   st.btnPoly.setVisible(vis);
        st.comboSteps.setVisible(vis); stripMeter[i].setVisible(vis);
        if (! vis) continue;
        int y = GRID_TOP + rrow * ROW_H;
        st.numBtn.setBounds      (sbPad + 4,   y + 8, 20, 24);
        st.comboSound.setBounds  (sbPad + 26,  y + 7, 144, 26); // Sound mixes
        st.btnTest.setBounds     (sbPad + 174, y + 8, 42, 24);
        st.btnMute->setBounds    (sbPad + 220, y + 8, 23, 24);
        st.btnSolo->setBounds    (sbPad + 245, y + 8, 23, 24);
        st.btnPoly.setBounds     (sbPad + 270, y + 8, 26, 24);
        st.comboSteps.setBounds  (sbPad + 300, y + 7, 108 - sbPad, 26);   // wider now Influence moved to the top bar (text was clipped)
        stripMeter[i].setBounds  (sbPad + 27,  y + ROW_H - 9, 382 - sbPad, 8);  // level bar + VOLUME handle (sits in the row gap, clear of the combos above)
    }
    channelBar.setVisible(canScroll);
    if (canScroll) {
        channelBar.setBounds(2, GRID_TOP, 12, vr * ROW_H);
        channelBar.setRangeLimits(0.0, (double) Sequencer::NUM_CHANNELS, juce::dontSendNotification);
        channelBar.setCurrentRange((double) firstChannelRow, (double) vr, juce::dontSendNotification);
    }

    // Step grid
    stepGrid.rowH = ROW_H;
    stepGrid.visibleRows = vr;
    stepGrid.firstRow    = firstChannelRow;
    stepGrid.setBounds(gridLeft, GRID_TOP, gridW, vr * ROW_H);
    // Magnifier overlay covers the whole content canvas (so a magnified first-row/-column cell can
    // spill over the top bar + channel strips) and stays front-most + mouse-transparent.
    stepMagOverlay.setBounds(content.getLocalBounds());
    stepMagOverlay.toFront(false);
    countdownOverlay.setBounds(content.getLocalBounds());
    countdownOverlay.toFront(false);   // count-in sits above even the magnifier

    // ---- Bigger knob helpers (readable) -------------------------------------
    const int kg = 4, boxH = 16, hdrH = 15;
    const int knobS = 56, comboW = 92;
    int x = 12, curHy = 0, curKy = 0;

    auto group = [&](juce::Label& hdr, int n) {
        hdr.setBounds(x, curHy, n * knobS + (n - 1) * kg, hdrH);
    };
    auto knob = [&](LearnableKnob& k, juce::Label& l) {
        k.setBounds(x, curKy, knobS, knobS + boxH);
        l.setBounds(x - 2, curKy + knobS + boxH, knobS + 4, 14);
        x += knobS + kg;
    };
    auto combo = [&](juce::ComboBox& c, juce::Label& l) {
        c.setBounds(x, curKy + (knobS + boxH - 26) / 2, comboW, 26);
        l.setBounds(x, curKy + knobS + boxH, comboW, 14);
        x += comboW + kg;
    };
    auto groupW = [&](int n) { return n * knobS + (n - 1) * kg; };

    // =====================================================================
    // DETAIL PANEL
    //   Row 1 : sound-select+pad | SAMPLE | ANALOG | NOISE | FM | MASTER FX/OUT
    //   Row 2 : Channel EQ (+spectrum) | Channel FX | Channel Filter | Channel
    //   Every source group has its own per-source AHD envelope knobs.
    // =====================================================================
    const int detailY = GRID_TOP + viewRows() * ROW_H + 24;   // detail panel sits below the viewport (<= 8 rows)
    // Title-strip buttons: raised a couple px (clear of the box outlines below) + wide enough for the full UPPERCASE
    // text (no "..." truncation).
    btnToggleDetail.setBounds(DESIGN_W - 200, detailY - 2, 190, 18);   // collapse/expand (always visible)
    btnKeysView.setBounds(DESIGN_W - 200 - 116, detailY - 2, 110, 18); // KEYS <-> SOUND EDITOR (radio)
    btn16View.setBounds(DESIGN_W - 200 - 116 - 146, detailY - 2, 140, 18);   // 8 <-> 16 channel-row VIEW
    // [2026-07-15 19:45] OTHERS trim + VOL RESET, left of the view toggle (user placement)
    othersVolF.setBounds(DESIGN_W - 200 - 116 - 146 - 150, detailY - 2, 144, 18);
    btnVolReset.setBounds(DESIGN_W - 200 - 116 - 146 - 150 - 92, detailY - 2, 86, 18);
    btnKeysView.setVisible(detailShown);
    lblSelected.setVisible(detailShown); btnSaveMix.setVisible(detailShown);
    lblSelected.setBounds(12, detailY - 2, 200, 18);
    btnSaveMix.setBounds(214, detailY - 2, 172, 18);
    // NOTE: we DON'T early-return when collapsed. Letting the detail layout run repositions every editor component
    // relative to the (now much lower) detailY, so they land below the short collapsed window and get CLIPPED -
    // which truly hides them. (Early-returning left them at stale positions overlapping the expanded 12/16-row grid.)


    // ---- ROW 1 : SOUNDS + five sound-source groups, justified edge-to-edge ----
    // Every source group uses two BALANCED knob rows (keeps each box narrow so
    // labels stay inside it). Column count = the wider of its two rows.
    const int r1 = detailY + 22;

    // ---- DETAIL PANEL: 5 full-height columns, edge to edge (no more 55/45 split). --------------------
    //   L -> R:  SLOTS (stacked) + vertical BLEND | AMP ENV + EQ | PITCH ENV | FX | MASTER (right edge)
    const int colTop = r1;                              // detailY + 22
    const int colBot = detailY + 360;
    const int colH   = colBot - colTop;                 // full column height
    const int gp     = 10;                              // gap between columns
    // Column widths (user 2026-07-12): the FIVE right boxes (amp/filter | pitch/unison | FX |
    // MODULATION | MASTER) all share ONE width now; only the left SLOTS column keeps its own size.
    const int slotsColW = 376;
    // [12 | slots 376 | gp | 5 x colW5 with a gp between each | 12] = DESIGN_W(1510) -> colW5 = 212, edges symmetric.
    const int colW5 = (DESIGN_W - 12 - slotsColW - 5 * gp - 12) / 5;   // = 212
    const int ampEqW = colW5, pitchW = colW5, fxColW = colW5, modW = colW5, masterW = colW5;
    const int cxSlots  = 12;
    const int cxAmp    = cxSlots + slotsColW + gp;
    const int cxPitch  = cxAmp   + ampEqW   + gp;
    const int cxFx     = cxPitch + pitchW   + gp;
    const int cxMod    = cxFx    + fxColW   + gp;        // MODULATION group
    const int cxMaster = cxMod   + modW     + gp;        // right edge
    // Slots: two half-height boxes stacked on the LEFT edge; a vertical blend fader to their right.
    const int slotBoxW = slotsColW - 50;                // leave a wide column for the blend fader + % read-outs
    const int slotVGap = 6;
    const int slotH    = (colH - slotVGap) / 2;
    const int slotW    = slotBoxW;                      // SlotEditor width
    const int sbx[DrumChannel::NUM_SLOTS] = { cxSlots, cxSlots };
    const int sby[DrumChannel::NUM_SLOTS] = { colTop, colTop + slotH + slotVGap };
    {
        // ===== MASTER box (RIGHT edge, amber) = a NARROW 3/5-width strip. Volume stays a horizontal
        //       fader; every knob is a VERTICAL drag-fader (value inside, name in the Label below).
        //       The 12 proxy knobs are hidden (they stay the model). =====
        int sx = cxMaster;
        hdrSounds.setBounds(sx, colTop, masterW, hdrH);   // "MASTER"
        for (int i = 0; i < 5; ++i) { srcSwitch[i].setBounds(0, 0, 0, 0); lblSrc[i].setBounds(0, 0, 0, 0); }
        btnPadLayout.setBounds(0, 0, 0, 0); lblPadHint.setBounds(0, 0, 0, 0);
        soundPad.setBounds(0, 0, 0, 0);
        hdrMasterOut.setBounds(0, 0, 0, 0); hdrMasterOut.setVisible(false);
        { LearnableKnob* hk[NMVF] = { &knobMasterTilt, &knobMasterSat, &knobMasterGlue, &knobMasterLimit,
            &knobReverbRoom, &knobReverbDecay, &knobReverbWet, &knobReverbPre, &knobReverbWidth,
            &knobDelayTime, &knobDelayFB, &knobDelayWet };
          for (auto* k : hk) { k->setVisible(false); k->setBounds(0, 0, 0, 0); } }   // hidden model behind the faders
        // Master volume: horizontal fader across the top (kept horizontal - user).
        knobMasterVol.setBounds(sx + 8, colTop + 22, masterW - 16, 16); lblMasterVol.setBounds(0, 0, 0, 0);
        // vertical-fader placer: fader + a centred Label below it. Spread across the (now wider) box.
        const int FH = 64;   // fader height - all 3 rows + labels fit INSIDE the box (colH 338)
        // Row 1: Tilt / Sat / Glue / Limit / Width + Mono toggle (col 5) - six slots like the
        // other rows [2026-07-15 12:10]; Width = the new bass-safe master stereo width.
        { const int fy = colTop + 44;
          const int FP6a = (masterW - 16) / 6, FW6a = FP6a - 6;
          auto vfa = [&](juce::Component& c, juce::Label& lbl, int col) {
              const int fx = sx + 8 + col * FP6a;
              c.setVisible(true); c.setBounds(fx, fy, FW6a, FH);
              lbl.setVisible(true); lbl.setJustificationType(juce::Justification::centred);
              lbl.setFont(juce::Font(9.5f, juce::Font::bold)); lbl.setMinimumHorizontalScale(0.62f);
              lbl.setBounds(fx - 4, fy + FH + 1, FW6a + 8, 11);
          };
          vfa(masterVF[0], lblMasterTilt, 0); vfa(masterVF[1], lblMasterSat, 1);
          vfa(masterVF[2], lblMasterGlue, 2); vfa(masterVF[3], lblMasterLimit, 3);
          vfa(masterWidthF, lblMasterWidth, 4); lblMasterWidth.setText("Width", juce::dontSendNotification);
          const int mx = sx + 8 + 5 * FP6a;
          monoToggle.setVisible(true); monoToggle.setBounds(mx - 5, fy + 4, FW6a + 10, 38); }
        // [2026-07-15 02:30] Rows 2+3 = SIX slots each now (user layout): Wet moved to a PILL on the
        // title line, freeing slots for Gate (reverb) and Trail/Duck/Character (delay). The synced
        // variants (Decay/Pre in bars, Time in echoes-per-bar) share bounds with their free proxies;
        // refreshMasterSyncFaders() picks which one shows.
        const int FP6 = (masterW - 16) / 6, FW6 = FP6 - 6;
        auto vf6 = [&](juce::Component& c, juce::Label& lbl, int col, int fy) {
            const int fx = sx + 8 + col * FP6;
            c.setVisible(true); c.setBounds(fx, fy, FW6, FH);
            lbl.setVisible(true); lbl.setJustificationType(juce::Justification::centred);
            lbl.setFont(juce::Font(9.5f, juce::Font::bold)); lbl.setMinimumHorizontalScale(0.62f);
            lbl.setBounds(fx - 4, fy + FH + 1, FW6 + 8, 11);
        };
        // Row 2: REVERB - Size / Decay(~s | bars) / Pre(ms | fraction) / Width / Gate + [Sync]; Wet = pill
        { hdrReverb.setVisible(true); refreshReverbModeHeader();
          hdrReverb.setBounds(sx + 6, colTop + 126, masterW - 86, hdrH);
          masterVF[6].setVertical(false); masterVF[6].setVisible(true);            // the Wet PILL (same proxy)
          masterVF[6].setBounds(sx + masterW - 78, colTop + 127, 70, 15);
          lblRevWet.setVisible(false); lblRevWet.setBounds(0, 0, 0, 0);
          // Order pairs the delay row's jobs by COLUMN [2026-07-15 12:10]: col1 = the space
          // (Size ~m | Time /bar), col2 = the length (Decay | Feedback), col3 = the hard stop
          // (Gate | Trail). Cols 4-5 = each effect's own personality (Pre/Width | Duck/Char).
          const int fy = colTop + 144;
          vf6(masterVF[4], lblRevRoom,  0, fy);
          vf6(masterVF[5], lblRevDecay, 1, fy); revDecBarsF.setBounds(masterVF[5].getBounds());
          vf6(revGateF,    lblRevGate,  2, fy); lblRevGate.setText("Gate", juce::dontSendNotification);
          vf6(masterVF[7], lblRevPre,   3, fy); revPreBarsF.setBounds(masterVF[7].getBounds());
          vf6(masterVF[8], lblRevWidth, 4, fy);
          lblRevSyncT.setVisible(false); lblRevSyncT.setBounds(0, 0, 0, 0);   // the lit button carries its own word now
          const int cx5 = sx + 8 + 5 * FP6;
          // full remaining width so "Sync" never truncates [2026-07-15 13:30]
          swReverbSync.setVisible(true); swReverbSync.setBounds(cx5, fy + 20, sx + masterW - 8 - cx5, 22); }
        // Row 3: DELAY - Time(ms | /bar) / Trail / Feedback / Duck / Character + [Sync over Ping]; Wet = pill
        { hdrDelayG.setVisible(true);   // text ("DELAY A - TAPE") comes from refreshReverbModeHeader - never overwrite it here
          hdrDelayG.setBounds(sx + 6, colTop + 230, masterW - 86, hdrH);
          masterVF[11].setVertical(false); masterVF[11].setVisible(true);          // the Wet PILL (same proxy)
          masterVF[11].setBounds(sx + masterW - 78, colTop + 231, 70, 15);
          lblDelWet.setVisible(false); lblDelWet.setBounds(0, 0, 0, 0);
          const int fy = colTop + 248;
          vf6(masterVF[9],  lblDelTime,  0, fy); delayBarNF.setBounds(masterVF[9].getBounds());
          vf6(masterVF[10], lblDelFB,    1, fy); lblDelFB.setText("F.back", juce::dontSendNotification);
          vf6(delayTrailF,  lblDelTrail, 2, fy); lblDelTrail.setText("Trail", juce::dontSendNotification);
          vf6(delayDuckF,   lblDelDuck,  3, fy); lblDelDuck.setText("Duck", juce::dontSendNotification);
          vf6(delayCharF,   lblDelChar,  4, fy); lblDelChar.setText("Char", juce::dontSendNotification);
          // Sync + Ping = two single-row lit buttons stacked in col 5 (the words live INSIDE) [2026-07-15 12:10]
          lblDelaySync.setVisible(false);    lblDelaySync.setBounds(0, 0, 0, 0);
          lblDelayPingPong.setVisible(false); lblDelayPingPong.setBounds(0, 0, 0, 0);
          const int cx5 = sx + 8 + 5 * FP6, bw = sx + masterW - 8 - cx5;   // full remaining width = readable words
          swDelaySync.setBounds(cx5, fy + 6, bw, 22);
          swDelayPingPong.setBounds(cx5, fy + 34, bw, 22); }
        refreshMasterSyncFaders();   // free proxy vs synced variant + Character dim (Digital)

        // -- AMP ENVELOPE + EQ column. The amp-env graph top is GRAPH_Y so it aligns with the pitch-env graph. --
        const int GRAPH_Y = colTop + 38, GRAPH_H = 118;
        { int ax = cxAmp;
          hdrAmpEnv.setBounds (ax, colTop, ampEqW, hdrH);                    // box header / "AMP ENVELOPE"
          slotSelAmp.setBounds(ax + 8, colTop + 18, ampEqW - 16, 16);
          envEditor.setBounds (ax + 8, GRAPH_Y, ampEqW - 16, GRAPH_H);
          hdrEqBox.setBounds  (ax + 4, GRAPH_Y + GRAPH_H + 10, ampEqW - 8, hdrH);   // "FILTER / EQ" sub-title
          slotSelEq.setBounds (ax + 8, GRAPH_Y + GRAPH_H + 30, ampEqW - 16, 16);
          // The filter/EQ display gets the FULL width (the keytrack faders are gone - keytrack is a
          // Note -> Filter Cutoff matrix route now).
          const int fdY = GRAPH_Y + GRAPH_H + 50, fdH = 124;
          freqDisplay.setBounds (ax + 8, fdY, ampEqW - 16, fdH); }

        // -- PITCH ENVELOPE column: title on TOP, its 1/2 selector BELOW it (matches the amp-env column), graph aligned
        //    with the amp-env graph. Then "UNISON/DETUNE/VIBRATO" with ITS OWN 1/2 selector under it, the voice visual,
        //    and the channel Pitch fader at the bottom. Both selectors drive the same per-channel slot (synced).
        int px = cxPitch;
        hdrPitch.setBounds(px, colTop, pitchW, hdrH);                       // "PITCH ENVELOPE" (top)
        slotSelPitch.setBounds(px + 8, colTop + 18, pitchW - 16, 16);       // 1/2 selector BELOW the title
        pitchEditor.setBounds (px + 8, GRAPH_Y, pitchW - 16, GRAPH_H);      // aligned with the amp-env graph
        // UNISON/DETUNE/VIBRATO mirrors the EQ section EXACTLY (title +10, selector +30, visual +50 x 124) so the
        // two right-hand visuals line up + reach the same bottom. The channel Pitch fader that used to sit below
        // here moved to the FX box's free bottom (see below) to make room.
        hdrVoice.setBounds    (px + 4, GRAPH_Y + GRAPH_H + 10, pitchW - 8,  hdrH);   // "UNISON / DETUNE / VIBRATO" (= hdrEqBox)
        slotSelVoice.setBounds(px + 8, GRAPH_Y + GRAPH_H + 30, pitchW - 16, 16);     // its OWN 1/2 selector (= slotSelEq)
        voiceMod.setBounds    (px + 8, GRAPH_Y + GRAPH_H + 50, pitchW - 16, 124);    // = freqDisplay (same height + bottom)

        // -- Hide every engine knob/header/sample-widget; the active boxes re-show. --
        auto hideK = [](std::initializer_list<LearnableKnob*> ks) { for (auto* k : ks) k->setVisible(false); };
        auto hideL = [](std::initializer_list<juce::Label*> ls) { for (auto* l : ls) l->setVisible(false); };
        hideK({ &knobPitch, &knobPEnvAmt, &knobPEnvTime, &knobSmpPOff,   // knobSpeed is a hidden legacy loader (never shown; loads old ch.pitch)
                &knobLayOscShape, &knobLaySineFreq, &knobOscUnison, &knobOscDetune, &knobLaySinePEA, &knobLaySinePET, &knobLaySinePOff, &knobOscSustain, &knobOscVib,
                &knobLayNoiseType, &knobLayNoiseCtr, &knobLayNoiseWid, &knobNoiseSus,
                &knobFmPitch, &knobFmSpread, &knobFmDepth, &knobFmSub, &knobFmFeedback, &knobFmPEnv, &knobFmPTime, &knobFmPOff, &knobFmSustain,
                &knobPhysFreq, &knobPhysTone, &knobPhysMat, &knobPhysPos, &knobPhysPEnv, &knobPhysVib, &knobPhysPTime, &knobPhysPOff, &knobPhysSus });
        for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s) { knobSrcAtk[s].setVisible(false); knobSrcHold[s].setVisible(false); knobSrcDec[s].setVisible(false);
                                                             lblSrcAtk[s].setVisible(false); lblSrcHold[s].setVisible(false); lblSrcDec[s].setVisible(false); }
        hideL({ &lblPit, &lblPEnvAmt, &lblPEnvTime, &lblSmpPOff,   // lblSpeed = channel Pitch (always shown)
                &lblLayOscShape, &lblLaySineFreq, &lblOscUnison, &lblOscDetune, &lblLaySinePEA, &lblLaySinePET, &lblLaySinePOff, &lblOscSustain, &lblOscVib,
                &lblLayNoiseTypeK, &lblLayNoiseCtr, &lblLayNoiseWid, &lblNoiseSus,
                &lblFmPitch, &lblFmSpread, &lblFmDepth, &lblFmSub, &lblFmFeedback, &lblFmPEnv, &lblFmPTime, &lblFmPOff, &lblFmSustain,
                &lblPhysFreq, &lblPhysTone, &lblPhysMat, &lblPhysPos, &lblPhysPEnv, &lblPhysVib, &lblPhysPTime, &lblPhysPOff, &lblPhysSus });
        comboSampleSel.setVisible(false);
        for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) {
            waveform[b].setVisible(false); lblSampleLen[b].setVisible(false);
            lblUseRegion[b].setVisible(false); swUseRegion[b].setVisible(false);
            lblSampleReverse[b].setVisible(false); swSampleReverse[b].setVisible(false);
            lblSmpPreserve[b].setVisible(false); swSmpPreserve[b].setVisible(false);
        }

        // Use NUM_SLOTS engine headers purely as box OUTLINES (empty text; the dropdown is the visible header).
        juce::Label* boxHdr[DrumChannel::NUM_SLOTS] = { &hdrSamplerG, &hdrOscG };
        for (auto* h : { &hdrFmG, &hdrPhysG, &hdrNoiseG }) { h->setBounds(0, 0, 0, 0); h->setVisible(false); }
        for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
        {
            boxHdr[b]->setText("", juce::dontSendNotification);
            boxHdr[b]->setBounds(sbx[b], sby[b], slotW, hdrH);   // LEFT edge, two stacked half-height boxes
            boxHdr[b]->setVisible(true);
            boxHdr[b]->setInterceptsMouseClicks(false, false);   // it's only a box outline; don't eat clicks
            slotCombo[b].setVisible(true);
            slotCombo[b].setBounds(sbx[b] + 22, sby[b] - 1, slotW - 28, 19);  // clear of the corner "+" button
            slotCombo[b].toFront(false);                          // keep the dropdown clickable on top
        }

        // Each slot gets its own SlotEditor panel, stacked on the LEFT edge (HALF height each).
        // A Sample slot also shows its waveform/region/reverse above the knobs (compact for the short box).
        for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
        {
            if (boxEngine[b] < 0) {
                // Empty slot: keep it a VISIBLE (transparent) drop target so a dragged slot can land on
                // it and fill it in (it drew nothing before = nothing to drop onto). No "drop here" text.
                slotEd[b].setVisible(true);
                slotEd[b].setEngine(-1);
                slotEd[b].place(sbx[b], sby[b] + 22, slotW, slotH - 22 - 4);
                continue;
            }
            slotEd[b].setVisible(true);
            slotEd[b].setEngine(boxEngine[b]);
            int knobTop = sby[b] + 22, knobX = sbx[b], knobW = slotW;
            if (boxEngine[b] == DrumChannel::SrcSample)
            {
                waveform[b].setVisible(true);
                waveform[b].setBounds(sbx[b] + 6, sby[b] + 20, slotW - 12, 46);   // slightly shorter -> room for 3 toggles
                lblSampleLen[b].setVisible(false);                               // length is a watermark in the waveform now
                knobTop = sby[b] + 70;                                            // knobs a bit higher (user)
                // Trim + Reverse + Keep-pitch toggles stacked LEFT of the knobs - spaced so all 3 fit the box.
                const int tcW = 52;
                const int tcx = sbx[b] + 6, ty = knobTop + 2;
                auto placeTog = [&](juce::Label& l, ToggleSwitch& sw, int yy) {
                    l.setVisible(true); sw.setVisible(true); l.setJustificationType(juce::Justification::centred);
                    l.setBounds(tcx, yy, tcW, 11); sw.setBounds(tcx + (tcW - 28) / 2, yy + 12, 28, 14);
                };
                placeTog(lblUseRegion[b],     swUseRegion[b],     ty);
                placeTog(lblSampleReverse[b], swSampleReverse[b], ty + 28);
                placeTog(lblSmpPreserve[b],   swSmpPreserve[b],   ty + 56);   // bottom ~= sby+140, clears the 166px box
                knobX = sbx[b] + tcW + 6; knobW = slotW - tcW - 6;   // knobs occupy the rest, right of the toggles
            }
            slotEd[b].place(knobX, knobTop, knobW, slotH - (knobTop - sby[b]) - 4);
        }
        // BLEND fader: VERTICAL, between the two stacked slots (top = Slot 1, bottom = Slot 2).
        { const int bareaX = cxSlots + slotBoxW;        // wide blend sub-column (right of the slot boxes)
          const int bareaW = slotsColW - slotBoxW;      // ~50px so the % read-outs are legible
          const int fw = 18, fx = bareaX + (bareaW - fw) / 2;
          lblBlendTitle.setVisible(true); lblBlendBot.setVisible(true);
          lblBlendTitle.setBounds(bareaX, colTop + 2, bareaW, 14);          // Slot 1 % (yellow, ABOVE the fader)
          blendFader.setBounds(fx, colTop + 18, fw, colH - 36);
          lblBlendBot.setBounds(bareaX, colTop + colH - 16, bareaW, 14);    // Slot 2 % (pink, BELOW the fader)
          auto& bch = proc.sequencer.channel(selectedChannel);
          const bool o0 = bch.slots[0].engine >= 0, o1 = bch.slots[1].engine >= 0;
          blendFader.setEnabled(o0 && o1);             // lock (maxed to the occupied side) when only one slot is used
          blendFader.setValue((o0 && o1) ? 1.0f - bch.padX : (o1 ? 0.0f : 1.0f), juce::dontSendNotification);
          const int s2pct = (o0 && o1) ? juce::roundToInt(bch.padX * 100.0f) : (o1 ? 100 : 0);
          lblBlendTitle.setText(juce::String(100 - s2pct) + "%", juce::dontSendNotification);
          lblBlendBot.setText(juce::String(s2pct) + "%", juce::dontSendNotification); }
    }

    // ---- FX column + hide the removed boxes' widgets --------------------
    {
        // Hide the removed Channel-EQ / Channel-Filter / Channel / Pattern-Output widgets.
        hdrEq.setBounds(0, 0, 0, 0); hdrEq.setVisible(false);
        hdrFilter.setBounds(0, 0, 0, 0); hdrFilter.setVisible(false);
        comboFilterType.setBounds(0, 0, 0, 0); lblFiltType.setBounds(0, 0, 0, 0);
        knobCutoff.setBounds(0, 0, 0, 0); lblCutoff.setBounds(0, 0, 0, 0);
        knobReso.setBounds(0, 0, 0, 0);   lblReso.setBounds(0, 0, 0, 0);
        knobEnvAmt.setBounds(0, 0, 0, 0); lblEnvAmt.setBounds(0, 0, 0, 0);
        for (auto* k : { &knobVolume, &knobPan, &knobSpeed, &knobSlices, &knobStretch }) k->setBounds(0, 0, 0, 0);
        for (auto* l : { &lblVol, &lblPan, &lblSpeed, &lblSlices, &lblStretch })         l->setBounds(0, 0, 0, 0);
        hdrChan.setBounds(0, 0, 0, 0); hdrChan.setVisible(false);
        hdrMasterFX.setBounds(0, 0, 0, 0); hdrMasterFX.setVisible(false);
        comboOutput.setBounds(0, 0, 0, 0); lblOutput.setBounds(0, 0, 0, 0);

        // ===== FX column (cxFx, per-sound only): 1/2 selector | Drive dropdown + Drive-amount fader |
        //       Rev/Dly sends + Tone/Punch/Ring KNOBS | then the CHANNEL FX box (whole instrument). =====
        hdrSend.setBounds(cxFx, colTop, fxColW, hdrH);                            // "FX" header
        slotSelFx.setBounds(cxFx + 6, colTop + 18, fxColW - 12, 16);
        knobDrive.setBounds(0, 0, 0, 0); knobDrive.setVisible(false);            // drive AMOUNT is the fader now
        lblDrive.setBounds(0, 0, 0, 0); lblDrvType.setBounds(0, 0, 0, 0);        // (label removed; combo says "Drive: ...")
        const int fxX = cxFx + 6, fxW = fxColW - 12;
        // Row 1: Drive TYPE dropdown (left) + Drive AMOUNT fader (right), same line.
        const int dRow = colTop + 38, dComboW = fxW / 2 - 2;
        comboDriveType.setBounds(fxX, dRow, dComboW, 22);
        fxDriveFader.setBounds(fxX + dComboW + 4, dRow, fxW - dComboW - 4, 22);
        // Rows 2 + 3: the PER-SLOT FX knobs. Chorus/Flanger/Phaser/Comp moved OUT to CHANNEL FX below,
        // so the FX box is shorter now (5 knobs: Rev/Dly sends + Tone/Punch/Ring).
        const int KSB = 44, kboxH = 14, cw = fxW / 3;   // compressed: the CHANNEL FX box below grew a 3rd row
        auto kFx = [&](LearnableKnob& kn, juce::Label& l, int col, int y) {
            kn.setVisible(true); l.setVisible(true);
            kn.setBounds(fxX + col * cw + (cw - KSB) / 2, y, KSB, KSB + kboxH);
            l.setBounds(fxX + col * cw, y + KSB + kboxH, cw, 12);
        };
        const int fxRowStep = KSB + kboxH + 10;
        const int row2 = colTop + 62, row3 = row2 + fxRowStep;
        kFx(knobPunch, lblPunch, 0, row2); kFx(knobSub, lblSub, 1, row2); kFx(knobFormant, lblFormant, 2, row2);
        kFx(knobRing, lblRing, 0, row3); kFx(knobRingHz, lblRingHz, 1, row3); kFx(knobSlotPan, lblSlotPan, 2, row3);

        // ===== CHANNEL FX box (under the shortened FX box): Chorus / Flanger / Phaser / Comp as FOUR
        //       VERTICAL faders in ONE row (MASTER style). These act on the WHOLE channel (both slots)
        //       and do NOT follow the slot selector. =====
        hdrChannelFx.setVisible(true);
        hdrChannelFx.setBounds(cxFx, colTop + CHFX_TOP, fxColW, hdrH);
        {   // THREE effect-slot rows [type combo | Amount | Character] + the sends row [Rev | Dly] below
            const int y0 = colTop + CHFX_TOP + 20, rh = 19, gap2 = 3;
            const int cbW = 66, fW2 = (fxW - cbW - 3 * gap2) / 2;
            for (int i = 0; i < 3; ++i)
            {
                const int y = y0 + i * (rh + gap2);
                comboChFx[i].setVisible(true); comboChFx[i].setBounds(fxX, y, cbW, rh);
                chFxAmtF[i].setVisible(true);  chFxAmtF[i].setBounds(fxX + cbW + gap2, y, fW2, rh);
                chFxChrF[i].setVisible(true);  chFxChrF[i].setBounds(fxX + cbW + gap2 * 2 + fW2, y, fW2, rh);
            }
            const int ys = y0 + 3 * (rh + gap2) + 3, sW = (fxW - gap2) / 2;   // sends sit LOWER (room for the value + bus name)
            sendRevF.setVisible(true); sendRevF.setBounds(fxX, ys, sW, rh);
            sendDelF.setVisible(true); sendDelF.setBounds(fxX + sW + gap2, ys, fxW - sW - gap2, rh);
        }
        // (The MOD/LFO visual moved OUT of the FX column into the new MODULATION column - see below.)

        // ===== MODULATION column (new, user 2026-07-12): holds the MOD (= LFO) visual for now; the
        //       sources/targets routing will fill the space below in the next steps. =====
        hdrModulation.setVisible(true);
        hdrModulation.setBounds(cxMod, colTop, modW, hdrH);
        slotSelMod.setBounds(cxMod + 6, colTop + 18, modW - 12, 16);    // 1/2 slot picker (like the other boxes)
        lfoDisplay.setBounds(cxMod + 6, colTop + 38, modW - 12, 132);   // LFO 1/2/3 + Mod Env tabs
        modFaders.setVisible(true);                                     // the 12 route faders (6 rows x 2 cols)
        modFaders.setBounds(cxMod + 6, colTop + 174, modW - 12, colH - 174 - 6);
    }

    // DRAW HARMONICS overlay: parked over the amp/pitch columns (opened from the Custom wave preview;
    // hidden again at the top of every layoutContent like the sound picker).
    harmEd.setBounds(cxAmp, colTop, ampEqW + gp + pitchW + gp + fxColW, colH);   // covers amp..FX (user-outlined area)
    lfoCurveEd.setBounds(cxPitch, colTop + 60, pitchW + gp + fxColW, 220);   // LFO SHAPER overlay (near the LFO box)
    routePicker.setBounds(cxPitch, colTop + 24, pitchW + gp + fxColW, colH - 48);   // route source|target chooser (parked left of the faders)

    hdrDrive.setBounds(0, 0, 0, 0);
    lblSampleSel.setBounds(0, 0, 0, 0);

    // "Zoom" button beside every group title (row 1 + row 2), at the box top-left.
    {
        const juce::Component* zh[NUM_ZOOM] = {
            &hdrSounds, &hdrSamplerG, &hdrOscG, &hdrNoiseG, &hdrFmG, &hdrPhysG,
            &hdrEq, &hdrSend, &hdrFilter, &hdrChan, &hdrMasterFX, &hdrMasterOut,
            &hdrAmpEnv, &hdrPitch, &hdrModulation };
        for (int i = 0; i < NUM_ZOOM; ++i)
        {
            auto b = (zh[i] == &hdrMasterOut) ? juce::Rectangle<int>() : zh[i]->getBounds();
            if (b.isEmpty()) { zoomBtns[i].setBounds(0, 0, 0, 0); zoomBoxH[i] = 0; continue; }  // hidden header -> no glyph
            // Box heights: the two slots are HALF height; every other visible box is FULL column height.
            zoomBoxH[i] = (zh[i] == &hdrSamplerG || zh[i] == &hdrOscG) ? slotH : colH;
            zoomBtns[i].setBounds(b.getX() + 1, b.getY() - 1, 15, 14);   // small magnifier in the box header's top-left corner
        }
    }
    // Fade overlay over each source group's box (dims it when the source is off).
    {
        const juce::Component* fh[DrumChannel::NUM_SOURCES] =
            { &hdrSamplerG, &hdrNoiseG, &hdrOscG, &hdrFmG, &hdrPhysG };   // Sample/Noise/Osc/FM/Phys
        for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s)
        {
            auto b = fh[s]->getBounds();
            srcFade[s].setBounds(b.getX() - 5, b.getY() - 3, b.getWidth() + 10, slotH);   // half-height stacked slots
            srcFade[s].toFront(false);
        }
    }
    if (! zoomed) for (auto& zb : zoomBtns) zb.toFront(false);
    for (auto& c : slotCombo) c.toFront(false);   // engine dropdowns must stay clickable on top

    // ==== KEYS panel: covers everything RIGHT of the slot boxes (amp/EQ, pitch env, FX and
    // master columns) when the KEYS view is on. Added ON TOP (z-order) so the covered controls
    // stay laid out underneath but can't be seen or clicked; only the magnifier overlay floats
    // above it. Same column maths as the detail panel (cxAmp-6 .. right edge).
    {
        const int kx = 12 + 376 + 10 - 6;                        // cxAmp - 6
        const int kr = 12 + 376 + 10 + 330 + 10 + 250 + 10 + 200 + 10 + 290;   // cxMaster + masterW
        keysPanel.setBounds(kx, detailY + 18, kr - kx, 366 - 18 + 4);
        keysPanel.setVisible(keysView && detailShown);
        if (keysPanel.isVisible()) keysPanel.toFront(false);
    }
    stepMagOverlay.toFront(false);   // the held-step magnifier stays top-most
    countdownOverlay.toFront(false); // the count-in overlay stays above everything

    juce::ignoreUnused(group, knob, combo, groupW, curHy, curKy, x);
}

//==============================================================================
juce::AudioProcessorEditor* DrumSequencerProcessor::createEditor()
{
    uiCreatedOnce = true;   // any setStateInformation AFTER this is a user load (preset/undo), not the standalone's startup auto-restore
    return new DrumSequencerEditor(*this);
}
