#include "PluginEditor.h"
#include "../plugin/FactoryContent.h"
#include "../plugin/UserPaths.h"
#include <cstring>

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
static juce::String fmtSlot(const SlotParam& p, double v)
{
    if (p.choices.size() > 0)
        return p.choices[juce::jlimit(0, p.choices.size() - 1, (int) std::lround(v))];
    if (p.isPct) return juce::String(juce::roundToInt(v * 100.0)) + "%";
    if (p.suffix == "st") return (v > 0 ? "+" : "") + juce::String(juce::roundToInt(v)) + "st";
    if (p.suffix == "Hz") return v >= 1000.0 ? juce::String(v / 1000.0, 1) + "k"
                                             : juce::String(juce::roundToInt(v)) + "";
    if (p.suffix == "x")  return juce::String(v, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + "x";
    if (p.suffix == "sl") return (v < 1.5) ? juce::String("Off") : juce::String((int) std::lround(v));  // slices (int)
    if (p.suffix == "ms") return v < 1.0 ? juce::String(juce::roundToInt(v * 1000.0)) + "ms"
                                         : juce::String(v, 2) + "s";
    return juce::String(v, 2) + p.suffix;
}

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
    auto I = [](juce::String lab, double mn, double mx, int S::* f, juce::String suf) {
        SlotParam sp; sp.label = lab; sp.min = mn; sp.max = mx; sp.suffix = suf;
        sp.get = [f](const S& s) { return (double)(s.*f); };
        sp.set = [f](S& s, double v) { s.*f = (int) std::lround(v); };
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
                     "the Ratio + Feedback knobs then shape that FM tone (the wave display shows it live)."));
            p.add(F ("Ratio", 0, 1, &S::fmSpread, "", true,
                     "FM Ratio = the FM CHARACTER: the modulator's frequency relative to the carrier (1x..6x). "
                     "Integer-ish ratios sound harmonic/bell-like, in-between ratios sound metallic/clangy. "
                     "Only audible when the FM (Depth) fader is up."));
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
            p.add(F ("Freq", 20, 2000, &S::physFreq, "Hz", false,
                     "Base pitch of the plucked/struck string (also follows per-step + channel Pitch)."));
            p.add(F ("Tone", 0, 1, &S::physTone, "", true,
                     "Brightness of the resonator (how much high end rings)."));
            p.add(Fc("Material", 0, 5, &S::physMaterial, { "Nylon","Steel","Wood","Glass","Metal","Skin" },
                     "The string/body material - changes the damping + overtone character (Nylon = soft, Steel/Metal = "
                     "bright + long, Wood/Skin = short + dull)."));
            p.add(F ("Position", 0, 1, &S::physPosition, "", true,
                     "Strike/pluck position along the string - combs out harmonics for a hollow/nasal tone."));
            p.add(F ("Stiffness", 0, 1, &S::physStiff, "", true,
                     "Inharmonicity: bends the overtones away from a pure string toward a stiff BAR / BELL (metallic, "
                     "detuned partials). 0 = pure string."));
            p.add(Ic("Excite", 0, 2, &S::physExcite, { "Pluck","Strike","Mallet" },
                     "How the string is excited: Pluck (bright, narrow), Strike (harder, fuller), Mallet (soft, darker)."));
            break;   // pitch-env/Vibrato moved to the shared shape groups
        case DrumChannel::SrcSample:
            // Pitch (varispeed) is the channel "Pitch" control + the pitch envelope. Slices + Stretch live HERE now
            // (sample-only, per-slot) - moved out of the Channel box.
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
        case DrumChannel::SrcSynth:
            // Unified engine: every section in one box. Turn a section's Level to 0 to
            // disable it (Osc Lvl / Noise / Reson). Reuses the same fields as the
            // dedicated engines, so it can voice Analog / FM / Noise / Physical sounds.
            // Row 1 - oscillator + FM
            p.add(Ic("Shape", 0, 3, &S::oscShape, { "Sine","Tri","Square","Saw" }));
            p.add(F ("Freq", 20, 4000, &S::oscFreq, "Hz"));
            p.add(F ("Osc Lvl", 0, 1, &S::oscLevel, "", true));
            p.add(F ("Fold", 0, 1, &S::oscFold, "", true));
            p.add(F ("FM Amt", 0, 1, &S::fmDepth, "", true));
            p.add(F ("FM Ratio", 0, 1, &S::fmSpread, "", true));
            // Row 2 - FM feedback + noise + resonator
            p.add(F ("FM Fb", 0, 1, &S::fmFeedback, "", true));
            p.add(F ("Noise", 0, 1, &S::noiseLevel, "", true));
            p.add(Ic("Color", 0, 4, &S::noiseType, { "White","Pink","Brown","Grey","Purple" }));
            p.add(F ("N.Tone", 100, 16000, &S::noiseCenter, "Hz"));
            p.add(F ("N.Width", 0, 1, &S::noiseWidth, "", true));
            p.add(F ("Reson", 0, 1, &S::resonAmt, "", true));
            p.add(Fc("R.Mat", 0, 5, &S::physMaterial, { "Nylon","Steel","Wood","Glass","Metal","Skin" }));
            p.add(F ("R.Tone", 0, 1, &S::physTone, "", true));
            return p;   // Unison/Detune/Vibrato/pitch-env moved to the shared shape groups
        case DrumChannel::SrcWave: {
            juce::StringArray tnames;
            for (int i = 0; i < DrumChannel::wavetableCount(); ++i) tnames.add(DrumChannel::wavetableName(i));
            p.add(F ("Freq", 20, 1000, &S::oscFreq, "Hz", false, "Base pitch of the wavetable oscillator."));
            p.add(Ic("Table", 0, juce::jmax(0, DrumChannel::wavetableCount() - 1), &S::waveTable, tnames));
            p.add(F ("Position", 0, 1, &S::wavePos, "", true,
                     "Scan through the wavetable - morphs between its waveforms (watch the wave display)."));
            return p; }
        case DrumChannel::SrcModal: {
            juce::StringArray mats;
            for (int i = 0; i < DrumChannel::modalMaterialCount(); ++i) mats.add(DrumChannel::modalMaterialName(i));
            p.add(F ("Freq", 20, 2000, &S::oscFreq, "Hz", false, "Base pitch of the struck body (follows per-step + channel Pitch; the pitch ENVELOPE doesn't apply to Modal)."));
            p.add(Ic("Material", 0, juce::jmax(0, DrumChannel::modalMaterialCount() - 1), &S::modalMaterial, mats,
                     "The struck body - sets each mode's frequency, gain + decay (Marimba/Tubular Bell/Glass/Membrane/"
                     "Metal Plate/Wood Block/Kalimba/Cowbell). The starting point Decay/Tone/Struct then shape."));
            // Decay moved OUT to the shared amp-env editor's RING handle (Strike/Ring, like Physical) - no longer here.
            p.add(F ("Tone", 0, 1, &S::modalTone, "", true, "Brightness: dark (highs damped, quick) <-> bright (highs ring)."));
            p.add(F ("Struct", 0, 1, &S::modalStruct, "", true,
                     "Structure: stretches/compresses the mode pitches - harmonic/tuned <-> inharmonic/metallic."));
            p.add(F ("Hit Pos", 0, 1, &S::modalHit, "", true,
                     "Strike position: where the body is hit. Moves from edge (all modes ring) toward the centre, "
                     "which lands on mode nodes and combs some out - hollow/woody vs full. 0 = no comb."));
            p.add(F ("Damp", 0, 1, &S::modalDamp, "", true,
                     "Extra damping (like a hand on the bell): shortens the ring, high modes most. 0 = none."));
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
    const float cy = in.getCentreY(), hh = in.getHeight() * 0.5f - 1.0f;

    auto* s = getSlot ? getSlot() : nullptr;
    const int   wave  = s ? juce::jlimit(0, DrumChannel::oscShapeCount() - 1, s->oscShape) : 0;  // the single Wave
    const float ratio = s ? juce::jlimit(0.0f, 1.0f, s->fmSpread) : 0.0f;
    const float depth = s ? juce::jlimit(0.0f, 1.0f, s->fmDepth)  : 0.0f;

    g.setColour(juce::Colour(0xff2a2a44)); g.drawHorizontalLine((int) cy, in.getX(), in.getRight());

    // A few cycles across the width of the SELECTED wave. FM phase-modulates it (Ratio/Depth); Warp skews it.
    const int   CYC     = 4;
    const float twoPi   = juce::MathConstants<float>::twoPi;
    const float fmRatio = 1.0f + ratio * 5.0f;     // matches DSP fmModF = f*(1+spread*5)
    const float fmIndex = depth * 6.0f;            // radians of phase modulation (visual)
    const float warp = s ? juce::jlimit(0.0f, 1.0f, s->oscWarp) : 0.0f;   // Warp = one-way WAVEFOLD (0 = off)
    auto fold = [warp](float v) {                   // same fold as the DSP (adds harmonics as warp rises)
        if (warp < 0.001f) return v;
        const float folded = std::sin(v * (1.0f + warp * 4.0f) * 1.5707963f);
        return v + warp * (folded - v);
    };
    juce::Path path; const int NP = 320;
    for (int i = 0; i < NP; ++i) {
        const float fx = (float) i / (float)(NP - 1);
        float ph = fx * CYC;
        if (fmMode) ph += (fmIndex / twoPi) * std::sin(ph * fmRatio * twoPi);
        const float v  = fold(DrumChannel::oscShapeSample(wave, ph));   // all 17 shapes + wavefold warp
        const float x  = in.getX() + fx * in.getWidth();
        const float y  = cy - juce::jlimit(-1.0f, 1.0f, v) * hh;
        if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
    }
    g.setColour(juce::Colour(0xff35c0ff)); g.strokePath(path, juce::PathStrokeType(1.7f));

    // Wave name under it (the fader shows it too; this confirms what's drawn).
    if (! compact) {
        g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.setColour(juce::Colour(0xff8aa0c8));
        g.drawText(juce::String(wave + 1) + "-" + DrumChannel::oscShapeName(wave), labelRow,
                   juce::Justification::centred, false);
    }
}

void WaveMorphDisplay::mouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);   // From/To are now chosen with the faders flanking the wave (not click-to-cycle)
}

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

void SlotEditor::init(int idx, MidiLearnManager& mlm, juce::LookAndFeel* knobLNF,
                      std::function<DrumChannel::Slot*()> slotFn, std::function<void()> editFn)
{
    index = idx; getSlot = slotFn; onEdit = editFn;
    for (int i = 0; i < MAXK; ++i)
    {
        auto k = std::make_unique<LearnableKnob>("slot" + juce::String(idx) + "_k" + juce::String(i), mlm);
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
                    params[kid].set(*s, knobs[kid]->getValue());
                    if (params[kid].reBake) pendingRebake = true;   // Stretch -> needs a re-bake on release
                    if (onEdit) onEdit(); morphView.repaint(); waveView.repaint();
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
    freqFader  = mkFader("freq");
    depthFader = mkFader("fmdepth");
    resonFader = mkFader("reson");
    freqFader->setRange(20.0, 2000.0, 0.0);   // match the Physical engine's range (so its sounds can be dialed in)
    freqFader->setSkewFactorFromMidPoint(500.0);   // middle of the fader ~= 500 Hz (gently slow, not too slow)
    freqFader->textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v)) + " Hz"; };
    freqFader->setTooltip("Frequency: the oscillator's base pitch (Hz). Shared by the analog/FM tone AND the "
                          "resonator string tuning. (Shown as a fader since every section uses it.)");
    freqFader->onValueChange = [this] {
        if (auto* s = getSlot ? getSlot() : nullptr) { s->oscFreq = (float) freqFader->getValue(); if (onEdit) onEdit(); morphView.repaint(); }
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
    resonFader->setRange(0.0, 1.0, 0.0);
    resonFader->setColour(juce::Slider::trackColourId, juce::Colour(0xffd08a2e));
    resonFader->textFromValueFunction = [](double v) { return v <= 0.0005 ? juce::String("OFF")
                                                         : juce::String(juce::roundToInt(v * 100.0)) + "%"; };
    resonFader->setTooltip("Resonator (PHYSICAL): dry oscillator/FM (0) <-> pure resonator string (100%). The "
                           "RING LENGTH is the amp DECAY knob (same as the Physical engine), NOT this. So to copy a "
                           "Physical sound: Reson 100%, Drive 0, same Material/Tone/Position/Freq + amp Decay.");
    resonFader->onValueChange = [this] {
        if (auto* s = getSlot ? getSlot() : nullptr) {
            s->resonAmt = (float) resonFader->getValue();
            if (onEdit) onEdit(); morphView.repaint();
            if (activeParamCount() != lastActiveCount) relayoutSelf();   // crossing 0 reveals/hides the Physical row
        }
    };
    resonFader->onDragEnd = [this] { if (onAudition) onAudition(); };

    // Warp (ANALOG section): phase skew / PWM. 0.5 = neutral. Distinct from FM (this reshapes the cycle).
    warpFader = mkFader("warp");
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
    fromFader->setTextBoxStyle(juce::Slider::TextBoxRight, true, 92, 16);   // WIDER name read-out (track shrinks) so the shape name fits
    fromFader->setColour(juce::Slider::trackColourId, juce::Colour(0xff35c0ff));
    fromFader->setRange(0.0, (double) (DrumChannel::oscShapeCount() - 1), 1.0);
    fromFader->textFromValueFunction = [](double v) { const int i = juce::roundToInt(v);
        return juce::String(i + 1) + "-" + juce::String(DrumChannel::oscShapeName(i)); };
    fromFader->setTooltip("Wave: the oscillator's waveform - slide through all shapes (Sine, Tri, Square, Saw, Ramp, "
                          "Pulse, Hump, Vowel A/E/O, Formant, Organ, Bell, Glass, Reed, Brass, Voice).");
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
    const bool morph = (eng == DrumChannel::SrcOsc || eng == DrumChannel::SrcFM);
    morphView.setVisible(morph);
    waveView.setVisible(eng == DrumChannel::SrcWave);   // wavetable visual
    morphView.fmMode = morph;   // Analog now does FM too -> show the live FM result for both (Depth 0 = plain carrier)
    oscLayout = (eng == DrumChannel::SrcOsc);   // sectioned Analog/FM/Resonator layout with Freq/Depth/Reson faders
    freqFader->setVisible(oscLayout);
    depthFader->setVisible(false);          // FM amount is now the "Amount" KNOB (params[0]), not a fader
    resonFader->setVisible(false);          // resonator removed from Analog+FM
    warpFader->setVisible(oscLayout);
    fromFader->setVisible(oscLayout);       // the single "Wave" fader
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
        if (p.suffix == "Hz" && p.max > p.min * 4.0) knobs[i]->setSkewFactorFromMidPoint(std::sqrt(p.min * p.max));
        else                                         knobs[i]->setSkewFactor(1.0);
        knobs[i]->textFromValueFunction = [p](double v) { return fmtSlot(p, v); };
        knobs[i]->setTooltip(p.tooltip);
        labels[i]->setText(p.label, juce::dontSendNotification);
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
        fromFader->setValue (s->oscShape, juce::dontSendNotification); fromFader->updateText();   // the Wave fader
    }
    morphView.repaint();          // reflect this slot's wave (+ warp + FM)
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
    repaint();                                            // section divider lines / labels (SrcOsc)
}

// Knobs in up to TWO balanced rows that FILL the box height (so the knobs are as big as fit, no empty
// space below). 1 row for <=4 params; 2 rows for 5+ (Physical/Modal/Noise).
void SlotEditor::placeGeneric(int boxW)
{
    const int n = activeParamCount();                     // only the currently-revealed knobs
    const int mL = 6, innerW = boxW - 2 * mL;
    int yTop = 4;
    if (morphView.isVisible()) {                          // legacy Analog/FM: morph view across the top
        const int mw = juce::jmin(boxW - 8, rowWidth(MAX_ROW) + 12);
        const int mh = 40;
        morphView.compact = true;
        morphView.setBounds((boxW - mw) / 2, yTop, mw, mh);
        morphView.repaint();
        yTop += mh + 4;
    }
    if (waveView.isVisible()) {                           // Wavetable: the current waveform across the top
        const int mw = juce::jmin(boxW - 8, rowWidth(MAX_ROW) + 12);
        const int mh = 56;
        waveView.setBounds((boxW - mw) / 2, yTop, mw, mh);
        waveView.repaint();
        yTop += mh + 4;
    }
    if (n == 0) return;

    // 5+ params (Physical / Modal / Noise): stacked FULL-WIDTH faders - readable + uses the height well
    // (a cramped 2-row knob grid was wasting space). Name on the left, value on the right.
    if (n >= 5)
    {
        const int availH = juce::jmax(20, getHeight() - yTop - 2);
        const int rowH = juce::jlimit(16, 30, availH / n);
        // Wider name + value columns (so Modal material names etc. are readable) -> the fader track itself is shorter.
        // Same dimensions for Noise/Physical/Modal (they all share this path) so the boxes look consistent.
        const int tag = 62;                          // name column (left)
        const int valW = juce::jlimit(60, 90, innerW - tag - 60);  // value column (right) - room for long choice names
        for (int i = 0; i < n; ++i) {
            const int y = yTop + i * rowH;
            knobs[i]->setSliderStyle(juce::Slider::LinearHorizontal);
            knobs[i]->setTextBoxStyle(juce::Slider::TextBoxRight, true, valW, juce::jmax(12, rowH - 4));
            knobs[i]->setBounds(mL + tag, y, innerW - tag, rowH - 2);
            labels[i]->setJustificationType(juce::Justification::centredLeft);
            labels[i]->setBounds(mL, y, tag - 2, rowH - 2);
        }
        return;
    }

    // <= 4 params: one row of rotary knobs, as big as fits.
    const int cols = n;
    const int availH = juce::jmax(50, getHeight() - yTop);
    int KS = juce::jmin((innerW - (cols - 1) * GAP) / cols, availH - 24);
    KS = juce::jlimit(30, 64, KS);
    const int kh = KS + 13;
    const int rowW = cols * KS + (cols - 1) * GAP;
    int kx = mL + (innerW - rowW) / 2;                    // centre the row
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
    const int KS_FM = 44, kh_FM = KS_FM + 16;             // Ratio/Feedback knobs (value read-out below)
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
        const int reserve = 18 /*Freq+Warp row*/ + kh_FM /*FM row, names beside -> no label row*/ + 4;
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
    // -- FM: "FM" tag on the left, then the 3 FM KNOBS (Amount, Ratio, Feedback), each with its NAME BESIDE it
    //    (full-size knobs - the space freed by the old Depth fader lets all three fit one row, no shrinking). --
    fmLineY = y;
    { const int tag = 20;                                  // "FM" tag drawn at fmLabelR (paint)
      fmLabelR = juce::Rectangle<int>(mL, y + (kh_FM - 16) / 2, tag, 16);
      const int fx0 = mL + tag;
      const int nfm = juce::jmin(3, (int) params.size());  // Amount, Ratio, Feedback
      const int cell = (innerW - tag) / juce::jmax(1, nfm);
      for (int gi = 0; gi < nfm; ++gi) {
          const int cellX = fx0 + gi * cell;
          knobs[gi]->setBounds(cellX, y, KS_FM, kh_FM);                          // knob (value read-out below the dial)
          labels[gi]->setJustificationType(juce::Justification::centredLeft);
          labels[gi]->setBounds(cellX + KS_FM + 2, y + (kh_FM - 14) / 2, cell - KS_FM - 2, 14);   // NAME beside the knob
      }
      y += kh_FM + 2; }
}

// Section divider lines + names for the SrcOsc sectioned layout (drawn behind the children).
void SlotEditor::paint(juce::Graphics& g)
{
    if (! oscLayout) return;
    const float right = (float) getWidth() - 6.0f;
    const auto  lineCol = juce::Colour(0xff3a3a5e);
    g.setFont(juce::Font(10.0f, juce::Font::bold));

    // ANALOG: "Wave" (above the visual) + "Freq" + "Warp" tags to the left of their faders.
    g.setColour(juce::Colour(0xff9a9ac0));
    g.drawText("Wave", oscLabelR,  juce::Justification::centredLeft, false);
    g.drawText("Freq", freqLabelR, juce::Justification::centredLeft, false);
    g.drawText("Warp", warpLabelR, juce::Justification::centredLeft, false);

    // FM: a rule, then an "FM" tag to the left of the Depth fader.
    if (fmLineY >= 0) {
        g.setColour(lineCol);
        g.drawHorizontalLine(fmLineY - 3, 6.0f, right);
        g.setColour(juce::Colour(0xff35c0ff));
        g.drawText("FM", fmLabelR, juce::Justification::centredLeft, false);
    }
}

//==============================================================================
// Shared MIDI-learn popup (shows current assignment, lets you assign/clear)
//==============================================================================
void showMidiLearnMenu(juce::Component* target, MidiLearnManager& mlm,
                       const juce::String& paramId, int forcedChannel)
{
    juce::PopupMenu menu;
    int cc = mlm.getCCForParam(paramId);
    int ch = mlm.getChannelForParam(paramId);
    const bool learningThis  = mlm.isLearning() && mlm.getLearningParam() == paramId;
    const bool learningOther = mlm.isLearning() && ! learningThis;

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
        [mlmPtr, paramId, forcedChannel](int result) {
            if      (result == 1) mlmPtr->clearParam(paramId);
            else if (result == 2) mlmPtr->startLearning(paramId, forcedChannel);
            else if (result == 3) mlmPtr->stopLearning();
        });
}

//==============================================================================
// StepGridComponent
//==============================================================================

juce::String StepGridComponent::stepParamId(int ch, int step) const
{
    return "p" + juce::String(currentPattern) + "_step_"
         + juce::String(ch) + "_" + juce::String(step);
}

juce::Rectangle<int> StepGridComponent::stepRect(int ch, int step) const
{
    int n = numSteps[ch];
    if (n <= 0) n = 1;
    float stepW = (float)getWidth() / (float)n;
    int x = (int)(step * stepW);
    int w = (int)((step + 1) * stepW) - x;
    int y = (ch - firstRow) * rowH;          // map channel -> on-screen row (scroll offset)
    return { x, y, w, rowH };
}

void StepGridComponent::update(const Sequencer& seq, bool hasSolo)
{
    currentPattern = seq.currentPattern;
    for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch)
    {
        numSteps[ch] = seq.channel(ch).numSteps;
        muted[ch]    = seq.channel(ch).mute;
        soloed[ch]   = seq.channel(ch).solo;
        midiOutCh[ch]= seq.channel(ch).midiOut;
        playStep[ch] = seq.getChannelStep(ch);
        const auto& c = seq.channel(ch);
        for (int s = 0; s < DrumChannel::MAX_STEPS; ++s)
        {
            steps[ch][s] = c.steps[s];
            vel[ch][s]   = c.stepVel[s];
            pit[ch][s]   = c.stepPitch[s];
            prob[ch][s]  = c.stepProb[s];
            roll[ch][s]  = c.stepRoll[s];
            rollDec[ch][s] = c.stepRollDecay[s];
            noteLen[ch][s] = c.stepNoteLen[s];
            pan[ch][s]     = c.stepPan[s];
            condLen[ch][s]  = c.stepCondLen[s];
            condMask[ch][s] = c.stepCondMask[s];
        }
    }
    anySolo = hasSolo;
    curLoop = seq.loopCount;   // for the Prob-mode current-loop indicator
    repaint();
}

void StepGridComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff161626));

    const int last = juce::jmin(firstRow + visibleRows, Sequencer::NUM_CHANNELS);
    for (int ch = juce::jmax(0, firstRow); ch < last; ++ch)
    {
        bool effectiveMute = muted[ch] || (anySolo && !soloed[ch]);
        int n = numSteps[ch];

        for (int step = 0; step < n; ++step)
        {
            auto rr = stepRect(ch, step);
            auto r  = rr.toFloat().reduced(3.5f, 2.0f);   // taller cells (small horiz gap; vertical drag-lock prevents mis-hits)
            bool isActive  = steps[ch][step];
            bool isCurrent = (playStep[ch] == step);

            if (editMode == ModeSteps)
            {
                if (isCurrent)
                {
                    g.setColour(isActive ? juce::Colour(0xffffaa00) : juce::Colour(0xff3a3810));
                    g.fillRoundedRectangle(r, 4.0f);
                    g.setColour(juce::Colour(0xffffcc33));
                    g.drawRoundedRectangle(r, 4.0f, 1.5f);
                }
                else if (isActive)
                {
                    g.setColour(effectiveMute ? juce::Colour(0xff666666) : juce::Colour(0xff22cc55));
                    g.fillRoundedRectangle(r, 4.0f);
                }
                else
                {
                    bool dark = ((step * 2 / (n > 1 ? n / 2 + 1 : 2)) % 2 == 1);
                    g.setColour(dark ? juce::Colour(0xff222233) : juce::Colour(0xff2a2a40));
                    g.fillRoundedRectangle(r, 4.0f);
                    g.setColour(juce::Colour(0xff3a3a55));
                    g.drawRoundedRectangle(r, 4.0f, 0.5f);
                }

                // MIDI assignment label (top of cell). Assigned = two proportional
                // lines: "ch{N}" over "cc{N}"; the font scales with the cell width so
                // it stays readable with few steps and still fits when there are many.
                if (midiLearn != nullptr && rr.getWidth() > 22)
                {
                    juce::String pid = stepParamId(ch, step);
                    int cc = midiLearn->getCCForParam(pid);
                    g.setColour((isActive || isCurrent) ? juce::Colours::black.withAlpha(0.72f)
                                : (cc >= 0 ? juce::Colour(0xff9fc4f2) : juce::Colour(0xff55556e)));

                    const float fs = juce::jlimit(8.0f, 13.0f, rr.getWidth() * 0.16f);
                    if (cc >= 0)
                    {
                        const int mch = midiLearn->getChannelForParam(pid);
                        const int lh = (int) fs + 1;
                        g.setFont(juce::Font(fs, juce::Font::bold));
                        g.drawText("ch" + juce::String(mch), rr.getX() + 1, rr.getY() + 2,
                                   rr.getWidth() - 2, lh, juce::Justification::centredTop, false);
                        g.drawText("cc" + juce::String(cc),  rr.getX() + 1, rr.getY() + 2 + lh,
                                   rr.getWidth() - 2, lh, juce::Justification::centredTop, false);
                    }
                    else
                    {
                        g.setFont(juce::Font(juce::jmin(fs, 10.5f)));
                        g.drawText("no midi", rr.getX() + 1, rr.getY() + 3, rr.getWidth() - 2,
                                   (int) fs + 2, juce::Justification::centredTop, false);
                    }
                }
            }
            else
            {
                // Value mode: each cell is a fader (Velocity/Probability) or a bipolar
                // bar centred on 0 (Pitch). Inactive steps are dimmed.
                g.setColour(juce::Colour(0xff1d1d2e));
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(juce::Colour(0xff3a3a55));
                g.drawRoundedRectangle(r, 4.0f, 0.5f);

                const float alpha = isActive ? 1.0f : 0.30f;
                juce::String txt;
                if (editMode == ModeRoll)
                {
                    // Roll = a 2D cell: COUNT (number of bars, Y) x RAMP (X). The bars step UP or DOWN
                    // across the ratchet so you SEE the per-hit velocity ramp: left-drag = fade out
                    // (each hit quieter), centre = flat, right-drag = build up (each hit louder).
                    const int   rc = juce::jlimit(1, 6, roll[ch][step]);
                    const float rr = juce::jlimit(-1.0f, 1.0f, rollDec[ch][step]);   // bipolar ramp
                    const float innerW = r.getWidth() - 4.0f, innerH = r.getHeight() - 6.0f;
                    const float bw = innerW / (float) rc;
                    g.setColour(juce::Colour(0xffffaa33).withAlpha(alpha));
                    for (int k = 0; k < rc; ++k)
                    {
                        const float frac = (rc > 1) ? (float) k / (float)(rc - 1) : 0.0f; // 0=first..1=last
                        const float vs   = (rr >= 0.0f) ? (1.0f - rr) + rr * frac          // build up
                                                        : 1.0f + rr * frac;                // fade out
                        const float bh   = juce::jmax(2.0f, innerH * juce::jmax(0.06f, vs));
                        const float bxx  = r.getX() + 2.0f + k * bw;
                        g.fillRect(juce::Rectangle<float>(bxx + 0.5f, r.getBottom() - 2.0f - bh, bw - 1.0f, bh));
                    }
                    txt = "x" + juce::String(rc);
                }
                else if (editMode == ModePitch)
                {
                    const float semis = juce::jlimit(-24.0f, 24.0f, pit[ch][step]);
                    const float midY = r.getCentreY();
                    const float frac = semis / 24.0f;                 // -1..1
                    const float h = std::abs(frac) * (r.getHeight() * 0.5f);
                    juce::Rectangle<float> bar = frac >= 0
                        ? juce::Rectangle<float>(r.getX(), midY - h, r.getWidth(), h)
                        : juce::Rectangle<float>(r.getX(), midY,     r.getWidth(), h);
                    g.setColour(juce::Colour(0xff35c0ff).withAlpha(alpha));
                    g.fillRect(bar.reduced(1.0f, 0.0f));
                    g.setColour(juce::Colour(0x66ffffff));
                    g.drawHorizontalLine((int) midY, r.getX(), r.getRight()); // zero line
                    txt = (semis > 0 ? "+" : "") + juce::String(juce::roundToInt(semis));
                }
                else if (editMode == ModePan)
                {
                    // Pan = a bipolar HORIZONTAL bar from the centre: left-drag pans left, right pans right.
                    const float pn = juce::jlimit(-1.0f, 1.0f, pan[ch][step]);
                    const float midX = r.getCentreX();
                    const float w = std::abs(pn) * (r.getWidth() * 0.5f);
                    juce::Rectangle<float> bar = pn >= 0
                        ? juce::Rectangle<float>(midX,     r.getY(), w, r.getHeight())
                        : juce::Rectangle<float>(midX - w, r.getY(), w, r.getHeight());
                    g.setColour(juce::Colour(0xff2ec4b6).withAlpha(alpha));
                    g.fillRect(bar.reduced(0.0f, 1.0f));
                    g.setColour(juce::Colour(0x66ffffff));
                    g.drawVerticalLine((int) midX, r.getY(), r.getBottom());   // centre line
                    txt = pn == 0.0f ? juce::String("C")
                                     : juce::String(pn < 0 ? "L" : "R") + juce::String(juce::roundToInt(std::abs(pn) * 100.0f));
                }
                else if (editMode == ModeProb)
                {
                    // Loop condition: N bars (cycle length) across the bottom ~60%. Enabled bars filled bright +
                    // white outline; disabled = empty with a grey outline; the playing loop's bar = amber outline.
                    const int N = juce::jlimit(1, 5, condLen[ch][step]);
                    const int mask = condMask[ch][step];
                    const float innerW = r.getWidth() - 4.0f, bw = innerW / (float) N;
                    const float barH = r.getHeight() * 0.6f, top = r.getBottom() - barH - 2.0f;
                    int cnt = 0;
                    for (int k = 0; k < N; ++k)
                    {
                        const float bxx = r.getX() + 2.0f + k * bw;
                        juce::Rectangle<float> bar(bxx + 1.0f, top, bw - 2.0f, barH);
                        const bool on = ((mask >> k) & 1) != 0; if (on) ++cnt;
                        g.setColour(juce::Colour(0xff1a1530).withAlpha(alpha));  g.fillRect(bar);   // empty slot
                        if (on) { g.setColour(juce::Colour(0xffc77dff).withAlpha(alpha)); g.fillRect(bar); }
                        const bool playing = (N > 1 && (curLoop % N) == k);
                        g.setColour(playing ? juce::Colour(0xffffcc33) : juce::Colour(0xddffffff).withAlpha(alpha));
                        g.drawRect(bar, playing ? 1.6f : 1.0f);
                    }
                    txt = (mask == 0) ? ("/" + juce::String(N)) : (juce::String(cnt) + "/" + juce::String(N));
                }
                else
                {
                    const float v01 = juce::jlimit(0.0f, 1.0f, vel[ch][step]);   // ModeVel
                    const float h = v01 * r.getHeight();
                    g.setColour(juce::Colour(0xff22cc55).withAlpha(alpha));
                    if (editMode == ModeVel && midiOutCh[ch])
                    {
                        // MIDI-out: a 2D "note" - WIDTH = length, HEIGHT = velocity (like a piano-roll note).
                        const float lw = juce::jlimit(0.08f, 1.0f, noteLen[ch][step]) * r.getWidth();
                        g.fillRect(juce::Rectangle<float>(r.getX(), r.getBottom() - h, lw, h).reduced(1.0f, 0.0f));
                        txt = juce::String(juce::roundToInt(v01 * 100.0f)) + "%";
                    }
                    else
                    {
                        g.fillRect(juce::Rectangle<float>(r.getX(), r.getBottom() - h, r.getWidth(), h).reduced(1.0f, 0.0f));
                        txt = juce::String(juce::roundToInt(v01 * 100.0f)) + "%";
                    }
                }
                if (isCurrent) { g.setColour(juce::Colour(0xffffcc33)); g.drawRoundedRectangle(r, 4.0f, 1.5f); }
                if (rr.getWidth() > 20)
                {
                    g.setColour(juce::Colours::white.withAlpha(isActive ? 0.9f : 0.4f));
                    g.setFont(juce::Font(9.5f, juce::Font::bold));
                    g.drawText(txt, rr.getX() + 2, rr.getY() + 3, rr.getWidth() - 4, 12,
                               juce::Justification::centredTop, false);
                }
            }

            // Step number (bottom-right)
            if (rr.getWidth() > 22)
            {
                g.setColour((isActive || isCurrent) ? juce::Colours::black.withAlpha(0.5f)
                                                    : juce::Colour(0xff444466));
                g.setFont(juce::Font(9.0f));
                g.drawText(juce::String(step + 1), r.toNearestInt().reduced(3),
                           juce::Justification::bottomRight, false);
            }

            // MIDI-learn highlight: amber ring while this step waits to learn a CC.
            if (midiLearn != nullptr && midiLearn->isLearning()
                && midiLearn->getLearningParam() == stepParamId(ch, step))
            {
                g.setColour(juce::Colour(0x55ffd23b)); g.fillRoundedRectangle(r, 4.0f);
                g.setColour(juce::Colour(0xffffd23b)); g.drawRoundedRectangle(r, 4.0f, 2.2f);
            }
        }
    }
}

bool StepGridComponent::findStepAt(juce::Point<int> pos, int& outCh, int& outStep) const
{
    const int last = juce::jmin(firstRow + visibleRows, Sequencer::NUM_CHANNELS);
    for (int ch = juce::jmax(0, firstRow); ch < last; ++ch)
    {
        int n = numSteps[ch];
        for (int step = 0; step < n; ++step)
            if (stepRect(ch, step).reduced(1, 1).contains(pos))   // ~full cell clickable (the drag-lock prevents mis-hits now)
            {
                outCh = ch; outStep = step;
                return true;
            }
    }
    return false;
}

void StepGridComponent::handleClick(juce::Point<int> pos, bool setDragState)
{
    int ch, step;
    if (!findStepAt(pos, ch, step)) return;

    if (setDragState)
        lastDragState = !steps[ch][step];

    if (steps[ch][step] != lastDragState)
    {
        steps[ch][step] = lastDragState;
        if (onStepClicked) onStepClicked(ch, step);
    }
}

void StepGridComponent::applyInfluence(int ch, int srcStep)
{
    if (ch < 0 || ch >= Sequencer::NUM_CHANNELS) return;
    // Copy ONLY the parameter currently being edited onto every step.
    for (int s = 0; s < DrumChannel::MAX_STEPS; ++s)
    {
        if (editMode == ModeVel)        vel[ch][s]  = vel[ch][srcStep];
        else if (editMode == ModePitch) pit[ch][s]  = pit[ch][srcStep];
        else if (editMode == ModeProb)  { condLen[ch][s] = condLen[ch][srcStep]; condMask[ch][s] = condMask[ch][srcStep]; }
        else if (editMode == ModePan)   pan[ch][s]  = pan[ch][srcStep];
        else if (editMode == ModeRoll)  { roll[ch][s] = roll[ch][srcStep]; rollDec[ch][s] = rollDec[ch][srcStep]; }
    }
    if (onInfluenceApply) onInfluenceApply(ch, srcStep); // write through to the channel data
}

void StepGridComponent::handleValueDrag(juce::Point<int> pos)
{
    // HARD LOCK: a value drag ONLY ever edits the step the gesture started on (set in mouseDown). No per-move
    // findStepAt fallback - so once you click a step you can NEVER drag onto a neighbour's parameter.
    if (dragChannel < 0 || dragStep < 0) return;
    const int ch = dragChannel, step = dragStep;
    auto r = stepRect(ch, step);
    const float v01 = juce::jlimit(0.0f, 1.0f, 1.0f - (float)(pos.y - r.getY()) / (float) juce::jmax(1, r.getHeight()));
    float value = v01;                                   // Velocity / Probability
    if (editMode == ModePitch)     value = (v01 * 2.0f - 1.0f) * 24.0f;     // -24..+24 semis
    else if (editMode == ModeRoll) value = (float)(1 + juce::roundToInt(v01 * 5.0f)); // 1..6
    else if (editMode == ModePan)  { const float xn = (float)(pos.x - r.getX()) / (float) juce::jmax(1, r.getWidth());
                                     value = juce::jlimit(-1.0f, 1.0f, (xn - 0.5f) * 2.0f); }   // X = pan -1..+1

    if (editMode == ModeVel) {
        vel[ch][step] = value;                           // Y = velocity
        if (midiOutCh[ch]) {                             // MIDI-out: X = per-step note length (2D cell)
            const float xn = (float)(pos.x - r.getX()) / (float) juce::jmax(1, r.getWidth());
            noteLen[ch][step] = juce::jlimit(0.0f, 1.0f, xn);
        }
    }
    else if (editMode == ModeProb)  prob[ch][step] = value;
    else if (editMode == ModePitch) pit[ch][step]  = value;
    else if (editMode == ModePan)   pan[ch][step]  = value;
    else if (editMode == ModeRoll) {
        roll[ch][step] = (int) value;
        // 2D pad: Y = ratchet count, X = the per-hit RAMP across the ratchet. Centre = flat,
        // left = each hit quieter (fade out), right = each hit louder (build up). Stored -1..+1.
        const float xn = (float)(pos.x - r.getX()) / (float) juce::jmax(1, r.getWidth());  // 0..1
        rollDec[ch][step] = juce::jlimit(-1.0f, 1.0f, (xn - 0.5f) * 2.0f);
    }

    if (onStepValueChanged) onStepValueChanged(ch, step, editMode, value);
    if (influenceArmed[ch]) applyInfluence(ch, step);    // propagate this step to all
    repaint();
}

void StepGridComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (editMode == ModeSteps) return;   // on/off has no "value" to reset
    int ch, step;
    if (! findStepAt(e.getPosition(), ch, step)) return;
    // Reset this step's value (for the current edit mode) to its default.
    if (editMode == ModeProb) {   // reset the loop condition (every loop)
        condLen[ch][step] = 1; condMask[ch][step] = 0;
        if (onStepCondChanged) onStepCondChanged(ch, step, 1, 0);
        if (influenceArmed[ch]) applyInfluence(ch, step);
        repaint(); return;
    }
    float prim = 1.0f;
    switch (editMode) {
        case ModeVel:   vel[ch][step] = 1.0f; noteLen[ch][step] = 0.25f; prim = 1.0f; break;
        case ModePitch: pit[ch][step] = 0.0f; prim = 0.0f; break;
        case ModePan:   pan[ch][step] = 0.0f; prim = 0.0f; break;
        case ModeRoll:  roll[ch][step] = 1; rollDec[ch][step] = 0.0f; prim = 1.0f; break;
        default: return;
    }
    if (onStepValueChanged) onStepValueChanged(ch, step, editMode, prim);
    if (influenceArmed[ch]) applyInfluence(ch, step);
    repaint();
}

void StepGridComponent::mouseDown(const juce::MouseEvent& e)
{
    int ch, step;
    bool onStep = findStepAt(e.getPosition(), ch, step);

    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
    {
        if (onStep && midiLearn != nullptr)
        {
            int forced = getLearnChannel ? getLearnChannel() : -1;
            showMidiLearnMenu(this, *midiLearn, stepParamId(ch, step), forced);
        }
        return;
    }

    if (onStep && onChannelSelected) onChannelSelected(ch);
    if (editMode == ModeSteps) handleClick(e.getPosition(), true);
    else if (editMode == ModeProb)
    {
        // Loop-condition editor: remember where we pressed (drag = set cycle length; click a bar = toggle it).
        condDragCh = onStep ? ch : -1; condDragStep = step; condDragged = false; condDownX = e.getPosition().x;
        if (onStep) { auto r = stepRect(ch, step); const int N = juce::jmax(1, condLen[ch][step]);
            const float xn = juce::jlimit(0.0f, 0.999f, (float)(e.getPosition().x - r.getX()) / (float) juce::jmax(1, r.getWidth()));
            condDownBar = juce::jlimit(0, N - 1, (int)(xn * (float) N)); }
    }
    else { dragChannel = onStep ? ch : -1; dragStep = onStep ? step : -1; handleValueDrag(e.getPosition()); }
}

void StepGridComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu()) return;
    if (editMode == ModeSteps) { handleClick(e.getPosition(), false); return; }
    if (editMode == ModeProb)
    {
        if (condDragCh < 0) return;
        if (std::abs(e.getPosition().x - condDownX) > 4) condDragged = true;
        if (condDragged) {                                    // horizontal drag -> cycle length 1..5
            auto r = stepRect(condDragCh, condDragStep);
            const float xn = juce::jlimit(0.0f, 0.999f, (float)(e.getPosition().x - r.getX()) / (float) juce::jmax(1, r.getWidth()));
            const int N = juce::jlimit(1, 5, 1 + (int)(xn * 5.0f));
            if (N != condLen[condDragCh][condDragStep]) {
                condLen[condDragCh][condDragStep] = N;
                if (onStepCondChanged) onStepCondChanged(condDragCh, condDragStep, N, condMask[condDragCh][condDragStep]);
                repaint();
            }
        }
        return;
    }
    handleValueDrag(e.getPosition());
}

void StepGridComponent::mouseUp(const juce::MouseEvent&)
{
    // Loop-condition: a plain CLICK (no drag) toggles the bar under the cursor.
    if (editMode == ModeProb && condDragCh >= 0)
    {
        if (! condDragged && condDownBar >= 0) {
            condMask[condDragCh][condDragStep] ^= (1 << condDownBar);
            if (onStepCondChanged) onStepCondChanged(condDragCh, condDragStep, condLen[condDragCh][condDragStep], condMask[condDragCh][condDragStep]);
            if (influenceArmed[condDragCh]) applyInfluence(condDragCh, condDragStep);
            repaint();
        }
        condDragCh = -1; condDragStep = -1; condDownBar = -1;
    }
    // An Influence drag ends: stop propagating, so later edits are per-step again.
    if (dragChannel >= 0 && influenceArmed[dragChannel])
    {
        influenceArmed[dragChannel] = false;
        if (onInfluenceDisarm) onInfluenceDisarm(dragChannel);
    }
    dragChannel = -1;
    dragStep = -1;
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
    g.setFont(juce::Font(11.0f));
    g.drawText("Drag MIDI", getLocalBounds(), juce::Justification::centred);
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
    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
    {
        int forced = learnChannelProvider ? learnChannelProvider() : -1;
        showMidiLearnMenu(this, midiLearn, paramId, forced);
        return;
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
        showMidiLearnMenu(this, *midiLearn, paramId, forced);
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
    if (onSelect) onSelect();
}

//==============================================================================
// ContentComponent
//==============================================================================

void ContentComponent::paint(juce::Graphics& g) { owner.paintContent(g); }
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
    // AHD only (decay to zero). A/H/D all max at 6 s, so they get EQUAL horizontal travel (1/3 each); sum 1.0 so a
    // fully-maxed env reaches the right edge. Sustain + Release are retired from the UI (susW/wR = 0).
    wA = Wu / 3.0f; wH = Wu / 3.0f; wD = Wu / 3.0f; susW = 0.0f; wR = 0.0f;
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
    if (strikeRing) { wA = in.getWidth() * 0.42f; wD = in.getWidth() * 0.50f; wH = 0.0f; }  // 2 bands: Strike | Ring
    // Strike (attack) is a SHORT 0..50ms range on a LINEAR axis (spreads evenly); the generic AHD attack keeps its
    // ms-skewed 0..6s axis.
    g.xA = g.left + wA * (strikeRing ? juce::jlimit(0.0f, 1.0f, atk / maxAStrike) : skew(atk / maxA));
    g.xH = strikeRing ? g.xA : (g.xA + kMinHold + wH * skew(hld / maxH));   // Strike/Ring: peak is a single point (no Hold)
    g.xD = g.xH   + wD * skew(dcy / maxD);
    g.xS = g.xD   + susW;     // susW == 0 now (sustain retired) -> xS == xD
    g.xR = g.xS   + wR * skew(rel / maxR);   // wR == 0 -> xR == xD
    g.susY = g.bottom;        // decay falls to ZERO (AHD); no sustain plateau
    return g;
}

void ADSRDisplay::handlePts(juce::Point<float> out[4]) const
{
    const Geo q = geom();    // 3 handles: Attack, Hold, Decay (to zero). [3] kept = Decay so old [4] callers are safe.
    out[0] = { q.xA, q.top }; out[1] = { q.xH, q.top }; out[2] = { q.xD, q.bottom }; out[3] = out[2];
}

int ADSRDisplay::nearestHandle(juce::Point<float> p) const
{
    juce::Point<float> h[4]; handlePts(h);
    int best = 0; float bd = 1.0e9f;
    for (int i = 0; i < 3; ++i) { if (strikeRing && i == 1) continue;   // no Hold handle in Strike/Ring
        float d = p.getDistanceFrom(h[i]); if (d < bd) { bd = d; best = i; } }
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
    p.lineTo(q.xA, q.top); p.lineTo(q.xH, q.top); p.lineTo(q.xD, q.bottom);   // Attack / Hold / Decay-to-zero
    juce::Path fill = p; fill.lineTo(q.left, q.bottom); fill.closeSubPath();
    g.setColour(juce::Colour(0x3340cc88)); g.fillPath(fill);
    g.setColour(juce::Colour(0xff55ddaa)); g.strokePath(p, juce::PathStrokeType(1.6f));

    juce::Point<float> h[4]; handlePts(h);
    const int active = drag >= 0 ? drag : hover;
    for (int i = 0; i < 3; ++i) {
        if (strikeRing && i == 1) continue;   // Strike/Ring: no Hold handle
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
    g.drawText("length ~" + envTimeStr(total), getLocalBounds().reduced(5).withTrimmedTop(14), juce::Justification::topRight, false);

    if (strikeRing) {   // make it obvious this is the Physical-specific envelope (cue at top-left)
        g.setColour(juce::Colour(0xff7a8aa8)); g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText("STRIKE / RING", getLocalBounds().reduced(6), juce::Justification::topLeft, false);
    }

    // Live read-out of the hovered / dragged handle (top-right, above the length).
    if (active >= 0)
    {
        juce::String t;
        switch (active) {
            case 0: t = (strikeRing ? "Strike " : "Attack ") + envTimeStr(atk); break;
            case 1: t = "Hold " + envTimeStr(hld); break;
            case 2: t = (strikeRing ? "Ring " : "Decay ") + envTimeStr(dcy); break;
        }
        g.setColour(kEnvCols[active]); g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.drawText(t, getLocalBounds().reduced(5), juce::Justification::topRight, false);
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
    if (ts <= dEnd) { float p = (ts - hEnd) / dWin;                       return { lp(q.xH, q.xD, p), lp(q.top, q.bottom, p) }; }
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
    const int i = drag >= 0 ? drag : hover;
    if (strikeRing) {   // Physical: Strike(attack) + Ring(decay), no Hold
        switch (i) {
            case 0: return "Strike (" + envTimeStr(atk) + ") - how soft the pluck/strike is: 0 = sharp pluck, higher = "
                           "a slow swelled strike (the string is held up so it still reaches full volume)";
            case 2: return "Ring (" + envTimeStr(dcy) + ") - how long the string rings out after the strike (drag left/right)";
            default: return "Physical envelope: drag Strike (pluck softness) + Ring (how long it rings)";
        }
    }
    switch (i) {
        case 0: return "Attack (" + envTimeStr(atk) + ") - time to rise from silence to full level";
        case 1: return "Hold (" + envTimeStr(hld) + ") - time held at full before the decay";
        case 2: return "Decay (" + envTimeStr(dcy) + ") - fall time from full to silence (drag left/right)";
        default: return "Drag the coloured handles to shape Attack / Hold / Decay";
    }
}

void ADSRDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (! enabledLook) return;   // sample slots: no amp envelope to edit
    drag = nearestHandle(e.position);
    mouseDrag(e);
}

void ADSRDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (drag < 0) return;
    const Geo q = geom();
    const float bw = getLocalBounds().toFloat().reduced(6.0f).getWidth();
    float wA, wH, wD, susW, wR; adsrBands(bw, wA, wH, wD, susW, wR);
    if (strikeRing) { wA = bw * 0.42f; wD = bw * 0.50f; }   // match geom()'s Strike|Ring bands
    switch (drag)
    {
        case 0: atk = strikeRing ? maxAStrike * juce::jlimit(0.0f, 1.0f, (e.position.x - q.left) / wA)
                                 : maxA * invSkew((e.position.x - q.left) / wA); break;
        case 1: hld = maxH * invSkew((e.position.x - q.xA - kMinHold) / wH); break;
        case 2: dcy = maxD * invSkew((e.position.x - q.xH) / wD); break;   // horizontal only (decay to zero; no sustain)
        default: break;
    }
    atk = juce::jlimit(0.0005f, strikeRing ? maxAStrike : maxA, atk); hld = juce::jlimit(0.0f, maxH, hld);
    dcy = juce::jlimit(0.002f, maxD, dcy);  rel = juce::jlimit(0.0f, maxR, rel);
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
        // green->amber->red gradient laid over the FULL width, clipped to the filled portion.
        juce::ColourGradient grad (green, r.getX(), 0.0f, red, r.getRight(), 0.0f, false);
        grad.addColour (0.78, amber);
        g.setGradientFill (grad);
        g.fillRect (r.withWidth (r.getWidth() * lv));
        if (pk > 0.01f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            const float px = r.getX() + r.getWidth() * pk;
            g.fillRect (juce::Rectangle<float> (px - 0.75f, r.getY(), 1.5f, r.getHeight()));
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
void VoiceModDisplay::setValues(int unison, float detune, float vibrato, bool centreOn, int detuneMode)
{
    uni = juce::jlimit(1, kMaxUni, unison);
    det = juce::jlimit(0.0f, 1.0f, detune);
    vib = juce::jlimit(0.0f, 1.0f, vibrato);
    centre = centreOn;
    mode = juce::jlimit(0, 2, detuneMode);
    repaint();
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
    q.left = b.getX(); q.right = b.getRight(); q.top = b.getY() + 10.0f; q.bottom = b.getBottom() - 2.0f;
    q.cy = (q.top + q.bottom) * 0.5f; q.hh = (q.bottom - q.top) * 0.5f - 2.0f;
    const float w = q.right - q.left;
    q.uX = q.left + w * 0.16f;   // Unison handle (left)
    q.dX = q.left + w * 0.46f;   // Detune handle home X (centre)
    q.vX = q.left + w * 0.86f;   // Vibrato handle (right)
    q.rangeX = (q.vX - q.dX) * 0.7f;
    q.rangeY = q.hh * 0.85f;
    // Detune dot: a simple vertical handle (drag up = more symmetric spread). [directional mode reverted]
    q.dPtX = q.dX; q.dPtY = q.cy - det * q.rangeY;
    return q;
}
int VoiceModDisplay::nearestHandle(juce::Point<float> p) const
{
    const Geo q = geom();
    juce::Point<float> pts[3] = {
        { q.uX, q.bottom - (float)(uni - 1) / (float)(kMaxUni - 1) * (q.bottom - q.top) },  // 0 Unison
        { q.dPtX, q.dPtY },                                                                 // 1 Detune (mode-aware)
        { q.vX, q.cy - vib * q.hh * 0.85f } };                                              // 2 Vibrato
    int best = -1; float bd = 22.0f * 22.0f;
    for (int i = 0; i < 3; ++i) {
        if ((i <= 1 && !uniOn) || (i == 2 && !vibOn)) continue;
        const float dx = p.x - pts[i].x, dy = p.y - pts[i].y, d2 = dx * dx + dy * dy;
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best;
}
juce::String VoiceModDisplay::getTooltip()
{
    if (!uniOn && !vibOn) return "Voice: " + reason;
    // Per-handle tooltip (depends on which dot the mouse is over).
    if (hover == 0 && uniOn)
        return "UNISON (cyan): drag up/down to set how many voices are stacked (1-7). More voices = a thicker, "
               "wider sound. Detune spreads them apart.";
    if (hover == 1 && uniOn)
        return "DETUNE (amber): drag up to spread the unison voices apart (symmetrically, both sharp and flat). "
               "The cents value is how far the OUTERMOST voice sits from the original pitch, each way "
               "(100c = 1 semitone). DOUBLE-CLICK to also play the original (undetuned) pitch alongside the "
               "detuned copies" + juce::String(centre ? " (currently ON)." : " (currently OFF).");
    if (hover == 2 && vibOn)
        return "VIBRATO (pink): drag up for more pitch wobble at ~5.5 Hz. Shown in semitones (up to ~1.5 st).";
    juce::String s = "Voice controls for the selected slot. ";
    if (uniOn) s += "Unison = stacked voices; Detune = spread (double-click Detune to add the dry/original pitch). ";
    if (vibOn) s += "Vibrato = ~5.5 Hz pitch wobble.";
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
    g.setColour(juce::Colour(0x14ffffff)); g.drawHorizontalLine((int) q.cy, q.left, q.right);  // centre pitch line

    // The voice cluster: `uni` lines spread symmetrically by detune, each wobbling by vibrato.
    const int n = uniOn ? uni : 1;
    const float spread  = uniOn ? det * q.hh * 0.85f : 0.0f;
    const float vibAmp  = vibOn ? vib * q.hh * 0.30f : 0.0f;
    const int   W = juce::jmax(2, (int) (q.right - q.left));
    auto drawVoice = [&](float off, juce::Colour col, float thick) {
        juce::Path path;
        for (int xi = 0; xi <= W; xi += 2) {
            const float x = q.left + (float) xi;
            const float ph = (float) xi / (float) W * juce::MathConstants<float>::twoPi * 3.0f;
            const float y = q.cy + off + std::sin(ph) * vibAmp;
            if (xi == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
        }
        g.setColour(col); g.strokePath(path, juce::PathStrokeType(thick));
    };
    for (int k = 0; k < n; ++k) {
        const float off = (n > 1) ? (-spread + 2.0f * spread * (float) k / (float)(n - 1)) : 0.0f;
        const bool isMid = (n % 2 == 1 && k == n / 2);
        drawVoice(off, juce::Colour(isMid ? 0xff35c0ff : 0x9935c0ff).withMultipliedAlpha(isMid ? 1.0f : 0.55f),
                  isMid ? 1.6f : 1.0f);
    }
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
        const float uy = q.bottom - (float)(uni - 1) / (float)(kMaxUni - 1) * (q.bottom - q.top);
        handle(q.uX, uy, juce::Colour(0xff35c0ff), 0, juce::String(uni) + "x");
        // DETUNE: real cents spread (how far the outermost voice sits from the original, each way).
        handle(q.dPtX, q.dPtY, juce::Colour(0xffffc23a), 1,
               juce::String::fromUTF8("±") + juce::String(juce::roundToInt(det * 100.0f)) + "c");
    }
    if (vibOn) {
        // VIBRATO shown as the real peak pitch deviation in semitones (~1.5 st at full).
        const float vibSt = 12.0f * std::log2(1.0f + 0.09f * vib);
        handle(q.vX, q.cy - vib * q.hh * 0.85f, juce::Colour(0xffff7ab0), 2, juce::String(vibSt, 2) + "st");
    }

    // tiny per-handle captions (which dot is which), since the box title names all three
    g.setColour(juce::Colour(0x559a9ac0)); g.setFont(juce::Font(8.0f, juce::Font::plain));
    if (uniOn) {
        g.drawText("uni",    juce::Rectangle<float>(q.uX - 18, q.bottom - 1, 36, 9), juce::Justification::centred, false);
        g.drawText("detune", juce::Rectangle<float>(q.dX - 24, q.bottom - 1, 48, 9), juce::Justification::centred, false);
    }
    if (vibOn) g.drawText("vib", juce::Rectangle<float>(q.vX - 18, q.bottom - 1, 36, 9), juce::Justification::centred, false);
}
void VoiceModDisplay::mouseMove(const juce::MouseEvent& e) { int h = nearestHandle(e.position); if (h != hover) { hover = h; repaint(); } }
void VoiceModDisplay::mouseDown(const juce::MouseEvent& e) { drag = nearestHandle(e.position); if (drag >= 0) mouseDrag(e); }
void VoiceModDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (drag < 0) return;
    const Geo q = geom();
    if (drag == 0) uni = juce::jlimit(1, kMaxUni, 1 + juce::roundToInt((q.bottom - e.position.y) / juce::jmax(1.0f, q.bottom - q.top) * (kMaxUni - 1)));
    else if (drag == 1) { mode = 0; det = juce::jlimit(0.0f, 1.0f, (q.cy - e.position.y) / juce::jmax(1.0f, q.rangeY)); }
    else if (drag == 2) vib = juce::jlimit(0.0f, 1.0f, (q.cy - e.position.y) / juce::jmax(1.0f, q.hh * 0.85f));
    emit();
    repaint();
}
void VoiceModDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Double-click the DETUNE dot -> also play the original (undetuned) pitch alongside the detuned copies.
    if (uniOn && nearestHandle(e.position) == 1) { centre = ! centre; emit(); repaint(); if (onDragEnd) onDragEnd(); }
}

//==============================================================================
// FrequencyDisplay
//==============================================================================
static const juce::Colour kEqCols[DrumChannel::NUM_EQ_BANDS] = {
    juce::Colour(0xff35c0ff),  // HP
    juce::Colour(0xff5ad17a),  // bell 1
    juce::Colour(0xffffc23a),  // bell 2
    juce::Colour(0xffff7ab0),  // bell 3
    juce::Colour(0xffb98cff),  // LP
};

void FrequencyDisplay::setBands(DrumChannel::EqBand* b, int formantType, float cutoff, float reso, double sr)
{
    bands = b; fType = formantType; fCutoff = cutoff; fReso = reso; sampleRate = sr;
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

// Combined EQ magnitude (dB) at frequency f from the enabled bands.
float FrequencyDisplay::responseDb(float f) const
{
    if (bands == nullptr || sampleRate <= 0.0) return 0.0f;
    const double nyq = sampleRate * 0.49;
    double m = 1.0;
    if (bands[DrumChannel::EQ_HP].on) {
        const double fr = juce::jlimit(20.0, nyq, (double) bands[DrumChannel::EQ_HP].freq);
        Biquad h; h.highpass(sampleRate, fr, 0.5412); m *= h.magnitudeAt(f, sampleRate);
                  h.highpass(sampleRate, fr, 1.3066); m *= h.magnitudeAt(f, sampleRate);
    }
    for (int b = 0; b < 3; ++b) {
        const auto& bd = bands[DrumChannel::EQ_B1 + b];
        if (! bd.on) continue;
        Biquad pk; pk.peaking(sampleRate, juce::jlimit(20.0, nyq, (double) bd.freq),
                              juce::jlimit(0.2, 12.0, (double) bd.q), (double) bd.gainDb);
        m *= pk.magnitudeAt(f, sampleRate);
    }
    if (bands[DrumChannel::EQ_LP].on) {
        const double fr = juce::jlimit(20.0, nyq, (double) bands[DrumChannel::EQ_LP].freq);
        Biquad l; l.lowpass(sampleRate, fr, 0.5412); m *= l.magnitudeAt(f, sampleRate);
                  l.lowpass(sampleRate, fr, 1.3066); m *= l.magnitudeAt(f, sampleRate);
    }
    return juce::Decibels::gainToDecibels((float) m);
}

juce::Point<float> FrequencyDisplay::handlePos(juce::Rectangle<float> a, int b) const
{
    if (bands == nullptr) return {};
    const float x = xForFreq(a, bands[b].freq);
    // HP/LP handles ride the response curve at their freq; bells use their gain.
    const float y = (b == DrumChannel::EQ_HP || b == DrumChannel::EQ_LP)
                    ? yForDb(a, juce::jlimit(-kMaxDb, kMaxDb, responseDb(bands[b].freq)))
                    : yForDb(a, bands[b].gainDb);
    return { x, y };
}

int FrequencyDisplay::nearestBand(juce::Point<float> p) const
{
    if (bands == nullptr) return -1;
    const auto a = plotArea();
    int best = -1; float bd = 16.0f * 16.0f;
    for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) {
        const float d = p.getDistanceSquaredFrom(handlePos(a, b));
        if (d < bd) { bd = d; best = b; }
    }
    return best;
}

void FrequencyDisplay::paint(juce::Graphics& g)
{
    auto bb = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff101022)); g.fillRoundedRectangle(bb, 4.0f);
    g.setColour(juce::Colour(0xff33335a)); g.drawRoundedRectangle(bb.reduced(0.5f), 4.0f, 1.0f);

    const auto a = plotArea();
    const float left = a.getX(), right = a.getRight(), top = a.getY(), bottom = a.getBottom(), w = a.getWidth(), h = a.getHeight();

    // 0 dB centre line + frequency grid with labels.
    g.setColour(juce::Colour(0xff242440)); g.drawHorizontalLine((int) a.getCentreY(), left, right);
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = xForFreq(a, f);
        g.setColour(juce::Colour(0xff20203a)); g.drawVerticalLine((int) x, top, bottom);
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

    // Combined EQ response curve.
    if (bands != nullptr && sampleRate > 0.0)
    {
        juce::Path resp; bool started = false;
        for (float px = 0; px <= w; px += 2.0f) {
            const float y = yForDb(a, responseDb(normToFreq(px / w)));
            if (! started) { resp.startNewSubPath(left + px, y); started = true; } else resp.lineTo(left + px, y);
        }
        g.setColour(juce::Colour(0xffffaa33)); g.strokePath(resp, juce::PathStrokeType(1.9f));

        // Band handles: solid when enabled, hollow/dim when off. Double-click toggles.
        const int active = drag >= 0 ? drag : hover;
        for (int b = 0; b < DrumChannel::NUM_EQ_BANDS; ++b) {
            const auto p = handlePos(a, b);
            const float r = (b == active) ? 6.5f : 5.0f;
            const auto col = kEqCols[b];
            if (bands[b].on) {
                if (b == active) { g.setColour(juce::Colours::white); g.drawEllipse(p.x - r - 2, p.y - r - 2, (r + 2) * 2, (r + 2) * 2, 1.2f); }
                g.setColour(col); g.fillEllipse(p.x - r, p.y - r, r * 2, r * 2);
                g.setColour(juce::Colours::black); g.drawEllipse(p.x - r, p.y - r, r * 2, r * 2, 1.0f);
            } else {
                g.setColour(col.withAlpha(0.45f)); g.drawEllipse(p.x - r, p.y - r, r * 2, r * 2, 1.4f);
            }
            g.setColour(bands[b].on ? juce::Colours::black : col.withAlpha(0.6f));
            g.setFont(juce::Font(8.5f, juce::Font::bold));
            const char* tag = (b == DrumChannel::EQ_HP) ? "H" : (b == DrumChannel::EQ_LP) ? "L" : juce::String(b).toRawUTF8();
            g.drawText(tag, juce::Rectangle<float>(p.x - r, p.y - r, r * 2, r * 2), juce::Justification::centred, false);
        }
    }

    g.setColour(juce::Colour(0xff8090b0)); g.setFont(juce::Font(9.0f, juce::Font::bold));
    g.drawText("EQ  (drag bands - wheel = width - double-click = on/off)", getLocalBounds().reduced(4),
               juce::Justification::topLeft, false);
}

void FrequencyDisplay::mouseMove(const juce::MouseEvent& e) { int b = nearestBand(e.position); if (b != hover) { hover = b; repaint(); } }

void FrequencyDisplay::mouseDown(const juce::MouseEvent& e) { drag = nearestBand(e.position); if (drag >= 0) mouseDrag(e); }

void FrequencyDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (drag < 0 || bands == nullptr) return;
    const auto a = plotArea();
    auto& bd = bands[drag];
    bd.on = true;                                   // dragging a band turns it on
    bd.freq = juce::jlimit(20.0f, 20000.0f, freqForX(a, e.position.x));
    if (drag != DrumChannel::EQ_HP && drag != DrumChannel::EQ_LP)
        bd.gainDb = dbForY(a, e.position.y);         // bells: vertical = gain
    if (onEdit) onEdit();
    repaint();
}

void FrequencyDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (bands == nullptr) return;
    int b = nearestBand(e.position);
    if (b < 0) {   // empty space -> reset ALL bands to their defaults (all off)
        for (int i = 0; i < DrumChannel::NUM_EQ_BANDS; ++i) bands[i] = DrumChannel::defaultEqBand(i);
        if (onEdit) onEdit();
        repaint();
        return;
    }
    bands[b].on = ! bands[b].on;   // on a handle -> toggle that band on/off
    if (onEdit) onEdit();
    repaint();
}

void FrequencyDisplay::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wd)
{
    if (bands == nullptr) return;
    int b = nearestBand(e.position);
    if (b < 0 || b == DrumChannel::EQ_HP || b == DrumChannel::EQ_LP) return;   // Q only for bells
    auto& bd = bands[b];
    bd.q = juce::jlimit(0.2f, 12.0f, bd.q * (wd.deltaY > 0 ? 1.12f : 1.0f / 1.12f));
    if (onEdit) onEdit();
    repaint();
}

juce::String FrequencyDisplay::getTooltip()
{
    const int b = drag >= 0 ? drag : hover;
    if (b < 0 || bands == nullptr)
        return "Channel EQ: drag a band to move it, mouse-wheel a bell for width (Q), double-click to enable/disable. "
               "H = high-pass, L = low-pass (both 24 dB/oct), 1/2/3 = bells.";
    const auto& bd = bands[b];
    const juce::String nm = (b == DrumChannel::EQ_HP) ? "High-pass" : (b == DrumChannel::EQ_LP) ? "Low-pass" : ("Bell " + juce::String(b));
    juce::String fs = bd.freq >= 1000.0f ? juce::String(bd.freq / 1000.0f, 2) + " kHz" : juce::String((int) bd.freq) + " Hz";
    juce::String s = nm + (bd.on ? "  " : " (off)  ") + fs;
    if (b != DrumChannel::EQ_HP && b != DrumChannel::EQ_LP)
        s += "  " + juce::String(bd.gainDb, 1) + " dB  Q " + juce::String(bd.q, 2);
    return s;
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

static constexpr int SAMPLE_ID_BASE   = 1000;
static constexpr int FACTORY_MIX_BASE = 5000;   // factory sound mixes: 5000 + index
static constexpr int FACTORY_PST_BASE = 6000;   // factory presets:     6000 + index
static constexpr int ID_INIT_PRESET   = 9003;
static constexpr int ID_INIT_MIX      = 9996;
static constexpr int ID_NONE          = 9997;
static constexpr int ID_OPEN_FOLDER   = 9998;
static constexpr int ID_LOAD_SAMPLE   = 9999;
static constexpr int ID_OPEN_BANK     = 9995;   // reveal the Sound Bank folder (from the channel sound dropdown)
static constexpr int ID_BROWSE        = 10001;   // open the sound-browser window (outside the sample-id range)

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
            &hdrAmpEnv, &hdrPitch };   // <- amp/eq + pitch columns added (they had no zoom)
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
    visiblePatterns = juce::jlimit(16, Sequencer::NUM_PATTERNS, proc.visiblePatterns) <= 16 ? 16 : 32;
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
    startTimerHz(24);
    proc.midiLearn.addListener(this);
}

DrumSequencerEditor::~DrumSequencerEditor()
{
    proc.midiLearn.removeListener(this);
    stopTimer();
    comboSampleSel.setLookAndFeel(nullptr);
    for (auto& c : slotCombo) c.setLookAndFeel(nullptr);
    for (auto* k : allKnobs) k->setLookAndFeel(nullptr);
    blendFader.setLookAndFeel(nullptr);   // had a custom two-tone LNF
    patModeBtn.setLookAndFeel(nullptr);
    for (juce::Button* b : { (juce::Button*)&btnPlay, (juce::Button*)&btnStop, (juce::Button*)&btnUndo,
                             (juce::Button*)&btnRedo, (juce::Button*)&btnRoute, (juce::Button*)&btnSaveMix,
                             (juce::Button*)&btnToggleDetail, (juce::Button*)&btnClearPat, (juce::Button*)&btnTooltips,
                             (juce::Button*)&btnCh8, (juce::Button*)&btnCh16, (juce::Button*)&btnPat16,
                             (juce::Button*)&btnPat32 }) b->setLookAndFeel(nullptr);
    for (auto& s : strips)
    {
        s.btnPoly.setLookAndFeel(nullptr);
        s.btnInfluence.setLookAndFeel(nullptr);
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
    root->addItem(ID_LOAD_SAMPLE, "Load Sample...");
    root->addItem(ID_OPEN_FOLDER, "Open Samples Folder");

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
    else if (id == ID_OPEN_FOLDER)
    {
        getSamplesFolder().revealToUser();
        rescanSamples();
        rebuildSampleMenu();
    }
    else if (id == ID_LOAD_SAMPLE)
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load Sample", getSamplesFolder(), "*.wav;*.aiff;*.aif;*.mp3;*.flac;*.ogg");
        const int ch = selectedChannel;
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, ch](const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f.existsAsFile())
                {
                    // Load the file IN PLACE (no copy). Only samples that actually live in the samples folder
                    // appear in the dropdown; one loaded from elsewhere just plays (it isn't added to the library).
                    auto& c2 = proc.sequencer.channel(ch);
                    c2.slots[envTargetSlot()].engine = DrumChannel::SrcSample;
                    c2.loadUserSample(envTargetSlot(), f);
                    cacheWaveform(ch);
                }
                rescanSamples();
                rebuildSampleMenu();
            });
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
    // automatically flows it into extra columns to the side.
    auto facNames = Factory::mixNames();
    auto facCats  = Factory::mixCategories();
    static const char* catOrder[] = { "Kicks", "Snares & Claps", "Hats & Cymbals", "Toms",
                                      "Percussion", "Bass", "Bells & Mallets", "Plucks & Strings", "Modal", "FX & Synth" };
    // "Unified Synth" category removed - that engine + its factory mixes are retired from the UI.
    for (auto* cat : catOrder)
    {
        bool wroteHeader = false;
        for (int i = 0; i < facNames.size(); ++i)
            if (facCats[i] == cat)
            {
                if (! wroteHeader) { root->addSectionHeader(cat); wroteHeader = true; }
                root->addItem(FACTORY_MIX_BASE + i, facNames[i] + "  (" + Factory::mixSourceTag(i) + ")");
            }
    }
    // The user's own saved sounds (Your Sound Bank), mirroring any subfolders as submenus.
    root->addSectionHeader("Your Sound Bank");
    if (soundMixFiles.isEmpty())
        root->addItem(-1, "(none saved yet)", false, false);
    else
        addSoundFolderToMenu(*root, getSoundMixFolder());
    root->addSeparator();
    root->addItem(ID_OPEN_BANK, "Open Sound Bank Folder");
    juce::ignoreUnused(keep);
    // clear() above reset the combo's displayed text, so invalidate the cache to
    // force updateStripMixLabel to re-apply this channel's mix name.
    stripMixShown[ch] = juce::String();
    // Show this pattern/channel's own selected mix name (+ * if edited).
    updateStripMixLabel(ch);
}

void DrumSequencerEditor::handleSoundMixChange(int ch)
{
    int id = strips[ch].comboSound.getSelectedId();
    auto& c = proc.sequencer.channel(ch);
    if (id == ID_OPEN_BANK)
    {
        getSoundMixFolder().revealToUser();
        rescanSoundMixes(); for (int k = 0; k < Sequencer::NUM_CHANNELS; ++k) rebuildSoundMixMenu(k);
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
}

//==============================================================================
// Cheap rolling hashes used to detect "modified since saved" for the * marker.
juce::int64 DrumSequencerEditor::channelSoundHash(const DrumChannel& c) const
{
    auto mix = [](juce::int64 h, juce::int64 v) { return h * 1000003LL ^ v; };
    auto f   = [](float x) { juce::int64 b = 0; std::memcpy(&b, &x, sizeof(float)); return b; };
    juce::int64 h = 146959810934665603LL;
    for (int i = 0; i < DrumChannel::NUM_SOURCES; ++i) { h = mix(h, c.srcOn[i] ? 1 : 0); h = mix(h, f(c.srcWeight[i])); }
    h = mix(h, f(c.padX)); h = mix(h, f(c.padY)); h = mix(h, c.padLayoutB ? 1 : 0);
    // Slots are the runtime source of truth (incl. duplicate engines) - hash them too.
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) {
        const auto& sl = c.slots[b];
        h = mix(h, sl.engine); h = mix(h, f(sl.weight));
        h = mix(h, f(sl.atk)); h = mix(h, f(sl.hold)); h = mix(h, f(sl.dec)); h = mix(h, f(sl.sustain)); h = mix(h, f(sl.release)); h = mix(h, f(sl.vibrato));
        h = mix(h, sl.oscShape); h = mix(h, sl.oscShapeB); h = mix(h, f(sl.oscFreq)); h = mix(h, f(sl.oscPEnvAmt)); h = mix(h, f(sl.oscPEnvTime)); h = mix(h, f(sl.oscPOffset));
        h = mix(h, sl.oscUnison); h = mix(h, f(sl.oscDetune)); h = mix(h, sl.oscUniCenter ? 1 : 0); h = mix(h, sl.oscDetuneMode);
        h = mix(h, sl.fxDriveType); h = mix(h, f(sl.fxDrive)); h = mix(h, f(sl.fxReverbSend)); h = mix(h, f(sl.fxDelaySend));
        h = mix(h, sl.noiseType); h = mix(h, f(sl.noiseCenter)); h = mix(h, f(sl.noiseWidth)); h = mix(h, f(sl.noiseRes)); h = mix(h, f(sl.noiseDrive)); h = mix(h, f(sl.noiseCrackle));
        h = mix(h, f(sl.fmPitch)); h = mix(h, f(sl.fmSpread)); h = mix(h, f(sl.fmDepth)); h = mix(h, f(sl.fmPEnvAmt)); h = mix(h, f(sl.fmPEnvTime)); h = mix(h, f(sl.fmPOffset)); h = mix(h, f(sl.fmFeedback)); h = mix(h, f(sl.fmSub));
        h = mix(h, f(sl.physFreq)); h = mix(h, f(sl.physTone)); h = mix(h, f(sl.physMaterial)); h = mix(h, f(sl.physPosition)); h = mix(h, f(sl.physPEnvAmt)); h = mix(h, f(sl.physPEnvTime)); h = mix(h, f(sl.physPOffset)); h = mix(h, f(sl.physStiff)); h = mix(h, sl.physExcite);
        h = mix(h, f(sl.smpSpeed)); h = mix(h, f(sl.smpCrush)); h = mix(h, f(sl.smpPitch)); h = mix(h, f(sl.smpPEnvAmt)); h = mix(h, f(sl.smpPEnvTime)); h = mix(h, f(sl.smpPOffset)); h = mix(h, sl.smpReverse ? 1 : 0); h = mix(h, sl.smpUseRegion ? 1 : 0);
        h = mix(h, f(sl.smpStart)); h = mix(h, f(sl.smpEnd)); h = mix(h, sl.smpSlices); h = mix(h, f(sl.smpStretch)); h = mix(h, f(sl.smpGain));
        h = mix(h, sl.smpRegN); for (int r = 0; r < DrumChannel::Slot::MAXREG; ++r) { h = mix(h, f(sl.smpRegLo[r])); h = mix(h, f(sl.smpRegHi[r])); }
        h = mix(h, (juce::int64) c.slotSample[b].file.getFullPathName().hashCode64());   // this slot's sample
        h = mix(h, f(sl.oscFold)); h = mix(h, f(sl.oscLevel)); h = mix(h, f(sl.noiseLevel)); h = mix(h, f(sl.resonAmt)); h = mix(h, f(sl.resonDrive));
        h = mix(h, sl.waveTable); h = mix(h, f(sl.wavePos)); h = mix(h, f(sl.oscWarp));
        h = mix(h, sl.modalMaterial); h = mix(h, f(sl.modalDecay)); h = mix(h, f(sl.modalTone)); h = mix(h, f(sl.modalStruct)); h = mix(h, f(sl.modalHit)); h = mix(h, f(sl.modalDamp));
        for (int k = 0; k < DrumChannel::Slot::NPE; ++k) { h = mix(h, f(sl.pEnvP[k])); h = mix(h, f(sl.pEnvT[k])); }
        for (int e = 0; e < DrumChannel::NUM_EQ_BANDS; ++e) { const auto& eb = sl.eqBand[e]; h = mix(h, eb.on ? 1 : 0); h = mix(h, f(eb.freq)); h = mix(h, f(eb.gainDb)); h = mix(h, f(eb.q)); }
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
    h = mix(h, f(c.bloom)); h = mix(h, f(c.drift)); h = mix(h, f(c.spread)); h = mix(h, f(c.punch)); h = mix(h, f(c.glue));
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
        h = mix(h, P.chainLen); for (int k = 0; k < P.chainLen; ++k) { h = mix(h, P.chainSeq[k]); h = mix(h, P.chainLoops[k]); }
        const auto& m = P.master;        // per-pattern master FX + output
        h = mix(h, f(m.reverbRoom)); h = mix(h, f(m.reverbDamp)); h = mix(h, f(m.reverbWet));
        h = mix(h, f(m.reverbPreDelay)); h = mix(h, f(m.reverbWidth));
        h = mix(h, f(m.delayTime)); h = mix(h, f(m.delayFeedback)); h = mix(h, m.delaySync ? 1 : 0); h = mix(h, m.delayDivision); h = mix(h, m.delayPingPong ? 1 : 0);
        h = mix(h, f(m.volume)); h = mix(h, f(m.pan)); h = mix(h, m.mono ? 1 : 0); h = mix(h, f(m.limit)); h = mix(h, f(m.glue));
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            auto& ch = P.channels[c];
            h = mix(h, channelSoundHash(ch));
            h = mix(h, ch.numSteps);
            juce::int64 st = 0; for (int i = 0; i < DrumChannel::MAX_STEPS; ++i) st = (st << 1) | (ch.steps[i] ? 1 : 0);
            h = mix(h, st); h = mix(h, ch.mute ? 1 : 0); h = mix(h, ch.solo ? 2 : 0);
            for (int i = 0; i < ch.numSteps; ++i) { h = mix(h, f(ch.stepVel[i])); h = mix(h, f(ch.stepPitch[i])); h = mix(h, f(ch.stepProb[i])); h = mix(h, ch.stepRoll[i]); h = mix(h, f(ch.stepRollDecay[i])); h = mix(h, f(ch.stepPan[i])); h = mix(h, ch.stepCondLen[i]); h = mix(h, ch.stepCondMask[i]); }
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
    c.bloom = 0.0f; c.drift = 0.0f; c.spread = 0.0f; c.punch = 0.0f; c.glue = 0.0f;
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
    t.setProperty("bloom",  ch.bloom,  nullptr);
    t.setProperty("drift",  ch.drift,  nullptr);
    t.setProperty("spread", ch.spread, nullptr);
    t.setProperty("punch",  ch.punch,  nullptr);
    t.setProperty("glue",   ch.glue,   nullptr);
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
    t.setProperty("rev", ch.reverbSend, nullptr); t.setProperty("dly", ch.delaySend, nullptr);
    ch.writeSlots(t);   // 3-slot model (duplicate engines survive Save/Load)
}

void DrumSequencerEditor::readChannelMix(const juce::ValueTree& t, DrumChannel& ch, juce::String& missingSample) const
{
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
    ch.bloom  = (float)t.getProperty("bloom",  0.0f);
    ch.drift  = (float)t.getProperty("drift",  0.0f);
    ch.spread = (float)t.getProperty("spread", 0.0f);
    ch.punch  = (float)t.getProperty("punch",  0.0f);
    ch.glue   = (float)t.getProperty("glue",   0.0f);
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
    comboPreset.addItem("Save Preset...",       9001);
    comboPreset.addItem("Open Presets Folder",  9002);
    comboPreset.setItemEnabled(-1, false);
    comboPreset.setTextWhenNothingSelected("Presets");
    comboPreset.setSelectedId(0, juce::dontSendNotification);
}

void DrumSequencerEditor::handlePresetChange()
{
    int id = comboPreset.getSelectedId();

    if (id >= FACTORY_PST_BASE && id < FACTORY_PST_BASE + Factory::presetNames().size())
    {
        const int pi = id - FACTORY_PST_BASE;
        Factory::applyPreset(proc.sequencer, pi);
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
    else if (id == 9002) // Open folder
    {
        getPresetsFolder().revealToUser();
        comboPreset.setSelectedId(0, juce::dontSendNotification);
    }
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
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        auto& P = s.patterns[p];
        P.swing = 0.0f; P.playMode = Sequencer::LoopForever; P.repeatTarget = 2; P.gotoPattern = 0;
        P.chainLen = 0; P.chainStep = 0;
        for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c)
        {
            auto& ch = P.channels[c];
            resetChannelToDefault(ch, c);
            ch.chokeGroup = 0; ch.outputBus = 0; ch.midiOut = false; ch.midiOutChannel = 1;   // routing is preset-level -> reset on Init too
            ch.numSteps = 8;
            for (int i = 0; i < DrumChannel::MAX_STEPS; ++i) ch.steps[i] = false;
        }
    }
    syncAfterStateChange();
    rebaselinePreset(juce::String());   // a fresh init has no preset name, clean baseline
}

void DrumSequencerEditor::refreshPatternOptions()
{
    auto& p = proc.sequencer.patterns[currentPattern()];
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
    refreshKeysButton();
    refreshAuditionButton();
    // Routing (choke group / aux Out / MIDI Out) is CHANNEL-WIDE - it must be identical on every pattern. A load can
    // leave it inconsistent (old projects, or patterns 16-31 that weren't in the saved file), which made it "sometimes"
    // show stale routing. Force every pattern's copy to match pattern 0 (the preset/reset authority).
    for (int c = 0; c < Sequencer::NUM_CHANNELS; ++c) {
        const auto& s0 = proc.sequencer.patterns[0].channels[c];
        for (int p = 1; p < Sequencer::NUM_PATTERNS; ++p) {
            auto& cc = proc.sequencer.patterns[p].channels[c];
            cc.chokeGroup = s0.chokeGroup; cc.outputBus = s0.outputBus; cc.midiOut = s0.midiOut; cc.midiOutChannel = s0.midiOutChannel;
        }
    }
    refreshRouting();   // routing/choke are preset-level now -> recolour the strips after a preset/state change
    visiblePatterns = juce::jlimit(16, Sequencer::NUM_PATTERNS, proc.visiblePatterns) <= 16 ? 16 : 32;
    firstPatternCol = juce::jlimit(0, juce::jmax(0, visiblePatterns - patShown()), firstPatternCol);
    refreshCountButtons();
}

//== Undo / redo (whole-instrument state snapshots) ===========================
void DrumSequencerEditor::pushUndoSnapshot()
{
    UndoEntry e;
    proc.getStateInformation(e.state);
    e.presetName         = presetName;          // remember the preset label as it is now
    e.presetBaselineHash = presetBaselineHash;
    e.presetModified     = presetModified;
    undoStack.push_back(std::move(e));
    if ((int) undoStack.size() > kUndoMax) undoStack.erase(undoStack.begin());
    redoStack.clear();
    updateUndoRedoEnabled();
}

void DrumSequencerEditor::applyUndoState(const UndoEntry& e)
{
    applyingUndo = true;
    proc.setStateInformation(e.state.getData(), (int) e.state.getSize());
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

void DrumSequencerEditor::doUndo()
{
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
    content.addAndMakeVisible(verLink);
    verLink.setButtonText("v" DAVULSEQ_VERSION);
    verLink.setURL(juce::URL("https://github.com/Kanebos9/BASAMAK/releases/latest"));
    verLink.setFont(juce::Font(11.5f, juce::Font::bold), false, juce::Justification::centredLeft);
    verLink.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xffe8bf4d));   // brand gold - inviting
    verLink.setTooltip("BASAMAK v" DAVULSEQ_VERSION " - click to check GitHub for the latest version & updates.\n\n"
                       "Installed a newer version but this still shows the old number? Rescan your plugins "
                       "in your DAW/host and reopen the project - the DAW may have cached the old build.");

    content.addAndMakeVisible(btnClearMidi);
    btnClearMidi.setTooltip("Clear ALL MIDI assignments (every knob, button and step). Asks first. "
                            "Use this to start your MIDI mapping over from scratch.");
    btnClearMidi.onClick = [this] {
        juce::NativeMessageBox::showOkCancelBox(juce::AlertWindow::QuestionIcon,
            "Clear all MIDI assignments?",
            "This removes every MIDI CC assignment in the whole plugin. This cannot be undone.",
            this, juce::ModalCallbackFunction::create([this](int ok) {
                if (ok) { proc.midiLearn.clearAll(); content.repaint(); }
            }));
    };

    content.addAndMakeVisible(btnUndo);
    btnUndo.setTooltip("Undo the last change (sounds, steps, FX, patterns - up to a couple dozen steps back).");
    btnUndo.onClick = [this] { doUndo(); };
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
    content.addAndMakeVisible(sliderSwing);
    sliderSwing.setRange(0.0, 0.7, 0.01);
    sliderSwing.setValue(proc.sequencer.current().swing, juce::dontSendNotification);
    sliderSwing.setSliderStyle(juce::Slider::LinearHorizontal);
    sliderSwing.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 42, 20);
    sliderSwing.textFromValueFunction = [](double v){ return juce::String(juce::roundToInt(v / 0.7 * 100.0)) + "%"; };
    sliderSwing.onValueChange = [this] { proc.sequencer.current().swing = (float)sliderSwing.getValue(); };

    // Step-grid edit-mode radio buttons.
    content.addAndMakeVisible(lblEditMode);
    lblEditMode.setText("Edit:", juce::dontSendNotification);
    lblEditMode.setFont(juce::Font(11.0f, juce::Font::bold));
    lblEditMode.setColour(juce::Label::textColourId, juce::Colour(0xff7799cc));
    lblEditMode.setJustificationType(juce::Justification::centredRight);
    lblEditMode.setMinimumHorizontalScale(0.7f);   // squeeze "Edit:" rather than clip it ("Ed...") on wider fonts
    for (auto* b : { &btnModeVel, &btnModePitch, &btnModeProb, &btnModeRoll, &btnModePan })
    {
        content.addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2a4a));
        b->setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    }
    btnModeVel.onClick   = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeVel   ? 0 : StepGridComponent::ModeVel);   };
    btnModePitch.onClick = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModePitch ? 0 : StepGridComponent::ModePitch); };
    btnModeProb.onClick  = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeProb  ? 0 : StepGridComponent::ModeProb);  };
    btnModeRoll.onClick  = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModeRoll  ? 0 : StepGridComponent::ModeRoll);  };
    btnModePan.onClick   = [this] { setStepEditMode(stepGrid.editMode == StepGridComponent::ModePan   ? 0 : StepGridComponent::ModePan);   };
    // Make the edit-mode buttons MIDI-learnable (right-click). They drive UI state,
    // so the processor relays the CC back to the editor (see uiMidiEditMode).
    btnModeVel.midiLearn   = &proc.midiLearn; btnModeVel.paramId   = "ui_mode_vel";
    btnModePitch.midiLearn = &proc.midiLearn; btnModePitch.paramId = "ui_mode_pitch";
    btnModeProb.midiLearn  = &proc.midiLearn; btnModeProb.paramId  = "ui_mode_prob";
    btnModeRoll.midiLearn  = &proc.midiLearn; btnModeRoll.paramId  = "ui_mode_roll";
    btnModePan.midiLearn   = &proc.midiLearn; btnModePan.paramId   = "ui_mode_pan";
    btnModeVel.setTooltip("Velocity / Length mode: drag a step UP/DOWN for its velocity (0-100%). For channels that "
                          "sequence another plugin (MIDI Out), drag LEFT/RIGHT to set that note's LENGTH too - the bar "
                          "gets wider for longer notes. Length does nothing for built-in sounds (their amp envelope sets "
                          "their length); it only applies in MIDI Out mode. Click again to leave.");
    btnModePitch.setTooltip("Pitch edit mode: each step becomes a bipolar bar - centre is +0, drag up for higher / down for lower pitch (semitones). Affects the whole sound of that hit.");
    btnModeProb.setTooltip("Loop-condition mode: DRAG a step left/right to set a cycle of N bars, then CLICK the bars to pick which loops it fires on (e.g. N=6, bars 3 & 6 -> fires only on the 3rd and 6th loop). No bars picked = every loop. The step must also be ON in normal mode. Double-click resets.");
    btnModeRoll.setTooltip("Roll / ratchet mode: each step is a 2D cell. Drag UP/DOWN = how many times it re-fires (1-6). "
                           "Drag LEFT/RIGHT = the velocity RAMP across those hits: centre = flat (all equal), left = fade "
                           "OUT (each hit quieter), right = build UP (each hit louder). The bars show the ramp shape. Great "
                           "for drum-roll fades, crescendo buzzes and stutters.");
    btnModePan.setTooltip("Pan edit mode: each step becomes a bipolar bar - drag LEFT/RIGHT to place that hit in the "
                          "stereo field (centre = middle). Per-step pan rides on top of the channel Pan. For built-in "
                          "sounds only (a MIDI-Out channel sends notes, not audio). Click again to leave.");

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
    dragMidi.getMidiFile = [this] { return proc.exportMidiFile(); };

    // Preset menu
    content.addAndMakeVisible(lblPreset);
    lblPreset.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    content.addAndMakeVisible(comboPreset);
    comboPreset.onChange = [this] { handlePresetChange(); };

    // Pattern row
    content.addAndMakeVisible(lblPatterns);
    lblPatterns.setFont(juce::Font(11.0f, juce::Font::bold));
    lblPatterns.setColour(juce::Label::textColourId, juce::Colour(0xff7799cc));
    for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
    {
        auto& pb = patternBtns[p];
        pb.index = p;
        pb.midiLearn = &proc.midiLearn;
        // Clicking a pattern is a manual choice -> stop auto-following so the view sticks
        // (and, while playing, it only changes the view; playback keeps going on its own).
        pb.onSelect = [this, p] { selectPattern(p); };   // Follow is a global toggle - clicking a pattern doesn't change it
        pb.onCopyFrom = [this, p] (int src) { copyPatternContent(src, p); selectPattern(p); };
        pb.setTooltip("Pattern " + juce::String(p + 1) + ". Click to view + edit it. DRAG it onto another pattern "
                      "to COPY this pattern's steps, swing, play-mode + FX into that slot (the sounds are shared, so "
                      "only the sequencing copies). Right-click to MIDI-learn.");
        content.addAndMakeVisible(pb);
    }
    content.addAndMakeVisible(btnFollow);
    btnFollow.setClickingTogglesState(false);
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

    content.addAndMakeVisible(btnTooltips);
    btnTooltips.setLookAndFeel(&tinyBtnLNF);
    btnTooltips.setClickingTogglesState(false);
    btnTooltips.setTooltip("Tooltips: turn these hover help bubbles ON or OFF everywhere. On by default; "
                           "turn OFF once you know your way around.");
    btnTooltips.onClick = [this] { tooltipsOn = ! tooltipsOn; applyTooltipsSetting(); };
    applyTooltipsSetting();

    content.addAndMakeVisible(btnClearPat);
    btnClearPat.setLookAndFeel(&tinyBtnLNF);
    btnClearPat.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a2030));
    btnClearPat.setTooltip("Clear this pattern: disable every step and reset all per-step values (velocity, pan, "
                           "pitch, loop, roll) back to default - across all channels. Undoable.");
    btnClearPat.onClick = [this] {
        auto& pat = proc.sequencer.patterns[currentPattern()];
        for (auto& ch : pat.channels)
            for (int s = 0; s < DrumChannel::MAX_STEPS; ++s) {
                ch.steps[s] = false; ch.stepVel[s] = 1.0f; ch.stepPitch[s] = 0.0f; ch.stepProb[s] = 1.0f;
                ch.stepRoll[s] = 1; ch.stepRollDecay[s] = 0.0f; ch.stepNoteLen[s] = 0.25f; ch.stepPan[s] = 0.0f;
                ch.stepCondLen[s] = 1; ch.stepCondMask[s] = 0;
            }
        stepGrid.update(proc.sequencer, proc.anySolo);
        refreshPatternButtons();
        content.repaint();
    };

    // Channel-count (8 / 16) + pattern-count (16 / 32) TOGGLE BUTTONS in the top bar; the active one highlights.
    // They sit in two labelled boxes ("Channels" / "Patterns", drawn in paintContent) and show just the numbers.
    for (auto* b : { &btnCh8, &btnCh16, &btnPat16, &btnPat32 }) {
        content.addAndMakeVisible(*b); b->setLookAndFeel(&tinyBtnLNF);
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    }
    for (auto* l : { &lblChannels, &lblNumPat }) {
        content.addAndMakeVisible(*l); l->setFont(juce::Font(9.0f, juce::Font::bold));
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, juce::Colour(0xff8090b0));
    }
    btnCh8.setTooltip ("Show 8 channels.");
    btnCh16.setTooltip("Show 16 channels (with the editor hidden you see them all; otherwise scroll).");
    btnPat16.setTooltip("Use 16 patterns.");
    btnPat32.setTooltip("Use 32 patterns - the pattern row then SCROLLS; drag the thin bar just below the patterns to the right.");
    btnCh8.onClick   = [this] { setVisibleChannels(8); };
    btnCh16.onClick  = [this] { setVisibleChannels(16); };
    btnPat16.onClick = [this] { setNumPatterns(16); };
    btnPat32.onClick = [this] { setNumPatterns(32); };

    // Vertical scrollbar for the channel area (shown only when there are more channels than fit).
    content.addAndMakeVisible(channelBar);
    channelBar.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff5a5a80));
    channelBar.setAutoHide(false);
    channelBar.addListener(this);
    // Horizontal scrollbar for the pattern row (shown only with >16 patterns).
    content.addAndMakeVisible(patternBar);
    patternBar.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff5a5a80));
    patternBar.setAutoHide(false);
    patternBar.addListener(this);

    content.addAndMakeVisible(btnKeys);
    btnKeys.setClickingTogglesState(false);
    btnKeys.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnKeys.setTooltip("Keys mode: play the channel SOUNDS from a MIDI keyboard. ALL channels (1-16) respond - each "
                       "triggers when you play its MIDI note (set/seen in the Routing popup; the factory defaults start "
                       "at white keys C2..C3 and continue up the scale) - like hitting its TEST button. While ON, "
                       "incoming MIDI notes play sounds instead of being used to edit the step grid from a pad "
                       "controller. Sound channels only - channels routed to MIDI Out are skipped.");
    btnKeys.onClick = [this] { proc.keysMode.store(! proc.keysMode.load()); refreshKeysButton(); };
    refreshKeysButton();

    content.addAndMakeVisible(btnToggleDetail);
    btnToggleDetail.setLookAndFeel(&tinyBtnLNF);   // smaller font so the text fits + reads
    btnToggleDetail.setClickingTogglesState(false);
    btnToggleDetail.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnToggleDetail.setTooltip("Show/hide the sound-editing panel. Hide it to give the step sequencer the whole window.");
    btnToggleDetail.onClick = [this] {
        detailShown = ! detailShown;
        btnToggleDetail.setButtonText(detailShown ? "HIDE SOUND EDITOR" : "SHOW SOUND EDITOR");
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

    // Per-pattern play options (apply to the current pattern)
    content.addAndMakeVisible(patModeBtn);
    patModeBtn.setLookAndFeel(&dropBtnLNF);   // draws a clean down-triangle on the right
    patModeBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    patModeBtn.onClick = [this] {
        auto& p = proc.sequencer.patterns[currentPattern()];
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
                auto& pp = proc.sequencer.patterns[currentPattern()];
                if      (r == 1)        { pp.playMode = Sequencer::LoopForever; refreshPatternOptions(); }
                else if (r == 2)        askLoopCount("Stop after", pp.repeatTarget, [this](int n) {
                                            auto& q = proc.sequencer.patterns[currentPattern()];
                                            q.playMode = Sequencer::StopAfterN; q.repeatTarget = n; refreshPatternOptions(); });
                else if (r == 3)        { if (pp.chainLen > 0) --pp.chainLen;          // delete the LAST chain entry
                                          if (pp.chainLen == 0) pp.playMode = Sequencer::LoopForever; refreshPatternOptions(); }
                else if (r >= 220000)   { const int pat = r - 220000;
                                          askLoopCount("Play Pattern " + juce::String(pat + 1) + " after how many loops", 2,
                                            [this, pat](int n) {
                                                auto& q = proc.sequencer.patterns[currentPattern()];
                                                if (q.chainLen < Sequencer::CHAIN_MAX) {
                                                    q.chainSeq[q.chainLen] = pat; q.chainLoops[q.chainLen] = n;
                                                    ++q.chainLen; q.playMode = Sequencer::Chain; }
                                                refreshPatternOptions(); }); }
            });
    };

    content.addAndMakeVisible(lblLoopCount);
    lblLoopCount.setFont(juce::Font(10.0f));
    lblLoopCount.setColour(juce::Label::textColourId, juce::Colour(0xff9090b0));
    lblLoopCount.setJustificationType(juce::Justification::centred);

    content.addAndMakeVisible(sliderPatN);
    sliderPatN.setSliderStyle(juce::Slider::IncDecButtons);
    sliderPatN.setRange(1.0, 99.0, 1.0);
    sliderPatN.setValue(2.0, juce::dontSendNotification);
    sliderPatN.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 34, 22);
    sliderPatN.onValueChange = [this] {
        proc.sequencer.patterns[currentPattern()].repeatTarget = (int) sliderPatN.getValue();
        refreshPatternOptions();
    };

    // Step grid
    content.addAndMakeVisible(stepGrid);
    stepGrid.midiLearn = &proc.midiLearn;
    stepGrid.onStepClicked     = [this](int ch, int step) { proc.toggleStep(ch, step); };
    stepGrid.onChannelSelected = [this](int ch) { selectChannel(ch); };
    stepGrid.onStepValueChanged = [this](int ch, int step, int mode, float value) {
        auto& c = proc.sequencer.channel(ch);
        if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
        if (mode == StepGridComponent::ModeVel) {
            c.stepVel[step] = value;                                    // Y = velocity
            if (c.midiOut) c.stepNoteLen[step] = stepGrid.getNoteLen(ch, step);  // X = note length (MIDI only)
        }
        else if (mode == StepGridComponent::ModePitch) c.stepPitch[step] = value;
        else if (mode == StepGridComponent::ModeProb)  c.stepProb[step]  = value;
        else if (mode == StepGridComponent::ModePan)   c.stepPan[step]   = value;   // X = pan -1..+1
        else if (mode == StepGridComponent::ModeRoll) {
            c.stepRoll[step]      = juce::jlimit(1, 6, (int) value);   // Y = ratchet count
            c.stepRollDecay[step] = stepGrid.getRollDec(ch, step);     // X = per-hit ramp (-1..+1)
        }
    };
    stepGrid.onStepCondChanged = [this](int ch, int step, int len, int mask) {
        if (step < 0 || step >= DrumChannel::MAX_STEPS) return;
        auto& c = proc.sequencer.channel(ch);
        c.stepCondLen[step]  = juce::jlimit(1, 5, len);
        c.stepCondMask[step] = mask;
    };
    stepGrid.onInfluenceApply = [this](int ch, int srcStep) {
        auto& c = proc.sequencer.channel(ch);
        if (srcStep < 0 || srcStep >= DrumChannel::MAX_STEPS) return;
        const int mode = stepGrid.editMode;   // copy ONLY the parameter being edited
        for (int s = 0; s < DrumChannel::MAX_STEPS; ++s)
        {
            if (mode == StepGridComponent::ModeVel)        { c.stepVel[s] = c.stepVel[srcStep]; if (c.midiOut) c.stepNoteLen[s] = c.stepNoteLen[srcStep]; }
            else if (mode == StepGridComponent::ModePitch) c.stepPitch[s] = c.stepPitch[srcStep];
            else if (mode == StepGridComponent::ModeProb)  { c.stepCondLen[s] = c.stepCondLen[srcStep]; c.stepCondMask[s] = c.stepCondMask[srcStep]; }
            else if (mode == StepGridComponent::ModePan)   c.stepPan[s]   = c.stepPan[srcStep];
            else if (mode == StepGridComponent::ModeRoll)  { c.stepRoll[s] = c.stepRoll[srcStep]; c.stepRollDecay[s] = c.stepRollDecay[srcStep]; }
        }
    };
    stepGrid.onInfluenceDisarm = [this](int ch) {
        strips[ch].btnInfluence.setToggleState(false, juce::dontSendNotification);
    };

    // Channel strips:  [#] [sound mixes ▾] [TEST] [M] [S] [OV] [steps ▾]

    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        auto& strip = strips[i];
        int ci = i;

        stripMeter[i].horizontal = true;
        content.addAndMakeVisible(stripMeter[i]);

        content.addAndMakeVisible(strip.numBtn);
        strip.numBtn.setLookAndFeel(&tinyBtnLNF);   // small bold font so 2-digit numbers (10-16) fit
        strip.numBtn.setButtonText(juce::String(i + 1));
        strip.numBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
        strip.numBtn.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff20203a));
        strip.numBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3355aa));
        strip.numBtn.onClick = [this, ci] { selectChannel(ci); };
        strip.numBtn.chIndex = i;
        strip.numBtn.onCopyFrom = [this, ci] (int src) { proc.copyChannel(currentPattern(), src, ci); selectChannel(ci); fullRefresh(); };

        strip.btnMute  = std::make_unique<LearnableButton>("M", "p0_ch" + juce::String(i) + "_mute",  proc.midiLearn);
        strip.btnSolo  = std::make_unique<LearnableButton>("S", "p0_ch" + juce::String(i) + "_solo",  proc.midiLearn);

        content.addAndMakeVisible(*strip.btnMute);
        content.addAndMakeVisible(*strip.btnSolo);

        strip.btnMute->setClickingTogglesState(true);
        strip.btnMute->setColour(juce::TextButton::buttonOnColourId, juce::Colours::orange);
        strip.btnMute->onClick = [this, ci] {
            selectChannel(ci);
            proc.sequencer.channel(ci).mute = strips[ci].btnMute->getToggleState();
            proc.requestLaunchpadRefresh();
        };

        strip.btnSolo->setClickingTogglesState(true);
        strip.btnSolo->setColour(juce::TextButton::buttonOnColourId, juce::Colours::yellow);
        strip.btnSolo->onClick = [this, ci] {
            selectChannel(ci);
            proc.sequencer.channel(ci).solo = strips[ci].btnSolo->getToggleState();
            proc.requestLaunchpadRefresh();
        };

        content.addAndMakeVisible(strip.comboSound);  // now the "Sound Bank" selector
        strip.comboSound.setLookAndFeel(&wideMenuLNF); // 3-column popup (no tall scroll)
        strip.comboSound.onChange = [this, ci] { handleSoundMixChange(ci); selectChannel(ci); };

        content.addAndMakeVisible(strip.btnTest);
        strip.btnTest.onClick = [this, ci] { selectChannel(ci); proc.requestTestTrigger(ci); };

        content.addAndMakeVisible(strip.btnPoly);
        strip.btnPoly.setClickingTogglesState(true);
        strip.btnPoly.setLookAndFeel(&tinyBtnLNF);
        strip.btnMute->setLookAndFeel(&tinyBtnLNF);
        strip.btnSolo->setLookAndFeel(&tinyBtnLNF);
        strip.btnPoly.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff35b56a));
        strip.btnPoly.setTooltip("Overlap: lets a sound keep ringing into the next step instead of being cut off - but only if the sound is actually long enough to ring (a short sound still ends on its own). Off = each trigger restarts the sound.\n\nRight-click to assign a MIDI control.");
        strip.btnPoly.midiLearn = &proc.midiLearn;   // paramId set per-pattern in updateStripParamIds()
        strip.btnPoly.onClick = [this, ci] {
            selectChannel(ci);
            proc.sequencer.channel(ci).allowOverlap = strips[ci].btnPoly.getToggleState();
        };

        content.addAndMakeVisible(strip.btnInfluence);
        strip.btnInfluence.setClickingTogglesState(true);
        strip.btnInfluence.setLookAndFeel(&tinyBtnLNF);
        strip.btnInfluence.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb96bff));
        strip.btnInfluence.setTooltip("Influence: arm, then in a Vel/Pitch/Prob/Roll mode touch ONE step - the parameter being edited is copied from that step onto every step in this channel. It un-arms after that, so you can still tweak individual steps afterwards. Re-arm to copy from a different step.\n\nRight-click to assign a MIDI control.");
        strip.btnInfluence.midiLearn = &proc.midiLearn;
        strip.btnInfluence.paramId   = "ui_influence_ch" + juce::String(i); // UI state (not per-pattern)
        strip.btnInfluence.onClick = [this, ci] {
            selectChannel(ci);
            stepGrid.influenceArmed[ci] = strips[ci].btnInfluence.getToggleState();
        };

        content.addAndMakeVisible(strip.comboSteps);
        for (int si = 0; si < DrumChannel::NUM_VALID_STEP_COUNTS; ++si)
        {
            int s = DrumChannel::VALID_STEP_COUNTS[si];
            strip.comboSteps.addItem(juce::String(s) + (s == 1 ? " step" : " steps"), s);
        }
        strip.comboSteps.setSelectedId(proc.sequencer.channel(i).numSteps, juce::dontSendNotification);
        strip.comboSteps.onChange = [this, ci] {
            selectChannel(ci);
            proc.sequencer.channel(ci).numSteps = strips[ci].comboSteps.getSelectedId();
            proc.requestLaunchpadRefresh();
        };

        strip.numBtn.setTooltip("Channel number - click to select + edit its sound below. DRAG it onto another channel's "
                                "number to copy this channel's whole sound + steps there (that channel keeps its own routing). "
                                "The COLOUR shows its routing (set via the top-bar Routing button or the CHANNEL box's Output "
                                "dropdown): dark = normal (plays into the Main mix); TEAL = sent to its own aux Output for a "
                                "separate DAW track; PURPLE = MIDI Out (makes no sound, sends MIDI notes to sequence another plugin).");
        strip.comboSound.setTooltip("Load a saved 'sound mix' onto this channel, or start a fresh one.");
        strip.btnTest.setTooltip("Play this channel once with its current settings, to hear it without running the sequencer.");
        strip.btnMute->setTooltip("Mute: silence this channel.");
        strip.btnSolo->setTooltip("Solo: play only this channel (and other soloed ones), muting the rest.");
        strip.comboSteps.setTooltip("Number of steps in this channel's pattern. The bar is split into this many even slices.");
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
    setupGroupHeader(hdrDelayG,    "Delay");
    setupGroupHeader(hdrMasterOut, "MASTER");   // now a sub-header inside the SOUND BLEND box (Pattern Output group removed)

    // Compact value formatters (shown always under each knob)
    auto fmtMs   = [](double v){ return v < 1.0 ? juce::String(juce::roundToInt(v * 1000.0)) + "ms"
                                                : juce::String(v, 2) + "s"; };
    auto fmtPct  = [](double v){ return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
    auto fmtDb   = [](double v){ bool whole = std::abs(v - std::round(v)) < 0.05;
                                 juce::String s = whole ? juce::String((int) std::round(v)) : juce::String(v, 1);
                                 return (v >= 0 ? "+" : "") + s + "dB"; };
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
    setupKnob(knobVolume,  lblVol, "Volume",  0.0,   1.25,  1.0,   1.0, fmtPct);  // up to 125%, default 100%
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
    comboDriveType.addItem("Off",       1);
    comboDriveType.addItem("Soft Clip", 2);
    comboDriveType.addItem("Hard Clip", 3);
    comboDriveType.addItem("Tube",      4);
    comboDriveType.addItem("Foldback",  5);
    comboDriveType.addItem("Fuzz",      6);
    comboDriveType.addItem("Bitcrush",  7);
    comboDriveType.onChange = [this] {   // per-slot drive type
        if (ignoreKnobCallbacks) return;
        proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxDriveType = comboDriveType.getSelectedId() - 1;
    };

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
        comboMidiNote.setEnabled(midi);
        refreshRouting();
    };

    // The MIDI note this channel SENDS when routed to MIDI Out (channel-wide). Default = its
    // GM drum note. Step Pitch transposes it; step velocity + Roll drive the note dynamics.
    content.addAndMakeVisible(comboMidiNote);
    content.addAndMakeVisible(lblMidiNote);
    lblMidiNote.setText("MIDI note", juce::dontSendNotification);
    lblMidiNote.setFont(juce::Font(9.0f)); lblMidiNote.setJustificationType(juce::Justification::centred);
    lblMidiNote.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    for (int n = 0; n <= 127; ++n)
        comboMidiNote.addItem(juce::MidiMessage::getMidiNoteName(n, true, true, 4) + " (" + juce::String(n) + ")", n + 1);
    comboMidiNote.setTooltip("The MIDI note this channel sends in MIDI Out mode (e.g. C1 = 36). "
                             "Set it to whatever note the target plugin/track expects (a drum pad, or a pitch for a synth). "
                             "Per-step Pitch transposes it, step velocity + Roll set the note's dynamics. Channel-wide (all patterns).");
    comboMidiNote.onChange = [this] {
        if (ignoreKnobCallbacks) return;
        const int note = juce::jlimit(0, 127, comboMidiNote.getSelectedId() - 1);
        for (auto& pat : proc.sequencer.patterns) pat.channels[selectedChannel].midiNote = note;
    };

    // Top-bar "Routing" button: an at-a-glance overview to wire every channel (1-8) to Main,
    // an aux Out, or MIDI Out - all in one popup. Highlights purple when any channel is MIDI.
    content.addAndMakeVisible(btnRoute);
    btnRoute.setLookAndFeel(&dropBtnLNF);   // draws a down-triangle (it's a dropdown)
    btnRoute.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    btnRoute.setTooltip("Routing overview - wire every channel at once: Main (normal mix), its own aux Output "
                        "(a separate DAW track - the DAW routes 'Out N' to a track), or MIDI Out -> a note (sequences "
                        "another plugin; the DAW routes this plugin's MIDI output to your synth/sampler). No internal "
                        "sound when on MIDI. Each channel shows two MIDI lines: 'MIDI Out (note)' switches it on with its "
                        "current note, and 'Change MIDI Out note' picks a new one. The MIDI note's LENGTH = this channel's "
                        "amp-envelope length (shape it on the AMP ENVELOPE graph). Channels are colour-coded in the strip: "
                        "purple = MIDI, teal = aux out. Channel-wide (applies to all patterns).");
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
            // Two MIDI lines: (1) route to MIDI out keeping the current note; (2) change the note.
            sub.addItem(ch * 100 + 50,
                        "MIDI Out (" + juce::MidiMessage::getMidiNoteName(c.midiNote, true, true, 4) + ")",
                        true, c.midiOut);
            juce::PopupMenu notes;
            for (int n = 0; n <= 127; ++n)
                notes.addItem(100000 + ch * 200 + n,
                              juce::MidiMessage::getMidiNoteName(n, true, true, 4) + " (" + juce::String(n) + ")",
                              true, c.midiOut && c.midiNote == n);
            sub.addSubMenu("Change MIDI Out note", notes);
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
            choke.addItem(300000 + ch * 100 + 0, "Off (no choke)", true, c.chokeGroup == 0);
            for (int g = 1; g <= 8; ++g)
                choke.addItem(300000 + ch * 100 + g, "Group " + juce::String(g), true, c.chokeGroup == g);
            sub.addSubMenu("Choke group" + juce::String(c.chokeGroup > 0 ? " (" + juce::String(c.chokeGroup) + ")" : ""), choke);
            const juce::String tag = c.midiOut ? "  [MIDI " + juce::MidiMessage::getMidiNoteName(c.midiNote, true, true, 4) + " ch" + juce::String(c.midiOutChannel) + "]"
                                               : (c.outputBus > 0 ? "  [Out " + juce::String(c.outputBus)
                                                     + ": ch " + juce::String(c.outputBus * 2 + 1) + "/" + juce::String(c.outputBus * 2 + 2) + "]" : "");
            menu.addSubMenu("Channel " + juce::String(ch + 1) + tag, sub);
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(btnRoute), [this](int r) {
            if (r <= 0) return;
            if (r >= 500000) {                         // "Sound -> Main" (dedicated id so it's never 0/unclickable)
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

    setupKnob(knobReverb,  lblRev, "Reverb",  0.0,   1.0,   0.0,   1.0, fmtPct);
    setupKnob(knobDelay,   lblDel, "Delay",   0.0,   1.0,   0.0,   1.0, fmtPct);
    setupKnob(knobReverbRoom,  lblRevRoom,  "Size",  0.0, 1.0,  0.5,   1.0, fmtPct);
    setupKnob(knobReverbDecay, lblRevDecay, "Decay", 0.0, 1.0,  0.5,   1.0, fmtPct);
    setupKnob(knobReverbWet,   lblRevWet,   "Wet",   0.0, 1.0,  0.4,   1.0, fmtPct);
    setupKnob(knobReverbPre,   lblRevPre,   "Pre",   0.0, 1.0,  0.0,   1.0,
              [](double v){ return juce::String(juce::roundToInt(v * 120.0)) + " ms"; });   // pre-delay 0..120 ms (not %)
    setupKnob(knobReverbWidth, lblRevWidth, "Width", 0.0, 1.0,  1.0,   1.0, fmtPct);
    setupKnob(knobDelayTime,  lblDelTime, "Time", 0.05, 2.0, 0.375, 1.0, fmtMs);
    setupKnob(knobDelayFB,    lblDelFB,   "Feedback", 0.0, 0.95, 0.3, 1.0, fmtPct);
    knobDelayTime.setSkewFactorFromMidPoint(0.3);
    setupKnob(knobMasterVol,   lblMasterVol,   "Volume", 0.0, 1.0,  0.9, 1.0, fmtPct);
    // Master VOLUME is a horizontal FADER now (lives in the SOUND BLEND box; Pattern Output group removed).
    knobMasterVol.setSliderStyle(juce::Slider::LinearHorizontal);
    knobMasterVol.setTextBoxStyle(juce::Slider::TextBoxRight, true, 38, 16);
    knobMasterVol.setColour(juce::Slider::trackColourId,      juce::Colour(0xffffc24a)); // filled (left of thumb) = amber
    knobMasterVol.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff2a2a44)); // empty (right) = dark
    setupKnob(knobMasterPan,   lblMasterPan,   "Pan",   -1.0, 1.0,  0.0, 1.0, fmtPan);   // (Pan removed from the UI)
    // Limiter read-out = the output CEILING in dB (concrete) instead of a meaningless %. "Off" at 0.
    setupKnob(knobMasterLimit, lblMasterLimit, "Limit",  0.0, 1.0,  0.003, 1.0,
              [](double v){ return v <= 0.0005 ? juce::String("Off") : juce::String(-0.1 - v * 11.9, 1) + " dB"; });
    knobMasterLimit.setSkewFactorFromMidPoint(0.12);   // start slower at the light end so -0.1..-1.5 dB is easy to dial
    setupKnob(knobMasterGlue,  lblMasterGlue,  "Glue",   0.0, 1.0,  0.0,  1.0, fmtPct);  // 0 = off; master bus compressor

    for (auto& m : masterMeter) { m.horizontal = false; content.addAndMakeVisible(m); }
    masterMeter[0].setTooltip ("Master output level (L / R), post everything. Green ok - amber hot - red = clipping.");
    masterMeter[1].setTooltip ("Master output level (L / R), post everything. Green ok - amber hot - red = clipping.");

    knobPitch.onValueChange   = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).sampleCrush = (float)knobPitch.getValue(); };
    knobVolume.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).volume  = (float)knobVolume.getValue(); };
    knobPan.onValueChange     = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).pan     = (float)knobPan.getValue(); };
    knobSlices.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].smpSlices = juce::jlimit(1, 16, (int)knobSlices.getValue()); };
    knobCutoff.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).filterCutoff = (float)knobCutoff.getValue(); };
    knobReso.onValueChange    = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).filterReso   = (float)knobReso.getValue(); };
    knobEnvAmt.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).filterEnvAmt = (float)knobEnvAmt.getValue(); };
    knobDrive.onValueChange   = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxDrive       = (float)knobDrive.getValue(); };
    knobReverb.onValueChange  = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxReverbSend = (float)knobReverb.getValue(); };
    knobDelay.onValueChange   = [this] { if (!ignoreKnobCallbacks)   proc.sequencer.channel(selectedChannel).slots[envTargetSlot()].fxDelaySend  = (float)knobDelay.getValue(); };
    // Reverb/Delay FLAVOUR is MASTER-level now (write ALL patterns), so it's one shared sound for the whole kit.
    auto allM = [this](std::function<void(Sequencer::MasterFX&)> fn) {
        if (ignoreKnobCallbacks) return; for (auto& p : proc.sequencer.patterns) fn(p.master); };
    knobReverbRoom.onValueChange  = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.reverbRoom = (float)knobReverbRoom.getValue(); }); };
    knobReverbDecay.onValueChange = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.reverbDamp = 1.0f - (float)knobReverbDecay.getValue(); }); };
    knobReverbWet.onValueChange   = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.reverbWet = (float)knobReverbWet.getValue(); }); };
    knobReverbPre.onValueChange   = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.reverbPreDelay = (float)knobReverbPre.getValue(); }); };
    knobReverbWidth.onValueChange = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.reverbWidth = (float)knobReverbWidth.getValue(); }); };
    knobDelayFB.onValueChange     = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.delayFeedback = (float)knobDelayFB.getValue(); }); };
    knobMasterVol.onValueChange   = [this] { if (!ignoreKnobCallbacks) proc.masterFX().volume = (float)knobMasterVol.getValue(); };
    knobMasterPan.onValueChange   = [this] { if (!ignoreKnobCallbacks) proc.masterFX().pan    = (float)knobMasterPan.getValue(); };
    knobMasterLimit.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.masterFX().limit  = (float)knobMasterLimit.getValue(); };
    // Glue is a MASTER bus compressor (one shared setting for the whole kit) -> write ALL patterns, like the FX flavour.
    knobMasterGlue.onValueChange  = [this, allM] { allM([this](Sequencer::MasterFX& m){ m.glue = (float)knobMasterGlue.getValue(); }); };

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
        for (auto& p : proc.sequencer.patterns) { p.master.delayTime = (float) v; if (p.master.delaySync) p.master.delayDivision = div; }
    };

    //-- Sample pitch envelope (knobs live in the "Pitch & Level" group now)
    setupKnob(knobPEnvAmt,  lblPEnvAmt,  "P.Env",  -48.0, 48.0, 0.0,  1.0, fmtSemi);
    setupKnob(knobPEnvTime, lblPEnvTime, "P.Time", 0.001, 1.0,  0.05, 0.3, fmtMs);
    knobPEnvAmt.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).pitchEnvAmt  = (float)knobPEnvAmt.getValue(); };
    knobPEnvTime.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).pitchEnvTime = (float)knobPEnvTime.getValue(); };

    //-- Sounds section: 4 source toggle-switches + 2D blend pad + per-source knobs
    setupGroupHeader(hdrSounds, "MASTER");
    hdrSounds.setColour(juce::Label::textColourId, juce::Colour(0xffffc24a));   // amber = this box is global (master), not per-channel
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
                       [this]() { proc.sequencer.channel(selectedChannel).markDspDirty(); });
        // "Auto" toggle ON: releasing a slot knob fires a TEST hit so you hear the edit (default OFF).
        slotEd[b].onAudition = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
        // Sample Stretch was changed -> re-bake that slot's buffer (SoundTouch) + refresh the waveform.
        slotEd[b].onSampleEdit = [this](int box) {
            auto& ch = proc.sequencer.channel(selectedChannel);
            ch.updateStretch(box); cacheWaveform(selectedChannel);
        };
        // Per-slot accent: Slot 1 = yellow, Slot 2 = pink (knobs + faders).
        slotEd[b].setAccent(b == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8));
    }
    content.addAndMakeVisible(lblPadHint);
    lblPadHint.setText("Drag the yellow dot to\nadjust sound levels.", juce::dontSendNotification);
    lblPadHint.setFont(juce::Font(12.5f)); lblPadHint.setJustificationType(juce::Justification::centred);
    lblPadHint.setColour(juce::Label::textColourId, juce::Colour(0xff90a0c0));

    // Channel A-H-D-S-R envelope editor + which-slots dropdown (in the SOUND BLEND box).
    // Shared shape editors (amp env + pitch + voice), all driven by one selected slot.
    content.addAndMakeVisible(envEditor);
    // Auto-audition also for the VISUAL editors (amp/pitch/voice) - fire a TEST hit on drag-release, like knobs do.
    auto auditionEnd = [this] { if (proc.auditionOnEdit.load()) proc.requestTestTrigger(selectedChannel); };
    envEditor.onChange = [this](float a, float h, float d, float s, float r) {
        if (ignoreKnobCallbacks) return; applyEnvToTargets(a, h, d, s, r); };
    envEditor.onDragEnd = auditionEnd;
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
    setupGroupHeader(hdrPitch, "PITCH ENVELOPE");
    setupGroupHeader(hdrVoice, "UNISON / DETUNE / VIBRATO");   // sub-title above the voice visual
    setupGroupHeader(hdrBlend2, "SOUND BLEND");
    setupGroupHeader(hdrAmpEnv, "AMP ENVELOPE");
    setupGroupHeader(hdrEqBox,  "EQ");
    // === PER-SLOT EQ (begin) - target picker (All / 1 / 2 / 3) ===
    slotSelEq.labels = { "All", "1", "2" };
    content.addAndMakeVisible(slotSelEq);
    slotSelEq.onSelect = [this](int s) { if (ignoreKnobCallbacks) return; eqEditTarget = s; refreshEqTarget(); };
    // === PER-SLOT EQ (end) ===
    setupGroupHeader(lblShapeSlot, "EDIT SLOT"); setupGroupHeader(lblPitchSlot, "EDIT SLOT");

    // Unison / Detune / Vibrato as an interactive VISUAL (like the amp/pitch env editors), editing the
    // selected slot. The 1/2/3 selector (slotSelPitch) above it chooses which slot.
    content.addAndMakeVisible(voiceMod);
    voiceMod.onChange = [this](int u, float d, float v, bool centre, int detuneMode) {
        if (ignoreKnobCallbacks) return;
        auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
        sl.oscUnison = u; sl.oscDetune = d; sl.vibrato = v; sl.oscUniCenter = centre; sl.oscDetuneMode = detuneMode;
        proc.sequencer.channel(selectedChannel).markDspDirty();
    };

    static const char* srcNames[5] = { "Sample", "Noise", "Analog", "FM", "Physical" };
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
        "Blend layout A / B (works with 4 or 5 sources on). On the blend shape, sources on OPPOSITE "
        "corners can never both be loud at once, so some mixes are impossible in layout A. Layout B "
        "re-arranges which sources are neighbours - the square swaps its two top corners; the pentagon "
        "uses a star order - so a pair that was opposite in A becomes adjacent and you can push both up "
        "together. It does not change the sound on its own, only which blends are reachable.");

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
    }
    // PITCH (semitones) = transpose the WHOLE channel - works for every engine (synth freq + sample
    // varispeed), applied via vPitchMul in the render. Same unit as the pitch envelope. Per-channel.
    setupKnob(knobSpeed, lblSpeed, "Pitch", -24.0, 24.0, 0.0, 1.0,
              [](double v){ return (v > 0 ? "+" : "") + juce::String(juce::roundToInt(v)) + " st"; });
    // Pitch lives in the PITCH ENVELOPE box now (grouped with pitch), as a horizontal fader.
    knobSpeed.setSliderStyle(juce::Slider::LinearHorizontal);
    knobSpeed.setTextBoxStyle(juce::Slider::TextBoxRight, true, 44, 16);
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
    setupGroupHeader(hdrOscG,   "ANALOG");
    setupGroupHeader(hdrNoiseG, "NOISE");
    setupGroupHeader(hdrFmG,    "FM");
    setupGroupHeader(hdrPhysG,  "PHYSICAL");

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
    setupKnob(knobBloom,  lblBloom,  "Bloom",  0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobDrift,  lblDrift,  "Drift",  0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobSpread, lblSpread, "Spread", 0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobPunch,  lblPunch,  "Punch",  0.0, 1.0, 0.0, 1.0, fmtPct);
    setupKnob(knobGlue,   lblGlue,   "Glue",   0.0, 1.0, 0.0, 1.0, fmtPct);
    knobBloom.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).bloom  = (float)knobBloom.getValue(); };
    knobDrift.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).drift  = (float)knobDrift.getValue(); };
    knobSpread.onValueChange = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).spread = (float)knobSpread.getValue(); };
    knobPunch.onValueChange  = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).punch  = (float)knobPunch.getValue(); };
    knobGlue.onValueChange   = [this] { if (!ignoreKnobCallbacks) proc.sequencer.channel(selectedChannel).glue   = (float)knobGlue.getValue(); };
    knobBloom.setTooltip(
        "Bloom: at the start of each hit only the loudest source plays, then the rest fade in over a few "
        "tens of ms - so the hit begins focused/punchy and 'blooms' into the full layered blend (e.g. a "
        "noisy attack opening into a tonal body). 0% = off. Needs at least 2 sources on.");
    knobDrift.setTooltip(
        "Drift: slowly moves the blend over time so the timbre keeps gently evolving bar to bar instead "
        "of being perfectly static. 0% = off. Needs at least 2 sources on.");
    knobSpread.setTooltip(
        "Spread: widens the synth sources (Noise/Analog/FM/Physical) across the stereo field - the blend "
        "gets wider and more three-dimensional. The Sample keeps its own stereo image. 0% = mono/centred.");
    knobPunch.setTooltip(
        "Punch: a transient shaper on the combined hit - boosts the initial attack so the sound hits "
        "harder and snappier. Great for kicks and snares. 0% = off.");
    knobGlue.setTooltip(
        "Glue: gentle compression that fuses the blended sources into one cohesive hit (with automatic "
        "make-up gain). Adds weight and consistency without distortion. 0% = off.");

    // Master FX: Delay Sync toggle + Save Mix (header) + Master Output Mono toggle
    content.addAndMakeVisible(lblDelaySync);
    lblDelaySync.setText("Sync", juce::dontSendNotification);
    lblDelaySync.setFont(juce::Font(8.5f)); lblDelaySync.setJustificationType(juce::Justification::centred);
    lblDelaySync.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    content.addAndMakeVisible(swDelaySync);
    swDelaySync.onClick = [this] { if (!ignoreKnobCallbacks) { const bool on = swDelaySync.getToggleState();
        for (auto& p : proc.sequencer.patterns) p.master.delaySync = on; knobDelayTime.updateText(); } };

    content.addAndMakeVisible(lblDelayPingPong);
    lblDelayPingPong.setText("Ping", juce::dontSendNotification);
    lblDelayPingPong.setFont(juce::Font(8.5f)); lblDelayPingPong.setJustificationType(juce::Justification::centred);
    lblDelayPingPong.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    content.addAndMakeVisible(swDelayPingPong);
    swDelayPingPong.setTooltip("Ping-Pong: echoes bounce L<->R across the stereo field instead of repeating in place. "
                               "Great for width on hats/percussion. Needs the channel's Delay send up.");
    swDelayPingPong.onClick = [this] { if (!ignoreKnobCallbacks) { const bool on = swDelayPingPong.getToggleState();
        for (auto& p : proc.sequencer.patterns) p.master.delayPingPong = on; } };

    content.addAndMakeVisible(lblMasterMono);
    lblMasterMono.setText("Stereo/Mono", juce::dontSendNotification);   // off(left)=Stereo is the default; on=Mono
    lblMasterMono.setFont(juce::Font(9.5f)); lblMasterMono.setJustificationType(juce::Justification::centred);
    lblMasterMono.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    content.addAndMakeVisible(swMasterMono);
    swMasterMono.onClick = [this] { if (!ignoreKnobCallbacks) proc.masterFX().mono = swMasterMono.getToggleState(); };

    content.addAndMakeVisible(btnSaveMix);
    btnSaveMix.setLookAndFeel(&tinyBtnLNF);   // smaller font so the long text fits + reads
    btnSaveMix.onClick = [this] { saveSoundMix(); };
    btnSaveMix.setTooltip("Save this channel's current sound as a new, reusable entry in the Sound Bank.");

    //-- Beginner-friendly tooltips on hover (a TooltipWindow shows them).
    btnDawSync.setTooltip("When sync is enabled, BPM and time signature are taken from the project. "
                          "And play/stop functions will also be controlled by the DAW.");
    sliderBpm.setTooltip("Tempo in beats per minute. Sets how fast the pattern plays (only editable when DAW Sync is off).");
    sliderSwing.setTooltip("Swing (per pattern) adds groove by delaying every other step slightly, so the rhythm feels less robotic.");
    barSigX.setTooltip("Top number of the time signature: how many beats are in one bar. Click to type a value.");
    barSigY.setTooltip("Bottom number of the time signature: which note value counts as one beat. Click to type a value.");
    lblBarResult.setTooltip("How many seconds one full bar lasts, from the BPM and time signature. One pattern = one bar.");
    btnPlay.setTooltip("Start playback (used when DAW Sync is off, so the plugin runs on its own).");
    btnStop.setTooltip("Stop playback (used when DAW Sync is off).");
    comboPreset.setTooltip("Save or load a whole-kit preset: all channels, patterns and settings at once.");
    dragMidi.setTooltip("Drag this onto a track in your DAW to export the current pattern as a MIDI clip.");
    patModeBtn.setTooltip("What happens after this pattern finishes: loop forever, stop after N loops, or jump to another pattern.");
    sliderPatN.setTooltip("How many times the pattern loops before the chosen action (stop / go to another pattern) happens.");

    freqDisplay.setTooltip("Live spectrum (the frequencies of what's playing) + the EQ/filter curve. The spectrum "
                           "refreshes once per step, so channels with more steps show the frequencies at a higher resolution.\n\n"
                           "Boosting EQ bands makes the channel louder, which adds up across all channels. If the "
                           "final mix gets too hot, some DAWs (Reaper included) clip or auto-mute the track. Cut "
                           "other bands, lower the channel/master Volume, or raise the master Limit knob to keep it safe.");
    soundPad.setTooltip("Drag the yellow dot to blend the enabled sound sources. Closer to a corner = more of that source.");
    comboSampleSel.setTooltip("Pick a sample for this channel from your samples folder (or load a new one).");
    knobSpeed.setTooltip("PITCH: transpose the whole channel in semitones - every engine. Synths shift frequency; "
                         "samples are PITCH-SHIFTED (high-quality SoundTouch) so the length stays the same. "
                         "Change length without pitch = Stretch. (The per-step grid Pitch + the pitch ENVELOPE are varispeed.)");
    knobLayNoiseType.setTooltip("Noise colour: White (bright/hissy), Pink (softer), Brown (deep/rumbly), Grey (balanced), Purple (very bright/airy).");
    swDelaySync.setTooltip("Lock the delay time to the tempo (note values like 1/8, 1/4) instead of free milliseconds.");
    swMasterMono.setTooltip("Switch the final output between Stereo (off) and Mono (on, both speakers identical).");
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
    knobReverb.setTooltip("How much of this channel is sent to the shared reverb (set its size/decay in Pattern FX).");
    knobDelay.setTooltip("How much of this channel is sent to the shared delay (echo).");
    knobReverbRoom.setTooltip("Reverb size - small room to large hall.");
    knobReverbDecay.setTooltip("How long the reverb tail lasts before fading away.");
    knobReverbWet.setTooltip("Overall reverb amount (how loud the reverb is in the mix).");
    knobReverbPre.setTooltip("Pre-delay: a short gap (0-120 ms) before the reverb tail starts, so the dry hit is heard "
                             "first and the tail blooms after. Adds clarity + size to drums. 0 = no gap.");
    knobReverbWidth.setTooltip("Stereo width of the reverb tail. 1 = full wide, lower = narrower (toward mono).");
    knobDelayTime.setTooltip("Time between echoes. With Sync on it snaps to note values; otherwise it's free milliseconds.");
    knobDelayFB.setTooltip("Delay feedback - how many times the echo repeats. Higher = more repeats.");
    knobMasterVol.setTooltip("MASTER output volume (the final fader, per pattern).");
    knobMasterPan.setTooltip("(unused - master pan was removed)");
    knobMasterLimit.setTooltip("MASTER output limiter. The read-out is the output CEILING in dB - peaks are held just "
                               "below this level so loud EQ/volume boosts can't make your DAW mute or clip. 'Off' = no "
                               "limiting. A light ceiling (-0.1 to -1 dB) just catches stray peaks transparently; lower "
                               "it (toward -12 dB) to squash peaks harder + push the overall level up.");
    knobMasterGlue.setTooltip("GLUE - a master 'bus compressor' that gently squeezes the WHOLE kit together so the "
                              "separate hits feel like one punchy, cohesive groove (the subtle 'pump' on produced drum "
                              "loops). At 0% it's OFF. Turn it up for more glue + loudness + punch. It sits BEFORE the "
                              "Limiter and reacts to both channels equally, so your stereo image stays put. "
                              "Optional finishing touch - not needed for every sound; try it on the full pattern.");
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
void DrumSequencerEditor::syncBoxesFromSrcOn()
{
    auto& ch = proc.sequencer.channel(selectedChannel);
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)
    {
        boxEngine[b] = ch.slots[b].engine;
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
        sampleSub.addItem(ID_LOAD_SAMPLE, "Load Sample...");      // "Browse Library..." removed (redundant with Load + the inline list)
        sampleSub.addItem(ID_OPEN_FOLDER, "Open Samples Folder");
        root->addSubMenu("Sample", sampleSub);
        root->addItem(3, "Noise"); root->addItem(4, "Analog + FM");   // Depth 0 = analog, raise it for FM (resonator removed)
        root->addItem(6, "Physical");   // "FM"/"Synth"/"Wavetable" retired from the menu (kept parseable for old
                                        // projects); the Oscillator now covers wavetable-style shaping (more shapes + Warp).
        root->addItem(9, "Modal");      // id = engine+2 -> SrcModal (7); maps automatically (struck resonant body)
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
    static const char* eng[DrumChannel::NUM_SOURCES + 3] = { "Sample", "Noise", "Analog + FM", "FM", "Physical", "Synth", "Wavetable", "Modal" };
    bool a[DrumChannel::NUM_SLOTS];
    int total[DrumChannel::NUM_SOURCES + 3] = {}, seen[DrumChannel::NUM_SOURCES + 3] = {};
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
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b) ch.slots[b].weight = soundPad.weights[b];
    ch.padX = soundPad.dotX; ch.padY = soundPad.dotY;
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
    slotSelAmp.sel = slotSelPitch.sel = slotSelVoice.sel = slotSelFx.sel = s;
    proc.sequencer.channel(selectedChannel).envEditMode = s + 1;   // remembered per channel
    slotSelAmp.repaint(); slotSelPitch.repaint(); slotSelVoice.repaint(); slotSelFx.repaint();
    content.repaint();   // redraw the slot-box highlight (the selected slot is emphasised)
    loadEnvIntoEditor();
    loadPitchAndVoice();
}

// Show the selected slot's amp envelope in the graph.
void DrumSequencerEditor::loadEnvIntoEditor()
{
    const auto& s = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    const bool isModal = (s.engine == DrumChannel::SrcModal);
    // Modal: the Ring handle IS the modal decay (modalDecay 0..1 -> 0.05..4 s, the DSP's mapping), shown in seconds.
    if (isModal) envEditor.setValues(s.atk, 0.0f, 0.05f + s.modalDecay * 3.95f, 0.0f, 0.0f);
    else         envEditor.setValues(s.atk, s.hold, s.dec, s.sustain, s.release);
    // The amp envelope now applies to every engine except Sample (which plays its own length). Modal used to be
    // greyed; it now has the Strike/Ring envelope (Strike = a soft onset, Ring = the mode decay).
    const bool ampApplies = (s.engine != DrumChannel::SrcSample);
    envEditor.setNa("AMP ENVELOPE (n/a - sample)", "sample plays full length");
    envEditor.setEnabledLook(ampApplies);
    // Physical + Modal (both struck/plucked) get a tailored 2-handle Strike(onset softness)/Ring(decay) editor - no
    // Hold/Sustain (they don't fit a struck body). For Physical the Strike sustains the string so a slow strike hits full.
    envEditor.setStrikeRing(s.engine == DrumChannel::SrcPhys || isModal);
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
    return sl.atk + sl.hold + sl.dec;   // AHD perceptual length (matches the amp-env "length" read-out)
}

// Show the selected slot's pitch envelope + Unison/Detune/Vibrato in their controls.
void DrumSequencerEditor::loadPitchAndVoice()
{
    auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    // Pitch env applies to every engine except Noise (no pitch), Modal (its resonators can't be swept per-sample),
    // and empty slots. (Modal still follows per-STEP + channel pitch - just not the envelope sweep.)
    const bool hasPitch = (sl.engine >= 0 && sl.engine != DrumChannel::SrcNoise && sl.engine != DrumChannel::SrcModal);
    pitchEditor.setEnabledLook(hasPitch);
    pitchEditor.setDots(sl.pEnvP, sl.pEnvT);
    // X-axis = the sound's length (sample length for samples, else amp env) so the dots/playhead stay synced.
    pitchEditor.setLengthSec(pitchEnvLenSec(envTargetSlot()));
    const juce::ScopedValueSetter<bool> guard(ignoreKnobCallbacks, true);
    // Voice visual: Unison/Detune = oscillator engines only (Analog+FM / Synth); Vibrato = those + Physical
    // + Sample (varispeed). Noise has no pitch; Modal is fixed-pitch -> say so, like the pitch-env n/a.
    const int e = sl.engine;
    const bool uniOn = (e == DrumChannel::SrcOsc || e == DrumChannel::SrcFM || e == DrumChannel::SrcSynth);
    const bool vibOn = uniOn || e == DrumChannel::SrcPhys || e == DrumChannel::SrcSample;
    juce::String naReason;
    if (!uniOn && !vibOn)
        naReason = (e == DrumChannel::SrcNoise)  ? "(n/a - noise has no pitch)"
                 : (e == DrumChannel::SrcModal)  ? "(n/a - Modal is fixed-pitch)"
                 : "(no engine)";
    voiceMod.setValues((int) sl.oscUnison, sl.oscDetune, sl.vibrato, sl.oscUniCenter, sl.oscDetuneMode);
    voiceMod.setSupport(uniOn, vibOn, naReason);
}

// Apply the dragged amp envelope to the selected slot (live; no coeff rebuild needed).
void DrumSequencerEditor::applyEnvToTargets(float a, float h, float d, float s, float r)
{
    auto& sl = proc.sequencer.channel(selectedChannel).slots[envTargetSlot()];
    if (sl.engine == DrumChannel::SrcModal) {   // Ring handle (seconds) -> modalDecay (0..1; inverse of 0.05..4 s)
        sl.atk = a; sl.modalDecay = juce::jlimit(0.0f, 1.0f, (d - 0.05f) / 3.95f);
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

    if (id == ID_BROWSE)
    {
        openSoundBrowser(box);
        syncBoxesFromSrcOn();   // restore this combo's display (the action isn't an engine)
        return;
    }
    if (id == ID_OPEN_FOLDER)
    {
        getSamplesFolder().revealToUser(); rescanSamples(); rebuildSampleMenu();
        syncBoxesFromSrcOn();   // restore this combo's display (the action isn't an engine)
        return;
    }
    if (id == ID_LOAD_SAMPLE)
    {
        boxEngine[box] = DrumChannel::SrcSample; ch.slots[box].engine = DrumChannel::SrcSample;
        fileChooser = std::make_unique<juce::FileChooser>("Load Sample", getSamplesFolder(), kAudioWildcard);
        const int chN = selectedChannel;
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, chN, box](const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f.existsAsFile()) {
                    // Load IN PLACE (no copy). Only samples that live in the samples folder appear in the dropdown;
                    // one loaded from elsewhere just plays in this slot (it isn't added to the library).
                    proc.sequencer.channel(chN).loadUserSample(box, f);   // into THIS slot
                    cacheWaveform(chN);
                }
                rescanSamples(); rebuildSampleMenu();
                if (chN == selectedChannel) { syncPadFromSlots(true); layoutContent(); refreshDetailPanel(); }
            });
        syncPadFromSlots(true); ch.markDspDirty(); layoutContent(); refreshDetailPanel();
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
        boxEngine[box] = (id <= 1) ? -1 : id - 2;   // None / Noise / Analog+FM / FM / Physical / Synth / Wave / Modal
        ch.slots[box].engine = boxEngine[box];
        ch.silenceAllVoices();   // don't let a voice ringing on the OLD engine get re-read as the new one (= noise)
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
    hl(btnModePitch, mode == StepGridComponent::ModePitch);
    hl(btnModeProb,  mode == StepGridComponent::ModeProb);
    hl(btnModeRoll,  mode == StepGridComponent::ModeRoll);
    hl(btnModePan,   mode == StepGridComponent::ModePan);
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

//==============================================================================
void DrumSequencerEditor::selectChannel(int ch)
{
    selectedChannel = ch;
    proc.lastSelectedChannel = ch; // remember across editor open/close
    proc.analyzeChannel.store(ch); // analyse the channel we're inspecting
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
        strips[i].numBtn.setToggleState(i == ch, juce::dontSendNotification);
    updateKnobParamIds();
    syncBoxesFromSrcOn();   // set boxEngine[] from this channel's active sources
    refreshDetailPanel();
    layoutContent();        // re-place the slot boxes for this channel's engines
    updateVisuals();
    content.repaint();
}

void DrumSequencerEditor::selectPattern(int p)
{
    proc.sequencer.setCurrentPattern(p);
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
    juce::String prefix = "p" + juce::String(currentPattern()) + "_ch" + juce::String(selectedChannel) + "_";
    for (int s = 0; s < DrumChannel::NUM_SOURCES; ++s)
    {
        knobSrcAtk[s].paramId  = prefix + "atk" + juce::String(s);
        knobSrcHold[s].paramId = prefix + "hld" + juce::String(s);
        knobSrcDec[s].paramId  = prefix + "dec" + juce::String(s);
    }
    knobPitch.paramId    = prefix + "pitch";
    knobBloom.paramId  = prefix + "bloom";
    knobDrift.paramId  = prefix + "drift";
    knobSpread.paramId = prefix + "spread";
    knobPunch.paramId  = prefix + "punch";
    knobGlue.paramId   = prefix + "glue";
    knobVolume.paramId   = prefix + "volume";
    knobPan.paramId      = prefix + "pan";
    knobCutoff.paramId   = prefix + "filterCutoff";
    knobReso.paramId     = prefix + "filterReso";
    knobEnvAmt.paramId   = prefix + "filterEnvAmt";
    knobDrive.paramId    = prefix + "drive";
    knobReverb.paramId   = prefix + "reverb";
    knobDelay.paramId    = prefix + "delay";
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

void DrumSequencerEditor::refreshDetailPanel()
{
    ignoreKnobCallbacks = true;
    auto& ch = proc.sequencer.channel(selectedChannel);

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
    comboMidiNote.setSelectedId  (juce::jlimit(0, 127, ch.midiNote) + 1, juce::dontSendNotification);
    comboMidiNote.setEnabled(ch.midiOut);   // only relevant when this channel is on MIDI Out
    // Per-slot FX (Drive + Reverb/Delay send) for the selected slot:
    { auto& sl = ch.slots[envTargetSlot()];
      knobDrive.setValue (sl.fxDrive,       juce::dontSendNotification);
      knobReverb.setValue(sl.fxReverbSend,  juce::dontSendNotification);
      knobDelay.setValue (sl.fxDelaySend,   juce::dontSendNotification);
      comboDriveType.setSelectedId(sl.fxDriveType + 1, juce::dontSendNotification); }

    knobReverbRoom.setValue (proc.masterFX().reverbRoom,    juce::dontSendNotification);
    knobReverbDecay.setValue(1.0f - proc.masterFX().reverbDamp, juce::dontSendNotification);
    knobReverbWet.setValue  (proc.masterFX().reverbWet,         juce::dontSendNotification);
    knobReverbPre.setValue  (proc.masterFX().reverbPreDelay,    juce::dontSendNotification);
    knobReverbWidth.setValue(proc.masterFX().reverbWidth,       juce::dontSendNotification);
    knobDelayTime.setValue  (proc.masterFX().delayTime,         juce::dontSendNotification);
    knobDelayFB.setValue    (proc.masterFX().delayFeedback,     juce::dontSendNotification);
    swDelaySync.setToggleState(proc.masterFX().delaySync,       juce::dontSendNotification);
    swDelayPingPong.setToggleState(proc.masterFX().delayPingPong, juce::dontSendNotification);
    knobDelayTime.updateText();
    knobMasterVol.setValue  (proc.masterFX().volume,            juce::dontSendNotification);
    knobMasterPan.setValue  (proc.masterFX().pan,               juce::dontSendNotification);
    knobMasterLimit.setValue(proc.masterFX().limit,             juce::dontSendNotification);
    knobMasterGlue.setValue (proc.masterFX().glue,              juce::dontSendNotification);
    swMasterMono.setToggleState(proc.masterFX().mono,           juce::dontSendNotification);

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
    knobBloom.setValue  (ch.bloom,  juce::dontSendNotification);
    knobDrift.setValue  (ch.drift,  juce::dontSendNotification);
    knobSpread.setValue (ch.spread, juce::dontSendNotification);
    knobPunch.setValue  (ch.punch,  juce::dontSendNotification);
    knobGlue.setValue   (ch.glue,   juce::dontSendNotification);
    knobSpeed.setValue(ch.pitch, juce::dontSendNotification);  // CHANNEL Pitch (st) = transpose whole channel
    for (int b = 0; b < DrumChannel::NUM_SLOTS; ++b)   // each slot's own trim/reverse/region
    {
        const auto& sl = ch.slots[b];
        swSampleReverse[b].setToggleState(sl.smpReverse, juce::dontSendNotification);
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
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        strips[i].btnMute->setToggleState (proc.sequencer.channel(i).mute,        juce::dontSendNotification);
        strips[i].btnSolo->setToggleState (proc.sequencer.channel(i).solo,        juce::dontSendNotification);
        strips[i].btnPoly.setToggleState  (proc.sequencer.channel(i).allowOverlap, juce::dontSendNotification);
        strips[i].comboSteps.setSelectedId(proc.sequencer.channel(i).numSteps,    juce::dontSendNotification);
        updateStripMixLabel(i);   // per-pattern sound-mix name (+ * if edited)
    }
    refreshRouting();   // recolour strips by MIDI/aux routing
    btnDawSync.setToggleState(proc.sequencer.dawSync,       juce::dontSendNotification);
    sliderSwing.setValue     (proc.sequencer.current().swing, juce::dontSendNotification);

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
    // OFF = push the appear-delay so far out it never shows (effectively disables every hover tooltip).
    tooltipWindow.setMillisecondsBeforeTipAppears(tooltipsOn ? 700 : 0x3FFFFFFF);
    btnTooltips.setColour(juce::TextButton::buttonColourId,  tooltipsOn ? juce::Colour(0xff35c0ff) : juce::Colour(0xff20203a));
    btnTooltips.setColour(juce::TextButton::textColourOffId, tooltipsOn ? juce::Colours::black : juce::Colours::lightgrey);
    btnTooltips.setButtonText(tooltipsOn ? "Tooltips" : "Tooltips: Off");
    btnTooltips.repaint();
}

void DrumSequencerEditor::refreshKeysButton()
{
    const bool on = proc.keysMode.load();
    btnKeys.setColour(juce::TextButton::buttonColourId, on ? juce::Colour(0xff35d07a) : juce::Colour(0xff20203a));
    btnKeys.setColour(juce::TextButton::textColourOffId, on ? juce::Colours::black : juce::Colours::lightgrey);
    btnKeys.setButtonText(on ? "Keys: On" : "Keys: Off");
    btnKeys.repaint();
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

// Point the EQ display at the chosen target: All = the final channel EQ, 1/2/3 = that slot's EQ.
void DrumSequencerEditor::refreshEqTarget()
{
    auto& ch = proc.sequencer.channel(selectedChannel);
    DrumChannel::EqBand* bands = (eqEditTarget <= 0) ? ch.eqBand
                                                     : ch.slots[juce::jlimit(0, DrumChannel::NUM_SLOTS - 1, eqEditTarget - 1)].eqBand;
    freqDisplay.setBands(bands, ch.filterType, ch.filterCutoff, ch.filterReso, proc.spectrumRate());
    proc.analysisSlot.store(eqEditTarget <= 0 ? -1 : eqEditTarget - 1);   // spectrum follows the selected slot
    slotSelEq.sel = eqEditTarget; slotSelEq.repaint();
}

void DrumSequencerEditor::timerCallback()
{
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
        }
        const float dm[2] = { toDisp(proc.masterMeterL.load(std::memory_order_relaxed)),
                              toDisp(proc.masterMeterR.load(std::memory_order_relaxed)) };
        proc.masterMeterL.store(0.0f, std::memory_order_relaxed);   // consume the held peaks
        proc.masterMeterR.store(0.0f, std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
        {
            ballistic(dm[c], mMeterVal[c], mMeterPk[c], mMeterHold[c]);
            masterMeter[c].setLevel(mMeterVal[c], mMeterPk[c]);
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
            btnModeVel.repaint();  btnModePitch.repaint();
            btnModeProb.repaint(); btnModeRoll.repaint(); btnModePan.repaint();
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
        strips[ic].btnInfluence.setToggleState(ns, juce::dontSendNotification);
    }

    stepGrid.update(proc.sequencer, proc.anySolo);
    refreshChannelStrips();
    updateVisuals();

    // MIDI-in monitor: flash green + show the last CC whenever MIDI arrives.
    {
        uint32_t mc = proc.midiInCount.load(std::memory_order_relaxed);
        if (mc != lastMidiInSeen) { lastMidiInSeen = mc; midiFlash = 8; }
        int cn = proc.lastCcNum.load(std::memory_order_relaxed);
        juce::String t = "MIDI: ";
        if (cn >= 0)
            t << "cc" << cn << " v" << proc.lastCcVal.load(std::memory_order_relaxed)
              << " ch" << proc.lastCcChan.load(std::memory_order_relaxed);
        else
            t << (mc > 0 ? "in (notes)" : "waiting");
        lblMidiIn.setText(t, juce::dontSendNotification);
        lblMidiIn.setColour(juce::Label::textColourId,
                            midiFlash > 0 ? juce::Colour(0xff44ff66) : juce::Colour(0xff556688));
        if (midiFlash > 0) --midiFlash;
    }

    // Undo history (per-action): snapshot once a change has settled AND the mouse
    // button isn't held. So each discrete click (a step toggle, a button) is its
    // own undo step, while a continuous knob / grid / pad DRAG (mouse held the
    // whole time) collapses into a single step, captured when you release.
    if (! applyingUndo)
    {
        juce::int64 h = stateHash();
        const bool mouseHeld = juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown();
        if (undoStack.empty()) { pushUndoSnapshot(); lastUndoHash = h; }   // baseline
        else
        {
            if (h != lastUndoHash) { lastUndoHash = h; undoDirty = true; undoStableTicks = 0; }
            if (undoDirty && ! mouseHeld && ++undoStableTicks >= 3) { pushUndoSnapshot(); undoDirty = false; }
        }
    }

    // Detect "modified since saved" for the * markers (cheap once already dirty).
    {
        auto& sc = proc.sequencer.channel(selectedChannel);
        if (!sc.mixModified && channelSoundHash(sc) != sc.mixHash)
        {
            sc.mixModified = true;
            updateStripMixLabel(selectedChannel);
        }
        if (!presetModified && stateHash() != presetBaselineHash)
        {
            presetModified = true;
            updatePresetLabel();
        }
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
    proc.pushParamsFromFields();   // reflect manual/preset/undo changes onto the host params
    float heads[8]; int nh = proc.sequencer.channel(selectedChannel).activeVoiceTimes(heads, 8);
    envEditor.setPlayheads(heads, nh);
    pitchEditor.setPlayheads(heads, nh);
    // Keep the pitch X-axis = the sound's current length live (so editing the amp envelope -
    // or the sample trim - rescales the pitch time immediately and the playhead stays synced).
    pitchEditor.setLengthSec(pitchEnvLenSec(envTargetSlot()));
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

    // Lift every control whose centre is inside the group box into the panel,
    // re-positioned relative to the group (the panel's transform enlarges them).
    const auto origin = designRect.getTopLeft();
    juce::Array<juce::Component*> toMove;
    for (auto* c : content.getChildren())
    {
        bool skip = false;
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

    g.setColour(juce::Colour(0x18ffffff));
    const int selRow = selectedChannel - firstChannelRow;        // selection highlight at its on-screen row
    if (selRow >= 0 && selRow < nCh)
        g.fillRect(0, GRID_TOP + selRow * ROW_H, gridRightP, ROW_H);

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
    // Full-height columns: AMP ENV+EQ | PITCH ENV | FX. (Slots are half-height; MASTER is special below.)
    boxGroup(hdrAmpEnv, (float) colH); boxGroup(hdrSend, (float) colH);
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
        g.setColour(juce::Colour(0xffffc24a)); g.drawRoundedRectangle(mb.reduced(0.5f), 4.0f, 1.8f);
        g.setColour(juce::Colour(0xff353560));
    }
}

void DrumSequencerEditor::setVisibleChannels(int n)
{
    n = (n <= 8) ? 8 : 16;   // only 8 or 16 now
    visibleChannels      = n;
    contentHeightPx      = contentHeightFor(n, detailShown);
    proc.visibleChannels = n;
    firstChannelRow      = juce::jlimit(0, juce::jmax(0, n - viewRows()), firstChannelRow);
    stepGrid.visibleRows = viewRows();
    stepGrid.firstRow    = firstChannelRow;
    channelBar.setRangeLimits(0.0, (double) n, juce::dontSendNotification);
    channelBar.setCurrentRange((double) firstChannelRow, (double) viewRows(), juce::dontSendNotification);
    if (selectedChannel >= n) selectChannel(n - 1);   // keep the selection within range
    refreshCountButtons();
    setResizeLimits(DESIGN_W / 2, contentHeightPx / 2, DESIGN_W * 2, contentHeightPx * 2);
    const double s = juce::jmax(0.1, (double) getWidth() / (double) DESIGN_W);  // keep the current width-scale
    layoutContent();
    setSize(getWidth(), juce::roundToInt(contentHeightPx * s));   // adjust height -> triggers resized()
    repaint();
}

void DrumSequencerEditor::setNumPatterns(int n)
{
    n = (n <= 16) ? 16 : 32;   // only 16 or 32 now
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
    auto hl = [](juce::TextButton& b, bool on, juce::Colour onCol) {
        b.setColour(juce::TextButton::buttonColourId, on ? onCol : juce::Colour(0xff20203a));
        b.setColour(juce::TextButton::textColourOffId, on ? juce::Colours::black : juce::Colours::lightgrey);
        b.repaint();
    };
    const juce::Colour yellow(0xffe8bf4d), blue(0xff35c0ff);
    hl(btnCh8,  visibleChannels == 8,  yellow);  hl(btnCh16, visibleChannels == 16, yellow);   // channels = yellow
    hl(btnPat16, visiblePatterns == 16, blue);   hl(btnPat32, visiblePatterns == 32, blue);    // patterns = blue
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
    const int fr = juce::jlimit(0, juce::jmax(0, visibleChannels - viewRows()), (int) (newRangeStart + 0.5));
    if (fr == firstChannelRow) return;
    firstChannelRow   = fr;
    stepGrid.firstRow = fr;
    layoutContent();   // reposition the strips into the new window
    content.repaint();
}

void DrumSequencerEditor::layoutContent()
{
    const int W = DESIGN_W;
    const int gridLeft = STRIP_W + 4;
    const int gridW = W - gridLeft - 12;            // step grid now spans the full width
    // (Master FX/Output used to sit to the right of the grid; they now live in the
    //  detail panel's first row, so the grid reclaims that space - longer steps.)

    // Top toolbar (logo + version are painted in paintContent; leave room for them)
    titleLabel.setBounds  (0, 0, 0, 0); // logo painted directly, not a label
    verLink.setBounds     (166, 11, 48, 18);   // clickable version, just right of the logo wordmark
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
    btnClearMidi.setBounds(1018, 7, 76, 26);
    btnAudition.setBounds (1098, 7, 70, 26);      // "Auto Test"
    comboVisChannels.setBounds(0, 0, 0, 0); comboNumPat.setBounds(0, 0, 0, 0);   // replaced by the toggle buttons
    // Where the Channels/Patterns count boxes used to be: Tooltips toggle + the (global) Follow toggle.
    btnTooltips.setBounds(1172, 7, 66, 26);
    btnFollow.setBounds  (1242, 7, 66, 26);   // moved up from the pattern row (it's a GLOBAL setting)
    btnKeys.setBounds     (W - 190, 7, 62, 26);   // right-anchored, just left of Drag MIDI
    dragMidi.setBounds    (W - 108, 7, 100, 26);  // slightly smaller per request

    // Pattern row: a window of the pattern buttons (16 visible; 24/32 scroll via patternBar).
    lblPatterns.setBounds(6, PAT_Y + 8, 60, 18);
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
            patternBar.setBounds(px0, PAT_Y + PAT_H, shown * step - pg, 5);   // thin bar just below the patterns
            patternBar.setRangeLimits(0.0, (double) visiblePatterns, juce::dontSendNotification);
            patternBar.setCurrentRange((double) firstPatternCol, (double) shown, juce::dontSendNotification);
        }
    }
    patModeBtn.setBounds(664, PAT_Y + 8, 160, 26);   // nudged left; wide enough to show "Chain P2(4)>P3(2)"
    lblLoopCount.setBounds(0, 0, 0, 0);              // loop-count meter REMOVED (loops are picked in the play-mode popup now)
    sliderPatN.setBounds(0, 0, 0, 0);
    // Channel-count (8/16) + pattern-count (16/32) toggles, right next to the loop dropdown (Follow moved to the top bar).
    lblChannels.setJustificationType(juce::Justification::centredRight);
    lblNumPat.setJustificationType(juce::Justification::centredRight);
    lblChannels.setBounds(858, PAT_Y + 8, 20, 24);   btnCh8.setBounds (880, PAT_Y + 10, 25, 21); btnCh16.setBounds(905, PAT_Y + 10, 25, 21);
    lblNumPat.setBounds  (930, PAT_Y + 8, 22, 24);   btnPat16.setBounds(954, PAT_Y + 10, 25, 21); btnPat32.setBounds(979, PAT_Y + 10, 25, 21);
    lblSwing.setBounds   (1018, PAT_Y + 8, 46, 22);  // swing is per-pattern -> pattern row
    sliderSwing.setBounds(1064, PAT_Y + 8, 86, 26);
    // Step edit-mode radio buttons at the right end of the pattern row.
    // Edit-mode group: evenly spaced (8px gaps) so it spans flush to the right edge - Clear ends ~1504 (no weird gap).
    lblEditMode.setBounds (1152, PAT_Y + 8, 42, 24);   // wider so "Edit:" fits in Windows' wider font
    btnModeVel.setBounds  (1198, PAT_Y + 8, 54, 24);   // wider for "Vel/Len"
    btnModePitch.setBounds(1260, PAT_Y + 8, 38, 24);
    btnModeProb.setBounds (1306, PAT_Y + 8, 38, 24);
    btnModeRoll.setBounds (1352, PAT_Y + 8, 38, 24);
    btnModePan.setBounds  (1398, PAT_Y + 8, 38, 24);
    btnClearPat.setBounds (1444, PAT_Y + 8, 60, 24);   // Clear - flush near the right edge

    // Channel strips:  [#] [sound ▸ sub-menu] [M] [S] [Ø] [steps]
    // Only the channels in the scroll window [firstChannelRow, +viewRows) are shown, mapped to on-screen
    // rows. The rest are hidden (the engine still runs them). When scrolling is active a scrollbar sits at
    // the far left, so the strips shift right by `sbPad` to make room for it.
    juce::ignoreUnused(W);
    const int vr     = viewRows();
    firstChannelRow  = juce::jlimit(0, juce::jmax(0, visibleChannels - vr), firstChannelRow);  // keep valid as the view grows/shrinks
    const bool canScroll = visibleChannels > vr;
    const int sbPad  = canScroll ? 16 : 0;
    for (int i = 0; i < Sequencer::NUM_CHANNELS; ++i)
    {
        const int rrow = i - firstChannelRow;                 // on-screen row of this channel
        const bool vis = rrow >= 0 && rrow < vr;
        auto& st = strips[i];
        st.numBtn.setVisible(vis);  st.comboSound.setVisible(vis);  st.btnTest.setVisible(vis);
        st.btnMute->setVisible(vis); st.btnSolo->setVisible(vis);   st.btnPoly.setVisible(vis);
        st.btnInfluence.setVisible(vis); st.comboSteps.setVisible(vis); stripMeter[i].setVisible(vis);
        if (! vis) continue;
        int y = GRID_TOP + rrow * ROW_H;
        st.numBtn.setBounds      (sbPad + 4,   y + 8, 20, 24);
        st.comboSound.setBounds  (sbPad + 26,  y + 7, 144, 26); // Sound mixes
        st.btnTest.setBounds     (sbPad + 174, y + 8, 42, 24);
        st.btnMute->setBounds    (sbPad + 220, y + 8, 23, 24);
        st.btnSolo->setBounds    (sbPad + 245, y + 8, 23, 24);
        st.btnPoly.setBounds     (sbPad + 270, y + 8, 26, 24);
        st.btnInfluence.setBounds(sbPad + 299, y + 8, 22, 24);
        st.comboSteps.setBounds  (sbPad + 325, y + 7, 81 - sbPad, 26);
        stripMeter[i].setBounds  (sbPad + 27,  y + ROW_H - 5, 382 - sbPad, 4);  // thin level bar under the row
    }
    channelBar.setVisible(canScroll);
    if (canScroll) {
        channelBar.setBounds(2, GRID_TOP, 12, vr * ROW_H);
        channelBar.setRangeLimits(0.0, (double) visibleChannels, juce::dontSendNotification);
        channelBar.setCurrentRange((double) firstChannelRow, (double) vr, juce::dontSendNotification);
    }

    // Step grid
    stepGrid.rowH = ROW_H;
    stepGrid.visibleRows = vr;
    stepGrid.firstRow    = firstChannelRow;
    stepGrid.setBounds(gridLeft, GRID_TOP, gridW, vr * ROW_H);

    // ---- Bigger knob helpers (readable) -------------------------------------
    const int kg = 4, gg = 18, boxH = 16, hdrH = 15;
    const int knobS = 56, comboW = 92;
    const int rightEdge = W - 12;
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
    // Even-spread group start positions across [startX, endX].
    auto spread = [&](int startX, int endX, const std::vector<int>& widths) {
        int total = 0; for (int w : widths) total += w;
        int n = (int) widths.size();
        int gap = n > 1 ? juce::jmax(gg, (endX - startX - total) / (n - 1)) : 0;
        std::vector<int> xs; int xx = startX;
        for (int w : widths) { xs.push_back(xx); xx += w + gap; }
        return xs;
    };

    // =====================================================================
    // DETAIL PANEL
    //   Row 1 : sound-select+pad | SAMPLE | ANALOG | NOISE | FM | MASTER FX/OUT
    //   Row 2 : Channel EQ (+spectrum) | Channel FX | Channel Filter | Channel
    //   Every source group has its own per-source AHD envelope knobs.
    // =====================================================================
    const int detailY = GRID_TOP + viewRows() * ROW_H + 24;   // detail panel sits below the viewport (<= 8 rows)
    // Title-strip buttons: raised a couple px (clear of the box outlines below) + wide enough for the full UPPERCASE
    // text (no "..." truncation).
    btnToggleDetail.setBounds(DESIGN_W - 160, detailY - 2, 150, 18);   // collapse/expand (always visible)
    lblSelected.setVisible(detailShown); btnSaveMix.setVisible(detailShown);
    lblSelected.setBounds(12, detailY - 2, 200, 18);
    btnSaveMix.setBounds(214, detailY - 2, 172, 18);
    // NOTE: we DON'T early-return when collapsed. Letting the detail layout run repositions every editor component
    // relative to the (now much lower) detailY, so they land below the short collapsed window and get CLIPPED -
    // which truly hides them. (Early-returning left them at stale positions overlapping the expanded 12/16-row grid.)

    const int KS = 46;                       // detail-row knob size
    auto kAt = [&](LearnableKnob& k, juce::Label& l, int kx, int ky) {
        k.setBounds(kx, ky, KS, KS + boxH);
        l.setBounds(kx - 7, ky + KS + boxH, KS + 14, 13);
    };
    auto cAt = [&](juce::ComboBox& c, juce::Label& l, int kx, int ky) {
        c.setBounds(kx, ky + (KS + boxH - 24) / 2, comboW, 24);
        l.setBounds(kx, ky + KS + boxH, comboW, 13);
    };
    auto kw = [&](int n) { return n * KS + (n - 1) * kg; };
    const int step = KS + kg;
    auto cstart = [&](int gx, int gw, int m) { return gx + (gw - kw(m)) / 2; };

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
    const int slotsColW = 376, ampEqW = 330, pitchW = 250, fxColW = 200, masterW = 290;
    const int cxSlots  = 12;
    const int cxAmp    = cxSlots + slotsColW + gp;
    const int cxPitch  = cxAmp   + ampEqW   + gp;
    const int cxFx     = cxPitch + pitchW   + gp;
    const int cxMaster = cxFx    + fxColW   + gp;        // right edge (== rightEdge)
    // Slots: two half-height boxes stacked on the LEFT edge; a vertical blend fader to their right.
    const int slotBoxW = slotsColW - 50;                // leave a wide column for the blend fader + % read-outs
    const int slotVGap = 6;
    const int slotH    = (colH - slotVGap) / 2;
    const int slotW    = slotBoxW;                      // SlotEditor width
    const int sbx[DrumChannel::NUM_SLOTS] = { cxSlots, cxSlots };
    const int sby[DrumChannel::NUM_SLOTS] = { colTop, colTop + slotH + slotVGap };
    {
        const int KS1 = 35;
        auto k1 = [&](LearnableKnob& k, juce::Label& l, int kx, int ky) {
            k.setVisible(true); l.setVisible(true);
            k.setBounds(kx, ky, KS1, KS1 + boxH);
            l.setBounds(kx - 7, ky + KS1 + boxH, KS1 + 14, 12);
        };
        const int KSB = 46;                            // bigger knobs for the (now full-height) MASTER box
        auto kB = [&](LearnableKnob& k, juce::Label& l, int kx, int ky) {
            k.setVisible(true); l.setVisible(true);
            k.setBounds(kx, ky, KSB, KSB + boxH);
            l.setBounds(kx - 7, ky + KSB + boxH, KSB + 14, 12);
        };

        // ===== MASTER box (RIGHT edge, distinct amber, full height): master Vol fader + Limit + Mono + meters +
        //       the SHARED reverb/delay flavour. Spread to fill the tall box (no empty space below). =====
        int sx = cxMaster;
        hdrSounds.setBounds(sx, colTop, masterW, hdrH);   // "MASTER" (text set in setup)
        hdrBlend2.setBounds(0, 0, 0, 0);
        for (int i = 0; i < 5; ++i) { srcSwitch[i].setBounds(0, 0, 0, 0); lblSrc[i].setBounds(0, 0, 0, 0); }
        btnPadLayout.setBounds(0, 0, 0, 0); lblPadHint.setBounds(0, 0, 0, 0);
        lblShapeSlot.setBounds(0, 0, 0, 0); lblPitchSlot.setBounds(0, 0, 0, 0);
        for (auto* k : { &knobBloom, &knobDrift, &knobSpread, &knobPunch, &knobGlue }) k->setVisible(false);
        for (auto* l : { &lblBloom, &lblDrift, &lblSpread, &lblPunch, &lblGlue })       l->setVisible(false);
        soundPad.setBounds(0, 0, 0, 0);
        hdrMasterOut.setBounds(0, 0, 0, 0); hdrMasterOut.setVisible(false);   // box header is "MASTER" now
        // Master OUT: Volume fader + tall meters, then Limit + Mono.
        knobMasterVol.setBounds(sx + 14, colTop + 30, masterW - 28, 20); lblMasterVol.setBounds(0, 0, 0, 0);
        masterMeter[0].setBounds(0, 0, 0, 0); masterMeter[1].setBounds(0, 0, 0, 0);   // L/R live in the logo meter now
        kB(knobMasterGlue,  lblMasterGlue,  sx + 16, colTop + 60);                 // signal flow is Glue -> Limiter, so Glue is on the LEFT
        kB(knobMasterLimit, lblMasterLimit, sx + 66, colTop + 60);
        lblMasterMono.setBounds(sx + 124, colTop + 66, 64, 11); swMasterMono.setBounds(sx + 142, colTop + 82, 34, 16);
        // Shared REVERB flavour (Size/Decay/Wet/Pre/Width) + DELAY flavour (Time/FB + Sync/Ping), big knobs.
        { const int rstep = 55, rx = sx + 12;
          hdrReverb.setVisible(true);  hdrReverb.setText("REVERB", juce::dontSendNotification);
          hdrReverb.setBounds(sx + 8, colTop + 122, masterW - 16, hdrH);
          kB(knobReverbRoom,  lblRevRoom,  rx,             colTop + 142);
          kB(knobReverbDecay, lblRevDecay, rx + rstep,     colTop + 142);
          kB(knobReverbWet,   lblRevWet,   rx + 2 * rstep, colTop + 142);
          kB(knobReverbPre,   lblRevPre,   rx + 3 * rstep, colTop + 142);
          kB(knobReverbWidth, lblRevWidth, rx + 4 * rstep, colTop + 142);
          hdrDelayG.setVisible(true);  hdrDelayG.setText("DELAY", juce::dontSendNotification);
          hdrDelayG.setBounds(sx + 8, colTop + 228, masterW - 16, hdrH);
          kB(knobDelayTime,   lblDelTime,  rx,             colTop + 248);
          kB(knobDelayFB,     lblDelFB,    rx + rstep,     colTop + 248);
          lblDelaySync.setBounds(rx + 2 * rstep, colTop + 256, 48, 11);   swDelaySync.setBounds(rx + 2 * rstep + 8, colTop + 270, 34, 16);
          lblDelayPingPong.setBounds(rx + 3 * rstep, colTop + 256, 48, 11); swDelayPingPong.setBounds(rx + 3 * rstep + 8, colTop + 270, 34, 16); }

        // -- AMP ENVELOPE + EQ column. The amp-env graph top is GRAPH_Y so it aligns with the pitch-env graph. --
        const int GRAPH_Y = colTop + 38, GRAPH_H = 118;
        { int ax = cxAmp;
          hdrAmpEnv.setBounds (ax, colTop, ampEqW, hdrH);                    // box header / "AMP ENVELOPE"
          slotSelAmp.setBounds(ax + 8, colTop + 18, ampEqW - 16, 16);
          envEditor.setBounds (ax + 8, GRAPH_Y, ampEqW - 16, GRAPH_H);
          hdrEqBox.setBounds  (ax + 4, GRAPH_Y + GRAPH_H + 10, ampEqW - 8, hdrH);   // "EQ" sub-title
          slotSelEq.setBounds (ax + 8, GRAPH_Y + GRAPH_H + 30, ampEqW - 16, 16);
          freqDisplay.setBounds(ax + 8, GRAPH_Y + GRAPH_H + 50, ampEqW - 16, 124); }

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
        hideK({ &knobPitch, &knobPEnvAmt, &knobPEnvTime, &knobSmpPOff,   // knobSpeed = channel Pitch (always shown)
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
            if (boxEngine[b] < 0) { slotEd[b].setVisible(false); continue; }
            slotEd[b].setVisible(true);
            slotEd[b].setEngine(boxEngine[b]);
            int knobTop = sby[b] + 22, knobX = sbx[b], knobW = slotW;
            if (boxEngine[b] == DrumChannel::SrcSample)
            {
                waveform[b].setVisible(true);
                waveform[b].setBounds(sbx[b] + 6, sby[b] + 20, slotW - 12, 52);   // TALLER waveform (toggles moved out)
                lblSampleLen[b].setVisible(false);                               // length is a watermark in the waveform now
                knobTop = sby[b] + 78;
                // Trim + Reverse toggles in a small column to the LEFT of the knobs (stacked, centred together).
                const int tcW = 50;
                const int tcx = sbx[b] + 6, ty = knobTop + 6;
                lblUseRegion[b].setVisible(true); swUseRegion[b].setVisible(true);
                lblUseRegion[b].setJustificationType(juce::Justification::centred);
                lblUseRegion[b].setBounds(tcx, ty, tcW, 12);   swUseRegion[b].setBounds(tcx + (tcW - 28) / 2, ty + 13, 28, 16);
                lblSampleReverse[b].setVisible(true); swSampleReverse[b].setVisible(true);
                lblSampleReverse[b].setJustificationType(juce::Justification::centred);
                lblSampleReverse[b].setBounds(tcx, ty + 34, tcW, 12); swSampleReverse[b].setBounds(tcx + (tcW - 28) / 2, ty + 47, 28, 16);
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
        comboOutput.setBounds(0, 0, 0, 0); comboMidiNote.setBounds(0, 0, 0, 0);
        lblMidiNote.setBounds(0, 0, 0, 0); lblOutput.setBounds(0, 0, 0, 0);
        knobMasterPan.setBounds(0, 0, 0, 0); lblMasterPan.setBounds(0, 0, 0, 0);

        // ===== FX column (cxFx, per-sound only): 1/2 selector, then a 2x2 grid -
        //       top row = Drive type + Drive, bottom row = Reverb + Delay (sends). =====
        hdrSend.setBounds(cxFx, colTop, fxColW, hdrH);                            // "FX" header
        slotSelFx.setBounds(cxFx + 6, colTop + 18, fxColW - 12, 16);              // aligned with the pitch column's 1/2 selector (its left neighbour)
        // Channel PITCH (transpose) fader - relocated here from the pitch column's bottom so the voice visual can
        // match the EQ height. Sits in the FX box's free bottom space.
        lblSpeed.setVisible(true); lblSpeed.setJustificationType(juce::Justification::centredLeft);
        lblSpeed.setBounds(cxFx + 6,  colTop + 296, 38, 16);
        knobSpeed.setBounds(cxFx + 44, colTop + 296, fxColW - 50, 16);
        const int lblH = 12;
        const int colL = cxFx + 6, colR = cxFx + fxColW / 2 + 2;                  // two cell columns
        const int cellW = fxColW / 2 - 8;
        const int KS = 58, kxL = colL + (cellW - KS) / 2, kxR = colR + (cellW - KS) / 2;
        const int row1 = colTop + 58, row2 = colTop + 184;
        auto kc = [&](LearnableKnob& kn, juce::Label& l, int kx, int y, int cellX) {
            kn.setVisible(true); l.setVisible(true);
            kn.setBounds(kx, y, KS, KS + 13); l.setBounds(cellX, y + KS + 13, cellW, lblH); };
        // Top row: Drive type (combo) | Drive (knob).
        comboDriveType.setBounds(colL + (cellW - juce::jmin(cellW, 78)) / 2, row1 + 24, juce::jmin(cellW, 78), 26);
        lblDrvType.setBounds(colL, row1 + KS + 13, cellW, lblH);
        kc(knobDrive, lblDrive, kxR, row1, colR);
        // Bottom row: Reverb send | Delay send.
        kc(knobReverb, lblRev, kxL, row2, colL);
        kc(knobDelay,  lblDel, kxR, row2, colR);
        // (The reverb/delay character + Sync/Ping live in the MASTER box now.)
    }

    hdrDrive.setBounds(0, 0, 0, 0);
    lblSampleSel.setBounds(0, 0, 0, 0);

    // "Zoom" button beside every group title (row 1 + row 2), at the box top-left.
    {
        const juce::Component* zh[NUM_ZOOM] = {
            &hdrSounds, &hdrSamplerG, &hdrOscG, &hdrNoiseG, &hdrFmG, &hdrPhysG,
            &hdrEq, &hdrSend, &hdrFilter, &hdrChan, &hdrMasterFX, &hdrMasterOut,
            &hdrAmpEnv, &hdrPitch };
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

    juce::ignoreUnused(group, knob, combo, groupW, curHy, curKy, x);
}

//==============================================================================
juce::AudioProcessorEditor* DrumSequencerProcessor::createEditor()
{
    return new DrumSequencerEditor(*this);
}
